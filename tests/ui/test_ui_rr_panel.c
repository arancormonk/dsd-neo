// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for the RadioReference import panel's pure formatters.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_import.h>

#include "rr_panel_format.h"

static int
select_all(const void* user, size_t index) {
    (void)user;
    (void)index;
    return 1;
}

static void
make_site(dsd_rr_site* site, dsd_rr_site_freq* freqs, size_t freq_count, int site_number, const char* descr) {
    DSD_MEMSET(site, 0, sizeof *site);
    site->site_number = site_number;
    DSD_SNPRINTF(site->descr, sizeof site->descr, "%s", descr);
    site->freqs = freqs;
    site->freq_count = freq_count;
}

static void
test_site_row_trunked_with_control(void) {
    dsd_rr_site_freq freq;
    DSD_MEMSET(&freq, 0, sizeof freq);
    freq.freq_hz = 851050000LL;
    freq.is_control = 1;
    dsd_rr_site site;
    make_site(&site, &freq, 1, 1, "Johnson Co Simulcast");

    char row[160];
    assert(rr_panel_site_row_format(&site, 1, 1, 0, row, sizeof row) == 0);
    assert(strcmp(row, "  (*)      1 Johnson Co Simulcast           851.05 MHz  CTL") == 0);
}

static void
test_site_row_trunked_without_control(void) {
    dsd_rr_site_freq freq;
    DSD_MEMSET(&freq, 0, sizeof freq);
    freq.freq_hz = 852400000LL;
    dsd_rr_site site;
    make_site(&site, &freq, 1, 7, "No Control Site");

    char row[160];
    assert(rr_panel_site_row_format(&site, 1, 1, 0, row, sizeof row) == 0);
    assert(strcmp(row, "  (*)      7 No Control Site                 852.4 MHz  (no control)") == 0);
}

static void
test_site_row_conventional_with_color_code(void) {
    dsd_rr_site_freq freq;
    DSD_MEMSET(&freq, 0, sizeof freq);
    freq.freq_hz = 146755000LL;
    DSD_SNPRINTF(freq.color_code, sizeof freq.color_code, "%s", "1");
    dsd_rr_site site;
    make_site(&site, &freq, 1, 310011, "Waukee");

    char row[160];
    assert(rr_panel_site_row_format(&site, 0, 1, 1, row, sizeof row) == 0);
    assert(strcmp(row, "> [x] 310011 Waukee                        146.755 MHz  cc:1") == 0);

    assert(rr_panel_site_row_format(&site, 0, 0, 0, row, sizeof row) == 0);
    assert(strcmp(row, "  [ ] 310011 Waukee                        146.755 MHz  cc:1") == 0);

    /* No colour code -> no suffix at all. */
    freq.color_code[0] = '\0';
    assert(rr_panel_site_row_format(&site, 0, 0, 0, row, sizeof row) == 0);
    assert(strcmp(row, "  [ ] 310011 Waukee                        146.755 MHz") == 0);
}

static void
test_counter_counts_distinct_frequencies(void) {
    dsd_rr_site_freq freqs[3];
    dsd_rr_site sites[3];
    const long long hz[3] = {146755000LL, 444525000LL, 443125000LL};
    for (size_t i = 0; i < 3; i++) {
        DSD_MEMSET(&freqs[i], 0, sizeof freqs[i]);
        freqs[i].freq_hz = hz[i];
        make_site(&sites[i], &freqs[i], 1, (int)i + 1, "site");
    }
    RrPanelCounter counter;
    rr_panel_counter_state(sites, 3, select_all, NULL, &counter);
    assert(counter.kept == 3);
    assert(counter.empty == 0);
    assert(counter.duplicates == 0);
    assert(strcmp(counter.text, "[ 3 distinct frequencies ]") == 0);
    /* The heading falls back to this when the body is too narrow for the full label. */
    assert(strcmp(counter.short_text, "[ 3 freqs ]") == 0);
    assert(strlen(counter.short_text) < strlen(counter.text));
}

static void
test_counter_keeps_every_distinct_frequency(void) {
    /* 27 selected repeaters with 27 DISTINCT frequencies: the scan list is
     * unbounded, so all 27 are kept. */
    dsd_rr_site_freq freqs[27];
    dsd_rr_site sites[27];
    for (size_t i = 0; i < 27; i++) {
        DSD_MEMSET(&freqs[i], 0, sizeof freqs[i]);
        freqs[i].freq_hz = 400000000LL + ((long long)i * 25000LL);
        make_site(&sites[i], &freqs[i], 1, (int)i + 1, "repeater");
    }
    RrPanelCounter counter;
    rr_panel_counter_state(sites, 27, select_all, NULL, &counter);
    assert(counter.kept == 27);
    assert(strcmp(counter.text, "[ 27 distinct frequencies ]") == 0);
    assert(strcmp(counter.short_text, "[ 27 freqs ]") == 0);
}

static void
test_counter_skips_empty_and_duplicates(void) {
    dsd_rr_site_freq freqs[3];
    dsd_rr_site sites[4];
    const long long hz[3] = {146755000LL, 146755000LL, 443125000LL};
    for (size_t i = 0; i < 3; i++) {
        DSD_MEMSET(&freqs[i], 0, sizeof freqs[i]);
        freqs[i].freq_hz = hz[i];
        make_site(&sites[i], &freqs[i], 1, (int)i + 1, "repeater");
    }
    make_site(&sites[3], NULL, 0, 4, "no frequency");

    RrPanelCounter counter;
    rr_panel_counter_state(sites, 4, select_all, NULL, &counter);
    assert(counter.kept == 2);
    assert(counter.duplicates == 1);
    assert(counter.empty == 1);
    assert(strcmp(counter.text, "[ 2 distinct frequencies ]") == 0);
    assert(strcmp(counter.short_text, "[ 2 freqs ]") == 0);
}

static void
test_plan_line_normal(void) {
    char group_text[] = "1,ONE,D\n";
    char chan_text[] = "1,146.755\n";
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof plan);
    plan.ok = 1;
    plan.trunking = 0;
    plan.group_csv_text = group_text;
    plan.chan_csv_text = chan_text;
    DSD_SNPRINTF(plan.decode_flag, sizeof plan.decode_flag, "%s", "-fs -Y");
    DSD_SNPRINTF(plan.freq_mhz, sizeof plan.freq_mhz, "%s", "146.755");

    char line[320];
    assert(rr_panel_plan_line(&plan, line, sizeof line) == 0);
    assert(strcmp(line, "Plan: -fs -Y | conventional | group.csv + chan.csv | 146.755 MHz") == 0);

    /* A single conventional repeater yields no channel map at all. */
    plan.chan_csv_text = NULL;
    assert(rr_panel_plan_line(&plan, line, sizeof line) == 0);
    assert(strcmp(line, "Plan: -fs -Y | conventional | group.csv | 146.755 MHz") == 0);
}

static void
test_plan_line_blocked(void) {
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof plan);
    DSD_SNPRINTF(plan.blocked_reason, sizeof plan.blocked_reason, "%s",
                 "This site lists no frequency to start on, so the session would have nothing to tune.");

    char line[320];
    assert(rr_panel_plan_line(&plan, line, sizeof line) == 1);
    assert(strncmp(line, "Blocked: This site lists no frequency", 37) == 0);
}

/*
 * An unmade choice is not a refusal. The panel paints a blocked plan red and
 * bold, and "Blocked: Select a site." lands under the status line the moment an
 * import succeeds and releases its selection - reading as a verdict on the
 * import that just worked. A plan waiting for a site says so plainly instead.
 */
static void
test_plan_line_awaiting_a_selection(void) {
    dsd_rr_import_plan plan;
    DSD_MEMSET(&plan, 0, sizeof plan);
    plan.awaiting_selection = 1;
    DSD_SNPRINTF(plan.blocked_reason, sizeof plan.blocked_reason, "%s", "Select a site.");

    char line[320];
    assert(rr_panel_plan_line(&plan, line, sizeof line) == 0);
    assert(strcmp(line, "Select a site.") == 0);

    DSD_SNPRINTF(plan.blocked_reason, sizeof plan.blocked_reason, "%s", "Select at least one repeater.");
    assert(rr_panel_plan_line(&plan, line, sizeof line) == 0);
    assert(strcmp(line, "Select at least one repeater.") == 0);
}

int
main(void) {
    test_site_row_trunked_with_control();
    test_site_row_trunked_without_control();
    test_site_row_conventional_with_color_code();
    test_counter_counts_distinct_frequencies();
    test_counter_keeps_every_distinct_frequency();
    test_counter_skips_empty_and_duplicates();
    test_plan_line_normal();
    test_plan_line_blocked();
    test_plan_line_awaiting_a_selection();
    printf("UI_RR_PANEL: OK\n");
    return 0;
}
