cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel"
        FORCE)
endif()

project(SaidaEngine LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A crash report and its symbol bundle share this immutable identity. Track the
# current ref as a configure dependency so a local commit followed by `cmake
# --build` cannot silently retain the previous SHA.
set(SAIDA_BUILD_COMMIT "unknown")
# A containerized CI checkout can be owned by a different uid, causing Git's
# safe-directory guard to reject rev-parse. GitHub already exposes the exact
# immutable commit being built, so use it as a fail-closed identity fallback.
if(DEFINED ENV{GITHUB_SHA})
    string(LENGTH "$ENV{GITHUB_SHA}" SAIDA_CI_HEAD_LENGTH)
    if(SAIDA_CI_HEAD_LENGTH EQUAL 40 AND
       "$ENV{GITHUB_SHA}" MATCHES "^[0-9a-fA-F]+$")
        set(SAIDA_BUILD_COMMIT "$ENV{GITHUB_SHA}")
    endif()
endif()
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --verify HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SAIDA_GIT_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    string(LENGTH "${SAIDA_GIT_HEAD}" SAIDA_GIT_HEAD_LENGTH)
    if(SAIDA_GIT_HEAD_LENGTH EQUAL 40 AND SAIDA_GIT_HEAD MATCHES "^[0-9a-fA-F]+$")
        set(SAIDA_BUILD_COMMIT "${SAIDA_GIT_HEAD}")
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --git-path HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SAIDA_GIT_HEAD_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(SAIDA_GIT_HEAD_PATH)
        get_filename_component(SAIDA_GIT_HEAD_PATH "${SAIDA_GIT_HEAD_PATH}"
            ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        if(EXISTS "${SAIDA_GIT_HEAD_PATH}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${SAIDA_GIT_HEAD_PATH}")
        endif()
    endif()
    execute_process(
        COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SAIDA_GIT_REF
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(SAIDA_GIT_REF)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${SAIDA_GIT_REF}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE SAIDA_GIT_REF_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        get_filename_component(SAIDA_GIT_REF_PATH "${SAIDA_GIT_REF_PATH}"
            ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        if(EXISTS "${SAIDA_GIT_REF_PATH}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${SAIDA_GIT_REF_PATH}")
        endif()
    endif()
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/saida")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/core/BuildIdentity.hpp.in"
    "${CMAKE_BINARY_DIR}/generated/saida/BuildIdentity.hpp"
    @ONLY)

set(SAIDA_APP_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin)
set(SAIDA_TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/tests)

function(saida_app_target target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${SAIDA_APP_OUTPUT_DIR}
    )
endfunction()

function(saida_test_target target)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${SAIDA_TEST_OUTPUT_DIR}
    )
    # The tests assert() their checks: strip the config-level -DNDEBUG
    # (RelWithDebInfo/Release) or every assert-based test silently passes empty.
    target_compile_options(${target} PRIVATE -UNDEBUG)
endfunction()

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_program(GLSLC glslc REQUIRED)

# Compile shaders
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

set(SHADERS
    ${SHADER_DIR}/shader.vert
    ${SHADER_DIR}/shader.frag
    ${SHADER_DIR}/unlit.frag
    ${SHADER_DIR}/shadow.vert
    ${SHADER_DIR}/culling.comp
    ${SHADER_DIR}/tonemap.vert
    ${SHADER_DIR}/tonemap.frag
    ${SHADER_DIR}/bloom_downsample.frag
    ${SHADER_DIR}/bloom_upsample.frag
    ${SHADER_DIR}/skybox.vert
    ${SHADER_DIR}/skybox.frag
    ${SHADER_DIR}/water.vert
    ${SHADER_DIR}/water.frag
    ${SHADER_DIR}/cartoon_water.vert
    ${SHADER_DIR}/cartoon_water.frag
    ${SHADER_DIR}/particle_render.vert
    ${SHADER_DIR}/particle_render.frag
    ${SHADER_DIR}/particle_init.comp
    ${SHADER_DIR}/particle_prepare.comp
    ${SHADER_DIR}/particle_emit.comp
    ${SHADER_DIR}/particle_sim.comp
    ${SHADER_DIR}/particle_finalize.comp
    ${SHADER_DIR}/outline.vert
    ${SHADER_DIR}/outline.frag
    ${SHADER_DIR}/ui.vert
    ${SHADER_DIR}/ui.frag
    ${SHADER_DIR}/web_canvas_world.vert
    ${SHADER_DIR}/web_canvas_world.frag
    ${SHADER_DIR}/voxelize.vert
    ${SHADER_DIR}/voxelize.frag
    ${SHADER_DIR}/ddgi_trace.comp
    ${SHADER_DIR}/ddgi_blend.comp
    ${SHADER_DIR}/ddgi_borders.comp
    ${SHADER_DIR}/debug_line.vert
    ${SHADER_DIR}/debug_line.frag
)

# GLSL headers #included by the shaders above. Listed as a dependency so editing
# them re-triggers compilation of every shader that includes them.
file(GLOB SHADER_INCLUDES ${SHADER_DIR}/*.glsl)

set(SHADER_SPIRV "")
# Web (WGSL) set: shaders converted to the web shading model (separate textures +
# samplers + UBO, via web_compat.glsl). A -DWEB variant is compiled and then
# transpiled to WGSL only for these. The list grows as the port proceeds
# Web is mono: no bindless or multiview. web.<name>.spv is built
# on demand by the WebShaders target below.
set(WEB_READY_SHADERS
    shader.vert shader.frag unlit.frag
    shadow.vert
    tonemap.vert tonemap.frag
    bloom_downsample.frag bloom_upsample.frag
    skybox.vert skybox.frag
    outline.vert outline.frag
    voxelize.vert voxelize.frag
    debug_line.vert debug_line.frag
    ui.vert ui.frag
    water.vert water.frag cartoon_water.vert cartoon_water.frag
    web_canvas_world.vert web_canvas_world.frag
    particle_render.vert particle_render.frag
    culling.comp
    ddgi_trace.comp ddgi_blend.comp ddgi_borders.comp
    particle_init.comp particle_prepare.comp particle_emit.comp
    particle_sim.comp particle_finalize.comp)
set(SHADER_SPIRV_WEB "")
foreach(SHADER ${SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPIRV_OUTPUT ${SHADER_OUT_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SPIRV_OUTPUT}
        COMMAND ${GLSLC} -I ${SHADER_DIR} ${SHADER} -o ${SPIRV_OUTPUT}
        DEPENDS ${SHADER} ${SHADER_INCLUDES}
        COMMENT "Compiling ${SHADER_NAME} -> ${SHADER_NAME}.spv"
    )
    list(APPEND SHADER_SPIRV ${SPIRV_OUTPUT})

    # Web variant (-DWEB): separate samplers + UBO, transpilable to WGSL by naga.
    if(SHADER_NAME IN_LIST WEB_READY_SHADERS)
        set(WEB_SPV_OUTPUT ${SHADER_OUT_DIR}/web.${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${WEB_SPV_OUTPUT}
            COMMAND ${GLSLC} -I ${SHADER_DIR} -DWEB ${SHADER} -o ${WEB_SPV_OUTPUT}
            DEPENDS ${SHADER} ${SHADER_INCLUDES}
            COMMENT "Compiling web.${SHADER_NAME} -> web.${SHADER_NAME}.spv"
        )
        list(APPEND SHADER_SPIRV_WEB ${WEB_SPV_OUTPUT})
    endif()

    if(SHADER_NAME STREQUAL "shader.vert" OR SHADER_NAME STREQUAL "shader.frag")
        set(BINDLESS_OUTPUT ${SHADER_OUT_DIR}/bindless.${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${BINDLESS_OUTPUT}
            COMMAND ${GLSLC} -I ${SHADER_DIR} -DBINDLESS ${SHADER} -o ${BINDLESS_OUTPUT}
            DEPENDS ${SHADER} ${SHADER_INCLUDES}
            COMMENT "Compiling bindless.${SHADER_NAME} -> bindless.${SHADER_NAME}.spv"
        )
        list(APPEND SHADER_SPIRV ${BINDLESS_OUTPUT})
    endif()

    # Multiview (XR stereo, 1 pass / 2 layers): shaders that read per-eye data via
    # gl_ViewIndex get a -DMULTIVIEW variant. The scene vertex picks per-eye
    # camera matrices; the skybox fragment picks per-eye inverse view-proj.
    if(SHADER_NAME STREQUAL "shader.vert" OR SHADER_NAME STREQUAL "skybox.frag"
       OR SHADER_NAME STREQUAL "water.vert" OR SHADER_NAME STREQUAL "cartoon_water.vert"
       OR SHADER_NAME STREQUAL "outline.vert"
       OR SHADER_NAME STREQUAL "particle_render.vert")
        set(MULTIVIEW_OUTPUT ${SHADER_OUT_DIR}/multiview.${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${MULTIVIEW_OUTPUT}
            COMMAND ${GLSLC} -I ${SHADER_DIR} -DMULTIVIEW ${SHADER} -o ${MULTIVIEW_OUTPUT}
            DEPENDS ${SHADER} ${SHADER_INCLUDES}
            COMMENT "Compiling multiview.${SHADER_NAME} -> multiview.${SHADER_NAME}.spv"
        )
        list(APPEND SHADER_SPIRV ${MULTIVIEW_OUTPUT})
    endif()
endforeach()

add_custom_target(Shaders DEPENDS ${SHADER_SPIRV})

# ── Web shaders: SPIR-V → WGSL ───────────────────────────────────────────────
# Browsers' WebGPU only accepts WGSL, so the web build transpiles the very same
# SPIR-V we already produce with glslc. Uses `naga` (cargo install naga-cli).
# This is a separate on-demand target — desktop builds never trigger it — and is
# skipped entirely (with a notice) when naga isn't installed, so configuring the
# project never requires the web toolchain. Validate with:
#   cmake --build build --target WebShaders
find_program(NAGA naga)
if(NAGA)
    set(WGSL_OUT_DIR ${SHADER_OUT_DIR}/wgsl)
    file(MAKE_DIRECTORY ${WGSL_OUT_DIR})
    set(SHADER_WGSL "")
    foreach(SPV ${SHADER_SPIRV_WEB})
        get_filename_component(SPV_NAME ${SPV} NAME)             # web.shader.vert.spv
        string(REGEX REPLACE "^web\\." "" WGSL_NAME ${SPV_NAME}) # shader.vert.spv
        string(REGEX REPLACE "\\.spv$" ".wgsl" WGSL_NAME ${WGSL_NAME})  # shader.vert.wgsl
        set(WGSL_OUTPUT ${WGSL_OUT_DIR}/${WGSL_NAME})
        add_custom_command(
            OUTPUT ${WGSL_OUTPUT}
            COMMAND ${NAGA} ${SPV} ${WGSL_OUTPUT}
            DEPENDS ${SPV}
            COMMENT "Transpiling ${WGSL_NAME} (web) -> WGSL"
        )
        list(APPEND SHADER_WGSL ${WGSL_OUTPUT})
    endforeach()
    add_custom_target(WebShaders DEPENDS ${SHADER_WGSL})
    add_dependencies(WebShaders Shaders)
    message(STATUS "naga found (${NAGA}): WebShaders target enabled (SPIR-V -> WGSL)")
else()
    message(STATUS "naga not found - web shader (WGSL) generation disabled "
                   "(install with: cargo install naga-cli)")
endif()

# ── Jolt Physics (vendored) ─────────────────────────────────────────────────
# Only the static `Jolt` lib is built — the samples/tests/unit-test targets are
# auto-skipped because Jolt's Build/CMakeLists gates them on being the top-level
# project. The SIMD/define flags Jolt picks are PUBLIC on the target, so anything
# linking it gets a matching ABI automatically.
set(INTERPROCEDURAL_OPTIMIZATION OFF)          # consumer isn't LTO; mixing breaks the MinGW link
set(ENABLE_ALL_WARNINGS OFF)                    # don't -Werror Jolt on this toolchain's warning set
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)      # MSVC-only feature; off avoids surprises elsewhere
set(ENABLE_OBJECT_STREAM OFF)                   # we don't use Jolt's RTTI ObjectStream serialization
set(ENABLE_INSTALL OFF)                         # no install/export targets needed
set(CROSS_PLATFORM_DETERMINISTIC OFF)
set(DOUBLE_PRECISION OFF)                        # 32-bit positions: smaller/faster, fine for our worlds
add_subdirectory(third_party/jolt/Build ${CMAKE_BINARY_DIR}/jolt)

# ── MCP server ───────────────────────────────────────────────────────────────
# In-process MCP server. Compiled into saida_editor only,
# so the shipped runtime never links it. Set OFF for a pure no-MCP editor.
option(SAIDA_ENABLE_MCP "Enable the in-process MCP server" ON)

# ── OpenXR (vendored, loader only) ──────────────────────────────────────────
# Set SAIDA_ENABLE_XR=OFF to produce a pure-desktop build with no OpenXR dependency.
option(SAIDA_ENABLE_XR "Enable OpenXR support" ON)

if(SAIDA_ENABLE_XR)
    # Build just the static openxr_loader from the pre-generated SDK repo (no Python
    # generation, no API layers/tests). The loader finds the active runtime (Quest
    # Link / Oculus) via the registry at runtime. Headers are propagated by the target.
    set(DYNAMIC_LOADER OFF)        # static loader, consistent with the project's -static link
    set(BUILD_API_LAYERS OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_CONFORMANCE_TESTS OFF)
    set(BUILD_SDK_TESTS OFF)
    set(BUILD_ALL_EXTENSIONS OFF)
    add_subdirectory(third_party/openxr ${CMAKE_BINARY_DIR}/openxr)

    # MinGW's winapifamily.h does not define the UWP system partition constant
    # used by OpenXR 1.1.60.  This is a desktop Win32 build, so that partition is
    # absent; defining it to zero also keeps the loader's -Werror=undef happy.
    if (MINGW)
        target_compile_definitions(openxr_loader PRIVATE WINAPI_PARTITION_SYSTEM=0)
    endif()
endif()

# ── QuickJS + RmlUi (vendored, static) ───────────────────────────────────────
# QuickJS is the single JavaScript runtime for gameplay scripts, editor tooling
# and web UI logic. We build only the library, not qjs/qjsc executables.
add_library(quickjs STATIC
    third_party/quickjs/dtoa.c
    third_party/quickjs/libregexp.c
    third_party/quickjs/libunicode.c
    third_party/quickjs/quickjs.c
)
target_include_directories(quickjs PUBLIC third_party/quickjs)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
if (WIN32)
    target_compile_definitions(quickjs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()
if (MINGW)
    target_link_libraries(quickjs PUBLIC m)
endif()

# ── meshoptimizer (vendored, static) ────────────────────────────────────────
# Geometry compression/optimization. Runtime needs the
# decoders (EXT_meshopt_compression GLBs); the editor's import also uses the
# encoders/optimizers. Header + .cpp only, no executables. v0.22.
file(GLOB MESHOPT_SOURCES ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPT_SOURCES})
target_include_directories(meshoptimizer PUBLIC third_party/meshoptimizer/src)

# Freetype is used only by RmlUi's font engine. Disable optional compression /
# shaping dependencies so the engine remains easy to build from source.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HVF ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype ${CMAKE_BINARY_DIR}/freetype)
if (NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_TESTS OFF CACHE BOOL "" FORCE)
set(RMLUI_BACKEND "auto" CACHE STRING "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_HARFBUZZ_SAMPLE OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/rmlui ${CMAKE_BINARY_DIR}/rmlui)

# AutoLOD is built separately: `cmake -S autolod -B build/autolod -G Ninja && cmake --build build/autolod`
# AutoLODBridge resolves build/autolod/autolod.exe at runtime.

# ── Engine library ──────────────────────────────────────────────────────────
# Everything reusable lives here and compiles once. Game and editor targets link
# it, so changing application code does not recompile the engine.
add_library(saida_engine STATIC
    src/Engine.cpp
    src/authoring/EngineManifest.cpp
    src/authoring/SceneSnapshot.cpp
    src/authoring/SaidaOp.cpp
    src/authoring/SaidaOpApplier.cpp
    src/core/FileWatcher.cpp
    src/core/Window.cpp
    src/core/Camera.cpp
    src/core/AtomicFile.cpp
    src/core/CrashReporter.cpp
    src/core/Input.cpp
    src/core/InputProfile.cpp
    src/core/Paths.cpp
    src/core/PngWriter.cpp
    src/core/Profiler.cpp
    src/core/Reflection.cpp
    src/fx/ParticleEffect.cpp
    src/fx/ParticlePresetLibrary.cpp
    src/fx/ParticleQuality.cpp
    src/core/PlatformCaps.cpp
    src/runtime/BootManifest.cpp
    src/runtime/RuntimeRoundTripContract.cpp
    src/scene/NodeId.cpp
    src/scene/Node.cpp
    src/scene/Transform.cpp
    src/scene/ReflectedTypes.cpp
    src/scripting/JsContext.cpp
    src/scripting/JsEngineBindings.cpp
    src/scripting/JsRuntime.cpp
    src/scripting/JsTimerBindings.cpp
    src/scripting/ScriptBehaviour.cpp
    src/ui/HudRasterizer.cpp
    src/ui/RmlUiRenderInterface.cpp
    src/ui/RmlUiRuntime.cpp
    src/ui/WorldPanelGeometry.cpp
    src/scene/Behaviour.cpp
    src/nodes/MeshNode.cpp
    src/scene/MeshLod.cpp
    src/behaviours/LODGroupBehaviour.cpp
    src/nodes/LightNode.cpp
    src/nodes/ParticleSystemNode.cpp
    src/nodes/CameraNode.cpp
    src/render/CameraDirector.cpp
    src/behaviours/CameraFollowBehaviour.cpp
    src/scene/Scene.cpp
    src/scene/SceneSettingsSerialization.cpp
    src/scene/SceneTree.cpp
    src/scene/SceneTimerQueue.cpp
    src/scene/SceneSerializer.cpp
    src/scene/SignalWiring.cpp
    src/scene/BVHLoader.cpp
    src/scene/BehaviourRegistry.cpp
    src/scene/NodeRegistry.cpp
    src/scene/GltfMeshopt.cpp
    src/cli/MeshoptGlbExporter.cpp
    src/nodes/WebCanvasNode.cpp
    src/nodes/UINode.cpp
    src/nodes/UICanvasNode.cpp
    src/nodes/UIColorNode.cpp
    src/nodes/UIImageNode.cpp
    src/nodes/UITextNode.cpp
    src/nodes/UIInteractableNode.cpp
    src/nodes/UIButtonNode.cpp
    src/nodes/UIToggleNode.cpp
    src/scene/animation/Pose.cpp
    src/scene/animation/AnimationClip.cpp
    src/scene/animation/Rig.cpp
    src/scene/animation/ClipView.cpp
    src/scene/animation/AnimGraphAsset.cpp
    src/scene/animation/ProceduralNodes.cpp
    src/scene/animation/RetargetProfile.cpp
    src/scene/animation/RigAsset.cpp
    src/scene/animation/ClipNode.cpp
    src/scene/animation/Blend1DNode.cpp
    src/scene/animation/BlendNode.cpp
    src/scene/animation/LayerNode.cpp
    src/scene/animation/AnimStateMachine.cpp
    src/scene/animation/Animator.cpp
    src/scene/animation/AnimBlackboard.cpp
    src/scene/animation/AnimationAssetDecoders.cpp
    src/scene/animation/AnimationCache.cpp
    src/scene/animation/AnimationCooker.cpp
    src/scene/animation/AnimationProgram.cpp
    src/scene/animation/AnimationSequence.cpp
    src/scene/animation/CookedClip.cpp
    src/scene/animation/SequenceDirectorBehaviour.cpp
    src/scene/animation/PlayableDirector.cpp
    src/scene/animation/Retarget.cpp
    src/ui/UIInteractionSystem.cpp
    src/audio/AudioManager.cpp
    src/rhi/vulkan/BindGroup.cpp
    src/rhi/vulkan/CommandEncoder.cpp
    src/rhi/vulkan/RenderTexture.cpp
    src/rhi/vulkan/Sampler.cpp
    src/graphics/VulkanDevice.cpp
    src/graphics/Swapchain.cpp
    src/graphics/Buffer.cpp
    src/graphics/Material.cpp
    src/graphics/GpuProfiler.cpp
    src/graphics/ImGuiLayer.cpp
    src/graphics/MemoryProfiler.cpp
    src/graphics/GeometryRegistry.cpp
    src/graphics/Mesh.cpp
    src/graphics/UIRenderer.cpp
    src/graphics/StbImageUsage.cpp
    src/graphics/Primitives.cpp
    src/graphics/Pipeline.cpp
    src/graphics/ComputePipeline.cpp
    src/graphics/ShadowMap.cpp
    src/graphics/Texture.cpp
    src/graphics/BindlessTables.cpp
    src/graphics/GpuGraveyard.cpp
    src/graphics/GpuBudget.cpp
    src/graphics/MeshCache.cpp
    src/graphics/TextureCache.cpp
    src/graphics/ResourceManager.cpp
    src/render/Renderer.cpp
    src/render/EnvironmentSH.cpp
    src/render/PostProcessor.cpp
    src/render/RenderFeatureRegistry.cpp
    src/fx/ParticleRuntime.cpp
    src/render/features/WaterFeature.cpp
    src/render/features/ParticleFeature.cpp
    src/render/features/OutlineFeature.cpp
    src/render/features/SkyboxFeature.cpp
    src/render/features/DebugLinesFeature.cpp
    src/render/GIVolume.cpp
    # XR sources are added conditionally below via target_sources (SAIDA_ENABLE_XR)
    src/project/Project.cpp
    src/project/ProjectRename.cpp
    src/project/AssetRegistry.cpp
    src/project/AssetLoader.cpp
    src/project/PlayerStorage.cpp
    src/graphics/VmaUsage.cpp
    src/graphics/TinyObjUsage.cpp
    src/graphics/cgltf_impl.cpp
    src/scene/GLTFLoader.cpp
    src/cli/AutoLODBridge.cpp
    src/audio/AudioUsage.cpp
    src/audio/AudioSourceBehaviour.cpp
    src/behaviours/AnimGraphBehaviour.cpp
    src/behaviours/CharacterBehaviour.cpp
    src/behaviours/HealthBehaviour.cpp
    src/nodes/WaterNode.cpp
    src/behaviours/SpawnerBehaviour.cpp
    src/behaviours/RotatorBehaviour.cpp
    src/scene/Blackboard.cpp
    src/behaviours/StateMachineBehaviour.cpp
    src/scenario/ScenarioAsset.cpp
    src/scenario/ScenarioRegistry.cpp
    src/scenario/ScenarioAnchor.cpp
    src/scenario/ScenarioDirector.cpp
    src/scenario/ScenarioRunnerBehaviour.cpp
    src/physics/PhysicsWorld.cpp
    src/physics/CollisionShapeNode.cpp
    src/physics/CollisionObjectNode.cpp
    src/physics/CharacterBodyNode.cpp
    src/physics/PhysicsNodes.cpp
    src/physics/JointNodes.cpp
    # Dear ImGui (vendored) + GLFW/Vulkan backends
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
)

# Keep generated behaviour sources automatic; new files need no CMake edit.
file(GLOB SAIDA_GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/generated/*.cpp)
if(SAIDA_GENERATED_SOURCES)
    target_sources(saida_engine PRIVATE ${SAIDA_GENERATED_SOURCES})
endif()

# PUBLIC: propagated to anything linking the engine (the game/editor exe).
target_include_directories(saida_engine PUBLIC
    src
    third_party/vma
    third_party/tinyobjloader
    third_party/stb
    third_party/cgltf
    third_party/imgui
    third_party/json
    third_party/miniaudio
)
target_link_libraries(saida_engine PUBLIC Vulkan::Vulkan glfw glm::glm Jolt quickjs RmlUi::RmlUi meshoptimizer)
if(WIN32)
    target_link_libraries(saida_engine PUBLIC winmm psapi)
endif()
if(SAIDA_ENABLE_XR)
    target_sources(saida_engine PRIVATE
        src/xr/XrInstance.cpp
        src/xr/XrVulkanBinding.cpp
        src/xr/XrRegistration.cpp
        src/xr/XrSwapchain.cpp
        src/xr/XrSession.cpp
        src/xr/XrActions.cpp
        src/xr/XrHandTracking.cpp
        src/xr/toolkit/XRInput.cpp
        src/xr/toolkit/XRController.cpp
        src/xr/toolkit/XRHand.cpp
        src/xr/toolkit/XRGrabbable.cpp
        src/xr/toolkit/XRTouchable.cpp
        src/xr/toolkit/XRDirectInteractor.cpp
        src/xr/toolkit/XROrigin.cpp
        src/xr/toolkit/TeleportArea.cpp
        src/xr/toolkit/XRRayInteractor.cpp
        src/xr/toolkit/XRPassthrough.cpp
        src/xr/toolkit/XRAnchors.cpp
        src/xr/toolkit/XRAnchor.cpp
    )
    target_link_libraries(saida_engine PUBLIC openxr_loader)
endif()
add_dependencies(saida_engine Shaders)

# Absolute paths baked at configure time so the exe runs from any cwd; and GLM
# conventions (radians, Vulkan 0..1 depth) — PUBLIC so game code sees them too.
target_compile_definitions(saida_engine PRIVATE SAIDA_SCENE_TREE_TIMERS)
target_compile_definitions(saida_engine PUBLIC
    SAIDA_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    SAIDA_BINARY_DIR="${CMAKE_BINARY_DIR}"
    SAIDA_RUNTIME_DIR="${SAIDA_APP_OUTPUT_DIR}"
    SAIDA_SHADER_DIR="${SHADER_OUT_DIR}"
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_include_directories(saida_engine PRIVATE "${CMAKE_BINARY_DIR}/generated")
if(SAIDA_ENABLE_XR)
    target_compile_definitions(saida_engine PUBLIC SAIDA_ENABLE_XR)
endif()

# Warnings for the engine's own sources (third-party TUs override this with -w).
target_compile_options(saida_engine PRIVATE -Wall -Wextra)
set_source_files_properties(
    src/graphics/VmaUsage.cpp
    src/graphics/TinyObjUsage.cpp
    src/graphics/StbImageUsage.cpp
    src/graphics/cgltf_impl.cpp
    src/audio/AudioUsage.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp
    PROPERTIES COMPILE_OPTIONS "-w")

# Statically link the GCC/libstdc++ runtimes. Required here because this MSYS2
# toolchain's dynamic libstdc++ import library crashes the linker (ld exit 116)
# on iostream/exception symbols; bonus: self-contained exe. PUBLIC so the link
# options propagate to executables linking the engine.
if (MINGW)
    target_link_options(saida_engine PUBLIC -static -static-libgcc -static-libstdc++)
endif()
if (WIN32)
    target_link_libraries(saida_engine PUBLIC dbghelp)
endif()

# ── Editor library ──────────────────────────────────────────────────────────
# The editor (UI panels, commands, input/camera control) lives on top of the
# engine, not inside it — so a shipped game can link saida_engine without it.
add_library(saida_editor STATIC
    src/editor/EditorApp.cpp
    src/editor/EditorUI.cpp
    src/editor/GizmoController.cpp
    src/editor/BuildController.cpp
    src/editor/ProjectDialogs.cpp
    src/editor/SettingsWindow.cpp
    src/editor/BuildExporter.cpp
    src/editor/ExeMetadata.cpp
    src/editor/SceneDocument.cpp
    src/editor/Command.cpp
    src/editor/CommandHistory.cpp
    src/editor/ThumbnailCache.cpp
    src/editor/InspectorRegistry.cpp
    src/editor/ProfilerTriangleMetrics.cpp
    src/editor/panels/MenuBarPanel.cpp
    src/editor/panels/SceneHierarchyPanel.cpp
    src/editor/panels/InspectorPanel.cpp
    src/editor/panels/FileBrowserPanel.cpp
    src/editor/panels/ViewportPanel.cpp
    src/editor/panels/ModelImporterPanel.cpp
    src/editor/panels/ProfilerPanel.cpp
    src/editor/panels/AnimationPanel.cpp
)
target_link_libraries(saida_editor PUBLIC saida_engine)
target_compile_options(saida_editor PRIVATE -Wall -Wextra)

if(SAIDA_ENABLE_MCP)
    target_sources(saida_editor PRIVATE
        src/mcp/McpServer.cpp
        src/mcp/McpBridge.cpp
        src/mcp/tools/ToolContext.cpp
        src/mcp/tools/ToolRegistry.cpp
        src/mcp/tools/IntrospectionTools.cpp
        src/mcp/tools/SceneTools.cpp
        src/mcp/tools/NodeTools.cpp
        src/mcp/tools/AssetTools.cpp
        src/mcp/tools/ScenarioTools.cpp
        src/mcp/tools/AnimationTools.cpp
    )
    target_compile_definitions(saida_editor PUBLIC SAIDA_ENABLE_MCP)
    if(WIN32)
        target_link_libraries(saida_editor PUBLIC ws2_32)
    endif()
endif()

if (BUILD_TESTING)
    add_executable(saida_core_tests tests/CoreTests.cpp)
    saida_test_target(saida_core_tests)
    target_sources(saida_core_tests PRIVATE src/core/Paths.cpp)
    target_include_directories(saida_core_tests PRIVATE src)
    target_compile_definitions(saida_core_tests PRIVATE
        SAIDA_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
        SAIDA_SHADER_DIR="${SHADER_OUT_DIR}"
    )
    target_compile_options(saida_core_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_core_tests COMMAND $<TARGET_FILE:saida_core_tests>)

    # The PNG encoder is written in-tree, so its output is proven by decoding it
    # back with stb_image (an independent implementation) rather than by trusting
    # the encoder to describe itself.
    add_executable(saida_png_writer_tests tests/PngWriterTests.cpp src/core/PngWriter.cpp)
    saida_test_target(saida_png_writer_tests)
    target_include_directories(saida_png_writer_tests PRIVATE src third_party/stb)
    target_compile_options(saida_png_writer_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_png_writer_tests COMMAND $<TARGET_FILE:saida_png_writer_tests>)

    add_executable(saida_crash_reporter_tests tests/CrashReporterTests.cpp)
    saida_test_target(saida_crash_reporter_tests)
    target_link_libraries(saida_crash_reporter_tests PRIVATE saida_engine)
    target_compile_options(saida_crash_reporter_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_crash_reporter_tests
        COMMAND $<TARGET_FILE:saida_crash_reporter_tests>)

    # The GPU culling buffers are all StorageBuffer descriptors, so a binding
    # swap remains shader-valid and is not caught by normal compilation. Keep a
    # lightweight contract test outside the Vulkan runtime path.
    add_executable(saida_gpu_driven_contract_tests tests/GpuDrivenContractTests.cpp)
    saida_test_target(saida_gpu_driven_contract_tests)
    target_include_directories(saida_gpu_driven_contract_tests PRIVATE src)
    target_link_libraries(saida_gpu_driven_contract_tests PRIVATE glm::glm)
    target_compile_definitions(saida_gpu_driven_contract_tests PRIVATE
        SAIDA_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    target_compile_options(saida_gpu_driven_contract_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_gpu_driven_contract_tests COMMAND $<TARGET_FILE:saida_gpu_driven_contract_tests>)

    # Player boot (game.saida manifest + capability contract) — no GPU.
    add_executable(saida_runtime_boot_tests tests/RuntimeBootTests.cpp
                   src/runtime/BootManifest.cpp src/core/PlatformCaps.cpp)
    saida_test_target(saida_runtime_boot_tests)
    target_include_directories(saida_runtime_boot_tests PRIVATE src third_party/json)
    target_compile_options(saida_runtime_boot_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_runtime_boot_tests COMMAND $<TARGET_FILE:saida_runtime_boot_tests>)

    add_executable(saida_input_gamepad_tests tests/InputGamepadTests.cpp)
    saida_test_target(saida_input_gamepad_tests)
    target_include_directories(saida_input_gamepad_tests PRIVATE src)
    target_compile_options(saida_input_gamepad_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_input_gamepad_tests COMMAND $<TARGET_FILE:saida_input_gamepad_tests>)

    add_executable(saida_input_profile_tests tests/InputProfileTests.cpp
                   src/core/InputProfile.cpp)
    saida_test_target(saida_input_profile_tests)
    target_include_directories(saida_input_profile_tests PRIVATE
        src third_party/glm third_party/json)
    target_compile_options(saida_input_profile_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_input_profile_tests COMMAND $<TARGET_FILE:saida_input_profile_tests>)

    add_executable(saida_input_touch_tests tests/InputTouchTests.cpp)
    saida_test_target(saida_input_touch_tests)
    target_include_directories(saida_input_touch_tests PRIVATE src third_party/glm)
    target_compile_options(saida_input_touch_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_input_touch_tests COMMAND $<TARGET_FILE:saida_input_touch_tests>)

    add_executable(saida_animation_core_tests tests/AnimationCoreTests.cpp)
    saida_test_target(saida_animation_core_tests)
    target_link_libraries(saida_animation_core_tests PRIVATE saida_engine)
    target_compile_options(saida_animation_core_tests PRIVATE -Wall -Wextra)
    target_compile_definitions(saida_animation_core_tests PRIVATE
        SAIDA_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
    add_test(NAME saida_animation_core_tests COMMAND $<TARGET_FILE:saida_animation_core_tests>)

    add_executable(saida_animation_playback_tests tests/AnimationPlaybackTests.cpp)
    saida_test_target(saida_animation_playback_tests)
    target_link_libraries(saida_animation_playback_tests PRIVATE saida_engine)
    target_compile_options(saida_animation_playback_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_animation_playback_tests COMMAND $<TARGET_FILE:saida_animation_playback_tests>)

    add_executable(saida_animation_retarget_tests tests/AnimationRetargetTests.cpp)
    saida_test_target(saida_animation_retarget_tests)
    target_link_libraries(saida_animation_retarget_tests PRIVATE saida_engine)
    target_compile_options(saida_animation_retarget_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_animation_retarget_tests COMMAND $<TARGET_FILE:saida_animation_retarget_tests>)

    add_executable(saida_animation_sequence_tests tests/AnimationSequenceTests.cpp)
    saida_test_target(saida_animation_sequence_tests)
    target_link_libraries(saida_animation_sequence_tests PRIVATE saida_engine)
    target_compile_options(saida_animation_sequence_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_animation_sequence_tests COMMAND $<TARGET_FILE:saida_animation_sequence_tests>)

    add_executable(saida_animation_cooker_tests tests/AnimationCookerTests.cpp)
    saida_test_target(saida_animation_cooker_tests)
    target_link_libraries(saida_animation_cooker_tests PRIVATE saida_engine)
    target_compile_options(saida_animation_cooker_tests PRIVATE -Wall -Wextra)
    target_compile_definitions(saida_animation_cooker_tests PRIVATE
        SAIDA_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
    add_test(NAME saida_animation_cooker_tests COMMAND $<TARGET_FILE:saida_animation_cooker_tests>)

    add_executable(saida_animation_instance_tests tests/AnimationInstanceTests.cpp)
    saida_test_target(saida_animation_instance_tests)
    target_link_libraries(saida_animation_instance_tests PRIVATE saida_engine)
    target_compile_options(saida_animation_instance_tests PRIVATE -Wall -Wextra)
    target_compile_definitions(saida_animation_instance_tests PRIVATE
        SAIDA_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
    add_test(NAME saida_animation_instance_tests COMMAND $<TARGET_FILE:saida_animation_instance_tests>)

    add_executable(saida_cartoon_water_tests tests/CartoonWaterTests.cpp)
    saida_test_target(saida_cartoon_water_tests)
    target_link_libraries(saida_cartoon_water_tests PRIVATE saida_engine)
    target_compile_definitions(saida_cartoon_water_tests PRIVATE
        SAIDA_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")
    target_compile_options(saida_cartoon_water_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_cartoon_water_tests COMMAND $<TARGET_FILE:saida_cartoon_water_tests>)

    # Headless animation benchmark. The ctest entry runs
    # a tiny workload as a smoke test; run it manually with real sizes to measure.
    add_executable(saida_animation_bench tests/AnimationBench.cpp)
    saida_test_target(saida_animation_bench)
    target_link_libraries(saida_animation_bench PRIVATE saida_engine)
    target_compile_options(saida_animation_bench PRIVATE -Wall -Wextra)
    add_test(NAME saida_animation_bench_smoke
             COMMAND $<TARGET_FILE:saida_animation_bench> --chars 4 --bones 16 --keys 8 --frames 20)

    add_executable(saida_scene_lifecycle_tests tests/SceneLifecycleTests.cpp)
    saida_test_target(saida_scene_lifecycle_tests)
    target_link_libraries(saida_scene_lifecycle_tests PRIVATE saida_engine)
    target_compile_options(saida_scene_lifecycle_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_scene_lifecycle_tests COMMAND $<TARGET_FILE:saida_scene_lifecycle_tests>)

    add_executable(saida_scene_settings_tests tests/SceneSettingsSerializationTests.cpp)
    saida_test_target(saida_scene_settings_tests)
    target_link_libraries(saida_scene_settings_tests PRIVATE saida_engine)
    target_compile_options(saida_scene_settings_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_scene_settings_tests COMMAND $<TARGET_FILE:saida_scene_settings_tests>)

    add_executable(saida_scene_timer_tests tests/SceneTimerQueueTests.cpp)
    saida_test_target(saida_scene_timer_tests)
    target_link_libraries(saida_scene_timer_tests PRIVATE saida_engine)
    target_compile_options(saida_scene_timer_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_scene_timer_tests COMMAND $<TARGET_FILE:saida_scene_timer_tests>)

    add_executable(saida_reflection_tests tests/ReflectionTests.cpp)
    saida_test_target(saida_reflection_tests)
    target_link_libraries(saida_reflection_tests PRIVATE saida_engine)
    target_compile_options(saida_reflection_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_reflection_tests COMMAND $<TARGET_FILE:saida_reflection_tests>)

    add_executable(saida_authoring_op_tests tests/AuthoringOpTests.cpp)
    saida_test_target(saida_authoring_op_tests)
    target_link_libraries(saida_authoring_op_tests PRIVATE saida_engine)
    target_compile_options(saida_authoring_op_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_authoring_op_tests COMMAND $<TARGET_FILE:saida_authoring_op_tests>)

    add_executable(saida_particle_effect_tests tests/ParticleEffectTests.cpp)
    saida_test_target(saida_particle_effect_tests)
    target_link_libraries(saida_particle_effect_tests PRIVATE saida_engine)
    target_compile_options(saida_particle_effect_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_particle_effect_tests COMMAND $<TARGET_FILE:saida_particle_effect_tests>)

    add_executable(saida_signal_wiring_tests tests/SignalWiringTests.cpp)
    saida_test_target(saida_signal_wiring_tests)
    target_link_libraries(saida_signal_wiring_tests PRIVATE saida_engine)
    target_compile_options(saida_signal_wiring_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_signal_wiring_tests COMMAND $<TARGET_FILE:saida_signal_wiring_tests>)

    add_executable(saida_js_signal_tests tests/JsSignalBindingTests.cpp)
    saida_test_target(saida_js_signal_tests)
    target_link_libraries(saida_js_signal_tests PRIVATE saida_engine)
    target_compile_options(saida_js_signal_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_js_signal_tests COMMAND $<TARGET_FILE:saida_js_signal_tests>)

    add_executable(saida_js_gameplay_tests tests/JsGameplayBindingTests.cpp)
    saida_test_target(saida_js_gameplay_tests)
    target_link_libraries(saida_js_gameplay_tests PRIVATE saida_engine)
    target_compile_options(saida_js_gameplay_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_js_gameplay_tests COMMAND $<TARGET_FILE:saida_js_gameplay_tests>)

    add_executable(saida_js_permission_policy_tests tests/JsPermissionPolicyTests.cpp)
    saida_test_target(saida_js_permission_policy_tests)
    target_link_libraries(saida_js_permission_policy_tests PRIVATE saida_engine)
    target_compile_options(saida_js_permission_policy_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_js_permission_policy_tests COMMAND $<TARGET_FILE:saida_js_permission_policy_tests>)

    add_executable(saida_js_storage_flush_tests tests/JsStorageFlushTests.cpp)
    saida_test_target(saida_js_storage_flush_tests)
    target_link_libraries(saida_js_storage_flush_tests PRIVATE saida_engine)
    target_compile_options(saida_js_storage_flush_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_js_storage_flush_tests COMMAND $<TARGET_FILE:saida_js_storage_flush_tests>)

    add_executable(saida_js_cross_context_tests tests/JsCrossContextTests.cpp)
    saida_test_target(saida_js_cross_context_tests)
    target_link_libraries(saida_js_cross_context_tests PRIVATE saida_engine)
    target_compile_options(saida_js_cross_context_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_js_cross_context_tests COMMAND $<TARGET_FILE:saida_js_cross_context_tests>)

    add_executable(saida_js_timer_tests tests/JsTimerBindingTests.cpp)
    saida_test_target(saida_js_timer_tests)
    target_link_libraries(saida_js_timer_tests PRIVATE saida_engine)
    target_compile_options(saida_js_timer_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_js_timer_tests COMMAND $<TARGET_FILE:saida_js_timer_tests>)

    add_executable(saida_js_runtime_safety_tests tests/JsRuntimeSafetyTests.cpp)
    saida_test_target(saida_js_runtime_safety_tests)
    target_link_libraries(saida_js_runtime_safety_tests PRIVATE saida_engine)
    target_compile_options(saida_js_runtime_safety_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_js_runtime_safety_tests COMMAND $<TARGET_FILE:saida_js_runtime_safety_tests>)

    add_executable(saida_atomic_file_tests tests/AtomicFileTests.cpp)
    saida_test_target(saida_atomic_file_tests)
    target_link_libraries(saida_atomic_file_tests PRIVATE saida_engine)
    target_compile_options(saida_atomic_file_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_atomic_file_tests COMMAND $<TARGET_FILE:saida_atomic_file_tests>)

    add_executable(saida_test_autoload_tests tests/TestAutoloadTests.cpp)
    saida_test_target(saida_test_autoload_tests)
    target_link_libraries(saida_test_autoload_tests PRIVATE saida_engine)
    target_compile_options(saida_test_autoload_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_test_autoload_tests COMMAND $<TARGET_FILE:saida_test_autoload_tests>)

    add_executable(saida_runtime_type_matrix_tests tests/RuntimeTypeMatrixTests.cpp)
    saida_test_target(saida_runtime_type_matrix_tests)
    target_link_libraries(saida_runtime_type_matrix_tests PRIVATE saida_engine)
    target_compile_options(saida_runtime_type_matrix_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_runtime_type_matrix_tests
             COMMAND $<TARGET_FILE:saida_runtime_type_matrix_tests>)

    add_executable(saida_gameplay_primitive_tests tests/GameplayPrimitiveTests.cpp)
    saida_test_target(saida_gameplay_primitive_tests)
    target_link_libraries(saida_gameplay_primitive_tests PRIVATE saida_engine)
    target_compile_options(saida_gameplay_primitive_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_gameplay_primitive_tests COMMAND $<TARGET_FILE:saida_gameplay_primitive_tests>)

    add_executable(saida_scenario_system_tests tests/ScenarioSystemTests.cpp)
    saida_test_target(saida_scenario_system_tests)
    target_link_libraries(saida_scenario_system_tests PRIVATE saida_engine)
    target_compile_options(saida_scenario_system_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_scenario_system_tests COMMAND $<TARGET_FILE:saida_scenario_system_tests>)

    add_executable(saida_format_version_tests tests/FormatVersionTests.cpp)
    saida_test_target(saida_format_version_tests)
    target_link_libraries(saida_format_version_tests PRIVATE saida_engine)
    target_compile_options(saida_format_version_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_format_version_tests COMMAND $<TARGET_FILE:saida_format_version_tests>)

    add_executable(saida_player_storage_tests tests/PlayerStorageTests.cpp)
    saida_test_target(saida_player_storage_tests)
    target_link_libraries(saida_player_storage_tests PRIVATE saida_engine)
    target_compile_options(saida_player_storage_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_player_storage_tests COMMAND $<TARGET_FILE:saida_player_storage_tests>)

    add_executable(saida_project_rename_tests tests/ProjectRenameTests.cpp)
    saida_test_target(saida_project_rename_tests)
    target_link_libraries(saida_project_rename_tests PRIVATE saida_engine)
    target_compile_options(saida_project_rename_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_project_rename_tests COMMAND $<TARGET_FILE:saida_project_rename_tests>)

    # V1 UI corpus: RmlUi CPU rendering proven pixel-by-pixel without a GPU.
    add_executable(saida_ui_corpus_tests tests/UiCorpusTests.cpp)
    saida_test_target(saida_ui_corpus_tests)
    target_link_libraries(saida_ui_corpus_tests PRIVATE saida_engine)
    target_compile_options(saida_ui_corpus_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_ui_corpus_tests COMMAND $<TARGET_FILE:saida_ui_corpus_tests>)

    # World-Space UI: raycast geometry of a 3D panel (interaction).
    add_executable(saida_ui_worldspace_tests tests/UiWorldPanelTests.cpp)
    saida_test_target(saida_ui_worldspace_tests)
    target_link_libraries(saida_ui_worldspace_tests PRIVATE saida_engine)
    target_compile_options(saida_ui_worldspace_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_ui_worldspace_tests COMMAND $<TARGET_FILE:saida_ui_worldspace_tests>)

    # Interaction UI V1 : hit-test, hover/press/click, capture, transparence HUD.
    add_executable(saida_ui_interaction_tests tests/UiInteractionTests.cpp)
    saida_test_target(saida_ui_interaction_tests)
    target_link_libraries(saida_ui_interaction_tests PRIVATE saida_engine)
    target_compile_options(saida_ui_interaction_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_ui_interaction_tests COMMAND $<TARGET_FILE:saida_ui_interaction_tests>)

    add_executable(saida_asset_loader_tests tests/AssetLoaderTests.cpp)
    saida_test_target(saida_asset_loader_tests)
    target_link_libraries(saida_asset_loader_tests PRIVATE saida_engine)
    target_compile_options(saida_asset_loader_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_asset_loader_tests COMMAND $<TARGET_FILE:saida_asset_loader_tests>)

    # The V1 fixtures are frozen and loaded on every build.
    add_executable(saida_v1_format_corpus_tests tests/V1FormatCorpusTests.cpp)
    saida_test_target(saida_v1_format_corpus_tests)
    target_link_libraries(saida_v1_format_corpus_tests PRIVATE saida_engine)
    target_compile_definitions(saida_v1_format_corpus_tests PRIVATE
        SAIDA_V1_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/v1-format")
    target_compile_options(saida_v1_format_corpus_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_v1_format_corpus_tests COMMAND $<TARGET_FILE:saida_v1_format_corpus_tests>)

    # Exe metadata for the Build: VERSIONINFO + icon.
    add_executable(saida_exe_metadata_tests tests/ExeMetadataTests.cpp
                   src/editor/ExeMetadata.cpp)
    saida_test_target(saida_exe_metadata_tests)
    target_include_directories(saida_exe_metadata_tests PRIVATE src)
    target_compile_options(saida_exe_metadata_tests PRIVATE -Wall -Wextra)
    if (WIN32)
        target_link_libraries(saida_exe_metadata_tests PRIVATE version)
    endif()
    add_test(NAME saida_exe_metadata_tests COMMAND $<TARGET_FILE:saida_exe_metadata_tests>)

    add_executable(saida_meshopt_export_tests tests/MeshoptExportTests.cpp)
    saida_test_target(saida_meshopt_export_tests)
    target_link_libraries(saida_meshopt_export_tests PRIVATE saida_engine)
    target_compile_options(saida_meshopt_export_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_meshopt_export_tests COMMAND $<TARGET_FILE:saida_meshopt_export_tests>)

    add_executable(saida_physics_character_tests tests/PhysicsCharacterTest.cpp)
    saida_test_target(saida_physics_character_tests)
    target_link_libraries(saida_physics_character_tests PRIVATE saida_engine)
    add_test(NAME saida_physics_character_tests COMMAND $<TARGET_FILE:saida_physics_character_tests>)

    add_executable(saida_character_controller_tests tests/CharacterControllerTests.cpp)
    saida_test_target(saida_character_controller_tests)
    target_link_libraries(saida_character_controller_tests PRIVATE saida_engine)
    add_test(NAME saida_character_controller_tests COMMAND $<TARGET_FILE:saida_character_controller_tests>)

    add_executable(saida_hostile_asset_tests tests/HostileAssetTests.cpp)
    saida_test_target(saida_hostile_asset_tests)
    target_link_libraries(saida_hostile_asset_tests PRIVATE saida_engine)
    target_compile_options(saida_hostile_asset_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_hostile_asset_tests COMMAND $<TARGET_FILE:saida_hostile_asset_tests>)

    add_executable(saida_physics_query_joint_tests tests/PhysicsQueryJointTests.cpp)
    saida_test_target(saida_physics_query_joint_tests)
    target_link_libraries(saida_physics_query_joint_tests PRIVATE saida_engine)
    target_compile_options(saida_physics_query_joint_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_physics_query_joint_tests COMMAND $<TARGET_FILE:saida_physics_query_joint_tests>)

    add_executable(saida_collision_auto_tests tests/CollisionAutoShapeTest.cpp)
    saida_test_target(saida_collision_auto_tests)
    target_link_libraries(saida_collision_auto_tests PRIVATE saida_engine)
    add_test(NAME saida_collision_auto_tests COMMAND $<TARGET_FILE:saida_collision_auto_tests>)

    add_executable(saida_editor_command_tests tests/EditorCommandTests.cpp)
    saida_test_target(saida_editor_command_tests)
    target_link_libraries(saida_editor_command_tests PRIVATE saida_editor)
    target_compile_options(saida_editor_command_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_editor_command_tests COMMAND $<TARGET_FILE:saida_editor_command_tests>)

    add_executable(saida_editor_document_tests tests/EditorDocumentTests.cpp)
    saida_test_target(saida_editor_document_tests)
    target_link_libraries(saida_editor_document_tests PRIVATE saida_editor)
    target_compile_options(saida_editor_document_tests PRIVATE -Wall -Wextra)
    add_test(NAME saida_editor_document_tests COMMAND $<TARGET_FILE:saida_editor_document_tests>)

    if(SAIDA_ENABLE_MCP)
        add_executable(saida_mcp_tool_registry_tests tests/McpToolRegistryTests.cpp)
        saida_test_target(saida_mcp_tool_registry_tests)
        target_link_libraries(saida_mcp_tool_registry_tests PRIVATE saida_editor)
        target_compile_options(saida_mcp_tool_registry_tests PRIVATE -Wall -Wextra)
        add_test(NAME saida_mcp_tool_registry_tests
                 COMMAND $<TARGET_FILE:saida_mcp_tool_registry_tests>)
    endif()

endif()

# ── Editor executable (the app the Hub launches) ────────────────────────────
# Thin target: just the entry point, linking the editor. Launched directly it
# opens an empty scene; the Hub passes a project via --project to load content.
add_executable(SaidaEngine
    src/main.cpp
)
saida_app_target(SaidaEngine)
target_link_libraries(SaidaEngine PRIVATE saida_editor)
target_compile_options(SaidaEngine PRIVATE -Wall -Wextra)
add_dependencies(SaidaEngine Shaders)

if (WIN32)
    # Copy glfw
    add_custom_command(TARGET SaidaEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:glfw>"
        "$<TARGET_FILE_DIR:SaidaEngine>"
    )
endif()

# ── Standalone game runtime (export template) ───────────────────────────────
# Editor-less player: links saida_engine only (no ImGui/editor). The editor's Build
# step copies this exe next to the packaged game data; it reads game.saida at
# startup and runs the project's main scene. Built once with the engine.
add_executable(SaidaEngineRuntime
    src/runtime/main.cpp
)
saida_app_target(SaidaEngineRuntime)
target_link_libraries(SaidaEngineRuntime PRIVATE saida_engine)
target_compile_options(SaidaEngineRuntime PRIVATE -Wall -Wextra)
add_dependencies(SaidaEngineRuntime Shaders)

if (WIN32)
    add_custom_command(TARGET SaidaEngineRuntime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:glfw>"
        "$<TARGET_FILE_DIR:SaidaEngineRuntime>"
    )
endif()

# Building the editor also builds the runtime template, so the Build button
# always has a fresh SaidaEngineRuntime.exe to package.
add_dependencies(SaidaEngine SaidaEngineRuntime)

# ── Hub executable ──────────────────────────────────────────────────────────
add_executable(SaidaEngineHub
    src/hub/main.cpp
    src/hub/Hub.cpp
)
saida_app_target(SaidaEngineHub)
target_link_libraries(SaidaEngineHub PRIVATE saida_engine)
target_compile_options(SaidaEngineHub PRIVATE -Wall -Wextra)

if (WIN32)
    add_custom_command(TARGET SaidaEngineHub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:glfw>"
        "$<TARGET_FILE_DIR:SaidaEngineHub>"
    )
endif()

# ── Headless tooling CLI ─────────────────────────────────────────────────────
# No window, no GPU: links saida_engine only to reach the authoring-core
# (EngineManifest / SaidaOp). Backbone of the Temporal workers (describe/validate/
# apply-ops). Never opens a Vulkan context — safe to run in CI/headless.
add_executable(saida_tool
    src/cli/saida_tool_main.cpp
    # render-ui drives the same CPU RmlUi backend the UI corpus tests exercise,
    # so a UI document is renderable without a GPU or a window.
    src/cli/RenderUiCommand.cpp
    # export-game reuses the Build button's exporter (no UI dependency).
    src/editor/BuildExporter.cpp
    src/editor/ExeMetadata.cpp
)
saida_app_target(saida_tool)
target_link_libraries(saida_tool PRIVATE saida_engine)
target_compile_options(saida_tool PRIVATE -Wall -Wextra)

if (BUILD_TESTING)
    # The shipped binary must prove that the manifest it emits is coherent and
    # that every announced type round-trips through the durable snapshot codec.
    add_test(
        NAME saida_tool_verify_manifest
        COMMAND $<TARGET_FILE:saida_tool> verify-manifest
    )

    add_test(
        NAME saida_tool_validate_scenario_valid
        COMMAND $<TARGET_FILE:saida_tool> validate-scenario
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scenario-valid.json
    )
    add_test(
        NAME saida_tool_validate_scenario_invalid
        COMMAND $<TARGET_FILE:saida_tool> validate-scenario
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scenario-invalid-action.json
    )
    set_tests_properties(saida_tool_validate_scenario_invalid PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_apply_ops_valid
        COMMAND $<TARGET_FILE:saida_tool> apply-ops
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-snapshot.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ops-apply-valid.json
    )
    add_test(
        NAME saida_tool_apply_ops_invalid
        COMMAND $<TARGET_FILE:saida_tool> apply-ops
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-snapshot.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ops-apply-invalid.json
    )
    set_tests_properties(saida_tool_apply_ops_invalid PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_apply_ops_rejects_unknown_snapshot_type
        COMMAND $<TARGET_FILE:saida_tool> apply-ops
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-snapshot-unknown-node.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ops-empty.json
    )
    set_tests_properties(saida_tool_apply_ops_rejects_unknown_snapshot_type PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_apply_ops_rejects_future_snapshot_schema
        COMMAND $<TARGET_FILE:saida_tool> apply-ops
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-snapshot-future-schema.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ops-empty.json
    )
    set_tests_properties(saida_tool_apply_ops_rejects_future_snapshot_schema PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_apply_ops_rejects_schema_version_conflict
        COMMAND $<TARGET_FILE:saida_tool> apply-ops
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-snapshot-schema-conflict.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ops-empty.json
    )
    set_tests_properties(saida_tool_apply_ops_rejects_schema_version_conflict PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_apply_ops_skip_invalid
        COMMAND $<TARGET_FILE:saida_tool> apply-ops --skip-invalid
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-snapshot.json
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ops-apply-conflict.json
    )

    add_test(
        NAME saida_tool_validate_scene_valid
        COMMAND $<TARGET_FILE:saida_tool> validate-scene
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-snapshot.json
    )
    add_test(
        NAME saida_tool_validate_scene_invalid
        COMMAND $<TARGET_FILE:saida_tool> validate-scene
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/scene-invalid-dupid.json
    )
    set_tests_properties(saida_tool_validate_scene_invalid PROPERTIES WILL_FAIL TRUE)

    # render-ui: a UI document renders without a GPU or a window, and a document
    # RmlUi complained about is rejected rather than rendered into a plausible
    # picture. The PNG lands in the build tree, never in the fixture project.
    add_test(
        NAME saida_tool_render_ui_valid
        COMMAND $<TARGET_FILE:saida_tool> render-ui ui/render-valid.html
                --project ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ui-project
                --size 320x200
                --out ${CMAKE_CURRENT_BINARY_DIR}/render-ui-valid.png
                --layout-json ${CMAKE_CURRENT_BINARY_DIR}/render-ui-valid.json
    )
    add_test(
        NAME saida_tool_render_ui_rejects_malformed
        COMMAND $<TARGET_FILE:saida_tool> render-ui ui/render-malformed.html
                --project ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ui-project
                --size 320x200
                --out ${CMAKE_CURRENT_BINARY_DIR}/render-ui-malformed.png
    )
    set_tests_properties(saida_tool_render_ui_rejects_malformed PROPERTIES WILL_FAIL TRUE)

    # Confinement (SPEC section 6.2): a document path is project-relative, so
    # traversal out of the project is refused before anything is read.
    add_test(
        NAME saida_tool_render_ui_refuses_traversal
        COMMAND $<TARGET_FILE:saida_tool> render-ui ../../../../SPEC.md
                --project ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ui-project
                --out ${CMAKE_CURRENT_BINARY_DIR}/render-ui-escaped.png
    )
    set_tests_properties(saida_tool_render_ui_refuses_traversal PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_validate_script_valid
        COMMAND $<TARGET_FILE:saida_tool> validate-script
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/script-valid.js
    )
    add_test(
        NAME saida_tool_validate_script_invalid
        COMMAND $<TARGET_FILE:saida_tool> validate-script
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/script-invalid.js
    )
    set_tests_properties(saida_tool_validate_script_invalid PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_inspect_anim
        COMMAND $<TARGET_FILE:saida_tool> inspect-anim
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation/two_bone.gltf
    )
    add_test(
        NAME saida_tool_validate_clipview_valid
        COMMAND $<TARGET_FILE:saida_tool> validate-clipview
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation/run_loop.sclip
    )
    add_test(
        NAME saida_tool_validate_clipview_invalid
        COMMAND $<TARGET_FILE:saida_tool> validate-clipview
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation/bad_range.sclip
    )
    set_tests_properties(saida_tool_validate_clipview_invalid PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_validate_sequence_valid
        COMMAND $<TARGET_FILE:saida_tool> validate-sequence
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation/intro.sseq
    )
    add_test(
        NAME saida_tool_validate_sequence_invalid
        COMMAND $<TARGET_FILE:saida_tool> validate-sequence
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation/bad_sequence.sseq
    )
    set_tests_properties(saida_tool_validate_sequence_invalid PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME saida_tool_validate_animgraph_valid
        COMMAND $<TARGET_FILE:saida_tool> validate-animgraph
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation/locomotion.sgraph
    )
    add_test(
        NAME saida_tool_validate_animgraph_invalid
        COMMAND $<TARGET_FILE:saida_tool> validate-animgraph
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animation/bad_graph.sgraph
    )
    set_tests_properties(saida_tool_validate_animgraph_invalid PROPERTIES WILL_FAIL TRUE)
endif()

if (WIN32)
    add_custom_command(TARGET saida_tool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:glfw>"
        "$<TARGET_FILE_DIR:saida_tool>"
    )
endif()
