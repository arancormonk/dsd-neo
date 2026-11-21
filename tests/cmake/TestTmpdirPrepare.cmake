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

# Only ever a test-tmp folder, and only once the list of tests has been read.
if(NOT ROOT MATCHES "/test-tmp$" OR NOT EXISTS "${NAMES}")
    message(
        FATAL_ERROR
        "refusing to prepare ROOT=${ROOT} with NAMES=${NAMES}: not a test-tmp folder, or no list of tests"
    )
endif()
file(STRINGS "${NAMES}" _names)
# A list written on Windows ends its lines in CR LF.
list(TRANSFORM _names REPLACE "\r$" "")
file(REMOVE_RECURSE "${ROOT}")
if(EXISTS "${ROOT}" OR IS_SYMLINK "${ROOT}")
    message(
        FATAL_ERROR
        "could not empty the test temp folders in ${ROOT}: remove what is left there and run ctest again"
    )
endif()
list(TRANSFORM _names PREPEND "${ROOT}/")
file(MAKE_DIRECTORY "${ROOT}" ${_names})
