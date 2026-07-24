#include "Hub.hpp"
#include "core/CrashReporter.hpp"
#include "core/Log.hpp"

#include <cstdlib>
#include <exception>

int main() {
    saida::crash::install("SaidaEngineHub");
    try {
        saida::Hub hub;
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
