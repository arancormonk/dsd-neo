// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * #782: libusb's own error and warning lines reach the runtime log through the USB log callback once each, at error or
 * warning level, starting with libusb's "libusb: " header and never with two of them. The diagnostics keep such a
 * line's error code (Task 1's template in diagnostics_log.cpp), so the text must reach the log as libusb wrote it.
 */
#include <cstdio>
#include <dsd-neo/runtime/log.h>
#include <string>
#include <utility>
#include <vector>
#include "rtl_usb_identity.h"

static int failures;
static std::vector<std::pair<dsd_neo_log_level_t, std::string>> records;

static void
check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        ++failures;
    }
}

static void
capture(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    records.emplace_back(level, text != nullptr ? text : "");
}

static std::pair<dsd_neo_log_level_t, std::string>
forward(int error, const char* text) {
    records.clear();
    rtl_usb_log_forward(error, text);
    check(records.size() == 1, "each libusb line is logged once");
    return records.empty() ? std::make_pair(LOG_LEVEL_DEBUG, std::string()) : records.front();
}

int
main() {
    dsd_neo_log_set_tap(capture, nullptr);

    /* libusb's own error line, as its log handler receives it: header, function, text, errno, line ending. */
    auto line = forward(1, "libusb: error [op_wrap_sys_device] failed to read descriptor, errno=19\n");
    check(line.first == LOG_LEVEL_ERROR, "an error is logged as an error");
    check(line.second == "libusb: error [op_wrap_sys_device] failed to read descriptor, errno=19\n",
          "the line reaches the log as libusb wrote it, with one header");

    line = forward(0, "libusb: warning [libusb_exit] device 1.2 still referenced\r\n");
    check(line.first == LOG_LEVEL_WARN, "a warning is logged as a warning");
    check(line.second == "libusb: warning [libusb_exit] device 1.2 still referenced\n", "one line ending");

    /* A line without libusb's header gets it, once. */
    line = forward(1, "error [usbi_claim] claim interface 0 failed -6");
    check(line.second == "libusb: error [usbi_claim] claim interface 0 failed -6\n", "the header is added, once");

    records.clear();
    rtl_usb_log_forward(1, "\n");
    rtl_usb_log_forward(1, nullptr);
    check(records.empty(), "an empty line logs nothing");

    /* Installing the callback is a no-op off Android, and harmless to repeat. */
    rtl_usb_log_install();
    rtl_usb_log_install();

    dsd_neo_log_set_tap(nullptr, nullptr);
    return failures ? 1 : 0;
}
