// Rollback netcode: a fixed-step simulation shared by every participant, each
// machine simulating everyone, with no server and no arbiter.
//
// Header-only, standard library only. It knows nothing about the engine, the
// socket underneath or what is being simulated. The host supplies three
// things -- a Simulation (save, load, advance one frame), a Transport (send a
// datagram to a peer) and the packets it receives -- and calls tick() once per
// fixed step with its local input.
//
// The model:
//   - Every participant's input for frame f is a fixed-size blob. A frame is
//     *confirmed* once every input it needs is known. Local input is
//     scheduled `inputDelay` frames ahead, sent to everyone at once and resent
//     until acknowledged; missing remote inputs are *predicted* (the last one
//     received is repeated) for at most `maxPrediction` frames.
//   - When a received input differs from what was predicted, the state saved
//     before that frame is loaded and the frames since are simulated again
//     (rollback). The present is never delayed by the network; the past is
//     corrected. Effects played during a prediction are not played twice
//     (FrameContext::effect), and persistent consequences are only handed out
//     once their frame is confirmed (FrameContext::commit).
//   - Membership is part of the input stream. A participant admits a newcomer,
//     makes it live and drops a lost peer by writing a command into its own
//     input for some frame; every machine applies the same commands at the same
//     frames, so the roster at any frame is as deterministic as the inputs.
//     A newcomer receives a confirmed snapshot, catches up, then goes live at a
//     frame its sponsor sets. Any member may sponsor; competing commands are
//     ordered by (frame, sponsor id).
//   - A peer silent past `disconnectSeconds` is reported lost to everyone; the
//     member with the lowest id still standing writes the drop, at the last
//     frame any member received from it. Members that miss some of those
//     frames fetch them from each other (input relay). Every member must reach
//     every other: a participant that loses one link is dropped by all.
//   - Each member publishes the hash of confirmed states every
//     `checksumInterval` frames. A mismatch with the coordinator's hash is a
//     desynchronization; the session reloads the coordinator's confirmed
//     state and simulates forward from it.
//
// Refusals are never silent: every refused packet, admission or rollback is
// counted under a named Reason and reported as an Event.
#pragma once

#include "saida/netcode/Replay.hpp"
#include "saida/netcode/Simulation.hpp"
#include "saida/netcode/Wire.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace saida::netcode {

inline constexpr uint8_t kProtocolVersion = 1;
inline constexpr size_t kMaxFramesPerBlock = 128;
inline constexpr size_t kMaxCommandsPerFrame = 8;

// --- What the host provides ---------------------------------------------------

enum class Delivery : uint8_t {
    Unreliable,  // may be lost, duplicated or reordered: inputs, pings
    Reliable,    // delivered once and in order per peer: control, snapshots
};

// The datagram layer underneath. A message is never larger than
// Config::maxMessageBytes. Reliable messages to one peer must arrive in the
// order sent; the transport authenticates who a PeerId is.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(PeerId to, const uint8_t* data, size_t size, Delivery delivery) = 0;
};

struct Config {
    uint64_t sessionId = 0;         // shared by all members; the founder picks it (nonzero)
    uint32_t incarnation = 1;       // a joiner's: new for every rejoin (founders are all 1)
    uint16_t inputSize = 8;         // bytes of one player's input for one frame
    int inputDelay = 2;             // frames between sampling and using local input (initial)
    // Delay follows the line, like a fighting game's "auto" setting: the
    // one-way time to the farthest member (plus twice its jitter), in frames,
    // minus the rollback this machine is willing to show. Changes are one
    // frame at a time and never lose or invent an input.
    bool adaptiveDelay = true;
    int minInputDelay = 1, maxInputDelay = 6;
    int rollbackComfort = 3;        // frames of rollback accepted before adding delay
    int maxPrediction = 8;          // frames simulated past the last confirmed one
    double skipAdvantage = 2.5;     // frames ahead of the others past which a tick is skipped
    int skipInterval = 4;           // at most one skipped tick per this many
    int inputHistory = 512;         // frames of input kept per player
    int maxPlayers = 16;
    int checksumInterval = 30;      // frames between published state hashes
    int maxCatchUpFrames = 8;       // frames simulated per tick while catching up
    double tickSeconds = 1.0 / 60.0;
    double resendSeconds = 0.05;    // unacknowledged input is resent this often while no new input goes out
    double heartbeatSeconds = 0.1;
    double pingSeconds = 0.25;
    double interruptSeconds = 0.5;  // silence reported as an interruption
    double disconnectSeconds = 2.0; // silence after which a member is lost
    double relayAfterSeconds = 0.2; // missing input is asked from others after this
    double joinTimeoutSeconds = 10.0;
    double syncSeconds = 3.0;       // founders measure the line at most this long before frame 0
    int syncSamples = 4;            // round trips measured per founder before frame 0
    double lingerSeconds = 1.0;     // a leaver waits this long for acknowledgements
    double maxTimeAdjust = 0.1;     // bound of the time scale around 1
    size_t maxMessageBytes = 1200;
    size_t receiveBudget = 256 * 1024;      // bytes per second per peer
    size_t maxSnapshotBytes = 64u << 20;
    bool autoAdmit = true;
    bool autoResync = true;
};

// --- What the session reports --------------------------------------------------

enum class Reason : uint8_t {
    None,
    Timeout,
    Full,
    NotReady,
    InputSizeMismatch,
    BadMagic,
    BadVersion,
    WrongSession,
    UnknownPeer,
    Impersonation,
    UnknownSender,
    StaleIncarnation,
    SenderLost,
    Malformed,
    OutOfWindow,
    OverBudget,
    TooLarge,
    HistoryExhausted,
    SnapshotTooLarge,
    Refused,
    Unreachable,
    Kicked,
    Count
};

inline const char* reasonName(Reason r) {
    static const char* const names[] = {
        "none", "timeout", "session full", "not ready", "input size mismatch", "bad magic",
        "bad protocol version", "wrong session", "unknown peer", "sender impersonation",
        "unknown sender", "stale incarnation", "sender lost", "malformed", "frame out of window",
        "over receive budget", "message too large", "input history exhausted",
        "snapshot too large", "refused by host", "member unreachable", "dropped by the session"};
    const size_t i = size_t(r);
    return i < size_t(Reason::Count) ? names[i] : "?";
}

enum class EventType : uint8_t {
    Joined,         // snapshot loaded at `frame`; catching up
    Live,           // the local player takes part from `frame`
    PlayerAdded,    // admitted by a sponsor: connect to it
    PlayerLive,     // takes part from `frame`
    PlayerLeft,     // last frame `frame`
    PlayerDropped,  // last frame `frame`
    PlayerRemoved,  // gone from the roster: its peer may be unbound
    PlayerLost,     // silent past disconnectSeconds, or reported so by a member
    Interrupted,    // silent past interruptSeconds
    Resumed,
    JoinRequested,  // autoAdmit is off: admit() or refuse()
    JoinFailed,     // `reason`
    Desync,         // state hash at `frame` differs from `player`'s: `local`, `remote`
    Resynced,       // reloaded the coordinator's state at `frame`
    Rejected,       // a packet was refused: `reason` (at most once per reason per second)
    Left,           // the session is over for this machine: `reason`
    Started,        // founders synchronized: frame 0 begins now, `local` = input delay
    InputDelay,     // the local input delay is now `local` frames
};

struct Event {
    EventType type;
    PlayerId player = 0;
    uint32_t incarnation = 0;
    Frame frame = kNoFrame;
    Reason reason = Reason::None;
    uint64_t local = 0, remote = 0;
};

enum class State : uint8_t { Idle, Synchronizing, Joining, CatchingUp, Running, Leaving, Left, Failed };
enum class TickStatus : uint8_t { Advanced, Waiting, Skipped, Synchronizing, Joining, CatchingUp, Idle, Left, Failed };

struct TickResult {
    TickStatus status = TickStatus::Idle;
    PlayerId waitingFor = 0;  // Waiting: the participant whose input is missing
};

// The line as a player would want it shown: the worst member counts.
struct Quality {
    double rtt = 0.0, jitter = 0.0, loss = 0.0;  // seconds, seconds, fraction
    double rollbackFrames = 0.0;  // frames resimulated per second, last second
    Frame deepestRollback = 0;    // last second
    double stalls = 0.0;          // ticks waited per second, last second
    int inputDelay = 0;
    double advantage = 0.0;
    int bars = 5;                 // 0 (interrupted) .. 5
};

struct Counters {
    uint64_t framesSimulated = 0, framesResimulated = 0, rollbacks = 0, stalls = 0, skips = 0;
    uint64_t delayChanges = 0, statesSaved = 0;
    Frame deepestRollback = 0;
    uint64_t messagesSent = 0, messagesReceived = 0, bytesSent = 0, bytesReceived = 0;
    uint64_t duplicates = 0, desyncs = 0, resyncs = 0, snapshotsSent = 0, relaysSent = 0;
    uint64_t rejected[size_t(Reason::Count)] = {};
};

struct PeerStats {
    PlayerId player = 0;
    uint32_t incarnation = 0;
    bool bound = false, live = false, pending = false, lost = false, interrupted = false;
    double rtt = 0.0, jitter = 0.0, loss = 0.0, silence = 0.0, advantage = 0.0;
    Frame remoteFrame = kNoFrame, received = kNoFrame, acknowledged = kNoFrame;
    uint64_t bytesIn = 0, bytesOut = 0;
};

struct Member {
    PlayerId player;
    uint32_t incarnation;
    Frame liveFrom, liveUntil;  // kForever: not (yet) set
    bool local, lost;
};

// --- The session ------------------------------------------------------------------

class Session {
public:
    Session(const Config& config, PlayerId local, Simulation& simulation, Transport& transport)
        : cfg_(config), local_(local), sim_(simulation), net_(transport) {
        cfg_.inputSize = std::max<uint16_t>(cfg_.inputSize, 1);
        cfg_.minInputDelay = std::clamp(cfg_.minInputDelay, 0, 30);
        cfg_.maxInputDelay = std::clamp(cfg_.maxInputDelay, cfg_.minInputDelay, 30);
        cfg_.inputDelay = std::clamp(cfg_.inputDelay, 0, 30);
        cfg_.maxPrediction = std::clamp(cfg_.maxPrediction, 1, 120);
        cfg_.skipInterval = std::max(cfg_.skipInterval, 1);
        cfg_.syncSamples = std::max(cfg_.syncSamples, 1);
        cfg_.inputHistory = std::max(
            cfg_.inputHistory, 4 * (cfg_.maxPrediction + std::max(cfg_.inputDelay, cfg_.maxInputDelay)) + 64);
        delay_ = cfg_.inputDelay;
        cfg_.maxPlayers = std::max(cfg_.maxPlayers, 1);
        cfg_.checksumInterval = std::max(cfg_.checksumInterval, 1);
        cfg_.maxCatchUpFrames = std::max(cfg_.maxCatchUpFrames, 1);
        cfg_.maxMessageBytes = std::max<size_t>(cfg_.maxMessageBytes, 256);
        if (cfg_.incarnation == 0) cfg_.incarnation = 1;
        history_ = size_t(cfg_.inputHistory);
        states_ = size_t(cfg_.maxPrediction) + 2;
        stateRing_.assign(states_, {});
        stateTags_.assign(states_, kNoFrame);
        ledger_.resize(states_);
        zeros_.assign(cfg_.inputSize, 0);
        for (double& t : lastReject_) t = -1e300;
    }

    // --- Lifecycle ---------------------------------------------------------------

    // Founds a session at frame 0 with a fixed roster: every founder calls it
    // with the same list and session id, and all founders are incarnation 1.
    // `now` is the host clock poll() will be given. With other founders, the
    // session first measures the line to each (Synchronizing); the
    // coordinator then gives everyone a start time, offset by each one's
    // one-way delay, so frame 0 begins at the same instant everywhere (the
    // Started event: reset the host's TickClock then).
    bool start(std::vector<PlayerId> founders, double now) {
        if (state_ != State::Idle || cfg_.sessionId == 0) return false;
        now_ = now;
        cfg_.incarnation = 1;
        if (std::find(founders.begin(), founders.end(), local_) == founders.end()) founders.push_back(local_);
        for (PlayerId id : founders) {
            Slot& s = slot(id, 1);
            s.local = id == local_;
            s.liveFrom = 0;
            s.liveKey = {kNoFrame, 0};
            s.announcedLive = true;
            s.acked = true;  // every founder starts from frame 0
        }
        syncStarted_ = now;
        state_ = slots_.size() > 1 ? State::Synchronizing : State::Running;
        return true;
    }
    bool startAlone(double now) { return start({local_}, now); }

    // Asks `sponsor`, an existing member whose peer is bound, to admit us.
    bool join(PlayerId sponsor, double now) {
        if (state_ != State::Idle) return false;
        now_ = now;
        sponsor_ = sponsor;
        state_ = State::Joining;
        joinStarted_ = now;
        return true;
    }

    // Graceful departure: the local player's last frame is written into its
    // input; the session lingers until the members acknowledge it.
    void leave() {
        const Slot* me = local();
        if (state_ == State::Running && me && me->member()) {
            schedule({CommandType::Leave, local_, cfg_.incarnation, 0});
            state_ = State::Leaving;
            leaveStarted_ = now_;
        } else if (state_ != State::Left && state_ != State::Failed && state_ != State::Idle) {
            finish(State::Left, Reason::None);
        }
    }

    // The transport's authenticated endpoint for a participant.
    void bindPeer(PlayerId player, PeerId peer) {
        for (auto& b : bindings_)
            if (b.first == player) return void(b.second = peer);
        bindings_.push_back({player, peer});
    }
    void unbindPeer(PlayerId player) {
        bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                       [&](const auto& b) { return b.first == player; }),
                        bindings_.end());
    }

    // With autoAdmit off, answers a JoinRequested event.
    bool admit(PlayerId player) {
        for (size_t i = 0; i < requests_.size(); ++i)
            if (requests_[i].first == player) {
                const uint32_t incarnation = requests_[i].second;
                requests_.erase(requests_.begin() + long(i));
                return admitNow(player, incarnation);
            }
        return false;
    }
    void refuse(PlayerId player) {
        for (size_t i = 0; i < requests_.size(); ++i)
            if (requests_[i].first == player) {
                sendRefusal(player, Reason::Refused);
                requests_.erase(requests_.begin() + long(i));
                return;
            }
    }

    // Reloads the coordinator's confirmed state. Automatic on a desync with
    // the coordinator when autoResync is on.
    bool resync() {
        Slot* c = coordinator();
        if (!c || c->local || state_ != State::Running || !peerOf(c->id)) return false;
        send(*c, message(MessageType::ResyncRequest, 0), Delivery::Reliable);
        resyncPending_ = true;
        resyncAsked_ = now_;
        return true;
    }

    // A fixed local input delay (a player's setting); turns adaptiveDelay
    // off. Reached one frame at a time, like adaptive changes.
    void setInputDelay(int frames) {
        cfg_.adaptiveDelay = false;
        wantedDelay_ = std::clamp(frames, 0, 30);
        wantedSince_ = now_;
    }
    int inputDelay() const { return delay_; }

    // Records every confirmed frame into `replay` from the next confirmed
    // checksum frame on (nullptr stops). A resync restarts the recording.
    void record(Replay* replay) { replay_ = replay; }

    // --- The loop ----------------------------------------------------------------

    // Every received datagram. Never simulates; never blocks. `now`, when
    // given, is the arrival time (it refines the synchronized start).
    void receive(PeerId from, const uint8_t* data, size_t size, double now = -1.0) {
        if (now >= now_) now_ = now;
        ++counters_.messagesReceived;
        counters_.bytesReceived += size;
        if (state_ == State::Idle || state_ == State::Left || state_ == State::Failed) return;
        if (size > cfg_.maxMessageBytes) return reject(Reason::TooLarge);
        Reader r(data, size);
        const uint8_t m0 = r.u8(), m1 = r.u8(), version = r.u8(), rawType = r.u8();
        const uint64_t session = r.var();
        const PlayerId sender = r.var();
        const uint64_t incarnation = r.var();
        const uint64_t seq = r.var();
        if (!r.ok()) return reject(Reason::Malformed);
        if (m0 != 'S' || m1 != 'N') return reject(Reason::BadMagic);
        if (version != kProtocolVersion) return reject(Reason::BadVersion);
        if (rawType == 0 || rawType >= uint8_t(MessageType::Count) || incarnation > 0xffffffffULL)
            return reject(Reason::Malformed);
        const PeerId* bound = peerOf(sender);
        if (!bound) return reject(Reason::UnknownPeer);
        if (*bound != from) return reject(Reason::Impersonation);
        const auto type = MessageType(rawType);
        const auto inc = uint32_t(incarnation);

        if (state_ == State::Joining) {
            if (sender != sponsor_) return reject(Reason::UnknownSender);
            if (type == MessageType::JoinRefused) {
                const uint8_t raw = r.u8();
                const auto reason = raw < uint8_t(Reason::Count) ? Reason(raw) : Reason::Refused;
                return finish(State::Failed, reason, EventType::JoinFailed);
            }
            if (type == MessageType::SnapshotChunk) return onChunk(r);
            return;  // nothing else matters before the snapshot
        }
        if (session != cfg_.sessionId && !(type == MessageType::JoinRequest && session == 0))
            return reject(Reason::WrongSession);
        if (type == MessageType::JoinRequest) return onJoinRequest(r, sender, inc);

        Slot* s = find(sender, inc);
        if (!s) return reject(find(sender) ? Reason::StaleIncarnation : Reason::UnknownSender);
        if (s->local) return reject(Reason::Impersonation);
        if (s->lost && type != MessageType::LostReport) return reject(Reason::SenderLost);
        // A snapshot this machine asked for may exceed the budget.
        if (!(type == MessageType::SnapshotChunk && resyncPending_)) {
            if (now_ - s->budgetStart >= 1.0) s->budgetStart = now_, s->budgetUsed = 0;
            s->budgetUsed += size;
            if (s->budgetUsed > cfg_.receiveBudget) return reject(Reason::OverBudget);
        }
        if (seq && !s->sequence(seq)) return void(++counters_.duplicates);
        s->bytesIn += size;
        s->lastHeard = now_;
        s->heardAny = true;
        if (s->interrupted) {
            s->interrupted = false;
            emit(EventType::Resumed, *s);
        }
        switch (type) {
        case MessageType::Input: return onInput(r, *s);
        case MessageType::Ping: return onPing(r, *s);
        case MessageType::Pong: return onPong(r, *s);
        case MessageType::Ready: return onReady(r, *s);
        case MessageType::LostReport: return onLostReport(r, *s);
        case MessageType::InputRequest: return onInputRequest(r, *s);
        case MessageType::Relay: return onRelay(r);
        case MessageType::ResyncRequest: return onResyncRequest(*s);
        case MessageType::SnapshotChunk:
            if (resyncPending_) onChunk(r);
            return;
        case MessageType::Start: return onStart(r);
        default: return;
        }
    }

    // Timers, retransmission, loss detection. Call once per rendered frame with
    // a monotonic time in seconds, after delivering the frame's packets.
    void poll(double now) {
        now_ = now;
        if (state_ == State::Joining) return pollJoin();
        if (state_ == State::Synchronizing) return pollSync();
        if (state_ != State::CatchingUp && state_ != State::Running && state_ != State::Leaving) return;
        rollWindow();
        adaptDelay();
        const bool member = isMember();
        for (auto& p : slots_) {
            Slot& s = *p;
            if (s.local || s.retiring()) continue;
            if (!s.lost && peerOf(s.id) && now_ - s.lastPing >= cfg_.pingSeconds) {
                s.lastPing = now_;
                Writer(message(MessageType::Ping, s.nextSeq())).f64(now_);
                send(s, out_, Delivery::Unreliable);
            }
            if (s.lost || s.departed()) continue;
            const double silence = now_ - s.lastHeard;
            if (silence > cfg_.interruptSeconds && !s.interrupted && s.heardAny) {
                s.interrupted = true;
                emit(EventType::Interrupted, s);
            }
            // A joiner gives the members time to drop one of theirs before
            // concluding it cannot reach the session.
            const double limit = member && s.heardAny ? cfg_.disconnectSeconds : cfg_.joinTimeoutSeconds;
            if (silence <= limit) continue;
            if (member) declareLost(s);
            else if (s.member()) return finish(State::Failed, Reason::Unreachable, EventType::JoinFailed);
        }
        askRelays();
        if (member) coordinate();
        retire();
        const Slot* me = local();
        if (state_ == State::Running && me && me->liveFrom == kForever && proposedLive_ != kForever &&
            now_ - lastReady_ >= 1.0)
            sendReady();
        if (resyncPending_ && now_ - resyncAsked_ > cfg_.joinTimeoutSeconds) resyncPending_ = false;
        if (state_ == State::Leaving && me) {
            bool acknowledged = me->liveUntil != kForever;
            for (const auto& p : slots_)
                if (!p->local && p->member() && peerOf(p->id) && p->ackOfMine < me->liveUntil) acknowledged = false;
            if (acknowledged || now_ - leaveStarted_ > cfg_.lingerSeconds) return finish(State::Left, Reason::None);
        }
        updateTimeScale();
        flush();
    }

    // One fixed step. `input` is the local player's input (inputSize bytes)
    // sampled now; it is used `inputDelay` frames from now.
    TickResult tick(const void* input) {
        switch (state_) {
        case State::Idle: return {TickStatus::Idle};
        case State::Synchronizing: return {TickStatus::Synchronizing};
        case State::Joining: return {TickStatus::Joining};
        case State::Left: return {TickStatus::Left};
        case State::Failed: return {TickStatus::Failed};
        default: break;
        }
        updateConfirmed();
        rollback();
        if (state_ == State::Failed) return {TickStatus::Failed};
        settleFinal();
        serveSnapshots();
        if (state_ == State::CatchingUp) {
            for (int n = 0; n < cfg_.maxCatchUpFrames && current_ <= confirmed_; ++n) {
                simulate(current_, Pass::CatchUp);
                ++current_;
                settleFinal();
            }
            if (current_ > confirmed_ && heardAllMembers()) {
                if (cfg_.adaptiveDelay) delay_ = targetDelay();
                proposedLive_ = current_ + delay_ + 1;
                state_ = State::Running;
                sendReady();
            }
            flush();
            return {TickStatus::CatchingUp};
        }
        if (current_ - confirmed_ > cfg_.maxPrediction) {
            ++counters_.stalls;
            ++window_.stalls;
            // A drop or a leave must not wait for the frames it unblocks: it
            // rides on one more frame of the last input.
            Slot* me = local();
            if (!pending_.empty() && me && me->member() && has(*me, me->contiguous)) {
                const uint8_t* last = inputAt(*me, me->contiguous);
                repeat_.assign(last, last + cfg_.inputSize);
                record(repeat_.data(), me->contiguous + 1);
            }
            flush();
            return {TickStatus::Waiting, laggard()};
        }
        // Well ahead of the others (after a hitch on their side, or a faster
        // clock): give one tick back now instead of making them roll back
        // further. The estimate is corrected at once so skips do not pile up.
        if (++sinceSkip_ > cfg_.skipInterval && advantage_ > cfg_.skipAdvantage) {
            sinceSkip_ = 0;
            ++counters_.skips;
            for (auto& p : slots_)
                if (!p->local) p->localAdvantage -= 1.0;
            advantage_ -= 0.5;
            flush();
            return {TickStatus::Skipped};
        }
        record(static_cast<const uint8_t*>(input), current_ + changeDelay(static_cast<const uint8_t*>(input)));
        updateConfirmed();
        simulate(current_, Pass::Live);
        ++current_;
        settleFinal();
        flush();
        return {TickStatus::Advanced};
    }

    // --- Queries -----------------------------------------------------------------

    State state() const { return state_; }
    Frame frame() const { return current_; }             // next frame to simulate
    Frame confirmedFrame() const { return confirmed_; }  // every input known through it
    uint64_t sessionId() const { return cfg_.sessionId; }
    PlayerId localPlayer() const { return local_; }
    uint32_t localIncarnation() const { return cfg_.incarnation; }
    const Config& config() const { return cfg_; }
    // Stretch the fixed step by this factor (TickClock::advance) to stay in
    // step with the other members; within 1 +/- maxTimeAdjust.
    double timeScale() const { return timeScale_; }
    // Mean frames this machine runs ahead of the others (positive: ahead).
    double advantage() const { return advantage_; }
    // How late frame 0 began after the agreed instant (poll granularity):
    // carry it into the host clock, TickClock::reset(startLateness()).
    double startLateness() const { return startLateness_; }

    // The connection indicator: the worst member's line, last second's
    // rollback and stalls. Bars: 5 under 60 ms of round trip (jitter counted
    // twice), 4 under 100, 3 under 150, 2 under 220, else 1; one less over
    // 5% loss, two less over 15%; 0 while a member is silent.
    Quality quality() const {
        Quality q;
        bool silent = false;
        for (const auto& p : slots_) {
            if (p->local || !p->member()) continue;
            q.rtt = std::max(q.rtt, p->rtt);
            q.jitter = std::max(q.jitter, p->jitter);
            q.loss = std::max(q.loss, p->loss());
            silent |= p->interrupted;
        }
        q.rollbackFrames = double(lastWindow_.resimulated);
        q.deepestRollback = lastWindow_.deepest;
        q.stalls = double(lastWindow_.stalls);
        q.inputDelay = delay_;
        q.advantage = advantage_;
        const double line = q.rtt + 2 * q.jitter;
        q.bars = line < 0.06 ? 5 : line < 0.1 ? 4 : line < 0.15 ? 3 : line < 0.22 ? 2 : 1;
        q.bars -= q.loss > 0.15 ? 2 : q.loss > 0.05 ? 1 : 0;
        q.bars = silent ? 0 : std::max(q.bars, 1);
        return q;
    }
    const Counters& counters() const { return counters_; }
    // The state hash this machine computed for a confirmed frame, if kept.
    bool checksum(Frame frame, uint64_t& hash) const {
        for (const auto& c : checksums_)
            if (c.first == frame) return hash = c.second, true;
        return false;
    }
    std::vector<Member> roster() const {
        std::vector<Member> out;
        for (const auto& p : slots_) out.push_back({p->id, p->incarnation, p->liveFrom, p->liveUntil, p->local, p->lost});
        return out;
    }
    std::vector<PeerStats> peers() const {
        std::vector<PeerStats> out;
        for (const auto& p : slots_) {
            const Slot& s = *p;
            if (s.local) continue;
            PeerStats st;
            st.player = s.id;
            st.incarnation = s.incarnation;
            st.bound = peerOf(s.id) != nullptr;
            st.live = s.member();
            st.pending = s.liveFrom == kForever && s.liveUntil == kForever;
            st.lost = s.lost;
            st.interrupted = s.interrupted;
            st.rtt = s.rtt;
            st.jitter = s.jitter;
            st.loss = s.loss();
            st.silence = now_ - s.lastHeard;
            st.advantage = (s.localAdvantage - s.remoteAdvantage) / 2;
            st.remoteFrame = s.remoteFrame;
            st.received = s.contiguous;
            st.acknowledged = s.ackOfMine;
            st.bytesIn = s.bytesIn;
            st.bytesOut = s.bytesOut;
            out.push_back(st);
        }
        return out;
    }
    // Events since the last call.
    std::vector<Event> takeEvents() {
        std::vector<Event> out;
        out.swap(events_);
        return out;
    }

private:
    enum class MessageType : uint8_t {
        Input = 1,
        Ping,
        Pong,
        JoinRequest,
        JoinRefused,
        SnapshotChunk,
        Ready,
        LostReport,
        InputRequest,
        Relay,
        ResyncRequest,
        Start,
        Count
    };
    enum class CommandType : uint8_t { Add = 1, Live, Leave, Drop };
    struct Command {
        CommandType type;
        PlayerId target;
        uint32_t incarnation;
        Frame value;  // Live: first frame; Drop: last frame
    };
    // Orders competing commands about one player: the earliest frame, then
    // the lowest sponsor, wins on every machine.
    struct Key {
        Frame frame = kForever;
        PlayerId sponsor = ~PlayerId(0);
        bool before(const Key& o) const { return frame != o.frame ? frame < o.frame : sponsor < o.sponsor; }
    };

    struct Slot {
        PlayerId id = 0;
        uint32_t incarnation = 0;
        bool local = false;
        Frame liveFrom = kForever, leaveAt = kForever, dropAt = kForever, liveUntil = kForever;
        Key liveKey, dropKey;
        bool announcedLive = false;
        Frame contiguous = kNoFrame;  // every input through this frame is held
        std::vector<uint8_t> inputs, used;
        std::vector<Frame> tags, usedTags;
        std::vector<std::pair<Frame, Command>> commands;

        // Link state (remote slots).
        double lastHeard = 0, lastSent = -1e300, lastPing = -1e300, lastAsk = -1e300, lostAt = 0;
        double budgetStart = -1e300;
        size_t budgetUsed = 0;
        bool heardAny = false, interrupted = false, lost = false, acked = false, dropIssued = false;
        Frame ackOfMine = kNoFrame, sentThrough = kNoFrame, remoteFrame = kNoFrame;
        double rtt = 0, lastRtt = 0, jitter = 0, localAdvantage = 0, remoteAdvantage = 0;
        bool rttKnown = false, advantageKnown = false;
        uint64_t seqOut = 0, seqHigh = 0, seqMask = 0;
        uint64_t windowSeq = 0, windowReceived = 0;  // loss over the current second
        double lossAverage = 0;                      // smoothed over seconds
        int pongs = 0;
        uint64_t bytesIn = 0, bytesOut = 0;
        std::vector<std::pair<Frame, uint64_t>> checksums;  // published by this peer
        std::vector<std::pair<PlayerId, Frame>> reports;    // while lost: who held what

        bool member() const { return liveFrom != kForever && liveUntil == kForever && !lost; }
        bool departed() const { return liveUntil != kForever && contiguous >= liveUntil; }
        bool retiring() const { return liveUntil != kForever && liveFrom > liveUntil; }
        uint64_t nextSeq() { return ++seqOut; }
        bool sequence(uint64_t seq) {
            if (seq > seqHigh) {
                const uint64_t shift = seq - seqHigh;
                seqMask = shift >= 64 ? 0 : seqMask << shift;
                seqMask |= 1;
                seqHigh = seq;
                ++windowReceived;
                return true;
            }
            const uint64_t back = seqHigh - seq;
            if (back >= 64 || (seqMask >> back & 1)) return false;
            seqMask |= uint64_t(1) << back;
            ++windowReceived;
            return true;
        }
        double loss() const { return lossAverage; }
        // Once a second: the share of sequence numbers that never arrived.
        void rollLoss() {
            const uint64_t expected = seqHigh - windowSeq;
            if (expected >= 8) {
                const double lost = 1.0 - std::min(1.0, double(windowReceived) / double(expected));
                lossAverage += (lost - lossAverage) * 0.5;
            }
            windowSeq = seqHigh;
            windowReceived = 0;
        }
    };

    struct Block {
        Frame first = 0;
        size_t count = 0;
        std::vector<uint8_t> data;
        std::vector<std::pair<Frame, Command>> commands;
    };

    struct Admission {
        PlayerId player;
        uint32_t incarnation;
        Frame addFrame;
        bool sent;
    };

    // --- Slots -------------------------------------------------------------------

    Slot* find(PlayerId id, uint32_t incarnation) {
        for (auto& p : slots_)
            if (p->id == id && p->incarnation == incarnation) return p.get();
        return nullptr;
    }
    Slot* find(PlayerId id) {
        for (auto& p : slots_)
            if (p->id == id) return p.get();
        return nullptr;
    }
    Slot* local() {
        for (auto& p : slots_)
            if (p->local) return p.get();
        return nullptr;
    }
    const Slot* local() const { return const_cast<Session*>(this)->local(); }
    bool isMember() const {
        const Slot* me = local();
        return me && me->member();
    }

    // Finds or creates; slots stay sorted by (id, incarnation) at stable addresses.
    Slot& slot(PlayerId id, uint32_t incarnation, bool* created = nullptr) {
        if (Slot* s = find(id, incarnation)) return *s;
        auto s = std::make_unique<Slot>();
        s->id = id;
        s->incarnation = incarnation;
        s->inputs.assign(history_ * cfg_.inputSize, 0);
        s->used.assign(history_ * cfg_.inputSize, 0);
        s->tags.assign(history_, kNoFrame);
        s->usedTags.assign(history_, kNoFrame);
        s->lastHeard = now_;
        auto at = std::lower_bound(slots_.begin(), slots_.end(), s, [](const auto& a, const auto& b) {
            return a->id != b->id ? a->id < b->id : a->incarnation < b->incarnation;
        });
        if (created) *created = true;
        return **slots_.insert(at, std::move(s));
    }

    // The member that writes drops and whose state wins a desync: the lowest
    // id still standing. Deterministic from the roster, never permanent.
    Slot* coordinator() {
        Slot* best = nullptr;
        for (auto& p : slots_)
            if (p->member() && (!best || p->id < best->id)) best = p.get();
        return best;
    }

    const PeerId* peerOf(PlayerId id) const {
        for (const auto& b : bindings_)
            if (b.first == id) return &b.second;
        return nullptr;
    }

    uint8_t* inputAt(Slot& s, Frame f) { return s.inputs.data() + size_t(f) % history_ * cfg_.inputSize; }
    bool has(const Slot& s, Frame f) const { return f >= 0 && s.tags[size_t(f) % history_] == f; }

    // --- Inputs ------------------------------------------------------------------

    void schedule(const Command& c) { pending_.push_back(c); }

    // The local input for frame r (normally current + inputDelay), with any
    // scheduled command.
    void record(const uint8_t* input, Frame r) {
        Slot* me = local();
        if (!me || r > me->liveUntil || r <= me->contiguous) return;
        // A frame skipped over (the start, a raised delay) holds the last input.
        const Frame from = std::max<Frame>(me->contiguous + 1, r - Frame(history_) + 1);
        if (from < r) repeat_.assign(zeros_.begin(), zeros_.end());
        if (from < r && has(*me, from - 1)) repeat_.assign(inputAt(*me, from - 1), inputAt(*me, from - 1) + cfg_.inputSize);
        for (Frame f = from; f < r; ++f) store(*me, f, repeat_.data());
        store(*me, r, input ? input : zeros_.data());
        me->contiguous = r;
        std::vector<Command> commands;
        commands.swap(pending_);
        for (Command c : commands) {
            if (c.type == CommandType::Live) c.value = std::max(c.value, r);
            me->commands.push_back({r, c});
            for (Admission& a : admissions_)
                if (c.type == CommandType::Add && a.player == c.target && a.incarnation == c.incarnation)
                    a.addFrame = r;
            apply(*me, r, c);
        }
        prune(*me);
    }

    void store(Slot& s, Frame f, const uint8_t* data) {
        std::memcpy(inputAt(s, f), data, cfg_.inputSize);
        s.tags[size_t(f) % history_] = f;
    }

    // Commands older than the input history can no longer be sent or relayed.
    void prune(Slot& s) {
        const Frame oldest = s.contiguous - Frame(history_);
        if (s.commands.empty() || s.commands.front().first >= oldest) return;
        s.commands.erase(std::remove_if(s.commands.begin(), s.commands.end(),
                                        [&](const auto& c) { return c.first < oldest; }),
                         s.commands.end());
    }

    // A remote input arrived, directly or relayed.
    void receiveInput(Slot& s, Frame f, const uint8_t* data, const std::vector<std::pair<Frame, Command>>& commands) {
        if (s.local || f <= s.contiguous || has(s, f)) return;
        if (f < 0 || int64_t(f) >= int64_t(confirmed_) + int64_t(history_)) return reject(Reason::OutOfWindow);
        if (f > s.liveUntil) return;
        store(s, f, data);
        const size_t at = size_t(f) % history_;
        if (f < current_ && s.usedTags[at] == f &&
            std::memcmp(s.used.data() + at * cfg_.inputSize, data, cfg_.inputSize) != 0)
            rewind(f);
        for (const auto& c : commands)
            if (c.first == f) {
                s.commands.push_back(c);
                apply(s, f, c.second);
            }
        advance(s);
        prune(s);
    }

    void advance(Slot& s) {
        while (has(s, s.contiguous + 1)) ++s.contiguous;
    }

    // --- Roster commands -----------------------------------------------------------

    void apply(Slot& sponsor, Frame f, const Command& c) {
        const Key key{f, sponsor.id};
        switch (c.type) {
        case CommandType::Add: {
            bool created = false;
            Slot& t = slot(c.target, c.incarnation, &created);
            if (created) emit(EventType::PlayerAdded, t, f);
            break;
        }
        case CommandType::Live: {
            bool created = false;
            Slot& t = slot(c.target, c.incarnation, &created);
            if (created) emit(EventType::PlayerAdded, t, f);
            if (!key.before(t.liveKey)) break;
            const Frame old = t.liveFrom;
            t.liveKey = key;
            t.liveFrom = std::max(c.value, f);
            if (t.local) {
                for (Frame g = t.liveFrom; g <= t.contiguous; ++g)
                    if (!has(t, g)) store(t, g, zeros_.data());
                t.contiguous = std::max(t.contiguous, t.liveFrom - 1);
                emit(EventType::Live, t, t.liveFrom);
            } else {
                t.contiguous = t.liveFrom - 1;
                advance(t);
            }
            if (!t.announcedLive) {
                t.announcedLive = true;
                if (!t.local) emit(EventType::PlayerLive, t, t.liveFrom);
            }
            changed(std::min(old, t.liveFrom));
            break;
        }
        case CommandType::Leave: {
            if (c.target != sponsor.id || c.incarnation != sponsor.incarnation || f >= sponsor.leaveAt) break;
            const Frame old = sponsor.liveUntil;
            sponsor.leaveAt = f;
            settle(sponsor);
            emit(EventType::PlayerLeft, sponsor, sponsor.liveUntil);
            changed(std::min(old, sponsor.liveUntil) + 1);
            break;
        }
        case CommandType::Drop: {
            Slot* t = find(c.target, c.incarnation);
            if (!t || !key.before(t->dropKey)) break;
            const Frame old = t->liveUntil;
            t->dropKey = key;
            t->dropAt = c.value;
            t->lost = true;
            settle(*t);
            emit(EventType::PlayerDropped, *t, t->liveUntil);
            // Frames it never took part in are not changed by its departure.
            if (t->liveFrom != kForever) changed(std::max(t->liveFrom, std::min(old, t->liveUntil) + 1));
            if (t->local) finish(State::Left, Reason::Kicked);
            break;
        }
        }
    }

    void settle(Slot& s) { s.liveUntil = std::min(s.leaveAt, s.dropAt); }

    // The roster changed from frame f on: frames simulated since are wrong.
    void changed(Frame f) {
        if (f < current_) rewind(std::max<Frame>(f, 0));
    }
    void rewind(Frame f) { rollbackTo_ = std::min(rollbackTo_, f); }

    // --- Simulation ----------------------------------------------------------------

    // The last frame through which this slot's part is known.
    Frame knownUntil(const Slot& s) const {
        if (s.liveFrom == kForever || s.liveFrom > s.liveUntil || s.contiguous >= s.liveUntil) return kForever;
        return s.contiguous;
    }

    void updateConfirmed() {
        Frame c = kForever;
        for (const auto& p : slots_) c = std::min(c, knownUntil(*p));
        if (c == kForever) c = current_ - 1;
        if (c > confirmed_) confirmed_ = c;
    }

    PlayerId laggard() const {
        PlayerId who = 0;
        Frame least = kForever;
        for (const auto& p : slots_)
            if (knownUntil(*p) < least) least = knownUntil(*p), who = p->id;
        return who;
    }

    // A state is only kept if it may be returned to: a rollback lands after
    // the last confirmed frame, so a frame whose every input is already known
    // (always, alone; usually, once the delay covers the line) is not saved.
    // Checksum frames are, and so is everything while a snapshot is owed.
    bool saveNeeded(Frame f) const {
        return (slots_.size() > 1 && f > confirmed_) || f % cfg_.checksumInterval == 0 || !admissions_.empty() ||
               !resyncs_.empty();
    }

    void simulate(Frame f, Pass pass, bool save = true) {
        const size_t at = size_t(f) % states_;
        if (save) {
            if (saveNeeded(f)) {
                stateRing_[at].clear();
                sim_.save(f, stateRing_[at]);
                stateTags_[at] = f;
                ++counters_.statesSaved;
            } else {
                stateTags_[at] = kNoFrame;
            }
        }
        players_.clear();
        departed_.clear();
        const size_t ui = size_t(f) % history_;
        for (auto& p : slots_) {
            Slot& s = *p;
            if (s.liveFrom <= f && f <= s.liveUntil) {
                uint8_t* dst = s.used.data() + ui * cfg_.inputSize;
                if (has(s, f))
                    std::memcpy(dst, inputAt(s, f), cfg_.inputSize);
                else if (s.contiguous >= s.liveFrom && has(s, s.contiguous))
                    sim_.predict(s.id, int(f - s.contiguous), inputAt(s, s.contiguous), dst, cfg_.inputSize);
                else
                    std::memcpy(dst, zeros_.data(), cfg_.inputSize);
                s.usedTags[ui] = f;
                players_.push_back({s.id, s.incarnation, f == s.liveFrom, dst});
            } else if (s.liveUntil != kForever && s.liveFrom <= s.liveUntil && s.liveUntil + 1 == f) {
                departed_.push_back({s.id, s.incarnation, s.leaveAt <= s.dropAt ? Departure::Left : Departure::Dropped});
            }
        }
        FrameContext ctx;
        ctx.frame = f;
        ctx.pass = pass;
        ctx.players = players_.data();
        ctx.playerCount = players_.size();
        ctx.departed = departed_.data();
        ctx.departedCount = departed_.size();
        ctx.inputSize = cfg_.inputSize;
        ctx.ledger = &ledger_;
        ledger_.begin(f, pass);
        sim_.advance(ctx);
        ledger_.end([&](Frame frame, uint64_t key) { sim_.retract(frame, key); });
        ++counters_.framesSimulated;
    }

    void rollback() {
        if (rollbackTo_ >= current_) return void(rollbackTo_ = kForever);
        const Frame from = rollbackTo_;
        rollbackTo_ = kForever;
        const size_t at = size_t(from) % states_;
        if (stateTags_[at] != from) {
            // Unreachable while the invariants hold; say so and recover.
            reject(Reason::HistoryExhausted);
            if (!resync()) finish(State::Failed, Reason::HistoryExhausted);
            return;
        }
        sim_.load(from, stateRing_[at].data(), stateRing_[at].size());
        for (Frame f = from; f < current_; ++f) {
            simulate(f, Pass::Resimulate, f != from);
            ++counters_.framesResimulated;
        }
        ++counters_.rollbacks;
        counters_.deepestRollback = std::max(counters_.deepestRollback, current_ - from);
        window_.resimulated += uint64_t(current_ - from);
        window_.deepest = std::max(window_.deepest, current_ - from);
    }

    // Commits and checksums of the frames that became final.
    void settleFinal() {
        const Frame finalThrough = std::min(confirmed_, current_ - 1);
        for (Frame f = committed_ + 1; f <= finalThrough; ++f) {
            ledger_.flush(f, [&](uint64_t key, const uint8_t* data, size_t size) { sim_.commit(f, key, data, size); });
            if (replay_) recordReplay(f);
            committed_ = f;
        }
        // The state at frame f is final once every input before f is known.
        const Frame bound = std::min(confirmed_ + 1, current_ - 1);
        for (Frame f = checksummed_ + 1; f <= bound; ++f) {
            const size_t at = size_t(f) % states_;
            if (f % cfg_.checksumInterval == 0 && stateTags_[at] == f) {
                const uint64_t h = sim_.checksum(stateRing_[at].data(), stateRing_[at].size());
                checksums_.push_back({f, h});
                if (checksums_.size() > 16) checksums_.erase(checksums_.begin());
                if (replay_ && replay_->started() && f > replay_->first()) replay_->checksum(f, h);
                for (auto& p : slots_)
                    for (const auto& c : p->checksums)
                        if (c.first == f && c.second != h) desync(*p, f, h, c.second);
            }
            checksummed_ = f;
        }
    }

    // A final frame into the replay, as it was last simulated. Recording
    // starts at a checksum frame, whose state is always saved.
    void recordReplay(Frame f) {
        const size_t at = size_t(f) % states_;
        if (!replay_->started()) {
            if (f % cfg_.checksumInterval != 0 || stateTags_[at] != f) return;
            replay_->begin(cfg_.inputSize, f, stateRing_[at].data(), stateRing_[at].size());
        }
        if (replay_->end() != f) return;
        players_.clear();
        departed_.clear();
        const size_t ui = size_t(f) % history_;
        for (auto& p : slots_) {
            const Slot& s = *p;
            if (s.liveFrom <= f && f <= s.liveUntil && s.usedTags[ui] == f)
                players_.push_back({s.id, s.incarnation, f == s.liveFrom, s.used.data() + ui * cfg_.inputSize});
            else if (s.liveUntil != kForever && s.liveFrom <= s.liveUntil && s.liveUntil + 1 == f)
                departed_.push_back({s.id, s.incarnation, s.leaveAt <= s.dropAt ? Departure::Left : Departure::Dropped});
        }
        replay_->frame(players_.data(), players_.size(), departed_.data(), departed_.size());
    }

    void desync(Slot& s, Frame f, uint64_t mine, uint64_t theirs) {
        ++counters_.desyncs;
        events_.push_back({EventType::Desync, s.id, s.incarnation, f, Reason::None, mine, theirs});
        if (cfg_.autoResync && coordinator() == &s && !resyncPending_) resync();
    }

    // --- Messages ------------------------------------------------------------------

    std::vector<uint8_t>& message(MessageType type, uint64_t seq) {
        out_.clear();
        Writer w(out_);
        w.u8('S'), w.u8('N'), w.u8(kProtocolVersion), w.u8(uint8_t(type));
        w.var(cfg_.sessionId);
        w.var(local_);
        w.var(cfg_.incarnation);
        w.var(seq);
        return out_;
    }

    void sendTo(PlayerId id, const std::vector<uint8_t>& m, Delivery d) {
        if (const PeerId* peer = peerOf(id)) {
            net_.send(*peer, m.data(), m.size(), d);
            ++counters_.messagesSent;
            counters_.bytesSent += m.size();
        }
    }
    void send(Slot& s, const std::vector<uint8_t>& m, Delivery d) {
        if (peerOf(s.id)) s.bytesOut += m.size();
        sendTo(s.id, m, d);
    }

    void writeBlock(Writer& w, Slot& s, Frame first, size_t count) {
        w.svar(first);
        w.var(count);
        scratch_.resize(count * cfg_.inputSize);
        for (size_t i = 0; i < count; ++i)
            std::memcpy(scratch_.data() + i * cfg_.inputSize, inputAt(s, first + Frame(i)), cfg_.inputSize);
        encodeInputs(w, scratch_.data(), count, cfg_.inputSize);
        const Frame end = first + Frame(count);
        size_t n = 0;
        for (const auto& c : s.commands) n += c.first >= first && c.first < end;
        w.var(n);
        for (const auto& c : s.commands)
            if (c.first >= first && c.first < end) {
                w.var(uint64_t(c.first - first));
                w.u8(uint8_t(c.second.type));
                w.var(c.second.target);
                w.var(c.second.incarnation);
                w.svar(c.second.value);
            }
    }

    bool readBlock(Reader& r, Block& b) {
        b.first = r.frame();
        b.count = size_t(r.var());
        if (!r.ok() || b.first < 0 || b.count > kMaxFramesPerBlock) return false;
        b.data.resize(b.count * cfg_.inputSize);
        if (b.count && !decodeInputs(r, b.data.data(), b.count, cfg_.inputSize)) return false;
        const uint64_t n = r.var();
        if (!r.ok() || n > b.count * kMaxCommandsPerFrame) return false;
        b.commands.clear();
        for (uint64_t i = 0; i < n; ++i) {
            const uint64_t offset = r.var();
            const uint8_t type = r.u8();
            const PlayerId target = r.var();
            const uint64_t incarnation = r.var();
            const Frame value = r.frame();
            if (!r.ok() || offset >= b.count || type < 1 || type > 4 || incarnation > 0xffffffffULL) return false;
            b.commands.push_back({b.first + Frame(offset), {CommandType(type), target, uint32_t(incarnation), value}});
        }
        return true;
    }

    void storeBlock(Slot& s, const Block& b) {
        for (size_t i = 0; i < b.count; ++i)
            receiveInput(s, b.first + Frame(i), b.data.data() + i * cfg_.inputSize, b.commands);
    }

    // The first local frame others need: the local player's live frame, or
    // the frame it proposed while joining.
    Frame firstLocalFrame() const {
        const Slot* me = local();
        if (!me) return kForever;
        return me->liveFrom != kForever ? me->liveFrom : proposedLive_;
    }

    void flush() {
        for (auto& p : slots_)
            if (!p->local && !p->lost && !p->retiring() && peerOf(p->id)) sendInputs(*p);
    }

    // Every local input the peer has not acknowledged (oldest first, as many
    // as fit), its acknowledgement of ours, our frame and the latest hashes.
    void sendInputs(Slot& s) {
        Slot* me = local();
        if (!me) return;
        const Frame first = firstLocalFrame();
        const Frame to = std::min(me->contiguous, me->liveUntil);
        const Frame from = std::max<Frame>(first, s.ackOfMine + 1);
        size_t count = 0;
        if (s.acked && first != kForever && from <= to) {
            if (int64_t(to) - from >= int64_t(history_) - 1) {
                reject(Reason::HistoryExhausted);
                if (isMember()) declareLost(s);
                return;
            }
            count = std::min<size_t>(size_t(to - from + 1), kMaxFramesPerBlock);
        }
        const bool fresh = count && from + Frame(count) - 1 > s.sentThrough;
        const bool resend = count && now_ - s.lastSent >= cfg_.resendSeconds;
        if (!fresh && !resend && now_ - s.lastSent < cfg_.heartbeatSeconds) return;
        for (;;) {
            Writer w(message(MessageType::Input, s.nextSeq()));
            w.svar(current_);
            w.svar(int64_t(std::lround(std::clamp(s.localAdvantage, -1000.0, 1000.0) * 16)));
            w.svar(s.contiguous);
            writeBlock(w, *me, from, count);
            const size_t n = std::min<size_t>(checksums_.size(), 2);
            w.var(n);
            for (size_t i = checksums_.size() - n; i < checksums_.size(); ++i) {
                w.svar(checksums_[i].first);
                w.u64(checksums_[i].second);
            }
            if (out_.size() > cfg_.maxMessageBytes && count > 1) {
                count /= 2;
                continue;
            }
            send(s, out_, Delivery::Unreliable);
            break;
        }
        s.lastSent = now_;
        if (count) s.sentThrough = std::max(s.sentThrough, from + Frame(count) - 1);
    }

    void onInput(Reader& r, Slot& s) {
        const Frame remoteFrame = r.frame();
        const double remoteAdvantage = double(r.svar()) / 16;
        const Frame ack = r.frame();
        if (!r.ok() || !readBlock(r, block_)) return reject(Reason::Malformed);
        const uint64_t n = r.var();
        if (!r.ok() || n > 4) return reject(Reason::Malformed);
        std::pair<Frame, uint64_t> sums[4];
        for (uint64_t i = 0; i < n; ++i) sums[i].first = r.frame(), sums[i].second = r.u64();
        if (!r.ok()) return reject(Reason::Malformed);

        if (ack > s.ackOfMine) s.ackOfMine = ack;
        s.acked = true;
        s.remoteFrame = remoteFrame;
        s.remoteAdvantage = remoteAdvantage;
        const double estimate = remoteFrame + (s.rttKnown ? s.rtt / 2 / cfg_.tickSeconds : 0.0);
        const double sample = double(current_) - estimate;
        s.localAdvantage = s.advantageKnown ? s.localAdvantage + (sample - s.localAdvantage) * 0.1 : sample;
        s.advantageKnown = true;
        storeBlock(s, block_);
        for (uint64_t i = 0; i < n; ++i) {
            bool seen = false;
            for (const auto& c : s.checksums) seen |= c.first == sums[i].first;
            if (seen) continue;
            s.checksums.push_back(sums[i]);
            if (s.checksums.size() > 8) s.checksums.erase(s.checksums.begin());
            for (const auto& own : checksums_)
                if (own.first == sums[i].first && own.second != sums[i].second)
                    desync(s, own.first, own.second, sums[i].second);
        }
    }

    void onPing(Reader& r, Slot& s) {
        const double t = r.f64();
        if (!r.ok()) return reject(Reason::Malformed);
        Writer(message(MessageType::Pong, s.nextSeq())).f64(t);
        send(s, out_, Delivery::Unreliable);
    }

    void onPong(Reader& r, Slot& s) {
        const double t = r.f64();
        if (!r.ok() || !std::isfinite(t)) return reject(Reason::Malformed);
        const double sample = now_ - t;
        if (sample < 0 || sample > 30) return;
        // Jitter as RFC 3550 measures it, between successive round trips: a
        // line that changes latency once is not mistaken for a jittery one.
        if (!s.rttKnown) s.rtt = s.lastRtt = sample, s.jitter = 0, s.rttKnown = true;
        s.jitter += (std::fabs(sample - s.lastRtt) - s.jitter) / 16;
        s.rtt += (sample - s.rtt) * 0.125;
        s.lastRtt = sample;
        ++s.pongs;
    }

    // --- Admission --------------------------------------------------------------------

    void onJoinRequest(Reader& r, PlayerId sender, uint32_t incarnation) {
        const uint16_t inputSize = r.u16();
        if (!r.ok()) return reject(Reason::Malformed);
        if (find(sender, incarnation)) return;  // admitted: the snapshot is on its way
        if (state_ != State::Running || !isMember()) return sendRefusal(sender, Reason::NotReady);
        if (inputSize != cfg_.inputSize) return sendRefusal(sender, Reason::InputSizeMismatch);
        for (const Admission& a : admissions_)
            if (a.player == sender && a.incarnation == incarnation) return;
        int present = 0;
        for (const auto& p : slots_) present += p->liveUntil == kForever && !p->lost;
        for (const Admission& a : admissions_) present += a.addFrame == kNoFrame;
        if (present >= cfg_.maxPlayers) return sendRefusal(sender, Reason::Full);
        if (cfg_.autoAdmit) return void(admitNow(sender, incarnation));
        for (const auto& q : requests_)
            if (q.first == sender && q.second == incarnation) return;
        requests_.push_back({sender, incarnation});
        events_.push_back({EventType::JoinRequested, sender, incarnation});
    }

    bool admitNow(PlayerId player, uint32_t incarnation) {
        if (state_ != State::Running || !isMember()) return false;
        admissions_.push_back({player, incarnation, kNoFrame, false});
        schedule({CommandType::Add, player, incarnation, 0});
        return true;
    }

    void sendRefusal(PlayerId to, Reason reason) {
        Writer(message(MessageType::JoinRefused, 0)).u8(uint8_t(reason));
        sendTo(to, out_, Delivery::Reliable);
    }

    void pollJoin() {
        if (now_ - joinStarted_ > cfg_.joinTimeoutSeconds)
            return finish(State::Failed, Reason::Timeout, EventType::JoinFailed);
        if (now_ - lastJoinAsk_ >= 1.0 && peerOf(sponsor_)) {
            lastJoinAsk_ = now_;
            Writer(message(MessageType::JoinRequest, 0)).u16(cfg_.inputSize);
            sendTo(sponsor_, out_, Delivery::Reliable);
        }
    }

    // A confirmed snapshot for each admitted player whose Add is confirmed,
    // and for each member asking to resynchronize.
    void serveSnapshots() {
        if (admissions_.empty() && resyncs_.empty()) return;
        const Frame s = std::min(confirmed_ + 1, current_ - 1);
        if (s < 0 || stateTags_[size_t(s) % states_] != s) return;
        for (Admission& a : admissions_) {
            if (a.addFrame == kNoFrame || confirmed_ < a.addFrame || !peerOf(a.player)) continue;
            sendSnapshot(a.player, s, false);
            a.sent = true;
        }
        admissions_.erase(std::remove_if(admissions_.begin(), admissions_.end(), [](const Admission& a) { return a.sent; }),
                          admissions_.end());
        for (PlayerId id : resyncs_) sendSnapshot(id, s, true);
        resyncs_.clear();
    }

    void sendSnapshot(PlayerId to, Frame s, bool resync) {
        snapshot_.clear();
        Writer w(snapshot_);
        w.var(cfg_.sessionId);
        w.svar(s);
        w.u16(cfg_.inputSize);
        w.u8(resync);
        w.var(slots_.size());
        for (auto& p : slots_) {
            Slot& t = *p;
            w.var(t.id);
            w.var(t.incarnation);
            w.svar(t.liveFrom);
            w.svar(t.leaveAt);
            w.svar(t.dropAt);
            w.svar(t.liveKey.frame);
            w.var(t.liveKey.sponsor);
            w.svar(t.dropKey.frame);
            w.var(t.dropKey.sponsor);
            w.u8(t.lost);
            const bool base = has(t, s - 1);
            w.u8(base);
            if (base) w.bytes(inputAt(t, s - 1), cfg_.inputSize);
        }
        const auto& state = stateRing_[size_t(s) % states_];
        w.var(state.size());
        w.bytes(state.data(), state.size());

        const size_t chunk = cfg_.maxMessageBytes - 64;
        const size_t total = (snapshot_.size() + chunk - 1) / chunk;
        const uint64_t id = ++snapshotSeq_;
        for (size_t i = 0; i < total; ++i) {
            Writer c(message(MessageType::SnapshotChunk, 0));
            c.var(id);
            c.var(i);
            c.var(total);
            c.var(snapshot_.size());
            const size_t at = i * chunk;
            c.bytes(snapshot_.data() + at, std::min(chunk, snapshot_.size() - at));
            sendTo(to, out_, Delivery::Reliable);
        }
        ++counters_.snapshotsSent;
    }

    // Chunks arrive in order (reliable delivery); a new id restarts assembly.
    void onChunk(Reader& r) {
        const uint64_t id = r.var(), index = r.var(), total = r.var(), bytes = r.var();
        if (!r.ok() || total == 0 || index >= total) return reject(Reason::Malformed);
        if (bytes > cfg_.maxSnapshotBytes) {
            reject(Reason::SnapshotTooLarge);
            if (state_ == State::Joining) finish(State::Failed, Reason::SnapshotTooLarge, EventType::JoinFailed);
            return;
        }
        if (index == 0) {
            assemblyId_ = id;
            assembly_.clear();
            assemblyNext_ = 0;
        }
        if (id != assemblyId_ || index != assemblyNext_) return reject(Reason::Malformed);
        const size_t n = r.left();
        const uint8_t* data = r.bytes(n);
        if (n) assembly_.insert(assembly_.end(), data, data + n);
        if (assembly_.size() > bytes) return reject(Reason::Malformed);
        if (++assemblyNext_ < total) return;
        if (assembly_.size() != bytes) return reject(Reason::Malformed);
        applySnapshot();
    }

    void applySnapshot() {
        Reader r(assembly_.data(), assembly_.size());
        const uint64_t session = r.var();
        const Frame s = r.frame();
        const uint16_t inputSize = r.u16();
        const bool resync = r.u8() != 0;
        const uint64_t count = r.var();
        if (!r.ok() || s < 0 || count > uint64_t(cfg_.maxPlayers) * 4 + 16) return reject(Reason::Malformed);
        if (inputSize != cfg_.inputSize) {
            reject(Reason::InputSizeMismatch);
            if (state_ == State::Joining) finish(State::Failed, Reason::InputSizeMismatch, EventType::JoinFailed);
            return;
        }
        struct Entry {
            PlayerId id;
            uint32_t incarnation;
            Frame liveFrom, leaveAt, dropAt;
            Key liveKey, dropKey;
            bool lost;
            const uint8_t* input;
        };
        std::vector<Entry> entries;
        for (uint64_t i = 0; i < count; ++i) {
            Entry e{};
            e.id = r.var();
            const uint64_t inc = r.var();
            e.incarnation = uint32_t(inc);
            e.liveFrom = r.frame(), e.leaveAt = r.frame(), e.dropAt = r.frame();
            e.liveKey.frame = r.frame(), e.liveKey.sponsor = r.var();
            e.dropKey.frame = r.frame(), e.dropKey.sponsor = r.var();
            e.lost = r.u8() != 0;
            e.input = r.u8() ? r.bytes(cfg_.inputSize) : nullptr;
            if (!r.ok() || inc > 0xffffffffULL) return reject(Reason::Malformed);
            entries.push_back(e);
        }
        const uint64_t stateSize = r.var();
        const uint8_t* state = r.bytes(size_t(stateSize));
        if (!r.ok() || !r.done()) return reject(Reason::Malformed);

        if (resync) return applyResync(session, s, state, size_t(stateSize));
        if (state_ != State::Joining) return;
        bool foundSelf = false;
        for (const Entry& e : entries) foundSelf |= e.id == local_ && e.incarnation == cfg_.incarnation;
        if (!foundSelf) {
            reject(Reason::Malformed);
            return finish(State::Failed, Reason::Malformed, EventType::JoinFailed);
        }
        cfg_.sessionId = session;
        slots_.clear();
        for (const Entry& e : entries) {
            Slot& t = slot(e.id, e.incarnation);
            t.liveFrom = e.liveFrom;
            t.leaveAt = e.leaveAt;
            t.dropAt = e.dropAt;
            t.liveKey = e.liveKey;
            t.dropKey = e.dropKey;
            t.lost = e.lost;
            t.announcedLive = t.liveFrom != kForever;
            settle(t);
            t.local = e.id == local_ && e.incarnation == cfg_.incarnation;
            if (t.local) {
                t.contiguous = s + delay_ - 1;  // frames before going live are never used
            } else {
                t.contiguous = t.liveFrom == kForever ? kNoFrame : std::max(s - 1, t.liveFrom - 1);
                if (e.input && s >= 1) store(t, s - 1, e.input);
            }
        }
        const size_t at = size_t(s) % states_;
        stateRing_[at].assign(state, state + stateSize);
        stateTags_[at] = s;
        sim_.load(s, state, size_t(stateSize));
        current_ = s;
        confirmed_ = committed_ = checksummed_ = s - 1;
        state_ = State::CatchingUp;
        events_.push_back({EventType::Joined, local_, cfg_.incarnation, s});
    }

    // The coordinator's confirmed state replaces ours at frame s; the frames
    // since are simulated again from it. Nothing already committed is
    // committed again.
    void applyResync(uint64_t session, Frame s, const uint8_t* state, size_t size) {
        if (session != cfg_.sessionId || !resyncPending_) return;
        if (int64_t(current_) - s >= int64_t(history_) - 1) {
            reject(Reason::HistoryExhausted);
            return finish(State::Failed, Reason::HistoryExhausted);
        }
        resyncPending_ = false;
        const size_t at = size_t(s) % states_;
        stateRing_[at].assign(state, state + size);
        stateTags_[at] = s;
        checksums_.erase(std::remove_if(checksums_.begin(), checksums_.end(), [&](const auto& c) { return c.first >= s; }),
                         checksums_.end());
        for (auto& p : slots_)
            p->checksums.erase(std::remove_if(p->checksums.begin(), p->checksums.end(),
                                              [&](const auto& c) { return c.first >= s; }),
                               p->checksums.end());
        checksummed_ = std::min(checksummed_, s - 1);
        if (s >= current_) {
            sim_.load(s, state, size);
            current_ = s;
        } else {
            rewind(s);
        }
        ++counters_.resyncs;
        if (replay_) *replay_ = Replay();  // what was recorded was the diverged timeline
        events_.push_back({EventType::Resynced, local_, cfg_.incarnation, s});
    }

    bool heardAllMembers() const {
        for (const auto& p : slots_)
            if (!p->local && p->member() && (!p->heardAny || !p->acked)) return false;
        return true;
    }

    // To the sponsor, or to any member standing if the sponsor is gone.
    void sendReady() {
        lastReady_ = now_;
        Writer(message(MessageType::Ready, 0)).svar(proposedLive_);
        Slot* to = find(sponsor_);
        if (to && !(to->member() && peerOf(to->id))) to = nullptr;
        for (auto& p : slots_)
            if (!to && !p->local && p->member() && peerOf(p->id)) to = p.get();
        if (to) send(*to, out_, Delivery::Reliable);
    }

    void onReady(Reader& r, Slot& s) {
        const Frame proposed = r.frame();
        if (!r.ok()) return reject(Reason::Malformed);
        if (!isMember() || s.liveFrom != kForever || s.liveUntil != kForever) return;
        for (const Command& c : pending_)
            if (c.type == CommandType::Live && c.target == s.id && c.incarnation == s.incarnation) return;
        schedule({CommandType::Live, s.id, s.incarnation, proposed});
    }

    // --- Loss ---------------------------------------------------------------------------

    void declareLost(Slot& s) {
        if (s.lost) return;
        s.lost = true;
        s.lostAt = now_;
        emit(EventType::PlayerLost, s, s.contiguous);
        report(s, local_, s.contiguous);
        Writer w(message(MessageType::LostReport, 0));
        w.var(s.id);
        w.var(s.incarnation);
        w.svar(s.contiguous);
        for (auto& p : slots_)
            if (!p->local && !p->lost) send(*p, out_, Delivery::Reliable);
    }

    void report(Slot& lost, PlayerId reporter, Frame held) {
        for (auto& rep : lost.reports)
            if (rep.first == reporter) return void(rep.second = held);
        lost.reports.push_back({reporter, held});
    }

    void onLostReport(Reader& r, Slot& from) {
        const PlayerId id = r.var();
        const uint64_t inc = r.var();
        const Frame held = r.frame();
        if (!r.ok() || inc > 0xffffffffULL) return reject(Reason::Malformed);
        Slot* lost = find(id, uint32_t(inc));
        if (!lost || lost->local) return;
        report(*lost, from.id, held);
        if (lost->lost) return;
        if (isMember()) {
            declareLost(*lost);
        } else {
            lost->lost = true;
            lost->lostAt = now_;
        }
    }

    // The coordinator writes the drop once every member standing has said
    // which frame it last held: the drop frame is the latest of them, so no
    // member ever confirmed a frame the drop would rewrite. A member that
    // does not report in time is lost too.
    void coordinate() {
        const Slot* c = coordinator();
        if (!c || !c->local) return;
        for (auto& p : slots_) {
            Slot& lost = *p;
            if (!lost.lost || lost.dropIssued || lost.dropKey.frame != kForever || lost.departed()) continue;
            bool complete = true;
            for (auto& q : slots_) {
                Slot& m = *q;
                if (!m.member() || m.local) continue;
                bool reported = false;
                for (const auto& rep : lost.reports) reported |= rep.first == m.id;
                if (reported) continue;
                complete = false;
                if (now_ - lost.lostAt > cfg_.disconnectSeconds) declareLost(m);
            }
            if (!complete) continue;
            Frame last = kNoFrame;
            for (const auto& rep : lost.reports) last = std::max(last, rep.second);
            lost.dropIssued = true;
            const Frame until = lost.liveFrom == kForever ? kNoFrame : std::max(last, lost.liveFrom - 1);
            schedule({CommandType::Drop, lost.id, lost.incarnation, until});
        }
    }

    // Input of a member gone quiet, or the frames a lost one sent before it
    // went: asked from everyone else, who answers with what it holds.
    void askRelays() {
        for (auto& p : slots_) {
            Slot& s = *p;
            if (s.local || knownUntil(s) == kForever) continue;
            if (!s.lost && now_ - s.lastHeard < cfg_.relayAfterSeconds) continue;
            if (now_ - s.lastAsk < cfg_.relayAfterSeconds) continue;
            s.lastAsk = now_;
            const Frame from = s.contiguous + 1;
            const Frame to = std::min<Frame>(s.liveUntil, from + Frame(kMaxFramesPerBlock) - 1);
            for (auto& q : slots_) {
                Slot& t = *q;
                if (t.local || t.lost || &t == &s || !peerOf(t.id)) continue;
                Writer w(message(MessageType::InputRequest, t.nextSeq()));
                w.var(s.id);
                w.var(s.incarnation);
                w.svar(from);
                w.svar(to);
                send(t, out_, Delivery::Unreliable);
            }
        }
    }

    void onInputRequest(Reader& r, Slot& asker) {
        const PlayerId id = r.var();
        const uint64_t inc = r.var();
        const Frame from = r.frame(), to = r.frame();
        if (!r.ok() || inc > 0xffffffffULL || from < 0) return reject(Reason::Malformed);
        Slot* s = find(id, uint32_t(inc));
        if (!s || !has(*s, from) || from > s->contiguous) return;
        const Frame last = std::min({to, s->contiguous, from + Frame(kMaxFramesPerBlock) - 1});
        size_t count = size_t(std::max<Frame>(last - from + 1, 1));
        for (;;) {
            Writer w(message(MessageType::Relay, asker.nextSeq()));
            w.var(s->id);
            w.var(s->incarnation);
            writeBlock(w, *s, from, count);
            if (out_.size() > cfg_.maxMessageBytes && count > 1) {
                count /= 2;
                continue;
            }
            send(asker, out_, Delivery::Unreliable);
            ++counters_.relaysSent;
            break;
        }
    }

    void onRelay(Reader& r) {
        const PlayerId id = r.var();
        const uint64_t inc = r.var();
        if (!r.ok() || inc > 0xffffffffULL || !readBlock(r, block_)) return reject(Reason::Malformed);
        Slot* s = find(id, uint32_t(inc));
        if (s && !s->local) storeBlock(*s, block_);
    }

    void onResyncRequest(Slot& s) {
        const Slot* c = coordinator();
        if (!c || !c->local || !s.member()) return;
        if (std::find(resyncs_.begin(), resyncs_.end(), s.id) == resyncs_.end()) resyncs_.push_back(s.id);
    }

    // Players gone for good: no frame that may still be simulated sees them.
    void retire() {
        const Frame safe = std::min(committed_, current_ - 1);
        for (size_t i = 0; i < slots_.size();) {
            const Slot& s = *slots_[i];
            const bool gone = !s.local && s.liveUntil != kForever && (s.liveFrom > s.liveUntil || s.liveUntil + 1 < safe);
            if (!gone) {
                ++i;
                continue;
            }
            events_.push_back({EventType::PlayerRemoved, s.id, s.incarnation, s.liveUntil});
            slots_.erase(slots_.begin() + long(i));
        }
    }

    // --- Synchronized start ---------------------------------------------------------

    // Founders ping each other quickly; the coordinator, once it has
    // syncSamples round trips to everyone (or after syncSeconds), sends each
    // founder how long to wait so that all begin together: its own lead minus
    // that founder's one-way time.
    void pollSync() {
        for (auto& p : slots_) {
            Slot& s = *p;
            if (s.local || !peerOf(s.id) || now_ - s.lastPing < 0.05) continue;
            s.lastPing = now_;
            Writer(message(MessageType::Ping, s.nextSeq())).f64(now_);
            send(s, out_, Delivery::Unreliable);
        }
        if (startAt_ < 0) {
            const Slot* c = coordinator();
            if (c && c->local) {
                bool measured = true;
                double lead = 0;
                for (const auto& p : slots_)
                    if (!p->local) {
                        measured &= p->pongs >= cfg_.syncSamples;
                        if (p->rttKnown) lead = std::max(lead, p->rtt / 2);
                    }
                if (measured || now_ - syncStarted_ > cfg_.syncSeconds) {
                    lead += cfg_.tickSeconds;
                    for (auto& p : slots_) {
                        if (p->local) continue;
                        Writer(message(MessageType::Start, 0)).f64(lead - (p->rttKnown ? p->rtt / 2 : 0));
                        send(*p, out_, Delivery::Reliable);
                    }
                    startAt_ = now_ + lead;
                }
            } else if (now_ - syncStarted_ > cfg_.syncSeconds + cfg_.joinTimeoutSeconds) {
                startAt_ = now_;  // no word from the coordinator: play, and let loss detection decide
            }
        }
        if (startAt_ >= 0 && now_ >= startAt_) begin();
    }

    void onStart(Reader& r) {
        const double wait = r.f64();
        if (!r.ok() || !std::isfinite(wait) || wait < 0 || wait > 10) return reject(Reason::Malformed);
        if (state_ == State::Synchronizing && startAt_ < 0) startAt_ = now_ + wait;
    }

    void begin() {
        state_ = State::Running;
        startLateness_ = now_ - startAt_;
        if (cfg_.adaptiveDelay) delay_ = targetDelay();
        for (auto& p : slots_) p->lastHeard = now_;
        windowStart_ = now_;
        events_.push_back({EventType::Started, local_, cfg_.incarnation, 0, Reason::None, uint64_t(delay_)});
    }

    // --- Input delay --------------------------------------------------------------------

    // The fewest frames of delay that keep rollbacks within rollbackComfort
    // frames for the farthest member: its one-way time plus twice its jitter.
    int targetDelay() const {
        double worst = -1;
        for (const auto& p : slots_)
            if (!p->local && p->member() && p->rttKnown) worst = std::max(worst, p->rtt / 2 + 2 * p->jitter);
        if (worst < 0) return delay_;
        const int frames = int(std::ceil(worst / cfg_.tickSeconds - 1e-9));
        return std::clamp(frames - cfg_.rollbackComfort, cfg_.minInputDelay, cfg_.maxInputDelay);
    }

    // Each second, the delay heads for the highest target seen during it:
    // it does not flicker with every ping, and a bad moment is covered.
    void adaptDelay() {
        if (!cfg_.adaptiveDelay || !isMember()) return;
        candidateDelay_ = std::max(candidateDelay_, targetDelay());
        if (now_ - candidateSince_ < 1.0) return;
        if (candidateDelay_ != wantedDelay_) wantedDelay_ = candidateDelay_, wantedSince_ = now_;
        candidateDelay_ = -1;
        candidateSince_ = now_;
    }

    // One frame toward the wanted delay, at a tick that loses nothing:
    // raising holds the last input over the frame skipped; lowering waits for
    // an input equal to the last one sent, which then needs no frame of its
    // own (or gives up one sample after half a second of constant change).
    int changeDelay(const uint8_t* input) {
        if (wantedDelay_ < 0 || wantedDelay_ == delay_) return delay_;
        const Slot* me = local();
        if (!me || !me->member()) return delay_;
        int next = delay_;
        if (wantedDelay_ > delay_) {
            next = delay_ + 1;
        } else {
            Slot& m = *local();
            const bool same = input && has(m, m.contiguous) && std::memcmp(input, inputAt(m, m.contiguous), cfg_.inputSize) == 0;
            if (same || now_ - wantedSince_ > 0.5) next = delay_ - 1;
        }
        if (next != delay_) {
            delay_ = next;
            ++counters_.delayChanges;
            events_.push_back({EventType::InputDelay, local_, cfg_.incarnation, current_, Reason::None, uint64_t(delay_)});
        }
        return delay_;
    }

    void rollWindow() {
        if (now_ - windowStart_ < 1.0) return;
        lastWindow_ = window_;
        window_ = {};
        windowStart_ = now_;
        for (auto& p : slots_) p->rollLoss();
    }

    void updateTimeScale() {
        double sum = 0;
        int n = 0;
        for (const auto& p : slots_)
            if (!p->local && p->member() && p->advantageKnown && now_ - p->lastHeard < cfg_.interruptSeconds) {
                sum += (p->localAdvantage - p->remoteAdvantage) / 2;
                ++n;
            }
        advantage_ = n ? sum / n : 0.0;
        timeScale_ = state_ == State::Running || state_ == State::Leaving
                         ? 1.0 - std::clamp(advantage_ * 0.025, -cfg_.maxTimeAdjust, cfg_.maxTimeAdjust)
                         : 1.0;
    }

    // --- Events -------------------------------------------------------------------------

    void emit(EventType type, const Slot& s, Frame f = kNoFrame) {
        events_.push_back({type, s.id, s.incarnation, f});
    }

    void reject(Reason reason) {
        ++counters_.rejected[size_t(reason)];
        double& last = lastReject_[size_t(reason)];
        if (now_ - last < 1.0) return;
        last = now_;
        events_.push_back({EventType::Rejected, 0, 0, current_, reason});
    }

    void finish(State state, Reason reason, EventType type = EventType::Left) {
        state_ = state;
        events_.push_back({type, local_, cfg_.incarnation, current_, reason});
        if (type != EventType::Left) events_.push_back({EventType::Left, local_, cfg_.incarnation, current_, reason});
    }

    Config cfg_;
    PlayerId local_;
    Simulation& sim_;
    Transport& net_;
    State state_ = State::Idle;
    size_t history_ = 0, states_ = 0;

    std::vector<std::unique_ptr<Slot>> slots_;  // stable: commands create slots mid-loop
    std::vector<std::pair<PlayerId, PeerId>> bindings_;
    std::vector<Command> pending_;
    std::vector<Admission> admissions_;
    std::vector<std::pair<PlayerId, uint32_t>> requests_;
    std::vector<PlayerId> resyncs_;

    Frame current_ = 0, confirmed_ = kNoFrame, committed_ = kNoFrame, checksummed_ = kNoFrame;
    Frame rollbackTo_ = kForever, proposedLive_ = kForever;
    std::vector<std::vector<uint8_t>> stateRing_;
    std::vector<Frame> stateTags_;
    std::vector<std::pair<Frame, uint64_t>> checksums_;
    Ledger ledger_;

    PlayerId sponsor_ = 0;
    double now_ = 0, joinStarted_ = 0, lastJoinAsk_ = -1e300, lastReady_ = -1e300, leaveStarted_ = 0;
    double resyncAsked_ = 0, timeScale_ = 1.0, advantage_ = 0.0;
    double syncStarted_ = 0, startAt_ = -1.0, startLateness_ = 0;
    int delay_ = 0, wantedDelay_ = -1, candidateDelay_ = -1, sinceSkip_ = 0;
    double wantedSince_ = 0, candidateSince_ = 0;
    struct Window {
        uint64_t resimulated = 0, stalls = 0;
        Frame deepest = 0;
    } window_, lastWindow_;
    double windowStart_ = 0;
    Replay* replay_ = nullptr;
    bool resyncPending_ = false;
    uint64_t snapshotSeq_ = 0, assemblyId_ = 0, assemblyNext_ = 0;
    std::vector<uint8_t> assembly_, snapshot_;

    std::vector<Event> events_;
    Counters counters_;
    double lastReject_[size_t(Reason::Count)];

    std::vector<PlayerInput> players_;
    std::vector<Departed> departed_;
    std::vector<uint8_t> zeros_, repeat_, out_, scratch_;
    Block block_;
};

} // namespace saida::netcode
