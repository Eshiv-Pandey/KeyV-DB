#pragma once

#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "wal/wal_defs.h"

// ─────────────────────────────────────────────────────────────────────────────
// log_manager.h — Append-only WAL writer.
//
// LogManager owns the WAL file descriptor and provides the write path: callers
// append records with append_record(), and force records to disk with
// flush(). The manager assigns monotonically increasing LSNs and tracks the
// next-available LSN.
//
// File format:
//   The WAL file is a flat sequence of variable-length records. Each record
//   starts with a WalRecordHeader (32 bytes, see wal_defs.h), followed by
//   key_len + before_len + after_len bytes of payload. There is no file-level
//   header; the first record is the first byte of the file.
//
// Crash semantics — write ordering:
//   1. append_record(BEGIN)
//   2. append_record(WRITE, key, before, after) — one per modified key
//   3. append_record(COMMIT)
//   4. flush()                    ← fsync: COMMIT is now durable
//   5. buffer pool flushes pages  ← data pages can reach disk after log
//
//   If the process crashes before step 4, the transaction has no COMMIT record.
//   Recovery will undo all its WRITE records. This is correct.
//
//   If the process crashes after step 4 but before step 5, recovery will
//   redo all the transaction's WRITE records, making the data pages consistent
//   with the committed log.
//
// LSN assignment:
//   LSNs start at 1. Each call to append_record() increments the counter by 1
//   and returns the assigned LSN. LSN 0 is reserved as "no LSN" (equivalent
//   to INVALID for page_lsn fields on pages that have never been logged).
//
// Thread safety:
//   A single std::mutex serialises all append_record() and flush() calls.
//   This is sufficient for Milestone 3 (single writer). Milestone 4 will
//   keep this mutex but transactions will batch their writes.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

class LogManager {
public:
    // Open (or create) the WAL file at wal_path.
    // If the file already exists, scans to the end to recover the next_lsn_
    // and next_txn_id_ so that they continue monotonically after a restart.
    // Throws std::system_error on open failure.
    explicit LogManager(const std::string& wal_path);

    // Close the file descriptor.
    ~LogManager();

    // Non-copyable, non-movable.
    LogManager(const LogManager&)            = delete;
    LogManager& operator=(const LogManager&) = delete;
    LogManager(LogManager&&)                 = delete;
    LogManager& operator=(LogManager&&)      = delete;

    // ── Write path ────────────────────────────────────────────────────────────

    // Append a BEGIN record and return the assigned LSN.
    // Also allocates and returns a new unique transaction ID.
    lsn_t begin_txn(txn_id_t& out_txn_id);

    // Append a WRITE record for a single key mutation.
    // before_exists: false if the key was absent before this write (INSERT).
    // before_val: the old value (relevant for UPDATE and DELETE; undo uses this).
    // after_val: the new value (empty string means DELETE).
    // Returns the assigned LSN.
    lsn_t append_write(txn_id_t txn_id,
                       const std::string& key,
                       bool before_exists,
                       const std::string& before_val,
                       const std::string& after_val);

    // Append a COMMIT record and return the assigned LSN.
    // Caller MUST call flush() immediately after to make the commit durable.
    lsn_t append_commit(txn_id_t txn_id);

    // Append an ABORT record and return the assigned LSN.
    lsn_t append_abort(txn_id_t txn_id);

    // fsync the WAL file. Must be called after append_commit() to guarantee
    // durability. After this call returns, all records written before it are
    // safely on the storage device.
    // Throws std::system_error on fsync failure.
    void flush();

    // ── Read path (used by RecoveryManager) ──────────────────────────────────

    // Read all records from the WAL file into `out_records`, in LSN order.
    // Called once at startup by RecoveryManager before the database accepts
    // any new operations.
    // Throws std::runtime_error on malformed record.
    void read_all(std::vector<WalRecord>& out_records) const;

    // Truncate the WAL file to zero bytes. Called after a successful recovery
    // checkpoint to reclaim disk space. Should only be called when there are
    // no active transactions.
    void truncate();

    // ── Accessors ─────────────────────────────────────────────────────────────

    lsn_t     next_lsn()    const { return next_lsn_; }
    txn_id_t  next_txn_id() const { return next_txn_id_; }

private:
    int      fd_;
    lsn_t    next_lsn_    = 1;  // LSN to assign to the next record
    txn_id_t next_txn_id_ = 1;  // txn_id to assign to the next transaction

    mutable std::mutex mu_;

    // Append a fully-formed WalRecord to the file (called by the public API
    // methods after they populate a WalRecord struct).
    lsn_t append_record_locked(WalRecord& rec);

    // Scan the WAL file from the beginning to find the highest LSN and txn_id
    // present. Used on open to resume monotonic counters.
    void scan_for_max_ids();
};

} // namespace keyvdb
