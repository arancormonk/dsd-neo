// SPDX-License-Identifier: GPL-3.0-or-later
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <dsd-neo/core/dsd.h>
#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_metrics.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_control.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <dsd-neo/runtime/unicode.h>
#include <dsd-neo/runtime/worker_pool.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
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
int dsd_rtl_stream_set_rtltcp_autotune(int onoff);
int dsd_rtl_stream_get_rtltcp_autotune(void);
#ifdef __cplusplus
}
#endif

/* Forward declaration for eye ring append used in demod loop */
static inline void eye_ring_append_i_chan(const int16_t* iq_interleaved, int len_interleaved);

#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_BUF_LENGTH  (1 * 16384)
#define MAXIMUM_OVERSAMPLE  16
#define MAXIMUM_BUF_LENGTH  (MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#define AUTO_GAIN           -100
#define BUFFER_DUMP         4096

#define FREQUENCIES_LIMIT   1000

static int lcm_post[17] = {1, 1, 1, 3, 1, 5, 3, 7, 1, 9, 5, 11, 3, 13, 7, 15, 1};
static int ACTUAL_BUF_LENGTH;

static const double kPi = 3.14159265358979323846;
static const double kC4fmSNRNoiseBiasDb = 7.95;
/* Bias for EVM/2-level SNR on Gaussian noise (QPSK/GFSK): ~2.43 dB */
static const double kEvmSNRNoiseBiasDb = 2.43;

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

/* Debug/compat toggles via env (implemented in rtl_demod_config.cpp). */
extern int combine_rotate_enabled;      /* DSD_NEO_COMBINE_ROT (1 default) */
extern int upsample_fixedpoint_enabled; /* DSD_NEO_UPSAMPLE_FP (1 default) */

/* Runtime flag (default enabled). Set DSD_NEO_HB_DECIM=0 to use legacy decimator. */
extern int use_halfband_decimator;
/* Allow disabling the fs/4 capture frequency shift via env for trunking/exact-center use cases. */
extern int disable_fs4_shift; /* Set by env DSD_NEO_DISABLE_FS4_SHIFT=1 */

// UDP control handle
static struct udp_control* g_udp_ctrl = NULL;

/* DSP baseband for RTL path in Hz (derived from opts->rtl_dsp_bw_khz). */
int rtl_dsp_bw_hz;
short int volume_multiplier;
uint16_t port;

struct dongle_state {
    int exit_flag;
    pthread_t thread;
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
    const dsd_opts* opts; /* snapshot for mode hints (P25p1/2, etc.) */
    const dsdneoRuntimeConfig* cfg;
    /* Cooperative shutdown flag for threads launched by this stream */
    std::atomic<int> should_exit;
};

static struct RtlSdrInternals* g_stream = NULL;

/*
 * Pick a hardware tuner bandwidth.
 *
 * Priority:
 *  - If env DSD_NEO_TUNER_BW_HZ is set:
 *      - value "auto" or 0 => return 0 (driver automatic)
 *      - positive integer => clamp and use that value (in Hz)
 *  - Otherwise, prefer setting BW ~= capture sample rate to avoid
 *    overly narrow IF filtering across retunes/hops.
 *  - As a conservative fallback, derive from DSP bandwidth with a
 *    safety margin and clamp to practical bounds.
 */
static uint32_t
choose_tuner_bw_hz(uint32_t capture_rate_hz, uint32_t dsp_bw_hz) {
    const char* e = getenv("DSD_NEO_TUNER_BW_HZ");
    if (e && *e) {
        if (strcasecmp(e, "auto") == 0) {
            return 0; /* driver automatic */
        }
        long v = strtol(e, NULL, 10);
        if (v >= 0 && v <= 20000000) {
            return (uint32_t)v; /* allow explicit 0 => auto */
        }
    }

    /* Mode-aware policy:
       - Scanning (multiple freqs): prefer BW ~= capture rate for consistent IF while hopping.
       - Single freq:
           - Digital-like (no deemphasis): target ~2x channel BW with clamps.
           - Analog-like (deemphasis enabled): keep wide but sane; use min(capture, 1.8 MHz).
       If uncertain, fall back to prior heuristics. */

    int scanning = (controller.freq_len > 1) ? 1 : 0;
    int analog_like = (demod.deemph != 0) ? 1 : 0;

    if (scanning) {
        if (capture_rate_hz >= 225000 && capture_rate_hz <= 5000000) {
            return capture_rate_hz;
        }
    } else {
        if (!analog_like && dsp_bw_hz > 0) {
            /* Digital single-channel: ~2x channel bandwidth (guard), clamps 100 kHz..1.5 MHz */
            uint64_t tgt = (uint64_t)dsp_bw_hz * 2ULL;
            if (tgt < 100000ULL) {
                tgt = 100000ULL;
            }
            if (tgt > 1500000ULL) {
                tgt = 1500000ULL;
            }
            /* Optional: don't exceed capture rate much */
            if (capture_rate_hz > 0 && tgt > capture_rate_hz) {
                tgt = capture_rate_hz;
            }
            return (uint32_t)tgt;
        }
        if (analog_like && capture_rate_hz > 0) {
            uint32_t maxa = 1800000U; /* ~1.8 MHz ceiling for analog */
            return (capture_rate_hz < maxa) ? capture_rate_hz : maxa;
        }
    }

    /* Fallback: derive from DSP bandwidth with margin (x8), clamped. */
    uint32_t bw = 0;
    if (dsp_bw_hz > 0) {
        uint64_t hinted = (uint64_t)dsp_bw_hz * 8ULL; /* generous guard */
        if (hinted < 100000ULL) {
            hinted = 100000ULL; /* min 100 kHz */
        }
        if (hinted > 3000000ULL) {
            hinted = 3000000ULL; /* max 3 MHz */
        }
        bw = (uint32_t)hinted;
    }
    if (bw == 0) {
        /* Last-resort default */
        bw = 1200000; /* 1.2 MHz */
    }
    return bw;
}

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

/* C-linkage helper to toggle bias tee on the active RTL device.
   For rtl_tcp sources, forwards the request via protocol; for USB, uses
   librtlsdr API when available. Returns 0 on success; negative on error. */
extern "C" int
dsd_rtl_stream_set_bias_tee(int on) {
    if (!rtl_device_handle) {
        return -1;
    }
    return rtl_device_set_bias_tee(rtl_device_handle, on ? 1 : 0);
}

/* Export applied tuner gain for UI without exposing internals. */
extern "C" int
dsd_rtl_stream_get_gain(int* out_tenth_db, int* out_is_auto) {
    if (out_tenth_db) {
        *out_tenth_db = 0;
    }
    if (out_is_auto) {
        *out_is_auto = 1;
    }
    if (!rtl_device_handle) {
        return -1;
    }
    int is_auto = rtl_device_is_auto_gain(rtl_device_handle);
    if (out_is_auto) {
        *out_is_auto = (is_auto > 0) ? 1 : 0;
    }
    if (is_auto > 0) {
        /* In auto mode, report AGC without a specific gain value. */
        return 0;
    }
    int g = rtl_device_get_tuner_gain(rtl_device_handle);
    if (g < 0) {
        return -1;
    }
    if (out_tenth_db) {
        *out_tenth_db = g;
    }
    return 0;
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
    /* CQPSK differential history */
    s->cqpsk_diff_prev_r = 0;
    s->cqpsk_diff_prev_j = 0;
    s->costas_err_avg_q14 = 0;
    s->costas_state.phase = 0.0f;
    s->costas_state.freq = 0.0f;
    s->costas_state.alpha = 0.0f;
    s->costas_state.beta = 0.0f;
    s->costas_state.error = 0.0f;
    s->costas_state.initialized = 0;
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
    /* Post-demod polyphase decimator history */
    s->post_polydecim_hist_head = 0;
    s->post_polydecim_phase = 0;
    if (s->post_polydecim_hist && s->post_polydecim_K > 0) {
        memset(s->post_polydecim_hist, 0, (size_t)s->post_polydecim_K * sizeof(int16_t));
    }
    /* Channel LPF history */
    s->channel_lpf_hist_len = 62; /* kChannelLpfHistLen */
    s->channel_lpf_profile = 0;
    for (int k = 0; k < 64; k++) {
        s->channel_lpf_hist_i[k] = 0;
        s->channel_lpf_hist_q[k] = 0;
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
/* SNR buffers for different modulations
 * NOTE: These are updated from the demod thread and read from the UI thread.
 * Use atomics to avoid data races and stale reads. Relaxed ordering is
 * sufficient because we only need value coherence, not synchronization. */
#include <atomic>
std::atomic<double> g_snr_c4fm_db{-100.0};
std::atomic<double> g_snr_qpsk_db{-100.0};
std::atomic<double> g_snr_gfsk_db{-100.0};
/* Supervisory tuner autogain gate owned by rtl_metrics.cpp */
extern std::atomic<int> g_tuner_autogain_on;
/* Track recency and source of SNR updates: src 1=direct (symbols), 2=fallback (eye/constellation) */
std::atomic<long long> g_snr_c4fm_last_ms{0};
std::atomic<int> g_snr_c4fm_src{0};
std::atomic<long long> g_snr_qpsk_last_ms{0};
std::atomic<int> g_snr_qpsk_src{0};
std::atomic<long long> g_snr_gfsk_last_ms{0};
std::atomic<int> g_snr_gfsk_src{0};

/* Fwd decl: eye-based C4FM SNR fallback */
extern "C" double dsd_rtl_stream_estimate_snr_c4fm_eye(void);
/* Fwd decl: QPSK and GFSK fallbacks */
extern "C" double dsd_rtl_stream_estimate_snr_qpsk_const(void);
extern "C" double dsd_rtl_stream_estimate_snr_gfsk_eye(void);
/* Fwd decl: spectrum snapshot getter used for spectral SNR gating */
extern "C" int dsd_rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate);
/* Blanker and tuner autogain runtime get/set (implemented in rtl_sdr_fm.cpp) */
extern "C" int dsd_rtl_stream_get_blanker(int* out_thr, int* out_win);
extern "C" void dsd_rtl_stream_set_blanker(int enable, int thr, int win);
extern "C" int dsd_rtl_stream_get_tuner_autogain(void);
extern "C" void dsd_rtl_stream_set_tuner_autogain(int onoff);

/* Spectrum updater used in demod thread (implemented in rtl_metrics.cpp). */
extern "C" void rtl_metrics_update_spectrum_from_iq(const int16_t* iq_interleaved, int len_interleaved,
                                                    int out_rate_hz);

static void*
demod_thread_fn(void* arg) {
    struct demod_state* d = static_cast<demod_state*>(arg);
    struct output_state* o = d->output_target;
    maybe_set_thread_realtime_and_affinity("DEMOD");
    /* Optional supervisory tuner autogain (env-gated).
       Goals:
       - Avoid constant twitching: throttle adjustments and add retune/scan holdoff.
       - Be conservative on quiet systems: allow down-steps anytime on clipping,
         but only permit up-steps when a carrier is actually present (squelch open
         and reasonable SNR).
       - If device starts in driver auto-gain, we may exit into manual on threshold
         events and continue supervising thereafter. */
    static int ag_initialized = 0;
    static int ag_blocks = 0, ag_high = 0, ag_low = 0;
    static int ag_manual_target = 180;    /* 18.0 dB initial manual target if needed */
    static int ag_target_initialized = 0; /* latched from current driver gain on first window */
    /* Adjustment throttle (ms) and retune holdoff (ms) */
    static auto ag_next_allowed = std::chrono::steady_clock::now();
    static auto ag_hold_until = std::chrono::steady_clock::time_point{};
    static auto ag_probe_until = std::chrono::steady_clock::time_point{};
    static uint32_t ag_last_freq = 0;
    const int ag_throttle_ms = 1500; /* min interval between changes */
    const int ag_hold_ms = 1200;     /* pause after retune/scanning before adjusting */
    /* Up-step gating: now uses spectral SNR + in-band power ratio (see below). */
    /* Probe window: allow RTL device auto-gain to settle before any takeover. */
    static int s_probe_ms = 3000; /* allow device AGC to work first */
    /* Manual seed gain used when exiting device auto due to persistently low level. */
    static int s_seed_gain_db10 = 300; /* 30.0 dB in tenth-dB units */
    /* Spectral gating for up-steps: thresholds and persistence */
    static double s_ag_spec_snr_db = 6.0;   /* min spectral SNR (dB) within channel */
    static double s_ag_inband_ratio = 0.60; /* min fraction of power near center */
    static int s_ag_up_step_db10 = 30;      /* up-step size in tenth-dB (default +3.0 dB) */
    static int s_ag_up_persist = 2;         /* require consecutive passes before stepping up */
    static int ag_spec_pass = 0;            /* persistence counter for spectral gate */
    int logged_once = 0;
    while (!exitflag && !(g_stream && g_stream->should_exit.load())) {
        /* Read a block from input ring */
        int got = input_ring_read_block(&input_ring, d->input_cb_buf, MAXIMUM_BUF_LENGTH);
        if (got <= 0) {
            continue;
        }
        if (!ag_initialized) {
            const char* ag = getenv("DSD_NEO_TUNER_AUTOGAIN");
            // Default OFF. Enable only if explicitly '1', 't', 'y'.
            if (ag && (*ag == '1' || *ag == 't' || *ag == 'T' || *ag == 'y' || *ag == 'Y')) {
                g_tuner_autogain_on.store(1, std::memory_order_relaxed);
            }
            /* Optional env overrides for probe behavior */
            if (const char* ep = getenv("DSD_NEO_TUNER_AUTOGAIN_PROBE_MS")) {
                int v = atoi(ep);
                if (v >= 0 && v <= 20000) {
                    s_probe_ms = v;
                }
            }
            if (const char* eg = getenv("DSD_NEO_TUNER_AUTOGAIN_SEED_DB")) {
                double v = atof(eg);
                if (v >= 0.0 && v <= 60.0) {
                    s_seed_gain_db10 = (int)lrint(v * 10.0);
                }
            }
            /* Optional env overrides for spectral SNR gating */
            if (const char* esn = getenv("DSD_NEO_TUNER_AUTOGAIN_SPEC_SNR_DB")) {
                double v = atof(esn);
                if (v >= 0.0 && v <= 60.0) {
                    s_ag_spec_snr_db = v;
                }
            }
            if (const char* eir = getenv("DSD_NEO_TUNER_AUTOGAIN_INBAND_RATIO")) {
                char* endp = NULL;
                double v = strtod(eir, &endp);
                if (endp && eir != endp && v >= 0.10 && v <= 0.95) {
                    s_ag_inband_ratio = v;
                }
            }
            if (const char* eus = getenv("DSD_NEO_TUNER_AUTOGAIN_UP_STEP_DB")) {
                double v = atof(eus);
                if (v >= 1.0 && v <= 10.0) {
                    s_ag_up_step_db10 = (int)lrint(v * 10.0);
                }
            }
            if (const char* epp = getenv("DSD_NEO_TUNER_AUTOGAIN_UP_PERSIST")) {
                int v = atoi(epp);
                if (v >= 1 && v <= 5) {
                    s_ag_up_persist = v;
                }
            }
            ag_initialized = 1;
        }
        /* Update simple occupancy metrics on pre-DSP input for autogain */
        if (g_tuner_autogain_on.load(std::memory_order_relaxed)) {
            /* Detect retune and apply short holdoff to avoid reacting on noise */
            if (ag_last_freq != dongle.freq) {
                ag_last_freq = dongle.freq;
                ag_blocks = ag_high = ag_low = 0;
                ag_hold_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ag_hold_ms);
                /* On retune, defer any takeover to allow device auto to settle */
                if (s_probe_ms > 0) {
                    ag_probe_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s_probe_ms);
                } else {
                    ag_probe_until = std::chrono::steady_clock::time_point{};
                }
                ag_spec_pass = 0; /* reset spectral gate persistence on retune */
            }
            int max_abs = 0;
            int64_t sum_abs = 0;
            int pairs = got >> 1;
            const int16_t* p = d->input_cb_buf;
            for (int n = 0; n < pairs; n++) {
                int I = p[(size_t)(n << 1) + 0];
                int Q = p[(size_t)(n << 1) + 1];
                int aI = (I >= 0) ? I : -I;
                int aQ = (Q >= 0) ? Q : -Q;
                int m = (aI > aQ) ? aI : aQ;
                if (m > max_abs) {
                    max_abs = m;
                }
                sum_abs += (aI + aQ);
            }
            int mean_abs = (pairs > 0) ? (int)(sum_abs / pairs) : 0;
            ag_blocks++;
            if (max_abs > 30000) {
                ag_high++;
            }
            if (mean_abs < 2000) {
                ag_low++;
            }
            /* Every ~40 blocks, consider a nudge */
            if (ag_blocks >= 40) {
                /* Initialize manual target from current driver state once */
                if (!ag_target_initialized) {
                    int cg = rtl_device_get_tuner_gain(rtl_device_handle);
                    int is_auto_boot = rtl_device_is_auto_gain(rtl_device_handle);
                    /* If the device is already in manual mode, honor the exact manual setting, including 0 dB.
                       When still in device auto, skip latching (to avoid capturing a synthetic 0).
                       We'll bootstrap out of auto separately if needed. */
                    if (!is_auto_boot && cg >= 0) {
                        ag_manual_target = cg;
                    }
                    ag_target_initialized = 1;
                }
                /* Respect retune holdoff and throttle */
                auto now = std::chrono::steady_clock::now();
                bool in_hold = (ag_hold_until.time_since_epoch().count() != 0) && (now < ag_hold_until);
                bool in_probe = (ag_probe_until.time_since_epoch().count() != 0) && (now < ag_probe_until)
                                && (rtl_device_is_auto_gain(rtl_device_handle) > 0);
                bool throttled = now < ag_next_allowed;

                if (!in_hold && !throttled) {
                    int is_auto = rtl_device_is_auto_gain(rtl_device_handle);
                    bool changed = false;
                    /* During probe window, let device AGC act; skip takeover/adjustments unless clipping. */
                    if (in_probe) {
                        if (ag_high >= 3) {
                            /* Severe clipping even in auto: take control and step down. */
                            int seed = s_seed_gain_db10;
                            if (seed < 0) {
                                seed = 0;
                            }
                            if (seed > 490) {
                                seed = 490;
                            }
                            ag_manual_target = seed - 50; /* start slightly below seed when clipping */
                            if (ag_manual_target < 0) {
                                ag_manual_target = 0;
                            }
                            rtl_device_set_gain_nearest(rtl_device_handle, ag_manual_target);
                            dongle.gain = ag_manual_target;
                            ag_next_allowed = now + std::chrono::milliseconds(ag_throttle_ms);
                            LOG_INFO("AUTOGAIN: exiting probe due to clipping; set ~%d.%d dB.\n", ag_manual_target / 10,
                                     ag_manual_target % 10);
                        }
                        goto after_adjustments; /* skip below logic during probe */
                    }
                    /* One-time bootstrap: if device is still in auto and input level is consistently low,
                       exit auto into a reasonable manual gain even when SNR is not yet measurable. */
                    if (is_auto > 0 && ag_high == 0 && ag_low >= (ag_blocks * 3) / 4) {
                        int kick = s_seed_gain_db10;
                        if (kick < 0) {
                            kick = 0;
                        }
                        if (kick > 490) {
                            kick = 490;
                        }
                        rtl_device_set_gain_nearest(rtl_device_handle, kick);
                        dongle.gain = kick;
                        ag_manual_target = kick; /* keep target in sync with seeded manual gain */
                        ag_next_allowed = now + std::chrono::milliseconds(ag_throttle_ms);
                        LOG_INFO("AUTOGAIN: bootstrapping from device auto to ~%d.%d dB due to low input level.\n",
                                 kick / 10, kick % 10);
                        /* After exiting auto, subsequent adjustments use normal thresholds */
                        changed = false; /* already applied */
                    }
                    /* Always allow downward steps on clipping */
                    if (ag_high >= 3) {
                        ag_manual_target -= 50; /* -5.0 dB */
                        if (ag_manual_target < 0) {
                            ag_manual_target = 0;
                        }
                        changed = true;
                    } else {
                        /* Only consider upward steps when squelch gate is open and spectral SNR indicates
                           a dominant in-band carrier. Avoid chasing spurs or OOB energy by requiring:
                           - spectral SNR >= threshold (peak - median)
                           - energy concentrated near DC (central band ratio)
                           - reject sharp isolated DC spikes
                           - persistence over s_ag_up_persist windows */
                        bool spec_ok = false;
                        if (d->squelch_gate_open) {
                            const int kLocalMaxSpec = 1024;
                            float spec_db[kLocalMaxSpec];
                            int rate_hz = 0;
                            int N = dsd_rtl_stream_spectrum_get(spec_db, kLocalMaxSpec, &rate_hz);
                            if (N >= 64 && N <= kLocalMaxSpec) {
                                int i_max = 0;
                                float p_max = -1e30f;
                                for (int i = 0; i < N; i++) {
                                    float v = spec_db[i];
                                    if (v > p_max) {
                                        p_max = v;
                                        i_max = i;
                                    }
                                }
                                /* median noise (dB) */
                                float tmp_med[kLocalMaxSpec];
                                for (int i = 0; i < N; i++) {
                                    tmp_med[i] = spec_db[i];
                                }
                                int mid = N / 2;
                                std::nth_element(tmp_med, tmp_med + mid, tmp_med + N);
                                float noise_med_db = tmp_med[mid];
                                float spec_snr_db = p_max - noise_med_db;
                                /* DC spur guard */
                                int k_center = N / 2;
                                bool dc_spur = false;
                                if (i_max == k_center && i_max > 0 && i_max + 1 < N) {
                                    float l = spec_db[i_max - 1];
                                    float r = spec_db[i_max + 1];
                                    float side_max = (l > r) ? l : r;
                                    if ((p_max - side_max) > 12.0f) {
                                        dc_spur = true;
                                    }
                                }
                                /* In-band ratio: central +/- N/8 bins */
                                int half = N / 8;
                                if (half < 2) {
                                    half = 2;
                                }
                                int i0 = k_center - half;
                                int i1 = k_center + half;
                                if (i0 < 0) {
                                    i0 = 0;
                                }
                                if (i1 > (N - 1)) {
                                    i1 = N - 1;
                                }
                                double sum_all = 0.0, sum_center = 0.0;
                                for (int i = 0; i < N; i++) {
                                    /* convert dB to linear power */
                                    double p = pow(10.0, (double)spec_db[i] / 10.0);
                                    sum_all += p;
                                    if (i >= i0 && i <= i1) {
                                        sum_center += p;
                                    }
                                }
                                double ratio_center = (sum_all > 0.0) ? (sum_center / sum_all) : 0.0;
                                /* Require peak within central band */
                                bool peak_in_center = (i_max >= i0 && i_max <= i1);
                                bool gate = (!dc_spur) && peak_in_center && (spec_snr_db >= s_ag_spec_snr_db)
                                            && (ratio_center >= s_ag_inband_ratio);
                                if (gate) {
                                    ag_spec_pass++;
                                } else {
                                    ag_spec_pass = 0;
                                }
                                spec_ok = (ag_spec_pass >= s_ag_up_persist);
                            } else {
                                ag_spec_pass = 0;
                            }
                        } else {
                            ag_spec_pass = 0;
                        }
                        if (spec_ok && ag_low >= (ag_blocks * 3) / 4) {
                            ag_manual_target += s_ag_up_step_db10; /* up-step */
                            if (ag_manual_target > 490) {
                                ag_manual_target = 490;
                            }
                            changed = true;
                        }
                    }

                    if (changed) {
                        /* Apply manual gain near target regardless of prior auto/manual. */
                        rtl_device_set_gain_nearest(rtl_device_handle, ag_manual_target);
                        dongle.gain = ag_manual_target;
                        ag_next_allowed = now + std::chrono::milliseconds(ag_throttle_ms);
                        ag_spec_pass = 0; /* reset persistence after applying */
                        if (is_auto > 0) {
                            LOG_INFO("AUTOGAIN: threshold hit; exiting device auto and setting ~%d.%d dB.\n",
                                     ag_manual_target / 10, ag_manual_target % 10);
                        } else {
                            LOG_INFO("AUTOGAIN: adjusting manual gain to ~%d.%d dB.\n", ag_manual_target / 10,
                                     ag_manual_target % 10);
                        }
                    }
                after_adjustments:;
                }
                ag_blocks = ag_high = ag_low = 0;
            }
        }
        d->lowpassed = d->input_cb_buf;
        d->lp_len = got;
        full_demod(d);
        /* Capture decimated I/Q for constellation view after DSP. */
        extern void constellation_ring_append(const int16_t* iq, int len, int sps_hint);
        constellation_ring_append(d->lowpassed, d->lp_len, d->ted_sps);
        /* Capture I-channel for eye diagram */
        eye_ring_append_i_chan(d->lowpassed, d->lp_len);
        /* Update spectrum snapshot from post-filter complex baseband */
        rtl_metrics_update_spectrum_from_iq(d->lowpassed, d->lp_len, d->rate_out);

        /* Estimate SNR per modulation using post-filter samples */
        static int c4fm_missed = 0; /* counts loops without C4FM SNR update */
        static int qpsk_missed = 0; /* counts loops without QPSK SNR update */
        static int gfsk_missed = 0; /* counts loops without GFSK SNR update */
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
            bool qpsk_updated = false;
            if (d->cqpsk_enable) {
                enum { MAXP = 2048 };

                static int16_t syms[(size_t)MAXP * 2];
                int n = cqpsk_eq_get_symbols(&demod.cqpsk_eq, syms, MAXP);
                if (n > 32) {
                    /* Use per-axis normalization to avoid bias from I/Q gain imbalance.
                       Estimate target amplitudes from mean absolute I and Q. */
                    double sum_abs_i = 0.0, sum_abs_q = 0.0;
                    for (int i = 0; i < n; i++) {
                        double I = (double)syms[(size_t)(i << 1) + 0];
                        double Q = (double)syms[(size_t)(i << 1) + 1];
                        sum_abs_i += fabs(I);
                        sum_abs_q += fabs(Q);
                    }
                    double aI = sum_abs_i / (double)n;
                    double aQ = sum_abs_q / (double)n;
                    /* Guard small values */
                    if (aI < 1e-9) {
                        aI = 1e-9;
                    }
                    if (aQ < 1e-9) {
                        aQ = 1e-9;
                    }
                    /* Evaluate both axis-aligned and 45°-diagonal target sets; choose best. */
                    double e2_axis = 0.0;
                    for (int i = 0; i < n; i++) {
                        double I = (double)syms[(size_t)(i << 1) + 0];
                        double Q = (double)syms[(size_t)(i << 1) + 1];
                        double ti = (I >= 0.0) ? aI : -aI;
                        double tq = (Q >= 0.0) ? aQ : -aQ;
                        double ei = I - ti;
                        double eq = Q - tq;
                        e2_axis += ei * ei + eq * eq;
                    }
                    double t2_axis = (double)n * (aI * aI + aQ * aQ);
                    double aD = 0.5 * (aI + aQ); /* diagonal amplitude */
                    if (aD < 1e-9) {
                        aD = 1e-9;
                    }
                    double e2_diag = 0.0;
                    for (int i = 0; i < n; i++) {
                        double I = (double)syms[(size_t)(i << 1) + 0];
                        double Q = (double)syms[(size_t)(i << 1) + 1];
                        double ti = (I >= 0.0) ? aD : -aD;
                        double tq = (Q >= 0.0) ? aD : -aD;
                        double ei = I - ti;
                        double eq = Q - tq;
                        e2_diag += ei * ei + eq * eq;
                    }
                    double t2_diag = (double)n * (2.0 * aD * aD);
                    double snr = -100.0;
                    if (t2_axis > 1e-9 && e2_axis > 0.0) {
                        double ratio = (e2_axis <= 1e-12) ? 1e12 : (t2_axis / e2_axis);
                        snr = 10.0 * log10(ratio);
                    }
                    if (t2_diag > 1e-9 && e2_diag > 0.0) {
                        double ratio_d = (e2_diag <= 1e-12) ? 1e12 : (t2_diag / e2_diag);
                        double snr_d = 10.0 * log10(ratio_d);
                        if (snr_d > snr) {
                            snr = snr_d;
                        }
                    }
                    if (snr > -99.0) {
                        snr -= kEvmSNRNoiseBiasDb;
                        static double ema = -100.0;
                        if (ema < -50.0) {
                            ema = snr;
                        } else {
                            /* Make SNR meter snappier: 60% old, 40% new */
                            ema = 0.6 * ema + 0.4 * snr;
                        }
                        g_snr_qpsk_db.store(ema, std::memory_order_relaxed);
                        g_snr_qpsk_src.store(1, std::memory_order_relaxed);
                        auto now = std::chrono::steady_clock::now();
                        long long ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        g_snr_qpsk_last_ms.store(ms, std::memory_order_relaxed);
                        qpsk_updated = true;
                    }
                }
            }
            bool c4fm_updated = false;
            bool gfsk_updated = false;
            /* Include low-SPS FSK paths (e.g., 5 sps ProVoice/EDACS) in the SNR estimator. */
            if (sps >= 4 && sps <= 12) {
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
                    /* Compute quartiles in O(n) using nth_element */
                    int idx1 = (int)((size_t)m / 4);
                    int idx2 = (int)((size_t)m / 2);
                    int idx3 = (int)((size_t)(3 * (size_t)m) / 4);
                    std::nth_element(vals, vals + idx2, vals + m);
                    int q2 = vals[idx2];
                    std::nth_element(vals, vals + idx1, vals + idx2);
                    int q1 = vals[idx1];
                    std::nth_element(vals + idx2 + 1, vals + idx3, vals + m);
                    int q3 = vals[idx3];
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
                                    double snr = 10.0 * log10(sig_var / noise_var) - kC4fmSNRNoiseBiasDb;
                                    static double ema = -100.0;
                                    if (ema < -50.0) {
                                        ema = snr;
                                    } else {
                                        /* Make SNR meter snappier: 60% old, 40% new */
                                        ema = 0.6 * ema + 0.4 * snr;
                                    }
                                    g_snr_c4fm_db.store(ema, std::memory_order_relaxed);
                                    g_snr_c4fm_src.store(1, std::memory_order_relaxed);
                                    auto now = std::chrono::steady_clock::now();
                                    long long ms =
                                        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                                            .count();
                                    g_snr_c4fm_last_ms.store(ms, std::memory_order_relaxed);
                                    c4fm_updated = true;
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
                                        double snr = 10.0 * log10(sig_var / noise_var) - kEvmSNRNoiseBiasDb;
                                        static double ema = -100.0;
                                        if (ema < -50.0) {
                                            ema = snr;
                                        } else {
                                            /* Make SNR meter snappier: 60% old, 40% new */
                                            ema = 0.6 * ema + 0.4 * snr;
                                        }
                                        g_snr_gfsk_db.store(ema, std::memory_order_relaxed);
                                        g_snr_gfsk_src.store(1, std::memory_order_relaxed);
                                        auto now = std::chrono::steady_clock::now();
                                        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           now.time_since_epoch())
                                                           .count();
                                        g_snr_gfsk_last_ms.store(ms, std::memory_order_relaxed);
                                        gfsk_updated = true;
                                    }
                                }
                            }
                        }
                    }
                    /* Record that we updated C4FM within this loop */
                    c4fm_updated = true;
                }
            }
            /* If no C4FM update occurred for a while, synthesize a value from the eye buffer */
            if (!c4fm_updated) {
                if (++c4fm_missed >= 50) { /* ~guard against long stalls */
                    double fb = dsd_rtl_stream_estimate_snr_c4fm_eye();
                    if (fb > -50.0) {
                        double prev = g_snr_c4fm_db.load(std::memory_order_relaxed);
                        /* Slightly quicker fallback blending to avoid long stalls */
                        double blended = (prev < -50.0) ? fb : (0.8 * prev + 0.2 * fb);
                        g_snr_c4fm_db.store(blended, std::memory_order_relaxed);
                        g_snr_c4fm_src.store(2, std::memory_order_relaxed);
                        auto now = std::chrono::steady_clock::now();
                        long long ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        g_snr_c4fm_last_ms.store(ms, std::memory_order_relaxed);
                    }
                    c4fm_missed = 0;
                }
            } else {
                c4fm_missed = 0;
            }
            /* If no QPSK update, synthesize from the constellation snapshot */
            if (!qpsk_updated) {
                if (++qpsk_missed >= 50) {
                    double fb = dsd_rtl_stream_estimate_snr_qpsk_const();
                    if (fb > -50.0) {
                        double prev = g_snr_qpsk_db.load(std::memory_order_relaxed);
                        /* Slightly quicker fallback blending to avoid long stalls */
                        double blended = (prev < -50.0) ? fb : (0.8 * prev + 0.2 * fb);
                        g_snr_qpsk_db.store(blended, std::memory_order_relaxed);
                        g_snr_qpsk_src.store(2, std::memory_order_relaxed);
                        auto now = std::chrono::steady_clock::now();
                        long long ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        g_snr_qpsk_last_ms.store(ms, std::memory_order_relaxed);
                    }
                    qpsk_missed = 0;
                }
            } else {
                qpsk_missed = 0;
            }
            /* If no GFSK update, synthesize from the eye buffer */
            if (!gfsk_updated) {
                if (++gfsk_missed >= 50) {
                    double fb = dsd_rtl_stream_estimate_snr_gfsk_eye();
                    if (fb > -50.0) {
                        double prev = g_snr_gfsk_db.load(std::memory_order_relaxed);
                        /* Slightly quicker fallback blending to avoid long stalls */
                        double blended = (prev < -50.0) ? fb : (0.8 * prev + 0.2 * fb);
                        g_snr_gfsk_db.store(blended, std::memory_order_relaxed);
                        g_snr_gfsk_src.store(2, std::memory_order_relaxed);
                        auto now = std::chrono::steady_clock::now();
                        long long ms =
                            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                        g_snr_gfsk_last_ms.store(ms, std::memory_order_relaxed);
                    }
                    gfsk_missed = 0;
                }
            } else {
                gfsk_missed = 0;
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
            /* Small adjustment: prefer capture rates that fall near known-stable
               RTL2832U clocks (e.g., 960k, 1024k, 1200k, 1536k, 1920k, 2048k, 2400k).
               We keep the power-of-two structure by choosing among nearby pass counts. */
            auto choose_passes_near_good_rate = [&](int rate_in_hz, int suggested_passes) {
                const int good_rates[] = {960000, 1024000, 1200000, 1536000, 1920000, 2048000, 2400000};
                int best_p = suggested_passes;
                long long best_err = LLONG_MAX;
                for (int delta = -1; delta <= 1; delta++) {
                    int p = suggested_passes + delta;
                    if (p < 0) {
                        p = 0;
                    }
                    if (p > 10) {
                        p = 10;
                    }
                    long long cap = (long long)rate_in_hz * (long long)(1 << p);
                    /* stay in a reasonable RTL range */
                    if (cap < 225000LL || cap > 3200000LL) {
                        continue;
                    }
                    for (size_t i = 0; i < sizeof(good_rates) / sizeof(good_rates[0]); i++) {
                        long long err = cap - (long long)good_rates[i];
                        if (err < 0) {
                            err = -err;
                        }
                        if (err < best_err) {
                            best_err = err;
                            best_p = p;
                        }
                    }
                }
                return best_p;
            };
            int adj_passes = choose_passes_near_good_rate(dm->rate_in, passes);
            dm->downsample_passes = adj_passes;
            dm->downsample = 1 << adj_passes;
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
    if (dm->mode_demod == &dsd_fm_demod) {
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
    /* Use driver auto hardware bandwidth by default, or override via env */
    rtl_device_set_tuner_bandwidth(rtl_device_handle, choose_tuner_bw_hz(dongle.rate, (uint32_t)rtl_dsp_bw_hz));
    /* Sync to actual device rate (USB may quantize). If it changed, update rate_out. */
    int actual = rtl_device_get_sample_rate(rtl_device_handle);
    if (actual > 0 && (uint32_t)actual != dongle.rate) {
        uint32_t prev = dongle.rate;
        dongle.rate = (uint32_t)actual;
        int base_decim = (demod.downsample_passes > 0) ? (1 << demod.downsample_passes)
                                                       : ((demod.downsample > 0) ? demod.downsample : 1);
        if (base_decim < 1) {
            base_decim = 1;
        }
        int out_rate = (int)(dongle.rate / (uint32_t)base_decim);
        if (demod.post_downsample > 1) {
            out_rate /= demod.post_downsample;
            if (out_rate < 1) {
                out_rate = 1;
            }
        }
        demod.rate_out = out_rate;
        LOG_INFO("Adjusted to actual device rate: requested=%u, actual=%u, demod_out=%d Hz.\n", prev, dongle.rate,
                 demod.rate_out);
    }
    /* Ensure TED SPS reflects the current effective sampling rate unless explicitly overridden. */
    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, g_stream ? g_stream->opts : NULL, &output);
}

/* Resampler and TED SPS helpers are implemented in rtl_demod_config.cpp. */

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
        rtl_device_set_direct_sampling(rtl_device_handle, dongle.direct_sampling);
    }

    /* Try enabling offset tuning before any other tuning calls (rtl_fm order).
       Respect explicit env override when provided. */
    {
        int want = 1;
        if (const char* ot = getenv("DSD_NEO_RTL_OFFSET_TUNING")) {
            want = (ot[0] != '0' && ot[0] != 'n' && ot[0] != 'N' && ot[0] != 'f' && ot[0] != 'F') ? 1 : 0;
        }
        int r = rtl_device_set_offset_tuning_enabled(rtl_device_handle, want);
        if (r == 0) {
            dongle.offset_tuning = want ? 1 : 0;
        } else {
            dongle.offset_tuning = 0;
        }
    }

    /* Recompute capture parameters now that offset_tuning may have changed. */
    optimal_settings(s->freqs[0], demod.rate_in);

    /* Set the frequency then sample rate (rtl_fm order). */
    rtl_device_set_frequency(rtl_device_handle, dongle.freq);
    LOG_INFO("Oversampling input by: %ix.\n", demod.downsample);
    LOG_INFO("Oversampling output by: %ix.\n", demod.post_downsample);
    LOG_INFO("Buffer size: %0.2fms\n", 1000 * 0.5 * (float)ACTUAL_BUF_LENGTH / (float)dongle.rate);

    /* Set the sample rate after frequency */
    rtl_device_set_sample_rate(rtl_device_handle, dongle.rate);
    /* USB may quantize the applied sample rate; sync and update out rate if changed. */
    {
        int actual = rtl_device_get_sample_rate(rtl_device_handle);
        if (actual > 0 && (uint32_t)actual != dongle.rate) {
            uint32_t prev = dongle.rate;
            dongle.rate = (uint32_t)actual;
            int base_decim = (demod.downsample_passes > 0) ? (1 << demod.downsample_passes)
                                                           : ((demod.downsample > 0) ? demod.downsample : 1);
            if (base_decim < 1) {
                base_decim = 1;
            }
            int out_rate = (int)(dongle.rate / (uint32_t)base_decim);
            if (demod.post_downsample > 1) {
                out_rate /= demod.post_downsample;
                if (out_rate < 1) {
                    out_rate = 1;
                }
            }
            demod.rate_out = out_rate;
            LOG_INFO("Adjusted to actual device rate: requested=%u, actual=%u, demod_out=%d Hz.\n", prev, dongle.rate,
                     demod.rate_out);
        }
    }
    /* Apply tuner IF bandwidth with mode-aware heuristic */
    rtl_device_set_tuner_bandwidth(rtl_device_handle, choose_tuner_bw_hz(dongle.rate, (uint32_t)rtl_dsp_bw_hz));
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
            rtl_demod_maybe_update_resampler_after_rate_change(&demod, &output, rtl_dsp_bw_hz);
            rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, g_stream ? g_stream->opts : NULL, &output);
            /* Reset demod/FLL/TED and clear any stale buffers on retune */
            demod_reset_on_retune(&demod);
            input_ring_clear(&input_ring);
            /* Dynamically mute a short slice of incoming bytes to flush tuner/transient tail. */
            {
                int mute_ms = 10; /* ~10ms default */
                int64_t bytes = ((int64_t)dongle.rate * 2LL * (int64_t)mute_ms) / 1000LL;
                if (bytes < BUFFER_DUMP) {
                    bytes = BUFFER_DUMP; /* keep previous minimum */
                }
                if (bytes > INT32_MAX) {
                    bytes = INT32_MAX;
                }
                rtl_device_mute(rtl_device_handle, (int)bytes);
            }
            drain_output_on_retune();
            LOG_INFO("Retune applied: %u Hz.\n", tgt);
            continue;
        }
        if (s->freq_len <= 1) {
            continue;
        }
        s->freq_now = (s->freq_now + 1) % s->freq_len;
        apply_capture_settings((uint32_t)s->freqs[s->freq_now]);
        rtl_demod_maybe_update_resampler_after_rate_change(&demod, &output, rtl_dsp_bw_hz);
        rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, g_stream ? g_stream->opts : NULL, &output);
        /* Reset demod/FLL/TED and clear any stale buffers on hop */
        demod_reset_on_retune(&demod);
        input_ring_clear(&input_ring);
        /* Dynamically mute a short slice of incoming bytes to flush tuner/transient tail. */
        {
            int mute_ms = 10; /* ~10ms default */
            int64_t bytes = ((int64_t)dongle.rate * 2LL * (int64_t)mute_ms) / 1000LL;
            if (bytes < BUFFER_DUMP) {
                bytes = BUFFER_DUMP; /* keep previous minimum */
            }
            if (bytes > INT32_MAX) {
                bytes = INT32_MAX;
            }
            rtl_device_mute(rtl_device_handle, (int)bytes);
        }
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

/* ---------------- Eye-based SNR estimation (C4FM fallback) ---------------- */
extern "C" double
dsd_rtl_stream_estimate_snr_c4fm_eye(void) {
    enum { MAXS = 4096 };

    static int16_t eb[(size_t)MAXS];
    int sps_fb = 0;
    int nfb = dsd_rtl_stream_eye_get(eb, MAXS, &sps_fb);
    if (nfb <= 100 || sps_fb <= 0) {
        return -100.0;
    }
    int two_sps = 2 * sps_fb;
    int c1 = sps_fb / 2;
    int c2 = (3 * sps_fb) / 2;
    int win = sps_fb / 10;
    if (win < 1) {
        win = 1;
    }
    /* Quartiles over a downsampled set */
    int step_ds = (nfb > 4096) ? (nfb / 4096) : 1;
    int mct = (nfb + step_ds - 1) / step_ds;
    if (mct > 4096) {
        mct = 4096;
    }
    static int qv[4096];
    int vi = 0;
    for (int i = 0; i < nfb && vi < mct; i += step_ds) {
        qv[vi++] = (int)eb[i];
    }
    mct = vi;
    if (mct < 8) {
        return -100.0;
    }
    /* Quartiles via nth_element (O(n)) */
    int idx1 = (int)((size_t)mct / 4);
    int idx2 = (int)((size_t)mct / 2);
    int idx3 = (int)((size_t)(3 * (size_t)mct) / 4);
    std::nth_element(qv, qv + idx2, qv + mct);
    int q2 = qv[idx2];
    std::nth_element(qv, qv + idx1, qv + idx2);
    int q1 = qv[idx1];
    std::nth_element(qv + idx2 + 1, qv + idx3, qv + mct);
    int q3 = qv[idx3];
    long long cnt[4] = {0, 0, 0, 0};
    double sum[4] = {0, 0, 0, 0};
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        int inwin = (abs(phase - c1) <= win) || (abs(phase - c2) <= win);
        if (!inwin) {
            continue;
        }
        int v = (int)eb[i];
        int b = (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
        cnt[b]++;
        sum[b] += (double)v;
    }
    long long total = cnt[0] + cnt[1] + cnt[2] + cnt[3];
    if (!(total > 50 && cnt[0] && cnt[1] && cnt[2] && cnt[3])) {
        return -100.0;
    }
    double mu[4];
    for (int b = 0; b < 4; b++) {
        mu[b] = sum[b] / (double)cnt[b];
    }
    double nsum = 0.0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        int inwin = (abs(phase - c1) <= win) || (abs(phase - c2) <= win);
        if (!inwin) {
            continue;
        }
        int v = (int)eb[i];
        int b = (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
        double e = (double)v - mu[b];
        nsum += e * e;
    }
    double noise_var = nsum / (double)total;
    if (noise_var <= 1e-9) {
        return -100.0;
    }
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
    if (sig_var <= 1e-9) {
        return -100.0;
    }
    return 10.0 * log10(sig_var / noise_var) - kC4fmSNRNoiseBiasDb;
}

/* ---------------- Constellation-based SNR estimation (QPSK fallback) ---------------- */
extern "C" double
dsd_rtl_stream_estimate_snr_qpsk_const(void) {
    enum { MAXP = 4096 };

    static int16_t xy[(size_t)MAXP * 2];
    int n = dsd_rtl_stream_constellation_get(xy, MAXP);
    if (n <= 64) {
        return -100.0;
    }
    /* Per-axis normalization to mitigate I/Q gain imbalance in raw snapshot */
    double sum_abs_i = 0.0, sum_abs_q = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        sum_abs_i += fabs(I);
        sum_abs_q += fabs(Q);
    }
    double aI = sum_abs_i / (double)n;
    double aQ = sum_abs_q / (double)n;
    if (!(aI > 1e-9 && aQ > 1e-9)) {
        return -100.0;
    }
    /* Evaluate both axis-aligned and 45°-diagonal targets; choose best. */
    double e2_axis = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        double ti = (I >= 0.0) ? aI : -aI;
        double tq = (Q >= 0.0) ? aQ : -aQ;
        double ei = I - ti;
        double eq = Q - tq;
        e2_axis += ei * ei + eq * eq;
    }
    double t2_axis = (double)n * (aI * aI + aQ * aQ);
    double aD = 0.5 * (aI + aQ);
    if (aD < 1e-9) {
        aD = 1e-9;
    }
    double e2_diag = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        double ti = (I >= 0.0) ? aD : -aD;
        double tq = (Q >= 0.0) ? aD : -aD;
        double ei = I - ti;
        double eq = Q - tq;
        e2_diag += ei * ei + eq * eq;
    }
    double t2_diag = (double)n * (2.0 * aD * aD);
    double best_snr = -100.0;
    if (t2_axis > 1e-9 && e2_axis > 0.0) {
        double ratio = (e2_axis <= 1e-12) ? 1e12 : (t2_axis / e2_axis);
        best_snr = 10.0 * log10(ratio);
    }
    if (t2_diag > 1e-9 && e2_diag > 0.0) {
        double ratio_d = (e2_diag <= 1e-12) ? 1e12 : (t2_diag / e2_diag);
        double snr_d = 10.0 * log10(ratio_d);
        if (snr_d > best_snr) {
            best_snr = snr_d;
        }
    }
    return best_snr - kEvmSNRNoiseBiasDb;
}

/* ---------------- Eye-based SNR estimation (GFSK fallback, 2-level) ---------------- */
extern "C" double
dsd_rtl_stream_estimate_snr_gfsk_eye(void) {
    enum { MAXS = 4096 };

    static int16_t eb[(size_t)MAXS];
    int sps_fb = 0;
    int nfb = dsd_rtl_stream_eye_get(eb, MAXS, &sps_fb);
    if (nfb <= 100 || sps_fb <= 0) {
        return -100.0;
    }
    int two_sps = 2 * sps_fb;
    int c1 = sps_fb / 2;
    int c2 = (3 * sps_fb) / 2;
    int win = sps_fb / 10;
    if (win < 1) {
        win = 1;
    }
    /* Downsample */
    int step_ds = (nfb > 4096) ? (nfb / 4096) : 1;
    int mct = (nfb + step_ds - 1) / step_ds;
    if (mct > 4096) {
        mct = 4096;
    }
    static int qv[4096];
    int vi = 0;
    for (int i = 0; i < nfb && vi < mct; i += step_ds) {
        qv[vi++] = (int)eb[i];
    }
    mct = vi;
    if (mct < 8) {
        return -100.0;
    }
    /* Median via nth_element (O(n)) */
    int idx2 = (int)((size_t)mct / 2);
    std::nth_element(qv, qv + idx2, qv + mct);
    int q2 = qv[idx2]; /* median split */
    double sumL = 0.0, sumH = 0.0;
    int cntL = 0, cntH = 0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        int inwin = (abs(phase - c1) <= win) || (abs(phase - c2) <= win);
        if (!inwin) {
            continue;
        }
        int v = (int)eb[i];
        if (v <= q2) {
            sumL += v;
            cntL++;
        } else {
            sumH += v;
            cntH++;
        }
    }
    if (cntL == 0 || cntH == 0) {
        return -100.0;
    }
    double muL = sumL / (double)cntL, muH = sumH / (double)cntH;
    int total = cntL + cntH;
    double nsum = 0.0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        int inwin = (abs(phase - c1) <= win) || (abs(phase - c2) <= win);
        if (!inwin) {
            continue;
        }
        int v = (int)eb[i];
        double mu = (v <= q2) ? muL : muH;
        double e = (double)v - mu;
        nsum += e * e;
    }
    double noise_var = nsum / (double)total;
    if (noise_var <= 1e-9) {
        return -100.0;
    }
    double mu_all = (muL * (double)cntL + muH * (double)cntH) / (double)total;
    double ssum = (double)cntL * (muL - mu_all) * (muL - mu_all) + (double)cntH * (muH - mu_all) * (muH - mu_all);
    double sig_var = ssum / (double)total;
    if (sig_var <= 1e-9) {
        return -100.0;
    }
    return 10.0 * log10(sig_var / noise_var) - kEvmSNRNoiseBiasDb;
}

/* Auto-PPM status (spectrum-based) is implemented in rtl_metrics.cpp. */
extern std::atomic<int> g_auto_ppm_enabled;
extern std::atomic<int> g_auto_ppm_user_en;
extern std::atomic<int> g_auto_ppm_locked;
extern std::atomic<int> g_auto_ppm_training;
extern std::atomic<int> g_auto_ppm_lock_ppm;
extern std::atomic<double> g_auto_ppm_lock_snr_db;
extern std::atomic<double> g_auto_ppm_lock_df_hz;
extern std::atomic<double> g_auto_ppm_snr_db;
extern std::atomic<double> g_auto_ppm_df_hz;
extern std::atomic<double> g_auto_ppm_est_ppm;
extern std::atomic<int> g_auto_ppm_last_dir;
extern std::atomic<int> g_auto_ppm_cooldown;

/* Spectrum, carrier diagnostics, blanker, tuner autogain, and auto-PPM metrics
 * exports are implemented in rtl_metrics.cpp. */

/**
 * @brief Initialize dongle (RTL-SDR source) state with default parameters.
 *
 * @param s Dongle state to initialize.
 */
void
dongle_init(struct dongle_state* s) {
    s->rate = rtl_dsp_bw_hz;
    s->gain = AUTO_GAIN; // tenths of a dB
    s->mute = 0;
    s->direct_sampling = 0;
    s->offset_tuning = 0; //E4000 tuners only
    s->demod_target = &demod;
}

/**
 * @brief Initialize output ring buffer and synchronization primitives.
 *
 * @param s Output state to initialize.
 */
void
output_init(struct output_state* s) {
    s->rate = rtl_dsp_bw_hz;
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
    LOG_INFO("Setting DSP baseband to %d Hz\n", rtl_dsp_bw_hz);
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

/* Forward decls for auto-PPM status helpers */
extern "C" int dsd_rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm,
                                                  int* last_dir, int* cooldown, int* locked);
extern "C" int dsd_rtl_stream_auto_ppm_training_active(void);
extern "C" void dsd_rtl_stream_set_auto_ppm(int onoff);
extern "C" int dsd_rtl_stream_get_auto_ppm(void);

/* Option B: Perform a short auto-PPM pre-training window at startup before returning control,
   so trunking/hunt logic begins after a stable PPM lock when possible. */

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
extern "C" int
dsd_rtl_stream_open(dsd_opts* opts) {

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
    } persist = {};

    rtl_dsp_bw_hz = opts->rtl_dsp_bw_khz * 1000; // base DSP bandwidth in Hz
    /* Honor the user-requested DSP bandwidth directly for the demodulator base rate so
       half-band decimators/resampler scale with the CLI argument (4/6/8/12/16/24 kHz). */
    int demod_base_rate_hz = rtl_dsp_bw_hz;
    /* Apply CLI volume multiplier (1..3), default to 1 if out of range */
    {
        int vm = opts->rtl_volume_multiplier;
        if (vm < 1 || vm > 3) {
            vm = 1;
        }
        volume_multiplier = (short int)vm;
    }

    dongle_init(&dongle);
    rtl_demod_init_for_mode(&demod, &output, opts, demod_base_rate_hz);
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
    rtl_demod_config_from_env_and_opts(&demod, opts);
    rtl_demod_select_defaults_for_mode(&demod, opts, &output);

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

    /* Default: if user did not specify a manual tuner gain (0=AGC), enable
       supervisory tuner autogain unless explicitly disabled via env. This starts
        in device auto-gain and promotes to nearby manual values when needed. */
    if (opts && opts->rtl_gain_value <= 0) {
        // Supervisory auto gain DISABLED by default (user request).
        // Only enable if explicitly requested via env DSD_NEO_TUNER_AUTOGAIN=1.
        const char* ag = getenv("DSD_NEO_TUNER_AUTOGAIN");
        int env_on = (ag && (*ag == '1' || *ag == 't' || *ag == 'T' || *ag == 'y' || *ag == 'Y')) ? 1 : 0;
        if (env_on) {
            g_tuner_autogain_on.store(1, std::memory_order_relaxed);
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

    if (opts && opts->rtltcp_enabled) {
        int autotune = opts->rtltcp_autotune;
        if (!autotune) {
            const char* ea = getenv("DSD_NEO_TCP_AUTOTUNE");
            if (ea && ea[0] != '\0' && ea[0] != '0' && ea[0] != 'f' && ea[0] != 'F' && ea[0] != 'n' && ea[0] != 'N') {
                autotune = 1;
            }
        }
        rtl_device_handle = rtl_device_create_tcp(opts->rtltcp_hostname, opts->rtltcp_portno, &input_ring,
                                                  combine_rotate_enabled, autotune);
        if (!rtl_device_handle) {
            LOG_ERROR("Failed to connect rtl_tcp at %s:%d.\n", opts->rtltcp_hostname, opts->rtltcp_portno);
            return -1;
        } else {
            LOG_INFO("Using rtl_tcp source %s:%d.\n", opts->rtltcp_hostname, opts->rtltcp_portno);
            rtl_device_print_offset_capability(rtl_device_handle);
        }
    } else {
        rtl_device_handle = rtl_device_create(dongle.dev_index, &input_ring, combine_rotate_enabled);
        if (!rtl_device_handle) {
            LOG_ERROR("Failed to open rtlsdr device %d.\n", dongle.dev_index);
            return -1;
        } else {
            LOG_INFO("Using RTLSDR Device Index: %d. \n", dongle.dev_index);
            /* Print tuner and expected offset-tuning capability before any attempts */
            rtl_device_print_offset_capability(rtl_device_handle);
        }
    }

    /* Apply bias tee setting before other tuner config (USB via librtlsdr; rtl_tcp via protocol cmd 0x0E) */
    if (opts && opts->rtl_bias_tee) {
        rtl_device_set_bias_tee(rtl_device_handle, 1);
    }

    /* Advanced RTL-SDR driver options via environment */
    {
        /* Direct sampling selection: DSD_NEO_RTL_DIRECT=0|1|2|I|Q */
        if (const char* ds = getenv("DSD_NEO_RTL_DIRECT")) {
            int mode = 0;
            if (ds[0] == '1' || ds[0] == 'I' || ds[0] == 'i') {
                mode = 1;
            } else if (ds[0] == '2' || ds[0] == 'Q' || ds[0] == 'q') {
                mode = 2;
            } else if (ds[0] == '0') {
                mode = 0;
            } else {
                int v = atoi(ds);
                if (v >= 0 && v <= 2) {
                    mode = v;
                }
            }
            if (mode >= 0 && mode <= 2) {
                rtl_device_set_direct_sampling(rtl_device_handle, mode);
                dongle.direct_sampling = mode;
            }
        }

        /* Offset tuning: DSD_NEO_RTL_OFFSET_TUNING=0|1 (default try enable) */
        if (const char* ot = getenv("DSD_NEO_RTL_OFFSET_TUNING")) {
            int on = (ot[0] != '0' && ot[0] != 'n' && ot[0] != 'N' && ot[0] != 'f' && ot[0] != 'F') ? 1 : 0;
            rtl_device_set_offset_tuning_enabled(rtl_device_handle, on);
            dongle.offset_tuning = on ? 1 : 0;
        }

        /* Xtal frequencies (Hz): DSD_NEO_RTL_XTAL_HZ / DSD_NEO_TUNER_XTAL_HZ */
        uint32_t rtl_xtal_hz = 0, tuner_xtal_hz = 0;
        if (const char* ex = getenv("DSD_NEO_RTL_XTAL_HZ")) {
            long v = strtol(ex, NULL, 10);
            if (v > 0 && v <= 1000000000L) {
                rtl_xtal_hz = (uint32_t)v;
            }
        }
        if (const char* et = getenv("DSD_NEO_TUNER_XTAL_HZ")) {
            long v = strtol(et, NULL, 10);
            if (v > 0 && v <= 1000000000L) {
                tuner_xtal_hz = (uint32_t)v;
            }
        }
        if (rtl_xtal_hz > 0 || tuner_xtal_hz > 0) {
            rtl_device_set_xtal_freq(rtl_device_handle, rtl_xtal_hz, tuner_xtal_hz);
        }

        /* Test mode: DSD_NEO_RTL_TESTMODE=0|1 */
        if (const char* tm = getenv("DSD_NEO_RTL_TESTMODE")) {
            int on = (tm[0] != '0' && tm[0] != 'n' && tm[0] != 'N' && tm[0] != 'f' && tm[0] != 'F') ? 1 : 0;
            rtl_device_set_testmode(rtl_device_handle, on);
        }

        /* IF gains: DSD_NEO_RTL_IF_GAINS="stage:gain[,stage:gain]..." gain in dB or 0.1dB */
        if (const char* ig = getenv("DSD_NEO_RTL_IF_GAINS")) {
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", ig);
            char* save = NULL;
            for (char* tok = strtok_r(buf, ",; ", &save); tok; tok = strtok_r(NULL, ",; ", &save)) {
                int stage = -1;
                double gain_db = 0.0;
                char* colon = strchr(tok, ':');
                if (!colon) {
                    continue;
                }
                *colon = '\0';
                const char* s_stage = tok;
                const char* s_gain = colon + 1;
                stage = atoi(s_stage);
                /* Strip 'dB' suffix if present */
                char gbuf[64];
                snprintf(gbuf, sizeof gbuf, "%s", s_gain);
                size_t gl = strlen(gbuf);
                if (gl >= 2 && (gbuf[gl - 1] == 'B' || gbuf[gl - 1] == 'b')) {
                    gbuf[gl - 1] = '\0';
                    if (gl >= 3 && (gbuf[gl - 2] == 'D' || gbuf[gl - 2] == 'd')) {
                        gbuf[gl - 2] = '\0';
                    }
                }
                gain_db = atof(gbuf);
                int gain_tenth = 0;
                if (strchr(gbuf, '.')) {
                    gain_tenth = (int)lrint(gain_db * 10.0);
                } else {
                    /* Assume already in 0.1 dB if large; else interpret as dB */
                    int gi = atoi(gbuf);
                    gain_tenth = (abs(gi) > 90) ? gi : (gi * 10);
                }
                if (stage >= 0) {
                    rtl_device_set_if_gain(rtl_device_handle, stage, gain_tenth);
                }
            }
        }
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
            const char* approx = dsd_unicode_or_ascii("≈", "~");
            LOG_INFO("Audio LPF enabled: fc%s%d Hz, alpha_q15=%d\n", approx, cutoff_hz, demod.audio_lpf_alpha);
        }
    }

    /* Set the tuner gain */
    rtl_device_set_gain(rtl_device_handle, dongle.gain);
    if (dongle.gain == AUTO_GAIN) {
        LOG_INFO("Setting RTL Autogain. \n");
    }

    rtl_device_set_ppm(rtl_device_handle, dongle.ppm_error);

    /* Prepare initial settings; controller thread will program the device. */
    if (controller.freq_len == 0) {
        controller.freqs[controller.freq_len] = 446000000;
        controller.freq_len++;
    }
    optimal_settings(controller.freqs[0], demod.rate_in);
    if (dongle.direct_sampling) {
        rtl_device_set_direct_sampling(rtl_device_handle, 1);
    }
    LOG_INFO("Oversampling input by: %ix.\n", demod.downsample);
    LOG_INFO("Oversampling output by: %ix.\n", demod.post_downsample);
    LOG_INFO("Buffer size: %0.2fms\n", 1000 * 0.5 * (float)ACTUAL_BUF_LENGTH / (float)dongle.rate);
    LOG_INFO("Demod output at %u Hz.\n", (unsigned int)demod.rate_out);

    /* Recompute resampler with the actual demod output rate now known */
    if (demod.resamp_target_hz > 0) {
        int target = demod.resamp_target_hz;
        int inRate = demod.rate_out > 0 ? demod.rate_out : rtl_dsp_bw_hz;
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
        if (scale > 12) {
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

    /* With demod.rate_out known and resampler configured, refresh TED SPS unless overridden. */
    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, opts, &output);

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
        g_stream->opts = opts;
        g_stream->cfg = dsd_neo_get_config();
        g_stream->should_exit.store(0);
    }

    /* For rtl_tcp sources, optionally prebuffer before starting demod/controller
       to reduce initial under-runs and jitter on the consumer side. */
    if (opts && opts->rtltcp_enabled) {
        int pre_ms = 1000; /* default deeper prebuffer for rtltcp */
        if (const char* e = getenv("DSD_NEO_TCP_PREBUF_MS")) {
            int v = atoi(e);
            if (v >= 5 && v <= 1000) {
                pre_ms = v;
            }
        }

        /* Compute desired prebuffer in int16 samples (I+Q), and ensure the
           input ring is large enough so that half the ring equals the
           requested prebuffer. This provides headroom while starting demod. */
        size_t desired_prebuf = (size_t)((double)dongle.rate * 2.0 * ((double)pre_ms / 1000.0));
        if (desired_prebuf < 16384) {
            desired_prebuf = 16384;
        }
        size_t min_capacity = desired_prebuf * 2; /* use <= 50% for prebuffer */
        if (min_capacity > input_ring.capacity) {
            int16_t* nb = (int16_t*)dsd_neo_aligned_malloc(min_capacity * sizeof(int16_t));
            if (nb) {
                if (input_ring.buffer) {
                    dsd_neo_aligned_free(input_ring.buffer);
                }
                input_ring.buffer = nb;
                input_ring.capacity = min_capacity;
                input_ring.head.store(0);
                input_ring.tail.store(0);
                LOG_INFO("rtltcp resized input ring to %zu samples (%.2f MiB) for ~%d ms prebuffer.\n",
                         input_ring.capacity, (double)input_ring.capacity * sizeof(int16_t) / (1024.0 * 1024.0),
                         pre_ms);
            } else {
                LOG_WARNING("rtltcp: allocation for %zu samples (%.2f MiB) failed; using existing ring (%zu).\n",
                            min_capacity, (double)min_capacity * sizeof(int16_t) / (1024.0 * 1024.0),
                            input_ring.capacity);
            }
        }

        size_t target = desired_prebuf;
        /* Guard against unreasonable target */
        if (target < 16384) {
            target = 16384;
        }
        if (target > (input_ring.capacity / 2)) {
            target = input_ring.capacity / 2;
        }

        /* Announce computed prebuffer duration at current sample rate */
        double target_sec = (dongle.rate > 0) ? ((double)target / (2.0 * (double)dongle.rate)) : 0.0;
        LOG_INFO("rtltcp prebuffer target: %zu samples (%.3f s at %u Hz).\n", target, target_sec,
                 (unsigned)dongle.rate);

        /* Begin async capture first, then wait for ring to accumulate */
        LOG_INFO("Starting RTL async read (rtltcp prebuffer %d ms)...\n", pre_ms);
        rtl_device_start_async(rtl_device_handle, (uint32_t)ACTUAL_BUF_LENGTH);

        /* Wait up to ~2 seconds to reach target; exit early if flagged */
        {
            int waited_ms = 0;
            while (!exitflag && input_ring_used(&input_ring) < target && waited_ms < 2000) {
                usleep(2000); /* 2 ms */
                waited_ms += 2;
            }
            LOG_INFO("rtltcp prebuffer filled: %zu/%zu samples in ring.\n", input_ring_used(&input_ring), target);
        }

        /* Launch controller and demod threads after prebuffer */
        pthread_create(&controller.thread, NULL, controller_thread_fn, (void*)(&controller));
        pthread_create(&demod.thread, NULL, demod_thread_fn, (void*)(&demod));
        if (port != 0) {
            g_udp_ctrl = udp_control_start(
                port,
                [](uint32_t new_freq_hz, void* /*user_data*/) {
                    pthread_mutex_lock(&controller.hop_m);
                    controller.manual_retune_freq = new_freq_hz;
                    controller.manual_retune_pending.store(1);
                    pthread_cond_signal(&controller.hop);
                    pthread_mutex_unlock(&controller.hop_m);
                },
                NULL);
        }
    } else {
        /* Start controller/demod threads and async (USB path and defaults) */
        start_threads_and_async();
    }

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
            const char* approx2 = dsd_unicode_or_ascii("≈", "~");
            LOG_INFO("Derived SPS (@%u Hz): P25P1%s%d, P25P2%s%d, NXDN48%s%d.\n", out_hz, approx2, sps_p25p1, approx2,
                     sps_p25p2, approx2, sps_nxdn48);
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
    /* Wake any demod waits on both ready and space condition variables */
    safe_cond_signal(&demod.ready, &demod.ready_m);
    safe_cond_signal(&output.space, &output.ready_m);
    pthread_join(demod.thread, NULL);
    /* Wake any consumers blocked on output.ready to finish */
    safe_cond_signal(&output.ready, &output.ready_m);
    pthread_join(controller.thread, NULL);

    rtl_demod_cleanup(&demod);
    output_cleanup(&output);
    controller_cleanup(&controller);

    /* free input ring */
    if (input_ring.buffer) {
        dsd_neo_aligned_free(input_ring.buffer);
        input_ring.buffer = NULL;
    }

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
    /* Wake any demod waits on both ready and space condition variables */
    safe_cond_signal(&demod.ready, &demod.ready_m);
    safe_cond_signal(&output.space, &output.ready_m);
    pthread_join(demod.thread, NULL);
    /* Wake any consumers blocked on output.ready to finish */
    safe_cond_signal(&output.ready, &output.ready_m);
    pthread_join(controller.thread, NULL);

    rtl_demod_cleanup(&demod);
    output_cleanup(&output);
    controller_cleanup(&controller);

    if (input_ring.buffer) {
        dsd_neo_aligned_free(input_ring.buffer);
        input_ring.buffer = NULL;
    }
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
    /* If stream is being torn down, abort read promptly. */
    if (!output.buffer || (g_stream && g_stream->should_exit.load())) {
        return -1;
    }

    /* Optional: spectrum-based auto PPM correction (opt-in via DSD_NEO_AUTO_PPM).
       Uses quadratic interpolation of the spectrum peak (industry practice)
        and a simple SNR gate. When SNR >= 6 dB, nudge PPM by +/-1 toward center. */
    do {
        static int init = 0;
        static int enabled = 0;
        /* minimum SNR to trigger (kept for env/opt parsing below) */
        static double snr_thr_db = 6.0;
        static int cooldown = 0; /* simple rate limiter (loops) */
        if (!init) {
            init = 1;
            const char* on = getenv("DSD_NEO_AUTO_PPM");
            if (on && (*on == '1' || *on == 'y' || *on == 'Y' || *on == 't' || *on == 'T')) {
                enabled = 1;
            }
            const char* sthr = getenv("DSD_NEO_AUTO_PPM_SNR_DB");
            if (sthr) {
                double v = atof(sthr);
                if (v >= 0.0 && v <= 40.0) {
                    snr_thr_db = v;
                }
            }
            if (opts && opts->rtl_auto_ppm_snr_db > 0.0f && opts->rtl_auto_ppm_snr_db <= 60.0f) {
                snr_thr_db = (double)opts->rtl_auto_ppm_snr_db;
            }
        }
        /* Allow CLI/opts to enable even if env unset */
        /* Runtime user override takes precedence; otherwise follow opts */
        {
            int u = g_auto_ppm_user_en.load(std::memory_order_relaxed);
            if (u == 0) {
                enabled = 0;
            } else if (u == 1) {
                enabled = 1;
            } else {
                if (opts) {
                    enabled = (opts->rtl_auto_ppm != 0) ? 1 : 0;
                }
            }
        }
        g_auto_ppm_enabled.store(enabled, std::memory_order_relaxed);
        if (!g_auto_ppm_locked.load(std::memory_order_relaxed) && enabled) {
            /* Initialize training window once on first entry */
            static int train_init = 0;
            static std::chrono::steady_clock::time_point t0;
            if (!train_init) {
                t0 = std::chrono::steady_clock::now();
                train_init = 1;
            }
        }
        if (!enabled) {
            g_auto_ppm_training.store(0, std::memory_order_relaxed);
            break;
        }
        if (g_auto_ppm_locked.load(std::memory_order_relaxed)) {
            g_auto_ppm_training.store(0, std::memory_order_relaxed);
            /* Locked: do not adjust further */
            break;
        }
        if (cooldown > 0) {
            cooldown--;
            g_auto_ppm_cooldown.store(cooldown, std::memory_order_relaxed);
            break;
        }
        /* Snapshot current spectrum via helper */
        static float spec_copy[1024];
        int rate = 0;
        int N = dsd_rtl_stream_spectrum_get(spec_copy, (int)(sizeof(spec_copy) / sizeof(spec_copy[0])), &rate);
        if (N <= 0 || rate <= 0) {
            g_auto_ppm_last_dir.store(0, std::memory_order_relaxed);
            break;
        }
        /* Find strongest bin near DC primarily: widen search to +/- N/4 of center
           to tolerate larger initial frequency errors. */
        int k_center_i = N >> 1;
        int W = N >> 2; /* N/4 */
        if (W < 8) {
            W = 8;
        }
        int i_lo = k_center_i - W;
        int i_hi = k_center_i + W;
        if (i_lo < 2) {
            i_lo = 2;
        }
        if (i_hi > N - 3) {
            i_hi = N - 3;
        }
        int i_max = i_lo;
        float p_max = spec_copy[i_lo];
        for (int i = i_lo + 1; i <= i_hi; i++) {
            if (spec_copy[i] > p_max) {
                p_max = spec_copy[i];
                i_max = i;
            }
        }
        /* Estimate noise floor via median of bins excluding +-2 around peak */
        static float tmp[1024];
        int m = 0;
        for (int i = i_lo; i < i_hi; i++) {
            if (i >= i_max - 2 && i <= i_max + 2) {
                continue;
            }
            tmp[m++] = spec_copy[i];
        }
        if (m < 16) {
            g_auto_ppm_last_dir.store(0, std::memory_order_relaxed);
            break;
        }
        /* Require recent direct demod SNR for gating; also compute spectral SNR */
        int mid = m / 2; /* still compute median to keep spectrum path consistent */
        std::nth_element(tmp, tmp + mid, tmp + m);
        float noise_med_db = tmp[mid];
        float spec_snr_db = p_max - noise_med_db;
        auto nowtp = std::chrono::steady_clock::now();
        long long nowms = std::chrono::duration_cast<std::chrono::milliseconds>(nowtp.time_since_epoch()).count();
        const long long fresh_ms = 800; /* direct SNR must be updated within 0.8 s */
        long long c4_ms = g_snr_c4fm_last_ms.load(std::memory_order_relaxed);
        int c4_src = g_snr_c4fm_src.load(std::memory_order_relaxed);
        long long qp_ms = g_snr_qpsk_last_ms.load(std::memory_order_relaxed);
        int qp_src = g_snr_qpsk_src.load(std::memory_order_relaxed);
        long long gf_ms = g_snr_gfsk_last_ms.load(std::memory_order_relaxed);
        int gf_src = g_snr_gfsk_src.load(std::memory_order_relaxed);
        double d_c4 = g_snr_c4fm_db.load(std::memory_order_relaxed);
        double d_qp = g_snr_qpsk_db.load(std::memory_order_relaxed);
        double d_gf = g_snr_gfsk_db.load(std::memory_order_relaxed);
        bool c4_ok = (c4_src == 1) && (nowms - c4_ms <= fresh_ms);
        bool qp_ok = (qp_src == 1) && (nowms - qp_ms <= fresh_ms);
        bool gf_ok = (gf_src == 1) && (nowms - gf_ms <= fresh_ms);
        bool have_demod = c4_ok || qp_ok || gf_ok; /* currently unused for gating */
        (void)have_demod;
        double gate_snr_db = -100.0;
        if (c4_ok) {
            gate_snr_db = d_c4;
        }
        if (qp_ok && d_qp > gate_snr_db) {
            gate_snr_db = d_qp;
        }
        if (gf_ok && d_gf > gate_snr_db) {
            gate_snr_db = d_gf;
        }
        /* Store spectral SNR for UI */
        g_auto_ppm_snr_db.store(spec_snr_db, std::memory_order_relaxed);
        /* Debounce absolute power (peak dB) to avoid brief spikes */
        static double pwr_thr_db = -80.0;
        static int pwr_thr_inited = 0;
        if (!pwr_thr_inited) {
            if (const char* ep = getenv("DSD_NEO_AUTO_PPM_PWR_DB")) {
                char* endp = NULL;
                double v = strtod(ep, &endp);
                if (endp && ep != endp && v <= 0.0) {
                    pwr_thr_db = v;
                }
            }
            pwr_thr_inited = 1;
        }
        static int pwr_gate_active = 0;
        static auto pwr_gate_since = std::chrono::steady_clock::now();
        const int pwr_gate_debounce_ms = 2000; /* require 2s continuously above threshold */
        auto now_gate = std::chrono::steady_clock::now();
        bool pwr_over = (p_max >= (float)pwr_thr_db);
        if (pwr_over) {
            if (!pwr_gate_active) {
                pwr_gate_active = 1;
                pwr_gate_since = now_gate;
            }
        } else {
            pwr_gate_active = 0;
        }
        bool pwr_debounced =
            pwr_gate_active
            && (std::chrono::duration_cast<std::chrono::milliseconds>(now_gate - pwr_gate_since).count()
                >= pwr_gate_debounce_ms);
        /* Also require spectral SNR above threshold; reuse parsed env/opt value */
        double spec_thr_db = snr_thr_db;
        if (!pwr_debounced || spec_snr_db < spec_thr_db) {
            g_auto_ppm_training.store(0, std::memory_order_relaxed);
            g_auto_ppm_last_dir.store(0, std::memory_order_relaxed);
            break; /* below thresholds */
        }
        /* DC spur guard: if max is exactly the center bin and looks spur-like, ignore */
        if (i_max == k_center_i) {
            float l = spec_copy[i_max - 1];
            float r = spec_copy[i_max + 1];
            float side_max = (l > r) ? l : r;
            if ((p_max - side_max) > 12.0f) {
                /* sharp isolated spike at DC: likely residual DC spur */
                g_auto_ppm_last_dir.store(0, std::memory_order_relaxed);
                break;
            }
        }
        /* Parabolic interpolation around peak using log-power (dB) */
        int k = i_max;
        if (k <= 1 || k >= N - 2) {
            g_auto_ppm_last_dir.store(0, std::memory_order_relaxed);
            break;
        }
        double p1 = spec_copy[k - 1];
        double p2 = spec_copy[k + 0];
        double p3 = spec_copy[k + 1];
        double denom = (p1 - 2.0 * p2 + p3);
        double delta = 0.0; /* fractional bin offset in [-1,1] */
        if (fabs(denom) > 1e-6) {
            delta = 0.5 * (p1 - p3) / denom;
            if (delta > 1.0) {
                delta = 1.0;
            }
            if (delta < -1.0) {
                delta = -1.0;
            }
        }
        double k_hat = (double)k + delta;
        double k_center = 0.5 * (double)N; /* DC after shift */
        double k_err = k_hat - k_center;
        double bin_hz = (double)rate / (double)N;
        double df_hz = k_err * bin_hz; /* positive: signal to + side */
        g_auto_ppm_df_hz.store(df_hz, std::memory_order_relaxed);
        /* Convert to ppm relative to current tuned hardware center */
        double f0 = (double)dongle.freq;
        if (f0 <= 0.0) {
            g_auto_ppm_last_dir.store(0, std::memory_order_relaxed);
            break;
        }
        double est_ppm = (df_hz * 1e6) / f0;
        g_auto_ppm_est_ppm.store(est_ppm, std::memory_order_relaxed);
        /* Nudge by 1 ppm toward center with persistence, throttle, and direction self-calibration */
        static int dir_run = 0; /* -1,0,+1 (consensus of successive decisions) */
        static int run_len = 0; /* consecutive consistent decisions */
        static auto next_allowed = std::chrono::steady_clock::now();
        static int dir_calibrated = 0;          /* 0=unknown; +/-1 = mapping sign */
        static int awaiting_eval = 0;           /* 1 if last step pending evaluation */
        static int last_dir_applied = 0;        /* +/-1 for last applied change */
        static int last_ppm_after = 0;          /* ppm value after last applied change */
        static int last_step_size = 1;          /* ppm step size used for last change */
        static double prev_abs_df = 1e12;       /* |df| before last applied change */
        static double prev_demod_snr_db = -1e9; /* demod SNR at last step */
        /* Training + lock policy */
        static auto train_start = std::chrono::steady_clock::now();
        static int train_started = 0;
        static int train_steps = 0;
        const int train_max_ms = 15000;             /* train for up to 15 seconds */
        const int train_max_steps = 8;              /* or up to 8 steps */
        const int lock_hold_ms = 3000;              /* lock if stable for 3 seconds */
        static double lock_deadband_hz = 120.0;     /* and within 120 Hz window */
        static double zero_lock_deadband_hz = 60.0; /* tighter window for zero-step lock */
        /* Only allow zero-step lock when estimated offset is very small. This
           prevents premature lock at 0 when a meaningful offset remains. */
        static double zero_lock_max_est_ppm = 0.6; /* require |est_ppm| <= 0.6 */
        /* One-time env overrides for lock thresholds */
        static int zero_thr_inited = 0;
        if (!zero_thr_inited) {
            if (const char* ep = getenv("DSD_NEO_AUTO_PPM_ZEROLOCK_PPM")) {
                char* endp = NULL;
                double v = strtod(ep, &endp);
                if (endp && ep != endp && v >= 0.0 && v <= 2.0) {
                    zero_lock_max_est_ppm = v;
                }
            }
            if (const char* eh = getenv("DSD_NEO_AUTO_PPM_ZEROLOCK_HZ")) {
                char* endp = NULL;
                double v = strtod(eh, &endp);
                if (endp && eh != endp && v >= 10.0 && v <= 500.0) {
                    zero_lock_deadband_hz = v;
                }
            }
            zero_thr_inited = 1;
        }

        const int hold_needed = 4;             /* require 4 consistent decisions */
        const int throttle_ms = 1000;          /* at most 1 adjustment per 1s */
        const double deadband_ppm = 0.8;       /* ignore tiny offsets */
        const int ppm_limit = 200;             /* clamp absolute ppm range */
        const double eval_margin_hz = 120;     /* require ~>1/4-bin improvement to accept direction */
        const double eval_snr_margin_db = 0.5; /* prefer direction that improves demod SNR by >=0.5 dB */

        /* If evaluating last step, prefer demod SNR change; fallback to |df| change */
        if (awaiting_eval) {
            double cur_abs_df = fabs(df_hz);
            auto now = std::chrono::steady_clock::now();
            bool time_ok = (now >= next_allowed); /* wait full throttle interval for stable measurement */
            if (time_ok) {
                bool snr_improved = (gate_snr_db > (prev_demod_snr_db + eval_snr_margin_db));
                bool snr_worsened = (gate_snr_db + eval_snr_margin_db < prev_demod_snr_db);
                if (snr_improved) {
                    dir_calibrated = last_dir_applied;
                    awaiting_eval = 0;
                } else if (snr_worsened) {
                    int target_ppm = last_ppm_after - 2 * last_dir_applied * last_step_size;
                    if (target_ppm > ppm_limit) {
                        target_ppm = ppm_limit;
                    }
                    if (target_ppm < -ppm_limit) {
                        target_ppm = -ppm_limit;
                    }
                    if (target_ppm != opts->rtlsdr_ppm_error) {
                        opts->rtlsdr_ppm_error = target_ppm;
                        LOG_INFO("AUTO-PPM(spectrum): flip dir (SNR %.1f->%.1f dB). ppm->%d\n", prev_demod_snr_db,
                                 gate_snr_db, target_ppm);
                    }
                    dir_calibrated = -last_dir_applied;
                    awaiting_eval = 0;
                    next_allowed = now + std::chrono::milliseconds(throttle_ms);
                    g_auto_ppm_cooldown.store(throttle_ms / 10, std::memory_order_relaxed);
                } else if (cur_abs_df + eval_margin_hz < prev_abs_df) {
                    dir_calibrated = last_dir_applied;
                    awaiting_eval = 0;
                } else if (cur_abs_df > prev_abs_df + eval_margin_hz) {
                    int target_ppm = last_ppm_after - 2 * last_dir_applied * last_step_size;
                    if (target_ppm > ppm_limit) {
                        target_ppm = ppm_limit;
                    }
                    if (target_ppm < -ppm_limit) {
                        target_ppm = -ppm_limit;
                    }
                    if (target_ppm != opts->rtlsdr_ppm_error) {
                        opts->rtlsdr_ppm_error = target_ppm;
                        LOG_INFO("AUTO-PPM(spectrum): flip dir (|df| %.1f->%.1f Hz). ppm->%d\n", prev_abs_df,
                                 cur_abs_df, target_ppm);
                    }
                    dir_calibrated = -last_dir_applied;
                    awaiting_eval = 0;
                    next_allowed = now + std::chrono::milliseconds(throttle_ms);
                    g_auto_ppm_cooldown.store(throttle_ms / 10, std::memory_order_relaxed);
                } else {
                    /* Inconclusive: extend wait window a bit */
                    next_allowed = now + std::chrono::milliseconds(250);
                }
            }
        }

        int dir_base = 0;
        if (est_ppm > deadband_ppm) {
            dir_base = +1;
        } else if (est_ppm < -deadband_ppm) {
            dir_base = -1;
        }
        /* Apply mapping sign when calibrated */
        int dir = (dir_calibrated == 0) ? dir_base : (dir_base * dir_calibrated);
        /* Fast catch-up: choose step size based on estimated absolute ppm */
        int step_size = 1;
        double abs_est_ppm = fabs(est_ppm);
        if (abs_est_ppm >= 50.0) {
            step_size = 8;
        } else if (abs_est_ppm >= 25.0) {
            step_size = 4;
        } else if (abs_est_ppm >= 12.0) {
            step_size = 2;
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
        }

        g_auto_ppm_last_dir.store(dir, std::memory_order_relaxed);

        auto now = std::chrono::steady_clock::now();
        bool time_ok = (now >= next_allowed);
        int effective_hold = (step_size > 1) ? (hold_needed / 2) : hold_needed;
        if (effective_hold < 1) {
            effective_hold = 1;
        }
        if (!awaiting_eval && dir != 0 && run_len >= effective_hold && time_ok) {
            int new_ppm = opts->rtlsdr_ppm_error + dir * step_size; /* variable step size */
            if (new_ppm > ppm_limit) {
                new_ppm = ppm_limit;
            }
            if (new_ppm < -ppm_limit) {
                new_ppm = -ppm_limit;
            }
            if (new_ppm != opts->rtlsdr_ppm_error) {
                prev_abs_df = fabs(df_hz);
                prev_demod_snr_db = gate_snr_db;
                opts->rtlsdr_ppm_error = new_ppm;
                last_dir_applied = dir;
                last_ppm_after = new_ppm;
                last_step_size = step_size;
                awaiting_eval = 1; /* evaluate on next allowed window */
                next_allowed = now + std::chrono::milliseconds(throttle_ms);
                g_auto_ppm_cooldown.store(throttle_ms / 10, std::memory_order_relaxed);
                LOG_INFO("AUTO-PPM(spectrum): PWR=%.1f dB, SNR=%.1f dB, df=%.1f Hz, dir=%+d, step=%d, ppm->%d\n", p_max,
                         gate_snr_db, df_hz, dir, step_size, new_ppm);
                if (!train_started) {
                    train_started = 1;
                    train_start = now;
                }
                train_steps++;
            }
            run_len = 0; /* require persistence again */
        }

        /* Evaluate lock conditions: duration, step count, or stability window */
        auto now2 = std::chrono::steady_clock::now();
        if (!train_started) {
            /* Start the training window only after gate debounce is satisfied */
            if (pwr_debounced) {
                train_started = 1;
                train_start = now2;
            }
        } else {
            int elapsed_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now2 - train_start).count();
            bool time_over = (elapsed_ms >= train_max_ms);
            bool steps_over = (train_steps >= train_max_steps);
            static auto last_change = std::chrono::steady_clock::now();
            if (awaiting_eval == 0 && run_len == 0 && dir == 0) {
                /* if not changing, update stability timer */
                /* we keep last_change unless a step occurs (handled above) */
            } else if (awaiting_eval == 0 && last_dir_applied != 0) {
                last_change = now2;
            }
            int since_change_ms =
                (int)std::chrono::duration_cast<std::chrono::milliseconds>(now2 - last_change).count();
            bool stable_ok = (since_change_ms >= lock_hold_ms && fabs(df_hz) <= lock_deadband_hz);
            /* Allow lock without steps only when df is stably near zero and
               the estimated offset is tiny (avoid false zero locks). */
            bool significant_offset = (fabs(est_ppm) > deadband_ppm);
            bool stable_zero_ok = (train_steps == 0) && stable_ok && (fabs(df_hz) <= zero_lock_deadband_hz)
                                  && (!significant_offset) && (fabs(est_ppm) <= zero_lock_max_est_ppm);
            /* Lock if: lots of steps, or (>=1 step and time/stability satisfied), or (permissible zero-step lock) */
            bool can_lock = steps_over || ((train_steps >= 1) && (time_over || stable_ok)) || stable_zero_ok;
            if (can_lock) {
                g_auto_ppm_locked.store(1, std::memory_order_relaxed);
                g_auto_ppm_training.store(0, std::memory_order_relaxed);
                g_auto_ppm_lock_ppm.store(opts->rtlsdr_ppm_error, std::memory_order_relaxed);
                /* Store peak power at lock for UI */
                g_auto_ppm_lock_snr_db.store(p_max, std::memory_order_relaxed);
                g_auto_ppm_lock_df_hz.store(df_hz, std::memory_order_relaxed);
                LOG_INFO("AUTO-PPM(spectrum): training complete, locked (elapsed=%d ms, steps=%d, |df|=%.1f Hz)\n",
                         elapsed_ms, train_steps, fabs(df_hz));
            }
        }
        if (!g_auto_ppm_locked.load(std::memory_order_relaxed)) {
            g_auto_ppm_training.store(1, std::memory_order_relaxed);
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
    /* No internal scaling here; callers may apply their own multiplier. */
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

/* Helper for generic rings to observe RTL stream shutdown without using exitflag */
extern "C" int
dsd_rtl_stream_should_exit(void) {
    return (g_stream && g_stream->should_exit.load()) ? 1 : 0;
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
    if (sps > 64) {
        sps = 64;
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

/* -------- FM/C4FM amplitude stabilization + DC blocker (runtime) -------- */
extern "C" int
dsd_rtl_stream_get_fm_agc(void) {
    return demod.fm_agc_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_fm_agc(int onoff) {
    demod.fm_agc_enable = onoff ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_get_fm_agc_params(int* target_rms, int* min_rms, int* alpha_up_q15, int* alpha_down_q15) {
    if (target_rms) {
        *target_rms = demod.fm_agc_target_rms;
    }
    if (min_rms) {
        *min_rms = demod.fm_agc_min_rms;
    }
    if (alpha_up_q15) {
        *alpha_up_q15 = demod.fm_agc_alpha_up_q15;
    }
    if (alpha_down_q15) {
        *alpha_down_q15 = demod.fm_agc_alpha_down_q15;
    }
}

extern "C" void
dsd_rtl_stream_set_fm_agc_params(int target_rms, int min_rms, int alpha_up_q15, int alpha_down_q15) {
    if (target_rms >= 0) {
        if (target_rms < 1000) {
            target_rms = 1000;
        }
        if (target_rms > 20000) {
            target_rms = 20000;
        }
        demod.fm_agc_target_rms = target_rms;
    }
    if (min_rms >= 0) {
        if (min_rms < 0) {
            min_rms = 0;
        }
        if (min_rms > 15000) {
            min_rms = 15000;
        }
        demod.fm_agc_min_rms = min_rms;
    }
    if (alpha_up_q15 >= 0) {
        if (alpha_up_q15 < 1) {
            alpha_up_q15 = 1;
        }
        if (alpha_up_q15 > 32768) {
            alpha_up_q15 = 32768;
        }
        demod.fm_agc_alpha_up_q15 = alpha_up_q15;
    }
    if (alpha_down_q15 >= 0) {
        if (alpha_down_q15 < 1) {
            alpha_down_q15 = 1;
        }
        if (alpha_down_q15 > 32768) {
            alpha_down_q15 = 32768;
        }
        demod.fm_agc_alpha_down_q15 = alpha_down_q15;
    }
}

extern "C" int
dsd_rtl_stream_get_fm_limiter(void) {
    return demod.fm_limiter_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_fm_limiter(int onoff) {
    demod.fm_limiter_enable = onoff ? 1 : 0;
}

extern "C" int
dsd_rtl_stream_get_iq_dc(int* out_shift_k) {
    if (out_shift_k) {
        *out_shift_k = demod.iq_dc_shift;
    }
    return demod.iq_dc_block_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_iq_dc(int enable, int shift_k) {
    int was = demod.iq_dc_block_enable ? 1 : 0;
    if (enable >= 0) {
        demod.iq_dc_block_enable = enable ? 1 : 0;
    }
    if (shift_k >= 0) {
        if (shift_k < 6) {
            shift_k = 6;
        }
        if (shift_k > 15) {
            shift_k = 15;
        }
        demod.iq_dc_shift = shift_k;
    }
    /* If enabling now, precharge DC estimate to current block mean and retarget AGC
       so there is no apparent level drop. */
    if (!was && demod.iq_dc_block_enable && demod.lowpassed && demod.lp_len >= 2) {
        const int pairs = demod.lp_len >> 1;
        int64_t sumI = 0, sumQ = 0;
        for (int n = 0; n < pairs; n++) {
            sumI += (int)demod.lowpassed[(size_t)(n << 1) + 0];
            sumQ += (int)demod.lowpassed[(size_t)(n << 1) + 1];
        }
        int meanI = (pairs > 0) ? (int)(sumI / pairs) : 0;
        int meanQ = (pairs > 0) ? (int)(sumQ / pairs) : 0;
        demod.iq_dc_avg_r = meanI;
        demod.iq_dc_avg_i = meanQ;
        /* Estimate RMS after subtraction and retarget AGC gain */
        uint64_t acc = 0;
        for (int n = 0; n < pairs; n++) {
            int32_t I = (int)demod.lowpassed[(size_t)(n << 1) + 0] - meanI;
            int32_t Q = (int)demod.lowpassed[(size_t)(n << 1) + 1] - meanQ;
            acc += (uint64_t)((int64_t)I * I + (int64_t)Q * Q);
        }
        if (pairs > 0) {
            double mean_r2 = (double)acc / (double)pairs;
            double rms = sqrt(mean_r2);
            int target = (demod.fm_agc_target_rms > 0) ? demod.fm_agc_target_rms : 10000;
            if (target < 1000) {
                target = 1000;
            }
            if (target > 20000) {
                target = 20000;
            }
            double g_raw = (rms > 1e-6) ? ((double)target / rms) : 1.0;
            if (g_raw > 8.0) {
                g_raw = 8.0;
            }
            if (g_raw < 0.125) {
                g_raw = 0.125;
            }
            demod.fm_agc_gain_q15 = (int)(g_raw * 32768.0 + 0.5);
            if (demod.fm_agc_gain_q15 < 1024) {
                demod.fm_agc_gain_q15 = 1024;
            }
            if (demod.fm_agc_gain_q15 > 262144) {
                demod.fm_agc_gain_q15 = 262144;
            }
        }
    }
}

/* -------- FM CMA equalizer runtime control -------- */
extern "C" int
dsd_rtl_stream_get_fm_cma(void) {
    return demod.fm_cma_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_fm_cma(int onoff) {
    demod.fm_cma_enable = onoff ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_get_fm_cma_params(int* taps, int* mu_q15, int* warmup) {
    if (taps) {
        *taps = demod.fm_cma_taps;
    }
    if (mu_q15) {
        *mu_q15 = demod.fm_cma_mu_q15;
    }
    if (warmup) {
        *warmup = demod.fm_cma_warmup;
    }
}

extern "C" void
dsd_rtl_stream_set_fm_cma_params(int taps, int mu_q15, int warmup) {
    if (taps >= 0) {
        /* Allow 1, 3, 5, 7, or 9 taps:
           - 1: complex gain (CMA-only)
           - 3: fixed symmetric smoother
           - 5/7/9: adaptive symmetric linear-phase FIR (CMA-driven) */
        if (taps >= 9) {
            demod.fm_cma_taps = 9;
        } else if (taps >= 7) {
            demod.fm_cma_taps = 7;
        } else if (taps >= 5) {
            demod.fm_cma_taps = 5;
        } else if (taps >= 3) {
            demod.fm_cma_taps = 3;
        } else {
            demod.fm_cma_taps = 1;
        }
    }
    if (mu_q15 >= 0) {
        if (mu_q15 < 1) {
            mu_q15 = 1;
        }
        if (mu_q15 > 64) {
            mu_q15 = 64;
        }
        demod.fm_cma_mu_q15 = mu_q15;
    }
    if (warmup >= 0) {
        demod.fm_cma_warmup = warmup; /* 0 => continuous */
    }
}

extern "C" int
dsd_rtl_stream_get_fm_cma_strength(void) {
    int s = demod.fm_cma_strength;
    if (s < 0) {
        s = 0;
    }
    if (s > 2) {
        s = 2;
    }
    return s;
}

extern "C" void
dsd_rtl_stream_set_fm_cma_strength(int strength) {
    if (strength < 0) {
        strength = 0;
    }
    if (strength > 2) {
        strength = 2;
    }
    demod.fm_cma_strength = strength;
}

/* FM CMA adaptive guard status */
extern "C" void
dsd_rtl_stream_get_fm_cma_guard(int* freeze_blocks, int* accepts, int* rejects) {
    if (freeze_blocks) {
        *freeze_blocks = demod.fm_cma_guard_freeze;
    }
    if (accepts) {
        *accepts = demod.fm_cma_guard_accepts;
    }
    if (rejects) {
        *rejects = demod.fm_cma_guard_rejects;
    }
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

/* Forward decl for CQPSK RRC reconfig */
extern "C" void dsd_rtl_stream_cqpsk_set_rrc(int enable, int alpha_percent, int span_syms);

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

/* ---------------- CQPSK ISI-based auto-EQ (simulcast helper) ---------------- */
static inline double
isi_index_from_eq() {
    /* Compute ISI severity from CQPSK equalizer taps: ratio of off-center energy
       (FFE side-taps + DFE taps) to total energy (center + off-center).
       Returns [0,1]. Low values (<=~0.01) indicate little ISI; higher implies stronger ISI. */
    const cqpsk_eq_state_t* eq = &demod.cqpsk_eq;
    int T = eq->num_taps;
    if (T <= 0) {
        return 0.0;
    }
    /* Center energy from c[0] */
    const int c0i = eq->c_i[0];
    const int c0q = eq->c_q[0];
    double e_center = (double)c0i * (double)c0i + (double)c0q * (double)c0q;
    /* FFE side-tap energy */
    double e_side = 0.0;
    for (int k = 1; k < T; k++) {
        const int ci = eq->c_i[k];
        const int cq = eq->c_q[k];
        e_side += (double)ci * (double)ci + (double)cq * (double)cq;
    }
    /* DFE branch energy (post-cursor mitigation) */
    if (eq->dfe_taps > 0) {
        const int Nt = (eq->dfe_taps > 4) ? 4 : eq->dfe_taps;
        for (int k = 0; k < Nt; k++) {
            const int bi = eq->b_i[k];
            const int bq = eq->b_q[k];
            e_side += (double)bi * (double)bi + (double)bq * (double)bq;
        }
    }
    double e_tot = e_center + e_side;
    if (e_tot <= 1e-12) {
        return 0.0;
    }
    return e_side / e_tot;
}

/* Forward declaration for CQPSK runtime setter used by runtime helpers */
extern "C" void rtl_stream_cqpsk_set(int lms_enable, int taps, int mu_q15, int update_stride, int wl_enable,
                                     int dfe_enable, int dfe_taps, int mf_enable, int cma_warmup_samples);

/**
 * @brief P25 Phase 2 error callbacks for runtime helpers.
 * Aggregates recent RS/voice error deltas.
 */
extern "C" void
dsd_rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                int sacch_err_delta, int voice_err_delta) {
    (void)slot;            /* reserved for future per-slot tuning */
    (void)facch_ok_delta;  /* currently unused */
    (void)facch_err_delta; /* currently unused */
    (void)sacch_ok_delta;  /* currently unused */
    (void)sacch_err_delta; /* currently unused */
    (void)voice_err_delta; /* currently unused */

    /* Auto-set TED SPS for P25 Phase 2 (≈6000 sym/s) based on complex baseband rate. */
    if (demod.ted_enabled) {
        int Fs_cx = 0;
        /* TED operates on demod.rate_out; prefer that even when an audio
           resampler is enabled. */
        if (demod.rate_out > 0) {
            Fs_cx = demod.rate_out;
        } else {
            Fs_cx = (int)output.rate;
        }
        if (Fs_cx <= 0) {
            Fs_cx = 48000;
        }
        int sps = (Fs_cx + 3000) / 6000; /* round(Fs/6000) */
        if (sps < 2) {
            sps = 2;
        }
        if (sps > 32) {
            sps = 32;
        }
        if (demod.ted_sps != sps) {
            dsd_rtl_stream_set_ted_sps(sps);
        }
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
    /* Auto-set TED SPS for P25 Phase 1 (≈4800 sym/s) based on complex baseband rate. */
    if (demod.ted_enabled) {
        int Fs_cx = 0;
        /* TED operates on demod.rate_out; prefer that even when an audio
           resampler is enabled. */
        if (demod.rate_out > 0) {
            Fs_cx = demod.rate_out;
        } else {
            Fs_cx = (int)output.rate;
        }
        if (Fs_cx <= 0) {
            Fs_cx = 48000;
        }
        int sps = (Fs_cx + 2400) / 4800; /* round(Fs/4800) */
        if (sps < 2) {
            sps = 2;
        }
        if (sps > 32) {
            sps = 32;
        }
        if (demod.ted_sps != sps) {
            dsd_rtl_stream_set_ted_sps(sps);
        }
    }
    (void)fec_ok_delta;
    (void)fec_err_delta;
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

/* CQPSK EQ debug snapshot (C shim) */
extern "C" int cqpsk_runtime_get_debug(int* updates, int* adapt_mode, int* c0_i, int* c0_q, int* taps,
                                       int* isi_ratio_q15, int* wl_improp_q15, int* cma_warmup, int* mu_q15,
                                       int* sym_stride, int* dfe_taps, int* err_ema_q14);

extern "C" int
dsd_rtl_stream_cqpsk_get_debug(int* updates, int* adapt_mode, int* c0_i, int* c0_q, int* taps, int* isi_ratio_q15,
                               int* wl_improp_q15, int* cma_warmup, int* mu_q15, int* sym_stride, int* dfe_taps,
                               int* err_ema_q14) {
    return cqpsk_runtime_get_debug(updates, adapt_mode, c0_i, c0_q, taps, isi_ratio_q15, wl_improp_q15, cma_warmup,
                                   mu_q15, sym_stride, dfe_taps, err_ema_q14);
}

/* Toggle generic IQ balance prefilter */
extern "C" void
dsd_rtl_stream_toggle_iq_balance(int onoff) {
    demod.iqbal_enable = onoff ? 1 : 0;
}

extern "C" int
dsd_rtl_stream_get_iq_balance(void) {
    return demod.iqbal_enable ? 1 : 0;
}

/* Coarse DSP feature toggles and snapshot */
extern "C" void
rtl_stream_toggle_cqpsk(int onoff) {
    demod.cqpsk_enable = onoff ? 1 : 0;
    /* Reset EQ state when toggling path to avoid stale filters */
    cqpsk_reset_all();
    /* Switch demod output selector and reset CQPSK differential history. */
    if (demod.cqpsk_enable) {
        extern void qpsk_differential_demod(struct demod_state*);
        demod.mode_demod = &qpsk_differential_demod;
        demod.cqpsk_diff_prev_r = 0;
        demod.cqpsk_diff_prev_j = 0;
    } else {
        extern void dsd_fm_demod(struct demod_state*);
        demod.mode_demod = &dsd_fm_demod;
    }
}

/* CQPSK acquisition-only FLL toggle/get */
extern "C" int
dsd_rtl_stream_get_cqpsk_acq_fll(void) {
    return demod.cqpsk_acq_fll_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_cqpsk_acq_fll(int onoff) {
    demod.cqpsk_acq_fll_enable = onoff ? 1 : 0;
    /* Reset latch so it can re-acquire if re-enabled */
    demod.cqpsk_acq_fll_locked = 0;
    demod.cqpsk_acq_quiet_runs = 0;
}

extern "C" int
dsd_rtl_stream_get_cqpsk_acq_fll_locked(void) {
    return demod.cqpsk_acq_fll_locked ? 1 : 0;
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
rtl_stream_dsp_get(int* cqpsk_enable, int* fll_enable, int* ted_enable) {
    if (cqpsk_enable) {
        *cqpsk_enable = demod.cqpsk_enable ? 1 : 0;
    }
    if (fll_enable) {
        *fll_enable = demod.fll_enabled ? 1 : 0;
    }
    if (ted_enable) {
        *ted_enable = demod.ted_enabled ? 1 : 0;
    }
    return 0;
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
    /* Freeze retunes during auto-PPM training (to allow lock) unless disabled via env */
    static int freeze_checked = 0;
    static int freeze_on_train = 1; /* default on */
    if (!freeze_checked) {
        freeze_checked = 1;
        const char* e = getenv("DSD_NEO_AUTO_PPM_FREEZE");
        if (e && (e[0] == '0' || e[0] == 'n' || e[0] == 'N' || e[0] == 'f' || e[0] == 'F')) {
            freeze_on_train = 0;
        }
    }
    extern int dsd_rtl_stream_auto_ppm_training_active(void);
    if (freeze_on_train && dsd_rtl_stream_auto_ppm_training_active()) {
        LOG_NOTICE("Retune deferred: auto-PPM training active.\n");
        return 0; /* no retune while training */
    }
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
    /* Use a slightly longer window for a more stable estimate. */
    if (n > 512) {
        n = 512;
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

extern "C" int
dsd_rtl_stream_set_rtltcp_autotune(int onoff) {
    if (!rtl_device_handle) {
        return -1;
    }
    return rtl_device_set_tcp_autotune(rtl_device_handle, onoff ? 1 : 0);
}

extern "C" int
dsd_rtl_stream_get_rtltcp_autotune(void) {
    if (!rtl_device_handle) {
        return 0;
    }
    return rtl_device_get_tcp_autotune(rtl_device_handle);
}
