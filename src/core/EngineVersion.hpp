#pragma once

// Single home for the engine's version strings. The project has two distinct
// notions of "version"; keep them separate and change each only in the one
// place below.
//
//  1. Product / release version (kProductVersion) — the human-facing version:
//     the About box, the default exe metadata, the documentation, and the git
//     release tag (prefixed with 'v'). Follows SemVer with an optional
//     pre-release suffix. Bump this on every release.
//
//  2. Engine format / contract version (kEngineVersion) — recorded in every
//     project file and the authoring manifest; it governs on-disk document
//     migration (see SPEC.md). It is a data contract, NOT a marketing version,
//     and changes only when the persisted format changes. Tests assert its
//     exact value, so never fold a pre-release suffix into it.

namespace saida {

// -- Product / release version (single source of truth) -----------------------
inline constexpr const char* kProductVersion = "1.0.0-beta.4";

// MAJOR.MINOR.PATCH core of kProductVersion, without any pre-release suffix.
// Windows VERSIONINFO cannot carry a suffix, so exe metadata uses this. Keep the
// leading numbers in sync with kProductVersion.
inline constexpr const char* kProductVersionNumeric = "1.0.0";

// -- Engine format / contract version -----------------------------------------
inline constexpr const char* kEngineVersion = "1.0.0";

} // namespace saida
