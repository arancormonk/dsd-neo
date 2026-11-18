// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The rate and tuner filter a SoapySDR device runs, through the device's own rate and bandwidth calls on a stand-in
 * driver that reports what a real driver lists (SoapyHackRF 0.3.6, SoapySDRPlay3 0.5.3, SoapyPlutoSDR and LimeSuite's
 * SoapyLMS7, read from their sources):
 *
 * - a HackRF runs the 10 MS/s its profile prefers, whatever RTL-shaped rate the stream asks for (it used to get the
 *   listed rate nearest the request: 2 MS/s, or 1 MS/s, below the 2 MS/s its makers list as its lowest);
 * - SoapySDR captures sit a quarter of the sample rate off the channel (the Fs/4 shift), so a profile's default filter
 *   opens to at least rate / 2 + 72 kHz, the smallest the driver supports at or above that (HackRF 5.5 MHz at 10 MS/s,
 *   SDRplay 1.536 MHz at 2 MS/s and 600 kHz at 1 MS/s, ADALM-Pluto 2 MHz and 1 MHz; each used to be 1.75 MHz or
 *   200 kHz); a LimeSDR's filter, already wider, is unchanged;
 * - an explicit bandwidth the driver snaps below that floor is applied as asked and warned about once;
 * - a driver-automatic bandwidth, a device with no bandwidth control, and soapy_profile = "generic" stay as they were;
 * - a stream open on a HackRF runs 10 MS/s with the capture 2.5 MHz above the centre and an input ring scaled to the
 *   rate, as a native Airspy's is; a 2 MS/s device keeps the default ring and a 500 kHz shift;
 * - a retune writes the filter again after the rate, and one whose read-back moved the rate writes the filter for it.
 *
 * Skips in a build without SoapySDR.
 */

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include "dsd-neo/core/opts_fwd.h"
#include "rtl_finite.h"
#include "rtl_stream_test_support.h"
#include "test_env.h"
#include "test_support.h"

struct rtl_device;

namespace {

int g_failures = 0;

/* Within half a hertz, and finite first: a NaN compares false both ways, and fast-math may fold a plain check away, so
 * the finiteness test is the out-of-line IEEE one. */
void
expect_eq(const char* label, double got, double want) {
    if (!dsd::io::radio::rtl_is_finite(got) || got < want - 0.5 || got > want + 0.5) {
        DSD_FPRINTF(stderr, "FAIL %s: got %.0f, want %.0f\n", label, got, want);
        g_failures++;
    }
}

void
expect_true(const char* label, bool value) {
    if (!value) {
        DSD_FPRINTF(stderr, "FAIL %s\n", label);
        g_failures++;
    }
}

constexpr uint32_t kCenterHz = 851012500U;

/* SoapyHackRF 0.3.6: listSampleRates() 1..20 MHz in 1 MHz steps, getSampleRateRange() 2..20 MHz, and the MAX2837's
 * filters as listBandwidths(). */
const double k_hackrf_rates[] = {1e6,  2e6,  3e6,  4e6,  5e6,  6e6,  7e6,  8e6,  9e6,  10e6,
                                 11e6, 12e6, 13e6, 14e6, 15e6, 16e6, 17e6, 18e6, 19e6, 20e6};
const rtl_device_test_soapy_range k_hackrf_rate_range[] = {{2e6, 20e6, 0.0}};
const double k_hackrf_bandwidths[] = {1750000.0,  2500000.0,  3500000.0,  5000000.0,  5500000.0,  6000000.0,
                                      7000000.0,  8000000.0,  9000000.0,  10000000.0, 12000000.0, 14000000.0,
                                      15000000.0, 20000000.0, 24000000.0, 28000000.0};

/* SoapySDRPlay3 0.5.3 (single tuner). */
const double k_sdrplay_rates[] = {62500.0,   96000.0,   125000.0,  192000.0,  250000.0,  384000.0,  500000.0,
                                  768000.0,  1000000.0, 2000000.0, 2048000.0, 3000000.0, 4000000.0, 5000000.0,
                                  6000000.0, 7000000.0, 8000000.0, 9000000.0, 10000000.0};
const double k_sdrplay_bandwidths[] = {200000.0,  300000.0,  600000.0,  1536000.0,
                                       5000000.0, 6000000.0, 7000000.0, 8000000.0};

/* SoapyPlutoSDR. */
const double k_pluto_rates[] = {65105.0, 1e6, 2e6, 3e6, 4e6, 5e6, 6e6, 7e6, 8e6, 9e6, 10e6};
/* SoapyPlutoSDR also advertises a continuous range (25 MHz / 96 to 61.44 MHz without libad9361). */
const rtl_device_test_soapy_range k_pluto_rate_range[] = {{25e6 / 96.0, 61440000.0, 0.0}};
const double k_pluto_bandwidths[] = {0.2e6, 1e6, 2e6, 3e6, 4e6, 5e6, 6e6, 7e6, 8e6, 9e6, 10e6};

/* SoapyLMS7: rates every 0.5 MHz, an RX low-pass range of 1.4001..130 MHz. */
double g_lime_rates[122];
const rtl_device_test_soapy_range k_lime_bandwidth_range[] = {{1400100.0, 130000000.0, 0.0}};

/* SoapyAirspyHF: the firmware's rates, no bandwidth control. */
const double k_airspyhf_rates[] = {912000.0, 768000.0, 456000.0, 384000.0, 256000.0, 192000.0};

/* A stand-in driver keyed and listing as a HackRF; the profile's default filter applies unless @p bandwidth_request_hz
 * (soapy_bandwidth_hz) says otherwise. */
rtl_device_test_soapy_gain_fake
hackrf_fake(int bandwidth_request_hz = -1) {
    rtl_device_test_soapy_gain_fake fake{};
    fake.driver_key = "HackRF";
    fake.hardware_key = "HackRF One";
    fake.bandwidth_request_hz = bandwidth_request_hz;
    fake.listed_rates = k_hackrf_rates;
    fake.listed_rate_count = sizeof k_hackrf_rates / sizeof k_hackrf_rates[0];
    fake.rate_ranges = k_hackrf_rate_range;
    fake.rate_range_count = 1U;
    fake.bandwidths = k_hackrf_bandwidths;
    fake.bandwidth_count = sizeof k_hackrf_bandwidths / sizeof k_hackrf_bandwidths[0];
    return fake;
}

rtl_device_test_soapy_gain_fake
sdrplay_fake(void) {
    rtl_device_test_soapy_gain_fake fake{};
    fake.driver_key = "SDRplay";
    fake.bandwidth_request_hz = -1;
    fake.listed_rates = k_sdrplay_rates;
    fake.listed_rate_count = sizeof k_sdrplay_rates / sizeof k_sdrplay_rates[0];
    fake.bandwidths = k_sdrplay_bandwidths;
    fake.bandwidth_count = sizeof k_sdrplay_bandwidths / sizeof k_sdrplay_bandwidths[0];
    return fake;
}

rtl_device_test_soapy_gain_fake
pluto_fake(void) {
    rtl_device_test_soapy_gain_fake fake{};
    fake.driver_key = "PlutoSDR";
    fake.hardware_key = "ADALM-PLUTO";
    fake.bandwidth_request_hz = -1;
    fake.listed_rates = k_pluto_rates;
    fake.listed_rate_count = sizeof k_pluto_rates / sizeof k_pluto_rates[0];
    fake.rate_ranges = k_pluto_rate_range;
    fake.rate_range_count = 1U;
    fake.bandwidths = k_pluto_bandwidths;
    fake.bandwidth_count = sizeof k_pluto_bandwidths / sizeof k_pluto_bandwidths[0];
    return fake;
}

rtl_device_test_soapy_gain_fake
lime_fake(void) {
    for (size_t i = 0; i < sizeof g_lime_rates / sizeof g_lime_rates[0]; i++) {
        g_lime_rates[i] = 500000.0 * (double)(i + 1U);
    }
    rtl_device_test_soapy_gain_fake fake{};
    fake.driver_key = "lime";
    fake.bandwidth_request_hz = -1;
    fake.listed_rates = g_lime_rates;
    fake.listed_rate_count = sizeof g_lime_rates / sizeof g_lime_rates[0];
    fake.bandwidth_ranges = k_lime_bandwidth_range;
    fake.bandwidth_range_count = 1U;
    return fake;
}

rtl_device_test_soapy_gain_fake
airspyhf_fake(void) {
    rtl_device_test_soapy_gain_fake fake{};
    fake.driver_key = "AirspyHF";
    fake.hardware_key = "AirspyHF";
    fake.bandwidth_request_hz = -1;
    fake.listed_rates = k_airspyhf_rates;
    fake.listed_rate_count = sizeof k_airspyhf_rates / sizeof k_airspyhf_rates[0];
    return fake;
}

/* The rate the stream plans for @p request_hz and the rate it then programs (it writes the planned rate, as
 * optimal_settings() and the programming after it do), and the bandwidth the device then writes for the
 * driver-automatic tuner bandwidth the stream asks for. */
struct programmed {
    double nearest_hz;
    double rate_hz;
    double bandwidth_hz;
    int bandwidth_writes;
};

programmed
program(rtl_device_test_soapy_gain_fake* fake, uint32_t request_hz) {
    programmed out{};
    rtl_device* dev = rtl_device_test_soapy_gain_device(fake, 0);
    if (!dev) {
        return out;
    }
    uint32_t nearest = 0U;
    expect_true("nearest supported rate answers", rtl_device_nearest_supported_rate(dev, request_hz, &nearest) == 0);
    out.nearest_hz = nearest;
    expect_true("rate write takes", rtl_device_set_sample_rate(dev, nearest) == 0);
    out.rate_hz = fake->rate_hz;
    (void)rtl_device_set_tuner_bandwidth(dev, 0U);
    out.bandwidth_hz = fake->last_bandwidth_hz;
    out.bandwidth_writes = fake->bandwidth_writes;
    rtl_device_test_soapy_gain_device_free(dev);
    return out;
}

/* B3: whatever the stream asks for, a HackRF runs at the 10 MS/s its profile prefers. */
void
test_hackrf_runs_preferred_rate(void) {
    const uint32_t requests[] = {1536000U, 1152000U, 1024000U};
    for (uint32_t request : requests) {
        rtl_device_test_soapy_gain_fake fake = hackrf_fake();
        const programmed got = program(&fake, request);
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "HackRF nearest rate for a %u Hz request", request);
        expect_eq(label, got.nearest_hz, 10000000.0);
        DSD_SNPRINTF(label, sizeof label, "HackRF rate programmed for a %u Hz request", request);
        expect_eq(label, got.rate_hz, 10000000.0);
    }
}

/* B4: the profile's default filter opens to cover the channel a quarter of the rate off centre. */
void
test_default_filters_cover_offset_channel(void) {
    rtl_device_test_soapy_gain_fake hackrf = hackrf_fake();
    programmed got = program(&hackrf, 1536000U);
    expect_eq("HackRF filter at 10 MS/s", got.bandwidth_hz, 5500000.0);

    rtl_device_test_soapy_gain_fake sdrplay = sdrplay_fake();
    got = program(&sdrplay, 1536000U);
    expect_eq("SDRplay rate for a 1536000 Hz request", got.rate_hz, 2000000.0);
    expect_eq("SDRplay filter at 2 MS/s", got.bandwidth_hz, 1536000.0);
    sdrplay = sdrplay_fake();
    got = program(&sdrplay, 1024000U);
    expect_eq("SDRplay rate for a 1024000 Hz request", got.rate_hz, 1000000.0);
    expect_eq("SDRplay filter at 1 MS/s", got.bandwidth_hz, 600000.0);

    /* A device with a stepped filter range rather than a list (none of the drivers above): at 3 MS/s the floor is
       1.572 MHz, between the 250 kHz steps, so the filter opens to the next step up. */
    static const double only_3m[] = {3e6};
    static const rtl_device_test_soapy_range stepped[] = {{200000.0, 8000000.0, 250000.0}};
    rtl_device_test_soapy_gain_fake stepped_fake = sdrplay_fake();
    stepped_fake.listed_rates = only_3m;
    stepped_fake.listed_rate_count = 1U;
    stepped_fake.bandwidths = nullptr;
    stepped_fake.bandwidth_count = 0U;
    stepped_fake.bandwidth_ranges = stepped;
    stepped_fake.bandwidth_range_count = 1U;
    got = program(&stepped_fake, 1536000U);
    expect_eq("stepped-range device rate", got.rate_hz, 3000000.0);
    expect_eq("stepped-range filter at 3 MS/s", got.bandwidth_hz, 1700000.0);

    /* Its continuous range holds the request, so the request is the rate. */
    rtl_device_test_soapy_gain_fake pluto = pluto_fake();
    got = program(&pluto, 1536000U);
    expect_eq("ADALM-Pluto rate for a 1536000 Hz request", got.rate_hz, 1536000.0);
    expect_eq("ADALM-Pluto filter at 1.536 MS/s", got.bandwidth_hz, 1000000.0);
    pluto = pluto_fake();
    got = program(&pluto, 1152000U);
    expect_eq("ADALM-Pluto rate for a 1152000 Hz request", got.rate_hz, 1152000.0);
    expect_eq("ADALM-Pluto filter at 1.152 MS/s", got.bandwidth_hz, 1000000.0);
}

/* Guards: a LimeSDR's filter (its narrowest, wider than the floor already), a driver-automatic bandwidth, a device with
 * no bandwidth control, and the generic profile's nearest rate stay as they were. */
void
test_unchanged_filters_and_rates(void) {
    rtl_device_test_soapy_gain_fake lime = lime_fake();
    programmed got = program(&lime, 1536000U);
    expect_eq("LimeSDR rate for a 1536000 Hz request", got.rate_hz, 1500000.0);
    expect_eq("LimeSDR filter at 1.5 MS/s", got.bandwidth_hz, 1400100.0);

    rtl_device_test_soapy_gain_fake automatic = hackrf_fake(0);
    got = program(&automatic, 1536000U);
    expect_true("a driver-automatic bandwidth writes no bandwidth", got.bandwidth_writes == 0);

    rtl_device_test_soapy_gain_fake airspyhf = airspyhf_fake();
    got = program(&airspyhf, 1536000U);
    expect_eq("AirspyHF rate for a 1536000 Hz request", got.rate_hz, 912000.0);
    expect_true("a device with no bandwidth control gets no bandwidth", got.bandwidth_writes == 0);

    rtl_device_test_soapy_gain_fake generic = hackrf_fake();
    generic.requested_profile = "generic";
    got = program(&generic, 1536000U);
    expect_eq("soapy_profile = generic keeps a HackRF's nearest rate", got.rate_hz, 2000000.0);
    expect_true("soapy_profile = generic leaves a HackRF's filter to the driver", got.bandwidth_writes == 0);
}

int
count_text(const char* haystack, const char* needle) {
    int count = 0;
    const size_t len = strlen(needle);
    for (const char* at = strstr(haystack, needle); at != nullptr; at = strstr(at + len, needle)) {
        count++;
    }
    return count;
}

/* B8: an explicit bandwidth the driver snaps below the floor is applied as the driver takes it, and warned about once
 * for that filter and rate however often it is applied. The device lists 10 MS/s alone, so it runs that rate whatever
 * its profile prefers. */
void
test_explicit_narrow_bandwidth_warns(void) {
    static const double only_10m[] = {10e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake(5100000);
    fake.listed_rates = only_10m;
    fake.listed_rate_count = 1U;
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    if (!dev) {
        return;
    }
    dsd_test_capture_stderr cap;
    expect_true("stderr capture begins", dsd_test_capture_stderr_begin(&cap, "soapy_rate") == 0);
    expect_true("rate write takes", rtl_device_set_sample_rate(dev, 1536000U) == 0);
    const int first = rtl_device_set_tuner_bandwidth(dev, 0U);
    const int second = rtl_device_set_tuner_bandwidth(dev, 0U);
    (void)dsd_test_capture_stderr_end(&cap);
    char log[4096];
    expect_true("stderr capture reads", dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0);
    rtl_device_test_soapy_gain_device_free(dev);

    expect_true("the explicit bandwidth is applied", first == 0 && second == 0);
    expect_eq("explicit 5.1 MHz as the driver takes it", fake.last_bandwidth_hz, 5000000.0);
    const int warnings = count_text(log, "narrower than");
    if (warnings != 1) {
        DSD_FPRINTF(stderr, "FAIL explicit narrow bandwidth warnings: got %d, want 1; log:\n%s\n", warnings, log);
        g_failures++;
    }
    expect_true("the warning names the filter, the floor and the rate",
                strstr(log, "5000000 Hz is narrower than 5072000 Hz") != nullptr
                    && strstr(log, "2500000 Hz off centre") != nullptr && strstr(log, "10000000 Hz") != nullptr);
}

dsd_opts*
make_soapy_opts(int bandwidth_request_hz) {
    dsd_opts* opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    if (opts) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
        opts->soapy_bandwidth_hz = bandwidth_request_hz;
    }
    return opts;
}

/* A stream open on @p fake: the capture, the shift and the input ring it leaves. */
rtl_stream_test_capture_result
open_on(rtl_device_test_soapy_gain_fake* fake) {
    rtl_stream_test_capture_result got{};
    got.rc = -100;
    rtl_device* dev = rtl_device_test_soapy_gain_device(fake, 0);
    dsd_opts* opts = make_soapy_opts(fake->bandwidth_request_hz);
    if (dev && opts) {
        expect_true("the open seam runs",
                    rtl_stream_test_open_radio_capture(RTL_STREAM_TEST_SOURCE_SOAPY, dev, opts, kCenterHz, &got) == 0);
    }
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
    return got;
}

/* A stream open: a 2 MS/s device keeps the default ring and a 500 kHz Fs/4 shift; a HackRF runs 10 MS/s, 2.5 MHz above
 * the centre, with an input ring holding the default ring's time at that rate. */
void
test_open_capture_and_ring(void) {
    static const double only_2m[] = {2e6};
    rtl_device_test_soapy_gain_fake two = {};
    two.driver_key = "generic";
    two.listed_rates = only_2m;
    two.listed_rate_count = 1U;
    rtl_stream_test_capture_result got = open_on(&two);
    expect_true("a 2 MS/s open starts", got.rc == 0);
    expect_eq("2 MS/s capture rate", got.capture_rate_hz, 2000000.0);
    expect_true("2 MS/s: the stream shifts by Fs/4 and rotates back", got.fs4_rotation == 1 && got.centered_iq == 0);
    expect_eq("2 MS/s capture frequency", got.capture_freq_hz, (double)kCenterHz + 500000.0);
    expect_eq("2 MS/s input ring", (double)got.input_ring_capacity_floats, 2097152.0);

    rtl_device_test_soapy_gain_fake hackrf = hackrf_fake();
    got = open_on(&hackrf);
    expect_true("a HackRF open starts", got.rc == 0);
    expect_eq("HackRF capture rate", got.capture_rate_hz, 10000000.0);
    expect_true("HackRF: the stream shifts by Fs/4 and rotates back", got.fs4_rotation == 1 && got.centered_iq == 0);
    expect_eq("HackRF capture frequency", got.capture_freq_hz, (double)kCenterHz + 2500000.0);
    expect_eq("HackRF input ring", (double)got.input_ring_capacity_floats, 8388608.0);
    expect_eq("HackRF filter after the open", hackrf.last_bandwidth_hz, 5500000.0);
}

/* The filter follows the rate: SoapySDR rewrites the rate on every retune and the bandwidth after it, so a retune
 * applies the opened filter again, and one whose read-back moved the rate applies the filter for the new rate. */
void
test_filter_follows_rate_on_retune(void) {
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(-1);
    if (!dev || !opts) {
        free(opts);
        rtl_device_test_soapy_gain_device_free(dev);
        return;
    }
    rtl_stream_test_capture_result got{};
    expect_true("session begins", rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, &got) == 0);
    expect_true("the start runs", got.rc == 0);
    expect_eq("filter at the start", fake.last_bandwidth_hz, 5500000.0);

    fake.last_bandwidth_hz = 0.0;
    int writes = fake.bandwidth_writes;
    expect_true("a retune lands",
                rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_HOP, kCenterHz + 25000U, 0, &got) == 0
                    && got.rc == 0);
    expect_true("a retune rewrites the filter", fake.bandwidth_writes > writes);
    expect_eq("filter after a retune", fake.last_bandwidth_hz, 5500000.0);

    /* The driver moves to 8 MS/s: the floor is 4.072 MHz, which 5 MHz keeps. */
    fake.readback_rate_hz = 8000000.0;
    writes = fake.bandwidth_writes;
    expect_true("a retune on a moved rate lands",
                rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_HOP, kCenterHz + 50000U, 0, &got) == 0
                    && got.rc == 0);
    expect_eq("the stream runs the read-back", got.capture_rate_hz, 8000000.0);
    expect_true("the moved rate rewrites the filter", fake.bandwidth_writes > writes);
    expect_eq("filter for the moved rate", fake.last_bandwidth_hz, 5000000.0);
    rtl_stream_test_capture_session_end();
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
}

/* The rate a write asks for is the rate programmed: the profile's preferred rate is chosen when the stream plans
 * (rtl_device_nearest_supported_rate()), and a rollback that asks a HackRF for the 2 MS/s it ran gets 2 MS/s, not the
 * 10 MS/s the profile prefers. An advertised point within 1 Hz of the preference is programmed as advertised. */
void
test_write_programs_asked_rate(void) {
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    if (!dev) {
        return;
    }
    expect_true("rate write takes", rtl_device_set_sample_rate(dev, 2000000U) == 0);
    expect_eq("HackRF asked for 2 MS/s", fake.rate_hz, 2000000.0);
    uint32_t nearest = 0U;
    expect_true("nearest answers", rtl_device_nearest_supported_rate(dev, 1536000U, &nearest) == 0);
    expect_eq("HackRF plans its preferred rate", nearest, 10000000.0);
    rtl_device_test_soapy_gain_device_free(dev);

    static const double near_10m[] = {2e6, 9999999.0};
    rtl_device_test_soapy_gain_fake matched = hackrf_fake();
    matched.listed_rates = near_10m;
    matched.listed_rate_count = 2U;
    /* No range: the HackRF's 2-20 MS/s range holds 10 MS/s exactly, which would win over the near point. */
    matched.rate_ranges = nullptr;
    matched.rate_range_count = 0U;
    const programmed got = program(&matched, 1536000U);
    expect_eq("the advertised point is planned", got.nearest_hz, 9999999.0);
}

/* A retune refused for a rate above the running input ring rolls back to the rate the stream ran, and the stream goes
 * on: a HackRF that opened at 2 MS/s (10 MS/s not offered) then offers 10 MS/s too. */
void
test_rollback_restores_saved_rate(void) {
    static const double only_2m[] = {2e6};
    static const double both[] = {2e6, 10e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = only_2m;
    fake.listed_rate_count = 1U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(-1);
    if (!dev || !opts) {
        free(opts);
        rtl_device_test_soapy_gain_device_free(dev);
        return;
    }
    dsd_exitflag_store(0);
    rtl_stream_test_capture_result got{};
    expect_true("session begins", rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, &got) == 0);
    expect_eq("the start runs 2 MS/s", got.capture_rate_hz, 2000000.0);
    fake.listed_rates = both;
    fake.listed_rate_count = 2U;
    /* 72 kHz asks for another rate than the start's, so the device is asked anew. */
    expect_true("the retune runs", rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_MANUAL_RETUNE,
                                                                        kCenterHz + 25000U, 72000, &got)
                                       == 0);
    expect_true("the retune to 10 MS/s is refused", got.completion == RTL_STREAM_TUNE_FAILED);
    expect_eq("the stream goes back on 2 MS/s", got.capture_rate_hz, 2000000.0);
    expect_eq("the device goes back on 2 MS/s", fake.rate_hz, 2000000.0);
    expect_true("the stream keeps running", dsd_exitflag_load() == 0);
    rtl_stream_test_capture_session_end();
    dsd_exitflag_store(0);
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
}

/* A HackRF listing 2 and 10 MS/s that delivers 2 MS/s at the start, asked for 10, then honours 10 on a hop: the ring
 * sized for 2 MS/s refuses the hop, and the rollback puts the device back on the 2 MS/s it delivered, checked by its
 * read-back, instead of writing the 10 MS/s it was asked for again and stopping the stream. */
void
test_rollback_restores_delivered_rate(void) {
    static const double both[] = {2e6, 10e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = both;
    fake.listed_rate_count = 2U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    fake.readback_rate_hz = 2e6;
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(-1);
    if (!dev || !opts) {
        free(opts);
        rtl_device_test_soapy_gain_device_free(dev);
        return;
    }
    dsd_exitflag_store(0);
    rtl_stream_test_capture_result got{};
    expect_true("session begins", rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, &got) == 0);
    expect_eq("the start runs the 2 MS/s delivered", got.capture_rate_hz, 2000000.0);
    fake.readback_rate_hz = 0.0;
    expect_true("the retune runs", rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_MANUAL_RETUNE,
                                                                        kCenterHz + 25000U, 72000, &got)
                                       == 0);
    expect_true("the retune to 10 MS/s is refused", got.completion == RTL_STREAM_TUNE_FAILED);
    expect_eq("the device goes back on the 2 MS/s it delivered", fake.rate_hz, 2000000.0);
    expect_eq("the stream goes back on 2 MS/s", got.capture_rate_hz, 2000000.0);
    expect_true("the stream keeps running", dsd_exitflag_load() == 0);
    rtl_stream_test_capture_session_end();
    dsd_exitflag_store(0);
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
}

/* A driver that reads back 2.4 GS/s for a 2 MS/s write delivers more than any ring holds: the open is refused, and a
 * hop that reads it back is refused too, never taken for a device that reported no rate. */
void
test_oversized_readback_refused(void) {
    static const double only_2m[] = {2e6};
    rtl_device_test_soapy_gain_fake fake = {};
    fake.driver_key = "generic";
    fake.listed_rates = only_2m;
    fake.listed_rate_count = 1U;
    fake.readback_rate_hz = 2.4e9;
    const rtl_stream_test_capture_result opened = open_on(&fake);
    expect_true("an open that reads back 2.4 GS/s is refused", opened.rc != 0);

    fake.readback_rate_hz = 0.0;
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(-1);
    if (!dev || !opts) {
        free(opts);
        rtl_device_test_soapy_gain_device_free(dev);
        return;
    }
    dsd_exitflag_store(0);
    rtl_stream_test_capture_result got{};
    expect_true("session begins", rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, &got) == 0);
    fake.readback_rate_hz = 2.4e9;
    expect_true("the retune runs", rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_MANUAL_RETUNE,
                                                                        kCenterHz + 25000U, 72000, &got)
                                       == 0);
    expect_true("a hop that reads back 2.4 GS/s is refused", got.completion == RTL_STREAM_TUNE_FAILED);
    expect_eq("the stream stays on 2 MS/s", got.capture_rate_hz, 2000000.0);
    rtl_stream_test_capture_session_end();
    dsd_exitflag_store(0);
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
}

/* A refused hop on @p fake (rtl_device_test_soapy_gain_fake::delivered_by_write sets what each write delivers): the
 * session starts, the 72 kHz hop is refused, and @p out holds where the stream and the device ended up. */
bool
refused_hop(rtl_device_test_soapy_gain_fake* fake, rtl_stream_test_capture_result* out) {
    rtl_device* dev = rtl_device_test_soapy_gain_device(fake, 0);
    dsd_opts* opts = make_soapy_opts(-1);
    if (!dev || !opts) {
        free(opts);
        rtl_device_test_soapy_gain_device_free(dev);
        return false;
    }
    dsd_exitflag_store(0);
    *out = {};
    expect_true("session begins", rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, out) == 0);
    expect_true("the retune runs", rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_MANUAL_RETUNE,
                                                                        kCenterHz + 25000U, 72000, out)
                                       == 0);
    expect_true("the hop is refused", out->completion == RTL_STREAM_TUNE_FAILED);
    rtl_stream_test_capture_session_end();
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
    return true;
}

/* A device that read back 3,072,025 Hz for the 3,072,000 Hz written, refused on a hop: the rollback asks for the
 * 3,072,025 Hz it delivered, which the device snaps to the 3,072,000 Hz it lists and then delivers. The plan is made
 * for the rate written, so the stream runs on. */
void
test_rollback_plans_the_rate_written(void) {
    static const double listed[] = {3072000.0, 9e6};
    static const double delivered[] = {3072025.0, 9e6, 0.0};
    rtl_device_test_soapy_gain_fake fake = {};
    fake.driver_key = "generic";
    fake.listed_rates = listed;
    fake.listed_rate_count = 2U;
    fake.delivered_by_write = delivered;
    fake.delivered_by_write_count = 3U;
    rtl_stream_test_capture_result got{};
    if (!refused_hop(&fake, &got)) {
        return;
    }
    expect_eq("the device is back on 3,072,000 Hz", fake.rate_hz, 3072000.0);
    expect_eq("the stream runs 3,072,000 Hz", got.capture_rate_hz, 3072000.0);
    expect_true("the stream keeps running", dsd_exitflag_load() == 0);
    dsd_exitflag_store(0);
}

/* A HackRF listing 2 and 10 MS/s that delivered 3 MS/s for 10 MS/s: 3 MS/s is not a rate it can be asked for (the
 * write snaps to 2 MS/s), so the rollback asks again for the 10 MS/s that gave the 3 MS/s, checked by its read-back. */
void
test_rollback_falls_back_to_the_request(void) {
    static const double both[] = {2e6, 10e6};
    static const double delivered[] = {3e6, 10e6, 0.0, 3e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = both;
    fake.listed_rate_count = 2U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    fake.delivered_by_write = delivered;
    fake.delivered_by_write_count = 4U;
    rtl_stream_test_capture_result got{};
    if (!refused_hop(&fake, &got)) {
        return;
    }
    expect_eq("the device is asked for 10 MS/s again", fake.rate_hz, 10000000.0);
    expect_eq("the stream goes back on the 3 MS/s it ran", got.capture_rate_hz, 3000000.0);
    expect_true("the stream keeps running", dsd_exitflag_load() == 0);
    dsd_exitflag_store(0);
}

/* Infinity from its bit pattern: the global fast-math option warns on INFINITY. */
double
positive_infinity(void) {
    const uint64_t bits = UINT64_C(0x7FF0000000000000);
    double value = 0.0;
    DSD_MEMCPY(&value, &bits, sizeof value);
    return value;
}

/* expect_eq() fails a value that is not a number: a NaN rate or filter is never "within half a hertz". */
void
test_expect_eq_rejects_nan(void) {
    const int before = g_failures;
    const uint64_t nan_bits = UINT64_C(0x7FF8000000000000);
    double nan_value = 0.0;
    DSD_MEMCPY(&nan_value, &nan_bits, sizeof nan_value);
    expect_eq("a NaN filter (expected to fail)", nan_value, 10000000.0);
    const bool flagged = g_failures == before + 1;
    g_failures = before;
    expect_true("expect_eq() fails NaN", flagged);
}

/* Listed 10,000,000.5 and 9,999,999.5 Hz, equally near the preferred 10 MS/s: the plan takes the one that rounds to
 * 10 MS/s, and the write keeps it, so the read-back is not refused at the 10 MS/s ceiling. */
void
test_tied_points_keep_the_planned_rate(void) {
    static const double tied[] = {10000000.5, 9999999.5};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = tied;
    fake.listed_rate_count = 2U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    const rtl_stream_test_capture_result got = open_on(&fake);
    expect_true("the open starts", got.rc == 0);
    expect_eq("the device was given 9,999,999.5 Hz", fake.rate_hz * 2.0, 19999999.0);
}

/* A centre so near the top of the tuning range that neither the preferred 10 MS/s nor the 1.536 MS/s request fits its
 * quarter-rate offset (2.5 MHz and 384 kHz): the 1.5 MS/s the HackRF lists nearest the request does (375 kHz), so the
 * device is asked for that. */
void
test_fallback_rate_near_the_top(void) {
    static const double listed[] = {1500000.0, 10e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = listed;
    fake.listed_rate_count = 2U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(fake.bandwidth_request_hz);
    rtl_stream_test_capture_result got{};
    got.rc = -100;
    const uint32_t center_hz = UINT32_MAX - 380000U;
    if (dev && opts) {
        expect_true("the open seam runs",
                    rtl_stream_test_open_radio_capture(RTL_STREAM_TEST_SOURCE_SOAPY, dev, opts, center_hz, &got) == 0);
    }
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
    expect_true("the open starts", got.rc == 0);
    expect_eq("the capture runs 1.5 MS/s", got.capture_rate_hz, 1500000.0);
    expect_eq("the capture sits a quarter of it above the centre", got.capture_freq_hz, (double)center_hz + 375000.0);
}

/* The HackRF that delivered 3 MS/s for 10 MS/s, whose rollback write of 3 MS/s snaps to 2 MS/s and already brings the
 * 3 MS/s back: the read-back says so, and the rollback stops there instead of writing 10 MS/s again. */
void
test_rollback_stops_once_restored(void) {
    static const double both[] = {2e6, 10e6};
    static const double delivered[] = {3e6, 10e6, 3e6, 10e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = both;
    fake.listed_rate_count = 2U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    fake.delivered_by_write = delivered;
    fake.delivered_by_write_count = 4U;
    rtl_stream_test_capture_result got{};
    if (!refused_hop(&fake, &got)) {
        return;
    }
    expect_eq("rate writes: start, hop and one rollback", (double)fake.rate_writes, 3.0);
    expect_eq("the stream goes back on 3 MS/s", got.capture_rate_hz, 3000000.0);
    expect_true("the stream keeps running", dsd_exitflag_load() == 0);
    dsd_exitflag_store(0);
}

/* A rollback write the driver refuses outright: the rate the capture was programmed with is tried next. */
void
test_rollback_retries_after_a_refused_write(void) {
    static const double both[] = {2e6, 10e6};
    static const double delivered[] = {3e6, 10e6, 0.0, 3e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = both;
    fake.listed_rate_count = 2U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    fake.delivered_by_write = delivered;
    fake.delivered_by_write_count = 4U;
    fake.rate_write_refused_at = 3;
    rtl_stream_test_capture_result got{};
    if (!refused_hop(&fake, &got)) {
        return;
    }
    expect_eq("the device is asked for 10 MS/s again", fake.rate_hz, 10000000.0);
    expect_eq("the stream goes back on the 3 MS/s it ran", got.capture_rate_hz, 3000000.0);
    expect_true("the stream keeps running", dsd_exitflag_load() == 0);
    dsd_exitflag_store(0);
}

/* Near the top of the tuning range a HackRF is asked for the request, 1.536 MS/s, which it writes as the 3,072,000 Hz
 * it lists and reads back 3,072,025 Hz: the read-back is judged against the rate written, which it is within the
 * planner's tolerance of, at the open and on a retune there. */
void
test_rate_written_judges_the_readback(void) {
    static const double listed[] = {3072000.0, 10e6};
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.listed_rates = listed;
    fake.listed_rate_count = 2U;
    fake.rate_ranges = nullptr;
    fake.rate_range_count = 0U;
    fake.readback_rate_hz = 3072025.0;
    const uint32_t high_hz = 4293000000U;
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(fake.bandwidth_request_hz);
    rtl_stream_test_capture_result got{};
    got.rc = -100;
    if (dev && opts) {
        expect_true("the open seam runs",
                    rtl_stream_test_open_radio_capture(RTL_STREAM_TEST_SOURCE_SOAPY, dev, opts, high_hz, &got) == 0);
    }
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
    expect_true("the open starts", got.rc == 0);
    expect_eq("it runs the 3,072,025 Hz read back", got.capture_rate_hz, 3072025.0);

    /* The same device started at 10 MS/s further down, then retuned near the top. */
    static const double delivered[] = {0.0, 3072025.0};
    rtl_device_test_soapy_gain_fake hopper = fake;
    hopper.readback_rate_hz = 0.0;
    hopper.rate_writes = 0;
    hopper.delivered_by_write = delivered;
    hopper.delivered_by_write_count = 2U;
    dev = rtl_device_test_soapy_gain_device(&hopper, 0);
    opts = make_soapy_opts(hopper.bandwidth_request_hz);
    if (dev && opts) {
        dsd_exitflag_store(0);
        rtl_stream_test_capture_result hop{};
        expect_true("session begins", rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, &hop) == 0);
        expect_eq("the session starts at 10 MS/s", hop.capture_rate_hz, 10000000.0);
        expect_true("the retune runs",
                    rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_MANUAL_RETUNE, high_hz, 48000, &hop)
                        == 0);
        expect_true("the retune near the top lands", hop.completion != RTL_STREAM_TUNE_FAILED);
        expect_eq("it runs the 3,072,025 Hz read back", hop.capture_rate_hz, 3072025.0);
        rtl_stream_test_capture_session_end();
        dsd_exitflag_store(0);
    }
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
}

/* A device that lists only 2.4 GS/s and reports no usable rate (infinity): the rate written is past every capture
 * ceiling, so the open is refused rather than run on a plan for the rate asked for. */
void
test_oversized_rate_written_refused(void) {
    static const double huge[] = {2.4e9};
    rtl_device_test_soapy_gain_fake fake = {};
    fake.driver_key = "generic";
    fake.listed_rates = huge;
    fake.listed_rate_count = 1U;
    fake.readback_rate_hz = positive_infinity();
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(fake.bandwidth_request_hz);
    rtl_stream_test_capture_result got{};
    got.rc = -100;
    if (dev && opts) {
        expect_true("the open seam runs",
                    rtl_stream_test_open_radio_capture(RTL_STREAM_TEST_SOURCE_SOAPY, dev, opts, 4000000000U, &got)
                        == 0);
    }
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
    expect_true("an open that wrote 2.4 GS/s is refused", got.rc != 0);

    /* With no usable read-back the rate written stands for it, bounded as a read-back is. */
    fake.rate_writes = 0;
    dev = rtl_device_test_soapy_gain_device(&fake, 0);
    if (!dev) {
        return;
    }
    expect_true("the 2.4 GS/s write takes", rtl_device_set_sample_rate(dev, 2000000U) == 0);
    expect_eq("the rate reported for it", (double)rtl_device_get_sample_rate(dev), (double)INT_MAX);
    rtl_device_test_soapy_gain_device_free(dev);
}

/* A 1,536,000 Hz request written as 1,536,025 Hz and delivered as 1,536,000 Hz: the plan made on the rate delivered
 * still counts the device as having forced its rate, since it was given another than the request. */
void
test_delivered_plan_keeps_the_forced_flag(void) {
    dsd_opts* opts = make_soapy_opts(-1);
    if (!opts) {
        return;
    }
    rtl_stream_test_live_chain_result r{};
    expect_true("the live chain seam runs",
                rtl_stream_test_live_rate_chain(opts, DSD_RATE_PLAN_BACKEND_SOAPY, 48000, 1536025U, 1536000U, &r) == 0);
    free(opts);
    expect_true("the open starts", r.rc == 0);
    expect_eq("the device forced its rate", (double)r.forced, 1.0);
}

/* A device that lists 3,072,025 Hz, which the 48 kHz chain cannot use, and delivers 3,072,000 Hz, which it can: the
 * plan refused on the rate written is made again on the rate delivered. */
void
test_usable_readback_of_an_unusable_rate(void) {
    static const double listed[] = {3072025.0};
    rtl_device_test_soapy_gain_fake fake = {};
    fake.driver_key = "generic";
    fake.listed_rates = listed;
    fake.listed_rate_count = 1U;
    fake.readback_rate_hz = 3072000.0;
    const rtl_stream_test_capture_result got = open_on(&fake);
    expect_true("the open starts", got.rc == 0);
    expect_eq("it runs the 3,072,000 Hz delivered", got.capture_rate_hz, 3072000.0);
}

/* A HackRF near the top of the tuning range: the preferred 10 MS/s would put the capture 2.5 MHz above the centre,
 * past what a frequency holds, but the rate it is asked for instead does not. */
void
test_preference_out_of_tuning_range(void) {
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(fake.bandwidth_request_hz);
    rtl_stream_test_capture_result got{};
    got.rc = -100;
    const uint32_t center_hz = 4293000000U;
    if (dev && opts) {
        expect_true("the open seam runs",
                    rtl_stream_test_open_radio_capture(RTL_STREAM_TEST_SOURCE_SOAPY, dev, opts, center_hz, &got) == 0);
    }
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
    expect_true("the open starts", got.rc == 0);
    expect_true("a rate is written", fake.rate_writes > 0);
    expect_eq("the capture runs 2 MS/s", got.capture_rate_hz, 2000000.0);
    expect_eq("the capture sits a quarter of it above the centre", got.capture_freq_hz, (double)center_hz + 500000.0);
}

/* A device that advertises a rate above the ceiling but delivers one within it starts: the ceiling holds on the rate
 * read back, and the ring is sized for it. */
void
test_advertised_above_ceiling_delivers_within(void) {
    static const double only_12m[] = {12e6};
    rtl_device_test_soapy_gain_fake fake = {};
    fake.driver_key = "generic";
    fake.listed_rates = only_12m;
    fake.listed_rate_count = 1U;
    fake.readback_rate_hz = 8e6;
    const rtl_stream_test_capture_result got = open_on(&fake);
    expect_true("a device that delivers 8 MS/s starts", got.rc == 0);
    expect_eq("it runs the read-back", got.capture_rate_hz, 8000000.0);
    expect_eq("its ring holds 8 MS/s", (double)got.input_ring_capacity_floats, 8388608.0);

    /* An advertised rate the DSP chain cannot use is only a request: the chain is planned on the rate read back. */
    static const double unusable[] = {12288100.0};
    fake = {};
    fake.driver_key = "generic";
    fake.listed_rates = unusable;
    fake.listed_rate_count = 1U;
    fake.readback_rate_hz = 8e6;
    const rtl_stream_test_capture_result unusable_got = open_on(&fake);
    expect_true("a device advertising an unusable rate that delivers 8 MS/s starts", unusable_got.rc == 0);
    expect_eq("it runs the 8 MS/s read back", unusable_got.capture_rate_hz, 8000000.0);
    /* Within the ceiling, a device that delivers a rate the chain cannot use is refused once it reads it back. */
    static const double unusable_within[] = {3072025.0};
    fake.listed_rates = unusable_within;
    fake.readback_rate_hz = 0.0;
    expect_true("one that delivers the unusable rate is refused", open_on(&fake).rc != 0);
}

/* An input ring that cannot be sized for the rate leaves the stream's working ring in place, so the open fails cleanly
 * and its teardown still has the ring's lock and conditions. */
void
test_failed_ring_sizing_keeps_ring(void) {
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    rtl_stream_test_fail_input_ring_sizing(1);
    const rtl_stream_test_capture_result got = open_on(&fake);
    rtl_stream_test_fail_input_ring_sizing(0);
    expect_true("the open fails", got.rc != 0);
    expect_eq("the working ring stays", (double)got.input_ring_capacity_floats, 2097152.0);

    /* A raw I/Q stream's sizing, which replaces the default ring, keeps it on the same failure. */
    size_t capacity = 0U;
    rtl_stream_test_fail_input_ring_sizing(1);
    const int net_rc = rtl_stream_test_size_net_input(48000U, &capacity);
    rtl_stream_test_fail_input_ring_sizing(0);
    expect_true("the I/Q stream sizing fails", net_rc != 0);
    expect_eq("the I/Q stream keeps the default ring", (double)capacity, 2097152.0);
    expect_true("the I/Q stream sizing takes", rtl_stream_test_size_net_input(48000U, &capacity) == 0 && capacity > 0U);
}

/* What the driver reports after a filter write is what the warnings judge: an ADALM-Pluto whose 1 MHz write left
 * 200 kHz in place is warned about, and a default whose widest filter is still too narrow is not called opened. */
void
test_bandwidth_notices_follow_readback(void) {
    rtl_device_test_soapy_gain_fake pluto = pluto_fake();
    pluto.readback_bandwidth_hz = 200000.0;
    dsd_test_capture_stderr cap;
    expect_true("stderr capture begins", dsd_test_capture_stderr_begin(&cap, "soapy_rate") == 0);
    (void)program(&pluto, 1536000U);
    static const double only_10m[] = {10e6};
    static const double narrow[] = {200000.0, 1000000.0};
    rtl_device_test_soapy_gain_fake narrow_fake = sdrplay_fake();
    narrow_fake.listed_rates = only_10m;
    narrow_fake.listed_rate_count = 1U;
    narrow_fake.bandwidths = narrow;
    narrow_fake.bandwidth_count = 2U;
    (void)program(&narrow_fake, 1536000U);
    (void)dsd_test_capture_stderr_end(&cap);
    char log[8192];
    expect_true("stderr capture reads", dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0);
    expect_true("the filter the driver kept is warned about",
                strstr(log, "200000 Hz is narrower than 840000 Hz") != nullptr);
    expect_true("the widest filter is warned about", strstr(log, "1000000 Hz is narrower than 5072000 Hz") != nullptr);
    if (count_text(log, "opened the profile's") != 0) {
        DSD_FPRINTF(stderr, "FAIL a filter that does not keep the channel is called opened; log:\n%s\n", log);
        g_failures++;
    }
}

/* An SDRplay listing only 10 MS/s whose filters stop at 1 MHz, narrower than the Fs/4 channel needs. */
rtl_device_test_soapy_gain_fake
narrow_filter_fake(void) {
    static const double only_10m[] = {10e6};
    static const double narrow[] = {200000.0, 1000000.0};
    rtl_device_test_soapy_gain_fake fake = sdrplay_fake();
    fake.listed_rates = only_10m;
    fake.listed_rate_count = 1U;
    fake.bandwidths = narrow;
    fake.bandwidth_count = 2U;
    return fake;
}

/* A driver whose getBandwidth() reports infinity has reported nothing: the filter written is judged, and the 1 MHz
 * filter at 10 MS/s is warned about. */
void
test_nonfinite_bandwidth_readback(void) {
    rtl_device_test_soapy_gain_fake fake = narrow_filter_fake();
    fake.readback_bandwidth_hz = positive_infinity();
    dsd_test_capture_stderr cap;
    expect_true("stderr capture begins", dsd_test_capture_stderr_begin(&cap, "soapy_rate") == 0);
    (void)program(&fake, 1536000U);
    (void)dsd_test_capture_stderr_end(&cap);
    char log[4096];
    expect_true("stderr capture reads", dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0);
    expect_true("an infinite read-back falls back to the filter written",
                strstr(log, "1000000 Hz is narrower than 5072000 Hz") != nullptr);
}

/* With the Fs/4 shift off no filter notice applies, so a filter write is not followed by a read-back: a network
 * driver would pay a round trip on every retune for nothing. */
void
test_no_bandwidth_readback_without_fs4(void) {
    rtl_device_test_soapy_gain_fake fake = narrow_filter_fake();
    const int saved = disable_fs4_shift;
    disable_fs4_shift = 1;
    (void)program(&fake, 1536000U);
    disable_fs4_shift = saved;
    expect_true("the filter is written", fake.bandwidth_writes > 0);
    expect_eq("getBandwidth() calls with the Fs/4 shift off", (double)fake.bandwidth_reads, 0.0);
}

/* The opened filter is reported once the driver took it: a write that throws says nothing, and the retry that takes
 * it says so. */
void
test_opened_notice_after_write(void) {
    rtl_device_test_soapy_gain_fake fake = hackrf_fake();
    fake.bandwidth_write_throws = 1;
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    if (!dev) {
        return;
    }
    dsd_test_capture_stderr cap;
    expect_true("stderr capture begins", dsd_test_capture_stderr_begin(&cap, "soapy_rate") == 0);
    expect_true("rate write takes", rtl_device_set_sample_rate(dev, 10000000U) == 0);
    const int failed = rtl_device_set_tuner_bandwidth(dev, 0U);
    (void)dsd_test_capture_stderr_end(&cap);
    char log[4096];
    expect_true("stderr capture reads", dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0);
    expect_true("the refused write fails", failed != 0);
    if (count_text(log, "opened the profile's") != 0) {
        DSD_FPRINTF(stderr, "FAIL a refused filter write is reported as opened; log:\n%s\n", log);
        g_failures++;
    }
    expect_true("stderr capture begins again", dsd_test_capture_stderr_begin(&cap, "soapy_rate") == 0);
    const int retried = rtl_device_set_tuner_bandwidth(dev, 0U);
    (void)dsd_test_capture_stderr_end(&cap);
    expect_true("stderr capture reads again", dsd_test_capture_stderr_read(&cap, log, sizeof log) == 0);
    expect_true("the retry takes", retried == 0);
    expect_eq("the retry writes the opened filter", fake.last_bandwidth_hz, 5500000.0);
    if (count_text(log, "opened the profile's") != 1) {
        DSD_FPRINTF(stderr, "FAIL the retry that took the filter does not report it once; log:\n%s\n", log);
        g_failures++;
    }
    rtl_device_test_soapy_gain_device_free(dev);
}

/* A retune whose rate write fails while the device moved to a rate the chain takes runs the filter for that rate:
 * an SDRplay going from 2 MS/s to 1 MS/s narrows its filter from 1.536 MHz to 600 kHz. */
void
test_failed_rate_write_reapplies_filter(void) {
    rtl_device_test_soapy_gain_fake fake = sdrplay_fake();
    rtl_device* dev = rtl_device_test_soapy_gain_device(&fake, 0);
    dsd_opts* opts = make_soapy_opts(-1);
    if (!dev || !opts) {
        free(opts);
        rtl_device_test_soapy_gain_device_free(dev);
        return;
    }
    rtl_stream_test_capture_result got{};
    expect_true("session begins", rtl_stream_test_capture_session_begin(dev, opts, kCenterHz, &got) == 0);
    expect_eq("filter at 2 MS/s", fake.last_bandwidth_hz, 1536000.0);
    fake.rate_write_throws = 1;
    (void)rtl_stream_test_capture_session_step(RTL_STREAM_TEST_CAPTURE_HOP, kCenterHz + 25000U, 72000, &got);
    fake.rate_write_throws = 0;
    expect_eq("the device moved to 1 MS/s", fake.rate_hz, 1000000.0);
    expect_eq("the stream runs the 1 MS/s read-back", got.capture_rate_hz, 1000000.0);
    expect_eq("filter for 1 MS/s", fake.last_bandwidth_hz, 600000.0);
    rtl_stream_test_capture_session_end();
    free(opts);
    rtl_device_test_soapy_gain_device_free(dev);
}

} // namespace

int
main(int argc, char** argv) {
    (void)dsd_test_unsetenv("DSD_NEO_TUNER_BW_HZ");
    (void)dsd_test_unsetenv("DSD_NEO_DISABLE_FS4_SHIFT");
    dsd_neo_config_init();
    rtl_device_test_soapy_gain_fake probe{};
    rtl_device* dev = rtl_device_test_soapy_gain_device(&probe, 0);
    if (dev == nullptr) {
        printf("IO_RTL_DEVICE_SOAPY_RATE: no SoapySDR in this build; skipped\n");
        return 0;
    }
    rtl_device_test_soapy_gain_device_free(dev);

    static const struct {
        const char* name;
        void (*run)(void);
    } k_cases[] = {
        {"hackrf_rate", test_hackrf_runs_preferred_rate},
        {"default_filters", test_default_filters_cover_offset_channel},
        {"unchanged", test_unchanged_filters_and_rates},
        {"explicit_warns", test_explicit_narrow_bandwidth_warns},
        {"open", test_open_capture_and_ring},
        {"filter_follows_rate", test_filter_follows_rate_on_retune},
        {"write_programs_asked_rate", test_write_programs_asked_rate},
        {"rollback_restores_saved_rate", test_rollback_restores_saved_rate},
        {"rollback_restores_delivered_rate", test_rollback_restores_delivered_rate},
        {"advertised_above_ceiling", test_advertised_above_ceiling_delivers_within},
        {"rollback_plans_the_rate_written", test_rollback_plans_the_rate_written},
        {"rollback_falls_back_to_the_request", test_rollback_falls_back_to_the_request},
        {"usable_readback_of_an_unusable_rate", test_usable_readback_of_an_unusable_rate},
        {"rollback_stops_once_restored", test_rollback_stops_once_restored},
        {"expect_eq_rejects_nan", test_expect_eq_rejects_nan},
        {"tied_points_keep_the_planned_rate", test_tied_points_keep_the_planned_rate},
        {"fallback_rate_near_the_top", test_fallback_rate_near_the_top},
        {"delivered_plan_keeps_the_forced_flag", test_delivered_plan_keeps_the_forced_flag},
        {"rollback_retries_after_a_refused_write", test_rollback_retries_after_a_refused_write},
        {"rate_written_judges_the_readback", test_rate_written_judges_the_readback},
        {"oversized_rate_written_refused", test_oversized_rate_written_refused},
        {"preference_out_of_tuning_range", test_preference_out_of_tuning_range},
        {"oversized_readback_refused", test_oversized_readback_refused},
        {"failed_ring_sizing_keeps_ring", test_failed_ring_sizing_keeps_ring},
        {"bandwidth_notices_follow_readback", test_bandwidth_notices_follow_readback},
        {"opened_notice_after_write", test_opened_notice_after_write},
        {"failed_rate_write_reapplies_filter", test_failed_rate_write_reapplies_filter},
        {"nonfinite_bandwidth_readback", test_nonfinite_bandwidth_readback},
        {"no_bandwidth_readback_without_fs4", test_no_bandwidth_readback_without_fs4},
    };

    /* An argument runs that case alone. */
    for (const auto& c : k_cases) {
        if (argc < 2 || strcmp(argv[1], c.name) == 0) {
            c.run();
        }
    }
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "IO_RTL_DEVICE_SOAPY_RATE: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("IO_RTL_DEVICE_SOAPY_RATE: OK\n");
    return 0;
}
