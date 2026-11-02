#include "saida/p2p/Dht.hpp"

#include "saida/p2p/Bencode.hpp"
#include "saida/p2p/Crypto.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace saida::p2p {
namespace {

enum State : uint8_t { Fresh, Asked, Answered, Failed };

struct Node {
    NodeId id;
    Address address;
    double lastResponse = -1e18;
    int fails = 0;
};

struct Candidate {
    NodeId id;
    Address address;
    State state = Fresh;
    std::string token;
    double askedAt = 0;
};

struct Lookup {
    uint64_t serial;
    NodeId target;
    bool peers;  // get_peers (else find_node)
    std::vector<Candidate> candidates;
    int inflight = 0;
    bool done = false;
};

struct Watch {
    NodeId key;
    bool announce;
    double refresh;
    std::unique_ptr<Lookup> lookup;
    std::vector<Candidate> closest;  // answered, with tokens
    double nextRefresh = 0, lastAnnounce = -1e18, lastLookup = -1e18;
};

enum class Kind : uint8_t { Bootstrap, Step, Refresh, Announce };

struct Transaction {
    Address address;
    double sent;
    Kind kind;
    uint64_t lookup = 0;
    NodeId key{};
};

bool lessDistance(const NodeId& target, const NodeId& a, const NodeId& b) {
    for (size_t i = 0; i < 20; ++i) {
        const uint8_t x = a[i] ^ target[i], y = b[i] ^ target[i];
        if (x != y) return x < y;
    }
    return false;
}

int bucketOf(const NodeId& self, const NodeId& id) {
    for (int i = 0; i < 20; ++i) {
        const uint8_t x = self[size_t(i)] ^ id[size_t(i)];
        if (x)
            for (int b = 7; b >= 0; --b)
                if (x >> b & 1) return i * 8 + (7 - b);
    }
    return 160;
}

std::string compactNode(const Node& n) {
    std::string s(reinterpret_cast<const char*>(n.id.data()), 20);
    uint8_t a[18];
    n.address.writeCompact(a);
    s.append(reinterpret_cast<const char*>(a), 6);
    return s;
}

bool idFrom(const std::string* s, NodeId& out) {
    if (!s || s->size() != 20) return false;
    std::memcpy(out.data(), s->data(), 20);
    return true;
}

} // namespace

NodeId Dht::keyOf(const void* data, size_t size) {
    const Digest d = sha256(data, size);
    NodeId id;
    std::memcpy(id.data(), d.data(), 20);
    return id;
}

struct Dht::Impl {
    Send send;
    DhtConfig cfg;
    NodeId self;
    double now = 0;
    std::vector<Node> table;
    std::map<uint16_t, Transaction> txns;
    uint16_t nextTid = 1;
    uint64_t nextSerial = 1;
    std::vector<std::unique_ptr<Watch>> watches;
    std::unique_ptr<Lookup> selfLookup;
    std::vector<Address> bootstrap;  // asked at the next poll(), when the time is known
    double lastSelfLookup = -1e18;
    double budget, budgetAt = 0;
    std::vector<DhtFound> found;
    struct Vote {
        Address address;
        uint32_t count;
    };
    std::vector<Vote> votes;
    // Serving (as a node): rotating token secrets and stored announcements.
    uint8_t secret[8], previous[8];
    double secretAt = 0;
    struct Stored {
        Address peer;
        double until;
    };
    std::map<std::string, std::vector<Stored>> store;
    double lastCleanup = 0;
    DhtCounters counters;

    Impl(Send s, const DhtConfig& c, const NodeId* id) : send(std::move(s)), cfg(c) {
        if (id) self = *id;
        else randomBytes(self.data(), self.size());
        cfg.k = std::max<size_t>(cfg.k, 1);
        cfg.alpha = std::max(cfg.alpha, 1);
        budget = cfg.queriesPerSecond;
        randomBytes(secret, sizeof secret);
        std::memcpy(previous, secret, sizeof secret);
    }

    // --- Routing table ----------------------------------------------------------

    bool good(const Node& n) const { return n.fails < 2 && now - n.lastResponse < 900; }

    Node* nodeAt(const Address& a) {
        for (Node& n : table)
            if (n.address == a) return &n;
        return nullptr;
    }

    // `verified`: it answered us. Unverified nodes (heard of) fill free room only.
    void learn(const NodeId& id, const Address& a, bool verified) {
        if (!a.valid() || a.v6 || id == self) return;
        for (Node& n : table) {
            if (n.id == id || n.address == a) {
                if (verified) n.id = id, n.address = a, n.lastResponse = now, n.fails = 0;
                return;
            }
        }
        const int bucket = bucketOf(self, id);
        size_t inBucket = 0;
        Node* worst = nullptr;
        for (Node& n : table) {
            if (bucketOf(self, n.id) != bucket) continue;
            ++inBucket;
            if (!good(n) && (!worst || n.fails > worst->fails || n.lastResponse < worst->lastResponse)) worst = &n;
        }
        Node fresh{id, a, verified ? now : -1e18, 0};
        if (inBucket >= 2 * cfg.k || table.size() >= cfg.maxNodes) {
            if (worst && (verified || worst->fails > 0)) *worst = fresh;
            return;
        }
        table.push_back(fresh);
    }

    void failed(const Address& a) {
        for (size_t i = 0; i < table.size(); ++i)
            if (table[i].address == a && ++table[i].fails >= 3) {
                table.erase(table.begin() + long(i));
                return;
            }
    }

    std::vector<const Node*> closest(const NodeId& target, size_t n, bool goodOnly) const {
        std::vector<const Node*> out;
        for (const Node& x : table)
            if (!goodOnly || good(x)) out.push_back(&x);
        const size_t keep = std::min(n, out.size());
        std::partial_sort(out.begin(), out.begin() + long(keep), out.end(),
                          [&](const Node* a, const Node* b) { return lessDistance(target, a->id, b->id); });
        out.resize(keep);
        return out;
    }

    // --- Messages ---------------------------------------------------------------

    bool spend() {
        budget = std::min(cfg.queriesPerSecond, budget + (now - budgetAt) * cfg.queriesPerSecond);
        budgetAt = now;
        if (budget < 1) return ++counters.throttled, false;
        budget -= 1;
        return true;
    }

    bool query(const Address& to, const char* name, BValue args, Transaction t) {
        if (!spend()) return false;
        uint16_t tid = nextTid++;
        while (txns.count(tid)) tid = nextTid++;
        args.set("id", BValue::bytes(self.data(), 20));
        BValue m = BValue::makeDict();
        const uint8_t tb[2] = {uint8_t(tid >> 8), uint8_t(tid)};
        m.set("t", BValue::bytes(tb, 2));
        m.set("y", BValue::string("q"));
        m.set("q", BValue::string(name));
        m.set("a", std::move(args));
        if (cfg.readOnly) m.set("ro", BValue::integer(1));
        m.set("v", BValue::string(std::string("SD\x00\x01", 4)));
        const std::string bytes = m.encode();
        t.address = to;
        t.sent = now;
        txns[tid] = t;
        ++counters.queriesSent;
        send(to, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        return true;
    }

    void reply(const Address& to, const std::string& tid, BValue r) {
        r.set("id", BValue::bytes(self.data(), 20));
        BValue m = BValue::makeDict();
        m.set("t", BValue::string(tid));
        m.set("y", BValue::string("r"));
        m.set("r", std::move(r));
        if (!to.v6) {
            uint8_t a[18];
            to.writeCompact(a);
            m.set("ip", BValue::bytes(a, 6));
        }
        const std::string bytes = m.encode();
        send(to, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }

    void error(const Address& to, const std::string& tid, int code, const char* text) {
        BValue m = BValue::makeDict();
        m.set("t", BValue::string(tid));
        m.set("y", BValue::string("e"));
        BValue e = BValue::makeList();
        e.list.push_back(BValue::integer(code));
        e.list.push_back(BValue::string(text));
        m.set("e", std::move(e));
        const std::string bytes = m.encode();
        send(to, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }

    std::string token(const Address& a, const uint8_t* s) const {
        const Digest d = sha256(s, 8, a.ip.data(), 4);
        return std::string(reinterpret_cast<const char*>(d.data()), 8);
    }

    void receive(const Address& from, const uint8_t* data, size_t size) {
        BValue m;
        if (from.v6 || !BValue::decode(data, size, m) || m.type != BValue::Dict) return void(++counters.malformed);
        const std::string* t = m.str("t");
        const std::string* y = m.str("y");
        if (!t || !y || t->size() > 16) return void(++counters.malformed);
        if (*y == "q") return onQuery(from, *t, m);
        if (t->size() != 2) return;
        const uint16_t tid = uint16_t(uint8_t((*t)[0]) << 8 | uint8_t((*t)[1]));
        auto it = txns.find(tid);
        if (it == txns.end() || it->second.address != from) return;
        const Transaction txn = it->second;
        txns.erase(it);
        if (*y == "e") {
            ++counters.errors;
            return onFailure(txn);
        }
        const BValue* r = m.dictAt("r");
        NodeId id;
        if (*y != "r" || !r || !idFrom(r->str("id"), id)) {
            ++counters.malformed;
            return onFailure(txn);
        }
        ++counters.responses;
        if (const std::string* ip = m.str("ip"); ip && ip->size() == 6)
            vote(Address::readCompact(reinterpret_cast<const uint8_t*>(ip->data()), 6));
        learn(id, from, true);
        std::vector<Candidate> heard;
        if (const std::string* nodes = r->str("nodes"))
            for (size_t i = 0; i + 26 <= nodes->size(); i += 26) {
                Candidate c;
                std::memcpy(c.id.data(), nodes->data() + i, 20);
                c.address = Address::readCompact(reinterpret_cast<const uint8_t*>(nodes->data() + i + 20), 6);
                if (c.address.valid() && c.id != self) heard.push_back(c), learn(c.id, c.address, false);
            }
        const std::string* tok = r->str("token");
        if (const BValue* values = r->get("values"); values && values->type == BValue::List &&
                                                       (txn.kind == Kind::Step || txn.kind == Kind::Refresh)) {
            const NodeId key = txn.key;
            for (const BValue& v : values->list)
                if (v.type == BValue::Str && v.s.size() == 6) {
                    const Address peer = Address::readCompact(reinterpret_cast<const uint8_t*>(v.s.data()), 6);
                    if (peer.valid()) found.push_back({key, peer});
                }
        }
        if (txn.kind == Kind::Step) {
            if (Lookup* l = lookupBySerial(txn.lookup)) {
                --l->inflight;
                for (Candidate& c : l->candidates)
                    if (c.address == from) {
                        c.state = Answered;
                        c.id = id;
                        if (tok) c.token = *tok;
                    }
                for (const Candidate& c : heard) addCandidate(*l, c);
            }
        } else if (txn.kind == Kind::Refresh) {
            for (auto& w : watches)
                if (w->key == txn.key)
                    for (Candidate& c : w->closest)
                        if (c.address == from && tok) c.token = *tok;
        }
    }

    void onFailure(const Transaction& txn) {
        failed(txn.address);
        if (txn.kind == Kind::Step) {
            if (Lookup* l = lookupBySerial(txn.lookup)) {
                --l->inflight;
                for (Candidate& c : l->candidates)
                    if (c.address == txn.address) c.state = Failed;
            }
        } else if (txn.kind == Kind::Refresh || txn.kind == Kind::Announce) {
            for (auto& w : watches)
                if (w->key == txn.key)
                    w->closest.erase(std::remove_if(w->closest.begin(), w->closest.end(),
                                                    [&](const Candidate& c) { return c.address == txn.address; }),
                                     w->closest.end());
        }
    }

    void onQuery(const Address& from, const std::string& tid, const BValue& m) {
        const std::string* q = m.str("q");
        const BValue* a = m.dictAt("a");
        NodeId id;
        if (!q || !a || !idFrom(a->str("id"), id)) return error(from, tid, 203, "Protocol Error");
        ++counters.queriesAnswered;
        int64_t ro = 0;
        if (!(m.integerAt("ro", ro) && ro == 1)) learn(id, from, false);
        BValue r = BValue::makeDict();
        auto nodesNear = [&](const NodeId& target) {
            std::string s;
            for (const Node* n : closest(target, cfg.k, true)) s += compactNode(*n);
            return s;
        };
        if (*q == "ping") return reply(from, tid, std::move(r));
        if (*q == "find_node") {
            NodeId target;
            if (!idFrom(a->str("target"), target)) return error(from, tid, 203, "Protocol Error");
            r.set("nodes", BValue::string(nodesNear(target)));
            return reply(from, tid, std::move(r));
        }
        if (*q == "get_peers") {
            NodeId key;
            if (!idFrom(a->str("info_hash"), key)) return error(from, tid, 203, "Protocol Error");
            r.set("token", BValue::string(token(from, secret)));
            auto it = store.find(std::string(reinterpret_cast<const char*>(key.data()), 20));
            if (it != store.end() && !it->second.empty()) {
                BValue values = BValue::makeList();
                for (const Stored& s : it->second) {
                    if (s.until < now || values.list.size() >= 50) continue;
                    uint8_t c[18];
                    s.peer.writeCompact(c);
                    values.list.push_back(BValue::bytes(c, 6));
                }
                r.set("values", std::move(values));
            }
            r.set("nodes", BValue::string(nodesNear(key)));
            return reply(from, tid, std::move(r));
        }
        if (*q == "announce_peer") {
            NodeId key;
            const std::string* tok = a->str("token");
            int64_t port = 0, implied = 0;
            a->integerAt("port", port);
            a->integerAt("implied_port", implied);
            if (!idFrom(a->str("info_hash"), key) || !tok) return error(from, tid, 203, "Protocol Error");
            if (*tok != token(from, secret) && *tok != token(from, previous)) {
                ++counters.announcesRefused;
                return error(from, tid, 203, "Bad token");
            }
            Address peer = from;
            if (implied != 1) {
                if (port <= 0 || port > 65535) return error(from, tid, 203, "Bad port");
                peer.port = uint16_t(port);
            }
            const std::string k(reinterpret_cast<const char*>(key.data()), 20);
            if (store.size() >= cfg.maxStoredKeys && !store.count(k)) return error(from, tid, 202, "Server Error");
            auto& list = store[k];
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Stored& s) {
                           return s.peer == peer || s.until < now;
                       }),
                       list.end());
            if (list.size() >= cfg.maxPeersPerKey) list.erase(list.begin());
            list.push_back({peer, now + cfg.peerSeconds});
            ++counters.announcesStored;
            return reply(from, tid, std::move(r));
        }
        error(from, tid, 204, "Method Unknown");
    }

    void vote(const Address& a) {
        if (!a.valid()) return;
        for (Vote& v : votes)
            if (v.address == a) return void(++v.count);
        if (votes.size() >= 16) votes.erase(std::min_element(votes.begin(), votes.end(), [](auto& x, auto& y) {
            return x.count < y.count;
        }));
        votes.push_back({a, 1});
    }

    // --- Lookups ----------------------------------------------------------------

    Lookup* lookupBySerial(uint64_t serial) {
        if (selfLookup && selfLookup->serial == serial) return selfLookup.get();
        for (auto& w : watches)
            if (w->lookup && w->lookup->serial == serial) return w->lookup.get();
        return nullptr;
    }

    void addCandidate(Lookup& l, const Candidate& c) {
        for (const Candidate& x : l.candidates)
            if (x.id == c.id || x.address == c.address) return;
        auto at = std::lower_bound(l.candidates.begin(), l.candidates.end(), c, [&](const Candidate& a, const Candidate& b) {
            return lessDistance(l.target, a.id, b.id);
        });
        l.candidates.insert(at, c);
        if (l.candidates.size() > 8 * cfg.k) l.candidates.pop_back();
    }

    std::unique_ptr<Lookup> startLookup(const NodeId& target, bool peers) {
        auto l = std::make_unique<Lookup>();
        l->serial = nextSerial++;
        l->target = target;
        l->peers = peers;
        for (const Node* n : closest(target, 4 * cfg.k, false)) addCandidate(*l, {n->id, n->address, Fresh, {}});
        return l;
    }

    // A node that has not answered within slowSeconds no longer holds the
    // lookup up: it stops counting against alpha and the lookup may end
    // without it (libtorrent's short timeout). It can still answer.
    void step(Lookup& l) {
        if (l.done) return;
        int fast = 0;
        for (const Candidate& c : l.candidates) fast += c.state == Asked && now - c.askedAt < cfg.slowSeconds;
        size_t considered = 0;
        bool open = false;
        for (Candidate& c : l.candidates) {
            if (c.state == Failed) continue;
            if (considered++ >= cfg.k) break;
            if (c.state == Asked && now - c.askedAt < cfg.slowSeconds) open = true;
            if (c.state != Fresh) continue;
            open = true;
            if (fast >= cfg.alpha) continue;
            BValue args = BValue::makeDict();
            if (l.peers) args.set("info_hash", BValue::bytes(l.target.data(), 20));
            else args.set("target", BValue::bytes(l.target.data(), 20));
            Transaction t{};
            t.kind = Kind::Step;
            t.lookup = l.serial;
            t.key = l.target;
            if (!query(c.address, l.peers ? "get_peers" : "find_node", std::move(args), t)) return;
            c.state = Asked;
            c.askedAt = now;
            ++l.inflight;
            ++fast;
        }
        if (!open) l.done = true;
    }

    void announce(Watch& w) {
        for (const Candidate& c : w.closest) {
            if (c.token.empty()) continue;
            BValue args = BValue::makeDict();
            args.set("info_hash", BValue::bytes(w.key.data(), 20));
            args.set("port", BValue::integer(cfg.port ? cfg.port : 1));
            args.set("implied_port", BValue::integer(1));
            args.set("token", BValue::string(c.token));
            Transaction t{};
            t.kind = Kind::Announce;
            t.key = w.key;
            query(c.address, "announce_peer", std::move(args), t);
        }
        w.lastAnnounce = now;
    }

    void poll(double t) {
        now = t;
        if (now - secretAt > 300) {
            std::memcpy(previous, secret, sizeof secret);
            randomBytes(secret, sizeof secret);
            secretAt = now;
        }
        for (auto it = txns.begin(); it != txns.end();) {
            if (now - it->second.sent > cfg.queryTimeout) {
                ++counters.timeouts;
                const Transaction txn = it->second;
                it = txns.erase(it);
                onFailure(txn);
            } else {
                ++it;
            }
        }
        for (const Address& a : std::exchange(bootstrap, {})) {
            BValue args = BValue::makeDict();
            args.set("target", BValue::bytes(self.data(), 20));
            Transaction t{};
            t.kind = Kind::Bootstrap;
            if (!query(a, "find_node", std::move(args), t)) bootstrap.push_back(a);
        }
        // Keep the routing table populated: walk towards our own id.
        const size_t g = goodCount();
        if (!selfLookup && !table.empty() &&
            ((g < 4 * cfg.k && now - lastSelfLookup > 15) || now - lastSelfLookup > 600)) {
            selfLookup = startLookup(self, false);
            lastSelfLookup = now;
        }
        if (selfLookup) {
            step(*selfLookup);
            if (selfLookup->done) selfLookup.reset();
        }
        for (auto& wp : watches) {
            Watch& w = *wp;
            const bool stale = w.closest.size() < (cfg.k + 1) / 2 || now - w.lastLookup > cfg.relookupSeconds;
            if (!w.lookup && stale && !table.empty() && now - w.lastLookup > 1.0) {
                w.lookup = startLookup(w.key, true);
                w.lastLookup = now;
            }
            if (w.lookup) {
                step(*w.lookup);
                if (w.lookup->done) {
                    std::vector<Candidate> best;
                    for (const Candidate& c : w.lookup->candidates)
                        if (c.state == Answered && best.size() < cfg.k) best.push_back(c);
                    if (!best.empty()) w.closest = std::move(best);
                    w.lookup.reset();
                    if (w.announce && !w.closest.empty()) announce(w);
                    w.nextRefresh = now + w.refresh;
                }
                continue;
            }
            if (now >= w.nextRefresh && !w.closest.empty()) {
                for (const Candidate& c : w.closest) {
                    BValue args = BValue::makeDict();
                    args.set("info_hash", BValue::bytes(w.key.data(), 20));
                    Transaction tx{};
                    tx.kind = Kind::Refresh;
                    tx.key = w.key;
                    query(c.address, "get_peers", std::move(args), tx);
                }
                w.nextRefresh = now + w.refresh;
                if (w.announce && now - w.lastAnnounce > cfg.announceSeconds) announce(w);
            }
        }
        if (!store.empty() && now - lastCleanup > 60) {
            lastCleanup = now;
            for (auto it = store.begin(); it != store.end();) {
                auto& list = it->second;
                list.erase(std::remove_if(list.begin(), list.end(), [&](const Stored& s) { return s.until < now; }),
                           list.end());
                it = list.empty() ? store.erase(it) : std::next(it);
            }
        }
    }

    size_t goodCount() const {
        size_t n = 0;
        for (const Node& x : table) n += good(x);
        return n;
    }
};

Dht::Dht(Send send, const DhtConfig& config, const NodeId* id) : impl_(std::make_unique<Impl>(std::move(send), config, id)) {}
Dht::~Dht() = default;

void Dht::addNode(const Address& node) {
    if (node.valid() && !node.v6 && impl_->bootstrap.size() < 256) impl_->bootstrap.push_back(node);
}
void Dht::receive(const Address& from, const uint8_t* data, size_t size) { impl_->receive(from, data, size); }
void Dht::poll(double now) { impl_->poll(now); }

void Dht::watch(const NodeId& key, bool announce, double refreshSeconds) {
    for (auto& w : impl_->watches)
        if (w->key == key) {
            if (announce && !w->announce) w->announce = true, w->lastAnnounce = -1e18, w->nextRefresh = impl_->now;
            w->refresh = std::max(refreshSeconds, 1.0);
            return;
        }
    auto w = std::make_unique<Watch>();
    w->key = key;
    w->announce = announce;
    w->refresh = std::max(refreshSeconds, 1.0);
    impl_->watches.push_back(std::move(w));
}
void Dht::unwatch(const NodeId& key) {
    auto& ws = impl_->watches;
    ws.erase(std::remove_if(ws.begin(), ws.end(), [&](auto& w) { return w->key == key; }), ws.end());
}
std::vector<DhtFound> Dht::takeFound() { return std::exchange(impl_->found, {}); }
const NodeId& Dht::id() const { return impl_->self; }
size_t Dht::nodeCount() const { return impl_->table.size(); }
size_t Dht::goodNodes() const { return impl_->goodCount(); }
bool Dht::ready() const { return impl_->goodCount() >= impl_->cfg.k; }
Address Dht::externalAddress() const {
    const Impl::Vote* best = nullptr;
    for (const auto& v : impl_->votes)
        if (!best || v.count > best->count) best = &v;
    return best ? best->address : Address{};
}
std::vector<Address> Dht::nodes(size_t max) const {
    std::vector<const Node*> all;
    for (const Node& n : impl_->table)
        if (impl_->good(n)) all.push_back(&n);
    std::sort(all.begin(), all.end(), [](const Node* a, const Node* b) { return a->lastResponse > b->lastResponse; });
    std::vector<Address> out;
    for (size_t i = 0; i < all.size() && i < max; ++i) out.push_back(all[i]->address);
    return out;
}
const DhtCounters& Dht::counters() const { return impl_->counters; }

} // namespace saida::p2p
