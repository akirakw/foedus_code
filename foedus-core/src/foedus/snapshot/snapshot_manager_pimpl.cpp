/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include "foedus/snapshot/snapshot_manager_pimpl.hpp"

#include <glog/logging.h>

#include <chrono>
#include <map>
#include <string>

#include "foedus/engine.hpp"
#include "foedus/engine_options.hpp"
#include "foedus/error_stack_batch.hpp"
#include "foedus/assorted/atomic_fences.hpp"
#include "foedus/debugging/stop_watch.hpp"
#include "foedus/fs/filesystem.hpp"
#include "foedus/fs/path.hpp"
#include "foedus/log/log_manager.hpp"
#include "foedus/memory/engine_memory.hpp"
#include "foedus/memory/numa_node_memory.hpp"
#include "foedus/memory/page_pool.hpp"
#include "foedus/savepoint/savepoint_manager.hpp"
#include "foedus/snapshot/log_gleaner_impl.hpp"
#include "foedus/snapshot/log_mapper_impl.hpp"
#include "foedus/snapshot/log_reducer_impl.hpp"
#include "foedus/snapshot/log_reducer_ref.hpp"
#include "foedus/snapshot/snapshot_metadata.hpp"
#include "foedus/snapshot/snapshot_options.hpp"
#include "foedus/soc/soc_manager.hpp"
#include "foedus/storage/composer.hpp"
#include "foedus/storage/metadata.hpp"
#include "foedus/storage/storage_manager.hpp"
#include "foedus/thread/numa_thread_scope.hpp"
#include "foedus/xct/xct_manager.hpp"

namespace foedus {
namespace snapshot {
const SnapshotOptions& SnapshotManagerPimpl::get_option() const {
  return engine_->get_options().snapshot_;
}

ErrorStack SnapshotManagerPimpl::initialize_once() {
  LOG(INFO) << "Initializing SnapshotManager..";
  if (!engine_->get_log_manager()->is_initialized()) {
    return ERROR_STACK(kErrorCodeDepedentModuleUnavailableInit);
  }
  soc::SharedMemoryRepo* repo = engine_->get_soc_manager()->get_shared_memory_repo();
  control_block_ = repo->get_global_memory_anchors()->snapshot_manager_memory_;
  if (engine_->is_master()) {
    control_block_->initialize();
    // get snapshot status from savepoint
    control_block_->snapshot_epoch_
      = engine_->get_savepoint_manager()->get_latest_snapshot_epoch().value();
    control_block_->previous_snapshot_id_
      = engine_->get_savepoint_manager()->get_latest_snapshot_id();
    LOG(INFO) << "Latest snapshot: id=" << control_block_->previous_snapshot_id_ << ", epoch="
      << control_block_->snapshot_epoch_;
    control_block_->immediate_snapshot_requested_.store(false);

    const EngineOptions& options = engine_->get_options();
    uint32_t reducer_count = options.thread_.group_count_;
    uint32_t mapper_count = reducer_count * options.log_.loggers_per_node_;
    control_block_->gleaner_.reducers_count_ = reducer_count;
    control_block_->gleaner_.mappers_count_ = mapper_count;
    control_block_->gleaner_.all_count_ = reducer_count + mapper_count;
  }

  // in child engines, we instantiate local mappers/reducer objects (but not the threads yet)
  previous_snapshot_time_ = std::chrono::system_clock::now();
  stop_requested_ = false;
  if (!engine_->is_master()) {
    local_reducer_ = new LogReducer(engine_);
    CHECK_ERROR(local_reducer_->initialize());
    for (uint16_t i = 0; i < engine_->get_options().log_.loggers_per_node_; ++i) {
      local_mappers_.push_back(new LogMapper(engine_, i));
      CHECK_ERROR(local_mappers_[i]->initialize());
    }
  }

  // Launch the daemon thread at last
  if (engine_->is_master()) {
    snapshot_thread_ = std::move(std::thread(&SnapshotManagerPimpl::handle_snapshot, this));
  } else {
    snapshot_thread_ = std::move(std::thread(&SnapshotManagerPimpl::handle_snapshot_child, this));
  }
  return kRetOk;
}

ErrorStack SnapshotManagerPimpl::uninitialize_once() {
  LOG(INFO) << "Uninitializing SnapshotManager..";
  ErrorStackBatch batch;
  if (!engine_->get_log_manager()->is_initialized()) {
    batch.emprace_back(ERROR_STACK(kErrorCodeDepedentModuleUnavailableUninit));
  }
  if (snapshot_thread_.joinable()) {
    stop_requested_ = true;
    if (engine_->is_master()) {
      control_block_->gleaner_.cancelled_ = true;
      wakeup();
    } else {
      control_block_->wakeup_snapshot_children();
    }
    snapshot_thread_.join();
  }
  if (engine_->is_master()) {
    control_block_->uninitialize();
    ASSERT_ND(local_reducer_ == nullptr);
    ASSERT_ND(local_mappers_.size() == 0);
  } else {
    if (local_reducer_) {
      batch.emprace_back(local_reducer_->uninitialize());
      delete local_reducer_;
      local_reducer_ = nullptr;
    }
    for (LogMapper* mapper : local_mappers_) {
      batch.emprace_back(mapper->uninitialize());
      delete mapper;
    }
    local_mappers_.clear();
  }

  return SUMMARIZE_ERROR_BATCH(batch);
}

void SnapshotManagerPimpl::sleep_a_while() {
  soc::SharedMutexScope scope(control_block_->snapshot_wakeup_.get_mutex());
  if (!is_stop_requested()) {
    control_block_->snapshot_wakeup_.timedwait(&scope, 100000000ULL);
  }
}
void SnapshotManagerPimpl::wakeup() {
  soc::SharedMutexScope scope(control_block_->snapshot_wakeup_.get_mutex());
  control_block_->snapshot_wakeup_.signal(&scope);
}

void SnapshotManagerPimpl::handle_snapshot() {
  LOG(INFO) << "Snapshot daemon started";
  // The actual snapshotting can't start until all other modules are initialized.
  SPINLOCK_WHILE(!is_stop_requested() && !engine_->is_initialized()) {
    assorted::memory_fence_acquire();
  }

  LOG(INFO) << "Snapshot daemon now starts taking snapshot";
  while (!is_stop_requested()) {
    sleep_a_while();
    if (is_stop_requested()) {
      break;
    }
    // should we start snapshotting? or keep sleeping?
    bool triggered = false;
    std::chrono::system_clock::time_point until = previous_snapshot_time_ +
      std::chrono::milliseconds(get_option().snapshot_interval_milliseconds_);
    Epoch durable_epoch = engine_->get_log_manager()->get_durable_global_epoch();
    Epoch previous_epoch = get_snapshot_epoch();
    if (previous_epoch.is_valid() && previous_epoch == durable_epoch) {
      LOG(INFO) << "Current snapshot is already latest. durable_epoch=" << durable_epoch;
    } else if (control_block_->immediate_snapshot_requested_) {
      // if someone requested immediate snapshot, do it.
      triggered = true;
      control_block_->immediate_snapshot_requested_.store(false);
      LOG(INFO) << "Immediate snapshot request detected. snapshotting..";
    } else if (std::chrono::system_clock::now() >= until) {
      triggered = true;
      LOG(INFO) << "Snapshot interval has elapsed. snapshotting..";
    } else {
      // TODO(Hideaki): check free pages in page pool and compare with configuration.
    }

    if (triggered) {
      Snapshot new_snapshot;
      // TODO(Hideaki): graceful error handling
      COERCE_ERROR(handle_snapshot_triggered(&new_snapshot));
    } else {
      VLOG(1) << "Snapshotting not triggered. going to sleep again";
    }
  }

  LOG(INFO) << "Snapshot daemon ended. ";
}

void SnapshotManagerPimpl::handle_snapshot_child() {
  LOG(INFO) << "Child snapshot daemon-" << engine_->get_soc_id() << " started";
  thread::NumaThreadScope scope(engine_->get_soc_id());
  SnapshotId previous_id = control_block_->gleaner_.cur_snapshot_.id_;
  while (!is_stop_requested()) {
    {
      soc::SharedMutexScope scope(control_block_->snapshot_children_wakeup_.get_mutex());
      if (!is_stop_requested() && !is_gleaning()) {
        control_block_->snapshot_children_wakeup_.timedwait(&scope, 100000000ULL);
      }
    }
    if (is_stop_requested()) {
      break;
    } else if (!is_gleaning() || previous_id == control_block_->gleaner_.cur_snapshot_.id_) {
      continue;
    }
    SnapshotId current_id = control_block_->gleaner_.cur_snapshot_.id_;
    LOG(INFO) << "Child snapshot daemon-" << engine_->get_soc_id() << " received a request"
      << " for snapshot-" << current_id;
    local_reducer_->launch_thread();
    for (LogMapper* mapper : local_mappers_) {
      mapper->launch_thread();
    }
    LOG(INFO) << "Child snapshot daemon-" << engine_->get_soc_id() << " launched mappers/reducer"
      " for snapshot-" << current_id;
    for (LogMapper* mapper : local_mappers_) {
      mapper->join_thread();
    }
    local_reducer_->join_thread();
    LOG(INFO) << "Child snapshot daemon-" << engine_->get_soc_id() << " joined mappers/reducer"
      " for snapshot-" << current_id;
    previous_id = current_id;
  }

  LOG(INFO) << "Child snapshot daemon-" << engine_->get_soc_id() << " ended";
}


void SnapshotManagerPimpl::trigger_snapshot_immediate(bool wait_completion) {
  LOG(INFO) << "Requesting to immediately take a snapshot...";
  Epoch before = get_snapshot_epoch();
  Epoch durable_epoch = engine_->get_log_manager()->get_durable_global_epoch();
  if (before.is_valid() && before == durable_epoch) {
    LOG(INFO) << "Current snapshot is already latest. durable_epoch=" << durable_epoch;
    return;
  }

  while (before == get_snapshot_epoch() && !is_stop_requested()) {
    control_block_->immediate_snapshot_requested_.store(true);
    wakeup();
    if (wait_completion) {
      LOG(INFO) << "Waiting for the completion of snapshot... before=" << before;
      {
        soc::SharedMutexScope scope(control_block_->snapshot_taken_.get_mutex());
        control_block_->snapshot_taken_.timedwait(&scope, 10000000ULL);
      }
    } else {
      break;
    }
  }
  LOG(INFO) << "Observed the completion of snapshot! after=" << get_snapshot_epoch();
}

ErrorStack SnapshotManagerPimpl::handle_snapshot_triggered(Snapshot *new_snapshot) {
  ASSERT_ND(engine_->is_master());
  Epoch durable_epoch = engine_->get_log_manager()->get_durable_global_epoch();
  Epoch previous_epoch = get_snapshot_epoch();
  LOG(INFO) << "Taking a new snapshot. durable_epoch=" << durable_epoch
    << ". previous_snapshot=" << previous_epoch;
  ASSERT_ND(durable_epoch.is_valid() &&
    (!previous_epoch.is_valid() || durable_epoch > previous_epoch));
  new_snapshot->base_epoch_ = previous_epoch;
  new_snapshot->valid_until_epoch_ = durable_epoch;
  new_snapshot->max_storage_id_ = engine_->get_storage_manager()->get_largest_storage_id();

  // determine the snapshot ID
  SnapshotId snapshot_id;
  if (control_block_->previous_snapshot_id_ == kNullSnapshotId) {
    snapshot_id = 1;
  } else {
    snapshot_id = increment(control_block_->previous_snapshot_id_);
  }
  LOG(INFO) << "Issued ID for this snapshot:" << snapshot_id;
  new_snapshot->id_ = snapshot_id;

  // okay, let's start the snapshotting.
  // The procedures below will take long time, so we keep checking our "is_stop_requested"
  // and stops our child threads when it happens.

  // For each storage that was modified in this snapshotting,
  // this holds the pointer to new root page.
  std::map<storage::StorageId, storage::SnapshotPagePointer> new_root_page_pointers;

  // Log gleaners design partitioning and do scatter-gather to consume the logs.
  // This will create snapshot files at each partition and tell us the new root pages of
  // each storage.
  CHECK_ERROR(glean_logs(*new_snapshot, &new_root_page_pointers));

  // Write out the metadata file.
  CHECK_ERROR(snapshot_metadata(*new_snapshot, new_root_page_pointers));

  // Invokes savepoint module to make sure this snapshot has "happened".
  CHECK_ERROR(snapshot_savepoint(*new_snapshot));

  // install pointers to snapshot pages and drop volatile pages.
  CHECK_ERROR(replace_pointers(*new_snapshot, new_root_page_pointers));

  Epoch new_snapshot_epoch = new_snapshot->valid_until_epoch_;
  ASSERT_ND(new_snapshot_epoch.is_valid() &&
    (!get_snapshot_epoch().is_valid() || new_snapshot_epoch > get_snapshot_epoch()));

  // done. notify waiters if exist
  Epoch::EpochInteger epoch_after = new_snapshot_epoch.value();
  control_block_->previous_snapshot_id_ = snapshot_id;
  previous_snapshot_time_ = std::chrono::system_clock::now();
  {
    soc::SharedMutexScope scope(control_block_->snapshot_taken_.get_mutex());
    control_block_->snapshot_epoch_ = epoch_after;
    control_block_->snapshot_taken_.broadcast(&scope);
  }
  return kRetOk;
}

ErrorStack SnapshotManagerPimpl::glean_logs(
  const Snapshot& new_snapshot,
  std::map<storage::StorageId, storage::SnapshotPagePointer>* new_root_page_pointers) {
  // Log gleaner is an object allocated/deallocated per snapshotting.
  // Gleaner runs on this thread (snapshot_thread_)
  LogGleaner gleaner(engine_, new_snapshot);
  ErrorStack result = gleaner.execute();
  if (result.is_error()) {
    LOG(ERROR) << "Log Gleaner encountered either an error or early termination request";
  }
  // the output is list of pointers to new root pages
  *new_root_page_pointers = gleaner.get_new_root_page_pointers();
  return result;
}

ErrorStack SnapshotManagerPimpl::snapshot_metadata(
  const Snapshot& new_snapshot,
  const std::map<storage::StorageId, storage::SnapshotPagePointer>& new_root_page_pointers) {
  // construct metadata object
  SnapshotMetadata metadata;
  metadata.id_ = new_snapshot.id_;
  metadata.base_epoch_ = new_snapshot.base_epoch_.value();
  metadata.valid_until_epoch_ = new_snapshot.valid_until_epoch_.value();
  metadata.largest_storage_id_ = new_snapshot.max_storage_id_;
  CHECK_ERROR(engine_->get_storage_manager()->clone_all_storage_metadata(&metadata));

  // we modified the root page. install it.
  uint32_t installed_root_pages_count = 0;
  for (storage::StorageId id = 1; id <= metadata.largest_storage_id_; ++id) {
    const auto& it = new_root_page_pointers.find(id);
    if (it != new_root_page_pointers.end()) {
      storage::SnapshotPagePointer new_pointer = it->second;
      storage::Metadata* meta = metadata.get_metadata(id);
      ASSERT_ND(new_pointer != meta->root_snapshot_page_id_);
      meta->root_snapshot_page_id_ = new_pointer;
      ++installed_root_pages_count;
    }
  }
  LOG(INFO) << "Out of " << metadata.largest_storage_id_ << " storages, "
    << installed_root_pages_count << " changed their root pages.";
  ASSERT_ND(installed_root_pages_count == new_root_page_pointers.size());

  // save it to a file
  fs::Path folder(get_option().get_primary_folder_path());
  if (!fs::exists(folder)) {
    if (!fs::create_directories(folder, true)) {
      LOG(ERROR) << "Failed to create directory:" << folder << ". check permission.";
      return ERROR_STACK(kErrorCodeFsMkdirFailed);
    }
  }

  fs::Path file = get_snapshot_metadata_file_path(new_snapshot.id_);
  LOG(INFO) << "New snapshot metadata file fullpath=" << file;

  debugging::StopWatch stop_watch;
  CHECK_ERROR(metadata.save_to_file(file));
  stop_watch.stop();
  LOG(INFO) << "Wrote a snapshot metadata file. size=" << fs::file_size(file) << " bytes"
    << ", elapsed time to write=" << stop_watch.elapsed_ms() << "ms. now fsyncing...";
  stop_watch.start();
  fs::fsync(file, true);
  stop_watch.stop();
  LOG(INFO) << "fsynced the file and the folder! elapsed=" << stop_watch.elapsed_ms() << "ms.";
  return kRetOk;
}

ErrorStack SnapshotManagerPimpl::read_snapshot_metadata(
  SnapshotId snapshot_id,
  SnapshotMetadata* out) {
  fs::Path file = get_snapshot_metadata_file_path(snapshot_id);
  LOG(INFO) << "Reading snapshot metadata file fullpath=" << file;

  debugging::StopWatch stop_watch;
  CHECK_ERROR(out->load_from_file(file));
  stop_watch.stop();
  LOG(INFO) << "Read a snapshot metadata file. size=" << fs::file_size(file) << " bytes"
    << ", elapsed time to read+parse=" << stop_watch.elapsed_ms() << "ms.";

  ASSERT_ND(out->id_ == snapshot_id);
  return kRetOk;
}

ErrorStack SnapshotManagerPimpl::snapshot_savepoint(const Snapshot& new_snapshot) {
  LOG(INFO) << "Taking savepoint to include this new snapshot....";
  CHECK_ERROR(engine_->get_savepoint_manager()->take_savepoint_after_snapshot(
    new_snapshot.id_,
    new_snapshot.valid_until_epoch_));
  ASSERT_ND(engine_->get_savepoint_manager()->get_latest_snapshot_id() == new_snapshot.id_);
  ASSERT_ND(engine_->get_savepoint_manager()->get_latest_snapshot_epoch()
    == new_snapshot.valid_until_epoch_);
  return kRetOk;
}

fs::Path SnapshotManagerPimpl::get_snapshot_metadata_file_path(SnapshotId snapshot_id) const {
  fs::Path folder(get_option().get_primary_folder_path());
  fs::Path file(folder);
  file /= std::string("snapshot_metadata_")
    + std::to_string(snapshot_id) + std::string(".xml");
  return file;
}

ErrorStack SnapshotManagerPimpl::replace_pointers(
  const Snapshot& new_snapshot,
  const std::map<storage::StorageId, storage::SnapshotPagePointer>& new_root_page_pointers) {
  // To speed up, this method should be parallelized at least for per-storage.
  LOG(INFO) << "Installing new snapshot pointers and dropping volatile pointers...";

  // To avoid invoking volatile pool for every dropped page, we cache them in chunks
  memory::AlignedMemory chunks_memory;
  chunks_memory.alloc(
    sizeof(memory::PagePoolOffsetChunk) * engine_->get_soc_count(),
    1U << 12,
    memory::AlignedMemory::kNumaAllocOnnode,
    0);
  memory::PagePoolOffsetChunk* dropped_chunks = reinterpret_cast<memory::PagePoolOffsetChunk*>(
    chunks_memory.get_block());
  for (uint16_t node = 0; node < engine_->get_soc_count(); ++node) {
    dropped_chunks[node].clear();
  }

  // For whatever use.. this is automatically expanded by the replace_pointers() implementation.
  memory::AlignedMemory work_memory;
  work_memory.alloc(1U << 21, 1U << 12, memory::AlignedMemory::kNumaAllocOnnode, 0);

  cache::SnapshotFileSet fileset(engine_);
  CHECK_ERROR(fileset.initialize());

  // initializations done.
  // below, we should release the resources before exiting. So, let's not just use CHECK_ERROR.
  ErrorStack result;
  uint64_t installed_count_total = 0;
  uint64_t dropped_count_total = 0;
  // So far, we pause transaction executions during this step to simplify the algorithm.
  // Without this simplification, not only this thread but also normal transaction executions
  // have to do several complex and expensive checks.
  engine_->get_xct_manager()->pause_accepting_xct();
  // It will take a while for individual worker threads to complete the currently running xcts.
  // Just wait for a while to let that happen.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // almost forever in OLTP xcts.
  LOG(INFO) << "Paused transaction executions to safely drop volatile pages and waited enough"
    << " to let currently running xcts end. Now start replace pointers.";
  debugging::StopWatch stop_watch;
  for (storage::StorageId id = 1; id <= new_snapshot.max_storage_id_; ++id) {
    const auto& it = new_root_page_pointers.find(id);
    if (it != new_root_page_pointers.end()) {
      storage::SnapshotPagePointer new_root_page_pointer = it->second;
      storage::Composer composer(engine_, id);
      uint64_t installed_count = 0;
      uint64_t dropped_count = 0;
      storage::Composer::ReplacePointersArguments args = {
        new_snapshot,
        &fileset,
        new_root_page_pointer,
        &work_memory,
        dropped_chunks,
        &installed_count,
        &dropped_count};
      result = composer.replace_pointers(args);
      if (result.is_error()) {
        LOG(ERROR) << "composer.replace_pointers() failed with storage-" << id << ":" << result;
        break;
      }
      installed_count_total += installed_count;
      dropped_count_total += dropped_count;
    }
  }
  engine_->get_xct_manager()->resume_accepting_xct();

  stop_watch.stop();
  LOG(INFO) << "Installed " << installed_count_total << " new snapshot pointers and "
    << dropped_count_total << " dropped volatile pointers in "
    << stop_watch.elapsed_ms() << "ms.";

  ErrorStack fileset_error = fileset.uninitialize();
  if (fileset_error.is_error()) {
    LOG(WARNING) << "Failed to close snapshot fileset. weird. " << fileset_error;
  }
  for (uint16_t node = 0; node < engine_->get_soc_count(); ++node) {
    memory::PagePoolOffsetChunk* chunk = dropped_chunks + node;
    memory::PagePool* volatile_pool
      = engine_->get_memory_manager()->get_node_memory(node)->get_volatile_pool();
    if (!chunk->empty()) {
      volatile_pool->release(chunk->size(), chunk);
    }
    ASSERT_ND(chunk->empty());
  }
  chunks_memory.release_block();
  return result;
}

}  // namespace snapshot
}  // namespace foedus
