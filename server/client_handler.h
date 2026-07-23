#pragma once

#include <memory>
#include "db.h"
#include "server/protocol.h"

namespace keyvdb {

// ─────────────────────────────────────────────────────────────────────────────
// client_handler.h — Per-connection session handler.
//
// One ClientHandler is created per accepted TCP connection. It owns a single
// transaction slot (at most one open transaction per connection at a time)
// and processes the request/response loop until the client disconnects.
//
// Each ClientHandler runs in its own thread (spawned by Server).
// The DB object is shared across all handlers and is thread-safe.
// ─────────────────────────────────────────────────────────────────────────────

class ClientHandler {
public:
    ClientHandler(int conn_fd, DB& db);

    // Run the request loop for this connection.
    // Returns when the client disconnects or a fatal I/O error occurs.
    void run();

private:
    int  conn_fd_;
    DB&  db_;
    std::unique_ptr<Transaction> txn_;  // null when no transaction is open

    // ── I/O helpers ───────────────────────────────────────────────────────────

    // Read exactly `n` bytes into buf. Returns false on EOF or error.
    bool read_exact(void* buf, size_t n);

    // Write exactly `n` bytes from buf. Returns false on error.
    bool write_exact(const void* buf, size_t n);

    // ── Request parsing ───────────────────────────────────────────────────────

    // Read a length-prefixed string (uint16_t len + bytes).
    bool read_string(std::string& out);

    // ── Response builders ─────────────────────────────────────────────────────

    void send_ok();
    void send_ok_value(const std::string& value);
    void send_not_found();
    void send_error(const std::string& msg);
    void send_deadlock();

    // Send a complete response envelope: [status][4-byte payload_len][payload].
    void send_response(uint8_t status,
                       const char* payload, uint32_t payload_len);

    // ── Handlers ──────────────────────────────────────────────────────────────
    void handle_begin();
    void handle_get(uint32_t payload_len);
    void handle_put(uint32_t payload_len);
    void handle_delete(uint32_t payload_len);
    void handle_commit();
    void handle_rollback();
};

} // namespace keyvdb
