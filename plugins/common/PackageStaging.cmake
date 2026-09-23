# Shared staging and install-time contract validation for the native Property
# packages. Every package stages the manifest (plugin.json), one helper
# executable, and the user-facing README.md (staged/renamed from
# PACKAGE_README.md).
#
# Usage in a package CMakeLists.txt (after the manifest test executable has
# been declared with its own includes and dependencies):
#
#   include(${PLUGIN_COMMON_INCLUDE_DIR}/PackageStaging.cmake)
#   seer_property_package_staging(
#       PACKAGE_ID io.1218.seer.git-info
#       PACKAGE_NAME "Git Info"
#       COMMAND_NAME git_info.exe
#       COMMAND_TARGET git_info
#       TEST_TARGET git_info_manifest_test
#       STAGE_TARGET git_info_manifest_stage
#       PACKAGE_TARGET git_info_package
#       STAGING_DIR ${GIT_INFO_MANIFEST_STAGING_DIR})
#
# The manifest command is accepted either at the top level (the flat
# command/arguments form) or under invocations.property, because both shapes are
# valid Canonical v1 Property manifests in this repository.
#
# PACKAGE_README.md documents what the plugin does and every option its helper
# accepts; it is what the user finds next to plugin.json inside the shipped ZIP.
# Update it in the same change that adds, removes, renames or re-defaults an
# option.

include_guard(GLOBAL)

function(seer_property_package_staging)
    set(oneValueArgs
        PACKAGE_ID
        PACKAGE_NAME
        COMMAND_NAME
        COMMAND_TARGET
        TEST_TARGET
        STAGE_TARGET
        PACKAGE_TARGET
        STAGING_DIR
    )
    cmake_parse_arguments(PKG "" "${oneValueArgs}" "" ${ARGN})

    foreach(_required
            PACKAGE_ID PACKAGE_NAME COMMAND_NAME COMMAND_TARGET
            TEST_TARGET STAGE_TARGET PACKAGE_TARGET STAGING_DIR)
        if(NOT PKG_${_required})
            message(FATAL_ERROR "seer_property_package_staging: missing ${_required}")
        endif()
    endforeach()

    set(_package_readme "${CMAKE_CURRENT_SOURCE_DIR}/PACKAGE_README.md")
    if(NOT EXISTS "${_package_readme}")
        message(FATAL_ERROR "${PKG_PACKAGE_NAME}: missing PACKAGE_README.md in ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    add_custom_command(
        OUTPUT
            ${PKG_STAGING_DIR}/plugin.json
            ${PKG_STAGING_DIR}/${PKG_COMMAND_NAME}
            ${PKG_STAGING_DIR}/README.md
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PKG_STAGING_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
            ${PKG_STAGING_DIR}/plugin.json
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PKG_COMMAND_TARGET}>
            ${PKG_STAGING_DIR}/${PKG_COMMAND_NAME}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_package_readme}
            ${PKG_STAGING_DIR}/README.md
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json
            ${_package_readme}
            ${PKG_COMMAND_TARGET}
        VERBATIM
    )
    add_custom_target(${PKG_STAGE_TARGET}
        DEPENDS
            ${PKG_STAGING_DIR}/plugin.json
            ${PKG_STAGING_DIR}/${PKG_COMMAND_NAME}
            ${PKG_STAGING_DIR}/README.md
    )

    add_dependencies(${PKG_TEST_TARGET} ${PKG_STAGE_TARGET})
    add_test(NAME ${PKG_TEST_TARGET}
        COMMAND ${PKG_TEST_TARGET} ${PKG_STAGING_DIR}
    )
    add_custom_target(${PKG_PACKAGE_TARGET}
        COMMAND $<TARGET_FILE:${PKG_TEST_TARGET}> ${PKG_STAGING_DIR}
        DEPENDS ${PKG_TEST_TARGET}
        VERBATIM
    )

    install(TARGETS ${PKG_COMMAND_TARGET} RUNTIME DESTINATION .)
    install(FILES plugin.json DESTINATION .)
    install(FILES "${_package_readme}" DESTINATION . RENAME README.md)

    # Double-quoted string on purpose: ${PKG_*} expands at configure time and
    # every \${...} stays literal until the generated install script runs, so
    # the checks execute against the installed tree with the package's fixed
    # identity baked in.
    install(CODE "
        set(_package_root \"\${CMAKE_INSTALL_PREFIX}\")
        file(REAL_PATH \"\${_package_root}\" _package_root_real)
        set(_manifest \"\${_package_root_real}/plugin.json\")
        set(_helper \"\${_package_root_real}/${PKG_COMMAND_NAME}\")
        set(_readme \"\${_package_root_real}/README.md\")

        foreach(_required IN ITEMS \"\${_manifest}\" \"\${_helper}\" \"\${_readme}\")
            if(NOT EXISTS \"\${_required}\" OR IS_DIRECTORY \"\${_required}\")
                message(FATAL_ERROR \"${PKG_PACKAGE_NAME} package is missing \${_required}\")
            endif()
        endforeach()

        file(SIZE \"\${_readme}\" _readme_size)
        if(_readme_size EQUAL 0)
            message(FATAL_ERROR \"${PKG_PACKAGE_NAME} package ships an empty README.md\")
        endif()

        file(READ \"\${_manifest}\" _manifest_json)
        string(JSON _id GET \"\${_manifest_json}\" id)
        string(JSON _backend GET \"\${_manifest_json}\" backend)
        string(JSON _capability GET \"\${_manifest_json}\" capabilities 0)
        string(JSON _flat_command ERROR_VARIABLE _flat_error
               GET \"\${_manifest_json}\" command)
        string(JSON _invoked_command ERROR_VARIABLE _invoked_error
               GET \"\${_manifest_json}\" invocations property command)

        if(NOT _id STREQUAL \"${PKG_PACKAGE_ID}\"
           OR NOT _backend STREQUAL \"process\"
           OR NOT _capability STREQUAL \"property\"
           OR (NOT _flat_command STREQUAL \"${PKG_COMMAND_NAME}\"
               AND NOT _invoked_command STREQUAL \"${PKG_COMMAND_NAME}\"))
            message(FATAL_ERROR \"${PKG_PACKAGE_NAME} plugin.json does not match its fixed package contract\")
        endif()
    ")
endfunction()
