// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Pure plan -> apply-payload mapping, and the frontends' pre-check of an import over the published snapshots. No live
   session state, no I/O, no threads of its own. */

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "next_start_check.h"
#include "rr_apply_verdict.h"

/**
 * @brief Copy a path into the payload and report whether it is meaningful.
 *
 * @return 1 when @p path is non-NULL and non-empty, else 0 (dst is left empty).
 */
static uint8_t
rr_copy_path(char* dst, size_t dst_sz, const char* path) {
    dst[0] = '\0';
    if (!path || path[0] == '\0') {
        return 0U;
    }
    DSD_SNPRINTF(dst, dst_sz, "%s", path);
    return 1U;
}

/**
 * @brief Whether the plan's simulcast answer is one this protocol acts on.
 *
 * NOT a bare copy of plan->simulcast: dsd_rr_site_is_simulcast() keys off the
 * site description and modulation and fires for ANY protocol, which is why
 * dsd_rr_decode_flag() only lets the answer select an alternate for the family
 * it belongs to (rr_generate.c, RR_ALT_ON_SIMULCAST). Forcing QPSK on a DMR,
 * NXDN or EDACS site merely described as "Simulcast" would publish a QPSK symbol
 * profile for a C4FM protocol and decode nothing - and the wrong answer would
 * then be stored in the .rr recipe and re-applied forever. Asking the table the
 * same question the flag did covers a protocol that gains an alternate later
 * without a second list to keep in step.
 *
 * Split out of dsd_app_rr_fill_apply_payload() to keep it under the CCN ceiling
 * tools/lizard.sh enforces.
 */
static uint8_t
rr_simulcast_qpsk(const dsd_rr_import_plan* plan) {
    if (!plan->simulcast) {
        return 0U;
    }
    const char* with_simulcast = dsd_rr_decode_flag(plan->protocol, 1, plan->esk, plan->scan_list);
    const char* without_simulcast = dsd_rr_decode_flag(plan->protocol, 0, plan->esk, plan->scan_list);
    return (with_simulcast != NULL && without_simulcast != NULL && strcmp(with_simulcast, without_simulcast) != 0) ? 1U
                                                                                                                   : 0U;
}

int
dsd_app_rr_fill_apply_mode(const dsd_rr_import_plan* plan, dsd_app_rr_apply_payload* out) {
    if (!plan || !out) {
        return -1;
    }
    int mode = 0;
    if (dsd_rr_protocol_decode_mode(plan->protocol, &mode) != 0) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->decode_mode = (int32_t)mode;
    /* Exactly one automatic tuner owner, matching ui_handle_trunk_set/
       ui_handle_scanner_toggle in src/app_control/actions/actions_trunk.c. */
    out->trunking = plan->trunking ? 1U : 0U;
    out->scanner = (!plan->trunking && plan->scan_list) ? 1U : 0U;
    return 0;
}

int
dsd_app_rr_fill_apply_payload(const dsd_rr_import_plan* plan, const char* chan_path, const char* group_path,
                              dsd_app_rr_apply_payload* out) {
    if (!plan || !out || !plan->ok) {
        return -1;
    }
    if (plan->tune_hz < 0 || plan->tune_hz > (long long)UINT32_MAX) {
        return -1;
    }
    if (dsd_app_rr_fill_apply_mode(plan, out) != 0) {
        return -1;
    }
    out->edacs_ea = dsd_rr_protocol_edacs_ea(plan->protocol) ? 1U : 0U;
    out->edacs_esk = plan->esk ? 1U : 0U;
    out->simulcast_qpsk = rr_simulcast_qpsk(plan);
    /* -^ only makes sense on a trunked P25 control channel: supplying a channel
       map sets p25_has_user_lcn_list(), which would otherwise disable the
       decoder's own learned SCCB candidates (rr_generate.c, k_protocols[]). */
    out->p25_prefer_candidates = (plan->trunking && plan->protocol == DSD_RR_PROTO_P25) ? 1U : 0U;
    out->tune_hz = (uint32_t)plan->tune_hz;
    out->has_chan = rr_copy_path(out->chan_path, sizeof out->chan_path, chan_path);
    out->has_group = rr_copy_path(out->group_path, sizeof out->group_path, group_path);
    return 0;
}

int
dsd_app_rr_apply_block_reason(const dsd_app_rr_apply_payload* p, int has_group, int has_chan, char* why,
                              size_t why_size) {
    if (why && why_size > 0U) {
        why[0] = '\0';
    }
    const dsd_opts* osnap = p ? dsd_app_get_latest_opts_snapshot() : NULL;
    if (osnap == NULL) {
        return 0; /* nothing published yet: no evidence against the import */
    }
    /* No measured rate here: the frontend metric lags the committed rate, and the stream belongs to the decoder
       thread. Every live radio's rate is its committed DSP bandwidth (an Airspy or SoapySDR device's capture rate is
       stepped to it, issue #806). */
    dsd_app_rr_receiver_facts facts;
    rr_apply_receiver_facts_fill(osnap, p, has_group, has_chan, &facts);
    if (dsd_app_rr_receiver_verdict(&facts, why, why_size) != 0) {
        return 1;
    }
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    if (snap != NULL && rr_apply_next_start_refusal(osnap, snap, &facts, p->chan_path, why, why_size) != 0) {
        /* The rule without its advice, as the decoder's refusal names it. */
        if (why && why_size > 0U) {
            why[strcspn(why, ";")] = '\0';
        }
        return 1;
    }
    return 0;
}

int
dsd_app_rr_files_block_reason(const char* chan_path, char* why, size_t why_size) {
    if (why && why_size > 0U) {
        why[0] = '\0';
    }
    const dsd_opts* osnap = dsd_app_get_latest_opts_snapshot();
    if (osnap == NULL) {
        return 0;
    }
    /* The verdict with no decode mode asks only what refuses any import: a running trunk scan. */
    dsd_app_rr_apply_payload keep_mode;
    DSD_MEMSET(&keep_mode, 0, sizeof keep_mode);
    keep_mode.decode_mode = (int32_t)DSDCFG_MODE_UNSET;
    dsd_app_rr_receiver_facts facts;
    rr_apply_receiver_facts_fill(osnap, &keep_mode, 0, 0, &facts);
    if (dsd_app_rr_receiver_verdict(&facts, why, why_size) != 0) {
        return 1;
    }
    const dsd_state* snap = dsd_app_get_latest_snapshot();
    if (chan_path == NULL || chan_path[0] == '\0' || snap == NULL) {
        return 0;
    }
    /* Loading a channel map on its own, as svc_import_channel_map() asks the next-start rule: only the map changes. */
    if (app_check_next_start(osnap, snap, app_next_start_load_channel_map, chan_path, why, why_size) != 0) {
        if (why && why_size > 0U) {
            why[strcspn(why, ";")] = '\0';
        }
        return 1;
    }
    return 0;
}
