#
# Architecture guardrails check (cross-platform, CMake-only).
#
# Usage:
#   cmake -P cmake/arch_rules.cmake
#

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    message(FATAL_ERROR "ARCH_RULES: must be run via 'cmake -P <script>'")
endif()

get_filename_component(
    _ARCH_RULES_SCRIPT_DIR
    "${CMAKE_SCRIPT_MODE_FILE}"
    DIRECTORY
)
get_filename_component(
    _ARCH_RULES_ROOT_DIR
    "${_ARCH_RULES_SCRIPT_DIR}"
    DIRECTORY
)

set(_ARCH_RULES_GLOBS
    "${_ARCH_RULES_ROOT_DIR}/src/*.c"
    "${_ARCH_RULES_ROOT_DIR}/src/*.cc"
    "${_ARCH_RULES_ROOT_DIR}/src/*.cpp"
    "${_ARCH_RULES_ROOT_DIR}/src/*.cxx"
    "${_ARCH_RULES_ROOT_DIR}/src/*.h"
    "${_ARCH_RULES_ROOT_DIR}/src/*.hpp"
    "${_ARCH_RULES_ROOT_DIR}/include/dsd-neo/*.h"
    "${_ARCH_RULES_ROOT_DIR}/include/dsd-neo/*.hpp"
)

file(
    GLOB_RECURSE _ARCH_RULES_FILES
    RELATIVE "${_ARCH_RULES_ROOT_DIR}"
    LIST_DIRECTORIES false
    ${_ARCH_RULES_GLOBS}
)

list(FILTER _ARCH_RULES_FILES EXCLUDE REGEX "^src/third_party/")
list(SORT _ARCH_RULES_FILES)

set(_ARCH_RULES_VIOLATIONS 0)

foreach(_ARCH_RULES_REL IN LISTS _ARCH_RULES_FILES)
    set(_ARCH_RULES_ABS "${_ARCH_RULES_ROOT_DIR}/${_ARCH_RULES_REL}")

    file(
        STRINGS "${_ARCH_RULES_ABS}"
        _ARCH_RULES_ALTERNATENAME_LINES
        REGEX
            "^[ \t]*#[ \t]*pragma[ \t]+comment[ \t]*\\([ \t]*linker[ \t]*,[ \t]*\"/alternatename:"
    )

    foreach(
        _ARCH_RULES_ALTERNATENAME_LINE
        IN
        LISTS _ARCH_RULES_ALTERNATENAME_LINES
    )
        string(
            STRIP "${_ARCH_RULES_ALTERNATENAME_LINE}"
            _ARCH_RULES_ALTERNATENAME_LINE_STRIPPED
        )
        message(
            SEND_ERROR
            "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden /alternatename pragma '${_ARCH_RULES_ALTERNATENAME_LINE_STRIPPED}'"
        )
        math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
    endforeach()

    if(_ARCH_RULES_REL MATCHES "^src/")
        file(
            STRINGS "${_ARCH_RULES_ABS}"
            _ARCH_RULES_WEAK_LINES
            REGEX "__attribute__\\s*\\(\\(weak\\)\\)"
        )

        foreach(_ARCH_RULES_WEAK_LINE IN LISTS _ARCH_RULES_WEAK_LINES)
            if(_ARCH_RULES_WEAK_LINE MATCHES "^[ \t]*(//|/\\*)")
                continue()
            endif()

            string(
                STRIP "${_ARCH_RULES_WEAK_LINE}"
                _ARCH_RULES_WEAK_LINE_STRIPPED
            )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden weak symbol usage '${_ARCH_RULES_WEAK_LINE_STRIPPED}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
        endforeach()
    endif()

    set(_ARCH_RULES_UI_FORBIDDEN_AREA OFF)
    if(
        _ARCH_RULES_REL
            MATCHES
            "^(src/dsp/|src/protocol/|include/dsd-neo/dsp/|include/dsd-neo/protocol/)"
    )
        set(_ARCH_RULES_UI_FORBIDDEN_AREA ON)
    endif()

    set(_ARCH_RULES_IO_FORBIDDEN_AREA OFF)
    if(
        _ARCH_RULES_REL
            MATCHES
            "^(src/core/|src/dsp/|src/protocol/|include/dsd-neo/core/|include/dsd-neo/dsp/|include/dsd-neo/protocol/)"
    )
        set(_ARCH_RULES_IO_FORBIDDEN_AREA ON)
    endif()

    set(_ARCH_RULES_ENGINE_FORBIDDEN_AREA OFF)
    if(_ARCH_RULES_REL MATCHES "^(src/engine/|include/dsd-neo/engine/)")
        set(_ARCH_RULES_ENGINE_FORBIDDEN_AREA ON)
    endif()

    set(_ARCH_RULES_FRONTEND_KIND_BACKEND_AREA OFF)
    if(
        _ARCH_RULES_REL
            MATCHES
            "^(src/dsp/|src/protocol/|src/engine/|include/dsd-neo/dsp/|include/dsd-neo/protocol/|include/dsd-neo/engine/)"
    )
        set(_ARCH_RULES_FRONTEND_KIND_BACKEND_AREA ON)
    endif()

    set(_ARCH_RULES_UI_RTL_FORBIDDEN_AREA OFF)
    if(_ARCH_RULES_REL MATCHES "^(src/ui/|include/dsd-neo/ui/)")
        set(_ARCH_RULES_UI_RTL_FORBIDDEN_AREA ON)
    endif()

    set(_ARCH_RULES_NATIVE_UI_AREA OFF)
    if(
        _ARCH_RULES_REL MATCHES "^src/ui/native/"
        OR _ARCH_RULES_REL STREQUAL "include/dsd-neo/ui/native_provider.h"
    )
        set(_ARCH_RULES_NATIVE_UI_AREA ON)
    endif()

    file(
        STRINGS "${_ARCH_RULES_ABS}"
        _ARCH_RULES_EXIT_LINES
        REGEX "(^|[^A-Za-z0-9_])exit[ \t]*\\("
    )

    foreach(_ARCH_RULES_EXIT_LINE IN LISTS _ARCH_RULES_EXIT_LINES)
        if(_ARCH_RULES_EXIT_LINE MATCHES "^[ \t]*(//|/\\*)")
            continue()
        endif()

        if(_ARCH_RULES_EXIT_LINE MATCHES "\"[^\"]*exit[ \t]*\\(")
            continue()
        endif()

        string(STRIP "${_ARCH_RULES_EXIT_LINE}" _ARCH_RULES_EXIT_LINE_STRIPPED)
        message(
            SEND_ERROR
            "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden exit() usage '${_ARCH_RULES_EXIT_LINE_STRIPPED}'"
        )
        math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
    endforeach()

    if(_ARCH_RULES_FRONTEND_KIND_BACKEND_AREA)
        file(
            STRINGS "${_ARCH_RULES_ABS}"
            _ARCH_RULES_FRONTEND_KIND_LINES
            REGEX "((->)|(\\.))frontend_kind"
        )

        foreach(
            _ARCH_RULES_FRONTEND_KIND_LINE
            IN
            LISTS _ARCH_RULES_FRONTEND_KIND_LINES
        )
            if(_ARCH_RULES_FRONTEND_KIND_LINE MATCHES "^[ \t]*(//|/\\*)")
                continue()
            endif()

            string(
                STRIP "${_ARCH_RULES_FRONTEND_KIND_LINE}"
                _ARCH_RULES_FRONTEND_KIND_LINE_STRIPPED
            )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: backend code must use frontend predicates, not direct frontend_kind access '${_ARCH_RULES_FRONTEND_KIND_LINE_STRIPPED}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
        endforeach()
    endif()

    if(_ARCH_RULES_UI_RTL_FORBIDDEN_AREA)
        file(
            STRINGS "${_ARCH_RULES_ABS}"
            _ARCH_RULES_UI_RTL_CALL_LINES
            REGEX "rtl_stream_"
        )

        foreach(
            _ARCH_RULES_UI_RTL_CALL_LINE
            IN
            LISTS _ARCH_RULES_UI_RTL_CALL_LINES
        )
            if(_ARCH_RULES_UI_RTL_CALL_LINE MATCHES "^[ \t]*(//|/\\*)")
                continue()
            endif()

            string(
                STRIP "${_ARCH_RULES_UI_RTL_CALL_LINE}"
                _ARCH_RULES_UI_RTL_CALL_LINE_STRIPPED
            )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: terminal UI must use app-control metrics, not rtl_stream_* directly '${_ARCH_RULES_UI_RTL_CALL_LINE_STRIPPED}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
        endforeach()
    endif()

    file(
        STRINGS "${_ARCH_RULES_ABS}"
        _ARCH_RULES_INCLUDE_LINES
        REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]+[>\"]"
    )

    if(NOT _ARCH_RULES_INCLUDE_LINES)
        continue()
    endif()

    foreach(_ARCH_RULES_LINE IN LISTS _ARCH_RULES_INCLUDE_LINES)
        string(
            REGEX REPLACE
                "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"][ \t]*.*"
            "\\1"
            _ARCH_RULES_HEADER
            "${_ARCH_RULES_LINE}"
        )

        if(_ARCH_RULES_NATIVE_UI_AREA)
            if(
                _ARCH_RULES_HEADER MATCHES "^dsd-neo/core/(opts|state)\\.h$"
                OR (
                    _ARCH_RULES_HEADER MATCHES "^dsd-neo/ui/"
                    AND NOT _ARCH_RULES_HEADER STREQUAL "dsd-neo/ui/native_provider.h"
                )
                OR _ARCH_RULES_HEADER MATCHES "^dsd-neo/ui/ncurses"
                OR _ARCH_RULES_HEADER MATCHES "^dsd-neo/ui/menu_"
                OR _ARCH_RULES_HEADER STREQUAL "dsd-neo/platform/curses_compat.h"
                OR _ARCH_RULES_HEADER MATCHES "(^|.*/)(curses|ncurses)\\.h$"
                OR _ARCH_RULES_HEADER
                    MATCHES
                    "(^|.*/)(dsd_ncurses_|ncurses_|menu_)[^/]*\\.h$"
                OR _ARCH_RULES_HEADER
                    MATCHES
                    "(^|.*/)(commands_internal|frontend_internal|snapshot_internal|telemetry_hooks_impl)\\.h$"
            )
                message(
                    SEND_ERROR
                    "ARCH_RULES: ${_ARCH_RULES_REL}: native UI must use app-control boundary, not '${_ARCH_RULES_HEADER}'"
                )
                math(
                    EXPR
                    _ARCH_RULES_VIOLATIONS
                    "${_ARCH_RULES_VIOLATIONS} + 1"
                )
                continue()
            endif()
        endif()

        if(
            _ARCH_RULES_UI_FORBIDDEN_AREA
            AND _ARCH_RULES_HEADER MATCHES "^dsd-neo/ui/"
        )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden UI include '${_ARCH_RULES_HEADER}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
            continue()
        endif()

        if(
            _ARCH_RULES_ENGINE_FORBIDDEN_AREA
            AND _ARCH_RULES_HEADER MATCHES "^dsd-neo/ui/"
        )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden UI include '${_ARCH_RULES_HEADER}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
            continue()
        endif()

        if(
            _ARCH_RULES_ENGINE_FORBIDDEN_AREA
            AND _ARCH_RULES_HEADER MATCHES "^dsd-neo/app_control/"
        )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden app-control include '${_ARCH_RULES_HEADER}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
            continue()
        endif()

        if(
            _ARCH_RULES_UI_FORBIDDEN_AREA
            AND _ARCH_RULES_HEADER MATCHES "^dsd-neo/engine/"
        )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden engine include '${_ARCH_RULES_HEADER}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
            continue()
        endif()

        if(
            _ARCH_RULES_IO_FORBIDDEN_AREA
            AND _ARCH_RULES_HEADER MATCHES "^dsd-neo/io/"
        )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden IO include '${_ARCH_RULES_HEADER}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
            continue()
        endif()

        if(
            _ARCH_RULES_UI_RTL_FORBIDDEN_AREA
            AND _ARCH_RULES_HEADER MATCHES "^dsd-neo/io/rtl"
        )
            message(
                SEND_ERROR
                "ARCH_RULES: ${_ARCH_RULES_REL}: terminal UI must use app-control metrics, not RTL include '${_ARCH_RULES_HEADER}'"
            )
            math(EXPR _ARCH_RULES_VIOLATIONS "${_ARCH_RULES_VIOLATIONS} + 1")
            continue()
        endif()

        if(_ARCH_RULES_HEADER STREQUAL "dsd-neo/platform/curses_compat.h")
            if(NOT _ARCH_RULES_REL MATCHES "^(src/ui/|include/dsd-neo/ui/)")
                message(
                    SEND_ERROR
                    "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden curses wrapper include '${_ARCH_RULES_HEADER}'"
                )
                math(
                    EXPR
                    _ARCH_RULES_VIOLATIONS
                    "${_ARCH_RULES_VIOLATIONS} + 1"
                )
            endif()
            continue()
        endif()

        if(
            NOT _ARCH_RULES_HEADER MATCHES "^dsd-neo/"
            AND _ARCH_RULES_HEADER MATCHES "(^|.*/)(curses|ncurses)\\.h$"
        )
            if(
                NOT (
                    _ARCH_RULES_REL
                        STREQUAL
                        "include/dsd-neo/platform/curses_compat.h"
                    OR _ARCH_RULES_REL MATCHES "^src/ui/"
                    OR _ARCH_RULES_REL MATCHES "^include/dsd-neo/ui/"
                )
            )
                message(
                    SEND_ERROR
                    "ARCH_RULES: ${_ARCH_RULES_REL}: forbidden curses include '${_ARCH_RULES_HEADER}'"
                )
                math(
                    EXPR
                    _ARCH_RULES_VIOLATIONS
                    "${_ARCH_RULES_VIOLATIONS} + 1"
                )
            endif()
            continue()
        endif()
    endforeach()
endforeach()

if(_ARCH_RULES_VIOLATIONS GREATER 0)
    message(
        FATAL_ERROR
        "ARCH_RULES: ${_ARCH_RULES_VIOLATIONS} violation(s) found"
    )
endif()

message(STATUS "ARCH_RULES: OK")
