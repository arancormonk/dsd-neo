// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Demodulator state shared across DSP modules and RTL-SDR front-end.
 *
 * This is the canonical `struct demod_state` definition consumed by the DSP
 * pipeline and radio front-end.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_DEMOD_STATE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_DEMOD_STATE_H_

#include <dsd-neo/platform/platform.h>
#include <stdint.h>

#ifdef __cplusplus
#include <atomic>
#include <dsd-neo/core/safe_api.h>
#endif
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/fsk_modem.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/platform/threading.h>

/* Buffer sizing constants shared by the demodulator and radio front-end. */
#define DEFAULT_BUF_LENGTH       16384
#define MAXIMUM_OVERSAMPLE       16
#define MAXIMUM_BUF_LENGTH       (MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)

/* Maximum half-band tap count used to dimension complex-decimator histories. */
#define HB_TAPS_MAX              31

/* Channel LPF plan/history capacity. Digital profiles design at most 144 taps
 * (and keep their 63-tap fallback); the analog family uses the full capacity so
 * device-forced rates up to ~102 kHz get a real design. Static-asserted against
 * DSD_ANALOG_CHANNEL_MAX_TAPS in demod_pipeline.cpp. */
#define DSD_CHANNEL_LPF_MAX_TAPS 288

/* Channel LPF history, in complex samples: the streaming FIR (simd_fir_complex_apply()) keeps the newest this many
 * input samples. Its windows reach pending + c samples back, with pending at most the largest c (143, for the
 * largest odd tap count) and c at most that too, so a tap change without a reset always finds its history. */
#define DSD_CHANNEL_LPF_HIST_LEN (DSD_CHANNEL_LPF_MAX_TAPS - 1)

/* The channel LPF's output work buffers (hb_workbuf, timing_buf), in floats: a call makes at most N + 143 complex
 * outputs for N in (N + c_old - c_new on a tap shrink), and N is at most MAXIMUM_BUF_LENGTH / 2 complex. */
#define DSD_DEMOD_WORKBUF_LENGTH (MAXIMUM_BUF_LENGTH + 2 * DSD_CHANNEL_LPF_HIST_LEN)

/* The alignment of demod_state's aligned work buffers, in bytes: DSD_NEO_ALIGN (runtime/mem.h), which the SIMD kernels
 * assume of them. */
#define DSD_DEMOD_BUF_ALIGN      64

/* Floats in one DSD_DEMOD_BUF_ALIGN-byte block, as an int so the capacities below round in the int arithmetic they are
 * written in. */
#define DSD_DEMOD_BLOCK_FLOATS   (DSD_DEMOD_BUF_ALIGN / (int)sizeof(float))

/* The floats an aligned buffer of n floats is declared with: n rounded up to whole DSD_DEMOD_BUF_ALIGN-byte blocks, so
 * the buffer ends where the next one's alignment starts and the struct needs no padding between them. The capacity is
 * still n; the spare tail past it is never read. */
#define DSD_DEMOD_ALIGNED_FLOATS(n)                                                                                    \
    ((((n) + DSD_DEMOD_BLOCK_FLOATS - 1) / DSD_DEMOD_BLOCK_FLOATS) * DSD_DEMOD_BLOCK_FLOATS)

/* Channel LPF profile ids */
enum DSD_ATTR_PACKED {
    DSD_CH_LPF_PROFILE_WIDE = 0,
    DSD_CH_LPF_PROFILE_6K25 = 1,      /* 6.25 kHz modes: protects the 3125 Hz channel edge */
    DSD_CH_LPF_PROFILE_12K5 = 2,      /* 12.5 kHz 4FSK modes: protects the 6250 Hz channel edge */
    DSD_CH_LPF_PROFILE_PROVOICE = 3,  /* ProVoice: protects the 6250 Hz channel edge */
    DSD_CH_LPF_PROFILE_P25_C4FM = 4,  /* P25 C4FM: protects the 6250 Hz channel edge */
    DSD_CH_LPF_PROFILE_P25_CQPSK = 5, /* P25 CQPSK/LSM: 12.5 kHz edge plus guard */
};

enum DSD_ATTR_PACKED dsd_demod_output_kind {
    DSD_DEMOD_OUTPUT_AUDIO_MONITOR = 0,
    DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR = 1,
    DSD_DEMOD_OUTPUT_SYMBOL_CQPSK = 2,
};

enum DSD_ATTR_PACKED dsd_digital_resample_mode {
    DSD_DIGITAL_RESAMPLE_AUTO = 0,
    DSD_DIGITAL_RESAMPLE_ON = 1,
    DSD_DIGITAL_RESAMPLE_OFF = 2,
};

/**
 * @brief Aggregate state container for the demodulator processing chain.
 *
 * Holds working buffers, configuration, and module states used by the DSP
 * pipeline (filters, resamplers, CQPSK recovery, etc.) and by the RTL-SDR front-end
 * thread.
 *
 * Radio and DSP implementation units include this definition directly.
 */
// NOLINTBEGIN(clang-analyzer-optin.performance.Padding)
/* What a squelch plan is designed for (issue #518 follow-up): the channel rate, the channel filter's plan, the
   half-band stage ahead of it (0 none, else its tap count) and how many stages the cascade runs. set is 0 until a plan
   was designed. */
typedef struct {
    int rate_hz;
    int rate_out;
    int profile;
    int width_hz;
    int taps_len;
    int hb_taps;
    int passes; /* half-band stages ahead of the channel filter */
    int set;
} dsd_demod_squelch_plan_key;

struct demod_state {
#ifdef __cplusplus
    demod_state() noexcept { DSD_MEMSET(this, 0, sizeof(*this)); }
#endif

    /* Large aligned buffers first to minimize padding. Each is declared as DSD_DEMOD_ALIGNED_FLOATS of its capacity, so
       none needs padding before the next (static-asserted below the struct). */
    alignas(DSD_DEMOD_BUF_ALIGN) float hb_i_buf[DSD_DEMOD_ALIGNED_FLOATS(MAXIMUM_BUF_LENGTH / 2)];
    alignas(DSD_DEMOD_BUF_ALIGN) float hb_q_buf[DSD_DEMOD_ALIGNED_FLOATS(MAXIMUM_BUF_LENGTH / 2)];
    alignas(DSD_DEMOD_BUF_ALIGN) float hb_i_out[DSD_DEMOD_ALIGNED_FLOATS(MAXIMUM_BUF_LENGTH / 2)];
    alignas(DSD_DEMOD_BUF_ALIGN) float hb_q_out[DSD_DEMOD_ALIGNED_FLOATS(MAXIMUM_BUF_LENGTH / 2)];
    alignas(DSD_DEMOD_BUF_ALIGN) float input_cb_buf[DSD_DEMOD_ALIGNED_FLOATS(MAXIMUM_BUF_LENGTH)];
    alignas(DSD_DEMOD_BUF_ALIGN) float result[DSD_DEMOD_ALIGNED_FLOATS(MAXIMUM_BUF_LENGTH)];
    alignas(DSD_DEMOD_BUF_ALIGN) float timing_buf[DSD_DEMOD_ALIGNED_FLOATS(DSD_DEMOD_WORKBUF_LENGTH)];
    alignas(DSD_DEMOD_BUF_ALIGN) float resamp_outbuf[DSD_DEMOD_ALIGNED_FLOATS(MAXIMUM_BUF_LENGTH * 4)];
    /* Streaming FIR history, DSD_CHANNEL_LPF_HIST_LEN complex samples, newest right-aligned */
    alignas(DSD_DEMOD_BUF_ALIGN) float channel_lpf_hist_i[DSD_DEMOD_ALIGNED_FLOATS(DSD_CHANNEL_LPF_HIST_LEN)];
    alignas(DSD_DEMOD_BUF_ALIGN) float channel_lpf_hist_q[DSD_DEMOD_ALIGNED_FLOATS(DSD_CHANNEL_LPF_HIST_LEN)];
    alignas(DSD_DEMOD_BUF_ALIGN) float channel_lpf_plan_taps[DSD_DEMOD_ALIGNED_FLOATS(DSD_CHANNEL_LPF_MAX_TAPS)];
    /* The auto squelch's per-sample gate flags (DSD_SQUELCH_FLAG_CLOSED), parallel to result and to resamp_outbuf.
       Valid only while result_flags_active; otherwise every sample is open. */
    alignas(DSD_DEMOD_BUF_ALIGN) uint8_t result_flags[MAXIMUM_BUF_LENGTH];
    alignas(DSD_DEMOD_BUF_ALIGN) uint8_t resamp_outflags[MAXIMUM_BUF_LENGTH * 4];

    /* Pointers and 64-bit items next */
    dsd_thread_t thread;
    float* lowpassed;
    double squelch_running_power;
    float* resamp_taps; /* normalized taps as L contiguous phase blocks, length = K*L */
    float* resamp_hist; /* mirrored history window, length = 2*K */
    void (*mode_demod)(struct demod_state*);
    float* post_polydecim_taps; /* normalized taps length K */
    float* post_polydecim_hist; /* circular history length K */
    dsd_thread_t mt_threads[2];

    struct {
        void (*run)(void*);
        void* arg;
    } mt_tasks[2];

    struct {
        struct demod_state* s;
        int id;
    } mt_args[2];

    dsd_mutex_t mt_lock;
    dsd_mutex_t ready_m;
    dsd_cond_t mt_cv;
    dsd_cond_t mt_done_cv;
    dsd_cond_t ready;

    /* Scalars and small arrays */
    int exit_flag;
    int lp_len;
    int result_len;
    int rate_in;
    int rate_out;
    int rate_out2;
    float pre_r, pre_j;
    /* 1 once pre_r/pre_j hold a valid sample from a prior block; 0 before the
       first sample has been observed, including when that sample is exactly zero. */
    int fm_demod_history_valid;
    int post_downsample;
    float output_scale;
    float squelch_level;
    int conseq_squelch, squelch_hits, terminate_on_squelch;
    int squelch_decim_stride;
    int squelch_decim_phase;
    int squelch_window;
    /* Squelch soft gate (audio envelope) */
    int squelch_gate_open;     /* 1=open, 0=closed (latched per block) */
    float squelch_env;         /* envelope gain [0,1] */
    float squelch_env_attack;  /* attack alpha [0,1] for opening */
    float squelch_env_release; /* release alpha [0,1] for closing */
    int downsample_passes;
    int custom_atan;
    int deemph;
    float deemph_a; /* deemphasis alpha [0.0, 1.0] for one-pole IIR */
    float deemph_avg;
    /* De-emphasis time constant in microseconds (0 = none); deemph_a is recomputed from it whenever rate_out
       changes. */
    int deemph_tau_us;
    /* Optional post-demod audio low-pass filter (one-pole) */
    int audio_lpf_enable;
    /* Audio LPF cutoff in Hz (0 = none); audio_lpf_alpha is recomputed from it whenever rate_out changes. */
    int audio_lpf_cutoff_hz;
    float audio_lpf_alpha; /* alpha [0.0, 1.0] for one-pole LPF */
    float audio_lpf_state; /* state/output y[n-1] */
    float now_lpr;
    int prev_lpr_index;
    int dc_block;
    float dc_avg;
    /* Half-band decimator */
    float hb_workbuf[DSD_DEMOD_WORKBUF_LENGTH];
    float hb_hist_i[10][HB_TAPS_MAX - 1];
    float hb_hist_q[10][HB_TAPS_MAX - 1];
    /* Per stage: input samples taken but not yet decimated past (simd_hb_decim2_complex()), the look-ahead and the
       decimation phase in one count. 0 after a reset (dsd_demod_reset_filter_state()), so the stage's next output is
       centred on its next input. */
    int hb_pending[10];
    /* The downsample_passes the half-band state above belongs to: the cascade starts over when the count changes,
       since writers of the count (rate setup, restore_capture_rate_settings()) do not reset it. */
    int hb_state_passes;

    /* Fixed channel low-pass (post-HB) to bound noise bandwidth at higher Fs.
     * At 48 kHz with 1200 Hz transition, Blackman needs 135 taps.
     * Digital profiles cap the design at 144 taps; the analog family may use
     * the full DSD_CHANNEL_LPF_MAX_TAPS. The history is DSD_CHANNEL_LPF_HIST_LEN
     * samples whatever the taps. */
    int channel_lpf_enable; /* gate */
    /* The historical enable rule (DSD_NEO_CHANNEL_LPF, else a 20 kHz rate_in) as decided when the stream was
       configured. The unset analog default and a switch back to digital restore it; an explicit analog width
       overrides it. */
    int channel_lpf_default_enable;
    /* Channel FIR outputs held back for look-ahead (simd_fir_complex_apply()). 0 after a reset
       (dsd_demod_reset_filter_state()), so the first output is centred on the first new sample; above 0 exactly when
       the filter took samples since. */
    int channel_lpf_pending;
    int channel_lpf_profile;       /* see DSD_CH_LPF_PROFILE_* */
    int channel_lpf_plan_rate_out; /* cached rate for channel_lpf_plan_taps */
    int channel_lpf_plan_profile;  /* cached profile for channel_lpf_plan_taps */
    int channel_lpf_plan_width_hz; /* cached analog width for channel_lpf_plan_taps (0 = profile design) */
    int channel_lpf_plan_taps_len; /* cached tap count; 0 = not designed, or the analog width is unrealizable */
    /* Analog receive family (the -fA monitor; not the M17 encoder, which shares the analog front end). While set,
       channel_lpf_width_hz > 0 drives the channel filter instead of channel_lpf_profile. */
    int analog_family;
    int analog_demod; /* dsd_analog_demod (runtime/analog_channel.h); 0 = FM */
    /* Full RF channel width in Hz the analog filter protects (cutoff W/2 + 600 Hz, 1200 Hz transition).
       0 keeps the profile design, including the legacy WIDE design for an unset default the rate cannot fit. */
    int channel_lpf_width_hz;
    /* The analog width the stream was asked for (0 = the unset NFM default; the AM default counts as requested),
       kept so a later rate change can resolve the channel again: the unset default between 16 kHz and the legacy
       WIDE design, and a requested width against the new rate. */
    int analog_width_request_hz;
    /* The configured width that request came from (0 = the kind's default, AM's included), as the decoder spells the
       setting. The AM default and an explicit 6000 Hz make the same request; a stream that refuses a later request
       reports this as the width it kept (rtl_stream_receive_request_refusal()), so the decoder puts back the setting
       it had, not the default for an explicit 6000. */
    int analog_width_setting_hz;
    /* AM envelope detector (dsd_am_demod(), issue #524): the carrier estimate the envelope is divided by, a one-pole
       average of |z| with a DSD_AM_CARRIER_TAU_MS time constant. A squelched block leaves it where it was. Every reset
       of the monitor audio state (stream open, retune, receive-family or FM/AM switch) sets it to 0, and the next
       unsquelched block warm-starts it from that block's mean magnitude. */
    float am_carrier;
    /* Samples the channel squelch has kept closed since the AM detector last ran on audio (saturating); past
       DSD_AM_CARRIER_HOLD_MS the next unsquelched block warm-starts am_carrier. Cleared with it. */
    int am_squelched_samples;
    float channel_pwr; /* mean power (RMS^2 proxy) measured after channel LPF */
    /* Squelch threshold (linear power); 0 = disabled. Written from the control thread
     * (config apply, menus) while the demod thread reads it per block. */
    std::atomic<float> channel_squelch_level;
    int channel_squelched; /* 1 if squelched this block, 0 otherwise */
    /* 1 when full_demod()'s front end made no samples this block (a filter warm-up, or nothing in): the block decided
       nothing, so channel_squelched and the other per-block decisions still hold the last block's. */
    int front_end_empty;

    /* The dynamic squelches (issue #518 follow-up). The IO layer copies the setting (squelch_mode, squelch_margin_db)
       and the context (what sets the channel's noise) in before each full_demod(), on the demod thread; the DSP reads
       and writes everything here as plain fields. While one runs (an AUTO or NOISE setting on the analog monitor) the
       block is never zeroed: the auto squelch's tracker flags each channel sample, or the noise squelch each
       discriminator output, the flags travel with the samples through the post-decimator and the resampler
       (result_flags, resamp_outflags) to the output ring, and the decoder mutes at its sink. */
    int squelch_mode;      /* dsd_squelch_mode */
    int squelch_margin_db; /* AUTO: the margin over the floor; NOISE: the quieting it opens at */
    dsd_squelch_floor_key squelch_context;
    /* 1 when this block's result_flags hold the tracker's flags. */
    int result_flags_active;
    /* 1 when the previous block ran the tracker, and the context it ran in. */
    int squelch_auto_ran;
    int squelch_context_applied_set;
    dsd_squelch_floor_key squelch_context_applied;
    /* The floor cache's clock when the tracker last ran: its floor ages from there while it does not. */
    double squelch_floor_active_s;
    /* What the tracker's plan was designed for. */
    dsd_demod_squelch_plan_key squelch_plan;
    dsd_squelch_floor squelch_floor;
    dsd_squelch_floor_cache squelch_cache;
    /* The NFM noise squelch: a NOISE setting on the FM monitor whose channel plan has a band above voice runs it on
       the discriminator output (elsewhere the tracker runs the setting as AUTO). squelch_noise_armed marks a block it
       runs on: decided before the discriminator, run after it. squelch_noise_ran is 1 when the previous block ran it,
       in noise_context (its own, so the tracker's floor is never stored under a context it did not run in). Its plan
       follows the same channel plan as the tracker's; designing one calibrates on 2 s of noise. */
    int squelch_noise_armed;
    int squelch_noise_ran;
    int noise_context_set;
    dsd_squelch_floor_key noise_context;
    dsd_demod_squelch_plan_key noise_plan;
    dsd_noise_squelch noise_squelch;
    /* The flags of the post-decimator's and the resampler's last inputs: each output takes the flag of the input at or
       just before its filter's centre, half the filter's length back. */
    uint8_t post_flag_hist[32];
    int post_flag_head;
    uint8_t post_fallback_mid_flag; /* the fallback's part-filled group: the flag of its middle sample */
    uint8_t resamp_flag_hist[32];
    int resamp_flag_head;

    /* Polyphase rational resampler (L/M) */
    int resamp_enabled;
    /* Whether the digital FSK discriminator stream may pass through the resampler:
       0 = auto (only when a device-imposed rate gives a non-integer SPS), 1 = always, 2 = never. */
    int digital_resample_mode;
    /* Set when the capture rate came from the device's fixed rate grid rather than from the
       requested DSP bandwidth. Auto-mode digital resampling only triggers in that case, so a
       user-chosen bandwidth keeps its existing rate chain. */
    int capture_rate_device_forced;
    int resamp_target_hz;      /* desired output sample rate */
    int resamp_L;              /* upsample factor */
    int resamp_M;              /* downsample factor */
    int resamp_phase;          /* 0..L-1 accumulator */
    int resamp_taps_len;       /* prototype taps length (padded to K*L) */
    int resamp_taps_per_phase; /* K = ceil(taps_len/L) */
    int resamp_hist_head;      /* next write index into base history window [0..K-1] */

    /* OP25-compatible CQPSK carrier recovery.
     * Signal flow: FLL band-edge (coarse freq) -> Gardner TED -> diff_phasor -> Costas (fine freq)
     * Total CFO for metrics = fll_band_edge_state.freq + costas_state.freq/sps */
    dsd_costas_loop_state_t costas_state;          /* Symbol-rate Costas loop */
    dsd_fll_band_edge_state_t fll_band_edge_state; /* Sample-rate FLL band-edge */
    dsd_fsk_modem_state fsk_modem_state;           /* FSK discriminator state */

    /* Timing error detector (Gardner) - native float */
    int ted_enabled;
    float ted_gain;           /* loop gain, typically 0.01..0.1 */
    int ted_gain_is_set;      /* env/API/UI override; disables automatic mode-specific gain changes */
    float ted_effective_gain; /* loop gain actually used by mode-specific TED */
    int ted_sps;              /* nominal samples per symbol */
    int ted_sps_override;     /* >0 = manual override (used during P25P2 VC tunes) */
    int costas_reset_pending; /* 1 = reset Costas loop on next retune (set when SPS override changes) */
    float ted_mu;             /* fractional phase [0.0, 1.0) */

    /* Non-integer SPS detection: set when Fs/sym_rate doesn't divide evenly.
       Blocks like TED/FLL band-edge require integer SPS and auto-disable. */
    int sps_is_integer; /* 1 = integer SPS, 0 = non-integer (blocks disabled) */

    /* TED module state */
    ted_state_t ted_state;

    /* Minimal 2-thread worker pool bookkeeping */
    int mt_enabled;
    int mt_ready;
    int mt_should_exit;
    int mt_epoch;
    int mt_completed_in_epoch;
    int mt_posted_count;
    int mt_worker_id[2];

    /* CQPSK (H-DQPSK) path enable for P25 LSM/TDMA */
    int cqpsk_enable;
    int output_kind;    /* dsd_demod_output_kind */
    int symbol_rate_hz; /* FSK/CQPSK protocol symbol rate */
    int symbol_levels;  /* 2 or 4 for FSK; 4 for SYMBOL_CQPSK */

    /* CQPSK pre-Costas differential phasor history (previous raw sample) */
    float cqpsk_diff_prev_r;
    float cqpsk_diff_prev_j;

    /* OP25-style RMS AGC state for CQPSK path.
     * Algorithm from op25/gr-op25_repeater/apps/rms_agc.py:
     *   rms = sqrt(alpha * mag_sqrd + (1-alpha) * rms_prev^2)
     *   out = in * (reference / rms)
     * OP25 uses: rms_agc.rms_agc(alpha=0.45, reference=0.85) */
    float cqpsk_agc_avg; /* running average of mag^2 (d_avg in op25) */

    /* Generic mode-aware IQ balance (image suppression) */
    int iqbal_enable;        /* 0/1 gate */
    float iqbal_thr;         /* |alpha| threshold for enable (normalized) */
    float iqbal_alpha_ema_r; /* EMA of alpha real (normalized) */
    float iqbal_alpha_ema_i; /* EMA of alpha imag (normalized) */
    float iqbal_alpha_ema_a; /* EMA smoothing alpha [0.0, 1.0] */

    /* Complex DC blocker before discriminator */
    int iq_dc_block_enable; /* 0/1 gate */
    int iq_dc_shift;        /* shift k for dc += (x-dc)>>k; typical 10..14 */
    float iq_dc_avg_r;      /* running DC estimate for I */
    float iq_dc_avg_i;      /* running DC estimate for Q */

    /* Post-demod audio polyphase decimator (M>2) */
    int post_polydecim_enabled;   /* 0/1 gate for audio polyphase decimator */
    int post_polydecim_M;         /* integer decimation factor */
    int post_polydecim_K;         /* taps per phase (phase==1), e.g., 16 */
    int post_polydecim_hist_head; /* head index into circular history [0..K-1] */
    int post_polydecim_phase;     /* sample phase accumulator [0..M-1] */
    /* Its fallback, run when the polyphase allocation fails: a one-pole low-pass, then the mean of each M samples. Its
       state carries across blocks as the polyphase decimator's does, so neither depends on where the blocks are cut;
       dsd_demod_reset_filter_state() clears both. */
    float post_fallback_lp_y;    /* the one-pole's last output */
    int post_fallback_lp_valid;  /* 1 once post_fallback_lp_y holds an output; 0 starts it from the next sample */
    float post_fallback_box_acc; /* sum of the part-filled group */
    int post_fallback_box_phase; /* samples in the part-filled group [0..M-1] */
    /* What the decimator state above belongs to: the factor (0 = none yet), the rate_out and the path (1 = the
       fallback) of the block that last ran it. A block with another of any starts the stage over. */
    int post_decim_state_M;
    int post_decim_state_rate_out;
    int post_decim_state_fallback;

    /* Costas diagnostics (updated per block) */
    int costas_err_avg_q14;     /* average smoothed |err| scaled to Q14 for UI/metrics */
    int costas_err_raw_avg_q14; /* average raw |err| before smoothing, scaled to Q14 */
    int costas_conf_avg_q14;    /* average Costas confidence, scaled to Q14 */
    int costas_zero_conf_pct;   /* percent of symbols with zero Costas confidence */
};

// NOLINTEND(clang-analyzer-optin.performance.Padding)

/* The aligned buffers fill whole alignment blocks, so each starts right where the one before it ends: no compiler pads
 * between them (MSVC warns C4324 when one does). */
static_assert(sizeof(demod_state::hb_i_buf) % DSD_DEMOD_BUF_ALIGN == 0, "hb_i_buf must fill whole alignment blocks");
static_assert(sizeof(demod_state::hb_q_buf) % DSD_DEMOD_BUF_ALIGN == 0, "hb_q_buf must fill whole alignment blocks");
static_assert(sizeof(demod_state::hb_i_out) % DSD_DEMOD_BUF_ALIGN == 0, "hb_i_out must fill whole alignment blocks");
static_assert(sizeof(demod_state::hb_q_out) % DSD_DEMOD_BUF_ALIGN == 0, "hb_q_out must fill whole alignment blocks");
static_assert(sizeof(demod_state::input_cb_buf) % DSD_DEMOD_BUF_ALIGN == 0,
              "input_cb_buf must fill whole alignment blocks");
static_assert(sizeof(demod_state::result) % DSD_DEMOD_BUF_ALIGN == 0, "result must fill whole alignment blocks");
static_assert(sizeof(demod_state::timing_buf) % DSD_DEMOD_BUF_ALIGN == 0,
              "timing_buf must fill whole alignment blocks");
static_assert(sizeof(demod_state::resamp_outbuf) % DSD_DEMOD_BUF_ALIGN == 0,
              "resamp_outbuf must fill whole alignment blocks");
static_assert(sizeof(demod_state::channel_lpf_hist_i) % DSD_DEMOD_BUF_ALIGN == 0,
              "channel_lpf_hist_i must fill whole alignment blocks");
static_assert(sizeof(demod_state::channel_lpf_hist_q) % DSD_DEMOD_BUF_ALIGN == 0,
              "channel_lpf_hist_q must fill whole alignment blocks");
static_assert(sizeof(demod_state::channel_lpf_plan_taps) % DSD_DEMOD_BUF_ALIGN == 0,
              "channel_lpf_plan_taps must fill whole alignment blocks");
static_assert(sizeof(demod_state::result_flags) % DSD_DEMOD_BUF_ALIGN == 0,
              "result_flags must fill whole alignment blocks");
static_assert(sizeof(demod_state::resamp_outflags) % DSD_DEMOD_BUF_ALIGN == 0,
              "resamp_outflags must fill whole alignment blocks");

/*
 * Whether the analog family's monitor audio, on the analog channel, is what the demodulator produces. The width-driven
 * channel filter and the published analog profile describe that path only. A symbol profile applied without a family
 * switch leaves the family flag set but moves the front end off that path, and keeps its own profile filter: a CQPSK
 * toggle under -fA moves the output to symbols, and a typed digital scan row's profile puts the row's channel profile
 * in place of the analog (WIDE) one while the monitor output stays.
 */
static inline int
dsd_demod_analog_monitor_active(const struct demod_state* d) {
    return (d && d->analog_family && d->output_kind == DSD_DEMOD_OUTPUT_AUDIO_MONITOR && !d->cqpsk_enable
            && d->channel_lpf_profile == DSD_CH_LPF_PROFILE_WIDE)
               ? 1
               : 0;
}

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_DEMOD_STATE_H_ */
