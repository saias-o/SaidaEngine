#include "core/CrashReporter.hpp"

#include "core/Log.hpp"
#include "saida/BuildIdentity.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#else
#include <unistd.h>
#endif

namespace saida::crash {
namespace {

namespace fs = std::filesystem;

std::string gProduct = "SaidaEngine";
std::atomic<unsigned long long> gSequence{0};
std::atomic_flag gWriting = ATOMIC_FLAG_INIT;

std::string safeComponent(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            out.push_back(static_cast<char>(c));
        else if (c == ' ')
            out.push_back('_');
    }
    if (out.empty()) out = "SaidaEngine";
    return out;
}

const char* envOrNull(const char* name) {
    const char* value = std::getenv(name);
    return (value && *value) ? value : nullptr;
}

fs::path reportRoot() {
    if (const char* overrideDir = envOrNull("SAIDA_CRASH_DIR"))
        return fs::path(overrideDir);
#ifdef _WIN32
    if (const char* local = envOrNull("LOCALAPPDATA"))
        return fs::path(local) / "SaidaEngine" / "CrashReports" /
               safeComponent(gProduct);
    if (const char* roaming = envOrNull("APPDATA"))
        return fs::path(roaming) / "SaidaEngine" / "CrashReports" /
               safeComponent(gProduct);
#else
    if (const char* state = envOrNull("XDG_STATE_HOME"))
        return fs::path(state) / "SaidaEngine" / "CrashReports" /
               safeComponent(gProduct);
    if (const char* home = envOrNull("HOME"))
        return fs::path(home) / ".local" / "state" / "SaidaEngine" /
               "CrashReports" / safeComponent(gProduct);
#endif
    return fs::temp_directory_path() / "SaidaEngine" / "CrashReports" /
           safeComponent(gProduct);
}

struct Timestamp {
    std::string id;
    std::string iso;
};

Timestamp timestampUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() %
        1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream id;
    id << std::put_time(&utc, "%Y%m%dT%H%M%S") << '.' << std::setw(3)
       << std::setfill('0') << millis << 'Z';
    std::ostringstream iso;
    iso << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
        << std::setfill('0') << millis << 'Z';
    return {id.str(), iso.str()};
}

unsigned long processId() {
#ifdef _WIN32
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

unsigned long long threadId() {
#ifdef _WIN32
    return ::GetCurrentThreadId();
#else
    return static_cast<unsigned long long>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

std::string executablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD count = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count != 0 && count < buffer.size())
        return fs::path(std::wstring(buffer.data(), count)).generic_string();
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    const ssize_t count =
        ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (count > 0) {
        buffer[static_cast<std::size_t>(count)] = '\0';
        return std::string(buffer.data(), static_cast<std::size_t>(count));
    }
#endif
    std::error_code error;
    return fs::absolute(fs::current_path(), error).generic_string();
}

std::string singleLine(std::string value) {
    for (char& c : value) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    return value;
}

#ifdef _WIN32
struct NativeFailure {
    EXCEPTION_POINTERS* pointers = nullptr;
    DWORD code = 0;
    const void* address = nullptr;
};

bool writeMinidump(const fs::path& path, EXCEPTION_POINTERS* pointers,
                   DWORD& errorCode) {
    HANDLE file = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorCode = ::GetLastError();
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = ::GetCurrentThreadId();
    exception.ExceptionPointers = pointers;
    exception.ClientPointers = FALSE;
    const BOOL ok = ::MiniDumpWriteDump(
        ::GetCurrentProcess(), ::GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
        pointers ? &exception : nullptr, nullptr, nullptr);
    if (!ok) errorCode = ::GetLastError();
    ::CloseHandle(file);
    if (!ok) {
        std::error_code removeError;
        fs::remove(path, removeError);
    }
    return ok == TRUE;
}
#else
struct NativeFailure {};
#endif

Report writeReport(const std::string& reason,
                   const NativeFailure& native = {}) noexcept {
    Report result;
    if (gWriting.test_and_set()) return result;
    struct ClearFlag {
        ~ClearFlag() { gWriting.clear(); }
    } clearFlag;

    try {
        const fs::path root = reportRoot();
        std::error_code error;
        fs::create_directories(root, error);
        if (error) return result;

        const Timestamp stamp = timestampUtc();
        const unsigned long pid = processId();
        const unsigned long long tid = threadId();
        const unsigned long long sequence = gSequence.fetch_add(1);
        std::ostringstream baseName;
        baseName << safeComponent(gProduct) << '-' << stamp.id << "-p" << pid
                 << "-t" << tid << '-' << sequence;

        result.logPath = root / (baseName.str() + ".crash.log");
#ifdef _WIN32
        result.dumpPath = root / (baseName.str() + ".dmp");
        DWORD dumpError = ERROR_SUCCESS;
        result.dumpWritten =
            writeMinidump(result.dumpPath, native.pointers, dumpError);
#endif

        std::ofstream out(result.logPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.logPath.clear();
            return result;
        }
        out << "schema=1\n"
            << "product=" << safeComponent(gProduct) << '\n'
            << "engineCommit=" << buildCommit() << '\n'
            << "symbolsArtifact=windows-symbols-" << buildCommit() << '\n'
            << "timestampUtc=" << stamp.iso << '\n'
            << "processId=" << pid << '\n'
            << "threadId=" << tid << '\n'
            << "executable=" << singleLine(executablePath()) << '\n'
            << "reason=" << singleLine(reason) << '\n';
#ifdef _WIN32
        const std::uintptr_t moduleBase = reinterpret_cast<std::uintptr_t>(
            ::GetModuleHandleW(nullptr));
        const std::uintptr_t exceptionAddress =
            reinterpret_cast<std::uintptr_t>(native.address);
        const std::uintptr_t exceptionRva =
            exceptionAddress >= moduleBase ? exceptionAddress - moduleBase : 0;
        out << "exceptionCode=0x" << std::hex << std::uppercase << native.code
            << std::dec << '\n'
            << "exceptionAddress=" << native.address << '\n'
            << "moduleBase=0x" << std::hex << std::uppercase << moduleBase
            << '\n'
            << "exceptionRva=0x" << exceptionRva << std::dec << '\n'
            << "minidump="
            << (result.dumpWritten ? result.dumpPath.filename().generic_string()
                                   : "unavailable")
            << '\n'
            << "minidumpError="
            << (result.dumpWritten ? 0 : static_cast<unsigned long>(dumpError))
            << '\n';
#else
        out << "minidump=unsupported-on-this-platform\n";
#endif

        const std::vector<std::string> recent = Log::recentNonBlocking(128);
        out << "recentLogCount=" << recent.size() << "\n[recent-logs]\n";
        for (const std::string& line : recent)
            out << singleLine(line) << '\n';
        out.flush();
        if (!out) result.logPath.clear();
    } catch (...) {
        result.logPath.clear();
    }
    return result;
}

[[noreturn]] void terminateHandler() noexcept {
    std::string reason = "std::terminate";
    try {
        const std::exception_ptr pending = std::current_exception();
        if (pending) std::rethrow_exception(pending);
    } catch (const std::exception& error) {
        reason += ": ";
        reason += error.what();
    } catch (...) {
        reason += ": non-standard exception";
    }
    writeReport(reason);
    std::_Exit(EXIT_FAILURE);
}

#ifdef _WIN32
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pointers) noexcept {
    NativeFailure failure;
    failure.pointers = pointers;
    if (pointers && pointers->ExceptionRecord) {
        failure.code = pointers->ExceptionRecord->ExceptionCode;
        failure.address = pointers->ExceptionRecord->ExceptionAddress;
    }
    writeReport("unhandled Windows exception", failure);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace

void install(const std::string& productName) {
    gProduct = safeComponent(productName);
    std::set_terminate(terminateHandler);
#ifdef _WIN32
    ::SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif
}

Report writeFatalReport(const std::string& reason) noexcept {
    return writeReport(reason);
}

const char* buildCommit() noexcept { return SAIDA_BUILD_COMMIT; }

} // namespace saida::crash
