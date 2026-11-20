# SPDX-License-Identifier: GPL-3.0-or-later
#
# TEST_TMPDIR_GUARD: tests/cmake/TestTmpdirGuard.cmake, the per-test temp folder
# setup and check, run on folders made here. The check must fail on any entry
# left, named 0 or OFF (CMake false values), hidden, a folder, or a link it must
# not follow; pass on an empty folder; fail when the folder is gone; and remove
# the folder in each case.
#
# Expected -D inputs: GUARD (the script under test), WORK (a scratch folder this
# script owns and removes).

cmake_minimum_required(VERSION 3.20)

foreach(_var GUARD WORK)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(_failures "")

# Run the check on WORK/@p name; @p want is "pass" or a regular expression its
# error must match, with each run of whitespace in the error (CMake wraps and
# indents it) read as one space.
function(_check name want)
    execute_process(
        COMMAND
            ${CMAKE_COMMAND} -DMODE=check "-DDIR=${WORK}/${name}"
            "-DTEST_NAME=${name}" -P "${GUARD}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
    )
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_err}")
    set(_problem "")
    if(want STREQUAL "pass")
        if(NOT _rc STREQUAL "0")
            set(_problem "failed: ${_err}")
        endif()
    elseif(_rc STREQUAL "0")
        set(_problem "passed, expected a failure matching '${want}'")
    elseif(NOT _flat MATCHES "${want}")
        set(_problem "error does not match '${want}': ${_err}")
    endif()
    if(EXISTS "${WORK}/${name}" OR IS_SYMLINK "${WORK}/${name}")
        string(APPEND _problem " the folder is still there")
    endif()
    if(NOT _problem STREQUAL "")
        set(_failures "${_failures}  ${name}: ${_problem}\n" PARENT_SCOPE)
    endif()
endfunction()

# Setup empties a folder a previous run left full, and makes a missing one.
file(MAKE_DIRECTORY "${WORK}/stale/sub")
file(WRITE "${WORK}/stale/sub/old.txt" "old\n")
foreach(_name stale fresh)
    execute_process(
        COMMAND
            ${CMAKE_COMMAND} -DMODE=setup "-DDIR=${WORK}/${_name}" -P "${GUARD}"
        RESULT_VARIABLE _rc
    )
    file(GLOB _entries "${WORK}/${_name}/*")
    if(NOT _rc STREQUAL "0" OR NOT IS_DIRECTORY "${WORK}/${_name}" OR _entries)
        string(
            APPEND _failures
            "  setup ${_name}: rc ${_rc}, entries '${_entries}'\n"
        )
    endif()
endforeach()
_check(fresh pass)

file(MAKE_DIRECTORY "${WORK}/zero")
file(WRITE "${WORK}/zero/0" "")
_check(zero "zero left 1 entries in its temp folder \\(now removed\\): 0 A test")

file(MAKE_DIRECTORY "${WORK}/hidden")
file(WRITE "${WORK}/hidden/.dotfile" "")
_check(hidden "hidden left 1 entries [^:]*: \\.dotfile A test")

file(MAKE_DIRECTORY "${WORK}/several/OFF")
file(WRITE "${WORK}/several/OFF/inner.txt" "")
file(WRITE "${WORK}/several/a.txt" "")
_check(several "several left 2 entries [^:]*: OFF a\\.txt A test")

_check(missing "missing: its temp folder [^ ]*/missing is gone")

if(NOT CMAKE_HOST_WIN32)
    # A leaked link is named and removed, never followed: what it points at stays.
    file(MAKE_DIRECTORY "${WORK}/outside" "${WORK}/linked")
    file(WRITE "${WORK}/outside/keep.txt" "keep\n")
    file(CREATE_LINK "${WORK}/outside" "${WORK}/linked/link" SYMBOLIC)
    _check(linked "linked left 1 entries [^:]*: link A test")
    if(NOT EXISTS "${WORK}/outside/keep.txt")
        string(APPEND _failures "  linked: the check followed the link\n")
    endif()
    # The folder itself replaced by a link is refused without walking it.
    file(CREATE_LINK "${WORK}/outside" "${WORK}/swapped" SYMBOLIC)
    _check(swapped "swapped: its temp folder [^ ]*/swapped is gone or is no longer a folder")
    if(NOT EXISTS "${WORK}/outside/keep.txt")
        string(
            APPEND _failures
            "  swapped: removing the link removed its target's files\n"
        )
    endif()
endif()

file(REMOVE_RECURSE "${WORK}")
if(NOT _failures STREQUAL "")
    message(FATAL_ERROR "TEST_TMPDIR_GUARD:\n${_failures}")
endif()
message(STATUS "TEST_TMPDIR_GUARD: OK")
