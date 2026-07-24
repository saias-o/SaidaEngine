#include "mcp/tools/Tools.hpp"

#include "graphics/ResourceManager.hpp"
#include "mcp/tools/ToolRegistry.hpp"
#include "project/Project.hpp"
#include "scene/BVHLoader.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/Scene.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/AnimationSequence.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipView.hpp"
#include "scene/animation/RetargetProfile.hpp"
#include "scene/animation/RigAsset.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace saida::mcp {
namespace {
namespace fs = std::filesystem;

// ── Authoring animation ─────────────────────────────────────────────────────
// These tools operate on versioned assets (.sclip/.sgraph/.sretarget/
// .srig/.sseq): systematic validation before writing, {dryRun:true} to
// get diagnostics without touching disk.

json animationDiagsJson(const std::vector<AssetDiagnostic>& diags) {
    json out = json::array();
    for (const auto& d : diags) out.push_back(d.toJson());
    return out;
}

bool animationExtensionIs(const std::string& path, const char* extension) {
    const std::string ext = fs::path(path).extension().string();
    return ext == extension;
}

// Writes a validated document; refuses to overwrite without {overwrite:true}.
json writeAnimationAsset(const std::string& abs, const json& doc,
                         const std::vector<AssetDiagnostic>& diags, const json& args) {
    if (hasErrors(diags))
        return {{"ok", false}, {"path", abs}, {"diagnostics", animationDiagsJson(diags)}};
    if (args.value("dryRun", false))
        return {{"ok", true}, {"dryRun", true}, {"path", abs}, {"document", doc},
                {"diagnostics", animationDiagsJson(diags)}};
    std::error_code ec;
    if (fs::exists(abs, ec) && !args.value("overwrite", false))
        fail("'" + abs + "' exists (pass overwrite:true to replace it)");
    fs::create_directories(fs::path(abs).parent_path(), ec);
    writeFile(abs, doc.dump(1) + "\n");
    return {{"ok", true}, {"path", abs}, {"diagnostics", animationDiagsJson(diags)}};
}

// Parses + internally validates an animation asset document, by extension.
json validateAnimationDocument(const std::string& abs, std::vector<AssetDiagnostic>& diags) {
    if (animationExtensionIs(abs, ".sclip")) {
        auto parsed = ClipView::loadFile(abs);
        diags = std::move(parsed.diagnostics);
        if (parsed.ok) {
            auto more = parsed.view.validate(nullptr);
            diags.insert(diags.end(), more.begin(), more.end());
        }
        return parsed.ok ? parsed.view.toJson() : json();
    }
    if (animationExtensionIs(abs, ".sgraph")) {
        auto parsed = AnimGraphAsset::loadFile(abs);
        diags = std::move(parsed.diagnostics);
        if (parsed.ok) {
            auto more = parsed.graph.validate();
            diags.insert(diags.end(), more.begin(), more.end());
        }
        return parsed.ok ? parsed.graph.toJson() : json();
    }
    if (animationExtensionIs(abs, ".sretarget")) {
        auto parsed = RetargetProfile::loadFile(abs);
        diags = std::move(parsed.diagnostics);
        if (parsed.ok) {
            auto more = parsed.profile.validate(nullptr, nullptr);
            diags.insert(diags.end(), more.begin(), more.end());
        }
        return parsed.ok ? parsed.profile.toJson() : json();
    }
    if (animationExtensionIs(abs, ".srig")) {
        auto parsed = RigAsset::loadFile(abs);
        diags = std::move(parsed.diagnostics);
        if (parsed.ok) {
            auto more = parsed.asset.validate(nullptr);
            diags.insert(diags.end(), more.begin(), more.end());
        }
        return parsed.ok ? parsed.asset.toJson() : json();
    }
    if (animationExtensionIs(abs, ".sseq")) {
        auto parsed = AnimationSequence::loadFile(abs);
        diags = std::move(parsed.diagnostics);
        if (parsed.ok) {
            auto more = parsed.sequence.validate();
            diags.insert(diags.end(), more.begin(), more.end());
        }
        return parsed.ok ? parsed.sequence.toJson() : json();
    }
    fail("unsupported animation asset '" + abs + "'");
}

json toolListAnimationAssets(const ToolContext& ctx, const json&) {
    if (!ctx.project || !ctx.project->isLoaded()) fail("no project loaded");
    const fs::path root(ctx.project->rootPath());

    json assets = json::array();
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            const std::string dir = it->path().filename().string();
            if (dir == "build" || dir == ".git" || dir == ".saida")
                it.disable_recursion_pending();
            continue;
        }
        const std::string ext = it->path().extension().string();
        const char* type = ext == ".sclip"     ? "clipView"
                           : ext == ".sgraph"  ? "graph"
                           : ext == ".sretarget" ? "retargetProfile"
                           : ext == ".srig"    ? "rig"
                           : ext == ".sseq"    ? "sequence"
                           : (ext == ".gltf" || ext == ".glb" || ext == ".bvh") ? "source"
                                                                                : nullptr;
        if (!type) continue;
        assets.push_back({{"path", it->path().lexically_relative(root).generic_string()},
                          {"type", type}});
    }
    return {{"assets", assets}};
}

json toolInspectAnimationAsset(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;
    const std::string ext = fs::path(abs).extension().string();

    if (ext == ".gltf" || ext == ".glb" || ext == ".bvh") {
        GltfAnimationData data;
        std::string error;
        if (ext == ".bvh") {
            auto clip = BVHLoader::parse(abs);
            if (!clip) fail("failed to parse BVH '" + abs + "'");
            data.clipNames.push_back(clip->name());
            data.clips.push_back(std::move(clip));
        } else if (!GLTFLoader::loadAnimationData(abs, data, &error)) {
            fail(error);
        }
        json rigs = json::array();
        for (const auto& rig : data.rigs) {
            json bones = json::array();
            for (const auto& bone : rig->bones()) bones.push_back(bone.name);
            rigs.push_back({{"boneCount", rig->boneCount()}, {"bones", bones}});
        }
        json clips = json::array();
        for (size_t i = 0; i < data.clips.size(); ++i) {
            clips.push_back({{"name", data.clipNames[i]},
                             {"key", args["path"].get<std::string>() + "#" + data.clipNames[i]},
                             {"duration", data.clips[i]->duration()},
                             {"animatedBones", data.clips[i]->boneNames().size()}});
        }
        return {{"path", abs}, {"rigs", rigs}, {"clips", clips}};
    }

    std::vector<AssetDiagnostic> diags;
    const json doc = validateAnimationDocument(abs, diags);
    return {{"ok", !hasErrors(diags)}, {"path", abs}, {"document", doc},
            {"diagnostics", animationDiagsJson(diags)}};
}

json toolValidateAnimationAsset(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;
    std::vector<AssetDiagnostic> diags;
    validateAnimationDocument(abs, diags);
    return {{"ok", !hasErrors(diags)}, {"path", abs},
            {"diagnostics", animationDiagsJson(diags)}};
}

json toolCreateClipView(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    if (!args.contains("view") || !args["view"].is_object()) fail("missing object 'view'");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;

    json doc = args["view"];
    if (!doc.contains("schema")) doc["schema"] = kClipViewSchema;
    auto parsed = ClipView::parse(doc);
    std::vector<AssetDiagnostic> diags = std::move(parsed.diagnostics);
    if (parsed.ok) {
        auto more = parsed.view.validate(nullptr);
        diags.insert(diags.end(), more.begin(), more.end());
    }
    return writeAnimationAsset(abs, parsed.ok ? parsed.view.toJson() : doc, diags, args);
}

json toolCreateAnimationGraph(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    if (!args.contains("graph") || !args["graph"].is_object()) fail("missing object 'graph'");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;

    json doc = args["graph"];
    if (!doc.contains("schema")) doc["schema"] = kAnimGraphSchema;
    auto parsed = AnimGraphAsset::parse(doc);
    std::vector<AssetDiagnostic> diags = std::move(parsed.diagnostics);
    if (parsed.ok) {
        auto more = parsed.graph.validate();
        diags.insert(diags.end(), more.begin(), more.end());
    }
    return writeAnimationAsset(abs, parsed.ok ? parsed.graph.toJson() : doc, diags, args);
}

// Locomotion recipe: idle/walk(/run) driven by a speed parameter,
// with crossfaded up/down transitions and synced phase.
json toolCreateLocomotionGraph(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    if (!args.contains("idle") || !args.contains("walk"))
        fail("'idle' and 'walk' clip keys are required");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;

    const std::string parameter = args.value("parameter", "speed");
    const float walkThreshold = args.value("walkThreshold", 0.1f);
    const float runThreshold = args.value("runThreshold", 3.0f);
    const float crossfade = args.value("crossfade", 0.2f);
    const bool hasRun = args.contains("run");

    json clips = {{"idle", args["idle"]}, {"walk", args["walk"]}};
    json states = json::array({{{"name", "Idle"}, {"play", "idle"}},
                               {{"name", "Walk"}, {"play", "walk"}}});
    const auto condition = [&](const char* op, float value) {
        return json::array({{{"param", parameter}, {"op", op}, {"value", value}}});
    };
    json transitions = json::array(
        {{{"from", "Idle"}, {"to", "Walk"}, {"crossfade", crossfade},
          {"when", condition(">", walkThreshold)}},
         {{"from", "Walk"}, {"to", "Idle"}, {"crossfade", crossfade},
          {"when", condition("<=", walkThreshold)}}});
    if (hasRun) {
        clips["run"] = args["run"];
        states.push_back({{"name", "Run"}, {"play", "run"}});
        transitions.push_back({{"from", "Walk"}, {"to", "Run"}, {"crossfade", crossfade},
                               {"syncPhase", true}, {"when", condition(">", runThreshold)}});
        transitions.push_back({{"from", "Run"}, {"to", "Walk"}, {"crossfade", crossfade},
                               {"syncPhase", true}, {"when", condition("<=", runThreshold)}});
    }

    const json doc = {
        {"schema", kAnimGraphSchema},
        {"name", args.value("name", fs::path(abs).stem().string())},
        {"parameters",
         json::array({{{"name", parameter}, {"type", "float"}, {"default", 0.0f}}})},
        {"clips", clips},
        {"states", states},
        {"initial", "Idle"},
        {"transitions", transitions}};

    auto parsed = AnimGraphAsset::parse(doc);
    std::vector<AssetDiagnostic> diags = std::move(parsed.diagnostics);
    if (parsed.ok) {
        auto more = parsed.graph.validate();
        diags.insert(diags.end(), more.begin(), more.end());
    }
    return writeAnimationAsset(abs, doc, diags, args);
}

json toolCreateAnimationSequence(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    if (!args.contains("sequence") || !args["sequence"].is_object())
        fail("missing object 'sequence'");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;

    json doc = args["sequence"];
    if (!doc.contains("schema")) doc["schema"] = kAnimationSequenceSchema;
    auto parsed = AnimationSequence::parse(doc);
    std::vector<AssetDiagnostic> diags = std::move(parsed.diagnostics);
    if (parsed.ok) {
        auto more = parsed.sequence.validate();
        diags.insert(diags.end(), more.begin(), more.end());
    }
    return writeAnimationAsset(abs, parsed.ok ? parsed.sequence.toJson() : doc, diags, args);
}

// Patches an existing .sgraph: rereads, modifies, revalidates, rewrites.
json patchAnimationGraph(const ToolContext& ctx, const json& args,
                         const std::function<void(json&)>& mutate) {
    if (!args.contains("path")) fail("missing 'path'");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;
    const std::string text = readFile(abs);
    if (text.empty()) fail("could not read graph: " + abs);
    json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) fail(abs + " is not valid JSON");

    mutate(doc);

    auto parsed = AnimGraphAsset::parse(doc);
    std::vector<AssetDiagnostic> diags = std::move(parsed.diagnostics);
    if (parsed.ok) {
        auto more = parsed.graph.validate();
        diags.insert(diags.end(), more.begin(), more.end());
    }
    json write = args;
    write["overwrite"] = true;
    return writeAnimationAsset(abs, parsed.ok ? parsed.graph.toJson() : doc, diags, write);
}

json toolSetGraphParameter(const ToolContext& ctx, const json& args) {
    if (!args.contains("name")) fail("missing 'name'");
    return patchAnimationGraph(ctx, args, [&](json& doc) {
        json entry = {{"name", args["name"]},
                      {"type", args.value("type", "float")}};
        if (args.contains("default")) entry["default"] = args["default"];
        json& parameters = doc["parameters"];
        if (!parameters.is_array()) parameters = json::array();
        for (auto& p : parameters) {
            if (p.is_object() && p.value("name", std::string()) == args["name"]) {
                p = entry;
                return;
            }
        }
        parameters.push_back(std::move(entry));
    });
}

json toolAddAnimationState(const ToolContext& ctx, const json& args) {
    if (!args.contains("state") || !args["state"].is_object()) fail("missing object 'state'");
    return patchAnimationGraph(ctx, args, [&](json& doc) {
        // {clipKey} declares the alias played by the state if it doesn't exist yet.
        const std::string alias = args["state"].value("play", std::string());
        if (args.contains("clipKey") && !alias.empty()) {
            if (!doc["clips"].is_object()) doc["clips"] = json::object();
            doc["clips"][alias] = args["clipKey"];
        }
        const std::string name = args["state"].value("name", std::string());
        json& states = doc["states"];
        if (!states.is_array()) states = json::array();
        for (auto& s : states) {
            if (s.is_object() && s.value("name", std::string()) == name) {
                s = args["state"];
                return;
            }
        }
        states.push_back(args["state"]);
    });
}

json toolAddAnimationTransition(const ToolContext& ctx, const json& args) {
    if (!args.contains("transition") || !args["transition"].is_object())
        fail("missing object 'transition'");
    return patchAnimationGraph(ctx, args, [&](json& doc) {
        json& transitions = doc["transitions"];
        if (!transitions.is_array()) transitions = json::array();
        transitions.push_back(args["transition"]);
    });
}

Animator* findPreviewAnimator(const ToolContext& ctx, const json& args) {
    if (args.contains("id")) {
        Node* node = requireNode(ctx, args);
        while (node) {
            if (auto* animator = node->getBehaviour<Animator>()) return animator;
            node = node->parent();
        }
        fail("no Animator on this node or its ancestors");
    }
    // Without an explicit target: the first Animator in the scene.
    std::function<Animator*(Node&)> search = [&](Node& node) -> Animator* {
        if (auto* animator = node.getBehaviour<Animator>()) return animator;
        for (const auto& child : node.children())
            if (Animator* found = search(*child)) return found;
        return nullptr;
    };
    Animator* animator = ctx.scene ? search(*ctx.scene) : nullptr;
    if (!animator) fail("no Animator in the scene");
    return animator;
}

json toolPreviewAnimationAsset(const ToolContext& ctx, const json& args) {
    if (!args.contains("path")) fail("missing 'path'");
    const std::string abs = resolveToolPath(ctx, args["path"].get<std::string>()).absolute;
    Animator* animator = findPreviewAnimator(ctx, args);
    if (!animator->rig()) fail("the Animator has no rig");

    if (animationExtensionIs(abs, ".sclip")) {
        auto parsed = ClipView::loadFile(abs);
        if (!parsed.ok)
            return {{"ok", false}, {"diagnostics", animationDiagsJson(parsed.diagnostics)}};
        animator->playView(parsed.view, 0.0f);
        return {{"ok", true}, {"playing", parsed.view.name}};
    }
    if (animationExtensionIs(abs, ".sgraph")) {
        auto parsed = AnimGraphAsset::loadFile(abs);
        if (!parsed.ok)
            return {{"ok", false}, {"diagnostics", animationDiagsJson(parsed.diagnostics)}};
        std::vector<AssetDiagnostic> diags;
        const bool ok = animator->setGraph(parsed.graph, &diags);
        return {{"ok", ok}, {"diagnostics", animationDiagsJson(diags)}};
    }
    fail("preview supports .sclip and .sgraph assets");
}


} // namespace

void registerAnimationTools(ToolRegistry& registry) {
    registry.add("list_animation_assets",
                 "Animation assets of the project: sources (.gltf/.glb/.bvh) and authored .sclip/.sgraph/.sretarget/.srig/.sseq.",
                 objectSchema({}), toolListAnimationAssets);
    registry.add("inspect_animation_asset",
                 "Inspect an animation asset: sources return rigs+clips (with sub-asset keys), authored assets return their document + diagnostics.",
                 objectSchema({{"path", stringSchema()}}, {"path"}),
                 toolInspectAnimationAsset);
    registry.add("validate_animation_asset",
                 "Validate an authored animation asset (.sclip/.sgraph/.sretarget/.srig/.sseq). Returns {ok, diagnostics}.",
                 objectSchema({{"path", stringSchema()}}, {"path"}),
                 toolValidateAnimationAsset);
    registry.add("create_clip_view",
                 "Create a .sclip non-destructive view of a source clip. {path, view:{source,name,range?,loop?,speed?,events?}, overwrite?, dryRun?}.",
                 objectSchema({{"path", stringSchema()},
                               {"view", json{{"type", "object"}}}}, {"path", "view"}),
                 toolCreateClipView);
    registry.add("create_animation_graph",
                 "Create a .sgraph playback graph from a full document. Validates before writing; {dryRun:true} returns diagnostics only.",
                 objectSchema({{"path", stringSchema()},
                               {"graph", json{{"type", "object"}}}}, {"path", "graph"}),
                 toolCreateAnimationGraph);
    registry.add("create_locomotion_graph",
                 "Recipe: idle/walk(/run) locomotion .sgraph driven by a speed parameter, with crossfades and phase sync. {path, idle, walk, run?, parameter?, walkThreshold?, runThreshold?, crossfade?}.",
                 objectSchema({{"path", stringSchema()}, {"idle", stringSchema()},
                               {"walk", stringSchema()}, {"run", stringSchema()},
                               {"parameter", stringSchema()}},
                              {"path", "idle", "walk"}), toolCreateLocomotionGraph);
    registry.add("create_animation_sequence",
                 "Create a .sseq multi-track sequence (animation/event/property tracks). Validates before writing.",
                 objectSchema({{"path", stringSchema()},
                               {"sequence", json{{"type", "object"}}}},
                              {"path", "sequence"}), toolCreateAnimationSequence);
    registry.add("set_graph_parameter",
                 "Add or update a typed parameter of a .sgraph. {path, name, type?, default?}.",
                 objectSchema({{"path", stringSchema()}, {"name", stringSchema()},
                               {"type", stringSchema()}}, {"path", "name"}),
                 toolSetGraphParameter);
    registry.add("add_animation_state",
                 "Add or replace a state of a .sgraph. {path, state:{name,play,loop?,speed?}, clipKey?} — clipKey declares the played alias if new.",
                 objectSchema({{"path", stringSchema()},
                               {"state", json{{"type", "object"}}},
                               {"clipKey", stringSchema()}}, {"path", "state"}),
                 toolAddAnimationState);
    registry.add("add_animation_transition",
                 "Append a transition to a .sgraph. {path, transition:{from,to,crossfade?,exitTime?,syncPhase?,when?}}.",
                 objectSchema({{"path", stringSchema()},
                               {"transition", json{{"type", "object"}}}},
                              {"path", "transition"}), toolAddAnimationTransition);
    registry.add("preview_animation_asset",
                 "Play a .sclip or apply a .sgraph on an Animator ({id} or the first one in the scene).",
                 objectSchema({{"path", stringSchema()}, {"id", stringSchema()}},
                              {"path"}), toolPreviewAnimationAsset);
}

} // namespace saida::mcp

