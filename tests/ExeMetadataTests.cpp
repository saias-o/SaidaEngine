// The test copies its own executable, patches it via
// applyExeMetadata, then reads back VERSIONINFO and RT_GROUP_ICON with the Windows API.

#include "editor/ExeMetadata.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

void testParseVersion() {
    unsigned short v[4];
    assert(saida::parseExeVersion("1.2.3.4", v));
    assert(v[0] == 1 && v[1] == 2 && v[2] == 3 && v[3] == 4);
    assert(saida::parseExeVersion("1.0.0", v));
    assert(v[0] == 1 && v[1] == 0 && v[2] == 0 && v[3] == 0);
    assert(saida::parseExeVersion("65535", v));
    assert(v[0] == 65535);
    assert(!saida::parseExeVersion("", v));
    assert(!saida::parseExeVersion("1.2.3.4.5", v));
    assert(!saida::parseExeVersion("1.x", v));
    assert(!saida::parseExeVersion("65536", v));
    assert(!saida::parseExeVersion("1..2", v));
}

#ifdef _WIN32

// A minimal valid .ico: a 1x1 image in 32-bit BMP (BITMAPINFOHEADER +
// 1 pixel + AND mask), enough for the icon parser.
std::vector<uint8_t> minimalIco() {
    std::vector<uint8_t> image(40 + 4 + 4, 0);
    const uint32_t headerSize = 40, width = 1, heightX2 = 2;  // XOR+AND height
    const uint16_t planes = 1, bitCount = 32;
    std::memcpy(image.data() + 0, &headerSize, 4);
    std::memcpy(image.data() + 4, &width, 4);
    std::memcpy(image.data() + 8, &heightX2, 4);
    std::memcpy(image.data() + 12, &planes, 2);
    std::memcpy(image.data() + 14, &bitCount, 2);
    image[40] = image[41] = image[42] = 0x80;  // one opaque gray pixel
    image[43] = 0xff;

    std::vector<uint8_t> ico(6 + 16);
    const uint16_t reserved = 0, type = 1, count = 1;
    std::memcpy(ico.data() + 0, &reserved, 2);
    std::memcpy(ico.data() + 2, &type, 2);
    std::memcpy(ico.data() + 4, &count, 2);
    ico[6] = 1;  // width
    ico[7] = 1;  // height
    const uint16_t entryPlanes = 1, entryBits = 32;
    std::memcpy(ico.data() + 10, &entryPlanes, 2);
    std::memcpy(ico.data() + 12, &entryBits, 2);
    const uint32_t bytesInRes = static_cast<uint32_t>(image.size()), offset = 22;
    std::memcpy(ico.data() + 14, &bytesInRes, 4);
    std::memcpy(ico.data() + 18, &offset, 4);
    ico.insert(ico.end(), image.begin(), image.end());
    return ico;
}

std::string queryString(const std::vector<uint8_t>& block, const wchar_t* name) {
    wchar_t query[128];
    wsprintfW(query, L"\\StringFileInfo\\040904B0\\%s", name);
    void* value = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(const_cast<uint8_t*>(block.data()), query, &value, &length) ||
        !value)
        return {};
    const std::wstring wide(static_cast<wchar_t*>(value));
    std::string out;
    for (wchar_t c : wide) out.push_back(static_cast<char>(c));
    return out;
}

void testApplyMetadata(const char* selfPath) {
    const fs::path tmp = fs::temp_directory_path() / "SaidaExeMetadataTests";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const fs::path exe = tmp / "PatchedGame.exe";
    fs::copy_file(selfPath, exe);

    const fs::path icon = tmp / "game.ico";
    {
        const auto bytes = minimalIco();
        std::ofstream file(icon, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    saida::ExeMetadata meta;
    meta.productName = "Metadata Witness";
    meta.version = "1.2.3.4";
    meta.companyName = "Saida";
    meta.iconPath = icon.string();

    std::string error;
    assert(saida::applyExeMetadata(exe.string(), meta, error));
    assert(error.empty());

    // Read VERSIONINFO back.
    const std::wstring wideExe = exe.wstring();
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(wideExe.c_str(), &ignored);
    assert(size > 0);
    std::vector<uint8_t> block(size);
    assert(::GetFileVersionInfoW(wideExe.c_str(), 0, size, block.data()));

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLen = 0;
    assert(::VerQueryValueW(block.data(), L"\\",
                            reinterpret_cast<void**>(&fixed), &fixedLen));
    assert(fixed && fixed->dwSignature == 0xFEEF04BDu);
    assert(HIWORD(fixed->dwFileVersionMS) == 1);
    assert(LOWORD(fixed->dwFileVersionMS) == 2);
    assert(HIWORD(fixed->dwFileVersionLS) == 3);
    assert(LOWORD(fixed->dwFileVersionLS) == 4);

    assert(queryString(block, L"ProductName") == "Metadata Witness");
    assert(queryString(block, L"CompanyName") == "Saida");
    assert(queryString(block, L"FileVersion") == "1.2.3.4");
    assert(queryString(block, L"OriginalFilename") == "PatchedGame.exe");

    // Read the icon back: icon group id 1 must exist.
    HMODULE module = ::LoadLibraryExW(wideExe.c_str(), nullptr,
                                      LOAD_LIBRARY_AS_DATAFILE);
    assert(module);
    HRSRC groupIcon = ::FindResourceW(module, MAKEINTRESOURCEW(1),
                                      MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */);
    assert(groupIcon);
    ::FreeLibrary(module);

    // Invalid version -> explicit refusal.
    saida::ExeMetadata bad = meta;
    bad.version = "not-a-version";
    assert(!saida::applyExeMetadata(exe.string(), bad, error));
    assert(!error.empty());

    // Missing icon -> explicit refusal.
    saida::ExeMetadata missingIcon = meta;
    missingIcon.iconPath = (tmp / "missing.ico").string();
    assert(!saida::applyExeMetadata(exe.string(), missingIcon, error));

    fs::remove_all(tmp);
}

#endif // _WIN32

} // namespace

int main(int, char** argv) {
    testParseVersion();
#ifdef _WIN32
    testApplyMetadata(argv[0]);
#else
    (void)argv;
#endif
    return 0;
}
