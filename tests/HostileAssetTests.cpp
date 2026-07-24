// P0.5: hostile content -- a corrupt or malicious OBJ/glTF/GLB is refused by
// the real loaders with a diagnostic, never killing the process (on the wasm
// player, an out-of-bounds read aborts: cgltf_validate closes this hole).
#include "graphics/Mesh.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Rig.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace saida;

namespace {

std::filesystem::path gDir;

std::string writeFile(const char* name, const std::string& bytes) {
    const std::filesystem::path p = gDir / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return p.string();
}

int gChecks = 0;
void require(bool ok, const char* what) {
    ++gChecks;
    if (!ok) {
        std::printf("[hostile-assets] FAIL: %s\n", what);
        std::abort();
    }
}

// A GLB with a valid header but a JSON chunk truncated midway.
std::string truncatedGlb() {
    std::string glb = "glTF";
    const uint32_t version = 2, length = 4096, chunkLen = 2048, chunkType = 0x4E4F534A;
    glb.append(reinterpret_cast<const char*>(&version), 4);
    glb.append(reinterpret_cast<const char*>(&length), 4);
    glb.append(reinterpret_cast<const char*>(&chunkLen), 4);
    glb.append(reinterpret_cast<const char*>(&chunkType), 4);
    glb += "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"primi";  // truncated
    return glb;
}

// A glTF that parses but whose accessor points outside its buffer (an OOB
// read if consumed without validation) -- the typical malicious case.
std::string outOfBoundsAccessorGltf() {
    return R"({
  "asset": {"version": "2.0"},
  "buffers": [{"byteLength": 16, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAA=="}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 16}],
  "accessors": [{"bufferView": 0, "byteOffset": 8, "componentType": 5126,
                 "count": 4096, "type": "VEC3"}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})";
}

} // namespace

int main() {
    gDir = std::filesystem::temp_directory_path() / "saida_hostile_assets";
    std::filesystem::create_directories(gDir);

    // --- OBJ ---
    {
        MeshData out;
        std::string error;
        // Pure binary garbage.
        std::string garbage(4096, '\0');
        for (size_t i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<char>(i * 37);
        require(!Mesh::parseObjBytes(reinterpret_cast<const uint8_t*>(garbage.data()),
                                     garbage.size(), out, error) ||
                    out.vertices.empty(),
                "garbage OBJ yields no usable mesh");

        // Out-of-range face indices: refused or ignored, never a crash.
        const std::string evil = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 999999\nf -5 2 3\n";
        MeshData out2;
        std::string error2;
        const bool ok = Mesh::parseObjBytes(
            reinterpret_cast<const uint8_t*>(evil.data()), evil.size(), out2, error2);
        if (ok) {
            for (uint32_t idx : out2.indices)
                require(idx < out2.vertices.size(), "OBJ indices stay in range");
        }
        ++gChecks;
    }

    // --- glTF/GLB via the real GLTFLoader (animation/rig path, pure CPU) ---
    {
        GltfAnimationData anim;
        std::string error;

        const std::string glbPath = writeFile("truncated.glb", truncatedGlb());
        require(!GLTFLoader::loadAnimationData(glbPath, anim, &error),
                "truncated GLB refused");

        const std::string gltfPath = writeFile("oob.gltf", outOfBoundsAccessorGltf());
        require(!GLTFLoader::loadAnimationData(gltfPath, anim, &error),
                "out-of-bounds accessor glTF refused by validation");

        const std::string junkPath = writeFile("junk.gltf", "\x7f\x45\x4c\x46 not a gltf at all");
        require(!GLTFLoader::loadAnimationData(junkPath, anim, &error),
                "binary junk refused");

        const std::string missingPath = (gDir / "missing.gltf").string();
        require(!GLTFLoader::loadAnimationData(missingPath, anim, &error),
                "missing file refused");
    }

    std::filesystem::remove_all(gDir);
    std::printf("[hostile-assets] PASS (%d checks)\n", gChecks);
    return 0;
}
