cmake_minimum_required(VERSION 3.21)
project(pdb_property LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The helper has no Qt dependency, so it can link the static C runtime and
# ship as a single self-contained executable without a redistributable
# requirement.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
enable_testing()

set(NLOHMANN_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
# Header-only helpers shared with the other non-Qt packages.
set(PLUGIN_COMMON_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)
# The vendored raw_pdb parsing engine (BSD 2-Clause, see
# ../third_party/rawpdb/README.md for the pinned revision and caveats).
set(RAWPDB_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rawpdb/src)

add_library(rawpdb STATIC
    ${RAWPDB_INCLUDE_DIR}/PDB.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_CoalescedMSFStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_DBIStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_DBITypes.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_DirectMSFStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_GlobalSymbolStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_ImageSectionStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_InfoStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_IPIStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_ModuleInfoStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_ModuleLineStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_ModuleSymbolStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_NamesStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_PCH.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_PublicSymbolStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_RawFile.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_SectionContributionStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_SourceFileStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_TPIStream.cpp
    ${RAWPDB_INCLUDE_DIR}/PDB_Types.cpp
)
target_include_directories(rawpdb PUBLIC ${RAWPDB_INCLUDE_DIR})

add_library(pdb_property_core STATIC
    src/pdbformat.cpp
    src/pdbquery.cpp
    src/pdbhelper.cpp
)
target_include_directories(pdb_property_core PUBLIC
    src
    ${NLOHMANN_INCLUDE_DIR}
    ${PLUGIN_COMMON_INCLUDE_DIR}
)
target_link_libraries(pdb_property_core PUBLIC rawpdb)

add_executable(pdb_property
    src/main.cpp
)
target_link_libraries(pdb_property PRIVATE pdb_property_core)

add_executable(pdb_property_format_test
    test/pdbformat_test.cpp
)
target_link_libraries(pdb_property_format_test PRIVATE pdb_property_core)
add_test(NAME pdb_property_format_test COMMAND pdb_property_format_test)

# The query and helper tests run against the committed fixtures in
# test/fixtures (see fixtures/README.md for their provenance).
add_executable(pdb_property_query_test
    test/pdbquery_test.cpp
)
target_link_libraries(pdb_property_query_test PRIVATE pdb_property_core)
add_test(NAME pdb_property_query_test
    COMMAND pdb_property_query_test
        "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")

add_executable(pdb_property_helper_test
    test/pdbhelper_test.cpp
)
target_link_libraries(pdb_property_helper_test PRIVATE pdb_property_core)
add_test(NAME pdb_property_helper_test
    COMMAND pdb_property_helper_test
        "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")

add_executable(pdb_property_manifest_test test/manifest_test.cpp)
target_include_directories(pdb_property_manifest_test PRIVATE
    src
    ${NLOHMANN_INCLUDE_DIR})

# Stages plugin.json, the helper and PACKAGE_README.md (as README.md) so the
# manifest test and the installed package both cover the shipped layout.
set(PDB_PROPERTY_MANIFEST_STAGING_DIR ${CMAKE_CURRENT_BINARY_DIR}/manifest-stage)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/PackageStaging.cmake)
seer_property_package_staging(
    PACKAGE_ID io.1218.seer.pdb
    PACKAGE_NAME "PDB"
    COMMAND_NAME pdb_property.exe
    COMMAND_TARGET pdb_property
    TEST_TARGET pdb_property_manifest_test
    STAGE_TARGET pdb_property_manifest_stage
    PACKAGE_TARGET pdb_property_package
    STAGING_DIR ${PDB_PROPERTY_MANIFEST_STAGING_DIR}
)
