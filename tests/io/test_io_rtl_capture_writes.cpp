// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #782 D3: a retune writes only the capture settings that changed. Every hop used to set the sample rate again
 * (on an RTL-SDR: the tuner filter, the resampler, the PPM correction and a demod soft reset while it streams), then
 * the tuner bandwidth, and a scan target's gain. Each case runs the stream's own programming (its start, then hops and
 * gain changes) on a whole fake dongle opened from a fake Android descriptor, and counts what reaches the dongle:
 *
 * - hops at the rate the device was last set to write the frequency alone, and the stream runs the rate the device
 *   read back then: a quantized rate is never rewritten, and the capture centre, output rate and forced flag follow it;
 * - a changed rate is written once, read back, and the bandwidth with it;
 * - a scan target's gain is written on the hop where it changes, compared on the gain asked for, not the nearest one
 *   the tuner took;
 * - a failed write, a PPM or offset tuning change, and a USB recovery's replay make the next hop write again;
 * - a SoapySDR device keeps every write, whatever its driver;
 * - a tuner whose tune undoes a setting has it written again on every hop: an FC2580 its bandwidth, a tuner this build
 *   does not know everything (gate r3 J1).
 *
 * The record is a local USB dongle's alone: an rtl_tcp server keeps every write behind any tuner (gate r4 K2), which
 * IO_RTL_TCP_RECONNECT's hops_write_every_setting pins against its fake server.
 *
 * Issue #670 B4 and B16, on the same fake dongle:
 *
 * - a centre above 2147.483647 MHz is programmed as it is, on the start and on a retune (it used to be narrowed through
 *   int, which programmed the dongle near 0 Hz), and a capture frequency past UINT32_MAX is refused with nothing
 *   written instead of saturated;
 * - a retune whose actual-rate correction cannot be written (the write fails, or the corrected capture is past
 *   UINT32_MAX), after a rate write that took or one that failed, is refused: the device goes back on the capture it had, the controller finalizes on the centre it
 *   kept and the retune completes as failed; a device that will not go back stops the stream.
 */

#include <assert.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <memory>
#include "dsd-neo/core/opts_fwd.h"
#include "rtl_stream_test_support.h"
#include "rtl_usb_identity.h"

struct rtl_device;

extern "C" void rtl_device_test_set_open_fd_failure_hook(int (*hook)(int));

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

constexpr uint32_t kCenterHz = 851012500U;
constexpr uint32_t kHopsHz[] = {851262500U, 852037500U, 853087500U};
constexpr int kFd = 41;
constexpr int kTunerUnknown = 0;
constexpr int kTunerE4000 = 1;
constexpr int kTunerFC0012 = 2;
constexpr int kTunerFC0013 = 3;
constexpr int kTunerFC2580 = 4;
constexpr int kTunerR820T = 5;
constexpr int kTunerR828D = 6;
constexpr int kRateIn = 48000;
/* An R820T's gain table in tenths of a dB: 30 dB is asked for, 29.7 dB takes. */
const int k_gains[] = {0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372};

/* ---- The descriptor seams: the open, USBDEVFS_RESET and the identity read never reach a real descriptor ---- */

int
fake_open_fd(int fd) {
    (void)fd;
    return 1;
}

int
fake_fd_reset(int fd) {
    (void)fd;
    return 0;
}

int
fake_fd_identity(int fd, rtl_usb_identity* out) {
    (void)fd;
    out->vid = 0x0bda;
    out->pid = 0x2838;
    DSD_SNPRINTF(out->manufacturer, sizeof out->manufacturer, "%s", "Nooelec");
    DSD_SNPRINTF(out->product, sizeof out->product, "%s", "NESDR SMArt v5");
    DSD_SNPRINTF(out->serial, sizeof out->serial, "%s", "00000001");
    return 0;
}

dsd_opts*
make_rtl_opts(void) {
    dsd_opts* opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    assert(opts != nullptr);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0");
    return opts;
}

/* A stream's device on the fake dongle, its start programmed. */
struct rig {
    std::unique_ptr<input_ring_state> ring = std::make_unique<input_ring_state>();
    /* Heap-held: the device keeps its address in a global while it is installed. */
    std::unique_ptr<rtl_device_test_usb_dongle> dongle = std::make_unique<rtl_device_test_usb_dongle>();
    dsd_opts* opts = nullptr;
    rtl_device* dev = nullptr;
    rtl_stream_test_capture_result start{};
};

/* The fake dongle (reading back @p actual_rate for any rate it is given, 0: the rate given) and its device, with a
 * capture session begun on @p center_hz; the start's result in r.start. */
void
rig_begin(rig& r, int tuner_type, uint32_t center_hz, uint32_t actual_rate = 0U) {
    assert(input_ring_init(r.ring.get(), 1U << 18) == 0);
    DSD_MEMSET(r.dongle.get(), 0, sizeof *r.dongle);
    r.dongle->tuner.tuner_type = tuner_type;
    r.dongle->tuner.actual_rate = actual_rate;
    r.dongle->gain.gain_count = (int)(sizeof k_gains / sizeof k_gains[0]);
    DSD_MEMCPY(r.dongle->gain.gains, k_gains, sizeof k_gains);
    rtl_device_test_usb_dongle_install(r.dongle.get());
    rtl_device_publish_preopened_fd(kFd, 0);
    r.dev = rtl_device_create(0, r.ring.get());
    assert(r.dev != nullptr);
    assert(rtl_device_usb_recovery_eligible(r.dev) == 1);
    r.opts = make_rtl_opts();
    assert(rtl_stream_test_capture_session_begin(r.dev, r.opts, center_hz, &r.start) == 0);
}

void
rig_open(rig& r, int tuner_type, uint32_t actual_rate = 0U) {
    rig_begin(r, tuner_type, kCenterHz, actual_rate);
    assert(r.start.rc == 0);
}

void
rig_close(rig& r) {
    rtl_stream_test_capture_session_end();
    rtl_device_destroy(r.dev);
    r.dev = nullptr;
    rtl_device_test_usb_dongle_install(nullptr);
    rtl_device_set_preopened_fd(-1);
    assert(r.dongle->stale_handle_calls == 0);
    input_ring_destroy(r.ring.get());
    free(r.opts);
    r.opts = nullptr;
}

/* What reached the dongle, counted from a mark. */
struct writes {
    int freq, rate, bandwidth, gain_mode, gain, reg19, ppm, offset;
};

writes
writes_now(const rig& r) {
    const rtl_device_test_usb_dongle* d = r.dongle.get();
    return writes{d->calls[RTL_TEST_DONGLE_FREQ],
                  d->calls[RTL_TEST_DONGLE_RATE],
                  d->calls[RTL_TEST_DONGLE_BANDWIDTH],
                  d->gain.mode_calls,
                  d->gain.gain_calls,
                  d->gain.reg19_calls,
                  d->calls[RTL_TEST_DONGLE_PPM],
                  d->calls[RTL_TEST_DONGLE_OFFSET]};
}

writes
writes_since(const rig& r, const writes& mark) {
    const writes now = writes_now(r);
    return writes{now.freq - mark.freq,
                  now.rate - mark.rate,
                  now.bandwidth - mark.bandwidth,
                  now.gain_mode - mark.gain_mode,
                  now.gain - mark.gain,
                  now.reg19 - mark.reg19,
                  now.ppm - mark.ppm,
                  now.offset - mark.offset};
}

int
gain_writes(const writes& w) {
    return w.gain_mode + w.gain + w.reg19;
}

rtl_stream_test_capture_result
hop(uint32_t center_hz, int rate_in = 0) {
    rtl_stream_test_capture_result out{};
    assert(rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_HOP, center_hz, rate_in, &out) == 0);
    return out;
}

int
step(int kind, int value) {
    rtl_stream_test_capture_result out{};
    assert(rtl_stream_test_capture_session_step(kind, 0U, value, &out) == 0);
    return out.rc;
}

/* The capture a hop to @p center_hz runs when the device delivers @p rate_hz: the stream's rate chain on that rate,
 * the dongle tuned to the centre the stream asked for. */
void
expect_capture_at(const rig& r, const rtl_stream_test_capture_result& got, uint32_t center_hz, uint32_t rate_hz,
                  int forced) {
    assert(got.rc == 0);
    assert(got.capture_rate_hz == rate_hz);
    assert(got.rate_out_hz == (int)(rate_hz >> got.downsample_passes));
    assert(got.rate_forced == forced);
    const uint32_t shift = got.fs4_rotation ? rate_hz / 4U : 0U;
    assert(got.capture_freq_hz == center_hz + shift);
    assert(r.dongle->tuner.freq == got.capture_freq_hz);
    assert(r.dongle->tuner.rate == rate_hz);
}

/* ---- Cases ---- */

/* Hops at the rate the device was last set to write the frequency alone: no rate, no bandwidth, no gain. */
void
test_same_rate_hops_write_frequency_only(void) {
    rig r;
    rig_open(r, kTunerE4000);
    assert(r.start.offset_tuning == 1);
    const writes mark = writes_now(r);
    for (uint32_t center : kHopsHz) {
        expect_capture_at(r, hop(center), center, r.start.capture_rate_hz, 0);
    }
    const writes w = writes_since(r, mark);
    assert(w.freq == 3);
    assert(w.rate == 0);
    assert(w.bandwidth == 0);
    assert(gain_writes(w) == 0);
    assert(w.offset == 0);
    /* The LO offset the start computed from the rate stands, with the filter opened for it. */
    assert(r.dongle->tuner.offs_freq == (r.dongle->tuner.rate / 2U) * 170U / 100U);
    assert(r.dongle->tuner.bw >= 2U * r.dongle->tuner.offs_freq);
    rig_close(r);
}

/* A rate the device quantizes (requested != actual) is not written again on every hop, and the stream runs the rate it
 * read back: the Fs/4 centre, the output rate and the forced flag follow it as the readback after a write sets them. */
void
test_quantized_rate_not_rewritten(void) {
    rig r;
    rig_open(r, kTunerR820T, 1535999U);
    assert(r.start.offset_tuning == 0 && r.start.fs4_rotation == 1);
    assert(r.start.capture_rate_hz == 1535999U);
    const writes mark = writes_now(r);
    for (uint32_t center : kHopsHz) {
        expect_capture_at(r, hop(center), center, 1535999U, 1);
    }
    const writes w = writes_since(r, mark);
    assert(w.freq == 3);
    assert(w.rate == 0);
    assert(w.bandwidth == 0);
    rig_close(r);
}

/* A changed rate is written once and read back, with the bandwidth for it; the hops after it write it no more. */
void
test_changed_rate_written_once_and_read_back(void) {
    rig r;
    rig_open(r, kTunerR820T);
    const uint32_t start_rate = r.start.capture_rate_hz;
    /* The device quantizes the next rate it is given: what the stream runs is the readback. */
    r.dongle->tuner.actual_rate = 960123U;
    writes mark = writes_now(r);
    const rtl_stream_test_capture_result changed = hop(kHopsHz[0], 30000);
    writes w = writes_since(r, mark);
    assert(w.rate == 1);
    assert(w.bandwidth == 1);
    assert(gain_writes(w) == 0);
    assert(start_rate != 960123U);
    expect_capture_at(r, changed, kHopsHz[0], 960123U, 1);

    mark = writes_now(r);
    expect_capture_at(r, hop(kHopsHz[1], 30000), kHopsHz[1], 960123U, 1);
    expect_capture_at(r, hop(kHopsHz[2]), kHopsHz[2], 960123U, 1);
    w = writes_since(r, mark);
    assert(w.freq == 2 && w.rate == 0 && w.bandwidth == 0);
    rig_close(r);
}

/* A scan target's gain: compared on the gain asked for, so 30 dB, which takes as 29.7 dB, is written once; a change,
 * the mode, and the autogain supervisor's own gain each make the next request write. */
void
test_gain_written_when_changed(void) {
    rig r;
    rig_open(r, kTunerR820T);
    writes mark = writes_now(r);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    assert(r.dongle->gain.gain == 297);
    assert(writes_since(r, mark).gain == 1);
    int gain = 0;
    int is_auto = -1;
    assert(rtl_stream_get_gain(&gain, &is_auto) == 0 && gain == 297 && is_auto == 0);

    mark = writes_now(r);
    for (uint32_t center : kHopsHz) {
        (void)hop(center);
        assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    }
    assert(gain_writes(writes_since(r, mark)) == 0);

    mark = writes_now(r);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 330) == 0);
    assert(writes_since(r, mark).gain == 1 && r.dongle->gain.gain == 328);

    mark = writes_now(r);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_AUTO, 0) == 0);
    assert(writes_since(r, mark).gain_mode == 1 && r.dongle->gain.manual == 0);
    mark = writes_now(r);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_AUTO, 0) == 0);
    assert(gain_writes(writes_since(r, mark)) == 0);

    /* The supervisor moved the gain: the target's 30 dB applies again. */
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    assert(r.dongle->gain.gain == 297);
    assert(step(RTL_STREAM_TEST_CAPTURE_AUTOGAIN, 250) == 0);
    assert(r.dongle->gain.gain == 254);
    mark = writes_now(r);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    assert(writes_since(r, mark).gain == 1 && r.dongle->gain.gain == 297);
    rig_close(r);
}

/* A failed gain change leaves nothing recorded: the next request is written, the one recorded before it included. */
void
test_failed_gain_rewritten(void) {
    rig r;
    rig_open(r, kTunerR820T);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    r.dongle->gain.gain_fault.fail_value_set = 1;
    r.dongle->gain.gain_fault.fail_value = 328;
    /* The retune goes on (the profile's gain is logged as failed), on the gain rolled back. */
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 330) == 0);
    assert(r.dongle->gain.gain == 297);
    r.dongle->gain.gain_fault.fail_value_set = 0;
    writes mark = writes_now(r);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    assert(writes_since(r, mark).gain == 1 && r.dongle->gain.gain == 297);
    mark = writes_now(r);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 330) == 0);
    assert(writes_since(r, mark).gain == 1 && r.dongle->gain.gain == 328);
    rig_close(r);
}

/* A refused rate write leaves nothing recorded: the next hop writes its rate, even the one recorded before, and the
 * hops after it no more. */
void
test_failed_rate_rewritten(void) {
    rig r;
    rig_open(r, kTunerR820T);
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_from_call = r.dongle->calls[RTL_TEST_DONGLE_RATE] + 1;
    assert(hop(kHopsHz[0], 30000).rc != 0);
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_from_call = 0;
    /* Back on the rate recorded before the failure: written all the same. */
    writes mark = writes_now(r);
    expect_capture_at(r, hop(kHopsHz[1], kRateIn), kHopsHz[1], r.start.capture_rate_hz, 0);
    assert(writes_since(r, mark).rate == 1);
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_from_call = r.dongle->calls[RTL_TEST_DONGLE_RATE] + 1;
    assert(hop(kHopsHz[0], 30000).rc != 0);
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_from_call = 0;
    mark = writes_now(r);
    const rtl_stream_test_capture_result retried = hop(kHopsHz[1], 30000);
    assert(writes_since(r, mark).rate == 1);
    expect_capture_at(r, retried, kHopsHz[1], r.dongle->tuner.rate, 0);
    mark = writes_now(r);
    (void)hop(kHopsHz[2], 30000);
    assert(writes_since(r, mark).rate == 0);
    rig_close(r);
}

/* A refused frequency fails the hop and forgets the rate, which the next hop writes again. */
void
test_failed_frequency_rewrites_rate(void) {
    rig r;
    rig_open(r, kTunerR820T);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_from_call = r.dongle->calls[RTL_TEST_DONGLE_FREQ] + 1;
    assert(hop(kHopsHz[0]).rc != 0);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_from_call = 0;
    writes mark = writes_now(r);
    expect_capture_at(r, hop(kHopsHz[1]), kHopsHz[1], r.start.capture_rate_hz, 0);
    writes w = writes_since(r, mark);
    assert(w.rate == 1 && w.bandwidth == 1);
    rig_close(r);
}

/* A PPM change makes the next hop write the rate and the bandwidth; the same correction again changes nothing. */
void
test_ppm_change_rewrites(void) {
    rig r;
    rig_open(r, kTunerR820T);
    assert(step(RTL_STREAM_TEST_CAPTURE_PPM, 5) == 0);
    writes mark = writes_now(r);
    (void)hop(kHopsHz[0]);
    writes w = writes_since(r, mark);
    assert(w.rate == 1 && w.bandwidth == 1);
    assert(step(RTL_STREAM_TEST_CAPTURE_PPM, 5) == 0);
    mark = writes_now(r);
    (void)hop(kHopsHz[1]);
    w = writes_since(r, mark);
    assert(w.rate == 0 && w.bandwidth == 0 && w.ppm == 0);
    rig_close(r);
}

/* An offset tuning change makes the next hop write the rate (librtlsdr re-applies the LO offset in it) and the
 * bandwidth for it. */
void
test_offset_tuning_change_rewrites(void) {
    rig r;
    rig_open(r, kTunerE4000);
    assert(r.start.offset_tuning == 1);
    assert(step(RTL_STREAM_TEST_CAPTURE_OFFSET, 0) == 0);
    writes mark = writes_now(r);
    const rtl_stream_test_capture_result off = hop(kHopsHz[0]);
    writes w = writes_since(r, mark);
    assert(w.rate == 1 && w.bandwidth == 1);
    assert(off.offset_tuning == 0 && off.fs4_rotation == 1);
    expect_capture_at(r, off, kHopsHz[0], r.start.capture_rate_hz, 0);
    assert(r.dongle->tuner.offs_freq == 0U);
    mark = writes_now(r);
    (void)hop(kHopsHz[1]);
    assert(writes_since(r, mark).rate == 0);
    rig_close(r);
}

/* A device that reads back another rate than the one recorded (changed behind the stream) has it written again. */
void
test_readback_disagreeing_rewrites(void) {
    rig r;
    rig_open(r, kTunerR820T);
    r.dongle->tuner.rate = 2048000U;
    const writes mark = writes_now(r);
    expect_capture_at(r, hop(kHopsHz[0]), kHopsHz[0], r.start.capture_rate_hz, 0);
    const writes w = writes_since(r, mark);
    assert(w.rate == 1 && w.bandwidth == 1);
    rig_close(r);
}

/* A USB recovery's reopen and replay leave nothing recorded: the next hop writes the rate and the bandwidth, and the
 * next gain request the gain, then they settle again. */
void
test_recovery_rewrites(void) {
    rig r;
    rig_open(r, kTunerR820T);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    (void)hop(kHopsHz[0]);
    uint32_t before = 0U;
    assert(rtl_device_capture_settings_generation(r.dev, &before) == 1);
    char why[256] = "";
    assert(rtl_device_usb_recover_handle(r.dev, why, sizeof why) == 0);
    uint32_t after = 0U;
    assert(rtl_device_capture_settings_generation(r.dev, &after) == 1);
    assert(after != before);

    writes mark = writes_now(r);
    expect_capture_at(r, hop(kHopsHz[1]), kHopsHz[1], r.start.capture_rate_hz, 0);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    writes w = writes_since(r, mark);
    assert(w.rate == 1 && w.bandwidth == 1 && w.gain == 1);

    mark = writes_now(r);
    (void)hop(kHopsHz[2]);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    w = writes_since(r, mark);
    assert(w.rate == 0 && w.bandwidth == 0 && gain_writes(w) == 0);
    rig_close(r);
}

/* Gate r3 J1: an FC2580 sets its channel filter for the band on every tune (fc2580_set_freq(): 7.8 MHz on UHF), which
 * only a bandwidth write puts back. Each same-rate hop writes the frequency and then the bandwidth again, and the
 * filter ends on what the stream asked for; the rate and a repeated gain stay unwritten. */
void
test_fc2580_hops_rewrite_bandwidth(void) {
    rig r;
    rig_open(r, kTunerFC2580);
    const uint32_t asked_bw = r.dongle->tuner.bw;
    assert(asked_bw != 0U);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    const writes mark = writes_now(r);
    for (uint32_t center : kHopsHz) {
        const writes before = writes_now(r);
        expect_capture_at(r, hop(center), center, r.start.capture_rate_hz, 0);
        assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
        const writes w = writes_since(r, before);
        assert(w.freq == 1 && w.bandwidth == 1);
        /* The filter the tune reset is the one the stream asked for again. */
        assert(r.dongle->tuner.bw == asked_bw);
        assert(r.dongle->tuner.filter_bw == asked_bw);
    }
    const writes w = writes_since(r, mark);
    assert(w.freq == 3 && w.bandwidth == 3);
    assert(w.rate == 0);
    assert(gain_writes(w) == 0);
    assert(w.offset == 0);
    rig_close(r);
}

/* The tuners whose tune leaves the rate, the filter and the gain as they were (rtl_device_capture_retune_resets()):
 * same-rate hops with a repeated gain write the frequency alone, as before J1. */
void
test_safe_tuners_skip_unchanged(void) {
    const int tuners[] = {kTunerE4000, kTunerFC0012, kTunerFC0013, kTunerR820T, kTunerR828D};
    for (int tuner : tuners) {
        rig r;
        rig_open(r, tuner);
        assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
        const uint32_t filter_bw = r.dongle->tuner.filter_bw;
        const writes mark = writes_now(r);
        for (uint32_t center : kHopsHz) {
            expect_capture_at(r, hop(center), center, r.start.capture_rate_hz, 0);
            assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
        }
        const writes w = writes_since(r, mark);
        assert(w.freq == 3);
        assert(w.rate == 0 && w.bandwidth == 0 && gain_writes(w) == 0);
        assert(r.dongle->tuner.filter_bw == filter_bw);
        rig_close(r);
    }
}

/* A tuner this build does not know may undo anything when it tunes: every hop writes the rate, the bandwidth and the
 * gain again, as every hop did before the record. */
void
test_unknown_tuner_rewrites_every_hop(void) {
    rig r;
    rig_open(r, kTunerUnknown);
    assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
    for (uint32_t center : kHopsHz) {
        const writes before = writes_now(r);
        assert(hop(center).rc == 0);
        assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
        const writes w = writes_since(r, before);
        assert(w.freq >= 1 && w.rate == 1 && w.bandwidth == 1 && w.gain == 1);
    }
    rig_close(r);
}

/* A SoapySDR device keeps every write, whatever its driver: a generic one, and ones keyed as an RTL-SDR or an SDRplay
 * alike write the rate on every same-rate hop and the gain on every repeated request. */
void
test_soapy_rewrites_every_hop(void) {
    const char* const keys[] = {"generic", "RTLSDR", "SDRplay"};
    for (const char* key : keys) {
        rtl_device_test_soapy_gain_fake fake{};
        fake.driver_key = key;
        fake.selected_db = 29.7;
        rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
        if (dev == nullptr) {
            printf("IO_RTL_CAPTURE_WRITES: no SoapySDR in this build; Soapy case skipped\n");
            return;
        }
        uint32_t generation = 0U;
        assert(rtl_device_capture_settings_generation(dev, &generation) == 0);
        dsd_opts* opts = make_rtl_opts();
        rtl_stream_test_capture_result start{};
        assert(rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, &start) == 0);
        assert(start.rc == 0 && fake.rate_writes == 1);
        for (uint32_t center : kHopsHz) {
            assert(hop(center).rc == 0);
        }
        assert(fake.rate_writes == 4);
        assert(fake.frequency_writes >= 4);
        const int gain_writes_before = fake.writes;
        assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
        assert(step(RTL_STREAM_TEST_CAPTURE_GAIN_MANUAL, 300) == 0);
        assert(fake.writes - gain_writes_before == 2);
        rtl_stream_test_capture_session_end();
        rtl_device_test_soapy_gain_device_free(dev);
        free(opts);
    }
}

/* ---- Issue #670 B4: centres above 2147.483647 MHz, and capture frequencies past UINT32_MAX ---- */

constexpr uint32_t kAbove2147Hz = 2200000000U;
/* Demod input rate a retune asks for to run the capture at 960 kS/s (32x). */
constexpr int kRateIn960k = 30000;
constexpr uint32_t kCapture960kHz = 960000U;

/* The centre frequencies the dongle took from the @p from-th on. */
uint32_t
freq_taken(const rig& r, int from) {
    assert(from < r.dongle->tuner.freq_log_count);
    return r.dongle->tuner.freq_log[from];
}

/* A retune the controller applies, finalizes and completes, to @p center_hz at demod input rate @p rate_in. */
rtl_stream_test_capture_result
manual_retune(uint32_t center_hz, int rate_in) {
    rtl_stream_test_capture_result out{};
    assert(rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_MANUAL_RETUNE, center_hz, rate_in, &out) == 0);
    return out;
}

/* A retune from 851 MHz to 2.2 GHz with the Fs/4 shift at 960 kS/s: the first frequency the dongle is given is the
 * shifted 2.2 GHz centre, and the retune lands. */
void
test_retune_above_2147_mhz(void) {
    rig r;
    rig_open(r, kTunerR820T);
    assert(r.start.fs4_rotation == 1);
    const int from = r.dongle->tuner.freq_log_count;
    const rtl_stream_test_capture_result got = manual_retune(kAbove2147Hz, kRateIn960k);
    assert(r.dongle->tuner.freq_log_count > from);
    assert(freq_taken(r, from) == 2200240000U);
    expect_capture_at(r, got, kAbove2147Hz, kCapture960kHz, 0);
    assert(got.applied_hz == kAbove2147Hz);
    assert(got.completion == RTL_STREAM_TUNE_OK);
    rig_close(r);
}

/* A start on 2.2 GHz: the first frequency the dongle is given is the shifted 2.2 GHz centre. */
void
test_start_above_2147_mhz(void) {
    rig r;
    rig_begin(r, kTunerR820T, kAbove2147Hz);
    assert(r.start.rc == 0);
    assert(r.start.fs4_rotation == 1);
    assert(r.dongle->tuner.freq_log_count >= 1);
    assert(freq_taken(r, 0) == kAbove2147Hz + r.start.capture_rate_hz / 4U);
    assert(r.dongle->tuner.freq == kAbove2147Hz + r.start.capture_rate_hz / 4U);
    rig_close(r);
}

/* A retune to 4294.967295 MHz with the Fs/4 shift on: its capture frequency is past UINT32_MAX, so the retune is
 * refused before anything is written, the stream keeps the capture it ran, and the retune completes as failed. */
void
test_retune_past_uint32_refused(void) {
    rig r;
    rig_open(r, kTunerR820T);
    const writes mark = writes_now(r);
    const rtl_stream_test_capture_result got = manual_retune(UINT32_MAX, kRateIn960k);
    const writes w = writes_since(r, mark);
    assert(got.rc != 0);
    assert(w.freq == 0 && w.rate == 0 && w.bandwidth == 0);
    assert(got.capture_freq_hz == r.start.capture_freq_hz);
    assert(got.capture_rate_hz == r.start.capture_rate_hz);
    assert(got.rate_out_hz == r.start.rate_out_hz);
    assert(got.applied_hz == kCenterHz);
    assert(got.completion == RTL_STREAM_TUNE_FAILED);
    assert(r.dongle->tuner.freq == r.start.capture_freq_hz);
    rig_close(r);
}

/* A start on 4294.967295 MHz with the Fs/4 shift on fails with nothing written. */
void
test_start_past_uint32_refused(void) {
    rig r;
    rig_begin(r, kTunerR820T, UINT32_MAX);
    assert(r.start.rc != 0);
    assert(r.dongle->calls[RTL_TEST_DONGLE_FREQ] == 0);
    assert(r.dongle->calls[RTL_TEST_DONGLE_RATE] == 0);
    rig_close(r);
}

/* ---- Issue #670 B16: a retune whose actual-rate correction cannot be written ---- */

/* Stops the dongle reading back another rate on its at_call-th rate write: the rate a refused retune's rollback puts
 * back reads back as given. */
struct readback_reset {
    rtl_device_test_usb_dongle* dongle;
    int at_call;
};

void
readback_reset_on_op(void* ctx, int op) {
    readback_reset* reset = static_cast<readback_reset*>(ctx);
    /* Called before the op is counted. */
    if (op == RTL_TEST_DONGLE_RATE && reset->dongle->calls[RTL_TEST_DONGLE_RATE] + 1 == reset->at_call) {
        reset->dongle->tuner.actual_rate = 0U;
    }
}

/* A rig whose next retune's rate write reads back @p actual_rate and whose rollback's rate write reads back as given. */
void
arm_readback(rig& r, readback_reset* reset, uint32_t actual_rate) {
    r.dongle->tuner.actual_rate = actual_rate;
    reset->dongle = r.dongle.get();
    reset->at_call = r.dongle->calls[RTL_TEST_DONGLE_RATE] + 2;
    r.dongle->on_op = readback_reset_on_op;
    r.dongle->on_op_ctx = reset;
}

void
disarm_readback(rig& r) {
    r.dongle->on_op = nullptr;
    r.dongle->on_op_ctx = nullptr;
}

/* A refused retune that put the device back: the stream runs the capture it started on, the controller finalized on
 * the centre it kept, and the retune completes as failed. */
void
expect_rolled_back(const rig& r, const rtl_stream_test_capture_result& got) {
    assert(got.rc != 0);
    assert(got.completion == RTL_STREAM_TUNE_FAILED);
    assert(got.applied_hz == kCenterHz);
    assert(got.capture_freq_hz == r.start.capture_freq_hz);
    assert(got.capture_rate_hz == r.start.capture_rate_hz);
    assert(got.rate_out_hz == r.start.rate_out_hz);
    assert(got.rate_forced == r.start.rate_forced);
    assert(r.dongle->tuner.freq == r.start.capture_freq_hz);
    assert(r.dongle->tuner.rate == r.start.capture_rate_hz);
    assert(dsd_exitflag_load() == 0);
}

/* The device reads back 960123 Hz for 960 kS/s and refuses the frequency correction for it: the retune is refused and
 * rolled back onto the capture the stream ran. */
void
test_failed_rate_correction_rolls_back(void) {
    rig r;
    rig_open(r, kTunerR820T);
    readback_reset reset{};
    arm_readback(r, &reset, 960123U);
    const uint32_t corrected_hz = kHopsHz[0] + 960123U / 4U;
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 1;
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value = (int)corrected_hz;
    const int from = r.dongle->tuner.freq_log_count;
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], kRateIn960k);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 0;
    disarm_readback(r);
    expect_rolled_back(r, got);
    /* The retune's capture, then the one the stream ran, put back. */
    assert(r.dongle->tuner.freq_log_count == from + 2);
    assert(freq_taken(r, from) == kHopsHz[0] + kCapture960kHz / 4U);
    assert(freq_taken(r, from + 1) == r.start.capture_freq_hz);
    rig_close(r);
}

/* The device reads back 960004 Hz for 960 kS/s on a centre whose capture is UINT32_MAX: the correction would be past
 * it, so the retune is refused and rolled back without writing it. */
void
test_rate_correction_past_uint32_rolls_back(void) {
    rig r;
    rig_open(r, kTunerR820T);
    readback_reset reset{};
    arm_readback(r, &reset, 960004U);
    const uint32_t center_hz = UINT32_MAX - kCapture960kHz / 4U;
    const int from = r.dongle->tuner.freq_log_count;
    const rtl_stream_test_capture_result got = manual_retune(center_hz, kRateIn960k);
    disarm_readback(r);
    expect_rolled_back(r, got);
    assert(r.dongle->tuner.freq_log_count == from + 2);
    assert(freq_taken(r, from) == UINT32_MAX);
    assert(freq_taken(r, from + 1) == r.start.capture_freq_hz);
    rig_close(r);
}

/* The rate write for 960 kS/s is refused and the device still runs the rate it had, whose capture correction it refuses
 * too: the retune is refused and rolled back onto the capture the stream ran, as after a rate write that took. */
void
test_failed_rate_write_correction_rolls_back(void) {
    rig r;
    rig_open(r, kTunerR820T);
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 1;
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value = (int)kCapture960kHz;
    const uint32_t corrected_hz = kHopsHz[0] + r.start.capture_rate_hz / 4U;
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 1;
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value = (int)corrected_hz;
    const int from = r.dongle->tuner.freq_log_count;
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], kRateIn960k);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 0;
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 0;
    expect_rolled_back(r, got);
    assert(r.dongle->tuner.freq_log_count == from + 2);
    assert(freq_taken(r, from) == kHopsHz[0] + kCapture960kHz / 4U);
    assert(freq_taken(r, from + 1) == r.start.capture_freq_hz);
    rig_close(r);
}

/* The correction fails and so does the rollback's frequency write: the device may still run the retune's capture, so
 * the stream stops, reported as a device failure of the input. */
void
test_failed_rollback_stops_stream(void) {
    rig r;
    rig_open(r, kTunerR820T);
    readback_reset reset{};
    arm_readback(r, &reset, 960123U);
    /* The retune's own frequency write takes; the correction and the rollback's are refused. */
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_from_call = r.dongle->calls[RTL_TEST_DONGLE_FREQ] + 2;
    dsd_input_failure_clear();
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], kRateIn960k);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_from_call = 0;
    disarm_readback(r);
    assert(got.rc != 0);
    assert(got.completion == RTL_STREAM_TUNE_FAILED);
    assert(got.applied_hz == kCenterHz);
    assert(dsd_exitflag_load() != 0);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    dsd_exitflag_store(0);
    dsd_input_failure_clear();
    rig_close(r);
}

} // namespace

int
main(int argc, char** argv) {
    (void)dsd_unsetenv("DSD_NEO_TUNER_BW_HZ");
    (void)dsd_unsetenv("DSD_NEO_RTL_OFFSET_TUNING");
    dsd_neo_config_init();
    rtl_device_test_set_open_fd_failure_hook(fake_open_fd);
    rtl_device_test_set_fd_reset_hook(fake_fd_reset);
    rtl_device_test_set_fd_identity_hook(fake_fd_identity);

    static const struct {
        const char* name;
        void (*run)(void);
    } k_cases[] = {
        {"same_rate_hops", test_same_rate_hops_write_frequency_only},
        {"quantized_rate", test_quantized_rate_not_rewritten},
        {"changed_rate", test_changed_rate_written_once_and_read_back},
        {"gain", test_gain_written_when_changed},
        {"failed_gain", test_failed_gain_rewritten},
        {"failed_rate", test_failed_rate_rewritten},
        {"failed_frequency", test_failed_frequency_rewrites_rate},
        {"ppm", test_ppm_change_rewrites},
        {"offset_tuning", test_offset_tuning_change_rewrites},
        {"readback", test_readback_disagreeing_rewrites},
        {"recovery", test_recovery_rewrites},
        {"soapy", test_soapy_rewrites_every_hop},
        {"fc2580_bandwidth", test_fc2580_hops_rewrite_bandwidth},
        {"safe_tuners", test_safe_tuners_skip_unchanged},
        {"unknown_tuner", test_unknown_tuner_rewrites_every_hop},
        {"retune_above_2147", test_retune_above_2147_mhz},
        {"start_above_2147", test_start_above_2147_mhz},
        {"retune_past_uint32", test_retune_past_uint32_refused},
        {"start_past_uint32", test_start_past_uint32_refused},
        {"failed_rate_correction", test_failed_rate_correction_rolls_back},
        {"rate_correction_past_uint32", test_rate_correction_past_uint32_rolls_back},
        {"failed_rate_write_correction", test_failed_rate_write_correction_rolls_back},
        {"failed_rollback", test_failed_rollback_stops_stream},
    };

    /* An argument runs that case alone. */
    for (const auto& c : k_cases) {
        if (argc < 2 || strcmp(argv[1], c.name) == 0) {
            c.run();
        }
    }

    rtl_device_test_set_fd_identity_hook(nullptr);
    rtl_device_test_set_fd_reset_hook(nullptr);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    printf("IO_RTL_CAPTURE_WRITES: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
