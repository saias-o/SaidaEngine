#include "core/PngWriter.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// The engine writes PNGs with its own encoder (no zlib, no third-party
// encoder), so nothing external proves the bytes are a real PNG. Every case
// here encodes pixels, decodes the result with stb_image — an independent
// implementation already vendored for texture loading — and requires an exact
// match. A wrong Huffman code, a wrong filter or a wrong checksum fails the
// decode instead of producing a plausible file.

using namespace saida;
namespace fs = std::filesystem;

namespace {

int gChecks = 0;

void require(bool condition, const char* what) {
    ++gChecks;
    if (!condition) {
        std::cerr << "[png] FAIL: " << what << "\n";
        std::abort();
    }
}

struct Decoded {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

bool decode(const std::vector<uint8_t>& png, Decoded& out) {
    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(png.data(), static_cast<int>(png.size()),
                                          &out.width, &out.height, &channels, 4);
    if (!data) return false;
    out.pixels.assign(data, data + static_cast<size_t>(out.width) * out.height * 4);
    stbi_image_free(data);
    return true;
}

void requireRoundTrip(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                      const char* what) {
    const std::vector<uint8_t> png = encodePngRGBA8(pixels.data(), width, height);
    require(!png.empty(), "encoder produced bytes");

    Decoded decoded;
    require(decode(png, decoded), what);
    require(decoded.width == static_cast<int>(width) && decoded.height == static_cast<int>(height),
            "decoded dimensions match the source");
    require(decoded.pixels == pixels, "decoded pixels are identical to the source");
}

std::vector<uint8_t> solid(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = a;
    }
    return pixels;
}

// -- A flat image: the LZ77 path, long matches everywhere -------------------

void testSolidImageRoundTrips() {
    requireRoundTrip(solid(64, 32, 12, 240, 90, 255), 64, 32, "solid image decodes");
}

// -- A single pixel: no previous scanline, no match to find -----------------

void testSinglePixelRoundTrips() {
    requireRoundTrip(solid(1, 1, 1, 2, 3, 4), 1, 1, "1x1 image decodes");
}

// -- A single row and a single column: the degenerate filter cases ----------

void testDegenerateShapesRoundTrip() {
    requireRoundTrip(solid(256, 1, 200, 10, 10, 255), 256, 1, "1-pixel-tall image decodes");
    requireRoundTrip(solid(1, 256, 10, 200, 10, 255), 1, 256, "1-pixel-wide image decodes");
}

// -- Incompressible noise: exercises the literal path and the worst case ----
//
// A fixed seed keeps this deterministic; the point is byte-exact recovery of
// data that LZ77 cannot help with, where a bit-packing error would surface.

void testNoiseRoundTrips() {
    constexpr uint32_t kWidth = 71;   // deliberately not a power of two
    constexpr uint32_t kHeight = 53;
    std::mt19937 rng(0xC0FFEEu);
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
    for (uint8_t& value : pixels) value = static_cast<uint8_t>(rng() & 0xFFu);
    requireRoundTrip(pixels, kWidth, kHeight, "random noise decodes");
}

// -- A gradient with transparency: every filter competes, alpha is preserved -

void testGradientWithAlphaRoundTrips() {
    constexpr uint32_t kWidth = 128;
    constexpr uint32_t kHeight = 96;
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            pixels[i] = static_cast<uint8_t>(x * 2);
            pixels[i + 1] = static_cast<uint8_t>(y * 2);
            pixels[i + 2] = static_cast<uint8_t>((x + y) & 0xFF);
            pixels[i + 3] = static_cast<uint8_t>(x < kWidth / 2 ? 0 : 255);
        }
    }
    requireRoundTrip(pixels, kWidth, kHeight, "gradient with alpha decodes");
}

// -- Determinism: the same pixels must always produce the same bytes --------
//
// This is what makes an encoded frame usable as a committed golden image.

void testEncodingIsDeterministic() {
    const std::vector<uint8_t> pixels = solid(37, 19, 7, 99, 200, 128);
    const std::vector<uint8_t> first = encodePngRGBA8(pixels.data(), 37, 19);
    const std::vector<uint8_t> second = encodePngRGBA8(pixels.data(), 37, 19);
    require(first == second, "encoding the same pixels twice produces identical bytes");
}

// -- Compression actually happens ------------------------------------------
//
// A stored-block encoder would also round-trip; the reason this encoder exists
// is that captures stay small enough to commit. Guard the property, not a
// specific ratio.

void testFlatImageCompresses() {
    constexpr uint32_t kWidth = 512;
    constexpr uint32_t kHeight = 512;
    const std::vector<uint8_t> pixels = solid(kWidth, kHeight, 30, 30, 40, 255);
    const std::vector<uint8_t> png = encodePngRGBA8(pixels.data(), kWidth, kHeight);
    const size_t raw = pixels.size();
    std::cout << "[png] 512x512 flat: " << png.size() << " bytes for " << raw << " raw\n";
    require(png.size() * 100 < raw, "a flat image compresses to under 1% of its raw size");
}

// -- Refusals and file output ----------------------------------------------

void testEmptyInputIsRefused() {
    const std::vector<uint8_t> pixels = solid(4, 4, 0, 0, 0, 0);
    require(encodePngRGBA8(pixels.data(), 0, 4).empty(), "zero width produces no PNG");
    require(encodePngRGBA8(pixels.data(), 4, 0).empty(), "zero height produces no PNG");
    require(encodePngRGBA8(nullptr, 4, 4).empty(), "null pixels produce no PNG");

    std::string error;
    const fs::path target = fs::temp_directory_path() / "SaidaPngTests" / "refused.png";
    require(!writePngRGBA8(target.string(), pixels.data(), 0, 0, error),
            "writing a zero-sized image fails");
    require(!error.empty(), "the refusal carries a diagnostic");
    require(!fs::exists(target), "a refused write leaves no file behind");
}

void testWriteProducesADecodableFile() {
    const fs::path root = fs::temp_directory_path() / "SaidaPngTests";
    fs::remove_all(root);
    // The parent directory does not exist yet: writing must create it.
    const fs::path target = root / "nested" / "capture.png";

    const std::vector<uint8_t> pixels = solid(48, 24, 255, 128, 0, 255);
    std::string error;
    require(writePngRGBA8(target.string(), pixels.data(), 48, 24, error),
            "writing a PNG to a fresh directory succeeds");
    require(error.empty(), "a successful write reports no error");
    require(fs::exists(target), "the PNG file exists");
    require(!fs::exists(fs::path(target.string() + ".tmp")), "no temporary file is left behind");

    std::vector<uint8_t> bytes;
    {
        // Scoped: Windows refuses to remove a directory holding an open handle.
        std::ifstream file(target, std::ios::binary);
        bytes.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    Decoded decoded;
    require(decode(bytes, decoded), "the written file decodes as a PNG");
    require(decoded.pixels == pixels, "the written file holds the source pixels");

    fs::remove_all(root);
}

} // namespace

int main() {
    testSolidImageRoundTrips();
    testSinglePixelRoundTrips();
    testDegenerateShapesRoundTrip();
    testNoiseRoundTrips();
    testGradientWithAlphaRoundTrips();
    testEncodingIsDeterministic();
    testFlatImageCompresses();
    testEmptyInputIsRefused();
    testWriteProducesADecodableFile();

    std::cout << "[png] PASS (" << gChecks << " checks)\n";
    return 0;
}
