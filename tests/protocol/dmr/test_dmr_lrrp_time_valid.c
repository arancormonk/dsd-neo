// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate LRRP date handling: when decoded date/time is within range, the
 * LRRP writer should use the decoded timestamp (not system fallback).
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/core/dsd.h>

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

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* str, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)str;
    (void)slot;
}

// Provide deterministic system time stubs (should not be used when decoded time is valid)
void
getTimeC_buf(char out[9]) {
    snprintf(out, 9, "%s", "11:22:33");
}

void
getDateS_buf(char out[11]) {
    snprintf(out, 11, "%s", "1999/01/02");
}

// Under test
void dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU);

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!strstr(buf, needle)) {
        fprintf(stderr, "%s: missing '%s'\n", tag, needle);
        return 1;
    }
    return 0;
}

static int
expect_no_substr(const char* buf, const char* needle, const char* tag) {
    if (strstr(buf, needle)) {
        fprintf(stderr, "%s: found unexpected '%s'\n", tag, needle);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    dsd_opts opts;
    dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    st.currentslot = 0;

    // Prepare output file
    char outtmpl[] = "/tmp/dmr_lrrp_time_valid_XXXXXX";
    int ofd = mkstemp(outtmpl);
    if (ofd < 0) {
        return 100;
    }
    close(ofd);
    snprintf(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", outtmpl);
    opts.lrrp_file_output = 1;

    // Capture stderr to check decoded Time print
    char errtmpl[] = "/tmp/dmr_lrrp_time_valid_err_XXXXXX";
    int efd = mkstemp(errtmpl);
    if (efd < 0) {
        return 101;
    }
    close(efd);
    if (!freopen(errtmpl, "w+", stderr)) {
        return 102;
    }

    // Build LRRP PDU with valid decoded date/time and point-2d
    uint8_t pdu[64];
    int i = 0;
    memset(pdu, 0, sizeof pdu);
    pdu[i++] = 0x07; // response
    pdu[i++] = 24;   // message_len
    pdu[i++] = 0x22; // pattern
    pdu[i++] = 0x00;

    // point-2d (lat/lon)
    pdu[i++] = 0x66;
    pdu[i++] = 0x10;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00; // lat
    pdu[i++] = 0x20;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00;
    pdu[i++] = 0x00; // lon

    // Time token 0x34 encoding: year=2024, month=12, day=1, hour=23, minute=59, second=58
    // b1=0x1F, b2=0xA3 (see decoder formula)
    // b3 constructs day=1 and low bit=1; b4 for hour/min; b5 for minute/second
    pdu[i++] = 0x34;
    pdu[i++] = 0x1F; // year high
    pdu[i++] = 0xA3; // year low with month coarse bits
    pdu[i++] = 0x03; // day=1, low bit=1
    pdu[i++] = 0x7E; // hour: high nibble 0x7 -> 7 + (1<<4)=23; minute low nibble 0xE => 56 base
    pdu[i++] = 0xFA; // minute top bits=3 -> +3 -> 59; seconds=0x3A -> 58

    dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 111, /*dst*/ 222, pdu);

    fflush(stderr);
    // Verify stderr includes decoded Time: 2024.12.01 23:59:58
    FILE* ef = fopen(errtmpl, "rb");
    if (!ef) {
        return 103;
    }
    fseek(ef, 0, SEEK_END);
    long esz = ftell(ef);
    fseek(ef, 0, SEEK_SET);
    size_t pesz = 0;
    if (esz > 0 && (unsigned long)esz <= (unsigned long)(SIZE_MAX - 1)) {
        pesz = (size_t)esz;
    }
    char* ebuf = calloc(pesz + 1u, 1u);
    if (!ebuf) {
        fclose(ef);
        return 105;
    }
    fread(ebuf, 1, pesz, ef);
    fclose(ef);
    rc |= expect_has_substr(ebuf, " Time: 2024.12.01 23:59:58", "stderr has decoded Time");
    free(ebuf);

    // Verify LRRP output starts with decoded timestamp, not system fallback
    FILE* of = fopen(outtmpl, "rb");
    if (!of) {
        return 104;
    }
    fseek(of, 0, SEEK_END);
    long osz = ftell(of);
    fseek(of, 0, SEEK_SET);
    size_t posz = 0;
    if (osz > 0 && (unsigned long)osz <= (unsigned long)(SIZE_MAX - 1)) {
        posz = (size_t)osz;
    }
    char* obuf = calloc(posz + 1u, 1u);
    if (!obuf) {
        fclose(of);
        return 106;
    }
    fread(obuf, 1, posz, of);
    fclose(of);
    rc |= expect_has_substr(obuf, "2024/12/01\t23:59:58\t", "LRRP uses decoded timestamp");
    rc |= expect_no_substr(obuf, "1999/01/02\t11:22:33\t", "LRRP not using system fallback");
    free(obuf);

    remove(outtmpl);
    remove(errtmpl);
    return rc;
}
