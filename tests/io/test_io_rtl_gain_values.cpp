// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The gain values the stream's open turns into tenths of a dB.
 *
 * - if_gain_text: one DSD_NEO_RTL_IF_GAINS stage's gain. An integer up to 90 in magnitude is whole dB, a larger one is
 *   already tenths, a decimal is dB, and an optional dB suffix is allowed. Text that is not a gain, or whose tenths do
 *   not fit an int (INT_MIN included, whose magnitude does not), is refused rather than read as 0 dB.
 * - if_gains_applied: the open skips a stage whose gain (or stage number) is refused, with a warning, and programs
 *   the rest, on a USB device over the fake dongle.
 * - manual_gain_tenths: a manual gain in whole dB past what int tenths hold asks for INT_MAX tenths (the tuner's top
 *   step), never a wrapped value.
 */

#include <climits>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <memory>
#include <string>
#include "rtl_stream_test_support.h"
#include "rtl_usb_identity.h"

extern "C" void rtl_device_test_set_open_fd_failure_hook(int (*hook)(int));

namespace {

int g_failures = 0;
std::string g_log;

void
check(bool ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

void
capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    g_log += (text != nullptr) ? text : "";
}

void
test_if_gain_text(void) {
    static const struct {
        const char* text;
        int want;
    } ok[] = {
        {"9", 90},
        {"9dB", 90},
        {"9db", 90},
        {"9.5", 95},
        {"9.5dB", 95},
        {"-9", -90},
        {"90", 900},
        {"-90", -900},
        {"91", 91},
        {"-91", -91},
        {"125", 125},
        {"0", 0},
        {"2147483647", INT_MAX},
        {"-2147483647", -INT_MAX},
        {"214748364.0", 2147483640},
        {"-214748364.0", -2147483640},
    };

    for (const auto& c : ok) {
        int tenths = 12345;
        const int rc = rtl_stream_test_parse_if_gain_tenth_db(c.text, &tenths);
        if (rc != 0 || tenths != c.want) {
            DSD_FPRINTF(stderr, "FAIL: IF gain '%s': rc %d tenths %d, want %d\n", c.text, rc, tenths, c.want);
            ++g_failures;
        }
    }

    static const char* const refused[] = {
        "abc",
        "",
        "dB",
        "9.5x",
        "9x",
        "-2147483648",
        "2147483648",
        "214748365.0",
        "-214748365.0",
        "1e300.5",
        "99999999999999999999.5",
        "inf.0",
        "nan.0",
    };
    for (const char* text : refused) {
        int tenths = 12345;
        const int rc = rtl_stream_test_parse_if_gain_tenth_db(text, &tenths);
        if (rc == 0) {
            DSD_FPRINTF(stderr, "FAIL: IF gain '%s' read as %d tenths, want refused\n", text, tenths);
            ++g_failures;
        }
    }
}

int
fake_open_fd(int fd) {
    (void)fd;
    return 1;
}

int
fake_fd_identity(int fd, rtl_usb_identity* out) {
    (void)fd;
    out->vid = 0x0bda;
    out->pid = 0x2838;
    DSD_SNPRINTF(out->manufacturer, sizeof out->manufacturer, "%s", "Realtek");
    DSD_SNPRINTF(out->product, sizeof out->product, "%s", "RTL2838UHIDIR");
    DSD_SNPRINTF(out->serial, sizeof out->serial, "%s", "00000001");
    return 0;
}

/* The gain the fake dongle holds for IF stage @p stage, or INT_MIN when that stage was never programmed. */
int
dongle_if_gain(const rtl_device_test_usb_dongle& d, int stage) {
    for (int i = 0; i < d.if_gain_count; ++i) {
        if (d.if_gains[i].stage == stage) {
            return d.if_gains[i].gain;
        }
    }
    return INT_MIN;
}

void
test_if_gains_applied(void) {
    auto ring = std::make_unique<input_ring_state>();
    check(input_ring_init(ring.get(), 1U << 16) == 0, "ring init");
    /* Heap-held: the device keeps its address in a global while it is installed. */
    auto dongle = std::make_unique<rtl_device_test_usb_dongle>();
    DSD_MEMSET(dongle.get(), 0, sizeof *dongle);
    dongle->tuner.tuner_type = 1; /* E4000, the tuner with IF stages */
    rtl_device_test_set_open_fd_failure_hook(fake_open_fd);
    rtl_device_test_set_fd_identity_hook(fake_fd_identity);
    rtl_device_test_usb_dongle_install(dongle.get());
    rtl_device_publish_preopened_fd(41, 0);
    rtl_device* dev = rtl_device_create(0, ring.get());
    check(dev != nullptr, "the fake dongle opens");
    if (dev != nullptr) {
        g_log.clear();
        rtl_stream_test_apply_if_gains(dev, "1:abc,2:9,3:-2147483648,4:9.5,5:-91,x:30,6:9dB");
        check(dongle_if_gain(*dongle, 1) == INT_MIN, "stage 1 ('abc') is skipped, not programmed at 0 dB");
        check(dongle_if_gain(*dongle, 2) == 90, "stage 2 takes 9 dB");
        check(dongle_if_gain(*dongle, 3) == INT_MIN, "stage 3 (INT_MIN) is skipped");
        check(dongle_if_gain(*dongle, 4) == 95, "stage 4 takes 9.5 dB");
        check(dongle_if_gain(*dongle, 5) == -91, "stage 5 takes -91 tenths");
        check(dongle_if_gain(*dongle, 0) == INT_MIN, "an unreadable stage number is skipped, not stage 0");
        check(dongle_if_gain(*dongle, 6) == 90, "stage 6 takes 9 dB");
        check(dongle->if_gain_count == 4, "four stages programmed");
        check(g_log.find("Ignoring IF gain 'abc' for stage 1") != std::string::npos, "stage 1 warning");
        check(g_log.find("Ignoring IF gain '-2147483648' for stage 3") != std::string::npos, "stage 3 warning");
        check(g_log.find("Ignoring IF gain stage 'x'") != std::string::npos, "stage number warning");
        if (g_failures != 0) {
            DSD_FPRINTF(stderr, "log:\n%s\n", g_log.c_str());
        }
        rtl_device_destroy(dev);
    }
    rtl_device_test_usb_dongle_install(nullptr);
    rtl_device_set_preopened_fd(-1);
    rtl_device_test_set_fd_identity_hook(nullptr);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    input_ring_destroy(ring.get());
}

void
test_manual_gain_tenths(void) {
    check(rtl_stream_test_gain_tenth_db_from_db(24) == 240, "24 dB is 240 tenths");
    check(rtl_stream_test_gain_tenth_db_from_db(49) == 490, "49 dB is 490 tenths");
    check(rtl_stream_test_gain_tenth_db_from_db(INT_MAX / 10) == (INT_MAX / 10) * 10, "the largest that fits");
    check(rtl_stream_test_gain_tenth_db_from_db(INT_MAX / 10 + 1) == INT_MAX, "one more holds at INT_MAX");
    check(rtl_stream_test_gain_tenth_db_from_db(INT_MAX) == INT_MAX, "INT_MAX dB holds at INT_MAX");
}

} // namespace

int
main(void) {
    dsd_neo_log_set_tap(capture_log, nullptr);
    test_if_gain_text();
    test_if_gains_applied();
    test_manual_gain_tenths();
    dsd_neo_log_set_tap(nullptr, nullptr);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "IO_RTL_GAIN_VALUES: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "IO_RTL_GAIN_VALUES: OK\n");
    return 0;
}
