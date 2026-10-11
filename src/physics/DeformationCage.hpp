#pragma once

#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <cstdint>

namespace saida {

struct DeformationSettings {
    uint32_t resolution = 5;
    float stiffness = 80000.f; // J/m², controls energy -> displacement
    float yieldEnergy = 150.f;
    float radius = .8f;
    float maximumDisplacement = .45f;
    float elasticFraction = .15f;
    float relaxationRate = 12.f;
    float maximumGradient = .45f; // keeps the local mapping orientation-preserving
};

struct CageBinding {
    std::array<uint16_t, 8> indices{};
    glm::vec3 fraction{0.f};
};
struct CageSample {
    glm::vec3 displacement{0.f};
    glm::mat3 derivative{1.f};
    bool active = false;
};

// Small local-space displacement lattice. Rest geometry and UVs stay immutable.
// Bindings stay immutable during a cage's lifetime; only offsets are mutable.
class DeformationCage {
public:
    DeformationCage(glm::vec3 minimum, glm::vec3 maximum, DeformationSettings settings = {});
    CageBinding bind(glm::vec3 position) const;
    CageSample sample(const CageBinding& binding) const;
    glm::vec3 offset(const CageBinding& binding) const;
    glm::mat3 jacobian(const CageBinding& binding) const;
    bool impact(glm::vec3 position, glm::vec3 direction, float energy);
    bool relax(float dt);
    void reset();
    bool active() const;
    uint64_t revision() const { return revision_; }
    const DeformationSettings& settings() const { return settings_; }
    const std::vector<glm::vec3>& permanentOffsets() const { return permanent_; }
    bool restore(const std::vector<glm::vec3>& offsets);
private:
    size_t index(uint32_t x, uint32_t y, uint32_t z) const;
    glm::vec3 point(size_t index) const;
    void limitGradients(std::vector<glm::vec3>& offsets) const;
    void updateCells();
    struct Cell { std::array<glm::vec3,8> coefficients{}; bool active=false; };
    DeformationSettings settings_;
    glm::vec3 minimum_, spacing_;
    std::vector<glm::vec3> permanent_, elastic_;
    std::vector<Cell> cells_;
    uint64_t revision_ = 0;
};

} // namespace saida
