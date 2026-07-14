// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * ncurses_trunk_display.c
 * Trunk system display functions for ncurses UI
 */

#include <curses.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/ui/ncurses_p25_display.h>
#include <dsd-neo/ui/ncurses_trunk_display.h>
#include <dsd-neo/ui/ui_prims.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#define UI_TRUNK_SEEN_FREQ_CAP 256
#define UI_TRUNK_MAX_MAP_PRINT 32
#define UI_TRUNK_COLS_PER_LINE 3
#define UI_TRUNK_LCN_COUNT     26

typedef struct ui_trunk_render_state_s {
    long int seen_freqs[UI_TRUNK_SEEN_FREQ_CAP];
    int seen_count;
    int col_in_row;
} ui_trunk_render_state;

static int
ui_trunk_has_lcn_freq(const dsd_state* state) {
    for (int i = 0; i < UI_TRUNK_LCN_COUNT; i++) {
        if (state->trunk_lcn_freq[i] != 0) {
            return 1;
        }
    }
    return 0;
}

static void
ui_trunk_set_base_color(const dsd_state* state) {
    if (state->carrier == 1) {
        attron(COLOR_PAIR(3));
    } else {
        attron(COLOR_PAIR(4));
    }
}

static int
ui_trunk_freq_seen(const ui_trunk_render_state* render, long int freq) {
    for (int i = 0; i < render->seen_count; i++) {
        if (render->seen_freqs[i] == freq) {
            return 1;
        }
    }
    return 0;
}

static void
ui_trunk_mark_freq_seen(ui_trunk_render_state* render, long int freq) {
    if (render->seen_count < UI_TRUNK_SEEN_FREQ_CAP) {
        render->seen_freqs[render->seen_count++] = freq;
    }
}

static void
ui_trunk_begin_row_if_needed(const ui_trunk_render_state* render) {
    if (render->col_in_row == 0) {
        ui_print_lborder_green();
        addch(' ');
    }
}

static void
ui_trunk_advance_column(ui_trunk_render_state* render) {
    render->col_in_row++;
    if (render->col_in_row >= UI_TRUNK_COLS_PER_LINE) {
        addch('\n');
        render->col_in_row = 0;
    } else {
        addstr("   ");
    }
}

static void
ui_trunk_flush_row(ui_trunk_render_state* render) {
    if (render->col_in_row > 0) {
        addch('\n');
        render->col_in_row = 0;
    }
}

static void
ui_trunk_print_channel_freq(const dsd_state* state, int channel, long int freq) {
    attr_t saved_attrs = 0;
    short saved_pair = 0;
    attr_get(&saved_attrs, &saved_pair, NULL);

    int iden = -1;
    int is_iden = ui_match_iden_channel(state, channel, freq, &iden);
    if (is_iden) {
        attron(COLOR_PAIR(ui_iden_color_pair(iden)));
        printw("CH %04X[I%d]: %.06lf MHz", channel & 0xFFFF, iden & 0xF, (double)freq / 1000000.0);
        attr_set(saved_attrs, saved_pair, NULL);
    } else {
        printw("CH %04X: %.06lf MHz", channel & 0xFFFF, (double)freq / 1000000.0);
    }
}

static int
ui_trunk_find_channel_for_freq(const dsd_state* state, uint32_t chan_map_count, long int freq) {
    for (uint32_t n = 0; n < chan_map_count && n < DSD_TRUNK_CHAN_MAP_SIZE; n++) {
        const uint16_t channel = state->trunk_chan_map_used[n];
        if (!dsd_state_trunk_chan_tracked(channel)) {
            continue;
        }
        if (state->trunk_chan_map[channel] == freq) {
            return channel;
        }
    }
    return -1;
}

static void
ui_trunk_render_chan_map(const dsd_state* state, uint32_t chan_map_count, ui_trunk_render_state* render) {
    int printed = 0;
    int extra = 0;

    for (uint32_t n = 0; n < chan_map_count && n < DSD_TRUNK_CHAN_MAP_SIZE; n++) {
        const uint16_t channel = state->trunk_chan_map_used[n];
        if (!dsd_state_trunk_chan_tracked(channel)) {
            continue;
        }

        const long int freq = state->trunk_chan_map[channel];
        if (freq == 0 || ui_trunk_freq_seen(render, freq)) {
            continue;
        }

        if (printed < UI_TRUNK_MAX_MAP_PRINT) {
            ui_trunk_begin_row_if_needed(render);
            ui_trunk_print_channel_freq(state, channel, freq);
            ui_trunk_advance_column(render);
            printed++;
        } else {
            extra++;
        }
        ui_trunk_mark_freq_seen(render, freq);
    }

    ui_trunk_flush_row(render);
    if (extra > 0) {
        ui_print_lborder_green();
        printw(" ... and %d more learned channels\n", extra);
    }
}

static void
ui_trunk_render_lcn_freqs(const dsd_state* state, uint32_t chan_map_count, ui_trunk_render_state* render) {
    for (int i = 0; i < UI_TRUNK_LCN_COUNT; i++) {
        const long int freq = state->trunk_lcn_freq[i];
        if (freq == 0 || ui_trunk_freq_seen(render, freq)) {
            continue;
        }

        const int channel = ui_trunk_find_channel_for_freq(state, chan_map_count, freq);
        ui_trunk_begin_row_if_needed(render);
        if (channel >= 0) {
            ui_trunk_print_channel_freq(state, channel, freq);
        } else {
            printw("CH ----: %.06lf MHz", (double)freq / 1000000.0);
        }
        ui_trunk_advance_column(render);
        ui_trunk_mark_freq_seen(render, freq);
    }

    ui_trunk_flush_row(render);
}

static void
ui_trunk_render_iden_legend(const dsd_state* state) {
    int lls = state ? state->synctype : DSD_SYNC_NONE;
    int is_p25p1 = DSD_SYNC_IS_P25P1(lls);
    int is_p25p2 = DSD_SYNC_IS_P25P2(lls);

    if (is_p25p1 || is_p25p2) {
        ui_print_lborder_green();
        printw(" Legend: IDEN colors ");
        for (int c = 0; c < 8; c++) {
            attron(COLOR_PAIR(ui_iden_color_pair(c)));
            printw("I%d", c);
            attroff(COLOR_PAIR(ui_iden_color_pair(c)));
            addch(' ');
        }
        addch('\n');
    }
}

// Print learned trunking LCNs and their mapped frequencies
void
ui_print_learned_lcns(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (opts->trunk_enable != 1) {
        return;
    }

    int have_lcn_freq = ui_trunk_has_lcn_freq(state);
    const uint32_t chan_map_count = state->trunk_chan_map_used_count;
    int have_chan_map = chan_map_count > 0U;

    if (!have_lcn_freq && !have_chan_map) {
        return;
    }

    ui_print_header("Channels");
    ui_trunk_set_base_color(state);

    ui_trunk_render_state render = {0};

    if (have_chan_map) {
        ui_trunk_render_chan_map(state, chan_map_count, &render);
    }

    if (have_lcn_freq) {
        ui_trunk_render_lcn_freqs(state, chan_map_count, &render);
    }

    ui_trunk_render_iden_legend(state);
    ui_trunk_set_base_color(state);
}
