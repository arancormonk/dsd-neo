// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/rx_tone_view.h>
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

static void
rx_tone_set_text(dsd_app_rx_tone* out, const char* text) {
    DSD_SNPRINTF(out->text, sizeof(out->text), "%s", text);
}

/* A locked CTCSS tone from the supported table, or 0. */
static int
rx_tone_fill_ctcss(dsd_app_rx_tone* out, const dsd_analog_rx_publication* pub) {
    if (dsd_ctcss_tone_index(pub->ctcss_tenths_hz) < 0
        || dsd_ctcss_format_label(pub->ctcss_tenths_hz, out->text, sizeof(out->text)) <= 0) {
        return 0;
    }
    out->ctcss_tenths_hz = pub->ctcss_tenths_hz;
    return 1;
}

/* A locked DCS code, as the detector names it: a supported code under the canonical name of
   its alias class (runtime/analog_tones.h), or 0. */
static int
rx_tone_fill_dcs(dsd_app_rx_tone* out, const dsd_analog_rx_publication* pub) {
    const int inverted = pub->dcs_inverted ? 1 : 0;
    int canon_code = -1;
    int canon_inverted = -1;
    if (dsd_dcs_canonical(pub->dcs_code, inverted, &canon_code, &canon_inverted) != 0 || canon_code != pub->dcs_code
        || canon_inverted != inverted
        || dsd_dcs_format_label(pub->dcs_code, inverted, out->text, sizeof(out->text)) <= 0) {
        return 0;
    }
    out->dcs_code = pub->dcs_code;
    out->dcs_inverted = (uint8_t)inverted;
    return 1;
}

/* A locked tone or code of a kind this build can name; anything else still reads as detecting. */
static int
rx_tone_fill_locked(dsd_app_rx_tone* out, const dsd_analog_rx_publication* pub) {
    int named = 0;
    if (pub->tone_kind == DSD_ANALOG_TONE_KIND_CTCSS) {
        named = rx_tone_fill_ctcss(out, pub);
    } else if (pub->tone_kind == DSD_ANALOG_TONE_KIND_DCS) {
        named = rx_tone_fill_dcs(out, pub);
    }
    if (!named) {
        out->text[0] = '\0';
        return 0;
    }
    out->status = DSD_APP_RX_TONE_LOCKED;
    out->kind = (uint8_t)pub->tone_kind;
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
            /* INACTIVE (nothing processed since a reset), IDLE, or a state from a newer
               decoder: nothing heard to report. */
            out->status = DSD_APP_RX_TONE_NO_CARRIER;
            rx_tone_set_text(out, RX_TONE_NO_CARRIER_TEXT);
            return;
    }
}

/* An input that stopped delivering (a stdin, UDP or TCP producer gone quiet, a live radio
   stream whose source stopped): the decoder is blocked waiting for the next sample, so its last
   word -- often a locked tone -- stays published. Past the deadline the tap set, the pause has
   outlasted the carrier hangover, and the next read will start a new reception; until then the
   row shows no carrier. */
static int
rx_tone_stale(const dsd_analog_rx_publication* pub, double now_m) {
    if (pub->stale_after_ms == 0U || !(now_m > 0.0)) {
        return 0;
    }
    return (uint64_t)(now_m * 1000.0) > pub->stale_after_ms;
}

int
dsd_app_rx_tone_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_rx_tone* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
        DSD_SNPRINTF(out->configured_text, sizeof(out->configured_text), "%s", RX_TONE_POLICY_OFF_TEXT);
    }
    if (!opts || !state || !out) {
        return -1;
    }
    /* The tap's own question (runtime/analog_tones.h), so the row is on screen exactly while
       detection listens. Not INACTIVE in the publication: right after a reset it reads that
       until the next block, and the row should not blink off and on across every retune. The
       one publication state that hides it is UNAVAILABLE, an input rate the front end cannot
       use: detection is on but hears nothing (the log says so once), and an em dash would
       claim there is no carrier. */
    const dsd_analog_rx_publication* pub = &state->analog_rx;
    if (!dsd_analog_tone_detection_active(opts) || pub->tone_state == DSD_ANALOG_TONE_STATE_UNAVAILABLE) {
        out->status = DSD_APP_RX_TONE_HIDDEN;
        return 0;
    }
    out->visible = 1U;
    out->generation = pub->generation;
    if (rx_tone_stale(pub, now_m)) {
        out->status = DSD_APP_RX_TONE_NO_CARRIER;
        rx_tone_set_text(out, RX_TONE_NO_CARRIER_TEXT);
        return 1;
    }
    out->carrier_open = pub->carrier_open ? 1U : 0U;
    rx_tone_fill_status(out, pub);
    return 1;
}
