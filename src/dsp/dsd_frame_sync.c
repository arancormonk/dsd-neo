// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <dsd-neo/core/dsd.h>
#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif
#include <dsd-neo/runtime/config.h>
#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/protocol/p25/p25_p2_sm_min.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <locale.h>

void
printFrameSync(dsd_opts* opts, dsd_state* state, char* frametype, int offset, char* modulation) {
    UNUSED3(state, offset, modulation);

    char timestr[9];
    getTimeC_buf(timestr);
    if (opts->verbose > 0) {
        fprintf(stderr, "%s ", timestr);
        fprintf(stderr, "Sync: %s ", frametype);
    }

    //oops, that made a nested if-if-if-if statement,
    //causing a memory leak

    // if (opts->verbose > 2)
    //fprintf (stderr,"o: %4i ", offset);
    // if (opts->verbose > 1)
    //fprintf (stderr,"mod: %s ", modulation);
    // if (opts->verbose > 2)
    //fprintf (stderr,"g: %f ", state->aout_gain);

    /* stack buffer; no free */
}

int
getFrameSync(dsd_opts* opts, dsd_state* state) {
    /* Dwell timer for CQPSK entry to prevent immediate fallback to C4FM. */
    static double qpsk_dwell_enter_m = 0.0;
    const time_t now = time(NULL);
    // Periodic P25 trunk SM heartbeat (once per second) to enforce hangtime
    // fallbacks even if frame processing stalls due to signal loss.
    static time_t last_tick = 0;
    if (now != last_tick) {
        p25_sm_try_tick(opts, state);
        last_tick = now;
    }
    /* detects frame sync and returns frame type
   *  0 = +P25p1
   *  1 = -P25p1
   *  2 = +X2-TDMA (non inverted signal data frame)
   *  3 = -X2-TDMA (inverted signal voice frame)
   *  4 = +X2-TDMA (non inverted signal voice frame)
   *  5 = -X2-TDMA (inverted signal data frame)
   *  6 = +D-STAR
   *  7 = -D-STAR
   *  8 = +M17 STR (non inverted stream frame)
   *  9 = -M17 STR (inverted stream frame)
   * 10 = +DMR (non inverted signal data frame)
   * 11 = -DMR (inverted signal voice frame)
   * 12 = +DMR (non inverted signal voice frame)
   * 13 = -DMR (inverted signal data frame)
   * 14 = +ProVoice
   * 15 = -ProVoice
   * 16 = +M17 LSF (non inverted link frame)
   * 17 = -M17 LSF (inverted link frame)
   * 18 = +D-STAR_HD
   * 19 = -D-STAR_HD
   * 20 = +dPMR Frame Sync 1
   * 21 = +dPMR Frame Sync 2
   * 22 = +dPMR Frame Sync 3
   * 23 = +dPMR Frame Sync 4
   * 24 = -dPMR Frame Sync 1
   * 25 = -dPMR Frame Sync 2
   * 26 = -dPMR Frame Sync 3
   * 27 = -dPMR Frame Sync 4
   * 28 = +NXDN (sync only)
   * 29 = -NXDN (sync only)
   * 30 = +YSF
   * 31 = -YSF
   * 32 = DMR MS Voice
   * 33 = DMR MS Data
   * 34 = DMR RC Data
   * 35 = +P25 P2
   * 36 = -P25 P2
   * 37 = +EDACS
     * 38 = -EDACS
     */

    // P25 CC hunting and all tuner control are owned by the P25 SM now.

    /* When LSM Simple is enabled, ensure the symbol sampler uses QPSK windowing
       immediately by pinning rf_mod to QPSK. This keeps the demod path (CQPSK)
       and the symbol domain in sync even before the SNR-based auto switch. */
    {
        int lsm_simple = dsd_neo_get_lsm_simple();
        if (lsm_simple) {
            state->rf_mod = 1; /* QPSK */
        }
    }

    int i, t, dibit, sync, symbol, synctest_pos, lastt;
    char synctest[25];
    char synctest12[13]; //dPMR
    char synctest10[11]; //NXDN FSW only
    char synctest32[33];
    char synctest20[21]; //YSF
    char synctest48[49]; //EDACS
    char synctest8[9];   //M17
    char synctest16[17]; //M17 Preamble
    char modulation[8];
    char* synctest_p;
    char synctest_buf[10240]; //what actually is assigned to this, can't find its use anywhere?
    int lmin, lmax, lidx;

    //assign t_max value based on decoding type expected (all non-auto decodes first)
    int t_max = 24; //initialize as an actual value to prevent any overflow related issues
    if (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1) {
        t_max = 10;
    }
    //dPMR
    else if (opts->frame_dpmr == 1) {
        t_max = 12; //based on Frame_Sync_2 pattern
    } else if (opts->frame_m17 == 1) {
        t_max = 8;
    } else if (state->lastsynctype == 30 || state->lastsynctype == 31) {
        t_max = 20; //20 on YSF
    }
    //if Phase 2, then only 19
    else if (state->lastsynctype == 35 || state->lastsynctype == 36) {
        t_max = 19; //Phase 2 S-ISCH is only 19
    } else {
        t_max = 24; //24 for everything else
    }

    int lbuf[48],
        lbuf2
            [48]; //if we use t_max in these arrays, and t >=  t_max in condition below, then it can overflow those checks in there if t exceeds t_max
    int lsum;
    //init the lbuf
    memset(lbuf, 0, sizeof(lbuf));
    memset(lbuf2, 0, sizeof(lbuf2));

    // detect frame sync
    t = 0;
    synctest10[10] = 0;
    synctest[24] = 0;
    synctest8[8] = 0; //M17, wasn't initialized or terminated (source of much pain and frustration in Cygwin)
    synctest12[12] = 0;
    synctest16[16] = 0; //M17, wasn't initialized or terminated (source of much pain and frustration in Cygwin)
    synctest48[48] = 0;
    synctest32[32] = 0;
    synctest20[20] = 0;
    modulation[7] = 0; //not initialized or terminated (unsure if this would be an issue or not)
    synctest_pos = 0;
    synctest_p = synctest_buf + 10;
    sync = 0;
    // lmin/lmax initialized later before use
    lidx = 0;
    lastt = 0;
    state->numflips = 0;

    //run here as well
    if (opts->use_ncurses_terminal == 1) {
        ncursesPrinter(opts, state);
    }

    //slot 1
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);

    //slot 2 for TDMA systems
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);

    if ((opts->symboltiming == 1) && (state->carrier == 1)) {
        //fprintf (stderr,"\nSymbol Timing:\n");
        //printw("\nSymbol Timing:\n");
    }
    /* Simple hysteresis for modulation auto-detect to avoid rapid flapping
     * between C4FM/QPSK/GFSK when scanning for sync on marginal signals. The
     * vote counters persist across calls to getFrameSync to carry momentum. */
    static int vote_qpsk = 0;
    static int vote_c4fm = 0;
    static int vote_gfsk = 0;

    while (sync == 0) {

        t++;

        //run ncurses printer more frequently when no sync to speed up responsiveness of it during no sync period
        //NOTE: Need to monitor and test this, if responsiveness issues arise, then disable this
        if (opts->use_ncurses_terminal == 1 && ((t % 300) == 0)) { //t maxes out at 1800 (6 times each getFrameSync)
            ncursesPrinter(opts, state);
        }

        symbol = getSymbol(opts, state, 0);

        lbuf[lidx] = symbol;
        state->sbuf[state->sidx] = symbol;
        if (lidx == (t_max - 1)) //23 //9 for NXDN
        {
            lidx = 0;
        } else {
            lidx++;
        }
        if (state->sidx == (opts->ssize - 1)) {
            state->sidx = 0;
        } else {
            state->sidx++;
        }

        if (lastt == t_max) {
            lastt = 0;
            /* Decide preferred modulation for this window */
            int want_mod = 0; /* 0=C4FM, 1=QPSK, 2=GFSK */
            if (state->numflips > opts->mod_threshold) {
                want_mod = 1; /* QPSK */
            } else if (state->numflips > 18 && opts->mod_gfsk == 1) {
                want_mod = 2; /* GFSK */
            } else {
                want_mod = 0; /* C4FM */
            }

            /* Bias decision with demod SNR when available to avoid C4FM<->QPSK flapping
               on P25 LSM/CQPSK. Prefer QPSK when its SNR clearly exceeds C4FM; conversely
               prefer C4FM only when it exceeds QPSK by a larger margin. Also apply a small
               stickiness when already in QPSK and SNRs are similar. */
#ifdef USE_RTLSDR
            do {
                /* Pull smoothed SNR; fall back to lightweight estimators if needed */
                extern double rtl_stream_get_snr_c4fm(void);
                extern double rtl_stream_get_snr_cqpsk(void);
                extern double rtl_stream_estimate_snr_c4fm_eye(void);
                extern double rtl_stream_estimate_snr_qpsk_const(void);
                double snr_c = rtl_stream_get_snr_c4fm();
                double snr_q = rtl_stream_get_snr_cqpsk();
                if (snr_c <= -50.0) {
                    snr_c = rtl_stream_estimate_snr_c4fm_eye();
                }
                if (snr_q <= -50.0) {
                    snr_q = rtl_stream_estimate_snr_qpsk_const();
                }
                if (snr_c > -50.0 || snr_q > -50.0) {
                    /* Only apply bias when at least one metric is sane */
                    if (snr_q > -50.0 && snr_c > -50.0) {
                        double delta = snr_q - snr_c;
                        double nowm_bias = dsd_time_now_monotonic_s();
                        int in_qpsk_dwell =
                            (state->rf_mod == 1 && qpsk_dwell_enter_m > 0.0 && (nowm_bias - qpsk_dwell_enter_m) < 2.0);
                        if (delta >= 2.0) {
                            want_mod = 1; /* clear QPSK advantage */
                        } else if (delta <= -3.0 && !in_qpsk_dwell) {
                            want_mod = 0; /* clear C4FM advantage (but not during dwell) */
                        } else {
                            /* Within small margin: if currently QPSK, keep favoring it */
                            if (state->rf_mod == 1) {
                                want_mod = 1;
                            }
                        }
                    } else if (snr_q > -50.0 && state->rf_mod == 1) {
                        /* Only QPSK SNR available and already on QPSK: prefer QPSK */
                        want_mod = 1;
                    }
                }
            } while (0);
#endif

            /* If LSM Simple is active, lock to QPSK and do not allow modulation flaps. */
            {
                int lsm_simple_active = dsd_neo_get_lsm_simple();
                if (lsm_simple_active) {
                    want_mod = 1; /* prefer QPSK */
                }
            }
            /* Update votes (use hysteresis; be more eager for GFSK to avoid misclassification dwell) */
            if (want_mod == 1) {
                vote_qpsk++;
                vote_c4fm = 0;
                vote_gfsk = 0;
            } else if (want_mod == 2) {
                vote_gfsk++;
                vote_qpsk = 0;
                vote_c4fm = 0;
            } else {
                vote_c4fm++;
                vote_qpsk = 0;
                vote_gfsk = 0;
            }

            int do_switch = -1; /* -1=no-op, else new rf_mod */
            /* Guard: if LSM Simple is active, suppress switching logic entirely.
               However, ensure CQPSK DSP path is actually enabled once. */
            if (dsd_neo_get_lsm_simple()) {
#ifdef USE_RTLSDR
                int cq = 0, f = 0, t = 0, a = 0;
                rtl_stream_dsp_get(&cq, &f, &t, &a);
                if (a && !rtl_stream_get_manual_dsp() && !cq) {
                    /* Bring up the CQPSK path with conservative defaults */
                    rtl_stream_toggle_iq_balance(0);
                    rtl_stream_toggle_cqpsk(1);
                    rtl_stream_toggle_fll(1);
                    rtl_stream_toggle_ted(1);
                    /* LMS on; 5 taps; µ=2; stride=6; WL off; DFE off; MF on; short CMA warmup */
                    rtl_stream_cqpsk_set(1, 5, 2, 6, 0, 0, 0, 1, 1200);
                }
#endif
                do_switch = -1;
            } else {
                /*
             * Require 2 consecutive windows for C4FM<->QPSK to prevent flapping on marginal signals.
             * For GFSK (DMR/dPMR/NXDN), permit immediate switch on first qualifying window to minimize
             * misclassification time that can corrupt early bursts and elevate audio errors.
             */
                /* Slightly increase hysteresis when leaving QPSK to avoid flip-flop on LSM */
                double nowm_dwell = dsd_time_now_monotonic_s();
                int in_qpsk_dwell2 =
                    (state->rf_mod == 1 && qpsk_dwell_enter_m > 0.0 && (nowm_dwell - qpsk_dwell_enter_m) < 2.0);
                int req_c4_votes = (state->rf_mod == 1) ? (in_qpsk_dwell2 ? 5 : 3) : 2;
                if (want_mod == 1 && vote_qpsk >= 2 && state->rf_mod != 1) {
                    do_switch = 1;
                } else if (want_mod == 2 && vote_gfsk >= 1 && state->rf_mod != 2) {
                    do_switch = 2; /* eager switch to GFSK on first vote */
                } else if (want_mod == 0 && vote_c4fm >= req_c4_votes && state->rf_mod != 0) {
                    do_switch = 0;
                }
            }
            if (do_switch >= 0) {
                state->rf_mod = do_switch;
#ifdef USE_RTLSDR
                int cq = 0, f = 0, t = 0, a = 0;
                rtl_stream_dsp_get(&cq, &f, &t, &a);
                if (a && !rtl_stream_get_manual_dsp()) {
                    if (do_switch == 1) {
                        /* Switch to CQPSK path */
                        rtl_stream_toggle_iq_balance(0);
                        rtl_stream_toggle_cqpsk(1);
                        rtl_stream_toggle_fll(1);
                        rtl_stream_toggle_ted(1);
                        /* Conservative initial preset: LMS on; 5 taps; µ=2; stride=6; WL off; DFE off; MF on; CMA warmup */
                        rtl_stream_cqpsk_set(1, 5, 2, 6, 0, 0, 0, 1, 1200);
                        /* Start CQPSK dwell timer */
                        qpsk_dwell_enter_m = dsd_time_now_monotonic_s();
                    } else {
                        /* Switch away from CQPSK path */
                        rtl_stream_toggle_iq_balance(1);
                        rtl_stream_toggle_cqpsk(0);
                        rtl_stream_toggle_fll(0);
                        rtl_stream_toggle_ted(0);
                        qpsk_dwell_enter_m = 0.0;
                    }
                }
#endif
            }

            state->numflips = 0;
        } else {
            lastt++;
        }

        if (state->dibit_buf_p > state->dibit_buf + 900000) {
            state->dibit_buf_p = state->dibit_buf + 200;
        }

        //determine dibit state
        if (symbol > 0) {
            *state->dibit_buf_p = 1;
            state->dibit_buf_p++;
            dibit = 49; // '1'
        } else {
            *state->dibit_buf_p = 3;
            state->dibit_buf_p++;
            dibit = 51; // '3'
        }

        if (opts->symbol_out_f && dibit != 0) {
            int csymbol = 0;
            if (dibit == 49) {
                csymbol = 1; //1
            }
            if (dibit == 51) {
                csymbol = 3; //3
            }
            //fprintf (stderr, "%d", dibit);
            fputc(csymbol, opts->symbol_out_f);
        }

        //digitize test for storing dibits in buffer correctly for dmr recovery

        if (state->dmr_payload_p > state->dmr_payload_buf + 900000) {
            state->dmr_payload_p = state->dmr_payload_buf + 200;
        }
        if (state->dmr_reliab_p && state->dmr_reliab_p > state->dmr_reliab_buf + 900000) {
            state->dmr_reliab_p = state->dmr_reliab_buf + 200;
        }

        if (1 == 1) {
            if (symbol > state->center) {
                if (symbol > state->umid) {
                    *state->dmr_payload_p = 1; // +3
                } else {
                    *state->dmr_payload_p = 0; // +1
                }
            } else {
                if (symbol < state->lmid) {
                    *state->dmr_payload_p = 3; // -3
                } else {
                    *state->dmr_payload_p = 2; // -1
                }
            }
            if (state->dmr_reliab_p) {
                int sym = symbol;
                int rel = 0;
                if (sym > state->umid) {
                    int span = state->max - state->umid;
                    if (span < 1) {
                        span = 1;
                    }
                    rel = (sym - state->umid) * 255 / span;
                } else if (sym > state->center) {
                    int d1 = sym - state->center;
                    int d2 = state->umid - sym;
                    int span = state->umid - state->center;
                    if (span < 1) {
                        span = 1;
                    }
                    int m = d1 < d2 ? d1 : d2;
                    rel = (m * 510) / span;
                } else if (sym >= state->lmid) {
                    int d1 = state->center - sym;
                    int d2 = sym - state->lmid;
                    int span = state->center - state->lmid;
                    if (span < 1) {
                        span = 1;
                    }
                    int m = d1 < d2 ? d1 : d2;
                    rel = (m * 510) / span;
                } else {
                    int span = state->lmid - state->min;
                    if (span < 1) {
                        span = 1;
                    }
                    rel = (state->lmid - sym) * 255 / span;
                }
                if (rel < 0) {
                    rel = 0;
                }
                if (rel > 255) {
                    rel = 255;
                }
#ifdef USE_RTLSDR
                double snr_db = rtl_stream_get_snr_c4fm();
                if (snr_db < -50.0) {
                    snr_db = rtl_stream_estimate_snr_c4fm_eye();
                }
                int w256 = 0;
                if (snr_db > -5.0) {
                    if (snr_db >= 20.0) {
                        w256 = 255;
                    } else {
                        double w = (snr_db + 5.0) / 25.0;
                        if (w < 0.0) {
                            w = 0.0;
                        }
                        if (w > 1.0) {
                            w = 1.0;
                        }
                        w256 = (int)(w * 255.0 + 0.5);
                    }
                }
                int scale_num = 204 + (w256 >> 2);
                int scaled = (rel * scale_num) >> 8;
                if (scaled > 255) {
                    scaled = 255;
                }
                if (scaled < 0) {
                    scaled = 0;
                }
                rel = scaled;
#endif
                *state->dmr_reliab_p = (uint8_t)rel;
                state->dmr_reliab_p++;
            }
        }

        state->dmr_payload_p++;
        // end digitize and dmr buffer testing

        *synctest_p = dibit;
        if (t >= t_max) //works excelent now with short sync patterns, and no issues with large ones!
        {
            for (i = 0; i < t_max; i++) //24
            {
                lbuf2[i] = lbuf[i];
            }
            qsort(lbuf2, t_max, sizeof(int), comp);
            lmin = (lbuf2[1] + lbuf2[2] + lbuf2[3]) / 3;
            lmax = (lbuf2[t_max - 3] + lbuf2[t_max - 2] + lbuf2[t_max - 1]) / 3;

            if (state->rf_mod == 1) {
                state->minbuf[state->midx] = lmin;
                state->maxbuf[state->midx] = lmax;
                if (state->midx == (opts->msize - 1)) //-1
                {
                    state->midx = 0;
                } else {
                    state->midx++;
                }
                lsum = 0;
                for (i = 0; i < opts->msize; i++) {
                    lsum += state->minbuf[i];
                }
                state->min = lsum / opts->msize;
                lsum = 0;
                for (i = 0; i < opts->msize; i++) {
                    lsum += state->maxbuf[i];
                }
                state->max = lsum / opts->msize;
                state->center = ((state->max) + (state->min)) / 2;
                state->maxref = (int)((state->max) * 0.80F);
                state->minref = (int)((state->min) * 0.80F);
            } else {
                state->maxref = state->max;
                state->minref = state->min;
            }

            // Optional SNR-based pre-decode squelch: skip expensive sync search when SNR is low.
            // Falls back to legacy power squelch gating for certain modes.
#ifdef USE_RTLSDR
            {
                extern const dsdneoRuntimeConfig* dsd_neo_get_config(void);
                const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
                int snr_gate = 0;
                if (cfg && cfg->snr_sql_is_set) {
                    double snr_db = -200.0;
                    if (opts->frame_p25p1 == 1) {
                        extern double rtl_stream_get_snr_c4fm(void);
                        snr_db = rtl_stream_get_snr_c4fm();
                    } else if (opts->frame_p25p2 == 1) {
                        extern double rtl_stream_get_snr_cqpsk(void);
                        snr_db = rtl_stream_get_snr_cqpsk();
                    } else if (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1 || opts->frame_dpmr == 1
                               || opts->frame_m17 == 1) {
                        extern double rtl_stream_get_snr_gfsk(void);
                        snr_db = rtl_stream_get_snr_gfsk();
                    }
                    if (snr_db > -150.0 && snr_db < (double)cfg->snr_sql_db) {
                        snr_gate = 1;
                    }
                }
                if (snr_gate) {
                    goto SYNC_TEST_END;
                }
            }
#endif
            // Legacy power-based pre-gate for some GFSK modes when using RTL input
            if (opts->audio_in_type == 3 && opts->rtl_pwr < opts->rtl_squelch_level) {
                if (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1 || opts->frame_dpmr == 1
                    || opts->frame_m17 == 1) {
                    goto SYNC_TEST_END;
                }
            }

            strncpy(synctest, (synctest_p - 23), 24);
            if (opts->frame_p25p1 == 1) {
                if (strcmp(synctest, P25P1_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    state->dmrburstR = 17;
                    state->payload_algidR = 0;
                    state->dmr_stereo =
                        1; //check to see if this causes dmr data issues later on during mixed sync types
                    sprintf(state->ftype, "P25 Phase 1");
                    if (opts->errorbars == 1) {
                        printFrameSync(opts, state, "+P25p1", synctest_pos + 1, modulation);
                    }
                    state->lastsynctype = 0;
                    state->last_cc_sync_time = now;
                    return (0);
                }
                if (strcmp(synctest, INV_P25P1_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    state->dmrburstR = 17;
                    state->payload_algidR = 0;
                    state->dmr_stereo =
                        1; //check to see if this causes dmr data issues later on during mixed sync types
                    sprintf(state->ftype, "P25 Phase 1");
                    if (opts->errorbars == 1) {
                        printFrameSync(opts, state, "-P25p1 ", synctest_pos + 1, modulation);
                    }
                    state->lastsynctype = 1;
                    state->last_cc_sync_time = now;
                    return (1);
                }
            }
            /* When DMR/dPMR/NXDN are enabled targets, proactively disable FM AGC/limiter which can
             * distort 2-level/FSK symbol envelopes and elevate early audio errors under marginal SNR.
             * Also force FLL/TED off for FSK paths. */
#ifdef USE_RTLSDR
            if ((opts->frame_dmr == 1 || opts->frame_dpmr == 1 || opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1)) {
                extern void rtl_stream_set_fm_agc(int onoff);
                extern void rtl_stream_set_fm_limiter(int onoff);
                extern void rtl_stream_toggle_iq_balance(int onoff);
                extern void rtl_stream_toggle_fll(int onoff);
                extern void rtl_stream_toggle_ted(int onoff);
                rtl_stream_set_fm_agc(0);
                rtl_stream_set_fm_limiter(0);
                rtl_stream_toggle_iq_balance(0);
                rtl_stream_toggle_fll(0);
                rtl_stream_toggle_ted(0);
            }
#endif
            if (opts->frame_x2tdma == 1) {
                if ((strcmp(synctest, X2TDMA_BS_DATA_SYNC) == 0) || (strcmp(synctest, X2TDMA_MS_DATA_SYNC) == 0)) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + (lmax)) / 2;
                    state->min = ((state->min) + (lmin)) / 2;
                    if (opts->inverted_x2tdma == 0) {
                        // data frame
                        sprintf(state->ftype, "X2-TDMA");
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "+X2-TDMA ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 2;
                        return (2);
                    } else {
                        // inverted voice frame
                        sprintf(state->ftype, "X2-TDMA");
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "-X2-TDMA ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 3) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 3;
                        return (3);
                    }
                }
                if ((strcmp(synctest, X2TDMA_BS_VOICE_SYNC) == 0) || (strcmp(synctest, X2TDMA_MS_VOICE_SYNC) == 0)) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    if (opts->inverted_x2tdma == 0) {
                        // voice frame
                        sprintf(state->ftype, "X2-TDMA");
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "+X2-TDMA ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 4) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 4;
                        return (4);
                    } else {
                        // inverted data frame
                        sprintf(state->ftype, "X2-TDMA");
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "-X2-TDMA ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 5;
                        return (5);
                    }
                }
            }
            //YSF sync
            strncpy(synctest20, (synctest_p - 19), 20);
            if (opts->frame_ysf == 1) {
                if (strcmp(synctest20, FUSION_SYNC) == 0) {
                    printFrameSync(opts, state, "+YSF ", synctest_pos + 1, modulation);
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    opts->inverted_ysf = 0;
                    state->lastsynctype = 30;
                    return (30);
                } else if (strcmp(synctest20, INV_FUSION_SYNC) == 0) {
                    printFrameSync(opts, state, "-YSF ", synctest_pos + 1, modulation);
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    opts->inverted_ysf = 1;
                    state->lastsynctype = 31;
                    return (31);
                }
            }
            //end YSF sync

            //M17 Sync -- Just STR and LSF for now
            strncpy(synctest16, (synctest_p - 15), 16);
            strncpy(synctest8, (synctest_p - 7), 8);
            if (opts->frame_m17 == 1) {
                //preambles will skip dibits in an attempt to prime the
                //demodulator but not attempt any decoding
                if (strcmp(synctest8, M17_PRE) == 0) {
                    if (opts->inverted_m17 == 0) {
                        printFrameSync(opts, state, "+M17 PREAMBLE", synctest_pos + 1, modulation);
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        state->lastsynctype = 98;
                        fprintf(stderr, "\n");
                        return (98);
                    }
                } else if (strcmp(synctest8, M17_PIV) == 0) {
                    if (opts->inverted_m17 == 1) {
                        printFrameSync(opts, state, "-M17 PREAMBLE", synctest_pos + 1, modulation);
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        state->lastsynctype = 99;
                        fprintf(stderr, "\n");
                        return (99);
                    }
                } else if (strcmp(synctest8, M17_PKT) == 0) {
                    if (opts->inverted_m17 == 0) {
                        printFrameSync(opts, state, "+M17 PKT", synctest_pos + 1, modulation);
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        if (state->lastsynctype == 86 || state->lastsynctype == 8) {
                            state->lastsynctype = 86;
                            return (86);
                        }
                        state->lastsynctype = 86;
                        fprintf(stderr, "\n");
                    }
                    // else //unknown, -BRT?
                    // {
                    //   printFrameSync (opts, state, "-M17 BRT", synctest_pos + 1, modulation);
                    //   state->carrier = 1;
                    //   state->offset = synctest_pos;
                    //   state->max = ((state->max) + lmax) / 2;
                    //   state->min = ((state->min) + lmin) / 2;
                    //   if (state->lastsynctype == 77)
                    //   {
                    //     state->lastsynctype = 77;
                    //     return (77);
                    //   }
                    //   state->lastsynctype = 77;
                    //   fprintf (stderr, "\n");
                    // }
                } else if (strcmp(synctest8, M17_STR) == 0) {
                    if (opts->inverted_m17 == 0) {
                        printFrameSync(opts, state, "+M17 STR", synctest_pos + 1, modulation);
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        if (state->lastsynctype == 16 || state->lastsynctype == 8) {
                            state->lastsynctype = 16;
                            return (16);
                        }
                        state->lastsynctype = 16;
                        fprintf(stderr, "\n");
                    } else {
                        printFrameSync(opts, state, "-M17 LSF", synctest_pos + 1, modulation);
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        if (state->lastsynctype == 99) {
                            state->lastsynctype = 9;
                            return (9);
                        }
                        state->lastsynctype = 9;
                        fprintf(stderr, "\n");
                    }
                } else if (strcmp(synctest8, M17_LSF) == 0) {
                    if (opts->inverted_m17 == 1) {
                        printFrameSync(opts, state, "-M17 STR", synctest_pos + 1, modulation);
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        if (state->lastsynctype == 17 || state->lastsynctype == 9) {
                            state->lastsynctype = 17;
                            return (17);
                        }
                        state->lastsynctype = 17;
                        fprintf(stderr, "\n");
                    } else {
                        printFrameSync(opts, state, "+M17 LSF", synctest_pos + 1, modulation);
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        if (state->lastsynctype == 98) {
                            state->lastsynctype = 8;
                            return (8);
                        }
                        state->lastsynctype = 8;
                        fprintf(stderr, "\n");
                    }
                }
            }
            //end M17

            //P25 P2 sync S-ISCH VCH
            strncpy(synctest20, (synctest_p - 19), 20);
            if (opts->frame_p25p2 == 1) {
                if (0 == 0) {
                    if (strcmp(synctest20, P25P2_SYNC) == 0) {
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        opts->inverted_p2 = 0;
                        state->lastsynctype = 35; //35
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "+P25p2", synctest_pos + 1, modulation);
                        }
                        if (state->p2_wacn != 0 && state->p2_cc != 0 && state->p2_sysid != 0) {
                            printFrameInfo(opts, state);
                        } else {
                            fprintf(stderr, "%s", KRED);
                            fprintf(stderr, " P2 Missing Parameters            ");
                            fprintf(stderr, "%s", KNRM);
                        }
                        state->last_cc_sync_time = time(NULL);
                        return (35); //35
                    }
                }
            }
            if (opts->frame_p25p2 == 1) {
                if (0 == 0) {
                    //S-ISCH VCH
                    if (strcmp(synctest20, INV_P25P2_SYNC) == 0) {
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        opts->inverted_p2 = 1;
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "-P25p2", synctest_pos + 1, modulation);
                        }
                        if (state->p2_wacn != 0 && state->p2_cc != 0 && state->p2_sysid != 0) {
                            printFrameInfo(opts, state);
                        } else {
                            fprintf(stderr, "%s", KRED);
                            fprintf(stderr, " P2 Missing Parameters            ");
                            fprintf(stderr, "%s", KNRM);
                        }

                        state->lastsynctype = 36; //36
                        state->last_cc_sync_time = time(NULL);
                        return (36); //36
                    }
                }
            }

            //dPMR sync
            strncpy(synctest, (synctest_p - 23), 24);
            strncpy(synctest12, (synctest_p - 11), 12);
            if (opts->frame_dpmr == 1) {
                if (opts->inverted_dpmr == 0) {
                    if (strcmp(synctest, DPMR_FRAME_SYNC_1) == 0) {
                        //fprintf (stderr, "+dPMR FS1\n");
                    }
                    if (strcmp(synctest12, DPMR_FRAME_SYNC_2) == 0) {
                        //fprintf (stderr, "DPMR_FRAME_SYNC_2\n");
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;

                        sprintf(state->ftype, "dPMR ");
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "+dPMR ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 21;
                        return (21);
                    }
                    if (strcmp(synctest12, DPMR_FRAME_SYNC_3) == 0) {
                        //fprintf (stderr, "+dPMR FS3 \n");
                    }
                    if (strcmp(synctest, DPMR_FRAME_SYNC_4) == 0) {
                        //fprintf (stderr, "+dPMR FS4 \n");
                    }
                }
                if (opts->inverted_dpmr == 1) {
                    if (strcmp(synctest, INV_DPMR_FRAME_SYNC_1) == 0) {
                        //fprintf (stderr, "-dPMR FS1 \n");
                    }
                    if (strcmp(synctest12, INV_DPMR_FRAME_SYNC_2) == 0) {
                        //fprintf (stderr, "INV_DPMR_FRAME_SYNC_2\n");
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;

                        sprintf(state->ftype, "dPMR ");
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "-dPMR ", synctest_pos + 1, modulation);
                        }

                        state->lastsynctype = 25;
                        return (25);
                    }
                    if (strcmp(synctest12, INV_DPMR_FRAME_SYNC_3) == 0) {
                        //fprintf (stderr, "-dPMR FS3 \n");
                    }
                    if (strcmp(synctest, INV_DPMR_FRAME_SYNC_4) == 0) {
                        //fprintf (stderr, "-dPMR FS4 \n");
                    }
                }
            }

            //New DMR Sync
            if (opts->frame_dmr == 1) {

                if (strcmp(synctest, DMR_MS_DATA_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    //state->directmode = 0;
                    //fprintf (stderr, "DMR MS Data");
                    if (opts->inverted_dmr == 0) //opts->inverted_dmr
                    {
                        // data frame
                        sprintf(state->ftype, "DMR MS");
                        if (opts->errorbars == 1) {
                            //printFrameSync (opts, state, "+DMR MS Data", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 33) //33
                        {
                            //state->firstframe = 1;
                        }
                        state->lastsynctype = 33; //33
                        return (33);              //33
                    } else                        //inverted MS voice frame
                    {
                        sprintf(state->ftype, "DMR MS");
                        state->lastsynctype = 32;
                        return (32);
                    }
                }

                if (strcmp(synctest, DMR_MS_VOICE_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    //state->directmode = 0;
                    //fprintf (stderr, "DMR MS VOICE\n");
                    if (opts->inverted_dmr == 0) //opts->inverted_dmr
                    {
                        // voice frame
                        sprintf(state->ftype, "DMR MS");
                        if (opts->errorbars == 1) {
                            //printFrameSync (opts, state, "+DMR MS Voice", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 32) {
                            //state->firstframe = 1;
                        }
                        state->lastsynctype = 32;
                        return (32);
                    } else //inverted MS data frame
                    {
                        sprintf(state->ftype, "DMR MS");
                        state->lastsynctype = 33;
                        return (33);
                    }
                }

                //if ((strcmp (synctest, DMR_MS_DATA_SYNC) == 0) || (strcmp (synctest, DMR_BS_DATA_SYNC) == 0))
                if (strcmp(synctest, DMR_BS_DATA_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + (lmax)) / 2;
                    state->min = ((state->min) + (lmin)) / 2;
                    state->directmode = 0;
                    /* Force GFSK mode and stable symbol timing for DMR */
                    state->rf_mod = 2; /* GFSK */
                    if (state->samplesPerSymbol != 10) {
                        state->samplesPerSymbol = 10;
                    }
                    if (state->symbolCenter < 2 || state->symbolCenter > 8) {
                        state->symbolCenter = 5; /* middle of 0..9 */
                    }
                    if (opts->inverted_dmr == 0) {
                        // data frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1) {
                            printFrameSync(opts, state, "+DMR ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 10;
                        state->last_cc_sync_time = time(NULL);
                        return (10);
                    } else {
                        // inverted voice frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1 && opts->dmr_stereo == 0) {
                            //printFrameSync (opts, state, "-DMR ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 11) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 11;
                        state->last_cc_sync_time = time(NULL);
                        return (11); //11
                    }
                }
                if (strcmp(synctest, DMR_DIRECT_MODE_TS1_DATA_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + (lmax)) / 2;
                    state->min = ((state->min) + (lmin)) / 2;
                    //state->currentslot = 0;
                    state->directmode = 1; //Direct mode
                    state->rf_mod = 2;
                    if (state->samplesPerSymbol != 10) {
                        state->samplesPerSymbol = 10;
                    }
                    if (state->symbolCenter < 2 || state->symbolCenter > 8) {
                        state->symbolCenter = 5;
                    }
                    if (opts->inverted_dmr == 0) {
                        // data frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1) {
                            //printFrameSync (opts, state, "+DMR ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 33;
                        state->last_cc_sync_time = time(NULL);
                        return (33);
                    } else {
                        // inverted voice frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1) {
                            //printFrameSync (opts, state, "-DMR ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 11) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 32;
                        state->last_cc_sync_time = time(NULL);
                        return (32);
                    }
                } /* End if(strcmp (synctest, DMR_DIRECT_MODE_TS1_DATA_SYNC) == 0) */
                if (strcmp(synctest, DMR_DIRECT_MODE_TS2_DATA_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + (lmax)) / 2;
                    state->min = ((state->min) + (lmin)) / 2;
                    //state->currentslot = 1;
                    state->directmode = 1; //Direct mode
                    if (opts->inverted_dmr == 0) {
                        // data frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1) {
                            //printFrameSync (opts, state, "+DMR ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 33;
                        state->last_cc_sync_time = time(NULL);
                        return (33);
                    } else {
                        // inverted voice frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1 && opts->dmr_stereo == 0) {
                            //printFrameSync (opts, state, "-DMR ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 11) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 32;
                        state->last_cc_sync_time = time(NULL);
                        return (32);
                    }
                } /* End if(strcmp (synctest, DMR_DIRECT_MODE_TS2_DATA_SYNC) == 0) */
                //if((strcmp (synctest, DMR_MS_VOICE_SYNC) == 0) || (strcmp (synctest, DMR_BS_VOICE_SYNC) == 0))
                if (strcmp(synctest, DMR_BS_VOICE_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    state->directmode = 0;
                    state->rf_mod = 2;
                    if (state->samplesPerSymbol != 10) {
                        state->samplesPerSymbol = 10;
                    }
                    if (state->symbolCenter < 2 || state->symbolCenter > 8) {
                        state->symbolCenter = 5;
                    }
                    if (opts->inverted_dmr == 0) {
                        // voice frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1 && opts->dmr_stereo == 0) {
                            //printFrameSync (opts, state, "+DMR ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 12) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 12;
                        state->last_cc_sync_time = time(NULL);
                        return (12);
                    }

                    else {
                        // inverted data frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1) //&& opts->dmr_stereo == 0
                        {
                            printFrameSync(opts, state, "-DMR ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 13;
                        state->last_cc_sync_time = time(NULL);
                        return (13);
                    }
                }
                if (strcmp(synctest, DMR_DIRECT_MODE_TS1_VOICE_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    //state->currentslot = 0;
                    state->directmode = 1;       //Direct mode
                    if (opts->inverted_dmr == 0) //&& opts->dmr_stereo == 1
                    {
                        state->rf_mod = 2;
                        if (state->samplesPerSymbol != 10) {
                            state->samplesPerSymbol = 10;
                        }
                        if (state->symbolCenter < 2 || state->symbolCenter > 8) {
                            state->symbolCenter = 5;
                        }
                        // voice frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1 && opts->dmr_stereo == 0) {
                            //printFrameSync (opts, state, "+DMR ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 12) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 32;
                        state->last_cc_sync_time = time(NULL);
                        return (32); //treat Direct Mode same as MS mode for now
                    } else {
                        // inverted data frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1 && opts->dmr_stereo == 0) {
                            //printFrameSync (opts, state, "-DMR ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 33;
                        return (33);
                    }
                } /* End if(strcmp (synctest, DMR_DIRECT_MODE_TS1_VOICE_SYNC) == 0) */
                if (strcmp(synctest, DMR_DIRECT_MODE_TS2_VOICE_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    // state->currentslot = 1;
                    state->directmode = 1;       //Direct mode
                    if (opts->inverted_dmr == 0) //&& opts->dmr_stereo == 1
                    {
                        state->rf_mod = 2;
                        if (state->samplesPerSymbol != 10) {
                            state->samplesPerSymbol = 10;
                        }
                        if (state->symbolCenter < 2 || state->symbolCenter > 8) {
                            state->symbolCenter = 5;
                        }
                        // voice frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1 && opts->dmr_stereo == 0) {
                            //printFrameSync (opts, state, "+DMR ", synctest_pos + 1, modulation);
                        }
                        if (state->lastsynctype != 12) {
                            state->firstframe = 1;
                        }
                        state->lastsynctype = 32;
                        state->last_cc_sync_time = time(NULL);
                        return (32);
                    } else {
                        // inverted data frame
                        sprintf(state->ftype, "DMR ");
                        if (opts->errorbars == 1 && opts->dmr_stereo == 0) {
                            //printFrameSync (opts, state, "-DMR ", synctest_pos + 1, modulation);
                        }
                        state->lastsynctype = 33;
                        state->last_cc_sync_time = time(NULL);
                        return (33);
                    }
                } //End if(strcmp (synctest, DMR_DIRECT_MODE_TS2_VOICE_SYNC) == 0)
            } //End if (opts->frame_dmr == 1)

            //end DMR Sync

            //ProVoice and EDACS sync
            if (opts->frame_provoice == 1) {
                strncpy(synctest32, (synctest_p - 31), 32);
                strncpy(synctest48, (synctest_p - 47), 48);
                if ((strcmp(synctest32, PROVOICE_SYNC) == 0) || (strcmp(synctest32, PROVOICE_EA_SYNC) == 0)) {
                    state->last_cc_sync_time = now;
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    sprintf(state->ftype, "ProVoice ");
                    if (opts->errorbars == 1) {
                        printFrameSync(opts, state, "+PV   ", synctest_pos + 1, modulation);
                    }
                    state->lastsynctype = 14;
                    return (14);
                } else if ((strcmp(synctest32, INV_PROVOICE_SYNC) == 0)
                           || (strcmp(synctest32, INV_PROVOICE_EA_SYNC) == 0)) {
                    state->last_cc_sync_time = now;
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    sprintf(state->ftype, "ProVoice ");
                    printFrameSync(opts, state, "-PV   ", synctest_pos + 1, modulation);
                    state->lastsynctype = 15;
                    return (15);
                } else if (strcmp(synctest48, EDACS_SYNC) == 0) {
                    state->last_cc_sync_time = time(NULL);
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    printFrameSync(opts, state, "-EDACS", synctest_pos + 1, modulation);
                    state->lastsynctype = 38;
                    return (38);
                } else if (strcmp(synctest48, INV_EDACS_SYNC) == 0) {
                    state->last_cc_sync_time = time(NULL);
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    printFrameSync(opts, state, "+EDACS", synctest_pos + 1, modulation);
                    state->lastsynctype = 37;
                    return (37);
                } else if ((strcmp(synctest48, DOTTING_SEQUENCE_A) == 0)
                           || (strcmp(synctest48, DOTTING_SEQUENCE_B) == 0)) {
                    //only print and execute Dotting Sequence if Trunking and Tuned so we don't get multiple prints on this
                    if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1) {
                        printFrameSync(opts, state, " EDACS  DOTTING SEQUENCE: ", synctest_pos + 1, modulation);
                        eot_cc(opts, state);
                    }
                }

            }

            else if (opts->frame_dstar == 1) {
                if (strcmp(synctest, DSTAR_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    sprintf(state->ftype, "DSTAR ");
                    if (opts->errorbars == 1) {
                        printFrameSync(opts, state, "+DSTAR VOICE ", synctest_pos + 1, modulation);
                    }
                    state->lastsynctype = 6;
                    return (6);
                }
                if (strcmp(synctest, INV_DSTAR_SYNC) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    sprintf(state->ftype, "DSTAR ");
                    if (opts->errorbars == 1) {
                        printFrameSync(opts, state, "-DSTAR VOICE ", synctest_pos + 1, modulation);
                    }
                    state->lastsynctype = 7;
                    return (7);
                }
                if (strcmp(synctest, DSTAR_HD) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    sprintf(state->ftype, "DSTAR_HD ");
                    if (opts->errorbars == 1) {
                        printFrameSync(opts, state, "+DSTAR HEADER", synctest_pos + 1, modulation);
                    }
                    state->lastsynctype = 18;
                    return (18);
                }
                if (strcmp(synctest, INV_DSTAR_HD) == 0) {
                    state->carrier = 1;
                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    sprintf(state->ftype, " DSTAR_HD");
                    if (opts->errorbars == 1) {
                        printFrameSync(opts, state, "-DSTAR HEADER", synctest_pos + 1, modulation);
                    }
                    state->lastsynctype = 19;
                    return (19);
                }

            }

            //NXDN
            else if ((opts->frame_nxdn96 == 1) || (opts->frame_nxdn48 == 1)) {
                strncpy(synctest10, (synctest_p - 9), 10); //FSW only
                if ((strcmp(synctest10, "3131331131")
                     == 0) //this seems to be the most common 'correct' pattern on Type-C
                    || (strcmp(synctest10, "3331331131") == 0) //this one hits on new sync but gives a bad lich code
                    || (strcmp(synctest10, "3131331111") == 0) || (strcmp(synctest10, "3331331111") == 0)
                    || (strcmp(synctest10, "3131311131")
                        == 0) //First few FSW on NXDN48 Type-C seems to hit this for some reason

                ) {

                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    if (state->lastsynctype == 28) {
                        state->last_cc_sync_time = now;
                        return (28);
                    }
                    state->lastsynctype = 28;
                }

                else if (

                    (strcmp(synctest10, "1313113313") == 0) || (strcmp(synctest10, "1113113313") == 0)
                    || (strcmp(synctest10, "1313113333") == 0) || (strcmp(synctest10, "1113113333") == 0)
                    || (strcmp(synctest10, "1313133313") == 0)

                ) {

                    state->offset = synctest_pos;
                    state->max = ((state->max) + lmax) / 2;
                    state->min = ((state->min) + lmin) / 2;
                    if (state->lastsynctype == 29) {
                        state->last_cc_sync_time = now;
                        return (29);
                    }
                    state->lastsynctype = 29;
                }
            }

//Provoice Conventional -- Some False Positives due to shortened frame sync pattern, so use squelch if possible
#ifdef PVCONVENTIONAL
            if (opts->frame_provoice == 1) {
                memset(synctest32, 0, sizeof(synctest32));
                strncpy(synctest32, (synctest_p - 31), 16); //short sync grab here on 32
                char pvc_txs[9];                            //string (symbol) value of TX Address
                char pvc_rxs[9];                            //string (symbol) value of RX Address
                uint8_t pvc_txa = 0;                        //actual value of TX Address
                uint8_t pvc_rxa = 0;                        //actual value of RX Address
                strncpy(pvc_txs, (synctest_p - 15), 8);     //copy string value of TX Address
                strncpy(pvc_rxs, (synctest_p - 7), 8);      //copy string value of RX Address
                if ((strcmp(synctest32, INV_PROVOICE_CONV_SHORT) == 0)) {
                    if (state->lastsynctype
                        == 15) //use this condition, like NXDN, to migitage false positives due to short sync pattern
                    {
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        sprintf(state->ftype, "ProVoice ");
                        // fprintf (stderr, "Sync Pattern = %s ", synctest32);
                        // fprintf (stderr, "TX = %s ", pvc_txs);
                        // fprintf (stderr, "RX = %s ", pvc_rxs);
                        for (int i = 0; i < 8; i++) {
                            pvc_txa = pvc_txa << 1;
                            pvc_rxa = pvc_rxa << 1;
                            //symbol 1 is binary 1 on inverted
                            //I hate working with strings, has to be a better way to evaluate this
                            memset(pvc_txs, 0, sizeof(pvc_txs));
                            memset(pvc_rxs, 0, sizeof(pvc_rxs));
                            strncpy(pvc_txs, (synctest_p - 15 + i), 1);
                            strncpy(pvc_rxs, (synctest_p - 7 + i), 1);
                            if ((strcmp(pvc_txs, "1") == 0)) {
                                pvc_txa = pvc_txa + 1;
                            }
                            if ((strcmp(pvc_rxs, "1") == 0)) {
                                pvc_rxa = pvc_rxa + 1;
                            }
                        }
                        printFrameSync(opts, state, "-PV_C ", synctest_pos + 1, modulation);
                        fprintf(stderr, "TX: %d ", pvc_txa);
                        fprintf(stderr, "RX: %d ", pvc_rxa);
                        if (pvc_txa == 172) {
                            fprintf(stderr, "ALL CALL ");
                        }
                        state->lastsynctype = 15;
                        return (15);
                    }
                    state->lastsynctype = 15;
                } else if ((strcmp(synctest32, PROVOICE_CONV_SHORT) == 0)) {
                    if (state->lastsynctype
                        == 14) //use this condition, like NXDN, to migitage false positives due to short sync pattern
                    {
                        state->carrier = 1;
                        state->offset = synctest_pos;
                        state->max = ((state->max) + lmax) / 2;
                        state->min = ((state->min) + lmin) / 2;
                        sprintf(state->ftype, "ProVoice ");
                        // fprintf (stderr, "Sync Pattern = %s ", synctest32);
                        // fprintf (stderr, "TX = %s ", pvc_txs);
                        // fprintf (stderr, "RX = %s ", pvc_rxs);
                        for (int i = 0; i < 8; i++) {
                            pvc_txa = pvc_txa << 1;
                            pvc_rxa = pvc_rxa << 1;
                            //symbol 3 is binary 1 on positive
                            //I hate working with strings, has to be a better way to evaluate this
                            memset(pvc_txs, 0, sizeof(pvc_txs));
                            memset(pvc_rxs, 0, sizeof(pvc_rxs));
                            strncpy(pvc_txs, (synctest_p - 15 + i), 1);
                            strncpy(pvc_rxs, (synctest_p - 7 + i), 1);
                            if ((strcmp(pvc_txs, "3") == 0)) {
                                pvc_txa = pvc_txa + 1;
                            }
                            if ((strcmp(pvc_rxs, "3") == 0)) {
                                pvc_rxa = pvc_rxa + 1;
                            }
                        }
                        printFrameSync(opts, state, "+PV_C ", synctest_pos + 1, modulation);
                        fprintf(stderr, "TX: %d ", pvc_txa);
                        fprintf(stderr, "RX: %d ", pvc_rxa);
                        if (pvc_txa == 172) {
                            fprintf(stderr, "ALL CALL ");
                        }
                        state->lastsynctype = 14;
                        return (14);
                    }
                    state->lastsynctype = 14;
                }
            }
#endif //End Provoice Conventional

        SYNC_TEST_END:; //do nothing

        } // t >= 10

        if (exitflag == 1) {
            cleanupAndExit(opts, state);
        }

        if (synctest_pos < 10200) {
            synctest_pos++;
            synctest_p++;

        } else {
            // buffer reset
            synctest_pos = 0;
            synctest_p = synctest_buf;
            noCarrier(opts, state);
        }

        if (state->lastsynctype != 1) {

            if (synctest_pos >= 1800) {
                if ((opts->errorbars == 1) && (opts->verbose > 1) && (state->carrier == 1)) {
                    fprintf(stderr, "Sync: no sync\n");
                    // fprintf (stderr,"Press CTRL + C to close.\n");
                }
                // Defensive trunking fallback: if tuned to a P25 VC and voice
                // activity is stale beyond hangtime, consider a safe return to
                // the control channel. Mirror the P25 SM tick's gating so we do
                // not thrash back to CC while a slot still indicates ACTIVE.
                if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1) {
                    double dt = (state->last_vc_sync_time != 0) ? (double)(now - state->last_vc_sync_time) : 1e9;
                    double dt_since_tune =
                        (state->p25_last_vc_tune_time != 0) ? (double)(now - state->p25_last_vc_tune_time) : 1e9;
                    // Startup grace after a VC tune to avoid bouncing before PTT/audio
                    double vc_grace = 1.5; // seconds
                    {
                        const char* s = getenv("DSD_NEO_P25_VC_GRACE");
                        if (s && s[0] != '\0') {
                            double v = atof(s);
                            if (v >= 0.0 && v < 10.0) {
                                vc_grace = v;
                            }
                        }
                    }
                    int is_p2_vc = (state->p25_p2_active_slot != -1);
                    // Mirror trunk SM gating: treat jitter ring as activity
                    // only when gated by recent MAC_ACTIVE/PTT on that slot;
                    // after hangtime, ignore stale audio_allowed alone.
                    double ring_hold = 0.75; // seconds; DSD_NEO_P25_RING_HOLD
                    {
                        const char* s = getenv("DSD_NEO_P25_RING_HOLD");
                        if (s && s[0] != '\0') {
                            double v = atof(s);
                            if (v >= 0.0 && v <= 5.0) {
                                ring_hold = v;
                            }
                        }
                    }
                    double mac_hold = 3.0; // seconds; override via DSD_NEO_P25_MAC_HOLD
                    {
                        const char* s = getenv("DSD_NEO_P25_MAC_HOLD");
                        if (s && s[0] != '\0') {
                            double v = atof(s);
                            if (v >= 0.0 && v < 10.0) {
                                mac_hold = v;
                            }
                        }
                    }
                    double l_dmac = (state->p25_p2_last_mac_active[0] != 0)
                                        ? (double)(now - state->p25_p2_last_mac_active[0])
                                        : 1e9;
                    double r_dmac = (state->p25_p2_last_mac_active[1] != 0)
                                        ? (double)(now - state->p25_p2_last_mac_active[1])
                                        : 1e9;
                    int l_ring = (state->p25_p2_audio_ring_count[0] > 0) && (l_dmac <= ring_hold);
                    int r_ring = (state->p25_p2_audio_ring_count[1] > 0) && (r_dmac <= ring_hold);
                    int left_has_audio = state->p25_p2_audio_allowed[0] || l_ring;
                    int right_has_audio = state->p25_p2_audio_allowed[1] || r_ring;
                    if (dt >= opts->trunk_hangtime) {
                        left_has_audio = l_ring;
                        right_has_audio = r_ring;
                    }
                    int left_active = left_has_audio || (l_dmac <= mac_hold);
                    int right_active = right_has_audio || (r_dmac <= mac_hold);
                    int both_slots_idle = (!is_p2_vc) ? 1 : !(left_active || right_active);
                    if (dt >= opts->trunk_hangtime && both_slots_idle && dt_since_tune >= vc_grace) {
                        state->p25_sm_force_release = 1;
                        p25_sm_on_release(opts, state);
                    }
                }
                // Minimal SM: notify no-sync to help it consider HANG
                {
                    dsd_p25p2_min_evt ev = {DSD_P25P2_MIN_EV_NOSYNC, -1, 0, 0};
                    dsd_p25p2_min_handle_event(dsd_p25p2_min_get(), opts, state, &ev);
                }
                noCarrier(opts, state);

                return (-1);
            }
        }
    }

    return (-1);
}
