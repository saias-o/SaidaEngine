#pragma once

namespace saida::xr {

// Registers all XR Toolkit node types and behaviours with the engine registries.
// This decouples the core saida::Engine from the specific VR/AR toolkit types.
void registerTypes();

} // namespace saida::xr
