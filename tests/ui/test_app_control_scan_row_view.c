// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The scan row on air as the "this channel" editors see it (issue #518): whether one is on air for the scanner
 * running now, the fields it offers, the payload naming it, and the words of every notice. */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
expect_notice(const char* label, uint32_t field, unsigned int mode, int action, const char* value, int status,
              const char* why, const char* want) {
    char out[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    assert(dsd_app_scan_row_notice(label, field, mode, action, value, status, why, out, sizeof out) == 0);
    if (strcmp(out, want) != 0) {
        DSD_FPRINTF(stderr, "notice '%s', want '%s'\n", out, want);
        assert(0);
    }
}

static void
test_view_follows_the_published_row(dsd_opts* opts, dsd_state* state) {
    dsd_app_scan_row_view view;
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && !view.active && view.row == -1);
    assert(!dsd_app_scan_row_view_offers(&view, DSD_SCAN_ROW_FIELD_SQUELCH));
    dsd_app_scan_row_edit_payload p;
    assert(dsd_app_scan_row_view_payload(&view, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, &p) == -1);

    /* A --trunk-scan target on air. */
    opts->trunk_scan_enabled = 1;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_AM) == 0);
    state->scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_TRUNK_SCAN;
    state->scan_row_session = 9U;
    state->scan_row_index = 2;
    state->scan_row_editable = (uint8_t)(DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_WIDTH);
    state->scan_row_listed = (uint8_t)DSD_SCAN_ROW_FIELD_WIDTH;
    state->scan_row_edited = (uint8_t)DSD_SCAN_ROW_FIELD_SQUELCH;
    DSD_SNPRINTF(state->trunk_scan_active_id, sizeof state->trunk_scan_active_id, "%s", "tower-am");
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && view.active);
    assert(view.scanner == DSD_SCAN_ROW_SCANNER_TRUNK_SCAN && view.session == 9U && view.row == 2);
    assert(view.mode == DSD_SCAN_MODE_AM && strcmp(view.label, "tower-am") == 0);
    assert(strcmp(view.target_id, "tower-am") == 0);
    assert(view.listed == DSD_SCAN_ROW_FIELD_WIDTH && view.edited == DSD_SCAN_ROW_FIELD_SQUELCH);
    assert(dsd_app_scan_row_view_offers(&view, DSD_SCAN_ROW_FIELD_WIDTH));
    assert(!dsd_app_scan_row_view_offers(&view, DSD_SCAN_ROW_FIELD_TONE));
    assert(!dsd_app_scan_row_view_offers(&view, 0U));
    assert(dsd_app_scan_row_view_payload(&view, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_INHERIT, &p) == 0);
    assert(p.session == 9U && p.scanner == DSD_SCAN_ROW_SCANNER_TRUNK_SCAN && p.row == 2);
    assert(p.mode == DSD_SCAN_MODE_AM && p.field == (int32_t)DSD_SCAN_ROW_FIELD_WIDTH);
    assert(p.action == DSD_SCAN_ROW_EDIT_INHERIT && strcmp(p.target_id, "tower-am") == 0 && p.tone_list[0] == '\0');

    /* The options snapshot is of another row's scope (read between the two publishes): the row is still on air, but
       no editor is offered to start from values that are not its own. */
    assert(view.opts_match);
    state->scan_row_scope_seq = opts->scan_row_scope_seq + 1U;
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && view.active && !view.opts_match);
    assert(!dsd_app_scan_row_view_offers(&view, DSD_SCAN_ROW_FIELD_WIDTH));
    state->scan_row_scope_seq = opts->scan_row_scope_seq;
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && view.opts_match);

    /* The row belongs to the scanner running now: with trunk scan off it is not on air. */
    opts->trunk_scan_enabled = 0;
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && !view.active);

    /* A -Y row: its name, or "Ch N (F MHz)". */
    opts->scanner_mode = 1;
    state->scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_CHANNEL_SCAN;
    state->scan_row_index = 1;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = 851012500L;
    state->trunk_lcn_freq[1] = 154430000L;
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && view.active && view.target_id[0] == '\0');
    assert(strcmp(view.label, "Ch 2 (154.430000 MHz)") == 0);
    assert(dsd_state_trunk_lcn_name_set(state, 1, "Fire Dispatch") == 0);
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && strcmp(view.label, "Fire Dispatch") == 0);
    state->scan_row_session = 0U;
    assert(dsd_app_scan_row_view_get(opts, state, &view) == 0 && !view.active);
    dsd_scan_mode_leave(opts, state);
}

static void
test_values_and_notices(void) {
    dsd_scan_row_edit_value v;
    DSD_MEMSET(&v, 0, sizeof v);
    char out[64];
    v.squelch_db = -55;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_SQUELCH, &v, out, sizeof out) == 0);
    assert(strcmp(out, "-55 dB") == 0);
    v.squelch_db = 0;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_SQUELCH, &v, out, sizeof out) == 0);
    assert(strcmp(out, "off") == 0);
    v.squelch_mode = DSD_SQUELCH_MODE_NOISE;
    v.squelch_margin_db = 12;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_SQUELCH, &v, out, sizeof out) == 0);
    assert(strcmp(out, "noise +12 dB") == 0);
    v.squelch_mode = DSD_SQUELCH_MODE_AUTO;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_SQUELCH, &v, out, sizeof out) == 0);
    assert(strcmp(out, "auto +12 dB") == 0);
    v.squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    v.squelch_margin_db = 0;
    v.width_hz = 12500;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_WIDTH, &v, out, sizeof out) == 0);
    assert(strcmp(out, "12.5 kHz") == 0);
    v.tone_filter = DSD_TONE_FILTER_BLOCK;
    assert(dsd_tone_set_parse("100.0/D023N", &v.tone_set, NULL, 0) == 0);
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_TONE, &v, out, sizeof out) == 0);
    assert(strncmp(out, "block 100.0 Hz", 14) == 0 && strstr(out, "D023N") != NULL);
    v.tone_filter = DSD_TONE_FILTER_OFF;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_TONE, &v, out, sizeof out) == 0);
    assert(strcmp(out, "off") == 0);
    v.gain_db = 20;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_GAIN, &v, out, sizeof out) == 0);
    assert(strcmp(out, "20 dB") == 0);
    v.gain_db = 0;
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_GAIN, &v, out, sizeof out) == 0);
    assert(strcmp(out, "AGC") == 0);
    assert(dsd_app_scan_row_value_text(0U, &v, out, sizeof out) == -1);
    assert(dsd_app_scan_row_value_text(DSD_SCAN_ROW_FIELD_GAIN, NULL, out, sizeof out) == -1);

    assert(strcmp(dsd_app_scan_row_field_name(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_AM), "AM bandwidth") == 0);
    assert(strcmp(dsd_app_scan_row_field_name(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_NFM), "NFM bandwidth") == 0);

    expect_notice("county-p25", DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_MODE_P25, DSD_SCAN_ROW_EDIT_SET, "-55 dB",
                  DSD_SCAN_ROW_EDIT_APPLIED, NULL, "This channel (county-p25): squelch -55 dB for this session");
    expect_notice("county-p25", DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_MODE_P25, DSD_SCAN_ROW_EDIT_SET, "-55 dB",
                  DSD_SCAN_ROW_EDIT_STORED, NULL, "This channel (county-p25): squelch -55 dB from its next visit");
    expect_notice("air", DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_AM, DSD_SCAN_ROW_EDIT_INHERIT, NULL,
                  DSD_SCAN_ROW_EDIT_APPLIED, NULL, "This channel (air): AM bandwidth follows the default");
    expect_notice("air", DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_AM, DSD_SCAN_ROW_EDIT_RESET, NULL,
                  DSD_SCAN_ROW_EDIT_STORED, NULL,
                  "This channel (air): AM bandwidth back to the list value from its next visit");
    expect_notice("air", DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_EDIT_SET, "25 kHz",
                  DSD_SCAN_ROW_EDIT_REFUSED, "does not fit", "Refused: this channel's NFM bandwidth: does not fit");
    expect_notice(NULL, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_MODE_DMR, DSD_SCAN_ROW_EDIT_SET, NULL,
                  DSD_SCAN_ROW_EDIT_REFUSED, NULL, "Refused: this channel's RTL gain: not allowed");
    expect_notice("x", DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_EDIT_SET, NULL, DSD_SCAN_ROW_EDIT_STALE,
                  NULL, "Refused: the scan changed; nothing applied");
    expect_notice("x", DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_EDIT_SET, NULL, DSD_SCAN_ROW_EDIT_BUSY,
                  NULL, "Busy: the scan is retuning; try again");
    expect_notice("x", DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_EDIT_SET, NULL,
                  DSD_SCAN_ROW_EDIT_UNAVAILABLE, NULL, "Refused: no scan is running");
    assert(dsd_app_scan_row_notice("x", 0U, 0U, 0, NULL, 0, NULL, NULL, 0U) == -1);
    /* The longest label still leaves the outcome whole: the label gives way, cut short, and never inside a UTF-8
       character. */
    char label[DSD_APP_SCAN_ROW_LABEL_SIZE];
    DSD_MEMSET(label, 'L', sizeof label - 1U);
    label[sizeof label - 1U] = '\0';
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    assert(dsd_app_scan_row_notice(label, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_EDIT_RESET, NULL,
                                   DSD_SCAN_ROW_EDIT_STORED, NULL, notice, sizeof notice)
           == 0);
    static const char k_tail[] = "...): NFM bandwidth back to the list value from its next visit";
    const size_t len = strlen(notice);
    assert(len < sizeof notice && strncmp(notice, "This channel (LLL", 17) == 0);
    assert(len >= sizeof k_tail - 1U && strcmp(notice + len - (sizeof k_tail - 1U), k_tail) == 0);
    /* Two-byte characters ("é") all the way: the room left (51 bytes before the "...") ends inside one, so the cut
       backs off to the boundary before it. */
    DSD_MEMSET(label, 0, sizeof label);
    for (size_t i = 0U; i + 2U < sizeof label; i += 2U) {
        DSD_MEMCPY(&label[i], "\xC3\xA9", 2U);
    }
    assert(dsd_app_scan_row_notice(label, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_EDIT_RESET, NULL,
                                   DSD_SCAN_ROW_EDIT_STORED, NULL, notice, sizeof notice)
           == 0);
    const char* dots = strstr(notice, "...): ");
    assert(dots && dots > notice + 14 && ((unsigned char)dots[-1] & 0xC0U) == 0x80U
           && (unsigned char)dots[-2] == 0xC3U);
    /* A short label is left whole. */
    expect_notice("fire", DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_EDIT_RESET, NULL,
                  DSD_SCAN_ROW_EDIT_STORED, NULL,
                  "This channel (fire): NFM bandwidth back to the list value from its next visit");
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    assert(opts && state);
    test_view_follows_the_published_row(opts, state);
    test_values_and_notices();
    assert(dsd_app_scan_row_view_get(NULL, state, NULL) == -1);
    dsd_state_trunk_lcn_name_free(state);
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    printf("APP_CONTROL_SCAN_ROW_VIEW: OK\n");
    return 0;
}
