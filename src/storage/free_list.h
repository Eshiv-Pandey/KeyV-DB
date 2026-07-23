#pragma once

#include <vector>
#include "storage/disk_manager.h"

// ─────────────────────────────────────────────────────────────────────────────
// free_list.h — On-disk free-page registry.
//
// The FreeList lives on page 0 of the database file. It tracks which page IDs
// have been freed (via BufferPool::delete_page) and are available for reuse.
// The BufferPool calls FreeList::allocate() before DiskManager::allocate_page()
// so that deleted pages are recycled before the file grows.
//
// Why FreeList talks to DiskManager directly (not BufferPool):
//
//   1. Circular dependency prevention. BufferPool calls into FreeList, so
//      FreeList cannot hold a reference back to BufferPool. The chain is:
//        DiskManager → FreeList → (FreeList + DiskManager) → BufferPool
//      i.e., construction order: DiskManager, then FreeList, then BufferPool.
//
//   2. Page 0 must not be evicted. If page 0 went through the pool it could
//      be silently evicted mid-mutation, leaving an inconsistent free list.
//      Reading and writing it directly through DiskManager avoids this.
//
// On-disk layout of page 0 (data area, after the 24-byte PageHeader):
//   Offset 0:  uint32_t  count          — number of entries
//   Offset 4:  page_id_t ids[count]     — free page IDs, 4 bytes each
//
// Maximum capacity: (Page::DATA_SIZE - sizeof(uint32_t)) / sizeof(page_id_t)
//                 = (4072 - 4) / 4 = 1017 entries.
//
// Root page ID storage: the last sizeof(page_id_t) bytes of page 0's data area
// are reserved for the B+Tree root page ID. This is a fixed slot at offset
// (Page::DATA_SIZE - sizeof(page_id_t)) = 4068. It does not overlap the free
// list entries because the free list can hold at most 1017 entries × 4 bytes =
// 4068 bytes, but the count field (4 bytes) at offset 0 brings the free list's
// maximum used range to 4 + 4068 = 4072 bytes. To avoid collision we cap
// MAX_FREE_PAGES at 1016 entries (leaving the final 4 bytes free for root ID).
// Root page ID is INVALID_PAGE_ID (-1) until the B+Tree creates its first page.
//
// Crash semantics: the in-memory list is authoritative. save() serialises it
// to disk and calls dm_.flush() (fsync). A crash before save() returns can
// orphan a freed page — it will not be reused. This is an accepted M1
// limitation; the WAL in Milestone 3 will journal free-list mutations so
// recovery can replay them.
//
// Thread safety: single-threaded for Milestone 1.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

class FreeList {
public:
    // Maximum number of free-page IDs that fit in page 0's data area.
    // Capped at 1016 (not 1017) so the final 4 bytes of the data area are
    // reserved for the root page ID without any overlap.
    static constexpr uint32_t MAX_FREE_PAGES =
        (Page::DATA_SIZE - sizeof(uint32_t) - sizeof(page_id_t)) / sizeof(page_id_t);

    // Byte offset of the root page ID slot within page 0's data area.
    // Fixed at the very end of the data area.
    static constexpr uint32_t ROOT_ID_OFFSET =
        Page::DATA_SIZE - sizeof(page_id_t);

    // Takes DiskManager& — NOT BufferPool& — to avoid the circular dependency.
    explicit FreeList(DiskManager& dm);

    // Non-copyable.
    FreeList(const FreeList&)            = delete;
    FreeList& operator=(const FreeList&) = delete;

    // Read the free list from page 0 of the database file.
    // If the database is new (dm_.num_pages() == 0), this method allocates
    // page 0 via dm_.allocate_page(), initialises it on disk, and returns.
    // Must be called exactly once after construction, before any allocate/free.
    void load();

    // Write the current in-memory list to page 0 and call dm_.flush() (fsync).
    // Called automatically inside allocate() and free() — every mutation is
    // immediately durable. Intentionally conservative for Milestone 1.
    void save();

    // Remove and return a free page ID from the list, then call save().
    // Returns INVALID_PAGE_ID if the list is empty — the caller should then
    // call dm_.allocate_page() to extend the file.
    page_id_t allocate();

    // Add page_id to the free list and call save().
    // Asserts that the list has not exceeded MAX_FREE_PAGES.
    void free(page_id_t id);

    // Read the B+Tree root page ID from the reserved slot in page 0.
    // Returns INVALID_PAGE_ID if the slot has never been written (new database).
    // Reads directly from disk (does not go through BufferPool) for consistency
    // with how load()/save() work.
    page_id_t root_page_id() const;

    // Write the B+Tree root page ID to the reserved slot in page 0 and
    // call dm_.flush(). Called by BTree whenever the root page changes.
    void set_root_page_id(page_id_t id);

    bool     empty() const { return free_pages_.empty(); }
    uint32_t size()  const { return static_cast<uint32_t>(free_pages_.size()); }

private:
    DiskManager&            dm_;
    std::vector<page_id_t>  free_pages_;
};

} // namespace keyvdb
