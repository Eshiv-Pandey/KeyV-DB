#pragma once

#include <cstdint>
#include <cstring>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// page.h — The fundamental unit of storage in KeyVDB.
//
// Every other module talks about pages. A page is a fixed-size (4096-byte)
// buffer that lives both on disk and in the buffer pool. Because all I/O is
// in whole pages, we never have to deal with partial reads/writes at the
// database level.
//
// Layout of every page:
//   [PageHeader  —  24 bytes ]
//   [Usable data — 4072 bytes]
//
// The header is just cast from the front of the raw buffer. The "usable data"
// area is what the B+Tree and WAL modules write into.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

// ── Fundamental constants ─────────────────────────────────────────────────────

// 4096 bytes — matches the OS virtual-memory page size and the internal block
// size of most SSDs. Reads/writes at this size are atomic at the hardware level
// on most storage devices.
static constexpr uint32_t PAGE_SIZE = 4096;

// How many pages the buffer pool holds in RAM. Compile-time constant as agreed.
// 64 * 4096 = 256 KB total pool size.
static constexpr uint32_t BUFFER_POOL_SIZE = 64;

// Sentinel value meaning "no page" — the database equivalent of nullptr.
static constexpr int32_t INVALID_PAGE_ID = -1;

// ── Type aliases ──────────────────────────────────────────────────────────────

// Every page on disk has an integer ID. Page 0 is the header page (owned by
// FreeList). Pages 1+ are B+Tree nodes or WAL segments.
using page_id_t  = int32_t;

// Frame IDs index into the buffer pool's fixed-size frame array.
using frame_id_t = int32_t;

// Log Sequence Number — a monotonically increasing ID assigned to every WAL
// record. Stored in each page header so recovery knows whether a page's on-disk
// version is newer or older than a given WAL record. Used in Milestone 3.
using lsn_t = int64_t;

// ── Page type tag ─────────────────────────────────────────────────────────────

// Stored in every page header so we know how to interpret its contents.
enum class PageType : uint32_t {
    INVALID         = 0,  // Zeroed / uninitialized. Should never be read.
    DB_HEADER       = 1,  // Page 0: database metadata and free-page list.
    BTREE_INTERNAL  = 2,  // B+Tree internal node (keys + child pointers).
    BTREE_LEAF      = 3,  // B+Tree leaf node (keys + values + sibling pointer).
    WAL_SEGMENT     = 4,  // Write-ahead log data (Milestone 3).
};

// ── Page header ───────────────────────────────────────────────────────────────

// The first 24 bytes of every page. Tells us the page's identity and kind.
// Always exactly 24 bytes so that the header fits in a cache line and the
// usable data area starts at a predictable, 8-byte-aligned offset.
struct PageHeader {
    page_id_t page_id   = INVALID_PAGE_ID;   // 4 bytes
    PageType  page_type = PageType::INVALID;  // 4 bytes
    uint32_t  _padding  = 0;                 // 4 bytes — keeps the struct aligned
    uint32_t  _padding2 = 0;                 // 4 bytes
    lsn_t     page_lsn  = 0;                 // 8 bytes — last WAL record that wrote to this page
};
static_assert(sizeof(PageHeader) == 24, "PageHeader size changed — update comments above");

// ── Page ─────────────────────────────────────────────────────────────────────

// A 4096-byte page. This is the type that lives in buffer pool frames and
// gets passed to pread/pwrite.
//
// IMPORTANT: Don't put Page on the stack. It's 4KB and aligned to 4096 bytes,
// which will overflow typical stack sizes if you have many of them. Always
// allocate via the buffer pool or on the heap.
//
// The `alignas(PAGE_SIZE)` alignment is needed for O_DIRECT I/O (which we
// may use in the future) and ensures pread/pwrite operate on a well-aligned
// buffer, which the kernel can handle more efficiently.
class Page {
public:
    static constexpr uint32_t HEADER_SIZE = sizeof(PageHeader);  // 24
    static constexpr uint32_t DATA_SIZE   = PAGE_SIZE - HEADER_SIZE; // 4072

    Page() { clear(); }

    // Zero out everything and reset the header to its default state.
    void clear() { std::memset(data_, 0, PAGE_SIZE); }

    // ── Raw buffer access (used by DiskManager for pread/pwrite) ─────────────
    char*       raw_data()       { return data_; }
    const char* raw_data() const { return data_; }

    // ── Header access ─────────────────────────────────────────────────────────
    // We cast the front of the buffer to PageHeader. This is safe because:
    //   1. data_ is aligned to PAGE_SIZE (which is >= alignof(PageHeader))
    //   2. PageHeader only contains trivially-copyable types
    //   3. We wrote the header into this buffer ourselves
    PageHeader&       header()
        { return *reinterpret_cast<PageHeader*>(data_); }
    const PageHeader& header() const
        { return *reinterpret_cast<const PageHeader*>(data_); }

    // ── Usable data area (what B+Tree and WAL modules write into) ─────────────
    char*       data()       { return data_ + HEADER_SIZE; }
    const char* data() const { return data_ + HEADER_SIZE; }

    // ── Convenience getters / setters ─────────────────────────────────────────
    page_id_t page_id()   const { return header().page_id; }
    PageType  page_type() const { return header().page_type; }
    lsn_t     page_lsn()  const { return header().page_lsn; }

    void set_page_id(page_id_t id)  { header().page_id   = id; }
    void set_page_type(PageType t)  { header().page_type = t; }
    void set_page_lsn(lsn_t lsn)   { header().page_lsn  = lsn; }

private:
    // The raw 4096-byte buffer. The PageHeader lives at the start; usable
    // data follows. Aligned to PAGE_SIZE for efficient and correct I/O.
    alignas(PAGE_SIZE) char data_[PAGE_SIZE];
};

static_assert(sizeof(Page) == PAGE_SIZE, "Page must be exactly PAGE_SIZE bytes");

} // namespace keyvdb
