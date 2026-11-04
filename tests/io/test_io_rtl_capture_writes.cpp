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
 *
 * Issue #806, the live rate chain through the same read-back path:
 *
 * - a read-back within 100 ppm of the rate written runs the chain planned for that rate (1,535,999 Hz: 5 passes,
 *   48 kHz exactly, not forced), while the capture still follows the rate the dongle runs;
 * - an RTL-SDR read-back out of tolerance fails the start, and refuses a retune with the device put back, the
 *   read-back and the forced flag reported in the refusal;
 * - a retune whose read-back changes the rate while an I/Q capture records seals the capture: it ends with the samples
 *   before, replays, says so once, and leaves the retunes and drops after it out of its sidecar (X9);
 * - a device whose driver snaps the rate (a SoapySDR device, stood in for by the fake dongle planned as one) has its
 *   passes picked again for a far read-back, and its I/Q rate step designed for it and published; a design that fails
 *   after programming refuses the retune with the device put back; a rollback whose own read-back plans the step's
 *   earlier design again swaps it back, a third rate is designed, and a third rate that cannot be designed stops the
 *   stream.
 */

#include <assert.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/stream_notice_hooks.h>
#include <memory>
#include "dsd-neo/core/opts_fwd.h"
#include "rtl_stream_test_support.h"
#include "rtl_usb_identity.h"
#include "test_env.h"
#include "test_support.h"

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

/* Makes the dongle read back then_rate (0: the rate given) from its at_call-th rate write on: the rate a refused
 * retune's rollback puts back reads back as given, or as a third rate. */
struct readback_reset {
    rtl_device_test_usb_dongle* dongle;
    int at_call;
    uint32_t then_rate;
};

/* A stream's device on the fake dongle, its start programmed. */
struct rig {
    std::unique_ptr<input_ring_state> ring = std::make_unique<input_ring_state>();
    /* Heap-held: the device keeps its address in a global while it is installed. */
    std::unique_ptr<rtl_device_test_usb_dongle> dongle = std::make_unique<rtl_device_test_usb_dongle>();
    /* Heap-held as well: the dongle keeps its address as the op hook's context while a readback is armed. */
    std::unique_ptr<readback_reset> readback = std::make_unique<readback_reset>();
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

/* The capture a hop to @p center_hz runs when the dongle delivers @p capture_rate_hz and the rate chain plans
 * @p passes half-band passes to exactly @p rate_out_hz (issue #806: never the capture rate truncated). */
void
expect_chain_at(const rig& r, const rtl_stream_test_capture_result& got, uint32_t center_hz, uint32_t capture_rate_hz,
                int passes, int rate_out_hz, int forced) {
    assert(got.rc == 0);
    assert(got.capture_rate_hz == capture_rate_hz);
    assert(got.downsample_passes == passes);
    assert(got.rate_out_hz == rate_out_hz);
    assert(got.rate_forced == forced);
    const uint32_t shift = got.fs4_rotation ? capture_rate_hz / 4U : 0U;
    assert(got.capture_freq_hz == center_hz + shift);
    assert(r.dongle->tuner.freq == got.capture_freq_hz);
    assert(r.dongle->tuner.rate == capture_rate_hz);
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
 * read back: the Fs/4 centre follows it as the readback after a write sets it. Within 100 ppm of the rate written, the
 * rate chain is the one planned for that rate (issue #806 A-B3): 1,535,999 Hz runs 5 passes to exactly 48 kHz, not
 * forced, where it ran 47,999 Hz, forced, which put the FSK output resampler on a 48000/47999 ratio. */
void
test_quantized_rate_not_rewritten(void) {
    rig r;
    rig_open(r, kTunerR820T, 1535999U);
    assert(r.start.offset_tuning == 0 && r.start.fs4_rotation == 1);
    expect_chain_at(r, r.start, kCenterHz, 1535999U, 5, kRateIn, 0);
    assert(r.start.nominal_hz == 1536000U);
    assert(r.start.step_enabled == 0);
    assert(r.start.published_rate_out_hz == kRateIn && r.start.published_forced == 0);
    const writes mark = writes_now(r);
    for (uint32_t center : kHopsHz) {
        expect_chain_at(r, hop(center), center, 1535999U, 5, kRateIn, 0);
    }
    const writes w = writes_since(r, mark);
    assert(w.freq == 3);
    assert(w.rate == 0);
    assert(w.bandwidth == 0);
    rig_close(r);
}

/* A changed rate is written once and read back, with the bandwidth for it; the hops after it write it no more. The
 * dongle reads back 960,050 Hz for 960 kS/s, within 100 ppm: the capture runs 960,050 Hz, the chain exactly 30 kHz. */
void
test_changed_rate_written_once_and_read_back(void) {
    rig r;
    rig_open(r, kTunerR820T);
    const uint32_t start_rate = r.start.capture_rate_hz;
    /* The device quantizes the next rate it is given: what the stream runs is the readback. */
    r.dongle->tuner.actual_rate = 960050U;
    writes mark = writes_now(r);
    const rtl_stream_test_capture_result changed = hop(kHopsHz[0], 30000);
    writes w = writes_since(r, mark);
    assert(w.rate == 1);
    assert(w.bandwidth == 1);
    assert(gain_writes(w) == 0);
    assert(start_rate != 960050U);
    expect_chain_at(r, changed, kHopsHz[0], 960050U, 5, 30000, 0);
    assert(changed.published_rate_out_hz == 30000);

    mark = writes_now(r);
    expect_chain_at(r, hop(kHopsHz[1], 30000), kHopsHz[1], 960050U, 5, 30000, 0);
    expect_chain_at(r, hop(kHopsHz[2]), kHopsHz[2], 960050U, 5, 30000, 0);
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

void
readback_reset_on_op(void* ctx, int op) {
    readback_reset* reset = static_cast<readback_reset*>(ctx);
    /* Called before the op is counted. */
    if (op == RTL_TEST_DONGLE_RATE && reset->dongle->calls[RTL_TEST_DONGLE_RATE] + 1 == reset->at_call) {
        reset->dongle->tuner.actual_rate = reset->then_rate;
    }
}

/* A rig whose next retune's rate write reads back @p actual_rate and whose rollback's rate write reads back
 * @p rollback_rate (0: as given). */
void
arm_readback(rig& r, uint32_t actual_rate, uint32_t rollback_rate = 0U) {
    readback_reset* reset = r.readback.get();
    r.dongle->tuner.actual_rate = actual_rate;
    reset->dongle = r.dongle.get();
    reset->then_rate = rollback_rate;
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

/* The device reads back 960050 Hz for 960 kS/s (within 100 ppm, so the rate chain takes it) and refuses the frequency
 * correction for it: the retune is refused and rolled back onto the capture the stream ran. */
void
test_failed_rate_correction_rolls_back(void) {
    rig r;
    rig_open(r, kTunerR820T);
    arm_readback(r, 960050U);
    const uint32_t corrected_hz = kHopsHz[0] + 960050U / 4U;
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
    arm_readback(r, 960004U);
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

/* The rate write for 960 kS/s is refused and the device reports 960,050 Hz (within 100 ppm of the rate written, so the
 * rate chain takes it), whose capture correction it refuses too: the retune is refused and rolled back onto the
 * capture the stream ran, as after a rate write that took. */
void
test_failed_rate_write_correction_rolls_back(void) {
    rig r;
    rig_open(r, kTunerR820T);
    /* The device moved under the stream: the record no longer holds, and it reports this until a write takes. */
    r.dongle->tuner.rate = 960050U;
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 1;
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value = (int)kCapture960kHz;
    const uint32_t corrected_hz = kHopsHz[0] + 960050U / 4U;
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 1;
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value = (int)corrected_hz;
    const int from = r.dongle->tuner.freq_log_count;
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], kRateIn960k);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 0;
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 0;
    expect_rolled_back(r, got);
    assert(got.refused_status == DSD_RATE_PLAN_OK);
    assert(r.dongle->tuner.freq_log_count == from + 2);
    assert(freq_taken(r, from) == kHopsHz[0] + kCapture960kHz / 4U);
    assert(freq_taken(r, from + 1) == r.start.capture_freq_hz);
    rig_close(r);
}

/* A refused rate write whose device reports a rate within 100 ppm of the one written (issue #806): the chain is the one
 * planned for the rate written, not forced, with the capture on the rate the device runs. 1,535,968 Hz for 1.536 MS/s
 * used to run 4 passes to a forced 95,998 Hz (a 24000/47999 FSK resampler), 960,050 Hz a forced 30,001 Hz. */
void
test_failed_rate_write_in_tolerance_runs(void) {
    const struct {
        int rate_in;
        uint32_t written;
        uint32_t reported;
    } cases[] = {{kRateIn, 1536000U, 1535968U}, {kRateIn960k, kCapture960kHz, 960050U}};

    for (const auto& c : cases) {
        rig r;
        rig_open(r, kTunerR820T);
        r.dongle->tuner.rate = c.reported;
        r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 1;
        r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value = (int)c.written;
        const rtl_stream_test_capture_result got = hop(kHopsHz[0], c.rate_in);
        r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 0;
        assert(got.rc != 0); /* the write's failure */
        assert(got.capture_rate_hz == c.reported);
        assert(got.downsample_passes == 5 && got.rate_out_hz == c.rate_in && got.rate_forced == 0);
        assert(got.nominal_hz == c.written && got.step_enabled == 0);
        assert(got.capture_freq_hz == kHopsHz[0] + c.reported / 4U && r.dongle->tuner.freq == got.capture_freq_hz);
        rig_close(r);
    }
}

/* A refused rate write for 960 kS/s whose device still runs its 1.536 MS/s (60 % off the rate written): an RTL-SDR's
 * chain cannot run it, so the retune is refused and rolled back like a read-back out of tolerance, the reported rate and
 * the forced flag in the refusal (issue #806). It used to run a forced 48 kHz chain at a 30 kHz DSP bandwidth. */
void
test_failed_rate_write_out_of_tolerance_refused(void) {
    rig r;
    rig_open(r, kTunerR820T);
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 1;
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value = (int)kCapture960kHz;
    const int from = r.dongle->tuner.freq_log_count;
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], kRateIn960k);
    r.dongle->faults[RTL_TEST_DONGLE_RATE].fail_value_set = 0;
    expect_rolled_back(r, got);
    assert(got.refused_status == DSD_RATE_PLAN_ERR_READBACK);
    assert(got.refused_readback_hz == r.start.capture_rate_hz && got.refused_forced == 1);
    /* The retune's capture, then the one the stream ran: no correction for a rate the chain refused. */
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
    arm_readback(r, 960050U);
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

/* ---- Issue #806: the live rate chain ---- */

/* An RTL-SDR that reads back 1,536,200 Hz for 1.536 MS/s (130 ppm) at the start: its chain would run a fractional
 * 48,006.25 Hz it cannot step, so the start fails, the read-back and the forced flag reported. */
void
test_start_readback_out_of_tolerance_refused(void) {
    rig r;
    rig_begin(r, kTunerR820T, kCenterHz, 1536200U);
    assert(r.start.rc != 0);
    assert(r.start.refused_status == DSD_RATE_PLAN_ERR_READBACK);
    assert(r.start.refused_readback_hz == 1536200U);
    assert(r.start.refused_forced == 1);
    rig_close(r);
}

/* The dongle reads back 960,123 Hz for 960 kS/s (128 ppm) on a retune: refused, the device put back on the capture the
 * stream ran, the read-back and the forced flag reported. Before issue #806 it ran a forced 30,003 Hz chain. */
void
test_retune_readback_out_of_tolerance_refused(void) {
    rig r;
    rig_open(r, kTunerR820T);
    arm_readback(r, 960123U);
    const writes mark = writes_now(r);
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], kRateIn960k);
    disarm_readback(r);
    expect_rolled_back(r, got);
    /* The retune's rate and the rollback's; no capture correction for a read-back the chain refused. */
    const writes w = writes_since(r, mark);
    assert(w.rate == 2 && w.freq == 2);
    assert(got.refused_status == DSD_RATE_PLAN_ERR_READBACK);
    assert(got.refused_readback_hz == 960123U && got.refused_forced == 1);
    assert(got.nominal_hz == r.start.nominal_hz && got.step_retained == 0);
    rig_close(r);
}

/* A rig on a fake dongle planned as a SoapySDR device whose driver snaps the rate, reading back @p actual_rate for any
 * rate it is given (0: the rate given). */
void
rig_open_snapping(rig& r, uint32_t actual_rate) {
    rtl_stream_test_set_rate_plan_backend(DSD_RATE_PLAN_BACKEND_SOAPY);
    rig_open(r, kTunerR820T, actual_rate);
}

void
rig_close_snapping(rig& r) {
    rig_close(r);
    rtl_stream_test_set_rate_plan_backend(-1);
    (void)rtl_stream_test_fail_step_designs(0, 0);
}

/* A-B2: a read-back far from the rate written picks the passes again for the rate the device runs, so the chain never
 * runs below the DSP bandwidth (1 MS/s used to run 31,250 Hz on the request's 5 passes), and the step takes the
 * half-band output to exactly 48 kHz; hops on that rate keep it, writing no rate. */
void
test_far_readback_repicks_passes(void) {
    const struct {
        uint32_t readback;
        int passes;
        int L;
        int M;
    } cases[] = {{1000000U, 4, 96, 125}, {1200000U, 4, 16, 25}, {2000000U, 5, 96, 125}, {2500000U, 5, 384, 625}};

    for (const auto& c : cases) {
        rig r;
        rig_open_snapping(r, c.readback);
        expect_chain_at(r, r.start, kCenterHz, c.readback, c.passes, kRateIn, 1);
        assert(r.start.nominal_hz == c.readback);
        assert(r.start.step_enabled == 1 && r.start.step_l == c.L && r.start.step_m == c.M);
        assert(r.start.step_retained == 0);
        assert(r.start.published_rate_out_hz == kRateIn && r.start.published_forced == 1);
        const writes mark = writes_now(r);
        expect_chain_at(r, hop(kHopsHz[0]), kHopsHz[0], c.readback, c.passes, kRateIn, 1);
        assert(writes_since(r, mark).rate == 0);
        assert(rtl_stream_test_fail_step_designs(0, 0) == 1);
        rig_close_snapping(r);
    }
}

/* A retune whose read-back moves the plan designs the step for it and publishes the chain within the retune, before
 * any block runs; back on the rate asked for, the step goes off. */
void
test_retune_readback_changes_plan(void) {
    rig r;
    rig_open_snapping(r, 0U);
    expect_chain_at(r, r.start, kCenterHz, 1536000U, 5, kRateIn, 0);
    assert(r.start.step_enabled == 0 && r.start.published_forced == 0);

    const struct {
        uint32_t readback;
        int L;
        int M;
    } moves[] = {{2000000U, 96, 125}, {2500000U, 384, 625}};

    for (const auto& m : moves) {
        /* The driver moved the rate: the next hop writes it again and reads this back. */
        r.dongle->tuner.actual_rate = m.readback;
        r.dongle->tuner.rate = m.readback;
        const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], 0);
        assert(got.completion == RTL_STREAM_TUNE_OK);
        expect_chain_at(r, got, kHopsHz[0], m.readback, 5, kRateIn, 1);
        assert(got.step_enabled == 1 && got.step_l == m.L && got.step_m == m.M && got.step_retained == 0);
        assert(got.published_rate_out_hz == kRateIn && got.published_forced == 1);
    }
    r.dongle->tuner.actual_rate = 0U;
    r.dongle->tuner.rate = 1536000U;
    const rtl_stream_test_capture_result back = manual_retune(kHopsHz[1], 0);
    assert(back.completion == RTL_STREAM_TUNE_OK);
    expect_chain_at(r, back, kHopsHz[1], 1536000U, 5, kRateIn, 0);
    assert(back.step_enabled == 0 && back.step_retained == 0 && back.published_forced == 0);
    assert(rtl_stream_test_fail_step_designs(0, 0) == 2);
    rig_close_snapping(r);
}

/* A step design that fails once the device is programmed: the start fails; a retune is refused, the device goes back
 * on the capture it had, and the step keeps running the design it ran there. */
void
test_step_design_failure_rolls_back(void) {
    {
        rig r;
        rtl_stream_test_set_rate_plan_backend(DSD_RATE_PLAN_BACKEND_SOAPY);
        (void)rtl_stream_test_fail_step_designs(0, 1);
        rig_begin(r, kTunerR820T, kCenterHz, 2000000U);
        assert(r.start.rc != 0 && r.start.step_enabled == 0);
        rig_close_snapping(r);
    }
    rig r;
    rig_open_snapping(r, 2000000U);
    assert(r.start.step_enabled == 1 && r.start.step_l == 96 && r.start.step_m == 125);
    r.dongle->tuner.rate = 2500000U;
    arm_readback(r, 2500000U, 2000000U);
    (void)rtl_stream_test_fail_step_designs(0, 1);
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], 0);
    disarm_readback(r);
    expect_rolled_back(r, got);
    assert(got.step_enabled == 1 && got.step_l == 96 && got.step_m == 125 && got.step_retained == 0);
    assert(got.published_rate_out_hz == kRateIn && got.published_forced == 1);
    /* The failed design, and none for the rollback, whose read-back is the plan the step runs. */
    assert(rtl_stream_test_fail_step_designs(0, 0) == 1);
    rig_close_snapping(r);
}

/* A retune whose step design ran, then refused (the device does not take the capture correction for its read-back):
 * the rollback programs the device back and reads back @p rollback_rate, which the step then runs. */
rtl_stream_test_capture_result
refused_after_design(rig& r, uint32_t rollback_rate) {
    r.dongle->tuner.rate = 2500000U;
    arm_readback(r, 2500000U, rollback_rate);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 1;
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value = (int)(kHopsHz[0] + 2500000U / 4U);
    const rtl_stream_test_capture_result got = manual_retune(kHopsHz[0], 0);
    r.dongle->faults[RTL_TEST_DONGLE_FREQ].fail_value_set = 0;
    disarm_readback(r);
    return got;
}

/* The rollback's own read-back plans the design the step ran before the retune: it goes back in, with nothing designed
 * (any design after the retune's would fail). */
void
test_rollback_swaps_step_back(void) {
    rig r;
    rig_open_snapping(r, 2000000U);
    (void)rtl_stream_test_fail_step_designs(1, 1);
    const rtl_stream_test_capture_result got = refused_after_design(r, 2000000U);
    expect_rolled_back(r, got);
    assert(got.step_enabled == 1 && got.step_l == 96 && got.step_m == 125 && got.step_retained == 0);
    assert(rtl_stream_test_fail_step_designs(0, 0) == 1);
    rig_close_snapping(r);
}

/* The rollback's read-back is a third rate (2.4 MS/s): its plan is designed and runs, and the stream goes on. */
void
test_rollback_designs_third_rate(void) {
    rig r;
    rig_open_snapping(r, 2000000U);
    (void)rtl_stream_test_fail_step_designs(0, 0);
    const rtl_stream_test_capture_result got = refused_after_design(r, 2400000U);
    assert(got.rc != 0 && got.completion == RTL_STREAM_TUNE_FAILED);
    assert(got.applied_hz == kCenterHz);
    assert(got.capture_rate_hz == 2400000U && got.downsample_passes == 5 && got.rate_out_hz == kRateIn);
    assert(got.rate_forced == 1);
    assert(got.capture_freq_hz == kCenterHz + 2400000U / 4U && r.dongle->tuner.freq == got.capture_freq_hz);
    assert(got.step_enabled == 1 && got.step_l == 16 && got.step_m == 25 && got.step_retained == 0);
    assert(got.published_rate_out_hz == kRateIn);
    assert(dsd_exitflag_load() == 0);
    assert(rtl_stream_test_fail_step_designs(0, 0) == 2);
    rig_close_snapping(r);
}

/* A third rate the step cannot be designed for: no design matches the rate the device runs, so the step is off and the
 * stream stops, reported as a device failure. */
void
test_rollback_third_rate_design_failure_stops(void) {
    rig r;
    rig_open_snapping(r, 2000000U);
    (void)rtl_stream_test_fail_step_designs(1, 1);
    dsd_input_failure_clear();
    const rtl_stream_test_capture_result got = refused_after_design(r, 2400000U);
    assert(got.rc != 0 && got.completion == RTL_STREAM_TUNE_FAILED);
    assert(got.step_enabled == 0);
    assert(dsd_exitflag_load() != 0);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    dsd_exitflag_store(0);
    dsd_input_failure_clear();
    rig_close_snapping(r);
}

/* The stream notices posted (dsd_runtime_stream_notice_post()), by kind. */
int g_notices[4];

void
count_notice(int kind) {
    if (kind >= 0 && kind < 4) {
        g_notices[kind]++;
    }
}

/* Issue #806 X9: a retune whose read-back changes the device rate while an I/Q capture records. The capture used to
 * refuse the retune's event and go on taking the new rate's samples under a sidecar naming the old rate. Now the event
 * seals it: the stream says so once (a notice its decoder shows; the log), the file ends with the samples before it and
 * still replays, and the retunes, samples and input-ring drops after it are not in it, however the stream's totals
 * grow until its shutdown finalizes the capture. */
void
test_rate_change_seals_capture(void) {
    char dir[DSD_TEST_PATH_MAX];
    char data_path[DSD_TEST_PATH_MAX];
    char meta_path[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_capture_seal") != nullptr);
    assert(dsd_test_path_join(data_path, sizeof data_path, dir, "seal.iq") == 0);
    assert(dsd_test_path_join(meta_path, sizeof meta_path, dir, "seal.iq.json") == 0);

    rig r;
    rig_open_snapping(r, 0U);
    expect_chain_at(r, r.start, kCenterHz, 1536000U, 5, kRateIn, 0);
    r.opts->rtlsdr_center_freq = kCenterHz;
    r.opts->iq_capture_requested = 1;
    r.opts->iq_capture_format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(r.opts->iq_capture_path, sizeof r.opts->iq_capture_path, "%s", data_path);
    dsd_iq_capture_writer* writer = rtl_stream_test_capture_session_open_capture(r.opts);
    assert(writer != nullptr);
    DSD_MEMSET(g_notices, 0, sizeof g_notices);
    dsd_runtime_stream_notice_set(count_notice);
    const int warnings_before = rtl_stream_test_capture_stopped_warnings(nullptr, 0U);

    static uint8_t samples[1024];
    for (size_t i = 0; i < sizeof samples; i++) {
        samples[i] = (uint8_t)(i * 7U);
    }
    assert(dsd_iq_capture_submit(writer, samples, 512U) == DSD_IQ_OK);
    /* A retune at the capture's rate is recorded, its RETUNE and RESET. */
    assert(manual_retune(kHopsHz[0], 0).completion == RTL_STREAM_TUNE_OK);
    assert(dsd_iq_capture_submit(writer, samples + 512U, 256U) == DSD_IQ_OK);
    r.ring->producer_drops.store(7U);

    /* The driver moves the rate: the retune lands on 2 MS/s, and its RETUNE seals the capture. */
    r.dongle->tuner.actual_rate = 2000000U;
    r.dongle->tuner.rate = 2000000U;
    const rtl_stream_test_capture_result moved = manual_retune(kHopsHz[1], 0);
    assert(moved.completion == RTL_STREAM_TUNE_OK);
    expect_chain_at(r, moved, kHopsHz[1], 2000000U, 5, kRateIn, 1);
    assert(g_notices[DSD_STREAM_NOTICE_CAPTURE_STOPPED] == 1);
    assert(dsd_iq_capture_submit(writer, samples, 128U) == DSD_IQ_ERR_SEALED);

    /* What follows is the stream's, not the capture's: more retunes, more drops; said once. */
    assert(manual_retune(kHopsHz[2], 0).completion == RTL_STREAM_TUNE_OK);
    assert(manual_retune(kHopsHz[0], 0).completion == RTL_STREAM_TUNE_OK);
    r.ring->producer_drops.store(1007U);
    assert(g_notices[DSD_STREAM_NOTICE_CAPTURE_STOPPED] == 1 && g_notices[DSD_STREAM_NOTICE_USB_RECOVERED] == 0);
    char warning[192] = {0};
    assert(rtl_stream_test_capture_stopped_warnings(warning, sizeof warning) == warnings_before + 1);
    assert(strcmp(warning, "I/Q capture stopped: the device rate changed from 1536000 to 2000000 Hz on a retune; the "
                           "file ends at the last sample at 1536000 Hz.")
           == 0);
    rtl_stream_test_capture_session_close_capture(r.ring->producer_drops.load());
    dsd_runtime_stream_notice_set(nullptr);

    dsd_iq_replay_config meta{};
    char err[256] = {0};
    assert(dsd_iq_replay_read_metadata(meta_path, &meta, err, sizeof err) == DSD_IQ_OK);
    assert(meta.sample_rate_hz == 1536000U && meta.base_decimation == 32U);
    /* The live rate chain at the seal (issue #806): the RTL-SDR's request, not forced, 48 kHz with no step. */
    assert(meta.rate_chain.has_nominal && meta.rate_chain.nominal_rate_hz == 1536000U);
    assert(meta.rate_chain.has_forced && meta.rate_chain.device_forced == 0);
    assert(meta.rate_chain.has_step && meta.rate_chain.dsp_rate_hz == 48000U && meta.rate_chain.step_l == 0U
           && meta.rate_chain.step_m == 0U);
    assert(meta.data_bytes == 768U);
    assert(meta.event_count == 2U && meta.events[0].kind == DSD_IQ_EVENT_RETUNE);
    assert(meta.events[1].kind == DSD_IQ_EVENT_RESET && meta.events[1].byte_offset == 512U);
    assert(meta.capture_retune_count == 1U && meta.contains_retunes == 1);
    assert(meta.input_ring_drops == 7U);
    dsd_iq_replay_config_clear(&meta);
    /* The whole file replays: every byte the sidecar names is there to read. */
    dsd_iq_replay_source* src = nullptr;
    assert(dsd_iq_replay_open(meta_path, &meta, &src, err, sizeof err) == DSD_IQ_OK && src != nullptr);
    uint8_t back[1024];
    size_t total = 0U;
    for (;;) {
        size_t got = 0U;
        assert(dsd_iq_replay_read(src, back + total, sizeof back - total, &got) == DSD_IQ_OK);
        if (got == 0U) {
            break;
        }
        total += got;
    }
    assert(total == 768U && memcmp(back, samples, 768U) == 0);
    dsd_iq_replay_close(src);
    dsd_iq_replay_config_clear(&meta);
    char text[4096] = {0};
    FILE* fp = dsd_fopen_private(meta_path, "rb");
    assert(fp != nullptr);
    const size_t n = fread(text, 1, sizeof text - 1U, fp);
    (void)fclose(fp);
    assert(n > 0U && strstr(text, "\"capture_end_reason\": \"rate_changed\"") != nullptr);

    rig_close_snapping(r);
    static const char* const kFiles[] = {"seal.iq", "seal.iq.json", nullptr};
    assert(dsd_test_remove_temp_dir(dir, kFiles) == 0);
}

} // namespace

int
main(int argc, char** argv) {
    (void)dsd_test_unsetenv("DSD_NEO_TUNER_BW_HZ");
    (void)dsd_test_unsetenv("DSD_NEO_RTL_OFFSET_TUNING");
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
        {"failed_rate_write_in_tolerance", test_failed_rate_write_in_tolerance_runs},
        {"failed_rate_write_refused", test_failed_rate_write_out_of_tolerance_refused},
        {"failed_rollback", test_failed_rollback_stops_stream},
        {"start_readback_refused", test_start_readback_out_of_tolerance_refused},
        {"retune_readback_refused", test_retune_readback_out_of_tolerance_refused},
        {"far_readback", test_far_readback_repicks_passes},
        {"readback_changes_plan", test_retune_readback_changes_plan},
        {"step_design_failure", test_step_design_failure_rolls_back},
        {"rollback_swaps_back", test_rollback_swaps_step_back},
        {"rollback_third_rate", test_rollback_designs_third_rate},
        {"rollback_third_rate_fails", test_rollback_third_rate_design_failure_stops},
        {"rate_change_seals_capture", test_rate_change_seals_capture},
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
