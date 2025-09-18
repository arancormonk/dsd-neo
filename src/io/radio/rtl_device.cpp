// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR device I/O implementation and USB ingestion pipeline.
 *
 * Implements the opaque `rtl_device` handle, device configuration helpers,
 * realtime threading hooks, and the asynchronous USB callback that widens
 * u8 I/Q samples into s16 and feeds the `input_ring_state`.
 */

#include <atomic>
#include <dsd-neo/core/dsd.h> // for exitflag
#include <dsd-neo/dsp/simd_widen.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <math.h>
#include <pthread.h>
#include <rtl-sdr.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// Forward declarations from main file
extern volatile uint8_t exitflag;

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
    pthread_t thread;
    int thread_started;
    struct input_ring_state* input_ring;
    int combine_rotate_enabled;
};

/**
 * Hint that a pointer is aligned to a compile-time boundary for vectorization.
 *
 * This is a lightweight wrapper over compiler intrinsics to improve
 * auto-vectorization by promising the compiler that the pointer meets the
 * specified alignment. Use with care and only when the alignment guarantee
 * is actually met.
 */
#define DSD_NEO_RESTRICT __restrict__

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
/* moved to runtime/rt_sched.cpp */

/**
 * @brief Rotate IQ data by 90 degrees in-place.
 *
 * @param buf Interleaved IQ byte buffer.
 * @param len Buffer length in bytes (processed in blocks of 8).
 */
static void
rotate_90(unsigned char* buf, uint32_t len) {
    uint32_t i;
    unsigned char tmp;
    /* Process only full 8-byte blocks (4 IQ pairs) to avoid overrun */
    uint32_t full = len - (len % 8);
    for (i = 0; i < full; i += 8) {
        /* uint8_t negation = 255 - x */
        tmp = 255 - buf[i + 3];
        buf[i + 3] = buf[i + 2];
        buf[i + 2] = tmp;

        tmp = 255 - buf[i + 2];
        buf[i + 2] = buf[i + 3];
        buf[i + 3] = tmp;

        tmp = 255 - buf[i + 6];
        buf[i + 6] = buf[i + 7];
        buf[i + 7] = tmp;

        tmp = 255 - buf[i + 7];
        buf[i + 7] = buf[i + 6];
        buf[i + 6] = tmp;
    }
}

/**
 * @brief RTL-SDR asynchronous USB callback.
 * Converts incoming u8 I/Q to s16 and enqueues into the input ring. If
 * `offset_tuning` is off and `DSD_NEO_COMBINE_ROT` is enabled (default), a
 * combined rotate+widen implementation is used. Otherwise it falls back to
 * legacy two-pass (rotate_90 u8, then widen subtracting 128) or a simple
 * widen subtracting 127. On overflow, drops oldest ring data to avoid stalls.
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
    if (s->mute) {
        /* Clamp mute length to buffer size to avoid overwrite; carry remainder */
        int old = s->mute.load(std::memory_order_relaxed);
        if (old > 0) {
            uint32_t m = (uint32_t)old;
            if (m > len) {
                m = len;
            }
            memset(buf, 127, m);
            s->mute.fetch_sub((int)m, std::memory_order_relaxed);
        }
    }
    /* Convert incoming u8 I/Q and write directly into input ring without extra copy */
    size_t need = len;
    size_t done = 0;
    /* For legacy two-pass path, rotate the incoming byte buffer once up front */
    int use_two_pass = (!s->offset_tuning && !s->combine_rotate_enabled);
    if (use_two_pass) {
        rotate_90(buf, len);
    }
    while (need > 0) {
        int16_t *p1 = NULL, *p2 = NULL;
        size_t n1 = 0, n2 = 0;
        input_ring_reserve(s->input_ring, need, &p1, &n1, &p2, &n2);
        if (n1 == 0 && n2 == 0) {
            /* Ring still full after drop attempt; give up this callback to avoid stall */
            break;
        }
        /* Ensure even counts to keep I/Q pairs aligned */
        if (n1 & 1) {
            n1--;
        }
        size_t w1 = (n1 < need) ? n1 : need;
        size_t rem_after_w1 = need - w1;
        if (n2 & 1) {
            n2--;
        }
        size_t w2 = (n2 < rem_after_w1) ? n2 : rem_after_w1;

        if (!s->offset_tuning && s->combine_rotate_enabled) {
            if (w1) {
                widen_rotate90_u8_to_s16_bias127(buf + done, p1, (uint32_t)w1);
            }
            if (w2) {
                widen_rotate90_u8_to_s16_bias127(buf + done + w1, p2, (uint32_t)w2);
            }
        } else if (use_two_pass) {
            /* bytes already rotated in-place; widen with 128 subtraction to avoid bias */
            if (w1) {
                widen_u8_to_s16_bias128_scalar(buf + done, p1, (uint32_t)w1);
            }
            if (w2) {
                widen_u8_to_s16_bias128_scalar(buf + done + w1, p2, (uint32_t)w2);
            }
        } else {
            if (w1) {
                widen_u8_to_s16_bias127(buf + done, p1, (uint32_t)w1);
            }
            if (w2) {
                widen_u8_to_s16_bias127(buf + done + w1, p2, (uint32_t)w2);
            }
        }
        input_ring_commit(s->input_ring, w1 + w2);
        done += w1 + w2;
        need -= w1 + w2;
    }
}

/**
 * @brief RTL-SDR USB thread entry: reads samples asynchronously into the input ring.
 * Applies optional realtime scheduling/affinity if configured.
 *
 * @param arg Pointer to `rtl_device`.
 * @return NULL on exit.
 */
static void*
dongle_thread_fn(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    maybe_set_thread_realtime_and_affinity("DONGLE");
    rtlsdr_read_async(s->dev, rtlsdr_callback, s, 16, s->buf_len);
    return 0;
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
 * @brief Enable offset tuning on the tuner if supported.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success or a negative error code.
 */
static int
verbose_offset_tuning(rtlsdr_dev_t* dev) {
    int r;
    r = rtlsdr_set_offset_tuning(dev, 1);
    if (r != 0) {
        fprintf(stderr, "WARNING: Failed to set offset tuning.\n");
    } else {
        fprintf(stderr, "Offset tuning mode enabled.\n");
    }
    return r;
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
        fprintf(stderr, "Tuner bandwidth set to %u Hz.\n", bw_hz);
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
    const char* e = getenv("DSD_NEO_RTL_AGC");
    int want = 1;
    if (e && (*e == '0' || *e == 'n' || *e == 'N' || *e == 'f' || *e == 'F')) {
        want = 0;
    }
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

    dev->dev_index = dev_index;
    dev->input_ring = input_ring;
    dev->thread_started = 0;
    dev->mute = 0;
    dev->combine_rotate_enabled = combine_rotate_enabled_param;

    int r = rtlsdr_open(&dev->dev, (uint32_t)dev_index);
    if (r < 0) {
        fprintf(stderr, "Failed to open rtlsdr device %d.\n", dev_index);
        free(dev);
        return NULL;
    }

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
        if (dev->dev) {
            rtlsdr_cancel_async(dev->dev);
        }
        pthread_join(dev->thread, NULL);
        dev->thread_started = 0;
    }

    if (dev->dev) {
        rtlsdr_close(dev->dev);
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
    if (!dev || !dev->dev) {
        return -1;
    }
    dev->freq = frequency;
    return verbose_set_frequency(dev->dev, frequency);
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
    if (!dev || !dev->dev) {
        return -1;
    }
    dev->rate = samp_rate;
    return verbose_set_sample_rate(dev->dev, samp_rate);
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
    if (!dev || !dev->dev) {
        return -1;
    }

#define AUTO_GAIN -100
    dev->gain = gain;
    if (gain == AUTO_GAIN) {
        return verbose_auto_gain(dev->dev);
    } else {
        int nearest = nearest_gain(dev->dev, gain);
        return verbose_gain_set(dev->dev, nearest);
    }
}

int
rtl_device_get_tuner_gain(struct rtl_device* dev) {
    if (!dev || !dev->dev) {
        return -1;
    }
    return rtlsdr_get_tuner_gain(dev->dev);
}

int
rtl_device_is_auto_gain(struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    /* We track AUTO vs manual in the requested field. */
    return (dev->gain == AUTO_GAIN) ? 1 : 0;
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
    if (!dev || !dev->dev) {
        return -1;
    }
    dev->ppm_error = ppm_error;
    return verbose_ppm_set(dev->dev, ppm_error);
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
    if (!dev || !dev->dev) {
        return -1;
    }
    dev->direct_sampling = on;
    return verbose_direct_sampling(dev->dev, on);
}

/**
 * @brief Enable offset tuning mode.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_set_offset_tuning(struct rtl_device* dev) {
    if (!dev || !dev->dev) {
        return -1;
    }
    dev->offset_tuning = 1;
    return verbose_offset_tuning(dev->dev);
}

int
rtl_device_set_tuner_bandwidth(struct rtl_device* dev, uint32_t bw_hz) {
    if (!dev || !dev->dev) {
        return -1;
    }
    return verbose_set_tuner_bandwidth(dev->dev, bw_hz);
}

/**
 * @brief Reset device buffer.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_reset_buffer(struct rtl_device* dev) {
    if (!dev || !dev->dev) {
        return -1;
    }
    return verbose_reset_buffer(dev->dev);
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
    if (!dev || !dev->dev || dev->thread_started) {
        return -1;
    }

    dev->buf_len = buf_len;
    dev->thread_started = 1;

    int r = pthread_create(&dev->thread, NULL, dongle_thread_fn, dev);
    if (r != 0) {
        dev->thread_started = 0;
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

    /* Signal the asynchronous reader to stop, then join the thread */
    if (dev->dev) {
        rtlsdr_cancel_async(dev->dev);
    }
    pthread_join(dev->thread, NULL);
    dev->thread_started = 0;

    return 0;
}

/**
 * @brief Mute the device for a specified number of samples.
 *
 * @param dev RTL-SDR device handle.
 * @param samples Number of samples to mute.
 */
void
rtl_device_mute(struct rtl_device* dev, int samples) {
    if (!dev) {
        return;
    }
    dev->mute.store(samples);
}
