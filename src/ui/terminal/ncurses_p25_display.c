// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_p25_display.c
 * P25 protocol display functions for ncurses UI
 */

#include <curses.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/ui/ncurses_internal.h>
#include <dsd-neo/ui/ncurses_p25_display.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef USE_RTLSDR
#endif

static int
ui_is_p25_synctype(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype) || DSD_SYNC_IS_P25P2(synctype);
}

static int
ui_iden_entry_is_usable(const p25_iden_entry_t* entry) {
    return entry && entry->populated && entry->base_freq != 0 && entry->chan_spac != 0;
}

static long int
ui_iden_entry_calc_freq(const p25_iden_entry_t* entry, int step) {
    return (entry->base_freq * 5) + ((long)step * entry->chan_spac * 125);
}

static int
ui_fdma_denom(const dsd_state* state, int iden) {
    if (state->p25_cc_is_tdma == 1 && !(state->p25_chan_tdma_explicit[iden] & 0x01)) {
        return 2;
    }
    return 1;
}

static int
ui_tdma_denom(const p25_iden_entry_t* tdma) {
    int denom = p25_channel_type_slots_per_carrier(tdma->chan_type);
    return (denom > 0) ? denom : 1;
}

static int
ui_iden_entry_matches(const p25_iden_entry_t* entry, int raw, int denom, long int freq) {
    if (!ui_iden_entry_is_usable(entry)) {
        return 0;
    }
    if (denom <= 0) {
        denom = 1;
    }
    return ui_iden_entry_calc_freq(entry, raw / denom) == freq;
}

static int
ui_screen_cols_default80(void) {
    int rows = 0;
    int cols = 80;
    getmaxyx(stdscr, rows, cols);
    if (cols < 1 || rows < 1) {
        cols = 80;
    }
    return cols;
}

static void
ui_emit_wrapped_item(const char* buf, int len, int cols, int* line_used) {
    int sep = (*line_used == 0) ? 0 : 4;
    int left_border = (*line_used == 0) ? 2 : 0;
    if ((left_border + *line_used + sep + len) > cols) {
        if (*line_used > 0) {
            addch('\n');
        }
        *line_used = 0;
    }
    if (*line_used == 0) {
        ui_print_lborder_green();
        addch(' ');
    } else {
        addstr("    ");
    }
    addnstr(buf, len);
    *line_used += ((*line_used == 0) ? 0 : sep) + len;
}

static const char*
ui_p25_sm_mode_to_str(int mode) {
    switch (mode) {
        case DSD_P25_SM_MODE_ON_CC: return "CC";
        case DSD_P25_SM_MODE_ON_VC: return "VC";
        case DSD_P25_SM_MODE_HANG: return "HANG";
        case DSD_P25_SM_MODE_HUNTING: return "HUNT";
        case DSD_P25_SM_MODE_ARMED: return "ARM";
        case DSD_P25_SM_MODE_FOLLOW: return "FOL";
        case DSD_P25_SM_MODE_RETURNING: return "RET";
        default: return "?";
    }
}

static void
ui_format_freq_mhz_or_dash(long freq, char* out, size_t out_len) {
    if (freq != 0) {
        DSD_SNPRINTF(out, out_len, "%.6lf MHz", (double)freq / 1000000.0);
    } else {
        DSD_SNPRINTF(out, out_len, "-");
    }
}

static int
ui_p25_iden_confirmed_count(const dsd_state* state, int* out_total, int* out_confirmed) {
    int total = 0;
    int confirmed = 0;
    for (int i = 0; i < 16; i++) {
        if (state->p25_iden_fdma[i].populated) {
            total++;
            if (state->p25_iden_fdma[i].trust >= 2) {
                confirmed++;
            }
        }
        if (state->p25_iden_tdma[i].populated) {
            total++;
            if (state->p25_iden_tdma[i].trust >= 2) {
                confirmed++;
            }
        }
    }
    if (out_total) {
        *out_total = total;
    }
    if (out_confirmed) {
        *out_confirmed = confirmed;
    }
    return total > 0;
}

static char
ui_sm_tag_symbol(const char* tag) {
    if (strstr(tag, "after-tune") != NULL) {
        return 'V';
    }
    if (strstr(tag, "after-release") != NULL) {
        return 'R';
    }
    if (strstr(tag, "release-") != NULL) {
        return 'H';
    }
    if (strstr(tag, "after-neigh") != NULL) {
        return 'N';
    }
    if (strstr(tag, "tick") != NULL) {
        return 'T';
    }
    return '?';
}

static double
ui_time_diff_maybe_monotonic(time_t now, time_t wall_ts, double mono_now, double mono_ts) {
    if (mono_ts > 0.0) {
        return mono_now - mono_ts;
    }
    if (wall_ts != 0) {
        return (double)(now - wall_ts);
    }
    return -1.0;
}

static int
ui_p25_iden_entry_has_data(const p25_iden_entry_t* entry) {
    return entry->populated && (entry->base_freq != 0 || entry->chan_spac != 0);
}

static const char*
ui_p25_iden_trust_str(int trust) {
    if (trust >= 2) {
        return "ok";
    }
    if (trust == 1) {
        return "prov";
    }
    return "-";
}

static void
ui_p25_print_iden_line(int id, const p25_iden_entry_t* entry, int has_other_class, const char* mode_base) {
    double base_mhz = (double)(entry->base_freq * 5) / 1000000.0;
    double spac_mhz = (double)(entry->chan_spac * 125) / 1000000.0;
    const char* trust_str = ui_p25_iden_trust_str(entry->trust);
    const char* mode_tag = has_other_class ? ((mode_base[0] == 'F') ? "FDMA[F/T]" : "TDMA[F/T]") : mode_base;
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    attr_get(&saved_attrs, &saved_pair, NULL);
    attron(COLOR_PAIR(ui_iden_color_pair(id)));
    ui_print_lborder_green();
    addch(' ');
    printw("IDEN %d: %s type:%d base:%.6lfMHz spac:%.6lfMHz off:%d trust:%s", id, mode_tag, entry->chan_type & 0xF,
           base_mhz, spac_mhz, entry->trans_off, trust_str);
    if (entry->bw_vu != 0U) {
        int bw_hz = p25_iden_vu_bandwidth_hz(entry->bw_vu);
        if (bw_hz > 0) {
            printw(" bw:%dHz", bw_hz);
        } else {
            printw(" bw:?raw:%X", entry->bw_vu & 0xF);
        }
    }
    if (entry->wacn || entry->sysid) {
        printw(" W:%05llX S:%03llX", entry->wacn, entry->sysid);
    }
    if (entry->rfss || entry->site) {
        printw(" R:%llu I:%llu", entry->rfss, entry->site);
    }
    addch('\n');
    attr_set(saved_attrs, saved_pair, NULL);
}

static int
ui_extract_channel_token(const char* channel_str, char* tok, size_t tok_len) {
    const char* p = strstr(channel_str, "Ch:");
    if (!p || tok_len == 0) {
        return 0;
    }
    p += 3;
    while (*p == ' ') {
        p++;
    }
    size_t t = 0;
    while (*p && t + 1 < tok_len) {
        char c = *p;
        int is_hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!is_hex) {
            break;
        }
        tok[t++] = c;
        p++;
    }
    tok[t] = '\0';
    return t > 0;
}

static long int
ui_lookup_trunk_chan_map(const dsd_state* state, const char* tok) {
    char* endp = NULL;
    long ch_hex = strtol(tok, &endp, 16);
    if (endp && *endp == '\0' && ch_hex > 0 && ch_hex < 65535) {
        long int freq = state->trunk_chan_map[ch_hex];
        if (freq != 0) {
            return freq;
        }
    }
    long ch_dec = strtol(tok, &endp, 10);
    if (endp && *endp == '\0' && ch_dec > 0 && ch_dec < 65535) {
        return state->trunk_chan_map[ch_dec];
    }
    return 0;
}

static int
ui_collect_neighbor_indices(const dsd_state* state, int idxs[], int max_idxs) {
    int n = 0;
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX && n < max_idxs; i++) {
        if (state->p25_nb_entries[i].freq != 0) {
            idxs[n++] = i;
        }
    }
    return n;
}

static void
ui_sort_neighbors_by_last_seen(const dsd_state* state, int idxs[], int n) {
    for (int i = 0; i < n; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (state->p25_nb_entries[idxs[j]].last_seen > state->p25_nb_entries[idxs[best]].last_seen) {
                best = j;
            }
        }
        if (best != i) {
            int tmp = idxs[i];
            idxs[i] = idxs[best];
            idxs[best] = tmp;
        }
    }
}

static int
ui_freq_in_cc_candidates(const dsd_state* state, long freq) {
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    int count = (cc && cc->count > 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX) ? cc->count : 0;
    for (int i = 0; i < count; i++) {
        if (cc->candidates[i] == freq) {
            return 1;
        }
    }
    return 0;
}

static int
ui_format_neighbor_line(const dsd_state* state, int idx, time_t now, char* out, size_t out_len) {
    if (!out || out_len == 0U) {
        return 0;
    }
    const p25_nb_entry_t* entry = &state->p25_nb_entries[idx];
    long freq = entry->freq;
    long age = (long)((entry->last_seen != 0) ? (now - entry->last_seen) : 0);
    int is_cc = (freq == state->p25_cc_freq);
    int in_cands = ui_freq_in_cc_candidates(state, freq);
    if (age < 0) {
        age = 0;
    }
    char wacn[24] = {0};
    char lra[16] = {0};
    char cfva[64] = {0};
    if (entry->wacn_valid) {
        DSD_SNPRINTF(wacn, sizeof wacn, " W:%05X", entry->wacn);
    }
    if (entry->lra_valid) {
        DSD_SNPRINTF(lra, sizeof lra, " LRA:%02X", entry->lra);
    }
    if (entry->cfva_valid) {
        char tags[48] = {0};
        (void)p25_format_adjacent_cfva(entry->cfva, tags, sizeof(tags));
        DSD_SNPRINTF(cfva, sizeof cfva, " CFVA:%s", tags[0] ? tags : "-");
    } else {
        DSD_SNPRINTF(cfva, sizeof cfva, "%s", " CFVA:?");
    }
    int n = DSD_SNPRINTF(out, out_len, "%.6lf MHz%s%s SYS:%03X R:%03u S:%03u%s%s%s age:%lds", (double)freq / 1000000.0,
                         is_cc ? " [CC]" : "", in_cands ? " [C]" : "", entry->sysid, entry->rfss, entry->site, wacn,
                         lra, cfva, age);
    return (n < 0) ? n : (int)strlen(out);
}

static int
ui_format_secondary_cc_line(const dsd_state* state, int idx, time_t now, char* out, size_t out_len) {
    const p25_secondary_cc_entry_t* entry = &state->p25_secondary_cc_entries[idx];
    long age = (long)((entry->last_seen != 0) ? (now - entry->last_seen) : 0);
    int in_cands = ui_freq_in_cc_candidates(state, entry->freq);
    if (age < 0) {
        age = 0;
    }
    return DSD_SNPRINTF(out, out_len, "%.6lf MHz%s CH:%04X R:%03u S:%03u SSC:%02X age:%lds",
                        (double)entry->freq / 1000000.0, in_cands ? " [C]" : "", entry->channel, entry->rfss,
                        entry->site, entry->ssc, age);
}

int
ui_is_iden_channel(const dsd_state* state, int ch16, long int freq) {
    if (!state || ch16 <= 0 || ch16 >= 65535) {
        return 0;
    }
    if (!ui_is_p25_synctype(state->synctype)) {
        return 0;
    }
    int iden = (ch16 >> 12) & 0xF;
    if (iden < 0 || iden > 15) {
        return 0;
    }
    int raw = ch16 & 0xFFF;
    const p25_iden_entry_t* fdma = &state->p25_iden_fdma[iden];
    if (ui_iden_entry_matches(fdma, raw, ui_fdma_denom(state, iden), freq)) {
        return 1;
    }
    const p25_iden_entry_t* tdma = &state->p25_iden_tdma[iden];
    return ui_iden_entry_matches(tdma, raw, ui_tdma_denom(tdma), freq);
}

int
ui_match_iden_channel(const dsd_state* state, int ch16, long int freq, int* out_iden) {
    if (!state) {
        return 0;
    }
    if (!ui_is_iden_channel(state, ch16, freq)) {
        return 0;
    }
    int iden = (ch16 >> 12) & 0xF;
    if (out_iden) {
        *out_iden = iden;
    }
    return 1;
}

int
compute_p25p1_voice_avg_err(const dsd_state* s, double* out_avg) {
    int len = s->p25_p1_voice_err_hist_len;
    if (len <= 0) {
        return 0;
    }
    double avg = (double)s->p25_p1_voice_err_hist_sum / (double)len;
    if (out_avg) {
        *out_avg = avg;
    }
    return 1;
}

int
compute_p25p2_voice_avg_err(const dsd_state* s, int slot, double* out_avg) {
    if (slot < 0 || slot > 1) {
        return 0;
    }
    int len = s->p25_p2_voice_err_hist_len;
    if (len <= 0) {
        return 0;
    }
    double avg = (double)s->p25_p2_voice_err_hist_sum[slot] / (double)len;
    if (out_avg) {
        *out_avg = avg;
    }
    return 1;
}

static int
ui_print_p25_sync_metric(const dsd_state* state) {
    int cur = ncurses_last_synctype;
    int prev = state->lastsynctype;
    const char* cur_s = dsd_synctype_to_string(cur);
    const char* prev_s = dsd_synctype_to_string(prev);
    printw("| Sync: cur:%s(%d) prev:%s(%d)\n", cur_s, cur, prev_s, prev);
    return 1;
}

static int
ui_print_p1_voice_err_metric(const dsd_state* state) {
    double avg_ber = 0.0;
    if (compute_p25p1_voice_avg_err(state, &avg_ber)) {
        printw("| P1 Voice: ERR [%X][%X] Avg BER:%4.1f%%\n", state->errs & 0xF, state->errs2 & 0xF, avg_ber);
    } else {
        printw("| P1: ERR [%X][%X]\n", state->errs & 0xF, state->errs2 & 0xF);
    }
    return 1;
}

static int
ui_print_p1_cc_fec_metric(const dsd_state* state) {
    unsigned int ok = state->p25_p1_fec_ok;
    unsigned int err = state->p25_p1_fec_err;
    unsigned int total = ok + err;
    if (total == 0) {
        return 0;
    }
    double okpct = (100.0 * (double)ok) / (double)total;
    printw("| P1 CC FEC: %u/%u (ok:%4.1f%%)\n", ok, err, okpct);
    return 1;
}

static int
ui_print_p1_voice_fec_metric(const dsd_state* state, int is_p25p1) {
    if (!is_p25p1) {
        return 0;
    }
    unsigned int ok = state->p25_p1_voice_fec_ok;
    unsigned int err = state->p25_p1_voice_fec_err;
    unsigned int total = ok + err;
    if (total == 0) {
        return 0;
    }
    double okpct = (100.0 * (double)ok) / (double)total;
    printw("| P1 Voice FEC: %u/%u (ok:%4.1f%%)\n", ok, err, okpct);
    return 1;
}

static int
ui_print_p1_header_metric(const dsd_state* state, int is_p25p1) {
    if (!is_p25p1) {
        return 0;
    }
    unsigned int hdr_fix = state->debug_header_errors;
    unsigned int hdr_crit = state->debug_header_critical_errors;
    if (!hdr_fix && !hdr_crit) {
        return 0;
    }
    printw("| P1 Hdr: fixed:%u crit:%u\n", hdr_fix, hdr_crit);
    return 1;
}

static int
ui_print_p1_voice_percentile_metric(const dsd_state* state) {
    int n = state->p25_p1_voice_err_hist_len;
    if (n <= 0) {
        return 0;
    }
    double p50 = 0.0;
    double p95 = 0.0;
    if (!compute_percentiles_u8(state->p25_p1_voice_err_hist, n, &p50, &p95)) {
        return 0;
    }
    printw("| P1 Voice: P50/P95: %4.1f/%4.1f%%\n", p50, p95);
    return 1;
}

static int
ui_print_p25_core_metrics(const dsd_state* state, int is_p25p1, int is_p25p2) {
    if (!is_p25p1 && !is_p25p2) {
        return 0;
    }
    int lines = 0;
    lines += ui_print_p25_sync_metric(state);
    lines += ui_print_p1_voice_err_metric(state);
    lines += ui_print_p1_cc_fec_metric(state);
    lines += ui_print_p1_voice_fec_metric(state, is_p25p1);
    lines += ui_print_p1_header_metric(state, is_p25p1);
    lines += ui_print_p1_voice_percentile_metric(state);
    return lines;
}

static int
ui_print_p2_voice_avg_metric(const dsd_state* state) {
    double avg_l = 0.0;
    double avg_r = 0.0;
    int has_l = compute_p25p2_voice_avg_err(state, 0, &avg_l);
    int has_r = compute_p25p2_voice_avg_err(state, 1, &avg_r);
    if (!has_l && !has_r) {
        return 0;
    }
    if (has_l && has_r) {
        printw("| P2 Voice: Avg BER - S1:%4.1f%%, S2:%4.1f%%\n", avg_l, avg_r);
    } else if (has_l) {
        printw("| P2 Voice: Avg BER - S1:%4.1f%%\n", avg_l);
    } else {
        printw("| P2 Voice: Avg BER - S2:%4.1f%%\n", avg_r);
    }
    return 1;
}

static int
ui_print_p2_voice_percentile_metric(const dsd_state* state) {
    int n = state->p25_p2_voice_err_hist_len;
    if (n <= 0) {
        return 0;
    }
    double l50 = 0.0;
    double l95 = 0.0;
    double r50 = 0.0;
    double r95 = 0.0;
    int have_any = 0;
    if (compute_percentiles_u8(state->p25_p2_voice_err_hist[0], n, &l50, &l95)) {
        have_any = 1;
    }
    if (compute_percentiles_u8(state->p25_p2_voice_err_hist[1], n, &r50, &r95)) {
        have_any = 1;
    }
    if (!have_any) {
        return 0;
    }
    printw("| P2 Voice: P50/P95 - S1:%4.1f/%4.1f%% S2:%4.1f/%4.1f%%\n", l50, l95, r50, r95);
    return 1;
}

static double
ui_avg_corr_per_block(unsigned int corr, unsigned int ok) {
    if (ok == 0) {
        return 0.0;
    }
    return (double)corr / (double)ok;
}

static int
ui_print_p2_rs_metric(const dsd_state* state) {
    unsigned int any = state->p25_p2_rs_facch_ok | state->p25_p2_rs_facch_err | state->p25_p2_rs_sacch_ok
                       | state->p25_p2_rs_sacch_err | state->p25_p2_rs_ess_ok | state->p25_p2_rs_ess_err;
    if (any == 0) {
        return 0;
    }
    int lines = 0;
    printw("| P2 RS: FACCH %u/%u SACCH %u/%u ESS %u/%u\n", state->p25_p2_rs_facch_ok, state->p25_p2_rs_facch_err,
           state->p25_p2_rs_sacch_ok, state->p25_p2_rs_sacch_err, state->p25_p2_rs_ess_ok, state->p25_p2_rs_ess_err);
    lines++;
    if (state->p25_p2_rs_facch_ok || state->p25_p2_rs_sacch_ok || state->p25_p2_rs_ess_ok) {
        double fac = ui_avg_corr_per_block(state->p25_p2_rs_facch_corr, state->p25_p2_rs_facch_ok);
        double sac = ui_avg_corr_per_block(state->p25_p2_rs_sacch_corr, state->p25_p2_rs_sacch_ok);
        double ess = ui_avg_corr_per_block(state->p25_p2_rs_ess_corr, state->p25_p2_rs_ess_ok);
        printw("| P2 RS avg corr: FACCH %4.1f SACCH %4.1f ESS %4.1f\n", fac, sac, ess);
        lines++;
    }
    return lines;
}

static int
ui_print_p25p2_metrics(const dsd_opts* opts, const dsd_state* state, int is_p25p1, int is_p25p2) {
    if (!is_p25p2 && !(is_p25p1 && opts && opts->trunk_enable == 1)) {
        return 0;
    }
    int lines = 0;
    lines += ui_print_p2_voice_avg_metric(state);
    lines += ui_print_p2_voice_percentile_metric(state);
    lines += ui_print_p2_rs_metric(state);
    return lines;
}

static int
ui_print_p25_rtl_metrics(int is_p25p1, int is_p25p2) {
#ifdef USE_RTLSDR
    if (!is_p25p1 && !is_p25p2) {
        return 0;
    }
    dsd_frontend_metrics metrics;
    (void)dsd_app_frontend_get_metrics(&metrics);
    const dsd_frontend_decode_health* health = &metrics.decode_health;
    if (!health->valid) {
        return 0;
    }
    printw("| RTL Health: P1 FEC %u/%u P2 FACCH %u/%u SACCH %u/%u VERR %u\n", health->p25p1_fec_ok,
           health->p25p1_fec_err, health->p25p2_facch_ok, health->p25p2_facch_err, health->p25p2_sacch_ok,
           health->p25p2_sacch_err, health->p25p2_voice_err);
    return 1;
#else
    UNUSED(is_p25p1);
    UNUSED(is_p25p2);
    return 0;
#endif
}

static int
ui_p25_sm_tag_idx(const dsd_state* state, int back_offset) {
    int idx = (state->p25_sm_tag_head - 1 - back_offset) % 8;
    if (idx < 0) {
        idx += 8;
    }
    return idx;
}

static int
ui_print_p25_sm_overview(const dsd_state* state) {
    const dsd_trunk_cc_candidates* cc_candidates = dsd_trunk_cc_candidates_peek(state);
    unsigned int cc_added = cc_candidates ? cc_candidates->added : 0;
    unsigned int cc_used = cc_candidates ? cc_candidates->used : 0;
    int cc_count = (cc_candidates && cc_candidates->count > 0 && cc_candidates->count <= DSD_TRUNK_CC_CANDIDATES_MAX)
                       ? cc_candidates->count
                       : 0;
    const char* sm_mode = ui_p25_sm_mode_to_str(state->p25_sm_mode);
    printw("| SM: mode:%s tunes %u rel %u/%u; CC cands add:%u used:%u count:%d\n", sm_mode, state->p25_sm_tune_count,
           state->p25_sm_release_count, state->p25_sm_cc_return_count, cc_added, cc_used, cc_count);
    return 1;
}

static int
ui_print_p25_cc_vc_metric(const dsd_state* state) {
    long cc = (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;
    long vc = ui_guess_active_vc_freq(state);
    char cc_buf[48];
    char vc_buf[48];
    ui_format_freq_mhz_or_dash(cc, cc_buf, sizeof(cc_buf));
    ui_format_freq_mhz_or_dash(vc, vc_buf, sizeof(vc_buf));
    printw("| CC/VC: CC:%s VC:%s\n", cc_buf, vc_buf);
    return 1;
}

static int
ui_print_p25_sm_last_release(const dsd_state* state) {
    if (state->p25_sm_last_release_time == 0) {
        return 0;
    }
    double dt_rel = (double)(time(NULL) - state->p25_sm_last_release_time);
    printw("| SM Last: release d=%4.1fs\n", dt_rel);
    return 1;
}

static int
ui_print_p25_sm_last_reason(const dsd_state* state) {
    if (state->p25_sm_last_reason[0] == '\0' || state->p25_sm_last_reason_time == 0) {
        return 0;
    }
    double dt_tag = (double)(time(NULL) - state->p25_sm_last_reason_time);
    printw("| SM Last: %s d=%4.1fs\n", state->p25_sm_last_reason, dt_tag);
    return 1;
}

static int
ui_print_p25_sm_tags(const dsd_state* state) {
    if (state->p25_sm_tag_count <= 0) {
        return 0;
    }
    time_t now = time(NULL);
    ui_print_lborder_green();
    addstr(" SM Tags: ");
    int shown = 0;
    for (int k = 0; k < state->p25_sm_tag_count && k < 3; k++) {
        int idx = ui_p25_sm_tag_idx(state, k);
        const char* tag = state->p25_sm_tags[idx];
        double dt = (double)(now - state->p25_sm_tag_time[idx]);
        if (shown > 0) {
            addstr(" | ");
        }
        printw("%s(%0.1fs)", tag[0] ? tag : "-", dt);
        shown++;
    }
    addch('\n');
    return 1;
}

static int
ui_append_sm_path_symbol(char* path, size_t path_len, int wrote, char sym) {
    int m = (int)strlen(path);
    if (m + 3 >= (int)path_len) {
        return wrote;
    }
    if (wrote > 0) {
        path[m++] = '\xE2';
        path[m++] = '\x86';
        path[m++] = '\x92';
    }
    path[m++] = sym;
    path[m] = '\0';
    return wrote + 1;
}

static int
ui_print_p25_sm_path(const dsd_state* state) {
    if (state->p25_sm_tag_count <= 0) {
        return 0;
    }
    char path[64] = {0};
    int n = state->p25_sm_tag_count;
    if (n > 6) {
        n = 6;
    }
    int wrote = 0;
    for (int k = n - 1; k >= 0; k--) {
        int idx = ui_p25_sm_tag_idx(state, k);
        char sym = ui_sm_tag_symbol(state->p25_sm_tags[idx]);
        wrote = ui_append_sm_path_symbol(path, sizeof(path), wrote, sym);
    }
    ui_print_lborder_green();
    addstr(" SM Path: ");
    addstr(path[0] ? path : "-");
    addch('\n');
    return 1;
}

static int
ui_print_p25_sm_iden_summary(const dsd_state* state) {
    int iden_total = 0;
    int iden_confirmed = 0;
    if (!ui_p25_iden_confirmed_count(state, &iden_total, &iden_confirmed)) {
        return 0;
    }
    printw("| IDENs: %d total (%d confirmed)\n", iden_total, iden_confirmed);
    return 1;
}

static int
ui_print_p25_sm_cc_mode(const dsd_state* state) {
    if (state->p25_cc_freq == 0 && state->trunk_cc_freq == 0) {
        return 0;
    }
    printw("| CC: %s\n", state->p25_cc_is_tdma ? "TDMA" : "FDMA");
    return 1;
}

static int
ui_print_p25_site_status_metric(const dsd_state* state) {
    if (!state->p25_site_lra_valid && !state->p25_site_network_active_valid && !state->p25_cc_prot_valid) {
        return 0;
    }
    char lra[12] = "-";
    char net[16] = "-";
    char prot[12] = "-";
    if (state->p25_site_lra_valid) {
        DSD_SNPRINTF(lra, sizeof lra, "%02X", state->p25_site_lra);
    }
    if (state->p25_site_network_active_valid) {
        DSD_SNPRINTF(net, sizeof net, "%s", state->p25_site_network_active ? "active" : "failsoft");
    }
    if (state->p25_cc_prot_valid) {
        DSD_SNPRINTF(prot, sizeof prot, "%02X", state->p25_cc_prot_algid);
    }
    printw("| Site: LRA:%s Net:%s protected-CC-ALGID:%s\n", lra, net, prot);
    return 1;
}

static void
ui_print_p25_service_wrap_prefix(const char* prefix, int prefix_len, int continuation) {
    ui_print_lborder_green();
    if (!continuation) {
        addnstr(prefix, prefix_len);
        return;
    }
    for (int i = 0; i < prefix_len; i++) {
        addch(' ');
    }
}

static void
ui_print_p25_service_wrap_newline(const char* prefix, int prefix_len, int* lines, int* used) {
    addch('\n');
    (*lines)++;
    *used = 1 + prefix_len;
    ui_print_p25_service_wrap_prefix(prefix, prefix_len, 1);
}

static const char*
ui_p25_service_next_token(const char* text, const char** out_token, int* out_len) {
    const char* comma = strchr(text, ',');
    int len = comma ? (int)(comma - text) : (int)strlen(text);
    const char* next = comma ? comma + 1 : text + len;
    if (*next == ' ') {
        next++;
    }
    *out_token = text;
    *out_len = len;
    return next;
}

static void
ui_print_p25_service_token_text(const char* token, int len, const char* prefix, int prefix_len, int cols, int* lines,
                                int* used) {
    while (len > 0) {
        int avail = cols - *used;
        if (avail <= 0) {
            ui_print_p25_service_wrap_newline(prefix, prefix_len, lines, used);
            avail = cols - *used;
        }
        int chunk = (len < avail) ? len : avail;
        addnstr(token, chunk);
        token += chunk;
        len -= chunk;
        *used += chunk;
    }
}

static void
ui_print_p25_service_token(const char* token, int len, const char* prefix, int prefix_len, int cols, int* lines,
                           int* used) {
    int sep = (*used > 1 + prefix_len) ? 2 : 0;
    if (sep > 0 && *used + sep + len > cols) {
        ui_print_p25_service_wrap_newline(prefix, prefix_len, lines, used);
        sep = 0;
    }
    if (sep > 0) {
        addstr(", ");
        *used += sep;
    }
    ui_print_p25_service_token_text(token, len, prefix, prefix_len, cols, lines, used);
}

static int
ui_print_p25_service_names_wrapped(const char* label, const char* names) {
    char prefix[32] = {0};
    int prefix_len = DSD_SNPRINTF(prefix, sizeof(prefix), " %s: ", label);
    if (prefix_len < 0) {
        prefix_len = 0;
    }
    if (prefix_len >= (int)sizeof(prefix)) {
        prefix_len = (int)sizeof(prefix) - 1;
    }

    int cols = ui_screen_cols_default80();
    const int min_cols = 1 + prefix_len + 8;
    if (cols < min_cols) {
        cols = min_cols;
    }

    const char* text = (names && names[0]) ? names : "-";
    int lines = 1;
    int used = 1 + prefix_len;
    ui_print_p25_service_wrap_prefix(prefix, prefix_len, 0);

    const char* p = text;
    while (*p) {
        const char* token = NULL;
        int len = 0;
        p = ui_p25_service_next_token(p, &token, &len);
        ui_print_p25_service_token(token, len, prefix, prefix_len, cols, &lines, &used);
    }
    addch('\n');
    return lines;
}

static int
ui_print_p25_service_metric(const dsd_state* state) {
    if (!state->p25_sys_services_valid) {
        return 0;
    }
    char available[512] = {0};
    char supported[512] = {0};
    size_t available_count =
        p25_format_system_service_names(state->p25_sys_services_available, available, sizeof(available));
    size_t supported_count =
        p25_format_system_service_names(state->p25_sys_services_supported, supported, sizeof(supported));
    printw("| Services: Avail:%06X(%u) Supp:%06X(%u) RPL:%u\n", state->p25_sys_services_available,
           (unsigned)available_count, state->p25_sys_services_supported, (unsigned)supported_count,
           state->p25_sys_services_request_priority);
    int lines = 1;
    if (state->p25_sys_services_available == state->p25_sys_services_supported) {
        lines += ui_print_p25_service_names_wrapped("Avail/Supp", available);
    } else {
        lines += ui_print_p25_service_names_wrapped("Avail", available);
        lines += ui_print_p25_service_names_wrapped("Supp", supported);
    }
    return lines;
}

static int
ui_print_p25_trunk_metrics(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || opts->trunk_enable != 1) {
        return 0;
    }
    int lines = 0;
    lines += ui_print_p25_sm_overview(state);
    lines += ui_print_p25_cc_vc_metric(state);
    lines += ui_print_p25_sm_last_release(state);
    lines += ui_print_p25_sm_last_reason(state);
    lines += ui_print_p25_sm_tags(state);
    lines += ui_print_p25_sm_path(state);
    lines += ui_print_p25_sm_iden_summary(state);
    lines += ui_print_p25_sm_cc_mode(state);
    lines += ui_print_p25_site_status_metric(state);
    lines += ui_print_p25_service_metric(state);
    return lines;
}

static int
ui_clamp_p2_ring_fill(int fill) {
    if (fill < 0) {
        return 0;
    }
    if (fill > DSD_P25_P2_AUDIO_RING_DEPTH) {
        return DSD_P25_P2_AUDIO_RING_DEPTH;
    }
    return fill;
}

static void
ui_get_p2_hold_windows(double* ring_hold, double* mac_hold) {
    *ring_hold = 0.75;
    *mac_hold = 3.0;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg) {
        *ring_hold = cfg->p25_ring_hold_s;
        *mac_hold = cfg->p25_mac_hold_s;
    }
}

static int
ui_p2_ring_recent(int ring_count, double dmac, double ring_hold) {
    return (ring_count > 0) && (dmac >= 0.0) && (dmac <= ring_hold);
}

static int
ui_p2_slot_active(const dsd_opts* opts, const dsd_state* state, int slot, double dmac, double dt, double ring_hold,
                  double mac_hold) {
    int ring_recent = ui_p2_ring_recent(state->p25_p2_audio_ring_count[slot], dmac, ring_hold);
    int has_audio = state->p25_p2_audio_allowed[slot] || ring_recent;
    if (opts && dt >= opts->trunk_hangtime) {
        has_audio = ring_recent;
    }
    if (dmac >= 0.0 && dmac <= mac_hold) {
        return 1;
    }
    return has_audio;
}

static int
ui_print_p25p2_slot_line(const dsd_state* state) {
    int act = state->p25_p2_active_slot;
    int lfill = ui_clamp_p2_ring_fill(state->p25_p2_audio_ring_count[0]);
    int rfill = ui_clamp_p2_ring_fill(state->p25_p2_audio_ring_count[1]);
    printw("| P2 slot: %s; jitter S1:%d/%d S2:%d/%d\n",
           (act == 0)   ? "1"
           : (act == 1) ? "2"
                        : "-",
           lfill, DSD_P25_P2_AUDIO_RING_DEPTH, rfill, DSD_P25_P2_AUDIO_RING_DEPTH);
    return 1;
}

static int
ui_print_p25p2_gate_line(const dsd_opts* opts, const dsd_state* state) {
    time_t now = time(NULL);
    double l_dmac = (state->p25_p2_last_mac_active[0] != 0) ? (double)(now - state->p25_p2_last_mac_active[0]) : -1.0;
    double r_dmac = (state->p25_p2_last_mac_active[1] != 0) ? (double)(now - state->p25_p2_last_mac_active[1]) : -1.0;
    double dt = (state->last_vc_sync_time != 0) ? (double)(now - state->last_vc_sync_time) : -1.0;
    double dt_tune = (state->p25_last_vc_tune_time != 0) ? (double)(now - state->p25_last_vc_tune_time) : -1.0;
    double ring_hold = 0.75;
    double mac_hold = 3.0;
    ui_get_p2_hold_windows(&ring_hold, &mac_hold);
    int l_act = ui_p2_slot_active(opts, state, 0, l_dmac, dt, ring_hold, mac_hold);
    int r_act = ui_p2_slot_active(opts, state, 1, r_dmac, dt, ring_hold, mac_hold);
    printw("| SM Gate: L[a=%d rc=%d dMAC=%4.1fs act=%d]  R[a=%d rc=%d dMAC=%4.1fs act=%d]  dt=%4.1fs tune=%4.1fs\n",
           state->p25_p2_audio_allowed[0] ? 1 : 0, state->p25_p2_audio_ring_count[0], l_dmac, l_act,
           state->p25_p2_audio_allowed[1] ? 1 : 0, state->p25_p2_audio_ring_count[1], r_dmac, r_act, dt, dt_tune);
    return 1;
}

static int
ui_print_p25p2_slot_metrics(const dsd_opts* opts, const dsd_state* state, int is_p25p2) {
    if (!is_p25p2) {
        return 0;
    }
    int lines = 0;
    lines += ui_print_p25p2_slot_line(state);
    lines += ui_print_p25p2_gate_line(opts, state);
    return lines;
}

static int
ui_print_p25p1_sm_timers_metric(const dsd_state* state) {
    time_t now = time(NULL);
    double nowm = dsd_time_now_monotonic_s();
    double dt_cc = ui_time_diff_maybe_monotonic(now, state->last_cc_sync_time, nowm, state->last_cc_sync_time_m);
    double dt_vc = ui_time_diff_maybe_monotonic(now, state->last_vc_sync_time, nowm, state->last_vc_sync_time_m);
    double dt_tune =
        ui_time_diff_maybe_monotonic(now, state->p25_last_vc_tune_time, nowm, state->p25_last_vc_tune_time_m);
    double tdu_age = (state->p25_p1_last_tdu_m > 0.0) ? nowm - state->p25_p1_last_tdu_m : -1.0;
    printw("| SM Timers: dCC=%4.1fs dVC=%4.1fs dTune=%4.1fs TDU_age=%4.1fs\n", dt_cc, dt_vc, dt_tune, tdu_age);
    return 1;
}

static int
ui_print_p25p1_sm_flags_metric(const dsd_opts* opts, const dsd_state* state) {
    int tuned = (opts->trunk_is_tuned == 1) ? 1 : 0;
    int tick = p25_sm_in_tick();
    const char* hold = (state->tg_hold != 0) ? "on" : "-";
    printw("| SM Flags: tuned:%d force_rel:%d tick:%d hold:%s\n", tuned, state->p25_sm_force_release ? 1 : 0, tick,
           hold);
    return 1;
}

static int
ui_print_p25p1_policy_metric(const dsd_opts* opts) {
    const char* pol_data = (opts->trunk_tune_data_calls == 1) ? "on" : "off";
    const char* pol_priv = (opts->trunk_tune_private_calls == 1) ? "on" : "off";
    const char* pol_enc = (opts->trunk_tune_enc_calls == 1) ? "follow" : "lockout";
    printw("| Policy: data:%s priv:%s enc:%s hang:%.1fs\n", pol_data, pol_priv, pol_enc, opts->trunk_hangtime);
    return 1;
}

static int
ui_print_p25p1_sm_metrics(const dsd_opts* opts, const dsd_state* state, int is_p25p1) {
    if (!is_p25p1 || !opts || opts->trunk_enable != 1) {
        return 0;
    }
    int lines = 0;
    lines += ui_print_p25p1_sm_timers_metric(state);
    lines += ui_print_p25p1_sm_flags_metric(opts, state);
    lines += ui_print_p25p1_policy_metric(opts);
    return lines;
}

static int
ui_print_p25p1_duid_metrics(const dsd_state* state) {
    unsigned int du_sum = state->p25_p1_duid_hdu + state->p25_p1_duid_ldu1 + state->p25_p1_duid_ldu2
                          + state->p25_p1_duid_tdu + state->p25_p1_duid_tdulc + state->p25_p1_duid_tsbk
                          + state->p25_p1_duid_mpdu;
    if (du_sum == 0) {
        return 0;
    }
    printw("| P1 DUID: HDU %u LDU1 %u LDU2 %u TDU %u TDULC %u TSBK %u MPDU %u\n", state->p25_p1_duid_hdu,
           state->p25_p1_duid_ldu1, state->p25_p1_duid_ldu2, state->p25_p1_duid_tdu, state->p25_p1_duid_tdulc,
           state->p25_p1_duid_tsbk, state->p25_p1_duid_mpdu);
    return 1;
}

int
ui_print_p25_metrics(const dsd_opts* opts, const dsd_state* state) {
    if (!state) {
        return 0;
    }
    int lines = 0;
    int is_p25p1 = DSD_SYNC_IS_P25P1(ncurses_last_synctype);
    int is_p25p2 = DSD_SYNC_IS_P25P2(ncurses_last_synctype);
    lines += ui_print_p25_core_metrics(state, is_p25p1, is_p25p2);
    lines += ui_print_p25p2_metrics(opts, state, is_p25p1, is_p25p2);
    lines += ui_print_p25_rtl_metrics(is_p25p1, is_p25p2);
    lines += ui_print_p25_trunk_metrics(opts, state);
    lines += ui_print_p25p2_slot_metrics(opts, state, is_p25p2);
    lines += ui_print_p25p1_sm_metrics(opts, state, is_p25p1);
    lines += ui_print_p25p1_duid_metrics(state);
    return lines;
}

void
ui_print_p25_cc_candidates(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (opts->trunk_enable != 1) {
        return;
    }
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    const int count = (cc && cc->count > 0 && cc->count <= DSD_TRUNK_CC_CANDIDATES_MAX) ? cc->count : 0;
    if (count <= 0) {
        ui_print_lborder_green();
        addstr(" (none)\n");
        return;
    }
    int cols = ui_screen_cols_default80();
    int shown = 0;
    int line_used = 0;
    for (int i = 0; i < count; i++) {
        long f = cc->candidates[i];
        if (f == 0) {
            continue;
        }
        char buf[64];
        int is_next = (i == (cc->idx % count));
        int m = DSD_SNPRINTF(buf, sizeof buf, "%c%.6lf MHz", is_next ? '>' : ' ', (double)f / 1000000.0);
        if (m < 0) {
            m = 0;
        }
        ui_emit_wrapped_item(buf, m, cols, &line_used);
        shown++;
    }
    if (shown > 0 && line_used > 0) {
        addch('\n');
    }
}

void
ui_print_p25_secondary_ccs(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (opts->trunk_enable != 1) {
        return;
    }
    if (state->p25_secondary_cc_count <= 0) {
        ui_print_lborder_green();
        addstr(" (none)\n");
        return;
    }
    int cols = ui_screen_cols_default80();
    int shown = 0;
    int line_used = 0;
    time_t now = time(NULL);
    int count = state->p25_secondary_cc_count;
    if (count > P25_SECONDARY_CC_MAX) {
        count = P25_SECONDARY_CC_MAX;
    }
    for (int i = 0; i < count && shown < P25_SECONDARY_CC_MAX; i++) {
        if (state->p25_secondary_cc_entries[i].freq == 0) {
            continue;
        }
        char buf[96];
        int m = ui_format_secondary_cc_line(state, i, now, buf, sizeof(buf));
        if (m < 0) {
            m = 0;
        }
        ui_emit_wrapped_item(buf, m, cols, &line_used);
        shown++;
    }
    if (shown > 0 && line_used > 0) {
        addch('\n');
    }
}

void
ui_print_p25_neighbors(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (state->p25_nb_count <= 0) {
        ui_print_lborder_green();
        addstr(" (none)\n");
        return;
    }
    int idxs[32];
    int n = ui_collect_neighbor_indices(state, idxs, (int)(sizeof(idxs) / sizeof(idxs[0])));
    ui_sort_neighbors_by_last_seen(state, idxs, n);
    int cols = ui_screen_cols_default80();
    int shown = 0;
    int line_used = 0;
    time_t now = time(NULL);
    for (int i = 0; i < n && shown < 20; i++) {
        int k = idxs[i];
        char buf[160];
        int m = ui_format_neighbor_line(state, k, now, buf, sizeof(buf));
        if (m < 0) {
            m = 0;
        }
        ui_emit_wrapped_item(buf, m, cols, &line_used);
        shown++;
    }
    if (shown > 0 && line_used > 0) {
        addch('\n');
    }
}

void
ui_print_p25_iden_plan(const dsd_opts* opts, const dsd_state* state) {
    UNUSED(opts);
    if (!state) {
        return;
    }
    int any = 0;
    for (int id = 0; id < 16; id++) {
        if (state->p25_iden_fdma[id].populated || state->p25_iden_tdma[id].populated) {
            any = 1;
            break;
        }
    }
    if (!any) {
        ui_print_lborder_green();
        addstr(" (none)\n");
        return;
    }
    for (int id = 0; id < 16; id++) {
        const p25_iden_entry_t* fdma = &state->p25_iden_fdma[id];
        const p25_iden_entry_t* tdma = &state->p25_iden_tdma[id];
        int has_fdma = ui_p25_iden_entry_has_data(fdma);
        int has_tdma = ui_p25_iden_entry_has_data(tdma);
        if (!has_fdma && !has_tdma) {
            continue;
        }
        if (has_fdma) {
            ui_p25_print_iden_line(id, fdma, has_tdma, "FDMA");
        }
        if (has_tdma) {
            ui_p25_print_iden_line(id, tdma, has_fdma, "TDMA");
        }
    }
}

static long int
ui_canonical_active_p25_freq(const dsd_call_state_snapshot* calls) {
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        const dsd_call_snapshot* call = &calls->slots[slot];
        if (call->phase == DSD_CALL_PHASE_ACTIVE && ui_is_p25_synctype(call->protocol) && call->frequency_hz > 0) {
            return (long int)call->frequency_hz;
        }
    }
    return 0;
}

static long int
ui_recent_activity_vc_freq(const dsd_state* state) {
    for (int i = 0; i < 31; i++) {
        const char* activity = state->active_channel[i];
        if (!activity || activity[0] == '\0') {
            continue;
        }
        char channel[8] = {0};
        if (!ui_extract_channel_token(activity, channel, sizeof(channel))) {
            continue;
        }
        const long int frequency = ui_lookup_trunk_chan_map(state, channel);
        if (frequency != 0) {
            return frequency;
        }
    }
    return 0;
}

long int
ui_guess_active_vc_freq(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    dsd_call_state_snapshot calls;
    const int has_canonical = dsd_call_state_copy_snapshot(state, &calls) > 0;
    if (has_canonical) {
        const long int canonical_frequency = ui_canonical_active_p25_freq(&calls);
        if (canonical_frequency != 0) {
            return canonical_frequency;
        }
    }
    if (state->trunk_vc_freq[0] != 0) {
        return state->trunk_vc_freq[0];
    }
    if (state->p25_vc_freq[0] != 0) {
        return state->p25_vc_freq[0];
    }
    return ui_recent_activity_vc_freq(state);
}
