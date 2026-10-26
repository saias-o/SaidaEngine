cmake_minimum_required(VERSION 3.20)
project(SaidaXRGesture LANGUAGES CXX)

if(NOT TARGET glm::glm)
    find_package(glm CONFIG REQUIRED)
endif()

add_library(saida_xrgesture INTERFACE)
add_library(Saida::XRGesture ALIAS saida_xrgesture)
target_compile_features(saida_xrgesture INTERFACE cxx_std_17)
target_include_directories(saida_xrgesture INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
target_link_libraries(saida_xrgesture INTERFACE glm::glm)

option(SAIDA_XRGESTURE_BUILD_TESTS "Build headless XRGesture contracts" ${PROJECT_IS_TOP_LEVEL})
if(SAIDA_XRGESTURE_BUILD_TESTS)
    enable_testing()
    add_executable(saida_xrgesture_tests tests/GestureTests.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../../src/xr/toolkit/XRInput.cpp")
    target_link_libraries(saida_xrgesture_tests PRIVATE Saida::XRGesture)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(saida_xrgesture_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    endif()
    add_test(NAME saida_xrgesture_tests COMMAND saida_xrgesture_tests)
    if(WIN32)
        find_package(Vulkan REQUIRED)
        add_executable(saida_xrgesture_openxr_tests tests/OpenXRTests.cpp
            "${CMAKE_CURRENT_SOURCE_DIR}/../../src/xr/XrInstance.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../src/xr/XrInputExtensions.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../src/xr/XrActions.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../src/xr/toolkit/XRInput.cpp")
        target_link_libraries(saida_xrgesture_openxr_tests PRIVATE Saida::XRGesture Vulkan::Vulkan)
        target_include_directories(saida_xrgesture_openxr_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openxr/include")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(saida_xrgesture_openxr_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        endif()
        add_test(NAME saida_xrgesture_openxr_tests COMMAND saida_xrgesture_openxr_tests)
    endif()
endif()
