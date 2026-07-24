#include "editor/ExeMetadata.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace saida {

bool parseExeVersion(const std::string& version, unsigned short out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (version.empty()) return false;
    size_t begin = 0;
    int field = 0;
    while (begin <= version.size()) {
        if (field >= 4) return false;
        const size_t dot = version.find('.', begin);
        const std::string part = version.substr(
            begin, dot == std::string::npos ? std::string::npos : dot - begin);
        if (part.empty() || part.size() > 5) return false;
        unsigned long value = 0;
        for (char c : part) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + static_cast<unsigned long>(c - '0');
        }
        if (value > 65535) return false;
        out[field++] = static_cast<unsigned short>(value);
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    return true;
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int count = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                            static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(count), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                          wide.data(), count);
    return wide;
}

// VERSIONINFO block builder: {wLength, wValueLength, wType, szKey, padding,
// value/children} structures aligned to 4 bytes, lengths patched retroactively.
class VersionBlockWriter {
public:
    void u16(uint16_t v) {
        bytes_.push_back(static_cast<uint8_t>(v & 0xff));
        bytes_.push_back(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        u16(static_cast<uint16_t>(v & 0xffff));
        u16(static_cast<uint16_t>(v >> 16));
    }
    void wsz(const std::wstring& text) {
        for (wchar_t c : text) u16(static_cast<uint16_t>(c));
        u16(0);
    }
    void pad4() {
        while (bytes_.size() % 4) bytes_.push_back(0);
    }

    size_t beginBlock(const std::wstring& key, uint16_t type) {
        pad4();
        const size_t at = bytes_.size();
        u16(0);     // wLength, patched retroactively by endBlock
        u16(0);     // wValueLength, patched retroactively by setValueLength
        u16(type);  // 0 = binary, 1 = text
        wsz(key);
        pad4();
        return at;
    }
    void endBlock(size_t at) {
        const uint16_t length = static_cast<uint16_t>(bytes_.size() - at);
        bytes_[at] = static_cast<uint8_t>(length & 0xff);
        bytes_[at + 1] = static_cast<uint8_t>(length >> 8);
    }
    void setValueLength(size_t at, uint16_t valueLength) {
        bytes_[at + 2] = static_cast<uint8_t>(valueLength & 0xff);
        bytes_[at + 3] = static_cast<uint8_t>(valueLength >> 8);
    }

    void stringEntry(const std::wstring& key, const std::wstring& value) {
        const size_t at = beginBlock(key, 1);
        wsz(value);
        // A String's wValueLength counts WCHARs, including the terminator.
        setValueLength(at, static_cast<uint16_t>(value.size() + 1));
        endBlock(at);
    }

    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

// Writes the complete VERSIONINFO resource.
std::vector<uint8_t> makeVersionInfo(const ExeMetadata& meta,
                                     const unsigned short v[4],
                                     const std::wstring& originalFilename) {
    VersionBlockWriter w;
    const size_t root = w.beginBlock(L"VS_VERSION_INFO", 0);

    VS_FIXEDFILEINFO fixed{};
    fixed.dwSignature = 0xFEEF04BDu;
    fixed.dwStrucVersion = 0x00010000u;
    fixed.dwFileVersionMS = (uint32_t(v[0]) << 16) | v[1];
    fixed.dwFileVersionLS = (uint32_t(v[2]) << 16) | v[3];
    fixed.dwProductVersionMS = fixed.dwFileVersionMS;
    fixed.dwProductVersionLS = fixed.dwFileVersionLS;
    fixed.dwFileFlagsMask = 0x3F;
    fixed.dwFileOS = VOS_NT_WINDOWS32;
    fixed.dwFileType = VFT_APP;

    const uint16_t* words = reinterpret_cast<const uint16_t*>(&fixed);
    for (size_t i = 0; i < sizeof(fixed) / 2; ++i) w.u16(words[i]);
    w.setValueLength(root, sizeof(fixed));

    const std::wstring fileVersion = widen(meta.version);
    const std::wstring productName = widen(meta.productName);

    const size_t sfi = w.beginBlock(L"StringFileInfo", 1);
    {
        // 0409 = en-US, 04B0 = Unicode: the standard table for exes.
        const size_t table = w.beginBlock(L"040904B0", 1);
        if (!meta.companyName.empty())
            w.stringEntry(L"CompanyName", widen(meta.companyName));
        w.stringEntry(L"FileDescription", productName);
        w.stringEntry(L"FileVersion", fileVersion);
        w.stringEntry(L"InternalName", productName);
        w.stringEntry(L"OriginalFilename", originalFilename);
        w.stringEntry(L"ProductName", productName);
        w.stringEntry(L"ProductVersion", fileVersion);
        w.endBlock(table);
    }
    w.endBlock(sfi);

    const size_t vfi = w.beginBlock(L"VarFileInfo", 1);
    {
        const size_t translation = w.beginBlock(L"Translation", 0);
        w.u16(0x0409);
        w.u16(0x04B0);
        w.setValueLength(translation, 4);
        w.endBlock(translation);
    }
    w.endBlock(vfi);

    w.endBlock(root);
    return w.bytes();
}

#pragma pack(push, 2)
struct IconDirEntry {
    uint8_t width, height, colorCount, reserved;
    uint16_t planes, bitCount;
    uint32_t bytesInRes, imageOffset;
};
struct GroupIconDirEntry {
    uint8_t width, height, colorCount, reserved;
    uint16_t planes, bitCount;
    uint32_t bytesInRes;
    uint16_t id;
};
#pragma pack(pop)

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size <= 0) return false;
    out.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

constexpr WORD kLangEnUs = 0x0409;

// Wide equivalents of the RT_* macros (ANSI variants without UNICODE defined).
const LPCWSTR kRtIcon = MAKEINTRESOURCEW(3);        // RT_ICON
const LPCWSTR kRtGroupIcon = MAKEINTRESOURCEW(14);  // RT_GROUP_ICON
const LPCWSTR kRtVersion = MAKEINTRESOURCEW(16);    // RT_VERSION

bool updateIcon(HANDLE update, const std::string& iconPath, std::string& error) {
    std::vector<uint8_t> ico;
    if (!readFile(iconPath, ico) || ico.size() < 6) {
        error = "cannot read icon file: " + iconPath;
        return false;
    }
    uint16_t reserved = 0, type = 0, count = 0;
    std::memcpy(&reserved, ico.data(), 2);
    std::memcpy(&type, ico.data() + 2, 2);
    std::memcpy(&count, ico.data() + 4, 2);
    if (reserved != 0 || type != 1 || count == 0 ||
        ico.size() < 6 + size_t(count) * sizeof(IconDirEntry)) {
        error = "not a valid .ico file: " + iconPath;
        return false;
    }

    std::vector<uint8_t> group(6 + size_t(count) * sizeof(GroupIconDirEntry));
    std::memcpy(group.data(), ico.data(), 6);  // same header (reserved/type/count)

    for (uint16_t i = 0; i < count; ++i) {
        IconDirEntry entry{};
        std::memcpy(&entry, ico.data() + 6 + size_t(i) * sizeof(IconDirEntry),
                    sizeof(entry));
        if (size_t(entry.imageOffset) + entry.bytesInRes > ico.size()) {
            error = "corrupt .ico entry: " + iconPath;
            return false;
        }
        if (!::UpdateResourceW(update, kRtIcon, MAKEINTRESOURCEW(i + 1), kLangEnUs,
                               ico.data() + entry.imageOffset, entry.bytesInRes)) {
            error = "UpdateResource(RT_ICON) failed";
            return false;
        }
        GroupIconDirEntry grp{};
        std::memcpy(&grp, &entry, 12);  // common fields (up to bytesInRes)
        grp.id = static_cast<uint16_t>(i + 1);
        std::memcpy(group.data() + 6 + size_t(i) * sizeof(GroupIconDirEntry), &grp,
                    sizeof(grp));
    }

    if (!::UpdateResourceW(update, kRtGroupIcon, MAKEINTRESOURCEW(1), kLangEnUs,
                           group.data(), static_cast<DWORD>(group.size()))) {
        error = "UpdateResource(RT_GROUP_ICON) failed";
        return false;
    }
    return true;
}

} // namespace

bool applyExeMetadata(const std::string& exePath, const ExeMetadata& meta,
                      std::string& error) {
    unsigned short v[4];
    if (!parseExeVersion(meta.version, v)) {
        error = "invalid version '" + meta.version + "' (expected a.b.c or a.b.c.d)";
        return false;
    }

    const std::wstring wideExe = widen(exePath);
    std::wstring originalFilename = wideExe;
    if (const size_t slash = originalFilename.find_last_of(L"/\\");
        slash != std::wstring::npos)
        originalFilename = originalFilename.substr(slash + 1);

    const std::vector<uint8_t> versionInfo = makeVersionInfo(meta, v, originalFilename);

    HANDLE update = ::BeginUpdateResourceW(wideExe.c_str(), FALSE);
    if (!update) {
        error = "BeginUpdateResource failed (exe missing or in use): " + exePath;
        return false;
    }

    bool ok = ::UpdateResourceW(update, kRtVersion, MAKEINTRESOURCEW(1), kLangEnUs,
                                const_cast<uint8_t*>(versionInfo.data()),
                                static_cast<DWORD>(versionInfo.size())) != FALSE;
    if (!ok) error = "UpdateResource(RT_VERSION) failed";

    if (ok && !meta.iconPath.empty()) ok = updateIcon(update, meta.iconPath, error);

    if (!::EndUpdateResourceW(update, /*fDiscard=*/!ok)) {
        if (ok) error = "EndUpdateResource failed: " + exePath;
        return false;
    }
    return ok;
}

#else // !_WIN32

bool applyExeMetadata(const std::string&, const ExeMetadata&, std::string&) {
    return true;  // no Windows exe to patch on this platform
}

#endif

} // namespace saida
