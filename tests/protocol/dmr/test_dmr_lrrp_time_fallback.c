// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate LRRP date handling: out-of-range decoded dates should be ignored
 * and system time used instead in the LRRP output file, and the decoded
 * timestamp should not be printed to stderr.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/core/dsd.h>

// Stubs to keep test self-contained and avoid pulling heavy deps
const char*
dsd_degrees_glyph(void) {
    return ""; // no unicode in tests
}

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)data_string;
    (void)slot;
}

void
getTimeC_buf(char out[9]) { // HH:MM:SS
    // Not asserting exact value in this test; but keep deterministic if used
    snprintf(out, 9, "%s", "12:34:56");
}

void
getDateS_buf(char out[11]) { // YYYY/MM/DD
    snprintf(out, 11, "%s", "2001/02/03");
}

// Additional stubs to satisfy dmr_pdu.c when linked directly
int
dsd_unicode_supported(void) {
    return 0;
}

void
unpack_byte_array_into_bit_array(uint8_t* input, uint8_t* output, int len) {
    (void)input;
    if (len > 0) {
        memset(output, 0, (size_t)len);
    }
}

void
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
}

void
decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

// Under test
void dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU);

static int
expect_no_substr(const char* buf, const char* needle, const char* tag) {
    if (strstr(buf, needle)) {
        fprintf(stderr, "%s: found unexpected substring '%s'\n", tag, needle);
        return 1;
    }
    return 0;
}

static int
expect_nonempty(const char* buf, const char* tag) {
    if (!buf || buf[0] == '\0') {
        fprintf(stderr, "%s: got empty output\n", tag);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    // Prepare opts/state
    dsd_opts opts;
    dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    st.currentslot = 0;

    // Temp file to capture LRRP output
    char outtmpl[] = "/tmp/dmr_lrrp_time_fallback_XXXXXX";
    int ofd = mkstemp(outtmpl);
    if (ofd < 0) {
        perror("mkstemp out");
        return 100;
    }
    close(ofd);
    snprintf(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", outtmpl);
    opts.lrrp_file_output = 1;

    // Capture stderr to a temp
    char errtmpl[] = "/tmp/dmr_lrrp_time_stderr_XXXXXX";
    int efd = mkstemp(errtmpl);
    if (efd < 0) {
        perror("mkstemp err");
        return 101;
    }
    close(efd);
    if (!freopen(errtmpl, "w+", stderr)) {
        return 102;
    }

    // Craft minimal LRRP PDU:
    // [0]=0x07 (Immediate Location Response), [1]=len, [2]=0x22, [3]=0x00 (padding)
    // then token 0x66 (point-2d) with lat/lon, then token 0x34 (Time) with invalid year 2038
    uint8_t pdu[64];
    memset(pdu, 0, sizeof pdu);
    int idx = 0;
    pdu[idx++] = 0x07; // response (fmt)
    pdu[idx++] = 24;   // message_len placeholder
    pdu[idx++] = 0x22; // typical pattern in responses
    pdu[idx++] = 0x00;

    // 0x66 point-2d: lat(4) lon(4)
    pdu[idx++] = 0x66;
    // lat = 0x10000000
    pdu[idx++] = 0x10;
    pdu[idx++] = 0x00;
    pdu[idx++] = 0x00;
    pdu[idx++] = 0x00;
    // lon = 0x20000000
    pdu[idx++] = 0x20;
    pdu[idx++] = 0x00;
    pdu[idx++] = 0x00;
    pdu[idx++] = 0x00;

    // 0x34 Time with decoded year=2038 -> invalid -> should be discarded
    // year = (b1<<6) + (b2>>2); choose b1=31 (0x1F), (b2>>2)=54 -> b2=216 (0xD8)
    pdu[idx++] = 0x34;
    pdu[idx++] = 0x1F; // year high
    pdu[idx++] = 0xD8; // year low (>>2 == 54)
    // Remaining bytes for date/time (make them zero/minimal)
    pdu[idx++] = 0x00; // contributes to month/day/hour
    pdu[idx++] = 0x00; // minute
    pdu[idx++] = 0x00; // second

    // Call under test
    dmr_lrrp(&opts, &st, (uint16_t)idx, /*source=*/123, /*dest=*/456, pdu);

    // Flush and read stderr
    fflush(stderr);
    FILE* ef = fopen(errtmpl, "rb");
    if (!ef) {
        return 103;
    }
    fseek(ef, 0, SEEK_END);
    long esize = ftell(ef);
    fseek(ef, 0, SEEK_SET);
    size_t pesize = 0;
    if (esize > 0 && (unsigned long)esize <= (unsigned long)(SIZE_MAX - 1)) {
        pesize = (size_t)esize;
    }
    char* ebuf = calloc(pesize + 1u, 1u);
    if (!ebuf) {
        fclose(ef);
        return 105;
    }
    fread(ebuf, 1, pesize, ef);
    fclose(ef);

    // Ensure decoded time was NOT printed (fallback path)
    rc |= expect_no_substr(ebuf, " Time:", "stderr no decoded Time");
    free(ebuf);

    // Read LRRP output file and check fallback occurred (no bogus year)
    FILE* of = fopen(outtmpl, "rb");
    if (!of) {
        return 104;
    }
    fseek(of, 0, SEEK_END);
    long osize = ftell(of);
    fseek(of, 0, SEEK_SET);
    size_t posize = 0;
    if (osize > 0 && (unsigned long)osize <= (unsigned long)(SIZE_MAX - 1)) {
        posize = (size_t)osize;
    }
    char* obuf = calloc(posize + 1u, 1u);
    if (!obuf) {
        fclose(of);
        return 106;
    }
    fread(obuf, 1, posize, of);
    fclose(of);

    // Ensure the file has some content and does not contain the bogus decoded year "2038/"
    rc |= expect_nonempty(obuf, "LRRP file non-empty");
    rc |= expect_no_substr(obuf, "2038/", "LRRP file excludes bogus decoded year");
    free(obuf);

    // Cleanup
    remove(outtmpl);
    remove(errtmpl);

    return rc;
}
