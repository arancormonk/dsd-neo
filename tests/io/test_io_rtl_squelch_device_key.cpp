// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The device part of the auto squelch's floor context (issue #899 B12). In an rtl, rtltcp or Airspy spec the start
 * frequency, the squelch field and the volume set no noise, so a start that changes only those finds the floors learned
 * before; every other field (device index, endpoint, serial, gain, PPM, DSP bandwidth, bias) keeps them apart as
 * before, and any other spec (SoapySDR, an I/Q replay path, a sound device) counts whole, as it always has. An rtl
 * spec's serial counts in one form however it is spelled.
 */

#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

namespace {

/* FNV-1a of the whole string: the key every spec had before. */
uint32_t
whole_hash(const char* spec) {
    uint32_t h = 2166136261U;
    for (const char* c = spec; *c != '\0'; c++) {
        h = (h ^ (uint32_t)(unsigned char)*c) * 16777619U;
    }
    return h;
}

int
expect(int same, const char* a, const char* b) {
    const int got = rtl_stream_test_squelch_device_hash(a) == rtl_stream_test_squelch_device_hash(b);
    if (got != same) {
        DSD_FPRINTF(stderr, "IO_RTL_SQUELCH_DEVICE_KEY: \"%s\" and \"%s\" %s one key\n", a, b,
                    got ? "share" : "do not share");
        return 1;
    }
    return 0;
}

int
expect_whole(const char* spec) {
    if (rtl_stream_test_squelch_device_hash(spec) != whole_hash(spec)) {
        DSD_FPRINTF(stderr, "IO_RTL_SQUELCH_DEVICE_KEY: \"%s\" is not keyed whole\n", spec);
        return 1;
    }
    return 0;
}

} // namespace

int
main(void) {
    int rc = 0;
    /* The app's RTL-SDR spec: another start frequency, squelch field or volume trim, or none of the last two. */
    const char* rtl = "rtl:0:851.375M:30:0:48:0:2";
    rc |= expect(1, rtl, "rtl:0:162.475M:30:0:48:0:2");
    rc |= expect(1, rtl, "rtl:0:851.375M:30:0:48:noise+10:2");
    rc |= expect(1, rtl, "rtl:0:851.375M:30:0:48:auto+12:3");
    rc |= expect(1, rtl, "rtl:0:851.375M:30:0:48:-60:1");
    rc |= expect(1, rtl, "rtl:0:851.375M:30:0:48");
    rc |= expect(1, rtl, "rtl:0::30:0:48::");
    rc |= expect(1, "rtl:0:851.375M:30:0:48:0:2:bias=1", "rtl:0:160M:30:0:48:noise+10:4:bias=1");
    /* rtl_tcp: the same. */
    const char* tcp = "rtltcp:127.0.0.1:1234:851.375M:30:0:48:0:2";
    rc |= expect(1, tcp, "rtltcp:127.0.0.1:1234:852.0125M:30:0:48:noise+10:2");
    rc |= expect(1, tcp, "rtltcp:127.0.0.1:1234:154.43M:30:0:48:auto:5");
    rc |= expect(1, tcp, "rtltcp:127.0.0.1:1234:851.375M:30:0:48");
    /* Fields that set the noise, or name another receiver: apart, as before. */
    rc |= expect(0, rtl, "rtl:1:851.375M:30:0:48:0:2");
    rc |= expect(0, rtl, "rtl:0:851.375M:40:0:48:0:2");
    rc |= expect(0, rtl, "rtl:0:851.375M:30:5:48:0:2");
    rc |= expect(0, rtl, "rtl:0:851.375M:30:0:24:0:2");
    rc |= expect(0, rtl, "rtl:0:851.375M:30:0:48:0:2:bias=1");
    rc |= expect(0, rtl, "rtl::851.375M:30:0:48:0:2");
    rc |= expect(0, tcp, "rtltcp:127.0.0.1:1235:851.375M:30:0:48:0:2");
    rc |= expect(0, tcp, "rtltcp:192.168.0.2:1234:851.375M:30:0:48:0:2");
    rc |= expect(0, tcp, "rtltcp:127.0.0.1:1234:851.375M:20:0:48:0:2");
    rc |= expect(0, tcp, "rtltcp:127.0.0.1:1234:851.375M:30:0:48:0:2:bias=1");
    rc |= expect(0, tcp, "rtltcp:127.0.0.1:1234:851.375M:30:5:48:0:2");
    rc |= expect(0, tcp, "rtltcp:127.0.0.1:1234:851.375M:30:0:24:0:2");
    rc |= expect(0, rtl, "rtltcp:0:851.375M:30:0:48:0:2");
    /* A dongle named by serial: one key however the spec spells the serial (rtl:<sn> or rtl:serial=<sn>), another
       serial another key, and not the key of the index it may be found at. An index is keyed as written, as before,
       so floors learned under it stay. */
    const char* sn = "rtl:serial=00000001:851.375M:30:0:48:0:2";
    rc |= expect(1, sn, "rtl:00000001:851.375M:30:0:48:0:2");
    rc |= expect(1, sn, "rtl:00000001:162.475M:30:0:48:noise+10:3");
    rc |= expect(1, "rtl:serial=00000001", "rtl:00000001");
    rc |= expect(1, "rtl:serial=A1B2:851M:30:0:48:0:2:bias=1", "rtl:A1B2:160M:30:0:48:0:2:bias=1");
    rc |= expect(0, sn, "rtl:00000002:851.375M:30:0:48:0:2");
    rc |= expect(0, sn, "rtl:serial=00000002:851.375M:30:0:48:0:2");
    rc |= expect(0, sn, "rtl:1:851.375M:30:0:48:0:2");
    rc |= expect(0, sn, "rtl:00000001:851.375M:40:0:48:0:2");
    rc |= expect(0, "rtl:serial=1090", "rtl:1090");
    rc |= expect_whole("rtl:serial=00000001");
    rc |= expect_whole("rtl:1090");
    /* The bare forms, which name only the kind (and an rtltcp endpoint), are keyed as written. */
    rc |= expect_whole("rtl");
    rc |= expect_whole("rtltcp");
    rc |= expect_whole("rtltcp:127.0.0.1:1234");
    rc |= expect_whole("rtl:0");
    rc |= expect(0, "rtltcp:127.0.0.1:1234", "rtltcp:127.0.0.1:1235");
    /* Airspy (airspy[:serial=...][:freq[:bw[:sql[:vol]]]], the app's spec): the same, the serial and the bandwidth
       keeping floors apart. A stream opens on the parsed spec, the device alone, which is keyed as written. */
    const char* airspy = "airspy:162.475M:48:0:2";
    rc |= expect(1, airspy, "airspy:851.375M:48:noise+10:3");
    rc |= expect(1, airspy, "airspy:162.475M:48:auto+12");
    rc |= expect(1, airspy, "airspy:162.475M:48");
    rc |= expect(1, "airspy:serial=0x1234:162.475M:48:0:2", "airspy:serial=0x1234:851M:48:-60:1");
    rc |= expect(1, "airspy:serial=0x1234:162.475M::noise+10:3", "airspy:serial=0x1234");
    rc |= expect(0, airspy, "airspy:162.475M:24:0:2");
    rc |= expect(0, "airspy:serial=0x1234:162.475M:48:0:2", "airspy:serial=0x5678:162.475M:48:0:2");
    rc |= expect(0, airspy, "airspy:serial=0x1234:162.475M:48:0:2");
    /* Any other spec is keyed whole: a SoapySDR spec at another frequency is another key, an I/Q replay path or a
       device string is its whole text. */
    rc |= expect_whole("soapy:driver=rtlsdr:851.375M:30:0:48:0:2");
    rc |= expect(0, "soapy:driver=rtlsdr:851.375M:30:0:48:0:2", "soapy:driver=rtlsdr:852M:30:0:48:0:2");
    rc |= expect_whole("iqreplay:/captures/rtl:0:851.375M.cu8");
    rc |= expect_whole("airspy:serial=0x1234");
    rc |= expect_whole("rtlfoo:0:851.375M");
    rc |= expect_whole("pulse");
    rc |= expect_whole("");
    rc |= expect(1, "", NULL);
    if (rc == 0) {
        printf("IO_RTL_SQUELCH_DEVICE_KEY: OK\n");
    }
    return rc;
}
