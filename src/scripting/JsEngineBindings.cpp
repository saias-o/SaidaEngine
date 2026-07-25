#include "scripting/JsEngineBindings.hpp"

#include "audio/AudioManager.hpp"
#include "core/AtomicFile.hpp"
#include "core/Input.hpp"
#include "core/InputProfile.hpp"
#include "core/InputTouch.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/PlatformCaps.hpp"
#include "core/Reflection.hpp"
#include "core/Time.hpp"
#include "graphics/ResourceManager.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/PhysicsWorld.hpp"
#include "project/AssetLoader.hpp"
#include "project/PlayerStorage.hpp"
#include "scene/Behaviour.hpp"
#include "scene/Blackboard.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "nodes/UITextNode.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/SequenceDirectorBehaviour.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/ScriptBehaviour.hpp"
#include "scripting/JsTimerBindings.hpp"

#include <quickjs.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace saida {

namespace {

JSClassID gAssetHandleClassId = 0;
SceneTree* treeFromJs(JSContext* ctx);
JSValue makeNodeRef(JSContext* ctx, Node* target);

AssetHandle* assetHandleFromJs(JSContext* ctx, JSValueConst value) {
    return static_cast<AssetHandle*>(JS_GetOpaque2(ctx, value, gAssetHandleClassId));
}

void jsAssetHandleFinalizer(JSRuntime*, JSValueConst value) {
    delete static_cast<AssetHandle*>(JS_GetOpaque(value, gAssetHandleClassId));
}

JSValue jsAssetHandleState(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    AssetHandle* handle = assetHandleFromJs(ctx, self);
    return handle ? JS_NewString(ctx, assetLoadStateName(handle->state())) : JS_EXCEPTION;
}

JSValue jsAssetHandleReady(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    AssetHandle* handle = assetHandleFromJs(ctx, self);
    return handle ? JS_NewBool(ctx, handle->ready()) : JS_EXCEPTION;
}

JSValue jsAssetHandleFailed(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    AssetHandle* handle = assetHandleFromJs(ctx, self);
    return handle ? JS_NewBool(ctx, handle->failed()) : JS_EXCEPTION;
}

JSValue jsAssetHandleError(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    AssetHandle* handle = assetHandleFromJs(ctx, self);
    return handle ? JS_NewString(ctx, handle->error().c_str()) : JS_EXCEPTION;
}

JSValue jsAssetHandleSize(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    AssetHandle* handle = assetHandleFromJs(ctx, self);
    return handle ? JS_NewInt64(ctx, static_cast<int64_t>(handle->bytes().size())) : JS_EXCEPTION;
}

JSValue jsAssetHandleId(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    AssetHandle* handle = assetHandleFromJs(ctx, self);
    return handle ? JS_NewBigUint64(ctx, handle->id()) : JS_EXCEPTION;
}

JSValue jsAssetHandleRelease(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    AssetHandle* handle = assetHandleFromJs(ctx, self);
    if (!handle) return JS_EXCEPTION;
    handle->reset();
    return JS_UNDEFINED;
}

AssetLoadPriority readAssetPriority(JSContext* ctx, int argc, JSValueConst* argv) {
    if (argc < 2 || JS_IsUndefined(argv[1])) return AssetLoadPriority::Normal;
    const char* value = JS_ToCString(ctx, argv[1]);
    if (!value) return AssetLoadPriority::Normal;
    const std::string priority(value);
    JS_FreeCString(ctx, value);
    if (priority == "low") return AssetLoadPriority::Low;
    if (priority == "high") return AssetLoadPriority::High;
    if (priority == "critical") return AssetLoadPriority::Critical;
    return AssetLoadPriority::Normal;
}

JSValue jsAssetsLoad(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return JS_ThrowInternalError(ctx, "assets.load requires a mounted SceneTree");
    if (argc < 1) return JS_ThrowTypeError(ctx, "assets.load(path[, priority]) requires a path");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    const std::string path(raw);
    JS_FreeCString(ctx, raw);
    const std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
    if (path.empty() || normalized.is_absolute() || normalized.begin()->string() == "..")
        return JS_ThrowTypeError(ctx, "asset path must stay inside the project");

    AssetHandle handle = tree->resources().assetLoader().request(
        normalized.generic_string(), AssetType::Unknown,
        readAssetPriority(ctx, argc, argv));
    if (!handle) return JS_ThrowInternalError(ctx, "asset loader is unavailable");
    JSValue object = JS_NewObjectClass(ctx, gAssetHandleClassId);
    if (JS_IsException(object)) return object;
    JS_SetOpaque(object, new AssetHandle(std::move(handle)));
    return object;
}

JSValue jsAudioPlay(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "audio.play(aliasName)");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const bool ok = AudioManager::get().play(name) != kInvalidAudioID;
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

// ---- storage: per-slot persistence delegated to PlayerStorage -------------
// JS serializes on its own (JSON.stringify/parse); the engine only stores
// opaque strings. PlayerStorage adds the versioned envelope, slot metadata,
// the progress/preferences separation, and quotas; it refuses a future or
// tampered save instead of misreading it. Directories are resolved under the
// project root, so they're identical in the editor, the packaged runtime
// (root = exe folder), and the web player (IDBFS/MEMFS).
//
// storage.* operates on progress, storage.prefs.* on preferences. The last
// failure of a non-thrown operation (quota, io, corruption) is available via
// storage.lastError(); save/remove return a boolean.

// Last status/diagnostic of a storage operation on the current context.
thread_local StorageStatus gLastStorageStatus = StorageStatus::Ok;
thread_local std::string gLastStorageError;

void recordStorageResult(const StorageResult& r) {
    gLastStorageStatus = r.status;
    gLastStorageError = r.error;
}

PlayerStorage storageFor(SceneTree& tree) {
    // Packaged game: saves/prefs under the OS user folder (never next to the
    // exe). Editor/dev and web: project root (userSaveRoot() returns empty).
    const std::string base = userSaveRoot();
    if (!base.empty())
        return PlayerStorage(std::filesystem::path(base) / "saves",
                             std::filesystem::path(base) / "prefs");
    return PlayerStorage(std::filesystem::path(tree.resolveProjectPath("saves")),
                         std::filesystem::path(tree.resolveProjectPath("prefs")));
}

void syncfsAfterMutation() {
#ifdef __EMSCRIPTEN__
    // saves/ and prefs/ live on IDBFS (mounted by the shell): flush to
    // IndexedDB after every durable write.
    EM_ASM({ FS.syncfs(false, function() {}); });
#else
    (void)0;
#endif
}

// ---- storage.flush(): asynchronous durability contract ---------------------
// Synchronous visibility (a load following a save returns the value),
// asynchronous durability: the promise resolves `true` once pending writes
// are durable, `false` on failure — never a rejection. Desktop: writes are
// atomic and durable as of save(), the promise resolves on the next
// microtask drain. Web: resolved by the FS.syncfs callback (IndexedDB). A
// future cloud backend slots in behind the same promise without changing
// the API. Covers both saves AND prefs (flush is global to the persistent
// filesystem).

#ifdef __EMSCRIPTEN__
struct PendingFlush {
    JSContext* ctx;
    JSValue resolve;
};

std::unordered_map<int, PendingFlush>& pendingFlushes() {
    static std::unordered_map<int, PendingFlush> map;
    return map;
}

int gNextFlushToken = 0;
#endif

JSValue jsStorageFlush(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) return promise;

#ifdef __EMSCRIPTEN__
    const int token = ++gNextFlushToken;
    pendingFlushes()[token] = {ctx, JS_DupValue(ctx, funcs[0])};
    EM_ASM({
        FS.syncfs(false, function(err) {
            _saida_storage_flush_done($0, err ? 1 : 0);
        });
    }, token);
#else
    JSValue arg = JS_NewBool(ctx, true);
    JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
#endif
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

// Reads the slot (argv[0]), validating it; returns false and sets outError if
// missing or empty. Does not validate the pattern: PlayerStorage does that
// and returns a status.
bool readSlotArg(JSContext* ctx, int argc, JSValueConst* argv, std::string& outSlot,
                 JSValue& outError) {
    outError = JS_UNDEFINED;
    if (argc < 1) {
        outError = JS_ThrowTypeError(ctx, "storage: missing slot name");
        return false;
    }
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) {
        outError = JS_EXCEPTION;
        return false;
    }
    outSlot.assign(raw);
    JS_FreeCString(ctx, raw);
    return true;
}

SceneTree* storageTree(JSContext* ctx, JSValue& outError) {
    outError = JS_UNDEFINED;
    SceneTree* tree = treeFromJs(ctx);
    if (!tree)
        outError = JS_ThrowInternalError(ctx, "storage requires a mounted SceneTree");
    return tree;
}

JSValue metaToJs(JSContext* ctx, const StorageMeta& meta) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, toString(meta.kind)));
    JS_SetPropertyStr(ctx, o, "bytes", JS_NewInt64(ctx, meta.bytes));
    JS_SetPropertyStr(ctx, o, "savedAt", JS_NewInt64(ctx, meta.savedAt));
    JS_SetPropertyStr(ctx, o, "dataVersion", JS_NewInt32(ctx, meta.dataVersion));
    JS_SetPropertyStr(ctx, o, "schema", JS_NewInt32(ctx, meta.schema));
    return o;
}

JSValue jsStorageSaveKind(JSContext* ctx, StorageKind kind, int argc,
                          JSValueConst* argv) {
    JSValue error;
    SceneTree* tree = storageTree(ctx, error);
    if (!tree) return error;
    std::string slot;
    if (!readSlotArg(ctx, argc, argv, slot, error)) return error;
    if (argc < 2) return JS_ThrowTypeError(ctx, "storage.save(slot, jsonString)");
    const char* value = JS_ToCString(ctx, argv[1]);
    if (!value) return JS_EXCEPTION;
    // Optional application version, defaults to 0.
    int dataVersion = 0;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
        JS_ToInt32(ctx, &dataVersion, argv[2]);

    PlayerStorage store = storageFor(*tree);
    const StorageResult r = store.save(kind, slot, value, dataVersion);
    JS_FreeCString(ctx, value);
    recordStorageResult(r);
    if (!r)
        Log::warn("[JS] storage.save: ", toString(r.status), ": ", r.error);
    else
        syncfsAfterMutation();
    return JS_NewBool(ctx, static_cast<bool>(r));
}

JSValue jsStorageLoadKind(JSContext* ctx, StorageKind kind, int argc,
                          JSValueConst* argv) {
    JSValue error;
    SceneTree* tree = storageTree(ctx, error);
    if (!tree) return error;
    std::string slot;
    if (!readSlotArg(ctx, argc, argv, slot, error)) return error;
    const StorageResult r = storageFor(*tree).load(kind, slot);
    recordStorageResult(r);
    if (r.status == StorageStatus::NotFound) return JS_NULL;
    if (!r) {
        // Corrupted/rejected envelope or invalid slot: diagnostic logged, null
        // returned (available via storage.lastError()).
        Log::warn("[JS] storage.load: ", toString(r.status), ": ", r.error);
        return JS_NULL;
    }
    return JS_NewStringLen(ctx, r.payload.data(), r.payload.size());
}

JSValue jsStorageHasKind(JSContext* ctx, StorageKind kind, int argc,
                         JSValueConst* argv) {
    JSValue error;
    SceneTree* tree = storageTree(ctx, error);
    if (!tree) return error;
    std::string slot;
    if (!readSlotArg(ctx, argc, argv, slot, error)) return error;
    return JS_NewBool(ctx, storageFor(*tree).has(kind, slot));
}

JSValue jsStorageRemoveKind(JSContext* ctx, StorageKind kind, int argc,
                            JSValueConst* argv) {
    JSValue error;
    SceneTree* tree = storageTree(ctx, error);
    if (!tree) return error;
    std::string slot;
    if (!readSlotArg(ctx, argc, argv, slot, error)) return error;
    PlayerStorage store = storageFor(*tree);
    const StorageResult r = store.remove(kind, slot);
    recordStorageResult(r);
    if (r.status == StorageStatus::IoError)
        Log::warn("[JS] storage.remove: ", r.error);
    if (r.found) syncfsAfterMutation();
    return JS_NewBool(ctx, r.found);
}

JSValue jsStorageInfoKind(JSContext* ctx, StorageKind kind, int argc,
                          JSValueConst* argv) {
    JSValue error;
    SceneTree* tree = storageTree(ctx, error);
    if (!tree) return error;
    std::string slot;
    if (!readSlotArg(ctx, argc, argv, slot, error)) return error;
    const StorageResult r = storageFor(*tree).info(kind, slot);
    recordStorageResult(r);
    if (!r.found || !r) return JS_NULL;
    return metaToJs(ctx, r.meta);
}

JSValue jsStorageListKind(JSContext* ctx, StorageKind kind) {
    JSValue error;
    SceneTree* tree = storageTree(ctx, error);
    if (!tree) return error;
    const std::vector<std::string> slots = storageFor(*tree).list(kind);
    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const std::string& s : slots)
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, s.data(), s.size()));
    return arr;
}

// storage.* (progress)
JSValue jsStorageSave(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageSaveKind(ctx, StorageKind::Progress, argc, argv);
}
JSValue jsStorageLoad(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageLoadKind(ctx, StorageKind::Progress, argc, argv);
}
JSValue jsStorageHas(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageHasKind(ctx, StorageKind::Progress, argc, argv);
}
JSValue jsStorageRemove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageRemoveKind(ctx, StorageKind::Progress, argc, argv);
}
JSValue jsStorageInfo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageInfoKind(ctx, StorageKind::Progress, argc, argv);
}
JSValue jsStorageList(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return jsStorageListKind(ctx, StorageKind::Progress);
}

// storage.prefs.* (preferences)
JSValue jsPrefSave(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageSaveKind(ctx, StorageKind::Preference, argc, argv);
}
JSValue jsPrefLoad(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageLoadKind(ctx, StorageKind::Preference, argc, argv);
}
JSValue jsPrefHas(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageHasKind(ctx, StorageKind::Preference, argc, argv);
}
JSValue jsPrefRemove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageRemoveKind(ctx, StorageKind::Preference, argc, argv);
}
JSValue jsPrefInfo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return jsStorageInfoKind(ctx, StorageKind::Preference, argc, argv);
}
JSValue jsPrefList(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return jsStorageListKind(ctx, StorageKind::Preference);
}

JSValue jsStorageLastError(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (gLastStorageStatus == StorageStatus::Ok) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "status",
                      JS_NewString(ctx, toString(gLastStorageStatus)));
    JS_SetPropertyStr(ctx, o, "message",
                      JS_NewStringLen(ctx, gLastStorageError.data(),
                                      gLastStorageError.size()));
    return o;
}

JSValue jsAssetsStats(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return JS_ThrowInternalError(ctx, "assets.stats requires a mounted SceneTree");
    const AssetLoadStats stats = tree->resources().assetLoader().stats();
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "live", JS_NewInt64(ctx, stats.live));
    JS_SetPropertyStr(ctx, o, "queued", JS_NewInt64(ctx, stats.queued));
    JS_SetPropertyStr(ctx, o, "loading", JS_NewInt64(ctx, stats.loading));
    JS_SetPropertyStr(ctx, o, "ready", JS_NewInt64(ctx, stats.ready));
    JS_SetPropertyStr(ctx, o, "failed", JS_NewInt64(ctx, stats.failed));
    // Cumulative rejections since boot (corrupted/hostile content) — CI criterion.
    JS_SetPropertyStr(ctx, o, "failedTotal",
                      JS_NewInt64(ctx, static_cast<int64_t>(stats.failedTotal)));
    JS_SetPropertyStr(ctx, o, "streamedFetches",
                      JS_NewInt64(ctx, static_cast<int64_t>(stats.streamedFetches)));
    JS_SetPropertyStr(ctx, o, "residentBytes",
                      JS_NewInt64(ctx, static_cast<int64_t>(stats.residentBytes)));
    JS_SetPropertyStr(ctx, o, "budgetBytes",
                      JS_NewInt64(ctx, static_cast<int64_t>(stats.budgetBytes)));
    // GPU bytes of resident resources (textures/meshes loaded per asset)
    // — leak criterion for the phase-3 E2E work.
    JS_SetPropertyStr(ctx, o, "gpuResidentBytes",
                      JS_NewInt64(ctx, static_cast<int64_t>(tree->resources().gpuResidentBytes())));
    // Mid-scene GPU budget (P0.5): cap and cumulative LRU evictions.
    JS_SetPropertyStr(ctx, o, "gpuBudgetBytes",
                      JS_NewInt64(ctx, static_cast<int64_t>(tree->resources().gpuBudgetBytes())));
    JS_SetPropertyStr(ctx, o, "gpuEvictedCount",
                      JS_NewInt64(ctx, static_cast<int64_t>(tree->resources().gpuEvictedCount())));
    JS_SetPropertyStr(ctx, o, "gpuEvictedBytes",
                      JS_NewInt64(ctx, static_cast<int64_t>(tree->resources().gpuEvictedBytes())));
    return o;
}

// assets.setGpuBudget(bytes): mid-scene GPU cap (0 = unlimited). A test/
// diagnostic tool first — a game can also tune it to the platform.
JSValue jsAssetsSetGpuBudget(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return JS_ThrowInternalError(ctx, "assets.setGpuBudget requires a mounted SceneTree");
    double bytes = 0.0;
    if (argc < 1 || JS_ToFloat64(ctx, &bytes, argv[0]) != 0 || bytes < 0.0)
        return JS_ThrowTypeError(ctx, "assets.setGpuBudget(bytes >= 0)");
    tree->resources().setGpuBudget(static_cast<uint64_t>(bytes));
    return JS_NewBool(ctx, true);
}

void installAssetHandleClass(JSContext* ctx) {
    JSRuntime* runtime = JS_GetRuntime(ctx);
    if (gAssetHandleClassId == 0) JS_NewClassID(runtime, &gAssetHandleClassId);
    JSClassDef def{};
    def.class_name = "SaidaAssetHandle";
    def.finalizer = jsAssetHandleFinalizer;
    JS_NewClass(runtime, gAssetHandleClassId, &def);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "state", JS_NewCFunction(ctx, jsAssetHandleState, "state", 0));
    JS_SetPropertyStr(ctx, proto, "ready", JS_NewCFunction(ctx, jsAssetHandleReady, "ready", 0));
    JS_SetPropertyStr(ctx, proto, "failed", JS_NewCFunction(ctx, jsAssetHandleFailed, "failed", 0));
    JS_SetPropertyStr(ctx, proto, "error", JS_NewCFunction(ctx, jsAssetHandleError, "error", 0));
    JS_SetPropertyStr(ctx, proto, "size", JS_NewCFunction(ctx, jsAssetHandleSize, "size", 0));
    JS_SetPropertyStr(ctx, proto, "id", JS_NewCFunction(ctx, jsAssetHandleId, "id", 0));
    JS_SetPropertyStr(ctx, proto, "release", JS_NewCFunction(ctx, jsAssetHandleRelease, "release", 0));
    JS_SetClassProto(ctx, gAssetHandleClassId, proto);
}

Behaviour* behaviourFromJs(JSContext* ctx) {
    return static_cast<Behaviour*>(JS_GetContextOpaque(ctx));
}

Node* nodeFromJs(JSContext* ctx) {
    Behaviour* behaviour = behaviourFromJs(ctx);
    return behaviour ? behaviour->node() : nullptr;
}

SceneTree* treeFromJs(JSContext* ctx) {
    Behaviour* behaviour = behaviourFromJs(ctx);
    return behaviour ? behaviour->tree() : nullptr;
}

bool toNumber(JSContext* ctx, JSValueConst value, double& out) {
    return JS_ToFloat64(ctx, &out, value) == 0;
}

bool readVec3Args(JSContext* ctx, int argc, JSValueConst* argv, glm::vec3& out) {
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue x = JS_GetPropertyStr(ctx, argv[0], "x");
        JSValue y = JS_GetPropertyStr(ctx, argv[0], "y");
        JSValue z = JS_GetPropertyStr(ctx, argv[0], "z");

        double vx = 0.0;
        double vy = 0.0;
        double vz = 0.0;
        bool ok = toNumber(ctx, x, vx) && toNumber(ctx, y, vy) && toNumber(ctx, z, vz);

        JS_FreeValue(ctx, x);
        JS_FreeValue(ctx, y);
        JS_FreeValue(ctx, z);

        if (!ok) return false;
        out = glm::vec3(static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz));
        return true;
    }

    if (argc < 3) return false;

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    if (!toNumber(ctx, argv[0], x) || !toNumber(ctx, argv[1], y) || !toNumber(ctx, argv[2], z)) {
        return false;
    }

    out = glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    return true;
}

JSValue makeVec3(JSContext* ctx, const glm::vec3& v) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, v.y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, v.z));
    return obj;
}

JSValue makeQuat(JSContext* ctx, const glm::quat& q) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, q.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, q.y));
    JS_SetPropertyStr(ctx, obj, "z", JS_NewFloat64(ctx, q.z));
    JS_SetPropertyStr(ctx, obj, "w", JS_NewFloat64(ctx, q.w));
    return obj;
}

// Accepts ({x,y,z,w}) or (x,y,z,w). A non-finite or zero-length quaternion is
// rejected rather than normalized: silently repairing it would hide the caller's
// bug and leave the node with an orientation it never asked for.
bool readQuatArgs(JSContext* ctx, int argc, JSValueConst* argv, glm::quat& out) {
    double v[4] = {0.0, 0.0, 0.0, 0.0};
    if (argc >= 1 && JS_IsObject(argv[0])) {
        static const char* kKeys[4] = {"x", "y", "z", "w"};
        for (int i = 0; i < 4; ++i) {
            JSValue prop = JS_GetPropertyStr(ctx, argv[0], kKeys[i]);
            const bool ok = toNumber(ctx, prop, v[i]);
            JS_FreeValue(ctx, prop);
            if (!ok) return false;
        }
    } else if (argc >= 4) {
        for (int i = 0; i < 4; ++i)
            if (!toNumber(ctx, argv[i], v[i])) return false;
    } else {
        return false;
    }

    const glm::quat q(static_cast<float>(v[3]), static_cast<float>(v[0]),
                      static_cast<float>(v[1]), static_cast<float>(v[2]));
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) ||
        !std::isfinite(q.w) || glm::length(q) < 1e-6f) {
        return false;
    }
    out = glm::normalize(q);
    return true;
}

// Character control resolves like the other gameplay bindings: the node itself,
// else the first CharacterBody below it. That keeps a controller script working
// whether it sits on the body or on a wrapper node above it.
CharacterBodyNode* characterFor(Node* node) {
    if (!node) return nullptr;
    if (CharacterBodyNode* self = node->asCharacterBody()) return self;
    for (const std::unique_ptr<Node>& child : node->children()) {
        if (CharacterBodyNode* found = characterFor(child.get())) return found;
    }
    return nullptr;
}

JSValue makeVec2(JSContext* ctx, const glm::vec2& v) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "x", JS_NewFloat64(ctx, v.x));
    JS_SetPropertyStr(ctx, obj, "y", JS_NewFloat64(ctx, v.y));
    return obj;
}

bool readActionName(JSContext* ctx, int argc, JSValueConst* argv, int index, std::string& out) {
    if (index >= argc) return false;
    const char* action = JS_ToCString(ctx, argv[index]);
    if (!action) return false;
    out = action;
    JS_FreeCString(ctx, action);
    return !out.empty();
}

JSValue jsNodeGetName(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Node* node = nodeFromJs(ctx);
    return JS_NewString(ctx, node ? node->name().c_str() : "");
}

JSValue jsNodeSetName(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node || argc < 1) return JS_NewBool(ctx, false);

    const char* name = JS_ToCString(ctx, argv[0]);
    bool ok = name != nullptr;
    if (ok) node->setName(name);
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, ok);
}

JSValue jsNodeGetPosition(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Node* node = nodeFromJs(ctx);
    return makeVec3(ctx, node ? node->transform().position : glm::vec3(0.0f));
}

JSValue jsNodeSetPosition(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node) return JS_NewBool(ctx, false);

    glm::vec3 value(0.0f);
    if (!readVec3Args(ctx, argc, argv, value)) return JS_NewBool(ctx, false);

    node->transform().position = value;
    return JS_NewBool(ctx, true);
}

JSValue jsNodeTranslate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node) return JS_NewBool(ctx, false);

    glm::vec3 value(0.0f);
    if (!readVec3Args(ctx, argc, argv, value)) return JS_NewBool(ctx, false);

    node->transform().position += value;
    return JS_NewBool(ctx, true);
}

JSValue jsNodeGetRotation(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    Node* node = nodeFromJs(ctx);
    return makeQuat(ctx, node ? node->transform().rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
}

JSValue jsNodeSetRotation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node) return JS_NewBool(ctx, false);

    glm::quat value(1.0f, 0.0f, 0.0f, 0.0f);
    if (!readQuatArgs(ctx, argc, argv, value)) return JS_NewBool(ctx, false);

    node->transform().rotation = value;
    return JS_NewBool(ctx, true);
}

// Gameplay writes the character's velocity; the engine integrates it during the
// physics step. Without a CharacterBody the setter reports false and the getters
// answer zero/false, matching the physics queries: no world, no error.
JSValue jsNodeSetVelocity(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    CharacterBodyNode* body = characterFor(nodeFromJs(ctx));
    if (!body) return JS_NewBool(ctx, false);

    glm::vec3 value(0.0f);
    if (!readVec3Args(ctx, argc, argv, value)) return JS_NewBool(ctx, false);

    body->velocity = value;
    return JS_NewBool(ctx, true);
}

JSValue jsNodeGetVelocity(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    CharacterBodyNode* body = characterFor(nodeFromJs(ctx));
    return makeVec3(ctx, body ? body->velocity : glm::vec3(0.0f));
}

JSValue jsNodeIsOnFloor(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    CharacterBodyNode* body = characterFor(nodeFromJs(ctx));
    return JS_NewBool(ctx, body && body->isOnFloor());
}

JSValue jsNodeIsOnSteepSlope(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    CharacterBodyNode* body = characterFor(nodeFromJs(ctx));
    return JS_NewBool(ctx, body && body->isOnSteepSlope());
}

JSValue jsNodeGroundNormal(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    CharacterBodyNode* body = characterFor(nodeFromJs(ctx));
    return makeVec3(ctx, body ? body->groundNormal() : glm::vec3(0.0f, 1.0f, 0.0f));
}

JSValue jsNodeSetEnabled(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node || argc < 1) return JS_NewBool(ctx, false);

    int enabled = JS_ToBool(ctx, argv[0]);
    if (enabled < 0) return JS_EXCEPTION;
    node->setEnabled(enabled != 0);
    return JS_NewBool(ctx, true);
}

JSValue jsNodeQueueFree(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (Node* node = nodeFromJs(ctx)) {
        node->queueFree();
    }
    return JS_UNDEFINED;
}

JSValue jsNodeSetText(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    auto* text = dynamic_cast<UITextNode*>(node);
    if (!text)
        return JS_ThrowTypeError(ctx, "node.setText: script is not attached to a UITextNode");
    if (argc < 1) return JS_ThrowTypeError(ctx, "node.setText(text)");
    const char* value = JS_ToCString(ctx, argv[0]);
    if (!value) return JS_EXCEPTION;
    text->setText(value);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

JSValue jsNodeGetText(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* text = dynamic_cast<UITextNode*>(nodeFromJs(ctx));
    if (!text)
        return JS_ThrowTypeError(ctx, "node.getText: script is not attached to a UITextNode");
    return JS_NewString(ctx, text->text().c_str());
}

JSValue jsNodeAddToGroup(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node || argc < 1) return JS_NewBool(ctx, false);

    const char* group = JS_ToCString(ctx, argv[0]);
    bool ok = group != nullptr;
    if (ok) node->addToGroup(group);
    JS_FreeCString(ctx, group);
    return JS_NewBool(ctx, ok);
}

JSValue jsNodeRemoveFromGroup(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node || argc < 1) return JS_NewBool(ctx, false);

    const char* group = JS_ToCString(ctx, argv[0]);
    bool ok = group != nullptr;
    if (ok) node->removeFromGroup(group);
    JS_FreeCString(ctx, group);
    return JS_NewBool(ctx, ok);
}

JSValue jsNodeIsInGroup(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    Node* node = nodeFromJs(ctx);
    if (!node || argc < 1) return JS_NewBool(ctx, false);

    const char* group = JS_ToCString(ctx, argv[0]);
    bool ok = group && node->isInGroup(group);
    JS_FreeCString(ctx, group);
    return JS_NewBool(ctx, ok);
}

JSValue jsTimeDelta(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewFloat64(ctx, Time::delta());
}

JSValue jsTimeElapsed(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewFloat64(ctx, Time::elapsed());
}

JSValue jsInputIsHeld(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string action;
    if (!readActionName(ctx, argc, argv, 0, action)) return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, Input::isActionHeld(action));
}

JSValue jsInputJustPressed(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string action;
    if (!readActionName(ctx, argc, argv, 0, action)) return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, Input::isActionJustPressed(action));
}

JSValue jsInputJustReleased(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string action;
    if (!readActionName(ctx, argc, argv, 0, action)) return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, Input::isActionJustReleased(action));
}

// Test/CI tool: drives an action without a keyboard (see Input::injectAction).
JSValue jsInputInject(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string action;
    if (!readActionName(ctx, argc, argv, 0, action)) return JS_NewBool(ctx, false);
    double strength = 1.0;
    if (argc >= 2 && JS_ToFloat64(ctx, &strength, argv[1]) != 0) return JS_EXCEPTION;
    Input::injectAction(action, static_cast<float>(strength));
    return JS_NewBool(ctx, true);
}

JSValue jsInputStrength(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string action;
    if (!readActionName(ctx, argc, argv, 0, action)) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, Input::getActionStrength(action));
}

JSValue jsInputAxis(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string negative;
    std::string positive;
    if (!readActionName(ctx, argc, argv, 0, negative) || !readActionName(ctx, argc, argv, 1, positive)) {
        return JS_NewFloat64(ctx, 0.0);
    }
    return JS_NewFloat64(ctx, Input::getAxis(negative, positive));
}

JSValue jsInputVector(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string left;
    std::string right;
    std::string down;
    std::string up;
    if (!readActionName(ctx, argc, argv, 0, left) ||
        !readActionName(ctx, argc, argv, 1, right) ||
        !readActionName(ctx, argc, argv, 2, down) ||
        !readActionName(ctx, argc, argv, 3, up)) {
        return makeVec2(ctx, glm::vec2(0.0f));
    }
    return makeVec2(ctx, Input::getVector(left, right, down, up));
}

JSValue jsInputMousePosition(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return makeVec2(ctx, Input::mousePosition());
}

JSValue jsInputMouseDelta(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return makeVec2(ctx, Input::mouseDelta());
}

JSValue jsInputRebindKey(JSContext* ctx, JSValueConst, int argc,
                         JSValueConst* argv) {
    std::string action;
    std::string control;
    std::string context = kGlobalContext;
    if (!readActionName(ctx, argc, argv, 0, action) ||
        !readActionName(ctx, argc, argv, 1, control) ||
        (argc >= 3 && !readActionName(ctx, argc, argv, 2, context))) {
        return JS_ThrowTypeError(ctx,
                                 "input.rebindKey(action, control, context?)");
    }
    KeyCode key = KeyCode::Unknown;
    if (!parseInputControl(control, key))
        return JS_ThrowTypeError(ctx, "input.rebindKey: unknown control '%s'",
                                 control.c_str());
    Input::rebindKey(action, key, context);
    return JS_NewBool(ctx, true);
}

JSValue jsInputRebindMouse(JSContext* ctx, JSValueConst, int argc,
                           JSValueConst* argv) {
    std::string action;
    std::string control;
    std::string context = kGlobalContext;
    if (!readActionName(ctx, argc, argv, 0, action) ||
        !readActionName(ctx, argc, argv, 1, control) ||
        (argc >= 3 && !readActionName(ctx, argc, argv, 2, context))) {
        return JS_ThrowTypeError(ctx,
                                 "input.rebindMouse(action, control, context?)");
    }
    MouseButton button = MouseButton::Left;
    if (!parseInputControl(control, button))
        return JS_ThrowTypeError(ctx, "input.rebindMouse: unknown control '%s'",
                                 control.c_str());
    Input::rebindMouse(action, button, context);
    return JS_NewBool(ctx, true);
}

JSValue jsInputRebindGamepadButton(JSContext* ctx, JSValueConst, int argc,
                                   JSValueConst* argv) {
    std::string action;
    std::string control;
    std::string context = kGlobalContext;
    if (!readActionName(ctx, argc, argv, 0, action) ||
        !readActionName(ctx, argc, argv, 1, control) ||
        (argc >= 3 && !readActionName(ctx, argc, argv, 2, context))) {
        return JS_ThrowTypeError(
            ctx, "input.rebindGamepadButton(action, control, context?)");
    }
    GamepadButton button = GamepadButton::A;
    if (!parseInputControl(control, button))
        return JS_ThrowTypeError(
            ctx, "input.rebindGamepadButton: unknown control '%s'",
            control.c_str());
    Input::rebindGamepadButton(action, button, context);
    return JS_NewBool(ctx, true);
}

JSValue jsInputRebindGamepadAxis(JSContext* ctx, JSValueConst, int argc,
                                 JSValueConst* argv) {
    std::string action;
    std::string control;
    if (!readActionName(ctx, argc, argv, 0, action) ||
        !readActionName(ctx, argc, argv, 1, control)) {
        return JS_ThrowTypeError(
            ctx,
            "input.rebindGamepadAxis(action, control, scale?, deadzone?, context?)");
    }
    GamepadAxis axis = GamepadAxis::LeftX;
    if (!parseInputControl(control, axis))
        return JS_ThrowTypeError(
            ctx, "input.rebindGamepadAxis: unknown control '%s'",
            control.c_str());

    double scale = 1.0;
    double deadzone = 0.1;
    std::string context = kGlobalContext;
    if ((argc >= 3 && JS_ToFloat64(ctx, &scale, argv[2]) != 0) ||
        (argc >= 4 && JS_ToFloat64(ctx, &deadzone, argv[3]) != 0) ||
        (argc >= 5 && !readActionName(ctx, argc, argv, 4, context))) {
        return JS_EXCEPTION;
    }
    if (!std::isfinite(scale) || std::abs(scale) > 10.0 ||
        !std::isfinite(deadzone) || deadzone < 0.0 || deadzone > 0.99) {
        return JS_ThrowRangeError(
            ctx, "input.rebindGamepadAxis: scale/deadzone out of range");
    }
    Input::rebindGamepadAxis(action, axis, static_cast<float>(scale),
                             static_cast<float>(deadzone), context);
    return JS_NewBool(ctx, true);
}

JSValue jsInputRebindTouch(JSContext* ctx, JSValueConst, int argc,
                           JSValueConst* argv) {
    std::string action;
    std::string control;
    if (!readActionName(ctx, argc, argv, 0, action) ||
        !readActionName(ctx, argc, argv, 1, control) || argc < 6) {
        return JS_ThrowTypeError(
            ctx,
            "input.rebindTouch(action, gesture, minX, minY, maxX, maxY, "
            "minDistance?, context?)");
    }
    TouchGesture gesture = TouchGesture::Press;
    if (!parseInputControl(control, gesture))
        return JS_ThrowTypeError(ctx,
                                 "input.rebindTouch: unknown gesture '%s'",
                                 control.c_str());

    double minX = 0.0;
    double minY = 0.0;
    double maxX = 1.0;
    double maxY = 1.0;
    double minDistance = 48.0;
    std::string context = kGlobalContext;
    if (JS_ToFloat64(ctx, &minX, argv[2]) != 0 ||
        JS_ToFloat64(ctx, &minY, argv[3]) != 0 ||
        JS_ToFloat64(ctx, &maxX, argv[4]) != 0 ||
        JS_ToFloat64(ctx, &maxY, argv[5]) != 0 ||
        (argc >= 7 && JS_ToFloat64(ctx, &minDistance, argv[6]) != 0) ||
        (argc >= 8 && !readActionName(ctx, argc, argv, 7, context))) {
        return JS_EXCEPTION;
    }
    const glm::vec2 zoneMin{static_cast<float>(minX), static_cast<float>(minY)};
    const glm::vec2 zoneMax{static_cast<float>(maxX), static_cast<float>(maxY)};
    if (!std::isfinite(minDistance) || minDistance < 0.0 ||
        minDistance > 4096.0 ||
        !std::isfinite(minX) || !std::isfinite(minY) ||
        !std::isfinite(maxX) || !std::isfinite(maxY) ||
        !input_detail::validTouchZone(zoneMin, zoneMax)) {
        return JS_ThrowRangeError(
            ctx, "input.rebindTouch: zone/distance out of range");
    }
    Input::rebindTouch(action, gesture, zoneMin, zoneMax,
                       static_cast<float>(minDistance), context);
    return JS_NewBool(ctx, true);
}

JSValue jsInputExportProfile(JSContext* ctx, JSValueConst, int argc,
                             JSValueConst* argv) {
    std::string name = "default";
    if (argc >= 1 && !readActionName(ctx, argc, argv, 0, name))
        return JS_ThrowTypeError(ctx, "input.exportProfile(name?)");
    if (name.size() > 64)
        return JS_ThrowRangeError(ctx,
                                  "input.exportProfile: name exceeds 64 characters");
    const std::string serialized = Input::serializeBindingProfile(name);
    return JS_NewStringLen(ctx, serialized.data(), serialized.size());
}

JSValue jsInputApplyProfile(JSContext* ctx, JSValueConst, int argc,
                            JSValueConst* argv) {
    std::string serialized;
    if (!readActionName(ctx, argc, argv, 0, serialized))
        return JS_ThrowTypeError(ctx, "input.applyProfile(serializedJson)");
    std::string error;
    if (!Input::applyBindingProfile(serialized, error))
        return JS_ThrowTypeError(ctx, "input.applyProfile: %s", error.c_str());
    return JS_NewBool(ctx, true);
}

JSValue jsInputLastActiveDevice(JSContext* ctx, JSValueConst, int,
                                JSValueConst*) {
    return JS_NewString(ctx, Input::deviceName(Input::lastActiveDevice()));
}

// Test/CI only, like input.inject: simulates device activity so adaptive-prompt
// proofs can drive lastActiveDevice without physical hardware.
JSValue jsInputInjectDevice(JSContext* ctx, JSValueConst, int argc,
                            JSValueConst* argv) {
    std::string name;
    if (!readActionName(ctx, argc, argv, 0, name))
        return JS_ThrowTypeError(ctx, "input.injectDevice(deviceName)");
    for (InputDevice device : {InputDevice::None, InputDevice::KeyboardMouse,
                               InputDevice::Gamepad, InputDevice::Touch}) {
        if (name == Input::deviceName(device)) {
            Input::injectDeviceActivity(device);
            return JS_NewBool(ctx, true);
        }
    }
    return JS_ThrowTypeError(ctx, "input.injectDevice: unknown device '%s'",
                             name.c_str());
}

JSValue jsInputRumble(JSContext* ctx, JSValueConst, int argc,
                      JSValueConst* argv) {
    if (argc < 3)
        return JS_ThrowTypeError(
            ctx, "input.rumble(lowFrequency, highFrequency, durationMs)");
    double lowFrequency = 0.0;
    double highFrequency = 0.0;
    int32_t durationMs = 0;
    if (JS_ToFloat64(ctx, &lowFrequency, argv[0]) != 0 ||
        JS_ToFloat64(ctx, &highFrequency, argv[1]) != 0 ||
        JS_ToInt32(ctx, &durationMs, argv[2]) != 0) {
        return JS_EXCEPTION;
    }
    if (!std::isfinite(lowFrequency) || !std::isfinite(highFrequency) ||
        lowFrequency < 0.0 || lowFrequency > 1.0 ||
        highFrequency < 0.0 || highFrequency > 1.0 ||
        durationMs < 0 || durationMs > 5000) {
        return JS_ThrowRangeError(
            ctx, "input.rumble: magnitudes must be within [0, 1] and "
                 "duration within [0, 5000] ms");
    }
    return JS_NewBool(
        ctx, Input::rumble(static_cast<float>(lowFrequency),
                           static_cast<float>(highFrequency),
                           static_cast<uint32_t>(durationMs)));
}

JSValue jsInputStopRumble(JSContext* ctx, JSValueConst, int,
                          JSValueConst*) {
    return JS_NewBool(ctx, Input::stopRumble());
}

JSValue jsTreeChangeScene(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree || argc < 1) return JS_NewBool(ctx, false);

    const char* path = JS_ToCString(ctx, argv[0]);
    bool ok = path != nullptr;
    if (ok) tree->changeScene(path);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

JSValue jsTreeReloadScene(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (SceneTree* tree = treeFromJs(ctx)) {
        tree->reloadScene();
        return JS_NewBool(ctx, true);
    }
    return JS_NewBool(ctx, false);
}

JSValue jsTreeQuit(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (SceneTree* tree = treeFromJs(ctx)) {
        tree->quit();
        return JS_NewBool(ctx, true);
    }
    return JS_NewBool(ctx, false);
}

JSValue jsTreeSetPaused(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree || argc < 1) return JS_NewBool(ctx, false);

    int paused = JS_ToBool(ctx, argv[0]);
    if (paused < 0) return JS_EXCEPTION;
    tree->setPaused(paused != 0);
    return JS_NewBool(ctx, true);
}

JSValue jsTreePaused(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    SceneTree* tree = treeFromJs(ctx);
    return JS_NewBool(ctx, tree && tree->paused());
}

JSValue jsTreeAutoload(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return JS_ThrowInternalError(ctx, "tree.autoload requires a mounted SceneTree");
    if (argc < 1) return JS_ThrowTypeError(ctx, "tree.autoload(name)");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    Node* target = tree->autoloadNode(raw);
    JS_FreeCString(ctx, raw);
    return target ? makeNodeRef(ctx, target) : JS_NULL;
}

JSValue jsTreeFirstInGroup(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return JS_ThrowInternalError(ctx, "tree.firstInGroup requires a mounted SceneTree");
    if (argc < 1) return JS_ThrowTypeError(ctx, "tree.firstInGroup(name)");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    Node* target = tree->firstInGroup(raw);
    JS_FreeCString(ctx, raw);
    return target ? makeNodeRef(ctx, target) : JS_NULL;
}

JSValue jsTreeNodesInGroup(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return JS_ThrowInternalError(ctx, "tree.nodesInGroup requires a mounted SceneTree");
    if (argc < 1) return JS_ThrowTypeError(ctx, "tree.nodesInGroup(name)");
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    const std::vector<Node*>& nodes = tree->group(raw);
    JS_FreeCString(ctx, raw);
    JSValue result = JS_NewArray(ctx);
    for (uint32_t i = 0; i < nodes.size(); ++i)
        JS_SetPropertyUint32(ctx, result, i, makeNodeRef(ctx, nodes[i]));
    return result;
}

JSValue jsTreeNodeById(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return JS_ThrowInternalError(ctx, "tree.nodeById requires a mounted SceneTree");
    if (argc < 1) return JS_ThrowTypeError(ctx, "tree.nodeById(id)");
    uint64_t id = 0;
    if (JS_ToBigUint64(ctx, &id, argv[0]) != 0) return JS_EXCEPTION;
    Node* target = tree->nodeById(id);
    return target ? makeNodeRef(ctx, target) : JS_NULL;
}

struct SignalHit { void* obj = nullptr; const reflect::SignalDesc* desc = nullptr; };

SignalHit findSignalOnNode(Node* node, const std::string& name) {
    if (!node) return {};
    auto& reg = reflect::TypeRegistry::instance();
    if (const auto* d = reg.find(node->typeName()))
        if (const auto* s = d->findSignal(name)) return {node, s};
    for (const auto& b : node->behaviours())
        if (const auto* d = reg.find(b->typeName() ? b->typeName() : ""))
            if (const auto* s = d->findSignal(name)) return {b.get(), s};
    return {};
}

JSValue jsonToJs(JSContext* ctx, const nlohmann::json& v) {
    if (v.is_null()) return JS_NULL;
    if (v.is_boolean()) return JS_NewBool(ctx, v.get<bool>());
    if (v.is_number_integer()) return JS_NewInt64(ctx, v.get<int64_t>());
    if (v.is_number_unsigned()) return JS_NewFloat64(ctx, static_cast<double>(v.get<uint64_t>()));
    if (v.is_number()) return JS_NewFloat64(ctx, v.get<double>());
    if (v.is_string()) return JS_NewString(ctx, v.get<std::string>().c_str());
    if (v.is_array()) {
        JSValue array = JS_NewArray(ctx);
        uint32_t index = 0;
        for (const auto& item : v)
            JS_SetPropertyUint32(ctx, array, index++, jsonToJs(ctx, item));
        return array;
    }
    JSValue object = JS_NewObject(ctx);
    for (auto it = v.begin(); it != v.end(); ++it)
        JS_SetPropertyStr(ctx, object, it.key().c_str(), jsonToJs(ctx, it.value()));
    return object;
}

bool jsToJson(JSContext* ctx, JSValueConst v, nlohmann::json& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        out = nullptr;
        return true;
    }
    if (JS_IsBool(v)) {
        out = JS_ToBool(ctx, v) != 0;
        return true;
    }
    if (JS_IsNumber(v)) {
        double d = 0.0;
        if (JS_ToFloat64(ctx, &d, v) != 0) return false;
        out = d;
        return true;
    }
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (!s) return false;
        out = std::string(s);
        JS_FreeCString(ctx, s);
        return true;
    }

    JSValue encoded = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(encoded)) {
        JS_FreeValue(ctx, encoded);
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        return false;
    }
    const char* text = JS_ToCString(ctx, encoded);
    if (!text) {
        JS_FreeValue(ctx, encoded);
        return false;
    }
    out = nlohmann::json::parse(text, nullptr, false);
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, encoded);
    return !out.is_discarded();
}

JSValue subscribeNodeSignal(JSContext* ctx, Node* node, int argc, JSValueConst* argv) {
    JsContext* self = JsContext::fromRaw(ctx);
    if (!node || !self || argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_NewBool(ctx, false);

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NewBool(ctx, false);
    std::string signalName = name;
    JS_FreeCString(ctx, name);

    SignalHit hit = findSignalOnNode(node, signalName);
    if (!hit.desc) {
        Log::warn("[JS] node.on: no signal '", signalName, "' on node '", node->name(), "'");
        return JS_NewBool(ctx, false);
    }

    // Retain one reference to the callback; the subscription frees it on context
    // teardown (hot-reload/destroy), which also disconnects this handler.
    JSValue callback = JS_DupValue(ctx, argv[1]);
    Connection conn = hit.desc->connect(hit.obj, [ctx, callback](const nlohmann::json& args) {
        std::vector<JSValue> jsArgs;
        jsArgs.reserve(args.size());
        for (const auto& a : args) jsArgs.push_back(jsonToJs(ctx, a));
        JSValue result = JS_Call(ctx, callback, JS_UNDEFINED,
                                 static_cast<int>(jsArgs.size()), jsArgs.data());
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(ctx);
            const char* msg = JS_ToCString(ctx, exc);
            Log::error("[JS] signal handler threw: ", msg ? msg : "unknown");
            JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, result);
        for (JSValue v : jsArgs) JS_FreeValue(ctx, v);
    });
    self->retainSignalSubscription(std::move(conn), callback);
    return JS_NewBool(ctx, true);
}

JSValue emitNodeSignal(JSContext* ctx, Node* node, int argc, JSValueConst* argv) {
    if (!node || argc < 1) return JS_NewBool(ctx, false);

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NewBool(ctx, false);
    std::string signalName = name;
    JS_FreeCString(ctx, name);

    SignalHit hit = findSignalOnNode(node, signalName);
    if (!hit.desc || !hit.desc->emit) {
        Log::warn("[JS] node.emit: no signal '", signalName, "' on node '", node->name(), "'");
        return JS_NewBool(ctx, false);
    }

    nlohmann::json arr = nlohmann::json::array();
    for (int i = 1; i < argc; ++i) {
        nlohmann::json value;
        if (!jsToJson(ctx, argv[i], value))
            return JS_ThrowTypeError(ctx, "signal arguments must be JSON-compatible");
        arr.push_back(std::move(value));
    }
    hit.desc->emit(hit.obj, arr);
    return JS_NewBool(ctx, true);
}

// ---- gameplay: animation / graph / sequences / blackboard ------------------
// Common resolution rule (the same as SequenceDirector's): the behaviour is
// looked up on the targeted node, otherwise the first one found among its
// descendants. Methods return false when the target doesn't have the
// behaviour — never an exception — so scripts can probe without guarding.

template <typename B>
B* behaviourOnOrUnder(Node* node) {
    if (!node) return nullptr;
    if (B* b = node->getBehaviour<B>()) return b;
    return node->findBehaviourInChildren<B>();
}

// Every Animator on or under the node, not just the first.
//
// The glTF import attaches one Animator per skinned mesh, so a character split
// into body + cap + hands carries several. Driving only the first leaves the
// rest of the body on whatever state it started in -- the character's head
// animates while his legs stand still. CharacterBehaviour already drives them
// all; the script API has to do the same.
std::vector<Animator*> animatorsOnOrUnder(Node* node) {
    std::vector<Animator*> out;
    if (!node) return out;
    if (Animator* self = node->getBehaviour<Animator>()) out.push_back(self);
    node->findBehavioursInChildren<Animator>(out);
    return out;
}

// node.playClip(name, loop=true, crossfade=0.2) — clip by name on the Animator.
// true = Animator found (an unknown clip name is logged by the engine).
JSValue gameplayPlayClip(JSContext* ctx, Node* target, int argc, JSValueConst* argv) {
    std::vector<Animator*> animators = animatorsOnOrUnder(target);
    if (animators.empty() || argc < 1) return JS_NewBool(ctx, false);
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string name = raw;
    JS_FreeCString(ctx, raw);

    bool loop = true;
    double crossfade = 0.2;
    if (argc >= 2) {
        const int b = JS_ToBool(ctx, argv[1]);
        if (b < 0) return JS_EXCEPTION;
        loop = b != 0;
    }
    if (argc >= 3 && !toNumber(ctx, argv[2], crossfade)) return JS_NewBool(ctx, false);

    for (Animator* a : animators) a->play(name, loop, static_cast<float>(crossfade));
    return JS_NewBool(ctx, true);
}

JSValue gameplayCurrentClip(JSContext* ctx, Node* target) {
    Animator* animator = behaviourOnOrUnder<Animator>(target);
    if (!animator) return JS_NULL;
    return JS_NewString(ctx, animator->currentClip().c_str());
}

enum class AnimParamKind { Float, Bool, Trigger };

// setAnimFloat/setAnimBool/setAnimTrigger — animation blackboard parameters
// (drive the transitions of a .sgraph).
JSValue gameplaySetAnimParam(JSContext* ctx, Node* target, AnimParamKind kind,
                             int argc, JSValueConst* argv) {
    std::vector<Animator*> animators = animatorsOnOrUnder(target);
    if (animators.empty() || argc < 1) return JS_NewBool(ctx, false);
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string name = raw;
    JS_FreeCString(ctx, raw);

    switch (kind) {
    case AnimParamKind::Float: {
        double value = 0.0;
        if (argc < 2 || !toNumber(ctx, argv[1], value)) return JS_NewBool(ctx, false);
        for (Animator* a : animators) a->setFloat(name, static_cast<float>(value));
        break;
    }
    case AnimParamKind::Bool: {
        if (argc < 2) return JS_NewBool(ctx, false);
        const int b = JS_ToBool(ctx, argv[1]);
        if (b < 0) return JS_EXCEPTION;
        for (Animator* a : animators) a->setBool(name, b != 0);
        break;
    }
    case AnimParamKind::Trigger:
        for (Animator* a : animators) a->setTrigger(name);
        break;
    }
    return JS_NewBool(ctx, true);
}

// playSequence()/stopSequence() — SequenceDirector of the node (or a descendant).
JSValue gameplaySequence(JSContext* ctx, Node* target, bool play) {
    auto* director = behaviourOnOrUnder<SequenceDirectorBehaviour>(target);
    if (!director) return JS_NewBool(ctx, false);
    if (play) director->play();
    else director->stop();
    return JS_NewBool(ctx, true);
}

// setData/getData/hasData — gameplay Blackboard of the node (or a descendant).
// number/bool/string values, same as the C++ store.
JSValue gameplaySetData(JSContext* ctx, Node* target, int argc, JSValueConst* argv) {
    Blackboard* board = behaviourOnOrUnder<Blackboard>(target);
    if (!board || argc < 2) return JS_NewBool(ctx, false);
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string key = raw;
    JS_FreeCString(ctx, raw);

    JSValueConst v = argv[1];
    if (JS_IsBool(v)) {
        board->setBool(std::move(key), JS_ToBool(ctx, v) != 0);
    } else if (JS_IsNumber(v)) {
        double d = 0.0;
        if (JS_ToFloat64(ctx, &d, v) != 0) return JS_EXCEPTION;
        board->setNumber(std::move(key), d);
    } else if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (!s) return JS_EXCEPTION;
        board->setString(std::move(key), s);
        JS_FreeCString(ctx, s);
    } else {
        return JS_ThrowTypeError(ctx, "setData value must be number, bool or string");
    }
    return JS_NewBool(ctx, true);
}

JSValue gameplayGetData(JSContext* ctx, Node* target, int argc, JSValueConst* argv) {
    Blackboard* board = behaviourOnOrUnder<Blackboard>(target);
    if (!board || argc < 1) return JS_NULL;
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    std::string key = raw;
    JS_FreeCString(ctx, raw);

    const Blackboard::Value* value = board->find(key);
    if (!value) return argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_NULL;
    if (const double* d = std::get_if<double>(value)) return JS_NewFloat64(ctx, *d);
    if (const bool* b = std::get_if<bool>(value)) return JS_NewBool(ctx, *b);
    return JS_NewString(ctx, std::get<std::string>(*value).c_str());
}

JSValue gameplayHasData(JSContext* ctx, Node* target, int argc, JSValueConst* argv) {
    Blackboard* board = behaviourOnOrUnder<Blackboard>(target);
    if (!board || argc < 1) return JS_NewBool(ctx, false);
    const char* raw = JS_ToCString(ctx, argv[0]);
    if (!raw) return JS_EXCEPTION;
    const bool has = board->has(raw);
    JS_FreeCString(ctx, raw);
    return JS_NewBool(ctx, has);
}

// Self variants (`node.*`) of the gameplay methods.
JSValue jsNodePlayClip(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return gameplayPlayClip(ctx, nodeFromJs(ctx), argc, argv);
}
JSValue jsNodeCurrentClip(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return gameplayCurrentClip(ctx, nodeFromJs(ctx));
}
JSValue jsNodeSetAnimFloat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return gameplaySetAnimParam(ctx, nodeFromJs(ctx), AnimParamKind::Float, argc, argv);
}
JSValue jsNodeSetAnimBool(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return gameplaySetAnimParam(ctx, nodeFromJs(ctx), AnimParamKind::Bool, argc, argv);
}
JSValue jsNodeSetAnimTrigger(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return gameplaySetAnimParam(ctx, nodeFromJs(ctx), AnimParamKind::Trigger, argc, argv);
}
JSValue jsNodePlaySequence(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return gameplaySequence(ctx, nodeFromJs(ctx), true);
}
JSValue jsNodeStopSequence(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return gameplaySequence(ctx, nodeFromJs(ctx), false);
}
JSValue jsNodeSetData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return gameplaySetData(ctx, nodeFromJs(ctx), argc, argv);
}
JSValue jsNodeGetData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return gameplayGetData(ctx, nodeFromJs(ctx), argc, argv);
}
JSValue jsNodeHasData(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return gameplayHasData(ctx, nodeFromJs(ctx), argc, argv);
}

JSValue jsNodeOn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return subscribeNodeSignal(ctx, nodeFromJs(ctx), argc, argv);
}

JSValue jsNodeEmit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return emitNodeSignal(ctx, nodeFromJs(ctx), argc, argv);
}

Node* nodeRefTarget(JSContext* ctx, JSValueConst* data) {
    SceneTree* tree = treeFromJs(ctx);
    if (!tree) return nullptr;
    uint64_t id = 0;
    if (JS_ToBigUint64(ctx, &id, data[0]) != 0) return nullptr;
    return tree->nodeById(id);
}

enum NodeRefMethod {
    NodeRefValid,
    NodeRefGetName,
    NodeRefSetName,
    NodeRefGetPosition,
    NodeRefSetPosition,
    NodeRefTranslate,
    NodeRefSetEnabled,
    NodeRefQueueFree,
    NodeRefSetText,
    NodeRefGetText,
    NodeRefAddToGroup,
    NodeRefRemoveFromGroup,
    NodeRefIsInGroup,
    NodeRefPlayClip,
    NodeRefCurrentClip,
    NodeRefSetAnimFloat,
    NodeRefSetAnimBool,
    NodeRefSetAnimTrigger,
    NodeRefPlaySequence,
    NodeRefStopSequence,
    NodeRefSetData,
    NodeRefGetData,
    NodeRefHasData,
    NodeRefGetRotation,
    NodeRefSetRotation,
    NodeRefSetVelocity,
    NodeRefGetVelocity,
    NodeRefIsOnFloor,
    NodeRefIsOnSteepSlope,
    NodeRefGroundNormal,
};

JSValue jsNodeRefMethod(JSContext* ctx, JSValueConst, int argc,
                        JSValueConst* argv, int magic, JSValueConst* data) {
    Node* target = nodeRefTarget(ctx, data);
    if (magic == NodeRefValid) return JS_NewBool(ctx, target != nullptr);
    if (!target) return JS_ThrowReferenceError(ctx, "NodeRef target no longer exists");

    switch (magic) {
    case NodeRefGetName:
        return JS_NewString(ctx, target->name().c_str());
    case NodeRefSetName: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "NodeRef.setName(name)");
        const char* value = JS_ToCString(ctx, argv[0]);
        if (!value) return JS_EXCEPTION;
        target->setName(value);
        JS_FreeCString(ctx, value);
        return JS_NewBool(ctx, true);
    }
    case NodeRefGetPosition:
        return makeVec3(ctx, target->transform().position);
    case NodeRefGetRotation:
        return makeQuat(ctx, target->transform().rotation);
    case NodeRefSetRotation: {
        glm::quat value(1.0f, 0.0f, 0.0f, 0.0f);
        if (!readQuatArgs(ctx, argc, argv, value))
            return JS_ThrowTypeError(ctx, "expected ({x,y,z,w}) or (x,y,z,w)");
        target->transform().rotation = value;
        return JS_NewBool(ctx, true);
    }
    case NodeRefSetVelocity: {
        CharacterBodyNode* body = characterFor(target);
        if (!body) return JS_NewBool(ctx, false);
        glm::vec3 value(0.0f);
        if (!readVec3Args(ctx, argc, argv, value))
            return JS_ThrowTypeError(ctx, "expected ({x,y,z}) or (x,y,z)");
        body->velocity = value;
        return JS_NewBool(ctx, true);
    }
    case NodeRefGetVelocity: {
        CharacterBodyNode* body = characterFor(target);
        return makeVec3(ctx, body ? body->velocity : glm::vec3(0.0f));
    }
    case NodeRefIsOnFloor: {
        CharacterBodyNode* body = characterFor(target);
        return JS_NewBool(ctx, body && body->isOnFloor());
    }
    case NodeRefIsOnSteepSlope: {
        CharacterBodyNode* body = characterFor(target);
        return JS_NewBool(ctx, body && body->isOnSteepSlope());
    }
    case NodeRefGroundNormal: {
        CharacterBodyNode* body = characterFor(target);
        return makeVec3(ctx, body ? body->groundNormal() : glm::vec3(0.0f, 1.0f, 0.0f));
    }
    case NodeRefSetPosition:
    case NodeRefTranslate: {
        glm::vec3 value(0.0f);
        if (!readVec3Args(ctx, argc, argv, value))
            return JS_ThrowTypeError(ctx, "expected ({x,y,z}) or (x,y,z)");
        if (magic == NodeRefSetPosition) target->transform().position = value;
        else target->transform().position += value;
        return JS_NewBool(ctx, true);
    }
    case NodeRefSetEnabled: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "NodeRef.setEnabled(enabled)");
        const int enabled = JS_ToBool(ctx, argv[0]);
        if (enabled < 0) return JS_EXCEPTION;
        target->setEnabled(enabled != 0);
        return JS_NewBool(ctx, true);
    }
    case NodeRefQueueFree:
        target->queueFree();
        return JS_UNDEFINED;
    case NodeRefSetText: {
        auto* text = dynamic_cast<UITextNode*>(target);
        if (!text) return JS_ThrowTypeError(ctx, "NodeRef target is not a UITextNode");
        if (argc < 1) return JS_ThrowTypeError(ctx, "NodeRef.setText(text)");
        const char* value = JS_ToCString(ctx, argv[0]);
        if (!value) return JS_EXCEPTION;
        text->setText(value);
        JS_FreeCString(ctx, value);
        return JS_UNDEFINED;
    }
    case NodeRefGetText: {
        auto* text = dynamic_cast<UITextNode*>(target);
        if (!text) return JS_ThrowTypeError(ctx, "NodeRef target is not a UITextNode");
        return JS_NewString(ctx, text->text().c_str());
    }
    case NodeRefPlayClip:
        return gameplayPlayClip(ctx, target, argc, argv);
    case NodeRefCurrentClip:
        return gameplayCurrentClip(ctx, target);
    case NodeRefSetAnimFloat:
        return gameplaySetAnimParam(ctx, target, AnimParamKind::Float, argc, argv);
    case NodeRefSetAnimBool:
        return gameplaySetAnimParam(ctx, target, AnimParamKind::Bool, argc, argv);
    case NodeRefSetAnimTrigger:
        return gameplaySetAnimParam(ctx, target, AnimParamKind::Trigger, argc, argv);
    case NodeRefPlaySequence:
        return gameplaySequence(ctx, target, true);
    case NodeRefStopSequence:
        return gameplaySequence(ctx, target, false);
    case NodeRefSetData:
        return gameplaySetData(ctx, target, argc, argv);
    case NodeRefGetData:
        return gameplayGetData(ctx, target, argc, argv);
    case NodeRefHasData:
        return gameplayHasData(ctx, target, argc, argv);
    case NodeRefAddToGroup:
    case NodeRefRemoveFromGroup:
    case NodeRefIsInGroup: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "NodeRef group method requires a name");
        const char* group = JS_ToCString(ctx, argv[0]);
        if (!group) return JS_EXCEPTION;
        bool result = true;
        if (magic == NodeRefAddToGroup) target->addToGroup(group);
        else if (magic == NodeRefRemoveFromGroup) target->removeFromGroup(group);
        else result = target->isInGroup(group);
        JS_FreeCString(ctx, group);
        return JS_NewBool(ctx, result);
    }
    default:
        return JS_ThrowInternalError(ctx, "unknown NodeRef method");
    }
}

JSValue jsNodeRefOn(JSContext* ctx, JSValueConst, int argc,
                    JSValueConst* argv, int, JSValueConst* data) {
    Node* target = nodeRefTarget(ctx, data);
    if (!target) return JS_ThrowReferenceError(ctx, "NodeRef target no longer exists");
    return subscribeNodeSignal(ctx, target, argc, argv);
}

JSValue jsNodeRefEmit(JSContext* ctx, JSValueConst, int argc,
                      JSValueConst* argv, int, JSValueConst* data) {
    Node* target = nodeRefTarget(ctx, data);
    if (!target) return JS_ThrowReferenceError(ctx, "NodeRef target no longer exists");
    return emitNodeSignal(ctx, target, argc, argv);
}

JSValue jsNodeRefCall(JSContext* ctx, JSValueConst, int argc,
                      JSValueConst* argv, int, JSValueConst* data) {
    Node* target = nodeRefTarget(ctx, data);
    if (!target) return JS_ThrowReferenceError(ctx, "NodeRef target no longer exists");
    if (argc < 1) return JS_ThrowTypeError(ctx, "NodeRef.call(exportName, ...args)");

    const char* rawName = JS_ToCString(ctx, argv[0]);
    if (!rawName) return JS_EXCEPTION;
    const std::string exportName(rawName);
    JS_FreeCString(ctx, rawName);

    nlohmann::json args = nlohmann::json::array();
    for (int i = 1; i < argc; ++i) {
        nlohmann::json value;
        if (!jsToJson(ctx, argv[i], value))
            return JS_ThrowTypeError(ctx, "NodeRef.call arguments must be JSON-compatible");
        args.push_back(std::move(value));
    }

    for (const auto& behaviour : target->behaviours()) {
        auto* script = dynamic_cast<ScriptBehaviour*>(behaviour.get());
        if (!script) continue;
        nlohmann::json result;
        const ScriptCallStatus status = script->callExport(exportName, args, result);
        if (status == ScriptCallStatus::Succeeded) return jsonToJs(ctx, result);
        if (status == ScriptCallStatus::Failed)
            return JS_ThrowInternalError(ctx, "NodeRef.call('%s') failed", exportName.c_str());
    }
    return JS_ThrowTypeError(ctx, "NodeRef target has no callable export '%s'",
                             exportName.c_str());
}


// ---- physics: scene queries (raycast / overlap) ----------------------------
// Desktop/web parity: the web player embeds the same Jolt (wasm). The physics
// world only exists in Play with at least one body; without a world, queries
// answer "nothing" (null / empty list) rather than an error — a script can
// query a scene without physics without guarding.

PhysicsWorld* physicsFromJs(JSContext* ctx) {
    SceneTree* tree = treeFromJs(ctx);
    return tree ? tree->world().physics() : nullptr;
}

// Reads an {x,y,z} at the given index; throws a TypeError otherwise.
bool readVec3At(JSContext* ctx, int argc, JSValueConst* argv, int index, glm::vec3& out) {
    if (index >= argc || !JS_IsObject(argv[index])) return false;
    return readVec3Args(ctx, 1, argv + index, out);
}

// Common options { hitSensors?: bool, ignoreSelf?: bool } (defaults: false, true).
struct JsQueryOptions {
    bool hitSensors = false;
    bool ignoreSelf = true;
};

JsQueryOptions readQueryOptions(JSContext* ctx, int argc, JSValueConst* argv, int index) {
    JsQueryOptions out;
    if (index >= argc || !JS_IsObject(argv[index])) return out;
    JSValue sensors = JS_GetPropertyStr(ctx, argv[index], "hitSensors");
    if (!JS_IsUndefined(sensors)) out.hitSensors = JS_ToBool(ctx, sensors) > 0;
    JS_FreeValue(ctx, sensors);
    JSValue self = JS_GetPropertyStr(ctx, argv[index], "ignoreSelf");
    if (!JS_IsUndefined(self)) out.ignoreSelf = JS_ToBool(ctx, self) > 0;
    JS_FreeValue(ctx, self);
    return out;
}

// The body of the calling node (or its nearest ancestor) — excluded by
// default from queries so a script doesn't touch itself.
JPH::BodyID callerBodyId(JSContext* ctx) {
    for (Node* n = nodeFromJs(ctx); n; n = n->parent())
        if (CollisionObjectNode* body = n->asCollisionObject()) return body->bodyId();
    return JPH::BodyID();
}

QueryFilter makeQueryFilter(JSContext* ctx, const JsQueryOptions& opts) {
    QueryFilter filter;
    filter.hitSensors = opts.hitSensors;
    if (opts.ignoreSelf) filter.ignore = callerBodyId(ctx);
    return filter;
}

JSValue physicsNodeRef(JSContext* ctx, PhysicsWorld& world, JPH::BodyID body) {
    auto* node = static_cast<CollisionObjectNode*>(world.bodyUserData(body));
    return node ? makeNodeRef(ctx, node) : JS_NULL;
}

// physics.raycast(origin, direction, maxDistance, opts?) →
//   null | { point, normal, distance, node: NodeRef|null }
JSValue jsPhysicsRaycast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    glm::vec3 origin(0.0f);
    glm::vec3 direction(0.0f);
    double maxDistance = 0.0;
    if (!readVec3At(ctx, argc, argv, 0, origin) ||
        !readVec3At(ctx, argc, argv, 1, direction) ||
        argc < 3 || !toNumber(ctx, argv[2], maxDistance)) {
        return JS_ThrowTypeError(
            ctx, "physics.raycast(origin{x,y,z}, direction{x,y,z}, maxDistance, opts?)");
    }
    if (glm::dot(direction, direction) < 1e-12f || !(maxDistance > 0.0))
        return JS_NULL;

    PhysicsWorld* world = physicsFromJs(ctx);
    if (!world) return JS_NULL;

    const QueryFilter filter = makeQueryFilter(ctx, readQueryOptions(ctx, argc, argv, 3));
    const RaycastHit hit = world->raycast(origin, glm::normalize(direction),
                                          static_cast<float>(maxDistance), filter);
    if (!hit.hit) return JS_NULL;

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "point", makeVec3(ctx, hit.point));
    JS_SetPropertyStr(ctx, o, "normal", makeVec3(ctx, hit.normal));
    JS_SetPropertyStr(ctx, o, "distance", JS_NewFloat64(ctx, hit.distance));
    JS_SetPropertyStr(ctx, o, "node", physicsNodeRef(ctx, *world, hit.body));
    return o;
}

// physics.overlapSphere(center, radius, opts?) → [NodeRef...]
JSValue jsPhysicsOverlapSphere(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    glm::vec3 center(0.0f);
    double radius = 0.0;
    if (!readVec3At(ctx, argc, argv, 0, center) ||
        argc < 2 || !toNumber(ctx, argv[1], radius)) {
        return JS_ThrowTypeError(ctx,
                                 "physics.overlapSphere(center{x,y,z}, radius, opts?)");
    }

    JSValue arr = JS_NewArray(ctx);
    PhysicsWorld* world = physicsFromJs(ctx);
    if (!world || !(radius > 0.0)) return arr;

    const QueryFilter filter = makeQueryFilter(ctx, readQueryOptions(ctx, argc, argv, 2));
    uint32_t i = 0;
    for (JPH::BodyID body : world->overlapSphere(center, static_cast<float>(radius), filter)) {
        JSValue ref = physicsNodeRef(ctx, *world, body);
        if (JS_IsNull(ref)) continue;  // body with no owning node
        JS_SetPropertyUint32(ctx, arr, i++, ref);
    }
    return arr;
}

// physics.available() → true as soon as the physics capability exists on the
// platform (the world itself is created lazily in Play).
JSValue jsPhysicsAvailable(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, platform::has(platform::Capability::Physics));
}

void setNodeRefMethod(JSContext* ctx, JSValue object, NodeId id,
                      const char* name, JSCFunctionData* fn, int length,
                      int magic = 0) {
    JSValue data = JS_NewBigUint64(ctx, id);
    JSValue method = JS_NewCFunctionData(ctx, fn, length, magic, 1, &data);
    JS_FreeValue(ctx, data);
    JS_SetPropertyStr(ctx, object, name, method);
}

JSValue makeNodeRef(JSContext* ctx, Node* target) {
    if (!target) return JS_NULL;
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "id", JS_NewBigUint64(ctx, target->id()));
    setNodeRefMethod(ctx, object, target->id(), "valid", jsNodeRefMethod, 0, NodeRefValid);
    setNodeRefMethod(ctx, object, target->id(), "getName", jsNodeRefMethod, 0, NodeRefGetName);
    setNodeRefMethod(ctx, object, target->id(), "setName", jsNodeRefMethod, 1, NodeRefSetName);
    setNodeRefMethod(ctx, object, target->id(), "getPosition", jsNodeRefMethod, 0, NodeRefGetPosition);
    setNodeRefMethod(ctx, object, target->id(), "setPosition", jsNodeRefMethod, 3, NodeRefSetPosition);
    setNodeRefMethod(ctx, object, target->id(), "translate", jsNodeRefMethod, 3, NodeRefTranslate);
    setNodeRefMethod(ctx, object, target->id(), "getRotation", jsNodeRefMethod, 0, NodeRefGetRotation);
    setNodeRefMethod(ctx, object, target->id(), "setRotation", jsNodeRefMethod, 4, NodeRefSetRotation);
    setNodeRefMethod(ctx, object, target->id(), "setVelocity", jsNodeRefMethod, 3, NodeRefSetVelocity);
    setNodeRefMethod(ctx, object, target->id(), "getVelocity", jsNodeRefMethod, 0, NodeRefGetVelocity);
    setNodeRefMethod(ctx, object, target->id(), "isOnFloor", jsNodeRefMethod, 0, NodeRefIsOnFloor);
    setNodeRefMethod(ctx, object, target->id(), "isOnSteepSlope", jsNodeRefMethod, 0, NodeRefIsOnSteepSlope);
    setNodeRefMethod(ctx, object, target->id(), "groundNormal", jsNodeRefMethod, 0, NodeRefGroundNormal);
    setNodeRefMethod(ctx, object, target->id(), "setEnabled", jsNodeRefMethod, 1, NodeRefSetEnabled);
    setNodeRefMethod(ctx, object, target->id(), "queueFree", jsNodeRefMethod, 0, NodeRefQueueFree);
    setNodeRefMethod(ctx, object, target->id(), "setText", jsNodeRefMethod, 1, NodeRefSetText);
    setNodeRefMethod(ctx, object, target->id(), "getText", jsNodeRefMethod, 0, NodeRefGetText);
    setNodeRefMethod(ctx, object, target->id(), "addToGroup", jsNodeRefMethod, 1, NodeRefAddToGroup);
    setNodeRefMethod(ctx, object, target->id(), "removeFromGroup", jsNodeRefMethod, 1, NodeRefRemoveFromGroup);
    setNodeRefMethod(ctx, object, target->id(), "isInGroup", jsNodeRefMethod, 1, NodeRefIsInGroup);
    setNodeRefMethod(ctx, object, target->id(), "playClip", jsNodeRefMethod, 3, NodeRefPlayClip);
    setNodeRefMethod(ctx, object, target->id(), "currentClip", jsNodeRefMethod, 0, NodeRefCurrentClip);
    setNodeRefMethod(ctx, object, target->id(), "setAnimFloat", jsNodeRefMethod, 2, NodeRefSetAnimFloat);
    setNodeRefMethod(ctx, object, target->id(), "setAnimBool", jsNodeRefMethod, 2, NodeRefSetAnimBool);
    setNodeRefMethod(ctx, object, target->id(), "setAnimTrigger", jsNodeRefMethod, 1, NodeRefSetAnimTrigger);
    setNodeRefMethod(ctx, object, target->id(), "playSequence", jsNodeRefMethod, 0, NodeRefPlaySequence);
    setNodeRefMethod(ctx, object, target->id(), "stopSequence", jsNodeRefMethod, 0, NodeRefStopSequence);
    setNodeRefMethod(ctx, object, target->id(), "setData", jsNodeRefMethod, 2, NodeRefSetData);
    setNodeRefMethod(ctx, object, target->id(), "getData", jsNodeRefMethod, 2, NodeRefGetData);
    setNodeRefMethod(ctx, object, target->id(), "hasData", jsNodeRefMethod, 1, NodeRefHasData);
    setNodeRefMethod(ctx, object, target->id(), "on", jsNodeRefOn, 2);
    setNodeRefMethod(ctx, object, target->id(), "emit", jsNodeRefEmit, 1);
    setNodeRefMethod(ctx, object, target->id(), "call", jsNodeRefCall, 1);
    return object;
}

} // namespace

void JsEngineBindings::installForBehaviour(JsContext& context, Behaviour& behaviour) {
    JSContext* ctx = context.raw();
    context.setOpaque(&behaviour);

    JSValue global = JS_GetGlobalObject(ctx);
    installAssetHandleClass(ctx);

    JSValue node = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, node, "getName", JS_NewCFunction(ctx, jsNodeGetName, "getName", 0));
    JS_SetPropertyStr(ctx, node, "setName", JS_NewCFunction(ctx, jsNodeSetName, "setName", 1));
    JS_SetPropertyStr(ctx, node, "getPosition", JS_NewCFunction(ctx, jsNodeGetPosition, "getPosition", 0));
    JS_SetPropertyStr(ctx, node, "setPosition", JS_NewCFunction(ctx, jsNodeSetPosition, "setPosition", 3));
    JS_SetPropertyStr(ctx, node, "translate", JS_NewCFunction(ctx, jsNodeTranslate, "translate", 3));
    JS_SetPropertyStr(ctx, node, "getRotation", JS_NewCFunction(ctx, jsNodeGetRotation, "getRotation", 0));
    JS_SetPropertyStr(ctx, node, "setRotation", JS_NewCFunction(ctx, jsNodeSetRotation, "setRotation", 4));
    JS_SetPropertyStr(ctx, node, "setVelocity", JS_NewCFunction(ctx, jsNodeSetVelocity, "setVelocity", 3));
    JS_SetPropertyStr(ctx, node, "getVelocity", JS_NewCFunction(ctx, jsNodeGetVelocity, "getVelocity", 0));
    JS_SetPropertyStr(ctx, node, "isOnFloor", JS_NewCFunction(ctx, jsNodeIsOnFloor, "isOnFloor", 0));
    JS_SetPropertyStr(ctx, node, "isOnSteepSlope", JS_NewCFunction(ctx, jsNodeIsOnSteepSlope, "isOnSteepSlope", 0));
    JS_SetPropertyStr(ctx, node, "groundNormal", JS_NewCFunction(ctx, jsNodeGroundNormal, "groundNormal", 0));
    JS_SetPropertyStr(ctx, node, "setEnabled", JS_NewCFunction(ctx, jsNodeSetEnabled, "setEnabled", 1));
    JS_SetPropertyStr(ctx, node, "queueFree", JS_NewCFunction(ctx, jsNodeQueueFree, "queueFree", 0));
    JS_SetPropertyStr(ctx, node, "setText", JS_NewCFunction(ctx, jsNodeSetText, "setText", 1));
    JS_SetPropertyStr(ctx, node, "getText", JS_NewCFunction(ctx, jsNodeGetText, "getText", 0));
    JS_SetPropertyStr(ctx, node, "addToGroup", JS_NewCFunction(ctx, jsNodeAddToGroup, "addToGroup", 1));
    JS_SetPropertyStr(ctx, node, "removeFromGroup", JS_NewCFunction(ctx, jsNodeRemoveFromGroup, "removeFromGroup", 1));
    JS_SetPropertyStr(ctx, node, "isInGroup", JS_NewCFunction(ctx, jsNodeIsInGroup, "isInGroup", 1));
    JS_SetPropertyStr(ctx, node, "on", JS_NewCFunction(ctx, jsNodeOn, "on", 2));
    JS_SetPropertyStr(ctx, node, "emit", JS_NewCFunction(ctx, jsNodeEmit, "emit", 1));
    JS_SetPropertyStr(ctx, node, "playClip", JS_NewCFunction(ctx, jsNodePlayClip, "playClip", 3));
    JS_SetPropertyStr(ctx, node, "currentClip", JS_NewCFunction(ctx, jsNodeCurrentClip, "currentClip", 0));
    JS_SetPropertyStr(ctx, node, "setAnimFloat", JS_NewCFunction(ctx, jsNodeSetAnimFloat, "setAnimFloat", 2));
    JS_SetPropertyStr(ctx, node, "setAnimBool", JS_NewCFunction(ctx, jsNodeSetAnimBool, "setAnimBool", 2));
    JS_SetPropertyStr(ctx, node, "setAnimTrigger", JS_NewCFunction(ctx, jsNodeSetAnimTrigger, "setAnimTrigger", 1));
    JS_SetPropertyStr(ctx, node, "playSequence", JS_NewCFunction(ctx, jsNodePlaySequence, "playSequence", 0));
    JS_SetPropertyStr(ctx, node, "stopSequence", JS_NewCFunction(ctx, jsNodeStopSequence, "stopSequence", 0));
    JS_SetPropertyStr(ctx, node, "setData", JS_NewCFunction(ctx, jsNodeSetData, "setData", 2));
    JS_SetPropertyStr(ctx, node, "getData", JS_NewCFunction(ctx, jsNodeGetData, "getData", 2));
    JS_SetPropertyStr(ctx, node, "hasData", JS_NewCFunction(ctx, jsNodeHasData, "hasData", 1));
    JS_SetPropertyStr(ctx, global, "node", node);

    JSValue time = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, time, "delta", JS_NewCFunction(ctx, jsTimeDelta, "delta", 0));
    JS_SetPropertyStr(ctx, time, "elapsed", JS_NewCFunction(ctx, jsTimeElapsed, "elapsed", 0));
    JsTimerBindings::install(context, behaviour, time);
    JS_SetPropertyStr(ctx, global, "time", time);

    JSValue input = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, input, "isHeld", JS_NewCFunction(ctx, jsInputIsHeld, "isHeld", 1));
    JS_SetPropertyStr(ctx, input, "justPressed", JS_NewCFunction(ctx, jsInputJustPressed, "justPressed", 1));
    JS_SetPropertyStr(ctx, input, "justReleased", JS_NewCFunction(ctx, jsInputJustReleased, "justReleased", 1));
    JS_SetPropertyStr(ctx, input, "strength", JS_NewCFunction(ctx, jsInputStrength, "strength", 1));
    JS_SetPropertyStr(ctx, input, "axis", JS_NewCFunction(ctx, jsInputAxis, "axis", 2));
    JS_SetPropertyStr(ctx, input, "vector", JS_NewCFunction(ctx, jsInputVector, "vector", 4));
    JS_SetPropertyStr(ctx, input, "inject", JS_NewCFunction(ctx, jsInputInject, "inject", 2));
    JS_SetPropertyStr(ctx, input, "injectDevice",
                      JS_NewCFunction(ctx, jsInputInjectDevice, "injectDevice", 1));
    JS_SetPropertyStr(ctx, input, "mousePosition", JS_NewCFunction(ctx, jsInputMousePosition, "mousePosition", 0));
    JS_SetPropertyStr(ctx, input, "mouseDelta", JS_NewCFunction(ctx, jsInputMouseDelta, "mouseDelta", 0));
    JS_SetPropertyStr(ctx, input, "rebindKey", JS_NewCFunction(ctx, jsInputRebindKey, "rebindKey", 3));
    JS_SetPropertyStr(ctx, input, "rebindMouse", JS_NewCFunction(ctx, jsInputRebindMouse, "rebindMouse", 3));
    JS_SetPropertyStr(ctx, input, "rebindGamepadButton",
                      JS_NewCFunction(ctx, jsInputRebindGamepadButton,
                                      "rebindGamepadButton", 3));
    JS_SetPropertyStr(ctx, input, "rebindGamepadAxis",
                      JS_NewCFunction(ctx, jsInputRebindGamepadAxis,
                                      "rebindGamepadAxis", 5));
    JS_SetPropertyStr(ctx, input, "rebindTouch",
                      JS_NewCFunction(ctx, jsInputRebindTouch,
                                      "rebindTouch", 8));
    JS_SetPropertyStr(ctx, input, "exportProfile",
                      JS_NewCFunction(ctx, jsInputExportProfile,
                                      "exportProfile", 1));
    JS_SetPropertyStr(ctx, input, "applyProfile",
                      JS_NewCFunction(ctx, jsInputApplyProfile,
                                      "applyProfile", 1));
    JS_SetPropertyStr(ctx, input, "lastActiveDevice",
                      JS_NewCFunction(ctx, jsInputLastActiveDevice,
                                      "lastActiveDevice", 0));
    JS_SetPropertyStr(ctx, input, "rumble",
                      JS_NewCFunction(ctx, jsInputRumble, "rumble", 3));
    JS_SetPropertyStr(ctx, input, "stopRumble",
                      JS_NewCFunction(ctx, jsInputStopRumble,
                                      "stopRumble", 0));
    JS_SetPropertyStr(ctx, global, "input", input);

    JSValue tree = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tree, "changeScene", JS_NewCFunction(ctx, jsTreeChangeScene, "changeScene", 1));
    JS_SetPropertyStr(ctx, tree, "reloadScene", JS_NewCFunction(ctx, jsTreeReloadScene, "reloadScene", 0));
    JS_SetPropertyStr(ctx, tree, "quit", JS_NewCFunction(ctx, jsTreeQuit, "quit", 0));
    JS_SetPropertyStr(ctx, tree, "setPaused", JS_NewCFunction(ctx, jsTreeSetPaused, "setPaused", 1));
    JS_SetPropertyStr(ctx, tree, "paused", JS_NewCFunction(ctx, jsTreePaused, "paused", 0));
    JS_SetPropertyStr(ctx, tree, "autoload", JS_NewCFunction(ctx, jsTreeAutoload, "autoload", 1));
    JS_SetPropertyStr(ctx, tree, "firstInGroup", JS_NewCFunction(ctx, jsTreeFirstInGroup, "firstInGroup", 1));
    JS_SetPropertyStr(ctx, tree, "nodesInGroup", JS_NewCFunction(ctx, jsTreeNodesInGroup, "nodesInGroup", 1));
    JS_SetPropertyStr(ctx, tree, "nodeById", JS_NewCFunction(ctx, jsTreeNodeById, "nodeById", 1));
    JS_SetPropertyStr(ctx, global, "tree", tree);

    JSValue assets = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, assets, "load", JS_NewCFunction(ctx, jsAssetsLoad, "load", 2));
    JS_SetPropertyStr(ctx, assets, "stats", JS_NewCFunction(ctx, jsAssetsStats, "stats", 0));
    JS_SetPropertyStr(ctx, assets, "setGpuBudget", JS_NewCFunction(ctx, jsAssetsSetGpuBudget, "setGpuBudget", 1));
    JS_SetPropertyStr(ctx, global, "assets", assets);

    JSValue audio = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, audio, "play", JS_NewCFunction(ctx, jsAudioPlay, "play", 1));
    JS_SetPropertyStr(ctx, global, "audio", audio);

    JSValue physics = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, physics, "available", JS_NewCFunction(ctx, jsPhysicsAvailable, "available", 0));
    JS_SetPropertyStr(ctx, physics, "raycast", JS_NewCFunction(ctx, jsPhysicsRaycast, "raycast", 4));
    JS_SetPropertyStr(ctx, physics, "overlapSphere", JS_NewCFunction(ctx, jsPhysicsOverlapSphere, "overlapSphere", 3));
    JS_SetPropertyStr(ctx, global, "physics", physics);

    JSValue storage = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, storage, "save", JS_NewCFunction(ctx, jsStorageSave, "save", 3));
    JS_SetPropertyStr(ctx, storage, "load", JS_NewCFunction(ctx, jsStorageLoad, "load", 1));
    JS_SetPropertyStr(ctx, storage, "has", JS_NewCFunction(ctx, jsStorageHas, "has", 1));
    JS_SetPropertyStr(ctx, storage, "remove", JS_NewCFunction(ctx, jsStorageRemove, "remove", 1));
    JS_SetPropertyStr(ctx, storage, "info", JS_NewCFunction(ctx, jsStorageInfo, "info", 1));
    JS_SetPropertyStr(ctx, storage, "list", JS_NewCFunction(ctx, jsStorageList, "list", 0));
    JS_SetPropertyStr(ctx, storage, "lastError", JS_NewCFunction(ctx, jsStorageLastError, "lastError", 0));
    // storage.prefs.*: preferences, a namespace separate from progress.
    JSValue prefs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, prefs, "save", JS_NewCFunction(ctx, jsPrefSave, "save", 3));
    JS_SetPropertyStr(ctx, prefs, "load", JS_NewCFunction(ctx, jsPrefLoad, "load", 1));
    JS_SetPropertyStr(ctx, prefs, "has", JS_NewCFunction(ctx, jsPrefHas, "has", 1));
    JS_SetPropertyStr(ctx, prefs, "remove", JS_NewCFunction(ctx, jsPrefRemove, "remove", 1));
    JS_SetPropertyStr(ctx, prefs, "info", JS_NewCFunction(ctx, jsPrefInfo, "info", 1));
    JS_SetPropertyStr(ctx, prefs, "list", JS_NewCFunction(ctx, jsPrefList, "list", 0));
    JS_SetPropertyStr(ctx, storage, "flush", JS_NewCFunction(ctx, jsStorageFlush, "flush", 0));
    JS_SetPropertyStr(ctx, storage, "prefs", prefs);
    JS_SetPropertyStr(ctx, global, "storage", storage);

    JS_FreeValue(ctx, global);
}

#ifdef __EMSCRIPTEN__
// Callback of storage.flush()'s FS.syncfs: resolves the promise if the
// context is still alive (otherwise the resolver was already freed by
// ~JsContext).
extern "C" EMSCRIPTEN_KEEPALIVE void saida_storage_flush_done(int token, int failed) {
    auto& pending = pendingFlushes();
    auto it = pending.find(token);
    if (it == pending.end()) return;
    JSContext* ctx = it->second.ctx;
    JSValue resolve = it->second.resolve;
    pending.erase(it);
    if (!JsContext::fromRaw(ctx)) return;  // context destroyed in the meantime
    JSValue ok = JS_NewBool(ctx, failed == 0);
    JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &ok);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, ok);
    JS_FreeValue(ctx, resolve);
}
#endif

void JsEngineBindings::dropPendingFlushes(JSContext* context) {
#ifdef __EMSCRIPTEN__
    auto& pending = pendingFlushes();
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->second.ctx == context) {
            JS_FreeValue(context, it->second.resolve);
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
#else
    (void)context;
#endif
}

} // namespace saida
