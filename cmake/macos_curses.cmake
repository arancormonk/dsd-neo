# SPDX-License-Identifier: GPL-3.0-or-later
#
# macOS curses flavour selection.
#
# CURSES_NEED_WIDE is set for every non-Windows target, but macOS ships no
# libncursesw: its unified libncurses carries the wide entry points behind
# _XOPEN_SOURCE_EXTENDED. FindCurses therefore misses the wide library, falls
# through to its plain-curses branch, and skips the header search entirely -
# that branch only probes for headers when the wide flag is off - so configure
# dies with a bare "Could NOT find Curses (missing: CURSES_INCLUDE_PATH)" one
# line after reporting the library it just found in the SDK.
#
# The decision of which curses to aim FindCurses at lives here, apart from the
# probing, so it can be exercised by MACOS_CURSES_PLAN without a macOS host.

# dsd_neo_macos_curses_plan(
#     NCURSESW_LIBRARY <path>   # what find_library(ncursesw) returned, may be
#                               # empty or a <var>-NOTFOUND string
#     PREFIXES <dir>...         # keg-only prefixes to search, most specific first
#     OUT_NEED_WIDE <var>       # TRUE to keep CURSES_NEED_WIDE, FALSE to drop it
#     OUT_PREFIX <var>          # prefix to put on CMAKE_PREFIX_PATH, "" for none
#     OUT_REASON <var>          # one line describing the choice, for message()
# )
#
# A wide ncurses already on the search path is left alone: second-guessing it
# would risk pairing one installation's headers with another's library. Homebrew
# keeps its ncurses keg-only, so those prefixes have to be spelled out by the
# caller. Failing both, the wide requirement is dropped and FindCurses takes the
# SDK's unified libncurses, whose headers it locates through its own HINTS.
# Nothing in the ncurses path calls the wide API - the only addwstr() sits
# behind DSD_USE_PDCURSES in src/ui/terminal/ncurses_snr.c - so the narrow build
# is complete.
function(dsd_neo_macos_curses_plan)
    set(_options "")
    set(_one_value NCURSESW_LIBRARY OUT_NEED_WIDE OUT_PREFIX OUT_REASON)
    set(_multi_value PREFIXES)
    cmake_parse_arguments(
        _MCP
        "${_options}"
        "${_one_value}"
        "${_multi_value}"
        ${ARGN}
    )

    foreach(_required IN ITEMS OUT_NEED_WIDE OUT_PREFIX OUT_REASON)
        if(NOT _MCP_${_required})
            message(
                FATAL_ERROR
                "dsd_neo_macos_curses_plan: ${_required} is required"
            )
        endif()
    endforeach()
    if(_MCP_UNPARSED_ARGUMENTS)
        message(
            FATAL_ERROR
            "dsd_neo_macos_curses_plan: unexpected arguments: ${_MCP_UNPARSED_ARGUMENTS}"
        )
    endif()

    # if() reads a value ending in -NOTFOUND as false, so an unfound cache entry
    # needs no special casing here.
    if(_MCP_NCURSESW_LIBRARY)
        set(${_MCP_OUT_NEED_WIDE} TRUE PARENT_SCOPE)
        set(${_MCP_OUT_PREFIX} "" PARENT_SCOPE)
        set(${_MCP_OUT_REASON}
            "wide ncurses already on the search path (${_MCP_NCURSESW_LIBRARY})"
            PARENT_SCOPE
        )
        return()
    endif()

    foreach(_prefix IN LISTS _MCP_PREFIXES)
        if(NOT _prefix)
            continue()
        endif()
        if(NOT EXISTS "${_prefix}/include/ncursesw/curses.h")
            continue()
        endif()
        # Homebrew ships libncursesw.dylib next to the versioned library, but a
        # keg whose headers arrived without one would only send FindCurses back
        # to the same failure, so both halves have to be there.
        file(GLOB _libs "${_prefix}/lib/libncursesw*.dylib")
        if(NOT _libs)
            continue()
        endif()
        set(${_MCP_OUT_NEED_WIDE} TRUE PARENT_SCOPE)
        set(${_MCP_OUT_PREFIX} "${_prefix}" PARENT_SCOPE)
        set(${_MCP_OUT_REASON}
            "using the keg-only ncursesw at ${_prefix}"
            PARENT_SCOPE
        )
        return()
    endforeach()

    set(${_MCP_OUT_NEED_WIDE} FALSE PARENT_SCOPE)
    set(${_MCP_OUT_PREFIX} "" PARENT_SCOPE)
    set(${_MCP_OUT_REASON}
        "no ncursesw on this host, using the system curses (narrow)"
        PARENT_SCOPE
    )
endfunction()
