// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The boundary between a Phase 1 TSBK and the Phase 2 MAC messages it is decoded with.
 *
 * The TSBK decoder hands each CRC-valid TSBK to the MAC message handlers in MAC form
 * (process_MAC_VPDU_bridged_p1()): one 9-octet message, opcode at MAC[1] and TSBK octet n at MAC[n]. It must decode as
 * exactly that one message, with the Phase 1 field layout (TIA-102.AABC-D), never split by the Phase 2 length table
 * into a second message made of its trailing octets. A native Phase 2 PDU keeps its own layout and its segmentation
 * (TIA-102.BBAD-A message lengths), whatever its header or a previous message's last octet holds.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

enum { CAPTURE_BYTES = 16384 };

static char g_out[CAPTURE_BYTES];

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} fixture;

static int
fixture_open(fixture* f) {
    f->opts = (dsd_opts*)calloc(1, sizeof(*f->opts));
    f->state = (dsd_state*)calloc(1, sizeof(*f->state));
    if (f->opts == NULL || f->state == NULL) {
        free(f->opts);
        free(f->state);
        return -1;
    }
    /* A known site the bridged messages must leave alone. */
    f->state->p2_wacn = 0xBEE00;
    f->state->p2_sysid = 0x123;
    f->state->p2_rfssid = 3;
    f->state->p2_siteid = 5;
    f->state->p25_site_lra = 0x11;
    f->state->p25_site_lra_valid = 1;
    return 0;
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
}

/* What the TSBK decoder hands over for TSBK octets 0-9 (p25p1_tsbk.c tsbk_build_mac_like_pdu(); P25_P1_TSBK_HANDLERS
   test_dispatch_hands_mac_form_to_bridged_entry() pins that form). Octets past the TSBK hold junk here: nothing may
   read them. */
static void
tsbk_mac_form(const uint8_t tsbk[10], unsigned long long int mac[24]) {
    for (int i = 0; i < 24; i++) {
        mac[i] = 0xA5U;
    }
    mac[0] = 0x07;
    mac[1] = (unsigned long long int)((tsbk[0] & 0x3FU) ^ 0x40U);
    for (int i = 2; i <= 9; i++) {
        mac[i] = tsbk[i];
    }
}

static int
capture_begin(dsd_test_capture_stderr* cap) {
    if (dsd_test_capture_stderr_begin(cap, "p25_tsbk_bridge") != 0) {
        DSD_FPRINTF(stderr, "capture failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void
capture_end(dsd_test_capture_stderr* cap) {
    dsd_test_capture_stderr_end(cap);
    (void)dsd_test_capture_stderr_read(cap, g_out, sizeof g_out);
}

/* Decode one TSBK the way the TSBK decoder does, capturing what it prints. */
static int
run_tsbk(fixture* f, const uint8_t tsbk[10]) {
    unsigned long long int mac[24];
    dsd_test_capture_stderr cap;
    tsbk_mac_form(tsbk, mac);
    f->state->synctype = DSD_SYNC_P25P1_POS;
    if (capture_begin(&cap) != 0) {
        return -1;
    }
    process_MAC_VPDU_bridged_p1(f->opts, f->state, mac);
    capture_end(&cap);
    return 0;
}

/* Decode a native Phase 2 SACCH MAC PDU (header at mac[0]) received with sync type @p synctype. */
static int
run_native_sacch_sync(fixture* f, const uint8_t* octets, int count, int synctype) {
    unsigned long long int mac[24] = {0};
    dsd_test_capture_stderr cap;
    for (int i = 0; i < count && i < 24; i++) {
        mac[i] = octets[i];
    }
    f->state->synctype = synctype;
    if (capture_begin(&cap) != 0) {
        return -1;
    }
    process_MAC_VPDU(f->opts, f->state, 1, P25_MAC_PDU_SIGNAL, mac);
    capture_end(&cap);
    return 0;
}

static int
run_native_sacch(fixture* f, const uint8_t* octets, int count) {
    return run_native_sacch_sync(f, octets, count, DSD_SYNC_P25P2_POS);
}

/* Messages printed. This relies on the handlers' print convention: a message opens with its title one space in
   ("\n <Name>") and indents its fields two spaces ("\n  Target [..]"). A future one-space line that is no title would
   count as a message here. */
static int
message_count(const char* out) {
    int count = 0;
    for (const char* p = out; (p = strstr(p, "\n ")) != NULL; p += 2) {
        if (p[2] != ' ' && p[2] != '\n' && p[2] != '\0') {
            count++;
        }
    }
    return count;
}

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n--- output ---%s\n---\n", tag, got, want, g_out);
        return 1;
    }
    return 0;
}

static int
expect_has(const char* tag, const char* needle) {
    if (strstr(g_out, needle) == NULL) {
        DSD_FPRINTF(stderr, "%s: missing '%s'\n--- output ---%s\n---\n", tag, needle, g_out);
        return 1;
    }
    return 0;
}

static int
expect_lacks(const char* tag, const char* needle) {
    if (strstr(g_out, needle) != NULL) {
        DSD_FPRINTF(stderr, "%s: unexpected '%s'\n--- output ---%s\n---\n", tag, needle, g_out);
        return 1;
    }
    return 0;
}

static int
expect_site_untouched(const char* tag, const dsd_state* state) {
    int rc = 0;
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "%s wacn", tag);
    rc |= expect_int(label, (long)state->p2_wacn, 0xBEE00);
    DSD_SNPRINTF(label, sizeof label, "%s sysid", tag);
    rc |= expect_int(label, (long)state->p2_sysid, 0x123);
    DSD_SNPRINTF(label, sizeof label, "%s rfss", tag);
    rc |= expect_int(label, (long)state->p2_rfssid, 3);
    DSD_SNPRINTF(label, sizeof label, "%s site", tag);
    rc |= expect_int(label, (long)state->p2_siteid, 5);
    DSD_SNPRINTF(label, sizeof label, "%s lra", tag);
    rc |= expect_int(label, (long)state->p25_site_lra, 0x11);
    return rc;
}

/* A Call Alert's source octets used to split off as a second message: source 0x017A22 made an RFSS Status
   Broadcast that set RFSS/site to 0/0 and the LRA to 0x22. Every middle octet of the source decodes as the one Call
   Alert and nothing else. */
static int
test_call_alert_bridge_no_second_message(void) {
    int rc = 0;
    for (int vv = 0; vv <= 0xFF; vv++) {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        const uint8_t tsbk[10] = {0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x01, (uint8_t)vv, 0x22};
        char tag[64];
        char want[64];
        rc |= run_tsbk(&f, tsbk) != 0;
        DSD_SNPRINTF(tag, sizeof tag, "call alert src 01%02X22", vv);
        rc |= expect_int(tag, message_count(g_out), 1);
        rc |= expect_has(tag, "Call Alert - Abbreviated");
        DSD_SNPRINTF(want, sizeof want, "Target [100] Source [%d]", 0x010022 | (vv << 8));
        rc |= expect_has(tag, want);
        rc |= expect_site_untouched(tag, f.state);
        rc |= expect_int(tag, f.state->p25_aff_count, 0);
        fixture_close(&f);
        if (rc != 0) {
            break;
        }
    }
    return rc;
}

/* Each Phase 1 OSP shorter than the TSBK's 8 payload octets in the Phase 2 length table (Telephone Interconnect
   Answer Request, SNDCP Data Page Request, Status Query, Radio Unit Monitor Command, Call Alert, Group Affiliation
   Query, Unit Registration Command) decodes as exactly one message, whatever its last octets hold. */
static int
test_bridge_short_opcodes_single_structure(void) {
    static const uint8_t opcodes[] = {0x0A, 0x15, 0x1A, 0x1D, 0x1F, 0x2A, 0x2D};
    int rc = 0;
    for (size_t o = 0; o < sizeof opcodes && rc == 0; o++) {
        for (int v = 0; v <= 0xFF && rc == 0; v++) {
            fixture f;
            if (fixture_open(&f) != 0) {
                return 1;
            }
            uint8_t tsbk[10] = {opcodes[o], 0x00, 0x12, 0x34, 0x56, 0x01, 0x23, 0x45, (uint8_t)v, (uint8_t)v};
            char tag[64];
            rc |= run_tsbk(&f, tsbk) != 0;
            DSD_SNPRINTF(tag, sizeof tag, "tsbk %02X tail %02X", opcodes[o], v);
            rc |= expect_int(tag, message_count(g_out), 1);
            rc |= expect_site_untouched(tag, f.state);
            fixture_close(&f);
        }
    }
    return rc;
}

/* A native Call Alert from source 7, then a Unit Registration Response: the second message's preceding octet is 0x07,
   which once read it as a Phase 1 TSBK and registered RID 13434898. Only its Source Address 4660 registers. */
static int
test_source_ending_07_no_misfire(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    static const uint8_t pdu[] = {0x00, 0x5F, 0x00, 0x00, 0x01, 0x00, 0x00, 0x07, 0x6C,
                                  0x0A, 0x03, 0xA5, 0x00, 0xAB, 0xCD, 0x00, 0x12, 0x34};
    rc |= run_native_sacch(&f, pdu, (int)sizeof pdu) != 0;
    rc |= expect_has("07 tail call alert", "Target [1] Source [7]");
    rc |= expect_has("07 tail registration source", "SRC_ID: 43981 SRC: 4660");
    rc |= expect_int("07 tail aff count", f.state->p25_aff_count, 1);
    rc |= expect_int("07 tail aff rid", (long)f.state->p25_aff_rid[0], 4660);
    fixture_close(&f);
    return rc;
}

/* Phase 1 Unit-to-Unit Answer Request (TIA-102.AABC-D §4.2.4.1): Service Options octet 2, octet 3 reserved, Target
   Address octets 4-6, Source ID octets 7-9. */
static int
test_p1_0x05_layout(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    static const uint8_t tsbk[10] = {0x05, 0x00, 0x80, 0x00, 0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF};
    rc |= run_tsbk(&f, tsbk) != 0;
    rc |= expect_int("p1 05 messages", message_count(g_out), 1);
    rc |= expect_has("p1 05 label", "Unit-to-Unit Answer Request - Abbreviated");
    rc |= expect_has("p1 05 fields", "SVC [80] Target [1193046] Source [11259375]");
    fixture_close(&f);
    return rc;
}

/* Phase 2 Unit to Unit Answer Request is 8 octets abbreviated, 12 extended (TIA-102.BBAD-A §3.10 Figs 36-37 and its
   message length table): a message after it starts where it ends. */
static int
test_p2_0x45_len8_0xc5_len12(void) {
    int rc = 0;
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t pdu[] = {0x00, 0x45, 0x80, 0x00, 0x30, 0x39, 0x01, 0x09,
                                      0x32, 0x5F, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
        rc |= run_native_sacch(&f, pdu, (int)sizeof pdu) != 0;
        rc |= expect_int("0x45 messages", message_count(g_out), 2);
        rc |= expect_has("0x45 fields", "SVC [80] Target [12345] Source [67890]");
        rc |= expect_has("0x45 next message", "Call Alert - Abbreviated\n  Target [100] Source [200]");
        fixture_close(&f);
    }
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t pdu[] = {0x00, 0xC5, 0x00, 0x00, 0x30, 0x39, 0xBE, 0xE0, 0x01, 0x23,
                                      0x01, 0x09, 0x32, 0x5F, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
        rc |= run_native_sacch(&f, pdu, (int)sizeof pdu) != 0;
        rc |= expect_int("0xC5 messages", message_count(g_out), 2);
        rc |= expect_has("0xC5 fields", "Target [12345] Source [BEE00:123.67890]");
        rc |= expect_has("0xC5 next message", "Call Alert - Abbreviated\n  Target [100] Source [200]");
        fixture_close(&f);
    }
    return rc;
}

/* Telephone Interconnect Answer Request: ten digits in octets 2-6 and a 24-bit Target Address in octets 7-9, 9
   octets long, as Phase 2 0x4A (TIA-102.BBAD-A §3.12 Fig 42) and as Phase 1 TSBK 0x0A (TIA-102.AABC-D §4.2.7). */
static int
test_0x4a_target_24bit(void) {
    int rc = 0;
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t pdu[] = {0x00, 0x4A, 0x55, 0x51, 0x23, 0x45, 0x67, 0x12, 0xD6,
                                      0x87, 0x5F, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
        rc |= run_native_sacch(&f, pdu, (int)sizeof pdu) != 0;
        rc |= expect_int("0x4A messages", message_count(g_out), 2);
        rc |= expect_has("0x4A fields", "Target [1234567] Digits [5551234567]");
        rc |= expect_has("0x4A next message", "Call Alert - Abbreviated\n  Target [100] Source [200]");
        fixture_close(&f);
    }
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t tsbk[10] = {0x0A, 0x00, 0x55, 0x51, 0x23, 0x45, 0x67, 0x12, 0xD6, 0x87};
        rc |= run_tsbk(&f, tsbk) != 0;
        rc |= expect_int("p1 0A messages", message_count(g_out), 1);
        rc |= expect_has("p1 0A fields", "Target [1234567] Digits [5551234567]");
        fixture_close(&f);
    }
    return rc;
}

/* TSBK 0x1E: Radio Unit Monitor Enhanced Command is a multi-block message in Phase 1 (TIA-102.AABC-D §6.2.34), so a
   single TSBK with that opcode has no layout to decode. It prints its own octets raw; the Phase 2 0x5E handler, which
   reads through octet 14, does not run, and nothing past octet 9 shows. */
static int
test_0x1e_bridged_no_overread(void) {
    fixture f;
    int rc = 0;
    if (fixture_open(&f) != 0) {
        return 1;
    }
    static const uint8_t tsbk[10] = {0x1E, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    rc |= run_tsbk(&f, tsbk) != 0;
    rc |= expect_int("p1 1E messages", message_count(g_out), 1);
    rc |= expect_has("p1 1E raw", "Phase 1 TSBK 1E Radio Unit Monitor Enhanced Command - not decoded");
    rc |= expect_has("p1 1E octets", "Octets 2-9 [01 02 03 04 05 06 07 08]");
    rc |= expect_lacks("p1 1E no P2 handler", "ALG [");
    rc |= expect_lacks("p1 1E no octet 10", "A5");
    dsd_recent_activity_snapshot recent;
    DSD_MEMSET(&recent, 0, sizeof recent);
    rc |= expect_int("p1 1E no activity", dsd_recent_activity_copy_snapshot(f.state, &recent) > 0, 0);
    fixture_close(&f);
    return rc;
}

/* Other TSBKs without a single-block Phase 1 layout print raw: reserved 0x01 and 0x0C, obsolete 0x12 and 0x13 (the
   TIA-102.AABC-D OSP list), multi-block-only 0x31 and 0x37. None runs the Phase 2 handler of its MAC opcode: 0x01 is
   no Group Voice Service Request (MAC 0x41), so it adds no group affiliation row, 0x12 / 0x13 no SNDCP request or
   response, and TSBK 0x31 (Authentication Demand, §6.2.30) never enters the multi-fragment assembler MAC 0x71 names. */
static int
test_bridged_without_p1_layout_prints_raw(void) {
    static const uint8_t opcodes[] = {0x01, 0x0C, 0x12, 0x13, 0x31, 0x37};
    int rc = 0;
    for (size_t o = 0; o < sizeof opcodes; o++) {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        const uint8_t tsbk[10] = {opcodes[o], 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        char tag[64];
        char want[64];
        rc |= run_tsbk(&f, tsbk) != 0;
        DSD_SNPRINTF(tag, sizeof tag, "raw tsbk %02X", opcodes[o]);
        DSD_SNPRINTF(want, sizeof want, "Phase 1 TSBK %02X ", opcodes[o]);
        rc |= expect_int(tag, message_count(g_out), 1);
        rc |= expect_has(tag, want);
        rc |= expect_has(tag, "Octets 2-9 [11 22 33 44 55 66 77 88]");
        rc |= expect_lacks(tag, "Multi-Fragment");
        rc |= expect_lacks(tag, "multi-fragment");
        rc |= expect_lacks(tag, "Group Voice Service Request");
        rc |= expect_lacks(tag, "SNDCP");
        rc |= expect_int(tag, f.state->p25_ga_count, 0);
        rc |= expect_int(tag, f.state->p25_aff_count, 0);
        fixture_close(&f);
    }
    return rc;
}

/* Unit Registration Command. Phase 2 0x6D (TIA-102.BBAD-A §3.32 Fig 81): Target Address octets 2-4, Source Address
   octets 5-7. Phase 1 TSBK 0x2D (TIA-102.AABC-D §6.2.20): octets 2-3 reserved, Target 4-6, Source 7-9. */
static int
test_0x6d_native_offsets(void) {
    int rc = 0;
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t pdu[] = {0x00, 0x6D, 0x00, 0xAB, 0xCD, 0xFF, 0xFF, 0xFD,
                                      0x5F, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8};
        rc |= run_native_sacch(&f, pdu, (int)sizeof pdu) != 0;
        rc |= expect_int("0x6D messages", message_count(g_out), 2);
        rc |= expect_has("0x6D fields", "Unit Registration Command - TGT: 43981; SRC: 16777213;");
        rc |= expect_has("0x6D next message", "Call Alert - Abbreviated\n  Target [100] Source [200]");
        fixture_close(&f);
    }
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t tsbk[10] = {0x2D, 0x00, 0x00, 0x00, 0x00, 0xAB, 0xCD, 0xFF, 0xFF, 0xFD};
        rc |= run_tsbk(&f, tsbk) != 0;
        rc |= expect_int("p1 2D messages", message_count(g_out), 1);
        rc |= expect_has("p1 2D fields", "Unit Registration Command - TGT: 43981; SRC: 16777213;");
        fixture_close(&f);
    }
    return rc;
}

/* Roaming Address Command. Phase 1 TSBK 0x36 (TIA-102.AABC-D §6.2.25): Stack Operation octet 2, WACN and System ID
   octets 3-6, Target ID octets 7-9; Phase 2 0x76 (TIA-102.BBAD-A §3.50 Fig 113) carries a length octet first. */
static int
test_0x36_roaming_p1_layout(void) {
    int rc = 0;
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t tsbk[10] = {0x36, 0x00, 0x01, 0xBE, 0xE0, 0x01, 0x23, 0x12, 0xD6, 0x87};
        rc |= run_tsbk(&f, tsbk) != 0;
        rc |= expect_int("p1 36 messages", message_count(g_out), 1);
        rc |= expect_has("p1 36 fields", "StackOp [01] Target [BEE00:123.1234567]");
        fixture_close(&f);
    }
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t pdu[] = {0x00, 0x76, 0x0A, 0x01, 0xBE, 0xE0, 0x01, 0x23, 0x12, 0xD6, 0x87};
        rc |= run_native_sacch(&f, pdu, (int)sizeof pdu) != 0;
        rc |= expect_has("p2 76 fields", "StackOp [01] Target [BEE00:123.1234567]");
        fixture_close(&f);
    }
    return rc;
}

/* SNDCP Data Page Request: Phase 1 TSBK 0x15 (TIA-102.AABC-D §5.2.6) has two reserved octets before its Data Access
   Control and Target Address; Phase 2 0x55 does not. The transport picks the layout, not the sync type last seen. */
static int
test_0x15_layout_follows_transport(void) {
    int rc = 0;
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t tsbk[10] = {0x15, 0x00, 0x12, 0x00, 0x00, 0xAB, 0xCD, 0x12, 0xD6, 0x87};
        rc |= run_tsbk(&f, tsbk) != 0;
        rc |= expect_has("p1 15 fields", "DSO: 12; DAC: ABCD; Target: 1234567;");
        fixture_close(&f);
    }
    {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        static const uint8_t pdu[] = {0x00, 0x55, 0x12, 0xAB, 0xCD, 0x12, 0xD6, 0x87};
        rc |= run_native_sacch_sync(&f, pdu, (int)sizeof pdu, DSD_SYNC_P25P1_POS) != 0;
        rc |= expect_has("p2 55 fields", "DSO: 12; DAC: ABCD; Target: 1234567;");
        fixture_close(&f);
    }
    return rc;
}

/* A native PDU of two messages decodes both with the Phase 2 layout, also when its MAC header octet is 0x07. */
static int
test_native_multi_message_pdu_still_segments(void) {
    int rc = 0;
    static const uint8_t headers[] = {0x00, 0x07};
    for (size_t h = 0; h < sizeof headers; h++) {
        fixture f;
        if (fixture_open(&f) != 0) {
            return 1;
        }
        const uint8_t pdu[] = {headers[h], 0x5F, 0x00, 0x00, 0x64, 0x00, 0x00, 0xC8,
                               0x5A,       0x00, 0x01, 0x2C, 0x00, 0x01, 0x90};
        char tag[64];
        rc |= run_native_sacch(&f, pdu, (int)sizeof pdu) != 0;
        DSD_SNPRINTF(tag, sizeof tag, "native header %02X", headers[h]);
        rc |= expect_int(tag, message_count(g_out), 2);
        rc |= expect_has(tag, "Call Alert - Abbreviated\n  Target [100] Source [200]");
        rc |= expect_has(tag, "Status Query - Abbreviated\n  Target [300] Source [400]");
        fixture_close(&f);
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_call_alert_bridge_no_second_message();
    rc |= test_bridge_short_opcodes_single_structure();
    rc |= test_source_ending_07_no_misfire();
    rc |= test_p1_0x05_layout();
    rc |= test_p2_0x45_len8_0xc5_len12();
    rc |= test_0x4a_target_24bit();
    rc |= test_0x1e_bridged_no_overread();
    rc |= test_bridged_without_p1_layout_prints_raw();
    rc |= test_0x6d_native_offsets();
    rc |= test_0x36_roaming_p1_layout();
    rc |= test_0x15_layout_follows_transport();
    rc |= test_native_multi_message_pdu_still_segments();
    if (rc == 0) {
        printf("P25_TSBK_BRIDGE_BOUNDARY: OK\n");
    }
    return rc;
}
