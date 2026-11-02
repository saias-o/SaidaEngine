// Qualifies a Simulation for rollback before any network is involved. Every
// frame is simulated once, then the last `distance` frames are loaded back and
// simulated again, and every state on the way must hash the same as the first
// time. A simulation that keeps state outside save(), reads a clock, an
// unseeded random or uninitialized memory fails on the first frame it does so,
// with that frame named -- instead of a desync an hour into a match.
#pragma once

#include "saida/netcode/Session.hpp"

#include <cstring>
#include <vector>

namespace saida::netcode {

class SyncTest {
public:
    SyncTest(Simulation& simulation, size_t inputSize, int distance = 4)
        : sim_(simulation), inputSize_(inputSize ? inputSize : 1), distance_(distance > 0 ? distance : 1),
          ring_(size_t(distance_) + 1) {
        ledger_.resize(size_t(distance_) + 2);
    }

    // One frame for a fixed roster: `inputs` holds players.size() * inputSize
    // bytes in roster order. False once a replayed state differs.
    bool advance(const std::vector<PlayerId>& players, const uint8_t* inputs) {
        if (mismatch_ != kNoFrame) return false;
        const Frame f = frame_;
        Record& rec = at(f);
        rec.frame = f;
        rec.players = players;
        rec.inputs.assign(inputs, inputs + players.size() * inputSize_);
        rec.state.clear();
        sim_.save(f, rec.state);
        rec.hash = sim_.checksum(rec.state.data(), rec.state.size());
        run(rec, Pass::Live);
        frame_ = f + 1;
        scratch_.clear();
        sim_.save(frame_, scratch_);
        const uint64_t end = sim_.checksum(scratch_.data(), scratch_.size());
        if (frame_ < distance_) return true;

        const Frame from = frame_ - distance_;
        const Record& start = at(from);
        sim_.load(from, start.state.data(), start.state.size());
        for (Frame g = from; g < frame_; ++g) {
            const Record& r = at(g);
            if (g > from) {
                scratch_.clear();
                sim_.save(g, scratch_);
                if (sim_.checksum(scratch_.data(), scratch_.size()) != r.hash) return fail(g);
            }
            run(r, Pass::Resimulate);
        }
        scratch_.clear();
        sim_.save(frame_, scratch_);
        if (sim_.checksum(scratch_.data(), scratch_.size()) != end) return fail(frame_);
        return true;
    }

    Frame frame() const { return frame_; }
    // The first frame whose replayed state differed, or kNoFrame.
    Frame mismatch() const { return mismatch_; }

private:
    struct Record {
        Frame frame = kNoFrame;
        std::vector<PlayerId> players;
        std::vector<uint8_t> inputs, state;
        uint64_t hash = 0;
    };

    Record& at(Frame f) { return ring_[size_t(f) % ring_.size()]; }

    void run(const Record& r, Pass pass) {
        entries_.clear();
        for (size_t i = 0; i < r.players.size(); ++i)
            entries_.push_back({r.players[i], 1, r.frame == 0, r.inputs.data() + i * inputSize_});
        FrameContext ctx;
        ctx.frame = r.frame;
        ctx.pass = pass;
        ctx.players = entries_.data();
        ctx.playerCount = entries_.size();
        ctx.inputSize = inputSize_;
        ctx.ledger = &ledger_;
        ledger_.begin(r.frame, pass);
        sim_.advance(ctx);
        ledger_.end([](Frame, uint64_t) {});
    }

    bool fail(Frame f) {
        mismatch_ = f;
        return false;
    }

    Simulation& sim_;
    size_t inputSize_;
    int distance_;
    std::vector<Record> ring_;
    std::vector<PlayerInput> entries_;
    std::vector<uint8_t> scratch_;
    Ledger ledger_;
    Frame frame_ = 0, mismatch_ = kNoFrame;
};

} // namespace saida::netcode
