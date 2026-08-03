#include "core/PngWriter.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace saida {

namespace {

// ── Checksums ───────────────────────────────────────────────────────────────

const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }();
    return table;
}

uint32_t crc32Of(const uint8_t* data, size_t size) {
    const std::array<uint32_t, 256>& table = crcTable();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t adler32Of(const uint8_t* data, size_t size) {
    constexpr uint32_t kModulo = 65521u;
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % kModulo;
        b = (b + a) % kModulo;
    }
    return (b << 16) | a;
}

// ── Deflate bit stream ──────────────────────────────────────────────────────

// Deflate packs plain values LSB-first inside each byte, but Huffman codes
// travel MSB-first. The two writers below keep that asymmetry explicit rather
// than leaving it to each call site.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    void bits(uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            push((value >> i) & 1u);
        }
    }

    void huffman(uint32_t code, int count) {
        for (int i = count - 1; i >= 0; --i) {
            push((code >> i) & 1u);
        }
    }

    void flush() {
        if (bitCount_ > 0) {
            out_.push_back(static_cast<uint8_t>(buffer_));
            buffer_ = 0;
            bitCount_ = 0;
        }
    }

private:
    void push(uint32_t bit) {
        buffer_ |= bit << bitCount_;
        if (++bitCount_ == 8) {
            out_.push_back(static_cast<uint8_t>(buffer_));
            buffer_ = 0;
            bitCount_ = 0;
        }
    }

    std::vector<uint8_t>& out_;
    uint32_t buffer_ = 0;
    int bitCount_ = 0;
};

// RFC 1951 section 3.2.6: the fixed literal/length alphabet.
void emitFixedSymbol(BitWriter& writer, unsigned symbol) {
    if (symbol < 144) {
        writer.huffman(0x30u + symbol, 8);
    } else if (symbol < 256) {
        writer.huffman(0x190u + (symbol - 144), 9);
    } else if (symbol < 280) {
        writer.huffman(symbol - 256, 7);
    } else {
        writer.huffman(0xC0u + (symbol - 280), 8);
    }
}

constexpr uint16_t kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3,
    3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
    8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void emitMatch(BitWriter& writer, unsigned length, unsigned distance) {
    int lengthCode = 28;
    while (lengthCode > 0 && kLengthBase[lengthCode] > length) --lengthCode;
    emitFixedSymbol(writer, 257u + static_cast<unsigned>(lengthCode));
    writer.bits(length - kLengthBase[lengthCode], kLengthExtra[lengthCode]);

    int distanceCode = 29;
    while (distanceCode > 0 && kDistanceBase[distanceCode] > distance) --distanceCode;
    // Fixed-Huffman distances are plain 5-bit codes, still written MSB-first.
    writer.huffman(static_cast<uint32_t>(distanceCode), 5);
    writer.bits(distance - kDistanceBase[distanceCode], kDistanceExtra[distanceCode]);
}

// ── LZ77 over a bounded hash chain ──────────────────────────────────────────

constexpr size_t kWindowSize = 32768;
constexpr size_t kMinMatch = 3;
constexpr size_t kMaxMatch = 258;
constexpr size_t kHashBits = 15;
constexpr size_t kHashSize = size_t{1} << kHashBits;
// Bounded chain walk: the cost ceiling that keeps a 1080p capture fast. Deeper
// searches buy little on the flat regions these images are made of.
constexpr int kMaxChainWalk = 128;

size_t hashAt(const uint8_t* data, size_t pos) {
    return ((static_cast<size_t>(data[pos]) << 10) ^
            (static_cast<size_t>(data[pos + 1]) << 5) ^
            static_cast<size_t>(data[pos + 2])) &
           (kHashSize - 1);
}

std::vector<uint8_t> deflateFixed(const std::vector<uint8_t>& input) {
    std::vector<uint8_t> out;
    out.reserve(input.size() / 2 + 64);
    BitWriter writer(out);

    // A single final block with fixed Huffman codes (BFINAL=1, BTYPE=01).
    writer.bits(1, 1);
    writer.bits(1, 2);

    const size_t size = input.size();
    const uint8_t* data = input.data();
    std::vector<int32_t> head(kHashSize, -1);
    std::vector<int32_t> prev(size, -1);

    auto insert = [&](size_t pos) {
        if (pos + kMinMatch > size) return;
        const size_t h = hashAt(data, pos);
        prev[pos] = head[h];
        head[h] = static_cast<int32_t>(pos);
    };

    size_t pos = 0;
    while (pos < size) {
        size_t bestLength = 0;
        size_t bestDistance = 0;

        if (pos + kMinMatch <= size) {
            const size_t maxLength = std::min(kMaxMatch, size - pos);
            int32_t candidate = head[hashAt(data, pos)];
            int walk = 0;
            while (candidate >= 0 && walk < kMaxChainWalk) {
                const size_t distance = pos - static_cast<size_t>(candidate);
                if (distance == 0 || distance > kWindowSize) break;

                size_t length = 0;
                while (length < maxLength &&
                       data[static_cast<size_t>(candidate) + length] == data[pos + length]) {
                    ++length;
                }
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = distance;
                    if (length == maxLength) break;
                }
                candidate = prev[static_cast<size_t>(candidate)];
                ++walk;
            }
        }

        if (bestLength >= kMinMatch) {
            emitMatch(writer, static_cast<unsigned>(bestLength), static_cast<unsigned>(bestDistance));
            // Every position the match covers still has to enter the chain, or
            // later positions lose the references that make the next match.
            for (size_t i = 0; i < bestLength; ++i) insert(pos + i);
            pos += bestLength;
        } else {
            emitFixedSymbol(writer, data[pos]);
            insert(pos);
            ++pos;
        }
    }

    emitFixedSymbol(writer, 256);  // end of block
    writer.flush();
    return out;
}

std::vector<uint8_t> zlibWrap(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> stream;
    // CMF=0x78 (deflate, 32 KiB window), FLG=0x01 → (0x7801 % 31) == 0.
    stream.push_back(0x78);
    stream.push_back(0x01);
    const std::vector<uint8_t> compressed = deflateFixed(raw);
    stream.insert(stream.end(), compressed.begin(), compressed.end());

    const uint32_t adler = adler32Of(raw.data(), raw.size());
    stream.push_back(static_cast<uint8_t>(adler >> 24));
    stream.push_back(static_cast<uint8_t>(adler >> 16));
    stream.push_back(static_cast<uint8_t>(adler >> 8));
    stream.push_back(static_cast<uint8_t>(adler));
    return stream;
}

// ── PNG scanline filtering ──────────────────────────────────────────────────

uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - static_cast<int>(a));
    const int pb = std::abs(p - static_cast<int>(b));
    const int pc = std::abs(p - static_cast<int>(c));
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// Standard minimum-sum-of-absolute-differences heuristic: the filter whose
// output has the smallest signed magnitude usually compresses best.
size_t filterScore(const std::vector<uint8_t>& row) {
    size_t score = 0;
    for (uint8_t value : row) {
        score += value < 128 ? value : 256u - value;
    }
    return score;
}

std::vector<uint8_t> filterScanlines(const uint8_t* pixels, uint32_t width, uint32_t height) {
    constexpr size_t kBytesPerPixel = 4;
    const size_t stride = static_cast<size_t>(width) * kBytesPerPixel;

    std::vector<uint8_t> out;
    out.reserve((stride + 1) * height);

    std::vector<uint8_t> candidate(stride);
    std::vector<uint8_t> best(stride);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * stride;
        const uint8_t* above = y > 0 ? row - stride : nullptr;

        uint8_t bestFilter = 0;
        size_t bestScore = 0;
        bool haveBest = false;

        for (uint8_t filter = 0; filter <= 4; ++filter) {
            for (size_t i = 0; i < stride; ++i) {
                const uint8_t left = i >= kBytesPerPixel ? row[i - kBytesPerPixel] : 0;
                const uint8_t up = above ? above[i] : 0;
                const uint8_t upLeft =
                    (above && i >= kBytesPerPixel) ? above[i - kBytesPerPixel] : 0;
                switch (filter) {
                case 0: candidate[i] = row[i]; break;
                case 1: candidate[i] = static_cast<uint8_t>(row[i] - left); break;
                case 2: candidate[i] = static_cast<uint8_t>(row[i] - up); break;
                case 3:
                    candidate[i] = static_cast<uint8_t>(
                        row[i] - static_cast<uint8_t>((static_cast<int>(left) + static_cast<int>(up)) / 2));
                    break;
                default:
                    candidate[i] = static_cast<uint8_t>(row[i] - paethPredictor(left, up, upLeft));
                    break;
                }
            }
            const size_t score = filterScore(candidate);
            if (!haveBest || score < bestScore) {
                bestScore = score;
                bestFilter = filter;
                best = candidate;
                haveBest = true;
            }
        }

        out.push_back(bestFilter);
        out.insert(out.end(), best.begin(), best.end());
    }
    return out;
}

// ── Chunks ──────────────────────────────────────────────────────────────────

void appendBigEndian32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void appendChunk(std::vector<uint8_t>& out, const char type[5], const std::vector<uint8_t>& data) {
    appendBigEndian32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> checked;
    checked.reserve(4 + data.size());
    for (int i = 0; i < 4; ++i) checked.push_back(static_cast<uint8_t>(type[i]));
    checked.insert(checked.end(), data.begin(), data.end());
    out.insert(out.end(), checked.begin(), checked.end());
    appendBigEndian32(out, crc32Of(checked.data(), checked.size()));
}

} // namespace

std::vector<uint8_t> encodePngRGBA8(const uint8_t* pixels, uint32_t width, uint32_t height) {
    if (pixels == nullptr || width == 0 || height == 0) return {};

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    appendBigEndian32(ihdr, width);
    appendBigEndian32(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // colour type: truecolour with alpha
    ihdr.push_back(0);  // compression: deflate
    ihdr.push_back(0);  // filter method: adaptive
    ihdr.push_back(0);  // interlace: none
    appendChunk(png, "IHDR", ihdr);

    appendChunk(png, "IDAT", zlibWrap(filterScanlines(pixels, width, height)));
    appendChunk(png, "IEND", {});
    return png;
}

bool writePngRGBA8(const std::string& path, const uint8_t* pixels,
                   uint32_t width, uint32_t height, std::string& error) {
    const std::vector<uint8_t> png = encodePngRGBA8(pixels, width, height);
    if (png.empty()) {
        error = "refusing to write a PNG with no pixels";
        return false;
    }

    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
    }

    // Write through a temporary so a failed write never leaves a truncated PNG
    // where a reader (or a golden-image comparison) would pick it up.
    const std::filesystem::path temporary = target.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot open '" + temporary.string() + "' for writing";
            return false;
        }
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) {
            error = "failed while writing '" + temporary.string() + "'";
            file.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        std::filesystem::rename(temporary, target, ec);
    }
    if (ec) {
        error = "cannot move the encoded PNG into '" + target.string() + "': " + ec.message();
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return false;
    }
    return true;
}

} // namespace saida
