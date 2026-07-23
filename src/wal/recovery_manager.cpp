#include "wal/recovery_manager.h"

#include <cassert>
#include <stdexcept>

namespace keyvdb {

RecoveryManager::RecoveryManager(LogManager& lm, BTree& btree, BufferPool& bp)
    : lm_(lm), btree_(btree), bp_(bp)
{}

// ── Public: full recovery sequence ───────────────────────────────────────────

int RecoveryManager::recover() {
    std::vector<WalRecord> records;
    lm_.read_all(records);

    if (records.empty()) {
        return 0;  // clean database — nothing to do
    }

    auto losers = analysis(records);
    redo(records);
    undo(records, losers);

    return static_cast<int>(losers.size());
}

// ── Phase 1: Analysis ─────────────────────────────────────────────────────────

std::unordered_set<txn_id_t>
RecoveryManager::analysis(const std::vector<WalRecord>& records) {
    // Build the set of active transactions: started but not finished.
    std::unordered_set<txn_id_t> active;

    for (const auto& r : records) {
        switch (r.type) {
            case WalRecordType::BEGIN:
                active.insert(r.txn_id);
                break;
            case WalRecordType::COMMIT:
            case WalRecordType::ABORT:
                active.erase(r.txn_id);
                break;
            default:
                break;
        }
    }
    // active now contains every txn that had no COMMIT or ABORT — these are
    // the "losers" whose writes must be undone.
    return active;
}

// ── Phase 2: Redo ─────────────────────────────────────────────────────────────

void RecoveryManager::redo(const std::vector<WalRecord>& records) {
    // Replay every WRITE record in LSN order.
    // For each key, apply the after_val (or delete if after_val is empty).
    //
    // Redo is idempotent via page_lsn: if a page's on-disk page_lsn is
    // already >= this record's lsn, the write was already persisted before
    // the crash. We still apply it here because:
    //   - Reading page_lsn from the B+Tree requires a page fetch, which is
    //     expensive for every record.
    //   - For a simple embedded key-value store the WAL is small enough that
    //     replaying everything is fast.
    //   - Correctness: applying the same write twice to a B+Tree is safe
    //     (insert_leaf_cell handles duplicate keys by overwriting).
    //
    // A production system would compare page_lsn for each record to avoid
    // unnecessary page fetches. We omit that optimisation in M3.
    //
    // We only redo WRITE records from COMMITTED transactions. Aborted
    // transactions were already undone at runtime — their writes were reversed
    // before the pages were flushed. Redoing them would re-introduce data that
    // was deliberately rolled back.

    // First pass: collect committed and aborted transaction IDs.
    std::unordered_set<txn_id_t> committed;
    std::unordered_set<txn_id_t> aborted;
    for (const auto& r : records) {
        if (r.type == WalRecordType::COMMIT) committed.insert(r.txn_id);
        if (r.type == WalRecordType::ABORT)  aborted.insert(r.txn_id);
    }

    for (const auto& r : records) {
        if (r.type != WalRecordType::WRITE) continue;
        // Skip writes from aborted transactions — the in-memory rollback
        // already undid them and the undo was flushed to disk before shutdown.
        if (aborted.count(r.txn_id)) continue;

        if (r.after_val.empty()) {
            // DELETE: remove the key.
            btree_.remove(r.key);
        } else {
            // INSERT or UPDATE: apply the after value.
            btree_.insert(r.key, r.after_val);
        }
    }
}

// ── Phase 3: Undo ─────────────────────────────────────────────────────────────

void RecoveryManager::undo(const std::vector<WalRecord>& records,
                            const std::unordered_set<txn_id_t>& losers) {
    if (losers.empty()) return;

    // Process WRITE records belonging to losers in REVERSE LSN order.
    // For each record, restore the "before" state:
    //   - before_exists == false → key was inserted; undo = delete it.
    //   - before_exists == true  → key was updated/deleted; undo = restore old value.
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const WalRecord& r = *it;
        if (r.type != WalRecordType::WRITE) continue;
        if (losers.find(r.txn_id) == losers.end()) continue;

        if (!r.before_exists) {
            // Key was inserted by this transaction — undo = delete it.
            btree_.remove(r.key);
        } else {
            // Key existed before — undo = restore the before_val.
            // (If before_val is empty string, the original value was "".
            //  If this was a DELETE, after_val is empty but before_val
            //  holds the original value that must be restored.)
            btree_.insert(r.key, r.before_val);
        }
    }
}

} // namespace keyvdb
