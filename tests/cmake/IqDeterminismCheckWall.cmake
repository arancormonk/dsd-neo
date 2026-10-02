# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises the realtime pacing check of tests/iq_determinism_check.cmake
# (issue #572). The runner takes a realtime leg's wall time from the host's
# REPLAY WALL line only, never from a clock of its own, so it is run here on a
# fast and a realtime leg of a stand-in host
# (tests/engine/test_iq_determinism_check_wall.c) that prints a passing leg of
# a 1 s capture (media_ms=1000) and whatever REPLAY WALL line the case gives it.
# A host that leaves the line out, or prints it without wall_ms, must fail the
# case; 899 ms of wall time must fail the 90 % floor and 900 ms must meet it.
#
# Expected -D inputs: FAKE_HOST, RUNNER.

cmake_minimum_required(VERSION 3.20)

foreach(_var FAKE_HOST RUNNER)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "IQ_DETERMINISM_CHECK_WALL: missing -D${_var}")
    endif()
endforeach()

# Runs the runner with the stand-in host printing "REPLAY WALL: <wall>" (no
# such line for "omit"). The runner must pass when want_pass is TRUE and fail
# otherwise, and want_regex must match its output, the line breaks CMake wraps
# its messages at folded into single spaces.
function(_check_case wall want_pass want_regex)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" "-DHOST_BIN=${FAKE_HOST}"
            "-DMODE=--fake-wall ${wall}" "-DFIXTURE=unused.iq.json"
            "-DRUNS=fast;realtime" "-DEXPECTED=Src=901" "-DMIN_FSK=0"
            "-DMIN_CQPSK=0" "-DMIN_TOTAL=0" -P "${RUNNER}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 60
    )
    set(_all "${_out}\n${_err}")
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_all}")
    if(want_pass AND NOT "${_rc}" STREQUAL "0")
        message(
            FATAL_ERROR
            "IQ_DETERMINISM_CHECK_WALL: '${wall}': the runner failed with '${_rc}', expected it to pass\n${_all}"
        )
    endif()
    if(NOT want_pass AND "${_rc}" STREQUAL "0")
        message(
            FATAL_ERROR
            "IQ_DETERMINISM_CHECK_WALL: '${wall}': the runner passed, expected it to fail\n${_all}"
        )
    endif()
    if(NOT _flat MATCHES "${want_regex}")
        message(
            FATAL_ERROR
            "IQ_DETERMINISM_CHECK_WALL: '${wall}': the runner's output does not match /${want_regex}/\n${_all}"
        )
    endif()
    message("IQ_DETERMINISM_CHECK_WALL: '${wall}' ok")
endfunction()

_check_case(
    omit
    FALSE
    "leg #1 fast: expected one REPLAY WALL line, found 0"
)
_check_case(
    elapsed_ms=900.000000
    FALSE
    "leg #1 fast: no wall_ms on the host's REPLAY WALL line [(]REPLAY WALL: elapsed_ms=900[.]000000[)]"
)
_check_case(
    wall_ms=899.999999
    FALSE
    "leg #2 realtime: a realtime leg took 899 ms of wall time for 1000 ms of air time [(]need at least 90 %[)]"
)
_check_case(
    wall_ms=900.000000
    TRUE
    "iq_determinism_check: 2 legs decoded identically"
)
