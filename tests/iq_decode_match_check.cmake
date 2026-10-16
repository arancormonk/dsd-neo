# SPDX-License-Identifier: GPL-3.0-or-later
#
# Replays two I/Q captures of the same samples through dsd-neo with the same
# decoder flags and requires their decoded lines to be identical: both exit 0,
# no sanitizer report, and the lines matching LINE_REGEX (anchored at a line's
# start) are the same, in the same order, and at least MIN_LINES of them.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, REFERENCE, LINE_REGEX, MIN_LINES,
# WORK_DIR.
foreach(
    _var
    DSD_BIN
    MODE
    FIXTURE
    REFERENCE
    LINE_REGEX
    MIN_LINES
    WORK_DIR
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_decode_match_check: missing -D${_var}")
    endif()
endforeach()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(_n 0)
foreach(_capture "${FIXTURE}" "${REFERENCE}")
    math(EXPR _n "${_n} + 1")
    execute_process(
        COMMAND
            "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${_capture}"
            -o null
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 110
    )
    set(_all "${_out}\n${_err}")
    if(
        "${_all}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        message(
            FATAL_ERROR
            "iq_decode_match_check: sanitizer report detected:\n${_all}"
        )
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        message(
            FATAL_ERROR
            "iq_decode_match_check: dsd-neo exited with '${_rc}' on ${_capture}\n${_all}"
        )
    endif()
    file(WRITE "${WORK_DIR}/run${_n}.log" "${_all}")
    file(STRINGS "${WORK_DIR}/run${_n}.log" _lines${_n} REGEX "^${LINE_REGEX}")
    list(LENGTH _lines${_n} _count${_n})
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")

if(_count1 LESS MIN_LINES)
    message(
        FATAL_ERROR
        "iq_decode_match_check: ${FIXTURE} decoded ${_count1} lines matching /^${LINE_REGEX}/, expected at least ${MIN_LINES}"
    )
endif()
if(NOT "${_lines1}" STREQUAL "${_lines2}")
    message(
        FATAL_ERROR
        "iq_decode_match_check: ${FIXTURE} (${_count1} lines) and ${REFERENCE} (${_count2} lines) decode differently\n--- ${FIXTURE}\n${_lines1}\n--- ${REFERENCE}\n${_lines2}"
    )
endif()
message(STATUS "iq_decode_match_check: ${_count1} identical lines")
