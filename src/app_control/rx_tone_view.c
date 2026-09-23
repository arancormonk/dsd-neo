// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <stdint.h>

/* U+2014 EM DASH, the "nothing to report" mark the other monitor rows use. */
#define RX_TONE_NO_CARRIER_TEXT "\xE2\x80\x94"

/* The configured receive policy. Tone filtering is #527; until it exists the policy is off,
   and this text comes from the configuration side, never from the received tone. */
#define RX_TONE_POLICY_OFF_TEXT "off"

/**
 * @brief Whether the analog FM monitor is running, which is when detection runs.
 *
 * Decided from the options rather than from the publication: right after a reset the
 * publication reads INACTIVE until the next block, and the row should not blink off and on
 * across every retune.
 */
static int
rx_tone_monitor_active(const dsd_opts* opts) {
    return opts->analog_only == 1 && opts->monitor_input_audio == 1;
}

static void
rx_tone_set_text(dsd_app_rx_tone* out, const char* text) {
    DSD_SNPRINTF(out->text, sizeof(out->text), "%s", text);
}

/* A locked tone of a kind this build can name; anything else still reads as detecting. */
static int
rx_tone_fill_locked(dsd_app_rx_tone* out, const dsd_analog_rx_publication* pub) {
    if (pub->tone_kind != DSD_ANALOG_TONE_KIND_CTCSS || dsd_ctcss_tone_index(pub->ctcss_tenths_hz) < 0) {
        return 0;
    }
    if (dsd_ctcss_format_label(pub->ctcss_tenths_hz, out->text, sizeof(out->text)) <= 0) {
        return 0;
    }
    out->status = DSD_APP_RX_TONE_LOCKED;
    out->kind = (uint8_t)DSD_ANALOG_TONE_KIND_CTCSS;
    out->ctcss_tenths_hz = pub->ctcss_tenths_hz;
    return 1;
}

static void
rx_tone_fill_status(dsd_app_rx_tone* out, const dsd_analog_rx_publication* pub) {
    switch (pub->tone_state) {
        case DSD_ANALOG_TONE_STATE_LOCKED:
            if (rx_tone_fill_locked(out, pub)) {
                return;
            }
            out->status = DSD_APP_RX_TONE_DETECTING;
            rx_tone_set_text(out, "detecting");
            return;
        case DSD_ANALOG_TONE_STATE_ACQUIRING:
            out->status = DSD_APP_RX_TONE_DETECTING;
            rx_tone_set_text(out, "detecting");
            return;
        case DSD_ANALOG_TONE_STATE_NONE:
            out->status = DSD_APP_RX_TONE_NONE;
            rx_tone_set_text(out, "none");
            return;
        default:
            /* INACTIVE, IDLE, or a state from a newer decoder: nothing heard to report. */
            out->status = DSD_APP_RX_TONE_NO_CARRIER;
            rx_tone_set_text(out, RX_TONE_NO_CARRIER_TEXT);
            return;
    }
}

int
dsd_app_rx_tone_view(const dsd_opts* opts, const dsd_state* state, dsd_app_rx_tone* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
        DSD_SNPRINTF(out->configured_text, sizeof(out->configured_text), "%s", RX_TONE_POLICY_OFF_TEXT);
    }
    if (!opts || !state || !out) {
        return -1;
    }
    if (!rx_tone_monitor_active(opts)) {
        out->status = DSD_APP_RX_TONE_HIDDEN;
        return 0;
    }
    const dsd_analog_rx_publication* pub = &state->analog_rx;
    out->visible = 1U;
    out->carrier_open = pub->carrier_open ? 1U : 0U;
    out->generation = pub->generation;
    rx_tone_fill_status(out, pub);
    return 1;
}
