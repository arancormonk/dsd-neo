// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A stalled RTL-SDR recovered in place (issue #518 B1, D1, D2), over a fake dongle and a fake USB device list (the real
 * dongles are never touched). The silence limit is 300 ms here, sampled every 20 ms.
 *
 * Device level, the test standing in for the stream's controller: the progress monitor of an eligible receiver posts
 * one recovery request per stall through the stream's callback, pauses while it is pending or running, and watches a
 * restarted reader afresh from its baseline. A request not taken in time, a second silence with no samples since the
 * reset and a stall inside the cooldown after the previous request end the session as #774 does; an ineligible
 * receiver, or one with no callback, ends exactly as before; a receiver verdict waiting out the unplug-owner grace when
 * the recovery starts stays silent.
 *
 * Stream level, through a real stream and its controller thread: the request becomes a stream boundary (gate, device
 * swap and replay, retune tail, output moved on and gap noted, reader restarted, gate opened), the tail runs on a
 * failure and a stop too, a failure ends the session with the step that failed, a stop is silent, samples after the
 * recovery reach the decoder and the I/Q capture, which records the gap once, and the decoder is told.
 */

#include <assert.h>
#include <atomic>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/stream_notice_hooks.h>
#include <errno.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "rtl_stream_test_support.h"
#include "rtl_usb_identity.h"
#include "test_support.h"

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

#ifdef DSD_HAVE_LIBUSB
constexpr int kHaveLibusb = 1;
#else
constexpr int kHaveLibusb = 0;
#endif

constexpr uint32_t kLimitMs = 300U;
constexpr uint32_t kPeriodMs = 20U;
/* A step that must land well inside a wait: far beyond any time a loaded runner keeps a thread off the CPU. */
constexpr uint64_t kWaitMs = 5000U;

/* ---- Fake USB device list (the identity unit's driver ops) ---- */

struct usb_dev {
    rtl_usb_device_info info;
    const char* serial;
    std::atomic<int> resets{0};
};

struct usb_world {
    usb_dev devs[2];
    int dev_count = 0;
    std::atomic<uint64_t> now_ms{1000U};
    /* Called by the reset on the thread running it (the controller), before it resets. */
    void (*on_reset)(void) = nullptr;
    /* The reset brings nothing back: the recovery fails. */
    int never_back = 0;
    int hidden = 0;
};

usb_world g_usb;

void
copy_str(char* dst, const char* src) {
    DSD_MEMSET(dst, 0, 256);
    const size_t n = strnlen(src, 255);
    DSD_MEMCPY(dst, src, n);
}

void
fill_strings(const usb_dev& d, char* m, char* p, char* s) {
    copy_str(m, "Realtek");
    copy_str(p, "RTL2838UHIDIR");
    copy_str(s, d.serial);
}

uint32_t
fake_rtl_count(void) {
    return g_usb.hidden != 0 ? 0U : (uint32_t)g_usb.dev_count;
}

int
fake_rtl_index_strings(uint32_t index, char* m, char* p, char* s) {
    if ((int)index >= g_usb.dev_count) {
        return -1;
    }
    fill_strings(g_usb.devs[index], m, p, s);
    return 0;
}

int
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the driver-ops table's.
fake_rtl_handle_strings(rtlsdr_dev_t* dev, char* m, char* p, char* s) {
    (void)dev;
    fill_strings(g_usb.devs[0], m, p, s);
    return 0;
}

int
fake_open_session(void** session, int* count) {
    *session = &g_usb;
    *count = g_usb.hidden != 0 ? 0 : g_usb.dev_count;
    return RTL_USB_OP_OK;
}

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the driver-ops table's.
fake_close_session(void* session) {
    (void)session;
}

int
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the driver-ops table's.
fake_describe(void* session, int i, rtl_usb_device_info* out) {
    (void)session;
    *out = g_usb.devs[i].info;
    return RTL_USB_OP_OK;
}

int
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the driver-ops table's.
fake_usb_open(void* session, int i, void** handle) {
    (void)session;
    *handle = &g_usb.devs[i];
    return RTL_USB_OP_OK;
}

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the driver-ops table's.
fake_usb_close(void* handle) {
    (void)handle;
}

int
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the driver-ops table's.
fake_read_strings(void* handle, char* m, char* p, char* s) {
    fill_strings(*static_cast<const usb_dev*>(handle), m, p, s);
    return RTL_USB_OP_OK;
}

int
fake_reset(void* handle) {
    auto* d = static_cast<usb_dev*>(handle);
    if (g_usb.on_reset != nullptr) {
        g_usb.on_reset();
    }
    d->resets.fetch_add(1);
    if (g_usb.never_back != 0) {
        g_usb.hidden = 1;
        return RTL_USB_OP_GONE;
    }
    return RTL_USB_OP_OK;
}

uint64_t
fake_now(void) {
    return g_usb.now_ms.load();
}

/* Rediscovery after a reset polls on this clock; real time passes too, so a stop can land while it waits. */
void
fake_sleep(unsigned int ms) {
    dsd_sleep_ms(1);
    g_usb.now_ms.fetch_add(ms);
}

const rtl_usb_driver_ops k_usb_ops = {
    fake_rtl_count,
    fake_rtl_index_strings,
    fake_rtl_handle_strings,
    fake_open_session,
    fake_close_session,
    fake_describe,
    fake_usb_open,
    fake_usb_close,
    fake_read_strings,
    fake_reset,
    fake_now,
    fake_sleep,
};

/* One dongle (eligible), or two with the same strings (not). */
void
usb_world_reset(int duplicate) {
    for (usb_dev& d : g_usb.devs) {
        d.info = rtl_usb_device_info{};
        d.serial = "";
        d.resets.store(0);
    }
    g_usb.on_reset = nullptr;
    g_usb.never_back = 0;
    g_usb.hidden = 0;
    usb_dev& a = g_usb.devs[0];
    a.info.vid = 0x0bda;
    a.info.pid = 0x2838;
    a.info.bus = 1;
    a.info.ports[0] = 2;
    a.info.port_depth = 1;
    a.info.vendor_specific = 1;
    a.serial = "56435444";
    g_usb.dev_count = 1;
    if (duplicate != 0) {
        usb_dev& b = g_usb.devs[1];
        b.info = a.info;
        b.info.bus = 3;
        b.serial = a.serial;
        g_usb.dev_count = 2;
    }
}

/* ---- Log tap ---- */

struct log_capture {
    dsd_mutex_t lock;
    std::string text;
};

log_capture g_log;

void
capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    auto* log = static_cast<log_capture*>(ctx);
    dsd_mutex_lock(&log->lock);
    log->text += (text != nullptr) ? text : "";
    dsd_mutex_unlock(&log->lock);
}

std::string
log_take(void) {
    dsd_mutex_lock(&g_log.lock);
    std::string out;
    out.swap(g_log.text);
    dsd_mutex_unlock(&g_log.lock);
    return out;
}

bool
log_has(const std::string& log, const char* needle) {
    if (log.find(needle) != std::string::npos) {
        return true;
    }
    DSD_FPRINTF(stderr, "missing \"%s\" in log:\n%s\n", needle, log.c_str());
    return false;
}

/* ---- Session ---- */

void
reset_session(void) {
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
}

void
expect_running(void) {
    assert(dsd_exitflag_load() == 0);
    assert(dsd_input_failure_receiver_ended() == 0);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_NONE);
}

void
expect_receiver_ending(int native_code) {
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    assert(failure.native_code == native_code);
    assert(dsd_exitflag_load() == 1);
    assert(dsd_input_failure_receiver_ended() == 1);
}

/* Poll until the receiver's ending is committed: the time it was seen, or 0 when @p timeout_ms passed first. */
uint64_t
wait_receiver_ended(uint64_t timeout_ms) {
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + timeout_ms;
    while (dsd_realtime_mono_ms() < deadline_ms) {
        if (dsd_input_failure_receiver_ended() != 0) {
            return dsd_realtime_mono_ms();
        }
        dsd_sleep_ms(2);
    }
    return 0U;
}

/* The notices the stream posted (the decoder's sink stands in). */
std::atomic<int> g_notices{0};

void
count_notice(int kind) {
    assert(kind == DSD_STREAM_NOTICE_USB_RECOVERED);
    g_notices.fetch_add(1);
}

/* ---- A device on the fake dongle, its controller played by the test ---- */

unsigned char g_buffer[16384];

const int k_gains[] = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};

void
dongle_init(rtl_device_test_usb_dongle* dongle) {
    DSD_MEMSET(dongle, 0, sizeof *dongle);
    dongle->tuner.tuner_type = 1; /* E4000 */
    dongle->gain.gain_count = (int)(sizeof k_gains / sizeof k_gains[0]);
    DSD_MEMCPY(dongle->gain.gains, k_gains, sizeof k_gains);
}

/* The recovery requests the device posted: how many, and when the last came. */
struct posts {
    std::atomic<int> count{0};
    std::atomic<uint64_t> last_ms{0U};
};

void
note_post(void* ctx) {
    auto* p = static_cast<posts*>(ctx);
    p->last_ms.store(dsd_realtime_mono_ms());
    p->count.fetch_add(1);
}

struct rig {
    std::unique_ptr<input_ring_state> ring;
    rtl_device_test_usb_dongle dongle{};
    rtl_device* dev = nullptr;
    posts posted;
};

void
rig_open(rig& r, int with_callback) {
    r.ring = std::make_unique<input_ring_state>();
    assert(input_ring_init(r.ring.get(), 1U << 18) == 0);
    dongle_init(&r.dongle);
    rtl_device_test_usb_dongle_install(&r.dongle);
    r.dev = rtl_device_create(0, r.ring.get());
    assert(r.dev != nullptr);
    assert(rtl_device_set_frequency(r.dev, 851012500U) == 0);
    assert(rtl_device_set_sample_rate(r.dev, 1536000U) == 0);
    assert(rtl_device_set_gain(r.dev, 240) == 0);
    if (with_callback != 0) {
        rtl_device_set_recovery_callback(r.dev, note_post, &r.posted);
    }
    rtl_device_test_usb_stream_attach();
    reset_session();
    assert(rtl_device_start_async(r.dev, 16U * 32U * 512U) == 0);
}

void
rig_close(rig& r) {
    rtl_device_destroy(r.dev);
    r.dev = nullptr;
    rtl_device_test_usb_stream_detach();
    rtl_device_test_usb_dongle_install(nullptr);
    assert(r.dongle.open_handles == 0);
    assert(r.dongle.stale_handle_calls == 0);
    input_ring_destroy(r.ring.get());
    reset_session();
}

void
deliver(const rig& r, int callbacks, unsigned int period_ms) {
    for (int i = 0; i < callbacks; ++i) {
        if (i > 0) {
            dsd_sleep_ms(period_ms);
        }
        rtl_device_test_usb_stream_deliver(r.dev, g_buffer, 512U);
    }
}

/* Wait until the device posted its @p n-th request; the time it did. */
uint64_t
wait_posted(const rig& r, int n) {
    for (uint64_t waited = 0; waited < kWaitMs && r.posted.count.load() < n; ++waited) {
        dsd_sleep_ms(1);
    }
    assert(r.posted.count.load() >= n);
    return r.posted.last_ms.load();
}

/* What the stream's controller does with a request: take it, reset and reopen, restart the reader, report. */
int
play_controller(const rig& r) {
    assert(rtl_device_usb_recovery_take(r.dev) == 1);
    char why[256] = {};
    int rc = rtl_device_usb_recover_handle(r.dev, why, sizeof why);
    if (rc == 0) {
        rc = rtl_device_usb_resume_reader(r.dev);
    }
    rtl_device_usb_recovery_finish(r.dev, rc, why);
    return rc;
}

/* ---- Device-level tests ---- */

/* A request the controller does not take within the bound is withdrawn and ends the session; it cannot be taken
 * afterwards and nothing is reset. */
void
test_request_not_taken_ends_session(void) {
    usb_world_reset(0);
    rtl_device_test_set_recovery_take_ms(400U);
    (void)log_take();
    rig r;
    rig_open(r, 1);
    deliver(r, 5, 10U);
    const uint64_t posted_ms = wait_posted(r, 1);
    const uint64_t ended_ms = wait_receiver_ended(kWaitMs);
    assert(ended_ms != 0U);
    assert(ended_ms - posted_ms >= 400U - kPeriodMs);
    expect_receiver_ending(-ETIMEDOUT);
    assert(log_has(log_take(), "and its reset did not start within 400 ms; the stream stops"));
    assert(rtl_device_usb_recovery_take(r.dev) == 0);
    assert(g_usb.devs[0].resets.load() == 0);
    assert(r.posted.count.load() == 1);
    rig_close(r);
    rtl_device_test_set_recovery_take_ms(0U);
}

/* A recovery whose restarted reader delivers nothing is the same stall: the second silence ends the session, however
 * short the cooldown. */
void
test_second_stall_without_samples_ends_session(void) {
    usb_world_reset(0);
    rtl_device_test_set_recovery_cooldown_ms(1U);
    (void)log_take();
    rig r;
    rig_open(r, 1);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    assert(play_controller(r) == 0);
    expect_running();
    assert(g_usb.devs[0].resets.load() == 1);
    assert(wait_receiver_ended(kWaitMs) != 0U);
    expect_receiver_ending(-ETIMEDOUT);
    const std::string log = log_take();
    assert(log_has(log, "RTL-SDR: the receiver sent no samples for "));
    assert(log_has(log, " ms; resetting the USB device"));
    assert(log_has(log, " ms, none since it was reset and reopened; the stream stops"));
    assert(r.posted.count.load() == 1);
    rig_close(r);
    rtl_device_test_set_recovery_cooldown_ms(0U);
}

/* The cooldown, on both sides: a new stall (samples came after the reset) inside it ends the session; one after it is
 * recovered again. */
void
test_cooldown_both_sides(void) {
    constexpr uint32_t kCooldownMs = 1500U;
    rtl_device_test_set_recovery_cooldown_ms(kCooldownMs);

    /* Inside: samples for 100 ms after the reset, then silence. */
    usb_world_reset(0);
    (void)log_take();
    rig inside;
    rig_open(inside, 1);
    deliver(inside, 5, 10U);
    const uint64_t first_ms = wait_posted(inside, 1);
    assert(play_controller(inside) == 0);
    deliver(inside, 10, 10U);
    const uint64_t ended_ms = wait_receiver_ended(kWaitMs);
    assert(ended_ms != 0U);
    assert(ended_ms - first_ms < kCooldownMs);
    expect_receiver_ending(-ETIMEDOUT);
    assert(log_has(log_take(), " ms after it was last reset; the stream stops"));
    assert(inside.posted.count.load() == 1);
    rig_close(inside);

    /* Outside: samples until the cooldown has run out, then silence: a second recovery. */
    usb_world_reset(0);
    rig outside;
    rig_open(outside, 1);
    deliver(outside, 5, 10U);
    const uint64_t posted_ms = wait_posted(outside, 1);
    assert(play_controller(outside) == 0);
    while (dsd_realtime_mono_ms() - posted_ms < kCooldownMs + 100U) {
        deliver(outside, 1, 0U);
        dsd_sleep_ms(10U);
    }
    const uint64_t second_ms = wait_posted(outside, 2);
    assert(second_ms - posted_ms >= kCooldownMs);
    expect_running();
    assert(play_controller(outside) == 0);
    expect_running();
    assert(g_usb.devs[0].resets.load() == 2);
    rig_close(outside);
    rtl_device_test_set_recovery_cooldown_ms(0U);
}

/* The watch pauses while the request runs (here three limits: no verdict is made) and restarts with nothing accrued:
 * first samples that come two thirds of a limit after the reader restarted keep the session running. */
void
test_rearm_with_delayed_first_samples(void) {
    usb_world_reset(0);
    rig r;
    rig_open(r, 1);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    assert(rtl_device_usb_recovery_take(r.dev) == 1);
    /* With a grace set, any verdict the monitor made meanwhile would be counted. */
    rtl_device_set_unplug_owner_grace_ms(kLimitMs);
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    dsd_sleep_ms(3U * kLimitMs);
    assert(rtl_device_test_verdicts_in_grace() == in_grace);
    rtl_device_set_unplug_owner_grace_ms(0U);
    expect_running();
    char why[256] = {};
    assert(rtl_device_usb_recover_handle(r.dev, why, sizeof why) == 0);
    assert(rtl_device_usb_resume_reader(r.dev) == 0);
    rtl_device_usb_recovery_finish(r.dev, 0, why);
    dsd_sleep_ms(2U * kLimitMs / 3U);
    deliver(r, 3U * kLimitMs / 10U, 10U);
    expect_running();
    assert(r.posted.count.load() == 1);
    rig_close(r);
}

/* Callbacks between the restarted reader's baseline and its thread start are samples since the reset: the silence
 * after them is a new stall (here recovered again: the cooldown is 1 ms), not the same one. */
void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the restart hook's.
deliver_one_at_restart(rtl_device* dev, void* ctx) {
    (void)ctx;
    rtl_device_test_usb_stream_deliver(dev, g_buffer, 512U);
}

void
test_callbacks_before_thread_start_count_for_rearm(void) {
    usb_world_reset(0);
    rtl_device_test_set_recovery_cooldown_ms(1U);
    rig r;
    rig_open(r, 1);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    rtl_device_test_set_reader_restart_hook(deliver_one_at_restart, nullptr);
    assert(play_controller(r) == 0);
    rtl_device_test_set_reader_restart_hook(nullptr, nullptr);
    (void)wait_posted(r, 2);
    expect_running();
    assert(play_controller(r) == 0);
    expect_running();
    rig_close(r);
    rtl_device_test_set_recovery_cooldown_ms(0U);
}

/* A receiver whose identity is not unique, or whose stream registered no recovery, ends as #774 did: the same cause,
 * no request. */
void
test_ineligible_device_ends_as_before(void) {
    for (int duplicate = 1; duplicate >= 0; --duplicate) {
        usb_world_reset(duplicate);
        (void)log_take();
        rig r;
        rig_open(r, duplicate);
        assert(rtl_device_usb_recovery_eligible(r.dev) == (duplicate == 0 ? kHaveLibusb : 0));
        deliver(r, 5, 10U);
        assert(wait_receiver_ended(kWaitMs) != 0U);
        expect_receiver_ending(-ETIMEDOUT);
        const std::string log = log_take();
        assert(log_has(log, " ms; the stream stops"));
        assert(log.find("resetting the USB device") == std::string::npos);
        assert(log.find(" since it was reset") == std::string::npos);
        assert(r.posted.count.load() == 0);
        assert(g_usb.devs[0].resets.load() == 0);
        rig_close(r);
    }
}

/* A receiver verdict already waiting out the unplug-owner grace when the recovery is requested (the reader returned by
 * itself just before) stays silent once the recovery succeeded. */
void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the post hook's.
reader_returns_before_post(rtl_device* dev, void* ctx) {
    (void)dev;
    (void)ctx;
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    rtl_device_test_usb_stream_return(-1);
    for (uint64_t waited = 0; waited < kWaitMs && rtl_device_test_verdicts_in_grace() == in_grace; ++waited) {
        dsd_sleep_ms(1);
    }
    assert(rtl_device_test_verdicts_in_grace() == in_grace + 1U);
}

void
test_verdict_in_grace_yields_to_recovery(void) {
    constexpr uint32_t kGraceMs = 1000U;
    usb_world_reset(0);
    rtl_device_set_unplug_owner_grace_ms(kGraceMs);
    rtl_device_test_set_recovery_post_hook(reader_returns_before_post, nullptr);
    rig r;
    rig_open(r, 1);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    rtl_device_test_set_recovery_post_hook(nullptr, nullptr);
    /* No samples until the grace ran out, so only the recovery can keep the verdict silent; the restarted reader's
       own silence must not end the session meanwhile. */
    rtl_device_test_set_progress_limit_ms(60000U);
    assert(play_controller(r) == 0);
    dsd_sleep_ms(kGraceMs + kGraceMs / 2U);
    expect_running();
    deliver(r, 5, 10U);
    expect_running();
    rig_close(r);
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

/* ---- Stream level: the real controller ---- */

/* What the probe saw at each step of the stream boundary. */
struct step_view {
    int step;
    int gate_closed;
    int capture_held;
    int has_handle;
    int thread_started;
    uint64_t gap_seq;
    uint32_t generation;
};

struct boundary_log {
    dsd_mutex_t lock;
    std::vector<step_view> steps;
};

boundary_log g_boundary;

void
// NOLINTNEXTLINE(misc-const-correctness) -- The signature is the boundary probe's.
probe_boundary(int step, void* ctx) {
    (void)ctx;
    step_view v{};
    v.step = step;
    v.gate_closed = rtl_stream_test_reconfigure_gate_closed();
    rtl_device* dev = rtl_stream_test_device();
    v.capture_held = rtl_device_test_capture_reconfigure_held(dev);
    rtl_device_test_usb_swap_state st{};
    rtl_device_test_usb_swap_state_get(dev, &st);
    v.has_handle = st.has_handle;
    v.thread_started = st.thread_started;
    v.gap_seq = rtl_stream_discontinuity_seq();
    v.generation = rtl_stream_output_generation();
    dsd_mutex_lock(&g_boundary.lock);
    g_boundary.steps.push_back(v);
    dsd_mutex_unlock(&g_boundary.lock);
}

std::vector<step_view>
boundary_take(void) {
    dsd_mutex_lock(&g_boundary.lock);
    std::vector<step_view> out;
    out.swap(g_boundary.steps);
    dsd_mutex_unlock(&g_boundary.lock);
    return out;
}

/* The steps, in order, the boundary ran. */
std::vector<int>
step_ids(const std::vector<step_view>& steps) {
    std::vector<int> ids;
    ids.reserve(steps.size());
    for (const step_view& v : steps) {
        ids.push_back(v.step);
    }
    return ids;
}

/* A thread reading the stream's output as the decoder does, until it is told to quit (and a read returns) or a read
 * fails (the session ended). The stream is stopped only once it is joined: the decoder reads and stops its stream on
 * one thread. */
struct output_reader {
    RtlSdrContext* ctx = nullptr;
    std::atomic<uint64_t> samples{0U};
    std::atomic<int> quit{0};
    std::atomic<int> done{0};
    int started = 0;
    dsd_thread_t thread{};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
read_output(void* arg) {
    auto* reader = static_cast<output_reader*>(arg);
    std::vector<float> block(4096U);
    while (reader->quit.load() == 0) {
        int got = 0;
        if (rtl_stream_read(reader->ctx, block.data(), block.size(), &got) != 0 || got < 0) {
            break;
        }
        reader->samples.fetch_add((uint64_t)got);
    }
    reader->done.store(1);
    DSD_THREAD_RETURN;
}

struct stream_rig {
    rtl_device_test_usb_dongle dongle{};
    std::unique_ptr<dsd_opts> opts;
    RtlSdrContext* ctx = nullptr;
    rtl_device* dev = nullptr;
    output_reader reader;
    char capture_dir[DSD_TEST_PATH_MAX] = {};
    char capture_meta[DSD_TEST_PATH_MAX] = {};
};

const char* const kCaptureFiles[] = {"capture.iq", "capture.iq.json", nullptr};

void
stream_open(stream_rig& r, int capture, int reader) {
    dongle_init(&r.dongle);
    rtl_device_test_usb_dongle_install(&r.dongle);
    rtl_device_test_usb_stream_attach();
    r.opts = std::make_unique<dsd_opts>();
    dsd_opts* opts = r.opts.get();
    DSD_MEMSET(opts, 0, sizeof *opts);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl");
    opts->rtl_dev_index = 0;
    opts->rtlsdr_center_freq = 851012500U;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_gain_value = 24;
    opts->frame_dmr = 1;
    if (capture != 0) {
        assert(dsd_test_mkdtemp(r.capture_dir, sizeof r.capture_dir, "dsdneo_usb_recovery") != nullptr);
        char data_path[DSD_TEST_PATH_MAX];
        assert(dsd_test_path_join(data_path, sizeof data_path, r.capture_dir, "capture.iq") == 0);
        assert(dsd_test_path_join(r.capture_meta, sizeof r.capture_meta, r.capture_dir, "capture.iq.json") == 0);
        opts->iq_capture_requested = 1;
        opts->iq_capture_format = DSD_IQ_FORMAT_CU8;
        DSD_SNPRINTF(opts->iq_capture_path, sizeof opts->iq_capture_path, "%s", data_path);
    }
    reset_session();
    assert(rtl_stream_create(opts, &r.ctx) == 0 && r.ctx != nullptr);
    assert(rtl_stream_start(r.ctx) == 0);
    r.dev = rtl_stream_test_device();
    assert(r.dev != nullptr);
    assert(rtl_device_usb_recovery_eligible(r.dev) == 1);
    if (reader != 0) {
        r.reader.ctx = r.ctx;
        const int created = dsd_thread_create(&r.reader.thread, read_output, &r.reader);
        assert(created == 0);
        r.reader.started = 1;
    }
}

void stream_deliver(const stream_rig& r, int callbacks, unsigned int period_ms);

/* Let the reader quit: samples wake a read waiting for them, and a read in a stream that ended returns at once. */
void
stop_reader(stream_rig& r) {
    if (r.reader.started == 0) {
        return;
    }
    r.reader.quit.store(1);
    for (uint64_t waited = 0; waited < kWaitMs && r.reader.done.load() == 0; waited += 5U) {
        if (dsd_exitflag_load() == 0) {
            stream_deliver(r, 1, 0U);
        }
        dsd_sleep_ms(5U);
    }
    assert(r.reader.done.load() == 1);
    dsd_thread_join(r.reader.thread);
    r.reader.started = 0;
}

void
stream_close(stream_rig& r) {
    stop_reader(r);
    (void)rtl_stream_stop(r.ctx);
    (void)rtl_stream_destroy(r.ctx);
    r.ctx = nullptr;
    r.dev = nullptr;
    rtl_device_test_usb_stream_detach();
    rtl_device_test_usb_dongle_install(nullptr);
    assert(r.dongle.open_handles == 0);
    assert(r.dongle.stale_handle_calls == 0);
}

void
stream_deliver(const stream_rig& r, int callbacks, unsigned int period_ms) {
    for (int i = 0; i < callbacks; ++i) {
        if (i > 0) {
            dsd_sleep_ms(period_ms);
        }
        rtl_device_test_usb_stream_deliver(r.dev, g_buffer, (uint32_t)sizeof g_buffer);
    }
}

/* Wait until the boundary probe saw @p step. */
void
wait_step(int step) {
    for (uint64_t waited = 0; waited < kWaitMs; ++waited) {
        dsd_mutex_lock(&g_boundary.lock);
        bool seen = false;
        for (const step_view& v : g_boundary.steps) {
            seen = seen || v.step == step;
        }
        dsd_mutex_unlock(&g_boundary.lock);
        if (seen) {
            return;
        }
        dsd_sleep_ms(1);
    }
    assert(0 && "the stream boundary never reached the step");
}

/* recovery_transaction_order: the stream boundary ran its steps in order, the gate closed and the capture held through
 * the swap, the tail released the hold, and the gate opened last, after the reader restarted. */
void
check_transaction_order(const std::vector<step_view>& steps) {
    const std::vector<int> want = {
        RTL_STREAM_TEST_BOUNDARY_GATE,          RTL_STREAM_TEST_BOUNDARY_DEVICE_OP, RTL_STREAM_TEST_BOUNDARY_TAIL,
        RTL_STREAM_TEST_BOUNDARY_DISCONTINUITY, RTL_STREAM_TEST_BOUNDARY_RESUME,    RTL_STREAM_TEST_BOUNDARY_END,
    };
    assert(step_ids(steps) == want);
    assert(steps[0].gate_closed && steps[0].capture_held && steps[0].has_handle);
    assert(steps[1].gate_closed && steps[1].capture_held && steps[1].has_handle && !steps[1].thread_started);
    assert(steps[2].gate_closed && !steps[2].capture_held && !steps[2].thread_started);
    assert(steps[4].gate_closed && steps[4].thread_started);
    assert(!steps[5].gate_closed && steps[5].thread_started);
}

/* no_post_gap_sample_before_discontinuity: until the gap is noted the reader stays stopped; the output generation moves
 * (nothing read before the gap is taken after it) and the count moves before the reader restarts. */
void
check_no_post_gap_sample_before_discontinuity(const std::vector<step_view>& steps, uint64_t gap_before) {
    const step_view& tail = steps[2];
    const step_view& gap = steps[3];
    const step_view& resumed = steps[4];
    assert(tail.gap_seq == gap_before && !tail.thread_started);
    assert(!gap.thread_started);
    assert(gap.generation != tail.generation);
    assert(gap.gap_seq == gap_before + 1U);
    assert(resumed.gap_seq == gap_before + 1U);
    assert(rtl_stream_discontinuity_seq() == gap_before + 1U);
}

/* capture_events_retune_then_reset: the capture holds one RETUNE then one RESET at the gap, both named usb_recovery (no
 * pair of the boundary's own), and resumed_samples_reach_ring_and_capture: samples after them. */
void
check_capture_events_retune_then_reset(const char* metadata_path) {
    dsd_iq_replay_config cfg{};
    char err[256] = {};
    assert(dsd_iq_replay_read_metadata(metadata_path, &cfg, err, sizeof err) == 0);
    int gap_retunes = 0;
    int gap_resets = 0;
    uint64_t gap_offset = 0U;
    for (uint32_t i = 0; i < cfg.event_count; ++i) {
        const dsd_iq_event& ev = cfg.events[i];
        if (ev.kind != DSD_IQ_EVENT_RETUNE && ev.kind != DSD_IQ_EVENT_RESET) {
            continue;
        }
        assert(strcmp(ev.reason, RTL_STREAM_GAP_REASON_USB_RECOVERY) == 0);
        if (ev.kind == DSD_IQ_EVENT_RETUNE) {
            assert(gap_retunes == 0 && gap_resets == 0);
            gap_retunes++;
            gap_offset = ev.byte_offset;
        } else {
            assert(gap_retunes == 1 && ev.byte_offset == gap_offset);
            gap_resets++;
        }
    }
    assert(gap_retunes == 1 && gap_resets == 1);
    assert(gap_offset > 0U);
    assert(cfg.data_bytes > gap_offset);
    dsd_iq_replay_config_clear(&cfg);
}

/* silence_posts_recovery_and_reader_return_is_silent: a stall on a running stream, with an I/Q capture. The monitor
 * posts, the controller resets and reopens the dongle under the stream, the old reader's return reports nothing, the
 * session keeps running and the decoder is told once. Samples after the recovery reach the decoder and the capture. */
void
test_silence_posts_recovery_and_reader_return_is_silent(void) {
    usb_world_reset(0);
    (void)log_take();
    g_notices.store(0);
    (void)boundary_take();
    stream_rig r;
    stream_open(r, 1, 1);
    stream_deliver(r, 40, 5U);
    const uint64_t gap_before = rtl_stream_discontinuity_seq();
    wait_step(RTL_STREAM_TEST_BOUNDARY_END);
    /* No second stall from here: the test feeds the stream at its own pace. */
    rtl_device_test_set_progress_limit_ms(60000U);
    expect_running();
    assert(g_usb.devs[0].resets.load() == 1);
    assert(g_notices.load() == 1);
    const std::string log = log_take();
    assert(log_has(log, " ms; resetting the USB device"));
    assert(log_has(log, "RTL-SDR: the USB device was reset and reopened; the stream resumes"));
    assert(log.find("the stream stops") == std::string::npos);
    assert(log.find("reader stopped unexpectedly") == std::string::npos);
    const std::vector<step_view> steps = boundary_take();
    check_transaction_order(steps);
    check_no_post_gap_sample_before_discontinuity(steps, gap_before);

    const uint64_t read_before = r.reader.samples.load();
    for (int i = 0; i < 60 && r.reader.samples.load() == read_before; ++i) {
        stream_deliver(r, 4, 5U);
    }
    assert(r.reader.samples.load() > read_before);
    expect_running();
    stream_close(r);
    expect_running();
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    check_capture_events_retune_then_reset(r.capture_meta);
    assert(dsd_test_remove_temp_dir(r.capture_dir, kCaptureFiles) == 0);
}

/* The dongle does not come back after the reset: the boundary runs its tail (hold released, gate opened) without
 * noting a gap or restarting the reader, and the session ends for the receiver with the step that failed. */
void
test_recovery_failure_runs_the_tail_and_ends(void) {
    usb_world_reset(0);
    g_usb.never_back = 1;
    (void)log_take();
    g_notices.store(0);
    (void)boundary_take();
    stream_rig r;
    stream_open(r, 0, 1);
    stream_deliver(r, 20, 5U);
    const uint64_t gap_before = rtl_stream_discontinuity_seq();
    assert(wait_receiver_ended(kWaitMs + kWaitMs) != 0U);
    wait_step(RTL_STREAM_TEST_BOUNDARY_END);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    assert(dsd_exitflag_load() == 1);
    const std::string log = log_take();
    assert(log_has(log, "RTL-SDR: the receiver sent no samples and resetting it failed: the USB reset failed"));
    const std::vector<step_view> steps = boundary_take();
    const std::vector<int> want = {
        RTL_STREAM_TEST_BOUNDARY_GATE,
        RTL_STREAM_TEST_BOUNDARY_DEVICE_OP,
        RTL_STREAM_TEST_BOUNDARY_TAIL,
        RTL_STREAM_TEST_BOUNDARY_END,
    };
    assert(step_ids(steps) == want);
    assert(!steps[2].capture_held && steps[2].gate_closed && !steps[2].has_handle);
    assert(!steps[3].gate_closed && !steps[3].thread_started);
    assert(rtl_stream_discontinuity_seq() == gap_before);
    assert(g_notices.load() == 0);
    stream_close(r);
    reset_session();
}

/* A stop while the dongle is being reset: the recovery gives up, the tail runs, nothing restarts, and the session
 * reports nothing. */
std::atomic<int> g_stop_requested{0};

void
wait_for_stop_at_reset(void) {
    g_stop_requested.store(1);
    rtl_device* dev = rtl_stream_test_device();
    for (uint64_t waited = 0; waited < kWaitMs; ++waited) {
        rtl_device_test_usb_swap_state st{};
        rtl_device_test_usb_swap_state_get(dev, &st);
        if (st.stop_requested != 0) {
            return;
        }
        dsd_sleep_ms(1);
    }
    assert(0 && "the stop never latched");
}

void
test_stop_during_recovery_is_silent(void) {
    usb_world_reset(0);
    g_usb.on_reset = wait_for_stop_at_reset;
    g_stop_requested.store(0);
    (void)log_take();
    g_notices.store(0);
    (void)boundary_take();
    stream_rig r;
    stream_open(r, 0, 0);
    stream_deliver(r, 20, 5U);
    for (uint64_t waited = 0; waited < kWaitMs && g_stop_requested.load() == 0; ++waited) {
        dsd_sleep_ms(1);
    }
    assert(g_stop_requested.load() == 1);
    stream_close(r);
    expect_running();
    const std::vector<step_view> steps = boundary_take();
    const std::vector<int> want = {
        RTL_STREAM_TEST_BOUNDARY_GATE,
        RTL_STREAM_TEST_BOUNDARY_DEVICE_OP,
        RTL_STREAM_TEST_BOUNDARY_TAIL,
        RTL_STREAM_TEST_BOUNDARY_END,
    };
    assert(step_ids(steps) == want);
    assert(!steps[2].capture_held);
    assert(!steps[3].gate_closed && !steps[3].thread_started);
    assert(g_notices.load() == 0);
    const std::string log = log_take();
    assert(log.find("the stream stops") == std::string::npos);
    assert(log.find("resetting it failed") == std::string::npos);
    g_usb.on_reset = nullptr;
}

} // namespace

int
main(void) {
    assert(dsd_mutex_init(&g_log.lock) == 0);
    assert(dsd_mutex_init(&g_boundary.lock) == 0);
    dsd_neo_log_set_tap(capture_log, &g_log);
    rtl_usb_identity_set_driver_ops(&k_usb_ops);
    dsd_runtime_stream_notice_set(count_notice);
    for (size_t i = 0; i < sizeof g_buffer; ++i) {
        g_buffer[i] = (unsigned char)(127U + (i & 1U));
    }
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_test_set_progress_period_ms(kPeriodMs);

    test_ineligible_device_ends_as_before();
    /* Without libusb no receiver is eligible; the case above is all there is. */
    if (kHaveLibusb != 0) {
        test_request_not_taken_ends_session();
        test_second_stall_without_samples_ends_session();
        test_cooldown_both_sides();
        test_rearm_with_delayed_first_samples();
        test_callbacks_before_thread_start_count_for_rearm();
        test_verdict_in_grace_yields_to_recovery();

        rtl_stream_test_set_stream_boundary_probe(probe_boundary, nullptr);
        test_silence_posts_recovery_and_reader_return_is_silent();
        test_recovery_failure_runs_the_tail_and_ends();
        test_stop_during_recovery_is_silent();
        rtl_stream_test_set_stream_boundary_probe(nullptr, nullptr);
    }

    dsd_runtime_stream_notice_set(nullptr);
    rtl_device_test_set_progress_limit_ms(0U);
    rtl_device_test_set_progress_period_ms(0U);
    rtl_usb_identity_set_driver_ops(nullptr);
    dsd_neo_log_set_tap(nullptr, nullptr);
    DSD_FPRINTF(stderr, "IO_RTL_USB_RECOVERY: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
