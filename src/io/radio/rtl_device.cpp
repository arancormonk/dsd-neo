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

#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/iq_replay_checkpoint.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/input_failure.h>
#include "rtl_thread.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <complex>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/dsp/iq_resampler.h>
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
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <errno.h>
#include <iterator>
#include <limits.h>
#include <mutex>
#include <new>
#if !DSD_PLATFORM_WIN_NATIVE
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include "airspy_source.h"
struct airspy_source;
#include "rtl_capture_phase.h"
#include "rtl_finite.h"
#include "rtl_perf.h"
#include "rtl_replay_device.h"
#include "rtl_stream_shared.hpp"
#include "rtl_usb_identity.h"
#include "soapy_profile.h"
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
#include "rtl_stream_test_support.h"
#include "rtl_test_hook.h"
#endif

#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
#include <excpt.h>
#endif
#if defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD)
/* USBDEVFS_RESET and the device descriptor read through the app's usbfs descriptor (#782). */
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
/* Some platforms (e.g. non-glibc) may not define MSG_NOSIGNAL */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {
enum RtlCaptureReconfigureState : unsigned char {
    RTL_CAPTURE_RECONFIGURE_INACTIVE = 0,
    RTL_CAPTURE_RECONFIGURE_ACTIVE = 1,
    RTL_CAPTURE_RECONFIGURE_RELEASING = 2,
};
} // namespace

/* rtl_device::recovery_request */
namespace {
enum RtlUsbRecoveryRequest : int {
    RTL_USB_RECOVERY_IDLE = 0,
    RTL_USB_RECOVERY_REQUESTED = 1,
    RTL_USB_RECOVERY_TAKEN = 2,
};
} // namespace

#ifdef USE_SOAPYSDR
#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Types.hpp>
#include <exception>
#include <stdexcept>

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

/* What the stand-ins below answer: a failure, and no reading. Each makes a round trip through a volatile object, as
 * <dsd-neo/platform/fp_opaque.h> does, so like a real librtlsdr call's result it is a runtime value, not a constant
 * the code checking it can be judged against: the checks are the same runtime checks in every build. */
static int
rtl_sdr_unavailable_status(void) {
    volatile int status = -1;
    return status;
}

static uint32_t
rtl_sdr_unavailable_reading(void) {
    volatile uint32_t reading = 0U;
    return reading;
}

static int
rtlsdr_open(rtlsdr_dev_t** dev, uint32_t index) {
    (void)dev;
    (void)index;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_close(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_read_async(rtlsdr_dev_t* dev, void (*cb)(unsigned char*, uint32_t, void*), void* ctx, uint32_t buf_num,
                  uint32_t buf_len) {
    (void)dev;
    (void)cb;
    (void)ctx;
    (void)buf_num;
    (void)buf_len;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_cancel_async(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t* dev, int manual) {
    (void)dev;
    (void)manual;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_get_tuner_gains(rtlsdr_dev_t* dev, int* gains) {
    (void)dev;
    (void)gains;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_center_freq(rtlsdr_dev_t* dev, uint32_t freq) {
    (void)dev;
    (void)freq;
    return rtl_sdr_unavailable_status();
}

static uint32_t
rtlsdr_get_center_freq(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtl_sdr_unavailable_reading();
}

static int
rtlsdr_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate) {
    (void)dev;
    (void)rate;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_direct_sampling(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtl_sdr_unavailable_status();
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
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_agc_mode(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_tuner_gain(rtlsdr_dev_t* dev, int gain) {
    (void)dev;
    (void)gain;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_freq_correction(rtlsdr_dev_t* dev, int ppm) {
    (void)dev;
    (void)ppm;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_reset_buffer(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtl_sdr_unavailable_status();
}

static uint32_t
rtlsdr_get_sample_rate(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtl_sdr_unavailable_reading();
}

static int
rtlsdr_get_tuner_gain(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_offset_tuning(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_get_offset_tuning(rtlsdr_dev_t* dev) {
    (void)dev;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_xtal_freq(rtlsdr_dev_t* dev, uint32_t rtl, uint32_t tuner) {
    (void)dev;
    (void)rtl;
    (void)tuner;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_testmode(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtl_sdr_unavailable_status();
}

static int
rtlsdr_set_tuner_if_gain(rtlsdr_dev_t* dev, int stage, int gain) {
    (void)dev;
    (void)stage;
    (void)gain;
    return rtl_sdr_unavailable_status();
}

#ifdef USE_RTLSDR_BIAS_TEE
static int
rtlsdr_set_bias_tee(rtlsdr_dev_t* dev, int on) {
    (void)dev;
    (void)on;
    return rtl_sdr_unavailable_status();
}
#endif
#endif

namespace {
enum RtlBackend : unsigned char {
    RTL_BACKEND_USB = 0,
    RTL_BACKEND_TCP = 1,
    RTL_BACKEND_SOAPY = 2,
    RTL_BACKEND_IQ_REPLAY = 3,
    RTL_BACKEND_AIRSPY = 4,
};
} // namespace

namespace {
enum SoapySampleFormat : unsigned char {
    SOAPY_FMT_NONE = 0,
    SOAPY_FMT_CF32 = 1,
    SOAPY_FMT_CS16 = 2,
};
} // namespace

static const int RTL_AUTO_GAIN = -100;

/* librtlsdr's LO displacement while offset tuning is on, from the sample rate the dongle holds; it recomputes it
 * whenever the rate changes. */
static uint32_t
rtl_usb_offs_for_rate(uint32_t rate) {
    return (rate / 2U) * 170U / 100U;
}

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

/* What a USB dongle took: each value only once the hardware acknowledged it, with a flag for each control that ever
 * took. A USB recovery replays it onto the reopened dongle (rtl_device_usb_recover_handle()); a control that never
 * took is left at the fresh handle's default, and a failed change never reaches it. Written by the USB setters under
 * usb_ctrl_lock; read by the recovery while every setter is refused. */
namespace {
struct rtl_if_gain {
    int stage;
    int gain;
};

struct rtl_usb_effective {
    uint32_t freq, rate, tuner_bw; /* rate as requested: librtlsdr derives the same real rate from it again */
    int gain_mode;                 /* 1 automatic, 0 manual */
    int gain, digital_agc, ppm, direct_sampling, offset_requested, offset_took, testmode;
    uint32_t rtl_xtal, tuner_xtal;
    int if_gain_count;
    struct rtl_if_gain if_gains[16];

    int freq_took, rate_took, tuner_bw_took, gain_took, ppm_took, direct_sampling_took, testmode_took, xtal_took;
    int gain_unknown; /* a failed gain change could not be rolled back: the recorded gain is re-applied */
#ifdef USE_RTLSDR_BIAS_TEE
    int bias_tee, bias_tee_took; /* only a librtlsdr with a bias tee control records it */
#endif
};

/* One rtl_tcp connection (issue #518 WP3): its socket, the identity a command and its completion are bound to, and
 * the lock that keeps one command (or one multi-command gain change) whole on the wire. Counted: the device holds a
 * reference while the connection is published or handed off, the reader one while it reads it, a sender one while it
 * sends; the last to let go closes the socket, so no thread uses a descriptor another closed. */
struct rtl_tcp_conn {
    dsd_socket_t sock = DSD_INVALID_SOCKET;
    uint64_t generation = 0U;
    dsd_mutex_t tx_lock;
    std::atomic<int> refs;
    /* Taken down (a failed command, a stop): its reader stops reading it at once. A shutdown alone does not: what
     * the socket already holds (up to its receive buffer) still reads after it, and would reach the rings after the
     * loss. */
    std::atomic<int> lost;
};

/* Which controls an rtl_tcp server took (D6), beside the values the device keeps for every backend (freq, rate, gain,
 * agc_mode, digital_agc_wanted, ppm_error, ...): set only once the command went out on the connection that was still
 * the published one. A reconnect replays every control that took, as last taken. */
struct rtl_tcp_took {
    int freq, rate, gain, ppm, direct_sampling, offset_tuning, bias_tee, testmode, xtal;
    /* A gain (or test mode) change failed after part of it may have reached the server, which may then disagree with
     * the record: the replay asserts the record's gain state (or test mode) even if none ever took, as a USB recovery
     * does for gain_state_unknown. */
    int gain_unknown, testmode_unknown;
};
} // namespace

// Internal RTL device structure
struct rtl_device {
    /* The librtlsdr handle. Published (written) only with usb_lifecycle_lock and usb_ctrl_lock both held, so it is
     * read under either; every control call and cancel uses it under usb_ctrl_lock. The reader thread reads it without
     * a lock: a reader starts after the handle is published and is joined before it is retired. */
    rtlsdr_dev_t* dev = nullptr;
    airspy_source* airspy = nullptr;
    /* Set when this device wrapped the app-supplied USB descriptor, so closing it
     * knows to hand the descriptor back to its owner. See
     * rtl_device_preopened_fd_in_use(). */
    int owns_preopened_fd = 0;
    /* ... that descriptor, and the publish it was taken at (rtl_preopened_fd_take()); meaningful only while
     * owns_preopened_fd is set. Written at open, before any reader or monitor starts. */
    int preopened_fd = -1;
    uint32_t preopened_generation = 0U;
    /* The last stall recovery reopened that descriptor without a reset (the kernel refused it), so a stall that
     * persists says so (rtl_usb_recovery_refusal()). Written by the swap, read by the progress monitor. */
    std::atomic<int> reopened_without_reset{0};
    dsd_thread_t thread{};
    struct input_ring_state* input_ring = nullptr;
    struct dsd_iq_capture_writer* iq_capture_writer = nullptr;
    std::atomic<uint64_t> capture_mute_pending_bytes{0U};
    uint64_t replay_initial_center_frequency_hz = 0U;
    uint64_t replay_initial_capture_center_frequency_hz = 0U;
    dsd_iq_replay_source* replay_src = nullptr;
    uint64_t replay_float_elements_written = 0U;
    /* A capture read that failed after part of a chunk was read: replay_read_exact() hands over that part and returns
       the failure on its next call. Replay reader thread only. */
    int replay_deferred_read_rc = 0;
    /* A replay whose capture rate cannot reach the mode's DSP rate is resampled to replay_cfg.dsp_input_rate_hz before
       the demodulator (issue #701). Made at open, before the reader starts, freed after it is joined; only the reader
       thread uses it between. NULL when the capture runs at its own rate. */
    dsd_iq_resampler* replay_resampler = nullptr;
    /* Where the reader stood after the last capture chunk it handed the demodulator (issue #701): a run-time re-plan
       reopens the replay there (rtl_device_replay_checkpoint()). Written by the reader thread only, read once it is
       joined; at open, the point the replay opened at. */
    dsd_iq_replay_checkpoint replay_committed{};
    /* SoapySDR backend */
    SoapySDR::Device* soapy_dev = nullptr;
    SoapySDR::Stream* soapy_stream = nullptr;
    /* Written by the reader, read by its progress monitor for the silence verdict's cause. */
    std::atomic<uint64_t> soapy_overflow_count{0U};
    std::atomic<uint64_t> soapy_timeout_count{0U};
    uint64_t soapy_read_errors = 0U;
    uint64_t soapy_last_overflow_log_ns = 0U;
    int soapy_profile_id = 0;
    int soapy_requested_bandwidth_hz = 0;
    uint32_t soapy_startup_center_freq_hz = 0U;
    /* Memoized rtl_device_nearest_supported_rate answer; valid while the device configuration
       (and therefore its rate grid) is unchanged. actual == 0 means empty. */
    uint32_t soapy_nearest_rate_cache_requested = 0U;
    uint32_t soapy_nearest_rate_cache_actual = 0U;
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
    /* Set while a control or setup transaction holds soapy_lock (never a read): the progress monitor does not count
     * that time as silence, since the reader cannot read meanwhile. */
    std::atomic<int> soapy_control_hold{0};
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
    /* Hardware offset tuning is on and displaces the LO: the capture is centred, so nothing rotates it by Fs/4. Set
     * only when the device took it (librtlsdr refuses it on R82xx tuners and in direct sampling). */
    int offset_took = 0;
    int direct_sampling = 0;
    /* Tuner bandwidth a USB dongle last took (0: driver automatic). */
    uint32_t tuner_bw_hz = 0U;
    std::atomic<int> mute{0};
    /* Sample progress: counted first thing by every librtlsdr callback with bytes, and by every SoapySDR read with
     * samples, read by the progress monitor. (An Airspy's count is its source's, airspy_source_progress().) */
    std::atomic<uint64_t> progress{0U};
    int thread_started = 0;
    std::atomic<uint32_t> capture_retune_count{0U};
    /* Moves before a USB recovery reopens the dongle and before an rtl_tcp reconnect replays the server's settings:
       rtl_device_capture_settings_generation(). */
    std::atomic<uint32_t> capture_settings_generation{0U};
    std::atomic<int> capture_reconfigure_hold{0};
    /* Backend selector: 0 = USB (librtlsdr), 1 = rtl_tcp, 2 = SoapySDR */
    int backend = 0;
    int live_combine_rotate_enabled = 1;
    int replay_fs4_shift_enabled = 0;
    int replay_historical_cu8_two_pass = 0;
    uint32_t replay_initial_sample_rate_hz = 0U;
    uint32_t replay_initial_freq = 0U;
    uint32_t replay_initial_rate = 0U;
    int replay_has_eof_state = 0;
    int soapy_lock_inited = 0;
    int soapy_format = 0; /* SOAPY_FMT_* */
    uint32_t soapy_mtu_elems = 0U;
    int rot_phase = 0;                   /* persistent j^n phase in [0..3] for capture-side FS/4 rotation */
    std::atomic<int> mute_byte_phase{0}; /* byte carry while an active mute span is discarded in fragments */
    /* rtl_tcp: the server's port, and whether tcp_lock and tcp_cond (below) were made. */
    int port = 0;
    int tcp_sync_inited = 0;
    std::atomic<int> run{0};
    /* The USB reader thread's progress, reset by each start: entered before it checks run and calls
     * rtlsdr_read_async(), exited once that returned (or was skipped). A stop cancels until it sees exited. The
     * SoapySDR reader's: entered once its stream is active, exited once it left its read loop (or never got there). */
    std::atomic<int> reader_entered{0};
    std::atomic<int> reader_exited{0};
    /* Taken by the verdict that ends the session for its receiver, so a reader exit and the progress monitor that race
     * report it once. Reset by each start. */
    std::atomic<int> receiver_end_claimed{0};
    /* The sample-progress monitor: the RTL-SDR's or the SoapySDR device's own thread (the Airspy's is the device
     * thread), the flag that stops it, and the lock and condition a stop wakes either with. The lock and condition are
     * made by the first start and freed with the device. */
    dsd_thread_t progress_monitor{};
    int progress_monitor_started = 0;
    std::atomic<int> progress_monitor_run{0};
    dsd_mutex_t monitor_lock{};
    dsd_cond_t monitor_wake{};
    int monitor_sync_inited = 0;
    int agc_mode = 0; /* cached for TCP backend */
    int bias_tee_on = 0;
    int tcp_autotune = 0; /* adaptive recv/buffering */
    int stats_enabled = 0;
    int tcp_metrics_lock_inited = 0;
    /* Extra driver state for reconnect replay */
    int testmode_on = 0;
    /* USB gain controls, beside gain and agc_mode: the RTL2832U digital AGC as wanted (on only with automatic gain,
     * when the runtime config enables it; register 0x19 carries it and test mode both), whether a gain state ever
     * took, and whether a failed gain change could not be rolled back, so the dongle holds neither state. */
    int digital_agc_wanted = 0;
    int gain_took = 0;
    int gain_state_unknown = 0;
    uint32_t rtl_xtal_hz = 0U;
    uint32_t tuner_xtal_hz = 0U;
    int if_gain_count = 0;
    /* USB recovery (rtl_device_usb_recover_handle()). usb_lifecycle_lock serialises the swap with stop, destroy and
     * the reader's restart; usb_ctrl_lock covers every use of the handle and the controls' record. Order:
     * usb_lifecycle_lock, then usb_ctrl_lock, never the reverse; neither is taken on the sample path. */
    dsd_mutex_t usb_lifecycle_lock{};
    mutable dsd_mutex_t usb_ctrl_lock{};
    int usb_locks_inited = 0;
    /* Set (under usb_ctrl_lock) while a swap runs: setters return RTL_DEVICE_EBUSY and change nothing. */
    std::atomic<int> recovering{0};
    /* Latched by stop and destroy before anything else: the swap gives up and the reader never restarts. */
    std::atomic<int> stop_requested{0};
    /* The USB device the dongle was opened on, and whether it can be reset and reopened: set at open, never cleared;
     * the reason when not. The identity is re-resolved after a reopen that found the dongle on another port. */
    struct rtl_usb_identity identity{};
    std::atomic<int> usb_recovery_on{0};
    char usb_recovery_off_reason[128] = "";
    struct rtl_usb_effective usb_eff{};
    /* A restarted reader's starting point: the progress count when it started, and a sequence bumped each time, so the
     * progress monitor can watch it afresh. */
    std::atomic<uint64_t> progress_baseline{0U};
    std::atomic<uint32_t> watch_rearm_seq{0U};
    /* A stall recovery request (D1, D2): the progress monitor posts it through the stream's callback (REQUESTED), the
     * stream's controller takes it (TAKEN) and finishes it (IDLE), or the monitor withdraws one not taken in time
     * (IDLE). recovery_epoch moves with each post, so a receiver verdict made before it stays silent. The callback is
     * set and read under usb_ctrl_lock and called outside it. */
    std::atomic<int> recovery_request{0};
    std::atomic<uint32_t> recovery_epoch{0U};
    /* Which USB reader is which: reader_incarnation moves before each reader thread starts, and the thread keeps the
     * value it found. recovery_handoff carries the reader a recovery restarted (its incarnation, high 32 bits) and
     * where its return stands (rtl_usb_handoff_state, low 32 bits); every transition is one compare-and-swap, so the
     * restarted reader and the recovery's finish agree on who reports its return (rtl_usb_recovery_reader_handoff(),
     * rtl_device_usb_recovery_finish()). recovery_reader_rc is that return's result, written by the one reader thread
     * there is before its handoff. */
    std::atomic<uint32_t> reader_incarnation{0U};
    std::atomic<uint64_t> recovery_handoff{0U};
    std::atomic<int> recovery_reader_rc{0};
    std::atomic<uint64_t> recovery_silence_ms{0U};
    rtl_device_recovery_cb recovery_cb = nullptr;
    void* recovery_cb_ctx = nullptr;
    /* rtl_tcp (issue #518 WP3). tcp_lock guards the published connection, the hand-off and the effective record (the
     * values above with tcp_took); a command is sent under its connection's tx_lock, never under tcp_lock, and records
     * what took under both (tx_lock first). tcp_connected mirrors "a connection is published" for lock-free reads. */
    mutable dsd_mutex_t tcp_lock{};
    dsd_cond_t tcp_cond{};
    struct rtl_tcp_conn* tcp_conn = nullptr;
    /* A reconnected connection on its way to publication: the reader posts it (QUEUED), the controller takes it
     * (CLAIMED) and publishes it or gives it up (FAILED, CANCELLED), then releases the reader (RELEASED) until the
     * reader acknowledges (NONE). Admission closes for good once the device stops. */
    struct rtl_tcp_conn* tcp_handoff_conn = nullptr;
    uint64_t tcp_generation = 0U;
    std::atomic<int> tcp_connected{0};
    /* The tuner the server's header named (RTLSDR_TUNER_*), at the first connection and at each reconnect:
       rtl_device_capture_retune_resets(). */
    std::atomic<int> tcp_tuner_type{RTLSDR_TUNER_UNKNOWN};
    int tcp_handoff = 0;
    int tcp_admission_closed = 0;
    /* A retune went out: the reader drops what it buffered for the old frequency (rtl_tcp_drop_buffered_for_retune()),
     * which it owns, before its next chunk. */
    std::atomic<int> tcp_restart_pending{0};
    struct rtl_tcp_took tcp_took{};
    struct rtl_capture_u8_byte_carry iq_byte_carry{}; /* one buffered raw byte when a chunk ends mid-I/Q sample */

    struct rtl_if_gain if_gains[16] = {};

    char host[1024] = {};
};

/* The USB locks (see struct rtl_device): usb_lifecycle_lock, then usb_ctrl_lock, never the reverse. */
static void
rtl_usb_lifecycle_lock(struct rtl_device* dev) {
    if (dev->usb_locks_inited) {
        (void)dsd_mutex_lock(&dev->usb_lifecycle_lock);
    }
}

static void
rtl_usb_lifecycle_unlock(struct rtl_device* dev) {
    if (dev->usb_locks_inited) {
        (void)dsd_mutex_unlock(&dev->usb_lifecycle_lock);
    }
}

static void
rtl_usb_ctrl_lock(const struct rtl_device* dev) {
    if (dev->usb_locks_inited) {
        (void)dsd_mutex_lock(&dev->usb_ctrl_lock);
    }
}

static void
rtl_usb_ctrl_unlock(const struct rtl_device* dev) {
    if (dev->usb_locks_inited) {
        (void)dsd_mutex_unlock(&dev->usb_ctrl_lock);
    }
}

/* Begin a USB control: usb_ctrl_lock held and 0 returned, or the lock released and RTL_DEVICE_EBUSY while a recovery
 * swaps the handle (nothing changes), -1 without a handle. rtl_usb_control_end() ends it. */
static int
rtl_usb_control_begin(struct rtl_device* dev) {
    rtl_usb_ctrl_lock(dev);
    if (dev->recovering.load(std::memory_order_acquire)) {
        rtl_usb_ctrl_unlock(dev);
        return RTL_DEVICE_EBUSY;
    }
    if (!dev->dev) {
        rtl_usb_ctrl_unlock(dev);
        return -1;
    }
    return 0;
}

static void
rtl_usb_control_end(const struct rtl_device* dev) {
    rtl_usb_ctrl_unlock(dev);
}

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
    return (s && !s->offset_took && !disable_fs4_shift) ? 1 : 0;
}

static inline void
rtl_get_u8_transform_policy(const struct rtl_device* s, int* out_fs4_active, int* out_combine_active,
                            int* out_use_two_pass) {
    int fs4_active = 0;
    int combine_active = 0;
    if (s && s->backend == RTL_BACKEND_IQ_REPLAY) {
        fs4_active = s->replay_fs4_shift_enabled ? 1 : 0;
        combine_active = s->replay_historical_cu8_two_pass ? 0 : 1;
    } else {
        fs4_active = fs4_shift_capture_active(s);
        combine_active = (s && s->live_combine_rotate_enabled) ? 1 : 0;
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

/* Persisted IQ metadata and the live transform policy may select the two-pass
 * CU8 transform. New captures use the combined bias-127.5 transform by default. */
static inline void
rtl_cu8_moments_add_sample(dsd_input_level_cu8_moments* moments, unsigned char sample) {
    if (!moments) {
        return;
    }
    moments->count++;
    moments->sum += (uint64_t)sample;
    moments->sum_sq += (uint64_t)sample * (uint64_t)sample;
    moments->clipped += (sample <= 1U || sample >= 254U) ? 1U : 0U;
    if (sample < moments->min_sample) {
        moments->min_sample = sample;
    }
    if (sample > moments->max_sample) {
        moments->max_sample = sample;
    }
}

static uint32_t
rtl_apply_two_pass_cu8_rotation(unsigned char* buf, uint32_t len, uint32_t phase,
                                dsd_input_level_cu8_moments* moments) {
    uint32_t cur_phase = phase & 3U;
    if (!buf || len < 2U) {
        return cur_phase;
    }
    dsd_input_level_cu8_moments local;
    if (moments) {
        dsd_input_level_cu8_moments_reset(&local);
    }
    const uint32_t pairs = len >> 1;
    for (uint32_t n = 0; n < pairs; n++) {
        const uint32_t idx = n << 1;
        const unsigned char in_i = buf[idx];
        const unsigned char in_q = buf[idx + 1U];
        if (moments) {
            rtl_cu8_moments_add_sample(&local, in_i);
            rtl_cu8_moments_add_sample(&local, in_q);
        }
        switch (cur_phase) {
            case 0: break;
            case 1:
                buf[idx] = (unsigned char)(255U - (uint32_t)in_q);
                buf[idx + 1U] = in_i;
                break;
            case 2:
                buf[idx] = (unsigned char)(255U - (uint32_t)in_i);
                buf[idx + 1U] = (unsigned char)(255U - (uint32_t)in_q);
                break;
            default:
                buf[idx] = in_q;
                buf[idx + 1U] = (unsigned char)(255U - (uint32_t)in_i);
                break;
        }
        cur_phase = (cur_phase + 1U) & 3U;
    }
    if (moments && (len & 1U) != 0U) {
        rtl_cu8_moments_add_sample(&local, buf[len - 1U]);
    }
    if (moments) {
        (void)dsd_input_level_cu8_moments_merge(moments, &local);
    }
    return cur_phase;
}

static void
rtl_widen_two_pass_cu8(const unsigned char* src, float* dst, uint32_t len) {
    if (!src || !dst || len == 0U) {
        return;
    }
    const float inv = 1.0f / 127.5f;
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = ((float)src[i] - 128.0f) * inv;
    }
}

static inline int
rtl_process_u8_chunk(const struct rtl_device* s, unsigned char* src, float* dst, size_t len, int fs4_shift_active,
                     int combine_rotate_active, int use_two_pass, int* phase, dsd_input_level_cu8_moments* moments) {
    if (!s || !src || !dst || len == 0) {
        return phase ? *phase : 0;
    }
    int cur_phase = phase ? (*phase & 3) : 0;
    if (fs4_shift_active && combine_rotate_active) {
        cur_phase = moments ? (int)widen_rotate90_u8_to_f32_bias127_phase_moments(src, dst, (uint32_t)len,
                                                                                  (uint32_t)cur_phase, moments)
                            : (int)widen_rotate90_u8_to_f32_bias127_phase(src, dst, (uint32_t)len, (uint32_t)cur_phase);
    } else if (use_two_pass) {
        cur_phase = (int)rtl_apply_two_pass_cu8_rotation(src, (uint32_t)len, (uint32_t)cur_phase, moments);
        rtl_widen_two_pass_cu8(src, dst, (uint32_t)len);
    } else if (moments) {
        widen_u8_to_f32_bias127_moments(src, dst, (uint32_t)len, moments);
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
    dsd_input_level_cu8_moments* moments;
};

} // namespace

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
static std::atomic<int> g_rtl_test_force_u8_generation_stale{0};
#endif

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
                             cursor->use_two_pass, cursor->phase, NULL);
        if (cursor->moments) {
            (void)dsd_input_level_cu8_moments_accumulate(cursor->moments, cursor->src + *cursor->done, prefix);
        }
        *cursor->done += prefix;
        *cursor->need -= prefix;
        from_prefix = 2U;
    }
    if (produced_bytes > from_prefix) {
        size_t body = produced_bytes - from_prefix;
        rtl_process_u8_chunk(s, cursor->src + *cursor->done, dst + from_prefix, body, cursor->fs4_shift_active,
                             cursor->combine_rotate_active, cursor->use_two_pass, cursor->phase, cursor->moments);
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
    dsd_input_level_cu8_moments* moments;
};

struct rtl_u8_perf_state {
    int enabled;
    uint64_t start_ns;
    uint64_t drops_before;
};

} // namespace

static inline void
rtl_finalize_u8_ring_exhaustion(struct rtl_device* s, const rtl_u8_finalize_state* final_state) {
    if (final_state->moments && final_state->need > 0U) {
        (void)dsd_input_level_cu8_moments_accumulate(final_state->moments, final_state->src + final_state->done,
                                                     final_state->need);
    }
    size_t dropped =
        rtl_drop_u8_bytes_preserve_alignment(final_state->src + final_state->done, final_state->need,
                                             final_state->carry, final_state->phase, final_state->fs4_shift_active);
    rtl_accumulate_ring_drops(s->input_ring, dropped);
    if (final_state->count_full_reserve) {
        s->reserve_full_events++;
    }
}

static inline void
rtl_finalize_u8_trailing_carry(const rtl_u8_finalize_state* final_state) {
    if (final_state->need != 1U || final_state->done >= final_state->len || final_state->carry->valid) {
        return;
    }
    if (final_state->moments) {
        (void)dsd_input_level_cu8_moments_accumulate(final_state->moments, final_state->src + final_state->done, 1U);
    }
    rtl_capture_u8_byte_carry_save(final_state->carry, final_state->src[final_state->done]);
}

static inline void
rtl_finalize_u8_ring_write(struct rtl_device* s, const rtl_u8_finalize_state* final_state) {
    if (!s || !final_state || !final_state->src || !final_state->carry || !final_state->phase) {
        return;
    }

    if (final_state->generation_stale) {
        rtl_clear_capture_alignment_after_discard(s, final_state->carry);
        return;
    }
    if (final_state->ring_exhausted) {
        rtl_finalize_u8_ring_exhaustion(s, final_state);
    } else {
        rtl_finalize_u8_trailing_carry(final_state);
    }

    s->iq_byte_carry = *final_state->carry;
    if (final_state->fs4_shift_active) {
        s->rot_phase = *final_state->phase;
    }
}

static inline rtl_u8_perf_state
rtl_u8_perf_begin(const struct rtl_device* s) {
    rtl_u8_perf_state perf = {};
    perf.enabled = rtl_perf_enabled();
    if (perf.enabled) {
        perf.start_ns = dsd_realtime_mono_ns();
        perf.drops_before = s->input_ring->producer_drops.load(std::memory_order_relaxed);
    }
    return perf;
}

static inline void
rtl_u8_perf_end(const struct rtl_device* s, const rtl_u8_perf_state* perf, size_t done) {
    if (!perf->enabled) {
        return;
    }
    uint64_t drops_after = s->input_ring->producer_drops.load(std::memory_order_relaxed);
    uint64_t drops_delta = (drops_after >= perf->drops_before) ? (drops_after - perf->drops_before) : 0ULL;
    rtl_perf_record_ingest(dsd_realtime_mono_ns() - perf->start_ns, done, drops_delta);
}

static inline int
rtl_u8_write_result(int ring_exhausted, int generation_stale) {
    return generation_stale ? 2 : ring_exhausted;
}

static int
rtl_write_u8_to_ring(struct rtl_device* s, unsigned char* src, size_t len, int fs4_shift_active, int use_two_pass,
                     int combine_rotate_active, int count_full_reserve, dsd_input_level_cu8_moments* moments) {
    if (!s || !s->input_ring || !src || len == 0) {
        return 0;
    }

    rtl_u8_perf_state perf = rtl_u8_perf_begin(s);
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
            src, &done, &need, &carry, &phase, fs4_shift_active, combine_rotate_active, use_two_pass, moments};
        rtl_write_u8_reserved_segment(s, &cursor, p1, w1);
        rtl_write_u8_reserved_segment(s, &cursor, p2, w2);

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
        if (g_rtl_test_force_u8_generation_stale.exchange(0, std::memory_order_acq_rel) != 0) {
            input_ring_request_discard(s->input_ring);
        }
#endif
        if (!input_ring_discard_generation_matches(s->input_ring, discard_generation)) {
            if (moments && need > 0U) {
                (void)dsd_input_level_cu8_moments_accumulate(moments, src + done, need);
            }
            generation_stale =
                rtl_handle_u8_ring_generation_stale(s, src, done, need, &carry, &phase, fs4_shift_active, produced);
            break;
        }
        input_ring_commit(s->input_ring, produced);
    }

    rtl_u8_finalize_state final_state = {
        src,    len, done, need, &carry, &phase, fs4_shift_active, count_full_reserve, ring_exhausted, generation_stale,
        moments};
    rtl_finalize_u8_ring_write(s, &final_state);
    rtl_u8_perf_end(s, &perf, done);
    return rtl_u8_write_result(ring_exhausted, generation_stale);
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
rtl_publish_cu8_input_level_moments(const struct rtl_device* s, const dsd_input_level_cu8_moments* moments) {
    dsd_input_level_snapshot snapshot;
    if (!s || !moments || moments->count == 0U) {
        return;
    }
    if (dsd_input_level_cu8_moments_finalize(moments, rtl_u8_input_level_source(s), &snapshot) == 0) {
        rtl_stream_input_level_publish(&snapshot);
    }
}

static inline void
rtl_publish_cf32_input_level_source(const float* samples, size_t count, dsd_input_level_source source) {
    dsd_input_level_snapshot snapshot;
    if (!samples || count == 0U) {
        return;
    }
    if (dsd_input_level_metrics_from_cf32(samples, count, source, &snapshot) == 0) {
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
    rtl_publish_cf32_input_level_source(samples, count, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32);
}
#endif

static dsd_input_level_source
replay_cf32_level_source(const rtl_device* s) {
    return strcmp(s->replay_cfg.source_backend, "airspy") == 0 ? DSD_INPUT_LEVEL_SOURCE_AIRSPY_CF32
                                                               : DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32;
}

static inline int
rtl_prepare_replay_input_level_snapshot(const struct rtl_device* s, const uint8_t* raw_block, size_t raw_bytes,
                                        float* scratch_f32, size_t scratch_cap_f32, dsd_input_level_snapshot* out) {
    if (!s || !raw_block || raw_bytes == 0U || !out) {
        return -1;
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CU8) {
        return dsd_input_level_metrics_from_cu8(raw_block, raw_bytes, rtl_u8_input_level_source(s), out);
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CS16) {
        if ((raw_bytes & 3U) != 0U) {
            return -1;
        }
        size_t sample_count = raw_bytes / sizeof(int16_t);
        if (sample_count == 0U) {
            return -1;
        }
        std::vector<int16_t> samples(sample_count);
        DSD_MEMCPY(samples.data(), raw_block, sample_count * sizeof(int16_t));
        return dsd_input_level_metrics_from_cs16(samples.data(), sample_count, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16, out);
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CF32) {
        if (!scratch_f32 || (raw_bytes & 7U) != 0U
            || strcmp(s->replay_cfg.capture_stage, "post_driver_cf32_pre_ring") != 0) {
            return -1;
        }
        size_t float_count = raw_bytes / sizeof(float);
        if (float_count == 0U || float_count > scratch_cap_f32) {
            return -1;
        }
        DSD_MEMCPY(scratch_f32, raw_block, float_count * sizeof(float));
        return dsd_input_level_metrics_from_cf32(scratch_f32, float_count, replay_cf32_level_source(s), out);
    }
    return -1;
}

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
    if (s->backend == RTL_BACKEND_AIRSPY) {
        return 2U * sizeof(float);
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

/*
 * A SoapySDR driver's CF32 buffer and a cf32 I/Q replay hand the decoder floats nothing has checked, and one Inf or NaN
 * among them poisons every IIR stage downstream until the next retune, so every component enters bounded: Inf, NaN and
 * magnitudes of 2^60 or more as 0 (bound_cf32_to_f32() in <dsd-neo/dsp/simd_widen.h>). With @p apply_rot set and
 * @p phase given, sample k is also rotated at fs/4 phase *phase + k, as rtl_apply_j4_rotation() rotates it, and *phase
 * is advanced past them. Airspy skips it: libairspy converts 12-bit ADC integers, which are always finite.
 */
static inline void
rtl_cf32_copy_bounded(float* dst, const void* src, size_t elem_count, int apply_rot, int* phase) {
    if (apply_rot && phase) {
        *phase = static_cast<int>(
            bound_rotate90_cf32_to_f32_phase(src, dst, elem_count, static_cast<uint32_t>(*phase) & 3U));
    } else {
        bound_cf32_to_f32(src, dst, elem_count);
    }
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

/* Copies one ring segment. With @p bounded set (a SoapySDR driver's CF32 buffer), every component enters bounded, as
 * rtl_cf32_copy_bounded() describes; Airspy's samples, converted from ADC integers, are copied as they are. */
static inline void
soapy_copy_cf32_samples(float* dst, const float* src, size_t elem_count, int apply_rot, int* phase, int bounded) {
    if (bounded) {
        rtl_cf32_copy_bounded(dst, src, elem_count, apply_rot, phase);
        return;
    }
    if (!dst || !src || elem_count == 0U) {
        return;
    }
    for (size_t i = 0U; i < elem_count; i++) {
        float i_in = src[2 * i];
        float q_in = src[2 * i + 1];
        if (apply_rot && phase) {
            rtl_apply_j4_rotation(i_in, q_in, *phase, &dst[(i * 2U) + 0U], &dst[(i * 2U) + 1U]);
            *phase = (*phase + 1) & 3;
        } else {
            dst[(i * 2U) + 0U] = i_in;
            dst[(i * 2U) + 1U] = q_in;
        }
    }
}

#ifdef USE_SOAPYSDR
static inline void
soapy_copy_cs16_samples(float* dst, const int16_t* src_iq, size_t elem_count, float scale, int apply_rot, int* phase) {
    if (!dst || !src_iq || elem_count == 0U) {
        return;
    }
    if (!apply_rot || !phase) {
        for (size_t k = 0U; k < elem_count * 2U; k++) {
            dst[k] = (float)src_iq[k] * scale;
        }
        return;
    }
    /* Sample i is rotated at phase *phase + i (rtl_apply_j4_rotation()): one at a time up to the next phase-0 sample,
     * then four at a time, each at its fixed phase, so the loop carries no per-sample phase switch, then the rest. */
    int p = *phase & 3;
    size_t i = 0U;
    for (; i < elem_count && p != 0; i++) {
        rtl_apply_j4_rotation((float)src_iq[2U * i] * scale, (float)src_iq[(2U * i) + 1U] * scale, p, &dst[2U * i],
                              &dst[(2U * i) + 1U]);
        p = (p + 1) & 3;
    }
    for (; i + 4U <= elem_count; i += 4U) {
        const int16_t* s = src_iq + (2U * i);
        float* d = dst + (2U * i);
        d[0] = (float)s[0] * scale; /* phase 0: (I, Q) */
        d[1] = (float)s[1] * scale;
        d[2] = -((float)s[3] * scale); /* phase 1: (-Q, I) */
        d[3] = (float)s[2] * scale;
        d[4] = -((float)s[4] * scale); /* phase 2: (-I, -Q) */
        d[5] = -((float)s[5] * scale);
        d[6] = (float)s[7] * scale; /* phase 3: (Q, -I) */
        d[7] = -((float)s[6] * scale);
    }
    for (; i < elem_count; i++) {
        rtl_apply_j4_rotation((float)src_iq[2U * i] * scale, (float)src_iq[(2U * i) + 1U] * scale, p, &dst[2U * i],
                              &dst[(2U * i) + 1U]);
        p = (p + 1) & 3;
    }
    *phase = p;
}
#endif

static size_t
soapy_write_cf32_to_ring(struct rtl_device* s, const float* src, size_t num_elems, int apply_rot, int bounded) {
    if (!s || !s->input_ring || !src || num_elems == 0) {
        return 0;
    }
    int perf_on = rtl_perf_enabled();
    uint64_t perf_t0 = perf_on ? dsd_realtime_mono_ns() : 0ULL;
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
        soapy_copy_cf32_samples(p1, src + src_idx * 2, w1 / 2U, apply_rot, &phase, bounded);
        src_idx += w1 / 2U;
        soapy_copy_cf32_samples(p2, src + src_idx * 2, w2 / 2U, apply_rot, &phase, bounded);
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
        rtl_perf_record_ingest(dsd_realtime_mono_ns() - perf_t0, done, drops_delta);
    }
    return done / 2;
}

#ifdef USE_SOAPYSDR
static size_t
soapy_write_cs16_to_ring(struct rtl_device* s, const int16_t* src, size_t num_elems, int apply_rot) {
    if (!s || !s->input_ring || !src || num_elems == 0) {
        return 0;
    }
    int perf_on = rtl_perf_enabled();
    uint64_t perf_t0 = perf_on ? dsd_realtime_mono_ns() : 0ULL;
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
        rtl_perf_record_ingest(dsd_realtime_mono_ns() - perf_t0, done, drops_delta);
    }
    return done / 2;
}
#endif

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* Replay reader seams (rtl_stream_test_support.h), set while no replay runs. */
static std::atomic<int> g_test_replay_read_error_after{-1};
static std::atomic<int> g_test_replay_read_error_code{0};
static std::atomic<int> g_test_replay_reads_with_data{0};
static std::atomic<int> g_test_replay_fail_start{0};
static std::atomic<uint64_t> g_test_replay_eof_wait_iterations{0U};
static std::atomic<size_t> g_test_replay_read_limit{0U};
static std::atomic<int> g_test_replay_convert_after{-1};
static std::atomic<int> g_test_replay_convert_result{0};
static std::atomic<int> g_test_replay_conversions{0};

extern "C" void
rtl_device_test_replay_inject_read_error(int after_chunks, int code) {
    g_test_replay_reads_with_data.store(0, std::memory_order_relaxed);
    g_test_replay_read_error_code.store(code, std::memory_order_relaxed);
    g_test_replay_read_error_after.store(after_chunks, std::memory_order_release);
}

extern "C" void
rtl_device_test_replay_limit_read(size_t max_bytes) {
    g_test_replay_read_limit.store(max_bytes, std::memory_order_release);
}

extern "C" void
rtl_device_test_replay_override_conversion(int after_chunks, int produced) {
    g_test_replay_conversions.store(0, std::memory_order_relaxed);
    g_test_replay_convert_result.store(produced, std::memory_order_relaxed);
    g_test_replay_convert_after.store(after_chunks, std::memory_order_release);
}

/* A chunk conversion's result, or the one a test put in its place once its first chunks were converted. */
static int
replay_test_override_conversion(int produced) {
    const int after = g_test_replay_convert_after.load(std::memory_order_acquire);
    if (after < 0 || g_test_replay_conversions.fetch_add(1, std::memory_order_acq_rel) < after) {
        return produced;
    }
    return g_test_replay_convert_result.load(std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_replay_fail_start(int fail) {
    g_test_replay_fail_start.store(fail ? 1 : 0, std::memory_order_release);
}

extern "C" uint64_t
rtl_device_test_replay_eof_wait_iterations(void) {
    return g_test_replay_eof_wait_iterations.load(std::memory_order_acquire);
}

/* Takes the injected read error when it is due: at most one per arming. */
static int
replay_test_take_injected_read_error(int* out_code) {
    int after = g_test_replay_read_error_after.load(std::memory_order_acquire);
    if (after < 0 || g_test_replay_reads_with_data.load(std::memory_order_relaxed) < after) {
        return 0;
    }
    if (!g_test_replay_read_error_after.compare_exchange_strong(after, -1, std::memory_order_acq_rel)) {
        return 0;
    }
    *out_code = g_test_replay_read_error_code.load(std::memory_order_relaxed);
    return 1;
}
#endif

/* dsd_iq_replay_read(), or the error a test injected in its place. */
static int
replay_read_capture(struct rtl_device* s, uint8_t* raw_block, size_t read_limit, size_t* out_bytes) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    int injected = DSD_IQ_OK;
    if (replay_test_take_injected_read_error(&injected)) {
        *out_bytes = 0U;
        return injected;
    }
    const size_t test_limit = g_test_replay_read_limit.load(std::memory_order_acquire);
    if (test_limit > 0U && read_limit > test_limit) {
        read_limit = test_limit;
    }
#endif
    int rc = dsd_iq_replay_read(s->replay_src, raw_block, read_limit, out_bytes);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rc == DSD_IQ_OK && *out_bytes > 0U) {
        g_test_replay_reads_with_data.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    return rc;
}

/* Read the next @p limit bytes of the capture, looping over short reads until they are all read or the capture ends,
 * so a chunk is whole: a cf32 read that stopped inside a complex sample would otherwise be skipped as unaligned, and
 * every read after it would stay off the sample grid. A read that fails after part of the chunk was read hands over
 * that part (the replay ends after the samples already read) and reports the failure on the next call. A chunk the
 * capture's end or a failure cut short ends on its last whole complex sample: nothing can complete a part of one, and
 * a cf32 chunk holding one could not be converted at all. */
static int
replay_read_exact(struct rtl_device* s, uint8_t* raw_block, size_t limit, size_t* out_bytes) {
    *out_bytes = 0U;
    if (s->replay_deferred_read_rc != DSD_IQ_OK) {
        int rc = s->replay_deferred_read_rc;
        s->replay_deferred_read_rc = DSD_IQ_OK;
        return rc;
    }
    int failure_rc = DSD_IQ_OK;
    while (*out_bytes < limit) {
        size_t got = 0U;
        int rc = replay_read_capture(s, raw_block + *out_bytes, limit - *out_bytes, &got);
        if (rc != DSD_IQ_OK) {
            failure_rc = rc;
            break;
        }
        if (got == 0U) {
            break;
        }
        *out_bytes += got;
    }
    if (*out_bytes < limit) {
        const size_t align = dsd_iq_sample_format_alignment_bytes(s->replay_cfg.format);
        if (align > 0U) {
            *out_bytes -= *out_bytes % align;
        }
    }
    if (failure_rc != DSD_IQ_OK) {
        if (*out_bytes == 0U) {
            return failure_rc;
        }
        s->replay_deferred_read_rc = failure_rc;
    }
    return DSD_IQ_OK;
}

static inline void
replay_test_note_eof_wait(void) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_replay_eof_wait_iterations.fetch_add(1U, std::memory_order_relaxed);
#endif
}

static inline void
replay_test_reset_eof_wait(void) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_replay_eof_wait_iterations.store(0U, std::memory_order_relaxed);
#endif
}

/* A replay reader stage a test can stop at (RTL_STREAM_TEST_REPLAY_*). Outside test builds the stage names do not
 * exist, and only the count is evaluated. */
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
#define REPLAY_TEST_READER_STAGE(stage, count) rtl_stream_test_replay_stage((stage), (count))
#else
#define REPLAY_TEST_READER_STAGE(stage, count) ((void)(count))
#endif

static inline int
replay_forced_stop_requested(const struct rtl_device* s) {
    if (!s) {
        return 1;
    }
    if (!s->run.load(std::memory_order_acquire) || dsd_exitflag_load()) {
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

/* Wake every thread waiting on a replay condition: through the stream (replay_eof.wake_all, which reaches both rings
 * and the EOF condition), else the input ring's waiters and the EOF condition. */
static void
replay_device_wake_all(struct rtl_device* s) {
    if (!s) {
        return;
    }
    if (s->replay_has_eof_state && s->replay_eof.wake_all) {
        s->replay_eof.wake_all(s->replay_eof.eof_user);
        return;
    }
    replay_signal_input_waiters(s);
    if (s->replay_has_eof_state && s->replay_eof.eof_cond && s->replay_eof.eof_m) {
        dsd_mutex_lock(s->replay_eof.eof_m);
        dsd_cond_broadcast(s->replay_eof.eof_cond);
        dsd_mutex_unlock(s->replay_eof.eof_m);
    }
}

/* Sleep on the input ring's space condition, which each demod commit signals, until the demod has taken everything in
 * the input ring: 50 ms timed waits with no deadline, which also see a stop request. Returns 1 once the ring is empty, 0
 * on a stop. The EOF sequence counts its looks at the ring for a test (@p note_eof_wait). */
static int
replay_wait_for_empty_input_ring(struct rtl_device* s, int note_eof_wait) {
    struct input_ring_state* ring = s->input_ring;
    if (!ring || ring->capacity == 0U) {
        return 0;
    }
    dsd_mutex_lock(&ring->ready_m);
    while (!replay_forced_stop_requested(s) && input_ring_used(ring) > 0U) {
        if (note_eof_wait) {
            replay_test_note_eof_wait();
        }
        (void)dsd_cond_timedwait_monotonic(&ring->space, &ring->ready_m, dsd_realtime_mono_ns() + 50000000ULL);
    }
    dsd_mutex_unlock(&ring->ready_m);
    return replay_forced_stop_requested(s) ? 0 : 1;
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

/* The input half of an event boundary on its own: 50 ms timed waits until the demod has taken and acknowledged every
 * chunk. This is the no-stream path, for a replay device whose EOF state carries no wait_event_boundary. The stream
 * always installs one (stream_open_fill_replay_eof_state() in rtl_sdr_fm.cpp), so a stream's replay never comes here.
 * It stays as the device's own boundary rule, which replay_event_boundary_drained() states and IO_RTL_RETUNE_PREPARE
 * checks through rtl_device_test_replay_event_boundary_drained(). */
static int
replay_wait_for_input_boundary_drain(struct rtl_device* s) {
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
            uint64_t wait_deadline = dsd_realtime_mono_ns() + 50000000ULL;
            (void)dsd_cond_timedwait_monotonic(&ring->space, &ring->ready_m, wait_deadline);
        }
        dsd_mutex_unlock(&ring->ready_m);
    }
}

/* Wait, with no deadline, until the pipeline is idle before the event of kind @p event_kind (0: a loop rewind) is applied
 * (issue #572): the stream's wait_event_boundary, which also waits for the decoder to read and acknowledge every batch
 * published, so the event lands between the same two chunks however the decoder is scheduled and nothing it has not
 * read is cleared. Returns 1 once idle, 0 on a stop. */
static int
replay_wait_for_event_boundary_drain(struct rtl_device* s, int event_kind) {
    REPLAY_TEST_READER_STAGE(RTL_STREAM_TEST_REPLAY_READER_EVENT_BOUNDARY, (size_t)event_kind);
    if (!s || !s->replay_has_eof_state || !s->replay_eof.wait_event_boundary) {
        return replay_wait_for_input_boundary_drain(s);
    }
    return (s->replay_eof.wait_event_boundary(s->replay_eof.eof_user) && !replay_forced_stop_requested(s)) ? 1 : 0;
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
    if (dsd_realtime_mono_ns() >= deadline_ns) {
        return 1;
    }
    dsd_mutex_lock(&s->input_ring->ready_m);
    while (!replay_forced_stop_requested(s)) {
        uint64_t now_ns = dsd_realtime_mono_ns();
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

/* Capture time of @p complex_samples at the replay's sample rate, in ns, rounded down. Replay refuses events that change
 * the rate, so the one rate times the whole capture. */
static uint64_t
replay_media_ns(const struct rtl_device* s, uint64_t complex_samples) {
    const uint64_t rate = s->replay_cfg.sample_rate_hz;
    if (rate == 0U) {
        return 0U;
    }
    return (complex_samples / rate) * 1000000000ULL + ((complex_samples % rate) * 1000000000ULL) / rate;
}

namespace {

/* The failure code of a replay step that failed outside the capture file: an input ring that would not take a whole
 * chunk, an event boundary the pipeline gave up on without a stop, or a missing source, buffer or ring. No
 * DSD_IQ_ERR_* has its value, so replay_report_failure() can name the pipeline instead of the read; the session's input
 * failure latch still gets DSD_IQ_ERR_INVALID_ARG for it, as before. */
const int kReplayPipelineFailureRc = -1000;
/* The failure code of a chunk the reader read whole but could not convert to samples, which
 * replay_report_failure() names with the capture's format and stage. */
const int kReplayConvertFailureRc = -1001;

/* What one pass of the replay reader loop (replay_thread_process_block()) leaves the reader to do. */
enum replay_step {
    REPLAY_STEP_CONTINUE = 0, /* read on */
    REPLAY_STEP_STOP = 1,     /* a stop was requested: leave now */
    REPLAY_STEP_EOF_DONE = 2, /* the capture ended and does not loop: drain what was delivered, then leave */
    REPLAY_STEP_FAILED = 3,   /* the replay cannot go on: report it, drain what was delivered, then leave */
};

} // namespace

/* A step that could not complete: a requested stop, else a failure with @p rc. */
static replay_step
replay_step_stopped_or_failed(const struct rtl_device* s, int* out_failure_rc, int rc) {
    if (replay_forced_stop_requested(s)) {
        return REPLAY_STEP_STOP;
    }
    if (out_failure_rc) {
        *out_failure_rc = rc;
    }
    return REPLAY_STEP_FAILED;
}

static int replay_dispatch_pending_events(struct rtl_device* s, uint32_t* event_cursor, uint64_t data_offset,
                                          int* phase, int* have_carry, uint8_t* carry_byte, uint64_t* complex_written);
static size_t replay_next_read_limit(const struct rtl_device* s, uint64_t data_offset, uint32_t event_cursor,
                                     size_t block_size);
static int replay_convert_block_to_f32(const struct rtl_device* s, const uint8_t* raw_block, size_t out_bytes,
                                       float* f32_block, size_t f32_cap, int* phase, int* have_carry,
                                       uint8_t* carry_byte, dsd_input_level_cu8_moments* moments);

static inline int
replay_submit_chunk_inputs_valid(const struct rtl_device* s, const float* src, const size_t* out_grant_f32,
                                 const struct rtl_replay_chunk_meta* meta, size_t chunk_f32) {
    if (!s || !s->input_ring) {
        return 0;
    }
    if (!src || !out_grant_f32 || !meta) {
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

/* Number the next chunk: bump the submit generation and return the chunk's. */
static inline uint64_t
replay_record_submit_generation(struct rtl_device* s) {
    if (!s || !s->replay_has_eof_state || !s->replay_eof.replay_last_submit_gen) {
        return 0U;
    }
    return s->replay_eof.replay_last_submit_gen->fetch_add(1ULL, std::memory_order_release) + 1ULL;
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

/* Copy one whole chunk into the input ring, number it and commit it. Its metadata, generation included, reaches the
 * stream's chunk slot before the commit makes the chunk visible to the demod. The reservation must take the whole chunk
 * (an empty ring always grants it); a shorter one fails. */
static int
replay_reserve_and_submit_chunk(struct rtl_device* s, const float* src, size_t chunk_f32,
                                struct rtl_replay_chunk_meta* meta, size_t* out_grant_f32) {
    if (!replay_submit_chunk_inputs_valid(s, src, out_grant_f32, meta, chunk_f32)) {
        return -1;
    }
    struct input_ring_state* ring = s->input_ring;
    uint64_t discard_generation = input_ring_discard_generation(ring);
    float *p1 = NULL, *p2 = NULL;
    size_t first = 0U;
    size_t second = 0U;
    size_t grant = replay_reserve_even_segments(ring, chunk_f32, &p1, &first, &p2, &second);
    *out_grant_f32 = grant;
    if (grant != chunk_f32) {
        return -1;
    }

    if (first > 0U) {
        DSD_MEMCPY(p1, src, first * sizeof(float));
    }
    if (second > 0U) {
        DSD_MEMCPY(p2, src + first, second * sizeof(float));
    }

    meta->submit_gen = replay_record_submit_generation(s);
    if (s->replay_has_eof_state && s->replay_eof.chunk) {
        *s->replay_eof.chunk = *meta;
    }
    REPLAY_TEST_READER_STAGE(RTL_STREAM_TEST_REPLAY_READER_BEFORE_COMMIT, (size_t)meta->submit_gen);
    replay_commit_or_drop_reserved(ring, discard_generation, grant);
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
    uint64_t* timeline_samples; /* capture samples handed over or omitted so far (the media timeline); a loop goes on */
    uint64_t* chunk_sequence;   /* chunks submitted so far */
    uint64_t* pass_start_sequence; /* chunk_sequence when this pass over the capture began: at the start, or a rewind */
    uint32_t* loop_pass;           /* passes completed under --iq-loop */
    int realtime;
    /* What REPLAY_STEP_FAILED failed on: the capture file's DSD_IQ_* error, or kReplayPipelineFailureRc for a failure
       outside it. */
    int failure_rc;
    /* A resampled replay's output block (rtl_device::replay_resampler), room for rs_cap_complex samples; NULL when
       the capture runs at its own rate. */
    float* rs_block;
    size_t rs_cap_complex;
};

} // namespace

static replay_step replay_handle_empty_read(struct rtl_device* s, replay_thread_io_state* io);

/* Whether every position and count the reader keeps has its storage. */
static inline int
replay_thread_io_counters_valid(const replay_thread_io_state* io) {
    if (!io->complex_written || !io->data_offset || !io->event_cursor || !io->start_ns) {
        return 0;
    }
    if (!io->timeline_samples || !io->chunk_sequence || !io->pass_start_sequence || !io->loop_pass) {
        return 0;
    }
    return 1;
}

static inline int
replay_thread_process_inputs_valid(const struct rtl_device* s, const uint8_t* raw_block, const float* f32_block,
                                   const replay_thread_io_state* io) {
    if (!s || !raw_block || !f32_block) {
        return 0;
    }
    if (!io || !io->phase || !io->have_carry || !io->carry_byte) {
        return 0;
    }
    return replay_thread_io_counters_valid(io);
}

/* The reader's position now that the demodulator has the capture up to it: what a run-time re-plan resumes from
 * (issue #701). The data offset is the bytes read, a cu8 byte carried to the next chunk included (the resume's seek
 * rounds down to it); the fs/4 phase is the next sample's. */
static void
replay_commit_checkpoint(struct rtl_device* s, const replay_thread_io_state* io) {
    s->replay_committed.data_offset = *io->data_offset;
    s->replay_committed.event_index = *io->event_cursor;
    s->replay_committed.timeline_samples = *io->timeline_samples;
    s->replay_committed.pass_samples = *io->complex_written;
    s->replay_committed.loop_pass = *io->loop_pass;
    s->replay_committed.fs4_phase = (int32_t)(*io->phase & 3);
}

/* Hand one capture chunk to the demod whole (issue #572): wait for its realtime deadline, then, with no deadline, for
 * the demod to take the chunk before it (an empty input ring), then commit all of it at once with its sequence, submit
 * generation and media span. The demod's next block is exactly this chunk, and the reader reads and converts the next
 * one while the demod works on this. @p float_count is what the demod takes; @p capture_complex the capture samples
 * they came from, which pacing, the media span and the timeline count: the same samples, or what a resampled replay
 * read (issue #701). */
static int
replay_submit_whole_chunk(struct rtl_device* s, const float* src, size_t float_count, uint64_t capture_complex,
                          replay_thread_io_state* io, struct rtl_replay_chunk_meta* chunk) {
    if (!replay_wait_for_enqueue_deadline(s, *io->complex_written, *io->start_ns, io->realtime)) {
        return -1;
    }
    REPLAY_TEST_READER_STAGE(RTL_STREAM_TEST_REPLAY_READER_WAIT_FOR_EMPTY_INPUT, (size_t)(*io->chunk_sequence + 1U));
    if (!replay_wait_for_empty_input_ring(s, 0)) {
        return -1;
    }
    chunk->sequence = *io->chunk_sequence + 1U;
    chunk->media_start_ns = replay_media_ns(s, *io->timeline_samples);
    chunk->media_end_ns = replay_media_ns(s, *io->timeline_samples + capture_complex);
    size_t grant = 0U;
    if (replay_reserve_and_submit_chunk(s, src, float_count, chunk, &grant) != 0) {
        return -1;
    }
    *io->chunk_sequence = chunk->sequence;
    *io->complex_written += capture_complex;
    *io->timeline_samples += capture_complex;
    replay_commit_checkpoint(s, io);
    return 0;
}

/* Read the next chunk of the capture. At its end, rewind a looping replay (CONTINUE with no bytes) or report the end
 * (EOF_DONE); a read the source refuses is a failure. */
static inline replay_step
replay_thread_read_or_handle_empty(struct rtl_device* s, uint8_t* raw_block, size_t read_limit,
                                   replay_thread_io_state* io, size_t* out_bytes) {
    if (!s || !raw_block || !io || !io->data_offset || !io->complex_written || !io->start_ns || !io->phase
        || !io->have_carry || !io->carry_byte || !io->event_cursor || !out_bytes) {
        if (io) {
            io->failure_rc = kReplayPipelineFailureRc;
        }
        return REPLAY_STEP_FAILED;
    }
    *out_bytes = 0U;
    int rc = replay_read_exact(s, raw_block, read_limit, out_bytes);
    if (rc != DSD_IQ_OK) {
        io->failure_rc = rc;
        return REPLAY_STEP_FAILED;
    }
    if (*out_bytes == 0U) {
        return replay_handle_empty_read(s, io);
    }
    *io->data_offset += (uint64_t)(*out_bytes);
    return REPLAY_STEP_CONTINUE;
}

/* Submit @p float_count converted floats of the capture (replay_submit_whole_chunk()): as they are, or, for a
 * resampled replay, what the resampler makes of them. */
static replay_step
replay_thread_submit_converted(struct rtl_device* s, const float* f32_block, size_t float_count,
                               replay_thread_io_state* io, struct rtl_replay_chunk_meta* chunk) {
    const uint64_t capture_complex = (uint64_t)float_count / 2U;
    if (!s->replay_resampler) {
        if (replay_submit_whole_chunk(s, f32_block, float_count, capture_complex, io, chunk) != 0) {
            return replay_step_stopped_or_failed(s, &io->failure_rc, kReplayPipelineFailureRc);
        }
        return REPLAY_STEP_CONTINUE;
    }
    /* Resampled (issue #701): the demod takes the chunk at the DSP input rate; pacing and media time count the capture
       samples read. */
    const int resampled = dsd_iq_resampler_process(s->replay_resampler, f32_block, (size_t)capture_complex,
                                                   io->rs_block, io->rs_cap_complex);
    if (resampled < 0) {
        return replay_step_stopped_or_failed(s, &io->failure_rc, kReplayConvertFailureRc);
    }
    if (resampled == 0) {
        /* Too few samples for an output yet: they are in the resampler's history, and their capture time passes. */
        *io->complex_written += capture_complex;
        *io->timeline_samples += capture_complex;
        replay_commit_checkpoint(s, io);
        return REPLAY_STEP_CONTINUE;
    }
    if (replay_submit_whole_chunk(s, io->rs_block, (size_t)resampled * 2U, capture_complex, io, chunk) != 0) {
        return replay_step_stopped_or_failed(s, &io->failure_rc, kReplayPipelineFailureRc);
    }
    return REPLAY_STEP_CONTINUE;
}

static replay_step
replay_thread_process_block(struct rtl_device* s, uint8_t* raw_block, size_t raw_block_bytes, float* f32_block,
                            replay_thread_io_state* io) {
    if (!replay_thread_process_inputs_valid(s, raw_block, f32_block, io)) {
        if (io) {
            io->failure_rc = kReplayPipelineFailureRc;
        }
        return REPLAY_STEP_FAILED;
    }

    const uint64_t complex_written_before_events = *io->complex_written;
    if (!replay_dispatch_pending_events(s, io->event_cursor, *io->data_offset, io->phase, io->have_carry,
                                        io->carry_byte, io->complex_written)) {
        return replay_step_stopped_or_failed(s, &io->failure_rc, kReplayPipelineFailureRc);
    }
    /* Capture time an event omits (a MUTE) passes on the media timeline as it does on the realtime schedule. */
    *io->timeline_samples += *io->complex_written - complex_written_before_events;

    size_t read_limit = replay_next_read_limit(s, *io->data_offset, *io->event_cursor, raw_block_bytes);
    if (read_limit == 0U) {
        return REPLAY_STEP_CONTINUE;
    }

    size_t out_bytes = 0U;
    replay_step read_step = replay_thread_read_or_handle_empty(s, raw_block, read_limit, io, &out_bytes);
    if (read_step != REPLAY_STEP_CONTINUE || out_bytes == 0U) {
        return read_step;
    }

    dsd_input_level_snapshot input_level;
    DSD_MEMSET(&input_level, 0, sizeof(input_level));
    dsd_input_level_cu8_moments moments;
    dsd_input_level_cu8_moments_reset(&moments);
    const int is_cu8 = s->replay_cfg.format == DSD_IQ_FORMAT_CU8;
    int have_input_level =
        !is_cu8
        && (rtl_prepare_replay_input_level_snapshot(s, raw_block, out_bytes, f32_block, raw_block_bytes, &input_level)
            == 0);

    int produced = replay_convert_block_to_f32(s, raw_block, out_bytes, f32_block, raw_block_bytes, io->phase,
                                               io->have_carry, io->carry_byte, is_cu8 ? &moments : NULL);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    produced = replay_test_override_conversion(produced);
#endif
    if (produced < 0) {
        /* The open refuses every format and stage the converters cannot handle, so a chunk they refuse is a read the
           replay cannot complete: it ends after the samples already delivered, with the failure reported, rather than
           going on without the chunk. */
        return replay_step_stopped_or_failed(s, &io->failure_rc, kReplayConvertFailureRc);
    }
    if (produced == 0) {
        /* No whole sample yet: a one-byte cu8 chunk stays in the carry for the next. */
        return REPLAY_STEP_CONTINUE;
    }
    if (is_cu8) {
        have_input_level =
            dsd_input_level_cu8_moments_finalize(&moments, rtl_u8_input_level_source(s), &input_level) == 0;
    }

    /* The chunk's input level travels with it: the demod publishes it when it starts the chunk's block. */
    struct rtl_replay_chunk_meta chunk;
    DSD_MEMSET(&chunk, 0, sizeof(chunk));
    if (have_input_level) {
        chunk.have_input_level = 1;
        chunk.input_level = input_level;
    }
    return replay_thread_submit_converted(s, f32_block, (size_t)produced, io, &chunk);
}

static int
replay_convert_cu8_to_f32(const struct rtl_device* s, const uint8_t* in, size_t in_bytes, float* out_f32,
                          size_t out_cap_f32, int* io_phase, int* io_have_carry, uint8_t* io_carry_byte,
                          dsd_input_level_cu8_moments* moments) {
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
        rtl_process_u8_chunk(s, pair, out_f32 + out_pos, 2U, fs4_active, combine_active, use_two_pass, io_phase, NULL);
        if (moments) {
            (void)dsd_input_level_cu8_moments_accumulate(moments, in, 1U);
        }
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
                             combine_active, use_two_pass, io_phase, moments);
        out_pos += aligned;
        consumed += aligned;
    }

    if (consumed < in_bytes) {
        if (moments) {
            (void)dsd_input_level_cu8_moments_accumulate(moments, in + consumed, 1U);
        }
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

    /* A capture file is external data: every component enters bounded, rotated or not. */
    rtl_cf32_copy_bounded(out_f32, in, complex_count, s->replay_cfg.fs4_shift_enabled, io_phase);
    return (int)float_count;
}

static int
replay_convert_cs16_to_f32(const struct rtl_device* s, const uint8_t* in, size_t in_bytes, float* out_f32,
                           size_t out_cap_f32, int* io_phase) {
    if (!s || !in || !out_f32 || !io_phase || (in_bytes & 3U) != 0U) {
        return -1;
    }
    if (strcmp(s->replay_cfg.capture_stage, "post_mute_pre_widen") != 0) {
        return -1;
    }

    const size_t complex_count = in_bytes / 4U;
    const size_t float_count = complex_count * 2U;
    if (float_count > out_cap_f32) {
        return -1;
    }

    const float scale = 1.0f / 32768.0f;
    int phase = *io_phase & 3;
    for (size_t i = 0; i < complex_count; ++i) {
        int16_t raw_i = 0;
        int16_t raw_q = 0;
        DSD_MEMCPY(&raw_i, in + i * 4U, sizeof(raw_i));
        DSD_MEMCPY(&raw_q, in + i * 4U + 2U, sizeof(raw_q));
        const float sample_i = (float)raw_i * scale;
        const float sample_q = (float)raw_q * scale;
        if (s->replay_cfg.fs4_shift_enabled) {
            rtl_apply_j4_rotation(sample_i, sample_q, phase, &out_f32[i * 2U], &out_f32[i * 2U + 1U]);
            phase = (phase + 1) & 3;
        } else {
            out_f32[i * 2U] = sample_i;
            out_f32[i * 2U + 1U] = sample_q;
        }
    }
    *io_phase = phase;
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

/* A resampled replay's signal is discontinuous here (a retune, a reset, samples a MUTE omitted, a loop rewind): its
 * resampler starts over from silence, as the demod's filters do. */
static void
replay_clear_resampler(struct rtl_device* s) {
    if (s && s->replay_resampler) {
        dsd_iq_resampler_clear(s->replay_resampler);
    }
}

static void
replay_dispatch_event(struct rtl_device* s, const dsd_iq_event* event, int* io_phase, int* io_have_carry,
                      uint8_t* io_carry_byte, uint64_t* complex_written) {
    if (!s || !event) {
        return;
    }
    /* The callbacks see the rate the demodulator takes, which a resampled replay's differs from the capture's
       (issue #701); byte offsets and durations stay in capture bytes. */
    dsd_iq_event seen = *event;
    seen.sample_rate_hz = dsd_iq_replay_dsp_input_rate_hz(&s->replay_cfg);
    switch (event->kind) {
        case DSD_IQ_EVENT_RETUNE:
            replay_reset_stream_position(s, io_phase, io_have_carry, io_carry_byte);
            replay_clear_resampler(s);
            s->replay_cfg.center_frequency_hz = event->center_frequency_hz;
            s->replay_cfg.capture_center_frequency_hz = event->capture_center_frequency_hz;
            s->replay_cfg.sample_rate_hz = event->sample_rate_hz;
            s->freq = (event->capture_center_frequency_hz > UINT32_MAX) ? UINT32_MAX
                                                                        : (uint32_t)event->capture_center_frequency_hz;
            s->rate = seen.sample_rate_hz;
            if (s->replay_has_eof_state && s->replay_eof.on_retune_event) {
                s->replay_eof.on_retune_event(&seen, s->replay_eof.event_user);
            }
            break;
        case DSD_IQ_EVENT_MUTE:
            replay_advance_omitted_bytes(s, event->duration_bytes, io_phase, io_have_carry, io_carry_byte,
                                         complex_written);
            replay_clear_resampler(s);
            if (s->replay_has_eof_state && s->replay_eof.on_mute_event) {
                s->replay_eof.on_mute_event(&seen, s->replay_eof.event_user);
            }
            break;
        case DSD_IQ_EVENT_RESET:
            replay_reset_stream_position(s, io_phase, io_have_carry, io_carry_byte);
            replay_clear_resampler(s);
            if (s->replay_has_eof_state && s->replay_eof.on_reset_event) {
                s->replay_eof.on_reset_event(&seen, s->replay_eof.event_user);
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

extern "C" int
rtl_device_test_replay_input_level_snapshot(int format, int backend, const char* capture_stage, size_t raw_bytes,
                                            size_t scratch_cap_f32, int* out_rc, int* out_source, uint64_t* out_count) {
    if (!out_rc || !out_source || !out_count || raw_bytes > 32U || scratch_cap_f32 > 8U) {
        return -1;
    }

    rtl_device dev{};
    dev.backend = backend;
    dev.replay_cfg.format = (dsd_iq_sample_format)format;
    DSD_SNPRINTF(dev.replay_cfg.capture_stage, sizeof(dev.replay_cfg.capture_stage), "%s",
                 capture_stage ? capture_stage : "");

    uint8_t raw[32] = {};
    for (size_t i = 0U; i < sizeof(raw); i++) {
        raw[i] = (i & 1U) ? 136U : 119U;
    }
    if (format == DSD_IQ_FORMAT_CF32) {
        const float samples[] = {0.25f, -0.25f, 0.50f, -0.50f};
        size_t copy_bytes = raw_bytes < sizeof(samples) ? raw_bytes : sizeof(samples);
        DSD_MEMCPY(raw, samples, copy_bytes);
    } else if (format == DSD_IQ_FORMAT_CS16) {
        const int16_t samples[] = {8192, -8192, 16384, -16384};
        size_t copy_bytes = raw_bytes < sizeof(samples) ? raw_bytes : sizeof(samples);
        DSD_MEMCPY(raw, samples, copy_bytes);
    }

    float scratch[8] = {};
    dsd_input_level_snapshot snapshot{};
    *out_rc = rtl_prepare_replay_input_level_snapshot(&dev, raw, raw_bytes, scratch, scratch_cap_f32, &snapshot);
    *out_source = (*out_rc == 0) ? (int)snapshot.source : -1;
    *out_count = (*out_rc == 0) ? snapshot.sample_count : 0U;
    return 0;
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

/* The EOF drain decision is made under eof_m on every side: here, and by the demod (demod_update_replay_drain_state(),
 * replay_note_input_purge_consumed() in rtl_sdr_fm.cpp). Each side stores what it knows and loads what the other
 * stored in one critical section, so whichever side decides second sees the other's store, and the wakeup cannot be
 * lost between them. */
static inline void
replay_eof_lock(struct rtl_device* s) {
    if (s->replay_eof.eof_m) {
        dsd_mutex_lock(s->replay_eof.eof_m);
    }
}

static inline void
replay_eof_broadcast_and_unlock(struct rtl_device* s) {
    if (s->replay_eof.eof_m) {
        if (s->replay_eof.eof_cond) {
            dsd_cond_broadcast(s->replay_eof.eof_cond);
        }
        dsd_mutex_unlock(s->replay_eof.eof_m);
    }
}

static void
replay_mark_input_eof(struct rtl_device* s) {
    if (!s || !s->replay_has_eof_state) {
        return;
    }
    replay_eof_lock(s);
    if (s->replay_eof.replay_last_submit_gen && s->replay_eof.replay_last_submit_gen_at_eof) {
        uint64_t last_gen = s->replay_eof.replay_last_submit_gen->load(std::memory_order_acquire);
        s->replay_eof.replay_last_submit_gen_at_eof->store(last_gen, std::memory_order_release);
    }
    if (s->replay_eof.replay_input_eof) {
        s->replay_eof.replay_input_eof->store(1, std::memory_order_release);
    }
    replay_eof_broadcast_and_unlock(s);
    replay_device_wake_all(s);
}

/* The input ring is empty at EOF: mark the input drained, and the demod drained too once it has acknowledged every
 * generation submitted before EOF. The demod decides the same thing after each block; whichever decides second marks
 * it. */
static void
replay_mark_input_drained(struct rtl_device* s) {
    REPLAY_TEST_READER_STAGE(RTL_STREAM_TEST_REPLAY_READER_DRAIN_DECISION, 0U);
    int demod_drained_now = 0;
    replay_eof_lock(s);
    if (s->replay_eof.replay_input_drained) {
        s->replay_eof.replay_input_drained->store(1, std::memory_order_release);
    }
    if (s->replay_eof.replay_demod_drained && s->replay_eof.replay_last_consume_gen
        && s->replay_eof.replay_last_submit_gen_at_eof
        && !s->replay_eof.replay_demod_drained->load(std::memory_order_acquire)) {
        uint64_t consumed_gen = s->replay_eof.replay_last_consume_gen->load(std::memory_order_acquire);
        uint64_t eof_gen = s->replay_eof.replay_last_submit_gen_at_eof->load(std::memory_order_acquire);
        if (consumed_gen >= eof_gen) {
            s->replay_eof.replay_demod_drained->store(1, std::memory_order_release);
            demod_drained_now = 1;
        }
    }
    replay_eof_broadcast_and_unlock(s);
    REPLAY_TEST_READER_STAGE(RTL_STREAM_TEST_REPLAY_READER_DRAIN_DECIDED, (size_t)demod_drained_now);
    if (s->replay_eof.on_input_drained) {
        s->replay_eof.on_input_drained(s->replay_eof.eof_user);
    }
}

/* A 50 ms timed wait for the demod to drain, which also sees a stop request (the demod thread marks itself drained
 * when it leaves). */
static void
replay_wait_for_demod_drain(struct rtl_device* s) {
    if (!s->replay_eof.eof_m || !s->replay_eof.eof_cond || !s->replay_eof.replay_demod_drained) {
        return;
    }
    dsd_mutex_lock(s->replay_eof.eof_m);
    while (!replay_forced_stop_requested(s) && !s->replay_eof.replay_demod_drained->load(std::memory_order_acquire)) {
        (void)dsd_cond_timedwait(s->replay_eof.eof_cond, s->replay_eof.eof_m, 50U);
    }
    dsd_mutex_unlock(s->replay_eof.eof_m);
}

/* End the replay input: mark EOF, wait for the demod to take what was delivered and drain it, so the decoder reads
 * every sample and then learns the stream ended. The end of the capture and a failure to read it both end this way. */
static void
replay_handle_eof_sequence(struct rtl_device* s) {
    if (!s || !s->replay_has_eof_state) {
        return;
    }
    replay_mark_input_eof(s);
    (void)replay_wait_for_empty_input_ring(s, 1);
    if (!replay_forced_stop_requested(s)) {
        replay_mark_input_drained(s);
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
        /* Every event waits for an idle pipeline, even one at the offset of the event before it (the wait then returns
           at once). */
        if (!replay_wait_for_event_boundary_drain(s, (int)event->kind)) {
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

/* The capture's end on this pass: the replay's end, or, under --iq-loop, a rewind (CONTINUE, the next pass begun).
 * The reader's io state is checked whole before this (replay_thread_process_inputs_valid()). */
static replay_step
replay_handle_empty_read(struct rtl_device* s, replay_thread_io_state* io) {
    if (!s || !io) {
        if (io) {
            io->failure_rc = kReplayPipelineFailureRc;
        }
        return REPLAY_STEP_FAILED;
    }
    if (!replay_wait_for_timeline_position(s, *io->complex_written, *io->start_ns, io->realtime)) {
        return replay_step_stopped_or_failed(s, &io->failure_rc, kReplayPipelineFailureRc);
    }
    if (!s->replay_cfg.loop) {
        return REPLAY_STEP_EOF_DONE;
    }
    if (*io->chunk_sequence == *io->pass_start_sequence) {
        /* A pass that handed the demod no chunk hands it none the next time either: a rewind would only spin through
           the boundary, the reconfigure gate and a full finalize, pass after pass, with nothing reaching the decoder.
           The replay ends here instead, as one without --iq-loop ends at the capture's end. */
        LOG_WARN("IQ replay: a pass over the capture delivered no samples to the demodulator; --iq-loop does not "
                 "rewind it, and the replay ends.\n");
        return REPLAY_STEP_EOF_DONE;
    }
    /* A rewind is a replay timeline boundary just like RESET. Do not restore
     * the initial capture state while the demodulator still owns samples from
     * the completed pass, or the decoder has not read their output; the
     * reconfigure callback would otherwise purge that work and a short
     * fast-mode capture could loop without ever publishing output. */
    if (!replay_wait_for_event_boundary_drain(s, 0)) {
        return replay_step_stopped_or_failed(s, &io->failure_rc, kReplayPipelineFailureRc);
    }
    int rewind_rc = dsd_iq_replay_rewind(s->replay_src);
    if (rewind_rc != DSD_IQ_OK) {
        io->failure_rc = rewind_rc;
        return REPLAY_STEP_FAILED;
    }
    replay_restore_initial_state(s);
    replay_clear_resampler(s);
    *io->phase = 0;
    *io->have_carry = 0;
    *io->carry_byte = 0;
    *io->complex_written = 0;
    *io->data_offset = 0;
    *io->event_cursor = 0;
    *io->start_ns = dsd_realtime_mono_ns();
    *io->pass_start_sequence = *io->chunk_sequence;
    (*io->loop_pass)++;
    return REPLAY_STEP_CONTINUE;
}

static int
replay_convert_block_to_f32(const struct rtl_device* s, const uint8_t* raw_block, size_t out_bytes, float* f32_block,
                            size_t f32_cap, int* phase, int* have_carry, uint8_t* carry_byte,
                            dsd_input_level_cu8_moments* moments) {
    if (!s) {
        return -1;
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CU8) {
        return replay_convert_cu8_to_f32(s, raw_block, out_bytes, f32_block, f32_cap, phase, have_carry, carry_byte,
                                         moments);
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CF32) {
        return replay_convert_cf32_to_f32(s, raw_block, out_bytes, f32_block, f32_cap, phase);
    }
    if (s->replay_cfg.format == DSD_IQ_FORMAT_CS16) {
        return replay_convert_cs16_to_f32(s, raw_block, out_bytes, f32_block, f32_cap, phase);
    }
    return -1;
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
// Internal test hooks use a pointer-shaped C ABI; the request layout is mirrored by the test TU.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct rtl_device_test_replay_convert_block_request {
    int format;
    const char* capture_stage;
    int fs4_shift_enabled;
    int historical_cu8_two_pass;
    const uint8_t* raw_block;
    size_t raw_bytes;
    size_t out_cap_f32;
    int start_phase;
    int start_have_carry;
    uint8_t start_carry_byte;
};

static int
rtl_device_test_replay_convert_block_request_valid(const rtl_device_test_replay_convert_block_request* request,
                                                   const float* out_f32, size_t out_f32_count, const int* out_phase,
                                                   const int* out_have_carry, const uint8_t* out_carry_byte) {
    return request && request->raw_block && out_f32 && out_phase && out_have_carry && out_carry_byte
           && request->raw_bytes <= 32U && request->out_cap_f32 <= out_f32_count;
}

static size_t
rtl_device_test_replay_convert_copy_count(int rc, size_t out_f32_count, size_t scratch_count) {
    size_t copy_count = (rc > 0 && (size_t)rc < out_f32_count) ? (size_t)rc : out_f32_count;
    return (copy_count > scratch_count) ? scratch_count : copy_count;
}

extern "C" int
rtl_device_test_replay_convert_block(const rtl_device_test_replay_convert_block_request* request, float* out_f32,
                                     size_t out_f32_count, int* out_phase, int* out_have_carry,
                                     uint8_t* out_carry_byte) {
    if (!rtl_device_test_replay_convert_block_request_valid(request, out_f32, out_f32_count, out_phase, out_have_carry,
                                                            out_carry_byte)) {
        return -1;
    }

    rtl_device dev{};
    dev.backend = RTL_BACKEND_IQ_REPLAY;
    dev.replay_cfg.format = (dsd_iq_sample_format)request->format;
    dev.replay_cfg.fs4_shift_enabled = request->fs4_shift_enabled ? 1 : 0;
    dev.replay_fs4_shift_enabled = request->fs4_shift_enabled ? 1 : 0;
    dev.replay_historical_cu8_two_pass = request->historical_cu8_two_pass ? 1 : 0;
    DSD_SNPRINTF(dev.replay_cfg.capture_stage, sizeof(dev.replay_cfg.capture_stage), "%s",
                 request->capture_stage ? request->capture_stage : "");

    float scratch[8] = {};
    int phase = request->start_phase;
    int have_carry = request->start_have_carry;
    uint8_t carry_byte = request->start_carry_byte;
    int rc = replay_convert_block_to_f32(&dev, request->raw_block, request->raw_bytes, scratch, request->out_cap_f32,
                                         &phase, &have_carry, &carry_byte, NULL);
    size_t copy_count =
        rtl_device_test_replay_convert_copy_count(rc, out_f32_count, sizeof(scratch) / sizeof(scratch[0]));
    DSD_MEMCPY(out_f32, scratch, copy_count * sizeof(float));
    *out_phase = phase;
    *out_have_carry = have_carry;
    *out_carry_byte = carry_byte;
    return rc;
}
#endif

/* The capture bytes one chunk reads: dsd_iq_replay_chunk_bytes() at the capture's own rate; for a resampled replay
 * dsd_iq_replay_chunk_bytes_for(), cut further, if the resampler's ratio error needs it, until what the chunk
 * resamples to fits one live transfer (issue #701). */
static size_t
replay_raw_block_bytes(const struct rtl_device* s) {
    const uint32_t capture_hz = s->replay_cfg.sample_rate_hz;
    const uint32_t dsp_in_hz = dsd_iq_replay_dsp_input_rate_hz(&s->replay_cfg);
    size_t bytes = dsd_iq_replay_chunk_bytes_for(s->replay_cfg.format, capture_hz, dsp_in_hz);
    const size_t align = dsd_iq_sample_format_alignment_bytes(s->replay_cfg.format);
    if (!s->replay_resampler || bytes == 0U || align == 0U) {
        return bytes;
    }
    size_t samples = bytes / align;
    while (samples > 1U && dsd_iq_resampler_max_out(s->replay_resampler, samples) > DSD_IQ_REPLAY_CHUNK_MAX_SAMPLES) {
        samples--;
    }
    return samples * align;
}

/* A run-time re-plan's resume (issue #701): the open sought the capture to the checkpoint, rounded down to a whole
 * sample. The media timeline and the event cursor go on from it, and the realtime schedule is rebased so its next
 * deadline is now. A pass resumed past its start has handed the demod chunks already, so its end rewinds under
 * --iq-loop. */
static void
replay_thread_resume_position(const struct rtl_device* s, const replay_thread_io_state* io) {
    const dsd_iq_replay_checkpoint* at = &s->replay_cfg.resume_at;
    const size_t align = dsd_iq_sample_format_alignment_bytes(s->replay_cfg.format);
    *io->data_offset = align > 0U ? at->data_offset - at->data_offset % (uint64_t)align : at->data_offset;
    *io->event_cursor = at->event_index;
    *io->timeline_samples = at->timeline_samples;
    *io->complex_written = at->pass_samples;
    *io->loop_pass = at->loop_pass;
    *io->phase = (int)(at->fs4_phase & 3);
    const uint64_t elapsed_ns = replay_media_ns(s, *io->complex_written);
    *io->start_ns = *io->start_ns > elapsed_ns ? *io->start_ns - elapsed_ns : 0U;
    *io->pass_start_sequence = *io->data_offset > 0U ? UINT64_MAX : 0U;
}

/* Read the capture into the input ring until the reader has to stop, and say why it stopped. Each chunk, and so each
 * demod block, is at most one live transfer (dsd_iq_replay_chunk_bytes(), issue #626). */
static replay_step
replay_thread_read_capture(struct rtl_device* s, int* out_failure_rc) {
    const size_t raw_block_bytes = replay_raw_block_bytes(s);
    if (raw_block_bytes == 0U) {
        /* Not reachable: the open refuses a capture with no sample rate or an unknown format. */
        *out_failure_rc = kReplayPipelineFailureRc;
        return REPLAY_STEP_FAILED;
    }
    uint8_t* raw_block = static_cast<uint8_t*>(malloc(raw_block_bytes));
    float* f32_block = static_cast<float*>(malloc(raw_block_bytes * sizeof(float)));
    /* A resampled replay's output block: what the most capture samples one chunk holds resample to. */
    const size_t align = dsd_iq_sample_format_alignment_bytes(s->replay_cfg.format);
    const size_t rs_cap_complex =
        s->replay_resampler ? dsd_iq_resampler_max_out(s->replay_resampler, raw_block_bytes / align) : 0U;
    float* rs_block = rs_cap_complex > 0U ? static_cast<float*>(malloc(rs_cap_complex * 2U * sizeof(float))) : NULL;
    replay_step step = REPLAY_STEP_CONTINUE;
    if (!raw_block || !f32_block || (s->replay_resampler && !rs_block)) {
        *out_failure_rc = DSD_IQ_ERR_ALLOC;
        step = REPLAY_STEP_FAILED;
    }

    int phase = 0;
    int have_carry = 0;
    uint8_t carry_byte = 0;
    uint64_t complex_written = 0;
    uint64_t data_offset = 0;
    uint32_t event_cursor = 0;
    uint64_t start_ns = dsd_realtime_mono_ns();
    uint64_t timeline_samples = 0;
    uint64_t chunk_sequence = 0;
    uint64_t pass_start_sequence = 0;
    uint32_t loop_pass = 0;
    int realtime = s->replay_cfg.realtime ? 1 : 0;
    s->replay_deferred_read_rc = DSD_IQ_OK;
    replay_thread_io_state io_base = {
        &phase,        &have_carry, &carry_byte,       &complex_written, &data_offset,
        &event_cursor, &start_ns,   &timeline_samples, &chunk_sequence,  &pass_start_sequence,
        &loop_pass,    realtime,    DSD_IQ_OK,         rs_block,         rs_cap_complex};
    if (s->replay_cfg.resume) {
        replay_thread_resume_position(s, &io_base);
    }

    while (step == REPLAY_STEP_CONTINUE) {
        if (replay_forced_stop_requested(s)) {
            step = REPLAY_STEP_STOP;
            break;
        }
        replay_thread_io_state io = io_base; /* a failure_rc of its own for each chunk */
        step = replay_thread_process_block(s, raw_block, raw_block_bytes, f32_block, &io);
        if (step == REPLAY_STEP_FAILED) {
            *out_failure_rc = io.failure_rc;
        }
    }

    free(rs_block);
    free(f32_block);
    free(raw_block);
    return step;
}

/* The replay cannot go on: say why, and latch it as the session's file input failure, for which
 * dsd_engine_run_with_lifecycle() returns 1 once the replay ends. */
static void
replay_report_failure(const struct rtl_device* s, int rc) {
    if (rc == DSD_IQ_ERR_ALLOC) {
        LOG_ERROR("IQ replay: no memory for the capture read buffers (rc=%d); the replay ends.\n", rc);
    } else if (rc == kReplayConvertFailureRc) {
        LOG_ERROR("IQ replay: a capture chunk could not be converted to samples (sample_format '%s', capture_stage "
                  "'%s'); the replay ends after the samples already delivered.\n",
                  dsd_iq_sample_format_name(s->replay_cfg.format), s->replay_cfg.capture_stage);
        rc = DSD_IQ_ERR_UNSUPPORTED_FMT;
    } else if (rc == kReplayPipelineFailureRc) {
        LOG_ERROR("IQ replay: the capture could not be handed on to the demodulator (no replay source or input ring "
                  "to start from, an input ring that would not take a whole chunk, or an event boundary the pipeline "
                  "gave up on); the replay ends after the samples already delivered.\n");
        rc = DSD_IQ_ERR_INVALID_ARG;
    } else {
        LOG_ERROR("IQ replay: reading the capture failed (rc=%d); the replay ends after the samples already read.\n",
                  rc);
    }
    dsd_input_failure_report(DSD_INPUT_FAILURE_FILE, rc);
}

static DSD_THREAD_RETURN_TYPE DSD_RTL_THREAD_CALL
replay_thread_fn(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    if (!s) {
        DSD_THREAD_RETURN;
    }

    maybe_set_thread_realtime_and_affinity("DONGLE");
    replay_test_reset_eof_wait();

    int failure_rc = kReplayPipelineFailureRc;
    replay_step step =
        (s->replay_src && s->input_ring) ? replay_thread_read_capture(s, &failure_rc) : REPLAY_STEP_FAILED;
    if (step == REPLAY_STEP_FAILED) {
        replay_report_failure(s, failure_rc);
    }
    if (step == REPLAY_STEP_EOF_DONE || step == REPLAY_STEP_FAILED) {
        replay_handle_eof_sequence(s);
    }

    if (s->replay_has_eof_state && s->replay_eof.replay_reader_exited) {
        s->replay_eof.replay_reader_exited->store(1, std::memory_order_release);
    }
    s->run.store(0, std::memory_order_release);
    replay_device_wake_all(s);
    DSD_THREAD_RETURN;
}

/* Start the replay reader thread; a test can make the start fail as a failed thread create does. */
static int
replay_start_reader_thread(struct rtl_device* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_replay_fail_start.load(std::memory_order_acquire)) {
        return EAGAIN;
    }
#endif
    return dsd_thread_create(&dev->thread, replay_thread_fn, dev);
}

/**
 * @brief RTL-SDR asynchronous USB callback.
 * Converts incoming u8 I/Q to normalized float and enqueues into the input ring.
 * When offset tuning is off, rotation and widening use the combined bias-127.5
 * transform. On overflow, drops oldest ring data to avoid stalls.
 *
 * @param buf USB I/Q byte buffer.
 * @param len Buffer length in bytes (I/Q interleaved).
 * @param ctx Opaque pointer to `rtl_device`.
 */
static void
rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    struct rtl_device* s = static_cast<rtl_device*>(ctx);
    if (!s) {
        return;
    }
    /* Counted before anything below can drop the buffer: a callback with bytes is the receiver delivering, which is
     * all the progress monitor asks. A stalled transfer can still complete with none, and that is not progress. */
    if (len > 0U) {
        s->progress.fetch_add(1U, std::memory_order_relaxed);
    }

    /* One-time: ensure the USB callback thread gets RT scheduling/affinity if enabled */
    {
        static std::atomic<int> usb_sched_applied{0};
        int expected = 0;
        if (usb_sched_applied.compare_exchange_strong(expected, 1)) {
            maybe_set_thread_realtime_and_affinity("USB");
        }
    }

    if (dsd_exitflag_load()) {
        return;
    }
    int fs4_shift_active = 0;
    int combine_rotate_active = 0;
    int use_two_pass = 0;
    rtl_get_u8_transform_policy(s, &fs4_shift_active, &combine_rotate_active, &use_two_pass);
    /* Handle muting by discarding muted samples entirely. Feeding midpoint
     * bytes through bias subtraction would create zero-magnitude samples that
     * corrupt the Costas loop and TED after the retune gate opens.
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
    dsd_input_level_cu8_moments moments;
    dsd_input_level_cu8_moments_reset(&moments);
    /* Convert incoming u8 I/Q and write directly into input ring without extra copy. */
    rtl_write_u8_to_ring(s, buf, len, fs4_shift_active, use_two_pass, combine_rotate_active, 0, &moments);
    rtl_publish_cu8_input_level_moments(s, &moments);
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* rtl_device_test_set_progress_limit_ms() / _period_ms(); 0 keeps the production value. */
static std::atomic<uint32_t> g_test_progress_limit_ms{0U};
static std::atomic<uint32_t> g_test_progress_period_ms{0U};
/* rtl_device_test_set_recovery_cooldown_ms() / _take_ms(); 0 keeps the production value. */
static std::atomic<uint32_t> g_test_recovery_cooldown_ms{0U};
static std::atomic<uint32_t> g_test_recovery_take_ms{0U};
/* rtl_device_test_set_recovery_post_hook(): called by the monitor once it decided to post, before it does. */
static RtlTestHook<struct rtl_device*> g_test_recovery_post_hook;
/* rtl_device_test_set_reader_restart_hook(): called by a reader restart once its baseline is taken, before the thread
 * starts. */
static RtlTestHook<struct rtl_device*> g_test_reader_restart_hook;
static RtlTestHook<struct rtl_device*> g_test_recovery_take_hook;
/* rtl_device_test_set_recovery_finish_hook(): called inside rtl_device_usb_recovery_finish(), between the request's
 * move to idle and the hand-off's close, the window a restarted reader's return must not fall through. */
static RtlTestHook<struct rtl_device*> g_test_recovery_finish_hook;
/* rtl_device_test_set_owed_clear_hook(): called by the progress monitor once samples decided it clears the reset
 * obligation its watch holds, before it clears it. */
static RtlTestHook<const struct rtl_device*> g_test_owed_clear_hook;
/* rtl_device_test_refuse_progress_monitor(): the RTL or SoapySDR monitor's thread creation fails as a refused one
 * does. */
static std::atomic<int> g_test_refuse_progress_monitor{0};
/* rtl_device_test_pause_next_verdict_ms(): one verdict, past its checks, waits this long before it claims the
 * report. */
static std::atomic<uint32_t> g_test_verdict_pause_ms{0U};
/* rtl_device_test_verdicts_paused(): verdicts that began the pause rtl_device_test_pause_next_verdict_ms() set. */
static std::atomic<unsigned int> g_test_verdicts_paused{0U};
/* rtl_device_test_verdicts_in_grace(): verdicts that began waiting out the unplug-owner grace. */
static std::atomic<unsigned int> g_test_verdicts_in_grace{0U};
/* rtl_device_test_recoveries_in_grace(): stall recoveries that began waiting out the pre-recovery grace (#782). */
static std::atomic<unsigned int> g_test_recoveries_in_grace{0U};
/* rtl_device_test_hold_recovery_grace(): while set, a pre-recovery grace does not pass at its deadline. */
static std::atomic<int> g_test_hold_recovery_grace{0};
/* rtl_device_test_recovery_grace_started_ms(): the clock reading the latest pre-recovery grace's deadline counts from. */
static std::atomic<uint64_t> g_test_recovery_grace_started_ms{0U};
/* rtl_device_test_recovery_graces_ended() and _grace_result(): pre-recovery graces that ended, and the latest's end. */
static std::atomic<unsigned int> g_test_recovery_graces_ended{0U};
static std::atomic<int> g_test_recovery_grace_result{-1};

extern "C" void
rtl_device_test_set_progress_limit_ms(uint32_t limit_ms) {
    g_test_progress_limit_ms.store(limit_ms, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_set_progress_period_ms(uint32_t period_ms) {
    g_test_progress_period_ms.store(period_ms, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_set_recovery_cooldown_ms(uint32_t cooldown_ms) {
    g_test_recovery_cooldown_ms.store(cooldown_ms, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_set_recovery_take_hook(void (*hook)(struct rtl_device*, void*), void* ctx) {
    g_test_recovery_take_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_set_recovery_take_ms(uint32_t take_ms) {
    g_test_recovery_take_ms.store(take_ms, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_set_recovery_finish_hook(void (*hook)(struct rtl_device*, void*), void* ctx) {
    g_test_recovery_finish_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_set_recovery_post_hook(void (*hook)(struct rtl_device*, void*), void* ctx) {
    g_test_recovery_post_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_set_reader_restart_hook(void (*hook)(struct rtl_device*, void*), void* ctx) {
    g_test_reader_restart_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_set_owed_clear_hook(void (*hook)(const struct rtl_device*, void*), void* ctx) {
    g_test_owed_clear_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_pause_next_verdict_ms(uint32_t ms) {
    g_test_verdict_pause_ms.store(ms, std::memory_order_release);
}

extern "C" void
rtl_device_test_refuse_progress_monitor(int refuse) {
    g_test_refuse_progress_monitor.store(refuse ? 1 : 0, std::memory_order_release);
}

extern "C" unsigned int
rtl_device_test_verdicts_paused(void) {
    return g_test_verdicts_paused.load(std::memory_order_acquire);
}

extern "C" unsigned int
rtl_device_test_verdicts_in_grace(void) {
    return g_test_verdicts_in_grace.load(std::memory_order_acquire);
}

extern "C" unsigned int
rtl_device_test_recoveries_in_grace(void) {
    return g_test_recoveries_in_grace.load(std::memory_order_acquire);
}

extern "C" void
rtl_device_test_hold_recovery_grace(int hold) {
    g_test_hold_recovery_grace.store(hold ? 1 : 0, std::memory_order_release);
}

extern "C" uint64_t
rtl_device_test_recovery_grace_started_ms(void) {
    return g_test_recovery_grace_started_ms.load(std::memory_order_acquire);
}

extern "C" unsigned int
rtl_device_test_recovery_graces_ended(void) {
    return g_test_recovery_graces_ended.load(std::memory_order_acquire);
}

extern "C" int
rtl_device_test_recovery_grace_result(void) {
    return g_test_recovery_grace_result.load(std::memory_order_acquire);
}
#endif

/*
 * Taking the descriptor (reading the slot, then marking it held) and replacing the
 * slot are one step each under this lock. Once rtl_device_set_preopened_fd() has
 * returned, either the engine holds the old descriptor (and says so) or it can no
 * longer take it, so an owner that cleared the slot can trust the held query. A
 * device re-arming its descriptor's reset obligation takes it too
 * (rtl_usb_reset_owed_set()), so that store and a publish never interleave.
 */
static std::mutex g_preopened_usb_fd_lock;

/* The descriptor whose dongle owes a USB reset before its next open, -1 when none (#782): bound to the descriptor, so
 * an obligation can never be applied to another one. Set with the slot, under its lock (the host's publish), or for
 * the held descriptor by its stall verdict or recovery, under the same lock and only while the host still publishes
 * it; cleared by a successful reset, or by samples after the verdict that set it. See
 * rtl_device_preopened_fd_reset_owed(). */
static std::atomic<int> g_preopened_usb_fd_reset_owed{-1};

/* Moves with every publish, under the slot's lock: a device that took a descriptor at one value finds the host's
 * connection replaced (or released, on a detach) when it moved since. */
static std::atomic<uint32_t> g_preopened_usb_fd_generation{0U};

/* @p dev's dongle, opened from an Android descriptor, owes a reset before its next open. Only while the host still
 * publishes the connection the device took (the generation it took it at): once the host released or replaced it, the
 * slot's obligation is the next connection's, which the host published with its own descriptor, and a late verdict,
 * failed recovery or reader return of the old device must not overwrite it (#782 gate r1 G3). The host read what the
 * old descriptor owed before it moved the slot, and its detach drops it. */
static void
rtl_usb_reset_owed_set(const struct rtl_device* dev) {
    if (!dev->owns_preopened_fd) {
        return;
    }
    const std::lock_guard<std::mutex> guard(g_preopened_usb_fd_lock);
    if (g_preopened_usb_fd_generation.load(std::memory_order_acquire) == dev->preopened_generation) {
        g_preopened_usb_fd_reset_owed.store(dev->preopened_fd, std::memory_order_release);
    }
}

/* @p fd's dongle no longer owes one; an obligation the host bound to another descriptor since stays. Whether this
 * cleared it. */
static int
rtl_usb_reset_owed_clear(int fd) {
    int expected = fd;
    return g_preopened_usb_fd_reset_owed.compare_exchange_strong(expected, -1, std::memory_order_acq_rel,
                                                                 std::memory_order_acquire)
               ? 1
               : 0;
}

/* Whether the host replaced or released the connection behind @p dev's descriptor since the device took it: a detach
 * clears the slot, and a new session's connection is published in its place. */
static int
rtl_usb_preopened_fd_moved(const struct rtl_device* dev) {
    return dev->owns_preopened_fd
           && g_preopened_usb_fd_generation.load(std::memory_order_acquire) != dev->preopened_generation;
}

/* rtl_device_set_unplug_owner_grace_ms(): set once by the Android host at startup, read whenever a failure is
 * raised. */
static std::atomic<uint32_t> g_unplug_owner_grace_ms{0U};

void
rtl_device_set_unplug_owner_grace_ms(uint32_t ms) {
    g_unplug_owner_grace_ms.store(ms, std::memory_order_relaxed);
}

uint32_t
rtl_device_unplug_owner_grace_ms(void) {
    return g_unplug_owner_grace_ms.load(std::memory_order_relaxed);
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* USB reader seam (rtl_stream_test_support.h): while set, the reader returns the code a test chose in place of running
 * rtlsdr_read_async(), after the device's stop request when the test asked for one. A test holding the reader while
 * it starts (g_test_usb_reader_hold), or streaming through it (g_test_usb_reader_stream), gets a stand-in for
 * librtlsdr's async state instead: rtlsdr_cancel_async() finds nothing to cancel (-2) until the reader streams, and
 * the first cancel after that ends the stream. */
namespace {
enum RtlTestUsbReaderHold : int {
    RTL_TEST_USB_HOLD_NONE = 0,
    RTL_TEST_USB_HOLD_BEFORE_ENTRY = 1,
    RTL_TEST_USB_HOLD_BEFORE_READ = 2,
};
} // namespace

static std::atomic<int> g_test_usb_reader_active{0};
static std::atomic<int> g_test_usb_reader_rc{0};
static std::atomic<int> g_test_usb_reader_wait_stop{0};
static std::atomic<int> g_test_usb_reader_hold{RTL_TEST_USB_HOLD_NONE};
static std::atomic<int> g_test_usb_reader_held{0};
static std::atomic<int> g_test_usb_reader_cancels{0};
static std::atomic<int> g_test_usb_reader_streaming{0};
static std::atomic<int> g_test_usb_reader_streamed{0};
static std::atomic<int> g_test_usb_reader_cancelled{0};
static std::atomic<int> g_test_usb_reader_stream{0};
static std::atomic<int> g_test_usb_reader_stream_return{0};
/* USB reader returns rtl_usb_reader_finished() has handled. */
static std::atomic<unsigned int> g_test_usb_reader_returns{0U};
/* The callbacks a test delivers stand for the streaming reader's (rtl_device_test_usb_stream_deliver()), but run on the
 * test's thread: each runs under this lock, and the streaming fake takes it before it returns, so that every callback
 * delivered for a reader is ordered before its return, and through the join before whatever follows it, as a real
 * reader's callbacks are, on its own thread. */
static std::mutex g_test_usb_deliver_lock;

/* Whether the seam stands in for librtlsdr's async state (its streaming and cancel). */
static int
rtl_usb_test_fake_async(void) {
    return g_test_usb_reader_active.load(std::memory_order_acquire)
           && (g_test_usb_reader_hold.load(std::memory_order_acquire) != RTL_TEST_USB_HOLD_NONE
               || g_test_usb_reader_stream.load(std::memory_order_acquire));
}

/* At @p point, the reader a test holds there waits until a stop's first cancel attempt; bounded, so a stop that never
 * cancels fails the test rather than hanging it. */
static void
rtl_usb_test_hold_reader(int point) {
    if (!g_test_usb_reader_active.load(std::memory_order_acquire)
        || g_test_usb_reader_hold.load(std::memory_order_acquire) != point) {
        return;
    }
    g_test_usb_reader_held.store(1, std::memory_order_release);
    for (int waited_ms = 0; waited_ms < 2000 && g_test_usb_reader_cancels.load(std::memory_order_acquire) == 0;
         ++waited_ms) {
        dsd_sleep_ms(1);
    }
}
#endif

/* Whether a USB reader can start: the device is open, or a test stands in for the reader. */
static int
rtl_usb_reader_available(const struct rtl_device* s) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_reader_active.load(std::memory_order_acquire)) {
        return 1;
    }
#endif
    return s->dev != NULL;
}

/* Run the USB reader until it is cancelled, the device is lost or its transfers cannot be set up; returns
 * rtlsdr_read_async()'s result. */
static int
rtl_usb_read_async(struct rtl_device* s) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_reader_active.load(std::memory_order_acquire)) {
        rtl_usb_test_hold_reader(RTL_TEST_USB_HOLD_BEFORE_READ);
        if (g_test_usb_reader_stream.load(std::memory_order_acquire)) {
            /* Stream until a cancel reaches it, however long, or until the test makes it return by itself; the test
             * delivers the callbacks. */
            g_test_usb_reader_streaming.store(1, std::memory_order_release);
            while (!g_test_usb_reader_cancelled.load(std::memory_order_acquire)
                   && !g_test_usb_reader_stream_return.load(std::memory_order_acquire)) {
                dsd_sleep_ms(1);
            }
            g_test_usb_reader_streaming.store(0, std::memory_order_release);
            { const std::lock_guard<std::mutex> delivered(g_test_usb_deliver_lock); }
            return g_test_usb_reader_cancelled.load(std::memory_order_acquire)
                       ? 0
                       : g_test_usb_reader_rc.load(std::memory_order_acquire);
        }
        if (g_test_usb_reader_hold.load(std::memory_order_acquire) != RTL_TEST_USB_HOLD_NONE) {
            /* Stream until a cancel reaches it; 5 s at most. */
            g_test_usb_reader_streamed.store(1, std::memory_order_release);
            g_test_usb_reader_streaming.store(1, std::memory_order_release);
            for (int waited_ms = 0; waited_ms < 5000 && !g_test_usb_reader_cancelled.load(std::memory_order_acquire);
                 ++waited_ms) {
                dsd_sleep_ms(1);
            }
            g_test_usb_reader_streaming.store(0, std::memory_order_release);
            return 0;
        }
        if (g_test_usb_reader_wait_stop.load(std::memory_order_acquire)) {
            /* Bounded, so a stop that never reaches the reader fails the test rather than hanging it. */
            for (int waited_ms = 0; waited_ms < 5000 && s->run.load(std::memory_order_acquire); ++waited_ms) {
                dsd_sleep_ms(1);
            }
        }
        return g_test_usb_reader_rc.load(std::memory_order_acquire);
    }
#endif
    return rtlsdr_read_async(s->dev, rtlsdr_callback, s, 16, s->buf_len);
}

/* Ask the USB reader's rtlsdr_read_async() to return: 0 when it was streaming, else librtlsdr's refusal (-2 while
 * nothing streams yet or any more), -1 with no device. */
static int
rtl_usb_cancel_async(struct rtl_device* s) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rtl_usb_test_fake_async()) {
        g_test_usb_reader_cancels.fetch_add(1, std::memory_order_acq_rel);
        if (!g_test_usb_reader_streaming.load(std::memory_order_acquire)) {
            return -2;
        }
        g_test_usb_reader_cancelled.store(1, std::memory_order_release);
        return 0;
    }
#endif
    rtl_usb_ctrl_lock(s);
    const int rc = s->dev ? rtlsdr_cancel_async(s->dev) : -1;
    rtl_usb_ctrl_unlock(s);
    return rc;
}

/* The receiver's sample progress: the librtlsdr callbacks counted on the device, or the Airspy source's. */
static uint64_t
rtl_device_sample_progress(const struct rtl_device* s) {
    return s->backend == RTL_BACKEND_AIRSPY ? airspy_source_progress(s->airspy)
                                            : s->progress.load(std::memory_order_relaxed);
}

/* Whether the receiver delivered samples since a failure verdict made when its progress stood at @p verdict_progress,
 * which drops the verdict. Only samples count: a stream restarted since (a new generation) or an SDK that says it
 * streams does not. A returned USB reader delivers nothing more, so its verdicts always stand. */
static int
rtl_device_receiver_progressed_since(const struct rtl_device* s, uint64_t verdict_progress) {
    return rtl_device_sample_progress(s) != verdict_progress;
}

/* Whether a stall recovery owns the receiver's outcome for a verdict made at @p epoch: one is pending or running, or
 * one was requested since. Its own result reports (rtl_device_usb_recovery_finish()). */
static int
rtl_device_recovery_supersedes(const struct rtl_device* s, uint32_t epoch) {
    return s->recovery_request.load(std::memory_order_acquire) != RTL_USB_RECOVERY_IDLE
           || s->recovery_epoch.load(std::memory_order_acquire) != epoch;
}

/* Whether a verdict made at @p epoch, when the receiver's progress stood at @p verdict_progress, no longer ends the
 * session: a stop, an exit request, samples since, or a stall recovery that took the receiver over. */
static int
rtl_device_verdict_dropped(const struct rtl_device* s, uint64_t verdict_progress, uint32_t epoch) {
    return !s->run.load(std::memory_order_acquire) || dsd_exitflag_load()
           || rtl_device_receiver_progressed_since(s, verdict_progress) || rtl_device_recovery_supersedes(s, epoch);
}

/* The one path that ends a session for its receiver: an RTL-SDR, Airspy or SoapySDR receiver stopped delivering, and unless a stop or an exit
 * request comes first, @p cause is logged, the session ends as a device failure of the input (@p native_code kept for
 * diagnostics), the receiver's ending is committed and the demod waiting on the input ring is woken. The cause comes
 * first, so the teardown that the report starts on other threads logs after it. While an unplug owner's grace is
 * set (rtl_device_set_unplug_owner_grace_ms()), the verdict first waits it out, polling the session: a stop inside it
 * (an unplug the owner handled) or samples since the verdict, made when the receiver's progress stood at
 * @p verdict_progress, keep it silent, and observing the session does not restart the deadline. A stall recovery
 * pending or running when the verdict is made, or requested while it waits, keeps it silent too: the recovery's own
 * result decides. Of verdicts that race (a reader exit and the progress monitor), only the first reports. Returns 1
 * when it reported. */
static int
rtl_device_end_for_receiver(struct rtl_device* s, int native_code, uint64_t verdict_progress, const char* cause) {
    const uint32_t epoch = s->recovery_epoch.load(std::memory_order_acquire);
    const uint32_t grace_ms = rtl_device_unplug_owner_grace_ms();
    if (grace_ms > 0U) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
        g_test_verdicts_in_grace.fetch_add(1U, std::memory_order_acq_rel);
#endif
        const uint64_t deadline_ms = dsd_realtime_mono_ms() + grace_ms;
        while (dsd_realtime_mono_ms() < deadline_ms) {
            if (rtl_device_verdict_dropped(s, verdict_progress, epoch)) {
                return 0;
            }
            dsd_sleep_ms(10);
        }
    }
    if (rtl_device_verdict_dropped(s, verdict_progress, epoch)) {
        return 0;
    }
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    const uint32_t pause_ms = g_test_verdict_pause_ms.exchange(0U, std::memory_order_acq_rel);
    if (pause_ms > 0U) {
        g_test_verdicts_paused.fetch_add(1U, std::memory_order_acq_rel);
        dsd_sleep_ms(pause_ms);
    }
#endif
    /* Two verdicts can both get this far (a reader exit and the monitor's, together); the first one reports. */
    if (s->receiver_end_claimed.exchange(1, std::memory_order_acq_rel) != 0) {
        return 0;
    }
    LOG_ERROR("%s\n", cause);
    dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, native_code);
    dsd_input_failure_commit_receiver_end();
    dsd_exitflag_store(1);
    replay_signal_input_waiters(s);
    return 1;
}

/* What the USB reader's return means for the session. A stop clears run before it cancels, so a return with run still
 * set (and no exit requested) is one nobody asked for, whatever its code: -1 when librtlsdr lost the device
 * (unplugged, or so many transfer errors that it gave up), a failed transfer submission or event poll, or even 0. Any
 * of them leaves the session waiting on samples that never come, so it ends the session for its receiver
 * (rtl_device_end_for_receiver(), after the unplug-owner grace that lets Android's USB detach broadcast own an
 * unplug). -ENOMEM is the host's: librtlsdr could not allocate its transfers, so it is reported at once, without the
 * grace and without committing the receiver's ending. A cancel's return is left alone, and the return of a reader a
 * recovery restarted, before that recovery finished, is the recovery's to report. */
static void
rtl_usb_reader_end_session(struct rtl_device* s, int rc) {
    /* A dongle behind an Android descriptor whose reader gave up owes a reset before its next open, as a stalled one
     * does (#782): the Listen notice promises Reconnect resets it, and the worst it costs is one reset that was not
     * needed. An unplug's detach drops it again on the host. */
    rtl_usb_reset_owed_set(s);
    char cause[96];
    DSD_SNPRINTF(cause, sizeof cause, "RTL-SDR: the USB %s; the stream stops (rc=%d)",
                 rc == -1 ? "device was lost" : "reader stopped unexpectedly", rc);
    (void)rtl_device_end_for_receiver(s, rc, rtl_device_sample_progress(s), cause);
}

/* Where the return of the reader a recovery restarted stands (recovery_handoff's low half): NONE, no such reader;
 * PENDING, its recovery has not finished and it has not returned; FAILED, it returned first, and the finish reports
 * it; CLOSED, the finish came first, and a return from now on reports itself, with the request idle. */
namespace {
enum rtl_usb_handoff_state : uint32_t {
    RTL_USB_HANDOFF_NONE = 0U,
    RTL_USB_HANDOFF_PENDING = 1U,
    RTL_USB_HANDOFF_FAILED = 2U,
    RTL_USB_HANDOFF_CLOSED = 3U,
};
} // namespace

static uint64_t
rtl_usb_handoff_word(uint32_t incarnation, uint32_t state) {
    return ((uint64_t)incarnation << 32) | (uint64_t)state;
}

static uint32_t
rtl_usb_handoff_state_of(uint64_t word) {
    return (uint32_t)(word & 0xFFFFFFFFU);
}

/* The reader a stall recovery restarted (incarnation @p incarnation) can return, @p rc, before that recovery
 * finishes. Its verdict would then be dropped as the recovery's own (rtl_device_recovery_supersedes(), there for the
 * retired reader), and the recovery would report success with no reader running. One compare-and-swap of its own
 * hand-off, PENDING to FAILED, decides it, whatever the request says: done, the finish reports it
 * (rtl_device_usb_recovery_finish()); not done, the hand-off was closed (or belongs to another reader) and the caller
 * reports it. Returns 1 when the finish reports it. */
static int
rtl_usb_recovery_reader_handoff(struct rtl_device* s, int rc, uint32_t incarnation) {
    if (incarnation == 0U) {
        return 0;
    }
    s->recovery_reader_rc.store(rc, std::memory_order_relaxed);
    uint64_t expected = rtl_usb_handoff_word(incarnation, RTL_USB_HANDOFF_PENDING);
    return s->recovery_handoff.compare_exchange_strong(expected,
                                                       rtl_usb_handoff_word(incarnation, RTL_USB_HANDOFF_FAILED),
                                                       std::memory_order_acq_rel, std::memory_order_acquire)
               ? 1
               : 0;
}

static void
rtl_usb_reader_finished(struct rtl_device* s, int rc, uint32_t incarnation) {
    /* A recovery cancelled it: the swap reports for itself. */
    if (!s->run.load(std::memory_order_acquire) || dsd_exitflag_load()
        || s->recovering.load(std::memory_order_acquire)) {
        return;
    }
    if (rc == -ENOMEM) {
        LOG_ERROR("RTL-SDR: could not allocate USB transfer buffers (rc=%d)\n", rc);
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, rc);
        dsd_exitflag_store(1);
        replay_signal_input_waiters(s);
        return;
    }
    if (rtl_usb_recovery_reader_handoff(s, rc, incarnation)) {
        return;
    }
    rtl_usb_reader_end_session(s, rc);
}

/**
 * @brief RTL-SDR USB thread entry: reads samples asynchronously into the input ring.
 * Applies optional realtime scheduling/affinity if configured.
 *
 * @param arg Pointer to `rtl_device`.
 * @return NULL on exit.
 */
static DSD_THREAD_RETURN_TYPE DSD_RTL_THREAD_CALL
dongle_thread_fn(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    maybe_set_thread_realtime_and_affinity("DONGLE");
    /* No other reader starts before this one is joined, so the value is this reader's. */
    const uint32_t incarnation = s->reader_incarnation.load(std::memory_order_acquire);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    rtl_usb_test_hold_reader(RTL_TEST_USB_HOLD_BEFORE_ENTRY);
#endif
    s->reader_entered.store(1, std::memory_order_release);
    int rc = 0;
    /* A stop that cleared run before this point found nothing to cancel (librtlsdr refuses until the reader streams);
     * the reader must not start streaming for it. A stop after the check keeps cancelling until the reader exits. */
    if (s->run.load(std::memory_order_acquire)) {
#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
        __try {
            rc = rtl_usb_read_async(s);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            DSD_FPRINTF(
                stderr,
                "ERROR: libusb exception in rtlsdr_read_async (MSVC/Windows). "
                "Check that the bundled libusb/librtlsdr DLLs match the build and the device driver is installed.\n");
            dsd_exitflag_store(1);
        }
#else
        rc = rtl_usb_read_async(s);
#endif
    }
    s->reader_exited.store(1, std::memory_order_release);
    rtl_usb_reader_finished(s, rc, incarnation);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_usb_reader_returns.fetch_add(1U, std::memory_order_acq_rel);
#endif
    DSD_THREAD_RETURN;
}

/* ---- USB sample-progress monitor (RTL-SDR and Airspy) ----
 * A receiver can stay enumerated and stop delivering samples with nothing returning: librtlsdr drops a transfer for
 * good on each error status and then waits forever once none is left, and a device that stops completing transfers
 * (they have no timeout) leaves both SDKs waiting. Every receive callback with bytes counts progress first (one
 * relaxed increment, no clock read), and a monitor samples the count every 100 ms on the realtime clock. Silence
 * accrues by the time between samples while the stream is up and not being reconfigured, does not reset when the
 * stream restarts, and only observed progress ends it. 5 s of it ends the session for its receiver
 * (rtl_device_end_for_receiver(), through the unplug-owner grace), unless the RTL-SDR's stall recovery takes it over
 * (rtl_usb_progress_monitor()). */
static const uint32_t RTL_PROGRESS_SILENCE_LIMIT_MS = 5000U;
static const uint32_t RTL_PROGRESS_PERIOD_MS = 100U;
/* Stall recovery budget (D2): none within 60 s of the previous request, and a request the stream's controller has not
 * taken within 2 s ends the session instead. */
static const uint32_t RTL_RECOVERY_COOLDOWN_MS = 60000U;
static const uint32_t RTL_RECOVERY_TAKE_MS = 2000U;

static uint32_t
rtl_progress_limit_ms(void) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    const uint32_t test_ms = g_test_progress_limit_ms.load(std::memory_order_relaxed);
    if (test_ms > 0U) {
        return test_ms;
    }
#endif
    return RTL_PROGRESS_SILENCE_LIMIT_MS;
}

static uint32_t
rtl_progress_period_ms(void) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    const uint32_t test_ms = g_test_progress_period_ms.load(std::memory_order_relaxed);
    if (test_ms > 0U) {
        return test_ms;
    }
#endif
    return RTL_PROGRESS_PERIOD_MS;
}

static uint32_t
rtl_recovery_cooldown_ms(void) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    const uint32_t test_ms = g_test_recovery_cooldown_ms.load(std::memory_order_relaxed);
    if (test_ms > 0U) {
        return test_ms;
    }
#endif
    return RTL_RECOVERY_COOLDOWN_MS;
}

static uint32_t
rtl_recovery_take_ms(void) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    const uint32_t test_ms = g_test_recovery_take_ms.load(std::memory_order_relaxed);
    if (test_ms > 0U) {
        return test_ms;
    }
#endif
    return RTL_RECOVERY_TAKE_MS;
}

namespace {
struct rtl_progress_watch {
    uint64_t progress;   /* the count last observed */
    uint64_t sampled_ms; /* when the last period ended */
    uint64_t silence_ms; /* accrued since progress was last observed */
};
} // namespace

static void
rtl_progress_watch_begin(rtl_progress_watch* w, uint64_t progress) {
    w->progress = progress;
    w->sampled_ms = dsd_realtime_mono_ms();
    w->silence_ms = 0U;
}

/* One sample at the end of a period: progress ends the silence; with none, the period counts as silence while
 * @p accruing. Returns 1 once the silence reached the limit. */
static int
rtl_progress_watch_step(rtl_progress_watch* w, uint64_t progress, int accruing) {
    const uint64_t now_ms = dsd_realtime_mono_ms();
    const uint64_t period_ms = now_ms - w->sampled_ms;
    w->sampled_ms = now_ms;
    if (progress != w->progress) {
        w->progress = progress;
        w->silence_ms = 0U;
        return 0;
    }
    if (accruing) {
        w->silence_ms += period_ms;
    }
    return w->silence_ms >= rtl_progress_limit_ms();
}

/* The monitor's lock and condition, made by the first RTL-SDR, Airspy or SoapySDR start; 0 when they are ready. */
static int
rtl_monitor_sync_init(struct rtl_device* s) {
    if (s->monitor_sync_inited) {
        return 0;
    }
    if (dsd_mutex_init(&s->monitor_lock) != 0) {
        return -1;
    }
    if (dsd_cond_init_monotonic(&s->monitor_wake) != 0) {
        (void)dsd_mutex_destroy(&s->monitor_lock);
        return -1;
    }
    s->monitor_sync_inited = 1;
    return 0;
}

/* Wait out one monitor period, or less once a stop cleared run or the monitor's own flag and woke it. */
static void
rtl_monitor_wait(struct rtl_device* s) {
    const uint64_t deadline_ns = dsd_realtime_mono_ns() + (uint64_t)rtl_progress_period_ms() * 1000000ULL;
    dsd_mutex_lock(&s->monitor_lock);
    if (s->run.load(std::memory_order_acquire) && s->progress_monitor_run.load(std::memory_order_acquire)) {
        (void)dsd_cond_timedwait_monotonic(&s->monitor_wake, &s->monitor_lock, deadline_ns);
    }
    dsd_mutex_unlock(&s->monitor_lock);
}

/* Wake a monitor waiting out its period; the caller cleared run or the monitor's flag first. */
static void
rtl_monitor_wake(struct rtl_device* s) {
    if (!s->monitor_sync_inited) {
        return;
    }
    dsd_mutex_lock(&s->monitor_lock);
    (void)dsd_cond_broadcast(&s->monitor_wake);
    dsd_mutex_unlock(&s->monitor_lock);
}

/* Whether a monitor keeps watching: its device streams, nobody stopped it, and the session is not ending. */
static int
rtl_monitor_running(const struct rtl_device* s) {
    return s->run.load(std::memory_order_acquire) && s->progress_monitor_run.load(std::memory_order_acquire)
           && !dsd_exitflag_load();
}

/* The stall recovery the RTL-SDR's progress monitor keeps for its device: the request it posted last (when, and the
 * rearm sequence then), and the watch since the restarted reader's baseline. Owned by the monitor thread. */
namespace {
struct rtl_recovery_budget {
    int requested;         /* a recovery was requested on this device */
    uint64_t requested_ms; /* when the last one was */
    uint32_t rearm_seq;    /* watch_rearm_seq as last seen */
    int rearmed;           /* the watch restarted from a restarted reader's baseline */
    uint64_t baseline;     /* ... that baseline */
};
} // namespace

/* Why a silence verdict on @p s cannot be handed to a stall recovery, written to @p cause (which then ends the
 * session), or NULL when a recovery may be requested. A device that was never eligible, or whose stream registered no
 * recovery, ends as it always did. */
static const char*
rtl_usb_recovery_refusal(const struct rtl_device* s, const rtl_recovery_budget* b, const rtl_progress_watch* w,
                         uint64_t now_ms, char* cause, size_t cause_size) {
    rtl_usb_ctrl_lock(s);
    const int have_cb = s->recovery_cb != nullptr;
    rtl_usb_ctrl_unlock(s);
    if (!rtl_device_usb_recovery_eligible(s) || !have_cb) {
        DSD_SNPRINTF(cause, cause_size, "RTL-SDR: the receiver sent no samples for %llu ms; the stream stops",
                     (unsigned long long)w->silence_ms);
        return cause;
    }
    /* One recovery per stall: a reader restarted by the last one that delivered nothing since is not a new stall. */
    if (b->rearmed && w->progress == b->baseline) {
        DSD_SNPRINTF(cause, cause_size, "RTL-SDR: the receiver sent no samples for %llu ms, none since it was %s",
                     (unsigned long long)w->silence_ms,
                     s->reopened_without_reset.load(std::memory_order_acquire)
                         ? "reopened (its USB reset was refused); the stream stops"
                         : "reset and reopened; the stream stops");
        return cause;
    }
    if (b->requested && now_ms - b->requested_ms < rtl_recovery_cooldown_ms()) {
        DSD_SNPRINTF(cause, cause_size,
                     "RTL-SDR: the receiver sent no samples for %llu ms, %llu"
                     " ms after it was last reset; the stream stops",
                     (unsigned long long)w->silence_ms, (unsigned long long)(now_ms - b->requested_ms));
        return cause;
    }
    return nullptr;
}

/* The reset obligation the RTL-SDR's progress monitor set for its descriptor (#782): at the progress count of the
 * verdict that set it, so samples after it clear it again. Owned by the monitor thread. A posted recovery owns the
 * obligation from then on (it resets, or keeps it), so the watch ends there, and it starts again from a restarted
 * reader's baseline only if that recovery kept it. */
namespace {
struct rtl_usb_owed_watch {
    int set;           /* this monitor's verdict set it */
    uint64_t progress; /* ... when the progress count stood here */
};
} // namespace

/* A stall verdict on @p s, made when its progress stood at @p progress: a dongle behind an Android descriptor owes a
 * reset before its next open from now on, before any grace and whatever Stop follows (#782). */
static void
rtl_usb_owed_on_verdict(const struct rtl_device* s, rtl_usb_owed_watch* w, uint64_t progress) {
    if (!s->owns_preopened_fd) {
        return;
    }
    rtl_usb_reset_owed_set(s);
    w->set = 1;
    w->progress = progress;
}

/* Samples verified since the verdict that set the obligation clear it: the dongle streams without a reset. Only while
 * the reader still streams (@p streaming): one that returned set the obligation for its own ending, whatever it
 * delivered before. The reader can still deliver its last block and return after @p streaming was read, setting the
 * obligation for its ending just before the clear takes it, so a clear that finds the reader gone sets it again: the
 * worst that costs, for a reader a stop or a recovery cancelled, is one reset that was not needed. */
static void
rtl_usb_owed_on_progress(const struct rtl_device* s, rtl_usb_owed_watch* w, uint64_t progress, int streaming) {
    if (w->set && streaming && progress != w->progress) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
        g_test_owed_clear_hook.invoke(s);
#endif
        w->set = 0;
        if (rtl_usb_reset_owed_clear(s->preopened_fd) && s->reader_exited.load(std::memory_order_acquire)) {
            rtl_usb_reset_owed_set(s);
        }
    }
}

/* An obligation @p s's descriptor owes now (one it came with, or one a recovery that reopened without its reset kept)
 * is cleared by samples from @p progress on, as one this monitor set would be. */
static void
rtl_usb_owed_adopt(const struct rtl_device* s, rtl_usb_owed_watch* w, uint64_t progress) {
    if (s->owns_preopened_fd && rtl_device_preopened_fd_reset_owed() == s->preopened_fd) {
        w->set = 1;
        w->progress = progress;
    }
}

/* How the pre-recovery grace (rtl_usb_recovery_grace()) ended. */
namespace {
enum rtl_usb_grace_result : int {
    RTL_USB_GRACE_PASSED = 0,  /* still silent, still the session's: recover */
    RTL_USB_GRACE_RESUMED = 1, /* samples since the verdict: no stall after all */
    RTL_USB_GRACE_LEFT = 2,    /* a stop, an exit request or a detach (which stops the session) */
    RTL_USB_GRACE_MOVED = 3,   /* the host released or replaced the receiver's connection */
};
} // namespace

/* Before a stall recovery resets a dongle behind an Android descriptor, wait out the unplug-owner grace
 * (rtl_device_set_unplug_owner_grace_ms()) as a session's ending does (#782): a stall can be an unplug whose detach
 * broadcast is on its way, and the host owns that. Polled until the deadline and once more at it: samples since the
 * verdict (made at @p verdict_progress), a stop, an exit request or the monitor's own stop, and the connection's
 * publish each end it early. Desktop receivers, and a host with no grace, recover at once. */
static rtl_usb_grace_result
rtl_usb_recovery_grace(const struct rtl_device* s, uint64_t verdict_progress) {
    const uint32_t grace_ms = rtl_device_unplug_owner_grace_ms();
    if (!s->owns_preopened_fd || grace_ms == 0U) {
        return RTL_USB_GRACE_PASSED;
    }
    const uint64_t started_ms = dsd_realtime_mono_ms();
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    /* The start first: a test that sees the count reads this grace's start. */
    g_test_recovery_grace_started_ms.store(started_ms, std::memory_order_release);
    g_test_recoveries_in_grace.fetch_add(1U, std::memory_order_acq_rel);
#endif
    const uint64_t deadline_ms = started_ms + grace_ms;
    rtl_usb_grace_result result = RTL_USB_GRACE_PASSED;
    for (;;) {
        if (!rtl_monitor_running(s)) {
            result = RTL_USB_GRACE_LEFT;
            break;
        }
        if (rtl_device_receiver_progressed_since(s, verdict_progress)) {
            result = RTL_USB_GRACE_RESUMED;
            break;
        }
        if (rtl_usb_preopened_fd_moved(s)) {
            result = RTL_USB_GRACE_MOVED;
            break;
        }
        if (dsd_realtime_mono_ms() >= deadline_ms) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
            if (g_test_hold_recovery_grace.load(std::memory_order_acquire) != 0) {
                dsd_sleep_ms(10);
                continue;
            }
#endif
            result = RTL_USB_GRACE_PASSED;
            break;
        }
        dsd_sleep_ms(10);
    }
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_recovery_grace_result.store(static_cast<int>(result), std::memory_order_release);
    g_test_recovery_graces_ended.fetch_add(1U, std::memory_order_acq_rel);
#endif
    return result;
}

/* Post a stall recovery request for @p s to its stream's controller (the registered callback). */
static void
rtl_usb_recovery_post(struct rtl_device* s, uint64_t silence_ms) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_recovery_post_hook.invoke(s);
#endif
    rtl_usb_ctrl_lock(s);
    const rtl_device_recovery_cb cb = s->recovery_cb;
    void* cb_ctx = s->recovery_cb_ctx;
    rtl_usb_ctrl_unlock(s);
    s->recovery_silence_ms.store(silence_ms, std::memory_order_relaxed);
    /* Pending first, then the epoch: a verdict that read the epoch before the bump finds the request pending. */
    s->recovery_request.store(RTL_USB_RECOVERY_REQUESTED, std::memory_order_release);
    s->recovery_epoch.fetch_add(1U, std::memory_order_acq_rel);
    if (cb) {
        cb(cb_ctx);
    }
}

/* The RTL-SDR's progress monitor's state, owned by its thread. */
namespace {
struct rtl_usb_monitor {
    struct rtl_device* s;
    rtl_progress_watch watch;
    rtl_recovery_budget budget;
    rtl_usb_owed_watch owed;
    char cause[192];
};
} // namespace

/* A recovery request still pending: withdrawn once the controller has not taken it in time, which ends the session.
 * Whether the monitor stops (it ended the session). */
static int
rtl_usb_monitor_withdraw_request(rtl_usb_monitor* m) {
    struct rtl_device* s = m->s;
    if (dsd_realtime_mono_ms() - m->budget.requested_ms < rtl_recovery_take_ms()) {
        return 0;
    }
    int expected = RTL_USB_RECOVERY_REQUESTED;
    if (!s->recovery_request.compare_exchange_strong(expected, RTL_USB_RECOVERY_IDLE, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
        return 0;
    }
    DSD_SNPRINTF(m->cause, sizeof m->cause,
                 "RTL-SDR: the receiver sent no samples for %llu"
                 " ms and its reset did not start within %u ms; the stream stops",
                 (unsigned long long)s->recovery_silence_ms.load(std::memory_order_relaxed),
                 (unsigned)rtl_recovery_take_ms());
    if (rtl_device_end_for_receiver(s, -ETIMEDOUT, s->progress.load(std::memory_order_relaxed), m->cause)) {
        (void)rtl_usb_cancel_async(s);
        return 1;
    }
    return 0;
}

/* A recovery that restarted the reader moved watch_rearm_seq: the watch restarts from the reader's baseline. */
static void
rtl_usb_monitor_follow_rearm(rtl_usb_monitor* m) {
    const struct rtl_device* s = m->s;
    const uint32_t rearm_seq = s->watch_rearm_seq.load(std::memory_order_acquire);
    if (rearm_seq == m->budget.rearm_seq) {
        return;
    }
    /* Callbacks since the baseline, taken before the reader thread started, are progress already. */
    m->budget.rearm_seq = rearm_seq;
    m->budget.rearmed = 1;
    m->budget.baseline = s->progress_baseline.load(std::memory_order_relaxed);
    rtl_progress_watch_begin(&m->watch, m->budget.baseline);
    /* A recovery that reopened without its reset kept the obligation: the restarted reader's samples clear it, as a
     * session's first samples do. */
    rtl_usb_owed_adopt(s, &m->owed, m->budget.baseline);
}

/* A silence verdict on the monitor's device: handed to a stall recovery when one may be requested and the grace
 * passes, or the session ends for its receiver (cause written). Whether the monitor stops (it ended the session). */
static int
rtl_usb_monitor_on_silence(rtl_usb_monitor* m) {
    struct rtl_device* s = m->s;
    rtl_usb_owed_on_verdict(s, &m->owed, m->watch.progress);
    if (!rtl_usb_recovery_refusal(s, &m->budget, &m->watch, dsd_realtime_mono_ms(), m->cause, sizeof m->cause)) {
        const rtl_usb_grace_result grace = rtl_usb_recovery_grace(s, m->watch.progress);
        if (grace == RTL_USB_GRACE_RESUMED) {
            const uint64_t resumed = s->progress.load(std::memory_order_relaxed);
            rtl_usb_owed_on_progress(s, &m->owed, resumed, !s->reader_exited.load(std::memory_order_acquire));
            rtl_progress_watch_begin(&m->watch, resumed);
            return 0;
        }
        if (grace == RTL_USB_GRACE_LEFT) {
            return 0;
        }
        if (grace == RTL_USB_GRACE_PASSED) {
            m->budget.requested = 1;
            m->budget.requested_ms = dsd_realtime_mono_ms();
            /* The recovery owns the obligation from here: a block the old reader delivers before the swap cancels it
             * must not clear what a failed recovery's finish sets. */
            m->owed.set = 0;
            rtl_usb_recovery_post(s, m->watch.silence_ms);
            return 0;
        }
        DSD_SNPRINTF(m->cause, sizeof m->cause,
                     "RTL-SDR: the receiver sent no samples for %llu"
                     " ms and its USB connection was released, so it was not reset; the stream stops",
                     (unsigned long long)m->watch.silence_ms);
    }
    if (rtl_device_end_for_receiver(s, -ETIMEDOUT, m->watch.progress, m->cause)) {
        (void)rtl_usb_cancel_async(s);
        return 1;
    }
    return 0;
}

/* One period of the monitor. Whether it stops (it ended the session). */
static int
rtl_usb_monitor_step(rtl_usb_monitor* m) {
    const struct rtl_device* s = m->s;
    const int request = s->recovery_request.load(std::memory_order_acquire);
    if (request == RTL_USB_RECOVERY_REQUESTED) {
        return rtl_usb_monitor_withdraw_request(m);
    }
    if (request == RTL_USB_RECOVERY_TAKEN) {
        return 0;
    }
    rtl_usb_monitor_follow_rearm(m);
    const int streaming =
        s->reader_entered.load(std::memory_order_acquire) && !s->reader_exited.load(std::memory_order_acquire);
    const uint64_t progress = s->progress.load(std::memory_order_relaxed);
    rtl_usb_owed_on_progress(s, &m->owed, progress, streaming);
    if (!rtl_progress_watch_step(&m->watch, progress, streaming)) {
        return 0;
    }
    return rtl_usb_monitor_on_silence(m);
}

/* The RTL-SDR's progress monitor, started with the reader. Silence accrues while the reader is inside
 * rtlsdr_read_async() (a reader that returned is rtl_usb_reader_finished()'s). At the limit, an eligible receiver
 * within its budget (one recovery per stall, none within the cooldown of the previous request) is handed to its
 * stream's controller as a recovery request, and the watch pauses while the request is pending or running; a request
 * not taken in time is withdrawn and the session ends. A recovery that restarted the reader moves watch_rearm_seq, and
 * the watch restarts from the reader's baseline with no silence accrued. Every other verdict ends the session for its
 * receiver and cancels the reader, as nothing else would make it return. */
static DSD_THREAD_RETURN_TYPE DSD_RTL_THREAD_CALL
rtl_usb_progress_monitor(void* arg) {
    rtl_usb_monitor m{};
    m.s = static_cast<rtl_device*>(arg);
    rtl_progress_watch_begin(&m.watch, m.s->progress.load(std::memory_order_relaxed));
    m.budget.rearm_seq = m.s->watch_rearm_seq.load(std::memory_order_acquire);
    /* An obligation the descriptor came with, whose reset at open was refused, is cleared by this session's first
     * samples as one this monitor set would be. */
    rtl_usb_owed_adopt(m.s, &m.owed, m.watch.progress);
    while (rtl_monitor_running(m.s)) {
        rtl_monitor_wait(m.s);
        if (!rtl_monitor_running(m.s)) {
            break;
        }
        if (rtl_usb_monitor_step(&m)) {
            break;
        }
    }
    DSD_THREAD_RETURN;
}

/* Start @p dev's progress monitor thread, running @p monitor; a test can refuse it as a failed thread create does. */
static int
rtl_progress_monitor_create(struct rtl_device* dev, dsd_thread_fn monitor) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_refuse_progress_monitor.load(std::memory_order_acquire)) {
        return EAGAIN;
    }
#endif
    return dsd_thread_create(&dev->progress_monitor, monitor, dev);
}

/* Start the RTL-SDR's progress monitor thread. */
static int
rtl_usb_progress_monitor_create(struct rtl_device* dev) {
    return rtl_progress_monitor_create(dev, rtl_usb_progress_monitor);
}

/* Stop and join the RTL-SDR's or the SoapySDR device's progress monitor, if it runs. One waiting out the unplug-owner
 * grace returns once run is clear. */
static void
rtl_progress_monitor_stop(struct rtl_device* dev) {
    if (!dev->progress_monitor_started) {
        return;
    }
    dev->progress_monitor_run.store(0, std::memory_order_release);
    rtl_monitor_wake(dev);
    dsd_thread_join(dev->progress_monitor);
    dev->progress_monitor_started = 0;
}

/* Cancel the USB reader every 1 ms until its thread exited, and join it. One cancel is not enough: until the reader is
 * inside rtlsdr_read_async(), librtlsdr refuses the cancel (-2), and a reader that then started streaming would have
 * nobody left to cancel it, so the join would never return. Under usb_lifecycle_lock. */
static void
rtl_usb_reader_cancel_and_join(struct rtl_device* dev) {
    for (;;) {
        (void)rtl_usb_cancel_async(dev);
        if (dev->reader_exited.load(std::memory_order_acquire)) {
            break;
        }
        dsd_sleep_ms(1);
    }
    dsd_thread_join(dev->thread);
    dev->thread_started = 0;
}

/* The one shutdown handshake for a USB reader, shared by rtl_device_stop_async() and rtl_device_destroy(): clear run,
 * stop and join the progress monitor (so it cannot cancel alongside), then end the reader, if one runs (a recovery
 * may have retired it). Under usb_lifecycle_lock. */
static void
rtl_usb_reader_stop(struct rtl_device* dev) {
    dev->run.store(0, std::memory_order_release);
    rtl_progress_monitor_stop(dev);
    if (dev->thread_started) {
        rtl_usb_reader_cancel_and_join(dev);
    }
}

static void
rtl_device_copy_cstr(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", src ? src : "");
    dst[dst_size - 1] = '\0';
}

#if defined(USE_SOAPYSDR) || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
static std::string
soapy_trim_copy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace((unsigned char)value[start])) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace((unsigned char)value[end - 1])) {
        end--;
    }
    return value.substr(start, end - start);
}
#endif

#ifdef USE_SOAPYSDR
/* Mark a control or setup transaction holding soapy_lock (soapy_control_hold), once the lock is held: one still
 * waiting behind a hung read must not pause the silence count. */
static void
soapy_control_hold_mark(struct rtl_device* dev, int held) {
    dev->soapy_control_hold.store(held, std::memory_order_relaxed);
}

template <typename Fn>
static int
soapy_call_locked(struct rtl_device* dev, const char* op, Fn&& fn) {
    if (!dev || !dev->soapy_lock_inited) {
        return -1;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex for %s.\n", op);
        return -1;
    }
    /* Looked at under the lock: a reader whose stream failed releases the device under it. */
    if (!dev->soapy_dev) {
        (void)dsd_mutex_unlock(&dev->soapy_lock);
        return -1;
    }
    soapy_control_hold_mark(dev, 1);
    int ret = -1;
    try {
        ret = fn();
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: exception in %s: %s\n", op, e.what());
        ret = -1;
    }
    soapy_control_hold_mark(dev, 0);
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

static int
soapy_apply_antenna_locked(struct rtl_device* dev) {
    if (!dev || !dev->soapy_dev) {
        return 0;
    }
    const dsdneo::SoapyProfile& profile = dsdneo::soapy_profile_by_id((dsdneo::SoapyProfileId)dev->soapy_profile_id);
    const dsdneo::SoapyAntennaChoice choice = dsdneo::soapy_choose_antenna(
        profile.default_antenna, dev->soapy_requested_antenna, (double)dev->soapy_startup_center_freq_hz);
    if (choice.name.empty()) {
        return 0;
    }
    std::vector<std::string> antennas = dev->soapy_dev->listAntennas(SOAPY_SDR_RX, 0);
    if (!dsdneo::soapy_name_list_contains(antennas, choice.name)) {
        if (choice.auto_selected) {
            /* The profile guessed; leave the driver default rather than failing the open. */
            DSD_FPRINTF(stderr, "SoapySDR: %s profile antenna '%s' is unavailable; leaving driver default.\n",
                        profile.name, choice.name.c_str());
            return 0;
        }
        DSD_FPRINTF(stderr, "SoapySDR: antenna '%s' is unavailable; choices=[%s].\n", choice.name.c_str(),
                    dsdneo::soapy_join_names(antennas, 160).c_str());
        return DSD_ERR_NOT_SUPPORTED;
    }
    dev->soapy_dev->setAntenna(SOAPY_SDR_RX, 0, choice.name);
    if (choice.auto_selected) {
        DSD_FPRINTF(stderr, "SoapySDR: selected antenna '%s' for %.4f MHz (%s profile default).\n", choice.name.c_str(),
                    (double)dev->soapy_startup_center_freq_hz / 1e6, profile.name);
    } else {
        DSD_FPRINTF(stderr, "SoapySDR: selected antenna '%s'.\n", choice.name.c_str());
    }
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
    if (!dsd::io::radio::rtl_is_finite(minimum) || !dsd::io::radio::rtl_is_finite(maximum) || maximum < minimum) {
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

#if defined(USE_SOAPYSDR) && defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
/* The SoapySDR stream seam (rtl_stream_test_support.h): while a driver is attached, the device's stream calls reach it
 * in place of its SoapySDR::Device, which is a sentinel never dereferenced. */
static std::atomic<const rtl_device_test_soapy_driver*> g_test_soapy_driver{nullptr};
static std::atomic<unsigned int> g_test_soapy_stream_closes{0U};
/* What the seam's device and stream point at while it is attached: a distinct address each, never dereferenced. */
static unsigned char g_test_soapy_handle_tokens[2];
static std::atomic<int> g_test_soapy_internal_read_failure{0};
static std::atomic<int> g_test_soapy_refuse_reader{0};

static int
soapy_test_driver_result(int rc) {
    if (rc == RTL_DEVICE_TEST_SOAPY_THROW) {
        throw std::runtime_error("test driver failure");
    }
    return rc;
}

/* The seam's stream close: the sentinels are dropped, so the driver's own close finds nothing to do. */
static void
soapy_test_close_stream_locked(struct rtl_device* dev, int unmake_device) {
    if (!g_test_soapy_driver.load(std::memory_order_acquire)) {
        return;
    }
    if (dev->soapy_stream) {
        g_test_soapy_stream_closes.fetch_add(1U, std::memory_order_relaxed);
        dev->soapy_stream = NULL;
    }
    if (unmake_device) {
        dev->soapy_dev = NULL;
    }
}

static_assert(RTL_DEVICE_TEST_SOAPY_TIMEOUT == SOAPY_SDR_TIMEOUT, "seam timeout code");
static_assert(RTL_DEVICE_TEST_SOAPY_STREAM_ERROR == SOAPY_SDR_STREAM_ERROR, "seam stream error code");
static_assert(RTL_DEVICE_TEST_SOAPY_OVERFLOW == SOAPY_SDR_OVERFLOW, "seam overflow code");
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
    soapy_control_hold_mark(dev, 1);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    soapy_test_close_stream_locked(dev, unmake_device);
#endif
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
    soapy_control_hold_mark(dev, 0);
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

/* Set up and activate the RX stream, under soapy_lock: 0, activateStream()'s error code, or -EIO for any other
 * failure. @p io_mtu_elems takes the stream's MTU when the driver reports one. Throws what the driver throws. */
static int
soapy_open_stream_locked(struct rtl_device* dev, size_t* io_mtu_elems) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (const rtl_device_test_soapy_driver* fake = g_test_soapy_driver.load(std::memory_order_acquire)) {
        const int rc = soapy_test_driver_result(fake->setup(fake->ctx, io_mtu_elems));
        if (rc == 0) {
            /* Never dereferenced: the seam's cleanup closes it. */
            dev->soapy_stream = reinterpret_cast<SoapySDR::Stream*>(&g_test_soapy_handle_tokens[1]);
            dev->soapy_mtu_elems = (uint32_t)(*io_mtu_elems);
        }
        return rc;
    }
#endif
    std::string stream_format;
    if (!dev->soapy_dev || soapy_refresh_stream_format_locked(dev, &stream_format) != 0) {
        return -EIO;
    }
    std::vector<size_t> channels(1, 0);
    SoapySDR::Kwargs args;
    dev->soapy_stream = dev->soapy_dev->setupStream(SOAPY_SDR_RX, stream_format, channels, args);
    if (!dev->soapy_stream) {
        DSD_FPRINTF(stderr, "SoapySDR: setupStream returned null.\n");
        return -EIO;
    }
    size_t mtu = dev->soapy_dev->getStreamMTU(dev->soapy_stream);
    if (mtu > 0U) {
        *io_mtu_elems = mtu;
    }
    dev->soapy_mtu_elems = (uint32_t)(*io_mtu_elems);
    int rc = dev->soapy_dev->activateStream(dev->soapy_stream, 0, 0, 0);
    if (rc < 0) {
        DSD_FPRINTF(stderr, "SoapySDR: activateStream failed: %s (%d).\n", SoapySDR::errToStr(rc), rc);
        return rc;
    }
    return 0;
}

/* The reader's stream setup, as a control transaction holds soapy_lock: 0 once the stream is active, else the
 * failure's native code (soapy_open_stream_locked(); -EIO for an exception). */
static int
soapy_setup_stream(struct rtl_device* dev, size_t* io_mtu_elems) {
    if (!dev || !io_mtu_elems) {
        return -EIO;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex for stream setup.\n");
        return -EIO;
    }
    soapy_control_hold_mark(dev, 1);
    int rc = -EIO;
    try {
        rc = soapy_open_stream_locked(dev, io_mtu_elems);
    } catch (const std::exception& e) {
        DSD_FPRINTF(stderr, "SoapySDR: exception during stream setup: %s\n", e.what());
        rc = -EIO;
    }
    soapy_control_hold_mark(dev, 0);
    (void)dsd_mutex_unlock(&dev->soapy_lock);
    return rc;
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
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_soapy_internal_read_failure.load(std::memory_order_relaxed)) {
        return -1;
    }
#endif
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
soapy_read_stream_locked(struct rtl_device* dev, void* buffs[1], size_t mtu_elems, int* receiver_failure) {
    *receiver_failure = 0;
    if (!dev || !buffs) {
        return INT_MIN;
    }
    if (dsd_mutex_lock(&dev->soapy_lock) != 0) {
        DSD_FPRINTF(stderr, "SoapySDR: failed to lock mutex for readStream.\n");
        return INT_MIN;
    }
    /* No control hold here: a read that hangs, or keeps timing out, holding the lock is the silence the progress
     * monitor watches for. */
    int ret = INT_MIN;
    try {
        int flags = 0;
        long long time_ns = 0;
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
        const rtl_device_test_soapy_driver* fake = g_test_soapy_driver.load(std::memory_order_acquire);
        ret = fake ? soapy_test_driver_result(fake->read(fake->ctx, buffs[0], mtu_elems, 15000))
                   : dev->soapy_dev->readStream(dev->soapy_stream, buffs, mtu_elems, flags, time_ns, 15000);
#else
        ret = dev->soapy_dev->readStream(dev->soapy_stream, buffs, mtu_elems, flags, time_ns, 15000);
#endif
    } catch (const std::exception& e) {
        *receiver_failure = 1;
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
    /* Only the reader writes the counts. */
    if (ret == SOAPY_SDR_TIMEOUT) {
        dev->soapy_timeout_count.store(dev->soapy_timeout_count.load(std::memory_order_relaxed) + 1U,
                                       std::memory_order_relaxed);
        return 1;
    }
    if (ret == SOAPY_SDR_OVERFLOW) {
        const uint64_t overflows = dev->soapy_overflow_count.load(std::memory_order_relaxed) + 1U;
        dev->soapy_overflow_count.store(overflows, std::memory_order_relaxed);
        uint64_t now_ns = dsd_realtime_mono_ns();
        if ((now_ns - dev->soapy_last_overflow_log_ns) > 1000000000ULL) {
            DSD_FPRINTF(stderr, "SoapySDR: RX overflow count=%llu.\n", (unsigned long long)overflows);
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
        (void)soapy_write_cf32_to_ring(dev, reinterpret_cast<const float*>(src), kept_elems, apply_rot, 1);
        return;
    }
    const int16_t* src = cs16_buf.data() + (drop_elems * 2U);
    rtl_submit_capture_bytes(dev, src, kept_elems * 2U * sizeof(int16_t));
    rtl_publish_cs16_input_level(src, kept_elems * 2U);
    (void)soapy_write_cs16_to_ring(dev, src, kept_elems, apply_rot);
}

/* The SoapySDR reader's loop, while the device runs and the session does not end: 0 once either stopped it, else the
 * native code of the failure that ended it (a fatal read result's SoapySDR code; -EIO for a read that threw). A read
 * with samples counts as progress (one relaxed increment) before a retune mute or a reconfigure hold can drop them:
 * the receiver delivered. Timeouts, overflows and empty reads do not. */
static int
soapy_read_loop(struct rtl_device* s, size_t mtu_elems, std::vector<std::complex<float>>* cf32_buf,
                std::vector<int16_t>* cs16_buf, int* receiver_failure) {
    *receiver_failure = 0;
    while (s->run.load() && dsd_exitflag_load() == 0) {
        void* buffs[1] = {NULL};
        if (soapy_prepare_read_buffer(s, cf32_buf, cs16_buf, buffs) != 0) {
            return -EIO;
        }

        int ret = soapy_read_stream_locked(s, buffs, mtu_elems, receiver_failure);
        if (ret == INT_MIN) {
            return -EIO;
        }
        int read_status = soapy_handle_read_result(s, ret);
        if (read_status > 0) {
            continue;
        }
        if (read_status < 0) {
            *receiver_failure = 1;
            return ret;
        }
        s->progress.fetch_add(1U, std::memory_order_relaxed);

        const int apply_rot = fs4_shift_capture_active(s);
        size_t frame_elems = (size_t)ret;
        size_t drop_elems = 0U;
        int mute_status = soapy_consume_mute_prefix(s, frame_elems, apply_rot, &drop_elems);
        if (mute_status < 0) {
            return -EIO;
        }
        if (mute_status > 0) {
            continue;
        }

        size_t kept_elems = frame_elems - drop_elems;
        if (soapy_handle_reconfigure_hold(s, kept_elems, apply_rot)) {
            continue;
        }
        soapy_submit_samples(s, drop_elems, kept_elems, *cf32_buf, *cs16_buf, apply_rot);
    }
    return 0;
}

/* What the SoapySDR reader's end, with @p native_code (0: a stop or the session's end asked for it, which is left
 * alone), means for the session. A start that failed in the reader (@p streamed clear: the stream could not be set
 * up or activated, or its read buffers could not be allocated) is the driver's or the host's, not a receiver that
 * stopped: it is reported at once as a device failure, without the unplug-owner grace and without committing the
 * receiver's ending, as librtlsdr's -ENOMEM is. A driver read that failed while the stream ran ends the session for its
 * receiver; internal buffer, mute or locking failures take the immediate device-failure path (rtl_device_end_for_receiver()). Either raises the exit flag and wakes the demod waiting on the input ring;
 * of it and the progress monitor's verdict, only the first reports. */
static void
soapy_reader_finished(struct rtl_device* s, int streamed, int receiver_failure, int native_code) {
    if (native_code == 0 || !s->run.load(std::memory_order_acquire) || dsd_exitflag_load()) {
        return;
    }
    if (receiver_failure) {
        char cause[96];
        DSD_SNPRINTF(cause, sizeof cause, "SoapySDR: reading the receiver's stream failed; the stream stops (rc=%d)",
                     native_code);
        (void)rtl_device_end_for_receiver(s, native_code, rtl_device_sample_progress(s), cause);
        return;
    }
    if (s->receiver_end_claimed.exchange(1, std::memory_order_acq_rel) != 0) {
        return;
    }
    const char* operation = streamed ? "continue the RX read loop"
                                     : (native_code == -ENOMEM ? "allocate the stream's read buffers"
                                                               : "set up and activate the RX stream");
    LOG_ERROR("SoapySDR: could not %s; the stream stops (rc=%d)\n", operation, native_code);
    dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, native_code);
    dsd_exitflag_store(1);
    replay_signal_input_waiters(s);
}
#endif

/* The SoapySDR reader thread: sets the stream up, then reads it into the input ring until a stop or the session's end.
 * It publishes reader_entered once the stream is active (the progress monitor watches from then on) and reader_exited
 * once it left its loop, and reports an end nobody asked for (soapy_reader_finished()) before it closes the stream,
 * which a lost device can hold up. */
static DSD_THREAD_RETURN_TYPE DSD_RTL_THREAD_CALL
soapy_thread_fn(void* arg) {
#ifndef USE_SOAPYSDR
    (void)arg;
    DSD_THREAD_RETURN;
#else
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    maybe_set_thread_realtime_and_affinity("DONGLE");

    size_t mtu_elems = 16384;
    std::vector<std::complex<float>> cf32_buf;
    std::vector<int16_t> cs16_buf;
    int streamed = 0;
    int receiver_failure = 0;
    int rc = soapy_setup_stream(s, &mtu_elems);
    if (rc == 0 && soapy_allocate_thread_buffers(s, mtu_elems, &cf32_buf, &cs16_buf) != 0) {
        rc = -ENOMEM;
    }
    if (rc == 0) {
        streamed = 1;
        s->reader_entered.store(1, std::memory_order_release);
        rc = soapy_read_loop(s, mtu_elems, &cf32_buf, &cs16_buf, &receiver_failure);
    }
    s->reader_exited.store(1, std::memory_order_release);
    soapy_reader_finished(s, streamed, receiver_failure, rc);
    soapy_stream_cleanup(s, rc != 0 ? 1 : 0);
    s->run.store(0);
    DSD_THREAD_RETURN;
#endif
}

/* The SoapySDR device's sample-progress monitor, started with its reader. A driver whose reads keep timing out or
 * overflowing, or one read that hangs, delivers nothing and returns nothing fatal, so nothing else notices; the USB
 * receivers' silence rule applies. Silence accrues while the reader streams (its stream active and its loop not
 * left) and no control or setup transaction holds the driver lock, which keeps the reader from reading (a read
 * holding it accrues); at the limit the session ends for its receiver. The verdict waits out the unplug-owner grace
 * here, never on the reader, which keeps reading: samples meanwhile drop it. A reader that left its loop reports for
 * itself (soapy_reader_finished()). */
static DSD_THREAD_RETURN_TYPE DSD_RTL_THREAD_CALL
soapy_progress_monitor(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    rtl_progress_watch watch;
    rtl_progress_watch_begin(&watch, s->progress.load(std::memory_order_relaxed));
    while (rtl_monitor_running(s)) {
        rtl_monitor_wait(s);
        if (!rtl_monitor_running(s)) {
            break;
        }
        const int streaming =
            s->reader_entered.load(std::memory_order_acquire) && !s->reader_exited.load(std::memory_order_acquire);
        const int accruing = streaming && !s->soapy_control_hold.load(std::memory_order_relaxed);
        if (!rtl_progress_watch_step(&watch, s->progress.load(std::memory_order_relaxed), accruing)) {
            continue;
        }
        char cause[192];
        DSD_SNPRINTF(cause, sizeof cause,
                     "SoapySDR: the receiver sent no samples for %llu ms (%llu read timeouts, %llu"
                     " overflows); the stream stops",
                     (unsigned long long)watch.silence_ms,
                     (unsigned long long)s->soapy_timeout_count.load(std::memory_order_relaxed),
                     (unsigned long long)s->soapy_overflow_count.load(std::memory_order_relaxed));
        if (rtl_device_end_for_receiver(s, -ETIMEDOUT, watch.progress, cause)) {
            break;
        }
    }
    DSD_THREAD_RETURN;
}

/* libairspy's single consumer owns sample ingestion; the monitor only observes
 * streaming health. Never stop/join the driver from its receive callback. */
static void
airspy_receive(void* context, const float* samples, size_t pairs, uint64_t dropped) {
    auto* dev = static_cast<rtl_device*>(context);
    if (!dev->run.load(std::memory_order_acquire)) {
        return;
    }
    if (dropped) {
        rtl_accumulate_ring_drops(dev->input_ring, (size_t)dropped * 2U);
        rtl_record_capture_mute(dev, dropped * 2U * sizeof(float), "driver_overflow");
        rtl_finish_capture_mute_span(dev);
    }
    int mute = dev->mute.load(std::memory_order_relaxed);
    size_t skip = mute > 0 ? std::min(pairs, ((size_t)mute + 1U) / 2U) : 0;
    if (skip) {
        int consumed = (int)std::min((size_t)mute, skip * 2U);
        dev->mute.fetch_sub(consumed, std::memory_order_relaxed);
        rtl_record_capture_mute(dev, skip * 2U * sizeof(float), "retune_mute");
        if (consumed == mute) {
            rtl_finish_capture_mute_span(dev);
        }
    }
    samples += skip * 2U;
    pairs -= skip;
    if (rtl_capture_reconfigure_hold_active(dev)) {
        rtl_record_capture_mute(dev, pairs * 2U * sizeof(float), "retune_reconfigure");
        return;
    }
    if (!pairs) {
        return;
    }
    rtl_submit_capture_bytes(dev, samples, pairs * 2U * sizeof(float));
    rtl_publish_cf32_input_level_source(samples, pairs * 2U, DSD_INPUT_LEVEL_SOURCE_AIRSPY_CF32);
    (void)soapy_write_cf32_to_ring(dev, samples, pairs, 0, 0);
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" int
rtl_device_test_airspy_ingest(const rtl_device_test_airspy_request* request, float* output, size_t* output_count,
                              uint64_t* dropped) {
    if (!request || !output || !output_count || !dropped || request->start >= request->capacity) {
        return -1;
    }
    input_ring_state ring{};
    if (input_ring_init(&ring, request->capacity) != 0) {
        return -1;
    }
    ring.head.store(request->start);
    ring.tail.store(request->start);
    rtl_device dev{};
    dev.backend = RTL_BACKEND_AIRSPY;
    dev.input_ring = &ring;
    dev.run.store(1);
    dev.mute.store(request->mute);
    dev.capture_reconfigure_hold.store(request->hold);
    airspy_receive(&dev, request->samples, request->pairs, request->hardware_drops);
    size_t used = input_ring_used(&ring);
    size_t count = std::min(used, *output_count);
    for (size_t i = 0; i < count; ++i) {
        output[i] = ring.buffer[(request->start + i) % ring.capacity];
    }
    *output_count = count;
    *dropped = ring.producer_drops.load();
    input_ring_destroy(&ring);
    return 0;
}

#ifdef USE_SOAPYSDR
/* One SoapySDR CF32 read through soapy_submit_samples() into a fresh input ring. */
extern "C" int
rtl_device_test_soapy_cf32_ingest(const rtl_device_test_soapy_cf32_request* request, float* output,
                                  size_t* output_count, int* out_phase) {
    if (!request || !request->samples || !output || !output_count || !out_phase
        || request->start >= request->capacity) {
        return -1;
    }
    input_ring_state ring{};
    if (input_ring_init(&ring, request->capacity) != 0) {
        return -1;
    }
    ring.head.store(request->start);
    ring.tail.store(request->start);
    rtl_device dev{};
    dev.backend = RTL_BACKEND_SOAPY;
    dev.soapy_format = SOAPY_FMT_CF32;
    dev.input_ring = &ring;
    dev.rot_phase = request->start_phase & 3;
    std::vector<std::complex<float>> cf32_buf(request->pairs);
    const std::vector<int16_t> cs16_buf;
    DSD_MEMCPY(cf32_buf.data(), request->samples, request->pairs * 2U * sizeof(float));
    soapy_submit_samples(&dev, 0U, request->pairs, cf32_buf, cs16_buf, request->fs4_shift ? 1 : 0);
    size_t count = std::min(input_ring_used(&ring), *output_count);
    for (size_t i = 0; i < count; ++i) {
        output[i] = ring.buffer[(request->start + i) % ring.capacity];
    }
    *output_count = count;
    *out_phase = dev.rot_phase;
    input_ring_destroy(&ring);
    return 0;
}

/* soapy_copy_cs16_samples() itself: @p elem_count CS16 pairs from @p src_iq into @p dst. */
extern "C" int
rtl_device_test_soapy_cs16_copy(float* dst, const int16_t* src_iq, size_t elem_count, int apply_rot, int* phase) {
    soapy_copy_cs16_samples(dst, src_iq, elem_count, 1.0f / 32768.0f, apply_rot, phase);
    return 0;
}
#else
/* The build has no SoapySDR: nothing is copied, and the hook returns 1. */
extern "C" int
rtl_device_test_soapy_cs16_copy(float* dst, const int16_t* src_iq, size_t elem_count, int apply_rot, int* phase) {
    (void)dst;
    (void)src_iq;
    (void)elem_count;
    (void)apply_rot;
    (void)phase;
    return 1;
}

/* The build has no SoapySDR: there is no read to replay, so the outputs are not touched and the hook returns 1. */
extern "C" int
rtl_device_test_soapy_cf32_ingest(const rtl_device_test_soapy_cf32_request* request, float* output,
                                  size_t* output_count, int* out_phase) {
    (void)output;
    (void)output_count;
    (void)out_phase;
    if (!request || !request->samples || request->start >= request->capacity) {
        return -1;
    }
    return 1;
}
#endif
#endif

static DSD_THREAD_RETURN_TYPE DSD_RTL_THREAD_CALL
airspy_monitor(void* context) {
    auto* dev = static_cast<rtl_device*>(context);
    rtl_progress_watch watch;
    rtl_progress_watch_begin(&watch, airspy_source_progress(dev->airspy));
    uint64_t silence_generation = airspy_source_generation(dev->airspy);
    while (rtl_monitor_running(dev)) {
        rtl_monitor_wait(dev);
        if (!rtl_monitor_running(dev)) {
            break;
        }
        const uint64_t progress = airspy_source_progress(dev->airspy);
        const int reconfiguring = airspy_source_reconfiguring(dev->airspy);
        const int silent = rtl_progress_watch_step(&watch, progress, !reconfiguring);
        if (watch.silence_ms == 0U) {
            silence_generation = airspy_source_generation(dev->airspy);
        }
        if (reconfiguring) {
            continue;
        }
        /* A retune or control change holding the source delays this; that time is the change's, not silence. */
        const int running = airspy_source_running(dev->airspy);
        watch.sampled_ms = dsd_realtime_mono_ms();
        if (!running) {
            /* The SDK lost the stream, or a failed change left it stopped. */
            if (rtl_device_end_for_receiver(dev, -1, progress, "Airspy stream stopped unexpectedly.")) {
                break;
            }
            continue;
        }
        if (!silent) {
            continue;
        }
        char cause[128];
        DSD_SNPRINTF(cause, sizeof cause,
                     "Airspy: the receiver sent no samples for %llu ms (%llu"
                     " stream restarts); the stream stops",
                     (unsigned long long)watch.silence_ms,
                     (unsigned long long)(airspy_source_generation(dev->airspy) - silence_generation));
        if (rtl_device_end_for_receiver(dev, -ETIMEDOUT, watch.progress, cause)) {
            break;
        }
    }
    (void)airspy_source_stop(dev->airspy);
    dev->run.store(0, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* ---- rtl_tcp backend helpers ---- */

namespace {
/* rtl_tcp_read_header()'s results. */
enum rtl_tcp_header_result : int {
    RTL_TCP_HEADER_SUCCESS = 0,
    RTL_TCP_HEADER_EOF = 1,       /* the connection ended or failed before the whole header came */
    RTL_TCP_HEADER_INVALID = 2,   /* twelve bytes came, without the "RTL0" magic */
    RTL_TCP_HEADER_CANCELLED = 3, /* a stop latched, or the process is exiting */
    RTL_TCP_HEADER_TIMEOUT = 4,   /* no whole header within the budget */
};

/* The hand-off of a reconnected connection (rtl_device::tcp_handoff). */
enum RtlTcpHandoffState : int {
    RTL_TCP_HANDOFF_NONE = 0,
    RTL_TCP_HANDOFF_QUEUED = 1,    /* posted by the reader; the device holds the connection */
    RTL_TCP_HANDOFF_CLAIMED = 2,   /* the controller owns it, and publishes it or gives it up */
    RTL_TCP_HANDOFF_RELEASED = 3,  /* published; the reader is to read it and acknowledge */
    RTL_TCP_HANDOFF_FAILED = 4,    /* the replay failed: the controller closed it, the reader reconnects again */
    RTL_TCP_HANDOFF_CANCELLED = 5, /* a stop came first: the reader stops */
};

/* One rtl_tcp protocol command: 1-byte id, 4-byte big-endian parameter. */
struct rtl_tcp_cmd {
    uint8_t id;
    uint32_t param;
};

/* The most commands one unit sends: a manual gain change with test mode on, or a reconnect's replay. */
constexpr size_t kRtlTcpMaxUnit = 32U;
} // namespace

/* The connect, the header and a whole command unit each get at most this long; between two checks of the
 * cancellation token at most one slice passes. */
static constexpr unsigned int kRtlTcpConnectMs = 10000U;
static constexpr unsigned int kRtlTcpHeaderMs = 10000U;
static constexpr unsigned int kRtlTcpSendMs = 2000U;
static constexpr unsigned int kRtlTcpSliceMs = 50U;

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
static std::atomic<int> g_test_tcp_fail_cmd{-1};
static std::atomic<size_t> g_test_tcp_send_chunk{0U};
static std::atomic<int> g_test_tcp_sndbuf{0};
static RtlTestHook<uint64_t> g_test_tcp_send_hook;
static RtlTestHook<uint64_t> g_test_tcp_commit_hook;
static std::atomic<int> g_test_tcp_refuse_read_buffer{0};
static RtlTestHook<> g_test_tcp_discard_hook;
static std::atomic<uint64_t> g_test_tcp_odd_drops{0U};
static RtlTestHook<> g_test_tcp_read_hook;
#endif

static void
rtl_tcp_lock(const struct rtl_device* dev) {
    if (dev->tcp_sync_inited) {
        (void)dsd_mutex_lock(&dev->tcp_lock);
    }
}

static void
rtl_tcp_unlock(const struct rtl_device* dev) {
    if (dev->tcp_sync_inited) {
        (void)dsd_mutex_unlock(&dev->tcp_lock);
    }
}

static void
rtl_tcp_broadcast(struct rtl_device* dev) {
    if (dev->tcp_sync_inited) {
        (void)dsd_cond_broadcast(&dev->tcp_cond);
    }
}

/* Wait (tcp_lock held) up to one slice for a hand-off change. */
static void
rtl_tcp_wait_slice(struct rtl_device* dev) {
    if (dev->tcp_sync_inited) {
        (void)dsd_cond_timedwait(&dev->tcp_cond, &dev->tcp_lock, kRtlTcpSliceMs);
    }
}

/* The rtl_tcp cancellation token: a stop or destroy latched (stop_requested), or the process is exiting. Unlike run,
 * it holds before the reader starts, so the session's first connect (made before any reader, with run still 0) can be
 * cancelled without run == 0 cancelling it. */
static int
rtl_tcp_cancelled(void* ctx) {
    const auto* dev = static_cast<struct rtl_device*>(ctx);
    return (dsd_exitflag_load() || (dev && dev->stop_requested.load(std::memory_order_acquire))) ? 1 : 0;
}

/* Whether a socket call's negative result @p r only means its timeout (or a signal) cut it short. */
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

/* Connect to the rtl_tcp server, within kRtlTcpConnectMs, giving up once @p dev's cancellation token holds. */
static dsd_socket_t
tcp_connect_host(const char* host, int port, struct rtl_device* dev) {
    dsd_socket_t sockfd = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sockfd == DSD_INVALID_SOCKET) {
        dsd_input_failure_report(DSD_INPUT_FAILURE_NETWORK, dsd_socket_get_error());
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
        dsd_input_failure_report(DSD_INPUT_FAILURE_RESOLVE, dsd_socket_get_error());
        DSD_FPRINTF(stderr, "rtl_tcp: ERROR, no such host as %s\n", host);
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }
    int code = 0;
    if (dsd_socket_connect_bounded(sockfd, reinterpret_cast<const struct sockaddr*>(&serveraddr), sizeof(serveraddr),
                                   kRtlTcpConnectMs, rtl_tcp_cancelled, dev, &code)
        != 0) {
        if (!rtl_tcp_cancelled(dev)) {
            dsd_input_failure_report(dsd_input_failure_classify_socket(code), code);
            DSD_FPRINTF(stderr, "rtl_tcp: ERROR connecting to %s:%d\n", host, port);
        }
        dsd_socket_close(sockfd);
        return DSD_INVALID_SOCKET;
    }
    dsd_input_failure_clear();
    return sockfd;
}

/* The options every connection gets before its header is read: a larger receive buffer (brief processing stalls),
 * no Nagle delay for the commands, the receive timeout that turns a silent server into a stall (DSD_NEO_TCP_RCVTIMEO,
 * default 2 s) and a send timeout as long as a command send may take: a send waits for room first
 * (rtl_tcp_send_all()), so the timeout only bounds one that blocks all the same. */
static void
rtl_tcp_prepare_socket(dsd_socket_t sock) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int rcvbuf = cfg ? cfg->tcp_rcvbuf_bytes : (4 * 1024 * 1024);
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int nodelay = 1;
    (void)dsd_socket_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int to_ms = cfg ? cfg->tcp_rcvtimeo_ms : 2000;
    (void)dsd_socket_set_recv_timeout(sock, (unsigned int)to_ms);
    (void)dsd_socket_set_send_timeout(sock, kRtlTcpSendMs);
#if defined(SO_NOSIGPIPE)
    /* Where MSG_NOSIGNAL does not exist, a command to a server that went away must fail, not raise SIGPIPE. */
    int nosigpipe = 1;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    const int sndbuf = g_test_tcp_sndbuf.load(std::memory_order_relaxed);
    if (sndbuf > 0) {
        (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
#endif
}

/* One wait slice of the header read: the count of bytes received into @p buf (at most @p len), 0 when none came in the
 * slice, or -1 once the connection closed or failed. */
static int
rtl_tcp_header_recv_slice(dsd_socket_t sock, uint8_t* buf, size_t len) {
    const int ready = dsd_socket_wait(sock, DSD_SOCKET_WAIT_READ, kRtlTcpSliceMs);
    if (ready < 0) {
        return -1;
    }
    if (ready == 0) {
        return 0;
    }
    const int n = dsd_socket_recv(sock, buf, len, 0);
    if (n < 0 && rtl_tcp_recv_is_timeout(n)) {
        return 0;
    }
    return (n > 0) ? n : -1;
}

static uint32_t
rtl_tcp_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Read the server's header: exactly twelve bytes, "RTL0", the tuner type and the gain count (both big-endian), and
 * nothing more: standard rtl_tcp sends no gain table, so the first byte after it is the first I sample (B13). The
 * header may come in fragments across receive timeouts; each wait is one slice, between which @p cancelled is asked,
 * and the whole read gets @p budget_ms. */
static rtl_tcp_header_result
rtl_tcp_read_header(dsd_socket_t sock, unsigned int budget_ms, int (*cancelled)(void*), void* ctx, uint32_t* out_tuner,
                    uint32_t* out_gain_count) {
    uint8_t hdr[12];
    size_t got = 0U;
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + budget_ms;
    while (got < sizeof hdr) {
        if (cancelled && cancelled(ctx)) {
            return RTL_TCP_HEADER_CANCELLED;
        }
        if (dsd_realtime_mono_ms() >= deadline_ms) {
            return RTL_TCP_HEADER_TIMEOUT;
        }
        const int n = rtl_tcp_header_recv_slice(sock, hdr + got, sizeof hdr - got);
        if (n < 0) {
            return RTL_TCP_HEADER_EOF;
        }
        got += (size_t)n;
    }
    if (hdr[0] != 'R' || hdr[1] != 'T' || hdr[2] != 'L' || hdr[3] != '0') {
        return RTL_TCP_HEADER_INVALID;
    }
    if (out_tuner) {
        *out_tuner = rtl_tcp_be32(hdr + 4);
    }
    if (out_gain_count) {
        *out_gain_count = rtl_tcp_be32(hdr + 8);
    }
    return RTL_TCP_HEADER_SUCCESS;
}

static const char*
rtl_tcp_header_failure_name(rtl_tcp_header_result r) {
    switch (r) {
        case RTL_TCP_HEADER_EOF: return "the connection closed first";
        case RTL_TCP_HEADER_INVALID: return "it is not an rtl_tcp header";
        case RTL_TCP_HEADER_TIMEOUT: return "none came within 10 s";
        default: return "cancelled";
    }
}

static struct rtl_tcp_conn*
rtl_tcp_conn_new(dsd_socket_t sock) {
    auto* conn = new (std::nothrow) rtl_tcp_conn();
    if (!conn) {
        return nullptr;
    }
    if (dsd_mutex_init(&conn->tx_lock) != 0) {
        delete conn;
        return nullptr;
    }
    conn->sock = sock;
    conn->refs.store(1, std::memory_order_relaxed);
    conn->lost.store(0, std::memory_order_relaxed);
    return conn;
}

static void
rtl_tcp_conn_ref(struct rtl_tcp_conn* conn) {
    conn->refs.fetch_add(1, std::memory_order_relaxed);
}

/* Drop a reference; the last one closes the socket. */
static void
rtl_tcp_conn_unref(struct rtl_tcp_conn* conn) {
    if (!conn || conn->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    dsd_socket_shutdown(conn->sock, SHUT_RDWR);
    dsd_socket_close(conn->sock);
    (void)dsd_mutex_destroy(&conn->tx_lock);
    delete conn;
}

/* Wake whoever waits on the connection's socket (the reader, a blocked sender): their next call fails. The socket
 * stays open until the last reference goes. */
static void
rtl_tcp_conn_shutdown(struct rtl_tcp_conn* conn) {
    if (conn) {
        conn->lost.store(1, std::memory_order_release);
        dsd_socket_shutdown(conn->sock, SHUT_RDWR);
    }
}

/* Take the published connection down (tcp_lock held): no stream from here until another is published, and its socket
 * shut down so the reader and any sender on it wake. Returns the device's reference for the caller to drop. */
static struct rtl_tcp_conn*
rtl_tcp_unpublish_locked(struct rtl_device* dev) {
    struct rtl_tcp_conn* conn = dev->tcp_conn;
    if (!conn) {
        return nullptr;
    }
    dev->tcp_conn = nullptr;
    dev->tcp_connected.store(0, std::memory_order_release);
    rtl_tcp_conn_shutdown(conn);
    return conn;
}

/* A command send never waits inside send(): it waits for room (rtl_tcp_send_all()) and then sends what fits. Where a
 * send can be asked not to block (MSG_DONTWAIT), a send that finds the room gone again just waits again. Elsewhere
 * (Winsock) the socket stays blocking for its reader; its send timeout (kRtlTcpSendMs) only bounds a send that blocks
 * after all, and a timed-out send leaves the socket in an unknown state (Winsock), so it ends the connection. */
#if DSD_PLATFORM_WIN_NATIVE
static constexpr int kRtlTcpSendFlags = MSG_NOSIGNAL;
#elif defined(MSG_DONTWAIT)
static constexpr int kRtlTcpSendFlags = MSG_NOSIGNAL | MSG_DONTWAIT;
static constexpr int kRtlTcpSendMayRetry = 1;
#else
static constexpr int kRtlTcpSendFlags = MSG_NOSIGNAL;
static constexpr int kRtlTcpSendMayRetry = 0;
#endif

#if !DSD_PLATFORM_WIN_NATIVE
/* Whether a send that sent nothing only found no room (MSG_DONTWAIT) or a signal, so it may wait and go again. Never
 * on Winsock, where a failed send ends the connection. */
static int
rtl_tcp_send_would_block(void) {
    const int e = dsd_socket_get_error();
    return (kRtlTcpSendMayRetry && (e == EAGAIN || ((EWOULDBLOCK != EAGAIN) && (e == EWOULDBLOCK)))) || e == EINTR;
}
#endif

/* What a send that sent nothing (@p n, 0 or an error) means: 0 when it may wait and go again, else -ETIMEDOUT or
 * -EIO. */
static int
rtl_tcp_send_failure(int n) {
#if !DSD_PLATFORM_WIN_NATIVE
    if (n < 0 && rtl_tcp_send_would_block()) {
        return 0;
    }
#endif
    return (n < 0 && rtl_tcp_recv_is_timeout(n)) ? -ETIMEDOUT : -EIO;
}

/* Send @p len bytes on @p conn whole (its tx_lock held): wait (a slice at a time, asking @p cancelled between slices)
 * until the socket has room, send what fits, and go on from where a partial write stopped; the whole send gets
 * kRtlTcpSendMs. Returns 0, -ECANCELED, -ETIMEDOUT or -EIO; any failure loses the connection, since part of a command
 * may have gone out. */
static int
rtl_tcp_send_all(const struct rtl_tcp_conn* conn, const uint8_t* buf, size_t len, int (*cancelled)(void*), void* ctx) {
    size_t sent = 0U;
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + kRtlTcpSendMs;
    while (sent < len) {
        if (cancelled && cancelled(ctx)) {
            return -ECANCELED;
        }
        if (dsd_realtime_mono_ms() >= deadline_ms) {
            return -ETIMEDOUT;
        }
        const int ready = dsd_socket_wait(conn->sock, DSD_SOCKET_WAIT_WRITE, kRtlTcpSliceMs);
        if (ready < 0) {
            return -EIO;
        }
        if (ready == 0) {
            continue;
        }
        size_t want = len - sent;
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
        const size_t chunk = g_test_tcp_send_chunk.load(std::memory_order_relaxed);
        if (chunk > 0U && want > chunk) {
            want = chunk;
        }
#endif
        const int n = dsd_socket_send(conn->sock, buf + sent, want, kRtlTcpSendFlags);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        const int failed = rtl_tcp_send_failure(n);
        if (failed != 0) {
            return failed;
        }
    }
    return 0;
}

/* Send @p n commands as one unit on @p conn (its tx_lock held). */
static int
rtl_tcp_send_unit(struct rtl_device* dev, const struct rtl_tcp_conn* conn, const struct rtl_tcp_cmd* cmds, size_t n) {
    uint8_t wire[kRtlTcpMaxUnit * 5U];
    size_t count = (n < kRtlTcpMaxUnit) ? n : kRtlTcpMaxUnit;
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_tcp_send_hook.invoke(conn->generation);
    /* A failure a test injects at a command: the commands before it go out, then the unit fails. */
    size_t fail_at = n;
    const int fail_cmd = g_test_tcp_fail_cmd.load(std::memory_order_relaxed);
    for (size_t i = 0; fail_cmd >= 0 && i < n && fail_at == n; i++) {
        if (cmds[i].id == (uint8_t)fail_cmd) {
            fail_at = i;
            g_test_tcp_fail_cmd.store(-1, std::memory_order_relaxed);
        }
    }
    if (fail_at < count) {
        count = fail_at;
    }
#endif
    size_t len = 0U;
    for (size_t i = 0; i < count; i++) {
        wire[len++] = cmds[i].id;
        wire[len++] = (uint8_t)((cmds[i].param >> 24) & 0xFFU);
        wire[len++] = (uint8_t)((cmds[i].param >> 16) & 0xFFU);
        wire[len++] = (uint8_t)((cmds[i].param >> 8) & 0xFFU);
        wire[len++] = (uint8_t)(cmds[i].param & 0xFFU);
    }
    int rc = (len > 0U) ? rtl_tcp_send_all(conn, wire, len, rtl_tcp_cancelled, dev) : 0;
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rc == 0 && fail_at < n) {
        rc = -EIO;
    }
#endif
    return rc;
}

/* Send one setter's commands (@p build fills them, under tcp_lock, from the record) as one unit on the published
 * connection, under its tx_lock and never under tcp_lock, then, still holding tx_lock so the record follows the order
 * the server saw, let @p commit record what took under tcp_lock, only while that connection is still the published
 * one; when the send failed, @p failed notes (under tcp_lock) what part of it may have reached the server. Returns 0;
 * RTL_DEVICE_ENOTCONN with no published connection (nothing sent or recorded), when the connection was lost or replaced
 * meanwhile (nothing recorded), or when the send failed, which loses that connection and only that one. */
template <typename Build, typename Commit, typename Failed>
static int
rtl_tcp_apply(struct rtl_device* dev, Build&& build, Commit&& commit, Failed&& failed) {
    rtl_tcp_lock(dev);
    struct rtl_tcp_conn* conn = dev->tcp_conn;
    if (conn) {
        rtl_tcp_conn_ref(conn);
    }
    rtl_tcp_unlock(dev);
    if (!conn) {
        return RTL_DEVICE_ENOTCONN;
    }
    (void)dsd_mutex_lock(&conn->tx_lock);
    struct rtl_tcp_cmd cmds[kRtlTcpMaxUnit];
    rtl_tcp_lock(dev);
    const size_t n = build(cmds);
    rtl_tcp_unlock(dev);
    int rc = rtl_tcp_send_unit(dev, conn, cmds, n);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_tcp_commit_hook.invoke(conn->generation);
#endif
    struct rtl_tcp_conn* lost = nullptr;
    rtl_tcp_lock(dev);
    const int current = (dev->tcp_conn == conn) ? 1 : 0;
    if (rc == 0 && current) {
        commit();
    } else {
        failed();
        if (current) {
            lost = rtl_tcp_unpublish_locked(dev);
        }
        rc = RTL_DEVICE_ENOTCONN;
    }
    rtl_tcp_unlock(dev);
    (void)dsd_mutex_unlock(&conn->tx_lock);
    if (lost) {
        if (!rtl_tcp_cancelled(dev)) {
            DSD_FPRINTF(stderr, "rtl_tcp: a command to %s:%d failed; reconnecting.\n", dev->host, dev->port);
        }
        rtl_tcp_conn_unref(lost);
    }
    rtl_tcp_conn_unref(conn);
    return rc;
}

template <typename Build, typename Commit>
static int
rtl_tcp_apply(struct rtl_device* dev, Build&& build, Commit&& commit) {
    return rtl_tcp_apply(dev, build, commit, []() {});
}

/* One command with nothing to read from the record. */
template <typename Commit>
static int
rtl_tcp_command(struct rtl_device* dev, uint8_t id, uint32_t param, Commit&& commit) {
    return rtl_tcp_apply(
        dev,
        [&](struct rtl_tcp_cmd* cmds) {
            cmds[0] = {id, param};
            return (size_t)1U;
        },
        commit);
}

static int
env_agc_want(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        return 1;
    }
    return cfg->rtl_agc_enable ? 1 : 0;
}

/* The gain policy over rtl_tcp, the USB register-0x19 policy in protocol commands (B9, B16): manual gain is digital
 * AGC off (0x08, left out while test mode is on, since the server writes that register for both), manual mode (0x03)
 * and the gain (0x04); automatic gain is automatic mode and the digital AGC as wanted; test mode (0x07) wins and is
 * asserted again after either. Appends to @p cmds at @p n and returns the new count. */
static size_t
rtl_tcp_gain_commands(int auto_gain, int gain, int digital_agc, int testmode, struct rtl_tcp_cmd* cmds, size_t n) {
    if (auto_gain) {
        cmds[n++] = {0x03, 0U};
        cmds[n++] = testmode ? rtl_tcp_cmd{0x07, 1U} : rtl_tcp_cmd{0x08, (uint32_t)(digital_agc ? 1 : 0)};
        return n;
    }
    if (!testmode) {
        cmds[n++] = {0x08, 0U};
    }
    cmds[n++] = {0x03, 1U};
    cmds[n++] = {0x04, (uint32_t)gain};
    if (testmode) {
        cmds[n++] = {0x07, 1U};
    }
    return n;
}

/* Change the gain as one unit (rtl_tcp_gain_commands()), recording mode, gain and the digital AGC only once all of it
 * went out (B16). */
static int
rtl_tcp_set_gain(struct rtl_device* dev, int auto_gain, int gain) {
    const int digital_agc = auto_gain ? env_agc_want() : 0;
    return rtl_tcp_apply(
        dev,
        [&](struct rtl_tcp_cmd* cmds) {
            return rtl_tcp_gain_commands(auto_gain, gain, digital_agc, dev->testmode_on, cmds, 0U);
        },
        [&]() {
            dev->agc_mode = auto_gain ? 1 : 0;
            dev->gain = auto_gain ? RTL_AUTO_GAIN : gain;
            dev->digital_agc_wanted = digital_agc;
            dev->tcp_took.gain = 1;
        },
        [&]() { dev->tcp_took.gain_unknown = 1; });
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
    unsigned int recv_timeout_ms; /* the receive timeout (DSD_NEO_TCP_RCVTIMEO) */
    unsigned char* u8;
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

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" int
rtl_device_test_misc_string_helpers(char* tuner_labels, size_t tuner_labels_size, char* reason_small,
                                    size_t reason_small_size, char* reason_null, size_t reason_null_size, char* trimmed,
                                    size_t trimmed_size, size_t* rounded_pages, size_t rounded_pages_len) {
    if (!tuner_labels || tuner_labels_size == 0U || !reason_small || reason_small_size == 0U || !reason_null
        || reason_null_size == 0U || !trimmed || trimmed_size == 0U || !rounded_pages || rounded_pages_len < 4U) {
        return -1;
    }

    DSD_SNPRINTF(tuner_labels, tuner_labels_size, "%s|%s|%s|%s|%s|%s|%s", rtl_tuner_type_name(RTLSDR_TUNER_E4000),
                 rtl_tuner_type_name(RTLSDR_TUNER_FC0012), rtl_tuner_type_name(RTLSDR_TUNER_FC0013),
                 rtl_tuner_type_name(RTLSDR_TUNER_FC2580), rtl_tuner_type_name(RTLSDR_TUNER_R820T),
                 rtl_tuner_type_name(RTLSDR_TUNER_R828D), rtl_tuner_type_name(-1));

    rtl_copy_event_reason(reason_small, reason_small_size, "capture-reconfigure");
    rtl_copy_event_reason(reason_null, reason_null_size, NULL);

    std::string t = soapy_trim_copy(" \t gain = 12.5 \n");
    DSD_SNPRINTF(trimmed, trimmed_size, "%s", t.c_str());

    rounded_pages[0] = rtl_tcp_round_up_page(0U);
    rounded_pages[1] = rtl_tcp_round_up_page(1U);
    rounded_pages[2] = rtl_tcp_round_up_page(4096U);
    rounded_pages[3] = rtl_tcp_round_up_page(4097U);
    return 0;
}
#endif

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
    st->auto_last_ns = dsd_realtime_mono_ns();
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
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_tcp_refuse_read_buffer.load(std::memory_order_relaxed)) {
        return 0;
    }
#endif
    st->u8 = static_cast<unsigned char*>(malloc(st->bufsz));
    if (!st->u8) {
        return 0;
    }
    st->waitall = rtl_tcp_cfg_waitall(s, cfg);
    st->recv_timeout_ms = (cfg && cfg->tcp_rcvtimeo_ms > 0) ? (unsigned int)cfg->tcp_rcvtimeo_ms : 2000U;
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

/* The replay unit's tuning commands (tcp_lock held): crystals, direct sampling, frequency, rate, offset tuning, PPM,
 * appended at @p n. Returns the new count. */
static size_t
rtl_tcp_replay_tuning_commands(const struct rtl_device* dev, struct rtl_tcp_cmd* cmds, size_t n) {
    const struct rtl_tcp_took& took = dev->tcp_took;
    if (took.xtal && dev->rtl_xtal_hz > 0U) {
        cmds[n++] = {0x0B, dev->rtl_xtal_hz};
    }
    if (took.xtal && dev->tuner_xtal_hz > 0U) {
        cmds[n++] = {0x0C, dev->tuner_xtal_hz};
    }
    if (took.direct_sampling) {
        cmds[n++] = {0x09, (uint32_t)dev->direct_sampling};
    }
    if (took.freq) {
        cmds[n++] = {0x01, dev->freq};
    }
    if (took.rate) {
        cmds[n++] = {0x02, dev->rate};
    }
    if (took.offset_tuning) {
        cmds[n++] = {0x0A, (uint32_t)(dev->offset_took ? 1 : 0)};
    }
    if (took.ppm) {
        cmds[n++] = {0x05, (uint32_t)dev->ppm_error};
    }
    return n;
}

/* The replay unit's gain and front-end commands (tcp_lock held): gain (with the digital AGC and test mode), IF gains,
 * bias tee, appended at @p n. Returns the new count. */
static size_t
rtl_tcp_replay_gain_commands(const struct rtl_device* dev, struct rtl_tcp_cmd* cmds, size_t n) {
    const struct rtl_tcp_took& took = dev->tcp_took;
    if (took.gain || took.gain_unknown) {
        n = rtl_tcp_gain_commands(dev->agc_mode, dev->gain, dev->digital_agc_wanted, dev->testmode_on, cmds, n);
    } else if (took.testmode || took.testmode_unknown) {
        cmds[n++] = {0x07, (uint32_t)(dev->testmode_on ? 1 : 0)};
        if (!dev->testmode_on) {
            cmds[n++] = {0x08, (uint32_t)(dev->digital_agc_wanted ? 1 : 0)};
        }
    }
    for (int i = 0; i < dev->if_gain_count && i < 16 && n < kRtlTcpMaxUnit - 1U; i++) {
        cmds[n++] = {0x06, ((uint32_t)(dev->if_gains[i].stage & 0xFFFF) << 16)
                               | ((uint16_t)(dev->if_gains[i].gain & 0xFFFF))};
    }
    if (took.bias_tee) {
        cmds[n++] = {0x0E, (uint32_t)(dev->bias_tee_on ? 1 : 0)};
    }
    return n;
}

/* The effective record as one replay unit (tcp_lock held), in the order a USB recovery replays: crystals, direct
 * sampling, frequency, rate, offset tuning, PPM, gain (with the digital AGC and test mode), IF gains, bias tee. Every
 * control that took is replayed as last taken, a value its server's defaults may share or not: a server restarted
 * with other defaults ends with the session's settings. Returns the count. */
static size_t
rtl_tcp_replay_commands(const struct rtl_device* dev, struct rtl_tcp_cmd* cmds) {
    return rtl_tcp_replay_gain_commands(dev, cmds, rtl_tcp_replay_tuning_commands(dev, cmds, 0U));
}

int
rtl_device_tcp_handoff_take(struct rtl_device* dev) {
    if (!dev || dev->backend != RTL_BACKEND_TCP) {
        return 0;
    }
    rtl_tcp_lock(dev);
    const int took = (dev->tcp_handoff == RTL_TCP_HANDOFF_QUEUED && !dev->tcp_admission_closed) ? 1 : 0;
    if (took) {
        dev->tcp_handoff = RTL_TCP_HANDOFF_CLAIMED;
    }
    rtl_tcp_unlock(dev);
    return took;
}

/* Send the effective record to the handed-over @p conn as one unit. Nobody else can send on it yet: it is published
 * only after. The record holds still meanwhile, since every setter fails while no connection is published and a
 * command still in flight on the lost one records nothing. */
static int
rtl_tcp_send_replay(struct rtl_device* dev, struct rtl_tcp_conn* conn) {
    (void)dsd_mutex_lock(&conn->tx_lock);
    struct rtl_tcp_cmd cmds[kRtlTcpMaxUnit];
    rtl_tcp_lock(dev);
    const size_t n = rtl_tcp_replay_commands(dev, cmds);
    rtl_tcp_unlock(dev);
    const int rc = rtl_tcp_send_unit(dev, conn, cmds, n);
    (void)dsd_mutex_unlock(&conn->tx_lock);
    return rc;
}

/* Publish the handed-over @p conn after a replay that returned @p rc, or fail the hand-off (tcp_lock held). Returns
 * 0 once published. */
static int
rtl_tcp_settle_handoff_locked(struct rtl_device* dev, struct rtl_tcp_conn* conn, int rc) {
    if (rc == 0 && (dev->tcp_admission_closed || dev->stop_requested.load(std::memory_order_acquire))) {
        rc = -ECANCELED;
    }
    dev->tcp_handoff_conn = nullptr;
    if (rc == 0) {
        /* The hand-off's reference becomes the published one. */
        dev->tcp_conn = conn;
        dev->tcp_connected.store(1, std::memory_order_release);
        return 0;
    }
    dev->tcp_handoff = (rc == -ECANCELED || dev->stop_requested.load(std::memory_order_acquire))
                           ? RTL_TCP_HANDOFF_CANCELLED
                           : RTL_TCP_HANDOFF_FAILED;
    rtl_tcp_conn_shutdown(conn);
    rtl_tcp_broadcast(dev);
    return rc;
}

/* A hand-off whose replay failed with @p rc: drop its reference to @p conn and say why. */
static int
rtl_tcp_handoff_failed(const struct rtl_device* dev, struct rtl_tcp_conn* conn, int rc, char* why, size_t why_size) {
    rtl_tcp_conn_unref(conn);
    const int stopped = (rc == -ECANCELED || dev->stop_requested.load(std::memory_order_acquire)) ? 1 : 0;
    if (!stopped) {
        DSD_FPRINTF(stderr, "rtl_tcp: could not restore the settings on %s:%d (rc=%d); reconnecting again.\n",
                    dev->host, dev->port, rc);
    }
    if (why && why_size > 0U) {
        if (stopped) {
            DSD_SNPRINTF(why, why_size, "%s", "stopped");
        } else {
            DSD_SNPRINTF(why, why_size, "could not restore the server's settings (rc=%d)", rc);
        }
    }
    return stopped ? -ECANCELED : rc;
}

int
rtl_device_tcp_publish_handoff(struct rtl_device* dev, char* why, size_t why_size) {
    if (!dev || dev->backend != RTL_BACKEND_TCP) {
        return -EINVAL;
    }
    rtl_tcp_lock(dev);
    struct rtl_tcp_conn* conn = (dev->tcp_handoff == RTL_TCP_HANDOFF_CLAIMED) ? dev->tcp_handoff_conn : nullptr;
    rtl_tcp_unlock(dev);
    if (!conn) {
        if (why && why_size > 0U) {
            DSD_SNPRINTF(why, why_size, "%s", "no connection was handed over");
        }
        return -EINVAL;
    }
    /* A new server connection, set up from the record: the next retune writes what it recorded again. */
    dev->capture_settings_generation.fetch_add(1U, std::memory_order_acq_rel);
    int rc = rtl_tcp_send_replay(dev, conn);
    rtl_tcp_lock(dev);
    rc = rtl_tcp_settle_handoff_locked(dev, conn, rc);
    const uint32_t rate = dev->rate;
    rtl_tcp_unlock(dev);
    if (rc != 0) {
        return rtl_tcp_handoff_failed(dev, conn, rc, why, why_size);
    }
    rtl_tcp_metrics_reset_device(dev, rate);
    return 0;
}

/* Release the reader onto the published connection (rtl_device_resume_stream()) and wait until it acknowledged. */
static int
rtl_tcp_resume_reader(struct rtl_device* dev) {
    rtl_tcp_lock(dev);
    if (dev->tcp_handoff != RTL_TCP_HANDOFF_CLAIMED) {
        rtl_tcp_unlock(dev);
        return 0;
    }
    if (!dev->tcp_conn) {
        /* A command lost the published connection before the release: the reader reconnects again. */
        dev->tcp_handoff = RTL_TCP_HANDOFF_FAILED;
        rtl_tcp_broadcast(dev);
        rtl_tcp_unlock(dev);
        return RTL_DEVICE_ENOTCONN;
    }
    if (dev->tcp_admission_closed || rtl_tcp_cancelled(dev)) {
        dev->tcp_handoff = RTL_TCP_HANDOFF_CANCELLED;
        rtl_tcp_broadcast(dev);
        rtl_tcp_unlock(dev);
        return -ECANCELED;
    }
    dev->tcp_handoff = RTL_TCP_HANDOFF_RELEASED;
    rtl_tcp_broadcast(dev);
    int rc = 0;
    while (dev->tcp_handoff == RTL_TCP_HANDOFF_RELEASED) {
        if (rtl_tcp_cancelled(dev)) {
            rc = -ECANCELED;
            break;
        }
        rtl_tcp_wait_slice(dev);
    }
    rtl_tcp_unlock(dev);
    return rc;
}

/* Stop an rtl_tcp device's connection handling, first thing in a stop or destroy (D1a): latch the stop (the
 * cancellation token every connect, header read, send and hand-off wait checks), close hand-off admission for good,
 * cancel a hand-off the controller has not taken (its waiter, the reader, closes the connection it still holds; one
 * the controller took is the controller's to close, which it does at its next step) and shut the published socket
 * down, which wakes the reader and any blocked sender. */
static void
rtl_tcp_stop(struct rtl_device* dev) {
    dev->stop_requested.store(1, std::memory_order_release);
    rtl_tcp_lock(dev);
    dev->tcp_admission_closed = 1;
    struct rtl_tcp_conn* withdrawn = nullptr;
    if (dev->tcp_handoff == RTL_TCP_HANDOFF_QUEUED) {
        withdrawn = dev->tcp_handoff_conn;
        dev->tcp_handoff_conn = nullptr;
        dev->tcp_handoff = RTL_TCP_HANDOFF_CANCELLED;
    }
    rtl_tcp_conn_shutdown(dev->tcp_conn);
    rtl_tcp_broadcast(dev);
    rtl_tcp_unlock(dev);
    rtl_tcp_conn_unref(withdrawn);
}

/* Queue reconnected @p conn for the controller, with a reference of its own. Returns 0, or RTL_TCP_HANDOFF_CANCELLED
 * once a stop closed admission. */
static int
rtl_tcp_queue_handoff(struct rtl_device* dev, struct rtl_tcp_conn* conn) {
    rtl_tcp_lock(dev);
    if (dev->tcp_admission_closed || rtl_tcp_cancelled(dev)) {
        rtl_tcp_unlock(dev);
        return RTL_TCP_HANDOFF_CANCELLED;
    }
    conn->generation = ++dev->tcp_generation;
    rtl_tcp_conn_ref(conn);
    dev->tcp_handoff_conn = conn;
    dev->tcp_handoff = RTL_TCP_HANDOFF_QUEUED;
    rtl_tcp_unlock(dev);
    return 0;
}

/* Tell the stream controller a hand-off is queued; with none (the device on its own), publish and release it here. */
static void
rtl_tcp_post_handoff(struct rtl_device* dev) {
    rtl_usb_ctrl_lock(dev);
    const rtl_device_recovery_cb cb = dev->recovery_cb;
    void* cb_ctx = dev->recovery_cb_ctx;
    rtl_usb_ctrl_unlock(dev);
    if (cb) {
        cb(cb_ctx);
    } else if (rtl_device_tcp_handoff_take(dev) && rtl_device_tcp_publish_handoff(dev, NULL, 0U) == 0) {
        rtl_tcp_lock(dev);
        dev->tcp_handoff = RTL_TCP_HANDOFF_RELEASED;
        rtl_tcp_unlock(dev);
    }
}

/* Where the reader's queued hand-off stands (tcp_lock held), its connection @p dead or not: RTL_TCP_HANDOFF_NONE
 * while it still waits, else RTL_TCP_HANDOFF_RELEASED, RTL_TCP_HANDOFF_FAILED or RTL_TCP_HANDOFF_CANCELLED. A hand-off
 * the controller has not taken is withdrawn into @p withdrawn once its connection died or the session is ending. */
static int
rtl_tcp_handoff_verdict_locked(struct rtl_device* dev, int dead, struct rtl_tcp_conn** withdrawn) {
    const int state = dev->tcp_handoff;
    if (state == RTL_TCP_HANDOFF_RELEASED) {
        return state;
    }
    if (state == RTL_TCP_HANDOFF_FAILED || state == RTL_TCP_HANDOFF_CANCELLED) {
        dev->tcp_handoff = RTL_TCP_HANDOFF_NONE;
        return state;
    }
    if (state == RTL_TCP_HANDOFF_QUEUED && (dead || rtl_tcp_cancelled(dev) || !dev->run.load())) {
        *withdrawn = dev->tcp_handoff_conn;
        dev->tcp_handoff_conn = nullptr;
        dev->tcp_handoff = RTL_TCP_HANDOFF_NONE;
        return dead ? RTL_TCP_HANDOFF_FAILED : RTL_TCP_HANDOFF_CANCELLED;
    }
    return RTL_TCP_HANDOFF_NONE;
}

/* One slice of draining the handed-over @p conn into the discard path, keeping the parity of the bytes received in
 * @p parity. Returns 1 once the connection died. */
static int
rtl_tcp_discard_slice(const struct rtl_tcp_conn* conn, struct rtl_tcp_loop_state* st, unsigned int* parity) {
    const int ready = dsd_socket_wait(conn->sock, DSD_SOCKET_WAIT_READ, kRtlTcpSliceMs);
    if (ready < 0) {
        return 1;
    }
    if (ready == 0) {
        return 0;
    }
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_tcp_discard_hook.invoke();
#endif
    const int got = dsd_socket_recv(conn->sock, st->u8, st->bufsz, 0);
    if (got > 0) {
        *parity ^= (unsigned int)got & 1U;
        return 0;
    }
    return (got == 0 || !rtl_tcp_recv_is_timeout(got)) ? 1 : 0;
}

/* The reader's acknowledgement of a released hand-off, after any receive still in flight. */
static void
rtl_tcp_ack_handoff_release(struct rtl_device* dev, unsigned int parity, int* skip_odd) {
    *skip_odd = (int)parity;
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (parity) {
        g_test_tcp_odd_drops.fetch_add(1U, std::memory_order_relaxed);
    }
#endif
    rtl_reset_capture_state_on_stream_boundary(dev);
    rtl_tcp_lock(dev);
    const uint32_t rate = dev->rate;
    dev->tcp_handoff = RTL_TCP_HANDOFF_NONE;
    rtl_tcp_broadcast(dev);
    rtl_tcp_unlock(dev);
    rtl_tcp_metrics_reset_device(dev, rate);
}

/* Hand reconnected @p conn to the controller and drain it into a discard path while it restores the settings, so the
 * server's I/Q from before then builds no backlog. Every received byte counts toward the I/Q pair; on release the
 * reader acknowledges itself, after any receive still in flight: @p skip_odd tells it to drop the odd byte (if any)
 * so ingestion resumes on an I sample, and the carry and partial slice from the lost connection are cleared. Returns
 * RTL_TCP_HANDOFF_RELEASED (read @p conn from here), RTL_TCP_HANDOFF_FAILED (reconnect again) or
 * RTL_TCP_HANDOFF_CANCELLED (stop). The caller keeps its own reference to @p conn either way. */
static int
rtl_tcp_handoff_and_wait(struct rtl_device* dev, struct rtl_tcp_conn* conn, struct rtl_tcp_loop_state* st,
                         int* skip_odd) {
    if (rtl_tcp_queue_handoff(dev, conn) != 0) {
        return RTL_TCP_HANDOFF_CANCELLED;
    }
    rtl_tcp_post_handoff(dev);

    unsigned int parity = 0U;
    int dead = 0;
    for (;;) {
        struct rtl_tcp_conn* withdrawn = nullptr;
        rtl_tcp_lock(dev);
        const int verdict = rtl_tcp_handoff_verdict_locked(dev, dead, &withdrawn);
        if (verdict == RTL_TCP_HANDOFF_NONE && dead) {
            /* Claimed: the controller's replay fails on it, or publishes it; either way it says so. */
            rtl_tcp_wait_slice(dev);
        }
        rtl_tcp_unlock(dev);
        rtl_tcp_conn_unref(withdrawn);
        if (verdict == RTL_TCP_HANDOFF_RELEASED) {
            break;
        }
        if (verdict != RTL_TCP_HANDOFF_NONE) {
            return verdict;
        }
        if (!dead) {
            dead = rtl_tcp_discard_slice(conn, st, &parity);
        }
    }
    rtl_tcp_ack_handoff_release(dev, parity, skip_odd);
    return RTL_TCP_HANDOFF_RELEASED;
}

/* One reconnect attempt: connect, read the header and hand the connection over (rtl_tcp_handoff_and_wait()). Returns
 * 1 with the released connection in @p out (the reader's reference), 0 to try again, or -1 once the session ends. */
static int
rtl_tcp_reconnect_attempt(struct rtl_device* s, struct rtl_tcp_loop_state* st, int* skip_odd,
                          struct rtl_tcp_conn** out) {
    dsd_socket_t sock = tcp_connect_host(s->host, s->port, s);
    if (sock == DSD_INVALID_SOCKET) {
        return 0;
    }
    rtl_tcp_prepare_socket(sock);
    uint32_t tuner_type = 0U;
    const rtl_tcp_header_result hr =
        rtl_tcp_read_header(sock, kRtlTcpHeaderMs, rtl_tcp_cancelled, s, &tuner_type, nullptr);
    struct rtl_tcp_conn* conn = (hr == RTL_TCP_HEADER_SUCCESS) ? rtl_tcp_conn_new(sock) : nullptr;
    if (conn) {
        /* Before the hand-off moves the generation: a hop recorded under the old one rewrites what this tuner needs. */
        s->tcp_tuner_type.store((int)tuner_type, std::memory_order_release);
    }
    if (!conn) {
        dsd_socket_close(sock);
        if (hr == RTL_TCP_HEADER_CANCELLED) {
            return -1;
        }
        if (hr != RTL_TCP_HEADER_SUCCESS) {
            DSD_FPRINTF(stderr, "rtl_tcp: %s:%d sent no rtl_tcp header (%s).\n", s->host, s->port,
                        rtl_tcp_header_failure_name(hr));
        }
        return 0;
    }
    const int handed = rtl_tcp_handoff_and_wait(s, conn, st, skip_odd);
    if (handed == RTL_TCP_HANDOFF_RELEASED) {
        *out = conn;
        return 1;
    }
    rtl_tcp_conn_unref(conn);
    return (handed == RTL_TCP_HANDOFF_CANCELLED) ? -1 : 0;
}

/* Whether the reconnect loop goes on: the session runs and no stop latched. */
static int
rtl_tcp_reconnect_continues(struct rtl_device* s) {
    return s->run.load() && !rtl_tcp_cancelled(s);
}

/* The reader lost @p lost (its reference is dropped here; NULL when it never had one), for the reason @p why (logged;
 * NULL when it was reported already): take it down if it is still the published one, then reconnect until a new
 * connection was published and released to the reader, which is returned with the reader's reference, or the session
 * ends (NULL). Each attempt connects, reads the header and hands the connection over (rtl_tcp_handoff_and_wait()); all
 * of it is bounded and cancellable. */
static struct rtl_tcp_conn*
rtl_tcp_reconnect(struct rtl_device* s, struct rtl_tcp_conn* lost, const char* why, struct rtl_tcp_loop_state* st,
                  int* skip_odd) {
    rtl_tcp_lock(s);
    struct rtl_tcp_conn* dropped = (lost && s->tcp_conn == lost) ? rtl_tcp_unpublish_locked(s) : nullptr;
    rtl_tcp_unlock(s);
    rtl_tcp_conn_unref(dropped);
    rtl_tcp_conn_unref(lost);
    if (!rtl_tcp_reconnect_continues(s)) {
        return nullptr;
    }
    if (why) {
        DSD_FPRINTF(stderr, "rtl_tcp: %s; attempting to reconnect to %s:%d...\n", why, s->host, s->port);
    }

    int attempt = 0;
    while (rtl_tcp_reconnect_continues(s)) {
        attempt++;
        struct rtl_tcp_conn* conn = nullptr;
        const int got = rtl_tcp_reconnect_attempt(s, st, skip_odd, &conn);
        if (got > 0) {
            DSD_FPRINTF(stderr, "rtl_tcp: reconnected on attempt %d.\n", attempt);
            return conn;
        }
        if (got < 0) {
            return nullptr;
        }
        /* Back off, a slice at a time so a stop is not kept waiting. */
        const uint64_t until_ms = dsd_realtime_mono_ms() + (uint64_t)(200 * (attempt < 10 ? attempt : 10));
        while (rtl_tcp_reconnect_continues(s) && dsd_realtime_mono_ms() < until_ms) {
            dsd_sleep_ms(10U);
        }
    }
    return nullptr;
}

/* Whether a chunk of @p r bytes may be processed: the throughput watchdog (lag resilience) instead declares the
 * connection lost when the server falls too far behind. */
static int
rtl_tcp_watchdog_allows_processing(struct rtl_device* s, int r) {
    if (!s) {
        return 0;
    }
    uint64_t recv_ns = dsd_realtime_mono_ns();
    if (!rtl_tcp_metrics_record_recv_device(s, (uint32_t)r, recv_ns)) {
        return 1;
    }
    DSD_FPRINTF(stderr, "rtl_tcp: throughput watchdog triggered; reconnecting to %s:%d...\n", s->host, s->port);
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

static void
rtl_tcp_copy_cu8_with_moments(unsigned char* dst, const unsigned char* src, size_t len,
                              dsd_input_level_cu8_moments* moments) {
    if (!dst || !src || len == 0U) {
        return;
    }
    if (!moments) {
        DSD_MEMCPY(dst, src, len);
        return;
    }

    dsd_input_level_cu8_moments local;
    dsd_input_level_cu8_moments_reset(&local);
    for (size_t i = 0U; i < len; i++) {
        const unsigned char sample = src[i];
        dst[i] = sample;
        rtl_cu8_moments_add_sample(&local, sample);
    }
    (void)dsd_input_level_cu8_moments_merge(moments, &local);
}

static size_t
rtl_tcp_fill_pending_slice(struct rtl_device* s, const unsigned char* u8, size_t len, size_t slice,
                           const struct rtl_u8_transform_policy_state* policy, dsd_input_level_cu8_moments* moments) {
    if (!s || !u8 || !policy || s->tcp_pending_len == 0U) {
        return 0U;
    }
    size_t consumed = 0U;
    size_t missing = (slice > s->tcp_pending_len) ? (slice - s->tcp_pending_len) : 0U;
    size_t take = (missing < len) ? missing : len;
    if (take > 0U && rtl_tcp_ensure_pending_capacity(s, slice) && s->tcp_pending_cap >= (s->tcp_pending_len + take)) {
        rtl_tcp_copy_cu8_with_moments(s->tcp_pending + s->tcp_pending_len, u8, take, moments);
        s->tcp_pending_len += take;
        consumed += take;
    }
    if (s->tcp_pending_len == slice) {
        int ring_status = rtl_write_u8_to_ring(s, s->tcp_pending, slice, policy->fs4_shift_active, policy->use_two_pass,
                                               policy->combine_rotate_active, 1, NULL);
        s->tcp_pending_len = 0U;
        if (ring_status == 2) {
            if (moments && take < len) {
                (void)dsd_input_level_cu8_moments_accumulate(moments, u8 + take, len - take);
            }
            return len;
        }
    }
    return consumed;
}

static void
rtl_tcp_process_full_slices(struct rtl_device* s, unsigned char* u8, size_t len, size_t slice,
                            const struct rtl_u8_transform_policy_state* policy, size_t* io_consumed,
                            dsd_input_level_cu8_moments* moments) {
    if (!s || !u8 || !policy || !io_consumed || slice == 0U) {
        return;
    }
    while ((len - *io_consumed) >= slice) {
        int ring_status = rtl_write_u8_to_ring(s, u8 + *io_consumed, slice, policy->fs4_shift_active,
                                               policy->use_two_pass, policy->combine_rotate_active, 1, moments);
        *io_consumed += slice;
        if (ring_status == 2) {
            if (moments && *io_consumed < len) {
                (void)dsd_input_level_cu8_moments_accumulate(moments, u8 + *io_consumed, len - *io_consumed);
            }
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
rtl_tcp_store_remainder(struct rtl_device* s, const unsigned char* data, size_t rem, int fs4_shift_active,
                        dsd_input_level_cu8_moments* moments) {
    if (!s || !data || rem == 0U) {
        return;
    }
    if (!rtl_tcp_ensure_pending_capacity(s, rem)) {
        if (moments) {
            (void)dsd_input_level_cu8_moments_accumulate(moments, data, rem);
        }
        (void)rtl_drop_u8_bytes_preserve_alignment(data, rem, &s->iq_byte_carry, &s->rot_phase, fs4_shift_active);
        return;
    }
    rtl_tcp_copy_cu8_with_moments(s->tcp_pending, data, rem, moments);
    s->tcp_pending_len = rem;
}

static void
rtl_tcp_reassemble_and_submit(struct rtl_device* s, unsigned char* u8, uint32_t len,
                              const struct rtl_u8_transform_policy_state* policy,
                              dsd_input_level_cu8_moments* moments) {
    if (!s || !u8 || !policy) {
        return;
    }
    const size_t slice = (s->buf_len > 0 ? (size_t)s->buf_len : 16384U);
    size_t consumed = rtl_tcp_fill_pending_slice(s, u8, len, slice, policy, moments);
    if (consumed < len) {
        rtl_tcp_process_full_slices(s, u8, len, slice, policy, &consumed, moments);
    }
    if (consumed < len) {
        rtl_tcp_store_remainder(s, u8 + consumed, len - consumed, policy->fs4_shift_active, moments);
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

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" int
rtl_device_test_tcp_policy_helpers(size_t* bufsz_out, size_t bufsz_count, int* waitall_out, size_t waitall_count,
                                   uint64_t* delta_out, size_t delta_count, int* agc_out) {
    if (!bufsz_out || bufsz_count < 8U || !waitall_out || waitall_count < 4U || !delta_out || delta_count < 2U
        || !agc_out) {
        return -1;
    }

    rtl_device dev{};
    dsdneoRuntimeConfig cfg{};

    bufsz_out[0] = rtl_tcp_default_bufsz(NULL);
    dev.backend = RTL_BACKEND_TCP;
    bufsz_out[1] = rtl_tcp_default_bufsz(&dev);
    dev.backend = RTL_BACKEND_USB;
    dev.rate = 0U;
    bufsz_out[2] = rtl_tcp_default_bufsz(&dev);
    dev.rate = 48000U;
    bufsz_out[3] = rtl_tcp_default_bufsz(&dev);
    dev.rate = 960000U;
    bufsz_out[4] = rtl_tcp_default_bufsz(&dev);
    dev.rate = 10000000U;
    bufsz_out[5] = rtl_tcp_default_bufsz(&dev);
    cfg.tcp_bufsz_is_set = 1;
    cfg.tcp_bufsz_bytes = 12345;
    bufsz_out[6] = rtl_tcp_cfg_bufsz(&dev, &cfg);
    cfg.tcp_bufsz_bytes = 0;
    bufsz_out[7] = rtl_tcp_cfg_bufsz(&dev, &cfg);

    waitall_out[0] = rtl_tcp_cfg_waitall(NULL, NULL);
    dev.backend = RTL_BACKEND_TCP;
    waitall_out[1] = rtl_tcp_cfg_waitall(&dev, NULL);
    cfg = {};
    cfg.tcp_waitall_is_set = 1;
    cfg.tcp_waitall_enable = 1;
    waitall_out[2] = rtl_tcp_cfg_waitall(&dev, &cfg);
    cfg.tcp_waitall_enable = 0;
    waitall_out[3] = rtl_tcp_cfg_waitall(&dev, &cfg);

    delta_out[0] = rtl_tcp_counter_delta(100U, 40U);
    delta_out[1] = rtl_tcp_counter_delta(40U, 100U);
    *agc_out = env_agc_want();
    return 0;
}
#endif

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
    uint64_t now_ns = dsd_realtime_mono_ns();
    rtl_tcp_print_stats_if_due(s, now_ns);
    rtl_tcp_autotune_if_due(s, st, now_ns);
}

/* A retune went out on the connection the reader keeps reading (rtl_device_set_frequency()): drop the bytes it buffered
 * for the old frequency on their way to the ring (a partial slice of whole pairs; the capture already holds them). The
 * stream goes on, so the mute in flight and its byte phase are left alone; the rotation restarts at phase 0. */
static void
rtl_tcp_drop_buffered_for_retune(struct rtl_device* s) {
    s->tcp_pending_len = 0U;
    rtl_capture_u8_byte_carry_clear(&s->iq_byte_carry);
    s->rot_phase = 0;
    rtl_stream_input_level_reset();
}

/* One receive on @p conn into the loop buffer after its first @p held bytes; @p timed_out set when it ended for the
 * receive timeout alone. Where a
 * receive that timed out leaves the socket in an unknown state (Winsock, as for a send), the reader waits for data
 * itself, a slice at a time (so a stop is seen), and receives only what is there; elsewhere the socket's receive
 * timeout bounds a plain blocking receive. */
static int
rtl_tcp_reader_recv(const struct rtl_device* s, const struct rtl_tcp_conn* conn, struct rtl_tcp_loop_state* st,
                    size_t held, int* timed_out) {
    *timed_out = 0;
#if DSD_PLATFORM_WIN_NATIVE
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + st->recv_timeout_ms;
    for (;;) {
        if (!s->run.load() || conn->lost.load(std::memory_order_acquire)) {
            return 0;
        }
        const int ready = dsd_socket_wait(conn->sock, DSD_SOCKET_WAIT_READ, kRtlTcpSliceMs);
        if (ready < 0) {
            return -1;
        }
        if (ready > 0) {
            return dsd_socket_recv(conn->sock, st->u8 + held, st->bufsz - held, 0);
        }
        if (dsd_realtime_mono_ms() >= deadline_ms) {
            *timed_out = 1;
            return -1;
        }
    }
#else
    (void)s;
    const int r = dsd_socket_recv(conn->sock, st->u8 + held, st->bufsz - held, st->waitall ? MSG_WAITALL : 0);
    *timed_out = (r < 0 && rtl_tcp_recv_is_timeout(r)) ? 1 : 0;
    return r;
#endif
}

/* The reader's pipeline for @p len bytes (whole I/Q pairs) at the front of the loop buffer: the mute and the capture
 * hold, then the capture, the ring and the input level. */
static void
rtl_tcp_process_pairs(struct rtl_device* s, struct rtl_tcp_loop_state* st, uint32_t len) {
    rtl_u8_transform_policy_state policy{};
    rtl_tcp_load_transform_policy(s, &policy);
    if (rtl_tcp_apply_mute_and_reconfigure(s, st->u8, &len, policy.fs4_shift_active)) {
        return;
    }
    rtl_tcp_account_input_stats(s, len);
    rtl_submit_capture_bytes(s, st->u8, len);
    dsd_input_level_cu8_moments moments;
    dsd_input_level_cu8_moments_reset(&moments);
    rtl_tcp_reassemble_and_submit(s, st->u8, len, &policy, &moments);
    rtl_publish_cu8_input_level_moments(s, &moments);
    rtl_tcp_periodic_maintenance(s, st);
}

/* Why the reader lost @p conn, for the reconnect's log: NULL when a failed command took it down (which said so). */
static const char*
rtl_tcp_loss_reason(const struct rtl_tcp_conn* conn, int timed_out, int r) {
    if (conn->lost.load(std::memory_order_acquire)) {
        return nullptr;
    }
    if (timed_out) {
        return "input stalled";
    }
    return (r == 0) ? "the server closed the connection" : "the connection failed";
}

/* The published connection, with a reference for the reader; NULL with none. */
static struct rtl_tcp_conn*
rtl_tcp_reader_take_published(struct rtl_device* s) {
    rtl_tcp_lock(s);
    struct rtl_tcp_conn* conn = s->tcp_conn;
    if (conn) {
        rtl_tcp_conn_ref(conn);
    }
    rtl_tcp_unlock(s);
    return conn;
}

/* After a read that brought nothing (@p r <= 0): -1 once the session ends, 0 to read on (a receive timeout still
 * tolerated), or 1 once the reader lost @p *conn and reconnected (the new connection in @p *conn). */
static int
rtl_tcp_reader_empty_read(struct rtl_device* s, struct rtl_tcp_conn** conn, struct rtl_tcp_loop_state* st,
                          int timed_out, int r, int* skip_odd) {
    if (!s->run.load() || dsd_exitflag_load()) {
        return -1;
    }
    if (timed_out && ++st->consec_timeouts < st->timeout_limit) {
        return 0;
    }
    st->consec_timeouts = 0;
    *conn = rtl_tcp_reconnect(s, *conn, rtl_tcp_loss_reason(*conn, timed_out, r), st, skip_odd);
    return *conn ? 1 : -1;
}

/* Process @p r bytes the reader received behind the @p *held odd byte at the front of the loop buffer: whole I/Q pairs
 * go through rtl_tcp_process_pairs(), and an odd last byte stays at the front for its Q. */
static void
rtl_tcp_reader_process_chunk(struct rtl_device* s, struct rtl_tcp_loop_state* st, int r, size_t* held, int* skip_odd) {
    if (s->tcp_restart_pending.load(std::memory_order_relaxed)
        && s->tcp_restart_pending.exchange(0, std::memory_order_acq_rel)) {
        rtl_tcp_drop_buffered_for_retune(s);
    }
    uint32_t total = (uint32_t)*held + (uint32_t)r;
    if (*skip_odd) {
        *skip_odd = 0;
        DSD_MEMMOVE(st->u8, st->u8 + 1, total - 1U);
        total--;
        if (total == 0U) {
            return;
        }
    }
    uint32_t len = total & ~1U;
    *held = total - len;
    const unsigned char odd_byte = st->u8[total - 1U];
    if (len > 0U) {
        rtl_tcp_process_pairs(s, st, len);
    }
    if (*held != 0U) {
        st->u8[0] = odd_byte;
    }
}

/* TCP reader thread: read u8 IQ, widen to float, push to ring. A lost connection (an error, the server closing it,
 * too many receive timeouts in a row, the throughput watchdog) is reconnected here (rtl_tcp_reconnect()). */
static DSD_THREAD_RETURN_TYPE DSD_RTL_THREAD_CALL
tcp_thread_fn(void* arg) {
    struct rtl_device* s = static_cast<rtl_device*>(arg);
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    maybe_set_thread_realtime_and_affinity("DONGLE");
    struct rtl_tcp_loop_state st{};
    if (!rtl_tcp_init_loop_state(s, cfg, &st)) {
        LOG_ERROR("rtl_tcp: could not allocate the receive buffer; the stream stops\n");
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -ENOMEM);
        dsd_exitflag_store(1);
        replay_signal_input_waiters(s);
        s->run.store(0);
        DSD_THREAD_RETURN;
    }
    /* Set by a reconnect whose discard path ended on an odd byte: the next byte is a Q sample and is dropped. */
    int skip_odd = 0;
    /* What reaches the mute, the capture hold, the capture and the ring is whole I/Q pairs, as a USB reader's buffers
     * are: TCP hands the reader chunks of any length, and the byte accounting those share with the controller (a mute
     * armed while a chunk is being muted, the capture writer's half pair at an event) slips a pair on an odd chunk. An
     * odd last byte waits at the front of the buffer (@c held, 0 or 1) for its Q. */
    size_t held = 0U;
    /* A command may have lost the first connection before this thread ran: reconnect then. */
    struct rtl_tcp_conn* conn = rtl_tcp_reader_take_published(s);
    if (!conn) {
        conn = rtl_tcp_reconnect(s, nullptr, "the first connection was lost before the reader started", &st, &skip_odd);
    }
    if (!conn) {
        free(st.u8);
        s->run.store(0);
        DSD_THREAD_RETURN;
    }

    while (s->run.load() && dsd_exitflag_load() == 0) {
        rtl_tcp_apply_backpressure_if_needed(s);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
        g_test_tcp_read_hook.invoke();
#endif
        int timed_out = 0;
        int r = rtl_tcp_reader_recv(s, conn, &st, held, &timed_out);
        if (r > 0 && conn->lost.load(std::memory_order_acquire)) {
            r = 0;
        }
        if (r <= 0) {
            const int next = rtl_tcp_reader_empty_read(s, &conn, &st, timed_out, r, &skip_odd);
            if (next < 0) {
                break;
            }
            if (next > 0) {
                held = 0U; /* a new stream: the old one's half pair goes */
            }
            continue;
        }

        st.consec_timeouts = 0;
        if (!rtl_tcp_watchdog_allows_processing(s, r)) {
            /* The watchdog reported it. */
            conn = rtl_tcp_reconnect(s, conn, nullptr, &st, &skip_odd);
            if (!conn) {
                break;
            }
            held = 0U;
            continue;
        }
        rtl_tcp_reader_process_chunk(s, &st, r, &held, &skip_odd);
    }

    rtl_tcp_conn_unref(conn);
    free(st.u8);
    s->run.store(0);
    DSD_THREAD_RETURN;
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* While set, rtlsdr_open() and every control below reach this whole fake dongle (rtl_device_test_usb_dongle_install()),
 * through the gain and tuning fakes for theirs. Each open hands out a fresh token as the handle, so an op that reaches
 * a handle already closed is seen. */
static rtl_device_test_usb_dongle* g_test_usb_dongle = nullptr;
static unsigned char g_test_usb_dongle_tokens[64];
static unsigned int g_test_usb_dongle_opens = 0U;
static rtlsdr_dev_t* g_test_usb_dongle_handle = nullptr;

/* One op on the fake dongle through @p handle: noted, checked against the open handle and the streaming reader, and
 * refused when its fault says so (the gain fake's three ops keep their own faults). Returns 1 when refused. */
static int
rtl_usb_test_dongle_op(const rtlsdr_dev_t* handle, int op, int value) {
    rtl_device_test_usb_dongle* d = g_test_usb_dongle;
    if (d->on_op) {
        d->on_op(d->on_op_ctx, op);
    }
    if (op != RTL_TEST_DONGLE_OPEN && (handle == nullptr || handle != g_test_usb_dongle_handle)) {
        d->stale_handle_calls++;
    }
    if (g_test_usb_reader_streaming.load(std::memory_order_acquire)) {
        d->calls_while_reading++;
    }
    if (d->op_count < (int)(sizeof d->ops / sizeof d->ops[0])) {
        d->ops[d->op_count++] = op;
    }
    if (op == RTL_TEST_DONGLE_REG19 || op == RTL_TEST_DONGLE_GAIN_MODE || op == RTL_TEST_DONGLE_GAIN) {
        return 0;
    }
    const rtl_device_test_usb_gain_fault* fault = &d->faults[op];
    ++d->calls[op];
    if (fault->fail_from_call > 0 && d->calls[op] >= fault->fail_from_call) {
        return 1;
    }
    return (fault->fail_value_set && value == fault->fail_value) ? 1 : 0;
}

/* A read of the fake dongle through @p handle: only checked against the open handle. */
static void
rtl_usb_test_dongle_read(const rtlsdr_dev_t* handle) {
    if (handle == nullptr || handle != g_test_usb_dongle_handle) {
        g_test_usb_dongle->stale_handle_calls++;
    }
}
#endif

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* While set, the gain controls below reach this fake dongle in place of librtlsdr
 * (rtl_device_test_usb_gain_device()). */
static rtl_device_test_usb_gain_fake* g_test_usb_gain_fake = nullptr;
static rtl_device_test_usb_tuner_fake* g_test_usb_tuner_fake = nullptr;

/* Count a call to a fake control and say whether its fault refuses writing @p value. */
static int
rtl_usb_test_gain_refuses(const rtl_device_test_usb_gain_fault* fault, int* calls, int value) {
    ++*calls;
    if (fault->fail_from_call > 0 && *calls >= fault->fail_from_call) {
        return 1;
    }
    return (fault->fail_value_set && value == fault->fail_value) ? 1 : 0;
}

/* Write register 0x19 of the fake dongle as librtlsdr's setters do. */
static int
rtl_usb_test_gain_write_reg19(int value) {
    rtl_device_test_usb_gain_fake* fake = g_test_usb_gain_fake;
    if (rtl_usb_test_gain_refuses(&fake->reg19_fault, &fake->reg19_calls, value)) {
        return -1;
    }
    fake->reg19 = value;
    return 0;
}
#endif

/* The librtlsdr gain controls: register 0x19 (test mode and digital AGC), the tuner's gain mode, gain and gain
 * table. */
static int
rtl_usb_ctl_set_testmode(rtlsdr_dev_t* dev, int on) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        (void)rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_REG19, on ? 0x03 : 0x05);
    }
    if (g_test_usb_gain_fake) {
        return rtl_usb_test_gain_write_reg19(on ? 0x03 : 0x05);
    }
    if (g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_testmode(dev, on);
}

static int
rtl_usb_ctl_set_agc_mode(rtlsdr_dev_t* dev, int on) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        (void)rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_REG19, on ? 0x25 : 0x05);
    }
    if (g_test_usb_gain_fake) {
        return rtl_usb_test_gain_write_reg19(on ? 0x25 : 0x05);
    }
    if (g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_agc_mode(dev, on);
}

static int
rtl_usb_ctl_set_tuner_gain_mode(rtlsdr_dev_t* dev, int manual) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        (void)rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_GAIN_MODE, manual);
    }
    if (rtl_device_test_usb_gain_fake* fake = g_test_usb_gain_fake) {
        if (rtl_usb_test_gain_refuses(&fake->mode_fault, &fake->mode_calls, manual)) {
            return -1;
        }
        fake->manual = manual;
        return 0;
    }
    if (g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_tuner_gain_mode(dev, manual);
}

static int
rtl_usb_ctl_set_tuner_gain(rtlsdr_dev_t* dev, int gain) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        (void)rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_GAIN, gain);
    }
    if (rtl_device_test_usb_gain_fake* fake = g_test_usb_gain_fake) {
        /* librtlsdr forgets the gain it had once a write fails. */
        if (rtl_usb_test_gain_refuses(&fake->gain_fault, &fake->gain_calls, gain)) {
            fake->gain = 0;
            return -1;
        }
        fake->gain = gain;
        return 0;
    }
    if (g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_tuner_gain(dev, gain);
}

static int
rtl_usb_ctl_get_tuner_gain(rtlsdr_dev_t* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        rtl_usb_test_dongle_read(dev);
    }
    if (g_test_usb_gain_fake) {
        return g_test_usb_gain_fake->gain;
    }
    if (g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_get_tuner_gain(dev);
}

static int
rtl_usb_ctl_get_tuner_gains(rtlsdr_dev_t* dev, int* gains) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        rtl_usb_test_dongle_read(dev);
    }
    if (const rtl_device_test_usb_gain_fake* fake = g_test_usb_gain_fake) {
        const int count = std::min(fake->gain_count, (int)(sizeof fake->gains / sizeof fake->gains[0]));
        if (count > 0 && gains) {
            DSD_MEMCPY(gains, fake->gains, sizeof(int) * (size_t)count);
        }
        return count;
    }
    if (g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_get_tuner_gains(dev, gains);
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* While set, the tuning controls below reach this fake dongle in place of librtlsdr
 * (rtl_device_test_usb_tuner_device()). */

/* The tuner filter librtlsdr sets outside offset tuning: the bandwidth last set explicitly, else the sample rate. */
static uint32_t
rtl_usb_test_tuner_plain_bw(const rtl_device_test_usb_tuner_fake* fake) {
    return (fake->bw > 0U) ? fake->bw : fake->rate;
}

/* rtlsdr_set_offset_tuning() on the fake dongle. */
static int
rtl_usb_test_tuner_set_offset(rtl_device_test_usb_tuner_fake* fake, int on) {
    if (fake->tuner_type == RTLSDR_TUNER_R820T || fake->tuner_type == RTLSDR_TUNER_R828D) {
        return -2;
    }
    if (fake->direct_sampling) {
        return -3;
    }
    /* From the rate the dongle holds, which is 0 on a fresh handle. */
    fake->offs_freq = on ? rtl_usb_offs_for_rate(fake->rate) : 0U;
    fake->filter_bw = on ? 2U * fake->offs_freq : rtl_usb_test_tuner_plain_bw(fake);
    return fake->offset_result;
}
#endif

/* The librtlsdr tuning controls: centre frequency, sample rate, direct sampling, offset tuning, tuner bandwidth. */
static int
rtl_usb_ctl_set_center_freq(rtlsdr_dev_t* dev, uint32_t freq) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle && rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_FREQ, (int)freq)) {
        return -1;
    }
    if (rtl_device_test_usb_tuner_fake* fake = g_test_usb_tuner_fake) {
        fake->frequency_calls++;
        if (fake->fail_frequency_from_call > 0 && fake->frequency_calls >= fake->fail_frequency_from_call) {
            return -1;
        }
        fake->freq = freq;
        if (fake->tuner_type == RTLSDR_TUNER_FC2580) {
            /* fc2580_set_freq(): the band's channel filter, set on every tune from the LO in kHz. */
            const uint32_t lo_khz = (freq - fake->offs_freq) / 1000U;
            fake->filter_bw = (lo_khz > 1000000U) ? 1530000U : (lo_khz > 400000U) ? 7800000U : 6800000U;
        }
        return 0;
    }
    if (g_test_usb_gain_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_center_freq(dev, freq);
}

static uint32_t
rtl_usb_ctl_get_center_freq(rtlsdr_dev_t* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        rtl_usb_test_dongle_read(dev);
    }
    if (g_test_usb_tuner_fake) {
        return g_test_usb_tuner_fake->freq;
    }
    if (g_test_usb_gain_fake) {
        return 0U;
    }
#endif
    return rtlsdr_get_center_freq(dev);
}

static int
rtl_usb_ctl_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle && rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_RATE, (int)rate)) {
        return -1;
    }
    if (rtl_device_test_usb_tuner_fake* fake = g_test_usb_tuner_fake) {
        if (rate <= 225000U || rate > 3200000U || (rate > 300000U && rate <= 900000U)) {
            return -EINVAL;
        }
        if (fake->freq == 0U) {
            fake->rate_without_freq = 1;
        }
        fake->rate = fake->actual_rate ? fake->actual_rate : rate;
        fake->filter_bw = rtl_usb_test_tuner_plain_bw(fake);
        /* librtlsdr recomputes the offset for the new rate while offset tuning is on. */
        if (fake->offs_freq != 0U) {
            (void)rtl_usb_test_tuner_set_offset(fake, 1);
        }
        return 0;
    }
    if (g_test_usb_gain_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_sample_rate(dev, rate);
}

static uint32_t
rtl_usb_ctl_get_sample_rate(rtlsdr_dev_t* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        rtl_usb_test_dongle_read(dev);
    }
    if (g_test_usb_tuner_fake) {
        return g_test_usb_tuner_fake->rate;
    }
    if (g_test_usb_gain_fake) {
        return 0U;
    }
#endif
    return rtlsdr_get_sample_rate(dev);
}

static int
rtl_usb_ctl_set_direct_sampling(rtlsdr_dev_t* dev, int on) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle && rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_DIRECT_SAMPLING, on)) {
        return -1;
    }
    if (rtl_device_test_usb_tuner_fake* fake = g_test_usb_tuner_fake) {
        fake->direct_sampling = on;
        return 0;
    }
    if (g_test_usb_gain_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_direct_sampling(dev, on);
}

static int
rtl_usb_ctl_set_offset_tuning(rtlsdr_dev_t* dev, int on) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle && rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_OFFSET, on)) {
        return -1;
    }
    if (rtl_device_test_usb_tuner_fake* fake = g_test_usb_tuner_fake) {
        return rtl_usb_test_tuner_set_offset(fake, on);
    }
    if (g_test_usb_gain_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_offset_tuning(dev, on);
}

static int
rtl_usb_ctl_get_offset_tuning(rtlsdr_dev_t* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        rtl_usb_test_dongle_read(dev);
    }
    if (g_test_usb_tuner_fake) {
        return (g_test_usb_tuner_fake->offs_freq != 0U) ? 1 : 0;
    }
    if (g_test_usb_gain_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_get_offset_tuning(dev);
}

static int
rtl_usb_ctl_set_tuner_bandwidth(rtlsdr_dev_t* dev, uint32_t bw_hz) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle && rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_BANDWIDTH, (int)bw_hz)) {
        return -1;
    }
    if (rtl_device_test_usb_tuner_fake* fake = g_test_usb_tuner_fake) {
        fake->bw = bw_hz;
        fake->filter_bw = rtl_usb_test_tuner_plain_bw(fake);
        return 0;
    }
    if (g_test_usb_gain_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_tuner_bandwidth(dev, (int)bw_hz);
}

static int
rtl_usb_ctl_get_tuner_type(rtlsdr_dev_t* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        rtl_usb_test_dongle_read(dev);
    }
    if (g_test_usb_tuner_fake) {
        return g_test_usb_tuner_fake->tuner_type;
    }
    if (g_test_usb_gain_fake) {
        return RTLSDR_TUNER_UNKNOWN;
    }
#endif
    return rtlsdr_get_tuner_type(dev);
}

/* Opening and closing the dongle, its FIFO reset, and the controls a USB recovery replays beside gain and tuning:
 * frequency correction, crystals, IF gains and the bias tee. */
static int
rtl_usb_ctl_open(rtlsdr_dev_t** out, uint32_t index) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rtl_device_test_usb_dongle* d = g_test_usb_dongle) {
        d->op_count = 0;
        if (rtl_usb_test_dongle_op(nullptr, RTL_TEST_DONGLE_OPEN, (int)index) || g_test_usb_dongle_handle) {
            return -6;
        }
        /* Power-on state; the tuner type, gain table and faults stay. */
        d->tuner.rate = 0U;
        d->tuner.freq = 0U;
        d->tuner.offs_freq = 0U;
        d->tuner.bw = 0U;
        d->tuner.filter_bw = 0U;
        d->tuner.direct_sampling = 0;
        d->tuner.rate_without_freq = 0;
        d->gain.reg19 = 0x05;
        d->gain.manual = 0;
        d->gain.gain = 0;
        d->ppm = 0;
        d->rtl_xtal = 0U;
        d->tuner_xtal = 0U;
        d->bias_tee = 0;
        d->if_gain_count = 0;
        const size_t token = g_test_usb_dongle_opens++ % sizeof g_test_usb_dongle_tokens;
        g_test_usb_dongle_handle = reinterpret_cast<rtlsdr_dev_t*>(&g_test_usb_dongle_tokens[token]);
        d->open_handles++;
        *out = g_test_usb_dongle_handle;
        return 0;
    }
#endif
    return rtlsdr_open(out, index);
}

static int
rtl_usb_ctl_close(rtlsdr_dev_t* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rtl_device_test_usb_dongle* d = g_test_usb_dongle) {
        (void)rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_CLOSE, 0);
        if (dev != nullptr && dev == g_test_usb_dongle_handle) {
            g_test_usb_dongle_handle = nullptr;
            d->open_handles--;
        }
        return 0;
    }
    if (g_test_usb_gain_fake || g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_close(dev);
}

static int
rtl_usb_ctl_reset_buffer(rtlsdr_dev_t* dev) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (g_test_usb_dongle) {
        return rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_RESET_BUFFER, 0) ? -1 : 0;
    }
    if (g_test_usb_gain_fake || g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_reset_buffer(dev);
}

static int
rtl_usb_ctl_set_freq_correction(rtlsdr_dev_t* dev, int ppm) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rtl_device_test_usb_dongle* d = g_test_usb_dongle) {
        if (rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_PPM, ppm)) {
            return -1;
        }
        /* librtlsdr refuses the correction it already holds. */
        if (d->ppm == ppm) {
            return -2;
        }
        d->ppm = ppm;
        return 0;
    }
    if (g_test_usb_gain_fake || g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_freq_correction(dev, ppm);
}

static int
rtl_usb_ctl_set_xtal_freq(rtlsdr_dev_t* dev, uint32_t rtl_xtal_hz, uint32_t tuner_xtal_hz) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rtl_device_test_usb_dongle* d = g_test_usb_dongle) {
        if (rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_XTAL, (int)rtl_xtal_hz)) {
            return -1;
        }
        /* 0 leaves a crystal as it is. */
        d->rtl_xtal = rtl_xtal_hz ? rtl_xtal_hz : d->rtl_xtal;
        d->tuner_xtal = tuner_xtal_hz ? tuner_xtal_hz : d->tuner_xtal;
        return 0;
    }
    if (g_test_usb_gain_fake || g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_xtal_freq(dev, rtl_xtal_hz, tuner_xtal_hz);
}

static int
rtl_usb_ctl_set_tuner_if_gain(rtlsdr_dev_t* dev, int stage, int gain_tenth_db) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rtl_device_test_usb_dongle* d = g_test_usb_dongle) {
        if (rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_IF_GAIN, gain_tenth_db)) {
            return -1;
        }
        int slot = -1;
        for (int i = 0; i < d->if_gain_count && slot < 0; i++) {
            if (d->if_gains[i].stage == stage) {
                slot = i;
            }
        }
        if (slot < 0) {
            if (d->if_gain_count >= (int)(sizeof d->if_gains / sizeof d->if_gains[0])) {
                return -1; /* a stage beyond the ones the fake has room for */
            }
            slot = d->if_gain_count++;
        }
        d->if_gains[slot].stage = stage;
        d->if_gains[slot].gain = gain_tenth_db;
        return 0;
    }
    if (g_test_usb_gain_fake || g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_tuner_if_gain(dev, stage, (int16_t)gain_tenth_db);
}

#ifdef USE_RTLSDR_BIAS_TEE
static int
rtl_usb_ctl_set_bias_tee(rtlsdr_dev_t* dev, int on) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (rtl_device_test_usb_dongle* d = g_test_usb_dongle) {
        if (rtl_usb_test_dongle_op(dev, RTL_TEST_DONGLE_BIAS_TEE, on)) {
            return -1;
        }
        d->bias_tee = on;
        return 0;
    }
    if (g_test_usb_gain_fake || g_test_usb_tuner_fake) {
        return DSD_ERR_NOT_SUPPORTED;
    }
#endif
    return rtlsdr_set_bias_tee(dev, on);
}
#endif

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" void
rtl_device_test_usb_dongle_install(rtl_device_test_usb_dongle* fake) {
    g_test_usb_dongle = fake;
    g_test_usb_dongle_handle = nullptr;
    g_test_usb_gain_fake = fake ? &fake->gain : nullptr;
    g_test_usb_tuner_fake = fake ? &fake->tuner : nullptr;
}
#endif

/**
 * @brief Find the nearest supported gain to the target gain.
 *
 * The status travels apart from the value, since a tuner's gains reach below zero (E4K -1.0 dB, FC0012/FC0013
 * -9.9 dB), and a table that cannot be read is an error rather than a gain of 0 (B10).
 *
 * @param dev RTL-SDR device handle.
 * @param target_gain Target gain in tenths of dB.
 * @param out_gain Nearest supported gain in tenths of dB, written on success.
 * @return 0 on success, or a negative error code when the tuner's gain table cannot be read.
 */
static int
nearest_gain(rtlsdr_dev_t* dev, int target_gain, int* out_gain) {
    int count = rtl_usb_ctl_get_tuner_gains(dev, NULL);
    if (count <= 0) {
        return -1;
    }
    std::vector<int> gains((size_t)count);
    int filled = rtl_usb_ctl_get_tuner_gains(dev, gains.data());
    if (filled <= 0) {
        return -1;
    }
    if (filled < count) {
        count = filled;
    }
    int nearest = gains[0];
    for (int i = 1; i < count; i++) {
        if (llabs((long long)target_gain - gains[(size_t)i]) < llabs((long long)target_gain - nearest)) {
            nearest = gains[(size_t)i];
        }
    }
    *out_gain = nearest;
    return 0;
}

namespace {

struct rtl_usb_apply_policy {
    int verify_enabled;
    int attempts;
};

/* A control's apply and verify take the context type that control is applied with, so the retry and replay helpers
 * are instantiated per context type and no control's context reaches another control's callback through a shared
 * untyped pointer. Ctx is deduced from the context argument alone, so a NULL verify still converts. */
template <typename Ctx>
struct rtl_usb_apply_ops {
    typedef int (*apply_fn)(Ctx* ctx);
    typedef int (*verify_fn)(Ctx* ctx);
};

static struct rtl_usb_apply_policy
rtl_usb_apply_policy_from_runtime(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    struct rtl_usb_apply_policy policy{};
    policy.verify_enabled = (!cfg || cfg->rtl_verify_enable) ? 1 : 0;
    policy.attempts = cfg ? cfg->rtl_verify_attempts : 10;
    if (policy.attempts < 1) {
        policy.attempts = 1;
    }
    if (policy.attempts > 10) {
        policy.attempts = 10;
    }
    if (!policy.verify_enabled) {
        policy.attempts = 1;
    }
    return policy;
}

template <typename Ctx>
static int
rtl_usb_apply_with_retry(const struct rtl_usb_apply_policy* policy, typename rtl_usb_apply_ops<Ctx>::apply_fn apply,
                         typename rtl_usb_apply_ops<Ctx>::verify_fn verify, Ctx* ctx, int* out_attempts) {
    if (!policy || !apply) {
        return -1;
    }
    int attempts = policy->attempts;
    if (attempts < 1) {
        attempts = 1;
    }
    if (attempts > 10) {
        attempts = 10;
    }
    if (!policy->verify_enabled) {
        attempts = 1;
    }

    int last_rc = -1;
    int used_attempts = 0;
    for (int i = 0; i < attempts; i++) {
        used_attempts = i + 1;
        last_rc = apply(ctx);
        if (last_rc != 0) {
            continue;
        }
        if (!policy->verify_enabled || !verify || verify(ctx) == 0) {
            if (out_attempts) {
                *out_attempts = used_attempts;
            }
            return 0;
        }
        last_rc = -1;
    }

    if (out_attempts) {
        *out_attempts = used_attempts;
    }
    return (last_rc < 0) ? last_rc : -1;
}

template <typename Ctx>
static int
rtl_usb_apply_with_runtime_retry(typename rtl_usb_apply_ops<Ctx>::apply_fn apply,
                                 typename rtl_usb_apply_ops<Ctx>::verify_fn verify, Ctx* ctx, int* out_attempts) {
    struct rtl_usb_apply_policy policy = rtl_usb_apply_policy_from_runtime();
    return rtl_usb_apply_with_retry<Ctx>(&policy, apply, verify, ctx, out_attempts);
}

static void
rtl_usb_log_retry_attempts(const char* control, int attempts) {
    if (attempts > 1) {
        DSD_FPRINTF(stderr, "RTL-SDR %s took %d attempts.\n", control ? control : "control apply", attempts);
    }
}

struct rtl_usb_frequency_apply_ctx {
    rtlsdr_dev_t* dev;
    uint32_t frequency;
};

static int
rtl_usb_apply_center_freq(rtl_usb_frequency_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_center_freq(ctx->dev, ctx->frequency) : -1;
}

static int
rtl_usb_verify_center_freq(rtl_usb_frequency_apply_ctx* ctx) {
    if (!ctx || !ctx->dev) {
        return -1;
    }
    return (rtl_usb_ctl_get_center_freq(ctx->dev) == ctx->frequency) ? 0 : -1;
}

struct rtl_usb_sample_rate_apply_ctx {
    rtlsdr_dev_t* dev;
    uint32_t requested_rate;
    uint32_t actual_rate;
};

static int
rtl_usb_sample_rate_readback_is_usable(uint32_t actual_rate) {
    return actual_rate != 0U;
}

static int
rtl_usb_apply_sample_rate(rtl_usb_sample_rate_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_sample_rate(ctx->dev, ctx->requested_rate) : -1;
}

static int
rtl_usb_verify_sample_rate(rtl_usb_sample_rate_apply_ctx* ctx) {
    if (!ctx || !ctx->dev) {
        return -1;
    }
    uint32_t actual = rtl_usb_ctl_get_sample_rate(ctx->dev);
    if (!rtl_usb_sample_rate_readback_is_usable(actual)) {
        return -1;
    }
    ctx->actual_rate = actual;
    return 0;
}

/* Demod register 0x19 holds test mode and the RTL2832U's digital AGC both, and librtlsdr's setter for each writes the
 * whole register (test mode 0x03, digital AGC 0x25, neither 0x05), so whichever was written last wins (B9). Resolve it
 * from the record: test mode while it is on, else the digital AGC as wanted. One write: a compound gain change runs it
 * as one of its steps inside its own retries, and a caller that runs it alone wraps it in the retry policy. */
static int
rtl_usb_write_register19(rtlsdr_dev_t* handle, int testmode, int digital_agc) {
    if (!handle) {
        return -1;
    }
    if (testmode) {
        return rtl_usb_ctl_set_testmode(handle, 1);
    }
    return rtl_usb_ctl_set_agc_mode(handle, digital_agc ? 1 : 0);
}

static int
rtl_usb_apply_register19(struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    return rtl_usb_write_register19(dev->dev, dev->testmode_on, dev->digital_agc_wanted);
}

static int
rtl_usb_apply_register19_once(struct rtl_device* dev) {
    return rtl_usb_apply_register19(dev);
}

/* The tuner's side of a gain state on @p handle: its gain mode and, for a manual @p gain, the gain. Shared by the gain
 * transaction and a recovery's replay. */
static int
rtl_usb_write_tuner_gain(rtlsdr_dev_t* handle, int gain) {
    if (!handle) {
        return -1;
    }
    if (gain == RTL_AUTO_GAIN) {
        return rtl_usb_ctl_set_tuner_gain_mode(handle, 0);
    }
    const int r = rtl_usb_ctl_set_tuner_gain_mode(handle, 1);
    return (r != 0) ? r : rtl_usb_ctl_set_tuner_gain(handle, gain);
}

/* Whether the tuner on @p handle reads back the manual @p gain (automatic gain has nothing to read back). */
static int
rtl_usb_verify_tuner_gain_on(rtlsdr_dev_t* handle, int gain) {
    if (!handle) {
        return -1;
    }
    return (gain == RTL_AUTO_GAIN || rtl_usb_ctl_get_tuner_gain(handle) == gain) ? 0 : -1;
}

/* Apply the record's gain state once, as one compound control: a manual gain is the digital AGC off, the tuner's
 * manual mode and the gain; automatic gain is the tuner's automatic mode and the digital AGC as wanted. Register 0x19
 * is resolved with test mode, so a gain change never ends test mode. */
static int
rtl_usb_apply_gain_record(struct rtl_device* dev) {
    if (!dev || !dev->dev) {
        return -1;
    }
    int r = 0;
    if (dev->gain == RTL_AUTO_GAIN) {
        r = rtl_usb_write_tuner_gain(dev->dev, RTL_AUTO_GAIN);
        return (r != 0) ? r : rtl_usb_apply_register19(dev);
    }
    r = rtl_usb_apply_register19(dev);
    return (r != 0) ? r : rtl_usb_write_tuner_gain(dev->dev, dev->gain);
}

static int
rtl_usb_verify_gain_record(struct rtl_device* dev) {
    return dev ? rtl_usb_verify_tuner_gain_on(dev->dev, dev->gain) : -1;
}

/* Copy the gain record, as a change left it, into the effective record a recovery replays. */
static void
rtl_usb_effective_note_gain(struct rtl_device* dev) {
    struct rtl_usb_effective* e = &dev->usb_eff;
    e->gain = dev->gain;
    e->gain_mode = (dev->gain == RTL_AUTO_GAIN) ? 1 : 0;
    e->digital_agc = dev->digital_agc_wanted;
    e->gain_took = dev->gain_took;
    e->gain_unknown = dev->gain_state_unknown;
}

/* Move the USB gain controls to @p gain (RTL_AUTO_GAIN or tenths of a dB) with the digital AGC @p digital_agc, as one
 * transaction, under usb_ctrl_lock. The record holds the new state while it is applied (no getter sees it then, as
 * they take the lock) and keeps it only once the hardware took it. On a failure part way, the steps already applied
 * are rolled back by applying the state the controls last took; when that fails too, or there is none (the device's
 * first gain change), the hardware holds neither state, and the gain reads as unknown (no mode, no value) until a
 * later change takes, or a recovery puts the recorded gain back. Returns the change's result. */

static int
rtl_usb_change_gain(struct rtl_device* dev, int gain, int digital_agc, int* out_attempts) {
    const int previous_gain = dev->gain;
    const int previous_digital_agc = dev->digital_agc_wanted;
    dev->gain = gain;
    dev->digital_agc_wanted = digital_agc ? 1 : 0;
    int rc = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_gain_record, rtl_usb_verify_gain_record, dev, out_attempts);
    if (rc == 0) {
        dev->agc_mode = (gain == RTL_AUTO_GAIN) ? 1 : 0;
        dev->gain_took = 1;
        dev->gain_state_unknown = 0;
        rtl_usb_effective_note_gain(dev);
        return 0;
    }
    dev->gain = previous_gain;
    dev->digital_agc_wanted = previous_digital_agc;
    int attempts = 0;
    const int restored = dev->gain_took ? rtl_usb_apply_with_runtime_retry(rtl_usb_apply_gain_record,
                                                                           rtl_usb_verify_gain_record, dev, &attempts)
                                        : -1;
    if (restored == 0) {
        dev->gain_state_unknown = 0;
        rtl_usb_effective_note_gain(dev);
        DSD_FPRINTF(stderr, "RTL-SDR gain change failed; previous gain restored.\n");
        return rc;
    }
    dev->gain_state_unknown = 1;
    rtl_usb_effective_note_gain(dev);
    DSD_FPRINTF(stderr, "WARNING: RTL-SDR gain change failed and the previous gain could not be restored; the tuner "
                        "gain is unknown until a gain change takes.\n");
    return rc;
}

struct rtl_usb_int_apply_ctx {
    rtlsdr_dev_t* dev;
    int value;
};

static int
rtl_usb_apply_direct_sampling(rtl_usb_int_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_direct_sampling(ctx->dev, ctx->value) : -1;
}

static int
rtl_usb_apply_ppm(rtl_usb_int_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_freq_correction(ctx->dev, ctx->value) : -1;
}

static int
rtl_usb_apply_offset_tuning(rtl_usb_int_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_offset_tuning(ctx->dev, ctx->value ? 1 : 0) : -1;
}

struct rtl_usb_u32_apply_ctx {
    rtlsdr_dev_t* dev;
    uint32_t value;
};

static int
rtl_usb_apply_tuner_bandwidth(rtl_usb_u32_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_tuner_bandwidth(ctx->dev, ctx->value) : -1;
}

#ifdef USE_RTLSDR_BIAS_TEE
static int
rtl_usb_apply_bias_tee(rtl_usb_int_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_bias_tee(ctx->dev, ctx->value) : -1;
}
#endif

/* The rest of what a USB recovery replays onto the reopened handle (rtl_usb_replay_effective()), each from the
 * effective record rather than the device's fields. */
struct rtl_usb_xtal_apply_ctx {
    rtlsdr_dev_t* dev;
    uint32_t rtl_xtal_hz;
    uint32_t tuner_xtal_hz;
};

static int
rtl_usb_apply_xtal(rtl_usb_xtal_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_xtal_freq(ctx->dev, ctx->rtl_xtal_hz, ctx->tuner_xtal_hz) : -1;
}

static int
rtl_usb_verify_offset_tuning_on(rtl_usb_int_apply_ctx* ctx) {
    return (ctx && ctx->dev && rtl_usb_ctl_get_offset_tuning(ctx->dev) == 1) ? 0 : -1;
}

/* The tuner's gain mode and, when manual, its gain, as the gain transaction writes them. */
static int
rtl_usb_apply_tuner_gain(rtl_usb_int_apply_ctx* ctx) {
    return ctx ? rtl_usb_write_tuner_gain(ctx->dev, ctx->value) : -1;
}

static int
rtl_usb_verify_tuner_gain(rtl_usb_int_apply_ctx* ctx) {
    return ctx ? rtl_usb_verify_tuner_gain_on(ctx->dev, ctx->value) : -1;
}

struct rtl_usb_register19_apply_ctx {
    rtlsdr_dev_t* dev;
    int testmode;
    int digital_agc;
};

static int
rtl_usb_apply_register19_values(rtl_usb_register19_apply_ctx* ctx) {
    return ctx ? rtl_usb_write_register19(ctx->dev, ctx->testmode, ctx->digital_agc) : -1;
}

struct rtl_usb_if_gain_apply_ctx {
    rtlsdr_dev_t* dev;
    int stage;
    int gain;
};

static int
rtl_usb_apply_if_gain(rtl_usb_if_gain_apply_ctx* ctx) {
    return (ctx && ctx->dev) ? rtl_usb_ctl_set_tuner_if_gain(ctx->dev, ctx->stage, ctx->gain) : -1;
}

static int
rtl_usb_apply_reset_buffer(rtlsdr_dev_t* dev) {
    return dev ? rtl_usb_ctl_reset_buffer(dev) : -1;
}

} // namespace

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
namespace {
struct rtl_usb_retry_test_ctx {
    int apply_calls;
    int verify_calls;
    int apply_success_after;
    int verify_success_after;
};

static int
rtl_usb_retry_test_apply(rtl_usb_retry_test_ctx* ctx) {
    if (!ctx) {
        return -1;
    }
    ctx->apply_calls++;
    return (ctx->apply_calls >= ctx->apply_success_after) ? 0 : -1;
}

static int
rtl_usb_retry_test_verify(rtl_usb_retry_test_ctx* ctx) {
    if (!ctx) {
        return -1;
    }
    ctx->verify_calls++;
    return (ctx->verify_calls >= ctx->verify_success_after) ? 0 : -1;
}

} // namespace

extern "C" int
rtl_device_test_usb_apply_retry(int verify_enabled, int attempts, int apply_success_after, int verify_success_after,
                                int* out_apply_calls, int* out_verify_calls, int* out_used_attempts) {
    if (!out_apply_calls || !out_verify_calls || !out_used_attempts || apply_success_after < 1
        || verify_success_after < 1) {
        return -100;
    }
    rtl_usb_apply_policy policy{};
    policy.verify_enabled = verify_enabled ? 1 : 0;
    policy.attempts = attempts;
    rtl_usb_retry_test_ctx ctx{};
    ctx.apply_success_after = apply_success_after;
    ctx.verify_success_after = verify_success_after;
    int used_attempts = 0;
    int rc =
        rtl_usb_apply_with_retry(&policy, rtl_usb_retry_test_apply, rtl_usb_retry_test_verify, &ctx, &used_attempts);
    *out_apply_calls = ctx.apply_calls;
    *out_verify_calls = ctx.verify_calls;
    *out_used_attempts = used_attempts;
    return rc;
}

extern "C" int
rtl_device_test_usb_sample_rate_readback(uint32_t requested_rate, uint32_t actual_rate, uint32_t* out_actual_rate) {
    if (!out_actual_rate) {
        return -100;
    }
    UNUSED(requested_rate);
    *out_actual_rate = 0U;
    if (!rtl_usb_sample_rate_readback_is_usable(actual_rate)) {
        return -1;
    }
    *out_actual_rate = actual_rate;
    return 0;
}

#endif

/**
 * @brief Set RTL-SDR center frequency with a brief status message.
 *
 * @param dev RTL-SDR device handle.
 * @param frequency Center frequency in Hz.
 * @return 0 on success or a negative error code.
 */
static int
verbose_set_frequency(rtlsdr_dev_t* dev, uint32_t frequency) {
    rtl_usb_frequency_apply_ctx ctx{dev, frequency};
    int attempts = 0;
    int r = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_center_freq, rtl_usb_verify_center_freq, &ctx, &attempts);
    if (r < 0) {
        DSD_FPRINTF(stderr, " (WARNING: Failed to set Center Frequency). \n");
    } else {
        rtl_usb_log_retry_attempts("center frequency apply", attempts);
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
verbose_set_sample_rate(rtlsdr_dev_t* dev, uint32_t samp_rate, uint32_t* out_actual_rate) {
    rtl_usb_sample_rate_apply_ctx ctx{dev, samp_rate, 0U};
    int attempts = 0;
    int r = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_sample_rate, rtl_usb_verify_sample_rate, &ctx, &attempts);
    if (r < 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set sample rate.\n");
    } else {
        if (ctx.actual_rate == 0U) {
            ctx.actual_rate = rtl_usb_ctl_get_sample_rate(dev);
        }
        if (ctx.actual_rate == 0U) {
            ctx.actual_rate = samp_rate;
        }
        if (out_actual_rate) {
            *out_actual_rate = ctx.actual_rate;
        }
        rtl_usb_log_retry_attempts("sample rate apply", attempts);
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
    rtl_usb_int_apply_ctx ctx{dev, on};
    int attempts = 0;
    int r = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_direct_sampling, NULL, &ctx, &attempts);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set direct sampling mode.\n");
        return r;
    }
    rtl_usb_log_retry_attempts("direct sampling apply", attempts);
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
    rtl_usb_ctrl_lock(dev);
    const int t = dev->dev ? rtl_usb_ctl_get_tuner_type(dev->dev) : RTLSDR_TUNER_UNKNOWN;
    const int opened = dev->dev ? 1 : 0;
    rtl_usb_ctrl_unlock(dev);
    if (!opened) {
        return;
    }
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
    rtl_usb_u32_apply_ctx ctx{dev, bw_hz};
    int attempts = 0;
    int r = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_tuner_bandwidth, NULL, &ctx, &attempts);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set tuner bandwidth to %u Hz.\n", bw_hz);
    } else {
        rtl_usb_log_retry_attempts("tuner bandwidth apply", attempts);
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
 * @param dev RTL-SDR device.
 * @return 0 on success or a negative error code.
 */
static int
verbose_auto_gain(struct rtl_device* dev) {
    /* The RTL digital AGC runs with automatic gain unless the runtime config turns it off (DSD_NEO_RTL_AGC=0). */
    int want = env_agc_want();
    int attempts = 0;
    int r = rtl_usb_change_gain(dev, RTL_AUTO_GAIN, want, &attempts);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set automatic tuner gain.\n");
    } else {
        rtl_usb_log_retry_attempts("automatic gain apply", attempts);
        DSD_FPRINTF(stderr, "Tuner gain set to automatic.\n");
        DSD_FPRINTF(stderr, "RTL AGC %s%s.\n", want ? "enabled" : "disabled",
                    dev->testmode_on ? " once test mode is off" : "");
    }
    return r;
}

/**
 * @brief Set a fixed tuner gain with a message indicating the result.
 *
 * @param dev RTL-SDR device.
 * @param gain Desired gain in tenths of dB, one the tuner supports.
 * @return 0 on success or a negative error code.
 */
static int
verbose_gain_set(struct rtl_device* dev, int gain) {
    int attempts = 0;
    int r = rtl_usb_change_gain(dev, gain, 0, &attempts);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set tuner gain.\n");
    } else {
        rtl_usb_log_retry_attempts("manual gain apply", attempts);
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
    rtl_usb_int_apply_ctx ctx{dev, ppm_error};
    int attempts = 0;
    int r = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_ppm, NULL, &ctx, &attempts);
    if (r < 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set ppm error.\n");
    } else {
        rtl_usb_log_retry_attempts("PPM apply", attempts);
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
    r = rtl_usb_ctl_reset_buffer(dev);
    if (r < 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to reset buffers.\n");
    }
    return r;
}

// Public API Implementation

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
static std::atomic<int> g_test_common_sync_step{0};

extern "C" void
rtl_device_test_refuse_common_sync(int step) {
    g_test_common_sync_step.store(step, std::memory_order_relaxed);
}
#endif

template <typename Sync>
static int
rtl_device_common_sync_init(Sync* sync, int (*init)(Sync*)) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    const int step = g_test_common_sync_step.load(std::memory_order_relaxed);
    if (step > 0 && g_test_common_sync_step.fetch_sub(1, std::memory_order_relaxed) == 1) {
        return -EAGAIN;
    }
#endif
    return init(sync);
}

static void rtl_device_cleanup_common_state(struct rtl_device* dev);

static int
rtl_device_init_common_state(struct rtl_device* dev) {
    if (!dev) {
        return -1;
    }
    dev->iq_capture_writer = NULL;
    dev->capture_retune_count.store(0, std::memory_order_relaxed);
    dev->capture_mute_pending_bytes.store(0U, std::memory_order_relaxed);
    dev->capture_reconfigure_hold.store(RTL_CAPTURE_RECONFIGURE_INACTIVE, std::memory_order_relaxed);
    const dsdneoRuntimeConfig* runtime_config = dsd_neo_get_config();
    dev->live_combine_rotate_enabled = runtime_config ? (runtime_config->combine_rot != 0) : 1;
    dev->replay_fs4_shift_enabled = 0;
    dev->replay_historical_cu8_two_pass = 0;
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
    dev->soapy_startup_center_freq_hz = 0U;
    dev->soapy_nearest_rate_cache_requested = 0U;
    dev->soapy_nearest_rate_cache_actual = 0U;
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
    if (rtl_device_common_sync_init(&dev->tcp_metrics_lock, dsd_mutex_init) == 0) {
        dev->tcp_metrics_lock_inited = 1;
    }
    if (rtl_device_common_sync_init(&dev->tcp_lock, dsd_mutex_init) == 0) {
        if (rtl_device_common_sync_init(&dev->tcp_cond, dsd_cond_init) == 0) {
            dev->tcp_sync_inited = 1;
        } else {
            (void)dsd_mutex_destroy(&dev->tcp_lock);
        }
    }
    if (rtl_device_common_sync_init(&dev->usb_lifecycle_lock, dsd_mutex_init) == 0) {
        if (rtl_device_common_sync_init(&dev->usb_ctrl_lock, dsd_mutex_init) == 0) {
            dev->usb_locks_inited = 1;
        } else {
            (void)dsd_mutex_destroy(&dev->usb_lifecycle_lock);
        }
    }
    if (!dev->tcp_metrics_lock_inited || !dev->tcp_sync_inited || !dev->usb_locks_inited) {
        rtl_device_cleanup_common_state(dev);
        return -1;
    }
    return 0;
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
    if (dev->tcp_sync_inited) {
        (void)dsd_cond_destroy(&dev->tcp_cond);
        (void)dsd_mutex_destroy(&dev->tcp_lock);
        dev->tcp_sync_inited = 0;
    }
    if (dev->monitor_sync_inited) {
        (void)dsd_cond_destroy(&dev->monitor_wake);
        (void)dsd_mutex_destroy(&dev->monitor_lock);
        dev->monitor_sync_inited = 0;
    }
    if (dev->usb_locks_inited) {
        (void)dsd_mutex_destroy(&dev->usb_ctrl_lock);
        (void)dsd_mutex_destroy(&dev->usb_lifecycle_lock);
        dev->usb_locks_inited = 0;
    }
}

/*
 * Descriptor handed down from an Android app (USB-OTG). Written before the engine
 * starts and read on the engine/open path, so the two threads need it atomic.
 */
static std::atomic<int> g_last_usb_open_error{0};

int
rtl_device_last_open_error(void) {
    return g_last_usb_open_error.load(std::memory_order_acquire);
}

void
rtl_device_clear_open_error(void) {
    g_last_usb_open_error.store(0, std::memory_order_release);
}

static std::atomic<int> g_preopened_usb_fd{-1};

/*
 * The descriptor libusb has wrapped, or is about to wrap, as opposed to merely
 * recorded; -1 when none. Its owner (the Android app) has to close each connection
 * eventually and needs to know when that is safe for that one descriptor: "is the
 * engine running" is a poor proxy, because the descriptor is taken and released well
 * inside a run, and a later session holds a different descriptor (#782). Set before
 * the wrap is attempted and cleared once the device is closed, so it never stops
 * naming a descriptor while a transfer could still be in flight.
 */
static std::atomic<int> g_preopened_usb_fd_held{-1};

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
static std::atomic<int (*)(int)> g_open_fd_failure_hook{nullptr};
/* rtl_device_test_set_fd_reset_hook(): stands in for USBDEVFS_RESET on the descriptor, 0 or a negative errno. */
static std::atomic<int (*)(int)> g_fd_reset_hook{nullptr};
/* rtl_device_test_set_fd_identity_hook(): stands in for reading the descriptor's VID:PID and the opened handle's
 * strings, 0 or a negative errno. */
static std::atomic<int (*)(int, struct rtl_usb_identity*)> g_fd_identity_hook{nullptr};

extern "C" void
rtl_device_test_set_open_fd_failure_hook(int (*hook)(int)) {
    g_open_fd_failure_hook.store(hook, std::memory_order_release);
}

extern "C" void
rtl_device_test_set_fd_reset_hook(int (*hook)(int)) {
    g_fd_reset_hook.store(hook, std::memory_order_release);
}

extern "C" void
rtl_device_test_set_fd_identity_hook(int (*hook)(int, struct rtl_usb_identity*)) {
    g_fd_identity_hook.store(hook, std::memory_order_release);
}
#endif

void
rtl_device_publish_preopened_fd(int sys_fd, int reset_first) {
    const std::lock_guard<std::mutex> guard(g_preopened_usb_fd_lock);
    const int fd = (sys_fd < 0) ? -1 : sys_fd;
    g_preopened_usb_fd.store(fd, std::memory_order_release);
    g_preopened_usb_fd_reset_owed.store((reset_first && fd >= 0) ? fd : -1, std::memory_order_release);
    g_preopened_usb_fd_generation.fetch_add(1U, std::memory_order_acq_rel);
}

void
rtl_device_set_preopened_fd(int sys_fd) {
    rtl_device_publish_preopened_fd(sys_fd, 0);
}

int
rtl_device_preopened_fd_is_set(void) {
    return (g_preopened_usb_fd.load(std::memory_order_acquire) >= 0) ? 1 : 0;
}

int
rtl_device_preopened_fd_in_use(void) {
    return (g_preopened_usb_fd_held.load(std::memory_order_acquire) >= 0) ? 1 : 0;
}

int
rtl_device_preopened_fd_held(void) {
    return g_preopened_usb_fd_held.load(std::memory_order_acquire);
}

int
rtl_device_preopened_fd_reset_owed(void) {
    return g_preopened_usb_fd_reset_owed.load(std::memory_order_acquire);
}

#if (defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD))                                       \
    || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
/* What a device takes from the slot (rtl_preopened_fd_take()). */
namespace {
struct rtl_preopened_take {
    int fd = -1;
    int reset_first = 0;      /* the dongle owes a reset before this open */
    uint32_t generation = 0U; /* the publish it was taken at */
};
} // namespace

/* The slot's descriptor, now marked held, whether it owes a reset, and the publish it came with; fd -1 when none is
 * set. One step with rtl_device_publish_preopened_fd(). */
static rtl_preopened_take
rtl_preopened_fd_take(void) {
    const std::lock_guard<std::mutex> guard(g_preopened_usb_fd_lock);
    rtl_preopened_take take;
    take.fd = g_preopened_usb_fd.load(std::memory_order_acquire);
    if (take.fd >= 0) {
        g_preopened_usb_fd_held.store(take.fd, std::memory_order_release);
        take.reset_first = g_preopened_usb_fd_reset_owed.load(std::memory_order_acquire) == take.fd;
        take.generation = g_preopened_usb_fd_generation.load(std::memory_order_acquire);
    }
    return take;
}

/* How a USB reset through an Android descriptor ended (#782). */
namespace {
enum rtl_usb_fd_reset_result : int {
    RTL_USB_FD_RESET_DONE = 0,    /* reset; the descriptor still leads to the device */
    RTL_USB_FD_RESET_DEAD = 1,    /* the descriptor no longer leads to a device: it re-enumerates, or is gone */
    RTL_USB_FD_RESET_REFUSED = 2, /* the kernel refused the reset; the device is as it was */
};
} // namespace

/* USBDEVFS_RESET on @p fd: 0 or a negative errno. Never touches a descriptor anywhere else: a host test that installs no
 * seam gets -ENOTSUP. */
static int
rtl_usb_fd_reset_raw(int fd) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (const auto hook = g_fd_reset_hook.load(std::memory_order_acquire)) {
        return hook(fd);
    }
#endif
#if defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD)
    if (ioctl(fd, USBDEVFS_RESET, 0) == 0) {
        return 0;
    }
    return -errno;
#else
    (void)fd;
    return -ENOTSUP;
#endif
}

/* Reset the dongle behind @p fd, with nothing open on it, and log how it went, with errno. A successful reset clears
 * the descriptor's obligation; a refused or fatal one keeps it. @p before says what the reset precedes, for the log. */
static rtl_usb_fd_reset_result
rtl_usb_fd_reset(int fd, const char* before) {
    const int rc = rtl_usb_fd_reset_raw(fd);
    if (rc == 0) {
        (void)rtl_usb_reset_owed_clear(fd);
        LOG_INFO("RTL-SDR: reset the USB device through descriptor %d %s\n", fd, before);
        return RTL_USB_FD_RESET_DONE;
    }
    if (rc == -ENODEV || rc == -EBADF) {
        LOG_ERROR("RTL-SDR: the USB reset through descriptor %d %s left it unusable (errno %d)\n", fd, before, -rc);
        return RTL_USB_FD_RESET_DEAD;
    }
    LOG_WARN("RTL-SDR: the USB reset through descriptor %d %s was refused (errno %d); opening it without one\n", fd,
             before, -rc);
    return RTL_USB_FD_RESET_REFUSED;
}

/* What identifies the dongle behind Android descriptor @p fd (#782): its VID:PID, from the device descriptor usbfs
 * returns at the start of the file, and the strings read on its opened @p handle. The descriptor names the one device,
 * so the strings only confirm it. 0 or a negative errno. */
static int
rtl_usb_fd_identity_read(int fd, rtlsdr_dev_t* handle, struct rtl_usb_identity* out) {
    DSD_MEMSET(out, 0, sizeof *out);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (const auto hook = g_fd_identity_hook.load(std::memory_order_acquire)) {
        (void)handle;
        const int rc = hook(fd, out);
        out->valid = (rc == 0) ? 1 : 0;
        return rc;
    }
#endif
#if defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD)
    uint8_t desc[18];
    const ssize_t n = pread(fd, desc, sizeof desc, 0);
    if (n < 0) {
        return -errno;
    }
    if (n != (ssize_t)sizeof desc || desc[1] != 0x01U) {
        return -EIO;
    }
    out->vid = (uint16_t)(desc[8] | (desc[9] << 8));
    out->pid = (uint16_t)(desc[10] | (desc[11] << 8));
    if (rtlsdr_get_usb_strings(handle, out->manufacturer, out->product, out->serial) != 0) {
        return -EIO;
    }
    out->manufacturer[sizeof out->manufacturer - 1] = '\0';
    out->product[sizeof out->product - 1] = '\0';
    out->serial[sizeof out->serial - 1] = '\0';
    out->valid = 1;
    return 0;
#else
    (void)fd;
    (void)handle;
    return -ENOTSUP;
#endif
}

/* Whether two descriptor identities name the same dongle. */
static int
rtl_usb_fd_identity_same(const struct rtl_usb_identity* a, const struct rtl_usb_identity* b) {
    return a->valid && b->valid && a->vid == b->vid && a->pid == b->pid && strcmp(a->manufacturer, b->manufacturer) == 0
           && strcmp(a->product, b->product) == 0 && strcmp(a->serial, b->serial) == 0;
}
#endif

int
rtl_device_preopened_fd_supported(void) {
    /* Must match the guard on the open path in rtl_device_create() exactly: a build
     * that records a descriptor it cannot open from advertises a USB source that can
     * never work. */
#if defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD)
    return 1;
#elif defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
    return g_open_fd_failure_hook.load(std::memory_order_acquire) != nullptr;
#else
    return 0;
#endif
}

/* rtlsdr_open() by index, guarded on MSVC against the exception a mismatched libusb DLL raises. */
static int
rtl_usb_open_index(rtlsdr_dev_t** out, uint32_t index) {
#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
    __try {
        return rtl_usb_ctl_open(out, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DSD_FPRINTF(
            stderr,
            "ERROR: libusb exception in rtlsdr_open (MSVC/Windows). "
            "Check that the bundled libusb/librtlsdr DLLs match the build and the device driver is installed.\n");
        return -1;
    }
#else
    return rtl_usb_ctl_open(out, index);
#endif
}

/* This receiver will not be reset and reopened when it stalls: say why, once, where its open is logged. */
static void
rtl_usb_recovery_off(struct rtl_device* dev, const char* reason) {
    dev->usb_recovery_on.store(0, std::memory_order_release);
    DSD_SNPRINTF(dev->usb_recovery_off_reason, sizeof dev->usb_recovery_off_reason, "%s", reason);
    dev->identity.valid = 0;
    LOG_WARN("RTL-SDR: stall recovery is off for this receiver: %s\n", reason);
}

#if (defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD))                                       \
    || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
static int
rtl_usb_open_preopened_fd(rtlsdr_dev_t** out, int fd) {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (const auto hook = g_open_fd_failure_hook.load(std::memory_order_acquire)) {
        /* A positive answer opens the installed test dongle as the device behind the descriptor. */
        const int r = hook(fd);
        return r > 0 ? rtl_usb_ctl_open(out, 0U) : r;
    }
#endif
#if defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD)
    return rtlsdr_open_fd(out, fd);
#else
    (void)out;
    (void)fd;
    return -1;
#endif
}
#endif

/* The USB backend's fields of a new @p dev, for @p dev_index feeding @p input_ring. */
static void
rtl_device_init_usb_fields(struct rtl_device* dev, int dev_index, struct input_ring_state* input_ring) {
    dev->dev_index = dev_index;
    dev->input_ring = input_ring;
    dev->thread_started = 0;
    dev->mute = 0;
    dev->mute_byte_phase = 0;
    dev->backend = RTL_BACKEND_USB;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_lock_inited = 0;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->host[0] = '\0';
    dev->port = 0;
    dev->run.store(0);
    dev->agc_mode = 1;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;
}

#if (defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD))                                       \
    || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
/* The descriptor the host published, taken from the slot when this build opens from one (fd -1 otherwise). A native
 * Android build always does; a desktop test build does only while its open seam is installed
 * (rtl_device_preopened_fd_supported()). */
static rtl_preopened_take
rtl_preopened_fd_take_supported(void) {
#if defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD)
    return rtl_preopened_fd_take();
#else
    return rtl_device_preopened_fd_supported() ? rtl_preopened_fd_take() : rtl_preopened_take{};
#endif
}

/* Open @p dev from the descriptor @p taken from the slot: 0, or the open's negative error, reported as the session's
 * failure with the descriptor released (the caller frees @p dev). */
static int
rtl_device_open_preopened(struct rtl_device* dev, const rtl_preopened_take* taken) {
    const int preopened_fd = taken->fd;
    /* Claimed (held) before the wrap, not after: the owner must not be told the
     * descriptor is free while libusb is part way through taking it. libusb's own
     * error text reaches the diagnostics from here on (#782). */
    rtl_usb_log_install();
    /* A dongle a stall wedged streams nothing after a plain reopen; the reset through this descriptor clears it
     * (#782). A refused reset opens without one and keeps the obligation, and one that left the descriptor
     * unusable lets the open below fail with its own error. */
    if (taken->reset_first) {
        (void)rtl_usb_fd_reset(preopened_fd, "before opening it");
    }
    const int r = rtl_usb_open_preopened_fd(&dev->dev, preopened_fd);
    if (r < 0) {
        g_last_usb_open_error.store(r, std::memory_order_release);
        g_preopened_usb_fd_held.store(-1, std::memory_order_release);
        /* The session's failure, as Airspy's open reports its own: the UI words a device failure from it (#782). */
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, r);
        DSD_FPRINTF(stderr, "Failed to open rtlsdr device from descriptor %d.\n", preopened_fd);
        return r;
    }
    /* Opened: an earlier open's failure is not this session's, as Airspy's open clears it. */
    dsd_input_failure_clear();
    dev->owns_preopened_fd = 1;
    dev->preopened_fd = preopened_fd;
    dev->preopened_generation = taken->generation;
    /* The descriptor is the identity (#782): its VID:PID and the strings on the handle, checked again after a
     * stall recovery reopened it. */
    const int id_rc = rtl_usb_fd_identity_read(preopened_fd, dev->dev, &dev->identity);
    if (id_rc == 0) {
        dev->usb_recovery_on.store(1, std::memory_order_release);
    } else {
        char reason[96];
        DSD_SNPRINTF(reason, sizeof reason, "its descriptor's USB identity could not be read (errno %d)", -id_rc);
        rtl_usb_recovery_off(dev, reason);
    }
    return 0;
}
#endif

/* Open @p dev by its index: 0, or the open's negative error, reported as the session's failure (the caller frees
 * @p dev). */
static int
rtl_device_open_by_index(struct rtl_device* dev, int dev_index) {
    /* The USB device behind the index, read before the open (D3); the opened handle is checked against it below. */
    const rtl_usb_id_result identified = rtl_usb_identity_resolve(dev_index, &dev->identity);
    const int r = rtl_usb_open_index(&dev->dev, (uint32_t)dev_index);
    if (r < 0) {
        g_last_usb_open_error.store(r, std::memory_order_release);
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, r);
        DSD_FPRINTF(stderr, "Failed to open rtlsdr device %d.\n", dev_index);
        return r;
    }
    dsd_input_failure_clear();
    const rtl_usb_id_result verified =
        (identified == RTL_USB_ID_OK) ? rtl_usb_identity_verify_handle(dev->dev, &dev->identity) : identified;
    if (verified == RTL_USB_ID_OK) {
        dev->usb_recovery_on.store(1, std::memory_order_release);
    } else {
        rtl_usb_recovery_off(dev, rtl_usb_identity_off_reason(verified));
    }
    return 0;
}

/**
 * @brief Create and initialize an RTL-SDR device.
 *
 * @param dev_index Device index to open.
 * @param input_ring Pointer to input ring for USB data.
 * @return Pointer to rtl_device handle, or NULL on failure.
 */
struct rtl_device*
rtl_device_create(int dev_index, struct input_ring_state* input_ring) {
    rtl_device_clear_open_error();
    if (!input_ring) {
        return NULL;
    }

    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
    rtl_device_init_usb_fields(dev, dev_index, input_ring);

#if (defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD))                                       \
    || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
    /* An injected descriptor means the app already picked and opened the device and
     * libusb discovery is off, so opening by index cannot work. rtlsdr_open_fd() is a
     * project patch on the vendored tree; the configure step proves it is there. */
    const rtl_preopened_take taken = rtl_preopened_fd_take_supported();
    if (taken.fd >= 0) {
        if (rtl_device_open_preopened(dev, &taken) < 0) {
            rtl_device_cleanup_common_state(dev);
            free(dev);
            return NULL;
        }
        return dev;
    }
#endif
    if (rtl_device_open_by_index(dev, dev_index) < 0) {
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }
    return dev;
}

/* The session's first connection to @p host:@p port, its header read: NULL when either fails or is cancelled. */
static struct rtl_tcp_conn*
rtl_tcp_open_first_connection(struct rtl_device* dev, const char* host, int port, uint32_t* tuner_type,
                              uint32_t* gain_count) {
    dsd_socket_t sfd = tcp_connect_host(host, port, dev);
    if (sfd == DSD_INVALID_SOCKET) {
        return nullptr;
    }
    rtl_tcp_prepare_socket(sfd);
    const rtl_tcp_header_result hr =
        rtl_tcp_read_header(sfd, kRtlTcpHeaderMs, rtl_tcp_cancelled, dev, tuner_type, gain_count);
    struct rtl_tcp_conn* conn = (hr == RTL_TCP_HEADER_SUCCESS) ? rtl_tcp_conn_new(sfd) : nullptr;
    if (!conn) {
        if (hr != RTL_TCP_HEADER_SUCCESS && hr != RTL_TCP_HEADER_CANCELLED) {
            DSD_FPRINTF(stderr, "rtl_tcp: %s:%d sent no rtl_tcp header (%s).\n", host, port,
                        rtl_tcp_header_failure_name(hr));
        }
        dsd_socket_close(sfd);
    }
    return conn;
}

struct rtl_device*
rtl_device_create_tcp(const char* host, int port, struct input_ring_state* input_ring, int autotune_enabled) {
    if (!input_ring || !host || port <= 0) {
        return NULL;
    }
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
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
    dev->backend = RTL_BACKEND_TCP;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_lock_inited = 0;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    DSD_SNPRINTF(dev->host, sizeof(dev->host), "%s", host);
    dev->port = port;
    dev->run.store(0);
    /* Before any gain change takes, the record holds the session's default: automatic gain with the digital AGC as
       configured, which a reconnect asserts if a failed change may have moved the server off it. */
    dev->agc_mode = 1;
    dev->gain = RTL_AUTO_GAIN;
    dev->digital_agc_wanted = env_agc_want();
    dev->tcp_autotune = autotune_enabled ? 1 : 0;
    dev->offset_took = 0;
    dev->tcp_pending = NULL;
    dev->tcp_pending_len = 0;
    dev->tcp_pending_cap = 0;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;

    /* The first connect and header read run before any reader (run is 0); an exit request cancels them. */
    uint32_t tuner_type = 0U;
    uint32_t gain_count = 0U;
    struct rtl_tcp_conn* conn = rtl_tcp_open_first_connection(dev, host, port, &tuner_type, &gain_count);
    if (!conn) {
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }
    conn->generation = ++dev->tcp_generation;
    dev->tcp_conn = conn;
    dev->tcp_tuner_type.store((int)tuner_type, std::memory_order_release);
    dev->tcp_connected.store(1, std::memory_order_release);
    DSD_FPRINTF(stderr, "rtl_tcp: Connected to %s:%d (tuner %s, %u gains)\n", host, port,
                rtl_tuner_type_name((int)tuner_type), (unsigned)gain_count);
    /* Optional TCP stats: enable with DSD_NEO_TCP_STATS=1 */
    {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->tcp_stats_enable) {
            dev->stats_enabled = 1;
            dev->stats_last_ns = dsd_realtime_mono_ns();
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
rtl_device_alloc_soapy_base(struct input_ring_state* input_ring) {
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return NULL;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
    dev->dev = NULL;
    dev->dev_index = -1;
    dev->input_ring = input_ring;
    dev->backend = RTL_BACKEND_SOAPY;
    dev->soapy_format = SOAPY_FMT_NONE;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->host[0] = '\0';
    dev->run.store(0);
    dev->agc_mode = 1;
    return dev;
}

struct rtl_device*
rtl_device_create_airspy(const dsd_airspy_config* config, struct input_ring_state* ring) {
    auto* dev = static_cast<rtl_device*>(calloc(1, sizeof(rtl_device)));
    if (!dev) {
        return nullptr;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
    dev->backend = RTL_BACKEND_AIRSPY;
    dev->input_ring = ring;
    dev->airspy = airspy_source_open(config, airspy_receive, dev);
    if (!dev->airspy) {
        rtl_device_destroy(dev);
        return nullptr;
    }
    dsd_airspy_info info{};
    (void)airspy_source_info(dev->airspy, &info);
    dev->rate = info.sample_rate;
    return dev;
}

int
rtl_device_airspy_info(struct rtl_device* dev, dsd_airspy_info* info) {
    return dev ? airspy_source_info(dev->airspy, info) : -1;
}

int
rtl_device_airspy_controls(struct rtl_device* dev, const dsd_airspy_config* config) {
    return dev ? airspy_source_controls(dev->airspy, config) : -1;
}

int
rtl_device_airspy_list(uint64_t* serials, int capacity) {
    return airspy_source_list(serials, capacity);
}

int
rtl_device_airspy_set_preopened_fd(int fd) {
    return airspy_source_set_fd(fd);
}

int
rtl_device_airspy_preopened_fd_in_use(void) {
    return airspy_source_fd_in_use();
}

int
rtl_device_airspy_preopened_fd_held(void) {
    return airspy_source_fd_held();
}

int
rtl_device_airspy_last_open_error(void) {
    return airspy_source_last_open_error();
}

void
rtl_device_airspy_clear_open_error(void) {
    airspy_source_clear_open_error();
}

struct rtl_device*
rtl_device_create_soapy(const char* soapy_args, struct input_ring_state* input_ring) {
    if (!input_ring) {
        return NULL;
    }
    struct rtl_device* dev = rtl_device_alloc_soapy_base(input_ring);
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
    dev->offset_took = 0;
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
rtl_device_store_soapy_config_request(struct rtl_device* dev, const struct rtl_soapy_config* cfg) {
    if (!dev) {
        return;
    }
    const char* profile = cfg ? cfg->profile : NULL;
    const char* antenna = cfg ? cfg->antenna : NULL;
    const char* clock_source = cfg ? cfg->clock_source : NULL;
    const char* settings = cfg ? cfg->settings : NULL;
    const char* gains = cfg ? cfg->gains : NULL;
    const char* stream_format = cfg ? cfg->stream_format : NULL;

    rtl_device_copy_cstr(dev->soapy_requested_profile, sizeof(dev->soapy_requested_profile), profile);
    rtl_device_copy_cstr(dev->soapy_requested_antenna, sizeof(dev->soapy_requested_antenna), antenna);
    rtl_device_copy_cstr(dev->soapy_requested_clock, sizeof(dev->soapy_requested_clock), clock_source);
    rtl_device_copy_cstr(dev->soapy_requested_settings, sizeof(dev->soapy_requested_settings), settings);
    rtl_device_copy_cstr(dev->soapy_requested_gains, sizeof(dev->soapy_requested_gains), gains);
    rtl_device_copy_cstr(dev->soapy_requested_stream_format, sizeof(dev->soapy_requested_stream_format), stream_format);
    dev->soapy_requested_bandwidth_hz = cfg ? cfg->bandwidth_hz : -1;
    dev->soapy_startup_center_freq_hz = cfg ? cfg->center_freq_hz : 0U;
    /* Driver settings (for example the SDDC ADC clock) can move the rate grid. */
    dev->soapy_nearest_rate_cache_requested = 0U;
    dev->soapy_nearest_rate_cache_actual = 0U;
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
    if (!dev || dev->backend != RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    rtl_device_store_soapy_config_request(dev, config);
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
    dev->backend = RTL_BACKEND_IQ_REPLAY;
    dev->soapy_dev = NULL;
    dev->soapy_stream = NULL;
    dev->soapy_lock_inited = 0;
    dev->soapy_format = SOAPY_FMT_NONE;
    dev->soapy_mtu_elems = 0;
    dev->rot_phase = 0;
    rtl_capture_u8_byte_carry_clear(&dev->iq_byte_carry);
    dev->host[0] = '\0';
    dev->port = 0;
    dev->run.store(0, std::memory_order_relaxed);
    dev->agc_mode = 0;
    dev->offset_took = cfg->offset_tuning_enabled ? 1 : 0;
    dev->testmode_on = 0;
    dev->rtl_xtal_hz = 0;
    dev->tuner_xtal_hz = 0;
    dev->if_gain_count = 0;
    dev->replay_fs4_shift_enabled = cfg->fs4_shift_enabled ? 1 : 0;
    dev->replay_historical_cu8_two_pass = cfg->historical_cu8_two_pass ? 1 : 0;
    if (eof_state) {
        dev->replay_eof = *eof_state;
        dev->replay_has_eof_state = 1;
    }
}

/* Make the resampler a replay planned from @p capture_hz to @p dsp_in_hz needs (issue #701): none at equal rates. Its
 * protected band is 0.45 of the final DSP rate, the demod rate before post_downsample. */
static int
replay_open_resampler(struct rtl_device* dev, uint32_t capture_hz, uint32_t dsp_in_hz,
                      const dsd_iq_replay_config* cfg) {
    if (dsp_in_hz == capture_hz) {
        return 0;
    }
    const uint64_t final_dsp_hz =
        (uint64_t)cfg->demod_rate_hz * (cfg->post_downsample > 0U ? cfg->post_downsample : 1U);
    if (capture_hz > INT_MAX || dsp_in_hz > INT_MAX || final_dsp_hz == 0U || final_dsp_hz > INT_MAX) {
        LOG_ERROR("IQ replay: cannot resample %u Hz to %u Hz.\n", (unsigned)capture_hz, (unsigned)dsp_in_hz);
        return -1;
    }
    dsd_iq_resampler* r = static_cast<dsd_iq_resampler*>(malloc(sizeof(dsd_iq_resampler)));
    if (!r) {
        LOG_ERROR("IQ replay: no memory for the resampler.\n");
        return -1;
    }
    dsd_iq_resampler_init(r);
    const int protect_hz = (int)(0.45 * (double)final_dsp_hz);
    const size_t max_in = dsd_iq_replay_chunk_bytes_for(cfg->format, capture_hz, dsp_in_hz)
                          / dsd_iq_sample_format_alignment_bytes(cfg->format);
    const int rc =
        dsd_iq_resampler_configure(r, (int)capture_hz, (int)dsp_in_hz, protect_hz, max_in > 0U ? max_in : 1U);
    if (rc != DSD_IQ_RESAMPLER_OK) {
        LOG_ERROR("IQ replay: cannot resample %u Hz to %u Hz: %s.\n", (unsigned)capture_hz, (unsigned)dsp_in_hz,
                  dsd_iq_resampler_status_text(rc));
        free(r);
        return -1;
    }
    LOG_INFO("IQ replay: resampling the %u Hz capture to %u Hz (L/M=%d/%d, %+.2f ppm, %d taps per phase, group delay "
             "%.2f ms).\n",
             (unsigned)capture_hz, (unsigned)dsp_in_hz, r->L, r->M, dsd_iq_resampler_ratio_error_ppm(r),
             r->taps_per_phase, 1000.0 * dsd_iq_resampler_group_delay_seconds(r));
    dev->replay_resampler = r;
    return 0;
}

int
rtl_device_replay_resample_info(const struct rtl_device* dev, uint32_t* out_capture_hz, int* out_l, int* out_m,
                                double* out_ppm) {
    if (!dev || dev->backend != RTL_BACKEND_IQ_REPLAY || !dev->replay_resampler) {
        return 0;
    }
    if (out_capture_hz) {
        *out_capture_hz = dev->replay_cfg.sample_rate_hz;
    }
    if (out_l) {
        *out_l = dev->replay_resampler->L;
    }
    if (out_m) {
        *out_m = dev->replay_resampler->M;
    }
    if (out_ppm) {
        *out_ppm = dsd_iq_resampler_ratio_error_ppm(dev->replay_resampler);
    }
    return 1;
}

static void
replay_free_resampler(struct rtl_device* dev) {
    if (dev && dev->replay_resampler) {
        dsd_iq_resampler_free(dev->replay_resampler);
        free(dev->replay_resampler);
        dev->replay_resampler = nullptr;
    }
}

int
rtl_device_replay_checkpoint(const struct rtl_device* dev, dsd_iq_replay_checkpoint* out) {
    if (!dev || !out || dev->backend != RTL_BACKEND_IQ_REPLAY || dev->thread_started) {
        return -1;
    }
    *out = dev->replay_committed;
    return 0;
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
    /* The rate chain is the caller's plan for the mode it runs (dsd_iq_replay_plan_rate(), issue #701), not the one
       the file records. */
    if (opened_cfg.sample_rate_hz != cfg->sample_rate_hz
        || replay_open_resampler(dev, cfg->sample_rate_hz, dsd_iq_replay_dsp_input_rate_hz(cfg), cfg) != 0) {
        if (opened_cfg.sample_rate_hz != cfg->sample_rate_hz) {
            LOG_ERROR("IQ replay: '%s' now records %u Hz where it was planned at %u Hz.\n", open_path,
                      (unsigned)opened_cfg.sample_rate_hz, (unsigned)cfg->sample_rate_hz);
        }
        dsd_iq_replay_close(replay_src);
        dsd_iq_replay_config_clear(&opened_cfg);
        return -1;
    }
    opened_cfg.dsp_input_rate_hz = dsd_iq_replay_dsp_input_rate_hz(cfg);
    opened_cfg.base_decimation = cfg->base_decimation;
    opened_cfg.post_downsample = cfg->post_downsample;
    opened_cfg.demod_rate_hz = cfg->demod_rate_hz;
    /* A loop rewinds to the capture's opening centre and rate, wherever the replay opened. */
    dev->replay_initial_center_frequency_hz = opened_cfg.center_frequency_hz;
    dev->replay_initial_capture_center_frequency_hz = opened_cfg.capture_center_frequency_hz;
    dev->replay_initial_sample_rate_hz = opened_cfg.sample_rate_hz;
    dev->replay_initial_freq = (opened_cfg.capture_center_frequency_hz > UINT32_MAX)
                                   ? UINT32_MAX
                                   : (uint32_t)opened_cfg.capture_center_frequency_hz;
    dev->replay_initial_rate = opened_cfg.dsp_input_rate_hz;
    DSD_MEMSET(&dev->replay_committed, 0, sizeof(dev->replay_committed));
    if (cfg->resume) {
        /* A run-time re-plan's resume (issue #701): the reader starts at the checkpoint, on the centre the caller found
           in force there (dsd_iq_replay_resume_check()). */
        rc = dsd_iq_replay_seek(replay_src, cfg->resume_at.data_offset);
        if (rc != DSD_IQ_OK) {
            LOG_ERROR("IQ replay: cannot resume '%s' at capture byte %llu (rc=%d).\n", open_path,
                      (unsigned long long)cfg->resume_at.data_offset, rc);
            replay_free_resampler(dev);
            dsd_iq_replay_close(replay_src);
            dsd_iq_replay_config_clear(&opened_cfg);
            return -1;
        }
        opened_cfg.resume = 1;
        opened_cfg.resume_at = cfg->resume_at;
        opened_cfg.center_frequency_hz = cfg->center_frequency_hz;
        opened_cfg.capture_center_frequency_hz = cfg->capture_center_frequency_hz;
        dev->replay_committed = cfg->resume_at;
    }
    dev->replay_cfg = opened_cfg;
    dev->replay_src = replay_src;
    dev->freq = (opened_cfg.capture_center_frequency_hz > UINT32_MAX)
                    ? UINT32_MAX
                    : (uint32_t)opened_cfg.capture_center_frequency_hz;
    dev->rate = opened_cfg.dsp_input_rate_hz;
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
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
    rtl_device_init_iq_replay_backend_state(dev, cfg, input_ring, eof_state);
    if (rtl_device_open_iq_replay_source(dev, cfg) != 0) {
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return NULL;
    }
    return dev;
}

static void
rtl_device_signal_replay_forced_stop(struct rtl_device* dev) {
    if (!dev || dev->backend != RTL_BACKEND_IQ_REPLAY) {
        return;
    }
    if (dev->replay_has_eof_state && dev->replay_eof.replay_forced_stop) {
        dev->replay_eof.replay_forced_stop->store(1, std::memory_order_release);
    }
    replay_device_wake_all(dev);
}

static void
rtl_device_stop_streaming_backend_thread(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    dev->run.store(0);
    rtl_device_signal_replay_forced_stop(dev);
    rtl_monitor_wake(dev);
}

static void
rtl_device_request_thread_stop(struct rtl_device* dev) {
    if (!dev || !dev->thread_started) {
        return;
    }
    if (dev->backend == RTL_BACKEND_AIRSPY || dev->backend == RTL_BACKEND_TCP || dev->backend == RTL_BACKEND_SOAPY
        || dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rtl_device_stop_streaming_backend_thread(dev);
    }
}

/* Unpublish the USB handle, under usb_ctrl_lock (the caller holds usb_lifecycle_lock): no control or cancel reaches it
 * after this, and the caller, its only owner now, closes it outside the lock. */
static rtlsdr_dev_t*
rtl_usb_retire_handle(struct rtl_device* dev) {
    rtl_usb_ctrl_lock(dev);
    rtlsdr_dev_t* handle = dev->dev;
    dev->dev = nullptr;
    rtl_usb_ctrl_unlock(dev);
    return handle;
}

/* Close a retired USB handle for good: bias tee off and FIFO reset first, then the handle, and only then is an
 * Android descriptor handed back to its owner. A stall recovery that failed left no handle, and the descriptor is
 * handed back all the same (#782). */
static void
rtl_usb_close_retired(struct rtl_device* dev, rtlsdr_dev_t* handle) {
    if (handle) {
#ifdef USE_RTLSDR_BIAS_TEE
        (void)rtl_usb_ctl_set_bias_tee(handle, 0);
#endif
        (void)rtl_usb_ctl_reset_buffer(handle);
        (void)rtl_usb_ctl_close(handle);
    }
    if (dev->owns_preopened_fd) {
        /* Released only now that libusb is done with it: the owner polls this to
         * decide when closing the underlying connection is safe. */
        dev->owns_preopened_fd = 0;
        g_preopened_usb_fd_held.store(-1, std::memory_order_release);
    }
}

static void
rtl_device_close_backend_resources(struct rtl_device* dev) {
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        airspy_source_close(dev->airspy);
        dev->airspy = nullptr;
    }
    if (!dev) {
        return;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        /* The reader is joined: no hand-off is in flight. Senders still holding the connection close it last. */
        rtl_tcp_lock(dev);
        struct rtl_tcp_conn* published = rtl_tcp_unpublish_locked(dev);
        struct rtl_tcp_conn* handed = dev->tcp_handoff_conn;
        dev->tcp_handoff_conn = nullptr;
        rtl_tcp_unlock(dev);
        rtl_tcp_conn_unref(published);
        rtl_tcp_conn_unref(handed);
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
        /* The reader is joined by now: nothing else uses the resampler. */
        replay_free_resampler(dev);
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
    if (dev->backend == RTL_BACKEND_USB) {
        /* Latched first, whatever runs: a recovery in progress gives up at its next step, and it is waited for. */
        dev->stop_requested.store(1, std::memory_order_release);
        rtl_usb_lifecycle_lock(dev);
        rtl_usb_reader_stop(dev);
        rtl_usb_close_retired(dev, rtl_usb_retire_handle(dev));
        rtl_usb_lifecycle_unlock(dev);
    } else if (dev->backend == RTL_BACKEND_TCP) {
        rtl_tcp_stop(dev);
        if (dev->thread_started) {
            dev->run.store(0);
            dsd_thread_join(dev->thread);
            dev->thread_started = 0;
        }
    } else if (dev->thread_started) {
        rtl_device_request_thread_stop(dev);
        rtl_progress_monitor_stop(dev);
        dsd_thread_join(dev->thread);
        dev->thread_started = 0;
    }
    rtl_device_close_backend_resources(dev);
    rtl_device_release_tcp_pending(dev);
    rtl_device_cleanup_common_state(dev);

    free(dev);
}

/* ---- USB controls ----
 * Each runs under usb_ctrl_lock against the published handle (rtl_usb_control_begin(): RTL_DEVICE_EBUSY, nothing
 * changed, while a recovery swaps it) and records in the effective record what the dongle took, which a recovery
 * replays. */

/* Note IF gain @p gain for @p stage in a list of at most 16 stages. */
static void
rtl_if_gain_upsert(struct rtl_if_gain* list, int* count, int stage, int gain) {
    for (int i = 0; i < *count && i < 16; i++) {
        if (list[i].stage == stage) {
            list[i].gain = gain;
            return;
        }
    }
    if (*count < 16) {
        list[*count].stage = stage;
        list[*count].gain = gain;
        (*count)++;
    }
}

static int
rtl_usb_set_frequency(struct rtl_device* dev, uint32_t frequency) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    rc = verbose_set_frequency(dev->dev, frequency);
    if (rc == 0) {
        dev->freq = frequency;
        dev->usb_eff.freq = frequency;
        dev->usb_eff.freq_took = 1;
    }
    rtl_usb_control_end(dev);
    if (rc == 0) {
        rtl_reset_capture_state_on_stream_boundary(dev);
    }
    return rc;
}

static int
rtl_usb_set_sample_rate(struct rtl_device* dev, uint32_t samp_rate) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    uint32_t applied_rate = samp_rate;
    rc = verbose_set_sample_rate(dev->dev, samp_rate, &applied_rate);
    if (rc == 0) {
        dev->rate = applied_rate;
        dev->usb_eff.rate = samp_rate;
        dev->usb_eff.rate_took = 1;
    }
    rtl_usb_control_end(dev);
    return rc;
}

static int
rtl_usb_get_sample_rate(struct rtl_device* dev) {
    rtl_usb_ctrl_lock(dev);
    int rate = -1;
    if (dev->dev) {
        const uint32_t actual = rtl_usb_ctl_get_sample_rate(dev->dev);
        if (actual > 0U) {
            dev->rate = actual;
        }
        rate = (int)actual;
    }
    rtl_usb_ctrl_unlock(dev);
    return rate;
}

static int rtl_device_set_gain_usb(struct rtl_device* dev, int gain, int* out_applied_gain);
static int rtl_device_set_gain_nearest_usb(struct rtl_device* dev, int target_tenth_db, int* out_selected_tenth_db);

static int
rtl_usb_set_gain(struct rtl_device* dev, int gain) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    int applied_gain = gain;
    rc = rtl_device_set_gain_usb(dev, gain, &applied_gain);
    rtl_usb_control_end(dev);
    return rc;
}

static int
rtl_usb_set_gain_nearest(struct rtl_device* dev, int target_tenth_db, int* out_selected_tenth_db) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    rc = rtl_device_set_gain_nearest_usb(dev, target_tenth_db, out_selected_tenth_db);
    rtl_usb_control_end(dev);
    return rc;
}

static int
rtl_usb_get_tuner_gain(struct rtl_device* dev, int* out_tenth_db) {
    rtl_usb_ctrl_lock(dev);
    /* librtlsdr's gain tables reach below zero (E4K -10, FC0012/FC0013 -99 tenths): the value is the reading, and only
       a missing handle (none, or retired by a recovery) or a gain the last failed change left unknown is an error. */
    int rc = -1;
    if (dev->dev && !dev->gain_state_unknown) {
        *out_tenth_db = rtl_usb_ctl_get_tuner_gain(dev->dev);
        rc = 0;
    }
    rtl_usb_ctrl_unlock(dev);
    return rc;
}

static int
rtl_usb_set_ppm(struct rtl_device* dev, int ppm_error) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    /* Cache only the last correction the dongle accepted, so a retry of a failed value still reaches it. */
    if (ppm_error != dev->ppm_error) {
        rc = verbose_ppm_set(dev->dev, ppm_error);
        if (rc == 0) {
            dev->ppm_error = ppm_error;
            dev->usb_eff.ppm = ppm_error;
            dev->usb_eff.ppm_took = 1;
        }
    }
    rtl_usb_control_end(dev);
    return rc;
}

static int
rtl_usb_set_direct_sampling(struct rtl_device* dev, int on) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    rc = verbose_direct_sampling(dev->dev, on);
    if (rc == 0) {
        dev->direct_sampling = on;
        dev->usb_eff.direct_sampling = on;
        dev->usb_eff.direct_sampling_took = 1;
    }
    rtl_usb_control_end(dev);
    return rc;
}

/* Offset tuning counts as taken only once librtlsdr reports an LO offset: librtlsdr computes the offset from the
 * sample rate the dongle holds and accepts the call on a handle with no rate yet, displacing nothing (B11). */
static int
rtl_usb_set_offset_tuning(struct rtl_device* dev, int on) {
    int r = rtl_usb_control_begin(dev);
    if (r != 0) {
        return r;
    }
    rtl_usb_int_apply_ctx ctx{dev->dev, on ? 1 : 0};
    int attempts = 0;
    r = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_offset_tuning, NULL, &ctx, &attempts);
    /* A later filter or retune operation can fail after librtlsdr changed the LO offset. The capture geometry
     * must follow that actual offset even when the full request failed. */
#if defined(USE_RTLSDR) || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
    const int actual_offset = rtl_usb_ctl_get_offset_tuning(dev->dev);
    if (actual_offset >= 0) {
        dev->offset_took = actual_offset == 1 ? 1 : 0;
        dev->usb_eff.offset_took = dev->offset_took;
    }
#endif
    if (r == 0) {
        rtl_usb_log_retry_attempts("offset tuning apply", attempts);
        const int took = dev->offset_took;
        if (took) {
            DSD_FPRINTF(stderr, "Offset tuning mode enabled.\n");
        } else if (on) {
            DSD_FPRINTF(stderr, "WARNING: Offset tuning was accepted without an LO offset; the capture stays Fs/4 "
                                "shifted.\n");
        } else {
            DSD_FPRINTF(stderr, "Offset tuning mode disabled.\n");
        }
        dev->usb_eff.offset_requested = on ? 1 : 0;
    } else {
        const int tuner_type = rtl_usb_ctl_get_tuner_type(dev->dev);
        DSD_FPRINTF(stderr, "WARNING: Failed to set offset tuning (%d) for tuner %s.\n", r,
                    rtl_tuner_type_name(tuner_type));
    }
    rtl_usb_control_end(dev);
    return r;
}

static int
rtl_usb_set_tuner_bandwidth(struct rtl_device* dev, uint32_t bw_hz) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    rc = verbose_set_tuner_bandwidth(dev->dev, bw_hz);
    if (rc == 0) {
        dev->tuner_bw_hz = bw_hz;
        dev->usb_eff.tuner_bw = bw_hz;
        dev->usb_eff.tuner_bw_took = 1;
    }
    rtl_usb_control_end(dev);
    return rc;
}

static int
rtl_usb_reset_buffer(struct rtl_device* dev) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    rc = verbose_reset_buffer(dev->dev);
    rtl_usb_control_end(dev);
    return rc;
}

static int
rtl_usb_set_bias_tee(struct rtl_device* dev, int requested) {
#ifdef USE_RTLSDR_BIAS_TEE
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    rtl_usb_int_apply_ctx ctx{dev->dev, requested};
    int attempts = 0;
    rc = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_bias_tee, NULL, &ctx, &attempts);
    if (rc == 0) {
        dev->bias_tee_on = requested;
        dev->usb_eff.bias_tee = requested;
        dev->usb_eff.bias_tee_took = 1;
    }
    rtl_usb_control_end(dev);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to %sable RTL-SDR bias tee.\n", requested ? "en" : "dis");
        return -1;
    }
    rtl_usb_log_retry_attempts("bias tee apply", attempts);
    DSD_FPRINTF(stderr, "RTL-SDR bias tee %s.\n", requested ? "enabled" : "disabled");
    return 0;
#else
    (void)dev;
    (void)requested;
    DSD_FPRINTF(stderr, "NOTE: librtlsdr built without bias tee API; ignoring bias setting on USB.\n");
    return DSD_ERR_NOT_SUPPORTED;
#endif
}

static int
rtl_usb_set_xtal_freq(struct rtl_device* dev, uint32_t rtl_xtal_hz, uint32_t tuner_xtal_hz) {
    int r = rtl_usb_control_begin(dev);
    if (r != 0) {
        return r;
    }
    r = rtl_usb_ctl_set_xtal_freq(dev->dev, rtl_xtal_hz, tuner_xtal_hz);
    if (r == 0) {
        if (rtl_xtal_hz != 0U) {
            dev->rtl_xtal_hz = rtl_xtal_hz;
        }
        if (tuner_xtal_hz != 0U) {
            dev->tuner_xtal_hz = tuner_xtal_hz;
        }
        dev->usb_eff.rtl_xtal = dev->rtl_xtal_hz;
        dev->usb_eff.tuner_xtal = dev->tuner_xtal_hz;
        dev->usb_eff.xtal_took = 1;
    }
    rtl_usb_control_end(dev);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set xtal freq (rtl=%u, tuner=%u).\n", rtl_xtal_hz, tuner_xtal_hz);
        return -1;
    }
    DSD_FPRINTF(stderr, "Set xtal freq: rtl=%u Hz%s, tuner=%u Hz%s.\n", rtl_xtal_hz, rtl_xtal_hz ? "" : " (unchanged)",
                tuner_xtal_hz, tuner_xtal_hz ? "" : " (unchanged)");
    return 0;
}

/* Test mode shares register 0x19 with the digital AGC: turning it off (or asserting it off) puts the digital AGC back
 * as wanted rather than clearing it. */
static int
rtl_usb_set_testmode(struct rtl_device* dev, int requested) {
    int rc = rtl_usb_control_begin(dev);
    if (rc != 0) {
        return rc;
    }
    const int previous = dev->testmode_on;
    dev->testmode_on = requested;
    int attempts = 0;
    rc = rtl_usb_apply_with_runtime_retry(rtl_usb_apply_register19_once, NULL, dev, &attempts);
    if (rc == 0) {
        dev->usb_eff.testmode = requested;
        dev->usb_eff.testmode_took = 1;
    } else {
        dev->testmode_on = previous;
    }
    rtl_usb_control_end(dev);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to %s RTL-SDR test mode.\n", requested ? "enable" : "disable");
        return -1;
    }
    rtl_usb_log_retry_attempts("test mode apply", attempts);
    DSD_FPRINTF(stderr, "RTL-SDR test mode %s.\n", requested ? "enabled" : "disabled");
    return 0;
}

static int
rtl_usb_set_if_gain(struct rtl_device* dev, int stage, int gain_tenth_db) {
    int r = rtl_usb_control_begin(dev);
    if (r != 0) {
        return r;
    }
    r = rtl_usb_ctl_set_tuner_if_gain(dev->dev, stage, gain_tenth_db);
    if (r == 0) {
        rtl_if_gain_upsert(dev->if_gains, &dev->if_gain_count, stage, gain_tenth_db);
        rtl_if_gain_upsert(dev->usb_eff.if_gains, &dev->usb_eff.if_gain_count, stage, gain_tenth_db);
    }
    rtl_usb_control_end(dev);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set IF gain: stage=%d, gain=%d (0.1 dB).\n", stage, gain_tenth_db);
        return -1;
    }
    DSD_FPRINTF(stderr, "IF gain set: stage=%d, gain=%0.1f dB.\n", stage, gain_tenth_db / 10.0);
    return 0;
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
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_set_frequency(dev, frequency);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        const int rc = rtl_tcp_command(dev, 0x01, frequency, [&]() {
            dev->freq = frequency;
            dev->tcp_took.freq = 1;
        });
        if (rc == 0) {
            dev->tcp_restart_pending.store(1, std::memory_order_release);
            /* A retune restarts the throughput watchdog's grace, as the rate write each hop made always did; a hop
               that keeps the rate sends no rate command now (issue #782 D3). */
            rtl_tcp_lock(dev);
            const uint32_t rate = dev->rate;
            rtl_tcp_unlock(dev);
            rtl_tcp_metrics_reset_device(dev, rate);
        }
        return rc;
    }
    int rc = -1;
    if (dev->backend == RTL_BACKEND_AIRSPY) {
        rc = airspy_source_frequency(dev->airspy, frequency);
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        if (dev->replay_cfg.capture_center_frequency_hz > 0
            && (uint64_t)frequency != dev->replay_cfg.capture_center_frequency_hz) {
            DSD_FPRINTF(stderr, "IQ replay: ignoring retune request to %u Hz (capture center is %llu Hz).\n", frequency,
                        (unsigned long long)dev->replay_cfg.capture_center_frequency_hz);
        }
        rc = 0;
    } else {
#ifdef USE_SOAPYSDR
        if (dev->backend == RTL_BACKEND_SOAPY) {
            rc = soapy_call_locked(dev, "setFrequency", [&]() -> int {
                dev->soapy_dev->setFrequency(SOAPY_SDR_RX, 0, (double)frequency);
                return 0;
            });
        }
#endif
    }
    if (rc == 0) {
        dev->freq = frequency;
    }
    return rc;
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
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        int rc = airspy_source_rate(dev->airspy, samp_rate);
        if (!rc) {
            dev->rate = samp_rate;
        }
        return rc;
    }
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_set_sample_rate(dev, samp_rate);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        const int rc = rtl_tcp_command(dev, 0x02, samp_rate, [&]() {
            dev->rate = samp_rate;
            dev->tcp_took.rate = 1;
        });
        if (rc == 0) {
            /* The TCP device is created before the final stream rate is known.
             * Reset the metric windows when the rate is programmed so startup
             * delay cannot count against the first watchdog interval. */
            rtl_tcp_metrics_reset_device(dev, samp_rate);
        }
        return rc;
    }
    int rc = -1;
    uint32_t applied_rate = samp_rate;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rc = 0;
    } else {
#ifdef USE_SOAPYSDR
        if (dev->backend == RTL_BACKEND_SOAPY) {
            rc = soapy_call_locked(dev, "setSampleRate", [&]() -> int {
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
                    DSD_FPRINTF(stderr, "SoapySDR: adjusted sample rate from %.0f Hz to %.0f Hz.\n", requested,
                                applied);
                }
                dev->soapy_dev->setSampleRate(SOAPY_SDR_RX, 0, applied);
                applied_rate = (uint32_t)std::lround(applied);
                return 0;
            });
        }
#endif
    }
    if (rc == 0) {
        dev->rate = applied_rate;
    }
    return rc;
}

/**
 * @brief Report the rate the backend would deliver for a requested rate.
 *
 * Query only: the device is never reconfigured. Backends with a fixed rate grid (many
 * SoapySDR drivers) answer with the grid entry nearest the request so the rate
 * chain can pick decimation for the rate that will really arrive. The answer is
 * cached per request while the device stays configured, so per-retune callers do
 * not pay a driver round-trip; `rtl_device_store_soapy_config_request` clears the
 * cache because driver settings (for example the SDDC ADC clock) can move the grid.
 */
int
// cppcheck-suppress constParameterPointer -- The SoapySDR build locks dev and fills the cache; the stub build does not.
rtl_device_nearest_supported_rate(struct rtl_device* dev, uint32_t requested, uint32_t* out_actual) {
    if (dev && out_actual && dev->backend == RTL_BACKEND_AIRSPY) {
        *out_actual = dev->rate;
        return 0;
    }
    if (!dev || requested == 0U || !out_actual) {
        return -1;
    }
    *out_actual = requested;
    if (dev->backend != RTL_BACKEND_SOAPY) {
        return 0;
    }
#ifdef USE_SOAPYSDR
    /* The cache is read and refreshed under the Soapy lock so concurrent callers never
       observe a torn requested/actual pair. */
    uint32_t actual = 0U;
    int rc = soapy_call_locked(dev, "listSampleRates", [&]() -> int {
        if (dev->soapy_nearest_rate_cache_requested == requested && dev->soapy_nearest_rate_cache_actual != 0U) {
            actual = dev->soapy_nearest_rate_cache_actual;
            return 0;
        }
        bool adjusted = false;
        std::vector<double> listed = soapy_valid_positive_rates(dev->soapy_dev->listSampleRates(SOAPY_SDR_RX, 0));
        std::vector<dsdneo::SoapyRange> ranges =
            soapy_ranges_from_range_list(dev->soapy_dev->getSampleRateRange(SOAPY_SDR_RX, 0));
        double applied = dsdneo::soapy_nearest_sample_rate((double)requested, listed, ranges, &adjusted);
        if (!(applied > 0.0) || applied > (double)UINT32_MAX) {
            return -1;
        }
        actual = (uint32_t)std::lround(applied);
        dev->soapy_nearest_rate_cache_requested = requested;
        dev->soapy_nearest_rate_cache_actual = actual;
        return 0;
    });
    if (rc != 0 || actual == 0U) {
        return -1;
    }
    *out_actual = actual;
    return 0;
#else
    return -1;
#endif
}

/**
 * @brief Get current device sample rate.
 *
 * For USB, queries librtlsdr for the actual rate applied (which may be
 * quantized). For rtl_tcp, returns the last programmed value.
 */
int
rtl_device_get_sample_rate(struct rtl_device* dev) {
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        return (int)dev->rate;
    }
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_get_sample_rate(dev);
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return (int)dev->replay_cfg.sample_rate_hz;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        rtl_tcp_lock(dev);
        const uint32_t rate = dev->rate;
        rtl_tcp_unlock(dev);
        return (int)rate;
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

int
rtl_device_capture_settings_generation(const struct rtl_device* dev, uint32_t* out_generation) {
    if (!dev || !out_generation) {
        return 0;
    }
    *out_generation = dev->capture_settings_generation.load(std::memory_order_acquire);
    /* SoapySDR keeps every write, whatever its driver. A driver's tune can move what the stream recorded (SoapySDRPlay3's
       LNA-state range follows the band, so the same aggregate gain maps to other stages after a band change), the
       profile matcher cannot tell a reviewed driver from one that merely shares a keyword, and no SoapySDR hardware is
       at hand to check one on: the rewrite main always did is kept. */
    return (dev->backend == RTL_BACKEND_USB || dev->backend == RTL_BACKEND_TCP) ? 1 : 0;
}

/*
 * What each tuner's librtlsdr retune (rtlsdr_set_center_freq() -> tuner->set_freq()) does to the settings a stream may
 * skip (issue #782 gate r3 J1). Checked in the vendored osmocom librtlsdr 2.0.3 (android/third_party/librtlsdr), whose
 * tuner drivers and center-frequency, bandwidth and sample-rate functions are byte-for-byte those of the osmocom
 * release the desktop builds link (vcpkg's port and the distribution packages); the rtl-sdr-blog fork differs only in
 * the R82xx input switching below.
 *
 * - rtlsdr_set_center_freq() calls tuner->set_freq() alone (rtlsdr_set_if_freq() in direct sampling): no rate write,
 *   no bandwidth write. rtlsdr_set_sample_rate() calls tuner->set_bw(bw ? bw : rate) itself, so a rate write always
 *   resets the filter (the stream writes the bandwidth after every rate write); rtlsdr_set_freq_correction() retunes
 *   (a PPM change already forgets the rate and the bandwidth).
 * - E4000: e4k_tune_freq() writes the PLL (SYNTH1/3/4/5/7), the band (BIAS, SYNTH1) and the RF preselect filter (FILT1
 *   low nibble). The IF filters e4000_set_bw() sets (mixer, RC, channel) and the LNA, mixer and IF gains are not
 *   touched. Keeps all three.
 * - FC0012: fc0012_set_params(freq, 6 MHz) writes registers 1-6, 0x0e and GPIO 6; its bandwidth bits are always 6 MHz
 *   and fc0012_set_bw() does nothing, so a bandwidth write changes nothing either way. The LNA gain (0x13) is not
 *   touched. Keeps all three.
 * - FC0013: fc0013_set_params(freq, 6 MHz) writes registers 1-6, 0x07, 0x0e, 0x11, 0x1d and 0x14's band bits, keeping
 *   its gain bits [4:0]; fc0013_set_bw() does nothing; the gain mode (0x0d) is not touched. Keeps all three.
 * - FC2580: fc2580_set_freq() sets the channel filter for the band on every tune (7.8 MHz UHF, 6.8 MHz VHF, 1.53 MHz
 *   L-band) through fc2580_set_filter(); only fc2580_set_bw() (1.53 MHz) puts it back. Its gain controls do nothing.
 *   Resets the bandwidth. An rtl_tcp server takes no bandwidth command from this backend, and there the server's own
 *   rtlsdr_set_sample_rate() is what restores the filter: through rtl_tcp it resets the rate.
 * - R820T and R828D: r82xx_set_freq() writes the mux and tracking filter (0x08, 0x09, 0x10, 0x17, 0x1a, 0x1b), the PLL
 *   (0x10-0x14, 0x1a) and, on an R828D or an RTL-SDR Blog V4/V4L, the input select bits of 0x05 (0x20, 0x40, 0x60) and
 *   0x06 (0x08). The IF filter (0x0a, 0x0b) is set only by r82xx_set_bandwidth() and at init, the gains (0x05 bits
 *   [4:0], 0x07 bits [4:0], 0x0c) only by r82xx_set_gain(). Keeps all three.
 *
 * A tuner this build does not know resets all three, as main always rewrote them.
 */
static unsigned
rtl_capture_retune_resets_for_tuner(int tuner_type, int through_rtl_tcp) {
    switch (tuner_type) {
        case RTLSDR_TUNER_E4000:
        case RTLSDR_TUNER_FC0012:
        case RTLSDR_TUNER_FC0013:
        case RTLSDR_TUNER_R820T:
        case RTLSDR_TUNER_R828D: return 0U;
        case RTLSDR_TUNER_FC2580:
            return through_rtl_tcp ? (unsigned)(RTL_CAPTURE_SETTING_RATE | RTL_CAPTURE_SETTING_BANDWIDTH)
                                   : (unsigned)RTL_CAPTURE_SETTING_BANDWIDTH;
        default: return (unsigned)(RTL_CAPTURE_SETTING_RATE | RTL_CAPTURE_SETTING_BANDWIDTH | RTL_CAPTURE_SETTING_GAIN);
    }
}

unsigned
rtl_device_capture_retune_resets(const struct rtl_device* dev) {
    if (!dev) {
        return 0U;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_capture_retune_resets_for_tuner(dev->tcp_tuner_type.load(std::memory_order_acquire), 1);
    }
    if (dev->backend != RTL_BACKEND_USB) {
        return 0U;
    }
    rtl_usb_ctrl_lock(dev);
    const int tuner_type = dev->dev ? rtl_usb_ctl_get_tuner_type(dev->dev) : RTLSDR_TUNER_UNKNOWN;
    rtl_usb_ctrl_unlock(dev);
    return rtl_capture_retune_resets_for_tuner(tuner_type, 0);
}

static int
rtl_usb_select_gain(rtlsdr_dev_t* dev, int requested, int* selected) {
    const int rc = nearest_gain(dev, requested, selected);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to read the tuner's gain table (rc=%d); gain unchanged.\n", rc);
    }
    return rc;
}

static int
rtl_device_set_gain_usb(struct rtl_device* dev, int gain, int* out_applied_gain) {
    if (!dev || !dev->dev) {
        return -1;
    }
    if (gain == RTL_AUTO_GAIN) {
        int rc = verbose_auto_gain(dev);
        if (rc == 0 && out_applied_gain) {
            *out_applied_gain = RTL_AUTO_GAIN;
        }
        return rc;
    }
    int nearest = 0;
    int rc = rtl_usb_select_gain(dev->dev, gain, &nearest);
    if (rc != 0) {
        return rc;
    }
    rc = verbose_gain_set(dev, nearest);
    if (rc == 0 && out_applied_gain) {
        *out_applied_gain = nearest;
    }
    return rc;
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
    return 1;
}

static int
rtl_device_set_gain_soapy(struct rtl_device* dev, int gain, bool nearest, int* out_applied_gain) {
    int selected = gain;
    int automatic = 0;
    const int rc = soapy_call_locked(dev, "setGain", [&]() -> int {
        if (rtl_device_soapy_named_gain_override_active(dev)) {
            const double actual = dev->soapy_dev->getGain(SOAPY_SDR_RX, 0);
            selected = (int)std::lround(actual * 10.0);
            return 0;
        }
        if (!nearest && gain == RTL_AUTO_GAIN) {
            if (!dev->soapy_dev->hasGainMode(SOAPY_SDR_RX, 0)) {
                return DSD_ERR_NOT_SUPPORTED;
            }
            dev->soapy_dev->setGainMode(SOAPY_SDR_RX, 0, true);
            automatic = 1;
            return 0;
        }
        double gain_db = (double)gain / 10.0;
        if (nearest) {
            const std::vector<std::string> names = dev->soapy_dev->listGains(SOAPY_SDR_RX, 0);
            const SoapySDR::Range range = dev->soapy_dev->getGainRange(SOAPY_SDR_RX, 0);
            if (names.empty() && std::fabs(range.minimum() - range.maximum()) <= 1.0e-9) {
                return DSD_ERR_NOT_SUPPORTED;
            }
            gain_db = std::max(range.minimum(), std::min(gain_db, range.maximum()));
        }
        if (dev->soapy_dev->hasGainMode(SOAPY_SDR_RX, 0)) {
            dev->soapy_dev->setGainMode(SOAPY_SDR_RX, 0, false);
        }
        dev->soapy_dev->setGain(SOAPY_SDR_RX, 0, gain_db);
        /* Drivers may quantize aggregate gain; named stages may preserve a different value entirely. */
        selected = (int)std::lround(dev->soapy_dev->getGain(SOAPY_SDR_RX, 0) * 10.0);
        return 0;
    });
    if (rc == 0) {
        dev->agc_mode = automatic;
        dev->gain = selected;
        *out_applied_gain = selected;
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
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        (void)gain;
        return DSD_ERR_NOT_SUPPORTED; /* Airspy controls use native indices. */
    }
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        /* The gain record took the applied gain under the lock. */
        return rtl_usb_set_gain(dev, gain);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_set_gain(dev, gain == RTL_AUTO_GAIN, gain);
    }
    int rc = -1;
    int applied_gain = gain;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->agc_mode = 0;
        rc = 0;
    } else {
#ifdef USE_SOAPYSDR
        if (dev->backend == RTL_BACKEND_SOAPY) {
            rc = rtl_device_set_gain_soapy(dev, gain, false, &applied_gain);
        }
#endif
    }
    if (rc == 0) {
        dev->gain = applied_gain;
    }
    return rc;
}

static int
rtl_device_set_gain_nearest_usb(struct rtl_device* dev, int target_tenth_db, int* out_selected_tenth_db) {
    if (!dev || !dev->dev) {
        return -1;
    }
    int g = 0;
    int r = rtl_usb_select_gain(dev->dev, target_tenth_db, &g);
    if (r != 0) {
        return r;
    }
    r = verbose_gain_set(dev, g);
    if (r != 0) {
        DSD_FPRINTF(stderr, "WARNING: Failed to set tuner gain (nearest).\n");
        return r;
    }
    *out_selected_tenth_db = g;
    DSD_FPRINTF(stderr, "Tuner manual gain (nearest): %0.1f dB.\n", (double)g / 10.0);
    return 0;
}

/* The server picks the nearest gain itself, so the target goes out as a manual gain change, with the same policy
 * (digital AGC off) and the same record-on-success (B16). */
static int
rtl_device_set_gain_nearest_tcp(struct rtl_device* dev, int target_tenth_db, int* out_selected_tenth_db) {
    const int rc = rtl_tcp_set_gain(dev, 0, target_tenth_db);
    if (rc == 0) {
        *out_selected_tenth_db = target_tenth_db;
    }
    return rc;
}

#ifdef USE_SOAPYSDR
static int
rtl_device_set_gain_nearest_soapy(struct rtl_device* dev, int target_tenth_db, int* out_selected_tenth_db) {
    return rtl_device_set_gain_soapy(dev, target_tenth_db, true, out_selected_tenth_db);
}

#endif

int
rtl_device_set_gain_nearest(struct rtl_device* dev, int target_tenth_db, int* out_selected_tenth_db) {
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        (void)target_tenth_db;
        return DSD_ERR_NOT_SUPPORTED;
    }
    if (!dev) {
        return -1;
    }
    /* The selection may be negative (E4K, FC0012/FC0013), so it travels apart from the status. */
    int selected = target_tenth_db;
    int rc = -1;
    if (dev->backend == RTL_BACKEND_USB) {
        rc = rtl_usb_set_gain_nearest(dev, target_tenth_db, &selected);
    } else if (dev->backend == RTL_BACKEND_TCP) {
        rc = rtl_device_set_gain_nearest_tcp(dev, target_tenth_db, &selected);
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->gain = target_tenth_db;
        rc = 0;
    } else {
#ifdef USE_SOAPYSDR
        if (dev->backend == RTL_BACKEND_SOAPY) {
            rc = rtl_device_set_gain_nearest_soapy(dev, target_tenth_db, &selected);
        }
#endif
    }
    if (rc == 0 && out_selected_tenth_db) {
        *out_selected_tenth_db = selected;
    }
    return rc;
}

#ifdef USE_SOAPYSDR
/* A Soapy device's overall gain in tenths of a dB, or DSD_ERR_NOT_SUPPORTED when it has no gain to report. */
static int
rtl_device_get_tuner_gain_soapy(struct rtl_device* dev, int* out_tenth_db) {
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
    *out_tenth_db = (int)(gain_db * 10.0 + (gain_db >= 0.0 ? 0.5 : -0.5));
    return 0;
}
#endif

int
rtl_device_get_tuner_gain(struct rtl_device* dev, int* out_tenth_db) {
    if (!out_tenth_db) {
        return -1;
    }
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_get_tuner_gain(dev, out_tenth_db);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        rtl_tcp_lock(dev);
        *out_tenth_db = dev->agc_mode ? 0 : dev->gain;
        rtl_tcp_unlock(dev);
        return 0;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        if (dev->replay_cfg.tuner_gain_unknown) {
            return DSD_ERR_NOT_SUPPORTED;
        }
        *out_tenth_db = dev->replay_cfg.tuner_gain_tenth_db;
        return 0;
    }
#ifdef USE_SOAPYSDR
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return rtl_device_get_tuner_gain_soapy(dev, out_tenth_db);
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
        /* The gain field holds the mode the controls last took; none while a failed change left it unknown. Read under
         * the lock, which a gain change holds while the field carries the state it is applying. */
        rtl_usb_ctrl_lock(dev);
        const int is_auto = dev->gain_state_unknown ? -1 : ((dev->gain == RTL_AUTO_GAIN) ? 1 : 0);
        rtl_usb_ctrl_unlock(dev);
        return is_auto;
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    } else if (dev->backend == RTL_BACKEND_TCP) {
        rtl_tcp_lock(dev);
        const int is_auto = dev->agc_mode ? 1 : 0;
        rtl_tcp_unlock(dev);
        return is_auto;
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
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
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_set_ppm(dev, ppm_error);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_command(dev, 0x05, (uint32_t)ppm_error, [&]() {
            dev->ppm_error = ppm_error;
            dev->tcp_took.ppm = 1;
        });
    }
    /* Cache only the last correction that the backend accepted. Retries for a
     * previously failed value must still reach the driver/backend. */
    if (ppm_error == dev->ppm_error) {
        return 0;
    }
    int rc = -1;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rc = 0;
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
 * @brief Report whether the backend can apply a frequency (PPM) correction.
 */
int
// cppcheck-suppress constParameterPointer -- The SoapySDR build locks dev; the stub build does not.
rtl_device_supports_ppm(struct rtl_device* dev) {
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        return 0;
    }
    if (!dev) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
#ifdef USE_SOAPYSDR
        int supported = 0;
        int rc = soapy_call_locked(dev, "hasFrequencyCorrection", [&]() -> int {
            supported = dev->soapy_dev->hasFrequencyCorrection(SOAPY_SDR_RX, 0) ? 1 : 0;
            return 0;
        });
        return (rc == 0) ? supported : 0;
#else
        return 0;
#endif
    }
    return 1;
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
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_set_direct_sampling(dev, on);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_command(dev, 0x09, (uint32_t)on, [&]() {
            dev->direct_sampling = on;
            dev->tcp_took.direct_sampling = 1;
        });
    }
    int rc = -1;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rc = 0;
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        rc = DSD_ERR_NOT_SUPPORTED;
    }
    if (rc == 0) {
        dev->direct_sampling = on;
    }
    return rc;
}

/**
 * @brief Enable or disable offset tuning mode.
 *
 * On USB it counts as taken only once librtlsdr reports an LO offset: librtlsdr computes the offset from the sample
 * rate the dongle holds and accepts the call on a handle with no rate yet, displacing nothing (B11).
 */
int
rtl_device_set_offset_tuning_enabled(struct rtl_device* dev, int on) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_set_offset_tuning(dev, on);
    }
    int r = -1;
    const int took = on ? 1 : 0;
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_command(dev, 0x0A, (uint32_t)took, [&]() {
            dev->offset_took = took;
            dev->tcp_took.offset_tuning = 1;
        });
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        r = 0;
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        r = DSD_ERR_NOT_SUPPORTED;
    }
    if (r == 0) {
        dev->offset_took = took;
    }
    return r;
}

int
rtl_device_get_offset_tuning(const struct rtl_device* dev) {
    if (!dev) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        rtl_tcp_lock(dev);
        const int took = dev->offset_took ? 1 : 0;
        rtl_tcp_unlock(dev);
        return took;
    }
    if (dev->backend != RTL_BACKEND_USB) {
        return dev->offset_took ? 1 : 0;
    }
    rtl_usb_ctrl_lock(dev);
    const int took = dev->offset_took ? 1 : 0;
    rtl_usb_ctrl_unlock(dev);
    return took;
}

uint32_t
rtl_device_get_offset_tuning_hz(const struct rtl_device* dev) {
    if (!dev || dev->backend != RTL_BACKEND_USB) {
        return 0U;
    }
    rtl_usb_ctrl_lock(dev);
    uint32_t offs_hz = 0U;
    if (dev->offset_took && dev->dev) {
        offs_hz = rtl_usb_offs_for_rate(rtl_usb_ctl_get_sample_rate(dev->dev));
    }
    rtl_usb_ctrl_unlock(dev);
    return offs_hz;
}

uint32_t
rtl_offset_aware_bandwidth_hz(uint32_t wanted_hz, int offset_active, uint32_t offs_freq_hz) {
    if (!offset_active || offs_freq_hz == 0U) {
        return wanted_hz;
    }
    /* librtlsdr opens the tuner filter to twice the offset when offset tuning takes, since the channel sits offs_freq
     * from the displaced LO; any narrower filter, the driver's automatic one (the sample rate) included, cuts into it
     * (B14). */
    const uint64_t floor64 = 2ULL * (uint64_t)offs_freq_hz;
    const uint32_t floor_hz = (floor64 > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)floor64;
    if (wanted_hz >= floor_hz) {
        return wanted_hz;
    }
    if (wanted_hz != 0U) {
        /* Once per request, not on every retune. */
        static std::atomic<uint32_t> s_raised_request_hz{0U};
        if (s_raised_request_hz.exchange(wanted_hz, std::memory_order_relaxed) != wanted_hz) {
            LOG_WARN("Tuner bandwidth %u Hz is below the offset tuning floor of %u Hz (twice the LO offset); using "
                     "%u Hz.\n",
                     wanted_hz, floor_hz, floor_hz);
        }
    }
    return floor_hz;
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
        return rtl_usb_set_tuner_bandwidth(dev, bw_hz);
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
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        return 0;
    }
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_reset_buffer(dev);
    } else {
        /* No explicit reset; treat as success */
        return 0;
    }
}

/* Start the Airspy stream and its monitor thread, which also watches its sample progress. Nonzero when either did not
 * start; the stream is stopped again then. */
static int
rtl_airspy_start_stream(struct rtl_device* dev) {
    if (rtl_monitor_sync_init(dev) != 0) {
        return -1;
    }
    dev->receiver_end_claimed.store(0, std::memory_order_relaxed);
    dev->progress_monitor_run.store(1, std::memory_order_relaxed);
    dev->run.store(1, std::memory_order_release);
    int r = airspy_source_start(dev->airspy);
    if (r == 0) {
        r = dsd_thread_create(&dev->thread, airspy_monitor, dev);
    }
    if (r != 0) {
        (void)airspy_source_stop(dev->airspy);
    }
    return r;
}

/* Start the SoapySDR reader thread and its sample-progress monitor, the monitor first: without it a receiver that stops
 * would go unnoticed, so a monitor that cannot start fails the start before the reader sets the stream up. Nonzero
 * when either did not start; a monitor that started is stopped and joined again then. */
static int
soapy_start_reader(struct rtl_device* dev) {
    if (rtl_monitor_sync_init(dev) != 0) {
        return -1;
    }
    dev->reader_entered.store(0, std::memory_order_relaxed);
    dev->reader_exited.store(0, std::memory_order_relaxed);
    dev->receiver_end_claimed.store(0, std::memory_order_relaxed);
    dev->progress_monitor_run.store(1, std::memory_order_relaxed);
    dev->soapy_timeout_count.store(0U, std::memory_order_relaxed);
    dev->soapy_overflow_count.store(0U, std::memory_order_relaxed);
    dev->soapy_read_errors = 0U;
    dev->soapy_last_overflow_log_ns = 0U;
    dev->run.store(1, std::memory_order_release);
    int r = rtl_progress_monitor_create(dev, soapy_progress_monitor);
    if (r != 0) {
        LOG_ERROR("SoapySDR: could not start the sample-progress monitor (rc=%d); the stream does not start\n", r);
        dev->run.store(0, std::memory_order_release);
        return r;
    }
    dev->progress_monitor_started = 1;
#if defined(USE_SOAPYSDR) && defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
    r = g_test_soapy_refuse_reader.load(std::memory_order_relaxed)
            ? -EAGAIN
            : dsd_thread_create(&dev->thread, soapy_thread_fn, dev);
#else
    r = dsd_thread_create(&dev->thread, soapy_thread_fn, dev);
#endif
    if (r != 0) {
        dev->run.store(0, std::memory_order_release);
        rtl_progress_monitor_stop(dev);
    }
    return r;
}

/* Move the incarnation the next USB reader thread finds, skipping 0 (no reader); returns it. */
static uint32_t
rtl_usb_next_reader_incarnation(struct rtl_device* dev) {
    uint32_t next = dev->reader_incarnation.load(std::memory_order_relaxed) + 1U;
    if (next == 0U) {
        next = 1U;
    }
    dev->reader_incarnation.store(next, std::memory_order_release);
    return next;
}

/* Start the RTL-SDR's USB reader thread and its sample-progress monitor. Nonzero when either did not start; a reader
 * that started is stopped and joined again then. */
static int
rtl_usb_start_reader(struct rtl_device* dev) {
    if (!rtl_usb_reader_available(dev) || rtl_monitor_sync_init(dev) != 0) {
        return -1;
    }
    dev->reader_entered.store(0, std::memory_order_relaxed);
    dev->reader_exited.store(0, std::memory_order_relaxed);
    dev->receiver_end_claimed.store(0, std::memory_order_relaxed);
    dev->recovery_handoff.store(0U, std::memory_order_relaxed);
    (void)rtl_usb_next_reader_incarnation(dev);
    dev->progress_monitor_run.store(1, std::memory_order_relaxed);
    /* Cleared by a stop request, before it cancels the reader. */
    dev->run.store(1, std::memory_order_release);
    int r = dsd_thread_create(&dev->thread, dongle_thread_fn, dev);
    if (r != 0) {
        return r;
    }
    r = rtl_usb_progress_monitor_create(dev);
    if (r != 0) {
        /* Without its monitor a stalled receiver would go unnoticed; the start fails. */
        LOG_ERROR("RTL-SDR: could not start the sample-progress monitor (rc=%d); the stream does not start\n", r);
        rtl_usb_reader_stop(dev);
        return r;
    }
    dev->progress_monitor_started = 1;
    return 0;
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
    if (dev && dev->backend == RTL_BACKEND_USB) {
        /* thread_started changes only under the lifecycle lock, which a recovery holds while it swaps the reader. */
        rtl_usb_lifecycle_lock(dev);
        int r = -1;
        if (!dev->thread_started) {
            dev->buf_len = buf_len;
            rtl_reset_capture_state_on_stream_boundary(dev);
            dev->thread_started = 1;
            r = rtl_usb_start_reader(dev);
            if (r != 0) {
                dev->thread_started = 0;
                dev->run.store(0);
            }
        }
        rtl_usb_lifecycle_unlock(dev);
        return (r == 0) ? 0 : -1;
    }
    if (!dev || dev->thread_started) {
        return -1;
    }
    dev->buf_len = buf_len;
    rtl_reset_capture_state_on_stream_boundary(dev);
    dev->thread_started = 1;
    int r = 0;
    if (dev->backend == RTL_BACKEND_AIRSPY) {
        r = rtl_airspy_start_stream(dev);
    } else if (dev->backend == RTL_BACKEND_TCP) {
        dev->run.store(1);
        r = dsd_thread_create(&dev->thread, tcp_thread_fn, dev);
    } else if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        if (!dev->replay_src) {
            dev->thread_started = 0;
            return -1;
        }
        dev->run.store(1, std::memory_order_release);
        r = replay_start_reader_thread(dev);
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        if (!dev->soapy_dev) {
            dev->thread_started = 0;
            return -1;
        }
        r = soapy_start_reader(dev);
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

/* A USB device's stop: latched first, whether or not a reader runs: a recovery in progress gives up at its next step
 * and no reader restarts after it (rtl_device_usb_resume_reader()); the lock waits for it. */
static int
rtl_usb_stop_async(struct rtl_device* dev) {
    dev->stop_requested.store(1, std::memory_order_release);
    rtl_usb_lifecycle_lock(dev);
    const int stopped = (dev->thread_started || dev->progress_monitor_started) ? 1 : 0;
    rtl_usb_reader_stop(dev);
    rtl_usb_lifecycle_unlock(dev);
    if (!stopped) {
        return -1;
    }
    rtl_reset_capture_state_on_stream_boundary(dev);
    return 0;
}

/* Tell another backend's running reader thread to return; -1 for a backend that has none. */
static int
rtl_device_signal_reader_stop(struct rtl_device* dev) {
    if (dev->backend == RTL_BACKEND_TCP) {
        dev->run.store(0);
        return 0;
    }
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        dev->run.store(0, std::memory_order_release);
        if (dev->replay_has_eof_state && dev->replay_eof.replay_forced_stop) {
            dev->replay_eof.replay_forced_stop->store(1, std::memory_order_release);
        }
        replay_device_wake_all(dev);
        return 0;
    }
    if (dev->backend == RTL_BACKEND_SOAPY || dev->backend == RTL_BACKEND_AIRSPY) {
        dev->run.store(0);
        rtl_monitor_wake(dev);
        /* A SoapySDR device's monitor (an Airspy's is its device thread): joined before the device can be freed. */
        rtl_progress_monitor_stop(dev);
        return 0;
    }
    return -1;
}

/**
 * @brief Stop asynchronous reading and join the device thread.
 *
 * @param dev RTL-SDR device handle.
 * @return 0 on success, negative on failure.
 */
int
rtl_device_stop_async(struct rtl_device* dev) {
    if (dev && dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_stop_async(dev);
    }
    if (dev && dev->backend == RTL_BACKEND_TCP) {
        /* Latched and admission closed whether or not a reader runs, before it is joined (rtl_tcp_stop()). */
        rtl_tcp_stop(dev);
    }
    if (!dev || !dev->thread_started) {
        return -1;
    }
    if (rtl_device_signal_reader_stop(dev) != 0) {
        return -1;
    }
    dsd_thread_join(dev->thread);
    dev->thread_started = 0;
    rtl_reset_capture_state_on_stream_boundary(dev);
    return 0;
}

/* ---- USB recovery: reset and reopen a stalled dongle in place (D1, D1a, D3, D6) ----
 * The swap runs under usb_lifecycle_lock from start to finish, so a stop or destroy (which latch stop_requested first)
 * waits for it, and it gives up at its next step once the latch is set. While it runs, recovering makes every setter
 * return RTL_DEVICE_EBUSY untouched, which also keeps the effective record still for the replay. The old handle is
 * unpublished under usb_ctrl_lock and closed outside it; the new one is configured unpublished, by this thread alone,
 * and published only once every control the dongle had taken is back. The reader restarts separately
 * (rtl_device_usb_resume_reader()), so the caller can finish its stream boundary first. */

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
static void rtl_usb_test_reader_restarting(struct rtl_device* dev);
#endif

/* The session may already be ending before its owner reaches stop_async(). */
static int
rtl_usb_session_ending(const struct rtl_device* dev) {
    return dev->stop_requested.load(std::memory_order_acquire) || dsd_exitflag_load()
           || dev->receiver_end_claimed.load(std::memory_order_acquire);
}

/* The cancellation predicate the USB reset polls. */
static int
rtl_usb_stop_latched(void* ctx) {
    return rtl_usb_session_ending(static_cast<const struct rtl_device*>(ctx));
}

static void
rtl_usb_set_recovering(struct rtl_device* dev, int on) {
    rtl_usb_ctrl_lock(dev);
    dev->recovering.store(on, std::memory_order_release);
    rtl_usb_ctrl_unlock(dev);
}

/* Close a handle the swap retired or gives up on. While the bias tee took on, one write turns it off first, its failure
 * ignored (a wedged dongle still answers control transfers): librtlsdr's close leaves it as it is and a USB reset does
 * not power the dongle down, so its LNA would stay powered after a stopped or failed recovery ends the session. A swap
 * that succeeds turns it back on in its replay. */
static void
rtl_usb_close_swapped(const struct rtl_device* dev, rtlsdr_dev_t* handle) {
#ifdef USE_RTLSDR_BIAS_TEE
    if (dev->usb_eff.bias_tee_took && dev->usb_eff.bias_tee) {
        (void)rtl_usb_ctl_set_bias_tee(handle, 0);
    }
#else
    (void)dev;
#endif
    (void)rtl_usb_ctl_close(handle);
}

/* One replay step: nothing when the control is not @p wanted, else apply (and verify) it, naming it in @p failed when
 * it does not take. Returns 0, -ECANCELED once a stop latched, or -EIO. */
template <typename Ctx>
static int
rtl_usb_replay_step(const struct rtl_device* dev, int wanted, const char* name,
                    typename rtl_usb_apply_ops<Ctx>::apply_fn apply, typename rtl_usb_apply_ops<Ctx>::verify_fn verify,
                    Ctx* ctx, const char** failed) {
    if (!wanted) {
        return 0;
    }
    if (rtl_usb_session_ending(dev)) {
        return -ECANCELED;
    }
    if (rtl_usb_apply_with_runtime_retry<Ctx>(apply, verify, ctx, NULL) != 0) {
        *failed = name;
        return -EIO;
    }
    return 0;
}

/* The replay's tuning before offset tuning: crystal, direct sampling, frequency, rate. */
static int
rtl_usb_replay_tuning(const struct rtl_device* dev, rtlsdr_dev_t* handle, const struct rtl_usb_effective* e,
                      const char** failed) {
    rtl_usb_xtal_apply_ctx xtal{handle, e->rtl_xtal, e->tuner_xtal};
    rtl_usb_int_apply_ctx direct_sampling{handle, e->direct_sampling};
    rtl_usb_frequency_apply_ctx freq{handle, e->freq};
    rtl_usb_sample_rate_apply_ctx rate{handle, e->rate, 0U};
    int rc = rtl_usb_replay_step(dev, e->xtal_took, "crystal frequency", rtl_usb_apply_xtal, NULL, &xtal, failed);
    /* A fresh dongle samples through its tuner; turning direct sampling off again would only re-init the tuner. */
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, e->direct_sampling_took && e->direct_sampling != 0, "direct sampling",
                                 rtl_usb_apply_direct_sampling, NULL, &direct_sampling, failed);
    }
    /* The frequency before the rate: librtlsdr retunes an R82xx to 0 Hz when a rate is set on a handle with none. */
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, e->freq_took, "frequency", rtl_usb_apply_center_freq, rtl_usb_verify_center_freq,
                                 &freq, failed);
    }
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, e->rate_took, "sample rate", rtl_usb_apply_sample_rate,
                                 rtl_usb_verify_sample_rate, &rate, failed);
    }
    return rc;
}

/* The replay's offset tuning and tuner bandwidth. */
static int
rtl_usb_replay_offset_and_bandwidth(const struct rtl_device* dev, rtlsdr_dev_t* handle,
                                    const struct rtl_usb_effective* e, const char** failed) {
    int rc = 0;
    /* Only when it displaced the LO before (librtlsdr refuses it on R82xx tuners and in direct sampling); it must
     * displace it again, or the capture would lose its centring. */
    if (e->offset_took) {
        rtl_usb_int_apply_ctx offset{handle, 1};
        rc = rtl_usb_replay_step(dev, 1, "offset tuning", rtl_usb_apply_offset_tuning, NULL, &offset, failed);
        if (rc == 0 && rtl_usb_verify_offset_tuning_on(&offset) != 0) {
            *failed = "offset tuning";
            rc = -EIO;
        }
    }
    if (rc == 0 && e->tuner_bw_took) {
        const uint32_t offs_hz = e->offset_took ? rtl_usb_offs_for_rate(rtl_usb_ctl_get_sample_rate(handle)) : 0U;
        rtl_usb_u32_apply_ctx bw{handle, rtl_offset_aware_bandwidth_hz(e->tuner_bw, e->offset_took, offs_hz)};
        rc = rtl_usb_replay_step(dev, 1, "tuner bandwidth", rtl_usb_apply_tuner_bandwidth, NULL, &bw, failed);
    }
    return rc;
}

/* The replay's gain stage onwards: gain, register 0x19, IF gains, bias tee, FIFO reset. */
static int
rtl_usb_replay_gain_and_frontend(const struct rtl_device* dev, rtlsdr_dev_t* handle, const struct rtl_usb_effective* e,
                                 const char** failed) {
    rtl_usb_int_apply_ctx gain{handle, e->gain};
    rtl_usb_register19_apply_ctx register19{handle, e->testmode, e->digital_agc};
    /* A gain that never took stays the fresh dongle's; one a failed change left unknown is put back as recorded. */
    int rc = rtl_usb_replay_step(dev, e->gain_took, "gain", rtl_usb_apply_tuner_gain, rtl_usb_verify_tuner_gain, &gain,
                                 failed);
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, e->gain_took || e->testmode_took, e->testmode ? "test mode" : "digital AGC",
                                 rtl_usb_apply_register19_values, NULL, &register19, failed);
    }
    for (int i = 0; rc == 0 && i < e->if_gain_count && i < 16; i++) {
        rtl_usb_if_gain_apply_ctx if_gain{handle, e->if_gains[i].stage, e->if_gains[i].gain};
        rc = rtl_usb_replay_step(dev, 1, "IF gain", rtl_usb_apply_if_gain, NULL, &if_gain, failed);
    }
#ifdef USE_RTLSDR_BIAS_TEE
    rtl_usb_int_apply_ctx bias_tee{handle, e->bias_tee};
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, e->bias_tee_took, "bias tee", rtl_usb_apply_bias_tee, NULL, &bias_tee, failed);
    }
#endif
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, 1, "FIFO reset", rtl_usb_apply_reset_buffer, NULL, handle, failed);
    }
    return rc;
}

/* Replay the effective record @p e onto the reopened, unpublished @p handle, in order: crystal, direct sampling,
 * frequency, rate, offset tuning, tuner bandwidth, PPM, frequency, gain, register 0x19, IF gains, bias tee, FIFO
 * reset. A control that never took is skipped (the fresh dongle keeps its default); one that took and does not take
 * again fails the replay, naming it in @p failed. Returns 0, -ECANCELED once a stop latched, or -EIO. */
static int
rtl_usb_replay_effective(const struct rtl_device* dev, rtlsdr_dev_t* handle, const struct rtl_usb_effective* e,
                         const char** failed) {
    rtl_usb_frequency_apply_ctx freq{handle, e->freq};
    rtl_usb_int_apply_ctx ppm{handle, e->ppm};
    int rc = rtl_usb_replay_tuning(dev, handle, e, failed);
    if (rc == 0) {
        rc = rtl_usb_replay_offset_and_bandwidth(dev, handle, e, failed);
    }
    /* A fresh dongle holds no correction, and librtlsdr refuses to set the one it holds. */
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, e->ppm_took && e->ppm != 0, "PPM correction", rtl_usb_apply_ppm, NULL, &ppm,
                                 failed);
    }
    if (rc == 0) {
        rc = rtl_usb_replay_step(dev, e->freq_took, "frequency", rtl_usb_apply_center_freq, rtl_usb_verify_center_freq,
                                 &freq, failed);
    }
    if (rc == 0) {
        rc = rtl_usb_replay_gain_and_frontend(dev, handle, e, failed);
    }
    return rc;
}

/* Check the reopened @p handle is the identified device, record where the dongle is now, and replay its settings. */
static int
rtl_usb_restore_handle(struct rtl_device* dev, rtlsdr_dev_t* handle, const struct rtl_usb_identity* fresh, char* why,
                       size_t why_size) {
    const rtl_usb_id_result verified = rtl_usb_identity_verify_handle(handle, &dev->identity);
    if (verified != RTL_USB_ID_OK) {
        DSD_SNPRINTF(why, why_size, "the reopened receiver: %s", rtl_usb_identity_off_reason(verified));
        return -EIO;
    }
    /* It may have come back on another port; the next reset goes there. */
    dev->identity = *fresh;
    const char* failed = "";
    const int rc = rtl_usb_replay_effective(dev, handle, &dev->usb_eff, &failed);
    if (rc != 0 && rc != -ECANCELED) {
        DSD_SNPRINTF(why, why_size, "could not restore the receiver's %s", failed);
    }
    return rc;
}

/* Reset the dongle at its recorded port and find it again, uniquely, before anything is opened (D3): its librtlsdr
 * index in @p out_index and where it is now in @p fresh. */
static int
rtl_usb_reset_and_locate(struct rtl_device* dev, int* out_index, struct rtl_usb_identity* fresh, char* why,
                         size_t why_size) {
    /* A stop cancels the reset, whose result then says nothing; the latch says it was a stop. */
    const rtl_usb_id_result reset = rtl_usb_identity_reset(&dev->identity, rtl_usb_stop_latched, dev);
    if (rtl_usb_stop_latched(dev)) {
        return -ECANCELED;
    }
    if (reset != RTL_USB_ID_OK) {
        DSD_SNPRINTF(why, why_size, "the USB reset failed: %s", rtl_usb_identity_off_reason(reset));
        return -EIO;
    }
    /* String verification cannot tell identical dongles apart, so a triple that no longer leads to exactly one device
     * fails the recovery, and recovery stays off from then on, rather than open a dongle that may not be this one. */
    const int index = rtl_usb_identity_index(&dev->identity);
    if (index < 0) {
        rtl_usb_recovery_off(dev, "no single receiver carries its USB strings after the reset");
        DSD_SNPRINTF(why, why_size, "%s", "after the USB reset no single receiver carries its USB strings");
        return -ENODEV;
    }
    const rtl_usb_id_result resolved = rtl_usb_identity_resolve(index, fresh);
    if (rtl_usb_stop_latched(dev)) {
        return -ECANCELED;
    }
    if (resolved != RTL_USB_ID_OK) {
        rtl_usb_recovery_off(dev, rtl_usb_identity_off_reason(resolved));
        DSD_SNPRINTF(why, why_size, "after the USB reset: %s", rtl_usb_identity_off_reason(resolved));
        return -ENODEV;
    }
    *out_index = index;
    return 0;
}

/* Publish the restored @p handle, and end the swap; not at all once a stop latched: the stop found no handle and closes
 * nothing. */
static int
rtl_usb_publish_restored(struct rtl_device* dev, rtlsdr_dev_t* handle) {
    int rc = 0;
    rtl_usb_ctrl_lock(dev);
    if (rtl_usb_stop_latched(dev)) {
        rc = -ECANCELED;
    } else {
        dev->dev = handle;
        if (dev->gain_state_unknown && dev->usb_eff.gain_took) {
            dev->gain_state_unknown = 0;
            rtl_usb_effective_note_gain(dev);
        }
        dev->recovering.store(0, std::memory_order_release);
    }
    rtl_usb_ctrl_unlock(dev);
    return rc;
}

#if (defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD))                                       \
    || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
/* The swap's reset and reopen for a dongle opened from an Android descriptor (#782), with its handle closed: reset
 * through that descriptor, then open it again (the descriptor is the device, so nothing is located), into @p out.
 * Outcomes: reset and reopened; refused, so it is reopened without a reset (the obligation stays, and a stall that
 * persists ends the session through the budget); or the descriptor is dead, ENODEV or EBADF from the reset or the
 * open, which fails the recovery: the device re-enumerates, and the host's detach and attach take over. */
static int
rtl_usb_fd_reset_and_reopen(struct rtl_device* dev, rtlsdr_dev_t** out, char* why, size_t why_size) {
    const int fd = dev->preopened_fd;
    if (rtl_usb_stop_latched(dev)) {
        return -ECANCELED;
    }
    if (rtl_usb_preopened_fd_moved(dev)) {
        DSD_SNPRINTF(why, why_size, "%s", "the app released the receiver's USB connection");
        return -ENODEV;
    }
    const rtl_usb_fd_reset_result reset = rtl_usb_fd_reset(fd, "to recover it");
    if (reset == RTL_USB_FD_RESET_DEAD) {
        DSD_SNPRINTF(why, why_size, "%s", "the USB reset left the receiver's descriptor unusable");
        return -ENODEV;
    }
    dev->reopened_without_reset.store(reset == RTL_USB_FD_RESET_REFUSED ? 1 : 0, std::memory_order_release);
    if (rtl_usb_stop_latched(dev)) {
        return -ECANCELED;
    }
    const int rc = rtl_usb_open_preopened_fd(out, fd);
    if (rc < 0 || !*out) {
        DSD_SNPRINTF(why, why_size, "could not reopen the receiver's descriptor after the USB reset (rc=%d)", rc);
        return (rc < 0) ? rc : -EIO;
    }
    return 0;
}

/* Check the handle reopened from the Android descriptor is the same dongle, and replay its settings. */
static int
rtl_usb_restore_fd_handle(struct rtl_device* dev, rtlsdr_dev_t* handle, char* why, size_t why_size) {
    struct rtl_usb_identity fresh{};
    const int id_rc = rtl_usb_fd_identity_read(dev->preopened_fd, handle, &fresh);
    if (id_rc != 0) {
        DSD_SNPRINTF(why, why_size, "the reopened receiver's USB identity could not be read (errno %d)", -id_rc);
        return -EIO;
    }
    if (!rtl_usb_fd_identity_same(&fresh, &dev->identity)) {
        rtl_usb_recovery_off(dev, "the reopened descriptor leads to another receiver");
        DSD_SNPRINTF(why, why_size, "%s", "the reopened descriptor leads to another receiver");
        return -EIO;
    }
    const char* failed = "";
    const int rc = rtl_usb_replay_effective(dev, handle, &dev->usb_eff, &failed);
    if (rc != 0 && rc != -ECANCELED) {
        DSD_SNPRINTF(why, why_size, "could not restore the receiver's %s", failed);
    }
    return rc;
}
#endif

/* The desktop swap's reset and reopen, with the handle closed: reset the dongle at its recorded port, find it again
 * and open it by its index, into @p out, with where it is now in @p fresh. */
static int
rtl_usb_port_reset_and_reopen(struct rtl_device* dev, rtlsdr_dev_t** out, struct rtl_usb_identity* fresh, char* why,
                              size_t why_size) {
    int index = -1;
    int rc = rtl_usb_reset_and_locate(dev, &index, fresh, why, why_size);
    if (rc != 0) {
        return rc;
    }
    rc = rtl_usb_open_index(out, (uint32_t)index);
    if (rc < 0 || !*out) {
        DSD_SNPRINTF(why, why_size, "could not reopen the receiver after the USB reset (rc=%d)", rc);
        return (rc < 0) ? rc : -EIO;
    }
    return 0;
}

/* The swap itself, under usb_lifecycle_lock with recovering set. */
static int
rtl_usb_swap_handle(struct rtl_device* dev, char* why, size_t why_size) {
    /* Whatever the swap ends in, the dongle no longer holds what a retune recorded: the next one writes it all. */
    dev->capture_settings_generation.fetch_add(1U, std::memory_order_acq_rel);
    /* The reader first, cancelled until it returned (silently: recovering is set) and joined, so nothing streams from
     * the dongle while it is reset and configured. */
    if (dev->thread_started) {
        rtl_usb_reader_cancel_and_join(dev);
    }
    rtlsdr_dev_t* retired = rtl_usb_retire_handle(dev);
    if (retired) {
        /* Release the endpoint FIFO before the USB reset, as normal destruction does. Leaving it held in reset can
         * make an RTL2832U repeat stale samples after reopening. A wedged handle may refuse; still reset the device. */
        (void)rtl_usb_ctl_reset_buffer(retired);
        rtl_usb_close_swapped(dev, retired);
    }
    rtlsdr_dev_t* handle = nullptr;
    int rc = 0;
#if (defined(__ANDROID__) && defined(USE_RTLSDR) && defined(USE_RTLSDR_OPEN_FD))                                       \
    || defined(DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS)
    if (dev->owns_preopened_fd) {
        rc = rtl_usb_fd_reset_and_reopen(dev, &handle, why, why_size);
        if (rc == 0) {
            rc = rtl_usb_restore_fd_handle(dev, handle, why, why_size);
        }
        if (rc == 0) {
            rc = rtl_usb_publish_restored(dev, handle);
        }
        if (rc != 0 && handle) {
            rtl_usb_close_swapped(dev, handle);
        }
        /* A reset that went through cleared the obligation, but what followed it failed: the session ends as a failed
         * recovery, whose notice promises that Reconnect resets the dongle (#782). */
        if (rc != 0 && rc != -ECANCELED) {
            rtl_usb_reset_owed_set(dev);
        }
        return rc;
    }
#endif
    if (rtl_usb_stop_latched(dev)) {
        return -ECANCELED;
    }
    struct rtl_usb_identity fresh{};
    rc = rtl_usb_port_reset_and_reopen(dev, &handle, &fresh, why, why_size);
    if (rc != 0) {
        return rc;
    }
    rc = rtl_usb_restore_handle(dev, handle, &fresh, why, why_size);
    if (rc == 0) {
        rc = rtl_usb_publish_restored(dev, handle);
    }
    if (rc != 0) {
        rtl_usb_close_swapped(dev, handle);
    }
    return rc;
}

int
rtl_device_usb_recover_handle(struct rtl_device* dev, char* why, size_t why_size) {
    char none[1];
    if (!why || why_size == 0U) {
        why = none;
        why_size = sizeof none;
    }
    why[0] = '\0';
    if (!dev || dev->backend != RTL_BACKEND_USB) {
        DSD_SNPRINTF(why, why_size, "%s", "not a USB receiver");
        return -EINVAL;
    }
    rtl_usb_lifecycle_lock(dev);
    int rc = 0;
    if (!dev->usb_recovery_on.load(std::memory_order_acquire)) {
        DSD_SNPRINTF(why, why_size, "stall recovery is off for this receiver: %s", dev->usb_recovery_off_reason);
        rc = -ENOTSUP;
    } else if (rtl_usb_stop_latched(dev)) {
        rc = -ECANCELED;
    } else {
        rtl_usb_set_recovering(dev, 1);
        rc = rtl_usb_swap_handle(dev, why, why_size);
        if (rc != 0) {
            /* Without a handle now: setters fail plainly, and nothing restarts the reader. */
            rtl_usb_set_recovering(dev, 0);
            if (rtl_usb_stop_latched(dev)) {
                rc = -ECANCELED;
            }
        }
    }
    if (rc == -ECANCELED) {
        DSD_SNPRINTF(why, why_size, "%s", "stopped");
    }
    rtl_usb_lifecycle_unlock(dev);
    return rc;
}

int
rtl_device_usb_resume_reader(struct rtl_device* dev) {
    if (!dev || dev->backend != RTL_BACKEND_USB) {
        return -EINVAL;
    }
    rtl_usb_lifecycle_lock(dev);
    int rc = 0;
    if (rtl_usb_stop_latched(dev)) {
        rc = -ECANCELED;
    } else if (dev->thread_started) {
        rc = -EALREADY;
    } else if (!dev->dev || !rtl_usb_reader_available(dev)) {
        rc = -ENODEV;
    } else {
        /* The reopened dongle is a fresh stream, as at a start (rtl_device_start_async()) and an rtl_tcp reconnect: the
         * old reader's byte carry, fs/4 phase and mute alignment end with it, so the new one starts on an I byte, and
         * the input level starts over. The old reader was joined by the swap. */
        rtl_reset_capture_state_on_stream_boundary(dev);
        /* The progress monitor watches the restarted reader afresh from this baseline (D2), taken before the thread
         * starts so its first callbacks count. */
        dev->progress_baseline.store(dev->progress.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dev->watch_rearm_seq.fetch_add(1U, std::memory_order_release);
        dev->reader_entered.store(0, std::memory_order_relaxed);
        dev->reader_exited.store(0, std::memory_order_relaxed);
        /* Until the recovery that restarts it finishes, this reader's return is that recovery's to report. */
        dev->recovery_handoff.store(rtl_usb_handoff_word(rtl_usb_next_reader_incarnation(dev), RTL_USB_HANDOFF_PENDING),
                                    std::memory_order_release);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
        rtl_usb_test_reader_restarting(dev);
#endif
        if (dsd_thread_create(&dev->thread, dongle_thread_fn, dev) == 0) {
            dev->thread_started = 1;
        } else {
            dev->recovery_handoff.store(0U, std::memory_order_release);
            rc = -EAGAIN;
        }
    }
    rtl_usb_lifecycle_unlock(dev);
    return rc;
}

int
rtl_device_usb_recovery_eligible(const struct rtl_device* dev) {
    return (dev && dev->backend == RTL_BACKEND_USB && dev->usb_recovery_on.load(std::memory_order_acquire)) ? 1 : 0;
}

void
rtl_device_set_recovery_callback(struct rtl_device* dev, rtl_device_recovery_cb cb, void* ctx) {
    if (!dev) {
        return;
    }
    rtl_usb_ctrl_lock(dev);
    dev->recovery_cb = cb;
    dev->recovery_cb_ctx = cb ? ctx : nullptr;
    rtl_usb_ctrl_unlock(dev);
}

int
rtl_device_usb_recovery_take(struct rtl_device* dev) {
    if (!dev || dev->backend != RTL_BACKEND_USB || !dev->run.load(std::memory_order_acquire)
        || rtl_usb_session_ending(dev)) {
        return 0;
    }
    int expected = RTL_USB_RECOVERY_REQUESTED;
    if (!dev->recovery_request.compare_exchange_strong(expected, RTL_USB_RECOVERY_TAKEN, std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
        return 0;
    }
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_recovery_take_hook.invoke(dev);
#endif
    /* A stop may arrive while the controller claims the request; every swap step also checks it. */
    if (!dev->run.load(std::memory_order_acquire) || rtl_usb_session_ending(dev)) {
        dev->recovery_request.store(RTL_USB_RECOVERY_IDLE, std::memory_order_release);
        return 0;
    }
    LOG_WARN("RTL-SDR: the receiver sent no samples for %llu ms; resetting the USB device\n",
             (unsigned long long)dev->recovery_silence_ms.load(std::memory_order_relaxed));
    return 1;
}

/* Close the restarted reader's hand-off in one compare-and-swap, against the one it makes when it returns
 * (rtl_usb_recovery_reader_handoff()). FAILED: it returned first, so the finish reports it. PENDING: nothing returned,
 * and a return from now on finds CLOSED and reports itself, the request being idle already. Whether it had failed. */
static int
rtl_usb_handoff_close(struct rtl_device* dev) {
    uint64_t handoff = dev->recovery_handoff.load(std::memory_order_acquire);
    while (rtl_usb_handoff_state_of(handoff) == RTL_USB_HANDOFF_PENDING
           || rtl_usb_handoff_state_of(handoff) == RTL_USB_HANDOFF_FAILED) {
        const uint64_t closed = rtl_usb_handoff_word((uint32_t)(handoff >> 32), RTL_USB_HANDOFF_CLOSED);
        if (dev->recovery_handoff.compare_exchange_weak(handoff, closed, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            break;
        }
    }
    return (rtl_usb_handoff_state_of(handoff) == RTL_USB_HANDOFF_FAILED) ? 1 : 0;
}

/* A recovery that failed after the swap (@p rc, @p why): the reader's restart, or the stream boundary around it. */
static void
rtl_usb_recovery_failed(struct rtl_device* dev, int rc, const char* why) {
    /* Whatever failed ends the session the same way, so the descriptor owes its reset here too (#782). */
    rtl_usb_reset_owed_set(dev);
    char cause[320];
    DSD_SNPRINTF(cause, sizeof cause,
                 "RTL-SDR: the receiver sent no samples and resetting it failed: %s; the stream stops",
                 (why && why[0] != '\0') ? why : "unknown error");
    (void)rtl_device_end_for_receiver(dev, rc, rtl_device_sample_progress(dev), cause);
}

int
rtl_device_usb_recovery_finish(struct rtl_device* dev, int rc, const char* why) {
    if (!dev || dev->backend != RTL_BACKEND_USB) {
        return 0;
    }
    /* Idle first: the ending below is this recovery's own verdict, which nothing supersedes, and so is the restarted
     * reader's once the hand-off is closed. */
    dev->recovery_request.store(RTL_USB_RECOVERY_IDLE, std::memory_order_seq_cst);
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    g_test_recovery_finish_hook.invoke(dev);
#endif
    /* Then close the restarted reader's hand-off. */
    const int restarted_failed = rtl_usb_handoff_close(dev);
    if (rc == 0 && !restarted_failed) {
        LOG_INFO("RTL-SDR: the USB device was reset and reopened; the stream resumes\n");
        return 1;
    }
    /* A stop or an exit request during the recovery ends it silently. */
    if (rc == -ECANCELED || dev->stop_requested.load(std::memory_order_acquire)) {
        return 0;
    }
    if (rc == 0) {
        /* Reset and reopened, but the restarted reader returned already: the session ends as for any reader's. */
        rtl_usb_reader_end_session(dev, dev->recovery_reader_rc.load(std::memory_order_relaxed));
        return 0;
    }
    rtl_usb_recovery_failed(dev, rc, why);
    return 0;
}

int
rtl_device_resume_stream(struct rtl_device* dev) {
    if (!dev) {
        return -EINVAL;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_device_usb_resume_reader(dev);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_resume_reader(dev);
    }
    return 0;
}

int
rtl_device_has_stream(const struct rtl_device* dev) {
    if (!dev) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        /* From the progress monitor's request to the controller's finish: the reader is stopped, or is about to be, and
           only the recovery's end (or the session's) brings samples. */
        return (dev->recovery_request.load(std::memory_order_acquire) == RTL_USB_RECOVERY_IDLE
                && !dev->recovering.load(std::memory_order_acquire))
                   ? 1
                   : 0;
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        /* From the loss of the connection until a reconnected one is published. */
        return dev->tcp_connected.load(std::memory_order_acquire) ? 1 : 0;
    }
    /* Every other backend has one while it runs. */
    return 1;
}

int
rtl_device_control_unavailable(const struct rtl_device* dev) {
    if (!dev || rtl_device_has_stream(dev)) {
        return 0;
    }
    return (dev->backend == RTL_BACKEND_TCP) ? RTL_DEVICE_ENOTCONN : RTL_DEVICE_EBUSY;
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* A reader restarted on the streaming seam streams again until the next cancel. */
static void
rtl_usb_test_reader_restarting(struct rtl_device* dev) {
    g_test_reader_restart_hook.invoke(dev);
    if (g_test_usb_reader_stream.load(std::memory_order_acquire)) {
        g_test_usb_reader_cancelled.store(0, std::memory_order_release);
        g_test_usb_reader_stream_return.store(0, std::memory_order_release);
    }
}

extern "C" int
rtl_device_test_tcp_read_header(intptr_t sock, unsigned int budget_ms, int (*cancelled)(void* ctx), void* ctx,
                                uint32_t* out_tuner, uint32_t* out_gain_count) {
    return (int)rtl_tcp_read_header((dsd_socket_t)sock, budget_ms, cancelled, ctx, out_tuner, out_gain_count);
}

extern "C" void
rtl_device_test_tcp_fail_command(int cmd) {
    g_test_tcp_fail_cmd.store(cmd, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_tcp_set_send_chunk(size_t max_bytes) {
    g_test_tcp_send_chunk.store(max_bytes, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_tcp_set_sndbuf(int bytes) {
    g_test_tcp_sndbuf.store(bytes, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_tcp_set_send_hook(void (*hook)(uint64_t generation, void* ctx), void* ctx) {
    g_test_tcp_send_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_tcp_set_commit_hook(void (*hook)(uint64_t, void*), void* ctx) {
    g_test_tcp_commit_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_tcp_refuse_read_buffer(int refuse) {
    g_test_tcp_refuse_read_buffer.store(refuse, std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_tcp_set_discard_hook(void (*hook)(void* ctx), void* ctx) {
    g_test_tcp_discard_hook.set(hook, ctx);
}

extern "C" void
rtl_device_test_tcp_set_read_hook(void (*hook)(void* ctx), void* ctx) {
    g_test_tcp_read_hook.set(hook, ctx);
}

extern "C" uint64_t
rtl_device_test_tcp_odd_drops(void) {
    return g_test_tcp_odd_drops.load(std::memory_order_relaxed);
}

extern "C" void
rtl_device_test_tcp_state_get(struct rtl_device* dev, rtl_device_test_tcp_state* out) {
    if (!out) {
        return;
    }
    *out = {};
    if (!dev || dev->backend != RTL_BACKEND_TCP) {
        return;
    }
    rtl_tcp_lock(dev);
    out->generation = dev->tcp_conn ? dev->tcp_conn->generation : 0U;
    out->handoff = dev->tcp_handoff;
    out->gain_took = dev->tcp_took.gain;
    out->digital_agc = dev->digital_agc_wanted;
    out->gain_unknown = dev->tcp_took.gain_unknown;
    rtl_tcp_unlock(dev);
    out->stop_requested = dev->stop_requested.load(std::memory_order_acquire);
    rtl_tcp_metrics_lock(dev);
    out->watchdog_grace_from_ns = dev->tcp_metrics.connection_established_ns;
    rtl_tcp_metrics_unlock(dev);
}

extern "C" void
rtl_device_test_usb_stream_attach(void) {
    g_test_usb_reader_rc.store(0, std::memory_order_relaxed);
    g_test_usb_reader_wait_stop.store(0, std::memory_order_relaxed);
    g_test_usb_reader_hold.store(RTL_TEST_USB_HOLD_NONE, std::memory_order_relaxed);
    g_test_usb_reader_held.store(0, std::memory_order_relaxed);
    g_test_usb_reader_cancels.store(0, std::memory_order_relaxed);
    g_test_usb_reader_streaming.store(0, std::memory_order_relaxed);
    g_test_usb_reader_cancelled.store(0, std::memory_order_relaxed);
    g_test_usb_reader_stream.store(1, std::memory_order_relaxed);
    g_test_usb_reader_stream_return.store(0, std::memory_order_relaxed);
    g_test_usb_reader_active.store(1, std::memory_order_release);
}

extern "C" void
rtl_device_test_usb_stream_detach(void) {
    g_test_usb_reader_active.store(0, std::memory_order_release);
    g_test_usb_reader_stream.store(0, std::memory_order_release);
}

extern "C" int
rtl_device_test_capture_reconfigure_held(const struct rtl_device* dev) {
    return rtl_capture_reconfigure_hold_active(dev);
}

extern "C" void
rtl_device_test_usb_swap_state_get(struct rtl_device* dev, rtl_device_test_usb_swap_state* out) {
    if (!dev || !out) {
        return;
    }
    rtl_usb_ctrl_lock(dev);
    out->has_handle = dev->dev ? 1 : 0;
    out->recovering = dev->recovering.load(std::memory_order_acquire);
    rtl_usb_ctrl_unlock(dev);
    /* Read by the thread that last changed them, or after it was joined. */
    out->thread_started = dev->thread_started;
    out->reader_running =
        dev->reader_entered.load(std::memory_order_acquire) && !dev->reader_exited.load(std::memory_order_acquire);
    out->stop_requested = dev->stop_requested.load(std::memory_order_acquire);
    out->recovery_on = rtl_device_usb_recovery_eligible(dev);
    out->progress_baseline = dev->progress_baseline.load(std::memory_order_relaxed);
    out->watch_rearm_seq = dev->watch_rearm_seq.load(std::memory_order_acquire);
    out->identity_bus = dev->identity.bus;
    DSD_MEMCPY(out->identity_ports, dev->identity.ports, sizeof out->identity_ports);
    out->identity_port_depth = dev->identity.port_depth;
}
#endif

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* Arm the USB reader seam for @p request, its reader held at @p hold while it starts. */
static void
rtl_device_test_usb_reader_arm(const rtl_device_test_usb_reader_request* request, int hold) {
    g_test_usb_reader_rc.store(request->reader_rc, std::memory_order_relaxed);
    g_test_usb_reader_wait_stop.store(request->stop_while_reading ? 1 : 0, std::memory_order_relaxed);
    g_test_usb_reader_hold.store(hold, std::memory_order_relaxed);
    g_test_usb_reader_held.store(0, std::memory_order_relaxed);
    g_test_usb_reader_cancels.store(0, std::memory_order_relaxed);
    g_test_usb_reader_streaming.store(0, std::memory_order_relaxed);
    g_test_usb_reader_streamed.store(0, std::memory_order_relaxed);
    g_test_usb_reader_cancelled.store(0, std::memory_order_relaxed);
    g_test_usb_reader_stream.store(0, std::memory_order_relaxed);
    g_test_usb_reader_active.store(1, std::memory_order_release);
}

/* End the started reader of @p dev as @p request asks: once it is held where the test asked and the stop delay passed,
 * stop it (or destroy the device, which clears @p dev); with no stop asked, join the reader that returns by itself. */
static int
rtl_device_test_usb_reader_end(struct rtl_device** dev, const rtl_device_test_usb_reader_request* request, int hold) {
    if (hold != RTL_TEST_USB_HOLD_NONE) {
        /* Stop only once the reader is held where the test asked. */
        for (int waited_ms = 0; waited_ms < 2000 && !g_test_usb_reader_held.load(std::memory_order_acquire);
             ++waited_ms) {
            dsd_sleep_ms(1);
        }
    }
    if (request->stop_after_ms > 0) {
        dsd_sleep_ms((unsigned int)request->stop_after_ms);
    }
    if (!request->stop_while_reading && request->stop_after_ms <= 0 && hold == RTL_TEST_USB_HOLD_NONE) {
        /* The reader returns by itself; the session's stop comes after it. */
        dsd_thread_join((*dev)->thread);
        rtl_progress_monitor_stop(*dev);
        (*dev)->thread_started = 0;
        return 0;
    }
    if (request->destroy_instead_of_stop) {
        rtl_device_destroy(*dev);
        *dev = nullptr;
        return 0;
    }
    return rtl_device_stop_async(*dev);
}

extern "C" int
rtl_device_test_usb_reader_exit(rtl_device_test_usb_reader_request* request) {
    if (!request || !request->ring) {
        return -1;
    }
    /* Allocated as rtl_device_create() does, so rtl_device_destroy() can end it. */
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return -1;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return -1;
    }
    dev->backend = RTL_BACKEND_USB;
    dev->input_ring = request->ring;
    int hold = RTL_TEST_USB_HOLD_NONE;
    if (request->stop_before_reader_entry) {
        hold = RTL_TEST_USB_HOLD_BEFORE_ENTRY;
    } else if (request->stop_before_read_async) {
        hold = RTL_TEST_USB_HOLD_BEFORE_READ;
    }
    rtl_device_test_usb_reader_arm(request, hold);
    int rc = rtl_device_start_async(dev, 16U * 32U * 512U);
    if (rc == 0) {
        rc = rtl_device_test_usb_reader_end(&dev, request, hold);
    }
    request->reader_streamed = g_test_usb_reader_streamed.load(std::memory_order_acquire);
    g_test_usb_reader_active.store(0, std::memory_order_release);
    g_test_usb_reader_hold.store(RTL_TEST_USB_HOLD_NONE, std::memory_order_release);
    rtl_device_destroy(dev);
    return rc;
}

extern "C" struct rtl_device*
rtl_device_test_usb_stream_start(struct input_ring_state* ring) {
    if (!ring) {
        return nullptr;
    }
    /* Allocated as rtl_device_create() does, so rtl_device_destroy() can end it. */
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return nullptr;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
    dev->backend = RTL_BACKEND_USB;
    dev->input_ring = ring;
    g_test_usb_reader_rc.store(0, std::memory_order_relaxed);
    g_test_usb_reader_wait_stop.store(0, std::memory_order_relaxed);
    g_test_usb_reader_hold.store(RTL_TEST_USB_HOLD_NONE, std::memory_order_relaxed);
    g_test_usb_reader_held.store(0, std::memory_order_relaxed);
    g_test_usb_reader_cancels.store(0, std::memory_order_relaxed);
    g_test_usb_reader_streaming.store(0, std::memory_order_relaxed);
    g_test_usb_reader_cancelled.store(0, std::memory_order_relaxed);
    g_test_usb_reader_stream.store(1, std::memory_order_relaxed);
    g_test_usb_reader_stream_return.store(0, std::memory_order_relaxed);
    g_test_usb_reader_active.store(1, std::memory_order_release);
    if (rtl_device_start_async(dev, 16U * 32U * 512U) != 0) {
        g_test_usb_reader_active.store(0, std::memory_order_release);
        g_test_usb_reader_stream.store(0, std::memory_order_release);
        rtl_device_destroy(dev);
        return nullptr;
    }
    return dev;
}

extern "C" void
rtl_device_test_usb_stream_deliver(struct rtl_device* dev, unsigned char* buf, uint32_t len) {
    const std::lock_guard<std::mutex> delivering(g_test_usb_deliver_lock);
    rtlsdr_callback(buf, len, dev);
}

extern "C" void
rtl_device_test_usb_stream_return(int rc) {
    g_test_usb_reader_rc.store(rc, std::memory_order_relaxed);
    g_test_usb_reader_stream_return.store(1, std::memory_order_release);
}

extern "C" int
rtl_device_test_usb_stream_reader_exited(struct rtl_device* dev) {
    return dev && dev->reader_exited.load(std::memory_order_acquire);
}

extern "C" unsigned int
rtl_device_test_usb_reader_returns(void) {
    return g_test_usb_reader_returns.load(std::memory_order_acquire);
}

extern "C" void
rtl_device_test_usb_stream_stop(struct rtl_device* dev) {
    if (!dev) {
        return;
    }
    (void)rtl_device_stop_async(dev);
    g_test_usb_reader_active.store(0, std::memory_order_release);
    g_test_usb_reader_stream.store(0, std::memory_order_release);
    rtl_device_destroy(dev);
}

extern "C" struct rtl_device*
rtl_device_test_usb_gain_device(rtl_device_test_usb_gain_fake* fake) {
    if (!fake || g_test_usb_gain_fake) {
        return nullptr;
    }
    /* Allocated as rtl_device_create() does, so rtl_device_destroy() can end it. */
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return nullptr;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
    dev->backend = RTL_BACKEND_USB;
    dev->agc_mode = 1;
    /* Never dereferenced: every control the gain paths call reaches the fake while it is installed. */
    dev->dev = reinterpret_cast<rtlsdr_dev_t*>(fake);
    g_test_usb_gain_fake = fake;
    return dev;
}

extern "C" void
rtl_device_test_usb_gain_device_free(struct rtl_device* dev) {
    g_test_usb_gain_fake = nullptr;
    if (dev) {
        dev->dev = nullptr;
        rtl_device_destroy(dev);
    }
}

#ifdef USE_SOAPYSDR
namespace {
class RtlTestSoapyGainDevice : public SoapySDR::Device {
    rtl_device_test_soapy_gain_fake* fake_;

  public:
    explicit RtlTestSoapyGainDevice(rtl_device_test_soapy_gain_fake* fake) : fake_(fake) {}

    bool
    hasGainMode(const int, const size_t) const override {
        return true;
    }

    void
    setGainMode(const int, const size_t, const bool) override {}

    std::vector<std::string>
    listGains(const int, const size_t) const override {
        return {"LNA"};
    }

    SoapySDR::Range
    getGainRange(const int, const size_t) const override {
        return {0.0, 50.0};
    }

    void
    setGain(const int, const size_t, const double) override {
        fake_->writes++;
        if (fake_->refuse) {
            throw std::runtime_error("gain refused");
        }
        fake_->gain_db = fake_->selected_db;
    }

    double
    getGain(const int, const size_t) const override {
        return fake_->gain_db;
    }

    void
    setFrequency(const int, const size_t, const double, const SoapySDR::Kwargs&) override {
        fake_->frequency_writes++;
    }

    void
    setSampleRate(const int, const size_t, const double rate) override {
        fake_->rate_writes++;
        fake_->rate_hz = rate;
    }

    double
    getSampleRate(const int, const size_t) const override {
        return fake_->rate_hz;
    }

    void
    setBandwidth(const int, const size_t, const double) override {
        fake_->bandwidth_writes++;
    }
};
} // namespace
#endif

extern "C" struct rtl_device*
rtl_device_test_soapy_gain_device(rtl_device_test_soapy_gain_fake* fake, int named_override) {
#ifdef USE_SOAPYSDR
    if (!fake) {
        return nullptr;
    }
    struct rtl_device* dev = rtl_device_alloc_soapy_base(nullptr);
    if (!dev) {
        return nullptr;
    }
    if (dsd_mutex_init(&dev->soapy_lock) != 0) {
        rtl_device_cleanup_common_state(dev);
        free(dev);
        return nullptr;
    }
    dev->soapy_lock_inited = 1;
    dev->soapy_dev = new RtlTestSoapyGainDevice(fake);
    dev->soapy_named_gain_override = named_override;
    if (fake->driver_key) {
        rtl_device_copy_cstr(dev->soapy_driver_key, sizeof(dev->soapy_driver_key), fake->driver_key);
        soapy_select_profile_locked(dev);
    }
    return dev;
#else
    (void)fake;
    (void)named_override;
    return nullptr;
#endif
}

extern "C" void
rtl_device_test_soapy_gain_device_free(struct rtl_device* dev) {
#ifdef USE_SOAPYSDR
    if (dev) {
        delete dev->soapy_dev;
        dev->soapy_dev = nullptr;
    }
#endif
    rtl_device_destroy(dev);
}

extern "C" struct rtl_device*
rtl_device_test_soapy_device(struct input_ring_state* ring, const rtl_device_test_soapy_driver* driver) {
#ifdef USE_SOAPYSDR
    if (!ring || !driver || !driver->setup || !driver->read) {
        return nullptr;
    }
    const rtl_device_test_soapy_driver* expected = nullptr;
    if (!g_test_soapy_driver.compare_exchange_strong(expected, driver, std::memory_order_acq_rel)) {
        return nullptr;
    }
    g_test_soapy_stream_closes.store(0U, std::memory_order_relaxed);
    struct rtl_device* dev = rtl_device_alloc_soapy_base(ring);
    if (dev && dsd_mutex_init(&dev->soapy_lock) != 0) {
        rtl_device_cleanup_common_state(dev);
        free(dev);
        dev = nullptr;
    }
    if (!dev) {
        g_test_soapy_driver.store(nullptr, std::memory_order_release);
        return nullptr;
    }
    dev->soapy_lock_inited = 1;
    dev->soapy_format = driver->cs16 ? SOAPY_FMT_CS16 : SOAPY_FMT_CF32;
    /* Never dereferenced: every driver call reaches the seam while it is attached. */
    dev->soapy_dev = reinterpret_cast<SoapySDR::Device*>(&g_test_soapy_handle_tokens[0]);
    return dev;
#else
    (void)ring;
    (void)driver;
    return nullptr;
#endif
}

extern "C" void
rtl_device_test_soapy_device_free(struct rtl_device* dev) {
    rtl_device_destroy(dev);
#ifdef USE_SOAPYSDR
    g_test_soapy_driver.store(nullptr, std::memory_order_release);
#endif
}

extern "C" int
rtl_device_test_soapy_control_hold(struct rtl_device* dev, unsigned int ms) {
#ifdef USE_SOAPYSDR
    return soapy_call_locked(dev, "test control", [&]() -> int {
        dsd_sleep_ms(ms);
        return 0;
    });
#else
    (void)dev;
    (void)ms;
    return -1;
#endif
}

extern "C" void
rtl_device_test_soapy_internal_read_failure(int on) {
#ifdef USE_SOAPYSDR
    g_test_soapy_internal_read_failure.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}

extern "C" void
rtl_device_test_soapy_refuse_reader(int on) {
#ifdef USE_SOAPYSDR
    g_test_soapy_refuse_reader.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}

extern "C" void
rtl_device_test_soapy_state_get(struct rtl_device* dev, rtl_device_test_soapy_state* out) {
    if (!out) {
        return;
    }
    *out = {};
    if (!dev) {
        return;
    }
    out->reader_entered = dev->reader_entered.load(std::memory_order_acquire);
    out->reader_exited = dev->reader_exited.load(std::memory_order_acquire);
    out->monitor_started = dev->progress_monitor_started;
#ifdef USE_SOAPYSDR
    if (dev->soapy_lock_inited && dsd_mutex_lock(&dev->soapy_lock) == 0) {
        out->stream_open = dev->soapy_stream ? 1 : 0;
        (void)dsd_mutex_unlock(&dev->soapy_lock);
    }
    out->stream_closes = g_test_soapy_stream_closes.load(std::memory_order_relaxed);
    out->timeouts = dev->soapy_timeout_count.load(std::memory_order_relaxed);
    out->overflows = dev->soapy_overflow_count.load(std::memory_order_relaxed);
#endif
}

extern "C" void
rtl_device_test_set_recovering(struct rtl_device* dev, int on) {
    if (dev) {
        dev->recovering.store(on ? 1 : 0, std::memory_order_release);
    }
}

extern "C" int
rtl_device_test_usb_gain_unknown(const struct rtl_device* dev) {
    return (dev && dev->gain_state_unknown) ? 1 : 0;
}

extern "C" struct rtl_device*
rtl_device_test_usb_tuner_device(rtl_device_test_usb_tuner_fake* fake) {
    if (!fake || g_test_usb_tuner_fake) {
        return nullptr;
    }
    /* Allocated as rtl_device_create() does, so rtl_device_destroy() can end it. */
    struct rtl_device* dev = static_cast<rtl_device*>(calloc(1, sizeof(struct rtl_device)));
    if (!dev) {
        return nullptr;
    }
    if (rtl_device_init_common_state(dev) != 0) {
        free(dev);
        return nullptr;
    }
    dev->backend = RTL_BACKEND_USB;
    dev->agc_mode = 1;
    /* Never dereferenced: every control the tuning paths call reaches the fake while it is installed. */
    dev->dev = reinterpret_cast<rtlsdr_dev_t*>(fake);
    g_test_usb_tuner_fake = fake;
    return dev;
}

extern "C" void
rtl_device_test_usb_tuner_device_free(struct rtl_device* dev) {
    g_test_usb_tuner_fake = nullptr;
    if (dev) {
        dev->dev = nullptr;
        rtl_device_destroy(dev);
    }
}
#endif

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
    int requested = on ? 1 : 0;
    int rc = -1;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rc = 0;
    } else if (dev->backend == RTL_BACKEND_TCP) {
        /* rtl_tcp protocol command 0x0E toggles bias tee */
        return rtl_tcp_command(dev, 0x0E, (uint32_t)requested, [&]() {
            dev->bias_tee_on = requested;
            dev->tcp_took.bias_tee = 1;
        });
    } else if (dev->backend == RTL_BACKEND_SOAPY || dev->backend == RTL_BACKEND_AIRSPY) {
        rc = DSD_ERR_NOT_SUPPORTED;
    } else {
        return rtl_usb_set_bias_tee(dev, requested);
    }
    if (rc == 0) {
        dev->bias_tee_on = requested;
    }
    return rc;
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
rtl_device_set_xtal_freq(struct rtl_device* dev, uint32_t rtl_xtal_hz, uint32_t tuner_xtal_hz) {
    if (!dev) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_set_xtal_freq(dev, rtl_xtal_hz, tuner_xtal_hz);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        return rtl_tcp_apply(
            dev,
            [&](struct rtl_tcp_cmd* cmds) {
                size_t n = 0U;
                if (rtl_xtal_hz > 0U) {
                    cmds[n++] = {0x0B, rtl_xtal_hz};
                }
                if (tuner_xtal_hz > 0U) {
                    cmds[n++] = {0x0C, tuner_xtal_hz};
                }
                return n;
            },
            [&]() {
                if (rtl_xtal_hz != 0U) {
                    dev->rtl_xtal_hz = rtl_xtal_hz;
                }
                if (tuner_xtal_hz != 0U) {
                    dev->tuner_xtal_hz = tuner_xtal_hz;
                }
                dev->tcp_took.xtal = 1;
            });
    }
    dev->rtl_xtal_hz = rtl_xtal_hz;
    dev->tuner_xtal_hz = tuner_xtal_hz;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    return -1; /* Airspy: no such control */
}

int
rtl_device_set_testmode(struct rtl_device* dev, int on) {
    if (!dev) {
        return -1;
    }
    int requested = on ? 1 : 0;
    int rc = -1;
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        rc = 0;
    } else if (dev->backend == RTL_BACKEND_TCP) {
        /* The server writes register 0x19 for test mode and the digital AGC both (B9): turning test mode off puts the
         * digital AGC back as the gain state wants it. */
        return rtl_tcp_apply(
            dev,
            [&](struct rtl_tcp_cmd* cmds) {
                size_t n = 0U;
                cmds[n++] = {0x07, (uint32_t)requested};
                if (!requested) {
                    cmds[n++] = {0x08, (uint32_t)(dev->digital_agc_wanted ? 1 : 0)};
                }
                return n;
            },
            [&]() {
                dev->testmode_on = requested;
                dev->tcp_took.testmode = 1;
            },
            [&]() { dev->tcp_took.testmode_unknown = 1; });
    } else if (dev->backend == RTL_BACKEND_SOAPY) {
        rc = DSD_ERR_NOT_SUPPORTED;
    } else {
        return rtl_usb_set_testmode(dev, requested);
    }
    if (rc == 0) {
        dev->testmode_on = requested;
    }
    return rc;
}

int
rtl_device_set_if_gain(struct rtl_device* dev, int stage, int gain_tenth_db) {
    if (!dev) {
        return -1;
    }
    if (stage < 0) {
        return -1;
    }
    if (dev->backend == RTL_BACKEND_USB) {
        return rtl_usb_set_if_gain(dev, stage, gain_tenth_db);
    }
    if (dev->backend == RTL_BACKEND_TCP) {
        uint32_t packed = ((uint32_t)(stage & 0xFFFF) << 16) | ((uint16_t)(gain_tenth_db & 0xFFFF));
        return rtl_tcp_command(dev, 0x06, packed,
                               [&]() { rtl_if_gain_upsert(dev->if_gains, &dev->if_gain_count, stage, gain_tenth_db); });
    }
    rtl_if_gain_upsert(dev->if_gains, &dev->if_gain_count, stage, gain_tenth_db);
    if (dev->backend == RTL_BACKEND_IQ_REPLAY) {
        return 0;
    }
    if (dev->backend == RTL_BACKEND_SOAPY) {
        return DSD_ERR_NOT_SUPPORTED;
    }
    return -1; /* Airspy: no such control */
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
    if (rtl_device_init_common_state(&dev) != 0) {
        return -1;
    }
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
    if (rtl_device_init_common_state(&dev) != 0) {
        free(buf);
        input_ring_destroy(&ring);
        return -4;
    }
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
rtl_device_test_u8_odd_carry_bridge(size_t* out_used, int* out_phase, int* out_carry_valid, uint8_t* out_carry_byte,
                                    int* out_first_status, int* out_second_status) {
    if (!out_used || !out_phase || !out_carry_valid || !out_carry_byte || !out_first_status || !out_second_status) {
        return -1;
    }
    input_ring_state ring{};
    if (input_ring_init(&ring, 16U) != 0) {
        return -2;
    }

    rtl_device dev{};
    dev.input_ring = &ring;
    dev.backend = RTL_BACKEND_IQ_REPLAY;
    dev.replay_fs4_shift_enabled = 1;
    dev.replay_historical_cu8_two_pass = 0;
    dev.rot_phase = 0;

    unsigned char first[] = {129U};
    unsigned char second[] = {128U, 126U};
    *out_first_status = rtl_write_u8_to_ring(&dev, first, sizeof(first), 1, 0, 1, 0, NULL);
    *out_second_status = rtl_write_u8_to_ring(&dev, second, sizeof(second), 1, 0, 1, 0, NULL);
    *out_used = input_ring_used(&ring);
    *out_phase = dev.rot_phase;
    *out_carry_valid = dev.iq_byte_carry.valid ? 1 : 0;
    *out_carry_byte = dev.iq_byte_carry.byte;

    input_ring_destroy(&ring);
    return 0;
}

extern "C" int
rtl_device_test_u8_full_ring_drop(size_t* out_used, uint64_t* out_drops, uint64_t* out_full_events, int* out_phase,
                                  int* out_status) {
    if (!out_used || !out_drops || !out_full_events || !out_phase || !out_status) {
        return -1;
    }
    input_ring_state ring{};
    if (input_ring_init(&ring, 3U) != 0) {
        return -2;
    }

    rtl_device dev{};
    dev.input_ring = &ring;
    dev.backend = RTL_BACKEND_USB;
    dev.offset_took = 0;
    dev.rot_phase = 0;

    unsigned char input[] = {130U, 127U, 129U, 126U, 128U, 125U};
    *out_status = rtl_write_u8_to_ring(&dev, input, sizeof(input), 1, 0, 1, 1, NULL);
    *out_used = input_ring_used(&ring);
    *out_drops = ring.producer_drops.load(std::memory_order_acquire);
    *out_full_events = dev.reserve_full_events;
    *out_phase = dev.rot_phase;

    input_ring_destroy(&ring);
    return 0;
}

extern "C" int
rtl_device_test_u8_generation_stale_drop(uint64_t* out_drops, int* out_phase, int* out_dev_carry_valid,
                                         int* out_local_carry_valid, int* out_status) {
    if (!out_drops || !out_phase || !out_dev_carry_valid || !out_local_carry_valid || !out_status) {
        return -1;
    }
    input_ring_state ring{};
    if (input_ring_init(&ring, 8U) != 0) {
        return -2;
    }

    rtl_device dev{};
    dev.input_ring = &ring;
    dev.backend = RTL_BACKEND_USB;
    dev.rot_phase = 3;
    rtl_capture_u8_byte_carry_save(&dev.iq_byte_carry, 200U);

    unsigned char input[] = {120U, 121U, 122U, 123U, 124U};
    rtl_capture_u8_byte_carry carry = dev.iq_byte_carry;
    int phase = dev.rot_phase;
    size_t done = 1U;
    size_t need = 4U;
    size_t produced = 2U;
    *out_status = rtl_handle_u8_ring_generation_stale(&dev, input, done, need, &carry, &phase, 1, produced);

    rtl_u8_finalize_state final_state = {input, sizeof(input), done, need, &carry, &phase, 1, 0, 0, 1, NULL};
    rtl_finalize_u8_ring_write(&dev, &final_state);

    *out_drops = ring.producer_drops.load(std::memory_order_acquire);
    *out_phase = phase;
    *out_dev_carry_valid = dev.iq_byte_carry.valid ? 1 : 0;
    *out_local_carry_valid = carry.valid ? 1 : 0;

    input_ring_destroy(&ring);
    return 0;
}

static int
rtl_device_test_collect_write_moments(unsigned char* input, size_t input_len, size_t ring_capacity, size_t start_index,
                                      int use_two_pass, int force_stale, dsd_input_level_cu8_moments* out) {
    if (!input || input_len == 0U || !out || ring_capacity < 3U || start_index >= ring_capacity) {
        return -1;
    }
    input_ring_state ring{};
    if (input_ring_init(&ring, ring_capacity) != 0) {
        return -2;
    }
    ring.head.store(start_index, std::memory_order_relaxed);
    ring.tail.store(start_index, std::memory_order_relaxed);

    rtl_device dev{};
    dev.input_ring = &ring;
    dev.backend = RTL_BACKEND_USB;
    dsd_input_level_cu8_moments_reset(out);
    if (force_stale) {
        g_rtl_test_force_u8_generation_stale.store(1, std::memory_order_release);
    }
    (void)rtl_write_u8_to_ring(&dev, input, input_len, 1, use_two_pass, use_two_pass ? 0 : 1, 1, out);
    g_rtl_test_force_u8_generation_stale.store(0, std::memory_order_release);
    input_ring_destroy(&ring);
    return 0;
}

extern "C" int
rtl_device_test_u8_moment_accounting(dsd_input_level_cu8_moments* out, size_t out_count) {
    if (!out || out_count < 9U) {
        return -1;
    }

    unsigned char contiguous[] = {0U, 1U, 2U, 128U, 254U, 255U};
    unsigned char wrapped[] = {0U, 1U, 2U, 128U, 254U, 255U};
    unsigned char full[] = {0U, 1U, 2U, 128U, 254U, 255U};
    unsigned char stale[] = {0U, 1U, 2U, 128U, 254U, 255U};
    if (rtl_device_test_collect_write_moments(contiguous, sizeof(contiguous), 10U, 0U, 0, 0, &out[0]) != 0
        || rtl_device_test_collect_write_moments(wrapped, sizeof(wrapped), 10U, 8U, 0, 0, &out[1]) != 0) {
        return -2;
    }

    input_ring_state odd_ring{};
    if (input_ring_init(&odd_ring, 16U) != 0) {
        return -2;
    }
    rtl_device odd_dev{};
    odd_dev.input_ring = &odd_ring;
    odd_dev.backend = RTL_BACKEND_USB;
    unsigned char odd_first[] = {3U};
    unsigned char odd_second[] = {4U, 5U};
    dsd_input_level_cu8_moments_reset(&out[2]);
    dsd_input_level_cu8_moments_reset(&out[3]);
    (void)rtl_write_u8_to_ring(&odd_dev, odd_first, sizeof(odd_first), 1, 0, 1, 0, &out[2]);
    (void)rtl_write_u8_to_ring(&odd_dev, odd_second, sizeof(odd_second), 1, 0, 1, 0, &out[3]);
    input_ring_destroy(&odd_ring);

    if (rtl_device_test_collect_write_moments(full, sizeof(full), 3U, 0U, 0, 0, &out[4]) != 0
        || rtl_device_test_collect_write_moments(stale, sizeof(stale), 5U, 0U, 0, 1, &out[5]) != 0) {
        return -2;
    }

    input_ring_state tcp_ring{};
    if (input_ring_init(&tcp_ring, 32U) != 0) {
        return -2;
    }
    rtl_device tcp_dev{};
    tcp_dev.input_ring = &tcp_ring;
    tcp_dev.backend = RTL_BACKEND_TCP;
    tcp_dev.buf_len = 4;
    tcp_dev.tcp_pending = static_cast<unsigned char*>(malloc(4U));
    if (!tcp_dev.tcp_pending) {
        input_ring_destroy(&tcp_ring);
        return -2;
    }
    tcp_dev.tcp_pending_cap = 4U;
    tcp_dev.tcp_pending_len = 2U;
    tcp_dev.tcp_pending[0] = 91U;
    tcp_dev.tcp_pending[1] = 92U;
    unsigned char tcp_input[] = {0U, 255U, 1U, 2U, 3U, 4U, 254U, 253U, 128U};
    rtl_u8_transform_policy_state policy{1, 1, 0};
    dsd_input_level_cu8_moments_reset(&out[6]);
    rtl_tcp_reassemble_and_submit(&tcp_dev, tcp_input, (uint32_t)sizeof(tcp_input), &policy, &out[6]);
    free(tcp_dev.tcp_pending);
    input_ring_destroy(&tcp_ring);

    const uint8_t replay_raw[] = {0U, 1U, 254U, 255U};
    for (int historical = 0; historical <= 1; historical++) {
        rtl_device replay_dev{};
        replay_dev.backend = RTL_BACKEND_IQ_REPLAY;
        replay_dev.replay_cfg.format = DSD_IQ_FORMAT_CU8;
        replay_dev.replay_fs4_shift_enabled = 1;
        replay_dev.replay_historical_cu8_two_pass = historical;
        uint8_t mutable_raw[sizeof(replay_raw)];
        DSD_MEMCPY(mutable_raw, replay_raw, sizeof(replay_raw));
        float converted[sizeof(replay_raw) + 1U] = {};
        int phase = 0;
        int have_carry = 1;
        uint8_t carry_byte = 77U;
        dsd_input_level_cu8_moments_reset(&out[7U + (size_t)historical]);
        int produced = replay_convert_cu8_to_f32(&replay_dev, mutable_raw, sizeof(mutable_raw), converted,
                                                 sizeof(converted) / sizeof(converted[0]), &phase, &have_carry,
                                                 &carry_byte, &out[7U + (size_t)historical]);
        if (produced <= 0) {
            return -2;
        }
    }
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
    if (dev && dev->backend == RTL_BACKEND_AIRSPY) {
        return DSD_IQ_FORMAT_CF32;
    }
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

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" int
rtl_device_test_public_capture_policy(int* out_formats, size_t out_formats_len, uint32_t* out_counts,
                                      size_t out_counts_len) {
    if (!out_formats || out_formats_len < 8U || !out_counts || out_counts_len < 5U) {
        return -1;
    }

    rtl_device dev{};

    out_formats[0] = rtl_device_get_native_sample_format(NULL);
    dev.backend = RTL_BACKEND_USB;
    out_formats[1] = rtl_device_get_native_sample_format(&dev);
    dev.backend = RTL_BACKEND_TCP;
    out_formats[2] = rtl_device_get_native_sample_format(&dev);
    dev.backend = RTL_BACKEND_SOAPY;
    dev.soapy_format = SOAPY_FMT_CF32;
    out_formats[3] = rtl_device_get_native_sample_format(&dev);
    dev.soapy_format = SOAPY_FMT_CS16;
    out_formats[4] = rtl_device_get_native_sample_format(&dev);
    dev.soapy_format = SOAPY_FMT_NONE;
    out_formats[5] = rtl_device_get_native_sample_format(&dev);
    dev.backend = RTL_BACKEND_IQ_REPLAY;
    dev.replay_cfg.format = DSD_IQ_FORMAT_CS16;
    out_formats[6] = rtl_device_get_native_sample_format(&dev);
    dev.backend = 99;
    out_formats[7] = rtl_device_get_native_sample_format(&dev);

    out_counts[0] = rtl_device_get_capture_retune_count(NULL);
    out_counts[1] = rtl_device_get_capture_retune_count(&dev);
    rtl_device_note_capture_retune(&dev);
    out_counts[2] = rtl_device_get_capture_retune_count(&dev);
    // Opaque non-null sentinel used only to exercise pointer-presence accounting.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    rtl_device_set_iq_capture_writer(&dev, reinterpret_cast<dsd_iq_capture_writer*>(static_cast<uintptr_t>(1U)));
    rtl_device_note_capture_retune(&dev);
    rtl_device_note_capture_retune(&dev);
    out_counts[3] = rtl_device_get_capture_retune_count(&dev);
    rtl_device_set_iq_capture_writer(&dev, NULL);
    out_counts[4] = rtl_device_get_capture_retune_count(&dev);
    return 0;
}
#endif
