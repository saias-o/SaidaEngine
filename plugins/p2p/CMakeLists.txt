cmake_minimum_required(VERSION 3.20)
project(SaidaP2P LANGUAGES C CXX)

# A game adds this directory (add_subdirectory) and links Saida::P2P. It uses
# the engine's mbedTLS: the SDK's Saida::mbedcrypto, an in-tree mbedcrypto
# target, or, built alone, the engine's third_party copy.
set(SAIDA_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
if(TARGET Saida::mbedcrypto)
    set(_saida_p2p_crypto Saida::mbedcrypto)
elseif(TARGET mbedcrypto)
    set(_saida_p2p_crypto mbedcrypto)
else()
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    add_subdirectory("${SAIDA_ENGINE_ROOT}/third_party/mbedtls" "${CMAKE_BINARY_DIR}/p2p-mbedtls" EXCLUDE_FROM_ALL)
    set(_saida_p2p_crypto mbedcrypto)
endif()

add_library(saida_p2p STATIC
    src/Crypto.cpp
    src/Dht.cpp
    src/Host.cpp
    src/NetworkThread.cpp
    src/Rendezvous.cpp
    src/Socket.cpp)
add_library(Saida::P2P ALIAS saida_p2p)
target_compile_features(saida_p2p PUBLIC cxx_std_17)
target_include_directories(saida_p2p PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../netcode/include")
target_link_libraries(saida_p2p PRIVATE ${_saida_p2p_crypto})
find_package(Threads REQUIRED)
target_link_libraries(saida_p2p PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(saida_p2p PUBLIC ws2_32 bcrypt winmm)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(saida_p2p PRIVATE -Wall -Wextra)
endif()

option(SAIDA_P2P_BUILD_TESTS "Build the p2p qualification and the live probe" ${PROJECT_IS_TOP_LEVEL})
if(SAIDA_P2P_BUILD_TESTS)
    enable_testing()
    add_executable(saida_p2p_tests tests/test_p2p.cpp)
    target_link_libraries(saida_p2p_tests PRIVATE Saida::P2P)
    add_test(NAME saida_p2p_tests COMMAND saida_p2p_tests)
    add_executable(p2p_probe examples/p2p_probe.cpp)
    target_link_libraries(p2p_probe PRIVATE Saida::P2P)
endif()
