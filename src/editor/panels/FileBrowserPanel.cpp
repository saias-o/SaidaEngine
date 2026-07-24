#include "editor/panels/FileBrowserPanel.hpp"
#include "editor/EditorUI.hpp"
#include "project/Project.hpp"
#include "graphics/ResourceManager.hpp"

#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace saida {

namespace {

static std::string toLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
    return lower;
}

const char* fileIcon(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") return "[3D]";
    if (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".hdr")  return "[Img]";
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl" || ext == ".spv") return "[Sh]";
    if (ext == ".html" || ext == ".htm" || ext == ".css" || ext == ".js") return "[Web]";
    if (ext == ".lua")   return "[Lua]";
    if (ext == ".bvh")   return "[Anim]";
    if (ext == ".saidaproj") return "[Proj]";
    if (ext == ".scene") return "[Sc]";
    return "[F]";
}

} // namespace

void FileBrowserPanel::draw(EditorUI* editor, Project* project, ResourceManager* resources) {
    editor->thumbnails_.beginFrame();  // advance the cache clock / drain retired thumbnails
    ImGui::Begin("File Browser", &editor->showFileBrowser_);

    if (!project || !project->isLoaded()) {
        ImGui::TextDisabled("No project loaded.");
        ImGui::TextDisabled("Use File > New Project or Open Project to get started.");
        ImGui::End();
        return;
    }

    namespace fs = std::filesystem;
    fs::path root(project->rootPath());

    if (editor->currentBrowsePath_.empty() ||
        editor->currentBrowsePath_.find(project->rootPath()) == std::string::npos) {
        editor->currentBrowsePath_ = project->rootPath();
    }

    fs::path browsePath(editor->currentBrowsePath_);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    
    if (ImGui::Button(project->name().c_str())) {
        editor->currentBrowsePath_ = root.string();
    }

    if (browsePath != root) {
        std::vector<fs::path> components;
        fs::path p = browsePath;
        while (p != root && !p.empty() && p.string().find(root.string()) != std::string::npos) {
            components.push_back(p);
            p = p.parent_path();
        }
        std::reverse(components.begin(), components.end());

        for (const auto& comp : components) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::Button(comp.filename().string().c_str())) {
                editor->currentBrowsePath_ = comp.string();
            }
        }
    }
    ImGui::PopStyleColor();

    ImGui::SameLine(ImGui::GetWindowWidth() - 360.0f);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##FileSearch", "Search files...", editor->fileBrowserSearchBuf_, sizeof(editor->fileBrowserSearchBuf_));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("##Zoom", &editor->fileBrowserZoom_, 0.5f, 3.0f, "Zoom %.1f");

    if (ImGui::IsWindowHovered()) {
        float scroll = ImGui::GetIO().MouseWheel;
        if (scroll != 0.0f && ImGui::GetIO().KeyCtrl) {
            editor->fileBrowserZoom_ += scroll * 0.15f; 
            editor->fileBrowserZoom_ = std::clamp(editor->fileBrowserZoom_, 0.5f, 3.0f);
        }
    }

    ImGui::Separator();

    try {
        std::string query = toLower(editor->fileBrowserSearchBuf_);

        // Cached listing: refresh on path/query change, local mutation, or timer.
        constexpr double kRefreshSeconds = 1.0;
        EditorUI::FileListing& listing = editor->fileListing_;
        const std::string browsePathStr = browsePath.string();
        const double now = ImGui::GetTime();
        if (listing.path != browsePathStr || listing.query != query ||
            listing.time < 0.0 || now - listing.time > kRefreshSeconds) {
            listing.path = browsePathStr;
            listing.query = query;
            listing.time = now;
            listing.dirs.clear();
            listing.files.clear();

            auto consider = [&](const fs::directory_entry& entry) {
                const std::string name = entry.path().filename().string();
                if (name.empty() || name[0] == '.' || name == "build") return;
                if (!query.empty()) {
                    const std::string nameLower = toLower(name);
                    const std::string extLower = toLower(entry.path().extension().string());
                    if (nameLower.find(query) == std::string::npos &&
                        extLower.find(query) == std::string::npos)
                        return;
                }
                (entry.is_directory() ? listing.dirs : listing.files).push_back(entry.path().string());
            };

            if (query.empty())
                for (auto& e : fs::directory_iterator(browsePath)) consider(e);
            else
                for (auto& e : fs::recursive_directory_iterator(project->rootPath())) consider(e);

            auto byName = [](const std::string& a, const std::string& b) {
                return fs::path(a).filename() < fs::path(b).filename();
            };
            std::sort(listing.dirs.begin(), listing.dirs.end(), byName);
            std::sort(listing.files.begin(), listing.files.end(), byName);
        }

        const std::vector<std::string>& dirs = listing.dirs;
        const std::vector<std::string>& files = listing.files;

        bool useGrid = editor->fileBrowserZoom_ > 1.0f;
        float iconSize = 64.0f * editor->fileBrowserZoom_;
        float cellSize = iconSize + 20.0f;

        if (useGrid) {
            float panelWidth = ImGui::GetContentRegionAvail().x;
            int columnCount = std::max(1, static_cast<int>(panelWidth / cellSize));
            ImGui::Columns(columnCount, nullptr, false);
        }

        std::string pathToDelete_;

        auto drawItemContextMenu = [&](const std::string& pathStr, const std::string& filename) {
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Show in Explorer")) {
#ifdef _WIN32
                    std::string winPath = pathStr;
                    std::replace(winPath.begin(), winPath.end(), '/', '\\');
                    std::string cmd = "explorer /select,\"" + winPath + "\"";
                    std::system(cmd.c_str());
#endif
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Rename")) {
                    editor->fileToRename_ = pathStr;
                    std::strncpy(editor->fileRenameBuf_, filename.c_str(), sizeof(editor->fileRenameBuf_) - 1);
                    editor->fileRenameBuf_[sizeof(editor->fileRenameBuf_) - 1] = '\0';
                }
                if (ImGui::MenuItem("Delete")) {
                    pathToDelete_ = pathStr;
                }
                ImGui::EndPopup();
            }
        };

        auto handleInlineRename = [&](const std::string& pathStr, float width) {
            if (editor->fileToRename_ == pathStr) {
                ImGui::SetNextItemWidth(width);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText(("##rename_" + pathStr).c_str(), editor->fileRenameBuf_, sizeof(editor->fileRenameBuf_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    try { fs::rename(editor->fileToRename_, fs::path(editor->fileToRename_).parent_path() / editor->fileRenameBuf_); } catch(...) {}
                    editor->fileToRename_.clear();
                    editor->fileListing_.time = -1.0;  // refresh listing
                } else if (ImGui::IsItemDeactivated() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                    try { fs::rename(editor->fileToRename_, fs::path(editor->fileToRename_).parent_path() / editor->fileRenameBuf_); } catch(...) {}
                    editor->fileToRename_.clear();
                    editor->fileListing_.time = -1.0;  // refresh listing
                }
                return true;
            }
            return false;
        };

        char buffer[256];
        for (const auto& pathStr : dirs) {
            std::string filename = fs::path(pathStr).filename().string();
            if (useGrid) {
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
                ImGui::Button(("[DIR]\n" + filename).c_str(), ImVec2(iconSize, iconSize));
                ImGui::PopStyleColor();
                drawItemContextMenu(pathStr, filename);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    editor->currentBrowsePath_ = pathStr;
                
                if (!handleInlineRename(pathStr, iconSize)) {
                    ImGui::TextWrapped("%s", filename.c_str());
                }
                ImGui::EndGroup();
                ImGui::NextColumn();
            } else {
                if (!handleInlineRename(pathStr, -1.0f)) {
                    std::snprintf(buffer, sizeof(buffer), "[D] %s", filename.c_str());
                    if (ImGui::Selectable(buffer, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            editor->currentBrowsePath_ = pathStr;
                    }
                    drawItemContextMenu(pathStr, filename);
                }
            }
        }
        
        for (const auto& pathStr : files) {
            fs::path fp(pathStr);
            std::string filename = fp.filename().string();
            auto ext = fp.extension().string();

            if (useGrid) {
                ImGui::BeginGroup();
                
                if (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".hdr") {
                    // Thumbnails are lazy and budgeted; placeholders resolve later.
                    ImTextureID thumb = 0;
                    if (ImGui::IsRectVisible(ImVec2(iconSize, iconSize)))
                        thumb = editor->thumbnails_.get(resources->device(), pathStr);
                    if (thumb)
                        ImGui::Image(thumb, ImVec2(iconSize, iconSize));
                    else
                        ImGui::Button("[IMG]", ImVec2(iconSize, iconSize));
                } else if (ext == ".scene") {
                    ImGui::Button("[SCENE]", ImVec2(iconSize, iconSize));
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        editor->loadScene(pathStr);
                    }
                } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") {
                    ImGui::Button("[AUDIO]", ImVec2(iconSize, iconSize));
                } else if (ext == ".html" || ext == ".htm" || ext == ".css" || ext == ".js") {
                    ImGui::Button("[WEB]", ImVec2(iconSize, iconSize));
                } else if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
                    ImGui::Button("[3D]", ImVec2(iconSize, iconSize));
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        editor->openModelImporter(pathStr, resources);
                    }
                } else {
                    ImGui::Button("[FILE]", ImVec2(iconSize, iconSize));
                }

                if (ext == ".scene") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_SCENE", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Instantiate %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_MODEL", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Instantiate Model %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".hdr") {
                    if (ImGui::BeginDragDropSource()) {
                        AssetID id = project->assetRegistry().getID(pathStr);
                        if (id == kAssetInvalid) {
                            AssetType type = (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".hdr") ? AssetType::Texture : AssetType::Mesh;
                            id = resources->getOrRegister(pathStr, type);
                        }
                        if (id != kAssetInvalid) {
                            ImGui::SetDragDropPayload("ASSET_ID", &id, sizeof(AssetID));
                            ImGui::Text("Assign %s", filename.c_str());
                        }
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".ogg" || ext == ".wav" || ext == ".mp3") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_AUDIO", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Assign Audio %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".html" || ext == ".htm") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_HTML", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Assign Web %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".bvh") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_BVH", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Add Animation %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                }

                drawItemContextMenu(pathStr, filename);

                if (!handleInlineRename(pathStr, iconSize)) {
                    ImGui::TextWrapped("%s", filename.c_str());
                }
                ImGui::EndGroup();
                ImGui::NextColumn();
            } else {
                if (!handleInlineRename(pathStr, -1.0f)) {
                    std::snprintf(buffer, sizeof(buffer), "%s %s", fileIcon(fp), filename.c_str());
                    if (ImGui::Selectable(buffer, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            if (ext == ".scene") {
                                editor->loadScene(pathStr);
                            } else if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
                                editor->openModelImporter(pathStr, resources);
                            }
                        }
                    }
                    drawItemContextMenu(pathStr, filename);
                }
                
                if (ext == ".scene") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_SCENE", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Instantiate %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_MODEL", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Instantiate Model %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".hdr") {
                    if (ImGui::BeginDragDropSource()) {
                        AssetID id = project->assetRegistry().getID(pathStr);
                        if (id == kAssetInvalid) {
                            AssetType type = (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".hdr") ? AssetType::Texture : AssetType::Mesh;
                            id = resources->getOrRegister(pathStr, type);
                        }
                        if (id != kAssetInvalid) {
                            ImGui::SetDragDropPayload("ASSET_ID", &id, sizeof(AssetID));
                            ImGui::Text("Assign %s", filename.c_str());
                        }
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".ogg" || ext == ".wav" || ext == ".mp3") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_AUDIO", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Assign Audio %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".html" || ext == ".htm") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_HTML", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Assign Web %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                } else if (ext == ".bvh") {
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("FILE_BVH", pathStr.c_str(), pathStr.size() + 1);
                        ImGui::Text("Add Animation %s", filename.c_str());
                        ImGui::EndDragDropSource();
                    }
                }
            }
        }
        
        if (!pathToDelete_.empty()) {
            try { fs::remove_all(pathToDelete_); } catch (...) {}
            editor->fileListing_.time = -1.0;  // refresh listing
        }

        if (useGrid) {
            ImGui::Columns(1);
        }
    } catch (const fs::filesystem_error&) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error reading directory");
    }

    ImGui::End();
}

} // namespace saida
