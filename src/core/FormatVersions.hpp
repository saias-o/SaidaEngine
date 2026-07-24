#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace saida::format {

constexpr int kBootManifestVersion = 1;
constexpr int kProjectVersion = 1;
constexpr int kAssetRegistryVersion = 1;
constexpr int kScenarioVersion = 1;
constexpr int kSceneVersion = 2;

inline void writeSchema(nlohmann::json& doc, int schema) {
    doc["schema"] = schema;
    doc["version"] = schema;
}

// Both fields are mandatory and must exactly match the current schema. The
// The product has no previously released formats, so loaders require the exact
// current schema.
inline std::string schemaEnvelopeError(const nlohmann::json& doc, int current,
                                       const char* label) {
    const std::string name = label;
    if (!doc.is_object()) return name + " document must be a JSON object";
    const auto schema = doc.find("schema");
    const auto version = doc.find("version");
    if (schema == doc.end()) return name + " schema is required";
    if (version == doc.end()) return name + " version is required";
    if (!schema->is_number_integer()) return name + " schema must be an integer";
    if (!version->is_number_integer()) return name + " version must be an integer";
    if (schema->get<int>() != version->get<int>())
        return name + " schema/version mismatch (schema " +
               std::to_string(schema->get<int>()) + ", version " +
               std::to_string(version->get<int>()) + ")";
    if (schema->get<int>() != current)
        return "unsupported " + name + " schema v" +
               std::to_string(schema->get<int>()) + " (expected v" +
               std::to_string(current) + ")";
    return "";
}

} // namespace saida::format
