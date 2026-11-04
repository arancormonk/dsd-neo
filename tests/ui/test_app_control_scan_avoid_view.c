// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The scan avoid view (issue #672): which store the session's avoids live in, the summary and its key, the owned list
 * with identities and labels, the raw collection, and the decoder's LCN identity check. States are built by hand on the
 * heap, the way initState() leaves the avoid identity fields (instance and revisions stamped, the copy complete). */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_avoid_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scan_avoid_view_internal.h"

static void
reset(dsd_opts* opts, dsd_state* state) {
    dsd_state_trunk_lcn_free(state);
    dsd_state_trunk_scan_avoids_free(state);
    DSD_MEMSET(opts, 0, sizeof *opts);
    DSD_MEMSET(state, 0, sizeof *state);
    state->state_instance = dsd_state_rev_next();
    state->lcn_list_rev = dsd_state_rev_next();
    state->lcn_avoid_rev = dsd_state_rev_next();
    state->trunk_scan_avoids_ok = 1U;
    state->scan_avoids_complete = 1U;
}

static void
expect_label(const char* what, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: label \"%s\", want \"%s\"\n", what, got, want);
        assert(0);
    }
}

static void
expect_summary(const char* what, const dsd_opts* opts, const dsd_state* state, uint32_t kind, int available,
               size_t total, int can_avoid, int can_manage) {
    dsd_app_scan_avoid_summary s;
    DSD_MEMSET(&s, 0xA5, sizeof s);
    assert(dsd_app_scan_avoid_summary_get(opts, state, &s) == 0);
    if (s.kind != kind || s.available != available || s.total_known != available || s.total != total
        || s.can_avoid != can_avoid || s.can_manage != can_manage || s.instance != state->state_instance) {
        DSD_FPRINTF(stderr, "%s: kind=%u available=%u known=%u total=%zu avoid=%u manage=%u, want %u/%d/%d/%zu/%d/%d\n",
                    what, s.kind, s.available, s.total_known, s.total, s.can_avoid, s.can_manage, kind, available,
                    available, total, can_avoid, can_manage);
        assert(0);
    }
}

/* A -Y list of @p rows rows at 851.0125 MHz + 12.5 kHz steps. */
static void
seed_rows(dsd_state* state, int rows) {
    assert(dsd_state_trunk_lcn_reserve(state, (size_t)rows) == 0);
    for (int i = 0; i < rows; i++) {
        *dsd_state_trunk_lcn_slot(state, i) = 851012500L + (12500L * i);
    }
    state->lcn_freq_count = rows;
}

static void
test_null_arguments(dsd_opts* opts, dsd_state* state) {
    dsd_app_scan_avoid_summary s;
    DSD_MEMSET(&s, 0xA5, sizeof s);
    assert(dsd_app_scan_avoid_summary_get(NULL, state, &s) == -1 && s.kind == 0U && s.total == 0U);
    assert(dsd_app_scan_avoid_summary_get(opts, NULL, &s) == -1);
    assert(dsd_app_scan_avoid_summary_get(opts, state, NULL) == -1);
    assert(!dsd_app_scan_avoid_summary_same_key(NULL, &s) && !dsd_app_scan_avoid_summary_same_key(&s, NULL));
    dsd_app_scan_avoid_list list;
    DSD_MEMSET(&list, 0xA5, sizeof list);
    assert(dsd_app_scan_avoid_list_build(NULL, state, &list) == -1 && list.entries == NULL && list.count == 0U);
    assert(dsd_app_scan_avoid_list_build(opts, state, NULL) == -1);
    dsd_app_scan_avoid_raw raw;
    assert(dsd_app_scan_avoid_raw_collect(opts, NULL, &raw) == -1 && raw.entries == NULL);
    dsd_app_scan_avoid_list_free(NULL);
    dsd_app_scan_avoid_raw_free(NULL);
    char out[8] = "x";
    assert(dsd_app_scan_avoid_raw_label(NULL, out, sizeof out) == -1 && out[0] == '\0');
}

/* Trunk scan wins while configured, running or paused; -Y running, paused or stopped with its list (and -T over a list)
   is LCN; no list and no scan is NONE. Avoid is offered only while a rotation runs; the list and Clear whenever avoids
   exist (issue #672 B1, D2/D3). */
static void
test_kind_and_offers(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    expect_summary("nothing", opts, state, DSD_APP_SCAN_AVOID_KIND_NONE, 1, 0U, 0, 0);

    seed_rows(state, 3);
    expect_summary("stopped list, no avoids", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, 0U, 0, 0);
    opts->scanner_mode = 1;
    expect_summary("-Y running", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, 0U, 1, 1);
    assert(dsd_state_trunk_lcn_avoid_set(state, 0U, 1) == 0 && dsd_state_trunk_lcn_avoid_set(state, 1U, 1) == 0);
    expect_summary("-Y running, two avoided", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, 2U, 1, 1);
    opts->scanner_mode = 0;
    state->lcn_scan_paused = 1U;
    expect_summary("-Y paused", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, 2U, 0, 1);
    state->lcn_scan_paused = 0U;
    expect_summary("-Y stopped", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, 2U, 0, 1);
    opts->trunk_enable = 1;
    expect_summary("-T over the list", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, 2U, 0, 1);
    dsd_app_scan_avoid_summary s;
    assert(dsd_app_scan_avoid_summary_get(opts, state, &s) == 0);
    assert(s.list_rev == state->lcn_list_rev && s.revision == state->lcn_avoid_rev && s.session == 0U);

    /* -Y with no rows (nothing to avoid yet) is still the LCN store: its Avoid row stays as today. */
    reset(opts, state);
    opts->scanner_mode = 1;
    expect_summary("-Y with no rows", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, 0U, 1, 1);

    /* A trunk scan wins over the parked target's own LCN list and its flags. */
    reset(opts, state);
    seed_rows(state, 2);
    assert(dsd_state_trunk_lcn_avoid_set(state, 0U, 1) == 0);
    opts->trunk_scan_enabled = 1;
    state->trunk_scan_session = 77U;
    state->trunk_scan_avoid_rev = dsd_state_rev_next();
    expect_summary("trunk scan running, none avoided", opts, state, DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN, 1, 0U, 1, 1);
    const dsd_trunk_scan_avoid_entry entry = {.ordinal = 2U, .frequency_hz = 852000000U, .id = "b", .label = ""};
    assert(dsd_state_trunk_scan_avoids_set(state, &entry, 1U) == 0);
    state->trunk_scan_paused = 1U;
    expect_summary("trunk scan paused, one avoided", opts, state, DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN, 1, 1U, 0, 1);
    assert(dsd_app_scan_avoid_summary_get(opts, state, &s) == 0);
    assert(s.session == 77U && s.revision == state->trunk_scan_avoid_rev && s.list_rev == 0U);
}

/* An incomplete copy (a failed snapshot allocation) or a trunk-scan list the coordinator could not allocate: the total
   is unknown, no entries are listed, and the list row and Clear stay reachable (R7). */
static void
test_unavailable_keeps_manage(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    seed_rows(state, 3);
    assert(dsd_state_trunk_lcn_avoid_set(state, 2U, 1) == 0);
    state->scan_avoids_complete = 0U;
    expect_summary("incomplete LCN copy", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 0, 0U, 0, 1);
    dsd_app_scan_avoid_list list;
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0);
    assert(list.count == 0U && list.entries == NULL && list.summary.available == 0U);
    dsd_app_scan_avoid_list_free(&list);

    reset(opts, state);
    opts->trunk_scan_enabled = 1;
    state->trunk_scan_session = 5U;
    state->trunk_scan_paused = 1U;
    state->trunk_scan_avoids_ok = 0U;
    expect_summary("trunk-scan list not allocated", opts, state, DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN, 0, 0U, 0, 1);
    state->trunk_scan_avoids_ok = 1U;
    state->scan_avoids_complete = 0U;
    expect_summary("trunk-scan copy incomplete", opts, state, DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN, 0, 0U, 0, 1);
}

/* Labels: the row or list position first, so long duplicate names, ids with a common prefix and names that collide once
   a narrow terminal substitutes '?' for non-ASCII still read apart; 0 Hz says "frequency unknown". */
static void
test_lcn_list_and_labels(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    seed_rows(state, 5);
    *dsd_state_trunk_lcn_slot(state, 3) = 0L; /* a placeholder */
    static const char k_long[] = "Fire Dispatch County Wide Simulcast Tactical Channel Repeater Nord";
    assert(dsd_state_trunk_lcn_name_set(state, 1U, k_long) == 0);
    assert(dsd_state_trunk_lcn_name_set(state, 4U, k_long) == 0);
    assert(dsd_state_trunk_lcn_name_set(state, 3U, "\xc3\xa9t\xc3\xa9") == 0);
    assert(dsd_state_trunk_lcn_avoid_set(state, 4U, 1) == 0 && dsd_state_trunk_lcn_avoid_set(state, 1U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(state, 3U, 1) == 0 && dsd_state_trunk_lcn_avoid_set(state, 0U, 1) == 0);

    dsd_app_scan_avoid_list list;
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0);
    assert(list.count == 4U && list.summary.total == 4U && list.summary.kind == DSD_APP_SCAN_AVOID_KIND_LCN);
    static const int k_rows[] = {0, 1, 3, 4};
    for (size_t i = 0; i < 4U; i++) {
        const dsd_app_scan_avoid_remove_payload* id = &list.entries[i].identity;
        assert(id->kind == DSD_APP_SCAN_AVOID_KIND_LCN && id->row == k_rows[i]);
        assert(id->instance == state->state_instance && id->list_rev == state->lcn_list_rev);
        assert(id->session == 0U && id->target_id[0] == '\0' && id->pad == 0U);
        assert(id->frequency_hz == (uint64_t)*dsd_state_trunk_lcn_slot_const(state, id->row));
        assert(dsd_app_scan_avoid_lcn_match(state, id) == DSD_APP_SCAN_AVOID_MATCH_AVOIDED);
    }
    /* The name stored is cut to 63 bytes; the labels still differ by their leading row. */
    char want[DSD_APP_SCAN_AVOID_LABEL_SIZE];
    expect_label("unnamed row", list.entries[0].label, "Row 1  851.012500 MHz");
    DSD_SNPRINTF(want, sizeof want, "Row 2  851.025000 MHz  %.63s", k_long);
    expect_label("long name", list.entries[1].label, want);
    expect_label("placeholder", list.entries[2].label, "Row 4  frequency unknown  \xc3\xa9t\xc3\xa9");
    DSD_SNPRINTF(want, sizeof want, "Row 5  851.062500 MHz  %.63s", k_long);
    expect_label("duplicate long name", list.entries[3].label, want);
    assert(strcmp(list.entries[1].label, list.entries[3].label) != 0);
    dsd_app_scan_avoid_list_free(&list);
    assert(list.entries == NULL && list.count == 0U);

    /* The raw collection holds the same identities and the stored text, unformatted; its label is the list's. */
    dsd_app_scan_avoid_raw raw;
    assert(dsd_app_scan_avoid_raw_collect(opts, state, &raw) == 0 && raw.count == 4U);
    assert(raw.entries[2].identity.row == 3 && raw.entries[2].identity.frequency_hz == 0U);
    assert(strcmp(raw.entries[2].name, "\xc3\xa9t\xc3\xa9") == 0 && raw.entries[2].ordinal == 0U);
    char label[DSD_APP_SCAN_AVOID_LABEL_SIZE];
    assert(dsd_app_scan_avoid_raw_label(&raw.entries[0], label, sizeof label) == 0);
    expect_label("raw label", label, "Row 1  851.012500 MHz");
    dsd_app_scan_avoid_raw_free(&raw);

    /* No avoids: an empty list, no allocation. */
    assert(dsd_state_trunk_lcn_avoid_clear(state) == 4);
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0 && list.count == 0U && list.entries == NULL);
    assert(list.summary.available == 1U && list.summary.total == 0U);
}

static void
test_trunk_scan_list_and_labels(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    opts->trunk_scan_enabled = 1;
    state->trunk_scan_session = 4242U;
    state->trunk_scan_avoid_rev = dsd_state_rev_next();
    /* Ids with a 40-byte common prefix, and labels that both read "?????" once a narrow terminal replaces the non-ASCII
       characters. */
    dsd_trunk_scan_avoid_entry e[3];
    DSD_MEMSET(e, 0, sizeof e);
    e[0].ordinal = 1U;
    e[0].frequency_hz = 851012500U;
    DSD_SNPRINTF(e[0].id, sizeof e[0].id, "%s", "county-regional-interop-simulcast-site-A-north");
    DSD_SNPRINTF(e[0].label, sizeof e[0].label, "%s", "\xc3\xa9\xc3\xa9");
    e[1].ordinal = 7U;
    e[1].frequency_hz = 5000000000ULL; /* past the uint32_t MHz text */
    DSD_SNPRINTF(e[1].id, sizeof e[1].id, "%s", "county-regional-interop-simulcast-site-A-south");
    DSD_SNPRINTF(e[1].label, sizeof e[1].label, "%s", "\xc3\xbc\xc3\xbc");
    e[2].ordinal = 9U;
    DSD_SNPRINTF(e[2].id, sizeof e[2].id, "%s", "nofreq");
    assert(dsd_state_trunk_scan_avoids_set(state, e, 3U) == 0);

    dsd_app_scan_avoid_list list;
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0 && list.count == 3U);
    for (size_t i = 0; i < 3U; i++) {
        const dsd_app_scan_avoid_remove_payload* id = &list.entries[i].identity;
        assert(id->kind == DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN && id->session == 4242U && id->row == -1);
        assert(strcmp(id->target_id, e[i].id) == 0 && id->frequency_hz == e[i].frequency_hz);
        assert(id->instance == 0U && id->list_rev == 0U);
    }
    expect_label("target with label", list.entries[0].label,
                 "#1  county-regional-interop-simulcast-site-A-north  851.012500 MHz  \xc3\xa9\xc3\xa9");
    expect_label("target past 4.29 GHz", list.entries[1].label,
                 "#7  county-regional-interop-simulcast-site-A-south  5000.000000 MHz  \xc3\xbc\xc3\xbc");
    expect_label("target without frequency or label", list.entries[2].label, "#9  nofreq  frequency unknown");
    dsd_app_scan_avoid_list_free(&list);

    dsd_app_scan_avoid_raw raw;
    assert(dsd_app_scan_avoid_raw_collect(opts, state, &raw) == 0 && raw.count == 3U);
    assert(raw.entries[1].ordinal == 7U && strcmp(raw.entries[1].name, "\xc3\xbc\xc3\xbc") == 0);
    dsd_app_scan_avoid_raw_free(&raw);

    /* An unavailable list lists nothing, whatever the store still holds. */
    state->trunk_scan_avoids_ok = 0U;
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0 && list.count == 0U && list.entries == NULL);
}

/* The key moves with each membership change, also one that keeps the count (put one back, avoid another), with the
   instance (a restart) and with availability; a list built under one key is then rebuilt. */
static void
test_key(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    seed_rows(state, 4);
    opts->scanner_mode = 1;
    assert(dsd_state_trunk_lcn_avoid_set(state, 1U, 1) == 0);
    dsd_app_scan_avoid_summary a;
    dsd_app_scan_avoid_summary b;
    assert(dsd_app_scan_avoid_summary_get(opts, state, &a) == 0);
    assert(dsd_app_scan_avoid_summary_get(opts, state, &b) == 0 && dsd_app_scan_avoid_summary_same_key(&a, &b));
    /* A frame-to-frame change that is not the list's (the rotation pausing) keeps the key. */
    opts->scanner_mode = 0;
    state->lcn_scan_paused = 1U;
    assert(dsd_app_scan_avoid_summary_get(opts, state, &b) == 0 && dsd_app_scan_avoid_summary_same_key(&a, &b));

    assert(dsd_state_trunk_lcn_avoid_set(state, 1U, 0) == 0 && dsd_state_trunk_lcn_avoid_set(state, 2U, 1) == 0);
    assert(dsd_app_scan_avoid_summary_get(opts, state, &b) == 0);
    assert(b.total == a.total && !dsd_app_scan_avoid_summary_same_key(&a, &b));
    dsd_app_scan_avoid_list list;
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0 && list.count == 1U);
    assert(list.entries[0].identity.row == 2 && dsd_app_scan_avoid_summary_same_key(&list.summary, &b));
    dsd_app_scan_avoid_list_free(&list);

    a = b;
    state->scan_avoids_complete = 0U;
    assert(dsd_app_scan_avoid_summary_get(opts, state, &b) == 0 && !dsd_app_scan_avoid_summary_same_key(&a, &b));
    state->scan_avoids_complete = 1U;
    state->state_instance = dsd_state_rev_next();
    assert(dsd_app_scan_avoid_summary_get(opts, state, &b) == 0 && !dsd_app_scan_avoid_summary_same_key(&a, &b));
}

/* More avoided rows than the saturating lcn_avoid_count holds (issue #672 B6): the total and the list are exact. */
static void
test_more_than_65535_avoids(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);

    enum { ROWS = 70000, AVOIDED = 65537 };

    seed_rows(state, ROWS);
    assert(dsd_state_trunk_lcn_avoid_reserve(state, ROWS) == 0);
    for (int i = 0; i < AVOIDED - 1; i++) {
        state->trunk_lcn_avoid[i] = 1U;
    }
    assert(dsd_state_trunk_lcn_avoid_set(state, (size_t)(ROWS - 1), 1) == 0); /* recounts */
    assert(state->lcn_avoid_count == UINT16_MAX && state->lcn_avoid_total == (size_t)AVOIDED);
    expect_summary("65,537 avoided", opts, state, DSD_APP_SCAN_AVOID_KIND_LCN, 1, (size_t)AVOIDED, 0, 1);
    dsd_app_scan_avoid_list list;
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0 && list.count == (size_t)AVOIDED);
    assert(list.entries[AVOIDED - 2].identity.row == AVOIDED - 2);
    assert(list.entries[AVOIDED - 1].identity.row == ROWS - 1);
    expect_label("last of 65,537", list.entries[AVOIDED - 1].label, "Row 70000  1726.000000 MHz");
    dsd_app_scan_avoid_list_free(&list);
}

/* A collected list reports as its total the entries it holds. lcn_avoid_total counts the rows within lcn_freq_count
   as of the last recount; a writer that shrinks the list without one leaves an avoided row past the end, which the
   collection rightly skips. A page or list total that still counted it would promise an entry no page reaches. */
static void
test_collected_total_matches_entries(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    seed_rows(state, 8);
    assert(dsd_state_trunk_lcn_avoid_set(state, 2U, 1) == 0);
    assert(dsd_state_trunk_lcn_avoid_set(state, 5U, 1) == 0);
    state->lcn_freq_count = 4; /* no recount: lcn_avoid_total still says 2 */
    assert(state->lcn_avoid_total == 2U);
    dsd_app_scan_avoid_raw raw;
    assert(dsd_app_scan_avoid_raw_collect(opts, state, &raw) == 0);
    assert(raw.count == 1U && raw.entries[0].identity.row == 2);
    if (raw.summary.total != raw.count || !raw.summary.total_known) {
        DSD_FPRINTF(stderr, "collected %zu entries under total %zu\n", raw.count, raw.summary.total);
        assert(0);
    }
    dsd_app_scan_avoid_raw_free(&raw);
    dsd_app_scan_avoid_list list;
    assert(dsd_app_scan_avoid_list_build(opts, state, &list) == 0);
    assert(list.count == 1U && list.summary.total == 1U);
    dsd_app_scan_avoid_list_free(&list);

    /* The trunk-scan list holds what it counts; its total is the same either way. */
    reset(opts, state);
    opts->trunk_scan_enabled = 1;
    static const dsd_trunk_scan_avoid_entry k_targets[] = {
        {1U, 0U, 851012500U, "a", ""},
        {2U, 0U, 852012500U, "b", ""},
    };
    assert(dsd_state_trunk_scan_avoids_set(state, k_targets, 2U) == 0);
    assert(dsd_app_scan_avoid_raw_collect(opts, state, &raw) == 0);
    assert(raw.count == 2U && raw.summary.total == 2U);
    dsd_app_scan_avoid_raw_free(&raw);
}

/* The decoder's LCN identity check: every part must match, the row is range-checked first, and a matching row that is
   no longer avoided says so. */
static void
test_lcn_match(dsd_opts* opts, dsd_state* state) {
    reset(opts, state);
    seed_rows(state, 30); /* rows past the embedded slots live in the heap tail */
    assert(dsd_state_trunk_lcn_avoid_set(state, 28U, 1) == 0);
    dsd_app_scan_avoid_remove_payload id;
    DSD_MEMSET(&id, 0, sizeof id);
    id.kind = DSD_APP_SCAN_AVOID_KIND_LCN;
    id.instance = state->state_instance;
    id.list_rev = state->lcn_list_rev;
    id.row = 28;
    id.frequency_hz = dsd_app_scan_avoid_lcn_row_hz(state, 28);
    assert(id.frequency_hz == 851362500U);
    assert(dsd_app_scan_avoid_lcn_match(state, &id) == DSD_APP_SCAN_AVOID_MATCH_AVOIDED);

    dsd_app_scan_avoid_remove_payload bad = id;
    bad.instance++;
    assert(dsd_app_scan_avoid_lcn_match(state, &bad) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    bad = id;
    bad.list_rev++;
    assert(dsd_app_scan_avoid_lcn_match(state, &bad) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    bad = id;
    bad.frequency_hz++;
    assert(dsd_app_scan_avoid_lcn_match(state, &bad) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    bad = id;
    bad.row = 30;
    assert(dsd_app_scan_avoid_lcn_match(state, &bad) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    bad.row = -1;
    assert(dsd_app_scan_avoid_lcn_match(state, &bad) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    bad.row = INT32_MIN;
    assert(dsd_app_scan_avoid_lcn_match(state, &bad) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    bad = id;
    bad.kind = DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN;
    assert(dsd_app_scan_avoid_lcn_match(state, &bad) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    assert(dsd_app_scan_avoid_lcn_match(NULL, &id) == DSD_APP_SCAN_AVOID_MATCH_STALE);
    assert(dsd_app_scan_avoid_lcn_match(state, NULL) == DSD_APP_SCAN_AVOID_MATCH_STALE);

    assert(dsd_state_trunk_lcn_avoid_set(state, 28U, 0) == 0);
    assert(dsd_app_scan_avoid_lcn_match(state, &id) == DSD_APP_SCAN_AVOID_MATCH_NOT_AVOIDED);

    /* A tail the state does not hold (a snapshot that copied less) reads 0 Hz rather than past the buffer. */
    const size_t capacity = state->trunk_lcn_freq_ext_capacity;
    state->trunk_lcn_freq_ext_capacity = 2U;
    assert(dsd_app_scan_avoid_lcn_row_hz(state, 28) == 0U);
    state->trunk_lcn_freq_ext_capacity = capacity;
    assert(dsd_app_scan_avoid_lcn_row_hz(state, 30) == 0U && dsd_app_scan_avoid_lcn_row_hz(NULL, 0) == 0U);
}

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1, sizeof *state);
    assert(opts != NULL && state != NULL);
    test_null_arguments(opts, state);
    test_kind_and_offers(opts, state);
    test_unavailable_keeps_manage(opts, state);
    test_lcn_list_and_labels(opts, state);
    test_trunk_scan_list_and_labels(opts, state);
    test_key(opts, state);
    test_more_than_65535_avoids(opts, state);
    test_lcn_match(opts, state);
    test_collected_total_matches_entries(opts, state);
    dsd_state_trunk_lcn_free(state);
    dsd_state_trunk_scan_avoids_free(state);
    free(state);
    free(opts);
    printf("APP_CONTROL_SCAN_AVOID_VIEW: OK\n");
    return 0;
}
