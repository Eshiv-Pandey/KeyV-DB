#pragma once

#include <cstdint>
#include "storage/page.h"  // for lsn_t, page_id_t

// ─────────────────────────────────────────────────────────────────────────────
// wal_defs.h — On-disk record format and type tags for the Write-Ahead Log.
//
// The WAL is a sequential, append-only binary file. Records are written in
// strict LSN (Log Sequence Number) order. No record is ever modified after
// it is written.
//
// Record wire format (variable length):
//
//   Field         Type         Size        Notes
//   ─────────────────────────────────────────────────────────
//   lsn           lsn_t        8 bytes     monotonically increasing, starts at 1
//   txn_id        txn_id_t     8 bytes     which transaction wrote this record
//   type          WalRecordType 4 bytes    BEGIN / WRITE / COMMIT / ABORT
//   key_len       uint32_t     4 bytes     0 for BEGIN / COMMIT / ABORT
//   key           char[]       key_len     absent if key_len == 0
//   before_len    uint32_t     4 bytes     0 for INSERT (key did not exist)
//   before_val    char[]       before_len  absent if before_len == 0
//   after_len     uint32_t     4 bytes     0 for DELETE
//   after_val     char[]       after_len   absent if after_len == 0
//
// Total header overhead per WRITE record: 8+8+4+4+4+4 = 32 bytes + payload.
// Total overhead per BEGIN/COMMIT/ABORT: 8+8+4 = 20 bytes (no payload fields).
//
// The BEGIN/COMMIT/ABORT records still include key_len/before_len/after_len
// as zeros so the reader can use a single fixed-header parse path.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

// ── Transaction ID ────────────────────────────────────────────────────────────
// Unique per-transaction identifier. Monotonically increasing across the
// lifetime of the database. Stored in every WAL record so recovery can
// associate records with their originating transaction.
using txn_id_t = int64_t;

static constexpr txn_id_t INVALID_TXN_ID = -1;

// ── WAL record types ──────────────────────────────────────────────────────────
enum class WalRecordType : uint32_t {
    INVALID = 0,  // Sentinel — should never appear in a valid log.
    BEGIN   = 1,  // Start of a transaction.
    WRITE   = 2,  // A key-value write (insert, update, or delete).
    COMMIT  = 3,  // Transaction committed — all prior WRITEs are durable.
    ABORT   = 4,  // Transaction aborted — all prior WRITEs must be undone.
};

// ── WAL record fixed header ───────────────────────────────────────────────────
// Written at the start of every WAL record, regardless of type.
// Variable-length payload (key, before_val, after_val) follows immediately.
//
// We use a packed struct so the layout is unambiguous across compilers.
// The reader advances past the header, then reads key_len / before_len /
// after_len bytes of payload in sequence.
#pragma pack(push, 1)
struct WalRecordHeader {
    lsn_t          lsn        = 0;
    txn_id_t       txn_id     = INVALID_TXN_ID;
    WalRecordType  type       = WalRecordType::INVALID;
    uint32_t       key_len    = 0;
    uint32_t       before_len = 0;
    uint32_t       after_len  = 0;
};
#pragma pack(pop)
static_assert(sizeof(WalRecordHeader) == 32,
              "WalRecordHeader size changed — update wal_defs.h comment");

// ── In-memory WAL record ──────────────────────────────────────────────────────
// Used by LogManager and RecoveryManager to build and decode log records
// without carrying raw byte buffers through the call stack.
struct WalRecord {
    lsn_t         lsn        = 0;
    txn_id_t      txn_id     = INVALID_TXN_ID;
    WalRecordType type       = WalRecordType::INVALID;
    std::string   key;
    std::string   before_val;   // empty string = key did not exist (INSERT)
    std::string   after_val;    // empty string = DELETE operation
    bool          before_exists = false; // true if before_val is meaningful
};

} // namespace keyvdb
