#include "saida/p2p/Host.hpp"

#include "saida/netcode/Wire.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>

namespace saida::p2p {
namespace {

using netcode::Reader;
using netcode::Writer;

// Datagram types (the first byte). Anything else is foreign: a DHT message
// (bencoded, so 'd') or garbage.
constexpr uint8_t kInit = 1, kResponse = 2, kFinish = 3, kData = 4;
constexpr uint8_t kVersion = 1;
// An Init is padded to this size; the Response it earns is never larger.
constexpr size_t kInitSize = 256;
// A Response is 9 + 32 + 48 + (8 + hello) + 16 bytes: with these sizes it
// stays below the 256 of the Init that asked for it.
constexpr size_t kMaxIntent = 64, kMaxHello = 120;
constexpr size_t kDataHeader = 9;  // type, receiver index, packet number (low 32 bits)
// The largest acknowledgement frame (type, two varints, mask) and the largest
// fragment header (type, seq, index, count, service, length): a fragment of
// fragmentBytes always fits a packet beside an acknowledgement.
constexpr size_t kAckReserve = 1 + 10 + 10 + 8, kFragmentHeader = 1 + 10 + 3 + 3 + 1 + 3;
constexpr uint64_t kReliableWindow = 4096;  // messages in flight
constexpr size_t kMaxGroups = 8;            // unreliable messages being reassembled
constexpr uint8_t kObservedService = 255;   // the host's own: "this is the address I see for you"

enum FrameType : uint8_t { fAck = 1, fMsg, fRel, fRelFrag, fMsgFrag, fPing, fClose, fIntro, fService };

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The full packet number nearest to the next expected one (QUIC's rule).
uint64_t expandPacketNumber(uint64_t highest, uint32_t low) {
    const uint64_t win = 1ULL << 32, half = win / 2, expected = highest + 1;
    uint64_t c = (expected & ~(win - 1)) | low;
    if (c + half <= expected && c < ~0ULL - win) return c + win;
    if (c > expected + half && c >= win) return c - win;
    return c;
}

struct Session {
    uint32_t local = 0, remote = 0;
    Aead tx, rx;
    Address address;     // where this session's packets go (follows rebinding)
    bool confirmed = false;  // the other side has used it
    Bytes finish;        // initiator: the third handshake message, until confirmed
    double nextFinish = 0, finishInterval = 0, lastReceive = 0;
};

struct FragmentRef {
    uint64_t seq;
    uint16_t index;
};

struct Sent {
    uint64_t pn;
    double at;
    uint32_t bytes;
    bool settled = false;
    std::vector<FragmentRef> fragments;
};

struct OutMessage {
    uint64_t seq;
    uint8_t service = 0;  // 0: the game's
    Bytes data;
    uint16_t count, sent = 0, acked = 0;
    std::vector<uint8_t> done;
};

struct InMessage {
    uint8_t service = 0;
    uint16_t count = 0, have = 0;
    std::vector<Bytes> parts;
    std::vector<uint8_t> got;
};

struct Group {
    uint64_t id;
    uint16_t count, have = 0;
    std::vector<Bytes> parts;
    std::vector<uint8_t> got;
    double at;
};

struct Peer {
    PeerInfo info;
    uint64_t instance = 0;
    std::vector<std::unique_ptr<Session>> sessions;
    bool dead = false;
    // Sending.
    uint64_t nextPn = 1, largestAcked = 0;
    std::deque<Sent> inflight;
    std::deque<OutMessage> out;  // reliable, oldest unacknowledged first
    uint64_t nextSeq = 0;
    std::deque<FragmentRef> resend;
    std::deque<Bytes> frames;    // encoded unreliable and control frames, in order
    size_t framesBytes = 0;
    uint64_t nextGroup = 0;
    double lastSend = -1e9, rto = 0.25, srtt = 0, rttvar = 0;
    bool rttKnown = false;
    // Receiving: bit i of `window` is packet `highest - i`.
    uint64_t highest = 0;
    uint64_t window[4] = {};
    bool anyReceived = false, ackPending = false;
    double highestAt = 0, ackDue = 0;
    uint64_t deliver = 0;
    std::map<uint64_t, InMessage> in;
    size_t inBytes = 0;
    std::vector<Group> groups;
    double introTokens = 8, introAt = 0;
    // Packets declared lost lately: an acknowledgement that still arrives for
    // one makes it a late packet, not a lost one, in the statistics.
    std::deque<uint64_t> suspects;

    Session* sender() {
        for (auto it = sessions.rbegin(); it != sessions.rend(); ++it)
            if ((*it)->confirmed) return it->get();
        return sessions.empty() ? nullptr : sessions.back().get();
    }
    bool seen(uint64_t pn) const {
        if (!anyReceived || pn > highest) return false;
        const uint64_t i = highest - pn;
        return i >= 256 || (window[i / 64] >> (i % 64) & 1);
    }
    void markReceived(uint64_t pn, double now) {
        if (!anyReceived) {
            anyReceived = true, highest = pn, highestAt = now;
            window[0] = 1, window[1] = window[2] = window[3] = 0;
            return;
        }
        if (pn > highest) {
            const uint64_t s = pn - highest;
            if (s >= 256) {
                window[0] = window[1] = window[2] = window[3] = 0;
            } else {
                const unsigned words = unsigned(s / 64), bits = unsigned(s % 64);
                for (int w = 3; w >= 0; --w) {
                    uint64_t v = w - int(words) >= 0 ? window[w - int(words)] << bits : 0;
                    if (bits && w - int(words) - 1 >= 0) v |= window[w - int(words) - 1] >> (64 - bits);
                    window[w] = v;
                }
            }
            window[0] |= 1;
            highest = pn, highestAt = now;
            return;
        }
        const uint64_t i = highest - pn;
        window[i / 64] |= 1ULL << (i % 64);
    }
    // Packets highest-1 .. highest-64.
    uint64_t ackMask() const { return window[0] >> 1 | window[1] << 63; }
};

struct Pending {
    bool initiator = false;
    Address address;
    uint32_t local = 0, remote = 0;  // remote: the initiator's index (responder side)
    NoiseXX noise;
    Bytes intent, packet;
    double started = 0, next = 0, interval = 0;
    double sentAt = 0;  // last (re)send of our handshake packet
    int sends = 1;      // a round trip is measured only on an unrepeated exchange (Karn)
    bool expectSet = false;
    Key expect{};
};

} // namespace

uint64_t Identity::idOf(const Key& pub) {
    static const char tag[] = "saida-p2p id";
    const Digest d = sha256(tag, sizeof tag - 1, pub.data(), pub.size());
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id |= uint64_t(d[size_t(i)]) << (8 * i);
    return id ? id : 1;
}

const char* reasonName(Reason r) {
    static const char* names[] = {"none", "closed", "timeout", "restarted", "connect timeout", "wrong key",
                                  "refused", "malformed", "bad version", "unknown index", "authentication failed",
                                  "replayed", "rate limited", "too many peers", "too many handshakes",
                                  "not admitted", "handshake failed", "too large", "queue full", "unknown peer",
                                  "foreign"};
    static_assert(sizeof names / sizeof *names == size_t(Reason::Count), "one name per reason");
    return size_t(r) < size_t(Reason::Count) ? names[size_t(r)] : "?";
}

struct Host::Impl {
    DatagramSocket& socket;
    Identity identity;
    HostConfig cfg;
    Receiver receiver;
    Admission admission;
    Foreign foreign;
    std::vector<std::function<void(const HostEvent&)>> listeners;
    std::vector<HostEvent> unheard;
    Service services[256];
    double now = 0;
    bool clockKnown = false;  // a connect() before the first poll() starts its clock then
    uint64_t instance = randomU64();
    Bytes prologue;
    std::vector<HostEvent> events;
    std::vector<std::unique_ptr<Peer>> peers;
    std::unordered_map<uint32_t, std::pair<Peer*, Session*>> sessions;
    std::vector<std::unique_ptr<Pending>> pendings;
    PeerHandle nextHandle = 1;
    double tokens, tokensAt = 0;
    struct Vote {
        Address address;
        uint32_t count;
        double at;
    };
    std::vector<Vote> votes;
    HostCounters counters;
    size_t capacity, fragmentBytes;
    uint8_t rx[65536];
    Bytes plain, opened, fragment, wire, scratch;  // opened: what poll() decrypted

    Impl(DatagramSocket& s, const Identity& id, const HostConfig& c) : socket(s), identity(id), cfg(c) {
        cfg.mtu = std::clamp<size_t>(cfg.mtu, 576, 65000);
        if (cfg.hello.size() > kMaxHello) cfg.hello.resize(kMaxHello);
        cfg.maxPeers = std::max<size_t>(cfg.maxPeers, 1);
        cfg.maxPacketsPerFlush = std::max<size_t>(cfg.maxPacketsPerFlush, 1);
        cfg.retrySeconds = std::max(cfg.retrySeconds, 0.01);
        capacity = cfg.mtu - kDataHeader - Aead::kTag;
        fragmentBytes = capacity - kAckReserve - kFragmentHeader;
        cfg.maxMessage = std::min(cfg.maxMessage, 65535 * fragmentBytes);  // fragment indices are 16-bit
        cfg.maxUnreliable = std::min(cfg.maxUnreliable, 64 * fragmentBytes);
        tokens = cfg.handshakeRate;
        static const char tag[] = "saida-p2p/1 ";
        prologue.assign(tag, tag + sizeof tag - 1);
        prologue.insert(prologue.end(), cfg.application.begin(), cfg.application.end());
    }

    void refuse(Reason r) { ++counters.refused[size_t(r)]; }
    void event(HostEventType t, PeerHandle p, const Address& a, Reason r, Bytes intent = {}) {
        events.push_back({t, p, a, r, std::move(intent)});
        if (!listeners.empty()) unheard.push_back(events.back());
    }
    bool sendDatagram(const Address& to, const uint8_t* data, size_t size) {
        ++counters.datagramsSent;
        counters.bytesSent += size;
        return socket.send(to, data, size);
    }

    Peer* find(PeerHandle h) {
        for (auto& p : peers)
            if (p->info.handle == h && !p->dead) return p.get();
        return nullptr;
    }
    Peer* findKey(const Key& k) {
        for (auto& p : peers)
            if (!p->dead && p->info.key == k) return p.get();
        return nullptr;
    }
    Peer* findAt(const Address& a) {
        for (auto& p : peers) {
            if (p->dead) continue;
            if (p->info.address == a) return p.get();
            for (auto& s : p->sessions)
                if (s->address == a) return p.get();
        }
        return nullptr;
    }
    uint32_t newIndex() {
        for (;;) {
            uint32_t i;
            randomBytes(&i, sizeof i);
            if (i == 0 || sessions.count(i)) continue;
            bool used = false;
            for (auto& p : pendings) used = used || p->local == i;
            if (!used) return i;
        }
    }

    // --- Handshakes ---------------------------------------------------------------

    bool connect(const Address& to, const Bytes& intent, const Key* expect) {
        if (!to.valid() || intent.size() > kMaxIntent) return refuse(Reason::Malformed), false;
        if (findAt(to)) return true;
        for (auto& p : pendings)
            if (p->address == to) {
                if (p->initiator && expect) p->expectSet = true, p->expect = *expect;
                return true;
            }
        if (pendings.size() >= cfg.maxHandshakes) return refuse(Reason::TooManyHandshakes), false;
        auto p = std::make_unique<Pending>();
        p->initiator = true;
        p->address = to;
        p->local = newIndex();
        p->intent = intent;
        if (expect) p->expectSet = true, p->expect = *expect;
        p->noise.begin(true, identity.keys, prologue.data(), prologue.size());
        Bytes msg;
        p->noise.write1(intent.data(), intent.size(), msg);
        p->packet.assign(kInitSize, 0);
        p->packet[0] = kInit, p->packet[1] = kVersion;
        put32(&p->packet[2], p->local);
        p->packet[6] = uint8_t(intent.size());
        std::copy(msg.begin(), msg.end(), p->packet.begin() + 7);
        p->started = clockKnown ? now : -1;
        p->sentAt = now;
        p->interval = cfg.retrySeconds;
        p->next = now + p->interval;
        ++counters.handshakesStarted;
        sendDatagram(to, p->packet.data(), p->packet.size());
        pendings.push_back(std::move(p));
        return true;
    }

    void onInit(const Address& from, const uint8_t* d, size_t n) {
        if (n != kInitSize) return refuse(Reason::Malformed);
        if (d[1] != kVersion) return refuse(Reason::BadVersion);
        const uint32_t sender = get32(d + 2);
        const size_t intentSize = d[6];
        if (intentSize > kMaxIntent) return refuse(Reason::Malformed);
        const uint8_t* msg = d + 7;
        bool expectSet = false;
        Key expect{};
        for (size_t i = 0; i < pendings.size(); ++i) {
            Pending& p = *pendings[i];
            if (p.address != from) continue;
            if (!p.initiator) {
                // A retransmitted Init: the stored Response again, no new work.
                if (p.remote == sender) return void((++p.sends, sendDatagram(from, p.packet.data(), p.packet.size())));
                // A new attempt (the other side restarted): it replaces the old one.
                pendings.erase(pendings.begin() + long(i--));
                continue;
            }
            // Both sides connected at once. Answer theirs and keep ours: either
            // may be the one a NAT let through, and if both are, the two
            // sessions make one peer.
            if (std::memcmp(p.noise.localEphemeral().data(), msg, 32) == 0) return;  // our own, looped back
            if (p.expectSet) expectSet = true, expect = p.expect;
        }
        for (auto& [index, ps] : sessions)
            if (ps.second->remote == sender && ps.second->address == from) return;  // late retransmit
        if (pendings.size() >= cfg.maxHandshakes) return refuse(Reason::TooManyHandshakes);
        size_t live = 0;
        for (auto& p : peers) live += !p->dead;
        if (live >= cfg.maxPeers && !findAt(from)) return refuse(Reason::TooManyPeers);
        Bytes intent(msg + 32, msg + 32 + intentSize);
        if (admission && !admission(from, intent)) return refuse(Reason::NotAdmitted);
        tokens = std::min(cfg.handshakeRate, tokens + (now - tokensAt) * cfg.handshakeRate);
        tokensAt = now;
        if (tokens < 1) return refuse(Reason::RateLimited);
        tokens -= 1;

        auto p = std::make_unique<Pending>();
        p->address = from;
        p->local = newIndex();
        p->remote = sender;
        p->intent = intent;
        p->expectSet = expectSet, p->expect = expect;
        p->started = p->sentAt = now;
        p->noise.begin(false, identity.keys, prologue.data(), prologue.size());
        Bytes payload, out;
        if (!p->noise.read1(msg, 32 + intentSize, payload)) return refuse(Reason::HandshakeFailed);
        writeHello(payload);
        if (!p->noise.write2(payload.data(), payload.size(), out)) return refuse(Reason::HandshakeFailed);
        p->packet.resize(9);
        p->packet[0] = kResponse;
        put32(&p->packet[1], p->local);
        put32(&p->packet[5], sender);
        p->packet.insert(p->packet.end(), out.begin(), out.end());
        ++counters.handshakesAnswered;
        sendDatagram(from, p->packet.data(), p->packet.size());
        pendings.push_back(std::move(p));
    }

    void writeHello(Bytes& payload) {
        payload.clear();
        for (int i = 0; i < 8; ++i) payload.push_back(uint8_t(instance >> (8 * i)));
        payload.insert(payload.end(), cfg.hello.begin(), cfg.hello.end());
    }

    Pending* pendingByIndex(uint32_t local, bool initiator) {
        for (auto& p : pendings)
            if (p->local == local && p->initiator == initiator) return p.get();
        return nullptr;
    }
    void erasePending(Pending* p) {
        pendings.erase(std::remove_if(pendings.begin(), pendings.end(), [&](auto& q) { return q.get() == p; }),
                       pendings.end());
    }

    void onResponse(const Address& from, const uint8_t* d, size_t n) {
        if (n < 9 + 32 + 48 + Aead::kTag + 8) return refuse(Reason::Malformed);
        const uint32_t sender = get32(d + 1), receiver = get32(d + 5);
        Pending* p = pendingByIndex(receiver, true);
        if (!p) return refuse(Reason::UnknownIndex);
        NoiseXX trial = p->noise;  // a forged Response must not spoil the real one
        Bytes payload;
        if (!trial.read2(d + 9, n - 9, payload) || payload.size() < 8 || payload.size() > 8 + kMaxHello)
            return refuse(Reason::AuthFailed);
        if (p->expectSet && trial.remoteStatic() != p->expect) {
            event(HostEventType::ConnectFailed, kNoPeer, p->address, Reason::WrongKey);
            return erasePending(p), refuse(Reason::WrongKey);
        }
        Bytes mine, out;
        writeHello(mine);
        if (!trial.write3(mine.data(), mine.size(), out)) return refuse(Reason::HandshakeFailed);
        Bytes finish(5);
        finish[0] = kFinish;
        put32(&finish[1], sender);
        finish.insert(finish.end(), out.begin(), out.end());
        const Bytes intent = p->intent;
        const uint32_t local = p->local;
        const double rtt = p->sends == 1 && p->started >= 0 ? now - p->sentAt : -1;
        erasePending(p);
        Session* s = establish(true, trial, from, local, sender, intent, payload, rtt);
        if (!s) return;
        s->finish = std::move(finish);
        s->finishInterval = cfg.retrySeconds;
        s->nextFinish = now + s->finishInterval;
        sendDatagram(from, s->finish.data(), s->finish.size());
    }

    void onFinish(const Address& from, const uint8_t* d, size_t n) {
        if (n < 5 + 48 + Aead::kTag + 8) return refuse(Reason::Malformed);
        const uint32_t receiver = get32(d + 1);
        Pending* p = pendingByIndex(receiver, false);
        if (!p) {
            // A repeated Finish: our first packets were lost; answer at once.
            auto it = sessions.find(receiver);
            if (it != sessions.end()) {
                it->second.first->ackPending = true;
                it->second.first->ackDue = now;
                it->second.first->frames.push_back({fPing});
                return;
            }
            return refuse(Reason::UnknownIndex);
        }
        NoiseXX trial = p->noise;
        Bytes payload;
        if (!trial.read3(d + 5, n - 5, payload) || payload.size() < 8 || payload.size() > 8 + kMaxHello)
            return refuse(Reason::AuthFailed);
        if (p->expectSet && trial.remoteStatic() != p->expect) {
            event(HostEventType::ConnectFailed, kNoPeer, p->address, Reason::WrongKey);
            return erasePending(p), refuse(Reason::WrongKey);
        }
        const Bytes intent = p->intent;
        const uint32_t local = p->local, remote = p->remote;
        const double rtt = p->sends == 1 ? now - p->sentAt : -1;
        erasePending(p);
        establish(false, trial, from, local, remote, intent, payload, rtt);
    }

    // `rtt`: the handshake's own round trip, the link's first sample (< 0: none).
    Session* establish(bool initiator, const NoiseXX& noise, const Address& from, uint32_t local, uint32_t remote,
                       const Bytes& intent, const Bytes& payload, double rtt) {
        const Key& key = noise.remoteStatic();
        if (key == identity.keys.pub) return refuse(Reason::HandshakeFailed), nullptr;  // ourselves
        uint64_t remoteInstance = 0;
        for (int i = 0; i < 8; ++i) remoteInstance |= uint64_t(payload[size_t(i)]) << (8 * i);
        Peer* peer = findKey(key);
        if (peer && peer->instance != remoteInstance) {
            // The same identity, a new process: the old link is over.
            kill(*peer, Reason::Restarted);
            peer = nullptr;
        }
        if (!peer) {
            auto fresh = std::make_unique<Peer>();
            peer = fresh.get();
            peer->instance = remoteInstance;
            PeerInfo& i = peer->info;
            i.handle = nextHandle++;
            if (nextHandle == kNoPeer) ++nextHandle;
            i.address = from;
            i.key = key;
            i.id = Identity::idOf(key);
            i.hello.assign(payload.begin() + 8, payload.end());
            i.intent = intent;
            i.initiator = initiator;
            i.connectedAt = i.lastReceive = now;
            peers.push_back(std::move(fresh));
            event(HostEventType::Connected, i.handle, from, Reason::None, intent);
        }
        if (rtt >= 0 && !peer->rttKnown) rttSample(*peer, rtt);
        // Our own attempt towards the same address is now redundant.
        pendings.erase(std::remove_if(pendings.begin(), pendings.end(),
                                      [&](auto& q) { return q->initiator && q->address == from; }),
                       pendings.end());
        auto s = std::make_unique<Session>();
        s->local = local, s->remote = remote;
        s->address = from;
        s->confirmed = !initiator;
        s->lastReceive = now;
        noise.split(s->tx, s->rx);
        Session* raw = s.get();
        peer->sessions.push_back(std::move(s));
        sessions[local] = {peer, raw};
        ++counters.handshakesCompleted;
        queueObserved(*peer, from);
        return raw;
    }

    // Reliable: a peer behind a NAT has no other way to learn its public
    // address, and without it nobody can be told where to punch.
    void queueObserved(Peer& peer, const Address& seen) {
        uint8_t c[18];
        seen.writeCompact(c);
        send(peer.info.handle, c, seen.compactSize(), Channel::Reliable, kObservedService);
    }

    void kill(Peer& peer, Reason why) {
        if (peer.dead) return;
        peer.dead = true;
        event(HostEventType::Disconnected, peer.info.handle, peer.info.address, why);
    }

    // --- Data ---------------------------------------------------------------------

    void onData(const Address& from, const uint8_t* d, size_t n) {
        if (n < kDataHeader + Aead::kTag) return refuse(Reason::Malformed);
        auto it = sessions.find(get32(d + 1));
        if (it == sessions.end()) return refuse(Reason::UnknownIndex);
        Peer& peer = *it->second.first;
        Session& s = *it->second.second;
        if (peer.dead) return;
        const uint64_t pn = expandPacketNumber(peer.highest, get32(d + 5));
        if (peer.seen(pn)) return refuse(Reason::Replayed);
        opened.resize(n - kDataHeader - Aead::kTag);
        if (!s.rx.open(pn, d, kDataHeader, d + kDataHeader, n - kDataHeader, opened.data()))
            return refuse(Reason::AuthFailed);
        peer.markReceived(pn, now);
        s.lastReceive = peer.info.lastReceive = now;
        s.confirmed = true;
        s.finish.clear();
        if (s.address != from) s.address = from;  // the NAT rebound: follow it
        if (peer.sender() == &s) peer.info.address = s.address;
        ++peer.info.packetsReceived;
        peer.info.bytesReceived += n;
        Reader r(opened.data(), opened.size());
        bool eliciting = false;
        while (!r.done() && !peer.dead) {
            const uint8_t type = r.u8();
            eliciting |= type != fAck && type != fClose;
            if (!readFrame(peer, type, r)) return refuse(Reason::Malformed);
        }
        if (eliciting && !peer.ackPending) peer.ackPending = true, peer.ackDue = now + cfg.ackDelaySeconds;
    }

    bool readFrame(Peer& peer, uint8_t type, Reader& r) {
        switch (type) {
        case fAck: {
            const uint64_t largest = r.var(), delayUs = r.var(), mask = r.u64();
            if (!r.ok()) return false;
            onAck(peer, largest, double(delayUs) * 1e-6, mask);
            return true;
        }
        case fMsg: {
            const uint64_t size = r.var();
            const uint8_t* data = size <= cfg.maxUnreliable ? r.bytes(size_t(size)) : nullptr;
            if (!data) return false;
            if (receiver) receiver(peer.info.handle, data, size_t(size), Channel::Unreliable);
            return true;
        }
        case fRel:
        case fRelFrag:
        case fService: {
            const uint64_t seq = r.var();
            uint64_t index = 0, count = 1, service = 0;
            if (type == fRelFrag) index = r.var(), count = r.var();
            if (type == fService) service = r.u8();
            const uint64_t size = r.var();
            const uint8_t* data = size <= cfg.mtu ? r.bytes(size_t(size)) : nullptr;
            if (!data || count == 0 || count * fragmentBytes > cfg.maxMessage + fragmentBytes || index >= count ||
                (type == fService && !service))
                return false;
            onReliable(peer, seq, uint8_t(service), uint16_t(index), uint16_t(count), data, size_t(size));
            return true;
        }
        case fMsgFrag: {
            const uint64_t id = r.var(), index = r.var(), count = r.var(), size = r.var();
            const uint8_t* data = size <= cfg.mtu ? r.bytes(size_t(size)) : nullptr;
            if (!data || count < 2 || count > 64 || count * fragmentBytes > cfg.maxUnreliable + fragmentBytes ||
                index >= count)
                return false;
            onGroup(peer, id, uint16_t(index), uint16_t(count), data, size_t(size));
            return true;
        }
        case fPing:
            return true;
        case fClose:
            r.var();
            kill(peer, Reason::Closed);
            return r.ok();
        case fIntro: {
            const uint8_t asize = r.u8();
            const uint8_t* a = r.bytes(asize);
            const uint8_t* k = r.bytes(32);
            const uint64_t isize = r.var();
            const uint8_t* intent = isize <= kMaxIntent ? r.bytes(size_t(isize)) : nullptr;
            if (!a || !k || (!intent && isize)) return false;
            Key key;
            std::memcpy(key.data(), k, 32);
            onIntroduction(peer, Address::readCompact(a, asize), key, Bytes(intent, intent + isize));
            return true;
        }
        default:
            return false;
        }
    }

    void onAck(Peer& peer, uint64_t largest, double delay, uint64_t mask) {
        if (largest >= peer.nextPn) return;
        auto acked = [&](uint64_t pn) {
            const uint64_t back = largest - pn;
            return pn <= largest && (back == 0 || (back <= 64 && (mask >> (back - 1) & 1)));
        };
        for (auto it = peer.suspects.begin(); it != peer.suspects.end();) {
            if (!acked(*it)) {
                ++it;
                continue;
            }
            --peer.info.packetsLost;
            peer.info.loss = std::max(0.0, peer.info.loss - 1.0 / 128.0);
            it = peer.suspects.erase(it);
        }
        for (Sent& s : peer.inflight) {
            if (s.settled || !acked(s.pn)) continue;
            if (s.pn == largest) {
                const double sample = now - s.at;
                rttSample(peer, sample > delay ? sample - delay : sample);
            }
            settle(peer, s, false);
        }
        peer.largestAcked = std::max(peer.largestAcked, largest);
        // Lost: three later packets acknowledged and older than 9/8 of the
        // round trip (QUIC's thresholds; reordering by jitter is not loss).
        const double reorder = 1.125 * std::max(peer.srtt, peer.rttKnown ? 0.0 : 0.1);
        for (Sent& s : peer.inflight)
            if (!s.settled && s.pn + 3 <= peer.largestAcked && now - s.at > reorder) settle(peer, s, true);
        compact(peer);
    }

    void rttSample(Peer& peer, double r) {
        if (!peer.rttKnown) {
            peer.srtt = r, peer.rttvar = r / 2, peer.rttKnown = true;
        } else {
            peer.rttvar = 0.75 * peer.rttvar + 0.25 * std::fabs(peer.srtt - r);
            peer.srtt = 0.875 * peer.srtt + 0.125 * r;
        }
        peer.rto = std::clamp(peer.srtt + 4 * peer.rttvar + cfg.ackDelaySeconds, 0.02, 2.0);
        peer.info.rtt = peer.srtt;
        peer.info.rttVariance = peer.rttvar;
    }

    void settle(Peer& peer, Sent& s, bool lost) {
        s.settled = true;
        peer.info.inflightBytes -= std::min<size_t>(peer.info.inflightBytes, s.bytes);
        peer.info.loss += ((lost ? 1.0 : 0.0) - peer.info.loss) / 128.0;
        if (lost) {
            ++peer.info.packetsLost;
            peer.suspects.push_back(s.pn);
            if (peer.suspects.size() > 256) peer.suspects.pop_front();
        }
        for (const FragmentRef& f : s.fragments) {
            OutMessage* m = message(peer, f.seq);
            if (!m || m->done[f.index]) continue;
            if (lost) {
                peer.resend.push_back(f);
                ++peer.info.retransmits;
            } else {
                m->done[f.index] = 1;
                ++m->acked;
            }
        }
        while (!peer.out.empty() && peer.out.front().acked == peer.out.front().count) {
            peer.info.queuedBytes -= std::min(peer.info.queuedBytes, peer.out.front().data.size());
            peer.out.pop_front();
        }
    }

    void compact(Peer& peer) {
        while (!peer.inflight.empty() && peer.inflight.front().settled) peer.inflight.pop_front();
    }

    OutMessage* message(Peer& peer, uint64_t seq) {
        if (peer.out.empty() || seq < peer.out.front().seq) return nullptr;
        const uint64_t i = seq - peer.out.front().seq;
        return i < peer.out.size() ? &peer.out[size_t(i)] : nullptr;
    }

    void deliverReliable(Peer& peer, uint8_t service, const uint8_t* data, size_t size) {
        if (service == kObservedService) {
            notePublic(Address::readCompact(data, size));
        } else if (service) {
            if (services[service]) services[service](peer.info.handle, data, size);
        } else if (receiver) {
            receiver(peer.info.handle, data, size, Channel::Reliable);
        }
    }

    void onReliable(Peer& peer, uint64_t seq, uint8_t service, uint16_t index, uint16_t count, const uint8_t* data,
                    size_t size) {
        if (seq < peer.deliver || seq >= peer.deliver + kReliableWindow) return;
        if (seq == peer.deliver && count == 1) {
            ++peer.deliver;
            deliverReliable(peer, service, data, size);
        } else {
            // The packet is already acknowledged: a fragment cannot be dropped
            // and fetched again. An honest sender never queues past its own
            // maxQueuedBytes; a peer that makes us hold more is cut off. The
            // bookkeeping of a message counts too (one Bytes per fragment).
            auto found = peer.in.find(seq);
            const size_t bookkeeping = found == peer.in.end() ? count * (sizeof(Bytes) + 1) : 0;
            if (peer.inBytes + bookkeeping + size > 2 * cfg.maxQueuedBytes)
                return refuse(Reason::QueueFull), kill(peer, Reason::QueueFull);
            InMessage& m = peer.in[seq];
            if (m.count == 0) {
                m.count = count, m.service = service, m.parts.resize(count), m.got.assign(count, 0);
                peer.inBytes += bookkeeping;
            }
            if (m.count != count || m.got[index]) return;
            m.parts[index].assign(data, data + size);
            m.got[index] = 1;
            ++m.have;
            peer.inBytes += size;
        }
        while (!peer.dead && !peer.in.empty() && peer.in.begin()->first == peer.deliver &&
               peer.in.begin()->second.have == peer.in.begin()->second.count) {
            InMessage m = std::move(peer.in.begin()->second);
            peer.in.erase(peer.in.begin());
            ++peer.deliver;
            scratch.clear();
            peer.inBytes -= m.count * (sizeof(Bytes) + 1);
            for (Bytes& p : m.parts) {
                peer.inBytes -= p.size();
                scratch.insert(scratch.end(), p.begin(), p.end());
            }
            const Bytes whole = scratch;  // the receiver may send, reusing scratch
            deliverReliable(peer, m.service, whole.data(), whole.size());
        }
    }

    void onGroup(Peer& peer, uint64_t id, uint16_t index, uint16_t count, const uint8_t* data, size_t size) {
        Group* g = nullptr;
        for (Group& x : peer.groups)
            if (x.id == id) g = &x;
        if (!g) {
            if (peer.groups.size() >= kMaxGroups) peer.groups.erase(peer.groups.begin());
            peer.groups.push_back({id, count, 0, std::vector<Bytes>(count), std::vector<uint8_t>(count, 0), now});
            g = &peer.groups.back();
        }
        if (g->count != count || g->got[index]) return;
        g->parts[index].assign(data, data + size);
        g->got[index] = 1;
        if (++g->have < g->count) return;
        Bytes whole;
        for (Bytes& p : g->parts) whole.insert(whole.end(), p.begin(), p.end());
        peer.groups.erase(peer.groups.begin() + (g - peer.groups.data()));
        if (receiver) receiver(peer.info.handle, whole.data(), whole.size(), Channel::Unreliable);
    }

    void onIntroduction(Peer& from, const Address& at, const Key& key, const Bytes& intent) {
        from.introTokens = std::min(8.0, from.introTokens + (now - from.introAt) * cfg.introductionsPerSecond);
        from.introAt = now;
        if (from.introTokens < 1) return refuse(Reason::RateLimited);
        from.introTokens -= 1;
        if (!at.valid() || key == identity.keys.pub || findKey(key)) return;
        if (admission && !admission(at, intent)) return refuse(Reason::NotAdmitted);
        event(HostEventType::Introduced, from.info.handle, at, Reason::None, intent);
        connect(at, intent, &key);
    }

    void notePublic(const Address& a) {
        if (!a.valid()) return;
        for (Vote& v : votes)
            if (v.address == a) return void((++v.count, v.at = now));
        if (votes.size() >= 16) votes.erase(std::min_element(votes.begin(), votes.end(), [](auto& x, auto& y) {
            return x.at < y.at;
        }));
        votes.push_back({a, 1, now});
    }

    // --- Sending ------------------------------------------------------------------

    bool send(PeerHandle h, const uint8_t* data, size_t size, Channel channel, uint8_t service = 0) {
        Peer* peer = find(h);
        if (!peer) return refuse(Reason::UnknownPeer), false;
        if (channel == Channel::Reliable) {
            if (size > (service ? fragmentBytes - 1 : cfg.maxMessage)) return refuse(Reason::TooLarge), false;
            if (peer->info.queuedBytes + size > cfg.maxQueuedBytes) return refuse(Reason::QueueFull), false;
            OutMessage m;
            m.seq = peer->nextSeq++;
            m.service = service;
            m.data.assign(data, data + size);
            m.count = uint16_t(size <= fragmentBytes ? 1 : (size + fragmentBytes - 1) / fragmentBytes);
            m.done.assign(m.count, 0);
            peer->info.queuedBytes += size;
            peer->out.push_back(std::move(m));
            return true;
        }
        if (size > cfg.maxUnreliable) return refuse(Reason::TooLarge), false;
        if (peer->framesBytes + size > cfg.maxQueuedBytes) return refuse(Reason::QueueFull), false;
        if (size <= fragmentBytes) {
            Bytes f{fMsg};
            Writer w(f);
            w.var(size);
            w.bytes(data, size);
            peer->framesBytes += f.size();
            peer->frames.push_back(std::move(f));
            return true;
        }
        const uint64_t id = peer->nextGroup++;
        const size_t count = (size + fragmentBytes - 1) / fragmentBytes;
        for (size_t i = 0; i < count; ++i) {
            const size_t at = i * fragmentBytes, n = std::min(fragmentBytes, size - at);
            Bytes f{fMsgFrag};
            Writer w(f);
            w.var(id), w.var(i), w.var(count), w.var(n);
            w.bytes(data + at, n);
            peer->framesBytes += f.size();
            peer->frames.push_back(std::move(f));
        }
        return true;
    }

    void encodeFragment(const OutMessage& m, uint16_t index, Bytes& f) {
        f.clear();
        Writer w(f);
        const size_t at = size_t(index) * fragmentBytes;
        const size_t n = m.count == 1 ? m.data.size() : std::min(fragmentBytes, m.data.size() - at);
        w.u8(m.service ? fService : m.count == 1 ? fRel : fRelFrag);
        w.var(m.seq);
        if (m.count > 1) w.var(index), w.var(m.count);
        if (m.service) w.u8(m.service);
        w.var(n);
        w.bytes(m.data.data() + at, n);
    }

    bool wantsSend(Peer& peer) const {
        if (!peer.frames.empty() || !peer.resend.empty()) return true;
        if (peer.ackPending && now >= peer.ackDue) return true;
        if (now - peer.lastSend >= cfg.keepaliveSeconds) return true;
        for (const OutMessage& m : peer.out)
            if (m.sent < m.count) return true;
        return false;
    }

    void flushPeer(Peer& peer, bool force) {
        Session* s = peer.sender();
        if (!s || peer.dead) return;
        if (!force && !wantsSend(peer)) return;
        if (peer.frames.empty() && peer.resend.empty() && now - peer.lastSend >= cfg.keepaliveSeconds)
            peer.frames.push_back({fPing});
        size_t nextMessage = 0;
        for (size_t packets = 0; packets < cfg.maxPacketsPerFlush; ++packets) {
            plain.clear();
            Sent sent{peer.nextPn, now, 0, false, {}};
            if (peer.anyReceived) {
                Writer w(plain);
                w.u8(fAck);
                w.var(peer.highest);
                w.var(uint64_t(std::max(0.0, now - peer.highestAt) * 1e6));
                w.u64(peer.ackMask());
            }
            const size_t ackOnly = plain.size();
            auto fits = [&](size_t n) { return plain.size() + n <= capacity; };
            while (!peer.frames.empty() && fits(peer.frames.front().size())) {
                const Bytes& f = peer.frames.front();
                plain.insert(plain.end(), f.begin(), f.end());
                if (f[0] == fMsg || f[0] == fMsgFrag) peer.framesBytes -= std::min(peer.framesBytes, f.size());
                peer.frames.pop_front();
            }
            const bool budget = peer.info.inflightBytes < cfg.maxInflightBytes;
            while (budget && !peer.resend.empty()) {
                const FragmentRef f = peer.resend.front();
                OutMessage* m = message(peer, f.seq);
                if (!m || m->done[f.index]) {
                    peer.resend.pop_front();
                    continue;
                }
                encodeFragment(*m, f.index, fragment);
                if (!fits(fragment.size())) break;
                plain.insert(plain.end(), fragment.begin(), fragment.end());
                sent.fragments.push_back(f);
                peer.resend.pop_front();
            }
            while (budget && nextMessage < peer.out.size()) {
                OutMessage& m = peer.out[nextMessage];
                if (m.sent == m.count) {
                    ++nextMessage;
                    continue;
                }
                // Every message before out.front() was received, so the receiver's
                // window starts at or after it.
                if (m.seq >= peer.out.front().seq + kReliableWindow) break;
                encodeFragment(m, m.sent, fragment);
                if (!fits(fragment.size())) break;
                plain.insert(plain.end(), fragment.begin(), fragment.end());
                sent.fragments.push_back({m.seq, m.sent});
                ++m.sent;
            }
            const bool eliciting = plain.size() > ackOnly;
            if (!eliciting && !(peer.ackPending && peer.anyReceived)) break;
            emit(peer, *s, eliciting, sent);
            if (!eliciting) break;
            if (peer.frames.empty() && peer.resend.empty() && !moreReliable(peer, nextMessage)) break;
        }
        compact(peer);
    }

    bool moreReliable(Peer& peer, size_t from) const {
        if (peer.info.inflightBytes >= cfg.maxInflightBytes) return false;
        for (size_t i = from; i < peer.out.size(); ++i)
            if (peer.out[i].sent < peer.out[i].count) return true;
        return false;
    }

    void emit(Peer& peer, Session& s, bool eliciting, Sent& sent) {
        const uint64_t pn = peer.nextPn++;
        wire.resize(kDataHeader + plain.size() + Aead::kTag);
        wire[0] = kData;
        put32(&wire[1], s.remote);
        put32(&wire[5], uint32_t(pn));
        s.tx.seal(pn, wire.data(), kDataHeader, plain.data(), plain.size(), wire.data() + kDataHeader);
        sendDatagram(s.address, wire.data(), wire.size());
        peer.ackPending = false;
        peer.lastSend = now;
        ++peer.info.packetsSent;
        peer.info.bytesSent += wire.size();
        if (eliciting) {
            sent.pn = pn;
            sent.at = now;
            sent.bytes = uint32_t(wire.size());
            peer.info.inflightBytes += wire.size();
            peer.inflight.push_back(std::move(sent));
        }
    }

    void close(Peer& peer) {
        Session* s = peer.sender();
        if (!s || peer.dead) return;
        plain.assign({fClose, 0});
        Sent unused{0, now, 0, false, {}};
        for (int i = 0; i < 2; ++i) emit(peer, *s, false, unused);
    }

    // --- Timers -------------------------------------------------------------------

    void timers() {
        for (size_t i = 0; i < pendings.size();) {
            Pending& p = *pendings[i];
            if (p.started < 0) p.started = p.sentAt = now, p.next = now + p.interval;
            if (now - p.started > cfg.connectSeconds) {
                if (p.initiator && !findAt(p.address))  // linked the other way meanwhile: no failure
                    event(HostEventType::ConnectFailed, kNoPeer, p.address, Reason::ConnectTimeout);
                pendings.erase(pendings.begin() + long(i));
                continue;
            }
            if (p.initiator && now >= p.next) {
                sendDatagram(p.address, p.packet.data(), p.packet.size());
                ++p.sends;
                p.sentAt = now;
                p.interval = std::min(p.interval * 2, 1.0);
                p.next = now + p.interval;
            }
            ++i;
        }
        for (auto& owned : peers) {
            Peer& peer = *owned;
            if (peer.dead) continue;
            if (now - peer.info.lastReceive > cfg.timeoutSeconds) {
                kill(peer, Reason::Timeout);
                continue;
            }
            for (auto& s : peer.sessions)
                if (!s->finish.empty() && now >= s->nextFinish) {
                    sendDatagram(s->address, s->finish.data(), s->finish.size());
                    s->finishInterval = std::min(s->finishInterval * 2, 1.0);
                    s->nextFinish = now + s->finishInterval;
                }
            // Retire sessions that went quiet while a newer one carries the link.
            Session* current = peer.sender();
            for (size_t k = 0; k < peer.sessions.size();) {
                Session* s = peer.sessions[k].get();
                if (s != current && now - s->lastReceive > cfg.timeoutSeconds) {
                    sessions.erase(s->local);
                    peer.sessions.erase(peer.sessions.begin() + long(k));
                } else {
                    ++k;
                }
            }
            bool timedOut = false;
            for (Sent& s : peer.inflight)
                if (!s.settled && now - s.at > peer.rto) settle(peer, s, true), timedOut = true;
            if (timedOut) peer.rto = std::min(peer.rto * 2, 2.0);
            compact(peer);
            for (auto it = peer.groups.begin(); it != peer.groups.end();)
                it = now - it->at > 2.0 ? peer.groups.erase(it) : it + 1;
        }
    }

    void sweep() {
        for (size_t i = 0; i < peers.size();) {
            if (!peers[i]->dead) {
                ++i;
                continue;
            }
            for (auto& s : peers[i]->sessions) sessions.erase(s->local);
            peers.erase(peers.begin() + long(i));
        }
    }

    void poll(double t) {
        now = t;
        clockKnown = true;
        for (size_t i = 0; i < cfg.receiveBudget; ++i) {
            Address from;
            const int n = socket.receive(from, rx, sizeof rx);
            if (n < 0) break;
            ++counters.datagramsReceived;
            counters.bytesReceived += size_t(n);
            if (n == 0) {
                refuse(Reason::Malformed);
                continue;
            }
            switch (rx[0]) {
            case kInit: onInit(from, rx, size_t(n)); break;
            case kResponse: onResponse(from, rx, size_t(n)); break;
            case kFinish: onFinish(from, rx, size_t(n)); break;
            case kData: onData(from, rx, size_t(n)); break;
            default:
                if (foreign) foreign(from, rx, size_t(n));
                else refuse(Reason::Foreign);
            }
        }
        timers();
        for (auto& p : peers) flushPeer(*p, false);
        sweep();
        for (size_t i = 0; i < unheard.size(); ++i) {  // listeners may cause more
            const HostEvent e = unheard[i];
            for (auto& l : listeners) l(e);
        }
        unheard.clear();
    }
};

Host::Host(DatagramSocket& socket, const Identity& identity, const HostConfig& config)
    : impl_(std::make_unique<Impl>(socket, identity, config)) {}
Host::~Host() {
    for (auto& p : impl_->peers) impl_->close(*p);
}

bool Host::connect(const Address& to, const Bytes& intent, const Key* expect) { return impl_->connect(to, intent, expect); }
void Host::disconnect(PeerHandle h) {
    if (Peer* p = impl_->find(h)) {
        impl_->close(*p);
        impl_->kill(*p, Reason::Closed);
    }
}
bool Host::send(PeerHandle h, const uint8_t* data, size_t size, Channel c) { return impl_->send(h, data, size, c); }
void Host::introduce(PeerHandle a, PeerHandle b, const Bytes& intent) {
    Peer* pa = impl_->find(a);
    Peer* pb = impl_->find(b);
    if (!pa || !pb || a == b || intent.size() > kMaxIntent) return;
    auto frame = [&](const Peer& about) {
        Bytes f{fIntro, uint8_t(about.info.address.compactSize())};
        f.resize(2 + about.info.address.compactSize());
        about.info.address.writeCompact(&f[2]);
        f.insert(f.end(), about.info.key.begin(), about.info.key.end());
        Writer w(f);
        w.var(intent.size());
        w.bytes(intent.data(), intent.size());
        return f;
    };
    pa->frames.push_back(frame(*pb));
    pb->frames.push_back(frame(*pa));
}
void Host::poll(double now) { impl_->poll(now); }
void Host::flush() {
    for (auto& p : impl_->peers) impl_->flushPeer(*p, false);
}
void Host::setReceiver(Receiver r) { impl_->receiver = std::move(r); }
void Host::addListener(std::function<void(const HostEvent&)> l) { impl_->listeners.push_back(std::move(l)); }
void Host::setService(uint8_t service, Service handler) {
    if (service && service != kObservedService) impl_->services[service] = std::move(handler);
}
bool Host::sendService(PeerHandle peer, uint8_t service, const uint8_t* data, size_t size) {
    return service && service != kObservedService && impl_->send(peer, data, size, Channel::Reliable, service);
}
void Host::setAdmission(Admission a) { impl_->admission = std::move(a); }
void Host::setForeign(Foreign f) { impl_->foreign = std::move(f); }
bool Host::sendRaw(const Address& to, const uint8_t* data, size_t size) { return impl_->sendDatagram(to, data, size); }
std::vector<HostEvent> Host::takeEvents() { return std::exchange(impl_->events, {}); }
const PeerInfo* Host::peer(PeerHandle h) const {
    Peer* p = impl_->find(h);
    return p ? &p->info : nullptr;
}
PeerHandle Host::peerAt(const Address& a) const {
    Peer* p = impl_->findAt(a);
    return p ? p->info.handle : kNoPeer;
}
PeerHandle Host::peerWithKey(const Key& k) const {
    Peer* p = impl_->findKey(k);
    return p ? p->info.handle : kNoPeer;
}
std::vector<PeerHandle> Host::peers() const {
    std::vector<PeerHandle> out;
    for (auto& p : impl_->peers)
        if (!p->dead) out.push_back(p->info.handle);
    return out;
}
bool Host::connecting(const Address& to) const {
    for (auto& p : impl_->pendings)
        if (p->address == to) return true;
    return false;
}
Address Host::publicAddress() const {
    // The most reported of the last five minutes: a NAT that changed our
    // mapping is believed as soon as peers report the new one.
    const Impl::Vote* best = nullptr;
    double latest = -1e300;
    for (const auto& v : impl_->votes) latest = std::max(latest, v.at);
    for (const auto& v : impl_->votes)
        if (latest - v.at <= 300 && (!best || v.count > best->count || (v.count == best->count && v.at > best->at)))
            best = &v;
    return best ? best->address : Address{};
}
void Host::notePublicAddress(const Address& seen) { impl_->notePublic(seen); }
Address Host::localAddress() const { return impl_->socket.local(); }
const Identity& Host::identity() const { return impl_->identity; }
const HostConfig& Host::config() const { return impl_->cfg; }
const HostCounters& Host::counters() const { return impl_->counters; }
size_t Host::singleDatagramMessage() const { return impl_->fragmentBytes; }
double Host::now() const { return impl_->now; }

} // namespace saida::p2p
