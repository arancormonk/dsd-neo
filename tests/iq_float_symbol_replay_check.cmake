# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_*_FLOAT_SYMBOL_REPLAY case: replays an I/Q fixture through dsd-neo with a soft symbol capture
# (-c <WORK_DIR>/capture.bin), turns the capture into a float symbol file of the same symbols
# (dsd-neo_test_symbol_capture_to_float: <WORK_DIR>/symbols.raw), and replays that (-i symbols.raw). Each run must exit
# 0, raise no sanitizer report and print the expected payload at least MIN_MATCHES times, and the float symbol replay
# as many times as the I/Q replay: a float symbol file decodes as the I/Q input its symbols came from.
#
# Expected -D inputs: DSD_BIN, CONVERT_BIN, MODE, FIXTURE, EXPECTED, WORK_DIR, MIN_MATCHES.
foreach(
    _var
    DSD_BIN
    CONVERT_BIN
    MODE
    FIXTURE
    EXPECTED
    WORK_DIR
    MIN_MATCHES
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_float_symbol_replay_check: missing -D${_var}")
    endif()
endforeach()
if(NOT "${MIN_MATCHES}" MATCHES "^[1-9][0-9]*$")
    message(
        FATAL_ERROR
        "iq_float_symbol_replay_check: MIN_MATCHES must be a positive integer"
    )
endif()

include("${CMAKE_CURRENT_LIST_DIR}/regex_match_count.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Every failure removes the work folder first: a test leaves no files behind, passing or not.
function(_float_replay_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "iq_float_symbol_replay_check: ${text}")
endfunction()

# One run's output: exit code 0, no sanitizer report, and EXPECTED at least MIN_MATCHES times; the count in <out_var>.
function(_float_replay_expect leg rc all out_var)
    if(
        "${all}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        _float_replay_fail("${leg}: sanitizer report detected:\n${all}")
    endif()
    if(NOT "${rc}" STREQUAL "0")
        _float_replay_fail("${leg}: exited with '${rc}' (expected 0)\n${all}")
    endif()
    dsd_test_count_regex_matches(_count "${EXPECTED}" "${all}")
    if(_count LESS MIN_MATCHES)
        _float_replay_fail(
            "${leg}: payload /${EXPECTED}/ matched ${_count} times, expected at least ${MIN_MATCHES}\n${all}"
        )
    endif()
    message(STATUS "iq_float_symbol_replay_check: ${leg}: ${_count} matches")
    set(${out_var} ${_count} PARENT_SCOPE)
endfunction()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")

execute_process(
    COMMAND
        "${DSD_BIN}" --frontend none ${_mode_args} --iq-replay "${FIXTURE}" -o
        null -c capture.bin
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 110
)
_float_replay_expect("I/Q replay" "${_rc}" "${_out}\n${_err}" _iq_count)

execute_process(
    COMMAND "${CONVERT_BIN}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 60
)
if(NOT "${_rc}" STREQUAL "0")
    _float_replay_fail("converter exited with '${_rc}'\n${_out}\n${_err}")
endif()

execute_process(
    COMMAND "${DSD_BIN}" --frontend none ${_mode_args} -i symbols.raw -o null
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 110
)
_float_replay_expect("float symbol replay" "${_rc}" "${_out}\n${_err}" _float_count)
if(NOT _float_count EQUAL _iq_count)
    _float_replay_fail(
        "the float symbol replay matched /${EXPECTED}/ ${_float_count} times, the I/Q replay ${_iq_count}"
    )
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
