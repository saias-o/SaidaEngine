#include "editor/BuildExporter.hpp"

#include "core/Log.hpp"
#include "core/EngineVersion.hpp"
#include "core/FormatVersions.hpp"
#include "core/Paths.hpp"
#include "editor/ExeMetadata.hpp"
#include "project/Project.hpp"
#include "ui/RmlUiRuntime.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace saida {

namespace {
namespace fs = std::filesystem;

// Recursively copy a directory if it exists. Returns false on hard error.
bool copyTree(const fs::path& src, const fs::path& dst, std::string& log) {
    std::error_code ec;
    if (!fs::exists(src)) return true;  // nothing to copy is not an error
    fs::create_directories(dst, ec);
    if (ec) {
        log += "  ! failed to create " + dst.string() + " (" + ec.message() + ")\n";
        return false;
    }

    fs::recursive_directory_iterator it(src, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry& entry = *it;
        const fs::path relative = fs::relative(entry.path(), src, ec);
        if (ec) break;
        const fs::path target = dst / relative;

        const fs::file_status status = entry.symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status)) {
            log += "  ! refusing symlink in package: " + entry.path().string() + "\n";
            return false;
        }
        if (fs::is_directory(status)) {
            fs::create_directories(target, ec);
        } else if (fs::is_regular_file(status)) {
            fs::create_directories(target.parent_path(), ec);
            if (!ec) {
                fs::copy_file(entry.path(), target,
                              fs::copy_options::overwrite_existing, ec);
            }
        } else {
            log += "  ! refusing special file in package: " + entry.path().string() + "\n";
            return false;
        }
        if (!ec) it.increment(ec);
    }
    if (ec) {
        log += "  ! failed to copy " + src.string() + " -> " + dst.string() +
               " (" + ec.message() + ")\n";
        return false;
    }
    log += "  copied " + src.filename().string() + "/\n";
    return true;
}

// Is `child` inside `parent` (or equal)? Used to reject dangerous output dirs.
bool isInside(const fs::path& child, const fs::path& parent) {
    const fs::path c = child.lexically_normal();
    const fs::path p = parent.lexically_normal();
    auto it = c.begin();
    for (auto pit = p.begin(); pit != p.end(); ++pit, ++it) {
        if (it == c.end() || *it != *pit) return false;
    }
    return true;
}

bool safePackagePath(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    const fs::path normalized = path.lexically_normal();
    return !normalized.empty() && normalized.begin()->string() != "..";
}

std::string safeFileStem(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-' && c != '_' && c != ' ') c = '_';
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    return value.empty() ? "Game" : value;
}

// A package must bundle the engine's default fonts: without them, RmlUi has
// no face and all UI text silently disappears outside a dev checkout. The
// files go under assets/fonts/ (the runtime's first resolution root);
// copied before the project data, so a same-named project file wins.
bool copyEngineFonts(const fs::path& outRoot, std::string& log, std::string& error) {
    std::error_code ec;
    fs::create_directories(outRoot / "assets" / "fonts", ec);
    if (ec) {
        error = "cannot create assets/fonts: " + ec.message();
        return false;
    }
    for (const EngineFontFile& font : RmlUiRuntime::engineFontFiles()) {
        if (font.sourcePath.empty()) {
            if (!font.required) continue;
            error = "engine font not found: " + font.fileName;
            return false;
        }
        fs::copy_file(font.sourcePath, outRoot / "assets" / "fonts" / font.fileName,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "failed to copy engine font " + font.fileName + ": " + ec.message();
            return false;
        }
        log += "  assets/fonts/" + font.fileName + "\n";
    }
    return true;
}

bool copyProjectData(const Project& project, const fs::path& outRoot, std::string& log) {
    const fs::path root(project.rootPath());
    for (const char* dir : {"assets", "scenes", "scripts", "ui", "anim", "shaders"}) {
        if (!copyTree(root / dir, outRoot / dir, log)) return false;
    }

    std::error_code ec;
    const fs::path projectFile(project.filePath());
    fs::copy_file(projectFile, outRoot / projectFile.filename(),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    log += "  " + projectFile.filename().string() + "\n";

    const fs::path registry = root / "asset_registry.json";
    if (fs::exists(registry)) {
        fs::copy_file(registry, outRoot / registry.filename(),
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        log += "  asset_registry.json\n";
    }
    return true;
}

bool writeBootManifest(const Project& project, const fs::path& outRoot,
                       const std::string& mainScene) {
    std::ofstream manifest(outRoot / "game.saida", std::ios::trunc);
    if (!manifest) return false;
    manifest << "# SaidaEngine game boot manifest\n";
    manifest << "schema=" << format::kBootManifestVersion << "\n";
    manifest << "project=" << fs::path(project.filePath()).filename().string() << "\n";
    manifest << "main_scene=" << mainScene << "\n";
    return true;
}

std::vector<std::string> packageFileList(const fs::path& root) {
    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file())
            files.push_back(fs::relative(entry.path(), root).generic_string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

BuildExporter::Result BuildExporter::exportWindowsBuild(const Project& project,
                                                        const Options& options) {
    Result r;
    auto fail = [&](const std::string& msg) -> Result {
        r.success = false;
        r.error = msg;
        r.log += "ERROR: " + msg + "\n";
        Log::error("Build: ", msg);
        return r;
    };

    if (!project.isLoaded()) return fail("no project loaded");

    const fs::path projectRoot = fs::path(project.rootPath()).lexically_normal();
    const fs::path mainScene = fs::path(options.mainScene).lexically_normal();
    if (!safePackagePath(mainScene)) return fail("main scene must be a project-relative path");
    if (!fs::is_regular_file(projectRoot / mainScene))
        return fail("main scene not found: " + (projectRoot / mainScene).string());

    // Resolve output directory (relative paths are anchored to the project root).
    fs::path outDir = options.outputDir.empty() ? fs::path("build/export")
                                                : fs::path(options.outputDir);
    if (outDir.is_relative()) outDir = projectRoot / outDir;
    outDir = outDir.lexically_normal();
    r.outputDir = outDir.string();

    // Guard: never export into the project root or inside its asset folders —
    // that would copy a tree into itself.
    if (outDir == projectRoot)
        return fail("output directory must not be the project root itself");
    if (isInside(outDir, fs::path(project.assetsDir())) ||
        isInside(outDir, fs::path(project.scenesDir())) ||
        isInside(outDir, fs::path(project.scriptsDir())))
        return fail("output directory must not be inside assets/scenes/scripts");

    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) return fail("cannot create output directory: " + outDir.string());
    r.log += "Output: " + outDir.string() + "\n";

    const fs::path binDir = fs::path(runtimeBinaryRoot());
    const fs::path runtimeExe = binDir / "SaidaEngineRuntime.exe";
    if (!fs::exists(runtimeExe))
        return fail("runtime template not found — build the SaidaEngineRuntime "
                    "target first: " + runtimeExe.string());

    const std::string gameName = safeFileStem(project.name());
    const fs::path gameExe = outDir / (gameName + ".exe");

    // 1. Runtime exe (renamed to the game name).
    fs::copy_file(runtimeExe, gameExe, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("failed to copy runtime exe: " + ec.message());
    r.gameExe = gameExe.string();
    r.log += "  exe -> " + gameExe.filename().string() + "\n";

    // 1b. Exe version, metadata, and icon (VERSIONINFO / RT_GROUP_ICON).
    {
        ExeMetadata meta;
        meta.productName = project.name().empty() ? gameName : project.name();
        meta.version = options.productVersion.empty() ? kProductVersionNumeric : options.productVersion;
        meta.companyName = options.companyName;
        if (!options.iconPath.empty()) {
            fs::path icon = fs::path(options.iconPath);
            if (icon.is_relative()) icon = projectRoot / icon;
            if (!fs::is_regular_file(icon))
                return fail("icon file not found: " + icon.string());
            meta.iconPath = icon.string();
        }
        std::string metaError;
        if (!applyExeMetadata(gameExe.string(), meta, metaError))
            return fail("exe metadata: " + metaError);
        r.log += "  version " + meta.version +
                 (meta.iconPath.empty() ? "" : " + icon") + "\n";
    }

    // 2. GLFW runtime DLL.
    const fs::path glfwDll = binDir / "glfw3.dll";
    if (fs::exists(glfwDll)) {
        fs::copy_file(glfwDll, outDir / "glfw3.dll",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("failed to copy glfw3.dll: " + ec.message());
        r.log += "  glfw3.dll\n";
    } else {
        r.log += "  ! glfw3.dll not found in build dir (may already be on PATH)\n";
    }

    // 3. Compiled SPIR-V shaders. They live in the build's shader output dir
    //    (SAIDA_SHADER_DIR); the runtime resolves them under <exe>/shaders/.
    const fs::path shaderDir = fs::path(shaderRoot());
    if (!fs::exists(shaderDir))
        return fail("compiled shaders not found: " + shaderDir.string());
    if (!copyTree(shaderDir, outDir / "shaders", r.log))
        return fail("failed to copy shaders");

    // 4. Engine default fonts, then project data in a flat layout so
    //    Project::load() sets rootPath = outDir.
    {
        std::string fontError;
        if (!copyEngineFonts(outDir, r.log, fontError)) return fail(fontError);
    }
    if (!copyProjectData(project, outDir, r.log))
        return fail("failed to copy project data");

    // 5. Boot manifest (read by SaidaEngineRuntime at startup).
    if (!writeBootManifest(project, outDir, options.mainScene))
        return fail("cannot write boot manifest game.saida");
    r.log += "  game.saida (main_scene=" + options.mainScene + ")\n";

    r.success = true;
    r.log += "Build succeeded.\n";
    Log::info("Build succeeded: ", gameExe.string());

    if (options.launchAfterBuild) launch(r.gameExe);
    return r;
}

BuildExporter::Result BuildExporter::exportWebBuild(const Project& project,
                                                    const Options& options) {
    Result r;
    auto fail = [&](const std::string& msg) -> Result {
        r.success = false;
        r.error = msg;
        r.log += "ERROR: " + msg + "\n";
        Log::error("WebBuild: ", msg);
        return r;
    };

    if (!project.isLoaded()) return fail("no project loaded");
    const fs::path projectRoot = fs::path(project.rootPath()).lexically_normal();
    const fs::path mainScene = fs::path(options.mainScene).lexically_normal();
    if (!safePackagePath(mainScene)) return fail("main scene must be a project-relative path");
    if (!fs::is_regular_file(projectRoot / mainScene))
        return fail("main scene not found: " + (projectRoot / mainScene).string());

    const fs::path webBuild = installedLayout()
        ? fs::path(engineRoot()) / "web-player"
        : fs::path(engineRoot()) / "build-web-player";
    if (!fs::exists(webBuild / "index.html"))
        return fail("web player template missing — build web/player first (emsdk required)");

    fs::path outDir = options.outputDir.empty() ? fs::path("build/export")
                                                : fs::path(options.outputDir);
    if (outDir.is_relative()) outDir = projectRoot / outDir;
    outDir /= "web";
    outDir = outDir.lexically_normal();
    if (outDir == projectRoot || isInside(outDir, projectRoot / "assets") ||
        isInside(outDir, projectRoot / "scenes") ||
        isInside(outDir, projectRoot / "scripts") ||
        isInside(outDir, projectRoot / "ui"))
        return fail("output directory must not be inside packaged project data");
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) return fail("cannot create " + outDir.string());
    r.outputDir = outDir.string();
    r.log += "Web export -> " + outDir.string() + "\n";

    for (const char* name : {"index.html", "index.js", "index.wasm", "index.data"}) {
        const fs::path src = webBuild / name;
        if (!fs::exists(src)) continue;
        fs::copy_file(src, outDir / name, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("failed to copy " + src.string());
        r.log += std::string("  copied ") + name + "\n";
    }
    const fs::path packagedProject = outDir / "project";
    fs::create_directories(packagedProject, ec);
    if (ec || !copyProjectData(project, packagedProject, r.log))
        return fail("failed to copy project data");
    if (!writeBootManifest(project, packagedProject, options.mainScene))
        return fail("cannot write web boot manifest");

    // Schema 2: `boot` is mounted into MEMFS before main(); `streamed` (types
    // served async by the AssetLoader) is fetched on demand on first load —
    // preload no longer pays the cost of large assets.
    nlohmann::json bootFiles = nlohmann::json::array();
    nlohmann::json streamedFiles = nlohmann::json::array();
    for (const std::string& file : packageFileList(packagedProject)) {
        const std::string ext = fs::path(file).extension().string();
        const bool streamed = ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                              ext == ".obj" || ext == ".srig" || ext == ".sclip" ||
                              ext == ".sgraph";
        (streamed ? streamedFiles : bootFiles).push_back(file);
    }
    nlohmann::json fileManifest = {
        {"schema", 2},
        {"files", std::move(bootFiles)},
        {"streamed", std::move(streamedFiles)}
    };
    std::ofstream filesJson(outDir / "project-files.json", std::ios::trunc);
    if (!filesJson) return fail("cannot write project-files.json");
    filesJson << fileManifest.dump(2) << "\n";
    filesJson.close();
    r.log += "  project-files.json\n";
    const fs::path serveScript = installedLayout()
        ? fs::path(engineRoot()) / "web-player" / "serve.py"
        : fs::path(engineRoot()) / "web" / "serve.py";
    ec.clear();
    if (fs::is_regular_file(serveScript)) {
        fs::copy_file(serveScript, outDir / "serve.py",
                      fs::copy_options::overwrite_existing, ec);
        if (!ec) r.log += "  copied serve.py (COOP/COEP dev server)\n";
    }

    {
        std::ofstream readme(outDir / "README.txt", std::ios::trunc);
        readme << "SaidaEngine web build\n"
               << "Serve with COOP/COEP headers (required for WASM threads):\n"
               << "  python serve.py . 8080\n"
               << "then open http://localhost:8080/index.html in a WebGPU browser.\n";
    }

    r.success = true;
    r.gameExe = (outDir / "index.html").string();
    r.log += "Web export succeeded.\n";
    Log::info("Web export succeeded: ", outDir.string());
    return r;
}

bool BuildExporter::launch(const std::string& exePath) {
#ifdef _WIN32
    const fs::path exe(exePath);
    const std::string dir = exe.parent_path().string();
    HINSTANCE h = ::ShellExecuteA(nullptr, "open", exePath.c_str(), nullptr,
                                  dir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        Log::error("Build: failed to launch ", exePath);
        return false;
    }
    return true;
#else
    (void)exePath;
    return false;
#endif
}

void BuildExporter::openInExplorer(const std::string& dir) {
#ifdef _WIN32
    ::ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)dir;
#endif
}

} // namespace saida
