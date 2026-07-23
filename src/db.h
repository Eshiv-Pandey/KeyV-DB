#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>

#include "storage/disk_manager.h"
#include "storage/free_list.h"
#include "storage/buffer_pool.h"
#include "btree/btree.h"
#include "wal/log_manager.h"
#include "wal/recovery_manager.h"
#include "wal/wal_defs.h"
#include "txn/lock_manager.h"

// ─────────────────────────────────────────────────────────────────────────────
// db.h — Public API for KeyVDB (Milestone 4: full ACID with 2PL).
//
// DB is the top-level object. Callers:
//   1. Open (or create) a database with DB::Open().
//   2. Begin a transaction with db->Begin().
//   3. Call Get/Put/Delete on the transaction.
//   4. Call Commit() or Rollback().
//
// Isolation model — two-phase locking (2PL):
//   Get()    acquires a SHARED lock on the key.
//   Put()    acquires an EXCLUSIVE lock on the key.
//   Delete() acquires an EXCLUSIVE lock on the key.
//   Locks are released all-at-once at Commit() or Rollback() (strict 2PL).
//
//   If a lock cannot be granted within LockManager::LOCK_TIMEOUT_MS, a
//   DeadlockException is thrown. The caller must catch it, roll back the
//   transaction, and retry.
//
// Thread safety:
//   DB::Begin() is thread-safe — multiple threads may call it concurrently.
//   Individual Transaction objects are NOT shared across threads.
//   The BTree + BufferPool are protected by db_mu_ (a coarse mutex held for
//   the duration of each B+Tree operation). Key-level 2PL via LockManager
//   provides logical isolation between concurrent transactions.
//
// File layout:
//   <path>.db   — B+Tree data pages (managed by DiskManager)
//   <path>.wal  — Write-ahead log records (managed by LogManager)
// ─────────────────────────────────────────────────────────────────────────────

namespace keyvdb {

// Forward declaration.
class Transaction;

// ── DB ────────────────────────────────────────────────────────────────────────

class DB {
public:
    // Open (or create) a database at `db_path`.
    // On first open: creates a new B+Tree.
    // On reopen:     runs WAL recovery before accepting new operations.
    // lock_timeout_ms: how long a lock request waits before DeadlockException.
    static std::unique_ptr<DB> Open(const std::string& db_path,
                                    int lock_timeout_ms = LockManager::DEFAULT_TIMEOUT_MS);

    ~DB() = default;

    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;
    DB(DB&&)                 = delete;
    DB& operator=(DB&&)      = delete;

    // Begin a new read-write transaction. Thread-safe.
    std::unique_ptr<Transaction> Begin();

    // Flush all dirty pages, fsync, and truncate the WAL.
    // Call this for a clean shutdown. After Close(), no further operations
    // should be issued. The DB object may be destroyed after this.
    void Close();

    // Internal accessors for Transaction and tests.
    BTree&        btree()    { return *btree_; }
    LogManager&   log()      { return *log_; }
    BufferPool&   pool()     { return *bp_; }
    DiskManager&  dm()       { return *dm_; }
    LockManager&  locks()    { return lm_; }
    std::mutex&   db_mutex() { return db_mu_; }

private:
    DB() = default;

    std::unique_ptr<DiskManager>  dm_;
    std::unique_ptr<FreeList>     fl_;
    std::unique_ptr<BufferPool>   bp_;
    std::unique_ptr<BTree>        btree_;
    std::unique_ptr<LogManager>   log_;

    LockManager  lm_;    // key-level 2PL
    std::mutex   db_mu_; // coarse BTree/BufferPool guard

    explicit DB(int lock_timeout_ms = LockManager::DEFAULT_TIMEOUT_MS)
        : lm_(lock_timeout_ms) {}
};

// ── Transaction ───────────────────────────────────────────────────────────────

class Transaction {
public:
    Transaction(DB& db, LogManager& lm, BTree& btree,
                LockManager& lock_mgr, txn_id_t txn_id);

    // Auto-rollback if not committed or rolled back.
    ~Transaction();

    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&)                 = delete;
    Transaction& operator=(Transaction&&)      = delete;

    // Returns value for key, or nullopt. Acquires SHARED lock.
    // Throws DeadlockException on lock timeout.
    std::optional<std::string> Get(std::string_view key);

    // Insert/overwrite key. Acquires EXCLUSIVE lock.
    // Throws DeadlockException on lock timeout.
    void Put(std::string_view key, std::string_view value);

    // Delete key (no-op if absent). Acquires EXCLUSIVE lock.
    // Throws DeadlockException on lock timeout.
    void Delete(std::string_view key);

    // fsync WAL, flush pages, release all locks.
    void Commit();

    // Undo all writes in-memory, release all locks.
    void Rollback();

    txn_id_t id() const { return txn_id_; }

private:
    DB&           db_;
    LogManager&   lm_;
    BTree&        btree_;
    LockManager&  lock_mgr_;
    txn_id_t      txn_id_;
    bool          done_ = false;

    struct WriteEntry {
        bool        before_exists;
        std::string before_val;
        std::string after_val;
    };
    std::unordered_map<std::string, WriteEntry> write_set_;

    // Helpers: must be called with the appropriate lock already held.
    std::optional<std::string> get_locked(const std::string& key);
    void put_locked(const std::string& key, const std::string& value);
    void delete_locked(const std::string& key);
    void undo_all();
};

} // namespace keyvdb
