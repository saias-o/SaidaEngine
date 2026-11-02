// The contract between the netcode and the game: what one frame is, what the
// game must provide, and the ledger that keeps effects and persistent
// consequences right across rollbacks.
//
// Header-only, standard library only.
#pragma once

#include "saida/netcode/Wire.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace saida::netcode {

enum class Pass : uint8_t {
    Live,        // the first time this machine simulates the frame
    Resimulate,  // again, after a rollback
    CatchUp,     // fast-forward of a joiner: nothing is shown
};

enum class Departure : uint8_t { Left, Dropped };

struct PlayerInput {
    PlayerId player;
    uint32_t incarnation;
    bool joined;  // first frame this incarnation takes part in
    const uint8_t* data;
};

struct Departed {
    PlayerId player;
    uint32_t incarnation;
    Departure kind;
};

class Ledger;

// One frame to simulate. Players are sorted by (player, incarnation) and are
// exactly those taking part in this frame; departed lists who stopped taking
// part at this frame. Neither depends on which machine runs it.
struct FrameContext {
    Frame frame = 0;
    Pass pass = Pass::Live;
    const PlayerInput* players = nullptr;
    size_t playerCount = 0;
    const Departed* departed = nullptr;
    size_t departedCount = 0;
    size_t inputSize = 0;
    Ledger* ledger = nullptr;

    // A sound, a particle, a camera shake: true when the host should start it
    // now. A key fired once for this frame is not fired again when the frame
    // is simulated again; a key that a resimulation no longer fires is
    // retracted (Simulation::retract). Nothing fires during catch-up.
    bool effect(uint64_t key) const;
    // A persistent consequence (a save, a score, a purchase): recorded now,
    // handed to Simulation::commit exactly once, when this frame is confirmed,
    // with the bytes of its last simulation.
    void commit(uint64_t key, const void* data = nullptr, size_t size = 0) const;

    template <class T>
    T input(size_t i) const {
        T value{};
        std::memcpy(&value, players[i].data, std::min(sizeof(T), inputSize));
        return value;
    }
};

// The game. advance() must be a pure function of the loaded state and the
// frame context: no clock, no unseeded random, no local-only data. save()
// must write the same bytes for the same state on every machine (no padding,
// no pointers).
class Simulation {
public:
    virtual ~Simulation() = default;
    // Write the whole simulated state at the start of `frame` into `out`
    // (cleared, capacity kept).
    virtual void save(Frame frame, std::vector<uint8_t>& out) = 0;
    virtual void load(Frame frame, const uint8_t* data, size_t size) = 0;
    virtual void advance(const FrameContext& frame) = 0;
    // Hash used for desynchronization checks. Override to skip caches that
    // are saved but do not affect the simulation.
    virtual uint64_t checksum(const uint8_t* data, size_t size) { return hash64(data, size); }
    virtual void commit(Frame, uint64_t /*key*/, const uint8_t* /*data*/, size_t /*size*/) {}
    virtual void retract(Frame, uint64_t /*key*/) {}
    // Guess `player`'s input at `frame` from its last known one, `ahead`
    // frames older (>= 1). The default repeats it, as GGPO does. A game that
    // knows its layout does better: a held direction stays held, a button
    // press is not repeated, a press is never invented. Only this machine's
    // predictions depend on it; it need not agree with other machines.
    virtual void predict(PlayerId /*player*/, int /*ahead*/, const uint8_t* last, uint8_t* out, size_t size) {
        std::memcpy(out, last, size);
    }
};

// --- Effects and commits --------------------------------------------------------

// Per-frame record of fired effects and pending commits, over the frames that
// may still be simulated again.
class Ledger {
public:
    void resize(size_t frames) { slots_.assign(std::max<size_t>(frames, 1), {}); }

    void begin(Frame frame, Pass pass) {
        Slot& s = slots_[size_t(frame) % slots_.size()];
        current_ = &s;
        frame_ = frame;
        suppress_ = pass == Pass::CatchUp;
        resimulating_ = pass == Pass::Resimulate && s.frame == frame;
        // A resimulated frame too old to remember what it showed shows nothing.
        if (pass == Pass::Resimulate && !resimulating_) suppress_ = true;
        if (resimulating_) {
            s.seen.assign(s.fired.size(), 0);
        } else {
            s.frame = frame;
            s.fired.clear();
            s.seen.clear();
        }
        s.commits.clear();
        s.bytes.clear();
    }

    bool fire(uint64_t key) {
        if (!current_) return false;
        Slot& s = *current_;
        for (size_t i = 0; i < s.fired.size(); ++i)
            if (s.fired[i] == key) {
                s.seen[i] = 1;
                return false;
            }
        if (suppress_) return false;
        s.fired.push_back(key);
        s.seen.push_back(1);
        return true;
    }

    void commit(uint64_t key, const void* data, size_t size) {
        if (!current_) return;
        Slot& s = *current_;
        s.commits.push_back({key, s.bytes.size(), size});
        const auto* p = static_cast<const uint8_t*>(data);
        if (size) s.bytes.insert(s.bytes.end(), p, p + size);
    }

    template <class Retract>
    void end(Retract&& retract) {
        if (current_ && resimulating_) {
            Slot& s = *current_;
            size_t kept = 0;
            for (size_t i = 0; i < s.fired.size(); ++i) {
                if (s.seen[i]) s.fired[kept++] = s.fired[i];
                else retract(frame_, s.fired[i]);
            }
            s.fired.resize(kept);
            s.seen.assign(kept, 1);
        }
        current_ = nullptr;
    }

    template <class Commit>
    void flush(Frame frame, Commit&& commit) const {
        const Slot& s = slots_[size_t(frame) % slots_.size()];
        if (s.frame != frame) return;
        for (const auto& c : s.commits) commit(c.key, s.bytes.data() + c.offset, c.size);
    }

private:
    struct Entry {
        uint64_t key;
        size_t offset, size;
    };
    struct Slot {
        Frame frame = kNoFrame;
        std::vector<uint64_t> fired;
        std::vector<uint8_t> seen;
        std::vector<Entry> commits;
        std::vector<uint8_t> bytes;
    };
    std::vector<Slot> slots_;
    Slot* current_ = nullptr;
    Frame frame_ = kNoFrame;
    bool suppress_ = false, resimulating_ = false;
};

inline bool FrameContext::effect(uint64_t key) const { return ledger && ledger->fire(key); }
inline void FrameContext::commit(uint64_t key, const void* data, size_t size) const {
    if (ledger) ledger->commit(key, data, size);
}

} // namespace saida::netcode
