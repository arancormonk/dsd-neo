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
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <math.h>
#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#endif

/*
 * Centralized window selection helpers per modulation. These encapsulate
 * left/right offsets used during symbol decision and allow a single point for
 * future tuning. When freeze_window is enabled (env/config), defaults are used
 * and any per-protocol dynamic tweaks are disabled for A/B comparisons.
 */
static inline void
select_window_c4fm(const dsd_state* state, int* l_edge, int* r_edge, int freeze_window) {
    int l = 2;
    int r = 2;
    if (!freeze_window) {
        if (state->synctype == 30 || state->synctype == 31 || (state->lastsynctype >= 10 && state->lastsynctype <= 13)
            || state->lastsynctype == 32 || state->lastsynctype == 33) {
            l = 1; // YSF, DMR, some NXDN cases
        } else {
            l = 2; // P25 and NXDN96 prefer wider left window
        }
    }
    *l_edge = l;
    *r_edge = r;
}

/* ---------------- C4FM fixed RRC(alpha=0.5, 11*sps+1 taps) ---------------- */
/*
 * Designs and applies a root-raised-cosine matched filter for the real C4FM path
 * with a fixed roll-off alpha=0.5 and taps length ntaps = 11*sps + 1.
 * This matches common GNU Radio guidance (firdes.root_raised_cosine with ntaps=11*sps),
 * using an odd number of taps for symmetry.
 */
static short
c4fm_rrc_filter_alpha05_sample(short sample, int sps) {
    const double alpha = 0.5;
    if (sps <= 1) {
        return sample;
    }

    /* Cache across calls; redesign on SPS change. */
    static int last_sps = 0;
    static int taps_len = 0;
    static float taps[257];
    static float hist[257];
    static int head = -1;
    static int ready = 0;

    int desired_len = 11 * sps + 1; /* ensure odd length */
    if (desired_len < 7) {
        desired_len = 7;
    }
    if (desired_len > 257) {
        desired_len = 257;
    }

    if (!ready || sps != last_sps || taps_len != desired_len) {
        int mid = desired_len / 2;
        double sum = 0.0;
        const double pi = 3.14159265358979323846;
        for (int n = 0; n < desired_len; n++) {
            double tau = ((double)n - (double)mid) / (double)sps; /* t/T */
            double h;
            double four_a_tau = 4.0 * alpha * tau;
            double denom = pi * tau * (1.0 - (four_a_tau * four_a_tau));
            if (fabs(tau) < 1e-12) {
                h = (1.0 + alpha * (4.0 / pi - 1.0));
            } else if (alpha > 0.0 && fabs(fabs(tau) - (1.0 / (4.0 * alpha))) < 1e-9) {
                double a = alpha;
                double term1 = (1.0 + 2.0 / pi) * sin(pi / (4.0 * a));
                double term2 = (1.0 - 2.0 / pi) * cos(pi / (4.0 * a));
                h = (a / sqrt(2.0)) * (term1 + term2);
            } else {
                double num = sin(pi * tau * (1.0 - alpha)) + four_a_tau * cos(pi * tau * (1.0 + alpha));
                h = num / denom;
            }
            taps[n] = (float)h;
            sum += h;
        }
        if (fabs(sum) < 1e-18) {
            sum = 1.0;
        }
        for (int n = 0; n < desired_len; n++) {
            taps[n] = (float)(taps[n] / sum);
        }
        for (int i = 0; i < desired_len; i++) {
            hist[i] = 0.0f;
        }
        head = -1;
        taps_len = desired_len;
        last_sps = sps;
        ready = 1;
    }

    /* Push sample */
    head++;
    if (head >= taps_len) {
        head = 0;
    }
    hist[head] = (float)sample;

    /* Convolution */
    float acc = 0.0f;
    int zeros = taps_len - 1;
    for (int i = 0; i <= zeros; i++) {
        int idx = head - (zeros - i);
        if (idx < 0) {
            idx += taps_len;
        }
        acc += taps[i] * hist[idx];
    }
    if (acc > 32767.0f) {
        acc = 32767.0f;
    }
    if (acc < -32768.0f) {
        acc = -32768.0f;
    }
    return (short)lrintf(acc);
}

static inline void
select_window_qpsk(int* l_edge, int* r_edge, int freeze_window) {
    (void)freeze_window; /* no dynamic tweaks for QPSK at present */
    *l_edge = 1;         // pick i == center-1
    *r_edge = 2;         // pick i == center+2
}

static inline void
select_window_gfsk(int* l_edge, int* r_edge, int freeze_window) {
    (void)freeze_window; /* no dynamic tweaks for GFSK at present */
    *l_edge = 1;         // pick i == center-1
    *r_edge = 1;         // pick i == center+1
}

#ifdef USE_RTLSDR
/* --- C4FM clock assist (EL / M&M) --- */
static inline int
slice_c4fm_level(int x, const dsd_state* s) {
    /* Map sample to nearest of {-3,-1,1,3} using center/min/max refs. */
    int c = s->center;
    int lo = (s->minref + c) / 2;
    int hi = (s->maxref + c) / 2;
    if (x >= hi) {
        return 3;
    } else if (x >= c) {
        return 1;
    } else if (x >= lo) {
        return -1;
    } else {
        return -3;
    }
}

static inline void
maybe_c4fm_clock(dsd_opts* opts, dsd_state* state, int have_sync, int mode, int early, int mid, int late) {
    (void)opts;
    if (mode <= 0) {
        return;
    }
    /* Only on RTL pipeline and when not yet synchronized to avoid perturbing
       decoders during steady state. */
    if (opts->audio_in_type != 3) {
        return;
    }
    if (have_sync != 0) {
        return;
    }
    if (state->rf_mod != 0) {
        return; /* C4FM only */
    }

    /* Require valid neighborhood around center */
    if (state->symbolCenter < 1 || state->symbolCenter + 1 >= state->samplesPerSymbol) {
        return;
    }

    long long e = 0;
    if (mode == 1) { /* Early-Late using energy difference */
        long long er = (long long)early;
        long long lr = (long long)late;
        e = (lr * lr) - (er * er);
    } else if (mode == 2) { /* M&M using sliced decisions */
        int a_prev = state->c4fm_clk_prev_dec;
        int a_k;
        /* Prefer slicing on mid sample for stability */
        a_k = slice_c4fm_level(mid, state);
        if (a_prev == 0) {
            state->c4fm_clk_prev_dec = a_k;
            return; /* need one step of history */
        }
        /* e ≈ y_mid * (a_k - a_{k-1}) */
        e = (long long)mid * (long long)(a_k - a_prev);
        state->c4fm_clk_prev_dec = a_k;
    } else {
        return;
    }

    /* Convert to sign and apply simple persistence before nudging center */
    int dir = 0;
    if (e > 0) {
        dir = +1; /* sample early → center → right */
    } else if (e < 0) {
        dir = -1; /* sample late → center → left */
    } else {
        state->c4fm_clk_run_dir = 0;
        state->c4fm_clk_run_len = 0;
        return;
    }

    if (state->c4fm_clk_cooldown > 0) {
        state->c4fm_clk_cooldown--;
        return;
    }

    if (dir == state->c4fm_clk_run_dir) {
        state->c4fm_clk_run_len++;
    } else {
        state->c4fm_clk_run_dir = dir;
        state->c4fm_clk_run_len = 1;
    }

    /* Nudge after brief persistence */
    if (state->c4fm_clk_run_len >= 4) {
        int c = state->symbolCenter + dir;
        int min_c = 1;
        int max_c = state->samplesPerSymbol - 2;
        if (c < min_c) {
            c = min_c;
        }
        if (c > max_c) {
            c = max_c;
        }
        state->symbolCenter = c;
        state->c4fm_clk_cooldown = 12; /* short cooldown */
        state->c4fm_clk_run_len = 0;
    }
}
#endif

#ifdef USE_RTLSDR
/*
 * Nudge symbolCenter by ±1 based on a smoothed TED residual when available.
 * Guards against oscillation using a small deadband and a cooldown period.
 *
 * Safety gates:
 *  - Only when using RTL input (audio_in_type == 3)
 *  - Only when not currently synchronized (have_sync == 0)
 *  - Only for C4FM path (rf_mod == 0) to avoid QPSK perturbations
 */
static inline void
maybe_auto_center(dsd_opts* opts, dsd_state* state, int have_sync) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int freeze_window = (cfg && cfg->window_freeze_is_set) ? (cfg->window_freeze != 0) : 0;
    if (freeze_window) {
        return; // explicit freeze requested
    }
    if (opts->audio_in_type != 3) {
        return; // only when using RTL stream/demod pipeline
    }
    /* If synced, only run when explicitly allowed by runtime config. */
    if (have_sync != 0) {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        int allow_when_synced = (cfg && cfg->c4fm_clk_sync_is_set) ? (cfg->c4fm_clk_sync != 0) : 0;
        if (!allow_when_synced) {
            return;
        }
    }
    if (state->rf_mod != 0) {
        return; // limit to C4FM for now; avoid QPSK
    }

    /* Cooldown to avoid rapid flips */
    static int cooldown = 0;
    if (cooldown > 0) {
        cooldown--;
        return;
    }

    /* Read smoothed TED residual (can be 0 when TED disabled). */
    int e_ema = rtl_stream_ted_bias(NULL); /* ctx currently unused */
    if (e_ema == 0) {
        return;
    }

    /* Small deadband and persistence guard */
    const int deadband = 5000; /* empirically small; residual uses coarse units */
    static int run_dir = 0;    /* -1, 0, +1 */
    static int run_len = 0;
    int dir = 0;
    if (e_ema > deadband) {
        dir = +1; /* sample was early; center → right */
    } else if (e_ema < -deadband) {
        dir = -1; /* sample was late; center → left */
    } else {
        run_dir = 0;
        run_len = 0;
        return;
    }

    if (dir == run_dir) {
        run_len++;
    } else {
        run_dir = dir;
        run_len = 1;
    }

    /* Require brief persistence before nudging center. */
    if (run_len >= 6) {
        int c = state->symbolCenter + dir;
        /* Keep a reasonable margin within [0..samplesPerSymbol-1] */
        int min_c = 1;
        int max_c = state->samplesPerSymbol - 2;
        if (c < min_c) {
            c = min_c;
        }
        if (c > max_c) {
            c = max_c;
        }
        state->symbolCenter = c;
        cooldown = 12; /* short cooldown after each nudge */
        run_len = 0;
    }
}
#endif

#ifdef USE_RTLSDR
/*
 * When using the RTL pipeline without resampling to 48 kHz, adjust
 * samplesPerSymbol and symbolCenter proportional to the current output rate
 * to preserve decoder timing windows. Runs cheaply with a rate-change guard.
 */
static inline void
maybe_adjust_sps_for_output_rate(dsd_opts* opts, dsd_state* state) {
    if (opts->audio_in_type != 3) {
        return; /* only for RTL input */
    }
    static unsigned int last_rate = 0;
    unsigned int Fs = rtl_stream_output_rate(NULL);
    if (Fs == 0 || Fs == last_rate) {
        return;
    }
    last_rate = Fs;
    if (Fs == 48000) {
        return; /* canonical, keep existing SPS */
    }
    /* Scale SPS w.r.t. 48 kHz and preserve center ratio */
    int old_sps = state->samplesPerSymbol;
    if (old_sps <= 0) {
        old_sps = 10; /* safe default */
    }
    /* new_sps = round(old_sps * Fs / 48000) */
    long long num = (long long)old_sps * (long long)Fs;
    int new_sps = (int)((num + 24000) / 48000);
    if (new_sps < 2) {
        new_sps = 2;
    }
    if (new_sps == old_sps) {
        return;
    }
    /* Preserve existing center fraction within [1 .. new_sps-2] */
    double ratio = (double)state->symbolCenter / (double)old_sps;
    if (ratio < 0.05) {
        ratio = 0.05; /* avoid extreme left */
    }
    if (ratio > 0.95) {
        ratio = 0.95; /* avoid extreme right */
    }
    int new_center = (int)(ratio * (double)new_sps + 0.5);
    int min_c = 1;
    int max_c = new_sps - 2;
    if (new_center < min_c) {
        new_center = min_c;
    }
    if (new_center > max_c) {
        new_center = max_c;
    }
    state->samplesPerSymbol = new_sps;
    state->symbolCenter = new_center;
}
#endif

int
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    short sample;
    int i, sum, symbol, count;
    ssize_t result;

    sum = 0;
    count = 0;
    sample = 0; //init sample with a value of 0...see if this was causing issues with raw audio monitoring

#ifdef USE_RTLSDR
    /* C4FM clock assist capture around symbol center */
    int clk_mode = 0;
    int clk_early = 0, clk_mid = 0, clk_late = 0;
    if (state->rf_mod == 0) {
        const dsdneoRuntimeConfig* cfg_clk = dsd_neo_get_config();
        if (cfg_clk && cfg_clk->c4fm_clk_is_set) {
            clk_mode = cfg_clk->c4fm_clk_mode;
        } else {
            /* One-time env fallback */
            static int init_clk_env = 0;
            static int env_mode = 0;
            if (!init_clk_env) {
                init_clk_env = 1;
                const char* clk = getenv("DSD_NEO_C4FM_CLK");
                if (clk) {
                    if (strcasecmp(clk, "el") == 0 || strcmp(clk, "1") == 0) {
                        env_mode = 1;
                    } else if (strcasecmp(clk, "mm") == 0 || strcmp(clk, "2") == 0) {
                        env_mode = 2;
                    } else {
                        env_mode = 0;
                    }
                }
            }
            clk_mode = env_mode;
        }
    }
#endif

    /* Optional auto-centering based on TED residual (RTL path only, C4FM, when not synced) */
#ifdef USE_RTLSDR
    maybe_auto_center(opts, state, have_sync);
    /* Align SPS to current RTL output rate if not 48 kHz */
    maybe_adjust_sps_for_output_rate(opts, state);
#endif

    /* Resolve any window freeze override once per symbol to avoid inner-loop overhead */
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int freeze_window = (cfg && cfg->window_freeze_is_set) ? (cfg->window_freeze != 0) : 0;

    /* Precompute left/right edges for current modulation once per symbol */
    int l_edge_pre = 0, r_edge_pre = 0;
    if (state->rf_mod == 0) {
        select_window_c4fm(state, &l_edge_pre, &r_edge_pre, freeze_window);
    } else if (state->rf_mod == 1) {
        select_window_qpsk(&l_edge_pre, &r_edge_pre, freeze_window);
    } else {
        select_window_gfsk(&l_edge_pre, &r_edge_pre, freeze_window);
    }

    for (i = 0; i < state->samplesPerSymbol; i++) //right HERE
    {

        // timing control
        if ((i == 0) && (have_sync == 0)) {
            if (state->samplesPerSymbol == 20) {
                if ((state->jitter >= 7) && (state->jitter <= 10)) {
                    i--;
                } else if ((state->jitter >= 11) && (state->jitter <= 14)) {
                    i++;
                }
            } else if (state->rf_mod == 1) {
                if ((state->jitter >= 0) && (state->jitter < state->symbolCenter)) {
                    i++; // fall back
                } else if ((state->jitter > state->symbolCenter) && (state->jitter < 10)) {
                    i--; // catch up
                }
            } else if (state->rf_mod == 2) {
                if ((state->jitter >= state->symbolCenter - 1) && (state->jitter <= state->symbolCenter)) {
                    i--;
                } else if ((state->jitter >= state->symbolCenter + 1) && (state->jitter <= state->symbolCenter + 2)) {
                    i++;
                }
            } else if (state->rf_mod == 0) {
                if ((state->jitter > 0) && (state->jitter <= state->symbolCenter)) {
                    i--; // catch up
                } else if ((state->jitter > state->symbolCenter) && (state->jitter < state->samplesPerSymbol)) {
                    i++; // fall back
                }
            }
            state->jitter = -1;
        }

        // Read the new sample from the input
        if (opts->audio_in_type == 0) //pulse audio
        {
            pa_simple_read(opts->pulse_digi_dev_in, &sample, 2, NULL);
            if (opts->input_volume_multiplier > 1) {
                int v = (int)sample * opts->input_volume_multiplier;
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                sample = (short)v;
            }
        }

        else if (opts->audio_in_type == 5) //OSS
        {
            read(opts->audio_in_fd, &sample, 2);
        }

        //stdin only, wav files moving to new number
        else if (opts->audio_in_type == 1) //won't work in windows, needs posix pipe (mintty)
        {
            result = sf_read_short(opts->audio_in_file, &sample, 1);
            if (opts->input_volume_multiplier > 1) {
                int v = (int)sample * opts->input_volume_multiplier;
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                sample = (short)v;
            }
            if (result == 0) {
                sf_close(opts->audio_in_file);
                cleanupAndExit(opts, state);
            }
        }
        //wav files, same but using seperate value so we can still manipulate ncurses menu
        //since we can not worry about getch/stdin conflict
        else if (opts->audio_in_type == 2) {
            result = sf_read_short(opts->audio_in_file, &sample, 1);
            if (opts->input_volume_multiplier > 1) {
                int v = (int)sample * opts->input_volume_multiplier;
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                sample = (short)v;
            }
            if (result == 0) {

                sf_close(opts->audio_in_file);
                fprintf(stderr, "\nEnd of %s\n", opts->audio_in_dev);
                //open pulse input if we are pulse output AND using ncurses terminal
                if (opts->audio_out_type == 0 && opts->use_ncurses_terminal == 1) {
                    opts->audio_in_type = 0; //set input type
                    openPulseInput(opts);    //open pulse input
                }
                //else cleanup and exit
                else {
                    cleanupAndExit(opts, state);
                }
            }
        } else if (opts->audio_in_type == 3) {
#ifdef USE_RTLSDR
            // Read demodulated stream here
            if (!g_rtl_ctx) {
                cleanupAndExit(opts, state);
            }
            int got = 0;
            if (rtl_stream_read(g_rtl_ctx, &sample, 1, &got) < 0 || got != 1) {
                cleanupAndExit(opts, state);
            }
            //update root means square power level
            opts->rtl_pwr = rtl_stream_return_pwr(g_rtl_ctx);
            sample *= opts->rtl_volume_multiplier;

#endif
        }

        //tcp socket input from SDR++ -- now with 1 retry if connection is broken
        else if (opts->audio_in_type == 8) {
#ifdef __CYGWIN__
            result = sf_read_short(opts->tcp_file_in, &sample, 1);
            if (opts->input_volume_multiplier > 1) {
                int v = (int)sample * opts->input_volume_multiplier;
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                sample = (short)v;
            }
            if (result == 0) {
                fprintf(stderr, "\nConnection to TCP Server Interrupted. Trying again in 3 seconds.\n");
                sample = 0;
                sf_close(opts->tcp_file_in); //close current connection on this end
                sleep(3);                    //halt all processing and wait 3 seconds

                //attempt to reconnect to socket
                opts->tcp_sockfd = 0;
                opts->tcp_sockfd = Connect(opts->tcp_hostname, opts->tcp_portno);
                if (opts->tcp_sockfd != 0) {
                    //reset audio input stream
                    opts->audio_in_file_info = calloc(1, sizeof(SF_INFO));
                    opts->audio_in_file_info->samplerate = opts->wav_sample_rate;
                    opts->audio_in_file_info->channels = 1;
                    opts->audio_in_file_info->seekable = 0;
                    opts->audio_in_file_info->format = SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
                    opts->tcp_file_in = sf_open_fd(opts->tcp_sockfd, SFM_READ, opts->audio_in_file_info, 0);

                    if (opts->tcp_file_in == NULL) {
                        fprintf(stderr, "Error, couldn't Reconnect to TCP with libsndfile: %s\n", sf_strerror(NULL));
                    } else {
                        LOG_INFO("TCP Socket Reconnected Successfully.\n");
                    }
                } else {
                    LOG_ERROR("TCP Socket Connection Error.\n");
                }

                //now retry reading sample
                result = sf_read_short(opts->tcp_file_in, &sample, 1);
                if (result == 0) {
                    sf_close(opts->tcp_file_in);
                    opts->audio_in_type = 0; //set input type
                    opts->tcp_sockfd =
                        0; //added this line so we will know if it connected when using ncurses terminal keyboard shortcut
                    //openPulseInput(opts); //open pulse inpput
                    sample = 0; //zero sample on bad result, keep the ball rolling
                    //open pulse input if we are pulse output AND using ncurses terminal
                    if (opts->audio_out_type == 0 && opts->use_ncurses_terminal == 1) {
                        fprintf(stderr, "Connection to TCP Server Disconnected.\n");
                        fprintf(stderr, "Opening Pulse Audio Input.\n");
                        opts->audio_in_type = 0; //set input type
                        openPulseInput(opts);    //open pulse input
                    }
                    //else cleanup and exit
                    else {
                        fprintf(stderr, "Connection to TCP Server Disconnected.\n");
                        fprintf(stderr, "Closing DSD-neo.\n");
                        cleanupAndExit(opts, state);
                    }
                }
            }
#else
            result = sf_read_short(opts->tcp_file_in, &sample, 1);
            if (opts->input_volume_multiplier > 1) {
                int v = (int)sample * opts->input_volume_multiplier;
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                sample = (short)v;
            }
            if (result == 0) {
            TCP_RETRY:
                if (exitflag == 1) {
                    cleanupAndExit(opts, state); //needed to break the loop on ctrl+c
                }
                // shorter backoff on TCP input stall to avoid wedging decode/SM
                int backoff_ms = 300; // default 300ms
                const char* eb = getenv("DSD_NEO_TCPIN_BACKOFF_MS");
                if (eb && eb[0] != '\0') {
                    int v = atoi(eb);
                    if (v >= 50 && v <= 5000) {
                        backoff_ms = v;
                    }
                }
                fprintf(stderr, "\nConnection to TCP Server Interrupted. Trying again in %d ms.\n", backoff_ms);
                sample = 0;
                sf_close(opts->tcp_file_in); //close current connection on this end
                // short throttle to avoid busy loop, but keep UI/SM responsive
                struct timespec ts = {backoff_ms / 1000, (backoff_ms % 1000) * 1000000L};
                nanosleep(&ts, NULL);

                //attempt to reconnect to socket
                opts->tcp_sockfd = 0;
                opts->tcp_sockfd = Connect(opts->tcp_hostname, opts->tcp_portno);
                if (opts->tcp_sockfd != 0) {
                    //reset audio input stream
                    opts->audio_in_file_info = calloc(1, sizeof(SF_INFO));
                    opts->audio_in_file_info->samplerate = opts->wav_sample_rate;
                    opts->audio_in_file_info->channels = 1;
                    opts->audio_in_file_info->seekable = 0;
                    opts->audio_in_file_info->format = SF_FORMAT_RAW | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
                    opts->tcp_file_in = sf_open_fd(opts->tcp_sockfd, SFM_READ, opts->audio_in_file_info, 0);

                    if (opts->tcp_file_in == NULL) {
                        fprintf(stderr, "Error, couldn't Reconnect to TCP with libsndfile: %s\n", sf_strerror(NULL));
                    } else {
                        LOG_INFO("TCP Socket Reconnected Successfully.\n");
                    }
                } else {
                    LOG_ERROR("TCP Socket Connection Error.\n");
                    // If using M17 or other keepalive mode, keep trying quickly
                    if (opts->frame_m17 == 1) {
                        goto TCP_RETRY;
                    }
                }

                //now retry reading sample
                result = sf_read_short(opts->tcp_file_in, &sample, 1);
                if (result == 0) {
                    sf_close(opts->tcp_file_in);
                    opts->audio_in_type = 0; //set input type
                    opts->tcp_sockfd =
                        0; //added this line so we will know if it connected when using ncurses terminal keyboard shortcut
                    openPulseInput(opts); //open pulse inpput
                    sample = 0;           //zero sample on bad result, keep the ball rolling
                    fprintf(stderr, "Connection to TCP Server Disconnected.\n");
                }
            }
#endif
        }

        // UDP direct audio input (PCM16LE over UDP)
        else if (opts->audio_in_type == 6) {
            if (!udp_input_read_sample(opts, &sample)) {
                cleanupAndExit(opts, state);
            }
            if (opts->input_volume_multiplier > 1) {
                int v = (int)sample * opts->input_volume_multiplier;
                if (v > 32767) {
                    v = 32767;
                } else if (v < -32768) {
                    v = -32768;
                }
                sample = (short)v;
            }
        }

        //BUG REPORT: 1. DMR Simplex doesn't work with raw wav files. 2. Using the monitor w/ wav file saving may produce undecodable wav files.
        //reworked a bit to allow raw audio wav file saving without the monitoring poriton active
        if (have_sync == 0) {

            //do an extra checkfor carrier signal so that random raw audio spurts don't play during decoding
            // if ( (state->carrier == 1) && ((time(NULL) - state->last_vc_sync_time) < 2)) /This probably doesn't work correctly since we update time check when playing raw audio
            // {
            //   memset (state->analog_out, 0, sizeof(state->analog_out));
            //   state->analog_sample_counter = 0;
            // } //This is the root cause of issue listed above, will evaluate further at a later time for a more elegant solution, or determine if anything is negatively impacted by removing this

            //sanity check to prevent an overflow
            if (state->analog_sample_counter > 959) {
                state->analog_sample_counter = 959;
            }

            state->analog_out[state->analog_sample_counter++] = sample;

            if (state->analog_sample_counter == 960) {
                //measure input power for non-RTL inputs
                if (opts->audio_in_type != 3) {
                    opts->rtl_pwr = raw_pwr(state->analog_out, 960, 1);
                    // Optional: warn on persistently low input level
                    if (opts->input_warn_db < 0.0) {
                        double db = pwr_to_dB(opts->rtl_pwr);
                        time_t now = time(NULL);
                        if (db <= opts->input_warn_db
                            && (opts->last_input_warn_time == 0
                                || (int)(now - opts->last_input_warn_time) >= opts->input_warn_cooldown_sec)) {
                            LOG_WARNING(
                                "Input level low (%.1f dBFS). Consider raising sender gain or use --input-volume.\n",
                                db);
                            opts->last_input_warn_time = now;
                        }
                    }
                }

                //raw wav file saving -- only write when not NXDN, dPMR, or M17 due to noise that can cause tons of false positives when no sync
                if (opts->wav_out_raw != NULL && opts->frame_nxdn48 == 0 && opts->frame_nxdn96 == 0
                    && opts->frame_dpmr == 0 && opts->frame_m17 == 0) {
                    sf_write_short(opts->wav_out_raw, state->analog_out, 960);
                    sf_write_sync(opts->wav_out_raw);
                }

                //low pass filter
                if (opts->use_lpf == 1) {
                    lpf(state, state->analog_out, 960);
                }

                //high pass filter
                if (opts->use_hpf == 1) {
                    hpf(state, state->analog_out, 960);
                }

                //pass band filter
                if (opts->use_pbf == 1) {
                    pbf(state, state->analog_out, 960);
                }

                //manual gain control
                if (opts->audio_gainA > 0.0f) {
                    analog_gain(opts, state, state->analog_out, 960);
                }

                //automatic gain control
                else {
                    agsm(opts, state, state->analog_out, 960);
                }

                //Running PWR after filtering does remove the analog spike from the PWR value
                //but noise floor noise will still produce higher values
                // if (opts->audio_in_type != 3  && opts->monitor_input_audio == 1)
                //   opts->rtl_pwr = raw_pwr(state->analog_out, 960, 1);

                //seems to be working now, but PWR values are lower on actual analog signal than on no signal but noise
                if ((opts->rtl_pwr > opts->rtl_squelch_level) && opts->monitor_input_audio == 1 && state->carrier == 0
                    && opts->audio_out == 1) //added carrier check here in lieu of disabling it above
                {
                    if (opts->audio_out_type == 0) {
                        pa_simple_write(opts->pulse_raw_dev_out, state->analog_out, (size_t)960u * sizeof(short), NULL);
                    }

                    if (opts->audio_out_type == 8) {
                        udp_socket_blasterA(opts, state, (size_t)960u * sizeof(short), state->analog_out);
                    }

                    //NOTE: Worked okay earlier in Cygwin, so should be fine -- can only operate at 48k1, else slow mode lag

                    //This one will only operate when OSS 48k1 (when both input and output are OSS audio)
                    if (opts->audio_out_type == 5 && opts->pulse_digi_rate_out == 48000
                        && opts->pulse_digi_out_channels == 1) {
                        write(opts->audio_out_fd, state->analog_out, (size_t)960u * sizeof(short));
                    }

                    //STDOUT, but only when operating at 48k1 (no go just yet)
                    // if (opts->audio_out_type == 1 && opts->pulse_digi_rate_out == 48000 && opts->pulse_digi_out_channels == 1)
                    //   write (opts->audio_out_fd, state->analog_out, 960*2);

                    //OSS 8k1 (no go just yet)
                    // if (opts->audio_out_type == 2 && opts->pulse_digi_rate_out == 48000 && opts->pulse_digi_out_channels == 1)
                    //   write (opts->audio_out_fd, state->analog_out, 960*2);

                    // UI/scan heartbeat: avoid refreshing timers that the
                    // trunk SM depends on for hangtime and CC hunting logic.
                    // - Do NOT refresh last_cc_sync_time while trunking is
                    //   enabled and we are not voice‑tuned; the SM needs that
                    //   timer to age so CC hunting can start.
                    // - Do NOT refresh last_vc_sync_time while voice‑tuned; it
                    //   must reflect actual digital voice activity only.
                    if (opts->p25_trunk != 1) {
                        state->last_cc_sync_time = time(NULL);
                        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
                    }
                    if (!(opts->p25_trunk == 1 && opts->p25_is_tuned == 1)) {
                        state->last_vc_sync_time = time(NULL);
                        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
                    }
                }

                //raw wav file saving -- save the WAV file samples before we apply filtering to them
                // if (opts->wav_out_raw != NULL && opts->frame_nxdn48 == 0 && opts->frame_nxdn96 == 0 && opts->frame_dpmr == 0 && opts->frame_m17 == 0)
                // {
                //   sf_write_short(opts->wav_out_raw, state->analog_out, 960);
                //   sf_write_sync (opts->wav_out_raw);
                // }

                memset(state->analog_out, 0, sizeof(state->analog_out));
                state->analog_sample_counter = 0;
            }
        }

        if (have_sync == 1) {
            //sanity check to prevent an overflow
            if (state->analog_sample_counter > 959) {
                state->analog_sample_counter = 959;
            }

            state->analog_out[state->analog_sample_counter++] = sample;

            if (state->analog_sample_counter == 960) {
                //raw wav file saving -- file size on this blimps pretty fast 1 min ~= 6 MB;  1 hour ~= 360 MB;
                if (opts->wav_out_raw != NULL) {
                    sf_write_short(opts->wav_out_raw, state->analog_out, 960);
                    sf_write_sync(opts->wav_out_raw);
                }

                //zero out and reset counter
                memset(state->analog_out, 0, sizeof(state->analog_out));
                state->analog_sample_counter = 0;
            }
        }

        if (opts->use_cosine_filter) {
            if ((state->lastsynctype >= 10 && state->lastsynctype <= 13) || state->lastsynctype == 32
                || state->lastsynctype == 33 || state->lastsynctype == 34 || state->lastsynctype == 30
                || state->lastsynctype == 31) {
                sample = dmr_filter(sample);
            }

            else if (state->lastsynctype == 8 || state->lastsynctype == 9 || state->lastsynctype == 16
                     || state->lastsynctype == 17 || state->lastsynctype == 86 || state->lastsynctype == 87
                     || state->lastsynctype == 98 || state->lastsynctype == 99) {
                sample = m17_filter(sample);
            }

            // Apply matched filter to P25 Phase 1 (C4FM)
            else if (state->lastsynctype == 0 || state->lastsynctype == 1) {
                // Fixed RRC(alpha=0.5) with ntaps = 11*sps + 1 (e.g., 111 at sps=10)
                sample = c4fm_rrc_filter_alpha05_sample(sample, state->samplesPerSymbol);
            }

            else if (state->lastsynctype == 20 || state->lastsynctype == 21 || state->lastsynctype == 22
                     || state->lastsynctype == 23 || state->lastsynctype == 24 || state->lastsynctype == 25
                     || state->lastsynctype == 26 || state->lastsynctype == 27 || state->lastsynctype == 28
                     || state->lastsynctype == 29) //||
            //state->lastsynctype == 35 || state->lastsynctype == 36) //phase 2 C4FM disc tap input
            {
                //if(state->samplesPerSymbol == 20)
                if (opts->frame_nxdn48 == 1) {
                    sample = nxdn_filter(sample);
                }
                //else if (state->lastsynctype >= 20 && state->lastsynctype <=27) //this the right range?
                else if (opts->frame_dpmr == 1) {
                    sample = dpmr_filter(sample);
                } else if (state->samplesPerSymbol == 8) //phase 2 cqpsk
                {
                    //sample = dmr_filter(sample); //work on filter later
                } else {
                    sample = dmr_filter(sample);
                }
            }
        }

        if ((sample > state->max) && (have_sync == 1) && (state->rf_mod == 0)) {
            sample = state->max;
        } else if ((sample < state->min) && (have_sync == 1) && (state->rf_mod == 0)) {
            sample = state->min;
        }

        if (sample > state->center) {
            if (state->lastsample < state->center) {
                state->numflips += 1;
            }
            if (sample > (state->maxref * 1.25)) {
                if (state->lastsample < (state->maxref * 1.25)) {
                    state->numflips += 1;
                }
                if ((state->jitter < 0) && (state->rf_mod == 1)) { // first spike out of place
                    state->jitter = i;
                }
                if ((opts->symboltiming == 1) && (have_sync == 0) && (state->lastsynctype != -1)) {
                    fprintf(stderr, "O");
                }
            } else {
                if ((opts->symboltiming == 1) && (have_sync == 0) && (state->lastsynctype != -1)) {
                    fprintf(stderr, "+");
                }
                if ((state->jitter < 0) && (state->lastsample < state->center)
                    && (state->rf_mod != 1)) { // first transition edge
                    state->jitter = i;
                }
            }
        } else { // sample < 0
            if (state->lastsample > state->center) {
                state->numflips += 1;
            }
            if (sample < (state->minref * 1.25)) {
                if (state->lastsample > (state->minref * 1.25)) {
                    state->numflips += 1;
                }
                if ((state->jitter < 0) && (state->rf_mod == 1)) { // first spike out of place
                    state->jitter = i;
                }
                if ((opts->symboltiming == 1) && (have_sync == 0) && (state->lastsynctype != -1)) {
                    fprintf(stderr, "X");
                }
            } else {
                if ((opts->symboltiming == 1) && (have_sync == 0) && (state->lastsynctype != -1)) {
                    fprintf(stderr, "-");
                }
                if ((state->jitter < 0) && (state->lastsample > state->center)
                    && (state->rf_mod != 1)) { // first transition edge
                    state->jitter = i;
                }
            }
        }

        if (state->samplesPerSymbol == 20) //nxdn 4800 baud 2400 symbol rate
        {
            // if ((i >= 9) && (i <= 11))
            if ((i >= 7) && (i <= 13)) //7, 13 working good on multiple nxdn48, fewer random errors
            {
                sum += sample;
                count++;
            }
        }
        if (state->samplesPerSymbol == 5) //provoice or gfsk
        {
            if (i == 2) {
                sum += sample;
                count++;
            }
        } else {
            if (state->rf_mod == 0) {
                // 0: C4FM modulation — take single sample at matched-filter peak
                if (i == state->symbolCenter) {
                    sum += sample;
                    count++;
                }

#ifdef TRACE_DSD
                if (i == state->symbolCenter - 1) {
                    state->debug_sample_left_edge = state->debug_sample_index - 1;
                }
                if (i == state->symbolCenter + 2) {
                    state->debug_sample_right_edge = state->debug_sample_index - 1;
                }
#endif
            } else { // QPSK or GFSK share the same 2-sample window
                // Use symmetric two-sample window (precomputed)
                if ((i == state->symbolCenter - l_edge_pre) || (i == state->symbolCenter + r_edge_pre)) {
                    sum += sample;
                    count++;
                }
            }
        }

        state->lastsample = sample;

#ifdef USE_RTLSDR
        if (clk_mode && state->rf_mod == 0) {
            int c = state->symbolCenter;
            if (i == c - 1) {
                clk_early = sample;
            } else if (i == c) {
                clk_mid = sample;
            } else if (i == c + 1) {
                clk_late = sample;
            }
        }
#endif
    }

    if (count > 0) {
        symbol = (sum / count);
    } else {
        symbol = 0;
    }

    if ((opts->symboltiming == 1) && (have_sync == 0) && (state->lastsynctype != -1)) {
        if (state->jitter >= 0) {
            fprintf(stderr, " %i\n", state->jitter);
        } else {
            fprintf(stderr, "\n");
        }
    }

#ifdef TRACE_DSD
    if (state->samplesPerSymbol == 10) {
        float left, right;
        if (state->debug_label_file == NULL) {
            state->debug_label_file = fopen("pp_label.txt", "w");
        }
        left = state->debug_sample_left_edge / SAMPLE_RATE_IN;
        right = state->debug_sample_right_edge / SAMPLE_RATE_IN;
        if (state->debug_prefix != '\0') {
            if (state->debug_prefix == 'I') {
                fprintf(state->debug_label_file, "%f\t%f\t%c%c %i\n", left, right, state->debug_prefix,
                        state->debug_prefix_2, symbol);
            } else {
                fprintf(state->debug_label_file, "%f\t%f\t%c %i\n", left, right, state->debug_prefix, symbol);
            }
        } else {
            fprintf(state->debug_label_file, "%f\t%f\t%i\n", left, right, symbol);
        }
    }
#endif

    //read dibit capture bin files
    if (opts->audio_in_type == 4) {
        //use fopen and read in a symbol, check op25 for clues
        if (opts->symbolfile == NULL) {
            fprintf(stderr, "Error Opening File %s\n", opts->audio_in_dev); //double check this
            return (-1);
        }

        state->symbolc = fgetc(opts->symbolfile);

        //fprintf(stderr, "%d", state->symbolc);
        if (feof(opts->symbolfile)) {
            // opts->audio_in_type = 0; //switch to pulse after playback, ncurses terminal can initiate replay if wanted
            fclose(opts->symbolfile);
            fprintf(stderr, "\nEnd of %s\n", opts->audio_in_dev);
            //in debug mode, re-run .bin files over and over (look for memory leaks, etc)
            if (state->debug_mode == 1) {
                opts->symbolfile = NULL;
                opts->symbolfile = fopen(opts->audio_in_dev, "r");
                opts->audio_in_type = 4; //symbol capture bin files
            }
            //open pulse input if we are pulse output AND using ncurses terminal
            else if (opts->audio_out_type == 0 && opts->use_ncurses_terminal == 1) {
                opts->audio_in_type = 0; //set input type
                openPulseInput(opts);    //open pulse input
            }
            //else cleanup and exit
            else {
                cleanupAndExit(opts, state);
            }
        }

        //assign symbol/dibit values based on modulation type
        if (state->rf_mod == 2) //GFSK
        {
            symbol = state->symbolc;
            if (state->symbolc == 0) {
                symbol = -3; //-1
            }
            if (state->symbolc == 1) {
                symbol = -1; //-3
            }
        } else //everything else
        {
            if (state->symbolc == 0) {
                symbol = 1; //-1
            }
            if (state->symbolc == 1) {
                symbol = 3; //-3
            }
            if (state->symbolc == 2) {
                symbol = -1; //1
            }
            if (state->symbolc == 3) {
                symbol = -3; //3
            }
        }
    }

    //.raw or .sym float symbol files
    if (opts->audio_in_type == 44) {
        float float_symbol = 0.0f;
        fread(&float_symbol, sizeof(float), 1, opts->symbolfile); //sizeof(float) is 4 (usually)
        if (feof(opts->symbolfile)) {
            exitflag = 1; //end of file, exit
        }
        // float_symbol = -float_symbol; //inversion
        symbol = (int)float_symbol * (int)10000;
    }

    /* Apply C4FM symbol-domain DD equalizer (prototype) just before handing
       the symbol to the slicer. Keeps adaptation one symbol behind using the
       previous decision stored in state. */
    do {
        if (state->rf_mod != 0) {
            break; /* only C4FM */
        }
        /* Refresh from runtime config to honor UI toggles */
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg) {
            if (cfg->c4fm_dd_eq_is_set) {
                state->c4fm_dd_eq_enable = cfg->c4fm_dd_eq_enable ? 1 : 0;
            }
            if (cfg->c4fm_dd_eq_taps_is_set) {
                state->c4fm_dd_eq_taps = cfg->c4fm_dd_eq_taps;
                state->c4fm_dd_eq_hist_len = cfg->c4fm_dd_eq_taps;
            }
            if (cfg->c4fm_dd_eq_mu_is_set) {
                state->c4fm_dd_eq_mu_q15 = cfg->c4fm_dd_eq_mu_q15;
            }
        }
        static int dd_init = 0;
        if (!dd_init) {
            dd_init = 1;
            const char* en = getenv("DSD_NEO_C4FM_DD_EQ");
            const char* tp = getenv("DSD_NEO_C4FM_DD_EQ_TAPS");
            const char* mu = getenv("DSD_NEO_C4FM_DD_EQ_MU");
            if (en && en[0] == '1') {
                state->c4fm_dd_eq_enable = 1;
            }
            if (tp && tp[0] != '\0') {
                int v = atoi(tp);
                if (v < 3) {
                    v = 3;
                }
                if (v > 9) {
                    v = 9;
                }
                if ((v & 1) == 0) {
                    v++;
                }
                state->c4fm_dd_eq_taps = v;
                state->c4fm_dd_eq_hist_len = v;
            }
            if (mu && mu[0] != '\0') {
                int v = atoi(mu);
                if (v < 1) {
                    v = 1;
                }
                if (v > 64) {
                    v = 64;
                }
                state->c4fm_dd_eq_mu_q15 = v;
            }
        }
        if (!state->c4fm_dd_eq_enable) {
            break;
        }
        int L = state->c4fm_dd_eq_taps;
        if (L < 3) {
            L = 3;
        }
        if (L > 9) {
            L = 9;
        }
        if ((L & 1) == 0) {
            L++;
        }
        state->c4fm_dd_eq_hist_len = L;
        /* Adapt weights from last decided target */
        if (state->c4fm_dd_eq_have_last) {
            int e = state->c4fm_dd_eq_last_target - state->c4fm_dd_eq_last_y;
            int mu_q15 = state->c4fm_dd_eq_mu_q15 > 0 ? state->c4fm_dd_eq_mu_q15 : 2;
            for (int k = 0; k < L; k++) {
                int xk = state->c4fm_dd_eq_x_hist[k];
                long long dw = ((long long)mu_q15 * (long long)e * (long long)xk);
                dw >>= 23; /* heuristic scaling */
                long w = (long)state->c4fm_dd_eq_w_q15[k] + (long)dw;
                if (w > 98304) {
                    w = 98304; /* ~3.0 in Q15 */
                }
                if (w < -98304) {
                    w = -98304;
                }
                state->c4fm_dd_eq_w_q15[k] = (int)w;
            }
        }
        /* Shift in new symbol and compute y */
        for (int k = L - 1; k >= 1; k--) {
            state->c4fm_dd_eq_x_hist[k] = state->c4fm_dd_eq_x_hist[k - 1];
        }
        state->c4fm_dd_eq_x_hist[0] = symbol;
        long long acc = 0;
        for (int k = 0; k < L; k++) {
            acc += ((long long)state->c4fm_dd_eq_w_q15[k] * (long long)state->c4fm_dd_eq_x_hist[k]);
        }
        int y = (int)(acc >> 15);
        if (y > 32767) {
            y = 32767;
        }
        if (y < -32768) {
            y = -32768;
        }
        state->c4fm_dd_eq_last_y = y;
        state->c4fm_dd_eq_have_last = 1;
        symbol = y;
    } while (0);

    /* Apply C4FM clock assist after symbol decision (unsynced only) */
#ifdef USE_RTLSDR
    if (clk_mode && state->rf_mod == 0) {
        maybe_c4fm_clock(opts, state, have_sync, clk_mode, clk_early, clk_mid, clk_late);
    }
#endif

    state->symbolcnt++;
    return (symbol);
}
