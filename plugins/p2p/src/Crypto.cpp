#include "saida/p2p/Crypto.hpp"

#include <mbedtls/chachapoly.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <mutex>
#include <stdexcept>

namespace saida::p2p {
namespace {

struct Drbg {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    std::mutex mutex;
    Drbg() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static const char tag[] = "saida-p2p";
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, reinterpret_cast<const unsigned char*>(tag),
                                  sizeof tag - 1) != 0)
            throw std::runtime_error("saida::p2p: no system entropy");
    }
};

Drbg& drbg() {
    static Drbg d;
    return d;
}

int rng(void*, unsigned char* out, size_t size) {
    randomBytes(out, size);
    return 0;
}

void nonce12(uint64_t n, unsigned char out[12]) {
    std::memset(out, 0, 4);
    for (int i = 0; i < 8; ++i) out[4 + i] = uint8_t(n >> (8 * i));
}

// HKDF as Noise defines it (HMAC-SHA256, two outputs).
void hkdf2(const Digest& ck, const uint8_t* ikm, size_t ikmSize, Digest& o1, Digest& o2) {
    const Digest temp = hmacSha256(ck.data(), ck.size(), ikm, ikmSize);
    const uint8_t one = 1;
    o1 = hmacSha256(temp.data(), temp.size(), &one, 1);
    uint8_t buf[33];
    std::memcpy(buf, o1.data(), 32);
    buf[32] = 2;
    o2 = hmacSha256(temp.data(), temp.size(), buf, 33);
}

const char kProtocol[] = "Noise_XX_25519_ChaChaPoly_SHA256";

} // namespace

void randomBytes(void* out, size_t size) {
    Drbg& d = drbg();
    std::lock_guard<std::mutex> lock(d.mutex);
    auto* p = static_cast<unsigned char*>(out);
    while (size) {
        const size_t n = size < MBEDTLS_CTR_DRBG_MAX_REQUEST ? size : MBEDTLS_CTR_DRBG_MAX_REQUEST;
        if (mbedtls_ctr_drbg_random(&d.drbg, p, n) != 0) throw std::runtime_error("saida::p2p: random failed");
        p += n, size -= n;
    }
}

uint64_t randomU64() {
    uint64_t v;
    randomBytes(&v, sizeof v);
    return v;
}

Digest sha256(const void* data, size_t size) { return sha256(data, size, nullptr, 0); }

Digest sha256(const void* a, size_t aSize, const void* b, size_t bSize) {
    Digest out;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    if (aSize) mbedtls_sha256_update(&c, static_cast<const unsigned char*>(a), aSize);
    if (bSize) mbedtls_sha256_update(&c, static_cast<const unsigned char*>(b), bSize);
    mbedtls_sha256_finish(&c, out.data());
    mbedtls_sha256_free(&c);
    return out;
}

Digest hmacSha256(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size) {
    uint8_t k[64] = {};
    if (keySize > 64) {
        const Digest d = sha256(key, keySize);
        std::memcpy(k, d.data(), 32);
    } else if (keySize) {
        std::memcpy(k, key, keySize);
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    const Digest inner = sha256(pad, 64, data, size);
    for (int i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    return sha256(pad, 64, inner.data(), inner.size());
}

bool x25519(Key& out, const Key& secret, const Key& point) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point P, R;
    mbedtls_mpi d;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&P);
    mbedtls_ecp_point_init(&R);
    mbedtls_mpi_init(&d);
    uint8_t k[32];
    std::memcpy(k, secret.data(), 32);
    k[0] &= 248, k[31] &= 127, k[31] |= 64;
    // read_binary masks the top bit of a Curve25519 u-coordinate (RFC 7748 5).
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_mpi_read_binary_le(&d, k, 32) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &P, point.data(), 32) == 0 &&
              mbedtls_ecp_mul(&grp, &R, &d, &P, rng, nullptr) == 0;
    size_t written = 0;
    ok = ok && mbedtls_ecp_point_write_binary(&grp, &R, MBEDTLS_ECP_PF_UNCOMPRESSED, &written, out.data(), 32) == 0 &&
         written == 32;
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&R);
    mbedtls_ecp_point_free(&P);
    mbedtls_ecp_group_free(&grp);
    std::memset(k, 0, sizeof k);
    if (!ok) return out.fill(0), false;
    uint8_t any = 0;
    for (uint8_t b : out) any |= b;
    return any != 0;
}

KeyPair KeyPair::fromSecret(const Key& secret) {
    KeyPair kp;
    kp.secret = secret;
    Key base{};
    base[0] = 9;
    x25519(kp.pub, kp.secret, base);
    return kp;
}

KeyPair KeyPair::generate() {
    Key s;
    randomBytes(s.data(), s.size());
    return fromSecret(s);
}

void Aead::seal(uint64_t nonce, const uint8_t* aad, size_t aadSize, const uint8_t* plain, size_t size,
                uint8_t* out) const {
    mbedtls_chachapoly_context c;
    mbedtls_chachapoly_init(&c);
    mbedtls_chachapoly_setkey(&c, key_.data());
    unsigned char n[12];
    nonce12(nonce, n);
    mbedtls_chachapoly_encrypt_and_tag(&c, size, n, aad, aadSize, plain, out, out + size);
    mbedtls_chachapoly_free(&c);
}

bool Aead::open(uint64_t nonce, const uint8_t* aad, size_t aadSize, const uint8_t* cipher, size_t size,
                uint8_t* out) const {
    if (size < kTag) return false;
    mbedtls_chachapoly_context c;
    mbedtls_chachapoly_init(&c);
    mbedtls_chachapoly_setkey(&c, key_.data());
    unsigned char n[12];
    nonce12(nonce, n);
    const int r = mbedtls_chachapoly_auth_decrypt(&c, size - kTag, n, aad, aadSize, cipher + size - kTag, cipher, out);
    mbedtls_chachapoly_free(&c);
    return r == 0;
}

// --- Noise XX ------------------------------------------------------------------------

void NoiseXX::begin(bool initiator, const KeyPair& local, const uint8_t* prologue, size_t prologueSize) {
    initiator_ = initiator;
    s_ = local;
    if (!fixedEphemeral_) e_ = KeyPair::generate();
    rs_.fill(0), re_.fill(0);
    static_assert(sizeof kProtocol - 1 == 32, "the protocol name is exactly HASHLEN: h = name");
    std::memcpy(h_.data(), kProtocol, 32);
    ck_ = h_;
    k_ = Aead{};
    n_ = 0;
    mixHash(prologue, prologueSize);
}

void NoiseXX::mixHash(const uint8_t* data, size_t size) { h_ = sha256(h_.data(), h_.size(), data, size); }

void NoiseXX::mixKey(const Key& ikm) {
    Digest k;
    hkdf2(ck_, ikm.data(), ikm.size(), ck_, k);
    k_.setKey(k);
    n_ = 0;
}

bool NoiseXX::dh(const Key& secret, const Key& pub, Key& out) { return x25519(out, secret, pub); }

void NoiseXX::encryptAndHash(const uint8_t* plain, size_t size, Bytes& out) {
    const size_t at = out.size();
    if (!k_.ready()) {
        out.insert(out.end(), plain, plain + size);
    } else {
        out.resize(at + size + Aead::kTag);
        k_.seal(n_++, h_.data(), h_.size(), plain, size, out.data() + at);
    }
    mixHash(out.data() + at, out.size() - at);
}

bool NoiseXX::decryptAndHash(const uint8_t* cipher, size_t size, Bytes& out) {
    if (!k_.ready()) {
        out.assign(cipher, cipher + size);
    } else {
        if (size < Aead::kTag) return false;
        out.resize(size - Aead::kTag);
        if (!k_.open(n_++, h_.data(), h_.size(), cipher, size, out.data())) return false;
    }
    mixHash(cipher, size);
    return true;
}

void NoiseXX::write1(const uint8_t* payload, size_t size, Bytes& out) {
    out.assign(e_.pub.begin(), e_.pub.end());
    mixHash(e_.pub.data(), 32);
    encryptAndHash(payload, size, out);
}

bool NoiseXX::read1(const uint8_t* msg, size_t size, Bytes& payload) {
    if (size < 32) return false;
    std::memcpy(re_.data(), msg, 32);
    mixHash(re_.data(), 32);
    return decryptAndHash(msg + 32, size - 32, payload);
}

bool NoiseXX::write2(const uint8_t* payload, size_t size, Bytes& out) {
    out.assign(e_.pub.begin(), e_.pub.end());
    mixHash(e_.pub.data(), 32);
    Key shared;
    if (!dh(e_.secret, re_, shared)) return false;  // ee
    mixKey(shared);
    encryptAndHash(s_.pub.data(), 32, out);
    if (!dh(s_.secret, re_, shared)) return false;  // es
    mixKey(shared);
    encryptAndHash(payload, size, out);
    return true;
}

bool NoiseXX::read2(const uint8_t* msg, size_t size, Bytes& payload) {
    if (size < 32 + 48 + Aead::kTag) return false;
    std::memcpy(re_.data(), msg, 32);
    mixHash(re_.data(), 32);
    Key shared;
    if (!dh(e_.secret, re_, shared)) return false;  // ee
    mixKey(shared);
    Bytes s;
    if (!decryptAndHash(msg + 32, 48, s) || s.size() != 32) return false;
    std::memcpy(rs_.data(), s.data(), 32);
    if (!dh(e_.secret, rs_, shared)) return false;  // es
    mixKey(shared);
    return decryptAndHash(msg + 80, size - 80, payload);
}

bool NoiseXX::write3(const uint8_t* payload, size_t size, Bytes& out) {
    out.clear();
    encryptAndHash(s_.pub.data(), 32, out);
    Key shared;
    if (!dh(s_.secret, re_, shared)) return false;  // se
    mixKey(shared);
    encryptAndHash(payload, size, out);
    return true;
}

bool NoiseXX::read3(const uint8_t* msg, size_t size, Bytes& payload) {
    if (size < 48 + Aead::kTag) return false;
    Bytes s;
    if (!decryptAndHash(msg, 48, s) || s.size() != 32) return false;
    std::memcpy(rs_.data(), s.data(), 32);
    Key shared;
    if (!dh(e_.secret, rs_, shared)) return false;  // se
    mixKey(shared);
    return decryptAndHash(msg + 48, size - 48, payload);
}

void NoiseXX::split(Aead& send, Aead& receive) const {
    Digest k1, k2;
    hkdf2(ck_, nullptr, 0, k1, k2);
    send.setKey(initiator_ ? k1 : k2);
    receive.setKey(initiator_ ? k2 : k1);
}

} // namespace saida::p2p
