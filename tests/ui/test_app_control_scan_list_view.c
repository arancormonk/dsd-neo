// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The scan list view (issue #697): the trunk-scan target lists every frontend shows and switches, read from the fields
 * the coordinator publishes. States are built by hand on the heap, as a published snapshot would carry them. */

#include <assert.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
reset(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
}

/* A running trunk scan with @p count lists named "list1" ..., list i holding i * 10 targets, all on. */
static void
seed_lists(dsd_opts* opts, dsd_state* state, int count) {
    reset(opts, state);
    opts->trunk_scan_enabled = 1;
    state->trunk_scan_session = 41U;
    state->trunk_scan_lists_seq = 9U;
    state->trunk_scan_list_count = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        DSD_SNPRINTF(state->trunk_scan_lists[i].name, sizeof state->trunk_scan_lists[i].name, "list%d", i + 1);
        state->trunk_scan_lists[i].enabled = 1U;
        state->trunk_scan_lists[i].targets = (uint16_t)((i + 1) * 10);
    }
}

static dsd_app_scan_list_view
view_of(const dsd_opts* opts, const dsd_state* state) {
    dsd_app_scan_list_view v;
    DSD_MEMSET(&v, 0xA5, sizeof v);
    assert(dsd_app_scan_list_view_get(opts, state, &v) == 0);
    return v;
}

/* Every entry's number, state and on-air mark at once, written as "1 [2] *3": "*" before a list that is off, the list
   on air in brackets. */
static void
expect_entries(const char* what, const dsd_app_scan_list_view* v, const char* want) {
    char got[64] = "";
    size_t used = 0U;
    for (int i = 0; i < (int)v->list_count; i++) {
        const dsd_app_scan_list_entry* e = &v->lists[i];
        DSD_SNPRINTF(got + used, sizeof got - used, "%s%s%s%u%s", i > 0 ? " " : "", e->has_active ? "[" : "",
                     e->enabled ? "" : "*", (unsigned)e->number, e->has_active ? "]" : "");
        used = strlen(got);
    }
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: entries \"%s\", want \"%s\"\n", what, got, want);
        assert(0);
    }
}

static void
expect_toggleable(const char* what, const dsd_app_scan_list_view* v, int toggleable, int running, int paused) {
    if (v->toggleable != toggleable || v->running != running || v->paused != paused) {
        DSD_FPRINTF(stderr, "%s: toggleable %u running %u paused %u, want %d %d %d\n", what, v->toggleable, v->running,
                    v->paused, toggleable, running, paused);
        assert(0);
    }
}

static void
test_null_arguments(dsd_opts* opts, dsd_state* state) {
    dsd_app_scan_list_view v;
    DSD_MEMSET(&v, 0xA5, sizeof v);
    assert(dsd_app_scan_list_view_get(NULL, state, &v) == -1 && v.list_count == 0U);
    assert(dsd_app_scan_list_view_get(opts, NULL, &v) == -1);
    assert(dsd_app_scan_list_view_get(opts, state, NULL) == -1);
}

/* No trunk scan: nothing, even with lists still published (they are the coordinator's only while it is configured). */
static void
test_no_lists(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    dsd_app_scan_list_view v = view_of(opts, state);
    assert(v.list_count == 0U && v.lists_on == 0U && v.targets_on == 0U && v.session == 0U);
    expect_toggleable("no scan", &v, 0, 0, 0);

    seed_lists(opts, state, 3);
    opts->trunk_scan_enabled = 0;
    v = view_of(opts, state);
    assert(v.list_count == 0U && v.session == 0U);
    expect_toggleable("lists left behind, no scan", &v, 0, 0, 0);

    /* A trunk scan whose coordinator has not published yet. */
    seed_lists(opts, state, 0);
    v = view_of(opts, state);
    assert(v.list_count == 0U && v.session == 41U);
    expect_toggleable("no lists yet", &v, 0, 1, 0);
}

/* One list: shown, never toggleable. */
static void
test_one_list(dsd_opts* opts, dsd_state* state) {
    seed_lists(opts, state, 1);
    state->trunk_scan_active_list = 1U;
    dsd_app_scan_list_view v = view_of(opts, state);
    assert(v.list_count == 1U && v.lists_on == 1U && v.targets_on == 10U && v.active_list == 1U);
    assert(v.lists[0].number == 1U && v.lists[0].enabled == 1U && v.lists[0].has_active == 1U);
    assert(v.lists[0].targets == 10U && strcmp(v.lists[0].name, "list1") == 0);
    expect_entries("one list", &v, "[1]");
    expect_toggleable("one list", &v, 0, 1, 0);
}

/* Three lists: the counts, the list on air, the off marker, and the session and key a frontend acts on. */
static void
test_three_lists(dsd_opts* opts, dsd_state* state) {
    seed_lists(opts, state, 3);
    state->trunk_scan_lists[2].enabled = 0U;
    state->trunk_scan_active_list = 2U;
    dsd_app_scan_list_view v = view_of(opts, state);
    assert(v.session == 41U && v.lists_seq == 9U);
    assert(v.list_count == 3U && v.lists_on == 2U && v.targets_on == 30U);
    assert(v.active_list == 2U && v.active_list_off == 0U);
    for (int i = 0; i < 3; i++) {
        assert(v.lists[i].number == (uint8_t)(i + 1) && v.lists[i].targets == (uint16_t)((i + 1) * 10));
        assert(v.lists[i].has_active == (i == 1 ? 1U : 0U));
    }
    assert(v.lists[0].enabled == 1U && v.lists[1].enabled == 1U && v.lists[2].enabled == 0U);
    assert(strcmp(v.lists[2].name, "list3") == 0);
    /* Entries past the count stay zeroed. */
    assert(v.lists[3].number == 0U && v.lists[3].name[0] == '\0');
    expect_entries("three lists, 2 on air, 3 off", &v, "1 [2] *3");
    expect_toggleable("three lists running", &v, 1, 1, 0);

    /* The receiver still on a target whose list went off (it leaves when another target can take it). */
    state->trunk_scan_lists[1].enabled = 0U;
    state->trunk_scan_lists[2].enabled = 1U;
    state->trunk_scan_active_list_off = 1U;
    v = view_of(opts, state);
    assert(v.active_list == 2U && v.active_list_off == 1U && v.lists_on == 2U && v.targets_on == 40U);
    expect_entries("on air on a list that is off", &v, "1 [*2] 3");

    /* Nothing on air: no list marked. */
    state->trunk_scan_active_list = 0U;
    state->trunk_scan_active_list_off = 0U;
    v = view_of(opts, state);
    assert(v.active_list == 0U && v.active_list_off == 0U);
    expect_entries("none on air", &v, "1 *2 3");

    /* An active list past the count is no list. */
    state->trunk_scan_active_list = 5U;
    state->trunk_scan_active_list_off = 1U;
    v = view_of(opts, state);
    assert(v.active_list == 0U && v.active_list_off == 0U);
    expect_entries("active list out of range", &v, "1 *2 3");
}

/* toggleable: running with 2+ lists only; paused and stopped (no coordinator configured) refuse it. */
static void
test_toggleable(dsd_opts* opts, dsd_state* state) {
    seed_lists(opts, state, 2);
    state->trunk_scan_active_list = 1U;
    dsd_app_scan_list_view v = view_of(opts, state);
    expect_toggleable("two lists running", &v, 1, 1, 0);

    /* Paused: the lists stay readable, the on-air fields are zeroed by the coordinator. */
    state->trunk_scan_paused = 1U;
    state->trunk_scan_active_list = 0U;
    v = view_of(opts, state);
    expect_toggleable("paused", &v, 0, 0, 1);
    assert(v.list_count == 2U && v.lists_on == 2U && v.session == 41U);
    expect_entries("paused", &v, "1 2");

    state->trunk_scan_paused = 0U;
    opts->trunk_scan_enabled = 0;
    v = view_of(opts, state);
    expect_toggleable("stopped", &v, 0, 0, 0);
}

/* Every list there can be, and a published count past it clamped; each entry fits. */
static void
test_eight_lists(dsd_opts* opts, dsd_state* state) {
    seed_lists(opts, state, DSD_TRUNK_SCAN_MAX_LISTS);
    for (int i = 0; i < DSD_TRUNK_SCAN_MAX_LISTS; i++) {
        DSD_MEMSET(state->trunk_scan_lists[i].name, 'n', sizeof state->trunk_scan_lists[i].name - 1U);
        state->trunk_scan_lists[i].enabled = 0U;
    }
    state->trunk_scan_lists[0].enabled = 1U;
    state->trunk_scan_active_list = 8U;
    state->trunk_scan_active_list_off = 1U;
    dsd_app_scan_list_view v = view_of(opts, state);
    assert(v.list_count == 8U && v.lists_on == 1U && v.targets_on == 10U);
    assert(strlen(v.lists[7].name) == sizeof v.lists[7].name - 1U && v.lists[7].has_active == 1U);
    expect_entries("eight lists", &v, "1 *2 *3 *4 *5 *6 *7 [*8]");
    expect_toggleable("eight lists", &v, 1, 1, 0);

    state->trunk_scan_list_count = 200U;
    v = view_of(opts, state);
    assert(v.list_count == 8U);
    expect_entries("count past the cap", &v, "1 *2 *3 *4 *5 *6 *7 [*8]");
}

/* Issue #644: which targets a frequency typed by hand could pick, as the coordinator published it: read while the scan
   in force is a trunk scan, one answer per target, past the count none; with no trunk scan, nothing. */
static void
test_entry_eligibility(dsd_opts* opts, dsd_state* state) {
    seed_lists(opts, state, 2);
    const uint8_t reasons[] = {0U, 4U, 1U};
    assert(dsd_state_trunk_scan_entry_reasons_set(state, reasons, 3U) == 0);
    state->trunk_scan_entry_rev = 77U;
    state->trunk_scan_entry_on_air = -1;
    dsd_app_scan_entry_summary summary;
    DSD_MEMSET(&summary, 0xA5, sizeof summary);
    assert(dsd_app_scan_entry_summary_get(opts, state, &summary) == 0);
    assert(summary.count == 3U && summary.revision == 77U && summary.on_air == -1);
    state->trunk_scan_entry_on_air = 2;
    assert(dsd_app_scan_entry_summary_get(opts, state, &summary) == 0 && summary.on_air == 2);
    state->trunk_scan_entry_on_air = 3; /* past the count: none */
    assert(dsd_app_scan_entry_summary_get(opts, state, &summary) == 0 && summary.on_air == -1);
    assert(dsd_app_scan_entry_eligible(opts, state, 0U) == 1);
    assert(dsd_app_scan_entry_eligible(opts, state, 1U) == 0);
    assert(dsd_app_scan_entry_eligible(opts, state, 2U) == 0);
    assert(dsd_app_scan_entry_eligible(opts, state, 3U) == -1);
    opts->trunk_scan_enabled = 0;
    assert(dsd_app_scan_entry_summary_get(opts, state, &summary) == 0 && summary.count == 0U);
    assert(dsd_app_scan_entry_eligible(opts, state, 0U) == -1);
    assert(dsd_app_scan_entry_summary_get(NULL, state, &summary) == -1);
    dsd_state_trunk_scan_entry_reasons_free(state);
}

/* The distinct targets of a set of lists (issue #697): the membership counts of every mask that meets it, never the
   lists' own counts. Lists 1 {a, b, s}, 2 {s, t, c}, 3 {t, d}: s in lists 1 and 2, t in 2 and 3. */
static void
test_targets_on(dsd_state* state) {
    DSD_MEMSET(state, 0, sizeof *state);
    assert(dsd_app_scan_lists_targets_on(NULL, 0xFFU) == 0U);
    assert(dsd_app_scan_lists_targets_on(state, 0xFFU) == 0U);
    state->trunk_scan_membership_counts[0x1 - 1] = 2U; /* a, b */
    state->trunk_scan_membership_counts[0x2 - 1] = 1U; /* c */
    state->trunk_scan_membership_counts[0x3 - 1] = 1U; /* s */
    state->trunk_scan_membership_counts[0x4 - 1] = 1U; /* d */
    state->trunk_scan_membership_counts[0x6 - 1] = 1U; /* t */

    static const struct {
        unsigned int on;
        unsigned int want;
    } k_cases[] = {
        {0x0U, 0U}, {0x1U, 3U}, {0x2U, 3U}, {0x4U, 2U},  {0x3U, 5U},
        {0x5U, 5U}, {0x6U, 4U}, {0x7U, 6U}, {0xF8U, 0U}, {0xFFU, 6U},
    };

    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        const unsigned int got = dsd_app_scan_lists_targets_on(state, k_cases[i].on);
        if (got != k_cases[i].want) {
            DSD_FPRINTF(stderr, "targets on for lists 0x%x: %u, want %u\n", k_cases[i].on, got, k_cases[i].want);
            assert(0);
        }
    }
    /* The eighth list's own targets (mask 0x80) and those every list holds (0xFF) are counted too. */
    state->trunk_scan_membership_counts[0x80 - 1] = 4U;
    state->trunk_scan_membership_counts[0xFF - 1] = 1U;
    assert(dsd_app_scan_lists_targets_on(state, 0x80U) == 5U);
    assert(dsd_app_scan_lists_targets_on(state, 0x1U) == 4U);
    assert(dsd_app_scan_lists_targets_on(state, 0xFFU) == 11U);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts != NULL && state != NULL);
    test_null_arguments(opts, state);
    test_no_lists(opts, state);
    test_one_list(opts, state);
    test_three_lists(opts, state);
    test_toggleable(opts, state);
    test_eight_lists(opts, state);
    test_entry_eligibility(opts, state);
    test_targets_on(state);
    free(state);
    free(opts);
    printf("APP_CONTROL_SCAN_LIST_VIEW: OK\n");
    return 0;
}
