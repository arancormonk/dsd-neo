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

//ncursesMenu
void
ncursesMenu(dsd_opts* opts, dsd_state* state) {
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
            rtl_stream_clear_output(g_rtl_ctx);
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
        if (opts->rtl_started == 0) {
            opts->rtl_started = 1;
            if (g_rtl_ctx == NULL) {
                if (rtl_stream_create(opts, &g_rtl_ctx) < 0) {
                    fprintf(stderr, "Failed to create RTL stream.\n");
                }
            }
            if (g_rtl_ctx && rtl_stream_start(g_rtl_ctx) < 0) {
                fprintf(stderr, "Failed to open RTL-SDR stream.\n");
            }
        }
        if (g_rtl_ctx) {
            rtl_stream_clear_output(g_rtl_ctx);
        }
        reset_dibit_buffer(state);
#elif AERO_BUILD
        opts->audio_out_type = 5;
#else
        opts->audio_out_type = 0;
        openPulseOutput(opts);
#endif
    }
    if (opts->audio_in_type == 8) {
        opts->tcp_file_in = sf_open_fd(opts->tcp_sockfd, SFM_READ, opts->audio_in_file_info, 0);
    }

    // Update sync time once more on exit
    state->last_cc_sync_time = time(NULL);
} // end ncursesMenu
