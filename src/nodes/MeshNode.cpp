#include "nodes/MeshNode.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Material.hpp"
#include <nlohmann/json.hpp>

namespace saida {

void MeshNode::setLods(std::vector<MeshLodLevel> levels) {
    lods_ = std::move(levels);
    activeLodIndex_ = 0;
    if (lods_.size() > 1 && !getBehaviour<LODGroupBehaviour>())
        addBehaviour<LODGroupBehaviour>();
}

Mesh* MeshNode::meshForLod(int lodIndex) const {
    if (lodIndex <= 0 || lods_.empty()) return mesh_;
    if (lodIndex >= static_cast<int>(lods_.size())) return lods_.back().mesh ? lods_.back().mesh : mesh_;
    return lods_[static_cast<size_t>(lodIndex)].mesh ? lods_[static_cast<size_t>(lodIndex)].mesh : mesh_;
}

Material* MeshNode::materialForLod(int lodIndex) const {
    if (lodIndex <= 0 || lods_.empty()) return material_;
    if (lodIndex >= static_cast<int>(lods_.size())) {
        Material* m = lods_.back().material;
        return m ? m : material_;
    }
    Material* m = lods_[static_cast<size_t>(lodIndex)].material;
    return m ? m : material_;
}

int MeshNode::selectLodIndex(float screenCoverage) const {
    if (lods_.empty()) return 0;
    const int picked = saida::selectLodIndex(screenCoverage, lods_);
    if (picked == activeLodIndex_) return picked;
    if (activeLodIndex_ < 0 || activeLodIndex_ >= static_cast<int>(lods_.size()))
        return picked;

    constexpr float kHysteresis = 0.10f;
    if (picked > activeLodIndex_) {
        const float currentMin = lods_[static_cast<size_t>(activeLodIndex_)].minScreenCoverage;
        if (screenCoverage > currentMin * (1.0f - kHysteresis))
            return activeLodIndex_;
    } else {
        const float pickedMin = lods_[static_cast<size_t>(picked)].minScreenCoverage;
        if (screenCoverage < pickedMin * (1.0f + kHysteresis))
            return activeLodIndex_;
    }
    return picked;
}

void MeshNode::captureDurableResourceRefs(const nlohmann::json& j) {
    static const char* kRefKeys[] = {"mesh",      "texture", "baseColor",
                                     "metallic",  "roughness", "ao",
                                     "emissive",  "shader",  "lods"};
    nlohmann::json refs = nlohmann::json::object();
    for (const char* key : kRefKeys)
        if (auto it = j.find(key); it != j.end()) refs[key] = *it;
    durableResourceRefs_ = refs.empty() ? std::string() : refs.dump();
}

void MeshNode::serialize(nlohmann::json& j, ResourceManager& resources) const {
    Node::serialize(j, resources);
    j["mesh"] = resources.meshId(mesh_);
    if (material_) {
        const MaterialDesc& d = material_->desc();
        j["texture"] = d.albedoId;
        j["baseColor"] = {d.baseColor.r, d.baseColor.g, d.baseColor.b, d.baseColor.a};
        j["metallic"] = d.metallic;
        j["roughness"] = d.roughness;
        j["ao"] = d.ao;
        j["emissive"] = {d.emissiveColor.r, d.emissiveColor.g, d.emissiveColor.b, d.emissiveColor.a};
        j["shader"] = (d.type == MaterialType::Unlit) ? "unlit" : "lit";
    }
    j["castShadows"] = castShadows_;
    j["meshEnabled"] = meshEnabled_;
    j["outlineEnabled"] = outlineEnabled_;
    j["outlineColor"] = {outlineColor_.r, outlineColor_.g, outlineColor_.b, outlineColor_.a};
    j["outlineWidth"] = outlineWidth_;
    if (lods_.size() > 1) {
        nlohmann::json arr = nlohmann::json::array();
        for (const MeshLodLevel& lvl : lods_) {
            nlohmann::json entry;
            entry["mesh"] = resources.meshId(lvl.mesh ? lvl.mesh : mesh_);
            if (lvl.material)
                entry["materialAlbedo"] = lvl.material->desc().albedoId;
            entry["minCoverage"] = lvl.minScreenCoverage;
            arr.push_back(std::move(entry));
        }
        j["lods"] = std::move(arr);
    }
}

void MeshNode::deserialize(const nlohmann::json& j, ResourceManager& resources) {
    Node::deserialize(j, resources);
    captureDurableResourceRefs(j);
    if (j.contains("mesh")) {
        AssetID meshId = kAssetInvalid;
        if (j["mesh"].is_number_integer()) {
            meshId = j["mesh"].get<AssetID>();
        } else if (j["mesh"].is_string()) {
            std::string meshStr = j["mesh"].get<std::string>();
            if (meshStr == "cube") {
                meshId = kAssetBuiltinCube;
            } else {
                meshId = resources.getOrRegister(meshStr, AssetType::Mesh);
            }
        }
        if (meshId != kAssetInvalid) {
            mesh_ = resources.getMesh(meshId);
        }
    }
    if (j.contains("texture") || j.contains("baseColor")) {
        MaterialDesc desc{};
        if (j.contains("texture")) {
            if (j["texture"].is_number_integer()) {
                desc.albedoId = j["texture"].get<AssetID>();
            } else if (j["texture"].is_string()) {
                desc.albedoId = resources.getOrRegister(j["texture"].get<std::string>(), AssetType::Texture);
            }
        }
        if (j.contains("baseColor") && j["baseColor"].is_array() && j["baseColor"].size() == 4) {
            desc.baseColor = {j["baseColor"][0].get<float>(), j["baseColor"][1].get<float>(),
                              j["baseColor"][2].get<float>(), j["baseColor"][3].get<float>()};
        }
        if (j.contains("metallic")) desc.metallic = j["metallic"].get<float>();
        if (j.contains("roughness")) desc.roughness = j["roughness"].get<float>();
        if (j.contains("ao")) desc.ao = j["ao"].get<float>();
        if (j.contains("emissive") && j["emissive"].is_array() && j["emissive"].size() == 4) {
            desc.emissiveColor = {j["emissive"][0].get<float>(), j["emissive"][1].get<float>(),
                                  j["emissive"][2].get<float>(), j["emissive"][3].get<float>()};
        }
        if (j.contains("shader"))
            desc.type = (j["shader"].get<std::string>() == "unlit") ? MaterialType::Unlit
                                                                    : MaterialType::Lit;
        material_ = resources.getMaterial(desc);
    }
    if (j.contains("castShadows")) castShadows_ = j["castShadows"].get<bool>();
    if (j.contains("meshEnabled")) meshEnabled_ = j["meshEnabled"].get<bool>();
    if (j.contains("outlineEnabled")) outlineEnabled_ = j["outlineEnabled"].get<bool>();
    if (j.contains("outlineColor") && j["outlineColor"].is_array() && j["outlineColor"].size() == 4) {
        outlineColor_ = {j["outlineColor"][0].get<float>(), j["outlineColor"][1].get<float>(),
                         j["outlineColor"][2].get<float>(), j["outlineColor"][3].get<float>()};
    }
    if (j.contains("outlineWidth")) outlineWidth_ = j["outlineWidth"].get<float>();
    if (j.contains("lods") && j["lods"].is_array()) {
        std::vector<MeshLodLevel> levels;
        for (const auto& entry : j["lods"]) {
            MeshLodLevel lvl;
            if (entry.contains("mesh")) {
                AssetID meshId = kAssetInvalid;
                if (entry["mesh"].is_number_integer())
                    meshId = entry["mesh"].get<AssetID>();
                else if (entry["mesh"].is_string())
                    meshId = resources.getOrRegister(entry["mesh"].get<std::string>(), AssetType::Mesh);
                if (meshId != kAssetInvalid)
                    lvl.mesh = resources.getMesh(meshId);
            }
            if (entry.contains("materialAlbedo")) {
                MaterialDesc desc{};
                if (entry["materialAlbedo"].is_number_integer())
                    desc.albedoId = entry["materialAlbedo"].get<AssetID>();
                else if (entry["materialAlbedo"].is_string())
                    desc.albedoId = resources.getOrRegister(entry["materialAlbedo"].get<std::string>(), AssetType::Texture);
                lvl.material = resources.getMaterial(desc);
            }
            if (entry.contains("minCoverage"))
                lvl.minScreenCoverage = entry["minCoverage"].get<float>();
            levels.push_back(lvl);
        }
        setLods(std::move(levels));
    }
}

} // namespace saida
