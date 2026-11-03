// A Host (and its Rendezvous) on a thread of its own, at a fixed step.
//
// Polled from a game's frame loop, the network runs at the frame rate: at
// 5 frames a second, or through a half-second hitch while a level streams,
// datagrams wait in the kernel, acknowledgements and keepalives are late,
// arrivals are timestamped late (a Replica takes that for jitter) and what
// the game sends leaves in bursts. Here the socket wakes the thread the
// moment a datagram arrives, timers run on time, and the game's `step`
// callback runs every `stepSeconds` whatever the frame does: a game sends
// its snapshots from there, at an even rate.
//
// Threading contract: once start()ed, the Host and the Rendezvous belong to
// the thread. The game touches them (send, join, peers, status...) only
// while holding lock(); the Host's receiver, listeners and services, and the
// step callback, run on the thread with the lock held. Hold it briefly: the
// network waits meanwhile. The lock is recursive: game code reached from a
// callback, or from another locked function of its own, may take it again.
#pragma once

#include "saida/p2p/Host.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

namespace saida::p2p {

class Rendezvous;

struct NetworkThreadConfig {
    double stepSeconds = 1.0 / 60.0;  // the step callback's period, and the longest wait
};

struct NetworkThreadStats {
    uint64_t steps = 0, polls = 0;  // fixed steps run; polls (steps and wake-ups by a datagram)
    double worstStep = 0;           // seconds spent in the slowest poll, lock held
    double worstLate = 0;           // the latest a step ran behind its time
};

class NetworkThread {
public:
    using Step = std::function<void(double now)>;

    NetworkThread(Host& host, Rendezvous* meet, DatagramSocket& socket, const NetworkThreadConfig& config = {});
    ~NetworkThread();
    NetworkThread(const NetworkThread&) = delete;
    NetworkThread& operator=(const NetworkThread&) = delete;

    void start(Step step = {});
    void stop();  // joins; the Host and Rendezvous are the caller's again
    bool running() const { return thread_.joinable(); }

    using Lock = std::unique_lock<std::recursive_mutex>;
    Lock lock() { return Lock(mutex_); }
    // The thread's clock (seconds since construction): the `now` the Host is
    // polled with, which the game uses for its own timestamps too.
    double now() const;
    NetworkThreadStats stats();

private:
    void run();

    Host& host_;
    Rendezvous* meet_;
    DatagramSocket& socket_;
    NetworkThreadConfig cfg_;
    Step step_;
    std::recursive_mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::chrono::steady_clock::time_point origin_ = std::chrono::steady_clock::now();
    NetworkThreadStats stats_;
};

} // namespace saida::p2p
