#pragma once

#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace saida {

// Minimal leveled logger. Header-only; streams its arguments to stdout/stderr
// behind a tag. Also keeps a small in-memory ring of recent lines so tools (the
// MCP `read_logs`) can surface what the engine just printed without scraping a
// console. Replace the sink here later (file, callback) without touching call
// sites: Log::info("loaded ", n, " vertices");
class Log {
public:
    template <typename... Args>
    static void info(Args&&... args) { line(std::cout, "[info] ", std::forward<Args>(args)...); }

    template <typename... Args>
    static void warn(Args&&... args) { line(std::cerr, "[warn] ", std::forward<Args>(args)...); }

    template <typename... Args>
    static void error(Args&&... args) { line(std::cerr, "[error] ", std::forward<Args>(args)...); }

    // The most recent log lines (newest last), capped at `count`.
    static std::vector<std::string> recent(size_t count = 100) {
        std::lock_guard<std::mutex> lock(bufferMutex());
        return recentUnlocked(count);
    }

    // Crash reporting cannot wait on a logger mutex potentially held by the
    // failing thread. Return an empty snapshot when the ring is unavailable.
    static std::vector<std::string> recentNonBlocking(size_t count = 100) {
        std::unique_lock<std::mutex> lock(bufferMutex(), std::try_to_lock);
        if (!lock.owns_lock()) return {};
        return recentUnlocked(count);
    }

private:
    static std::vector<std::string> recentUnlocked(size_t count) {
        auto& buf = buffer();
        std::vector<std::string> out;
        size_t n = count < buf.size() ? count : buf.size();
        out.reserve(n);
        for (size_t i = buf.size() - n; i < buf.size(); ++i) out.push_back(buf[i]);
        return out;
    }

    static constexpr size_t kRingCapacity = 512;

    static std::deque<std::string>& buffer() {
        static std::deque<std::string> ring;
        return ring;
    }
    static std::mutex& bufferMutex() {
        static std::mutex m;
        return m;
    }

    static void capture(const char* tag, const std::string& message) {
        std::lock_guard<std::mutex> lock(bufferMutex());
        auto& buf = buffer();
        buf.push_back(std::string(tag) + message);
        while (buf.size() > kRingCapacity) buf.pop_front();
    }

    template <typename... Args>
    static void line(std::ostream& os, const char* tag, Args&&... args) {
        std::ostringstream ss;
        (ss << ... << args);  // C++17 fold
        const std::string message = ss.str();
        os << tag << message << std::endl;
        capture(tag, message);
    }
};

} // namespace saida
