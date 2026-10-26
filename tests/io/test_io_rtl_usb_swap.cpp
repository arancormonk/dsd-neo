// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Resetting and reopening a stalled RTL-SDR under a running device, over a fake dongle and a fake USB device list
 * (the real dongles are never touched). The swap stops and joins the reader before it touches the dongle, retires and
 * closes the handle, resets the identified USB device, reopens and verifies it, and replays what the dongle had
 * taken -- the latest effective values, never the initial ones or a change that failed -- before it publishes the new
 * handle; the reader restarts only on request. Setters during the swap are refused with RTL_DEVICE_EBUSY and change
 * nothing; one that took just before is replayed; getters and setters racing the swap never reach a retired handle.
 * A stop at any step ends the swap silently and no reader starts after it. A control that took and cannot be restored
 * fails the recovery. A receiver whose USB identity is not unique is never reset, and its open says why.
 */

#include <assert.h>
#include <atomic>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "rtl_stream_test_support.h"
#include "rtl_usb_identity.h"

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

/* rtl_device_set_gain()'s automatic gain. */
constexpr int kAutoGain = -100;
/* Steps of the swap the fake USB list sees, beside the dongle's RTL_TEST_DONGLE_* ops. */
constexpr int kStepReset = 100;
constexpr int kStepVerify = 101;

/* ---- Fake USB device list (Task 6's driver ops) ---- */

struct usb_dev {
    rtl_usb_device_info info;
    const char* serial;
    int hidden;         /* absent from enumerations: -1 for ever */
    int gone_on_reset;  /* the reset makes it re-enumerate (GONE) */
    uint8_t port_after; /* ... back on this port (0: its own) */
    int never_back;     /* ... and it never comes back */
    std::atomic<int> resets{0};
};

struct usb_world {
    usb_dev devs[2];
    int dev_count = 0;
    int rtl_count = 0;
    std::atomic<uint64_t> now_ms{1000U};
    void (*on_step)(int step) = nullptr;
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
    return (uint32_t)g_usb.rtl_count;
}

int
fake_rtl_index_strings(uint32_t index, char* m, char* p, char* s) {
    if ((int)index >= g_usb.rtl_count) {
        return -1;
    }
    fill_strings(g_usb.devs[index], m, p, s);
    return 0;
}

int
fake_rtl_handle_strings(rtlsdr_dev_t* dev, char* m, char* p, char* s) {
    (void)dev;
    if (g_usb.on_step) {
        g_usb.on_step(kStepVerify);
    }
    fill_strings(g_usb.devs[0], m, p, s);
    return 0;
}

struct fake_session {
    std::vector<int> visible;
};

int
fake_open_session(void** session, int* count) {
    auto* sess = new fake_session();
    for (int i = 0; i < g_usb.dev_count; i++) {
        if (g_usb.devs[i].hidden == 0) {
            sess->visible.push_back(i);
        }
    }
    *session = sess;
    *count = (int)sess->visible.size();
    return RTL_USB_OP_OK;
}

void
fake_close_session(void* session) {
    delete static_cast<fake_session*>(session);
}

int
fake_describe(void* session, int i, rtl_usb_device_info* out) {
    const auto* sess = static_cast<fake_session*>(session);
    *out = g_usb.devs[sess->visible[(size_t)i]].info;
    return RTL_USB_OP_OK;
}

int
fake_usb_open(void* session, int i, void** handle) {
    const auto* sess = static_cast<fake_session*>(session);
    *handle = &g_usb.devs[sess->visible[(size_t)i]];
    return RTL_USB_OP_OK;
}

void
fake_usb_close(void* handle) {
    (void)handle;
}

int
fake_read_strings(void* handle, char* m, char* p, char* s) {
    fill_strings(*static_cast<const usb_dev*>(handle), m, p, s);
    return RTL_USB_OP_OK;
}

int
fake_reset(void* handle) {
    auto* d = static_cast<usb_dev*>(handle);
    if (g_usb.on_step) {
        g_usb.on_step(kStepReset);
    }
    d->resets.fetch_add(1);
    if (!d->gone_on_reset) {
        return RTL_USB_OP_OK;
    }
    if (d->never_back) {
        d->hidden = -1;
    } else if (d->port_after != 0U) {
        d->info.ports[d->info.port_depth - 1] = d->port_after;
    }
    return RTL_USB_OP_GONE;
}

uint64_t
fake_now(void) {
    return g_usb.now_ms.load();
}

/* Rediscovery after a reset polls on this clock: real time passes too, so a stop can land while it waits. */
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

void
usb_world_reset(int duplicate) {
    for (usb_dev& d : g_usb.devs) {
        d.info = rtl_usb_device_info{};
        d.serial = "";
        d.hidden = 0;
        d.gone_on_reset = 0;
        d.port_after = 0U;
        d.never_back = 0;
        d.resets.store(0);
    }
    g_usb.on_step = nullptr;
    usb_dev& a = g_usb.devs[0];
    a.info.vid = 0x0bda;
    a.info.pid = 0x2838;
    a.info.bus = 1;
    a.info.ports[0] = 2;
    a.info.port_depth = 1;
    a.info.vendor_specific = 1;
    a.serial = "56435444";
    g_usb.dev_count = 1;
    g_usb.rtl_count = 1;
    if (duplicate) {
        usb_dev& b = g_usb.devs[1];
        b.info = a.info;
        b.info.bus = 3;
        b.serial = a.serial;
        g_usb.dev_count = 2;
        g_usb.rtl_count = 2;
    }
}

/* ---- Threads ---- */

void
start_thread(dsd_thread_t* thread, dsd_thread_fn fn, void* arg) {
    const int created = dsd_thread_create(thread, fn, arg);
    assert(created == 0);
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
    log->text += text ? text : "";
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

/* ---- A device on the fake dongle ---- */

/* What the startup configures, through the public setters, in the stream's order. */
struct settings {
    int tuner_type = 1; /* E4000: takes offset tuning */
    uint32_t rtl_xtal = 0U;
    uint32_t tuner_xtal = 0U;
    int direct_sampling = 0;
    uint32_t freq = 851012500U;
    uint32_t rate = 1536000U;
    int offset = 0;
    uint32_t bw = 0U;
    int set_bw = 0;
    int ppm = 0;
    int gain = kAutoGain;
    int testmode = 0;
    int if_stage = -1;
    int if_gain = 0;
    int bias = 0;
};

/* Whether librtlsdr has the bias tee control (USE_RTLSDR_BIAS_TEE); without it the setter refuses on every dongle. */
int g_bias_api = 1;

struct rig {
    std::unique_ptr<input_ring_state> ring;
    rtl_device_test_usb_dongle dongle{};
    rtl_device* dev = nullptr;
    int reader_attached = 0;
};

const int k_gains[] = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};

void
rig_open(rig& r, int tuner_type) {
    r.ring = std::make_unique<input_ring_state>();
    assert(input_ring_init(r.ring.get(), 1U << 16) == 0);
    DSD_MEMSET(&r.dongle, 0, sizeof r.dongle);
    r.dongle.tuner.tuner_type = tuner_type;
    r.dongle.gain.gain_count = (int)(sizeof k_gains / sizeof k_gains[0]);
    DSD_MEMCPY(r.dongle.gain.gains, k_gains, sizeof k_gains);
    rtl_device_test_usb_dongle_install(&r.dongle);
    r.dev = rtl_device_create(0, r.ring.get());
    assert(r.dev != nullptr);
}

void
rig_configure(rig& r, const settings& s) {
    rtl_device* dev = r.dev;
    if (s.rtl_xtal != 0U || s.tuner_xtal != 0U) {
        assert(rtl_device_set_xtal_freq(dev, s.rtl_xtal, s.tuner_xtal) == 0);
    }
    if (s.direct_sampling != 0) {
        assert(rtl_device_set_direct_sampling(dev, s.direct_sampling) == 0);
    }
    assert(rtl_device_set_frequency(dev, s.freq) == 0);
    assert(rtl_device_set_sample_rate(dev, s.rate) == 0);
    if (s.offset) {
        (void)rtl_device_set_offset_tuning_enabled(dev, 1);
    }
    if (s.set_bw) {
        assert(rtl_device_set_tuner_bandwidth(dev, s.bw) == 0);
    }
    if (s.ppm != 0) {
        assert(rtl_device_set_ppm(dev, s.ppm) == 0);
    }
    assert(rtl_device_set_frequency(dev, s.freq) == 0);
    assert(rtl_device_set_gain(dev, s.gain) == 0);
    if (s.testmode) {
        assert(rtl_device_set_testmode(dev, 1) == 0);
    }
    if (s.if_stage >= 0) {
        assert(rtl_device_set_if_gain(dev, s.if_stage, s.if_gain) == 0);
    }
    if (s.bias) {
        const int rc = rtl_device_set_bias_tee(dev, 1);
        assert(rc == 0 || rc == DSD_ERR_NOT_SUPPORTED);
        g_bias_api = (rc == 0) ? 1 : 0;
    }
}

rtl_device_test_usb_swap_state
swap_state(const rig& r) {
    rtl_device_test_usb_swap_state st{};
    rtl_device_test_usb_swap_state_get(r.dev, &st);
    return st;
}

void
wait_reader_running(const rig& r) {
    for (int i = 0; i < 2000 && !swap_state(r).reader_running; i++) {
        dsd_sleep_ms(1);
    }
    assert(swap_state(r).reader_running);
}

void
rig_start(rig& r) {
    rtl_device_test_usb_stream_attach();
    r.reader_attached = 1;
    assert(rtl_device_start_async(r.dev, 16U * 32U * 512U) == 0);
    wait_reader_running(r);
}

void
rig_close(rig& r) {
    if (r.dev) {
        rtl_device_destroy(r.dev);
        r.dev = nullptr;
    }
    if (r.reader_attached) {
        rtl_device_test_usb_stream_detach();
        r.reader_attached = 0;
    }
    rtl_device_test_usb_dongle_install(nullptr);
    assert(r.dongle.open_handles == 0);
    assert(r.dongle.stale_handle_calls == 0);
    input_ring_destroy(r.ring.get());
}

int
recover(rig& r, std::string* why = nullptr) {
    char buf[256] = {};
    const int rc = rtl_device_usb_recover_handle(r.dev, buf, sizeof buf);
    if (why) {
        *why = buf;
    }
    return rc;
}

int
op_index(const rtl_device_test_usb_dongle& d, int op, int from) {
    for (int i = from; i < d.op_count; i++) {
        if (d.ops[i] == op) {
            return i;
        }
    }
    return -1;
}

/* librtlsdr's offset for a rate, and the bandwidth the offset-aware policy gives a wanted one. */
uint32_t
offs_for(uint32_t rate) {
    return (rate / 2U) * 170U / 100U;
}

/* What the dongle holds once it took @p s (the gain table maps the gain to itself). */
void
expect_dongle_holds(const rtl_device_test_usb_dongle& d, const settings& s, int offset_took) {
    assert(d.tuner.freq == s.freq);
    assert(d.tuner.rate == s.rate);
    assert(d.tuner.rate_without_freq == 0);
    assert(d.tuner.direct_sampling == s.direct_sampling);
    assert((d.tuner.offs_freq != 0U) == (offset_took != 0));
    if (s.set_bw) {
        uint32_t want = s.bw;
        if (offset_took && want < 2U * offs_for(s.rate)) {
            want = 2U * offs_for(s.rate);
        }
        assert(d.tuner.bw == want);
    }
    assert(d.ppm == s.ppm);
    assert(d.rtl_xtal == s.rtl_xtal);
    assert(d.tuner_xtal == s.tuner_xtal);
    if (s.gain == kAutoGain) {
        assert(d.gain.manual == 0);
    } else {
        assert(d.gain.manual == 1);
        assert(d.gain.gain == s.gain);
    }
    if (s.testmode) {
        assert(d.gain.reg19 == 0x03);
    } else if (s.gain != kAutoGain) {
        assert(d.gain.reg19 == 0x05);
    }
    if (s.if_stage >= 0) {
        assert(d.if_gain_count == 1);
        assert(d.if_gains[0].stage == s.if_stage);
        assert(d.if_gains[0].gain == s.if_gain);
    } else {
        assert(d.if_gain_count == 0);
    }
    assert(d.bias_tee == (g_bias_api ? s.bias : 0));
}

/* ---- Tests ---- */

/* The swap touches the dongle only once the reader returned, and the reader restarts only on request. */
void
test_swap_configures_before_reader_runs(void) {
    usb_world_reset(0);
    rig r;
    rig_open(r, 1);
    settings s;
    s.gain = 240;
    s.ppm = 3;
    s.bias = 1;
    rig_configure(r, s);
    rig_start(r);
    const rtl_device_test_usb_swap_state before = swap_state(r);
    r.dongle.calls_while_reading = 0;
    const int opens = r.dongle.calls[RTL_TEST_DONGLE_OPEN];
    std::string why;
    assert(recover(r, &why) == 0);
    assert(r.dongle.calls_while_reading == 0);
    assert(r.dongle.calls[RTL_TEST_DONGLE_OPEN] == opens + 1);
    assert(r.dongle.calls[RTL_TEST_DONGLE_RESET_BUFFER] >= 1);
    assert(g_usb.devs[0].resets.load() == 1);
    rtl_device_test_usb_swap_state st = swap_state(r);
    assert(st.has_handle && !st.thread_started && !st.reader_running && !st.recovering);
    /* The FIFO reset is the last thing the swap does. */
    assert(r.dongle.ops[r.dongle.op_count - 1] == RTL_TEST_DONGLE_RESET_BUFFER);
    assert(rtl_device_usb_resume_reader(r.dev) == 0);
    wait_reader_running(r);
    st = swap_state(r);
    assert(st.thread_started);
    assert(st.watch_rearm_seq == before.watch_rearm_seq + 1U);
    assert(r.dongle.calls_while_reading == 0);
    expect_dongle_holds(r.dongle, s, 0);
    rig_close(r);
}

/* The replay restores what the dongle took last, in the replay order, not the startup values. */
void
test_replay_restores_effective_not_initial(void) {
    rig r;
    rig_open(r, 1);
    settings s;
    s.rtl_xtal = 28800100U;
    s.tuner_xtal = 28800200U;
    s.offset = 1;
    s.set_bw = 1;
    s.bw = 1000000U;
    s.ppm = 2;
    s.gain = 90;
    s.if_stage = 1;
    s.if_gain = 60;
    rig_configure(r, s);
    rig_start(r);
    /* Changes while it streams: a retune, a new gain, PPM, bias tee, an IF gain and test mode. */
    s.freq = 853037500U;
    assert(rtl_device_set_frequency(r.dev, s.freq) == 0);
    s.gain = 340;
    assert(rtl_device_set_gain(r.dev, s.gain) == 0);
    s.ppm = -4;
    assert(rtl_device_set_ppm(r.dev, s.ppm) == 0);
    s.bias = 1;
    assert(rtl_device_set_bias_tee(r.dev, 1) == (g_bias_api ? 0 : DSD_ERR_NOT_SUPPORTED));
    s.if_gain = 90;
    assert(rtl_device_set_if_gain(r.dev, s.if_stage, s.if_gain) == 0);
    s.testmode = 1;
    assert(rtl_device_set_testmode(r.dev, 1) == 0);
    assert(recover(r) == 0);
    expect_dongle_holds(r.dongle, s, 1);
    /* Replay order on the reopened dongle. */
    const rtl_device_test_usb_dongle& d = r.dongle;
    assert(d.ops[0] == RTL_TEST_DONGLE_OPEN);
    const int xtal = op_index(d, RTL_TEST_DONGLE_XTAL, 0);
    const int freq1 = op_index(d, RTL_TEST_DONGLE_FREQ, 0);
    const int rate = op_index(d, RTL_TEST_DONGLE_RATE, 0);
    const int offset = op_index(d, RTL_TEST_DONGLE_OFFSET, 0);
    const int bw = op_index(d, RTL_TEST_DONGLE_BANDWIDTH, 0);
    const int ppm = op_index(d, RTL_TEST_DONGLE_PPM, 0);
    const int freq2 = op_index(d, RTL_TEST_DONGLE_FREQ, ppm < 0 ? 0 : ppm);
    const int gain = op_index(d, RTL_TEST_DONGLE_GAIN, 0);
    const int ifg = op_index(d, RTL_TEST_DONGLE_IF_GAIN, 0);
    const int bias = op_index(d, RTL_TEST_DONGLE_BIAS_TEE, 0);
    const int fifo = op_index(d, RTL_TEST_DONGLE_RESET_BUFFER, 0);
    assert(xtal > 0 && xtal < freq1 && freq1 < rate && rate < offset && offset < bw && bw < ppm && ppm < freq2);
    assert(freq2 < gain && gain < ifg && ifg < bias && bias < fifo);
    assert(rtl_device_get_offset_tuning(r.dev) == 1);
    int tenth_db = 0;
    assert(rtl_device_get_tuner_gain(r.dev, &tenth_db) == 0 && tenth_db == s.gain);
    rig_close(r);
}

/* Direct sampling and a crystal on an R820T: replayed first, and no rate reaches a dongle with no frequency. */
void
test_replay_direct_sampling_r820t(void) {
    rig r;
    rig_open(r, 5);
    settings s;
    s.tuner_type = 5;
    s.direct_sampling = 2;
    s.freq = 7100000U;
    s.rate = 2400000U;
    s.offset = 1; /* refused on R82xx: never took, so never replayed */
    s.gain = 190;
    rig_configure(r, s);
    rig_start(r);
    assert(recover(r) == 0);
    expect_dongle_holds(r.dongle, s, 0);
    assert(op_index(r.dongle, RTL_TEST_DONGLE_OFFSET, 0) < 0);
    const int ds = op_index(r.dongle, RTL_TEST_DONGLE_DIRECT_SAMPLING, 0);
    assert(ds > 0 && ds < op_index(r.dongle, RTL_TEST_DONGLE_FREQ, 0));
    rig_close(r);
}

/* A retune the dongle refused leaves the record at the old frequency, which is what the recovery restores. */
void
test_failed_retune_not_replayed(void) {
    rig r;
    rig_open(r, 1);
    settings s;
    rig_configure(r, s);
    rig_start(r);
    r.dongle.faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 1;
    r.dongle.faults[RTL_TEST_DONGLE_FREQ].fail_value = 857000000;
    assert(rtl_device_set_frequency(r.dev, 857000000U) != 0);
    r.dongle.faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 0;
    /* So is a gain the tuner refused (rolled back). */
    r.dongle.gain.gain_fault.fail_value_set = 1;
    r.dongle.gain.gain_fault.fail_value = 420;
    assert(rtl_device_set_gain(r.dev, 420) != 0);
    r.dongle.gain.gain_fault.fail_value_set = 0;
    assert(recover(r) == 0);
    expect_dongle_holds(r.dongle, s, 0);
    rig_close(r);
}

/* ---- Setters during the swap ---- */

struct busy_probe {
    rtl_device* dev;
    int results[16];
    int count;
    int getter_rc;
    int calls_before;
    int calls_after;
    rtl_device_test_usb_dongle* dongle;
};

int
dongle_calls(const rtl_device_test_usb_dongle& d) {
    int n = 0;
    for (int op = 0; op < RTL_TEST_DONGLE_OP_COUNT; op++) {
        n += d.calls[op];
    }
    return n + d.gain.reg19_calls + d.gain.mode_calls + d.gain.gain_calls;
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
busy_probe_thread(void* arg) {
    auto* p = static_cast<busy_probe*>(arg);
    rtl_device* dev = p->dev;
    p->calls_before = dongle_calls(*p->dongle);
    int i = 0;
    int selected = 0;
    p->results[i++] = rtl_device_set_frequency(dev, 860000000U);
    p->results[i++] = rtl_device_set_sample_rate(dev, 2048000U);
    p->results[i++] = rtl_device_set_gain(dev, 420);
    p->results[i++] = rtl_device_set_gain_nearest(dev, 415, &selected);
    p->results[i++] = rtl_device_set_ppm(dev, 9);
    p->results[i++] = rtl_device_set_direct_sampling(dev, 1);
    p->results[i++] = rtl_device_set_offset_tuning_enabled(dev, 0);
    p->results[i++] = rtl_device_set_tuner_bandwidth(dev, 300000U);
    p->results[i++] = rtl_device_set_bias_tee(dev, 0);
    p->results[i++] = rtl_device_set_xtal_freq(dev, 28799000U, 0U);
    p->results[i++] = rtl_device_set_testmode(dev, 1);
    p->results[i++] = rtl_device_set_if_gain(dev, 2, 30);
    p->results[i++] = rtl_device_reset_buffer(dev);
    p->count = i;
    int tenth_db = 0;
    p->getter_rc = rtl_device_get_tuner_gain(dev, &tenth_db);
    p->calls_after = dongle_calls(*p->dongle);
    DSD_THREAD_RETURN;
}

busy_probe g_busy;
int g_busy_at = -1;
int g_busy_ran = 0;

void
busy_on_op(void* ctx, int op) {
    (void)ctx;
    if (op != g_busy_at || g_busy_ran) {
        return;
    }
    g_busy_ran = 1;
    dsd_thread_t t;
    start_thread(&t, busy_probe_thread, &g_busy);
    dsd_thread_join(t);
}

void
test_setter_during_swap_returns_busy(void) {
    /* Once with the handle retired (at the reopen) and once in the replay (at the PPM write). */
    const int points[] = {RTL_TEST_DONGLE_OPEN, RTL_TEST_DONGLE_PPM};
    for (int point : points) {
        rig r;
        rig_open(r, 1);
        settings s;
        s.offset = 1;
        s.gain = 115;
        s.ppm = 5;
        s.bias = 1;
        s.if_stage = 3;
        s.if_gain = 20;
        rig_configure(r, s);
        rig_start(r);
        g_busy = busy_probe{};
        g_busy.dev = r.dev;
        g_busy.dongle = &r.dongle;
        g_busy_at = point;
        g_busy_ran = 0;
        r.dongle.on_op = busy_on_op;
        assert(recover(r) == 0);
        r.dongle.on_op = nullptr;
        assert(g_busy_ran);
        for (int i = 0; i < g_busy.count; i++) {
            if (g_busy.results[i] != RTL_DEVICE_EBUSY) {
                DSD_FPRINTF(stderr, "setter %d returned %d during the swap\n", i, g_busy.results[i]);
            }
            assert(g_busy.results[i] == RTL_DEVICE_EBUSY);
        }
        /* Nothing reached the dongle, and the getter has no reading to give. */
        assert(g_busy.calls_after == g_busy.calls_before);
        assert(g_busy.getter_rc != 0);
        /* And nothing was remembered: the dongle holds the settings from before, and so does the record. */
        expect_dongle_holds(r.dongle, s, 1);
        assert(rtl_device_get_offset_tuning(r.dev) == 1);
        assert(rtl_device_is_auto_gain(r.dev) == 0);
        int tenth_db = 0;
        assert(rtl_device_get_tuner_gain(r.dev, &tenth_db) == 0 && tenth_db == s.gain);
        rig_close(r);
    }
}

/* ---- A setter that took just before the handle was retired ---- */

struct held_setter {
    rtl_device* dev;
    std::atomic<int> inside{0};
    std::atomic<int> release{0};
    int rc;
};

held_setter g_held;

void
hold_gain_write(void* ctx, int op) {
    (void)ctx;
    if (op != RTL_TEST_DONGLE_GAIN || g_held.inside.load()) {
        return;
    }
    g_held.inside.store(1);
    for (int i = 0; i < 2000 && !g_held.release.load(); i++) {
        dsd_sleep_ms(1);
    }
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
held_setter_thread(void* arg) {
    auto* h = static_cast<held_setter*>(arg);
    h->rc = rtl_device_set_gain(h->dev, 290);
    DSD_THREAD_RETURN;
}

struct recover_call {
    rig* r;
    int rc;
    std::atomic<int> done{0};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
recover_thread(void* arg) {
    auto* c = static_cast<recover_call*>(arg);
    c->rc = recover(*c->r);
    c->done.store(1);
    DSD_THREAD_RETURN;
}

void
test_setter_before_retire_is_replayed(void) {
    /* Plainly: a change that took while the reader streamed is replayed. */
    {
        rig r;
        rig_open(r, 1);
        settings s;
        rig_configure(r, s);
        rig_start(r);
        s.gain = 165;
        assert(rtl_device_set_gain(r.dev, s.gain) == 0);
        assert(recover(r) == 0);
        expect_dongle_holds(r.dongle, s, 0);
        rig_close(r);
    }
    /* A setter inside its gain write when the recovery begins: the swap waits for it, and replays what it set. */
    rig r;
    rig_open(r, 1);
    settings s;
    rig_configure(r, s);
    rig_start(r);
    g_held.dev = r.dev;
    g_held.inside.store(0);
    g_held.release.store(0);
    g_held.rc = -99;
    r.dongle.on_op = hold_gain_write;
    dsd_thread_t setter;
    start_thread(&setter, held_setter_thread, &g_held);
    for (int i = 0; i < 2000 && !g_held.inside.load(); i++) {
        dsd_sleep_ms(1);
    }
    assert(g_held.inside.load());
    recover_call call;
    call.r = &r;
    call.rc = -99;
    dsd_thread_t recovery;
    start_thread(&recovery, recover_thread, &call);
    dsd_sleep_ms(50);
    assert(!call.done.load()); /* waiting for the setter */
    g_held.release.store(1);
    dsd_thread_join(setter);
    dsd_thread_join(recovery);
    r.dongle.on_op = nullptr;
    assert(g_held.rc == 0);
    assert(call.rc == 0);
    s.gain = 290;
    expect_dongle_holds(r.dongle, s, 0);
    rig_close(r);
}

/* ---- Getters and setters racing the swap (run under TSan) ----
 * The menu's and the autogain's setters, from their own threads; retunes are the controller's, as is the swap, so
 * they never race it. */

struct hammer {
    rtl_device* dev;
    std::atomic<int> run{1};
    std::atomic<int> bad{0};
    std::atomic<int> busy{0};
    std::atomic<int> took{0};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
setter_hammer(void* arg) {
    auto* h = static_cast<hammer*>(arg);
    int n = 0;
    while (h->run.load()) {
        const int rcs[] = {
            rtl_device_set_gain(h->dev, k_gains[n % 14]),
            rtl_device_set_ppm(h->dev, (n % 5) - 2),
            g_bias_api ? rtl_device_set_bias_tee(h->dev, n & 1) : 0,
            rtl_device_set_tuner_bandwidth(h->dev, 1000000U + (uint32_t)(n % 4) * 100000U),
        };
        for (int rc : rcs) {
            if (rc == RTL_DEVICE_EBUSY) {
                h->busy.fetch_add(1);
            } else if (rc == 0) {
                h->took.fetch_add(1);
            } else {
                h->bad.fetch_add(1);
            }
        }
        n++;
    }
    DSD_THREAD_RETURN;
}

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
getter_hammer(void* arg) {
    auto* h = static_cast<hammer*>(arg);
    while (h->run.load()) {
        int tenth_db = 0;
        (void)rtl_device_get_tuner_gain(h->dev, &tenth_db);
        (void)rtl_device_is_auto_gain(h->dev);
        (void)rtl_device_get_sample_rate(h->dev);
        (void)rtl_device_get_offset_tuning(h->dev);
        (void)rtl_device_get_offset_tuning_hz(h->dev);
        (void)rtl_device_usb_recovery_eligible(h->dev);
    }
    DSD_THREAD_RETURN;
}

void
test_getter_and_setter_in_flight_during_retire(void) {
    rig r;
    rig_open(r, 1);
    settings s;
    s.gain = 240;
    rig_configure(r, s);
    rig_start(r);
    hammer h;
    h.dev = r.dev;
    dsd_thread_t setters[2];
    dsd_thread_t getters[2];
    for (dsd_thread_t& t : setters) {
        start_thread(&t, setter_hammer, &h);
    }
    for (dsd_thread_t& t : getters) {
        start_thread(&t, getter_hammer, &h);
    }
    for (int i = 0; i < 6; i++) {
        assert(recover(r) == 0);
        assert(rtl_device_usb_resume_reader(r.dev) == 0);
        dsd_sleep_ms(5);
    }
    h.run.store(0);
    for (dsd_thread_t t : setters) {
        dsd_thread_join(t);
    }
    for (dsd_thread_t t : getters) {
        dsd_thread_join(t);
    }
    assert(h.bad.load() == 0);
    assert(h.took.load() > 0);
    assert(r.dongle.stale_handle_calls == 0);
    /* Hardware and record agree after the last swap. */
    int tenth_db = 0;
    assert(rtl_device_get_tuner_gain(r.dev, &tenth_db) == 0);
    assert(tenth_db == r.dongle.gain.gain);
    rig_close(r);
}

/* ---- A stop at each step of the swap ---- */

struct stopper {
    rtl_device* dev;
    int use_destroy;
    int rc;
};

stopper g_stopper;
dsd_thread_t g_stopper_thread;
int g_stop_at = -1;
int g_stop_fired = 0;
std::vector<int> g_steps_seen;
int g_recording_steps = 0;

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
stopper_thread(void* arg) {
    auto* s = static_cast<stopper*>(arg);
    if (s->use_destroy) {
        rtl_device_destroy(s->dev);
        s->rc = 0;
    } else {
        s->rc = rtl_device_stop_async(s->dev);
    }
    DSD_THREAD_RETURN;
}

void
stop_on_step(int step) {
    if (g_recording_steps) {
        if (g_steps_seen.empty() || g_steps_seen.back() != step) {
            g_steps_seen.push_back(step);
        }
        return;
    }
    if (step != g_stop_at || g_stop_fired) {
        return;
    }
    g_stop_fired = 1;
    start_thread(&g_stopper_thread, stopper_thread, &g_stopper);
    /* Until the stop latched (it then waits for the swap). */
    for (int i = 0; i < 2000; i++) {
        rtl_device_test_usb_swap_state st{};
        rtl_device_test_usb_swap_state_get(g_stopper.dev, &st);
        if (st.stop_requested) {
            return;
        }
        dsd_sleep_ms(1);
    }
    assert(!"the stop never latched");
}

void
stop_on_op(void* ctx, int op) {
    (void)ctx;
    stop_on_step(op);
}

void
configure_full(rig& r) {
    settings s;
    s.rtl_xtal = 28800100U;
    s.offset = 1;
    s.set_bw = 1;
    s.bw = 2000000U;
    s.ppm = 1;
    s.gain = 215;
    s.if_stage = 2;
    s.if_gain = 40;
    s.bias = 1;
    rig_configure(r, s);
}

void
test_stop_at_each_swap_step(void) {
    /* The steps a full swap goes through. */
    {
        rig r;
        rig_open(r, 1);
        configure_full(r);
        rig_start(r);
        g_steps_seen.clear();
        g_recording_steps = 1;
        g_usb.on_step = stop_on_step;
        r.dongle.on_op = stop_on_op;
        assert(recover(r) == 0);
        g_recording_steps = 0;
        g_usb.on_step = nullptr;
        r.dongle.on_op = nullptr;
        rig_close(r);
    }
    assert(g_steps_seen.size() >= 10U);
    std::vector<int> steps = g_steps_seen;
    steps.push_back(-2); /* a stop while the reset waits for the dongle to come back */
    for (size_t i = 0; i < steps.size(); i++) {
        for (int use_destroy = 0; use_destroy < 2; use_destroy++) {
            if (use_destroy && (i % 3U) != 0U) {
                continue; /* destroy at a sample of the steps */
            }
            usb_world_reset(0);
            rig r;
            rig_open(r, 1);
            configure_full(r);
            rig_start(r);
            int step = steps[i];
            if (step == -2) {
                g_usb.devs[0].gone_on_reset = 1;
                g_usb.devs[0].never_back = 1;
                step = kStepReset;
            }
            g_stopper = stopper{r.dev, use_destroy, -99};
            g_stop_at = step;
            g_stop_fired = 0;
            g_usb.on_step = stop_on_step;
            r.dongle.on_op = stop_on_op;
            const uint64_t t0 = dsd_realtime_mono_ms();
            std::string why;
            const int rc = recover(r, &why);
            dsd_thread_join(g_stopper_thread);
            g_usb.on_step = nullptr;
            assert(g_stop_fired);
            if (rc != -ECANCELED) {
                DSD_FPRINTF(stderr, "stop at step %d: recovery returned %d (%s)\n", step, rc, why.c_str());
            }
            assert(rc == -ECANCELED);
            /* Cancelled promptly, not after the rediscovery's 5 s. */
            assert(dsd_realtime_mono_ms() - t0 < 4000U);
            if (use_destroy) {
                r.dev = nullptr;
            } else {
                r.dongle.on_op = nullptr;
                const rtl_device_test_usb_swap_state st = swap_state(r);
                assert(st.stop_requested && !st.thread_started && !st.reader_running && !st.recovering);
                assert(!st.has_handle);
                assert(r.dongle.open_handles == 0);
                assert(rtl_device_usb_resume_reader(r.dev) == -ECANCELED);
                assert(!swap_state(r).thread_started);
            }
            rig_close(r);
        }
    }
    usb_world_reset(0);
}

/* A stop after the new handle was published and before the reader restarts: the reader never starts. And racing,
 * either the reader started before the stop (which joins it) or it never starts. */
DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
resume_thread(void* arg) {
    auto* r = static_cast<rig*>(arg);
    (void)rtl_device_usb_resume_reader(r->dev);
    DSD_THREAD_RETURN;
}

void
test_stop_after_publish_before_resume(void) {
    {
        rig r;
        rig_open(r, 1);
        rig_configure(r, settings{});
        rig_start(r);
        assert(recover(r) == 0);
        assert(rtl_device_stop_async(r.dev) == 0);
        assert(rtl_device_usb_resume_reader(r.dev) == -ECANCELED);
        const rtl_device_test_usb_swap_state st = swap_state(r);
        assert(!st.thread_started && !st.reader_running);
        rig_close(r);
    }
    for (int i = 0; i < 20; i++) {
        rig r;
        rig_open(r, 1);
        rig_configure(r, settings{});
        rig_start(r);
        assert(recover(r) == 0);
        g_stopper = stopper{r.dev, 0, -99};
        dsd_thread_t resumer;
        start_thread(&resumer, resume_thread, &r);
        dsd_thread_t stop;
        start_thread(&stop, stopper_thread, &g_stopper);
        dsd_thread_join(resumer);
        dsd_thread_join(stop);
        const rtl_device_test_usb_swap_state st = swap_state(r);
        assert(!st.thread_started && !st.reader_running);
        assert(rtl_device_usb_resume_reader(r.dev) == -ECANCELED);
        rig_close(r);
    }
}

/* ---- A control that took and cannot be restored fails the recovery ---- */

enum restore_control {
    RESTORE_BIAS,
    RESTORE_GAIN,
    RESTORE_PPM,
    RESTORE_XTAL,
    RESTORE_IF_GAIN,
    RESTORE_TESTMODE,
    RESTORE_DIRECT_SAMPLING,
    RESTORE_OFFSET,
    RESTORE_BANDWIDTH,
    RESTORE_FREQ,
    RESTORE_RATE,
    RESTORE_COUNT
};

const char* const k_restore_names[RESTORE_COUNT] = {
    "bias tee",        "gain",          "PPM",       "crystal",   "IF gain",     "test mode",
    "direct sampling", "offset tuning", "bandwidth", "frequency", "sample rate",
};

void
test_previously_effective_control_restore_failure_fails(void) {
    for (int c = 0; c < RESTORE_COUNT; c++) {
        if (c == RESTORE_BIAS && !g_bias_api) {
            continue;
        }
        rig r;
        rig_open(r, 1);
        settings s;
        if (c == RESTORE_DIRECT_SAMPLING) {
            s.direct_sampling = 1;
        } else {
            s.offset = 1;
        }
        s.rtl_xtal = 28800100U;
        s.set_bw = 1;
        s.bw = 2000000U;
        s.ppm = 3;
        s.gain = 240;
        s.testmode = (c == RESTORE_TESTMODE) ? 1 : 0;
        s.if_stage = 1;
        s.if_gain = 30;
        s.bias = 1;
        rig_configure(r, s);
        rig_start(r);
        rtl_device_test_usb_dongle& d = r.dongle;
        switch (c) {
            case RESTORE_BIAS: d.faults[RTL_TEST_DONGLE_BIAS_TEE].fail_from_call = 1; break;
            case RESTORE_GAIN:
                d.gain.gain_fault.fail_value_set = 1;
                d.gain.gain_fault.fail_value = 240;
                break;
            case RESTORE_PPM: d.faults[RTL_TEST_DONGLE_PPM].fail_from_call = 1; break;
            case RESTORE_XTAL: d.faults[RTL_TEST_DONGLE_XTAL].fail_from_call = 1; break;
            case RESTORE_IF_GAIN: d.faults[RTL_TEST_DONGLE_IF_GAIN].fail_from_call = 1; break;
            case RESTORE_TESTMODE:
                d.gain.reg19_fault.fail_value_set = 1;
                d.gain.reg19_fault.fail_value = 0x03;
                break;
            case RESTORE_DIRECT_SAMPLING: d.faults[RTL_TEST_DONGLE_DIRECT_SAMPLING].fail_from_call = 1; break;
            case RESTORE_OFFSET: d.faults[RTL_TEST_DONGLE_OFFSET].fail_from_call = 1; break;
            case RESTORE_BANDWIDTH: d.faults[RTL_TEST_DONGLE_BANDWIDTH].fail_from_call = 1; break;
            case RESTORE_FREQ: d.faults[RTL_TEST_DONGLE_FREQ].fail_from_call = 1; break;
            default: d.faults[RTL_TEST_DONGLE_RATE].fail_from_call = 1; break;
        }
        /* The faults count calls from here on. */
        for (int op = 0; op < RTL_TEST_DONGLE_OP_COUNT; op++) {
            d.calls[op] = 0;
        }
        d.gain.reg19_calls = 0;
        d.gain.mode_calls = 0;
        d.gain.gain_calls = 0;
        std::string why;
        const int rc = recover(r, &why);
        if (rc == 0 || why.find(k_restore_names[c]) == std::string::npos) {
            DSD_FPRINTF(stderr, "restore failure of %s: rc=%d why=\"%s\"\n", k_restore_names[c], rc, why.c_str());
        }
        assert(rc < 0 && rc != -ECANCELED);
        assert(why.find(k_restore_names[c]) != std::string::npos);
        /* The half-restored dongle was closed, never published, and its FIFO never reset for a reader. */
        rtl_device_test_usb_swap_state st = swap_state(r);
        assert(!st.has_handle && !st.thread_started && !st.recovering);
        assert(d.open_handles == 0);
        assert(op_index(d, RTL_TEST_DONGLE_RESET_BUFFER, 0) < 0);
        /* The reader never restarts. */
        assert(rtl_device_usb_resume_reader(r.dev) < 0);
        st = swap_state(r);
        assert(!st.thread_started && !st.reader_running);
        rig_close(r);
    }
}

/* A dongle that comes back on another port after the reset is recorded there. */
void
test_reenumerated_port_recorded(void) {
    usb_world_reset(0);
    rig r;
    rig_open(r, 1);
    rig_configure(r, settings{});
    rig_start(r);
    g_usb.devs[0].gone_on_reset = 1;
    g_usb.devs[0].port_after = 4;
    assert(recover(r) == 0);
    rtl_device_test_usb_swap_state st = swap_state(r);
    assert(st.recovery_on && st.identity_port_depth == 1 && st.identity_ports[0] == 4);
    /* And the next reset reaches it there. */
    g_usb.devs[0].gone_on_reset = 0;
    assert(rtl_device_usb_resume_reader(r.dev) == 0);
    assert(recover(r) == 0);
    assert(g_usb.devs[0].resets.load() == 2);
    rig_close(r);
    usb_world_reset(0);
}

/* The dongle not coming back fails the recovery with the reset's cause, and leaves no handle. */
void
test_reset_not_back_fails(void) {
    usb_world_reset(0);
    rig r;
    rig_open(r, 1);
    rig_configure(r, settings{});
    rig_start(r);
    g_usb.devs[0].gone_on_reset = 1;
    g_usb.devs[0].never_back = 1;
    std::string why;
    const int rc = recover(r, &why);
    assert(rc < 0 && rc != -ECANCELED);
    assert(why.find("not found") != std::string::npos);
    assert(!swap_state(r).has_handle);
    (void)log_take();
    rig_close(r);
    usb_world_reset(0);
}

/* A receiver whose identity is not unique is never reset; its open logs why, once. */
void
test_ineligible_recovery_off_reason_logged(void) {
    (void)log_take();
    usb_world_reset(1);
    rig r;
    rig_open(r, 1);
    std::string log = log_take();
#ifdef DSD_HAVE_LIBUSB
    const char* reason = "another device has the same serial (set a unique one with rtl_eeprom)";
#else
    const char* reason = "built without libusb";
#endif
    const std::string line = std::string("RTL-SDR: stall recovery is off for this receiver: ") + reason;
    if (log.find(line) == std::string::npos) {
        DSD_FPRINTF(stderr, "log was: %s\n", log.c_str());
    }
    assert(log.find(line) != std::string::npos);
    assert(log.find(line, log.find(line) + 1U) == std::string::npos);
    assert(rtl_device_usb_recovery_eligible(r.dev) == 0);
    rig_configure(r, settings{});
    rig_start(r);
    std::string why;
    const int rc = recover(r, &why);
    assert(rc < 0 && rc != -ECANCELED);
    assert(why.find(reason) != std::string::npos);
    /* Nothing was touched: no reset, the handle and reader still there. */
    assert(g_usb.devs[0].resets.load() == 0 && g_usb.devs[1].resets.load() == 0);
    assert(r.dongle.calls[RTL_TEST_DONGLE_CLOSE] == 0);
    const rtl_device_test_usb_swap_state st = swap_state(r);
    assert(st.has_handle && st.thread_started && st.reader_running);
    rig_close(r);
    usb_world_reset(0);

    /* A unique receiver: nothing logged (with libusb). */
    rig ok;
    rig_open(ok, 1);
    log = log_take();
#ifdef DSD_HAVE_LIBUSB
    assert(log.find("stall recovery is off") == std::string::npos);
    assert(rtl_device_usb_recovery_eligible(ok.dev) == 1);
#else
    assert(log.find(line) != std::string::npos);
    assert(rtl_device_usb_recovery_eligible(ok.dev) == 0);
#endif
    rig_close(ok);
}

} // namespace

int
main(void) {
    assert(dsd_mutex_init(&g_log.lock) == 0);
    dsd_neo_log_set_tap(capture_log, &g_log);
    rtl_usb_identity_set_driver_ops(&k_usb_ops);
    /* The monitor never ends these sessions: no sample is ever delivered. */
    rtl_device_test_set_progress_limit_ms(3600000U);
    usb_world_reset(0);

    test_ineligible_recovery_off_reason_logged();
#ifdef DSD_HAVE_LIBUSB
    const int have_libusb = 1;
#else
    const int have_libusb = 0;
#endif
    /* Without libusb no receiver is eligible; the report above is all there is. */
    if (have_libusb) {
        test_swap_configures_before_reader_runs();
        test_replay_restores_effective_not_initial();
        test_replay_direct_sampling_r820t();
        test_failed_retune_not_replayed();
        test_setter_during_swap_returns_busy();
        test_setter_before_retire_is_replayed();
        test_getter_and_setter_in_flight_during_retire();
        test_stop_at_each_swap_step();
        test_stop_after_publish_before_resume();
        test_previously_effective_control_restore_failure_fails();
        test_reenumerated_port_recorded();
        test_reset_not_back_fails();
    }

    rtl_device_test_set_progress_limit_ms(0U);
    rtl_usb_identity_set_driver_ops(nullptr);
    DSD_FPRINTF(stderr, "IO_RTL_USB_SWAP: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
