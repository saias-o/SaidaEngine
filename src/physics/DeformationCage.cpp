#include "physics/DeformationCage.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saida {
namespace {
constexpr float kMinimumExtent = .01f;
constexpr float kSleepOffset = .0001f;
constexpr uint32_t kMinimumResolution = 2, kMaximumResolution = 8;
constexpr uint32_t kGradientPasses = 4;
bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
float smooth(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }
}

DeformationCage::DeformationCage(glm::vec3 minimum, glm::vec3 maximum, DeformationSettings settings)
    : settings_(settings), minimum_(minimum) {
    if (!finite(minimum) || !finite(maximum) || glm::any(glm::lessThan(maximum, minimum)) ||
        settings.resolution < kMinimumResolution || settings.resolution > kMaximumResolution ||
        !std::isfinite(settings.stiffness) || settings.stiffness <= 0.f ||
        !std::isfinite(settings.radius) || settings.radius <= 0.f ||
        !std::isfinite(settings.maximumDisplacement) || settings.maximumDisplacement <= 0.f ||
        !std::isfinite(settings.yieldEnergy) || settings.yieldEnergy < 0.f ||
        !std::isfinite(settings.elasticFraction) || settings.elasticFraction < 0.f || settings.elasticFraction > 1.f ||
        !std::isfinite(settings.relaxationRate) || settings.relaxationRate <= 0.f ||
        !std::isfinite(settings.maximumGradient) || settings.maximumGradient <= 0.f || settings.maximumGradient >= .5f)
        throw std::invalid_argument("invalid deformation cage bounds or settings");
    spacing_ = glm::max(maximum - minimum, glm::vec3(kMinimumExtent)) / float(settings.resolution - 1);
    const size_t count = size_t(settings.resolution) * settings.resolution * settings.resolution;
    permanent_.resize(count, glm::vec3(0.f));
    elastic_.resize(count, glm::vec3(0.f));
    cells_.resize(count);
}

size_t DeformationCage::index(uint32_t x, uint32_t y, uint32_t z) const {
    return (size_t(z) * settings_.resolution + y) * settings_.resolution + x;
}
glm::vec3 DeformationCage::point(size_t i) const {
    const auto n = settings_.resolution;
    return minimum_ + spacing_ * glm::vec3(float(i % n), float(i / n % n), float(i / (n * n)));
}
CageBinding DeformationCage::bind(glm::vec3 position) const {
    if (!finite(position)) throw std::invalid_argument("nonfinite deformation binding");
    const uint32_t n = settings_.resolution;
    const glm::vec3 cell = glm::clamp((position - minimum_) / spacing_, glm::vec3(0), glm::vec3(float(n - 1)));
    const glm::uvec3 base = glm::min(glm::uvec3(cell), glm::uvec3(n - 2));
    CageBinding binding;
    binding.fraction = cell - glm::vec3(base);
    for (uint32_t corner = 0; corner < binding.indices.size(); ++corner)
        binding.indices[corner] = uint16_t(index(base.x + (corner & 1), base.y + ((corner >> 1) & 1), base.z + ((corner >> 2) & 1)));
    return binding;
}
void DeformationCage::updateCells() {
    const auto n=settings_.resolution;
    for(uint32_t z=0;z+1<n;++z)for(uint32_t y=0;y+1<n;++y)for(uint32_t x=0;x+1<n;++x) {
        std::array<glm::vec3,8> p;
        for(uint32_t corner=0;corner<p.size();++corner) {
            const auto i=index(x+(corner&1),y+((corner>>1)&1),z+((corner>>2)&1));
            p[corner]=permanent_[i]+elastic_[i];
        }
        auto& cell=cells_[index(x,y,z)];auto& c=cell.coefficients;
        cell.active=std::any_of(p.begin(),p.end(),[](glm::vec3 value){return glm::dot(value,value)>0.f;});
        c[0]=p[0];c[1]=p[1]-p[0];c[2]=p[2]-p[0];c[4]=p[4]-p[0];
        c[3]=p[3]-p[1]-p[2]+p[0];c[5]=p[5]-p[1]-p[4]+p[0];c[6]=p[6]-p[2]-p[4]+p[0];
        c[7]=p[7]-p[3]-p[5]-p[6]+p[1]+p[2]+p[4]-p[0];
    }
}
CageSample DeformationCage::sample(const CageBinding& b) const {
    // Cache each cell's trilinear polynomial when controls change. A detailed
    // vertex now reads one cell, instead of gathering the same eight controls
    // twice to calculate displacement and its analytic derivative.
    const auto& cell=cells_.at(b.indices[0]);
    if(!cell.active)return {};
    const auto& c=cell.coefficients;
    const float x=b.fraction.x,y=b.fraction.y,z=b.fraction.z;
    CageSample result;
    result.active=true;
    result.displacement=c[0]+c[1]*x+c[2]*y+c[4]*z+c[3]*(x*y)+c[5]*(x*z)+c[6]*(y*z)+c[7]*(x*y*z);
    result.derivative[0]+=(c[1]+c[3]*y+c[5]*z+c[7]*(y*z))/spacing_.x;
    result.derivative[1]+=(c[2]+c[3]*x+c[6]*z+c[7]*(x*z))/spacing_.y;
    result.derivative[2]+=(c[4]+c[5]*x+c[6]*y+c[7]*(x*y))/spacing_.z;
    return result;
}
glm::vec3 DeformationCage::offset(const CageBinding& b) const { return sample(b).displacement; }
glm::mat3 DeformationCage::jacobian(const CageBinding& b) const { return sample(b).derivative; }
void DeformationCage::limitGradients(std::vector<glm::vec3>& offsets) const {
    const auto n = settings_.resolution;
    for (uint32_t pass = 0; pass < kGradientPasses; ++pass)
        for (uint32_t z = 0; z < n; ++z) for (uint32_t y = 0; y < n; ++y) for (uint32_t x = 0; x < n; ++x) {
            const auto a = index(x, y, z);
            const std::array<bool, 3> valid{x + 1 < n, y + 1 < n, z + 1 < n};
            const std::array<size_t, 3> neighbours{a + 1, a + n, a + n * n};
            for (size_t axis = 0; axis < valid.size(); ++axis) {
                if (!valid[axis]) continue;
                const auto b = neighbours[axis];
                const glm::vec3 delta = offsets[b] - offsets[a];
                const float distance = glm::length(delta);
                const float limit = spacing_[axis] * settings_.maximumGradient / std::sqrt(3.f);
                if (distance <= limit) continue;
                const glm::vec3 correction = delta * (.5f * (1.f - limit / distance));
                offsets[a] += correction;
                offsets[b] -= correction;
            }
        }
    float scale = 1.f;
    for (uint32_t z = 0; z < n; ++z) for (uint32_t y = 0; y < n; ++y) for (uint32_t x = 0; x < n; ++x) {
        const auto a = index(x, y, z);
        const std::array<bool, 3> valid{x + 1 < n, y + 1 < n, z + 1 < n};
        const std::array<size_t, 3> neighbours{a + 1, a + n, a + n * n};
        for (size_t axis = 0; axis < valid.size(); ++axis) if (valid[axis]) {
            const float gradient = glm::length(offsets[neighbours[axis]] - offsets[a]) / spacing_[axis];
            if (gradient > 0.f) scale = std::min(scale, settings_.maximumGradient / (2.f * std::sqrt(3.f) * gradient));
        }
    }
    // Bound the sum of permanent and elastic Jacobians, including between samples.
    if (scale < 1.f) for (auto& value : offsets) value *= scale;
}
bool DeformationCage::impact(glm::vec3 position, glm::vec3 direction, float energy) {
    if (!finite(position) || !finite(direction) || !std::isfinite(energy) || energy <= settings_.yieldEnergy) return false;
    const float magnitude = glm::length(direction);
    if (magnitude < kSleepOffset) return false;
    direction /= magnitude;
    const float depth = std::min(settings_.maximumDisplacement, std::sqrt(2.f * (energy - settings_.yieldEnergy) / settings_.stiffness));
    bool changed = false;
    for (size_t i = 0; i < permanent_.size(); ++i) {
        const glm::vec3 support = glm::max(glm::vec3(settings_.radius), spacing_ * 1.5f);
        const float weight = smooth(1.f - glm::length((point(i) - position) / support));
        if (weight == 0.f) continue;
        glm::vec3 delta = direction * (depth * weight);
        const glm::vec3 proposed = permanent_[i] + delta * (1.f - settings_.elasticFraction);
        const float length = glm::length(proposed);
        permanent_[i] = length > settings_.maximumDisplacement ? proposed * (settings_.maximumDisplacement / length) : proposed;
        elastic_[i] = delta * settings_.elasticFraction;
        changed = true;
    }
    if (changed) { limitGradients(permanent_); limitGradients(elastic_); updateCells(); ++revision_; }
    return changed;
}
bool DeformationCage::relax(float dt) {
    if (!std::isfinite(dt) || dt <= 0.f || !active()) return false;
    const float decay = std::exp(-settings_.relaxationRate * dt);
    for (auto& value : elastic_) {
        value *= decay;
        if (glm::dot(value, value) < kSleepOffset * kSleepOffset) value = glm::vec3(0.f);
    }
    ++revision_;
    updateCells();
    return true;
}
bool DeformationCage::active() const {
    for (const auto value : elastic_) if (glm::dot(value, value) > 0.f) return true;
    return false;
}
void DeformationCage::reset() {
    std::fill(permanent_.begin(), permanent_.end(), glm::vec3(0.f));
    std::fill(elastic_.begin(), elastic_.end(), glm::vec3(0.f));
    updateCells();
    ++revision_;
}
bool DeformationCage::restore(const std::vector<glm::vec3>& offsets) {
    if (offsets.size() != permanent_.size()) return false;
    for (auto value : offsets)
        if (!finite(value) || glm::length(value) > settings_.maximumDisplacement + kSleepOffset) return false;
    permanent_ = offsets;
    limitGradients(permanent_);
    std::fill(elastic_.begin(), elastic_.end(), glm::vec3(0.f));
    updateCells();
    ++revision_;
    return true;
}

} // namespace saida
