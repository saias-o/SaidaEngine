#include "Hub.hpp"
#include "core/CrashReporter.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"

#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char** argv) {
    saida::initializeInstalledLayout();
    saida::crash::install("SaidaEngineHub");
    try {
        saida::Hub hub;
        if (argc == 2 && std::string(argv[1]) == "--verify-installation")
            return EXIT_SUCCESS;
        hub.run();
    } catch (const std::exception& e) {
        const auto report =
            saida::crash::writeFatalReport(std::string("fatal exception: ") + e.what());
        saida::Log::error(e.what());
        if (!report.logPath.empty())
            saida::Log::error("crash report: ", report.logPath.string());
        return EXIT_FAILURE;
    } catch (...) {
        const auto report =
            saida::crash::writeFatalReport("fatal non-standard exception");
        saida::Log::error("fatal non-standard exception");
        if (!report.logPath.empty())
            saida::Log::error("crash report: ", report.logPath.string());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
