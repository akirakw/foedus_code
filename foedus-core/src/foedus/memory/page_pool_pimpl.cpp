/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#include <foedus/engine.hpp>
#include <foedus/engine_options.hpp>
#include <foedus/error_stack_batch.hpp>
#include <foedus/assorted/assorted_func.hpp>
#include <foedus/memory/page_pool.hpp>
#include <foedus/memory/page_pool_pimpl.hpp>
#include <foedus/memory/memory_options.hpp>
#include <foedus/storage/storage_manager.hpp>
#include <foedus/thread/thread_pool.hpp>
#include <foedus/thread/thread.hpp>
#include <glog/logging.h>
#include <algorithm>
namespace foedus {
namespace memory {
ErrorStack PagePoolPimpl::initialize_once() {
    pool_base_ = nullptr;
    pool_size_= 0;
    free_pool_ = nullptr;
    free_pool_capacity_ = 0;
    free_pool_head_ = 0;
    free_pool_count_ = 0;

    const MemoryOptions &options = engine_->get_options().memory_;
    LOG(INFO) << "Acquiring memory for Page Pool on NUMA node "
        << static_cast<int>(numa_node_)<< "...";
    {
        AlignedMemory::AllocType alloc_type = AlignedMemory::kNumaAllocOnnode;
        uint64_t size = static_cast<uint64_t>(options.page_pool_size_mb_per_node_) << 20;
        ASSERT_ND(size >= (2 << 20));
        uint64_t alignment = storage::kPageSize;
        memory_ = std::move(AlignedMemory(size, alignment, alloc_type, numa_node_));
        pool_base_ = reinterpret_cast<storage::Page*>(memory_.get_block());
        pool_size_ = memory_.get_size() / storage::kPageSize;
    }
    LOG(INFO) << "Acquired memory Page Pool. " << memory_ << ". pages=" << pool_size_;

    ASSERT_ND(memory_.get_size() % storage::kPageSize == 0);
    uint64_t total_pages = memory_.get_size() / storage::kPageSize;
    uint64_t pointers_total_size = total_pages * sizeof(PagePoolOffset);
    pages_for_free_pool_ = assorted::int_div_ceil(pointers_total_size, storage::kPageSize);
    LOG(INFO) << "total_pages=" << total_pages
        << ", pointers_total_size=" << pointers_total_size
        << ", pages_for_free_pool_=" << pages_for_free_pool_;

    LOG(INFO) << "Constructing circular free pool...";
    free_pool_ = reinterpret_cast<PagePoolOffset*>(memory_.get_block());
    free_pool_capacity_ = total_pages - pages_for_free_pool_;
    // all pages after pages_for_free_pool_-th page is in the free pool at first
    for (uint64_t i = 0; i < free_pool_capacity_; ++i) {
        free_pool_[i] = pages_for_free_pool_ + i;
    }
    free_pool_head_ = 0;
    free_pool_count_ = free_pool_capacity_;
    resolver_ = LocalPageResolver(pool_base_, pages_for_free_pool_, pool_size_);
    LOG(INFO) << "Constructed circular free pool.";

    return kRetOk;
}

ErrorStack PagePoolPimpl::uninitialize_once() {
    LOG(INFO) << "Releasing memory of Page Pool.";
    if (free_pool_count_ != free_pool_capacity_) {
        // This is not a memory leak as we anyway releases everything, but it's a smell of bug.
        LOG(WARNING) << "Page Pool has not received back all free pages by its uninitialization!!"
            << " count=" << free_pool_count_ << ", capacity=" << free_pool_capacity_;
    } else {
        LOG(INFO) << "Page Pool has received back all free pages. No suspicious behavior.";
    }
    memory_.release_block();
    free_pool_ = nullptr;
    pool_base_ = nullptr;
    LOG(INFO) << "Released memory. ";
    return kRetOk;
}

ErrorCode PagePoolPimpl::grab(uint32_t desired_grab_count, PagePoolOffsetChunk* chunk) {
    ASSERT_ND(is_initialized());
    ASSERT_ND(chunk->size() + desired_grab_count <= chunk->capacity());
    VLOG(0) << "Grabbing " << desired_grab_count << " pages."
        << " free_pool_count_=" << free_pool_count_;
    std::lock_guard<std::mutex> guard(lock_);
    if (free_pool_count_ == 0) {
        LOG(WARNING) << "No more free pages left in the pool";
        return kErrorCodeMemoryNoFreePages;
    }

    // grab from the head
    uint64_t grab_count = std::min<uint64_t>(desired_grab_count, free_pool_count_);
    PagePoolOffset* head = free_pool_ + free_pool_head_;
    if (free_pool_head_ + grab_count > free_pool_capacity_) {
        // wrap around
        uint64_t wrap_count = free_pool_capacity_ - free_pool_head_;
        chunk->push_back(head, head + wrap_count);
        free_pool_head_ = 0;
        free_pool_count_ -= wrap_count;
        grab_count -= wrap_count;
        head = free_pool_;
    }

    // no wrap around (or no more wrap around)
    ASSERT_ND(free_pool_head_ + grab_count <= free_pool_capacity_);
    chunk->push_back(head, head + grab_count);
    free_pool_head_ += grab_count;
    free_pool_count_ -= grab_count;
    return kErrorCodeOk;
}

void PagePoolPimpl::release(uint32_t desired_release_count, PagePoolOffsetChunk *chunk) {
    ASSERT_ND(is_initialized());
    ASSERT_ND(chunk->size() >= desired_release_count);
    VLOG(0) << "Releasing " << desired_release_count << " pages."
        << " free_pool_count_=" << free_pool_count_;
    std::lock_guard<std::mutex> guard(lock_);
    if (free_pool_count_ + desired_release_count > free_pool_capacity_) {
        // this can't happen unless something is wrong! This is a critical issue from which
        // we can't recover because page pool is inconsistent!
        LOG(ERROR) << "PagePoolPimpl::release() More than full free-pool. inconsistent state!";
        // TODO(Hideaki) Minor: Do a duplicate-check here to identify the problemetic pages.
        COERCE_ERROR(ERROR_STACK(kErrorCodeMemoryDuplicatePage));
    }

    // append to the tail
    uint64_t release_count = std::min<uint64_t>(desired_release_count, chunk->size());
    uint64_t tail = free_pool_head_ + free_pool_count_;
    if (tail >= free_pool_capacity_) {
        tail -= free_pool_capacity_;
    }
    if (tail + release_count > free_pool_capacity_) {
        // wrap around
        uint32_t wrap_count = free_pool_capacity_ - tail;
        chunk->move_to(free_pool_ + tail, wrap_count);
        free_pool_count_ += wrap_count;
        release_count -= wrap_count;
        tail = 0;
    }

    // no wrap around (or no more wrap around)
    ASSERT_ND(tail + release_count <= free_pool_capacity_);
    chunk->move_to(free_pool_ + tail, release_count);
    free_pool_count_ += release_count;
}

}  // namespace memory
}  // namespace foedus
