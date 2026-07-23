#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "server/server.h"
#include "server/protocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// server/main.cpp — Entry point for the KeyVDB TCP server.
//
// Usage:
//   keyvdb-server [--db <path>] [--port <port>]
//
// Defaults:
//   --db   ./keyvdb.db
//   --port 6380
//
// Signals: SIGINT / SIGTERM trigger a clean shutdown.
// ─────────────────────────────────────────────────────────────────────────────

static keyvdb::Server* g_server = nullptr;

static void signal_handler(int /*sig*/) {
    std::cout << "\nShutting down KeyVDB server..." << std::endl;
    if (g_server) g_server->stop();
}

int main(int argc, char* argv[]) {
    std::string db_path = "keyvdb.db";
    int         port    = keyvdb::proto::DEFAULT_PORT;

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--db")   db_path = argv[i + 1];
        if (std::string(argv[i]) == "--port") port    = std::stoi(argv[i + 1]);
    }

    std::cout << "KeyVDB " << "Phase 2" << std::endl;
    std::cout << "Database: " << db_path << std::endl;

    ::signal(SIGINT,  signal_handler);
    ::signal(SIGTERM, signal_handler);

    try {
        keyvdb::Server server(db_path, port);
        g_server = &server;
        server.run();
        g_server = nullptr;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Server stopped cleanly." << std::endl;
    return 0;
}
