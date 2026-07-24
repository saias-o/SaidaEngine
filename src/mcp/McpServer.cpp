#include "mcp/McpServer.hpp"

#include "core/Log.hpp"

#include "nlohmann/json.hpp"

// Winsock must precede <windows.h>; we only need the socket APIs here.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

namespace saida {

using json = nlohmann::json;

namespace {
bool winsockInit() {
    static bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}

// Read one '\n'-terminated line from the socket. Returns false on disconnect.
bool recvLine(SOCKET sock, std::string& line, std::string& buffer) {
    for (;;) {
        auto nl = buffer.find('\n');
        if (nl != std::string::npos) {
            line = buffer.substr(0, nl);
            buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        char chunk[4096];
        int n = recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(n));
    }
}

bool sendAll(SOCKET sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}
} // namespace

McpServer::McpServer() : listenSocket_(static_cast<uintptr_t>(INVALID_SOCKET)) {}

McpServer::~McpServer() { stop(); }

bool McpServer::start(uint16_t port) {
    if (running_.load()) return true;
    if (!winsockInit()) {
        Log::error("[MCP] WSAStartup failed");
        return false;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        Log::error("[MCP] socket() failed");
        return false;
    }
    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        Log::error("[MCP] bind() to 127.0.0.1:", port, " failed");
        closesocket(listenSock);
        return false;
    }
    if (listen(listenSock, 1) == SOCKET_ERROR) {
        Log::error("[MCP] listen() failed");
        closesocket(listenSock);
        return false;
    }

    listenSocket_ = static_cast<uintptr_t>(listenSock);
    port_ = port;
    running_.store(true);
    thread_ = std::thread([this] { acceptLoop(); });
    Log::info("[MCP] server listening on 127.0.0.1:", port);
    return true;
}

void McpServer::stop() {
    if (!running_.exchange(false)) return;
    SOCKET listenSock = static_cast<SOCKET>(listenSocket_);
    if (listenSock != INVALID_SOCKET) {
        closesocket(listenSock);  // unblocks accept()
        listenSocket_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
    if (thread_.joinable()) thread_.join();
}

void McpServer::acceptLoop() {
    SOCKET listenSock = static_cast<SOCKET>(listenSocket_);
    while (running_.load()) {
        // select() with a timeout so we can notice stop() promptly.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenSock, &readable);
        timeval tv{0, 200000};  // 200 ms
        int r = select(0, &readable, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        serveClient(static_cast<uintptr_t>(client));
    }
}

void McpServer::serveClient(uintptr_t clientHandle) {
    SOCKET client = static_cast<SOCKET>(clientHandle);
    std::string buffer, line;
    Log::info("[MCP] client connected");
    while (running_.load()) {
        if (!recvLine(client, line, buffer)) break;
        if (line.empty()) continue;

        Pending pending;
        pending.requestLine = line;
        auto future = pending.response.get_future();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push(std::move(pending));
        }
        std::string response = future.get();  // resolved by poll() on the main thread
        if (!response.empty() && !sendAll(client, response + "\n")) break;
    }
    closesocket(client);
    Log::info("[MCP] client disconnected");
}

void McpServer::poll(const Handler& handler) {
    std::queue<Pending> local;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        std::swap(local, queue_);
    }

    while (!local.empty()) {
        Pending& p = local.front();
        std::string responseLine;

        json id = nullptr;
        bool isNotification = true;
        try {
            json req = json::parse(p.requestLine);
            if (req.contains("id")) { id = req["id"]; isNotification = false; }
            std::string method = req.value("method", std::string());
            json params = req.contains("params") ? req["params"] : json::object();

            json result = handler(method, params);
            if (!isNotification) {
                json resp{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
                responseLine = resp.dump();
            }
        } catch (const std::exception& e) {
            if (!isNotification) {
                json resp{{"jsonrpc", "2.0"}, {"id", id},
                          {"error", {{"code", -32000}, {"message", e.what()}}}};
                responseLine = resp.dump();
            } else {
                Log::warn("[MCP] notification error: ", e.what());
            }
        }

        p.response.set_value(responseLine);
        local.pop();
    }
}

} // namespace saida
