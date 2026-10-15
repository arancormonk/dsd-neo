# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises tests/regex_match_count.cmake, which the decode check scripts count a payload regex's matches with
# (iq_decode_check.cmake, iq_float_symbol_replay_check.cmake, audio_stdout_check.cmake). A TETRA payload line such as
# "[TETRA NDB  NTS1  pol=1 rot=0  AACH 2/1/9]" gives matches holding one square bracket of a pair; a list of them,
# which string(REGEX MATCHALL) returns, does not split where it should, so each match is pinned to count once.
#
# Usage:
#   cmake -P tests/cmake/RegexMatchCount.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    message(
        FATAL_ERROR
        "REGEX_MATCH_COUNT: must be run via 'cmake -P <script>'"
    )
endif()

get_filename_component(_RMC_SCRIPT_DIR "${CMAKE_SCRIPT_MODE_FILE}" DIRECTORY)
get_filename_component(_RMC_TESTS_DIR "${_RMC_SCRIPT_DIR}" DIRECTORY)
include("${_RMC_TESTS_DIR}/regex_match_count.cmake")

function(_rmc_expect label regex text want)
    dsd_test_count_regex_matches(_got "${regex}" "${text}")
    if(NOT _got EQUAL want)
        message(
            FATAL_ERROR
            "REGEX_MATCH_COUNT: ${label}: /${regex}/ counted ${_got}, expected ${want}"
        )
    endif()
endfunction()

set(_rmc_payload
    "[TETRA NDB  NTS1  pol=1 rot=0  AACH 2/1/9]18:00:00 Sync: -TETRA NDB  [TETRA NDB  NTS1  pol=1 rot=0  AACH 2/1/9]\n"
)
string(
    APPEND _rmc_payload
    "[TETRA NDB  NTS1  pol=1 rot=0  AACH 2/1/9][TETRA SCH/F] tn=2 CRC OK\n"
)
_rmc_expect("closing bracket only" "AACH 2/1/9\\][^[]" "${_rmc_payload}" 2)
_rmc_expect("both brackets, closing first" "AACH 2/1/9\\]\\[TETRA" "${_rmc_payload}" 1)
_rmc_expect("opening bracket only" "\\[TETRA NDB" "${_rmc_payload}" 3)
_rmc_expect("balanced brackets" "\\[TETRA SCH/F\\] tn=2" "${_rmc_payload}" 1)
_rmc_expect("semicolon" "a;b" "a;b a;b a;b" 3)
_rmc_expect("no match" "AACH 3/51/51" "${_rmc_payload}" 0)
# Text that already holds the marker the helper would first choose.
_rmc_expect("marker in the text" "foo" "foo <dsd-test-regex-match>" 1)
_rmc_expect(
    "markers in the text"
    "foo"
    "foo <dsd-test-regex-match> <dsd-test-regex-match-1> foo"
    2
)

message(STATUS "REGEX_MATCH_COUNT: OK")
