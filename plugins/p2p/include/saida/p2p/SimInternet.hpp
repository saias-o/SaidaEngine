// An Internet in a box: public hosts and hosts behind NAT routers of the four
// classic kinds, every line with its latency, jitter, loss and duplication,
// all drawn from a seed. Each host gets a DatagramSocket, so a Host, a DHT
// node or a whole rendezvous runs in one process exactly as it would on the
// network -- including the hole punching that decides whether two players
// behind home routers can reach each other.
//
// NAT behaviour (RFC 4787 terms):
//   FullCone            endpoint-independent mapping and filtering
//   AddressRestricted   endpoint-independent mapping, address-dependent filtering
//   PortRestricted      endpoint-independent mapping, address-and-port filtering
//   Symmetric           a new mapping per destination, address-and-port filtering
// A mapping is created by an outbound datagram and expires `mappingSeconds`
// after the last one. Inbound filtering is decided when a datagram reaches
// the router, so two hosts punching at once really do race.
#pragma once

#include "saida/p2p/Address.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace saida::p2p {

enum class Nat : uint8_t { None, FullCone, AddressRestricted, PortRestricted, Symmetric };

struct Line {
    double latency = 0.02;  // seconds, one way, between this host and the Internet core
    double jitter = 0.0;    // uniform extra delay: reorders
    double loss = 0.0;
    double duplicate = 0.0;
};

class SimInternet {
public:
    explicit SimInternet(uint64_t seed = 1) : rng_(seed ? seed : 1) {}

    // A router with its own public IPv4 address; returns its index.
    int addRouter(Nat type, bool hairpin = false, double mappingSeconds = 120) {
        Router r;
        r.type = type;
        r.hairpin = hairpin;
        r.mappingSeconds = mappingSeconds;
        r.publicIp = Address::v4(100, 70 + uint8_t(routers_.size() / 250), uint8_t(1 + routers_.size() % 250), 1, 0);
        routers_.push_back(r);
        return int(routers_.size() - 1);
    }
    // A host on the open Internet (router -1) or behind a router.
    DatagramSocket& addHost(int router = -1, const Line& line = {}, uint16_t port = 0) {
        auto h = std::make_unique<SimSocket>(*this);
        h->router = router;
        h->line = line;
        const size_t n = hosts_.size() + 1;
        if (router < 0) {
            h->address = Address::v4(198, 18 + uint8_t(n / 62000), uint8_t(n / 250 % 248), uint8_t(1 + n % 250),
                                     port ? port : 6881);
        } else {
            const int inside = ++routers_[size_t(router)].hosts;
            h->address = Address::v4(192, 168, uint8_t(router % 250), uint8_t(1 + inside % 250), port ? port : 5000);
        }
        hosts_.push_back(std::move(h));
        return *hosts_.back();
    }
    Address publicIp(int router) const { return routers_.at(size_t(router)).publicIp; }
    void setLine(DatagramSocket& s, const Line& line) { static_cast<SimSocket&>(s).line = line; }
    // Cuts a host off (its datagrams vanish both ways), or puts it back.
    void setOffline(DatagramSocket& s, bool offline) { static_cast<SimSocket&>(s).offline = offline; }

    void setNow(double now) { now_ = now; }
    double now() const { return now_; }

    uint64_t sent() const { return sent_; }
    uint64_t delivered() const { return delivered_; }
    uint64_t filtered() const { return filtered_; }
    uint64_t lost() const { return lost_; }

private:
    struct Mapping {
        Address inside;       // private host endpoint
        Address destination;  // Symmetric only
        uint16_t port;        // public port
        double lastOut;
        std::vector<Address> contacted;
    };
    struct Router {
        Nat type;
        bool hairpin;
        double mappingSeconds;
        Address publicIp;
        int hosts = 0;
        uint16_t nextPort = 40000;
        std::vector<Mapping> mappings;
    };
    struct Packet {
        double at;
        uint64_t order;
        Address from, to;  // `from` already translated by the sender's router
        int fromRouter;
        Bytes data;
    };
    class SimSocket final : public DatagramSocket {
    public:
        explicit SimSocket(SimInternet& net) : net_(net) {}
        bool send(const Address& to, const uint8_t* data, size_t size) override {
            return net_.transmit(*this, to, data, size);
        }
        int receive(Address& from, uint8_t* buf, size_t capacity) override {
            net_.advance();
            if (inbox.empty()) return -1;
            Packet p = std::move(inbox.front());
            inbox.erase(inbox.begin());
            if (p.data.size() > capacity) return receive(from, buf, capacity);
            from = p.from;
            std::copy(p.data.begin(), p.data.end(), buf);
            return int(p.data.size());
        }
        Address local() const override { return address; }

        SimInternet& net_;
        int router = -1;
        Line line;
        Address address;
        bool offline = false;
        std::vector<Packet> inbox;
    };

    double uniform() {
        rng_ += 0x9e3779b97f4a7c15ULL;
        uint64_t x = rng_;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return double(x >> 11) * (1.0 / 9007199254740992.0);
    }

    Mapping& outbound(Router& r, const Address& inside, const Address& to) {
        for (auto it = r.mappings.begin(); it != r.mappings.end();) {
            if (now_ - it->lastOut > r.mappingSeconds) {
                it = r.mappings.erase(it);
                continue;
            }
            if (it->inside == inside && (r.type != Nat::Symmetric || it->destination == to)) {
                it->lastOut = now_;
                return *it;
            }
            ++it;
        }
        r.mappings.push_back({inside, to, r.nextPort++, now_, {}});
        if (r.nextPort < 40000) r.nextPort = 40000;
        return r.mappings.back();
    }

    bool transmit(SimSocket& s, const Address& to, const uint8_t* data, size_t size) {
        ++sent_;
        if (s.offline) return ++lost_, true;
        Address from = s.address;
        if (s.router >= 0) {
            const bool sameLan = to.ip[0] == 192 && to.ip[1] == 168 && to.ip[2] == uint8_t(s.router % 250);
            if (!sameLan) {
                Router& r = routers_[size_t(s.router)];
                Mapping& m = outbound(r, s.address, to);
                if (std::find(m.contacted.begin(), m.contacted.end(), to) == m.contacted.end())
                    m.contacted.push_back(to);
                from = r.publicIp;
                from.port = m.port;
            }
        }
        SimSocket* dest = nullptr;
        for (auto& h : hosts_)
            if (h->router < 0 && h->address == to) dest = h.get();
        const Line& in = dest ? dest->line : Line{};
        const double loss = 1 - (1 - s.line.loss) * (1 - in.loss);
        if (uniform() < loss) return ++lost_, true;
        const int copies = uniform() < s.line.duplicate ? 2 : 1;
        for (int i = 0; i < copies; ++i) {
            const double at = now_ + s.line.latency + in.latency + s.line.jitter * uniform() + in.jitter * uniform();
            transit_.push_back({at, order_++, from, to, s.router, Bytes(data, data + size)});
        }
        return true;
    }

    // Moves every datagram that has arrived by now into its socket's inbox.
    void advance() {
        if (now_ == advanced_) return;  // nothing sent at `now` can have arrived yet
        advanced_ = now_;
        std::sort(transit_.begin(), transit_.end(), [](const Packet& a, const Packet& b) {
            return a.at != b.at ? a.at < b.at : a.order < b.order;
        });
        size_t done = 0;
        for (; done < transit_.size() && transit_[done].at <= now_; ++done) arrive(std::move(transit_[done]));
        transit_.erase(transit_.begin(), transit_.begin() + long(done));
    }

    void arrive(Packet p) {
        SimSocket* dest = nullptr;
        for (auto& h : hosts_) {
            if (h->router < 0 && h->address == p.to) dest = h.get();
            if (h->router >= 0 && h->address == p.to && p.fromRouter == h->router) dest = h.get();  // same LAN
        }
        if (!dest) {
            for (size_t ri = 0; ri < routers_.size() && !dest; ++ri) {
                Router& r = routers_[ri];
                if (!p.to.sameHost(r.publicIp)) continue;
                if (p.fromRouter == int(ri) && !r.hairpin) return void(++filtered_);
                for (Mapping& m : r.mappings) {
                    if (m.port != p.to.port || now_ - m.lastOut > r.mappingSeconds) continue;
                    bool pass = r.type == Nat::FullCone;
                    for (const Address& c : m.contacted)
                        pass = pass || (r.type == Nat::AddressRestricted ? c.sameHost(p.from) : c == p.from);
                    if (!pass) return void(++filtered_);
                    for (auto& h : hosts_)
                        if (h->router == int(ri) && h->address == m.inside) dest = h.get();
                    break;
                }
            }
        }
        if (!dest || dest->offline) return void(++lost_);
        ++delivered_;
        dest->inbox.push_back(std::move(p));
    }

    std::vector<std::unique_ptr<SimSocket>> hosts_;
    std::vector<Router> routers_;
    std::vector<Packet> transit_;
    uint64_t rng_, order_ = 0, sent_ = 0, delivered_ = 0, filtered_ = 0, lost_ = 0;
    double now_ = 0, advanced_ = -1;
};

} // namespace saida::p2p
