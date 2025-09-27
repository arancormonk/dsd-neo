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

// (legacy file-scope variables removed)

//testing a few things, going to put this into ncursesMenu
#define WIDTH  36
#define HEIGHT 25

int startx = 0;
int starty = 0;

static void
destroy_window(WINDOW** win) {
    if (win != NULL && *win != NULL) {
        delwin(*win);
        *win = NULL;
    }
}

// (legacy helpers and static choice tables removed)

#ifdef USE_RTLSDR
// Snapshot and reapply DSP settings selected in the ncurses menu so they
// persist across stream destroy/recreate during menu lifetime.
typedef struct {
    int have;
    int manual_override;
    int cqpsk_enable, fll_enable, ted_enable, auto_dsp_enable;
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
    // CQPSK runtime params (leave -1 to keep from snapshot when not relevant)
    rtl_stream_cqpsk_set(s->lms_enable, s->taps, s->mu_q15, s->update_stride, s->wl_enable, s->dfe_enable, s->dfe_taps,
                         s->mf_enable, -1);
    // Matched filter / RRC and DQPSK decision
    rtl_stream_cqpsk_set_rrc(s->rrc_enable, s->rrc_alpha_percent, s->rrc_span_syms);
    rtl_stream_cqpsk_set_dqpsk(s->dqpsk_enable);
    // IQ balance (mode-aware)
    rtl_stream_toggle_iq_balance(s->iqbal_enable);
}
#endif

//ncursesMenu
void
ncursesMenu(dsd_opts* opts, dsd_state* state) {
    // Mark menu open so the main loop (e.g., frame sync) suppresses processing
    state->menuopen = 1;

    // Update sync time so we don't immediately go CC hunting when exiting the menu
    state->last_cc_sync_time = time(NULL);

    // Pre-menu: close current outputs/inputs to avoid buffering while menu is open
    if (opts->audio_out == 1 && opts->audio_out_type == 0) {
        closePulseOutput(opts);
    }
    if (opts->audio_out_type == 2 || opts->audio_out_type == 5) {
        close(opts->audio_out_fd);
    }
    if (opts->audio_in_type == 0) {
        closePulseInput(opts);
    }
    if (opts->audio_in_type == 3) {
#ifdef USE_RTLSDR
        if (g_rtl_ctx) {
            // Soft-stop the RTL stream while the menu is open to avoid CPU spin
            rtl_stream_soft_stop(g_rtl_ctx);
        }
#endif
    }
    if (opts->audio_in_type == 8) {
        sf_close(opts->tcp_file_in);
    }

    // Reset some transient state
    state->payload_keyid = 0;
    state->payload_keyidR = 0;
    state->nxdn_last_tg = 0;
    state->nxdn_last_ran = -1;
    state->nxdn_last_rid = 0;

    // Run the data-driven main menu
    ui_menu_main(opts, state);

    // Capture current DSP runtime selections before we restart the stream
#ifdef USE_RTLSDR
    DspMenuSnapshot dsp_snap = {0};
    dsp_capture_snapshot(&dsp_snap);
#endif

    // Minimal window cleanup (kept for symmetry; not used by the new menu)
    WINDOW* menu_win = NULL;
    WINDOW* test_win = NULL;
    WINDOW* entry_win = NULL;
    WINDOW* info_win = NULL;
    destroy_window(&info_win);
    destroy_window(&entry_win);
    destroy_window(&test_win);
    destroy_window(&menu_win);

    clrtoeol();
    refresh();
    state->menuopen = 0;

    // Post-menu: reopen outputs/inputs based on current configuration
    if (opts->audio_out == 1 && opts->audio_out_type == 0) {
        openPulseOutput(opts);
    }
    if (opts->audio_out_type == 2 || opts->audio_out_type == 5) {
        openOSSOutput(opts);
    }
    if (opts->audio_in_type == 0) {
        openPulseInput(opts);
    }
    if (opts->audio_in_type == 3) {
#ifdef USE_RTLSDR
        /* If a full restart was requested (or the context was dropped),
           destroy any stale context, then create and start fresh. Otherwise,
           simply resume the existing stream. */
        if (opts->rtl_needs_restart || g_rtl_ctx == NULL) {
            if (g_rtl_ctx) {
                rtl_stream_destroy(g_rtl_ctx);
                g_rtl_ctx = NULL;
            }
            if (rtl_stream_create(opts, &g_rtl_ctx) < 0) {
                fprintf(stderr, "Failed to create RTL stream.\n");
            } else if (rtl_stream_start(g_rtl_ctx) < 0) {
                fprintf(stderr, "Failed to open RTL-SDR stream.\n");
            } else {
                // Reapply DSP settings selected in the menu to the fresh stream
                dsp_apply_snapshot(&dsp_snap);
                opts->rtl_started = 1;
                opts->rtl_needs_restart = 0;
            }
        } else {
            if (rtl_stream_start(g_rtl_ctx) < 0) {
                fprintf(stderr, "Failed to resume RTL-SDR stream.\n");
            } else {
                opts->rtl_started = 1;
            }
        }
        reset_dibit_buffer(state);
#elif AERO_BUILD
        opts->audio_out_type = 5;
#else
        opts->audio_out_type = 0;
        openPulseOutput(opts);
#endif
    } else {
#ifdef USE_RTLSDR
        /* If RTL is no longer the active input, free any existing context
           so background tune calls become no-ops and resources are returned. */
        if (g_rtl_ctx) {
            rtl_stream_soft_stop(g_rtl_ctx);
            rtl_stream_destroy(g_rtl_ctx);
            g_rtl_ctx = NULL;
        }
        opts->rtl_started = 0;
        opts->rtl_needs_restart = 0;
#endif
    }
    if (opts->audio_in_type == 8) {
        opts->tcp_file_in = sf_open_fd(opts->tcp_sockfd, SFM_READ, opts->audio_in_file_info, 0);
    }

    // Update sync time once more on exit
    state->last_cc_sync_time = time(NULL);
} // end ncursesMenu
