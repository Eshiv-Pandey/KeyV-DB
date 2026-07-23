#include "storage/buffer_pool.h"

#include <cassert>
#include <stdexcept>

namespace keyvdb {

BufferPool::BufferPool(DiskManager& dm, FreeList& free_list)
    : dm_(dm), free_list_(free_list)
{
    // All frames start free.
    free_frames_.reserve(POOL_SIZE);
    for (frame_id_t i = 0; i < static_cast<frame_id_t>(POOL_SIZE); ++i) {
        free_frames_.push_back(i);
    }
}

BufferPool::~BufferPool() {
    // Flush all dirty frames on destruction. If the caller forgot to unpin
    // pages, we still flush — that's a logic error in the caller, but we
    // don't want to silently lose data.
    flush_all();
}

// ── Internal helpers ─────────────────────────────────────────────────────────

void BufferPool::touch(frame_id_t frame_id) {
    // If already in the LRU list, remove from current position first.
    auto it = lru_map_.find(frame_id);
    if (it != lru_map_.end()) {
        lru_list_.erase(it->second);
        lru_map_.erase(it);
    }
    // Insert at the MRU (front) end.
    lru_list_.push_front(frame_id);
    lru_map_[frame_id] = lru_list_.begin();
}

void BufferPool::evict_frame(frame_id_t frame_id) {
    Frame& f = frames_[frame_id];
    assert(f.in_use     && "evict_frame: frame is not in use");
    assert(f.pin_count == 0 && "evict_frame: frame is still pinned");

    // ── DIRTY-FLUSH BEFORE EVICTION ────────────────────────────────────────
    // Write the page to disk BEFORE overwriting the frame's contents.
    //
    // This is data loss prevention under normal buffer pool pressure, not
    // crash safety. If we skip this write, any data marked dirty by the
    // caller via unpin_page(..., dirty=true) is silently discarded when the
    // frame is reused. No crash occurs, no error is raised, and crash_test.sh
    // would not detect it because it only covers crash scenarios.
    //
    // The eviction path here must be functionally identical to flush_page():
    // if the frame is dirty, the data goes to disk, full stop.
    // ──────────────────────────────────────────────────────────────────────
    if (f.is_dirty) {
        dm_.write_page(f.page.page_id(), f.page);
        f.is_dirty = false;
    }

    // Remove from page_table_.
    page_table_.erase(f.page.page_id());

    // Remove from LRU structures.
    auto lru_it = lru_map_.find(frame_id);
    if (lru_it != lru_map_.end()) {
        lru_list_.erase(lru_it->second);
        lru_map_.erase(lru_it);
    }

    // Clear frame and return to free pool.
    f.page.clear();
    f.pin_count = 0;
    f.is_dirty  = false;
    f.in_use    = false;
}

frame_id_t BufferPool::find_free_frame() {
    // Fast path: a frame is not currently holding any page.
    if (!free_frames_.empty()) {
        frame_id_t id = free_frames_.back();
        free_frames_.pop_back();
        return id;
    }

    // Slow path: find the LRU unpinned frame and evict it.
    // Scan from the back of lru_list_ (least recently used first).
    // We capture the victim ID before calling evict_frame to avoid
    // invalidating the iterator via list mutation inside evict_frame.
    frame_id_t victim = static_cast<frame_id_t>(INVALID_PAGE_ID);
    for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
        if (frames_[*it].pin_count == 0) {
            victim = *it;
            break;
        }
    }

    if (victim == static_cast<frame_id_t>(INVALID_PAGE_ID)) {
        // Every frame is pinned — pool is exhausted.
        return static_cast<frame_id_t>(INVALID_PAGE_ID);
    }

    evict_frame(victim);
    // evict_frame clears the frame and removes it from all structures.
    // The frame_id is now clean and ready for use.
    return victim;
}

// ── Public interface ─────────────────────────────────────────────────────────

Page* BufferPool::fetch_page(page_id_t page_id) {
    // Cache hit.
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t id = it->second;
        frames_[id].pin_count++;
        touch(id);
        return &frames_[id].page;
    }

    // Cache miss — get a free frame (may evict LRU).
    frame_id_t id = find_free_frame();
    if (id == static_cast<frame_id_t>(INVALID_PAGE_ID)) {
        return nullptr;  // pool exhausted
    }

    Frame& f = frames_[id];
    dm_.read_page(page_id, f.page);
    f.in_use    = true;
    f.pin_count = 1;
    f.is_dirty  = false;

    page_table_[page_id] = id;
    touch(id);

    return &f.page;
}

void BufferPool::unpin_page(page_id_t page_id, bool dirty) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return;

    Frame& f = frames_[it->second];
    if (f.pin_count > 0) {
        --f.pin_count;
    }
    if (dirty) {
        f.is_dirty = true;
    }
}

bool BufferPool::flush_page(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;

    Frame& f = frames_[it->second];
    if (f.is_dirty) {
        dm_.write_page(page_id, f.page);
        f.is_dirty = false;
    }
    return true;
}

void BufferPool::flush_all() {
    for (auto& [pid, fid] : page_table_) {
        Frame& f = frames_[fid];
        if (f.is_dirty) {
            dm_.write_page(pid, f.page);
            f.is_dirty = false;
        }
    }
}

Page* BufferPool::new_page(page_id_t& out_page_id) {
    // Try the free list first; only extend the file if no recycled page exists.
    page_id_t page_id = free_list_.allocate();
    if (page_id == INVALID_PAGE_ID) {
        page_id = dm_.allocate_page();
    }

    frame_id_t id = find_free_frame();
    if (id == static_cast<frame_id_t>(INVALID_PAGE_ID)) {
        // Pool is full. The page_id was consumed (either from free list or
        // file extension). For M1 workloads (max 64 pinned pages) this is a
        // caller bug, so we assert rather than silently lose track of the ID.
        assert(false && "new_page: pool exhausted — all frames are pinned");
        return nullptr;
    }

    Frame& f = frames_[id];
    f.page.clear();
    f.page.set_page_id(page_id);
    f.in_use    = true;
    f.pin_count = 1;
    f.is_dirty  = true;  // New page must reach disk eventually.

    page_table_[page_id] = id;
    touch(id);

    out_page_id = page_id;
    return &f.page;
}

void BufferPool::delete_page(page_id_t page_id) {
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t id = it->second;
        Frame& f = frames_[id];
        assert(f.pin_count == 0 &&
               "delete_page: page is still pinned — unpin it first");

        // The page's contents are being discarded. Clear dirty flag so
        // evict_frame does not flush stale data to disk for a page that
        // no longer logically exists.
        f.is_dirty = false;
        evict_frame(id);
    }

    // Return the page ID to the free list so it can be reused.
    free_list_.free(page_id);
}

} // namespace keyvdb
