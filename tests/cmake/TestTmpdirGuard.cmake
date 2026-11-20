# SPDX-License-Identifier: GPL-3.0-or-later
#
# One test's temp folder (cmake/TestTmpdir.cmake): MODE=setup empties DIR, or
# makes it; MODE=check, which CTest runs after the test, fails naming each entry
# TEST_NAME left in DIR, and removes DIR either way, so a failed check leaves nothing
# in the build tree and the next run of the test starts clean. The listing is one
# level deep: one entry is enough to show a leak, and a leaked link is reported,
# never followed.
#
# Expected -D inputs: MODE, DIR; TEST_NAME for check.

cmake_minimum_required(VERSION 3.20)

foreach(_var MODE DIR)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()

if(MODE STREQUAL "setup")
    file(REMOVE_RECURSE "${DIR}")
    file(MAKE_DIRECTORY "${DIR}")
    return()
endif()

if(NOT MODE STREQUAL "check")
    message(FATAL_ERROR "MODE must be setup or check, not '${MODE}'")
endif()
if(NOT DEFINED TEST_NAME OR TEST_NAME STREQUAL "")
    message(FATAL_ERROR "TEST_NAME is required")
endif()

if(IS_SYMLINK "${DIR}" OR NOT IS_DIRECTORY "${DIR}")
    file(REMOVE_RECURSE "${DIR}")
    message(
        FATAL_ERROR
        "${TEST_NAME}: its temp folder ${DIR} is gone or is no longer a folder"
    )
endif()

file(GLOB _left LIST_DIRECTORIES true RELATIVE "${DIR}" "${DIR}/*")
list(LENGTH _left _count)
file(REMOVE_RECURSE "${DIR}")
if(_count GREATER 0)
    list(SORT _left)
    list(SUBLIST _left 0 20 _shown)
    string(REPLACE ";" "\n  " _shown "${_shown}")
    message(
        FATAL_ERROR
        "${TEST_NAME} left ${_count} entries in its temp folder (now removed):\n  ${_shown}\n"
        "A test must remove every temp file and folder it creates; see docs/testing.md, \"Temporary files and directories\"."
    )
endif()
