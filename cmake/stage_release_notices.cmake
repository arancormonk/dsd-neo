# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copies into a package's documentation folder the notices legal/components.json names for
# one platform: the <platform>_release map of every component record, which maps a path
# under the folder to where the notice comes from (tools/legal_inventory.py validates the
# maps):
#   repo:<path>      a file of the repository
#   build:<path>     a file of the configured build tree (the downloaded PFFFT's and
#                    TETRA_crypto's notices)
#   vcpkg:<port>     the copyright file the port installs, <VCPKG_SHARE_DIR>/<port>/copyright
#   legal-component  a file the package's general legal files already put there (the legal
#                    install component, or the APK staging's copies of LICENSES/, legal/ and
#                    mbelib's notices): checked, not copied
#
# Usage:
#   cmake -DPLATFORM=<android|macos|...> -DREPO_DIR=<repo> -DBUILD_DIR=<build> \
#         -DDOC_DIR=<package>/share/doc/dsd-neo [-DVCPKG_SHARE_DIR=<installed>/<triplet>/share] \
#         [-DCOMPONENTS=<components.json>] -P cmake/stage_release_notices.cmake
#
# Also run by cmake/stage_android_package.cmake, with DOC_DIR its fresh documentation folder.

# Script mode leaves policies unset: ask for the project's minimum, as its CMakeLists.txt does,
# so the if() rules do not depend on the host CMake (string(JSON) needs 3.19).
cmake_minimum_required(VERSION 3.20)

foreach(_required IN ITEMS PLATFORM REPO_DIR BUILD_DIR DOC_DIR)
    if(NOT DEFINED ${_required})
        message(
            FATAL_ERROR
            "stage_release_notices.cmake requires -D${_required}=<value>"
        )
    endif()
endforeach()
if(NOT PLATFORM MATCHES "^(linux|macos|windows|android)$")
    message(FATAL_ERROR "Unknown release platform '${PLATFORM}'")
endif()
if(NOT DEFINED COMPONENTS)
    set(COMPONENTS "${REPO_DIR}/legal/components.json")
endif()

# A relative path with no empty, '.' or '..' part and no backslash or drive.
function(_dsd_check_relative what path)
    if(
        path STREQUAL ""
        OR path MATCHES "^/"
        OR path MATCHES "[\\\\:]"
        OR path MATCHES "(^|/)\\.\\.?(/|$)"
        OR path MATCHES "//"
        OR path MATCHES "/$"
    )
        message(FATAL_ERROR "${what} '${path}' is not a path inside its root")
    endif()
endfunction()

file(READ "${COMPONENTS}" _inventory)
string(JSON _count LENGTH "${_inventory}" components)
if(_count GREATER 0)
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE ${_last})
        string(JSON _id GET "${_inventory}" components ${_index} id)
        string(
            JSON _release
            ERROR_VARIABLE _absent
            GET "${_inventory}"
            components
            ${_index}
            ${PLATFORM}_release
            notices
        )
        if(_absent)
            continue()
        endif()
        string(JSON _notices LENGTH "${_release}")
        if(_notices EQUAL 0)
            continue()
        endif()
        math(EXPR _notice_last "${_notices} - 1")
        foreach(_n RANGE ${_notice_last})
            string(JSON _notice MEMBER "${_release}" ${_n})
            string(JSON _from GET "${_release}" "${_notice}")
            _dsd_check_relative("The notice ${_notice} of ${_id}" "${_notice}")
            if(_from STREQUAL "legal-component")
                if(NOT EXISTS "${DOC_DIR}/${_notice}")
                    message(
                        FATAL_ERROR
                        "The notice ${_notice} of ${_id} should come with the package's legal files, but ${DOC_DIR}/${_notice} is missing"
                    )
                endif()
                continue()
            elseif(_from MATCHES "^repo:(.+)$")
                set(_relative "${CMAKE_MATCH_1}")
                _dsd_check_relative("The source of ${_notice} of ${_id}" "${_relative}")
                set(_source "${REPO_DIR}/${_relative}")
            elseif(_from MATCHES "^build:(.+)$")
                set(_relative "${CMAKE_MATCH_1}")
                _dsd_check_relative("The source of ${_notice} of ${_id}" "${_relative}")
                set(_source "${BUILD_DIR}/${_relative}")
            elseif(_from MATCHES "^vcpkg:([a-z0-9-]+)$")
                if(NOT DEFINED VCPKG_SHARE_DIR)
                    message(
                        FATAL_ERROR
                        "The notice ${_notice} of ${_id} comes from vcpkg's tree: pass -DVCPKG_SHARE_DIR=<installed>/<triplet>/share"
                    )
                endif()
                set(_source "${VCPKG_SHARE_DIR}/${CMAKE_MATCH_1}/copyright")
            else()
                message(
                    FATAL_ERROR
                    "The notice ${_notice} of ${_id} names a source staging does not know: '${_from}'"
                )
            endif()
            if(NOT EXISTS "${_source}" OR IS_DIRECTORY "${_source}")
                message(
                    FATAL_ERROR
                    "The notice ${_notice} of ${_id} comes from ${_source}, which is not a file"
                )
            endif()
            # COPYONLY skips a file that is already current, so a no-op build changes nothing.
            configure_file("${_source}" "${DOC_DIR}/${_notice}" COPYONLY)
        endforeach()
    endforeach()
endif()
