# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# Stages the directory androiddeployqt packages into the APK: android/package plus the
# license/notice set every shipped asset carries (docs/release-assets-checklist.md): the
# project's legal files, LICENSES/, the selected mbelib's notices, and the notices the
# android_release map of each record in legal/components.json names
# (cmake/stage_release_notices.cmake).
#
# Usage:
#   cmake -DREPO_DIR=<repo> -DSOURCE_DIR=<android/package> -DSTAGE_DIR=<out> \
#         -DBUILD_DIR=<build> \
#         -DMBELIB_NOTICE_DIR=<prefix>/share/doc/mbelib-neo \
#         -DVCPKG_SHARE_DIR=<vcpkg_installed>/<triplet>/share \
#         -P cmake/stage_android_package.cmake
#
# Run as a build step rather than at configure time so edits to android/package/ reach the
# APK without re-running CMake. file(COPY) and configure_file(COPYONLY) both skip files
# that are already current, so a no-op build does not make Gradle repackage.
#
# The documentation folder is staged afresh, from the notices and then the files of
# android/package/'s own documentation whose paths no notice takes, in a new folder this
# run makes beside the package and removes again, and the package's folder is then brought
# in line with it: a notice no longer staged leaves the package and its index, as a clean
# build would leave it out, and a file whose content is unchanged is not rewritten. Those
# steps run as a child of this script (the same script, given -D_DSD_FRESH_DIR=<folder>),
# so the new folder is removed whichever of them fails.

# Script mode leaves policies unset, and CMake 3.x then refuses if(IN_LIST): ask for the
# project's minimum, as its CMakeLists.txt does.
cmake_minimum_required(VERSION 3.20)

foreach(
    _required
    IN
    ITEMS
        REPO_DIR
        SOURCE_DIR
        STAGE_DIR
        BUILD_DIR
        MBELIB_NOTICE_DIR
        VCPKG_SHARE_DIR
)
    if(NOT DEFINED ${_required})
        message(
            FATAL_ERROR
            "stage_android_package.cmake requires -D${_required}=<path>"
        )
    endif()
endforeach()

get_filename_component(_source "${SOURCE_DIR}" ABSOLUTE)
get_filename_component(_stage "${STAGE_DIR}" ABSOLUTE)
set(_doc_dir "${_stage}/assets/doc/dsd-neo")

# Every path below <dir>, relative to it, folders included when <list_dirs> is true. A glob
# reads [, ], * and ? in the folder's own path as a pattern too, so they are put in brackets
# there, where each stands for itself alone.
function(_dsd_glob_tree out dir list_dirs)
    string(REGEX REPLACE "([][*?])" "[\\1]" _pattern "${dir}")
    file(
        GLOB_RECURSE _entries
        LIST_DIRECTORIES ${list_dirs}
        RELATIVE "${dir}"
        "${_pattern}/*"
    )
    set(${out} "${_entries}" PARENT_SCOPE)
endfunction()

# The first symbolic link among <root>, the folders on the way from it to <root>/<relative>
# and everything in the folder there, or "" when there is none.
function(_dsd_find_link out root relative)
    get_filename_component(_path "${root}" ABSOLUTE)
    set(_link "")
    if(IS_SYMLINK "${_path}")
        set(_link "${_path}")
    endif()
    string(REPLACE "/" ";" _parts "${relative}")
    foreach(_part IN LISTS _parts)
        if(NOT _link STREQUAL "")
            break()
        endif()
        string(APPEND _path "/${_part}")
        if(IS_SYMLINK "${_path}")
            set(_link "${_path}")
        endif()
    endforeach()
    if(_link STREQUAL "" AND IS_DIRECTORY "${_path}")
        _dsd_glob_tree(_entries "${_path}" true)
        foreach(_entry IN LISTS _entries)
            if(IS_SYMLINK "${_path}/${_entry}")
                set(_link "${_path}/${_entry}")
                break()
            endif()
        endforeach()
    endif()
    set(${out} "${_link}" PARENT_SCOPE)
endfunction()

function(_dsd_refuse_links root relative)
    _dsd_find_link(_link "${root}" "${relative}")
    if(NOT _link STREQUAL "")
        message(
            FATAL_ERROR
            "${_link} is a symbolic link. The APK's documentation is staged only through real folders and files: replace it with the folder or file it points to."
        )
    endif()
endfunction()

if(DEFINED _DSD_FRESH_DIR)
    # The child: stage the documentation folder in the parent's new folder and bring the
    # package's folder in line with it.
    set(_fresh_dir "${_DSD_FRESH_DIR}")
    # The notices first, into the empty folder, and android/package/'s own documentation
    # after them, below. file(COPY) would give each folder the mode of its source, and an
    # installed notice folder may be read-only: the copies are this run's own, so it can
    # write into them.
    set(_folder_modes
        DIRECTORY_PERMISSIONS
        OWNER_READ
        OWNER_WRITE
        OWNER_EXECUTE
        GROUP_READ
        GROUP_EXECUTE
        WORLD_READ
        WORLD_EXECUTE
    )
    file(COPY "${REPO_DIR}/legal" DESTINATION "${_fresh_dir}" ${_folder_modes})
    file(
        COPY "${REPO_DIR}/LICENSES/"
        DESTINATION "${_fresh_dir}/licenses"
        ${_folder_modes}
    )
    # Leave out the other FFT's notice tree (MbelibNotices.cmake, in the parent).
    set(_mbe_exclude)
    if(NOT MBELIB_UNSELECTED_FFT_REGEX STREQUAL "")
        set(_mbe_exclude REGEX "${MBELIB_UNSELECTED_FFT_REGEX}" EXCLUDE)
    endif()
    file(
        COPY "${MBELIB_NOTICE_DIR}/"
        DESTINATION "${_fresh_dir}/licenses/mbelib-neo"
        ${_folder_modes}
        ${_mbe_exclude}
    )

    file(
        COPY
            "${REPO_DIR}/LICENSE"
            "${REPO_DIR}/COPYRIGHT"
            "${REPO_DIR}/THIRD_PARTY.md"
        DESTINATION "${_fresh_dir}"
    )

    # file(COPY) copies a symbolic link as a link, and the notice and package files merged
    # below would be written through one to wherever it points, so the notices bring none.
    _dsd_find_link(_link "${_fresh_dir}" "")
    if(NOT _link STREQUAL "")
        file(RELATIVE_PATH _name "${_fresh_dir}" "${_link}")
        if(_name MATCHES "^licenses/mbelib-neo/(.*)$")
            set(_link "${MBELIB_NOTICE_DIR}/${CMAKE_MATCH_1}")
        elseif(_name MATCHES "^licenses/(.*)$")
            set(_link "${REPO_DIR}/LICENSES/${CMAKE_MATCH_1}")
        else()
            set(_link "${REPO_DIR}/${_name}")
        endif()
        message(
            FATAL_ERROR
            "${_link} is a symbolic link. The APK's notices are staged only from real folders and files: replace it with the folder or file it points to."
        )
    endif()

    # The notices of every component the APK carries, under the names its record gives them:
    # the vendored USB libraries, the fonts compiled into the Qt resource bundle, the
    # downloaded PFFFT and TETRA_crypto, and the libraries vcpkg links into the app.
    set(_notice_args
        -DPLATFORM=android
        -DREPO_DIR=${REPO_DIR}
        -DBUILD_DIR=${BUILD_DIR}
        -DVCPKG_SHARE_DIR=${VCPKG_SHARE_DIR}
        -DDOC_DIR=${_fresh_dir}
    )
    if(DEFINED COMPONENTS)
        list(APPEND _notice_args -DCOMPONENTS=${COMPONENTS})
    endif()
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" ${_notice_args} -P
            "${REPO_DIR}/cmake/stage_release_notices.cmake"
        RESULT_VARIABLE _notice_result
    )
    if(NOT _notice_result EQUAL 0)
        message(
            FATAL_ERROR
            "Staging the notices legal/components.json names for the APK failed"
        )
    endif()

    # Then each file of android/package/'s documentation whose path no notice takes: none
    # at a notice's path or under it, whatever its timestamp (file(COPY) would keep a
    # destination of the same time), and none where a notice folder is. The index written
    # below takes its path as a notice does.
    set(_index_name "notice-index.txt")
    set(_package_doc "${_source}/assets/doc/dsd-neo")
    set(_package_files "")
    if(IS_DIRECTORY "${_package_doc}")
        _dsd_glob_tree(_package_files "${_package_doc}" false)
    endif()
    foreach(_name IN LISTS _package_files)
        set(_path "${_fresh_dir}")
        set(_taken FALSE)
        if("${_name}/" MATCHES "^notice-index[.]txt/")
            set(_taken TRUE)
        endif()
        string(REPLACE "/" ";" _parts "${_name}")
        foreach(_part IN LISTS _parts)
            string(APPEND _path "/${_part}")
            if(EXISTS "${_path}" AND NOT IS_DIRECTORY "${_path}")
                set(_taken TRUE)
                break()
            endif()
        endforeach()
        if(NOT _taken AND NOT IS_DIRECTORY "${_path}")
            configure_file(
                "${_package_doc}/${_name}"
                "${_fresh_dir}/${_name}"
                COPYONLY
            )
        endif()
    endforeach()

    _dsd_glob_tree(_staged "${_fresh_dir}" false)
    list(REMOVE_ITEM _staged "${_index_name}")
    set(_present "")
    if(EXISTS "${_doc_dir}")
        _dsd_glob_tree(_present "${_doc_dir}" false)
    endif()
    foreach(_name IN LISTS _present)
        if(NOT _name STREQUAL _index_name AND NOT _name IN_LIST _staged)
            file(REMOVE "${_doc_dir}/${_name}")
        endif()
    endforeach()
    foreach(_name IN LISTS _staged)
        # A notice that was a folder's file before is now a file of the folder's name.
        if(IS_DIRECTORY "${_doc_dir}/${_name}")
            file(REMOVE_RECURSE "${_doc_dir}/${_name}")
        endif()
        # COPYONLY leaves a file whose content is already current as it is.
        configure_file("${_fresh_dir}/${_name}" "${_doc_dir}/${_name}" COPYONLY)
    endforeach()

    # The app's licences screen reads the files this index names, one path per line: the
    # project's own legal files first, then every other file the folder holds, sorted. It is
    # taken from the folder as staged, so a notice a record's map adds is listed with no
    # change to the app.
    list(SORT _staged)
    set(_first LICENSE COPYRIGHT THIRD_PARTY.md)
    list(REMOVE_ITEM _staged ${_first})
    foreach(_name IN LISTS _first)
        if(NOT EXISTS "${_doc_dir}/${_name}")
            message(
                FATAL_ERROR
                "The package's ${_name} is missing from ${_doc_dir}"
            )
        endif()
    endforeach()
    string(JOIN "\n" _index ${_first} ${_staged})
    string(APPEND _index "\n")
    # A folder an older staging left at the index's path gives way to it.
    if(IS_DIRECTORY "${_doc_dir}/${_index_name}")
        file(REMOVE_RECURSE "${_doc_dir}/${_index_name}")
    endif()
    # Unchanged content is not rewritten, so a no-op build does not make Gradle repackage.
    set(_old_index "")
    if(EXISTS "${_doc_dir}/${_index_name}")
        file(READ "${_doc_dir}/${_index_name}" _old_index)
    endif()
    if(NOT _old_index STREQUAL _index)
        file(WRITE "${_doc_dir}/${_index_name}" "${_index}")
    endif()
    return()
endif()

set(DSD_MBELIB_NOTICE_DIR "${MBELIB_NOTICE_DIR}")
include("${REPO_DIR}/cmake/MbelibNotices.cmake")

# Every copy below writes through a symbolic link as through a folder, to wherever it
# points, so none may be in the documentation's path: the source and stage folders
# themselves, android/package/'s documentation or the way to it, or the staged package's.
_dsd_refuse_links("${_source}" "assets/doc/dsd-neo")
_dsd_refuse_links("${_stage}" "assets/doc/dsd-neo")

# The package's documentation folder comes from the fresh one alone, below.
file(
    COPY "${_source}/"
    DESTINATION "${_stage}"
    REGEX "/assets/doc/dsd-neo$"
    EXCLUDE
)

# A folder of a name nothing beside the package has yet, so the run removes only its own.
get_filename_component(_stage_parent "${_stage}" DIRECTORY)
get_filename_component(_stage_name "${_stage}" NAME)
while(TRUE)
    string(RANDOM LENGTH 16 _tag)
    set(_fresh_dir "${_stage_parent}/.${_stage_name}-doc-${_tag}")
    if(NOT EXISTS "${_fresh_dir}")
        break()
    endif()
endwhile()
file(MAKE_DIRECTORY "${_fresh_dir}")

# The steps that fill the new folder and publish it, as a child, so the folder is removed
# here whichever of them fails, and the child's output is reported with the failure.
set(_child_args
    -DREPO_DIR=${REPO_DIR}
    -DSOURCE_DIR=${_source}
    -DSTAGE_DIR=${STAGE_DIR}
    -DBUILD_DIR=${BUILD_DIR}
    -DMBELIB_NOTICE_DIR=${MBELIB_NOTICE_DIR}
    "-DMBELIB_UNSELECTED_FFT_REGEX=${DSD_MBELIB_UNSELECTED_FFT_REGEX}"
    -DVCPKG_SHARE_DIR=${VCPKG_SHARE_DIR}
    -D_DSD_FRESH_DIR=${_fresh_dir}
)
if(DEFINED COMPONENTS)
    list(APPEND _child_args -DCOMPONENTS=${COMPONENTS})
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_child_args} -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _child_result
    OUTPUT_VARIABLE _child_output
    ERROR_VARIABLE _child_output
)
file(REMOVE_RECURSE "${_fresh_dir}")
string(STRIP "${_child_output}" _child_output)
if(NOT _child_result EQUAL 0)
    message(
        FATAL_ERROR
        "Staging the APK's documentation folder failed:\n${_child_output}"
    )
endif()
if(NOT _child_output STREQUAL "")
    message("${_child_output}")
endif()
