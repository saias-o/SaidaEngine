// Profiler::summarize and the --profile flag: a run reduced to where it spent
// its time, and the one flag that asks for it.
#include "core/Profiler.hpp"
#include "runtime/ProfileArgs.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "[profiler summary] FAIL: %s\n", what);
    ++failures;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

saida::ProfileFrame frame(uint64_t index, double ms, std::vector<saida::ProfileEvent> events,
                          std::vector<saida::ProfileCounter> counters = {}) {
    saida::ProfileFrame f;
    f.index = index;
    f.cpuFrameMs = ms;
    f.events = std::move(events);
    f.counters = std::move(counters);
    return f;
}

saida::ProfileEvent event(const char* name, double start, double end, uint32_t depth = 0) {
    saida::ProfileEvent e;
    e.name = name;
    e.threadName = "Main";
    e.startMs = start;
    e.endMs = end;
    e.depth = depth;
    return e;
}

void testScopesAreSummedAveragedAndRanked() {
    const std::vector<saida::ProfileFrame> frames = {
        frame(1, 10.0, {event("Frame", 0, 10), event("Animate", 1, 4, 1), event("Animate", 5, 6, 1)},
              {{"Animators", 3}}),
        frame(2, 20.0, {event("Frame", 0, 20), event("Draw", 2, 14, 1)}, {{"Animators", 7}}),
    };
    const saida::ProfileSummary s = saida::Profiler::summarize(frames);
    require(s.frames == 2, "two frames");
    require(near(s.averageFrameMs, 15.0), "average frame");
    require(near(s.worstFrameMs, 20.0), "worst frame");
    require(s.scopes.size() == 3, "one entry per thread, name and depth");
    require(s.scopes[0].name == "Frame" && near(s.scopes[0].totalMs, 30.0), "the most expensive first");
    require(s.scopes[1].name == "Draw" && near(s.scopes[1].averageMs, 6.0),
            "the average counts the frames a scope was absent from");
    require(s.scopes[2].name == "Animate" && s.scopes[2].calls == 2 && near(s.scopes[2].totalMs, 4.0),
            "two calls in one frame are summed");
    require(s.counterPeaks.size() == 1 && near(s.counterPeaks[0].value, 7.0) &&
                std::string(s.counterPeaks[0].name) == "Animators",
            "a counter's peak");
}

void testNothingRecordedSummarizesToNothing() {
    const saida::ProfileSummary s = saida::Profiler::summarize({});
    require(s.frames == 0 && s.scopes.empty() && s.averageFrameMs == 0.0, "an empty run");
}

void testTheFlag() {
    std::string path, error;
    char exe[] = "game", flag[] = "--profile", json[] = "run.json", png[] = "run.png", other[] = "--smoke";
    char* none[] = {exe, other};
    require(saida::runtime::parseProfileArgs(2, none, path, error) && path.empty(), "no flag, no profile");
    char* good[] = {exe, other, flag, json};
    require(saida::runtime::parseProfileArgs(4, good, path, error) && path == "run.json", "a trace path");
    path.clear();
    char* bad[] = {exe, flag, png};
    require(!saida::runtime::parseProfileArgs(3, bad, path, error) && !error.empty(), "not a trace");
    char* missing[] = {exe, flag};
    require(!saida::runtime::parseProfileArgs(2, missing, path, error), "a flag without its value");
}

} // namespace

int main() {
    testScopesAreSummedAveragedAndRanked();
    testNothingRecordedSummarizesToNothing();
    testTheFlag();
    if (failures) return EXIT_FAILURE;
    std::puts("saida_profiler_summary_tests: OK");
    return EXIT_SUCCESS;
}
