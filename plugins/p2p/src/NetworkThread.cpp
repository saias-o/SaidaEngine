#include "saida/p2p/NetworkThread.hpp"

#include "saida/p2p/Rendezvous.hpp"

#include <algorithm>

#ifdef _WIN32
// The system timer ticks every 15.6 ms unless asked for 1 ms (the engine asks
// too, for frame pacing): a wait of one step must not last two.
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int period);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int period);
#endif

namespace saida::p2p {

NetworkThread::NetworkThread(Host& host, Rendezvous* meet, DatagramSocket& socket, const NetworkThreadConfig& config)
    : host_(host), meet_(meet), socket_(socket), cfg_(config) {
    cfg_.stepSeconds = std::clamp(cfg_.stepSeconds, 0.001, 0.5);
}

NetworkThread::~NetworkThread() { stop(); }

void NetworkThread::start(Step step) {
    if (thread_.joinable()) return;
    step_ = std::move(step);
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void NetworkThread::stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
}

double NetworkThread::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin_).count();
}

NetworkThreadStats NetworkThread::stats() {
    auto held = lock();
    return stats_;
}

void NetworkThread::run() {
#ifdef _WIN32
    const bool fine = timeBeginPeriod(1) == 0;
#endif
    double next = now();
    while (!stop_) {
        // Until the next step, or the first datagram: arrivals are read and
        // stamped when they land, not at the next step.
        const double wait = next - now();
        if (wait > 0) socket_.wait(wait);
        auto held = lock();
        const double t = now();
        host_.poll(t);
        if (meet_) meet_->poll(t);
        if (t >= next) {
            stats_.worstLate = std::max(stats_.worstLate, t - next);
            if (step_) step_(t);
            ++stats_.steps;
            // Evenly spaced on average: a late step is caught up at once; a
            // thread a quarter of a second behind (suspended) starts afresh.
            next += cfg_.stepSeconds;
            if (next < t - 0.25) next = t + cfg_.stepSeconds;
        }
        host_.flush();
        ++stats_.polls;
        stats_.worstStep = std::max(stats_.worstStep, now() - t);
    }
#ifdef _WIN32
    if (fine) timeEndPeriod(1);
#endif
}

} // namespace saida::p2p
