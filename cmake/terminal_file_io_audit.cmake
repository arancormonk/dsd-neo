# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

# Issue #672 (B18): menu callbacks call app-control services and do no file
# I/O (AGENTS.md, docs/code_map.md "Terminal UI"). The terminal reads and
# writes files only through those services: the config file through
# <dsd-neo/app_control/config_files.h>, the imports directory (CSV picker,
# Imported Systems browser, RadioReference wizard) through
# <dsd-neo/app_control/import_files.h>. This audit refuses a direct file or
# directory call anywhere under src/ui/terminal/, comments and string literals
# aside. The one exception is the terminal's own lifecycle, which is no menu
# code: dsd_terminal_open() points stderr at the null device while curses owns
# the screen.
#
# A decided limit: the audit reads one line at a time, so a call whose name and
# opening parenthesis sit on different lines, or one reached through a macro or
# a function pointer, gets past it. Reviews catch those; none exists today.

get_filename_component(_DSD_NEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

file(
    GLOB_RECURSE _DSD_NEO_TERMINAL_FILES
    LIST_DIRECTORIES FALSE
    "${_DSD_NEO_ROOT}/src/ui/terminal/*.c"
    "${_DSD_NEO_ROOT}/src/ui/terminal/*.h"
)

# The C library's and the platform layer's file calls, and the runtime readers
# and writers that open files themselves.
set(_DSD_NEO_IO_NAMES
    "fopen|freopen|fdopen|fread|fwrite|remove|unlink|rename|mkdir|rmdir|opendir|readdir|stat|lstat|access"
    "|dsd_fopen[A-Za-z0-9_]*|dsd_rename[A-Za-z0-9_]*|dsd_replace_file_with_temp|dsd_fsync_parent_dir"
    "|dsd_dir_list|dsd_mkdir[A-Za-z0-9_]*|dsd_stat_path|dsd_fstat|dsd_path_canonical|dsd_path_free_bytes"
    "|dsd_resolve_existing_local_file|dsd_rr_provenance_read|dsd_rr_provenance_write"
    "|dsd_csv_validate_[A-Za-z0-9_]+|dsd_user_config_load[A-Za-z0-9_]*|dsd_user_config_save[A-Za-z0-9_]*"
    "|dsd_user_config_list_profiles|dsd_user_imports_dir_create"
)
string(JOIN "" _DSD_NEO_IO_NAMES ${_DSD_NEO_IO_NAMES})
set(_DSD_NEO_IO_RE "(^|[^A-Za-z0-9_.>])(${_DSD_NEO_IO_NAMES})[ \t]*\\(")

# file|code the exception allows, matched against the whole line with its
# comments and strings removed.
set(_DSD_NEO_IO_ALLOWED
    "src/ui/terminal/ncurses_init.c|FILE\\* devnull = dsd_fopen_private\\(dsd_null_device\\(\\), \"\"\\)\\;"
)

set(_DSD_NEO_AUDIT_FAILURES "")
set(_DSD_NEO_ALLOWED_SEEN 0)
foreach(_DSD_NEO_FILE IN LISTS _DSD_NEO_TERMINAL_FILES)
    file(RELATIVE_PATH _DSD_NEO_REL "${_DSD_NEO_ROOT}" "${_DSD_NEO_FILE}")
    file(STRINGS "${_DSD_NEO_FILE}" _DSD_NEO_LINES)
    set(_DSD_NEO_IN_BLOCK FALSE)
    foreach(_DSD_NEO_LINE IN LISTS _DSD_NEO_LINES)
        set(_DSD_NEO_CODE "${_DSD_NEO_LINE}")
        # The rest of a block comment opened on an earlier line.
        if(_DSD_NEO_IN_BLOCK)
            string(FIND "${_DSD_NEO_CODE}" "*/" _DSD_NEO_END)
            if(_DSD_NEO_END EQUAL -1)
                continue()
            endif()
            math(EXPR _DSD_NEO_END "${_DSD_NEO_END} + 2")
            string(
                SUBSTRING "${_DSD_NEO_CODE}"
                ${_DSD_NEO_END}
                -1
                _DSD_NEO_CODE
            )
            set(_DSD_NEO_IN_BLOCK FALSE)
        endif()
        # A double-quote character literal, then strings, then comments that
        # close on this line, then a line comment, then a block comment left
        # open.
        string(REGEX REPLACE "'\\\\?\"'" "''" _DSD_NEO_CODE "${_DSD_NEO_CODE}")
        string(
            REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\""
            "\"\""
            _DSD_NEO_CODE
            "${_DSD_NEO_CODE}"
        )
        string(
            REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/"
            " "
            _DSD_NEO_CODE
            "${_DSD_NEO_CODE}"
        )
        string(REGEX REPLACE "//.*$" "" _DSD_NEO_CODE "${_DSD_NEO_CODE}")
        string(FIND "${_DSD_NEO_CODE}" "/*" _DSD_NEO_OPEN)
        if(NOT _DSD_NEO_OPEN EQUAL -1)
            string(
                SUBSTRING "${_DSD_NEO_CODE}"
                0
                ${_DSD_NEO_OPEN}
                _DSD_NEO_CODE
            )
            set(_DSD_NEO_IN_BLOCK TRUE)
        endif()
        if(NOT _DSD_NEO_CODE MATCHES "${_DSD_NEO_IO_RE}")
            continue()
        endif()
        string(STRIP "${_DSD_NEO_CODE}" _DSD_NEO_CODE)
        set(_DSD_NEO_IS_ALLOWED FALSE)
        foreach(_DSD_NEO_ENTRY IN LISTS _DSD_NEO_IO_ALLOWED)
            string(FIND "${_DSD_NEO_ENTRY}" "|" _DSD_NEO_BAR)
            string(
                SUBSTRING "${_DSD_NEO_ENTRY}"
                0
                ${_DSD_NEO_BAR}
                _DSD_NEO_ALLOWED_FILE
            )
            math(EXPR _DSD_NEO_BAR "${_DSD_NEO_BAR} + 1")
            string(
                SUBSTRING "${_DSD_NEO_ENTRY}"
                ${_DSD_NEO_BAR}
                -1
                _DSD_NEO_ALLOWED_RE
            )
            if(
                _DSD_NEO_REL STREQUAL _DSD_NEO_ALLOWED_FILE
                AND _DSD_NEO_CODE MATCHES "^${_DSD_NEO_ALLOWED_RE}$"
            )
                set(_DSD_NEO_IS_ALLOWED TRUE)
            endif()
        endforeach()
        if(_DSD_NEO_IS_ALLOWED)
            math(EXPR _DSD_NEO_ALLOWED_SEEN "${_DSD_NEO_ALLOWED_SEEN} + 1")
            continue()
        endif()
        string(
            APPEND _DSD_NEO_AUDIT_FAILURES
            "\n  ${_DSD_NEO_REL}: ${_DSD_NEO_CODE}"
        )
    endforeach()
endforeach()

# The exception must still name real code: one that matches nothing has gone
# stale and would otherwise sit there allowing a future call.
list(LENGTH _DSD_NEO_IO_ALLOWED _DSD_NEO_ALLOWED_COUNT)
if(NOT _DSD_NEO_ALLOWED_SEEN EQUAL _DSD_NEO_ALLOWED_COUNT)
    string(
        APPEND _DSD_NEO_AUDIT_FAILURES
        "\n  allowed calls found ${_DSD_NEO_ALLOWED_SEEN}, listed ${_DSD_NEO_ALLOWED_COUNT}: update the exception list"
    )
endif()

if(_DSD_NEO_AUDIT_FAILURES)
    message(
        FATAL_ERROR
        "Terminal file I/O audit: the terminal UI does file I/O only through app-control services (config_files.h, import_files.h):${_DSD_NEO_AUDIT_FAILURES}"
    )
endif()

message(STATUS "Terminal file I/O audit passed")
