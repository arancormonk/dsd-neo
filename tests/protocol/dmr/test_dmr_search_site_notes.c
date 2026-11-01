// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Signal search site notes from DMR: a C_ALOHA CSBK that passed its CRC and a C_SYS_PARMS / P_SYS_PARMS short LC that
 * passed Hamming(17,12,3) and its CRC-8 note the Tier III identity they carry; the same messages failing their checks
 * note nothing. Tier III numbers are shown one-based, split only by -D (the top SITE bits are the area); the received
 * identity keeps them raw, with the Connect Plus network in full and the Capacity Plus site from its checked messages.
 */

#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int g_search_running = 1;

static int
search_running(const dsd_state* state) {
    (void)state;
    return g_search_running;
}

static int g_notes;
static dsd_signal_search_site_note g_note;

static void
capture_site(const dsd_state* state, const dsd_signal_search_site_note* note) {
    (void)state;
    ++g_notes;
    g_note = *note;
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
expect_text(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
put_bits(uint8_t* bits, int at, uint32_t value, int width) {
    for (int i = 0; i < width; ++i) {
        bits[at + i] = (uint8_t)((value >> (width - 1 - i)) & 1U);
    }
}

/* Small model, network 12, site 3: 01 0001100 00011. */
enum { SYSCODE_SMALL_12_3 = (1 << 12) | (12 << 5) | 3 };

/* Small model, network 2, site 27 (11011b): 01 0000010 11011. */
enum { SYSCODE_SMALL_2_27 = (1 << 12) | (2 << 5) | 27 };

/* A C_ALOHA of feature set @p fid (0 standard, 0x10 Motorola: Capacity Max) carrying the 14-bit system code @p code. */
static void
aloha_code(dsd_opts* opts, dsd_state* state, int fid, int code, int crc_ok) {
    uint8_t bits[196];
    uint8_t bytes[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 25; // C_ALOHA
    bytes[1] = (uint8_t)fid;
    put_bits(bits, 0, 25, 8);
    put_bits(bits, 8, (uint32_t)fid, 8);
    put_bits(bits, 40, (uint32_t)code, 14);
    dmr_cspdu(opts, state, bits, bytes, (uint32_t)crc_ok, 0);
}

static void
aloha(dsd_opts* opts, dsd_state* state, int crc_ok) {
    aloha_code(opts, state, 0, SYSCODE_SMALL_12_3, crc_ok);
}

/* The same C_ALOHA as a BPTC-encoded CSBK burst through the data burst handler: with its CRC, or with the CRC off by one
   bit and the RAS reserved-bit marker, which -F accepts as RAS. */
static void
aloha_burst(dsd_opts* opts, dsd_state* state, int ras) {
    uint8_t bits[96];
    DSD_MEMSET(bits, 0, sizeof(bits));
    put_bits(bits, 0, 25, 8);
    put_bits(bits, 40, SYSCODE_SMALL_12_3, 14);
    uint16_t crc = (uint16_t)(ComputeCrcCCITT(bits) ^ 0xA5A5U);
    if (ras) {
        crc ^= 0x0001U;
    }
    put_bits(bits, 80, crc, 16);
    static const uint8_t k_ras_reserved[3] = {0, 0, 1};
    static const uint8_t k_no_reserved[3] = {0, 0, 0};
    uint8_t info[196];
    dmr_test_encode_bptc_196x96(bits, ras ? k_ras_reserved : k_no_reserved, info);
    dmr_data_burst_handler(opts, state, info, 0x03, NULL);
}

static void
hamming17_parity(uint8_t* slc) {
    slc[12] = (uint8_t)(slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[6] ^ slc[7] ^ slc[9]);
    slc[13] = (uint8_t)(slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[7] ^ slc[8] ^ slc[10]);
    slc[14] = (uint8_t)(slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[5] ^ slc[8] ^ slc[9] ^ slc[11]);
    slc[15] = (uint8_t)(slc[0] ^ slc[1] ^ slc[4] ^ slc[5] ^ slc[7] ^ slc[10]);
    slc[16] = (uint8_t)(slc[0] ^ slc[1] ^ slc[2] ^ slc[5] ^ slc[6] ^ slc[8] ^ slc[11]);
}

/* The TACT for @p lcss: the Hamming(7,4) codeword the decoder takes as it is. */
static void
tact(uint8_t cach[25], int lcss) {
    for (int p = 0; p < 8; ++p) {
        uint8_t bits[7] = {0,
                           0,
                           (uint8_t)((lcss >> 1) & 1),
                           (uint8_t)(lcss & 1),
                           (uint8_t)((p >> 2) & 1),
                           (uint8_t)((p >> 1) & 1),
                           (uint8_t)(p & 1)};
        uint8_t check[7];
        DSD_MEMCPY(check, bits, sizeof(check));
        if (Hamming_7_4_decode(check) && memcmp(check, bits, sizeof(bits)) == 0) {
            DSD_MEMCPY(cach, bits, sizeof(bits));
            return;
        }
    }
    abort();
}

/* Feed a four-fragment short LC whose 28 data bits are @p payload; @p corrupt flips one data bit after the CRC-8 is
   computed, so the message fails it. */
static void
slco_send(dsd_opts* opts, dsd_state* state, const uint8_t payload[28], int corrupt) {
    uint8_t data[44];
    DSD_MEMSET(data, 0, sizeof(data));
    DSD_MEMCPY(data, payload, 28);
    // The CRC-8 whose residue over the 36 bits is zero, as crc8_ok() reads it with the bits after them cleared.
    int crc = 0;
    for (; crc < 256; ++crc) {
        put_bits(data, 28, (uint32_t)crc, 8);
        if (crc8_ok(data, 36)) {
            break;
        }
    }
    if (crc == 256) {
        abort();
    }
    if (corrupt) {
        data[20] ^= 1U;
    }
    uint8_t slco_bits[68];
    DSD_MEMSET(slco_bits, 0, sizeof(slco_bits));
    for (size_t block = 0; block < 3; ++block) {
        for (size_t i = 0; i < 12; ++i) {
            slco_bits[block * 17 + i] = data[block * 12 + i];
        }
        hamming17_parity(&slco_bits[block * 17]);
    }
    uint8_t raw[68];
    for (int i = 0; i < 67; ++i) {
        raw[(i * 4) % 67] = slco_bits[i];
    }
    raw[67] = slco_bits[67];
    static const int lcss[4] = {1, 3, 3, 2};
    for (size_t f = 0; f < 4; ++f) {
        uint8_t cach[25];
        DSD_MEMSET(cach, 0, sizeof(cach));
        tact(cach, lcss[f]);
        DSD_MEMCPY(&cach[7], &raw[f * 17], 17);
        (void)dmr_cach(opts, state, cach);
    }
}

/* A short LC carrying @p slco with the system code @p code. */
static void
slco_sys_parms_code(dsd_opts* opts, dsd_state* state, int slco, int code, int corrupt) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, (uint32_t)slco, 4);
    put_bits(payload, 4, (uint32_t)code, 14);
    slco_send(opts, state, payload, corrupt);
}

/* A short LC carrying @p slco with SYSCODE_SMALL_12_3. */
static void
slco_sys_parms(dsd_opts* opts, dsd_state* state, int slco, int corrupt) {
    slco_sys_parms_code(opts, state, slco, SYSCODE_SMALL_12_3, corrupt);
}

/* A Connect Plus control (0xA) or traffic (0x9) channel short LC naming the 12-bit network @p net (bits 4..15) and
   @p site (bits 16..23). */
static void
slco_con_plus(dsd_opts* opts, dsd_state* state, int slco, int net, int site) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, (uint32_t)slco, 4);
    put_bits(payload, 4, (uint32_t)net, 12);
    put_bits(payload, 16, (uint32_t)site, 8);
    slco_send(opts, state, payload, 0);
}

/* A Capacity Plus short LC (0xF) naming the rest channel @p rest (bits 16..19) and site @p site (bits 22..24), with
   @p high in bits 20..21 (two more site bits in one reading of the field, reserved in another). */
static void
slco_cap_plus_high(dsd_opts* opts, dsd_state* state, int rest, int high, int site) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, 0xF, 4);
    put_bits(payload, 16, (uint32_t)rest, 4);
    put_bits(payload, 20, (uint32_t)high, 2);
    put_bits(payload, 22, (uint32_t)site, 3);
    slco_send(opts, state, payload, 0);
}

/* A Capacity Plus short LC (0xF) naming the rest channel @p rest and site @p site. */
static void
slco_cap_plus(dsd_opts* opts, dsd_state* state, int rest, int site, int corrupt) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, 0xF, 4);
    put_bits(payload, 16, (uint32_t)rest, 4);
    put_bits(payload, 22, (uint32_t)site, 3);
    slco_send(opts, state, payload, corrupt);
}

/* A Capacity Plus neighbour CSBK (opcode 0x3B, FID 0x10) whose own site (bits 25..28) is @p site, naming neighbour
   site 3 on rest LSN 2, its CRC passed. */
static void
cap_plus_neighbours(dsd_opts* opts, dsd_state* state, int site) {
    uint8_t bits[196];
    uint8_t bytes[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 0x3B;
    bytes[1] = 0x10;
    put_bits(bits, 0, 0x3B, 8);
    put_bits(bits, 8, 0x10, 8);
    put_bits(bits, 19, 1, 5); // own rest LSN
    put_bits(bits, 25, (uint32_t)site, 4);
    put_bits(bits, 29, 1, 3); // one neighbour
    put_bits(bits, 32, 3, 4);
    put_bits(bits, 36, 2, 4);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

/* A Capacity Plus channel status CSBK (opcode 0x3E, FID 0x10) in one block, naming rest LSN @p rest. */
static void
cap_plus_status(dsd_opts* opts, dsd_state* state, int rest) {
    uint8_t bits[196];
    uint8_t bytes[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 0x3E;
    bytes[1] = 0x10;
    put_bits(bits, 0, 0x3E, 8);
    put_bits(bits, 8, 0x10, 8);
    put_bits(bits, 16, 3, 2); // first and last block
    put_bits(bits, 19, (uint32_t)rest, 5);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

/* A Capacity Plus group voice LC (FLCO 0x04, FID 0x10) naming rest LSN @p rest, its check passed or not. */
static void
cap_plus_lc(dsd_opts* opts, dsd_state* state, int rest, uint32_t crc_correct) {
    uint8_t lc[96];
    DSD_MEMSET(lc, 0, sizeof(lc));
    put_bits(lc, 2, 0x04, 6);
    put_bits(lc, 8, 0x10, 8);
    put_bits(lc, 40, 7, 8);
    put_bits(lc, 51, (uint32_t)rest, 5);
    put_bits(lc, 56, 1234, 16);
    uint32_t irrecoverable = 0;
    dmr_flco(opts, state, lc, crc_correct, &irrecoverable, 1);
}

/* A conventional group voice LC (FLCO 0x00) under the vendor feature set @p fid, its check passed or not. */
static void
vendor_group_lc(dsd_opts* opts, dsd_state* state, int fid, uint32_t crc_correct) {
    uint8_t lc[96];
    DSD_MEMSET(lc, 0, sizeof(lc));
    put_bits(lc, 2, 0x00, 6);
    put_bits(lc, 8, (uint32_t)fid, 8);
    put_bits(lc, 24, 100, 24);
    put_bits(lc, 48, 1234, 24);
    uint32_t irrecoverable = 0;
    dmr_flco(opts, state, lc, crc_correct, &irrecoverable, 1);
}

/* A Hytera XPT site status CSBK (opcode 0x0A, FID 0x68) naming free LCN @p free_lcn. */
static void
xpt_site_status(dsd_opts* opts, dsd_state* state, int free_lcn) {
    uint8_t bits[196];
    uint8_t bytes[24];
    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(bytes, 0, sizeof(bytes));
    bytes[0] = 0x0A;
    bytes[1] = 0x68;
    put_bits(bits, 0, 0x0A, 8);
    put_bits(bits, 8, 0x68, 8);
    put_bits(bits, 16, (uint32_t)free_lcn, 4);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

/* A Hytera XPT call alert LC (FLCO 0x09, FID 0x68) naming free LCN @p free_lcn, its check passed or not. */
static void
xpt_call_alert(dsd_opts* opts, dsd_state* state, int free_lcn, uint32_t crc_correct) {
    uint8_t lc[96];
    DSD_MEMSET(lc, 0, sizeof(lc));
    put_bits(lc, 2, 0x09, 6);
    put_bits(lc, 8, 0x68, 8);
    put_bits(lc, 16, 2, 4);
    put_bits(lc, 24, (uint32_t)free_lcn, 4);
    put_bits(lc, 32, 100, 16);
    put_bits(lc, 56, 1234, 16);
    uint32_t irrecoverable = 0;
    dmr_flco(opts, state, lc, crc_correct, &irrecoverable, 1);
}

/* A Hytera XPT short LC (0x8) naming free LCN @p free_lcn. */
static void
slco_xpt(dsd_opts* opts, dsd_state* state, int free_lcn, int corrupt) {
    uint8_t payload[28];
    DSD_MEMSET(payload, 0, sizeof(payload));
    put_bits(payload, 0, 0x8, 4);
    put_bits(payload, 12, (uint32_t)free_lcn, 4);
    put_bits(payload, 16, 1, 4);
    slco_send(opts, state, payload, corrupt);
}

/* What a checked Tier III site message notes: the system code, the raw network and site, and the text. */
#define T3_SITE_PARTS (DSD_RXID_DMR_T3_SYSCODE | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT)

/* A fresh carrier: the record and the DMR RAS evidence the cases before left (a carrier boundary resets both). */
static void
clear_record(dsd_state* state) {
    dsd_rxid_reset(state);
    dmr_ras_reset(state);
}

/* The received identity record (dsd_state::rx_id) takes the identity of the same checked messages, whether or not a
   search runs; a message the decoder accepted only through RAS (-F), or one whose check failed, names nothing. */
static int
test_received_identity(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    g_search_running = 0;
    opts->dmr_dmrla_is_set = 1;
    opts->dmr_dmrla_n = 0;

    clear_record(state);
    aloha(opts, state, 1);
    rc |= expect_int("rx-id aloha parts", (long)id->valid, T3_SITE_PARTS);
    rc |= expect_int("rx-id aloha syscode", id->dmr_t3_syscode, SYSCODE_SMALL_12_3);
    rc |= expect_text("rx-id aloha site", id->dmr_site_text,
                      "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4 \xC2\xB7 SYS 1183");
    rc |= expect_int("rx-id aloha raw net", id->dmr_net, 12);
    rc |= expect_int("rx-id aloha raw site", id->dmr_site, 3);
    rc |= expect_int("rx-id aloha raw model", id->dmr_t3_model, 1);
    // The same message again, after the record went stale: an unchanged repeat proves it again.
    dsd_rxid_mark_stale(state);
    aloha(opts, state, 1);
    rc |= expect_int("rx-id aloha repeat is fresh", (long)id->fresh, T3_SITE_PARTS);

    static const uint8_t unverified[2] = {DSD_CRC_RAS_UNVERIFIED, DSD_CRC_FAILED};
    for (int i = 0; i < 2; ++i) {
        clear_record(state);
        state->currentslot = 1;
        state->event_crc_invalid[1] = unverified[i];
        aloha(opts, state, 1);
        cap_plus_status(opts, state, 5);
        state->event_crc_invalid[1] = 0;
        state->currentslot = 0;
        rc |= expect_int("rx-id unverified csbks note nothing", (long)id->valid, 0);
    }
    rc |= expect_int("rx-id unverified cap+ status still follows", state->dmr_rest_channel, 5);
    clear_record(state);
    opts->aggressive_framesync = 0;
    aloha_burst(opts, state, 1);
    rc |= expect_int("rx-id ras aloha burst notes nothing", (long)id->valid, 0);
    aloha_burst(opts, state, 0);
    rc |= expect_int("rx-id checked aloha burst notes", (long)id->valid, T3_SITE_PARTS);

    for (int slco = 2; slco <= 3; ++slco) {
        clear_record(state);
        slco_sys_parms(opts, state, slco, 1);
        rc |= expect_int("rx-id failed sys_parms notes nothing", (long)id->valid, 0);
        slco_sys_parms(opts, state, slco, 0);
        rc |= expect_int("rx-id sys_parms notes", (long)id->valid, T3_SITE_PARTS);
    }

    // Capacity Plus: the channel status CSBK names the rest LSN, with the brand; the voice LC only with its check.
    clear_record(state);
    state->dmr_rest_channel = 5;
    cap_plus_status(opts, state, 5);
    rc |= expect_int("rx-id cap+ status rest lsn (unchanged repeat)", id->dmr_rest_lsn, 5);
    rc |= expect_int("rx-id cap+ status parts", (long)id->valid,
                     DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_BRAND | DSD_RXID_DMR_VENDOR);
    rc |= expect_text("rx-id cap+ brand", id->dmr_brand, "Capacity Plus");
    clear_record(state);
    cap_plus_lc(opts, state, 9, 0U);
    rc |= expect_int("rx-id failed cap+ lc notes nothing", (long)id->valid, 0);
    state->currentslot = 0;
    state->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    cap_plus_lc(opts, state, 9, 1U);
    state->event_crc_invalid[0] = 0;
    rc |= expect_int("rx-id ras cap+ lc notes nothing", (long)id->valid, 0);
    cap_plus_lc(opts, state, 9, 1U);
    rc |= expect_int("rx-id checked cap+ lc rest lsn",
                     (long)(id->valid & DSD_RXID_DMR_REST_LSN) != 0 ? id->dmr_rest_lsn : -1, 9);
    clear_record(state);
    slco_cap_plus(opts, state, 4, 2, 1);
    rc |= expect_int("rx-id failed cap+ short lc notes nothing", (long)id->valid, 0);
    slco_cap_plus(opts, state, 4, 2, 0);
    rc |= expect_int("rx-id cap+ short lc parts", (long)id->valid,
                     DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND);
    rc |= expect_int("rx-id cap+ short lc rest lsn", id->dmr_rest_lsn, 4);
    rc |= expect_text("rx-id cap+ short lc site", id->dmr_site_text, "Cap+ \xC2\xB7 Site 2");
    rc |= expect_int("rx-id cap+ short lc raw site", id->dmr_site, 2);
    // Another Capacity Plus site whose short LC names no rest channel: the rest channel the old site named goes.
    clear_record(state);
    slco_cap_plus(opts, state, 5, 1, 0);
    slco_cap_plus(opts, state, 0, 2, 0);
    rc |= expect_text("rx-id cap+ other site", id->dmr_site_text, "Cap+ \xC2\xB7 Site 2");
    rc |= expect_int("rx-id cap+ other site drops the old rest lsn", (long)(id->valid & DSD_RXID_DMR_REST_LSN), 0);

    // The vendor a checked LC or CSBK names (its feature set ID), apart from any trunking brand; one RAS accepted or one
    // that failed its check names none.
    clear_record(state);
    vendor_group_lc(opts, state, 0x68, 0U);
    state->currentslot = 0;
    state->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    vendor_group_lc(opts, state, 0x68, 1U);
    state->event_crc_invalid[0] = 0;
    rc |= expect_int("rx-id unchecked lc names no vendor", (long)(id->valid & DSD_RXID_DMR_VENDOR), 0);
    vendor_group_lc(opts, state, 0x68, 1U);
    rc |=
        expect_int("rx-id checked lc vendor", (long)(id->valid & DSD_RXID_DMR_VENDOR) != 0 ? id->dmr_vendor : -1, 0x68);
    rc |= expect_int("rx-id a vendor is no trunking brand", (long)(id->valid & DSD_RXID_DMR_BRAND), 0);
    clear_record(state);
    cap_plus_status(opts, state, 5);
    rc |= expect_int("rx-id checked csbk vendor", (long)(id->valid & DSD_RXID_DMR_VENDOR) != 0 ? id->dmr_vendor : -1,
                     0x10);

    // Hytera XPT: the site status CSBK, the call alert LC and the short LC each name the site's free channel, which the
    // record takes from a checked message only.
    for (int i = 0; i < 2; ++i) {
        clear_record(state);
        state->currentslot = 1;
        state->event_crc_invalid[1] = unverified[i];
        xpt_site_status(opts, state, 6);
        state->event_crc_invalid[1] = 0;
        state->currentslot = 0;
        rc |= expect_int("rx-id unverified xpt status notes no free lcn", (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN),
                         0);
    }
    clear_record(state);
    xpt_site_status(opts, state, 6);
    rc |= expect_int("rx-id xpt status free lcn",
                     (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN) != 0 ? id->dmr_xpt_free_lcn : -1, 6);
    rc |= expect_text("rx-id xpt status brand", id->dmr_brand, "Hytera XPT");
    clear_record(state);
    xpt_call_alert(opts, state, 7, 0U);
    rc |= expect_int("rx-id failed xpt alert notes no free lcn", (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN), 0);
    state->currentslot = 0;
    state->event_crc_invalid[0] = DSD_CRC_RAS_UNVERIFIED;
    xpt_call_alert(opts, state, 7, 1U);
    state->event_crc_invalid[0] = 0;
    rc |= expect_int("rx-id ras xpt alert notes no free lcn", (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN), 0);
    rc |= expect_text("rx-id ras xpt alert is still decoded", state->dmr_site_parms, "Free LCN - 7 ");
    xpt_call_alert(opts, state, 7, 1U);
    rc |= expect_int("rx-id checked xpt alert free lcn",
                     (long)(id->valid & DSD_RXID_DMR_XPT_FREE_LCN) != 0 ? id->dmr_xpt_free_lcn : -1, 7);
    clear_record(state);
    slco_xpt(opts, state, 8, 1);
    rc |= expect_int("rx-id failed xpt short lc notes nothing", (long)id->valid, 0);
    slco_xpt(opts, state, 8, 0);
    rc |= expect_int("rx-id xpt short lc parts", (long)id->valid, DSD_RXID_DMR_XPT_FREE_LCN | DSD_RXID_DMR_BRAND);
    rc |= expect_int("rx-id xpt short lc free lcn", id->dmr_xpt_free_lcn, 8);

    // Connect Plus: control channel A, then an operator move with trunking on to system B's traffic channel. B's traffic
    // channel short LC names B; A's control channel frequency is not B's, and B's traffic channel is no control channel.
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->rtlsdr_center_freq = 451000000U;
    clear_record(state);
    slco_con_plus(opts, state, 0xA, 3, 5);
    rc |= expect_text("rx-id con+ control site", id->dmr_site_text, "Con+ \xC2\xB7 Net 3 \xC2\xB7 Site 5");
    rc |= expect_int("rx-id con+ control channel frequency", (long)id->cc_freq_hz, 451000000L);
    rc |= expect_int("rx-id con+ control parts", (long)id->valid,
                     DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_BRAND
                         | DSD_RXID_CC_FREQ);
    clear_record(state); // the carrier boundary of the operator's move
    opts->rtlsdr_center_freq = 452500000U;
    slco_con_plus(opts, state, 0x9, 4, 6);
    rc |= expect_text("rx-id con+ traffic site", id->dmr_site_text, "Con+ \xC2\xB7 Net 4 \xC2\xB7 Site 6");
    rc |= expect_int("rx-id con+ traffic has no control channel", (long)(id->valid & DSD_RXID_CC_FREQ), 0);
    // A Tier III control channel names its frequency with the system.
    clear_record(state);
    aloha(opts, state, 1);
    rc |= expect_int("rx-id aloha control channel frequency", (long)id->cc_freq_hz, 452500000L);
    opts->audio_in_type = 0;
    opts->trunk_enable = 0;
    opts->rtlsdr_center_freq = 0;
    g_search_running = 1;
    return rc;
}

#define DOT " \xC2\xB7 "

/* Sets -D as the command line does: @p is_set 0 is no -D. */
static void
set_dmrla(dsd_opts* opts, int is_set, int n) {
    opts->dmr_dmrla_is_set = (uint8_t)is_set;
    opts->dmr_dmrla_n = (uint8_t)n;
}

/* No -D (or -D 0): no area split; the network and site show one-based, the record keeps them raw (issue #799 D1, D2). */
static int
test_t3_default_no_split_plus_one(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    for (int set = 0; set <= 1; ++set) {
        set_dmrla(opts, set, 0);
        clear_record(state);
        aloha_code(opts, state, 0, SYSCODE_SMALL_2_27, 1);
        rc |= expect_text("t3 no split text", id->dmr_site_text, "TIII Small" DOT "Net 3" DOT "Site 28" DOT "SYS 105B");
        rc |= expect_text("t3 no split site parameters", state->dmr_site_parms, "TIII Small:3-28;105B; ");
        rc |= expect_int("t3 no split raw net", id->dmr_net, 2);
        rc |= expect_int("t3 no split raw site", id->dmr_site, 27);
        rc |= expect_int("t3 no split raw model", id->dmr_t3_model, 1);
        rc |= expect_int("t3 no split trunk", id->dmr_trunk, DSD_RXID_DMR_TRUNK_TIER3);
        rc |= expect_int("t3 no split parts", (long)id->valid, T3_SITE_PARTS);
    }
    return rc;
}

/* -D 2 is the spec's DMRLA 2: the top two SITE bits of 11011b (11) are the area, the other three (011) the site in it. */
static int
test_t3_dmrla_top_bits(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    set_dmrla(opts, 1, 2);
    clear_record(state);
    aloha_code(opts, state, 0, SYSCODE_SMALL_2_27, 1);
    rc |= expect_text("t3 dmrla text", id->dmr_site_text, "TIII Small" DOT "Net 3" DOT "Site 4.4" DOT "SYS 105B");
    rc |= expect_text("t3 dmrla site parameters", state->dmr_site_parms, "TIII Small:3-4.4;105B; ");
    rc |= expect_int("t3 dmrla raw site", id->dmr_site, 27);
    set_dmrla(opts, 1, 0);
    return rc;
}

/* -D past the SITE width is clamped to it: the whole field is the area, nothing is left to split off. */
static int
test_t3_dmrla_clamped(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    set_dmrla(opts, 1, 9);
    clear_record(state);
    aloha_code(opts, state, 0, SYSCODE_SMALL_2_27, 1);
    rc |= expect_text("t3 clamped text", state->rx_id.dmr_site_text,
                      "TIII Small" DOT "Net 3" DOT "Site 28" DOT "SYS 105B");
    set_dmrla(opts, 1, 0);
    return rc;
}

/* A Capacity Max C_ALOHA (FID 0x10) reads as any Tier III one: the same split and numbering, the "Capacity Max" label
   and the Motorola branding, and it never changes the user's -D. */
static int
test_capmax_keeps_label_and_opts(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    static const int dmrla[3][2] = {{0, 0}, {1, 0}, {1, 2}};
    static const char* const texts[3] = {"TIII Small" DOT "Net 3" DOT "Site 28" DOT "SYS 105B",
                                         "TIII Small" DOT "Net 3" DOT "Site 28" DOT "SYS 105B",
                                         "TIII Small" DOT "Net 3" DOT "Site 4.4" DOT "SYS 105B"};
    for (int i = 0; i < 3; ++i) {
        set_dmrla(opts, dmrla[i][0], dmrla[i][1]);
        clear_record(state);
        DSD_SNPRINTF(state->dmr_branding, sizeof(state->dmr_branding), "%s", "");
        dsd_test_capture_stderr cap;
        char log[4096];
        if (dsd_test_capture_stderr_begin(&cap, "dmr_capmax") != 0) {
            DSD_FPRINTF(stderr, "capmax: stderr capture failed\n");
            return 1;
        }
        aloha_code(opts, state, 0x10, SYSCODE_SMALL_2_27, 1);
        (void)dsd_test_capture_stderr_end(&cap);
        if (dsd_test_capture_stderr_read(&cap, log, sizeof(log)) != 0) {
            DSD_FPRINTF(stderr, "capmax: stderr read failed\n");
            return 1;
        }
        rc |= expect_text("capmax text", state->rx_id.dmr_site_text, texts[i]);
        rc |= expect_int("capmax label", strstr(log, " Capacity Max") != NULL, 1);
        rc |= expect_text("capmax branding", state->dmr_branding, "Motorola");
        rc |= expect_int("capmax keeps -D set", opts->dmr_dmrla_is_set, dmrla[i][0]);
        rc |= expect_int("capmax keeps -D n", opts->dmr_dmrla_n, dmrla[i][1]);
    }
    // A standard Tier III C_ALOHA after it keeps the user's -D too: no split it left behind.
    set_dmrla(opts, 0, 0);
    aloha_code(opts, state, 0x10, SYSCODE_SMALL_2_27, 1);
    clear_record(state);
    aloha_code(opts, state, 0, SYSCODE_SMALL_2_27, 1);
    rc |= expect_int("tier iii after capmax keeps no -D", opts->dmr_dmrla_is_set, 0);
    rc |= expect_text("tier iii after capmax text", state->rx_id.dmr_site_text, texts[0]);
    return rc;
}

/* One system through C_SYS_PARMS, P_SYS_PARMS and C_ALOHA reads as one text, with and without a split, and the
   messages alternating never move the record's epoch. */
static int
test_slco_and_csbk_same_split(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    for (int n = 0; n <= 2; n += 2) {
        set_dmrla(opts, 1, n);
        clear_record(state);
        slco_sys_parms_code(opts, state, 2, SYSCODE_SMALL_2_27, 0);
        char first[sizeof(id->dmr_site_text)];
        DSD_SNPRINTF(first, sizeof(first), "%s", id->dmr_site_text);
        const uint32_t epoch = id->epoch;
        rc |= expect_text("slco text", first,
                          n == 0 ? "TIII Small" DOT "Net 3" DOT "Site 28" DOT "SYS 105B"
                                 : "TIII Small" DOT "Net 3" DOT "Site 4.4" DOT "SYS 105B");
        aloha_code(opts, state, 0, SYSCODE_SMALL_2_27, 1);
        rc |= expect_text("aloha text equals slco", id->dmr_site_text, first);
        slco_sys_parms_code(opts, state, 3, SYSCODE_SMALL_2_27, 0);
        rc |= expect_text("p_sys_parms text equals slco", id->dmr_site_text, first);
        aloha_code(opts, state, 0x10, SYSCODE_SMALL_2_27, 1);
        rc |= expect_text("capmax aloha text equals slco", id->dmr_site_text, first);
        rc |= expect_int("one system keeps the epoch", (long)id->epoch, (long)epoch);
    }
    set_dmrla(opts, 1, 0);
    return rc;
}

/* The Connect Plus network is the short LC's bits 4..15 (12 bits), not only their low byte. */
static int
test_conplus_net_12_bit(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    clear_record(state);
    slco_con_plus(opts, state, 0xA, 0x4D2, 7);
    rc |= expect_text("con+ 12-bit net text", id->dmr_site_text, "Con+" DOT "Net 1234" DOT "Site 7");
    rc |= expect_text("con+ 12-bit net site parameters", state->dmr_site_parms, "1234-7 ");
    rc |= expect_int("con+ 12-bit raw net", id->dmr_net, 1234);
    rc |= expect_int("con+ raw site", id->dmr_site, 7);
    rc |= expect_int("con+ trunk", id->dmr_trunk, DSD_RXID_DMR_TRUNK_CONNECT_PLUS);
    clear_record(state);
    slco_con_plus(opts, state, 0x9, 0xFFF, 255);
    rc |= expect_text("con+ traffic max net", id->dmr_site_text, "Con+" DOT "Net 4095" DOT "Site 255");
    rc |= expect_int("con+ traffic raw net", id->dmr_net, 4095);
    return rc;
}

/* The Capacity Plus neighbour CSBK 0x3B names its own site in bits 25..28: a checked one notes it, one -F accepted by
   RAS or whose CRC failed does not. */
static int
test_capplus_csbk_3b_site(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    clear_record(state);
    cap_plus_neighbours(opts, state, 9);
    rc |= expect_text("cap+ 3b site text", id->dmr_site_text, "Cap+" DOT "Site 9");
    rc |= expect_int("cap+ 3b raw site", (id->valid & DSD_RXID_DMR_SITE) != 0U ? id->dmr_site : -1, 9);
    rc |= expect_int("cap+ 3b no net", (long)(id->valid & DSD_RXID_DMR_NET), 0);
    rc |= expect_int("cap+ 3b trunk", id->dmr_trunk, DSD_RXID_DMR_TRUNK_CAPACITY_PLUS);
    rc |= expect_text("cap+ 3b brand", id->dmr_brand, "Capacity Plus");
    static const uint8_t unverified[2] = {DSD_CRC_RAS_UNVERIFIED, DSD_CRC_FAILED};
    for (int i = 0; i < 2; ++i) {
        clear_record(state);
        state->currentslot = 1;
        state->event_crc_invalid[1] = unverified[i];
        cap_plus_neighbours(opts, state, 9);
        state->event_crc_invalid[1] = 0;
        state->currentslot = 0;
        rc |= expect_int("cap+ unverified 3b notes no site", (long)(id->valid & DSD_RXID_DMR_SITE), 0);
    }
    return rc;
}

/* A Capacity Plus short LC whose bits 20..21 are set names no site (the field's width is disputed: one reading takes
   them as the site's high bits, another as reserved); its rest LSN still counts, and a site the CSBK named stays. */
static int
test_capplus_slco_reserved_bits_no_site(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    for (int high = 1; high <= 3; ++high) {
        clear_record(state);
        slco_cap_plus_high(opts, state, 4, high, 1);
        rc |= expect_int("cap+ high bits note no site",
                         (long)(id->valid & (DSD_RXID_DMR_SITE | DSD_RXID_DMR_SITE_TEXT)), 0);
        rc |=
            expect_int("cap+ high bits rest lsn", (id->valid & DSD_RXID_DMR_REST_LSN) != 0U ? id->dmr_rest_lsn : -1, 4);
        rc |= expect_text("cap+ high bits brand", id->dmr_brand, "Capacity Plus");
    }
    clear_record(state);
    cap_plus_neighbours(opts, state, 9);
    const uint32_t epoch = id->epoch;
    slco_cap_plus_high(opts, state, 4, 2, 1); // 01001b in the five-bit reading
    rc |= expect_int("cap+ high bits keep the csbk site", (id->valid & DSD_RXID_DMR_SITE) != 0U ? id->dmr_site : -1, 9);
    rc |= expect_text("cap+ high bits keep the csbk text", id->dmr_site_text, "Cap+" DOT "Site 9");
    rc |= expect_int("cap+ high bits keep the epoch", (long)id->epoch, (long)epoch);
    rc |= expect_int("cap+ high bits still note the rest lsn", id->dmr_rest_lsn, 4);
    return rc;
}

/* The short LC (bits 20..21 clear) and the CSBK 0x3B name one site alike, so alternating them is one site. */
static int
test_capplus_slco_and_3b_agree(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    const dsd_received_identity* id = &state->rx_id;
    clear_record(state);
    slco_cap_plus(opts, state, 4, 5, 0);
    const uint32_t epoch = id->epoch;
    cap_plus_neighbours(opts, state, 5);
    slco_cap_plus(opts, state, 4, 5, 0);
    cap_plus_neighbours(opts, state, 5);
    rc |= expect_text("cap+ alternating text", id->dmr_site_text, "Cap+" DOT "Site 5");
    rc |= expect_int("cap+ alternating keeps the epoch", (long)id->epoch, (long)epoch);
    rc |= expect_int("cap+ alternating keeps the rest lsn", (long)(id->valid & DSD_RXID_DMR_REST_LSN),
                     DSD_RXID_DMR_REST_LSN);
    cap_plus_neighbours(opts, state, 6);
    rc |= expect_int("cap+ another 3b site moves the epoch", (long)id->epoch, (long)epoch + 1);
    return rc;
}

static int
test_site_numbers(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    g_search_running = 0;
    rc |= test_t3_default_no_split_plus_one(opts, state);
    rc |= test_t3_dmrla_top_bits(opts, state);
    rc |= test_t3_dmrla_clamped(opts, state);
    rc |= test_capmax_keeps_label_and_opts(opts, state);
    rc |= test_slco_and_csbk_same_split(opts, state);
    rc |= test_conplus_net_12_bit(opts, state);
    rc |= test_capplus_csbk_3b_site(opts, state);
    rc |= test_capplus_slco_reserved_bits_no_site(opts, state);
    rc |= test_capplus_slco_and_3b_agree(opts, state);
    set_dmrla(opts, 1, 0);
    g_search_running = 1;
    return rc;
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: %s%s\n", !opts ? "dsd_opts" : "", !state ? " dsd_state" : "");
        free(state);
        free(opts);
        return 1;
    }
    dsd_signal_search_site_set(capture_site);
    dsd_signal_search_active_set(search_running);
    int rc = 0;
    // -D 0: no area split, the site shown whole.
    opts->dmr_dmrla_is_set = 1;
    opts->dmr_dmrla_n = 0;

    aloha(opts, state, 1);
    rc |= expect_int("crc-valid aloha notes once", g_notes, 1);
    rc |= expect_int("aloha note kind", g_note.kind, DSD_SEARCH_SITE_NOTE_DMR_TIII);
    rc |= expect_text("aloha note text", g_note.text, "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4 \xC2\xB7 SYS 1183");
    rc |= expect_text("aloha site parameters agree", state->dmr_site_parms, "TIII Small:13-4;1183; ");
    g_notes = 0;
    aloha(opts, state, 0);
    rc |= expect_int("failed-crc aloha notes nothing", g_notes, 0);

    // dmr_cspdu()'s gate also passes a burst -F accepted by RAS or an MBC whose header CRC failed; the dispatcher flags
    // those for the slot, and neither names a site.
    static const uint8_t unverified[2] = {DSD_CRC_RAS_UNVERIFIED, DSD_CRC_FAILED};
    for (int i = 0; i < 2; ++i) {
        g_notes = 0;
        state->currentslot = 1;
        state->event_crc_invalid[1] = unverified[i];
        aloha(opts, state, 1);
        state->event_crc_invalid[1] = 0;
        rc |= expect_int("unverified aloha notes nothing", g_notes, 0);
        rc |= expect_text("unverified aloha is still decoded", state->dmr_site_parms, "TIII Small:13-4;1183; ");
        state->currentslot = 0;
    }
    // Through the burst handler: a checked CSBK notes the site, a RAS-accepted one under -F does not.
    opts->aggressive_framesync = 0;
    g_notes = 0;
    aloha_burst(opts, state, 0);
    rc |= expect_int("checked aloha burst notes once", g_notes, 1);
    g_notes = 0;
    DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "%s", "");
    aloha_burst(opts, state, 1);
    rc |= expect_text("ras aloha burst is decoded", state->dmr_site_parms, "TIII Small:13-4;1183; ");
    rc |= expect_int("ras aloha burst notes nothing", g_notes, 0);
    // With no search running there is nothing to note.
    g_search_running = 0;
    g_notes = 0;
    aloha(opts, state, 1);
    rc |= expect_int("aloha without a search notes nothing", g_notes, 0);
    g_search_running = 1;

    // A -D 2 split: site 3 (00011b) is area 0 (00), site 3 (011) in it, shown one-based as the site parameters are.
    opts->dmr_dmrla_n = 2;
    aloha(opts, state, 1);
    rc |= expect_text("split aloha note text", g_note.text,
                      "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 1.4 \xC2\xB7 SYS 1183");
    rc |= expect_text("split aloha site parameters agree", state->dmr_site_parms, "TIII Small:13-1.4;1183; ");
    opts->dmr_dmrla_n = 0;

    for (int slco = 2; slco <= 3; ++slco) {
        g_notes = 0;
        DSD_MEMSET(g_note.text, 0, sizeof(g_note.text));
        slco_sys_parms(opts, state, slco, 0);
        rc |= expect_int("checked sys_parms short lc notes once", g_notes, 1);
        rc |= expect_text("sys_parms note text", g_note.text,
                          "TIII Small \xC2\xB7 Net 13 \xC2\xB7 Site 4 \xC2\xB7 SYS 1183");
        g_notes = 0;
        slco_sys_parms(opts, state, slco, 1);
        rc |= expect_int("failed-crc sys_parms short lc notes nothing", g_notes, 0);
    }

    rc |= test_received_identity(opts, state);
    rc |= test_site_numbers(opts, state);

    dsd_signal_search_site_set(NULL);
    dsd_signal_search_active_set(NULL);
    dsd_state_ext_free_all(state); // the Capacity Plus status keeps recent activity in a state extension
    free(state);
    free(opts);
    return rc;
}
