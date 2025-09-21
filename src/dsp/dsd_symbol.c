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
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/runtime/config.h>
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
        if (state->synctype == 30 || state->synctype == 31) {
            l = 1; // YSF
        } else if ((state->lastsynctype >= 10 && state->lastsynctype <= 13) || state->lastsynctype == 32
                   || state->lastsynctype == 33) {
            l = 1; // DMR and some NXDN cases
        } else {
            l = 2; // P25 and NXDN96 prefer wider left window
        }
    }
    *l_edge = l;
    *r_edge = r;
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
/*
 * Nudge symbolCenter by ±1 based on a smoothed TED residual when available.
 * Guards against oscillation using a small deadband and a cooldown period.
 *
 * Safety gates:
 *  - Only when using RTL input (audio_in_type == 3)
 *  - Only when not currently synchronized (have_sync == 0)
 *  - Only for C4FM path (rf_mod == 0) to avoid QPSK/LSM perturbations
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
    if (have_sync != 0) {
        return; // do not adjust while synchronized
    }
    if (state->rf_mod != 0) {
        return; // limit to C4FM for now; avoid QPSK/LSM
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
        }

        else if (opts->audio_in_type == 5) //OSS
        {
            read(opts->audio_in_fd, &sample, 2);
        }

        //stdin only, wav files moving to new number
        else if (opts->audio_in_type == 1) //won't work in windows, needs posix pipe (mintty)
        {
            result = sf_read_short(opts->audio_in_file, &sample, 1);
            if (result == 0) {
                sf_close(opts->audio_in_file);
                cleanupAndExit(opts, state);
            }
        }
        //wav files, same but using seperate value so we can still manipulate ncurses menu
        //since we can not worry about getch/stdin conflict
        else if (opts->audio_in_type == 2) {
            result = sf_read_short(opts->audio_in_file, &sample, 1);
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
                        fprintf(stderr, "TCP Socket Reconnected Successfully.\n");
                    }
                } else {
                    fprintf(stderr, "TCP Socket Connection Error.\n");
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
            if (result == 0) {
            TCP_RETRY:
                if (exitflag == 1) {
                    cleanupAndExit(opts, state); //needed to break the loop on ctrl+c
                }
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
                        fprintf(stderr, "TCP Socket Reconnected Successfully.\n");
                    }
                } else {
                    fprintf(stderr, "TCP Socket Connection Error.\n");
                    if (opts->frame_m17 == 1) {
                        goto TCP_RETRY; //if using m17 encoder/decoder, just keep looping to keep alive
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
                //get an pwr value if not using the rtl built in version
                if (opts->audio_in_type != 3 && opts->monitor_input_audio == 1) {
                    opts->rtl_pwr = raw_pwr(state->analog_out, 960, 1);
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

                    //test updating the sync time, so we can hold here while trunking or scanning
                    state->last_cc_sync_time = time(NULL);
                    state->last_vc_sync_time = time(NULL);
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

            // Apply matched filter to P25 Phase 1 (C4FM) using legacy C4FM kernel
            else if (state->lastsynctype == 0 || state->lastsynctype == 1) {
                sample = p25_c4fm_filter(sample);
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
                // 0: C4FM modulation
                if ((i >= state->symbolCenter - l_edge_pre) && (i <= state->symbolCenter + r_edge_pre)) {
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
            } else if (state->rf_mod == 1) //QPSK
            {
                // Two-sample window: one left, two right (precomputed)
                if ((i == state->symbolCenter - l_edge_pre) || (i == state->symbolCenter + r_edge_pre)) {
                    sum += sample;
                    count++;
                }
            } else //GFSK
            {
                // 1: QPSK modulation
                // 2: GFSK modulation
                // Note: this has been changed to use an additional symbol to the left
                // On the p25_raw_unencrypted.flac it is evident that the timing
                // comes one sample too late.
                // This change makes a significant improvement in the BER, at least for
                // this file.
                // if ((i == state->symbolCenter) || (i == state->symbolCenter + 1))
                // GFSK should ideally be symmetric (precomputed)
                if ((i == state->symbolCenter - l_edge_pre) || (i == state->symbolCenter + r_edge_pre)) {
                    sum += sample;
                    count++;
                }

                //MISC Notes: When using RTL input, NXDN96 at BW:16 NXDN48 at BW:12
                //I am beginning to suspect that RTL input is halfing the bandwidth, which
                //is my original consclusion back when I was working on it last, unsure
                //or the best course on that, could just be a rtl_fm flaw
                //P25 may also work better at 16 as well now, hard to tell the difference since both are good
                //DMR may not be any different on 12 or 16
                //most likely though, this all will just depend on signal stregth more than anything
                //as to how much BW you should set

#ifdef TRACE_DSD
                //if (i == state->symbolCenter) {
                if (i == state->symbolCenter - 1) {
                    state->debug_sample_left_edge = state->debug_sample_index - 1;
                }
                if (i == state->symbolCenter + 1) {
                    state->debug_sample_right_edge = state->debug_sample_index - 1;
                }
#endif
            }
        }

        state->lastsample = sample;
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

    state->symbolcnt++;
    return (symbol);
}
