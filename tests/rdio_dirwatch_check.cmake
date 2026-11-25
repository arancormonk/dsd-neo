# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DECODE_*_RDIO_DIRWATCH_* case (issue #888): a digital call whose voice rows carry no talkgroup number,
# exported to an rdio-scanner DirWatch folder. Three legs of dsd-neo, each with MODE's options (the decode mode, and
# any export option the case adds, such as --rdio-system-by-frequency, issue #931) and -P -7 <calls> --wav-journal
# <journal> --rdio-mode dirwatch --rdio-system-id 48:
#
#   rf        replays the I/Q fixture, which knows its frequency, and saves its discriminator audio (-6 raw.wav). Every
#             recording has a sidecar holding RF_SIDECAR, and a freqList that names the sidecar's freq.
#   audio     decodes raw.wav as an audio input (no frequency) with --rdio-unknown-talkgroup AUDIO_TALKGROUP. Every
#             recording has a sidecar holding AUDIO_SIDECAR, and no freqList.
#   audio_off decodes raw.wav without the option: the recordings are published with no sidecar, and one log line says
#             why, once.
#
# With NO_FALLBACK set, the calls are ones the fallback may not number (a YSF call that does not end on a decoded
# terminator): no leg leaves a sidecar, and each says once that such a call is not exported (RF_SIDECAR and
# AUDIO_SIDECAR are then not read).
#
# Each leg must exit 0, print the EXPECTED payload regex and report no sanitizer error, and leave at least one
# recording and no TEMP_ file. Its journal has one "publish" line a recording, not failed, naming a file in the folder,
# with "target" TARGET (default empty: a call named by no text): the number is the export's alone, and the call's row
# keeps no talkgroup.
#
# Expected -D inputs: DSD_BIN, MODE, FIXTURE, EXPECTED, WORK_DIR, RF_SIDECAR, AUDIO_TALKGROUP, AUDIO_SIDECAR; optional
# TARGET, NO_FALLBACK. A *_SIDECAR is a comma-separated list of key=value pairs, each a whole field of the sidecar: a
# number, true, false or [] as it is, any other value as a JSON string.
foreach(
    _var
    DSD_BIN
    MODE
    FIXTURE
    EXPECTED
    WORK_DIR
    RF_SIDECAR
    AUDIO_TALKGROUP
    AUDIO_SIDECAR
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "rdio_dirwatch_check: missing -D${_var}")
    endif()
endforeach()

if(NOT DEFINED TARGET)
    set(TARGET "")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Every failure removes the work folder first: a test leaves no files behind, passing or not.
function(_rdio_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "rdio_dirwatch_check: ${text}")
endfunction()

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")
set(_raw "${WORK_DIR}/raw.wav")

# One leg into WORK_DIR/<leg>; @p ARGN are its input options. Sets <out_wavs> to the recordings its journal published,
# in order, and <out_log> to what it printed.
function(_rdio_leg leg out_wavs out_log)
    set(_dir "${WORK_DIR}/${leg}")
    set(_calls "${_dir}/calls")
    set(_journal "${_dir}/journal.jsonl")
    file(MAKE_DIRECTORY "${_dir}")
    # DSD_NEO_CONFIG unset, as for every run the tests start: a leg neither loads the operator's config nor autosaves
    # the leg's options into it (which would also hand -6 raw.wav to the next leg).
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env --unset=DSD_NEO_CONFIG "${DSD_BIN}"
            --frontend none ${_mode_args} ${ARGN} -o null -P -7 "${_calls}"
            --wav-journal "${_journal}" --rdio-mode dirwatch --rdio-system-id 48
        WORKING_DIRECTORY "${_dir}"
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
        _rdio_fail("${leg}: sanitizer report detected:\n${_all}")
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        _rdio_fail("${leg}: dsd-neo exited with '${_rc}' (expected 0)\n${_all}")
    endif()
    if(NOT "${_all}" MATCHES "${EXPECTED}")
        _rdio_fail("${leg}: expected payload /${EXPECTED}/ not found in output\n${_all}")
    endif()

    file(GLOB _wavs RELATIVE "${_calls}" "${_calls}/*.wav")
    file(GLOB _temps RELATIVE "${_calls}" "${_calls}/TEMP_*")
    list(LENGTH _wavs _wav_count)
    if(_wav_count LESS 1 OR _temps)
        _rdio_fail("${leg}: ${_wav_count} recordings, TEMP_ files: '${_temps}'\n${_all}")
    endif()

    set(_lines "")
    if(EXISTS "${_journal}")
        # One list item a line: file(STRINGS) also splits on ';' and treats '[' ']' as list syntax, which neither the
        # sanitized file names nor the journal fields checked here hold.
        file(STRINGS "${_journal}" _lines)
    endif()
    set(_ordered "")
    foreach(_line IN LISTS _lines)
        if(NOT _line MATCHES "\"type\":\"publish\"")
            continue()
        endif()
        if(
            NOT _line MATCHES "\"failed\":false"
            OR NOT _line MATCHES "\"target\":\"${TARGET}\""
        )
            _rdio_fail("${leg}: not a published call with no talkgroup: '${_line}'")
        endif()
        if(NOT _line MATCHES "\"file\":\"([^\"]+\\.wav)\"")
            _rdio_fail("${leg}: publish names no file: '${_line}'")
        endif()
        # The name, past a '/' or a JSON-escaped '\' (Windows) folder separator.
        string(REPLACE "\\\\" "/" _name "${CMAKE_MATCH_1}")
        get_filename_component(_name "${_name}" NAME)
        if(NOT EXISTS "${_calls}/${_name}")
            _rdio_fail("${leg}: published ${_name} is not in the folder")
        endif()
        list(APPEND _ordered "${_calls}/${_name}")
    endforeach()
    list(LENGTH _ordered _publish_count)
    if(NOT _publish_count EQUAL _wav_count)
        _rdio_fail("${leg}: ${_publish_count} publish lines for ${_wav_count} recordings\n${_lines}")
    endif()
    set(${out_wavs} "${_ordered}" PARENT_SCOPE)
    set(${out_log} "${_all}" PARENT_SCOPE)
endfunction()

# Every recording in @p wavs has a sidecar holding each of @p pairs as a whole field; with @p freq_list a freqList
# naming the sidecar's freq, without it none.
function(_rdio_check_sidecars leg wavs pairs freq_list)
    string(REPLACE "," ";" _pairs "${pairs}")
    foreach(_wav IN LISTS wavs)
        string(REGEX REPLACE "\\.wav$" ".json" _sidecar "${_wav}")
        if(NOT EXISTS "${_sidecar}")
            _rdio_fail("${leg}: ${_wav} has no sidecar")
        endif()
        file(READ "${_sidecar}" _json)
        foreach(_pair IN LISTS _pairs)
            string(REGEX MATCH "^([^=]+)=(.*)$" _unused "${_pair}")
            set(_key "${CMAKE_MATCH_1}")
            set(_value "${CMAKE_MATCH_2}")
            if(NOT _value MATCHES "^(-?[0-9]+|true|false|\\[\\])$")
                set(_value "\"${_value}\"")
            endif()
            # The value ends the field: a comma or the line's end follows it.
            string(FIND "${_json}" "\"${_key}\": ${_value}," _at)
            string(FIND "${_json}" "\"${_key}\": ${_value}\n" _at_end)
            if(_at LESS 0 AND _at_end LESS 0)
                _rdio_fail("${leg}: ${_sidecar} lacks \"${_key}\": ${_value}\n${_json}")
            endif()
        endforeach()
        string(FIND "${_json}" "\"freqList\"" _list_at)
        if(freq_list)
            if(NOT _json MATCHES "\"freq\": ([0-9]+),")
                _rdio_fail("${leg}: ${_sidecar} has no freq\n${_json}")
            endif()
            string(
                FIND "${_json}"
                "\"freqList\": [{\"pos\": 0, \"freq\": ${CMAKE_MATCH_1}}],"
                _list_at
            )
            if(_list_at LESS 0)
                _rdio_fail("${leg}: ${_sidecar} has no freqList naming its freq\n${_json}")
            endif()
        elseif(NOT _list_at LESS 0)
            _rdio_fail("${leg}: ${_sidecar} has a freqList with no frequency known\n${_json}")
        endif()
    endforeach()
endfunction()

# No recording in @p wavs has a sidecar, and @p log says @p notice once and nothing of an export that failed.
function(_rdio_check_unexported leg wavs log notice)
    foreach(_wav IN LISTS wavs)
        string(REGEX REPLACE "\\.wav$" ".json" _sidecar "${_wav}")
        if(EXISTS "${_sidecar}")
            _rdio_fail("${leg}: ${_wav} was exported with no number to give it")
        endif()
    endforeach()
    string(REGEX MATCHALL "${notice}" _notices "${log}")
    list(LENGTH _notices _notice_count)
    if(NOT _notice_count EQUAL 1)
        _rdio_fail("${leg}: ${_notice_count} '${notice}' log lines, expected one\n${log}")
    endif()
    if(log MATCHES "Rdio export( failed|: skipped)")
        _rdio_fail("${leg}: a skipped call was logged as a failure\n${log}")
    endif()
endfunction()

set(_no_number "set --rdio-unknown-talkgroup to export them")
set(_blocked "or it is a YSF call that did not end on its own terminator")

_rdio_leg(rf _rf_wavs _rf_log --iq-replay "${FIXTURE}" -6 "${_raw}")
if(NO_FALLBACK)
    _rdio_check_unexported(rf "${_rf_wavs}" "${_rf_log}" "${_blocked}")
else()
    _rdio_check_sidecars(rf "${_rf_wavs}" "${RF_SIDECAR}" ON)
endif()
if(NOT EXISTS "${_raw}")
    _rdio_fail("rf: no discriminator audio saved for the audio legs")
endif()

_rdio_leg(audio _audio_wavs _audio_log -i "${_raw}" --rdio-unknown-talkgroup "${AUDIO_TALKGROUP}")
if(NO_FALLBACK)
    _rdio_check_unexported(audio "${_audio_wavs}" "${_audio_log}" "${_blocked}")
else()
    _rdio_check_sidecars(audio "${_audio_wavs}" "talkgroup=${AUDIO_TALKGROUP},${AUDIO_SIDECAR}" OFF)
endif()

_rdio_leg(audio_off _off_wavs _off_log -i "${_raw}")
if(NO_FALLBACK)
    _rdio_check_unexported(audio_off "${_off_wavs}" "${_off_log}" "${_blocked}")
else()
    _rdio_check_unexported(audio_off "${_off_wavs}" "${_off_log}" "${_no_number}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
