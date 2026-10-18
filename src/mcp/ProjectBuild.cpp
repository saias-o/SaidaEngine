#include "mcp/ProjectBuild.hpp"

#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace saida::mcp {
namespace {
void appendTail(std::string& tail, const char* data, size_t size) {
    constexpr size_t limit = 6000;
    tail.append(data, size);
    if (tail.size() > limit) tail.erase(0, tail.size() - limit);
}
#ifdef _WIN32
std::wstring quote(const std::wstring& value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    return out + L'\"';
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
#endif
} // namespace

ProjectBuildResult buildNativeProject(const std::string& directory) {
    ProjectBuildResult result;
    char buffer[4096];
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle read, write;
    if (!CreatePipe(&read.value, &write.value, &security, 0) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("cannot create compiler output pipe");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = write.value;
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) {
        input.value = nullptr;
        throw std::runtime_error("cannot open compiler input handle");
    }
    startup.hStdInput = input.value;
    PROCESS_INFORMATION process{};
    const auto cmake = std::filesystem::path(SAIDA_CMAKE_COMMAND).wstring();
    auto command = quote(cmake) + L" --build " + quote(std::filesystem::path(directory).wstring()) + L" --parallel 2";
    // Direct argv launch: project paths containing shell metacharacters are data.
    if (!CreateProcessW(cmake.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("cannot launch CMake; install the native build toolchain");
    Handle child{process.hProcess}, thread{process.hThread};
    CloseHandle(write.value); write.value = nullptr;
    DWORD count;
    while (ReadFile(read.value, buffer, sizeof(buffer), &count, nullptr) && count)
        appendTail(result.output, buffer, count);
    WaitForSingleObject(child.value, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.value, &code);
    result.exitCode = static_cast<int>(code);
#else
    int descriptors[2];
    if (pipe(descriptors)) throw std::runtime_error("cannot create compiler output pipe");
    const pid_t child = fork();
    if (child == 0) {
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO); dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        execl(SAIDA_CMAKE_COMMAND, SAIDA_CMAKE_COMMAND, "--build", directory.c_str(),
              "--parallel", "2", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) { close(descriptors[0]); throw std::runtime_error("cannot launch CMake"); }
    ssize_t count;
    for (;;) {
        count = ::read(descriptors[0], buffer, sizeof(buffer));
        if (count > 0) appendTail(result.output, buffer, size_t(count));
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
    close(descriptors[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return result;
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return result;
}
} // namespace saida::mcp
