#pragma once

#include <filesystem>
#include <string>

namespace saida::crash {

struct Report {
    std::filesystem::path logPath;
    std::filesystem::path dumpPath;
    bool dumpWritten = false;
};

// Installs process-wide fatal handlers. Desktop entry points call this before
// constructing any engine subsystem so startup failures are captured too.
void install(const std::string& productName);

// Writes a diagnostic for a fatal exception caught at an application boundary.
// On Windows this also writes a minidump. SAIDA_CRASH_DIR overrides the default
// per-user report directory and is used by tests/automation.
Report writeFatalReport(const std::string& reason) noexcept;

const char* buildCommit() noexcept;

} // namespace saida::crash
