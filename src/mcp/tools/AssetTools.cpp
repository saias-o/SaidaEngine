#include "mcp/tools/Tools.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "mcp/ProjectBuild.hpp"
#include "project/Project.hpp"
#include "editor/Command.hpp"
#include "editor/SceneDocument.hpp"
#include "mcp/tools/ToolRegistry.hpp"
#include "scene/Node.hpp"
#include "scene/Scene.hpp"
#include "scripting/JsContext.hpp"
#include "scripting/JsRuntime.hpp"
#include "scripting/ScriptBehaviour.hpp"

#include <set>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
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

std::string cppString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') { out += '\\'; out += char(c); }
        else if (c < 32 || c >= 127) {
            out += '\\';
            out += char('0' + (c >> 6));
            out += char('0' + ((c >> 3) & 7));
            out += char('0' + (c & 7));
        } else out += char(c);
    }
    return out + '"';
}

std::string cppFloat(const json& value) {
    const float number = value.get<float>();
    if (!std::isfinite(number)) fail("C++ float defaults must be finite and representable");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::scientific << std::setprecision(std::numeric_limits<float>::max_digits10) << number << 'f';
    return out.str();
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
    if (t == "string") return cppString(d.is_string() ? d.get<std::string>() : std::string());
    if (t == "int") return d.is_number() ? std::to_string(d.get<int>()) : "0";
    if (t == "vec3") {
        if (d.is_array() && d.size() == 3)
            return "{" + cppFloat(d[0]) + "," + cppFloat(d[1]) + "," + cppFloat(d[2]) + "}";
        return "{0.0f, 0.0f, 0.0f}";
    }
    return d.is_number() ? cppFloat(d) : "0.0f";
}

std::string cppIdentifier(const std::string& text) {
    static const std::set<std::string> reserved{
        "std", "glm", "saida", "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
        "case", "catch", "char", "char16_t", "char32_t", "class", "compl", "const", "constexpr",
        "const_cast", "continue", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
        "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
    if (text.empty() || sanitizeIdentifier(text) != text || reserved.count(text) ||
        text.front() == '_' || text.find("__") != std::string::npos)
        fail("invalid or reserved C++ identifier: " + text);
    return text;
}

json toolWriteCppBehaviour(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    if (!ctx.project || !ctx.project->isLoaded()) fail("open a project before generating C++");
    if (!args.contains("name")) fail("missing 'name'");
    std::string className = cppIdentifier(args["name"].get<std::string>());
    const json& props = args.contains("properties") ? args["properties"] : json::array();
    std::string doc = args.value("doc", std::string());
    std::string onUpdate = args.value("onUpdate", std::string());
    std::string onReady = args.value("onReady", std::string());

    std::set<std::string> names{className, "onReady", "onUpdate", "describe", "reflectName", "typeName", "save", "load"};
    // Members + describe() property lines.
    std::string members, describeBody;
    for (const auto& p : props) {
        std::string pn = cppIdentifier(p.at("name").get<std::string>());
        if (!names.insert(pn).second) fail("duplicate or reserved property: " + pn);
        std::string ty = p.value("type", std::string("float"));
        if (ty != "int" && ty != "bool" && ty != "float" && ty != "string" && ty != "vec3")
            fail("unsupported C++ property type: " + ty);
        json def = p.contains("default") ? p["default"] : json(nullptr);
        if (!def.is_null() && ((ty == "bool" && !def.is_boolean()) ||
            (ty == "string" && !def.is_string()) || (ty == "float" && !def.is_number()) ||
            (ty == "int" && (!def.is_number_integer() || def.get<double>() < -2147483648. || def.get<double>() > 2147483647.)) ||
            (ty == "vec3" && (!def.is_array() || def.size() != 3 ||
                !def[0].is_number() || !def[1].is_number() || !def[2].is_number()))))
            fail("default does not match C++ property type: " + pn);
        members += "    " + cppType(ty) + " " + pn + " = " + cppDefault(ty, def) + ";\n";
        describeBody += "    t.property(\"" + pn + "\", &" + className + "::" + pn + ")";
        if (p.contains("min") && p.contains("max"))
            describeBody += ".range(" + std::to_string(p["min"].get<double>()) + ", " +
                            std::to_string(p["max"].get<double>()) + ")";
        if (p.contains("tooltip"))
            describeBody += ".tooltip(" + cppString(p["tooltip"].get<std::string>()) + ")";
        describeBody += ";\n";
    }

    std::string header =
        "#pragma once\n\n"
        "#include \"core/Reflection.hpp\"\n"
        "#include \"scene/Behaviour.hpp\"\n\n"
        "#include <glm/glm.hpp>\n"
        "#include <string>\n\n"
        "namespace game {\n\n"
        "class " + className + " : public saida::Behaviour {\n"
        "public:\n";
    if (!onReady.empty()) header += "    void onReady() override;\n";
    header +=
        "    void onUpdate(float dt) override;\n\n"
        "    SAIDA_REFLECT_BEHAVIOUR(" + className + ", \"" + className + "\")\n\n" +
        members +
        "};\n\n"
        "} // namespace game\n";

    std::string body =
        "#include \"" + className + ".hpp\"\n\n"
        "#include \"core/Reflection.hpp\"\n"
        "#include \"scene/Node.hpp\"\n\n"
        "namespace game {\n\n"
        "void " + className + "::describe(saida::reflect::TypeBuilder<" + className + ">& t) {\n";
    if (!doc.empty()) body += "    t.doc(" + cppString(doc) + ");\n";
    body += describeBody;
    body += "}\n\n";
    if (!onReady.empty())
        body += "void " + className + "::onReady() {\n" + onReady + "\n}\n\n";
    body +=
        "void " + className + "::onUpdate(float dt) {\n"
        "    (void)dt;\n" +
        onUpdate + "\n"
        "}\n\n"
        "} // namespace game\n";

    const auto hpp = resolveToolPath(ctx, "src/generated/" + className + ".hpp");
    const auto cpp = resolveToolPath(ctx, "src/generated/" + className + ".cpp");
    if (fs::exists(hpp.absolute) || fs::exists(cpp.absolute)) fail("behaviour files already exist; edit them explicitly");
    const std::string dir = fs::path(hpp.absolute).parent_path().string() + "/";
    writeFile(dir + className + ".hpp", header);
    writeFile(dir + className + ".cpp", body);

    return {{"type", className},
            {"files", json::array({dir + className + ".hpp", dir + className + ".cpp"})},
            {"note", "Build the project's saida_add_game target for its native player. WITH_EDITOR provides a project editor: close it, build <target>_editor externally and restart to discover new types. The stock editor and Web player do not load project C++."}};
}

json toolBuild(const ToolContext& ctx, const json& args) {
    requireEdit(ctx);
    if (!ctx.project || !ctx.project->isLoaded()) fail("open a project before building");
    const auto directory = resolveToolPath(ctx, args.value("directory", std::string("build")));
    if (!fs::is_regular_file(fs::path(directory.absolute) / "CMakeCache.txt"))
        fail("configure the project's native CMake build first: " + directory.absolute);
    const auto cache = readFile((fs::path(directory.absolute) / "CMakeCache.txt").string());
    const std::string key = "\nCMAKE_HOME_DIRECTORY:INTERNAL=";
    const auto home = cache.find(key);
    if (home == std::string::npos) fail("CMake cache has no source directory");
    const auto start = home + key.size();
    const auto end = cache.find_first_of("\r\n", start);
    std::error_code pathError;
    if (!fs::equivalent(fs::path(cache.substr(start, end - start)), fs::path(projectRoot(ctx)), pathError))
        fail("build directory belongs to another source project; configure this project's CMakeLists.txt");
    const auto result = buildNativeProject(directory.absolute);
    return {{"ok", result.exitCode == 0}, {"exitCode", result.exitCode}, {"output", result.output},
            {"directory", directory.relative}};
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
                 "Scaffold a reflected C++ behaviour under the open project src/generated (native player only). {name, doc?, properties:[{name,type,default,min,max,tooltip}], onReady?, onUpdate?}. Then call 'build'.",
                 objectSchema({{"name", stringSchema()}, {"doc", stringSchema()},
                               {"properties", json{{"type", "array"}}},
                               {"onReady", stringSchema()}, {"onUpdate", stringSchema()}},
                              {"name"}), toolWriteCppBehaviour);
    registry.add("build",
                 "Build the open project's configured CMake directory (default build). Never edits engine source. Restart the native player to load compiled types.",
                 objectSchema({{"directory", stringSchema()}}), toolBuild);
    registry.add("run_headless_check",
                 "Validate the current scene: compile every attached script (no GPU). Returns ok + per-script errors.",
                 objectSchema({}), toolHeadlessCheck);
    registry.add("read_logs", "Recent engine log lines (newest last).",
                 objectSchema({{"count", json{{"type", "number"}}}}), toolReadLogs);
}

} // namespace saida::mcp
