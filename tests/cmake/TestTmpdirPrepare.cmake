# SPDX-License-Identifier: GPL-3.0-or-later
#
# CTest's pre-test step (cmake/TestTmpdir.cmake): remove ROOT, the build tree's
# per-test temp folders, with whatever a previous run left there, and make one
# empty folder for each test NAMES (a file, one name a line) lists. It fails
# when ROOT cannot be removed, so no test starts on an old run's files.
#
# Expected -D inputs: ROOT, NAMES.

cmake_minimum_required(VERSION 3.20)

foreach(_var ROOT NAMES)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${ROOT}")
if(EXISTS "${ROOT}" OR IS_SYMLINK "${ROOT}")
    message(
        FATAL_ERROR
        "could not empty the test temp folders in ${ROOT}: remove what is left there and run ctest again"
    )
endif()
file(STRINGS "${NAMES}" _names)
list(TRANSFORM _names PREPEND "${ROOT}/")
file(MAKE_DIRECTORY "${ROOT}" ${_names})
