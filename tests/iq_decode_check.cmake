# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_* case: replays an I/Q fixture through dsd-neo and
# requires all of (1) exit code 0, (2) the expected payload regex in the
# combined output, and (3) no sanitizer report. CTest's PASS_REGULAR_EXPRESSION
# alone ignores the exit code, which would let a crash occurring after the
# first payload match pass silently.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, EXPECTED.
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
