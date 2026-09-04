# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises dsd_neo_macos_curses_plan() from cmake/macos_curses.cmake.
#
# The function decides which curses FindCurses is aimed at on macOS, where the
# wrong answer is a configure failure on a stock host. Its inputs are a library
# path and a list of prefixes, so the decision can be checked against fabricated
# prefix trees from any platform.
#
# Usage:
#   cmake -DDSD_TEST_WORK_DIR=<dir> -P tests/cmake/MacosCursesPlan.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    message(
        FATAL_ERROR
        "MACOS_CURSES_PLAN: must be run via 'cmake -P <script>'"
    )
endif()
if(NOT DSD_TEST_WORK_DIR)
    message(FATAL_ERROR "MACOS_CURSES_PLAN: DSD_TEST_WORK_DIR is required")
endif()

get_filename_component(_MCPT_SCRIPT_DIR "${CMAKE_SCRIPT_MODE_FILE}" DIRECTORY)
get_filename_component(_MCPT_TESTS_DIR "${_MCPT_SCRIPT_DIR}" DIRECTORY)
get_filename_component(_MCPT_ROOT_DIR "${_MCPT_TESTS_DIR}" DIRECTORY)

include("${_MCPT_ROOT_DIR}/cmake/macos_curses.cmake")

set(_MCPT_FAILURES "")

function(_mcpt_expect what actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        set(_MCPT_FAILURES
            "${_MCPT_FAILURES}\n  - ${what}: expected '${expected}', got '${actual}'"
            PARENT_SCOPE
        )
    endif()
endfunction()

# A keg-only prefix as Homebrew lays one out: wide headers in their own subdir,
# versioned library plus the unversioned symlink beside it.
function(_mcpt_make_keg dir)
    file(MAKE_DIRECTORY "${dir}/include/ncursesw")
    file(MAKE_DIRECTORY "${dir}/lib")
    file(WRITE "${dir}/include/ncursesw/curses.h" "/* fixture */\n")
    file(WRITE "${dir}/include/curses.h" "/* fixture */\n")
    file(WRITE "${dir}/lib/libncursesw.6.dylib" "fixture\n")
    file(WRITE "${dir}/lib/libncursesw.dylib" "fixture\n")
endfunction()

set(_MCPT_WORK "${DSD_TEST_WORK_DIR}")
file(REMOVE_RECURSE "${_MCPT_WORK}")
file(MAKE_DIRECTORY "${_MCPT_WORK}")

set(_MCPT_KEG "${_MCPT_WORK}/homebrew/opt/ncurses")
set(_MCPT_KEG_ALT "${_MCPT_WORK}/homebrew-alt/opt/ncurses")
_mcpt_make_keg("${_MCPT_KEG}")
_mcpt_make_keg("${_MCPT_KEG_ALT}")

# A keg whose headers arrived without the library, and one with neither.
set(_MCPT_KEG_HEADERS_ONLY "${_MCPT_WORK}/broken/opt/ncurses")
file(MAKE_DIRECTORY "${_MCPT_KEG_HEADERS_ONLY}/include/ncursesw")
file(MAKE_DIRECTORY "${_MCPT_KEG_HEADERS_ONLY}/lib")
file(
    WRITE "${_MCPT_KEG_HEADERS_ONLY}/include/ncursesw/curses.h"
    "/* fixture */\n"
)
set(_MCPT_KEG_ABSENT "${_MCPT_WORK}/absent/opt/ncurses")

# ---------------------------------------------------------------------------
# A wide ncurses on the default search path is used as found, prefixes ignored.
# ---------------------------------------------------------------------------
dsd_neo_macos_curses_plan(
    NCURSESW_LIBRARY "/opt/local/lib/libncursesw.dylib"
    PREFIXES "${_MCPT_KEG}"
    OUT_NEED_WIDE _need_wide
    OUT_PREFIX _prefix
    OUT_REASON _reason
)
_mcpt_expect("found ncursesw: need wide" "${_need_wide}" "TRUE")
_mcpt_expect("found ncursesw: prefix" "${_prefix}" "")

# ---------------------------------------------------------------------------
# An unfound cache entry reads as absent, not as a path.
# ---------------------------------------------------------------------------
dsd_neo_macos_curses_plan(
    NCURSESW_LIBRARY "DSD_NCURSESW_LIBRARY-NOTFOUND"
    PREFIXES "${_MCPT_KEG}"
    OUT_NEED_WIDE _need_wide
    OUT_PREFIX _prefix
    OUT_REASON _reason
)
_mcpt_expect("NOTFOUND library: need wide" "${_need_wide}" "TRUE")
_mcpt_expect("NOTFOUND library: prefix" "${_prefix}" "${_MCPT_KEG}")

# ---------------------------------------------------------------------------
# No ncursesw anywhere: drop the wide requirement rather than fail the configure.
# ---------------------------------------------------------------------------
dsd_neo_macos_curses_plan(
    NCURSESW_LIBRARY ""
    PREFIXES "${_MCPT_KEG_ABSENT}" "${_MCPT_KEG_HEADERS_ONLY}"
    OUT_NEED_WIDE _need_wide
    OUT_PREFIX _prefix
    OUT_REASON _reason
)
_mcpt_expect("no ncursesw: need wide" "${_need_wide}" "FALSE")
_mcpt_expect("no ncursesw: prefix" "${_prefix}" "")

# ---------------------------------------------------------------------------
# No prefixes at all is the same answer, and no error.
# ---------------------------------------------------------------------------
dsd_neo_macos_curses_plan(
    NCURSESW_LIBRARY ""
    OUT_NEED_WIDE _need_wide
    OUT_PREFIX _prefix
    OUT_REASON _reason
)
_mcpt_expect("no prefixes: need wide" "${_need_wide}" "FALSE")
_mcpt_expect("no prefixes: prefix" "${_prefix}" "")

# ---------------------------------------------------------------------------
# Prefixes are searched in order, so the caller's precedence is what decides
# between two usable kegs, and an empty entry does not end the search.
# ---------------------------------------------------------------------------
dsd_neo_macos_curses_plan(
    NCURSESW_LIBRARY ""
    PREFIXES
        ""
        "${_MCPT_KEG_HEADERS_ONLY}"
        "${_MCPT_KEG_ALT}"
        "${_MCPT_KEG}"
    OUT_NEED_WIDE _need_wide
    OUT_PREFIX _prefix
    OUT_REASON _reason
)
_mcpt_expect("ordered prefixes: need wide" "${_need_wide}" "TRUE")
_mcpt_expect("ordered prefixes: prefix" "${_prefix}" "${_MCPT_KEG_ALT}")

# The reason is what a user reads on the configure line, so it has to name the
# prefix that was chosen.
if(NOT _reason MATCHES "${_MCPT_KEG_ALT}")
    set(_MCPT_FAILURES
        "${_MCPT_FAILURES}\n  - ordered prefixes: reason '${_reason}' does not name ${_MCPT_KEG_ALT}"
    )
endif()

file(REMOVE_RECURSE "${_MCPT_WORK}")

if(_MCPT_FAILURES)
    message(FATAL_ERROR "MACOS_CURSES_PLAN failed:${_MCPT_FAILURES}")
endif()

message(STATUS "MACOS_CURSES_PLAN: ok")
