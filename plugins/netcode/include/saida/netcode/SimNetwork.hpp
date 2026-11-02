// A network in a box: any number of endpoints exchanging datagrams through
// links with latency, jitter, loss, duplication and reordering, all drawn from
// a seed. It implements Transport, so a whole multiplayer session runs in one
// process, deterministically, without a socket -- the bench every rollback
// game should be tested on before a real network.
//
// Reliable messages are never lost or duplicated and arrive in order per
// link, as the Transport contract requires; a cut link drops everything.
#pragma once

#include "saida/netcode/Session.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace saida::netcode {

struct LinkConditions {
    double latency = 0.02;    // seconds, one way
    double jitter = 0.0;      // seconds of uniform extra delay: reorders packets
    double loss = 0.0;        // probability an unreliable packet is dropped
    double duplicate = 0.0;   // probability an unreliable packet arrives twice
};

class SimNetwork {
public:
    explicit SimNetwork(uint64_t seed = 1) : rng_(seed ? seed : 1) {}

    // A new endpoint; its PeerId is its address on this network.
    PeerId add() {
        ports_.push_back(std::make_unique<Port>(*this, PeerId(ports_.size())));
        return PeerId(ports_.size() - 1);
    }
    Transport& transport(PeerId endpoint) { return *ports_.at(endpoint); }

    void setDefault(const LinkConditions& c) { default_ = c; }
    void setLink(PeerId from, PeerId to, const LinkConditions& c) { link(from, to).conditions = c, link(from, to).custom = true; }
    void cut(PeerId a, PeerId b, bool cut) { link(a, b).cut = cut, link(b, a).cut = cut; }
    // Cuts every link of an endpoint (a crash, a cable pulled).
    void isolate(PeerId a, bool cut) {
        for (PeerId b = 0; b < PeerId(ports_.size()); ++b)
            if (b != a) this->cut(a, b, cut);
    }

    // Packets sent from now on leave at this time.
    void setNow(double now) { now_ = now; }

    // Hands every packet due at `now` for `endpoint` to f(from, data, size),
    // in arrival order.
    template <class F>
    void deliver(PeerId endpoint, double now, F&& f) {
        due_.clear();
        for (size_t i = 0; i < queue_.size();) {
            if (queue_[i].to == endpoint && queue_[i].at <= now) {
                due_.push_back(std::move(queue_[i]));
                queue_[i] = std::move(queue_.back());
                queue_.pop_back();
            } else {
                ++i;
            }
        }
        std::sort(due_.begin(), due_.end(), [](const Packet& a, const Packet& b) {
            return a.at != b.at ? a.at < b.at : a.order < b.order;
        });
        for (const Packet& p : due_) {
            ++delivered_;
            f(p.from, p.data.data(), p.data.size());
        }
    }

    uint64_t sent() const { return sent_; }
    uint64_t lost() const { return lost_; }
    uint64_t duplicated() const { return duplicated_; }
    uint64_t delivered() const { return delivered_; }
    size_t inFlight() const { return queue_.size(); }

private:
    struct Packet {
        double at;
        uint64_t order;
        PeerId from, to;
        std::vector<uint8_t> data;
    };
    struct Link {
        PeerId from, to;
        LinkConditions conditions;
        bool custom = false, cut = false;
        double lastReliable = 0;
    };
    class Port : public Transport {
    public:
        Port(SimNetwork& net, PeerId self) : net_(net), self_(self) {}
        void send(PeerId to, const uint8_t* data, size_t size, Delivery delivery) override {
            net_.enqueue(self_, to, data, size, delivery);
        }

    private:
        SimNetwork& net_;
        PeerId self_;
    };

    Link& link(PeerId from, PeerId to) {
        for (Link& l : links_)
            if (l.from == from && l.to == to) return l;
        links_.push_back({from, to, default_});
        return links_.back();
    }

    double uniform() {
        rng_ += 0x9e3779b97f4a7c15ULL;
        return double(mix64(rng_) >> 11) * (1.0 / 9007199254740992.0);
    }

    void enqueue(PeerId from, PeerId to, const uint8_t* data, size_t size, Delivery delivery) {
        ++sent_;
        if (to >= ports_.size()) return void(++lost_);
        Link& l = link(from, to);
        const LinkConditions& c = l.custom ? l.conditions : default_;
        if (l.cut) return void(++lost_);
        const bool reliable = delivery == Delivery::Reliable;
        if (!reliable && uniform() < c.loss) return void(++lost_);
        const int copies = !reliable && uniform() < c.duplicate ? 2 : 1;
        duplicated_ += copies - 1;
        for (int i = 0; i < copies; ++i) {
            double at = now_ + c.latency + c.jitter * uniform();
            if (reliable) at = l.lastReliable = std::max(at, l.lastReliable);
            queue_.push_back({at, order_++, from, to, std::vector<uint8_t>(data, data + size)});
        }
    }

    std::vector<std::unique_ptr<Port>> ports_;
    std::vector<Link> links_;
    std::vector<Packet> queue_, due_;
    LinkConditions default_;
    uint64_t rng_;
    uint64_t order_ = 0, sent_ = 0, lost_ = 0, duplicated_ = 0, delivered_ = 0;
    double now_ = 0;
};

} // namespace saida::netcode
