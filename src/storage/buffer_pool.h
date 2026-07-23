#pragma once

#include <list>
#include <unordered_map>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/free_list.h"

// ─────────────────────────────────────────────────────────────────────────────
// buffer_pool.h — In-memory page cache with LRU eviction.
//
// The BufferPool holds BUFFER_POOL_SIZE (64) page frames in RAM. The rest of
// the database only ever touches Page* pointers returned by the pool. The one
// exception is FreeList, which bypasses the pool to avoid a circular dependency
// and to keep page 0 out of the eviction path.
//
// Ownership chain (no cycles):
//   DiskManager  ←  FreeList  ←─┐
//         ↑                     │
//         └──────  BufferPool  ──┘
//
// Construction order: DiskManager → FreeList → BufferPool.
//
// Frame lifecycle:
//   1. new_page() or fetch_page() loads a page into a frame, pin_count = 1.
//   2. Caller reads or writes through the Page* pointer.
//   3. unpin_page(id, dirty) decrements pin_count. dirty=true marks the frame.
//   4. When pin_count == 0, the frame is eviction-eligible (tracked in LRU).
//   5. On eviction: flush if dirty, then overwrite with the newly requested page.
//
// Dirty-on-eviction guarantee:
//   evict_frame() ALWAYS flushes a dirty frame to disk before overwriting it.
//   This is not crash-safety — it is loss prevention under normal buffer pool
//   pressure. A dirty eviction that skips the write permanently loses data
//   without any crash, and the crash test would not catch it. The eviction
//   path must be identical to an explicit flush_page() call in terms of what
//   it writes to disk.
//
// Thread safety: single-threaded for Milestone 1. A coarse std::mutex will be
// added in Milestone 4 alongside the lock manager and concurrent transactions.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

class BufferPool {
public:
    BufferPool(DiskManager& dm, FreeList& free_list);

    // Flush all dirty frames to disk on destruction.
    ~BufferPool();

    // Non-copyable, non-movable.
    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&)                 = delete;
    BufferPool& operator=(BufferPool&&)      = delete;

    // Pin the page into a frame and return a pointer to it.
    //
    // Cache hit: increments pin_count, moves to MRU, returns existing pointer.
    // Cache miss: finds a free frame (evicting the LRU unpinned frame if all
    //   frames are occupied), reads the page from disk, pins it.
    //
    // Returns nullptr if every frame is pinned (pool exhausted).
    Page* fetch_page(page_id_t page_id);

    // Decrement the pin count for page_id.
    // If dirty == true, marks the frame as needing a disk write before eviction.
    // When pin_count reaches 0, the frame becomes eviction-eligible.
    // No-op if the page is not in the pool.
    void unpin_page(page_id_t page_id, bool dirty);

    // Immediately write page_id to disk if it is dirty (does not unpin).
    // Returns false if the page is not in the pool.
    bool flush_page(page_id_t page_id);

    // Write all dirty frames to disk.
    void flush_all();

    // Allocate a new page (from FreeList if non-empty, else extend the file),
    // place it in a frame with pin_count == 1, zero its contents, and set its
    // page_id in the header. out_page_id is set to the new page's ID.
    // Returns nullptr if no frame can be evicted (all frames are pinned).
    Page* new_page(page_id_t& out_page_id);

    // Remove the page from the pool (if present) and add its ID to the FreeList.
    // The frame is evicted without flushing (contents are being discarded).
    // Asserts that pin_count == 0 (deleting a pinned page is a logic error).
    void delete_page(page_id_t page_id);

    // Number of pages currently resident in the pool. For testing.
    size_t pool_size() const { return page_table_.size(); }

private:
    struct Frame {
        Page page;
        int  pin_count = 0;
        bool is_dirty  = false;
        bool in_use    = false;
    };

    static constexpr size_t POOL_SIZE = BUFFER_POOL_SIZE;  // 64

    Frame        frames_[POOL_SIZE];
    DiskManager& dm_;
    FreeList&    free_list_;

    // page_id → frame_id, only contains currently in-use frames.
    std::unordered_map<page_id_t, frame_id_t> page_table_;

    // Frame indices not currently holding any page (fast allocation path).
    std::vector<frame_id_t> free_frames_;

    // LRU ordering of in-use frames.
    // Front = most recently used (MRU). Back = least recently used (LRU).
    // Only contains frames with in_use == true.
    std::list<frame_id_t>                                           lru_list_;
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> lru_map_;

    // Move frame_id to the MRU end of lru_list_. Adds it if not present.
    void touch(frame_id_t frame_id);

    // Return a usable frame:
    //   1. Pop from free_frames_ if available (fast path).
    //   2. Otherwise, scan lru_list_ from the back (LRU) for a frame with
    //      pin_count == 0, call evict_frame() on it, and return it.
    // Returns INVALID_PAGE_ID (cast to frame_id_t) if all frames are pinned.
    frame_id_t find_free_frame();

    // IMPORTANT — dirty-flush before eviction:
    //
    // If the frame is dirty, write it to disk via dm_.write_page() BEFORE
    // overwriting its contents. This is not crash-safety; it is normal-operation
    // data loss prevention. Skipping the write under memory pressure would
    // silently discard dirty data with no crash and no error message. The
    // crash_test.sh scenarios would not detect this because they test crash
    // behaviour, not eviction behaviour. Dirty eviction must be treated
    // identically to an explicit flush_page() call.
    //
    // After flushing (if needed), removes the frame from page_table_ and LRU
    // structures, clears it, and marks it in_use = false so find_free_frame()
    // can reuse it.
    void evict_frame(frame_id_t frame_id);
};

} // namespace keyvdb
