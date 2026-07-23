#include "txn/lock_manager.h"

#include <cassert>

namespace keyvdb {

// ── can_grant (static helper) ─────────────────────────────────────────────────

bool LockManager::can_grant(const LockState& ls,
                             txn_id_t txn_id,
                             LockMode mode) {
    if (mode == LockMode::SHARED) {
        // S is compatible with other S-holders, but not with an X-holder
        // (unless the X-holder is the same transaction).
        if (ls.exclusive_holder == INVALID_TXN_ID) return true;
        return ls.exclusive_holder == txn_id;  // re-entrant: already holds X
    }

    // Exclusive: must be the only holder.
    // Allow if:
    //   - Nobody holds anything.
    //   - Only this transaction holds an S-lock (upgrade case: S → X).
    //   - This transaction already holds X (re-entrant).

    if (ls.exclusive_holder != INVALID_TXN_ID) {
        // Someone holds X.
        return ls.exclusive_holder == txn_id;  // ok if it's us
    }

    if (ls.shared_holders.empty()) return true;  // nobody holds anything

    // Upgrade case: we hold S alone.
    if (ls.shared_holders.size() == 1 &&
        ls.shared_holders.count(txn_id)) {
        return true;
    }

    return false;  // someone else holds S
}

// ── lock ─────────────────────────────────────────────────────────────────────

void LockManager::lock(txn_id_t txn_id, const std::string& key, LockMode mode) {
    std::unique_lock<std::mutex> ul(mu_);

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms_);

    // Re-look up (or create) the LockState on every iteration.
    // We must NOT hold a LockState& reference across cv_.wait_until because
    // release_all() may erase the entry while we are waiting, invalidating
    // any held reference (caught by ASan as heap-use-after-free).
    while (true) {
        LockState& ls = lock_table_[key];
        if (can_grant(ls, txn_id, mode)) break;

        auto status = cv_.wait_until(ul, deadline);
        if (status == std::cv_status::timeout) {
            // Clean up the empty entry we may have created via operator[].
            auto it = lock_table_.find(key);
            if (it != lock_table_.end() &&
                it->second.shared_holders.empty() &&
                it->second.exclusive_holder == INVALID_TXN_ID) {
                lock_table_.erase(it);
            }
            throw DeadlockException(
                "LockManager: lock timeout on key '" + key +
                "' for txn " + std::to_string(txn_id) +
                " — assumed deadlock, aborting transaction");
        }
        // Woke up — loop back and re-check can_grant with a fresh LockState ref.
    }

    // Lock granted — record it.
    LockState& ls = lock_table_[key];
    if (mode == LockMode::SHARED) {
        if (ls.exclusive_holder == txn_id) {
            // Already holds X — no need to add to shared_holders.
            return;
        }
        ls.shared_holders.insert(txn_id);
    } else {
        ls.shared_holders.erase(txn_id);  // upgrade: remove from S if present
        ls.exclusive_holder = txn_id;
    }

    txn_locks_[txn_id].push_back(key);
}

// ── release_all ───────────────────────────────────────────────────────────────

void LockManager::release_all(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lg(mu_);

    auto it = txn_locks_.find(txn_id);
    if (it == txn_locks_.end()) return;  // nothing to release

    for (const auto& key : it->second) {
        auto ls_it = lock_table_.find(key);
        if (ls_it == lock_table_.end()) continue;

        LockState& ls = ls_it->second;
        ls.shared_holders.erase(txn_id);
        if (ls.exclusive_holder == txn_id) {
            ls.exclusive_holder = INVALID_TXN_ID;
        }

        // Prune empty lock states to keep the table compact.
        if (ls.shared_holders.empty() &&
            ls.exclusive_holder == INVALID_TXN_ID) {
            lock_table_.erase(ls_it);
        }
    }

    txn_locks_.erase(it);

    // Wake all waiting transactions — let them re-check can_grant.
    cv_.notify_all();
}

// ── Testing helpers ───────────────────────────────────────────────────────────

bool LockManager::holds_lock(txn_id_t txn_id, const std::string& key) const {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) return false;
    const LockState& ls = it->second;
    return ls.shared_holders.count(txn_id) > 0 ||
           ls.exclusive_holder == txn_id;
}

bool LockManager::holds_exclusive(txn_id_t txn_id, const std::string& key) const {
    std::lock_guard<std::mutex> lg(mu_);
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) return false;
    return it->second.exclusive_holder == txn_id;
}

} // namespace keyvdb
