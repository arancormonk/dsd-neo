// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Pure plan -> apply-payload mapping. No session state, no I/O, no threads. */

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>

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

int
dsd_app_rr_fill_apply_payload(const dsd_rr_import_plan* plan, const char* chan_path, const char* group_path,
                              dsd_app_rr_apply_payload* out) {
    if (!plan || !out || !plan->ok) {
        return -1;
    }
    int mode = 0;
    if (dsd_rr_protocol_decode_mode(plan->protocol, &mode) != 0) {
        return -1;
    }
    if (plan->tune_hz < 0 || plan->tune_hz > (long long)UINT32_MAX) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->decode_mode = (int32_t)mode;
    out->edacs_ea = dsd_rr_protocol_edacs_ea(plan->protocol) ? 1U : 0U;
    out->edacs_esk = plan->esk ? 1U : 0U;
    out->simulcast_qpsk = plan->simulcast ? 1U : 0U;
    /* -^ only makes sense on a trunked P25 control channel: supplying a channel
       map sets p25_has_user_lcn_list(), which would otherwise disable the
       decoder's own learned SCCB candidates (rr_generate.c, k_protocols[]). */
    out->p25_prefer_candidates = (plan->trunking && plan->protocol == DSD_RR_PROTO_P25) ? 1U : 0U;
    /* Exactly one automatic tuner owner, matching ui_handle_trunk_set/
       ui_handle_scanner_toggle in src/app_control/actions/actions_trunk.c. */
    out->trunking = plan->trunking ? 1U : 0U;
    out->scanner = (!plan->trunking && plan->scan_list) ? 1U : 0U;
    out->tune_hz = (uint32_t)plan->tune_hz;
    out->has_chan = rr_copy_path(out->chan_path, sizeof out->chan_path, chan_path);
    out->has_group = rr_copy_path(out->group_path, sizeof out->group_path, group_path);
    return 0;
}
