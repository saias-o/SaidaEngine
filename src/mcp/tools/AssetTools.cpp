#include "mcp/tools/Tools.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "editor/Command.hpp"
#include "editor/SceneDocument.hpp"
#include "mcp/tools/ToolRegistry.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsRuntime.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace saida::mcp {
namespace {
namespace fs = std::filesystem;

json toolWriteScript(const ToolContext& ctx, const json& args) {
    if (!args.contains("path") || !args.contains("code")) fail("missing 'path'/'code'");
    SandboxedPathResult scriptPath =
        resolveToolPath(ctx, args["path"].get<std::string>(), "scripts");
    writeFile(scriptPath.absolute, args["code"].get<std::string>());

    json out{{"path", scriptPath.absolute}, {"relativePath", scriptPath.relative}};
    if (args.contains("attachTo")) {
        requireEdit(ctx);
        Node* node = ctx.doc->find(parseNodeId(args["attachTo"]));
        if (!node) fail("attachTo node not found");
        ctx.exec(std::make_unique<AttachScriptCommand>(node->id(), scriptPath.relative));
        out["attached"] = true;
    }
    return out;
}

json toolWriteUi(const ToolContext& ctx, const json& args) {
    if (!args.contains("path") || !args.contains("code")) fail("missing 'path'/'code'");
    SandboxedPathResult uiPath = resolveToolPath(ctx, args["path"].get<std::string>());
    writeFile(uiPath.absolute, args["code"].get<std::string>());
    return {{"path", uiPath.absolute}, {"relativePath", uiPath.relative}};
}

std::string cppType(const std::string& t) {
    if (t == "int") return "int";
    if (t == "bool") return "bool";
    if (t == "string") return "std::string";
    if (t == "vec3") return "glm::vec3";
    return "float";
}

std::string cppDefault(const std::string& t, const json& d) {
    if (t == "bool") return (d.is_boolean() && d.get<bool>()) ? "true" : "false";
    if (t == "string") return "\"" + (d.is_string() ? d.get<std::string>() : std::string()) + "\"";
    if (t == "int") return d.is_number() ? std::to_string(d.get<int>()) : "0";
    if (t == "vec3") {
        if (d.is_array() && d.size() == 3)
            return "{" + std::to_string(d[0].get<float>()) + "f," + std::to_string(d[1].get<float>()) +
                   "f," + std::to_string(d[2].get<float>()) + "f}";
        return "{0.0f, 0.0f, 0.0f}";
    }
    return (d.is_number() ? std::to_string(d.get<float>()) : "0.0") + "f";
}

void patchReflectedTypes(const std::string& className) {
    std::string path = std::string(SAIDA_PROJECT_ROOT) + "/src/scene/ReflectedTypes.cpp";
    std::string src = readFile(path);
    if (src.empty()) fail("could not read ReflectedTypes.cpp");

    std::string includeLine = "#include \"generated/" + className + ".hpp\"";
    std::string registerLine = "    registerBehaviour<" + className + ">();";
    if (src.find(includeLine) != std::string::npos) return;  // already registered

    const std::string incMarker = "// <<SAIDA_MCP_INCLUDES>>";
    const std::string regMarker = "// <<SAIDA_MCP_REGISTER>>";
    auto incPos = src.find(incMarker);
    auto regPos = src.find(regMarker);
    if (incPos == std::string::npos || regPos == std::string::npos)
        fail("ReflectedTypes.cpp markers missing");
    src.insert(incPos, includeLine + "\n");
    regPos = src.find(regMarker);  // shifted by the insert above
    src.insert(regPos, registerLine + "\n    ");
    writeFile(path, src);
}

json toolWriteCppBehaviour(const ToolContext&, const json& args) {
    if (!args.contains("name")) fail("missing 'name'");
    std::string className = sanitizeIdentifier(args["name"].get<std::string>());
    const json& props = args.contains("properties") ? args["properties"] : json::array();
    std::string doc = args.value("doc", std::string());
    std::string onUpdate = args.value("onUpdate", std::string());
    std::string onReady = args.value("onReady", std::string());

    // Members + describe() property lines.
    std::string members, describeBody;
    for (const auto& p : props) {
        std::string pn = sanitizeIdentifier(p.at("name").get<std::string>());
        std::string ty = p.value("type", std::string("float"));
        json def = p.contains("default") ? p["default"] : json(nullptr);
        members += "    " + cppType(ty) + " " + pn + " = " + cppDefault(ty, def) + ";\n";
        describeBody += "    t.property(\"" + pn + "\", &" + className + "::" + pn + ")";
        if (p.contains("min") && p.contains("max"))
            describeBody += ".range(" + std::to_string(p["min"].get<double>()) + ", " +
                            std::to_string(p["max"].get<double>()) + ")";
        if (p.contains("tooltip"))
            describeBody += ".tooltip(\"" + p["tooltip"].get<std::string>() + "\")";
        describeBody += ";\n";
    }

    std::string header =
        "#pragma once\n\n"
        "#include \"core/Reflection.hpp\"\n"
        "#include \"scene/Behaviour.hpp\"\n\n"
        "#include <glm/glm.hpp>\n"
        "#include <string>\n\n"
        "namespace saida {\n\n"
        "class " + className + " : public Behaviour {\n"
        "public:\n";
    if (!onReady.empty()) header += "    void onReady() override;\n";
    header +=
        "    void onUpdate(float dt) override;\n\n"
        "    SAIDA_REFLECT_BEHAVIOUR(" + className + ", \"" + className + "\")\n\n" +
        members +
        "};\n\n"
        "} // namespace saida\n";

    std::string body =
        "#include \"generated/" + className + ".hpp\"\n\n"
        "#include \"core/Reflection.hpp\"\n"
        "#include \"scene/Node.hpp\"\n\n"
        "namespace saida {\n\n"
        "void " + className + "::describe(reflect::TypeBuilder<" + className + ">& t) {\n";
    if (!doc.empty()) body += "    t.doc(\"" + doc + "\");\n";
    body += describeBody;
    body += "}\n\n";
    if (!onReady.empty())
        body += "void " + className + "::onReady() {\n" + onReady + "\n}\n\n";
    body +=
        "void " + className + "::onUpdate(float dt) {\n"
        "    (void)dt;\n" +
        onUpdate + "\n"
        "}\n\n"
        "} // namespace saida\n";

    std::string dir = std::string(SAIDA_PROJECT_ROOT) + "/src/generated/";
    writeFile(dir + className + ".hpp", header);
    writeFile(dir + className + ".cpp", body);
    patchReflectedTypes(className);

    return {{"type", className},
            {"files", json::array({dir + className + ".hpp", dir + className + ".cpp"})},
            {"note", "Run the 'build' tool to compile; the type then appears in describe_api."}};
}

json toolBuild(const ToolContext&, const json&) {
    std::string cmd = "cmake --build \"" + std::string(SAIDA_PROJECT_ROOT) + "/build\" -j 2 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) fail("could not launch build (is cmake on PATH?)");
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    int rc = pclose(pipe);
    std::string tail = out.size() > 6000 ? out.substr(out.size() - 6000) : out;
    return {{"ok", rc == 0}, {"exitCode", rc}, {"output", tail}};
}

json toolHeadlessCheck(const ToolContext& ctx, const json&) {
    json errors = json::array();
    int checked = 0;
    if (ctx.scene) {
        JsContext jsctx(JsRuntime::instance());
        std::string root = projectRoot(ctx);
        ctx.scene->traverse([&](Node& n, const glm::mat4&) {
            for (const auto& b : n.behaviours()) {
                auto* sb = dynamic_cast<ScriptBehaviour*>(b.get());
                if (!sb || sb->scriptPath().empty()) continue;
                fs::path pp(sb->scriptPath());
                std::string abs = pp.is_absolute() ? sb->scriptPath() : root + "/" + sb->scriptPath();
                std::string code = readFile(abs);
                ++checked;
                if (code.empty()) {
                    errors.push_back({{"script", sb->scriptPath()}, {"error", "file not found or empty"}});
                    continue;
                }
                std::string err;
                if (!jsctx.compileCheck(code, abs, pp.extension() == ".mjs", err))
                    errors.push_back({{"script", sb->scriptPath()}, {"error", err}});
            }
        });
    }
    return {{"ok", errors.empty()}, {"scriptsChecked", checked}, {"errors", errors}};
}

json toolReadLogs(const ToolContext&, const json& args) {
    size_t count = args.value("count", static_cast<size_t>(100));
    return Log::recent(count);
}


} // namespace

void registerAssetTools(ToolRegistry& registry) {
    registry.add("write_script",
                 "Write a JS/.mjs gameplay script (hot-reloaded). Path is project-relative (defaults under scripts/). Optionally {attachTo:<nodeId>} to add a ScriptBehaviour.",
                 objectSchema({{"path", stringSchema()}, {"code", stringSchema()},
                               {"attachTo", stringSchema()}}, {"path", "code"}),
                 toolWriteScript);
    registry.add("write_ui",
                 "Write an HTML/RML/CSS file for a WebCanvasNode (hot-reloaded). Path is project-relative.",
                 objectSchema({{"path", stringSchema()}, {"code", stringSchema()}},
                              {"path", "code"}), toolWriteUi);
    registry.add("write_cpp_behaviour",
                 "Scaffold a reflected C++ behaviour (perf path). {name, doc?, properties:[{name,type,default,min,max,tooltip}], onReady?, onUpdate?}. Then call 'build'.",
                 objectSchema({{"name", stringSchema()}, {"doc", stringSchema()},
                               {"properties", json{{"type", "array"}}},
                               {"onReady", stringSchema()}, {"onUpdate", stringSchema()}},
                              {"name"}), toolWriteCppBehaviour);
    registry.add("build",
                 "Compile the engine (picks up new C++ behaviours). Returns ok + compiler output.",
                 objectSchema({}), toolBuild);
    registry.add("run_headless_check",
                 "Validate the current scene: compile every attached script (no GPU). Returns ok + per-script errors.",
                 objectSchema({}), toolHeadlessCheck);
    registry.add("read_logs", "Recent engine log lines (newest last).",
                 objectSchema({{"count", json{{"type", "number"}}}}), toolReadLogs);
}

} // namespace saida::mcp
