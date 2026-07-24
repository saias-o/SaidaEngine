#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Player-facing persistence for game scripts.
//
// The engine only ever stores opaque strings: the game serializes its own state
// (JSON.stringify) and hands the result over as a slot payload. PlayerStorage
// adds the durability guarantees the raw file API lacked:
//   - a versioned envelope (schema/version, shared fail-closed guard) so a
//     future or tampered save is refused instead of silently misread;
//   - per-slot metadata (kind, byte size, save timestamp, app data version)
//     without the game having to reserve fields inside its own payload;
//   - two separate namespaces, progression and preferences, so clearing a save
//     never wipes settings and vice versa;
//   - byte and slot quotas with typed errors, so a runaway save fails loudly
//     rather than filling the disk.
//
// PlayerStorage is pure filesystem logic with no engine dependency, so it is unit-tested headless
// and shared by desktop, the packaged runtime and the web player unchanged.
namespace saida {

enum class StorageKind { Progress, Preference };

enum class StorageStatus {
    Ok,
    InvalidSlot,    // slot name breaks [A-Za-z0-9_-]{1,64}
    QuotaExceeded,  // payload, namespace budget or slot count exceeded
    NotFound,       // load/info/remove of a missing slot
    Corrupt,        // envelope present but malformed / unsupported schema
    IoError,        // write/read/remove failed on disk
};

const char* toString(StorageStatus status);
const char* toString(StorageKind kind);

// Envelope schema written by this version.
constexpr int kSaveEnvelopeVersion = 1;

struct StorageMeta {
    StorageKind kind = StorageKind::Progress;
    std::int64_t bytes = 0;    // payload length in bytes
    std::int64_t savedAt = 0;
    int dataVersion = 0;
    int schema = 0;
};

struct StorageResult {
    StorageStatus status = StorageStatus::Ok;
    std::string error;    // actionable diagnostic; empty when status == Ok
    std::string payload;  // populated by load()
    bool found = false;   // whether the slot existed (load/info/remove)
    StorageMeta meta;

    explicit operator bool() const { return status == StorageStatus::Ok; }
};

struct StorageQuota {
    std::int64_t maxSlotBytes = 1 * 1024 * 1024;        // 1 MiB per payload
    std::int64_t maxNamespaceBytes = 16 * 1024 * 1024;  // 16 MiB per namespace
    int maxSlots = 256;                                 // per namespace
};

class PlayerStorage {
public:
    // progressDir/prefDir are the directories owning each namespace's slot
    // files; they are created on demand. Passing them explicitly keeps the
    // service free of any path-resolution policy (editor vs packaged vs web).
    PlayerStorage(std::filesystem::path progressDir, std::filesystem::path prefDir,
                  StorageQuota quota = {});

    StorageResult save(StorageKind kind, const std::string& slot,
                       const std::string& payload, int dataVersion = 0);
    StorageResult load(StorageKind kind, const std::string& slot) const;
    StorageResult info(StorageKind kind, const std::string& slot) const;
    StorageResult remove(StorageKind kind, const std::string& slot);
    bool has(StorageKind kind, const std::string& slot) const;
    std::vector<std::string> list(StorageKind kind) const;

    const StorageQuota& quota() const { return quota_; }

    static bool validSlot(const std::string& slot);

private:
    std::filesystem::path dirFor(StorageKind kind) const;
    std::filesystem::path pathFor(StorageKind kind, const std::string& slot) const;
    // Total on-disk bytes of a namespace, optionally excluding one slot file
    // (used to price a rewrite against the namespace budget).
    std::int64_t namespaceBytes(StorageKind kind, const std::string& excludeSlot) const;

    std::filesystem::path progressDir_;
    std::filesystem::path prefDir_;
    StorageQuota quota_;
};

} // namespace saida
