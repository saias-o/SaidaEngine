# Source/build-tree SDK. Consumers use real CMake usage requirements, including
# the transitive Jolt ABI, rather than reverse-engineering a generator's output.
set_target_properties(saida_engine PROPERTIES EXPORT_NAME Engine)
set_target_properties(saida_editor PROPERTIES EXPORT_NAME Editor)
target_compile_features(saida_engine PUBLIC cxx_std_17)
add_library(Saida::Engine ALIAS saida_engine)

set(_saida_sdk_targets saida_engine saida_editor quickjs meshoptimizer Jolt
    rmlui rmlui_core rmlui_debugger freetype libcurl_static
    mbedtls mbedx509 mbedcrypto)
foreach(_optional openxr_loader headers everest p256m)
    if(TARGET ${_optional})
        get_target_property(_imported ${_optional} IMPORTED)
        if(NOT _imported)
            list(APPEND _saida_sdk_targets ${_optional})
        endif()
    endif()
endforeach()
set(SAIDA_SDK_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
set(SAIDA_SDK_BUILD_DIR "${CMAKE_BINARY_DIR}")
set(SAIDA_SDK_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(SAIDA_SDK_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(SAIDA_SDK_POINTER_SIZE "${CMAKE_SIZEOF_VOID_P}")
set(SAIDA_SDK_CONFIGURATION "${CMAKE_BUILD_TYPE}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
export(TARGETS ${_saida_sdk_targets} NAMESPACE Saida::
    FILE "${CMAKE_BINARY_DIR}/sdk/SaidaEngineTargets.cmake")
configure_file("${CMAKE_CURRENT_LIST_DIR}/SaidaEngineConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/sdk/SaidaEngineConfig.cmake" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/SaidaGame.cmake"
    "${CMAKE_BINARY_DIR}/sdk/SaidaGame.cmake" COPYONLY)
if(BUILD_TESTING)
    add_test(NAME saida_native_sdk_consumer
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DBINARY_DIR=${CMAKE_BINARY_DIR}" "-DCONFIGURATION=${CMAKE_BUILD_TYPE}"
            "-DCOMPILER=${CMAKE_CXX_COMPILER}"
            -P "${CMAKE_CURRENT_LIST_DIR}/tests/NativeSdkConsumer.cmake")
    set_tests_properties(saida_native_sdk_consumer PROPERTIES TIMEOUT 180)
endif()
