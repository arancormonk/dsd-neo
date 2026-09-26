# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one negative control for dsd-neo_test_analog_replay (issue #518): a
# replay whose bounds must NOT hold. iq_decode_check.cmake requires exit code 0,
# so it can only show that bounds pass; this script shows that a missed bound
# fails. It requires all of (1) exit code EXPECTED_RC (default 1, a missed bound),
# (2) the expected failure regex in the combined output, (3) no "ANALOG AUDIO OK",
# (4) for exit code 1, an "ANALOG METRIC:" line, so the failure came from a
# scored replay and not from startup, (5) no match for the optional NOT_EXPECTED
# regex (a bound that must keep holding), and (6) no sanitizer report.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, EXPECTED; optional EXPECTED_RC,
# NOT_EXPECTED and TRAILING. MODE goes before "--iq-replay FIXTURE -o null" and
# TRAILING after it, so a host option can be the last argument on the command line.
foreach(_var DSD_BIN MODE FIXTURE EXPECTED)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "analog_replay_fail_check: missing -D${_var}")
    endif()
endforeach()
if(NOT DEFINED EXPECTED_RC)
    set(EXPECTED_RC 1)
endif()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")
set(_trailing_args)
if(DEFINED TRAILING)
    separate_arguments(_trailing_args UNIX_COMMAND "${TRAILING}")
endif()

execute_process(
    COMMAND
        "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}" -o
        null ${_trailing_args}
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
        "analog_replay_fail_check: sanitizer report detected:\n${_all}"
    )
endif()
if(NOT "${_rc}" STREQUAL "${EXPECTED_RC}")
    message(
        FATAL_ERROR
        "analog_replay_fail_check: host exited with '${_rc}' (expected ${EXPECTED_RC})\n${_all}"
    )
endif()
if(NOT "${_all}" MATCHES "${EXPECTED}")
    message(
        FATAL_ERROR
        "analog_replay_fail_check: expected failure /${EXPECTED}/ not found in output\n${_all}"
    )
endif()
if("${_all}" MATCHES "ANALOG AUDIO OK")
    message(
        FATAL_ERROR
        "analog_replay_fail_check: host reported ANALOG AUDIO OK on a run that must fail\n${_all}"
    )
endif()
if("${EXPECTED_RC}" STREQUAL "1" AND NOT "${_all}" MATCHES "ANALOG METRIC:")
    message(
        FATAL_ERROR
        "analog_replay_fail_check: no ANALOG METRIC line, so the replay was never scored\n${_all}"
    )
endif()
if(DEFINED NOT_EXPECTED AND NOT "${NOT_EXPECTED}" STREQUAL "")
    if("${_all}" MATCHES "${NOT_EXPECTED}")
        string(REGEX MATCH "${NOT_EXPECTED}" _hit "${_all}")
        message(
            FATAL_ERROR
            "analog_replay_fail_check: forbidden output /${NOT_EXPECTED}/ matched '${_hit}'\n${_all}"
        )
    endif()
endif()
