# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

# Issue #672 (B9): the terminal UI reads the published snapshot pair its frame
# acquired once, at the frame's start, in ui_frame_acquire() (ui_async.c). Both
# accessors refresh the single consumer's buffers in place, so any other call
# under src/ui/terminal/ (a label, an action, a callback, the RadioReference
# panel) would change what the rest of that frame reads. This audit allows each
# accessor exactly once, inside that helper.

get_filename_component(_DSD_NEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

file(
    GLOB_RECURSE _DSD_NEO_TERMINAL_FILES
    LIST_DIRECTORIES FALSE
    "${_DSD_NEO_ROOT}/src/ui/terminal/*.c"
    "${_DSD_NEO_ROOT}/src/ui/terminal/*.h"
)

set(_DSD_NEO_ACCESSOR_RE "dsd_app_get_latest_(opts_)?snapshot[ \t]*\\(")
set(_DSD_NEO_FRAME_FILE "src/ui/terminal/ui_async.c")
set(_DSD_NEO_HELPER_START_RE "^ui_frame_acquire\\(void\\) \\{$")

set(_DSD_NEO_AUDIT_FAILURES "")
set(_DSD_NEO_HELPER_OPTS 0)
set(_DSD_NEO_HELPER_STATE 0)
foreach(_DSD_NEO_FILE IN LISTS _DSD_NEO_TERMINAL_FILES)
    file(RELATIVE_PATH _DSD_NEO_REL "${_DSD_NEO_ROOT}" "${_DSD_NEO_FILE}")
    file(STRINGS "${_DSD_NEO_FILE}" _DSD_NEO_LINES)
    set(_DSD_NEO_IN_HELPER FALSE)
    foreach(_DSD_NEO_LINE IN LISTS _DSD_NEO_LINES)
        if(_DSD_NEO_REL STREQUAL _DSD_NEO_FRAME_FILE)
            if(_DSD_NEO_LINE MATCHES "${_DSD_NEO_HELPER_START_RE}")
                set(_DSD_NEO_IN_HELPER TRUE)
            elseif(_DSD_NEO_IN_HELPER AND _DSD_NEO_LINE MATCHES "^\\}")
                set(_DSD_NEO_IN_HELPER FALSE)
            endif()
        endif()
        if(NOT _DSD_NEO_LINE MATCHES "${_DSD_NEO_ACCESSOR_RE}")
            continue()
        endif()
        if(_DSD_NEO_IN_HELPER)
            if(_DSD_NEO_LINE MATCHES "dsd_app_get_latest_opts_snapshot")
                math(EXPR _DSD_NEO_HELPER_OPTS "${_DSD_NEO_HELPER_OPTS} + 1")
            else()
                math(EXPR _DSD_NEO_HELPER_STATE "${_DSD_NEO_HELPER_STATE} + 1")
            endif()
            continue()
        endif()
        string(STRIP "${_DSD_NEO_LINE}" _DSD_NEO_LINE)
        string(
            APPEND _DSD_NEO_AUDIT_FAILURES
            "\n  ${_DSD_NEO_REL}: ${_DSD_NEO_LINE}"
        )
    endforeach()
endforeach()

if(NOT _DSD_NEO_HELPER_OPTS EQUAL 1 OR NOT _DSD_NEO_HELPER_STATE EQUAL 1)
    string(
        APPEND _DSD_NEO_AUDIT_FAILURES
        "\n  ${_DSD_NEO_FRAME_FILE}: ui_frame_acquire() must call each accessor once (options ${_DSD_NEO_HELPER_OPTS}, state ${_DSD_NEO_HELPER_STATE})"
    )
endif()

if(_DSD_NEO_AUDIT_FAILURES)
    message(
        FATAL_ERROR
        "Terminal snapshot audit: only ui_frame_acquire() may acquire the published snapshots:${_DSD_NEO_AUDIT_FAILURES}"
    )
endif()

message(STATUS "Terminal snapshot audit passed")
