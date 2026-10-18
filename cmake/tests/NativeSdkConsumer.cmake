function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Native SDK consumer failed (${code}): ${out}\n${err}")
    endif()
endfunction()

set(consumer "${BINARY_DIR}/tests/Native SDK & project")
file(MAKE_DIRECTORY "${consumer}")
file(COPY "${SOURCE_DIR}/templates/native-game/" DESTINATION "${consumer}")
run("${CMAKE_COMMAND}" -S "${consumer}" -B "${consumer}/build" -G Ninja
    "-DCMAKE_BUILD_TYPE=${CONFIGURATION}" "-DSaidaEngine_DIR=${BINARY_DIR}/sdk"
    "-DCMAKE_CXX_COMPILER=${COMPILER}")
run("${CMAKE_COMMAND}" --build "${consumer}/build" --target MyGame_package --parallel 2)
run("${CMAKE_COMMAND}" --build "${consumer}/build" --target MyGame_editor --parallel 2)
if(NOT EXISTS "${consumer}/build/package/game.saida")
    message(FATAL_ERROR "Native project was not packaged")
endif()
# A cached SDK cannot silently reuse a configuration with a different Jolt ABI.
if(CONFIGURATION STREQUAL "Debug")
    set(other Release)
else()
    set(other Debug)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${consumer}" -B "${consumer}/incompatible" -G Ninja
    "-DCMAKE_BUILD_TYPE=${other}" "-DSaidaEngine_DIR=${BINARY_DIR}/sdk"
    "-DCMAKE_CXX_COMPILER=${COMPILER}"
    RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(code EQUAL 0 OR NOT "${out}${err}" MATCHES "same CMAKE_BUILD_TYPE")
    message(FATAL_ERROR "SDK configuration mismatch was not diagnosed: ${out}\n${err}")
endif()
