#include "btree/node.h"
#include "storage/buffer_pool.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// node.cpp — Slotted page implementation for B+Tree nodes.
//
// Memory layout reminder (all offsets relative to page_->data()):
//   [0 .. NODE_HEADER_SIZE)            NodeHeader
//   [SLOT_ARRAY_START .. free_end)     Slot array (grows toward higher offsets)
//   [free_end .. cell_end)             Free space
//   [cell_end .. NODE_DATA_SIZE)       Cell heap (grows toward lower offsets)
//
// hdr().free_end  = offset of first byte PAST the last slot entry.
//                   Starts at SLOT_ARRAY_START; grows by sizeof(slot_offset_t)
//                   per new slot.
// hdr().cell_end  = offset of the FIRST byte of the lowest cell in the heap.
//                   Starts at NODE_DATA_SIZE; grows downward (shrinks numerically)
//                   as cells are added.
//
// free_space() = cell_end - free_end.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

// ── Construction ──────────────────────────────────────────────────────────────

Node::Node(Page* page) : page_(page) {
    assert(page_ != nullptr);
    assert(page_->page_type() == PageType::BTREE_LEAF ||
           page_->page_type() == PageType::BTREE_INTERNAL);
}

void Node::init_leaf(Page* page, page_id_t page_id, page_id_t parent_id) {
    assert(page != nullptr);
    page->clear();
    page->set_page_id(page_id);
    page->set_page_type(PageType::BTREE_LEAF);

    NodeHeader h;
    h.num_keys      = 0;
    h.free_end      = SLOT_ARRAY_START;
    h.cell_end      = NODE_DATA_SIZE;
    h.parent_id     = parent_id;
    h.right_sibling = INVALID_PAGE_ID;
    h.left_sibling  = INVALID_PAGE_ID;
    h.first_child   = INVALID_PAGE_ID;  // unused for leaves
    std::memcpy(page->data(), &h, sizeof(h));
}

void Node::init_internal(Page* page, page_id_t page_id, page_id_t parent_id) {
    assert(page != nullptr);
    page->clear();
    page->set_page_id(page_id);
    page->set_page_type(PageType::BTREE_INTERNAL);

    NodeHeader h;
    h.num_keys      = 0;
    h.free_end      = SLOT_ARRAY_START;
    h.cell_end      = NODE_DATA_SIZE;
    h.parent_id     = parent_id;
    h.right_sibling = INVALID_PAGE_ID;  // unused for internal nodes
    h.left_sibling  = INVALID_PAGE_ID;
    h.first_child   = INVALID_PAGE_ID;  // caller sets this after the first split
    std::memcpy(page->data(), &h, sizeof(h));
}

// ── Capacity queries ──────────────────────────────────────────────────────────

size_t Node::free_space() const {
    const NodeHeader& h = hdr();
    assert(h.cell_end >= h.free_end);
    return static_cast<size_t>(h.cell_end - h.free_end);
}

bool Node::is_full(size_t new_cell_bytes) const {
    // We need room for: one new slot entry + the cell payload itself.
    return free_space() < total_cell_cost(new_cell_bytes);
}

bool Node::underflow() const {
    // A node is underflowing if it is less than half full (by byte occupancy).
    // Occupied bytes = NODE_USABLE_SIZE - free_space().
    size_t occupied = NODE_USABLE_SIZE - free_space();
    return occupied < NODE_USABLE_SIZE / 2;
}

// ── Key / value / child access ────────────────────────────────────────────────
//
// Cell wire format (bytes within the cell heap, at slots()[slot]):
//
//  Leaf:
//    Offset 0:                uint16_t key_len
//    Offset 2:                char     key_bytes[key_len]
//    Offset 2+key_len:        uint16_t val_len
//    Offset 4+key_len:        char     val_bytes[val_len]
//
//  Internal:
//    Offset 0:                uint16_t key_len
//    Offset 2:                char     key_bytes[key_len]
//    Offset 2+key_len:        page_id_t right_child   (4 bytes)

std::string_view Node::key_at(uint16_t slot) const {
    assert(slot < hdr().num_keys);
    const char* p = cell_ptr(slot);
    uint16_t klen = 0;
    std::memcpy(&klen, p, sizeof(klen));
    return {p + sizeof(uint16_t), klen};
}

std::string_view Node::value_at(uint16_t slot) const {
    assert(is_leaf());
    assert(slot < hdr().num_keys);
    const char* p = cell_ptr(slot);
    uint16_t klen = 0;
    std::memcpy(&klen, p, sizeof(klen));
    const char* vptr = p + sizeof(uint16_t) + klen;
    uint16_t vlen = 0;
    std::memcpy(&vlen, vptr, sizeof(vlen));
    return {vptr + sizeof(uint16_t), vlen};
}

page_id_t Node::child_at(uint16_t slot) const {
    assert(!is_leaf());
    assert(slot < hdr().num_keys);
    const char* p = cell_ptr(slot);
    uint16_t klen = 0;
    std::memcpy(&klen, p, sizeof(klen));
    page_id_t child = INVALID_PAGE_ID;
    std::memcpy(&child, p + sizeof(uint16_t) + klen, sizeof(child));
    return child;
}

// ── Search ────────────────────────────────────────────────────────────────────

uint16_t Node::lower_bound(std::string_view key) const {
    uint16_t lo = 0, hi = hdr().num_keys;
    while (lo < hi) {
        uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
        if (key_at(mid) < key) {
            lo = static_cast<uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

// ── Slot array helpers ────────────────────────────────────────────────────────

void Node::shift_slots_right(uint16_t from_slot) {
    uint16_t n = hdr().num_keys;
    slot_offset_t* s = slots();
    // Move entries [from_slot .. n-1] one to the right.
    for (uint16_t i = n; i > from_slot; --i) {
        s[i] = s[i - 1];
    }
}

void Node::shift_slots_left(uint16_t from_slot) {
    uint16_t n = hdr().num_keys;
    slot_offset_t* s = slots();
    // Move entries [from_slot+1 .. n-1] one to the left.
    for (uint16_t i = from_slot; i + 1 < n; ++i) {
        s[i] = s[i + 1];
    }
}

// ── Write helpers ─────────────────────────────────────────────────────────────

void Node::write_leaf_cell(uint16_t insert_slot,
                           std::string_view key, std::string_view value) {
    NodeHeader& h = hdr();
    size_t payload = leaf_cell_size(key.size(), value.size());

    // Verify free space (caller should have checked is_full first).
    assert(!is_full(payload));

    // Carve space from the cell heap (grows downward).
    h.cell_end = static_cast<uint16_t>(h.cell_end - payload);
    slot_offset_t cell_off = h.cell_end;

    // Write the cell: key_len, key, val_len, val.
    char* dst = page_->data() + cell_off;
    uint16_t klen = static_cast<uint16_t>(key.size());
    uint16_t vlen = static_cast<uint16_t>(value.size());
    std::memcpy(dst, &klen, sizeof(klen));                dst += sizeof(klen);
    std::memcpy(dst, key.data(), key.size());             dst += key.size();
    std::memcpy(dst, &vlen, sizeof(vlen));                dst += sizeof(vlen);
    std::memcpy(dst, value.data(), value.size());

    // Insert slot at insert_slot, shifting others right.
    shift_slots_right(insert_slot);
    slots()[insert_slot] = cell_off;
    h.free_end = static_cast<uint16_t>(h.free_end + sizeof(slot_offset_t));
    ++h.num_keys;
}

void Node::write_internal_cell(uint16_t insert_slot,
                               std::string_view key, page_id_t right_child) {
    NodeHeader& h = hdr();
    size_t payload = internal_cell_size(key.size());

    assert(!is_full(payload));

    h.cell_end = static_cast<uint16_t>(h.cell_end - payload);
    slot_offset_t cell_off = h.cell_end;

    char* dst = page_->data() + cell_off;
    uint16_t klen = static_cast<uint16_t>(key.size());
    std::memcpy(dst, &klen, sizeof(klen));            dst += sizeof(klen);
    std::memcpy(dst, key.data(), key.size());         dst += key.size();
    std::memcpy(dst, &right_child, sizeof(right_child));

    shift_slots_right(insert_slot);
    slots()[insert_slot] = cell_off;
    h.free_end = static_cast<uint16_t>(h.free_end + sizeof(slot_offset_t));
    ++h.num_keys;
}

// ── Compact ───────────────────────────────────────────────────────────────────

void Node::compact() {
    // Rebuild the cell heap from scratch by copying all live cells into
    // a temporary buffer, then writing them back in order.
    uint16_t n = hdr().num_keys;
    if (n == 0) {
        hdr().cell_end = NODE_DATA_SIZE;
        return;
    }

    // ── Save everything BEFORE page_->clear() ────────────────────────────────
    // clear() zeroes all of raw_data(), which includes the PageHeader fields
    // (page_id, page_type, page_lsn). We must save and restore them explicitly.
    page_id_t saved_pid   = page_->page_id();
    PageType  saved_ptype = page_->page_type();
    NodeHeader saved_hdr  = hdr();
    page_id_t saved_first_child = hdr().first_child;

    // Collect all live cells in slot order.
    std::vector<std::pair<std::string, std::string>> leaf_cells;
    std::vector<std::pair<std::string, page_id_t>>   internal_cells;

    if (is_leaf()) {
        leaf_cells.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            leaf_cells.emplace_back(std::string(key_at(i)),
                                    std::string(value_at(i)));
        }
    } else {
        internal_cells.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            internal_cells.emplace_back(std::string(key_at(i)), child_at(i));
        }
    }

    // ── Zero the page, then restore PageHeader fields ─────────────────────────
    page_->clear();
    page_->set_page_id(saved_pid);    // restore page_id
    page_->set_page_type(saved_ptype); // restore page_type — CRITICAL

    // ── Restore NodeHeader fields with reset counters ─────────────────────────
    NodeHeader& h = hdr();
    h             = saved_hdr;
    h.num_keys    = 0;
    h.free_end    = SLOT_ARRAY_START;
    h.cell_end    = NODE_DATA_SIZE;
    h.first_child = saved_first_child;

    // ── Re-insert all cells in sorted order ───────────────────────────────────
    if (is_leaf()) {
        for (auto& [k, v] : leaf_cells) {
            write_leaf_cell(h.num_keys, k, v);
        }
    } else {
        for (auto& [k, c] : internal_cells) {
            write_internal_cell(h.num_keys, k, c);
        }
    }
}


// ── Mutation — leaf ───────────────────────────────────────────────────────────

bool Node::insert_leaf_cell(std::string_view key, std::string_view value) {
    assert(is_leaf());

    size_t payload = leaf_cell_size(key.size(), value.size());

    // Find sorted insertion position.
    uint16_t slot = lower_bound(key);

    // Handle key already present (overwrite).
    if (slot < hdr().num_keys && key_at(slot) == key) {
        // Check if the value is the same size — if so, update in-place.
        std::string_view old_val = value_at(slot);
        if (old_val.size() == value.size()) {
            // In-place update: value bytes follow key bytes.
            const char* p = cell_ptr(slot);
            uint16_t klen = 0;
            std::memcpy(&klen, p, sizeof(klen));
            char* vptr = page_->data() + slots()[slot]
                         + sizeof(uint16_t) + klen + sizeof(uint16_t);
            std::memcpy(vptr, value.data(), value.size());
            return true;
        }
        // Different size: remove old entry, then re-insert below.
        remove_cell(slot);
        slot = lower_bound(key);  // recompute after removal
        payload = leaf_cell_size(key.size(), value.size());
    }

    // Compact if fragmentation is blocking insertion.
    if (is_full(payload)) {
        compact();
    }

    if (is_full(payload)) {
        return false;  // genuinely no room even after compaction
    }

    write_leaf_cell(slot, key, value);
    return true;
}

// ── Mutation — internal ───────────────────────────────────────────────────────

bool Node::insert_separator(std::string_view key, page_id_t right_child) {
    assert(!is_leaf());

    size_t payload = internal_cell_size(key.size());

    uint16_t slot = lower_bound(key);

    if (is_full(payload)) {
        compact();
    }
    if (is_full(payload)) {
        return false;
    }

    write_internal_cell(slot, key, right_child);
    return true;
}

void Node::update_separator_key(uint16_t slot, std::string_view new_key) {
    assert(!is_leaf());
    assert(slot < hdr().num_keys);
    // The old and new key must be the same length for in-place update.
    // (BTree::steal_from_* must ensure this or rebuild the cell.)
    assert(key_at(slot).size() == new_key.size());
    char* p = page_->data() + slots()[slot];
    uint16_t klen = static_cast<uint16_t>(new_key.size());
    std::memcpy(p, &klen, sizeof(klen));
    std::memcpy(p + sizeof(klen), new_key.data(), new_key.size());
}

void Node::update_child_at(uint16_t slot, page_id_t child_id) {
    assert(!is_leaf());
    assert(slot < hdr().num_keys);
    char* p = page_->data() + slots()[slot];
    uint16_t klen = 0;
    std::memcpy(&klen, p, sizeof(klen));
    std::memcpy(p + sizeof(uint16_t) + klen, &child_id, sizeof(child_id));
}

// ── Remove ────────────────────────────────────────────────────────────────────

void Node::remove_cell(uint16_t slot) {
    assert(slot < hdr().num_keys);
    // We don't reclaim the bytes in the cell heap immediately; they become
    // fragmented free space. compact() reclaims them when needed.
    shift_slots_left(slot);
    --hdr().num_keys;
    hdr().free_end = static_cast<uint16_t>(
        hdr().free_end - sizeof(slot_offset_t));
}

// ── Split ─────────────────────────────────────────────────────────────────────

void Node::split_into(Node& right, std::string& separator_key) {
    uint16_t n = hdr().num_keys;
    assert(n >= 2 && "cannot split a node with fewer than 2 keys");
    assert(right.hdr().num_keys == 0 && "right node must be empty");

    // ── Parent pointer postcondition ──────────────────────────────────────────
    // Both halves share the same parent after the split. btree.cpp will set
    // the correct parent ID on the right node once it knows the parent's page.
    // We set right's parent_id to this->parent_id here as a starting point;
    // btree.cpp will overwrite it if the parent itself splits.
    right.hdr().parent_id = hdr().parent_id;

    if (is_leaf()) {
        // ── Leaf split ────────────────────────────────────────────────────────
        // B+Tree convention: the median key stays in the right half.
        // Copy ALL keys first to compute a fair byte-split midpoint.
        size_t total_used = NODE_USABLE_SIZE - free_space();
        size_t half = total_used / 2;

        // Walk from the left accumulating bytes until we reach or pass half.
        size_t acc = 0;
        uint16_t split_idx = 0;
        for (uint16_t i = 0; i < n; ++i) {
            size_t cell_bytes = leaf_cell_size(key_at(i).size(), value_at(i).size())
                                + sizeof(slot_offset_t);
            if (acc + cell_bytes > half && i > 0) {
                split_idx = i;
                break;
            }
            acc += cell_bytes;
            split_idx = static_cast<uint16_t>(i + 1);
        }

        // Separator key = first key of right half (copied to parent, NOT promoted).
        separator_key = std::string(key_at(split_idx));

        // Copy [split_idx .. n-1] to right.
        for (uint16_t i = split_idx; i < n; ++i) {
            bool ok = right.insert_leaf_cell(key_at(i), value_at(i));
            assert(ok && "right node should have room during split");
        }

        // Remove [split_idx .. n-1] from this node.
        // Remove in reverse order to keep slot indices valid.
        for (int i = n - 1; i >= split_idx; --i) {
            remove_cell(static_cast<uint16_t>(i));
        }

        // Update leaf linked list pointers.
        right.hdr().right_sibling = hdr().right_sibling;
        right.hdr().left_sibling  = page_id();
        if (hdr().right_sibling != INVALID_PAGE_ID) {
            // The old right sibling will have its left_sibling updated by btree.cpp
            // after it fetches that page (we don't have bp here).
        }
        hdr().right_sibling = right.page_id();

    } else {
        // ── Internal split ────────────────────────────────────────────────────
        // The median key is PROMOTED (not kept in either half).
        // Left half: [0 .. mid-1],  promoted key: key_at(mid),
        // Right half: [mid+1 .. n-1].

        size_t total_used = NODE_USABLE_SIZE - free_space();
        size_t half = total_used / 2;

        size_t acc = 0;
        uint16_t mid = 0;
        for (uint16_t i = 0; i < n; ++i) {
            size_t cell_bytes = internal_cell_size(key_at(i).size())
                                + sizeof(slot_offset_t);
            if (acc + cell_bytes > half && i > 0) {
                mid = i;
                break;
            }
            acc += cell_bytes;
            mid = static_cast<uint16_t>(i + 1);
        }
        // Clamp: mid must be strictly interior.
        if (mid == 0)   mid = 1;
        if (mid >= n-1) mid = static_cast<uint16_t>(n - 1);

        // Save the promoted key before we mutate.
        separator_key = std::string(key_at(mid));

        // The right half's leftmost child = right child of the promoted key.
        right.hdr().first_child = child_at(mid);

        // Copy [mid+1 .. n-1] to right.
        for (uint16_t i = static_cast<uint16_t>(mid + 1); i < n; ++i) {
            bool ok = right.insert_separator(key_at(i), child_at(i));
            assert(ok && "right node should have room during split");
        }

        // Remove [mid .. n-1] from this node (mid is promoted, so also removed).
        for (int i = n - 1; i >= mid; --i) {
            remove_cell(static_cast<uint16_t>(i));
        }
    }
}

// ── Merge ─────────────────────────────────────────────────────────────────────

void Node::merge_from(Node& right, std::string_view separator_key,
                      BufferPool& bp) {
    if (is_leaf()) {
        // ── Leaf merge ────────────────────────────────────────────────────────
        // Copy all of right's cells into this node.
        uint16_t rn = right.hdr().num_keys;
        for (uint16_t i = 0; i < rn; ++i) {
            bool ok = insert_leaf_cell(right.key_at(i), right.value_at(i));
            assert(ok && "merged node should have room");
        }

        // Stitch the linked list.
        hdr().right_sibling = right.hdr().right_sibling;
        // The caller (btree.cpp) must update right's right sibling's left_sibling
        // pointer after this merge, since we don't have access to that page here.

    } else {
        // ── Internal merge ────────────────────────────────────────────────────
        // Pull the parent separator down as the median key, then absorb right.
        // Right's first child becomes the right-child of the pulled-down key.

        // Insert the separator with right.hdr().first_child as its right child.
        bool ok = insert_separator(separator_key, right.hdr().first_child);
        assert(ok && "merged internal node should have room for separator");

        uint16_t rn = right.hdr().num_keys;
        for (uint16_t i = 0; i < rn; ++i) {
            ok = insert_separator(right.key_at(i), right.child_at(i));
            assert(ok && "merged internal node should have room");
        }

        // ── Parent pointer postcondition ──────────────────────────────────────
        // All children that used to point to `right` must now point to `this`.
        // Children of an internal node = first_child + child_at(0..n-1).
        // We must fetch each child, update parent_id, and unpin dirty.

        // Update parent_id on right's first_child.
        auto update_parent = [&](page_id_t child_pid) {
            if (child_pid == INVALID_PAGE_ID) return;
            Page* cp = bp.fetch_page(child_pid);
            assert(cp != nullptr);
            Node child(cp);
            child.hdr().parent_id = page_id();
            bp.unpin_page(child_pid, /*dirty=*/true);
            // dirty=true: we changed parent_id in the child header.
        };

        update_parent(right.hdr().first_child);
        for (uint16_t i = 0; i < rn; ++i) {
            update_parent(right.child_at(i));
        }
        // Also update the right child of the pulled-down separator.
        update_parent(right.hdr().first_child);  // already done above; harmless.
    }
}

// ── Steal from left ───────────────────────────────────────────────────────────

void Node::steal_from_left(Node& left, std::string_view /* old_sep */,
                            std::string& new_sep, BufferPool& bp) {
    uint16_t last = static_cast<uint16_t>(left.hdr().num_keys - 1);

    if (is_leaf()) {
        // Move left's rightmost key-value into this node at position 0.
        std::string k(left.key_at(last));
        std::string v(left.value_at(last));
        left.remove_cell(last);

        // Insert at front of this node.
        bool ok = insert_leaf_cell(k, v);
        assert(ok);

        // New parent separator = the key we just moved (it is now this node's
        // first key, which is the correct separator between left and this).
        new_sep = k;

    } else {
        // Internal steal: the leftmost child of `this` gains the pulled-down
        // separator; left's rightmost key is promoted to parent.

        std::string stolen_key(left.key_at(last));
        page_id_t stolen_child = left.child_at(last);  // right child of stolen key
        left.remove_cell(last);

        // Insert old_sep with first_child as its right child at position 0 of this.
        // Actually: old separator goes in front of this node's cells,
        //           and hdr().first_child becomes stolen_child.
        // We'll do it as: push this node's first_child right, insert at front.
        //   new first_child = stolen_child (left's old rightmost child)
        //   new cells[0]    = (old_sep, old first_child)   pushed right
        //   rest unchanged.

        // Save the old first_child.
        page_id_t old_fc = hdr().first_child;
        hdr().first_child = stolen_child;

        // Insert old_sep at slot 0 with right_child = old_fc.
        // We don't use insert_separator because it uses lower_bound — just
        // write directly at position 0 by shifting slots right.
        size_t payload = internal_cell_size(stolen_key.size());
        if (is_full(payload)) compact();
        assert(!is_full(payload));
        hdr().cell_end = static_cast<uint16_t>(hdr().cell_end - payload);
        slot_offset_t cell_off = hdr().cell_end;
        char* dst = page_->data() + cell_off;
        uint16_t klen = static_cast<uint16_t>(stolen_key.size());
        std::memcpy(dst, &klen, sizeof(klen));                     dst += sizeof(klen);
        std::memcpy(dst, stolen_key.data(), stolen_key.size());    dst += stolen_key.size();
        std::memcpy(dst, &old_fc, sizeof(old_fc));
        shift_slots_right(0);
        slots()[0] = cell_off;
        hdr().free_end = static_cast<uint16_t>(hdr().free_end + sizeof(slot_offset_t));
        ++hdr().num_keys;

        // ── Parent pointer: stolen_child's parent_id must now point to this ──
        if (stolen_child != INVALID_PAGE_ID) {
            Page* cp = bp.fetch_page(stolen_child);
            assert(cp != nullptr);
            Node child(cp);
            child.hdr().parent_id = page_id();
            bp.unpin_page(stolen_child, /*dirty=*/true);
            // dirty=true: changed parent_id.
        }

        new_sep = stolen_key;
    }
}

// ── Steal from right ──────────────────────────────────────────────────────────

void Node::steal_from_right(Node& right, std::string_view /* old_sep */,
                             std::string& new_sep, BufferPool& bp) {
    if (is_leaf()) {
        // Move right's leftmost key-value to the end of this node.
        std::string k(right.key_at(0));
        std::string v(right.value_at(0));
        right.remove_cell(0);

        bool ok = insert_leaf_cell(k, v);
        assert(ok);

        // New separator = right's new first key.
        new_sep = std::string(right.key_at(0));

    } else {
        // Internal steal: pull right's leftmost key-value into this node.
        std::string stolen_key(right.key_at(0));
        page_id_t stolen_child = right.hdr().first_child;

        // Advance right's first_child to the right child of the stolen key.
        right.hdr().first_child = right.child_at(0);
        right.remove_cell(0);

        // Append (old_sep, stolen_child) to this node.
        // stolen_child becomes the right child of the new last separator.
        bool ok = insert_separator(stolen_key, stolen_child);
        assert(ok);

        // ── Parent pointer: stolen_child's parent_id must now point to this ──
        if (stolen_child != INVALID_PAGE_ID) {
            Page* cp = bp.fetch_page(stolen_child);
            assert(cp != nullptr);
            Node child(cp);
            child.hdr().parent_id = page_id();
            bp.unpin_page(stolen_child, /*dirty=*/true);
            // dirty=true: changed parent_id.
        }

        new_sep = stolen_key;
    }
}

} // namespace keyvdb
