# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one per-call NFM recording case (issue #756) through the analog replay
# host (tests/engine/analog_replay.c), which runs the real engine: MODE plus
# `-P -7 <dir> --wav-journal <file>`, then reads what the run left on disk.
# Each leg must, as in iq_decode_check.cmake, exit 0, show no sanitizer report
# and print "ANALOG AUDIO OK", and then:
#   - its folder holds exactly FILES recordings (*.wav), and no TEMP_ file;
#   - its journal has FILES "publish" lines, each an analog recording pending its
#     call's history row and not failed, its system the call's mode (NFM from an
#     I/Q replay, ANALOG from the discriminator tap, an audio input whose mode is
#     unknown), naming a file in the folder, and one
#     "link" line for each of them; no "unlink" and no "error" line;
#   - the host printed FILES "ANALOG CALL ROW:" lines: one history row a file.
#
#   - each published recording's journal duration_ms, in publication order, is
#     within one 20 ms block of its DURATIONS_MS entry.
#
# Expected -D inputs: HOST_BIN, MODE, WORK_DIR, FILES, DURATIONS_MS (a
# comma-separated list of FILES expected lengths in ms). Optional:
#   RATES         a comma-separated list of at least two --iq-replay-rate
#                 values, one leg each (one leg without the option when
#                 unset). Every leg after the first
#                 must match the first: the same WAV payloads in publication
#                 order, the same journal start, duration_ms, epoch and row
#                 values, and the same rows.
#   MIN_REALTIME_S  the least whole seconds a "realtime" leg may take, so a
#                 pacing the run ignored fails instead of comparing two fast
#                 legs.
#   HELD_AT_END_MS  the input must end less than this long after the last
#                 audible audio (the host's total_ms and last_audible_ms), so
#                 the run's last call was still held through a pause when the
#                 input ended and the engine's normal teardown closed it.
foreach(
    _var
    HOST_BIN
    MODE
    WORK_DIR
    FILES
    DURATIONS_MS
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "analog_recording_check: missing -D${_var}")
    endif()
endforeach()
if(NOT "${FILES}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "analog_recording_check: FILES must be a count")
endif()
string(REPLACE "," ";" _expected_durations "${DURATIONS_MS}")
list(LENGTH _expected_durations _expected_duration_count)
if(NOT _expected_duration_count EQUAL FILES)
    message(
        FATAL_ERROR
        "analog_recording_check: DURATIONS_MS names ${_expected_duration_count} lengths for ${FILES} recordings"
    )
endif()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")
if(MODE MATCHES "--analog-pcm-tap")
    set(_system "ANALOG")
else()
    set(_system "NFM")
endif()
set(_rates "-")
if(DEFINED RATES)
    string(REPLACE "," ";" _rates "${RATES}")
    list(LENGTH _rates _rate_count)
    if(_rate_count LESS 2)
        message(
            FATAL_ERROR
            "analog_recording_check: RATES names ${_rate_count} leg(s); a comparison needs two"
        )
    endif()
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Runs one leg into WORK_DIR/<leg>; sets <out_files> to its recordings in
# publication order and <out_summary> to what the legs must agree on.
function(_analog_recording_leg leg rate out_files out_summary)
    set(_dir "${WORK_DIR}/${leg}")
    set(_calls "${_dir}/calls")
    set(_journal "${_dir}/journal.jsonl")
    file(MAKE_DIRECTORY "${_dir}")
    set(_rate_args)
    if(NOT rate STREQUAL "-")
        set(_rate_args --iq-replay-rate "${rate}")
    endif()
    string(TIMESTAMP _began "%s" UTC)
    execute_process(
        COMMAND
            "${HOST_BIN}" --frontend none ${_mode_args} ${_rate_args} -P -7
            "${_calls}" --wav-journal "${_journal}" -o null
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 110
    )
    string(TIMESTAMP _ended "%s" UTC)
    set(_all "${_out}\n${_err}")
    if(
        "${_all}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        message(
            FATAL_ERROR
            "analog_recording_check: ${leg}: sanitizer report detected:\n${_all}"
        )
    endif()
    if(NOT "${_rc}" STREQUAL "0" OR NOT "${_all}" MATCHES "ANALOG AUDIO OK")
        message(
            FATAL_ERROR
            "analog_recording_check: ${leg}: exited with '${_rc}' or without ANALOG AUDIO OK\n${_all}"
        )
    endif()
    if(rate STREQUAL "realtime" AND DEFINED MIN_REALTIME_S)
        math(EXPR _took "${_ended} - ${_began}")
        if(_took LESS MIN_REALTIME_S)
            message(
                FATAL_ERROR
                "analog_recording_check: ${leg}: took ${_took} s, expected at least ${MIN_REALTIME_S} s paced"
            )
        endif()
    endif()

    file(GLOB _wavs RELATIVE "${_calls}" "${_calls}/*.wav")
    file(GLOB _temps RELATIVE "${_calls}" "${_calls}/TEMP_*")
    list(LENGTH _wavs _wav_count)
    if(NOT _wav_count EQUAL FILES OR _temps)
        message(
            FATAL_ERROR
            "analog_recording_check: ${leg}: ${_wav_count} recordings (expected ${FILES}), TEMP_ files: '${_temps}'\n${_wavs}\n${_all}"
        )
    endif()

    set(_lines "")
    if(EXISTS "${_journal}")
        # One list item a line: file(STRINGS) also splits on ';' and treats '[' ']' as list syntax, which neither the
        # sanitized NFM file names nor the journal fields checked here hold.
        file(STRINGS "${_journal}" _lines)
    endif()
    set(_published "")
    set(_linked "")
    set(_ordered "")
    set(_summary "")
    set(_durations "")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "\"type\":\"(unlink|error)\"")
            message(
                FATAL_ERROR
                "analog_recording_check: ${leg}: journal line '${_line}'"
            )
        endif()
        if(_line MATCHES "\"type\":\"publish\"")
            if(
                NOT _line MATCHES "\"id\":\"[^\"]+\""
                OR NOT _line MATCHES "\"pending\":true"
                OR NOT _line MATCHES "\"failed\":false"
                OR NOT _line MATCHES "\"system\":\"${_system}\""
            )
                message(
                    FATAL_ERROR
                    "analog_recording_check: ${leg}: not a pending analog publish: '${_line}'"
                )
            endif()
            string(REGEX MATCH "\"id\":\"([^\"]+)\"" _unused "${_line}")
            list(APPEND _published "${CMAKE_MATCH_1}")
            if(NOT _line MATCHES "\"file\":\"([^\"]+\\.wav)\"")
                message(
                    FATAL_ERROR
                    "analog_recording_check: ${leg}: publish names no file: '${_line}'"
                )
            endif()
            # The name, past a '/' or a JSON-escaped '\' (Windows) folder separator.
            string(REPLACE "\\\\" "/" _name "${CMAKE_MATCH_1}")
            get_filename_component(_name "${_name}" NAME)
            if(NOT EXISTS "${_calls}/${_name}")
                message(
                    FATAL_ERROR
                    "analog_recording_check: ${leg}: published ${_name} is not in the folder"
                )
            endif()
            list(APPEND _ordered "${_calls}/${_name}")
            string(REGEX MATCH "\"start\":[0-9]+" _start "${_line}")
            string(REGEX MATCH "\"duration_ms\":([0-9]+)" _duration "${_line}")
            list(APPEND _durations "${CMAKE_MATCH_1}")
            string(REGEX MATCH "\"epoch\":[0-9]+" _epoch "${_line}")
            list(APPEND _summary "publish ${_start} ${_duration} ${_epoch}")
        elseif(_line MATCHES "\"type\":\"link\"")
            if(
                NOT _line MATCHES "\"id\":\"[^\"]+\""
                OR NOT _line MATCHES "\"row\":[0-9]+"
            )
                message(
                    FATAL_ERROR
                    "analog_recording_check: ${leg}: link names no recording or row: '${_line}'"
                )
            endif()
            string(REGEX MATCH "\"id\":\"([^\"]+)\"" _unused "${_line}")
            set(_id "${CMAKE_MATCH_1}")
            string(REGEX MATCH "\"row\":([0-9]+)" _unused "${_line}")
            list(APPEND _linked "${_id}")
            list(APPEND _summary "link row ${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(LENGTH _published _publish_count)
    if(NOT _publish_count EQUAL FILES)
        message(
            FATAL_ERROR
            "analog_recording_check: ${leg}: ${_publish_count} publish lines, expected ${FILES}\n${_lines}"
        )
    endif()
    # The journal keeps time: a burst alone is the burst and the hangover, and a joined call adds its pauses, each at
    # block precision (20 ms).
    foreach(_got _want IN ZIP_LISTS _durations _expected_durations)
        math(EXPR _off "${_got} - ${_want}")
        if(_off GREATER 20 OR _off LESS -20)
            string(REPLACE ";" "," _got_text "${_durations}")
            message(
                FATAL_ERROR
                "analog_recording_check: ${leg}: journal durations ${_got_text} ms, expected ${DURATIONS_MS} (+/-20 ms each)"
            )
        endif()
    endforeach()
    foreach(_id IN LISTS _published)
        list(FIND _linked "${_id}" _at)
        list(REMOVE_ITEM _linked "${_id}")
        if(_at LESS 0)
            message(
                FATAL_ERROR
                "analog_recording_check: ${leg}: recording ${_id} was never linked to its row\n${_lines}"
            )
        endif()
    endforeach()
    if(_linked)
        message(
            FATAL_ERROR
            "analog_recording_check: ${leg}: links to no published recording: ${_linked}"
        )
    endif()

    string(REGEX MATCHALL "ANALOG CALL ROW: [^\n]*" _rows "${_err}")
    list(LENGTH _rows _row_count)
    if(NOT _row_count EQUAL FILES)
        message(
            FATAL_ERROR
            "analog_recording_check: ${leg}: ${_row_count} NFM history rows, expected ${FILES}\n${_all}"
        )
    endif()
    list(APPEND _summary ${_rows})

    if(DEFINED HELD_AT_END_MS)
        if(
            NOT _err
                MATCHES
                "ANALOG METRIC: [^\n]* total_ms=([0-9.]+) [^\n]* last_audible_ms=([0-9.]+)"
        )
            message(
                FATAL_ERROR
                "analog_recording_check: ${leg}: no total_ms and last_audible_ms\n${_all}"
            )
        endif()
        # Whole milliseconds are enough to compare against the bound.
        set(_total "${CMAKE_MATCH_1}")
        set(_last "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "\\..*" "" _total "${_total}")
        string(REGEX REPLACE "\\..*" "" _last "${_last}")
        math(EXPR _after "${_total} - ${_last}")
        if(NOT _after LESS HELD_AT_END_MS)
            message(
                FATAL_ERROR
                "analog_recording_check: ${leg}: the input ended ${_after} ms after the last audio, not within ${HELD_AT_END_MS} ms"
            )
        endif()
    endif()

    set(${out_files} "${_ordered}" PARENT_SCOPE)
    set(${out_summary} "${_summary}" PARENT_SCOPE)
endfunction()

set(_leg 0)
foreach(_rate IN LISTS _rates)
    _analog_recording_leg("leg${_leg}" "${_rate}" _files _summary)
    if(_leg EQUAL 0)
        set(_first_rate "${_rate}")
        set(_first_files "${_files}")
        set(_first_summary "${_summary}")
    else()
        if(NOT _summary STREQUAL _first_summary)
            string(REPLACE ";" "\n" _a "${_first_summary}")
            string(REPLACE ";" "\n" _b "${_summary}")
            message(
                FATAL_ERROR
                "analog_recording_check: ${_rate} differs from ${_first_rate}:\n${_a}\n--- vs ---\n${_b}"
            )
        endif()
        foreach(_a _b IN ZIP_LISTS _first_files _files)
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E compare_files "${_a}" "${_b}"
                RESULT_VARIABLE _same
            )
            if(NOT _same EQUAL 0)
                message(
                    FATAL_ERROR
                    "analog_recording_check: ${_rate} recorded ${_b}, which differs from ${_first_rate}'s ${_a}"
                )
            endif()
        endforeach()
    endif()
    math(EXPR _leg "${_leg} + 1")
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
