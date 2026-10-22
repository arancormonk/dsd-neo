// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Signal search site notes from P25: every broadcast that passed its CRC (a TSBK RFSS status through the vPDU bridge,
 * an MBT network or RFSS status, a Phase 2 network or RFSS status MAC) notes the identity it carries, and nothing else
 * does: a Phase 2 MAC_SIGNAL that failed its CRC (-F, which the decoder does not apply either), a voice LCW, a
 * secondary control channel fill-in or an adjacent site. Decoder state is left to the decoders.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

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

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_blocks_phase1(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
tait_iso7_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)lc_bits;
}

static int g_notes;
static dsd_signal_search_site_note g_note;
/* The record the notes build, as a search visit would. */
static dsd_signal_search_site g_record;

static void
capture_site(const dsd_state* state, const dsd_signal_search_site_note* note) {
    (void)state;
    ++g_notes;
    g_note = *note;
    (void)dsd_signal_search_site_apply(&g_record, note);
}

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_site(const char* tag, const char* want) {
    char text[96];
    (void)dsd_signal_search_site_format(&g_record, text, sizeof(text));
    if (strcmp(text, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, text, want);
        return 1;
    }
    return 0;
}

static dsd_opts g_opts;
static dsd_state g_state;

static void
reset(void) {
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_notes = 0;
    DSD_MEMSET(&g_note, 0, sizeof(g_note));
    dsd_signal_search_site_clear(&g_record);
}

/* An RFSS status in MAC form: header @p header (0x07 for a TSBK the vPDU bridge carries), opcode 0x7A (abbreviated)
   or 0xFA (explicit). */
static void
rfss_mac(unsigned long long mac[24], unsigned long long header, unsigned long long opcode, uint16_t sysid, uint8_t rfss,
         uint8_t site) {
    DSD_MEMSET(mac, 0, 24 * sizeof(mac[0]));
    mac[0] = header;
    mac[1] = opcode;
    mac[2] = 0x01; // LRA
    mac[3] = (sysid >> 8) & 0xF;
    mac[4] = sysid & 0xFF;
    mac[5] = rfss;
    mac[6] = site;
    mac[7] = 0x10; // CHAN-T
    mac[8] = 0x01;
}

/* A network status in MAC form: opcode 0x7B (abbreviated) or 0xFB (extended), NAC 0x293. */
static void
network_mac(unsigned long long mac[24], unsigned long long opcode, uint32_t wacn, uint16_t sysid) {
    DSD_MEMSET(mac, 0, 24 * sizeof(mac[0]));
    mac[0] = 0x00;
    mac[1] = opcode;
    mac[2] = 0x01; // LRA
    mac[3] = (wacn >> 12) & 0xFF;
    mac[4] = (wacn >> 4) & 0xFF;
    mac[5] = ((wacn & 0xF) << 4) | ((sysid >> 8) & 0xF);
    mac[6] = sysid & 0xFF;
    mac[7] = 0x10; // CHAN-T
    mac[8] = 0x01;
    const int nac_at = opcode == 0xFB ? 12 : 10;
    mac[nac_at] = 0x02;
    mac[nac_at + 1] = 0x93;
}

static int
test_vpdu_rfss(void) {
    int rc = 0;
    unsigned long long mac[24];

    // A TSBK RFSS status (CRC-16 passed before processTSBK() hands it to the vPDU bridge).
    reset();
    rfss_mac(mac, 0x07, 0x7A, 0x1A3, 1, 2);
    process_MAC_VPDU(&g_opts, &g_state, 0, P25_MAC_PDU_SIGNAL, mac);
    rc |= expect_int("tsbk rfss notes once", g_notes, 1);
    rc |= expect_int("tsbk rfss kind", g_note.kind, DSD_SEARCH_SITE_NOTE_P25_SITE);
    rc |= expect_site("tsbk rfss site", "SYS 1A3 \xC2\xB7 RFSS 1 \xC2\xB7 Site 2");

    // A Phase 2 RFSS status, abbreviated and explicit, whose CRC passed.
    reset();
    rfss_mac(mac, 0x00, 0x7A, 0x2B4, 3, 4);
    process_MAC_VPDU_crc(&g_opts, &g_state, 1, P25_MAC_PDU_SIGNAL, mac, 1);
    rc |= expect_int("p2 rfss notes", g_notes, 1);
    rc |= expect_site("p2 rfss site", "SYS 2B4 \xC2\xB7 RFSS 3 \xC2\xB7 Site 4");
    reset();
    rfss_mac(mac, 0x00, 0xFA, 0x2B4, 5, 6);
    process_MAC_VPDU_crc(&g_opts, &g_state, 0, P25_MAC_PDU_SIGNAL, mac, 1);
    rc |= expect_int("p2 explicit rfss notes", g_notes, 1);
    rc |= expect_site("p2 explicit rfss site", "SYS 2B4 \xC2\xB7 RFSS 5 \xC2\xB7 Site 6");

    // The same broadcasts let through with a failed CRC (-F): neither the decoder nor the search takes the site.
    reset();
    rfss_mac(mac, 0x00, 0x7A, 0x2B4, 3, 4);
    process_MAC_VPDU_crc(&g_opts, &g_state, 1, P25_MAC_PDU_SIGNAL, mac, 0);
    rc |= expect_int("failed-crc rfss is not applied", (long)g_state.p2_rfssid, 0);
    rc |= expect_int("failed-crc rfss notes nothing", g_notes, 0);
    reset();
    rfss_mac(mac, 0x00, 0xFA, 0x2B4, 5, 6);
    process_MAC_VPDU_crc(&g_opts, &g_state, 1, P25_MAC_PDU_SIGNAL, mac, 0);
    rc |= expect_int("failed-crc explicit rfss is not applied", (long)g_state.p2_siteid, 0);
    rc |= expect_int("failed-crc explicit rfss notes nothing", g_notes, 0);

    // SYSID 0 names no system.
    reset();
    rfss_mac(mac, 0x07, 0x7A, 0x000, 1, 2);
    process_MAC_VPDU(&g_opts, &g_state, 0, P25_MAC_PDU_SIGNAL, mac);
    rc |= expect_int("sysid 0 rfss is decoded", (long)g_state.p2_siteid, 2);
    rc |= expect_int("sysid 0 rfss notes nothing", g_notes, 0);

    // An adjacent site is another site.
    reset();
    rfss_mac(mac, 0x00, 0x7C, 0x1A3, 7, 8);
    process_MAC_VPDU(&g_opts, &g_state, 0, P25_MAC_PDU_SIGNAL, mac);
    rc |= expect_int("adjacent status notes nothing", g_notes, 0);
    return rc;
}

static int
test_vpdu_network(void) {
    int rc = 0;
    unsigned long long mac[24];
    static const unsigned long long opcodes[2] = {0x7B, 0xFB};
    for (int i = 0; i < 2; ++i) {
        reset();
        network_mac(mac, opcodes[i], 0xBEE00, 0x1A3);
        process_MAC_VPDU_crc(&g_opts, &g_state, 1, P25_MAC_PDU_SIGNAL, mac, 1);
        rc |= expect_int("p2 network notes once", g_notes, 1);
        rc |= expect_int("p2 network kind", g_note.kind, DSD_SEARCH_SITE_NOTE_P25_NETWORK);
        rc |= expect_site("p2 network site", "WACN BEE00 \xC2\xB7 SYS 1A3");

        // -F: a network status that failed its CRC names the system to neither the decoder nor the search.
        reset();
        network_mac(mac, opcodes[i], 0xBEE00, 0x1A3);
        process_MAC_VPDU_crc(&g_opts, &g_state, 1, P25_MAC_PDU_SIGNAL, mac, 0);
        rc |= expect_int("failed-crc network is not applied", (long)g_state.p2_wacn, 0);
        rc |= expect_int("failed-crc network notes nothing", g_notes, 0);

        // A WACN under SYSID 0 names no system either.
        reset();
        network_mac(mac, opcodes[i], 0xBEE00, 0x000);
        process_MAC_VPDU_crc(&g_opts, &g_state, 1, P25_MAC_PDU_SIGNAL, mac, 1);
        rc |= expect_int("sysid 0 network notes nothing", g_notes, 0);
    }
    return rc;
}

/* An outbound standard MBT (the MDPU layer passes p25_decode_pdu_trunking() only one whose header CRC-16 and CRC-32
   both passed). */
static void
mbt_header(uint8_t mbt[48], uint8_t opcode, uint16_t sysid) {
    DSD_MEMSET(mbt, 0, 48);
    mbt[0] = 0x37; // OSP ALT format
    mbt[2] = 0x00; // standard MFID
    mbt[3] = 0x01; // LRA
    mbt[4] = (uint8_t)((sysid >> 8) & 0xF);
    mbt[5] = (uint8_t)(sysid & 0xFF);
    mbt[6] = 0x02; // blocks
    mbt[7] = opcode;
}

static void
mbt_rfss(uint8_t mbt[48], uint16_t sysid, uint8_t rfss, uint8_t site) {
    mbt_header(mbt, 0x3A, sysid);
    mbt[12] = rfss;
    mbt[13] = site;
    mbt[14] = 0x10; // CHAN-T
    mbt[15] = 0x01;
}

static void
mbt_network(uint8_t mbt[48], uint32_t wacn, uint16_t sysid) {
    mbt_header(mbt, 0x3B, sysid);
    mbt[12] = (uint8_t)((wacn >> 12) & 0xFF);
    mbt[13] = (uint8_t)((wacn >> 4) & 0xFF);
    mbt[14] = (uint8_t)((wacn & 0xF) << 4);
    mbt[15] = 0x10; // CHAN-T
    mbt[16] = 0x01;
}

static int
test_mbt(void) {
    int rc = 0;
    uint8_t mbt[48];

    // An RFSS status before any network status, then the network status: the record binds them by SYSID.
    reset();
    mbt_rfss(mbt, 0x1A3, 2, 3);
    (void)p25_decode_pdu_trunking(&g_opts, &g_state, mbt, sizeof(mbt));
    rc |= expect_int("mbt rfss notes", g_notes, 1);
    rc |= expect_site("mbt rfss site", "SYS 1A3 \xC2\xB7 RFSS 2 \xC2\xB7 Site 3");
    mbt_network(mbt, 0xBEE00, 0x1A3);
    (void)p25_decode_pdu_trunking(&g_opts, &g_state, mbt, sizeof(mbt));
    rc |= expect_int("mbt network notes", g_notes, 2);
    rc |= expect_site("mbt rfss then network", "WACN BEE00 \xC2\xB7 SYS 1A3 \xC2\xB7 RFSS 2 \xC2\xB7 Site 3");
    rc |= expect_int("decoder keeps its own system", (long)g_state.p2_sysid, 0x1A3);

    // A network status of another system replaces the record whole.
    mbt_network(mbt, 0xBEE00, 0x2B4);
    (void)p25_decode_pdu_trunking(&g_opts, &g_state, mbt, sizeof(mbt));
    rc |= expect_site("network change replaces", "WACN BEE00 \xC2\xB7 SYS 2B4");

    // An RFSS status advertising a SYSID the decoder does not hold: the decoder ignores it, and the note still carries
    // the broadcast's own SYSID, which the record does not join to a different system.
    mbt_rfss(mbt, 0x1A3, 4, 5);
    const int before = g_notes;
    (void)p25_decode_pdu_trunking(&g_opts, &g_state, mbt, sizeof(mbt));
    rc |= expect_int("foreign-sysid rfss notes", g_notes, before + 1);
    rc |= expect_int("foreign-sysid rfss note sysid", g_note.sysid, 0x1A3);
    rc |= expect_int("decoder ignores the foreign site", (long)g_state.p2_rfssid, 2);
    rc |= expect_site("record ignores the foreign site", "WACN BEE00 \xC2\xB7 SYS 2B4");

    // An adjacent status names another site.
    reset();
    mbt_header(mbt, 0x3C, 0x1A3);
    mbt[8] = 4;
    mbt[9] = 5;
    mbt[12] = 0x10;
    mbt[13] = 0x0A;
    (void)p25_decode_pdu_trunking(&g_opts, &g_state, mbt, sizeof(mbt));
    rc |= expect_int("mbt adjacent notes nothing", g_notes, 0);
    return rc;
}

static void
put_bits(uint8_t* bits, int at, uint32_t value, int width) {
    for (int i = 0; i < width; ++i) {
        bits[at + i] = (uint8_t)((value >> (width - 1 - i)) & 1U);
    }
}

/* The voice LCW's Reed-Solomon word accepts a large share of random input: it updates decoder state, never a search. */
static int
test_lcw_and_fill_in(void) {
    int rc = 0;
    uint8_t lcw[72];
    reset();
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    put_bits(lcw, 0, 0x63, 8);    // RFSS Status Broadcast
    put_bits(lcw, 20, 0x1A3, 12); // SYSID
    put_bits(lcw, 32, 6, 8);      // RFSS
    put_bits(lcw, 40, 7, 8);      // Site
    p25_lcw(&g_opts, &g_state, lcw, 0);
    rc |= expect_int("lcw rfss is decoded", (long)g_state.p2_rfssid, 6);
    rc |= expect_int("lcw rfss notes nothing", g_notes, 0);

    reset();
    DSD_MEMSET(lcw, 0, sizeof(lcw));
    put_bits(lcw, 0, 0x64, 8);      // Network Status Broadcast
    put_bits(lcw, 16, 0xBEE00, 20); // WACN
    put_bits(lcw, 36, 0x1A3, 12);   // SYSID
    p25_lcw(&g_opts, &g_state, lcw, 0);
    rc |= expect_int("lcw network is decoded", (long)g_state.p2_wacn, 0xBEE00);
    rc |= expect_int("lcw network notes nothing", g_notes, 0);

    // A secondary control channel broadcast fills in a site the decoder lacks: mixed provenance, no note.
    reset();
    (void)p25_announce_secondary_cc_channel(&g_opts, &g_state, 0x1001, 2, 3, 0);
    rc |= expect_int("sccb fill-in is decoded", (long)g_state.p2_siteid, 3);
    rc |= expect_int("sccb fill-in notes nothing", g_notes, 0);
    return rc;
}

int
main(void) {
    dsd_signal_search_site_set(capture_site);
    int rc = 0;
    rc |= test_vpdu_rfss();
    rc |= test_vpdu_network();
    rc |= test_mbt();
    rc |= test_lcw_and_fill_in();
    dsd_signal_search_site_set(NULL);
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
