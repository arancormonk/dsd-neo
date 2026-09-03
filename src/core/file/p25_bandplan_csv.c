// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 band plan CSV: one row per identifier, read into and written from the IDEN tables.
 *
 * Format (docs/csv-formats.md, "P25 Band Plan CSV"):
 *
 *   iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid
 *
 * The first three columns are positional and required; the rest are matched by
 * header name (positional fallback for columns 4-6) and optional. Values are in
 * plain Hz and converted to the IDEN_UP units p25_iden_entry_t stores: base in
 * 5 Hz steps, spacing in 125 Hz steps, the transmit offset in channel-spacing
 * steps for a VHF/UHF row (one that gives a bandwidth) or a TDMA row (both
 * IDEN_UP_VU and IDEN_UP_TDMA carry a 13-bit offset in spacing units) and
 * 250 kHz steps for a standard FDMA row (8-bit), and the bandwidth as the
 * 4-bit VU code (6250 -> 4, 12500 -> 5).
 * A row that names a WACN/SYS seeds only when the receiver is on that system.
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdio.h>
#include "csv_parse_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define BP_MAX_FIELDS 16

/* Column roles; the first three are positional, the others resolved from the header. */
typedef struct {
    int type_idx;
    int offset_idx;
    int bw_idx;
    int wacn_idx;
    int sysid_idx;
} bp_header_cols;

/* IDEN_UP field limits: 32-bit base, 10-bit spacing, 8-bit (standard) or 13-bit (VU) offset. */
#define BP_BASE_UNIT_HZ         5ULL
#define BP_BASE_MAX_UNITS       0xFFFFFFFFULL
#define BP_SPACING_UNIT_HZ      125ULL
#define BP_SPACING_MAX_UNITS    1023ULL
#define BP_STD_OFFSET_UNIT_HZ   250000L
#define BP_STD_OFFSET_MAX_UNITS 255L
#define BP_VU_OFFSET_MAX_UNITS  8191L
#define BP_WACN_MAX             0xFFFFFULL
#define BP_SYSID_MAX            0xFFFULL

static void
bp_header_default(bp_header_cols* cols) {
    cols->type_idx = 3;
    cols->offset_idx = 4;
    cols->bw_idx = 5;
    cols->wacn_idx = -1;
    cols->sysid_idx = -1;
}

/*
 * A positional column (3-5) that a named wacn/sysid claims is no longer type/offset/bw.
 */
static void
bp_release_claimed_positional(bp_header_cols* cols, const int named[3]) {
    int* positional[3] = {&cols->type_idx, &cols->offset_idx, &cols->bw_idx};
    for (int k = 0; k < 3; k++) {
        const int pos = 3 + k;
        if (!named[k] && (cols->wacn_idx == pos || cols->sysid_idx == pos)) {
            *positional[k] = -1;
        }
    }
}

/*
 * Columns 0-2 are iden/base/spacing by position. From column 3 on a recognised
 * header name wins; an unrecognised name in columns 3-5 keeps the positional
 * meaning so a header written by hand ("type,offset,bw") still loads.
 */
static void
bp_parse_header(char* line, bp_header_cols* cols) {
    char* fields[BP_MAX_FIELDS];
    bp_header_default(cols);
    const size_t n = csv_split_preserve_empty(line, fields, BP_MAX_FIELDS);
    int named[3] = {0, 0, 0};

    const struct {
        const char* name;
        int* idx;
        int* named;
    } table[] = {
        {"type", &cols->type_idx, &named[0]},       {"tx_offset_hz", &cols->offset_idx, &named[1]},
        {"bandwidth_hz", &cols->bw_idx, &named[2]}, {"wacn", &cols->wacn_idx, NULL},
        {"sysid", &cols->sysid_idx, NULL},
    };

    for (size_t i = 3; i < n; i++) {
        const char* name = trim_ws(fields[i]);
        for (size_t k = 0; k < sizeof table / sizeof table[0]; k++) {
            if (csv_ascii_casecmp(name, table[k].name) != 0) {
                continue;
            }
            *table[k].idx = (int)i;
            if (table[k].named) {
                *table[k].named = 1;
            }
            break;
        }
    }
    bp_release_claimed_positional(cols, named);
}

static const char*
bp_cell(char** fields, size_t n, int idx) {
    if (idx < 0 || (size_t)idx >= n) {
        return "";
    }
    return trim_ws(fields[idx]);
}

static int
bp_bw_code_from_hz(unsigned long long hz, uint8_t* out) {
    if (hz == 0ULL) {
        *out = 0;
        return 1;
    }
    if (hz == 6250ULL) {
        *out = 4;
        return 1;
    }
    if (hz == 12500ULL) {
        *out = 5;
        return 1;
    }
    return 0;
}

static long
bp_bw_hz_from_code(uint8_t code) {
    if (code == 4) {
        return 6250L;
    }
    if (code == 5) {
        return 12500L;
    }
    return 0L;
}

static int
bp_parse_required(char** fields, size_t n, unsigned long long* iden, unsigned long long* base_hz,
                  unsigned long long* spacing_hz, const char** why) {
    if (n < 3) {
        *why = "expected at least iden,base_hz,spacing_hz";
        return 0;
    }
    if (!parse_dec_u64_strict(bp_cell(fields, n, 0), iden) || *iden > 15ULL) {
        *why = "iden must be 0-15";
        return 0;
    }
    if (!parse_dec_u64_strict(bp_cell(fields, n, 1), base_hz) || *base_hz == 0ULL || *base_hz % BP_BASE_UNIT_HZ != 0ULL
        || *base_hz / BP_BASE_UNIT_HZ > BP_BASE_MAX_UNITS) {
        *why = "base_hz must be a multiple of 5 Hz below 21474836475";
        return 0;
    }
    if (!parse_dec_u64_strict(bp_cell(fields, n, 2), spacing_hz) || *spacing_hz == 0ULL
        || *spacing_hz % BP_SPACING_UNIT_HZ != 0ULL || *spacing_hz / BP_SPACING_UNIT_HZ > BP_SPACING_MAX_UNITS) {
        *why = "spacing_hz must be a multiple of 125 Hz up to 127875";
        return 0;
    }
    return 1;
}

static int
bp_parse_type_and_bw(char** fields, size_t n, const bp_header_cols* cols, unsigned long long* type, uint8_t* bw_vu,
                     const char** why) {
    const char* type_s = bp_cell(fields, n, cols->type_idx);
    if (type_s[0] != '\0' && (!parse_dec_u64_strict(type_s, type) || *type > 15ULL)) {
        *why = "type must be 0-15 (1 FDMA, 3 two-slot TDMA, 4 four-slot)";
        return 0;
    }
    const char* bw_s = bp_cell(fields, n, cols->bw_idx);
    unsigned long long bw_hz = 0ULL;
    if (bw_s[0] != '\0' && (!parse_dec_u64_strict(bw_s, &bw_hz) || !bp_bw_code_from_hz(bw_hz, bw_vu))) {
        *why = "bandwidth_hz must be empty, 0, 6250 or 12500";
        return 0;
    }
    return 1;
}

/*
 * IDEN_UP_VU and IDEN_UP_TDMA carry a 13-bit offset in channel-spacing units;
 * the standard IDEN_UP an 8-bit one in 250 kHz units.
 */
static int
bp_parse_offset(char** fields, size_t n, const bp_header_cols* cols, unsigned long long spacing_hz, int spacing_units,
                int* trans_off, const char** why) {
    long offset_hz = 0L;
    const char* off_s = bp_cell(fields, n, cols->offset_idx);
    if (off_s[0] != '\0' && !parse_dec_long_strict(off_s, &offset_hz)) {
        *why = "tx_offset_hz must be a signed decimal";
        return 0;
    }
    const long offset_unit = spacing_units ? (long)spacing_hz : BP_STD_OFFSET_UNIT_HZ;
    const long offset_max = spacing_units ? BP_VU_OFFSET_MAX_UNITS : BP_STD_OFFSET_MAX_UNITS;
    if (offset_hz % offset_unit != 0L || offset_hz / offset_unit > offset_max
        || offset_hz / offset_unit < -offset_max) {
        *why = spacing_units ? "tx_offset_hz must be a multiple of spacing_hz (VHF/UHF or TDMA row) within 8191 steps"
                             : "tx_offset_hz must be a multiple of 250000 Hz within 255 steps";
        return 0;
    }
    *trans_off = (int)(offset_hz / offset_unit);
    return 1;
}

static int
bp_parse_system(char** fields, size_t n, const bp_header_cols* cols, unsigned long long* wacn,
                unsigned long long* sysid, const char** why) {
    const char* wacn_s = bp_cell(fields, n, cols->wacn_idx);
    const char* sysid_s = bp_cell(fields, n, cols->sysid_idx);
    if ((wacn_s[0] != '\0') != (sysid_s[0] != '\0')) {
        *why = "wacn and sysid must be given together";
        return 0;
    }
    if (wacn_s[0] == '\0') {
        return 1;
    }
    if (!parse_hex_u64_strict(wacn_s, wacn) || *wacn > BP_WACN_MAX) {
        *why = "wacn must be hex 00000-FFFFF";
        return 0;
    }
    if (!parse_hex_u64_strict(sysid_s, sysid) || *sysid > BP_SYSID_MAX) {
        *why = "sysid must be hex 000-FFF";
        return 0;
    }
    return 1;
}

static int
bp_parse_row_fields(char** fields, size_t n, const bp_header_cols* cols, p25_bandplan_row_t* row, const char** why) {
    unsigned long long iden = 0ULL;
    unsigned long long base_hz = 0ULL;
    unsigned long long spacing_hz = 0ULL;
    unsigned long long type = 1ULL;
    unsigned long long wacn = 0ULL;
    unsigned long long sysid = 0ULL;
    uint8_t bw_vu = 0;
    int trans_off = 0;

    if (!bp_parse_required(fields, n, &iden, &base_hz, &spacing_hz, why)
        || !bp_parse_type_and_bw(fields, n, cols, &type, &bw_vu, why)) {
        return 0;
    }
    // Same rule as p25_channel_type_slots_per_carrier(): types 0-2 are one slot per carrier.
    const int is_tdma = (type & 0xFULL) > 2ULL;
    if (!bp_parse_offset(fields, n, cols, spacing_hz, bw_vu != 0 || is_tdma, &trans_off, why)
        || !bp_parse_system(fields, n, cols, &wacn, &sysid, why)) {
        return 0;
    }

    DSD_MEMSET(row, 0, sizeof *row);
    row->iden = (uint8_t)iden;
    row->is_tdma = (uint8_t)is_tdma;
    row->entry.base_freq = (long int)(base_hz / BP_BASE_UNIT_HZ);
    row->entry.chan_spac = (int)(spacing_hz / BP_SPACING_UNIT_HZ);
    row->entry.chan_type = (int)type;
    row->entry.trans_off = trans_off;
    row->entry.bw_vu = bw_vu;
    row->entry.trust = 1;
    row->entry.populated = 1;
    row->entry.wacn = wacn;
    row->entry.sysid = sysid;
    return 1;
}

static int
bp_find_row(const p25_bandplan_row_t* rows, int count, const p25_bandplan_row_t* row) {
    for (int i = 0; i < count; i++) {
        if (rows[i].iden == row->iden && rows[i].is_tdma == row->is_tdma && rows[i].entry.wacn == row->entry.wacn
            && rows[i].entry.sysid == row->entry.sysid) {
            return i;
        }
    }
    return -1;
}

int
csv_p25_bandplan_parse_file(const char* path, p25_bandplan_row_t* rows, dsd_csv_validation* stats, char* filename,
                            size_t filename_size) {
    FILE* fp = csv_open_user_read_file("P25 band plan file", path, filename, filename_size);
    if (fp == NULL) {
        return -1;
    }

    char buffer[BSIZE];
    bp_header_cols cols;
    bp_header_default(&cols);
    int line_no = 0;
    int count = 0;

    while (fgets(buffer, BSIZE, fp) != NULL) {
        line_no++;
        trim_eol(buffer);
        if (line_no == 1) {
            bp_parse_header(buffer, &cols);
            continue;
        }
        if (csv_line_is_blank(buffer)) {
            continue;
        }
        if (stats) {
            stats->total++;
        }

        char* fields[BP_MAX_FIELDS];
        const size_t n = csv_split_preserve_empty(buffer, fields, BP_MAX_FIELDS);
        p25_bandplan_row_t row;
        const char* why = "";
        if (!bp_parse_row_fields(fields, n, &cols, &row, &why)) {
            if (!stats) {
                LOG_WARN("WARNING: P25 band plan file '%s' row %d: %s; skipping.\n", filename, line_no, why);
            }
            continue;
        }
        const int existing = bp_find_row(rows, count, &row);
        if (existing >= 0) {
            if (!stats) {
                LOG_WARN("WARNING: P25 band plan file '%s' row %d: duplicate identifier, replacing previous row.\n",
                         filename, line_no);
            }
            rows[existing] = row;
        } else if (count >= DSD_P25_BANDPLAN_MAX_ROWS) {
            if (!stats) {
                LOG_WARN("WARNING: P25 band plan file '%s' row %d: more than %d rows; skipping.\n", filename, line_no,
                         DSD_P25_BANDPLAN_MAX_ROWS);
            }
            continue;
        } else {
            rows[count++] = row;
        }
        if (stats) {
            stats->accepted++;
        }
    }
    fclose(fp);
    return count;
}

int
csvP25BandplanImportPath(const char* path, dsd_state* state) {
    if (!state || !path || path[0] == '\0') {
        LOG_ERROR("P25 band plan CSV path is missing.\n");
        return -1;
    }
    char filename[CSV_IMPORT_PATH_MAX] = "filename.csv";
    p25_bandplan_row_t rows[DSD_P25_BANDPLAN_MAX_ROWS];
    DSD_MEMSET(rows, 0, sizeof rows);
    const int count = csv_p25_bandplan_parse_file(path, rows, NULL, filename, sizeof filename);
    if (count < 0) {
        return -1;
    }
    if (count == 0) {
        LOG_ERROR("P25 band plan file '%s' contains no usable rows.\n", filename);
        return -1;
    }
    DSD_MEMSET(state->p25_bandplan_rows, 0, sizeof state->p25_bandplan_rows);
    DSD_MEMCPY(state->p25_bandplan_rows, rows, (size_t)count * sizeof rows[0]);
    state->p25_bandplan_row_count = count;
    const int seeded = dsd_state_p25_bandplan_seed(state);
    LOG_INFO("NOTICE: Loaded %d P25 band plan rows from '%s' (%d identifiers seeded now).\n", count, filename, seeded);
    return 0;
}

int
csvP25BandplanImport(const dsd_opts* opts, dsd_state* state) {
    if (!opts) {
        return -1;
    }
    return csvP25BandplanImportPath(opts->p25_bandplan_in_file, state);
}

static int
bp_write_rows(FILE* fp, const p25_bandplan_row_t* rows, int count) {
    if (DSD_FPRINTF(fp, "iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid\n") < 0) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        const p25_bandplan_row_t* row = &rows[i];
        const p25_iden_entry_t* e = &row->entry;
        const long long base_hz = (long long)e->base_freq * (long long)BP_BASE_UNIT_HZ;
        const long spacing_hz = (long)e->chan_spac * (long)BP_SPACING_UNIT_HZ;
        const long offset_unit = (e->bw_vu != 0 || row->is_tdma) ? spacing_hz : BP_STD_OFFSET_UNIT_HZ;
        const long long offset_hz = (long long)e->trans_off * (long long)offset_unit;
        int n;
        if (e->wacn != 0ULL || e->sysid != 0ULL) {
            n = DSD_FPRINTF(fp, "%u,%lld,%ld,%d,%lld,%ld,%05llX,%03llX\n", (unsigned)row->iden, base_hz, spacing_hz,
                            e->chan_type & 0xF, offset_hz, bp_bw_hz_from_code(e->bw_vu), e->wacn, e->sysid);
        } else {
            n = DSD_FPRINTF(fp, "%u,%lld,%ld,%d,%lld,%ld,,\n", (unsigned)row->iden, base_hz, spacing_hz,
                            e->chan_type & 0xF, offset_hz, bp_bw_hz_from_code(e->bw_vu));
        }
        if (n < 0) {
            return -1;
        }
    }
    return 0;
}

int
csvP25BandplanExportRows(const char* path, const p25_bandplan_row_t* rows, int count) {
    if (!path || path[0] == '\0' || !rows || count <= 0) {
        LOG_ERROR("P25 band plan export: nothing to write.\n");
        return -1;
    }
    char tmp_path[CSV_IMPORT_PATH_MAX];
    FILE* fp = dsd_fopen_private_temp_for_replace(path, tmp_path, sizeof tmp_path, "w");
    if (fp == NULL) {
        LOG_ERROR("P25 band plan export: unable to create '%s'.\n", path);
        return -1;
    }
    int bad = bp_write_rows(fp, rows, count) != 0;
    if (!bad && fflush(fp) != 0) {
        bad = 1;
    }
    const int fd = dsd_fileno(fp);
    if (!bad && fd >= 0 && dsd_fsync(fd) != 0) {
        bad = 1;
    }
    if (fclose(fp) != 0) {
        bad = 1;
    }
    if (!bad && dsd_replace_file_with_temp(tmp_path, path) != 0) {
        bad = 1;
    }
    if (bad) {
        (void)remove(tmp_path);
        LOG_ERROR("P25 band plan export: writing '%s' failed.\n", path);
        return -1;
    }
    LOG_INFO("NOTICE: Wrote %d P25 band plan rows to '%s'.\n", count, path);
    return 0;
}

/* Dry run for the import pickers: same parser, counts only, no state touched. */
int
dsd_csv_validate_p25_bandplan_file(const char* path, dsd_csv_validation* out) {
    if (!path || path[0] == '\0' || !out) {
        return -1;
    }
    out->accepted = 0U;
    out->skipped = 0U;
    out->total = 0U;
    char filename[CSV_IMPORT_PATH_MAX];
    p25_bandplan_row_t rows[DSD_P25_BANDPLAN_MAX_ROWS];
    if (csv_p25_bandplan_parse_file(path, rows, out, filename, sizeof filename) < 0) {
        out->accepted = 0U;
        out->total = 0U;
        return -1;
    }
    out->skipped = out->total - out->accepted;
    return 0;
}
