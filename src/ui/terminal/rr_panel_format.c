// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Pure text formatters for the RadioReference import panel.
 *
 * No curses and no wizard-core dependency: every function here maps runtime RR
 * structs onto one display string, so the panel's text can be unit-tested
 * without a terminal.
 */

#include "rr_panel_format.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/radioreference.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <dsd-neo/runtime/radioreference_import.h>
#include <stdlib.h>

/**
 * @brief Resolve the row's trailing tag and the frequency it advertises.
 *
 * Split out of rr_panel_site_row_format() purely to keep that function inside
 * tools/lizard.sh's CCN ceiling of 15; the two together are byte-for-byte the
 * single function this replaced.
 *
 * @return The frequency the row should print, in Hz; 0 when the site lists none.
 */
static long long
rr_panel_site_row_suffix(const dsd_rr_site* site, int trunked, char* suffix, size_t suffix_sz) {
    suffix[0] = '\0';
    if (trunked) {
        const long long control_hz = dsd_rr_site_control_freq_hz(site);
        if (control_hz != 0) {
            (void)DSD_SNPRINTF(suffix, suffix_sz, "  CTL");
            return control_hz;
        }
        (void)DSD_SNPRINTF(suffix, suffix_sz, "  (no control)");
        return dsd_rr_site_first_freq_hz(site);
    }
    if (site->freqs != NULL && site->freq_count > 0 && site->freqs[0].color_code[0] != '\0') {
        (void)DSD_SNPRINTF(suffix, suffix_sz, "  cc:%.4s", site->freqs[0].color_code);
    }
    return dsd_rr_site_first_freq_hz(site);
}

/** @brief Radio mark for a trunked (one-of) list, checkbox for a conventional (many-of) one. */
static const char*
rr_panel_site_row_mark(int trunked, int selected) {
    if (trunked) {
        return selected ? "(*)" : "( )";
    }
    return selected ? "[x]" : "[ ]";
}

int
rr_panel_site_row_format(const dsd_rr_site* site, int trunked, int selected, int cursor, char* out, size_t out_sz) {
    if (site == NULL || out == NULL || out_sz == 0) {
        return -1;
    }
    const char* mark = rr_panel_site_row_mark(trunked, selected);
    char suffix[24];
    const long long hz = rr_panel_site_row_suffix(site, trunked, suffix, sizeof suffix);
    char mhz[32];
    mhz[0] = '\0';
    (void)dsd_rr_hz_to_mhz_text(hz, mhz, sizeof mhz);
    const int n = DSD_SNPRINTF(out, out_sz, "%s%s %6d %-24.24s %12s MHz%s", cursor ? "> " : "  ", mark,
                               site->site_number, site->descr, (mhz[0] != '\0') ? mhz : "-", suffix);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

// Cppcheck 2.21 loses the final prototype name after a callback typedef parameter.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
void
rr_panel_counter_state(const dsd_rr_site* sites, size_t site_count, rr_panel_site_selected_fn is_selected,
                       const void* user, RrPanelCounter* out) {
    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    size_t count = 0;
    if (sites != NULL && is_selected != NULL && site_count > 0U) {
        // Runs on the panel redraw path, so nothing is allocated for the empty
        // list the screen shows before a system is loaded.
        long long* chosen = (long long*)calloc(site_count, sizeof *chosen);
        if (chosen == NULL) {
            (void)DSD_SNPRINTF(out->text, sizeof out->text, "[ selection unavailable ]");
            (void)DSD_SNPRINTF(out->short_text, sizeof out->short_text, "[ ? ]");
            return;
        }
        for (size_t i = 0; i < site_count; i++) {
            if (!is_selected(user, i)) {
                continue;
            }
            /* Mirrors rr_chan_conventional (src/runtime/radioreference/rr_generate.c): empty
             * first, then duplicates - the kept count is DISTINCT frequencies, with no cap. */
            const long long hz = dsd_rr_site_first_freq_hz(&sites[i]);
            if (hz == 0) {
                out->empty++;
                continue;
            }
            int duplicate = 0;
            for (size_t k = 0; k < count; k++) {
                if (chosen[k] == hz) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                out->duplicates++;
                continue;
            }
            chosen[count] = hz;
            count++;
        }
        free(chosen);
    }
    out->kept = (int)count;
    (void)DSD_SNPRINTF(out->text, sizeof out->text, "[ %d distinct frequencies ]", out->kept);
    /* The heading right-aligns the counter beside a ~38-column title; the full label needs a
     * 66-column body before it fits, so narrower terminals get this instead of nothing. */
    (void)DSD_SNPRINTF(out->short_text, sizeof out->short_text, "[ %d freqs ]", out->kept);
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed

int
rr_panel_plan_line(const dsd_rr_import_plan* plan, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';
    if (plan == NULL) {
        (void)DSD_SNPRINTF(out, out_sz, "Plan: nothing selected yet");
        return 0;
    }
    if (plan->awaiting_selection) {
        /* A question, not a refusal - and the reason is already an instruction,
           so it stands on its own without a prefix or the blocked styling. This
           is what the row reads immediately after a successful import, which
           releases its selection and rewords the instruction for that moment
           ("Select another site to import, or Esc to finish."). */
        (void)DSD_SNPRINTF(out, out_sz, "%s", plan->blocked_reason);
        return 0;
    }
    if (plan->blocked_reason[0] != '\0') {
        (void)DSD_SNPRINTF(out, out_sz, "Blocked: %s", plan->blocked_reason);
        return 1;
    }
    const char* files = "no files";
    if (plan->group_csv_text != NULL && plan->chan_csv_text != NULL) {
        files = "group.csv + chan.csv";
    } else if (plan->group_csv_text != NULL) {
        files = "group.csv";
    } else if (plan->chan_csv_text != NULL) {
        files = "chan.csv";
    }
    (void)DSD_SNPRINTF(
        out, out_sz, "Plan: %s | %s | %s | %s MHz", (plan->decode_flag[0] != '\0') ? plan->decode_flag : "-",
        plan->trunking ? "trunked" : "conventional", files, (plan->freq_mhz[0] != '\0') ? plan->freq_mhz : "-");
    return 0;
}
