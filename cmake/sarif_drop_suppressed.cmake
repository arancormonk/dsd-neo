# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# Drop already-suppressed results from a SARIF file (cross-platform, CMake-only).
#
# Semgrep reports a `nosemgrep`-suppressed match as a finding it did not count,
# but it still writes the match into SARIF carrying `suppressions: [{kind:
# inSource}]`. GitHub code scanning does not implement the SARIF `suppressions`
# property, so an uploaded suppression arrives as a plain open alert that no
# change to the tree can close - alerts #1777 and #1806 were both a documented
# `nosemgrep` that had to be dismissed by hand. Semgrep's own verdict is the
# gate; strip what it suppressed so the upload says the same thing.
#
# Usage:
#   cmake -DSARIF_FILE=<path> -P cmake/sarif_drop_suppressed.cmake
#

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    message(
        FATAL_ERROR
        "sarif_drop_suppressed: must be run via 'cmake -P <script>'"
    )
endif()
if(NOT SARIF_FILE)
    message(FATAL_ERROR "sarif_drop_suppressed: SARIF_FILE is required")
endif()
if(NOT EXISTS "${SARIF_FILE}")
    message(FATAL_ERROR "sarif_drop_suppressed: no such file: ${SARIF_FILE}")
endif()

file(READ "${SARIF_FILE}" _sds_sarif)

string(JSON _sds_runs ERROR_VARIABLE _sds_err LENGTH "${_sds_sarif}" runs)
if(_sds_err)
    message(
        FATAL_ERROR
        "sarif_drop_suppressed: ${SARIF_FILE} has no runs array: ${_sds_err}"
    )
endif()

set(_sds_dropped 0)
if(_sds_runs GREATER 0)
    math(EXPR _sds_last_run "${_sds_runs} - 1")
    foreach(_sds_run RANGE ${_sds_last_run})
        string(
            JSON _sds_results
            ERROR_VARIABLE _sds_err
            LENGTH "${_sds_sarif}"
            runs
            ${_sds_run}
            results
        )
        # A run may legitimately carry no results at all.
        if(_sds_err OR _sds_results LESS 1)
            continue()
        endif()
        math(EXPR _sds_last "${_sds_results} - 1")
        # Walk back to front so removing one result cannot shift an index we
        # have yet to visit.
        foreach(_sds_step RANGE ${_sds_last})
            math(EXPR _sds_idx "${_sds_last} - ${_sds_step}")
            string(
                JSON _sds_suppressions
                ERROR_VARIABLE _sds_err
                LENGTH "${_sds_sarif}"
                runs
                ${_sds_run}
                results
                ${_sds_idx}
                suppressions
            )
            # No `suppressions` key, or an empty one, means the tool is
            # reporting the result for real.
            if(_sds_err OR _sds_suppressions LESS 1)
                continue()
            endif()
            string(
                JSON _sds_sarif
                REMOVE "${_sds_sarif}"
                runs
                ${_sds_run}
                results
                ${_sds_idx}
            )
            math(EXPR _sds_dropped "${_sds_dropped} + 1")
        endforeach()
    endforeach()
endif()

if(_sds_dropped GREATER 0)
    file(WRITE "${SARIF_FILE}" "${_sds_sarif}")
    message(
        STATUS
        "sarif_drop_suppressed: dropped ${_sds_dropped} suppressed result(s) from ${SARIF_FILE}"
    )
else()
    message(
        STATUS
        "sarif_drop_suppressed: no suppressed results in ${SARIF_FILE}"
    )
endif()
