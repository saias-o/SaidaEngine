#include "scene/animation/AnimationAssetDecoders.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace saida {

namespace {

nlohmann::json parseJson(const std::vector<uint8_t>& bytes) {
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr,
                                 /*allow_exceptions=*/false);
}

std::string firstError(const std::vector<AssetDiagnostic>& diagnostics,
                       const char* fallback) {
    for (const AssetDiagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == AssetDiagnostic::Severity::Error)
            return "[" + diagnostic.code + "] " + diagnostic.message;
    }
    return fallback;
}

template <typename Payload>
uint64_t residentEstimate(const std::vector<uint8_t>& source) {
    // Documents are small JSON. Keeping at least their source size covers
    // the usual string allocations and avoids a zero accounting; sizeof
    // covers minimal documents.
    return std::max<uint64_t>(sizeof(Payload), source.size());
}

} // namespace

AssetDecoder makeRigAssetDecoder() {
    return [](std::vector<uint8_t>&& bytes, AssetDecodeResult& out, std::string& error) {
        const nlohmann::json json = parseJson(bytes);
        if (json.is_discarded()) {
            error = "invalid .srig JSON";
            return false;
        }
        RigAssetParseResult parsed = RigAsset::parse(json);
        if (!parsed.ok) {
            error = firstError(parsed.diagnostics, "invalid .srig document");
            return false;
        }
        auto payload = std::make_shared<DecodedRigAsset>();
        payload->asset = std::move(parsed.asset);
        payload->diagnostics = std::move(parsed.diagnostics);
        out.bytes = residentEstimate<DecodedRigAsset>(bytes);
        out.payload = std::move(payload);
        return true;
    };
}

AssetDecoder makeClipViewDecoder() {
    return [](std::vector<uint8_t>&& bytes, AssetDecodeResult& out, std::string& error) {
        const nlohmann::json json = parseJson(bytes);
        if (json.is_discarded()) {
            error = "invalid .sclip JSON";
            return false;
        }
        ClipViewParseResult parsed = ClipView::parse(json);
        if (!parsed.ok) {
            error = firstError(parsed.diagnostics, "invalid .sclip document");
            return false;
        }
        auto payload = std::make_shared<DecodedClipView>();
        payload->view = std::move(parsed.view);
        payload->diagnostics = std::move(parsed.diagnostics);
        out.bytes = residentEstimate<DecodedClipView>(bytes);
        out.payload = std::move(payload);
        return true;
    };
}

AssetDecoder makeAnimGraphDecoder() {
    return [](std::vector<uint8_t>&& bytes, AssetDecodeResult& out, std::string& error) {
        const nlohmann::json json = parseJson(bytes);
        if (json.is_discarded()) {
            error = "invalid .sgraph JSON";
            return false;
        }
        AnimGraphParseResult parsed = AnimGraphAsset::parse(json);
        if (!parsed.ok) {
            error = firstError(parsed.diagnostics, "invalid .sgraph document");
            return false;
        }
        std::vector<AssetDiagnostic> validation = parsed.graph.validate();
        parsed.diagnostics.insert(parsed.diagnostics.end(),
                                  std::make_move_iterator(validation.begin()),
                                  std::make_move_iterator(validation.end()));
        if (hasErrors(parsed.diagnostics)) {
            error = firstError(parsed.diagnostics, "invalid .sgraph document");
            return false;
        }
        auto payload = std::make_shared<DecodedAnimGraph>();
        payload->graph = std::move(parsed.graph);
        payload->diagnostics = std::move(parsed.diagnostics);
        out.bytes = residentEstimate<DecodedAnimGraph>(bytes);
        out.payload = std::move(payload);
        return true;
    };
}

} // namespace saida
