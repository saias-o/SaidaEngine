# SaidaEngine web runtime target (Step 16.4/16.6).
#
# Build (emsdk activated, from the repo root):
#   ./web/build_web.sh                                   # wraps the two lines below
#   emcmake cmake -S web/runtime -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-web
# Serve with COOP/COEP headers:  python web/serve.py build-web
#
# Requires the WGSL shaders: cmake --build build --target WebShaders (desktop tree).

cmake_minimum_required(VERSION 3.20)
project(SaidaWebRuntime LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This target is Emscripten-only; configure with emcmake.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SAIDA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# glm is vendored (third_party/glm) so the web build never leaks the desktop
# toolchain's system include directory into the Emscripten sysroot.
set(GLM_INCLUDE_DIR ${SAIDA_ROOT}/third_party/glm CACHE PATH "Directory containing glm/")

# Renderer now owns the same RmlUi-backed UI compositor as the game player.
# Keep the authoring runtime link-complete and able to preview the durable V1
# UICanvas/UIText subset instead of compiling a renderer with missing symbols.
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

add_executable(saida_web
    main.cpp
    # saida_apply_op needs the authoring core in the web editor runtime.
    ${SAIDA_ROOT}/src/authoring/EngineManifest.cpp
    ${SAIDA_ROOT}/src/authoring/SceneSnapshot.cpp
    ${SAIDA_ROOT}/src/authoring/SaidaOp.cpp
    ${SAIDA_ROOT}/src/authoring/SaidaOpApplier.cpp
    ${SAIDA_ROOT}/src/core/Reflection.cpp
    ${SAIDA_ROOT}/src/core/Profiler.cpp
    # Needed since UI documents resolve their assets against the loaded project
    # (Paths::projectContentPath), which RmlUiRenderInterface below calls.
    ${SAIDA_ROOT}/src/core/Paths.cpp
    ${SAIDA_ROOT}/src/core/Camera.cpp
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
    ${SAIDA_ROOT}/src/fx/ParticleEffect.cpp
    ${SAIDA_ROOT}/src/fx/ParticlePresetLibrary.cpp
    ${SAIDA_ROOT}/src/fx/ParticleQuality.cpp
    ${SAIDA_ROOT}/src/fx/ParticleRuntime.cpp
    ${SAIDA_ROOT}/src/project/AssetRegistry.cpp
    ${SAIDA_ROOT}/src/project/AssetLoader.cpp
    ${SAIDA_ROOT}/src/render/GIVolume.cpp
    ${SAIDA_ROOT}/src/render/RenderFeatureRegistry.cpp
    ${SAIDA_ROOT}/src/render/features/ParticleFeature.cpp
    ${SAIDA_ROOT}/src/render/features/SkyboxFeature.cpp
    ${SAIDA_ROOT}/src/render/features/WaterFeature.cpp
    ${SAIDA_ROOT}/src/render/PostProcessor.cpp
    ${SAIDA_ROOT}/src/render/TonemapPass.cpp
    ${SAIDA_ROOT}/src/render/Renderer.cpp
    ${SAIDA_ROOT}/src/render/EnvironmentSH.cpp
    ${SAIDA_ROOT}/src/runtime/RuntimeRoundTripContract.cpp
    ${SAIDA_ROOT}/src/scenario/ScenarioRegistry.cpp
    ${SAIDA_ROOT}/src/scene/Behaviour.cpp
    ${SAIDA_ROOT}/src/scene/BehaviourRegistry.cpp
    ${SAIDA_ROOT}/src/nodes/CameraNode.cpp
    ${SAIDA_ROOT}/src/nodes/LightNode.cpp
    ${SAIDA_ROOT}/src/scene/MeshLod.cpp
    ${SAIDA_ROOT}/src/nodes/MeshNode.cpp
    ${SAIDA_ROOT}/src/scene/Node.cpp
    ${SAIDA_ROOT}/src/scene/NodeId.cpp
    # create_node op (SaidaOpApplier) builds reflected nodes by name via the
    # NodeRegistry; ReflectedTypesWeb.cpp registers the web-renderable subset.
    ${SAIDA_ROOT}/src/scene/NodeRegistry.cpp
    ${SAIDA_ROOT}/src/scene/ReflectedTypesWeb.cpp
    ${SAIDA_ROOT}/src/nodes/ParticleSystemNode.cpp
    ${SAIDA_ROOT}/src/scene/Scene.cpp
    ${SAIDA_ROOT}/src/scene/SceneSettingsSerialization.cpp
    ${SAIDA_ROOT}/src/scene/Transform.cpp
    ${SAIDA_ROOT}/src/nodes/UINode.cpp
    ${SAIDA_ROOT}/src/nodes/UICanvasNode.cpp
    ${SAIDA_ROOT}/src/nodes/UITextNode.cpp
    ${SAIDA_ROOT}/src/nodes/WaterNode.cpp
    ${SAIDA_ROOT}/src/ui/HudRasterizer.cpp
    ${SAIDA_ROOT}/src/ui/RmlUiRenderInterface.cpp
    ${SAIDA_ROOT}/src/ui/RmlUiRuntime.cpp
    # ResourceManager loads animation assets (.sclip/.sgraph): their types
    # and the playback chain they reference must link too.
    ${SAIDA_ROOT}/src/scene/animation/AnimBlackboard.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimGraphAsset.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimationAssetDecoders.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimStateMachine.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimationClip.cpp
    ${SAIDA_ROOT}/src/scene/animation/Animator.cpp
    ${SAIDA_ROOT}/src/scene/animation/Blend1DNode.cpp
    ${SAIDA_ROOT}/src/scene/animation/ClipNode.cpp
    ${SAIDA_ROOT}/src/scene/animation/LayerNode.cpp
    ${SAIDA_ROOT}/src/scene/animation/AnimationProgram.cpp
    ${SAIDA_ROOT}/src/scene/animation/ClipView.cpp
    ${SAIDA_ROOT}/src/scene/animation/CookedClip.cpp
    ${SAIDA_ROOT}/src/scene/animation/Pose.cpp
    ${SAIDA_ROOT}/src/scene/animation/Retarget.cpp
    ${SAIDA_ROOT}/src/scene/animation/RetargetProfile.cpp
    ${SAIDA_ROOT}/src/scene/animation/Rig.cpp
    ${SAIDA_ROOT}/src/scene/animation/RigAsset.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/BindGroup.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Buffer.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/CommandEncoder.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Device.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Pipeline.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/RenderTexture.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Sampler.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Surface.cpp
    ${SAIDA_ROOT}/src/rhi/webgpu/Texture.cpp
)

target_include_directories(saida_web PRIVATE
    ${SAIDA_ROOT}/src
    ${GLM_INCLUDE_DIR}
    ${SAIDA_ROOT}/third_party/json
    ${SAIDA_ROOT}/third_party/stb
    ${SAIDA_ROOT}/third_party/tinyobjloader
)
# SAIDA_NO_PHYSICS / SAIDA_NO_AUDIO: the authoring viewer compiles neither
# Jolt nor miniaudio (the game player does).
target_compile_definitions(saida_web PRIVATE SAIDA_RHI_WEBGPU SAIDA_NO_PHYSICS SAIDA_NO_AUDIO
    SAIDA_AUTHORING_WEB GLM_FORCE_RADIANS GLM_FORCE_DEPTH_ZERO_TO_ONE
    SAIDA_PROJECT_ROOT="/" SAIDA_SHADER_DIR="/shaders" SAIDA_RUNTIME_DIR="/")

# Size-conscious flags (16.6): -Oz in release, plus link-time stripping.
target_compile_options(saida_web PRIVATE -fexceptions $<$<CONFIG:Release>:-Oz>)
target_link_libraries(saida_web PRIVATE RmlUi::RmlUi)

target_link_options(saida_web PRIVATE
    --use-port=emdawnwebgpu
    -sUSE_GLFW=3
    # FIXED memory (no ALLOW_MEMORY_GROWTH): recent Chrome builds refuse views
    # on a resizable ArrayBuffer in writeTexture/writeBuffer ("must not be
    # resizable"), and the bundled emdawnwebgpu port doesn't copy around this yet.
    # 512 MB comfortably covers current web editing scenes.
    -sINITIAL_MEMORY=512MB
    -sEXIT_RUNTIME=0
    # The JSON authoring boundary reports malformed snapshots as structured
    # errors. Without exception catching, nlohmann/type conversion throws abort
    # the entire WASM instance before saida_load_snapshot can roll back.
    -sDISABLE_EXCEPTION_CATCHING=0
    # Spike S0: expose ccall/cwrap to drive saida_apply_op from JS.
    # FS (Track 1-F): the platform writes project files (obj/textures/
    # asset_registry.json) into MEMFS under /project before saida_load_snapshot.
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_saida_runtime_status,_saida_apply_op,_saida_engine_manifest,_saida_scene_snapshot,_saida_scene_snapshot_compare,_saida_load_snapshot,_saida_pick,_saida_set_render_settings,_saida_camera_state
    $<$<CONFIG:Release>:-Oz>
    # WGSL shaders packaged into MEMFS at /shaders (16.1 web FS: preload).
    # Paths are quoted so a checkout under a directory with spaces still links.
    "SHELL:--preload-file \"${SAIDA_ROOT}/build/shaders/wgsl@/shaders\""
    # Engine default fonts, at the canonical path assets/fonts/ (same list as
    # RmlUiRuntime::kEngineFonts; NotoEmoji intentionally left out of the web bundle).
    "SHELL:--preload-file \"${SAIDA_ROOT}/third_party/rmlui/Samples/assets/LatoLatin-Regular.ttf@/assets/assets/fonts/LatoLatin-Regular.ttf\""
    "SHELL:--preload-file \"${SAIDA_ROOT}/third_party/rmlui/Samples/assets/LatoLatin-Bold.ttf@/assets/assets/fonts/LatoLatin-Bold.ttf\""
    "SHELL:--preload-file \"${SAIDA_ROOT}/third_party/rmlui/Samples/assets/RobotoMono-Regular.ttf@/assets/assets/fonts/RobotoMono-Regular.ttf\""
    # BeachDemo scene + sky texture, mapped to /project (parity-test scene).
    "SHELL:--preload-file \"${SAIDA_ROOT}/BeachDemo/scenes/beach.scene@/project/beach.scene\""
    "SHELL:--preload-file \"${SAIDA_ROOT}/BeachDemo/assets/skies/sunset.png@/project/sunset.png\""
    "SHELL:--shell-file \"${CMAKE_CURRENT_SOURCE_DIR}/shell.html\""
)
target_compile_options(saida_web PRIVATE --use-port=emdawnwebgpu)

# The link step embeds the WGSL shaders + BeachDemo assets (index.data) and the
# shell page: relink when any of them change, not just when a .cpp does.
file(GLOB WEB_WGSL_SHADERS ${SAIDA_ROOT}/build/shaders/wgsl/*.wgsl)
set_property(TARGET saida_web APPEND PROPERTY LINK_DEPENDS
    ${WEB_WGSL_SHADERS} ${CMAKE_CURRENT_SOURCE_DIR}/shell.html
    ${SAIDA_ROOT}/BeachDemo/scenes/beach.scene
    ${SAIDA_ROOT}/BeachDemo/assets/skies/sunset.png)

set_target_properties(saida_web PROPERTIES SUFFIX ".html" OUTPUT_NAME "index")
