// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Converter for tests/iq_float_symbol_replay_check.cmake: the soft symbol capture a replay wrote with -c, as the float
 * symbol file (.raw, .sym) a later run replays with -i.
 *
 *   dsd-neo_test_symbol_capture_to_float
 *
 * Run in the test's work directory. Only the two fixed files capture.bin (read) and symbols.raw (written) are used.
 *
 * capture.bin starts with the soft capture header (DSD_SYMBOL_CAPTURE_SOFT_MAGIC, version 2, record size 10) and holds
 * one record per symbol, the symbol the decoder measured in its last four bytes as a little-endian float. symbols.raw
 * gets those four bytes of every record, in order: a float symbol file of the same symbols. Fails on another header, a
 * partial record or no record at all.
 */

#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"

enum { SYMBOL_BYTES = 4, SYMBOL_OFFSET = DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE - SYMBOL_BYTES };

static int
capture_header_ok(FILE* in) {
    unsigned char header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), in) != sizeof(header)) {
        return 0;
    }
    return memcmp(header, DSD_SYMBOL_CAPTURE_SOFT_MAGIC, 8) == 0 && header[8] == 2
           && header[9] == DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE;
}

/* Copy the symbol of every record; the number copied, or -1 on a partial record or a write error. */
static long
copy_symbols(FILE* in, FILE* out) {
    unsigned char record[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    long count = 0;
    for (;;) {
        const size_t got = fread(record, 1, sizeof(record), in);
        if (got == 0U && feof(in)) {
            return count;
        }
        if (got != sizeof(record)) {
            return -1;
        }
        if (fwrite(record + SYMBOL_OFFSET, 1, SYMBOL_BYTES, out) != SYMBOL_BYTES) {
            return -1;
        }
        count++;
    }
}

int
main(void) {
    FILE* in = dsd_fopen_existing_regular_file("capture.bin", "rb");
    if (!in) {
        DSD_FPRINTF(stderr, "symbol capture to float: cannot open capture.bin\n");
        return 1;
    }
    if (!capture_header_ok(in)) {
        DSD_FPRINTF(stderr, "symbol capture to float: capture.bin is no soft symbol capture\n");
        (void)fclose(in);
        return 1;
    }
    FILE* out = dsd_fopen_private("symbols.raw", "wb");
    if (!out) {
        DSD_FPRINTF(stderr, "symbol capture to float: cannot create symbols.raw\n");
        (void)fclose(in);
        return 1;
    }
    const long count = copy_symbols(in, out);
    const int closed = fclose(out);
    (void)fclose(in);
    if (count <= 0 || closed != 0) {
        DSD_FPRINTF(stderr, "symbol capture to float: %s\n",
                    count < 0 || closed != 0 ? "a partial record or a write error" : "no symbol in capture.bin");
        return 1;
    }
    DSD_FPRINTF(stdout, "symbol capture to float: %ld symbols\n", count);
    return 0;
}
