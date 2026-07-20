#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "storage/page.h"
#include "storage/buffer_pool.h"
#include "storage/free_list.h"

// ─────────────────────────────────────────────────────────────────────────────
// btree.h — ACID-safe B+Tree built on top of KeyVDB's BufferPool.
//
// BTree is the primary data structure for key-value storage. Every operation
// goes through the BufferPool — pages are pinned, read or modified, then
// unpinned. The BufferPool's LRU eviction and dirty-flush-on-eviction guarantee
// ensure that modified pages reach disk without requiring BTree to know about
// DiskManager directly.
//
// Dirty-flag discipline (explicit rule):
//   Every page acquired via bp_.fetch_page() or bp_.new_page() must be
//   unpinned exactly once. The dirty argument must be true if and only if the
//   page was actually modified. Every unpin_page call site in btree.cpp carries
//   a comment explaining the dirty choice.
//
// Pin discipline during traversal (explicit rule):
//   find_leaf() unpins each internal node immediately after reading the child
//   pointer needed to descend. It returns only the final leaf page still pinned.
//   Breadcrumbs are page_id_t values (re-fetchable IDs), not held pins. This
//   pattern is safe for single-threaded M2 and is the correct shape for M4's
//   concurrent latching.
//
// Parent pointer invariant:
//   Every mutating operation maintains hdr().parent_id on all affected pages.
//   See btree.cpp and node.cpp for per-operation details.
//
// Thread safety: single-threaded for Milestone 2. A coarse std::mutex will be
// added in Milestone 4.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

class BTree {
public:
    // ── Factory methods ───────────────────────────────────────────────────────

    // Create a brand-new B+Tree: allocate a leaf root page and record its ID
    // in the FreeList's reserved slot on page 0.
    // Throws std::runtime_error if the buffer pool is exhausted.
    static BTree create(BufferPool& bp, FreeList& fl);

    // Open an existing B+Tree from a known root page ID.
    // root_page_id must be a valid, previously persisted root.
    static BTree open(BufferPool& bp, FreeList& fl, page_id_t root_page_id);

    // Non-copyable (owns a reference to the buffer pool).
    BTree(const BTree&)            = delete;
    BTree& operator=(const BTree&) = delete;
    BTree(BTree&&)                 = default;
    BTree& operator=(BTree&&)      = default;

    // ── Public API ────────────────────────────────────────────────────────────

    // Point lookup. Returns the value for `key`, or std::nullopt if not found.
    std::optional<std::string> get(std::string_view key);

    // Insert or replace. If `key` already exists, its value is overwritten.
    // Throws std::invalid_argument if key+value is too large to fit in one page.
    // Triggers splits as needed, growing the tree upward.
    void insert(std::string_view key, std::string_view value);

    // Delete. Returns true if `key` existed and was removed, false otherwise.
    // Triggers rebalancing (borrow or merge) as needed.
    bool remove(std::string_view key);

    // The current root page ID. Persisted to page 0 via FreeList after every
    // root change.
    page_id_t root_page_id() const { return root_page_id_; }

private:
    BTree(BufferPool& bp, FreeList& fl, page_id_t root);

    BufferPool& bp_;
    FreeList&   fl_;
    page_id_t   root_page_id_;

    // ── Internal helpers ──────────────────────────────────────────────────────

    // Traverse from the root to the leaf where `key` belongs.
    // Returns a pinned pointer to the leaf page.
    // Each internal node is unpinned immediately after reading its child pointer
    // (pin discipline rule — see header comment).
    // Breadcrumbs receives the page_id of each internal node visited, in
    // root-to-parent order. Only the returned leaf is still pinned.
    Page* find_leaf(std::string_view key, std::vector<page_id_t>& breadcrumbs);

    // After splitting a leaf (or internal node), insert the separator key and
    // right-child pointer into the parent. Propagates upward recursively if
    // the parent also needs to split. Creates a new root if the current root
    // splits.
    //
    // left_id and right_id are the two halves (both already unpinned by caller).
    // sep_key is the separator to insert into the parent.
    // breadcrumbs is the parent stack from find_leaf (popped as we ascend).
    void insert_into_parent(page_id_t left_id,
                            const std::string& sep_key,
                            page_id_t right_id,
                            std::vector<page_id_t>& breadcrumbs);

    // Allocate a new root internal node with `left_id` and `right_id` as
    // its two children and `sep_key` as the separator. Updates root_page_id_
    // and persists to the FreeList header.
    void create_new_root(page_id_t left_id,
                         const std::string& sep_key,
                         page_id_t right_id);

    // After removing a key that leaves a leaf underflowing, attempt to borrow
    // from a sibling or merge. Propagates upward if a merge empties the parent.
    //
    // page_id is the underflowing page (currently unpinned).
    // breadcrumbs is the parent stack.
    void fix_underflow(page_id_t page_id,
                       std::vector<page_id_t>& breadcrumbs);

    // Persist the current root_page_id_ to the FreeList's reserved slot.
    void persist_root();
};

} // namespace keyvdb
