#include "wal/log_manager.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace keyvdb {

// ── Constructor / destructor ──────────────────────────────────────────────────

LogManager::LogManager(const std::string& wal_path) {
    fd_ = ::open(wal_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "LogManager: failed to open WAL " + wal_path);
    }
    // Resume monotonic counters from any existing content.
    scan_for_max_ids();
}

LogManager::~LogManager() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ── Private: scan to recover next_lsn_ and next_txn_id_ ──────────────────────

void LogManager::scan_for_max_ids() {
    // Walk the file from the beginning, reading each record header.
    // We need the max LSN and max txn_id so we can continue monotonically.
    lsn_t    max_lsn    = 0;
    txn_id_t max_txn_id = 0;

    off_t offset = 0;
    while (true) {
        WalRecordHeader hdr{};
        ssize_t n = ::pread(fd_, &hdr, sizeof(hdr), offset);
        if (n == 0) break;                  // clean EOF
        if (n < static_cast<ssize_t>(sizeof(hdr))) break;  // truncated record — stop

        if (hdr.type == WalRecordType::INVALID) break;  // zeroed / corrupt tail

        if (hdr.lsn > max_lsn)       max_lsn    = hdr.lsn;
        if (hdr.txn_id > max_txn_id) max_txn_id = hdr.txn_id;

        // Advance past this record.
        offset += static_cast<off_t>(sizeof(WalRecordHeader))
                + static_cast<off_t>(hdr.key_len)
                + static_cast<off_t>(hdr.before_len)
                + static_cast<off_t>(hdr.after_len);
    }

    if (max_lsn    > 0) next_lsn_    = max_lsn    + 1;
    if (max_txn_id > 0) next_txn_id_ = max_txn_id + 1;
}

// ── Private: low-level append ─────────────────────────────────────────────────

lsn_t LogManager::append_record_locked(WalRecord& rec) {
    // Assign LSN.
    rec.lsn = next_lsn_++;

    // Serialize to a contiguous buffer: header + key + before_val + after_val.
    WalRecordHeader hdr{};
    hdr.lsn        = rec.lsn;
    hdr.txn_id     = rec.txn_id;
    hdr.type       = rec.type;
    hdr.key_len    = static_cast<uint32_t>(rec.key.size());
    hdr.before_len = static_cast<uint32_t>(rec.before_val.size());
    hdr.after_len  = static_cast<uint32_t>(rec.after_val.size());

    // Build a single contiguous buffer for one pwrite call.
    size_t total = sizeof(WalRecordHeader)
                 + rec.key.size()
                 + rec.before_val.size()
                 + rec.after_val.size();

    std::vector<char> buf(total);
    char* dst = buf.data();

    std::memcpy(dst, &hdr, sizeof(hdr));    dst += sizeof(hdr);
    if (!rec.key.empty()) {
        std::memcpy(dst, rec.key.data(), rec.key.size());
        dst += rec.key.size();
    }
    if (!rec.before_val.empty()) {
        std::memcpy(dst, rec.before_val.data(), rec.before_val.size());
        dst += rec.before_val.size();
    }
    if (!rec.after_val.empty()) {
        std::memcpy(dst, rec.after_val.data(), rec.after_val.size());
    }

    // Append: seek to end, then write.
    off_t end = ::lseek(fd_, 0, SEEK_END);
    if (end < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "LogManager: lseek(SEEK_END) failed");
    }

    ssize_t n = ::pwrite(fd_, buf.data(), total,  end);
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "LogManager::append_record: pwrite failed");
    }
    if (static_cast<size_t>(n) != total) {
        throw std::runtime_error("LogManager::append_record: short write");
    }

    return rec.lsn;
}

// ── Public write path ─────────────────────────────────────────────────────────

lsn_t LogManager::begin_txn(txn_id_t& out_txn_id) {
    std::lock_guard<std::mutex> lock(mu_);
    out_txn_id = next_txn_id_++;

    WalRecord rec;
    rec.txn_id = out_txn_id;
    rec.type   = WalRecordType::BEGIN;
    return append_record_locked(rec);
}

lsn_t LogManager::append_write(txn_id_t txn_id,
                                const std::string& key,
                                bool before_exists,
                                const std::string& before_val,
                                const std::string& after_val) {
    std::lock_guard<std::mutex> lock(mu_);

    WalRecord rec;
    rec.txn_id        = txn_id;
    rec.type          = WalRecordType::WRITE;
    rec.key           = key;
    rec.before_exists = before_exists;
    rec.before_val    = before_exists ? before_val : std::string{};
    rec.after_val     = after_val;
    return append_record_locked(rec);
}

lsn_t LogManager::append_commit(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(mu_);

    WalRecord rec;
    rec.txn_id = txn_id;
    rec.type   = WalRecordType::COMMIT;
    return append_record_locked(rec);
}

lsn_t LogManager::append_abort(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(mu_);

    WalRecord rec;
    rec.txn_id = txn_id;
    rec.type   = WalRecordType::ABORT;
    return append_record_locked(rec);
}

void LogManager::flush() {
    std::lock_guard<std::mutex> lock(mu_);
    if (::fsync(fd_) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "LogManager::flush: fsync failed");
    }
}

// ── Read path (RecoveryManager) ───────────────────────────────────────────────

void LogManager::read_all(std::vector<WalRecord>& out_records) const {
    std::lock_guard<std::mutex> lock(mu_);
    out_records.clear();

    off_t offset = 0;
    while (true) {
        WalRecordHeader hdr{};
        ssize_t n = ::pread(fd_, &hdr, sizeof(hdr), offset);
        if (n == 0) break;
        if (n < static_cast<ssize_t>(sizeof(hdr))) break;  // truncated tail
        if (hdr.type == WalRecordType::INVALID) break;      // zeroed tail

        offset += static_cast<off_t>(sizeof(WalRecordHeader));

        WalRecord rec;
        rec.lsn    = hdr.lsn;
        rec.txn_id = hdr.txn_id;
        rec.type   = hdr.type;

        if (hdr.key_len > 0) {
            rec.key.resize(hdr.key_len);
            ssize_t kn = ::pread(fd_, rec.key.data(), hdr.key_len, offset);
            if (kn != static_cast<ssize_t>(hdr.key_len)) {
                throw std::runtime_error(
                    "LogManager::read_all: short read on key at offset "
                    + std::to_string(offset));
            }
            offset += static_cast<off_t>(hdr.key_len);
        }

        if (hdr.before_len > 0) {
            rec.before_val.resize(hdr.before_len);
            rec.before_exists = true;
            ssize_t bn = ::pread(fd_, rec.before_val.data(), hdr.before_len, offset);
            if (bn != static_cast<ssize_t>(hdr.before_len)) {
                throw std::runtime_error(
                    "LogManager::read_all: short read on before_val at offset "
                    + std::to_string(offset));
            }
            offset += static_cast<off_t>(hdr.before_len);
        } else {
            rec.before_exists = false;
        }

        if (hdr.after_len > 0) {
            rec.after_val.resize(hdr.after_len);
            ssize_t an = ::pread(fd_, rec.after_val.data(), hdr.after_len, offset);
            if (an != static_cast<ssize_t>(hdr.after_len)) {
                throw std::runtime_error(
                    "LogManager::read_all: short read on after_val at offset "
                    + std::to_string(offset));
            }
            offset += static_cast<off_t>(hdr.after_len);
        }

        out_records.push_back(std::move(rec));
    }
}

void LogManager::truncate() {
    std::lock_guard<std::mutex> lock(mu_);
    if (::ftruncate(fd_, 0) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "LogManager::truncate: ftruncate failed");
    }
    next_lsn_    = 1;
    next_txn_id_ = 1;
}

} // namespace keyvdb
