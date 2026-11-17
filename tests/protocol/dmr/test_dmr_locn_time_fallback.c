// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate LOCN (NMEA-like) date handling: out-of-range decoded dates should
 * be ignored and system time used instead in the LRRP output file.
 *
 * Issue #800 bug 2.5: every LOCN field is checked before it is used (digits only, a sign per field, minutes below 60,
 * degrees within 90/180 with the boundary only at zero minutes, a date that exists), and an admitted report is staged
 * as a provisional-format position (decision D12). The coordinates are synthetic.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

// Minimal stubs required by dmr_pdu.c when linked directly
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

// Deterministic system time fallback for file writer
int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "2004/05/06" : "01:23:45";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

// Under test
void dmr_locn(dsd_opts* opts, dsd_state* state, uint16_t len, uint8_t* DMR_PDU);

static int
expect_nonempty(const char* buf, const char* tag) {
    if (!buf || buf[0] == '\0') {
        DSD_FPRINTF(stderr, "%s: empty\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_no_substr(const char* buf, const char* needle, const char* tag) {
    if (strstr(buf, needle)) {
        DSD_FPRINTF(stderr, "%s: found unexpected '%s'\n", tag, needle);
        return 1;
    }
    return 0;
}

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!buf || !strstr(buf, needle)) {
        DSD_FPRINTF(stderr, "%s: missing '%s'\n", tag, needle);
        return 1;
    }
    return 0;
}

typedef struct {
    const char* tag;
    const char* text;
    int published; // admitted, staged and written to -L
    int32_t lat_e7;
    int32_t lon_e7;
    const char* summary; // the slot text, or NULL to skip the check
} locn_case;

static const locn_case k_locn_cases[] = {
    {"plain", "N2230.0000E04500.0000", 1, 225000000, 450000000, "NMEA / LOCN; Source: 1193046 (22.50000, 45.00000)"},
    {"south west", "S2230.5000W04500.0000", 1, -225083333, -450000000, NULL},
    // A later N field is north even after an S field.
    {"sign per field", "S2230.0000N1130.0000E04500.0000", 1, 115000000, 450000000, NULL},
    {"boundary", "N9000.0000E18000.0000", 1, 900000000, 1800000000, NULL},
    {"minute 60", "N4560.0000E04500.0000", 0, 0, 0, "NMEA / LOCN; Source: 1193046 Position suppressed (invalid);"},
    {"latitude 99", "N9930.0000E04500.0000", 0, 0, 0, NULL},
    {"past the pole", "N9000.0001E04500.0000", 0, 0, 0, NULL},
    {"past the date line", "W18000.0001N1000.0000", 0, 0, 0, NULL},
    // A non-digit is no field at all (it used to shift by a negative amount).
    {"non-digit", "N/430.0000E04500.0000", 0, 0, 0, ""},
};

static int
run_locn_case(dsd_opts* opts, dsd_state* st, const locn_case* c) {
    FILE* trunc = dsd_fopen_private(opts->lrrp_out_file, "w");
    if (trunc == NULL || fclose(trunc) != 0) {
        return 1;
    }
    dsd_event_stage_clear(st, 0);
    st->dmr_lrrp_gps[0][0] = '\0';
    uint8_t pdu[64];
    const size_t len = strlen(c->text);
    DSD_MEMCPY(pdu, c->text, len);
    dmr_locn(opts, st, (uint16_t)len, pdu);

    int rc = 0;
    const dsd_position_fix* fix = dsd_event_staged_position(st, 0);
    char file[256] = "";
    FILE* f = fopen(opts->lrrp_out_file, "rb");
    if (f != NULL) {
        const size_t n = fread(file, 1, sizeof file - 1U, f);
        file[n] = '\0';
        fclose(f);
    }
    if (c->published) {
        const uint16_t want_flags = (uint16_t)DSD_POS_FLAG_FORMAT_PROVISIONAL;
        if (fix->rev == 0U || fix->kind != DSD_POS_DMR_LOCN || fix->lat_e7 != c->lat_e7 || fix->lon_e7 != c->lon_e7
            || (fix->flags & 0xFFFBU /* ~DSD_POS_FLAG_TARGET_GROUP */) != want_flags || fix->source_id != 0x123456U
            || file[0] == '\0') {
            DSD_FPRINTF(stderr, "%s: rev %llu kind %u (%d, %d) flags %04X, row '%s'\n", c->tag,
                        (unsigned long long)fix->rev, (unsigned)fix->kind, (int)fix->lat_e7, (int)fix->lon_e7,
                        (unsigned)fix->flags, file);
            rc = 1;
        }
    } else if (fix->rev != 0U || file[0] != '\0') {
        DSD_FPRINTF(stderr, "%s: published (rev %llu, row '%s')\n", c->tag, (unsigned long long)fix->rev, file);
        rc = 1;
    }
    if (c->summary != NULL && strcmp(st->dmr_lrrp_gps[0], c->summary) != 0) {
        DSD_FPRINTF(stderr, "%s: text '%s', want '%s'\n", c->tag, st->dmr_lrrp_gps[0], c->summary);
        rc = 1;
    }
    return rc;
}

// The date prints only when it exists: 31 February 2024 does not, 29 February 2024 does.
static int
run_locn_date_cases(dsd_opts* opts, dsd_state* st) {
    static const struct {
        const char* text;
        const char* needle;
        int present;
    } dates[] = {
        {"A120000310224N2230.0000E04500.0000", "2024/02/31", 0},
        {"A120000290224N2230.0000E04500.0000", "2024/02/29", 1},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof dates / sizeof dates[0]; i++) {
        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "dmr_locn_date") != 0) {
            return 1;
        }
        uint8_t pdu[64];
        const size_t len = strlen(dates[i].text);
        DSD_MEMCPY(pdu, dates[i].text, len);
        dmr_locn(opts, st, (uint16_t)len, pdu);
        dsd_test_capture_stderr_end(&cap);
        char err[1024];
        if (dsd_test_capture_stderr_read(&cap, err, sizeof err) != 0) {
            return 1;
        }
        if ((strstr(err, dates[i].needle) != NULL) != dates[i].present) {
            DSD_FPRINTF(stderr, "date %s: stderr '%s'\n", dates[i].needle, err);
            rc = 1;
        }
    }
    return rc;
}

/* Issue #800: a valid `A` date and time is the report's fix time (Unix seconds, UTC); a missing or impossible one leaves
 * it unknown. Two reports that differ only in their time are two reports, not a repeat. */
static int
run_locn_fix_time_cases(dsd_opts* opts, dsd_state* st) {
    static const struct {
        const char* text;
        int64_t fix_time_s;
    } cases[] = {
        {"A120000290224N2230.0000E04500.0000", 1709208000}, // 2024-02-29 12:00:00
        {"A120030290224N2230.0000E04500.0000", 1709208030}, // the same place, 30 s later
        {"N2230.0000E04500.0000", 0},                       // no time field
        {"A120000310224N2240.0000E04500.0000", 0},          // 31 February does not exist
    };

    if (dsd_position_feed_start(st) != 0) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_event_stage_clear(st, 0);
        uint8_t pdu[64];
        const size_t len = strlen(cases[i].text);
        DSD_MEMCPY(pdu, cases[i].text, len);
        dmr_locn(opts, st, (uint16_t)len, pdu);
        const dsd_position_fix* fix = dsd_event_staged_position(st, 0);
        if (fix->rev == 0U || fix->fix_time_s != cases[i].fix_time_s) {
            DSD_FPRINTF(stderr, "LOCN fix time %s: rev %llu fix_time %lld, want %lld\n", cases[i].text,
                        (unsigned long long)fix->rev, (long long)fix->fix_time_s, (long long)cases[i].fix_time_s);
            rc = 1;
        }
    }
    dsd_position_fix kept[8];
    dsd_position_feed_meta meta;
    const int n = dsd_position_feed_copy_since(st, 0U, kept, 8U, &meta);
    if (n != 4 || kept[0].fix_time_s != 1709208000 || kept[1].fix_time_s != 1709208030) {
        DSD_FPRINTF(stderr, "LOCN fix time: %d reports kept, want 4 (the later time is no repeat)\n", n);
        rc = 1;
    }
    dsd_state_ext_free_all(st);
    return rc;
}

static int
run_locn_cases(dsd_opts* opts) {
    static dsd_state st;
    st.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    st.dmr_lrrp_source[0] = 0x123456;
    st.event_history_s = calloc(2, sizeof *st.event_history_s);
    if (st.event_history_s == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof k_locn_cases / sizeof k_locn_cases[0]; i++) {
        rc |= run_locn_case(opts, &st, &k_locn_cases[i]);
    }
    rc |= run_locn_date_cases(opts, &st);

    // A failed check is the reason a damaged report is withheld, even when a field is also out of range.
    st.event_crc_invalid[0] = 1;
    static const locn_case damaged = {"damaged and out of range",
                                      "N4560.0000E04500.0000",
                                      0,
                                      0,
                                      0,
                                      "NMEA / LOCN; Source: 1193046 Position suppressed (CRC ERR);"};
    rc |= run_locn_case(opts, &st, &damaged);
    st.event_crc_invalid[0] = 0;

    // A DMR data packet addressed to its header's group target is a group report.
    st.dmr_lrrp_target[0] = 777;
    st.dmr_data_checked_group[0] = 1;
    static const locn_case group = {"group target", "N2230.0000E04500.0000", 1, 225000000, 450000000, NULL};
    rc |= run_locn_case(opts, &st, &group);
    const dsd_position_fix* fix = dsd_event_staged_position(&st, 0);
    if ((fix->flags & (uint16_t)DSD_POS_FLAG_TARGET_GROUP) == 0U || fix->target_id != 777U) {
        DSD_FPRINTF(stderr, "group target: flags %04X target %u\n", (unsigned)fix->flags, (unsigned)fix->target_id);
        rc = 1;
    }
    st.dmr_lrrp_target[0] = 0;
    st.dmr_data_checked_group[0] = 0;
    rc |= run_locn_fix_time_cases(opts, &st);
    free(st.event_history_s);
    st.event_history_s = NULL;
    return rc;
}

int
main(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    st.dmr_lrrp_source[0] = 0x123456;

    // Temp LRRP output path
    char outtmpl[DSD_TEST_PATH_MAX];
    int ofd = dsd_test_mkstemp(outtmpl, sizeof(outtmpl), "dmr_locn_time_fallback");
    if (ofd < 0) {
        return 100;
    }
    (void)dsd_close(ofd);
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", outtmpl);
    opts.lrrp_file_output = 1;

    // Build LOCN payload with invalid BCD year 2038
    uint8_t pdu[64];
    int i = 0;
    DSD_MEMSET(pdu, 0, sizeof pdu);

    // 'A' time/date token with invalid year 38 (-> 2038)
    pdu[i++] = 0x41; // 'A'
    pdu[i++] = '1';
    pdu[i++] = '2'; // hour 12
    pdu[i++] = '3';
    pdu[i++] = '4'; // minute 34
    pdu[i++] = '5';
    pdu[i++] = '6'; // second 56
    pdu[i++] = '0';
    pdu[i++] = '7'; // day 07
    pdu[i++] = '0';
    pdu[i++] = '8'; // month 08
    pdu[i++] = '3';
    pdu[i++] = '8'; // year 38 -> 2038 (invalid)

    // 'N' latitude: ddmm.mmmm -> using dd=12, mm=34, mmmm=5678
    pdu[i++] = 0x4E; // 'N'
    pdu[i++] = '1';
    pdu[i++] = '2';
    pdu[i++] = '3';
    pdu[i++] = '4';
    pdu[i++] = '.'; // dot ignored by decoder (will be treated as non-digit; but decoder reads separate sec digits)
    pdu[i++] = '5';
    pdu[i++] = '6';
    pdu[i++] = '7';
    pdu[i++] = '8';

    // 'E' longitude: dddmm.mmmm -> 123,45,6789
    pdu[i++] = 0x45; // 'E'
    pdu[i++] = '1';
    pdu[i++] = '2';
    pdu[i++] = '3';
    pdu[i++] = '4';
    pdu[i++] = '5';
    pdu[i++] = '.';
    pdu[i++] = '6';
    pdu[i++] = '7';
    pdu[i++] = '8';
    pdu[i++] = '9';

    dmr_locn(&opts, &st, (uint16_t)i, pdu);
    rc |= expect_has_substr(st.dmr_lrrp_gps[0], "Source: 1193046", "LOCN source id not truncated");

    // Read LRRP file content
    FILE* f = fopen(outtmpl, "rb");
    if (!f) {
        return 101;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t psz = 0;
    if (sz > 0 && (size_t)sz <= SIZE_MAX - 1U) {
        psz = (size_t)sz;
    }
    char* buf = calloc(psz + 1u, 1u);
    if (!buf) {
        fclose(f);
        remove(outtmpl);
        return 102;
    }
    // The buffer is calloc'd one byte longer than the file and therefore
    // already terminated; what the read owes is the count, since
    // _FORTIFY_SOURCE declares fread __wur and a short read here means the
    // capture never landed.
    if (fread(buf, 1, psz, f) != psz) {
        fclose(f);
        free(buf);
        remove(outtmpl);
        return 103;
    }
    fclose(f);

    rc |= expect_nonempty(buf, "LOCN LRRP file non-empty");
    rc |= expect_no_substr(buf, "2038/", "LOCN excludes bogus decoded year");
    free(buf);

    rc |= run_locn_cases(&opts);
    remove(outtmpl);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
