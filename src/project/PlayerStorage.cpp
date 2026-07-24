#include "project/PlayerStorage.hpp"

#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/AtomicFile.hpp"
#include "core/FormatVersions.hpp"

namespace saida {
namespace {

constexpr const char* kMarker = "__saidaStore";

std::int64_t nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string readWhole(const std::filesystem::path& path, bool& ok) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ok = false;
        return {};
    }
    std::ostringstream content;
    content << file.rdbuf();
    ok = true;
    return content.str();
}

} // namespace

const char* toString(StorageStatus status) {
    switch (status) {
        case StorageStatus::Ok: return "ok";
        case StorageStatus::InvalidSlot: return "invalid_slot";
        case StorageStatus::QuotaExceeded: return "quota_exceeded";
        case StorageStatus::NotFound: return "not_found";
        case StorageStatus::Corrupt: return "corrupt";
        case StorageStatus::IoError: return "io_error";
    }
    return "ok";
}

const char* toString(StorageKind kind) {
    return kind == StorageKind::Preference ? "preference" : "progress";
}

bool PlayerStorage::validSlot(const std::string& slot) {
    if (slot.empty() || slot.size() > 64) return false;
    for (char c : slot) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') return false;
    }
    return true;
}

PlayerStorage::PlayerStorage(std::filesystem::path progressDir,
                             std::filesystem::path prefDir, StorageQuota quota)
    : progressDir_(std::move(progressDir)),
      prefDir_(std::move(prefDir)),
      quota_(quota) {}

std::filesystem::path PlayerStorage::dirFor(StorageKind kind) const {
    return kind == StorageKind::Preference ? prefDir_ : progressDir_;
}

std::filesystem::path PlayerStorage::pathFor(StorageKind kind,
                                             const std::string& slot) const {
    return dirFor(kind) / (slot + ".json");
}

std::int64_t PlayerStorage::namespaceBytes(StorageKind kind,
                                           const std::string& excludeSlot) const {
    std::error_code ec;
    const std::filesystem::path dir = dirFor(kind);
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    const std::string exclude = excludeSlot.empty() ? std::string()
                                                    : (excludeSlot + ".json");
    std::int64_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".json") continue;
        if (!exclude.empty() && entry.path().filename() == exclude) continue;
        total += static_cast<std::int64_t>(entry.file_size(ec));
    }
    return total;
}

StorageResult PlayerStorage::save(StorageKind kind, const std::string& slot,
                                  const std::string& payload, int dataVersion) {
    StorageResult result;
    result.meta.kind = kind;
    if (!validSlot(slot)) {
        result.status = StorageStatus::InvalidSlot;
        result.error = "slot must match [A-Za-z0-9_-]{1,64}";
        return result;
    }
    const auto payloadBytes = static_cast<std::int64_t>(payload.size());
    if (payloadBytes > quota_.maxSlotBytes) {
        result.status = StorageStatus::QuotaExceeded;
        result.error = "payload " + std::to_string(payloadBytes) +
                       " B exceeds per-slot limit " +
                       std::to_string(quota_.maxSlotBytes) + " B";
        return result;
    }

    nlohmann::json envelope;
    format::writeSchema(envelope, kSaveEnvelopeVersion);
    envelope[kMarker] = true;
    envelope["kind"] = toString(kind);
    envelope["dataVersion"] = dataVersion;
    envelope["savedAt"] = nowUnixSeconds();
    envelope["bytes"] = payloadBytes;
    envelope["payload"] = payload;
    const std::string serialized = envelope.dump();

    // Price this write against the namespace: replacing a slot only costs the
    // delta, and a brand-new slot must fit both the byte budget and slot count.
    const std::filesystem::path path = pathFor(kind, slot);
    std::error_code ec;
    const bool existed = std::filesystem::is_regular_file(path, ec);
    const std::int64_t othersBytes = namespaceBytes(kind, slot);
    if (othersBytes + static_cast<std::int64_t>(serialized.size()) >
        quota_.maxNamespaceBytes) {
        result.status = StorageStatus::QuotaExceeded;
        result.error = "namespace '" + std::string(toString(kind)) +
                       "' would exceed budget " +
                       std::to_string(quota_.maxNamespaceBytes) + " B";
        return result;
    }
    if (!existed &&
        static_cast<int>(list(kind).size()) >= quota_.maxSlots) {
        result.status = StorageStatus::QuotaExceeded;
        result.error = "namespace '" + std::string(toString(kind)) +
                       "' already holds the maximum " +
                       std::to_string(quota_.maxSlots) + " slots";
        return result;
    }

    std::filesystem::create_directories(path.parent_path(), ec);
    const AtomicWriteResult write = writeFileAtomically(path, serialized);
    if (!write) {
        result.status = StorageStatus::IoError;
        result.error = write.error;
        return result;
    }
    result.found = true;
    result.meta.bytes = payloadBytes;
    result.meta.savedAt = envelope["savedAt"].get<std::int64_t>();
    result.meta.dataVersion = dataVersion;
    result.meta.schema = kSaveEnvelopeVersion;
    return result;
}

StorageResult PlayerStorage::load(StorageKind kind, const std::string& slot) const {
    StorageResult result;
    result.meta.kind = kind;
    if (!validSlot(slot)) {
        result.status = StorageStatus::InvalidSlot;
        result.error = "slot must match [A-Za-z0-9_-]{1,64}";
        return result;
    }
    bool ok = false;
    const std::string raw = readWhole(pathFor(kind, slot), ok);
    if (!ok) {
        result.status = StorageStatus::NotFound;
        return result;
    }
    result.found = true;

    nlohmann::json doc = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
    const bool isEnvelope = doc.is_object() && doc.contains(kMarker) &&
                            doc[kMarker].is_boolean() && doc[kMarker].get<bool>() &&
                            doc.contains("payload") && doc["payload"].is_string();
    if (!isEnvelope) {
        result.status = StorageStatus::Corrupt;
        result.error = "save document is not a Saida storage envelope";
        return result;
    }

    const std::string envelopeError =
        format::schemaEnvelopeError(doc, kSaveEnvelopeVersion, "save");
    if (!envelopeError.empty()) {
        result.status = StorageStatus::Corrupt;
        result.error = envelopeError;
        result.payload.clear();
        return result;
    }

    result.payload = doc["payload"].get<std::string>();
    result.meta.schema = kSaveEnvelopeVersion;
    result.meta.bytes = static_cast<std::int64_t>(result.payload.size());
    if (doc.contains("dataVersion") && doc["dataVersion"].is_number_integer())
        result.meta.dataVersion = doc["dataVersion"].get<int>();
    if (doc.contains("savedAt") && doc["savedAt"].is_number_integer())
        result.meta.savedAt = doc["savedAt"].get<std::int64_t>();
    return result;
}

StorageResult PlayerStorage::info(StorageKind kind, const std::string& slot) const {
    StorageResult result = load(kind, slot);
    result.payload.clear();  // info() reports metadata only
    return result;
}

StorageResult PlayerStorage::remove(StorageKind kind, const std::string& slot) {
    StorageResult result;
    result.meta.kind = kind;
    if (!validSlot(slot)) {
        result.status = StorageStatus::InvalidSlot;
        result.error = "slot must match [A-Za-z0-9_-]{1,64}";
        return result;
    }
    std::error_code ec;
    const bool removed = std::filesystem::remove(pathFor(kind, slot), ec);
    if (ec) {
        result.status = StorageStatus::IoError;
        result.error = ec.message();
        return result;
    }
    if (!removed) {
        result.status = StorageStatus::NotFound;
        return result;
    }
    result.found = true;
    return result;
}

bool PlayerStorage::has(StorageKind kind, const std::string& slot) const {
    if (!validSlot(slot)) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(pathFor(kind, slot), ec);
}

std::vector<std::string> PlayerStorage::list(StorageKind kind) const {
    std::vector<std::string> slots;
    std::error_code ec;
    const std::filesystem::path dir = dirFor(kind);
    if (!std::filesystem::is_directory(dir, ec)) return slots;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".json") continue;
        slots.push_back(entry.path().stem().string());
    }
    return slots;
}

} // namespace saida
