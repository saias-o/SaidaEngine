#include "saida/p2p/Rendezvous.hpp"

#include "saida/p2p/Socket.hpp"
#include "saida/netcode/Wire.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace saida::p2p {
namespace {

using netcode::Reader;
using netcode::Writer;

constexpr uint8_t kService = 1;
constexpr uint8_t kTopicsMessage = 1;
// Proximity: where the sender stands, whether it wants the link, and who
// stands near the receiver; "introduce me to this peer of yours".
constexpr uint8_t kNearMessage = 2, kIntroduceMessage = 3;
constexpr size_t kMaxGossip = 40;     // entries in one near message (one datagram)
constexpr size_t kMaxCandidates = 512;
constexpr uint8_t kIntent = 'R';
constexpr uint8_t kDirect = 0, kIntroduced = 1;
const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const uint8_t kBeaconMagic[4] = {'S', 'P', '2', 'L'};

using Tag = std::array<uint8_t, 8>;

Bytes intentFor(const Tag& tag, uint8_t how) {
    Bytes b{kIntent};
    b.insert(b.end(), tag.begin(), tag.end());
    b.push_back(how);
    return b;
}
Bytes intentFor(const Topic& t, uint8_t how) { return intentFor(t.tag(), how); }

using Point = std::array<double, 3>;
double distance2(const Point& a, const Point& b) {
    return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
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

NodeId Topic::dhtKey(uint32_t shard) const {
    const uint8_t tag[7] = {'d', 'h', 't', '/', uint8_t(shard), uint8_t(shard >> 8), uint8_t(shard >> 16)};
    const Digest d = sha256(key.data(), key.size(), tag, sizeof tag);
    NodeId id;
    std::memcpy(id.data(), d.data(), 20);
    return id;
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
        bool announcing = true;   // as last told to the DHT
        double refresh = 0;
        double lastHeal = -1e18;  // proximity: the last DHT address tried in a crowded topic
        // Proximity: the shard keys, ours among them, and what we do in the DHT.
        std::vector<NodeId> keys;
        uint32_t shard = 0;
        enum Role : uint8_t { None, Seeker, Finder, Announcer, Silent } role = None;
        double linkedSince = -1, aloneSince = -1;
        // The announcer sketch: the lowest-ranked ids heard of, and until when.
        struct Low {
            uint64_t id;
            uint64_t rank;
            double expires;
        };
        std::vector<Low> low;
        bool sketchHeard = false;
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
        // Proximity.
        uint64_t id = 0;
        bool placed = false;      // its position arrived
        Point at{};
        bool wantsUs = false;     // it keeps us among its nearest
        uint16_t look = 0;        // its setLook, passed on as a sighting
        bool sightTurn = false;   // sightings go with every other position message
        bool nearDue = false;     // tell it where we stand and who is near it, at the next poll
        double unwanted = -1;     // since when neither side wants the link
        double introTokens = 8, introAt = 0;
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
    // Proximity.
    uint64_t selfId = 0;
    bool positioned = false;
    Point position{}, told{};
    double lastPosition = -1e18, lastGossip = -1e18, lastSelect = -1e18, lastTend = -1e18;
    struct Candidate {
        uint64_t id;
        Point at;
        PeerHandle via;  // the peer that named it, who can introduce us
        double heard;
    };
    std::vector<Candidate> candidates;
    struct Asked {
        uint64_t id;
        double at;
        int times;
    };
    std::vector<Asked> asked;
    std::vector<uint64_t> wanted;  // sorted: the ids we keep links to
    double askTokens = 8, askAt = 0;
    // Sightings: the far crowd, kept by ring of distance (rings doubling from 32 m).
    struct Seen {
        uint32_t id;
        Point at;
        uint16_t look;
        double born;  // when it was true, our clock
    };
    std::vector<Seen> seen;
    uint16_t look = 0;
    static constexpr int kRings = 8;
    int ringOf(const Point& at) const {
        const double d = std::sqrt(distance2(at, position));
        int r = 0;
        while (r + 1 < kRings && d >= double(32 << r)) ++r;
        return r;
    }
    bool linked32(uint32_t id) const {
        for (const Remote& r : remotes)
            if (uint32_t(r.id) == id) return true;
        return false;
    }
    void sighted(uint32_t id, const Point& at, uint16_t how, double born) {
        if (cfg.sightings == 0 || id == uint32_t(selfId) || linked32(id) || born < now - cfg.sightingSeconds) return;
        for (Seen& s : seen)
            if (s.id == id) {
                if (born > s.born) s.at = at, s.look = how, s.born = born;
                return;
            }
        if (seen.size() < cfg.sightings) return seen.push_back({id, at, how, born});
        // Full: the most crowded ring gives way to a sparser one, so near and
        // far are drawn alike; within one ring, the stalest gives way.
        size_t count[kRings] = {};
        for (const Seen& s : seen) ++count[ringOf(s.at)];
        const int ring = ringOf(at);
        int fullest = 0;
        for (int r = 1; r < kRings; ++r)
            if (count[r] > count[fullest]) fullest = r;
        const int from = count[fullest] > count[ring] + 1 ? fullest : ring;
        Seen* stalest = nullptr;
        for (Seen& s : seen)
            if (ringOf(s.at) == from && (!stalest || s.born < stalest->born)) stalest = &s;
        if (stalest && (from != ring || stalest->born < born)) *stalest = {id, at, how, born};
    }
    // Introductions under way: punched for relayAfterSeconds, then relayed.
    struct Introduction {
        Address address;
        PeerHandle via;
        Key key;
        Bytes intent;
        double at;
        bool relayed;
    };
    std::vector<Introduction> introductions;

    Impl(Host& h, const RendezvousConfig& c) : host(h), cfg(c) {
        selfId = host.identity().id();
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
                        // Both families: the IPv6 DHT (BEP 32) is bootstrapped too.
                        for (const Address& a : resolve(name.substr(0, colon), uint16_t(port))) out.push_back(a);
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
        r.id = info->id;
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
        if (readIntent(intent, tag, how))
            return topicByTag(tag) && (cfg.neighbours > 0 || membersOf(tag) < cfg.maxPeersPerTopic);
        return fallback && fallback(from, intent);
    }

    void sendTopics(PeerHandle peer) {
        Bytes m{kTopicsMessage, uint8_t(std::min<size_t>(topics.size(), 64))};
        for (size_t i = 0; i < topics.size() && i < 64; ++i) m.insert(m.end(), topics[i].tag.begin(), topics[i].tag.end());
        host.sendService(peer, kService, m.data(), m.size());
    }

    void onService(PeerHandle peer, const uint8_t* p, size_t n) {
        Remote* r = ensure(peer);
        if (!r || n < 2) return;
        if (p[0] == kNearMessage) return onNear(*r, p + 1, n - 1);
        if (p[0] == kIntroduceMessage) return onIntroduce(*r, p + 1, n - 1);
        if (p[0] != kTopicsMessage || n != 2 + size_t(p[1]) * 8) return;
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
                r.nearDue = r.nearDue || r.member.empty();
                r.member.push_back(j.tag);
                events.push_back({RendezvousEventType::PeerJoined, j.topic, r.peer});
                // It reached us on its own: tell it about everyone else here, and
                // them about it, so the whole topic connects in one round trip.
                // A crowd (proximity) introduces it only to whoever is near it.
                if (cfg.introduce && cfg.neighbours == 0 && r.viaUs && !r.introduced)
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
            if (Remote* r = ensure(e.peer)) {
                sendTopics(e.peer);
                const uint64_t id = r->id;
                asked.erase(std::remove_if(asked.begin(), asked.end(), [&](const Asked& a) { return a.id == id; }),
                            asked.end());
            }
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
        } else if (e.type == HostEventType::Introduced && cfg.relay) {
            for (Introduction& i : introductions)
                if (i.key == e.key) return void((i.via = e.peer, i.address = e.address));
            if (introductions.size() >= 128) introductions.erase(introductions.begin());
            introductions.push_back({e.address, e.peer, e.key, e.intent, now, false});
        }
    }

    void tryConnect(const Joined& j, const Address& a) {
        if (!a.valid() || a == host.publicAddress() || (dht && dht->isExternal(a))) return;
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

    // --- Proximity ------------------------------------------------------------------

    void sendNear(Remote& r, bool gossip) {
        Bytes m{kNearMessage};
        Writer w(m);
        for (double v : position) w.f64(v);
        w.u8(std::binary_search(wanted.begin(), wanted.end(), r.id) ? 1 : 0);
        w.u16(look);
        std::vector<std::pair<double, const Remote*>> near;
        if (gossip) {
            // The peers nearest to it, as far as we know: where it should look.
            const Point& to = r.placed ? r.at : position;
            for (const Remote& o : remotes)
                if (&o != &r && o.placed && !o.member.empty()) near.push_back({distance2(o.at, to), &o});
            const size_t n = std::min({cfg.gossipEntries, kMaxGossip, near.size()});
            std::partial_sort(near.begin(), near.begin() + long(n), near.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
            near.resize(n);
        }
        w.u8(uint8_t(near.size()));
        for (const auto& [d, o] : near) {
            w.u64(o->id);
            for (int i = 0; i < 3; ++i) w.f32(float(o->at[size_t(i)] - position[size_t(i)]));
        }
        // The announcer sketch of every topic we share with it, with the
        // gossip (a newcomer hears it at once and falls silent in the DHT).
        std::vector<Joined*> shared;
        for (Joined& j : topics)
            if (gossip && std::find(r.member.begin(), r.member.end(), j.tag) != r.member.end() && shared.size() < 8)
                shared.push_back(&j);
        w.u8(uint8_t(shared.size()));
        for (Joined* j : shared) {
            elected(*j);
            w.bytes(j->tag.data(), 8);
            w.u8(uint8_t(j->low.size()));
            for (const Joined::Low& l : j->low) w.u64(l.id), w.u16(uint16_t(std::clamp(l.expires - now, 0.0, 65535.0)));
        }
        // Sightings, with every other position: a third first-hand (our links), the
        // rest what we were told, the farthest from the receiver first, one
        // per ring in turn, so what is far travels out hop after hop instead
        // of drowning in what is near; positions to the metre around us.
        std::vector<std::pair<uint32_t, Seen>> out;
        r.sightTurn = !r.sightTurn;
        if (cfg.sightings > 0 && r.sightTurn) {
            const size_t mine = std::max<size_t>(1, cfg.sightingsPerGossip / 3);
            std::vector<const Remote*> firsthand;
            for (const Remote& o : remotes)
                if (&o != &r && o.placed && !o.member.empty()) firsthand.push_back(&o);
            for (size_t i = 0; i < mine && !firsthand.empty(); ++i) {
                const size_t k = size_t(randomU64() % firsthand.size());
                out.push_back({uint32_t(firsthand[k]->id), {uint32_t(firsthand[k]->id), firsthand[k]->at, firsthand[k]->look, now}});
                firsthand.erase(firsthand.begin() + long(k));
            }
            const Point& to = r.placed ? r.at : position;
            std::vector<const Seen*> byRing[kRings];
            for (const Seen& s : seen) {
                if (s.id == uint32_t(r.id)) continue;
                const double d = std::sqrt(distance2(s.at, to));
                int ring = 0;
                while (ring + 1 < kRings && d >= double(32 << ring)) ++ring;
                byRing[ring].push_back(&s);
            }
            for (bool any = true; any && out.size() < cfg.sightingsPerGossip;) {
                any = false;
                for (int ring = kRings - 1; ring >= 0 && out.size() < cfg.sightingsPerGossip; --ring) {
                    auto& v = byRing[ring];
                    if (v.empty()) continue;
                    const size_t k = size_t(randomU64() % v.size());
                    out.push_back({v[k]->id, *v[k]});
                    v.erase(v.begin() + long(k));
                    any = true;
                }
            }
        }
        w.u8(uint8_t(out.size()));
        for (const auto& [id, s] : out) {
            w.u32(id);
            for (int i = 0; i < 3; ++i)
                w.u16(uint16_t(int16_t(std::clamp(std::round(s.at[size_t(i)] - position[size_t(i)]), -32767.0, 32767.0))));
            w.u16(s.look);
            w.u8(uint8_t(std::clamp(now - s.born, 0.0, 255.0)));
        }
        host.sendService(r.peer, kService, m.data(), m.size());
    }

    void onNear(Remote& r, const uint8_t* p, size_t n) {
        Reader rd(p, n);
        Point at;
        for (double& v : at) v = rd.f64();
        const uint8_t flags = rd.u8();
        const uint16_t how = rd.u16();
        const uint8_t count = rd.u8();
        if (!rd.ok() || !std::isfinite(at[0] + at[1] + at[2]) || count > kMaxGossip) return;
        const bool first = !r.placed;
        r.placed = true, r.at = at, r.wantsUs = flags & 1, r.look = how;
        for (uint8_t i = 0; i < count; ++i) {
            const uint64_t id = rd.u64();
            Point there;
            for (int k = 0; k < 3; ++k) there[size_t(k)] = at[size_t(k)] + rd.f32();
            if (!rd.ok() || !std::isfinite(there[0] + there[1] + there[2])) return;
            if (id == selfId || linked(id)) continue;
            Candidate* c = nullptr;
            for (Candidate& x : candidates)
                if (x.id == id) c = &x;
            if (!c) {
                if (candidates.size() >= kMaxCandidates)
                    candidates.erase(std::min_element(candidates.begin(), candidates.end(),
                                                      [](const Candidate& a, const Candidate& b) { return a.heard < b.heard; }));
                candidates.push_back({id, there, r.peer, now});
                continue;
            }
            c->at = there, c->via = r.peer, c->heard = now;
        }
        // Its announcer sketches: the lowest ranks of the crowd, merged into ours.
        if (rd.done()) return;
        const uint8_t sketches = rd.u8();
        for (uint8_t s = 0; s < sketches && rd.ok(); ++s) {
            Tag tag;
            if (const uint8_t* t = rd.bytes(8)) std::memcpy(tag.data(), t, 8);
            const uint8_t n = rd.u8();
            if (!rd.ok() || n > 64) return;
            Joined* j = nullptr;
            for (Joined& x : topics)
                if (x.tag == tag) j = &x;
            for (uint8_t i = 0; i < n; ++i) {
                const uint64_t id = rd.u64();
                const double left = rd.u16();
                if (!rd.ok()) return;
                if (j) mergeLow(*j, id, now + left);
            }
            if (j) j->sketchHeard = true;
        }
        // Its sightings: the crowd beyond, relative to where it stands.
        if (rd.done()) return;
        const uint8_t sights = rd.u8();
        for (uint8_t i = 0; i < sights; ++i) {
            const uint32_t id = rd.u32();
            Point there;
            for (int k = 0; k < 3; ++k) there[size_t(k)] = at[size_t(k)] + double(int16_t(rd.u16()));
            const uint16_t seenLook = rd.u16();
            const double age = rd.u8();
            if (!rd.ok()) return;
            sighted(id, there, seenLook, now - age);
        }
        // A newcomer: it learns at once who stands near it.
        if (first && positioned && cfg.neighbours > 0) r.nearDue = true;
    }

    void onIntroduce(Remote& r, const uint8_t* p, size_t n) {
        Reader rd(p, n);
        const uint64_t id = rd.u64();
        if (!rd.ok() || !rd.done() || cfg.neighbours == 0) return;
        r.introTokens = std::min(8.0, r.introTokens + (now - r.introAt) * cfg.introductionsPerSecond);
        r.introAt = now;
        if (r.introTokens < 1) return;
        r.introTokens -= 1;
        for (const Remote& t : remotes) {
            if (t.id != id || t.peer == r.peer) continue;
            // A topic both are in, which each side's admission will recognize.
            for (const Tag& tag : r.tags)
                if (std::find(t.tags.begin(), t.tags.end(), tag) != t.tags.end()) {
                    host.introduce(r.peer, t.peer, intentFor(tag, kIntroduced));
                    ++st.introductionsMade;
                    return;
                }
        }
    }

    bool linked(uint64_t id) const {
        for (const Remote& r : remotes)
            if (r.id == id) return true;
        return false;
    }

    void ask(const Candidate& c) {
        Remote* via = remote(c.via);
        if (!via) return;
        Asked* a = nullptr;
        for (Asked& x : asked)
            if (x.id == c.id) a = &x;
        if (a && now - a->at < double(1 << std::min(a->times, 4))) return;
        askTokens = std::min(8.0, askTokens + (now - askAt) * cfg.introductionsPerSecond);
        askAt = now;
        if (askTokens < 1) return;
        askTokens -= 1;
        if (!a) {
            if (asked.size() >= 256) asked.erase(asked.begin());
            asked.push_back({c.id, now, 0});
        } else {
            a->at = now, ++a->times;
        }
        Bytes m{kIntroduceMessage};
        Writer w(m);
        w.u64(c.id);
        host.sendService(via->peer, kService, m.data(), m.size());
        ++st.introductionsAsked;
    }

    // The `neighbours` nearest are linked (asked for when they are not);
    // `keepRatio` times as many are kept, so the edge of the set does not
    // churn; a link beyond that which the other side does not want either is
    // dropped.
    void select() {
        const double stale = 3 * cfg.gossipSeconds;
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& c) {
            return now - c.heard > stale || !remote(c.via) || linked(c.id);
        }), candidates.end());
        struct Option {
            double d;
            uint64_t id;
            Remote* linked;
            const Candidate* candidate;
        };
        std::vector<Option> all;
        for (Remote& r : remotes)
            if (r.placed && !r.member.empty()) all.push_back({distance2(r.at, position), r.id, &r, nullptr});
        for (const Candidate& c : candidates) all.push_back({distance2(c.at, position), c.id, nullptr, &c});
        std::sort(all.begin(), all.end(), [](const Option& a, const Option& b) { return a.d < b.d; });
        const size_t keep = std::max(cfg.neighbours, size_t(std::ceil(double(cfg.neighbours) * cfg.keepRatio)));
        wanted.clear();
        for (size_t i = 0; i < all.size() && i < keep; ++i) wanted.push_back(all[i].id);
        std::sort(wanted.begin(), wanted.end());
        for (size_t i = 0; i < all.size() && i < cfg.neighbours; ++i)
            if (!all[i].linked) ask(*all[i].candidate);
        for (size_t i = 0; i < all.size(); ++i) {
            Remote* r = all[i].linked;
            if (!r) continue;
            if (i < keep || r->wantsUs) {
                r->unwanted = -1;
            } else if (r->unwanted < 0) {
                r->unwanted = now;
            } else if (now - r->unwanted > cfg.pruneSeconds) {
                r->unwanted = -1;
                ++st.pruned;
                host.disconnect(r->peer);
            }
        }
        st.candidates = candidates.size();
        seen.erase(std::remove_if(seen.begin(), seen.end(), [&](const Seen& s) {
            return now - s.born > cfg.sightingSeconds || linked32(s.id);
        }), seen.end());
    }

    void proximity() {
        if (cfg.neighbours == 0 || !positioned) return;
        if (now - lastSelect >= 0.25) lastSelect = now, select();
        const bool gossip = now - lastGossip >= cfg.gossipSeconds;
        const bool tell = gossip || now - lastPosition >= cfg.positionSeconds || distance2(position, told) > 25.0 * 25.0;
        for (Remote& r : remotes) {
            if (r.member.empty() || !(tell || r.nearDue)) continue;
            sendNear(r, gossip || r.nearDue);
            r.nearDue = false;
        }
        if (tell) lastPosition = now, told = position;
        if (gossip) lastGossip = now;
    }

    // The DHT is the door: a topic with members asks it rarely, and only the
    // `announcers` members whose ids hash nearest the topic's key announce.
    // --- The announcer sketch (proximity) -------------------------------------------

    uint64_t rankOf(const Tag& tag, uint64_t id) const {
        uint8_t b[16];
        std::memcpy(b, tag.data(), 8);
        for (int i = 0; i < 8; ++i) b[8 + i] = uint8_t(id >> (8 * i));
        return netcode::hash64(b, sizeof b);
    }
    // Keeps the `announcers` lowest ranks heard of and not expired.
    void mergeLow(Joined& j, uint64_t id, double expires) {
        expires = std::min(expires, now + cfg.announcerSeconds);
        bool known = false;
        for (Joined::Low& l : j.low)
            if (l.id == id) known = true, l.expires = std::max(l.expires, expires);
        if (!known) j.low.push_back({id, rankOf(j.tag, id), expires});
        j.low.erase(std::remove_if(j.low.begin(), j.low.end(), [&](const Joined::Low& l) { return l.expires < now; }),
                    j.low.end());
        std::sort(j.low.begin(), j.low.end(), [](const Joined::Low& a, const Joined::Low& b) { return a.rank < b.rank; });
        if (j.low.size() > cfg.announcers) j.low.resize(cfg.announcers);
    }
    bool elected(Joined& j) {
        mergeLow(j, selfId, now + cfg.announcerSeconds);  // we stand for it as long as we are here
        for (const Joined::Low& l : j.low)
            if (l.id == selfId) return true;
        return false;
    }
    // k lowest of n uniform ranks: the k-th sits near k/n of the range.
    double population(const Joined& j) const {
        if (j.low.size() < cfg.announcers || cfg.announcers < 2) return double(j.low.size());
        return double(cfg.announcers - 1) / (double(j.low.back().rank) / 18446744073709551616.0);
    }

    // What we do in the DHT for a crowd's topic. Seeker: just arrived,
    // asking and announcing on our own shard only (a crowd's announcers are
    // on every shard: an arrival into a crowd costs one shard's nodes).
    // Finder: still alone after seekSeconds, findable on every shard, asking
    // our own. Announcer: findable, asking rarely (heal). Silent: linked and
    // not elected, nothing at all.
    void play(Joined& j, Joined::Role role) {
        if (role == j.role || !dht) return;
        j.role = role;
        if (role == Joined::Silent) {
            for (const NodeId& k : j.keys) dht->unwatch(k);
            return;
        }
        if (role == Joined::Seeker) {
            for (uint32_t s = 0; s < j.keys.size(); ++s)
                if (s != j.shard) dht->unwatch(j.keys[s]);
            j.refresh = cfg.refreshSeconds * (0.75 + 0.5 * double(randomU64() >> 11) / 9007199254740992.0);
            dht->watch(j.keys[j.shard], true, j.refresh);
            return;
        }
        // Arrivals are spread in time as in space: each finder refreshes on
        // its own rhythm, never in step with the others.
        const double jitter = 0.75 + 0.5 * double(randomU64() >> 11) / 9007199254740992.0;
        const double refresh = role == Joined::Finder ? cfg.refreshSeconds * jitter : cfg.populatedRefreshSeconds * jitter;
        j.refresh = refresh;
        for (uint32_t s = 0; s < j.keys.size(); ++s) dht->watch(j.keys[s], true, s == j.shard ? refresh : 1e9);
    }

    void tendDht() {
        st.announcing = 0, st.silent = 0, st.population = 0;
        if (!dht) return;
        for (Joined& j : topics) {
            if (cfg.neighbours > 0) {
                const size_t members = membersOf(j.tag);
                if (members == 0) {
                    j.linkedSince = -1;
                    if (j.aloneSince < 0) j.aloneSince = now;  // also the first tending after join()
                } else if (j.linkedSince < 0) {
                    j.linkedSince = now, j.aloneSince = -1;
                }
                const bool chosen = elected(j);
                Joined::Role role = members > 0 ? (chosen ? Joined::Announcer : Joined::Silent)
                                    : now - j.aloneSince < cfg.seekSeconds ? Joined::Seeker : Joined::Finder;
                // Just linked: stay as we were until the crowd's sketch says
                // who announces (a newcomer hears it within a gossip).
                if (members > 0 && !j.sketchHeard && now - j.linkedSince < 10) role = j.role;
                play(j, role);
                if (j.role == Joined::Finder && !j.keys.empty()) {
                    // Alone, findable: asking again matters only for whoever
                    // arrives at the same moment, so it slows down from every
                    // refreshSeconds to every populatedRefreshSeconds.
                    const double want = std::clamp((now - j.aloneSince) / 4, cfg.refreshSeconds, cfg.populatedRefreshSeconds);
                    if (want > j.refresh * 1.25 || want < j.refresh * .8) {
                        j.refresh = want;
                        dht->watch(j.keys[j.shard], true, want);
                    }
                }
                st.announcing += j.role != Joined::Silent && j.role != Joined::None;
                st.silent += j.role == Joined::Silent;
                st.population = std::max(st.population, population(j));
                continue;
            }
            const NodeId key = j.topic.dhtKey();
            auto rank = [&](uint64_t id) {
                uint8_t b[28];
                std::memcpy(b, key.data(), 20);
                for (int i = 0; i < 8; ++i) b[20 + i] = uint8_t(id >> (8 * i));
                return netcode::hash64(b, sizeof b);
            };
            size_t members = 0, nearer = 0;
            const uint64_t mine = rank(selfId);
            for (const Remote& r : remotes)
                if (std::find(r.member.begin(), r.member.end(), j.tag) != r.member.end()) ++members, nearer += rank(r.id) < mine;
            const bool announce = members == 0 || nearer < cfg.announcers;
            const double refresh = members == 0 ? cfg.refreshSeconds : cfg.populatedRefreshSeconds;
            if (announce != j.announcing || refresh != j.refresh) {
                dht->watch(key, announce, refresh);
                j.announcing = announce, j.refresh = refresh;
            }
            st.announcing += announce;
        }
    }

    // Introduced, not linked after relayAfterSeconds: no punching opens this
    // pair (symmetric NAT). The lower id links through the introducer, who
    // reaches both; the higher id too, twice as late, in case the lower never
    // heard the introduction (it travels unreliably); crossing attempts make
    // one link. The direct attempt goes on, and wins if it ever lands.
    void relays() {
        for (size_t i = 0; i < introductions.size();) {
            Introduction& in = introductions[i];
            const bool linkedNow = host.peerWithKey(in.key) != kNoPeer;
            if (linkedNow || now - in.at > 30 || !host.peer(in.via)) {
                introductions.erase(introductions.begin() + long(i));
                continue;
            }
            const uint64_t theirs = Identity::idOf(in.key);
            const double after = cfg.relayAfterSeconds * (selfId < theirs ? 1 : 2);
            if (!in.relayed && now - in.at >= after &&
                !Host::isRelayAddress(host.peer(in.via)->address)) {
                in.relayed = true;
                ++st.relayed;
                host.connect(Host::relayAddress(in.via, theirs), in.intent, &in.key);
            }
            ++i;
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
                for (Joined& j : topics) {
                    if (j.topic.dhtKey() != f.key && std::find(j.keys.begin(), j.keys.end(), f.key) == j.keys.end()) continue;
                    // A crowd meets newcomers through its own peers: an address
                    // from the DHT is only tried now and then, to heal a split.
                    if (cfg.neighbours > 0 && membersOf(j.tag) >= 2 && !host.peerAt(f.peer)) {
                        if (now - j.lastHeal < cfg.healSeconds) continue;
                        j.lastHeal = now;
                    }
                    tryConnect(j, f.peer);
                }
            }
            const Address ext = dht->externalAddress();
            if (ext.valid() && ext != lastExternal) host.notePublicAddress(lastExternal = ext);
        }
        beacons();
        if (now - lastTend >= 1.0) lastTend = now, tendDht();
        proximity();
        relays();
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
    impl_->topics.emplace_back();
    Impl::Joined& j = impl_->topics.back();
    j.topic = topic, j.tag = topic.tag(), j.refresh = impl_->cfg.refreshSeconds;
    if (impl_->cfg.neighbours > 0) {
        // A crowd's topic: its shard keys, ours chosen by our id; findable at once.
        const size_t shards = std::max<size_t>(1, impl_->cfg.dhtShards);
        for (uint32_t s = 0; s < shards; ++s) j.keys.push_back(topic.dhtKey(s));
        j.shard = uint32_t(impl_->selfId % shards);
        // Before the first poll our clock is not known yet: the first tending
        // starts the seeking time, not a clock still at zero.
        j.aloneSince = impl_->started < 0 ? -1 : impl_->now;
        impl_->play(j, Impl::Joined::Seeker);
    } else if (impl_->dht) {
        impl_->dht->watch(topic.dhtKey(), true, impl_->cfg.refreshSeconds);
    }
    impl_->lastBeacon = -1e18;
    for (auto& r : impl_->remotes) {
        impl_->sendTopics(r.peer);
        impl_->recompute(r);
    }
}

void Rendezvous::leave(const Topic& topic) {
    auto& ts = impl_->topics;
    const size_t before = ts.size();
    for (const Impl::Joined& j : ts)
        if (j.topic == topic && impl_->dht)
            for (const NodeId& k : j.keys) impl_->dht->unwatch(k);
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
void Rendezvous::setLook(uint16_t look) { impl_->look = look; }

std::vector<Sighting> Rendezvous::sightings() const {
    std::vector<Sighting> out;
    for (const auto& s : impl_->seen)
        if (impl_->now - s.born <= impl_->cfg.sightingSeconds && !impl_->linked32(s.id))
            out.push_back({s.id, s.at[0], s.at[1], s.at[2], s.look, impl_->now - s.born});
    return out;
}

void Rendezvous::setPosition(double x, double y, double z) {
    if (!std::isfinite(x + y + z)) return;
    impl_->positioned = true;
    impl_->position = {x, y, z};
}
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
