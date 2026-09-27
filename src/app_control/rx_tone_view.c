// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stddef.h>
#include <stdint.h>

/* U+2014 EM DASH, the "nothing to report" mark the other monitor rows use. */
#define RX_TONE_NO_CARRIER_TEXT "\xE2\x80\x94"

/* The receive policy (issue #527), from the configuration side (dsd_opts and the scan row's options), never from the
   received tone. */
#define RX_TONE_POLICY_OFF_TEXT "off"

/* Room for the list in force: alone, or beside a row's own the configured default it shadows, both fitting the view's
   text with the mode names and " (row; default " around them. The default gets the smaller share, since it is the one
   not in force. */
enum {
    RX_TONE_POLICY_LIST_SIZE = 80,
    RX_TONE_POLICY_ROW_LIST_SIZE = 56,
    RX_TONE_POLICY_DEFAULT_LIST_SIZE = 32,
};

_Static_assert((int)DSD_APP_RX_TONE_POLICY_TEXT_SIZE >= (int)(sizeof("block ") + RX_TONE_POLICY_LIST_SIZE),
               "the policy in force fits the view's text");
_Static_assert((int)DSD_APP_RX_TONE_POLICY_TEXT_SIZE
                   >= (int)(sizeof("block ") + RX_TONE_POLICY_ROW_LIST_SIZE + sizeof(" (row; default block )")
                            + RX_TONE_POLICY_DEFAULT_LIST_SIZE),
               "a row's policy and the default it shadows fit the view's text");

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

/* The label names both spellings of the code's signal. */
_Static_assert((int)DSD_APP_RX_TONE_TEXT_SIZE >= (int)DSD_DCS_LABEL_SIZE, "a DCS label fits the view's text");

/* A locked DCS code, as the detector names it: a supported code under the canonical name of
   its alias class (runtime/analog_tones.h), shown with the other standard spelling of the same
   signal, or 0. */
static int
rx_tone_fill_dcs(dsd_app_rx_tone* out, const dsd_analog_rx_publication* pub) {
    const int inverted = pub->dcs_inverted ? 1 : 0;
    int canon_code = -1;
    int canon_inverted = -1;
    int alias_code = -1;
    int alias_inverted = -1;
    if (dsd_dcs_canonical(pub->dcs_code, inverted, &canon_code, &canon_inverted) != 0 || canon_code != pub->dcs_code
        || canon_inverted != inverted || dsd_dcs_alias(pub->dcs_code, inverted, &alias_code, &alias_inverted) != 0
        || dsd_dcs_format_label(pub->dcs_code, inverted, out->text, sizeof(out->text)) <= 0) {
        return 0;
    }
    out->dcs_code = pub->dcs_code;
    out->dcs_inverted = (uint8_t)inverted;
    out->dcs_alias_code = alias_code;
    out->dcs_alias_inverted = alias_inverted ? 1U : 0U;
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

/* One policy as the row names it: "off", or its mode and display list ("allow 100.0 Hz/D023N"), the list held to
   @p list_size bytes ("…+N" for what does not fit). A list policy without a list runs as off, so it reads as off.
   Returns 1 for a list policy, 0 for off. */
static int
rx_tone_format_policy(int mode, const dsd_tone_set* set, char* out, size_t out_size, size_t list_size) {
    const char* name = dsd_tone_filter_mode_name(mode);
    char list[RX_TONE_POLICY_LIST_SIZE];
    if (list_size > sizeof(list)) {
        list_size = sizeof(list);
    }
    if (!name || mode == DSD_TONE_FILTER_OFF || dsd_tone_set_format_display(set, list, list_size) <= 0) {
        DSD_SNPRINTF(out, out_size, "%s", RX_TONE_POLICY_OFF_TEXT);
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "%s %s", name, list);
    return 1;
}

/* The scan row's own policy on air, or NULL when none is installed. */
static const dsd_scan_option_values*
rx_tone_row_policy(const dsd_state* state) {
    const dsd_scan_option_values* row_options = dsd_scan_mode_row_options(state);
    return (row_options && (row_options->present & DSD_SCAN_OPT_TONE) != 0U) ? row_options : NULL;
}

/* The tone policy in force (issue #527): dsd_opts holds a scan row's own while the row is on air. As every row override
   reads (squelch, channel width), the text then names the configured default it shadows: "block D023I (row; default
   allow 100.0 Hz)". */
static void
rx_tone_fill_policy_text(dsd_app_rx_tone* out, const dsd_opts* opts, const dsd_state* state) {
    const int row = rx_tone_row_policy(state) != NULL;
    char effective[DSD_APP_RX_TONE_POLICY_TEXT_SIZE];
    const int in_force =
        rx_tone_format_policy(opts->analog_tone_filter, &opts->analog_tone_set, effective, sizeof(effective),
                              row ? RX_TONE_POLICY_ROW_LIST_SIZE : RX_TONE_POLICY_LIST_SIZE);
    out->policy_row = row ? 1U : 0U;
    out->policy_visible = (in_force || row) ? 1U : 0U;
    if (!row) {
        DSD_SNPRINTF(out->configured_text, sizeof(out->configured_text), "%s", effective);
        return;
    }
    int default_mode = DSD_TONE_FILTER_OFF;
    dsd_tone_set default_set;
    DSD_MEMSET(&default_set, 0, sizeof(default_set));
    dsd_scan_mode_configured_tone_policy(opts, state, &default_mode, &default_set);
    char configured[DSD_APP_RX_TONE_POLICY_TEXT_SIZE];
    (void)rx_tone_format_policy(default_mode, &default_set, configured, sizeof(configured),
                                RX_TONE_POLICY_DEFAULT_LIST_SIZE);
    DSD_SNPRINTF(out->configured_text, sizeof(out->configured_text), "%s (row; default %s)", effective, configured);
}

/* What the policy does with the carrier on air: nothing to say without a carrier (or past a stale input's deadline). */
static void
rx_tone_fill_gate(dsd_app_rx_tone* out, const dsd_opts* opts, const dsd_state* state, int stale) {
    const dsd_analog_rx_publication* pub = &state->analog_rx;
    const int gate = dsd_analog_tone_gate_in_force(opts, state);
    if (!out->policy_visible || stale || !pub->carrier_open || gate == DSD_ANALOG_TONE_GATE_OFF) {
        return;
    }
    const char* text = "muted: checking tone";
    if (gate == DSD_ANALOG_TONE_GATE_ALLOWED) {
        text = "passing";
    } else if (gate == DSD_ANALOG_TONE_GATE_REJECTED) {
        text = pub->gate_no_tone ? "muted: no tone" : "muted: not allowed";
    }
    out->gate = (uint8_t)gate;
    out->gate_no_tone = (gate != DSD_ANALOG_TONE_GATE_PENDING && pub->gate_no_tone) ? 1U : 0U;
    DSD_SNPRINTF(out->gate_text, sizeof(out->gate_text), "%s", text);
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
    rx_tone_fill_policy_text(out, opts, state);
    /* Where detection runs a policy acts on what the monitor plays, so its editor belongs on screen, no policy set
       included; elsewhere nothing is shown, whatever is configured. */
    out->policy_editable = dsd_analog_tone_detection_active(opts) ? 1U : 0U;
    if (!out->policy_editable) {
        out->policy_visible = 0U;
    }
    rx_tone_fill_gate(out, opts, state, rx_tone_stale(&state->analog_rx, now_m));
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

/* The live editor (issue #527): what it opens on, and what its edit did. */

_Static_assert(DSD_APP_TONE_FILTER_LIST_SIZE == DSD_TONE_LIST_TEXT_MAX + 1,
               "the editor's list holds exactly the longest list the parser reads");

/* Room for the lists in an edit's notice when a row shadows it: the configured policy the edit made, then the row's own
   that stays in force, the configured one getting the larger share since it is the one edited. */
enum {
    RX_TONE_NOTICE_LIST_SIZE = 80,
    RX_TONE_NOTICE_DEFAULT_LIST_SIZE = 32,
    RX_TONE_NOTICE_ROW_LIST_SIZE = 24,
};

_Static_assert(DSD_APP_TONE_FILTER_NOTICE_SIZE
                   >= (int)(sizeof("Applied: Tone filter -> block ") + RX_TONE_NOTICE_LIST_SIZE),
               "an edit's notice fits");
_Static_assert(DSD_APP_TONE_FILTER_NOTICE_SIZE
                   >= (int)(sizeof("Default tone filter -> block ") + RX_TONE_NOTICE_DEFAULT_LIST_SIZE
                            + sizeof("; this channel overrides it (block )") + RX_TONE_NOTICE_ROW_LIST_SIZE),
               "a shadowed edit's notice fits");

int
dsd_app_tone_filter_setting_get(const dsd_opts* opts, const dsd_state* state, dsd_app_tone_filter_setting* out) {
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
        out->mode = DSD_TONE_FILTER_OFF;
    }
    if (!opts || !out) {
        return -1;
    }
    dsd_tone_set set;
    DSD_MEMSET(&set, 0, sizeof(set));
    dsd_scan_mode_configured_tone_policy(opts, state, &out->mode, &set);
    if (dsd_tone_set_format(&set, out->list, sizeof(out->list)) < 0) {
        out->list[0] = '\0';
    }
    out->row_override = rx_tone_row_policy(state) ? 1U : 0U;
    return 0;
}

int
dsd_app_tone_filter_edit_notice(const dsd_opts* opts, const dsd_state* state, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!opts) {
        return -1;
    }
    int mode = DSD_TONE_FILTER_OFF;
    dsd_tone_set set;
    DSD_MEMSET(&set, 0, sizeof(set));
    dsd_scan_mode_configured_tone_policy(opts, state, &mode, &set);
    const dsd_scan_option_values* row = rx_tone_row_policy(state);
    char configured[DSD_APP_RX_TONE_POLICY_TEXT_SIZE];
    (void)rx_tone_format_policy(mode, &set, configured, sizeof(configured),
                                row ? RX_TONE_NOTICE_DEFAULT_LIST_SIZE : RX_TONE_NOTICE_LIST_SIZE);
    if (!row) {
        DSD_SNPRINTF(out, out_size, "Applied: Tone filter -> %s", configured);
        return 0;
    }
    char shadowing[DSD_APP_RX_TONE_POLICY_TEXT_SIZE];
    (void)rx_tone_format_policy(row->tone_filter, &row->tone_set, shadowing, sizeof(shadowing),
                                RX_TONE_NOTICE_ROW_LIST_SIZE);
    DSD_SNPRINTF(out, out_size, "Default tone filter -> %s; this channel overrides it (%s)", configured, shadowing);
    return 0;
}
