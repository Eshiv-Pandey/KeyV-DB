#pragma once

#include <string>
#include <stdexcept>
#include <system_error>

#include "storage/page.h"

// ─────────────────────────────────────────────────────────────────────────────
// disk_manager.h — Raw POSIX I/O layer for KeyVDB.
//
// DiskManager is the only component that touches the filesystem. Every other
// module asks the BufferPool for pages; the BufferPool calls DiskManager when
// it needs to read from or write to disk. The one exception is FreeList, which
// bypasses the buffer pool and talks to DiskManager directly so that page 0
// is never silently evicted mid-mutation.
//
// File layout on disk:
//   Page 0            — DB header + FreeList entries (owned by FreeList)
//   Pages 1+          — B+Tree nodes, WAL segments, etc.
//
// Byte offset of page N = N * PAGE_SIZE.
//
// I/O strategy:
//   - pread / pwrite: explicit byte-offset I/O; no userspace buffering.
//   - fsync: called via flush() after every commit-critical write.
//   - No O_DIRECT for now (requires buffer alignment + Linux-specific
//     tuning). fsync is our durability mechanism.
//
// Thread safety: single-threaded for Milestone 1. A coarse lock will be added
// in Milestone 4 alongside the lock manager.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

class DiskManager {
public:
    // Open (or create) the database file at db_path.
    //
    // If the file already exists, num_pages_ is computed from the actual file
    // size: lseek(fd, 0, SEEK_END) / PAGE_SIZE. This is critical — if we
    // initialised num_pages_ to 0 on reopen, recovery logic and
    // PersistAcrossReopen would silently break.
    //
    // If the file is new (size == 0), num_pages_ starts at 0. FreeList::load()
    // will call allocate_page() to create page 0 on first use.
    //
    // Throws std::system_error on failure to open.
    explicit DiskManager(const std::string& db_path);

    // Close the file descriptor. Does not flush — call flush() first if needed.
    ~DiskManager();

    // Non-copyable, non-movable: owns an OS file descriptor.
    DiskManager(const DiskManager&)            = delete;
    DiskManager& operator=(const DiskManager&) = delete;
    DiskManager(DiskManager&&)                 = delete;
    DiskManager& operator=(DiskManager&&)      = delete;

    // Read the page at page_id into `page`.
    // Throws std::out_of_range if page_id >= num_pages_.
    // Throws std::system_error on pread failure.
    // Throws std::runtime_error on short read.
    void read_page(page_id_t page_id, Page& page);

    // Write `page` to the file at page_id's location.
    // Does NOT call fsync — the data may still be in the OS page cache.
    // Call flush() to guarantee durability.
    // Throws std::system_error on pwrite failure.
    // Throws std::runtime_error on short write.
    void write_page(page_id_t page_id, const Page& page);

    // Force all pending writes to the storage device (fsync).
    // After flush() returns, data written since the last flush() is durable.
    // Throws std::system_error on failure.
    void flush();

    // Extend the file by one page and return the new page's ID.
    // The new page is zero-initialised on disk (pwrite of a zeroed Page).
    // The caller is responsible for writing meaningful content.
    // Throws std::system_error on pwrite failure.
    page_id_t allocate_page();

    // Number of pages currently in the file (includes page 0 once allocated).
    page_id_t num_pages() const { return num_pages_; }

private:
    int         fd_;
    page_id_t   num_pages_;
    std::string db_path_;
};

} // namespace keyvdb
