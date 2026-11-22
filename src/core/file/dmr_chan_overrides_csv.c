// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The --dmr-chan-overrides file (<dsd-neo/core/dmr_chan_overrides_csv.h>): read and checked here, applied by the DMR
   module (dmr_t3_overlay_load_csv()). */

#include <dsd-neo/core/dmr_chan_overrides_csv.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "csv_parse_internal.h"

enum { DMR_CHAN_OVERRIDE_MAX_LPCN = 4094, DMR_CHAN_OVERRIDE_MIN_HZ = 100000 };

typedef struct {
    const char* filename;
    unsigned row;
    int data_rows; /* non-blank lines seen so far, this one included */
    uint8_t seen[(DMR_CHAN_OVERRIDE_MAX_LPCN + 8) / 8];
} dmr_chan_override_reader;

static void
dmr_chan_override_warn(const dmr_chan_override_reader* r, const char* why) {
    LOG_WARN("WARNING: DMR channel overrides file '%s' row %u %s; skipping.\n", r->filename, r->row, why);
}

/* Whether @p text starts like a number: the first line that does not is a header. */
static int
dmr_chan_override_starts_numeric(const char* text) {
    const char c = text[0];
    return (c >= '0' && c <= '9') || c == '+' || c == '-';
}

/* Parse one data line; returns 1 with the row in @p lpcn and @p hz, 0 when it is skipped (reported unless a header). */
static int
dmr_chan_override_parse(const dmr_chan_override_reader* r, char* line, uint16_t* lpcn, long* hz) {
    char* fields[3] = {NULL, NULL, NULL};
    const size_t n = csv_split_preserve_empty(line, fields, 3);
    const char* lpcn_text = trim_ws(fields[0]);
    if (r->data_rows == 1 && !dmr_chan_override_starts_numeric(lpcn_text)) {
        return 0; /* the header */
    }
    if (n != 2U) {
        dmr_chan_override_warn(r, "is not lpcn,frequency_hz");
        return 0;
    }
    long channel = 0;
    if (dsd_parse_long_strict(lpcn_text, 10, 1, DMR_CHAN_OVERRIDE_MAX_LPCN, &channel) != 0) {
        dmr_chan_override_warn(r, "has no channel number 1..4094");
        return 0;
    }
    long freq = 0;
    if (dsd_parse_long_strict(trim_ws(fields[1]), 10, 0, 2147483647L, &freq) != 0
        || (freq != 0 && freq < DMR_CHAN_OVERRIDE_MIN_HZ)) {
        dmr_chan_override_warn(r, "has no frequency (0 to hide, or 100000..2147483647 Hz)");
        return 0;
    }
    *lpcn = (uint16_t)channel;
    *hz = freq;
    return 1;
}

/* A channel given again: reported, and the later row is used. */
static void
dmr_chan_override_note_seen(dmr_chan_override_reader* r, uint16_t lpcn) {
    const uint8_t bit = (uint8_t)(1U << (lpcn & 7U));
    if ((r->seen[lpcn >> 3] & bit) != 0U) {
        LOG_WARN("WARNING: DMR channel overrides file '%s' row %u gives channel %u again; the later row is used.\n",
                 r->filename, r->row, (unsigned)lpcn);
    }
    r->seen[lpcn >> 3] = (uint8_t)(r->seen[lpcn >> 3] | bit);
}

int
dsd_csv_dmr_chan_overrides_read(const char* path, dsd_dmr_chan_override_row_fn row, void* ctx) {
    if (row == NULL) {
        return -1;
    }
    char filename[CSV_IMPORT_PATH_MAX];
    FILE* fp = csv_open_user_read_file("DMR channel overrides file", path, filename, sizeof filename);
    if (fp == NULL) {
        return -1;
    }
    dmr_chan_override_reader r;
    DSD_MEMSET(&r, 0, sizeof r);
    r.filename = filename;
    char buffer[BSIZE];
    int overlong = 0;
    int taken = 0;
    int got = 0;
    while ((got = csv_read_line(fp, buffer, sizeof buffer, &overlong)) > 0) {
        r.row++;
        if (!csv_data_row_ready(buffer, overlong, filename, r.row, NULL)) {
            r.data_rows += overlong ? 1 : 0;
            continue;
        }
        r.data_rows++;
        uint16_t lpcn = 0U;
        long hz = 0;
        if (!dmr_chan_override_parse(&r, buffer, &lpcn, &hz)) {
            continue;
        }
        dmr_chan_override_note_seen(&r, lpcn);
        if (row(lpcn, hz, ctx) == 0) {
            taken++;
        } else {
            dmr_chan_override_warn(&r, "was refused");
        }
    }
    fclose(fp);
    if (got < 0) {
        LOG_ERROR("Unable to read DMR channel overrides file '%s'\n", filename);
        return -1;
    }
    return taken;
}
