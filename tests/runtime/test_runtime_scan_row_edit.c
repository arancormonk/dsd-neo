// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * Session edits of a scan row's own settings ("this channel", issue #518): which fields a row of each class takes, the
 * value checks, the set / inherit / reset transitions, and how an edit lays over the options a list row carries.
 */

#include <assert.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static dsd_scan_option_values
parse_row(const char* text, unsigned int mode) {
    dsd_scan_options options;
    char err[160] = {0};
    const int rc = dsd_scan_options_parse(text, mode, 1, &options, err, sizeof err);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "parse '%s': %s\n", text, err);
    }
    assert(rc == 0);
    return options.values;
}

/* The fields follow the options grammar: squelch on every class, the width on nfm and am, the tone policy on nfm, and
   the gain only where the caller says the row runs one of its own. A blank -Y row in a legacy list has no scope. */
static void
test_fields_follow_the_option_grammar(void) {
    const uint32_t sq = DSD_SCAN_ROW_FIELD_SQUELCH;
    assert(dsd_scan_row_edit_fields(DSD_SCAN_MODE_NFM, 0) == (sq | DSD_SCAN_ROW_FIELD_WIDTH | DSD_SCAN_ROW_FIELD_TONE));
    assert(dsd_scan_row_edit_fields(DSD_SCAN_MODE_AM, 0) == (sq | DSD_SCAN_ROW_FIELD_WIDTH));
    assert(dsd_scan_row_edit_fields(DSD_SCAN_MODE_P25, 0) == sq);
    assert(dsd_scan_row_edit_fields(DSD_SCAN_MODE_DMR, 1) == (sq | DSD_SCAN_ROW_FIELD_GAIN));
    assert(dsd_scan_row_edit_fields(DSD_SCAN_MODE_AM, 1) == (sq | DSD_SCAN_ROW_FIELD_WIDTH | DSD_SCAN_ROW_FIELD_GAIN));
    assert(dsd_scan_row_edit_fields(DSD_SCAN_MODE_INHERIT, 1) == 0U);
    assert(dsd_scan_row_edit_fields(DSD_SCAN_MODE_LAST + 1U, 1) == 0U);
    /* The grammar the fields come from: an nfm row takes all three switches, an am row the width but no tone. */
    assert((dsd_scan_options_fields_for_mode(DSD_SCAN_MODE_NFM, 1)
            & (DSD_SCAN_OPT_SQUELCH | DSD_SCAN_OPT_BANDWIDTH | DSD_SCAN_OPT_TONE))
           == (DSD_SCAN_OPT_SQUELCH | DSD_SCAN_OPT_BANDWIDTH | DSD_SCAN_OPT_TONE));
    assert((dsd_scan_options_fields_for_mode(DSD_SCAN_MODE_AM, 1) & DSD_SCAN_OPT_TONE) == 0U);
    assert((dsd_scan_options_fields_for_mode(DSD_SCAN_MODE_DMR, 0) & DSD_SCAN_OPT_VOICE) == 0U);
    assert((dsd_scan_options_fields_for_mode(DSD_SCAN_MODE_DMR, 1) & DSD_SCAN_OPT_VOICE) != 0U);
    assert(dsd_scan_options_fields_for_mode(DSD_SCAN_MODE_LAST + 1U, 1) == 0U);
}

static void
test_value_checks(void) {
    dsd_scan_row_edit_value v;
    DSD_MEMSET(&v, 0, sizeof v);
    char err[96] = {0};
    v.squelch_db = -60;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_P25, DSD_SCAN_ROW_FIELD_SQUELCH, &v, err, sizeof err) == 1);
    v.squelch_db = 0;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_P25, DSD_SCAN_ROW_FIELD_SQUELCH, &v, err, sizeof err) == 1);
    v.squelch_db = 3;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_P25, DSD_SCAN_ROW_FIELD_SQUELCH, &v, err, sizeof err) == 0);
    assert(strstr(err, "-100 to 0") != NULL);
    v.squelch_db = -101;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_P25, DSD_SCAN_ROW_FIELD_SQUELCH, &v, NULL, 0) == 0);

    v.width_hz = 12500;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_FIELD_WIDTH, &v, err, sizeof err) == 1);
    v.width_hz = dsd_analog_width_max_hz(DSD_ANALOG_DEMOD_FM) + 1;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_FIELD_WIDTH, &v, err, sizeof err) == 0);
    v.width_hz = 8333;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_AM, DSD_SCAN_ROW_FIELD_WIDTH, &v, err, sizeof err) == 1);
    v.width_hz = dsd_analog_width_min_hz(DSD_ANALOG_DEMOD_AM) - 1;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_AM, DSD_SCAN_ROW_FIELD_WIDTH, &v, err, sizeof err) == 0);
    v.width_hz = 12500;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_DMR, DSD_SCAN_ROW_FIELD_WIDTH, &v, err, sizeof err) == 0);

    v.tone_filter = DSD_TONE_FILTER_ALLOW;
    DSD_MEMSET(&v.tone_set, 0, sizeof v.tone_set);
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_FIELD_TONE, &v, err, sizeof err) == 0);
    assert(dsd_tone_set_parse("100.0/D023N", &v.tone_set, NULL, 0) == 0);
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_FIELD_TONE, &v, err, sizeof err) == 1);
    v.tone_filter = DSD_TONE_FILTER_OFF;
    DSD_MEMSET(&v.tone_set, 0, sizeof v.tone_set);
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_FIELD_TONE, &v, err, sizeof err) == 1);
    v.tone_filter = 7;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_NFM, DSD_SCAN_ROW_FIELD_TONE, &v, err, sizeof err) == 0);

    v.gain_db = 0;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_DMR, DSD_SCAN_ROW_FIELD_GAIN, &v, err, sizeof err) == 1);
    v.gain_db = DSD_SCAN_ROW_EDIT_GAIN_MAX_DB;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_DMR, DSD_SCAN_ROW_FIELD_GAIN, &v, err, sizeof err) == 1);
    v.gain_db = DSD_SCAN_ROW_EDIT_GAIN_MAX_DB + 1;
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_DMR, DSD_SCAN_ROW_FIELD_GAIN, &v, err, sizeof err) == 0);
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_DMR, 0U, &v, err, sizeof err) == 0);
    assert(dsd_scan_row_edit_value_valid(DSD_SCAN_MODE_DMR, DSD_SCAN_ROW_FIELD_GAIN, NULL, err, sizeof err) == 0);
}

/* Set, inherit and reset move one field between the three states and leave the others alone; a bad change leaves the
   edit as it was. */
static void
test_change_transitions(void) {
    dsd_scan_row_edit edit;
    DSD_MEMSET(&edit, 0, sizeof edit);
    dsd_scan_row_edit_value v;
    DSD_MEMSET(&v, 0, sizeof v);
    v.squelch_db = -55;
    v.gain_db = 20;
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    assert(edit.set == DSD_SCAN_ROW_FIELD_SQUELCH && edit.inherit == 0U && edit.value.squelch_db == -55);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);
    assert(edit.set == DSD_SCAN_ROW_FIELD_SQUELCH && edit.inherit == DSD_SCAN_ROW_FIELD_GAIN);
    assert(dsd_scan_row_edit_fields_edited(&edit) == (DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_GAIN));
    /* SET over an inherited field takes it out of inherit, and the other field's value is untouched. */
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    assert(edit.inherit == 0U && edit.value.gain_db == 20 && edit.value.squelch_db == -55);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);
    assert(edit.set == DSD_SCAN_ROW_FIELD_GAIN && edit.inherit == DSD_SCAN_ROW_FIELD_SQUELCH);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_RESET, NULL) == 0);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_RESET, NULL) == 0);
    assert(dsd_scan_row_edit_fields_edited(&edit) == 0U);

    const dsd_scan_row_edit before = edit;
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, NULL) == -1);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_GAIN,
                                    DSD_SCAN_ROW_EDIT_INHERIT, NULL)
           == -1);
    assert(dsd_scan_row_edit_change(&edit, 1U << 9, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == -1);
    /* An action that is none of the three, as a frontend could send one. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- deliberately out of range: the refusal under test
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, (dsd_scan_row_edit_action)9, &v) == -1);
    assert(dsd_scan_row_edit_change(NULL, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_RESET, NULL) == -1);
    assert(edit.set == before.set && edit.inherit == before.inherit);
    assert(dsd_scan_row_edit_fields_edited(NULL) == 0U);
}

/* An edit lays over the row's own options: a set field takes the edit's value and its option bit, an inherited one
   loses the bit, everything else -- other fields, options the edit cannot touch -- stays the row's. */
static void
test_apply_over_the_row(void) {
    const dsd_scan_option_values row = parse_row(
        "--squelch-db -60 --nfm-bandwidth-hz 12500 --tone-allow 100.0 --scan-max-visit-ms 4000", DSD_SCAN_MODE_NFM);
    dsd_scan_option_values out;

    dsd_scan_row_edit_apply(&row, NULL, DSD_SCAN_MODE_NFM, &out);
    assert(out.present == row.present && out.squelch_db == -60 && out.channel_bw_hz == 12500);

    dsd_scan_row_edit edit;
    DSD_MEMSET(&edit, 0, sizeof edit);
    dsd_scan_row_edit_value v;
    DSD_MEMSET(&v, 0, sizeof v);
    v.squelch_db = -45;
    v.width_hz = 16000;
    v.tone_filter = DSD_TONE_FILTER_BLOCK;
    assert(dsd_tone_set_parse("D023N", &v.tone_set, NULL, 0) == 0);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    dsd_scan_row_edit_apply(&row, &edit, DSD_SCAN_MODE_NFM, &out);
    assert(out.squelch_db == -45 && out.channel_bw_hz == 16000 && out.channel_bw_kind == DSD_ANALOG_DEMOD_FM);
    assert(out.tone_filter == DSD_TONE_FILTER_BLOCK && dsd_tone_set_equal(&out.tone_set, &v.tone_set));
    assert((out.present & DSD_SCAN_OPT_MAX_VISIT) && out.max_visit_ms == 4000);

    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);
    dsd_scan_row_edit_apply(&row, &edit, DSD_SCAN_MODE_NFM, &out);
    assert((out.present & (DSD_SCAN_OPT_SQUELCH | DSD_SCAN_OPT_BANDWIDTH | DSD_SCAN_OPT_TONE)) == 0U);
    assert(out.present & DSD_SCAN_OPT_MAX_VISIT);

    /* A row with no options at all: the edit alone, the AM width under the AM kind. */
    DSD_MEMSET(&edit, 0, sizeof edit);
    v.width_hz = 8333;
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    dsd_scan_row_edit_apply(NULL, &edit, DSD_SCAN_MODE_AM, &out);
    assert(out.present == DSD_SCAN_OPT_BANDWIDTH && out.channel_bw_hz == 8333
           && out.channel_bw_kind == DSD_ANALOG_DEMOD_AM);
    /* Inheriting what the row never set is no change. */
    DSD_MEMSET(&edit, 0, sizeof edit);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);
    dsd_scan_row_edit_apply(NULL, &edit, DSD_SCAN_MODE_DMR, &out);
    assert(out.present == 0U);
    dsd_scan_row_edit_apply(&row, &edit, DSD_SCAN_MODE_NFM, NULL);
}

static void
test_gain(void) {
    int is_set = -1;
    int db = -1;
    dsd_scan_row_edit_gain(1, 10, NULL, &is_set, &db);
    assert(is_set == 1 && db == 10);
    dsd_scan_row_edit_gain(0, 33, NULL, &is_set, &db);
    assert(is_set == 0 && db == 0);

    dsd_scan_row_edit edit;
    DSD_MEMSET(&edit, 0, sizeof edit);
    dsd_scan_row_edit_value v;
    DSD_MEMSET(&v, 0, sizeof v);
    v.gain_db = 0;
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    dsd_scan_row_edit_gain(1, 10, &edit, &is_set, &db);
    assert(is_set == 1 && db == 0);
    v.gain_db = 25;
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_SET, &v) == 0);
    dsd_scan_row_edit_gain(0, 0, &edit, &is_set, &db);
    assert(is_set == 1 && db == 25);
    assert(dsd_scan_row_edit_change(&edit, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);
    dsd_scan_row_edit_gain(1, 10, &edit, &is_set, &db);
    assert(is_set == 0 && db == 0);
    dsd_scan_row_edit_gain(1, 10, &edit, NULL, NULL);
}

/* Field by field: the struct has padding, so its bytes say nothing. */
static int
edits_equal(const dsd_scan_row_edit* a, const dsd_scan_row_edit* b) {
    return a->set == b->set && a->inherit == b->inherit && a->value.squelch_db == b->value.squelch_db
           && a->value.width_hz == b->value.width_hz && a->value.tone_filter == b->value.tone_filter
           && dsd_tone_set_equal(&a->value.tone_set, &b->value.tone_set) && a->value.gain_db == b->value.gain_db;
}

/* A rollback of one field takes that field from the edit it goes back to -- set, inherited or neither, with its value --
   and leaves the others as they are now. */
static void
test_take_fields(void) {
    dsd_scan_row_edit now;
    DSD_MEMSET(&now, 0, sizeof now);
    dsd_scan_row_edit_value value;
    DSD_MEMSET(&value, 0, sizeof value);
    value.width_hz = 16000;
    value.squelch_db = -45;
    assert(dsd_scan_row_edit_change(&now, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, &value) == 0);
    assert(dsd_scan_row_edit_change(&now, DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, &value) == 0);
    dsd_scan_row_edit before;
    DSD_MEMSET(&before, 0, sizeof before);
    value.width_hz = 12500;
    assert(dsd_scan_row_edit_change(&before, DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, &value) == 0);
    assert(dsd_scan_row_edit_change(&before, DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_INHERIT, NULL) == 0);

    dsd_scan_row_edit edit = now;
    dsd_scan_row_edit_take_fields(&edit, &before, DSD_SCAN_ROW_FIELD_WIDTH);
    assert(edit.set == (DSD_SCAN_ROW_FIELD_WIDTH | DSD_SCAN_ROW_FIELD_SQUELCH) && edit.inherit == 0U);
    assert(edit.value.width_hz == 12500 && edit.value.squelch_db == -45);
    /* Neither set nor inherited before: the squelch edit goes; the gain comes back inherited. */
    dsd_scan_row_edit_take_fields(&edit, &before, DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_GAIN);
    assert(edit.set == DSD_SCAN_ROW_FIELD_WIDTH && edit.inherit == DSD_SCAN_ROW_FIELD_GAIN);
    edit = now;
    dsd_scan_row_edit_take_fields(&edit, &before, 0U);
    assert(edits_equal(&edit, &now));
    dsd_scan_row_edit_take_fields(NULL, &before, DSD_SCAN_ROW_FIELDS_ALL);
    dsd_scan_row_edit_take_fields(&edit, NULL, DSD_SCAN_ROW_FIELDS_ALL);
    assert(edits_equal(&edit, &now));
}

/* A receiver's width request differs when the row's own width does, in value or kind, or when one side sets none: the
   decoder can run the same width either way while a rigctl peer is asked for the row's passband or for -B. */
static void
test_width_request_differs(void) {
    const dsd_scan_option_values own = parse_row("--nfm-bandwidth-hz 12500", DSD_SCAN_MODE_NFM);
    const dsd_scan_option_values wider = parse_row("--nfm-bandwidth-hz 16000", DSD_SCAN_MODE_NFM);
    const dsd_scan_option_values none = parse_row("--squelch-db -50", DSD_SCAN_MODE_NFM);
    assert(!dsd_scan_row_edit_width_request_differs(&own, &own));
    assert(dsd_scan_row_edit_width_request_differs(&own, &wider));
    assert(dsd_scan_row_edit_width_request_differs(&own, &none));
    assert(dsd_scan_row_edit_width_request_differs(NULL, &own));
    assert(!dsd_scan_row_edit_width_request_differs(&none, NULL));
    assert(!dsd_scan_row_edit_width_request_differs(NULL, NULL));
    dsd_scan_option_values am = own;
    am.channel_bw_kind = DSD_ANALOG_DEMOD_AM;
    assert(dsd_scan_row_edit_width_request_differs(&own, &am));
}

int
main(void) {
    test_fields_follow_the_option_grammar();
    test_value_checks();
    test_change_transitions();
    test_apply_over_the_row();
    test_gain();
    test_take_fields();
    test_width_request_differs();
    printf("RUNTIME_SCAN_ROW_EDIT: OK\n");
    return 0;
}
