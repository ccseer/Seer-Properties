cmake_minimum_required(VERSION 3.21)
project(sha256_property LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The helper has no Qt dependency, so it can link the static C runtime and ship
# as a single self-contained executable without a redistributable requirement.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
enable_testing()

set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# Vendored third-party hash sources: the BLAKE3 reference implementation is
# compiled in its portable configuration (every SIMD dispatch path disabled),
# next to the compact SHA-3 (Keccak) reference implementation.
add_library(sha256_vendored_c STATIC
    ${THIRD_PARTY_DIR}/blake3/blake3.c
    ${THIRD_PARTY_DIR}/blake3/blake3_dispatch.c
    ${THIRD_PARTY_DIR}/blake3/blake3_portable.c
    ${THIRD_PARTY_DIR}/sha3/sha3.c
)
target_include_directories(sha256_vendored_c PUBLIC
    ${THIRD_PARTY_DIR}/blake3
    ${THIRD_PARTY_DIR}/sha3
)
target_compile_definitions(sha256_vendored_c PRIVATE
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
)

add_library(sha256_core STATIC
    src/sha256.cpp
    src/hashing.cpp
)
target_include_directories(sha256_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# One root for every vendored header: <nlohmann/json.hpp>, "blake3.h",
# "sha3.h" and "xxhash.h".
target_include_directories(sha256_core PUBLIC ${THIRD_PARTY_DIR})
# xxHash is used header-only with its implementation emitted inside
# src/hashing.cpp, so only the include path is needed here.
target_include_directories(sha256_core PUBLIC ${THIRD_PARTY_DIR}/xxhash)
target_link_libraries(sha256_core PUBLIC bcrypt sha256_vendored_c)

add_executable(sha256_property src/main.cpp)
target_link_libraries(sha256_property PRIVATE sha256_core shell32)

add_executable(sha256_property_test test/sha256_test.cpp)
target_link_libraries(sha256_property_test PRIVATE sha256_core)
add_test(NAME sha256_property_test COMMAND sha256_property_test)

add_executable(sha256_property_manifest_test test/manifest_test.cpp)

# Stages plugin.json, the helper and PACKAGE_README.md (as README.md) so the
# manifest test and the installed package both cover the shipped layout.
set(SHA256_PROPERTY_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/PackageStaging.cmake)
seer_property_package_staging(
    PACKAGE_ID io.1218.seer.sha256-property
    PACKAGE_NAME "File Hashes"
    COMMAND_NAME sha256_property.exe
    COMMAND_TARGET sha256_property
    TEST_TARGET sha256_property_manifest_test
    STAGE_TARGET sha256_property_manifest_stage
    PACKAGE_TARGET sha256_property_package
    STAGING_DIR ${SHA256_PROPERTY_MANIFEST_STAGING_DIR}
)
