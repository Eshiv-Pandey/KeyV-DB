# progress2.md — The Storage Manager and the B+Tree

*This is the second entry in the KeyVDB build log. Milestone 0 covered what a database is and why we're building one. This entry covers Milestones 1 and 2: the storage manager (the layer that talks to disk) and the B+Tree (the data structure that gives us ordered key lookups). Both are now complete and passing tests.*

*These notes assume you've read progress1.md. If you haven't, start there.*

---

## Milestone 1: The Storage Manager

Before we can store anything, we need a layer that handles raw I/O. Three components form the storage manager:

- **DiskManager** — reads and writes pages to and from the file using `pread`/`pwrite`.
- **FreeList** — tracks which page IDs have been freed and are available for reuse.
- **BufferPool** — an in-memory cache of recently-used pages, with LRU eviction.

They're built in dependency order: DiskManager has no dependencies, FreeList depends on DiskManager, and BufferPool depends on both.

---

### DiskManager: the only thing that touches the filesystem

The entire database lives in a single binary file. Every other module talks in pages; DiskManager is what translates "page 7" into "bytes 7 × 4096 through 8 × 4096 of the file."

The interface is simple:

```cpp
void read_page(page_id_t page_id, Page& page);
void write_page(page_id_t page_id, const Page& page);
void flush();
page_id_t allocate_page();
```

`read_page` and `write_page` call `pread` and `pwrite` directly with explicit byte offsets. There's no buffering in userspace — the bytes go straight to the OS. Whether the OS writes them to the physical device before the next crash is a separate question (answered by `flush()`).

`flush()` calls `fsync(fd)`. This is the durability call. Everything else is performance; `fsync` is correctness. After `flush()` returns, every `write_page` call issued since the last `flush()` is guaranteed to be on durable storage.

`allocate_page()` extends the file by one page. It writes a zeroed page at the new offset so the extension is explicit, not relying on sparse-file behavior from `ftruncate`. The new page's ID is returned to the caller.

When reopening an existing database, `num_pages_` is initialized from the actual file size via `lseek(fd, 0, SEEK_END) / PAGE_SIZE`. This is critical: if we reset `num_pages_` to 0 on every open, a reopened database would think the file is empty and overwrite existing data on the next allocation.

---

### The Page type

Every page is exactly 4096 bytes. The first 24 bytes are a `PageHeader`:

```cpp
struct PageHeader {
    page_id_t page_id   = INVALID_PAGE_ID;  // 4 bytes
    PageType  page_type = PageType::INVALID; // 4 bytes
    uint32_t  _padding  = 0;                // 4 bytes
    uint32_t  _padding2 = 0;                // 4 bytes
    lsn_t     page_lsn  = 0;               // 8 bytes  (used in Milestone 3)
};
```

The remaining 4072 bytes are the "data area" — what the B+Tree and WAL write into.

The `Page` class is `alignas(PAGE_SIZE)` so that a pointer to a `Page` is always 4096-byte aligned. This matters for `pread`/`pwrite` efficiency, and it'll matter more if we ever use `O_DIRECT` to bypass the OS page cache.

A few practical rules we've established around `Page`:

1. Don't put `Page` on the stack. It's 4KB — fine on the heap, fatal in a tight recursion.
2. `page_type` is how any module knows whether to interpret a page as a B+Tree leaf, an internal node, the free-list header, or a WAL segment. We never guess.
3. `page_lsn` (Log Sequence Number) is stored but not yet used. It will be read during crash recovery to determine whether a page is older or newer than a given WAL record.

---

### FreeList: page recycling

When a B+Tree node is deleted (after a merge), its page ID should be available for future allocations. Without recycling, the file would only ever grow. The FreeList handles this.

The FreeList stores its data on **page 0** of the database file. Every mutation (adding a page to the list, removing one) is immediately serialized to disk and `fsync`'d. This is conservative — a group-commit approach would be faster — but for Milestone 1 correctness matters more than throughput.

The layout of page 0's data area:

```
Offset 0:      uint32_t count          — number of free page IDs
Offset 4:      page_id_t ids[count]    — the free page IDs (4 bytes each)
...
Last 4 bytes:  page_id_t root_id       — reserved slot for the B+Tree root page ID
```

The last 4 bytes are reserved so the B+Tree can persist its root page ID without needing a separate metadata page. The FreeList and the root ID slot coexist on page 0 with no overlap, because the maximum number of free entries (1016) is calculated to stay clear of the tail slot.

**Why FreeList talks to DiskManager directly (not BufferPool):** BufferPool calls FreeList, so FreeList can't hold a reference back to BufferPool — that would be a circular dependency. There's also a practical reason: page 0 must never be silently evicted mid-mutation. If it went through the pool, it could be evicted while a save() operation was in progress. Talking to DiskManager directly avoids the problem entirely.

---

### BufferPool: keeping pages in RAM

Disk reads are 1000× slower than RAM accesses. The buffer pool is the bridge: it keeps recently-used pages in a fixed-size array of 64 "frames" and only goes to disk when a frame needs to be evicted.

The interface is:

```cpp
Page* fetch_page(page_id_t page_id);         // pin and return (load if needed)
void  unpin_page(page_id_t page_id, bool dirty);  // release a pin
Page* new_page(page_id_t& out_page_id);      // allocate + pin a fresh page
void  delete_page(page_id_t page_id);        // remove from pool + add to FreeList
bool  flush_page(page_id_t page_id);         // write dirty page to disk
void  flush_all();                           // write all dirty pages to disk
```

The "pin count" is the key mechanism. A page's pin count represents how many callers are currently using it. A page with `pin_count > 0` cannot be evicted. When you call `fetch_page`, the pin count goes up; when you call `unpin_page`, it goes down. When it hits 0, the frame becomes eligible for eviction.

Eviction policy is LRU (Least Recently Used). Every `fetch_page` call moves the frame to the "most recently used" end of a doubly-linked list. When we need a free frame and none are available, we scan from the "least recently used" end and evict the first frame with `pin_count == 0`.

The most important invariant in the buffer pool is **dirty-flush-before-eviction**: when a frame marked dirty is chosen for eviction, it is written to disk *before* its contents are overwritten. This is data loss prevention under normal operation. If we skipped the write, any caller that had called `unpin_page(id, dirty=true)` would have their changes silently discarded when memory pressure triggered eviction. No crash, no error — just vanished data.

The `dirty` argument to `unpin_page` is an explicit contract: the caller says "I did (or did not) modify this page." Every call site in the codebase has a comment explaining the dirty choice. This discipline catches bugs early.

---

## Milestone 2: The B+Tree

The B+Tree is the core data structure. It gives us ordered key-value storage with O(log n) lookups, inserts, and deletes, and O(1) range scans once you're at the right leaf.

The tree has two types of nodes:

- **Internal nodes**: contain keys and child page IDs. Used for routing lookups.
- **Leaf nodes**: contain actual key-value pairs. Linked in a sorted doubly-linked list for range scans.

All nodes live on B+Tree pages managed by the BufferPool.

---

### Why B+Tree, not binary tree?

A binary search tree puts one key per node. Finding a key in a million-row tree might require 20 node lookups, each of which could be a disk read. At 10ms per disk read, that's 200ms per lookup.

A B+Tree packs hundreds of keys into each node (a 4KB page can hold ~100 key-value pairs at typical sizes). A million-row tree needs only 2–3 levels. That's 2–3 disk reads, not 20.

The other advantage of B+Trees: all data lives in leaf nodes, and leaf nodes are linked. A range scan of "all keys between 'apple' and 'banana'" starts by finding the first leaf containing 'apple', then follows the `right_sibling` pointer until we pass 'banana'. No tree traversal needed for the range part — just follow the chain.

---

### Slotted page layout

Each node is a 4KB page. Within the 4072-byte data area, we use a slotted page layout:

```
[NodeHeader — 32 bytes       ]  fixed metadata
[Slot array — grows →        ]  uint16_t offsets into cell heap
[Free space — in the middle  ]
[Cell heap  — ← grows        ]  actual key/value/pointer bytes
```

The slot array starts right after the NodeHeader and grows toward the center as cells are inserted. The cell heap starts at the end of the data area and grows backward. Free space is the gap between them.

This layout has two important properties:

1. **Variable-length keys and values**: cells can be any size, and the slot array provides O(1) indexed access to any of them regardless of size.
2. **In-place delete**: removing a cell marks its slot as gone but doesn't immediately compact the heap. The freed bytes become "fragmented" space. `compact()` reclaims them when the pool of free space runs out — lazy compaction rather than eager.

**Fullness check**: we don't use a key count threshold. We check whether inserting the new cell (payload bytes + 2 for the slot entry) would exhaust the remaining free space. This handles variable-length keys correctly — a node with 30 short keys might have more room than a node with 10 long ones.

---

### Split

When a leaf is full and we need to insert a new key, we:

1. Allocate a new leaf page.
2. Split the existing leaf's cells ~evenly (by byte count, not key count) between the two pages.
3. Insert the new key into whichever half it belongs to.
4. Push a separator key (the first key of the right half) up into the parent.

If the parent is also full, we split the parent too, and so on up the tree. If we reach the root and the root needs to split, we allocate a new root and grow the tree by one level. The tree can only grow taller by splitting the root — it never grows from the bottom.

Leaf splits in B+Trees use "copy-up" semantics: the separator key is copied to the parent but also stays in the right leaf. Internal splits use "push-up" semantics: the median key is moved to the parent and removed from both halves. This is the standard B+Tree convention and is necessary for correctness — leaf nodes need to hold all data, while internal nodes only hold routing keys.

---

### Remove and underflow handling

Removing a key from a leaf is straightforward: find the slot, remove the cell, adjust the slot array. The hard part comes when the leaf is left less than half full ("underflowing").

An underflowing node needs to be fixed before we return. There are three cases, tried in order:

1. **Borrow from left sibling**: if the left sibling has more than the minimum number of entries, move its rightmost key to our leftmost position. Update the separator in the parent to reflect the new split point.

2. **Borrow from right sibling**: same idea, but move the right sibling's leftmost key to our rightmost position.

3. **Merge with a sibling**: if neither sibling can spare a key, merge this node with one of its siblings. The separator in the parent that divided them gets pulled down (for internal nodes) or discarded (for leaves), and the parent loses one key. If the parent is now underflowing, the same process recurses upward.

Merge can cascade all the way to the root. If the root ends up as an internal node with zero keys (only one child), we collapse the tree: the single child becomes the new root, and the old root page is freed. This is how the tree shrinks.

---

### Parent pointer invariant

Every page stores the page ID of its parent in `NodeHeader.parent_id`. This is maintained across every structural operation: splits, merges, and borrows. After a merge, all children of the absorbed node get their `parent_id` updated to point to the survivor. After a root creation, both children get updated to point to the new root.

Why do we maintain this? In the current single-threaded implementation, we could instead walk breadcrumbs. But maintaining parent pointers correctly now sets up Milestone 4 correctly: with concurrent latching, we need to know the parent without re-traversing the tree, because we'll be holding latches on the way down and can't hold the whole path pinned.

---

### Pin discipline during traversal

A core rule in the B+Tree: each internal node is unpinned immediately after reading the child pointer needed to descend. Only the final leaf page is returned still pinned. Parent IDs are stored as integers (re-fetchable IDs), not held pins.

Why? Because the buffer pool has only 64 frames. In a deep tree, if we kept every node on the path to a leaf pinned during traversal, we'd exhaust the pool after 64 levels of depth — and block our own operation from getting a frame for the next allocation.

The same pattern is correct for Milestone 4: when we add latching, we'll want "latch crabbing" (hold child latch, release parent latch), not holding all latches from root to leaf simultaneously.

---

### Crash test shape (as of M2)

The current `crash_test.sh` tests disk-level torn writes in the storage manager, not the B+Tree yet. A proper crash test for the B+Tree requires the WAL (Milestone 3): only with a recovery log can we distinguish between "crashed before writing the new page" and "data corruption." We'll extend the crash test in Milestone 3 to cover mid-commit crashes and verify that recovery restores the tree to a consistent state.

---

## What comes next

Milestone 3 is the Write-Ahead Log and crash recovery. By the end of it:

- Every B+Tree write will first log its intent to a WAL file.
- On `Commit()`, the WAL is `fsync`'d before the data pages are written.
- On startup after a crash, a recovery manager reads the WAL and brings the database to a consistent state using redo and undo.
- The crash test will be extended to cover mid-transaction crashes and verify correct recovery.

The transaction manager (Milestone 4) sits on top of the WAL. We'll also add the public `db.h` API in Milestone 4 — `Begin()`, `Get()`, `Put()`, `Commit()`, `Rollback()` — which is the interface shown in the README.

---

*Next: [progress3.md](progress3.md) — the Write-Ahead Log and crash recovery (coming after Milestone 3)*
