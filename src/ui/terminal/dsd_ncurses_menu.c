// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
* dsd_ncurses_menu.c
* DSD-FME ncurses terminal menu system
*
* LWVMOBILE
* 2025-05 DSD-FME Florida Man Edition
*-----------------------------------------------------------------------------*/

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/ui/menu_core.h>

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

uint32_t temp_freq = -1;

//testing a few things, going to put this into ncursesMenu
#define WIDTH  36
#define HEIGHT 25

int startx = 0;
int starty = 0;

#ifdef USE_RTLSDR
#if 0  /* Unused helpers kept for reference; re-enable when wired */
// Snapshot and reapply DSP settings selected in the ncurses menu so they
// persist across stream destroy/recreate during menu lifetime.
typedef struct {
    int have;
    int manual_override;
    int cqpsk_enable, fll_enable, ted_enable, auto_dsp_enable;
    int ted_force;
    int lms_enable, taps, mu_q15, update_stride, wl_enable, dfe_enable, dfe_taps, mf_enable, cma;
    int rrc_enable, rrc_alpha_percent, rrc_span_syms;
    int dqpsk_enable;
    int iqbal_enable;
} DspMenuSnapshot;

static void
dsp_capture_snapshot(DspMenuSnapshot* s) {
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->have = 1;
    s->manual_override = rtl_stream_get_manual_dsp();
    rtl_stream_dsp_get(&s->cqpsk_enable, &s->fll_enable, &s->ted_enable, &s->auto_dsp_enable);
    s->ted_force = rtl_stream_get_ted_force();
    rtl_stream_cqpsk_get(&s->lms_enable, &s->taps, &s->mu_q15, &s->update_stride, &s->wl_enable, &s->dfe_enable,
                         &s->dfe_taps, &s->mf_enable, &s->cma);
    rtl_stream_cqpsk_get_rrc(&s->rrc_enable, &s->rrc_alpha_percent, &s->rrc_span_syms);
    rtl_stream_cqpsk_get_dqpsk(&s->dqpsk_enable);
    s->iqbal_enable = rtl_stream_get_iq_balance();
}

static void
dsp_apply_snapshot(const DspMenuSnapshot* s) {
    if (!s || !s->have) {
        return;
    }
    // Manual override and auto-DSP gate
    rtl_stream_set_manual_dsp(s->manual_override);
    rtl_stream_toggle_auto_dsp(s->auto_dsp_enable);
    // Coarse toggles
    rtl_stream_toggle_cqpsk(s->cqpsk_enable);
    rtl_stream_toggle_fll(s->fll_enable);
    rtl_stream_toggle_ted(s->ted_enable);
    // Ensure TED force state is restored as well
    rtl_stream_set_ted_force(s->ted_force);
    // CQPSK runtime params (leave -1 to keep from snapshot when not relevant)
    rtl_stream_cqpsk_set(s->lms_enable, s->taps, s->mu_q15, s->update_stride, s->wl_enable, s->dfe_enable, s->dfe_taps,
                         s->mf_enable, -1);
    // Matched filter / RRC and DQPSK decision
    rtl_stream_cqpsk_set_rrc(s->rrc_enable, s->rrc_alpha_percent, s->rrc_span_syms);
    rtl_stream_cqpsk_set_dqpsk(s->dqpsk_enable);
    // IQ balance (mode-aware)
    rtl_stream_toggle_iq_balance(s->iqbal_enable);
}
#endif /* 0 */
#endif

//ncursesMenu
void
ncursesMenu(dsd_opts* opts, dsd_state* state) {
    // Open the data-driven menu as a nonblocking overlay and return immediately.
    // Decode and the base UI continue to run underneath.
    ui_menu_open_async(opts, state);
}
