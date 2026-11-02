// Replays: a confirmed state, then every confirmed frame's roster and inputs,
// and the state hashes the session computed along the way. Playing one back
// through the same Simulation must reproduce those hashes; the first frame
// that does not is named, which turns "it desynced an hour in" into a
// reproducible bug.
//
// A session records into a Replay once Session::record is called, starting at
// its next confirmed checksum frame. A frame costs a flag byte and the
// changed input bytes; a held stick costs about two bytes per frame for the
// whole roster.
#pragma once

#include "saida/netcode/Simulation.hpp"
#include "saida/netcode/Wire.hpp"

#include <cstring>
#include <utility>
#include <vector>

namespace saida::netcode {

class Replay {
public:
    bool started() const { return first_ != kNoFrame; }
    Frame first() const { return first_; }
    Frame end() const { return first_ == kNoFrame ? kNoFrame : first_ + Frame(frames_); }  // next frame to record
    size_t frames() const { return frames_; }
    uint16_t inputSize() const { return inputSize_; }

    // --- Recording (the session calls these) -----------------------------------------

    void begin(uint16_t inputSize, Frame frame, const uint8_t* state, size_t size) {
        *this = Replay();
        inputSize_ = inputSize;
        first_ = frame;
        state_.assign(state, state + size);
    }

    void frame(const PlayerInput* players, size_t count, const Departed* departed, size_t departedCount) {
        Writer w(stream_);
        bool same = count == ids_.size();
        for (size_t i = 0; same && i < count; ++i)
            same = ids_[i].first == players[i].player && ids_[i].second == players[i].incarnation && !players[i].joined;
        w.u8(uint8_t(!same) | uint8_t(departedCount ? 2 : 0));
        if (!same) {
            ids_.clear();
            w.var(count);
            for (size_t i = 0; i < count; ++i) {
                ids_.push_back({players[i].player, players[i].incarnation});
                w.var(players[i].player);
                w.var(players[i].incarnation);
                w.u8(players[i].joined);
            }
            last_.assign(count * inputSize_, 0);
        }
        if (departedCount) {
            w.var(departedCount);
            for (size_t i = 0; i < departedCount; ++i) {
                w.var(departed[i].player);
                w.var(departed[i].incarnation);
                w.u8(uint8_t(departed[i].kind));
            }
        }
        diff_.resize(count * inputSize_);
        for (size_t i = 0; i < count; ++i)
            for (size_t k = 0; k < inputSize_; ++k) {
                const size_t at = i * inputSize_ + k;
                diff_[at] = players[i].data[k] ^ last_[at];
                last_[at] = players[i].data[k];
            }
        if (count) encodeInputs(w, diff_.data(), 1, diff_.size());
        ++frames_;
    }

    void checksum(Frame frame, uint64_t hash) { checksums_.push_back({frame, hash}); }

    // --- Storage ------------------------------------------------------------------

    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out;
        Writer w(out);
        w.u8('S'), w.u8('R'), w.u8(kVersion);
        w.u16(inputSize_);
        w.svar(first_);
        w.var(frames_);
        w.var(state_.size());
        w.bytes(state_.data(), state_.size());
        w.var(stream_.size());
        w.bytes(stream_.data(), stream_.size());
        w.var(checksums_.size());
        for (const auto& c : checksums_) w.svar(c.first), w.u64(c.second);
        w.u64(hash64(out.data(), out.size()));
        return out;
    }

    // False, and nothing changed, unless the bytes are a whole, undamaged replay.
    bool parse(const uint8_t* data, size_t size) {
        if (size < 8) return false;
        uint64_t sealed = 0;
        for (int i = 0; i < 8; ++i) sealed |= uint64_t(data[size - 8 + size_t(i)]) << (8 * i);
        if (sealed != hash64(data, size - 8)) return false;
        Replay r;
        Reader in(data, size - 8);
        if (in.u8() != 'S' || in.u8() != 'R' || in.u8() != kVersion) return false;
        r.inputSize_ = in.u16();
        r.first_ = in.frame();
        r.frames_ = size_t(in.var());
        const size_t stateSize = size_t(in.var());
        const uint8_t* state = in.bytes(stateSize);
        const size_t streamSize = size_t(in.var());
        const uint8_t* stream = in.bytes(streamSize);
        const uint64_t n = in.var();
        if (!in.ok() || r.inputSize_ == 0 || r.first_ < 0 || n > in.left() / 9 + 1) return false;
        if (r.frames_ > streamSize || r.frames_ > size_t(kForever - r.first_)) return false;
        r.state_.assign(state, state + stateSize);
        r.stream_.assign(stream, stream + streamSize);
        for (uint64_t i = 0; i < n; ++i) {
            const Frame f = in.frame();
            const uint64_t h = in.u64();
            r.checksums_.push_back({f, h});
        }
        if (!in.ok() || !in.done()) return false;
        *this = std::move(r);
        return true;
    }

private:
    friend class ReplayPlayer;
    static constexpr uint8_t kVersion = 1;
    uint16_t inputSize_ = 0;
    Frame first_ = kNoFrame;
    size_t frames_ = 0;
    std::vector<uint8_t> state_, stream_;
    std::vector<std::pair<Frame, uint64_t>> checksums_;
    // Recording state: the roster and inputs of the previous frame.
    std::vector<std::pair<PlayerId, uint32_t>> ids_;
    std::vector<uint8_t> last_, diff_;
};

// Plays a replay through a simulation, one frame per step(). Effects fire as
// in a live session; commits are handed to the simulation only when asked
// (a replay viewer must not save anything).
class ReplayPlayer {
public:
    ReplayPlayer(const Replay& replay, Simulation& simulation, bool commits = false)
        : replay_(replay), sim_(simulation), commits_(commits), in_(replay.stream_.data(), replay.stream_.size()) {
        ledger_.resize(2);
        frame_ = replay.first_;
        if (replay.started()) sim_.load(frame_, replay.state_.data(), replay.state_.size());
    }

    // Simulates the next frame. False at the end, on a damaged replay, or
    // when the state before a frame does not hash as recorded (mismatch()).
    bool step() {
        if (!replay_.started() || done() || mismatch_ != kNoFrame || broken_) return false;
        for (const auto& c : replay_.checksums_)
            if (c.first == frame_) {
                scratch_.clear();
                sim_.save(frame_, scratch_);
                if (sim_.checksum(scratch_.data(), scratch_.size()) != c.second) return mismatch_ = frame_, false;
            }
        const size_t size = replay_.inputSize_;
        const uint8_t flags = in_.u8();
        if (flags & 1) {
            const size_t n = size_t(in_.var());
            if (!in_.ok() || n > in_.left()) return broken_ = true, false;
            players_.assign(n, {});
            for (size_t i = 0; i < n; ++i) {
                players_[i].player = in_.var();
                players_[i].incarnation = uint32_t(in_.var());
                players_[i].joined = in_.u8() != 0;
            }
            inputs_.assign(n * size, 0);
        } else {
            for (auto& p : players_) p.joined = false;
        }
        departed_.clear();
        if (flags & 2) {
            const size_t n = size_t(in_.var());
            if (!in_.ok() || n > in_.left()) return broken_ = true, false;
            for (size_t i = 0; i < n; ++i) {
                Departed d;
                d.player = in_.var();
                d.incarnation = uint32_t(in_.var());
                d.kind = Departure(in_.u8() & 1);
                departed_.push_back(d);
            }
        }
        if (!players_.empty()) {
            diff_.resize(inputs_.size());
            if (!decodeInputs(in_, diff_.data(), 1, diff_.size())) return broken_ = true, false;
            for (size_t i = 0; i < inputs_.size(); ++i) inputs_[i] ^= diff_[i];
        }
        if (!in_.ok()) return broken_ = true, false;
        for (size_t i = 0; i < players_.size(); ++i) players_[i].data = inputs_.data() + i * size;

        FrameContext ctx;
        ctx.frame = frame_;
        ctx.pass = Pass::Live;
        ctx.players = players_.data();
        ctx.playerCount = players_.size();
        ctx.departed = departed_.data();
        ctx.departedCount = departed_.size();
        ctx.inputSize = size;
        ctx.ledger = &ledger_;
        ledger_.begin(frame_, Pass::Live);
        sim_.advance(ctx);
        ledger_.end([](Frame, uint64_t) {});
        if (commits_)
            ledger_.flush(frame_, [&](uint64_t key, const uint8_t* data, size_t n) { sim_.commit(frame_, key, data, n); });
        ++frame_;
        return true;
    }

    bool done() const { return frame_ >= replay_.end(); }
    Frame frame() const { return frame_; }               // next frame to play
    Frame mismatch() const { return mismatch_; }          // first frame whose state differed
    bool broken() const { return broken_; }

private:
    const Replay& replay_;
    Simulation& sim_;
    bool commits_;
    Reader in_;
    Ledger ledger_;
    Frame frame_ = kNoFrame, mismatch_ = kNoFrame;
    bool broken_ = false;
    std::vector<PlayerInput> players_;
    std::vector<Departed> departed_;
    std::vector<uint8_t> inputs_, diff_, scratch_;
};

} // namespace saida::netcode
