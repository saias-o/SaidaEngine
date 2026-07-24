#include "core/CrashReporter.hpp"
#include "core/Log.hpp"

#include <cassert>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

void setCrashDirectory(const fs::path& path) {
#ifdef _WIN32
    _putenv_s("SAIDA_CRASH_DIR", path.string().c_str());
#else
    setenv("SAIDA_CRASH_DIR", path.string().c_str(), 1);
#endif
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argc, char** argv) {
    const fs::path output =
        fs::temp_directory_path() / "saida-crash-reporter-tests";
    if (argc == 2 && std::string(argv[1]) == "--crash-child") {
        saida::crash::install("Crash Reporter Test");
        saida::Log::info("crash-reporter-child-marker");
#ifdef _WIN32
        ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE,
                         0, nullptr);
#else
        std::terminate();
#endif
        return 99;
    }

    std::error_code error;
    fs::remove_all(output, error);
    fs::create_directories(output);
    setCrashDirectory(output);

    saida::crash::install("Crash Reporter Test");
    assert(std::string(saida::crash::buildCommit()) != "unknown");
    saida::Log::info("crash-reporter-test-marker");
    const saida::crash::Report report =
        saida::crash::writeFatalReport("synthetic fatal error");

    assert(!report.logPath.empty());
    assert(fs::is_regular_file(report.logPath));
    const std::string log = readText(report.logPath);
    assert(log.find("schema=1") != std::string::npos);
    assert(log.find("product=Crash_Reporter_Test") != std::string::npos);
    assert(log.find(std::string("engineCommit=") +
                    saida::crash::buildCommit()) != std::string::npos);
    assert(log.find(std::string("symbolsArtifact=windows-symbols-") +
                    saida::crash::buildCommit()) != std::string::npos);
    assert(log.find("reason=synthetic fatal error") != std::string::npos);
    assert(log.find("crash-reporter-test-marker") != std::string::npos);
#ifdef _WIN32
    assert(report.dumpWritten);
    assert(fs::is_regular_file(report.dumpPath));
    assert(fs::file_size(report.dumpPath) > 0);
    assert(log.find("moduleBase=0x") != std::string::npos);
    assert(log.find("exceptionRva=0x") != std::string::npos);
#endif

    const fs::path executable = fs::absolute(argv[0]);
    const std::string command =
        "\"" + executable.string() + "\" --crash-child";
    assert(std::system(command.c_str()) != 0);

    bool foundUnhandled = false;
    std::size_t logCount = 0;
    std::size_t dumpCount = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(output)) {
        if (entry.path().extension() == ".log") {
            ++logCount;
            const std::string childLog = readText(entry.path());
#ifdef _WIN32
            if (childLog.find("reason=unhandled Windows exception") !=
                std::string::npos)
                foundUnhandled = true;
#else
            if (childLog.find("reason=std::terminate") != std::string::npos)
                foundUnhandled = true;
#endif
        } else if (entry.path().extension() == ".dmp") {
            ++dumpCount;
        }
    }
    assert(foundUnhandled);
    assert(logCount == 2);
#ifdef _WIN32
    assert(dumpCount == 2);
#endif

    fs::remove_all(output, error);
    return 0;
}
