#include "db.h"

#include <cassert>
#include <stdexcept>

namespace keyvdb {

// ── DB::Open ──────────────────────────────────────────────────────────────────

std::unique_ptr<DB> DB::Open(const std::string& db_path, int lock_timeout_ms) {
    auto db = std::unique_ptr<DB>(new DB(lock_timeout_ms));

    std::string wal_path = db_path + ".wal";

    db->dm_ = std::make_unique<DiskManager>(db_path);
    db->fl_ = std::make_unique<FreeList>(*db->dm_);
    db->fl_->load();
    db->bp_ = std::make_unique<BufferPool>(*db->dm_, *db->fl_);
    db->log_ = std::make_unique<LogManager>(wal_path);

    page_id_t root = db->fl_->root_page_id();
    if (root == INVALID_PAGE_ID) {
        auto tree = BTree::create(*db->bp_, *db->fl_);
        db->btree_ = std::make_unique<BTree>(std::move(tree));
    } else {
        auto tree = BTree::open(*db->bp_, *db->fl_, root);
        db->btree_ = std::make_unique<BTree>(std::move(tree));

        RecoveryManager rm(*db->log_, *db->btree_, *db->bp_);
        rm.recover();

        db->bp_->flush_all();
        db->dm_->flush();
        db->log_->truncate();
    }

    return db;
}

// ── DB::Close ─────────────────────────────────────────────────────────────────

void DB::Close() {
    std::lock_guard<std::mutex> lg(db_mu_);
    bp_->flush_all();
    dm_->flush();
    log_->truncate();
}

// ── DB::Begin ─────────────────────────────────────────────────────────────────

std::unique_ptr<Transaction> DB::Begin() {
    txn_id_t txn_id = INVALID_TXN_ID;
    log_->begin_txn(txn_id);
    return std::make_unique<Transaction>(*this, *log_, *btree_, lm_, txn_id);
}

// ── Transaction ───────────────────────────────────────────────────────────────

Transaction::Transaction(DB& db, LogManager& lm, BTree& btree,
                         LockManager& lm_ref, txn_id_t txn_id)
    : db_(db), lm_(lm), btree_(btree), lock_mgr_(lm_ref), txn_id_(txn_id)
{}

Transaction::~Transaction() {
    if (!done_) {
        try { Rollback(); } catch (...) {}
    }
}

// ── Internal helpers ──────────────────────────────────────────────────────────

std::optional<std::string> Transaction::get_locked(const std::string& key) {
    std::lock_guard<std::mutex> lg(db_.db_mutex());
    return btree_.get(key);
}

void Transaction::put_locked(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lg(db_.db_mutex());

    bool is_first = (write_set_.find(key) == write_set_.end());
    WriteEntry* entry;
    if (is_first) {
        WriteEntry we;
        auto existing = btree_.get(key);
        we.before_exists = existing.has_value();
        we.before_val    = existing.has_value() ? *existing : "";
        we.after_val     = value;
        write_set_[key]  = std::move(we);
        entry = &write_set_[key];
    } else {
        entry = &write_set_[key];
        entry->after_val = value;
    }

    lm_.append_write(txn_id_, key,
                     entry->before_exists, entry->before_val, value);
    btree_.insert(key, value);
}

void Transaction::delete_locked(const std::string& key) {
    std::lock_guard<std::mutex> lg(db_.db_mutex());

    auto existing = btree_.get(key);
    if (!existing.has_value()) return;

    bool is_first = (write_set_.find(key) == write_set_.end());
    WriteEntry* entry;
    if (is_first) {
        WriteEntry we;
        we.before_exists = true;
        we.before_val    = *existing;
        we.after_val     = "";
        write_set_[key]  = std::move(we);
        entry = &write_set_[key];
    } else {
        entry = &write_set_[key];
        entry->after_val = "";
    }

    lm_.append_write(txn_id_, key,
                     entry->before_exists, entry->before_val, "");
    btree_.remove(key);
}

void Transaction::undo_all() {
    std::lock_guard<std::mutex> lg(db_.db_mutex());
    for (auto& [k, we] : write_set_) {
        if (!we.before_exists) {
            btree_.remove(k);
        } else {
            btree_.insert(k, we.before_val);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

std::optional<std::string> Transaction::Get(std::string_view key) {
    if (done_) throw std::runtime_error("Transaction::Get: already ended");
    std::string k(key);

    // Acquire shared lock — blocks if another txn holds X on this key.
    lock_mgr_.lock(txn_id_, k, LockMode::SHARED);

    return get_locked(k);
}

void Transaction::Put(std::string_view key, std::string_view value) {
    if (done_) throw std::runtime_error("Transaction::Put: already ended");
    std::string k(key);
    std::string v(value);

    // Acquire exclusive lock — blocks if anyone else holds S or X.
    lock_mgr_.lock(txn_id_, k, LockMode::EXCLUSIVE);

    put_locked(k, v);
}

void Transaction::Delete(std::string_view key) {
    if (done_) throw std::runtime_error("Transaction::Delete: already ended");
    std::string k(key);

    // Acquire exclusive lock.
    lock_mgr_.lock(txn_id_, k, LockMode::EXCLUSIVE);

    delete_locked(k);
}

void Transaction::Commit() {
    if (done_) throw std::runtime_error("Transaction::Commit: already ended");
    done_ = true;

    lm_.append_commit(txn_id_);
    lm_.flush();   // WAL fsync — durability point

    {
        std::lock_guard<std::mutex> lg(db_.db_mutex());
        db_.pool().flush_all();
        db_.dm().flush();
    }

    // Release all key-level locks — unblocks waiting transactions.
    lock_mgr_.release_all(txn_id_);
}

void Transaction::Rollback() {
    if (done_) throw std::runtime_error("Transaction::Rollback: already ended");
    done_ = true;

    undo_all();
    lm_.append_abort(txn_id_);

    // Release all key-level locks.
    lock_mgr_.release_all(txn_id_);
}

} // namespace keyvdb
