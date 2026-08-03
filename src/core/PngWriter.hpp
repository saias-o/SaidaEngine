#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace saida {

// 8-bit RGBA PNG encoder, self-contained (no zlib, no third-party encoder).
//
// Invariant: the encoding is a pure function of the pixels. The same input
// always produces the same bytes on every platform and build, so an encoded
// frame is usable as a committed golden image and as a byte-comparable
// artifact. Nothing here depends on time, locale, allocation addresses or
// floating point.
//
// The deflate stream uses fixed Huffman codes with bounded LZ77 matching:
// enough compression for UI and frame captures (large flat regions) without
// pulling in a compression library.

// Encode `width` * `height` RGBA pixels (4 bytes each, top row first,
// `pixels` holding width*height*4 bytes) into a complete PNG file image.
// Returns an empty vector when the dimensions are zero.
std::vector<uint8_t> encodePngRGBA8(const uint8_t* pixels, uint32_t width, uint32_t height);

// Encode and write to `path`. Returns false and fills `error` on an encoding
// or I/O failure; on failure no partial file is left behind.
bool writePngRGBA8(const std::string& path, const uint8_t* pixels,
                   uint32_t width, uint32_t height, std::string& error);

} // namespace saida
