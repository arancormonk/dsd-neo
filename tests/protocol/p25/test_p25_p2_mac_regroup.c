// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC regroup commands and patch state.
 *
 * Motorola Group Regroup Add (0x81) and Delete (0x89) carry opcode, MFID, a length octet that counts the whole
 * message, the supergroup (SG) and then two-octet work groups (WG): (len - 5) / 2 complete pairs. A message may not
 * run past the MAC octets its transport carries: FACCH 17, SACCH 20, LCCH 18 (after the MAC header, before the CRC and
 * the LCCH's NAC). Bytes past the span here are nonzero so a parser that reads them stores a member.
 *
 * Under -F a MAC_SIGNAL that failed its CRC still reaches the VPDU decoder (process_MAC_VPDU_crc() with crc_ok 0).
 * It prints, but changes no patch: no member, KEY context, cancel or supergroup refresh.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// Stubs referenced by the MAC VPDU path
void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

enum { XCH_FACCH, XCH_SACCH, XCH_LCCH };

#define SG_A  1400
#define SG_B  1280 /* low octet 0: a short message's next segment is null fill */
#define SG_C  1500
#define WG(n) (1400 + (n))

static dsd_opts g_opts;

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static dsd_state*
new_state(void) {
    return (dsd_state*)calloc(1, sizeof(dsd_state));
}

static void
free_state(dsd_state* state) {
    if (state) {
        dsd_state_ext_free_all(state);
        free(state);
    }
}

static void
put16(unsigned char* mac, int pos, int value) {
    mac[pos] = (unsigned char)((value >> 8) & 0xFF);
    mac[pos + 1] = (unsigned char)(value & 0xFF);
}

static void
run_pdu(dsd_state* state, int xch, p25_mac_pdu_type pdu_type, const unsigned char mac_bytes[24], int crc_ok) {
    unsigned long long int mac[24] = {0};
    for (int i = 0; i < 24; i++) {
        mac[i] = mac_bytes[i];
    }
    state->p2_is_lcch = (xch == XCH_LCCH) ? 1 : 0;
    process_MAC_VPDU_crc(&g_opts, state, (xch == XCH_FACCH) ? 0 : 1, pdu_type, mac, crc_ok);
}

static void
run_signal(dsd_state* state, int xch, const unsigned char mac_bytes[24]) {
    run_pdu(state, xch, (xch == XCH_FACCH) ? P25_MAC_PDU_ACTIVE : P25_MAC_PDU_SIGNAL, mac_bytes, 1);
}

static int
find_sg(const dsd_state* state, int sg) {
    for (int i = 0; i < state->p25_patch_count; i++) {
        if (state->p25_patch_sgid[i] == (uint16_t)sg) {
            return i;
        }
    }
    return -1;
}

static int
wg_count(const dsd_state* state, int sg) {
    const int idx = find_sg(state, sg);
    return (idx < 0) ? -1 : (int)state->p25_patch_wgid_count[idx];
}

static int
has_wg(const dsd_state* state, int sg, int wg) {
    const int idx = find_sg(state, sg);
    if (idx < 0) {
        return 0;
    }
    for (int i = 0; i < state->p25_patch_wgid_count[idx]; i++) {
        if (state->p25_patch_wgid[idx][i] == (uint16_t)wg) {
            return 1;
        }
    }
    return 0;
}

/* Opcode, MFID 0x90, length, SG at octets 1..5 of a message starting at MAC[pos]. */
static void
put_moto_regroup(unsigned char* mac, int pos, unsigned char opcode, int len, int sg) {
    mac[pos] = opcode;
    mac[pos + 1] = 0x90;
    mac[pos + 2] = (unsigned char)len;
    put16(mac, pos + 3, sg);
}

/* An Add of len 7 carries one member; the 0x83 message after it must not be read as a second. */
static int
test_add_one_member_then_next_message(void) {
    unsigned char mac[24] = {0};
    int rc = 0;
    dsd_state* state = new_state();

    put_moto_regroup(mac, 1, 0x81, 7, SG_A);
    put16(mac, 6, WG(1));
    mac[8] = 0x83; // Motorola Group Regroup Voice Channel Update (7 octets)
    mac[9] = 0x90;
    put16(mac, 10, 0x2001);
    put16(mac, 12, SG_C);
    run_signal(state, XCH_SACCH, mac);

    rc |= expect_int("one member: count", wg_count(state, SG_A), 1);
    rc |= expect_int("one member: WG stored", has_wg(state, SG_A, WG(1)), 1);
    rc |= expect_int("one member: next opcode not a member", has_wg(state, SG_A, 0x8390), 0);
    free_state(state);
    return rc;
}

/* A length under the 5-octet header names no SG at all; exactly 5 names an SG with no members. Neither creates one. */
static int
test_add_short_and_empty(void) {
    int rc = 0;
    /* len 1 is left out: its next segment starts on the MFID and reads as a standard regroup voice user for SG_B. */
    static const int lens[] = {0, 2, 3, 4, 5};
    for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        const int len = lens[li];
        unsigned char mac[24] = {0};
        char tag[64];
        dsd_state* state = new_state();
        put_moto_regroup(mac, 1, 0x81, len, SG_B);
        if (len < 5) {
            put16(mac, 6, SG_B + 1); // past the message: a parser reading it stores a member
        }
        run_signal(state, XCH_SACCH, mac);
        DSD_SNPRINTF(tag, sizeof tag, "add len %d creates no patch", len);
        rc |= expect_int(tag, state->p25_patch_count, 0);
        free_state(state);
    }
    return rc;
}

/* One transport: an Add that fills the span exactly stores every pair; one octet longer stores nothing. */
static int
check_span_case(const char* name, int xch, int span) {
    int rc = 0;
    char tag[96];

    for (int over = 0; over <= 1; over++) {
        unsigned char mac[24] = {0};
        const int len = span + over;
        const int members = (span - 5) / 2;
        dsd_state* state = new_state();

        put_moto_regroup(mac, 1, 0x81, len, SG_A);
        for (int m = 0; m < members; m++) {
            put16(mac, 6 + (m * 2), WG(m + 1));
        }
        for (int pos = 6 + (members * 2); pos < 24; pos++) {
            mac[pos] = (unsigned char)(0x0A + pos); // odd octet, CRC, NAC: never members
        }
        run_signal(state, xch, mac);

        if (over == 0) {
            DSD_SNPRINTF(tag, sizeof tag, "%s exact fit: member count", name);
            rc |= expect_int(tag, wg_count(state, SG_A), members);
            DSD_SNPRINTF(tag, sizeof tag, "%s exact fit: last member", name);
            rc |= expect_int(tag, has_wg(state, SG_A, WG(members)), 1);
        } else {
            DSD_SNPRINTF(tag, sizeof tag, "%s overflow: no patch", name);
            rc |= expect_int(tag, state->p25_patch_count, 0);
        }
        free_state(state);
    }
    return rc;
}

static int
test_transport_spans(void) {
    int rc = 0;
    rc |= check_span_case("FACCH", XCH_FACCH, 17);
    rc |= check_span_case("SACCH", XCH_SACCH, 20);
    rc |= check_span_case("LCCH", XCH_LCCH, 18);
    return rc;
}

/* len 8: one complete pair and an odd trailing octet, which is not half of a second member. */
static int
test_add_odd_trailing_octet(void) {
    unsigned char mac[24] = {0};
    int rc = 0;
    dsd_state* state = new_state();

    put_moto_regroup(mac, 1, 0x81, 8, SG_A);
    put16(mac, 6, WG(1));
    mac[8] = 0x05;
    run_signal(state, XCH_SACCH, mac);

    rc |= expect_int("odd octet: count", wg_count(state, SG_A), 1);
    rc |= expect_int("odd octet: not a member", has_wg(state, SG_A, 0x0500), 0);
    free_state(state);
    return rc;
}

static void
seed_patch(dsd_state* state, int sg, int wg1, int wg2) {
    p25_patch_add_wgid(state, sg, wg1);
    if (wg2 > 0) {
        p25_patch_add_wgid(state, sg, wg2);
    }
    p25_patch_update(state, sg, /*is_patch*/ 1, /*active*/ 1);
}

/* An Add and a Delete in one PDU: each reads only its own members. */
static int
test_adjacent_add_delete(void) {
    unsigned char mac[24] = {0};
    int rc = 0;
    dsd_state* state = new_state();
    seed_patch(state, SG_A, WG(2), 0);

    put_moto_regroup(mac, 1, 0x81, 7, SG_A);
    put16(mac, 6, WG(1));
    put_moto_regroup(mac, 8, 0x89, 7, SG_A);
    put16(mac, 13, WG(2));
    run_signal(state, XCH_SACCH, mac);

    rc |= expect_int("adjacent: added", has_wg(state, SG_A, WG(1)), 1);
    rc |= expect_int("adjacent: deleted", has_wg(state, SG_A, WG(2)), 0);
    rc |= expect_int("adjacent: delete header not a member", has_wg(state, SG_A, 0x8990), 0);
    rc |= expect_int("adjacent: count", wg_count(state, SG_A), 1);
    free_state(state);
    return rc;
}

/* A Delete shorter than its header removes nothing. */
static int
test_delete_short(void) {
    unsigned char mac[24] = {0};
    int rc = 0;
    dsd_state* state = new_state();
    seed_patch(state, SG_B, SG_B + 1, 0);

    put_moto_regroup(mac, 1, 0x89, 4, SG_B);
    put16(mac, 6, SG_B + 1);
    run_signal(state, XCH_SACCH, mac);

    rc |= expect_int("short delete keeps member", has_wg(state, SG_B, SG_B + 1), 1);
    free_state(state);
    return rc;
}

/* The patch fields a failed-CRC PDU must leave alone. */
static int
patch_unchanged(const dsd_state* a, const dsd_state* b) {
    return a->p25_patch_count == b->p25_patch_count
           && memcmp(a->p25_patch_sgid, b->p25_patch_sgid, sizeof a->p25_patch_sgid) == 0
           && memcmp(a->p25_patch_is_patch, b->p25_patch_is_patch, sizeof a->p25_patch_is_patch) == 0
           && memcmp(a->p25_patch_active, b->p25_patch_active, sizeof a->p25_patch_active) == 0
           && memcmp(a->p25_patch_last_update, b->p25_patch_last_update, sizeof a->p25_patch_last_update) == 0
           && memcmp(a->p25_patch_wgid_count, b->p25_patch_wgid_count, sizeof a->p25_patch_wgid_count) == 0
           && memcmp(a->p25_patch_wgid, b->p25_patch_wgid, sizeof a->p25_patch_wgid) == 0
           && memcmp(a->p25_patch_wuid_count, b->p25_patch_wuid_count, sizeof a->p25_patch_wuid_count) == 0
           && memcmp(a->p25_patch_wuid, b->p25_patch_wuid, sizeof a->p25_patch_wuid) == 0
           && memcmp(a->p25_patch_key, b->p25_patch_key, sizeof a->p25_patch_key) == 0
           && memcmp(a->p25_patch_alg, b->p25_patch_alg, sizeof a->p25_patch_alg) == 0
           && memcmp(a->p25_patch_ssn, b->p25_patch_ssn, sizeof a->p25_patch_ssn) == 0
           && memcmp(a->p25_patch_key_valid, b->p25_patch_key_valid, sizeof a->p25_patch_key_valid) == 0;
}

typedef void (*mac_builder)(unsigned char mac[24]);

static void
build_failed_add(unsigned char mac[24]) {
    put_moto_regroup(mac, 1, 0x81, 9, SG_A);
    put16(mac, 6, WG(3));
    put16(mac, 8, SG_C); // a second, new patch would need SG_C's own Add
}

static void
build_failed_delete(unsigned char mac[24]) {
    put_moto_regroup(mac, 1, 0x89, 7, SG_A);
    put16(mac, 6, WG(1));
}

/* Harris Group Regroup Explicit Encryption Command: WGID form, active patch, SSN 5, KEY 0000, ALG 80. */
static void
build_failed_kas(unsigned char mac[24]) {
    mac[1] = 0xB0;
    mac[2] = 0xA4;
    mac[3] = 11;
    mac[4] = (unsigned char)((0x3 << 5) | 5);
    put16(mac, 5, SG_A);
    put16(mac, 7, 0x0000);
    mac[9] = 0x80;
    put16(mac, 10, WG(4));
}

/* Motorola Group Regroup Extended Function: class 02 operand 01 cancels the supergroup. */
static void
build_failed_cancel(unsigned char mac[24]) {
    mac[1] = 0x84;
    mac[2] = 0x90;
    mac[3] = 11;
    mac[4] = 0x02;
    mac[5] = 0x01;
    put16(mac, 7, SG_A);
    mac[11] = 0x01;
}

/* Motorola Group Regroup Extended Function: class 02 operand 00 creates a supergroup for a radio. */
static void
build_failed_create(unsigned char mac[24]) {
    mac[1] = 0x84;
    mac[2] = 0x90;
    mac[3] = 11;
    mac[4] = 0x02;
    mac[5] = 0x00;
    put16(mac, 7, SG_C);
    mac[11] = 0x07;
}

/* Supergroup-only voice: Motorola abbreviated (0x80), Motorola extended (0xA0), standard abbreviated (0x90). */
static void
build_failed_moto_voice(unsigned char mac[24]) {
    mac[1] = 0x80;
    mac[2] = 0x90;
    put16(mac, 4, SG_A);
    mac[8] = 0x22;
}

static void
build_failed_moto_voice_ext(unsigned char mac[24]) {
    mac[1] = 0xA0;
    mac[2] = 0x90;
    put16(mac, 5, SG_A);
    mac[9] = 0x22;
}

static void
build_failed_std_voice(unsigned char mac[24]) {
    mac[1] = 0x90;
    mac[2] = 0x00;
    put16(mac, 3, SG_A);
    mac[7] = 0x22;
}

/* SG_A patched to WG 1401 and 1402, inactive and five seconds old, so a refresh, an add, a delete, KEY context or a
   cancel each shows. */
static dsd_state*
failed_crc_seed(void) {
    dsd_state* state = new_state();
    if (!state) {
        return NULL;
    }
    seed_patch(state, SG_A, WG(1), WG(2));
    const int idx = find_sg(state, SG_A);
    if (idx >= 0) {
        state->p25_patch_active[idx] = 0;
        state->p25_patch_last_update[idx] = dsd_decode_time() - 5;
    }
    return state;
}

static int
check_failed_crc_case(const char* name, mac_builder build) {
    int rc = 0;
    char tag[96];
    unsigned char mac[24] = {0};
    build(mac);

    for (int crc_ok = 0; crc_ok <= 1; crc_ok++) {
        dsd_state* state = failed_crc_seed();
        dsd_state* before = new_state();
        if (!state || !before) {
            free_state(state);
            free(before);
            return 1;
        }
        DSD_MEMCPY(before, state, sizeof(*state));
        run_pdu(state, XCH_LCCH, P25_MAC_PDU_SIGNAL, mac, crc_ok);
        if (crc_ok == 0) {
            DSD_SNPRINTF(tag, sizeof tag, "%s: failed CRC changes no patch", name);
            rc |= expect_int(tag, patch_unchanged(before, state), 1);
        } else {
            DSD_SNPRINTF(tag, sizeof tag, "%s: CRC-good PDU applies (control)", name);
            rc |= expect_int(tag, patch_unchanged(before, state), 0);
        }
        free(before);
        free_state(state);
    }
    return rc;
}

static int
test_failed_crc_changes_no_patch(void) {
    int rc = 0;
    rc |= check_failed_crc_case("moto add", build_failed_add);
    rc |= check_failed_crc_case("moto delete", build_failed_delete);
    rc |= check_failed_crc_case("harris kas", build_failed_kas);
    rc |= check_failed_crc_case("moto cancel supergroup", build_failed_cancel);
    rc |= check_failed_crc_case("moto create supergroup", build_failed_create);
    rc |= check_failed_crc_case("moto sg voice", build_failed_moto_voice);
    rc |= check_failed_crc_case("moto sg voice extended", build_failed_moto_voice_ext);
    rc |= check_failed_crc_case("standard sg voice", build_failed_std_voice);
    return rc;
}

static int
run_cases(void) {
    int rc = 0;
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    g_opts.aggressive_framesync = 0; /* -F */
    rc |= test_add_one_member_then_next_message();
    rc |= test_add_short_and_empty();
    rc |= test_transport_spans();
    rc |= test_add_odd_trailing_octet();
    rc |= test_adjacent_add_delete();
    rc |= test_delete_short();
    rc |= test_failed_crc_changes_no_patch();
    return rc;
}

static int
expect_file_contains(const char* tag, const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        DSD_FPRINTF(stderr, "%s: cannot open %s\n", tag, path);
        return 1;
    }
    char line[1024];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        found = strstr(line, needle) != NULL;
    }
    fclose(f);
    if (!found) {
        DSD_FPRINTF(stderr, "%s: missing '%s'\n", tag, needle);
        return 1;
    }
    return 0;
}

int
main(void) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_p2_mac_regroup") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 101;
    }
    int rc = run_cases();
    dsd_test_capture_stderr_end(&cap);
    if (rc != 0) {
        FILE* diagnostics = fopen(cap.path, "rb");
        if (diagnostics) {
            char line[512];
            while (fgets(line, sizeof line, diagnostics)) {
                DSD_FPRINTF(stderr, "%s", line);
            }
            fclose(diagnostics);
        }
    }
    rc |= expect_file_contains("failed CRC add still prints", cap.path, "Group Regroup Add Command");
    rc |= expect_file_contains("failed CRC note", cap.path, "(CRC failed; patch not applied)");
    (void)remove(cap.path);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
