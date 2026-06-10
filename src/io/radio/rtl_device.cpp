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

#include "dsd-neo/core/input_level.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/dsp/simd_widen.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/tcp_quality_metrics.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <errno.h>
#include <iterator>
#include <limits.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"
#include "rtl_capture_phase.h"
#include "rtl_perf.h"
#include "rtl_replay_device.h"
#include "rtl_stream_shared.hpp"
#include "soapy_profile.h"

#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
#include <excpt.h>
#endif
/* Some platforms (e.g. non-glibc) may not define MSG_NOSIGNAL */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum : unsigned char {
    RTL_CAPTURE_RECONFIGURE_INACTIVE = 0,
    RTL_CAPTURE_RECONFIGURE_ACTIVE = 1,
    RTL_CAPTURE_RECONFIGURE_RELEASING = 2,
};

#ifdef USE_SOAPYSDR
#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.hpp>
#include <cctype>
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
#include <dsd-neo/core/parse.h>
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
rtlsdr_stub_status(void) {
    const char* env = getenv("DSD_NEO_RTLSDR_STUB_STATUS");
    if (env && *env != '\0') {
        int parsed = 0;
        if (dsd_parse_int_strict(env, 10, INT_MIN, INT_MAX, &parsed) == 0) {
            return parsed;
        }
    }
    return -1;
}

static uint32_t
rtlsdr_stub_sample_rate(void) {
    const char* env = getenv("DSD_NEO_RTLSDR_STUB_SAMPLE_RATE_HZ");
    if (env && *env != '\0') {
        uint32_t parsed = 0;
        if (dsd_parse_uint32_strict(env, 10, UINT32_MAX, &parsed) == 0 && parsed > 0U) {
            return parsed;
        }
    }
    return 0U;
}

static int
rtlsdr_stub_tuner_type(void) {
    const char* env = getenv("DSD_NEO_RTLSDR_STUB_TUNER_TYPE");
    if (env && *env != '\0') {
        int parsed = RTLSDR_TUNER_UNKNOWN;
        if (dsd_parse_int_strict(env, 10, INT_MIN, INT_MAX, &parsed) == 0) {
            return parsed;
        }
    }
    return RTLSDR_TUNER_UNKNOWN;
}

static int
rtlsdr_open(rtlsdr_dev_t** dev, uint32_t index) {
    (void)dev;
    (void)index;
    return rtlsdr_stub_status();
}

static int
rtlsdr_close(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtlsdr_stub_status();
}

static int
rtlsdr_read_async(rtlsdr_dev_t* dev, void (*cb)(unsigned char*, uint32_t, void*), void* ctx, uint32_t buf_num,
                  uint32_t buf_len) {
    (void)dev;
    (void)cb;
    (void)ctx;
    (void)buf_num;
    (void)buf_len;
    return rtlsdr_stub_status();
}

static int
rtlsdr_cancel_async(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t* dev, int manual) {
    (void)dev;
    (void)manual;
    return rtlsdr_stub_status();
}

static int
rtlsdr_get_tuner_gains(rtlsdr_dev_t* dev, int* gains) {
    (void)dev;
    int count = rtlsdr_stub_status();
    if (count <= 0) {
        return count;
    }
    if (gains != nullptr) {
        gains[0] = 0;
        return 1;
    }
    return count;
}

static int
rtlsdr_set_center_freq(rtlsdr_dev_t* dev, uint32_t freq) {
    (void)dev;
    (void)freq;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate) {
    (void)dev;
    (void)rate;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_direct_sampling(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtlsdr_stub_status();
}

static int
rtlsdr_get_tuner_type(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtlsdr_stub_tuner_type();
}

static int
rtlsdr_set_tuner_bandwidth(rtlsdr_dev_t* dev, int bw_hz) {
    (void)dev;
    (void)bw_hz;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_agc_mode(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_tuner_gain(rtlsdr_dev_t* dev, int gain) {
    (void)dev;
    (void)gain;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_freq_correction(rtlsdr_dev_t* dev, int ppm) {
    (void)dev;
    (void)ppm;
    return rtlsdr_stub_status();
}

static int
rtlsdr_reset_buffer(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtlsdr_stub_status();
}

static uint32_t
rtlsdr_get_sample_rate(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtlsdr_stub_sample_rate();
}

static int
rtlsdr_get_tuner_gain(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_offset_tuning(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_xtal_freq(rtlsdr_dev_t* dev, uint32_t rtl, uint32_t tuner) {
    (void)dev;
    (void)rtl;
    (void)tuner;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_testmode(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtlsdr_stub_status();
}

static int
rtlsdr_set_tuner_if_gain(rtlsdr_dev_t* dev, int stage, int gain) {
    (void)dev;
    (void)stage;
    (void)gain;
    return rtlsdr_stub_status();
}

#ifdef USE_RTLSDR_BIAS_TEE
static int
rtlsdr_set_bias_tee(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtlsdr_stub_status();
}
#endif
#endif

enum : unsigned char {
    RTL_BACKEND_USB = 0,
    RTL_BACKEND_TCP = 1,
    RTL_BACKEND_SOAPY = 2,
    RTL_BACKEND_IQ_REPLAY = 3,
};

enum : unsigned char {
    SOAPY_FMT_NONE = 0,
    SOAPY_FMT_CF32 = 1,
    SOAPY_FMT_CS16 = 2,
};

static const int RTL_AUTO_GAIN = -100;

static const char*
rtl_tuner_type_name(int tuner_type) {
    switch (tuner_type) {
        case RTLSDR_TUNER_E4000: return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        case RTLSDR_TUNER_R820T: return "R820T";
        case RTLSDR_TUNER_R828D: return "R828D";
        default: return "unknown";
    }
}

// Internal RTL device structure
struct rtl_device {
    rtlsdr_dev_t* dev = nullptr;
    dsd_thread_t thread{};
    struct input_ring_state* input_ring = nullptr;
    struct dsd_iq_capture_writer* iq_capture_writer = nullptr;
    std::atomic<uint64_t> capture_mute_pending_bytes{0U};
    uint64_t replay_initial_center_frequency_hz = 0U;
    uint64_t replay_initial_capture_center_frequency_hz = 0U;
    dsd_iq_replay_source* replay_src = nullptr;
    uint64_t replay_float_elements_written = 0U;
    /* SoapySDR backend */
    SoapySDR::Device* soapy_dev = nullptr;
    SoapySDR::Stream* soapy_stream = nullptr;
    uint64_t soapy_overflow_count = 0U;
    uint64_t soapy_timeout_count = 0U;
    uint64_t soapy_read_errors = 0U;
    uint64_t soapy_last_overflow_log_ns = 0U;
    int soapy_profile_id = 0;
    int soapy_requested_bandwidth_hz = 0;
    int soapy_named_gain_override = 0;
    int soapy_named_gain_skip_logged = 0;
    char soapy_args_string[1024] = {};
    char soapy_driver_key[64] = {};
    char soapy_hardware_key[128] = {};
    char soapy_native_stream_format[16] = {};
    char soapy_requested_profile[32] = {};
    char soapy_requested_antenna[64] = {};
    char soapy_requested_clock[64] = {};
    char soapy_requested_settings[1024] = {};
    char soapy_requested_gains[512] = {};
    char soapy_requested_stream_format[16] = {};
    /* TCP stats (optional) */
    uint64_t tcp_bytes_total = 0U;
    uint64_t tcp_bytes_window = 0U;
    uint64_t reserve_full_events = 0U;
    uint64_t stats_last_ns = 0U;
    /* TCP reassembly to uniform chunk size */
    unsigned char* tcp_pending = nullptr;
    size_t tcp_pending_len = 0U;
    size_t tcp_pending_cap = 0U;
    dsd_mutex_t soapy_lock{};
    dsd_mutex_t tcp_metrics_lock{};
    /* TCP connection quality metrics (lag resilience) */
    struct tcp_quality_metrics tcp_metrics{};
    struct rtl_replay_eof_state replay_eof{};
    dsd_iq_replay_config replay_cfg{};
    int dev_index = 0;
    uint32_t freq = 0U;
    uint32_t rate = 0U;
    int gain = 0;
    uint32_t buf_len = 0U;
    int ppm_error = 0;
    int offset_tuning = 0;
    int direct_sampling = 0;
    std::atomic<int> mute{0};
    int thread_started = 0;
    int combine_rotate_enabled = 0;
    std::atomic<uint32_t> capture_retune_count{0U};
    std::atomic<int> capture_reconfigure_hold{0};
    /* Backend selector: 0 = USB (librtlsdr), 1 = rtl_tcp, 2 = SoapySDR */
    int backend = 0;
    int replay_fs4_shift_enabled = 0;
    int replay_combine_rotate_enabled = 0;
    uint32_t replay_initial_sample_rate_hz = 0U;
    uint32_t replay_initial_freq = 0U;
    uint32_t replay_initial_rate = 0U;
    int replay_has_eof_state = 0;
    int soapy_lock_inited = 0;
    int soapy_format = 0; /* SOAPY_FMT_* */
    uint32_t soapy_mtu_elems = 0U;
    int rot_phase = 0;                   /* persistent j^n phase in [0..3] for capture-side FS/4 rotation */
    std::atomic<int> mute_byte_phase{0}; /* byte carry while an active mute span is discarded in fragments */
    /* rtl_tcp connection */
    dsd_socket_t sockfd{};
    int port = 0;
    std::atomic<int> run{0};
    int agc_mode = 0; /* cached for TCP backend */
    int bias_tee_on = 0;
    int tcp_autotune = 0; /* adaptive recv/buffering */
    int stats_enabled = 0;
    int tcp_metrics_lock_inited = 0;
    /* Extra driver state for reconnect replay */
    int testmode_on = 0;
    uint32_t rtl_xtal_hz = 0U;
    uint32_t tuner_xtal_hz = 0U;
    int if_gain_count = 0;
    struct rtl_capture_u8_byte_carry iq_byte_carry{}; /* one buffered raw byte when a chunk ends mid-I/Q sample */

    struct {
        int stage = 0;
        int gain = 0;
    } if_gains[16] = {};

    char host[1024] = {};
};

static void
rtl_tcp_metrics_lock(struct rtl_device* dev) {
    if (dev && dev->tcp_metrics_lock_inited) {
        (void)dsd_mutex_lock(&dev->tcp_metrics_lock);
    }
}

static void
rtl_tcp_metrics_unlock(struct rtl_device* dev) {
    if (dev && dev->tcp_metrics_lock_inited) {
        (void)dsd_mutex_unlock(&dev->tcp_metrics_lock);
    }
}

static void
rtl_tcp_metrics_reset_device(struct rtl_device* dev, uint32_t sample_rate) {
    if (!dev) {
        return;
    }
    rtl_tcp_metrics_lock(dev);
    tcp_metrics_reset(&dev->tcp_metrics, sample_rate);
    rtl_tcp_metrics_unlock(dev);
}

static int
rtl_tcp_metrics_record_recv_device(struct rtl_device* dev, uint32_t bytes_received, uint64_t now_ns) {
    if (!dev) {
        return 0;
    }
    rtl_tcp_metrics_lock(dev);
    int fired = tcp_metrics_record_recv(&dev->tcp_metrics, bytes_received, now_ns);
    rtl_tcp_metrics_unlock(dev);
    return fired;
}

static void
rtl_tcp_metrics_update_ring_snapshot(struct rtl_device* dev) {
    if (!dev || !dev->input_ring) {
        return;
    }
    size_t used = input_ring_used(dev->input_ring);
    size_t capacity = dev->input_ring->capacity;
    uint64_t producer_drops = dev->input_ring->producer_drops.load(std::memory_order_acquire);

    rtl_tcp_metrics_lock(dev);
    tcp_metrics_update_ring_fill(&dev->tcp_metrics, used, capacity);
    dev->tcp_metrics.snapshot.producer_drops = producer_drops;
    rtl_tcp_metrics_unlock(dev);
}

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
rtl_process_u8_chunk(const struct rtl_device* s, unsigned char* src, float* dst, size_t len, int fs4_shift_active,
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
rtl_complete_fragmented_capture_discard(struct rtl_device* s) {
    if (!s || (s->backend != RTL_BACKEND_USB && s->backend != RTL_BACKEND_TCP)) {
        return;
    }

    unsigned int carry = (unsigned int)(s->mute_byte_phase.load(std::memory_order_relaxed) & 1U);
    if ((carry & 1U) == 0U) {
        return;
    }

    int remaining = s->mute.load(std::memory_order_relaxed);
    for (;;) {
        int realigned = rtl_capture_complete_fragmented_u8_discard(remaining, carry);
        if (realigned == remaining) {
            break;
        }
        if (s->mute.compare_exchange_weak(remaining, realigned, std::memory_order_relaxed)) {
            break;
        }
    }
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
    rtl_stream_input_level_reset();
}

static inline void
rtl_clear_capture_alignment_after_discard(struct rtl_device* s, struct rtl_capture_u8_byte_carry* carry) {
    if (!s) {
        return;
    }
    if (carry) {
        rtl_capture_u8_byte_carry_clear(carry);
    }
    rtl_capture_u8_byte_carry_clear(&s->iq_byte_carry);
    s->mute_byte_phase.store(0, std::memory_order_relaxed);
}

static inline void
rtl_even_split_ring_reserve(size_t ready, size_t n1, size_t n2, size_t* w1, size_t* w2) {
    if (!w1 || !w2) {
        return;
    }
    if (n1 & 1U) {
        n1--;
    }
    *w1 = (n1 < ready) ? n1 : ready;
    size_t rem_after_w1 = ready - *w1;
    if (n2 & 1U) {
        n2--;
    }
    *w2 = (n2 < rem_after_w1) ? n2 : rem_after_w1;
}

static inline void
rtl_accumulate_ring_drops(struct input_ring_state* ring, size_t dropped) {
    if (!ring || dropped == 0U) {
        return;
    }
    ring->producer_drops.fetch_add((uint64_t)dropped);
}

namespace {

struct rtl_u8_write_cursor {
    unsigned char* src;
    size_t* done;
    size_t* need;
    struct rtl_capture_u8_byte_carry* carry;
    int* phase;
    int fs4_shift_active;
    int combine_rotate_active;
    int use_two_pass;
};

} // namespace

static inline void
rtl_write_u8_reserved_segment(const struct rtl_device* s, const rtl_u8_write_cursor* cursor, float* dst,
                              size_t produced_bytes) {
    if (!s || !cursor || !cursor->src || !cursor->done || !cursor->need || !cursor->carry || !dst || !cursor->phase
        || produced_bytes == 0U) {
        return;
    }

    unsigned char pair[2];
    size_t prefix =
        rtl_capture_u8_byte_carry_consume_prefix(cursor->src + *cursor->done, *cursor->need, cursor->carry, pair);
    size_t from_prefix = 0U;
    if (prefix != 0U) {
        rtl_process_u8_chunk(s, pair, dst, 2U, cursor->fs4_shift_active, cursor->combine_rotate_active,
                             cursor->use_two_pass, cursor->phase);
        *cursor->done += prefix;
        *cursor->need -= prefix;
        from_prefix = 2U;
    }
    if (produced_bytes > from_prefix) {
        size_t body = produced_bytes - from_prefix;
        rtl_process_u8_chunk(s, cursor->src + *cursor->done, dst + from_prefix, body, cursor->fs4_shift_active,
                             cursor->combine_rotate_active, cursor->use_two_pass, cursor->phase);
        *cursor->done += body;
        *cursor->need -= body;
    }
}

static inline int
rtl_handle_u8_ring_generation_stale(const struct rtl_device* s, const unsigned char* src, size_t done, size_t need,
                                    struct rtl_capture_u8_byte_carry* carry, int* phase, int fs4_shift_active,
                                    size_t produced) {
    if (!s || !src || !carry || !phase) {
        return 0;
    }
    size_t dropped = produced + rtl_drop_u8_bytes_preserve_alignment(src + done, need, carry, phase, fs4_shift_active);
    rtl_accumulate_ring_drops(s->input_ring, dropped);
    return 1;
}

namespace {

struct rtl_u8_finalize_state {
    const unsigned char* src;
    size_t len;
    size_t done;
    size_t need;
    struct rtl_capture_u8_byte_carry* carry;
    int* phase;
    int fs4_shift_active;
    int count_full_reserve;
    int ring_exhausted;
    int generation_stale;
};

} // namespace

static inline void
rtl_finalize_u8_ring_write(struct rtl_device* s, const rtl_u8_finalize_state* final_state) {
    if (!s || !final_state || !final_state->src || !final_state->carry || !final_state->phase) {
        return;
    }

    if (final_state->generation_stale) {
        rtl_clear_capture_alignment_after_discard(s, final_state->carry);
    } else if (final_state->ring_exhausted) {
        size_t dropped =
            rtl_drop_u8_bytes_preserve_alignment(final_state->src + final_state->done, final_state->need,
                                                 final_state->carry, final_state->phase, final_state->fs4_shift_active);
        rtl_accumulate_ring_drops(s->input_ring, dropped);
        if (final_state->count_full_reserve) {
            s->reserve_full_events++;
        }
    } else if (final_state->need == 1U && final_state->done < final_state->len && !final_state->carry->valid) {
        rtl_capture_u8_byte_carry_save(final_state->carry, final_state->src[final_state->done]);
    }

    if (!final_state->generation_stale) {
        s->iq_byte_carry = *final_state->carry;
        if (final_state->fs4_shift_active) {
            s->rot_phase = *final_state->phase;
        }
    }
}

static int
rtl_write_u8_to_ring(struct rtl_device* s, unsigned char* src, size_t len, int fs4_shift_active, int use_two_pass,
                     int combine_rotate_active, int count_full_reserve) {
    if (!s || !s->input_ring || !src || len == 0) {
        return 0;
    }

    int perf_on = rtl_perf_enabled();
    uint64_t perf_t0 = perf_on ? rtl_perf_now_ns() : 0ULL;
    uint64_t perf_drops_before = perf_on ? s->input_ring->producer_drops.load(std::memory_order_relaxed) : 0ULL;
    size_t done = 0;
    size_t need = len;
    int phase = s->rot_phase & 3;
    struct rtl_capture_u8_byte_carry carry = s->iq_byte_carry;
    int ring_exhausted = 0;
    int generation_stale = 0;

    while (rtl_capture_u8_byte_carry_ready_bytes(need, &carry) >= 2U) {
        uint64_t discard_generation = input_ring_discard_generation(s->input_ring);
        float *p1 = NULL, *p2 = NULL;
        size_t n1 = 0, n2 = 0;
        size_t ready = rtl_capture_u8_byte_carry_ready_bytes(need, &carry);
        input_ring_reserve(s->input_ring, ready, &p1, &n1, &p2, &n2);
        if (n1 == 0 && n2 == 0) {
            ring_exhausted = 1;
            break;
        }

        size_t w1 = 0U;
        size_t w2 = 0U;
        rtl_even_split_ring_reserve(ready, n1, n2, &w1, &w2);
        size_t produced = w1 + w2;
        if (produced == 0) {
            ring_exhausted = 1;
            break;
        }

        rtl_u8_write_cursor cursor = {
            src, &done, &need, &carry, &phase, fs4_shift_active, combine_rotate_active, use_two_pass};
        rtl_write_u8_reserved_segment(s, &cursor, p1, w1);
        rtl_write_u8_reserved_segment(s, &cursor, p2, w2);

        if (!input_ring_discard_generation_matches(s->input_ring, discard_generation)) {
            generation_stale =
                rtl_handle_u8_ring_generation_stale(s, src, done, need, &carry, &phase, fs4_shift_active, produced);
            break;
        }
        input_ring_commit(s->input_ring, produced);
    }

    rtl_u8_finalize_state final_state = {
        src, len, done, need, &carry, &phase, fs4_shift_active, count_full_reserve, ring_exhausted, generation_stale};
    rtl_finalize_u8_ring_write(s, &final_state);
    if (perf_on) {
        uint64_t drops_after = s->input_ring->producer_drops.load(std::memory_order_relaxed);
        uint64_t drops_delta = (drops_after >= perf_drops_before) ? (drops_after - perf_drops_before) : 0ULL;
        rtl_perf_record_ingest(rtl_perf_now_ns() - perf_t0, done, drops_delta);
    }
    return generation_stale ? 2 : ring_exhausted;
}

static inline void
rtl_submit_capture_bytes(struct rtl_device* s, const void* data, size_t bytes) {
    if (!s || !s->iq_capture_writer || !data || bytes == 0) {
        return;
    }
    (void)dsd_iq_capture_submit(s->iq_capture_writer, data, bytes);
}

static inline dsd_input_level_source
rtl_u8_input_level_source(const struct rtl_device* s) {
    return (s && s->backend == RTL_BACKEND_TCP) ? DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8 : DSD_INPUT_LEVEL_SOURCE_RTL_CU8;
}

static inline void
rtl_publish_cu8_input_level(const struct rtl_device* s, const uint8_t* samples, size_t count) {
    dsd_input_level_snapshot snapshot;
    if (!s || !samples || count == 0U) {
        return;
    }
    if (dsd_input_level_metrics_from_cu8(samples, count, rtl_u8_input_level_source(s), &snapshot) == 0) {
        rtl_stream_input_level_publish(&snapshot);
    }
}

#ifdef USE_SOAPYSDR
static inline void
rtl_publish_cs16_input_level(const int16_t* samples, size_t count) {
    dsd_input_level_snapshot snapshot;
    if (!samples || count == 0U) {
        return;
    }
    if (dsd_input_level_metrics_from_cs16(samples, count, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16, &snapshot) == 0) {
        rtl_stream_input_level_publish(&snapshot);
    }
}

static inline void
rtl_publish_cf32_input_level(const float* samples, size_t count) {
    dsd_input_level_snapshot snapshot;
    if (!samples || count == 0U) {
        return;
    }
    if (dsd_input_level_metrics_from_cf32(samples, count, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32, &snapshot) == 0) {
        rtl_stream_input_level_publish(&snapshot);
    }
}
#endif

static inline void
rtl_copy_event_reason(char* dst, size_t dst_size, const char* reason) {
    if (!dst || dst_size == 0U) {
        return;
    }
    if (!reason) {
        reason = "";
    }
    DSD_SNPRINTF(dst, dst_size, "%s", reason);
}

static inline void
rtl_record_capture_event(struct rtl_device* s, const dsd_iq_event* event) {
    if (!s || !s->iq_capture_writer || !event) {
        return;
    }
    (void)dsd_iq_capture_record_event(s->iq_capture_writer, event);
}

static inline int
rtl_capture_reconfigure_hold_active(const struct rtl_device* s) {
    return (s && s->capture_reconfigure_hold.load(std::memory_order_acquire)) ? 1 : 0;
}

static inline int
rtl_capture_reconfigure_hold_accepting_discards(const struct rtl_device* s) {
    return (s && s->capture_reconfigure_hold.load(std::memory_order_acquire) == RTL_CAPTURE_RECONFIGURE_ACTIVE) ? 1 : 0;
}

static inline size_t
rtl_capture_event_alignment_bytes(const struct rtl_device* s) {
    if (!s) {
        return 0U;
    }
    if (s->backend == RTL_BACKEND_USB || s->backend == RTL_BACKEND_TCP) {
        return 2U;
    }
    if (s->backend == RTL_BACKEND_SOAPY) {
        if (s->soapy_format == SOAPY_FMT_CF32) {
            return 2U * sizeof(float);
        }
        if (s->soapy_format == SOAPY_FMT_CS16) {
            return 2U * sizeof(int16_t);
        }
    }
    return 0U;
}

static inline uint64_t
rtl_coalesce_capture_mute_duration(std::atomic<uint64_t>* pending_bytes, uint64_t duration_bytes, size_t alignment) {
    if (!pending_bytes || duration_bytes == 0U || alignment <= 1U) {
        return duration_bytes;
    }

    uint64_t pending = pending_bytes->load(std::memory_order_acquire);
    for (;;) {
        uint64_t next_pending = 0U;
        uint64_t emit = 0U;
        if (UINT64_MAX - pending < duration_bytes) {
            emit = duration_bytes - (duration_bytes % (uint64_t)alignment);
        } else {
            uint64_t total = pending + duration_bytes;
            emit = total - (total % (uint64_t)alignment);
            next_pending = total - emit;
        }
        if (pending_bytes->compare_exchange_weak(pending, next_pending, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            return emit;
        }
    }
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
static inline uint64_t
rtl_coalesce_capture_mute_duration(uint64_t* pending_bytes, uint64_t duration_bytes, size_t alignment) {
    if (!pending_bytes || duration_bytes == 0U || alignment <= 1U) {
        return duration_bytes;
    }

    if (UINT64_MAX - *pending_bytes < duration_bytes) {
        *pending_bytes = 0U;
        return duration_bytes - (duration_bytes % (uint64_t)alignment);
    }

    uint64_t total = *pending_bytes + duration_bytes;
    uint64_t emit = total - (total % (uint64_t)alignment);
    *pending_bytes = total - emit;
    return emit;
}

extern "C" uint64_t
rtl_device_test_coalesce_capture_mute_duration(uint64_t* pending_bytes, uint64_t duration_bytes, size_t alignment) {
    return rtl_coalesce_capture_mute_duration(pending_bytes, duration_bytes, alignment);
}

extern "C" int
rtl_device_test_complete_fragmented_capture_discard(int byte_count, unsigned int partial_byte_count) {
    return rtl_capture_complete_fragmented_u8_discard(byte_count, partial_byte_count);
}
#endif

static inline void
rtl_finish_capture_mute_span(struct rtl_device* s) {
    if (!s) {
        return;
    }
    s->capture_mute_pending_bytes.store(0U, std::memory_order_release);
}

static inline void
rtl_record_capture_mute(struct rtl_device* s, uint64_t duration_bytes, const char* reason) {
    if (!s || !s->iq_capture_writer || duration_bytes == 0U) {
        return;
    }
    duration_bytes = rtl_coalesce_capture_mute_duration(&s->capture_mute_pending_bytes, duration_bytes,
                                                        rtl_capture_event_alignment_bytes(s));
    if (duration_bytes == 0U) {
        return;
    }
    dsd_iq_event event;
    DSD_MEMSET(&event, 0, sizeof(event));
    event.kind = DSD_IQ_EVENT_MUTE;
    event.duration_bytes = duration_bytes;
    rtl_copy_event_reason(event.reason, sizeof(event.reason), reason);
    rtl_record_capture_event(s, &event);
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

static inline size_t
soapy_reserve_even_ring_segments(struct input_ring_state* ring, size_t need, float** p1, size_t* w1, float** p2,
                                 size_t* w2) {
    if (!ring || !p1 || !w1 || !p2 || !w2) {
        return 0U;
    }
    size_t n1 = 0U;
    size_t n2 = 0U;
    input_ring_reserve(ring, need, p1, &n1, p2, &n2);
    rtl_even_split_ring_reserve(need, n1, n2, w1, w2);
    return *w1 + *w2;
}

static inline void
soapy_handle_no_space(struct input_ring_state* ring, size_t need, int apply_rot, int* phase) {
    if (!ring) {
        return;
    }
    rtl_accumulate_ring_drops(ring, need);
    if (apply_rot && phase) {
        *phase = rtl_capture_phase_advance_pairs(*phase, need / 2U);
    }
}

static inline int
soapy_finalize_generation(struct input_ring_state* ring, uint64_t discard_generation, size_t produced) {
    if (!ring) {
        return 1;
    }
    if (!input_ring_discard_generation_matches(ring, discard_generation)) {
        rtl_accumulate_ring_drops(ring, produced);
        return 1;
    }
    input_ring_commit(ring, produced);
    return 0;
}

static inline void
soapy_copy_cf32_samples(float* dst, const std::complex<float>* src, size_t elem_count, int apply_rot, int* phase) {
    if (!dst || !src || elem_count == 0U) {
        return;
    }
    for (size_t i = 0U; i < elem_count; i++) {
        float i_in = src[i].real();
        float q_in = src[i].imag();
        if (apply_rot && phase) {
            apply_j4_rotation(i_in, q_in, *phase, &dst[(i * 2U) + 0U], &dst[(i * 2U) + 1U]);
            *phase = (*phase + 1) & 3;
        } else {
            dst[(i * 2U) + 0U] = i_in;
            dst[(i * 2U) + 1U] = q_in;
        }
    }
}

static inline void
soapy_copy_cs16_samples(float* dst, const int16_t* src_iq, size_t elem_count, float scale, int apply_rot, int* phase) {
    if (!dst || !src_iq || elem_count == 0U) {
        return;
    }
    for (size_t i = 0U; i < elem_count; i++) {
        size_t sample_idx = i * 2U;
        float i_in = (float)src_iq[sample_idx + 0U] * scale;
        float q_in = (float)src_iq[sample_idx + 1U] * scale;
        if (apply_rot && phase) {
            apply_j4_rotation(i_in, q_in, *phase, &dst[(i * 2U) + 0U], &dst[(i * 2U) + 1U]);
            *phase = (*phase + 1) & 3;
        } else {
            dst[(i * 2U) + 0U] = i_in;
            dst[(i * 2U) + 1U] = q_in;
        }
    }
}

static size_t
soapy_write_cf32_to_ring(struct rtl_device* s, const std::complex<float>* src, size_t num_elems, int apply_rot) {
    if (!s || !s->input_ring || !src || num_elems == 0) {
        return 0;
    }
    int perf_on = rtl_perf_enabled();
    uint64_t perf_t0 = perf_on ? rtl_perf_now_ns() : 0ULL;
    uint64_t perf_drops_before = perf_on ? s->input_ring->producer_drops.load(std::memory_order_relaxed) : 0ULL;
    size_t need = num_elems * 2;
    size_t done = 0;
    int phase = s->rot_phase & 3;
    while (need > 0) {
        uint64_t discard_generation = input_ring_discard_generation(s->input_ring);
        float *p1 = NULL, *p2 = NULL;
        size_t w1 = 0U;
        size_t w2 = 0U;
        size_t produced = soapy_reserve_even_ring_segments(s->input_ring, need, &p1, &w1, &p2, &w2);
        if (produced == 0U) {
            soapy_handle_no_space(s->input_ring, need, apply_rot, &phase);
            break;
        }
        size_t src_idx = done / 2U;
        soapy_copy_cf32_samples(p1, src + src_idx, w1 / 2U, apply_rot, &phase);
        src_idx += w1 / 2U;
        soapy_copy_cf32_samples(p2, src + src_idx, w2 / 2U, apply_rot, &phase);
        if (soapy_finalize_generation(s->input_ring, discard_generation, produced)) {
            break;
        }
        done += produced;
        need -= produced;
    }
    if (apply_rot) {
        s->rot_phase = phase;
    }
    if (perf_on) {
        uint64_t drops_after = s->input_ring->producer_drops.load(std::memory_order_relaxed);
        uint64_t drops_delta = (drops_after >= perf_drops_before) ? (drops_after - perf_drops_before) : 0ULL;
        rtl_perf_record_ingest(rtl_perf_now_ns() - perf_t0, done, drops_delta);
    }
    return done / 2;
}

static size_t
soapy_write_cs16_to_ring(struct rtl_device* s, const int16_t* src, size_t num_elems, int apply_rot) {
    if (!s || !s->input_ring || !src || num_elems == 0) {
        return 0;
    }
    int perf_on = rtl_perf_enabled();
    uint64_t perf_t0 = perf_on ? rtl_perf_now_ns() : 0ULL;
    uint64_t perf_drops_before = perf_on ? s->input_ring->producer_drops.load(std::memory_order_relaxed) : 0ULL;
    const float scale = 1.0f / 32768.0f;
    size_t need = num_elems * 2;
    size_t done = 0;
    int phase = s->rot_phase & 3;
    while (need > 0) {
        uint64_t discard_generation = input_ring_discard_generation(s->input_ring);
        float *p1 = NULL, *p2 = NULL;
        size_t w1 = 0U;
        size_t w2 = 0U;
        size_t produced = soapy_reserve_even_ring_segments(s->input_ring, need, &p1, &w1, &p2, &w2);
        if (produced == 0U) {
            soapy_handle_no_space(s->input_ring, need, apply_rot, &phase);
            break;
        }
        size_t src_idx = done / 2U;
        soapy_copy_cs16_samples(p1, src + (src_idx * 2U), w1 / 2U, scale, apply_rot, &phase);
        src_idx += w1 / 2U;
        soapy_copy_cs16_samples(p2, src + (src_idx * 2U), w2 / 2U, scale, apply_rot, &phase);
        if (soapy_finalize_generation(s->input_ring, discard_generation, produced)) {
            break;
        }
        done += produced;
        need -= produced;
    }
    if (apply_rot) {
        s->rot_phase = phase;
    }
    if (perf_on) {
        uint64_t drops_after = s->input_ring->producer_drops.load(std::memory_order_relaxed);
        uint64_t drops_delta = (drops_after >= perf_drops_before) ? (drops_after - perf_drops_before) : 0ULL;
        rtl_perf_record_ingest(rtl_perf_now_ns() - perf_t0, done, drops_delta);
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
replay_event_boundary_drained(struct rtl_device* s) {
    if (!s || !s->input_ring || s->input_ring->capacity == 0U) {
        return 1;
    }

    const struct input_ring_state* ring = s->input_ring;
    int generation_drained = 1;
    if (s->replay_has_eof_state && s->replay_eof.replay_last_submit_gen && s->replay_eof.replay_last_consume_gen) {
        uint64_t submitted = s->replay_eof.replay_last_submit_gen->load(std::memory_order_acquire);
        uint64_t consumed = s->replay_eof.replay_last_consume_gen->load(std::memory_order_acquire);
        generation_drained = (consumed >= submitted) ? 1 : 0;
    }
    return (input_ring_used(ring) == 0U && generation_drained) ? 1 : 0;
}

static int
replay_wait_for_event_boundary_drain(struct rtl_device* s) {
    if (!s || !s->input_ring || s->input_ring->capacity == 0U) {
        return 1;
    }

    struct input_ring_state* ring = s->input_ring;
    for (;;) {
        if (replay_forced_stop_requested(s)) {
            return 0;
        }

        if (replay_event_boundary_drained(s)) {
            return 1;
        }

        replay_signal_input_waiters(s);
        dsd_mutex_lock(&ring->ready_m);
        if (!replay_forced_stop_requested(s) && !replay_event_boundary_drained(s)) {
            uint64_t wait_deadline = dsd_time_monotonic_ns() + 50000000ULL;
            (void)dsd_cond_timedwait_monotonic(&ring->space, &ring->ready_m, wait_deadline);
        }
        dsd_mutex_unlock(&ring->ready_m);
    }
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" int
rtl_device_test_replay_event_boundary_drained(size_t ring_used, uint64_t submitted_gen, uint64_t consumed_gen) {
    input_ring_state ring{};
    float buffer[8] = {0};
    ring.buffer = buffer;
    ring.capacity = sizeof(buffer) / sizeof(buffer[0]);
    ring.tail.store(0, std::memory_order_relaxed);
    ring.head.store(ring_used < ring.capacity ? ring_used : (ring.capacity - 1U), std::memory_order_relaxed);

    std::atomic<uint64_t> submitted{submitted_gen};
    std::atomic<uint64_t> consumed{consumed_gen};
    rtl_device dev{};
    dev.replay_cfg.format = DSD_IQ_FORMAT_CU8;
    dev.input_ring = &ring;
    dev.replay_has_eof_state = 1;
    dev.replay_eof.replay_last_submit_gen = &submitted;
    dev.replay_eof.replay_last_consume_gen = &consumed;
    return replay_event_boundary_drained(&dev);
}
#endif

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
replay_wait_for_timeline_position(struct rtl_device* s, uint64_t complex_written, uint64_t start_ns, int realtime) {
    if (!realtime || !s || s->replay_cfg.sample_rate_hz == 0U) {
        return 1;
    }
    uint64_t deadline_ns = start_ns + ((complex_written * 1000000000ULL) / (uint64_t)s->replay_cfg.sample_rate_hz);
    return replay_wait_until(s, deadline_ns);
}

static int
replay_wait_for_enqueue_deadline(struct rtl_device* s, uint64_t complex_written, uint64_t start_ns, int realtime) {
    if (!realtime || !s || s->replay_cfg.sample_rate_hz == 0U) {
        return 1;
    }
    uint64_t deadline_ns = start_ns + ((complex_written * 1000000000ULL) / (uint64_t)s->replay_cfg.sample_rate_hz);
    return replay_wait_until(s, deadline_ns);
}

static size_t
replay_clamp_enqueue_chunk(size_t remaining_f32, size_t free_sp) {
    size_t chunk = remaining_f32;
    if (chunk > free_sp) {
        chunk = free_sp;
    }
    if (chunk & 1U) {
        chunk--;
    }
    return chunk;
}

static int replay_enqueue_f32_no_drop(struct rtl_device* s, const float* src, size_t float_count,
                                      uint64_t* complex_written, uint64_t start_ns, int realtime);
static int replay_dispatch_pending_events(struct rtl_device* s, uint32_t* event_cursor, uint64_t data_offset,
                                          int* phase, int* have_carry, uint8_t* carry_byte, uint64_t* complex_written);
static size_t replay_next_read_limit(const struct rtl_device* s, uint64_t data_offset, uint32_t event_cursor,
                                     size_t block_size);
static int replay_handle_empty_read(struct rtl_device* s, uint64_t* complex_written, uint64_t* start_ns, int realtime,
                                    int* phase, int* have_carry, uint8_t* carry_byte, uint64_t* data_offset,
                                    uint32_t* event_cursor);
static int replay_convert_block_to_f32(const struct rtl_device* s, const uint8_t* raw_block, size_t out_bytes,
                                       float* f32_block, size_t f32_cap, int* phase, int* have_carry,
                                       uint8_t* carry_byte);

static inline int
replay_submit_chunk_inputs_valid(const struct rtl_device* s, const float* src, const size_t* out_grant_f32,
                                 const uint64_t* complex_written, size_t chunk_f32) {
    if (!s || !s->input_ring) {
        return 0;
    }
    if (!src || !out_grant_f32 || !complex_written) {
        return 0;
    }
    if (chunk_f32 == 0U) {
        return 0;
    }
    return 1;
}

static inline size_t
replay_reserve_even_segments(struct input_ring_state* ring, size_t chunk_f32, float** p1, size_t* first, float** p2,
                             size_t* second) {
    if (!ring || !p1 || !first || !p2 || !second) {
        return 0U;
    }
    size_t n1 = 0U;
    size_t n2 = 0U;
    input_ring_reserve(ring, chunk_f32, p1, &n1, p2, &n2);
    rtl_even_split_ring_reserve(chunk_f32, n1, n2, first, second);
    return *first + *second;
}

static inline void
replay_record_submit_generation(struct rtl_device* s) {
    if (!s || !s->replay_has_eof_state || !s->replay_eof.replay_last_submit_gen) {
        return;
    }
    (void)s->replay_eof.replay_last_submit_gen->fetch_add(1ULL, std::memory_order_release);
}

static inline void
replay_commit_or_drop_reserved(struct input_ring_state* ring, uint64_t discard_generation, size_t grant) {
    if (!ring || grant == 0U) {
        return;
    }
    if (!input_ring_discard_generation_matches(ring, discard_generation)) {
        rtl_accumulate_ring_drops(ring, grant);
        return;
    }
    input_ring_commit(ring, grant);
}

static int
replay_reserve_and_submit_chunk(struct rtl_device* s, const float* src, size_t src_off, size_t chunk_f32,
                                size_t* out_grant_f32, uint64_t* complex_written) {
    if (!replay_submit_chunk_inputs_valid(s, src, out_grant_f32, complex_written, chunk_f32)) {
        return -1;
    }
    struct input_ring_state* ring = s->input_ring;
    uint64_t discard_generation = input_ring_discard_generation(ring);
    float *p1 = NULL, *p2 = NULL;
    size_t first = 0U;
    size_t second = 0U;
    size_t grant = replay_reserve_even_segments(ring, chunk_f32, &p1, &first, &p2, &second);
    if (grant == 0U) {
        *out_grant_f32 = 0U;
        return 0;
    }

    if (first > 0U) {
        DSD_MEMCPY(p1, src + src_off, first * sizeof(float));
    }
    if (second > 0U) {
        DSD_MEMCPY(p2, src + src_off + first, second * sizeof(float));
    }

    replay_record_submit_generation(s);
    replay_commit_or_drop_reserved(ring, discard_generation, grant);

    *out_grant_f32 = grant;
    *complex_written += grant / 2U;
    return 0;
}

namespace {

struct replay_thread_io_state {
    int* phase;
    int* have_carry;
    uint8_t* carry_byte;
    uint64_t* complex_written;
    uint64_t* data_offset;
    uint32_t* event_cursor;
    uint64_t* start_ns;
    int realtime;
};

} // namespace

static inline int
replay_thread_process_inputs_valid(const struct rtl_device* s, const uint8_t* raw_block, const float* f32_block,
                                   const replay_thread_io_state* io) {
    if (!s || !raw_block || !f32_block) {
        return 0;
    }
    if (!io || !io->phase || !io->have_carry || !io->carry_byte) {
        return 0;
    }
    if (!io->complex_written || !io->data_offset || !io->event_cursor || !io->start_ns) {
        return 0;
    }
    return 1;
}

static inline int
replay_thread_read_or_handle_empty(struct rtl_device* s, uint8_t* raw_block, size_t read_limit,
                                   replay_thread_io_state* io, size_t* out_bytes) {
    if (!s || !raw_block || !io || !io->data_offset || !io->complex_written || !io->start_ns || !io->phase
        || !io->have_carry || !io->carry_byte || !io->event_cursor || !out_bytes) {
        return 0;
    }
    *out_bytes = 0U;
    int rc = dsd_iq_replay_read(s->replay_src, raw_block, read_limit, out_bytes);
    if (rc != DSD_IQ_OK) {
        return 0;
    }
    if (*out_bytes == 0U) {
        if (!replay_handle_empty_read(s, io->complex_written, io->start_ns, io->realtime, io->phase, io->have_carry,
                                      io->carry_byte, io->data_offset, io->event_cursor)) {
            return 0;
        }
        return 2;
    }
    *io->data_offset += (uint64_t)(*out_bytes);
    return 1;
}

static int
replay_thread_process_block(struct rtl_device* s, uint8_t* raw_block, size_t raw_block_bytes, float* f32_block,
                            replay_thread_io_state* io) {
    if (!replay_thread_process_inputs_valid(s, raw_block, f32_block, io)) {
        return 0;
    }

    if (!replay_dispatch_pending_events(s, io->event_cursor, *io->data_offset, io->phase, io->have_carry,
                                        io->carry_byte, io->complex_written)) {
        return 0;
    }

    size_t read_limit = replay_next_read_limit(s, *io->data_offset, *io->event_cursor, raw_block_bytes);
    if (read_limit == 0U) {
        return 2;
    }

    size_t out_bytes = 0U;
    int read_status = replay_thread_read_or_handle_empty(s, raw_block, read_limit, io, &out_bytes);
    if (read_status == 0) {
        return 0;
    }
    if (read_status == 2) {
        return 2;
    }

    int produced = replay_convert_block_to_f32(s, raw_block, out_bytes, f32_block, raw_block_bytes, io->phase,
                                               io->have_carry, io->carry_byte);
    if (produced <= 0) {
        return 2;
    }

    if (replay_enqueue_f32_no_drop(s, f32_block, (size_t)produced, io->complex_written, *io->start_ns, io->realtime)
        != 0) {
        return 0;
    }
    return 1;
}

static int
replay_enqueue_f32_no_drop(struct rtl_device* s, const float* src, size_t float_count, uint64_t* complex_written,
                           uint64_t start_ns, int realtime) {
    if (!s || !s->input_ring || !src || !complex_written) {
        return -1;
    }

    const struct input_ring_state* ring = s->input_ring;
    size_t done = 0;
    while (done < float_count) {
        if (replay_forced_stop_requested(s)) {
            return -1;
        }

        if (!replay_wait_for_enqueue_deadline(s, *complex_written, start_ns, realtime)) {
            return -1;
        }

        size_t free_sp = input_ring_free(ring);
        if (free_sp < 2U) {
            if (!replay_wait_for_ring_progress(s, 2U, 0, 0)) {
                return -1;
            }
            continue;
        }

        size_t chunk = replay_clamp_enqueue_chunk(float_count - done, free_sp);
        if (chunk == 0U) {
            continue;
        }

        size_t grant = 0U;
        if (replay_reserve_and_submit_chunk(s, src, done, chunk, &grant, complex_written) != 0) {
            return -1;
        }
        if (grant == 0U) {
            continue;
        }
        done += grant;
    }
    return 0;
}

static int
replay_convert_cu8_to_f32(const struct rtl_device* s, const uint8_t* in, size_t in_bytes, float* out_f32,
                          size_t out_cap_f32, int* io_phase, int* io_have_carry, uint8_t* io_carry_byte) {
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
        rtl_process_u8_chunk(s, const_cast<unsigned char*>(in + consumed), out_f32 + out_pos, aligned, fs4_active,
                             combine_active, use_two_pass, io_phase);
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
replay_convert_cf32_to_f32(const struct rtl_device* s, const uint8_t* in, size_t in_bytes, float* out_f32,
                           size_t out_cap_f32, int* io_phase) {
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
            DSD_MEMCPY(&in_i, in + (i * 8U) + 0U, sizeof(float));
            DSD_MEMCPY(&in_q, in + (i * 8U) + 4U, sizeof(float));
            rtl_apply_j4_rotation(in_i, in_q, phase, &out_f32[i * 2U + 0U], &out_f32[i * 2U + 1U]);
            phase = (phase + 1) & 3;
        }
        *io_phase = phase;
    } else {
        DSD_MEMCPY(out_f32, in, float_count * sizeof(float));
    }

    return (int)float_count;
}

static void
replay_advance_omitted_cu8(const struct rtl_device* s, uint64_t duration_bytes, int* io_phase, int* io_have_carry,
                           uint8_t* io_carry_byte, uint64_t* complex_written) {
    uint64_t remaining = duration_bytes;
    if (*io_have_carry && remaining > 0U) {
        *io_have_carry = 0;
        *io_carry_byte = 0;
        remaining--;
        if (s->replay_cfg.fs4_shift_enabled) {
            *io_phase = rtl_capture_phase_advance_pairs(*io_phase & 3, 1U);
        }
        (*complex_written)++;
    }
    uint64_t pairs = remaining / 2U;
    if (pairs > 0U) {
        if (s->replay_cfg.fs4_shift_enabled) {
            *io_phase = rtl_capture_phase_advance_pairs(*io_phase & 3, (size_t)pairs);
        }
        *complex_written += pairs;
    }
    if ((remaining & 1U) != 0U) {
        *io_have_carry = 1;
        *io_carry_byte = 127U;
    }
}

static void
replay_advance_omitted_aligned(const struct rtl_device* s, uint64_t duration_bytes, int* io_phase,
                               uint64_t* complex_written) {
    size_t align = dsd_iq_sample_format_alignment_bytes(s->replay_cfg.format);
    if (align == 0U) {
        return;
    }
    uint64_t complex_samples = duration_bytes / (uint64_t)align;
    if (complex_samples == 0U) {
        return;
    }
    if (s->replay_cfg.fs4_shift_enabled) {
        *io_phase = rtl_capture_phase_advance_pairs(*io_phase & 3, (size_t)complex_samples);
    }
    *complex_written += complex_samples;
}

static void
replay_advance_omitted_bytes(const struct rtl_device* s, uint64_t duration_bytes, int* io_phase, int* io_have_carry,
                             uint8_t* io_carry_byte, uint64_t* complex_written) {
    if (!s || duration_bytes == 0U || !io_phase || !io_have_carry || !io_carry_byte || !complex_written) {
        return;
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CU8) {
        replay_advance_omitted_cu8(s, duration_bytes, io_phase, io_have_carry, io_carry_byte, complex_written);
        return;
    }
    replay_advance_omitted_aligned(s, duration_bytes, io_phase, complex_written);
}

static void
replay_reset_stream_position(const struct rtl_device* s, int* io_phase, int* io_have_carry, uint8_t* io_carry_byte) {
    if (s && s->replay_cfg.fs4_shift_enabled && io_phase) {
        *io_phase = 0;
    }
    if (io_have_carry) {
        *io_have_carry = 0;
    }
    if (io_carry_byte) {
        *io_carry_byte = 0;
    }
}

static void
replay_dispatch_event(struct rtl_device* s, const dsd_iq_event* event, int* io_phase, int* io_have_carry,
                      uint8_t* io_carry_byte, uint64_t* complex_written) {
    if (!s || !event) {
        return;
    }
    switch (event->kind) {
        case DSD_IQ_EVENT_RETUNE:
            replay_reset_stream_position(s, io_phase, io_have_carry, io_carry_byte);
            s->replay_cfg.center_frequency_hz = event->center_frequency_hz;
            s->replay_cfg.capture_center_frequency_hz = event->capture_center_frequency_hz;
            s->replay_cfg.sample_rate_hz = event->sample_rate_hz;
            s->freq = (event->capture_center_frequency_hz > UINT32_MAX) ? UINT32_MAX
                                                                        : (uint32_t)event->capture_center_frequency_hz;
            s->rate = event->sample_rate_hz;
            if (s->replay_has_eof_state && s->replay_eof.on_retune_event) {
                s->replay_eof.on_retune_event(event, s->replay_eof.event_user);
            }
            break;
        case DSD_IQ_EVENT_MUTE:
            replay_advance_omitted_bytes(s, event->duration_bytes, io_phase, io_have_carry, io_carry_byte,
                                         complex_written);
            if (s->replay_has_eof_state && s->replay_eof.on_mute_event) {
                s->replay_eof.on_mute_event(event, s->replay_eof.event_user);
            }
            break;
        case DSD_IQ_EVENT_RESET:
            replay_reset_stream_position(s, io_phase, io_have_carry, io_carry_byte);
            if (s->replay_has_eof_state && s->replay_eof.on_reset_event) {
                s->replay_eof.on_reset_event(event, s->replay_eof.event_user);
            }
            break;
        default: break;
    }
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" void
rtl_device_test_replay_dispatch_reset_event_state(int* phase, int* have_carry, uint8_t* carry_byte) {
    rtl_device dev{};
    dev.replay_cfg.format = DSD_IQ_FORMAT_CU8;
    dev.replay_cfg.fs4_shift_enabled = 1;

    dsd_iq_event event;
    DSD_MEMSET(&event, 0, sizeof(event));
    event.kind = DSD_IQ_EVENT_RESET;
    event.center_frequency_hz = 851500000ULL;
    event.capture_center_frequency_hz = 851884000ULL;
    event.sample_rate_hz = 1536000U;

    uint64_t complex_written = 0;
    replay_dispatch_event(&dev, &event, phase, have_carry, carry_byte, &complex_written);
}
#endif

static void
replay_restore_initial_state(struct rtl_device* s) {
    if (!s) {
        return;
    }
    s->replay_cfg.center_frequency_hz = s->replay_initial_center_frequency_hz;
    s->replay_cfg.capture_center_frequency_hz = s->replay_initial_capture_center_frequency_hz;
    s->replay_cfg.sample_rate_hz = s->replay_initial_sample_rate_hz;
    s->freq = s->replay_initial_freq;
    s->rate = s->replay_initial_rate;
    if (s->replay_has_eof_state && s->replay_eof.on_loop_restart) {
        s->replay_eof.on_loop_restart(&s->replay_cfg, s->replay_eof.event_user);
    }
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
replay_maybe_mark_demod_drained(struct rtl_device* s) {
    if (!s || !s->replay_has_eof_state || !s->replay_eof.replay_input_drained || !s->replay_eof.replay_demod_drained
        || !s->replay_eof.replay_last_consume_gen || !s->replay_eof.replay_last_submit_gen_at_eof) {
        return;
    }
    if (!s->replay_eof.replay_input_drained->load(std::memory_order_acquire)) {
        return;
    }
    if (s->replay_eof.replay_demod_drained->load(std::memory_order_acquire)) {
        return;
    }
    uint64_t consumed_gen = s->replay_eof.replay_last_consume_gen->load(std::memory_order_acquire);
    uint64_t eof_gen = s->replay_eof.replay_last_submit_gen_at_eof->load(std::memory_order_acquire);
    if (consumed_gen < eof_gen) {
        return;
    }
    s->replay_eof.replay_demod_drained->store(1, std::memory_order_release);
    if (s->replay_eof.eof_cond && s->replay_eof.eof_m) {
        dsd_mutex_lock(s->replay_eof.eof_m);
        dsd_cond_broadcast(s->replay_eof.eof_cond);
        dsd_mutex_unlock(s->replay_eof.eof_m);
    }
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
        replay_maybe_mark_demod_drained(s);
    }
    replay_wait_for_demod_drain(s);
}

static int
replay_dispatch_pending_events(struct rtl_device* s, uint32_t* event_cursor, uint64_t data_offset, int* phase,
                               int* have_carry, uint8_t* carry_byte, uint64_t* complex_written) {
    if (!s || !event_cursor) {
        return 0;
    }
    while (*event_cursor < s->replay_cfg.event_count
           && s->replay_cfg.events[*event_cursor].byte_offset <= data_offset) {
        const dsd_iq_event* event = &s->replay_cfg.events[*event_cursor];
        if (event->kind == DSD_IQ_EVENT_RESET && !replay_wait_for_event_boundary_drain(s)) {
            return 0;
        }
        replay_dispatch_event(s, event, phase, have_carry, carry_byte, complex_written);
        (*event_cursor)++;
    }
    return 1;
}

static size_t
replay_next_read_limit(const struct rtl_device* s, uint64_t data_offset, uint32_t event_cursor, size_t block_size) {
    if (!s) {
        return 0U;
    }
    size_t read_limit = block_size;
    if (event_cursor < s->replay_cfg.event_count && s->replay_cfg.events[event_cursor].byte_offset > data_offset) {
        uint64_t until_event = s->replay_cfg.events[event_cursor].byte_offset - data_offset;
        if (until_event < (uint64_t)read_limit) {
            read_limit = (size_t)until_event;
        }
    }
    return read_limit;
}

static int
replay_handle_empty_read(struct rtl_device* s, uint64_t* complex_written, uint64_t* start_ns, int realtime, int* phase,
                         int* have_carry, uint8_t* carry_byte, uint64_t* data_offset, uint32_t* event_cursor) {
    if (!s || !complex_written || !start_ns || !phase || !have_carry || !carry_byte || !data_offset || !event_cursor) {
        return 0;
    }
    if (!replay_wait_for_timeline_position(s, *complex_written, *start_ns, realtime)) {
        return 0;
    }
    if (!s->replay_cfg.loop) {
        replay_handle_eof_sequence(s);
        return 0;
    }
    if (dsd_iq_replay_rewind(s->replay_src) != DSD_IQ_OK) {
        return 0;
    }
    replay_restore_initial_state(s);
    *phase = 0;
    *have_carry = 0;
    *carry_byte = 0;
    *complex_written = 0;
    *data_offset = 0;
    *event_cursor = 0;
    *start_ns = dsd_time_monotonic_ns();
    return 1;
}

static int
replay_convert_block_to_f32(const struct rtl_device* s, const uint8_t* raw_block, size_t out_bytes, float* f32_block,
                            size_t f32_cap, int* phase, int* have_carry, uint8_t* carry_byte) {
    if (!s) {
        return -1;
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CU8) {
        return replay_convert_cu8_to_f32(s, raw_block, out_bytes, f32_block, f32_cap, phase, have_carry, carry_byte);
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CF32) {
        return replay_convert_cf32_to_f32(s, raw_block, out_bytes, f32_block, f32_cap, phase);
    }
    return -1;
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
    uint64_t data_offset = 0;
    uint32_t event_cursor = 0;
    uint64_t start_ns = dsd_time_monotonic_ns();
    int realtime = s->replay_cfg.realtime ? 1 : 0;

    while (!replay_forced_stop_requested(s)) {
        replay_thread_io_state io = {&phase,       &have_carry,   &carry_byte, &complex_written,
                                     &data_offset, &event_cursor, &start_ns,   realtime};
        int step = replay_thread_process_block(s, raw_block, raw_block_bytes, f32_block, &io);
        if (step == 0) {
            break;
        }
        if (step == 2) {
            continue;
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
                rtl_record_capture_mute(s, (uint64_t)len, "retune_mute");
                s->mute.fetch_sub((int)len, std::memory_order_relaxed);
                if (m == len) {
                    s->mute_byte_phase.store(0, std::memory_order_relaxed);
                    rtl_finish_capture_mute_span(s);
                }
                return; /* Nothing to process */
            }
            /* Partial mute: skip first m bytes, process remainder */
            rtl_account_fragmented_muted_u8_bytes(s, m, fs4_shift_active);
            rtl_record_capture_mute(s, (uint64_t)m, "retune_mute");
            buf += m;
            len -= m;
            s->mute.fetch_sub((int)m, std::memory_order_relaxed);
            s->mute_byte_phase.store(0, std::memory_order_relaxed);
            rtl_finish_capture_mute_span(s);
        }
    }
    if (rtl_capture_reconfigure_hold_active(s)) {
        rtl_account_fragmented_muted_u8_bytes(s, len, fs4_shift_active);
        rtl_record_capture_mute(s, (uint64_t)len, "retune_reconfigure");
        if (!rtl_capture_reconfigure_hold_accepting_discards(s)) {
            rtl_complete_fragmented_capture_discard(s);
        }
        return;
    }
    rtl_submit_capture_bytes(s, buf, len);
    rtl_publish_cu8_input_level(s, buf, len);
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
        DSD_FPRINTF(
            stderr,
            "ERROR: libusb exception in rtlsdr_read_async (MSVC/Windows). "
            "Check that the bundled libusb/librtlsdr DLLs match the build and the device driver is installed.\n");
        exitflag = 1;
    }
#else
    rtlsdr_read_async(s->dev, rtlsdr_callback, s, 16, s->buf_len);
#endif
    DSD_THREAD_RETURN;
}

static void
rtl_device_copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", src ? src : "");
    dst[dst_size - 1] = '\0';
}

static bool
rtl_soapy_config_has_field(size_t config_size, size_t field_offset, size_t field_size) {
    return field_offset <= config_size && field_size <= config_size - field_offset;
}

static bool
rtl_soapy_config_field_available(const struct rtl_soapy_config* cfg, size_t config_size, size_t field_offset,
                                 size_t field_size) {
    return cfg && rtl_soapy_config_has_field(config_size, field_offset, field_size);
}

#define RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, field)                                                      \
    rtl_soapy_config_field_available((cfg), (config_size), offsetof(struct rtl_soapy_config, field),                   \
                                     sizeof(((struct rtl_soapy_config*)0)->field))

#ifdef USE_SOAPYSDR
template <typename Fn>
static int
soapy_call_locked(struct rtl_device* dev, const char* op, Fn&& fn) {
    if (!dev || !dev->soapy_dev || !dev->soapy_lock_inited) {
        return -1;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex for %s.\n", op);
        return -1;
    }
    int ret = -1;
    try {
        ret = fn();
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: exception in %s: %s\n", op, e.what());
        ret = -1;
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);
    return ret;
}

static std::vector<dsdneo::SoapyRange>
soapy_ranges_from_range_list(const SoapySDR::RangeList& ranges) {
    std::vector<dsdneo::SoapyRange> out;
    out.reserve(ranges.size());
    std::transform(ranges.begin(), ranges.end(), std::back_inserter(out), [](const SoapySDR::Range& range) {
        return dsdneo::SoapyRange{range.minimum(), range.maximum(), range.step()};
    });
    return out;
}

static std::vector<double>
soapy_valid_positive_rates(const std::vector<double>& rates) {
    std::vector<double> out;
    out.reserve(rates.size());
    std::copy_if(rates.begin(), rates.end(), std::back_inserter(out), [](double rate) { return rate > 0.0; });
    return out;
}

static void
soapy_set_cached_format(struct rtl_device* dev, const std::string& format) {
    if (!dev) {
        return;
    }
    if (format == SOAPY_SDR_CF32) {
        dev->soapy_format = SOAPY_FMT_CF32;
    } else if (format == SOAPY_SDR_CS16) {
        dev->soapy_format = SOAPY_FMT_CS16;
    } else {
        dev->soapy_format = SOAPY_FMT_NONE;
    }
}

static void
soapy_select_profile_locked(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    dsdneo::SoapyProfileSelection selection;
    selection.requested_profile = dev->soapy_requested_profile;
    selection.driver_key = dev->soapy_driver_key;
    selection.hardware_key = dev->soapy_hardware_key;
    selection.args = dev->soapy_args_string;
    dev->soapy_profile_id = (int)dsdneo::soapy_select_profile_id(selection);
}

static void
soapy_cache_identity_locked(struct rtl_device* dev) {
    if (!dev || !dev->soapy_dev) {
        return;
    }
    try {
        rtl_device_copy_cstr(dev->soapy_driver_key, sizeof(dev->soapy_driver_key),
                             dev->soapy_dev->getDriverKey().c_str());
    } catch (const std::exception&) {
        dev->soapy_driver_key[0] = '\0';
    }
    try {
        rtl_device_copy_cstr(dev->soapy_hardware_key, sizeof(dev->soapy_hardware_key),
                             dev->soapy_dev->getHardwareKey().c_str());
    } catch (const std::exception&) {
        dev->soapy_hardware_key[0] = '\0';
    }
    try {
        double full_scale = 0.0;
        std::string native_format = dev->soapy_dev->getNativeStreamFormat(SOAPY_SDR_RX, 0, full_scale);
        rtl_device_copy_cstr(dev->soapy_native_stream_format, sizeof(dev->soapy_native_stream_format),
                             native_format.c_str());
    } catch (const std::exception&) {
        dev->soapy_native_stream_format[0] = '\0';
    }
    soapy_select_profile_locked(dev);
}

static int
soapy_refresh_stream_format_locked(struct rtl_device* dev, std::string* out_format) {
    if (!dev || !dev->soapy_dev) {
        return -1;
    }
    std::vector<std::string> formats = dev->soapy_dev->getStreamFormats(SOAPY_SDR_RX, 0);
    std::string chosen = dsdneo::soapy_choose_stream_format(formats, dev->soapy_requested_stream_format,
                                                            dev->soapy_native_stream_format);
    if (chosen.empty()) {
        DSD_FPRINTF(stderr, "SoapySDR: RX stream formats [%s] do not satisfy requested format '%s' (native=%s).\n",
                    dsdneo::soapy_join_names(formats, 160).c_str(),
                    dev->soapy_requested_stream_format[0] ? dev->soapy_requested_stream_format : "auto",
                    dev->soapy_native_stream_format[0] ? dev->soapy_native_stream_format : "-");
        dev->soapy_format = SOAPY_FMT_NONE;
        return -1;
    }
    soapy_set_cached_format(dev, chosen);
    if (out_format) {
        *out_format = chosen;
    }
    return 0;
}

static void
soapy_log_capability_summary_locked(struct rtl_device* dev) {
    if (!dev || !dev->soapy_dev) {
        return;
    }
    std::vector<std::string> formats;
    std::vector<std::string> gains;
    std::vector<std::string> antennas;
    std::vector<std::string> clocks;
    try {
        formats = dev->soapy_dev->getStreamFormats(SOAPY_SDR_RX, 0);
    } catch (const std::exception& ex) {
        (void)ex;
    }
    try {
        gains = dev->soapy_dev->listGains(SOAPY_SDR_RX, 0);
    } catch (const std::exception& ex) {
        (void)ex;
    }
    try {
        antennas = dev->soapy_dev->listAntennas(SOAPY_SDR_RX, 0);
    } catch (const std::exception& ex) {
        (void)ex;
    }
    try {
        clocks = dev->soapy_dev->listClockSources();
    } catch (const std::exception& ex) {
        (void)ex;
    }

    const dsdneo::SoapyProfile& profile = dsdneo::soapy_profile_by_id((dsdneo::SoapyProfileId)dev->soapy_profile_id);
    DSD_FPRINTF(
        stderr,
        "SoapySDR: driver=%s hardware=%s profile=%s native=%s formats=[%s] gains=[%s] antennas=[%s] clocks=[%s].\n",
        dev->soapy_driver_key[0] ? dev->soapy_driver_key : "-",
        dev->soapy_hardware_key[0] ? dev->soapy_hardware_key : "-", profile.name,
        dev->soapy_native_stream_format[0] ? dev->soapy_native_stream_format : "-",
        dsdneo::soapy_join_names(formats, 120).c_str(), dsdneo::soapy_join_names(gains, 120).c_str(),
        dsdneo::soapy_join_names(antennas, 80).c_str(), dsdneo::soapy_join_names(clocks, 80).c_str());
}

static std::string
soapy_trim_copy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && isspace((unsigned char)value[start])) {
        start++;
    }
    size_t end = value.size();
    while (end > start && isspace((unsigned char)value[end - 1])) {
        end--;
    }
    return value.substr(start, end - start);
}

static int
soapy_apply_antenna_locked(struct rtl_device* dev) {
    if (!dev || !dev->soapy_dev || dev->soapy_requested_antenna[0] == '\0') {
        return 0;
    }
    std::vector<std::string> antennas = dev->soapy_dev->listAntennas(SOAPY_SDR_RX, 0);
    if (!dsdneo::soapy_name_list_contains(antennas, dev->soapy_requested_antenna)) {
        DSD_FPRINTF(stderr, "SoapySDR: antenna '%s' is unavailable; choices=[%s].\n", dev->soapy_requested_antenna,
                    dsdneo::soapy_join_names(antennas, 160).c_str());
        return DSD_ERR_NOT_SUPPORTED;
    }
    dev->soapy_dev->setAntenna(SOAPY_SDR_RX, 0, dev->soapy_requested_antenna);
    DSD_FPRINTF(stderr, "SoapySDR: selected antenna '%s'.\n", dev->soapy_requested_antenna);
    return 0;
}

static int
soapy_apply_clock_locked(struct rtl_device* dev) {
    if (!dev || !dev->soapy_dev || dev->soapy_requested_clock[0] == '\0') {
        return 0;
    }
    std::vector<std::string> clocks = dev->soapy_dev->listClockSources();
    if (!dsdneo::soapy_name_list_contains(clocks, dev->soapy_requested_clock)) {
        DSD_FPRINTF(stderr, "SoapySDR: clock source '%s' is unavailable; choices=[%s].\n", dev->soapy_requested_clock,
                    dsdneo::soapy_join_names(clocks, 160).c_str());
        return DSD_ERR_NOT_SUPPORTED;
    }
    dev->soapy_dev->setClockSource(dev->soapy_requested_clock);
    DSD_FPRINTF(stderr, "SoapySDR: selected clock source '%s'.\n", dev->soapy_requested_clock);
    return 0;
}

static std::vector<std::string>
soapy_setting_keys_from_info(const SoapySDR::ArgInfoList& infos) {
    std::vector<std::string> keys;
    keys.reserve(infos.size());
    for (const SoapySDR::ArgInfo& info : infos) {
        if (!info.key.empty()) {
            keys.push_back(info.key);
        }
    }
    return keys;
}

static const SoapySDR::ArgInfo*
soapy_find_setting_info(const SoapySDR::ArgInfoList& infos, const std::string& key) {
    SoapySDR::ArgInfoList::const_iterator it =
        std::find_if(infos.begin(), infos.end(), [&key](const SoapySDR::ArgInfo& info) { return info.key == key; });
    return it != infos.end() ? &(*it) : NULL;
}

static bool
soapy_setting_option_allowed(const SoapySDR::ArgInfo& info, const std::string& value) {
    return std::find(info.options.begin(), info.options.end(), value) != info.options.end();
}

static dsdneo::SoapySettingValueType
soapy_setting_value_type_from_arg_info(const SoapySDR::ArgInfo& info) {
    switch (info.type) {
        case SoapySDR::ArgInfo::BOOL: return dsdneo::SoapySettingValueType::Bool;
        case SoapySDR::ArgInfo::INT: return dsdneo::SoapySettingValueType::Int;
        case SoapySDR::ArgInfo::FLOAT: return dsdneo::SoapySettingValueType::Float;
        case SoapySDR::ArgInfo::STRING:
        default: return dsdneo::SoapySettingValueType::String;
    }
}

static bool
soapy_arg_info_has_numeric_range(const SoapySDR::ArgInfo& info) {
    if (info.type != SoapySDR::ArgInfo::INT && info.type != SoapySDR::ArgInfo::FLOAT) {
        return false;
    }
    const double minimum = info.range.minimum();
    const double maximum = info.range.maximum();
    const double step = info.range.step();
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum < minimum) {
        return false;
    }
    /* Soapy's default empty Range is 0..0; no separate presence flag is exposed. */
    return minimum != 0.0 || maximum != 0.0 || step != 0.0;
}

static dsdneo::SoapyRange
soapy_range_from_arg_info(const SoapySDR::ArgInfo& info) {
    return {info.range.minimum(), info.range.maximum(), info.range.step()};
}

static bool
soapy_get_setting_info_locked(struct rtl_device* dev, dsdneo::SoapySettingScope scope,
                              SoapySDR::ArgInfoList* out_infos) {
    if (out_infos) {
        out_infos->clear();
    }
    if (!dev || !dev->soapy_dev || !out_infos) {
        return false;
    }
    try {
        if (scope == dsdneo::SoapySettingScope::Rx0) {
            *out_infos = dev->soapy_dev->getSettingInfo(SOAPY_SDR_RX, 0);
        } else {
            *out_infos = dev->soapy_dev->getSettingInfo();
        }
        return true;
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: could not query %s settings metadata: %s\n",
                    dsdneo::soapy_setting_scope_name(scope), e.what());
        return false;
    }
}

static int
soapy_validate_setting_request_locked(const dsdneo::SoapySettingRequest& request, const SoapySDR::ArgInfoList& infos) {
    if (infos.empty()) {
        return 0;
    }

    const SoapySDR::ArgInfo* info = soapy_find_setting_info(infos, request.key);
    if (!info) {
        std::vector<std::string> keys = soapy_setting_keys_from_info(infos);
        DSD_FPRINTF(stderr, "SoapySDR: setting '%s' is unavailable for %s scope; choices=[%s].\n", request.key.c_str(),
                    dsdneo::soapy_setting_scope_name(request.scope), dsdneo::soapy_join_names(keys, 200).c_str());
        return DSD_ERR_NOT_SUPPORTED;
    }

    if (!info->options.empty() && !soapy_setting_option_allowed(*info, request.value)) {
        DSD_FPRINTF(stderr, "SoapySDR: setting '%s' value is invalid for %s scope; choices=[%s].\n",
                    request.key.c_str(), dsdneo::soapy_setting_scope_name(request.scope),
                    dsdneo::soapy_join_names(info->options, 200).c_str());
        return DSD_ERR_NOT_SUPPORTED;
    }
    if (info->options.empty()) {
        const dsdneo::SoapyRange range = soapy_range_from_arg_info(*info);
        const dsdneo::SoapyRange* range_ptr = soapy_arg_info_has_numeric_range(*info) ? &range : NULL;
        std::string error;
        if (!dsdneo::soapy_validate_setting_value(soapy_setting_value_type_from_arg_info(*info), range_ptr,
                                                  request.value, &error)) {
            DSD_FPRINTF(stderr, "SoapySDR: setting '%s' value is invalid for %s scope; %s.\n", request.key.c_str(),
                        dsdneo::soapy_setting_scope_name(request.scope), error.c_str());
            return DSD_ERR_NOT_SUPPORTED;
        }
    }

    return 0;
}

static int
soapy_write_setting_locked(struct rtl_device* dev, const dsdneo::SoapySettingRequest& request,
                           const SoapySDR::ArgInfoList& infos) {
    try {
        if (request.scope == dsdneo::SoapySettingScope::Rx0) {
            dev->soapy_dev->writeSetting(SOAPY_SDR_RX, 0, request.key, request.value);
        } else {
            dev->soapy_dev->writeSetting(request.key, request.value);
        }
        return 0;
    } catch (const std::exception& e) {
        std::vector<std::string> keys = soapy_setting_keys_from_info(infos);
        DSD_FPRINTF(stderr, "SoapySDR: failed to write setting '%s' for %s scope: %s", request.key.c_str(),
                    dsdneo::soapy_setting_scope_name(request.scope), e.what());
        if (!keys.empty()) {
            DSD_FPRINTF(stderr, "; choices=[%s]", dsdneo::soapy_join_names(keys, 200).c_str());
        }
        DSD_FPRINTF(stderr, ".\n");
        return -1;
    }
}

static int
soapy_apply_settings_locked(struct rtl_device* dev) {
    if (!dev || !dev->soapy_dev || dev->soapy_requested_settings[0] == '\0') {
        return 0;
    }

    std::vector<dsdneo::SoapySettingRequest> requests;
    std::string error;
    if (!dsdneo::soapy_parse_settings(dev->soapy_requested_settings, &requests, &error)) {
        DSD_FPRINTF(stderr, "SoapySDR: invalid soapy_settings: %s.\n", error.c_str());
        return -1;
    }

    int applied_count = 0;
    for (const dsdneo::SoapySettingRequest& request : requests) {
        SoapySDR::ArgInfoList infos;
        (void)soapy_get_setting_info_locked(dev, request.scope, &infos);
        int rc = soapy_validate_setting_request_locked(request, infos);
        if (rc != 0) {
            return rc;
        }
        rc = soapy_write_setting_locked(dev, request, infos);
        if (rc != 0) {
            return rc;
        }
        applied_count++;
    }

    if (applied_count > 0) {
        DSD_FPRINTF(stderr, "SoapySDR: applied %d driver setting%s.\n", applied_count, applied_count == 1 ? "" : "s");
    }
    return 0;
}

static int
soapy_parse_named_gain_item(const std::string& item, std::string* out_name, std::string* out_value) {
    if (!out_name || !out_value) {
        return -1;
    }
    size_t colon = item.find(':');
    if (colon == std::string::npos) {
        DSD_FPRINTF(stderr, "SoapySDR: named gain '%s' must use NAME:dB syntax.\n", item.c_str());
        return -1;
    }
    *out_name = soapy_trim_copy(item.substr(0, colon));
    *out_value = soapy_trim_copy(item.substr(colon + 1));
    if (out_name->empty() || out_value->empty()) {
        DSD_FPRINTF(stderr, "SoapySDR: named gain '%s' must use NAME:dB syntax.\n", item.c_str());
        return -1;
    }
    return 0;
}

static int
soapy_apply_named_gain_item_locked(struct rtl_device* dev, const std::vector<std::string>& available,
                                   const std::string& item) {
    std::string name;
    std::string value;
    if (soapy_parse_named_gain_item(item, &name, &value) != 0) {
        return -1;
    }
    if (!dsdneo::soapy_name_list_contains(available, name)) {
        DSD_FPRINTF(stderr, "SoapySDR: gain stage '%s' is unavailable; choices=[%s].\n", name.c_str(),
                    dsdneo::soapy_join_names(available, 160).c_str());
        return DSD_ERR_NOT_SUPPORTED;
    }

    char* end = NULL;
    double requested_db = strtod(value.c_str(), &end);
    if (end == value.c_str()) {
        DSD_FPRINTF(stderr, "SoapySDR: invalid gain value '%s' for stage '%s'.\n", value.c_str(), name.c_str());
        return -1;
    }
    bool supported = false;
    SoapySDR::Range range = dev->soapy_dev->getGainRange(SOAPY_SDR_RX, 0, name);
    double applied_db =
        dsdneo::soapy_nearest_in_ranges(requested_db, {{range.minimum(), range.maximum(), range.step()}}, &supported);
    if (!supported) {
        applied_db = requested_db;
    }
    if (std::fabs(applied_db - requested_db) > 0.05) {
        DSD_FPRINTF(stderr, "SoapySDR: adjusted gain %s from %.2f dB to %.2f dB.\n", name.c_str(), requested_db,
                    applied_db);
    }
    if (dev->soapy_dev->hasGainMode(SOAPY_SDR_RX, 0)) {
        dev->soapy_dev->setGainMode(SOAPY_SDR_RX, 0, false);
    }
    dev->soapy_dev->setGain(SOAPY_SDR_RX, 0, name, applied_db);
    return 0;
}

static int
soapy_apply_named_gains_locked(struct rtl_device* dev) {
    if (!dev || !dev->soapy_dev || dev->soapy_requested_gains[0] == '\0') {
        return 0;
    }

    std::vector<std::string> available = dev->soapy_dev->listGains(SOAPY_SDR_RX, 0);
    std::string spec = dev->soapy_requested_gains;
    size_t pos = 0;
    int applied_count = 0;
    while (pos < spec.size()) {
        size_t next = spec.find_first_of(",; ", pos);
        std::string item =
            soapy_trim_copy(spec.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        pos = (next == std::string::npos) ? spec.size() : next + 1;
        if (item.empty()) {
            continue;
        }
        int rc = soapy_apply_named_gain_item_locked(dev, available, item);
        if (rc != 0) {
            return rc;
        }
        applied_count++;
    }

    if (applied_count > 0) {
        dev->soapy_named_gain_override = 1;
        dev->agc_mode = 0;
        DSD_FPRINTF(stderr, "SoapySDR: applied named gain profile '%s'.\n", dev->soapy_requested_gains);
    }
    return 0;
}
#endif

static void
soapy_stream_cleanup(struct rtl_device* dev, int unmake_device) {
#ifdef USE_SOAPYSDR
    if (!dev || !dev->soapy_lock_inited) {
        return;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex for cleanup.\n");
        return;
    }
    if (dev->soapy_dev && dev->soapy_stream) {
        try {
            int rc = dev->soapy_dev->deactivateStream(dev->soapy_stream, 0, 0);
            if (rc < 0 && rc != SOAPY_SDR_NOT_SUPPORTED) {
                DSD_FPRINTF(stderr, "SoapySDR: deactivateStream failed: %s (%d).\n", SoapySDR::errToStr(rc), rc);
            }
        } catch (const std::exception& e) {
            DSD_FPRINTF(stderr, "SoapySDR: exception in deactivateStream: %s\n", e.what());
        }
        try {
            dev->soapy_dev->closeStream(dev->soapy_stream);
        } catch (const std::exception& e) {
            DSD_FPRINTF(stderr, "SoapySDR: exception in closeStream: %s\n", e.what());
        }
        dev->soapy_stream = NULL;
    }
    if (unmake_device && dev->soapy_dev) {
        try {
            SoapySDR::Device::unmake(dev->soapy_dev);
        } catch (const std::exception& e) {
            DSD_FPRINTF(stderr, "SoapySDR: exception in Device::unmake: %s\n", e.what());
        }
        dev->soapy_dev = NULL;
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);
#else
    (void)dev;
    (void)unmake_device;
#endif
}

#ifdef USE_SOAPYSDR
static uint64_t
soapy_capture_event_bytes(const struct rtl_device* dev, size_t elements) {
    if (!dev || elements == 0U) {
        return 0U;
    }
    if (dev->soapy_format == SOAPY_FMT_CF32) {
        return (uint64_t)elements * (uint64_t)sizeof(std::complex<float>);
    }
    if (dev->soapy_format == SOAPY_FMT_CS16) {
        return (uint64_t)elements * 2ULL * (uint64_t)sizeof(int16_t);
    }
    return 0U;
}

static int
soapy_setup_stream(struct rtl_device* dev, size_t* io_mtu_elems) {
    if (!dev || !io_mtu_elems) {
        return -1;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex for stream setup.\n");
        return -1;
    }

    int fatal = 0;
    std::string stream_format;
    try {
        if (soapy_refresh_stream_format_locked(dev, &stream_format) != 0) {
            fatal = 1;
        }
        if (!fatal) {
            std::vector<size_t> channels(1, 0);
            SoapySDR::Kwargs args;
            dev->soapy_stream = dev->soapy_dev->setupStream(SOAPY_SDR_RX, stream_format, channels, args);
            if (!dev->soapy_stream) {
                DSD_FPRINTF(stderr, "SoapySDR: setupStream returned null.\n");
                fatal = 1;
            }
        }
        if (!fatal && dev->soapy_stream) {
            size_t mtu = dev->soapy_dev->getStreamMTU(dev->soapy_stream);
            if (mtu > 0U) {
                *io_mtu_elems = mtu;
            }
            dev->soapy_mtu_elems = (uint32_t)(*io_mtu_elems);
        }
        if (!fatal && dev->soapy_stream) {
            int rc = dev->soapy_dev->activateStream(dev->soapy_stream, 0, 0, 0);
            if (rc < 0) {
                DSD_FPRINTF(stderr, "SoapySDR: activateStream failed: %s (%d).\n", SoapySDR::errToStr(rc), rc);
                fatal = 1;
            }
        }
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: exception during stream setup: %s\n", e.what());
        fatal = 1;
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);
    return fatal ? -1 : 0;
}

static int
soapy_allocate_thread_buffers(const struct rtl_device* dev, size_t mtu_elems,
                              std::vector<std::complex<float>>* cf32_buf, std::vector<int16_t>* cs16_buf) {
    if (!dev || !cf32_buf || !cs16_buf) {
        return -1;
    }
    try {
        if (dev->soapy_format == SOAPY_FMT_CF32) {
            cf32_buf->resize(mtu_elems);
            return 0;
        }
        if (dev->soapy_format == SOAPY_FMT_CS16) {
            cs16_buf->resize(mtu_elems * 2U);
            return 0;
        }
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: buffer allocation exception: %s\n", e.what());
        return -1;
    }
    return -1;
}

static int
soapy_prepare_read_buffer(const struct rtl_device* dev, std::vector<std::complex<float>>* cf32_buf,
                          std::vector<int16_t>* cs16_buf, void* buffs[1]) {
    if (!dev || !cf32_buf || !cs16_buf || !buffs) {
        return -1;
    }
    buffs[0] = NULL;
    if (dev->soapy_format == SOAPY_FMT_CF32) {
        buffs[0] = static_cast<void*>(cf32_buf->data());
        return 0;
    }
    if (dev->soapy_format == SOAPY_FMT_CS16) {
        buffs[0] = static_cast<void*>(cs16_buf->data());
        return 0;
    }
    return -1;
}

static int
soapy_read_stream_locked(struct rtl_device* dev, void* buffs[1], size_t mtu_elems) {
    if (!dev || !buffs) {
        return INT_MIN;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex for readStream.\n");
        return INT_MIN;
    }
    int ret = INT_MIN;
    try {
        int flags = 0;
        long long time_ns = 0;
        ret = dev->soapy_dev->readStream(dev->soapy_stream, buffs, mtu_elems, flags, time_ns, 15000);
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: exception in readStream: %s\n", e.what());
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);
    return ret;
}

static int
soapy_handle_read_result(struct rtl_device* dev, int ret) {
    if (!dev) {
        return -1;
    }
    if (ret == SOAPY_SDR_TIMEOUT) {
        dev->soapy_timeout_count++;
        return 1;
    }
    if (ret == SOAPY_SDR_OVERFLOW) {
        dev->soapy_overflow_count++;
        uint64_t now_ns = dsd_time_monotonic_ns();
        if ((now_ns - dev->soapy_last_overflow_log_ns) > 1000000000ULL) {
            DSD_FPRINTF(stderr, "SoapySDR: RX overflow count=%llu.\n", (unsigned long long)dev->soapy_overflow_count);
            dev->soapy_last_overflow_log_ns = now_ns;
        }
        return 1;
    }
    if (ret < 0) {
        dev->soapy_read_errors++;
        DSD_FPRINTF(stderr, "SoapySDR: readStream failed: %s (%d).\n", SoapySDR::errToStr(ret), ret);
        return -1;
    }
    if (ret == 0) {
        return 1;
    }
    return 0;
}

static int
soapy_consume_mute_prefix(struct rtl_device* dev, size_t frame_elems, int apply_rot, size_t* out_drop_elems) {
    if (!dev || !out_drop_elems) {
        return -1;
    }
    *out_drop_elems = 0U;
    int mute_bytes = dev->mute.load(std::memory_order_relaxed);
    if (mute_bytes <= 0) {
        return 0;
    }

    size_t mute_elems = ((size_t)mute_bytes + 1U) / 2U;
    size_t drop_elems = (mute_elems < frame_elems) ? mute_elems : frame_elems;
    int consumed_mute_bytes = (drop_elems > (size_t)(INT_MAX / 2)) ? INT_MAX : (int)(drop_elems * 2U);
    if (consumed_mute_bytes > mute_bytes) {
        consumed_mute_bytes = mute_bytes;
    }
    if (apply_rot && drop_elems > 0U) {
        dev->rot_phase = rtl_capture_phase_advance_pairs(dev->rot_phase & 3, drop_elems);
    }
    if (drop_elems > 0U) {
        rtl_record_capture_mute(dev, soapy_capture_event_bytes(dev, drop_elems), "retune_mute");
    }
    dev->mute.fetch_sub(consumed_mute_bytes, std::memory_order_relaxed);
    if (consumed_mute_bytes >= mute_bytes) {
        rtl_finish_capture_mute_span(dev);
    }
    *out_drop_elems = drop_elems;
    return (drop_elems >= frame_elems) ? 1 : 0;
}

static int
soapy_handle_reconfigure_hold(struct rtl_device* dev, size_t kept_elems, int apply_rot) {
    if (!rtl_capture_reconfigure_hold_active(dev)) {
        return 0;
    }
    if (apply_rot && kept_elems > 0U) {
        dev->rot_phase = rtl_capture_phase_advance_pairs(dev->rot_phase & 3, kept_elems);
    }
    if (kept_elems > 0U) {
        rtl_record_capture_mute(dev, soapy_capture_event_bytes(dev, kept_elems), "retune_reconfigure");
    }
    return 1;
}

static void
soapy_submit_samples(struct rtl_device* dev, size_t drop_elems, size_t kept_elems,
                     const std::vector<std::complex<float>>& cf32_buf, const std::vector<int16_t>& cs16_buf,
                     int apply_rot) {
    if (!dev || kept_elems == 0U) {
        return;
    }
    if (dev->soapy_format == SOAPY_FMT_CF32) {
        const std::complex<float>* src = cf32_buf.data() + drop_elems;
        rtl_submit_capture_bytes(dev, src, kept_elems * sizeof(std::complex<float>));
        rtl_publish_cf32_input_level(reinterpret_cast<const float*>(src), kept_elems * 2U);
        (void)soapy_write_cf32_to_ring(dev, src, kept_elems, apply_rot);
        return;
    }
    const int16_t* src = cs16_buf.data() + (drop_elems * 2U);
    rtl_submit_capture_bytes(dev, src, kept_elems * 2U * sizeof(int16_t));
    rtl_publish_cs16_input_level(src, kept_elems * 2U);
    (void)soapy_write_cs16_to_ring(dev, src, kept_elems, apply_rot);
}
#endif

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

    size_t mtu_elems = 16384;
    if (soapy_setup_stream(s, &mtu_elems) != 0) {
        soapy_stream_cleanup(s, 1);
        s->run.store(0);
        DSD_THREAD_RETURN;
    }

    std::vector<std::complex<float>> cf32_buf;
    std::vector<int16_t> cs16_buf;
    if (soapy_allocate_thread_buffers(s, mtu_elems, &cf32_buf, &cs16_buf) != 0) {
        soapy_stream_cleanup(s, 1);
        s->run.store(0);
        DSD_THREAD_RETURN;
    }

    int fatal = 0;
    while (s->run.load() && exitflag == 0) {
        void* buffs[1] = {NULL};
        if (soapy_prepare_read_buffer(s, &cf32_buf, &cs16_buf, buffs) != 0) {
            fatal = 1;
            break;
        }

        int ret = soapy_read_stream_locked(s, buffs, mtu_elems);
        if (ret == INT_MIN) {
            fatal = 1;
            break;
        }
        int read_status = soapy_handle_read_result(s, ret);
        if (read_status > 0) {
            continue;
        }
        if (read_status < 0) {
            fatal = 1;
            break;
        }

        const int apply_rot = fs4_shift_capture_active(s);
        size_t frame_elems = (size_t)ret;
        size_t drop_elems = 0U;
        int mute_status = soapy_consume_mute_prefix(s, frame_elems, apply_rot, &drop_elems);
        if (mute_status < 0) {
            fatal = 1;
            break;
        }
        if (mute_status > 0) {
            continue;
        }

        size_t kept_elems = frame_elems - drop_elems;
        if (soapy_handle_reconfigure_hold(s, kept_elems, apply_rot)) {
            continue;
        }
        soapy_submit_samples(s, drop_elems, kept_elems, cf32_buf, cs16_buf, apply_rot);
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
        DSD_FPRINTF(stderr, "rtl_tcp: ERROR opening socket\n");
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
        DSD_FPRINTF(stderr, "rtl_tcp: ERROR, no such host as %s\n", host);
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }
    if (dsd_socket_connect(sockfd, reinterpret_cast<const struct sockaddr*>(&serveraddr), sizeof(serveraddr)) != 0) {
        DSD_FPRINTF(stderr, "rtl_tcp: ERROR connecting to %s:%d\n", host, port);
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

namespace {

struct rtl_tcp_loop_state {
    size_t bufsz;
    int waitall;
    int timeout_limit;
    int consec_timeouts;
    uint64_t prev_drops;
    uint64_t prev_rdto;
    uint64_t prev_res_full;
    uint64_t auto_last_ns;
    unsigned char* u8;
};

struct rtl_tcp_reconnect_snapshot {
    uint32_t freq;
    uint32_t rate;
    int gain;
    int agc;
    int ppm;
    int direct_sampling;
    int bias_tee_on;
    int offset_tuning;
    int testmode_on;
    uint32_t rtl_xtal_hz;
    uint32_t tuner_xtal_hz;
};

struct rtl_u8_transform_policy_state {
    int fs4_shift_active;
    int combine_rotate_active;
    int use_two_pass;
};

} // namespace

static inline size_t
rtl_tcp_round_up_page(size_t n) {
    return (n + 4095U) & ~((size_t)4095U);
}

static size_t
rtl_tcp_default_bufsz(const struct rtl_device* s) {
    if (!s) {
        return 65536U;
    }
    if (s->backend == RTL_BACKEND_TCP) {
        return 16384U;
    }
    if (s->rate == 0U) {
        return 65536U;
    }
    double bytes_per_sec = (double)s->rate * 2.0;
    size_t target = (size_t)(bytes_per_sec * 0.02);
    if (target < 16384U) {
        target = 16384U;
    }
    if (target > 262144U) {
        target = 262144U;
    }
    return target;
}

static inline size_t
rtl_tcp_cfg_bufsz(const struct rtl_device* s, const dsdneoRuntimeConfig* cfg) {
    size_t bufsz = rtl_tcp_default_bufsz(s);
    if (!cfg) {
        return bufsz;
    }
    if (!cfg->tcp_bufsz_is_set || cfg->tcp_bufsz_bytes <= 0) {
        return bufsz;
    }
    return (size_t)cfg->tcp_bufsz_bytes;
}

static inline int
rtl_tcp_cfg_waitall(const struct rtl_device* s, const dsdneoRuntimeConfig* cfg) {
    int waitall = (s && s->backend == RTL_BACKEND_TCP) ? 0 : 1;
    if (!cfg || !cfg->tcp_waitall_is_set) {
        return waitall;
    }
    return cfg->tcp_waitall_enable ? 1 : 0;
}

static inline void
rtl_tcp_maybe_enable_autotune(struct rtl_device* s, const dsdneoRuntimeConfig* cfg) {
    if (!s || s->tcp_autotune || !cfg || !cfg->tcp_autotune_enable) {
        return;
    }
    s->tcp_autotune = 1;
}

static inline void
rtl_tcp_init_adaptive_state(struct rtl_device* s, const dsdneoRuntimeConfig* cfg, struct rtl_tcp_loop_state* st) {
    if (!s || !st) {
        return;
    }
    st->prev_drops = s->input_ring ? s->input_ring->producer_drops.load() : 0ULL;
    st->prev_rdto = s->input_ring ? s->input_ring->read_timeouts.load() : 0ULL;
    st->prev_res_full = s->reserve_full_events;
    st->auto_last_ns = dsd_time_monotonic_ns();
    st->timeout_limit = cfg ? cfg->tcp_max_timeouts : 3;
    st->consec_timeouts = 0;
}

static int
rtl_tcp_init_loop_state(struct rtl_device* s, const dsdneoRuntimeConfig* cfg, struct rtl_tcp_loop_state* st) {
    if (!s || !st) {
        return 0;
    }
    DSD_MEMSET(st, 0, sizeof(*st));
    st->bufsz = rtl_tcp_cfg_bufsz(s, cfg);
    st->u8 = static_cast<unsigned char*>(malloc(st->bufsz));
    if (!st->u8) {
        return 0;
    }
    rtl_tcp_skip_header(s->sockfd);
    st->waitall = rtl_tcp_cfg_waitall(s, cfg);
    rtl_tcp_maybe_enable_autotune(s, cfg);
    rtl_tcp_init_adaptive_state(s, cfg, st);
    return 1;
}

static inline void
rtl_tcp_apply_backpressure_if_needed(const struct rtl_device* s) {
    if (!s || !s->input_ring || !s->tcp_autotune) {
        return;
    }
    const size_t slice = (s->buf_len > 0 ? (size_t)s->buf_len : 16384U);
    if (input_ring_free(s->input_ring) < (slice * 2U)) {
        dsd_sleep_us(500U);
    }
}

static inline void
rtl_tcp_close_socket_if_open(struct rtl_device* s) {
    if (!s || s->sockfd == DSD_INVALID_SOCKET) {
        return;
    }
    dsd_socket_shutdown(s->sockfd, SHUT_RDWR);
    dsd_socket_close(s->sockfd);
    s->sockfd = DSD_INVALID_SOCKET;
}

static void
rtl_tcp_capture_reconnect_snapshot(const struct rtl_device* s, struct rtl_tcp_reconnect_snapshot* snap) {
    if (!s || !snap) {
        return;
    }
    snap->freq = s->freq;
    snap->rate = s->rate;
    snap->gain = s->gain;
    snap->agc = s->agc_mode;
    snap->ppm = s->ppm_error;
    snap->direct_sampling = s->direct_sampling;
    snap->bias_tee_on = s->bias_tee_on;
    snap->offset_tuning = s->offset_tuning;
    snap->testmode_on = s->testmode_on;
    snap->rtl_xtal_hz = s->rtl_xtal_hz;
    snap->tuner_xtal_hz = s->tuner_xtal_hz;
}

static void
rtl_tcp_apply_post_reconnect_socket_options(dsd_socket_t sockfd) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int rcvbuf = cfg ? cfg->tcp_rcvbuf_bytes : (4 * 1024 * 1024);
    (void)dsd_socket_setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int nodelay = 1;
    (void)dsd_socket_setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int to_ms = cfg ? cfg->tcp_rcvtimeo_ms : 2000;
    (void)dsd_socket_set_recv_timeout(sockfd, (unsigned int)to_ms);
}

static inline void
rtl_tcp_send_cmd_if_nonzero(dsd_socket_t sockfd, uint8_t cmd, uint32_t value) {
    if (value == 0U) {
        return;
    }
    (void)rtl_tcp_send_cmd(sockfd, cmd, value);
}

static inline void
rtl_tcp_send_cmd_if_enabled(dsd_socket_t sockfd, uint8_t cmd, int enabled) {
    if (!enabled) {
        return;
    }
    (void)rtl_tcp_send_cmd(sockfd, cmd, 1U);
}

static void
rtl_tcp_replay_reconnect_state(struct rtl_device* s, const struct rtl_tcp_reconnect_snapshot* snap) {
    if (!s || !snap) {
        return;
    }
    rtl_tcp_send_cmd_if_nonzero(s->sockfd, 0x01, snap->freq);
    rtl_tcp_send_cmd_if_nonzero(s->sockfd, 0x02, snap->rate);
    if (snap->agc) {
        (void)rtl_tcp_send_cmd(s->sockfd, 0x03, 0U);
        (void)rtl_tcp_send_cmd(s->sockfd, 0x08, (uint32_t)env_agc_want());
    } else {
        (void)rtl_tcp_send_cmd(s->sockfd, 0x03, 1U);
        (void)rtl_tcp_send_cmd(s->sockfd, 0x04, (uint32_t)snap->gain);
    }
    if (snap->ppm != 0) {
        (void)rtl_tcp_send_cmd(s->sockfd, 0x05, (uint32_t)snap->ppm);
    }
    if (snap->direct_sampling != 0) {
        (void)rtl_tcp_send_cmd(s->sockfd, 0x09, (uint32_t)snap->direct_sampling);
    }
    rtl_tcp_send_cmd_if_enabled(s->sockfd, 0x0A, snap->offset_tuning);
    rtl_tcp_send_cmd_if_enabled(s->sockfd, 0x0E, snap->bias_tee_on);
    if (snap->testmode_on != 0) {
        (void)rtl_tcp_send_cmd(s->sockfd, 0x07, (uint32_t)snap->testmode_on);
    }
    rtl_tcp_send_cmd_if_nonzero(s->sockfd, 0x0B, snap->rtl_xtal_hz);
    rtl_tcp_send_cmd_if_nonzero(s->sockfd, 0x0C, snap->tuner_xtal_hz);
    if (s->if_gain_count > 0) {
        for (int i = 0; i < s->if_gain_count && i < 16; i++) {
            uint32_t packed =
                ((uint32_t)(s->if_gains[i].stage & 0xFFFF) << 16) | ((uint16_t)(s->if_gains[i].gain & 0xFFFF));
            (void)rtl_tcp_send_cmd(s->sockfd, 0x06, packed);
        }
    }
}

static int
rtl_tcp_try_reconnect_once(struct rtl_device* s, const struct rtl_tcp_reconnect_snapshot* snap,
                           struct rtl_tcp_loop_state* st, int attempt, int* out_r) {
    if (!s || !snap || !st || !out_r) {
        return 0;
    }
    dsd_socket_t newsfd = tcp_connect_host(s->host, s->port);
    if (newsfd == DSD_INVALID_SOCKET) {
        return 0;
    }
    s->sockfd = newsfd;
    DSD_FPRINTF(stderr, "rtl_tcp: reconnected on attempt %d.\n", attempt);
    rtl_tcp_skip_header(s->sockfd);
    rtl_reset_capture_state_on_stream_boundary(s);
    rtl_tcp_metrics_reset_device(s, s->rate);
    rtl_tcp_apply_post_reconnect_socket_options(s->sockfd);
    rtl_tcp_replay_reconnect_state(s, snap);

    int r = dsd_socket_recv(s->sockfd, st->u8, st->bufsz, st->waitall ? MSG_WAITALL : 0);
    if (r > 0) {
        *out_r = r;
        return 1;
    }
    rtl_tcp_close_socket_if_open(s);
    return 0;
}

static int
rtl_tcp_reconnect_after_stall(struct rtl_device* s, struct rtl_tcp_loop_state* st, int* out_r) {
    if (!s || !st || !out_r) {
        return 0;
    }
    DSD_FPRINTF(stderr, "rtl_tcp: input stalled; attempting to reconnect to %s:%d...\n", s->host, s->port);
    struct rtl_tcp_reconnect_snapshot snap{};
    rtl_tcp_capture_reconnect_snapshot(s, &snap);
    rtl_tcp_close_socket_if_open(s);

    int attempt = 0;
    while (s->run.load() && exitflag == 0) {
        attempt++;
        if (rtl_tcp_try_reconnect_once(s, &snap, st, attempt, out_r)) {
            return 1;
        }
        int backoff_ms = 200 * (attempt < 10 ? attempt : 10);
        dsd_sleep_ms((unsigned int)backoff_ms);
    }
    return 0;
}

static int
rtl_tcp_recv_is_timeout(int r) {
#if DSD_PLATFORM_WIN_NATIVE
    int e = dsd_socket_get_error();
    return (r < 0) && (e == WSAEWOULDBLOCK || e == WSAETIMEDOUT || e == WSAEINTR);
#else
    int e = dsd_socket_get_error();
    return (r < 0) && (e == EAGAIN || ((EWOULDBLOCK != EAGAIN) && (e == EWOULDBLOCK)) || e == EINTR);
#endif
}

static int
rtl_tcp_handle_recv_failure(struct rtl_device* s, struct rtl_tcp_loop_state* st, int r, int* out_r) {
    if (!s || !st || !out_r) {
        return 0;
    }
    if (!s->run.load() || exitflag) {
        return 0;
    }
    if (rtl_tcp_recv_is_timeout(r)) {
        st->consec_timeouts++;
        if (st->consec_timeouts < st->timeout_limit) {
            return 2;
        }
    }
    if (!rtl_tcp_reconnect_after_stall(s, st, out_r)) {
        return 0;
    }
    return 1;
}

static int
rtl_tcp_watchdog_allows_processing(struct rtl_device* s, int r) {
    if (!s) {
        return 0;
    }
    uint64_t recv_ns = dsd_time_monotonic_ns();
    if (!rtl_tcp_metrics_record_recv_device(s, (uint32_t)r, recv_ns)) {
        return 1;
    }
    DSD_FPRINTF(stderr, "rtl_tcp: throughput watchdog triggered; reconnecting to %s:%d...\n", s->host, s->port);
    rtl_tcp_close_socket_if_open(s);
    rtl_tcp_metrics_reset_device(s, s->rate);
    return 0;
}

static inline void
rtl_tcp_load_transform_policy(const struct rtl_device* s, struct rtl_u8_transform_policy_state* policy) {
    if (!policy) {
        return;
    }
    policy->fs4_shift_active = 0;
    policy->combine_rotate_active = 0;
    policy->use_two_pass = 0;
    rtl_get_u8_transform_policy(s, &policy->fs4_shift_active, &policy->combine_rotate_active, &policy->use_two_pass);
}

static int
rtl_tcp_apply_mute_and_reconfigure(struct rtl_device* s, unsigned char* u8, uint32_t* io_len, int fs4_shift_active) {
    if (!s || !u8 || !io_len) {
        return 0;
    }
    uint32_t len = *io_len;
    if (s->mute.load(std::memory_order_relaxed) > 0) {
        rtl_prepare_fragmented_u8_mute(s);
        int old = s->mute.load(std::memory_order_relaxed);
        if (old > 0) {
            uint32_t m = (uint32_t)old;
            if (m >= len) {
                rtl_account_fragmented_muted_u8_bytes(s, len, fs4_shift_active);
                rtl_record_capture_mute(s, (uint64_t)len, "retune_mute");
                s->mute.fetch_sub((int)len, std::memory_order_relaxed);
                if (m == len) {
                    s->mute_byte_phase.store(0, std::memory_order_relaxed);
                    rtl_finish_capture_mute_span(s);
                }
                return 1;
            }
            rtl_account_fragmented_muted_u8_bytes(s, m, fs4_shift_active);
            rtl_record_capture_mute(s, (uint64_t)m, "retune_mute");
            DSD_MEMMOVE(u8, u8 + m, len - m);
            len -= m;
            s->mute.fetch_sub((int)m, std::memory_order_relaxed);
            s->mute_byte_phase.store(0, std::memory_order_relaxed);
            rtl_finish_capture_mute_span(s);
        }
    }

    if (rtl_capture_reconfigure_hold_active(s)) {
        rtl_account_fragmented_muted_u8_bytes(s, len, fs4_shift_active);
        rtl_record_capture_mute(s, (uint64_t)len, "retune_reconfigure");
        if (!rtl_capture_reconfigure_hold_accepting_discards(s)) {
            rtl_complete_fragmented_capture_discard(s);
        }
        return 1;
    }
    *io_len = len;
    return 0;
}

static inline void
rtl_tcp_account_input_stats(struct rtl_device* s, uint32_t len) {
    if (!s || !s->stats_enabled) {
        return;
    }
    s->tcp_bytes_total += (uint64_t)len;
    s->tcp_bytes_window += (uint64_t)len;
}

static int
rtl_tcp_ensure_pending_capacity(struct rtl_device* s, size_t needed) {
    if (!s) {
        return 0;
    }
    if (needed == 0U) {
        return 1;
    }
    if (s->tcp_pending && s->tcp_pending_cap >= needed) {
        return 1;
    }
    size_t cap = rtl_tcp_round_up_page(needed);
    unsigned char* nb = static_cast<unsigned char*>(realloc(s->tcp_pending, cap));
    if (!nb) {
        return 0;
    }
    s->tcp_pending = nb;
    s->tcp_pending_cap = cap;
    return 1;
}

static size_t
rtl_tcp_fill_pending_slice(struct rtl_device* s, const unsigned char* u8, size_t len, size_t slice,
                           const struct rtl_u8_transform_policy_state* policy) {
    if (!s || !u8 || !policy || s->tcp_pending_len == 0U) {
        return 0U;
    }
    size_t consumed = 0U;
    size_t missing = (slice > s->tcp_pending_len) ? (slice - s->tcp_pending_len) : 0U;
    size_t take = (missing < len) ? missing : len;
    if (take > 0U && rtl_tcp_ensure_pending_capacity(s, slice) && s->tcp_pending_cap >= (s->tcp_pending_len + take)) {
        DSD_MEMCPY(s->tcp_pending + s->tcp_pending_len, u8, take);
        s->tcp_pending_len += take;
        consumed += take;
    }
    if (s->tcp_pending_len == slice) {
        int ring_status = rtl_write_u8_to_ring(s, s->tcp_pending, slice, policy->fs4_shift_active, policy->use_two_pass,
                                               policy->combine_rotate_active, 1);
        s->tcp_pending_len = 0U;
        if (ring_status == 2) {
            return len;
        }
    }
    return consumed;
}

static void
rtl_tcp_process_full_slices(struct rtl_device* s, unsigned char* u8, size_t len, size_t slice,
                            const struct rtl_u8_transform_policy_state* policy, size_t* io_consumed) {
    if (!s || !u8 || !policy || !io_consumed || slice == 0U) {
        return;
    }
    while ((len - *io_consumed) >= slice) {
        int ring_status = rtl_write_u8_to_ring(s, u8 + *io_consumed, slice, policy->fs4_shift_active,
                                               policy->use_two_pass, policy->combine_rotate_active, 1);
        *io_consumed += slice;
        if (ring_status == 2) {
            *io_consumed = len;
            s->tcp_pending_len = 0U;
            break;
        }
        if (ring_status != 0) {
            break;
        }
    }
}

static void
rtl_tcp_store_remainder(struct rtl_device* s, const unsigned char* data, size_t rem, int fs4_shift_active) {
    if (!s || !data || rem == 0U) {
        return;
    }
    if (!rtl_tcp_ensure_pending_capacity(s, rem)) {
        (void)rtl_drop_u8_bytes_preserve_alignment(data, rem, &s->iq_byte_carry, &s->rot_phase, fs4_shift_active);
        return;
    }
    DSD_MEMCPY(s->tcp_pending, data, rem);
    s->tcp_pending_len = rem;
}

static void
rtl_tcp_reassemble_and_submit(struct rtl_device* s, unsigned char* u8, uint32_t len,
                              const struct rtl_u8_transform_policy_state* policy) {
    if (!s || !u8 || !policy) {
        return;
    }
    const size_t slice = (s->buf_len > 0 ? (size_t)s->buf_len : 16384U);
    size_t consumed = rtl_tcp_fill_pending_slice(s, u8, len, slice, policy);
    if (consumed < len) {
        rtl_tcp_process_full_slices(s, u8, len, slice, policy, &consumed);
    }
    if (consumed < len) {
        rtl_tcp_store_remainder(s, u8 + consumed, len - consumed, policy->fs4_shift_active);
    }
}

static inline void
rtl_tcp_print_stats_if_due(struct rtl_device* s, uint64_t now_ns) {
    if (!s || !s->stats_enabled) {
        return;
    }
    uint64_t stats_dt_ns = now_ns - s->stats_last_ns;
    if (stats_dt_ns < 1000000000ULL) {
        return;
    }
    double dt = (double)stats_dt_ns / 1e9;
    double mbps = (double)s->tcp_bytes_window / dt / (1024.0 * 1024.0);
    double exp_bps = (s->rate > 0) ? (double)(s->rate * 2ULL) : 0.0;
    double exp_mbps = exp_bps / (1024.0 * 1024.0);
    uint64_t drops = s->input_ring ? s->input_ring->producer_drops.load() : 0ULL;
    uint64_t rdto = s->input_ring ? s->input_ring->read_timeouts.load() : 0ULL;
    DSD_FPRINTF(stderr, "rtl_tcp: %.2f MiB/s (exp %.2f), drops=%llu, res_full=%llu, read_timeouts=%llu\n", mbps,
                exp_mbps, (unsigned long long)drops, (unsigned long long)s->reserve_full_events,
                (unsigned long long)rdto);
    s->tcp_bytes_window = 0ULL;
    s->stats_last_ns = now_ns;
}

static inline void
rtl_tcp_resize_recv_buffer(struct rtl_tcp_loop_state* st, size_t new_size) {
    if (!st || !st->u8 || new_size == 0U || new_size == st->bufsz) {
        return;
    }
    unsigned char* nb = static_cast<unsigned char*>(realloc(st->u8, new_size));
    if (nb) {
        st->u8 = nb;
        st->bufsz = new_size;
    }
}

static inline uint64_t
rtl_tcp_counter_delta(uint64_t now, uint64_t prev) {
    return (now >= prev) ? (now - prev) : 0ULL;
}

static inline void
rtl_tcp_autotune_on_overflow(struct rtl_tcp_loop_state* st) {
    if (!st) {
        return;
    }
    if (st->bufsz > 16384U) {
        size_t next_size = st->bufsz / 2U;
        if (next_size < 16384U) {
            next_size = 16384U;
        }
        rtl_tcp_resize_recv_buffer(st, next_size);
    }
    st->waitall = 0;
}

static inline void
rtl_tcp_autotune_on_starvation(struct rtl_tcp_loop_state* st) {
    if (!st) {
        return;
    }
    if (st->bufsz > 8192U) {
        size_t next_size = st->bufsz / 2U;
        if (next_size < 8192U) {
            next_size = 8192U;
        }
        rtl_tcp_resize_recv_buffer(st, next_size);
    }
    st->waitall = 0;
}

static inline void
rtl_tcp_autotune_on_quiet(struct rtl_tcp_loop_state* st) {
    if (!st || st->bufsz >= 65536U) {
        return;
    }
    size_t next_size = st->bufsz + (st->bufsz / 2U);
    if (next_size > 65536U) {
        next_size = 65536U;
    }
    rtl_tcp_resize_recv_buffer(st, next_size);
}

static void
rtl_tcp_autotune_if_due(struct rtl_device* s, struct rtl_tcp_loop_state* st, uint64_t now_ns) {
    if (!s || !st || !s->tcp_autotune) {
        return;
    }
    uint64_t auto_dt_ns = now_ns - st->auto_last_ns;
    if (auto_dt_ns < 1000000000ULL) {
        return;
    }

    uint64_t drops = s->input_ring ? s->input_ring->producer_drops.load() : 0ULL;
    uint64_t rdto = s->input_ring ? s->input_ring->read_timeouts.load() : 0ULL;
    uint64_t resf = s->reserve_full_events;
    uint64_t d_drops = rtl_tcp_counter_delta(drops, st->prev_drops);
    uint64_t d_rdto = rtl_tcp_counter_delta(rdto, st->prev_rdto);
    uint64_t d_resf = rtl_tcp_counter_delta(resf, st->prev_res_full);
    st->prev_drops = drops;
    st->prev_rdto = rdto;
    st->prev_res_full = resf;

    if (d_drops > 0 || d_resf > 0) {
        rtl_tcp_autotune_on_overflow(st);
    } else if (d_rdto > 5U) {
        rtl_tcp_autotune_on_starvation(st);
    } else {
        rtl_tcp_autotune_on_quiet(st);
    }
    st->auto_last_ns = now_ns;
}

static inline void
rtl_tcp_periodic_maintenance(struct rtl_device* s, struct rtl_tcp_loop_state* st) {
    uint64_t now_ns = dsd_time_monotonic_ns();
    rtl_tcp_print_stats_if_due(s, now_ns);
    rtl_tcp_autotune_if_due(s, st, now_ns);
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
    struct rtl_tcp_loop_state st{};
    if (!rtl_tcp_init_loop_state(s, cfg, &st)) {
        if (s) {
            s->run.store(0);
        }
        DSD_THREAD_RETURN;
    }

    while (s->run.load() && exitflag == 0) {
        rtl_tcp_apply_backpressure_if_needed(s);
        int r = dsd_socket_recv(s->sockfd, st.u8, st.bufsz, st.waitall ? MSG_WAITALL : 0);
        if (r <= 0) {
            int recovered_r = 0;
            int recv_action = rtl_tcp_handle_recv_failure(s, &st, r, &recovered_r);
            if (recv_action == 0) {
                break;
            }
            if (recv_action == 2) {
                continue;
            }
            r = recovered_r;
        }

        st.consec_timeouts = 0;
        if (!rtl_tcp_watchdog_allows_processing(s, r)) {
            continue;
        }

        uint32_t len = (uint32_t)r;
        rtl_u8_transform_policy_state policy{};
        rtl_tcp_load_transform_policy(s, &policy);
        if (rtl_tcp_apply_mute_and_reconfigure(s, st.u8, &len, policy.fs4_shift_active)) {
            continue;
        }

        rtl_tcp_account_input_stats(s, len);
        rtl_submit_capture_bytes(s, st.u8, len);
        rtl_publish_cu8_input_level(s, st.u8, len);
        rtl_tcp_reassemble_and_submit(s, st.u8, len, &policy);
        rtl_tcp_metrics_update_ring_snapshot(s);
        rtl_tcp_periodic_maintenance(s, &st);
    }

    free(st.u8);
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
    int i, r, count, nearest;
    int* gains;
    r = rtlsdr_set_tuner_gain_mode(dev, 1);
    if (r < 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to enable manual gain.\n");
        return r;
    }
    count = rtlsdr_get_tuner_gains(dev, NULL);
    if (count <= 0) {
        return 0;
    }
    gains = static_cast<int*>(malloc(sizeof(int) * (size_t)count));
    if (gains == nullptr) {
        return 0;
    }
    count = rtlsdr_get_tuner_gains(dev, gains);
    if (count <= 0) {
        free(gains);
        return 0;
    }
    nearest = gains[0];
    for (i = 1; i < count; i++) {
        int err1 = abs(target_gain - nearest);
        int err2 = abs(target_gain - gains[i]);
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
        DSD_FPRINTF(stderr, " (WARNING: Failed to set Center Frequency). \n");
    } else {
        DSD_FPRINTF(stderr, " (Center Frequency: %u Hz.) \n", frequency);
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
        DSD_FPRINTF(stderr, "WARNING: Failed to set sample rate.\n");
    } else {
        DSD_FPRINTF(stderr, "Sampling at %u S/s.\n", samp_rate);
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
        DSD_FPRINTF(stderr, "WARNING: Failed to set direct sampling mode.\n");
        return r;
    }
    if (on == 0) {
        DSD_FPRINTF(stderr, "Direct sampling mode disabled.\n");
    }
    if (on == 1) {
        DSD_FPRINTF(stderr, "Enabled direct sampling mode, input 1/I.\n");
    }
    if (on == 2) {
        DSD_FPRINTF(stderr, "Enabled direct sampling mode, input 2/Q.\n");
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
        DSD_FPRINTF(
            stderr,
            "rtl_tcp: offset tuning capability is determined by the server; defaulting to disabled to match USB "
            "fs/4/rotate path (override with DSD_NEO_RTL_OFFSET_TUNING=1).\n");
        return;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        DSD_FPRINTF(stderr, "IQ replay: offset tuning capability query is not applicable.\n");
        return;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        DSD_FPRINTF(stderr,
                    "SoapySDR: offset tuning not implemented in this backend; using fs/4 shift fallback path.\n");
        return;
    }
    if (!dev->dev) {
        return;
    }
    int t = rtlsdr_get_tuner_type(dev->dev);
    int supported = 1;
    if (t == RTLSDR_TUNER_R820T || t == RTLSDR_TUNER_R828D) {
        supported = 0; /* per upstream librtlsdr */
    }
    DSD_FPRINTF(stderr, "RTL tuner: %s; hardware offset tuning supported by this librtlsdr: %s\n",
                rtl_tuner_type_name(t), supported ? "yes (expected)" : "no (expected upstream)");
}

/**
 * @brief Set tuner IF bandwidth (if supported by the library/driver).
 */
static int
verbose_set_tuner_bandwidth(rtlsdr_dev_t* dev, uint32_t bw_hz) {
    /* Pass-through to librtlsdr; bw_hz == 0 lets driver choose an appropriate filter */
    int r = rtlsdr_set_tuner_bandwidth(dev, (int)bw_hz);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set tuner bandwidth to %u Hz.\n", bw_hz);
    } else {
        if (bw_hz == 0) {
            DSD_FPRINTF(stderr, "Tuner bandwidth set to auto (driver).\n");
        } else {
            DSD_FPRINTF(stderr, "Tuner bandwidth set to %u Hz.\n", bw_hz);
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
        DSD_FPRINTF(stderr, "WARNING: Failed to set tuner gain.\n");
    } else {
        DSD_FPRINTF(stderr, "Tuner gain set to automatic.\n");
    }
    /* Original plan: enable RTL digital AGC in auto mode by default.
       Allow override via env DSD_NEO_RTL_AGC=0 to disable. */
    int want = env_agc_want();
    int ra = rtlsdr_set_agc_mode(dev, want);
    if (ra != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to %s RTL AGC.\n", want ? "enable" : "disable");
    } else {
        DSD_FPRINTF(stderr, "RTL AGC %s.\n", want ? "enabled" : "disabled");
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
        DSD_FPRINTF(stderr, "WARNING: Failed to enable manual gain.\n");
        return r;
    }
    r = rtlsdr_set_tuner_gain(dev, gain);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set tuner gain.\n");
    } else {
        DSD_FPRINTF(stderr, "Tuner gain set to %0.2f dB.\n", gain / 10.0);
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
        DSD_FPRINTF(stderr, "WARNING: Failed to set ppm error.\n");
    } else {
        DSD_FPRINTF(stderr, "Tuner error set to %i ppm.\n", ppm_error);
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
        DSD_FPRINTF(stderr, "WARNING: Failed to reset buffers.\n");
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
    dev->capture_mute_pending_bytes.store(0U, std::memory_order_relaxed);
    dev->capture_reconfigure_hold.store(RTL_CAPTURE_RECONFIGURE_INACTIVE, std::memory_order_relaxed);
    dev->replay_fs4_shift_enabled = 0;
    dev->replay_combine_rotate_enabled = 0;
    DSD_MEMSET(&dev->replay_cfg, 0, sizeof(dev->replay_cfg));
    dev->replay_initial_center_frequency_hz = 0;
    dev->replay_initial_capture_center_frequency_hz = 0;
    dev->replay_initial_sample_rate_hz = 0;
    dev->replay_initial_freq = 0;
    dev->replay_initial_rate = 0;
    dev->replay_src = NULL;
    DSD_MEMSET(&dev->replay_eof, 0, sizeof(dev->replay_eof));
    dev->replay_has_eof_state = 0;
    dev->replay_float_elements_written = 0;
    dev->soapy_profile_id = (int)dsdneo::SoapyProfileId::Generic;
    dev->soapy_requested_bandwidth_hz = -1;
    dev->soapy_named_gain_override = 0;
    dev->soapy_named_gain_skip_logged = 0;
    dev->soapy_args_string[0] = '\0';
    dev->soapy_driver_key[0] = '\0';
    dev->soapy_hardware_key[0] = '\0';
    dev->soapy_native_stream_format[0] = '\0';
    dev->soapy_requested_profile[0] = '\0';
    dev->soapy_requested_antenna[0] = '\0';
    dev->soapy_requested_clock[0] = '\0';
    dev->soapy_requested_settings[0] = '\0';
    dev->soapy_requested_gains[0] = '\0';
    dev->soapy_requested_stream_format[0] = '\0';
    if (dsd_mutex_init(&dev->tcp_metrics_lock) == 0) {
        dev->tcp_metrics_lock_inited = 1;
    }
}

static void
rtl_device_cleanup_common_state(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    if (dev->tcp_metrics_lock_inited) {
        (void)dsd_mutex_destroy(&dev->tcp_metrics_lock);
        dev->tcp_metrics_lock_inited = 0;
    }
}

/**
 * @brief Create and initialize an RTL-SDR device.
 *
 * @param dev_index Device index to open.
 * @param input_ring Pointer to input ring for USB data.
 * @param use_combine_rotate Whether to use combined rotate+widen when offset tuning is disabled.
 * @return Pointer to rtl_device handle, or NULL on failure.
 */
struct rtl_device*
rtl_device_create(int dev_index, struct input_ring_state* input_ring, int use_combine_rotate) {
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
    dev->combine_rotate_enabled = use_combine_rotate;
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
        DSD_FPRINTF(
            stderr,
            "ERROR: libusb exception in rtlsdr_open (MSVC/Windows). "
            "Check that the bundled libusb/librtlsdr DLLs match the build and the device driver is installed.\n");
        r = -1;
    }
#else
    r = rtlsdr_open(&dev->dev, (uint32_t)dev_index);
#endif
    if (r < 0) {
        DSD_FPRINTF(stderr, "Failed to open rtlsdr device %d.\n", dev_index);
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }

    return dev;
}

struct rtl_device*
rtl_device_create_tcp(const char* host, int port, struct input_ring_state* input_ring, int use_combine_rotate,
                      int autotune_enabled) {
    if (!input_ring || !host || port <= 0) {
        return NULL;
    }
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    rtl_device_init_common_state(dev);
    /*
     * Seed the common RTL-shaped fields before entering the Soapy-specific path.
     * The rest of the capture stack expects these defaults regardless of backend.
     * Cleanup can therefore use the normal rtl_device teardown on every failure.
     */
    dev->dev = NULL;
    dev->dev_index = -1;
    dev->input_ring = input_ring;
    dev->thread_started = 0;
    dev->mute = 0;
    dev->mute_byte_phase = 0;
    dev->combine_rotate_enabled = use_combine_rotate;
    dev->backend = RTL_BACKEND_TCP;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_lock_inited = 0;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->sockfd = DSD_INVALID_SOCKET;
    DSD_SNPRINTF(dev->host, sizeof(dev->host), "%s", host);
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
        rtl_device_cleanup_common_state(dev);
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
    DSD_FPRINTF(stderr, "rtl_tcp: Connected to %s:%d\n", host, port);
    /* Optional TCP stats: enable with DSD_NEO_TCP_STATS=1 */
    {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->tcp_stats_enable) {
            dev->stats_enabled = 1;
            dev->stats_last_ns = dsd_time_monotonic_ns();
            DSD_FPRINTF(stderr, "rtl_tcp: stats enabled.\n");
        }
    }
    /* Initialize TCP connection quality metrics (lag resilience). */
    rtl_tcp_metrics_reset_device(dev, dev->rate);
    /* Initialize autotune from env if not already enabled by caller */
    if (!dev->tcp_autotune) {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->tcp_autotune_enable) {
            dev->tcp_autotune = 1;
        }
    }
    return dev;
}

static struct rtl_device*
rtl_device_alloc_soapy_base(struct input_ring_state* input_ring, int use_combine_rotate) {
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    rtl_device_init_common_state(dev);
    dev->dev = NULL;
    dev->dev_index = -1;
    dev->input_ring = input_ring;
    dev->combine_rotate_enabled = use_combine_rotate;
    dev->backend = RTL_BACKEND_SOAPY;
    dev->soapy_format = SOAPY_FMT_NONE;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->sockfd = DSD_INVALID_SOCKET;
    dev->host[0] = '\0';
    dev->run.store(0);
    dev->agc_mode = 1;
    return dev;
}

struct rtl_device*
rtl_device_create_soapy(const char* soapy_args, struct input_ring_state* input_ring, int use_combine_rotate) {
    if (!input_ring) {
        return NULL;
    }
    struct rtl_device* dev = rtl_device_alloc_soapy_base(input_ring, use_combine_rotate);
    if (!dev) {
        return NULL;
    }
    dev->thread_started = 0;
    dev->mute = 0;
    dev->mute_byte_phase = 0;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    dev->port = 0;
    dev->offset_tuning = 0;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;

#ifndef USE_SOAPYSDR
    (void)soapy_args;
    DSD_FPRINTF(stderr, "SoapySDR backend unavailable in this build.\n");
    rtl_device_cleanup_common_state(dev);
    free(dev);
    return NULL;
#else
    // The Soapy device handle and cached capabilities are protected by one mutex.
    if (dsd_mutex_init(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to initialize mutex.\n");
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }
    dev->soapy_lock_inited = 1;

    const char* args_cstr = soapy_args ? soapy_args : "";
    std::string args_string;
    // Validate the args string before it is copied into the device state.
    try {
        args_string = args_cstr;
        rtl_device_copy_cstr(dev->soapy_args_string, sizeof(dev->soapy_args_string), args_cstr);
        (void)SoapySDR::KwargsFromString(args_string);
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: invalid args string '%s': %s\n", args_cstr, e.what());
        (void)dsd_mutex_destroy(&dev->soapy_lock);
        dev->soapy_lock_inited = 0;
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }

    // Create the device while holding the lock so later configuration paths share ordering.
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex during creation.\n");
        (void)dsd_mutex_destroy(&dev->soapy_lock);
        dev->soapy_lock_inited = 0;
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }
    try {
        SoapySDR::KwargsList found = SoapySDR::Device::enumerate(args_string);
        if (found.empty()) {
            DSD_FPRINTF(stderr, "SoapySDR: enumerate found no devices for args '%s'.\n", args_cstr);
        }
        dev->soapy_dev = SoapySDR::Device::make(args_string);
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: exception in Device::make: %s\n", e.what());
        dev->soapy_dev = NULL;
    }
    (void)dsd_mutex_unlock(&dev->soapy_lock);

    if (!dev->soapy_dev) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to create device for args '%s'.\n", args_cstr);
        (void)dsd_mutex_destroy(&dev->soapy_lock);
        dev->soapy_lock_inited = 0;
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }

    // Prime the UI/config caches; failures here leave a usable device with defaults.
    if (dsd_mutex_lock(&dev->soapy_lock) == 0) {
        try {
            soapy_cache_identity_locked(dev);
            (void)soapy_refresh_stream_format_locked(dev, NULL);
            soapy_log_capability_summary_locked(dev);
        } catch (const std::exception& e) {
            DSD_FPRINTF(stderr, "SoapySDR: exception selecting stream format: %s\n", e.what());
        }
        (void)dsd_mutex_unlock(&dev->soapy_lock);
    }

    return dev;
#endif
}

static void
rtl_device_store_soapy_config_request(struct rtl_device* dev, const struct rtl_soapy_config* cfg, size_t config_size) {
    if (!dev) {
        return;
    }
    const char* profile = RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, profile) ? cfg->profile : NULL;
    const char* antenna = RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, antenna) ? cfg->antenna : NULL;
    const char* clock_source =
        RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, clock_source) ? cfg->clock_source : NULL;
    const char* settings = RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, settings) ? cfg->settings : NULL;
    const char* gains = RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, gains) ? cfg->gains : NULL;
    const char* stream_format =
        RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, stream_format) ? cfg->stream_format : NULL;

    rtl_device_copy_cstr(dev->soapy_requested_profile, sizeof(dev->soapy_requested_profile), profile);
    rtl_device_copy_cstr(dev->soapy_requested_antenna, sizeof(dev->soapy_requested_antenna), antenna);
    rtl_device_copy_cstr(dev->soapy_requested_clock, sizeof(dev->soapy_requested_clock), clock_source);
    rtl_device_copy_cstr(dev->soapy_requested_settings, sizeof(dev->soapy_requested_settings), settings);
    rtl_device_copy_cstr(dev->soapy_requested_gains, sizeof(dev->soapy_requested_gains), gains);
    rtl_device_copy_cstr(dev->soapy_requested_stream_format, sizeof(dev->soapy_requested_stream_format), stream_format);
    dev->soapy_requested_bandwidth_hz =
        RTL_SOAPY_CONFIG_FIELD_AVAILABLE(cfg, config_size, bandwidth_hz) ? cfg->bandwidth_hz : -1;
}

#ifdef USE_SOAPYSDR
static int
rtl_device_apply_soapy_configuration_locked(struct rtl_device* dev) {
    dsdneo::SoapyProfileId parsed = dsdneo::SoapyProfileId::Auto;
    if (dev->soapy_requested_profile[0] != '\0'
        && !dsdneo::soapy_profile_parse_name(dev->soapy_requested_profile, &parsed)) {
        DSD_FPRINTF(stderr, "SoapySDR: unknown profile '%s'; using automatic profile detection.\n",
                    dev->soapy_requested_profile);
    }
    soapy_cache_identity_locked(dev);
    const dsdneo::SoapyProfile& profile = dsdneo::soapy_profile_by_id((dsdneo::SoapyProfileId)dev->soapy_profile_id);
    DSD_FPRINTF(stderr, "SoapySDR: using %s profile.\n", profile.display_name);

    int rc = soapy_apply_antenna_locked(dev);
    if (rc != 0) {
        return rc;
    }
    rc = soapy_apply_clock_locked(dev);
    if (rc != 0) {
        return rc;
    }
    rc = soapy_apply_settings_locked(dev);
    if (rc != 0) {
        return rc;
    }
    rc = soapy_apply_named_gains_locked(dev);
    if (rc != 0) {
        return rc;
    }
    return soapy_refresh_stream_format_locked(dev, NULL);
}
#endif

int
rtl_device_configure_soapy(struct rtl_device* dev, const struct rtl_soapy_config* config) {
    return rtl_device_configure_soapy_sized(dev, config, RTL_SOAPY_CONFIG_LEGACY_SIZE);
}

int
rtl_device_configure_soapy_sized(struct rtl_device* dev, const struct rtl_soapy_config* config, size_t config_size) {
    if (!dev || dev->backend != RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    rtl_device_store_soapy_config_request(dev, config, config ? config_size : 0U);
#ifndef USE_SOAPYSDR
    return DSD_ERR_NOT_SUPPORTED;
#else
    if (!dev->soapy_dev || !dev->soapy_lock_inited) {
        return -1;
    }
    return soapy_call_locked(dev, "configure Soapy profile",
                             [&]() -> int { return rtl_device_apply_soapy_configuration_locked(dev); });
#endif
}

static void
rtl_device_init_iq_replay_backend_state(struct rtl_device* dev, const dsd_iq_replay_config* cfg,
                                        struct input_ring_state* input_ring,
                                        const struct rtl_replay_eof_state* eof_state) {
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
}

static int
rtl_device_open_iq_replay_source(struct rtl_device* dev, const dsd_iq_replay_config* cfg) {
    dsd_iq_replay_config opened_cfg;
    DSD_MEMSET(&opened_cfg, 0, sizeof(opened_cfg));
    dsd_iq_replay_source* replay_src = NULL;
    char err_buf[256] = {0};
    const char* open_path = cfg->metadata_path[0] ? cfg->metadata_path : cfg->data_path;
    int rc = dsd_iq_replay_open(open_path, &opened_cfg, &replay_src, err_buf, sizeof(err_buf));
    if (rc != DSD_IQ_OK || !replay_src) {
        DSD_FPRINTF(stderr, "IQ replay: failed to open '%s': %s\n", open_path, err_buf[0] ? err_buf : "unknown error");
        return -1;
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
    dev->replay_initial_center_frequency_hz = opened_cfg.center_frequency_hz;
    dev->replay_initial_capture_center_frequency_hz = opened_cfg.capture_center_frequency_hz;
    dev->replay_initial_sample_rate_hz = opened_cfg.sample_rate_hz;
    dev->replay_initial_freq = dev->freq;
    dev->replay_initial_rate = dev->rate;
    dev->gain = opened_cfg.tuner_gain_tenth_db;
    dev->ppm_error = opened_cfg.ppm;
    return 0;
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
    rtl_device_init_iq_replay_backend_state(dev, cfg, input_ring, eof_state);
    if (rtl_device_open_iq_replay_source(dev, cfg) != 0) {
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }
    return dev;
}

static void
rtl_device_stop_usb_backend_thread(struct rtl_device* dev) {
    if (!dev || !dev->dev) {
        return;
    }
    rtlsdr_cancel_async(dev->dev);
}

static void
rtl_device_signal_replay_forced_stop(struct rtl_device* dev) {
    if (!dev || dev->backend != RTL_BACKEND_IQ_REPLAY) {
        return;
    }
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

static void
rtl_device_stop_streaming_backend_thread(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    dev->run.store(0);
    if (dev->backend == RTL_BACKEND_TCP && dev->sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_shutdown(dev->sockfd, SHUT_RDWR);
    }
    rtl_device_signal_replay_forced_stop(dev);
}

static void
rtl_device_request_thread_stop(struct rtl_device* dev) {
    if (!dev || !dev->thread_started) {
        return;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        rtl_device_stop_usb_backend_thread(dev);
        return;
    }
    if (dev->backend == RTL_BACKEND_TCP || dev->backend == RTL_BACKEND_SOAPY || dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rtl_device_stop_streaming_backend_thread(dev);
    }
}

static void
rtl_device_cleanup_usb_before_close(struct rtl_device* dev) {
    if (!dev || dev->backend != RTL_BACKEND_USB || !dev->dev) {
        return;
    }
#ifdef USE_RTLSDR_BIAS_TEE
    (void)rtlsdr_set_bias_tee(dev->dev, 0);
#endif
    (void)rtlsdr_reset_buffer(dev->dev);
}

static void
rtl_device_close_backend_resources(struct rtl_device* dev) {
    if (!dev) {
        return;
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
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dsd_iq_replay_config_clear(&dev->replay_cfg);
    }
}

static void
rtl_device_release_tcp_pending(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    free(dev->tcp_pending);
    dev->tcp_pending = NULL;
    dev->tcp_pending_len = 0;
    dev->tcp_pending_cap = 0;
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
        rtl_device_request_thread_stop(dev);
        dsd_thread_join(dev->thread);
        dev->thread_started = 0;
    }
    rtl_device_cleanup_usb_before_close(dev);
    rtl_device_close_backend_resources(dev);
    rtl_device_release_tcp_pending(dev);
    rtl_device_cleanup_common_state(dev);

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
    if (dev->backend == RTL_BACKEND_USB || dev->backend == RTL_BACKEND_TCP) {
        rtl_reset_capture_state_on_stream_boundary(dev);
    }
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
            DSD_FPRINTF(stderr, "IQ replay: ignoring retune request to %u Hz (capture center is %" PRIu64 " Hz).\n",
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
        int rc = rtl_tcp_send_cmd(dev->sockfd, 0x02, samp_rate);
        if (rc == 0) {
            /* The TCP device is created before the final stream rate is known.
             * Reset the metric windows when the rate is programmed so startup
             * delay cannot count against the first watchdog interval. */
            rtl_tcp_metrics_reset_device(dev, samp_rate);
        }
        return rc;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return soapy_call_locked(dev, "setSampleRate", [&]() -> int {
            double requested = (double)samp_rate;
            double applied = requested;
            bool adjusted = false;
            try {
                std::vector<double> listed =
                    soapy_valid_positive_rates(dev->soapy_dev->listSampleRates(SOAPY_SDR_RX, 0));
                std::vector<dsdneo::SoapyRange> ranges =
                    soapy_ranges_from_range_list(dev->soapy_dev->getSampleRateRange(SOAPY_SDR_RX, 0));
                applied = dsdneo::soapy_nearest_sample_rate(requested, listed, ranges, &adjusted);
            } catch (const std::exception&) {
                applied = requested;
            }
            if (adjusted) {
                DSD_FPRINTF(stderr, "SoapySDR: adjusted sample rate from %.0f Hz to %.0f Hz.\n", requested, applied);
            }
            dev->soapy_dev->setSampleRate(SOAPY_SDR_RX, 0, applied);
            dev->rate = (uint32_t)std::lround(applied);
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
            uint32_t rounded = (uint32_t)std::lround(actual);
            dev->rate = rounded;
            return (int)rounded;
        }
#endif
        return (dev->rate > 0) ? (int)dev->rate : -1;
    }
    return (int)dev->rate;
}

static int
rtl_device_set_gain_usb(struct rtl_device* dev, int gain) {
    if (!dev || !dev->dev) {
        return -1;
    }
    if (gain == RTL_AUTO_GAIN) {
        return verbose_auto_gain(dev->dev);
    }
    int nearest = nearest_gain(dev->dev, gain);
    return verbose_gain_set(dev->dev, nearest);
}

static int
rtl_device_set_gain_tcp(struct rtl_device* dev, int gain) {
    if (!dev) {
        return -1;
    }
    if (gain == RTL_AUTO_GAIN) {
        dev->agc_mode = 1;
        int r = rtl_tcp_send_cmd(dev->sockfd, 0x03, 0);
        if (r < 0) {
            return r;
        }
        return rtl_tcp_send_cmd(dev->sockfd, 0x08, (uint32_t)env_agc_want());
    }
    dev->agc_mode = 0;
    int r = rtl_tcp_send_cmd(dev->sockfd, 0x03, 1);
    if (r < 0) {
        return r;
    }
    return rtl_tcp_send_cmd(dev->sockfd, 0x04, (uint32_t)gain);
}

#ifdef USE_SOAPYSDR
static int
rtl_device_soapy_named_gain_override_active(struct rtl_device* dev) {
    if (!dev || !dev->soapy_named_gain_override) {
        return 0;
    }
    if (!dev->soapy_named_gain_skip_logged) {
        DSD_FPRINTF(stderr, "SoapySDR: preserving explicit named gains; aggregate gain requests are ignored.\n");
        dev->soapy_named_gain_skip_logged = 1;
    }
    dev->agc_mode = 0;
    return 1;
}

static int
rtl_device_set_gain_soapy(struct rtl_device* dev, int gain) {
    if (rtl_device_soapy_named_gain_override_active(dev)) {
        return 0;
    }
    if (gain == RTL_AUTO_GAIN) {
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
    dev->gain = gain;
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_device_set_gain_usb(dev, gain);
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->agc_mode = 0;
        return 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_device_set_gain_tcp(dev, gain);
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return rtl_device_set_gain_soapy(dev, gain);
    }
#endif
    return -1;
}

static int
rtl_device_set_gain_nearest_usb(struct rtl_device* dev, int target_tenth_db) {
    if (!dev || !dev->dev) {
        return -1;
    }
    int g = nearest_gain(dev->dev, target_tenth_db);
    if (g < 0) {
        return g;
    }
    int r = rtlsdr_set_tuner_gain_mode(dev->dev, 1);
    if (r < 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to enable manual gain.\n");
        return r;
    }
    r = rtlsdr_set_tuner_gain(dev->dev, g);
    if (r < 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set tuner gain (nearest).\n");
        return r;
    }
    dev->gain = g;
    DSD_FPRINTF(stderr, "Tuner manual gain (nearest): %0.1f dB.\n", (double)g / 10.0);
    return 0;
}

static int
rtl_device_set_gain_nearest_tcp(struct rtl_device* dev, int target_tenth_db) {
    if (!dev) {
        return -1;
    }
    (void)rtl_tcp_send_cmd(dev->sockfd, 0x03, 1U);
    (void)rtl_tcp_send_cmd(dev->sockfd, 0x04, (uint32_t)target_tenth_db);
    dev->gain = target_tenth_db;
    return 0;
}

#ifdef USE_SOAPYSDR
static int
rtl_device_set_gain_nearest_soapy(struct rtl_device* dev, int target_tenth_db) {
    if (rtl_device_soapy_named_gain_override_active(dev)) {
        return 0;
    }
    const double target_db = (double)target_tenth_db / 10.0;
    double applied_db = target_db;
    int rc = soapy_call_locked(dev, "setGain(nearest)", [&]() -> int {
        std::vector<std::string> names = dev->soapy_dev->listGains(SOAPY_SDR_RX, 0);
        SoapySDR::Range range = dev->soapy_dev->getGainRange(SOAPY_SDR_RX, 0);
        if (names.empty() && std::fabs(range.minimum() - range.maximum()) <= 1.0e-9) {
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

int
rtl_device_set_gain_nearest(struct rtl_device* dev, int target_tenth_db) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_device_set_gain_nearest_usb(dev, target_tenth_db);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_device_set_gain_nearest_tcp(dev, target_tenth_db);
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->gain = target_tenth_db;
        return 0;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return rtl_device_set_gain_nearest_soapy(dev, target_tenth_db);
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
            if (names.empty() && std::fabs(range.minimum() - range.maximum()) <= 1.0e-9) {
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
rtl_device_is_auto_gain(const struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        /* We track AUTO vs manual in the requested field. */
        return (dev->gain == RTL_AUTO_GAIN) ? 1 : 0;
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
    int r = -1;
    if (dev->backend == RTL_BACKEND_USB) {
        if (!dev->dev) {
            return -1;
        }
        r = rtlsdr_set_offset_tuning(dev->dev, on ? 1 : 0);
        if (r == 0) {
            DSD_FPRINTF(stderr, on ? "Offset tuning mode enabled.\n" : "Offset tuning mode disabled.\n");
        } else {
            int tuner_type = rtlsdr_get_tuner_type(dev->dev);
            DSD_FPRINTF(stderr, "WARNING: Failed to set offset tuning (%d) for tuner %s.\n", r,
                        rtl_tuner_type_name(tuner_type));
        }
    } else if (dev->backend == RTL_BACKEND_TCP) {
        r = rtl_tcp_send_cmd(dev->sockfd, 0x0A, (uint32_t)(on ? 1 : 0));
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        r = 0;
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        r = DSD_ERR_NOT_SUPPORTED;
    }
    if (r == 0) {
        dev->offset_tuning = on ? 1 : 0;
    }
    return r;
}

#ifdef USE_SOAPYSDR
static dsdneo::SoapyBandwidthChoice
rtl_device_choose_soapy_bandwidth(const struct rtl_device* dev, uint32_t bw_hz) {
    int tuner_bw_hz = (int)bw_hz;
    bool tuner_bw_hz_is_set = false;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg && cfg->tuner_bw_hz_is_set) {
        tuner_bw_hz = cfg->tuner_bw_hz;
        tuner_bw_hz_is_set = true;
    }
    const dsdneo::SoapyProfile& profile = dsdneo::soapy_profile_by_id((dsdneo::SoapyProfileId)dev->soapy_profile_id);
    return dsdneo::soapy_choose_bandwidth_hz(tuner_bw_hz, tuner_bw_hz_is_set, dev->soapy_requested_bandwidth_hz,
                                             profile.default_bandwidth_hz);
}

static int
rtl_device_apply_soapy_bandwidth_locked(struct rtl_device* dev, const dsdneo::SoapyBandwidthChoice& choice) {
    if (!choice.should_apply) {
        return 0;
    }
    std::vector<dsdneo::SoapyRange> ranges;
    try {
        ranges = soapy_ranges_from_range_list(dev->soapy_dev->getBandwidthRange(SOAPY_SDR_RX, 0));
    } catch (const std::exception& e) {
        if (choice.explicit_request) {
            DSD_FPRINTF(stderr,
                        "SoapySDR: explicit bandwidth %d Hz is unsupported; could not query bandwidth ranges: %s\n",
                        choice.bandwidth_hz, e.what());
        }
        return choice.explicit_request ? DSD_ERR_NOT_SUPPORTED : 0;
    }
    bool supported = false;
    double applied_hz = dsdneo::soapy_nearest_in_ranges((double)choice.bandwidth_hz, ranges, &supported);
    if (!supported) {
        if (choice.explicit_request) {
            DSD_FPRINTF(stderr, "SoapySDR: explicit bandwidth %d Hz is unsupported by this device.\n",
                        choice.bandwidth_hz);
        }
        return choice.explicit_request ? DSD_ERR_NOT_SUPPORTED : 0;
    }
    if (std::fabs(applied_hz - (double)choice.bandwidth_hz) > 0.5) {
        DSD_FPRINTF(stderr, "SoapySDR: adjusted bandwidth from %d Hz to %.0f Hz.\n", choice.bandwidth_hz, applied_hz);
    }
    dev->soapy_dev->setBandwidth(SOAPY_SDR_RX, 0, applied_hz);
    return 0;
}
#endif

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
        dsdneo::SoapyBandwidthChoice choice = rtl_device_choose_soapy_bandwidth(dev, bw_hz);
        return soapy_call_locked(dev, "setBandwidth",
                                 [&]() -> int { return rtl_device_apply_soapy_bandwidth_locked(dev, choice); });
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
        r = dsd_thread_create(&dev->thread, dongle_thread_fn, dev);
    } else if (dev->backend == RTL_BACKEND_TCP) {
        dev->run.store(1);
        r = dsd_thread_create(&dev->thread, tcp_thread_fn, dev);
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        if (!dev->replay_src) {
            dev->thread_started = 0;
            return -1;
        }
        dev->run.store(1, std::memory_order_release);
        r = dsd_thread_create(&dev->thread, replay_thread_fn, dev);
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        if (!dev->soapy_dev) {
            dev->thread_started = 0;
            return -1;
        }
        dev->run.store(1);
        r = dsd_thread_create(&dev->thread, soapy_thread_fn, dev);
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
    int mute_bytes = rtl_capture_align_u8_iq_bytes(bytes);
    if (rtl_capture_reconfigure_hold_active(dev)) {
        unsigned int carry = (unsigned int)(dev->mute_byte_phase.load(std::memory_order_relaxed) & 1U);
        mute_bytes = rtl_capture_complete_fragmented_u8_discard(mute_bytes, carry);
    } else {
        dev->capture_mute_pending_bytes.store(0U, std::memory_order_release);
        dev->mute_byte_phase.store(0, std::memory_order_relaxed);
    }
    dev->mute.store(mute_bytes, std::memory_order_relaxed);
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
        DSD_FPRINTF(stderr, "WARNING: Failed to %sable RTL-SDR bias tee.\n", dev->bias_tee_on ? "en" : "dis");
        return -1;
    }
    DSD_FPRINTF(stderr, "RTL-SDR bias tee %s.\n", dev->bias_tee_on ? "enabled" : "disabled");
    return 0;
#else
    (void)on;
    DSD_FPRINTF(stderr, "NOTE: librtlsdr built without bias tee API; ignoring bias setting on USB.\n");
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
rtl_device_get_tcp_autotune(const struct rtl_device* dev) {
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
        DSD_FPRINTF(stderr, "WARNING: Failed to set xtal freq (rtl=%u, tuner=%u).\n", rtl_xtal_hz, tuner_xtal_hz);
        return -1;
    }
    DSD_FPRINTF(stderr, "Set xtal freq: rtl=%u Hz%s, tuner=%u Hz%s.\n", rtl_xtal_hz, rtl_xtal_hz ? "" : " (unchanged)",
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
        DSD_FPRINTF(stderr, "WARNING: Failed to %s RTL-SDR test mode.\n", on ? "enable" : "disable");
        return -1;
    }
    DSD_FPRINTF(stderr, "RTL-SDR test mode %s.\n", on ? "enabled" : "disabled");
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
        DSD_FPRINTF(stderr, "WARNING: Failed to set IF gain: stage=%d, gain=%d (0.1 dB).\n", stage, gain_tenth_db);
        return -1;
    }
    DSD_FPRINTF(stderr, "IF gain set: stage=%d, gain=%0.1f dB.\n", stage, gain_tenth_db / 10.0);
    return 0;
}

void
rtl_device_set_iq_capture_writer(struct rtl_device* dev, struct dsd_iq_capture_writer* writer) {
    if (!dev) {
        return;
    }
    dev->iq_capture_writer = writer;
    dev->capture_mute_pending_bytes.store(0U, std::memory_order_release);
    dev->capture_reconfigure_hold.store(RTL_CAPTURE_RECONFIGURE_INACTIVE, std::memory_order_release);
    if (!writer) {
        dev->capture_retune_count.store(0, std::memory_order_release);
    }
}

void
rtl_device_begin_capture_reconfigure(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    dev->capture_reconfigure_hold.store(RTL_CAPTURE_RECONFIGURE_ACTIVE, std::memory_order_release);
}

void
rtl_device_end_capture_reconfigure(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    int expected = RTL_CAPTURE_RECONFIGURE_ACTIVE;
    if (dev->capture_reconfigure_hold.compare_exchange_strong(expected, RTL_CAPTURE_RECONFIGURE_RELEASING,
                                                              std::memory_order_acq_rel, std::memory_order_acquire)) {
        rtl_complete_fragmented_capture_discard(dev);
        dev->capture_reconfigure_hold.store(RTL_CAPTURE_RECONFIGURE_INACTIVE, std::memory_order_release);
    }
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" void
rtl_device_test_end_capture_reconfigure_with_odd_carry(int* out_hold, int* out_mute, int* out_mute_byte_phase) {
    rtl_device dev{};
    dev.replay_cfg.format = DSD_IQ_FORMAT_CU8;
    dev.backend = RTL_BACKEND_TCP;
    dev.capture_reconfigure_hold.store(RTL_CAPTURE_RECONFIGURE_ACTIVE, std::memory_order_relaxed);
    dev.mute.store(0, std::memory_order_relaxed);
    dev.mute_byte_phase.store(1, std::memory_order_relaxed);

    rtl_device_end_capture_reconfigure(&dev);

    if (out_hold) {
        *out_hold = dev.capture_reconfigure_hold.load(std::memory_order_acquire);
    }
    if (out_mute) {
        *out_mute = dev.mute.load(std::memory_order_acquire);
    }
    if (out_mute_byte_phase) {
        *out_mute_byte_phase = dev.mute_byte_phase.load(std::memory_order_acquire);
    }
}

extern "C" int
rtl_device_test_begin_capture_reconfigure_without_writer(int* out_hold) {
    rtl_device dev{};
    rtl_device_init_common_state(&dev);
    rtl_device_begin_capture_reconfigure(&dev);
    if (out_hold) {
        *out_hold = dev.capture_reconfigure_hold.load(std::memory_order_acquire);
    }
    rtl_device_cleanup_common_state(&dev);
    return 0;
}

extern "C" int
rtl_device_test_usb_reconfigure_discards_samples(size_t input_bytes, size_t* out_ring_used) {
    if (!out_ring_used || input_bytes == 0U || input_bytes > 256U) {
        return -1;
    }
    input_ring_state ring{};
    if (input_ring_init(&ring, 512U) != 0) {
        return -2;
    }
    unsigned char* buf = static_cast<unsigned char*>(calloc(input_bytes, sizeof(unsigned char)));
    if (!buf) {
        input_ring_destroy(&ring);
        return -3;
    }
    for (size_t i = 0; i < input_bytes; i++) {
        buf[i] = (unsigned char)(127U + (i & 1U));
    }

    rtl_device dev{};
    rtl_device_init_common_state(&dev);
    dev.input_ring = &ring;
    dev.backend = RTL_BACKEND_USB;
    rtl_device_begin_capture_reconfigure(&dev);
    rtlsdr_callback(buf, (uint32_t)input_bytes, &dev);
    *out_ring_used = input_ring_used(&ring);
    rtl_device_end_capture_reconfigure(&dev);
    rtl_device_cleanup_common_state(&dev);
    free(buf);
    input_ring_destroy(&ring);
    return 0;
}

extern "C" int
rtl_device_test_soapy_config_settings_visibility(size_t config_size, const char* settings, int* out_seen) {
    if (!out_seen) {
        return -1;
    }

    rtl_device dev{};
    rtl_device_init_common_state(&dev);
    rtl_soapy_config cfg{};
    cfg.settings = settings;

    rtl_device_store_soapy_config_request(&dev, &cfg, config_size);
    *out_seen = dev.soapy_requested_settings[0] != '\0' ? 1 : 0;

    rtl_device_cleanup_common_state(&dev);
    return 0;
}
#endif

void
rtl_device_note_capture_retune(struct rtl_device* dev) {
    if (!dev || !dev->iq_capture_writer) {
        return;
    }
    (void)dev->capture_retune_count.fetch_add(1U, std::memory_order_relaxed);
}

void
rtl_device_record_capture_retune(struct rtl_device* dev, uint64_t center_frequency_hz,
                                 uint64_t capture_center_frequency_hz, uint32_t sample_rate_hz, const char* reason) {
    if (!dev || !dev->iq_capture_writer) {
        return;
    }
    dsd_iq_event event;
    DSD_MEMSET(&event, 0, sizeof(event));
    event.kind = DSD_IQ_EVENT_RETUNE;
    event.center_frequency_hz = center_frequency_hz;
    event.capture_center_frequency_hz = capture_center_frequency_hz;
    event.sample_rate_hz = sample_rate_hz;
    rtl_copy_event_reason(event.reason, sizeof(event.reason), reason ? reason : "retune");
    rtl_record_capture_event(dev, &event);
    (void)dev->capture_retune_count.fetch_add(1U, std::memory_order_relaxed);
}

void
rtl_device_record_capture_reset(struct rtl_device* dev, uint64_t center_frequency_hz,
                                uint64_t capture_center_frequency_hz, uint32_t sample_rate_hz, const char* reason) {
    if (!dev || !dev->iq_capture_writer) {
        return;
    }
    dsd_iq_event event;
    DSD_MEMSET(&event, 0, sizeof(event));
    event.kind = DSD_IQ_EVENT_RESET;
    event.center_frequency_hz = center_frequency_hz;
    event.capture_center_frequency_hz = capture_center_frequency_hz;
    event.sample_rate_hz = sample_rate_hz;
    rtl_copy_event_reason(event.reason, sizeof(event.reason), reason ? reason : "reset");
    rtl_record_capture_event(dev, &event);
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

int
rtl_device_get_tcp_quality_snapshot(struct rtl_device* dev, struct tcp_quality_snapshot* out) {
    if (!dev || !out || dev->backend != RTL_BACKEND_TCP) {
        if (out) {
            *out = tcp_quality_snapshot{};
        }
        return -1;
    }
    rtl_tcp_metrics_lock(dev);
    *out = tcp_metrics_get_snapshot(&dev->tcp_metrics);
    rtl_tcp_metrics_unlock(dev);
    return 0;
}
