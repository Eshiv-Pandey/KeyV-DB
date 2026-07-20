#include "btree/btree.h"
#include "btree/node.h"

#include <cassert>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// btree.cpp — B+Tree over KeyVDB's BufferPool.
//
// ── Dirty-flag discipline (rule, applies to every unpin_page call) ──────────
//   Every page obtained via fetch_page or new_page must be unpinned exactly
//   once. dirty=true if and only if the page's bytes were modified. Every call
//   to bp_.unpin_page below has an inline comment stating the reason.
//
// ── Pin discipline during traversal (rule) ──────────────────────────────────
//   find_leaf() unpins each internal node immediately after reading the child
//   pointer needed to descend to the next level. Only the final leaf page is
//   returned still pinned. Breadcrumbs store page_id_t (re-fetchable IDs).
//
// ── Parent pointer invariant (rule) ─────────────────────────────────────────
//   Every mutating operation updates parent_id on every page whose parent
//   changed. See Node::split_into, merge_from, steal_from_* in node.cpp,
//   and create_new_root / insert_into_parent below.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

// ── Construction / factory ────────────────────────────────────────────────────

BTree::BTree(BufferPool& bp, FreeList& fl, page_id_t root)
    : bp_(bp), fl_(fl), root_page_id_(root)
{}

BTree BTree::create(BufferPool& bp, FreeList& fl) {
    page_id_t root_id;
    Page* root_page = bp.new_page(root_id);
    if (!root_page) {
        throw std::runtime_error("BTree::create: buffer pool exhausted");
    }

    // Initialise as an empty leaf.
    Node::init_leaf(root_page, root_id, /*parent_id=*/INVALID_PAGE_ID);

    bp.unpin_page(root_id, /*dirty=*/true);
    // dirty=true: we just initialised the root leaf's NodeHeader.

    BTree tree(bp, fl, root_id);
    tree.persist_root();  // write root_id to page 0's tail slot
    return tree;
}

BTree BTree::open(BufferPool& bp, FreeList& fl, page_id_t root_page_id) {
    return BTree(bp, fl, root_page_id);
}

void BTree::persist_root() {
    fl_.set_root_page_id(root_page_id_);
}

// ── Traversal ─────────────────────────────────────────────────────────────────

Page* BTree::find_leaf(std::string_view key,
                       std::vector<page_id_t>& breadcrumbs) {
    page_id_t cur = root_page_id_;

    while (true) {
        Page* page = bp_.fetch_page(cur);
        assert(page != nullptr && "find_leaf: buffer pool exhausted or bad page_id");

        if (page->page_type() == PageType::BTREE_LEAF) {
            // ── Pin discipline: leaf is returned still pinned ─────────────────
            // Caller is responsible for unpinning this leaf.
            return page;
        }

        // ── Internal node: read child pointer, then unpin immediately ─────────
        // Pin discipline rule: each internal node is unpinned before descending.
        // Breadcrumbs record the page_id (re-fetchable ID), not a live pin.
        Node node(page);
        uint16_t ub = node.lower_bound(key);

        // Advance past any exact separator match to get upper_bound.
        while (ub < node.num_keys() && node.key_at(ub) == key) {
            ++ub;
        }
        page_id_t next_child;
        if (ub == 0) {
            next_child = node.hdr().first_child;
        } else {
            next_child = node.child_at(static_cast<uint16_t>(ub - 1));
        }


        breadcrumbs.push_back(cur);

        bp_.unpin_page(cur, /*dirty=*/false);
        // dirty=false: read-only traversal, bytes unchanged.

        cur = next_child;
    }
}

// ── Get ────────────────────────────────────────────────────────────────────────

std::optional<std::string> BTree::get(std::string_view key) {
    std::vector<page_id_t> breadcrumbs;
    Page* leaf_page = find_leaf(key, breadcrumbs);
    page_id_t leaf_id = leaf_page->page_id();

    Node leaf(leaf_page);
    uint16_t slot = leaf.lower_bound(key);
    std::optional<std::string> result;

    if (slot < leaf.num_keys() && leaf.key_at(slot) == key) {
        result = std::string(leaf.value_at(slot));
    }

    bp_.unpin_page(leaf_id, /*dirty=*/false);
    // dirty=false: get() is a read-only operation.

    return result;
}

// ── Insert ────────────────────────────────────────────────────────────────────

void BTree::insert(std::string_view key, std::string_view value) {
    size_t payload = leaf_cell_size(key.size(), value.size());
    if (payload + sizeof(slot_offset_t) > NODE_USABLE_SIZE) {
        throw std::invalid_argument(
            "BTree::insert: key+value too large to fit in one page ("
            + std::to_string(payload) + " bytes, max "
            + std::to_string(NODE_USABLE_SIZE) + ")");
    }

    std::vector<page_id_t> breadcrumbs;
    Page* leaf_page = find_leaf(key, breadcrumbs);
    page_id_t leaf_id = leaf_page->page_id();
    Node leaf(leaf_page);

    // ── Attempt insert directly ───────────────────────────────────────────────
    if (leaf.insert_leaf_cell(key, value)) {
        bp_.unpin_page(leaf_id, /*dirty=*/true);
        // dirty=true: insert_leaf_cell modified the leaf's bytes.
        return;
    }

    // ── Leaf is full — split, then retry ─────────────────────────────────────
    page_id_t right_id;
    Page* right_page = bp_.new_page(right_id);
    if (!right_page) {
        bp_.unpin_page(leaf_id, /*dirty=*/false);
        // dirty=false: we didn't successfully modify the leaf yet.
        throw std::runtime_error("BTree::insert: buffer pool exhausted (split)");
    }
    Node::init_leaf(right_page, right_id, leaf.hdr().parent_id);
    Node right(right_page);

    // ── Update sibling's left pointer BEFORE split (we need old right sibling)
    page_id_t old_right_sib = leaf.hdr().right_sibling;

    std::string sep_key;
    leaf.split_into(right, sep_key);
    // After split_into:
    //   right.hdr().parent_id = leaf.hdr().parent_id (set by split_into).
    //   leaf.hdr().right_sibling = right_id (set by split_into).
    //   right.hdr().left_sibling = leaf_id (set by split_into).

    // If there was a right sibling, update its left_sibling to point to right.
    if (old_right_sib != INVALID_PAGE_ID) {
        Page* ors = bp_.fetch_page(old_right_sib);
        assert(ors != nullptr);
        Node ors_node(ors);
        ors_node.hdr().left_sibling = right_id;
        bp_.unpin_page(old_right_sib, /*dirty=*/true);
        // dirty=true: we updated left_sibling in the old right sibling.
    }

    // Decide which half gets the new key.
    if (key < sep_key) {
        leaf.insert_leaf_cell(key, value);
    } else {
        right.insert_leaf_cell(key, value);
    }

    bp_.unpin_page(leaf_id, /*dirty=*/true);
    // dirty=true: leaf was split (cells removed) and possibly got the new key.

    bp_.unpin_page(right_id, /*dirty=*/true);
    // dirty=true: right is a new node that was freshly written.

    insert_into_parent(leaf_id, sep_key, right_id, breadcrumbs);
}

// ── Insert into parent ────────────────────────────────────────────────────────

void BTree::insert_into_parent(page_id_t left_id,
                               const std::string& sep_key,
                               page_id_t right_id,
                               std::vector<page_id_t>& breadcrumbs) {
    if (breadcrumbs.empty()) {
        // The node that split was the root — create a new root.
        create_new_root(left_id, sep_key, right_id);
        return;
    }

    page_id_t parent_id = breadcrumbs.back();
    breadcrumbs.pop_back();

    Page* parent_page = bp_.fetch_page(parent_id);
    assert(parent_page != nullptr);
    Node parent(parent_page);

    size_t payload = internal_cell_size(sep_key.size());

    if (!parent.is_full(payload)) {
        // ── Room in parent: insert separator directly ─────────────────────────
        bool ok = parent.insert_separator(sep_key, right_id);
        assert(ok);

        // ── Parent pointer: right_id's parent_id must be parent_id ───────────
        Page* rp = bp_.fetch_page(right_id);
        assert(rp != nullptr);
        Node(rp).hdr().parent_id = parent_id;
        bp_.unpin_page(right_id, /*dirty=*/true);
        // dirty=true: updated parent_id in right node's header.

        bp_.unpin_page(parent_id, /*dirty=*/true);
        // dirty=true: we inserted a separator into the parent.

    } else {
        // ── Parent is full: split the parent too ──────────────────────────────
        // Insert the separator into the parent, then split it.

        // First compact to maximise room.
        parent.compact();

        // Try again after compaction.
        if (!parent.is_full(payload)) {
            bool ok = parent.insert_separator(sep_key, right_id);
            assert(ok);

            Page* rp = bp_.fetch_page(right_id);
            assert(rp != nullptr);
            Node(rp).hdr().parent_id = parent_id;
            bp_.unpin_page(right_id, /*dirty=*/true);
            // dirty=true: updated parent_id.

            bp_.unpin_page(parent_id, /*dirty=*/true);
            // dirty=true: compact + separator insert modified the parent.
            return;
        }

        // Still full: allocate right sibling for parent, split, propagate.
        page_id_t parent_right_id;
        Page* parent_right_page = bp_.new_page(parent_right_id);
        assert(parent_right_page != nullptr && "pool exhausted during parent split");
        Node::init_internal(parent_right_page, parent_right_id,
                            parent.hdr().parent_id);
        Node parent_right(parent_right_page);

        // Insert separator into the about-to-be-split parent first,
        // then split. We need to insert first so the split has all cells.
        // But parent is full — we need a temp path.
        //
        // Strategy: insert into the parent as if it had room (by temporarily
        // "borrowing" a slot), split evenly, then the new key ends up in one
        // of the halves. We approximate this by:
        //   1. Find which half the new key would go into.
        //   2. Split first.
        //   3. Insert into the correct half.
        //
        std::string parent_sep;
        parent.split_into(parent_right, parent_sep);
        // parent_right.hdr().parent_id = parent.hdr().parent_id (from split_into).

        // Insert sep_key into the correct half.
        if (sep_key < parent_sep) {
            // Goes into left (parent).
            bool ok = parent.insert_separator(sep_key, right_id);
            assert(ok && "left half should have room after split");
            // Update right_id's parent.
            Page* rp = bp_.fetch_page(right_id);
            Node(rp).hdr().parent_id = parent_id;
            bp_.unpin_page(right_id, /*dirty=*/true);
            // dirty=true: updated parent_id.
        } else {
            // Goes into right (parent_right).
            bool ok = parent_right.insert_separator(sep_key, right_id);
            assert(ok && "right half should have room after split");
            Page* rp = bp_.fetch_page(right_id);
            Node(rp).hdr().parent_id = parent_right_id;
            bp_.unpin_page(right_id, /*dirty=*/true);
            // dirty=true: updated parent_id to the new right parent.
        }

        // Update parent_id on all of parent_right's children (they were in
        // parent before the split).
        {
            uint16_t prn = parent_right.hdr().num_keys;
            auto reparent = [&](page_id_t child_pid) {
                if (child_pid == INVALID_PAGE_ID) return;
                Page* cp = bp_.fetch_page(child_pid);
                assert(cp != nullptr);
                Node(cp).hdr().parent_id = parent_right_id;
                bp_.unpin_page(child_pid, /*dirty=*/true);
                // dirty=true: changed parent_id.
            };
            reparent(parent_right.hdr().first_child);
            for (uint16_t i = 0; i < prn; ++i) {
                reparent(parent_right.child_at(i));
            }
        }

        bp_.unpin_page(parent_id, /*dirty=*/true);
        // dirty=true: parent was split (cells redistributed).

        bp_.unpin_page(parent_right_id, /*dirty=*/true);
        // dirty=true: parent_right is a freshly written internal node.

        // Propagate the split upward.
        insert_into_parent(parent_id, parent_sep, parent_right_id, breadcrumbs);
    }
}

// ── Create new root ───────────────────────────────────────────────────────────

void BTree::create_new_root(page_id_t left_id,
                            const std::string& sep_key,
                            page_id_t right_id) {
    page_id_t new_root_id;
    Page* new_root_page = bp_.new_page(new_root_id);
    assert(new_root_page != nullptr && "pool exhausted during root creation");

    Node::init_internal(new_root_page, new_root_id, /*parent_id=*/INVALID_PAGE_ID);
    Node new_root(new_root_page);
    new_root.hdr().first_child = left_id;

    bool ok = new_root.insert_separator(sep_key, right_id);
    assert(ok);

    bp_.unpin_page(new_root_id, /*dirty=*/true);
    // dirty=true: we initialised and populated the new root.

    // ── Parent pointer postcondition ──────────────────────────────────────────
    // Both children must now point to new_root_id as their parent.
    {
        Page* lp = bp_.fetch_page(left_id);
        assert(lp != nullptr);
        Node(lp).hdr().parent_id = new_root_id;
        bp_.unpin_page(left_id, /*dirty=*/true);
        // dirty=true: updated parent_id on left child.
    }
    {
        Page* rp = bp_.fetch_page(right_id);
        assert(rp != nullptr);
        Node(rp).hdr().parent_id = new_root_id;
        bp_.unpin_page(right_id, /*dirty=*/true);
        // dirty=true: updated parent_id on right child.
    }

    root_page_id_ = new_root_id;
    persist_root();
}

// ── Remove ────────────────────────────────────────────────────────────────────

bool BTree::remove(std::string_view key) {
    std::vector<page_id_t> breadcrumbs;
    Page* leaf_page = find_leaf(key, breadcrumbs);
    page_id_t leaf_id = leaf_page->page_id();
    Node leaf(leaf_page);

    uint16_t slot = leaf.lower_bound(key);
    if (slot >= leaf.num_keys() || leaf.key_at(slot) != key) {
        bp_.unpin_page(leaf_id, /*dirty=*/false);
        // dirty=false: key not found, nothing modified.
        return false;
    }

    leaf.remove_cell(slot);

    bool needs_fix = (leaf.underflow() && leaf_id != root_page_id_);
    bp_.unpin_page(leaf_id, /*dirty=*/true);
    // dirty=true: we removed a cell from the leaf.

    if (needs_fix) {
        fix_underflow(leaf_id, breadcrumbs);
    }

    return true;
}

// ── Fix underflow ─────────────────────────────────────────────────────────────

void BTree::fix_underflow(page_id_t page_id,
                          std::vector<page_id_t>& breadcrumbs) {
    if (breadcrumbs.empty()) {
        // This node is the root. An underflowing root is fine — it can stay
        // as-is until it hits 0 keys (leaf root) or 0 children (internal root
        // with one child, in which case we can collapse the tree by one level).
        Page* rp = bp_.fetch_page(page_id);
        assert(rp != nullptr);
        if (rp->page_type() == PageType::BTREE_INTERNAL) {
            Node root_node(rp);
            if (root_node.num_keys() == 0) {
                // Collapse: the root's only remaining child becomes the new root.
                page_id_t new_root = root_node.hdr().first_child;
                bp_.unpin_page(page_id, /*dirty=*/false);
                // dirty=false: we only read the first_child pointer.

                if (new_root != INVALID_PAGE_ID) {
                    // Update the new root's parent_id to INVALID (it IS the root).
                    Page* nrp = bp_.fetch_page(new_root);
                    assert(nrp != nullptr);
                    Node(nrp).hdr().parent_id = INVALID_PAGE_ID;
                    bp_.unpin_page(new_root, /*dirty=*/true);
                    // dirty=true: cleared parent_id on the new root.

                    bp_.delete_page(page_id);  // free the old root frame
                    root_page_id_ = new_root;
                    persist_root();
                }
                return;
            }
        }
        bp_.unpin_page(page_id, /*dirty=*/false);
        // dirty=false: underflowing root with keys — acceptable, leave as-is.
        return;
    }

    page_id_t parent_id = breadcrumbs.back();
    breadcrumbs.pop_back();

    Page* parent_page = bp_.fetch_page(parent_id);
    assert(parent_page != nullptr);
    Node parent(parent_page);

    // ── Find separator slot in parent for `page_id` ───────────────────────────
    // page_id is either first_child or child_at(k) for some k.
    // The separator for page_id is:
    //   if page_id == first_child:  separator is key_at(0), right sib is child_at(0)
    //   if page_id == child_at(k):  separator is key_at(k), left sib is first_child or child_at(k-1)
    //
    // We scan to find which position page_id occupies.

    int sep_slot = -1;           // index of the separator key between left_sib and page_id
    page_id_t left_sib  = INVALID_PAGE_ID;
    page_id_t right_sib = INVALID_PAGE_ID;

    if (parent.hdr().first_child == page_id) {
        // page_id is the leftmost child; only a right sibling exists.
        sep_slot  = 0;
        right_sib = parent.child_at(0);
        left_sib  = INVALID_PAGE_ID;
    } else {
        for (uint16_t i = 0; i < parent.num_keys(); ++i) {
            if (parent.child_at(i) == page_id) {
                sep_slot = static_cast<int>(i);
                right_sib = (i + 1 < parent.num_keys())
                            ? parent.child_at(static_cast<uint16_t>(i + 1))
                            : INVALID_PAGE_ID;
                left_sib  = (i == 0)
                            ? parent.hdr().first_child
                            : parent.child_at(static_cast<uint16_t>(i - 1));
                break;
            }
        }
    }

    assert(sep_slot >= 0 && "fix_underflow: page_id not found in parent");

    // ── Try to borrow from left sibling ───────────────────────────────────────
    if (left_sib != INVALID_PAGE_ID) {
        Page* lp = bp_.fetch_page(left_sib);
        assert(lp != nullptr);
        Node left_node(lp);

        if (!left_node.underflow()) {
            // Left has a spare cell — steal from it.
            Page* cur_page = bp_.fetch_page(page_id);
            assert(cur_page != nullptr);
            Node cur_node(cur_page);

            std::string_view old_sep = parent.key_at(
                static_cast<uint16_t>(sep_slot > 0 ? sep_slot - 1 : 0));
            // Correct: the separator between left_sib and page_id is key_at(sep_slot-1)
            // when page_id == child_at(sep_slot), or key_at(0) when page_id is
            // first_child's right neighbor.
            // Let's recompute:
            std::string_view separator_key;
            if (parent.hdr().first_child == page_id) {
                separator_key = parent.key_at(0);
            } else {
                // Find left_sib in parent to get its separator.
                separator_key = parent.key_at(static_cast<uint16_t>(sep_slot));
            }
            (void)old_sep;

            std::string new_sep;
            cur_node.steal_from_left(left_node, separator_key, new_sep, bp_);

            // Update the separator in parent with new_sep.
            uint16_t sep_key_slot = (parent.hdr().first_child == page_id)
                                    ? 0
                                    : static_cast<uint16_t>(sep_slot);
            // For in-place update the key must be the same size.
            // If sizes differ, we remove+reinsert.
            if (parent.key_at(sep_key_slot).size() == new_sep.size()) {
                parent.update_separator_key(sep_key_slot, new_sep);
            } else {
                page_id_t right_child = parent.child_at(sep_key_slot);
                parent.remove_cell(sep_key_slot);
                bool ok = parent.insert_separator(new_sep, right_child);
                assert(ok);
            }

            bp_.unpin_page(page_id, /*dirty=*/true);
            // dirty=true: steal_from_left modified cur_node.

            bp_.unpin_page(left_sib, /*dirty=*/true);
            // dirty=true: steal_from_left removed a cell from left_node.

            bp_.unpin_page(parent_id, /*dirty=*/true);
            // dirty=true: updated separator key in parent.

            return;
        }
        bp_.unpin_page(left_sib, /*dirty=*/false);
        // dirty=false: only read left_sib to check for spare cells.
    }

    // ── Try to borrow from right sibling ──────────────────────────────────────
    if (right_sib != INVALID_PAGE_ID) {
        Page* rp = bp_.fetch_page(right_sib);
        assert(rp != nullptr);
        Node right_node(rp);

        if (!right_node.underflow()) {
            Page* cur_page = bp_.fetch_page(page_id);
            assert(cur_page != nullptr);
            Node cur_node(cur_page);

            uint16_t sep_key_slot = static_cast<uint16_t>(sep_slot);
            std::string_view separator_key = parent.key_at(sep_key_slot);
            std::string new_sep;
            cur_node.steal_from_right(right_node, separator_key, new_sep, bp_);

            if (parent.key_at(sep_key_slot).size() == new_sep.size()) {
                parent.update_separator_key(sep_key_slot, new_sep);
            } else {
                page_id_t rc = parent.child_at(sep_key_slot);
                parent.remove_cell(sep_key_slot);
                bool ok = parent.insert_separator(new_sep, rc);
                assert(ok);
            }

            bp_.unpin_page(page_id, /*dirty=*/true);
            // dirty=true: steal_from_right modified cur_node.

            bp_.unpin_page(right_sib, /*dirty=*/true);
            // dirty=true: steal_from_right removed a cell from right_node.

            bp_.unpin_page(parent_id, /*dirty=*/true);
            // dirty=true: updated separator key in parent.

            return;
        }
        bp_.unpin_page(right_sib, /*dirty=*/false);
        // dirty=false: only read right_sib to check for spare cells.
    }

    // ── Merge with a sibling ──────────────────────────────────────────────────
    // Prefer merging with the right sibling; fall back to left.

    if (right_sib != INVALID_PAGE_ID) {
        // Merge page_id (left) with right_sib.
        Page* lp = bp_.fetch_page(page_id);
        Page* rp = bp_.fetch_page(right_sib);
        assert(lp && rp);
        Node left_node(lp);
        Node right_node(rp);

        uint16_t sep_key_slot = static_cast<uint16_t>(sep_slot);
        std::string separator_key_str(parent.key_at(sep_key_slot));

        left_node.merge_from(right_node, separator_key_str, bp_);
        // merge_from updates parent_id on right's children (for internal nodes).

        // Stitch the leaf linked list (leaf merge updates right_sibling in merge_from;
        // here we fix the right's right sibling's left pointer if needed).
        if (left_node.is_leaf() && left_node.hdr().right_sibling != INVALID_PAGE_ID) {
            Page* rrs = bp_.fetch_page(left_node.hdr().right_sibling);
            assert(rrs != nullptr);
            Node(rrs).hdr().left_sibling = page_id;
            bp_.unpin_page(left_node.hdr().right_sibling, /*dirty=*/true);
            // dirty=true: updated left_sibling in right's right neighbor.
        }

        // Remove the separator from the parent and fix child pointer.
        // The right sibling is gone; the separator between left and right is removed.
        // If page_id is first_child, remove key_at(0) and set first_child = page_id.
        // Otherwise remove key_at(sep_key_slot) and child_at(sep_key_slot) points
        // to right_sib (now gone).
        parent.remove_cell(sep_key_slot);
        // child_at(sep_key_slot) was right_sib — it's been removed together with its
        // slot. The left child (page_id) stays in its position (either first_child
        // or child_at(sep_key_slot - 1)).

        bp_.unpin_page(page_id, /*dirty=*/true);
        // dirty=true: merge_from added right's cells to left.

        bp_.unpin_page(right_sib, /*dirty=*/true);
        // dirty=true: mark dirty before delete_page so any in-pool frame
        // for right_sib reflects its cleared/merged state before eviction.
        // delete_page will then remove it from the pool and free its ID.
        bp_.delete_page(right_sib);

        bool parent_underflows = (parent.underflow() && parent_id != root_page_id_);
        bp_.unpin_page(parent_id, /*dirty=*/true);
        // dirty=true: removed a separator from the parent.

        if (parent_underflows) {
            fix_underflow(parent_id, breadcrumbs);
        } else if (parent.num_keys() == 0 && parent_id == root_page_id_) {
            // Root is now empty internal node — collapse the tree by one level.
            // page_id becomes the new root.
            // Fetch page_id to update its parent_id.
            Page* np = bp_.fetch_page(page_id);
            assert(np != nullptr);
            Node(np).hdr().parent_id = INVALID_PAGE_ID;
            bp_.unpin_page(page_id, /*dirty=*/true);
            // dirty=true: cleared parent_id on new root.

            bp_.delete_page(parent_id);
            root_page_id_ = page_id;
            persist_root();
        }

    } else if (left_sib != INVALID_PAGE_ID) {
        // Merge left_sib (left) + page_id (right): absorb page_id into left_sib.
        Page* lp = bp_.fetch_page(left_sib);
        Page* rp = bp_.fetch_page(page_id);
        assert(lp && rp);
        Node left_node(lp);
        Node right_node(rp);

        // Separator between left_sib and page_id:
        // page_id == child_at(sep_slot), so separator == key_at(sep_slot - 1)
        // ... actually when left_sib is found, sep_slot is the slot of page_id.
        // The separator is at key_at(sep_slot - 1) if page_id == child_at(sep_slot)
        // and sep_slot > 0, or key_at(0) if sep_slot == 0 (left_sib == first_child).
        // This branch means left_sib != INVALID and right_sib == INVALID,
        // meaning page_id is the rightmost child. The separator is key_at(sep_slot).
        // Actually let's find it cleanly:
        uint16_t sep_key_slot;
        if (parent.hdr().first_child == left_sib) {
            // left_sib is first_child, page_id must be child_at(0), sep is key_at(0).
            sep_key_slot = 0;
        } else {
            // Both are in the child_at array; find left_sib's index.
            sep_key_slot = 0;
            for (uint16_t i = 0; i < parent.num_keys(); ++i) {
                if (parent.child_at(i) == page_id) {
                    sep_key_slot = i;
                    break;
                }
            }
        }
        std::string separator_key_str(parent.key_at(sep_key_slot));

        left_node.merge_from(right_node, separator_key_str, bp_);

        // Fix leaf linked list right side.
        if (left_node.is_leaf() && left_node.hdr().right_sibling != INVALID_PAGE_ID) {
            Page* rrs = bp_.fetch_page(left_node.hdr().right_sibling);
            assert(rrs != nullptr);
            Node(rrs).hdr().left_sibling = left_sib;
            bp_.unpin_page(left_node.hdr().right_sibling, /*dirty=*/true);
            // dirty=true: updated left_sibling.
        }

        parent.remove_cell(sep_key_slot);

        bp_.unpin_page(left_sib, /*dirty=*/true);
        // dirty=true: left absorbed right's cells.

        bp_.unpin_page(page_id, /*dirty=*/true);
        // dirty=true: mark dirty before deleting (cells conceptually cleared).

        bp_.delete_page(page_id);

        bool parent_underflows = (parent.underflow() && parent_id != root_page_id_);
        bp_.unpin_page(parent_id, /*dirty=*/true);
        // dirty=true: removed a separator.

        if (parent_underflows) {
            fix_underflow(parent_id, breadcrumbs);
        } else if (parent.num_keys() == 0 && parent_id == root_page_id_) {
            Page* np = bp_.fetch_page(left_sib);
            assert(np != nullptr);
            Node(np).hdr().parent_id = INVALID_PAGE_ID;
            bp_.unpin_page(left_sib, /*dirty=*/true);
            // dirty=true: cleared parent_id.

            bp_.delete_page(parent_id);
            root_page_id_ = left_sib;
            persist_root();
        }
    } else {
        // No siblings — this should only happen if the node is the root.
        bp_.unpin_page(parent_id, /*dirty=*/false);
        // dirty=false: no change made.
    }
}

} // namespace keyvdb
