#pragma once

#include "scene/RuntimeTypeMatrix.hpp"

#include <cstddef>
#include <string>

namespace saida {

class ResourceManager;

namespace runtime {

struct RoundTripContractReport {
    std::size_t nodes = 0;
    std::size_t behaviours = 0;
    std::size_t reflectedProperties = 0;
};

// Builds an in-memory scene from every factory promised by target, seeds common
// and reflected data with non-default values, then requires a semantic
// serialize -> load -> serialize identity through the full SceneSerializer.
bool verifyRuntimeRoundTripContract(RuntimeTypeTarget target,
                                    ResourceManager& resources,
                                    RoundTripContractReport& report,
                                    std::string& error);

// Equivalent corpus through the resource-free SceneSnapshot codec. Used by
// headless/authoring targets that do not link the full SceneSerializer; seeds
// only common and reflected data, so it needs no ResourceManager.
bool verifySnapshotRoundTripContract(RuntimeTypeTarget target,
                                     RoundTripContractReport& report,
                                     std::string& error);

} // namespace runtime
} // namespace saida
