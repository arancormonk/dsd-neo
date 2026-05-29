// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
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
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/equalizer.h>
#include <dsd-neo/dsp/fll.h>
#include <dsd-neo/dsp/math_utils.h>
#include <dsd-neo/dsp/resampler.h>
#include <dsd-neo/dsp/snr_bias.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/udp_control.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/input_ring_watermark.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/mem.h>
#include <dsd-neo/runtime/ring.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/threading.h>
#include <dsd-neo/runtime/unicode.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/dsp/fsk_modem.h"
#include "dsd-neo/platform/platform.h"
#include "rtl_auto_ppm.h"
#include "rtl_perf.h"
#include "rtl_ppm_request.h"
#include "rtl_replay_device.h"
#include "rtl_stream_shared.hpp"

#ifdef __cplusplus
extern "C" {
#endif
/* Forward declarations for internal helpers used by shims */
void dsd_rtl_stream_clear_output(void);
double dsd_rtl_stream_return_pwr(void);
unsigned int dsd_rtl_stream_output_rate(void);
int dsd_rtl_stream_ted_bias(void);
int dsd_rtl_stream_set_rtltcp_autotune(int onoff);
int dsd_rtl_stream_get_rtltcp_autotune(void);
void dsd_rtl_stream_apply_pending_retune_profile_for_target(uint32_t target_freq_hz);
#ifdef __cplusplus
}
#endif

/* Forward declaration for eye ring append used in demod loop */
static void constellation_ring_append(const float* iq, int len, int sps_hint);
static inline void eye_ring_append_i_chan(const float* iq_interleaved, int len_interleaved);

#define DEFAULT_SAMPLE_RATE 48000
#define AUTO_GAIN           (-100)
#define BUFFER_DUMP         4096

#define FREQUENCIES_LIMIT   1000

static int lcm_post[17] = {1, 1, 1, 3, 1, 5, 3, 7, 1, 9, 5, 11, 3, 13, 7, 15, 1};
static int ACTUAL_BUF_LENGTH;

static const double kPi = 3.14159265358979323846;

#if defined(__clang__)
#define DSD_NEO_PRAGMA(x) _Pragma(#x)
#define DSD_NEO_IVDEP     DSD_NEO_PRAGMA(clang loop vectorize(enable))
#elif defined(__GNUC__)
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

// UDP control handle
static struct udp_control* g_udp_ctrl = NULL;

/* DSP baseband for RTL path in Hz (derived from opts->rtl_dsp_bw_khz). */
static int rtl_dsp_bw_hz;
static short int volume_multiplier;
static uint16_t port;
static char udp_control_bindaddr[64] = "127.0.0.1";

namespace {

struct RtlRetuneProfile {
    int active;
    int cqpsk_enable;
    int symbol_rate_hz;
    int levels;
    int channel_profile;
    int ted_sps;
    int ted_override;
    uint32_t request_id;
    uint32_t target_freq_hz;
};

struct dongle_state {
    dsd_thread_t thread;
    int dev_index;
    std::atomic<uint32_t> freq;
    std::atomic<uint32_t> rate;
    int gain;
    uint32_t buf_len;
    /* Last PPM value successfully applied to hardware. */
    std::atomic<int> ppm_error;
    int offset_tuning;
    int direct_sampling;
    std::atomic<int> mute;
    struct demod_state* demod_target;
};

struct controller_state {
    dsd_thread_t thread;
    uint32_t freqs[FREQUENCIES_LIMIT];
    int freq_len;
    int freq_now;
    int edge;
    int wb_mode;
    dsd_cond_t hop;
    dsd_mutex_t hop_m;
    /* Marshalled retune request from external threads (UDP/API). */
    std::atomic<int> manual_retune_pending;
    uint32_t manual_retune_freq;
    RtlRetuneProfile manual_retune_profile;
    /* Marshalled PPM correction updates stay on the controller thread so
     * device controls remain serialized with retunes/hops. */
    std::atomic<int> ppm_change_pending;
    std::atomic<int> pending_ppm_error;
    std::atomic<uint32_t> ppm_request_publish_seq;
    std::atomic<uint32_t> pending_ppm_request_seq;
    std::atomic<int> ppm_apply_in_progress;
    std::atomic<int> active_ppm_error;
    std::atomic<uint32_t> active_ppm_request_seq;
    /* Reconcile rejected PPM requests back on the read thread without
     * overwriting a newer request that arrived after the failure. */
    std::atomic<int> ppm_apply_failure_pending;
    std::atomic<int> failed_ppm_error;
    std::atomic<uint32_t> failed_ppm_request_seq;
    /* Cold start gate: demod thread skips CQPSK until controller signals ready.
     * This prevents the race where demod processes samples with uninitialized
     * TED/Costas state before the controller finishes cold start configuration. */
    std::atomic<int> cold_start_ready;
    /* Retune gate: demod thread skips processing while retune is in progress.
     * This prevents the race where demod processes transient/stale samples
     * during hardware retune before TED/Costas/AGC are reset. Set to 1 at
     * start of retune, cleared to 0 after reset complete. */
    std::atomic<int> retune_in_progress;
    /* Demod processing gate: the controller waits for the current demod block
     * to finish before mutating shared demodulator state during reconfigure. */
    std::atomic<int> demod_processing_active;
    /* Retune completion signaling: allows dsd_rtl_stream_tune() to block until
     * the controller thread has finished the hardware retune and DSP reset.
     * This prevents the race where trunking code sets SPS parameters before
     * demod_reset_on_retune() has executed, causing Costas/FLL state corruption. */
    dsd_cond_t retune_done_cond;
    dsd_mutex_t retune_done_m;
    std::atomic<int> retune_done_flag;
    /* Request ID for matching completion signals to requests (prevents stale wakeups) */
    std::atomic<uint32_t> retune_request_id;
    std::atomic<uint32_t> retune_complete_id;
    /* Last center frequency successfully applied by the controller thread. */
    std::atomic<uint32_t> last_applied_freq_hz;
    /* Completed capture reconfigure generation. This lets consumer-side
     * holdoffs reset even when the tuned center frequency remains unchanged
     * (for example, live PPM correction on the active stream). */
    std::atomic<uint32_t> reconfigure_seq;
};

} // namespace

struct demod_state demod;
static struct rtl_device* rtl_device_handle = NULL;
static struct dongle_state dongle;
static struct output_state output;
static struct output_state monitor_output;
static struct controller_state controller;
static struct input_ring_state input_ring;
static dsd_iq_capture_writer* g_iq_capture_writer = NULL;
/* Controller can request a ring purge; consumer/demod performs the discard safely. */
static std::atomic<int> g_ring_purge_pending{0};
static std::atomic<uint32_t> g_retune_diag_seq{0};
static std::atomic<uint32_t> g_retune_diag_freq_hz{0};
static std::atomic<int> g_retune_diag_reason{0};
static std::atomic<int> g_retune_diag_blocks_remaining{0};
static std::atomic<uint32_t> g_retune_settle_seq{0};
static std::atomic<int> g_retune_settle_blocks_remaining{0};
static std::atomic<uint32_t> g_rtl_output_generation{1};
static std::atomic<int> g_fsk_reacquire_pending{0};
static std::mutex g_pending_retune_profile_mutex;
static RtlRetuneProfile g_pending_retune_profile;
static std::atomic<uint32_t> g_replay_event_retune_count{0};
static std::atomic<uint32_t> g_replay_event_mute_count{0};
static std::atomic<uint32_t> g_replay_event_reset_count{0};
static std::atomic<uint32_t> g_replay_event_last_frequency_hz{0};
static std::atomic<uint64_t> g_replay_event_last_mute_bytes{0};
static std::atomic<int> g_replay_event_last_reset_reason{0};
static std::atomic<uint32_t> g_replay_loop_restart_count{0};
static std::atomic<uint32_t> g_replay_loop_restart_last_frequency_hz{0};

static int rtl_stream_consume_fsk_reacquire_pending(struct demod_state* d);
static int controller_apply_replay_settings(struct controller_state* s, const dsd_opts* opts,
                                            const dsd_iq_replay_config* cfg);
static const int kRetuneDiagBlocks = 20;
static uint32_t rtl_stream_bump_output_generation(void);
static void rtl_stream_clear_retune_profile(RtlRetuneProfile* profile);
static int rtl_stream_take_pending_retune_profile(RtlRetuneProfile* out_profile, uint32_t request_id,
                                                  uint32_t target_freq_hz);
static void rtl_stream_store_pending_retune_profile(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz,
                                                    int levels, int channel_profile, int ted_sps,
                                                    int persist_ted_override);
static void rtl_stream_apply_retune_profile(const RtlRetuneProfile* profile, uint32_t center_freq_hz);
static void rtl_fsk_metrics_reset_snapshot(void);
static void rtl_decode_health_reset(void);
static void rtl_publish_fsk_metrics_from_demod(const struct demod_state* d);
static void rtl_stream_signal_output_waiters(struct output_state* outp);
static void rtl_stream_clear_output_ring(struct output_state* outp, int bump_generation);
/*
 * The controller already purges stale samples and applies a time-based hardware
 * mute after retunes. Do not additionally drop whole demod blocks here: at RTL
 * block sizes one "block" can cover a large fraction of a P25P2 superframe, so
 * block-based CQPSK settling can discard the early ISCH/sync bursts needed for
 * Phase 2 acquisition.
 */
static const int kRetuneSettleMinBlocks = 1;
static const int kRetuneSettleStableBlocks = 1;
static const int kRetuneSettleMaxBlocks = 1;
static const float kRetuneSettleStableRel = 0.055f;
static const float kRetuneSettleMinMeanAbs = 0.015f;

static dsd::io::radio::RtlAutoPpmController g_auto_ppm_controller;

static inline uint32_t
load_dongle_frequency(void) {
    return dongle.freq.load(std::memory_order_acquire);
}

static inline void
store_dongle_frequency(uint32_t frequency_hz) {
    dongle.freq.store(frequency_hz, std::memory_order_release);
}

static inline uint32_t
load_dongle_rate(void) {
    return dongle.rate.load(std::memory_order_acquire);
}

static inline void
store_dongle_rate(uint32_t sample_rate_hz) {
    dongle.rate.store(sample_rate_hz, std::memory_order_release);
}

static uint32_t
clamp_capture_frequency_hz(int64_t frequency_hz) {
    if (frequency_hz <= 0) {
        return 0U;
    }
    if (frequency_hz > (int64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)frequency_hz;
}

static uint32_t
capture_frequency_for_rate(int64_t center_freq_hz, uint32_t capture_rate_hz) {
    int64_t capture_freq_hz = center_freq_hz;
    if (!dongle.offset_tuning && !disable_fs4_shift) {
        capture_freq_hz += (int64_t)(capture_rate_hz / 4U);
    }
    capture_freq_hz += ((int64_t)controller.edge * (int64_t)demod.rate_in) / 2;
    return clamp_capture_frequency_hz(capture_freq_hz);
}

static int
demod_output_rate_for_capture_rate(uint32_t capture_rate_hz) {
    int base_decim = (demod.downsample_passes > 0) ? (1 << demod.downsample_passes) : 1;
    if (base_decim < 1) {
        base_decim = 1;
    }

    int out_rate = (int)(capture_rate_hz / (uint32_t)base_decim);
    if (demod.post_downsample > 1) {
        out_rate /= demod.post_downsample;
        if (out_rate < 1) {
            out_rate = 1;
        }
    }
    return out_rate;
}

static void
retune_capture_frequency_for_actual_rate(uint32_t center_freq_hz, uint32_t requested_capture_freq_hz,
                                         uint32_t actual_capture_rate_hz) {
    uint32_t actual_capture_freq_hz = capture_frequency_for_rate((int64_t)center_freq_hz, actual_capture_rate_hz);
    if (actual_capture_freq_hz == requested_capture_freq_hz) {
        return;
    }

    int rc = rtl_device_set_frequency(rtl_device_handle, actual_capture_freq_hz);
    if (rc == 0) {
        store_dongle_frequency(actual_capture_freq_hz);
        LOG_INFO("Adjusted fs/4 capture center for actual device rate: center=%u, capture=%u Hz.\n", center_freq_hz,
                 actual_capture_freq_hz);
    } else {
        LOG_WARNING("Failed to adjust fs/4 capture center for actual device rate: center=%u, capture=%u Hz (rc=%d).\n",
                    center_freq_hz, actual_capture_freq_hz, rc);
    }
}

static uint32_t
apply_actual_capture_rate(uint32_t center_freq_hz, uint32_t requested_capture_freq_hz,
                          uint32_t requested_capture_rate_hz, uint32_t actual_capture_rate_hz) {
    if (actual_capture_rate_hz == 0 || actual_capture_rate_hz == requested_capture_rate_hz) {
        return requested_capture_rate_hz;
    }

    store_dongle_rate(actual_capture_rate_hz);
    demod.rate_out = demod_output_rate_for_capture_rate(actual_capture_rate_hz);
    retune_capture_frequency_for_actual_rate(center_freq_hz, requested_capture_freq_hz, actual_capture_rate_hz);
    LOG_INFO("Adjusted to actual device rate: requested=%u, actual=%u, demod_out=%d Hz.\n", requested_capture_rate_hz,
             actual_capture_rate_hz, demod.rate_out);
    return actual_capture_rate_hz;
}

static void
controller_request_input_purge(void) {
    input_ring_request_discard(&input_ring);
    g_ring_purge_pending.store(1, std::memory_order_release);
}

namespace {

struct RtlSdrInternals {
    struct rtl_device* device;
    struct dongle_state* dongle;
    struct demod_state* demod;
    struct output_state* output;
    struct controller_state* controller;
    struct input_ring_state* input_ring;
    struct udp_control** udp_ctrl_ptr;
    const dsd_opts* opts; /* snapshot for mode hints (P25p1/2, etc.) */
    /* Cooperative shutdown flag for threads launched by this stream */
    std::atomic<int> should_exit;
    std::atomic<int> controller_thread_started;
    std::atomic<int> demod_thread_started;
    std::atomic<int> async_started;

    /* Replay EOF State Machine. See "Replay EOF State Machine" section. */
    std::atomic<int> replay_input_eof;
    std::atomic<int> replay_input_drained;
    std::atomic<int> replay_demod_drained;
    std::atomic<int> replay_output_drained;
    std::atomic<int> replay_forced_stop;
    std::atomic<uint64_t> replay_last_submit_gen;
    std::atomic<uint64_t> replay_last_submit_gen_at_eof;
    std::atomic<uint64_t> replay_last_consume_gen;
    dsd_mutex_t replay_eof_m;
    dsd_cond_t replay_eof_cond;
    int replay_eof_sync_inited;

    /* Watermark-based flow control for TCP lag resilience */
    struct input_ring_watermark watermark;
};

} // namespace

static struct RtlSdrInternals* g_stream = NULL;
static float g_monitor_fm_prev_r = 0.0f;
static float g_monitor_fm_prev_j = 0.0f;
static int g_monitor_fm_have_prev = 0;
static float g_monitor_fm_buf[MAXIMUM_BUF_LENGTH / 2];

static void
ring_write_drop_oldest_signal(struct output_state* o, const float* data, size_t count) {
    if (!o || !o->buffer || !data || count == 0 || o->capacity < 2) {
        return;
    }
    if (count >= o->capacity) {
        size_t skip = count - (o->capacity - 1);
        data += skip;
        count -= skip;
    }
    size_t free_count = ring_free(o);
    if (free_count < count) {
        size_t drop = count - free_count;
        size_t tail = o->tail.load();
        tail = (tail + drop) % o->capacity;
        o->tail.store(tail);
    }
    int was_empty = ring_is_empty(o);
    ring_write_no_signal(o, data, count);
    if (was_empty) {
        safe_cond_signal(&o->ready, &o->ready_m);
    }
}

static int
ring_read_available(struct output_state* o, float* out, size_t count) {
    if (!o || !o->buffer || !out || count == 0) {
        return 0;
    }
    size_t got = 0;
    size_t tail = o->tail.load();
    const size_t head = o->head.load();
    while (got < count && tail != head) {
        out[got++] = o->buffer[tail];
        tail++;
        if (tail >= o->capacity) {
            tail = 0;
        }
    }
    if (got > 0) {
        o->tail.store(tail);
        safe_cond_signal(&o->space, &o->ready_m);
    }
    return (int)got;
}

static int
rtl_monitor_side_tap_active(const struct demod_state* d) {
    if (!d || !g_stream || !g_stream->opts) {
        return 0;
    }
    if (d->output_kind != DSD_DEMOD_OUTPUT_SYMBOL_FSK && d->output_kind != DSD_DEMOD_OUTPUT_SYMBOL_CQPSK) {
        return 0;
    }
    return (g_stream->opts->monitor_input_audio == 1 || g_stream->opts->analog_only == 1) ? 1 : 0;
}

static void
rtl_monitor_side_tap_process(const struct demod_state* d) {
    if (!rtl_monitor_side_tap_active(d) || !monitor_output.buffer || !d->lowpassed || d->lp_len < 2) {
        return;
    }
    const int pairs = d->lp_len >> 1;
    if (pairs <= 0) {
        return;
    }
    const int max_pairs = (int)(sizeof(g_monitor_fm_buf) / sizeof(g_monitor_fm_buf[0]));
    const int n_out = pairs < max_pairs ? pairs : max_pairs;
    const float* iq = d->lowpassed;
    float prev_r = g_monitor_fm_prev_r;
    float prev_j = g_monitor_fm_prev_j;
    if (!g_monitor_fm_have_prev) {
        prev_r = iq[0];
        prev_j = iq[1];
        g_monitor_fm_have_prev = 1;
    }
    int gain = g_stream->opts->rtl_volume_multiplier;
    if (gain < 1) {
        gain = 1;
    } else if (gain > 3) {
        gain = 3;
    }
    for (int n = 0; n < n_out; n++) {
        float cr = iq[(size_t)(n << 1) + 0];
        float cj = iq[(size_t)(n << 1) + 1];
        float re = cr * prev_r + cj * prev_j;
        float im = cj * prev_r - cr * prev_j;
        g_monitor_fm_buf[n] = atan2f(im, re) * (float)gain;
        prev_r = cr;
        prev_j = cj;
    }
    g_monitor_fm_prev_r = prev_r;
    g_monitor_fm_prev_j = prev_j;
    ring_write_drop_oldest_signal(&monitor_output, g_monitor_fm_buf, (size_t)n_out);
}

/* Keep the requested PPM value and its logical request generation paired so
 * UI and read-thread activity cannot observe mixed snapshots. */
static std::mutex g_requested_ppm_state_mutex;

static int radio_source_is_iq_replay(const dsd_opts* opts);

static inline int
stream_is_replay_active(void) {
    return (g_stream && g_stream->opts && radio_source_is_iq_replay(g_stream->opts)) ? 1 : 0;
}

static inline int
rtl_stream_context_active(void) {
    struct RtlSdrInternals* s = g_stream;
    if (!s || s->should_exit.load(std::memory_order_acquire)) {
        return 0;
    }
    return (s->async_started.load(std::memory_order_acquire) || s->demod_thread_started.load(std::memory_order_acquire)
            || s->controller_thread_started.load(std::memory_order_acquire))
               ? 1
               : 0;
}

static void
stream_reset_replay_eof_state(struct RtlSdrInternals* s) {
    if (!s) {
        return;
    }
    s->replay_input_eof.store(0, std::memory_order_release);
    s->replay_input_drained.store(0, std::memory_order_release);
    s->replay_demod_drained.store(0, std::memory_order_release);
    s->replay_output_drained.store(0, std::memory_order_release);
    s->replay_forced_stop.store(0, std::memory_order_release);
    s->replay_last_submit_gen.store(0, std::memory_order_release);
    s->replay_last_submit_gen_at_eof.store(0, std::memory_order_release);
    s->replay_last_consume_gen.store(0, std::memory_order_release);
    g_replay_event_retune_count.store(0, std::memory_order_release);
    g_replay_event_mute_count.store(0, std::memory_order_release);
    g_replay_event_reset_count.store(0, std::memory_order_release);
    g_replay_event_last_frequency_hz.store(0, std::memory_order_release);
    g_replay_event_last_mute_bytes.store(0, std::memory_order_release);
    g_replay_event_last_reset_reason.store(0, std::memory_order_release);
    g_replay_loop_restart_count.store(0, std::memory_order_release);
    g_replay_loop_restart_last_frequency_hz.store(0, std::memory_order_release);
}

static void
rtl_replay_on_input_drained(void* user) {
    struct RtlSdrInternals* s = static_cast<RtlSdrInternals*>(user);
    if (!s) {
        return;
    }
    dsd_mutex_lock(&s->replay_eof_m);
    dsd_cond_broadcast(&s->replay_eof_cond);
    dsd_mutex_unlock(&s->replay_eof_m);
    safe_cond_signal(&output.ready, &output.ready_m);
}

static void
replay_note_input_purge_consumed(void) {
    if (!stream_is_replay_active() || !g_stream) {
        return;
    }

    uint64_t consumed_gen = g_stream->replay_last_submit_gen.load(std::memory_order_acquire);
    g_stream->replay_last_consume_gen.store(consumed_gen, std::memory_order_release);
    if (!g_stream->replay_input_eof.load(std::memory_order_acquire) || input_ring_used(&input_ring) != 0U) {
        return;
    }

    g_stream->replay_input_drained.store(1, std::memory_order_release);
    uint64_t eof_gen = g_stream->replay_last_submit_gen_at_eof.load(std::memory_order_acquire);
    if (consumed_gen >= eof_gen && !g_stream->replay_demod_drained.load(std::memory_order_acquire)) {
        g_stream->replay_demod_drained.store(1, std::memory_order_release);
        safe_cond_signal(&output.ready, &output.ready_m);
    }
    if (g_stream->replay_eof_sync_inited) {
        dsd_mutex_lock(&g_stream->replay_eof_m);
        dsd_cond_broadcast(&g_stream->replay_eof_cond);
        dsd_mutex_unlock(&g_stream->replay_eof_m);
    }
}

static void rtl_replay_on_retune_event(const dsd_iq_event* event, void* user);
static void rtl_replay_on_mute_event(const dsd_iq_event* event, void* user);
static void rtl_replay_on_reset_event(const dsd_iq_event* event, void* user);
static void rtl_replay_on_loop_restart(const dsd_iq_replay_config* cfg, void* user);

namespace {

struct RtlRequestedPpmMirrors {
    dsd_opts* active_opts = NULL;
    dsd_opts* caller_opts = NULL;
};

static RtlRequestedPpmMirrors g_requested_ppm_mirrors = {};

static int
parse_int_atoi_compat(const char* text, int* out) {
    if (!text || !*text || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    long v = strtol(text, &end, 10);
    if (end == text || (end && *end != '\0') || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int
parse_double_atof_compat(const char* text, double* out) {
    if (!text || !*text || !out) {
        return 0;
    }
    errno = 0;
    char* end = NULL;
    double v = strtod(text, &end);
    if (end == text || (end && *end != '\0') || errno == ERANGE) {
        return 0;
    }
    *out = v;
    return 1;
}

enum RadioSourceKind : uint8_t {
    RADIO_SOURCE_RTL_USB = 0,
    RADIO_SOURCE_RTL_TCP = 1,
    RADIO_SOURCE_SOAPY = 2,
    RADIO_SOURCE_IQ_REPLAY = 3,
};

} // namespace

static RadioSourceKind
detect_radio_source(const dsd_opts* opts) {
    const char* dev = (opts) ? opts->audio_in_dev : NULL;
    if (!dev) {
        return RADIO_SOURCE_RTL_USB;
    }
    if ((strcmp(dev, "rtltcp") == 0) || (strncmp(dev, "rtltcp:", 7) == 0)) {
        return RADIO_SOURCE_RTL_TCP;
    }
    if ((strcmp(dev, "soapy") == 0) || (strncmp(dev, "soapy:", 6) == 0)) {
        return RADIO_SOURCE_SOAPY;
    }
    if (dsd_opts_audio_in_dev_is_iqreplay_spec(dev)) {
        return RADIO_SOURCE_IQ_REPLAY;
    }
    return RADIO_SOURCE_RTL_USB;
}

static int
radio_source_is_rtltcp(const dsd_opts* opts) {
    return detect_radio_source(opts) == RADIO_SOURCE_RTL_TCP;
}

static int
radio_source_is_soapy(const dsd_opts* opts) {
    return detect_radio_source(opts) == RADIO_SOURCE_SOAPY;
}

static int
radio_source_is_iq_replay(const dsd_opts* opts) {
    return detect_radio_source(opts) == RADIO_SOURCE_IQ_REPLAY;
}

static int
radio_source_is_rtl_family(const dsd_opts* opts) {
    RadioSourceKind kind = detect_radio_source(opts);
    return (kind == RADIO_SOURCE_RTL_USB || kind == RADIO_SOURCE_RTL_TCP || kind == RADIO_SOURCE_SOAPY
            || kind == RADIO_SOURCE_IQ_REPLAY)
               ? 1
               : 0;
}

static int
stream_steady_state_watermark_enabled(const dsd_opts* opts) {
    UNUSED(opts);
    /*
     * Do not pace live demodulation with the input-ring watermark. rtl_tcp
     * already has a startup prebuffer, socket buffering, reader-side
     * backpressure, metrics, and stall/reconnect checks. Re-enabling this
     * watermark makes normal real-time jitter trigger intentional demod stalls
     * until hundreds of milliseconds, and eventually 1.5 seconds, of IQ data
     * refills.
     */
    return 0;
}

static const char*
rtl_perf_source_name(void) {
    const dsd_opts* opts = (g_stream && g_stream->opts) ? g_stream->opts : NULL;
    switch (detect_radio_source(opts)) {
        case RADIO_SOURCE_RTL_TCP: return "rtltcp";
        case RADIO_SOURCE_SOAPY: return "soapy";
        case RADIO_SOURCE_IQ_REPLAY: return "iq_replay";
        case RADIO_SOURCE_RTL_USB:
        default: return "rtl";
    }
}

static int
opts_is_digital_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_provoice == 1 || opts->frame_dmr == 1
            || opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1 || opts->frame_x2tdma == 1 || opts->frame_ysf == 1
            || opts->frame_dstar == 1 || opts->frame_dpmr == 1 || opts->frame_m17 == 1);
}

static int
opts_has_4800_wide_four_level_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_dmr == 1 || opts->frame_nxdn96 == 1 || opts->frame_ysf == 1 || opts->frame_m17 == 1);
}

static int
opts_has_12k5_or_cqpsk_bw_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_provoice == 1 || opts->frame_dmr == 1
            || opts->frame_nxdn96 == 1 || opts->frame_x2tdma == 1 || opts->frame_ysf == 1 || opts->frame_m17 == 1
            || opts->mod_qpsk == 1);
}

static int
rtl_stream_fsk_profile_for_opts_by_sym_rate(const dsd_opts* opts, int sym_rate) {
    if (!opts) {
        return -1;
    }
    if (sym_rate == 9600 && opts->frame_provoice == 1) {
        return DSD_CH_LPF_PROFILE_PROVOICE;
    }
    if (sym_rate == 2400 && (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1)) {
        return DSD_CH_LPF_PROFILE_6K25;
    }
    if (sym_rate == 6000 && opts->frame_x2tdma == 1) {
        return DSD_CH_LPF_PROFILE_12K5;
    }
    return -1;
}

static int
rtl_stream_fsk_profile_for_opts_by_frame(const dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    if (opts_has_4800_wide_four_level_mode(opts)) {
        return DSD_CH_LPF_PROFILE_12K5;
    }
    if (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1) {
        return DSD_CH_LPF_PROFILE_P25_C4FM;
    }
    if (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1 || opts->frame_dstar == 1) {
        return DSD_CH_LPF_PROFILE_6K25;
    }
    if (opts->frame_x2tdma == 1) {
        return DSD_CH_LPF_PROFILE_12K5;
    }
    if (opts->frame_provoice == 1) {
        return DSD_CH_LPF_PROFILE_PROVOICE;
    }
    return -1;
}

static int
rtl_stream_fsk_profile_for_symbol_rate(int sym_rate, int levels) {
    if (sym_rate == 2400) {
        return DSD_CH_LPF_PROFILE_6K25;
    }
    if (sym_rate == 9600) {
        return DSD_CH_LPF_PROFILE_PROVOICE;
    }
    if (sym_rate >= 6000) {
        return DSD_CH_LPF_PROFILE_12K5;
    }
    if (sym_rate == 4800 && levels == 2) {
        return DSD_CH_LPF_PROFILE_6K25;
    }
    if (sym_rate == 4800 && levels == 4) {
        return DSD_CH_LPF_PROFILE_12K5;
    }
    return DSD_CH_LPF_PROFILE_WIDE;
}

static int
rtl_stream_fsk_channel_profile_for_current_mode(void) {
    const dsd_opts* opts = (g_stream && g_stream->opts) ? g_stream->opts : NULL;
    const int sym_rate = demod.symbol_rate_hz > 0 ? demod.symbol_rate_hz : 4800;
    const int levels = demod.symbol_levels == 2 ? 2 : 4;
    int profile = rtl_stream_fsk_profile_for_opts_by_sym_rate(opts, sym_rate);
    if (profile >= 0) {
        return profile;
    }
    profile = rtl_stream_fsk_profile_for_opts_by_frame(opts);
    if (profile >= 0) {
        return profile;
    }
    return rtl_stream_fsk_profile_for_symbol_rate(sym_rate, levels);
}

static void
stream_refresh_watermark_for_current_rate(void) {
    if (!g_stream) {
        return;
    }
    watermark_init(&g_stream->watermark, stream_steady_state_watermark_enabled(g_stream->opts), load_dongle_rate());
}

static const char*
radio_source_soapy_args(const dsd_opts* opts) {
    if (!radio_source_is_soapy(opts) || !opts) {
        return "";
    }
    const char* colon = strchr(opts->audio_in_dev, ':');
    if (!colon || colon[1] == '\0') {
        return "";
    }
    return colon + 1;
}

static const char*
radio_source_replay_path(const dsd_opts* opts) {
    if (!opts || !radio_source_is_iq_replay(opts)) {
        return NULL;
    }
    const char* dev = opts->audio_in_dev;
    const char* colon = dev ? strchr(dev, ':') : NULL;
    if (colon && colon[1] != '\0') {
        return colon + 1;
    }
    if (opts->iq_replay_path[0] != '\0') {
        return opts->iq_replay_path;
    }
    return NULL;
}

static void
log_unsupported_control_if_needed(const char* control_name, int rc) {
    if (rc == DSD_ERR_NOT_SUPPORTED) {
        LOG_NOTICE("%s unsupported by active radio backend.\n", control_name);
    }
}

static int
apply_ppm_setting(int ppm_error) {
    int rc = rtl_device_set_ppm(rtl_device_handle, ppm_error);
    log_unsupported_control_if_needed("PPM correction control", rc);
    return rc;
}

static inline int
load_dongle_ppm_error(void) {
    return dongle.ppm_error.load(std::memory_order_acquire);
}

static inline void
store_dongle_ppm_error(int ppm_error) {
    dongle.ppm_error.store(ppm_error, std::memory_order_release);
}

static inline int
clamp_requested_ppm(int ppm_error) {
    if (ppm_error < -200) {
        return -200;
    }
    if (ppm_error > 200) {
        return 200;
    }
    return ppm_error;
}

static const dsd_opts*
requested_ppm_source_opts_locked(const dsd_opts* fallback_opts) {
    if (g_requested_ppm_mirrors.active_opts) {
        return g_requested_ppm_mirrors.active_opts;
    }
    if (g_requested_ppm_mirrors.caller_opts) {
        return g_requested_ppm_mirrors.caller_opts;
    }
    return fallback_opts;
}

static void
sync_requested_ppm_snapshots_locked(dsd_opts* touched_opts, int ppm_error) {
    int clamped_ppm = clamp_requested_ppm(ppm_error);
    if (g_requested_ppm_mirrors.active_opts) {
        g_requested_ppm_mirrors.active_opts->rtlsdr_ppm_error = clamped_ppm;
    }
    if (g_requested_ppm_mirrors.caller_opts
        && g_requested_ppm_mirrors.caller_opts != g_requested_ppm_mirrors.active_opts) {
        g_requested_ppm_mirrors.caller_opts->rtlsdr_ppm_error = clamped_ppm;
    }
    if (touched_opts && touched_opts != g_requested_ppm_mirrors.active_opts
        && touched_opts != g_requested_ppm_mirrors.caller_opts) {
        touched_opts->rtlsdr_ppm_error = clamped_ppm;
    }
}

extern "C" void
dsd_rtl_stream_register_requested_ppm_opts(dsd_opts* active_opts, dsd_opts* caller_opts) {
    if (!active_opts) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_requested_ppm_state_mutex);
    g_requested_ppm_mirrors.active_opts = active_opts;
    g_requested_ppm_mirrors.caller_opts = caller_opts ? caller_opts : active_opts;
    int initial_ppm = caller_opts ? caller_opts->rtlsdr_ppm_error : active_opts->rtlsdr_ppm_error;
    sync_requested_ppm_snapshots_locked(active_opts, initial_ppm);
}

extern "C" void
dsd_rtl_stream_unregister_requested_ppm_opts(dsd_opts* active_opts, dsd_opts* caller_opts) {
    if (!active_opts) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_requested_ppm_state_mutex);
    const dsd_opts* expected_caller_opts = caller_opts ? caller_opts : active_opts;
    if (g_requested_ppm_mirrors.active_opts == active_opts
        && g_requested_ppm_mirrors.caller_opts == expected_caller_opts) {
        g_requested_ppm_mirrors.active_opts = NULL;
        g_requested_ppm_mirrors.caller_opts = NULL;
    }
}

namespace {

struct RtlRequestedPpmState {
    int ppm = 0;
    uint32_t request_id = 0;
};

} // namespace

static RtlRequestedPpmState
snapshot_requested_ppm_state(const dsd_opts* opts) {
    RtlRequestedPpmState snapshot = {};
    std::lock_guard<std::mutex> lock(g_requested_ppm_state_mutex);
    const dsd_opts* source_opts = requested_ppm_source_opts_locked(opts);
    if (!source_opts) {
        return snapshot;
    }
    snapshot.ppm = source_opts->rtlsdr_ppm_error;
    snapshot.request_id = controller.ppm_request_publish_seq.load(std::memory_order_relaxed);
    return snapshot;
}

static uint32_t
publish_requested_ppm(dsd_opts* opts, int ppm_error) {
    if (!opts) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_requested_ppm_state_mutex);
    sync_requested_ppm_snapshots_locked(opts, ppm_error);
    uint32_t request_id = controller.ppm_request_publish_seq.fetch_add(1U, std::memory_order_relaxed) + 1U;
    return request_id;
}

static uint32_t
publish_requested_ppm_delta(dsd_opts* opts, int delta) {
    if (!opts) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_requested_ppm_state_mutex);
    const dsd_opts* source_opts = requested_ppm_source_opts_locked(opts);
    int requested_ppm = source_opts ? source_opts->rtlsdr_ppm_error : 0;
    sync_requested_ppm_snapshots_locked(opts, requested_ppm + delta);
    uint32_t request_id = controller.ppm_request_publish_seq.fetch_add(1U, std::memory_order_relaxed) + 1U;
    return request_id;
}

static int
rollback_requested_ppm_if_latest(dsd_opts* opts, int applied_ppm, uint32_t failed_request_id) {
    if (!opts) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_requested_ppm_state_mutex);
    if (controller.ppm_request_publish_seq.load(std::memory_order_relaxed) != failed_request_id) {
        return 0;
    }
    sync_requested_ppm_snapshots_locked(opts, applied_ppm);
    controller.ppm_request_publish_seq.store(failed_request_id + 1U, std::memory_order_relaxed);
    return 1;
}

static void
note_failed_ppm_request(int requested_ppm, uint32_t request_id, int applied_ppm, int rc) {
    controller.failed_ppm_error.store(requested_ppm, std::memory_order_release);
    controller.failed_ppm_request_seq.store(request_id, std::memory_order_release);
    controller.ppm_apply_failure_pending.store(1, std::memory_order_release);
    LOG_NOTICE("PPM correction request %d failed (rc=%d); keeping applied value %d.\n", requested_ppm, rc, applied_ppm);
}

static void
sync_requested_ppm_after_failed_apply(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    if (!controller.ppm_apply_failure_pending.exchange(0, std::memory_order_acq_rel)) {
        return;
    }

    int applied_ppm = load_dongle_ppm_error();
    RtlRequestedPpmState requested = snapshot_requested_ppm_state(opts);
    uint32_t failed_request_id = controller.failed_ppm_request_seq.load(std::memory_order_acquire);
    dsd::io::radio::RtlPpmRejectedRequestResolution resolution = dsd::io::radio::rtl_ppm_resolve_rejected_request(
        applied_ppm, requested.ppm, requested.request_id, failed_request_id);
    if (resolution.rolled_back) {
        (void)rollback_requested_ppm_if_latest(opts, resolution.requested_ppm, failed_request_id);
    }
}

static dsd::io::radio::RtlPpmControllerRequestsSnapshot
snapshot_controller_ppm_request_state(void) {
    dsd::io::radio::RtlPpmControllerRequestsSnapshot snapshot = {};
    dsd_mutex_lock(&controller.hop_m);
    snapshot.active_request.pending = controller.ppm_apply_in_progress.load(std::memory_order_acquire);
    if (snapshot.active_request.pending) {
        snapshot.active_request.ppm = controller.active_ppm_error.load(std::memory_order_acquire);
        snapshot.active_request.request_id = controller.active_ppm_request_seq.load(std::memory_order_acquire);
    }
    snapshot.queued_request.pending = controller.ppm_change_pending.load(std::memory_order_acquire);
    if (snapshot.queued_request.pending) {
        snapshot.queued_request.ppm = controller.pending_ppm_error.load(std::memory_order_acquire);
        snapshot.queued_request.request_id = controller.pending_ppm_request_seq.load(std::memory_order_acquire);
    }
    dsd_mutex_unlock(&controller.hop_m);
    return snapshot;
}

static inline int
debug_cqpsk_enabled(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    return (cfg && cfg->debug_cqpsk_enable) ? 1 : 0;
}

static inline int
debug_sync_enabled(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    return (cfg && cfg->debug_sync_enable) ? 1 : 0;
}

/*
 * Pick a hardware tuner bandwidth.
 *
 * Priority:
 *  - If env DSD_NEO_TUNER_BW_HZ is set:
 *      - value "auto" or 0 => return 0 (driver automatic)
 *      - positive integer => use that value (in Hz)
 *  - Otherwise, leave tuner bandwidth in driver automatic mode.
 */
static uint32_t
choose_tuner_bw_hz(uint32_t capture_rate_hz, uint32_t dsp_bw_hz) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->tuner_bw_hz_is_set) {
        return (uint32_t)cfg->tuner_bw_hz; /* 0 => driver automatic */
    }

    UNUSED(capture_rate_hz);
    UNUSED(dsp_bw_hz);
    return 0; /* driver automatic */
}

static int
soapy_bandwidth_request_is_explicit(const dsd_opts* opts) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->tuner_bw_hz_is_set) {
        return 1;
    }
    return (opts && radio_source_is_soapy(opts) && opts->soapy_bandwidth_hz >= 0) ? 1 : 0;
}

static int
apply_capture_tuner_bandwidth(uint32_t capture_rate_hz, const dsd_opts* opts, int fail_explicit_soapy) {
    int rc =
        rtl_device_set_tuner_bandwidth(rtl_device_handle, choose_tuner_bw_hz(capture_rate_hz, (uint32_t)rtl_dsp_bw_hz));
    if (rc == 0) {
        return 0;
    }
    if (radio_source_is_soapy(opts) && soapy_bandwidth_request_is_explicit(opts)) {
        if (fail_explicit_soapy) {
            LOG_ERROR("Failed to apply explicit SoapySDR bandwidth request (rc=%d).\n", rc);
            return rc;
        }
        LOG_NOTICE("Explicit SoapySDR bandwidth request failed during reconfigure (rc=%d).\n", rc);
        return 0;
    }
    log_unsupported_control_if_needed("Tuner bandwidth control", rc);
    return 0;
}

/* Forward declarations for visualization ring clears (defined later in file) */
static void constellation_ring_clear(void);
static void eye_ring_clear(void);
static void snr_ema_reset(void);
static void controller_arm_retune_mute(const char* phase);

namespace {

enum class DemodRetuneResetReason : uint8_t {
    FrequencyRetune,
    DistantFrequencyRetune,
    PpmCorrection,
    FreshStream,
};

struct DemodRetuneResetPlan {
    DemodRetuneResetReason reason;
    float retained_fll_scale;
};

} // namespace

static const char*
retune_reset_reason_name(DemodRetuneResetReason reason) {
    switch (reason) {
        case DemodRetuneResetReason::FrequencyRetune: return "frequency";
        case DemodRetuneResetReason::DistantFrequencyRetune: return "distant-frequency";
        case DemodRetuneResetReason::PpmCorrection: return "ppm-correction";
        case DemodRetuneResetReason::FreshStream: return "fresh-stream";
        default: return "unknown";
    }
}

static DemodRetuneResetReason
retune_reset_reason_from_name(const char* reason) {
    if (!reason) {
        return DemodRetuneResetReason::FrequencyRetune;
    }
    if (strcmp(reason, "ppm-correction") == 0) {
        return DemodRetuneResetReason::PpmCorrection;
    }
    if (strcmp(reason, "distant-frequency") == 0) {
        return DemodRetuneResetReason::DistantFrequencyRetune;
    }
    if (strcmp(reason, "fresh-stream") == 0) {
        return DemodRetuneResetReason::FreshStream;
    }
    return DemodRetuneResetReason::FrequencyRetune;
}

static uint64_t
frequency_delta_hz(uint32_t lhs_hz, uint32_t rhs_hz) {
    return (lhs_hz >= rhs_hz) ? (uint64_t)(lhs_hz - rhs_hz) : (uint64_t)(rhs_hz - lhs_hz);
}

static DemodRetuneResetPlan
demod_retune_reset_plan(DemodRetuneResetReason requested_reason, uint32_t previous_center_freq_hz,
                        uint32_t next_center_freq_hz, int previous_rate_out_hz, int next_rate_out_hz) {
    DemodRetuneResetPlan plan = {requested_reason, 1.0f};
    if (requested_reason != DemodRetuneResetReason::FrequencyRetune) {
        return plan;
    }

    /* Retained band-edge FLL is a useful seed for quick CC/VC hops inside the
     * same RF band. For unknown or distant retunes, the old normalized NCO can
     * be a bad rotation seed, so force fresh acquisition instead. */
    const uint64_t kMaxRetainedFllRetuneDeltaHz = 25000000ULL;
    if (previous_center_freq_hz == 0 || next_center_freq_hz == 0 || previous_rate_out_hz <= 0 || next_rate_out_hz <= 0
        || frequency_delta_hz(previous_center_freq_hz, next_center_freq_hz) > kMaxRetainedFllRetuneDeltaHz) {
        plan.reason = DemodRetuneResetReason::DistantFrequencyRetune;
        return plan;
    }

    double rf_scale = (double)next_center_freq_hz / (double)previous_center_freq_hz;
    double rate_scale = (double)previous_rate_out_hz / (double)next_rate_out_hz;
    double retained_fll_scale = rf_scale * rate_scale;
    if (retained_fll_scale <= 0.25 || retained_fll_scale >= 4.0) {
        plan.reason = DemodRetuneResetReason::DistantFrequencyRetune;
        return plan;
    }

    plan.retained_fll_scale = (float)retained_fll_scale;
    return plan;
}

static void
iq_block_abs_stats(const float* iq, int len, float* mean_abs, float* max_abs, int* pair_count) {
    float local_max_abs = 0.0f;
    double sum_abs = 0.0;
    int pairs = len >> 1;

    if (iq) {
        for (int n = 0; n < pairs; n++) {
            float i = iq[(size_t)(n << 1) + 0];
            float q = iq[(size_t)(n << 1) + 1];
            float ai = fabsf(i);
            float aq = fabsf(q);
            float peak = (ai > aq) ? ai : aq;
            if (peak > local_max_abs) {
                local_max_abs = peak;
            }
            sum_abs += (double)ai + (double)aq;
        }
    }

    if (mean_abs) {
        *mean_abs = (pairs > 0) ? (float)(sum_abs / (double)pairs) : 0.0f;
    }
    if (max_abs) {
        *max_abs = local_max_abs;
    }
    if (pair_count) {
        *pair_count = pairs;
    }
}

/**
 * @brief On retune/hop, drain audio output ring for a short time to avoid
 * cutting off transmissions. If configured to clear, force-clear instead.
 *
 * Also clears the constellation and eye diagram buffers to prevent stale
 * samples from the previous frequency/SPS from contaminating the display.
 */
static void
retune_output_pending(const struct output_state* outp, size_t* out_ring_used, int* out_cached_symbols) {
    if (out_ring_used) {
        *out_ring_used = outp ? ring_used(outp) : 0U;
    }
    if (out_cached_symbols) {
        *out_cached_symbols = dsd_rtl_stream_metrics_hook_symbol_cache_pending();
    }
}

static int
retune_output_drained(const struct output_state* outp) {
    size_t ring_pending = 0U;
    int cache_pending = 0;
    retune_output_pending(outp, &ring_pending, &cache_pending);
    (void)cache_pending;
    /* The symbol cache belongs to the decoder thread. API/trunking retunes can
     * run on that same thread, so waiting for cached symbols here can only burn
     * the full drain timeout. The generation bump below invalidates stale cache
     * entries before the decoder can consume them. */
    return (ring_pending == 0U) ? 1 : 0;
}

static void
drain_output_on_retune(void) {
    const struct output_state* outp = &output;
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

    /* Clear visualization buffers to avoid mixing old/new frequency samples.
     * This is critical for P25P1->P25P2 transitions where SPS changes from 5 to 4;
     * stale constellation points from the old SPS create the appearance of
     * degraded SNR even when the DSP is performing correctly. */
    constellation_ring_clear();
    eye_ring_clear();
    snr_ema_reset();

    if (force_clear || drain_ms == 0) {
        dsd_rtl_stream_clear_output();
        return;
    }
    size_t before = ring_used(outp);
    int waited_ms = 0;
    while (!retune_output_drained(outp) && waited_ms < drain_ms) {
        dsd_sleep_ms(1);
        waited_ms++;
    }
    if (!retune_output_drained(outp)) {
        /* Timed out; clear remainder to avoid stale backlog */
        dsd_rtl_stream_clear_output();
        return;
    }
    rtl_stream_bump_output_generation();
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

static int
demod_effective_ted_sps(const struct demod_state* s) {
    if (!s) {
        return 5;
    }
    if (s->ted_sps_override > 0) {
        return s->ted_sps_override;
    }
    return (s->ted_sps > 0) ? s->ted_sps : 5;
}

static void
demod_clear_filter_histories(struct demod_state* s) {
    for (int st = 0; st < 10; st++) {
        DSD_MEMSET(s->hb_hist_i[st], 0, sizeof(s->hb_hist_i[st]));
        DSD_MEMSET(s->hb_hist_q[st], 0, sizeof(s->hb_hist_q[st]));
    }
    DSD_MEMSET(s->channel_lpf_hist_i, 0, sizeof(s->channel_lpf_hist_i));
    DSD_MEMSET(s->channel_lpf_hist_q, 0, sizeof(s->channel_lpf_hist_q));
    s->channel_lpf_hist_len = 0;
}

static void
demod_reset_resampler_state(struct demod_state* s) {
    s->resamp_phase = 0;
    s->resamp_hist_head = 0;
    if (s->resamp_hist && s->resamp_taps_per_phase > 0) {
        DSD_MEMSET(s->resamp_hist, 0, (size_t)s->resamp_taps_per_phase * 2U * sizeof(float));
    }
}

static void
demod_reset_for_sps_change(struct demod_state* s) {
    demod_clear_filter_histories(s);
    demod_reset_resampler_state(s);
    ted_init_state(&s->ted_state);
    s->ted_mu = 0.0f;
    s->costas_state.freq = 0.0f;
    s->costas_state.phase = 0.0f;
    s->costas_state.error = 0.0f;
    s->costas_state.error_smooth = 0.0f;
}

static void
demod_handle_sps_transition_reset(struct demod_state* s, DemodRetuneResetReason reason) {
    static int prev_ted_sps = 0;
    if (reason == DemodRetuneResetReason::FreshStream) {
        prev_ted_sps = 0;
    }
    int next_sps = demod_effective_ted_sps(s);
    int sps_changed = (prev_ted_sps > 0 && next_sps != prev_ted_sps);
    if (s->cqpsk_enable && sps_changed) {
        demod_reset_for_sps_change(s);
    }
    prev_ted_sps = next_sps;
}

static void
demod_reset_common_state_for_retune(struct demod_state* s) {
    s->squelch_hits = 0;
    s->squelch_running_power = 0;
    s->squelch_decim_phase = 0;
    s->prev_lpr_index = 0;
    s->now_lpr = 0;
    s->lp_len = 0;
    DSD_MEMSET(s->input_cb_buf, 0, sizeof(s->input_cb_buf));

    fll_init_state(&s->fll_state);
    s->fll_freq = 0.0f;
    s->fll_phase = 0.0f;
    s->fll_prev_r = 0.0f;
    s->fll_prev_j = 0.0f;

    s->fm_demod_history_valid = 0;
    s->pre_r = 0.0f;
    s->pre_j = 0.0f;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    if (s->cqpsk_enable) {
        dsd_cqpsk_cma_equalizer_reset(&s->cqpsk_eq_state, s->cqpsk_eq_taps);
    }
    s->costas_err_avg_q14 = 0;
    s->costas_err_raw_avg_q14 = 0;
    s->costas_conf_avg_q14 = 0;
    s->costas_zero_conf_pct = 0;
    s->costas_state.phase = 0.0f;
    s->costas_state.error = 0.0f;
    s->costas_state.error_smooth = 0.0f;
    if (s->cqpsk_enable) {
        s->costas_state.freq = 0.0f;
    }
}

static void
demod_refresh_fll_band_edge_state(struct demod_state* s, int reset_retained_fll, float retained_fll_scale) {
    int sps = demod_effective_ted_sps(s);
    if (reset_retained_fll) {
        s->fll_band_edge_state.freq = 0.0f;
    } else {
        s->fll_band_edge_state.freq *= retained_fll_scale;
    }
    dsd_fll_band_edge_init(&s->fll_band_edge_state, sps);
    if (reset_retained_fll) {
        return;
    }
    if (s->fll_band_edge_state.freq > s->fll_band_edge_state.max_freq) {
        s->fll_band_edge_state.freq = s->fll_band_edge_state.max_freq;
    } else if (s->fll_band_edge_state.freq < s->fll_band_edge_state.min_freq) {
        s->fll_band_edge_state.freq = s->fll_band_edge_state.min_freq;
    }
}

static void
demod_reset_ted_delay_for_retune(struct demod_state* s) {
    ted_soft_reset(&s->ted_state);
    if (!s->cqpsk_enable) {
        return;
    }
    DSD_MEMSET(s->ted_state.dl, 0, sizeof(s->ted_state.dl));
    s->ted_state.dl_index = 0;
    float mu_init = (float)(s->ted_state.twice_sps + 1);
    s->ted_state.mu = mu_init;
    s->ted_mu = mu_init;
}

static void
demod_apply_pending_ted_override(struct demod_state* s) {
    if (debug_cqpsk_enabled()) {
        DSD_FPRINTF(stderr, "[COSTAS-RESET] pending=%d ted_sps=%d override=%d\n", s->costas_reset_pending, s->ted_sps,
                    s->ted_sps_override);
    }
    if (s->costas_reset_pending) {
        s->costas_state.freq = 0.0f;
        s->costas_state.phase = 0.0f;
        s->costas_state.error = 0.0f;
        s->costas_state.error_smooth = 0.0f;
        s->costas_reset_pending = 0;
    }
    if (s->ted_sps_override > 0 && s->ted_sps != s->ted_sps_override) {
        s->ted_sps = s->ted_sps_override;
    }
}

static void
demod_reset_monitor_state(void) {
    g_monitor_fm_have_prev = 0;
    g_monitor_fm_prev_r = 0.0f;
    g_monitor_fm_prev_j = 0.0f;
}

static void
demod_reset_histories_for_output_mode(struct demod_state* s) {
    if (!(s->cqpsk_enable || s->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK)) {
        return;
    }
    demod_clear_filter_histories(s);
    demod_reset_resampler_state(s);
    if (s->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK) {
        dsd_fsk_modem_reset(&s->fsk_modem_state);
    }
    demod_reset_monitor_state();
}

static void
demod_log_post_retune_state(const struct demod_state* s) {
    if (!debug_cqpsk_enabled()) {
        return;
    }
    float fll_freq_hz = s->fll_band_edge_state.freq * ((float)s->rate_out / 6.28318530717958647692f);
    float costas_freq_hz =
        s->costas_state.freq
        * (((float)s->rate_out / (float)(s->ted_sps > 0 ? s->ted_sps : 5)) / 6.28318530717958647692f);
    DSD_FPRINTF(stderr,
                "[RETUNE] ted_sps=%d override=%d cqpsk=%d fll_freq=%.1fHz fll_phase=%.3f costas_freq=%.1fHz "
                "costas_phase=%.3f gardner_omega=%.3f gardner_mu=%.3f\n",
                s->ted_sps, s->ted_sps_override, s->cqpsk_enable, fll_freq_hz, s->fll_band_edge_state.phase,
                costas_freq_hz, s->costas_state.phase, s->ted_state.omega, s->ted_state.mu);
}

static void
demod_reset_on_retune(struct demod_state* s, const DemodRetuneResetPlan& plan) {
    if (!s) {
        return;
    }
    DemodRetuneResetReason reason = plan.reason;
    const bool reset_retained_fll =
        (reason == DemodRetuneResetReason::PpmCorrection || reason == DemodRetuneResetReason::FreshStream
         || reason == DemodRetuneResetReason::DistantFrequencyRetune);
    demod_handle_sps_transition_reset(s, reason);
    demod_reset_common_state_for_retune(s);
    demod_refresh_fll_band_edge_state(s, reset_retained_fll ? 1 : 0, plan.retained_fll_scale);
    demod_reset_ted_delay_for_retune(s);
    demod_apply_pending_ted_override(s);
    demod_reset_histories_for_output_mode(s);
    demod_log_post_retune_state(s);
}

std::atomic<double> g_snr_c4fm_db{-100.0};
std::atomic<double> g_snr_qpsk_db{-100.0};
std::atomic<double> g_snr_gfsk_db{-100.0};
/* Track recency and source of SNR updates: src 1=direct (symbols), 2=fallback (eye/constellation) */
static std::atomic<long long> g_snr_c4fm_last_ms{0};
static std::atomic<int> g_snr_c4fm_src{0};
static std::atomic<long long> g_snr_qpsk_last_ms{0};
static std::atomic<int> g_snr_qpsk_src{0};
static std::atomic<long long> g_snr_gfsk_last_ms{0};
static std::atomic<int> g_snr_gfsk_src{0};
/* EMA state for direct SNR estimation (reset on retune for fast acquisition).
 * These are atomic because snr_ema_reset() is called from the controller thread
 * while the demod thread reads/writes them during SNR computation. */
static std::atomic<double> g_snr_ema_c4fm{-100.0};
static std::atomic<double> g_snr_ema_qpsk{-100.0};
static std::atomic<double> g_snr_ema_gfsk{-100.0};
/* QPSK accumulator reset flag (actual buffer is in demod loop) */
static std::atomic<int> g_snr_qpsk_acc_reset{0};

static std::atomic<int> g_fsk_metrics_valid{0};
static std::atomic<uint32_t> g_fsk_metrics_generation{0};
static std::atomic<int> g_fsk_metrics_levels{0};
static std::atomic<int> g_fsk_metrics_symbol_rate_hz{0};
static std::atomic<uint64_t> g_fsk_metrics_symbols_total{0};
static std::atomic<unsigned int> g_fsk_metrics_window_symbols{0};
static std::atomic<unsigned int> g_fsk_metrics_mean_reliability{0};
static std::atomic<unsigned int> g_fsk_metrics_min_reliability{0};
static std::atomic<double> g_fsk_metrics_rms_error{0.0};
static std::atomic<double> g_fsk_metrics_evm_snr_db{-100.0};
static std::atomic<double> g_fsk_metrics_low_reliability_pct{0.0};
static std::atomic<double> g_fsk_metrics_clip_pct{0.0};
static std::atomic<int> g_fsk_metrics_timing_acquired{0};
static std::atomic<double> g_fsk_metrics_track_last_error{0.0};
static std::atomic<double> g_fsk_metrics_track_last_score{0.0};
static std::atomic<uint64_t> g_fsk_metrics_track_updates{0};
static std::atomic<uint64_t> g_fsk_metrics_track_skips{0};
static std::atomic<double> g_fsk_metrics_abs_est{0.0};
static std::atomic<double> g_fsk_metrics_dc_est{0.0};
static std::atomic<double> g_fsk_metrics_last_symbol{0.0};

static std::atomic<int> g_decode_health_valid{0};
static std::atomic<uint32_t> g_decode_health_generation{0};
static std::atomic<unsigned int> g_decode_p25p1_fec_ok{0};
static std::atomic<unsigned int> g_decode_p25p1_fec_err{0};
static std::atomic<unsigned int> g_decode_p25p2_facch_ok{0};
static std::atomic<unsigned int> g_decode_p25p2_facch_err{0};
static std::atomic<unsigned int> g_decode_p25p2_sacch_ok{0};
static std::atomic<unsigned int> g_decode_p25p2_sacch_err{0};
static std::atomic<unsigned int> g_decode_p25p2_voice_err{0};

static void
rtl_fsk_metrics_reset_snapshot(void) {
    g_fsk_metrics_valid.store(0, std::memory_order_release);
    g_fsk_metrics_generation.store(g_rtl_output_generation.load(std::memory_order_acquire), std::memory_order_release);
    g_fsk_metrics_levels.store(0, std::memory_order_relaxed);
    g_fsk_metrics_symbol_rate_hz.store(0, std::memory_order_relaxed);
    g_fsk_metrics_symbols_total.store(0, std::memory_order_relaxed);
    g_fsk_metrics_window_symbols.store(0, std::memory_order_relaxed);
    g_fsk_metrics_mean_reliability.store(0, std::memory_order_relaxed);
    g_fsk_metrics_min_reliability.store(0, std::memory_order_relaxed);
    g_fsk_metrics_rms_error.store(0.0, std::memory_order_relaxed);
    g_fsk_metrics_evm_snr_db.store(-100.0, std::memory_order_relaxed);
    g_fsk_metrics_low_reliability_pct.store(0.0, std::memory_order_relaxed);
    g_fsk_metrics_clip_pct.store(0.0, std::memory_order_relaxed);
    g_fsk_metrics_timing_acquired.store(0, std::memory_order_relaxed);
    g_fsk_metrics_track_last_error.store(0.0, std::memory_order_relaxed);
    g_fsk_metrics_track_last_score.store(0.0, std::memory_order_relaxed);
    g_fsk_metrics_track_updates.store(0, std::memory_order_relaxed);
    g_fsk_metrics_track_skips.store(0, std::memory_order_relaxed);
    g_fsk_metrics_abs_est.store(0.0, std::memory_order_relaxed);
    g_fsk_metrics_dc_est.store(0.0, std::memory_order_relaxed);
    g_fsk_metrics_last_symbol.store(0.0, std::memory_order_relaxed);
}

static void
rtl_decode_health_reset(void) {
    g_decode_health_valid.store(0, std::memory_order_release);
    g_decode_health_generation.store(g_rtl_output_generation.load(std::memory_order_acquire),
                                     std::memory_order_release);
    g_decode_p25p1_fec_ok.store(0, std::memory_order_relaxed);
    g_decode_p25p1_fec_err.store(0, std::memory_order_relaxed);
    g_decode_p25p2_facch_ok.store(0, std::memory_order_relaxed);
    g_decode_p25p2_facch_err.store(0, std::memory_order_relaxed);
    g_decode_p25p2_sacch_ok.store(0, std::memory_order_relaxed);
    g_decode_p25p2_sacch_err.store(0, std::memory_order_relaxed);
    g_decode_p25p2_voice_err.store(0, std::memory_order_relaxed);
}

static int
rtl_decode_health_prepare_update(void) {
    if (!rtl_stream_context_active()) {
        rtl_decode_health_reset();
        return 0;
    }
    uint32_t gen = g_rtl_output_generation.load(std::memory_order_acquire);
    if (g_decode_health_generation.load(std::memory_order_acquire) != gen) {
        rtl_decode_health_reset();
        g_decode_health_generation.store(gen, std::memory_order_release);
    }
    return 1;
}

static void
rtl_publish_fsk_metrics_from_demod(const struct demod_state* d) {
    dsd_fsk_modem_metrics m = {};
    if (!d || d->output_kind != DSD_DEMOD_OUTPUT_SYMBOL_FSK || dsd_fsk_modem_get_metrics(&d->fsk_modem_state, &m) != 0
        || !m.valid) {
        rtl_fsk_metrics_reset_snapshot();
        return;
    }

    uint32_t gen = g_rtl_output_generation.load(std::memory_order_acquire);
    g_fsk_metrics_valid.store(0, std::memory_order_release);
    g_fsk_metrics_generation.store(gen, std::memory_order_relaxed);
    g_fsk_metrics_levels.store(m.levels, std::memory_order_relaxed);
    g_fsk_metrics_symbol_rate_hz.store(m.symbol_rate_hz, std::memory_order_relaxed);
    g_fsk_metrics_symbols_total.store(m.symbols_total, std::memory_order_relaxed);
    g_fsk_metrics_window_symbols.store(m.window_symbols, std::memory_order_relaxed);
    g_fsk_metrics_mean_reliability.store(m.mean_reliability, std::memory_order_relaxed);
    g_fsk_metrics_min_reliability.store(m.min_reliability, std::memory_order_relaxed);
    g_fsk_metrics_rms_error.store((double)m.rms_error, std::memory_order_relaxed);
    g_fsk_metrics_evm_snr_db.store((double)m.evm_snr_db, std::memory_order_relaxed);
    g_fsk_metrics_low_reliability_pct.store((double)m.low_reliability_pct, std::memory_order_relaxed);
    g_fsk_metrics_clip_pct.store((double)m.clip_pct, std::memory_order_relaxed);
    g_fsk_metrics_timing_acquired.store(m.timing_acquired, std::memory_order_relaxed);
    g_fsk_metrics_track_last_error.store((double)m.track_last_error, std::memory_order_relaxed);
    g_fsk_metrics_track_last_score.store((double)m.track_last_score, std::memory_order_relaxed);
    g_fsk_metrics_track_updates.store(m.track_updates, std::memory_order_relaxed);
    g_fsk_metrics_track_skips.store(m.track_skips, std::memory_order_relaxed);
    g_fsk_metrics_abs_est.store((double)m.abs_est, std::memory_order_relaxed);
    g_fsk_metrics_dc_est.store((double)m.dc_est, std::memory_order_relaxed);
    g_fsk_metrics_last_symbol.store((double)m.last_symbol, std::memory_order_relaxed);
    g_fsk_metrics_valid.store(1, std::memory_order_release);
}

static void
snr_ema_reset(void) {
    g_snr_ema_c4fm.store(-100.0, std::memory_order_relaxed);
    g_snr_ema_qpsk.store(-100.0, std::memory_order_relaxed);
    g_snr_ema_gfsk.store(-100.0, std::memory_order_relaxed);
    g_snr_c4fm_db.store(-100.0, std::memory_order_relaxed);
    g_snr_qpsk_db.store(-100.0, std::memory_order_relaxed);
    g_snr_gfsk_db.store(-100.0, std::memory_order_relaxed);
    g_snr_c4fm_src.store(0, std::memory_order_relaxed);
    g_snr_qpsk_src.store(0, std::memory_order_relaxed);
    g_snr_gfsk_src.store(0, std::memory_order_relaxed);
    g_snr_qpsk_acc_reset.store(1, std::memory_order_relaxed);
    rtl_fsk_metrics_reset_snapshot();
    rtl_decode_health_reset();
}

namespace {
struct RetuneSettleWindowState {
    uint32_t active_seq;
    int block_index;
    int stable_run;
    float prev_mean_abs;
};
} // namespace

static RetuneSettleWindowState&
retune_settle_window_state(void) {
    static RetuneSettleWindowState state = {};
    return state;
}

static void
retune_settle_reset_window_for_seq(RetuneSettleWindowState* state, uint32_t seq) {
    if (!state) {
        return;
    }
    if (state->active_seq == seq) {
        return;
    }
    state->active_seq = seq;
    state->block_index = 0;
    state->stable_run = 0;
    state->prev_mean_abs = 0.0f;
}

static float
retune_settle_relative_delta(float mean_abs, float prev_mean_abs) {
    if (prev_mean_abs <= 0.0f) {
        return 1.0f;
    }
    float denom = std::max(prev_mean_abs, 0.01f);
    return fabsf(mean_abs - prev_mean_abs) / denom;
}

static void
retune_settle_log_release(uint32_t seq, int block_index, int remaining, float mean_abs, float max_abs, float rel_delta,
                          int stable_run, int timed_out) {
    if (!debug_cqpsk_enabled()) {
        return;
    }
    DSD_FPRINTF(stderr,
                "[RETUNE-SETTLE] seq=%u action=release block=%d remaining=%d reason=%s mean_abs=%.4f "
                "max_abs=%.4f rel_delta=%.4f stable=%d timeout=%d\n",
                seq, block_index, remaining,
                retune_reset_reason_name((DemodRetuneResetReason)g_retune_diag_reason.load(std::memory_order_acquire)),
                mean_abs, max_abs, rel_delta, stable_run, timed_out);
}

static void
retune_settle_log_drop(uint32_t seq, int block_index, int remaining, float mean_abs, float max_abs, float rel_delta,
                       int stable_run) {
    if (!debug_cqpsk_enabled()) {
        return;
    }
    DSD_FPRINTF(stderr,
                "[RETUNE-SETTLE] seq=%u action=drop block=%d remaining=%d reason=%s mean_abs=%.4f max_abs=%.4f "
                "rel_delta=%.4f stable=%d\n",
                seq, block_index, remaining,
                retune_reset_reason_name((DemodRetuneResetReason)g_retune_diag_reason.load(std::memory_order_acquire)),
                mean_abs, max_abs, rel_delta, stable_run);
}

static int
retune_settle_should_discard(const struct demod_state* d, float mean_abs, float max_abs, int pairs) {
    if (!d || !d->cqpsk_enable || pairs <= 0) {
        g_retune_settle_blocks_remaining.store(0, std::memory_order_release);
        return 0;
    }

    int remaining = g_retune_settle_blocks_remaining.load(std::memory_order_acquire);
    if (remaining <= 0) {
        return 0;
    }

    uint32_t seq = g_retune_settle_seq.load(std::memory_order_acquire);
    RetuneSettleWindowState& state = retune_settle_window_state();
    retune_settle_reset_window_for_seq(&state, seq);

    int before = g_retune_settle_blocks_remaining.fetch_sub(1, std::memory_order_acq_rel);
    if (before <= 0) {
        return 0;
    }

    state.block_index++;
    float rel_delta = retune_settle_relative_delta(mean_abs, state.prev_mean_abs);
    bool has_signal = (mean_abs >= kRetuneSettleMinMeanAbs && max_abs >= kRetuneSettleMinMeanAbs);
    bool stable_now = (state.prev_mean_abs > 0.0f && rel_delta <= kRetuneSettleStableRel);
    if (state.block_index >= kRetuneSettleMinBlocks && has_signal && stable_now) {
        state.stable_run++;
    } else {
        state.stable_run = 0;
    }
    state.prev_mean_abs = mean_abs;

    bool timed_out = (before <= 1);
    bool settled = (state.stable_run >= kRetuneSettleStableBlocks);
    if (settled || timed_out) {
        g_retune_settle_blocks_remaining.store(0, std::memory_order_release);
        g_snr_qpsk_acc_reset.store(1, std::memory_order_relaxed);
        retune_settle_log_release(seq, state.block_index, before - 1, mean_abs, max_abs, rel_delta, state.stable_run,
                                  timed_out ? 1 : 0);
        return 0;
    }

    retune_settle_log_drop(seq, state.block_index, before - 1, mean_abs, max_abs, rel_delta, state.stable_run);
    return 1;
}

/* Apply a single gain factor to the final demod block before handing it to consumers. */
static inline void
apply_output_scale(const struct demod_state* d, float* buf, int len) {
    if (!d || !buf || len <= 0) {
        return;
    }
    float s = d->output_scale;
    if (s == 0.0f) {
        return;
    }
    if (s == 1.0f) {
        return;
    }
    for (int i = 0; i < len; i++) {
        buf[i] *= s;
    }
}

/* Fwd decl: eye-based C4FM SNR fallback */
extern "C" double dsd_rtl_stream_estimate_snr_c4fm_eye(void);
/* Fwd decl: QPSK and GFSK fallbacks */
extern "C" double dsd_rtl_stream_estimate_snr_qpsk_const(void);
extern "C" double dsd_rtl_stream_estimate_snr_gfsk_eye(void);

/**
 * @brief Get the current C4FM SNR estimator bias (exposed for UI/external use).
 * @return Bias in dB, computed dynamically based on current DSP settings.
 */
extern "C" double
dsd_rtl_stream_get_snr_bias_c4fm(void) {
    return dsd_snr_bias_c4fm_db(demod.rate_out, demod.ted_sps, demod.channel_lpf_profile);
}

/**
 * @brief Get the current EVM/GFSK/QPSK SNR estimator bias (exposed for UI/external use).
 * @return Bias in dB, computed dynamically based on current DSP settings.
 */
extern "C" double
dsd_rtl_stream_get_snr_bias_evm(void) {
    return dsd_snr_bias_evm_db(demod.rate_out, demod.ted_sps, demod.channel_lpf_profile);
}

/* Fwd decl: spectrum snapshot getter used for spectral SNR gating */
extern "C" int dsd_rtl_stream_spectrum_get(float* out_db, int max_bins, int* out_rate);
extern "C" double dsd_rtl_stream_get_cfo_hz(void);
extern "C" double dsd_rtl_stream_get_residual_cfo_hz(void);
extern "C" int dsd_rtl_stream_get_carrier_lock(void);
/* Tuner autogain runtime get/set (implemented in rtl_sdr_fm.cpp) */
extern "C" int dsd_rtl_stream_get_tuner_autogain(void);
extern "C" void dsd_rtl_stream_set_tuner_autogain(int onoff);

/* Spectrum updater used in demod thread (implemented in rtl_metrics.cpp). */
extern "C" void rtl_metrics_update_spectrum_from_iq(const float* iq_interleaved, int len_interleaved, int out_rate_hz);

static int
demod_enter_processing_block(struct controller_state* s) {
    if (!s || s->retune_in_progress.load(std::memory_order_acquire)) {
        return 0;
    }
    s->demod_processing_active.store(1, std::memory_order_release);
    if (s->retune_in_progress.load(std::memory_order_acquire)) {
        s->demod_processing_active.store(0, std::memory_order_release);
        return 0;
    }
    return 1;
}

static void
demod_leave_processing_block(struct controller_state* s) {
    if (s) {
        s->demod_processing_active.store(0, std::memory_order_release);
    }
}

static void
controller_wait_for_demod_idle(struct controller_state* s) {
    if (!s) {
        return;
    }
    while (s->demod_processing_active.load(std::memory_order_acquire) && !exitflag
           && !(g_stream && g_stream->should_exit.load(std::memory_order_acquire))) {
        dsd_sleep_ms(1);
    }
}

namespace {

struct DemodInputSpan {
    float* ring_p1;
    float* ring_p2;
    size_t ring_n1;
    size_t ring_n2;
    size_t reserved_count;
    int got;
    int direct_input_span;
    float* input_block;
};

struct DemodRetuneDiagBlock {
    int block;
    uint32_t seq;
    uint32_t freq_hz;
    uint32_t reconfigure_seq;
    int reason;
    int pairs;
    float mean_abs;
    float max_abs;
    size_t ring_used;
};

struct DemodAutogainState {
    int initialized;
    int blocks;
    int high;
    int low;
    int manual_target;
    int target_initialized;
    std::chrono::steady_clock::time_point next_allowed;
    std::chrono::steady_clock::time_point hold_until;
    std::chrono::steady_clock::time_point probe_until;
    uint32_t last_freq;
    uint32_t last_reconfigure_seq;
    int probe_ms;
    int seed_gain_db10;
    double spec_snr_db;
    double inband_ratio;
    int up_step_db10;
    int up_persist;
    int spec_pass;
};

struct DemodMetricsState {
    unsigned int dsp_metrics_block;
    int c4fm_missed;
    int qpsk_missed;
    int gfsk_missed;
    float qpsk_acc_i[256];
    float qpsk_acc_q[256];
    int qpsk_acc_n;
};

struct DemodSnrUpdateFlags {
    bool qpsk_updated;
    bool c4fm_updated;
    bool gfsk_updated;
};

} // namespace

static inline int
demod_should_exit_requested(void) {
    return exitflag || (g_stream && g_stream->should_exit.load(std::memory_order_acquire));
}

static int
demod_wait_for_watermark_if_needed(void) {
    if (!g_stream) {
        return 0;
    }
    struct input_ring_watermark* wm = &g_stream->watermark;
    watermark_periodic_adjust(wm, dsd_time_monotonic_ns());
    int control_work_pending = 0;
    while (1) {
        if (g_ring_purge_pending.load(std::memory_order_acquire)
            || controller.retune_in_progress.load(std::memory_order_acquire)) {
            control_work_pending = 1;
            break;
        }
        int was_paused = wm->paused;
        int can_consume = watermark_should_consume(wm, input_ring_used(&input_ring), input_ring.capacity);
        if (can_consume) {
            break;
        }
        if (!was_paused) {
            watermark_on_low_event(wm, dsd_time_monotonic_ns());
        }
        dsd_sleep_ms(5);
        if (demod_should_exit_requested()) {
            break;
        }
        if (g_ring_purge_pending.load(std::memory_order_acquire)
            || controller.retune_in_progress.load(std::memory_order_acquire)) {
            control_work_pending = 1;
            break;
        }
        watermark_periodic_adjust(wm, dsd_time_monotonic_ns());
    }
    if (demod_should_exit_requested() || control_work_pending) {
        return 1;
    }
    return 0;
}

static int
demod_should_pause_before_read(int is_rtltcp_input) {
    if (is_rtltcp_input && !controller.cold_start_ready.load(std::memory_order_acquire)) {
        dsd_sleep_ms(1);
        return 1;
    }
    if (g_ring_purge_pending.exchange(0, std::memory_order_acq_rel)) {
        input_ring_discard_all_consumer(&input_ring);
        replay_note_input_purge_consumed();
        return 1;
    }
    if (controller.retune_in_progress.load(std::memory_order_acquire)) {
        dsd_sleep_ms(1);
        return 1;
    }
    if (demod_wait_for_watermark_if_needed()) {
        return 1;
    }
    return 0;
}

static void
demod_input_span_init(DemodInputSpan* span) {
    if (!span) {
        return;
    }
    span->ring_p1 = NULL;
    span->ring_p2 = NULL;
    span->ring_n1 = 0U;
    span->ring_n2 = 0U;
    span->reserved_count = 0U;
    span->got = 0;
    span->direct_input_span = 0;
    span->input_block = NULL;
}

static void
demod_input_span_commit_direct(DemodInputSpan* span) {
    if (!span || !span->direct_input_span) {
        return;
    }
    input_ring_read_commit(&input_ring, span->reserved_count);
    span->reserved_count = 0U;
    span->direct_input_span = 0;
}

static void
demod_input_span_commit_reserved(DemodInputSpan* span) {
    if (!span || span->reserved_count == 0U) {
        return;
    }
    input_ring_read_commit(&input_ring, span->reserved_count);
    span->reserved_count = 0U;
    span->direct_input_span = 0;
}

static void
demod_input_span_stabilize_direct_lowpassed(struct demod_state* d, const DemodInputSpan* span) {
    if (!d || !span || !span->direct_input_span || !span->ring_p1 || !d->lowpassed || d->lp_len <= 0) {
        return;
    }
    uintptr_t lp_addr = (uintptr_t)d->lowpassed;
    uintptr_t span_begin = (uintptr_t)span->ring_p1;
    uintptr_t span_end = span_begin + span->ring_n1 * sizeof(float);
    if (lp_addr < span_begin || lp_addr >= span_end) {
        return;
    }
    int stable_len = d->lp_len;
    if (stable_len > span->got) {
        stable_len = span->got;
    }
    if (stable_len > MAXIMUM_BUF_LENGTH) {
        stable_len = MAXIMUM_BUF_LENGTH;
    }
    DSD_MEMCPY(d->input_cb_buf, d->lowpassed, (size_t)stable_len * sizeof(float));
    d->lowpassed = d->input_cb_buf;
    d->lp_len = stable_len;
}

static void
demod_input_span_release_direct(struct demod_state* d, DemodInputSpan* span) {
    demod_input_span_stabilize_direct_lowpassed(d, span);
    demod_input_span_commit_direct(span);
}

static int
demod_copy_wrapped_input_block(struct demod_state* d, const DemodInputSpan* span) {
    if (!d || !span) {
        return 0;
    }
    size_t copied = 0;
    if (span->ring_p1 && span->ring_n1 > 0) {
        DSD_MEMCPY(d->input_cb_buf, span->ring_p1, span->ring_n1 * sizeof(float));
        copied += span->ring_n1;
    }
    if (span->ring_p2 && span->ring_n2 > 0 && copied < static_cast<size_t>(MAXIMUM_BUF_LENGTH)) {
        size_t room = static_cast<size_t>(MAXIMUM_BUF_LENGTH) - copied;
        size_t n2 = (span->ring_n2 < room) ? span->ring_n2 : room;
        DSD_MEMCPY(d->input_cb_buf + copied, span->ring_p2, n2 * sizeof(float));
        copied += n2;
    }
    return (int)copied;
}

static int
demod_read_input_block(const struct demod_state* d, DemodInputSpan* span) {
    demod_input_span_init(span);
    if (!d || !span) {
        return 0;
    }
    span->got = input_ring_read_reserve(&input_ring, static_cast<size_t>(MAXIMUM_BUF_LENGTH), &span->ring_p1,
                                        &span->ring_n1, &span->ring_p2, &span->ring_n2);
    if (span->got <= 0) {
        return 0;
    }
    span->direct_input_span =
        (span->ring_p1 && span->ring_n1 == (size_t)span->got && (!span->ring_p2 || span->ring_n2 == 0)) ? 1 : 0;
    span->reserved_count = (size_t)span->got;
    return 1;
}

static int
demod_prepare_input_block(struct demod_state* d, DemodInputSpan* span) {
    if (!d || !span) {
        return 0;
    }
    if (!span->direct_input_span) {
        int copied = demod_copy_wrapped_input_block(d, span);
        demod_input_span_commit_reserved(span);
        span->got = copied;
        if (span->got <= 0) {
            return 0;
        }
    }
    span->input_block = span->direct_input_span ? span->ring_p1 : d->input_cb_buf;
    return 1;
}

static DemodRetuneDiagBlock
demod_capture_retune_diag(float input_mean_abs, float input_max_abs, int input_pairs) {
    DemodRetuneDiagBlock diag = {};
    if (!debug_cqpsk_enabled()) {
        return diag;
    }
    int remaining = g_retune_diag_blocks_remaining.load(std::memory_order_acquire);
    if (remaining <= 0) {
        return diag;
    }
    int before = g_retune_diag_blocks_remaining.fetch_sub(1, std::memory_order_acq_rel);
    if (before <= 0) {
        return diag;
    }
    diag.block = kRetuneDiagBlocks - before + 1;
    diag.seq = g_retune_diag_seq.load(std::memory_order_acquire);
    diag.freq_hz = g_retune_diag_freq_hz.load(std::memory_order_acquire);
    diag.reason = g_retune_diag_reason.load(std::memory_order_acquire);
    diag.reconfigure_seq = controller.reconfigure_seq.load(std::memory_order_acquire);
    diag.ring_used = input_ring_used(&input_ring);
    diag.mean_abs = input_mean_abs;
    diag.max_abs = input_max_abs;
    diag.pairs = input_pairs;
    return diag;
}

static void
demod_log_retune_diag_block(const struct demod_state* d, int got, const DemodRetuneDiagBlock* diag) {
    if (!d || !diag || diag->block <= 0) {
        return;
    }
    float fll_freq_hz = d->fll_band_edge_state.freq * ((float)d->rate_out / 6.28318530717958647692f);
    float symbol_rate_hz = (float)d->rate_out / (float)(d->ted_sps > 0 ? d->ted_sps : 5);
    float costas_freq_hz = d->costas_state.freq * (symbol_rate_hz / 6.28318530717958647692f);
    double snr_qpsk = g_snr_qpsk_db.load(std::memory_order_relaxed);
    DSD_FPRINTF(stderr,
                "[RETUNE-BLOCK] seq=%u block=%d freq=%u reason=%s reconfig=%u ring=%zu got=%d pairs=%d "
                "mean_abs=%.4f max_abs=%.4f snr=%.1f fll=%.1fHz costas=%.1fHz costas_err=%.4f "
                "ted_lock=%d ted_err=%.4f ted_mu=%.3f ted_omega=%.3f carrier_lock=%d\n",
                diag->seq, diag->block, diag->freq_hz, retune_reset_reason_name((DemodRetuneResetReason)diag->reason),
                diag->reconfigure_seq, diag->ring_used, got, diag->pairs, diag->mean_abs, diag->max_abs, snr_qpsk,
                fll_freq_hz, costas_freq_hz, d->costas_state.error, d->ted_state.lock_count, d->ted_state.e_ema,
                d->ted_state.mu, d->ted_state.omega, dsd_rtl_stream_get_carrier_lock());
}

static DemodAutogainState&
demod_autogain_state(void) {
    static DemodAutogainState state = {0,
                                       0,
                                       0,
                                       0,
                                       180,
                                       0,
                                       std::chrono::steady_clock::now(),
                                       std::chrono::steady_clock::time_point{},
                                       std::chrono::steady_clock::time_point{},
                                       0U,
                                       0U,
                                       3000,
                                       300,
                                       6.0,
                                       0.60,
                                       30,
                                       2,
                                       0};
    return state;
}

static inline int
demod_autogain_clamp_db10(int gain_db10) {
    if (gain_db10 < 0) {
        return 0;
    }
    if (gain_db10 > 490) {
        return 490;
    }
    return gain_db10;
}

static void
demod_autogain_reset_window(DemodAutogainState* st, uint32_t current_freq_hz, uint32_t current_reconfigure_seq) {
    if (!st) {
        return;
    }
    st->last_freq = current_freq_hz;
    st->last_reconfigure_seq = current_reconfigure_seq;
    st->blocks = st->high = st->low = 0;
    st->hold_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    if (st->probe_ms > 0) {
        st->probe_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(st->probe_ms);
    } else {
        st->probe_until = std::chrono::steady_clock::time_point{};
    }
    st->spec_pass = 0;
}

static void
demod_autogain_init_once(DemodAutogainState* st) {
    if (!st || st->initialized) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg) {
        g_tuner_autogain_on.store(cfg->tuner_autogain_enable ? 1 : 0, std::memory_order_relaxed);
        st->probe_ms = cfg->tuner_autogain_probe_ms;
        st->seed_gain_db10 = (int)lrint(cfg->tuner_autogain_seed_db * 10.0);
        st->spec_snr_db = cfg->tuner_autogain_spec_snr_db;
        st->inband_ratio = cfg->tuner_autogain_inband_ratio;
        st->up_step_db10 = (int)lrint(cfg->tuner_autogain_up_step_db * 10.0);
        st->up_persist = cfg->tuner_autogain_up_persist;
    }
    st->initialized = 1;
}

static void
demod_autogain_latch_manual_target(DemodAutogainState* st) {
    if (!st || st->target_initialized) {
        return;
    }
    int cg = rtl_device_get_tuner_gain(rtl_device_handle);
    int is_auto_boot = rtl_device_is_auto_gain(rtl_device_handle);
    if (!is_auto_boot && cg >= 0) {
        st->manual_target = cg;
    }
    st->target_initialized = 1;
}

static void
demod_autogain_find_peak(const float* spec_db, int n, float* p_max, int* i_max) {
    if (!spec_db || n <= 0 || !p_max || !i_max) {
        return;
    }
    *p_max = -1e30f;
    *i_max = 0;
    for (int i = 0; i < n; i++) {
        float v = spec_db[i];
        if (v > *p_max) {
            *p_max = v;
            *i_max = i;
        }
    }
}

static float
demod_autogain_noise_median_db(const float* spec_db, int n) {
    float tmp_med[1024];
    for (int i = 0; i < n; i++) {
        tmp_med[i] = spec_db[i];
    }
    int mid = n / 2;
    std::nth_element(tmp_med, tmp_med + mid, tmp_med + n);
    return tmp_med[mid];
}

static int
demod_autogain_is_dc_spur(const float* spec_db, int n, int i_max, float p_max) {
    int k_center = n / 2;
    if (i_max != k_center || i_max <= 0 || i_max + 1 >= n) {
        return 0;
    }
    float l = spec_db[i_max - 1];
    float r = spec_db[i_max + 1];
    float side_max = (l > r) ? l : r;
    return ((p_max - side_max) > 12.0f) ? 1 : 0;
}

static double
demod_autogain_center_ratio(const float* spec_db, int n, int* out_i0, int* out_i1) {
    int k_center = n / 2;
    int half = n / 8;
    if (half < 2) {
        half = 2;
    }
    int i0 = k_center - half;
    int i1 = k_center + half;
    if (i0 < 0) {
        i0 = 0;
    }
    if (i1 > (n - 1)) {
        i1 = n - 1;
    }
    if (out_i0) {
        *out_i0 = i0;
    }
    if (out_i1) {
        *out_i1 = i1;
    }
    double sum_all = 0.0;
    double sum_center = 0.0;
    for (int i = 0; i < n; i++) {
        double p = pow(10.0, (double)spec_db[i] / 10.0);
        sum_all += p;
        if (i >= i0 && i <= i1) {
            sum_center += p;
        }
    }
    return (sum_all > 0.0) ? (sum_center / sum_all) : 0.0;
}

static int
demod_autogain_spectral_gate_ok(DemodAutogainState* st, const struct demod_state* d) {
    if (!st || !d || !d->squelch_gate_open) {
        if (st) {
            st->spec_pass = 0;
        }
        return 0;
    }
    float spec_db[1024];
    int rate_hz = 0;
    int n = dsd_rtl_stream_spectrum_get(spec_db, 1024, &rate_hz);
    if (n < 64 || n > 1024) {
        st->spec_pass = 0;
        return 0;
    }
    float p_max = -1e30f;
    int i_max = 0;
    demod_autogain_find_peak(spec_db, n, &p_max, &i_max);
    float noise_med_db = demod_autogain_noise_median_db(spec_db, n);
    float spec_snr_db = p_max - noise_med_db;
    int i0 = 0;
    int i1 = 0;
    double ratio_center = demod_autogain_center_ratio(spec_db, n, &i0, &i1);
    int dc_spur = demod_autogain_is_dc_spur(spec_db, n, i_max, p_max);
    int peak_in_center = (i_max >= i0 && i_max <= i1) ? 1 : 0;
    int gate = (!dc_spur) && peak_in_center && (spec_snr_db >= st->spec_snr_db) && (ratio_center >= st->inband_ratio);
    st->spec_pass = gate ? (st->spec_pass + 1) : 0;
    return (st->spec_pass >= st->up_persist) ? 1 : 0;
}

static void
demod_autogain_probe_handle(DemodAutogainState* st, const std::chrono::steady_clock::time_point& now) {
    if (!st || st->high < 3) {
        return;
    }
    int seed = demod_autogain_clamp_db10(st->seed_gain_db10);
    st->manual_target = demod_autogain_clamp_db10(seed - 50);
    rtl_device_set_gain_nearest(rtl_device_handle, st->manual_target);
    dongle.gain = st->manual_target;
    st->next_allowed = now + std::chrono::milliseconds(1500);
    LOG_INFO("AUTOGAIN: exiting probe due to clipping; set ~%d.%d dB.\n", st->manual_target / 10,
             st->manual_target % 10);
}

static void
demod_autogain_bootstrap_if_low(DemodAutogainState* st, int is_auto, const std::chrono::steady_clock::time_point& now) {
    if (!st || is_auto <= 0 || st->high != 0 || st->low < (st->blocks * 3) / 4) {
        return;
    }
    int kick = demod_autogain_clamp_db10(st->seed_gain_db10);
    rtl_device_set_gain_nearest(rtl_device_handle, kick);
    dongle.gain = kick;
    st->manual_target = kick;
    st->next_allowed = now + std::chrono::milliseconds(1500);
    LOG_INFO("AUTOGAIN: bootstrapping from device auto to ~%d.%d dB due to low input level.\n", kick / 10, kick % 10);
}

static void
demod_autogain_adjust_manual(DemodAutogainState* st, const struct demod_state* d,
                             const std::chrono::steady_clock::time_point& now) {
    if (!st || !d) {
        return;
    }
    int is_auto = rtl_device_is_auto_gain(rtl_device_handle);
    bool changed = false;
    demod_autogain_bootstrap_if_low(st, is_auto, now);
    if (st->high >= 3) {
        st->manual_target = demod_autogain_clamp_db10(st->manual_target - 50);
        changed = true;
    } else if (demod_autogain_spectral_gate_ok(st, d) && st->low >= (st->blocks * 3) / 4) {
        st->manual_target = demod_autogain_clamp_db10(st->manual_target + st->up_step_db10);
        changed = true;
    }
    if (!changed) {
        return;
    }
    rtl_device_set_gain_nearest(rtl_device_handle, st->manual_target);
    dongle.gain = st->manual_target;
    st->next_allowed = now + std::chrono::milliseconds(1500);
    st->spec_pass = 0;
    if (is_auto > 0) {
        LOG_INFO("AUTOGAIN: threshold hit; exiting device auto and setting ~%d.%d dB.\n", st->manual_target / 10,
                 st->manual_target % 10);
    } else {
        LOG_INFO("AUTOGAIN: adjusting manual gain to ~%d.%d dB.\n", st->manual_target / 10, st->manual_target % 10);
    }
}

static void
demod_autogain_maybe_adjust(DemodAutogainState* st, const struct demod_state* d) {
    if (!st || !d || st->blocks < 40) {
        return;
    }
    demod_autogain_latch_manual_target(st);
    auto now = std::chrono::steady_clock::now();
    bool in_hold = (st->hold_until.time_since_epoch().count() != 0) && (now < st->hold_until);
    bool in_probe = (st->probe_until.time_since_epoch().count() != 0) && (now < st->probe_until)
                    && (rtl_device_is_auto_gain(rtl_device_handle) > 0);
    bool throttled = now < st->next_allowed;
    if (!in_hold && !throttled) {
        if (in_probe) {
            demod_autogain_probe_handle(st, now);
        } else {
            demod_autogain_adjust_manual(st, d, now);
        }
    }
    st->blocks = st->high = st->low = 0;
}

static void
demod_autogain_update(struct demod_state* d, float input_mean_abs, float input_max_abs) {
    DemodAutogainState& st = demod_autogain_state();
    demod_autogain_init_once(&st);
    if (!g_tuner_autogain_on.load(std::memory_order_relaxed)) {
        return;
    }
    uint32_t current_freq_hz = load_dongle_frequency();
    uint32_t current_reconfigure_seq = controller.reconfigure_seq.load(std::memory_order_acquire);
    if (st.last_freq != current_freq_hz || st.last_reconfigure_seq != current_reconfigure_seq) {
        demod_autogain_reset_window(&st, current_freq_hz, current_reconfigure_seq);
    }
    st.blocks++;
    if (input_max_abs > 0.9f) {
        st.high++;
    }
    if (input_mean_abs < 0.06f) {
        st.low++;
    }
    demod_autogain_maybe_adjust(&st, d);
}

static long long
demod_now_ms(void) {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static DemodMetricsState&
demod_metrics_state(void) {
    static DemodMetricsState state = {};
    return state;
}

static int
demod_metrics_due_for_block(DemodMetricsState* st, const struct demod_state* d) {
    if (!st || !d) {
        return 0;
    }
    const int rtl_symbol_output =
        (d->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK || d->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK);
    return (!rtl_symbol_output || ((++st->dsp_metrics_block & 1U) == 0U)) ? 1 : 0;
}

static void
demod_metrics_capture_views(const struct demod_state* d) {
    if (!d) {
        return;
    }
    constellation_ring_append(d->lowpassed, d->lp_len, d->cqpsk_enable ? 1 : d->ted_sps);
    eye_ring_append_i_chan(d->lowpassed, d->lp_len);
    rtl_metrics_update_spectrum_from_iq(d->lowpassed, d->lp_len, d->rate_out);
}

static void
demod_snr_qpsk_accumulate(const struct demod_state* d, const float* iq, int pairs, int sps, int mid, int win,
                          DemodMetricsState* st) {
    if (!d || !iq || !st) {
        return;
    }
    for (int k = 0; k < pairs && st->qpsk_acc_n < 256; k++) {
        int phase = k % sps;
        if (d->cqpsk_enable || (phase >= mid - win && phase <= mid + win)) {
            st->qpsk_acc_i[st->qpsk_acc_n] = iq[2 * k + 0];
            st->qpsk_acc_q[st->qpsk_acc_n] = iq[2 * k + 1];
            st->qpsk_acc_n++;
        }
    }
}

static int
demod_snr_qpsk_axis_levels(const DemodMetricsState* st, int m, double* out_ai, double* out_aq) {
    if (!st || m <= 0 || !out_ai || !out_aq) {
        return 0;
    }
    double sum_abs_i = 0.0;
    double sum_abs_q = 0.0;
    for (int i = 0; i < m; i++) {
        sum_abs_i += fabs((double)st->qpsk_acc_i[i]);
        sum_abs_q += fabs((double)st->qpsk_acc_q[i]);
    }
    *out_ai = sum_abs_i / (double)m;
    *out_aq = sum_abs_q / (double)m;
    return (*out_ai > 1e-9 && *out_aq > 1e-9) ? 1 : 0;
}

static int
demod_snr_qpsk_error_ratio(const DemodMetricsState* st, int m, double aI, double aQ, double* out_ratio) {
    if (!st || m <= 0 || !out_ratio) {
        return 0;
    }
    double e2 = 0.0;
    for (int i = 0; i < m; i++) {
        double I = (double)st->qpsk_acc_i[i];
        double Q = (double)st->qpsk_acc_q[i];
        double ti = (I >= 0.0) ? aI : -aI;
        double tq = (Q >= 0.0) ? aQ : -aQ;
        double ei = I - ti;
        double eq = Q - tq;
        e2 += ei * ei + eq * eq;
    }
    double t2 = (double)m * (aI * aI + aQ * aQ);
    if (e2 <= 1e-12 || t2 <= 1e-9) {
        return 0;
    }
    *out_ratio = t2 / e2;
    return 1;
}

static void
demod_snr_qpsk_publish(const struct demod_state* d, double ratio, DemodSnrUpdateFlags* flags) {
    if (!d || !flags) {
        return;
    }
    double snr_raw = 10.0 * log10(ratio);
    double bias = dsd_snr_bias_evm_db(d->rate_out, d->ted_sps, d->channel_lpf_profile);
    double snr = snr_raw - bias;
    double ema = g_snr_ema_qpsk.load(std::memory_order_relaxed);
    ema = (ema < -50.0) ? snr : (0.5 * ema + 0.5 * snr);
    g_snr_ema_qpsk.store(ema, std::memory_order_relaxed);
    g_snr_qpsk_db.store(ema, std::memory_order_relaxed);
    g_snr_qpsk_src.store(1, std::memory_order_relaxed);
    g_snr_qpsk_last_ms.store(demod_now_ms(), std::memory_order_relaxed);
    flags->qpsk_updated = true;
}

static void
demod_snr_update_qpsk_direct(const struct demod_state* d, const float* iq, int pairs, int sps, int mid, int win,
                             DemodMetricsState* st, DemodSnrUpdateFlags* flags) {
    if (!d || !iq || !st || !flags || sps < 2 || sps > 12) {
        return;
    }
    if (g_snr_qpsk_acc_reset.exchange(0, std::memory_order_relaxed)) {
        st->qpsk_acc_n = 0;
    }
    demod_snr_qpsk_accumulate(d, iq, pairs, sps, mid, win, st);
    int m = st->qpsk_acc_n;
    if (m < 64) {
        return;
    }
    double aI = 0.0;
    double aQ = 0.0;
    double ratio = 0.0;
    if (demod_snr_qpsk_axis_levels(st, m, &aI, &aQ) && demod_snr_qpsk_error_ratio(st, m, aI, aQ, &ratio)) {
        demod_snr_qpsk_publish(d, ratio, flags);
    }
    st->qpsk_acc_n = 0;
}

static int
demod_snr_collect_fsk_vals(const float* iq, int pairs, int sps, int mid, int win, float* vals, int max_vals) {
    if (!iq || !vals || max_vals <= 0 || sps < 4 || sps > 12) {
        return 0;
    }
    int m = 0;
    for (int k = 0; k < pairs && m < max_vals; k++) {
        int phase = k % sps;
        if (phase >= mid - win && phase <= mid + win) {
            vals[m++] = iq[2 * k + 0];
        }
    }
    return m;
}

static int
demod_snr_compute_quartiles(float* vals, int m, float* q1, float* q2, float* q3) {
    if (!vals || m <= 32 || !q1 || !q2 || !q3) {
        return 0;
    }
    int idx1 = (int)((size_t)m / 4);
    int idx2 = (int)((size_t)m / 2);
    int idx3 = (int)((size_t)(3 * (size_t)m) / 4);
    std::nth_element(vals, vals + idx2, vals + m);
    *q2 = vals[idx2];
    std::nth_element(vals, vals + idx1, vals + idx2);
    *q1 = vals[idx1];
    std::nth_element(vals + idx2 + 1, vals + idx3, vals + m);
    *q3 = vals[idx3];
    return 1;
}

static void
demod_snr_c4fm_bucket_stats(const float* vals, int m, float q1, float q2, float q3, double* sum, int* cnt) {
    if (!vals || m <= 0 || !sum || !cnt) {
        return;
    }
    for (int i = 0; i < m; i++) {
        float v = vals[i];
        int b = (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
        sum[b] += v;
        cnt[b]++;
    }
}

static int
demod_snr_c4fm_cluster_means(const double* sum, const int* cnt, double* mu, int* total) {
    if (!sum || !cnt || !mu || !total) {
        return 0;
    }
    if (!(cnt[0] && cnt[1] && cnt[2] && cnt[3])) {
        return 0;
    }
    *total = 0;
    for (int b = 0; b < 4; b++) {
        mu[b] = sum[b] / (double)cnt[b];
        *total += cnt[b];
    }
    return 1;
}

static double
demod_snr_c4fm_noise_var(const float* vals, int m, float q1, float q2, float q3, const double* mu, int total) {
    double nsum = 0.0;
    for (int i = 0; i < m; i++) {
        float v = vals[i];
        int b = (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
        double e = (double)v - mu[b];
        nsum += e * e;
    }
    return nsum / (double)total;
}

static double
demod_snr_c4fm_signal_var(const double* mu, const int* cnt, int total) {
    double mu_all = 0.0;
    for (int b = 0; b < 4; b++) {
        mu_all += mu[b] * (double)cnt[b] / (double)total;
    }
    double ssum = 0.0;
    for (int b = 0; b < 4; b++) {
        double dv = mu[b] - mu_all;
        ssum += (double)cnt[b] * dv * dv;
    }
    return ssum / (double)total;
}

static void
demod_snr_update_c4fm_direct(const struct demod_state* d, const float* vals, int m, float q1, float q2, float q3,
                             DemodSnrUpdateFlags* flags) {
    if (!d || !vals || m <= 0 || !flags) {
        return;
    }
    double sum[4] = {0.0, 0.0, 0.0, 0.0};
    int cnt[4] = {0, 0, 0, 0};
    demod_snr_c4fm_bucket_stats(vals, m, q1, q2, q3, sum, cnt);
    double mu[4];
    int total = 0;
    if (!demod_snr_c4fm_cluster_means(sum, cnt, mu, &total)) {
        return;
    }
    double noise_var = demod_snr_c4fm_noise_var(vals, m, q1, q2, q3, mu, total);
    if (noise_var <= 1e-9) {
        return;
    }
    double sig_var = demod_snr_c4fm_signal_var(mu, cnt, total);
    if (sig_var <= 1e-9) {
        return;
    }
    double bias = dsd_snr_bias_c4fm_db(d->rate_out, d->ted_sps, d->channel_lpf_profile);
    double snr = 10.0 * log10(sig_var / noise_var) - bias;
    double ema = g_snr_ema_c4fm.load(std::memory_order_relaxed);
    ema = (ema < -50.0) ? snr : (0.5 * ema + 0.5 * snr);
    g_snr_ema_c4fm.store(ema, std::memory_order_relaxed);
    g_snr_c4fm_db.store(ema, std::memory_order_relaxed);
    g_snr_c4fm_src.store(1, std::memory_order_relaxed);
    g_snr_c4fm_last_ms.store(demod_now_ms(), std::memory_order_relaxed);
    flags->c4fm_updated = true;
}

static void
demod_snr_update_gfsk_direct(const struct demod_state* d, const float* vals, int m, float q2,
                             DemodSnrUpdateFlags* flags) {
    if (!d || !vals || m <= 0 || !flags) {
        return;
    }
    double sumL = 0.0;
    double sumH = 0.0;
    int cntL = 0;
    int cntH = 0;
    for (int i = 0; i < m; i++) {
        float v = vals[i];
        if (v <= q2) {
            sumL += v;
            cntL++;
        } else {
            sumH += v;
            cntH++;
        }
    }
    if (cntL <= 0 || cntH <= 0) {
        return;
    }
    double muL = sumL / (double)cntL;
    double muH = sumH / (double)cntH;
    int total = cntL + cntH;
    double nsum = 0.0;
    for (int i = 0; i < m; i++) {
        float v = vals[i];
        double mu = (v <= q2) ? muL : muH;
        double e = (double)v - mu;
        nsum += e * e;
    }
    double noise_var = nsum / (double)total;
    if (noise_var <= 1e-9) {
        return;
    }
    double mu_all = (muL * (double)cntL + muH * (double)cntH) / (double)total;
    double ssum = (double)cntL * (muL - mu_all) * (muL - mu_all) + (double)cntH * (muH - mu_all) * (muH - mu_all);
    double sig_var = ssum / (double)total;
    if (sig_var <= 1e-9) {
        return;
    }
    double bias = dsd_snr_bias_evm_db(d->rate_out, d->ted_sps, d->channel_lpf_profile);
    double snr = 10.0 * log10(sig_var / noise_var) - bias;
    double ema = g_snr_ema_gfsk.load(std::memory_order_relaxed);
    ema = (ema < -50.0) ? snr : (0.5 * ema + 0.5 * snr);
    g_snr_ema_gfsk.store(ema, std::memory_order_relaxed);
    g_snr_gfsk_db.store(ema, std::memory_order_relaxed);
    g_snr_gfsk_src.store(1, std::memory_order_relaxed);
    g_snr_gfsk_last_ms.store(demod_now_ms(), std::memory_order_relaxed);
    flags->gfsk_updated = true;
}

static void
demod_snr_update_fsk_direct(const struct demod_state* d, const float* iq, int pairs, int sps, int mid, int win,
                            DemodSnrUpdateFlags* flags) {
    if (!d || !iq || !flags) {
        return;
    }

    enum : uint16_t { MAXS = 8192 };

    static float vals[(size_t)MAXS];
    int m = demod_snr_collect_fsk_vals(iq, pairs, sps, mid, win, vals, MAXS);
    if (m <= 32) {
        return;
    }
    float q1 = 0.0f;
    float q2 = 0.0f;
    float q3 = 0.0f;
    if (!demod_snr_compute_quartiles(vals, m, &q1, &q2, &q3)) {
        return;
    }
    demod_snr_update_c4fm_direct(d, vals, m, q1, q2, q3, flags);
    demod_snr_update_gfsk_direct(d, vals, m, q2, flags);
}

static void
demod_snr_fallback_c4fm(DemodMetricsState* st, const DemodSnrUpdateFlags* flags) {
    if (!st || !flags) {
        return;
    }
    if (flags->c4fm_updated) {
        st->c4fm_missed = 0;
        return;
    }
    if (++st->c4fm_missed < 50) {
        return;
    }
    double fb = dsd_rtl_stream_estimate_snr_c4fm_eye();
    if (fb > -50.0) {
        double prev = g_snr_c4fm_db.load(std::memory_order_relaxed);
        double blended = (prev < -50.0) ? fb : (0.8 * prev + 0.2 * fb);
        g_snr_c4fm_db.store(blended, std::memory_order_relaxed);
        g_snr_c4fm_src.store(2, std::memory_order_relaxed);
        g_snr_c4fm_last_ms.store(demod_now_ms(), std::memory_order_relaxed);
    }
    st->c4fm_missed = 0;
}

static void
demod_snr_fallback_qpsk(DemodMetricsState* st, const DemodSnrUpdateFlags* flags) {
    if (!st || !flags) {
        return;
    }
    if (flags->qpsk_updated) {
        st->qpsk_missed = 0;
        return;
    }
    if (++st->qpsk_missed < 10) {
        return;
    }
    double fb = dsd_rtl_stream_estimate_snr_qpsk_const();
    if (fb > -50.0) {
        double prev = g_snr_qpsk_db.load(std::memory_order_relaxed);
        double alpha = (prev < -50.0) ? 1.0 : 0.5;
        double blended = alpha * fb + (1.0 - alpha) * prev;
        g_snr_qpsk_db.store(blended, std::memory_order_relaxed);
        g_snr_qpsk_src.store(2, std::memory_order_relaxed);
        g_snr_qpsk_last_ms.store(demod_now_ms(), std::memory_order_relaxed);
    }
    st->qpsk_missed = 0;
}

static void
demod_snr_fallback_gfsk(DemodMetricsState* st, const DemodSnrUpdateFlags* flags) {
    if (!st || !flags) {
        return;
    }
    if (flags->gfsk_updated) {
        st->gfsk_missed = 0;
        return;
    }
    if (++st->gfsk_missed < 50) {
        return;
    }
    double fb = dsd_rtl_stream_estimate_snr_gfsk_eye();
    if (fb > -50.0) {
        double prev = g_snr_gfsk_db.load(std::memory_order_relaxed);
        double blended = (prev < -50.0) ? fb : (0.8 * prev + 0.2 * fb);
        g_snr_gfsk_db.store(blended, std::memory_order_relaxed);
        g_snr_gfsk_src.store(2, std::memory_order_relaxed);
        g_snr_gfsk_last_ms.store(demod_now_ms(), std::memory_order_relaxed);
    }
    st->gfsk_missed = 0;
}

static void
demod_metrics_update_snr(const struct demod_state* d, DemodMetricsState* st) {
    if (!d || !st) {
        return;
    }
    DemodSnrUpdateFlags flags = {false, false, false};
    const float* iq = d->lowpassed;
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
        demod_snr_update_qpsk_direct(d, iq, pairs, sps, mid, win, st, &flags);
        if (sps >= 4 && sps <= 12) {
            demod_snr_update_fsk_direct(d, iq, pairs, sps, mid, win, &flags);
        }
    }
    demod_snr_fallback_c4fm(st, &flags);
    demod_snr_fallback_qpsk(st, &flags);
    demod_snr_fallback_gfsk(st, &flags);
}

static uint64_t
demod_metrics_process(struct demod_state* d, int perf_on) {
    DemodMetricsState& st = demod_metrics_state();
    if (!demod_metrics_due_for_block(&st, d)) {
        return 0ULL;
    }
    uint64_t perf_metrics_start_ns = perf_on ? rtl_perf_now_ns() : 0ULL;
    demod_metrics_capture_views(d);
    demod_metrics_update_snr(d, &st);
    if (!perf_on) {
        return 0ULL;
    }
    return rtl_perf_now_ns() - perf_metrics_start_ns;
}

static void
demod_update_replay_drain_state(int replay_active, uint64_t consumed_gen) {
    if (!replay_active || !g_stream) {
        return;
    }
    g_stream->replay_last_consume_gen.store(consumed_gen, std::memory_order_release);
    if (g_stream->replay_demod_drained.load(std::memory_order_acquire)) {
        return;
    }
    int input_drained = g_stream->replay_input_drained.load(std::memory_order_acquire);
    if (!input_drained && g_stream->replay_input_eof.load(std::memory_order_acquire)
        && input_ring_used(&input_ring) == 0U) {
        g_stream->replay_input_drained.store(1, std::memory_order_release);
        input_drained = 1;
        if (g_stream->replay_eof_sync_inited) {
            dsd_mutex_lock(&g_stream->replay_eof_m);
            dsd_cond_broadcast(&g_stream->replay_eof_cond);
            dsd_mutex_unlock(&g_stream->replay_eof_m);
        }
    }
    uint64_t consumed_at = g_stream->replay_last_consume_gen.load(std::memory_order_acquire);
    uint64_t eof_gen = g_stream->replay_last_submit_gen_at_eof.load(std::memory_order_acquire);
    if (input_drained && consumed_at >= eof_gen) {
        g_stream->replay_demod_drained.store(1, std::memory_order_release);
        if (g_stream->replay_eof_sync_inited) {
            dsd_mutex_lock(&g_stream->replay_eof_m);
            dsd_cond_broadcast(&g_stream->replay_eof_cond);
            dsd_mutex_unlock(&g_stream->replay_eof_m);
        }
        safe_cond_signal(&output.ready, &output.ready_m);
    }
}

static void
demod_maybe_signal_squelch_hop(struct demod_state* d) {
    if (!d) {
        return;
    }
    if (d->channel_squelch_level > 0.0f && d->channel_squelched) {
        d->squelch_hits++;
        if (d->squelch_hits > d->conseq_squelch) {
            d->squelch_hits = d->conseq_squelch + 1;
            safe_cond_signal(&controller.hop, &controller.hop_m);
        }
    } else {
        d->squelch_hits = 0;
    }
}

static int
demod_output_write_cancelled(void) {
    return (exitflag || controller.retune_in_progress.load(std::memory_order_acquire)) ? 1 : 0;
}

static int
demod_wait_for_output_space(struct output_state* o) {
    dsd_mutex_lock(&o->ready_m);
    int ret = dsd_cond_timedwait(&o->space, &o->ready_m, 10);
    dsd_mutex_unlock(&o->ready_m);
    if (ret == 0) {
        return 1;
    }
    if (exitflag) {
        return 0;
    }
    o->write_timeouts.fetch_add(1);
    return 1;
}

static size_t
demod_copy_output_chunk(struct output_state* o, const float* data, size_t count, size_t free_sp) {
    size_t write_now = (count < free_sp) ? count : free_sp;
    size_t h = o->head.load();
    size_t to_end = o->capacity - h;
    if (to_end >= write_now) {
        DSD_MEMCPY(o->buffer + h, data, write_now * sizeof(float));
        h += write_now;
        if (h >= o->capacity) {
            h = 0U;
        }
        o->head.store(h);
        return write_now;
    }
    if (to_end > 0U) {
        DSD_MEMCPY(o->buffer + h, data, to_end * sizeof(float));
    }
    size_t remaining = write_now - to_end;
    DSD_MEMCPY(o->buffer, data + to_end, remaining * sizeof(float));
    o->head.store(remaining);
    return write_now;
}

static size_t
demod_write_output_samples_interruptible(struct output_state* o, const float* data, size_t count) {
    if (!o || !o->buffer || !data || count == 0U) {
        return 0U;
    }
    int need_signal = ring_is_empty(o);
    size_t written = 0U;
    while (count > 0U && !demod_output_write_cancelled()) {
        size_t free_sp = ring_free(o);
        if (free_sp == 0U) {
            if (!demod_wait_for_output_space(o)) {
                break;
            }
            continue;
        }
        size_t write_now = demod_copy_output_chunk(o, data, count, free_sp);
        data += write_now;
        count -= write_now;
        written += write_now;
    }
    if (need_signal && written > 0U) {
        safe_cond_signal(&o->ready, &o->ready_m);
    }
    return written;
}

static size_t
demod_write_output_block(struct demod_state* d, struct output_state* o) {
    if (!d || !o) {
        return 0U;
    }
    if (d->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK || d->output_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK) {
        if (d->result_len > 0) {
            return demod_write_output_samples_interruptible(o, d->result, (size_t)d->result_len);
        }
        return 0U;
    }
    if (d->resamp_enabled) {
        int out_n = resamp_process_block(d, d->result, d->result_len, d->resamp_outbuf);
        if (out_n > 0) {
            apply_output_scale(d, d->resamp_outbuf, out_n);
            return demod_write_output_samples_interruptible(o, d->resamp_outbuf, (size_t)out_n);
        }
        return 0U;
    }
    if (d->result_len > 0) {
        apply_output_scale(d, d->result, d->result_len);
        return demod_write_output_samples_interruptible(o, d->result, (size_t)d->result_len);
    }
    return 0U;
}

static double
demod_perf_pick_snr_db(const struct demod_state* d) {
    if (!d) {
        return g_snr_c4fm_db.load(std::memory_order_relaxed);
    }
    double snr_db = g_snr_c4fm_db.load(std::memory_order_relaxed);
    if (d->cqpsk_enable) {
        return g_snr_qpsk_db.load(std::memory_order_relaxed);
    }
    double gfsk_snr = g_snr_gfsk_db.load(std::memory_order_relaxed);
    if (gfsk_snr > -50.0 && (snr_db <= -50.0 || gfsk_snr > snr_db)) {
        snr_db = gfsk_snr;
    }
    return snr_db;
}

static void
demod_perf_log_block(int perf_on, uint64_t perf_output_start_ns, uint64_t perf_full_demod_ns, uint64_t perf_metrics_ns,
                     int got, size_t perf_output_samples, const struct demod_state* d) {
    if (!perf_on || !d) {
        return;
    }
    uint64_t perf_output_write_ns = rtl_perf_now_ns() - perf_output_start_ns;
    rtl_perf_record_demod_block(perf_full_demod_ns, perf_metrics_ns, perf_output_write_ns, (size_t)got,
                                perf_output_samples);
    double snr_db = demod_perf_pick_snr_db(d);
    rtl_perf_log_snapshot snapshot = {
        rtl_perf_source_name(),
        load_dongle_rate(),
        d->output_kind,
        input_ring_used(&input_ring),
        input_ring.capacity,
        input_ring.producer_drops.load(std::memory_order_relaxed),
        ring_used(&output),
        output.capacity,
        -1,
        snr_db,
        dsd_rtl_stream_get_cfo_hz(),
        dsd_rtl_stream_get_carrier_lock(),
    };
    rtl_perf_maybe_log(&snapshot);
}

static int
demod_prepare_iteration_input(struct demod_state* d, int is_rtltcp_input, DemodInputSpan* span,
                              DemodRetuneDiagBlock* retune_diag) {
    if (!d || !span || !retune_diag) {
        return 0;
    }
    if (demod_should_pause_before_read(is_rtltcp_input)) {
        return 0;
    }
    if (!demod_read_input_block(d, span)) {
        return 0;
    }
    if (!demod_enter_processing_block(&controller)) {
        demod_input_span_commit_reserved(span);
        return 0;
    }
    if (!demod_prepare_input_block(d, span)) {
        demod_leave_processing_block(&controller);
        return 0;
    }
    if (controller.retune_in_progress.load(std::memory_order_acquire)
        || g_ring_purge_pending.load(std::memory_order_acquire)) {
        demod_input_span_commit_direct(span);
        demod_leave_processing_block(&controller);
        return 0;
    }
    int input_pairs = 0;
    float input_mean_abs = 0.0f;
    float input_max_abs = 0.0f;
    iq_block_abs_stats(span->input_block, span->got, &input_mean_abs, &input_max_abs, &input_pairs);
    if (retune_settle_should_discard(d, input_mean_abs, input_max_abs, input_pairs)) {
        demod_input_span_commit_direct(span);
        demod_leave_processing_block(&controller);
        return 0;
    }
    *retune_diag = demod_capture_retune_diag(input_mean_abs, input_max_abs, input_pairs);
    demod_autogain_update(d, input_mean_abs, input_max_abs);
    if (!controller.cold_start_ready.load(std::memory_order_acquire)) {
        demod_input_span_commit_direct(span);
        demod_leave_processing_block(&controller);
        return 0;
    }
    if (controller.retune_in_progress.load(std::memory_order_acquire)) {
        demod_input_span_commit_direct(span);
        demod_leave_processing_block(&controller);
        return 0;
    }
    d->lowpassed = span->input_block;
    d->lp_len = span->got;
    return 1;
}

static int
demod_prepare_iteration_processing(const struct demod_state* d, const DemodInputSpan* span, int* replay_active,
                                   uint64_t* consumed_gen) {
    if (!d || !span || !replay_active || !consumed_gen) {
        return 0;
    }
    *consumed_gen = 0ULL;
    *replay_active = stream_is_replay_active();
    if (*replay_active && g_stream) {
        *consumed_gen = g_stream->replay_last_submit_gen.load(std::memory_order_acquire);
    }
    return 1;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    demod_thread_fn(void* arg) {
    struct demod_state* d = static_cast<demod_state*>(arg);
    struct output_state* o = &output;
    maybe_set_thread_realtime_and_affinity("DEMOD");
    const int is_rtltcp_input = (g_stream && radio_source_is_rtltcp(g_stream->opts)) ? 1 : 0;
    while (!demod_should_exit_requested()) {
        DemodInputSpan span = {};
        DemodRetuneDiagBlock retune_diag = {};
        if (!demod_prepare_iteration_input(d, is_rtltcp_input, &span, &retune_diag)) {
            continue;
        }
        int replay_active = 0;
        uint64_t consumed_gen = 0ULL;
        if (!demod_prepare_iteration_processing(d, &span, &replay_active, &consumed_gen)) {
            demod_input_span_release_direct(d, &span);
            demod_leave_processing_block(&controller);
            continue;
        }
        int perf_on = rtl_perf_enabled();
        uint64_t perf_full_start_ns = perf_on ? rtl_perf_now_ns() : 0ULL;
        (void)rtl_stream_consume_fsk_reacquire_pending(d);
        full_demod(d);
        rtl_publish_fsk_metrics_from_demod(d);
        uint64_t perf_full_demod_ns = perf_on ? (rtl_perf_now_ns() - perf_full_start_ns) : 0ULL;
        rtl_monitor_side_tap_process(d);
        demod_log_retune_diag_block(d, span.got, &retune_diag);
        demod_input_span_release_direct(d, &span);
        demod_update_replay_drain_state(replay_active, consumed_gen);
        uint64_t perf_metrics_ns = demod_metrics_process(d, perf_on);
        if (d->exit_flag) {
            exitflag = 1;
        }
        demod_maybe_signal_squelch_hop(d);
        uint64_t perf_output_start_ns = perf_on ? rtl_perf_now_ns() : 0ULL;
        size_t perf_output_samples =
            controller.retune_in_progress.load(std::memory_order_acquire) ? 0U : demod_write_output_block(d, o);
        demod_perf_log_block(perf_on, perf_output_start_ns, perf_full_demod_ns, perf_metrics_ns, span.got,
                             perf_output_samples, d);
        demod_leave_processing_block(&controller);
    }
    DSD_THREAD_RETURN;
}

static int
rtl_floor_log2_nonzero(int value) {
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(value);
#else
    int floor_log2 = 0;
    int t = value;
    while (t >>= 1) {
        floor_log2++;
    }
    return floor_log2;
#endif
}

static int
rtl_choose_passes_near_good_rate(int rate_in_hz, int suggested_passes) {
    static const int good_rates[] = {960000, 1024000, 1200000, 1536000, 1920000, 2048000, 2400000};
    int best_p = suggested_passes;
    long long best_err = LLONG_MAX;
    for (int delta = -1; delta <= 1; delta++) {
        int p = suggested_passes + delta;
        p = std::max(0, std::min(10, p));
        long long cap = (long long)rate_in_hz * (1LL << p);
        if (cap < 225000LL || cap > 3200000LL) {
            continue;
        }
        for (size_t i = 0; i < sizeof(good_rates) / sizeof(good_rates[0]); i++) {
            long long err = llabs(cap - (long long)good_rates[i]);
            if (err < best_err) {
                best_err = err;
                best_p = p;
            }
        }
    }
    return best_p;
}

static int
rtl_downsample_passes_for_rate_in(int rate_in_hz) {
    int downsample_factor = (1000000 / rate_in_hz) + 1;
    if (downsample_factor <= 1) {
        return 0;
    }
    int floor_log2 = rtl_floor_log2_nonzero(downsample_factor);
    int is_pow2 = (downsample_factor & (downsample_factor - 1)) == 0;
    int passes = is_pow2 ? floor_log2 : (floor_log2 + 1);
    passes = std::max(0, std::min(10, passes));
    return rtl_choose_passes_near_good_rate(rate_in_hz, passes);
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

    struct demod_state* dm = &demod;
    dm->downsample_passes = rtl_downsample_passes_for_rate_in(dm->rate_in);
    int downsample_factor = 1 << dm->downsample_passes;
    int capture_rate = downsample_factor * dm->rate_in;
    uint32_t capture_rate_hz = (capture_rate > 0) ? (uint32_t)capture_rate : 0U;
    uint32_t capture_freq_hz = capture_frequency_for_rate((int64_t)freq, capture_rate_hz);
    /* Normalize discriminator radians into roughly [-1,1] for float pipeline. */
    dm->output_scale = (float)(1.0 / M_PI);
    /* Update the effective discriminator output sample rate based on current settings.
       HB cascade reduces by (1<<downsample_passes). Apply optional post_downsample on audio. */
    dm->rate_out = demod_output_rate_for_capture_rate(capture_rate_hz);
    store_dongle_frequency(capture_freq_hz);
    store_dongle_rate(capture_rate_hz);
}

/**
 * @brief Program device to new center frequency and sample rate using a
 * single, consistent path. Applies fs/4 shift when offset_tuning is off.
 *
 * @param center_freq_hz Desired RF center frequency in Hz.
 */
static void
program_capture_frequency_and_rate(uint32_t center_freq_hz) {
    optimal_settings((int)center_freq_hz, demod.rate_in);
    uint32_t capture_freq_hz = load_dongle_frequency();
    uint32_t capture_rate_hz = load_dongle_rate();
    rtl_device_set_frequency(rtl_device_handle, capture_freq_hz);
    rtl_device_set_sample_rate(rtl_device_handle, capture_rate_hz);
    /* Sync to actual device rate (USB may quantize). If it changed, update rate_out. */
    int actual = rtl_device_get_sample_rate(rtl_device_handle);
    if (actual > 0 && (uint32_t)actual != capture_rate_hz) {
        capture_rate_hz = apply_actual_capture_rate(center_freq_hz, capture_freq_hz, capture_rate_hz, (uint32_t)actual);
    }
    /* Use driver auto hardware bandwidth by default, or override via env */
    (void)apply_capture_tuner_bandwidth(capture_rate_hz, g_stream ? g_stream->opts : NULL, 0);
    stream_refresh_watermark_for_current_rate();
}

/**
 * @brief Program capture settings and apply hardware PPM correction when requested.
 *
 * @param ppm_error PPM correction to apply alongside the reconfigure.
 * @return Result from the PPM control apply attempt.
 */
static int
apply_capture_settings(uint32_t center_freq_hz, int ppm_error) {
    int ppm_rc = apply_ppm_setting(ppm_error);
    controller_arm_retune_mute("program");
    program_capture_frequency_and_rate(center_freq_hz);
    return ppm_rc;
}

static int
retune_mute_bytes_for_rate(uint32_t sample_rate_hz) {
    /* Drop the first post-retune callbacks so tuner-settling samples do not
     * train the freshly reset CQPSK TED/Costas loops or smear the retained FLL
     * coarse CFO estimate. */
    uint64_t mute_ms = 120;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->retune_mute_ms > 0) {
        mute_ms = (uint64_t)cfg->retune_mute_ms;
    }
    uint64_t bytes = ((uint64_t)sample_rate_hz * 2ULL * mute_ms) / 1000ULL;
    uint64_t min_bytes = (ACTUAL_BUF_LENGTH > 0) ? (uint64_t)ACTUAL_BUF_LENGTH : (uint64_t)DEFAULT_BUF_LENGTH;
    if (bytes < min_bytes) {
        bytes = min_bytes;
    }
    if (bytes > (uint64_t)INT_MAX) {
        bytes = (uint64_t)INT_MAX;
    }
    return (int)bytes;
}

static void
controller_arm_retune_mute(const char* phase) {
    uint32_t sample_rate_hz = load_dongle_rate();
    if (!rtl_device_handle || sample_rate_hz == 0) {
        return;
    }
    int mute_bytes = retune_mute_bytes_for_rate(sample_rate_hz);
    rtl_device_mute(rtl_device_handle, mute_bytes);
    if (debug_cqpsk_enabled()) {
        DSD_FPRINTF(stderr, "[RETUNE-MUTE] phase=%s rate=%u bytes=%d\n", phase ? phase : "unknown", sample_rate_hz,
                    mute_bytes);
    }
}

static void
controller_arm_post_retune_diagnostics(uint32_t center_freq_hz, DemodRetuneResetReason reason) {
    uint32_t seq = g_retune_diag_seq.fetch_add(1, std::memory_order_acq_rel) + 1U;
    g_retune_diag_freq_hz.store(center_freq_hz, std::memory_order_release);
    g_retune_diag_reason.store((int)reason, std::memory_order_release);
    g_retune_settle_seq.store(seq, std::memory_order_release);
    g_retune_settle_blocks_remaining.store(kRetuneSettleMaxBlocks, std::memory_order_release);
    g_retune_diag_blocks_remaining.store(debug_cqpsk_enabled() ? kRetuneDiagBlocks : 0, std::memory_order_release);
}

static uint32_t
rtl_stream_bump_output_generation(void) {
    uint32_t next = g_rtl_output_generation.fetch_add(1, std::memory_order_acq_rel) + 1U;
    if (next == 0U) {
        next = g_rtl_output_generation.fetch_add(1, std::memory_order_acq_rel) + 1U;
    }
    rtl_fsk_metrics_reset_snapshot();
    rtl_decode_health_reset();
    return next;
}

static int
rtl_stream_consume_fsk_reacquire_pending(struct demod_state* d) {
    int pending = g_fsk_reacquire_pending.exchange(0, std::memory_order_acq_rel);
    if (!pending || !d || d->output_kind != DSD_DEMOD_OUTPUT_SYMBOL_FSK) {
        return 0;
    }
    dsd_rtl_stream_clear_output();
    dsd_fsk_modem_reset(&d->fsk_modem_state);
    if (debug_sync_enabled()) {
        DSD_FPRINTF(stderr, "[FSKREACQ] consumed output_generation=%u\n",
                    g_rtl_output_generation.load(std::memory_order_acquire));
    }
    return 1;
}

static void
controller_finalize_rate_chain(struct controller_state* s, const dsd_opts* opts, uint32_t center_freq_hz,
                               int mark_reconfigure, DemodRetuneResetReason reset_reason,
                               uint32_t previous_center_freq_hz, int previous_rate_out_hz,
                               const RtlRetuneProfile* retune_profile) {
    if (!s || center_freq_hz == 0) {
        return;
    }
    s->last_applied_freq_hz.store(center_freq_hz, std::memory_order_release);
    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, opts, &output);
    rtl_stream_apply_retune_profile(retune_profile, center_freq_hz);
    rtl_demod_maybe_update_resampler_after_rate_change(&demod, &output, rtl_dsp_bw_hz);
    DemodRetuneResetPlan reset_plan = demod_retune_reset_plan(reset_reason, previous_center_freq_hz, center_freq_hz,
                                                              previous_rate_out_hz, demod.rate_out);
    demod_reset_on_retune(&demod, reset_plan);
    if (mark_reconfigure) {
        rtl_device_record_capture_reset(rtl_device_handle, center_freq_hz, load_dongle_frequency(), load_dongle_rate(),
                                        retune_reset_reason_name(reset_plan.reason));
    }
    /* Reconfigures invalidate after the configured output drain/clear policy runs. */
    if (!mark_reconfigure) {
        rtl_stream_bump_output_generation();
    }
    if (mark_reconfigure) {
        s->reconfigure_seq.fetch_add(1, std::memory_order_acq_rel);
        controller_request_input_purge();
        controller_arm_post_retune_diagnostics(center_freq_hz, reset_plan.reason);
    }
}

static void
controller_finalize_reconfigure(struct controller_state* s, const dsd_opts* opts, uint32_t center_freq_hz,
                                DemodRetuneResetReason reset_reason, uint32_t previous_center_freq_hz,
                                int previous_rate_out_hz, const RtlRetuneProfile* retune_profile) {
    controller_finalize_rate_chain(s, opts, center_freq_hz, 1, reset_reason, previous_center_freq_hz,
                                   previous_rate_out_hz, retune_profile);
}

static inline void
controller_enter_reconfigure_gate(struct controller_state* s) {
    if (!s) {
        return;
    }
    s->retune_in_progress.store(1, std::memory_order_release);
    rtl_stream_signal_output_waiters(g_stream && g_stream->output ? g_stream->output : &output);
    controller_wait_for_demod_idle(s);
    rtl_stream_clear_output_ring(g_stream && g_stream->output ? g_stream->output : &output, 1);
    g_retune_settle_blocks_remaining.store(0, std::memory_order_release);
    g_retune_diag_blocks_remaining.store(0, std::memory_order_release);
}

static inline void
controller_prepare_reconfigure_input(void) {
    rtl_device_begin_capture_reconfigure(rtl_device_handle);
    controller_request_input_purge();
    controller_arm_retune_mute("pre");
}

static inline void
controller_begin_reconfigure(struct controller_state* s) {
    controller_enter_reconfigure_gate(s);
    controller_prepare_reconfigure_input();
}

static inline void
controller_end_reconfigure(struct controller_state* s) {
    if (!s) {
        return;
    }
    s->retune_in_progress.store(0, std::memory_order_release);
}

static void
controller_reconfigure_active_stream_locked(struct controller_state* s, uint32_t center_freq_hz,
                                            DemodRetuneResetReason reset_reason) {
    if (!s || center_freq_hz == 0) {
        return;
    }
    uint32_t previous_center_freq_hz = s->last_applied_freq_hz.load(std::memory_order_acquire);
    int previous_rate_out_hz = demod.rate_out;
    controller_arm_retune_mute("program");
    program_capture_frequency_and_rate(center_freq_hz);
    controller_arm_retune_mute("post");
    rtl_device_record_capture_retune(rtl_device_handle, center_freq_hz, load_dongle_frequency(), load_dongle_rate(),
                                     retune_reset_reason_name(reset_reason));
    controller_finalize_reconfigure(s, g_stream ? g_stream->opts : NULL, center_freq_hz, reset_reason,
                                    previous_center_freq_hz, previous_rate_out_hz, NULL);
    controller_arm_retune_mute("post-reset");
    rtl_device_end_capture_reconfigure(rtl_device_handle);
}

static int
controller_apply_reconfigure(struct controller_state* s, uint32_t center_freq_hz, int ppm_error,
                             const RtlRetuneProfile* retune_profile) {
    if (!s || center_freq_hz == 0) {
        return -1;
    }
    int prev_ppm = load_dongle_ppm_error();
    uint32_t previous_center_freq_hz = s->last_applied_freq_hz.load(std::memory_order_acquire);
    int previous_rate_out_hz = demod.rate_out;
    controller_begin_reconfigure(s);
    int ppm_rc = apply_capture_settings(center_freq_hz, ppm_error);
    controller_arm_retune_mute("post");
    if (ppm_rc == 0) {
        store_dongle_ppm_error(ppm_error);
    }
    DemodRetuneResetReason reset_reason = (ppm_rc == 0 && ppm_error != prev_ppm)
                                              ? DemodRetuneResetReason::PpmCorrection
                                              : DemodRetuneResetReason::FrequencyRetune;
    rtl_device_record_capture_retune(rtl_device_handle, center_freq_hz, load_dongle_frequency(), load_dongle_rate(),
                                     retune_reset_reason_name(reset_reason));
    controller_finalize_reconfigure(s, g_stream ? g_stream->opts : NULL, center_freq_hz, reset_reason,
                                    previous_center_freq_hz, previous_rate_out_hz, retune_profile);
    controller_arm_retune_mute("post-reset");
    rtl_device_end_capture_reconfigure(rtl_device_handle);
    controller_end_reconfigure(s);
    return ppm_rc;
}

static void
replay_wait_for_input_purge_applied(void) {
    uint64_t deadline_ns = dsd_time_monotonic_ns() + 100000000ULL;
    while (g_ring_purge_pending.load(std::memory_order_acquire) && !exitflag
           && !(g_stream && g_stream->should_exit.load(std::memory_order_acquire))
           && dsd_time_monotonic_ns() < deadline_ns) {
        safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
        dsd_sleep_ms(1);
    }
}

static void
rtl_replay_on_retune_event(const dsd_iq_event* event, void* user) {
    UNUSED(user);
    if (!event) {
        return;
    }
    uint32_t center_hz = (event->center_frequency_hz > UINT32_MAX) ? UINT32_MAX : (uint32_t)event->center_frequency_hz;
    uint32_t capture_hz =
        (event->capture_center_frequency_hz > UINT32_MAX) ? UINT32_MAX : (uint32_t)event->capture_center_frequency_hz;
    store_dongle_frequency(capture_hz);
    store_dongle_rate(event->sample_rate_hz);
    g_replay_event_last_frequency_hz.store(center_hz, std::memory_order_release);
    g_replay_event_retune_count.fetch_add(1U, std::memory_order_acq_rel);
}

static void
rtl_replay_on_mute_event(const dsd_iq_event* event, void* user) {
    UNUSED(user);
    if (!event) {
        return;
    }
    g_replay_event_last_mute_bytes.store(event->duration_bytes, std::memory_order_release);
    g_replay_event_mute_count.fetch_add(1U, std::memory_order_acq_rel);
}

static void
rtl_replay_on_reset_event(const dsd_iq_event* event, void* user) {
    UNUSED(user);
    if (!event) {
        return;
    }
    uint32_t center_hz = (event->center_frequency_hz > UINT32_MAX) ? UINT32_MAX : (uint32_t)event->center_frequency_hz;
    uint32_t capture_hz =
        (event->capture_center_frequency_hz > UINT32_MAX) ? UINT32_MAX : (uint32_t)event->capture_center_frequency_hz;
    uint32_t previous_center_hz = controller.last_applied_freq_hz.load(std::memory_order_acquire);
    int previous_rate_out_hz = demod.rate_out;
    DemodRetuneResetReason reset_reason = retune_reset_reason_from_name(event->reason);
    store_dongle_frequency(capture_hz);
    store_dongle_rate(event->sample_rate_hz);
    controller_enter_reconfigure_gate(&controller);
    controller_request_input_purge();
    controller_finalize_reconfigure(&controller, g_stream ? g_stream->opts : NULL, center_hz, reset_reason,
                                    previous_center_hz, previous_rate_out_hz, NULL);
    controller_end_reconfigure(&controller);
    replay_wait_for_input_purge_applied();
    drain_output_on_retune();
    g_replay_event_last_reset_reason.store((int)reset_reason, std::memory_order_release);
    g_replay_event_last_frequency_hz.store(center_hz, std::memory_order_release);
    g_replay_event_reset_count.fetch_add(1U, std::memory_order_acq_rel);
}

static void
rtl_replay_on_loop_restart(const dsd_iq_replay_config* cfg, void* user) {
    UNUSED(user);
    if (!cfg) {
        return;
    }
    controller_enter_reconfigure_gate(&controller);
    controller_request_input_purge();
    (void)controller_apply_replay_settings(&controller, g_stream ? g_stream->opts : NULL, cfg);
    controller_end_reconfigure(&controller);
    replay_wait_for_input_purge_applied();
    drain_output_on_retune();
    g_replay_loop_restart_last_frequency_hz.store(controller.last_applied_freq_hz.load(std::memory_order_acquire),
                                                  std::memory_order_release);
    g_replay_loop_restart_count.fetch_add(1U, std::memory_order_acq_rel);
}

/* Resampler and TED SPS helpers are implemented in rtl_demod_config.cpp. */

static void
controller_apply_wb_frequency_offset(struct controller_state* s) {
    if (!s || !s->wb_mode) {
        return;
    }
    for (int i = 0; i < s->freq_len; i++) {
        s->freqs[i] += 16000;
    }
}

static void
controller_apply_initial_offset_tuning(const dsd_opts* opts) {
    int want = 1;
    if (radio_source_is_rtltcp(opts)) {
        want = 0;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->rtl_offset_tuning_is_set) {
        want = cfg->rtl_offset_tuning_enable ? 1 : 0;
    }
    int r = rtl_device_set_offset_tuning_enabled(rtl_device_handle, want);
    if (r == 0) {
        dongle.offset_tuning = want ? 1 : 0;
    } else {
        dongle.offset_tuning = 0;
    }
}

static uint32_t
controller_sync_initial_capture_rate(uint32_t center_freq_hz, uint32_t capture_freq_hz, uint32_t capture_rate_hz) {
    int actual = rtl_device_get_sample_rate(rtl_device_handle);
    if (actual > 0 && (uint32_t)actual != capture_rate_hz) {
        return apply_actual_capture_rate(center_freq_hz, capture_freq_hz, capture_rate_hz, (uint32_t)actual);
    }
    return capture_rate_hz;
}

static int
controller_program_initial_capture_settings(const struct controller_state* s, const dsd_opts* opts) {
    uint32_t capture_freq_hz = load_dongle_frequency();
    uint32_t capture_rate_hz = load_dongle_rate();
    (void)rtl_device_set_frequency(rtl_device_handle, capture_freq_hz);
    LOG_INFO("Oversampling input by: %ix.\n", (demod.downsample_passes > 0) ? (1 << demod.downsample_passes) : 1);
    LOG_INFO("Oversampling output by: %ix.\n", demod.post_downsample);
    LOG_INFO("Buffer size: %0.2fms\n", 1000 * 0.5 * (float)ACTUAL_BUF_LENGTH / (float)capture_rate_hz);
    (void)rtl_device_set_sample_rate(rtl_device_handle, capture_rate_hz);
    capture_rate_hz = controller_sync_initial_capture_rate((uint32_t)s->freqs[0], capture_freq_hz, capture_rate_hz);
    if (apply_capture_tuner_bandwidth(capture_rate_hz, opts, 1) != 0) {
        return -1;
    }
    LOG_INFO("Demod output at %u Hz.\n", (unsigned int)demod.rate_out);
    return 0;
}

static int
controller_apply_initial_settings(struct controller_state* s, const dsd_opts* opts) {
    if (!s || !opts || s->freq_len <= 0 || !rtl_device_handle) {
        return -1;
    }

    controller_apply_wb_frequency_offset(s);

    optimal_settings(s->freqs[0], demod.rate_in);
    if (dongle.direct_sampling) {
        (void)rtl_device_set_direct_sampling(rtl_device_handle, dongle.direct_sampling);
    }
    controller_apply_initial_offset_tuning(opts);

    optimal_settings(s->freqs[0], demod.rate_in);
    if (controller_program_initial_capture_settings(s, opts) != 0) {
        return -1;
    }

    controller_finalize_rate_chain(s, opts, (uint32_t)s->freqs[0], 0, DemodRetuneResetReason::FreshStream, 0U, 0, NULL);
    s->cold_start_ready.store(1, std::memory_order_release);
    return 0;
}

static int
controller_apply_replay_settings(struct controller_state* s, const dsd_opts* opts, const dsd_iq_replay_config* cfg) {
    if (!s || !opts || !cfg) {
        return -1;
    }
    if (cfg->sample_rate_hz == 0 || cfg->demod_rate_hz == 0 || cfg->base_decimation == 0 || cfg->post_downsample == 0) {
        return -1;
    }
    if ((cfg->base_decimation & (cfg->base_decimation - 1U)) != 0U) {
        return -1;
    }

    uint32_t dec = cfg->base_decimation;
    int passes = 0;
    while (dec > 1U) {
        dec >>= 1U;
        passes++;
    }

    store_dongle_rate(cfg->sample_rate_hz);
    uint32_t capture_freq_hz = (uint32_t)((cfg->capture_center_frequency_hz > 0) ? cfg->capture_center_frequency_hz
                                                                                 : cfg->center_frequency_hz);
    store_dongle_frequency(capture_freq_hz);
    demod.downsample_passes = passes;
    demod.post_downsample = (int)cfg->post_downsample;
    demod.rate_in = (int)(cfg->sample_rate_hz / cfg->base_decimation);
    if (demod.rate_in < 1) {
        demod.rate_in = 1;
    }
    demod.rate_out = (int)cfg->demod_rate_hz;

    uint32_t center_hz =
        (uint32_t)((cfg->center_frequency_hz > 0) ? cfg->center_frequency_hz : cfg->capture_center_frequency_hz);
    controller_finalize_rate_chain(s, opts, center_hz, 0, DemodRetuneResetReason::FreshStream, 0U, 0, NULL);
    s->cold_start_ready.store(1, std::memory_order_release);
    return 0;
}

/**
 * @brief Controller worker: scans/hops through configured center frequencies.
 *
 * The initial settings are applied synchronously during stream open. This loop
 * only handles runtime retune/PPM requests.
 */
namespace {
struct ControllerRetuneWork {
    int manual_pending;
    uint32_t manual_freq_hz;
    RtlRetuneProfile manual_profile;
    int requested_ppm;
    uint32_t requested_ppm_request_id;
    int ppm_pending;
    int current_ppm;
    int ppm_changed;
};
} // namespace

static void
controller_clear_active_ppm_request(struct controller_state* s, int ppm_pending) {
    if (!s || !ppm_pending) {
        return;
    }
    s->active_ppm_error.store(0, std::memory_order_release);
    s->active_ppm_request_seq.store(0, std::memory_order_release);
    s->ppm_apply_in_progress.store(0, std::memory_order_release);
}

static int
controller_wait_for_retune_work(struct controller_state* s, ControllerRetuneWork* work) {
    if (!s || !work) {
        return 0;
    }
    work->manual_pending = 0;
    work->manual_freq_hz = 0;
    rtl_stream_clear_retune_profile(&work->manual_profile);
    dsd_mutex_lock(&s->hop_m);
    while (!s->manual_retune_pending.load(std::memory_order_acquire)
           && !s->ppm_change_pending.load(std::memory_order_acquire) && !exitflag
           && !(g_stream && g_stream->should_exit.load())) {
        dsd_cond_wait(&s->hop, &s->hop_m);
    }
    if (exitflag || (g_stream && g_stream->should_exit.load())) {
        dsd_mutex_unlock(&s->hop_m);
        return 0;
    }
    work->manual_pending = s->manual_retune_pending.exchange(0, std::memory_order_acq_rel);
    if (work->manual_pending) {
        work->manual_freq_hz = s->manual_retune_freq;
        work->manual_profile = s->manual_retune_profile;
        rtl_stream_clear_retune_profile(&s->manual_retune_profile);
    }
    work->requested_ppm = s->pending_ppm_error.load(std::memory_order_acquire);
    work->requested_ppm_request_id = s->pending_ppm_request_seq.load(std::memory_order_acquire);
    work->ppm_pending = s->ppm_change_pending.exchange(0, std::memory_order_acq_rel);
    if (work->ppm_pending) {
        s->active_ppm_error.store(work->requested_ppm, std::memory_order_release);
        s->active_ppm_request_seq.store(work->requested_ppm_request_id, std::memory_order_release);
        s->ppm_apply_in_progress.store(1, std::memory_order_release);
    }
    dsd_mutex_unlock(&s->hop_m);
    work->current_ppm = load_dongle_ppm_error();
    work->ppm_changed = work->ppm_pending && (work->requested_ppm != work->current_ppm);
    return 1;
}

static void
controller_signal_manual_retune_complete(struct controller_state* s) {
    dsd_mutex_lock(&s->retune_done_m);
    s->retune_complete_id.fetch_add(1, std::memory_order_release);
    dsd_cond_broadcast(&s->retune_done_cond);
    dsd_mutex_unlock(&s->retune_done_m);
}

static int
controller_process_manual_retune(struct controller_state* s, const ControllerRetuneWork* work) {
    if (!s || !work || !work->manual_pending) {
        return 0;
    }
    uint32_t target_hz = work->manual_freq_hz;
    int target_ppm = work->ppm_changed ? work->requested_ppm : work->current_ppm;
    int ppm_rc = controller_apply_reconfigure(s, target_hz, target_ppm, &work->manual_profile);
    if (work->ppm_changed && ppm_rc != 0) {
        note_failed_ppm_request(work->requested_ppm, work->requested_ppm_request_id, work->current_ppm, ppm_rc);
    }
    controller_signal_manual_retune_complete(s);
    drain_output_on_retune();
    controller_clear_active_ppm_request(s, work->ppm_pending);
    if (work->ppm_changed && ppm_rc == 0) {
        LOG_INFO("Retune applied: %u Hz (PPM=%d).\n", target_hz, work->requested_ppm);
    } else {
        LOG_INFO("Retune applied: %u Hz.\n", target_hz);
    }
    return 1;
}

static uint32_t
controller_ppm_fallback_frequency_hz(const struct controller_state* s) {
    if (!s || s->freq_len <= 0) {
        return 0;
    }
    return (uint32_t)s->freqs[s->freq_now];
}

static void
controller_process_ppm_change(struct controller_state* s, const ControllerRetuneWork* work) {
    if (!s || !work || !work->ppm_changed) {
        return;
    }
    uint32_t fallback_freq_hz = controller_ppm_fallback_frequency_hz(s);
    const dsd::io::radio::RtlPpmApplyPlan ppm_plan =
        dsd::io::radio::rtl_ppm_plan_apply_to_active_stream(s->last_applied_freq_hz.load(std::memory_order_acquire),
                                                            fallback_freq_hz, work->current_ppm, work->requested_ppm);
    const bool reconfigure_gate_active = (ppm_plan.reconfigure != 0);
    if (reconfigure_gate_active) {
        controller_enter_reconfigure_gate(s);
    }
    const int ppm_rc = apply_ppm_setting(work->requested_ppm);
    if (dsd::io::radio::rtl_ppm_should_reconfigure_after_apply(ppm_plan, ppm_rc)) {
        controller_prepare_reconfigure_input();
        store_dongle_ppm_error(work->requested_ppm);
        controller_reconfigure_active_stream_locked(s, ppm_plan.freq_hz, DemodRetuneResetReason::PpmCorrection);
        controller_end_reconfigure(s);
        drain_output_on_retune();
        LOG_INFO("PPM correction applied: %d (reconfigured %u Hz).\n", work->requested_ppm, ppm_plan.freq_hz);
        return;
    }
    if (ppm_rc == 0) {
        if (reconfigure_gate_active) {
            controller_end_reconfigure(s);
        }
        store_dongle_ppm_error(work->requested_ppm);
        LOG_INFO("PPM correction applied: %d.\n", work->requested_ppm);
        return;
    }
    if (reconfigure_gate_active) {
        controller_end_reconfigure(s);
    }
    note_failed_ppm_request(work->requested_ppm, work->requested_ppm_request_id, work->current_ppm, ppm_rc);
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    controller_thread_retune_loop(void* arg) {
    struct controller_state* s = static_cast<controller_state*>(arg);

    while (!exitflag && !(g_stream && g_stream->should_exit.load())) {
        ControllerRetuneWork work = {};
        if (!controller_wait_for_retune_work(s, &work)) {
            break;
        }
        if (controller_process_manual_retune(s, &work)) {
            continue;
        }
        if (work.ppm_pending) {
            controller_process_ppm_change(s, &work);
            controller_clear_active_ppm_request(s, work.ppm_pending);
            continue;
        }
        if (s->freq_len <= 1) {
            continue;
        }
        s->freq_now = (s->freq_now + 1) % s->freq_len;
        controller_apply_reconfigure(s, (uint32_t)s->freqs[s->freq_now], work.current_ppm, NULL);
        drain_output_on_retune();
    }
    DSD_THREAD_RETURN;
}

/* ---------------- Constellation capture (simple lock-free ring) ---------------- */

static const int kConstMaxPairs = 8192;
static float g_const_xy[kConstMaxPairs * 2];
static volatile int g_const_head = 0; /* pairs written [0..kConstMaxPairs-1], wraps */

/**
 * @brief Clear the constellation ring buffer.
 *
 * Called on retune to prevent stale samples from the previous frequency/SPS
 * from contaminating the constellation display. Without this, the UI shows
 * a mix of old and new constellation points, creating the appearance of
 * degraded SNR even when the DSP is performing correctly.
 */
static void
constellation_ring_clear(void) {
    DSD_MEMSET(g_const_xy, 0, sizeof(g_const_xy));
    g_const_head = 0;
}

/* Forward decl for eye-ring append used in demod loop */
static inline void eye_ring_append_i_chan(const float* iq_interleaved, int len_interleaved);

/* Append decimated I/Q samples from lowpassed[] after DSP. */
static void
constellation_ring_append(const float* iq, int len, int sps_hint) {
    if (!iq || len < 2) {
        return;
    }
    int N = len >> 1;                                              /* complex samples */
    int stride = (sps_hint >= 1 && sps_hint <= 64) ? sps_hint : 4; /* rough decimation */
    for (int n = 0; n < N; n += stride) {
        float i = iq[(size_t)(n << 1) + 0];
        float q = iq[(size_t)(n << 1) + 1];
        int h = g_const_head;
        g_const_xy[(size_t)(h << 1) + 0] = i;
        g_const_xy[(size_t)(h << 1) + 1] = q;
        h++;
        if (h >= kConstMaxPairs) {
            h = 0;
        }
        g_const_head = h;
    }
}

extern "C" int
dsd_rtl_stream_constellation_get(float* out_xy, int max_points) {
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
static float g_eye_buf[kEyeMax];
static volatile int g_eye_head = 0; /* samples written [0..kEyeMax-1], wraps */

/**
 * @brief Clear the eye diagram ring buffer.
 *
 * Called on retune alongside constellation_ring_clear() to prevent stale
 * samples from contaminating the eye diagram display.
 */
static void
eye_ring_clear(void) {
    DSD_MEMSET(g_eye_buf, 0, sizeof(g_eye_buf));
    g_eye_head = 0;
}

static inline void
eye_ring_append_i_chan(const float* iq_interleaved, int len_interleaved) {
    if (!iq_interleaved || len_interleaved < 2) {
        return;
    }
    int N = len_interleaved >> 1; /* complex samples */
    for (int n = 0; n < N; n++) {
        float i = iq_interleaved[(size_t)(n << 1) + 0];
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
dsd_rtl_stream_eye_get(float* out, int max_samples, int* out_sps) {
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
static int
snr_eye_phase_in_window(int phase, int c1, int c2, int win) {
    return (abs(phase - c1) <= win) || (abs(phase - c2) <= win);
}

static int
snr_eye_downsample_for_quantiles(const float* src, int src_count, float* dst, int dst_capacity) {
    if (!src || !dst || src_count <= 0 || dst_capacity <= 0) {
        return 0;
    }
    int step = (src_count > dst_capacity) ? (src_count / dst_capacity) : 1;
    int copied = 0;
    for (int i = 0; i < src_count && copied < dst_capacity; i += step) {
        dst[copied++] = src[i];
    }
    return copied;
}

static int
snr_eye_quartiles(float* vals, int count, float* q1, float* q2, float* q3) {
    if (!vals || count < 8 || !q1 || !q2 || !q3) {
        return 0;
    }
    int idx1 = (int)((size_t)count / 4);
    int idx2 = (int)((size_t)count / 2);
    int idx3 = (int)((size_t)(3 * (size_t)count) / 4);
    std::nth_element(vals, vals + idx2, vals + count);
    *q2 = vals[idx2];
    std::nth_element(vals, vals + idx1, vals + idx2);
    *q1 = vals[idx1];
    std::nth_element(vals + idx2 + 1, vals + idx3, vals + count);
    *q3 = vals[idx3];
    return 1;
}

static int
snr_eye_median(float* vals, int count, float* q2) {
    if (!vals || !q2 || count < 8) {
        return 0;
    }
    int idx2 = (int)((size_t)count / 2);
    std::nth_element(vals, vals + idx2, vals + count);
    *q2 = vals[idx2];
    return 1;
}

static int
snr_eye_c4fm_cluster_index(float v, float q1, float q2, float q3) {
    return (v <= q1) ? 0 : (v <= q2) ? 1 : (v <= q3) ? 2 : 3;
}

static long long
snr_eye_c4fm_collect_clusters(const float* eb, int nfb, int two_sps, int c1, int c2, int win, float q1, float q2,
                              float q3, long long cnt[4], double sum[4]) {
    for (int b = 0; b < 4; b++) {
        cnt[b] = 0;
        sum[b] = 0.0;
    }
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        if (!snr_eye_phase_in_window(phase, c1, c2, win)) {
            continue;
        }
        float v = eb[i];
        int b = snr_eye_c4fm_cluster_index(v, q1, q2, q3);
        cnt[b]++;
        sum[b] += (double)v;
    }
    return cnt[0] + cnt[1] + cnt[2] + cnt[3];
}

static double
snr_eye_c4fm_noise_variance(const float* eb, int nfb, int two_sps, int c1, int c2, int win, float q1, float q2,
                            float q3, const double mu[4], long long total) {
    double nsum = 0.0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        if (!snr_eye_phase_in_window(phase, c1, c2, win)) {
            continue;
        }
        float v = eb[i];
        int b = snr_eye_c4fm_cluster_index(v, q1, q2, q3);
        double e = (double)v - mu[b];
        nsum += e * e;
    }
    return nsum / (double)total;
}

static double
snr_eye_signal_variance_from_means(const double* mu, const long long* cnt, long long total) {
    double mu_all = 0.0;
    for (int b = 0; b < 4; b++) {
        mu_all += mu[b] * (double)cnt[b] / (double)total;
    }
    double ssum = 0.0;
    for (int b = 0; b < 4; b++) {
        double d = mu[b] - mu_all;
        ssum += (double)cnt[b] * d * d;
    }
    return ssum / (double)total;
}

static int
snr_qpsk_axis_means(const float* xy, int n, double* aI_out, double* aQ_out) {
    double sum_abs_i = 0.0;
    double sum_abs_q = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        sum_abs_i += fabs(I);
        sum_abs_q += fabs(Q);
    }
    double aI = sum_abs_i / (double)n;
    double aQ = sum_abs_q / (double)n;
    if (!(aI > 1e-9 && aQ > 1e-9)) {
        return 0;
    }
    *aI_out = aI;
    *aQ_out = aQ;
    return 1;
}

static double
snr_qpsk_error_axis(const float* xy, int n, double aI, double aQ) {
    double e2 = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        double ti = (I >= 0.0) ? aI : -aI;
        double tq = (Q >= 0.0) ? aQ : -aQ;
        double ei = I - ti;
        double eq = Q - tq;
        e2 += ei * ei + eq * eq;
    }
    return e2;
}

static double
snr_qpsk_error_diag(const float* xy, int n, double aD) {
    double e2 = 0.0;
    for (int i = 0; i < n; i++) {
        double I = (double)xy[(size_t)(i << 1) + 0];
        double Q = (double)xy[(size_t)(i << 1) + 1];
        double ti = (I >= 0.0) ? aD : -aD;
        double tq = (Q >= 0.0) ? aD : -aD;
        double ei = I - ti;
        double eq = Q - tq;
        e2 += ei * ei + eq * eq;
    }
    return e2;
}

namespace {

struct snr_eye_gfsk_window {
    int two_sps;
    int c1;
    int c2;
    int win;
    float q2;
};

} // namespace

namespace {

struct snr_eye_gfsk_sums {
    double sumL;
    double sumH;
    int cntL;
    int cntH;
};

} // namespace

static int
snr_eye_gfsk_collect_binary(const float* eb, int nfb, const snr_eye_gfsk_window* window, snr_eye_gfsk_sums* sums) {
    sums->sumL = 0.0;
    sums->sumH = 0.0;
    sums->cntL = 0;
    sums->cntH = 0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % window->two_sps;
        if (!snr_eye_phase_in_window(phase, window->c1, window->c2, window->win)) {
            continue;
        }
        float v = eb[i];
        if (v <= window->q2) {
            sums->sumL += v;
            sums->cntL++;
        } else {
            sums->sumH += v;
            sums->cntH++;
        }
    }
    return (sums->cntL > 0 && sums->cntH > 0) ? 1 : 0;
}

static double
snr_eye_gfsk_noise_variance(const float* eb, int nfb, int two_sps, int c1, int c2, int win, float q2, double muL,
                            double muH, int total) {
    double nsum = 0.0;
    for (int i = 0; i < nfb; i++) {
        int phase = i % two_sps;
        if (!snr_eye_phase_in_window(phase, c1, c2, win)) {
            continue;
        }
        float v = eb[i];
        double mu = (v <= q2) ? muL : muH;
        double e = (double)v - mu;
        nsum += e * e;
    }
    return nsum / (double)total;
}

extern "C" double
dsd_rtl_stream_estimate_snr_c4fm_eye(void) {
    enum : uint16_t { MAXS = 4096 };

    static float eb[(size_t)MAXS];
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
    static float qv[4096];
    int mct = snr_eye_downsample_for_quantiles(eb, nfb, qv, 4096);
    float q1 = 0.0f;
    float q2 = 0.0f;
    float q3 = 0.0f;
    if (!snr_eye_quartiles(qv, mct, &q1, &q2, &q3)) {
        return -100.0;
    }

    long long cnt[4] = {0, 0, 0, 0};
    double sum[4] = {0, 0, 0, 0};
    long long total = snr_eye_c4fm_collect_clusters(eb, nfb, two_sps, c1, c2, win, q1, q2, q3, cnt, sum);
    if (!(total > 50 && cnt[0] && cnt[1] && cnt[2] && cnt[3])) {
        return -100.0;
    }
    double mu[4];
    for (int b = 0; b < 4; b++) {
        mu[b] = sum[b] / (double)cnt[b];
    }
    double noise_var = snr_eye_c4fm_noise_variance(eb, nfb, two_sps, c1, c2, win, q1, q2, q3, mu, total);
    if (noise_var <= 1e-9) {
        return -100.0;
    }
    double sig_var = snr_eye_signal_variance_from_means(mu, cnt, total);
    if (sig_var <= 1e-9) {
        return -100.0;
    }
    double bias = dsd_snr_bias_c4fm_db(demod.rate_out, demod.ted_sps, demod.channel_lpf_profile);
    return 10.0 * log10(sig_var / noise_var) - bias;
}

/* ---------------- Constellation-based SNR estimation (QPSK fallback) ---------------- */
extern "C" double
dsd_rtl_stream_estimate_snr_qpsk_const(void) {
    enum : uint16_t { MAXP = 4096 };

    static float xy[(size_t)MAXP * 2];
    int n = dsd_rtl_stream_constellation_get(xy, MAXP);
    if (n <= 64) {
        return -100.0;
    }
    double aI = 0.0;
    double aQ = 0.0;
    if (!snr_qpsk_axis_means(xy, n, &aI, &aQ)) {
        return -100.0;
    }
    double e2_axis = snr_qpsk_error_axis(xy, n, aI, aQ);
    double t2_axis = (double)n * (aI * aI + aQ * aQ);
    double aD = std::max(1e-9, 0.5 * (aI + aQ));
    double e2_diag = snr_qpsk_error_diag(xy, n, aD);
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
    double bias = dsd_snr_bias_evm_db(demod.rate_out, demod.ted_sps, demod.channel_lpf_profile);
    return best_snr - bias;
}

/* ---------------- Eye-based SNR estimation (GFSK fallback, 2-level) ---------------- */
extern "C" double
dsd_rtl_stream_estimate_snr_gfsk_eye(void) {
    enum : uint16_t { MAXS = 4096 };

    static float eb[(size_t)MAXS];
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
    static float qv[4096];
    int mct = snr_eye_downsample_for_quantiles(eb, nfb, qv, 4096);
    float q2 = 0.0f;
    if (!snr_eye_median(qv, mct, &q2)) {
        return -100.0;
    }
    snr_eye_gfsk_window window = {two_sps, c1, c2, win, q2};
    snr_eye_gfsk_sums sums;
    if (!snr_eye_gfsk_collect_binary(eb, nfb, &window, &sums)) {
        return -100.0;
    }
    double muL = sums.sumL / (double)sums.cntL, muH = sums.sumH / (double)sums.cntH;
    int total = sums.cntL + sums.cntH;
    double noise_var = snr_eye_gfsk_noise_variance(eb, nfb, two_sps, c1, c2, win, q2, muL, muH, total);
    if (noise_var <= 1e-9) {
        return -100.0;
    }
    double mu_all = (muL * (double)sums.cntL + muH * (double)sums.cntH) / (double)total;
    double ssum =
        (double)sums.cntL * (muL - mu_all) * (muL - mu_all) + (double)sums.cntH * (muH - mu_all) * (muH - mu_all);
    double sig_var = ssum / (double)total;
    if (sig_var <= 1e-9) {
        return -100.0;
    }
    double bias = dsd_snr_bias_evm_db(demod.rate_out, demod.ted_sps, demod.channel_lpf_profile);
    return 10.0 * log10(sig_var / noise_var) - bias;
}

/**
 * @brief Initialize dongle (RTL-SDR source) state with default parameters.
 *
 * @param s Dongle state to initialize.
 */
static void
dongle_init(struct dongle_state* s) {
    s->freq.store(0, std::memory_order_relaxed);
    s->rate.store((uint32_t)rtl_dsp_bw_hz, std::memory_order_relaxed);
    s->gain = AUTO_GAIN; // tenths of a dB
    s->ppm_error.store(0, std::memory_order_relaxed);
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
static void
output_init(struct output_state* s) {
    s->rate = rtl_dsp_bw_hz;
    dsd_cond_init(&s->ready);
    dsd_cond_init(&s->space);
    dsd_mutex_init(&s->ready_m);
    /* Allocate SPSC ring buffer */
    s->capacity = (size_t)(MAXIMUM_BUF_LENGTH * 8);
    /* Try aligned allocation for better vectorized copies; fall back if unavailable */
    {
        void* mem_ptr = dsd_neo_aligned_malloc(s->capacity * sizeof(float));
        if (!mem_ptr) {
            LOG_ERROR("Failed to allocate output ring buffer (%zu samples).\n", s->capacity);
            /* Propagate by keeping buffer NULL; callers must detect before use */
            return;
        }
        s->buffer = static_cast<float*>(mem_ptr);
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
static void
output_cleanup(struct output_state* s) {
    dsd_cond_destroy(&s->ready);
    dsd_cond_destroy(&s->space);
    dsd_mutex_destroy(&s->ready_m);
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
static void
controller_init(struct controller_state* s) {
    s->freqs[0] = 446000000;
    s->freq_len = 0;
    s->edge = 0;
    s->wb_mode = 0;
    dsd_cond_init(&s->hop);
    dsd_mutex_init(&s->hop_m);
    s->manual_retune_pending.store(0);
    s->manual_retune_freq = 0;
    rtl_stream_clear_retune_profile(&s->manual_retune_profile);
    s->ppm_change_pending.store(0);
    s->pending_ppm_error.store(0);
    s->ppm_request_publish_seq.store(0);
    s->pending_ppm_request_seq.store(0);
    s->ppm_apply_in_progress.store(0);
    s->active_ppm_error.store(0);
    s->active_ppm_request_seq.store(0);
    s->ppm_apply_failure_pending.store(0);
    s->failed_ppm_error.store(0);
    s->failed_ppm_request_seq.store(0);
    s->cold_start_ready.store(0); /* Demod will wait for controller to signal ready */
    s->retune_in_progress.store(0);
    s->demod_processing_active.store(0);
    /* Initialize retune completion synchronization */
    dsd_cond_init(&s->retune_done_cond);
    dsd_mutex_init(&s->retune_done_m);
    s->retune_done_flag.store(0);
    s->retune_request_id.store(0);
    s->retune_complete_id.store(0);
    s->last_applied_freq_hz.store(0, std::memory_order_release);
    s->reconfigure_seq.store(0, std::memory_order_release);
}

/**
 * @brief Destroy controller synchronization primitives.
 *
 * @param s Controller state to clean up.
 */
static void
controller_cleanup(struct controller_state* s) {
    dsd_cond_destroy(&s->hop);
    dsd_mutex_destroy(&s->hop_m);
    dsd_cond_destroy(&s->retune_done_cond);
    dsd_mutex_destroy(&s->retune_done_m);
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
        g_stream->replay_forced_stop.store(1, std::memory_order_release);
        g_stream->should_exit.store(1, std::memory_order_release);
    }
    safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
    safe_cond_signal(&controller.hop, &controller.hop_m);
    safe_cond_signal(&demod.ready, &demod.ready_m);
    safe_cond_signal(&output.ready, &output.ready_m);
    if (g_stream && g_stream->replay_eof_sync_inited) {
        dsd_mutex_lock(&g_stream->replay_eof_m);
        dsd_cond_broadcast(&g_stream->replay_eof_cond);
        dsd_mutex_unlock(&g_stream->replay_eof_m);
    }
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
        LOG_INFO("Requested RTL PPM Error Set to %d\n", opts->rtlsdr_ppm_error);
    }
    dongle.dev_index = opts->rtl_dev_index;
    LOG_INFO("Setting DSP baseband to %d Hz\n", rtl_dsp_bw_hz);
    LOG_INFO("Setting RTL Power Squelch Level to %.1f dB\n", pwr_to_dB(opts->rtl_squelch_level));
    port = 0;
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
    if (opts->rtl_udp_bindaddr[0] != '\0') {
        DSD_SNPRINTF(udp_control_bindaddr, sizeof udp_control_bindaddr, "%s", opts->rtl_udp_bindaddr);
        udp_control_bindaddr[sizeof udp_control_bindaddr - 1] = '\0';
    } else {
        DSD_SNPRINTF(udp_control_bindaddr, sizeof udp_control_bindaddr, "%s", "127.0.0.1");
    }
    if (opts->rtl_gain_value > 0) {
        dongle.gain = opts->rtl_gain_value * 10;
    }
}

/**
 * @brief Enqueue a manual retune on the controller thread and return its request ID.
 *
 * Coalesces callers when a retune is already pending (controller has not yet
 * consumed the request) so that completion IDs track the number of retunes
 * actually executed. This prevents synchronous waiters from timing out when
 * multiple retune requests arrive faster than the controller loop can service
 * them.
 *
 * @param target_freq_hz Desired center frequency in Hz.
 * @return Request ID that will be completed once the queued retune finishes.
 */
static uint32_t
schedule_manual_retune_on_controller(struct controller_state* s, uint32_t target_freq_hz) {
    if (!s) {
        return 0U;
    }
    dsd_mutex_lock(&s->hop_m);
    uint32_t request_id = s->retune_request_id.load(std::memory_order_acquire);
    int pending = s->manual_retune_pending.load(std::memory_order_acquire);
    if (!pending) {
        request_id = s->retune_request_id.fetch_add(1, std::memory_order_acq_rel) + 1;
        s->manual_retune_pending.store(1, std::memory_order_release);
        rtl_stream_clear_retune_profile(&s->manual_retune_profile);
    }
    /* Update/override target frequency even when coalescing into an existing pending retune. */
    s->manual_retune_freq = target_freq_hz;
    (void)rtl_stream_take_pending_retune_profile(&s->manual_retune_profile, request_id, target_freq_hz);
    dsd_cond_signal(&s->hop);
    dsd_mutex_unlock(&s->hop_m);
    return request_id;
}

static uint32_t
schedule_manual_retune(uint32_t target_freq_hz) {
    return schedule_manual_retune_on_controller(&controller, target_freq_hz);
}

static void
sync_requested_ppm_to_controller(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    int applied_ppm = load_dongle_ppm_error();
    dsd_mutex_lock(&controller.hop_m);
    std::lock_guard<std::mutex> request_lock(g_requested_ppm_state_mutex);
    int requested_ppm = opts->rtlsdr_ppm_error;
    uint32_t requested_ppm_request_id = controller.ppm_request_publish_seq.load(std::memory_order_relaxed);
    dsd::io::radio::RtlPpmControllerRequestState queued_request = {};
    queued_request.pending = controller.ppm_change_pending.load(std::memory_order_acquire);
    if (queued_request.pending) {
        queued_request.ppm = controller.pending_ppm_error.load(std::memory_order_acquire);
        queued_request.request_id = controller.pending_ppm_request_seq.load(std::memory_order_acquire);
    }
    dsd::io::radio::RtlPpmControllerRequestState active_request = {};
    active_request.pending = controller.ppm_apply_in_progress.load(std::memory_order_acquire);
    if (active_request.pending) {
        active_request.ppm = controller.active_ppm_error.load(std::memory_order_acquire);
        active_request.request_id = controller.active_ppm_request_seq.load(std::memory_order_acquire);
    }
    bool needs_schedule = dsd::io::radio::rtl_ppm_should_schedule_request(
        applied_ppm, requested_ppm, requested_ppm_request_id, queued_request, active_request);
    if (needs_schedule) {
        controller.pending_ppm_error.store(requested_ppm, std::memory_order_release);
        controller.pending_ppm_request_seq.store(requested_ppm_request_id, std::memory_order_release);
        controller.ppm_change_pending.store(1, std::memory_order_release);
        dsd_cond_signal(&controller.hop);
    }
    dsd_mutex_unlock(&controller.hop_m);
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
    if (dsd_thread_create(&controller.thread, controller_thread_retune_loop, &controller) == 0) {
        if (g_stream) {
            g_stream->controller_thread_started.store(1, std::memory_order_release);
        }
    }
    if (dsd_thread_create(&demod.thread, demod_thread_fn, &demod) == 0) {
        if (g_stream) {
            g_stream->demod_thread_started.store(1, std::memory_order_release);
        }
    }
    LOG_INFO("Starting RTL async read...\n");
    if (rtl_device_start_async(rtl_device_handle, (uint32_t)ACTUAL_BUF_LENGTH) == 0) {
        if (g_stream) {
            g_stream->async_started.store(1, std::memory_order_release);
        }
    }
    if (port != 0) {
        g_udp_ctrl = udp_control_start_bound(udp_control_bindaddr, port, [](uint32_t new_freq_hz) {
            /* Marshal onto controller thread: single programming path */
            schedule_manual_retune(new_freq_hz);
        });
        if (!g_udp_ctrl) {
            LOG_ERROR("Failed to start RTL UDP retune control on %s:%u\n", udp_control_bindaddr, (unsigned)port);
        }
    }
}

static void
capture_drop_warning_log(void* user, uint64_t dropped_bytes, uint64_t dropped_blocks) {
    UNUSED(user);
    LOG_WARNING("IQ capture queue dropping data: dropped_bytes=%llu dropped_blocks=%llu\n",
                (unsigned long long)dropped_bytes, (unsigned long long)dropped_blocks);
}

static int
capture_stage_for_format(int format, char* out_stage, size_t out_stage_size) {
    if (!out_stage || out_stage_size == 0) {
        return -1;
    }
    if (format == DSD_IQ_FORMAT_CU8) {
        DSD_SNPRINTF(out_stage, out_stage_size, "%s", "post_mute_pre_widen");
        return 0;
    }
    if (format == DSD_IQ_FORMAT_CF32) {
        DSD_SNPRINTF(out_stage, out_stage_size, "%s", "post_driver_cf32_pre_ring");
        return 0;
    }
    return -1;
}

static const char*
capture_backend_name(RadioSourceKind source_kind) {
    if (source_kind == RADIO_SOURCE_RTL_TCP) {
        return "rtl_tcp";
    }
    if (source_kind == RADIO_SOURCE_SOAPY) {
        return "soapy";
    }
    return "rtl_usb";
}

static void
capture_backend_args(const dsd_opts* opts, RadioSourceKind source_kind, char* out_args, size_t out_args_size) {
    if (!out_args || out_args_size == 0) {
        return;
    }
    out_args[0] = '\0';
    if (!opts) {
        return;
    }
    if (source_kind == RADIO_SOURCE_RTL_TCP) {
        const char* host = opts->rtltcp_hostname;
        size_t host_len = strnlen(host, out_args_size - 1U);
        int port_chars = DSD_SNPRINTF(NULL, 0, ":%d", opts->rtltcp_portno);
        if (port_chars < 0) {
            out_args[0] = '\0';
            return;
        }
        size_t port_len = (size_t)port_chars;
        if (port_len >= out_args_size) {
            out_args[0] = '\0';
            return;
        }
        if (host_len > out_args_size - port_len - 1U) {
            host_len = out_args_size - port_len - 1U;
        }
        DSD_MEMCPY(out_args, host, host_len);
        int written = DSD_SNPRINTF(out_args + host_len, out_args_size - host_len, ":%d", opts->rtltcp_portno);
        if (written < 0) {
            out_args[0] = '\0';
            return;
        }
        return;
    }
    if (source_kind == RADIO_SOURCE_SOAPY) {
        const char* soapy_args = radio_source_soapy_args(opts);
        DSD_SNPRINTF(out_args, out_args_size, "%s", soapy_args ? soapy_args : "");
        return;
    }
    DSD_SNPRINTF(out_args, out_args_size, "index=%d", opts->rtl_dev_index);
}

static int
stream_open_validate_capture_writer_request(const dsd_opts* opts, RadioSourceKind source_kind, int* native_format_out) {
    if (!opts || !native_format_out) {
        return -1;
    }
    int native_format = rtl_device_get_native_sample_format(rtl_device_handle);
    if (native_format != DSD_IQ_FORMAT_CU8 && native_format != DSD_IQ_FORMAT_CF32) {
        LOG_ERROR("IQ capture unsupported for active backend format.\n");
        return -1;
    }
    if (source_kind != RADIO_SOURCE_SOAPY && opts->iq_capture_format == DSD_IQ_FORMAT_CF32) {
        LOG_ERROR("--iq-capture-format cf32 is only supported for Soapy CF32 capture.\n");
        return -1;
    }
    if ((int)opts->iq_capture_format != native_format) {
        LOG_ERROR("Requested IQ capture format does not match active backend stream format.\n");
        return -1;
    }
    *native_format_out = native_format;
    return 0;
}

static int
stream_open_fill_capture_writer_config(const dsd_opts* opts, RadioSourceKind source_kind, int native_format,
                                       dsd_iq_capture_config* cfg, char* err_buf, size_t err_buf_size) {
    if (!opts || !cfg || !err_buf || err_buf_size == 0) {
        return -1;
    }
    char data_path[2048];
    char meta_path[2048];
    int rc = dsd_iq_capture_derive_paths(opts->iq_capture_path, data_path, sizeof(data_path), meta_path,
                                         sizeof(meta_path), err_buf, err_buf_size);
    if (rc != DSD_IQ_OK) {
        LOG_ERROR("Failed to resolve IQ capture paths: %s\n", err_buf[0] ? err_buf : "invalid path");
        return -1;
    }
    DSD_SNPRINTF(cfg->data_path, sizeof(cfg->data_path), "%s", data_path);
    DSD_SNPRINTF(cfg->metadata_path, sizeof(cfg->metadata_path), "%s", meta_path);
    cfg->format = (dsd_iq_sample_format)native_format;
    if (capture_stage_for_format(native_format, cfg->capture_stage, sizeof(cfg->capture_stage)) != 0) {
        LOG_ERROR("Failed to map IQ capture stage for active backend.\n");
        return -1;
    }
    cfg->sample_rate_hz = load_dongle_rate();
    cfg->center_frequency_hz = (uint64_t)opts->rtlsdr_center_freq;
    cfg->capture_center_frequency_hz = (uint64_t)load_dongle_frequency();
    cfg->ppm = load_dongle_ppm_error();
    cfg->tuner_gain_tenth_db = rtl_device_get_tuner_gain(rtl_device_handle);
    cfg->rtl_dsp_bw_khz = opts->rtl_dsp_bw_khz;
    cfg->base_decimation = (uint32_t)((demod.downsample_passes > 0) ? (1U << demod.downsample_passes) : 1U);
    cfg->post_downsample = (uint32_t)((demod.post_downsample > 0) ? demod.post_downsample : 1);
    cfg->demod_rate_hz = (uint32_t)demod.rate_out;
    cfg->offset_tuning_enabled = dongle.offset_tuning ? 1 : 0;
    cfg->fs4_shift_enabled = (!dongle.offset_tuning && !disable_fs4_shift) ? 1 : 0;
    cfg->combine_rotate_enabled = combine_rotate_enabled ? 1 : 0;
    cfg->muted_bytes_excluded = 1;
    DSD_SNPRINTF(cfg->source_backend, sizeof(cfg->source_backend), "%s", capture_backend_name(source_kind));
    capture_backend_args(opts, source_kind, cfg->source_args, sizeof(cfg->source_args));
    cfg->max_bytes = opts->iq_capture_max_bytes;
    cfg->drop_warning_cb = capture_drop_warning_log;
    return 0;
}

static int
stream_open_capture_writer(dsd_opts* opts, RadioSourceKind source_kind) {
    if (!opts || !opts->iq_capture_requested || !rtl_device_handle) {
        return 0;
    }

    int native_format = 0;
    if (stream_open_validate_capture_writer_request(opts, source_kind, &native_format) != 0) {
        return -1;
    }

    dsd_iq_capture_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err_buf[256];
    if (stream_open_fill_capture_writer_config(opts, source_kind, native_format, &cfg, err_buf, sizeof(err_buf)) != 0) {
        return -1;
    }

    dsd_iq_capture_writer* writer = NULL;
    int rc = dsd_iq_capture_open(&cfg, &writer, err_buf, sizeof(err_buf));
    if (rc != DSD_IQ_OK || !writer) {
        LOG_ERROR("Failed to open IQ capture writer: %s\n", err_buf[0] ? err_buf : "unknown error");
        return -1;
    }

    g_iq_capture_writer = writer;
    rtl_device_set_iq_capture_writer(rtl_device_handle, writer);
    return 0;
}

static void
stream_abort_capture_writer(void) {
    if (!g_iq_capture_writer) {
        return;
    }
    if (rtl_device_handle) {
        rtl_device_set_iq_capture_writer(rtl_device_handle, NULL);
    }
    dsd_iq_capture_abort(g_iq_capture_writer);
    g_iq_capture_writer = NULL;
}

static void
stream_close_capture_writer(void) {
    if (!g_iq_capture_writer) {
        return;
    }
    if (rtl_device_handle) {
        rtl_device_set_iq_capture_writer(rtl_device_handle, NULL);
    }
    dsd_iq_capture_final_stats stats = {};
    stats.input_ring_drops = input_ring.producer_drops.load(std::memory_order_acquire);
    stats.retune_count = rtl_device_get_capture_retune_count(rtl_device_handle);
    dsd_iq_capture_close(g_iq_capture_writer, &stats);
    g_iq_capture_writer = NULL;
}

static int
stream_prepare_internals(const dsd_opts* opts) {
    if (!opts) {
        return -1;
    }

    if (g_stream) {
        if (g_stream->replay_eof_sync_inited) {
            (void)dsd_cond_destroy(&g_stream->replay_eof_cond);
            (void)dsd_mutex_destroy(&g_stream->replay_eof_m);
        }
        free(g_stream);
        g_stream = NULL;
    }

    g_stream = static_cast<struct RtlSdrInternals*>(calloc(1, sizeof(struct RtlSdrInternals)));
    if (!g_stream) {
        return -1;
    }

    g_stream->device = rtl_device_handle;
    g_stream->dongle = &dongle;
    g_stream->demod = &demod;
    g_stream->output = &output;
    g_stream->controller = &controller;
    g_stream->input_ring = &input_ring;
    g_stream->udp_ctrl_ptr = &g_udp_ctrl;
    g_stream->opts = opts;
    g_stream->should_exit.store(0, std::memory_order_release);
    g_stream->controller_thread_started.store(0, std::memory_order_release);
    g_stream->demod_thread_started.store(0, std::memory_order_release);
    g_stream->async_started.store(0, std::memory_order_release);

    if (dsd_mutex_init(&g_stream->replay_eof_m) != 0) {
        free(g_stream);
        g_stream = NULL;
        return -1;
    }
    if (dsd_cond_init(&g_stream->replay_eof_cond) != 0) {
        (void)dsd_mutex_destroy(&g_stream->replay_eof_m);
        free(g_stream);
        g_stream = NULL;
        return -1;
    }
    g_stream->replay_eof_sync_inited = 1;
    stream_reset_replay_eof_state(g_stream);

    stream_refresh_watermark_for_current_rate();

    return 0;
}

static void
stream_destroy_internals(void) {
    if (!g_stream) {
        return;
    }
    if (g_stream->replay_eof_sync_inited) {
        (void)dsd_cond_destroy(&g_stream->replay_eof_cond);
        (void)dsd_mutex_destroy(&g_stream->replay_eof_m);
    }
    free(g_stream);
    g_stream = NULL;
}

/* Forward decls for auto-PPM status helpers */
extern "C" int dsd_rtl_stream_auto_ppm_get_status(int* enabled, double* snr_db, double* df_hz, double* est_ppm,
                                                  int* last_dir, int* cooldown, int* locked);
extern "C" int dsd_rtl_stream_auto_ppm_training_active(void);
extern "C" void dsd_rtl_stream_set_auto_ppm(int onoff);
extern "C" int dsd_rtl_stream_get_auto_ppm(void);

/* Option B: Perform a short auto-PPM pre-training window at startup before returning control,
   so trunking/hunt logic begins after a stable PPM lock when possible. */

namespace {
struct stream_open_persist_state {
    int use;
    int cqpsk_enable;
    int fll_enable;
    int ted_enable;
    float ted_gain;
    int ted_gain_is_set;
    int ted_force;
};
} // namespace

static int
stream_open_prepare_replay_config(dsd_opts* opts, RadioSourceKind source_kind, dsd_iq_replay_config* replay_cfg,
                                  int* replay_cfg_loaded) {
    if (!opts || !replay_cfg || !replay_cfg_loaded) {
        return -1;
    }
    *replay_cfg_loaded = 0;
    if (source_kind != RADIO_SOURCE_IQ_REPLAY) {
        return 0;
    }
    const char* replay_path = radio_source_replay_path(opts);
    if (!replay_path || replay_path[0] == '\0') {
        LOG_ERROR("IQ replay path is empty.\n");
        return -1;
    }
    char err_buf[256];
    int rc = dsd_iq_replay_read_metadata(replay_path, replay_cfg, err_buf, sizeof(err_buf));
    if (rc != DSD_IQ_OK) {
        LOG_ERROR("IQ replay metadata error: %s\n", err_buf[0] ? err_buf : "unknown error");
        return -1;
    }
    replay_cfg->loop = opts->iq_replay_loop ? 1 : 0;
    replay_cfg->realtime = (opts->iq_replay_rate_mode == DSD_IQ_REPLAY_RATE_REALTIME) ? 1 : 0;
    opts->rtlsdr_center_freq = (long int)replay_cfg->center_frequency_hz;
    opts->rtlsdr_ppm_error = replay_cfg->ppm;
    if (replay_cfg->tuner_gain_tenth_db > 0) {
        opts->rtl_gain_value = replay_cfg->tuner_gain_tenth_db / 10;
    }
    if (replay_cfg->rtl_dsp_bw_khz > 0) {
        opts->rtl_dsp_bw_khz = replay_cfg->rtl_dsp_bw_khz;
    }
    *replay_cfg_loaded = 1;
    return 0;
}

static void
stream_open_reset_auto_ppm_state(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    g_auto_ppm_controller.reset(load_dongle_ppm_error(), opts->rtlsdr_center_freq);
    g_auto_ppm_enabled.store(0, std::memory_order_relaxed);
    g_auto_ppm_locked.store(0, std::memory_order_relaxed);
    g_auto_ppm_training.store(0, std::memory_order_relaxed);
    g_auto_ppm_lock_ppm.store(0, std::memory_order_relaxed);
    g_auto_ppm_lock_snr_db.store(-100.0, std::memory_order_relaxed);
    g_auto_ppm_lock_df_hz.store(0.0, std::memory_order_relaxed);
    g_auto_ppm_snr_db.store(-100.0, std::memory_order_relaxed);
    g_auto_ppm_df_hz.store(0.0, std::memory_order_relaxed);
    g_auto_ppm_est_ppm.store(0.0, std::memory_order_relaxed);
    g_auto_ppm_last_dir.store(0, std::memory_order_relaxed);
    g_auto_ppm_cooldown.store(0, std::memory_order_relaxed);
}

static void
stream_open_warn_low_dsp_bw(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    if (opts_has_12k5_or_cqpsk_bw_mode(opts) && opts->rtl_dsp_bw_khz > 0 && opts->rtl_dsp_bw_khz < 16) {
        static int warned_16 = 0;
        if (!warned_16) {
            warned_16 = 1;
            LOG_WARNING(
                "RTL DSP-BW %dkHz is too low for active 12.5kHz/CQPSK modes; use at least 16kHz, preferably 24/48kHz, "
                "to keep the modulation off the filter skirt.\n",
                opts->rtl_dsp_bw_khz);
        }
        return;
    }
    if ((opts->frame_dmr == 1 || opts->frame_p25p2 == 1 || opts->mod_qpsk == 1) && opts->rtl_dsp_bw_khz > 0
        && opts->rtl_dsp_bw_khz < 24) {
        static int warned_24 = 0;
        if (!warned_24) {
            warned_24 = 1;
            LOG_WARNING("RTL DSP-BW %dkHz is marginal for DMR/P25P2/CQPSK; try 48kHz or at least 24kHz for more "
                        "reliable timing and data decode.\n",
                        opts->rtl_dsp_bw_khz);
        }
    }
}

static short int
stream_open_volume_multiplier(const dsd_opts* opts) {
    int vm = opts ? opts->rtl_volume_multiplier : 1;
    if (vm < 1 || vm > 3) {
        vm = 1;
    }
    return (short int)vm;
}

static int
stream_open_init_pipeline(const dsd_opts* opts, int demod_base_rate_hz) {
    dongle_init(&dongle);
    rtl_demod_init_for_mode(&demod, &output, opts, demod_base_rate_hz);
    output_init(&output);
    if (!output.buffer) {
        LOG_ERROR("Output ring buffer allocation failed.\n");
        return -1;
    }
    output_init(&monitor_output);
    if (monitor_output.buffer) {
        monitor_output.rate = demod.rate_out;
    } else {
        LOG_WARNING("Monitor ring buffer allocation failed; RTL monitor tap disabled.\n");
    }
    if (input_ring_init(&input_ring, (size_t)(MAXIMUM_BUF_LENGTH * 8)) != 0) {
        LOG_ERROR("Failed to initialize input ring buffer.\n");
        return -1;
    }
    input_ring_enable_space_notify(&input_ring, 0);
    controller_init(&controller);
    rtl_demod_config_from_env_and_opts(&demod, opts);
    rtl_demod_select_defaults_for_mode(&demod, opts, &output);
    if (stream_prepare_internals(opts) != 0) {
        LOG_ERROR("Failed to initialize RTL stream internals.\n");
        return -1;
    }
    return 0;
}

static void
stream_open_apply_persist_state(const stream_open_persist_state* persist) {
    if (!persist || !persist->use) {
        return;
    }
    demod.cqpsk_enable = persist->cqpsk_enable ? 1 : 0;
    demod.fll_enabled = persist->fll_enable ? 1 : 0;
    demod.ted_enabled = persist->ted_enable ? 1 : 0;
    if (persist->ted_gain > 0.0f) {
        demod.ted_gain = persist->ted_gain;
    }
    demod.ted_gain_is_set = persist->ted_gain_is_set ? 1 : 0;
    demod.ted_force = persist->ted_force ? 1 : 0;
}

static void
stream_open_enable_default_autogain(const dsd_opts* opts) {
    if (!opts || opts->rtl_gain_value > 0) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->tuner_autogain_enable) {
        g_tuner_autogain_on.store(1, std::memory_order_relaxed);
    }
}

static int
stream_open_validate_scan_inputs(void) {
    if (controller.freq_len == 0) {
        LOG_ERROR("Please specify a frequency.\n");
        return -1;
    }
    if (controller.freq_len >= FREQUENCIES_LIMIT) {
        LOG_ERROR("Too many channels, maximum %i.\n", FREQUENCIES_LIMIT);
        return -1;
    }
    if (controller.freq_len > 1 && demod.channel_squelch_level == 0.0f) {
        LOG_ERROR("Please specify a squelch level.  Required for scanning multiple frequencies.\n");
        return -1;
    }
    return 0;
}

static void
stream_open_fill_replay_eof_state(struct rtl_replay_eof_state* eof_state) {
    if (!eof_state) {
        return;
    }
    *eof_state = {};
    if (!g_stream) {
        return;
    }
    eof_state->stream_exit_flag = &g_stream->should_exit;
    eof_state->replay_input_eof = &g_stream->replay_input_eof;
    eof_state->replay_input_drained = &g_stream->replay_input_drained;
    eof_state->replay_demod_drained = &g_stream->replay_demod_drained;
    eof_state->replay_output_drained = &g_stream->replay_output_drained;
    eof_state->replay_forced_stop = &g_stream->replay_forced_stop;
    eof_state->replay_last_submit_gen = &g_stream->replay_last_submit_gen;
    eof_state->replay_last_submit_gen_at_eof = &g_stream->replay_last_submit_gen_at_eof;
    eof_state->replay_last_consume_gen = &g_stream->replay_last_consume_gen;
    eof_state->eof_m = &g_stream->replay_eof_m;
    eof_state->eof_cond = &g_stream->replay_eof_cond;
    eof_state->on_input_drained = rtl_replay_on_input_drained;
    eof_state->on_retune_event = rtl_replay_on_retune_event;
    eof_state->on_mute_event = rtl_replay_on_mute_event;
    eof_state->on_reset_event = rtl_replay_on_reset_event;
    eof_state->on_loop_restart = rtl_replay_on_loop_restart;
    eof_state->eof_user = g_stream;
    eof_state->event_user = g_stream;
}

static int
stream_open_open_device_rtltcp(const dsd_opts* opts) {
    int autotune = opts->rtltcp_autotune;
    if (!autotune) {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->tcp_autotune_enable) {
            autotune = 1;
        }
    }
    rtl_device_handle = rtl_device_create_tcp(opts->rtltcp_hostname, opts->rtltcp_portno, &input_ring,
                                              combine_rotate_enabled, autotune);
    if (!rtl_device_handle) {
        LOG_ERROR("Failed to connect rtl_tcp at %s:%d.\n", opts->rtltcp_hostname, opts->rtltcp_portno);
        return -1;
    }
    LOG_INFO("Using rtl_tcp source %s:%d.\n", opts->rtltcp_hostname, opts->rtltcp_portno);
    rtl_device_print_offset_capability(rtl_device_handle);
    return 0;
}

static int
stream_open_open_device_replay(const dsd_iq_replay_config* replay_cfg, int replay_cfg_loaded) {
    if (!replay_cfg_loaded || !replay_cfg) {
        LOG_ERROR("IQ replay metadata is unavailable.\n");
        return -1;
    }
    struct rtl_replay_eof_state eof_state = {};
    stream_open_fill_replay_eof_state(&eof_state);
    rtl_device_handle = rtl_device_create_iq_replay(replay_cfg, &input_ring, &eof_state);
    if (!rtl_device_handle) {
        LOG_ERROR("Failed to initialize IQ replay source.\n");
        return -1;
    }
    LOG_INFO("Using IQ replay source: %s.\n", replay_cfg->metadata_path);
    rtl_device_print_offset_capability(rtl_device_handle);
    return 0;
}

static int
stream_open_open_device_soapy(const dsd_opts* opts) {
    const char* soapy_args = radio_source_soapy_args(opts);
    rtl_device_handle = rtl_device_create_soapy(soapy_args, &input_ring, combine_rotate_enabled);
    if (!rtl_device_handle) {
        if (soapy_args[0] != '\0') {
            LOG_ERROR("Failed to open SoapySDR device with args: %s.\n", soapy_args);
        } else {
            LOG_ERROR("Failed to open SoapySDR device.\n");
        }
        return -1;
    }
    if (soapy_args[0] != '\0') {
        LOG_INFO("Using SoapySDR source: %s.\n", soapy_args);
    } else {
        LOG_INFO("Using SoapySDR default source.\n");
    }
    rtl_device_print_offset_capability(rtl_device_handle);
    struct rtl_soapy_config soapy_cfg = {};
    soapy_cfg.profile = opts->soapy_profile;
    soapy_cfg.antenna = opts->soapy_antenna;
    soapy_cfg.clock_source = opts->soapy_clock;
    soapy_cfg.gains = opts->soapy_gains;
    soapy_cfg.stream_format = opts->soapy_stream_format;
    soapy_cfg.bandwidth_hz = opts->soapy_bandwidth_hz;
    int soapy_cfg_rc = rtl_device_configure_soapy(rtl_device_handle, &soapy_cfg);
    if (soapy_cfg_rc != 0) {
        LOG_ERROR("Failed to apply SoapySDR profile/configuration (rc=%d).\n", soapy_cfg_rc);
        rtl_device_destroy(rtl_device_handle);
        rtl_device_handle = NULL;
        stream_destroy_internals();
        return -1;
    }
    return 0;
}

static int
stream_open_open_device_usb(void) {
    rtl_device_handle = rtl_device_create(dongle.dev_index, &input_ring, combine_rotate_enabled);
    if (!rtl_device_handle) {
        LOG_ERROR("Failed to open rtlsdr device %d.\n", dongle.dev_index);
        return -1;
    }
    LOG_INFO("Using RTLSDR Device Index: %d. \n", dongle.dev_index);
    rtl_device_print_offset_capability(rtl_device_handle);
    return 0;
}

static int
stream_open_open_device(RadioSourceKind source_kind, const dsd_opts* opts, const dsd_iq_replay_config* replay_cfg,
                        int replay_cfg_loaded) {
    if (!opts) {
        return -1;
    }
    int rc = -1;
    if (source_kind == RADIO_SOURCE_RTL_TCP) {
        rc = stream_open_open_device_rtltcp(opts);
    } else if (source_kind == RADIO_SOURCE_IQ_REPLAY) {
        rc = stream_open_open_device_replay(replay_cfg, replay_cfg_loaded);
    } else if (source_kind == RADIO_SOURCE_SOAPY) {
        rc = stream_open_open_device_soapy(opts);
    } else {
        rc = stream_open_open_device_usb();
    }
    if (rc != 0) {
        return rc;
    }
    if (g_stream) {
        g_stream->device = rtl_device_handle;
    }
    return 0;
}

static int
stream_open_parse_if_gain_tenth_db(const char* gain_text, int* gain_tenth_out) {
    if (!gain_text || !gain_tenth_out) {
        return -1;
    }
    char gbuf[64];
    DSD_SNPRINTF(gbuf, sizeof gbuf, "%s", gain_text);
    size_t gl = strlen(gbuf);
    if (gl >= 2 && (gbuf[gl - 1] == 'B' || gbuf[gl - 1] == 'b')) {
        gbuf[gl - 1] = '\0';
        if (gl >= 3 && (gbuf[gl - 2] == 'D' || gbuf[gl - 2] == 'd')) {
            gbuf[gl - 2] = '\0';
        }
    }
    double gain_db = 0.0;
    if (!parse_double_atof_compat(gbuf, &gain_db)) {
        gain_db = 0.0;
    }
    if (strchr(gbuf, '.')) {
        *gain_tenth_out = (int)lrint(gain_db * 10.0);
        return 0;
    }
    int gi = 0;
    if (!parse_int_atoi_compat(gbuf, &gi)) {
        gi = 0;
    }
    *gain_tenth_out = (abs(gi) > 90) ? gi : (gi * 10);
    return 0;
}

static void
stream_open_apply_if_gains_config(const char* gains) {
    if (!gains || gains[0] == '\0') {
        return;
    }
    char buf[1024];
    DSD_SNPRINTF(buf, sizeof buf, "%s", gains);
    char* save = NULL;
    for (char* tok = dsd_strtok_r(buf, ",; ", &save); tok; tok = dsd_strtok_r(NULL, ",; ", &save)) {
        char* colon = strchr(tok, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        int stage = 0;
        if (!parse_int_atoi_compat(tok, &stage)) {
            stage = 0;
        }
        if (stage < 0) {
            continue;
        }
        int gain_tenth = 0;
        stream_open_parse_if_gain_tenth_db(colon + 1, &gain_tenth);
        int rc = rtl_device_set_if_gain(rtl_device_handle, stage, gain_tenth);
        log_unsupported_control_if_needed("IF gain control", rc);
    }
}

static void
stream_open_apply_bias_tee(const dsd_opts* opts) {
    if (!opts || !opts->rtl_bias_tee) {
        return;
    }
    int rc = rtl_device_set_bias_tee(rtl_device_handle, 1);
    log_unsupported_control_if_needed("Bias tee control", rc);
}

static void
stream_open_apply_direct_sampling(const dsdneoRuntimeConfig* cfg) {
    if (!cfg || !cfg->rtl_direct_is_set) {
        return;
    }
    int mode = cfg->rtl_direct_mode;
    int rc = rtl_device_set_direct_sampling(rtl_device_handle, mode);
    if (rc == 0) {
        dongle.direct_sampling = mode;
    } else {
        dongle.direct_sampling = 0;
        log_unsupported_control_if_needed("Direct sampling control", rc);
    }
}

static void
stream_open_apply_offset_tuning(const dsdneoRuntimeConfig* cfg) {
    if (!cfg || !cfg->rtl_offset_tuning_is_set) {
        return;
    }
    int on = cfg->rtl_offset_tuning_enable ? 1 : 0;
    int rc = rtl_device_set_offset_tuning_enabled(rtl_device_handle, on);
    if (rc == 0) {
        dongle.offset_tuning = on ? 1 : 0;
    } else {
        dongle.offset_tuning = 0;
        log_unsupported_control_if_needed("Offset tuning control", rc);
    }
}

static void
stream_open_apply_xtal_control(const dsdneoRuntimeConfig* cfg) {
    if (!cfg || (!cfg->rtl_xtal_hz_is_set && !cfg->tuner_xtal_hz_is_set)) {
        return;
    }
    uint32_t rtl_xtal_hz = cfg->rtl_xtal_hz_is_set ? (uint32_t)cfg->rtl_xtal_hz : 0U;
    uint32_t tuner_xtal_hz = cfg->tuner_xtal_hz_is_set ? (uint32_t)cfg->tuner_xtal_hz : 0U;
    int rc = rtl_device_set_xtal_freq(rtl_device_handle, rtl_xtal_hz, tuner_xtal_hz);
    log_unsupported_control_if_needed("Xtal frequency control", rc);
}

static void
stream_open_apply_testmode(const dsdneoRuntimeConfig* cfg) {
    if (!cfg || !cfg->rtl_testmode_is_set) {
        return;
    }
    int on = cfg->rtl_testmode_enable ? 1 : 0;
    int rc = rtl_device_set_testmode(rtl_device_handle, on);
    log_unsupported_control_if_needed("Test mode control", rc);
}

static void
stream_open_apply_runtime_controls(const dsd_opts* opts) {
    stream_open_apply_bias_tee(opts);
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return;
    }
    stream_open_apply_direct_sampling(cfg);
    stream_open_apply_offset_tuning(cfg);
    stream_open_apply_xtal_control(cfg);
    stream_open_apply_testmode(cfg);
    if (cfg->rtl_if_gains_is_set && cfg->rtl_if_gains[0] != '\0') {
        stream_open_apply_if_gains_config(cfg->rtl_if_gains);
    }
}

static void
stream_open_apply_deemphasis_from_config(void) {
    if (!demod.deemph) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    double tau_s = 75e-6;
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
    if (!demod.deemph) {
        return;
    }
    double Fs = (double)demod.rate_out;
    if (Fs < 1.0) {
        Fs = 1.0;
    }
    double a = exp(-1.0 / (Fs * tau_s));
    double alpha = 1.0 - a;
    int coef_q15 = (int)lrint(alpha * (double)(1 << 15));
    if (coef_q15 < 1) {
        coef_q15 = 1;
    } else if (coef_q15 > ((1 << 15) - 1)) {
        coef_q15 = ((1 << 15) - 1);
    }
    demod.deemph_a = (float)((double)coef_q15 / (double)(1 << 15));
}

static void
stream_open_apply_audio_lpf_from_config(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    demod.audio_lpf_enable = 0;
    demod.audio_lpf_alpha = 0;
    demod.audio_lpf_state = 0;
    if (!cfg || !cfg->audio_lpf_is_set || cfg->audio_lpf_disable || cfg->audio_lpf_cutoff_hz <= 0) {
        return;
    }
    int cutoff_hz = cfg->audio_lpf_cutoff_hz;
    if (cutoff_hz < 100) {
        cutoff_hz = 100;
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
    demod.audio_lpf_alpha = (float)a;
    demod.audio_lpf_enable = 1;
    const char* approx = dsd_unicode_or_ascii("≈", "~");
    LOG_INFO("Audio LPF enabled: fc%s%d Hz, alpha=%.4f\n", approx, cutoff_hz, demod.audio_lpf_alpha);
}

static void
stream_open_apply_requested_ppm(const dsd_opts* opts) {
    if (!opts) {
        return;
    }
    RtlRequestedPpmState initial_ppm_request = snapshot_requested_ppm_state(opts);
    int ppm_rc = apply_ppm_setting(initial_ppm_request.ppm);
    if (ppm_rc == 0) {
        store_dongle_ppm_error(initial_ppm_request.ppm);
    } else {
        note_failed_ppm_request(initial_ppm_request.ppm, initial_ppm_request.request_id, load_dongle_ppm_error(),
                                ppm_rc);
    }
    g_auto_ppm_controller.reset(load_dongle_ppm_error(), opts->rtlsdr_center_freq);
}

static int
stream_open_apply_controller_settings(RadioSourceKind source_kind, const dsd_opts* opts,
                                      const dsd_iq_replay_config* replay_cfg, int replay_cfg_loaded) {
    if (!opts) {
        return -1;
    }
    if (source_kind == RADIO_SOURCE_IQ_REPLAY) {
        if (!replay_cfg_loaded || controller_apply_replay_settings(&controller, opts, replay_cfg) != 0) {
            LOG_ERROR("Failed to apply replay stream settings.\n");
            return -1;
        }
        input_ring_enable_space_notify(&input_ring, 1);
        return 0;
    }
    if (controller.freq_len == 0) {
        controller.freqs[controller.freq_len] = 446000000;
        controller.freq_len++;
    }
    if (controller_apply_initial_settings(&controller, opts) != 0) {
        LOG_ERROR("Failed to apply initial tuner settings.\n");
        return -1;
    }
    stream_refresh_watermark_for_current_rate();
    input_ring_enable_space_notify(&input_ring, 0);
    return 0;
}

static void
stream_open_configure_resampler_chain(void) {
    if (demod.resamp_target_hz <= 0) {
        demod.resamp_enabled = 0;
        return;
    }
    int target = demod.resamp_target_hz;
    int inRate = demod.rate_out > 0 ? demod.rate_out : rtl_dsp_bw_hz;
    if (target == inRate) {
        demod.resamp_enabled = 0;
        demod.resamp_L = 1;
        demod.resamp_M = 1;
        LOG_INFO("Resampler bypassed: input rate %d Hz matches target.\n", inRate);
        return;
    }
    int g = gcd_int(inRate, target);
    int L = target / g;
    int M = inRate / g;
    if (L < 1) {
        L = 1;
    }
    if (M < 1) {
        M = 1;
    }
    int scale = (L + M - 1) / M;
    if (scale > 12) {
        LOG_WARNING("Resampler ratio too large (L=%d,M=%d). Disabling resampler.\n", L, M);
        demod.resamp_enabled = 0;
        return;
    }
    demod.resamp_enabled = 1;
    resamp_design(&demod, L, M);
    LOG_INFO("Rational resampler configured: %d -> %d Hz (L=%d,M=%d).\n", inRate, target, L, M);
}

static void
stream_open_update_output_rates(void) {
    if (demod.resamp_enabled && demod.resamp_target_hz > 0) {
        output.rate = demod.resamp_target_hz;
        LOG_INFO("Output rate set to %d Hz via resampler.\n", output.rate);
    } else {
        output.rate = demod.rate_out;
    }
    if (monitor_output.buffer) {
        monitor_output.rate = demod.rate_out;
    }
}

static void
stream_open_log_rate_chain_summary(void) {
    unsigned int capture_hz = load_dongle_rate();
    int base_decim = (demod.downsample_passes > 0) ? (1 << demod.downsample_passes) : 1;
    int post = (demod.post_downsample > 0) ? demod.post_downsample : 1;
    unsigned int demod_hz = (unsigned int)demod.rate_out;
    unsigned int out_hz =
        demod.resamp_enabled && demod.resamp_target_hz > 0 ? (unsigned int)demod.resamp_target_hz : demod_hz;
    if (demod.resamp_enabled) {
        LOG_INFO("Rate chain: capture=%u Hz, base_decim=%d, post=%d -> demod=%u Hz; resampler L/M=%d/%d -> output=%u "
                 "Hz.\n",
                 capture_hz, base_decim, post, demod_hz, demod.resamp_L, demod.resamp_M, out_hz);
    } else {
        LOG_INFO("Rate chain: capture=%u Hz, base_decim=%d, post=%d -> demod=%u Hz; resampler bypassed -> output=%u "
                 "Hz.\n",
                 capture_hz, base_decim, post, demod_hz, out_hz);
    }
    if (out_hz > 0) {
        int sps_p25p1 = (int)((out_hz + 2400) / 4800);
        int sps_p25p2 = (int)((out_hz + 3000) / 6000);
        int sps_nxdn48 = (int)((out_hz + 1200) / 2400);
        const char* approx = dsd_unicode_or_ascii("≈", "~");
        LOG_INFO("Derived SPS (@%u Hz): P25P1%s%d, P25P2%s%d, NXDN48%s%d.\n", out_hz, approx, sps_p25p1, approx,
                 sps_p25p2, approx, sps_nxdn48);
        if ((sps_p25p1 < 8 || sps_p25p1 > 12) || (sps_p25p2 < 6 || sps_p25p2 > 10)
            || (sps_nxdn48 < 16 || sps_nxdn48 > 24)) {
            LOG_WARNING("Output rate %u Hz implies atypical SPS; digital decoders assume ~48k. Consider enabling "
                        "resampler to 48000 Hz.\n",
                        out_hz);
        }
    }
}

static int
stream_open_rtltcp_prebuffer_ms(void) {
    int pre_ms = 1000;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg) {
        pre_ms = cfg->tcp_prebuf_ms;
    }
    return pre_ms;
}

static size_t
stream_open_rtltcp_desired_prebuffer(uint32_t sample_rate_hz, int pre_ms) {
    size_t desired_prebuf = (size_t)((double)sample_rate_hz * 2.0 * ((double)pre_ms / 1000.0));
    if (desired_prebuf < 16384U) {
        desired_prebuf = 16384U;
    }
    return desired_prebuf;
}

static void
stream_open_rtltcp_resize_ring_if_needed(size_t min_capacity, int pre_ms) {
    if (min_capacity <= input_ring.capacity) {
        return;
    }
    float* nb = static_cast<float*>(dsd_neo_aligned_malloc(min_capacity * sizeof(float)));
    if (!nb) {
        LOG_WARNING("rtltcp: allocation for %zu samples (%.2f MiB) failed; using existing ring (%zu).\n", min_capacity,
                    (double)min_capacity * sizeof(float) / (1024.0 * 1024.0), input_ring.capacity);
        return;
    }
    if (input_ring.buffer) {
        dsd_neo_aligned_free(input_ring.buffer);
    }
    input_ring.buffer = nb;
    input_ring.capacity = min_capacity;
    input_ring.head.store(0);
    input_ring.tail.store(0);
    LOG_INFO("rtltcp resized input ring to %zu samples (%.2f MiB) for ~%d ms prebuffer.\n", input_ring.capacity,
             (double)input_ring.capacity * sizeof(float) / (1024.0 * 1024.0), pre_ms);
}

static size_t
stream_open_rtltcp_target_prebuffer(size_t desired_prebuf) {
    size_t target = desired_prebuf;
    target = std::max(target, (size_t)16384U);
    target = std::min(target, input_ring.capacity / 2U);
    return target;
}

static void
stream_open_rtltcp_wait_for_prebuffer(size_t target) {
    int waited_ms = 0;
    while (!exitflag && input_ring_used(&input_ring) < target && waited_ms < 2000) {
        dsd_sleep_ms(2);
        waited_ms += 2;
    }
}

static int
stream_open_start_rtltcp_pipeline(const dsd_opts* opts) {
    UNUSED(opts);
    int pre_ms = stream_open_rtltcp_prebuffer_ms();

    uint32_t sample_rate_hz = load_dongle_rate();
    size_t desired_prebuf = stream_open_rtltcp_desired_prebuffer(sample_rate_hz, pre_ms);
    size_t min_capacity = desired_prebuf * 2;
    stream_open_rtltcp_resize_ring_if_needed(min_capacity, pre_ms);

    size_t target = stream_open_rtltcp_target_prebuffer(desired_prebuf);
    double target_sec = (sample_rate_hz > 0) ? ((double)target / (2.0 * (double)sample_rate_hz)) : 0.0;
    LOG_INFO("rtltcp prebuffer target: %zu samples (%.3f s at %u Hz).\n", target, target_sec, (unsigned)sample_rate_hz);

    LOG_INFO("Starting RTL async read (rtltcp prebuffer %d ms)...\n", pre_ms);
    if (rtl_device_start_async(rtl_device_handle, (uint32_t)ACTUAL_BUF_LENGTH) != 0) {
        LOG_ERROR("Failed to start rtl_tcp async reader.\n");
        return -1;
    }
    if (g_stream) {
        g_stream->async_started.store(1, std::memory_order_release);
    }

    stream_open_rtltcp_wait_for_prebuffer(target);
    LOG_INFO("rtltcp prebuffer filled: %zu/%zu samples in ring.\n", input_ring_used(&input_ring), target);

    if (dsd_thread_create(&controller.thread, controller_thread_retune_loop, &controller) == 0 && g_stream) {
        g_stream->controller_thread_started.store(1, std::memory_order_release);
    }
    if (dsd_thread_create(&demod.thread, demod_thread_fn, &demod) == 0 && g_stream) {
        g_stream->demod_thread_started.store(1, std::memory_order_release);
    }
    if (port != 0) {
        g_udp_ctrl = udp_control_start_bound(udp_control_bindaddr, port,
                                             [](uint32_t new_freq_hz) { schedule_manual_retune(new_freq_hz); });
        if (!g_udp_ctrl) {
            LOG_ERROR("Failed to start RTL UDP retune control on %s:%u\n", udp_control_bindaddr, (unsigned)port);
        }
    }
    return 0;
}

static int
stream_open_start_replay_pipeline(void) {
    if (dsd_thread_create(&demod.thread, demod_thread_fn, &demod) != 0) {
        LOG_ERROR("Failed to start replay demod thread.\n");
        return -1;
    }
    if (g_stream) {
        g_stream->demod_thread_started.store(1, std::memory_order_release);
    }
    LOG_INFO("Starting IQ replay reader...\n");
    if (rtl_device_start_async(rtl_device_handle, (uint32_t)ACTUAL_BUF_LENGTH) != 0) {
        LOG_ERROR("Failed to start replay reader thread.\n");
        if (g_stream) {
            g_stream->should_exit.store(1, std::memory_order_release);
        }
        safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
        safe_cond_signal(&output.ready, &output.ready_m);
        dsd_thread_join(demod.thread);
        if (g_stream) {
            g_stream->demod_thread_started.store(0, std::memory_order_release);
        }
        return -1;
    }
    if (g_stream) {
        g_stream->async_started.store(1, std::memory_order_release);
    }
    return 0;
}

static int
stream_open_start_capture_threads(RadioSourceKind source_kind, const dsd_opts* opts) {
    if (source_kind == RADIO_SOURCE_RTL_TCP) {
        return stream_open_start_rtltcp_pipeline(opts);
    }
    if (source_kind == RADIO_SOURCE_IQ_REPLAY) {
        return stream_open_start_replay_pipeline();
    }
    start_threads_and_async();
    return 0;
}

static int
stream_open_validate_source_selection(dsd_opts* opts, RadioSourceKind source_kind) {
    if (!opts) {
        return -1;
    }
    opts->iq_replay_active = (source_kind == RADIO_SOURCE_IQ_REPLAY) ? 1 : 0;
    if (source_kind == RADIO_SOURCE_IQ_REPLAY && opts->iq_capture_requested) {
        LOG_ERROR("IQ replay cannot be combined with IQ capture in the same run.\n");
        return -1;
    }
    return 0;
}

static int
stream_open_configure_pipeline_state(dsd_opts* opts, RadioSourceKind source_kind,
                                     const dsd_iq_replay_config* replay_cfg, int replay_cfg_loaded) {
    stream_open_reset_auto_ppm_state(opts);
    stream_open_persist_state persist = {};

    rtl_dsp_bw_hz = opts->rtl_dsp_bw_khz * 1000;
    stream_open_warn_low_dsp_bw(opts);
    volume_multiplier = stream_open_volume_multiplier(opts);
    if (stream_open_init_pipeline(opts, rtl_dsp_bw_hz) != 0) {
        return -1;
    }
    stream_open_apply_persist_state(&persist);
    stream_open_enable_default_autogain(opts);
    setup_initial_freq_and_rate(opts);
    if (!output.rate) {
        output.rate = demod.rate_out;
    }
    if (stream_open_validate_scan_inputs() != 0) {
        return -1;
    }
    if (controller.freq_len > 1) {
        demod.terminate_on_squelch = 0;
    }

    ACTUAL_BUF_LENGTH = lcm_post[demod.post_downsample] * DEFAULT_BUF_LENGTH;
    dongle.buf_len = (uint32_t)ACTUAL_BUF_LENGTH;
    if (stream_open_open_device(source_kind, opts, replay_cfg, replay_cfg_loaded) != 0) {
        return -1;
    }
    stream_open_apply_runtime_controls(opts);
    stream_open_apply_deemphasis_from_config();
    stream_open_apply_audio_lpf_from_config();

    int gain_rc = rtl_device_set_gain(rtl_device_handle, dongle.gain);
    log_unsupported_control_if_needed("Gain control", gain_rc);
    if (dongle.gain == AUTO_GAIN) {
        LOG_INFO("Setting RTL Autogain. \n");
    }
    stream_open_apply_requested_ppm(opts);

    if (stream_open_apply_controller_settings(source_kind, opts, replay_cfg, replay_cfg_loaded) != 0) {
        return -1;
    }
    stream_open_configure_resampler_chain();
    rtl_demod_maybe_refresh_ted_sps_after_rate_change(&demod, opts, &output);
    stream_open_update_output_rates();
    stream_open_log_rate_chain_summary();
    return 0;
}

static int
stream_open_start_io_pipeline(dsd_opts* opts, RadioSourceKind source_kind) {
    if (source_kind != RADIO_SOURCE_IQ_REPLAY) {
        (void)rtl_device_reset_buffer(rtl_device_handle);
        if (stream_open_capture_writer(opts, source_kind) != 0) {
            stream_abort_capture_writer();
            return -1;
        }
    }
    if (stream_open_start_capture_threads(source_kind, opts) != 0) {
        if (source_kind != RADIO_SOURCE_IQ_REPLAY) {
            stream_abort_capture_writer();
        }
        return -1;
    }
    return 0;
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
extern "C" int
dsd_rtl_stream_open(dsd_opts* opts) {
    if (!opts) {
        LOG_ERROR("RTL stream open: missing opts\n");
        return -1;
    }

    RadioSourceKind source_kind = detect_radio_source(opts);
    if (stream_open_validate_source_selection(opts, source_kind) != 0) {
        return -1;
    }

    dsd_iq_replay_config replay_cfg = {};
    int replay_cfg_loaded = 0;

    struct ReplayConfigCleanup {
        dsd_iq_replay_config* cfg;
        int* loaded;

        ~ReplayConfigCleanup() {
            if (cfg && loaded && *loaded) {
                dsd_iq_replay_config_clear(cfg);
                *loaded = 0;
            }
        }
    } replay_cfg_cleanup = {&replay_cfg, &replay_cfg_loaded};

    if (stream_open_prepare_replay_config(opts, source_kind, &replay_cfg, &replay_cfg_loaded) != 0) {
        return -1;
    }
    if (stream_open_configure_pipeline_state(opts, source_kind, &replay_cfg, replay_cfg_loaded) != 0) {
        return -1;
    }
    if (stream_open_start_io_pipeline(opts, source_kind) != 0) {
        return -1;
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
    rtl_stream_bump_output_generation();
    if (g_stream) {
        g_stream->replay_forced_stop.store(1, std::memory_order_release);
        g_stream->should_exit.store(1, std::memory_order_release);
        if (g_stream->opts) {
            dsd_opts* mutable_opts = const_cast<dsd_opts*>(g_stream->opts);
            mutable_opts->iq_replay_active = 0;
        }
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
    if (g_stream && g_stream->replay_eof_sync_inited) {
        dsd_mutex_lock(&g_stream->replay_eof_m);
        dsd_cond_broadcast(&g_stream->replay_eof_cond);
        dsd_mutex_unlock(&g_stream->replay_eof_m);
    }
    rtl_device_stop_async(rtl_device_handle);
    /* Wake any demod waits on both ready and space condition variables */
    safe_cond_signal(&demod.ready, &demod.ready_m);
    safe_cond_signal(&output.space, &output.ready_m);
    safe_cond_signal(&monitor_output.space, &monitor_output.ready_m);
    if (g_stream && g_stream->demod_thread_started.load(std::memory_order_acquire)) {
        dsd_thread_join(demod.thread);
        g_stream->demod_thread_started.store(0, std::memory_order_release);
    }
    /* Wake any consumers blocked on output.ready to finish */
    safe_cond_signal(&output.ready, &output.ready_m);
    safe_cond_signal(&monitor_output.ready, &monitor_output.ready_m);
    if (g_stream && g_stream->controller_thread_started.load(std::memory_order_acquire)) {
        dsd_thread_join(controller.thread);
        g_stream->controller_thread_started.store(0, std::memory_order_release);
    }
    stream_close_capture_writer();

    rtl_demod_cleanup(&demod);
    output_cleanup(&output);
    output_cleanup(&monitor_output);
    controller_cleanup(&controller);

    input_ring_destroy(&input_ring);

    rtl_device_destroy(rtl_device_handle);
    rtl_device_handle = NULL;

    rtl_perf_shutdown();
    stream_destroy_internals();
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
    rtl_stream_bump_output_generation();
    if (g_stream) {
        g_stream->replay_forced_stop.store(1, std::memory_order_release);
        g_stream->should_exit.store(1, std::memory_order_release);
        if (g_stream->opts) {
            dsd_opts* mutable_opts = const_cast<dsd_opts*>(g_stream->opts);
            mutable_opts->iq_replay_active = 0;
        }
    }
    if (g_udp_ctrl) {
        udp_control_stop(g_udp_ctrl);
        g_udp_ctrl = NULL;
    }
    safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
    safe_cond_signal(&controller.hop, &controller.hop_m);
    if (g_stream && g_stream->replay_eof_sync_inited) {
        dsd_mutex_lock(&g_stream->replay_eof_m);
        dsd_cond_broadcast(&g_stream->replay_eof_cond);
        dsd_mutex_unlock(&g_stream->replay_eof_m);
    }
    rtl_device_stop_async(rtl_device_handle);
    /* Wake any demod waits on both ready and space condition variables */
    safe_cond_signal(&demod.ready, &demod.ready_m);
    safe_cond_signal(&output.space, &output.ready_m);
    safe_cond_signal(&monitor_output.space, &monitor_output.ready_m);
    if (g_stream && g_stream->demod_thread_started.load(std::memory_order_acquire)) {
        dsd_thread_join(demod.thread);
        g_stream->demod_thread_started.store(0, std::memory_order_release);
    }
    /* Wake any consumers blocked on output.ready to finish */
    safe_cond_signal(&output.ready, &output.ready_m);
    safe_cond_signal(&monitor_output.ready, &monitor_output.ready_m);
    if (g_stream && g_stream->controller_thread_started.load(std::memory_order_acquire)) {
        dsd_thread_join(controller.thread);
        g_stream->controller_thread_started.store(0, std::memory_order_release);
    }
    stream_close_capture_writer();

    rtl_demod_cleanup(&demod);
    output_cleanup(&output);
    output_cleanup(&monitor_output);
    controller_cleanup(&controller);

    input_ring_destroy(&input_ring);
    rtl_device_destroy(rtl_device_handle);
    rtl_device_handle = NULL;
    rtl_perf_shutdown();
    stream_destroy_internals();
    return 0;
}

static uint64_t
auto_ppm_now_ms(void) {
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

static double
auto_ppm_pick_demod_snr_db(uint64_t now_ms) {
    const long long fresh_ms = 800;
    double gate_snr_db = -100.0;

    long long c4_ms = g_snr_c4fm_last_ms.load(std::memory_order_relaxed);
    int c4_src = g_snr_c4fm_src.load(std::memory_order_relaxed);
    if (c4_src == 1 && (long long)now_ms - c4_ms <= fresh_ms) {
        gate_snr_db = g_snr_c4fm_db.load(std::memory_order_relaxed);
    }

    long long qp_ms = g_snr_qpsk_last_ms.load(std::memory_order_relaxed);
    int qp_src = g_snr_qpsk_src.load(std::memory_order_relaxed);
    if (qp_src == 1 && (long long)now_ms - qp_ms <= fresh_ms) {
        double snr = g_snr_qpsk_db.load(std::memory_order_relaxed);
        if (snr > gate_snr_db) {
            gate_snr_db = snr;
        }
    }

    long long gf_ms = g_snr_gfsk_last_ms.load(std::memory_order_relaxed);
    int gf_src = g_snr_gfsk_src.load(std::memory_order_relaxed);
    if (gf_src == 1 && (long long)now_ms - gf_ms <= fresh_ms) {
        double snr = g_snr_gfsk_db.load(std::memory_order_relaxed);
        if (snr > gate_snr_db) {
            gate_snr_db = snr;
        }
    }

    return gate_snr_db;
}

static int
auto_ppm_effective_enabled(const dsd_opts* opts) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int enabled = (cfg && cfg->auto_ppm_enable) ? 1 : 0;
    int user = g_auto_ppm_user_en.load(std::memory_order_relaxed);
    if (user == 0) {
        return 0;
    }
    if (user == 1) {
        return 1;
    }
    if (opts && opts->rtl_auto_ppm) {
        return 1;
    }
    return enabled;
}

static dsd::io::radio::RtlAutoPpmConfig
auto_ppm_make_config(const dsd_opts* opts) {
    dsd::io::radio::RtlAutoPpmConfig config = {};
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg) {
        config.min_snr_db = cfg->auto_ppm_snr_db;
        config.min_power_db = cfg->auto_ppm_pwr_db;
        config.zero_lock_ppm = cfg->auto_ppm_zerolock_ppm;
        config.zero_lock_hz = static_cast<double>(cfg->auto_ppm_zerolock_hz);
    }
    if (opts && opts->rtl_auto_ppm_snr_db > 0.0f && opts->rtl_auto_ppm_snr_db <= 60.0f) {
        config.min_snr_db = static_cast<double>(opts->rtl_auto_ppm_snr_db);
    }
    return config;
}

static int
auto_ppm_should_freeze_retunes(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return 1;
    }
    return cfg->auto_ppm_freeze_enable ? 1 : 0;
}

static void
auto_ppm_publish_status(int enabled, const dsd::io::radio::RtlAutoPpmUpdate& update) {
    g_auto_ppm_enabled.store(enabled, std::memory_order_relaxed);
    g_auto_ppm_snr_db.store(update.snr_db, std::memory_order_relaxed);
    g_auto_ppm_df_hz.store(update.df_hz, std::memory_order_relaxed);
    g_auto_ppm_est_ppm.store(update.est_ppm, std::memory_order_relaxed);
    g_auto_ppm_last_dir.store(update.last_dir, std::memory_order_relaxed);
    g_auto_ppm_cooldown.store(update.cooldown_ticks, std::memory_order_relaxed);
    g_auto_ppm_training.store(update.training, std::memory_order_relaxed);
    g_auto_ppm_locked.store(update.locked, std::memory_order_relaxed);
    g_auto_ppm_lock_ppm.store(update.lock_ppm, std::memory_order_relaxed);
    g_auto_ppm_lock_snr_db.store(update.lock_snr_db, std::memory_order_relaxed);
    g_auto_ppm_lock_df_hz.store(update.lock_df_hz, std::memory_order_relaxed);
}

static void
auto_ppm_maybe_adjust(dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return;
    }

    int enabled = auto_ppm_effective_enabled(opts);
    int applied_ppm = load_dongle_ppm_error();
    uint64_t now_ms = auto_ppm_now_ms();
    uint32_t applied_freq_hz = controller.last_applied_freq_hz.load(std::memory_order_acquire);
    double spec_snr_db = g_spec_snr_db.load(std::memory_order_relaxed);
    dsd::io::radio::RtlAutoPpmConfig config = auto_ppm_make_config(opts);

    dsd::io::radio::RtlAutoPpmSignalMetrics metrics = {};
    metrics.cqpsk_enable = demod.cqpsk_enable ? 1 : 0;
    metrics.tracking_enable = (demod.cqpsk_enable || demod.fll_enabled) ? 1 : 0;
    metrics.carrier_lock = dsd_rtl_stream_get_carrier_lock();
    metrics.spectrum_valid = (spec_snr_db > -99.0) ? 1 : 0;
    metrics.nco_cfo_hz = dsd_rtl_stream_get_cfo_hz();
    metrics.phase_cfo_hz = g_resid_cfo_phase_hz.load(std::memory_order_relaxed);
    metrics.spectrum_cfo_hz = dsd_rtl_stream_get_residual_cfo_hz();

    dsd::io::radio::RtlAutoPpmInputs inputs = {};
    inputs.now_ms = now_ms;
    inputs.enabled = enabled;
    /* Auto-PPM must train against the correction the tuner has actually
     * applied, not a queued request that may still be waiting on the
     * controller thread. */
    inputs.current_ppm = applied_ppm;
    inputs.requested_ppm = snapshot_requested_ppm_state(opts).ppm;
    dsd::io::radio::RtlPpmControllerRequestsSnapshot controller_requests = snapshot_controller_ppm_request_state();
    inputs.controller_queued_request = controller_requests.queued_request;
    inputs.controller_active_request = controller_requests.active_request;
    inputs.tuned_freq_hz = applied_freq_hz;
    inputs.signal_power_db = g_spec_peak_db.load(std::memory_order_relaxed);
    inputs.gate_snr_db = auto_ppm_pick_demod_snr_db(now_ms);
    inputs.spec_snr_db = spec_snr_db;
    inputs.estimate = dsd::io::radio::rtl_auto_ppm_select_estimate(metrics);

    /* P25 status-symbol classification is advisory by default; only enforce it
     * when explicitly enabled because status-derived direction is unreliable on
     * some systems. */
    if (enabled && state && opts->p25_afc_status_gate_enable && DSD_SYNC_IS_P25P1(state->synctype)
        && state->p25_afc_gate_valid && !state->p25_afc_gate_allow) {
        return;
    }

    dsd::io::radio::RtlAutoPpmUpdate update = g_auto_ppm_controller.update(config, inputs);
    auto_ppm_publish_status(enabled, update);
    if (update.apply_ppm) {
        LOG_INFO("AUTO-PPM: src=%d pwr=%.1f dB snr=%.1f dB df=%.1f Hz ppm %d->%d\n",
                 static_cast<int>(inputs.estimate.source), inputs.signal_power_db, update.snr_db, update.df_hz,
                 applied_ppm, update.new_ppm);
        (void)publish_requested_ppm(opts, update.new_ppm);
    }
}

static int
rtl_stream_replay_should_exit_now(int replay_active) {
    if (!g_stream || !g_stream->should_exit.load(std::memory_order_acquire)) {
        return 0;
    }
    if (!replay_active) {
        return 1;
    }
    int replay_input_eof = g_stream->replay_input_eof.load(std::memory_order_acquire);
    int replay_forced_stop = g_stream->replay_forced_stop.load(std::memory_order_acquire);
    int replay_output_drained = g_stream->replay_output_drained.load(std::memory_order_acquire);
    return (!replay_input_eof || replay_forced_stop || replay_output_drained) ? 1 : 0;
}

static void
rtl_stream_replay_mark_output_drained(void) {
    if (!g_stream) {
        return;
    }
    g_stream->replay_output_drained.store(1, std::memory_order_release);
    g_stream->should_exit.store(1, std::memory_order_release);
    safe_cond_signal(&input_ring.ready, &input_ring.ready_m);
    safe_cond_signal(&output.ready, &output.ready_m);
    if (g_stream->replay_eof_sync_inited) {
        dsd_mutex_lock(&g_stream->replay_eof_m);
        dsd_cond_broadcast(&g_stream->replay_eof_cond);
        dsd_mutex_unlock(&g_stream->replay_eof_m);
    }
}

static int
rtl_stream_read_live(float* out, size_t count, dsd_opts* opts, const dsd_state* state) {
    sync_requested_ppm_after_failed_apply(opts);
    auto_ppm_maybe_adjust(opts, state);
    sync_requested_ppm_to_controller(opts);

    int perf_on = rtl_perf_enabled();
    uint64_t perf_read_start_ns = perf_on ? rtl_perf_now_ns() : 0ULL;
    int got = ring_read_batch(&output, out, count);
    if (got <= 0) {
        return -1;
    }
    if (perf_on) {
        rtl_perf_record_consumer_read(rtl_perf_now_ns() - perf_read_start_ns, (size_t)got);
    }
    return got;
}

static int
rtl_stream_read_replay(float* out, size_t count) {
    for (;;) {
        if (!output.buffer || !g_stream) {
            return -1;
        }
        if (g_stream->replay_forced_stop.load(std::memory_order_acquire) || exitflag) {
            return -1;
        }
        size_t used = ring_used(&output);
        if (used > 0) {
            size_t want = (count < used) ? count : used;
            int perf_on = rtl_perf_enabled();
            uint64_t perf_read_start_ns = perf_on ? rtl_perf_now_ns() : 0ULL;
            int got = ring_read_batch(&output, out, want);
            if (got <= 0) {
                return -1;
            }
            if (perf_on) {
                rtl_perf_record_consumer_read(rtl_perf_now_ns() - perf_read_start_ns, (size_t)got);
            }
            if (g_stream->replay_demod_drained.load(std::memory_order_acquire) && ring_used(&output) == 0U) {
                rtl_stream_replay_mark_output_drained();
            }
            return got;
        }
        if (g_stream->replay_demod_drained.load(std::memory_order_acquire)) {
            rtl_stream_replay_mark_output_drained();
            return -1;
        }
        dsd_mutex_lock(&output.ready_m);
        (void)dsd_cond_timedwait(&output.ready, &output.ready_m, 10);
        dsd_mutex_unlock(&output.ready_m);
    }
}

/**
 * @brief Batched consumer API: read up to count samples with fewer wakeups/locks.
 * Applies volume scaling.
 *
 * @param out   Destination buffer for audio samples.
 * @param count Maximum number of samples to read.
 * @param opts  Decoder options (must not be NULL; used for runtime PPM changes).
 * @param state Decoder state (unused).
 * @return Number of samples read (>=1), 0 if count==0, or -1 on exit.
 */
extern "C" int
dsd_rtl_stream_read(float* out, size_t count, dsd_opts* opts, const dsd_state* state) {
    if (count == 0) {
        return 0;
    }
    if (!output.buffer) {
        return -1;
    }
    int replay_active = stream_is_replay_active();
    if (rtl_stream_replay_should_exit_now(replay_active)) {
        return -1;
    }
    if (!opts) {
        return -1;
    }
    if (!replay_active) {
        return rtl_stream_read_live(out, count, opts, state);
    }
    return rtl_stream_read_replay(out, count);
}

extern "C" int
dsd_rtl_stream_monitor_read(float* out, size_t count, int* out_got) {
    int got_tmp = 0;
    int* got_ptr = out_got ? out_got : &got_tmp;
    *got_ptr = 0;
    if (!out || count == 0) {
        return 0;
    }
    if (!monitor_output.buffer) {
        return -1;
    }
    *got_ptr = ring_read_available(&monitor_output, out, count);
    return 0;
}

extern "C" int
rtl_stream_request_ppm(dsd_opts* opts, int ppm) {
    if (!opts) {
        return -1;
    }
    (void)publish_requested_ppm(opts, ppm);
    return 0;
}

extern "C" int
rtl_stream_adjust_ppm(dsd_opts* opts, int delta) {
    if (!opts) {
        return -1;
    }
    (void)publish_requested_ppm_delta(opts, delta);
    return 0;
}

extern "C" int
rtl_stream_get_requested_ppm(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return snapshot_requested_ppm_state(opts).ppm;
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

extern "C" unsigned int
dsd_rtl_stream_monitor_rate(void) {
    return (unsigned int)monitor_output.rate;
}

/* Helper for generic rings to observe RTL stream shutdown without using exitflag */
extern "C" int
dsd_rtl_stream_should_exit(void) {
    return (g_stream && g_stream->should_exit.load()) ? 1 : 0;
}

/**
 * @brief Return smoothed TED residual (EMA of Gardner error) in Q14 units.
 *
 * `ted_state.e_ema` is a normalized float residual roughly in [-1, +1].
 * Exporting a scaled integer keeps hook/UI APIs stable while preserving sign
 * and sufficient dynamic range for center-nudging deadbands.
 */
extern "C" int
dsd_rtl_stream_ted_bias(void) {
    const float scaled = demod.ted_state.e_ema * 16384.0f; /* Q14 scale */
    if (scaled > (float)INT_MAX) {
        return INT_MAX;
    }
    if (scaled < (float)INT_MIN) {
        return INT_MIN;
    }
    return (int)lrintf(scaled);
}

extern "C" int
dsd_rtl_stream_get_ted_sps(void) {
    return demod.ted_sps;
}

extern "C" int
dsd_rtl_stream_get_ted_sps_override(void) {
    return demod.ted_sps_override;
}

extern "C" int
dsd_rtl_stream_get_output_kind(void) {
    return demod.output_kind;
}

extern "C" int
dsd_rtl_stream_is_active(void) {
    return rtl_stream_context_active();
}

extern "C" uint32_t
dsd_rtl_stream_output_generation(void) {
    return g_rtl_output_generation.load(std::memory_order_acquire);
}

extern "C" int
dsd_rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = demod.symbol_rate_hz;
    }
    if (out_levels) {
        *out_levels = demod.symbol_levels;
    }
    if (out_channel_profile) {
        *out_channel_profile = demod.channel_lpf_profile;
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_get_symbol_profile(int* out_symbol_rate_hz, int* out_levels) {
    return dsd_rtl_stream_get_symbol_profile_full(out_symbol_rate_hz, out_levels, NULL);
}

extern "C" int
dsd_rtl_stream_set_symbol_profile(int symbol_rate_hz, int levels, int channel_profile) {
    if (symbol_rate_hz <= 0) {
        return -1;
    }
    if (levels != 2 && levels != 4) {
        return -1;
    }
    int changed = (demod.symbol_rate_hz != symbol_rate_hz || demod.symbol_levels != levels);
    demod.symbol_rate_hz = symbol_rate_hz;
    demod.symbol_levels = levels;
    if (channel_profile >= DSD_CH_LPF_PROFILE_WIDE && channel_profile <= DSD_CH_LPF_PROFILE_P25_CQPSK) {
        demod.channel_lpf_profile = channel_profile;
    }
    dsd_fsk_modem_config cfg = {};
    cfg.sample_rate_hz = demod.rate_out > 0 ? demod.rate_out : demod.rate_in;
    cfg.symbol_rate_hz = demod.symbol_rate_hz;
    cfg.levels = demod.symbol_levels;
    cfg.channel_profile = demod.channel_lpf_profile;
    dsd_fsk_modem_configure(&demod.fsk_modem_state, &cfg);
    if (changed) {
        rtl_fsk_metrics_reset_snapshot();
        demod.costas_reset_pending = 1;
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_request_fsk_reacquire(void) {
    if (demod.output_kind != DSD_DEMOD_OUTPUT_SYMBOL_FSK) {
        return 0;
    }
    g_fsk_reacquire_pending.store(1, std::memory_order_release);
    if (debug_sync_enabled()) {
        DSD_FPRINTF(stderr, "[FSKREACQ] requested output_generation=%u\n",
                    g_rtl_output_generation.load(std::memory_order_acquire));
    }
    return 1;
}

extern "C" void
dsd_rtl_stream_set_ted_sps(int sps) {
    if (sps < 2) {
        sps = 2;
    }
    if (sps > 64) {
        sps = 64;
    }
    /* Only set the override here, NOT ted_sps itself.
     *
     * This fixes a race condition where trunk_tune_to_freq() sets ted_sps
     * before the hardware retune completes, causing the DSP thread to
     * process stale samples (from the old frequency) with the new SPS.
     *
     * By only setting the override, the DSP continues with the current
     * (correct for current signal) SPS until the controller thread applies
     * the hardware retune and calls rtl_demod_maybe_refresh_ted_sps_after_rate_change,
     * which will see the override and apply it at the right time.
     *
     * This keeps timing/carrier configuration changes aligned with the
     * actual frequency change, not applied prematurely.
     *
     * We also set costas_reset_pending to signal that the Costas loop should
     * be reset on the next retune. Some demodulators reset Costas when the
     * symbol rate changes; in dsd-neo the ted_sps may already be updated by
     * other code paths before retune runs.
     */
    /* Debug: log set_ted_sps call */
    {
        if (debug_cqpsk_enabled()) {
            DSD_FPRINTF(stderr, "[SET_TED_SPS] sps=%d current_ted_sps=%d will_set_pending=%d\n", sps, demod.ted_sps,
                        (sps != demod.ted_sps) ? 1 : 0);
        }
    }
    if (sps != demod.ted_sps) {
        demod.costas_reset_pending = 1;
    }
    demod.ted_sps_override = sps;
    if (demod.cqpsk_enable) {
        demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    }
    if (demod.rate_out > 0) {
        int sym_rate = (demod.rate_out + (sps / 2)) / sps;
        if (sym_rate > 0) {
            (void)dsd_rtl_stream_set_symbol_profile(sym_rate, demod.symbol_levels == 2 ? 2 : 4,
                                                    demod.channel_lpf_profile);
        }
    }
}

extern "C" void
dsd_rtl_stream_clear_ted_sps_override(void) {
    demod.ted_sps_override = 0;
}

extern "C" void
dsd_rtl_stream_set_ted_sps_no_override(int sps) {
    if (sps < 2) {
        sps = 2;
    }
    if (sps > 64) {
        sps = 64;
    }
    /* Debug: log set_ted_sps_no_override call */
    {
        if (debug_cqpsk_enabled()) {
            DSD_FPRINTF(stderr, "[SET_TED_SPS_NO_OVERRIDE] sps=%d current_ted_sps=%d will_reset=%d\n", sps,
                        demod.ted_sps, (sps != demod.ted_sps) ? 1 : 0);
        }
    }
    /* Reset Costas loop IMMEDIATELY when SPS changes, not via pending flag.
     *
     * This function is called AFTER rtl_stream_tune() completes (e.g., in trunk_tune_to_cc),
     * so demod_reset_on_retune() has already executed and won't consume a pending flag.
     * We must reset the Costas loop here directly to avoid running with a ~20-25% frequency
     * error (the Costas freq in rad/symbol represents different Hz at different symbol rates).
     *
     */
    if (sps != demod.ted_sps) {
        demod.costas_state.freq = 0.0f;
        demod.costas_state.phase = 0.0f;
        demod.costas_state.error = 0.0f;
        demod.costas_state.error_smooth = 0.0f;
        demod.costas_err_avg_q14 = 0;
        demod.costas_err_raw_avg_q14 = 0;
        demod.costas_conf_avg_q14 = 0;
        demod.costas_zero_conf_pct = 0;
    }
    demod.ted_sps = sps;
    if (demod.cqpsk_enable) {
        demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
    }
    if (demod.rate_out > 0) {
        int sym_rate = (demod.rate_out + (sps / 2)) / sps;
        if (sym_rate > 0) {
            (void)dsd_rtl_stream_set_symbol_profile(sym_rate, demod.symbol_levels == 2 ? 2 : 4,
                                                    demod.channel_lpf_profile);
        }
    }
    /* Does NOT set ted_sps_override, allowing rate-change refresh to
       recalculate SPS later. Use when returning to CC or switching protocols. */
}

extern "C" void
dsd_rtl_stream_set_ted_gain(float g) {
    if (g < 0.01f) {
        g = 0.01f;
    }
    if (g > 0.5f) {
        g = 0.5f;
    }
    demod.ted_gain = g;
    demod.ted_gain_is_set = 1;
    demod.ted_effective_gain = g;
}

extern "C" float
dsd_rtl_stream_get_ted_gain(void) {
    return demod.ted_gain;
}

static inline int
rtl_stream_symbol_output_active(void) {
    return demod.output_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK || demod.output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK
           || demod.cqpsk_enable;
}

static inline int
rtl_stream_fsk_symbol_output_active(void) {
    return demod.output_kind == DSD_DEMOD_OUTPUT_SYMBOL_FSK;
}

static inline int
rtl_stream_cqpsk_symbol_output_active(void) {
    return demod.output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK || demod.cqpsk_enable;
}

static void
rtl_stream_reset_fll_runtime_state(void) {
    fll_init_state(&demod.fll_state);
    demod.fll_freq = 0.0f;
    demod.fll_phase = 0.0f;
    demod.fll_prev_r = 0.0f;
    demod.fll_prev_j = 0.0f;
}

static void
rtl_stream_reset_ted_runtime_state(void) {
    ted_init_state(&demod.ted_state);
    demod.ted_mu = 0.0f;
}

static void
rtl_stream_clear_non_symbol_controls_for_symbol_output(void) {
    if (rtl_stream_fsk_symbol_output_active()) {
        demod.fm_agc_enable = 0;
        demod.fm_limiter_enable = 0;
        demod.ted_force = 0;
        if (demod.fll_enabled) {
            demod.fll_enabled = 0;
            rtl_stream_reset_fll_runtime_state();
        }
        if (demod.ted_enabled) {
            demod.ted_enabled = 0;
            rtl_stream_reset_ted_runtime_state();
        }
        return;
    }

    if (rtl_stream_cqpsk_symbol_output_active()) {
        demod.fm_agc_enable = 0;
        demod.fm_limiter_enable = 0;
        demod.ted_force = 0;
        if (demod.fll_enabled) {
            demod.fll_enabled = 0;
            rtl_stream_reset_fll_runtime_state();
        }
        demod.ted_enabled = 1;
    }
}

extern "C" void
dsd_rtl_stream_set_ted_force(int onoff) {
    if (rtl_stream_symbol_output_active()) {
        demod.ted_force = 0;
        return;
    }
    demod.ted_force = onoff ? 1 : 0;
}

extern "C" int
dsd_rtl_stream_get_ted_force(void) {
    if (rtl_stream_symbol_output_active()) {
        return 0;
    }
    return demod.ted_force ? 1 : 0;
}

/* -------- FM/C4FM amplitude stabilization + DC blocker (runtime) -------- */
extern "C" int
dsd_rtl_stream_get_fm_agc(void) {
    if (rtl_stream_symbol_output_active()) {
        return 0;
    }
    return demod.fm_agc_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_fm_agc(int onoff) {
    if (rtl_stream_symbol_output_active()) {
        demod.fm_agc_enable = 0;
        return;
    }
    demod.fm_agc_enable = onoff ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_get_fm_agc_params(float* target_rms, float* min_rms, float* alpha_up, float* alpha_down) {
    if (target_rms) {
        *target_rms = demod.fm_agc_target_rms;
    }
    if (min_rms) {
        *min_rms = demod.fm_agc_min_rms;
    }
    if (alpha_up) {
        *alpha_up = demod.fm_agc_alpha_up;
    }
    if (alpha_down) {
        *alpha_down = demod.fm_agc_alpha_down;
    }
}

extern "C" void
dsd_rtl_stream_set_fm_agc_params(float target_rms, float min_rms, float alpha_up, float alpha_down) {
    if (target_rms >= 0.0f) {
        if (target_rms < 0.05f) {
            target_rms = 0.05f;
        }
        if (target_rms > 2.5f) {
            target_rms = 2.5f;
        }
        demod.fm_agc_target_rms = target_rms;
    }
    if (min_rms >= 0.0f) {
        if (min_rms > 1.0f) {
            min_rms = 1.0f;
        }
        demod.fm_agc_min_rms = min_rms;
    }
    if (alpha_up >= 0.0f) {
        if (alpha_up > 1.0f) {
            alpha_up = 1.0f;
        }
        demod.fm_agc_alpha_up = alpha_up;
    }
    if (alpha_down >= 0.0f) {
        if (alpha_down > 1.0f) {
            alpha_down = 1.0f;
        }
        demod.fm_agc_alpha_down = alpha_down;
    }
}

extern "C" int
dsd_rtl_stream_get_fm_limiter(void) {
    if (rtl_stream_symbol_output_active()) {
        return 0;
    }
    return demod.fm_limiter_enable ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_fm_limiter(int onoff) {
    if (rtl_stream_symbol_output_active()) {
        demod.fm_limiter_enable = 0;
        return;
    }
    demod.fm_limiter_enable = onoff ? 1 : 0;
}

extern "C" int
dsd_rtl_stream_get_iq_dc(int* out_shift_k) {
    if (out_shift_k) {
        *out_shift_k = demod.iq_dc_shift;
    }
    return demod.iq_dc_block_enable ? 1 : 0;
}

static int
iq_dc_clamp_shift_k(int shift_k) {
    if (shift_k < 6) {
        return 6;
    }
    if (shift_k > 15) {
        return 15;
    }
    return shift_k;
}

static void
iq_dc_precharge_and_retarget_agc(void) {
    if (!demod.lowpassed || demod.lp_len < 2) {
        return;
    }
    const int pairs = demod.lp_len >> 1;
    double sumI = 0.0;
    double sumQ = 0.0;
    for (int n = 0; n < pairs; n++) {
        sumI += (double)demod.lowpassed[(size_t)(n << 1) + 0];
        sumQ += (double)demod.lowpassed[(size_t)(n << 1) + 1];
    }
    float meanI = (pairs > 0) ? (float)(sumI / (double)pairs) : 0.0f;
    float meanQ = (pairs > 0) ? (float)(sumQ / (double)pairs) : 0.0f;
    demod.iq_dc_avg_r = meanI;
    demod.iq_dc_avg_i = meanQ;

    double acc = 0.0;
    for (int n = 0; n < pairs; n++) {
        double I = (double)demod.lowpassed[(size_t)(n << 1) + 0] - (double)meanI;
        double Q = (double)demod.lowpassed[(size_t)(n << 1) + 1] - (double)meanQ;
        acc += I * I + Q * Q;
    }
    if (pairs <= 0) {
        return;
    }
    double mean_r2 = acc / (double)pairs;
    double rms = sqrt(mean_r2);
    float target = (demod.fm_agc_target_rms > 0.0f) ? demod.fm_agc_target_rms : 0.30f;
    target = std::max(0.05f, std::min(2.5f, target));
    double g_raw = (rms > 1e-6) ? ((double)target / rms) : 1.0;
    g_raw = std::max(0.125, std::min(8.0, g_raw));
    demod.fm_agc_gain = (float)g_raw;
    demod.fm_agc_ema_rms = rms;
}

extern "C" void
dsd_rtl_stream_set_iq_dc(int enable, int shift_k) {
    int was = demod.iq_dc_block_enable ? 1 : 0;
    if (enable >= 0) {
        demod.iq_dc_block_enable = enable ? 1 : 0;
    }
    if (shift_k >= 0) {
        demod.iq_dc_shift = iq_dc_clamp_shift_k(shift_k);
    }
    /* If enabling now, precharge DC estimate to current block mean and retarget AGC
       so there is no apparent level drop. */
    if (!was && demod.iq_dc_block_enable) {
        iq_dc_precharge_and_retarget_agc();
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
    schedule_manual_retune(load_dongle_frequency());
}

/* Runtime DSP tuning entrypoints (C shim) */

/**
 * @brief P25 Phase 2 error callbacks for runtime helpers.
 * Aggregates recent RS/voice error deltas.
 */
extern "C" void
dsd_rtl_stream_p25p2_err_update(int slot, int facch_ok_delta, int facch_err_delta, int sacch_ok_delta,
                                int sacch_err_delta, int voice_err_delta) {
    (void)slot;
    if (!rtl_decode_health_prepare_update()) {
        return;
    }
    int changed = 0;
    if (facch_ok_delta > 0) {
        g_decode_p25p2_facch_ok.fetch_add((unsigned int)facch_ok_delta, std::memory_order_relaxed);
        changed = 1;
    }
    if (facch_err_delta > 0) {
        g_decode_p25p2_facch_err.fetch_add((unsigned int)facch_err_delta, std::memory_order_relaxed);
        changed = 1;
    }
    if (sacch_ok_delta > 0) {
        g_decode_p25p2_sacch_ok.fetch_add((unsigned int)sacch_ok_delta, std::memory_order_relaxed);
        changed = 1;
    }
    if (sacch_err_delta > 0) {
        g_decode_p25p2_sacch_err.fetch_add((unsigned int)sacch_err_delta, std::memory_order_relaxed);
        changed = 1;
    }
    if (voice_err_delta > 0) {
        g_decode_p25p2_voice_err.fetch_add((unsigned int)voice_err_delta, std::memory_order_relaxed);
        changed = 1;
    }
    if (changed) {
        g_decode_health_valid.store(1, std::memory_order_release);
    }
}

extern "C" void
rtl_stream_p25p1_ber_update(int fec_ok_delta, int fec_err_delta) {
    if (!rtl_decode_health_prepare_update()) {
        return;
    }
    int changed = 0;
    if (fec_ok_delta > 0) {
        g_decode_p25p1_fec_ok.fetch_add((unsigned int)fec_ok_delta, std::memory_order_relaxed);
        changed = 1;
    }
    if (fec_err_delta > 0) {
        g_decode_p25p1_fec_err.fetch_add((unsigned int)fec_err_delta, std::memory_order_relaxed);
        changed = 1;
    }
    if (changed) {
        g_decode_health_valid.store(1, std::memory_order_release);
    }
}

extern "C" int
dsd_rtl_stream_get_fsk_metrics(rtl_stream_fsk_metrics* out) {
    if (!out) {
        return -1;
    }
    *out = rtl_stream_fsk_metrics{};
    uint32_t current_generation = g_rtl_output_generation.load(std::memory_order_acquire);
    uint32_t snapshot_generation = g_fsk_metrics_generation.load(std::memory_order_acquire);
    out->generation = current_generation;
    if (!rtl_stream_context_active()) {
        rtl_fsk_metrics_reset_snapshot();
        return 0;
    }
    if (!g_fsk_metrics_valid.load(std::memory_order_acquire) || snapshot_generation != current_generation) {
        return 0;
    }
    out->valid = 1;
    out->levels = g_fsk_metrics_levels.load(std::memory_order_relaxed);
    out->symbol_rate_hz = g_fsk_metrics_symbol_rate_hz.load(std::memory_order_relaxed);
    out->symbols_total = g_fsk_metrics_symbols_total.load(std::memory_order_relaxed);
    out->window_symbols = g_fsk_metrics_window_symbols.load(std::memory_order_relaxed);
    out->mean_reliability = g_fsk_metrics_mean_reliability.load(std::memory_order_relaxed);
    out->min_reliability = g_fsk_metrics_min_reliability.load(std::memory_order_relaxed);
    out->rms_error = (float)g_fsk_metrics_rms_error.load(std::memory_order_relaxed);
    out->evm_snr_db = (float)g_fsk_metrics_evm_snr_db.load(std::memory_order_relaxed);
    out->low_reliability_pct = (float)g_fsk_metrics_low_reliability_pct.load(std::memory_order_relaxed);
    out->clip_pct = (float)g_fsk_metrics_clip_pct.load(std::memory_order_relaxed);
    out->timing_acquired = g_fsk_metrics_timing_acquired.load(std::memory_order_relaxed);
    out->track_last_error = (float)g_fsk_metrics_track_last_error.load(std::memory_order_relaxed);
    out->track_last_score = (float)g_fsk_metrics_track_last_score.load(std::memory_order_relaxed);
    out->track_updates = g_fsk_metrics_track_updates.load(std::memory_order_relaxed);
    out->track_skips = g_fsk_metrics_track_skips.load(std::memory_order_relaxed);
    out->abs_est = (float)g_fsk_metrics_abs_est.load(std::memory_order_relaxed);
    out->dc_est = (float)g_fsk_metrics_dc_est.load(std::memory_order_relaxed);
    out->last_symbol = (float)g_fsk_metrics_last_symbol.load(std::memory_order_relaxed);
    return 0;
}

extern "C" int
dsd_rtl_stream_get_decode_health(rtl_stream_decode_health* out) {
    if (!out) {
        return -1;
    }
    *out = rtl_stream_decode_health{};
    uint32_t current_generation = g_rtl_output_generation.load(std::memory_order_acquire);
    uint32_t snapshot_generation = g_decode_health_generation.load(std::memory_order_acquire);
    out->generation = current_generation;
    if (!rtl_stream_context_active()) {
        return 0;
    }
    if (!g_decode_health_valid.load(std::memory_order_acquire) || snapshot_generation != current_generation) {
        return 0;
    }
    out->valid = 1;
    out->p25p1_fec_ok = g_decode_p25p1_fec_ok.load(std::memory_order_relaxed);
    out->p25p1_fec_err = g_decode_p25p1_fec_err.load(std::memory_order_relaxed);
    out->p25p2_facch_ok = g_decode_p25p2_facch_ok.load(std::memory_order_relaxed);
    out->p25p2_facch_err = g_decode_p25p2_facch_err.load(std::memory_order_relaxed);
    out->p25p2_sacch_ok = g_decode_p25p2_sacch_ok.load(std::memory_order_relaxed);
    out->p25p2_sacch_err = g_decode_p25p2_sacch_err.load(std::memory_order_relaxed);
    out->p25p2_voice_err = g_decode_p25p2_voice_err.load(std::memory_order_relaxed);
    return 0;
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

static void
rtl_stream_enable_cqpsk_mode(void) {
    demod.output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    demod.symbol_levels = 4;
    demod.ted_enabled = 1;
    demod.mode_demod = &qpsk_differential_demod;
    demod.cqpsk_diff_prev_r = 1.0f;
    demod.cqpsk_diff_prev_j = 0.0f;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    demod.cqpsk_eq_enable = (cfg && cfg->cqpsk_eq_is_set) ? cfg->cqpsk_eq_enable : 1;
    if (cfg && cfg->cqpsk_eq_taps_is_set) {
        demod.cqpsk_eq_taps = cfg->cqpsk_eq_taps;
    }
    if (cfg && cfg->cqpsk_eq_mu_is_set) {
        demod.cqpsk_eq_mu = cfg->cqpsk_eq_mu;
    }
    if (cfg && cfg->cqpsk_eq_modulus_is_set) {
        demod.cqpsk_eq_modulus = cfg->cqpsk_eq_modulus;
    }
    dsd_cqpsk_cma_equalizer_reset(&demod.cqpsk_eq_state, demod.cqpsk_eq_taps);
    demod.channel_lpf_profile = DSD_CH_LPF_PROFILE_P25_CQPSK;
}

static void
rtl_stream_disable_cqpsk_mode(void) {
    demod.mode_demod = &dsd_fm_demod;
    demod.cqpsk_eq_enable = 0;
    if (demod.channel_lpf_profile == DSD_CH_LPF_PROFILE_P25_CQPSK) {
        demod.channel_lpf_profile = rtl_stream_fsk_channel_profile_for_current_mode();
    }
    if (g_stream && opts_is_digital_mode(g_stream->opts) && radio_source_is_rtl_family(g_stream->opts)) {
        demod.output_kind = DSD_DEMOD_OUTPUT_SYMBOL_FSK;
    } else {
        demod.output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    }
}

/* Coarse DSP feature toggles and snapshot */
extern "C" void
rtl_stream_toggle_cqpsk(int onoff) {
    int was = demod.cqpsk_enable ? 1 : 0;
    demod.cqpsk_enable = onoff ? 1 : 0;
    if (demod.cqpsk_enable) {
        rtl_stream_enable_cqpsk_mode();
    } else {
        rtl_stream_disable_cqpsk_mode();
    }
    rtl_stream_clear_non_symbol_controls_for_symbol_output();
    /* If the demod family changed, request a Costas reset on the next retune.
     * This keeps loop state consistent when switching between FM and CQPSK paths. */
    if (demod.cqpsk_enable != was) {
        demod.costas_reset_pending = 1;
    }
}

static int
rtl_stream_clamp_retune_ted_sps(int sps) {
    if (sps < 2) {
        return 2;
    }
    if (sps > 64) {
        return 64;
    }
    return sps;
}

static void
rtl_stream_clear_retune_profile(RtlRetuneProfile* profile) {
    if (profile) {
        *profile = RtlRetuneProfile{};
    }
}

static int
rtl_stream_retune_profile_matches_target(const RtlRetuneProfile* profile, uint32_t target_freq_hz) {
    if (!profile || !profile->active) {
        return 0;
    }
    if (profile->target_freq_hz == 0U) {
        return 1;
    }
    if (target_freq_hz == 0U) {
        return 0;
    }
    return profile->target_freq_hz == target_freq_hz;
}

static int
rtl_stream_take_pending_retune_profile(RtlRetuneProfile* out_profile, uint32_t request_id, uint32_t target_freq_hz) {
    if (!out_profile) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_pending_retune_profile_mutex);
    if (!rtl_stream_retune_profile_matches_target(&g_pending_retune_profile, target_freq_hz)) {
        return 0;
    }

    rtl_stream_clear_retune_profile(out_profile);
    *out_profile = g_pending_retune_profile;
    out_profile->active = 1;
    out_profile->request_id = request_id;
    if (out_profile->target_freq_hz == 0U) {
        out_profile->target_freq_hz = target_freq_hz;
    }
    rtl_stream_clear_retune_profile(&g_pending_retune_profile);
    return 1;
}

static void
rtl_stream_store_pending_retune_profile(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz, int levels,
                                        int channel_profile, int ted_sps, int persist_ted_override) {
    if (levels != 2 && levels != 4) {
        levels = 4;
    }
    if (ted_sps > 0) {
        ted_sps = rtl_stream_clamp_retune_ted_sps(ted_sps);
    }

    RtlRetuneProfile profile{};
    profile.active = 1;
    profile.cqpsk_enable = cqpsk_enable;
    profile.symbol_rate_hz = symbol_rate_hz;
    profile.levels = levels;
    profile.channel_profile = channel_profile;
    profile.ted_sps = ted_sps;
    profile.ted_override = persist_ted_override ? 1 : 0;
    profile.target_freq_hz = target_freq_hz;

    std::lock_guard<std::mutex> lock(g_pending_retune_profile_mutex);
    g_pending_retune_profile = profile;
}

extern "C" void
dsd_rtl_stream_prepare_retune_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                      int ted_sps, int persist_ted_override) {
    rtl_stream_store_pending_retune_profile(0U, cqpsk_enable, symbol_rate_hz, levels, channel_profile, ted_sps,
                                            persist_ted_override);
}

extern "C" void
dsd_rtl_stream_prepare_retune_profile_for_target(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz,
                                                 int levels, int channel_profile, int ted_sps,
                                                 int persist_ted_override) {
    rtl_stream_store_pending_retune_profile(target_freq_hz, cqpsk_enable, symbol_rate_hz, levels, channel_profile,
                                            ted_sps, persist_ted_override);
}

extern "C" void
dsd_rtl_stream_clear_pending_retune_profile(void) {
    std::lock_guard<std::mutex> lock(g_pending_retune_profile_mutex);
    rtl_stream_clear_retune_profile(&g_pending_retune_profile);
}

static void
rtl_stream_apply_retune_profile(const RtlRetuneProfile* profile, uint32_t center_freq_hz) {
    if (!profile || !profile->active) {
        return;
    }
    if (profile->target_freq_hz != 0U) {
        if (center_freq_hz == 0U || profile->target_freq_hz != center_freq_hz) {
            return;
        }
    }

    int cqpsk = profile->cqpsk_enable;
    if (cqpsk >= 0) {
        rtl_stream_toggle_cqpsk(cqpsk);
    }

    int symbol_rate_hz = profile->symbol_rate_hz;
    int levels = profile->levels;
    int channel_profile = profile->channel_profile;
    if (symbol_rate_hz > 0 && (levels == 2 || levels == 4)) {
        (void)dsd_rtl_stream_set_symbol_profile(symbol_rate_hz, levels, channel_profile);
    }

    int ted_sps = profile->ted_sps;
    if (ted_sps <= 0) {
        return;
    }
    ted_sps = rtl_stream_clamp_retune_ted_sps(ted_sps);
    if (ted_sps != demod.ted_sps) {
        demod.costas_reset_pending = 1;
    }
    demod.ted_sps = ted_sps;
    demod.ted_sps_override = profile->ted_override ? ted_sps : 0;
}

extern "C" void
dsd_rtl_stream_apply_pending_retune_profile(void) {
    dsd_rtl_stream_apply_pending_retune_profile_for_target(0U);
}

extern "C" void
dsd_rtl_stream_apply_pending_retune_profile_for_target(uint32_t target_freq_hz) {
    RtlRetuneProfile profile{};
    (void)rtl_stream_take_pending_retune_profile(&profile, 0U, target_freq_hz);
    rtl_stream_apply_retune_profile(&profile, target_freq_hz);
}

extern "C" void
rtl_stream_toggle_fll(int onoff) {
    if (rtl_stream_symbol_output_active()) {
        demod.fll_enabled = 0;
        rtl_stream_reset_fll_runtime_state();
        return;
    }
    demod.fll_enabled = onoff ? 1 : 0;
    if (!demod.fll_enabled) {
        /* Reset FLL state to baseline to avoid carryover */
        rtl_stream_reset_fll_runtime_state();
    }
}

extern "C" void
rtl_stream_toggle_ted(int onoff) {
    if (rtl_stream_fsk_symbol_output_active()) {
        demod.ted_enabled = 0;
        demod.ted_force = 0;
        rtl_stream_reset_ted_runtime_state();
        return;
    }

    if (rtl_stream_cqpsk_symbol_output_active() || (!onoff && demod.cqpsk_enable)) {
        /* Prevent disabling TED while CQPSK path is active: the CQPSK
           Costas/differential stage requires symbol-rate samples from
           the Gardner TED. Ignore the request when CQPSK is enabled. */
        demod.ted_enabled = 1;
        demod.ted_force = 0;
        return;
    }

    demod.ted_enabled = onoff ? 1 : 0;
    if (!demod.ted_enabled) {
        /* Reset TED state */
        rtl_stream_reset_ted_runtime_state();
    }
}

extern "C" int
rtl_stream_dsp_get(int* cqpsk_enable, int* fll_enable, int* ted_enable) {
    if (cqpsk_enable) {
        *cqpsk_enable = (demod.cqpsk_enable || rtl_stream_cqpsk_symbol_output_active()) ? 1 : 0;
    }
    if (fll_enable) {
        *fll_enable = rtl_stream_symbol_output_active() ? 0 : (demod.fll_enabled ? 1 : 0);
    }
    if (ted_enable) {
        if (rtl_stream_fsk_symbol_output_active()) {
            *ted_enable = 0;
        } else if (rtl_stream_cqpsk_symbol_output_active()) {
            *ted_enable = 1;
        } else {
            *ted_enable = demod.ted_enabled ? 1 : 0;
        }
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_get_cqpsk_eq_status(rtl_stream_cqpsk_eq_status* out) {
    if (!out) {
        return -1;
    }

    *out = rtl_stream_cqpsk_eq_status{};
    out->enabled = demod.cqpsk_eq_enable ? 1 : 0;
    out->taps = demod.cqpsk_eq_taps;
    out->mu = demod.cqpsk_eq_mu;
    out->modulus = demod.cqpsk_eq_modulus;

    dsd_cqpsk_cma_equalizer_metrics_t m;
    dsd_cqpsk_cma_equalizer_get_metrics(&demod.cqpsk_eq_state, &m);
    out->initialized = m.initialized;
    out->taps = m.taps > 0 ? m.taps : out->taps;
    out->symbols = m.symbols;
    out->err_ema = m.err_ema;
    out->mag2_ema = m.mag2_ema;
    out->tap_energy = m.tap_energy;
    out->center_tap_mag = m.center_tap_mag;
    out->max_side_tap_mag = m.max_side_tap_mag;
    return 0;
}

static int
rtl_stream_cqpsk_eq_clamp_taps(int taps) {
    taps = std::max(3, std::min(DSD_CQPSK_CMA_EQ_MAX_TAPS, taps));
    if ((taps & 1) == 0) {
        taps += (taps < DSD_CQPSK_CMA_EQ_MAX_TAPS) ? 1 : -1;
    }
    return taps;
}

static float
rtl_stream_cqpsk_eq_clamp_mu(float mu) {
    return std::max(0.000001f, std::min(0.01f, mu));
}

static float
rtl_stream_cqpsk_eq_clamp_modulus(float modulus) {
    return std::max(0.05f, std::min(4.0f, modulus));
}

static int
rtl_stream_cqpsk_eq_should_reset(int desired_enable, int desired_taps) {
    if (desired_enable && !demod.cqpsk_eq_enable) {
        return 1;
    }
    return (desired_taps != demod.cqpsk_eq_taps) ? 1 : 0;
}

extern "C" void
dsd_rtl_stream_set_cqpsk_eq(int enable, int taps, float mu, float modulus) {
    int desired_enable = 1;
    int desired_taps = DSD_CQPSK_CMA_EQ_DEFAULT_TAPS;
    float desired_mu = DSD_CQPSK_CMA_EQ_DEFAULT_MU;
    float desired_modulus = DSD_CQPSK_CMA_EQ_DEFAULT_MODULUS;

    dsd_neo_get_cqpsk_eq(&desired_enable, &desired_taps, &desired_mu, &desired_modulus);

    if (enable >= 0) {
        desired_enable = enable ? 1 : 0;
    }

    if (taps > 0) {
        desired_taps = rtl_stream_cqpsk_eq_clamp_taps(taps);
    }

    if (mu >= 0.0f) {
        desired_mu = rtl_stream_cqpsk_eq_clamp_mu(mu);
    }

    if (modulus >= 0.0f) {
        desired_modulus = rtl_stream_cqpsk_eq_clamp_modulus(modulus);
    }

    int reset = rtl_stream_cqpsk_eq_should_reset(desired_enable, desired_taps);
    demod.cqpsk_eq_enable = demod.cqpsk_enable ? desired_enable : 0;
    demod.cqpsk_eq_taps = desired_taps;
    demod.cqpsk_eq_mu = desired_mu;
    demod.cqpsk_eq_modulus = desired_modulus;

    dsd_neo_set_cqpsk_eq(desired_enable, desired_taps, desired_mu, desired_modulus);
    if (reset) {
        dsd_cqpsk_cma_equalizer_reset(&demod.cqpsk_eq_state, demod.cqpsk_eq_taps);
    }
}

extern "C" void
dsd_rtl_stream_reset_cqpsk_eq(void) {
    dsd_cqpsk_cma_equalizer_reset(&demod.cqpsk_eq_state, demod.cqpsk_eq_taps);
}

/**
 * @brief Tune RTL-SDR to a new center frequency, updating optimal settings.
 *
 * This function is SYNCHRONOUS: it blocks until the controller thread has
 * completed the hardware retune and all DSP state resets. This ensures that
 * subsequent SPS/Costas configuration calls operate on properly reset state.
 *
 * @param opts      Decoder options.
 * @param frequency Target center frequency in Hz.
 * @return rtl_stream_tune_result: 0 on success, 1 when deferred, negative on error/timeout.
 */
static const char*
rtl_stream_backend_name(void) {
    if (stream_is_replay_active()) {
        return "iq-replay";
    }
    if (g_stream && g_stream->opts) {
        if (g_stream->opts->rtltcp_enabled) {
            return "rtl_tcp";
        }
        if (dsd_opts_audio_in_dev_is_soapy_spec(g_stream->opts->audio_in_dev)) {
            return "soapy";
        }
    }
    return "rtl";
}

static void
rtl_stream_log_tune_warning(uint32_t requested_freq, const char* reason) {
    uint32_t current_freq = controller.last_applied_freq_hz.load(std::memory_order_acquire);
    if (current_freq == 0U) {
        current_freq = load_dongle_frequency();
    }
    LOG_NOTICE("RTL retune warning: requested=%u Hz current=%u Hz backend=%s reason=%s\n", requested_freq, current_freq,
               rtl_stream_backend_name(), reason ? reason : "unknown");
}

static int
rtl_stream_tune_wait_for_completion(uint32_t request_id, uint32_t requested_freq) {
    int rc = RTL_STREAM_TUNE_OK;
    const uint64_t deadline_ns = dsd_time_monotonic_ns() + 500000000ULL;
    dsd_mutex_lock(&controller.retune_done_m);
    while (controller.retune_complete_id.load(std::memory_order_acquire) < request_id) {
        uint64_t now_ns = dsd_time_monotonic_ns();
        if (now_ns >= deadline_ns) {
            rtl_stream_log_tune_warning(requested_freq, "timeout");
            rc = RTL_STREAM_TUNE_TIMEOUT;
            break;
        }
        uint64_t remaining_ms = (deadline_ns - now_ns + 999999ULL) / 1000000ULL;
        int wait_ms = (remaining_ms > 25ULL) ? 25 : (int)remaining_ms;
        if (wait_ms <= 0) {
            wait_ms = 1;
        }
        int wait_rc = dsd_cond_timedwait(&controller.retune_done_cond, &controller.retune_done_m, wait_ms);
        if (wait_rc != 0) {
            continue;
        }
        if (exitflag || (g_stream && g_stream->should_exit.load(std::memory_order_relaxed))) {
            rtl_stream_log_tune_warning(requested_freq, "shutdown");
            rc = RTL_STREAM_TUNE_FAILED;
            break;
        }
    }
    dsd_mutex_unlock(&controller.retune_done_m);
    return rc;
}

static int
rtl_stream_tune_result_needs_output_drain(int rc) {
    return rc == RTL_STREAM_TUNE_OK || rc == RTL_STREAM_TUNE_TIMEOUT;
}

static void
rtl_stream_tune_reconcile_applied_frequency(dsd_opts* opts, uint32_t requested_freq) {
    uint32_t applied_freq = controller.last_applied_freq_hz.load(std::memory_order_acquire);
    if (applied_freq == 0 || applied_freq == requested_freq) {
        return;
    }
    LOG_NOTICE("Retune request %u Hz superseded by %u Hz (coalesced pending tune).\n", requested_freq, applied_freq);
    store_dongle_frequency(applied_freq);
    if (opts) {
        opts->rtlsdr_center_freq = (long int)applied_freq;
    }
}

extern "C" int
dsd_rtl_stream_tune(dsd_opts* opts, long int frequency) {
    if (stream_is_replay_active()) {
        static std::atomic<uint64_t> s_last_notice_ns{0};
        uint64_t now_ns = dsd_time_monotonic_ns();
        uint64_t prev_ns = s_last_notice_ns.load(std::memory_order_acquire);
        if (now_ns > prev_ns + 1000000000ULL) {
            s_last_notice_ns.store(now_ns, std::memory_order_release);
            LOG_NOTICE("Retune ignored during IQ replay.\n");
        }
        return RTL_STREAM_TUNE_DEFERRED;
    }
    if (auto_ppm_should_freeze_retunes() && dsd_rtl_stream_auto_ppm_training_active()) {
        rtl_stream_log_tune_warning((uint32_t)frequency, "auto_ppm_training");
        return RTL_STREAM_TUNE_DEFERRED;
    }
    if (opts->payload == 1) {
        LOG_INFO("\nTuning to %ld Hz.", frequency);
    }
    uint32_t requested_freq = (uint32_t)frequency;
    opts->rtlsdr_center_freq = frequency;
    store_dongle_frequency(requested_freq);

    /* Enqueue retune, coalescing with any already-pending request so completion IDs
     * stay aligned with the number of retunes the controller will actually execute. */
    uint32_t my_request_id = schedule_manual_retune(requested_freq);

    if (opts->payload == 1) {
        LOG_INFO(" (Center Frequency: %u Hz.) \n", requested_freq);
    }

    int rc = rtl_stream_tune_wait_for_completion(my_request_id, requested_freq);

    if (rc == RTL_STREAM_TUNE_OK) {
        rtl_stream_tune_reconcile_applied_frequency(opts, requested_freq);
    }
    if (rtl_stream_tune_result_needs_output_drain(rc)) {
        /* Honor drain/clear policy for API-triggered tunes and accepted-but-pending timeout paths. */
        drain_output_on_retune();
    }
    return rc;
}

#if defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
extern "C" int
dsd_rtl_stream_test_request_retune(long int frequency, int timeout_ms) {
    if (!g_stream) {
        return -2;
    }
    if (stream_is_replay_active()) {
        return -1;
    }

    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    uint32_t request_id = schedule_manual_retune((uint32_t)frequency);
    if (timeout_ms == 0) {
        return 0;
    }

    int remaining_ms = timeout_ms;
    dsd_mutex_lock(&controller.retune_done_m);
    while (controller.retune_complete_id.load(std::memory_order_acquire) < request_id) {
        if (remaining_ms <= 0) {
            dsd_mutex_unlock(&controller.retune_done_m);
            return -2;
        }
        int wait_slice_ms = (remaining_ms > 50) ? 50 : remaining_ms;
        int wait_rc = dsd_cond_timedwait(&controller.retune_done_cond, &controller.retune_done_m, wait_slice_ms);
        remaining_ms -= wait_slice_ms;
        if (wait_rc != 0) {
            dsd_mutex_unlock(&controller.retune_done_m);
            return -2;
        }
        if (exitflag || (g_stream && g_stream->should_exit.load(std::memory_order_acquire))) {
            dsd_mutex_unlock(&controller.retune_done_m);
            return -2;
        }
    }
    dsd_mutex_unlock(&controller.retune_done_m);
    return 0;
}

extern "C" int
dsd_rtl_stream_test_prepare_reconfigure_input(size_t queued_samples, size_t* out_used_after,
                                              uint32_t* out_generation_before, uint32_t* out_generation_after) {
    if (!out_used_after || !out_generation_before || !out_generation_after) {
        return -1;
    }

    int initialized_output = 0;
    if (!output.buffer) {
        output_init(&output);
        initialized_output = 1;
    }
    if (!output.buffer || output.capacity == 0U) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -2;
    }
    if (queued_samples >= output.capacity) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -3;
    }

    ring_clear(&output);
    output.tail.store(0);
    output.head.store(queued_samples);

    *out_generation_before = dsd_rtl_stream_output_generation();
    controller_enter_reconfigure_gate(&controller);
    controller_prepare_reconfigure_input();
    controller_end_reconfigure(&controller);
    *out_generation_after = dsd_rtl_stream_output_generation();
    *out_used_after = ring_used(&output);

    ring_clear(&output);
    if (initialized_output) {
        output_cleanup(&output);
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_test_retune_output_pending(size_t queued_samples, int cached_symbols, size_t* out_ring_pending,
                                          int* out_cache_pending, int* out_drained) {
    if (!out_ring_pending || !out_cache_pending || !out_drained || cached_symbols < 0) {
        return -1;
    }

    int initialized_output = 0;
    if (!output.buffer) {
        output_init(&output);
        initialized_output = 1;
    }
    if (!output.buffer || output.capacity == 0U) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -2;
    }
    if (queued_samples >= output.capacity) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -3;
    }

    ring_clear(&output);
    output.tail.store(0);
    output.head.store(queued_samples);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(cached_symbols);

    retune_output_pending(&output, out_ring_pending, out_cache_pending);
    *out_drained = retune_output_drained(&output);

    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    ring_clear(&output);
    if (initialized_output) {
        output_cleanup(&output);
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_test_tune_result_output_drain(int tune_result, size_t queued_samples, int cached_symbols,
                                             size_t* out_used_after, int* out_cache_pending_after,
                                             uint32_t* out_generation_before, uint32_t* out_generation_after) {
    if (!out_used_after || !out_cache_pending_after || !out_generation_before || !out_generation_after
        || cached_symbols < 0) {
        return -1;
    }

    int initialized_output = 0;
    if (!output.buffer) {
        output_init(&output);
        initialized_output = 1;
    }
    if (!output.buffer || output.capacity == 0U) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -2;
    }
    if (queued_samples >= output.capacity) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -3;
    }

    ring_clear(&output);
    output.tail.store(0);
    output.head.store(queued_samples);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(cached_symbols);

    *out_generation_before = dsd_rtl_stream_output_generation();
    if (rtl_stream_tune_result_needs_output_drain(tune_result)) {
        drain_output_on_retune();
    }
    *out_generation_after = dsd_rtl_stream_output_generation();
    *out_used_after = ring_used(&output);
    *out_cache_pending_after = dsd_rtl_stream_metrics_hook_symbol_cache_pending();

    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    ring_clear(&output);
    if (initialized_output) {
        output_cleanup(&output);
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_test_clear_output(size_t queued_samples, int cached_symbols, size_t* out_used_after,
                                 int* out_cache_pending_after, uint32_t* out_generation_before,
                                 uint32_t* out_generation_after) {
    if (!out_used_after || !out_cache_pending_after || !out_generation_before || !out_generation_after
        || cached_symbols < 0) {
        return -1;
    }

    int initialized_output = 0;
    if (!output.buffer) {
        output_init(&output);
        initialized_output = 1;
    }
    if (!output.buffer || output.capacity == 0U) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -2;
    }
    if (queued_samples >= output.capacity) {
        if (initialized_output) {
            output_cleanup(&output);
        }
        return -3;
    }

    ring_clear(&output);
    output.tail.store(0);
    output.head.store(queued_samples);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(cached_symbols);

    *out_generation_before = dsd_rtl_stream_output_generation();
    dsd_rtl_stream_clear_output();
    *out_generation_after = dsd_rtl_stream_output_generation();
    *out_used_after = ring_used(&output);
    *out_cache_pending_after = dsd_rtl_stream_metrics_hook_symbol_cache_pending();

    ring_clear(&output);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    if (initialized_output) {
        output_cleanup(&output);
    }
    return 0;
}

static void
fsk_reacquire_test_cleanup_output_ring(int initialized_output) {
    if (initialized_output) {
        output_cleanup(&output);
    }
}

static int
fsk_reacquire_test_prepare_output_ring(size_t queued_samples, int* initialized_output) {
    *initialized_output = 0;
    if (!output.buffer) {
        output_init(&output);
        *initialized_output = 1;
    }
    if (!output.buffer || output.capacity == 0U) {
        return -2;
    }
    if (queued_samples >= output.capacity) {
        return -3;
    }
    return 0;
}

static void
fsk_reacquire_test_reset_output_state(void) {
    ring_clear(&output);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
}

static int
fsk_reacquire_test_request_state_valid(size_t queued_samples, int cached_symbols, uint32_t generation_before) {
    size_t used_after_request = ring_used(&output);
    int cache_after_request = dsd_rtl_stream_metrics_hook_symbol_cache_pending();
    uint32_t generation_after_request = dsd_rtl_stream_output_generation();
    return (used_after_request == queued_samples && cache_after_request == cached_symbols
            && generation_after_request == generation_before)
               ? 1
               : 0;
}

extern "C" int
dsd_rtl_stream_test_fsk_reacquire(int output_kind, size_t queued_samples, int cached_symbols, size_t* out_used_after,
                                  int* out_cache_pending_after, uint32_t* out_generation_before,
                                  uint32_t* out_generation_after, int* out_request_rc, int* out_consumed) {
    if (!out_used_after || !out_cache_pending_after || !out_generation_before || !out_generation_after
        || !out_request_rc || !out_consumed || cached_symbols < 0) {
        return -1;
    }

    int initialized_output = 0;
    int prepare_rc = fsk_reacquire_test_prepare_output_ring(queued_samples, &initialized_output);
    if (prepare_rc != 0) {
        fsk_reacquire_test_cleanup_output_ring(initialized_output);
        return prepare_rc;
    }

    int prev_output_kind = demod.output_kind;
    if (demod.fsk_modem_state.pending_heap) {
        fsk_reacquire_test_cleanup_output_ring(initialized_output);
        return -4;
    }
    dsd_fsk_modem_state prev_modem = demod.fsk_modem_state;
    demod.output_kind = output_kind;
    demod.fsk_modem_state.have_prev = 1;
    demod.fsk_modem_state.timing_acquired = 1;
    demod.fsk_modem_state.track_len = 9;
    g_fsk_reacquire_pending.store(0, std::memory_order_release);

    ring_clear(&output);
    output.tail.store(0);
    output.head.store(queued_samples);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(cached_symbols);

    *out_generation_before = dsd_rtl_stream_output_generation();
    *out_request_rc = dsd_rtl_stream_request_fsk_reacquire();
    if (*out_request_rc > 0
        && !fsk_reacquire_test_request_state_valid(queued_samples, cached_symbols, *out_generation_before)) {
        demod.output_kind = prev_output_kind;
        demod.fsk_modem_state = prev_modem;
        g_fsk_reacquire_pending.store(0, std::memory_order_release);
        fsk_reacquire_test_reset_output_state();
        fsk_reacquire_test_cleanup_output_ring(initialized_output);
        return -5;
    }
    *out_consumed = rtl_stream_consume_fsk_reacquire_pending(&demod);
    *out_generation_after = dsd_rtl_stream_output_generation();
    *out_used_after = ring_used(&output);
    *out_cache_pending_after = dsd_rtl_stream_metrics_hook_symbol_cache_pending();

    demod.output_kind = prev_output_kind;
    demod.fsk_modem_state = prev_modem;
    g_fsk_reacquire_pending.store(0, std::memory_order_release);
    fsk_reacquire_test_reset_output_state();
    fsk_reacquire_test_cleanup_output_ring(initialized_output);
    return 0;
}

extern "C" int
dsd_rtl_stream_test_retune_profile_request_binding(int* out_first_profile, int* out_second_profile,
                                                   uint32_t* out_first_freq_hz, uint32_t* out_second_freq_hz,
                                                   uint32_t* out_first_request_id, uint32_t* out_second_request_id) {
    if (!out_first_profile || !out_second_profile || !out_first_freq_hz || !out_second_freq_hz || !out_first_request_id
        || !out_second_request_id) {
        return -1;
    }
    *out_first_profile = -1;
    *out_second_profile = -1;
    *out_first_freq_hz = 0U;
    *out_second_freq_hz = 0U;
    *out_first_request_id = 0U;
    *out_second_request_id = 0U;

    controller_state test_controller = {};
    controller_init(&test_controller);
    dsd_rtl_stream_clear_pending_retune_profile();

    dsd_rtl_stream_prepare_retune_profile_for_target(855000000U, 1, 6000, 4, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK, 8,
                                                     1);
    uint32_t unrelated_request_id = schedule_manual_retune_on_controller(&test_controller, 851000000U);
    uint32_t first_request_id = schedule_manual_retune_on_controller(&test_controller, 855000000U);
    ControllerRetuneWork first = {};
    if (!controller_wait_for_retune_work(&test_controller, &first) || !first.manual_pending) {
        controller_cleanup(&test_controller);
        return -2;
    }

    dsd_rtl_stream_prepare_retune_profile_for_target(851000000U, 0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM, 5, 0);
    uint32_t second_request_id = schedule_manual_retune_on_controller(&test_controller, 851000000U);
    ControllerRetuneWork second = {};
    if (!controller_wait_for_retune_work(&test_controller, &second) || !second.manual_pending) {
        controller_cleanup(&test_controller);
        return -3;
    }

    *out_first_profile = first.manual_profile.channel_profile;
    *out_second_profile = second.manual_profile.channel_profile;
    *out_first_freq_hz = first.manual_profile.target_freq_hz;
    *out_second_freq_hz = second.manual_profile.target_freq_hz;
    *out_first_request_id = first.manual_profile.request_id;
    *out_second_request_id = second.manual_profile.request_id;

    controller_cleanup(&test_controller);
    if (unrelated_request_id != first_request_id || *out_first_request_id != first_request_id
        || *out_second_request_id != second_request_id) {
        return -4;
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_test_retune_profile_coalesced_no_profile(int* out_profile, uint32_t* out_profile_freq_hz,
                                                        uint32_t* out_manual_freq_hz, uint32_t* out_request_id,
                                                        uint32_t* out_coalesced_request_id) {
    if (!out_profile || !out_profile_freq_hz || !out_manual_freq_hz || !out_request_id || !out_coalesced_request_id) {
        return -1;
    }
    *out_profile = -1;
    *out_profile_freq_hz = 0U;
    *out_manual_freq_hz = 0U;
    *out_request_id = 0U;
    *out_coalesced_request_id = 0U;

    controller_state test_controller = {};
    controller_init(&test_controller);
    dsd_rtl_stream_clear_pending_retune_profile();

    dsd_rtl_stream_prepare_retune_profile_for_target(855000000U, 1, 6000, 4, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK, 8,
                                                     1);
    uint32_t first_request_id = schedule_manual_retune_on_controller(&test_controller, 855000000U);
    uint32_t second_request_id = schedule_manual_retune_on_controller(&test_controller, 855000000U);
    ControllerRetuneWork work = {};
    if (!controller_wait_for_retune_work(&test_controller, &work) || !work.manual_pending) {
        controller_cleanup(&test_controller);
        return -2;
    }

    *out_profile = work.manual_profile.channel_profile;
    *out_profile_freq_hz = work.manual_profile.target_freq_hz;
    *out_manual_freq_hz = work.manual_freq_hz;
    *out_request_id = work.manual_profile.request_id;
    *out_coalesced_request_id = second_request_id;

    controller_cleanup(&test_controller);
    if (second_request_id != first_request_id) {
        return -3;
    }
    return 0;
}

extern "C" int
dsd_rtl_stream_test_get_replay_state(rtl_stream_test_replay_state* out_state) {
    if (!out_state || !g_stream) {
        return -1;
    }

    out_state->replay_input_eof = g_stream->replay_input_eof.load(std::memory_order_acquire);
    out_state->replay_input_drained = g_stream->replay_input_drained.load(std::memory_order_acquire);
    out_state->replay_demod_drained = g_stream->replay_demod_drained.load(std::memory_order_acquire);
    out_state->replay_output_drained = g_stream->replay_output_drained.load(std::memory_order_acquire);
    out_state->replay_forced_stop = g_stream->replay_forced_stop.load(std::memory_order_acquire);
    out_state->should_exit = g_stream->should_exit.load(std::memory_order_acquire);
    out_state->replay_last_submit_gen = g_stream->replay_last_submit_gen.load(std::memory_order_acquire);
    out_state->replay_last_submit_gen_at_eof = g_stream->replay_last_submit_gen_at_eof.load(std::memory_order_acquire);
    out_state->replay_last_consume_gen = g_stream->replay_last_consume_gen.load(std::memory_order_acquire);
    out_state->input_ring_used = input_ring_used(&input_ring);
    out_state->output_ring_used = ring_used(&output);
    out_state->replay_event_retune_count = g_replay_event_retune_count.load(std::memory_order_acquire);
    out_state->replay_event_mute_count = g_replay_event_mute_count.load(std::memory_order_acquire);
    out_state->replay_event_reset_count = g_replay_event_reset_count.load(std::memory_order_acquire);
    out_state->replay_event_last_frequency_hz = g_replay_event_last_frequency_hz.load(std::memory_order_acquire);
    out_state->replay_event_last_mute_bytes = g_replay_event_last_mute_bytes.load(std::memory_order_acquire);
    out_state->replay_event_last_reset_reason = g_replay_event_last_reset_reason.load(std::memory_order_acquire);
    out_state->replay_loop_restart_count = g_replay_loop_restart_count.load(std::memory_order_acquire);
    out_state->replay_loop_restart_last_frequency_hz =
        g_replay_loop_restart_last_frequency_hz.load(std::memory_order_acquire);
    return 0;
}

extern "C" int
dsd_rtl_stream_test_steady_state_watermark_enabled(const char* audio_in_dev) {
    dsd_opts opts = {};
    if (audio_in_dev && audio_in_dev[0] != '\0') {
        DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", audio_in_dev);
    }
    return stream_steady_state_watermark_enabled(&opts);
}
#endif

extern "C" int
dsd_rtl_stream_get_last_applied_freq(uint32_t* out_freq_hz) {
    if (!out_freq_hz) {
        return -1;
    }
    *out_freq_hz = controller.last_applied_freq_hz.load(std::memory_order_acquire);
    return 0;
}

/**
 * @brief Return mean power approximation (RMS^2 proxy) for soft squelch decisions.
 * Returns the post-channel-filter power computed in full_demod().
 *
 * @return Mean power value (approximate RMS squared) after channel filtering.
 */
extern "C" double
dsd_rtl_stream_return_pwr(void) {
    return (double)demod.channel_pwr;
}

/**
 * @brief Set the channel squelch level in the demod state.
 */
extern "C" void
dsd_rtl_stream_set_channel_squelch(float level) {
    demod.channel_squelch_level = level;
}

/**
 * @brief Clear the output ring buffer and wake any waiting producer.
 */
static void
rtl_stream_signal_output_waiters(struct output_state* outp) {
    if (!outp || !outp->buffer) {
        return;
    }
    dsd_mutex_lock(&outp->ready_m);
    dsd_cond_broadcast(&outp->space);
    dsd_cond_broadcast(&outp->ready);
    dsd_mutex_unlock(&outp->ready_m);
}

static void
rtl_stream_clear_output_ring(struct output_state* outp, int bump_generation) {
    if (!outp || !outp->buffer) {
        return;
    }
    if (bump_generation) {
        /* Invalidate decoder-owned cached symbols before clearing the shared output. */
        rtl_stream_bump_output_generation();
    }
    /* Clear the entire ring to prevent sample 'lag' */
    ring_clear(outp);
    dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset();
    rtl_stream_signal_output_waiters(outp);
}

extern "C" void
dsd_rtl_stream_clear_output(void) {
    struct output_state* outp = &output;
    if (g_stream && g_stream->output) {
        outp = g_stream->output;
    }
    rtl_stream_clear_output_ring(outp, 1);
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
