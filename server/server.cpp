#include "server/server.h"
#include "server/client_handler.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace keyvdb {

Server::Server(const std::string& db_path, int port)
    : port_(port)
{
    db_ = DB::Open(db_path);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("Server: failed to create socket");
    }

    // Allow immediate rebind after restart.
    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        throw std::runtime_error("Server: failed to bind to port "
                                 + std::to_string(port_));
    }

    if (::listen(listen_fd_, 128) < 0) {
        ::close(listen_fd_);
        throw std::runtime_error("Server: listen() failed");
    }

    std::cout << "KeyVDB server listening on port " << port_ << std::endl;
}

Server::~Server() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
    if (db_) db_->Close();
}

void Server::run() {
    while (!stopping_.load()) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);

        int conn_fd = ::accept(listen_fd_,
                               reinterpret_cast<sockaddr*>(&client_addr),
                               &client_len);
        if (conn_fd < 0) {
            if (stopping_.load()) break;
            // Transient error — log and continue.
            std::cerr << "Server: accept() error, continuing\n";
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "Client connected: " << client_ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // Each connection gets its own detached thread.
        // ClientHandler owns conn_fd and closes it when done.
        std::thread([conn_fd, this] {
            ClientHandler handler(conn_fd, *db_);
            handler.run();
        }).detach();
    }
}

void Server::stop() {
    stopping_ = true;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

} // namespace keyvdb
