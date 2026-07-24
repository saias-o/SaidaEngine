#pragma once

#include <string>

namespace saida {

// Executable metadata written into the packaged <Game>.exe by the Build:
// VERSIONINFO resource (product name, version, publisher) and the main
// icon from an .ico file. Windows only — succeeds as a no-op elsewhere
// (web export has no exe).
struct ExeMetadata {
    std::string productName;   // FileDescription + ProductName (the game's name)
    std::string version;       // "1.2.3" or "1.2.3.4" — each field <= 65535
    std::string companyName;   // optional
    std::string iconPath;      // absolute .ico path, optional (empty = no icon)
};

// Patches the exe in place (UpdateResource). Returns false with `error` set
// if the version fails to parse, the .ico is invalid, or writing the
// resources fails. The exe must not be running.
bool applyExeMetadata(const std::string& exePath, const ExeMetadata& meta,
                      std::string& error);

// Parses "a.b.c.d" (1 to 4 numeric fields <= 65535) into out[4] (missing =
// 0). Exposed for UI validation and tests.
bool parseExeVersion(const std::string& version, unsigned short out[4]);

} // namespace saida
