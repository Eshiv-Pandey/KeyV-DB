#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "wal/wal_defs.h"   // txn_id_t

// ─────────────────────────────────────────────────────────────────────────────
// lock_manager.h — Key-level two-phase locking for KeyVDB.
//
// LockManager implements the lock acquisition and release protocol that gives
// KeyVDB serializable isolation (the 'I' in ACID). Every read acquires a
// shared (S) lock; every write acquires an exclusive (X) lock. Locks are held
// until the transaction commits or rolls back — this is the "two-phase" rule:
// grow (acquire) then shrink (release all at once).
//
// Lock compatibility matrix:
//   Requester →       S        X
//   Holder ↓
//     S          compatible  conflict
//     X          conflict    conflict
//
// Two readers can hold S-locks on the same key simultaneously. A writer
// must wait until all readers and any other writer have released their locks.
//
// Deadlock handling — timeouts:
//   If a lock request cannot be granted within LOCK_TIMEOUT_MS milliseconds,
//   LockManager throws DeadlockException. The caller must catch this, roll
//   back the transaction, and retry or propagate the error.
//
//   Timeouts are simpler than cycle detection (wait-for graph) and sufficient
//   for correctness: any cycle of waiting transactions will eventually break
//   when the longest-waiting one times out. The tradeoff is occasional false
//   aborts of slow (but not actually deadlocked) transactions.
//
// Upgrade:
//   A transaction that holds an S-lock on a key and then issues a write
//   request for the same key needs a lock upgrade (S → X). This is handled
//   atomically: the S-lock is replaced by an X-lock, but only if no other
//   transaction holds a conflicting S-lock on the same key.
//
// Thread safety:
//   All public methods are thread-safe. A single std::mutex serialises access
//   to the lock table. Condition variables are used to block waiting callers
//   rather than spin-waiting.
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

// ── Exception types ───────────────────────────────────────────────────────────

// Thrown when a lock wait exceeds LOCK_TIMEOUT_MS.
// The caller must roll back the transaction.
class DeadlockException : public std::runtime_error {
public:
    explicit DeadlockException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ── Lock mode ─────────────────────────────────────────────────────────────────

enum class LockMode {
    SHARED,     // S-lock: shared read access
    EXCLUSIVE,  // X-lock: exclusive write access
};

// ── LockManager ───────────────────────────────────────────────────────────────

class LockManager {
public:
    // Maximum milliseconds a lock request will wait before timing out.
    // Configurable so tests can use a shorter timeout.
    explicit LockManager(int timeout_ms = 200) : timeout_ms_(timeout_ms) {}
    ~LockManager() = default;

    // Non-copyable, non-movable.
    LockManager(const LockManager&)            = delete;
    LockManager& operator=(const LockManager&) = delete;
    LockManager(LockManager&&)                 = delete;
    LockManager& operator=(LockManager&&)      = delete;

    // Acquire a lock on `key` for transaction `txn_id`.
    //
    // If txn_id already holds the requested mode (or a stronger mode), this
    // is a no-op (idempotent).
    //
    // If txn_id holds S-lock and requests X-lock (upgrade): waits until no
    // other transaction holds a conflicting lock on the key.
    //
    // If another transaction holds a conflicting lock: waits up to
    // LOCK_TIMEOUT_MS. Throws DeadlockException on timeout.
    //
    // Thread-safe.
    void lock(txn_id_t txn_id, const std::string& key, LockMode mode);

    // Release all locks held by `txn_id`.
    // Called at commit or rollback. After this, other waiting transactions
    // are woken and can acquire the released locks.
    // Thread-safe.
    void release_all(txn_id_t txn_id);

    // For testing: true if txn_id holds any lock on key.
    bool holds_lock(txn_id_t txn_id, const std::string& key) const;

    // For testing: true if txn_id holds an exclusive lock on key.
    bool holds_exclusive(txn_id_t txn_id, const std::string& key) const;

    static constexpr int DEFAULT_TIMEOUT_MS = 200;

private:
    // Per-key lock state.
    struct LockState {
        // Transactions currently holding a SHARED lock on this key.
        std::unordered_set<txn_id_t> shared_holders;

        // Transaction currently holding the EXCLUSIVE lock, or INVALID_TXN_ID.
        txn_id_t exclusive_holder = INVALID_TXN_ID;
    };

    mutable std::mutex                            mu_;
    std::condition_variable                       cv_;
    std::unordered_map<std::string, LockState>    lock_table_;
    std::unordered_map<txn_id_t,
        std::vector<std::string>>                 txn_locks_;
    int                                           timeout_ms_;

    // Return true if txn_id can immediately acquire `mode` on this LockState.
    // Accounts for upgrade (txn already holds S, requesting X with only itself
    // as the S-holder).
    static bool can_grant(const LockState& ls,
                          txn_id_t txn_id,
                          LockMode mode);
};

} // namespace keyvdb
