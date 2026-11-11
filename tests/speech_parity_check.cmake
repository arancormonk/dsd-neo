# SPDX-License-Identifier: GPL-3.0-or-later
#
# Speech level parity (issue #518): plays one deterministic talker through dsd-neo as digital voice and, when FIXTURE_DIR
# is given, as analog FM and AM, and hands every output to tests/core/speech_parity_tool.c. Established speech must
# play within 3 dB of the speech AGC's -24 dBFS active speech level in every rendition, and all renditions within 1.5 dB
# of each other.
#
# Digital: the tool encodes the talker with mbelib-neo's IMBE 4400, AMBE+2 2450 and AMBE 2400 encoders, and `-r` plays
# each file through the voice chain (8 kHz mono on stdout). Analog: the committed speech_*_synth I/Q fixtures carry the
# same talker (tools/build_iq_fixtures.py), replayed at the default auto gain (48 kHz mono on stdout).
#
# Expected -D inputs: DSD_BIN, TOOL_BIN, WORK_DIR. Optional: FIXTURE_DIR (adds the analog renditions).
foreach(_var DSD_BIN TOOL_BIN WORK_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "speech_parity_check: missing -D${_var}")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Every failure removes the work folder first: a test leaves no files behind, passing or not.
function(_parity_fail text)
    file(REMOVE_RECURSE "${WORK_DIR}")
    message(FATAL_ERROR "speech_parity_check: ${text}")
endfunction()

function(_parity_run what out)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE _rc
        OUTPUT_FILE "${WORK_DIR}/${out}"
        ERROR_VARIABLE _err
        TIMEOUT 100
    )
    if(
        "${_err}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        _parity_fail("${what}: sanitizer report detected:\n${_err}")
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        _parity_fail("${what}: exited with '${_rc}' (expected 0)\n${_err}")
    endif()
endfunction()

set(_renditions)
foreach(_codec imbe:imb ambe2450:amb ambe2400:dmb)
    string(REPLACE ":" ";" _pair "${_codec}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _ext)
    _parity_run("encode ${_name}" "encode_${_name}.log" "${TOOL_BIN}" encode ${_name} "talker.${_ext}")
    _parity_run("play talker.${_ext}" "${_name}.raw" "${DSD_BIN}" --frontend none -r "talker.${_ext}" -o -)
    list(APPEND _renditions "${_name}:${_name}.raw:8000:1:0")
endforeach()

if(DEFINED FIXTURE_DIR)
    # The carrier keys this long before the talker starts (SPEECH_SYNTH_LEAD_S in tools/build_iq_fixtures.py).
    set(_lead_s 0.25)
    # name, fixture, mode
    foreach(
        _case
        "fm_nb|speech_nfm_synth|-fA"
        "fm_wb|speech_wfm_synth|-fA --nfm-bandwidth-hz 25000"
        "am|speech_am_synth|-fM"
    )
        string(REPLACE "|" ";" _fields "${_case}")
        list(GET _fields 0 _name)
        list(GET _fields 1 _fixture)
        list(GET _fields 2 _mode)
        separate_arguments(_mode_args UNIX_COMMAND "${_mode}")
        _parity_run(
            "replay ${_fixture}"
            "${_name}.raw"
            "${DSD_BIN}"
            --frontend
            none
            ${_mode_args}
            --iq-replay
            "${FIXTURE_DIR}/${_fixture}.iq.json"
            -o
            -
        )
        list(APPEND _renditions "${_name}:${_name}.raw:48000:1:${_lead_s}")
    endforeach()
endif()

execute_process(
    COMMAND "${TOOL_BIN}" established ${_renditions}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
)
if(NOT "${_rc}" STREQUAL "0")
    _parity_fail("${_out}${_err}")
endif()
message(STATUS "${_err}")
file(REMOVE_RECURSE "${WORK_DIR}")
