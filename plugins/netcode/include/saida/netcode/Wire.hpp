// The bytes on the wire: identifiers, the byte writer and reader every message
// goes through, the state hash and the input run codec.
//
// Header-only, standard library only. Every multi-byte field is written
// little-endian explicitly; a reader never reads past its end -- it fails, and
// the message is refused with a named reason (Session.hpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace saida::netcode {

// A simulation step. Frame 0 is the first step of a session; frames only grow.
using Frame = int32_t;
inline constexpr Frame kNoFrame = -1;
inline constexpr Frame kForever = std::numeric_limits<Frame>::max();

// A participant's stable identity, the same on every machine and across
// sessions. Chosen by the host (a hash of a public key, a profile id); never 0.
using PlayerId = uint64_t;
// The host transport's handle for a remote endpoint. Local to one machine.
using PeerId = uint32_t;

// 64-bit state hash for desynchronization checks: eight bytes at a time with a
// final avalanche. Not cryptographic. Words are read little-endian, which is
// every target Saida ships (x86-64, ARM64, WebAssembly).
inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

inline uint64_t hash64(const void* data, size_t size, uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed ^ (static_cast<uint64_t>(size) * 0xff51afd7ed558ccdULL);
    while (size >= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        h ^= w * 0x87c37b91114253d5ULL;
        h = (h << 31 | h >> 33) * 0x4cf5ad432745937fULL;
        p += 8;
        size -= 8;
    }
    uint64_t tail = 0;
    for (size_t i = 0; i < size; ++i) tail |= static_cast<uint64_t>(p[i]) << (8 * i);
    return mix64(h ^ tail ^ 0x52dce729ULL);
}

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(uint8_t(v >> (8 * i)));
    }
    void f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        u64(bits);
    }
    void var(uint64_t v) {
        while (v >= 0x80) u8(uint8_t(v | 0x80)), v >>= 7;
        u8(uint8_t(v));
    }
    // Signed values (frames that may be kNoFrame, advantages) zigzagged.
    void svar(int64_t v) { var((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        out_.insert(out_.end(), p, p + size);
    }
    size_t size() const { return out_.size(); }

private:
    std::vector<uint8_t>& out_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    bool ok() const { return ok_; }
    bool done() const { return p_ == end_; }
    size_t left() const { return size_t(end_ - p_); }
    uint8_t u8() {
        if (p_ == end_) return fail(), 0;
        return *p_++;
    }
    uint16_t u16() {
        const uint16_t lo = u8();
        return uint16_t(lo | uint16_t(u8()) << 8);
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(u8()) << (8 * i);
        return v;
    }
    double f64() {
        const uint64_t bits = u64();
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    uint64_t var() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const uint8_t b = u8();
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        return fail(), 0;
    }
    int64_t svar() {
        const uint64_t v = var();
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    }
    // A frame, refused unless it lies in [kNoFrame, kForever].
    Frame frame() {
        const int64_t v = svar();
        if (v < kNoFrame || v > kForever) return fail(), kNoFrame;
        return Frame(v);
    }
    const uint8_t* bytes(size_t size) {
        if (left() < size) return fail(), nullptr;
        const uint8_t* at = p_;
        p_ += size;
        return at;
    }
    void fail() {
        ok_ = false;
        p_ = end_;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

// Consecutive inputs of one player: each frame XORed with the previous one
// (the first with zero), then the whole run as alternating zero-runs and
// literal-runs. A player holding a stick still costs about two bytes for any
// number of frames; nothing outside the message is needed to decode it.
inline void encodeInputs(Writer& w, const uint8_t* frames, size_t count, size_t inputSize) {
    const size_t total = count * inputSize;
    auto diff = [&](size_t i) -> uint8_t {
        return i < inputSize ? frames[i] : uint8_t(frames[i] ^ frames[i - inputSize]);
    };
    size_t i = 0;
    while (i < total) {
        size_t zeros = 0;
        while (i + zeros < total && diff(i + zeros) == 0) ++zeros;
        w.var(zeros);
        i += zeros;
        if (i >= total) break;
        size_t literal = 0;
        while (i + literal < total && diff(i + literal) != 0) ++literal;
        w.var(literal);
        for (size_t k = 0; k < literal; ++k) w.u8(diff(i + k));
        i += literal;
    }
}

// Decodes `count` frames into `out` (count * inputSize bytes). False on any
// malformed or overlong run; `out` is then unspecified.
inline bool decodeInputs(Reader& r, uint8_t* out, size_t count, size_t inputSize) {
    const size_t total = count * inputSize;
    size_t i = 0;
    while (i < total) {
        const uint64_t zeros = r.var();
        if (!r.ok() || zeros > total - i) return false;
        std::memset(out + i, 0, size_t(zeros));
        i += size_t(zeros);
        if (i >= total) break;
        const uint64_t literal = r.var();
        if (!r.ok() || literal == 0 || literal > total - i) return false;
        const uint8_t* src = r.bytes(size_t(literal));
        if (!src) return false;
        std::memcpy(out + i, src, size_t(literal));
        i += size_t(literal);
    }
    for (size_t k = inputSize; k < total; ++k) out[k] ^= out[k - inputSize];
    return r.ok();
}

} // namespace saida::netcode
