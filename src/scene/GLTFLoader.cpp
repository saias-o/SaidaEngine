#include "scene/GLTFLoader.hpp"
#include "scene/GltfMeshopt.hpp"
#include "core/Profiler.hpp"
#include "scene/Scene.hpp"
#include "nodes/MeshNode.hpp"
#include "scene/MeshLod.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "scene/Node.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/ResourceManager.hpp"
#include "scene/animation/Rig.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipNode.hpp"
#include "cli/AutoLODBridge.hpp"
#include "core/Log.hpp"

#include <cgltf.h>
#include <nlohmann/json.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>
#include <cmath>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace saida {

// A bare cgltf_result number in a log says nothing about which third-party file
// is at fault or why, and these failures are almost always an exporter quirk in
// content the engine did not produce. Naming the cause is what makes the file
// fixable instead of merely rejected.
static const char* cgltfResultName(cgltf_result result) {
    switch (result) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "truncated file";
        case cgltf_result_unknown_format: return "unknown format (not glTF/GLB)";
        case cgltf_result_invalid_json: return "malformed JSON";
        case cgltf_result_invalid_gltf:
            return "invalid glTF (a common cause is a scene root that is also "
                   "another node's child)";
        case cgltf_result_invalid_options: return "invalid loader options";
        case cgltf_result_file_not_found: return "file not found";
        case cgltf_result_io_error: return "I/O error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
        default: return "unknown error";
    }
}

static glm::vec4 toVec4(const float* f) { return {f[0], f[1], f[2], f[3]}; }
static glm::vec3 toVec3(const float* f) { return {f[0], f[1], f[2]}; }

static Transform nodeTransform(const cgltf_node* node) {
    Transform result;
    if (node->has_matrix) {
        const glm::mat4 matrix = glm::make_mat4(node->matrix);
        glm::vec3 skew;
        glm::vec4 perspective;
        if (glm::decompose(matrix, result.scale, result.rotation, result.position,
                           skew, perspective)) {
            result.rotation = glm::normalize(result.rotation);
        }
        return result;
    }

    if (node->has_translation) result.position = toVec3(node->translation);
    if (node->has_rotation) {
        result.rotation = glm::normalize(glm::quat(
            node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]));
    }
    if (node->has_scale) result.scale = toVec3(node->scale);
    return result;
}

// glTF wrap modes (KHR values). MIRRORED_REPEAT has no sampler of its own here
// and falls back to REPEAT, which is what it degrades to most gracefully.
static rhi::AddressMode gltfWrap(cgltf_texture* tex) {
    if (!tex || !tex->sampler) return rhi::AddressMode::Repeat;
    constexpr int kClampToEdge = 33071;
    const bool clampS = tex->sampler->wrap_s == kClampToEdge;
    const bool clampT = tex->sampler->wrap_t == kClampToEdge;
    // The sampler applies one mode to every axis, so a mixed declaration takes
    // the clamped one: repeating an axis the author clamped tiles the art, which
    // is the visible failure; clamping one they repeated only stretches an edge.
    return (clampS || clampT) ? rhi::AddressMode::ClampToEdge : rhi::AddressMode::Repeat;
}

static AssetID loadGLTFTexture(cgltf_texture* tex, ResourceManager& resources, const std::filesystem::path& basePath, bool srgb = true) {
    if (!tex || !tex->image) return kAssetInvalid;

    const rhi::AddressMode address = gltfWrap(tex);
    if (tex->image->buffer_view) {
        // Embedded texture
        const uint8_t* data = static_cast<const uint8_t*>(tex->image->buffer_view->buffer->data) + tex->image->buffer_view->offset;
        return resources.registerMemoryTexture(data, tex->image->buffer_view->size, srgb, address);
    } else if (tex->image->uri) {
        std::filesystem::path uriPath = tex->image->uri;
        if (uriPath.is_absolute()) return resources.getOrRegister(uriPath.string(), AssetType::Texture, srgb, address);
        else return resources.getOrRegister((basePath / uriPath).string(), AssetType::Texture, srgb, address);
    }
    return kAssetInvalid;
}

struct NodeLodInfo {
    std::vector<size_t> lowerDetailNodeIndices;
    std::vector<float> msftCoverage;
};

static const cgltf_extension* findExtension(const cgltf_node* node, const char* name) {
    for (size_t i = 0; i < node->extensions_count; ++i) {
        if (std::strcmp(node->extensions[i].name, name) == 0)
            return &node->extensions[i];
    }
    return nullptr;
}

static std::vector<size_t> parseMsftLodIds(const cgltf_node* node) {
    const cgltf_extension* ext = findExtension(node, "MSFT_lod");
    if (!ext || !ext->data) return {};
    try {
        auto j = nlohmann::json::parse(ext->data);
        if (!j.contains("ids") || !j["ids"].is_array()) return {};
        std::vector<size_t> ids;
        for (const auto& v : j["ids"]) ids.push_back(v.get<size_t>());
        return ids;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

static std::vector<float> parseMsftScreenCoverage(const cgltf_node* node) {
    if (!node->extras.data) return {};
    try {
        auto j = nlohmann::json::parse(node->extras.data);
        if (!j.contains("MSFT_screencoverage") || !j["MSFT_screencoverage"].is_array()) return {};
        std::vector<float> cov;
        for (const auto& v : j["MSFT_screencoverage"]) cov.push_back(v.get<float>());
        return cov;
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

static bool primHasAuthorTangents(const cgltf_primitive* prim) {
    for (size_t k = 0; k < prim->attributes_count; ++k)
        if (prim->attributes[k].type == cgltf_attribute_type_tangent) return true;
    return false;
}

static Material* resolvePrimitiveMaterial(cgltf_primitive* prim, cgltf_data* data,
                                          ResourceManager& resources,
                                          const std::vector<MaterialDesc>& materials) {
    if (prim->material) {
        size_t matIdx = prim->material - data->materials;
        MaterialDesc desc = materials[matIdx];
        // V1 policy: without authored tangents, normal mapping is explicitly
        // disabled — tangents reconstructed by triangle averaging aren't
        // MikkTSpace and would silently produce incorrect lighting.
        // (MikkTSpace: P1.)
        if (desc.normalId != kAssetInvalid && !primHasAuthorTangents(prim)) {
            Log::warn("GLTFLoader: material ", matIdx, " has a normal map but the "
                      "primitive has no authored tangents — normal mapping disabled");
            desc.normalId = kAssetInvalid;
        }
        return resources.getMaterial(desc);
    }
    return resources.getMaterial(MaterialDesc{});
}

static void buildLodChain(MeshNode* mNode, size_t nodeIndex, size_t primIndex,
                          cgltf_node* node, cgltf_data* data,
                          const std::vector<std::vector<AssetID>>& meshesPrimitives,
                          const std::vector<MaterialDesc>& materials,
                          ResourceManager& resources,
                          const std::unordered_map<size_t, NodeLodInfo>& lodByNodeIndex) {
    auto it = lodByNodeIndex.find(nodeIndex);
    if (it == lodByNodeIndex.end()) return;

    const NodeLodInfo& info = it->second;
    const size_t lodCount = 1 + info.lowerDetailNodeIndices.size();
    const std::vector<float> thresholds = coverageThresholdsFromMsft(info.msftCoverage, lodCount);

    std::vector<MeshLodLevel> levels;
    levels.reserve(lodCount);

    MeshLodLevel lod0;
    lod0.mesh = mNode->mesh();
    lod0.material = mNode->material();
    lod0.minScreenCoverage = thresholds.empty() ? 0.0f : thresholds[0];
    levels.push_back(lod0);

    for (size_t j = 0; j < info.lowerDetailNodeIndices.size(); ++j) {
        const size_t proxyIdx = info.lowerDetailNodeIndices[j];
        if (proxyIdx >= data->nodes_count) continue;
        cgltf_node* proxy = &data->nodes[proxyIdx];
        if (!proxy->mesh || proxy->mesh->primitives_count == 0) continue;

        const size_t proxyMeshIdx = proxy->mesh - data->meshes;
        const size_t proxyPrim = std::min(primIndex, proxy->mesh->primitives_count - 1);
        if (proxyMeshIdx >= meshesPrimitives.size() || proxyPrim >= meshesPrimitives[proxyMeshIdx].size())
            continue;

        MeshLodLevel lvl;
        lvl.mesh = resources.getMesh(meshesPrimitives[proxyMeshIdx][proxyPrim]);
        lvl.material = resolvePrimitiveMaterial(&proxy->mesh->primitives[proxyPrim], data, resources, materials);
        lvl.minScreenCoverage = (j + 1 < thresholds.size()) ? thresholds[j + 1] : 0.0f;
        levels.push_back(lvl);
    }

    if (levels.size() > 1) {
        mNode->setLods(std::move(levels));
        Log::info("GLTFLoader: ", levels.size(), " LOD levels on node ", node->name ? node->name : "?");
    }
}

static void processNode(cgltf_node* node, Node* parent, ResourceManager& resources,
                        const std::vector<std::vector<AssetID>>& meshesPrimitives,
                        const std::vector<MaterialDesc>& materials, cgltf_data* data,
                        const std::unordered_set<size_t>& lodProxyNodes,
                        const std::unordered_map<size_t, NodeLodInfo>& lodByNodeIndex,
                        bool autoMeshLods,
                        std::vector<std::pair<MeshNode*, cgltf_skin*>>& skinnedMeshes) {
    const size_t nodeIndex = static_cast<size_t>(node - data->nodes);
    if (lodProxyNodes.count(nodeIndex)) return;

    Node* neNode = parent->createChild<Node>(node->name ? node->name : "Node");

    neNode->transform() = nodeTransform(node);

    if (node->mesh) {
        size_t meshIdx = node->mesh - data->meshes;
        const auto& primitives = meshesPrimitives[meshIdx];
        
        for (size_t i = 0; i < primitives.size(); ++i) {
            cgltf_primitive* prim = &node->mesh->primitives[i];
            Material* mat = resolvePrimitiveMaterial(prim, data, resources, materials);
            
            std::string primName = (node->name ? std::string(node->name) : "Mesh") + "_prim" + std::to_string(i);
            MeshNode* mNode = neNode->createChild<MeshNode>(primName, resources.getMesh(primitives[i]), mat);
            buildLodChain(mNode, nodeIndex, i, node, data, meshesPrimitives, materials, resources, lodByNodeIndex);
            if (autoMeshLods && !mNode->getBehaviour<LODGroupBehaviour>()) {
                MeshLodLevel base;
                base.mesh = mNode->mesh();
                base.material = mNode->material();
                base.minScreenCoverage = 0.0f;
                mNode->setLods({base});
                mNode->addBehaviour<LODGroupBehaviour>();
            }
            if (node->skin) {
                skinnedMeshes.push_back({mNode, node->skin});
            }
        }
    }

    for (size_t i = 0; i < node->children_count; ++i) {
        processNode(node->children[i], neNode, resources, meshesPrimitives, materials, data,
                    lodProxyNodes, lodByNodeIndex, autoMeshLods, skinnedMeshes);
    }
}

// Rig construction shared by load() and loadAnimationData(): bone order follows
// skin.joints (JOINTS_0-compatible), rest pose from the joint node TRS.
static std::unique_ptr<Rig> buildRigFromSkin(const cgltf_skin& skin, std::string* error) {
    auto rig = std::make_unique<Rig>();

    for (size_t j = 0; j < skin.joints_count; ++j) {
        cgltf_node* joint = skin.joints[j];
        std::string name = joint->name ? joint->name : "Bone_" + std::to_string(j);

        int32_t parentIndex = -1;
        if (joint->parent) {
            for (size_t p = 0; p < skin.joints_count; ++p) {
                if (skin.joints[p] == joint->parent) {
                    parentIndex = static_cast<int32_t>(p);
                    break;
                }
            }
        }

        glm::mat4 invBind{1.0f};
        if (skin.inverse_bind_matrices) {
            cgltf_accessor_read_float(skin.inverse_bind_matrices, j, glm::value_ptr(invBind), 16);
        }

        rig->addBone(name, parentIndex, invBind, nodeTransform(joint));
    }

    if (!rig->finalize(error)) return nullptr;
    return rig;
}

// Clip construction shared by load() and loadAnimationData().
static std::unique_ptr<AnimationClip> buildClipFromAnimation(const cgltf_animation& anim,
                                                             size_t index) {
    std::string animName = anim.name ? anim.name : "Anim_" + std::to_string(index);

    // Find max duration
    float duration = 0.0f;
    for (size_t s = 0; s < anim.samplers_count; ++s) {
        cgltf_accessor* input = anim.samplers[s].input;
        if (input && input->count > 0) {
            float maxTime = 0.0f;
            cgltf_accessor_read_float(input, input->count - 1, &maxTime, 1);
            if (maxTime > duration) duration = maxTime;
        }
    }

    auto clip = std::make_unique<AnimationClip>(animName, duration);

    for (size_t c = 0; c < anim.channels_count; ++c) {
        cgltf_animation_channel& channel = anim.channels[c];
        if (!channel.target_node) continue;

        std::string targetName = channel.target_node->name ? channel.target_node->name : "";
        if (targetName.empty()) continue; // We need names for retargeting

        cgltf_animation_sampler* sampler = channel.sampler;
        cgltf_accessor* input = sampler->input;
        cgltf_accessor* output = sampler->output;

        const bool cubic = sampler->interpolation == cgltf_interpolation_type_cubic_spline;
        const TrackInterpolation interpolation = cubic
            ? TrackInterpolation::CubicSpline
            : sampler->interpolation == cgltf_interpolation_type_step
                ? TrackInterpolation::Step
                : TrackInterpolation::Linear;
        const size_t stride = cubic ? 3 : 1;
        const size_t valueOffset = cubic ? 1 : 0;

        size_t count = input->count;
        std::vector<float> timestamps(count);
        for (size_t v = 0; v < count; ++v) {
            cgltf_accessor_read_float(input, v, &timestamps[v], 1);
        }

        if (channel.target_path == cgltf_animation_path_type_translation || channel.target_path == cgltf_animation_path_type_scale) {
            auto track = std::make_unique<TypedAnimTrack<glm::vec3>>();
            track->target = (channel.target_path == cgltf_animation_path_type_translation) ? TrackTarget::Translation : TrackTarget::Scale;
            track->interpolation = interpolation;
            track->timestamps = std::move(timestamps);
            track->values.resize(count);
            for (size_t v = 0; v < count; ++v) {
                cgltf_accessor_read_float(output, v * stride + valueOffset, glm::value_ptr(track->values[v]), 3);
            }
            if (cubic) {
                track->inTangents.resize(count);
                track->outTangents.resize(count);
                for (size_t v = 0; v < count; ++v) {
                    cgltf_accessor_read_float(output, v * 3 + 0, glm::value_ptr(track->inTangents[v]), 3);
                    cgltf_accessor_read_float(output, v * 3 + 2, glm::value_ptr(track->outTangents[v]), 3);
                }
            }
            clip->addTrack(targetName, std::move(track));
        } else if (channel.target_path == cgltf_animation_path_type_rotation) {
            auto track = std::make_unique<TypedAnimTrack<glm::quat>>();
            track->target = TrackTarget::Rotation;
            track->interpolation = interpolation;
            track->timestamps = std::move(timestamps);
            track->values.resize(count);
            for (size_t v = 0; v < count; ++v) {
                float q[4];
                cgltf_accessor_read_float(output, v * stride + valueOffset, q, 4);
                track->values[v] = glm::quat(q[3], q[0], q[1], q[2]); // wxyz
            }
            if (cubic) {
                track->inTangents.resize(count);
                track->outTangents.resize(count);
                for (size_t v = 0; v < count; ++v) {
                    float qi[4], qo[4];
                    cgltf_accessor_read_float(output, v * 3 + 0, qi, 4);
                    cgltf_accessor_read_float(output, v * 3 + 2, qo, 4);
                    track->inTangents[v]  = glm::quat(qi[3], qi[0], qi[1], qi[2]);
                    track->outTangents[v] = glm::quat(qo[3], qo[0], qo[1], qo[2]);
                }
            }
            clip->addTrack(targetName, std::move(track));
        }
    }

    return clip;
}

bool GLTFLoader::loadAnimationData(const std::string& path, GltfAnimationData& out,
                                   std::string* error) {
    cgltf_options cgltfOptions = {};
    cgltf_data* data = nullptr;
    if (cgltf_result parsed = cgltf_parse_file(&cgltfOptions, path.c_str(), &data);
        parsed != cgltf_result_success) {
        if (error) *error = "failed to parse " + path + ": " + cgltfResultName(parsed);
        return false;
    }
    if (cgltf_load_buffers(&cgltfOptions, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        if (error) *error = "failed to load buffers for " + path;
        return false;
    }
    // Hostile content: reject accessors/views outside the declared buffers
    // before any read (an OOB read aborts the wasm player).
    if (cgltf_validate(data) != cgltf_result_success) {
        cgltf_free(data);
        if (error) *error = "invalid glTF data (validation failed) for " + path;
        return false;
    }
    if (!decodeMeshoptBuffers(data)) {
        cgltf_free(data);
        if (error) *error = "failed to decode meshopt buffers for " + path;
        return false;
    }

    for (size_t i = 0; i < data->skins_count; ++i) {
        std::string rigError;
        auto rig = buildRigFromSkin(data->skins[i], &rigError);
        if (!rig) {
            cgltf_free(data);
            if (error) *error = "invalid skin " + std::to_string(i) + ": " + rigError;
            return false;
        }
        out.rigs.push_back(std::move(rig));
    }

    for (size_t i = 0; i < data->animations_count; ++i) {
        auto clip = buildClipFromAnimation(data->animations[i], i);
        out.clipNames.push_back(clip->name());
        out.clips.push_back(std::move(clip));
    }

    cgltf_free(data);
    return true;
}

bool GLTFLoader::load(const std::string& path, Node& rootNode, ResourceManager& resources,
                      const GLTFLoadOptions& options) {
    SAIDA_PROFILE_SCOPE("Resource/LoadGLTF");
    const std::string loadPath = AutoLODBridge::resolveLoadPath(path, options.autoMeshLods);
    Log::info("GLTFLoader: loading ", loadPath);
    
    cgltf_options cgltfOptions = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&cgltfOptions, loadPath.c_str(), &data);
    
    if (result != cgltf_result_success) {
        Log::error("GLTFLoader: failed to parse ", loadPath, " — ",
                   cgltfResultName(result));
        return false;
    }
    
    result = cgltf_load_buffers(&cgltfOptions, data, loadPath.c_str());
    if (result != cgltf_result_success) {
        Log::error("GLTFLoader: Failed to load buffers for ", loadPath);
        cgltf_free(data);
        return false;
    }

    // Hostile content: reject accessors/views outside the declared buffers
    // before any read (an OOB read aborts the wasm player).
    if (cgltf_validate(data) != cgltf_result_success) {
        Log::error("GLTFLoader: validation failed for ", loadPath, " — refusing hostile/corrupt data");
        cgltf_free(data);
        return false;
    }

    // Decode meshopt-compressed buffer views in place (decoded data is owned by
    // `data` and released by the cgltf_free below).
    if (!decodeMeshoptBuffers(data)) {
        cgltf_free(data);
        return false;
    }

    std::filesystem::path basePath = std::filesystem::path(loadPath).parent_path();

    // 1. Load Materials
    std::vector<MaterialDesc> materials(data->materials_count);
    for (size_t i = 0; i < data->materials_count; ++i) {
        cgltf_material& mat = data->materials[i];
        MaterialDesc& desc = materials[i];
        
        if (mat.has_pbr_metallic_roughness) {
            desc.baseColor = toVec4(mat.pbr_metallic_roughness.base_color_factor);
            desc.metallic = mat.pbr_metallic_roughness.metallic_factor;
            desc.roughness = mat.pbr_metallic_roughness.roughness_factor;
            desc.albedoId = loadGLTFTexture(mat.pbr_metallic_roughness.base_color_texture.texture, resources, basePath, true);
            desc.metallicRoughnessId = loadGLTFTexture(mat.pbr_metallic_roughness.metallic_roughness_texture.texture, resources, basePath, false);
        }
        
        desc.normalId = loadGLTFTexture(mat.normal_texture.texture, resources, basePath, false);
        desc.emissiveId = loadGLTFTexture(mat.emissive_texture.texture, resources, basePath, true);
        desc.emissiveColor = glm::vec4(toVec3(mat.emissive_factor), 1.0f);
        desc.doubleSided = mat.double_sided;

        // MASK becomes the author's cutoff. BLEND has no sorted pass in the
        // renderer, so it is alpha-tested at the glTF default cutoff rather than
        // drawn opaque -- cut-out art (foliage, windows, decals) would otherwise
        // show the black backing of its texture. OPAQUE keeps a cutoff of 0,
        // which disables the test.
        switch (mat.alpha_mode) {
            case cgltf_alpha_mode_mask:
                desc.alphaCutoff = mat.alpha_cutoff > 0.0f ? mat.alpha_cutoff : 0.5f;
                break;
            case cgltf_alpha_mode_blend:
                desc.alphaCutoff = 0.5f;
                break;
            case cgltf_alpha_mode_opaque:
            default:
                desc.alphaCutoff = 0.0f;
                break;
        }
    }

    // 2. Load Meshes (Primitives)
    std::vector<std::vector<AssetID>> meshesPrimitives(data->meshes_count);
    for (size_t i = 0; i < data->meshes_count; ++i) {
        cgltf_mesh& mesh = data->meshes[i];
        for (size_t j = 0; j < mesh.primitives_count; ++j) {
            cgltf_primitive& prim = mesh.primitives[j];
            
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            bool hasTangents = false;
            
            // Vertices
            size_t vertexCount = 0;
            for (size_t k = 0; k < prim.attributes_count; ++k) {
                if (prim.attributes[k].type == cgltf_attribute_type_position) {
                    vertexCount = prim.attributes[k].data->count;
                } else if (prim.attributes[k].type == cgltf_attribute_type_tangent) {
                    hasTangents = true;
                }
            }
            vertices.resize(vertexCount);
            for (Vertex& vertex : vertices) {
                vertex.color = glm::vec3(1.0f);
                vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                vertex.tangent = glm::vec4(0.0f);
            }
            
            for (size_t k = 0; k < prim.attributes_count; ++k) {
                cgltf_attribute& attr = prim.attributes[k];
                for (size_t v = 0; v < vertexCount; ++v) {
                    if (attr.type == cgltf_attribute_type_position) {
                        cgltf_accessor_read_float(attr.data, v, glm::value_ptr(vertices[v].pos), 3);
                    } else if (attr.type == cgltf_attribute_type_normal) {
                        cgltf_accessor_read_float(attr.data, v, glm::value_ptr(vertices[v].normal), 3);
                    } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
                        cgltf_accessor_read_float(attr.data, v, glm::value_ptr(vertices[v].texCoord), 2);
                    } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 1) {
                        cgltf_accessor_read_float(attr.data, v, glm::value_ptr(vertices[v].lightmapUV), 2);
                    } else if (attr.type == cgltf_attribute_type_tangent) {
                        cgltf_accessor_read_float(attr.data, v, glm::value_ptr(vertices[v].tangent), 4);
                    } else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) {
                        uint32_t tmp[4] = {0,0,0,0};
                        cgltf_accessor_read_uint(attr.data, v, tmp, 4);
                        vertices[v].boneIndices = glm::ivec4(tmp[0], tmp[1], tmp[2], tmp[3]);
                    } else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) {
                        cgltf_accessor_read_float(attr.data, v, glm::value_ptr(vertices[v].boneWeights), 4);
                    }
                }
            }
            
            // Indices
            if (prim.indices) {
                if (prim.type == cgltf_primitive_type_triangle_strip) {
                    indices.reserve((prim.indices->count - 2) * 3);
                    for (size_t v = 0; v < prim.indices->count - 2; ++v) {
                        uint32_t i0 = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, v));
                        uint32_t i1 = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, v + 1));
                        uint32_t i2 = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, v + 2));
                        if (i0 == i1 || i1 == i2 || i2 == i0) continue; // skip degenerate triangles
                        if (v % 2 == 0) {
                            indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
                        } else {
                            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
                        }
                    }
                } else if (prim.type == cgltf_primitive_type_triangle_fan) {
                    indices.reserve((prim.indices->count - 2) * 3);
                    uint32_t i0 = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, 0));
                    for (size_t v = 1; v < prim.indices->count - 1; ++v) {
                        uint32_t i1 = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, v));
                        uint32_t i2 = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, v + 1));
                        if (i0 == i1 || i1 == i2 || i2 == i0) continue;
                        indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
                    }
                } else {
                    indices.resize(prim.indices->count);
                    for (size_t v = 0; v < prim.indices->count; ++v) {
                        indices[v] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, v));
                    }
                }
            } else {
                if (prim.type == cgltf_primitive_type_triangle_strip) {
                    indices.reserve((vertexCount - 2) * 3);
                    for (size_t v = 0; v < vertexCount - 2; ++v) {
                        uint32_t i0 = v, i1 = v + 1, i2 = v + 2;
                        if (v % 2 == 0) {
                            indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
                        } else {
                            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
                        }
                    }
                } else {
                    indices.resize(vertexCount);
                    for (size_t v = 0; v < vertexCount; ++v) indices[v] = static_cast<uint32_t>(v);
                }
            }

            if (!hasTangents) {
                for (size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
                    Vertex& v0 = vertices[indices[tri + 0]];
                    Vertex& v1 = vertices[indices[tri + 1]];
                    Vertex& v2 = vertices[indices[tri + 2]];

                    const glm::vec3 e1 = v1.pos - v0.pos;
                    const glm::vec3 e2 = v2.pos - v0.pos;
                    const glm::vec2 duv1 = v1.texCoord - v0.texCoord;
                    const glm::vec2 duv2 = v2.texCoord - v0.texCoord;
                    const float det = duv1.x * duv2.y - duv2.x * duv1.y;
                    if (std::abs(det) < 1e-8f) continue;
                    const glm::vec3 tangent = (e1 * duv2.y - e2 * duv1.y) / det;
                    if (!std::isfinite(tangent.x) || !std::isfinite(tangent.y) ||
                        !std::isfinite(tangent.z))
                        continue;
                    v0.tangent += glm::vec4(tangent, 0.0f);
                    v1.tangent += glm::vec4(tangent, 0.0f);
                    v2.tangent += glm::vec4(tangent, 0.0f);
                }
            }

            for (Vertex& v : vertices) {
                glm::vec3 n = glm::length(v.normal) > 1e-6f
                    ? glm::normalize(v.normal) : glm::vec3(0.0f, 1.0f, 0.0f);
                glm::vec3 t = glm::vec3(v.tangent);
                const float handedness = v.tangent.w < 0.0f ? -1.0f : 1.0f;
                if (glm::length(t) < 1e-6f) {
                    glm::vec3 c1 = glm::cross(n, glm::vec3(0.0f, 0.0f, 1.0f));
                    glm::vec3 c2 = glm::cross(n, glm::vec3(0.0f, 1.0f, 0.0f));
                    t = glm::length(c1) > glm::length(c2) ? c1 : c2;
                }
                t = t - n * glm::dot(n, t);
                if (glm::length(t) < 1e-6f) t = glm::vec3(1.0f, 0.0f, 0.0f);
                v.normal = n;
                v.tangent = glm::vec4(glm::normalize(t), handedness);
            }
            
            // Stable key per sub-asset: a re-import (Play/Stop, scene cycles)
            // yields the same AssetID — a restored snapshot stays resolvable.
            const std::string meshKey =
                loadPath + "#mesh" + std::to_string(i) + "_prim" + std::to_string(j);
            AssetID meshId = resources.registerMemoryMesh(meshKey, vertices, indices);
            Log::info("Loaded GLTF Primitive: ", vertexCount, " vertices, ", indices.size(), " indices, meshId=", meshId, " type=", prim.type);
            meshesPrimitives[i].push_back(meshId);
        }
    }

    // 2b. Scan MSFT_lod metadata before building the scene graph.
    std::unordered_map<size_t, NodeLodInfo> lodByNodeIndex;
    std::unordered_set<size_t> lodProxyNodes;
    for (size_t i = 0; i < data->nodes_count; ++i) {
        cgltf_node* gnode = &data->nodes[i];
        std::vector<size_t> ids = parseMsftLodIds(gnode);
        if (ids.empty()) continue;
        NodeLodInfo info;
        info.lowerDetailNodeIndices = std::move(ids);
        info.msftCoverage = parseMsftScreenCoverage(gnode);
        lodByNodeIndex[i] = std::move(info);
        for (size_t proxy : lodByNodeIndex[i].lowerDetailNodeIndices)
            lodProxyNodes.insert(proxy);
    }

    // 3. Process Scene Graph
    std::vector<std::pair<MeshNode*, cgltf_skin*>> skinnedMeshes;
    if (data->scene) {
        std::filesystem::path p(loadPath);
        Node* containerNode = rootNode.createChild<Node>(p.stem().string());
        containerNode->setImportedFromPath(loadPath);
        for (size_t i = 0; i < data->scene->nodes_count; ++i) {
            processNode(data->scene->nodes[i], containerNode, resources, meshesPrimitives, materials, data,
                        lodProxyNodes, lodByNodeIndex, options.autoMeshLods, skinnedMeshes);
        }
    }

    // 4. Process Skins (Rigs)
    std::vector<AssetID> skinRigs(data->skins_count, kAssetInvalid);
    for (size_t i = 0; i < data->skins_count; ++i) {
        cgltf_skin& skin = data->skins[i];
        std::string rigError;
        auto rig = buildRigFromSkin(skin, &rigError);
        if (!rig) {
            Log::error("GLTFLoader: invalid skin ", i, ": ", rigError);
            continue;
        }

        std::string rigName = loadPath + "#rig" + std::to_string(i);
        skinRigs[i] = resources.registerMemoryRig(rigName, std::move(rig));
        Log::info("Loaded GLTF Skin: ", skin.joints_count, " joints, rigId=", skinRigs[i]);
    }

    // 5. Attach Animators
    for (const auto& [mNode, skin] : skinnedMeshes) {
        size_t skinIdx = skin - data->skins;
        AssetID rigId = skinRigs[skinIdx];
        Rig* rig = resources.getRig(rigId);
        if (rig) {
            mNode->addBehaviour<Animator>()->setRig(rig);
            Log::info("Attached Animator with Rig to MeshNode ", mNode->name());
        }
    }

    // 6. Process Animations
    std::vector<AssetID> loadedClips;
    std::vector<std::string> clipNames;
    for (size_t i = 0; i < data->animations_count; ++i) {
        auto clip = buildClipFromAnimation(data->animations[i], i);
        const std::string animName = clip->name();
        const float duration = clip->duration();

        std::string clipPath = loadPath + "#" + animName;
        AssetID clipId = resources.registerMemoryAnimation(clipPath, std::move(clip));
        loadedClips.push_back(clipId);
        clipNames.push_back(animName);
        Log::info("Loaded GLTF Animation: ", animName, " duration=", duration, " id=", clipId);
    }

    // 7. Register every clip on each skinned mesh's Animator and start the first
    //    one looping, so a character glb animates immediately. Switch at runtime
    //    from a behaviour with animator->play("Idle"/"Walk"/...).
    if (!loadedClips.empty() && !skinnedMeshes.empty()) {
        for (const auto& pair : skinnedMeshes) {
            Animator* anim = pair.first->getBehaviour<Animator>();
            if (!anim || !anim->rig()) continue;
            for (size_t k = 0; k < loadedClips.size(); ++k) {
                if (const AnimationClip* c = resources.getAnimation(loadedClips[k]))
                    anim->addClip(clipNames[k], c);
            }
            anim->play(clipNames[0]);
        }
    }

    cgltf_free(data);
    return true;
}

} // namespace saida
