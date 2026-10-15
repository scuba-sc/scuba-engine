#pragma once
// scuba-hostd's single-threaded HTTP + WebSocket server. The owner calls Poll in its main loop, so
// every handler runs on that thread. Serves static files from staticDir and the JSON bridge on
// GET /ws.
//
// Browser access is limited to the hostd origin: requests must carry a Host naming this server
// (loopback names or the bind address, with the port), which defeats DNS rebinding, and a
// WebSocket upgrade that carries an Origin must name an allowed origin (the hostd origin itself, or
// one added with --allow-origin), which stops other web pages from driving the bridge.
#include "hostd/handlers.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace scuba::hostd {

struct ServerOptions {
    std::string bindAddress = "127.0.0.1";   // IPv4 literal
    uint16_t port = 7878;                    // 0: an ephemeral port (see Server::port)
    std::filesystem::path staticDir;         // studio/dist; empty or missing: a built-in notice page
    std::vector<std::string> allowedOrigins; // extra WebSocket origins, e.g. "http://localhost:5173"
    size_t maxMessageBytes = 16u * 1024u * 1024u;
};

class Server {
public:
    Server(ServerOptions options, const Registry& registry, Broadcaster& broadcaster);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and listens. False (and *error set) when the address or port is unusable.
    bool Start(std::string* error);
    uint16_t port() const;
    // One loop step: waits up to timeoutMs for socket activity, then accepts, reads, dispatches,
    // sends queued broadcasts and writes.
    void Poll(int timeoutMs);
    size_t client_count() const;
    // Sends a close frame to every WebSocket client and closes every socket.
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scuba::hostd
