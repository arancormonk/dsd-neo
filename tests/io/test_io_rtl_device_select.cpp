// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Choosing an RTL-SDR by USB serial, over a fake librtlsdr enumeration (the real dongles are never touched).
 *
 * The lookup (rtl_device_find_serial()) and the listing (rtl_device_list()): a unique serial is found at its index; a
 * missing one is not; two readable devices with it are a duplicate, however far apart, an unreadable one never counts;
 * a device whose strings cannot be read (a failed read, or one that returns no string, or no serial) never matches,
 * and the listing shows none of another device's strings for it, the first device included; indexes are the
 * enumeration's own, looked up afresh each time.
 *
 * The open (rtl_device_create_selected(), over the fake dongle): the serial is looked up and the opened handle must
 * report it before anything configures it; a handle that reports another is closed as it is and the serial looked up
 * once more, then the open fails with RTL_DEVICE_ESERIAL. A pre-opened descriptor (Android) is never enumerated: its
 * dongle must report the serial, or the descriptor is handed back. A stream looks the serial up at every start, hands
 * the index it was found at to the caller's options, and its I/Q capture names the dongle by index and serial.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_device_select.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <errno.h>
#include <initializer_list>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "rtl_select_driver.h"
#include "rtl_stream_test_support.h"
#include "rtl_usb_identity.h"
#include "test_support.h"

extern "C" void rtl_device_test_set_open_fd_failure_hook(int (*hook)(int));
extern "C" void rtl_device_test_set_fd_reset_hook(int (*hook)(int));
extern "C" void rtl_device_test_set_fd_identity_hook(int (*hook)(int, struct rtl_usb_identity*));

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

/* ---- Fake librtlsdr enumeration ---- */

struct fake_dev {
    const char* manufacturer = "Realtek";
    const char* product = "RTL2838UHIDIR";
    const char* serial = "";
    int rc = 0; /* what its string read returns; a failed read scribbles first, as a partial read could */
};

constexpr int kMaxDevs = 64;

struct fake_world {
    fake_dev devs[kMaxDevs];
    int count = 0;
    int count_rc = 0;
    int count_calls = 0;
    /* What the opened handles report, call by call (the last repeats), and how many asked. */
    const char* handle_serials[4] = {};
    int handle_serial_count = 0;
    int handle_calls = 0;
    /* Called before the n-th (from 0) handle read: a test moves the devices there. */
    void (*before_handle_read)(int n) = nullptr;
};

fake_world g_world;

void
world_reset(void) {
    g_world = fake_world{};
}

void
put(char* dst, const char* src) {
    DSD_SNPRINTF(dst, 256, "%s", src);
}

int
fake_count(uint32_t* count) {
    g_world.count_calls++;
    *count = (uint32_t)g_world.count;
    return g_world.count_rc;
}

int
fake_index_strings(uint32_t index, char* m, char* p, char* s) {
    if ((int)index >= g_world.count) {
        return -1;
    }
    const fake_dev& d = g_world.devs[index];
    if (d.rc != 0) {
        put(m, "STALE");
        put(p, "STALE");
        put(s, "STALE");
        return d.rc;
    }
    put(m, d.manufacturer);
    put(p, d.product);
    put(s, d.serial);
    return 0;
}

int
fake_handle_strings(rtlsdr_dev_t* dev, char* m, char* p, char* s) {
    (void)dev;
    const int n = g_world.handle_calls++;
    if (g_world.before_handle_read != nullptr) {
        g_world.before_handle_read(n);
    }
    if (g_world.handle_serial_count == 0) {
        return -1;
    }
    const int k = n < g_world.handle_serial_count ? n : g_world.handle_serial_count - 1;
    put(m, "Realtek");
    put(p, "RTL2838UHIDIR");
    put(s, g_world.handle_serials[k]);
    return 0;
}

const rtl_select_driver_ops k_ops = {fake_count, fake_index_strings, fake_handle_strings};

/* The stall recovery's identity sees no device: its resolution never reaches the fake handles. */
uint32_t
no_usb_count(void) {
    return 0U;
}

int
no_usb_strings(uint32_t, char*, char*, char*) {
    return RTL_USB_OP_ERROR;
}

int
no_usb_handle_strings(rtlsdr_dev_t*, char*, char*, char*) {
    return RTL_USB_OP_ERROR;
}

int
no_usb_session(void**, int*) {
    return RTL_USB_OP_ERROR;
}

void
no_usb_close_session(void*) {}

int
no_usb_describe(void*, int, rtl_usb_device_info*) {
    return RTL_USB_OP_ERROR;
}

int
no_usb_open(void*, int, void**) {
    return RTL_USB_OP_ERROR;
}

void
no_usb_close(void*) {}

int
no_usb_read_strings(void*, char*, char*, char*) {
    return RTL_USB_OP_ERROR;
}

int
no_usb_reset(void*) {
    return RTL_USB_OP_ERROR;
}

uint64_t
no_usb_now(void) {
    return 0U;
}

void
no_usb_sleep(unsigned int) {}

const rtl_usb_driver_ops k_no_usb = {
    no_usb_count, no_usb_strings, no_usb_handle_strings, no_usb_session, no_usb_close_session, no_usb_describe,
    no_usb_open,  no_usb_close,   no_usb_read_strings,   no_usb_reset,   no_usb_now,           no_usb_sleep,
};

void
set_devs(std::initializer_list<const char*> serials) {
    world_reset();
    for (const char* sn : serials) {
        fake_dev& d = g_world.devs[g_world.count++];
        if (sn == nullptr) {
            d.rc = -1;
        } else {
            d.serial = sn;
        }
    }
}

rtl_device_find_result
find(const char* serial, rtl_device_find_report* rep) {
    DSD_MEMSET(rep, 0x5a, sizeof *rep);
    return rtl_device_find_serial(serial, rep);
}

/* ---- Lookup and listing ---- */

void
test_unique_and_not_found(void) {
    set_devs({"00000001", "00000002", "A1B2"});
    rtl_device_find_report rep;
    assert(find("00000002", &rep) == RTL_DEVICE_FIND_FOUND);
    assert(rep.index == 1 && rep.devices == 3 && rep.matches == 1 && rep.unreadable == 0);
    assert(find("A1B2", &rep) == RTL_DEVICE_FIND_FOUND && rep.index == 2);
    /* Exact: no prefix, suffix or case folding, and no octal or decimal reading of the digits. */
    assert(find("0000000", &rep) == RTL_DEVICE_FIND_NOT_FOUND);
    assert(find("1", &rep) == RTL_DEVICE_FIND_NOT_FOUND);
    assert(find("a1b2", &rep) == RTL_DEVICE_FIND_NOT_FOUND);
    assert(find("00000003", &rep) == RTL_DEVICE_FIND_NOT_FOUND);
    assert(rep.index == -1 && rep.matches == 0 && rep.devices == 3 && rep.unreadable == 0);
}

void
test_duplicate_readable_only(void) {
    set_devs({"00000001", "00000002", "00000001"});
    rtl_device_find_report rep;
    assert(find("00000001", &rep) == RTL_DEVICE_FIND_DUPLICATE);
    assert(rep.matches == 2 && rep.index == 0);
    /* A twin whose strings cannot be read is no duplicate: it never matches. */
    set_devs({"00000001", nullptr, "00000002"});
    g_world.devs[1].serial = "00000001";
    assert(find("00000001", &rep) == RTL_DEVICE_FIND_FOUND);
    assert(rep.index == 0 && rep.matches == 1 && rep.unreadable == 1);
}

void
test_unreadable_never_matches_and_lists_blank(void) {
    /* The first device's read fails (the buffers would hold whatever it left), the second returns 0 with no string at
       all, the third has strings but no serial. */
    set_devs({nullptr, "", ""});
    g_world.devs[1].manufacturer = "";
    g_world.devs[1].product = "";
    g_world.devs[3].serial = "00000001";
    g_world.count = 4;
    rtl_device_find_report rep;
    assert(find("", &rep) == RTL_DEVICE_FIND_FAILED);
    assert(find("STALE", &rep) == RTL_DEVICE_FIND_NOT_FOUND);
    assert(rep.unreadable == 3 && rep.devices == 4);
    assert(find("00000001", &rep) == RTL_DEVICE_FIND_FOUND && rep.index == 3 && rep.unreadable == 3);

    rtl_device_entry list[4];
    DSD_MEMSET(list, 'x', sizeof list);
    int total = -1;
    assert(rtl_device_list(list, 4, &total) == 4 && total == 4);
    for (int i = 0; i < 2; i++) {
        assert(list[i].index == i && !list[i].strings_read && !list[i].readable);
        assert(list[i].manufacturer[0] == '\0' && list[i].product[0] == '\0' && list[i].serial[0] == '\0');
    }
    assert(list[2].strings_read && !list[2].readable && strcmp(list[2].product, "RTL2838UHIDIR") == 0);
    assert(list[3].strings_read && list[3].readable && strcmp(list[3].serial, "00000001") == 0);
    char line[1024];
    rtl_device_entry_describe(&list[0], line, sizeof line);
    assert(strcmp(line, RTL_DEVICE_UNREADABLE_TEXT) == 0);
    rtl_device_entry_describe(&list[1], line, sizeof line);
    assert(strcmp(line, RTL_DEVICE_UNREADABLE_TEXT) == 0);
    rtl_device_entry_describe(&list[2], line, sizeof line);
    assert(strcmp(line, "Realtek, RTL2838UHIDIR, SN: (none)") == 0);
    rtl_device_entry_describe(&list[3], line, sizeof line);
    assert(strcmp(line, "Realtek, RTL2838UHIDIR, SN: 00000001") == 0);
}

void
test_two_busy_then_unique(void) {
    /* Windows: two dongles already streaming in other processes, the requested one at index 2. */
    set_devs({nullptr, nullptr, "00000003"});
    rtl_device_find_report rep;
    assert(find("00000003", &rep) == RTL_DEVICE_FIND_FOUND);
    assert(rep.index == 2 && rep.unreadable == 2 && rep.matches == 1 && rep.devices == 3);
}

void
test_reorder_between_finds(void) {
    set_devs({"00000001", "00000002", "00000003"});
    rtl_device_find_report rep;
    assert(find("00000003", &rep) == RTL_DEVICE_FIND_FOUND && rep.index == 2);
    set_devs({"00000003", "00000001", "00000002"});
    assert(find("00000003", &rep) == RTL_DEVICE_FIND_FOUND && rep.index == 0);
}

void
test_duplicate_beyond_capacity(void) {
    world_reset();
    g_world.count = 40;
    static char serials[40][8];
    for (int i = 0; i < 40; i++) {
        DSD_SNPRINTF(serials[i], sizeof serials[i], "S%02d", i);
        g_world.devs[i].serial = serials[i];
    }
    g_world.devs[3].serial = "TWIN";
    g_world.devs[37].serial = "TWIN";
    rtl_device_entry list[4];
    int total = 0;
    assert(rtl_device_list(list, 4, &total) == 4 && total == 40);
    assert(list[3].readable && strcmp(list[3].serial, "TWIN") == 0);
    rtl_device_find_report rep;
    assert(find("TWIN", &rep) == RTL_DEVICE_FIND_DUPLICATE && rep.matches == 2 && rep.devices == 40);
    assert(find("S39", &rep) == RTL_DEVICE_FIND_FOUND && rep.index == 39);
    /* Counting only. */
    assert(rtl_device_list(nullptr, 0, &total) == 0 && total == 40);
}

void
test_no_devices_and_failed_enumeration(void) {
    set_devs({});
    rtl_device_find_report rep;
    int total = -1;
    assert(find("00000001", &rep) == RTL_DEVICE_FIND_NO_DEVICES && rep.devices == 0);
    assert(rtl_device_list(nullptr, 0, &total) == 0 && total == 0);
    set_devs({"00000001"});
    g_world.count_rc = -1;
    assert(find("00000001", &rep) == RTL_DEVICE_FIND_FAILED && rep.index == -1);
    assert(rtl_device_list(nullptr, 0, &total) == -1 && total == 0);
}

/* ---- --rtl-list lines ---- */

rtl_device_entry
entry_with(int index, const char* serial) {
    rtl_device_entry e{};
    e.index = index;
    e.strings_read = 1;
    e.readable = serial[0] != '\0';
    put(e.manufacturer, "Realtek");
    put(e.product, "RTL2838UHIDIR");
    put(e.serial, serial);
    return e;
}

void
expect_line(const rtl_device_entry& e, int shared, const char* want) {
    char line[RTL_DEVICE_LINE_SIZE];
    rtl_device_list_line(&e, shared, line, sizeof line);
    if (strcmp(line, want) != 0) {
        DSD_FPRINTF(stderr, "line \"%s\", want \"%s\"\n", line, want);
    }
    assert(strcmp(line, want) == 0);
}

void
test_list_lines(void) {
    /* A serial made of letters, digits, '.', '_' and '-': the -i argument to paste, in the form that reads back as it. */
    expect_line(entry_with(0, "00000001"), 0, "RTL-SDR 0: Realtek, RTL2838UHIDIR, SN: 00000001  (-i rtl:00000001)");
    expect_line(entry_with(1, "1090"), 0, "RTL-SDR 1: Realtek, RTL2838UHIDIR, SN: 1090  (-i rtl:serial=1090)");
    expect_line(entry_with(2, "North_2.v-1"), 0,
                "RTL-SDR 2: Realtek, RTL2838UHIDIR, SN: North_2.v-1  (-i rtl:North_2.v-1)");
    /* Anything else would split or expand in cmd or sh: a quoted literal, a note, and nothing to paste. */
    expect_line(entry_with(3, "1 East"), 0,
                "RTL-SDR 3: Realtek, RTL2838UHIDIR, SN: \"1 East\"  [spaces or shell characters: quote the whole -i "
                "argument]");
    expect_line(entry_with(4, "A&B|$(x)"), 0,
                "RTL-SDR 4: Realtek, RTL2838UHIDIR, SN: \"A&B|$(x)\"  [spaces or shell characters: quote the whole -i "
                "argument]");
    expect_line(entry_with(5, "A:B"), 0,
                "RTL-SDR 5: Realtek, RTL2838UHIDIR, SN: \"A:B\"  [this serial cannot be given in -i rtl:]");
    expect_line(entry_with(6, "00000001"), 1,
                "RTL-SDR 6: Realtek, RTL2838UHIDIR, SN: 00000001  [duplicate serial: cannot be chosen by serial]");
    rtl_device_entry none = entry_with(7, "");
    expect_line(none, 0, "RTL-SDR 7: Realtek, RTL2838UHIDIR, SN: (none)");
    rtl_device_entry unread{};
    unread.index = 8;
    expect_line(unread, 0, "RTL-SDR 8: " RTL_DEVICE_UNREADABLE_TEXT);

    /* What a device reports is shown escaped, never written to the terminal as it is: a newline, ESC or any byte past
       printable ASCII as \xNN, a backslash doubled (and '"' escaped inside quotes), in the line that offers an -i
       argument and in the quoted one alike, and in the startup listing's text. The strings themselves stay. */
    rtl_device_entry ctl = entry_with(9, "00000009");
    put(ctl.manufacturer, "Real\ntek");
    put(ctl.product, "RTL\x1b[31m\xff");
    expect_line(ctl, 0, "RTL-SDR 9: Real\\x0Atek, RTL\\x1B[31m\\xFF, SN: 00000009  (-i rtl:00000009)");
    assert(strcmp(ctl.manufacturer, "Real\ntek") == 0);
    rtl_device_entry quoted = entry_with(10, "1 East\\x");
    put(quoted.manufacturer, "\x1b]0;title\a");
    put(quoted.product, "two\nlines");
    expect_line(
        quoted, 0,
        "RTL-SDR 10: \\x1B]0;title\\x07, two\\x0Alines, SN: \"1 East\\\\x\"  [spaces or shell characters: quote "
        "the whole -i argument]");
    expect_line(entry_with(11, "A\nB\x1b"), 0,
                "RTL-SDR 11: Realtek, RTL2838UHIDIR, SN: \"A\\x0AB\\x1B\"  [this serial cannot be given in -i rtl:]");
    char text[RTL_DEVICE_LINE_SIZE];
    rtl_device_entry_describe(&quoted, text, sizeof text);
    assert(strcmp(text, "\\x1B]0;title\\x07, two\\x0Alines, SN: 1 East\\\\x") == 0);
    /* The longest strings, every byte escaped, fit the line. */
    rtl_device_entry wide = entry_with(12, "");
    DSD_MEMSET(wide.manufacturer, 0x1b, RTL_DEVICE_STRING_SIZE - 1);
    DSD_MEMSET(wide.product, 0x1b, RTL_DEVICE_STRING_SIZE - 1);
    DSD_MEMSET(wide.serial, 0x1b, RTL_DEVICE_STRING_SIZE - 1);
    wide.readable = 1;
    char line[RTL_DEVICE_LINE_SIZE];
    rtl_device_list_line(&wide, 0, line, sizeof line);
    assert(strstr(line, "[this serial cannot be given in -i rtl:]") != nullptr);
}

/* ---- Opening by serial ---- */

const int k_gains[] = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};

struct rig {
    std::unique_ptr<input_ring_state> ring = std::make_unique<input_ring_state>();
    std::unique_ptr<rtl_device_test_usb_dongle> dongle = std::make_unique<rtl_device_test_usb_dongle>();

    rig() {
        assert(input_ring_init(ring.get(), 1U << 16) == 0);
        DSD_MEMSET(dongle.get(), 0, sizeof *dongle);
        dongle->tuner.tuner_type = 1;
        dongle->gain.gain_count = (int)(sizeof k_gains / sizeof k_gains[0]);
        DSD_MEMCPY(dongle->gain.gains, k_gains, sizeof k_gains);
        rtl_device_test_usb_dongle_install(dongle.get());
    }

    ~rig() {
        rtl_device_test_usb_dongle_install(nullptr);
        input_ring_destroy(ring.get());
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;
};

/* Nothing but the opens and closes reached the dongle: no configuration, bias tee or FIFO reset. */
bool
only_opens_and_closes(const rtl_device_test_usb_dongle& d) {
    for (int op = RTL_TEST_DONGLE_XTAL; op < RTL_TEST_DONGLE_OP_COUNT; op++) {
        if (d.calls[op] != 0) {
            DSD_FPRINTF(stderr, "op %d reached the dongle %d time(s)\n", op, d.calls[op]);
            return false;
        }
    }
    return d.stale_handle_calls == 0;
}

bool
reported_serial_failure(void) {
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    return rtl_device_last_open_error() == RTL_DEVICE_ESERIAL && failure.kind == DSD_INPUT_FAILURE_DEVICE
           && failure.native_code == RTL_DEVICE_ESERIAL;
}

void
test_open_verified_serial(void) {
    set_devs({"00000001", "00000002"});
    g_world.handle_serials[0] = "00000002";
    g_world.handle_serial_count = 1;
    rig r;
    int index = -1;
    rtl_device* dev = rtl_device_create_selected(0, "00000002", r.ring.get(), &index);
    assert(dev != nullptr && index == 1);
    assert(r.dongle->calls[RTL_TEST_DONGLE_OPEN] == 1 && r.dongle->open_handles == 1);
    char sn[64];
    assert(rtl_device_usb_serial(dev, sn, sizeof sn) == 0 && strcmp(sn, "00000002") == 0);
    assert(rtl_device_usb_serial(dev, sn, 8) == -1 && sn[0] == '\0');
    rtl_device_destroy(dev);
    assert(r.dongle->open_handles == 0 && r.dongle->stale_handle_calls == 0);
}

/* The dongles move between the lookup and the open: the first handle reports another serial, the second lookup finds
   the serial elsewhere and that handle reports it. */
void
move_after_first_read(int n) {
    if (n == 0) {
        g_world.devs[0].serial = "00000002";
        g_world.devs[1].serial = "00000001";
    }
}

void
test_open_mismatch_then_retry(void) {
    set_devs({"00000001", "00000002"});
    g_world.handle_serials[0] = "00000001";
    g_world.handle_serials[1] = "00000002";
    g_world.handle_serial_count = 2;
    g_world.before_handle_read = move_after_first_read;
    rig r;
    int index = -1;
    rtl_device* dev = rtl_device_create_selected(0, "00000002", r.ring.get(), &index);
    assert(dev != nullptr && index == 0);
    assert(g_world.handle_calls == 2);
    /* The first handle was closed as it was: two opens, one close, nothing configured. */
    assert(r.dongle->calls[RTL_TEST_DONGLE_OPEN] == 2 && r.dongle->calls[RTL_TEST_DONGLE_CLOSE] == 1);
    assert(r.dongle->open_handles == 1);
    assert(only_opens_and_closes(*r.dongle));
    rtl_device_destroy(dev);
    assert(r.dongle->open_handles == 0);
}

void
test_open_mismatch_twice_fails(void) {
    set_devs({"00000001", "00000002"});
    g_world.handle_serials[0] = "00000001";
    g_world.handle_serial_count = 1;
    rig r;
    dsd_input_failure_clear();
    int index = 7;
    rtl_device* dev = rtl_device_create_selected(0, "00000002", r.ring.get(), &index);
    assert(dev == nullptr && index == 7);
    assert(reported_serial_failure());
    /* One bounded retry: two opens, both closed as they were, none configured. */
    assert(r.dongle->calls[RTL_TEST_DONGLE_OPEN] == 2 && r.dongle->calls[RTL_TEST_DONGLE_CLOSE] == 2);
    assert(r.dongle->open_handles == 0);
    assert(only_opens_and_closes(*r.dongle));
    /* A handle whose strings cannot be read is no match either. */
    g_world.handle_serial_count = 0;
    g_world.handle_calls = 0;
    assert(rtl_device_create_selected(0, "00000002", r.ring.get(), nullptr) == nullptr);
    assert(r.dongle->open_handles == 0 && reported_serial_failure());
    dsd_input_failure_clear();
}

void
test_open_not_found_or_duplicate_opens_nothing(void) {
    for (int dup = 0; dup < 2; dup++) {
        set_devs({"00000001", dup ? "00000001" : "00000002", nullptr});
        g_world.handle_serials[0] = "00000001";
        g_world.handle_serial_count = 1;
        rig r;
        const char* want = dup ? "00000001" : "00000009";
        assert(rtl_device_create_selected(0, want, r.ring.get(), nullptr) == nullptr);
        assert(reported_serial_failure());
        assert(r.dongle->calls[RTL_TEST_DONGLE_OPEN] == 0 && g_world.handle_calls == 0);
        dsd_input_failure_clear();
    }
}

void
test_open_by_index_unchanged(void) {
    set_devs({"00000001"});
    rig r;
    int index = -1;
    rtl_device* dev = rtl_device_create_selected(0, "", r.ring.get(), &index);
    assert(dev != nullptr && index == 0);
    /* No lookup, and the handle is not asked for a serial. */
    assert(g_world.count_calls == 0 && g_world.handle_calls == 0);
    char sn[64];
    assert(rtl_device_usb_serial(dev, sn, sizeof sn) == -1);
    rtl_device_destroy(dev);
    dev = rtl_device_create(0, r.ring.get());
    assert(dev != nullptr && g_world.count_calls == 0);
    rtl_device_destroy(dev);
}

/* ---- A pre-opened descriptor (Android) ---- */

/* rtlsdr_open_fd() on the descriptor: counted, then the fake dongle opens as the device behind it. */
int g_open_fd_calls = 0;

int
open_fd_opens_dongle(int fd) {
    (void)fd;
    g_open_fd_calls++;
    return 1;
}

/* The descriptor's USB reset and identity (the strings of the dongle behind it, readable before and after the open). */
int g_fd_resets = 0;
const char* g_fd_serial = "";
int g_fd_identity_rc = 0;

int
fd_reset_counts(int fd) {
    (void)fd;
    g_fd_resets++;
    return 0;
}

int
fd_identity(int fd, rtl_usb_identity* out) {
    (void)fd;
    if (g_fd_identity_rc != 0) {
        return g_fd_identity_rc;
    }
    out->vid = 0x0bda;
    out->pid = 0x2838;
    put(out->manufacturer, "Realtek");
    put(out->product, "RTL2838UHIDIR");
    put(out->serial, g_fd_serial);
    return 0;
}

/* With a serial, the dongle behind the descriptor must report it through the descriptor before anything opens it
   (rtlsdr_open_fd() claims, writes registers, can reset and inits the tuner): another dongle, or one whose serial
   cannot be read that way, is refused with no open at all. The handle check after the open stays. Nothing is
   enumerated: USB discovery is off in this mode. */
void
test_preopened_serial_checked_on_its_dongle(void) {
    rtl_device_test_set_open_fd_failure_hook(open_fd_opens_dongle);
    rtl_device_test_set_fd_identity_hook(fd_identity);

    struct {
        const char* fd_serial; /* through the descriptor, nullptr: the read fails */
        const char* handle_serial;
        int opens;
    } const cases[] = {
        {"00000001", "00000001", 0},
        {nullptr, "00000002", 0},
        {"00000002", "00000001", 1},
        {"00000002", "00000002", 1},
    };

    for (const auto& c : cases) {
        set_devs({});
        g_world.handle_serials[0] = c.handle_serial;
        g_world.handle_serial_count = 1;
        g_fd_serial = c.fd_serial != nullptr ? c.fd_serial : "";
        g_fd_identity_rc = c.fd_serial != nullptr ? 0 : -EIO;
        g_open_fd_calls = 0;
        rig r;
        dsd_input_failure_clear();
        rtl_device_set_preopened_fd(9);
        int index = -1;
        rtl_device* dev = rtl_device_create_selected(0, "00000002", r.ring.get(), &index);
        assert(g_world.count_calls == 0);
        assert(g_open_fd_calls == c.opens && r.dongle->calls[RTL_TEST_DONGLE_OPEN] == c.opens);
        const bool match = c.fd_serial != nullptr && strcmp(c.handle_serial, "00000002") == 0;
        if (match) {
            assert(dev != nullptr && index == 0 && rtl_device_preopened_fd_held() == 9);
            char sn[64];
            assert(rtl_device_usb_serial(dev, sn, sizeof sn) == 0 && strcmp(sn, "00000002") == 0);
            rtl_device_destroy(dev);
        } else {
            assert(dev == nullptr && reported_serial_failure());
            /* The descriptor is handed back; a dongle opened is closed as it was. */
            assert(rtl_device_preopened_fd_in_use() == 0 && rtl_device_preopened_fd_held() == -1);
            assert(r.dongle->calls[RTL_TEST_DONGLE_CLOSE] == c.opens && r.dongle->open_handles == 0);
            assert(g_world.handle_calls == c.opens);
            assert(only_opens_and_closes(*r.dongle));
        }
        assert(rtl_device_preopened_fd_in_use() == 0);
    }
    g_fd_identity_rc = 0;
    /* Without a serial the descriptor's dongle is taken as it is. */
    set_devs({});
    {
        rig r;
        rtl_device_set_preopened_fd(9);
        rtl_device* dev = rtl_device_create_selected(0, nullptr, r.ring.get(), nullptr);
        assert(dev != nullptr && g_world.handle_calls == 0 && g_world.count_calls == 0);
        rtl_device_destroy(dev);
    }
    rtl_device_set_preopened_fd(-1);
    rtl_device_test_set_fd_identity_hook(nullptr);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    dsd_input_failure_clear();
}

/* ---- Through a stream ---- */

/* One stream start on @p spec with the caller's options holding @p cached_serial / @p cached_index from before (what
   another spec selected), recording an I/Q capture into @p dir: whether it started, the caller's options after it in
   *@p caller, and the capture's source_args. */
int g_stream_opens = 0; /* the dongle's opens in the last stream_start() */

bool
stream_start(const char* dir, const char* spec, const char* cached_serial, int cached_index, dsd_opts* caller,
             std::string* source_args) {
    DSD_MEMSET(caller, 0, sizeof *caller);
    caller->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(caller->audio_in_dev, sizeof caller->audio_in_dev, "%s", spec);
    DSD_SNPRINTF(caller->rtl_dev_serial, sizeof caller->rtl_dev_serial, "%s", cached_serial);
    caller->rtl_dev_index = cached_index;
    caller->rtlsdr_center_freq = 851012500U;
    caller->rtl_dsp_bw_khz = 48;
    caller->rtl_gain_value = 24;
    caller->frame_dmr = 1;
    char data_path[DSD_TEST_PATH_MAX];
    char meta_path[DSD_TEST_PATH_MAX];
    assert(dsd_test_path_join(data_path, sizeof data_path, dir, "capture.iq") == 0);
    assert(dsd_test_path_join(meta_path, sizeof meta_path, dir, "capture.iq.json") == 0);
    caller->iq_capture_requested = 1;
    caller->iq_capture_format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(caller->iq_capture_path, sizeof caller->iq_capture_path, "%s", data_path);

    rig r;
    rtl_device_test_usb_stream_attach();
    RtlSdrContext* ctx = nullptr;
    assert(rtl_stream_create(caller, &ctx) == 0 && ctx != nullptr);
    const bool started = rtl_stream_start(ctx) == 0;
    (void)rtl_stream_stop(ctx);
    (void)rtl_stream_destroy(ctx);
    rtl_device_test_usb_stream_detach();
    assert(r.dongle->open_handles == 0);
    g_stream_opens = r.dongle->calls[RTL_TEST_DONGLE_OPEN];

    source_args->clear();
    if (started) {
        dsd_iq_replay_config cfg{};
        char err[256];
        assert(dsd_iq_replay_read_metadata(meta_path, &cfg, err, sizeof err) == DSD_IQ_OK);
        *source_args = cfg.source_args;
        dsd_iq_replay_config_clear(&cfg);
    }
    for (const char* leaf : {"capture.iq", "capture.iq.json"}) {
        char path[DSD_TEST_PATH_MAX];
        assert(dsd_test_path_join(path, sizeof path, dir, leaf) == 0);
        (void)remove(path);
    }
    return started;
}

/* One stream start on the dongle with serial 00000002: the index it was found at reaches the caller's options, and the
   capture's sidecar names the dongle by index and serial. */
int
stream_start_by_serial(const char* dir, std::string* source_args) {
    auto caller = std::make_unique<dsd_opts>();
    assert(stream_start(dir, "rtl:00000002:851.0125M", "00000002", 0, caller.get(), source_args));
    return caller->rtl_dev_index;
}

void
test_stream_looks_the_serial_up_at_every_start(void) {
    char dir[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_rtl_select") != nullptr);
    set_devs({"00000001", "00000002"});
    g_world.handle_serials[0] = "00000002";
    g_world.handle_serial_count = 1;
    std::string args;
    assert(stream_start_by_serial(dir, &args) == 1);
    assert(args == "index=1,serial=00000002");
    /* Replugged in another order: the next start finds it at index 0. */
    set_devs({"00000002", "00000001"});
    g_world.handle_serials[0] = "00000002";
    g_world.handle_serial_count = 1;
    assert(stream_start_by_serial(dir, &args) == 0);
    assert(args == "index=0,serial=00000002");
    assert(dsd_test_rmdir(dir) == 0);
}

/* The spec is the one record of the dongle a start opens: options a former spec left (a cached serial or index) never
   open another dongle. A restart onto rtl:serial=00000002 with 00000001 still cached opens 00000002 and hands that
   back; an index spec clears a cached serial without any lookup; a device field the grammar refuses opens nothing. */
void
test_stream_selects_from_its_spec(void) {
    char dir[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_rtl_spec") != nullptr);
    auto caller = std::make_unique<dsd_opts>();
    std::string args;
    set_devs({"00000001", "00000002"});
    g_world.handle_serials[0] = "00000002";
    g_world.handle_serial_count = 1;
    assert(stream_start(dir, "rtl:serial=00000002:851.0125M", "00000001", 0, caller.get(), &args));
    assert(caller->rtl_dev_index == 1 && strcmp(caller->rtl_dev_serial, "00000002") == 0);
    assert(args == "index=1,serial=00000002");

    set_devs({"00000001", "00000002"});
    assert(stream_start(dir, "rtl:1:851.0125M", "00000001", 0, caller.get(), &args));
    assert(g_world.count_calls == 0 && g_world.handle_calls == 0);
    assert(caller->rtl_dev_index == 1 && caller->rtl_dev_serial[0] == '\0');

    set_devs({"00000001", "00000002"});
    assert(!stream_start(dir, "rtl:serial=:851.0125M", "00000001", 0, caller.get(), &args));
    assert(g_world.count_calls == 0 && g_world.handle_calls == 0 && g_stream_opens == 0);
    assert(strcmp(caller->rtl_dev_serial, "00000001") == 0);
    assert(dsd_test_rmdir(dir) == 0);
}

/* A live restart on an Android descriptor (the device row, a config reopen) that names an index: the descriptor is the
   one device there is, so it runs as 0 and the caller's options, its spec and the capture all say 0. */
void
test_stream_descriptor_index_is_zero(void) {
    char dir[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(dir, sizeof dir, "dsdneo_rtl_fd") != nullptr);
    rtl_device_test_set_open_fd_failure_hook(open_fd_opens_dongle);
    rtl_device_test_set_fd_identity_hook(fd_identity);
    g_fd_serial = "00000007";
    g_fd_identity_rc = 0;
    set_devs({});
    rtl_device_set_preopened_fd(9);
    auto caller = std::make_unique<dsd_opts>();
    std::string args;
    assert(stream_start(dir, "rtl:5:851.0125M", "", 5, caller.get(), &args));
    assert(caller->rtl_dev_index == 0 && strcmp(caller->audio_in_dev, "rtl:0:851.0125M") == 0);
    assert(args == "index=0,serial=00000007");
    assert(g_world.count_calls == 0);
    rtl_device_set_preopened_fd(-1);
    rtl_device_test_set_fd_identity_hook(nullptr);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    assert(dsd_test_rmdir(dir) == 0);
}

/* A descriptor whose dongle owes a USB reset (#782): with a serial, the reset is paid only on the dongle with that
   serial, read through the descriptor first. Another dongle, or one whose serial cannot be read that way, is refused
   untouched and keeps its obligation. */
void
test_preopened_owed_reset_paid_only_on_the_requested_dongle(void) {
    rtl_device_test_set_open_fd_failure_hook(open_fd_opens_dongle);
    rtl_device_test_set_fd_reset_hook(fd_reset_counts);
    rtl_device_test_set_fd_identity_hook(fd_identity);
    set_devs({});
    {
        rig r;
        g_fd_serial = "00000001";
        g_fd_identity_rc = 0;
        g_fd_resets = 0;
        g_world.handle_serials[0] = "00000001";
        g_world.handle_serial_count = 1;
        dsd_input_failure_clear();
        rtl_device_publish_preopened_fd(9, 1);
        assert(rtl_device_create_selected(0, "00000002", r.ring.get(), nullptr) == nullptr);
        assert(g_fd_resets == 0 && rtl_device_preopened_fd_reset_owed() == 9);
        assert(r.dongle->calls[RTL_TEST_DONGLE_OPEN] == 0 && rtl_device_preopened_fd_held() == -1);
        assert(reported_serial_failure());
    }
    {
        rig r;
        g_fd_serial = "00000002";
        g_world.handle_serials[0] = "00000002";
        rtl_device_publish_preopened_fd(9, 1);
        rtl_device* dev = rtl_device_create_selected(0, "00000002", r.ring.get(), nullptr);
        assert(dev != nullptr && g_fd_resets == 1 && rtl_device_preopened_fd_reset_owed() == -1);
        rtl_device_destroy(dev);
    }
    {
        rig r;
        g_fd_identity_rc = -EIO;
        g_fd_resets = 0;
        rtl_device_publish_preopened_fd(9, 1);
        g_open_fd_calls = 0;
        assert(rtl_device_create_selected(0, "00000002", r.ring.get(), nullptr) == nullptr);
        assert(g_fd_resets == 0 && rtl_device_preopened_fd_reset_owed() == 9 && g_open_fd_calls == 0);
        assert(r.dongle->calls[RTL_TEST_DONGLE_OPEN] == 0 && rtl_device_preopened_fd_held() == -1);
        g_fd_identity_rc = 0;
    }
    rtl_device_publish_preopened_fd(-1, 0);
    rtl_device_test_set_fd_identity_hook(nullptr);
    rtl_device_test_set_fd_reset_hook(nullptr);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    dsd_input_failure_clear();
}

/* A descriptor opened by index (rtl:0 on Android): the serial its identity read on the handle is the opened dongle's,
   as an index open over USB records its verified identity's, so a capture names it. */
void
test_preopened_by_index_records_its_serial(void) {
    rtl_device_test_set_open_fd_failure_hook(open_fd_opens_dongle);
    rtl_device_test_set_fd_identity_hook(fd_identity);
    set_devs({});
    g_fd_serial = "A1B2";
    g_fd_identity_rc = 0;
    {
        rig r;
        rtl_device_set_preopened_fd(9);
        rtl_device* dev = rtl_device_create_selected(0, nullptr, r.ring.get(), nullptr);
        assert(dev != nullptr);
        char sn[64];
        assert(rtl_device_usb_serial(dev, sn, sizeof sn) == 0 && strcmp(sn, "A1B2") == 0);
        rtl_device_destroy(dev);
    }
    rtl_device_set_preopened_fd(-1);
    rtl_device_test_set_fd_identity_hook(nullptr);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
}

/* ---- What an I/Q capture records ---- */

void
test_capture_names_the_dongle(void) {
    char args[256];
    rtl_stream_test_capture_rtl_usb_args(1, "00000002", args, sizeof args);
    assert(strcmp(args, "index=1,serial=00000002") == 0);
    rtl_stream_test_capture_rtl_usb_args(0, nullptr, args, sizeof args);
    assert(strcmp(args, "index=0") == 0);
    /* A serial no spec can name (here a ':' in it) is left out. */
    rtl_stream_test_capture_rtl_usb_args(3, "A:B", args, sizeof args);
    assert(strcmp(args, "index=3") == 0);
}

} // namespace

int
main(void) {
    rtl_device_select_set_driver_ops(&k_ops);
    rtl_usb_identity_set_driver_ops(&k_no_usb);

    test_unique_and_not_found();
    test_duplicate_readable_only();
    test_unreadable_never_matches_and_lists_blank();
    test_two_busy_then_unique();
    test_reorder_between_finds();
    test_duplicate_beyond_capacity();
    test_no_devices_and_failed_enumeration();
    test_list_lines();
    test_open_verified_serial();
    test_open_mismatch_then_retry();
    test_open_mismatch_twice_fails();
    test_open_not_found_or_duplicate_opens_nothing();
    test_open_by_index_unchanged();
    test_preopened_serial_checked_on_its_dongle();
    test_preopened_owed_reset_paid_only_on_the_requested_dongle();
    test_preopened_by_index_records_its_serial();
    test_capture_names_the_dongle();
    test_stream_looks_the_serial_up_at_every_start();
    test_stream_selects_from_its_spec();
    test_stream_descriptor_index_is_zero();

    rtl_usb_identity_set_driver_ops(nullptr);
    rtl_device_select_set_driver_ops(nullptr);
    DSD_FPRINTF(stderr, "IO_RTL_DEVICE_SELECT: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
