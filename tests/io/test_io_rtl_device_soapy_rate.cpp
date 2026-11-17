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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/config.h>
#include "dsd-neo/core/opts_fwd.h"
#include "rtl_stream_test_support.h"
#include "test_env.h"
#include "test_support.h"

struct rtl_device;

namespace {

int g_failures = 0;

void
expect_eq(const char* label, double got, double want) {
    if (got < want - 0.5 || got > want + 0.5) {
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

/* The rate the device programs for @p request_hz, after the rate the stream plans it at, and the bandwidth it then
 * writes for the driver-automatic tuner bandwidth the stream asks for. */
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
    expect_true("rate write takes", rtl_device_set_sample_rate(dev, request_hz) == 0);
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

    rtl_device_test_soapy_gain_fake pluto = pluto_fake();
    got = program(&pluto, 1536000U);
    expect_eq("ADALM-Pluto rate for a 1536000 Hz request", got.rate_hz, 2000000.0);
    expect_eq("ADALM-Pluto filter at 2 MS/s", got.bandwidth_hz, 2000000.0);
    pluto = pluto_fake();
    got = program(&pluto, 1152000U);
    expect_eq("ADALM-Pluto rate for a 1152000 Hz request", got.rate_hz, 1000000.0);
    expect_eq("ADALM-Pluto filter at 1 MS/s", got.bandwidth_hz, 1000000.0);
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
