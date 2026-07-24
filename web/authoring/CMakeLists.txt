# Minimal headless WASM build of the authoring-core validation. Built with
# Emscripten and loaded in-process by the
# Node Collaboration Gateway. No GPU, no scene, no reflection — just SaidaOp.
#
#   ./web/build_authoring_wasm.sh
#   -> build-authoring-wasm/saida_authoring.mjs + .wasm  (ES6 module, Node+web)

cmake_minimum_required(VERSION 3.20)
project(saida_authoring_wasm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SAIDA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Emit an .mjs so Node treats the glue as an ES module (it uses import.meta.url)
# regardless of the consuming package's "type" field.
set(CMAKE_EXECUTABLE_SUFFIX ".mjs")

add_executable(saida_authoring
    ${CMAKE_CURRENT_SOURCE_DIR}/authoring_wasm.cpp
    ${SAIDA_ROOT}/src/authoring/SaidaOp.cpp
)

target_include_directories(saida_authoring PRIVATE
    ${SAIDA_ROOT}/src
    ${SAIDA_ROOT}/third_party/json
)

target_compile_options(saida_authoring PRIVATE -Os -Wall -Wextra)

# ES6 module usable from Node (and the browser). No main() → --no-entry; keep the
# reactor runtime alive so exported functions can be called after instantiation.
target_link_options(saida_authoring PRIVATE
    --no-entry
    -Os
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sENVIRONMENT=node,web
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
    -sEXPORTED_FUNCTIONS=_saida_validate_op,_saida_known_op_types,_saida_op_version,_malloc,_free
)

set_target_properties(saida_authoring PROPERTIES OUTPUT_NAME "saida_authoring")
