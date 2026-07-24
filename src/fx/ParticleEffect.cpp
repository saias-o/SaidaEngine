#include "fx/ParticleEffect.hpp"

#include "fx/ParticlePresetLibrary.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace saida {
namespace {

using json = nlohmann::json;

json vec3Json(const glm::vec3& v) {
    return json::array({v.x, v.y, v.z});
}

json vec4Json(const glm::vec4& v) {
    return json::array({v.x, v.y, v.z, v.w});
}

glm::vec3 vec3FromJson(const json& j, const glm::vec3& fallback) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

glm::vec4 vec4FromJson(const json& j, const glm::vec4& fallback) {
    if (!j.is_array() || j.size() < 4) return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
}

const char* shapeToString(ParticleSystemNode::Shape shape) {
    using Shape = ParticleSystemNode::Shape;
    switch (shape) {
        case Shape::Point: return "Point";
        case Shape::Disc: return "Disc";
        case Shape::Box: return "Box";
        case Shape::Cone: return "Cone";
        case Shape::Ring: return "Ring";
        case Shape::Sphere:
        default: return "Sphere";
    }
}

ParticleSystemNode::Shape shapeFromString(const std::string& text) {
    using Shape = ParticleSystemNode::Shape;
    if (text == "Point") return Shape::Point;
    if (text == "Disc") return Shape::Disc;
    if (text == "Box") return Shape::Box;
    if (text == "Cone") return Shape::Cone;
    if (text == "Ring") return Shape::Ring;
    return Shape::Sphere;
}

ParticleModule module(ParticleModuleType type, json params) {
    ParticleModule m;
    m.type = type;
    m.params = std::move(params);
    return m;
}

std::string presetName(ParticleSystemNode::EffectClass effectClass) {
    return std::string(toString(effectClass));
}

ParticleEmitterDesc emitterFromPreset(const ParticlePreset& preset) {
    ParticleEmitterDesc e;
    e.name = presetName(preset.effectClass);
    e.effectClass = preset.effectClass;
    e.maxParticles = preset.maxParticles;
    e.blendMode = preset.blendMode;
    e.looping = preset.looping;

    if (preset.spawnRate > 0.0f) {
        e.modules.push_back(module(ParticleModuleType::SpawnRate, {
            {"rate", preset.spawnRate}
        }));
    }

    if (preset.effectClass == ParticleSystemNode::EffectClass::Explosion) {
        e.modules.push_back(module(ParticleModuleType::Burst, {
            {"count", preset.burstCount > 0 ? preset.burstCount : std::max(1, preset.maxParticles / 3)}
        }));
    }

    e.modules.push_back(module(ParticleModuleType::Shape, {
        {"type", shapeToString(preset.shape)},
        {"radius", preset.radius},
        {"boxExtents", vec3Json(preset.boxExtents)},
        {"coneAngle", preset.coneAngle},
        {"ringThickness", preset.ringThickness}
    }));
    e.modules.push_back(module(ParticleModuleType::InitialVelocity, {
        {"speed", preset.startSpeed}
    }));
    e.modules.push_back(module(ParticleModuleType::Gravity, {
        {"acceleration", vec3Json(preset.gravity)}
    }));
    e.modules.push_back(module(ParticleModuleType::ColorOverLife, {
        {"start", vec4Json(preset.startColor)},
        {"end", vec4Json(preset.endColor)},
        {"emissive", preset.emissive}
    }));
    e.modules.push_back(module(ParticleModuleType::SizeOverLife, {
        {"start", preset.startSize},
        {"endScale", preset.endSizeScale},
        {"stretch", preset.stretch}
    }));

    if (preset.drag > 0.0f) {
        e.modules.push_back(module(ParticleModuleType::Drag, {
            {"linear", preset.drag}
        }));
    }
    if (preset.noiseStrength > 0.0f) {
        e.modules.push_back(module(ParticleModuleType::Noise, {
            {"strength", preset.noiseStrength},
            {"frequency", preset.noiseFrequency}
        }));
    }
    if (preset.attractorStrength != 0.0f) {
        e.modules.push_back(module(ParticleModuleType::Attractor, {
            {"position", vec3Json(preset.attractorPosition)},
            {"strength", preset.attractorStrength}
        }));
    }

    return e;
}

json moduleToJson(const ParticleModule& m) {
    return {
        {"type", toString(m.type)},
        {"enabled", m.enabled},
        {"params", m.params.is_null() ? json::object() : m.params}
    };
}

ParticleModule moduleFromJson(const json& j) {
    ParticleModule m;
    m.type = particleModuleTypeFromString(j.value("type", "SpawnRate"));
    m.enabled = j.value("enabled", true);
    m.params = j.value("params", json::object());
    return m;
}

json emitterToJson(const ParticleEmitterDesc& e) {
    json modules = json::array();
    for (const ParticleModule& m : e.modules) modules.push_back(moduleToJson(m));
    return {
        {"name", e.name},
        {"effectClass", toString(e.effectClass)},
        {"maxParticles", e.maxParticles},
        {"blendMode", toString(e.blendMode)},
        {"looping", e.looping},
        {"modules", std::move(modules)}
    };
}

ParticleEmitterDesc emitterFromJson(const json& j) {
    ParticleEmitterDesc e;
    e.name = j.value("name", "Emitter");
    e.effectClass = particleEffectClassFromString(j.value("effectClass", "Simple"));
    e.maxParticles = j.value("maxParticles", 256);
    e.blendMode = particleBlendModeFromString(j.value("blendMode", "Additive"));
    e.looping = j.value("looping", true);
    if (auto it = j.find("modules"); it != j.end() && it->is_array()) {
        for (const json& m : *it) e.modules.push_back(moduleFromJson(m));
    }
    return e;
}

} // namespace

const char* toString(ParticleModuleType type) {
    switch (type) {
        case ParticleModuleType::SpawnRate: return "SpawnRate";
        case ParticleModuleType::Burst: return "Burst";
        case ParticleModuleType::Shape: return "Shape";
        case ParticleModuleType::InitialVelocity: return "InitialVelocity";
        case ParticleModuleType::Gravity: return "Gravity";
        case ParticleModuleType::Drag: return "Drag";
        case ParticleModuleType::Noise: return "Noise";
        case ParticleModuleType::ColorOverLife: return "ColorOverLife";
        case ParticleModuleType::SizeOverLife: return "SizeOverLife";
        case ParticleModuleType::Attractor: return "Attractor";
        case ParticleModuleType::SubEmitter: return "SubEmitter";
        default: return "SpawnRate";
    }
}

ParticleModuleType particleModuleTypeFromString(const std::string& text) {
    if (text == "Burst") return ParticleModuleType::Burst;
    if (text == "Shape") return ParticleModuleType::Shape;
    if (text == "InitialVelocity") return ParticleModuleType::InitialVelocity;
    if (text == "Gravity") return ParticleModuleType::Gravity;
    if (text == "Drag") return ParticleModuleType::Drag;
    if (text == "Noise") return ParticleModuleType::Noise;
    if (text == "ColorOverLife") return ParticleModuleType::ColorOverLife;
    if (text == "SizeOverLife") return ParticleModuleType::SizeOverLife;
    if (text == "Attractor") return ParticleModuleType::Attractor;
    if (text == "SubEmitter") return ParticleModuleType::SubEmitter;
    return ParticleModuleType::SpawnRate;
}

const char* toString(ParticleSystemNode::EffectClass effectClass) {
    using Effect = ParticleSystemNode::EffectClass;
    switch (effectClass) {
        case Effect::Fire: return "Fire";
        case Effect::Magic: return "Magic";
        case Effect::Rain: return "Rain";
        case Effect::Snow: return "Snow";
        case Effect::Smoke: return "Smoke";
        case Effect::Explosion: return "Explosion";
        case Effect::Simple:
        default: return "Simple";
    }
}

ParticleSystemNode::EffectClass particleEffectClassFromString(const std::string& text) {
    using Effect = ParticleSystemNode::EffectClass;
    if (text == "Fire") return Effect::Fire;
    if (text == "Magic") return Effect::Magic;
    if (text == "Rain") return Effect::Rain;
    if (text == "Snow") return Effect::Snow;
    if (text == "Smoke") return Effect::Smoke;
    if (text == "Explosion") return Effect::Explosion;
    return Effect::Simple;
}

const char* toString(ParticleSystemNode::BlendMode blendMode) {
    return blendMode == ParticleSystemNode::BlendMode::Alpha ? "Alpha" : "Additive";
}

ParticleSystemNode::BlendMode particleBlendModeFromString(const std::string& text) {
    if (text == "Alpha") return ParticleSystemNode::BlendMode::Alpha;
    return ParticleSystemNode::BlendMode::Additive;
}

ParticleEffect ParticleEffect::fromPreset(ParticleSystemNode::EffectClass effectClass) {
    ParticleEffect effect;
    effect.name = presetName(effectClass);
    effect.emitters.push_back(emitterFromPreset(ParticlePresetLibrary::presetFor(effectClass)));
    return effect;
}

ParticleEffect ParticleEffect::fromJson(const json& j) {
    ParticleEffect effect;
    effect.name = j.value("name", "Effect");
    effect.version = j.value("version", kCurrentVersion);
    if (auto it = j.find("emitters"); it != j.end() && it->is_array()) {
        for (const json& e : *it) effect.emitters.push_back(emitterFromJson(e));
    }
    return effect;
}

ParticleEffect ParticleEffect::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) throw std::runtime_error("failed to open particle effect: " + path);
    json j;
    file >> j;
    return fromJson(j);
}

json ParticleEffect::toJson() const {
    json emitterArray = json::array();
    for (const ParticleEmitterDesc& e : emitters) emitterArray.push_back(emitterToJson(e));
    return {
        {"format", "SaidaFX"},
        {"version", version},
        {"name", name},
        {"emitters", std::move(emitterArray)}
    };
}

bool ParticleEffect::saveToFile(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << toJson().dump(4);
    return true;
}

bool ParticleEffect::applyTo(ParticleSystemNode& node, size_t emitterIndex) const {
    if (emitterIndex >= emitters.size()) return false;

    const ParticleEmitterDesc& emitter = emitters[emitterIndex];
    ParticlePresetLibrary::apply(node, emitter.effectClass);
    node.effectClass = emitter.effectClass;
    node.maxParticles = std::max(1, emitter.maxParticles);
    node.blendMode = emitter.blendMode;
    node.looping = emitter.looping;

    for (const ParticleModule& module : emitter.modules) {
        if (!module.enabled || !module.params.is_object()) continue;
        const json& p = module.params;
        switch (module.type) {
            case ParticleModuleType::SpawnRate:
                node.spawnRate = p.value("rate", node.spawnRate);
                break;
            case ParticleModuleType::Burst:
                node.burstCount = p.value("count", node.burstCount);
                node.spawnRate = p.value("spawnRate", node.spawnRate);
                node.looping = p.value("looping", node.looping);
                break;
            case ParticleModuleType::Shape:
                node.shape = shapeFromString(p.value("type", std::string("Sphere")));
                node.radius = p.value("radius", node.radius);
                if (auto it = p.find("boxExtents"); it != p.end()) {
                    node.boxExtents = vec3FromJson(*it, node.boxExtents);
                }
                node.coneAngle = p.value("coneAngle", node.coneAngle);
                node.ringThickness = p.value("ringThickness", node.ringThickness);
                break;
            case ParticleModuleType::InitialVelocity:
                node.startSpeed = p.value("speed", node.startSpeed);
                break;
            case ParticleModuleType::Gravity:
                if (auto it = p.find("acceleration"); it != p.end()) {
                    node.gravity = vec3FromJson(*it, node.gravity);
                }
                break;
            case ParticleModuleType::ColorOverLife:
                if (auto it = p.find("start"); it != p.end()) {
                    node.startColor = vec4FromJson(*it, node.startColor);
                }
                if (auto it = p.find("end"); it != p.end()) {
                    node.endColor = vec4FromJson(*it, node.endColor);
                }
                node.emissive = p.value("emissive", node.emissive);
                break;
            case ParticleModuleType::SizeOverLife:
                node.startSize = p.value("start", node.startSize);
                node.endSizeScale = p.value("endScale", node.endSizeScale);
                node.stretch = p.value("stretch", node.stretch);
                break;
            case ParticleModuleType::Drag:
                node.drag = p.value("linear", node.drag);
                break;
            case ParticleModuleType::Noise:
                node.noiseStrength = p.value("strength", node.noiseStrength);
                node.noiseFrequency = p.value("frequency", node.noiseFrequency);
                break;
            case ParticleModuleType::Attractor:
                if (auto it = p.find("position"); it != p.end()) {
                    node.attractorPosition = vec3FromJson(*it, node.attractorPosition);
                }
                node.attractorStrength = p.value("strength", node.attractorStrength);
                break;
            case ParticleModuleType::SubEmitter:
                break;
        }
    }
    return true;
}

std::vector<std::string> ParticleEffect::validate() const {
    std::vector<std::string> errors;
    if (version <= 0 || version > kCurrentVersion) {
        errors.push_back("unsupported SaidaFX version");
    }
    if (emitters.empty()) {
        errors.push_back("effect has no emitters");
    }

    for (size_t i = 0; i < emitters.size(); ++i) {
        const ParticleEmitterDesc& e = emitters[i];
        const std::string prefix = "emitter[" + std::to_string(i) + "] ";
        if (e.maxParticles <= 0) {
            errors.push_back(prefix + "maxParticles must be > 0");
        }
        if (e.modules.empty()) {
            errors.push_back(prefix + "has no modules");
        }
        for (size_t mIndex = 0; mIndex < e.modules.size(); ++mIndex) {
            const ParticleModule& m = e.modules[mIndex];
            if (!m.enabled) continue;
            const std::string mp = prefix + "module[" + std::to_string(mIndex) + "] ";
            if (!m.params.is_object()) {
                errors.push_back(mp + "params must be an object");
                continue;
            }
            switch (m.type) {
                case ParticleModuleType::SpawnRate:
                    if (!m.params.contains("rate")) errors.push_back(mp + "SpawnRate requires rate");
                    break;
                case ParticleModuleType::Burst:
                    if (!m.params.contains("count")) errors.push_back(mp + "Burst requires count");
                    break;
                case ParticleModuleType::Shape:
                    if (!m.params.contains("type")) errors.push_back(mp + "Shape requires type");
                    if (!m.params.contains("radius")) errors.push_back(mp + "Shape requires radius");
                    break;
                case ParticleModuleType::InitialVelocity:
                    if (!m.params.contains("speed")) errors.push_back(mp + "InitialVelocity requires speed");
                    break;
                case ParticleModuleType::Gravity:
                    if (!m.params.contains("acceleration")) errors.push_back(mp + "Gravity requires acceleration");
                    break;
                case ParticleModuleType::ColorOverLife:
                    if (!m.params.contains("start")) errors.push_back(mp + "ColorOverLife requires start");
                    if (!m.params.contains("end")) errors.push_back(mp + "ColorOverLife requires end");
                    break;
                case ParticleModuleType::SizeOverLife:
                    if (!m.params.contains("start")) errors.push_back(mp + "SizeOverLife requires start");
                    break;
                case ParticleModuleType::Drag:
                    if (!m.params.contains("linear")) errors.push_back(mp + "Drag requires linear");
                    break;
                case ParticleModuleType::Noise:
                    if (!m.params.contains("strength")) errors.push_back(mp + "Noise requires strength");
                    if (!m.params.contains("frequency")) errors.push_back(mp + "Noise requires frequency");
                    break;
                case ParticleModuleType::Attractor:
                    if (!m.params.contains("position")) errors.push_back(mp + "Attractor requires position");
                    if (!m.params.contains("strength")) errors.push_back(mp + "Attractor requires strength");
                    break;
                case ParticleModuleType::SubEmitter:
                    if (!m.params.contains("effect")) errors.push_back(mp + "SubEmitter requires effect");
                    break;
            }
        }
    }
    return errors;
}

} // namespace saida
