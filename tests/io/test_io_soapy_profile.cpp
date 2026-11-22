// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "soapy_profile.h"

using dsdneo::SoapyProfileId;
using dsdneo::SoapyProfileSelection;
using dsdneo::SoapyRange;
using dsdneo::SoapySettingScope;
using dsdneo::SoapySettingValueType;
using dsdneo::SoapyStreamFormat;

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
expect_cstr(const char* label, const char* got, const char* want) {
    if (std::string(got ? got : "") != want) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", label, got ? got : "(null)", want);
        return 1;
    }
    return 0;
}

static int
expect_scope(const char* label, SoapySettingScope got, SoapySettingScope want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got scope=%d want=%d\n", label, (int)got, (int)want);
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
    rc |= expect_profile("lime driver", {"", "lime", "", ""}, SoapyProfileId::Lime);
    rc |= expect_profile("pluto hardware", {"", "", "AD9363", ""}, SoapyProfileId::Pluto);
    rc |= expect_profile("rtlsdr args", {"", "", "", "driver=rtl_sdr"}, SoapyProfileId::Rtlsdr);
    rc |= expect_profile("uhd hardware", {"", "", "USRP B200", ""}, SoapyProfileId::Uhd);
    rc |= expect_profile("requested overrides auto", {"generic", "airspy", "", ""}, SoapyProfileId::Generic);
    rc |= expect_profile("unknown falls back generic", {"", "custom", "unknown", ""}, SoapyProfileId::Generic);
    rc |= expect_profile("sddc driver", {"", "SDDC", "", ""}, SoapyProfileId::Sddc);
    rc |= expect_profile("rx888 hardware", {"", "", "RX888 MKII", ""}, SoapyProfileId::Sddc);
    rc |= expect_profile("rx999 hardware", {"", "", "RX999", ""}, SoapyProfileId::Sddc);
    rc |= expect_profile("bbrf103 hardware", {"", "", "BBRF103", ""}, SoapyProfileId::Sddc);
    rc |= expect_profile("sddc args", {"", "", "", "driver=SDDC"}, SoapyProfileId::Sddc);
    rc |= expect_profile("sddc requested by name", {"sddc", "", "", ""}, SoapyProfileId::Sddc);
    /* An Airspy HF+ is not an Airspy: SoapyAirspyHF's driver and hardware key is "AirspyHF", which holds "airspy". */
    rc |= expect_profile("airspyhf driver", {"", "AirspyHF", "", ""}, SoapyProfileId::Generic);
    rc |= expect_profile("airspyhf hardware", {"", "", "AirspyHF", ""}, SoapyProfileId::Generic);
    rc |= expect_profile("airspyhf args", {"", "", "", "driver=airspyhf"}, SoapyProfileId::Generic);
    rc |= expect_profile("airspyhf keys", {"", "AirspyHF", "AirspyHF", "driver=airspyhf,serial=1"},
                         SoapyProfileId::Generic);
    rc |= expect_profile("airspy hf hardware", {"", "", "Airspy HF+ Discovery", ""}, SoapyProfileId::Generic);
    rc |= expect_profile("airspy keys", {"", "Airspy", "AirSpy One", "driver=airspy"}, SoapyProfileId::Airspy);
    rc |= expect_profile("airspy args", {"", "", "", "driver=airspy"}, SoapyProfileId::Airspy);

    SoapyProfileId parsed = SoapyProfileId::Generic;
    rc |= expect_true("empty profile parses auto", dsdneo::soapy_profile_parse_name("", &parsed));
    rc |= expect_true("empty profile returns auto", parsed == SoapyProfileId::Auto);
    rc |= expect_true("profile parse rejects null out", !dsdneo::soapy_profile_parse_name("airspy", nullptr));
    rc |= expect_true("profile parse rejects unknown", !dsdneo::soapy_profile_parse_name("mystery", &parsed));
    rc |= expect_cstr("invalid profile id falls back generic", dsdneo::soapy_profile_by_id((SoapyProfileId)255).name,
                      "generic");
    return rc;
}

static int
expect_antenna(const char* label, const dsdneo::SoapyAntennaChoice& got, const char* want_name, bool want_auto) {
    if (got.name != want_name || got.auto_selected != want_auto) {
        DSD_FPRINTF(stderr, "%s: got name=\"%s\" auto=%d want name=\"%s\" auto=%d\n", label, got.name.c_str(),
                    got.auto_selected ? 1 : 0, want_name, want_auto ? 1 : 0);
        return 1;
    }
    return 0;
}

/* The identity of an opened SoapySDR device, from its exact identifying fields only: the driver key and hardware key
   the handle reported, and the device arguments' driver selectors parsed as key=value pairs (driver=, and a proxy's
   remote:driver=). Hostnames, labels, serials and any other text never count. Anything that names an Airspy makes it
   one; else a proxy (driver key or driver=remote, a remote= key) or nothing known makes it unknown; else a known
   other driver (an Airspy HF+ among them) makes it not an Airspy. */
static int
test_airspy_identity(void) {
    using dsdneo::SoapyReceiver;
    const auto of = [](const char* driver, const char* hardware, const char* args) {
        return dsdneo::soapy_receiver_from_evidence(dsdneo::soapy_identity_evidence(driver, hardware, args));
    };

    struct Case {
        const char *label, *driver, *hardware, *args;
        SoapyReceiver want;
    };

    static const Case cases[] = {
        {"Airspy keys", "Airspy", "Airspy", "", SoapyReceiver::Airspy},
        {"Airspy driver key, no hardware key", "airspy", "", "", SoapyReceiver::Airspy},
        {"Airspy hardware key behind a proxy", "remote", "AirSpy Mini", "", SoapyReceiver::Airspy},
        {"a conflicting pair is an Airspy", "rtlsdr", "AirSpy Mini", "", SoapyReceiver::Airspy},
        {"an HF+ driver with an Airspy hardware key", "AirspyHF", "Airspy", "", SoapyReceiver::Airspy},
        {"the driver selector names an Airspy", "", "", "driver=airspy,serial=0x1234", SoapyReceiver::Airspy},
        {"a proxy's selector names an Airspy", "remote", "custom",
         "driver=remote,remote:driver=airspy,remote=airspyhf-lab", SoapyReceiver::Airspy},
        {"an Airspy HF+", "AirspyHF", "AirspyHF+", "", SoapyReceiver::NotAirspy},
        {"an RTL-SDR", "rtlsdr", "Generic RTL2832U", "", SoapyReceiver::NotAirspy},
        {"a HackRF", "HackRF", "HackRF One", "", SoapyReceiver::NotAirspy},
        {"a known driver with an opaque hardware key", "HackRF", "", "", SoapyReceiver::NotAirspy},
        {"keys failed, a driver selector", "", "", "driver=hackrf,serial=airspy01", SoapyReceiver::NotAirspy},
        {"a label is not identity", "rtlsdr", "", "driver=rtlsdr,label=airspy shelf", SoapyReceiver::NotAirspy},
        {"no keys", "", "", "", SoapyReceiver::Unknown},
        {"opaque keys", "remote", "custom", "", SoapyReceiver::Unknown},
        {"an unknown driver key", "customsdr", "", "", SoapyReceiver::Unknown},
        {"keys failed behind a proxy whose hostname names a HackRF", "", "", "driver=remote,remote=hackrf-lab",
         SoapyReceiver::Unknown},
        {"a HackRF behind a proxy whose hostname names an Airspy", "HackRF", "HackRF One",
         "driver=remote,remote=airspy-lab", SoapyReceiver::Unknown},
        {"a remote key alone is a proxy", "rtlsdr", "", "remote=lab-host", SoapyReceiver::Unknown},
        {"free text is not a selector", "", "", "rtl-sdr-lab", SoapyReceiver::Unknown},
    };
    int rc = 0;
    for (const Case& c : cases) {
        rc |= expect_true(c.label, of(c.driver, c.hardware, c.args) == c.want);
    }
    return rc;
}

static int
test_antenna_selection(void) {
    int rc = 0;
    /* The SDDC profile starts on its HF port, so VHF/UHF work needs the tuner port selected. */
    rc |= expect_antenna("auto selects profile default above split",
                         dsdneo::soapy_choose_antenna("VHF", "", 851375000.0), "VHF", true);
    rc |=
        expect_antenna("no auto selection below split", dsdneo::soapy_choose_antenna("VHF", "", 7100000.0), "", false);
    rc |= expect_antenna("split boundary is exclusive",
                         dsdneo::soapy_choose_antenna("VHF", "", dsdneo::kSoapyHfVhfSplitHz), "", false);
    rc |= expect_antenna("explicit antenna wins", dsdneo::soapy_choose_antenna("VHF", "HF", 851375000.0), "HF", false);
    rc |= expect_antenna("explicit antenna wins below split", dsdneo::soapy_choose_antenna("VHF", "HF", 7100000.0),
                         "HF", false);
    rc |= expect_antenna("configured antenna is trimmed", dsdneo::soapy_choose_antenna(nullptr, "  RX  ", 0.0), "RX",
                         false);
    rc |= expect_antenna("no profile default leaves driver choice",
                         dsdneo::soapy_choose_antenna(nullptr, "", 851375000.0), "", false);
    rc |= expect_antenna("empty profile default leaves driver choice",
                         dsdneo::soapy_choose_antenna("", "", 851375000.0), "", false);
    rc |= expect_antenna("unknown frequency skips auto selection", dsdneo::soapy_choose_antenna("VHF", "", 0.0), "",
                         false);

    rc |= expect_cstr("sddc profile defaults to VHF", dsdneo::soapy_profile_by_id(SoapyProfileId::Sddc).default_antenna,
                      "VHF");
    rc |= expect_true("generic profile has no default antenna",
                      dsdneo::soapy_profile_by_id(SoapyProfileId::Generic).default_antenna == nullptr);
    return rc;
}

static int
test_sddc_rate_grid(void) {
    int rc = 0;
    bool adjusted = false;
    /* SoapySDDC exposes {2,4,8,16,32,64} MSPS at the stock 128 MHz ADC clock. dsd-neo asks for
       1.536 MSPS with the default 48 kHz DSP bandwidth, so it must land on 2 MSPS. */
    const std::vector<double> stock_rates = {2e6, 4e6, 8e6, 16e6, 32e6, 64e6};
    rc |= expect_double("sddc stock grid snaps up",
                        dsdneo::soapy_nearest_sample_rate(1536000.0, stock_rates, {}, &adjusted), 2000000.0);
    rc |= expect_true("sddc stock grid reports adjustment", adjusted);

    /* At adc_frequency=98304000 the grid starts exactly on the requested rate. */
    const std::vector<double> tuned_rates = {1536000.0, 3072000.0, 6144000.0, 12288000.0, 24576000.0, 49152000.0};
    rc |= expect_double("sddc tuned grid matches request",
                        dsdneo::soapy_nearest_sample_rate(1536000.0, tuned_rates, {}, &adjusted), 1536000.0);
    rc |= expect_true("sddc tuned grid reports no adjustment", !adjusted);

    rc |= expect_bandwidth("sddc profile requests no hardware bandwidth",
                           dsdneo::soapy_choose_bandwidth_hz(
                               0, false, -1, dsdneo::soapy_profile_by_id(SoapyProfileId::Sddc).default_bandwidth_hz),
                           false, 0, false);
    return rc;
}

/* The advertised rate nearest a request among those not above a limit: listed rates past it are left out, a range is
 * cut back to its last point at or below it, and nothing at or below it is no answer. */
static int
test_nearest_rate_within_limit(void) {
    int rc = 0;
    bool found = false;
    const std::vector<double> hackrf_listed = {1e6, 2e6, 3e6, 4e6, 10e6};
    const std::vector<dsdneo::SoapyRange> hackrf_range = {{2e6, 20e6, 0.0}};
    rc |= expect_double(
        "a 10 MS/s list under a 3.2 MS/s limit gives 2 MS/s",
        dsdneo::soapy_nearest_sample_rate_within(1536000.0, 3200000.0, hackrf_listed, hackrf_range, &found), 2000000.0);
    rc |= expect_true("found under the limit", found);
    const std::vector<double> ends = {1500000.0, 10e6};
    const std::vector<dsdneo::SoapyRange> between = {{1500000.0, 10e6, 0.0}};
    /* The range reaches past the limit: its point at the limit's whole hertz is nearer the request than the listed
       1.5 MS/s, and the two are compared. */
    rc |=
        expect_double("a request the range holds past the limit gives the range's point at the limit",
                      dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, ends, between, &found), 1520003.0);
    rc |= expect_double("with no range, the listed rate under the limit",
                        dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, ends, {}, &found), 1500000.0);
    const std::vector<dsdneo::SoapyRange> stepped = {{1e6, 10e6, 250000.0}};
    rc |= expect_double("a stepped range is cut to its last step under the limit",
                        dsdneo::soapy_nearest_sample_rate_within(3000000.0, 2600000.0, {}, stepped, &found), 2500000.0);
    rc |= expect_true("found in the cut range", found);
    (void)dsdneo::soapy_nearest_sample_rate_within(1536000.0, 900000.0, ends, between, &found);
    rc |= expect_true("nothing advertised under the limit is no answer", !found);
    rc |= expect_double("a device that advertises nothing is given the request",
                        dsdneo::soapy_nearest_sample_rate_within(1536000.0, 3200000.0, {}, {}, &found), 1536000.0);
    rc |= expect_true("found for a device that advertises nothing", found);
    (void)dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1000000.0, {}, {}, &found);
    rc |= expect_true("nor above the limit", !found);
    /* In whole hertz, as a rate is programmed and read back: 1,520,003.2 Hz rounds to the 1,520,003 Hz limit. */
    rc |= expect_double("a rate that rounds to the limit is within it",
                        dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, {1520003.2, 10e6}, {}, &found),
                        1520003.2);
    rc |= expect_true("found once rounded", found);
    (void)dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, {1520003.5, 10e6}, {}, &found);
    rc |= expect_true("one that rounds above it is not", !found);
    /* A range's point nearer the request beats a listed rate further away. */
    const std::vector<double> far_listed = {3072025.0, 10e6};
    const std::vector<dsdneo::SoapyRange> halves = {{1000000.0, 2000000.0, 500000.0}};
    rc |= expect_double("the nearer range point wins over a listed rate",
                        dsdneo::soapy_nearest_sample_rate_within(1536000.0, 4000003.0, far_listed, halves, &found),
                        1500000.0);
    /* A range's point is one the device advertises, within the cap once rounded, and a write of it keeps it: a
       one-point range at 1,520,003.2 Hz under a 1,520,003 Hz cap, and a range in 3 Hz steps from 1,520,000.5 Hz whose
       next point, 1,520,003.5 Hz, rounds over it. */
    const std::vector<dsdneo::SoapyRange> singleton = {{1520003.2, 1520003.2, 0.0}};
    const std::vector<dsdneo::SoapyRange> thirds = {{1520000.5, 1600000.0, 3.0}};
    for (const auto* ranges : {&singleton, &thirds}) {
        const double point = dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, {}, *ranges, &found);
        bool adjusted = false;
        const double written = dsdneo::soapy_nearest_sample_rate(point, {}, *ranges, &adjusted);
        rc |= expect_true("a range point within the cap is found", found);
        rc |= expect_double("the setter keeps the point", written, point);
        rc |= expect_true("which rounds within the cap", std::llround(written) <= 1520003LL);
    }
    rc |=
        expect_double("the one-point range's point",
                      dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, {}, singleton, &found), 1520003.2);
    rc |= expect_double("the stepped range's last point within the cap",
                        dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, {}, thirds, &found), 1520000.5);
    /* The range's last point is the one range_last_point() gives, clamped to the maximum within rounding error: a
       range from 1,020,003.5 Hz to 1,520,003.4999 Hz in 500 kHz steps ends at its maximum, which rounds within the
       cap. */
    const std::vector<dsdneo::SoapyRange> clamped_end = {{1020003.5, 1520003.4999, 500000.0}};
    rc |= expect_double("the clamped last point within the cap",
                        dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, {}, clamped_end, &found),
                        1520003.4999);
    /* A grid finer than a double resolves at this magnitude still yields a point within the cap, and the query ends. */
    const std::vector<dsdneo::SoapyRange> fine = {{1e6, 2e6, std::ldexp(1.0, -36)}};
    const double fine_point = dsdneo::soapy_nearest_sample_rate_within(1536000.0, 1520003.0, {}, fine, &found);
    rc |= expect_true("a fine grid's point is found", found);
    rc |= expect_true("and rounds within the cap", std::llround(fine_point) <= 1520003LL);
    rc |= expect_double("a listed rate nearer than every range point wins",
                        dsdneo::soapy_nearest_sample_rate_within(2900000.0, 4000003.0, far_listed, halves, &found),
                        3072025.0);
    /* A stepped range whose maximum no 64-bit integer holds is still cut at the cap. */
    const std::vector<dsdneo::SoapyRange> wide = {{1e6, 1e19, 1000.0}};
    rc |= expect_double("a range past every integer is cut at the cap",
                        dsdneo::soapy_nearest_sample_rate_within(3e6, 2250000.0, {}, wide, &found), 2250000.0);
    rc |= expect_true("and its point is found", found);
    const std::vector<dsdneo::SoapyRange> widest = {{1e6, DBL_MAX, 1000.0}};
    rc |= expect_double("so is one up to the largest double",
                        dsdneo::soapy_nearest_sample_rate_within(3e6, 2e6, {}, widest, &found), 2e6);
    rc |= expect_true("and its point is found", found);
    /* A step so fine that the range's step count overflows a double has no grid point a write could keep: the
       bounded query refuses the range, as the setter cannot program its answer. */
    const std::vector<dsdneo::SoapyRange> overflowing = {{1e6, DBL_MAX, DBL_MIN}};
    (void)dsdneo::soapy_nearest_sample_rate_within(1.5e6, 2e6, {}, overflowing, &found);
    rc |= expect_true("a range whose step count overflows has no point within the cap", !found);
    /* A point below the cap is the grid point the setter itself computes (minimum + index * step), so a write of the
       answer programs it, for steps a double holds only approximately. */
    const std::vector<dsdneo::SoapyRange> inexact_a = {{1000000.5, 5000000.0, 249999.99999999994}};
    const std::vector<dsdneo::SoapyRange> inexact_b = {{1000003.5, 3000000.0, std::nextafter(62500.0, 0.0)}};

    const struct {
        const std::vector<dsdneo::SoapyRange>* ranges;
        double requested;
        double cap;
    } inexact[] = {{&inexact_a, 3e6, 2250000.0}, {&inexact_b, 2.5e6, 2125003.0}};

    for (const auto& c : inexact) {
        const double point = dsdneo::soapy_nearest_sample_rate_within(c.requested, c.cap, {}, *c.ranges, &found);
        bool adjusted = false;
        const double written = dsdneo::soapy_nearest_sample_rate(point, {}, *c.ranges, &adjusted);
        rc |= expect_true("an inexact grid's point is found", found);
        rc |= expect_true("and rounds within the cap", std::llround(point) <= std::llround(c.cap));
        rc |= expect_true("and a write of it programs the same whole hertz",
                          std::llround(written) == std::llround(point));
    }
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
    rc |= expect_string("forced cs16", dsdneo::soapy_choose_stream_format(both, "cs16", "CF32"), "CS16");
    rc |= expect_string("auto cf32 fallback", dsdneo::soapy_choose_stream_format({"CF32"}, "auto", ""), "CF32");
    rc |= expect_string("auto cs16 fallback", dsdneo::soapy_choose_stream_format({"CS16"}, "auto", ""), "CS16");
    rc |= expect_string("unsupported forced format", dsdneo::soapy_choose_stream_format({"CS16"}, "cf32", ""), "");
    rc |= expect_string("unknown forced format", dsdneo::soapy_choose_stream_format(both, "cu8", ""), "");
    rc |= expect_string("no supported formats", dsdneo::soapy_choose_stream_format({}, "", "CS16"), "");

    SoapyStreamFormat format = SoapyStreamFormat::CF32;
    rc |= expect_true("empty stream format parses auto", dsdneo::soapy_stream_format_parse_name("", &format));
    rc |= expect_true("empty stream format returns auto", format == SoapyStreamFormat::Auto);
    rc |= expect_true("stream format rejects null out", !dsdneo::soapy_stream_format_parse_name("CF32", nullptr));
    rc |= expect_true("stream format rejects unknown", !dsdneo::soapy_stream_format_parse_name("cu8", &format));
    rc |= expect_cstr("stream format name cf32", dsdneo::soapy_stream_format_name(SoapyStreamFormat::CF32), "CF32");
    rc |= expect_cstr("stream format name cs16", dsdneo::soapy_stream_format_name(SoapyStreamFormat::CS16), "CS16");
    rc |= expect_cstr("stream format name default", dsdneo::soapy_stream_format_name((SoapyStreamFormat)255), "auto");
    return rc;
}

static int
expect_full_scale(const char* label, const char* stream_format, const char* native_format, double reported,
                  double want) {
    const double got = dsdneo::soapy_cs16_full_scale(stream_format, native_format, reported);
    if (std::fabs(got - want) > 1e-9) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f\n", label, got, want);
        return 1;
    }
    return 0;
}

/* Native CS16 is read against the driver's own full scale; anything else at 32768. */
static int
test_cs16_full_scale(void) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    int rc = 0;
    rc |= expect_full_scale("pluto/bladerf native 2048", "CS16", "CS16", 2048.0, 2048.0);
    rc |= expect_full_scale("12-bit 2047 convention", "CS16", "CS16", 2047.0, 2047.0);
    rc |= expect_full_scale("airspy/lms7 native 32767", "CS16", "CS16", 32767.0, 32767.0);
    rc |= expect_full_scale("uhd native 32768", "CS16", "CS16", 32768.0, 32768.0);
    rc |= expect_full_scale("smallest accepted", "CS16", "CS16", 1.0, 1.0);
    rc |= expect_full_scale("cs16 not native (rtlsdr cs8)", "CS16", "CS8", 128.0, 32768.0);
    rc |= expect_full_scale("cs16 native unknown", "CS16", "", 2048.0, 32768.0);
    rc |= expect_full_scale("cf32 stream", "CF32", "CS16", 2048.0, 32768.0);
    rc |= expect_full_scale("cf32 native", "CS16", "CF32", 1.0, 32768.0);
    rc |= expect_full_scale("zero", "CS16", "CS16", 0.0, 32768.0);
    rc |= expect_full_scale("below one", "CS16", "CS16", 0.5, 32768.0);
    rc |= expect_full_scale("negative", "CS16", "CS16", -1.0, 32768.0);
    rc |= expect_full_scale("nan", "CS16", "CS16", nan, 32768.0);
    rc |= expect_full_scale("inf", "CS16", "CS16", inf, 32768.0);
    rc |= expect_full_scale("past 16 bits", "CS16", "CS16", 65536.0, 32768.0);
    rc |= expect_full_scale("just past 32768", "CS16", "CS16", 32768.5, 32768.0);
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
    rc |= expect_double("empty ranges preserve request", dsdneo::soapy_nearest_in_ranges(123.0, {}, &supported), 123.0);
    rc |= expect_true("empty ranges unsupported", !supported);
    rc |= expect_double("invalid ranges preserve request",
                        dsdneo::soapy_nearest_in_ranges(123.0, {{10.0, 1.0, 1.0}}, &supported), 123.0);
    rc |= expect_true("invalid ranges unsupported", !supported);

    bool adjusted = false;
    rc |= expect_double("listed sample rate",
                        dsdneo::soapy_nearest_sample_rate(768000.0, {1000000.0, 2500000.0}, {}, &adjusted), 1000000.0);
    rc |= expect_true("listed sample rate adjusted", adjusted);
    rc |= expect_double("nonpositive sample rate preserved",
                        dsdneo::soapy_nearest_sample_rate(0.0, {1000000.0}, ranges, &adjusted), 0.0);
    rc |= expect_true("nonpositive sample rate not adjusted", !adjusted);
    rc |= expect_double("range sample rate", dsdneo::soapy_nearest_sample_rate(1200000.0, {}, ranges, &adjusted),
                        1000000.0);
    rc |= expect_true("range sample rate adjusted", adjusted);
    rc |= expect_double("no rate ranges preserves request", dsdneo::soapy_nearest_sample_rate(123.0, {}, {}, &adjusted),
                        123.0);
    rc |= expect_true("no rate ranges not adjusted", !adjusted);

    rc |= expect_true("name list contains exact value", dsdneo::soapy_name_list_contains({"CF32", "CS16"}, "CS16"));
    rc |= expect_true("name list misses case mismatch", !dsdneo::soapy_name_list_contains({"CF32"}, "cf32"));
    return rc;
}

static int
test_join_and_scope_names(void) {
    int rc = 0;
    rc |= expect_string("join empty", dsdneo::soapy_join_names({}, 0), "-");
    rc |= expect_string("join unlimited", dsdneo::soapy_join_names({"CF32", "CS16"}, 0), "CF32,CS16");
    rc |= expect_string("join truncated", dsdneo::soapy_join_names({"CF32", "CS16", "CU8"}, 8), "CF32,...");
    rc |= expect_cstr("device scope name", dsdneo::soapy_setting_scope_name(SoapySettingScope::Device), "device");
    rc |= expect_cstr("rx0 scope name", dsdneo::soapy_setting_scope_name(SoapySettingScope::Rx0), "rx0");
    rc |= expect_cstr("default scope name", dsdneo::soapy_setting_scope_name((SoapySettingScope)255), "device");
    return rc;
}

static int
test_settings_parser_accepts_device_and_rx_scopes(void) {
    int rc = 0;
    std::vector<dsdneo::SoapySettingRequest> settings;
    std::string error;
    if (!dsdneo::soapy_parse_settings("rfnotch_ctrl=true; rx:agc_setpoint=-30, rx0:rfgain_sel=4", &settings, &error)) {
        DSD_FPRINTF(stderr, "settings parser rejected valid input: %s\n", error.c_str());
        return 1;
    }
    if (settings.size() != 3U) {
        DSD_FPRINTF(stderr, "settings parser count mismatch: got %zu want 3\n", settings.size());
        return 1;
    }
    rc |= expect_scope("device setting scope", settings[0].scope, SoapySettingScope::Device);
    rc |= expect_string("device setting key", settings[0].key, "rfnotch_ctrl");
    rc |= expect_string("device setting value", settings[0].value, "true");
    rc |= expect_scope("rx setting scope", settings[1].scope, SoapySettingScope::Rx0);
    rc |= expect_string("rx setting key", settings[1].key, "agc_setpoint");
    rc |= expect_string("rx setting value", settings[1].value, "-30");
    rc |= expect_scope("rx0 setting scope", settings[2].scope, SoapySettingScope::Rx0);
    rc |= expect_string("rx0 setting key", settings[2].key, "rfgain_sel");
    rc |= expect_string("rx0 setting value", settings[2].value, "4");

    error = "stale";
    rc |= expect_true("blank settings parse without outputs", dsdneo::soapy_parse_settings("   ", nullptr, &error));
    rc |= expect_true("blank settings clears error", error.empty());
    return rc;
}

static int
expect_settings_parse_rejects(const char* label, const char* spec) {
    std::vector<dsdneo::SoapySettingRequest> settings;
    std::string error;
    if (dsdneo::soapy_parse_settings(spec, &settings, &error)) {
        DSD_FPRINTF(stderr, "%s: parser accepted invalid spec '%s'\n", label, spec);
        return 1;
    }
    if (error.empty()) {
        DSD_FPRINTF(stderr, "%s: parser rejected without an error\n", label);
        return 1;
    }
    return 0;
}

static int
test_settings_parser_rejects_invalid_items(void) {
    int rc = 0;
    rc |= expect_settings_parse_rejects("missing equals", "rfnotch_ctrl");
    rc |= expect_settings_parse_rejects("unknown scope", "tx:rfnotch_ctrl=true");
    rc |= expect_settings_parse_rejects("empty key", "=true");
    rc |= expect_settings_parse_rejects("empty rx key", "rx:=true");
    rc |= expect_settings_parse_rejects("empty value", "rfnotch_ctrl=");
    rc |= expect_settings_parse_rejects("empty separated item", "rfnotch_ctrl=true,,biasT_ctrl=false");
    return rc;
}

static int
expect_setting_value(const char* label, SoapySettingValueType type, const SoapyRange* range, const char* value,
                     bool want_valid) {
    std::string error;
    const bool got_valid = dsdneo::soapy_validate_setting_value(type, range, value, &error);
    if (got_valid != want_valid) {
        DSD_FPRINTF(stderr, "%s: got valid=%d want=%d error='%s'\n", label, got_valid ? 1 : 0, want_valid ? 1 : 0,
                    error.c_str());
        return 1;
    }
    if (!want_valid && error.empty()) {
        DSD_FPRINTF(stderr, "%s: invalid value produced no error\n", label);
        return 1;
    }
    return 0;
}

static int
test_setting_value_validation(void) {
    int rc = 0;
    const SoapyRange agc_range = {-60.0, 0.0, 1.0};
    const SoapyRange float_range = {0.0, 2.0, 0.0};

    rc |= expect_setting_value("bool true accepted", SoapySettingValueType::Bool, NULL, "true", true);
    rc |= expect_setting_value("bool false accepted", SoapySettingValueType::Bool, NULL, "false", true);
    rc |= expect_setting_value("bool typo rejected", SoapySettingValueType::Bool, NULL, "treu", false);
    rc |= expect_setting_value("bool numeric rejected", SoapySettingValueType::Bool, NULL, "1", false);

    rc |= expect_setting_value("int in range accepted", SoapySettingValueType::Int, &agc_range, "-30", true);
    rc |= expect_setting_value("int out of range rejected", SoapySettingValueType::Int, &agc_range, "-90", false);
    rc |= expect_setting_value("int decimal rejected", SoapySettingValueType::Int, &agc_range, "-30.5", false);
    rc |= expect_setting_value("int without range accepted", SoapySettingValueType::Int, NULL, "1234", true);

    rc |= expect_setting_value("float in range accepted", SoapySettingValueType::Float, &float_range, "1.25", true);
    rc |= expect_setting_value("float out of range rejected", SoapySettingValueType::Float, &float_range, "2.5", false);
    rc |= expect_setting_value("float nan rejected", SoapySettingValueType::Float, NULL, "nan", false);
    rc |= expect_setting_value("float suffix rejected", SoapySettingValueType::Float, NULL, "1.0dB", false);

    rc |= expect_setting_value("string accepted", SoapySettingValueType::String, NULL, "driver-specific", true);
    return rc;
}

/* A profile's preferred rate runs only where the device supports it exactly; else the nearest rate, as before. */
static int
test_profile_sample_rate(void) {
    int rc = 0;
    const dsdneo::SoapyProfile& hackrf = dsdneo::soapy_profile_by_id(SoapyProfileId::Hackrf);
    const dsdneo::SoapyProfile& generic = dsdneo::soapy_profile_by_id(SoapyProfileId::Generic);
    const std::vector<double> whole_mhz = {1e6, 2e6, 3e6, 8e6, 10e6, 20e6};
    rc |= expect_true("listed rate supported", dsdneo::soapy_rate_supported_exactly(10e6, whole_mhz, {}));
    rc |= expect_true("listed rate within 1 Hz", dsdneo::soapy_rate_supported_exactly(10e6 + 0.9, whole_mhz, {}));
    rc |= expect_true("unlisted rate unsupported", !dsdneo::soapy_rate_supported_exactly(9e6, whole_mhz, {}));
    rc |= expect_true("continuous range point", dsdneo::soapy_rate_supported_exactly(10e6, {}, {{2e6, 20e6, 0.0}}));
    rc |= expect_true("outside range", !dsdneo::soapy_rate_supported_exactly(10e6, {}, {{2e6, 8e6, 0.0}}));
    rc |= expect_true("stepped range point", dsdneo::soapy_rate_supported_exactly(11e6, {}, {{2e6, 20e6, 3e6}}));
    /* 10 MHz lies inside [2, 20] MHz but between its 3 MHz steps (8, 11). */
    rc |= expect_true("stepped range gap", !dsdneo::soapy_rate_supported_exactly(10e6, {}, {{2e6, 20e6, 3e6}}));
    rc |= expect_true("no rate is unsupported", !dsdneo::soapy_rate_supported_exactly(0.0, whole_mhz, {}));

    bool adjusted = false;
    rc |= expect_double("HackRF preferred rate listed",
                        dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, whole_mhz, {}, &adjusted), 10e6);
    rc |= expect_true("HackRF preferred rate adjusts the request", adjusted);
    rc |= expect_double("HackRF preferred rate in a range",
                        dsdneo::soapy_profile_sample_rate(hackrf, 1152000.0, {}, {{2e6, 20e6, 0.0}}, &adjusted), 10e6);
    rc |= expect_double("HackRF falls back off a stepped range",
                        dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {}, {{2e6, 20e6, 3e6}}, &adjusted), 2e6);
    rc |= expect_double("HackRF falls back when unlisted",
                        dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {1e6, 2e6, 8e6}, {}, &adjusted), 2e6);
    rc |= expect_double("generic keeps the nearest rate",
                        dsdneo::soapy_profile_sample_rate(generic, 1536000.0, whole_mhz, {}, &adjusted), 2e6);
    rc |= expect_double("HackRF preferred rate needs a request",
                        dsdneo::soapy_profile_sample_rate(hackrf, 0.0, whole_mhz, {}, &adjusted), 0.0);
    rc |= expect_true("no request is not adjusted", !adjusted);
    return rc;
}

/* The Fs/4 offset channel's floor, and the narrowest bandwidth a device admits at or above it. */
static int
test_offset_channel_floor(void) {
    int rc = 0;
    rc |= expect_double("floor at 10 MS/s", dsdneo::soapy_fs4_channel_floor_hz(10e6), 5072000.0);
    rc |= expect_double("floor at 2 MS/s", dsdneo::soapy_fs4_channel_floor_hz(2e6), 1072000.0);
    const std::vector<SoapyRange> points = {{1.75e6, 1.75e6, 0.0}, {5e6, 5e6, 0.0}, {5.5e6, 5.5e6, 0.0}};
    rc |= expect_double("smallest point at or above", dsdneo::soapy_floor_bandwidth(5072000.0, points), 5.5e6);
    rc |= expect_double("point at the floor", dsdneo::soapy_floor_bandwidth(5e6, points), 5e6);
    rc |= expect_double("point within 1 Hz of the floor", dsdneo::soapy_floor_bandwidth(5e6 + 0.5, points), 5e6);
    rc |= expect_double("widest when none reaches", dsdneo::soapy_floor_bandwidth(8e6, points), 5.5e6);
    rc |= expect_double("continuous range takes the floor",
                        dsdneo::soapy_floor_bandwidth(1072000.0, {{200000.0, 56e6, 0.0}}), 1072000.0);
    rc |= expect_double("range minimum above the floor",
                        dsdneo::soapy_floor_bandwidth(822000.0, {{1400100.0, 130e6, 0.0}}), 1400100.0);
    rc |= expect_double("stepped range rounds up",
                        dsdneo::soapy_floor_bandwidth(1072000.0, {{200000.0, 8e6, 250000.0}}), 1200000.0);
    rc |= expect_double("stepped range on a step",
                        dsdneo::soapy_floor_bandwidth(1200000.0, {{200000.0, 8e6, 250000.0}}), 1200000.0);
    /* None reaches 7.96 MHz: the widest step, 7.95 MHz (the 8 MHz maximum is not a step). */
    rc |= expect_double("stepped range past its last step",
                        dsdneo::soapy_floor_bandwidth(7.96e6, {{200000.0, 8e6, 250000.0}}), 7.95e6);
    rc |= expect_double("narrowest of several ranges",
                        dsdneo::soapy_floor_bandwidth(1072000.0, {{2e6, 4e6, 0.0}, {1.5e6, 1.5e6, 0.0}}), 1.5e6);
    rc |= expect_double("no ranges keep the floor", dsdneo::soapy_floor_bandwidth(1072000.0, {}), 1072000.0);

    rc |= expect_true("below the floor cuts", dsdneo::soapy_bandwidth_cuts_offset(5071999.0, 10e6));
    rc |= expect_true("at the floor keeps", !dsdneo::soapy_bandwidth_cuts_offset(5072000.0, 10e6));
    rc |= expect_true("above the floor keeps", !dsdneo::soapy_bandwidth_cuts_offset(5.5e6, 10e6));
    rc |= expect_true("no rate never cuts", !dsdneo::soapy_bandwidth_cuts_offset(1.0, 0.0));
    return rc;
}

/* @p got is @p want to within a micro-hertz: a driver that accepts only its listed values must get one exactly. */
static int
expect_point(const char* label, double got, double want) {
    /* Finite first: NaN compares false against any tolerance. This file is built with IEEE semantics. */
    if (!std::isfinite(got) || std::fabs(got - want) > 1e-6) {
        DSD_FPRINTF(stderr, "%s: got %.6f want %.6f\n", label, got, want);
        return 1;
    }
    return 0;
}

/* Every rate and bandwidth these helpers return is a value the device advertises: the listed or range point that
 * matched, never the preference or the floor itself. */
static int
test_returned_values_are_advertised(void) {
    int rc = 0;
    const dsdneo::SoapyProfile& hackrf = dsdneo::soapy_profile_by_id(SoapyProfileId::Hackrf);
    bool adjusted = false;
    rc |=
        expect_point("preference matched by a listed point",
                     dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {2e6, 9999999.0}, {}, &adjusted), 9999999.0);
    rc |= expect_point("preference matched by a stepped range point",
                       dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {}, {{0.25, 20e6 + 0.25, 1e6}}, &adjusted),
                       10000000.25);
    rc |= expect_point("widest step of a range no step of which reaches the floor",
                       dsdneo::soapy_floor_bandwidth(5072000.0, {{200000.0, 5e6, 250000.0}}), 4950000.0);
    rc |= expect_point("a point within 0.5 Hz below the floor",
                       dsdneo::soapy_floor_bandwidth(5000000.5, {{5e6, 5e6, 0.0}}), 5e6);
    rc |= expect_point("a continuous range's end within 0.5 Hz below the floor",
                       dsdneo::soapy_floor_bandwidth(5000000.5, {{200000.0, 5e6, 0.0}}), 5e6);
    rc |= expect_point("a discrete point within 0.5 Hz below the floor",
                       dsdneo::soapy_floor_bandwidth(1000000.5, {{1e6, 1e6, 0.0}, {2e6, 2e6, 0.0}}), 1e6);
    rc |= expect_point("a discrete point 1 Hz below the floor, which the warning flags",
                       dsdneo::soapy_floor_bandwidth(1000001.0, {{1e6, 1e6, 0.0}, {2e6, 2e6, 0.0}}), 2e6);
    bool supported = false;
    rc |= expect_point("nearest of a stepped range above its last step",
                       dsdneo::soapy_nearest_in_ranges(6e6, {{200000.0, 5.1e6, 250000.0}}, &supported), 4950000.0);
    rc |= expect_point("nearest rate of a stepped range above its last step",
                       dsdneo::soapy_nearest_sample_rate(6e6, {}, {{200000.0, 5.1e6, 250000.0}}, &adjusted), 4950000.0);
    rc |= expect_point("a stepped range's step within 0.5 Hz below the floor",
                       dsdneo::soapy_floor_bandwidth(1200000.5, {{200000.0, 8e6, 250000.0}}), 1200000.0);
    return rc;
}

/* A stepped range's last point is its last whole step at or below the maximum, and a rate either the list or a range
 * holds is the rate, not the nearest listed one. */
static int
test_points_stay_inside_the_advertised_ranges(void) {
    int rc = 0;
    bool adjusted = false;
    bool supported = false;
    rc |= expect_point("widest 1 Hz step of a range below the floor",
                       dsdneo::soapy_floor_bandwidth(5072000.0, {{200000.0, 5e6, 1.0}}), 5e6);
    rc |= expect_point("widest 1000 Hz step of a range whose maximum is not a step",
                       dsdneo::soapy_floor_bandwidth(1072000.0, {{200000.0, 999999.0, 1000.0}}), 999000.0);
    rc |= expect_point("nearest of a 1 Hz stepped range above its maximum",
                       dsdneo::soapy_nearest_in_ranges(6e6, {{200000.0, 5e6, 1.0}}, &supported), 5e6);
    const dsdneo::SoapyProfile& hackrf = dsdneo::soapy_profile_by_id(SoapyProfileId::Hackrf);
    rc |= expect_point("preference 1 Hz above a 1 Hz stepped range's maximum",
                       dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {}, {{2e6, 9999999.0, 1.0}}, &adjusted),
                       9999999.0);
    rc |= expect_point("a range rate the list omits",
                       dsdneo::soapy_nearest_sample_rate(10e6, {2e6, 8e6, 20e6}, {{2e6, 20e6, 0.0}}, &adjusted), 10e6);
    if (adjusted) {
        DSD_FPRINTF(stderr, "a range rate the list omits is reported as adjusted\n");
        rc = 1;
    }
    rc |= expect_point("a rate neither holds goes to the nearest listed",
                       dsdneo::soapy_nearest_sample_rate(21e6, {2e6, 8e6, 20e6}, {{2e6, 20e6, 0.0}}, &adjusted), 20e6);
    return rc;
}

/* The closest point wins, an exact one whatever the order, and the HackRF preference never takes a point that rounds
 * above it (10 MS/s is the capture ceiling too). */
static int
test_closest_point_wins(void) {
    int rc = 0;
    bool adjusted = false;
    const dsdneo::SoapyProfile& hackrf = dsdneo::soapy_profile_by_id(SoapyProfileId::Hackrf);
    rc |= expect_point("an exact listed preference after a near one",
                       dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {2e6, 10000001.0, 10e6}, {}, &adjusted),
                       10e6);
    rc |= expect_point(
        "an exact range preference after a near listed one",
        dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {2e6, 10000001.0}, {{2e6, 20e6, 0.0}}, &adjusted), 10e6);
    rc |= expect_point("only a near point above the preference: the nearest rate instead",
                       dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {2e6, 10000001.0}, {}, &adjusted), 2e6);
    rc |= expect_point("an exact listed rate after a near one",
                       dsdneo::soapy_nearest_sample_rate(8e6, {8000001.0, 8e6}, {}, &adjusted), 8e6);
    return rc;
}

/* expect_point() fails a value that is not a number: NaN is never within the tolerance. */
static int
test_expect_point_rejects_nan(void) {
    const uint64_t nan_bits = UINT64_C(0x7FF8000000000000);
    double nan_value = 0.0;
    DSD_MEMCPY(&nan_value, &nan_bits, sizeof nan_value);
    if (expect_point("a NaN rate (expected to fail)", nan_value, 10e6) == 0) {
        DSD_FPRINTF(stderr, "expect_point() passes NaN\n");
        return 1;
    }
    return 0;
}

/* Two advertised points equally close to the rate asked for: the one that rounds to it wins, so the rate written reads
 * back as the rate planned. A stepped range's lower step is considered too when the nearer one is out of bounds. */
static int
test_ties_and_lower_steps(void) {
    int rc = 0;
    bool adjusted = false;
    const dsdneo::SoapyProfile& hackrf = dsdneo::soapy_profile_by_id(SoapyProfileId::Hackrf);
    rc |= expect_point("the tied point that rounds to the rate asked for",
                       dsdneo::soapy_nearest_sample_rate(10e6, {10000000.5, 9999999.5}, {}, &adjusted), 9999999.5);
    rc |= expect_point(
        "the preference's lower step when the nearer one rounds above it",
        dsdneo::soapy_profile_sample_rate(hackrf, 1536000.0, {}, {{2000000.5, 20000000.5, 1.0}}, &adjusted), 9999999.5);
    return rc;
}

/* The filter chosen for the Fs/4 channel is one the warning accepts: at 2 MS/s on a 1 Hz grid, the 1,072,000 Hz floor
 * itself. */
static int
test_floor_filter_passes_the_warning(void) {
    int rc = 0;
    const double floor_hz = dsdneo::soapy_fs4_channel_floor_hz(2e6);
    const double chosen = dsdneo::soapy_floor_bandwidth(floor_hz, {{200000.0, 5e6, 1.0}});
    rc |= expect_point("a 1 Hz grid's filter at the floor", chosen, 1072000.0);
    if (dsdneo::soapy_bandwidth_cuts_offset(chosen, 2e6)) {
        DSD_FPRINTF(stderr, "the filter chosen for the channel is warned about as too narrow\n");
        rc = 1;
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_profile_selection();
    rc |= test_airspy_identity();
    rc |= test_antenna_selection();
    rc |= test_sddc_rate_grid();
    rc |= test_bandwidth_selection();
    rc |= test_stream_format_selection();
    rc |= test_cs16_full_scale();
    rc |= test_range_selection();
    rc |= test_profile_sample_rate();
    rc |= test_offset_channel_floor();
    rc |= test_returned_values_are_advertised();
    rc |= test_points_stay_inside_the_advertised_ranges();
    rc |= test_closest_point_wins();
    rc |= test_nearest_rate_within_limit();
    rc |= test_ties_and_lower_steps();
    rc |= test_expect_point_rejects_nan();
    rc |= test_floor_filter_passes_the_warning();
    rc |= test_join_and_scope_names();
    rc |= test_settings_parser_accepts_device_and_rx_scopes();
    rc |= test_settings_parser_rejects_invalid_items();
    rc |= test_setting_value_validation();
    return rc;
}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
