# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one DMR_SYNTH_* case (issue #729): generates a synthetic DMR BS WAV and
# its expectation file with dsd-neo_test_dmr_synth_gen, decodes the WAV with
# `dsd-neo -o null -fs -i <wav> --frame-log <log> -Z [keys]`, places every
# printed AMBE frame at its (slot, superframe, VC, frame) and compares it with
# the 49-bit plaintext the generator recorded.
#
# Placement: each frame follows the voice burst header the decoder printed
# ("[SLOTn] ... | VCv"). The superframe is tracked per slot (the next burst
# position whose VC number matches, counting frame-sync-miss bursts; after a
# BS loop drop-out the decoder can only re-enter at a later VC1) and is
# re-anchored to the superframe whose expected frames the burst matches best
# when a run of bursts went missing unseen.
#
# Required -D inputs: DSD_BIN, GEN_BIN, WORK_DIR, CASE, GEN_ARGS.
# Optional -D inputs:
#   DEC_ARGS          extra decoder arguments (keys), shell-quoted
#   MATCH             all (default): every decoded frame equals its plaintext;
#                     after_fault: every frame decoded after the first fault
#                     burst does; none: no plaintext check
#   MAX_MISSING       per voice slot, decoded frames >= expected - MAX_MISSING
#   MIN_PERCENT       per voice slot, decoded frames >= MIN_PERCENT% of expected
#   RECOVER           next_superframe: on the fault slot, the first frame
#                     decoded after the fault is in the next superframe and
#                     every frame from it to the end of the call is decoded;
#                     immediate: every frame after the fault burst is decoded,
#                     on every voice slot
#   MAX_CORRECTIONS   sum of the AMBE corrections (second err field, both
#                     slots) in the frame log must stay below this
#   NO_DROPOUT        1: no "CACH/EMB ERR" / "CACH/SYNC ERR" BS loop drop-out
#                     before the last decoded voice frame
#   REQUIRE_REGEX     the decoder output must match this regex
foreach(
    _var
    DSD_BIN
    GEN_BIN
    WORK_DIR
    CASE
    GEN_ARGS
)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "dmr_synth_check: missing -D${_var}")
    endif()
endforeach()
if(NOT DEFINED MATCH)
    set(MATCH all)
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(_wav "${WORK_DIR}/signal.wav")
set(_exp "${WORK_DIR}/expected.txt")
set(_flog "${WORK_DIR}/frames.log")
set(_dec_out "${WORK_DIR}/decoder.stdout")
set(_dec_err "${WORK_DIR}/decoder.stderr")

separate_arguments(_gen_args UNIX_COMMAND "${GEN_ARGS}")
set(_dec_args)
if(DEFINED DEC_ARGS)
    separate_arguments(_dec_args UNIX_COMMAND "${DEC_ARGS}")
endif()

string(TIMESTAMP _t0 "%s" UTC)
execute_process(
    COMMAND "${GEN_BIN}" -o signal.wav -e expected.txt ${_gen_args}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    TIMEOUT 60
)
if(NOT "${_rc}" STREQUAL "0")
    message(
        FATAL_ERROR
        "${CASE}: generator failed (exit ${_rc}):\n${_out}\n${_err}"
    )
endif()
string(TIMESTAMP _t1 "%s" UTC)
execute_process(
    COMMAND
        "${DSD_BIN}" --frontend none -o null -fs -i "${_wav}" --frame-log
        "${_flog}" -Z ${_dec_args}
    RESULT_VARIABLE _rc
    OUTPUT_FILE "${_dec_out}"
    ERROR_FILE "${_dec_err}"
    TIMEOUT 100
)
string(TIMESTAMP _t2 "%s" UTC)
math(EXPR _gen_s "${_t1} - ${_t0}")
math(EXPR _dec_s "${_t2} - ${_t1}")
message(STATUS "${CASE}: generator ${_gen_s} s, decoder ${_dec_s} s")

file(READ "${_dec_err}" _err_text)
file(READ "${_dec_out}" _out_text)
set(_all_text "${_out_text}\n${_err_text}")
if(
    "${_all_text}"
        MATCHES
        "AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:"
)
    message(FATAL_ERROR "${CASE}: sanitizer report in ${_dec_err}")
endif()
if(NOT "${_rc}" STREQUAL "0")
    message(
        FATAL_ERROR
        "${CASE}: decoder exited with '${_rc}' (output in ${WORK_DIR})"
    )
endif()

# Decoder text -> CMake list: brackets and semicolons would change how CMake
# splits a list, and ANSI colour sequences carry unbalanced brackets.
function(_dmr_synth_to_lines text out_var)
    string(ASCII 27 _esc)
    string(REPLACE "${_esc}" "" text "${text}")
    string(REPLACE "\\" "/" text "${text}")
    string(REPLACE ";" "," text "${text}")
    string(REPLACE "[" "<" text "${text}")
    string(REPLACE "]" ">" text "${text}")
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE "\n" ";" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------- expectation
file(STRINGS "${_exp}" _exp_lines)
set(_nsf 0)
set(_voice_slots)
set(_fault_slot "")
# A superframe the generator destroyed entirely cannot be decoded: placement
# starts after the lost ones.
set(_last_pos_1 -1)
set(_last_pos_2 -1)
foreach(_line IN LISTS _exp_lines)
    if(_line MATCHES "^# superframes ([0-9]+)$")
        set(_nsf ${CMAKE_MATCH_1})
    elseif(_line MATCHES "^# lost ([12]) ([0-9]+)$")
        math(EXPR _lost_end "(${CMAKE_MATCH_2} + 1) * 6 - 1")
        if(_lost_end GREATER _last_pos_${CMAKE_MATCH_1})
            set(_last_pos_${CMAKE_MATCH_1} ${_lost_end})
        endif()
    elseif(_line MATCHES "^# voice_slot ([12])$")
        list(APPEND _voice_slots ${CMAKE_MATCH_1})
    elseif(_line MATCHES "^# fault ([12]) ([0-9]+) ([1-6]) ([a-z]+)$")
        if("${_fault_slot}" STREQUAL "")
            set(_fault_slot ${CMAKE_MATCH_1})
            set(_fault_sf ${CMAKE_MATCH_2})
            set(_fault_vc ${CMAKE_MATCH_3})
            set(_fault_mode ${CMAKE_MATCH_4})
        endif()
    elseif(_line MATCHES "^([12]) ([0-9]+) ([1-6]) ([0-2]) ([0-9A-F]+)$")
        set(_E_${CMAKE_MATCH_1}_${CMAKE_MATCH_2}_${CMAKE_MATCH_3}_${CMAKE_MATCH_4}
            ${CMAKE_MATCH_5}
        )
    endif()
endforeach()
if(_nsf EQUAL 0 OR NOT _voice_slots)
    message(FATAL_ERROR "${CASE}: malformed expectation file ${_exp}")
endif()
math(EXPR _nexp_per_slot "${_nsf} * 18")
if(MATCH STREQUAL "after_fault" OR DEFINED RECOVER)
    if("${_fault_slot}" STREQUAL "")
        message(FATAL_ERROR "${CASE}: MATCH/RECOVER need a generator fault")
    endif()
    math(EXPR _fault_burst "${_fault_sf} * 6 + ${_fault_vc} - 1")
endif()

# ---------------------------------------------------------------- placement
set(_skipped_1 0)
set(_skipped_2 0)
set(_dropped_1 0)
set(_dropped_2 0)
set(_have_1 0) # a burst of the slot has been placed
set(_have_2 0)
set(_cur_slot "")
set(_cur_vc "")
set(_pending)
set(_decoded) # entries "slot|sf|vc|frame|hex|line"

macro(_dmr_synth_flush)
    list(LENGTH _pending _np)
    if(NOT "${_cur_slot}" STREQUAL "" AND _np GREATER 0)
        set(_s ${_cur_slot})
        math(EXPR _min_pos "${_last_pos_${_s}} + 1 + ${_skipped_${_s}}")
        if(_dropped_${_s} AND _have_${_s})
            # the loop ended after the last decoded burst; re-entry is at a later VC1
            math(EXPR _vc1 "((${_last_pos_${_s}} + 1) / 6 + 1) * 6")
            if(_vc1 GREATER _min_pos)
                set(_min_pos ${_vc1})
            endif()
        endif()
        math(
            EXPR
            _pos
            "${_min_pos} + ((((${_cur_vc} - 1 - ${_min_pos}) % 6) + 6) % 6)"
        )
        math(EXPR _est "${_pos} / 6")
        # re-anchor on content when a run of bursts went missing unseen
        set(_best ${_est})
        set(_bestm -1)
        math(EXPR _cand "${_est} - 1")
        if(_cand LESS 0)
            set(_cand 0)
        endif()
        math(EXPR _cand_end "${_est} + 5")
        while(_cand LESS_EQUAL _cand_end)
            set(_m 0)
            set(_i 0)
            foreach(_hx IN LISTS _pending)
                if(
                    DEFINED _E_${_s}_${_cand}_${_cur_vc}_${_i}
                    AND "${_E_${_s}_${_cand}_${_cur_vc}_${_i}}" STREQUAL "${_hx}"
                )
                    math(EXPR _m "${_m} + 1")
                endif()
                math(EXPR _i "${_i} + 1")
            endforeach()
            if(_m GREATER _bestm)
                set(_best ${_cand})
                set(_bestm ${_m})
            endif()
            math(EXPR _cand "${_cand} + 1")
        endwhile()
        if(_bestm GREATER_EQUAL 2 AND NOT _best EQUAL _est)
            set(_est ${_best})
        endif()
        math(EXPR _last_pos_${_s} "${_est} * 6 + ${_cur_vc} - 1")
        set(_skipped_${_s} 0)
        set(_dropped_${_s} 0)
        set(_have_${_s} 1)
        set(_i 0)
        foreach(_hx IN LISTS _pending)
            list(GET _pending_lines ${_i} _pl)
            list(APPEND _decoded "${_s}|${_est}|${_cur_vc}|${_i}|${_hx}|${_pl}")
            math(EXPR _i "${_i} + 1")
        endforeach()
    endif()
    set(_pending)
    set(_pending_lines)
    set(_cur_slot "")
endmacro()

_dmr_synth_to_lines("${_err_text}" _lines)
set(_ln 0)
set(_last_ambe_ln 0)
set(_dropout_lns)
set(_pending_lines)
foreach(_line IN LISTS _lines)
    math(EXPR _ln "${_ln} + 1")
    if(_line MATCHES "<SLOT([12])>.*\\| VC([1-6])")
        _dmr_synth_flush()
        set(_cur_slot ${CMAKE_MATCH_1})
        set(_cur_vc ${CMAKE_MATCH_2})
    elseif(_line MATCHES "^ *AMBE ([0-9A-F]+) err")
        set(_last_ambe_ln ${_ln})
        if(NOT "${_cur_slot}" STREQUAL "")
            list(APPEND _pending ${CMAKE_MATCH_1})
            list(APPEND _pending_lines ${_ln})
            list(LENGTH _pending _np)
            if(_np EQUAL 3)
                _dmr_synth_flush()
            endif()
        endif()
    elseif(_line MATCHES "Frame Sync Err")
        _dmr_synth_flush()
        if(_line MATCHES "<SLOT1>")
            math(EXPR _skipped_1 "${_skipped_1} + 1")
        else()
            math(EXPR _skipped_2 "${_skipped_2} + 1")
        endif()
    elseif(_line MATCHES "CACH/EMB ERR|CACH/SYNC ERR")
        _dmr_synth_flush()
        set(_dropped_1 1)
        set(_dropped_2 1)
        list(APPEND _dropout_lns ${_ln})
    elseif(_line MATCHES "Sync:")
        _dmr_synth_flush()
    endif()
endforeach()
_dmr_synth_flush()

# ---------------------------------------------------------------- evaluation
set(_failures)
set(_summary)
set(_nbad 0)
foreach(_entry IN LISTS _decoded)
    string(REPLACE "|" ";" _f "${_entry}")
    list(GET _f 0 _s)
    list(GET _f 1 _sf)
    list(GET _f 2 _vc)
    list(GET _f 3 _fr)
    list(GET _f 4 _hx)
    list(GET _f 5 _l)
    set(_key ${_s}_${_sf}_${_vc}_${_fr})
    set(_D_${_key} 1)
    set(_exp_hx "<none: outside the call>")
    if(DEFINED _E_${_key})
        set(_exp_hx "${_E_${_key}}")
    endif()
    set(_check 0)
    if(MATCH STREQUAL "all")
        set(_check 1)
    elseif(MATCH STREQUAL "after_fault")
        math(EXPR _burst "${_sf} * 6 + ${_vc} - 1")
        if(_s EQUAL _fault_slot AND _burst GREATER _fault_burst)
            set(_check 1)
        elseif(NOT _s EQUAL _fault_slot AND _burst GREATER_EQUAL _fault_burst)
            set(_check 1)
        endif()
    endif()
    if(_check AND NOT "${_exp_hx}" STREQUAL "${_hx}")
        math(EXPR _nbad "${_nbad} + 1")
        if(_nbad LESS_EQUAL 24)
            list(
                APPEND _failures
                "${CASE}: wrong plaintext: slot ${_s} superframe ${_sf} vc ${_vc} frame ${_fr}: expected ${_exp_hx}, got ${_hx} (stderr line ${_l})"
            )
        endif()
    endif()
endforeach()
if(_nbad GREATER 24)
    math(EXPR _more "${_nbad} - 24")
    list(APPEND _failures "${CASE}: ... and ${_more} more wrong frames")
endif()

foreach(_s IN LISTS _voice_slots)
    set(_ndec 0)
    set(_nok 0)
    set(_sf 0)
    set(_rows)
    while(_sf LESS _nsf)
        set(_n 0)
        set(_k 0)
        foreach(_vc RANGE 1 6)
            foreach(_fr RANGE 0 2)
                set(_key ${_s}_${_sf}_${_vc}_${_fr})
                if(DEFINED _D_${_key})
                    math(EXPR _n "${_n} + 1")
                endif()
            endforeach()
        endforeach()
        if(NOT _n EQUAL 18)
            list(APPEND _rows "sf${_sf}:${_n}/18")
        endif()
        math(EXPR _ndec "${_ndec} + ${_n}")
        math(EXPR _sf "${_sf} + 1")
    endwhile()
    string(REPLACE ";" " " _rows "${_rows}")
    list(
        APPEND _summary
        "${CASE}: slot ${_s}: expected ${_nexp_per_slot} frames, decoded ${_ndec}; superframes not fully decoded: ${_rows}"
    )
    if(DEFINED MAX_MISSING)
        math(EXPR _need "${_nexp_per_slot} - ${MAX_MISSING}")
        if(_ndec LESS _need)
            list(
                APPEND _failures
                "${CASE}: slot ${_s}: decoded ${_ndec} of ${_nexp_per_slot} frames, need at least ${_need}"
            )
        endif()
    endif()
    if(DEFINED MIN_PERCENT)
        math(EXPR _need "(${_nexp_per_slot} * ${MIN_PERCENT} + 99) / 100")
        if(_ndec LESS _need)
            list(
                APPEND _failures
                "${CASE}: slot ${_s}: decoded ${_ndec} of ${_nexp_per_slot} frames, need at least ${_need} (${MIN_PERCENT}%)"
            )
        endif()
    endif()
endforeach()

# Frames from @first_pos (slot frame index) to the end of the call that were not decoded.
function(_dmr_synth_missing slot first_pos out_var)
    set(_missing)
    math(EXPR _end "${_nsf} * 18")
    set(_p ${first_pos})
    while(_p LESS _end)
        math(EXPR _sf "${_p} / 18")
        math(EXPR _vc "(${_p} % 18) / 3 + 1")
        math(EXPR _fr "${_p} % 3")
        if(NOT DEFINED _D_${slot}_${_sf}_${_vc}_${_fr})
            list(APPEND _missing "sf${_sf}/vc${_vc}/f${_fr}")
        endif()
        math(EXPR _p "${_p} + 1")
    endwhile()
    set(${out_var} "${_missing}" PARENT_SCOPE)
endfunction()

if(DEFINED RECOVER)
    if(RECOVER STREQUAL "next_superframe")
        math(EXPR _next_sf "${_fault_sf} + 1")
        math(EXPR _p "${_next_sf} * 18")
        math(EXPR _p_end "${_p} + 18")
        set(_first "")
        while(_p LESS _p_end AND "${_first}" STREQUAL "")
            math(EXPR _vc "(${_p} % 18) / 3 + 1")
            math(EXPR _fr "${_p} % 3")
            if(DEFINED _D_${_fault_slot}_${_next_sf}_${_vc}_${_fr})
                set(_first ${_p})
            endif()
            math(EXPR _p "${_p} + 1")
        endwhile()
        if("${_first}" STREQUAL "")
            list(
                APPEND _failures
                "${CASE}: slot ${_fault_slot}: no frame of superframe ${_next_sf} (the one after the fault) was decoded"
            )
        else()
            _dmr_synth_missing(${_fault_slot} ${_first} _miss)
            list(LENGTH _miss _nmiss)
            if(_nmiss GREATER 0)
                list(SUBLIST _miss 0 12 _shown)
                string(REPLACE ";" " " _shown "${_shown}")
                list(
                    APPEND _failures
                    "${CASE}: slot ${_fault_slot}: ${_nmiss} frames missing after recovery in superframe ${_next_sf}: ${_shown}"
                )
            endif()
        endif()
    elseif(RECOVER STREQUAL "immediate")
        foreach(_s IN LISTS _voice_slots)
            if(_s EQUAL _fault_slot)
                math(EXPR _first "(${_fault_burst} + 1) * 3")
            else()
                math(EXPR _first "${_fault_burst} * 3")
            endif()
            _dmr_synth_missing(${_s} ${_first} _miss)
            list(LENGTH _miss _nmiss)
            if(_nmiss GREATER 0)
                list(SUBLIST _miss 0 12 _shown)
                string(REPLACE ";" " " _shown "${_shown}")
                list(
                    APPEND _failures
                    "${CASE}: slot ${_s}: ${_nmiss} frames missing (muted or lost) after the ${_fault_mode} fault at slot ${_fault_slot} superframe ${_fault_sf} vc ${_fault_vc}: ${_shown}"
                )
            endif()
        endforeach()
    else()
        message(FATAL_ERROR "${CASE}: unknown RECOVER '${RECOVER}'")
    endif()
endif()

if(DEFINED MAX_CORRECTIONS)
    set(_corr 0)
    if(EXISTS "${_flog}")
        file(READ "${_flog}" _flog_text)
        _dmr_synth_to_lines("${_flog_text}" _flog_lines)
        foreach(_line IN LISTS _flog_lines)
            if(
                _line
                    MATCHES
                    "FRAME AMBE slot=[12] data=[0-9A-F]+ err=<([0-9A-F]+)> <([0-9A-F]+)>"
            )
                math(EXPR _corr "${_corr} + 0x${CMAKE_MATCH_2}")
            endif()
        endforeach()
    endif()
    list(APPEND _summary "${CASE}: AMBE corrections (both slots): ${_corr}")
    if(_corr GREATER_EQUAL MAX_CORRECTIONS)
        list(
            APPEND _failures
            "${CASE}: ${_corr} AMBE corrections, bound ${MAX_CORRECTIONS}"
        )
    endif()
endif()

if(NO_DROPOUT)
    set(_early)
    foreach(_d IN LISTS _dropout_lns)
        if(_d LESS _last_ambe_ln)
            list(APPEND _early ${_d})
        endif()
    endforeach()
    list(LENGTH _early _nearly)
    if(_nearly GREATER 0)
        string(REPLACE ";" " " _early "${_early}")
        list(
            APPEND _failures
            "${CASE}: ${_nearly} BS loop drop-outs (CACH/EMB or CACH/SYNC ERR) before the end of the call, at stderr lines ${_early}"
        )
    endif()
endif()

if(DEFINED REQUIRE_REGEX AND NOT "${_err_text}" MATCHES "${REQUIRE_REGEX}")
    list(
        APPEND _failures
        "${CASE}: decoder output does not match '${REQUIRE_REGEX}'"
    )
endif()

foreach(_l IN LISTS _summary)
    message(STATUS "${_l}")
endforeach()
if(_failures)
    string(REPLACE ";" "\n" _msg "${_failures}")
    message(FATAL_ERROR "${_msg}\n(decoder output in ${WORK_DIR})")
endif()
message(STATUS "${CASE}: passed")
