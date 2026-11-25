// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The engine's choice of a local RTL-SDR at startup: by serial, and an index past the devices found.
 *
 * librtlsdr's enumeration is wrapped (the real dongles are never touched). A serial an `rtl:` spec names is looked up:
 * found, it selects its index; missing, or carried by two readable devices, the start is refused (never device 0),
 * saying why and how to see the serials. An unreadable device is listed as such, never with another device's strings.
 * An index past the devices found whose digits are a readable device's serial is refused with the `rtl:serial=` hint;
 * any other keeps falling back to device 0, the spec saying so. A pre-opened descriptor skips the lookup.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/io/rtl_device_select.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* The devices librtlsdr enumerates: their serials, NULL for one whose strings cannot be read. */
static const char* g_serials[8];
static int g_device_count = 0;
static int g_device_count_calls = 0;
static int g_fake_rtl_context = 0;

static char g_log[16384];
static size_t g_log_len = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
uint32_t
__wrap_rtlsdr_get_device_count(void) {
    g_device_count_calls++;
    return (uint32_t)g_device_count;
}

int
__wrap_rtlsdr_get_device_usb_strings(uint32_t index, char* manufact, char* product, char* serial) {
    if ((int)index >= g_device_count || g_serials[index] == NULL) {
        /* librtlsdr leaves the buffers as they were when the open fails. */
        return -1;
    }
    DSD_SNPRINTF(manufact, 256, "%s", "Realtek");
    DSD_SNPRINTF(product, 256, "%s", "RTL2838UHIDIR");
    DSD_SNPRINTF(serial, 256, "%s", g_serials[index]);
    return 0;
}

/* The lookup by serial, answering g_find_forced (with nothing seen) while it is not RTL_DEVICE_FIND_FOUND: FAILED as
   an enumeration error makes it (an MSVC exception in the count), NO_DEVICES as a count of 0 does (librtlsdr's when
   libusb cannot start). */
static rtl_device_find_result g_find_forced = RTL_DEVICE_FIND_FOUND;
rtl_device_find_result __real_rtl_device_find_serial(const char* serial, struct rtl_device_find_report* report);

rtl_device_find_result
__wrap_rtl_device_find_serial(const char* serial, struct rtl_device_find_report* report) {
    if (g_find_forced != RTL_DEVICE_FIND_FOUND) {
        DSD_MEMSET(report, 0, sizeof *report);
        report->index = -1;
        return g_find_forced;
    }
    return __real_rtl_device_find_serial(serial, report);
}

int
__wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx) {
    (void)opts;
    *out_ctx = (RtlSdrContext*)&g_fake_rtl_context;
    return 0;
}

int
__wrap_rtl_stream_start_gated(RtlSdrContext* ctx, const rtl_stream_start_gate* start_gate) {
    (void)ctx;
    (void)start_gate;
    return 0;
}

int
__wrap_rtl_stream_destroy(RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    const size_t n = text ? strlen(text) : 0U;
    if (n == 0U || g_log_len + n >= sizeof g_log) {
        return;
    }
    DSD_MEMCPY(g_log + g_log_len, text, n);
    g_log_len += n;
    g_log[g_log_len] = '\0';
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_log(const char* tag, const char* needle) {
    if (strstr(g_log, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: \"%s\" not in the log:\n%s\n", tag, needle, g_log);
        return 1;
    }
    return 0;
}

static void
set_devices(int count, const char* s0, const char* s1, const char* s2) {
    g_device_count = count;
    g_serials[0] = s0;
    g_serials[1] = s1;
    g_serials[2] = s2;
}

/* What one local-RTL setup attempt decided. */
typedef struct {
    int rc;
    int in_type;
    int dev_index;
    int exitflag;
    char spec[64];
} run_result;

/* One local-USB setup attempt on @p spec (playfiles keeps the run out of the decode loop, the stream wrappers keep it
   off any dongle). */
static int
run_local_rtl_setup(const char* spec, run_result* out) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    opts->playfiles = 1;
    opts->audio_out_type = 9;
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", spec);
    g_device_count_calls = 0;
    g_log_len = 0;
    g_log[0] = '\0';
    dsd_exitflag_store(0);

    out->rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    out->in_type = opts->audio_in_type;
    out->dev_index = opts->rtl_dev_index;
    out->exitflag = dsd_exitflag_load();
    DSD_SNPRINTF(out->spec, sizeof out->spec, "%s", opts->audio_in_dev);

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_serial_found_selects_its_index(void) {
    /* Two dongles streaming in other programs, the requested one at index 2. */
    set_devices(3, NULL, NULL, "00000003");
    run_result r;
    if (run_local_rtl_setup("rtl:00000003:851.375M", &r) != 0) {
        return 1;
    }
    int rc = 0;
    rc |= expect_true("serial found: accepted", r.rc == 0);
    rc |= expect_true("serial found: RTL input", r.in_type == AUDIO_IN_RTL);
    rc |= expect_true("serial found: its index", r.dev_index == 2);
    rc |= expect_log("serial found", "Selected Device #2 with Serial Number: 00000003");
    rc |= expect_log("serial found", "RTL #2 (SN 00000003): Freq=851375000");
    /* B3: the unreadable devices show no other device's strings (the first one read nothing at all). */
    rc |= expect_log("unreadable listed", "0:  (could not read: in use by another program, or no permission)");
    rc |= expect_log("unreadable listed", "1:  (could not read: in use by another program, or no permission)");
    rc |= expect_log("readable listed", "2:  Realtek, RTL2838UHIDIR, SN: 00000003");
    return rc;
}

/* A serial a dongle reports is shown escaped in the selected-device notice, as in the listing: never a line of its
   own. */
static int
test_selected_notice_escapes_the_serial(void) {
    set_devices(1, "A\nB\x1b[2J", NULL, NULL);
    run_result r;
    if (run_local_rtl_setup("rtl:0", &r) != 0) {
        return 1;
    }
    int rc = expect_true("escaped notice: accepted", r.rc == 0 && r.dev_index == 0);
    rc |= expect_log("escaped notice", "Selected Device #0 with Serial Number: A\\x0AB\\x1B[2J \n");
    rc |= expect_log("escaped listing", "0:  Realtek, RTL2838UHIDIR, SN: A\\x0AB\\x1B[2J\n");
    rc |= expect_true("escaped notice: no raw bytes", strstr(g_log, "A\nB") == NULL && strchr(g_log, 0x1b) == NULL);
    return rc;
}

/* A dongle opened by index whose strings read but report no serial: the selected-device notice says so, never that it
   could not be read, as the listing shows it with SN: (none); one whose read failed still says that. */
static int
test_selected_notice_no_serial(void) {
    set_devices(1, "", NULL, NULL);
    run_result r;
    if (run_local_rtl_setup("rtl:0", &r) != 0) {
        return 1;
    }
    int rc = expect_true("no-serial notice: accepted", r.rc == 0 && r.dev_index == 0);
    rc |= expect_log("no-serial listed", "0:  Realtek, RTL2838UHIDIR, SN: (none)\n");
    rc |= expect_log("no-serial notice", "NOTICE: Selected Device #0 (reports no serial)\n");
    rc |= expect_true("no-serial notice: not unreadable", strstr(g_log, "could not read") == NULL);
    set_devices(2, "", NULL, NULL);
    if (run_local_rtl_setup("rtl:1", &r) != 0) {
        return 1;
    }
    rc |= expect_log("unreadable notice",
                     "Selected Device #1 with Serial Number: (could not read: in use by another program, or no "
                     "permission)");
    return rc;
}

static int
test_unreadable_after_readable_shows_no_stale_strings(void) {
    set_devices(2, "00000001", NULL, NULL);
    run_result r;
    if (run_local_rtl_setup("rtl:0", &r) != 0) {
        return 1;
    }
    int rc = expect_true("index 0: accepted", r.rc == 0 && r.dev_index == 0);
    rc |= expect_log("readable listed", "0:  Realtek, RTL2838UHIDIR, SN: 00000001");
    rc |= expect_log("unreadable listed", "1:  (could not read: in use by another program, or no permission)");
    return rc;
}

/* What an invalid spec does, which every refused selection matches. */
static run_result g_invalid;

static int
expect_refused(const char* tag, const run_result* r) {
    int rc = expect_true(tag, r->rc != 0 && r->in_type != AUDIO_IN_RTL);
    char what[96];
    DSD_SNPRINTF(what, sizeof what, "%s: as an invalid spec", tag);
    rc |= expect_true(what, r->rc == g_invalid.rc && r->exitflag == g_invalid.exitflag);
    return rc;
}

static int
test_serial_not_found_refuses(void) {
    set_devices(3, NULL, "00000001", "00000002");
    run_result r;
    if (run_local_rtl_setup("rtl:serial=1090:851.375M", &r) != 0) {
        return 1;
    }
    int rc = expect_refused("serial not found", &r);
    rc |= expect_log("serial not found", "No RTL-SDR with serial \"1090\" found.");
    rc |= expect_log("serial not found", "1 device(s) could not be read (in use by another program, or no permission)");
    rc |= expect_log("serial not found", "Run dsd-neo --rtl-list");
    rc |= expect_log("serial not found: listed", "2:  Realtek, RTL2838UHIDIR, SN: 00000002");
    rc |= expect_true("serial not found: never device 0", strstr(g_log, "Selected Device") == NULL);
    return rc;
}

/* A device that reports no serial is no device that could not be read: the not-found error counts each apart. */
static int
test_serial_not_found_counts_no_serial_apart(void) {
    set_devices(3, NULL, "", "00000002");
    run_result r;
    if (run_local_rtl_setup("rtl:serial=1090:851.375M", &r) != 0) {
        return 1;
    }
    int rc = expect_refused("no serial apart", &r);
    rc |= expect_log("no serial apart", "1 device(s) could not be read (in use by another program, or no permission)");
    rc |= expect_log("no serial apart", "1 device(s) report no serial: they can be opened by index only.");
    rc |= expect_log("no serial listed", "1:  Realtek, RTL2838UHIDIR, SN: (none)");
    set_devices(2, "", "00000002", NULL);
    if (run_local_rtl_setup("rtl:serial=1090:851.375M", &r) != 0) {
        return 1;
    }
    rc |= expect_true("no serial alone: none 'could not be read'", strstr(g_log, "could not be read") == NULL);
    rc |= expect_log("no serial alone", "1 device(s) report no serial");
    return rc;
}

static int
test_serial_duplicate_refuses(void) {
    set_devices(3, "00000001", NULL, "00000001");
    run_result r;
    if (run_local_rtl_setup("rtl:00000001", &r) != 0) {
        return 1;
    }
    int rc = expect_refused("duplicate serial", &r);
    rc |= expect_log("duplicate serial", "2 RTL-SDR devices have serial \"00000001\"");
    rc |= expect_log("duplicate serial", "rtl_eeprom -s");
    return rc;
}

static int
test_out_of_range_index(void) {
    /* The digits are a readable device's serial: the user meant that serial. */
    set_devices(2, "00000001", "1090", NULL);
    run_result r;
    if (run_local_rtl_setup("rtl:1090:851.375M", &r) != 0) {
        return 1;
    }
    int rc = expect_refused("index equal to a serial", &r);
    rc |= expect_log("index equal to a serial", "use rtl:serial=1090");
    /* Not when that device cannot be read: today's fallback. */
    set_devices(2, "00000001", NULL, NULL);
    if (run_local_rtl_setup("rtl:5:851.375M:22", &r) != 0) {
        return 1;
    }
    rc |= expect_true("other index: falls back to 0", r.rc == 0 && r.dev_index == 0 && r.in_type == AUDIO_IN_RTL);
    rc |= expect_true("other index: the spec says so", strcmp(r.spec, "rtl:0:851.375M:22") == 0);
    rc |= expect_log("other index", "Requested RTL device index 5 out of range (found 2 device(s)); using 0");
    /* The serials cannot be read to tell whether one is 5 (the lookup failed, or listed no device at all): refused,
       never device 0 instead. */
    static const rtl_device_find_result kUnknown[] = {RTL_DEVICE_FIND_FAILED, RTL_DEVICE_FIND_NO_DEVICES};
    for (size_t k = 0; k < sizeof kUnknown / sizeof kUnknown[0]; k++) {
        g_find_forced = kUnknown[k];
        const int ran = run_local_rtl_setup("rtl:5:851.375M:22", &r);
        g_find_forced = RTL_DEVICE_FIND_FOUND;
        if (ran != 0) {
            return 1;
        }
        rc |= expect_refused("index past the devices, lookup failed", &r);
        rc |= expect_log("index past the devices, lookup failed", "could not be listed again to check their serials");
        rc |= expect_true("lookup failed: no fallback", strstr(g_log, "using 0") == NULL);
    }
    return rc;
}

/* A pre-opened descriptor is the one device there is: an index names it as 0, the spec saying so (as before serials),
   with nothing enumerated; a serial is left for the open to check on it. */
static int
test_preopened_descriptor_index_is_zero(void) {
    set_devices(0, NULL, NULL, NULL);
    rtl_device_set_preopened_fd(11);
    run_result r;
    int ran = run_local_rtl_setup("rtl:3:851.375M", &r);
    if (ran != 0) {
        rtl_device_set_preopened_fd(-1);
        return 1;
    }
    int rc = expect_true("descriptor index: accepted as 0", r.rc == 0 && r.in_type == AUDIO_IN_RTL && r.dev_index == 0);
    rc |= expect_true("descriptor index: the spec says 0", strcmp(r.spec, "rtl:0:851.375M") == 0);
    rc |= expect_true("descriptor index: nothing enumerated", g_device_count_calls == 0);
    rc |= expect_log("descriptor index", "Requested RTL device index 3 out of range (found 1 device(s)); using 0");
    rc |= expect_log("descriptor index", "RTL #0: Freq=851375000");
    ran = run_local_rtl_setup("rtl:00000002:851.375M", &r);
    rtl_device_set_preopened_fd(-1);
    if (ran != 0) {
        return 1;
    }
    rc |= expect_true("descriptor serial: spec kept", strcmp(r.spec, "rtl:00000002:851.375M") == 0);
    rc |= expect_true("descriptor serial: no index warning", strstr(g_log, "out of range") == NULL);
    return rc;
}

static int
test_preopened_descriptor_skips_lookup(void) {
    set_devices(0, NULL, NULL, NULL);
    rtl_device_set_preopened_fd(11);
    run_result r;
    const int ran = run_local_rtl_setup("rtl:00000002", &r);
    rtl_device_set_preopened_fd(-1);
    if (ran != 0) {
        return 1;
    }
    int rc = expect_true("descriptor: accepted", r.rc == 0 && r.in_type == AUDIO_IN_RTL);
    rc |= expect_true("descriptor: nothing enumerated", g_device_count_calls == 0);
    return rc;
}

int
main(void) {
    dsd_neo_log_set_tap(capture_log, NULL);
    int rc = 0;
    set_devices(1, "00000001", NULL, NULL);
    if (run_local_rtl_setup("rtl: A", &g_invalid) != 0) {
        return 1;
    }
    rc |= expect_true("an invalid spec is refused", g_invalid.rc != 0);

    rc |= test_serial_found_selects_its_index();
    rc |= test_unreadable_after_readable_shows_no_stale_strings();
    rc |= test_selected_notice_escapes_the_serial();
    rc |= test_selected_notice_no_serial();
    rc |= test_serial_not_found_refuses();
    rc |= test_serial_duplicate_refuses();
    rc |= test_serial_not_found_counts_no_serial_apart();
    rc |= test_out_of_range_index();
    rc |= test_preopened_descriptor_skips_lookup();
    rc |= test_preopened_descriptor_index_is_zero();

    rtl_device_set_preopened_fd(-1);
    dsd_exitflag_store(0);
    if (rc == 0) {
        DSD_FPRINTF(stderr, "ENGINE_RTL_SERIAL_SELECT: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
