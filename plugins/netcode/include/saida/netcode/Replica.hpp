// State replication: objects that one machine owns and every other machine
// draws from the owner's snapshots. The other model beside the rollback
// Session, for games whose shared state cannot be one deterministic
// simulation -- an open world where each machine streams its own collision,
// runs its own crowds and its own physics -- but where each object has an
// obvious owner: a player is the authority on its own avatar.
//
//   RemoteClock  the sender's clock seen from here. Every snapshot carries the
//                time it was sent; arrival minus sending time is the clock
//                offset plus the one-way delay. The smallest of these over a
//                window is the offset plus the fastest path; how far samples
//                lie above it is the jitter, its peak over the window the
//                spread. No ping, no clock exchange.
//   Replica      a remote object drawn a little in the past, between two
//                snapshots it really had, so it moves smoothly however the
//                packets arrive. The delay follows the line like a jitter
//                buffer: the gap between two snapshots sent that 80 % of the
//                recent ones do not exceed, plus the spread, both over the
//                last window, between `minDelay` and `maxDelay`. An owner whose
//                frames stutter is covered by his recent stutters; a snapshot
//                lost now and then is bridged by a short extrapolation rather
//                than paid for by a longer delay for everyone.
//                The playback clock slews (a few percent faster or slower)
//                towards its target instead of jumping, so a change of delay
//                is never a visible jerk. When snapshots stop, the object is
//                extrapolated for at most `maxExtrapolation`, then held.
//   History      the owner's own recent states, by its own clock. A remote
//                machine that acts on what it drew (a shot at an avatar) says
//                which instant of the owner's it drew -- a time in the owner's
//                clock, the Replica's playback() -- and the owner checks the
//                claim against where it really was then: lag compensation with
//                the owner as the judge, and no clock to synchronize.
//
// Header-only, standard library only. No clock is read here: the host passes
// its time, so tests drive it exactly. `State` is any copyable value; the
// caller's lerp(a, b, t) blends two of them, with t > 1 meaning beyond b
// (extrapolation: positions continue along a to b, discrete fields take b's).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <utility>
#include <vector>

namespace saida::netcode {

class RemoteClock {
public:
    explicit RemoteClock(double windowSeconds = 4.0) : window_(windowSeconds > 0.0 ? windowSeconds : 4.0) {}

    // A message stamped `remoteTime` (the sender's seconds) arrived at `localTime`.
    void observe(double remoteTime, double localTime) {
        if (!std::isfinite(remoteTime) || !std::isfinite(localTime)) return;
        const double sample = localTime - remoteTime;
        if (!known_) {
            buckets_.fill({localTime, sample, sample});
            known_ = true;
            offset_ = sample;
            jitter_ = 0.0;
            return;
        }
        // The minimum over the window, in kBuckets slices: the oldest slice
        // ages out as a new one starts, so a clock drifting or a route that
        // got slower is followed within one window.
        if (localTime - buckets_[0].start >= window_ / double(kBuckets)) {
            for (size_t i = kBuckets - 1; i > 0; --i) buckets_[i] = buckets_[i - 1];
            buckets_[0] = {localTime, sample, sample};
        } else {
            buckets_[0].least = std::min(buckets_[0].least, sample);
            buckets_[0].most = std::max(buckets_[0].most, sample);
        }
        offset_ = buckets_[0].least;
        double most = buckets_[0].most;
        for (const Bucket& b : buckets_) offset_ = std::min(offset_, b.least), most = std::max(most, b.most);
        spread_ = most - offset_;
        jitter_ += (std::max(0.0, sample - offset_) - jitter_) / 16.0;
    }

    bool known() const { return known_; }
    // When a message stamped `remoteTime` arrives by the fastest path.
    double toLocal(double remoteTime) const { return remoteTime + offset_; }
    // The sender's time of a message that would arrive now by the fastest path.
    double toRemote(double localTime) const { return localTime - offset_; }
    // Mean lateness of arrivals behind the fastest path, smoothed (seconds).
    double jitter() const { return jitter_; }
    // The latest arrival behind the fastest path over the window (seconds).
    double spread() const { return spread_; }
    double window() const { return window_; }
    void reset() { known_ = false, offset_ = 0.0, jitter_ = 0.0, spread_ = 0.0; }

private:
    static constexpr size_t kBuckets = 4;
    struct Bucket {
        double start, least, most;
    };
    double window_;
    std::array<Bucket, kBuckets> buckets_{};
    bool known_ = false;
    double offset_ = 0.0, jitter_ = 0.0, spread_ = 0.0;
};

struct ReplicaConfig {
    double minDelay = 0.02;          // seconds behind the fastest path, at best
    double maxDelay = 0.4;           // and at worst
    double maxExtrapolation = 0.25;  // seconds drawn past the newest snapshot before holding
    double slew = 0.08;              // playback runs at most this much faster or slower than time
    double snap = 0.3;               // a playback this far from its target jumps to it
    size_t capacity = 64;            // snapshots kept
};

enum class ReplicaSample : uint8_t {
    Empty,         // no snapshot yet
    Interpolated,  // between two snapshots the owner really had
    Extrapolated,  // past the newest, within maxExtrapolation
    Held,          // before the oldest, or past maxExtrapolation: frozen
};

template <class State>
class Replica {
public:
    struct Snapshot {
        double sent;
        State state;
    };

    explicit Replica(const ReplicaConfig& config = {}) : cfg_(config) {}

    // A snapshot stamped `sent` (the owner's seconds) arrived at `now`. False
    // for a duplicate or for one older than what is already drawn.
    bool push(double sent, double now, const State& state) {
        if (!std::isfinite(sent) || !std::isfinite(now)) return false;
        clock_.observe(sent, now);
        lastArrival_ = now;
        if (started_ && sent <= playback_ && !buffer_.empty() && sent <= buffer_.front().sent) return false;
        auto at = std::lower_bound(buffer_.begin(), buffer_.end(), sent,
                                   [](const Snapshot& s, double t) { return s.sent < t; });
        if (at != buffer_.end() && at->sent == sent) return false;
        if (at == buffer_.end() && !buffer_.empty()) {
            // The gaps the owner left over the window: a stutter of his is
            // remembered for one window, then forgotten. A gap longer than
            // maxDelay is an outage no delay could cover, not jitter.
            const double gap = sent - buffer_.back().sent;
            if (gap <= cfg_.maxDelay) gaps_.push_back({now, gap});
            while (gaps_.size() > 1 && gaps_.front().first < now - clock_.window()) gaps_.pop_front();
            if (gaps_.size() > 1024) gaps_.pop_front();
            std::vector<double> values;
            values.reserve(gaps_.size());
            for (const auto& g : gaps_) values.push_back(g.second);
            if (values.empty()) gap_ = 0.0;
            else {
                const auto at = values.begin() + std::ptrdiff_t(values.size() * 4 / 5);
                std::nth_element(values.begin(), at == values.end() ? at - 1 : at, values.end());
                gap_ = *(at == values.end() ? at - 1 : at);
            }
        }
        buffer_.insert(at, Snapshot{sent, state});
        while (buffer_.size() > std::max<size_t>(2, cfg_.capacity)) buffer_.pop_front();
        return true;
    }

    // The state to draw at `now`, into `out`.
    template <class Lerp>
    ReplicaSample sample(double now, State& out, Lerp&& lerp) {
        if (buffer_.empty() || !std::isfinite(now)) return ReplicaSample::Empty;
        const double target = clock_.toRemote(now) - targetDelay();
        if (!started_ || !(std::fabs(target - playback_) <= cfg_.snap)) {
            playback_ = target;
            started_ = true;
        } else {
            const double dt = std::clamp(now - lastSample_, 0.0, 1.0);
            const double rate = 1.0 + std::clamp((target - playback_) * 4.0, -cfg_.slew, cfg_.slew);
            playback_ += dt * rate;
        }
        lastSample_ = now;
        // Keep one snapshot at or before the playback, for the next blend.
        while (buffer_.size() > 2 && buffer_[1].sent <= playback_) buffer_.pop_front();
        const Snapshot& a = buffer_.front();
        if (buffer_.size() == 1 || playback_ <= a.sent) {
            out = a.state;
            return ReplicaSample::Held;
        }
        const Snapshot& b = buffer_[1];
        const double span = b.sent - a.sent;
        double t = (playback_ - a.sent) / span;
        ReplicaSample kind = ReplicaSample::Interpolated;
        if (t > 1.0) {
            kind = ReplicaSample::Extrapolated;
            if (playback_ - b.sent > cfg_.maxExtrapolation) {
                t = 1.0 + cfg_.maxExtrapolation / span;
                kind = ReplicaSample::Held;
            }
        }
        out = lerp(a.state, b.state, t);
        return kind;
    }

    // The delay the line asks for now (seconds behind the fastest path): the
    // usual recent gap between snapshots plus the latest recent arrival.
    double targetDelay() const {
        return std::clamp(cfg_.minDelay + gap_ + clock_.spread(), cfg_.minDelay, std::max(cfg_.minDelay, cfg_.maxDelay));
    }
    // The owner's instant drawn by the last sample(): what to name in a claim.
    double playback() const { return playback_; }
    // How far the drawing runs behind the newest snapshot (negative: ahead of it).
    double lag() const { return buffer_.empty() ? 0.0 : buffer_.back().sent - playback_; }
    const State* newest() const { return buffer_.empty() ? nullptr : &buffer_.back().state; }
    double newestSent() const { return buffer_.empty() ? -std::numeric_limits<double>::infinity() : buffer_.back().sent; }
    // Local time of the last arrival (-inf before any).
    double lastArrival() const { return lastArrival_; }
    size_t size() const { return buffer_.size(); }
    const RemoteClock& clock() const { return clock_; }
    // Forgets everything (the owner restarted, or teleported far away).
    void clear() {
        buffer_.clear();
        clock_.reset();
        started_ = false;
        gaps_.clear();
        gap_ = 0.0;
        lastArrival_ = -std::numeric_limits<double>::infinity();
    }

private:
    ReplicaConfig cfg_;
    RemoteClock clock_;
    std::deque<Snapshot> buffer_;
    bool started_ = false;
    std::deque<std::pair<double, double>> gaps_;  // (arrival, gap) over the clock's window
    double gap_ = 0.0;  // the 80th percentile of gaps_
    double playback_ = 0.0, lastSample_ = 0.0;
    double lastArrival_ = -std::numeric_limits<double>::infinity();
};

template <class State>
class History {
public:
    explicit History(double seconds = 1.0, size_t capacity = 512)
        : seconds_(seconds > 0.0 ? seconds : 1.0), capacity_(std::max<size_t>(2, capacity)) {}

    // The owner's state at its own `time`; times only grow (an equal or earlier one is ignored).
    void record(double time, const State& state) {
        if (!std::isfinite(time) || (!entries_.empty() && time <= entries_.back().time)) return;
        entries_.push_back({time, state});
        while (entries_.size() > capacity_ || (entries_.size() > 2 && entries_[1].time < time - seconds_))
            entries_.pop_front();
    }

    // The state at `time`, blended between the two records around it. False
    // outside what is kept: too old to judge, or not lived yet.
    template <class Lerp>
    bool at(double time, State& out, Lerp&& lerp) const {
        if (entries_.empty() || !(time >= entries_.front().time) || !(time <= entries_.back().time)) return false;
        auto b = std::lower_bound(entries_.begin(), entries_.end(), time,
                                  [](const Entry& e, double t) { return e.time < t; });
        if (b->time == time || b == entries_.begin()) {
            out = b->state;
            return true;
        }
        auto a = b - 1;
        out = lerp(a->state, b->state, (time - a->time) / (b->time - a->time));
        return true;
    }

    double oldest() const { return entries_.empty() ? 0.0 : entries_.front().time; }
    double newest() const { return entries_.empty() ? 0.0 : entries_.back().time; }
    void clear() { entries_.clear(); }

private:
    struct Entry {
        double time;
        State state;
    };
    double seconds_;
    size_t capacity_;
    std::deque<Entry> entries_;
};

} // namespace saida::netcode
