cmake_minimum_required(VERSION 3.20)
project(AutoLOD LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# --- meshoptimizer : decimation industrielle (MIT) ---
FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG        v0.22
  GIT_SHALLOW    TRUE
)

# --- tinygltf : lecture/ecriture glTF & GLB (MIT) ---
FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG        v2.9.3
  GIT_SHALLOW    TRUE
)

# --- xatlas : re-depliage UV automatique pour les proxy LOD (MIT) ---
FetchContent_Declare(
  xatlas
  GIT_REPOSITORY https://github.com/jpcy/xatlas.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATOR_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_BUILDER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_HEADER_ONLY           ON  CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL               OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(meshoptimizer tinygltf xatlas)

# xatlas n'a pas de CMake : on compile son unique source nous-memes
add_library(xatlas STATIC ${xatlas_SOURCE_DIR}/source/xatlas/xatlas.cpp)
target_include_directories(xatlas PUBLIC ${xatlas_SOURCE_DIR}/source/xatlas)
find_package(Threads REQUIRED)
target_link_libraries(xatlas PUBLIC Threads::Threads)

add_executable(autolod src/main.cpp src/baker.cpp)
target_link_libraries(autolod PRIVATE meshoptimizer xatlas)
target_include_directories(autolod PRIVATE ${tinygltf_SOURCE_DIR})

if(MSVC)
  target_compile_options(autolod PRIVATE /O2 /EHsc /bigobj)
else()
  target_compile_options(autolod PRIVATE -O2)
  if(WIN32)
    target_compile_options(autolod PRIVATE -Wa,-mbig-obj)
  endif()
endif()
