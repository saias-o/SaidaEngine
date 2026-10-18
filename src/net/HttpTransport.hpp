#pragma once
#include "net/HttpRequest.hpp"
#include <atomic>
#include <functional>
namespace saida::http {
// Worker-only streaming transport. Receive applies backpressure; false aborts.
// The zero timeout is reserved for a live stream (connect/stall stay bounded).
HttpResponse receive(const std::string& url,double timeout,const std::atomic<bool>& cancelled,
    const std::function<bool(const char*,size_t)>& write);
}
