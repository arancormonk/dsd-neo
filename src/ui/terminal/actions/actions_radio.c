// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UI command actions — radio domain */

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/ui/ui_cmd_dispatch.h>

static int
ui_handle_ppm_delta(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    int32_t d = 0;
    if (c->n >= (int)sizeof(int32_t)) {
        memcpy(&d, c->data, sizeof(int32_t));
    }
    opts->rtlsdr_ppm_error += d;
    return 1;
}

static int
ui_handle_invert_toggle(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)state;
    (void)c;
    int inv = opts->inverted_dmr ? 0 : 1;
    opts->inverted_dmr = inv;
    opts->inverted_dpmr = inv;
    opts->inverted_x2tdma = inv;
    opts->inverted_ysf = inv;
    opts->inverted_m17 = inv;
    return 1;
}

static int
ui_handle_mod_toggle(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (state->rf_mod == 0) {
        opts->mod_c4fm = 0;
        opts->mod_qpsk = 1;
        opts->mod_gfsk = 0;
        state->rf_mod = 1;
        state->samplesPerSymbol = 10;
        state->symbolCenter = 4;
    } else {
        opts->mod_c4fm = 1;
        opts->mod_qpsk = 0;
        opts->mod_gfsk = 0;
        state->rf_mod = 0;
        // Keep current symbol timing unless other code adjusts it
    }
    return 1;
}

static int
ui_handle_mod_p2_toggle(dsd_opts* opts, dsd_state* state, const struct UiCmd* c) {
    (void)c;
    if (state->rf_mod == 0) {
        opts->mod_c4fm = 0;
        opts->mod_qpsk = 1;
        opts->mod_gfsk = 0;
        state->rf_mod = 1;
        state->samplesPerSymbol = 8;
        state->symbolCenter = 3;
    } else {
        opts->mod_c4fm = 1;
        opts->mod_qpsk = 0;
        opts->mod_gfsk = 0;
        state->rf_mod = 0;
        state->samplesPerSymbol = 8;
        state->symbolCenter = 3;
    }
    return 1;
}

const struct UiCmdReg ui_actions_radio[] = {
    {UI_CMD_PPM_DELTA, ui_handle_ppm_delta},
    {UI_CMD_INVERT_TOGGLE, ui_handle_invert_toggle},
    {UI_CMD_MOD_TOGGLE, ui_handle_mod_toggle},
    {UI_CMD_MOD_P2_TOGGLE, ui_handle_mod_p2_toggle},
    {0, NULL},
};
