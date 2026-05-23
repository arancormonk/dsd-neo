// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "soapy_profile.h"

using dsdneo::SoapyProfileId;
using dsdneo::SoapyProfileSelection;
using dsdneo::SoapyRange;

static int
expect_true(const char* label, bool value) {
    if (!value) {
        DSD_FPRINTF(stderr, "%s failed\n", label);
        return 1;
    }
    return 0;
}

static int
expect_profile(const char* label, const SoapyProfileSelection& selection, SoapyProfileId want) {
    SoapyProfileId got = dsdneo::soapy_select_profile_id(selection);
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got profile=%d want=%d\n", label, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static int
expect_string(const char* label, const std::string& got, const char* want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", label, got.c_str(), want);
        return 1;
    }
    return 0;
}

static int
expect_double(const char* label, double got, double want) {
    if (std::fabs(got - want) > 0.5) {
        DSD_FPRINTF(stderr, "%s: got %.3f want %.3f\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bandwidth(const char* label, const dsdneo::SoapyBandwidthChoice& got, bool should_apply, int bandwidth_hz,
                 bool explicit_request) {
    if (got.should_apply != should_apply || got.bandwidth_hz != bandwidth_hz
        || got.explicit_request != explicit_request) {
        DSD_FPRINTF(stderr, "%s: got apply=%d hz=%d explicit=%d want apply=%d hz=%d explicit=%d\n", label,
                    got.should_apply ? 1 : 0, got.bandwidth_hz, got.explicit_request ? 1 : 0, should_apply ? 1 : 0,
                    bandwidth_hz, explicit_request ? 1 : 0);
        return 1;
    }
    return 0;
}

static int
test_profile_selection(void) {
    int rc = 0;
    rc |= expect_profile("airspy driver", {"", "airspy", "", ""}, SoapyProfileId::Airspy);
    rc |= expect_profile("sdrplay args", {"", "", "", "driver=sdrplay,serial=123"}, SoapyProfileId::Sdrplay);
    rc |= expect_profile("hackrf hardware", {"", "", "HackRF One", ""}, SoapyProfileId::Hackrf);
    rc |= expect_profile("requested overrides auto", {"generic", "airspy", "", ""}, SoapyProfileId::Generic);
    rc |= expect_profile("unknown falls back generic", {"", "custom", "unknown", ""}, SoapyProfileId::Generic);
    return rc;
}

static int
test_bandwidth_selection(void) {
    int rc = 0;
    rc |= expect_bandwidth("profile default applies", dsdneo::soapy_choose_bandwidth_hz(0, false, -1, 250000.0), true,
                           250000, false);
    rc |= expect_bandwidth("env auto preserves driver automatic",
                           dsdneo::soapy_choose_bandwidth_hz(0, true, -1, 250000.0), false, 0, true);
    rc |= expect_bandwidth("env auto overrides soapy config default",
                           dsdneo::soapy_choose_bandwidth_hz(0, true, 200000, 250000.0), false, 0, true);
    rc |= expect_bandwidth("soapy config auto preserves driver automatic",
                           dsdneo::soapy_choose_bandwidth_hz(0, false, 0, 250000.0), false, 0, true);
    rc |= expect_bandwidth("soapy config explicit applies",
                           dsdneo::soapy_choose_bandwidth_hz(0, false, 200000, 250000.0), true, 200000, true);
    return rc;
}

static int
test_stream_format_selection(void) {
    int rc = 0;
    std::vector<std::string> both = {"CS16", "CF32"};
    rc |= expect_string("native supported wins", dsdneo::soapy_choose_stream_format(both, "", "CS16"), "CS16");
    rc |= expect_string("forced cf32", dsdneo::soapy_choose_stream_format(both, "cf32", "CS16"), "CF32");
    rc |= expect_string("auto cf32 fallback", dsdneo::soapy_choose_stream_format({"CF32"}, "auto", ""), "CF32");
    rc |= expect_string("unsupported forced format", dsdneo::soapy_choose_stream_format({"CS16"}, "cf32", ""), "");
    return rc;
}

static int
test_range_selection(void) {
    int rc = 0;
    bool supported = false;
    std::vector<SoapyRange> ranges = {{200000.0, 1000000.0, 100000.0}, {1750000.0, 3000000.0, 250000.0}};
    rc |=
        expect_double("inside stepped range", dsdneo::soapy_nearest_in_ranges(240000.0, ranges, &supported), 200000.0);
    rc |= expect_true("inside supported", supported);
    rc |= expect_double("between ranges", dsdneo::soapy_nearest_in_ranges(1200000.0, ranges, &supported), 1000000.0);
    rc |= expect_double("above ranges", dsdneo::soapy_nearest_in_ranges(4000000.0, ranges, &supported), 3000000.0);

    bool adjusted = false;
    rc |= expect_double("listed sample rate",
                        dsdneo::soapy_nearest_sample_rate(768000.0, {1000000.0, 2500000.0}, {}, &adjusted), 1000000.0);
    rc |= expect_true("listed sample rate adjusted", adjusted);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_profile_selection();
    rc |= test_bandwidth_selection();
    rc |= test_stream_format_selection();
    rc |= test_range_selection();
    return rc;
}
