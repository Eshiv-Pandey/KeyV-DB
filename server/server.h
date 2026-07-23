#pragma once

#include <string>
#include <atomic>
#include "db.h"

namespace keyvdb {

// ─────────────────────────────────────────────────────────────────────────────
// server.h — TCP server wrapping the KeyVDB embedded library.
//
// Server listens on a configurable port, accepts connections, and spawns a
// detached thread per connection running a ClientHandler.
//
// Lifecycle:
//   1. Server::Server(db_path, port) — opens the DB, sets up the listen socket.
//   2. Server::run()                 — accept loop (blocks until stop() called).
//   3. Server::stop()                — closes the listen socket, signals run() to exit.
//
// Thread model: one thread per client connection (detached). The DB object is
// shared; its internal mutexes handle concurrent access correctly.
// ─────────────────────────────────────────────────────────────────────────────

class Server {
public:
    // Open the database at db_path, bind to the given port.
    // Throws std::runtime_error if the port can't be bound.
    explicit Server(const std::string& db_path, int port = 6380);

    ~Server();

    // Accept connections and dispatch to ClientHandler threads.
    // Blocks until stop() is called or a fatal error occurs.
    void run();

    // Close the listen socket, causing run() to return.
    void stop();

private:
    std::unique_ptr<DB>  db_;
    int                  listen_fd_ = -1;
    int                  port_;
    std::atomic<bool>    stopping_{false};
};

} // namespace keyvdb
