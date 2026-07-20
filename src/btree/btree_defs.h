#pragma once

#include <cstdint>
#include <cstring>
#include "storage/page.h"

// ─────────────────────────────────────────────────────────────────────────────
// btree_defs.h — On-disk layout for B+Tree nodes in KeyVDB.
//
// Every B+Tree page uses the standard Page layout:
//   [PageHeader  — 24 bytes ]   (page_id, page_type, page_lsn — in raw_data())
//   [B+Tree data — 4072 bytes]  (everything below is within page.data())
//
// Within the 4072-byte data area:
//
//   [NodeHeader — NODE_HEADER_SIZE bytes ]  at offset 0
//   [Slot array — grows DOWNWARD         ]  from offset NODE_HEADER_SIZE
//   [Free space — in the middle          ]
//   [Cell heap  — grows UPWARD from bottom]
//
// Each slot is a uint16_t holding the byte offset within the data area where
// the corresponding cell begins. Slots are stored in key-sorted order; the
// cell heap has no ordering requirement (cells land wherever there is room).
//
// BTREE_ORDER: a rough estimate of how many fixed-size (16-byte) keys fit in
// a node. It is NOT used as a split trigger. is_full(new_cell_bytes) uses a
// free-space check instead, because variable-length keys make key-count an
// unreliable indicator.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

// ── Sizing estimate (documentation only, not a hard limit) ───────────────────
// Assuming 16-byte average key and 8-byte child pointer:
//   Internal cell: 2 (key_len) + 16 (key) + 4 (child_id) = 22 bytes + 2 (slot)
//   Leaf cell:     2 (key_len) + 16 (key) + 2 (val_len) + 16 (val) = 36 + 2
// At 36 bytes/cell with a 4072-byte data area ≈ 100 cells per node.
static constexpr uint32_t BTREE_ORDER_ESTIMATE = 100;

// Byte type for slot offsets within the data area.
using slot_offset_t = uint16_t;

// ── NodeHeader ────────────────────────────────────────────────────────────────
// Lives at offset 0 of page.data() on every B+Tree page.
// Exactly 32 bytes so the slot array starts at a 2-byte aligned offset.
struct NodeHeader {
    uint16_t  num_keys      = 0;                  // 2  — current number of cells
    uint16_t  free_end      = 0;                  // 2  — offset of first free byte from top of data area (grows down as slots are added)
    uint16_t  cell_end      = 0;                  // 2  — offset of lowest cell byte (grows down as cells are added; 0 means heap starts at DATA_SIZE)
    uint16_t  _pad          = 0;                  // 2  — alignment
    page_id_t parent_id     = INVALID_PAGE_ID;    // 4  — parent page ID (INVALID for root)
    page_id_t right_sibling = INVALID_PAGE_ID;    // 4  — leaf linked list (INVALID for internal/rightmost)
    page_id_t left_sibling  = INVALID_PAGE_ID;    // 4  — leaf linked list (INVALID for internal/leftmost)
    page_id_t first_child   = INVALID_PAGE_ID;    // 4  — internal: leftmost child (leaf: unused)
    uint32_t  _pad2         = 0;                  // 4  — pad to 28 bytes, then...
    uint32_t  _pad3         = 0;                  // 4  — ...total 32 bytes
};
static_assert(sizeof(NodeHeader) == 32, "NodeHeader must be 32 bytes");

// ── Layout constants ──────────────────────────────────────────────────────────

// Size of the NodeHeader in the data area.
static constexpr uint16_t NODE_HEADER_SIZE = sizeof(NodeHeader);

// Offset within data() where the slot array begins.
static constexpr uint16_t SLOT_ARRAY_START = NODE_HEADER_SIZE;

// Total bytes in a page's data area available to the B+Tree.
static constexpr uint16_t NODE_DATA_SIZE =
    static_cast<uint16_t>(Page::DATA_SIZE);

// After the NodeHeader, the usable area for slots + cells.
static constexpr uint16_t NODE_USABLE_SIZE =
    static_cast<uint16_t>(Page::DATA_SIZE - NODE_HEADER_SIZE);

// ── Cell encoding helpers (internal) ─────────────────────────────────────────
//
// Leaf cell wire format (contiguous bytes in the cell heap, read bottom-up):
//   [uint16_t val_len][val_bytes...][uint16_t key_len][key_bytes...]
//
// Internal cell wire format:
//   [page_id_t right_child][uint16_t key_len][key_bytes...]
//
// The slot stores the offset from the START of page.data() to the FIRST byte
// of the cell (i.e., the key_len field for both leaf and internal cells).
//
// We write from high addresses downward (same as how stacks grow).
// cell_end tracks the current lowest allocated address; after writing a new
// cell, cell_end is updated to the cell's start offset.

// Size of a leaf cell payload (not counting the slot entry itself).
inline size_t leaf_cell_size(size_t key_len, size_t val_len) {
    return sizeof(uint16_t) + key_len   // key_len field + key bytes
         + sizeof(uint16_t) + val_len;  // val_len field + val bytes
}

// Size of an internal cell payload.
inline size_t internal_cell_size(size_t key_len) {
    return sizeof(uint16_t) + key_len   // key_len field + key bytes
         + sizeof(page_id_t);           // right child pointer
}

// Total bytes consumed by adding one new cell (slot entry + cell payload).
inline size_t total_cell_cost(size_t cell_payload_bytes) {
    return sizeof(slot_offset_t) + cell_payload_bytes;
}

} // namespace keyvdb
