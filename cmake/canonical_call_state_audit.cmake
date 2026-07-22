# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

get_filename_component(_DSD_NEO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

file(
    GLOB_RECURSE _DSD_NEO_PRODUCTION_FILES
    LIST_DIRECTORIES FALSE
    "${_DSD_NEO_ROOT}/include/*.h"
    "${_DSD_NEO_ROOT}/include/*.hpp"
    "${_DSD_NEO_ROOT}/src/*.c"
    "${_DSD_NEO_ROOT}/src/*.cc"
    "${_DSD_NEO_ROOT}/src/*.cpp"
    "${_DSD_NEO_ROOT}/src/*.h"
    "${_DSD_NEO_ROOT}/src/*.hpp"
    "${_DSD_NEO_ROOT}/apps/*.c"
    "${_DSD_NEO_ROOT}/apps/*.cc"
    "${_DSD_NEO_ROOT}/apps/*.cpp"
    "${_DSD_NEO_ROOT}/apps/*.h"
    "${_DSD_NEO_ROOT}/apps/*.hpp"
)

# These names were current-call identity mirrors in dsd_state. Keeping the
# names out of production code makes accidental resurrection visible in CI.
set(_DSD_NEO_DELETED_CALL_FIELDS
    CalledID
    CalledIDOk
    CallingID
    CallingIDOk
    active_channel
    call_string
    dmr_end_alert
    dpmr_caller_id
    dpmr_target_id
    dstar_dst
    dstar_rpt1
    dstar_rpt2
    dstar_src
    edacs_vc_call_type
    edacs_vc_lcn
    generic_talker_alias_src
    last_active_time
    lastsrc
    lastsrcR
    lasttg
    lasttgR
    m17_dst
    m17_dst_csd
    m17_dst_str
    m17_src
    m17_src_csd
    m17_src_str
    nxdn_call_type
    nxdn_last_rid
    nxdn_last_tg
    p25_call_emergency
    p25_call_is_packet
    p25_call_priority
    p25_policy_tg
    p25_service_options_valid
    ysf_dnl
    ysf_src
    ysf_tgt
    ysf_upl
)

list(JOIN _DSD_NEO_DELETED_CALL_FIELDS "|" _DSD_NEO_DELETED_CALL_FIELDS_ALT)
set(_DSD_NEO_DELETED_CALL_FIELDS_RE
    "(^|[^A-Za-z0-9_])(${_DSD_NEO_DELETED_CALL_FIELDS_ALT})([^A-Za-z0-9_]|$)"
)
set(_DSD_NEO_LEGACY_SLOT_GI_RE "(\\.|->)gi[ \t]*\\[")

set(_DSD_NEO_AUDIT_FAILURES "")
foreach(_DSD_NEO_FILE IN LISTS _DSD_NEO_PRODUCTION_FILES)
    file(RELATIVE_PATH _DSD_NEO_REL "${_DSD_NEO_ROOT}" "${_DSD_NEO_FILE}")
    if(_DSD_NEO_REL MATCHES "^src/third_party/")
        continue()
    endif()

    file(
        STRINGS "${_DSD_NEO_FILE}"
        _DSD_NEO_FIELD_MATCHES
        REGEX "${_DSD_NEO_DELETED_CALL_FIELDS_RE}"
    )
    file(
        STRINGS "${_DSD_NEO_FILE}"
        _DSD_NEO_GI_MATCHES
        REGEX "${_DSD_NEO_LEGACY_SLOT_GI_RE}"
    )
    foreach(_DSD_NEO_MATCH IN LISTS _DSD_NEO_FIELD_MATCHES _DSD_NEO_GI_MATCHES)
        string(STRIP "${_DSD_NEO_MATCH}" _DSD_NEO_MATCH)
        string(
            APPEND _DSD_NEO_AUDIT_FAILURES
            "\n  ${_DSD_NEO_REL}: ${_DSD_NEO_MATCH}"
        )
    endforeach()
endforeach()

if(_DSD_NEO_AUDIT_FAILURES)
    message(
        FATAL_ERROR
        "Canonical call-state audit found deleted attribution fields in production code:${_DSD_NEO_AUDIT_FAILURES}"
    )
endif()

message(STATUS "Canonical call-state audit passed")
