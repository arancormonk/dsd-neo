# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one iq-determinism case (issue #572): replays an I/Q fixture once per leg
# through the replay jitter host (tests/engine/replay_jitter.c) and requires
# every leg to decode the same thing. Under --iq-replay the decoder paces the
# front end and the decode clock follows the capture, so neither the replay
# rate, nor when the decoder's reads fall, nor how many samples each returns,
# nor a stalled audio sink may change the decoder's output, its capture-time
# timestamps included.
#
# Expected -D inputs: HOST_BIN, MODE, FIXTURE, RUNS, EXPECTED, MIN_FSK,
# MIN_CQPSK; optional NOT_EXPECTED.
#
# RUNS is a ;-list of legs; a leg joins one or more parts with '+':
#   fast | realtime        --iq-replay-rate (fast when neither is given)
#   jitter:SEED:MAX_MS     --replay-jitter-seed SEED --replay-jitter-max-ms MAX_MS
#   short:SEED             --replay-short-reads SEED
#   sink:free|sink:stalled --replay-sink MODE, with -o pulse in place of -o null
#
# Each leg must, as in iq_decode_check.cmake, exit 0, show no sanitizer report,
# and match EXPECTED (and not NOT_EXPECTED). It must not print "Retune ignored
# during IQ replay": that notice fires only under -T/-Y, which the replay
# determinism guarantee excludes, so a case that prints it is misconfigured.
# Its REPLAY STREAM line must count at least MIN_FSK FSK discriminator samples
# and MIN_CQPSK CQPSK symbols (different units, kept apart).
#
# The legs' outputs are compared in memory after this normalization, applied to
# stdout and stderr separately:
#   - ANSI colour sequences are removed;
#   - these lines are dropped, and nothing else:
#       "NOTICE: Runtime: N ms"   the decode loop's real-time duration (-Z);
#       the input-level advisories ("WARNING: ... raise|lower RF gain" and
#       "... source/input volume"): their 10 s cooldown runs on real time
#       (dsd_input_level_publish()), so how often they repeat follows the
#       replay rate; the decoder may print one in the middle of its own line,
#       so the text is cut out together with its line break and the line it
#       split is joined again;
#       "REPLAY JITTER: ..."      what the host injected, which differs by design;
#       audio-sink diagnostics ("PulseAudio output stats:", "PortAudio output
#       stats:", "AAudio input|output stats:", the host's "Replay sink output
#       stats:"): the device side of a sink, which a stall changes on purpose;
#   - lines that IO or controller threads print (_io_thread_patterns below) are
#     compared as a sorted set, since where they fall among the decoder's lines
#     follows thread timing;
#   - every other line, the decoder's output with its HH:MM:SS capture-time
#     stamps, the host's REPLAY STREAM and REPLAY SINK lines and the end-of-run
#     totals, is compared verbatim and in order.
# A mismatch prints both legs' specs and the first differing line with context.
foreach(
    _var
    HOST_BIN
    MODE
    FIXTURE
    RUNS
    EXPECTED
    MIN_FSK
    MIN_CQPSK
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "iq_determinism_check: missing -D${_var}")
    endif()
endforeach()
foreach(_var MIN_FSK MIN_CQPSK)
    if(NOT "${${_var}}" MATCHES "^[0-9]+$")
        message(
            FATAL_ERROR
            "iq_determinism_check: -D${_var}=${${_var}} is not a count"
        )
    endif()
endforeach()
list(LENGTH RUNS _leg_count)
if(_leg_count LESS 2)
    message(
        FATAL_ERROR
        "iq_determinism_check: RUNS needs at least two legs to compare, got '${RUNS}'"
    )
endif()

# Regexes for the lines a non-decoder thread (the replay reader, the demod, the
# controller) prints during a passing replay, one documented pattern each. The
# list is as small as the measurement allows, and today that is empty: traced
# per thread (strace -f -e trace=write) under -fa on nxdn48_after_retune, every
# leg kind (fast, realtime, jitter, short reads, both sinks, realtime+jitter)
# writes all of its output from the decoder thread. Under replay those threads
# print only errors (which fail the leg on their exit code), the -T/-Y retune
# notice (refused above) and the DSD_NEO_DEBUG_* traces. A case whose legs do
# show such a line adds its pattern here.
set(_io_thread_patterns)

separate_arguments(_mode_args UNIX_COMMAND "${MODE}")
string(ASCII 27 _esc)

# Host arguments and audio output for one leg spec.
function(_leg_args leg out_args out_output)
    set(_args)
    set(_output null)
    set(_rate)
    string(REPLACE "+" ";" _parts "${leg}")
    foreach(_part IN LISTS _parts)
        if(_part STREQUAL "fast" OR _part STREQUAL "realtime")
            if(_rate)
                message(
                    FATAL_ERROR
                    "iq_determinism_check: leg '${leg}' names the replay rate twice"
                )
            endif()
            set(_rate "${_part}")
        elseif(_part MATCHES "^jitter:([0-9]+):([0-9]+)$")
            list(
                APPEND _args
                --replay-jitter-seed
                "${CMAKE_MATCH_1}"
                --replay-jitter-max-ms
                "${CMAKE_MATCH_2}"
            )
        elseif(_part MATCHES "^short:([0-9]+)$")
            list(APPEND _args --replay-short-reads "${CMAKE_MATCH_1}")
        elseif(_part MATCHES "^sink:(free|stalled)$")
            list(APPEND _args --replay-sink "${CMAKE_MATCH_1}")
            set(_output pulse)
        else()
            message(
                FATAL_ERROR
                "iq_determinism_check: leg '${leg}' has an unknown part '${_part}'"
            )
        endif()
    endforeach()
    if(NOT _rate)
        set(_rate fast)
    endif()
    list(APPEND _args --iq-replay-rate "${_rate}")
    set(${out_args} "${_args}" PARENT_SCOPE)
    set(${out_output} "${_output}" PARENT_SCOPE)
endfunction()

# One stream's text as a list of normalized lines: decoder lines in order
# (out_record) and IO-thread lines sorted (out_io). ';', '[' and ']' are swapped
# for placeholders so that a line stays one list element.
function(_normalize_stream text out_record out_io)
    string(REGEX REPLACE "${_esc}\\[[0-9;?]*[A-Za-z]" "" _t "${text}")
    string(
        REGEX REPLACE
            "WARNING: [^\n]*(raise|lower) (RF gain|source/input volume)[^\n]*\n?"
        ""
        _t
        "${_t}"
    )
    string(REPLACE ";" "<SC>" _t "${_t}")
    string(REPLACE "[" "<LB>" _t "${_t}")
    string(REPLACE "]" "<RB>" _t "${_t}")
    string(REPLACE "\n" ";" _lines "${_t}")
    set(_record)
    set(_io)
    foreach(_line IN LISTS _lines)
        if(
            _line MATCHES "^NOTICE: Runtime: "
            OR _line MATCHES "^REPLAY JITTER: "
            OR _line
                MATCHES
                "^(PulseAudio output|PortAudio output|AAudio input|AAudio output|Replay sink output) stats: "
        )
            continue()
        endif()
        set(_is_io 0)
        foreach(_pattern IN LISTS _io_thread_patterns)
            if(_line MATCHES "${_pattern}")
                set(_is_io 1)
                break()
            endif()
        endforeach()
        if(_is_io)
            list(APPEND _io "${_line}")
        else()
            list(APPEND _record "${_line}")
        endif()
    endforeach()
    list(SORT _io)
    set(${out_record} "${_record}" PARENT_SCOPE)
    set(${out_io} "${_io}" PARENT_SCOPE)
endfunction()

function(_print_context label lines index)
    list(LENGTH lines _count)
    math(EXPR _first "${index} - 3")
    math(EXPR _last "${index} + 3")
    if(_first LESS 0)
        set(_first 0)
    endif()
    if(_last GREATER_EQUAL _count)
        math(EXPR _last "${_count} - 1")
    endif()
    set(_text "")
    if(_last GREATER_EQUAL _first)
        foreach(_i RANGE ${_first} ${_last})
            list(GET lines ${_i} _line)
            string(REPLACE "<SC>" ";" _line "${_line}")
            string(REPLACE "<LB>" "[" _line "${_line}")
            string(REPLACE "<RB>" "]" _line "${_line}")
            math(EXPR _n "${_i} + 1")
            if(_i EQUAL index)
                string(APPEND _text "  > ${_n}: ${_line}\n")
            else()
                string(APPEND _text "    ${_n}: ${_line}\n")
            endif()
        endforeach()
    else()
        string(APPEND _text "    (no lines)\n")
    endif()
    message("${label} (${_count} lines):\n${_text}")
endfunction()

# Compares one normalized list of a leg against the first leg's.
function(
    _compare
    what
    base_leg
    base
    leg
    lines
)
    list(LENGTH base _base_count)
    list(LENGTH lines _count)
    set(_n ${_base_count})
    if(_count LESS _n)
        set(_n ${_count})
    endif()
    set(_diff -1)
    if(_n GREATER 0)
        math(EXPR _last "${_n} - 1")
        foreach(_i RANGE 0 ${_last})
            list(GET base ${_i} _a)
            list(GET lines ${_i} _b)
            if(NOT _a STREQUAL _b)
                set(_diff ${_i})
                break()
            endif()
        endforeach()
    endif()
    if(_diff LESS 0 AND NOT _base_count EQUAL _count)
        set(_diff ${_n})
    endif()
    if(_diff GREATER_EQUAL 0)
        math(EXPR _line_no "${_diff} + 1")
        message("iq_determinism_check: ${what} differ at line ${_line_no}")
        _print_context("leg ${base_leg}" "${base}" ${_diff})
        _print_context("leg ${leg}" "${lines}" ${_diff})
        message(
            FATAL_ERROR
            "iq_determinism_check: leg ${leg} decoded differently from leg ${base_leg} (${what}, line ${_line_no})"
        )
    endif()
endfunction()

set(_index 0)
foreach(_leg IN LISTS RUNS)
    math(EXPR _leg_number "${_index} + 1")
    set(_label "#${_leg_number} ${_leg}")
    _leg_args("${_leg}" _leg_extra _leg_output)
    execute_process(
        COMMAND
            "${HOST_BIN}" --frontend none ${_mode_args} ${_leg_extra}
            --iq-replay "${FIXTURE}" -o ${_leg_output}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 240
    )
    set(_all "${_out}\n${_err}")
    if(
        "${_all}"
            MATCHES
            "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
    )
        message(
            FATAL_ERROR
            "iq_determinism_check: leg ${_label}: sanitizer report detected:\n${_all}"
        )
    endif()
    if(NOT "${_rc}" STREQUAL "0")
        message(
            FATAL_ERROR
            "iq_determinism_check: leg ${_label}: the host exited with '${_rc}' (expected 0)\n${_all}"
        )
    endif()
    if("${_all}" MATCHES "Retune ignored during IQ replay")
        message(
            FATAL_ERROR
            "iq_determinism_check: leg ${_label}: a retune was refused during the replay, which only -T/-Y request; the "
            "determinism guarantee excludes them, so the case is misconfigured\n${_all}"
        )
    endif()
    if(NOT "${_all}" MATCHES "${EXPECTED}")
        message(
            FATAL_ERROR
            "iq_determinism_check: leg ${_label}: expected payload /${EXPECTED}/ not found in output\n${_all}"
        )
    endif()
    if(DEFINED NOT_EXPECTED AND NOT "${NOT_EXPECTED}" STREQUAL "")
        if("${_all}" MATCHES "${NOT_EXPECTED}")
            string(REGEX MATCH "${NOT_EXPECTED}" _hit "${_all}")
            message(
                FATAL_ERROR
                "iq_determinism_check: leg ${_label}: forbidden output /${NOT_EXPECTED}/ matched '${_hit}'\n${_all}"
            )
        endif()
    endif()

    _normalize_stream("${_out}" _out_record _out_io)
    _normalize_stream("${_err}" _err_record _err_io)
    set(_leg_${_index}_spec "${_label}")
    set(_leg_${_index}_all "${_all}")
    set(_leg_${_index}_out_record "${_out_record}")
    set(_leg_${_index}_out_io "${_out_io}")
    set(_leg_${_index}_err_record "${_err_record}")
    set(_leg_${_index}_err_io "${_err_io}")
    math(EXPR _index "${_index} + 1")
endforeach()

math(EXPR _last_leg "${_leg_count} - 1")
foreach(_i RANGE 1 ${_last_leg})
    foreach(_part out_record err_record out_io err_io)
        if(_part MATCHES "^out")
            set(_what "stdout")
        else()
            set(_what "stderr")
        endif()
        if(_part MATCHES "_io$")
            string(APPEND _what " IO-thread lines (sorted)")
        else()
            string(APPEND _what " decoder lines")
        endif()
        _compare("${_what}" "${_leg_0_spec}" "${_leg_0_${_part}}" "${_leg_${_i}_spec}" "${_leg_${_i}_${_part}}")
    endforeach()
endforeach()

foreach(_i RANGE 0 ${_last_leg})
    string(
        REGEX MATCHALL "REPLAY STREAM: fsk_samples=[0-9]+ cqpsk_symbols=[0-9]+"
        _stream_lines
        "${_leg_${_i}_all}"
    )
    list(LENGTH _stream_lines _stream_count)
    if(NOT _stream_count EQUAL 1)
        message(
            FATAL_ERROR
            "iq_determinism_check: leg ${_leg_${_i}_spec}: expected one REPLAY STREAM line, found ${_stream_count}"
        )
    endif()
    string(
        REGEX MATCH "fsk_samples=([0-9]+) cqpsk_symbols=([0-9]+)"
        _unused
        "${_stream_lines}"
    )
    set(_fsk "${CMAKE_MATCH_1}")
    set(_cqpsk "${CMAKE_MATCH_2}")
    if(_fsk LESS MIN_FSK OR _cqpsk LESS MIN_CQPSK)
        message(
            FATAL_ERROR
            "iq_determinism_check: leg ${_leg_${_i}_spec}: the decoder read ${_fsk} FSK samples and ${_cqpsk} CQPSK "
            "symbols (need at least ${MIN_FSK} and ${MIN_CQPSK})"
        )
    endif()
endforeach()

list(LENGTH _leg_0_err_record _record_lines)
list(LENGTH _leg_0_err_io _io_lines)
message(
    "iq_determinism_check: ${_leg_count} legs decoded identically (${_record_lines} decoder lines, ${_io_lines} "
    "IO-thread lines on stderr; fsk_samples=${_fsk} cqpsk_symbols=${_cqpsk})"
)
