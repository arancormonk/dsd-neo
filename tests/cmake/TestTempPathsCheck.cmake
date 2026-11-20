# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs tests/platform/test_platform_test_temp_paths.c once per mode, each with
# DSD_NEO_TEST_TMPDIR set to a fresh folder, and checks that the folder is empty
# afterwards, whether the run removed its paths itself, left them, or aborted,
# together with the exit code (a run that leaves a path fails, though it
# returned 0) and the cleanup lines on stderr.
#
# Expected -D inputs: EXE (the test program), WORK (a scratch folder this
# script owns and removes).

foreach(_var EXE WORK)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(_failures "")

# Run @p mode (extra arguments after it) in its own folder; @p want_rc is 0 or
# "fail"; @p want_err and @p refuse_err are regular expressions stderr must and
# must not match ("" for none).
function(_run_mode mode want_rc want_err refuse_err)
    set(_dir "${WORK}/${mode}")
    file(MAKE_DIRECTORY "${_dir}")
    execute_process(
        COMMAND
            ${CMAKE_COMMAND} -E env "DSD_NEO_TEST_TMPDIR=${_dir}" "${EXE}"
            ${mode} ${ARGN}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
    )
    set(_problems "")
    if(want_rc STREQUAL "fail")
        if(_rc STREQUAL "0")
            list(APPEND _problems "exited 0, expected a failure")
        endif()
    elseif(NOT _rc STREQUAL "${want_rc}")
        list(APPEND _problems "exit ${_rc}, expected ${want_rc}")
    endif()
    if(NOT want_err STREQUAL "" AND NOT _err MATCHES "${want_err}")
        list(APPEND _problems "stderr does not match '${want_err}'")
    endif()
    if(NOT refuse_err STREQUAL "" AND _err MATCHES "${refuse_err}")
        list(APPEND _problems "stderr matches '${refuse_err}'")
    endif()
    file(
        GLOB_RECURSE _left
        LIST_DIRECTORIES true
        RELATIVE "${_dir}"
        "${_dir}/*"
    )
    if(_left)
        list(APPEND _problems "left behind: ${_left}")
    endif()
    if(_problems)
        string(REPLACE ";" "; " _problems "${_problems}")
        set(_failures
            "${_failures}  ${mode}: ${_problems}\n    stderr: ${_err}\n"
            PARENT_SCOPE
        )
    endif()
endfunction()

set(_line "test temp cleanup: removed [^\n]*")
_run_mode(remove 0 "" "test temp cleanup")
set(_fails "test temp cleanup: failing the test")
_run_mode(
    leave
    fail
    "${_line}dsdneo_temp_paths_dir_[^\n]*left behind.*${_line}dsdneo_temp_paths_file_[^\n]*left behind.*${_fails}"
    "could not be removed"
)
_run_mode(
    abort
    fail
    "${_line}dsdneo_temp_paths_dir_[^\n]*after the test aborted.*${_line}dsdneo_temp_paths_file_[^\n]*after the test aborted"
    "could not be removed"
)
_run_mode(
    many
    fail
    "${_line}dsdneo_temp_paths_last_[^\n]*left behind.*${_fails}"
    "not tracked"
)
if(NOT CMAKE_HOST_WIN32)
    # The link's target lies outside the folder the run cleans, and must survive.
    set(_target "${WORK}/outside")
    file(MAKE_DIRECTORY "${_target}")
    file(WRITE "${_target}/keep.txt" "keep\n")
    _run_mode(
        link
        fail
        "${_line}dsdneo_temp_paths_link_[^\n]*left behind"
        ""
        "${_target}"
    )
    if(NOT EXISTS "${_target}/keep.txt")
        string(
            APPEND _failures
            "  link: the cleanup followed the link and removed its target\n"
        )
    endif()
endif()

if(NOT _failures STREQUAL "")
    message(FATAL_ERROR "TEST_TEMP_PATHS:\n${_failures}")
endif()
file(REMOVE_RECURSE "${WORK}")
message(STATUS "TEST_TEMP_PATHS: OK")
