# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_IQ_* case: replays an I/Q fixture through dsd-neo and
# requires all of (1) exit code 0, (2) the expected payload regex in the
# combined output, and (3) no sanitizer report. CTest's PASS_REGULAR_EXPRESSION
# alone ignores the exit code, which would let a crash occurring after the
# first payload match pass silently.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, EXPECTED; optional NOT_EXPECTED,
# MIN_MATCHES (minimum number of non-overlapping EXPECTED matches),
# EXPECTED2/MIN_MATCHES2 and EXPECTED3/MIN_MATCHES3 (further payloads the same
# run must print, at least as often; a MIN_MATCHESn of its own defaults to 1),
# EVENT_EXPECTED with WORK_DIR (a regex the run's event history must match),
# and AUDIO_MIN_BYTES with WORK_DIR (the run's decoded-audio WAV, -w, must hold
# at least that many bytes, not all of them zero).
foreach(_var DSD_BIN MODE FIXTURE EXPECTED)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_decode_check: missing -D${_var}")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/regex_match_count.cmake")

# MODE may carry several space-separated decoder flags (e.g. "-fs -F").
separate_arguments(_mode_args UNIX_COMMAND "${MODE}")

# Optional -DEVENT_EXPECTED, with -DWORK_DIR: the run also writes its event
# history (-J) into WORK_DIR, which must match EVENT_EXPECTED, for what a decoder
# publishes as a call rather than prints.
set(_event_args)
if(DEFINED EVENT_EXPECTED)
    if(NOT DEFINED WORK_DIR)
        message(FATAL_ERROR "iq_decode_check: EVENT_EXPECTED needs -DWORK_DIR")
    endif()
    file(REMOVE_RECURSE "${WORK_DIR}")
    file(MAKE_DIRECTORY "${WORK_DIR}")
    set(_event_log "${WORK_DIR}/events.log")
    set(_event_args -J "${_event_log}")
endif()

# Optional -DAUDIO_MIN_BYTES, with -DWORK_DIR: the run also saves its decoded
# audio (-w), which must be at least that long and not silence, for voice a
# decoder plays rather than prints.
set(_audio_args)
if(DEFINED AUDIO_MIN_BYTES)
    if(NOT DEFINED WORK_DIR)
        message(FATAL_ERROR "iq_decode_check: AUDIO_MIN_BYTES needs -DWORK_DIR")
    endif()
    if(NOT "${AUDIO_MIN_BYTES}" MATCHES "^[1-9][0-9]*$")
        message(
            FATAL_ERROR
            "iq_decode_check: AUDIO_MIN_BYTES must be a positive integer"
        )
    endif()
    if(NOT DEFINED EVENT_EXPECTED)
        file(REMOVE_RECURSE "${WORK_DIR}")
        file(MAKE_DIRECTORY "${WORK_DIR}")
    endif()
    set(_audio_wav "${WORK_DIR}/audio.wav")
    set(_audio_args -w "${_audio_wav}")
endif()

execute_process(
    COMMAND
        "${DSD_BIN}" --frontend none ${_mode_args} ${_event_args} ${_audio_args}
        --iq-replay "${FIXTURE}" -o null
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
        message(
            FATAL_ERROR
            "iq_decode_check: MIN_MATCHES must be a positive integer"
        )
    endif()
    dsd_test_count_regex_matches(_expected_match_count "${EXPECTED}" "${_all}")
    if(_expected_match_count LESS MIN_MATCHES)
        message(
            FATAL_ERROR
            "iq_decode_check: payload /${EXPECTED}/ matched ${_expected_match_count} times, expected at least ${MIN_MATCHES}\n${_all}"
        )
    endif()
endif()

foreach(_n 2 3)
    if(DEFINED EXPECTED${_n} AND NOT "${EXPECTED${_n}}" STREQUAL "")
        set(_min 1)
        if(DEFINED MIN_MATCHES${_n} AND NOT "${MIN_MATCHES${_n}}" STREQUAL "")
            if(NOT "${MIN_MATCHES${_n}}" MATCHES "^[1-9][0-9]*$")
                message(
                    FATAL_ERROR
                    "iq_decode_check: MIN_MATCHES${_n} must be a positive integer"
                )
            endif()
            set(_min "${MIN_MATCHES${_n}}")
        endif()
        dsd_test_count_regex_matches(_count "${EXPECTED${_n}}" "${_all}")
        if(_count LESS _min)
            message(
                FATAL_ERROR
                "iq_decode_check: payload /${EXPECTED${_n}}/ matched ${_count} times, expected at least ${_min}\n${_all}"
            )
        endif()
    endif()
endforeach()

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

if(DEFINED AUDIO_MIN_BYTES)
    set(_audio_size 0)
    set(_audio_hex "")
    if(EXISTS "${_audio_wav}")
        file(SIZE "${_audio_wav}" _audio_size)
        # Past the RIFF header: some sample must be nonzero.
        file(READ "${_audio_wav}" _audio_hex OFFSET 64 HEX)
    endif()
    if(_audio_size LESS AUDIO_MIN_BYTES)
        file(REMOVE_RECURSE "${WORK_DIR}")
        message(
            FATAL_ERROR
            "iq_decode_check: the decoded audio is ${_audio_size} bytes, expected at least ${AUDIO_MIN_BYTES}\n${_all}"
        )
    endif()
    if(NOT "${_audio_hex}" MATCHES "[1-9a-f]")
        file(REMOVE_RECURSE "${WORK_DIR}")
        message(FATAL_ERROR "iq_decode_check: the decoded audio is silence")
    endif()
    if(NOT DEFINED EVENT_EXPECTED)
        file(REMOVE_RECURSE "${WORK_DIR}")
    endif()
endif()

if(DEFINED EVENT_EXPECTED)
    set(_events "")
    if(EXISTS "${_event_log}")
        file(READ "${_event_log}" _events)
    endif()
    file(REMOVE_RECURSE "${WORK_DIR}")
    if(NOT "${_events}" MATCHES "${EVENT_EXPECTED}")
        message(
            FATAL_ERROR
            "iq_decode_check: /${EVENT_EXPECTED}/ not found in the event history\n${_events}"
        )
    endif()
endif()
