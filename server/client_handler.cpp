#include "server/client_handler.h"
#include "server/protocol.h"
#include "txn/lock_manager.h"

#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace keyvdb {

using namespace proto;

ClientHandler::ClientHandler(int conn_fd, DB& db)
    : conn_fd_(conn_fd), db_(db)
{}

// ── I/O helpers ───────────────────────────────────────────────────────────────

bool ClientHandler::read_exact(void* buf, size_t n) {
    auto* p = static_cast<char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t r = ::read(conn_fd_, p, remaining);
        if (r <= 0) return false;  // EOF or error
        p += r;
        remaining -= static_cast<size_t>(r);
    }
    return true;
}

bool ClientHandler::write_exact(const void* buf, size_t n) {
    const auto* p = static_cast<const char*>(buf);
    size_t remaining = n;
    while (remaining > 0) {
        ssize_t w = ::write(conn_fd_, p, remaining);
        if (w <= 0) return false;
        p += w;
        remaining -= static_cast<size_t>(w);
    }
    return true;
}

bool ClientHandler::read_string(std::string& out) {
    uint16_t net_len = 0;
    if (!read_exact(&net_len, sizeof(net_len))) return false;
    uint16_t len = ntohs(net_len);
    out.resize(len);
    if (len > 0 && !read_exact(out.data(), len)) return false;
    return true;
}

// ── Response helpers ──────────────────────────────────────────────────────────

void ClientHandler::send_response(uint8_t status,
                                   const char* payload, uint32_t payload_len) {
    // Envelope: [opcode(1)] [payload_len(4)] [payload]
    // We reuse the status byte as the "opcode" of the response.
    uint32_t net_len = htonl(payload_len);
    write_exact(&status,   1);
    write_exact(&net_len,  4);
    if (payload_len > 0) write_exact(payload, payload_len);
}

void ClientHandler::send_ok() {
    send_response(STATUS_OK, nullptr, 0);
}

void ClientHandler::send_ok_value(const std::string& value) {
    // Payload: [uint16_t val_len][val_bytes]
    uint16_t net_len = htons(static_cast<uint16_t>(value.size()));
    std::string payload;
    payload.resize(sizeof(net_len) + value.size());
    std::memcpy(payload.data(), &net_len, sizeof(net_len));
    std::memcpy(payload.data() + sizeof(net_len), value.data(), value.size());
    send_response(STATUS_OK, payload.data(), static_cast<uint32_t>(payload.size()));
}

void ClientHandler::send_not_found() {
    send_response(STATUS_NOT_FOUND, nullptr, 0);
}

void ClientHandler::send_error(const std::string& msg) {
    send_response(STATUS_ERROR, msg.c_str(), static_cast<uint32_t>(msg.size() + 1));
}

void ClientHandler::send_deadlock() {
    send_response(STATUS_DEADLOCK, nullptr, 0);
}

// ── Operation handlers ────────────────────────────────────────────────────────

void ClientHandler::handle_begin() {
    if (txn_) {
        send_error("transaction already open — COMMIT or ROLLBACK first");
        return;
    }
    txn_ = db_.Begin();
    send_ok();
}

void ClientHandler::handle_get(uint32_t /*payload_len*/) {
    if (!txn_) { send_error("no open transaction — send BEGIN first"); return; }

    std::string key;
    if (!read_string(key)) return;

    try {
        auto v = txn_->Get(key);
        if (v) send_ok_value(*v);
        else   send_not_found();
    } catch (const DeadlockException& e) {
        txn_.reset();
        send_deadlock();
    } catch (const std::exception& e) {
        send_error(e.what());
    }
}

void ClientHandler::handle_put(uint32_t /*payload_len*/) {
    if (!txn_) { send_error("no open transaction"); return; }

    std::string key, value;
    if (!read_string(key) || !read_string(value)) return;

    try {
        txn_->Put(key, value);
        send_ok();
    } catch (const DeadlockException&) {
        txn_.reset();
        send_deadlock();
    } catch (const std::exception& e) {
        send_error(e.what());
    }
}

void ClientHandler::handle_delete(uint32_t /*payload_len*/) {
    if (!txn_) { send_error("no open transaction"); return; }

    std::string key;
    if (!read_string(key)) return;

    try {
        txn_->Delete(key);
        send_ok();
    } catch (const DeadlockException&) {
        txn_.reset();
        send_deadlock();
    } catch (const std::exception& e) {
        send_error(e.what());
    }
}

void ClientHandler::handle_commit() {
    if (!txn_) { send_error("no open transaction"); return; }
    try {
        txn_->Commit();
        txn_.reset();
        send_ok();
    } catch (const std::exception& e) {
        txn_.reset();
        send_error(e.what());
    }
}

void ClientHandler::handle_rollback() {
    if (!txn_) { send_error("no open transaction"); return; }
    try {
        txn_->Rollback();
    } catch (...) {}
    txn_.reset();
    send_ok();
}

// ── Main request loop ─────────────────────────────────────────────────────────

void ClientHandler::run() {
    while (true) {
        // Read the 5-byte message header: [opcode(1)][payload_len(4)]
        uint8_t  opcode = 0;
        uint32_t net_payload_len = 0;

        if (!read_exact(&opcode, 1)) break;
        if (!read_exact(&net_payload_len, 4)) break;
        uint32_t payload_len = ntohl(net_payload_len);

        switch (opcode) {
            case OP_BEGIN:    handle_begin();              break;
            case OP_GET:      handle_get(payload_len);     break;
            case OP_PUT:      handle_put(payload_len);     break;
            case OP_DELETE:   handle_delete(payload_len);  break;
            case OP_COMMIT:   handle_commit();             break;
            case OP_ROLLBACK: handle_rollback();           break;
            default:
                send_error("unknown opcode");
                break;
        }
    }

    // Client disconnected — clean up any open transaction.
    if (txn_) {
        try { txn_->Rollback(); } catch (...) {}
        txn_.reset();
    }
    ::close(conn_fd_);
}

} // namespace keyvdb
