#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

struct JSRuntime;

namespace saida {

class JsContext;

class JsRuntime {
public:
    static JsRuntime& instance();

    JsRuntime();
    ~JsRuntime();

    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    std::unique_ptr<JsContext> createContext();
    bool executePendingJobs();
    void beginModuleDependencyCapture(std::vector<std::string>& paths);
    void endModuleDependencyCapture();

    // Every entry into user JavaScript is time-bounded. These methods are used
    // by JsContext's RAII execution scope and support nested native callbacks.
    void beginExecution();
    void endExecution();
    bool shouldInterrupt();

    JSRuntime* raw() const { return runtime_; }

private:
    JSRuntime* runtime_ = nullptr;
    std::chrono::steady_clock::time_point executionDeadline_{};
    unsigned executionDepth_ = 0;
    bool abortRequested_ = false;
};

} // namespace saida
