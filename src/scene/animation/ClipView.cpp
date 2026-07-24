#include "scene/animation/ClipView.hpp"

#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/ClipNode.hpp"

#include <cmath>
#include <fstream>

namespace saida {

namespace {

using json = nlohmann::json;

AssetDiagnostic error(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Error,
            std::move(path), std::move(message)};
}

AssetDiagnostic warning(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Warning,
            std::move(path), std::move(message)};
}

bool finiteNumber(const json& j) {
    return j.is_number() && std::isfinite(j.get<double>());
}

} // namespace

nlohmann::json AssetDiagnostic::toJson() const {
    return {{"code", code},
            {"severity", severity == Severity::Error ? "error" : "warning"},
            {"path", jsonPath},
            {"message", message}};
}

bool hasErrors(const std::vector<AssetDiagnostic>& diagnostics) {
    for (const auto& d : diagnostics)
        if (d.severity == AssetDiagnostic::Severity::Error) return true;
    return false;
}

ClipViewParseResult ClipView::parse(const nlohmann::json& input) {
    ClipViewParseResult result;
    auto& diags = result.diagnostics;

    if (!input.is_object()) {
        diags.push_back(error("clipview.root.not_object", "", "document must be a JSON object"));
        return result;
    }

    json j = input;

    const int schema = j.value("schema", 0);
    if (schema <= 0) {
        diags.push_back(error("clipview.schema.missing", "/schema",
                              "'schema' must be a positive integer"));
        return result;
    }
    if (schema != kClipViewSchema) {
        diags.push_back(error("clipview.schema.unsupported", "/schema",
                              "unsupported schema " + std::to_string(schema) +
                                  " (expected " + std::to_string(kClipViewSchema) + ")"));
        return result;
    }

    ClipView& v = result.view;

    if (!j.contains("source") || !j["source"].is_string() || j["source"].get<std::string>().empty()) {
        diags.push_back(error("clipview.source.missing", "/source",
                              "'source' must be a non-empty sub-asset key (\"file#clip\")"));
    } else {
        v.source = j["source"].get<std::string>();
    }

    if (!j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()) {
        diags.push_back(error("clipview.name.missing", "/name", "'name' must be a non-empty string"));
    } else {
        v.name = j["name"].get<std::string>();
    }

    if (j.contains("range")) {
        const json& r = j["range"];
        if (!r.is_object() || !finiteNumber(r.value("start", json())) ||
            !finiteNumber(r.value("end", json()))) {
            diags.push_back(error("clipview.range.malformed", "/range",
                                  "'range' must be {start:number, end:number}"));
        } else {
            v.hasRange = true;
            v.rangeStart = r["start"].get<float>();
            v.rangeEnd = r["end"].get<float>();
        }
    }

    if (j.contains("loop")) {
        if (!j["loop"].is_boolean())
            diags.push_back(error("clipview.loop.malformed", "/loop", "'loop' must be a boolean"));
        else
            v.loop = j["loop"].get<bool>();
    }

    if (j.contains("speed")) {
        if (!finiteNumber(j["speed"]))
            diags.push_back(error("clipview.speed.malformed", "/speed", "'speed' must be a finite number"));
        else
            v.speed = j["speed"].get<float>();
    }

    if (j.contains("events")) {
        if (!j["events"].is_array()) {
            diags.push_back(error("clipview.events.malformed", "/events", "'events' must be an array"));
        } else {
            size_t i = 0;
            for (const json& e : j["events"]) {
                const std::string path = "/events/" + std::to_string(i++);
                if (!e.is_object() || !finiteNumber(e.value("time", json())) ||
                    !e.contains("name") || !e["name"].is_string() ||
                    e["name"].get<std::string>().empty()) {
                    diags.push_back(error("clipview.event.malformed", path,
                                          "event must be {time:number, name:string}"));
                    continue;
                }
                v.events.push_back({e["time"].get<float>(), e["name"].get<std::string>()});
            }
        }
    }

    result.ok = !hasErrors(diags);
    return result;
}

ClipViewParseResult ClipView::loadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        ClipViewParseResult result;
        result.diagnostics.push_back(error("clipview.io.open", "", "cannot open " + path));
        return result;
    }
    json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        ClipViewParseResult result;
        result.diagnostics.push_back(error("clipview.io.json", "", path + " is not valid JSON"));
        return result;
    }
    return parse(j);
}

nlohmann::json ClipView::toJson() const {
    json j = {{"schema", kClipViewSchema}, {"source", source}, {"name", name},
              {"loop", loop}, {"speed", speed}};
    if (hasRange) j["range"] = {{"start", rangeStart}, {"end", rangeEnd}};
    if (!events.empty()) {
        json arr = json::array();
        for (const auto& e : events) arr.push_back({{"time", e.time}, {"name", e.name}});
        j["events"] = arr;
    }
    return j;
}

bool ClipView::saveFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << toJson().dump(1) << "\n";
    return file.good();
}

float ClipView::effectiveEnd(const AnimationClip& sourceClip) const {
    return hasRange ? rangeEnd : sourceClip.duration();
}

std::vector<AssetDiagnostic> ClipView::validate(const AnimationClip* sourceClip) const {
    std::vector<AssetDiagnostic> diags;

    if (speed == 0.0f)
        diags.push_back(warning("clipview.speed.zero", "/speed", "speed 0 freezes playback"));

    if (hasRange && rangeEnd <= rangeStart)
        diags.push_back(error("clipview.range.reversed", "/range/end",
                              "'end' must be greater than 'start'"));
    if (hasRange && rangeStart < 0.0f)
        diags.push_back(error("clipview.range.negative", "/range/start",
                              "'start' must be >= 0"));

    if (sourceClip) {
        const float duration = sourceClip->duration();
        if (hasRange && rangeEnd > duration)
            diags.push_back(error("clipview.range.past_end", "/range/end",
                                  "'end' (" + std::to_string(rangeEnd) + ") exceeds source duration (" +
                                      std::to_string(duration) + ")"));

        const float start = effectiveStart();
        const float end = hasRange ? rangeEnd : duration;
        size_t i = 0;
        for (const auto& e : events) {
            const std::string path = "/events/" + std::to_string(i++);
            if (e.time < start || e.time > end)
                diags.push_back(error("clipview.event.out_of_range", path + "/time",
                                      "event '" + e.name + "' at " + std::to_string(e.time) +
                                          "s is outside the view range"));
        }
    }

    return diags;
}

std::unique_ptr<ClipNode> ClipView::instantiate(const AnimationClip& sourceClip,
                                                const Rig& rig) const {
    auto node = std::make_unique<ClipNode>(&sourceClip, rig);
    if (hasRange) node->setRange(rangeStart, rangeEnd);
    node->setLooping(loop);
    node->setPlaybackSpeed(speed);
    node->setTime(effectiveStart());
    node->setEvents(events);
    return node;
}

} // namespace saida
