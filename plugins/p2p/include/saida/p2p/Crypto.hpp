// Cryptography for peer links, on the engine's mbedTLS (third_party/mbedtls):
// X25519, ChaCha20-Poly1305, SHA-256 and the Noise XX handshake
// (Noise_XX_25519_ChaChaPoly_SHA256, noiseprotocol.org revision 34).
//
// Nothing here is invented: every primitive is mbedTLS's, the handshake is
// Noise's, and tests/test_p2p.cpp checks the RFC 7748, RFC 8439, FIPS 180 and
// RFC 4231 vectors through these wrappers.
#pragma once

#include "saida/p2p/Address.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace saida::p2p {

using Key = std::array<uint8_t, 32>;
using Digest = std::array<uint8_t, 32>;

// The operating system's entropy through mbedTLS's CTR-DRBG; thread-safe.
void randomBytes(void* out, size_t size);
uint64_t randomU64();

Digest sha256(const void* data, size_t size);
Digest sha256(const void* a, size_t aSize, const void* b, size_t bSize);
Digest hmacSha256(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size);

// RFC 7748 X25519. False when the result is all zeros (a low-order point):
// the peer is refused rather than given a known key.
bool x25519(Key& out, const Key& secret, const Key& point);

struct KeyPair {
    Key secret{}, pub{};
    static KeyPair generate();
    static KeyPair fromSecret(const Key& secret);
};

// ChaCha20-Poly1305 (RFC 8439) with Noise's nonce: four zero bytes and the
// 64-bit counter, little-endian. seal writes size + 16 bytes; open reads
// size bytes (tag included) and writes size - 16.
class Aead {
public:
    static constexpr size_t kTag = 16;
    void setKey(const Key& key) { key_ = key, ready_ = true; }
    bool ready() const { return ready_; }
    void seal(uint64_t nonce, const uint8_t* aad, size_t aadSize, const uint8_t* plain, size_t size, uint8_t* out) const;
    bool open(uint64_t nonce, const uint8_t* aad, size_t aadSize, const uint8_t* cipher, size_t size, uint8_t* out) const;

private:
    Key key_{};
    bool ready_ = false;
};

// Noise XX, both roles, three messages:
//   -> e                 (initiator; carries a plaintext payload)
//   <- e, ee, s, es      (responder; its static key and payload encrypted)
//   -> s, se             (initiator; its static key and payload encrypted)
// After the third message both sides hold two transport keys and the other's
// authenticated static key. The prologue (application, version) is hashed
// in: peers with different prologues cannot complete a handshake.
class NoiseXX {
public:
    void begin(bool initiator, const KeyPair& local, const uint8_t* prologue, size_t prologueSize);
    // Initiator: message 1. Responder: reads it.
    void write1(const uint8_t* payload, size_t size, Bytes& out);
    bool read1(const uint8_t* msg, size_t size, Bytes& payload);
    // Responder: message 2. Initiator: reads it.
    bool write2(const uint8_t* payload, size_t size, Bytes& out);
    bool read2(const uint8_t* msg, size_t size, Bytes& payload);
    // Initiator: message 3. Responder: reads it.
    bool write3(const uint8_t* payload, size_t size, Bytes& out);
    bool read3(const uint8_t* msg, size_t size, Bytes& payload);
    // The transport keys: (send, receive) for this side.
    void split(Aead& send, Aead& receive) const;

    const Key& remoteStatic() const { return rs_; }
    const Key& localEphemeral() const { return e_.pub; }
    const Digest& handshakeHash() const { return h_; }
    // Overrides the ephemeral key (tests replay known vectors with it).
    void setEphemeral(const KeyPair& e) { e_ = e, fixedEphemeral_ = true; }

private:
    void mixHash(const uint8_t* data, size_t size);
    void mixKey(const Key& ikm);
    bool dh(const Key& secret, const Key& pub, Key& out);
    void encryptAndHash(const uint8_t* plain, size_t size, Bytes& out);
    bool decryptAndHash(const uint8_t* cipher, size_t size, Bytes& out);

    bool initiator_ = false, fixedEphemeral_ = false;
    KeyPair s_, e_;
    Key rs_{}, re_{};
    Digest h_{}, ck_{};
    Aead k_;
    uint64_t n_ = 0;
};

} // namespace saida::p2p
