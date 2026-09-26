# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_* case: replays an I/Q fixture through dsd-neo and
# requires all of (1) exit code 0, (2) the expected payload regex in the
# combined output, and (3) no sanitizer report. CTest's PASS_REGULAR_EXPRESSION
# alone ignores the exit code, which would let a crash occurring after the
# first payload match pass silently.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, EXPECTED; optional NOT_EXPECTED
# and MIN_MATCHES (minimum number of non-overlapping EXPECTED matches).
foreach(_var DSD_BIN MODE FIXTURE EXPECTED)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_decode_check: missing -D${_var}")
    endif()
endforeach()

# MODE may carry several space-separated decoder flags (e.g. "-fs -F").
separate_arguments(_mode_args UNIX_COMMAND "${MODE}")

execute_process(
    COMMAND
        "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}" -o
        null
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
    message(FATAL_ERROR "iq_decode_check: sanitizer report detected:\n${_all}")
endif()
if(NOT "${_rc}" STREQUAL "0")
    message(
        FATAL_ERROR
        "iq_decode_check: dsd-neo exited with '${_rc}' (expected 0)\n${_all}"
    )
endif()
if(NOT "${_all}" MATCHES "${EXPECTED}")
    message(
        FATAL_ERROR
        "iq_decode_check: expected payload /${EXPECTED}/ not found in output\n${_all}"
    )
endif()
if(DEFINED MIN_MATCHES AND NOT "${MIN_MATCHES}" STREQUAL "")
    if(NOT "${MIN_MATCHES}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "iq_decode_check: MIN_MATCHES must be a positive integer")
    endif()
    string(REGEX MATCHALL "${EXPECTED}" _expected_matches "${_all}")
    list(LENGTH _expected_matches _expected_match_count)
    if(_expected_match_count LESS MIN_MATCHES)
        message(
            FATAL_ERROR
            "iq_decode_check: payload /${EXPECTED}/ matched ${_expected_match_count} times, expected at least ${MIN_MATCHES}\n${_all}"
        )
    endif()
endif()

# Optional -DNOT_EXPECTED: a regex that must be absent. Reject cases pair it with an
# EXPECTED line proving the run happened, so an empty run cannot pass by printing nothing.
if(DEFINED NOT_EXPECTED AND NOT "${NOT_EXPECTED}" STREQUAL "")
    if("${_all}" MATCHES "${NOT_EXPECTED}")
        string(REGEX MATCH "${NOT_EXPECTED}" _hit "${_all}")
        message(
            FATAL_ERROR
            "iq_decode_check: forbidden output /${NOT_EXPECTED}/ matched '${_hit}'\n${_all}"
        )
    endif()
endif()
