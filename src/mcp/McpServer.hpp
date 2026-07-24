#pragma once

// In-process MCP transport. A background thread accepts a single localhost TCP
// client and reads newline-delimited JSON-RPC 2.0 requests. Each request is
// queued and fulfilled on the MAIN thread by poll() (so every tool runs where
// the scene lives — no locking of engine state). The response line is sent back
// on the client thread once the main thread resolves it.
//
// Editor/dev only (compiled into saida_editor under SAIDA_ENABLE_MCP); never linked
// into the shipped runtime.

#include "nlohmann/json_fwd.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace saida {

class McpServer {
public:
    // Runs on the MAIN thread. Returns the JSON-RPC `result` for the method, or
    // throws std::runtime_error (turned into a JSON-RPC error response).
    using Handler = std::function<nlohmann::json(const std::string& method, const nlohmann::json& params)>;

    McpServer();
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Bind 127.0.0.1:port and start accepting. Returns false on bind failure.
    bool start(uint16_t port);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }

    // Drain queued requests, dispatching each to `handler`. Call once per frame
    // from the main thread.
    void poll(const Handler& handler);

private:
    struct Pending {
        std::string requestLine;             // raw JSON-RPC request, parsed on main thread
        std::promise<std::string> response;  // serialized response line ("" = none)
    };

    void acceptLoop();
    void serveClient(uintptr_t client);

    std::thread thread_;
    std::atomic<bool> running_{false};
    uintptr_t listenSocket_;
    uint16_t port_ = 0;

    std::mutex queueMutex_;
    std::queue<Pending> queue_;
};

} // namespace saida
