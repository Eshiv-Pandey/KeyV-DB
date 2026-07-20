#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#include "storage/page.h"
#include "btree/btree_defs.h"

// ─────────────────────────────────────────────────────────────────────────────
// node.h — Thin view over a buffer-pool-pinned Page for B+Tree operations.
//
// Node does NOT own the page — it holds a raw pointer to a Page that is
// currently pinned in the BufferPool. Callers are responsible for:
//   1. Calling bp.fetch_page() / bp.new_page() to obtain and pin the page.
//   2. Using the Node view to read or mutate it.
//   3. Calling bp.unpin_page(id, dirty) exactly once when done.
//
// dirty=true must be passed to unpin_page if and only if any Node method that
// modifies the page was called. Read-only methods (lower_bound, key_at, etc.)
// do not dirty the page. See btree.cpp for the explicit rule and per-call-site
// comments.
//
// Parent pointer invariant:
//   Every method that changes the tree structure (split_into, merge_from,
//   steal_from_left, steal_from_right) must leave parent_id correct on every
//   affected page. See method docs below for specifics.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

class Node {
public:
    // ── Construction ──────────────────────────────────────────────────────────

    // Wrap an already-pinned page as a Node view.
    // The page must have page_type() == BTREE_INTERNAL or BTREE_LEAF.
    explicit Node(Page* page);

    // Initialise a brand-new, zeroed page as a leaf node.
    // Sets NodeHeader fields to their starting values.
    // page_type is set to BTREE_LEAF. Call set_page_type(BTREE_INTERNAL) after
    // this if constructing an internal node.
    static void init_leaf(Page* page, page_id_t page_id,
                          page_id_t parent_id = INVALID_PAGE_ID);

    // Initialise a brand-new, zeroed page as an internal node.
    static void init_internal(Page* page, page_id_t page_id,
                              page_id_t parent_id = INVALID_PAGE_ID);

    // ── Header accessors ──────────────────────────────────────────────────────

    NodeHeader&       hdr()       { return *reinterpret_cast<NodeHeader*>(page_->data()); }
    const NodeHeader& hdr() const { return *reinterpret_cast<const NodeHeader*>(page_->data()); }

    page_id_t page_id()   const { return page_->page_id(); }
    bool      is_leaf()   const { return page_->page_type() == PageType::BTREE_LEAF; }
    uint16_t  num_keys()  const { return hdr().num_keys; }

    // ── Key / value / child access (read-only) ────────────────────────────────

    // Return the key stored at sorted slot index `slot`.
    std::string_view key_at(uint16_t slot) const;

    // Return the value stored at sorted slot index `slot`. Leaf nodes only.
    std::string_view value_at(uint16_t slot) const;

    // Return the right-child page_id stored at sorted slot index `slot`.
    // Internal nodes only. The leftmost child is hdr().first_child.
    page_id_t child_at(uint16_t slot) const;

    // ── Search ────────────────────────────────────────────────────────────────

    // Binary search. Returns the first slot index whose key >= `key`.
    // Returns num_keys() if all keys are < `key`.
    uint16_t lower_bound(std::string_view key) const;

    // ── Capacity queries ──────────────────────────────────────────────────────

    // Free-space check. Returns true if adding a cell of `new_cell_bytes`
    // payload (not counting the slot entry) would leave no room.
    // The check accounts for both the slot array growing down from
    // SLOT_ARRAY_START + num_keys*sizeof(slot_offset_t), and the cell heap
    // growing up from the bottom of the data area.
    // BTREE_ORDER_ESTIMATE is NOT used here.
    bool is_full(size_t new_cell_bytes) const;

    // Returns true if the node's occupied bytes are < NODE_USABLE_SIZE / 2.
    // Used post-delete to decide whether to borrow from a sibling or merge.
    bool underflow() const;

    // Returns the number of bytes of free space between the slot array
    // frontier and the cell heap frontier.
    size_t free_space() const;

    // ── Mutation — leaf ───────────────────────────────────────────────────────

    // Insert or replace a key-value cell in a leaf node.
    // If key already exists, the old value is replaced in-place (or the cell
    // is removed-and-reinserted if the value size differs).
    // Returns false if there is no room (caller must split first, then retry).
    // Caller must call unpin_page(id, dirty=true) after this.
    bool insert_leaf_cell(std::string_view key, std::string_view value);

    // Remove the cell at sorted `slot`. The freed space stays fragmented
    // until compact() is called (compaction happens lazily — only when needed
    // to satisfy is_full()). Caller must unpin with dirty=true.
    void remove_cell(uint16_t slot);

    // ── Mutation — internal ───────────────────────────────────────────────────

    // Insert a (key, right_child) separator into an internal node.
    // `right_child` is the page to the right of the new key.
    // Returns false if there is no room.
    // Caller must unpin with dirty=true.
    bool insert_separator(std::string_view key, page_id_t right_child);

    // Update the separator key at slot `slot` (same size as old key required).
    // Used when a steal changes the separator in the parent.
    // Caller must unpin with dirty=true.
    void update_separator_key(uint16_t slot, std::string_view new_key);

    // Update the right-child pointer at slot `slot`.
    // Caller must unpin with dirty=true.
    void update_child_at(uint16_t slot, page_id_t child_id);

    // ── Structural mutations ──────────────────────────────────────────────────
    //
    // All of these methods mutate `this` AND the node(s) passed as arguments.
    // Callers must unpin ALL involved pages with dirty=true.
    // Parent pointer correctness is part of the postcondition.

    // Split this node's cells evenly (by byte count) into this (left) and
    // `right` (right, must be a freshly-zeroed, same-type node).
    // `separator_key` is set to the median key (for the parent to absorb).
    //
    // Parent pointer postcondition:
    //   right.hdr().parent_id is set to this->hdr().parent_id.
    //   (The parent's pointer to `this` is unchanged; btree.cpp will insert
    //   the separator and update child pointers in the parent.)
    //
    // For internal nodes: the median key is promoted (not copied to either
    // half). For leaf nodes: the median key stays in the right half (B+Tree
    // convention — leaf nodes keep all data).
    void split_into(Node& right, std::string& separator_key);

    // Absorb all cells from `right` into this node (merge).
    // `separator_key` is the parent separator between this and right; it is
    // pulled down into this node if this is an internal node.
    //
    // Parent pointer postcondition (internal merge only):
    //   All children of `right` (i.e., right.hdr().first_child and every
    //   right.child_at(i)) must have their parent_id rewritten to this->page_id().
    //   This requires fetching+unpinning those child pages inside merge_from,
    //   which means merge_from takes a BufferPool& to do so.
    //
    // After merge, right's page should be deleted by the caller.
    void merge_from(Node& right, std::string_view separator_key,
                    class BufferPool& bp);

    // Steal one cell from the left sibling to cure underflow.
    // `old_sep` is the current parent separator between left and this.
    // `new_sep` is set to the key that should replace that separator in parent.
    //
    // Parent pointer postcondition (internal steal only):
    //   The child pointer that moved from left to this must have its parent_id
    //   updated to this->page_id(). steal_from_left handles this internally.
    void steal_from_left(Node& left, std::string_view old_sep,
                         std::string& new_sep, class BufferPool& bp);

    // Steal one cell from the right sibling to cure underflow.
    // `old_sep` is the current parent separator between this and right.
    // `new_sep` is set to the new separator for the parent.
    void steal_from_right(Node& right, std::string_view old_sep,
                          std::string& new_sep, class BufferPool& bp);

    // ── Utility ───────────────────────────────────────────────────────────────

    // Compact the cell heap, eliminating any fragmentation left by remove_cell.
    // Rewrites all slot offsets to point into the newly compacted heap.
    void compact();

private:
    Page* page_;

    // ── Slot array helpers ────────────────────────────────────────────────────

    // Pointer to the start of the slot array (within page_.data()).
    slot_offset_t*       slots()
        { return reinterpret_cast<slot_offset_t*>(page_->data() + SLOT_ARRAY_START); }
    const slot_offset_t* slots() const
        { return reinterpret_cast<const slot_offset_t*>(page_->data() + SLOT_ARRAY_START); }

    // ── Cell layout parsing ───────────────────────────────────────────────────

    // Pointer to the start of cell `slot`'s bytes within page_.data().
    const char* cell_ptr(uint16_t slot) const
        { return page_->data() + slots()[slot]; }
    char*       cell_ptr(uint16_t slot)
        { return page_->data() + slots()[slot]; }

    // Write a leaf cell into the cell heap, update the slot array.
    // Asserts that there is room.
    void write_leaf_cell(uint16_t insert_slot,
                         std::string_view key, std::string_view value);

    // Write an internal cell into the cell heap, update the slot array.
    void write_internal_cell(uint16_t insert_slot,
                             std::string_view key, page_id_t right_child);

    // Shift slot entries [from_slot .. num_keys-1] one position to the right
    // to make room for a new slot at `from_slot`.
    void shift_slots_right(uint16_t from_slot);

    // Shift slot entries [from_slot+1 .. num_keys-1] one position to the left
    // to close the gap left by removing slot `from_slot`.
    void shift_slots_left(uint16_t from_slot);
};

} // namespace keyvdb
