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

int
open_fd_opens_dongle(int fd) {
    (void)fd;
    return 1;
}

void
test_preopened_serial_checked_on_its_dongle(void) {
    rtl_device_test_set_open_fd_failure_hook(open_fd_opens_dongle);
    for (int match = 0; match < 2; match++) {
        set_devs({});
        g_world.handle_serials[0] = match ? "00000002" : "00000001";
        g_world.handle_serial_count = 1;
        rig r;
        dsd_input_failure_clear();
        rtl_device_set_preopened_fd(9);
        int index = -1;
        rtl_device* dev = rtl_device_create_selected(0, "00000002", r.ring.get(), &index);
        /* Never enumerated: USB discovery is off in this mode. */
        assert(g_world.count_calls == 0 && g_world.handle_calls == 1);
        if (match) {
            assert(dev != nullptr && index == 0 && rtl_device_preopened_fd_held() == 9);
            char sn[64];
            assert(rtl_device_usb_serial(dev, sn, sizeof sn) == 0 && strcmp(sn, "00000002") == 0);
            rtl_device_destroy(dev);
        } else {
            assert(dev == nullptr && reported_serial_failure());
            /* The descriptor is handed back, its dongle closed as it was. */
            assert(rtl_device_preopened_fd_in_use() == 0 && rtl_device_preopened_fd_held() == -1);
            assert(r.dongle->calls[RTL_TEST_DONGLE_CLOSE] == 1 && r.dongle->open_handles == 0);
            assert(only_opens_and_closes(*r.dongle));
        }
        assert(rtl_device_preopened_fd_in_use() == 0);
    }
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
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    dsd_input_failure_clear();
}

/* ---- Through a stream ---- */

/* One stream start on the dongle with serial 00000002, recording an I/Q capture: the index it was found at reaches the
   caller's options, and the capture's sidecar names the dongle by index and serial. */
int
stream_start_by_serial(const char* dir, std::string* source_args) {
    auto opts = std::make_unique<dsd_opts>();
    DSD_MEMSET(opts.get(), 0, sizeof *opts);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:00000002:851.0125M");
    DSD_SNPRINTF(opts->rtl_dev_serial, sizeof opts->rtl_dev_serial, "%s", "00000002");
    opts->rtl_dev_index = 0;
    opts->rtlsdr_center_freq = 851012500U;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_gain_value = 24;
    opts->frame_dmr = 1;
    char data_path[DSD_TEST_PATH_MAX];
    char meta_path[DSD_TEST_PATH_MAX];
    assert(dsd_test_path_join(data_path, sizeof data_path, dir, "capture.iq") == 0);
    assert(dsd_test_path_join(meta_path, sizeof meta_path, dir, "capture.iq.json") == 0);
    opts->iq_capture_requested = 1;
    opts->iq_capture_format = DSD_IQ_FORMAT_CU8;
    DSD_SNPRINTF(opts->iq_capture_path, sizeof opts->iq_capture_path, "%s", data_path);

    rig r;
    rtl_device_test_usb_stream_attach();
    RtlSdrContext* ctx = nullptr;
    assert(rtl_stream_create(opts.get(), &ctx) == 0 && ctx != nullptr);
    assert(rtl_stream_start(ctx) == 0);
    const int index = opts->rtl_dev_index;
    (void)rtl_stream_stop(ctx);
    (void)rtl_stream_destroy(ctx);
    rtl_device_test_usb_stream_detach();
    assert(r.dongle->open_handles == 0);

    dsd_iq_replay_config cfg{};
    char err[256];
    assert(dsd_iq_replay_read_metadata(meta_path, &cfg, err, sizeof err) == DSD_IQ_OK);
    *source_args = cfg.source_args;
    dsd_iq_replay_config_clear(&cfg);
    for (const char* leaf : {"capture.iq", "capture.iq.json"}) {
        char path[DSD_TEST_PATH_MAX];
        assert(dsd_test_path_join(path, sizeof path, dir, leaf) == 0);
        (void)remove(path);
    }
    return index;
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
    test_open_verified_serial();
    test_open_mismatch_then_retry();
    test_open_mismatch_twice_fails();
    test_open_not_found_or_duplicate_opens_nothing();
    test_open_by_index_unchanged();
    test_preopened_serial_checked_on_its_dongle();
    test_capture_names_the_dongle();
    test_stream_looks_the_serial_up_at_every_start();

    rtl_usb_identity_set_driver_ops(nullptr);
    rtl_device_select_set_driver_ops(nullptr);
    DSD_FPRINTF(stderr, "IO_RTL_DEVICE_SELECT: OK\n");
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
