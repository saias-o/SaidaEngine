#include "behaviours/LODGroupBehaviour.hpp"

#include "core/Reflection.hpp"

namespace saida {

void LODGroupBehaviour::describe(reflect::TypeBuilder<LODGroupBehaviour>& t) {
    t.doc("Marker component that surfaces a MeshNode's LOD chain in the editor "
          "and authoring surfaces. Carries no properties of its own: the "
          "durable LOD data lives on the MeshNode.");
}

} // namespace saida
