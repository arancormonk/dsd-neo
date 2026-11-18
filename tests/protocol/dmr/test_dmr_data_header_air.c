// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/*
 * Issue #800, fail closed: a DMR data packet's blocks bind to its data header only when they came in a burst of the
 * same sender kind (by its sync word) on the same over-the-air slot, on the same reception. The bursts go through the
 * real single-slot data path (dmrMSData() -> dmr_data_sync() -> the burst handler -> the data header and the block
 * assembler); only the symbol reader is replaced (linker wraps of getDibitSoft and get_dibit_and_analog_signal), and
 * each burst's sync word and sync type are what frame sync leaves for it.
 *
 * The packet is a UDT (TS 102 361-4) from radio 123456 whose one block is a LIP short location report (22.5 S, 45 W):
 * heard whole from one sender kind on one slot it gives that position; a repeater-mode block after a direct-mode header
 * (or the reverse) gives none, nor, on the single-slot decoder's one lane, a block proven on the other slot than its
 * header, so the header's radio never names another transmission's position. (The direct-mode sync words also put
 * their slots on lanes of their own, so through the reader a block on the other slot finds no header at all.)
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/core/synctype_ids.h"

// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
int __wrap_getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft);
// NOLINTNEXTLINE(bugprone-reserved-identifier,misc-use-internal-linkage)
int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);

/* The burst's live half (54 dibits), then filler for the reads past it. */
static int g_live[54];
static size_t g_live_next;

int
__wrap_getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    (void)state;
    const int dibit = g_live_next < 54U ? g_live[g_live_next] : 1;
    g_live_next++;
    if (out_soft != NULL) {
        DSD_MEMSET(out_soft, 0, sizeof(*out_soft));
        out_soft->reliability = 200U;
    }
    return dibit;
}

int
__wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    if (out_analog_signal != NULL) {
        *out_analog_signal = 0;
    }
    return 1;
}

static void
unpack(const uint8_t* bytes, uint8_t* bits, size_t count) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - i % 8U)) & 1U);
    }
}

/* The slot type: colour code 1 and @p type, with its Golay(20,8) parity. */
static int
slot_type_bits(unsigned type, unsigned char st[20]) {
    unsigned char check[20];
    DSD_MEMSET(st, 0, 20);
    const unsigned cc = 1U;
    for (unsigned b = 0; b < 4U; b++) {
        st[b] = (unsigned char)((cc >> (3U - b)) & 1U);
        st[4U + b] = (unsigned char)((type >> (3U - b)) & 1U);
    }
    for (unsigned parity = 0; parity < 4096U; parity++) {
        for (unsigned b = 0; b < 12U; b++) {
            st[8U + b] = (unsigned char)((parity >> (11U - b)) & 1U);
        }
        DSD_MEMCPY(check, st, sizeof check);
        if (Golay_20_8_decode(check) && memcmp(check, st, sizeof check) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The 144 dibits of a BPTC burst as sent (CACH, payload, slot type, @p sync) carrying the 12 octets @p bytes. */
static int
build_burst(const char* sync, unsigned type, const uint8_t bytes[12], int dibits[144]) {
    static const uint8_t reserved[3] = {0, 0, 0};
    uint8_t payload[96];
    unpack(bytes, payload, 12);
    uint8_t info[196];
    dmr_test_encode_bptc_196x96(payload, reserved, info);
    unsigned char st[20];
    if (!slot_type_bits(type, st)) {
        return 0;
    }
    DSD_MEMSET(dibits, 0, 144 * sizeof(int));
    for (size_t i = 0; i < 49U; i++) {
        dibits[12U + i] = (info[2U * i] << 1) | info[2U * i + 1U];
        dibits[95U + i] = (info[98U + 2U * i] << 1) | info[99U + 2U * i];
    }
    for (size_t i = 0; i < 5U; i++) {
        dibits[61U + i] = (st[2U * i] << 1) | st[2U * i + 1U];
        dibits[90U + i] = (st[10U + 2U * i] << 1) | st[11U + 2U * i];
    }
    for (size_t i = 0; i < 24U; i++) {
        dibits[66U + i] = sync[i] == '3' ? 3 : 1;
    }
    return 1;
}

/* Who sends a burst: its sync word, and what frame sync leaves for it (the sync type and the direct-mode slot). */
typedef struct {
    const char* sync;
    int synctype;
    uint8_t dm_slot;
} burst_source;

static const burst_source k_direct_ts1 = {DMR_DIRECT_MODE_TS1_DATA_SYNC, DSD_SYNC_DMR_MS_DATA, 1U};
static const burst_source k_direct_ts2 = {DMR_DIRECT_MODE_TS2_DATA_SYNC, DSD_SYNC_DMR_MS_DATA, 2U};
static const burst_source k_ms = {DMR_MS_DATA_SYNC, DSD_SYNC_DMR_MS_DATA, 0U};

/* Receive one burst of data type @p type through the single-slot reader. */
static int
receive_burst(dsd_opts* opts, dsd_state* state, const burst_source* from, unsigned type, const uint8_t bytes[12]) {
    static int payload[90];
    int dibits[144];
    if (!build_burst(from->sync, type, bytes, dibits)) {
        return 0;
    }
    for (size_t i = 0; i < 90U; i++) {
        payload[i] = dibits[i];
    }
    for (size_t i = 0; i < 54U; i++) {
        g_live[i] = dibits[90U + i];
    }
    g_live_next = 0;
    state->dmr_payload_p = payload + 90;
    state->synctype = from->synctype;
    state->lastsynctype = from->synctype;
    state->dmr_dm_sync_slot = from->dm_slot;
    dmrMSData(opts, state);
    return 1;
}

/* The UDT header: DPF 0, target 42, source 123456, format 0x08 (LIP), one appended block, CRC-CCITT with the data
   header mask. */
static void
udt_header(uint8_t out[12]) {
    static const uint8_t hdr[10] = {0, 0x0B, 0, 0, 42, 0x01, 0xE2, 0x40, 0x08, 0};
    uint8_t bits[96] = {0};
    DSD_MEMCPY(out, hdr, sizeof hdr);
    unpack(out, bits, 10);
    const uint16_t crc = (uint16_t)(ComputeCrcCCITT(bits) ^ 0xCCCCU);
    out[10] = (uint8_t)(crc >> 8);
    out[11] = (uint8_t)crc;
}

static void
set_bits(uint8_t* bits, size_t offset, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        bits[offset + i] = (uint8_t)((value >> (width - i - 1U)) & 1U);
    }
}

/* The UDT block: a LIP short location report (22.5 S, 45 W, as DMR_LIP_CARRIERS sends it) and the UDT CRC-CCITT over
   it. */
static void
udt_block(uint8_t out[12]) {
    uint8_t bits[96] = {0};
    set_bits(bits, 2, 2, 1);
    set_bits(bits, 4, 1, 1);
    set_bits(bits, 5, 24, 0xC00000);
    set_bits(bits, 29, 1, 1);
    set_bits(bits, 30, 23, 0x600000);
    set_bits(bits, 53, 3, 1);
    set_bits(bits, 56, 7, 20);
    set_bits(bits, 68, 8, 32);
    DSD_MEMSET(out, 0, 12);
    for (size_t i = 0; i < 80U; ++i) {
        out[i / 8U] |= (uint8_t)(bits[i] << (7U - i % 8U));
    }
    const uint16_t crc = dsd_crc_ccitt16_bits(bits, 80);
    out[10] = (uint8_t)(crc >> 8);
    out[11] = (uint8_t)crc;
}

static void
reset_state(dsd_state* state) {
    Event_History_I* history = state->event_history_s;
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof *state);
    DSD_MEMSET(history, 0, 2 * sizeof *history);
    for (int slot = 0; slot < 2; ++slot) {
        init_event_history(&history[slot], 0, 255);
    }
    state->event_history_s = history;
    state->dmr_color_code = 16;
}

/* Send the header from @p header_from and the block from @p block_from; 1 when the feed then holds the report as
   radio 123456's, 0 when it holds none, -1 for anything else. */
static int
send_packet(dsd_opts* opts, dsd_state* state, const burst_source* header_from, const burst_source* block_from) {
    reset_state(state);
    if (dsd_position_feed_start(state) != 0) {
        return -1;
    }
    uint8_t hdr[12];
    uint8_t block[12];
    udt_header(hdr);
    udt_block(block);
    if (!receive_burst(opts, state, header_from, 0x06U, hdr) || !receive_burst(opts, state, block_from, 0x07U, block)) {
        return -1;
    }
    dsd_position_fix kept[4];
    dsd_position_feed_meta meta;
    const int n = dsd_position_feed_copy_since(state, 0U, kept, 4U, &meta);
    if (n == 0) {
        return 0;
    }
    return n == 1 && kept[0].source_id == 123456U && kept[0].lat_e7 == -225000000 && kept[0].lon_e7 == -450000000 ? 1
                                                                                                                  : -1;
}

/* The single-slot decoder's one lane carrying both slots (dsd_state::dmr_mono_rx_slot), driven at the header and
   assembler: a checked UDT header proven on slot 2, then a checked block proven on slot 1 whose own header was
   missed. 1 when the feed holds a report, 0 when none. */
static int
send_lane0_packet(dsd_opts* opts, dsd_state* state, uint8_t header_slot, uint8_t block_slot) {
    reset_state(state);
    if (dsd_position_feed_start(state) != 0) {
        return -1;
    }
    uint8_t hdr[12];
    uint8_t block[12];
    uint8_t bits[96];
    udt_header(hdr);
    udt_block(block);
    unpack(hdr, bits, 12);
    state->currentslot = 0;
    state->synctype = DSD_SYNC_DMR_MS_DATA;
    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    state->dmr_dm_sync_slot = header_slot;
    state->dmr_mono_rx_slot = (uint8_t)(DSD_DMR_MONO_RX_TS1 + header_slot - 1U);
    dmr_dheader(opts, state, hdr, bits, 1, 0);
    state->dmr_dm_sync_slot = block_slot;
    state->dmr_mono_rx_slot = (uint8_t)(DSD_DMR_MONO_RX_TS1 + block_slot - 1U);
    dmr_block_assembler(opts, state, block, 12, 0x07, 3);
    dsd_position_fix kept[4];
    dsd_position_feed_meta meta;
    return dsd_position_feed_copy_since(state, 0U, kept, 4U, &meta);
}

int
main(void) {
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        return 100;
    }
    static dsd_opts opts;
    static dsd_state state;
    state.event_history_s = calloc(2, sizeof *state.event_history_s);
    if (state.event_history_s == NULL) {
        return 101;
    }
    opts.aggressive_framesync = 1;

    static const struct {
        const burst_source* header;
        const burst_source* block;
        int want;
        const char* tag;
    } cases[] = {
        {&k_direct_ts2, &k_direct_ts2, 1, "direct mode, header and block on slot 2"},
        {&k_direct_ts1, &k_direct_ts1, 1, "direct mode, header and block on slot 1"},
        {&k_direct_ts2, &k_direct_ts1, 0, "direct mode, header on slot 2, block on slot 1"},
        {&k_direct_ts1, &k_direct_ts2, 0, "direct mode, header on slot 1, block on slot 2"},
        {&k_direct_ts1, &k_ms, 0, "direct-mode header, repeater-mode (MS-sourced) block"},
        {&k_ms, &k_direct_ts1, 0, "repeater-mode (MS-sourced) header, direct-mode block"},
        {&k_ms, &k_ms, 1, "repeater-mode (MS-sourced) header and block"},
    };

    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const int got = send_packet(&opts, &state, cases[i].header, cases[i].block);
        if (got != cases[i].want) {
            DSD_FPRINTF(stderr, "\n%s: got %d, want %d\n", cases[i].tag, got, cases[i].want);
            failed = 1;
        }
    }

    static const struct {
        uint8_t header_slot;
        uint8_t block_slot;
        int want;
    } lane0[] = {{2U, 2U, 1}, {2U, 1U, 0}, {1U, 2U, 0}};

    for (size_t i = 0; i < sizeof lane0 / sizeof lane0[0]; ++i) {
        const int got = send_lane0_packet(&opts, &state, lane0[i].header_slot, lane0[i].block_slot);
        if (got != lane0[i].want) {
            DSD_FPRINTF(stderr, "\none lane, header on slot %u, block on slot %u: got %d, want %d\n",
                        (unsigned)lane0[i].header_slot, (unsigned)lane0[i].block_slot, got, lane0[i].want);
            failed = 1;
        }
    }
    dsd_state_ext_free_all(&state);
    free(state.event_history_s);
    if (failed == 0) {
        DSD_FPRINTF(stdout, "DMR_DATA_HEADER_AIR: PASS\n");
    }
    return failed;
}
