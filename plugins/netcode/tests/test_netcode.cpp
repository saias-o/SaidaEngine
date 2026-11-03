// Contract tests for the netcode add-on. No engine, no window, no socket:
// one translation unit, the headers, standard library only.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -I../include test_netcode.cpp -o test_netcode && ./test_netcode
//
// Every session runs over SimNetwork with a toy arena: players pushing each
// other and kicking a ball, so a mispredicted input really changes the
// outcome. Each case is something players would notice: two screens showing
// different matches, a kick heard twice, a frozen game after someone's
// router died, a newcomer appearing in the wrong place.

#include "saida/netcode/Netcode.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace saida::netcode;

static int failures = 0;
static uint64_t seedOffset = 0;  // ./test_netcode <n> replays every bench with other draws

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

// --- The arena -----------------------------------------------------------------

struct Body {
    PlayerId id;
    uint32_t incarnation;
    int32_t x, y, vx, vy, score;
};

class Arena : public Simulation {
public:
    int32_t frame = 0;
    std::vector<Body> bodies;
    int32_t bx = 0, by = 0, bvx = 0, bvy = 0;
    uint64_t rng = 12345;
    std::vector<uint8_t> terrain;  // inert bulk, to make snapshots large

    // Host side, outside the simulated state.
    std::map<uint64_t, int> shown;  // effect key -> times currently shown
    int fired = 0, retracted = 0, doubled = 0;
    std::vector<std::pair<Frame, uint64_t>> commits;
    bool hidden = false;  // keeps a counter outside save(): not rollback-safe
    int hiddenCounter = 0;
    Frame faultFrom = kForever;  // this machine computes differently from here

    void save(Frame, std::vector<uint8_t>& out) override {
        Writer w(out);
        w.u64(uint64_t(uint32_t(frame)));
        w.u64(rng);
        for (int32_t v : {bx, by, bvx, bvy}) w.u64(uint64_t(uint32_t(v)));
        w.var(bodies.size());
        for (const Body& b : bodies) {
            w.u64(b.id);
            w.u64(b.incarnation);
            for (int32_t v : {b.x, b.y, b.vx, b.vy, b.score}) w.u64(uint64_t(uint32_t(v)));
        }
        w.var(terrain.size());
        w.bytes(terrain.data(), terrain.size());
    }
    void load(Frame, const uint8_t* data, size_t size) override {
        Reader r(data, size);
        auto i32 = [&] { return int32_t(uint32_t(r.u64())); };
        frame = i32();
        rng = r.u64();
        bx = i32(), by = i32(), bvx = i32(), bvy = i32();
        const size_t count = size_t(r.var());
        if (!r.ok() || count > r.left() / 56) return;  // a damaged state is refused, not read
        bodies.resize(count);
        for (Body& b : bodies) {
            b.id = r.u64();
            b.incarnation = uint32_t(r.u64());
            b.x = i32(), b.y = i32(), b.vx = i32(), b.vy = i32(), b.score = i32();
        }
        const size_t n = size_t(r.var());
        const uint8_t* t = r.bytes(n);
        if (r.ok()) terrain.assign(t, t + n);
    }
    void advance(const FrameContext& ctx) override {
        for (size_t d = 0; d < ctx.departedCount; ++d)
            for (size_t i = 0; i < bodies.size(); ++i)
                if (bodies[i].id == ctx.departed[d].player && bodies[i].incarnation == ctx.departed[d].incarnation) {
                    bodies.erase(bodies.begin() + long(i));
                    break;
                }
        for (size_t p = 0; p < ctx.playerCount; ++p) {
            const PlayerInput& in = ctx.players[p];
            Body* b = body(in.player, in.incarnation);
            if (!b) {
                const uint64_t h = mix64(in.player);
                bodies.push_back({in.player, in.incarnation, int32_t(h % 60000) - 30000,
                                  int32_t((h >> 20) % 60000) - 30000, 0, 0, 0});
                b = &bodies.back();
            }
            const int dx = int8_t(in.data[0]), dy = int8_t(in.data[1]);
            b->vx = b->vx * 7 / 8 + dx * 40;
            b->vy = b->vy * 7 / 8 + dy * 40;
            b->x = bounce(b->x + b->vx, b->vx);
            b->y = bounce(b->y + b->vy, b->vy);
            const int64_t ddx = bx - b->x, ddy = by - b->y;
            if (in.data[2] && ddx * ddx + ddy * ddy < int64_t(25000) * 25000) {
                bvx = int32_t(ddx / 8), bvy = int32_t(ddy / 8);
                ++b->score;
                const uint64_t key = uint64_t(ctx.frame) << 20 ^ in.player;
                if (ctx.effect(key)) {
                    ++fired;
                    if (++shown[key] > 1) ++doubled;
                }
                ctx.commit(key, &b->score, sizeof b->score);
            }
        }
        for (Body& a : bodies)
            for (Body& c : bodies)
                if (&a < &c && std::abs(a.x - c.x) < 1500 && std::abs(a.y - c.y) < 1500)
                    std::swap(a.vx, c.vx), std::swap(a.vy, c.vy);
        bvx = bvx * 31 / 32, bvy = bvy * 31 / 32;
        bx = bounce(bx + bvx, bvx), by = bounce(by + bvy, bvy);
        rng = mix64(rng + uint64_t(ctx.frame));
        if (hidden) bx += hiddenCounter++;
        if (ctx.frame >= faultFrom) by += 7;
        ++frame;
    }
    void commit(Frame f, uint64_t key, const uint8_t*, size_t) override { commits.push_back({f, key}); }
    // A game that knows its pad: directions stay held, a button is not.
    bool smartPredict = false;
    int predictions = 0;
    void predict(PlayerId, int, const uint8_t* last, uint8_t* out, size_t size) override {
        ++predictions;
        std::memcpy(out, last, size);
        if (smartPredict) out[2] = 0;
    }
    void retract(Frame, uint64_t key) override {
        ++retracted;
        --shown[key];
    }

private:
    Body* body(PlayerId id, uint32_t incarnation) {
        for (Body& b : bodies)
            if (b.id == id && b.incarnation == incarnation) return &b;
        return nullptr;
    }
    static int32_t bounce(int32_t v, int32_t& vel) {
        if (v > 50000 || v < -50000) vel = -vel, v = std::clamp(v, -50000, 50000);
        return v;
    }
};

// A deterministic stick: each player changes its mind every 12 of its ticks.
static void stick(PlayerId id, uint64_t tick, uint8_t* out) {
    const uint64_t h = mix64(id * 1000003 + tick / 12);
    std::memset(out, 0, 8);
    out[0] = uint8_t(int8_t(int(h % 3) - 1));
    out[1] = uint8_t(int8_t(int((h >> 8) % 3) - 1));
    out[2] = (h >> 16) % 5 == 0;
}

// --- The bench ---------------------------------------------------------------------

struct Peer {
    PlayerId id;
    PeerId address;
    Arena sim;
    std::unique_ptr<Session> session;
    TickClock clock;
    double rate = 1.0;  // this machine's clock drift
    bool crashed = false;
    double frozenUntil = -1, missed = 0;  // a host hitch: no poll, no tick
    double firstAdvance = -1;             // wall time of this machine's frame 0
    uint64_t ticks = 0;
    int advanced = 0, waited = 0, skipped = 0;
    std::vector<Event> events;

    bool saw(EventType type, PlayerId who = 0) const {
        for (const Event& e : events)
            if (e.type == type && (!who || e.player == who)) return true;
        return false;
    }
};

struct Bench {
    SimNetwork net;
    std::vector<std::unique_ptr<Peer>> peers;
    double now = 0, dt = 1.0 / 60;

    explicit Bench(uint64_t seed) : net(seed + seedOffset * 1000) {}

    Peer& add(PlayerId id, Config cfg, uint32_t incarnation = 1) {
        auto p = std::make_unique<Peer>();
        p->id = id;
        p->address = net.add();
        cfg.incarnation = incarnation;
        p->session = std::make_unique<Session>(cfg, id, p->sim, net.transport(p->address));
        // The P2P layer: everyone reaches everyone.
        for (auto& q : peers) {
            q->session->bindPeer(id, p->address);
            p->session->bindPeer(q->id, q->address);
        }
        peers.push_back(std::move(p));
        return *peers.back();
    }
    Peer& operator[](PlayerId id) {
        for (auto& p : peers)
            if (p->id == id) return *p;
        return *peers.front();
    }

    void step() {
        now += dt;
        net.setNow(now);
        for (auto& p : peers) {
            if (p->crashed) continue;
            if (now < p->frozenUntil) {
                p->missed += dt;
                continue;
            }
            Session& s = *p->session;
            net.deliver(p->address, now, [&](PeerId from, const uint8_t* d, size_t n) { s.receive(from, d, n, now); });
            s.poll(now);
            for (const Event& e : s.takeEvents()) {
                if (e.type == EventType::Started) p->clock.reset(s.startLateness());
                p->events.push_back(e);
            }
            const int ticks = p->clock.advance((dt + p->missed) * p->rate, s.timeScale());
            p->missed = 0;
            for (int t = 0; t < ticks; ++t) {
                uint8_t in[8];
                stick(p->id, p->ticks++, in);
                const TickResult r = s.tick(in);
                if (r.status == TickStatus::Advanced && p->firstAdvance < 0) p->firstAdvance = now;
                p->advanced += r.status == TickStatus::Advanced;
                p->waited += r.status == TickStatus::Waiting;
                p->skipped += r.status == TickStatus::Skipped;
            }
            for (const Event& e : s.takeEvents()) p->events.push_back(e);
        }
    }
    void run(double seconds) {
        for (int i = 0, n = int(seconds / dt); i < n; ++i) step();
    }

    // Frames from `from` on whose state hash every listed peer kept, all equal.
    int agree(const std::vector<PlayerId>& who, bool& equal, Frame from = 0) {
        equal = true;
        int compared = 0;
        Session& first = *(*this)[who.front()].session;
        for (Frame f = 0; f <= first.confirmedFrame() + 1; f += first.config().checksumInterval) {
            if (f < from) continue;
            uint64_t h0;
            if (!first.checksum(f, h0)) continue;
            bool all = true;
            for (PlayerId id : who) {
                uint64_t h;
                if (!(*this)[id].session->checksum(f, h)) all = false;
                else if (h != h0) equal = false;
            }
            compared += all;
        }
        return compared;
    }
};

static Config config() {
    Config c;
    c.sessionId = 0x5a1da;
    c.inputSize = 8;
    c.inputDelay = 2;
    c.maxPrediction = 8;
    c.checksumInterval = 15;
    return c;
}

static Frame finalThrough(Bench& b, const std::vector<PlayerId>& who) {
    Frame f = kForever;
    for (PlayerId id : who) {
        const Session& s = *b[id].session;
        f = std::min(f, std::min(s.confirmedFrame(), s.frame() - 1));
    }
    return f;
}

static std::vector<std::pair<Frame, uint64_t>> commitsThrough(const Peer& p, Frame f) {
    std::vector<std::pair<Frame, uint64_t>> out;
    for (const auto& c : p.sim.commits)
        if (c.first <= f) out.push_back(c);
    return out;
}

// --- Cases -------------------------------------------------------------------------

static void codec() {
    std::vector<uint8_t> buf;
    Writer w(buf);
    w.var(0), w.var(127), w.var(128), w.var(~0ULL), w.svar(-1), w.svar(-123456789), w.f64(3.25);
    Reader r(buf.data(), buf.size());
    check(r.var() == 0 && r.var() == 127 && r.var() == 128 && r.var() == ~0ULL, "varints round-trip");
    check(r.svar() == -1 && r.svar() == -123456789 && r.f64() == 3.25 && r.done() && r.ok(), "signed and floats round-trip");
    check(!(r.u8(), r.ok()), "reading past the end fails instead of reading garbage");

    // A player holding a stick for 64 frames costs a few bytes.
    std::vector<uint8_t> frames(64 * 8, 0);
    for (size_t i = 0; i < 64; ++i) frames[i * 8] = i < 30 ? 1 : 255, frames[i * 8 + 2] = i == 40;
    buf.clear();
    Writer e(buf);
    encodeInputs(e, frames.data(), 64, 8);
    std::vector<uint8_t> back(64 * 8, 0xcc);
    Reader d(buf.data(), buf.size());
    check(decodeInputs(d, back.data(), 64, 8) && back == frames, "input runs round-trip");
    check(buf.size() < 24, "64 frames of a held stick encode in " + std::to_string(buf.size()) + " bytes");

    // Random garbage never decodes past its buffer.
    uint64_t seed = 7;
    for (int t = 0; t < 2000; ++t) {
        std::vector<uint8_t> junk(size_t(mix64(++seed) % 64));
        for (auto& b : junk) b = uint8_t(mix64(++seed));
        Reader j(junk.data(), junk.size());
        std::vector<uint8_t> out(16 * 8);
        decodeInputs(j, out.data(), 16, 8);
    }
}

static void timing() {
    TickClock c(1.0 / 60, 4);
    int ticks = 0;
    for (int i = 0; i < 600; ++i) ticks += c.advance(1.0 / 60);
    check(ticks >= 599 && ticks <= 600, "a 60 Hz clock ticks once per 60 Hz frame");
    check(c.advance(2.0) == 4 && c.dropped() == 1, "a two-second hitch runs four ticks and drops the rest");
    ticks = 0;
    for (int i = 0; i < 600; ++i) ticks += c.advance(1.0 / 60, 0.95);
    check(ticks >= 565 && ticks <= 575, "a 0.95 time scale runs 5% fewer ticks");

    Smoother<2> s(0.1f, 4.f);
    s.correct({1.f, 0.f}, {0.f, 0.f});
    float drawn[2];
    s.apply({0.f, 0.f}, drawn);
    check(drawn[0] == 1.f, "a correction is drawn where the object was");
    s.decay(0.1f);
    s.apply({0.f, 0.f}, drawn);
    check(std::fabs(drawn[0] - 0.5f) < 1e-4f, "and glides halfway in one half-life");
    s.correct({100.f, 0.f}, {0.f, 0.f});
    s.apply({0.f, 0.f}, drawn);
    check(drawn[0] == 0.f, "a teleport-sized correction snaps");
}

static void ledger() {
    Ledger l;
    l.resize(4);
    l.begin(10, Pass::Live);
    check(l.fire(1) && l.fire(2) && !l.fire(1), "an effect fires once per frame");
    l.commit(9, "a", 1);
    int retracted = 0;
    l.end([&](Frame, uint64_t key) { retracted += key == 2; });
    l.begin(10, Pass::Resimulate);
    check(!l.fire(1), "an effect already shown is not shown again after a rollback");
    check(l.fire(3), "an effect the correction reveals is shown late");
    l.end([&](Frame f, uint64_t key) { retracted += f == 10 && key == 2; });
    check(retracted == 1, "an effect the correction removes is retracted");
    int commits = 0;
    l.flush(10, [&](uint64_t, const uint8_t*, size_t) { ++commits; });
    check(commits == 0, "a commit from a discarded simulation is forgotten");
    l.begin(11, Pass::CatchUp);
    check(!l.fire(1), "nothing fires while catching up");
    l.end([](Frame, uint64_t) {});
}

static void synctest() {
    Arena good;
    SyncTest t(good, 8, 6);
    bool ok = true;
    for (uint64_t f = 0; f < 300 && ok; ++f) {
        uint8_t in[16];
        stick(1, f, in);
        stick(2, f, in + 8);
        ok = t.advance({1, 2}, in);
    }
    check(ok && t.mismatch() == kNoFrame, "the arena survives replaying every frame");

    Arena bad;
    bad.hidden = true;
    SyncTest u(bad, 8, 6);
    for (uint64_t f = 0; f < 50 && u.advance({1}, std::vector<uint8_t>(8, 0).data()); ++f) {}
    check(u.mismatch() != kNoFrame && u.mismatch() < 10,
          "state kept outside save() is caught at frame " + std::to_string(u.mismatch()));
}

static void twoPlayers() {
    Bench b(11);
    b.net.setDefault({0.03, 0.0, 0.0, 0.0});
    b.add(1, config());
    b.add(2, config());
    b[1].session->start({1, 2}, 0);
    b[2].session->start({1, 2}, 0);
    b.run(10);
    bool equal;
    const int n = b.agree({1, 2}, equal);
    check(n >= 12 && equal, "two players on a clean line agree on " + std::to_string(n) + " hashed frames");
    check(b[1].advanced > 550 && b[2].advanced > 550, "and neither waits for the other");
    const Frame f = finalThrough(b, {1, 2});
    check(commitsThrough(b[1], f) == commitsThrough(b[2], f) && !commitsThrough(b[1], f).empty(),
          "both commit the same kicks");
}

static void lossyFour() {
    Bench b(23);
    b.net.setDefault({0.05, 0.03, 0.10, 0.05});  // 50-80 ms, 10% loss, 5% duplicated, reordered
    for (PlayerId id = 1; id <= 4; ++id) b.add(id, config()).rate = 1.0 + 0.01 * (double(id) - 2);
    for (auto& p : b.peers) p->session->start({1, 2, 3, 4}, 0);
    b.run(30);
    bool equal;
    const int n = b.agree({1, 2, 3, 4}, equal);
    check(n >= 12 && equal, "four players over a bad line agree on " + std::to_string(n) + " hashed frames");
    uint64_t rollbacks = 0;
    Frame deepest = 0;
    for (auto& p : b.peers) {
        rollbacks += p->session->counters().rollbacks;
        deepest = std::max(deepest, p->session->counters().deepestRollback);
        check(p->advanced > 1500, "player " + std::to_string(p->id) + " keeps playing (" +
                                      std::to_string(p->advanced) + " frames in 30 s)");
        check(p->sim.doubled == 0, "no effect is ever shown twice");
        check(!p->saw(EventType::Desync), "no desync");
        check(!p->saw(EventType::PlayerLost), "nobody is lost to packet loss");
    }
    check(rollbacks > 50, "mispredictions are corrected by rollback (" + std::to_string(rollbacks) + ")");
    check(deepest <= config().maxPrediction + 1, "no rollback is deeper than the prediction window");
    const Frame f = finalThrough(b, {1, 2, 3, 4});
    const auto ref = commitsThrough(b[1], f);
    check(ref.size() > 5, "kicks were committed (" + std::to_string(ref.size()) + ")");
    for (auto& p : b.peers) check(commitsThrough(*p, f) == ref, "every machine commits the same consequences once");
    // What is still shown is exactly what was committed.
    std::set<uint64_t> shown, committed;
    for (const auto& s : b[2].sim.shown)
        if (s.second == 1 && Frame(s.first >> 20) <= f) shown.insert(s.first);
    for (const auto& c : ref) committed.insert(c.second);
    check(shown == committed, "the effects left on screen are the confirmed ones");
    // Clock drift is absorbed by the time scale, not by stalls.
    Frame lo = kForever, hi = 0;
    for (auto& p : b.peers) lo = std::min(lo, p->session->frame()), hi = std::max(hi, p->session->frame());
    check(hi - lo <= config().maxPrediction + 4, "drifting clocks stay within " + std::to_string(hi - lo) + " frames");
    uint64_t bytes = 0;
    for (auto& p : b.peers) bytes = std::max(bytes, p->session->counters().bytesSent);
    std::printf("  lossy 4p: %llu rollbacks, deepest %d, %.1f KB/s sent per machine at most\n",
                (unsigned long long)rollbacks, deepest, double(bytes) / 30 / 1024);
    check(double(bytes) / 30 < 32 * 1024, "a four-player mesh stays under 32 KB/s up per machine");
}

static void joinAndLeave() {
    Bench b(5);
    b.net.setDefault({0.04, 0.02, 0.05, 0.0});
    Config cfg = config();
    b.add(1, cfg).sim.terrain.assign(150 * 1024, 0x5d);  // snapshots of ~150 KB: many chunks
    b[1].session->startAlone(0);
    b.run(2);
    check(b[1].advanced > 100, "a player alone plays without waiting");

    b.add(2, cfg, 7);
    b[2].session->join(1, b.now);
    b.run(3);
    check(b[2].saw(EventType::Joined) && b[2].saw(EventType::Live), "a newcomer loads a snapshot and goes live");
    check(b[1].saw(EventType::PlayerLive, 2), "its sponsor sees it arrive");
    check(b[2].sim.terrain.size() == 150 * 1024, "the whole state crossed in chunks");

    b.add(3, cfg, 9);
    b[3].session->join(2, b.now);  // sponsored by the newcomer
    b.run(4);
    check(b[3].saw(EventType::Live), "a newcomer can be sponsored by a newcomer");
    bool equal;
    int n = b.agree({1, 2, 3}, equal);
    check(n > 5 && equal, "three players who arrived one by one agree on " + std::to_string(n) + " frames");
    size_t bodies = b[3].sim.bodies.size();
    check(bodies == 3, "everyone sees everyone (" + std::to_string(bodies) + " bodies)");

    b[2].session->leave();
    b.run(3);
    check(b[2].session->state() == State::Left, "a leaver is done once acknowledged");
    check(b[1].saw(EventType::PlayerLeft, 2) && b[3].saw(EventType::PlayerLeft, 2), "the others see it leave");
    check(b[1].saw(EventType::PlayerRemoved, 2), "and forget it");
    check(b[1].sim.bodies.size() == 2 && b[3].sim.bodies.size() == 2, "its body is gone everywhere");
    n = b.agree({1, 3}, equal);
    check(n > 5 && equal, "the two who stayed still agree");

    // The same player comes back as a new incarnation.
    b.add(2, cfg, 8);
    b.peers.back()->session->join(3, b.now);
    b.run(4);
    check(b.peers.back()->saw(EventType::Live), "a player who left can come back");
}

static void abruptLoss(bool coordinatorDies) {
    Bench b(coordinatorDies ? 77 : 31);
    b.net.setDefault({0.03, 0.01, 0.02, 0.0});
    for (PlayerId id = 1; id <= 4; ++id) b.add(id, config());
    for (auto& p : b.peers) p->session->start({1, 2, 3, 4}, 0);
    b.run(3);
    const PlayerId victim = coordinatorDies ? 1 : 3;
    b[victim].crashed = true;
    b.net.isolate(b[victim].address, true);
    std::vector<PlayerId> survivors;
    for (PlayerId id = 1; id <= 4; ++id)
        if (id != victim) survivors.push_back(id);
    const int before = b[survivors[0]].advanced;
    b.run(6);
    const std::string who = coordinatorDies ? "the coordinator" : "a member";
    for (PlayerId id : survivors) {
        check(b[id].saw(EventType::PlayerDropped, victim), "when " + who + " vanishes, everyone drops it");
        check(b[id].sim.bodies.size() == 3, "and its body is gone");
    }
    check(b[survivors[0]].advanced - before > 60 * 3, "the survivors play on after the drop");
    bool equal;
    const int n = b.agree(survivors, equal);
    check(n > 5 && equal, "the survivors agree after losing " + who);
}

static void interruption() {
    Bench b(41);
    b.net.setDefault({0.02, 0.0, 0.0, 0.0});
    b.add(1, config());
    b.add(2, config());
    b[1].session->start({1, 2}, 0);
    b[2].session->start({1, 2}, 0);
    b.run(2);
    b.net.cut(b[1].address, b[2].address, true);
    b.run(0.8);
    check(b[1].waited > 0 && b[1].saw(EventType::Interrupted, 2), "a silent peer stalls the game and says so");
    b.net.cut(b[1].address, b[2].address, false);
    b.run(3);
    check(b[1].saw(EventType::Resumed, 2), "and it resumes when the line comes back");
    check(!b[1].saw(EventType::PlayerLost), "a short cut loses nobody");
    bool equal;
    check(b.agree({1, 2}, equal) > 5 && equal, "the two still agree");

    // Three players: a cut between two of them is bridged by the third.
    Bench c(43);
    c.net.setDefault({0.02, 0.0, 0.0, 0.0});
    for (PlayerId id = 1; id <= 3; ++id) c.add(id, config());
    for (auto& p : c.peers) p->session->start({1, 2, 3}, 0);
    c.run(2);
    c.net.cut(c[1].address, c[2].address, true);
    c.run(1.5);
    check(c[3].session->counters().relaysSent > 0, "a third player relays the missing inputs");
    c.net.cut(c[1].address, c[2].address, false);
    c.run(2);
    check(!c[1].saw(EventType::PlayerLost) && !c[2].saw(EventType::PlayerLost), "and nobody is dropped");
    check(c.agree({1, 2, 3}, equal) > 5 && equal, "the three still agree");
}

static void desyncRecovery() {
    Bench b(53);
    b.net.setDefault({0.03, 0.01, 0.0, 0.0});
    for (PlayerId id = 1; id <= 3; ++id) b.add(id, config());
    for (auto& p : b.peers) p->session->start({1, 2, 3}, 0);
    b.run(2);
    b[3].sim.faultFrom = b[3].session->frame() + 20;  // a machine that computes differently
    for (int i = 0; i < 600 && !b[3].saw(EventType::Resynced); ++i) b.step();
    check(b[3].saw(EventType::Desync, 1), "a diverging machine notices it differs from the coordinator");
    check(b[1].saw(EventType::Desync, 3), "and the coordinator notices too");
    check(b[3].saw(EventType::Resynced), "it reloads the coordinator's state");
    b[3].sim.faultFrom = kForever;  // the fault is gone
    b.run(3);
    Frame resynced = 0;
    for (const Event& e : b[3].events)
        if (e.type == EventType::Resynced) resynced = std::max(resynced, e.frame);
    bool equal;
    const int n = b.agree({1, 2, 3}, equal, resynced);
    check(n > 3 && equal, "after the resync everyone agrees again");
}

static void hostile() {
    // A real packet to mutate: what player 2 sends player 1.
    struct Tap : Transport {
        std::vector<uint8_t> last;
        void send(PeerId, const uint8_t* d, size_t n, Delivery) override { last.assign(d, d + n); }
    } tap, sink;
    Arena a, v;
    Session spy(config(), 2, a, tap);
    spy.bindPeer(1, 0);
    spy.start({1, 2}, 0);
    uint8_t in[8] = {};
    for (int i = 0; i < 5; ++i) spy.tick(in);
    spy.poll(0.5);
    check(!tap.last.empty(), "the spy produced a real packet");

    // Truncations, bit flips and noise from player 2's own address. Content
    // integrity is the transport's job; the session must survive anything.
    Session victim(config(), 1, v, sink);
    victim.bindPeer(2, 5);
    victim.start({1, 2}, 0);
    victim.poll(0.5);
    uint64_t seed = 99;
    for (int t = 0; t < 20000; ++t) {
        std::vector<uint8_t> m = tap.last;
        const int mode = t % 3;
        if (mode == 0) m.resize(size_t(mix64(++seed) % (m.size() + 1)));
        if (mode == 1)
            for (int k = 0; k < 3; ++k) {
                const size_t at = size_t(mix64(++seed) % m.size());
                m[at] ^= uint8_t(1 + mix64(++seed) % 255);
            }
        if (mode == 2) {
            m.resize(size_t(mix64(++seed) % 300));
            for (auto& x : m) x = uint8_t(mix64(++seed));
        }
        victim.receive(5, m.data(), m.size());
        if (t % 64 == 0) victim.tick(in), victim.poll(0.5 + t * 1e-4);
    }
    uint64_t rejected = 0;
    for (uint64_t r : victim.counters().rejected) rejected += r;
    check(rejected > 5000, "garbage is refused with a named reason (" + std::to_string(rejected) + ")");
    bool named = true;
    for (size_t r = 0; r < size_t(Reason::Count); ++r) named &= std::string(reasonName(Reason(r))) != "?";
    check(named, "every refusal reason has a name");

    // The right bytes from the wrong address.
    Bench b(61);
    b.add(1, config());
    b.add(2, config());
    b[1].session->start({1, 2}, 0);
    b[2].session->start({1, 2}, 0);
    b.run(1);
    for (int i = 0; i < 100; ++i) b[1].session->receive(b[1].address, tap.last.data(), tap.last.size());
    check(b[1].session->counters().rejected[size_t(Reason::Impersonation)] == 100,
          "a packet claiming another sender's id is refused");
    check(b[1].saw(EventType::Rejected) || !b[1].session->takeEvents().empty(), "and the refusal is reported");
    b.run(2);
    bool equal;
    check(b.agree({1, 2}, equal) > 3 && equal, "the real pair still agrees");
}

static void refusals() {
    Bench b(71);
    Config cfg = config();
    cfg.maxPlayers = 2;
    b.add(1, cfg);
    b[1].session->startAlone(0);
    b.run(0.5);
    b.add(2, cfg);
    b[2].session->join(1, b.now);
    b.run(2);
    check(b[2].saw(EventType::Live), "the second player gets in");
    b.add(3, cfg);
    b[3].session->join(1, b.now);
    b.run(2);
    bool full = false;
    for (const Event& e : b[3].events) full |= e.type == EventType::JoinFailed && e.reason == Reason::Full;
    check(full, "a full session refuses the third and says why");

    Config other = config();
    other.inputSize = 16;
    b.add(4, other);
    b[4].session->join(2, b.now);
    b.run(2);
    bool mismatch = false;
    for (const Event& e : b[4].events) mismatch |= e.type == EventType::JoinFailed && e.reason == Reason::InputSizeMismatch;
    check(mismatch, "a build with another input layout is refused and says why");
}

static void synchronizedStart() {
    Bench b(81);
    for (PlayerId id = 1; id <= 3; ++id) b.add(id, config());
    // Unequal lines: 20, 45 and 70 ms one way.
    auto line = [&](PlayerId x, PlayerId y, double latency) {
        b.net.setLink(b[x].address, b[y].address, {latency, 0.002, 0, 0});
        b.net.setLink(b[y].address, b[x].address, {latency, 0.002, 0, 0});
    };
    line(1, 2, 0.02);
    line(1, 3, 0.07);
    line(2, 3, 0.045);
    for (auto& p : b.peers) p->session->start({1, 2, 3}, 0);
    b.run(3);
    double lo = 1e9, hi = -1e9;
    for (auto& p : b.peers) {
        check(p->saw(EventType::Started), "founder " + std::to_string(p->id) + " is told when frame 0 begins");
        lo = std::min(lo, p->firstAdvance), hi = std::max(hi, p->firstAdvance);
    }
    check(hi - lo <= 2.0 / 60 + 1e-9, "founders on unequal lines begin frame 0 within " +
                                           std::to_string(int((hi - lo) * 1000 + 0.5)) + " ms of each other");
    check(lo < 0.5, "measuring the line takes a fraction of a second");
    bool equal;
    check(b.agree({1, 2, 3}, equal) > 3 && equal, "and they agree");
}

static void adaptiveDelay() {
    // 110 ms one way: two frames of delay is far too little.
    Bench b(83);
    b.net.setDefault({0.11, 0.005, 0.0, 0.0});
    b.add(1, config());
    b.add(2, config());
    b[1].session->start({1, 2}, 0);
    b[2].session->start({1, 2}, 0);
    b.run(6);
    const int high = b[1].session->inputDelay();
    check(high >= 4, "a long line raises the input delay (" + std::to_string(high) + " frames)");
    const Quality q = b[1].session->quality();
    check(q.rollbackFrames < 4 * 60, "so rollbacks stay small (" + std::to_string(int(q.rollbackFrames)) +
                                          " frames resimulated last second)");
    // The line improves: the delay comes down, one lossless frame at a time.
    b.net.setDefault({0.015, 0.002, 0.0, 0.0});
    b.run(8);
    const int low = b[1].session->inputDelay();
    check(low <= 1, "a short line lowers it again (" + std::to_string(low) + " frames)");
    check(b[1].session->counters().delayChanges >= uint64_t(high - low), "one frame at a time");
    bool equal;
    check(b.agree({1, 2}, equal) > 5 && equal, "changing delay never desynchronizes");
    check(b[1].sim.doubled == 0 && b[2].sim.doubled == 0, "nor shows an effect twice");
    const Frame f = finalThrough(b, {1, 2});
    check(commitsThrough(b[1], f) == commitsThrough(b[2], f), "nor changes what is committed");

    // A player's fixed setting wins over adaptation.
    b[2].session->setInputDelay(3);
    b.run(3);
    check(b[2].session->inputDelay() == 3, "a fixed delay is honoured");
}

static void hitch() {
    Bench b(85);
    b.net.setDefault({0.03, 0.002, 0.0, 0.0});
    b.add(1, config());
    b.add(2, config());
    b[1].session->start({1, 2}, 0);
    b[2].session->start({1, 2}, 0);
    b.run(3);
    b[2].frozenUntil = b.now + 0.3;  // a 300 ms hitch on machine 2
    b.run(4);
    check(b[1].skipped > 0, "the machine left ahead gives ticks back (" + std::to_string(b[1].skipped) + ")");
    const Frame gap = std::abs(b[1].session->frame() - b[2].session->frame());
    check(gap <= 3, "the two are back in step within a few seconds (" + std::to_string(gap) + " frames apart)");
    bool equal;
    check(b.agree({1, 2}, equal) > 5 && equal, "and agree");
}

static void prediction() {
    Bench b(87);
    b.net.setDefault({0.05, 0.01, 0.02, 0.0});
    b.add(1, config()).sim.smartPredict = true;
    b.add(2, config()).sim.smartPredict = true;
    b[1].session->start({1, 2}, 0);
    b[2].session->start({1, 2}, 0);
    b.run(5);
    check(b[1].sim.predictions > 100, "the game's own predictor is used");
    bool equal;
    check(b.agree({1, 2}, equal) > 5 && equal, "and a predictor of its own never desynchronizes");
}

static void quality() {
    Bench b(89);
    b.net.setDefault({0.012, 0.001, 0.0, 0.0});
    b.add(1, config());
    b.add(2, config());
    b[1].session->start({1, 2}, 0);
    b[2].session->start({1, 2}, 0);
    b.run(4);
    Quality q = b[1].session->quality();
    check(q.bars == 5, "a 24 ms round trip shows five bars (" + std::to_string(q.bars) + ")");
    check(q.rtt > 0.015 && q.rtt < 0.04, "and its round trip");
    b.net.setDefault({0.1, 0.01, 0.12, 0.0});
    b.run(6);
    q = b[1].session->quality();
    check(q.bars <= 2, "a 200 ms, 12% loss line shows two bars or fewer (" + std::to_string(q.bars) + ")");
    check(q.loss > 0.03, "and its loss");
    b.net.cut(b[1].address, b[2].address, true);
    b.run(0.8);
    check(b[1].session->quality().bars == 0, "a silent member shows no bar");
    b.net.cut(b[1].address, b[2].address, false);
    b.run(2);
}

// --- Replication ------------------------------------------------------------------

// An owner walking at 5 m/s sends its position at 20 Hz over a 60 ms line with
// jitter, loss and reordering, on a clock 1234.5 s off and drifting 50 ppm.
// The remote draws it at 60 Hz: smooth, a little behind, and what it draws is
// what the owner's history says it was at the instant named.
static void replicas() {
    uint64_t rng = 0x2545f4914f6cdd1dULL ^ (seedOffset * 0x9e3779b97f4a7c15ULL);
    auto uniform = [&] {
        rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
        return double(rng >> 11) / double(1ULL << 53);
    };
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    Replica<double> remote;
    History<double> owned(1.0);
    struct Flight { double arrive, sent, x; };
    std::vector<Flight> air;
    const double speed = 5.0, offset = 1234.5;
    auto ownerTime = [&](double t) { return t * (1 + 50e-6) + offset; };
    double nextSend = 0, lastDrawn = 0, worstStep = 0, worstLag = 0, bestLag = 1e9, worstHistory = 0;
    int interpolated = 0, sampled = 0, held = 0;
    bool first = true;
    for (int frame = 0; frame < 60 * 20; ++frame) {
        const double now = frame / 60.0;
        owned.record(ownerTime(now), speed * now);
        if (now >= nextSend) {
            nextSend += 0.05;
            if (uniform() >= 0.1) air.push_back({now + 0.06 + 0.03 * uniform(), ownerTime(now), speed * now});
        }
        std::sort(air.begin(), air.end(), [](const Flight& a, const Flight& b) { return a.arrive < b.arrive; });
        while (!air.empty() && air.front().arrive <= now) {
            remote.push(air.front().sent, now, air.front().x);
            air.erase(air.begin());
        }
        double drawn = 0;
        const ReplicaSample kind = remote.sample(now, drawn, lerp);
        if (kind == ReplicaSample::Empty || now < 2.0) {
            lastDrawn = drawn;
            continue;
        }
        ++sampled;
        interpolated += kind == ReplicaSample::Interpolated;
        held += kind == ReplicaSample::Held;
        if (!first) worstStep = std::max(worstStep, std::fabs((drawn - lastDrawn) - speed / 60.0));
        first = false;
        lastDrawn = drawn;
        const double lag = (speed * now - drawn) / speed;
        worstLag = std::max(worstLag, lag), bestLag = std::min(bestLag, lag);
        double was = 0;
        if (owned.at(remote.playback(), was, lerp)) worstHistory = std::max(worstHistory, std::fabs(was - drawn));
        else worstHistory = 1e9;
    }
    std::printf("  replica: %.1f%% between snapshots, drawn %.0f-%.0f ms behind the owner, delay %.0f ms\n",
                100.0 * interpolated / sampled, bestLag * 1000, worstLag * 1000, remote.targetDelay() * 1000);
    check(interpolated > sampled * 90 / 100 && held == 0,
          "a remote object is drawn between real snapshots, a lost one bridged, never frozen");
    check(worstStep < speed / 60.0 * 0.25, "it moves smoothly: no frame's step is a quarter off its speed");
    check(bestLag > 0.06 && worstLag < 0.3, "it is drawn a little behind, never more than the line asks");
    check(worstHistory < 1e-3, "the owner's history at the instant drawn is what was drawn");

    // The owner goes silent: extrapolated a moment, then held; back, it snaps on.
    double now = 20.0, drawn = 0, before = 0;
    remote.sample(now, before, lerp);
    ReplicaSample kind = ReplicaSample::Empty;
    for (int i = 0; i < 60; ++i) now += 1.0 / 60, kind = remote.sample(now, drawn, lerp);
    check(kind == ReplicaSample::Held && drawn - before < speed * 0.6, "a silent owner is held, not run off into the distance");
    remote.push(ownerTime(now), now + 0.06, speed * now);
    remote.push(ownerTime(now + 0.05), now + 0.11, speed * (now + 0.05));
    now += 0.2;
    remote.sample(now, drawn, lerp);
    check(std::fabs(drawn - speed * (now - 0.1)) < speed * 0.15, "and drawn on its new path as soon as it speaks again");

    // Two loaded machines: the owner sends once per frame of 130-240 ms, the
    // remote draws at 7 frames a second. The delay covers the worst recent
    // gap, so the drawing still runs between real snapshots.
    {
        Replica<double> slow;
        double sendAt = 0, lastFrame = 0;
        int between = 0, frames = 0;
        std::vector<Flight> queue;
        for (double t = 0; t < 30; t += 0.001) {
            if (t >= sendAt) {
                queue.push_back({t + 0.02 + 0.01 * uniform(), t, speed * t});
                sendAt = t + 0.13 + 0.11 * uniform();
            }
            if (t - lastFrame < 0.14) continue;
            lastFrame = t;
            for (auto it = queue.begin(); it != queue.end();)
                if (it->arrive <= t) slow.push(it->sent, t, it->x), it = queue.erase(it);
                else ++it;
            double x = 0;
            const ReplicaSample kind = slow.sample(t, x, lerp);
            if (t < 5) continue;
            ++frames;
            between += kind == ReplicaSample::Interpolated;
        }
        check(between > frames * 95 / 100, "a stuttering owner on a slow machine is still drawn between real snapshots");
    }

    check(!remote.push(std::nan(""), now, 0) && !remote.push(ownerTime(now), std::nan(""), 0), "a snapshot without a time is refused");
    check(!remote.push(ownerTime(now - 0.15), now, 0), "a duplicate snapshot is refused");
    double old = 0;
    check(!owned.at(ownerTime(0.5), old, lerp), "a claim older than the history is not judged");
}

static void replays() {
    Bench b(91);
    b.net.setDefault({0.05, 0.02, 0.08, 0.02});
    for (PlayerId id = 1; id <= 3; ++id) b.add(id, config());
    for (auto& p : b.peers) p->session->start({1, 2, 3}, 0);
    Replay replay;
    b[2].session->record(&replay);
    b.run(4);
    b.add(4, config(), 5);
    b[4].session->join(1, b.now);
    b.run(4);
    b[3].session->leave();
    b.run(4);
    check(replay.frames() > 600, "a match is recorded (" + std::to_string(replay.frames()) + " frames)");
    const std::vector<uint8_t> bytes = replay.bytes();
    Replay loaded;
    check(loaded.parse(bytes.data(), bytes.size()), "a replay survives storage");
    std::printf("  replay: %zu frames of 3-4 players in %zu bytes\n", loaded.frames(), bytes.size());

    Arena viewer;
    ReplayPlayer player(loaded, viewer, true);
    while (player.step()) {}
    check(player.done() && player.mismatch() == kNoFrame && !player.broken(),
          "playing it back reproduces every recorded state hash");
    check(viewer.bodies.size() == 3, "including who joined and who left (" + std::to_string(viewer.bodies.size()) + ")");
    std::vector<std::pair<Frame, uint64_t>> recorded;
    for (const auto& c : b[2].sim.commits)
        if (c.first >= loaded.first() && c.first < loaded.end()) recorded.push_back(c);
    check(!recorded.empty() && viewer.commits == recorded, "and every committed consequence, in order");

    // A simulation that changed since the recording is caught at once.
    Arena changed;
    changed.faultFrom = loaded.first() + 100;
    ReplayPlayer drift(loaded, changed);
    while (drift.step()) {}
    check(drift.mismatch() != kNoFrame && drift.mismatch() <= loaded.first() + 101 + config().checksumInterval,
          "a changed simulation is caught at frame " + std::to_string(drift.mismatch()));

    // A damaged or truncated replay is refused whole.
    uint64_t seed = 3;
    int refused = 0;
    for (int t = 0; t < 300; ++t) {
        std::vector<uint8_t> damaged = bytes;
        if (t % 2) damaged.resize(size_t(mix64(++seed) % damaged.size()));
        else damaged[size_t(mix64(++seed) % damaged.size())] ^= uint8_t(1 + t % 255);
        Replay r;
        refused += !r.parse(damaged.data(), damaged.size());
    }
    check(refused == 300, "every damaged or truncated replay is refused (" + std::to_string(refused) + "/300)");
}

int main(int argc, char** argv) {
    if (argc > 1) seedOffset = std::strtoull(argv[1], nullptr, 10);
    codec();
    timing();
    ledger();
    synctest();
    twoPlayers();
    lossyFour();
    joinAndLeave();
    abruptLoss(false);
    abruptLoss(true);
    interruption();
    desyncRecovery();
    hostile();
    refusals();
    synchronizedStart();
    adaptiveDelay();
    hitch();
    prediction();
    quality();
    replays();
    replicas();
    if (failures) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("PASS saida::netcode\n");
    return 0;
}
