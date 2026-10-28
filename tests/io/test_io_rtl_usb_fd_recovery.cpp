// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * #782: a stalled RTL-SDR opened from an Android descriptor, recovered in place and owing a reset before its next open,
 * over a fake dongle and the descriptor seams (the open, USBDEVFS_RESET and the identity read never reach a real
 * descriptor). The silence limit is 300 ms here, sampled every 20 ms; the test stands in for the stream's controller.
 *
 * - The obligation is bound to the descriptor the host published it with: a replacement publish clears or re-binds it,
 *   and the open that takes that descriptor resets it first and consumes it, or keeps it when the reset is refused.
 * - The swap closes the old handle, resets through the descriptor, opens the same descriptor again and replays the
 *   settings, in that order; a refused reset reopens without one, and a descriptor the reset left unusable, or one that
 *   no longer opens, fails the recovery and ends the session for the receiver.
 * - Before the recovery, the unplug-owner grace: samples inside it drop the recovery, a stop or a detach ends it, and the
 *   obligation is set at the verdict, before it, so a Stop inside it keeps the obligation for the next open.
 * - A stop that lands just before the reset cancels the recovery silently, with the obligation kept.
 */

#include <assert.h>
#include <atomic>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "rtl_stream_test_support.h"
#include "rtl_usb_identity.h"

extern "C" void rtl_device_test_set_open_fd_failure_hook(int (*hook)(int));

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

constexpr uint32_t kLimitMs = 300U;
constexpr uint32_t kPeriodMs = 20U;
constexpr uint32_t kGraceMs = 400U;
constexpr uint64_t kWaitMs = 5000U;
constexpr int kFd = 31;

/* ---- What reached the dongle and the descriptor, in order ---- */

enum : int {
    EV_RESET = -1,   /* USBDEVFS_RESET on the descriptor */
    EV_OPEN_FD = -2, /* rtlsdr_open_fd() on the descriptor */
};

struct events {
    std::mutex lock;
    std::vector<int> seen;
    std::vector<int> reset_fds;
    std::atomic<int> recording{0};
};

events g_events;

void
note_event(int ev) {
    if (g_events.recording.load() == 0) {
        return;
    }
    const std::lock_guard<std::mutex> guard(g_events.lock);
    g_events.seen.push_back(ev);
}

std::vector<int>
events_take(void) {
    const std::lock_guard<std::mutex> guard(g_events.lock);
    std::vector<int> out;
    out.swap(g_events.seen);
    return out;
}

/* Where @p ev first appears in @p seen, or -1. */
int
first_at(const std::vector<int>& seen, int ev) {
    for (size_t i = 0; i < seen.size(); ++i) {
        if (seen[i] == ev) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- The descriptor seams ---- */

std::atomic<int> g_reset_answer{0};
std::atomic<int> g_resets{0};
std::atomic<int> g_open_answer{1};
std::atomic<int> g_opens{0};
std::atomic<int> g_identity_calls{0};
/* From this identity read on, the descriptor leads to a dongle with another serial (0: never). */
std::atomic<int> g_identity_other_from{0};
/* Called by the reset seam before it answers, on the thread resetting. */
void (*g_on_reset)(void) = nullptr;

int
fake_fd_reset(int fd) {
    note_event(EV_RESET);
    {
        const std::lock_guard<std::mutex> guard(g_events.lock);
        g_events.reset_fds.push_back(fd);
    }
    if (g_on_reset != nullptr) {
        g_on_reset();
    }
    g_resets.fetch_add(1);
    return g_reset_answer.load();
}

/* A positive answer opens the fake dongle as the device behind the descriptor. */
int
fake_open_fd(int fd) {
    (void)fd;
    note_event(EV_OPEN_FD);
    g_opens.fetch_add(1);
    return g_open_answer.load();
}

int
fake_fd_identity(int fd, rtl_usb_identity* out) {
    (void)fd;
    const int call = g_identity_calls.fetch_add(1) + 1;
    out->vid = 0x0bda;
    out->pid = 0x2838;
    DSD_SNPRINTF(out->manufacturer, sizeof out->manufacturer, "%s", "Nooelec");
    DSD_SNPRINTF(out->product, sizeof out->product, "%s", "NESDR SMArt v5");
    const int other = g_identity_other_from.load();
    DSD_SNPRINTF(out->serial, sizeof out->serial, "%s", (other != 0 && call >= other) ? "00000002" : "00000001");
    return 0;
}

std::vector<int>
reset_fds_take(void) {
    const std::lock_guard<std::mutex> guard(g_events.lock);
    std::vector<int> out;
    out.swap(g_events.reset_fds);
    return out;
}

void
seams_reset(void) {
    g_reset_answer.store(0);
    g_resets.store(0);
    g_open_answer.store(1);
    g_opens.store(0);
    g_identity_calls.store(0);
    g_identity_other_from.store(0);
    g_on_reset = nullptr;
    g_events.recording.store(0);
    (void)events_take();
    (void)reset_fds_take();
}

/* ---- Log tap ---- */

struct log_capture {
    std::mutex lock;
    std::string text;
};

log_capture g_log;

void
capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    auto* log = static_cast<log_capture*>(ctx);
    const std::lock_guard<std::mutex> guard(log->lock);
    log->text += (text != nullptr) ? text : "";
}

std::string
log_take(void) {
    const std::lock_guard<std::mutex> guard(g_log.lock);
    std::string out;
    out.swap(g_log.text);
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
}

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

void
expect_receiver_ending(int native_code) {
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    assert(failure.native_code == native_code);
    assert(dsd_input_failure_receiver_ended() == 1);
}

/* ---- A device opened from the descriptor, its controller played by the test ---- */

unsigned char g_buffer[16384];
const int k_gains[] = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};

void
dongle_on_op(void* ctx, int op) {
    (void)ctx;
    note_event(op);
}

void
dongle_init(rtl_device_test_usb_dongle* dongle) {
    DSD_MEMSET(dongle, 0, sizeof *dongle);
    dongle->tuner.tuner_type = 1; /* E4000 */
    dongle->gain.gain_count = (int)(sizeof k_gains / sizeof k_gains[0]);
    DSD_MEMCPY(dongle->gain.gains, k_gains, sizeof k_gains);
    dongle->on_op = dongle_on_op;
}

struct posts {
    std::atomic<int> count{0};
    std::atomic<uint64_t> last_ms{0U};
    std::atomic<int> owed_at_post{-2};
};

void
note_post(void* ctx) {
    auto* p = static_cast<posts*>(ctx);
    p->owed_at_post.store(rtl_device_preopened_fd_reset_owed());
    p->last_ms.store(dsd_realtime_mono_ms());
    p->count.fetch_add(1);
}

struct rig {
    std::unique_ptr<input_ring_state> ring;
    std::unique_ptr<rtl_device_test_usb_dongle> dongle = std::make_unique<rtl_device_test_usb_dongle>();
    rtl_device* dev = nullptr;
    posts posted;
};

void
rig_open(rig& r) {
    r.ring = std::make_unique<input_ring_state>();
    assert(input_ring_init(r.ring.get(), 1U << 18) == 0);
    dongle_init(r.dongle.get());
    rtl_device_test_usb_dongle_install(r.dongle.get());
    rtl_device_set_preopened_fd(kFd);
    r.dev = rtl_device_create(0, r.ring.get());
    assert(r.dev != nullptr);
    assert(rtl_device_usb_recovery_eligible(r.dev) == 1);
    assert(rtl_device_set_frequency(r.dev, 851012500U) == 0);
    assert(rtl_device_set_sample_rate(r.dev, 1536000U) == 0);
    assert(rtl_device_set_gain(r.dev, 240) == 0);
    rtl_device_set_recovery_callback(r.dev, note_post, &r.posted);
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
    assert(r.dongle->open_handles == 0);
    assert(r.dongle->stale_handle_calls == 0);
    assert(rtl_device_preopened_fd_held() == -1);
    input_ring_destroy(r.ring.get());
    rtl_device_set_preopened_fd(-1);
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

uint64_t
wait_posted(const rig& r, int n) {
    for (uint64_t waited = 0; waited < kWaitMs && r.posted.count.load() < n; ++waited) {
        dsd_sleep_ms(1);
    }
    assert(r.posted.count.load() >= n);
    return r.posted.last_ms.load();
}

/* Wait until a stall recovery began its grace (the @p n-th ever); the time it was seen. */
uint64_t
wait_in_grace(unsigned int n) {
    for (uint64_t waited = 0; waited < kWaitMs && rtl_device_test_recoveries_in_grace() < n; ++waited) {
        dsd_sleep_ms(1);
    }
    assert(rtl_device_test_recoveries_in_grace() >= n);
    return dsd_realtime_mono_ms();
}

/* The stream's controller: take the request, reset and reopen, restart the reader, report. */
int
play_controller(const rig& r, char* why, size_t why_size) {
    assert(rtl_device_usb_recovery_take(r.dev) == 1);
    int rc = rtl_device_usb_recover_handle(r.dev, why, why_size);
    if (rc == 0) {
        rc = rtl_device_usb_resume_reader(r.dev);
    }
    rtl_device_usb_recovery_finish(r.dev, rc, why);
    return rc;
}

rtl_device_test_usb_swap_state
swap_state(const rig& r) {
    rtl_device_test_usb_swap_state st{};
    rtl_device_test_usb_swap_state_get(r.dev, &st);
    return st;
}

/* ---- The obligation and the open ---- */

/* Task 5 review: the reset request is bound to the descriptor it was published with. A replacement publish clears or
 * re-binds it, so it can never reach another descriptor's open. */
void
test_obligation_bound_to_its_descriptor(void) {
    seams_reset();
    rtl_device_publish_preopened_fd(45, 1);
    assert(rtl_device_preopened_fd_reset_owed() == 45);
    rtl_device_publish_preopened_fd(46, 0);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    rtl_device_publish_preopened_fd(45, 1);
    rtl_device_set_preopened_fd(45);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    rtl_device_publish_preopened_fd(45, 1);
    rtl_device_set_preopened_fd(-1);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    rtl_device_publish_preopened_fd(-1, 1);
    assert(rtl_device_preopened_fd_reset_owed() == -1);

    /* A then B: B's open, the one that takes the slot, resets nothing. */
    auto ring = std::make_unique<input_ring_state>();
    auto dongle = std::make_unique<rtl_device_test_usb_dongle>();
    dongle_init(dongle.get());
    rtl_device_test_usb_dongle_install(dongle.get());
    rtl_device_publish_preopened_fd(45, 1);
    rtl_device_publish_preopened_fd(47, 0);
    rtl_device* dev = rtl_device_create(0, ring.get());
    assert(dev != nullptr && rtl_device_preopened_fd_held() == 47);
    assert(g_resets.load() == 0);
    rtl_device_destroy(dev);

    /* The descriptor that owes it is reset, once, before the open that takes it, and the obligation is consumed. */
    rtl_device_publish_preopened_fd(47, 1);
    g_events.recording.store(1);
    dev = rtl_device_create(0, ring.get());
    g_events.recording.store(0);
    assert(dev != nullptr);
    assert(reset_fds_take() == std::vector<int>{47});
    const std::vector<int> seen = events_take();
    assert(first_at(seen, EV_RESET) == 0 && first_at(seen, EV_OPEN_FD) == 1);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    assert(log_has(log_take(), "RTL-SDR: reset the USB device through descriptor 47 before opening it"));
    rtl_device_destroy(dev);
    rtl_device_test_usb_dongle_install(nullptr);
    rtl_device_set_preopened_fd(-1);
}

/* The open's reset outcomes: refused, it opens without one and the obligation stays; a descriptor the reset left
 * unusable fails the open, which reports the session's device failure as any failed open does. */
void
test_open_reset_outcomes(void) {
    seams_reset();
    auto ring = std::make_unique<input_ring_state>();
    auto dongle = std::make_unique<rtl_device_test_usb_dongle>();
    dongle_init(dongle.get());
    rtl_device_test_usb_dongle_install(dongle.get());

    g_reset_answer.store(-EACCES);
    rtl_device_publish_preopened_fd(kFd, 1);
    rtl_device* dev = rtl_device_create(0, ring.get());
    assert(dev != nullptr && g_resets.load() == 1 && g_opens.load() == 1);
    assert(rtl_device_preopened_fd_reset_owed() == kFd);
    assert(log_has(log_take(), "RTL-SDR: the USB reset through descriptor 31 before opening it was refused (errno 13); "
                               "opening it without one"));
    rtl_device_destroy(dev);

    g_reset_answer.store(-ENODEV);
    g_open_answer.store(-4);
    dsd_input_failure_clear();
    rtl_device_publish_preopened_fd(kFd, 1);
    dev = rtl_device_create(0, ring.get());
    assert(dev == nullptr && g_resets.load() == 2);
    assert(log_has(log_take(), "RTL-SDR: the USB reset through descriptor 31 before opening it left it unusable "
                               "(errno 19)"));
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE && failure.native_code == -4);
    assert(rtl_device_preopened_fd_reset_owed() == kFd);
    assert(rtl_device_preopened_fd_held() == -1);
    rtl_device_test_usb_dongle_install(nullptr);
    rtl_device_set_preopened_fd(-1);
    dsd_input_failure_clear();
}

/* A descriptor whose identity cannot be read opens, but is never reset in place: its open says why, once. */
void
test_unidentified_descriptor_not_recovered(void) {
    seams_reset();
    rtl_device_test_set_fd_identity_hook(nullptr);
    auto ring = std::make_unique<input_ring_state>();
    auto dongle = std::make_unique<rtl_device_test_usb_dongle>();
    dongle_init(dongle.get());
    rtl_device_test_usb_dongle_install(dongle.get());
    rtl_device_set_preopened_fd(kFd);
    (void)log_take();
    rtl_device* dev = rtl_device_create(0, ring.get());
    assert(dev != nullptr);
    assert(rtl_device_usb_recovery_eligible(dev) == 0);
    assert(log_has(log_take(), "RTL-SDR: stall recovery is off for this receiver: its descriptor's USB identity could "
                               "not be read (errno 95)"));
    rtl_device_destroy(dev);
    rtl_device_test_usb_dongle_install(nullptr);
    rtl_device_set_preopened_fd(-1);
    rtl_device_test_set_fd_identity_hook(fake_fd_identity);
}

/* ---- The swap ---- */

/* Reset and reopened: the old handle is closed, then the descriptor is reset, then opened again, then the settings are
 * replayed; the stream resumes, the obligation the verdict set is consumed, and the device still holds its
 * descriptor. */
void
test_swap_order(void) {
    seams_reset();
    (void)log_take();
    rig r;
    rig_open(r);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    assert(r.posted.owed_at_post.load() == kFd);
    g_events.recording.store(1);
    char why[256] = {};
    assert(play_controller(r, why, sizeof why) == 0);
    g_events.recording.store(0);
    expect_running();
    const std::vector<int> seen = events_take();
    const int close_at = first_at(seen, RTL_TEST_DONGLE_CLOSE);
    const int reset_at = first_at(seen, EV_RESET);
    const int open_fd_at = first_at(seen, EV_OPEN_FD);
    const int open_at = first_at(seen, RTL_TEST_DONGLE_OPEN);
    const int freq_at = first_at(seen, RTL_TEST_DONGLE_FREQ);
    const int gain_at = first_at(seen, RTL_TEST_DONGLE_GAIN);
    assert(close_at >= 0 && close_at < reset_at && reset_at < open_fd_at && open_fd_at < open_at);
    assert(open_at < freq_at && freq_at < gain_at);
    assert(seen.back() == RTL_TEST_DONGLE_RESET_BUFFER);
    assert(reset_fds_take() == std::vector<int>{kFd});
    assert(r.dongle->tuner.freq != 0U && r.dongle->gain.gain == 240);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    assert(rtl_device_preopened_fd_held() == kFd);
    const rtl_device_test_usb_swap_state st = swap_state(r);
    assert(st.has_handle && st.thread_started && st.recovery_on && !st.recovering);
    const std::string log = log_take();
    assert(log_has(log, "RTL-SDR: reset the USB device through descriptor 31 to recover it"));
    assert(log_has(log, "RTL-SDR: the USB device was reset and reopened; the stream resumes"));
    rig_close(r);
}

/* Refused: reopened without a reset, the obligation kept; samples after it show the dongle streams, which clears it. */
void
test_swap_reset_refused(void) {
    seams_reset();
    g_reset_answer.store(-EBUSY);
    (void)log_take();
    rig r;
    rig_open(r);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    char why[256] = {};
    assert(play_controller(r, why, sizeof why) == 0);
    expect_running();
    assert(g_resets.load() == 1 && g_opens.load() == 2);
    assert(rtl_device_preopened_fd_reset_owed() == kFd);
    assert(log_has(log_take(), "RTL-SDR: the USB reset through descriptor 31 to recover it was refused (errno 16); "
                               "opening it without one"));
    deliver(r, 3, 10U);
    for (uint64_t waited = 0; waited < kWaitMs && rtl_device_preopened_fd_reset_owed() != -1; ++waited) {
        dsd_sleep_ms(1);
    }
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    rig_close(r);
}

/* Dead: the reset left the descriptor unusable. Nothing is opened from it again, the recovery fails and the session
 * ends for the receiver, with the obligation kept (the host's detach drops it); the device hands its descriptor back
 * on close though it holds no handle. */
void
test_swap_descriptor_dead(void) {
    for (const int dead : {-ENODEV, -EBADF}) {
        seams_reset();
        g_reset_answer.store(dead);
        (void)log_take();
        rig r;
        rig_open(r);
        deliver(r, 5, 10U);
        (void)wait_posted(r, 1);
        char why[256] = {};
        assert(play_controller(r, why, sizeof why) == -ENODEV);
        assert(g_opens.load() == 1);
        assert(wait_receiver_ended(kWaitMs) != 0U);
        expect_receiver_ending(-ENODEV);
        const std::string log = log_take();
        assert(log_has(log, "left it unusable"));
        assert(log_has(log, "resetting it failed: the USB reset left the receiver's descriptor unusable"));
        assert(rtl_device_preopened_fd_reset_owed() == kFd);
        assert(!swap_state(r).has_handle && rtl_device_preopened_fd_held() == kFd);
        rig_close(r);
    }
}

/* Reset, but the descriptor no longer opens (the device re-enumerated after all): the recovery fails. */
void
test_swap_reopen_fails(void) {
    seams_reset();
    (void)log_take();
    rig r;
    rig_open(r);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    g_open_answer.store(-4);
    char why[256] = {};
    assert(play_controller(r, why, sizeof why) == -4);
    assert(wait_receiver_ended(kWaitMs) != 0U);
    expect_receiver_ending(-4);
    assert(log_has(log_take(), "could not reopen the receiver's descriptor after the USB reset (rc=-4)"));
    rig_close(r);
}

/* The reopened descriptor leads to another dongle: nothing is replayed onto it, and recovery stays off. */
void
test_swap_identity_mismatch(void) {
    seams_reset();
    g_identity_other_from.store(2);
    (void)log_take();
    rig r;
    rig_open(r);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    char why[256] = {};
    assert(play_controller(r, why, sizeof why) == -EIO);
    assert(std::string(why) == "the reopened descriptor leads to another receiver");
    assert(rtl_device_usb_recovery_eligible(r.dev) == 0);
    assert(r.dongle->open_handles == 0);
    assert(wait_receiver_ended(kWaitMs) != 0U);
    rig_close(r);
}

/* A stop that lands just before the reset (here, while the old handle closes) cancels the recovery silently: nothing is
 * reset or opened, and the dongle still owes its reset. */
std::atomic<rtl_device*> g_stop_target{nullptr};
std::atomic<int> g_stop_on_close{0};
dsd_thread_t g_stopper{};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
stop_device(void* arg) {
    (void)rtl_device_stop_async(static_cast<rtl_device*>(arg));
    DSD_THREAD_RETURN;
}

void
stop_on_close(void* ctx, int op) {
    (void)ctx;
    if (op != RTL_TEST_DONGLE_CLOSE || g_stop_on_close.exchange(0) == 0) {
        return;
    }
    rtl_device* dev = g_stop_target.load();
    assert(dsd_thread_create(&g_stopper, stop_device, dev) == 0);
    /* The stop latches first, then waits for the swap's lock. */
    rtl_device_test_usb_swap_state st{};
    for (uint64_t waited = 0; waited < kWaitMs; ++waited) {
        rtl_device_test_usb_swap_state_get(dev, &st);
        if (st.stop_requested) {
            break;
        }
        dsd_sleep_ms(1);
    }
    assert(st.stop_requested);
}

void
test_stop_just_before_reset(void) {
    seams_reset();
    (void)log_take();
    rig r;
    rig_open(r);
    deliver(r, 5, 10U);
    (void)wait_posted(r, 1);
    g_stop_target.store(r.dev);
    r.dongle->on_op = stop_on_close;
    g_stop_on_close.store(1);
    char why[256] = {};
    assert(play_controller(r, why, sizeof why) == -ECANCELED);
    dsd_thread_join(g_stopper);
    r.dongle->on_op = dongle_on_op;
    assert(g_resets.load() == 0 && g_opens.load() == 1);
    assert(rtl_device_preopened_fd_reset_owed() == kFd);
    assert(dsd_input_failure_receiver_ended() == 0);
    rig_close(r);
}

/* ---- The pre-recovery grace ---- */

/* The verdict sets the obligation, then the recovery waits out the grace before it is posted. */
void
test_grace_then_recovery(void) {
    seams_reset();
    rig r;
    rig_open(r);
    const unsigned int graces = rtl_device_test_recoveries_in_grace();
    deliver(r, 5, 10U);
    const uint64_t in_grace_ms = wait_in_grace(graces + 1U);
    assert(rtl_device_preopened_fd_reset_owed() == kFd);
    assert(r.posted.count.load() == 0);
    const uint64_t posted_ms = wait_posted(r, 1);
    assert(posted_ms - in_grace_ms + kPeriodMs >= kGraceMs);
    char why[256] = {};
    assert(play_controller(r, why, sizeof why) == 0);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    rig_close(r);
}

/* Samples inside the grace: no stall after all. Nothing is reset, the obligation is cleared, the session goes on, and a
 * later stall is a new one. */
void
test_grace_samples_resume(void) {
    seams_reset();
    rig r;
    rig_open(r);
    const unsigned int graces = rtl_device_test_recoveries_in_grace();
    deliver(r, 5, 10U);
    (void)wait_in_grace(graces + 1U);
    deliver(r, 1, 0U);
    for (uint64_t waited = 0; waited < kWaitMs && rtl_device_preopened_fd_reset_owed() != -1; ++waited) {
        dsd_sleep_ms(1);
    }
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    dsd_sleep_ms(kGraceMs);
    assert(r.posted.count.load() == 0 && g_resets.load() == 0);
    expect_running();
    (void)wait_in_grace(graces + 2U);
    (void)wait_posted(r, 1);
    rig_close(r);
}

/* A Stop inside the grace ends the session as the user's: nothing is posted or reset, and the dongle still owes its
 * reset, so the next open resets it first. */
void
test_grace_stop_keeps_obligation(void) {
    seams_reset();
    rig r;
    rig_open(r);
    const unsigned int graces = rtl_device_test_recoveries_in_grace();
    deliver(r, 5, 10U);
    (void)wait_in_grace(graces + 1U);
    assert(rtl_device_stop_async(r.dev) == 0);
    assert(r.posted.count.load() == 0 && g_resets.load() == 0);
    assert(dsd_input_failure_receiver_ended() == 0);
    rtl_device_destroy(r.dev);
    r.dev = nullptr;
    assert(rtl_device_preopened_fd_reset_owed() == kFd);
    assert(rtl_device_preopened_fd_held() == -1);
    rtl_device_test_usb_stream_detach();

    /* The host reads it, and hands the next connection's descriptor with the reset; its open resets that one first. */
    rtl_device_publish_preopened_fd(46, 1);
    g_events.recording.store(1);
    r.dev = rtl_device_create(0, r.ring.get());
    g_events.recording.store(0);
    assert(r.dev != nullptr);
    assert(reset_fds_take() == std::vector<int>{46});
    const std::vector<int> seen = events_take();
    assert(first_at(seen, EV_RESET) == 0 && first_at(seen, EV_OPEN_FD) == 1);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    rtl_device_destroy(r.dev);
    r.dev = nullptr;
    rtl_device_test_usb_dongle_install(nullptr);
    input_ring_destroy(r.ring.get());
    rtl_device_set_preopened_fd(-1);
    reset_session();
}

/* A detach inside the grace: the host stops the session and releases the connection. Nothing is reset, the session is
 * the stop's, and the released connection's obligation goes with it. */
void
test_grace_detach(void) {
    seams_reset();
    rig r;
    rig_open(r);
    const unsigned int graces = rtl_device_test_recoveries_in_grace();
    deliver(r, 5, 10U);
    (void)wait_in_grace(graces + 1U);
    dsd_exitflag_store(1);
    rtl_device_set_preopened_fd(-1);
    dsd_sleep_ms(kGraceMs + 100U);
    assert(r.posted.count.load() == 0 && g_resets.load() == 0);
    assert(dsd_input_failure_receiver_ended() == 0);
    assert(rtl_device_preopened_fd_reset_owed() == -1);
    rig_close(r);
}

/* The connection released inside the grace without a stop (its detach broadcast not acted on yet): nothing is reset
 * through it, and the stall ends the session for the receiver, after its own grace, as #774 does. */
void
test_grace_connection_released(void) {
    seams_reset();
    (void)log_take();
    rig r;
    rig_open(r);
    const unsigned int graces = rtl_device_test_recoveries_in_grace();
    deliver(r, 5, 10U);
    (void)wait_in_grace(graces + 1U);
    rtl_device_set_preopened_fd(-1);
    assert(wait_receiver_ended(kWaitMs) != 0U);
    expect_receiver_ending(-ETIMEDOUT);
    assert(r.posted.count.load() == 0 && g_resets.load() == 0);
    assert(log_has(log_take(), " ms and its USB connection was released, so it was not reset; the stream stops"));
    rig_close(r);
}

} // namespace

int
main(void) {
    dsd_neo_log_set_tap(capture_log, &g_log);
    for (size_t i = 0; i < sizeof g_buffer; ++i) {
        g_buffer[i] = (unsigned char)(127U + (i & 1U));
    }
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_test_set_progress_period_ms(kPeriodMs);
    rtl_device_test_set_open_fd_failure_hook(fake_open_fd);
    rtl_device_test_set_fd_reset_hook(fake_fd_reset);
    rtl_device_test_set_fd_identity_hook(fake_fd_identity);

    test_obligation_bound_to_its_descriptor();
    test_open_reset_outcomes();
    test_unidentified_descriptor_not_recovered();
    test_swap_order();
    test_swap_reset_refused();
    test_swap_descriptor_dead();
    test_swap_reopen_fails();
    test_swap_identity_mismatch();
    test_stop_just_before_reset();

    rtl_device_set_unplug_owner_grace_ms(kGraceMs);
    test_grace_then_recovery();
    test_grace_samples_resume();
    test_grace_stop_keeps_obligation();
    test_grace_detach();
    test_grace_connection_released();
    rtl_device_set_unplug_owner_grace_ms(0U);

    rtl_device_test_set_fd_identity_hook(nullptr);
    rtl_device_test_set_fd_reset_hook(nullptr);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    rtl_device_test_set_progress_limit_ms(0U);
    rtl_device_test_set_progress_period_ms(0U);
    dsd_neo_log_set_tap(nullptr, nullptr);
    DSD_FPRINTF(stderr, "IO_RTL_USB_FD_RECOVERY: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
