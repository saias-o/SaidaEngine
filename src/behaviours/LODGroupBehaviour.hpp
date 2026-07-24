#pragma once

#include "core/ReflectionFwd.hpp"
#include "scene/Behaviour.hpp"

namespace saida {

// Marker/editor component for MeshNode LOD chains. The actual runtime data lives
// on MeshNode so the renderer can stay simple and fast; this behaviour makes the
// feature visible, serializable and editable like any other component.
class LODGroupBehaviour : public Behaviour {
public:
    const char* typeName() const override { return "LOD Group"; }

    // Reflection: marker descriptor without properties — the durable LOD data
    // is serialized by MeshNode. Registration flows through
    // registerReflectedTypes() like every other built-in behaviour.
    static constexpr const char* reflectName() { return "LOD Group"; }
    static void describe(reflect::TypeBuilder<LODGroupBehaviour>& t);
};

} // namespace saida
