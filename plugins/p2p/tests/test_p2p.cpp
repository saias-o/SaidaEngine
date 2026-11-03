// saida::p2p qualification: cryptography vectors, the handshake, link
// reliability under loss, NAT traversal across every pair of NAT kinds, the
// Mainline DHT, rendezvous by arena code and by map cell, a rollback session
// (plugins/netcode) over all of it, and real sockets on this machine.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -I../include -I../../netcode/include
//       -I../../../third_party/mbedtls/include test_p2p.cpp ../src/*.cpp
//       -L../../../build-rel/media-mbedtls/library -lmbedcrypto -lws2_32 -lbcrypt -lwinmm
//
// Prints PASS saida::p2p, or each failed check and FAIL.
#include "saida/p2p/Bencode.hpp"
#include "saida/p2p/Dht.hpp"
#include "saida/p2p/Host.hpp"
#include "saida/p2p/NetcodeTransport.hpp"
#include "saida/p2p/NetworkThread.hpp"
#include "saida/p2p/Rendezvous.hpp"
#include "saida/p2p/SimInternet.hpp"
#include "saida/p2p/Socket.hpp"
#include "saida/netcode/Clock.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>

using namespace saida::p2p;
namespace nc = saida::netcode;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static uint64_t seedOffset = 0;

static void unhex(const char* h, uint8_t* out) {
    for (size_t i = 0; h[2 * i]; ++i) {
        unsigned v;
        std::sscanf(h + 2 * i, "%2x", &v);
        out[i] = uint8_t(v);
    }
}
template <size_t N>
static bool equalsHex(const std::array<uint8_t, N>& a, const char* h) {
    uint8_t b[N];
    unhex(h, b);
    return std::memcmp(a.data(), b, N) == 0;
}

// --- Cryptography ----------------------------------------------------------------------

static void crypto() {
    std::printf("crypto\n");
    Key a, b, shared1, shared2;
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a.data());
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b.data());
    const KeyPair A = KeyPair::fromSecret(a), B = KeyPair::fromSecret(b);
    CHECK(equalsHex(A.pub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));  // RFC 7748 6.1
    CHECK(equalsHex(B.pub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    CHECK(x25519(shared1, a, B.pub) && x25519(shared2, b, A.pub));
    CHECK(equalsHex(shared1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    CHECK(shared1 == shared2);
    Key zero{}, out;
    CHECK(!x25519(out, a, zero));  // a low-order point gives no key
    CHECK(equalsHex(sha256("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    uint8_t k[20];
    std::memset(k, 0x0b, 20);
    CHECK(equalsHex(hmacSha256(k, 20, reinterpret_cast<const uint8_t*>("Hi There"), 8),
                    "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));  // RFC 4231 1
    Aead aead;
    Key key;
    for (int i = 0; i < 32; ++i) key[size_t(i)] = uint8_t(0x80 + i);
    aead.setKey(key);
    const char* text = "Ladies and Gentlemen of the class of '99";
    const size_t n = std::strlen(text);
    uint8_t aad[12], sealed[128], opened[128];
    unhex("50515253c0c1c2c3c4c5c6c7", aad);
    aead.seal(7, aad, 12, reinterpret_cast<const uint8_t*>(text), n, sealed);
    CHECK(aead.open(7, aad, 12, sealed, n + 16, opened) && std::memcmp(opened, text, n) == 0);
    CHECK(!aead.open(8, aad, 12, sealed, n + 16, opened));  // wrong nonce
    sealed[3] ^= 1;
    CHECK(!aead.open(7, aad, 12, sealed, n + 16, opened));  // one flipped bit
    // Throughput of one data packet's worth of sealing and opening.
    uint8_t payload[100] = {};
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20000; ++i) {
        aead.seal(uint64_t(i), aad, 9, payload, sizeof payload, sealed);
        aead.open(uint64_t(i), aad, 9, sealed, sizeof payload + 16, opened);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 20000;
    std::printf("  seal+open of a 100-byte packet: %.2f us\n", us);
}

static void noise() {
    std::printf("noise\n");
    const KeyPair si = KeyPair::generate(), sr = KeyPair::generate();
    const uint8_t prologue[] = "test";
    auto handshake = [&](const uint8_t* pi, const uint8_t* pr, int tamper, Key* seenByI, Key* seenByR) {
        NoiseXX i, r;
        i.begin(true, si, pi, 4);
        r.begin(false, sr, pr, 4);
        Bytes m1, m2, m3, p;
        const uint8_t hi[] = {1, 2, 3};
        i.write1(hi, 3, m1);
        if (!r.read1(m1.data(), m1.size(), p) || p != Bytes(hi, hi + 3)) return false;
        if (!r.write2(hi, 3, m2)) return false;
        if (tamper == 2) m2[40] ^= 1;
        if (!i.read2(m2.data(), m2.size(), p)) return false;
        if (!i.write3(hi, 2, m3)) return false;
        if (tamper == 3) m3.back() ^= 1;
        if (!r.read3(m3.data(), m3.size(), p) || p.size() != 2) return false;
        Aead it, ir, rt, rr;
        i.split(it, ir);
        r.split(rt, rr);
        uint8_t c[64], o[64];
        it.seal(0, nullptr, 0, hi, 3, c);
        if (!rr.open(0, nullptr, 0, c, 19, o) || std::memcmp(o, hi, 3) != 0) return false;
        rt.seal(5, nullptr, 0, hi, 3, c);
        if (!ir.open(5, nullptr, 0, c, 19, o)) return false;
        if (seenByI) *seenByI = i.remoteStatic();
        if (seenByR) *seenByR = r.remoteStatic();
        return i.handshakeHash() == r.handshakeHash();
    };
    Key byI, byR;
    CHECK(handshake(prologue, prologue, 0, &byI, &byR));
    CHECK(byI == sr.pub && byR == si.pub);  // each side learned the other's static key
    CHECK(!handshake(prologue, prologue, 2, nullptr, nullptr));
    CHECK(!handshake(prologue, prologue, 3, nullptr, nullptr));
    const uint8_t other[] = "tesT";
    CHECK(!handshake(prologue, other, 0, nullptr, nullptr));  // another application never connects
}

// --- Formats ----------------------------------------------------------------------------

static void formats() {
    std::printf("formats\n");
    Address a;
    CHECK(Address::parse("192.168.1.20:47474", a) && !a.v6 && a.port == 47474 && a.isPrivate());
    CHECK(a.toString() == "192.168.1.20:47474");
    CHECK(Address::parse("[2001:db8::1]:9", a) && a.v6 && a.ip[0] == 0x20 && a.ip[15] == 1);
    const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4};
    a = Address::fromIpv6(mapped, 5);
    CHECK(!a.v6 && a.ip[3] == 4 && a.toString() == "1.2.3.4:5");  // one spelling per endpoint
    CHECK(Address::parse("[::1]:80", a) && a.isLoopback());
    CHECK(!Address::parse("1.2.3:4", a) && !Address::parse("1.2.3.4", a) && !Address::parse("[::1:2", a) &&
          !Address::parse("256.1.1.1:1", a) && !Address::parse("1.2.3.4:0", a));
    uint8_t c[18];
    Address b = Address::v4(203, 0, 113, 9, 6881);
    b.writeCompact(c);
    CHECK(Address::readCompact(c, 6) == b);

    BValue d = BValue::makeDict();
    d.set("y", BValue::string("q"));
    d.set("a", BValue::makeDict()).set("id", BValue::string(std::string(20, 'x')));
    d.set("n", BValue::integer(-42));
    const std::string e = d.encode();
    CHECK(e.substr(0, 5) == "d1:ad");  // keys sorted
    BValue back;
    CHECK(BValue::decode(reinterpret_cast<const uint8_t*>(e.data()), e.size(), back) && back.encode() == e);
    for (const char* bad : {"i-0e", "i03e", "5:abc", "d1:a", "l", "de1", "i1", "d1:ai1e1:b"})
        CHECK(!BValue::decode(reinterpret_cast<const uint8_t*>(bad), std::strlen(bad), back));
    uint64_t rng = 99 + seedOffset;
    for (int i = 0; i < 20000; ++i) {  // random bytes and damaged messages never crash it
        std::string junk = e;
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        junk[size_t(rng >> 33) % junk.size()] = char(rng >> 50);
        if (i % 3 == 0) junk.resize(size_t(rng >> 40) % junk.size());
        BValue v;
        BValue::decode(reinterpret_cast<const uint8_t*>(junk.data()), junk.size(), v);
    }

    std::string canonical;
    const std::string code = newArenaCode();
    CHECK(code.size() == 9 && code[4] == '-');
    CHECK(normalizeArenaCode(code, canonical) && canonical == code);
    std::string typed = code;
    for (char& ch : typed) ch = char(std::tolower(ch));
    typed.erase(4, 1);
    CHECK(normalizeArenaCode(typed, canonical) && canonical == code);  // lower case, no dash
    int caught = 0, typos = 0;
    static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (size_t pos = 0; pos < code.size(); ++pos) {
        if (pos == 4) continue;
        for (int c2 = 0; c2 < 32; ++c2) {
            if (alphabet[c2] == code[pos]) continue;
            std::string t = code;
            t[pos] = alphabet[c2];
            ++typos;
            caught += !normalizeArenaCode(t, canonical);
        }
    }
    CHECK(caught == typos);  // every single mistyped character is refused, not searched for
    CHECK(normalizeArenaCode("O0OO-0000", canonical) == normalizeArenaCode("0000-0000", canonical));
    CHECK(arenaTopic("game", code) != arenaTopic("other game", code));
}

// --- A simulated Internet with hosts ------------------------------------------------------

struct Endpoint {
    DatagramSocket* socket = nullptr;
    std::unique_ptr<Host> host;
    std::unique_ptr<Rendezvous> meet;
    std::vector<HostEvent> events;
    std::vector<RendezvousEvent> meetings;
    std::vector<std::pair<PeerHandle, Bytes>> reliable, unreliable;
    bool record = true;
};

struct World {
    SimInternet net;
    double now = 0, dt = 1.0 / 60.0;
    std::vector<std::unique_ptr<Endpoint>> ends;
    std::vector<std::unique_ptr<Dht>> nodes;
    std::vector<DatagramSocket*> nodeSockets;

    explicit World(uint64_t seed) : net(seed + seedOffset * 7919) {}

    Endpoint& add(int router, const Line& line = {}, HostConfig cfg = {}) {
        auto e = std::make_unique<Endpoint>();
        e->socket = &net.addHost(router, line);
        e->host = std::make_unique<Host>(*e->socket, Identity::generate(), cfg);
        Endpoint* raw = e.get();
        e->host->setReceiver([raw](PeerHandle p, const uint8_t* d, size_t n, Channel c) {
            if (!raw->record) return;
            (c == Channel::Reliable ? raw->reliable : raw->unreliable).push_back({p, Bytes(d, d + n)});
        });
        ends.push_back(std::move(e));
        return *ends.back();
    }
    // Public full DHT nodes, each bootstrapped from a few before it.
    void dhtNetwork(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            DatagramSocket& s = net.addHost(-1, Line{0.01 + 0.002 * double(i % 20), 0.002, 0.01, 0});
            DhtConfig c;
            c.readOnly = false;
            nodes.push_back(std::make_unique<Dht>([&s](const Address& to, const uint8_t* d, size_t n) {
                return s.send(to, d, n);
            }, c));
            nodeSockets.push_back(&s);
            for (size_t k = 1; k <= 3 && k <= i; ++k) nodes.back()->addNode(nodeSockets[i - k]->local());
            if (i > 3) nodes.back()->addNode(nodeSockets[0]->local());
        }
    }
    std::vector<Address> seeds(size_t n) const {
        std::vector<Address> out;
        for (size_t i = 0; i < n && i < nodeSockets.size(); ++i) out.push_back(nodeSockets[i * 7 % nodeSockets.size()]->local());
        return out;
    }

    void step() {
        now += dt;
        net.setNow(now);
        uint8_t buf[2048];
        for (size_t i = 0; i < nodes.size(); ++i) {
            Address from;
            for (int n; (n = nodeSockets[i]->receive(from, buf, sizeof buf)) >= 0;) nodes[i]->receive(from, buf, size_t(n));
            nodes[i]->poll(now);
        }
        for (auto& e : ends) {
            e->host->poll(now);
            if (e->meet) e->meet->poll(now);
            for (auto& ev : e->host->takeEvents()) e->events.push_back(ev);
            if (e->meet)
                for (auto& m : e->meet->takeEvents()) e->meetings.push_back(m);
            e->host->flush();
        }
    }
    void run(double seconds) {
        for (int i = 0, n = int(seconds / dt + 0.5); i < n; ++i) step();
    }
    template <class F>
    bool runUntil(double seconds, F done) {
        for (int i = 0, n = int(seconds / dt + 0.5); i < n; ++i) {
            step();
            if (done()) return true;
        }
        return false;
    }
};

static PeerHandle connectedTo(const Endpoint& e) {
    for (const HostEvent& ev : e.events)
        if (ev.type == HostEventType::Connected) return ev.peer;
    return kNoPeer;
}
static int count(const Endpoint& e, HostEventType t, Reason r = Reason::None) {
    int n = 0;
    for (const HostEvent& ev : e.events) n += ev.type == t && (r == Reason::None || ev.reason == r);
    return n;
}

// --- The link -------------------------------------------------------------------------------

static void link() {
    std::printf("link\n");
    World w(11);
    Endpoint& a = w.add(-1, Line{0.025, 0.002, 0, 0});
    Endpoint& b = w.add(-1, Line{0.025, 0.002, 0, 0});
    CHECK(a.host->connect(b.socket->local(), Bytes{'h', 'i'}));
    CHECK(w.runUntil(2, [&] { return connectedTo(a) && connectedTo(b); }));
    const double handshake = w.now;
    const PeerHandle ab = connectedTo(a), ba = connectedTo(b);
    CHECK(b.events[0].intent == (Bytes{'h', 'i'}));  // the responder saw the intent
    CHECK(a.host->peer(ab)->key == b.host->identity().keys.pub && b.host->peer(ba)->id == a.host->identity().id());
    std::printf("  both sides connected on a 100 ms round trip after %.0f ms\n", handshake * 1000);
    w.run(1.0);
    // 2 x 25 ms each way, plus up to a frame of polling at each end.
    std::printf("  rtt %.1f ms, variance %.1f ms\n", a.host->peer(ab)->rtt * 1000, a.host->peer(ab)->rttVariance * 1000);
    CHECK(a.host->peer(ab)->rtt > 0.095 && a.host->peer(ab)->rtt < 0.14);
    CHECK(a.host->publicAddress() == a.socket->local());    // b told a what it sees

    // Large reliable messages are fragmented and reassembled; unreliable too.
    Bytes big(300000), medium(9000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7 + (i >> 9));
    for (size_t i = 0; i < medium.size(); ++i) medium[i] = uint8_t(i * 13);
    CHECK(a.host->send(ab, big, Channel::Reliable));
    CHECK(a.host->send(ab, medium, Channel::Unreliable));
    CHECK(!a.host->send(ab, Bytes(a.host->config().maxMessage + 1), Channel::Reliable));  // refused, and said so
    CHECK(a.host->counters().refused[size_t(Reason::TooLarge)] == 1);
    w.run(2.0);
    CHECK(b.reliable.size() == 1 && b.reliable[0].second == big);
    CHECK(b.unreliable.size() == 1 && b.unreliable[0].second == medium);

    // Graceful close: both sides hear it at once.
    a.host->disconnect(ab);
    w.run(0.5);
    CHECK(count(a, HostEventType::Disconnected, Reason::Closed) == 1);
    CHECK(count(b, HostEventType::Disconnected, Reason::Closed) == 1);

    // A peer that vanishes times out; when it comes back as a new process
    // (same identity), the old link is replaced.
    World w2(12);
    Endpoint& c = w2.add(-1);
    HostConfig quick;
    quick.timeoutSeconds = 3;
    Endpoint& d = w2.add(-1, Line{}, quick);
    c.host->connect(d.socket->local());
    w2.run(1);
    w2.net.setOffline(*c.socket, true);
    w2.run(4);
    CHECK(count(d, HostEventType::Disconnected, Reason::Timeout) == 1);
    w2.net.setOffline(*c.socket, false);
    const Identity same = c.host->identity();
    c.host = std::make_unique<Host>(*c.socket, same);
    c.events.clear();
    c.host->connect(d.socket->local());
    w2.run(1);
    CHECK(count(c, HostEventType::Connected) == 1 && count(d, HostEventType::Connected) == 2);
    // A crash and a restart while still connected (the old process never
    // says goodbye): the stale link ends as Restarted.
    w2.net.setOffline(*c.socket, true);
    c.host.reset();
    w2.net.setOffline(*c.socket, false);
    c.host = std::make_unique<Host>(*c.socket, same);
    c.host->connect(d.socket->local());
    w2.run(1);
    CHECK(count(d, HostEventType::Disconnected, Reason::Restarted) == 1 && count(d, HostEventType::Connected) == 3);

    // An expected key that does not answer is refused, not connected.
    World w3(13);
    Endpoint& e = w3.add(-1);
    Endpoint& f = w3.add(-1);
    const Key wrong = KeyPair::generate().pub;
    e.host->connect(f.socket->local(), {}, &wrong);
    w3.run(1);
    CHECK(count(e, HostEventType::ConnectFailed, Reason::WrongKey) == 1 && !connectedTo(e));
    // Admission refuses before any key agreement.
    f.host->setAdmission([](const Address&, const Bytes& intent) { return intent == Bytes{'o', 'k'}; });
    HostConfig patient;
    patient.connectSeconds = 1;
    Endpoint& g = w3.add(-1, Line{}, patient);
    g.host->connect(f.socket->local(), {'n', 'o'});
    w3.run(1.5);
    CHECK(count(g, HostEventType::ConnectFailed, Reason::ConnectTimeout) == 1);
    CHECK(f.host->counters().refused[size_t(Reason::NotAdmitted)] >= 1 && f.host->counters().handshakesAnswered == 1);
}

// A socket in the middle of a link: records, replays, corrupts.
struct Tap final : DatagramSocket {
    DatagramSocket& inner;
    std::vector<std::pair<Address, Bytes>> sent;
    bool corrupt = false;
    explicit Tap(DatagramSocket& s) : inner(s) {}
    bool send(const Address& to, const uint8_t* d, size_t n) override {
        sent.push_back({to, Bytes(d, d + n)});
        if (!corrupt) return inner.send(to, d, n);
        Bytes copy(d, d + n);
        copy[n / 2] ^= 0x40;
        return inner.send(to, copy.data(), n);
    }
    int receive(Address& from, uint8_t* buf, size_t cap) override { return inner.receive(from, buf, cap); }
    Address local() const override { return inner.local(); }
};

static void hostile() {
    std::printf("hostile\n");
    SimInternet net(21 + seedOffset);
    DatagramSocket& sa = net.addHost(-1);
    DatagramSocket& sb = net.addHost(-1);
    DatagramSocket& attacker = net.addHost(-1);
    Tap tap(sa);
    Host a(tap, Identity::generate()), b(sb, Identity::generate());
    int got = 0;
    b.setReceiver([&](PeerHandle, const uint8_t*, size_t, Channel) { ++got; });
    a.connect(sb.local());
    PeerHandle ab = kNoPeer;
    double t = 0;
    auto run = [&](double seconds) {
        for (int i = 0; i < int(seconds * 60); ++i) {
            t += 1.0 / 60;
            net.setNow(t);
            a.poll(t), b.poll(t);
            for (auto& e : a.takeEvents())
                if (e.type == HostEventType::Connected) ab = e.peer;
            b.takeEvents();
            a.flush(), b.flush();
        }
    };
    run(1);
    CHECK(ab != kNoPeer);
    for (int i = 0; i < 50; ++i) a.send(ab, Bytes{uint8_t(i)}, Channel::Reliable);
    run(1);
    CHECK(got == 50);
    // Every datagram a sent, sent again by an attacker from anywhere: no message
    // is delivered twice, no handshake is redone.
    const uint64_t answered = b.counters().handshakesAnswered;
    for (auto& [to, d] : tap.sent) attacker.send(to, d.data(), d.size());
    for (auto& [to, d] : tap.sent) sa.send(to, d.data(), d.size());
    run(1);
    CHECK(got == 50);
    CHECK(b.counters().refused[size_t(Reason::Replayed)] > 0);
    CHECK(b.counters().handshakesAnswered == answered + 0 || b.counters().handshakesAnswered == answered + 1);
    // Bit flips are refused by authentication, and the link recovers.
    tap.corrupt = true;
    for (int i = 0; i < 10; ++i) a.send(ab, Bytes{uint8_t(i)}, Channel::Reliable);
    run(0.5);
    CHECK(b.counters().refused[size_t(Reason::AuthFailed)] > 0 && got == 50);
    tap.corrupt = false;
    run(2);
    CHECK(got == 60);
    // Garbage of every size: refused and counted, never a crash.
    uint64_t rng = 5;
    for (int i = 0; i < 5000; ++i) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        Bytes junk(size_t(rng >> 54) + 1);
        for (auto& x : junk) rng = rng * 6364136223846793005ULL + 1, x = uint8_t(rng >> 56);
        if (i % 4 == 0) junk[0] = uint8_t(1 + i % 4);  // our datagram types, wrong contents
        if (i % 16 == 0) junk.resize(256, 0);           // the size of an Init
        attacker.send(sb.local(), junk.data(), junk.size());
    }
    run(1);
    uint64_t refused = 0;
    for (uint64_t r : b.counters().refused) refused += r;
    CHECK(refused > 4000 && got == 60 && b.peers().size() == 1);
    // A flood of Inits is answered within the rate budget only.
    const uint64_t before = b.counters().handshakesAnswered;
    Host flood(attacker, Identity::generate());
    for (int i = 0; i < 300; ++i) {
        Address fake = sb.local();
        flood.connect(fake);
        Bytes init(256, 0);
        init[0] = 1, init[1] = 1;
        randomBytes(&init[2], 4 + 32);
        init[6] = 0;
        attacker.send(sb.local(), init.data(), init.size());
    }
    run(0.1);
    CHECK(b.counters().handshakesAnswered - before <= 70);
    CHECK(b.counters().refused[size_t(Reason::RateLimited)] + b.counters().refused[size_t(Reason::TooManyHandshakes)] > 0);
}

static void reliability() {
    std::printf("reliability\n");
    World w(31);
    const Line bad{0.04, 0.03, 0.12, 0.05};  // per line; about 23% end-to-end loss, reordering, duplicates
    Endpoint& a = w.add(-1, bad);
    Endpoint& b = w.add(-1, bad);
    a.host->connect(b.socket->local());
    CHECK(w.runUntil(5, [&] { return connectedTo(a) && connectedTo(b); }));
    const PeerHandle ab = connectedTo(a), ba = connectedTo(b);
    uint64_t rng = 77 + seedOffset;
    std::vector<Bytes> expected;
    size_t unreliableSent = 0;
    for (int f = 0; f < 60 * 20; ++f) {
        for (int k = 0; k < 6; ++k) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            Bytes m(4 + (rng >> 60 == 0 ? size_t(rng >> 40) % 6000 : size_t(rng >> 50) % 200));
            for (size_t i = 0; i < m.size(); ++i) m[i] = uint8_t(expected.size() + i * 31);
            std::memcpy(m.data(), &f, 4);
            if (a.host->send(ab, m, Channel::Reliable)) expected.push_back(m);
        }
        Bytes u(40, uint8_t(f));
        a.host->send(ab, u, Channel::Unreliable);
        ++unreliableSent;
        b.host->send(ba, u, Channel::Unreliable);
        w.step();
    }
    w.runUntil(20, [&] { return b.reliable.size() >= expected.size(); });
    bool inOrder = b.reliable.size() == expected.size();
    for (size_t i = 0; inOrder && i < expected.size(); ++i) inOrder = b.reliable[i].second == expected[i];
    CHECK(inOrder);  // every message once, in order, intact
    bool intact = true;
    for (auto& [p, m] : b.unreliable) intact = intact && m.size() == 40 && m == Bytes(40, m[0]);
    CHECK(intact && b.unreliable.size() <= unreliableSent && b.unreliable.size() > unreliableSent * 6 / 10);
    const PeerInfo& i = *a.host->peer(ab);
    std::printf("  %zu reliable messages through ~23%% loss: %llu packets, %llu lost, %llu retransmitted, "
                "rtt %.0f ms (+-%.0f), loss seen %.0f%%\n",
                expected.size(), (unsigned long long)i.packetsSent, (unsigned long long)i.packetsLost,
                (unsigned long long)i.retransmits, i.rtt * 1000, i.rttVariance * 1000, i.loss * 100);
    CHECK(i.loss > 0.14 && i.loss < 0.34);  // 22.6% end to end
    CHECK(i.rtt > 0.14 && i.rtt < 0.25);  // 160 ms + up to 60 ms of jitter
}

// Each pair of NAT kinds: both sides learn their public address from a
// public server, swap them, and connect at the same moment.
static void natMatrix() {
    std::printf("nat matrix\n");
    const Nat kinds[] = {Nat::None, Nat::FullCone, Nat::AddressRestricted, Nat::PortRestricted, Nat::Symmetric};
    const char* names[] = {"open", "full", "addr", "port", "sym"};
    std::printf("  %-5s", "");
    for (const char* n : names) std::printf(" %-5s", n);
    std::printf("\n");
    for (int x = 0; x < 5; ++x) {
        std::printf("  %-5s", names[x]);
        for (int y = 0; y < 5; ++y) {
            World w(100 + uint64_t(x * 5 + y));
            const int rx = kinds[x] == Nat::None ? -1 : w.net.addRouter(kinds[x]);
            const int ry = kinds[y] == Nat::None ? -1 : w.net.addRouter(kinds[y]);
            Endpoint& server = w.add(-1);
            Endpoint& a = w.add(rx, Line{0.03, 0.005, 0.02, 0});
            Endpoint& b = w.add(ry, Line{0.03, 0.005, 0.02, 0});
            a.host->connect(server.socket->local());
            b.host->connect(server.socket->local());
            CHECK(w.runUntil(3, [&] { return a.host->publicAddress().valid() && b.host->publicAddress().valid(); }));
            const Address pa = a.host->publicAddress(), pb = b.host->publicAddress();
            a.host->connect(pb);
            b.host->connect(pa);
            const bool ok = w.runUntil(10, [&] {
                return a.host->peers().size() == 2 && b.host->peers().size() == 2;
            });
            // Symmetric NAT against a port-restricted or symmetric one is the
            // case no punching can open: it needs a relay.
            const bool hard = (kinds[x] == Nat::Symmetric && (kinds[y] == Nat::Symmetric || kinds[y] == Nat::PortRestricted)) ||
                              (kinds[y] == Nat::Symmetric && kinds[x] == Nat::PortRestricted);
            CHECK(ok == !hard);
            std::printf(" %-5s", ok ? "ok" : "-");
        }
        std::printf("\n");
    }
}

static void introductions() {
    std::printf("introductions\n");
    World w(41);
    const int ra = w.net.addRouter(Nat::PortRestricted), rb = w.net.addRouter(Nat::PortRestricted);
    Endpoint& c = w.add(-1);
    Endpoint& a = w.add(ra);
    Endpoint& b = w.add(rb);
    a.host->connect(c.socket->local());
    b.host->connect(c.socket->local());
    w.run(1);
    const auto cp = c.host->peers();
    CHECK(cp.size() == 2);
    c.host->introduce(cp[0], cp[1], {'g'});
    CHECK(w.runUntil(3, [&] { return a.host->peers().size() == 2 && b.host->peers().size() == 2; }));
    CHECK(count(a, HostEventType::Introduced) == 1 && count(b, HostEventType::Introduced) == 1);
}

// --- Discovery --------------------------------------------------------------------------------

static void dht() {
    std::printf("dht\n");
    World w(51);
    w.dhtNetwork(150);
    w.run(30);
    size_t good = 0;
    for (auto& n : w.nodes) good += n->goodNodes();
    std::printf("  150 nodes, %.1f good nodes known per node after 30 s\n", double(good) / 150);
    CHECK(good / 150 >= 16);
    // Two clients behind NATs: each announces, each finds the other's public mapping.
    const int ra = w.net.addRouter(Nat::PortRestricted), rb = w.net.addRouter(Nat::Symmetric);
    DatagramSocket& sa = w.net.addHost(ra, Line{0.03, 0.01, 0.02, 0});
    DatagramSocket& sb = w.net.addHost(rb, Line{0.03, 0.01, 0.02, 0});
    Dht ca([&](const Address& to, const uint8_t* d, size_t n) { return sa.send(to, d, n); });
    Dht cb([&](const Address& to, const uint8_t* d, size_t n) { return sb.send(to, d, n); });
    for (const Address& s : w.seeds(2)) ca.addNode(s), cb.addNode(s);
    const NodeId key = Dht::keyOf("arena", 5);
    ca.watch(key, true, 2.0);
    cb.watch(key, true, 2.0);
    std::vector<Address> foundA, foundB;
    double firstA = -1, firstB = -1;
    const double start = w.now;
    uint8_t buf[2048];
    for (int i = 0; i < 60 * 20; ++i) {
        w.step();
        Address from;
        for (int n; (n = sa.receive(from, buf, sizeof buf)) >= 0;) ca.receive(from, buf, size_t(n));
        for (int n; (n = sb.receive(from, buf, sizeof buf)) >= 0;) cb.receive(from, buf, size_t(n));
        ca.poll(w.now), cb.poll(w.now);
        for (auto& f : ca.takeFound())
            if (f.key == key) foundA.push_back(f.peer), firstA = firstA < 0 ? w.now - start : firstA;
        for (auto& f : cb.takeFound())
            if (f.key == key) foundB.push_back(f.peer), firstB = firstB < 0 ? w.now - start : firstB;
    }
    auto has = [](const std::vector<Address>& v, const Address& ip) {
        for (const Address& a : v)
            if (a.sameHost(ip)) return true;
        return false;
    };
    CHECK(has(foundA, w.net.publicIp(rb)) && has(foundB, w.net.publicIp(ra)));
    CHECK(ca.externalAddress().sameHost(w.net.publicIp(ra)));  // BEP 42: the network told us our address
    std::printf("  found each other after %.1f s and %.1f s; %llu + %llu queries, %llu throttled\n", firstA, firstB,
                (unsigned long long)ca.counters().queriesSent, (unsigned long long)cb.counters().queriesSent,
                (unsigned long long)(ca.counters().throttled + cb.counters().throttled));
    CHECK(firstA > 0 && firstA < 8 && firstB > 0 && firstB < 8);
}

// BEP 32: the IPv6 network, on an in-memory line (SimInternet is IPv4 only).
static void dht6() {
    std::printf("dht6\n");
    struct Packet {
        double at;
        Address from, to;
        Bytes data;
    };
    std::vector<Packet> air;
    double now = 0;
    std::vector<std::unique_ptr<Dht>> nodes;
    std::map<uint64_t, size_t> where;
    auto v6 = [](int i) {
        const uint8_t ip[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(i >> 8), uint8_t(i)};
        return Address::fromIpv6(ip, 6881);
    };
    auto make = [&](int i, bool serve) {
        const Address me = v6(i);
        DhtConfig c;
        c.readOnly = !serve;
        nodes.push_back(std::make_unique<Dht>([&air, &now, me](const Address& to, const uint8_t* d, size_t n) {
            air.push_back({now + 0.02, me, to, Bytes(d, d + n)});
            return true;
        }, c));
        where[me.hash()] = nodes.size() - 1;
        return nodes.back().get();
    };
    for (int i = 1; i <= 60; ++i) {
        Dht* n = make(i, true);
        for (int k = 1; k <= 3 && k < i; ++k) n->addNode(v6(i - k));
        if (i > 3) n->addNode(v6(1));
    }
    auto step = [&] {
        now += 0.05;
        std::vector<Packet> due;
        for (size_t i = 0; i < air.size();)
            if (air[i].at <= now) due.push_back(std::move(air[i])), air.erase(air.begin() + long(i));
            else ++i;
        for (Packet& p : due)
            if (auto it = where.find(p.to.hash()); it != where.end()) nodes[it->second]->receive(p.from, p.data.data(), p.data.size());
        for (auto& n : nodes) n->poll(now);
    };
    for (int i = 0; i < 400; ++i) step();
    Dht* a = make(1001, false);
    Dht* b = make(1002, false);
    a->addNode(v6(5)), b->addNode(v6(30));
    const NodeId key = Dht::keyOf("arena6", 6);
    a->watch(key, true, 2.0), b->watch(key, true, 2.0);
    bool aFound = false, bFound = false;
    double at = -1;
    for (int i = 0; i < 400 && !(aFound && bFound); ++i) {
        step();
        for (auto& f : a->takeFound()) aFound = aFound || (f.key == key && f.peer == v6(1002));
        for (auto& f : b->takeFound()) bFound = bFound || (f.key == key && f.peer == v6(1001));
        if (aFound && bFound) at = i * 0.05;
    }
    std::printf("  60 IPv6 nodes: two clients found each other after %.1f s, %zu IPv6 nodes known (%zu good), IPv4 %zu\n", at,
                a->nodeCount6(), a->goodNodes6(), a->nodeCount() - a->nodeCount6());
    CHECK(aFound && bFound && at < 8);
    CHECK(a->nodeCount6() > 8 && a->nodeCount() == a->nodeCount6() && a->ready());
    CHECK(a->isExternal(v6(1001)));  // the "ip" the network saw, 18 bytes
}

// --- Rendezvous: an arena code, then a rollback session over it --------------------------------

struct Sum final : nc::Simulation {
    std::map<uint64_t, uint64_t> acc;
    void save(nc::Frame, std::vector<uint8_t>& out) override {
        nc::Writer w(out);
        w.var(acc.size());
        for (auto& [k, v] : acc) w.u64(k), w.u64(v);
    }
    void load(nc::Frame, const uint8_t* d, size_t n) override {
        nc::Reader r(d, n);
        acc.clear();
        for (size_t c = size_t(r.var()); c-- && r.ok();) {
            const uint64_t k = r.u64();
            acc[k] = r.u64();
        }
    }
    void advance(const nc::FrameContext& ctx) override {
        for (size_t i = 0; i < ctx.playerCount; ++i) {
            uint64_t& v = acc[ctx.players[i].player];
            v = (v ^ ctx.players[i].data[0] ^ uint64_t(ctx.players[i].data[1]) << 8) * 1099511628211ULL;
        }
    }
};

static Endpoint& player(World& w, int router, const std::vector<Address>& seeds) {
    HostConfig hc;
    hc.application = "arena-fighter";
    Endpoint& e = w.add(router, Line{0.035, 0.006, 0.03, 0.01}, hc);
    RendezvousConfig rc;
    rc.bootstrap.clear();
    rc.knownNodes = seeds;
    e.meet = std::make_unique<Rendezvous>(*e.host, rc);
    return e;
}

static void arena() {
    std::printf("arena\n");
    World w(61);
    w.dhtNetwork(120);
    w.run(20);
    Endpoint& creator = player(w, w.net.addRouter(Nat::PortRestricted), w.seeds(3));
    Endpoint& joiner = player(w, w.net.addRouter(Nat::AddressRestricted), w.seeds(3));
    const std::string code = newArenaCode();
    creator.meet->join(arenaTopic("arena-fighter", code));
    w.run(3);  // the creator reads the code out; the friend types it in
    std::string typed;
    CHECK(normalizeArenaCode(code, typed));
    const Topic topic = arenaTopic("arena-fighter", typed);
    joiner.meet->join(topic);
    const double typedAt = w.now;
    auto joined = [](const Endpoint& e) {
        for (auto& m : e.meetings)
            if (m.type == RendezvousEventType::PeerJoined) return m.peer;
        return kNoPeer;
    };
    CHECK(w.runUntil(20, [&] { return joined(creator) && joined(joiner); }));
    std::printf("  together %.1f s after the code was typed\n", w.now - typedAt);
    CHECK(w.now - typedAt < 8);
    CHECK(creator.meet->status().problem.empty());

    // The rollback session: both founders, inputs through the punched link.
    struct Side {
        Endpoint* e;
        Sum sim;
        std::unique_ptr<NetcodeTransport> link;
        std::unique_ptr<nc::Session> session;
        nc::TickClock clock;
        int ticks = 0, advanced = 0;
    } sides[2];
    sides[0].e = &creator, sides[1].e = &joiner;
    std::vector<nc::PlayerId> founders = {creator.host->identity().id(), joiner.host->identity().id()};
    for (Side& s : sides) {
        nc::Config cfg;
        cfg.sessionId = 0x5eed;
        cfg.inputSize = 2;
        NetcodeTransport::fit(cfg, *s.e->host);
        s.e->record = false;
        s.link = std::make_unique<NetcodeTransport>(*s.e->host);
        s.session = std::make_unique<nc::Session>(cfg, s.e->host->identity().id(), s.sim, *s.link);
        s.link->attach(*s.session);
        s.link->bind(*s.session, joined(*s.e));
        s.session->start(founders, w.now);
    }
    const uint64_t bytesBefore = creator.host->counters().bytesSent;
    const double gameStart = w.now;
    for (int f = 0; f < 60 * 20; ++f) {
        w.step();
        for (Side& s : sides) {
            s.session->poll(w.now);
            for (const nc::Event& e : s.session->takeEvents())
                if (e.type == nc::EventType::Started) s.clock.reset(s.session->startLateness());
            for (int t = s.clock.advance(w.dt, s.session->timeScale()); t > 0; --t) {
                const int phase = s.ticks++ / 9;
                const uint8_t in[2] = {uint8_t(phase * 3 + int(&s - sides)), uint8_t(phase >> 2)};
                s.advanced += s.session->tick(in).status == nc::TickStatus::Advanced;
            }
            s.e->host->flush();
        }
    }
    int compared = 0;
    bool equal = true;
    for (nc::Frame f = 0; f <= sides[0].session->confirmedFrame(); f += 30) {
        uint64_t h0, h1;
        if (sides[0].session->checksum(f, h0) && sides[1].session->checksum(f, h1)) ++compared, equal = equal && h0 == h1;
    }
    const nc::Quality q = sides[0].session->quality();
    const double kbps = double(creator.host->counters().bytesSent - bytesBefore) / (w.now - gameStart) / 1000;
    std::printf("  rollback session: %d and %d frames, %d checksums equal, delay %d, rtt %.0f ms, %.1f KB/s up\n",
                sides[0].advanced, sides[1].advanced, compared, q.inputDelay, q.rtt * 1000, kbps);
    CHECK(equal && compared >= 10);
    CHECK(sides[0].advanced > 1000 && sides[1].advanced > 1000);
}

// R1World's case: players meet whoever is in the same map cell.
static void cells() {
    std::printf("cells\n");
    World w(71);
    w.dhtNetwork(120);
    w.run(20);
    const Nat kinds[] = {Nat::PortRestricted, Nat::FullCone, Nat::AddressRestricted, Nat::None};
    std::vector<Endpoint*> players;
    for (int i = 0; i < 8; ++i) {
        const int r = kinds[i % 4] == Nat::None ? -1 : w.net.addRouter(kinds[i % 4]);
        players.push_back(&player(w, r, w.seeds(3)));
    }
    const Topic cellA = Topic::named("arena-fighter", "cell/48.85,2.35"), cellB = Topic::named("arena-fighter", "cell/48.86,2.35");
    for (int i = 0; i < 8; ++i) {
        players[size_t(i)]->meet->join(i < 5 ? cellA : cellB);
        w.run(0.5);  // they arrive one after the other
    }
    auto settled = [&] {
        for (int i = 0; i < 8; ++i)
            if (players[size_t(i)]->meet->peers(i < 5 ? cellA : cellB).size() != (i < 5 ? 4u : 2u)) return false;
        return true;
    };
    CHECK(w.runUntil(20, settled));
    std::printf("  8 players in 2 cells met after %.1f s\n", w.now - 24);
    size_t links = 0;
    for (Endpoint* p : players) links += p->host->peers().size();
    CHECK(links == 2 * (10 + 3));  // a full mesh inside each cell, nothing across
    // Player 4 walks into cell B: it meets B's players and leaves A's.
    players[4]->meet->leave(cellA);
    players[4]->meet->join(cellB);
    CHECK(w.runUntil(20, [&] {
        return players[4]->meet->peers(cellB).size() == 3 && players[0]->meet->peers(cellA).size() == 3 &&
               players[4]->host->peers().size() == 3;  // A's players disconnected after lingering
    }));
}

// A crowd: each player links to its nearest, not to everyone; a newcomer is
// found through the peers already there; a walker's links follow him.
static Endpoint& crowdPlayer(World& w, int router, const std::vector<Address>& seeds, size_t neighbours,
                             size_t sightings = 0) {
    HostConfig hc;
    hc.application = "open-world";
    hc.maxPeers = std::max<size_t>(24, 3 * neighbours);
    Endpoint& e = w.add(router, Line{0.02, 0.004, 0.01, 0}, hc);
    e.record = false;
    RendezvousConfig rc;
    rc.bootstrap.clear();
    rc.knownNodes = seeds;
    rc.neighbours = neighbours;
    rc.sightings = sightings;
    e.meet = std::make_unique<Rendezvous>(*e.host, rc);
    return e;
}

using Spot = std::array<double, 3>;
static double apart(const Spot& a, const Spot& b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

// The share of every player's `k` true nearest it is linked to.
static double coverage(const std::vector<Endpoint*>& ps, const std::vector<Spot>& at, size_t k) {
    size_t want = 0, have = 0;
    for (size_t i = 0; i < ps.size(); ++i) {
        std::vector<std::pair<double, size_t>> d;
        for (size_t j = 0; j < ps.size(); ++j)
            if (j != i) d.push_back({apart(at[i], at[j]), j});
        std::sort(d.begin(), d.end());
        for (size_t n = 0; n < k && n < d.size(); ++n, ++want)
            have += ps[i]->host->peerWithKey(ps[d[n].second]->host->identity().keys.pub) != kNoPeer;
    }
    return want ? double(have) / double(want) : 1;
}

static void neighbourhood() {
    std::printf("neighbourhood\n");
    World w(81);
    w.dhtNetwork(120);
    w.run(20);
    const Nat kinds[] = {Nat::PortRestricted, Nat::FullCone, Nat::AddressRestricted, Nat::None};
    const size_t k = 6;
    const Topic cell = Topic::named("open-world", "cell/4422/8851");
    std::vector<Endpoint*> ps;
    std::vector<Spot> at;
    uint64_t r = 12345;
    auto jitter = [&] { return double((r = r * 6364136223846793005ULL + 1442695040888963407ULL) >> 40) / double(1 << 24) * 3; };
    for (int i = 0; i < 36; ++i) {
        const int router = kinds[i % 4] == Nat::None ? -1 : w.net.addRouter(kinds[i % 4]);
        ps.push_back(&crowdPlayer(w, router, w.seeds(3), k));
        at.push_back({10.0 * (i % 6) + jitter(), 10.0 * (i / 6) + jitter(), 0});
        ps.back()->meet->setPosition(at.back()[0], at.back()[1], at.back()[2]);
        ps.back()->meet->join(cell);
        w.run(0.2);  // they arrive one after the other
    }
    const double arrived = w.now;
    CHECK(w.runUntil(40, [&] { return coverage(ps, at, k) >= 0.97; }));
    w.run(8);  // the overlay settles: links nobody wants are dropped
    size_t most = 0, links = 0;
    for (Endpoint* p : ps) most = std::max(most, p->host->peers().size()), links += p->host->peers().size();
    uint64_t asked = 0, pruned = 0;
    for (Endpoint* p : ps) asked += p->meet->status().introductionsAsked, pruned += p->meet->status().pruned;
    std::printf("  36 players, %zu nearest each: %.0f%% linked %.1f s after the last arrived, %.1f links each (most %zu, "
                "a full mesh is 35), %llu introductions asked, %llu links dropped\n",
                k, 100 * coverage(ps, at, k), w.now - arrived - 8, double(links) / 36, most,
                (unsigned long long)asked, (unsigned long long)pruned);
    CHECK(coverage(ps, at, k) >= 0.97);
    CHECK(most <= 3 * k && double(links) / 36 <= 2.0 * k);
    // Player 0 walks from one corner to the far one: its links follow it.
    const Spot from = at[0], to = {58, 57, 0};
    for (int s = 0; s <= 600; ++s) {
        const double t = s / 600.0;
        at[0] = {from[0] + (to[0] - from[0]) * t, from[1] + (to[1] - from[1]) * t, 0};
        ps[0]->meet->setPosition(at[0][0], at[0][1], at[0][2]);
        w.step();
    }
    auto walkerNear = [&] {
        std::vector<std::pair<double, size_t>> d;
        for (size_t j = 1; j < ps.size(); ++j) d.push_back({apart(at[0], at[j]), j});
        std::sort(d.begin(), d.end());
        size_t n = 0;
        for (size_t i = 0; i < k; ++i) n += ps[0]->host->peerWithKey(ps[d[i].second]->host->identity().keys.pub) != kNoPeer;
        return n;
    };
    CHECK(w.runUntil(15, [&] { return walkerNear() == k; }));
    const size_t near = walkerNear();
    std::printf("  the walker crossed 80 m: %zu of its %zu new nearest linked, %zu links\n", near, k, ps[0]->host->peers().size());
    CHECK(near == k && ps[0]->host->peers().size() <= 3 * k);
}

// The pairs no punching opens link through a peer both reach: sealed end to
// end, carried inside each one's link to the relay.
static void relay() {
    std::printf("relay\n");
    World w(91);
    const int ra = w.net.addRouter(Nat::Symmetric), rb = w.net.addRouter(Nat::PortRestricted);
    Endpoint& r = w.add(-1, Line{0.02, 0.003, 0.01, 0});
    Endpoint& a = w.add(ra, Line{0.03, 0.005, 0.02, 0});
    Endpoint& b = w.add(rb, Line{0.03, 0.005, 0.02, 0});
    a.host->connect(r.socket->local());
    b.host->connect(r.socket->local());
    CHECK(w.runUntil(3, [&] { return r.host->peers().size() == 2; }));
    const PeerHandle viaA = connectedTo(a), viaB = connectedTo(b);
    // Punching first: it fails, as the NAT matrix says.
    const auto rp = r.host->peers();
    r.host->introduce(rp[0], rp[1], {'g'});
    w.run(4);
    CHECK(a.host->peers().size() == 1 && b.host->peers().size() == 1);
    // Then through the relay, proving b's key end to end.
    const Key bKey = b.host->identity().keys.pub;
    const Address virtualB = Host::relayAddress(viaA, b.host->identity().id());
    CHECK(Host::isRelayAddress(virtualB) && !Host::isRelayAddress(r.socket->local()));
    a.host->connect(virtualB, {'g'}, &bKey);
    const double asked = w.now;
    CHECK(w.runUntil(3, [&] { return a.host->peerWithKey(bKey) && b.host->peerWithKey(a.host->identity().keys.pub); }));
    const PeerHandle ab = a.host->peerWithKey(bKey), ba = b.host->peerWithKey(a.host->identity().keys.pub);
    CHECK(a.host->peer(ab)->via == viaA && b.host->peer(ba)->via == viaB);
    std::printf("  symmetric and port-restricted linked through the relay %.0f ms after asking\n", (w.now - asked) * 1000);
    // A large reliable message and a stream of small unreliable ones survive the detour.
    Bytes big(120000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7 + i / 251);
    b.reliable.clear(), b.unreliable.clear();
    CHECK(a.host->send(ab, big, Channel::Reliable));
    for (int i = 0; i < 100; ++i) {
        const uint8_t m[3] = {uint8_t(i), 1, 2};
        a.host->send(ab, m, 3, Channel::Unreliable);
        w.step();
    }
    CHECK(w.runUntil(10, [&] { return !b.reliable.empty(); }));
    CHECK(!b.reliable.empty() && b.reliable[0].second == big);
    std::printf("  120 KB reliable intact, %zu of 100 unreliable arrived, rtt %.0f ms (a to relay %.0f ms)\n",
                b.unreliable.size(), a.host->peer(ab)->rtt * 1000, a.host->peer(viaA)->rtt * 1000);
    CHECK(b.unreliable.size() >= 85);
    // The relay saw only sealed bytes: nothing reached its game.
    CHECK(r.reliable.empty() && r.unreliable.empty());
    // A peer the relay does not have is no route; a relayed link is never relayed again.
    const uint64_t refusedBefore = r.host->counters().refused[size_t(Reason::NoRoute)];
    a.host->connect(Host::relayAddress(viaA, 0x1234), {'g'});
    w.run(1);
    CHECK(r.host->counters().refused[size_t(Reason::NoRoute)] > refusedBefore);
    // The relay goes: the relayed link goes with it.
    w.net.setOffline(*r.socket, true);
    CHECK(w.runUntil(15, [&] { return !a.host->peerWithKey(bKey); }));
}

// The same, found by the rendezvous alone: two players behind symmetric NATs
// in one topic with a third reachable one are all linked.
static void relayedTopic() {
    std::printf("relayed topic\n");
    World w(93);
    w.dhtNetwork(120);
    w.run(20);
    Endpoint& open = player(w, -1, w.seeds(3));
    Endpoint& s1 = player(w, w.net.addRouter(Nat::Symmetric), w.seeds(3));
    Endpoint& s2 = player(w, w.net.addRouter(Nat::Symmetric), w.seeds(3));
    const Topic t = arenaTopic("arena-fighter", "RLAY-0000");
    for (Endpoint* e : {&open, &s1, &s2}) e->meet->join(t), w.run(0.5);
    CHECK(w.runUntil(30, [&] { return s1.meet->peers(t).size() == 2 && s2.meet->peers(t).size() == 2; }));
    const PeerHandle p = s1.host->peerWithKey(s2.host->identity().keys.pub);
    CHECK(p && s1.host->peer(p) && s1.host->peer(p)->via != kNoPeer);
    std::printf("  three players, two behind symmetric NATs: all linked %.1f s after the last joined, %llu relayed\n",
                w.now - 21, (unsigned long long)(s1.meet->status().relayed + s2.meet->status().relayed));
}

// A hundred and fifty players in one cell, 32 nearest each, one in ten
// behind a symmetric NAT: the overlay links each to its nearest (relayed
// where no punching opens), with links bounded per player, and the public DHT
// barely asked once the crowd has formed.
static void crowd() {
    std::printf("crowd\n");
    World w(97);
    w.dhtNetwork(120);
    w.run(20);
    const size_t n = 150, k = 32;
    const Nat kinds[] = {Nat::PortRestricted, Nat::FullCone, Nat::None, Nat::AddressRestricted, Nat::PortRestricted,
                         Nat::FullCone, Nat::None, Nat::AddressRestricted, Nat::PortRestricted, Nat::Symmetric};
    const Topic cell = Topic::named("open-world", "cell/4422/8852");
    std::vector<Endpoint*> ps;
    std::vector<Spot> at;
    uint64_t r = 777;
    auto jitter = [&] { return double((r = r * 6364136223846793005ULL + 1442695040888963407ULL) >> 40) / double(1 << 24) * 6; };
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n; ++i) {
        const Nat kind = kinds[i % 10];
        ps.push_back(&crowdPlayer(w, kind == Nat::None ? -1 : w.net.addRouter(kind), w.seeds(3), k, 256));
        at.push_back({15.0 * double(i % 15) + jitter(), 15.0 * double(i / 15) + jitter(), 0});
        ps.back()->meet->setPosition(at.back()[0], at.back()[1], at.back()[2]);
        ps.back()->meet->join(cell);
        w.run(0.1);
    }
    const double arrived = w.now;
    const bool formed = w.runUntil(60, [&] { return coverage(ps, at, k) >= 0.95; });
    const double formedIn = w.now - arrived;
    // Settled: what a crowd costs each player and the public DHT, over 30 s.
    w.run(10);
    std::vector<uint64_t> asked(w.nodes.size());
    for (size_t i = 0; i < w.nodes.size(); ++i) asked[i] = w.nodes[i]->counters().queriesAnswered;
    std::vector<uint64_t> sent(n);
    for (size_t i = 0; i < n; ++i) sent[i] = ps[i]->host->counters().bytesSent;
    w.run(30);
    double busiest = 0, total = 0;
    std::vector<double> up(n);
    for (size_t i = 0; i < w.nodes.size(); ++i) {
        const double q = double(w.nodes[i]->counters().queriesAnswered - asked[i]) / 30;
        busiest = std::max(busiest, q), total += q;
    }
    size_t most = 0, links = 0, relayed = 0;
    double upMost = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto peers = ps[i]->host->peers();
        most = std::max(most, peers.size()), links += peers.size();
        for (PeerHandle p : peers) relayed += ps[i]->host->peer(p)->via != kNoPeer;
        up[i] = double(ps[i]->host->counters().bytesSent - sent[i]) / 30;
        upMost = std::max(upMost, up[i]);
    }
    std::sort(up.begin(), up.end());
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("  %zu players, %zu nearest each: %.1f%% linked %.1f s after the last arrived; %.1f links each (most %zu, "
                "a full mesh is %zu), %zu links relayed\n",
                n, k, 100 * coverage(ps, at, k), formedIn, double(links) / double(n), most, n - 1, relayed / 2);
    std::printf("  settled: the overlay costs a player %.1f KB/s up (median; %.1f at most, a relay); the DHT answers "
                "%.1f queries/s in all, %.2f at the busiest node (%.1f s of simulation in %.1f s)\n",
                up[n / 2] / 1000, upMost / 1000, total, busiest, w.now, wall);
    CHECK(formed && coverage(ps, at, k) >= 0.95);
    CHECK(most <= 3 * k && double(links) / double(n) <= 1.8 * double(k));
    CHECK(relayed > 0);
    CHECK(busiest < 5);  // every player together: libtorrent blocks one source past 5 a second
    CHECK(up[n / 2] < 8000);
}

// A hot spot: 400 players pour into one cell in ten seconds. The arrivals
// spread over the topic's shard keys, the crowd elects a handful of
// announcers and everyone else falls silent in the DHT, and the sketch that
// elects them also says how many they are.
static void hotspot() {
    std::printf("hotspot\n");
    World w(131);
    w.dhtNetwork(160);
    w.run(20);
    const size_t n = 400, k = 12;
    const Nat kinds[] = {Nat::PortRestricted, Nat::FullCone, Nat::None, Nat::AddressRestricted};
    const Topic cell = Topic::named("open-world", "cell/4422/8853");
    std::vector<Endpoint*> ps;
    std::vector<Spot> at;
    uint64_t r = 99;
    auto uniform = [&] { return double((r = r * 6364136223846793005ULL + 1442695040888963407ULL) >> 40) / double(1 << 24); };
    auto served = [&] {
        uint64_t q = 0;
        for (auto& node : w.nodes) q += node->counters().queriesAnswered;
        return q;
    };
    // What the crowd's keys cost a node: get_peers served and announces stored
    // (a newcomer's routing-table walk is the DHT's own cost, spread thin over
    // millions of nodes on the real one, over 160 here).
    auto keyed = [&](size_t i) { return w.nodes[i]->counters().peersAnswered + w.nodes[i]->counters().announcesStored; };
    std::vector<uint64_t> before(w.nodes.size());
    for (size_t i = 0; i < w.nodes.size(); ++i) before[i] = keyed(i);
    const auto started = std::chrono::steady_clock::now();
    const double waveStart = w.now;
    for (size_t i = 0; i < n; ++i) {
        const Nat kind = kinds[i % 4];
        ps.push_back(&crowdPlayer(w, kind == Nat::None ? -1 : w.net.addRouter(kind), w.seeds(3), k, 256));
        at.push_back({200 * uniform(), 200 * uniform(), 0});  // a square of 200 m: one player per 100 m2
        ps.back()->meet->setLook(uint16_t(i));
        ps.back()->meet->setPosition(at.back()[0], at.back()[1], at.back()[2]);
        ps.back()->meet->join(cell);
        if (i % 4 == 3) w.run(0.1);  // forty a second
    }
    w.run(10);
    // The bootstrap seeds every player starts from (w.seeds) stand for the
    // public routers, sized for the whole Internet's arrivals: measured apart.
    auto seed = [&](size_t i) {
        for (const Address& a : w.seeds(3))
            if (a == w.nodeSockets[i]->local()) return true;
        return false;
    };
    double waveBusiest = 0, seedBusiest = 0;
    for (size_t i = 0; i < w.nodes.size(); ++i) {
        const double q = double(keyed(i) - before[i]) / (w.now - waveStart);
        (seed(i) ? seedBusiest : waveBusiest) = std::max(seed(i) ? seedBusiest : waveBusiest, q);
    }
    const bool formed = w.runUntil(60, [&] { return coverage(ps, at, k) >= 0.95; });
    w.run(30);  // the sketch spreads; announcers settle
    for (size_t i = 0; i < w.nodes.size(); ++i) before[i] = keyed(i);
    const uint64_t servedBefore = served();
    std::vector<uint64_t> asked(n);
    for (size_t i = 0; i < n; ++i) asked[i] = ps[i]->meet->dht()->counters().queriesSent;
    w.run(30);
    double busiest = 0;
    for (size_t i = 0; i < w.nodes.size(); ++i)
        busiest = std::max(busiest, double(keyed(i) - before[i]) / 30);
    size_t announcing = 0, silent = 0, talking = 0;
    double low = 1e18, high = 0;
    for (size_t i = 0; i < n; ++i) {
        const RendezvousStatus s = ps[i]->meet->status();
        announcing += s.announcing > 0, silent += s.silent > 0;
        talking += ps[i]->meet->dht()->counters().queriesSent - asked[i] > 0;
        low = std::min(low, s.population), high = std::max(high, s.population);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("  %zu players in 10 s: %.1f%% of the %zu nearest linked; the busiest DHT node answered %.1f queries/s "
                "during the wave (a bootstrap seed %.1f)\n",
                n, 100 * coverage(ps, at, k), k, waveBusiest, seedBusiest);
    std::printf("  settled: %zu announce, %zu silent, %zu asked the DHT in 30 s; %.0f queries/s in all, %.2f at the "
                "busiest node; each estimates the crowd at %.0f-%.0f (%.0f s in %.0f s)\n",
                announcing, silent, talking, double(served() - servedBefore) / 30, busiest, low, high, w.now, wall);
    // Beyond its 12 links, each sees the crowd sampled: how much, how true.
    // A sample, not a census: what gossip carries at a few hundred bytes a
    // second, spread over near and far alike.
    size_t seenMost = 0, seenLeast = n, beyond100 = n;
    double error = 0, errors = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto far = ps[i]->meet->sightings();
        seenMost = std::max(seenMost, far.size()), seenLeast = std::min(seenLeast, far.size());
        size_t distant = 0;
        for (const Sighting& s : far) {
            const size_t j = s.look;  // the test's look is the player's index
            if (j >= n || uint32_t(ps[j]->host->identity().id()) != s.id) continue;
            error += std::hypot(s.x - at[j][0], s.y - at[j][1], s.z - at[j][2]), ++errors;
            distant += apart(at[i], at[j]) > 100;
        }
        // Only players with somebody 100 m away count (not those in the middle).
        size_t possible = 0;
        for (size_t j = 0; j < n; ++j) possible += apart(at[i], at[j]) > 100;
        if (possible > 50) beyond100 = std::min(beyond100, distant);
    }
    std::printf("  sightings: each sees %zu-%zu of the crowd beyond its links, within %.2f m on average\n", seenLeast,
                seenMost, error / std::max(1.0, errors));
    std::printf("  at least %zu of them more than 100 m away\n", beyond100);
    CHECK(seenLeast >= 80 && beyond100 >= 15 && errors > 0 && error / errors < 1.5);
    CHECK(formed);
    CHECK(announcing <= 16 && silent + announcing == n);
    // One key with every member announcing and asking (before 4 November)
    // measured about 60 during this wave and 29 settled, with 400 announcing.
    CHECK(busiest < 2 && waveBusiest < 35);
    CHECK(low > n / 3.0 && high < n * 3.0);
}

// --- This machine's network ---------------------------------------------------------------------

static double clockNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void sockets() {
    std::printf("sockets\n");
    UdpSocket sa, sb;
    CHECK(sa.open() && sb.open());
    if (!sa.isOpen() || !sb.isOpen()) return void(std::printf("  %s %s\n", sa.error().c_str(), sb.error().c_str()));
    std::printf("  dual-stack: %s\n", sa.dualStack() ? "yes" : "no (IPv4 only)");
    Host a(sa, Identity::generate()), b(sb, Identity::generate());
    int got = 0;
    b.setReceiver([&](PeerHandle, const uint8_t* d, size_t n, Channel) { got += n == 3 && d[0] == 7; });
    CHECK(a.connect(Address::v4(127, 0, 0, 1, sb.local().port)));
    PeerHandle ab = kNoPeer;
    const double t0 = clockNow();
    while (clockNow() - t0 < 3 && got < 100) {
        const double t = clockNow();
        a.poll(t), b.poll(t);
        for (auto& e : a.takeEvents())
            if (e.type == HostEventType::Connected) {
                ab = e.peer;
                std::printf("  loopback handshake: %.2f ms\n", (t - t0) * 1000);
                for (int i = 0; i < 100; ++i) a.send(ab, Bytes{7, uint8_t(i), 0}, Channel::Reliable);
            }
        b.takeEvents();
        a.flush(), b.flush();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(ab != kNoPeer && got == 100);
    if (sa.dualStack()) {
        UdpSocket sc;
        sc.open();
        Host c(sc, Identity::generate());
        Address v6;
        Address::parse("[::1]:1", v6);
        v6.port = sb.local().port;
        c.connect(v6);
        bool ok = false;
        const double t1 = clockNow();
        while (clockNow() - t1 < 2 && !ok) {
            const double t = clockNow();
            c.poll(t), b.poll(t);
            for (auto& e : c.takeEvents()) ok = ok || e.type == HostEventType::Connected;
            b.takeEvents();
            c.flush(), b.flush();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        CHECK(ok);  // the same dual-stack socket answers IPv6
    }

    // LAN discovery: two games on this machine, no DHT, one arena code.
    UdpSocket la, lb;
    RendezvousConfig rc;
    rc.useDht = false;
    if (!openLanSocket(la, rc) || !openLanSocket(lb, rc)) {
        std::printf("  LAN multicast unavailable here: %s\n", (la.error() + lb.error()).c_str());
        return;
    }
    UdpSocket ga, gb;
    ga.open(), gb.open();
    HostConfig hc;
    hc.application = "lan-test";
    Host ha(ga, Identity::generate(), hc), hb(gb, Identity::generate(), hc);
    Rendezvous ma(ha, rc), mb(hb, rc);
    ma.setLanSocket(&la);
    mb.setLanSocket(&lb);
    const Topic t = arenaTopic("lan-test", newArenaCode());
    ma.join(t), mb.join(t);
    const double t2 = clockNow();
    bool met = false;
    while (clockNow() - t2 < 5 && !met) {
        const double now = clockNow();
        ha.poll(now), hb.poll(now);
        ma.poll(now), mb.poll(now);
        ha.takeEvents(), hb.takeEvents();
        mb.takeEvents();
        for (auto& e : ma.takeEvents()) met = met || e.type == RendezvousEventType::PeerJoined;
        ha.flush(), hb.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (met) std::printf("  LAN beacons: met in %.0f ms\n", (clockNow() - t2) * 1000);
    else std::printf("  LAN beacons: not delivered on this machine (%llu sent, %llu heard): multicast loopback blocked\n",
                     (unsigned long long)ma.status().lanBeaconsSent, (unsigned long long)ma.status().lanBeaconsHeard);
}

// The network on its own thread: a steady stream while the game's frame
// stalls, and a game that holds the lock only delays it.
static void networkThread() {
    std::printf("network thread\n");
    UdpSocket sa, sb;
    CHECK(sa.open() && sb.open());
    if (!sa.isOpen() || !sb.isOpen()) return;
    Host a(sa, Identity::generate()), b(sb, Identity::generate());
    NetworkThread ta(a, nullptr, sa), tb(b, nullptr, sb);
    std::vector<double> arrivals;  // on b's thread, lock held
    b.setReceiver([&](PeerHandle, const uint8_t*, size_t n, Channel) {
        if (n == 4) arrivals.push_back(tb.now());
    });
    {
        auto held = ta.lock();
        CHECK(a.connect(Address::v4(127, 0, 0, 1, sb.local().port)));
    }
    // a sends four bytes every step (60 a second) once linked.
    ta.start([&](double) {
        for (PeerHandle p : a.peers()) a.send(p, Bytes{1, 2, 3, 4}, Channel::Unreliable);
    });
    tb.start();
    // The game's frame stalls for 600 ms: the network does not.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    size_t during = 0;
    double gap = 0;
    {
        auto held = tb.lock();
        during = arrivals.size();
        for (size_t i = 1; i < arrivals.size(); ++i) gap = std::max(gap, arrivals[i] - arrivals[i - 1]);
    }
    // The game holds a's lock for 200 ms: a's sends wait, the link holds.
    {
        auto held = ta.lock();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ta.stop(), tb.stop();
    const NetworkThreadStats s = ta.stats();
    std::printf("  %zu snapshots in a 600 ms stall of the game's frame (widest gap %.0f ms); %llu steps, %llu polls, "
                "slowest poll %.2f ms\n",
                during, gap * 1000, (unsigned long long)s.steps, (unsigned long long)s.polls, s.worstStep * 1000);
    CHECK(during >= 25 && gap < 0.1);
    CHECK(a.peers().size() == 1 && b.peers().size() == 1);
    CHECK(arrivals.size() > during + 20);  // the stream resumed after the held lock
}

// test_p2p [seed [test ...]]: every simulated test is a function of the seed
// (network draws and keys alike), so a failure replays with the same arguments.
int main(int argc, char** argv) {
    if (argc > 1) seedOffset = std::strtoull(argv[1], nullptr, 10);
    const std::pair<const char*, void (*)()> tests[] = {
        {"crypto", crypto}, {"noise", noise}, {"formats", formats}, {"link", link},
        {"hostile", hostile}, {"reliability", reliability}, {"nat", natMatrix}, {"introductions", introductions},
        {"dht", dht}, {"dht6", dht6}, {"arena", arena}, {"cells", cells}, {"neighbourhood", neighbourhood}, {"crowd", crowd}, {"hotspot", hotspot}, {"relay", relay}, {"relayed-topic", relayedTopic},
        {"sockets", sockets}, {"thread", networkThread}};
    for (size_t i = 0; i < sizeof tests / sizeof *tests; ++i) {
        bool chosen = argc <= 2;
        for (int a = 2; a < argc; ++a) chosen = chosen || std::strcmp(argv[a], tests[i].first) == 0;
        if (!chosen) continue;
        seedRandomForTesting(seedOffset * 0x9e3779b97f4a7c15ULL + i + 1);
        tests[i].second();
    }
    std::printf(failures ? "FAIL saida::p2p (%d)\n" : "PASS saida::p2p\n", failures);
    return failures ? 1 : 0;
}
