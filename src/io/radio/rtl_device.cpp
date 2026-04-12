// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR device I/O implementation and USB ingestion pipeline.
 *
 * Implements the opaque `rtl_device` handle, device configuration helpers,
 * realtime threading hooks, and the asynchronous USB callback that widens
 * u8 I/Q samples into normalized float and feeds the `input_ring_state`.
 */

#include <atomic>
#include <cinttypes>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/dsp/simd_widen.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/platform/platform.h"
#include "rtl_capture_phase.h"
#include "rtl_replay_device.h"

#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
#include <excpt.h>
#endif
/* Some platforms (e.g. non-glibc) may not define MSG_NOSIGNAL */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include <stdint.h>

#ifdef USE_SOAPYSDR
#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.hpp>
#include <complex>
#include <exception>
#include <string>
#include <vector>

namespace SoapySDR {
class Stream;
} // namespace SoapySDR
#else
namespace SoapySDR {
class Device;
class Stream;
} // namespace SoapySDR
#endif

#ifdef USE_RTLSDR
#include <rtl-sdr.h>
#else
struct rtlsdr_dev;
typedef struct rtlsdr_dev rtlsdr_dev_t;

#ifndef RTLSDR_TUNER_E4000
#define RTLSDR_TUNER_UNKNOWN 0
#define RTLSDR_TUNER_E4000   1
#define RTLSDR_TUNER_FC0012  2
#define RTLSDR_TUNER_FC0013  3
#define RTLSDR_TUNER_FC2580  4
#define RTLSDR_TUNER_R820T   5
#define RTLSDR_TUNER_R828D   6
#endif

static int
rtlsdr_open(rtlsdr_dev_t** dev, uint32_t index) {
    (void)dev;
    (void)index;
    return -1;
}

static int
rtlsdr_close(rtlsdr_dev_t* dev) {
    (void)dev;
    return -1;
}

static int
rtlsdr_read_async(rtlsdr_dev_t* dev, void (*cb)(unsigned char*, uint32_t, void*), void* ctx, uint32_t buf_num,
                  uint32_t buf_len) {
    (void)dev;
    (void)cb;
    (void)ctx;
    (void)buf_num;
    (void)buf_len;
    return -1;
}

static int
rtlsdr_cancel_async(rtlsdr_dev_t* dev) {
    (void)dev;
    return -1;
}

static int
rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t* dev, int manual) {
    (void)dev;
    (void)manual;
    return -1;
}

static int
rtlsdr_get_tuner_gains(rtlsdr_dev_t* dev, int* gains) {
    (void)dev;
    (void)gains;
    return -1;
}

static int
rtlsdr_set_center_freq(rtlsdr_dev_t* dev, uint32_t freq) {
    (void)dev;
    (void)freq;
    return -1;
}

static int
rtlsdr_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate) {
    (void)dev;
    (void)rate;
    return -1;
}

static int
rtlsdr_set_direct_sampling(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return -1;
}

static int
rtlsdr_get_tuner_type(rtlsdr_dev_t* dev) {
    (void)dev;
    return RTLSDR_TUNER_UNKNOWN;
}

static int
rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t* dev, int bw_hz) {
    (void)dev;
    (void)bw_hz;
    return -1;
}

static int
rtlsdr_set_agc_mode(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return -1;
}

static int
rtlsdr_set_tuner_gain(rtlsdr_dev_t* dev, int gain) {
    (void)dev;
    (void)gain;
    return -1;
}

static int
rtlsdr_set_freq_correction(rtlsdr_dev_t* dev, int ppm) {
    (void)dev;
    (void)ppm;
    return -1;
}

static int
rtlsdr_reset_buffer(rtlsdr_dev_t* dev) {
    (void)dev;
    return -1;
}

static uint32_t
rtlsdr_get_sample_rate(rtlsdr_dev_t* dev) {
    (void)dev;
    return 0;
}

static int
rtlsdr_get_tuner_gain(rtlsdr_dev_t* dev) {
    (void)dev;
    return -1;
}

static int
rtlsdr_set_offset_tuning(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return -1;
}

static int
rtlsdr_set_xtal_freq(rtlsdr_dev_t* dev, uint32_t rtl, uint32_t tuner) {
    (void)dev;
    (void)rtl;
    (void)tuner;
    return -1;
}

static int
rtlsdr_set_testmode(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return -1;
}

static int
rtlsdr_set_tuner_if_gain(rtlsdr_dev_t* dev, int stage, int gain) {
    (void)dev;
    (void)stage;
    (void)gain;
    return -1;
}

#ifdef USE_RTLSDR_BIAS_TEE
static int
rtlsdr_set_bias_tee(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return -1;
}
#endif
#endif

// Shutdown signaling (defined in src/runtime/exitflag.c)
extern "C" volatile uint8_t exitflag;
/* Capture-shift override (defined in rtl_demod_config.cpp). */
extern int disable_fs4_shift;

enum : int {
    RTL_BACKEND_USB = 0,
    RTL_BACKEND_TCP = 1,
    RTL_BACKEND_SOAPY = 2,
    RTL_BACKEND_IQ_REPLAY = 3,
};

enum : int {
    SOAPY_FMT_NONE = 0,
    SOAPY_FMT_CF32 = 1,
    SOAPY_FMT_CS16 = 2,
};

// Internal RTL device structure
struct rtl_device {
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
    dsd_thread_t thread;
    int thread_started;
    struct input_ring_state* input_ring;
    int combine_rotate_enabled;
    struct dsd_iq_capture_writer* iq_capture_writer;
    std::atomic<uint32_t> capture_retune_count;
    /* Backend selector: 0 = USB (librtlsdr), 1 = rtl_tcp, 2 = SoapySDR */
    int backend;
    int replay_fs4_shift_enabled;
    int replay_combine_rotate_enabled;
    dsd_iq_replay_config replay_cfg;
    dsd_iq_replay_source* replay_src;
    struct rtl_replay_eof_state replay_eof;
    int replay_has_eof_state;
    uint64_t replay_float_elements_written;
    /* SoapySDR backend */
    SoapySDR::Device* soapy_dev;
    SoapySDR::Stream* soapy_stream;
    dsd_mutex_t soapy_lock;
    int soapy_lock_inited;
    int soapy_format; /* SOAPY_FMT_* */
    uint32_t soapy_mtu_elems;
    int rot_phase;                                  /* persistent j^n phase in [0..3] for capture-side FS/4 rotation */
    struct rtl_capture_u8_byte_carry iq_byte_carry; /* one buffered raw byte when a chunk ends mid-I/Q sample */
    std::atomic<int> mute_byte_phase;               /* byte carry while an active mute span is discarded in fragments */
    uint64_t soapy_overflow_count;
    uint64_t soapy_timeout_count;
    uint64_t soapy_read_errors;
    uint64_t soapy_last_overflow_log_ns;
    /* rtl_tcp connection */
    dsd_socket_t sockfd;
    char host[1024];
    int port;
    std::atomic<int> run;
    int agc_mode; /* cached for TCP backend */
    int bias_tee_on;
    int tcp_autotune; /* adaptive recv/buffering */
    /* TCP stats (optional) */
    uint64_t tcp_bytes_total;
    uint64_t tcp_bytes_window;
    uint64_t reserve_full_events;
    int stats_enabled;
    uint64_t stats_last_ns;
    /* TCP reassembly to uniform chunk size */
    unsigned char* tcp_pending;
    size_t tcp_pending_len;
    size_t tcp_pending_cap;
    /* Extra driver state for reconnect replay */
    int testmode_on;
    uint32_t rtl_xtal_hz;
    uint32_t tuner_xtal_hz;

    struct {
        int stage;
        int gain;
    } if_gains[16];

    int if_gain_count;
};

/**
 * Hint that a pointer is aligned to a compile-time boundary for vectorization.
 *
 * This is a lightweight wrapper over compiler intrinsics to improve
 * auto-vectorization by promising the compiler that the pointer meets the
 * specified alignment. Use with care and only when the alignment guarantee
 * is actually met.
 */
#ifndef DSD_NEO_RESTRICT
#if defined(_MSC_VER)
#define DSD_NEO_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_RESTRICT __restrict__
#else
#define DSD_NEO_RESTRICT
#endif
#endif

/**
 * @brief Optionally enable realtime scheduling and set CPU affinity for the current
 * thread based on environment variables.
 *
 * When `DSD_NEO_RT_SCHED=1`, attempts to switch the calling thread to
 * SCHED_FIFO with a priority derived from `DSD_NEO_RT_PRIO_<ROLE>` if present.
 * If `DSD_NEO_CPU_<ROLE>` is set to a valid CPU index, pins the thread to that
 * CPU.
 *
 * @param role Optional role label (e.g. "DEMOD", "DONGLE") used to look up
 *             per-role environment variables.
 */

static inline int
fs4_shift_capture_active(const struct rtl_device* s) {
    return (s && !s->offset_tuning && !disable_fs4_shift) ? 1 : 0;
}

static inline void
rtl_get_u8_transform_policy(const struct rtl_device* s, int* out_fs4_active, int* out_combine_active,
                            int* out_use_two_pass) {
    int fs4_active = 0;
    int combine_active = 0;
    if (s && s->backend == RTL_BACKEND_IQ_REPLAY) {
        fs4_active = s->replay_fs4_shift_enabled ? 1 : 0;
        combine_active = s->replay_combine_rotate_enabled ? 1 : 0;
    } else {
        fs4_active = fs4_shift_capture_active(s);
        combine_active = (s && s->combine_rotate_enabled) ? 1 : 0;
    }
    if (out_fs4_active) {
        *out_fs4_active = fs4_active;
    }
    if (out_combine_active) {
        *out_combine_active = combine_active;
    }
    if (out_use_two_pass) {
        *out_use_two_pass = (fs4_active && !combine_active) ? 1 : 0;
    }
}

static inline int
rtl_process_u8_chunk(struct rtl_device* s, unsigned char* src, float* dst, size_t len, int fs4_shift_active,
                     int combine_rotate_active, int use_two_pass, int* phase) {
    if (!s || !src || !dst || len == 0) {
        return phase ? *phase : 0;
    }
    int cur_phase = phase ? (*phase & 3) : 0;
    if (fs4_shift_active && combine_rotate_active) {
        cur_phase = (int)widen_rotate90_u8_to_f32_bias127_phase(src, dst, (uint32_t)len, (uint32_t)cur_phase);
    } else if (use_two_pass) {
        cur_phase = (int)rotate90_u8_inplace_phase(src, (uint32_t)len, (uint32_t)cur_phase);
        widen_u8_to_f32_bias128_scalar(src, dst, (uint32_t)len);
    } else {
        widen_u8_to_f32_bias127(src, dst, (uint32_t)len);
    }
    if (phase) {
        *phase = cur_phase;
    }
    return cur_phase;
}

static inline size_t
rtl_drop_u8_bytes_preserve_alignment(const unsigned char* src, size_t byte_count,
                                     struct rtl_capture_u8_byte_carry* carry, int* phase, int fs4_shift_active) {
    size_t dropped = rtl_capture_u8_byte_carry_drop_aligned(src, byte_count, carry);
    if (fs4_shift_active && phase && dropped != 0) {
        *phase = rtl_capture_phase_advance_pairs(*phase, dropped >> 1);
    }
    return dropped;
}

static inline void
rtl_prepare_fragmented_u8_mute(struct rtl_device* s) {
    if (!s || s->mute.load(std::memory_order_relaxed) <= 0 || !s->iq_byte_carry.valid) {
        return;
    }

    unsigned int carry = (unsigned int)(s->mute_byte_phase.load(std::memory_order_relaxed) & 1U);
    int remaining = s->mute.load(std::memory_order_relaxed);
    rtl_capture_u8_byte_carry_clear(&s->iq_byte_carry);
    if (carry == 0U) {
        carry = 1U;
        if ((remaining & 1) == 0 && remaining < INT_MAX) {
            s->mute.fetch_add(1, std::memory_order_relaxed);
        }
    }
    s->mute_byte_phase.store((int)carry, std::memory_order_relaxed);
}

static inline int
rtl_account_fragmented_muted_u8_bytes(struct rtl_device* s, size_t discarded_bytes, int fs4_shift_active) {
    if (!s || discarded_bytes == 0) {
        return 0;
    }
    unsigned int carry = (unsigned int)(s->mute_byte_phase.load(std::memory_order_relaxed) & 1U);
    if (fs4_shift_active) {
        s->rot_phase = rtl_capture_phase_advance_u8_bytes_fragmented(s->rot_phase & 3, discarded_bytes, &carry);
    } else {
        (void)rtl_capture_phase_advance_u8_bytes_fragmented(0, discarded_bytes, &carry);
    }
    s->mute_byte_phase.store((int)carry, std::memory_order_relaxed);
    return fs4_shift_active ? s->rot_phase : 0;
}

static inline void
rtl_reset_capture_state_on_stream_boundary(struct rtl_device* s) {
    if (!s) {
        return;
    }

    unsigned int carry = (unsigned int)(s->mute_byte_phase.load(std::memory_order_relaxed) & 1U);
    int remaining = s->mute.load(std::memory_order_relaxed);
    for (;;) {
        int realigned =
            rtl_capture_restart_u8_stream_with_pending(&s->rot_phase, remaining, &carry, &s->tcp_pending_len);
        if (realigned == remaining) {
            break;
        }
        if (s->mute.compare_exchange_weak(remaining, realigned, std::memory_order_relaxed)) {
            break;
        }
    }
    rtl_capture_u8_byte_carry_clear(&s->iq_byte_carry);
    s->mute_byte_phase.store((int)carry, std::memory_order_relaxed);
}

static int
rtl_write_u8_to_ring(struct rtl_device* s, unsigned char* src, size_t len, int fs4_shift_active, int use_two_pass,
                     int combine_rotate_active, int count_full_reserve) {
    if (!s || !s->input_ring || !src || len == 0) {
        return 0;
    }

    size_t done = 0;
    size_t need = len;
    int phase = s->rot_phase & 3;
    struct rtl_capture_u8_byte_carry carry = s->iq_byte_carry;
    int ring_exhausted = 0;

    while (rtl_capture_u8_byte_carry_ready_bytes(need, &carry) >= 2U) {
        float *p1 = NULL, *p2 = NULL;
        size_t n1 = 0, n2 = 0;
        size_t ready = rtl_capture_u8_byte_carry_ready_bytes(need, &carry);
        input_ring_reserve(s->input_ring, ready, &p1, &n1, &p2, &n2);
        if (n1 == 0 && n2 == 0) {
            ring_exhausted = 1;
            break;
        }

        if (n1 & 1U) {
            n1--;
        }
        size_t w1 = (n1 < ready) ? n1 : ready;
        size_t rem_after_w1 = ready - w1;
        if (n2 & 1U) {
            n2--;
        }
        size_t w2 = (n2 < rem_after_w1) ? n2 : rem_after_w1;
        size_t produced = w1 + w2;

        if (produced == 0) {
            ring_exhausted = 1;
            break;
        }

        size_t consumed = 0;
        if (w1) {
            unsigned char pair[2];
            size_t prefix = rtl_capture_u8_byte_carry_consume_prefix(src + done, need, &carry, pair);
            if (prefix != 0U) {
                rtl_process_u8_chunk(s, pair, p1, 2U, fs4_shift_active, combine_rotate_active, use_two_pass, &phase);
                done += prefix;
                need -= prefix;
                consumed += 2U;
            }
            if (w1 > consumed) {
                size_t body = w1 - consumed;
                rtl_process_u8_chunk(s, src + done, p1 + consumed, body, fs4_shift_active, combine_rotate_active,
                                     use_two_pass, &phase);
                done += body;
                need -= body;
            }
        }
        if (w2) {
            unsigned char pair[2];
            size_t prefix = rtl_capture_u8_byte_carry_consume_prefix(src + done, need, &carry, pair);
            size_t produced_w2 = 0;
            if (prefix != 0U) {
                rtl_process_u8_chunk(s, pair, p2, 2U, fs4_shift_active, combine_rotate_active, use_two_pass, &phase);
                done += prefix;
                need -= prefix;
                produced_w2 += 2U;
            }
            if (w2 > produced_w2) {
                size_t body = w2 - produced_w2;
                rtl_process_u8_chunk(s, src + done, p2 + produced_w2, body, fs4_shift_active, combine_rotate_active,
                                     use_two_pass, &phase);
                done += body;
                need -= body;
            }
        }

        input_ring_commit(s->input_ring, produced);
    }

    if (ring_exhausted) {
        size_t dropped = rtl_drop_u8_bytes_preserve_alignment(src + done, need, &carry, &phase, fs4_shift_active);
        if (dropped != 0U) {
            s->input_ring->producer_drops.fetch_add((uint64_t)dropped);
        }
        if (count_full_reserve) {
            s->reserve_full_events++;
        }
    } else if (need == 1U && !carry.valid) {
        rtl_capture_u8_byte_carry_save(&carry, src[done]);
    }

    s->iq_byte_carry = carry;
    if (fs4_shift_active) {
        s->rot_phase = phase;
    }
    return ring_exhausted;
}

static inline void
rtl_submit_capture_bytes(struct rtl_device* s, const void* data, size_t bytes) {
    if (!s || !s->iq_capture_writer || !data || bytes == 0) {
        return;
    }
    (void)dsd_iq_capture_submit(s->iq_capture_writer, data, bytes);
}

static inline void
rtl_apply_j4_rotation(float in_i, float in_q, int phase, float* out_i, float* out_q) {
    switch (phase & 3) {
        case 0:
            *out_i = in_i;
            *out_q = in_q;
            break;
        case 1:
            *out_i = -in_q;
            *out_q = in_i;
            break;
        case 2:
            *out_i = -in_i;
            *out_q = -in_q;
            break;
        default:
            *out_i = in_q;
            *out_q = -in_i;
            break;
    }
}

#ifdef USE_SOAPYSDR
static inline void
apply_j4_rotation(float in_i, float in_q, int phase, float* out_i, float* out_q) {
    rtl_apply_j4_rotation(in_i, in_q, phase, out_i, out_q);
}

static size_t
soapy_write_cf32_to_ring(struct rtl_device* s, const std::complex<float>* src, size_t num_elems, int apply_rot) {
    if (!s || !s->input_ring || !src || num_elems == 0) {
        return 0;
    }
    size_t need = num_elems * 2;
    size_t done = 0;
    int phase = s->rot_phase & 3;
    while (need > 0) {
        float *p1 = NULL, *p2 = NULL;
        size_t n1 = 0, n2 = 0;
        input_ring_reserve(s->input_ring, need, &p1, &n1, &p2, &n2);
        if (n1 == 0 && n2 == 0) {
            s->input_ring->producer_drops.fetch_add((uint64_t)need);
            if (apply_rot) {
                phase = rtl_capture_phase_advance_pairs(phase, need / 2);
            }
            break;
        }
        if (n1 & 1) {
            n1--;
        }
        size_t w1 = (n1 < need) ? n1 : need;
        size_t rem = need - w1;
        if (n2 & 1) {
            n2--;
        }
        size_t w2 = (n2 < rem) ? n2 : rem;
        size_t w1_elems = w1 / 2;
        size_t w2_elems = w2 / 2;
        size_t src_idx = done / 2;
        if (w1_elems > 0) {
            for (size_t i = 0; i < w1_elems; i++) {
                float i_in = src[src_idx + i].real();
                float q_in = src[src_idx + i].imag();
                if (apply_rot) {
                    apply_j4_rotation(i_in, q_in, phase, &p1[(i * 2) + 0], &p1[(i * 2) + 1]);
                    phase = (phase + 1) & 3;
                } else {
                    p1[(i * 2) + 0] = i_in;
                    p1[(i * 2) + 1] = q_in;
                }
            }
        }
        src_idx += w1_elems;
        if (w2_elems > 0) {
            for (size_t i = 0; i < w2_elems; i++) {
                float i_in = src[src_idx + i].real();
                float q_in = src[src_idx + i].imag();
                if (apply_rot) {
                    apply_j4_rotation(i_in, q_in, phase, &p2[(i * 2) + 0], &p2[(i * 2) + 1]);
                    phase = (phase + 1) & 3;
                } else {
                    p2[(i * 2) + 0] = i_in;
                    p2[(i * 2) + 1] = q_in;
                }
            }
        }
        input_ring_commit(s->input_ring, w1 + w2);
        done += w1 + w2;
        need -= w1 + w2;
    }
    if (apply_rot) {
        s->rot_phase = phase;
    }
    return done / 2;
}

static size_t
soapy_write_cs16_to_ring(struct rtl_device* s, const int16_t* src, size_t num_elems, int apply_rot) {
    if (!s || !s->input_ring || !src || num_elems == 0) {
        return 0;
    }
    const float scale = 1.0f / 32768.0f;
    size_t need = num_elems * 2;
    size_t done = 0;
    int phase = s->rot_phase & 3;
    while (need > 0) {
        float *p1 = NULL, *p2 = NULL;
        size_t n1 = 0, n2 = 0;
        input_ring_reserve(s->input_ring, need, &p1, &n1, &p2, &n2);
        if (n1 == 0 && n2 == 0) {
            s->input_ring->producer_drops.fetch_add((uint64_t)need);
            if (apply_rot) {
                phase = rtl_capture_phase_advance_pairs(phase, need / 2);
            }
            break;
        }
        if (n1 & 1) {
            n1--;
        }
        size_t w1 = (n1 < need) ? n1 : need;
        size_t rem = need - w1;
        if (n2 & 1) {
            n2--;
        }
        size_t w2 = (n2 < rem) ? n2 : rem;
        size_t w1_elems = w1 / 2;
        size_t w2_elems = w2 / 2;
        size_t src_idx = done / 2;
        if (w1_elems > 0) {
            for (size_t i = 0; i < w1_elems; i++) {
                size_t sample_idx = (src_idx + i) * 2;
                float i_in = (float)src[sample_idx + 0] * scale;
                float q_in = (float)src[sample_idx + 1] * scale;
                if (apply_rot) {
                    apply_j4_rotation(i_in, q_in, phase, &p1[(i * 2) + 0], &p1[(i * 2) + 1]);
                    phase = (phase + 1) & 3;
                } else {
                    p1[(i * 2) + 0] = i_in;
                    p1[(i * 2) + 1] = q_in;
                }
            }
        }
        src_idx += w1_elems;
        if (w2_elems > 0) {
            for (size_t i = 0; i < w2_elems; i++) {
                size_t sample_idx = (src_idx + i) * 2;
                float i_in = (float)src[sample_idx + 0] * scale;
                float q_in = (float)src[sample_idx + 1] * scale;
                if (apply_rot) {
                    apply_j4_rotation(i_in, q_in, phase, &p2[(i * 2) + 0], &p2[(i * 2) + 1]);
                    phase = (phase + 1) & 3;
                } else {
                    p2[(i * 2) + 0] = i_in;
                    p2[(i * 2) + 1] = q_in;
                }
            }
        }
        input_ring_commit(s->input_ring, w1 + w2);
        done += w1 + w2;
        need -= w1 + w2;
    }
    if (apply_rot) {
        s->rot_phase = phase;
    }
    return done / 2;
}
#endif

static inline int
replay_forced_stop_requested(const struct rtl_device* s) {
    if (!s) {
        return 1;
    }
    if (!s->run.load(std::memory_order_acquire) || exitflag) {
        return 1;
    }
    if (s->replay_has_eof_state && s->replay_eof.replay_forced_stop
        && s->replay_eof.replay_forced_stop->load(std::memory_order_acquire)) {
        return 1;
    }
    if (s->replay_has_eof_state && s->replay_eof.stream_exit_flag
        && s->replay_eof.stream_exit_flag->load(std::memory_order_acquire)) {
        return 1;
    }
    return 0;
}

static inline void
replay_signal_input_waiters(struct rtl_device* s) {
    if (!s || !s->input_ring) {
        return;
    }
    dsd_mutex_lock(&s->input_ring->ready_m);
    dsd_cond_broadcast(&s->input_ring->ready);
    dsd_cond_broadcast(&s->input_ring->space);
    dsd_mutex_unlock(&s->input_ring->ready_m);
}

static int
replay_wait_for_ring_progress(struct rtl_device* s, size_t needed_free, uint64_t deadline_ns, int have_deadline) {
    if (!s || !s->input_ring) {
        return 0;
    }
    struct input_ring_state* ring = s->input_ring;

    for (;;) {
        if (replay_forced_stop_requested(s)) {
            return 0;
        }
        if (input_ring_free(ring) >= needed_free) {
            return 1;
        }

        dsd_mutex_lock(&ring->ready_m);
        while (!replay_forced_stop_requested(s) && input_ring_free(ring) < needed_free) {
            uint64_t wait_deadline = deadline_ns;
            if (!have_deadline) {
                wait_deadline = dsd_time_monotonic_ns() + 50000000ULL;
            } else if (dsd_time_monotonic_ns() >= deadline_ns) {
                dsd_mutex_unlock(&ring->ready_m);
                return 0;
            }
            int rc = dsd_cond_timedwait_monotonic(&ring->space, &ring->ready_m, wait_deadline);
            if (have_deadline && rc == ETIMEDOUT && dsd_time_monotonic_ns() >= deadline_ns) {
                dsd_mutex_unlock(&ring->ready_m);
                return 0;
            }
        }
        dsd_mutex_unlock(&ring->ready_m);
    }
}

static int
replay_wait_until(struct rtl_device* s, uint64_t deadline_ns) {
    if (!s || !s->input_ring) {
        return 0;
    }
    if (dsd_time_monotonic_ns() >= deadline_ns) {
        return 1;
    }
    dsd_mutex_lock(&s->input_ring->ready_m);
    while (!replay_forced_stop_requested(s)) {
        uint64_t now_ns = dsd_time_monotonic_ns();
        if (now_ns >= deadline_ns) {
            dsd_mutex_unlock(&s->input_ring->ready_m);
            return 1;
        }
        (void)dsd_cond_timedwait_monotonic(&s->input_ring->space, &s->input_ring->ready_m, deadline_ns);
    }
    dsd_mutex_unlock(&s->input_ring->ready_m);
    return 0;
}

static int
replay_enqueue_f32_no_drop(struct rtl_device* s, const float* src, size_t float_count, uint64_t* complex_written,
                           uint64_t start_ns, int realtime) {
    if (!s || !s->input_ring || !src || !complex_written) {
        return -1;
    }

    struct input_ring_state* ring = s->input_ring;
    size_t done = 0;
    while (done < float_count) {
        if (replay_forced_stop_requested(s)) {
            return -1;
        }

        if (realtime && s->replay_cfg.sample_rate_hz > 0) {
            uint64_t deadline_ns =
                start_ns + ((*complex_written * 1000000000ULL) / (uint64_t)s->replay_cfg.sample_rate_hz);
            if (!replay_wait_until(s, deadline_ns)) {
                return -1;
            }
        }

        size_t free_sp = input_ring_free(ring);
        if (free_sp < 2U) {
            if (!replay_wait_for_ring_progress(s, 2U, 0, 0)) {
                return -1;
            }
            continue;
        }

        size_t chunk = float_count - done;
        if (chunk > free_sp) {
            chunk = free_sp;
        }
        if (chunk & 1U) {
            chunk--;
        }
        if (chunk == 0U) {
            continue;
        }

        float *p1 = NULL, *p2 = NULL;
        size_t n1 = 0, n2 = 0;
        input_ring_reserve(ring, chunk, &p1, &n1, &p2, &n2);
        size_t grant = n1 + n2;
        if (grant == 0) {
            continue;
        }
        if (grant > chunk) {
            grant = chunk;
        }
        if (grant & 1U) {
            grant--;
        }
        if (grant == 0U) {
            continue;
        }

        size_t first = (n1 < grant) ? n1 : grant;
        if (first & 1U) {
            first--;
        }
        if (first > 0U) {
            memcpy(p1, src + done, first * sizeof(float));
        }
        size_t second = grant - first;
        if (second > 0U) {
            memcpy(p2, src + done + first, second * sizeof(float));
        }

        if (s->replay_has_eof_state && s->replay_eof.replay_last_submit_gen) {
            (void)s->replay_eof.replay_last_submit_gen->fetch_add(1ULL, std::memory_order_release);
        }
        input_ring_commit(ring, grant);
        done += grant;
        *complex_written += grant / 2U;
    }
    return 0;
}

static int
replay_convert_cu8_to_f32(struct rtl_device* s, const uint8_t* in, size_t in_bytes, float* out_f32, size_t out_cap_f32,
                          int* io_phase, int* io_have_carry, uint8_t* io_carry_byte) {
    if (!s || !in || !out_f32 || !io_phase || !io_have_carry || !io_carry_byte) {
        return -1;
    }
    size_t out_pos = 0;
    int fs4_active = 0;
    int combine_active = 0;
    int use_two_pass = 0;
    rtl_get_u8_transform_policy(s, &fs4_active, &combine_active, &use_two_pass);

    size_t consumed = 0;
    if (*io_have_carry && in_bytes > 0) {
        if (out_cap_f32 < 2U) {
            return -1;
        }
        uint8_t pair[2];
        pair[0] = *io_carry_byte;
        pair[1] = in[0];
        rtl_process_u8_chunk(s, pair, out_f32 + out_pos, 2U, fs4_active, combine_active, use_two_pass, io_phase);
        out_pos += 2U;
        consumed = 1U;
        *io_have_carry = 0;
    }

    size_t avail = in_bytes - consumed;
    size_t aligned = avail & ~(size_t)1U;
    if (aligned > 0U) {
        if (out_pos + aligned > out_cap_f32) {
            return -1;
        }
        rtl_process_u8_chunk(s, (unsigned char*)(in + consumed), out_f32 + out_pos, aligned, fs4_active, combine_active,
                             use_two_pass, io_phase);
        out_pos += aligned;
        consumed += aligned;
    }

    if (consumed < in_bytes) {
        *io_carry_byte = in[consumed];
        *io_have_carry = 1;
    }

    return (int)out_pos;
}

static int
replay_convert_cf32_to_f32(struct rtl_device* s, const uint8_t* in, size_t in_bytes, float* out_f32, size_t out_cap_f32,
                           int* io_phase) {
    if (!s || !in || !out_f32 || !io_phase) {
        return -1;
    }
    if ((in_bytes & 7U) != 0U) {
        return -1;
    }

    size_t complex_count = in_bytes / 8U;
    size_t float_count = complex_count * 2U;
    if (float_count > out_cap_f32) {
        return -1;
    }

    if (strcmp(s->replay_cfg.capture_stage, "post_driver_cf32_pre_ring") != 0) {
        return -1;
    }

    if (s->replay_cfg.fs4_shift_enabled) {
        int phase = *io_phase & 3;
        for (size_t i = 0; i < complex_count; i++) {
            float in_i = 0.0f;
            float in_q = 0.0f;
            memcpy(&in_i, in + (i * 8U) + 0U, sizeof(float));
            memcpy(&in_q, in + (i * 8U) + 4U, sizeof(float));
            rtl_apply_j4_rotation(in_i, in_q, phase, &out_f32[i * 2U + 0U], &out_f32[i * 2U + 1U]);
            phase = (phase + 1) & 3;
        }
        *io_phase = phase;
    } else {
        memcpy(out_f32, in, float_count * sizeof(float));
    }

    return (int)float_count;
}

static void
replay_mark_input_eof(struct rtl_device* s) {
    if (!s || !s->replay_has_eof_state) {
        return;
    }
    if (s->replay_eof.replay_last_submit_gen && s->replay_eof.replay_last_submit_gen_at_eof) {
        uint64_t last_gen = s->replay_eof.replay_last_submit_gen->load(std::memory_order_acquire);
        s->replay_eof.replay_last_submit_gen_at_eof->store(last_gen, std::memory_order_release);
    }
    if (s->replay_eof.replay_input_eof) {
        s->replay_eof.replay_input_eof->store(1, std::memory_order_release);
    }
    replay_signal_input_waiters(s);
    if (s->replay_eof.eof_cond && s->replay_eof.eof_m) {
        dsd_mutex_lock(s->replay_eof.eof_m);
        dsd_cond_broadcast(s->replay_eof.eof_cond);
        dsd_mutex_unlock(s->replay_eof.eof_m);
    }
}

static void
replay_wait_for_demod_drain(struct rtl_device* s) {
    if (!s || !s->replay_has_eof_state || !s->replay_eof.eof_m || !s->replay_eof.eof_cond) {
        return;
    }
    dsd_mutex_lock(s->replay_eof.eof_m);
    while (!replay_forced_stop_requested(s)
           && !(s->replay_eof.replay_demod_drained
                && s->replay_eof.replay_demod_drained->load(std::memory_order_acquire))) {
        if (s->replay_eof.replay_forced_stop && s->replay_eof.replay_forced_stop->load(std::memory_order_acquire)) {
            break;
        }
        if (s->replay_eof.stream_exit_flag && s->replay_eof.stream_exit_flag->load(std::memory_order_acquire)) {
            break;
        }
        dsd_cond_wait(s->replay_eof.eof_cond, s->replay_eof.eof_m);
    }
    dsd_mutex_unlock(s->replay_eof.eof_m);
}

static void
replay_handle_eof_sequence(struct rtl_device* s) {
    if (!s || !s->input_ring) {
        return;
    }

    replay_mark_input_eof(s);
    while (!replay_forced_stop_requested(s) && input_ring_used(s->input_ring) > 0U) {
        (void)replay_wait_for_ring_progress(s, 1U, 0, 0);
    }

    if (!replay_forced_stop_requested(s) && s->replay_has_eof_state) {
        if (s->replay_eof.replay_input_drained) {
            s->replay_eof.replay_input_drained->store(1, std::memory_order_release);
        }
        if (s->replay_eof.on_input_drained) {
            s->replay_eof.on_input_drained(s->replay_eof.eof_user);
        }
    }
    replay_wait_for_demod_drain(s);
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    replay_thread_fn(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    if (!s || !s->replay_src || !s->input_ring) {
        if (s) {
            s->run.store(0, std::memory_order_release);
        }
        DSD_THREAD_RETURN;
    }

    maybe_set_thread_realtime_and_affinity("DONGLE");

    const size_t raw_block_bytes = 65536U;
    uint8_t* raw_block = static_cast<uint8_t*>(malloc(raw_block_bytes));
    float* f32_block = static_cast<float*>(malloc(raw_block_bytes * sizeof(float)));
    if (!raw_block || !f32_block) {
        free(raw_block);
        free(f32_block);
        s->run.store(0, std::memory_order_release);
        DSD_THREAD_RETURN;
    }

    int phase = 0;
    int have_carry = 0;
    uint8_t carry_byte = 0;
    uint64_t complex_written = 0;
    uint64_t start_ns = dsd_time_monotonic_ns();
    int realtime = s->replay_cfg.realtime ? 1 : 0;

    while (!replay_forced_stop_requested(s)) {
        size_t out_bytes = 0;
        int rc = dsd_iq_replay_read(s->replay_src, raw_block, raw_block_bytes, &out_bytes);
        if (rc != DSD_IQ_OK) {
            break;
        }
        if (out_bytes == 0U) {
            if (s->replay_cfg.loop) {
                if (dsd_iq_replay_rewind(s->replay_src) != DSD_IQ_OK) {
                    break;
                }
                phase = 0;
                have_carry = 0;
                carry_byte = 0;
                complex_written = 0;
                start_ns = dsd_time_monotonic_ns();
                continue;
            }
            replay_handle_eof_sequence(s);
            break;
        }

        int produced = -1;
        if (s->replay_cfg.format == DSD_IQ_FORMAT_CU8) {
            produced = replay_convert_cu8_to_f32(s, raw_block, out_bytes, f32_block, raw_block_bytes, &phase,
                                                 &have_carry, &carry_byte);
        } else if (s->replay_cfg.format == DSD_IQ_FORMAT_CF32) {
            produced = replay_convert_cf32_to_f32(s, raw_block, out_bytes, f32_block, raw_block_bytes, &phase);
        } else {
            break;
        }
        if (produced <= 0) {
            continue;
        }

        if (replay_enqueue_f32_no_drop(s, f32_block, (size_t)produced, &complex_written, start_ns, realtime) != 0) {
            break;
        }
    }

    free(f32_block);
    free(raw_block);
    s->run.store(0, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/**
 * @brief RTL-SDR asynchronous USB callback.
 * Converts incoming u8 I/Q to normalized float and enqueues into the input ring. If
 * `offset_tuning` is off and `DSD_NEO_COMBINE_ROT` is enabled (default), a
 * combined rotate+widen implementation is used. Otherwise it falls back to
 * a legacy two-pass byte-rotation + bias128 widen path or a simple widen
 * subtracting 127. On overflow, drops oldest ring data to avoid stalls.
 *
 * @param buf USB I/Q byte buffer.
 * @param len Buffer length in bytes (I/Q interleaved).
 * @param ctx Opaque pointer to `rtl_device`.
 */
static void
rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    struct rtl_device* s = static_cast<rtl_device*>(ctx);

    /* One-time: ensure the USB callback thread gets RT scheduling/affinity if enabled */
    {
        static std::atomic<int> usb_sched_applied{0};
        int expected = 0;
        if (usb_sched_applied.compare_exchange_strong(expected, 1)) {
            maybe_set_thread_realtime_and_affinity("USB");
        }
    }

    if (exitflag) {
        return;
    }
    if (!ctx) {
        return;
    }
    int fs4_shift_active = 0;
    int combine_rotate_active = 0;
    int use_two_pass = 0;
    rtl_get_u8_transform_policy(s, &fs4_shift_active, &combine_rotate_active, &use_two_pass);
    /* Handle muting: skip (discard) muted samples entirely instead of zero-filling.
     *
     * Previously we set muted samples to 127 (midpoint), which after bias subtraction
     * becomes 0.0. These zero-magnitude samples corrupt the Costas loop and TED when
     * they're processed after the retune gate opens. By discarding them entirely,
     * the demod thread never sees transient samples.
     *
     * Advance buf pointer and reduce len to skip the muted portion.
     *
     * Note: the stored mute value counts raw stream bytes still to discard. On the
     * rtl_tcp backend that remainder may legitimately be odd between callbacks because
     * recv() can split an aligned mute span across arbitrary byte boundaries. */
    if (s->mute.load(std::memory_order_relaxed) > 0) {
        rtl_prepare_fragmented_u8_mute(s);
        int old = s->mute.load(std::memory_order_relaxed);
        if (old > 0) {
            uint32_t m = (uint32_t)old;
            if (m >= len) {
                /* Entire buffer is muted - discard all and update counter */
                rtl_account_fragmented_muted_u8_bytes(s, len, fs4_shift_active);
                s->mute.fetch_sub((int)len, std::memory_order_relaxed);
                if (m == len) {
                    s->mute_byte_phase.store(0, std::memory_order_relaxed);
                }
                return; /* Nothing to process */
            }
            /* Partial mute: skip first m bytes, process remainder */
            rtl_account_fragmented_muted_u8_bytes(s, m, fs4_shift_active);
            buf += m;
            len -= m;
            s->mute.fetch_sub((int)m, std::memory_order_relaxed);
            s->mute_byte_phase.store(0, std::memory_order_relaxed);
        }
    }
    rtl_submit_capture_bytes(s, buf, len);
    /* Convert incoming u8 I/Q and write directly into input ring without extra copy. */
    rtl_write_u8_to_ring(s, buf, len, fs4_shift_active, use_two_pass, combine_rotate_active, 0);
}

/**
 * @brief RTL-SDR USB thread entry: reads samples asynchronously into the input ring.
 * Applies optional realtime scheduling/affinity if configured.
 *
 * @param arg Pointer to `rtl_device`.
 * @return NULL on exit.
 */
static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    dongle_thread_fn(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    maybe_set_thread_realtime_and_affinity("DONGLE");
#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
    __try {
        rtlsdr_read_async(s->dev, rtlsdr_callback, s, 16, s->buf_len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr,
                "ERROR: libusb exception in rtlsdr_read_async (MSVC/Windows). "
                "Check that the bundled libusb/librtlsdr DLLs match the build and the device driver is installed.\n");
        exitflag = 1;
    }
#else
    rtlsdr_read_async(s->dev, rtlsdr_callback, s, 16, s->buf_len);
#endif
    DSD_THREAD_RETURN;
}

#ifdef USE_SOAPYSDR
template <typename Fn>
static int
soapy_call_locked(struct rtl_device* dev, const char* op, Fn&& fn) {
    if (!dev || !dev->soapy_dev || !dev->soapy_lock_inited) {
        return -1;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        fprintf(stderr, "SoapySDR: failed to lock mutex for %s.\n", op);
        return -1;
    }
    int ret = -1;
    try {
        ret = fn();
    } catch (const std::exception& e) {
        fprintf(stderr, "SoapySDR: exception in %s: %s\n", op, e.what());
        ret = -1;
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);
    return ret;
}
#endif

static void
soapy_stream_cleanup(struct rtl_device* dev, int unmake_device) {
#ifdef USE_SOAPYSDR
    if (!dev || !dev->soapy_lock_inited) {
        return;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        fprintf(stderr, "SoapySDR: failed to lock mutex for cleanup.\n");
        return;
    }
    if (dev->soapy_dev && dev->soapy_stream) {
        try {
            int rc = dev->soapy_dev->deactivateStream(dev->soapy_stream, 0, 0);
            if (rc < 0 && rc != SOAPY_SDR_NOT_SUPPORTED) {
                fprintf(stderr, "SoapySDR: deactivateStream failed: %s (%d).\n", SoapySDR::errToStr(rc), rc);
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "SoapySDR: exception in deactivateStream: %s\n", e.what());
        }
        try {
            dev->soapy_dev->closeStream(dev->soapy_stream);
        } catch (const std::exception& e) {
            fprintf(stderr, "SoapySDR: exception in closeStream: %s\n", e.what());
        }
        dev->soapy_stream = NULL;
    }
    if (unmake_device && dev->soapy_dev) {
        try {
            SoapySDR::Device::unmake(dev->soapy_dev);
        } catch (const std::exception& e) {
            fprintf(stderr, "SoapySDR: exception in Device::unmake: %s\n", e.what());
        }
        dev->soapy_dev = NULL;
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);
#else
    (void)dev;
    (void)unmake_device;
#endif
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    soapy_thread_fn(void* arg) {
#ifndef USE_SOAPYSDR
    (void)arg;
    DSD_THREAD_RETURN;
#else
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    if (!s || !s->soapy_dev) {
        DSD_THREAD_RETURN;
    }
    maybe_set_thread_realtime_and_affinity("DONGLE");

    int fatal = 0;
    size_t mtu_elems = 16384;
    std::string stream_format;

    if (dsd_mutex_lock(&s->soapy_lock) != 0) {
        fprintf(stderr, "SoapySDR: failed to lock mutex for stream setup.\n");
        fatal = 1;
    } else {
        try {
            std::vector<std::string> formats = s->soapy_dev->getStreamFormats(SOAPY_SDR_RX, 0);
            bool have_cf32 = false;
            bool have_cs16 = false;
            for (size_t i = 0; i < formats.size(); i++) {
                if (formats[i] == SOAPY_SDR_CF32) {
                    have_cf32 = true;
                } else if (formats[i] == SOAPY_SDR_CS16) {
                    have_cs16 = true;
                }
            }
            if (have_cf32) {
                s->soapy_format = SOAPY_FMT_CF32;
                stream_format = SOAPY_SDR_CF32;
            } else if (have_cs16) {
                s->soapy_format = SOAPY_FMT_CS16;
                stream_format = SOAPY_SDR_CS16;
            } else {
                fprintf(stderr, "SoapySDR: RX stream formats do not include CF32 or CS16.\n");
                fatal = 1;
            }
            if (!fatal) {
                std::vector<size_t> channels(1, 0);
                SoapySDR::Kwargs args;
                s->soapy_stream = s->soapy_dev->setupStream(SOAPY_SDR_RX, stream_format, channels, args);
                if (!s->soapy_stream) {
                    fprintf(stderr, "SoapySDR: setupStream returned null.\n");
                    fatal = 1;
                }
            }
            if (!fatal && s->soapy_stream) {
                size_t mtu = s->soapy_dev->getStreamMTU(s->soapy_stream);
                if (mtu > 0) {
                    mtu_elems = mtu;
                }
                s->soapy_mtu_elems = (uint32_t)mtu_elems;
            }
            if (!fatal && s->soapy_stream) {
                int rc = s->soapy_dev->activateStream(s->soapy_stream, 0, 0, 0);
                if (rc < 0) {
                    fprintf(stderr, "SoapySDR: activateStream failed: %s (%d).\n", SoapySDR::errToStr(rc), rc);
                    fatal = 1;
                }
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "SoapySDR: exception during stream setup: %s\n", e.what());
            fatal = 1;
        }
        (void)dsd_mutex_unlock(&s->soapy_lock);
    }

    if (fatal) {
        soapy_stream_cleanup(s, 1);
        s->run.store(0);
        DSD_THREAD_RETURN;
    }

    std::vector<std::complex<float>> cf32_buf;
    std::vector<int16_t> cs16_buf;
    try {
        if (s->soapy_format == SOAPY_FMT_CF32) {
            cf32_buf.resize(mtu_elems);
        } else if (s->soapy_format == SOAPY_FMT_CS16) {
            cs16_buf.resize(mtu_elems * 2);
        } else {
            fatal = 1;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "SoapySDR: buffer allocation exception: %s\n", e.what());
        fatal = 1;
    }
    if (fatal) {
        soapy_stream_cleanup(s, 1);
        s->run.store(0);
        DSD_THREAD_RETURN;
    }

    while (s->run.load() && exitflag == 0) {
        int flags = 0;
        long long time_ns = 0;
        void* buffs[1] = {NULL};
        if (s->soapy_format == SOAPY_FMT_CF32) {
            buffs[0] = (void*)cf32_buf.data();
        } else if (s->soapy_format == SOAPY_FMT_CS16) {
            buffs[0] = (void*)cs16_buf.data();
        } else {
            fatal = 1;
            break;
        }

        int ret = -1;
        int read_exception = 0;
        if (dsd_mutex_lock(&s->soapy_lock) != 0) {
            fprintf(stderr, "SoapySDR: failed to lock mutex for readStream.\n");
            fatal = 1;
            break;
        }
        try {
            ret = s->soapy_dev->readStream(s->soapy_stream, buffs, mtu_elems, flags, time_ns, 15000);
        } catch (const std::exception& e) {
            fprintf(stderr, "SoapySDR: exception in readStream: %s\n", e.what());
            read_exception = 1;
        }
        (void)dsd_mutex_unlock(&s->soapy_lock);

        if (read_exception) {
            fatal = 1;
            break;
        }
        if (ret == SOAPY_SDR_TIMEOUT) {
            s->soapy_timeout_count++;
            continue;
        }
        if (ret == SOAPY_SDR_OVERFLOW) {
            s->soapy_overflow_count++;
            uint64_t now_ns = dsd_time_monotonic_ns();
            if ((now_ns - s->soapy_last_overflow_log_ns) > 1000000000ULL) {
                fprintf(stderr, "SoapySDR: RX overflow count=%llu.\n", (unsigned long long)s->soapy_overflow_count);
                s->soapy_last_overflow_log_ns = now_ns;
            }
            continue;
        }
        if (ret < 0) {
            s->soapy_read_errors++;
            fprintf(stderr, "SoapySDR: readStream failed: %s (%d).\n", SoapySDR::errToStr(ret), ret);
            fatal = 1;
            break;
        }
        if (ret == 0) {
            continue;
        }

        const int apply_rot = fs4_shift_capture_active(s);
        if (s->soapy_format == SOAPY_FMT_CF32) {
            rtl_submit_capture_bytes(s, cf32_buf.data(), (size_t)ret * sizeof(std::complex<float>));
            (void)soapy_write_cf32_to_ring(s, cf32_buf.data(), (size_t)ret, apply_rot);
        } else {
            (void)soapy_write_cs16_to_ring(s, cs16_buf.data(), (size_t)ret, apply_rot);
        }
    }

    soapy_stream_cleanup(s, fatal ? 1 : 0);
    s->run.store(0);
    DSD_THREAD_RETURN;
#endif
}

/* ---- rtl_tcp backend helpers ---- */

/* Connect to rtl_tcp server */
static dsd_socket_t
tcp_connect_host(const char* host, int port) {
    dsd_socket_t sockfd = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sockfd == DSD_INVALID_SOCKET) {
        fprintf(stderr, "rtl_tcp: ERROR opening socket\n");
        return DSD_INVALID_SOCKET;
    }
    /* Best-effort: enable TCP keepalive to detect half-open links */
    {
        int opt = 1;
        (void)dsd_socket_setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#if defined(TCP_KEEPIDLE)
        int idle = 15; /* seconds before starting keepalive probes */
        (void)dsd_socket_setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#if defined(TCP_KEEPCNT)
        int cnt = 4; /* number of probes */
        (void)dsd_socket_setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
#if defined(TCP_KEEPINTVL)
        int intvl = 5; /* seconds between probes */
        (void)dsd_socket_setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#if defined(TCP_USER_TIMEOUT)
        /* Optional fail-fast if ACKs are not received within timeout (ms) */
        int uto = 20000; /* 20s */
        (void)dsd_socket_setsockopt(sockfd, IPPROTO_TCP, TCP_USER_TIMEOUT, &uto, sizeof(uto));
#endif
    }
    struct sockaddr_in serveraddr;
    if (dsd_socket_resolve(host, port, &serveraddr) != 0) {
        fprintf(stderr, "rtl_tcp: ERROR, no such host as %s\n", host);
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }
    if (dsd_socket_connect(sockfd, reinterpret_cast<const struct sockaddr*>(&serveraddr), sizeof(serveraddr)) != 0) {
        fprintf(stderr, "rtl_tcp: ERROR connecting to %s:%d\n", host, port);
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }
    return sockfd;
}

/* Send rtl_tcp command: 1 byte id + 4 byte big-endian value */
static int
rtl_tcp_send_cmd(dsd_socket_t sockfd, uint8_t cmd, uint32_t param) {
    uint8_t buf[5];
    buf[0] = cmd;
    buf[1] = (uint8_t)((param >> 24) & 0xFF);
    buf[2] = (uint8_t)((param >> 16) & 0xFF);
    buf[3] = (uint8_t)((param >> 8) & 0xFF);
    buf[4] = (uint8_t)(param & 0xFF);
    int n = dsd_socket_send(sockfd, buf, 5, MSG_NOSIGNAL);
    return (n == 5) ? 0 : -1;
}

static int
env_agc_want(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return 1;
    }
    return cfg->rtl_agc_enable ? 1 : 0;
}

/* Read and discard rtl_tcp header: 'RTL0' + tuner(4) + ngains(4) + ngains*4 */
static void
rtl_tcp_skip_header(dsd_socket_t sockfd) {
    uint8_t hdr[12];
    int n = dsd_socket_recv(sockfd, hdr, sizeof(hdr), MSG_WAITALL);
    if (n != (int)sizeof(hdr)) {
        return;
    }
    if (!(hdr[0] == 'R' && hdr[1] == 'T' && hdr[2] == 'L' && hdr[3] == '0')) {
        return;
    }
    /* Parse ngains (last 4 bytes) as big-endian per rtl_tcp */
    uint32_t ngains =
        ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) | ((uint32_t)hdr[10] << 8) | (uint32_t)hdr[11];
    if (ngains > 0 && ngains < 4096) {
        size_t to_discard = (size_t)ngains * 4U;
        uint8_t buf[1024];
        while (to_discard > 0) {
            size_t chunk = to_discard > sizeof(buf) ? sizeof(buf) : to_discard;
            int r = dsd_socket_recv(sockfd, buf, chunk, MSG_WAITALL);
            if (r <= 0) {
                break;
            }
            to_discard -= (size_t)r;
        }
    }
}

/* TCP reader thread: read u8 IQ, widen to float, push to ring */
static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    tcp_thread_fn(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    maybe_set_thread_realtime_and_affinity("DONGLE");
    /* Default read size: for rtl_tcp prefer small (16 KiB) chunks for higher cadence.
       For USB, derive ~20 ms to reduce burstiness. */
    size_t BUFSZ = 0;
    if (s->backend == RTL_BACKEND_TCP) {
        BUFSZ = 16384; /* ~5 ms @ 1.536 Msps */
    } else {
        if (s->rate > 0) {
            double bytes_per_sec = (double)s->rate * 2.0;   /* 2 bytes per complex sample (u8 I, u8 Q) */
            size_t target = (size_t)(bytes_per_sec * 0.02); /* ~20 ms */
            if (target < 16384) {
                target = 16384; /* lower bound */
            }
            if (target > 262144) {
                target = 262144; /* upper bound */
            }
            BUFSZ = target;
        } else {
            BUFSZ = 65536; /* safe fallback when rate isn't known yet */
        }
    }
    if (cfg && cfg->tcp_bufsz_is_set && cfg->tcp_bufsz_bytes > 0) {
        BUFSZ = (size_t)cfg->tcp_bufsz_bytes;
    }
    unsigned char* u8 = (unsigned char*)malloc(BUFSZ);
    if (!u8) {
        s->run.store(0);
        return 0;
    }
    /* Discard server capability header so following bytes are pure IQ */
    rtl_tcp_skip_header(s->sockfd);
    int waitall = (s->backend == RTL_BACKEND_TCP) ? 0 : 1; /* rtl_tcp default off; USB default on */
    if (cfg && cfg->tcp_waitall_is_set) {
        waitall = cfg->tcp_waitall_enable ? 1 : 0;
    }
    /* Autotune can override BUFSZ/WAITALL adaptively. Initial state considers env, but
       each loop consults s->tcp_autotune so UI/runtime toggles take effect live. */
    if (!s->tcp_autotune) {
        if (cfg && cfg->tcp_autotune_enable) {
            s->tcp_autotune = 1; /* make it observable to loop */
        }
    }

    /* Track deltas for adaptive decisions */
    uint64_t prev_drops = s->input_ring ? s->input_ring->producer_drops.load() : 0ULL;
    uint64_t prev_rdto = s->input_ring ? s->input_ring->read_timeouts.load() : 0ULL;
    uint64_t prev_res_full = s->reserve_full_events;
    uint64_t auto_last_ns = dsd_time_monotonic_ns();
    /* Less aggressive reconnect: allow a few consecutive timeouts before
       declaring the connection lost. Default 3; override via
       DSD_NEO_TCP_MAX_TIMEOUTS. */
    int timeout_limit = cfg ? cfg->tcp_max_timeouts : 3;
    int consec_timeouts = 0;
    while (s->run.load() && exitflag == 0) {
        /* Light backpressure: if ring is nearly full, yield briefly */
        int autotune = s->tcp_autotune;
        if (autotune && s->input_ring) {
            const size_t SLICE = (s->buf_len > 0 ? (size_t)s->buf_len : 16384);
            size_t free_sp = input_ring_free(s->input_ring);
            if (free_sp < (SLICE * 2)) {
                dsd_sleep_us(500); /* 0.5 ms */
            }
        }
        int r = dsd_socket_recv(s->sockfd, u8, BUFSZ, waitall ? MSG_WAITALL : 0);
        if (r <= 0) {
            /* Timeout or connection closed. On timeout, tolerate up to
               timeout_limit consecutive occurrences before reconnecting. */
            if (!s->run.load() || exitflag) {
                break;
            }
            int e = dsd_socket_get_error();
#if DSD_PLATFORM_WIN_NATIVE
            int is_timeout = (r < 0) && (e == WSAEWOULDBLOCK || e == WSAETIMEDOUT || e == WSAEINTR);
#else
            int is_timeout = (r < 0) && (e == EAGAIN || e == EWOULDBLOCK || e == EINTR);
#endif
            if (is_timeout) {
                consec_timeouts++;
                if (consec_timeouts < timeout_limit) {
                    /* Try again without reconnecting */
                    continue;
                }
            }
            /* Either closed, hard error, or too many consecutive timeouts. */
            fprintf(stderr, "rtl_tcp: input stalled; attempting to reconnect to %s:%d...\n", s->host, s->port);
            /* Preserve current device state to replay after reconnect */
            const uint32_t prev_freq = s->freq;
            const uint32_t prev_rate = s->rate;
            const int prev_gain = s->gain;
            const int prev_agc = s->agc_mode;
            const int prev_ppm = s->ppm_error;
            const int prev_direct = s->direct_sampling;
            const int prev_bias = s->bias_tee_on;
            const int prev_offset = s->offset_tuning;
            const int prev_testmode = s->testmode_on;
            const uint32_t prev_rtl_xtal = s->rtl_xtal_hz;
            const uint32_t prev_tuner_xtal = s->tuner_xtal_hz;

            if (s->sockfd != DSD_INVALID_SOCKET) {
                dsd_socket_shutdown(s->sockfd, SHUT_RDWR);
                dsd_socket_close(s->sockfd);
                s->sockfd = DSD_INVALID_SOCKET;
            }
            /* Backoff loop */
            int attempt = 0;
            while (s->run.load() && exitflag == 0) {
                attempt++;
                dsd_socket_t newsfd = tcp_connect_host(s->host, s->port);
                if (newsfd != DSD_INVALID_SOCKET) {
                    s->sockfd = newsfd;
                    fprintf(stderr, "rtl_tcp: reconnected on attempt %d.\n", attempt);
                    /* Reinitialize stream framing. A reconnect starts a fresh IQ
                     * stream after a new RTL0 header, so buffered bytes and
                     * capture-side phase/carry state from the old socket must
                     * not carry into the resumed stream. */
                    rtl_tcp_skip_header(s->sockfd);
                    rtl_reset_capture_state_on_stream_boundary(s);
                    /* Reapply socket options: RCVBUF/NODELAY/RCVTIMEO */
                    {
                        const dsdneoRuntimeConfig* cfg2 = dsd_neo_get_config();
                        int rcvbuf = cfg2 ? cfg2->tcp_rcvbuf_bytes : (4 * 1024 * 1024);
                        (void)dsd_socket_setsockopt(s->sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
                        int nodelay = 1;
                        (void)dsd_socket_setsockopt(s->sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                        int to_ms = cfg2 ? cfg2->tcp_rcvtimeo_ms : 2000;
                        (void)dsd_socket_set_recv_timeout(s->sockfd, (unsigned int)to_ms);
                    }
                    /* Replay essential device state to server */
                    if (prev_freq > 0) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x01, prev_freq);
                    }
                    if (prev_rate > 0) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x02, prev_rate);
                    }
                    if (prev_agc) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x03, 0); /* tuner auto */
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x08, (uint32_t)env_agc_want());
                    } else {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x03, 1);
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x04, (uint32_t)prev_gain);
                    }
                    if (prev_ppm != 0) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x05, (uint32_t)prev_ppm);
                    }
                    if (prev_direct) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x09, (uint32_t)prev_direct);
                    }
                    if (prev_offset) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x0A, 1);
                    }
                    if (prev_bias) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x0E, 1);
                    }
                    if (prev_testmode) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x07, (uint32_t)prev_testmode);
                    }
                    if (prev_rtl_xtal > 0) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x0B, prev_rtl_xtal);
                    }
                    if (prev_tuner_xtal > 0) {
                        (void)rtl_tcp_send_cmd(s->sockfd, 0x0C, prev_tuner_xtal);
                    }
                    if (s->if_gain_count > 0) {
                        for (int i = 0; i < s->if_gain_count && i < 16; i++) {
                            uint32_t packed = ((uint32_t)(s->if_gains[i].stage & 0xFFFF) << 16)
                                              | ((uint16_t)(s->if_gains[i].gain & 0xFFFF));
                            (void)rtl_tcp_send_cmd(s->sockfd, 0x06, packed);
                        }
                    }
                    /* Resume recv loop */
                    r = dsd_socket_recv(s->sockfd, u8, BUFSZ, waitall ? MSG_WAITALL : 0);
                    if (r > 0) {
                        /* Continue with normal processing */
                        break;
                    }
                    /* Immediate failure: close and retry */
                    dsd_socket_shutdown(s->sockfd, SHUT_RDWR);
                    dsd_socket_close(s->sockfd);
                    s->sockfd = DSD_INVALID_SOCKET;
                }
                int backoff_ms = 200 * (attempt < 10 ? attempt : 10); /* up to ~2s */
                dsd_sleep_ms((unsigned int)backoff_ms);
            }
            if (s->sockfd == DSD_INVALID_SOCKET || r <= 0) {
                /* Could not reconnect or no data after reconnect; exit */
                break;
            }
        }
        /* Successful read: reset timeout counter */
        consec_timeouts = 0;
        uint32_t len = (uint32_t)r;
        int fs4_shift_active = 0;
        int combine_rotate_active = 0;
        int use_two_pass = 0;
        rtl_get_u8_transform_policy(s, &fs4_shift_active, &combine_rotate_active, &use_two_pass);
        /* Handle muting: discard muted samples for rtl_tcp backend.
         * Same logic as USB callback - skip samples entirely instead of processing.
         *
         * The remaining mute count tracks raw stream bytes, so it may be odd here
         * after an odd-length recv() that was fully discarded. That is still aligned
         * in the continuous transport stream and must not be rounded independently. */
        if (s->mute.load(std::memory_order_relaxed) > 0) {
            rtl_prepare_fragmented_u8_mute(s);
            int old = s->mute.load(std::memory_order_relaxed);
            if (old > 0) {
                uint32_t m = (uint32_t)old;
                if (m >= len) {
                    /* Entire buffer is muted - discard all */
                    rtl_account_fragmented_muted_u8_bytes(s, len, fs4_shift_active);
                    s->mute.fetch_sub((int)len, std::memory_order_relaxed);
                    if (m == len) {
                        s->mute_byte_phase.store(0, std::memory_order_relaxed);
                    }
                    continue; /* Skip processing, get next recv */
                }
                /* Partial mute: skip first m bytes, process remainder */
                rtl_account_fragmented_muted_u8_bytes(s, m, fs4_shift_active);
                memmove(u8, u8 + m, len - m);
                len -= m;
                s->mute.fetch_sub((int)m, std::memory_order_relaxed);
                s->mute_byte_phase.store(0, std::memory_order_relaxed);
            }
        }
        /* Stats: bytes in */
        if (s->stats_enabled) {
            s->tcp_bytes_total += (uint64_t)len;
            s->tcp_bytes_window += (uint64_t)len;
        }
        rtl_submit_capture_bytes(s, u8, len);
        /* Reassemble into uniform slices matching device buf_len to stabilize cadence */
        const size_t SLICE = (s->buf_len > 0 ? (size_t)s->buf_len : 16384);

        /* Fill pending if it exists to complete one slice */
        size_t consumed = 0;
        if (s->tcp_pending_len > 0) {
            size_t missing = (SLICE > s->tcp_pending_len) ? (SLICE - s->tcp_pending_len) : 0;
            size_t take = (missing < len) ? missing : len;
            if (take > 0) {
                if (!s->tcp_pending || s->tcp_pending_cap < SLICE) {
                    size_t cap = (SLICE + 4095) & ~((size_t)4095);
                    unsigned char* nb = (unsigned char*)realloc(s->tcp_pending, cap);
                    if (nb) {
                        s->tcp_pending = nb;
                        s->tcp_pending_cap = cap;
                    }
                }
                if (s->tcp_pending && s->tcp_pending_cap >= (s->tcp_pending_len + take)) {
                    memcpy(s->tcp_pending + s->tcp_pending_len, u8, take);
                    s->tcp_pending_len += take;
                    consumed += take;
                }
            }
            if (s->tcp_pending_len == SLICE) {
                unsigned char* src = s->tcp_pending;
                rtl_write_u8_to_ring(s, src, SLICE, fs4_shift_active, use_two_pass, combine_rotate_active, 1);
                s->tcp_pending_len = 0;
            }
        }

        /* Process full slices directly from current buffer */
        while ((len - consumed) >= SLICE) {
            unsigned char* src = u8 + consumed;
            int ring_exhausted =
                rtl_write_u8_to_ring(s, src, SLICE, fs4_shift_active, use_two_pass, combine_rotate_active, 1);
            consumed += SLICE;
            if (ring_exhausted) {
                break;
            }
        }

        /* Save remainder (<SLICE) into pending */
        size_t rem = len - consumed;
        if (rem > 0) {
            if (!s->tcp_pending || s->tcp_pending_cap < rem) {
                size_t cap = (rem + 4095) & ~((size_t)4095);
                unsigned char* nb = (unsigned char*)realloc(s->tcp_pending, cap);
                if (nb) {
                    s->tcp_pending = nb;
                    s->tcp_pending_cap = cap;
                } else {
                    /* Retain one tail byte if needed so a dropped reassembly tail does not break I/Q alignment. */
                    (void)rtl_drop_u8_bytes_preserve_alignment(u8 + consumed, rem, &s->iq_byte_carry, &s->rot_phase,
                                                               fs4_shift_active);
                    rem = 0;
                }
            }
            if (rem > 0) {
                memcpy(s->tcp_pending, u8 + consumed, rem);
                s->tcp_pending_len = rem;
            }
        }

        /* Once per ~1s: optional stats print and adaptive tuning */
        {
            uint64_t now_ns = dsd_time_monotonic_ns();
            uint64_t stats_dt_ns = now_ns - s->stats_last_ns;
            if (s->stats_enabled && stats_dt_ns >= 1000000000ULL) {
                double dt = (double)stats_dt_ns / 1e9;
                double mbps = (double)s->tcp_bytes_window / dt / (1024.0 * 1024.0);
                double exp_bps = (s->rate > 0) ? (double)(s->rate * 2ULL) : 0.0;
                double exp_mbps = exp_bps / (1024.0 * 1024.0);
                uint64_t drops = s->input_ring ? s->input_ring->producer_drops.load() : 0ULL;
                uint64_t rdto = s->input_ring ? s->input_ring->read_timeouts.load() : 0ULL;
                fprintf(stderr, "rtl_tcp: %.2f MiB/s (exp %.2f), drops=%llu, res_full=%llu, read_timeouts=%llu\n", mbps,
                        exp_mbps, (unsigned long long)drops, (unsigned long long)s->reserve_full_events,
                        (unsigned long long)rdto);
                s->tcp_bytes_window = 0ULL;
                s->stats_last_ns = now_ns;
            }
            /* Adaptive block ~1s cadence */
            uint64_t auto_dt_ns = now_ns - auto_last_ns;
            autotune = s->tcp_autotune;
            if (autotune && auto_dt_ns >= 1000000000ULL) {
                uint64_t drops = s->input_ring ? s->input_ring->producer_drops.load() : 0ULL;
                uint64_t rdto = s->input_ring ? s->input_ring->read_timeouts.load() : 0ULL;
                uint64_t resf = s->reserve_full_events;
                uint64_t d_drops = (drops >= prev_drops) ? (drops - prev_drops) : 0ULL;
                uint64_t d_rdto = (rdto >= prev_rdto) ? (rdto - prev_rdto) : 0ULL;
                uint64_t d_resf = (resf >= prev_res_full) ? (resf - prev_res_full) : 0ULL;
                prev_drops = drops;
                prev_rdto = rdto;
                prev_res_full = resf;
                /* If we're overflowing frequently, shrink BUFSZ and ensure WAITALL=0 */
                if (d_drops > 0 || d_resf > 0) {
                    if (BUFSZ > 16384) {
                        BUFSZ = BUFSZ / 2;
                        if (BUFSZ < 16384) {
                            BUFSZ = 16384;
                        }
                        unsigned char* nb = (unsigned char*)realloc(u8, BUFSZ);
                        if (nb) {
                            u8 = nb;
                        }
                    }
                    waitall = 0;
                } else if (d_rdto > 5) {
                    /* Consumer is starved: shrink BUFSZ to deliver smaller, faster packets */
                    if (BUFSZ > 8192) {
                        BUFSZ = BUFSZ / 2;
                        if (BUFSZ < 8192) {
                            BUFSZ = 8192;
                        }
                        unsigned char* nb = (unsigned char*)realloc(u8, BUFSZ);
                        if (nb) {
                            u8 = nb;
                        }
                    }
                    waitall = 0;
                } else {
                    /* Quiet period: slowly grow BUFSZ up to 64 KiB for efficiency */
                    if (BUFSZ < 65536) {
                        size_t nsz = BUFSZ + (BUFSZ / 2);
                        if (nsz > 65536) {
                            nsz = 65536;
                        }
                        unsigned char* nb = (unsigned char*)realloc(u8, nsz);
                        if (nb) {
                            u8 = nb;
                            BUFSZ = nsz;
                        }
                    }
                }
                auto_last_ns = now_ns;
            }
        }
    }
    free(u8);
    s->run.store(0);
    DSD_THREAD_RETURN;
}

/**
 * @brief Find the nearest supported gain to the target gain.
 *
 * @param dev RTL-SDR device handle.
 * @param target_gain Target gain in tenths of dB.
 * @return Nearest supported gain in tenths of dB, or negative error code.
 */
static int
nearest_gain(rtlsdr_dev_t* dev, int target_gain) {
    int i, r, err1, err2, count, nearest;
    int* gains;
    r = rtlsdr_set_tuner_gain_mode(dev, 1);
    if (r < 0) {
        fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
        return r;
    }
    count = rtlsdr_get_tuner_gains(dev, NULL);
    if (count <= 0) {
        return 0;
    }
    gains = static_cast<int*>(malloc(sizeof(int) * count));
    count = rtlsdr_get_tuner_gains(dev, gains);
    nearest = gains[0];
    for (i = 0; i < count; i++) {
        err1 = abs(target_gain - nearest);
        err2 = abs(target_gain - gains[i]);
        if (err2 < err1) {
            nearest = gains[i];
        }
    }
    free(gains);
    return nearest;
}

/**
 * @brief Set RTL-SDR center frequency with a brief status message.
 *
 * @param dev RTL-SDR device handle.
 * @param frequency Center frequency in Hz.
 * @return 0 on success or a negative error code.
 */
static int
verbose_set_frequency(rtlsdr_dev_t* dev, uint32_t frequency) {
    int r;
    r = rtlsdr_set_center_freq(dev, frequency);
    if (r < 0) {
        fprintf(stderr, " (WARNING: Failed to set Center Frequency). \n");
    } else {
        fprintf(stderr, " (Center Frequency: %u Hz.) \n", frequency);
    }
    return r;
}

/**
 * @brief Set RTL-SDR sampling rate with a brief status message.
 *
 * @param dev RTL-SDR device handle.
 * @param samp_rate Sampling rate in Hz.
 * @return 0 on success or a negative error code.
 */
static int
verbose_set_sample_rate(rtlsdr_dev_t* dev, uint32_t samp_rate) {
    int r;
    r = rtlsdr_set_sample_rate(dev, samp_rate);
    if (r < 0) {
        fprintf(stderr, "WARNING: Failed to set sample rate.\n");
    } else {
        fprintf(stderr, "Sampling at %u S/s.\n", samp_rate);
    }
    return r;
}

/**
 * @brief Enable or disable direct sampling mode.
 *
 * @param dev RTL-SDR device handle.
 * @param on Non-zero to enable, zero to disable.
 * @return 0 on success or a negative error code.
 */
static int
verbose_direct_sampling(rtlsdr_dev_t* dev, int on) {
    int r;
    r = rtlsdr_set_direct_sampling(dev, on);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to set direct sampling mode.\n");
        return r;
    }
    if (on == 0) {
        fprintf(stderr, "Direct sampling mode disabled.\n");
    }
    if (on == 1) {
        fprintf(stderr, "Enabled direct sampling mode, input 1/I.\n");
    }
    if (on == 2) {
        fprintf(stderr, "Enabled direct sampling mode, input 2/Q.\n");
    }
    return r;
}

/**
 * @brief Print tuner type and expected hardware offset tuning support for this librtlsdr.
 *
 * Note: This is a heuristic based on tuner type. Upstream librtlsdr returns -2 for
 * R820T/R828D when enabling offset tuning. Forks may differ.
 */
void
rtl_device_print_offset_capability(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        fprintf(stderr,
                "rtl_tcp: offset tuning capability is determined by the server; defaulting to disabled to match USB "
                "fs/4/rotate path (override with DSD_NEO_RTL_OFFSET_TUNING=1).\n");
        return;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        fprintf(stderr, "IQ replay: offset tuning capability query is not applicable.\n");
        return;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        fprintf(stderr, "SoapySDR: offset tuning not implemented in this backend; using fs/4 shift fallback path.\n");
        return;
    }
    if (!dev->dev) {
        return;
    }
    int t = rtlsdr_get_tuner_type(dev->dev);
    const char* tt = "unknown";
    switch (t) {
        case RTLSDR_TUNER_E4000: tt = "E4000"; break;
        case RTLSDR_TUNER_FC0012: tt = "FC0012"; break;
        case RTLSDR_TUNER_FC0013: tt = "FC0013"; break;
        case RTLSDR_TUNER_FC2580: tt = "FC2580"; break;
        case RTLSDR_TUNER_R820T: tt = "R820T"; break;
        case RTLSDR_TUNER_R828D: tt = "R828D"; break;
        default: break;
    }
    int supported = 1;
    if (t == RTLSDR_TUNER_R820T || t == RTLSDR_TUNER_R828D) {
        supported = 0; /* per upstream librtlsdr */
    }
    fprintf(stderr, "RTL tuner: %s; hardware offset tuning supported by this librtlsdr: %s\n", tt,
            supported ? "yes (expected)" : "no (expected upstream)");
}

/**
 * @brief Set tuner IF bandwidth (if supported by the library/driver).
 */
static int
verbose_set_tuner_bandwidth(rtlsdr_dev_t* dev, uint32_t bw_hz) {
    /* Pass-through to librtlsdr; bw_hz == 0 lets driver choose an appropriate filter */
    int r = rtlsdr_set_tuner_bandwidth(dev, (int)bw_hz);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to set tuner bandwidth to %u Hz.\n", bw_hz);
    } else {
        if (bw_hz == 0) {
            fprintf(stderr, "Tuner bandwidth set to auto (driver).\n");
        } else {
            fprintf(stderr, "Tuner bandwidth set to %u Hz.\n", bw_hz);
        }
    }
    return r;
}

/**
 * @brief Enable tuner automatic gain control.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success or a negative error code.
 */
static int
verbose_auto_gain(rtlsdr_dev_t* dev) {
    int r;
    r = rtlsdr_set_tuner_gain_mode(dev, 0);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
    } else {
        fprintf(stderr, "Tuner gain set to automatic.\n");
    }
    /* Original plan: enable RTL digital AGC in auto mode by default.
       Allow override via env DSD_NEO_RTL_AGC=0 to disable. */
    int want = env_agc_want();
    int ra = rtlsdr_set_agc_mode(dev, want);
    if (ra != 0) {
        fprintf(stderr, "WARNING: Failed to %s RTL AGC.\n", want ? "enable" : "disable");
    } else {
        fprintf(stderr, "RTL AGC %s.\n", want ? "enabled" : "disabled");
    }
    return r;
}

/**
 * @brief Set a fixed tuner gain with a message indicating the result.
 *
 * @param dev RTL-SDR device handle.
 * @param gain Desired gain in tenths of dB.
 * @return 0 on success or a negative error code.
 */
static int
verbose_gain_set(rtlsdr_dev_t* dev, int gain) {
    int r;
    /* Disable RTL digital AGC when setting manual tuner gain */
    (void)rtlsdr_set_agc_mode(dev, 0);
    r = rtlsdr_set_tuner_gain_mode(dev, 1);
    if (r < 0) {
        fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
        return r;
    }
    r = rtlsdr_set_tuner_gain(dev, gain);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
    } else {
        fprintf(stderr, "Tuner gain set to %0.2f dB.\n", gain / 10.0);
    }
    return r;
}

/**
 * @brief Set tuner PPM frequency error correction.
 *
 * @param dev RTL-SDR device handle.
 * @param ppm_error Error in parts-per-million.
 * @return 0 on success or a negative error code.
 */
static int
verbose_ppm_set(rtlsdr_dev_t* dev, int ppm_error) {
    int r;
    r = rtlsdr_set_freq_correction(dev, ppm_error);
    if (r < 0) {
        fprintf(stderr, "WARNING: Failed to set ppm error.\n");
    } else {
        fprintf(stderr, "Tuner error set to %i ppm.\n", ppm_error);
    }
    return r;
}

/**
 * @brief Reset RTL-SDR USB buffers.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success or a negative error code.
 */
static int
verbose_reset_buffer(rtlsdr_dev_t* dev) {
    int r;
    r = rtlsdr_reset_buffer(dev);
    if (r < 0) {
        fprintf(stderr, "WARNING: Failed to reset buffers.\n");
    }
    return r;
}

// Public API Implementation

static void
rtl_device_init_common_state(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    dev->iq_capture_writer = NULL;
    dev->capture_retune_count.store(0, std::memory_order_relaxed);
    dev->replay_fs4_shift_enabled = 0;
    dev->replay_combine_rotate_enabled = 0;
    memset(&dev->replay_cfg, 0, sizeof(dev->replay_cfg));
    dev->replay_src = NULL;
    memset(&dev->replay_eof, 0, sizeof(dev->replay_eof));
    dev->replay_has_eof_state = 0;
    dev->replay_float_elements_written = 0;
}

/**
 * @brief Create and initialize an RTL-SDR device.
 *
 * @param dev_index Device index to open.
 * @param input_ring Pointer to input ring for USB data.
 * @param combine_rotate_enabled_param Whether to use combined rotate+widen when offset tuning is disabled.
 * @return Pointer to rtl_device handle, or NULL on failure.
 */
struct rtl_device*
rtl_device_create(int dev_index, struct input_ring_state* input_ring, int combine_rotate_enabled_param) {
    if (!input_ring) {
        return NULL;
    }

    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    rtl_device_init_common_state(dev);

    dev->dev_index = dev_index;
    dev->input_ring = input_ring;
    dev->thread_started = 0;
    dev->mute = 0;
    dev->mute_byte_phase = 0;
    dev->combine_rotate_enabled = combine_rotate_enabled_param;
    dev->backend = RTL_BACKEND_USB;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_lock_inited = 0;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->sockfd = DSD_INVALID_SOCKET;
    dev->host[0] = '\0';
    dev->port = 0;
    dev->run.store(0);
    dev->agc_mode = 1;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;

    int r = 0;
#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
    __try {
        r = rtlsdr_open(&dev->dev, (uint32_t)dev_index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fprintf(stderr,
                "ERROR: libusb exception in rtlsdr_open (MSVC/Windows). "
                "Check that the bundled libusb/librtlsdr DLLs match the build and the device driver is installed.\n");
        r = -1;
    }
#else
    r = rtlsdr_open(&dev->dev, (uint32_t)dev_index);
#endif
    if (r < 0) {
        fprintf(stderr, "Failed to open rtlsdr device %d.\n", dev_index);
        free(dev);
        return NULL;
    }

    return dev;
}

struct rtl_device*
rtl_device_create_tcp(const char* host, int port, struct input_ring_state* input_ring, int combine_rotate_enabled_param,
                      int autotune_enabled) {
    if (!input_ring || !host || port <= 0) {
        return NULL;
    }
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    rtl_device_init_common_state(dev);
    dev->dev = NULL;
    dev->dev_index = -1;
    dev->input_ring = input_ring;
    dev->thread_started = 0;
    dev->mute = 0;
    dev->mute_byte_phase = 0;
    dev->combine_rotate_enabled = combine_rotate_enabled_param;
    dev->backend = RTL_BACKEND_TCP;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_lock_inited = 0;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->sockfd = DSD_INVALID_SOCKET;
    snprintf(dev->host, sizeof(dev->host), "%s", host);
    dev->port = port;
    dev->run.store(0);
    dev->agc_mode = 1;
    dev->tcp_autotune = autotune_enabled ? 1 : 0;
    dev->offset_tuning = 0;
    dev->tcp_pending = NULL;
    dev->tcp_pending_len = 0;
    dev->tcp_pending_cap = 0;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;

    dsd_socket_t sfd = tcp_connect_host(host, port);
    if (sfd == DSD_INVALID_SOCKET) {
        free(dev);
        return NULL;
    }
    /* Increase socket receive buffer to tolerate brief processing stalls */
    {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        int rcvbuf = cfg ? cfg->tcp_rcvbuf_bytes : (4 * 1024 * 1024);
        (void)dsd_socket_setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        int nodelay = 1;
        (void)dsd_socket_setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        /* Hard fix: apply a receive timeout so stalled connections don't appear
           as a P25 wedge. Default 2 seconds; override via DSD_NEO_TCP_RCVTIMEO (ms). */
        int to_ms = cfg ? cfg->tcp_rcvtimeo_ms : 2000;
        (void)dsd_socket_set_recv_timeout(sfd, (unsigned int)to_ms);
    }
    dev->sockfd = sfd;
    fprintf(stderr, "rtl_tcp: Connected to %s:%d\n", host, port);
    /* Optional TCP stats: enable with DSD_NEO_TCP_STATS=1 */
    {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->tcp_stats_enable) {
            dev->stats_enabled = 1;
            dev->stats_last_ns = dsd_time_monotonic_ns();
            fprintf(stderr, "rtl_tcp: stats enabled.\n");
        }
    }
    /* Initialize autotune from env if not already enabled by caller */
    if (!dev->tcp_autotune) {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->tcp_autotune_enable) {
            dev->tcp_autotune = 1;
        }
    }
    return dev;
}

struct rtl_device*
rtl_device_create_soapy(const char* soapy_args, struct input_ring_state* input_ring, int combine_rotate_enabled_param) {
    if (!input_ring) {
        return NULL;
    }
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    rtl_device_init_common_state(dev);
    dev->dev = NULL;
    dev->dev_index = -1;
    dev->input_ring = input_ring;
    dev->thread_started = 0;
    dev->mute = 0;
    dev->mute_byte_phase = 0;
    dev->combine_rotate_enabled = combine_rotate_enabled_param;
    dev->backend = RTL_BACKEND_SOAPY;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->sockfd = DSD_INVALID_SOCKET;
    dev->host[0] = '\0';
    dev->port = 0;
    dev->run.store(0);
    dev->agc_mode = 1;
    dev->offset_tuning = 0;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;

#ifndef USE_SOAPYSDR
    (void)soapy_args;
    fprintf(stderr, "SoapySDR backend unavailable in this build.\n");
    free(dev);
    return NULL;
#else
    if (dsd_mutex_init(&dev->soapy_lock) != 0) {
        fprintf(stderr, "SoapySDR: failed to initialize mutex.\n");
        free(dev);
        return NULL;
    }
    dev->soapy_lock_inited = 1;

    const char* args_cstr = soapy_args ? soapy_args : "";
    std::string args_string;
    try {
        args_string = args_cstr;
        (void)SoapySDR::KwargsFromString(args_string);
    } catch (const std::exception& e) {
        fprintf(stderr, "SoapySDR: invalid args string '%s': %s\n", args_cstr, e.what());
        (void)dsd_mutex_destroy(&dev->soapy_lock);
        dev->soapy_lock_inited = 0;
        free(dev);
        return NULL;
    }

    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        fprintf(stderr, "SoapySDR: failed to lock mutex during creation.\n");
        (void)dsd_mutex_destroy(&dev->soapy_lock);
        dev->soapy_lock_inited = 0;
        free(dev);
        return NULL;
    }
    try {
        SoapySDR::KwargsList found = SoapySDR::Device::enumerate(args_string);
        if (found.empty()) {
            fprintf(stderr, "SoapySDR: enumerate found no devices for args '%s'.\n", args_cstr);
        }
        dev->soapy_dev = SoapySDR::Device::make(args_string);
    } catch (const std::exception& e) {
        fprintf(stderr, "SoapySDR: exception in Device::make: %s\n", e.what());
        dev->soapy_dev = NULL;
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);

    if (!dev->soapy_dev) {
        fprintf(stderr, "SoapySDR: failed to create device for args '%s'.\n", args_cstr);
        (void)dsd_mutex_destroy(&dev->soapy_lock);
        dev->soapy_lock_inited = 0;
        free(dev);
        return NULL;
    }

    if (dsd_mutex_lock(&dev->soapy_lock) == 0) {
        try {
            std::vector<std::string> formats = dev->soapy_dev->getStreamFormats(SOAPY_SDR_RX, 0);
            bool have_cf32 = false;
            bool have_cs16 = false;
            for (size_t i = 0; i < formats.size(); i++) {
                if (formats[i] == SOAPY_SDR_CF32) {
                    have_cf32 = true;
                } else if (formats[i] == SOAPY_SDR_CS16) {
                    have_cs16 = true;
                }
            }
            if (have_cf32) {
                dev->soapy_format = SOAPY_FMT_CF32;
            } else if (have_cs16) {
                dev->soapy_format = SOAPY_FMT_CS16;
            } else {
                fprintf(stderr, "SoapySDR: RX stream formats do not include CF32 or CS16.\n");
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "SoapySDR: exception selecting stream format: %s\n", e.what());
        }
        (void)dsd_mutex_unlock(&dev->soapy_lock);
    }

    return dev;
#endif
}

struct rtl_device*
rtl_device_create_iq_replay(const dsd_iq_replay_config* cfg, struct input_ring_state* input_ring,
                            const struct rtl_replay_eof_state* eof_state) {
    if (!cfg || !input_ring) {
        return NULL;
    }
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    rtl_device_init_common_state(dev);

    dev->dev = NULL;
    dev->dev_index = -1;
    dev->input_ring = input_ring;
    dev->thread_started = 0;
    dev->mute.store(0, std::memory_order_relaxed);
    dev->mute_byte_phase.store(0, std::memory_order_relaxed);
    dev->combine_rotate_enabled = cfg->combine_rotate_enabled ? 1 : 0;
    dev->backend = RTL_BACKEND_IQ_REPLAY;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_lock_inited = 0;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->sockfd = DSD_INVALID_SOCKET;
    dev->host[0] = '\0';
    dev->port = 0;
    dev->run.store(0, std::memory_order_relaxed);
    dev->agc_mode = 0;
    dev->offset_tuning = cfg->offset_tuning_enabled ? 1 : 0;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;
    dev->replay_fs4_shift_enabled = cfg->fs4_shift_enabled ? 1 : 0;
    dev->replay_combine_rotate_enabled = cfg->combine_rotate_enabled ? 1 : 0;
    if (eof_state) {
        dev->replay_eof = *eof_state;
        dev->replay_has_eof_state = 1;
    }

    dsd_iq_replay_config opened_cfg;
    dsd_iq_replay_source* replay_src = NULL;
    char err_buf[256] = {0};
    const char* open_path = cfg->metadata_path[0] ? cfg->metadata_path : cfg->data_path;
    int rc = dsd_iq_replay_open(open_path, &opened_cfg, &replay_src, err_buf, sizeof(err_buf));
    if (rc != DSD_IQ_OK || !replay_src) {
        fprintf(stderr, "IQ replay: failed to open '%s': %s\n", open_path, err_buf[0] ? err_buf : "unknown error");
        free(dev);
        return NULL;
    }

    /* Loop/realtime are runtime controls from CLI, not metadata fields. */
    opened_cfg.loop = cfg->loop ? 1 : 0;
    opened_cfg.realtime = cfg->realtime ? 1 : 0;
    dev->replay_cfg = opened_cfg;
    dev->replay_src = replay_src;
    dev->freq = (opened_cfg.capture_center_frequency_hz > UINT32_MAX)
                    ? UINT32_MAX
                    : (uint32_t)opened_cfg.capture_center_frequency_hz;
    dev->rate = opened_cfg.sample_rate_hz;
    dev->gain = opened_cfg.tuner_gain_tenth_db;
    dev->ppm_error = opened_cfg.ppm;
    return dev;
}

/**
 * @brief Destroy an RTL-SDR device and free resources.
 *
 * @param dev Pointer to rtl_device handle.
 */
void
rtl_device_destroy(struct rtl_device* dev) {
    if (!dev) {
        return;
    }

    if (dev->thread_started) {
        /* Ensure async read is cancelled before joining to avoid blocking */
        if (dev->backend == RTL_BACKEND_USB) {
            if (dev->dev) {
                rtlsdr_cancel_async(dev->dev);
            }
        } else if (dev->backend == RTL_BACKEND_TCP || dev->backend == RTL_BACKEND_SOAPY
                   || dev->backend == RTL_BACKEND_IQ_REPLAY) {
            dev->run.store(0);
            if (dev->backend == RTL_BACKEND_TCP && dev->sockfd != DSD_INVALID_SOCKET) {
                dsd_socket_shutdown(dev->sockfd, SHUT_RDWR);
            }
            if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
                if (dev->replay_has_eof_state && dev->replay_eof.replay_forced_stop) {
                    dev->replay_eof.replay_forced_stop->store(1, std::memory_order_release);
                }
                replay_signal_input_waiters(dev);
                if (dev->replay_has_eof_state && dev->replay_eof.eof_cond && dev->replay_eof.eof_m) {
                    dsd_mutex_lock(dev->replay_eof.eof_m);
                    dsd_cond_broadcast(dev->replay_eof.eof_cond);
                    dsd_mutex_unlock(dev->replay_eof.eof_m);
                }
            }
        }
        dsd_thread_join(dev->thread);
        dev->thread_started = 0;
    }

    /* Best-effort device state cleanup before closing the USB handle. */
    if (dev->backend == RTL_BACKEND_USB && dev->dev) {
        /* Disable bias tee so subsequent runs don't inherit stale 5V state. */
#ifdef USE_RTLSDR_BIAS_TEE
        (void)rtlsdr_set_bias_tee(dev->dev, 0);
#endif
        /* Reset buffers after cancel to leave device in a clean state. */
        (void)rtlsdr_reset_buffer(dev->dev);
    }

    if (dev->backend == RTL_BACKEND_USB && dev->dev) {
        rtlsdr_close(dev->dev);
    }
    if (dev->backend == RTL_BACKEND_TCP && dev->sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_close(dev->sockfd);
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        soapy_stream_cleanup(dev, 1);
        if (dev->soapy_lock_inited) {
            (void)dsd_mutex_destroy(&dev->soapy_lock);
            dev->soapy_lock_inited = 0;
        }
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY && dev->replay_src) {
        dsd_iq_replay_close(dev->replay_src);
        dev->replay_src = NULL;
    }
    if (dev->tcp_pending) {
        free(dev->tcp_pending);
        dev->tcp_pending = NULL;
        dev->tcp_pending_len = 0;
        dev->tcp_pending_cap = 0;
    }

    free(dev);
}

/**
 * @brief Set device center frequency.
 *
 * @param dev RTL-SDR device handle.
 * @param frequency Frequency in Hz.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_set_frequency(struct rtl_device* dev, uint32_t frequency) {
    if (!dev) {
        return -1;
    }
    dev->freq = frequency;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        return verbose_set_frequency(dev->dev, frequency);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_send_cmd(dev->sockfd, 0x01, frequency);
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        if (dev->replay_cfg.capture_center_frequency_hz > 0
            && (uint64_t)frequency != dev->replay_cfg.capture_center_frequency_hz) {
            fprintf(stderr, "IQ replay: ignoring retune request to %u Hz (capture center is %" PRIu64 " Hz).\n",
                    frequency, dev->replay_cfg.capture_center_frequency_hz);
        }
        return 0;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return soapy_call_locked(dev, "setFrequency", [&]() -> int {
            dev->soapy_dev->setFrequency(SOAPY_SDR_RX, 0, (double)frequency);
            return 0;
        });
    }
#endif
    return -1;
}

/**
 * @brief Set device sample rate.
 *
 * @param dev RTL-SDR device handle.
 * @param samp_rate Sample rate in Hz.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_set_sample_rate(struct rtl_device* dev, uint32_t samp_rate) {
    if (!dev) {
        return -1;
    }
    dev->rate = samp_rate;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        return verbose_set_sample_rate(dev->dev, samp_rate);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_send_cmd(dev->sockfd, 0x02, samp_rate);
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return soapy_call_locked(dev, "setSampleRate", [&]() -> int {
            dev->soapy_dev->setSampleRate(SOAPY_SDR_RX, 0, (double)samp_rate);
            return 0;
        });
    }
#endif
    return -1;
}

/**
 * @brief Get current device sample rate.
 *
 * For USB, queries librtlsdr for the actual rate applied (which may be
 * quantized). For rtl_tcp, returns the last programmed value.
 */
int
rtl_device_get_sample_rate(struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        return (int)rtlsdr_get_sample_rate(dev->dev);
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return (int)dev->replay_cfg.sample_rate_hz;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
#ifdef USE_SOAPYSDR
        double actual = 0.0;
        int rc = soapy_call_locked(dev, "getSampleRate", [&]() -> int {
            actual = dev->soapy_dev->getSampleRate(SOAPY_SDR_RX, 0);
            return 0;
        });
        if (rc == 0 && actual > 0.0) {
            uint32_t rounded = (uint32_t)(actual + 0.5);
            dev->rate = rounded;
            return (int)rounded;
        }
#endif
        return (dev->rate > 0) ? (int)dev->rate : -1;
    }
    return (int)dev->rate;
}

/**
 * @brief Set tuner gain mode and value.
 *
 * @param dev RTL-SDR device handle.
 * @param gain Gain in tenths of dB, or AUTO_GAIN for automatic.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_set_gain(struct rtl_device* dev, int gain) {
    if (!dev) {
        return -1;
    }

#define AUTO_GAIN (-100)
    dev->gain = gain;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        if (gain == AUTO_GAIN) {
            return verbose_auto_gain(dev->dev);
        } else {
            int nearest = nearest_gain(dev->dev, gain);
            return verbose_gain_set(dev->dev, nearest);
        }
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->agc_mode = 0;
        return 0;
    } else if (dev->backend == RTL_BACKEND_TCP) {
        if (gain == AUTO_GAIN) {
            dev->agc_mode = 1;
            int r = rtl_tcp_send_cmd(dev->sockfd, 0x03, 0); /* tuner auto */
            if (r < 0) {
                return r;
            }
            /* Mirror USB path: set RTL2832 digital AGC according to env */
            r = rtl_tcp_send_cmd(dev->sockfd, 0x08, (uint32_t)env_agc_want());
            return r;
        } else {
            dev->agc_mode = 0;
            int r = rtl_tcp_send_cmd(dev->sockfd, 0x03, 1);
            if (r < 0) {
                return r;
            }
            return rtl_tcp_send_cmd(dev->sockfd, 0x04, (uint32_t)gain);
        }
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        if (gain == AUTO_GAIN) {
            int rc = soapy_call_locked(dev, "setGainMode(auto)", [&]() -> int {
                if (!dev->soapy_dev->hasGainMode(SOAPY_SDR_RX, 0)) {
                    return DSD_ERR_NOT_SUPPORTED;
                }
                dev->soapy_dev->setGainMode(SOAPY_SDR_RX, 0, true);
                return 0;
            });
            if (rc == 0) {
                dev->agc_mode = 1;
            }
            return rc;
        }
        const double gain_db = (double)gain / 10.0;
        int rc = soapy_call_locked(dev, "setGain", [&]() -> int {
            if (dev->soapy_dev->hasGainMode(SOAPY_SDR_RX, 0)) {
                dev->soapy_dev->setGainMode(SOAPY_SDR_RX, 0, false);
            }
            dev->soapy_dev->setGain(SOAPY_SDR_RX, 0, gain_db);
            return 0;
        });
        if (rc == 0) {
            dev->agc_mode = 0;
        }
        return rc;
    }
#endif
    return -1;
}

int
rtl_device_set_gain_nearest(struct rtl_device* dev, int target_tenth_db) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        /* USB: find nearest supported and set manual gain */
        if (!dev->dev) {
            return -1;
        }
        int g = nearest_gain(dev->dev, target_tenth_db);
        if (g < 0) {
            return g;
        }
        int r = rtlsdr_set_tuner_gain_mode(dev->dev, 1);
        if (r < 0) {
            fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
            return r;
        }
        r = rtlsdr_set_tuner_gain(dev->dev, g);
        if (r < 0) {
            fprintf(stderr, "WARNING: Failed to set tuner gain (nearest).\n");
            return r;
        }
        dev->gain = g;
        fprintf(stderr, "Tuner manual gain (nearest): %0.1f dB.\n", (double)g / 10.0);
        return 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        /* rtl_tcp: request manual mode and set target directly */
        int mode = 1;
        (void)rtl_tcp_send_cmd(dev->sockfd, 0x03, (uint32_t)mode);
        (void)rtl_tcp_send_cmd(dev->sockfd, 0x04, (uint32_t)target_tenth_db);
        dev->gain = target_tenth_db;
        return 0;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->gain = target_tenth_db;
        return 0;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        const double target_db = (double)target_tenth_db / 10.0;
        double applied_db = target_db;
        int rc = soapy_call_locked(dev, "setGain(nearest)", [&]() -> int {
            std::vector<std::string> names = dev->soapy_dev->listGains(SOAPY_SDR_RX, 0);
            SoapySDR::Range range = dev->soapy_dev->getGainRange(SOAPY_SDR_RX, 0);
            if (names.empty() && (range.minimum() == range.maximum())) {
                return DSD_ERR_NOT_SUPPORTED;
            }
            if (applied_db < range.minimum()) {
                applied_db = range.minimum();
            }
            if (applied_db > range.maximum()) {
                applied_db = range.maximum();
            }
            if (dev->soapy_dev->hasGainMode(SOAPY_SDR_RX, 0)) {
                dev->soapy_dev->setGainMode(SOAPY_SDR_RX, 0, false);
            }
            dev->soapy_dev->setGain(SOAPY_SDR_RX, 0, applied_db);
            return 0;
        });
        if (rc == 0) {
            dev->agc_mode = 0;
            dev->gain = (int)(applied_db * 10.0 + (applied_db >= 0.0 ? 0.5 : -0.5));
        }
        return rc;
    }
#endif
    return -1;
}

int
rtl_device_get_tuner_gain(struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        return rtlsdr_get_tuner_gain(dev->dev);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        if (dev->agc_mode) {
            return 0;
        }
        return dev->gain;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return dev->replay_cfg.tuner_gain_tenth_db;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        double gain_db = 0.0;
        int rc = soapy_call_locked(dev, "getGain", [&]() -> int {
            std::vector<std::string> names = dev->soapy_dev->listGains(SOAPY_SDR_RX, 0);
            SoapySDR::Range range = dev->soapy_dev->getGainRange(SOAPY_SDR_RX, 0);
            if (names.empty() && (range.minimum() == range.maximum())) {
                return DSD_ERR_NOT_SUPPORTED;
            }
            gain_db = dev->soapy_dev->getGain(SOAPY_SDR_RX, 0);
            return 0;
        });
        if (rc != 0) {
            return rc;
        }
        return (int)(gain_db * 10.0 + (gain_db >= 0.0 ? 0.5 : -0.5));
    }
#endif
    return -1;
}

int
rtl_device_is_auto_gain(struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        /* We track AUTO vs manual in the requested field. */
        return (dev->gain == AUTO_GAIN) ? 1 : 0;
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    } else if (dev->backend == RTL_BACKEND_TCP || dev->backend == RTL_BACKEND_SOAPY) {
        return dev->agc_mode ? 1 : 0;
    }
    return -1;
}

/**
 * @brief Set frequency correction (PPM error).
 *
 * @param dev RTL-SDR device handle.
 * @param ppm_error PPM correction value.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_set_ppm(struct rtl_device* dev, int ppm_error) {
    if (!dev) {
        return -1;
    }
    /* Cache only the last correction that the backend accepted. Retries for a
     * previously failed value must still reach the driver/backend. */
    if (ppm_error == dev->ppm_error) {
        return 0;
    }
    int rc = -1;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        rc = verbose_ppm_set(dev->dev, ppm_error);
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rc = 0;
    } else if (dev->backend == RTL_BACKEND_TCP) {
        rc = rtl_tcp_send_cmd(dev->sockfd, 0x05, (uint32_t)ppm_error);
#ifdef USE_SOAPYSDR
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        rc = soapy_call_locked(dev, "setFrequencyCorrection", [&]() -> int {
            if (!dev->soapy_dev->hasFrequencyCorrection(SOAPY_SDR_RX, 0)) {
                return DSD_ERR_NOT_SUPPORTED;
            }
            dev->soapy_dev->setFrequencyCorrection(SOAPY_SDR_RX, 0, (double)ppm_error);
            return 0;
        });
#endif
    }
    if (rc == 0) {
        dev->ppm_error = ppm_error;
    }
    return rc;
}

/**
 * @brief Set direct sampling mode.
 *
 * @param dev RTL-SDR device handle.
 * @param on 1 to enable, 0 to disable.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_set_direct_sampling(struct rtl_device* dev, int on) {
    if (!dev) {
        return -1;
    }
    dev->direct_sampling = on;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        return verbose_direct_sampling(dev->dev, on);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_send_cmd(dev->sockfd, 0x09, (uint32_t)on);
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    return -1;
}

/**
 * @brief Enable or disable offset tuning mode.
 */
int
rtl_device_set_offset_tuning_enabled(struct rtl_device* dev, int on) {
    if (!dev) {
        return -1;
    }
    int r = 0;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        r = rtlsdr_set_offset_tuning(dev->dev, on ? 1 : 0);
        if (r == 0) {
            fprintf(stderr, on ? "Offset tuning mode enabled.\n" : "Offset tuning mode disabled.\n");
        } else {
            int t = rtlsdr_get_tuner_type(dev->dev);
            const char* tt = "unknown";
            switch (t) {
                case RTLSDR_TUNER_E4000: tt = "E4000"; break;
                case RTLSDR_TUNER_FC0012: tt = "FC0012"; break;
                case RTLSDR_TUNER_FC0013: tt = "FC0013"; break;
                case RTLSDR_TUNER_FC2580: tt = "FC2580"; break;
                case RTLSDR_TUNER_R820T: tt = "R820T"; break;
                case RTLSDR_TUNER_R828D: tt = "R828D"; break;
                default: break;
            }
            fprintf(stderr, "WARNING: Failed to set offset tuning (%d) for tuner %s.\n", r, tt);
        }
    } else if (dev->backend == RTL_BACKEND_TCP) {
        r = rtl_tcp_send_cmd(dev->sockfd, 0x0A, (uint32_t)(on ? 1 : 0));
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        r = 0;
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        r = DSD_ERR_NOT_SUPPORTED;
    } else {
        r = -1;
    }
    if (r == 0) {
        dev->offset_tuning = on ? 1 : 0;
    }
    return r;
}

int
rtl_device_set_tuner_bandwidth(struct rtl_device* dev, uint32_t bw_hz) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        return verbose_set_tuner_bandwidth(dev->dev, bw_hz);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        /* Not universally supported by rtl_tcp; ignore */
        (void)bw_hz;
        return 0;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        (void)bw_hz;
        return 0;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return soapy_call_locked(dev, "setBandwidth", [&]() -> int {
            SoapySDR::RangeList bw_range = dev->soapy_dev->getBandwidthRange(SOAPY_SDR_RX, 0);
            if (bw_range.empty()) {
                return DSD_ERR_NOT_SUPPORTED;
            }
            dev->soapy_dev->setBandwidth(SOAPY_SDR_RX, 0, (double)bw_hz);
            return 0;
        });
    }
#endif
    return -1;
}

/**
 * @brief Reset device buffer.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_reset_buffer(struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        return verbose_reset_buffer(dev->dev);
    } else {
        /* No explicit reset; treat as success */
        return 0;
    }
}

/**
 * @brief Start asynchronous reading from the device.
 *
 * @param dev RTL-SDR device handle.
 * @param buf_len Buffer length for async read.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_start_async(struct rtl_device* dev, uint32_t buf_len) {
    if (!dev || dev->thread_started) {
        return -1;
    }
    dev->buf_len = buf_len;
    rtl_reset_capture_state_on_stream_boundary(dev);
    dev->thread_started = 1;
    int r = 0;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            dev->thread_started = 0;
            return -1;
        }
        r = dsd_thread_create(&dev->thread, (dsd_thread_fn)dongle_thread_fn, dev);
    } else if (dev->backend == RTL_BACKEND_TCP) {
        dev->run.store(1);
        r = dsd_thread_create(&dev->thread, (dsd_thread_fn)tcp_thread_fn, dev);
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        if (!dev->replay_src) {
            dev->thread_started = 0;
            return -1;
        }
        dev->run.store(1, std::memory_order_release);
        r = dsd_thread_create(&dev->thread, (dsd_thread_fn)replay_thread_fn, dev);
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        if (!dev->soapy_dev) {
            dev->thread_started = 0;
            return -1;
        }
        dev->run.store(1);
        r = dsd_thread_create(&dev->thread, (dsd_thread_fn)soapy_thread_fn, dev);
    } else {
        dev->thread_started = 0;
        return -1;
    }
    if (r != 0) {
        dev->thread_started = 0;
        dev->run.store(0);
        return -1;
    }
    return 0;
}

/**
 * @brief Stop asynchronous reading and join the device thread.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_stop_async(struct rtl_device* dev) {
    if (!dev || !dev->thread_started) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        if (dev->dev) {
            rtlsdr_cancel_async(dev->dev);
        }
    } else if (dev->backend == RTL_BACKEND_TCP) {
        dev->run.store(0);
        if (dev->sockfd != DSD_INVALID_SOCKET) {
            dsd_socket_shutdown(dev->sockfd, SHUT_RDWR);
        }
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->run.store(0, std::memory_order_release);
        if (dev->replay_has_eof_state && dev->replay_eof.replay_forced_stop) {
            dev->replay_eof.replay_forced_stop->store(1, std::memory_order_release);
        }
        replay_signal_input_waiters(dev);
        if (dev->replay_has_eof_state && dev->replay_eof.eof_cond && dev->replay_eof.eof_m) {
            dsd_mutex_lock(dev->replay_eof.eof_m);
            dsd_cond_broadcast(dev->replay_eof.eof_cond);
            dsd_mutex_unlock(dev->replay_eof.eof_m);
        }
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        dev->run.store(0);
    } else {
        return -1;
    }
    dsd_thread_join(dev->thread);
    dev->thread_started = 0;
    rtl_reset_capture_state_on_stream_boundary(dev);
    return 0;
}

/**
 * @brief Mute the incoming raw input stream for a specified number of bytes.
 *
 * @param dev RTL-SDR device handle.
 * @param bytes Number of input bytes to discard while muting. Odd values are
 *              rounded up so the muted span always covers whole I/Q pairs.
 */
void
rtl_device_mute(struct rtl_device* dev, int bytes) {
    if (!dev) {
        return;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return;
    }
    dev->mute_byte_phase.store(0, std::memory_order_relaxed);
    dev->mute.store(rtl_capture_align_u8_iq_bytes(bytes), std::memory_order_relaxed);
}

int
rtl_device_set_bias_tee(struct rtl_device* dev, int on) {
    if (!dev) {
        return -1;
    }
    dev->bias_tee_on = on ? 1 : 0;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        /* rtl_tcp protocol command 0x0E toggles bias tee */
        return rtl_tcp_send_cmd(dev->sockfd, 0x0E, (uint32_t)dev->bias_tee_on);
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#ifdef USE_RTLSDR_BIAS_TEE
    if (!dev->dev) {
        return -1;
    }
    int r = rtlsdr_set_bias_tee(dev->dev, dev->bias_tee_on);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to %sable RTL-SDR bias tee.\n", dev->bias_tee_on ? "en" : "dis");
        return -1;
    }
    fprintf(stderr, "RTL-SDR bias tee %s.\n", dev->bias_tee_on ? "enabled" : "disabled");
    return 0;
#else
    (void)on;
    fprintf(stderr, "NOTE: librtlsdr built without bias tee API; ignoring bias setting on USB.\n");
    return DSD_ERR_NOT_SUPPORTED;
#endif
}

int
rtl_device_set_tcp_autotune(struct rtl_device* dev, int onoff) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        (void)onoff;
        return 0;
    }
    if (dev->backend != RTL_BACKEND_TCP) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    dev->tcp_autotune = onoff ? 1 : 0;
    return 0;
}

int
rtl_device_get_tcp_autotune(struct rtl_device* dev) {
    if (!dev) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend != RTL_BACKEND_TCP) {
        return 0;
    }
    return dev->tcp_autotune ? 1 : 0;
}

int
rtl_device_set_xtal_freq(struct rtl_device* dev, uint32_t rtl_xtal_hz, uint32_t tuner_xtal_hz) {
    if (!dev) {
        return -1;
    }
    dev->rtl_xtal_hz = rtl_xtal_hz;
    dev->tuner_xtal_hz = tuner_xtal_hz;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        if (dev->sockfd == DSD_INVALID_SOCKET) {
            return -1;
        }
        if (rtl_xtal_hz > 0) {
            (void)rtl_tcp_send_cmd(dev->sockfd, 0x0B, rtl_xtal_hz);
        }
        if (tuner_xtal_hz > 0) {
            (void)rtl_tcp_send_cmd(dev->sockfd, 0x0C, tuner_xtal_hz);
        }
        return 0;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    if (!dev->dev) {
        return -1;
    }
    int r = rtlsdr_set_xtal_freq(dev->dev, rtl_xtal_hz, tuner_xtal_hz);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to set xtal freq (rtl=%u, tuner=%u).\n", rtl_xtal_hz, tuner_xtal_hz);
        return -1;
    }
    fprintf(stderr, "Set xtal freq: rtl=%u Hz%s, tuner=%u Hz%s.\n", rtl_xtal_hz, rtl_xtal_hz ? "" : " (unchanged)",
            tuner_xtal_hz, tuner_xtal_hz ? "" : " (unchanged)");
    return 0;
}

int
rtl_device_set_testmode(struct rtl_device* dev, int on) {
    if (!dev) {
        return -1;
    }
    dev->testmode_on = on ? 1 : 0;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        if (dev->sockfd == DSD_INVALID_SOCKET) {
            return -1;
        }
        return rtl_tcp_send_cmd(dev->sockfd, 0x07, (uint32_t)(on ? 1 : 0));
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    if (!dev->dev) {
        return -1;
    }
    int r = rtlsdr_set_testmode(dev->dev, on ? 1 : 0);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to %s RTL-SDR test mode.\n", on ? "enable" : "disable");
        return -1;
    }
    fprintf(stderr, "RTL-SDR test mode %s.\n", on ? "enabled" : "disabled");
    return 0;
}

int
rtl_device_set_if_gain(struct rtl_device* dev, int stage, int gain_tenth_db) {
    if (!dev) {
        return -1;
    }
    if (stage < 0) {
        return -1;
    }
    int replaced = 0;
    for (int i = 0; i < dev->if_gain_count && i < 16; i++) {
        if (dev->if_gains[i].stage == stage) {
            dev->if_gains[i].gain = gain_tenth_db;
            replaced = 1;
            break;
        }
    }
    if (!replaced && dev->if_gain_count < 16) {
        dev->if_gains[dev->if_gain_count].stage = stage;
        dev->if_gains[dev->if_gain_count].gain = gain_tenth_db;
        dev->if_gain_count++;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        if (dev->sockfd == DSD_INVALID_SOCKET) {
            return -1;
        }
        uint32_t packed = ((uint32_t)(stage & 0xFFFF) << 16) | ((uint16_t)(gain_tenth_db & 0xFFFF));
        return rtl_tcp_send_cmd(dev->sockfd, 0x06, packed);
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    if (!dev->dev) {
        return -1;
    }
    int r = rtlsdr_set_tuner_if_gain(dev->dev, stage, (int16_t)gain_tenth_db);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to set IF gain: stage=%d, gain=%d (0.1 dB).\n", stage, gain_tenth_db);
        return -1;
    }
    fprintf(stderr, "IF gain set: stage=%d, gain=%0.1f dB.\n", stage, gain_tenth_db / 10.0);
    return 0;
}

void
rtl_device_set_iq_capture_writer(struct rtl_device* dev, struct dsd_iq_capture_writer* writer) {
    if (!dev) {
        return;
    }
    dev->iq_capture_writer = writer;
    if (!writer) {
        dev->capture_retune_count.store(0, std::memory_order_release);
    }
}

void
rtl_device_note_capture_retune(struct rtl_device* dev) {
    if (!dev || !dev->iq_capture_writer) {
        return;
    }
    (void)dev->capture_retune_count.fetch_add(1U, std::memory_order_relaxed);
}

uint32_t
rtl_device_get_capture_retune_count(struct rtl_device* dev) {
    if (!dev) {
        return 0;
    }
    return dev->capture_retune_count.load(std::memory_order_acquire);
}

int
rtl_device_get_native_sample_format(const struct rtl_device* dev) {
    if (!dev) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_USB || dev->backend == RTL_BACKEND_TCP) {
        return DSD_IQ_FORMAT_CU8;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        if (dev->soapy_format == SOAPY_FMT_CF32) {
            return DSD_IQ_FORMAT_CF32;
        }
        if (dev->soapy_format == SOAPY_FMT_CS16) {
            return DSD_IQ_FORMAT_CS16;
        }
        return 0;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return (int)dev->replay_cfg.format;
    }
    return 0;
}
