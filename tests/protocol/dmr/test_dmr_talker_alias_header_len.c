// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

// Minimal stubs needed to link `src/core/util/dsd_alias.c` in isolation.
uint16_t
ComputeCrcCCITT16d(const uint8_t* buf, uint32_t len) {
    (void)buf;
    (void)len;
    return 0;
}

uint64_t
ConvertBitIntoBytes(uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t out = 0;
    uint8_t* p = BufferIn;
    uint32_t n = BitLength;

    while (n--) {
        out = (out << 1) | (uint64_t)(*p++ & 1u);
    }
    return out;
}

int
dsd_unicode_supported(void) {
    return 0;
}

void
p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors) {
    (void)opts;
    (void)state;
    (void)lcw_bits;
    (void)irrecoverable_errors;
}

static void
bytes_to_bits_msb(uint8_t* bits_out, size_t bits_out_sz, const uint8_t* in, size_t in_sz) {
    const size_t need = in_sz * 8u;
    if (bits_out_sz < need) {
        fprintf(stderr, "bytes_to_bits_msb: need=%zu have=%zu\n", need, bits_out_sz);
        exit(2);
    }

    for (size_t i = 0; i < in_sz; i++) {
        for (size_t b = 0; b < 8; b++) {
            bits_out[i * 8u + b] = (uint8_t)((in[i] >> (7u - b)) & 1u);
        }
    }
}

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!buf || !strstr(buf, needle)) {
        fprintf(stderr, "%s: missing '%s' in '%s'\n", tag, needle, buf ? buf : "(null)");
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    if (!opts || !st) {
        free(st);
        free(opts);
        return 100;
    }

    st->event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    if (!st->event_history_s) {
        free(st);
        free(opts);
        return 100;
    }

    st->currentslot = 0;
    st->lastsrc = 123;
    st->event_history_s[0].Event_History_Items[0].source_id = st->lastsrc;

    uint8_t lc_bits[80];
    memset(lc_bits, 0, sizeof lc_bits);

    // FLCO=0x04 (talker alias header), FID=0, SO byte=0x84 (format=2, bad len=2),
    // alias payload bytes are ASCII "KE8NAX".
    const uint8_t payload[] = {0x04, 0x00, 0x84, 0x4B, 0x45, 0x38, 0x4E, 0x41, 0x58};
    bytes_to_bits_msb(lc_bits, sizeof lc_bits, payload, sizeof payload);

    dmr_talker_alias_lc_header(opts, st, 0, lc_bits);
    rc |= expect_has_substr(st->generic_talker_alias[0], "KE8NAX", "talker_alias_header");

    // Capacity guard regression: when group table is full, runtime alias handling
    // must not write past group_array bounds.
    const size_t cap = sizeof(st->group_array) / sizeof(st->group_array[0]);
    st->group_tally = (unsigned int)cap;
    for (size_t i = 0; i < cap; i++) {
        st->group_array[i].groupNumber = (unsigned long)(i + 1u);
    }
    st->lastsrc = 600000u;
    st->late_entry_mi_fragment[0][0][0] = 0x1122334455667788ULL;

    uint8_t tait_bits[64];
    memset(tait_bits, 0, sizeof tait_bits);
    tait_iso7_embedded_alias_decode(opts, st, 0, 1, tait_bits);

    if (st->group_tally != cap) {
        fprintf(stderr, "group capacity guard failed: tally changed (%u)\n", st->group_tally);
        rc = 1;
    }
    if (st->late_entry_mi_fragment[0][0][0] != 0x1122334455667788ULL) {
        fprintf(stderr, "group capacity guard failed: sentinel changed\n");
        rc = 1;
    }

    free(st->event_history_s);
    st->event_history_s = NULL;
    free(st);
    free(opts);

    return rc;
}
