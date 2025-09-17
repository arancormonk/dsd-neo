// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR stream orchestration and demodulation pipeline.
 *
 * Sets up the RTL-SDR device and worker threads, configures capture
 * settings and the demodulation pipeline, manages rings and UDP control,
 * and exposes a consumer API for audio samples and tuning.
 */

#include <atomic>
#include <chrono>
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/polar_disc.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_control.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <dsd-neo/runtime/worker_pool.h>
#include <math.h>
#include <pthread.h>
#include <rtl-sdr.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Forward declarations for internal helpers used by shims */
void dsd_rtl_stream_clear_output(void);
long int dsd_rtl_stream_return_pwr(void);
unsigned int dsd_rtl_stream_output_rate(void);
int dsd_rtl_stream_ted_bias(void);
#ifdef __cplusplus
}
#endif

/* Forward declaration for eye ring append used in demod loop */
static inline void eye_ring_append_i_chan(const int16_t* iq_interleaved, int len_interleaved);

#define DEFAULT_SAMPLE_RATE      48000
#define DEFAULT_BUF_LENGTH       (1 * 16384)
#define MAXIMUM_OVERSAMPLE       16
#define MAXIMUM_BUF_LENGTH       (MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#define AUTO_GAIN                -100
#define BUFFER_DUMP              4096

#define FREQUENCIES_LIMIT        1000

/* Clamp for bandwidth upsampling multiplier to avoid extreme expansion */
#define MAX_BANDWIDTH_MULTIPLIER 8

static int lcm_post[17] = {1, 1, 1, 3, 1, 5, 3, 7, 1, 9, 5, 11, 3, 13, 7, 15, 1};
static int ACTUAL_BUF_LENGTH;

static const double kPi = 3.14159265358979323846;

#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_PRAGMA(x) _Pragma(#x)
#define DSD_NEO_IVDEP     DSD_NEO_PRAGMA(GCC ivdep)

/**
 * @brief Hint that a pointer is aligned to a compile-time boundary for vectorization.
 *
 * This is a lightweight wrapper over compiler intrinsics to improve
 * auto-vectorization by promising the compiler that the pointer meets the
 * specified alignment. Use with care and only when the alignment guarantee
 * is actually met.
 *
 * @tparam T Element type of the pointer.
 * @param p  Pointer to memory that is at least `align_unused` aligned.
 * @param align_unused Alignment in bytes (ignored at runtime; for readability).
 * @return Pointer `p` with alignment assumption applied.
 */
template <typename T>
static inline T*
assume_aligned_ptr(T* p, size_t /*align_unused*/) {
    return (T*)__builtin_assume_aligned(p, 64);
}

template <typename T>
static inline const T*
assume_aligned_ptr(const T* p, size_t /*align_unused*/) {
    return (const T*)__builtin_assume_aligned(p, 64);
}
#else
#define DSD_NEO_IVDEP

/**
 * @brief See aligned variant: noop fallback when compiler does not support alignment
 * assumptions.
 * @tparam T Element type of the pointer.
 * @param p  Pointer to return as-is.
 * @param align_unused Unused parameter for signature compatibility.
 * @return Pointer `p` unchanged.
 */
template <typename T>
static inline T*
assume_aligned_ptr(T* p, size_t /*align_unused*/) {
    return p;
}

template <typename T>
static inline const T*
assume_aligned_ptr(const T* p, size_t /*align_unused*/) {
    return p;
}
#endif
#ifndef DSD_NEO_ALIGN
#define DSD_NEO_ALIGN 64
#endif

/* Compiler-friendly restrict qualifier */
#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_RESTRICT __restrict__
#else
#define DSD_NEO_RESTRICT
#endif

int fll_lut_enabled = 0; /* DSD_NEO_FLL_LUT (0 default: use fast approx) */
/* Debug/compat toggles via env */
static int combine_rotate_enabled = 1;      /* DSD_NEO_COMBINE_ROT (1 default) */
static int upsample_fixedpoint_enabled = 1; /* DSD_NEO_UPSAMPLE_FP (1 default) */

/* Runtime flag (default enabled). Set DSD_NEO_HB_DECIM=0 to use legacy decimator */
int use_halfband_decimator = 1;
/* Allow disabling the fs/4 capture frequency shift via env for trunking/exact-center use cases */
static int disable_fs4_shift = 0; /* Set by env DSD_NEO_DISABLE_FS4_SHIFT=1 */

// UDP control handle
static struct udp_control* g_udp_ctrl = NULL;

int rtl_bandwidth;
int bandwidth_multiplier;
int bandwidth_divisor =
    48000; // multiplier = bandwidth_divisor / rtl_bandwidth; clamped to [1, MAX_BANDWIDTH_MULTIPLIER]

short int volume_multiplier;
uint16_t port;

struct dongle_state {
    int exit_flag;
    pthread_t thread;
    rtlsdr_dev_t* dev;
    int dev_index;
    uint32_t freq;
    uint32_t rate;
    int gain;
    uint32_t buf_len;
    int ppm_error;
    int offset_tuning;
    int direct_sampling;
    std::atomic<int> mute;
    struct demod_state* demod_target;
};

struct demod_mt_worker_arg {
    struct demod_state* s;
    int id;
};

struct controller_state {
    int exit_flag;
    pthread_t thread;
    uint32_t freqs[FREQUENCIES_LIMIT];
    int freq_len;
    int freq_now;
    int edge;
    int wb_mode;
    pthread_cond_t hop;
    pthread_mutex_t hop_m;
    /* Marshalled retune request from external threads (UDP/API). */
    std::atomic<int> manual_retune_pending;
    uint32_t manual_retune_freq;
};

struct rtl_device* rtl_device_handle = NULL;
struct dongle_state dongle;
struct demod_state demod;
struct output_state output;
struct controller_state controller;
static struct input_ring_state input_ring;

struct RtlSdrInternals {
    struct rtl_device* device;
    struct dongle_state* dongle;
    struct demod_state* demod;
    struct output_state* output;
    struct controller_state* controller;
    struct input_ring_state* input_ring;
    struct udp_control** udp_ctrl_ptr;
    const dsdneoRuntimeConfig* cfg;
    /* Cooperative shutdown flag for threads launched by this stream */
    std::atomic<int> should_exit;
};

static struct RtlSdrInternals* g_stream = NULL;

/**
 * @brief On retune/hop, drain audio output ring for a short time to avoid
 * cutting off transmissions. If configured to clear, force-clear instead.
 */
static void
drain_output_on_retune(void) {
    struct output_state* outp = &output;
    if (g_stream && g_stream->output) {
        outp = g_stream->output;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int force_clear = 0;
    int drain_ms = 50;
    if (cfg) {
        if (cfg->output_clear_on_retune_is_set) {
            force_clear = (cfg->output_clear_on_retune != 0);
        }
        if (cfg->retune_drain_ms_is_set) {
            drain_ms = cfg->retune_drain_ms;
        }
    }
    if (drain_ms < 0) {
        drain_ms = 0;
    }
    if (force_clear || drain_ms == 0) {
        dsd_rtl_stream_clear_output();
        return;
    }
    size_t before = ring_used(outp);
    int waited_ms = 0;
    while (!ring_is_empty(outp) && waited_ms < drain_ms) {
        usleep(1000);
        waited_ms++;
    }
    if (!ring_is_empty(outp)) {
        /* Timed out; clear remainder to avoid stale backlog */
        dsd_rtl_stream_clear_output();
    }
    (void)before; /* reserved for future diagnostics */
}

/**
 * @brief Reset demodulator state on retune/hop to avoid stale "lock"/bias.
 *
 * Clears squelch accumulators, FLL/TED integrators, deemphasis/audio LPF/DC
 * state, history buffers for HB/CIC paths, and resampler phase/history.
 * This ensures each new frequency starts from a neutral state.
 *
 * @param s Demodulator state to reset.
 */
static void
demod_reset_on_retune(struct demod_state* s) {
    if (!s) {
        return;
    }
    /* Squelch */
    s->squelch_hits = 0;
    s->squelch_running_power = 0;
    s->squelch_decim_phase = 0;
    s->prev_index = 0;
    s->prev_lpr_index = 0;
    s->now_lpr = 0;
    /* Clear any staged block so power API does not see stale data */
    s->lp_len = 0;
    memset(s->input_cb_buf, 0, sizeof(s->input_cb_buf));
    /* FLL */
    fll_init_state(&s->fll_state);
    s->fll_freq_q15 = 0;
    s->fll_phase_q15 = 0;
    s->fll_prev_r = 0;
    s->fll_prev_j = 0;
    /* TED */
    ted_init_state(&s->ted_state);
    s->ted_mu_q20 = 0;
    /* Deemphasis / audio LPF / DC */
    s->deemph_avg = 0;
    s->audio_lpf_state = 0;
    s->dc_avg = 0;
    /* HB histories */
    for (int st = 0; st < 10; st++) {
        memset(s->hb_hist_i[st], 0, sizeof(s->hb_hist_i[st]));
        memset(s->hb_hist_q[st], 0, sizeof(s->hb_hist_q[st]));
    }
    /* Legacy CIC-like histories */
    for (int st = 0; st < 10; st++) {
        memset(s->lp_i_hist[st], 0, sizeof(s->lp_i_hist[st]));
        memset(s->lp_q_hist[st], 0, sizeof(s->lp_q_hist[st]));
    }
    /* Resampler */
    s->resamp_phase = 0;
    s->resamp_hist_head = 0;
    if (s->resamp_hist && s->resamp_taps_per_phase > 0) {
        memset(s->resamp_hist, 0, (size_t)s->resamp_taps_per_phase * sizeof(int16_t));
    }
}

/**
 * @brief Demodulation worker: consume input ring, run pipeline, and produce audio.
 *
 * Reads baseband I/Q blocks from the input ring, invokes the full demodulation
 * pipeline, and writes audio samples to the output ring with optional
 * resampling or legacy upsampling. Runs until global exit flag is set.
 *
 * @param arg Pointer to `demod_state`.
 * @return NULL on exit.
 */
/* SNR buffers for different modulations */
static double g_snr_c4fm_db = -100.0;
static double g_snr_qpsk_db = -100.0;
static double g_snr_gfsk_db = -100.0;

static void*
demod_thread_fn(void* arg) {
    struct demod_state* d = static_cast<demod_state*>(arg);
    struct output_state* o = d->output_target;
    maybe_set_thread_realtime_and_affinity("DEMOD");
    int logged_once = 0;
    while (!exitflag && !(g_stream && g_stream->should_exit.load())) {
        /* Read a block from input ring */
        int got = input_ring_read_block(&input_ring, d->input_cb_buf, MAXIMUM_BUF_LENGTH);
        if (got <= 0) {
            continue;
        }
        d->lowpassed = d->input_cb_buf;
        d->lp_len = got;
        full_demod(d);
        /* Capture decimated I/Q for constellation view after DSP. */
        extern void constellation_ring_append(const int16_t* iq, int len, int sps_hint);
        constellation_ring_append(d->lowpassed, d->lp_len, d->ted_sps);
        /* Capture I-channel for eye diagram */
        eye_ring_append_i_chan(d->lowpassed, d->lp_len);

        /* Estimate SNR per modulation using post-filter samples */
        const int16_t* iq = d->lowpassed;
        const int n_iq = d->lp_len;
        const int sps = d->ted_sps;
        if (iq && n_iq >= 4 && sps >= 2) {
            const int pairs = n_iq / 2;
            const int mid = sps / 2;
            int win = sps / 10;
            if (win < 1) {
                win = 1;
            }
            if (win > mid) {
                win = mid;
            }
            /* QPSK/CQPSK: EVM-based SNR from equalizer symbol outputs */
            if (d->cqpsk_enable) {
                enum { MAXP = 2048 };

                static int16_t syms[(size_t)MAXP * 2];
                int n = cqpsk_eq_get_symbols(syms, MAXP);
                if (n > 32) {
                    double sum_mag = 0.0;
                    for (int i = 0; i < n; i++) {
                        double I = (double)syms[(size_t)(i << 1) + 0];
                        double Q = (double)syms[(size_t)(i << 1) + 1];
                        sum_mag += sqrt(I * I + Q * Q);
                    }
                    double r_mean = sum_mag / (double)n;
                    double a = r_mean / 1.41421356237; /* per-axis target amplitude */
                    double e2_sum = 0.0, t2_sum = 0.0;
                    for (int i = 0; i < n; i++) {
                        double I = (double)syms[(size_t)(i << 1) + 0];
                        double Q = (double)syms[(size_t)(i << 1) + 1];
                        double ti = (I >= 0.0) ? a : -a;
                        double tq = (Q >= 0.0) ? a : -a;
                        double ei = I - ti;
                        double eq = Q - tq;
                        e2_sum += ei * ei + eq * eq;
                        t2_sum += ti * ti + tq * tq;
                    }
                    if (t2_sum > 1e-9) {
                        double evm = sqrt(e2_sum / (double)n) / sqrt(t2_sum / (double)n);
                        if (evm < 1e-6) {
                            evm = 1e-6;
                        }
                        double snr = 20.0 * log10(1.0 / evm);
                        static double ema = -100.0;
                        if (ema < -50.0) {
                            ema = snr;
                        } else {
                            ema = 0.8 * ema + 0.2 * snr;
                        }
                        g_snr_qpsk_db = ema;
                    }
                }
            }
            if (sps >= 6 && sps <= 12) {
                /* FSK family: compute both 4-level (C4FM) and 2-level (GFSK-like) */
                enum { MAXS = 8192 };

                static int vals[(size_t)MAXS];
                int m = 0;
                for (int k = 0; k < pairs && m < MAXS; k++) {
                    int phase = k % sps;
                    if (phase >= mid - win && phase <= mid + win) {
                        vals[m++] = (int)iq[2 * k + 0]; /* I-channel */
                    }
                }
                if (m > 32) {
                    auto cmp_int = [](const void* a, const void* b) {
                        int ia = *(const int*)a, ib = *(const int*)b;
                        return (ia > ib) - (ia < ib);
                    };
                    qsort(vals, (size_t)m, sizeof(int), cmp_int);
                    int q1 = vals[(size_t)m / 4];
                    int q2 = vals[(size_t)m / 2];
                    int q3 = vals[(size_t)(3 * (size_t)m) / 4];
                    /* 4-level (C4FM-like) */
                    {
                        double sum[4] = {0, 0, 0, 0};
                        int cnt[4] = {0, 0, 0, 0};
                        for (int i = 0; i < m; i++) {
                            int v = vals[i];
                            int b = (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
                            sum[b] += v;
                            cnt[b]++;
                        }
                        if (cnt[0] && cnt[1] && cnt[2] && cnt[3]) {
                            double mu[4];
                            int total = 0;
                            for (int b = 0; b < 4; b++) {
                                mu[b] = sum[b] / (double)cnt[b];
                                total += cnt[b];
                            }
                            double nsum = 0.0;
                            for (int i = 0; i < m; i++) {
                                int v = vals[i];
                                int b = (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
                                double e = (double)v - mu[b];
                                nsum += e * e;
                            }
                            double noise_var = nsum / (double)total;
                            if (noise_var > 1e-9) {
                                double mu_all = 0.0;
                                for (int b = 0; b < 4; b++) {
                                    mu_all += mu[b] * (double)cnt[b] / (double)total;
                                }
                                double ssum = 0.0;
                                for (int b = 0; b < 4; b++) {
                                    double d = mu[b] - mu_all;
                                    ssum += (double)cnt[b] * d * d;
                                }
                                double sig_var = ssum / (double)total;
                                if (sig_var > 1e-9) {
                                    double snr = 10.0 * log10(sig_var / noise_var);
                                    static double ema = -100.0;
                                    if (ema < -50.0) {
                                        ema = snr;
                                    } else {
                                        ema = 0.8 * ema + 0.2 * snr;
                                    }
                                    g_snr_c4fm_db = ema;
                                }
                            }
                        }
                        /* 2-level (GFSK-like) using median split */
                        {
                            double sumL = 0.0, sumH = 0.0;
                            int cntL = 0, cntH = 0;
                            for (int i = 0; i < m; i++) {
                                int v = vals[i];
                                if (v <= q2) {
                                    sumL += v;
                                    cntL++;
                                } else {
                                    sumH += v;
                                    cntH++;
                                }
                            }
                            if (cntL > 0 && cntH > 0) {
                                double muL = sumL / (double)cntL, muH = sumH / (double)cntH;
                                int total = cntL + cntH;
                                double nsum = 0.0;
                                for (int i = 0; i < m; i++) {
                                    int v = vals[i];
                                    double mu = (v <= q2) ? muL : muH;
                                    double e = (double)v - mu;
                                    nsum += e * e;
                                }
                                double noise_var = nsum / (double)total;
                                if (noise_var > 1e-9) {
                                    double mu_all = (muL * (double)cntL + muH * (double)cntH) / (double)total;
                                    double ssum = (double)cntL * (muL - mu_all) * (muL - mu_all)
                                                  + (double)cntH * (muH - mu_all) * (muH - mu_all);
                                    double sig_var = ssum / (double)total;
                                    if (sig_var > 1e-9) {
                                        double snr = 10.0 * log10(sig_var / noise_var);
                                        static double ema = -100.0;
                                        if (ema < -50.0) {
                                            ema = snr;
                                        } else {
                                            ema = 0.8 * ema + 0.2 * snr;
                                        }
                                        g_snr_gfsk_db = ema;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (d->exit_flag) {
            exitflag = 1;
        }
        if (d->squelch_level && d->squelch_hits > d->conseq_squelch) {
            d->squelch_hits = d->conseq_squelch + 1; /* hair trigger */
            safe_cond_signal(&controller.hop, &controller.hop_m);
            continue;
        }
        if (d->resamp_enabled) {
            int out_n = resamp_process_block(d, d->result, d->result_len, d->resamp_outbuf);
            if (out_n > 0) {
                ring_write_signal_on_empty_transition(o, d->resamp_outbuf, (size_t)out_n);
            }
            if (!logged_once) {
                LOG_INFO("Demod first block: in=%d decim_len=%d resamp_out=%d\n", got, d->result_len, out_n);
                logged_once = 1;
            }
        } else {
            /* When resampler is disabled, pass-through. */
            if (d->result_len > 0) {
                ring_write_signal_on_empty_transition(o, d->result, (size_t)d->result_len);
            }
            if (!logged_once) {
                LOG_INFO("Demod first block: in=%d decim_len=%d (no resampler)\n", got, d->result_len);
                logged_once = 1;
            }
        }
        /* Signaling occurs only when the ring transitions from empty to non-empty. */
    }
    return 0;
}

/**
 * @brief Compute and stage tuner/demodulator capture settings based on the
 * requested center frequency and current demod configuration. The actual
 * device programming occurs elsewhere after these fields are updated.
 *
 * @param freq Desired RF center frequency in Hz.
 * @param rate Current input sample rate (unused).
 */
static void
optimal_settings(int freq, int rate) {
    UNUSED(rate);

    int capture_freq, capture_rate;
    struct dongle_state* d = &dongle;
    struct demod_state* dm = &demod;
    struct controller_state* cs = &controller;
    /* Compute integer oversample factor to target ~1 MS/s capture then map
       to a cascade of 2:1 decimators (half-band/CIC) via passes = ceil(log2(ds)). */
    dm->downsample = (1000000 / dm->rate_in) + 1;
    {
        int ds = dm->downsample;
        if (ds <= 1) {
            dm->downsample_passes = 0;
            dm->downsample = 1;
        } else {
#if defined(__GNUC__) || defined(__clang__)
            int floor_log2 = 31 - __builtin_clz(ds);
#else
            int floor_log2 = 0;
            {
                int t = ds;
                while (t >>= 1) {
                    floor_log2++;
                }
            }
#endif
            int is_pow2 = (ds & (ds - 1)) == 0;
            int passes = is_pow2 ? floor_log2 : (floor_log2 + 1);
            if (passes < 0) {
                passes = 0;
            }
            if (passes > 10) {
                passes = 10; /* practical guard */
            }
            dm->downsample_passes = passes;
            dm->downsample = 1 << passes;
        }
    }
    capture_freq = freq;
    capture_rate = dm->downsample * dm->rate_in; // input capture rate
    /* Apply fs/4 shift for zero-IF DC spur avoidance when offset_tuning is disabled. */
    if (!d->offset_tuning && !disable_fs4_shift) {
        capture_freq = freq + capture_rate / 4;
    }
    capture_freq += cs->edge * dm->rate_in / 2;
    dm->output_scale = (1 << 15) / (128 * dm->downsample);
    if (dm->output_scale < 1) {
        dm->output_scale = 1;
    }
    if (dm->mode_demod == &fm_demod) {
        dm->output_scale = 1;
    }
    /* Update the effective discriminator output sample rate based on current settings.
       If no HB cascade is used (downsample_passes==0), the legacy low_pass() decimator
       reduces by dm->downsample back toward the nominal bandwidth. Otherwise, HB/CIC
       reduces by (1<<downsample_passes). Apply optional post_downsample on audio. */
    {
        int base_decim = 1;
        if (dm->downsample_passes > 0) {
            base_decim = (1 << dm->downsample_passes);
        } else {
            base_decim = (dm->downsample > 0) ? dm->downsample : 1;
        }
        if (base_decim < 1) {
            base_decim = 1;
        }
        int out_rate = capture_rate / base_decim;
        if (dm->post_downsample > 1) {
            out_rate /= dm->post_downsample;
            if (out_rate < 1) {
                out_rate = 1;
            }
        }
        dm->rate_out = out_rate;
    }
    d->freq = (uint32_t)capture_freq;
    d->rate = (uint32_t)capture_rate;
}

/**
 * @brief Program device to new center frequency and sample rate using a
 * single, consistent path. Applies fs/4 shift when offset_tuning is off.
 *
 * @param center_freq_hz Desired RF center frequency in Hz.
 */
static void
apply_capture_settings(uint32_t center_freq_hz) {
    optimal_settings((int)center_freq_hz, demod.rate_in);
    rtl_device_set_frequency(rtl_device_handle, dongle.freq);
    rtl_device_set_sample_rate(rtl_device_handle, dongle.rate);
    /* Best-effort set tuner IF bandwidth close to configured RTL bandwidth */
    rtl_device_set_tuner_bandwidth(rtl_device_handle, (uint32_t)rtl_bandwidth);
}

/**
 * @brief Recompute resampler configuration if demod output rate changed.
 * Updates output rate accordingly. Runs on controller thread.
 */
static void
maybe_update_resampler_after_rate_change(void) {
    if (demod.resamp_target_hz <= 0) {
        demod.resamp_enabled = 0;
        output.rate = demod.rate_out;
        return;
    }
    int target = demod.resamp_target_hz;
    int inRate = demod.rate_out > 0 ? demod.rate_out : rtl_bandwidth;
    int g = gcd_int(inRate, target);
    int L = target / g;
    int M = inRate / g;
    if (L < 1) {
        L = 1;
    }
    if (M < 1) {
        M = 1;
    }
    int scale = (M > 0) ? ((L + M - 1) / M) : 1;

    if (scale > 8) {
        if (demod.resamp_enabled) {
            /* Disable and free on out-of-bounds ratio */
            if (demod.resamp_taps) {
                dsd_neo_aligned_free(demod.resamp_taps);
                demod.resamp_taps = NULL;
            }
            if (demod.resamp_hist) {
                dsd_neo_aligned_free(demod.resamp_hist);
                demod.resamp_hist = NULL;
            }
        }
        demod.resamp_enabled = 0;
        output.rate = demod.rate_out;
        LOG_WARNING("Resampler ratio too large on retune (L=%d,M=%d). Disabled.\n", L, M);
        return;
    }

    /* Re-design only if params changed or buffers not allocated */
    if (!demod.resamp_enabled || demod.resamp_L != L || demod.resamp_M != M || demod.resamp_taps == NULL
        || demod.resamp_hist == NULL) {
        if (demod.resamp_taps) {
            dsd_neo_aligned_free(demod.resamp_taps);
            demod.resamp_taps = NULL;
        }
        if (demod.resamp_hist) {
            dsd_neo_aligned_free(demod.resamp_hist);
            demod.resamp_hist = NULL;
        }
        resamp_design(&demod, L, M);
        demod.resamp_L = L;
        demod.resamp_M = M;
        demod.resamp_enabled = 1;
        LOG_INFO("Resampler reconfigured: %d -> %d Hz (L=%d,M=%d).\n", inRate, target, L, M);
    }
    output.rate = target;
}

/**
 * @brief Controller worker: scans/hops through configured center frequencies.
 *
 * Programs tuner frequency/sample rate according to current optimal settings
 * and hops when signaled by the demod path (e.g., squelch-triggered).
 *
 * @param arg Pointer to `controller_state`.
 * @return NULL on exit.
 */
static void*
controller_thread_fn(void* arg) {
    int i;
    struct controller_state* s = static_cast<controller_state*>(arg);

    if (s->wb_mode) {
        for (i = 0; i < s->freq_len; i++) {
            s->freqs[i] += 16000;
        }
    }

    /* set up primary channel */
    optimal_settings(s->freqs[0], demod.rate_in);
    if (dongle.direct_sampling) {
        rtl_device_set_direct_sampling(rtl_device_handle, 1);
    }
    if (dongle.offset_tuning) {
        rtl_device_set_offset_tuning(rtl_device_handle);
    }

    /* Set the frequency */
    rtl_device_set_frequency(rtl_device_handle, dongle.freq);
    LOG_INFO("Oversampling input by: %ix.\n", demod.downsample);
    LOG_INFO("Oversampling output by: %ix.\n", demod.post_downsample);
    LOG_INFO("Buffer size: %0.2fms\n", 1000 * 0.5 * (float)ACTUAL_BUF_LENGTH / (float)dongle.rate);

    /* Set the sample rate */
    rtl_device_set_sample_rate(rtl_device_handle, dongle.rate);
    LOG_INFO("Demod output at %u Hz.\n", (unsigned int)demod.rate_out);

    while (!exitflag && !(g_stream && g_stream->should_exit.load())) {
        /* Wait for a hop signal or a pending retune, with proper predicate guard */
        pthread_mutex_lock(&s->hop_m);
        while (!s->manual_retune_pending.load() && !exitflag && !(g_stream && g_stream->should_exit.load())) {
            pthread_cond_wait(&s->hop, &s->hop_m);
        }
        pthread_mutex_unlock(&s->hop_m);
        if (exitflag || (g_stream && g_stream->should_exit.load())) {
            break;
        }
        /* Process marshalled manual retunes first */
        if (s->manual_retune_pending.load()) {
            uint32_t tgt = s->manual_retune_freq;
            s->manual_retune_pending.store(0);
            apply_capture_settings((uint32_t)tgt);
            maybe_update_resampler_after_rate_change();
            /* Reset demod/FLL/TED and clear any stale buffers on retune */
            demod_reset_on_retune(&demod);
            input_ring_clear(&input_ring);
            rtl_device_mute(rtl_device_handle, BUFFER_DUMP);
            drain_output_on_retune();
            LOG_INFO("Retune applied: %u Hz.\n", tgt);
            continue;
        }
        if (s->freq_len <= 1) {
            continue;
        }
        s->freq_now = (s->freq_now + 1) % s->freq_len;
        apply_capture_settings((uint32_t)s->freqs[s->freq_now]);
        maybe_update_resampler_after_rate_change();
        /* Reset demod/FLL/TED and clear any stale buffers on hop */
        demod_reset_on_retune(&demod);
        input_ring_clear(&input_ring);
        rtl_device_mute(rtl_device_handle, BUFFER_DUMP);
        drain_output_on_retune();
    }
    return 0;
}

/* ---------------- Constellation capture (simple lock-free ring) ---------------- */

static const int kConstMaxPairs = 8192;
static int16_t g_const_xy[kConstMaxPairs * 2];
static volatile int g_const_head = 0; /* pairs written [0..kConstMaxPairs-1], wraps */
/* Forward decl for eye-ring append used in demod loop */
static inline void eye_ring_append_i_chan(const int16_t* iq_interleaved, int len_interleaved);

/* Append decimated I/Q samples from lowpassed[] after DSP. */
void
constellation_ring_append(const int16_t* iq, int len, int sps_hint) {
    if (!iq || len < 2) {
        return;
    }
    int N = len >> 1;                                              /* complex samples */
    int stride = (sps_hint >= 1 && sps_hint <= 64) ? sps_hint : 4; /* rough decimation */
    if (stride < 1) {
        stride = 1;
    }
    for (int n = 0; n < N; n += stride) {
        int i = iq[(size_t)(n << 1) + 0];
        int q = iq[(size_t)(n << 1) + 1];
        int h = g_const_head;
        g_const_xy[(size_t)(h << 1) + 0] = (int16_t)i;
        g_const_xy[(size_t)(h << 1) + 1] = (int16_t)q;
        h++;
        if (h >= kConstMaxPairs) {
            h = 0;
        }
        g_const_head = h;
    }
}

extern "C" int
dsd_rtl_stream_constellation_get(int16_t* out_xy, int max_points) {
    if (!out_xy || max_points <= 0) {
        return 0;
    }
    int head = g_const_head; /* snapshot */
    int n = (max_points < kConstMaxPairs) ? max_points : kConstMaxPairs;
    int start = head;
    for (int k = 0; k < n; k++) {
        int idx = (start + k) % kConstMaxPairs;
        out_xy[(size_t)(k << 1) + 0] = g_const_xy[(size_t)(idx << 1) + 0];
        out_xy[(size_t)(k << 1) + 1] = g_const_xy[(size_t)(idx << 1) + 1];
    }
    return n;
}

/* ---------------- Eye diagram capture (I-channel of complex baseband) ---------------- */
static const int kEyeMax = 16384;
static int16_t g_eye_buf[kEyeMax];
static volatile int g_eye_head = 0; /* samples written [0..kEyeMax-1], wraps */

static inline void
eye_ring_append_i_chan(const int16_t* iq_interleaved, int len_interleaved) {
    if (!iq_interleaved || len_interleaved < 2) {
        return;
    }
    int N = len_interleaved >> 1; /* complex samples */
    for (int n = 0; n < N; n++) {
        int16_t i = iq_interleaved[(size_t)(n << 1) + 0];
        int h = g_eye_head;
        g_eye_buf[h] = i;
        h++;
        if (h >= kEyeMax) {
            h = 0;
        }
        g_eye_head = h;
    }
}

extern "C" int
dsd_rtl_stream_eye_get(int16_t* out, int max_samples, int* out_sps) {
    if (out_sps) {
        *out_sps = demod.ted_sps;
    }
    if (!out || max_samples <= 0) {
        return 0;
    }
    int head = g_eye_head;
    int n = (max_samples < kEyeMax) ? max_samples : kEyeMax;
    int start = head;
    for (int k = 0; k < n; k++) {
        int idx = (start + k) % kEyeMax;
        out[k] = g_eye_buf[idx];
    }
    return n;
}

/* ---------------- SNR export (smoothed) ---------------- */
extern "C" double
rtl_stream_get_snr_c4fm(void) {
    return g_snr_c4fm_db;
}

extern "C" double
rtl_stream_get_snr_cqpsk(void) {
    return g_snr_qpsk_db;
}

extern "C" double
rtl_stream_get_snr_gfsk(void) {
    return g_snr_gfsk_db;
}

/**
 * @brief Initialize dongle (RTL-SDR source) state with default parameters.
 *
 * @param s Dongle state to initialize.
 */
void
dongle_init(struct dongle_state* s) {
    s->rate = rtl_bandwidth;
    s->gain = AUTO_GAIN; // tenths of a dB
    s->mute = 0;
    s->direct_sampling = 0;
    s->offset_tuning = 0; //E4000 tuners only
    s->demod_target = &demod;
}

typedef enum { DEMOD_DIGITAL = 0, DEMOD_ANALOG = 1, DEMOD_RO2 = 2 } DemodMode;

typedef struct {
    int deemph_default;
} DemodInitParams;

/**
 * @brief Initialize demodulator state using a unified entrypoint.
 *
 * Sets common defaults and applies mode-specific adjustments for digital,
 * analog, or RO2 operation. This centralizes prior duplicated init logic.
 *
 * @param s Demodulator state to initialize.
 * @param mode Demodulation mode selector (digital, analog, or RO2).
 * @param p Optional initialization parameters (e.g., default deemphasis).
 */
static void
demod_init_mode(struct demod_state* s, DemodMode mode, const DemodInitParams* p) {
    /* Common defaults */
    s->rate_in = rtl_bandwidth;
    s->rate_out = rtl_bandwidth;
    s->squelch_level = 0;
    s->conseq_squelch = 10;
    s->terminate_on_squelch = 0;
    s->squelch_hits = 11;
    s->downsample_passes = 0;
    s->comp_fir_size = 0;
    s->prev_index = 0;
    s->post_downsample = 1;
    s->custom_atan = 2;
    s->deemph = 0;
    s->rate_out2 = -1;
    s->mode_demod = &fm_demod;
    s->pre_j = s->pre_r = s->now_r = s->now_j = 0;
    s->prev_lpr_index = 0;
    s->deemph_a = 0;
    s->deemph_avg = 0;
    /* Audio LPF defaults */
    s->audio_lpf_enable = 0;
    s->audio_lpf_alpha = 0;
    s->audio_lpf_state = 0;
    s->now_lpr = 0;
    s->dc_block = 1;
    s->dc_avg = 0;
    /* Resampler defaults */
    s->resamp_enabled = 0;
    s->resamp_target_hz = 0;
    s->resamp_L = 1;
    s->resamp_M = 1;
    s->resamp_phase = 0;
    s->resamp_taps_len = 0;
    s->resamp_taps_per_phase = 0;
    s->resamp_taps = NULL;
    s->resamp_hist = NULL;
    /* FLL/TED defaults */
    s->fll_enabled = 0;
    s->fll_alpha_q15 = 0;
    s->fll_beta_q15 = 0;
    s->fll_freq_q15 = 0;
    s->fll_phase_q15 = 0;
    s->fll_prev_r = 0;
    s->fll_prev_j = 0;
    s->ted_enabled = 0;
    s->ted_gain_q20 = 0;
    s->ted_sps = 0;
    s->ted_mu_q20 = 0;
    /* Initialize FLL and TED module states */
    fll_init_state(&s->fll_state);
    ted_init_state(&s->ted_state);
    /* Squelch estimator init */
    s->squelch_running_power = 0;
    s->squelch_decim_stride = 16;
    s->squelch_decim_phase = 0;
    s->squelch_window = 2048;
    /* HB decimator histories */
    for (int st = 0; st < 10; st++) {
        memset(s->hb_hist_i[st], 0, sizeof(s->hb_hist_i[st]));
        memset(s->hb_hist_q[st], 0, sizeof(s->hb_hist_q[st]));
    }
    /* Legacy CIC histories used by fifth_order path */
    for (int st = 0; st < 10; st++) {
        memset(s->lp_i_hist[st], 0, sizeof(s->lp_i_hist[st]));
        memset(s->lp_q_hist[st], 0, sizeof(s->lp_q_hist[st]));
    }
    /* Input ring does not require double-buffer init */
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 0;
    pthread_cond_init(&s->ready, NULL);
    pthread_mutex_init(&s->ready_m, NULL);
    s->output_target = &output;

    /* Experimental CQPSK/LSM path (off by default). Enable via env DSD_NEO_CQPSK=1 */
    s->cqpsk_enable = 0;
    const char* env_cqpsk = getenv("DSD_NEO_CQPSK");
    if (env_cqpsk
        && (*env_cqpsk == '1' || *env_cqpsk == 'y' || *env_cqpsk == 'Y' || *env_cqpsk == 't' || *env_cqpsk == 'T')) {
        s->cqpsk_enable = 1;
        fprintf(stderr, " DSP: CQPSK/LSM pre-processing enabled (experimental)\n");
    }

    /* Mode-specific adjustments */
    if (mode == DEMOD_ANALOG) {
        s->downsample_passes = 1;
        s->comp_fir_size = 9;
        s->custom_atan = 1;
        s->deemph = 1;
        s->rate_out2 = rtl_bandwidth;
    } else if (mode == DEMOD_RO2) {
        s->downsample_passes = 0;
        s->comp_fir_size = 0;
        s->custom_atan = 2;
        s->deemph = 0;
        s->rate_out2 = rtl_bandwidth;
    } else {
        /* DEMOD_DIGITAL default already set */
        s->rate_out2 = -1;
    }

    if (p && p->deemph_default >= 0) {
        s->deemph = p->deemph_default;
    }

    if (s->custom_atan == 2) {
        atan_lut_init();
    }
    /* set discriminator function pointer */
    s->discriminator = (s->custom_atan == 0)   ? &polar_discriminant
                       : (s->custom_atan == 1) ? &polar_disc_fast
                                               : &polar_disc_lut;
    /* Init minimal worker pool (env-gated) */
    demod_mt_init(s);
}

/**
 * @brief Initialize demodulator state for analog FM path.
 *
 * Applies analog-specific defaults (e.g., deemphasis enabled, FIR size,
 * and downsample passes) on top of common initialization.
 *
 * @param s Demodulator state to initialize.
 */
void
demod_init_analog(struct demod_state* s) {
    DemodInitParams params = {0};
    params.deemph_default = 1;
    demod_init_mode(s, DEMOD_ANALOG, &params);
}

/**
 * @brief Initialize demodulator state for RO2 path (no CIC, LUT atan by default).
 *
 * @param s Demodulator state to initialize.
 */
void
demod_init_ro2(struct demod_state* s) {
    DemodInitParams params = {0};
    demod_init_mode(s, DEMOD_RO2, &params);
}

/**
 * @brief Initialize demodulator state for default digital path.
 *
 * @param s Demodulator state to initialize.
 */
void
demod_init(struct demod_state* s) {
    DemodInitParams params = {0};
    demod_init_mode(s, DEMOD_DIGITAL, &params);
}

/**
 * @brief Release resources owned by the demodulator state.
 *
 * @param s Demodulator state to clean up.
 */
void
demod_cleanup(struct demod_state* s) {
    pthread_cond_destroy(&s->ready);
    pthread_mutex_destroy(&s->ready_m);
    /* Destroy worker pool if enabled */
    demod_mt_destroy(s);
    /* Free resampler resources */
    if (s->resamp_taps) {
        dsd_neo_aligned_free(s->resamp_taps);
        s->resamp_taps = NULL;
    }
    if (s->resamp_hist) {
        dsd_neo_aligned_free(s->resamp_hist);
        s->resamp_hist = NULL;
    }
}

/**
 * @brief Initialize output ring buffer and synchronization primitives.
 *
 * @param s Output state to initialize.
 */
void
output_init(struct output_state* s) {
    s->rate = rtl_bandwidth;
    pthread_cond_init(&s->ready, NULL);
    pthread_cond_init(&s->space, NULL);
    pthread_mutex_init(&s->ready_m, NULL);
    /* Allocate SPSC ring buffer */
    s->capacity = (size_t)(MAXIMUM_BUF_LENGTH * 8);
    /* Try aligned allocation for better vectorized copies; fall back if unavailable */
    {
        void* mem_ptr = dsd_neo_aligned_malloc(s->capacity * sizeof(int16_t));
        if (!mem_ptr) {
            LOG_ERROR("Failed to allocate output ring buffer (%zu samples).\n", s->capacity);
            /* Propagate by keeping buffer NULL; callers must detect before use */
            return;
        }
        s->buffer = static_cast<int16_t*>(mem_ptr);
    }
    s->head.store(0);
    s->tail.store(0);
    /* Metrics */
    s->write_timeouts.store(0);
    s->read_timeouts.store(0);
}

/**
 * @brief Destroy output ring buffer and synchronization primitives.
 *
 * @param s Output state to clean up.
 */
void
output_cleanup(struct output_state* s) {
    pthread_cond_destroy(&s->ready);
    pthread_cond_destroy(&s->space);
    pthread_mutex_destroy(&s->ready_m);
    if (s->buffer) {
        dsd_neo_aligned_free(s->buffer);
        s->buffer = NULL;
    }
}

/**
 * @brief Initialize controller state (frequency list and hop control).
 *
 * @param s Controller state to initialize.
 */
void
controller_init(struct controller_state* s) {
    s->freqs[0] = 446000000;
    s->freq_len = 0;
    s->edge = 0;
    s->wb_mode = 0;
    pthread_cond_init(&s->hop, NULL);
    pthread_mutex_init(&s->hop_m, NULL);
    s->manual_retune_pending.store(0);
    s->manual_retune_freq = 0;
}

/**
 * @brief Destroy controller synchronization primitives.
 *
 * @param s Controller state to clean up.
 */
void
controller_cleanup(struct controller_state* s) {
    pthread_cond_destroy(&s->hop);
    pthread_mutex_destroy(&s->hop_m);
}

/**
 * @brief Handle termination signals by requesting RTL-SDR async cancel and exit.
 *
 * Logs the event and triggers a non-blocking stop of the async capture loop
 * so worker threads can wind down cleanly.
 */
extern "C" void
rtlsdr_sighandler(void) {
    LOG_ERROR("Signal caught, exiting!\n");
    /* Cooperative shutdown and wake any waiters */
    exitflag = 1;
    if (g_stream) {
        g_stream->should_exit.store(1);
    }
    safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
    safe_cond_signal(&controller.hop, &controller.hop_m);
    safe_cond_signal(&demod.ready, &demod.ready_m);
    safe_cond_signal(&output.ready, &output.ready_m);
    rtl_device_stop_async(rtl_device_handle);
}

/**
 * @brief Apply runtime configuration flags and set up optional resampler/FLL/TED.
 *
 * Reads environment-backed runtime configuration and options, enables
 * or disables modules, and designs the rational resampler when requested.
 *
 * @param opts Decoder options.
 */
static void
configure_from_env_and_opts(dsd_opts* opts) {
    dsd_neo_config_init(opts);
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return;
    }
    if (cfg->hb_decim_is_set) {
        use_halfband_decimator = (cfg->hb_decim != 0);
    }
    if (cfg->combine_rot_is_set) {
        combine_rotate_enabled = (cfg->combine_rot != 0);
    }
    if (cfg->upsample_fp_is_set) {
        upsample_fixedpoint_enabled = (cfg->upsample_fp != 0);
    }

    if (cfg->fs4_shift_disable_is_set) {
        disable_fs4_shift = (cfg->fs4_shift_disable != 0);
    }

    int enable_resamp = 1;
    int target = 48000;
    if (cfg->resamp_is_set) {
        enable_resamp = cfg->resamp_disable ? 0 : 1;
        target = cfg->resamp_target_hz > 0 ? cfg->resamp_target_hz : 48000;
    }
    /* Defer resampler design until after capture settings establish actual rates */
    demod.resamp_target_hz = enable_resamp ? target : 0;
    demod.resamp_enabled = 0;

    demod.fll_enabled = cfg->fll_is_set ? (cfg->fll_enable != 0) : 0;
    fll_lut_enabled = cfg->fll_lut_is_set ? (cfg->fll_lut_enable != 0) : fll_lut_enabled;
    demod.fll_alpha_q15 = cfg->fll_alpha_is_set ? cfg->fll_alpha_q15 : 50;
    demod.fll_beta_q15 = cfg->fll_beta_is_set ? cfg->fll_beta_q15 : 5;
    demod.fll_deadband_q14 = cfg->fll_deadband_is_set ? cfg->fll_deadband_q14 : 45;
    demod.fll_slew_max_q15 = cfg->fll_slew_is_set ? cfg->fll_slew_max_q15 : 64;
    demod.fll_freq_q15 = 0;
    demod.fll_phase_q15 = 0;
    demod.fll_prev_r = demod.fll_prev_j = 0;

    demod.ted_enabled = cfg->ted_is_set ? (cfg->ted_enable != 0) : 0;
    demod.ted_gain_q20 = cfg->ted_gain_is_set ? cfg->ted_gain_q20 : 64;
    demod.ted_sps = cfg->ted_sps_is_set ? cfg->ted_sps : 10;
    demod.ted_mu_q20 = 0;
    demod.ted_force = cfg->ted_force_is_set ? (cfg->ted_force != 0) : 0;

    /* Default all DSP handles OFF unless explicitly requested via env/CLI. */
    demod.cqpsk_enable = 0;
    const char* ev_cq = getenv("DSD_NEO_CQPSK");
    if (ev_cq && (*ev_cq == '1' || *ev_cq == 'y' || *ev_cq == 'Y' || *ev_cq == 't' || *ev_cq == 'T')) {
        demod.cqpsk_enable = 1;
    }

    /* Map CLI runtime toggles for CQPSK LMS */
    if (opts->cqpsk_lms != 0) {
        demod.cqpsk_lms_enable = 1;
    }
    if (opts->cqpsk_mu_q15 > 0) {
        demod.cqpsk_mu_q15 = opts->cqpsk_mu_q15;
    } else if (demod.cqpsk_mu_q15 == 0) {
        demod.cqpsk_mu_q15 = 1; /* tiny default */
    }
    if (opts->cqpsk_stride > 0) {
        demod.cqpsk_update_stride = opts->cqpsk_stride;
    } else if (demod.cqpsk_update_stride == 0) {
        demod.cqpsk_update_stride = 4;
    }

    /* Matched filter pre-EQ default OFF; allow env to enable */
    demod.cqpsk_mf_enable = 0;
    const char* mf = getenv("DSD_NEO_CQPSK_MF");
    if (mf && (*mf == '1' || *mf == 'y' || *mf == 'Y' || *mf == 't' || *mf == 'T')) {
        demod.cqpsk_mf_enable = 1;
    }

    /* Optional RRC matched filter configuration */
    demod.cqpsk_rrc_enable = 0;
    demod.cqpsk_rrc_alpha_q15 = (int)(0.25 * 32768.0); /* default 0.25 */
    demod.cqpsk_rrc_span_syms = 6;                     /* default 6 symbols (total span ~12) */
    const char* rrc = getenv("DSD_NEO_CQPSK_RRC");
    if (rrc && (*rrc == '1' || *rrc == 'y' || *rrc == 'Y' || *rrc == 't' || *rrc == 'T')) {
        demod.cqpsk_rrc_enable = 1;
    }
    const char* rrca = getenv("DSD_NEO_CQPSK_RRC_ALPHA");
    if (rrca) {
        int v = atoi(rrca);
        if (v < 1) {
            v = 1;
        }
        if (v > 100) {
            v = 100;
        }
        demod.cqpsk_rrc_alpha_q15 = (int)((v / 100.0) * 32768.0);
    }
    const char* rrcs = getenv("DSD_NEO_CQPSK_RRC_SPAN");
    if (rrcs) {
        int v = atoi(rrcs);
        if (v < 3) {
            v = 3;
        }
        if (v > 16) {
            v = 16;
        }
        demod.cqpsk_rrc_span_syms = v;
    }
}

/**
 * @brief Apply sensible defaults for digital vs analog modes when env not set.
 *
 * @param opts Decoder options.
 */
static void
select_defaults_for_mode(dsd_opts* opts) {
    int env_ted_set = dsd_neo_get_config()->ted_is_set;
    int env_fll_alpha_set = dsd_neo_get_config()->fll_alpha_is_set;
    int env_fll_beta_set = dsd_neo_get_config()->fll_beta_is_set;
    int env_ted_sps_set = dsd_neo_get_config()->ted_sps_is_set;
    int env_ted_gain_set = dsd_neo_get_config()->ted_gain_is_set;
    /* Treat all digital voice modes as digital for FLL/TED defaults */
    int digital_mode = (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_provoice == 1
                        || opts->frame_dmr == 1 || opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1
                        || opts->frame_dstar == 1 || opts->frame_dpmr == 1 || opts->frame_m17 == 1);
    if (digital_mode) {
        /* Keep TED disabled by default; user/UI can enable as needed. */
        if (!env_ted_sps_set) {
            /* Use configured target or current output rate to derive default SPS per mode */
            int Fs_cx = (demod.resamp_target_hz > 0) ? demod.resamp_target_hz
                                                     : (demod.rate_out > 0 ? demod.rate_out : (int)output.rate);
            if (Fs_cx <= 0) {
                Fs_cx = 48000; /* safe default */
            }
            int sps = 0;
            if (opts->frame_p25p2 == 1) {
                sps = (Fs_cx + 3000) / 6000; /* round(Fs/6000) */
            } else if (opts->frame_p25p1 == 1) {
                sps = (Fs_cx + 2400) / 4800; /* round(Fs/4800) */
            } else if (opts->frame_nxdn48 == 1) {
                sps = (Fs_cx + 1200) / 2400; /* round(Fs/2400) */
            } else {
                sps = (Fs_cx + 2400) / 4800; /* generic 4800 sym/s */
            }
            if (sps < 2) {
                sps = 2;
            }
            demod.ted_sps = sps;
        }
        if (!env_ted_gain_set) {
            demod.ted_gain_q20 = 96;
        }
        if (!env_fll_alpha_set) {
            demod.fll_alpha_q15 = 150;
        }
        if (!env_fll_beta_set) {
            demod.fll_beta_q15 = 15;
        }
        /* Keep FLL disabled by default; user/UI can enable as needed. */
    } else {
        if (!env_ted_set) {
            demod.ted_enabled = 0;
        }
        if (!env_fll_alpha_set) {
            demod.fll_alpha_q15 = 50;
        }
        if (!env_fll_beta_set) {
            demod.fll_beta_q15 = 5;
        }
    }
}

/**
 * @brief Seed initial device index, center frequency, gain and UDP port.
 *
 * @param opts Decoder options.
 */
static void
setup_initial_freq_and_rate(dsd_opts* opts) {
    if (opts->rtlsdr_center_freq > 0) {
        controller.freqs[controller.freq_len] = opts->rtlsdr_center_freq;
        controller.freq_len++;
    }
    if (opts->rtlsdr_ppm_error != 0) {
        dongle.ppm_error = opts->rtlsdr_ppm_error;
        LOG_INFO("Setting RTL PPM Error Set to %d\n", opts->rtlsdr_ppm_error);
    }
    dongle.dev_index = opts->rtl_dev_index;
    LOG_INFO("Setting RTL Bandwidth to %d Hz\n", rtl_bandwidth);
    LOG_INFO("Setting RTL Power Squelch Level to %d\n", opts->rtl_squelch_level);
    if (opts->rtl_udp_port != 0) {
        int p = opts->rtl_udp_port;
        if (p < 0) {
            p = 0;
        }
        if (p > 65535) {
            p = 65535;
        }
        port = (uint16_t)p;
    }
    if (opts->rtl_gain_value > 0) {
        dongle.gain = opts->rtl_gain_value * 10;
    }
}

/**
 * @brief Launch controller/demod threads and start async device capture.
 *
 * Spawns the controller and demodulation workers, begins RTL-SDR async
 * streaming with the configured buffer size, and starts optional UDP
 * control for on-the-fly tuning.
 */
static void
start_threads_and_async(void) {
    pthread_create(&controller.thread, NULL, controller_thread_fn, (void*)(&controller));
    pthread_create(&demod.thread, NULL, demod_thread_fn, (void*)(&demod));
    LOG_INFO("Starting RTL async read...\n");
    rtl_device_start_async(rtl_device_handle, (uint32_t)ACTUAL_BUF_LENGTH);
    if (port != 0) {
        g_udp_ctrl = udp_control_start(
            port,
            [](uint32_t new_freq_hz, void* /*user_data*/) {
                /* Marshal onto controller thread: single programming path */
                pthread_mutex_lock(&controller.hop_m);
                controller.manual_retune_freq = new_freq_hz;
                controller.manual_retune_pending.store(1);
                pthread_cond_signal(&controller.hop);
                pthread_mutex_unlock(&controller.hop_m);
            },
            NULL);
    }
}

/**
 * @brief Initialize and open the RTL-SDR streaming pipeline, threads, and buffers.
 *
 * Configures device and demod state, validates options, allocates buffers,
 * programs initial capture settings (including fs/4 shift when appropriate),
 * and starts async capture plus worker threads.
 *
 * @param opts Decoder options used to configure the pipeline.
 * @return 0 on success, negative on error.
 */
/* Forward decl for manual-DSP override query used during open */
extern "C" int rtl_stream_get_manual_dsp(void);

extern "C" int
dsd_rtl_stream_open(dsd_opts* opts) {
    /* If Manual DSP Override is active, preserve current DSP toggles across
       demod re-initialization so they don't reset to defaults. */
    struct {
        int use;
        int cqpsk_enable;
        int fll_enable;
        int ted_enable;
        int ted_sps;
        int ted_gain_q20;
        int ted_force;
        int mf_enable;
        int rrc_enable, rrc_alpha_q15, rrc_span_syms;
    } persist = {0};

    if (rtl_stream_get_manual_dsp()) {
        persist.use = 1;
        persist.cqpsk_enable = demod.cqpsk_enable;
        persist.fll_enable = demod.fll_enabled;
        persist.ted_enable = demod.ted_enabled;
        persist.ted_sps = demod.ted_sps;
        persist.ted_gain_q20 = demod.ted_gain_q20;
        persist.ted_force = demod.ted_force ? 1 : 0;
        persist.mf_enable = demod.cqpsk_mf_enable;
        persist.rrc_enable = demod.cqpsk_rrc_enable;
        persist.rrc_alpha_q15 = demod.cqpsk_rrc_alpha_q15;
        persist.rrc_span_syms = demod.cqpsk_rrc_span_syms;
    }
    rtl_bandwidth = opts->rtl_bandwidth * 1000; //reverted back to straight value
    bandwidth_multiplier = (bandwidth_divisor / rtl_bandwidth);
    /* Guard multiplier to a safe range [1, MAX_BANDWIDTH_MULTIPLIER] */
    {
        int orig_mult = bandwidth_multiplier;
        if (bandwidth_multiplier < 1) {
            LOG_WARNING("bandwidth_multiplier computed as %d (divisor=%d, bandwidth=%d Hz). Clamping to 1.\n",
                        orig_mult, bandwidth_divisor, rtl_bandwidth);
            bandwidth_multiplier = 1;
        } else if (bandwidth_multiplier > MAX_BANDWIDTH_MULTIPLIER) {
            LOG_WARNING("bandwidth_multiplier computed as %d exceeds max %d (divisor=%d, bandwidth=%d Hz). "
                        "Clamping to %d.\n",
                        orig_mult, MAX_BANDWIDTH_MULTIPLIER, bandwidth_divisor, rtl_bandwidth,
                        MAX_BANDWIDTH_MULTIPLIER);
            bandwidth_multiplier = MAX_BANDWIDTH_MULTIPLIER;
        }
    }
    volume_multiplier = 1;

    dongle_init(&dongle);
    {
        DemodInitParams params = {0};
        if (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_provoice == 1) {
            demod_init_mode(&demod, DEMOD_RO2, &params);
        } else if (opts->analog_only == 1 || opts->m17encoder == 1) {
            params.deemph_default = 1;
            demod_init_mode(&demod, DEMOD_ANALOG, &params);
        } else {
            demod_init_mode(&demod, DEMOD_DIGITAL, &params);
        }
    }
    output_init(&output);
    if (!output.buffer) {
        LOG_ERROR("Output ring buffer allocation failed.\n");
        return -1;
    }
    /* Init input ring */
    {
        void* mem_ptr = dsd_neo_aligned_malloc((size_t)(MAXIMUM_BUF_LENGTH * 8) * sizeof(int16_t));
        if (!mem_ptr) {
            LOG_ERROR("Failed to allocate input ring buffer.\n");
            return -1;
        }
        input_ring.buffer = static_cast<int16_t*>(mem_ptr);
        input_ring.capacity = (size_t)(MAXIMUM_BUF_LENGTH * 8);
        input_ring.head.store(0);
        input_ring.tail.store(0);
        pthread_cond_init(&input_ring.ready, NULL);
        pthread_mutex_init(&input_ring.ready_m, NULL);
        /* Metrics */
        input_ring.producer_drops.store(0);
        input_ring.read_timeouts.store(0);
    }
    controller_init(&controller);

    /* Read optional environment flags (centralized) */
    configure_from_env_and_opts(opts);
    select_defaults_for_mode(opts);

    /* Reapply preserved DSP toggles when Manual Override is active. */
    if (persist.use) {
        demod.cqpsk_enable = persist.cqpsk_enable ? 1 : 0;
        demod.fll_enabled = persist.fll_enable ? 1 : 0;
        demod.ted_enabled = persist.ted_enable ? 1 : 0;
        if (persist.ted_sps > 0) {
            demod.ted_sps = persist.ted_sps;
        }
        if (persist.ted_gain_q20 > 0) {
            demod.ted_gain_q20 = persist.ted_gain_q20;
        }
        demod.ted_force = persist.ted_force ? 1 : 0;
        demod.cqpsk_mf_enable = persist.mf_enable ? 1 : 0;
        demod.cqpsk_rrc_enable = persist.rrc_enable ? 1 : 0;
        if (persist.rrc_alpha_q15 > 0) {
            demod.cqpsk_rrc_alpha_q15 = persist.rrc_alpha_q15;
        }
        if (persist.rrc_span_syms > 0) {
            demod.cqpsk_rrc_span_syms = persist.rrc_span_syms;
        }
    }

    setup_initial_freq_and_rate(opts);

    if (!output.rate) {
        output.rate = demod.rate_out;
    }

    {
        /* Validate inputs; require at least one frequency and squelch when scanning */
        if (controller.freq_len == 0) {
            LOG_ERROR("Please specify a frequency.\n");
            return -1;
        }
        if (controller.freq_len >= FREQUENCIES_LIMIT) {
            LOG_ERROR("Too many channels, maximum %i.\n", FREQUENCIES_LIMIT);
            return -1;
        }
        if (controller.freq_len > 1 && demod.squelch_level == 0) {
            LOG_ERROR("Please specify a squelch level.  Required for scanning multiple frequencies.\n");
            return -1;
        }
    }

    if (controller.freq_len > 1) {
        demod.terminate_on_squelch = 0;
    }

    ACTUAL_BUF_LENGTH = lcm_post[demod.post_downsample] * DEFAULT_BUF_LENGTH;
    /* Ensure async read uses a valid, explicit buffer length */
    dongle.buf_len = (uint32_t)ACTUAL_BUF_LENGTH;

    rtl_device_handle = rtl_device_create(dongle.dev_index, &input_ring, combine_rotate_enabled);
    if (!rtl_device_handle) {
        LOG_ERROR("Failed to open rtlsdr device %d.\n", dongle.dev_index);
        return -1;
    } else {
        LOG_INFO("Using RTLSDR Device Index: %d. \n", dongle.dev_index);
    }

    if (demod.deemph) {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        double tau_s = 75e-6; /* default 75 microseconds */
        if (cfg && cfg->deemph_is_set) {
            if (cfg->deemph_mode == DSD_NEO_DEEMPH_OFF) {
                demod.deemph = 0;
            } else if (cfg->deemph_mode == DSD_NEO_DEEMPH_50) {
                tau_s = 50e-6;
            } else if (cfg->deemph_mode == DSD_NEO_DEEMPH_NFM) {
                tau_s = 750e-6;
            } else if (cfg->deemph_mode == DSD_NEO_DEEMPH_75) {
                tau_s = 75e-6;
            }
        }
        if (demod.deemph) {
            double Fs = (double)demod.rate_out;
            if (Fs < 1.0) {
                Fs = 1.0;
            }
            double a = exp(-1.0 / (Fs * tau_s));
            double alpha = 1.0 - a;
            int coef_q15 = (int)lrint(alpha * (double)(1 << 15));
            if (coef_q15 < 1) {
                coef_q15 = 1;
            }
            if (coef_q15 > ((1 << 15) - 1)) {
                coef_q15 = ((1 << 15) - 1);
            }
            demod.deemph_a = coef_q15;
        }
    }

    /* Configure optional post-demod audio LPF via env DSD_NEO_AUDIO_LPF.
       Values:
       - off or 0: disabled (default)
       - NNNN: cutoff in Hz (approximate), e.g., 3000 or 5000.
       One-pole: y[n] = y[n-1] + alpha * (x[n] - y[n-1]),
       alpha ≈ 1 - exp(-2*pi*fc/Fs) in Q15. */
    {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        demod.audio_lpf_enable = 0;
        demod.audio_lpf_alpha = 0;
        demod.audio_lpf_state = 0;
        if (cfg && cfg->audio_lpf_is_set && !cfg->audio_lpf_disable && cfg->audio_lpf_cutoff_hz > 0) {
            int cutoff_hz = cfg->audio_lpf_cutoff_hz;
            if (cutoff_hz < 100) {
                cutoff_hz = 100; /* guard */
            }
            double Fs = (double)demod.rate_out;
            if (Fs < 1.0) {
                Fs = 1.0;
            }
            double a = 1.0 - exp(-2.0 * kPi * (double)cutoff_hz / Fs);
            if (a < 0.0) {
                a = 0.0;
            }
            if (a > 1.0) {
                a = 1.0;
            }
            int alpha_q15 = (int)lrint(a * (double)(1 << 15));
            if (alpha_q15 < 1) {
                alpha_q15 = 1;
            }
            if (alpha_q15 > ((1 << 15) - 1)) {
                alpha_q15 = ((1 << 15) - 1);
            }
            demod.audio_lpf_alpha = alpha_q15;
            demod.audio_lpf_enable = 1;
            LOG_INFO("Audio LPF enabled: fc≈%d Hz, alpha_q15=%d\n", cutoff_hz, demod.audio_lpf_alpha);
        }
    }

    /* Set the tuner gain */
    rtl_device_set_gain(rtl_device_handle, dongle.gain);
    if (dongle.gain == AUTO_GAIN) {
        LOG_INFO("Setting RTL Autogain. \n");
    }

    rtl_device_set_ppm(rtl_device_handle, dongle.ppm_error);

    /* Program initial frequency and sample rate before starting async */
    if (controller.freq_len == 0) {
        controller.freqs[controller.freq_len] = 446000000;
        controller.freq_len++;
    }
    optimal_settings(controller.freqs[0], demod.rate_in);
    if (dongle.direct_sampling) {
        rtl_device_set_direct_sampling(rtl_device_handle, 1);
    }
    if (dongle.offset_tuning) {
        rtl_device_set_offset_tuning(rtl_device_handle);
    }
    apply_capture_settings((uint32_t)controller.freqs[0]);
    LOG_INFO("Oversampling input by: %ix.\n", demod.downsample);
    LOG_INFO("Oversampling output by: %ix.\n", demod.post_downsample);
    LOG_INFO("Buffer size: %0.2fms\n", 1000 * 0.5 * (float)ACTUAL_BUF_LENGTH / (float)dongle.rate);
    LOG_INFO("Demod output at %u Hz.\n", (unsigned int)demod.rate_out);

    /* Recompute resampler with the actual demod output rate now known */
    if (demod.resamp_target_hz > 0) {
        int target = demod.resamp_target_hz;
        int inRate = demod.rate_out > 0 ? demod.rate_out : rtl_bandwidth;
        int g = gcd_int(inRate, target);
        int L = target / g;
        int M = inRate / g;
        if (L < 1) {
            L = 1;
        }
        if (M < 1) {
            M = 1;
        }
        int scale = (M > 0) ? ((L + M - 1) / M) : 1;
        if (scale > 8) {
            LOG_WARNING("Resampler ratio too large (L=%d,M=%d). Disabling resampler.\n", L, M);
            demod.resamp_enabled = 0;
        } else {
            demod.resamp_enabled = 1;
            resamp_design(&demod, L, M);
            LOG_INFO("Rational resampler configured: %d -> %d Hz (L=%d,M=%d).\n", inRate, target, L, M);
        }
    } else {
        demod.resamp_enabled = 0;
    }

    /* Reset endpoint before we start reading from it (mandatory) */
    rtl_device_reset_buffer(rtl_device_handle);

    /* Create or refresh stream internals before launching threads */
    if (g_stream) {
        free(g_stream);
        g_stream = NULL;
    }
    g_stream = (struct RtlSdrInternals*)calloc(1, sizeof(struct RtlSdrInternals));
    if (g_stream) {
        g_stream->device = rtl_device_handle;
        g_stream->dongle = &dongle;
        g_stream->demod = &demod;
        g_stream->output = &output;
        g_stream->controller = &controller;
        g_stream->input_ring = &input_ring;
        g_stream->udp_ctrl_ptr = &g_udp_ctrl;
        g_stream->cfg = dsd_neo_get_config();
        g_stream->should_exit.store(0);
    }

    /* Start controller/demod threads and async */
    start_threads_and_async();

    /* If resampler is enabled, update output.rate for downstream consumers */
    if (demod.resamp_enabled && demod.resamp_target_hz > 0) {
        output.rate = demod.resamp_target_hz;
        LOG_INFO("Output rate set to %d Hz via resampler.\n", output.rate);
    } else {
        output.rate = demod.rate_out;
    }

    /* One-time startup summary of the rate chain */
    {
        unsigned int capture_hz = dongle.rate;
        int base_decim = (demod.downsample_passes > 0) ? (1 << demod.downsample_passes)
                                                       : (demod.downsample > 0 ? demod.downsample : 1);
        int post = (demod.post_downsample > 0) ? demod.post_downsample : 1;
        int L = demod.resamp_enabled ? demod.resamp_L : 1;
        int M = demod.resamp_enabled ? demod.resamp_M : 1;
        unsigned int demod_hz = (unsigned int)demod.rate_out;
        unsigned int out_hz =
            demod.resamp_enabled && demod.resamp_target_hz > 0 ? (unsigned int)demod.resamp_target_hz : demod_hz;
        LOG_INFO(
            "Rate chain: capture=%u Hz, base_decim=%d, post=%d -> demod=%u Hz; resampler L/M=%d/%d -> output=%u Hz.\n",
            capture_hz, base_decim, post, demod_hz, L, M, out_hz);

        /* Derived SPS for common digital modes at current output rate */
        if (out_hz > 0) {
            int sps_p25p1 = (int)((out_hz + 2400) / 4800);  /* ~10 at 48k */
            int sps_p25p2 = (int)((out_hz + 3000) / 6000);  /* ~8 at 48k */
            int sps_nxdn48 = (int)((out_hz + 1200) / 2400); /* ~20 at 48k */
            LOG_INFO("Derived SPS (@%u Hz): P25P1≈%d, P25P2≈%d, NXDN48≈%d.\n", out_hz, sps_p25p1, sps_p25p2,
                     sps_nxdn48);
            /* Warn if far from canonical 48k-based SPS expectations */
            if ((sps_p25p1 < 8 || sps_p25p1 > 12) || (sps_p25p2 < 6 || sps_p25p2 > 10)
                || (sps_nxdn48 < 16 || sps_nxdn48 > 24)) {
                LOG_WARNING("Output rate %u Hz implies atypical SPS; digital decoders assume ~48k. Consider enabling "
                            "resampler to 48000 Hz.\n",
                            out_hz);
            }
        }
    }

    return 0;
}

/**
 * @brief Stop threads, free resources, and close the RTL-SDR stream.
 *
 * Signals workers to exit, joins threads, destroys device objects and rings,
 * releases LUTs and aligned buffers, and tears down UDP control if enabled.
 */
extern "C" void
dsd_rtl_stream_close(void) {
    LOG_INFO("cleaning up...\n");
    if (g_stream) {
        g_stream->should_exit.store(1);
    }
    LOG_INFO("Output ring: write_timeouts=%llu read_timeouts=%llu\n", (unsigned long long)output.write_timeouts.load(),
             (unsigned long long)output.read_timeouts.load());
    LOG_INFO("Input ring: producer_drops=%llu read_timeouts=%llu\n",
             (unsigned long long)input_ring.producer_drops.load(), (unsigned long long)input_ring.read_timeouts.load());
    if (g_udp_ctrl) {
        udp_control_stop(g_udp_ctrl);
        g_udp_ctrl = NULL;
    }
    /* Request threads to exit and wake any waiters */
    exitflag = 1;
    safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
    safe_cond_signal(&controller.hop, &controller.hop_m);
    rtl_device_stop_async(rtl_device_handle);
    safe_cond_signal(&demod.ready, &demod.ready_m);
    pthread_join(demod.thread, NULL);
    safe_cond_signal(&output.ready, &output.ready_m);
    pthread_join(controller.thread, NULL);

    demod_cleanup(&demod);
    output_cleanup(&output);
    controller_cleanup(&controller);

    /* free input ring */
    if (input_ring.buffer) {
        dsd_neo_aligned_free(input_ring.buffer);
        input_ring.buffer = NULL;
    }

    /* free LUT memory if allocated */
    atan_lut_free();

    rtl_device_destroy(rtl_device_handle);
    rtl_device_handle = NULL;

    if (g_stream) {
        free(g_stream);
        g_stream = NULL;
    }
}

/**
 * @brief Soft-stop the RTL stream without setting global exitflag.
 *
 * Requests threads to exit via should_exit, stops async I/O, joins threads,
 * and cleans up resources similarly to dsd_rtl_stream_close(), but does not
 * touch the global exitflag so the application continues running.
 */
extern "C" int
dsd_rtl_stream_soft_stop(void) {
    LOG_INFO("soft stopping...\n");
    if (g_stream) {
        g_stream->should_exit.store(1);
    }
    if (g_udp_ctrl) {
        udp_control_stop(g_udp_ctrl);
        g_udp_ctrl = NULL;
    }
    safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
    safe_cond_signal(&controller.hop, &controller.hop_m);
    rtl_device_stop_async(rtl_device_handle);
    safe_cond_signal(&demod.ready, &demod.ready_m);
    pthread_join(demod.thread, NULL);
    safe_cond_signal(&output.ready, &output.ready_m);
    pthread_join(controller.thread, NULL);

    demod_cleanup(&demod);
    output_cleanup(&output);
    controller_cleanup(&controller);

    if (input_ring.buffer) {
        dsd_neo_aligned_free(input_ring.buffer);
        input_ring.buffer = NULL;
    }
    atan_lut_free();
    rtl_device_destroy(rtl_device_handle);
    rtl_device_handle = NULL;
    return 0;
}

/**
 * @brief Batched consumer API: read up to count samples with fewer wakeups/locks.
 * Applies volume scaling.
 *
 * @param out   Destination buffer for audio samples.
 * @param count Maximum number of samples to read.
 * @param opts  Decoder options (used for runtime PPM changes).
 * @param state Decoder state (unused).
 * @return Number of samples read (>=1), 0 if count==0, or -1 on exit.
 */
extern "C" int
dsd_rtl_stream_read(int16_t* out, size_t count, dsd_opts* opts, dsd_state* state) {
    UNUSED(state);
    if (count == 0) {
        return 0;
    }

    /* Optional: auto-adjust RTL PPM using smoothed TED residual (opt-in).
       This gently nudges opts->rtlsdr_ppm_error by +/-1 when the Gardner bias
       shows a persistent sign and magnitude above a threshold. */
    do {
        static int init = 0;
        static int enabled = 0;
        static int thr = 30000; /* coarse units */
        static int hold = 200;  /* consecutive hits before change */
        static int step = 1;    /* ppm per adjust */
        static int dir_run = 0; /* -1,0,+1 */
        static int run_len = 0;
        if (!init) {
            init = 1;
            const char* on = getenv("DSD_NEO_AUTO_PPM");
            if (on && (*on == '1' || *on == 'y' || *on == 'Y' || *on == 't' || *on == 'T')) {
                enabled = 1;
            }
            const char* sthr = getenv("DSD_NEO_AUTO_PPM_THR");
            if (sthr) {
                int v = atoi(sthr);
                if (v > 1000 && v < 200000) {
                    thr = v;
                }
            }
            const char* shold = getenv("DSD_NEO_AUTO_PPM_HOLD");
            if (shold) {
                int v = atoi(shold);
                if (v >= 50 && v <= 2000) {
                    hold = v;
                }
            }
            const char* sstep = getenv("DSD_NEO_AUTO_PPM_STEP");
            if (sstep) {
                int v = atoi(sstep);
                if (v >= 1 && v <= 5) {
                    step = v;
                }
            }
        }
        if (enabled) {
            int e = demod.ted_state.e_ema;
            int dir = 0;
            if (e > thr) {
                dir = +1;
            } else if (e < -thr) {
                dir = -1;
            }
            if (dir == 0) {
                dir_run = 0;
                run_len = 0;
            } else {
                if (dir == dir_run) {
                    run_len++;
                } else {
                    dir_run = dir;
                    run_len = 1;
                }
                if (run_len >= hold) {
                    int new_ppm = opts->rtlsdr_ppm_error + dir * step;
                    opts->rtlsdr_ppm_error = new_ppm;
                    run_len = 0;
                    /* leave dir_run as-is to require persistence for next change */
                    LOG_INFO("AUTO-PPM: e_ema=%d, dir=%d, ppm->%d\n", e, dir, new_ppm);
                }
            }
        }
    } while (0);

    /* If PPM Error is Manually Changed, change it here once per batch */
    if (opts->rtlsdr_ppm_error != dongle.ppm_error) {
        dongle.ppm_error = opts->rtlsdr_ppm_error;
        rtl_device_set_ppm(rtl_device_handle, dongle.ppm_error);
    }

    int got = ring_read_batch(&output, out, count);
    if (got <= 0) {
        return -1;
    }
    /* Apply volume scaling with saturation */
    for (int i = 0; i < got; i++) {
        int32_t y = (int32_t)out[i] * (int32_t)volume_multiplier;
        out[i] = sat16(y);
    }
    return got;
}

/**
 * @brief Return the current output audio sample rate in Hz.
 *
 * @return Output sample rate in Hz.
 */
extern "C" unsigned int
dsd_rtl_stream_output_rate(void) {
    return (unsigned int)output.rate;
}

/**
 * @brief Return smoothed TED residual (EMA of Gardner error). Sign indicates
 * persistent early/late bias; 0 when TED disabled or no bias.
 */
extern "C" int
dsd_rtl_stream_ted_bias(void) {
    return demod.ted_state.e_ema;
}

/**
 * @brief Set the Gardner TED nominal samples-per-symbol.
 * Clamps to a sensible range and applies immediately.
 */
extern "C" void
dsd_rtl_stream_set_ted_sps(int sps) {
    if (sps < 2) {
        sps = 2;
    }
    if (sps > 32) {
        sps = 32;
    }
    demod.ted_sps = sps;
}

extern "C" int
dsd_rtl_stream_get_ted_sps(void) {
    return demod.ted_sps;
}

extern "C" void
dsd_rtl_stream_set_ted_gain(int g) {
    if (g < 16) {
        g = 16;
    }
    if (g > 512) {
        g = 512;
    }
    demod.ted_gain_q20 = g;
}

extern "C" int
dsd_rtl_stream_get_ted_gain(void) {
    return demod.ted_gain_q20;
}

extern "C" void
dsd_rtl_stream_set_ted_force(int onoff) {
    demod.ted_force = onoff ? 1 : 0;
}

extern "C" int
dsd_rtl_stream_get_ted_force(void) {
    return demod.ted_force ? 1 : 0;
}

/**
 * @brief Set or disable the resampler target rate and reapply capture settings.
 *
 * Marshals onto the controller thread by scheduling a no-op retune to the
 * current frequency, which safely reconfigures the resampler and updates the
 * output rate with proper buffer draining.
 *
 * @param target_hz Target output rate in Hz. Pass 0 to disable resampler.
 */
extern "C" void
dsd_rtl_stream_set_resampler_target(int target_hz) {
    if (target_hz <= 0) {
        demod.resamp_target_hz = 0;
    } else {
        demod.resamp_target_hz = target_hz;
    }
    /* Schedule retune to current center to apply changes on controller thread */
    pthread_mutex_lock(&controller.hop_m);
    controller.manual_retune_freq = dongle.freq;
    controller.manual_retune_pending.store(1);
    pthread_cond_signal(&controller.hop);
    pthread_mutex_unlock(&controller.hop_m);
}

/* Runtime DSP tuning entrypoints (C shim) */
static std::atomic<int> g_auto_dsp_enable{0}; /* default OFF */
static int g_manual_dsp_override = 0;         /* default OFF: allow auto toggles */

/* Auto-DSP configuration with sensible defaults and light validation */
struct AutoDspConfig {
    int p25p1_window_min_total = 200;
    int p25p1_moderate_on_pct = 7;
    int p25p1_moderate_off_pct = 5;
    int p25p1_heavy_on_pct = 15;
    int p25p1_heavy_off_pct = 10;
    int p25p1_cooldown_ms = 700;

    int p25p2_ok_min = 4;
    int p25p2_err_margin_on = 2;
    int p25p2_err_margin_off = 0;
    int p25p2_cooldown_ms = 500;

    int ema_alpha_q15 = 6553; /* ~0.2 */
};

static AutoDspConfig g_auto_cfg; /* default initialized */

/* Auto-DSP live status; modes: 0=clean,1=moderate,2=heavy */
static std::atomic<int> g_p25p1_mode{0};
static std::atomic<int> g_p25p2_mode{0};
static std::atomic<int> g_p25p1_ema_pct{0};
static std::chrono::steady_clock::time_point g_p25p1_last_change;
static std::chrono::steady_clock::time_point g_p25p2_last_change;

/* Helper: clamp integer within [lo, hi] */
static inline int
clampi(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

extern "C" void
dsd_rtl_stream_auto_dsp_get_config(rtl_auto_dsp_config* out) {
    if (!out) {
        return;
    }
    out->p25p1_window_min_total = g_auto_cfg.p25p1_window_min_total;
    out->p25p1_moderate_on_pct = g_auto_cfg.p25p1_moderate_on_pct;
    out->p25p1_moderate_off_pct = g_auto_cfg.p25p1_moderate_off_pct;
    out->p25p1_heavy_on_pct = g_auto_cfg.p25p1_heavy_on_pct;
    out->p25p1_heavy_off_pct = g_auto_cfg.p25p1_heavy_off_pct;
    out->p25p1_cooldown_ms = g_auto_cfg.p25p1_cooldown_ms;
    out->p25p2_ok_min = g_auto_cfg.p25p2_ok_min;
    out->p25p2_err_margin_on = g_auto_cfg.p25p2_err_margin_on;
    out->p25p2_err_margin_off = g_auto_cfg.p25p2_err_margin_off;
    out->p25p2_cooldown_ms = g_auto_cfg.p25p2_cooldown_ms;
    out->ema_alpha_q15 = g_auto_cfg.ema_alpha_q15;
}

extern "C" void
dsd_rtl_stream_auto_dsp_set_config(const rtl_auto_dsp_config* in) {
    if (!in) {
        return;
    }
    AutoDspConfig c = g_auto_cfg; /* start from current */
    if (in->p25p1_window_min_total > 0) {
        c.p25p1_window_min_total = clampi(in->p25p1_window_min_total, 50, 2000);
    }
    if (in->p25p1_moderate_on_pct > 0) {
        c.p25p1_moderate_on_pct = clampi(in->p25p1_moderate_on_pct, 1, 50);
    }
    if (in->p25p1_moderate_off_pct > 0) {
        c.p25p1_moderate_off_pct = clampi(in->p25p1_moderate_off_pct, 0, 50);
    }
    if (in->p25p1_heavy_on_pct > 0) {
        c.p25p1_heavy_on_pct = clampi(in->p25p1_heavy_on_pct, 1, 90);
    }
    if (in->p25p1_heavy_off_pct > 0) {
        c.p25p1_heavy_off_pct = clampi(in->p25p1_heavy_off_pct, 0, 90);
    }
    if (in->p25p1_cooldown_ms > 0) {
        c.p25p1_cooldown_ms = clampi(in->p25p1_cooldown_ms, 50, 5000);
    }
    if (in->p25p2_ok_min > 0) {
        c.p25p2_ok_min = clampi(in->p25p2_ok_min, 1, 50);
    }
    if (in->p25p2_err_margin_on > 0) {
        c.p25p2_err_margin_on = clampi(in->p25p2_err_margin_on, 0, 50);
    }
    if (in->p25p2_err_margin_off > 0) {
        c.p25p2_err_margin_off = clampi(in->p25p2_err_margin_off, 0, 50);
    }
    if (in->p25p2_cooldown_ms > 0) {
        c.p25p2_cooldown_ms = clampi(in->p25p2_cooldown_ms, 50, 5000);
    }
    if (in->ema_alpha_q15 > 0) {
        c.ema_alpha_q15 = clampi(in->ema_alpha_q15, 1, 32768);
    }
    g_auto_cfg = c;
}

extern "C" void
dsd_rtl_stream_auto_dsp_get_status(rtl_auto_dsp_status* out) {
    if (!out) {
        return;
    }
    out->p25p1_mode = g_p25p1_mode.load();
    out->p25p1_ema_pct = g_p25p1_ema_pct.load();
    out->p25p1_since_ms = 0;
    out->p25p2_mode = g_p25p2_mode.load();
    out->p25p2_since_ms = 0;
    const auto now = std::chrono::steady_clock::now();
    if (g_p25p1_last_change.time_since_epoch().count() != 0) {
        out->p25p1_since_ms =
            (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - g_p25p1_last_change).count();
        if (out->p25p1_since_ms < 0) {
            out->p25p1_since_ms = 0;
        }
    }
    if (g_p25p2_last_change.time_since_epoch().count() != 0) {
        out->p25p2_since_ms =
            (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - g_p25p2_last_change).count();
        if (out->p25p2_since_ms < 0) {
            out->p25p2_since_ms = 0;
        }
    }
}

/* Forward declaration for CQPSK runtime setter used by auto-DSP update below */
extern "C" void rtl_stream_cqpsk_set(int lms_enable, int taps, int mu_q15, int update_stride, int wl_enable,
                                     int dfe_enable, int dfe_taps, int mf_enable, int cma_warmup_samples);

/**
 * @brief P25 Phase 2 error-driven auto-DSP adaptation.
 * Aggregates recent RS/voice error deltas and nudges CQPSK EQ settings.
 * No-ops when auto-DSP is disabled.
 */
extern "C" void
dsd_rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                int sacch_err_delta, int voice_err_delta) {
    (void)slot; /* reserved for future per-slot tuning */
    if (!g_auto_dsp_enable.load()) {
        return;
    }

    enum { MODE_CLEAN = 0, MODE_MODERATE = 1, MODE_HEAVY = 2 };

    static int mode = MODE_CLEAN;
    static std::chrono::steady_clock::time_point last_change;
    const auto now = std::chrono::steady_clock::now();
    int ok = 0, err = 0;
    if (facch_ok_delta > 0) {
        ok += facch_ok_delta;
    }
    if (sacch_ok_delta > 0) {
        ok += sacch_ok_delta;
    }
    if (facch_err_delta > 0) {
        err += facch_err_delta;
    }
    if (sacch_err_delta > 0) {
        err += sacch_err_delta;
    }
    if (voice_err_delta > 0) {
        err += voice_err_delta / 2; /* voice errors are noisier; downweight */
    }
    /* Hysteresis: engage with margin_on / low ok; relax with margin_off and ok_min */
    int aggressive_on = (err > ok + g_auto_cfg.p25p2_err_margin_on) || (ok < g_auto_cfg.p25p2_ok_min);
    int aggressive_off = (err <= ok + g_auto_cfg.p25p2_err_margin_off) && (ok >= g_auto_cfg.p25p2_ok_min);
    int moderate_on = (err > 0);
    int moderate_off = (err == 0) || aggressive_off;

    int desired = MODE_CLEAN;
    switch (mode) {
        case MODE_HEAVY:
            if (aggressive_on) {
                desired = MODE_HEAVY;
            } else if (!aggressive_on && !moderate_on) {
                desired = MODE_CLEAN;
            } else {
                desired = MODE_MODERATE;
            }
            break;
        case MODE_MODERATE:
            if (aggressive_on) {
                desired = MODE_HEAVY;
            } else if (!moderate_on) {
                desired = MODE_CLEAN;
            } else {
                desired = MODE_MODERATE;
            }
            break;
        default:
            if (aggressive_on) {
                desired = MODE_HEAVY;
            } else if (moderate_on) {
                desired = MODE_MODERATE;
            } else {
                desired = MODE_CLEAN;
            }
            break;
    }
    /* Cooldown to avoid thrashing */
    bool can_change = true;
    if (last_change.time_since_epoch().count() != 0) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change).count();
        can_change = (ms >= g_auto_cfg.p25p2_cooldown_ms);
    }
    if (desired != mode && !can_change) {
        desired = mode; /* hold until cooldown expires */
    }

    /* Guard: only adjust when CQPSK path active; otherwise, ensure defaults modest */
    if (!demod.cqpsk_enable) {
        if (desired != MODE_CLEAN) {
            /* Don't stack CMA bursts */
            int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
            rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
            int cma_burst = (cma > 0) ? 0 : 800;
            rtl_stream_cqpsk_set(1, 5, 2, 6, 0, 1, 2, 1, cma_burst);
            demod.ted_enabled = 1;
            if (demod.ted_gain_q20 < 64) {
                demod.ted_gain_q20 = 64;
            }
        }
        return;
    }

    if (desired == MODE_HEAVY) {
        /* More taps, enable WL+DFE, small µ, enable MF; brief CMA warmup */
        int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
        rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
        int cma_burst = (cma > 0) ? 0 : 2000;
        rtl_stream_cqpsk_set(1, 7, 2, 4, 1, 1, 3, 1, cma_burst);
        demod.ted_enabled = 1;
        if (demod.ted_gain_q20 < 64) {
            demod.ted_gain_q20 = 64;
        }
    } else if (desired == MODE_MODERATE) {
        int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
        rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
        int cma_burst = (cma > 0) ? 0 : 1000;
        rtl_stream_cqpsk_set(1, 5, 2, 6, 0, 1, 2, 1, cma_burst);
        demod.ted_enabled = 1;
    } else {
        /* Relax settings when clean */
        rtl_stream_cqpsk_set(1, 5, 1, 8, 0, 0, 0, 1, 0);
    }
    if (desired != mode) {
        mode = desired;
        last_change = now;
        g_p25p2_mode.store(desired);
        g_p25p2_last_change = now;
    }
}

/* Master toggle to gate automatic DSP assistance (BER-based eq tweaks, etc.) */

extern "C" void
rtl_stream_cqpsk_set(int lms_enable, int taps, int mu_q15, int update_stride, int wl_enable, int dfe_enable,
                     int dfe_taps, int mf_enable, int cma_warmup_samples) {
    /* Apply matched-filter toggle directly on demod state */
    if (mf_enable >= 0) {
        demod.cqpsk_mf_enable = mf_enable ? 1 : 0;
    }
    /* Forward the rest to CQPSK path */
    cqpsk_runtime_set_params(lms_enable, taps, mu_q15, update_stride, wl_enable, dfe_enable, dfe_taps,
                             cma_warmup_samples);
}

extern "C" void
rtl_stream_p25p1_ber_update(int fec_ok_delta, int fec_err_delta) {
    if (!g_auto_dsp_enable.load()) {
        return; /* auto-DSP disabled: ignore BER-driven tuning */
    }
    static int64_t ok_acc = 0;
    static int64_t err_acc = 0;
    static double err_ema = 0.0; /* EMA of error rate [0..1] */
    static std::chrono::steady_clock::time_point last_change;
    const auto now = std::chrono::steady_clock::now();
    if (fec_ok_delta > 0) {
        ok_acc += fec_ok_delta;
    }
    if (fec_err_delta > 0) {
        err_acc += fec_err_delta;
    }
    int64_t total = ok_acc + err_acc;
    if (total < g_auto_cfg.p25p1_window_min_total) {
        return; /* wait for window */
    }
    /* Compute error rate */
    const double er = (total > 0) ? ((double)err_acc / (double)total) : 0.0;
    /* EMA smoothing */
    const double a = (double)g_auto_cfg.ema_alpha_q15 / 32768.0;
    err_ema = a * er + (1.0 - a) * err_ema;
    /* Reset for next window */
    ok_acc = 0;
    err_acc = 0;
    /* Guard: only adjust when CQPSK path active */
    if (!demod.cqpsk_enable) {
        return;
    }
    /* Hysteresis thresholds with cooldown */
    int er_pct = (int)lrint(err_ema * 100.0);
    g_p25p1_ema_pct.store(er_pct);

    enum { MODE_CLEAN = 0, MODE_MODERATE = 1, MODE_HEAVY = 2 };

    static int mode = MODE_CLEAN;
    int desired = MODE_CLEAN;
    switch (mode) {
        case MODE_HEAVY:
            if (er_pct >= g_auto_cfg.p25p1_heavy_on_pct) {
                desired = MODE_HEAVY;
            } else if (er_pct >= g_auto_cfg.p25p1_moderate_on_pct) {
                desired = MODE_MODERATE;
            } else if (er_pct <= g_auto_cfg.p25p1_moderate_off_pct) {
                desired = MODE_CLEAN;
            } else {
                desired = MODE_MODERATE;
            }
            break;
        case MODE_MODERATE:
            if (er_pct >= g_auto_cfg.p25p1_heavy_on_pct) {
                desired = MODE_HEAVY;
            } else if (er_pct <= g_auto_cfg.p25p1_moderate_off_pct) {
                desired = MODE_CLEAN;
            } else {
                desired = MODE_MODERATE;
            }
            break;
        default:
            if (er_pct >= g_auto_cfg.p25p1_heavy_on_pct) {
                desired = MODE_HEAVY;
            } else if (er_pct >= g_auto_cfg.p25p1_moderate_on_pct) {
                desired = MODE_MODERATE;
            } else {
                desired = MODE_CLEAN;
            }
            break;
    }
    bool can_change = true;
    if (last_change.time_since_epoch().count() != 0) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_change).count();
        can_change = (ms >= g_auto_cfg.p25p1_cooldown_ms);
    }
    if (desired != mode && !can_change) {
        desired = mode;
    }

    if (desired == MODE_HEAVY) {
        /* Enable WL, DFE, more taps, small µ; brief CMA kick */
        int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
        rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
        int cma_burst = (cma > 0) ? 0 : 2000;
        rtl_stream_cqpsk_set(1, 7, 2, 4, 1, 1, 3, -1, cma_burst);
        /* Ensure TED is on and gain modest */
        demod.ted_enabled = 1;
        if (demod.ted_gain_q20 < 64) {
            demod.ted_gain_q20 = 64;
        }
    } else if (desired == MODE_MODERATE) {
        /* Keep WL off by default, enable DFE lightly, 5–7 taps, µ=2 */
        int l = 0, t = 0, mu = 0, st = 0, wl = 0, dfe = 0, dft = 0, mf = 0, cma = 0;
        rtl_stream_cqpsk_get(&l, &t, &mu, &st, &wl, &dfe, &dft, &mf, &cma);
        int cma_burst = (cma > 0) ? 0 : 1000;
        rtl_stream_cqpsk_set(1, 5, 2, 6, 0, 1, 2, -1, cma_burst);
        demod.ted_enabled = 1;
    } else {
        /* Relax settings when clean: fewer taps, µ=1, DFE off */
        rtl_stream_cqpsk_set(1, 5, 1, 8, 0, 0, 0, -1, 0);
    }
    if (desired != mode) {
        mode = desired;
        last_change = now;
        g_p25p1_mode.store(desired);
        g_p25p1_last_change = now;
    }
}

extern "C" int
rtl_stream_cqpsk_get(int* lms_enable, int* taps, int* mu_q15, int* update_stride, int* wl_enable, int* dfe_enable,
                     int* dfe_taps, int* mf_enable, int* cma_warmup_remaining) {
    if (mf_enable) {
        *mf_enable = demod.cqpsk_mf_enable ? 1 : 0;
    }
    return cqpsk_runtime_get_params(lms_enable, taps, mu_q15, update_stride, wl_enable, dfe_enable, dfe_taps,
                                    cma_warmup_remaining);
}

/* Coarse DSP feature toggles and snapshot */
extern "C" void
rtl_stream_toggle_cqpsk(int onoff) {
    demod.cqpsk_enable = onoff ? 1 : 0;
    /* Reset EQ state when toggling path to avoid stale filters */
    cqpsk_reset_all();
}

extern "C" void
rtl_stream_toggle_fll(int onoff) {
    demod.fll_enabled = onoff ? 1 : 0;
    if (!demod.fll_enabled) {
        /* Reset FLL state to baseline to avoid carryover */
        fll_init_state(&demod.fll_state);
        demod.fll_freq_q15 = 0;
        demod.fll_phase_q15 = 0;
        demod.fll_prev_r = 0;
        demod.fll_prev_j = 0;
    }
}

extern "C" void
rtl_stream_toggle_ted(int onoff) {
    demod.ted_enabled = onoff ? 1 : 0;
    if (!demod.ted_enabled) {
        /* Reset TED state */
        ted_init_state(&demod.ted_state);
        demod.ted_mu_q20 = 0;
    }
}

extern "C" int
rtl_stream_dsp_get(int* cqpsk_enable, int* fll_enable, int* ted_enable, int* auto_dsp_enable) {
    if (cqpsk_enable) {
        *cqpsk_enable = demod.cqpsk_enable ? 1 : 0;
    }
    if (fll_enable) {
        *fll_enable = demod.fll_enabled ? 1 : 0;
    }
    if (ted_enable) {
        *ted_enable = demod.ted_enabled ? 1 : 0;
    }
    if (auto_dsp_enable) {
        *auto_dsp_enable = g_auto_dsp_enable.load() ? 1 : 0;
    }
    return 0;
}

extern "C" void
rtl_stream_toggle_auto_dsp(int onoff) {
    g_auto_dsp_enable.store(onoff ? 1 : 0);
}

extern "C" void
rtl_stream_set_manual_dsp(int onoff) {
    g_manual_dsp_override = onoff ? 1 : 0;
}

extern "C" int
rtl_stream_get_manual_dsp(void) {
    return g_manual_dsp_override ? 1 : 0;
}

/* Configure RRC matched filter parameters. Any arg <0 leaves it unchanged. */
extern "C" void
dsd_rtl_stream_cqpsk_set_rrc(int enable, int alpha_percent, int span_syms) {
    if (enable >= 0) {
        demod.cqpsk_rrc_enable = enable ? 1 : 0;
    }
    if (alpha_percent >= 0) {
        int v = alpha_percent;
        if (v < 1) {
            v = 1;
        }
        if (v > 100) {
            v = 100;
        }
        demod.cqpsk_rrc_alpha_q15 = (int)((v / 100.0) * 32768.0);
    }
    if (span_syms >= 0) {
        int v = span_syms;
        if (v < 3) {
            v = 3;
        }
        if (v > 16) {
            v = 16;
        }
        demod.cqpsk_rrc_span_syms = v;
    }
}

/* Toggle DQPSK decision mode in CQPSK path */
extern "C" void
dsd_rtl_stream_cqpsk_set_dqpsk(int onoff) {
    cqpsk_runtime_set_dqpsk(onoff ? 1 : 0);
}

/* Get current RRC MF params */
extern "C" int
dsd_rtl_stream_cqpsk_get_rrc(int* enable, int* alpha_percent, int* span_syms) {
    if (enable) {
        *enable = demod.cqpsk_rrc_enable ? 1 : 0;
    }
    if (alpha_percent) {
        int ap = (int)lrint((demod.cqpsk_rrc_alpha_q15 / 32768.0) * 100.0);
        if (ap < 0) {
            ap = 0;
        }
        if (ap > 100) {
            ap = 100;
        }
        *alpha_percent = ap;
    }
    if (span_syms) {
        *span_syms = demod.cqpsk_rrc_span_syms;
    }
    return 0;
}

/* Get DQPSK decision mode */
extern "C" int
dsd_rtl_stream_cqpsk_get_dqpsk(int* onoff) {
    int v = 0;
    if (cqpsk_runtime_get_dqpsk(&v) != 0) {
        return -1;
    }
    if (onoff) {
        *onoff = v ? 1 : 0;
    }
    return 0;
}

/**
 * @brief Tune RTL-SDR to a new center frequency, updating optimal settings.
 *
 * @param opts      Decoder options.
 * @param frequency Target center frequency in Hz.
 * @return 0 on success.
 */
extern "C" int
dsd_rtl_stream_tune(dsd_opts* opts, long int frequency) {
    if (opts->payload == 1) {
        LOG_INFO("\nTuning to %ld Hz.", frequency);
    }
    dongle.freq = opts->rtlsdr_center_freq = frequency;
    /* Marshal onto controller thread to ensure single-threaded device programming */
    pthread_mutex_lock(&controller.hop_m);
    controller.manual_retune_freq = (uint32_t)dongle.freq;
    controller.manual_retune_pending.store(1);
    pthread_cond_signal(&controller.hop);
    pthread_mutex_unlock(&controller.hop_m);
    if (opts->payload == 1) {
        LOG_INFO(" (Center Frequency: %u Hz.) \n", dongle.freq);
    }

    /* Honor drain/clear policy for API-triggered tunes as well */
    drain_output_on_retune();
    return 0;
}

/**
 * @brief Return mean power approximation (RMS^2 proxy) for soft squelch decisions.
 * Uses a small fixed sample window for efficiency.
 *
 * @return Mean power value (approximate RMS squared).
 */
extern "C" long int
dsd_rtl_stream_return_pwr(void) {
    long int pwr = 0;
    int n = demod.lp_len;
    if (n > 160) {
        n = 160;
    }
    if (n < 0) {
        n = 0;
    }
    pwr = mean_power(demod.lowpassed, n, 1);
    return (pwr);
}

/**
 * @brief Clear the output ring buffer and wake any waiting producer.
 */
extern "C" void
dsd_rtl_stream_clear_output(void) {
    struct output_state* outp = &output;
    if (g_stream && g_stream->output) {
        outp = g_stream->output;
    }
    /* Clear the entire ring to prevent sample 'lag' */
    ring_clear(outp);
    /* Wake producer waiting for space */
    safe_cond_signal(&outp->space, &outp->ready_m);
}
