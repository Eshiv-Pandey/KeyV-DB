#pragma once

#include <unordered_set>
#include <vector>

#include "wal/log_manager.h"
#include "btree/btree.h"
#include "storage/buffer_pool.h"
#include "storage/free_list.h"

// ─────────────────────────────────────────────────────────────────────────────
// recovery_manager.h — Crash recovery via a simplified ARIES algorithm.
//
// RecoveryManager is invoked exactly once during database startup, before any
// new transactions are accepted. It reads the WAL and brings the B+Tree to a
// consistent state by replaying committed writes and undoing uncommitted ones.
//
// ── The three phases ─────────────────────────────────────────────────────────
//
// ANALYSIS: Scan the WAL forward to determine which transactions were active
//   (had a BEGIN but no COMMIT or ABORT) at the time of the crash. This
//   produces the "loser set" — transactions whose effects must be undone.
//
// REDO: Replay all WRITE records in LSN order, regardless of whether the
//   transaction committed. This brings every page to the exact byte state
//   it was in at the crash instant, including in-progress writes. Redo is
//   idempotent: if a page's page_lsn is already >= the record's LSN, the
//   record has already been applied and we skip it.
//
// UNDO: For each loser transaction, replay its WRITE records in reverse LSN
//   order, restoring the "before" value (or deleting the key if it was an
//   INSERT). This rolls back all uncommitted work.
//
// After these three phases the database is in a fully consistent state:
//   - Every committed transaction is fully reflected in the B+Tree.
//   - Every uncommitted transaction is fully absent from the B+Tree.
//
// ── Why redo then undo, rather than just undoing the losers? ─────────────────
//
// Because pages may not have been flushed to disk at crash time. A committed
// transaction's writes might be in the WAL but not yet in the data file. Redo
// ensures they are. Only after all committed writes are applied can we safely
// undo the uncommitted ones — because undo uses the B+Tree's current state as
// the starting point and applies before-images on top.
//
// ── page_lsn and redo skipping ────────────────────────────────────────────────
//
// Every B+Tree page stores its page_lsn — the LSN of the last WAL record that
// modified it. If page_lsn >= record.lsn, the write has already been applied
// to the on-disk page (it was flushed before the crash). We skip that record
// during redo. This is the standard ARIES optimisation.
//
// In Milestone 3 the page_lsn is set during recovery. Milestone 4 will set
// it during normal operation as well.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

class RecoveryManager {
public:
    RecoveryManager(LogManager& lm, BTree& btree, BufferPool& bp);

    // Execute the full recovery sequence: Analysis → Redo → Undo.
    // Must be called before any new transactions begin.
    // Returns the number of transactions that were rolled back.
    int recover();

private:
    LogManager&  lm_;
    BTree&       btree_;
    BufferPool&  bp_;

    // Phase 1: determine which transactions did not commit.
    // Returns the set of txn_ids that are losers.
    std::unordered_set<txn_id_t> analysis(
        const std::vector<WalRecord>& records);

    // Phase 2: replay all WRITE records forward.
    void redo(const std::vector<WalRecord>& records);

    // Phase 3: undo all writes belonging to loser transactions.
    void undo(const std::vector<WalRecord>& records,
              const std::unordered_set<txn_id_t>& losers);
};

} // namespace keyvdb
