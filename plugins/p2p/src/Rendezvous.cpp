#include "saida/p2p/Rendezvous.hpp"

#include "saida/p2p/Socket.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace saida::p2p {
namespace {

constexpr uint8_t kService = 1;
constexpr uint8_t kTopicsMessage = 1;
constexpr uint8_t kIntent = 'R';
constexpr uint8_t kDirect = 0, kIntroduced = 1;
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const uint8_t kBeaconMagic[4] = {'S', 'P', '2', 'L'};

using Tag = std::array<uint8_t, 8>;

Bytes intentFor(const Topic& t, uint8_t how) {
    const Tag tag = t.tag();
    Bytes b{kIntent};
    b.insert(b.end(), tag.begin(), tag.end());
    b.push_back(how);
    return b;
}

bool readIntent(const Bytes& intent, Tag& tag, uint8_t& how) {
    if (intent.size() != 10 || intent[0] != kIntent || intent[9] > kIntroduced) return false;
    std::memcpy(tag.data(), intent.data() + 1, 8);
    how = intent[9];
    return true;
}

int codeValue(char c) {
    c = char(c >= 'a' && c <= 'z' ? c - 32 : c);
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

int checkOf(const int* v) {
    int sum = 0;
    for (int i = 0; i < 7; ++i) sum += (2 * i + 1) * v[i];  // odd weights: every single typo changes it
    return sum % 32;
}

// Bootstrap names resolve on a detached thread; the Rendezvous may be gone
// by the time the resolver returns, so the result lives in shared state.
struct Resolution {
    std::mutex mutex;
    std::vector<Address> addresses;
    bool done = false;
};

} // namespace

Topic Topic::named(std::string_view application, std::string_view name) {
    std::string s = "saida-p2p topic ";
    s.append(application.data(), application.size());
    s.push_back('\0');
    s.append(name.data(), name.size());
    Topic t;
    t.key = sha256(s.data(), s.size());
    return t;
}

NodeId Topic::dhtKey() const {
    static const char tag[] = "dht";
    const Digest d = sha256(key.data(), key.size(), tag, 3);
    NodeId id;
    std::memcpy(id.data(), d.data(), 20);
    return id;
}

std::array<uint8_t, 8> Topic::tag() const {
    static const char tag[] = "hello";
    const Digest d = sha256(key.data(), key.size(), tag, 5);
    std::array<uint8_t, 8> out;
    std::memcpy(out.data(), d.data(), 8);
    return out;
}

std::string newArenaCode() {
    uint8_t r[7];
    randomBytes(r, sizeof r);
    int v[8];
    for (int i = 0; i < 7; ++i) v[i] = r[i] & 31;
    v[7] = checkOf(v);
    std::string s;
    for (int i = 0; i < 8; ++i) {
        if (i == 4) s.push_back('-');
        s.push_back(kAlphabet[v[i]]);
    }
    return s;
}

bool normalizeArenaCode(std::string_view typed, std::string& canonical) {
    int v[8];
    int n = 0;
    for (char c : typed) {
        if (c == '-' || c == ' ') continue;
        const int x = codeValue(c);
        if (x < 0 || n == 8) return false;
        v[n++] = x;
    }
    if (n != 8 || checkOf(v) != v[7]) return false;
    canonical.clear();
    for (int i = 0; i < 8; ++i) {
        if (i == 4) canonical.push_back('-');
        canonical.push_back(kAlphabet[v[i]]);
    }
    return true;
}

Topic arenaTopic(std::string_view application, std::string_view canonicalCode) {
    std::string name = "arena/";
    name.append(canonicalCode.data(), canonicalCode.size());
    return Topic::named(application, name);
}

bool openLanSocket(UdpSocket& socket, const RendezvousConfig& config) {
    SocketOptions o;
    o.port = config.lanGroup.port;
    o.ipv6 = false;
    o.reuseAddress = true;
    o.bufferBytes = 64 << 10;
    return socket.open(o) && socket.joinMulticast(config.lanGroup);
}

struct Rendezvous::Impl {
    Host& host;
    RendezvousConfig cfg;
    std::unique_ptr<Dht> dht;
    DatagramSocket* lan = nullptr;
    double now = 0, started = -1, lastBeacon = -1e18;
    struct Joined {
        Topic topic;
        Tag tag;
    };
    std::vector<Joined> topics;
    struct Remote {
        PeerHandle peer;
        bool viaUs = false;       // the link was made for one of our topics
        bool introduced = false;  // ... by an introduction
        bool heard = false;       // its topic list arrived
        std::vector<Tag> tags;
        std::vector<Tag> member;  // topics we reported it in
        double alone = -1;
    };
    std::vector<Remote> remotes;
    struct Attempt {
        Address address;
        double at;
        int failures;
    };
    std::vector<Attempt> attempts;
    std::vector<RendezvousEvent> events;
    Host::Admission fallback;
    std::shared_ptr<Resolution> resolution;
    bool resolutionTaken = false;
    uint64_t instance = randomU64();
    uint32_t appHash = 0;
    RendezvousStatus st;
    Address lastExternal;

    Impl(Host& h, const RendezvousConfig& c) : host(h), cfg(c) {
        const Digest d = sha256(host.config().application.data(), host.config().application.size());
        std::memcpy(&appHash, d.data(), 4);
        if (cfg.useDht) {
            cfg.dht.port = host.localAddress().port;
            dht = std::make_unique<Dht>([this](const Address& to, const uint8_t* p, size_t n) {
                return host.sendRaw(to, p, n);
            }, cfg.dht);
            for (const Address& a : cfg.knownNodes) dht->addNode(a);
            if (!cfg.bootstrap.empty()) {
                resolution = std::make_shared<Resolution>();
                std::thread([r = resolution, names = cfg.bootstrap] {
                    std::vector<Address> out;
                    for (const std::string& name : names) {
                        const size_t colon = name.rfind(':');
                        if (colon == std::string::npos) continue;
                        const int port = std::atoi(name.c_str() + colon + 1);
                        if (port <= 0 || port > 65535) continue;
                        for (const Address& a : resolve(name.substr(0, colon), uint16_t(port)))
                            if (!a.v6) out.push_back(a);
                    }
                    std::lock_guard<std::mutex> lock(r->mutex);
                    r->addresses = std::move(out);
                    r->done = true;
                }).detach();
            }
        }
        host.setForeign([this](const Address& from, const uint8_t* p, size_t n) {
            if (dht) dht->receive(from, p, n);
        });
        host.setAdmission([this](const Address& from, const Bytes& intent) { return admit(from, intent); });
        host.setService(kService, [this](PeerHandle peer, const uint8_t* p, size_t n) { onService(peer, p, n); });
        host.addListener([this](const HostEvent& e) { onHostEvent(e); });
    }

    const Joined* topicByTag(const Tag& t) const {
        for (const Joined& j : topics)
            if (j.tag == t) return &j;
        return nullptr;
    }
    Remote* remote(PeerHandle p) {
        for (Remote& r : remotes)
            if (r.peer == p) return &r;
        return nullptr;
    }
    // A peer's topic list can arrive in the very poll that connected it,
    // before the Connected event is heard: the link's intent says the rest.
    Remote* ensure(PeerHandle p) {
        if (Remote* r = remote(p)) return r;
        const PeerInfo* info = host.peer(p);
        if (!info) return nullptr;
        Remote r;
        r.peer = p;
        Tag tag;
        uint8_t how;
        if (readIntent(info->intent, tag, how)) r.viaUs = true, r.introduced = how == kIntroduced;
        remotes.push_back(r);
        return &remotes.back();
    }
    size_t membersOf(const Tag& t) const {
        size_t n = 0;
        for (const Remote& r : remotes) n += std::find(r.member.begin(), r.member.end(), t) != r.member.end();
        return n;
    }

    bool admit(const Address& from, const Bytes& intent) {
        Tag tag;
        uint8_t how;
        if (readIntent(intent, tag, how)) return topicByTag(tag) && membersOf(tag) < cfg.maxPeersPerTopic;
        return fallback && fallback(from, intent);
    }

    void sendTopics(PeerHandle peer) {
        Bytes m{kTopicsMessage, uint8_t(std::min<size_t>(topics.size(), 64))};
        for (size_t i = 0; i < topics.size() && i < 64; ++i) m.insert(m.end(), topics[i].tag.begin(), topics[i].tag.end());
        host.sendService(peer, kService, m.data(), m.size());
    }

    void onService(PeerHandle peer, const uint8_t* p, size_t n) {
        Remote* r = ensure(peer);
        if (!r || n < 2 || p[0] != kTopicsMessage || n != 2 + size_t(p[1]) * 8) return;
        r->tags.resize(p[1]);
        for (size_t i = 0; i < r->tags.size(); ++i) std::memcpy(r->tags[i].data(), p + 2 + 8 * i, 8);
        r->heard = true;
        recompute(*r);
    }

    void recompute(Remote& r) {
        for (const Joined& j : topics) {
            const bool both = std::find(r.tags.begin(), r.tags.end(), j.tag) != r.tags.end();
            const bool was = std::find(r.member.begin(), r.member.end(), j.tag) != r.member.end();
            if (both && !was) {
                r.member.push_back(j.tag);
                events.push_back({RendezvousEventType::PeerJoined, j.topic, r.peer});
                // It reached us on its own: tell it about everyone else here, and
                // them about it, so the whole topic connects in one round trip.
                if (cfg.introduce && r.viaUs && !r.introduced)
                    for (const Remote& o : remotes)
                        if (o.peer != r.peer && std::find(o.member.begin(), o.member.end(), j.tag) != o.member.end())
                            host.introduce(r.peer, o.peer, intentFor(j.topic, kIntroduced));
            }
        }
        for (size_t i = 0; i < r.member.size();) {
            const Joined* j = topicByTag(r.member[i]);
            const bool both = j && std::find(r.tags.begin(), r.tags.end(), r.member[i]) != r.tags.end();
            if (both) {
                ++i;
                continue;
            }
            if (j) events.push_back({RendezvousEventType::PeerLeft, j->topic, r.peer});
            r.member.erase(r.member.begin() + long(i));
        }
        if (!r.member.empty()) r.alone = -1;
        else if (r.heard && r.alone < 0) r.alone = now;
    }

    // A topic we left is no longer in `topics`, so its members get PeerLeft here.
    void forget(const Topic& t) {
        const Tag tag = t.tag();
        for (Remote& r : remotes) {
            auto it = std::find(r.member.begin(), r.member.end(), tag);
            if (it == r.member.end()) continue;
            r.member.erase(it);
            events.push_back({RendezvousEventType::PeerLeft, t, r.peer});
            if (r.member.empty() && r.alone < 0) r.alone = now;
        }
    }

    void onHostEvent(const HostEvent& e) {
        if (e.type == HostEventType::Connected) {
            if (ensure(e.peer)) sendTopics(e.peer);
            for (Attempt& a : attempts)
                if (a.address == e.address) a.failures = 0;  // reachable: no backoff next time
        } else if (e.type == HostEventType::Disconnected) {
            for (size_t i = 0; i < remotes.size(); ++i) {
                if (remotes[i].peer != e.peer) continue;
                for (const Tag& t : remotes[i].member)
                    if (const Joined* j = topicByTag(t)) events.push_back({RendezvousEventType::PeerLeft, j->topic, e.peer});
                remotes.erase(remotes.begin() + long(i));
                break;
            }
        } else if (e.type == HostEventType::ConnectFailed) {
            for (Attempt& a : attempts)
                if (a.address == e.address) ++a.failures;
        }
    }

    void tryConnect(const Joined& j, const Address& a) {
        if (!a.valid() || a == host.publicAddress() || (dht && a == dht->externalAddress())) return;
        if (host.peerAt(a) || host.connecting(a)) return;
        Attempt* at = nullptr;
        for (Attempt& x : attempts)
            if (x.address == a) at = &x;
        if (at && now - at->at < cfg.retrySeconds * double(1 << std::min(at->failures, 4))) return;
        if (!at) {
            if (attempts.size() >= 1024) attempts.erase(attempts.begin());
            attempts.push_back({a, now, 0});
            at = &attempts.back();
        }
        at->at = now;
        ++st.attempts;
        host.connect(a, intentFor(j.topic, kDirect));
    }

    // A refusal says why (CLAUDE.md 3): an unreachable DHT is named, not
    // left to look like nobody being there.
    void problem() {
        st.problem.clear();
        if (!dht) return;
        bool resolvedNothing = false;
        if (resolution && resolutionTaken) {
            std::lock_guard<std::mutex> lock(resolution->mutex);
            resolvedNothing = resolution->addresses.empty();
        }
        if (resolvedNothing && cfg.knownNodes.empty() && dht->nodeCount() == 0)
            st.problem = "no DHT bootstrap address resolved: offline, or DNS blocked";
        else if (now - started > 15 && dht->goodNodes() == 0)
            st.problem = "no DHT node answered in 15 s: UDP is blocked or the network is down";
    }

    void beacons() {
        if (!lan) return;
        if (!topics.empty() && now - lastBeacon >= cfg.lanSeconds) {
            lastBeacon = now;
            Bytes b(kBeaconMagic, kBeaconMagic + 4);
            b.push_back(1);
            for (int i = 0; i < 4; ++i) b.push_back(uint8_t(appHash >> (8 * i)));
            for (int i = 0; i < 8; ++i) b.push_back(uint8_t(instance >> (8 * i)));
            const uint16_t port = host.localAddress().port;
            b.push_back(uint8_t(port)), b.push_back(uint8_t(port >> 8));
            const size_t count = std::min<size_t>(topics.size(), 32);
            b.push_back(uint8_t(count));
            for (size_t i = 0; i < count; ++i) b.insert(b.end(), topics[i].tag.begin(), topics[i].tag.end());
            lan->send(cfg.lanGroup, b.data(), b.size());
            ++st.lanBeaconsSent;
        }
        uint8_t buf[512];
        Address from;
        for (int i = 0; i < 256; ++i) {
            const int n = lan->receive(from, buf, sizeof buf);
            if (n < 0) break;
            if (n < 20 || std::memcmp(buf, kBeaconMagic, 4) != 0 || buf[4] != 1) continue;
            uint32_t app = 0;
            uint64_t inst = 0;
            for (int k = 0; k < 4; ++k) app |= uint32_t(buf[5 + k]) << (8 * k);
            for (int k = 0; k < 8; ++k) inst |= uint64_t(buf[9 + k]) << (8 * k);
            const uint16_t port = uint16_t(buf[17] | buf[18] << 8);
            const size_t count = buf[19];
            if (app != appHash || inst == instance || size_t(n) != 20 + 8 * count) continue;
            ++st.lanBeaconsHeard;
            Address peer = from;
            peer.port = port;
            for (size_t k = 0; k < count; ++k) {
                Tag t;
                std::memcpy(t.data(), buf + 20 + 8 * k, 8);
                if (const Joined* j = topicByTag(t)) tryConnect(*j, peer);
            }
        }
    }

    void poll(double t) {
        now = t;
        if (started < 0) started = now;
        problem();
        if (dht) {
            if (resolution && !resolutionTaken) {
                std::lock_guard<std::mutex> lock(resolution->mutex);
                if (resolution->done) {
                    resolutionTaken = true;
                    for (const Address& a : resolution->addresses) dht->addNode(a);
                }
            }
            dht->poll(now);
            for (const DhtFound& f : dht->takeFound()) {
                ++st.found;
                for (const Joined& j : topics)
                    if (j.topic.dhtKey() == f.key) tryConnect(j, f.peer);
            }
            const Address ext = dht->externalAddress();
            if (ext.valid() && ext != lastExternal) host.notePublicAddress(lastExternal = ext);
        }
        beacons();
        if (cfg.lingerSeconds >= 0)
            for (Remote& r : remotes)
                if (r.viaUs && r.alone >= 0 && now - r.alone > cfg.lingerSeconds) {
                    r.alone = -1;
                    host.disconnect(r.peer);
                }
    }
};

Rendezvous::Rendezvous(Host& host, const RendezvousConfig& config) : impl_(std::make_unique<Impl>(host, config)) {}
Rendezvous::~Rendezvous() {
    impl_->host.setForeign(nullptr);
    impl_->host.setAdmission(nullptr);
    impl_->host.setService(kService, nullptr);
}

void Rendezvous::setLanSocket(DatagramSocket* socket) { impl_->lan = socket; }

void Rendezvous::join(const Topic& topic) {
    if (joined(topic)) return;
    impl_->topics.push_back({topic, topic.tag()});
    if (impl_->dht) impl_->dht->watch(topic.dhtKey(), true, impl_->cfg.refreshSeconds);
    impl_->lastBeacon = -1e18;
    for (auto& r : impl_->remotes) {
        impl_->sendTopics(r.peer);
        impl_->recompute(r);
    }
}

void Rendezvous::leave(const Topic& topic) {
    auto& ts = impl_->topics;
    const size_t before = ts.size();
    ts.erase(std::remove_if(ts.begin(), ts.end(), [&](const Impl::Joined& j) { return j.topic == topic; }), ts.end());
    if (ts.size() == before) return;
    if (impl_->dht) impl_->dht->unwatch(topic.dhtKey());
    impl_->forget(topic);
    for (auto& r : impl_->remotes) impl_->sendTopics(r.peer);
}

bool Rendezvous::joined(const Topic& topic) const {
    for (const auto& j : impl_->topics)
        if (j.topic == topic) return true;
    return false;
}

std::vector<PeerHandle> Rendezvous::peers(const Topic& topic) const {
    std::vector<PeerHandle> out;
    const auto tag = topic.tag();
    for (const auto& r : impl_->remotes)
        if (std::find(r.member.begin(), r.member.end(), tag) != r.member.end()) out.push_back(r.peer);
    return out;
}

void Rendezvous::poll(double now) { impl_->poll(now); }
std::vector<RendezvousEvent> Rendezvous::takeEvents() { return std::exchange(impl_->events, {}); }
void Rendezvous::setAdmission(Host::Admission fallback) { impl_->fallback = std::move(fallback); }
Dht* Rendezvous::dht() { return impl_->dht.get(); }

RendezvousStatus Rendezvous::status() const {
    RendezvousStatus s = impl_->st;
    if (impl_->dht) {
        s.dhtNodes = impl_->dht->nodeCount();
        s.dhtGoodNodes = impl_->dht->goodNodes();
        s.dhtReady = impl_->dht->ready();
    }
    s.publicAddress = impl_->host.publicAddress();
    return s;
}

} // namespace saida::p2p
