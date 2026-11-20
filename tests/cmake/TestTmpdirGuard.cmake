# SPDX-License-Identifier: GPL-3.0-or-later
#
# The per-test temp folders tests/CMakeLists.txt gives every test (TMPDIR, TEMP
# and TMP): MODE=setup empties ROOT and makes a folder in it for each test that
# NAMES (a file, one test name a line) lists; MODE=check, which CTest runs after
# every other test that ran, fails naming each test whose folder is not empty,
# and removes ROOT when all are. A failed check leaves the files there to look
# at; the next run's setup removes them.
#
# Expected -D inputs: MODE, ROOT; NAMES for setup.

foreach(_var MODE ROOT)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()

if(MODE STREQUAL "setup")
    if(NOT DEFINED NAMES)
        message(FATAL_ERROR "NAMES is required")
    endif()
    file(REMOVE_RECURSE "${ROOT}")
    file(STRINGS "${NAMES}" _names)
    list(TRANSFORM _names PREPEND "${ROOT}/")
    file(MAKE_DIRECTORY "${ROOT}" ${_names})
    return()
endif()

if(NOT MODE STREQUAL "check")
    message(FATAL_ERROR "MODE must be setup or check, not '${MODE}'")
endif()

file(GLOB _dirs LIST_DIRECTORIES true RELATIVE "${ROOT}" "${ROOT}/*")
list(SORT _dirs)
set(_report "")
foreach(_test IN LISTS _dirs)
    file(
        GLOB_RECURSE _left
        LIST_DIRECTORIES true
        RELATIVE "${ROOT}/${_test}"
        "${ROOT}/${_test}/*"
    )
    if(_left)
        list(SORT _left)
        list(LENGTH _left _count)
        list(SUBLIST _left 0 10 _shown)
        string(REPLACE ";" "\n    " _shown "${_shown}")
        string(APPEND _report "  ${_test} left ${_count}:\n    ${_shown}\n")
    endif()
endforeach()

if(NOT _report STREQUAL "")
    message(
        FATAL_ERROR
        "Tests left files in their temp folders (under ${ROOT}).\n"
        "Each test must remove every temp file and folder it creates; see docs/testing.md, \"Temporary files and directories\".\n"
        "${_report}"
    )
endif()
file(REMOVE_RECURSE "${ROOT}")
message(STATUS "Every test's temp folder is empty")
