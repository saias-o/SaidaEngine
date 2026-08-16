#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace saida {

struct Aabb;
class Node;
class Scene;

// Measures the world-space size of what a scene actually draws.
//
// A node's transform states where it was put; it never states how big the mesh
// hanging off it is. Every dimension a game is authored against — a car against
// its wheels, a door against the character walking through it — is the mesh's
// own extent scaled by the transform, and nothing in the engine exposed that
// pair together. The functions here compute it from the geometry the renderer
// draws, so a wrong proportion is a number rather than something to be looked at.
namespace measure {

// A world-space axis-aligned box plus what it was built from. `valid` is false
// when the subtree draws nothing at all: an empty box at the origin and an
// absent box must never be confused, because "size 0" reads as a real answer.
struct WorldBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool valid = false;
    int meshCount = 0;
    uint64_t triangles = 0;

    glm::vec3 center() const { return valid ? (min + max) * 0.5f : glm::vec3(0.0f); }
    glm::vec3 size() const { return valid ? max - min : glm::vec3(0.0f); }
    // Radius of the sphere that contains the box, for framing a camera on it.
    float radius() const { return valid ? glm::length(size()) * 0.5f : 0.0f; }

    void expand(const WorldBounds& other);
};

// World AABB of a local box under a transform. Rotation is honoured by
// transforming the eight corners: taking min/max of the transformed extremes
// alone collapses any rotated box onto its own diagonal.
WorldBounds transformedBounds(const Aabb& local, const glm::mat4& world);

// The node's own mesh only, in world space. Invalid when it draws nothing.
WorldBounds measureOwnMesh(const Node& node);

// Every drawable mesh in the subtree, in world space. Disabled nodes are
// skipped with their whole subtree: the measurement answers what is on screen,
// which is the question that was asked.
//
// Reads the cached world transforms, so it is only meaningful after the scene
// has updated them (Scene::update). A skinned mesh reports its bind pose.
WorldBounds measureSubtree(const Node& root);

// Slash-separated path from the scene root, for naming a node in a report.
std::string nodePath(const Node& node);

// What to include in a scene report.
struct ReportOptions {
    // Empty = the whole scene. Otherwise the name, path or group of the subtree
    // to describe, so a 2000-node city can be asked about one car.
    std::string root;
    // Nodes deeper than this below the reported root are summarised by their
    // parent's subtree bounds rather than listed. 0 = no limit.
    int maxDepth = 0;
};

// A machine-readable description of the scene's measurable geometry: per node
// its world transform, its own mesh extent, its subtree bounds and triangles.
// Returns a JSON object; `error` is set and the object is empty when the
// requested root does not resolve.
nlohmann::json describeScene(Scene& scene, const ReportOptions& options, std::string& error);

}  // namespace measure
}  // namespace saida
