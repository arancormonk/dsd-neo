# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises cmake/sarif_drop_suppressed.cmake.
#
# Semgrep writes `nosemgrep`-suppressed matches into SARIF as results carrying
# `suppressions`, and GitHub code scanning ignores that property: uploading one
# opens an alert that no change to the tree can close. The filter is what keeps
# the upload equal to Semgrep's own verdict, so what it drops and - just as
# important - what it leaves alone are both pinned here.
#
# Usage:
#   cmake -DDSD_TEST_WORK_DIR=<dir> -P tests/cmake/SarifDropSuppressed.cmake

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    message(
        FATAL_ERROR
        "SARIF_DROP_SUPPRESSED: must be run via 'cmake -P <script>'"
    )
endif()
if(NOT DSD_TEST_WORK_DIR)
    message(FATAL_ERROR "SARIF_DROP_SUPPRESSED: DSD_TEST_WORK_DIR is required")
endif()

get_filename_component(_SDS_SCRIPT_DIR "${CMAKE_SCRIPT_MODE_FILE}" DIRECTORY)
get_filename_component(_SDS_TESTS_DIR "${_SDS_SCRIPT_DIR}" DIRECTORY)
get_filename_component(_SDS_ROOT_DIR "${_SDS_TESTS_DIR}" DIRECTORY)
set(_SDS_FILTER "${_SDS_ROOT_DIR}/cmake/sarif_drop_suppressed.cmake")

set(_SDS_FAILURES "")

function(_sds_expect what actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        set(_SDS_FAILURES
            "${_SDS_FAILURES}\n  - ${what}: expected '${expected}', got '${actual}'"
            PARENT_SCOPE
        )
    endif()
endfunction()

function(_sds_fail what)
    set(_SDS_FAILURES "${_SDS_FAILURES}\n  - ${what}" PARENT_SCOPE)
endfunction()

set(_SDS_WORK "${DSD_TEST_WORK_DIR}")
file(REMOVE_RECURSE "${_SDS_WORK}")
file(MAKE_DIRECTORY "${_SDS_WORK}")

# Runs, in order: one Semgrep-shaped run mixing two adjacent suppressed results
# with two reported ones, one run with no results key at all, and one whose only
# result is suppressed. The adjacent pair is the case a front-to-back walk gets
# wrong, because removing the first shifts the second onto an index it has
# already passed.
set(_SDS_INPUT "${_SDS_WORK}/scan.sarif")
file(
    WRITE "${_SDS_INPUT}"
    [==[
{
  "$schema": "https://docs.oasis-open.org/sarif/sarif/v2.1.0/os/schemas/sarif-schema-2.1.0.json",
  "version": "2.1.0",
  "runs": [
    {
      "tool": {"driver": {"name": "Semgrep OSS", "rules": [{"id": "keep-me"}]}},
      "results": [
        {"ruleId": "reported.plain", "message": {"text": "no suppressions key"}},
        {"ruleId": "suppressed.first", "message": {"text": "nosemgrep"},
         "suppressions": [{"kind": "inSource"}]},
        {"ruleId": "suppressed.second", "message": {"text": "nosemgrep"},
         "suppressions": [{"kind": "inSource"}]},
        {"ruleId": "reported.empty", "message": {"text": "empty suppressions"},
         "suppressions": []}
      ]
    },
    {
      "tool": {"driver": {"name": "Semgrep OSS"}}
    },
    {
      "tool": {"driver": {"name": "Semgrep OSS"}},
      "results": [
        {"ruleId": "suppressed.only", "message": {"text": "nosemgrep"},
         "suppressions": [{"kind": "inSource"}]}
      ]
    }
  ]
}
]==]
)

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSARIF_FILE=${_SDS_INPUT}" -P "${_SDS_FILTER}"
    RESULT_VARIABLE _SDS_RC
    OUTPUT_VARIABLE _SDS_OUT
    ERROR_VARIABLE _SDS_ERR
)
_sds_expect("filter exit status" "${_SDS_RC}" "0")
if(NOT "${_SDS_OUT}${_SDS_ERR}" MATCHES "dropped 3 suppressed")
    _sds_fail(
        "filter did not report dropping 3 results: ${_SDS_OUT}${_SDS_ERR}"
    )
endif()

file(READ "${_SDS_INPUT}" _SDS_FILTERED)

# The two reported results survive, in order, and both suppressed ones are gone.
string(JSON _SDS_LEN LENGTH "${_SDS_FILTERED}" runs 0 results)
_sds_expect("run 0 result count" "${_SDS_LEN}" "2")
if(_SDS_LEN EQUAL 2)
    string(
        JSON _SDS_ID
        GET "${_SDS_FILTERED}"
        runs
        0
        results
        0
        ruleId
    )
    _sds_expect("run 0 result 0" "${_SDS_ID}" "reported.plain")
    # An empty `suppressions` array is not a suppression: the tool is
    # reporting the result.
    string(
        JSON _SDS_ID
        GET "${_SDS_FILTERED}"
        runs
        0
        results
        1
        ruleId
    )
    _sds_expect("run 0 result 1" "${_SDS_ID}" "reported.empty")
endif()

# Everything outside `results` is left as it was - the rules metadata GitHub
# renders the alert from lives there.
string(
    JSON _SDS_RULE
    GET "${_SDS_FILTERED}"
    runs
    0
    tool
    driver
    rules
    0
    id
)
_sds_expect("run 0 rule survives" "${_SDS_RULE}" "keep-me")

# A run with no results key is not an error, and does not gain one.
string(JSON _SDS_LEN LENGTH "${_SDS_FILTERED}" runs)
_sds_expect("run count" "${_SDS_LEN}" "3")
string(
    JSON _SDS_LEN
    ERROR_VARIABLE _SDS_ERRV
    LENGTH "${_SDS_FILTERED}"
    runs
    1
    results
)
if(NOT _SDS_ERRV)
    _sds_fail("run 1 gained a results key")
endif()

# A run whose every result was suppressed keeps an empty array, not a stale one.
string(JSON _SDS_LEN LENGTH "${_SDS_FILTERED}" runs 2 results)
_sds_expect("run 2 result count" "${_SDS_LEN}" "0")

# Running again is a no-op: nothing left to drop, file unchanged.
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSARIF_FILE=${_SDS_INPUT}" -P "${_SDS_FILTER}"
    RESULT_VARIABLE _SDS_RC
    OUTPUT_VARIABLE _SDS_OUT
    ERROR_VARIABLE _SDS_ERR
)
_sds_expect("second run exit status" "${_SDS_RC}" "0")
file(READ "${_SDS_INPUT}" _SDS_AGAIN)
if(NOT "${_SDS_AGAIN}" STREQUAL "${_SDS_FILTERED}")
    _sds_fail("second run rewrote the file")
endif()

# A missing file is a hard error rather than a silent pass, so a mistyped path
# in CI cannot look like a clean scan.
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" "-DSARIF_FILE=${_SDS_WORK}/absent.sarif" -P
        "${_SDS_FILTER}"
    RESULT_VARIABLE _SDS_RC
    OUTPUT_VARIABLE _SDS_OUT
    ERROR_VARIABLE _SDS_ERR
)
if(_SDS_RC EQUAL 0)
    _sds_fail("missing SARIF file was accepted")
endif()

# So is a file that is not a SARIF log.
file(WRITE "${_SDS_WORK}/bogus.sarif" "{\"version\": \"2.1.0\"}\n")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" "-DSARIF_FILE=${_SDS_WORK}/bogus.sarif" -P
        "${_SDS_FILTER}"
    RESULT_VARIABLE _SDS_RC
    OUTPUT_VARIABLE _SDS_OUT
    ERROR_VARIABLE _SDS_ERR
)
if(_SDS_RC EQUAL 0)
    _sds_fail("SARIF without a runs array was accepted")
endif()

file(REMOVE_RECURSE "${_SDS_WORK}")

if(_SDS_FAILURES)
    message(FATAL_ERROR "SARIF_DROP_SUPPRESSED failed:${_SDS_FAILURES}")
endif()

message(STATUS "SARIF_DROP_SUPPRESSED: ok")
