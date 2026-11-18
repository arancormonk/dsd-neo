// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression: LRRP position tokens from CRC-failed PDUs should not be written to
 * LRRP output files, to avoid emitting wildly incorrect coordinates on marginal
 * signals when CRC relaxation is enabled.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Minimal stubs for direct link with dmr_pdu.c
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_unicode_supported(void) {
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count,
                const dsd_gps_address* src) {
    (void)bit_count;
    (void)src;
    (void)opts;
    (void)state;
    (void)bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_cellocator(dsd_opts* opts, dsd_state* state, const uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
}

// Provide deterministic system time for LRRP output (when enabled).
int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "1999/01/02" : "11:22:33";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

// Under test
void dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU,
              uint8_t pdu_crc_ok);

static int
file_size_nonzero(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return (sz > 0) ? 1 : 0;
}

/* Issue #800, fail closed: a report whose packet failed its check prints only that its position was suppressed; the
 * rest of the document (here its time) prints for a checked packet, or with -Z. Returns 0 when it holds. */
static int
failed_report_prints_no_content(dsd_opts* opts, dsd_state* st) {
    // POINT_2D, then a timestamp (token 0x34): 2026-06-15 12:00:00 UTC.
    static const uint8_t pdu[] = {0x07, 17,   0x22, 0x00, 0x66, 0x10, 0x00, 0x00, 0x00, 0x20,
                                  0x00, 0x00, 0x00, 0x34, 0x1F, 0xA9, 0x9E, 0xC0, 0x00, 0x00};

    static const struct {
        uint8_t crc_ok;
        int payload;
        int shown;
    } runs[] = {{0, 0, 0}, {0, 1, 1}, {1, 0, 1}};

    const int saved_lrrp_output = opts->lrrp_file_output;
    opts->lrrp_file_output = 0;
    int rc = 0;
    for (size_t r = 0; r < sizeof runs / sizeof runs[0]; r++) {
        opts->payload = runs[r].payload;
        dsd_test_capture_stderr cap;
        char out[4096];
        if (dsd_test_capture_stderr_begin(&cap, "dmr_lrrp_failed_content") != 0) {
            return 1;
        }
        dmr_lrrp(opts, st, (uint16_t)(sizeof pdu - 1U), 111, 222, (uint8_t*)pdu, runs[r].crc_ok);
        dsd_test_capture_stderr_end(&cap);
        if (dsd_test_capture_stderr_read(&cap, out, sizeof out) != 0) {
            return 1;
        }
        if ((strstr(out, "LRRP Time: 2026.06.15 12:00:00") != NULL) != runs[r].shown
            || (!runs[r].crc_ok && strstr(out, "Position: (suppressed; CRC ERR)") == NULL)) {
            DSD_FPRINTF(stderr, "LRRP crc_ok %u -Z %d: content shown wrong: %s\n", runs[r].crc_ok, runs[r].payload,
                        out);
            rc = 1;
        }
    }
    opts->payload = 0;
    opts->lrrp_file_output = saved_lrrp_output;
    return rc;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;

    char outtmpl[DSD_TEST_PATH_MAX];
    int ofd = dsd_test_mkstemp(outtmpl, sizeof(outtmpl), "dmr_lrrp_crc_gating");
    if (ofd < 0) {
        return 100;
    }
    (void)dsd_close(ofd);
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", outtmpl);
    opts.lrrp_file_output = 1;

    // Minimal LRRP response with a POINT_2D token.
    uint8_t pdu[32];
    int i = 0;
    DSD_MEMSET(pdu, 0, sizeof pdu);
    pdu[i++] = 0x07; // response
    pdu[i++] = 11;   // the document length: TRIGGER pattern (2) + POINT_2D (9)
    pdu[i++] = 0x22; // pattern
    pdu[i++] = 0x00;

    // point-2d (lat/lon)
    pdu[i++] = 0x66;
    pdu[i++] = 0x10;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00; // lat raw
    pdu[i++] = 0x20;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00; // lon raw

    // CRC-failed PDU: should suppress file output of coordinates.
    dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 111, /*dst*/ 222, pdu, /*pdu_crc_ok*/ 0);
    int nz = file_size_nonzero(outtmpl);
    if (nz != 0) {
        DSD_FPRINTF(stderr, "Expected empty LRRP file on CRC fail; got non-empty\n");
        remove(outtmpl);
        return 1;
    }

    // CRC-ok PDU: should write one LRRP line.
    dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 111, /*dst*/ 222, pdu, /*pdu_crc_ok*/ 1);
    nz = file_size_nonzero(outtmpl);
    if (nz != 1) {
        DSD_FPRINTF(stderr, "Expected non-empty LRRP file on CRC ok; got empty\n");
        remove(outtmpl);
        return 2;
    }

    remove(outtmpl);
    return failed_report_prints_no_content(&opts, &st) != 0 ? 3 : 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
