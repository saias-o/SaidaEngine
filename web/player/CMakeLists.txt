# SaidaEngine web player.
#
# Distinct from the authoring runtime (web/runtime): this target compiles the
# real game loop — SceneTree, full SceneSerializer, gameplay behaviours,
# GLTFLoader — and boots a game.saida package just like the desktop exe.
#
# Build (emsdk activated, from the repo root):
#   emcmake cmake -S web/player -B build-web-player -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-web-player
# Servir : python web/serve.py build-web-player

cmake_minimum_required(VERSION 3.20)
project(SaidaWebPlayer LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This target is Emscripten-only; configure with emcmake.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SAIDA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(GLM_INCLUDE_DIR ${SAIDA_ROOT}/third_party/glm CACHE PATH "Directory containing glm/")

# Jolt in wasm (same options as the desktop build; single-threaded job system
# chosen at compile time in PhysicsWorld).
set(INTERPROCEDURAL_OPTIMIZATION OFF)
set(ENABLE_ALL_WARNINGS OFF)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF)
set(ENABLE_OBJECT_STREAM OFF)
set(ENABLE_INSTALL OFF)
set(CROSS_PLATFORM_DETERMINISTIC OFF)
set(DOUBLE_PRECISION OFF)
add_subdirectory(${SAIDA_ROOT}/third_party/jolt/Build ${CMAKE_BINARY_DIR}/jolt)

# meshopt decoders (EXT_meshopt_compression when loading GLBs).
file(GLOB MESHOPT_SOURCES ${SAIDA_ROOT}/third_party/meshoptimizer/src/*.cpp)

# QuickJS remains the engine's single JavaScript runtime. The player compiles
# it from the same vendored sources as the desktop target.
add_library(quickjs STATIC
    ${SAIDA_ROOT}/third_party/quickjs/dtoa.c
    ${SAIDA_ROOT}/third_party/quickjs/libregexp.c
    ${SAIDA_ROOT}/third_party/quickjs/libunicode.c
    ${SAIDA_ROOT}/third_party/quickjs/quickjs.c
)
target_include_directories(quickjs PUBLIC ${SAIDA_ROOT}/third_party/quickjs)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)

# RmlUi remains the sole UI backend across all platforms. The web player uses
# the same deterministic CPU rasterizer as WebCanvas, then composites the
# resulting texture in the WebGPU pass.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HVF ON CACHE BOOL "" FORCE)
add_subdirectory(${SAIDA_ROOT}/third_party/freetype ${CMAKE_BINARY_DIR}/freetype)
if(NOT TARGET Freetype::Freetype)
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
add_subdirectory(${SAIDA_ROOT}/third_party/rmlui ${CMAKE_BINARY_DIR}/rmlui)

add_executable(saida_player
    main.cpp
    # --- game core (the same game engine as the desktop build) ---
    ${SAIDA_ROOT}/src/core/AtomicFile.cpp
    ${SAIDA_ROOT}/src/core/FileWatcher.cpp
    ${SAIDA_ROOT}/src/core/Input.cpp
    ${SAIDA_ROOT}/src/core/InputProfile.cpp
    ${SAIDA_ROOT}/src/core/Paths.cpp
    ${SAIDA_ROOT}/src/core/PlatformCaps.cpp
    ${SAIDA_ROOT}/src/core/Reflection.cpp
    ${SAIDA_ROOT}/src/core/Profiler.cpp
    ${SAIDA_ROOT}/src/core/Camera.cpp
    ${SAIDA_ROOT}/src/runtime/BootManifest.cpp
    ${SAIDA_ROOT}/src/runtime/RuntimeRoundTripContract.cpp
    ${SAIDA_ROOT}/src/project/AssetRegistry.cpp
    ${SAIDA_ROOT}/src/project/AssetLoader.cpp
    ${SAIDA_ROOT}/src/project/Project.cpp
    ${SAIDA_ROOT}/src/project/PlayerStorage.cpp
    ${SAIDA_ROOT}/src/scene/Behaviour.cpp
    ${SAIDA_ROOT}/src/scene/BehaviourRegistry.cpp
    ${SAIDA_ROOT}/src/scene/Blackboard.cpp
    ${SAIDA_ROOT}/src/behaviours/CameraFollowBehaviour.cpp
    ${SAIDA_ROOT}/src/nodes/CameraNode.cpp
    ${SAIDA_ROOT}/src/behaviours/AnimGraphBehaviour.cpp
    ${SAIDA_ROOT}/src/behaviours/CharacterBehaviour.cpp
    ${SAIDA_ROOT}/src/behaviours/VehicleBehaviour.cpp
    ${SAIDA_ROOT}/src/scene/GLTFLoader.cpp
    ${SAIDA_ROOT}/src/scene/GltfMeshopt.cpp
    ${SAIDA_ROOT}/src/nodes/LightNode.cpp
    ${SAIDA_ROOT}/src/scene/MeshLod.cpp
    ${SAIDA_ROOT}/src/nodes/MeshNode.cpp
    ${SAIDA_ROOT}/src/scene/Node.cpp
    ${SAIDA_ROOT}/src/scene/NodeId.cpp
    ${SAIDA_ROOT}/src/scene/NodeRegistry.cpp
    ${SAIDA_ROOT}/src/nodes/ParticleSystemNode.cpp
    ${SAIDA_ROOT}/src/scene/ReflectedTypesPlayer.cpp
    ${SAIDA_ROOT}/src/behaviours/RotatorBehaviour.cpp
    ${SAIDA_ROOT}/src/scene/Scene.cpp
    ${SAIDA_ROOT}/src/scene/SceneMeasure.cpp
    ${SAIDA_ROOT}/src/scene/SceneSettingsSerialization.cpp
    ${SAIDA_ROOT}/src/scene/SceneSerializer.cpp
    ${SAIDA_ROOT}/src/scene/SceneTree.cpp
    ${SAIDA_ROOT}/src/scene/SceneTimerQueue.cpp
    ${SAIDA_ROOT}/src/scene/SignalWiring.cpp
    ${SAIDA_ROOT}/src/behaviours/SpawnerBehaviour.cpp
    ${SAIDA_ROOT}/src/behaviours/StateMachineBehaviour.cpp
    ${SAIDA_ROOT}/src/scene/Transform.cpp
    ${SAIDA_ROOT}/src/nodes/UINode.cpp
    ${SAIDA_ROOT}/src/nodes/UICanvasNode.cpp
    ${SAIDA_ROOT}/src/nodes/UITextNode.cpp
    ${SAIDA_ROOT}/src/nodes/WaterNode.cpp
    ${SAIDA_ROOT}/src/physics/CollisionObjectNode.cpp
    ${SAIDA_ROOT}/src/physics/CollisionShapeNode.cpp
    ${SAIDA_ROOT}/src/physics/CharacterBodyNode.cpp
    ${SAIDA_ROOT}/src/physics/PhysicsNodes.cpp
    ${SAIDA_ROOT}/src/physics/JointNodes.cpp
    ${SAIDA_ROOT}/src/physics/PhysicsWorld.cpp
    ${SAIDA_ROOT}/src/audio/AudioManager.cpp
    ${SAIDA_ROOT}/src/audio/AudioSourceBehaviour.cpp
    ${SAIDA_ROOT}/src/audio/AudioUsage.cpp
    ${SAIDA_ROOT}/src/scripting/JsContext.cpp
    ${SAIDA_ROOT}/src/scripting/JsEngineBindings.cpp
    ${SAIDA_ROOT}/src/scripting/JsRuntime.cpp
    ${SAIDA_ROOT}/src/scripting/JsTimerBindings.cpp
    ${SAIDA_ROOT}/src/scripting/ScriptBehaviour.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimBlackboard.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimGraphAsset.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimationAssetDecoders.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimStateMachine.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimationClip.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimationSequence.cpp
    ${SAIDA_ROOT}/src/scene/animation/Animator.cpp
    ${SAIDA_ROOT}/src/scene/animation/BlendNode.cpp
    ${SAIDA_ROOT}/src/scene/animation/ClipNode.cpp
    ${SAIDA_ROOT}/src/scene/animation/ClipView.cpp
    ${SAIDA_ROOT}/src/scene/animation/Pose.cpp
    ${SAIDA_ROOT}/src/scene/animation/Retarget.cpp
    ${SAIDA_ROOT}/src/scene/animation/RetargetProfile.cpp
    ${SAIDA_ROOT}/src/scene/animation/Rig.cpp
    ${SAIDA_ROOT}/src/scene/animation/RigAsset.cpp
    ${SAIDA_ROOT}/src/scene/animation/SequenceDirectorBehaviour.cpp
    ${SAIDA_ROOT}/src/cli/AutoLODBridge.cpp
    # --- rendering (same sources as the web viewer) ---
    ${SAIDA_ROOT}/src/graphics/GeometryRegistry.cpp
    ${SAIDA_ROOT}/src/graphics/Material.cpp
    ${SAIDA_ROOT}/src/graphics/Mesh.cpp
    ${SAIDA_ROOT}/src/graphics/Primitives.cpp
    ${SAIDA_ROOT}/src/graphics/BindlessTables.cpp
    ${SAIDA_ROOT}/src/graphics/GpuGraveyard.cpp
    ${SAIDA_ROOT}/src/graphics/GpuBudget.cpp
    ${SAIDA_ROOT}/src/graphics/MeshCache.cpp
    ${SAIDA_ROOT}/src/graphics/TextureCache.cpp
    ${SAIDA_ROOT}/src/graphics/ResourceManager.cpp
    ${SAIDA_ROOT}/src/graphics/ShadowMap.cpp
    ${SAIDA_ROOT}/src/graphics/StbImageUsage.cpp
    ${SAIDA_ROOT}/src/graphics/TinyObjUsage.cpp
    ${SAIDA_ROOT}/src/graphics/UIRenderer.cpp
    ${SAIDA_ROOT}/src/graphics/cgltf_impl.cpp
    ${SAIDA_ROOT}/src/ui/HudRasterizer.cpp
    ${SAIDA_ROOT}/src/ui/RmlUiRenderInterface.cpp
    ${SAIDA_ROOT}/src/ui/RmlUiRuntime.cpp
    ${SAIDA_ROOT}/src/fx/ParticleEffect.cpp
    ${SAIDA_ROOT}/src/fx/ParticlePresetLibrary.cpp
    ${SAIDA_ROOT}/src/fx/ParticleQuality.cpp
    ${SAIDA_ROOT}/src/fx/ParticleRuntime.cpp
    ${SAIDA_ROOT}/src/render/CameraDirector.cpp
    ${SAIDA_ROOT}/src/render/GIVolume.cpp
    ${SAIDA_ROOT}/src/render/PostProcessor.cpp
    ${SAIDA_ROOT}/src/render/TonemapPass.cpp
    ${SAIDA_ROOT}/src/render/RenderFeatureRegistry.cpp
    ${SAIDA_ROOT}/src/render/Renderer.cpp
    ${SAIDA_ROOT}/src/render/EnvironmentSH.cpp
    ${SAIDA_ROOT}/src/render/features/ParticleFeature.cpp
    ${SAIDA_ROOT}/src/render/features/SkyboxFeature.cpp
    ${SAIDA_ROOT}/src/render/features/WaterFeature.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/BindGroup.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Buffer.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/CommandEncoder.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Device.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Pipeline.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/RenderTexture.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Sampler.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Surface.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Texture.cpp
    ${MESHOPT_SOURCES}
)

target_include_directories(saida_player PRIVATE
    ${SAIDA_ROOT}/src
    ${GLM_INCLUDE_DIR}
    ${SAIDA_ROOT}/third_party/json
    ${SAIDA_ROOT}/third_party/stb
    ${SAIDA_ROOT}/third_party/tinyobjloader
    ${SAIDA_ROOT}/third_party/cgltf
    ${SAIDA_ROOT}/third_party/meshoptimizer/src
    ${SAIDA_ROOT}/third_party/quickjs
    ${SAIDA_ROOT}/third_party/jolt/Build/..
    ${SAIDA_ROOT}/third_party/miniaudio
)
target_compile_definitions(saida_player PRIVATE
    SAIDA_RHI_WEBGPU SAIDA_SCENE_TREE_TIMERS
    GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE
    SAIDA_PROJECT_ROOT="/" SAIDA_SHADER_DIR="/shaders" SAIDA_RUNTIME_DIR="/")
target_compile_options(saida_player PRIVATE -fexceptions $<$<CONFIG:Release>:-Oz> --use-port=emdawnwebgpu)
target_link_libraries(saida_player PRIVATE quickjs Jolt RmlUi::RmlUi)

target_link_options(saida_player PRIVATE
    --use-port=emdawnwebgpu
    -sUSE_GLFW=3
    -sINITIAL_MEMORY=512MB
    -sSTACK_SIZE=4MB
    -lidbfs.js
    -sEXIT_RUNTIME=0
    -sDISABLE_EXCEPTION_CATCHING=0
    -sEXPORTED_RUNTIME_METHODS=ccall
    -sEXPORTED_FUNCTIONS=_main,_saida_player_status
    $<$<CONFIG:Release>:-Oz>
    "SHELL:--preload-file \"${SAIDA_ROOT}/build/shaders/wgsl@/shaders\""
    # Engine default fonts, at the canonical path assets/fonts/ (same list as
    # RmlUiRuntime::kEngineFonts; NotoEmoji intentionally left out of the web bundle).
    "SHELL:--preload-file \"${SAIDA_ROOT}/third_party/rmlui/Samples/assets/LatoLatin-Regular.ttf@/assets/assets/fonts/LatoLatin-Regular.ttf\""
    "SHELL:--preload-file \"${SAIDA_ROOT}/third_party/rmlui/Samples/assets/LatoLatin-Bold.ttf@/assets/assets/fonts/LatoLatin-Bold.ttf\""
    "SHELL:--preload-file \"${SAIDA_ROOT}/third_party/rmlui/Samples/assets/RobotoMono-Regular.ttf@/assets/assets/fonts/RobotoMono-Regular.ttf\""
    # The reference game package, mounted as-is under /project.
    -sFORCE_FILESYSTEM=1
    "SHELL:--shell-file \"${CMAKE_CURRENT_SOURCE_DIR}/shell.html\""
)

file(GLOB WEB_WGSL_SHADERS ${SAIDA_ROOT}/build/shaders/wgsl/*.wgsl)
set_property(TARGET saida_player APPEND PROPERTY LINK_DEPENDS
    ${WEB_WGSL_SHADERS} ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)

set_target_properties(saida_player PROPERTIES SUFFIX ".html" OUTPUT_NAME "index")
