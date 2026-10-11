// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* Issue #496: exercise real header/assembler/service/event-log paths, not emitter stubs. */
#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dmr_rs_test_encoder.h"
#include "dsd-neo/core/bit_packing.h"
#include "test_support.h"

static void
unpack(const uint8_t* bytes, uint8_t* bits, size_t count) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - i % 8U)) & 1U);
    }
}

static void
header(dsd_opts* opts, dsd_state* state, uint8_t bytes[12], int crc_ok) {
    uint8_t bits[96];
    unpack(bytes, bits, 12);
    dmr_dheader(opts, state, bytes, bits, (uint32_t)crc_ok, 0);
}

static void
crc32(uint8_t* bytes, size_t count) {
    uint8_t bits[8 * 48];
    assert(count <= 48 && count >= 4 && count % 2 == 0);
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[(i / 8U) ^ 1U] >> (7U - i % 8U)) & 1U);
    }
    uint32_t crc = ComputeCrc32Bit(bits, (uint32_t)(count * 8U - 32U));
    for (size_t i = 0; i < 4; ++i) {
        bytes[count - 4 + i] = (uint8_t)(crc >> (24U - i * 8U));
    }
}

static void
send_udt(dsd_opts* opts, dsd_state* state, int header_ok, int payload_ok) {
    uint8_t hdr[12] = {0x00, 0x04, 0, 0, 42, 0, 0, 24, 0, 0, 0, 0};
    uint8_t block[12] = {'H', 'E', 'L', 'L', 'O', 0, 0, 0, 0, 0, 0, 0};
    uint8_t bits[96];
    unpack(block, bits, 12);
    uint16_t crc = (uint16_t)dsd_crc_ccitt16_bits(bits, 80);
    block[10] = (uint8_t)(crc >> 8);
    block[11] = (uint8_t)(crc ^ (payload_ok ? 0U : 1U));
    header(opts, state, hdr, header_ok);
    dmr_block_assembler(opts, state, block, 12, 0x07, 3);
}

static void
send_mnis(dsd_opts* opts, dsd_state* state, int header_ok, int extension_ok, int payload_ok) {
    /* Standard header plus actual DPF=15/SAP=1 extension, not the old DPF=2 test shortcut. */
    uint8_t hdr[12] = {0x02, 0x90, 0, 0, 42, 0, 0, 24, 0x82, 0, 0, 0};
    /* Independently computed using node-dmr-lib's span and CRC32 algorithm. */
    uint8_t pdu[22] = {0x1F, 0x10, 0x02, 0x01, 0x88, 0x55, 0x01, 0,    0,    0,    0,
                       0,    0,    0,    0,    0,    0,    0,    0x14, 0x43, 0xAE, 0x84};
    uint8_t ext[12] = {0};
    pdu[21] ^= (uint8_t)(payload_ok ? 0 : 1);
    DSD_MEMCPY(ext, pdu, 10);
    header(opts, state, hdr, header_ok);
    header(opts, state, ext, extension_ok);
    dmr_block_assembler(opts, state, pdu + 10, 12, 0x07, 1);
}

static void
send_captured_mnis(dsd_opts* opts, dsd_state* state, int corrupt) {
    // Untouched LRRP packet from https://github.com/lwvmobile/dsd-fme/issues/283.
    // The dump's last two zeros are buffer fill: first ten header octets + three data blocks = 46.
    uint8_t hdr[12] = {0x02, 0x99, 0, 0, 0x64, 0, 0x04, 0x42, 0x84, 0, 0xF4, 0xF0};
    uint8_t ext[12] = {0x1F, 0x10, 0x02, 0x01, 0x11, 0xF7, 0x32, 0x0D, 0x18, 0x23, 0x8F, 0x80};
    uint8_t blocks[36] = {0x41, 0x34, 0x1F, 0x9A, 0x36, 0x71, 0xCF, 0x51, 0x49, 0x22, 0x23, 0x00,
                          0x0F, 0xA0, 0x02, 0x0E, 0x21, 0x26, 0x6C, 0x00, 0x25, 0x56, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xE8, 0x1D, 0x14};
    blocks[5] ^= (uint8_t)(corrupt ? 1 : 0);
    header(opts, state, hdr, 1);
    header(opts, state, ext, 1);
    for (size_t i = 0; i < sizeof blocks; i += 12) {
        dmr_block_assembler(opts, state, blocks + i, 12, 0x07, 1);
    }
}

static void
send_unknown(dsd_opts* opts, dsd_state* state, int header_ok, int payload_ok, int encrypted) {
    uint8_t hdr[12] = {0x02, 0x50, 0, 0, 42, 0, 0, 24, 0x81, 0, 0, 0};
    uint8_t block[12] = {0};
    crc32(block, sizeof block);
    block[11] ^= (uint8_t)(payload_ok ? 0 : 1);
    header(opts, state, hdr, header_ok);
    if (encrypted) {
        state->dmr_so = 0x100;
        state->payload_algid = 0x07;
    }
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
}

static int
count_location_rows(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    int rows = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') {
            ++rows;
        }
    }
    int read_error = ferror(file);
    int close_error = fclose(file);
    return read_error || close_error ? -1 : rows;
}

static int
test_mnis_location_file(dsd_opts* opts, dsd_state* state) {
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dmr-mnis-lrrp-crc");
    if (fd < 0) {
        return 1;
    }
    if (dsd_close(fd) != 0) {
        remove(path);
        return 1;
    }
    DSD_SNPRINTF(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", path);
    opts->lrrp_file_output = 1;
    opts->aggressive_framesync = 0;
    opts->dmr_crc_relaxed_default = 1;
    state->currentslot = 0;
    int rc = 0;
    for (int corrupt = 0; corrupt <= 1; ++corrupt) {
        int before = count_location_rows(path);
        send_captured_mnis(opts, state, corrupt);
        int after = count_location_rows(path);
        int expected = corrupt ? 0 : 1;
        if (before < 0 || after < 0 || after != before + expected) {
            DSD_FPRINTF(stderr, "\nMNIS %s -L rows: expected %d new row(s), before=%d after=%d\n",
                        corrupt ? "corrupt" : "clean", expected, before, after);
            rc = 1;
            break;
        }
    }
    opts->lrrp_file_output = 0;
    if (remove(path) != 0) {
        rc = 1;
    }
    return rc;
}

// `flags` is the row's exact DSD_CRC_* provenance: a failure renders [CRC ERR] at warning severity whatever else it
// carries, RAS alone renders [RAS] at the row's ordinary severity, and a verified row carries neither.
static void
expect_row(const Event_History* row, uint8_t flags, dsd_event_severity clean_severity) {
    assert(row->crc_invalid == flags);
    assert((strstr(row->event_string, "[CRC ERR]") != NULL) == dsd_crc_is_failed(flags));
    assert((strstr(row->event_string, "[RAS]") != NULL) == (flags == DSD_CRC_RAS_UNVERIFIED));
    assert(row->severity == (dsd_crc_is_failed(flags) ? DSD_EVENT_SEVERITY_WARNING : clean_severity));
}

static void
expect_event(dsd_state* state, uint8_t slot, uint64_t before, int emitted, uint8_t flags, const char* payload) {
    assert(state->event_history_s[slot].push_seq == before + (uint64_t)emitted);
    if (!emitted) {
        return;
    }
    const Event_History* row = &state->event_history_s[slot].Event_History_Items[1];
    assert(strstr(row->event_string, payload) != NULL || strstr(row->text_message, payload) != NULL);
    expect_row(row, flags, DSD_EVENT_SEVERITY_INFO);
}

static const uint8_t k_ras_reserved[3] = {0, 0, 1}; // BPTC reserved bits = 4, the RAS marker
static const uint8_t k_no_reserved[3] = {0, 0, 0};

static void
send_burst_bits(dsd_opts* opts, dsd_state* state, const uint8_t bits[96], const uint8_t reserved[3],
                uint8_t databurst) {
    uint8_t info[196];
    dmr_test_encode_bptc_196x96(bits, reserved, info);
    dmr_data_burst_handler(opts, state, info, databurst, NULL);
}

// A fully BPTC-encoded data header whose CCITT CRC is deliberately wrong, with the RAS reserved bits: under -F the
// heuristic accepts it, and nothing else is wrong with it.
static void
send_ras_header(dsd_opts* opts, dsd_state* state, const uint8_t hdr[12]) {
    uint8_t bytes[12];
    uint8_t bits[96];
    DSD_MEMCPY(bytes, hdr, 12);
    unpack(bytes, bits, sizeof bytes);
    const uint16_t crc = (uint16_t)(ComputeCrcCCITT(bits) ^ 0xCCCCU ^ 0x0001U);
    bytes[10] = (uint8_t)(crc >> 8);
    bytes[11] = (uint8_t)crc;
    unpack(bytes, bits, sizeof bytes);
    send_burst_bits(opts, state, bits, k_ras_reserved, 0x06);
}

// One confirmed rate-1/2 block: DBSN and CRC9 (mask 0x0F0) ahead of ten data octets. A failed CRC9 sent with the RAS
// reserved bits is what the heuristic accepts; without them it is a plain failure.
static void
send_confirmed_block(dsd_opts* opts, dsd_state* state, uint8_t dbsn, const uint8_t data[10], int crc9_ok, int ras) {
    uint8_t bytes[12] = {0};
    uint8_t bits[96];
    uint8_t conf[87];
    DSD_MEMCPY(bytes + 2, data, 10);
    unpack(bytes, bits, sizeof bytes);
    for (unsigned i = 0; i < 7U; ++i) {
        bits[i] = (uint8_t)((dbsn >> (6U - i)) & 1U);
    }
    for (unsigned i = 0; i < 80U; ++i) {
        conf[i] = bits[i + 16U];
    }
    for (unsigned i = 0; i < 7U; ++i) {
        conf[80U + i] = bits[i];
    }
    uint16_t crc = (uint16_t)((ComputeCrc9Bit(conf, 87) ^ 0x0F0U) & 0x1FFU);
    if (!crc9_ok) {
        crc ^= 0x001U;
    }
    for (unsigned i = 0; i < 9U; ++i) {
        bits[7U + i] = (uint8_t)((crc >> (8U - i)) & 1U);
    }
    send_burst_bits(opts, state, bits, ras ? k_ras_reserved : k_no_reserved, 0x07);
}

// A confirmed packet of `blocks` blocks behind a clean header; crc9_ok/ras say how each block arrives.
static void
send_confirmed(dsd_opts* opts, dsd_state* state, int blocks, const int* crc9_ok, const int* ras) {
    uint8_t hdr[12] = {0x03, 0x50, 0, 0, 42, 0, 0, 24, (uint8_t)(0x80U | (unsigned)blocks), 0, 0, 0};
    uint8_t payload[30] = {0x11, 0x22, 0x33};
    assert(blocks >= 1 && blocks <= 3);
    crc32(payload, (size_t)blocks * 10U);
    header(opts, state, hdr, 1);
    for (int b = 0; b < blocks; ++b) {
        send_confirmed_block(opts, state, (uint8_t)b, payload + (size_t)b * 10U, crc9_ok[b], ras[b]);
    }
}

static void
put_bits(uint8_t* bits, size_t offset, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        bits[offset + i] = (uint8_t)((value >> (width - i - 1U)) & 1U);
    }
}

// An MBC TV_GRANT to absolute channel 0xFFF: the header (RAS-accepted, or checked) and its checked continuation both go
// through the burst handler. Learning the channel posts a notice from inside the MBC's CRC scope, which is the only
// place the MBC's provenance becomes visible.
static void
send_mbc_grant(dsd_opts* opts, dsd_state* state, int ras, uint16_t lpcn) {
    uint8_t hdr[12] = {0x31, 0x00, 0xFF, 0xF0, 0x00, 0x03, 0xE9, 0x00, 0x07, 0xD2, 0, 0};
    uint8_t bits[96];
    unpack(hdr, bits, sizeof hdr);
    uint16_t crc = (uint16_t)(ComputeCrcCCITT(bits) ^ 0xAAAAU);
    if (ras) {
        crc ^= 0x0001U;
    }
    hdr[10] = (uint8_t)(crc >> 8);
    hdr[11] = (uint8_t)crc;
    unpack(hdr, bits, sizeof hdr);
    send_burst_bits(opts, state, bits, ras ? k_ras_reserved : k_no_reserved, 0x04);

    uint8_t block[96] = {0};
    put_bits(block, 0, 1, 1U);     // last block
    put_bits(block, 22, 12, lpcn); // logical channel the absolute parameters describe (CSBK bits 118..129)
    put_bits(block, 57, 10, 851U); // RX MHz (CSBK bits 153..162)
    put_bits(block, 67, 13, 100U); // RX 125 Hz steps (CSBK bits 163..175)
    put_bits(block, 80, 16, (uint32_t)dsd_crc_ccitt16_bits(block, 80));
    send_burst_bits(opts, state, block, k_no_reserved, 0x05);
}

static void
test_ras_data_provenance(dsd_opts* opts, dsd_state* state) {
    const uint8_t slot = 0;
    state->currentslot = slot;
    opts->aggressive_framesync = 0;
    opts->dmr_crc_relaxed_default = 1;
    uint64_t before = state->event_history_s[slot].push_seq;

    // A RAS-accepted header followed by a packet that fails its own CRC32 is a failure, not RAS.
    const uint8_t unknown_hdr[12] = {0x02, 0x50, 0, 0, 42, 0, 0, 24, 0x81, 0, 0, 0};
    uint8_t block[12] = {0};
    crc32(block, sizeof block);
    block[11] ^= 1U;
    send_ras_header(opts, state, unknown_hdr);
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
    expect_event(state, slot, before++, 1, (uint8_t)(DSD_CRC_FAILED | DSD_CRC_RAS_UNVERIFIED), "Unknown PDU");
    block[11] ^= 1U;
    send_ras_header(opts, state, unknown_hdr);
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
    expect_event(state, slot, before++, 1, DSD_CRC_RAS_UNVERIFIED, "Unknown PDU");

    // UDT: the header's RAS provenance reaches the MBC/UDT flags; a failed continuation CRC16 still fails.
    const uint8_t udt_hdr[12] = {0x00, 0x04, 0, 0, 42, 0, 0, 24, 0, 0, 0, 0};
    for (int payload_ok = 1; payload_ok >= 0; --payload_ok) {
        uint8_t udt[12] = {'H', 'E', 'L', 'L', 'O', 0, 0, 0, 0, 0, 0, 0};
        uint8_t bits[96];
        unpack(udt, bits, sizeof udt);
        const uint16_t crc = (uint16_t)dsd_crc_ccitt16_bits(bits, 80);
        udt[10] = (uint8_t)(crc >> 8);
        udt[11] = (uint8_t)(crc ^ (payload_ok ? 0U : 1U));
        send_ras_header(opts, state, udt_hdr);
        dmr_block_assembler(opts, state, udt, 12, 0x07, 3);
        expect_event(state, slot, before++, 1,
                     payload_ok ? DSD_CRC_RAS_UNVERIFIED : (uint8_t)(DSD_CRC_FAILED | DSD_CRC_RAS_UNVERIFIED), "HELLO");
    }

    // A proprietary (DPF 15) extension inherits the standard header's RAS verdict and adds its own.
    const uint8_t mnis_hdr[12] = {0x02, 0x90, 0, 0, 42, 0, 0, 24, 0x82, 0, 0, 0};
    for (int extension_ok = 1; extension_ok >= 0; --extension_ok) {
        uint8_t pdu[22] = {0x1F, 0x10, 0x02, 0x01, 0x88, 0x55, 0x01, 0,    0,    0,    0,
                           0,    0,    0,    0,    0,    0,    0,    0x14, 0x43, 0xAE, 0x84};
        uint8_t ext[12] = {0};
        DSD_MEMCPY(ext, pdu, 10);
        send_ras_header(opts, state, mnis_hdr);
        header(opts, state, ext, extension_ok);
        dmr_block_assembler(opts, state, pdu + 10, 12, 0x07, 1);
        expect_event(state, slot, before++, 1,
                     extension_ok ? DSD_CRC_RAS_UNVERIFIED : (uint8_t)(DSD_CRC_FAILED | DSD_CRC_RAS_UNVERIFIED),
                     "IP ID: 5501");
    }

    // Confirmed blocks: a RAS-accepted CRC9 is RAS; a genuinely failed one, in either order beside a RAS block and
    // followed by a clean final block, is a failure.
    static const struct {
        int blocks;
        int crc9_ok[3];
        int ras[3];
        uint8_t flags;
    } confirmed[] = {
        {1, {1, 1, 1}, {0, 0, 0}, 0U},
        {1, {0, 1, 1}, {1, 0, 0}, DSD_CRC_RAS_UNVERIFIED},
        {3, {0, 0, 1}, {0, 1, 0}, DSD_CRC_FAILED | DSD_CRC_RAS_UNVERIFIED},
        {3, {0, 0, 1}, {1, 0, 0}, DSD_CRC_FAILED | DSD_CRC_RAS_UNVERIFIED},
    };

    for (size_t i = 0; i < sizeof confirmed / sizeof confirmed[0]; ++i) {
        send_confirmed(opts, state, confirmed[i].blocks, confirmed[i].crc9_ok, confirmed[i].ras);
        expect_event(state, slot, before++, 1, confirmed[i].flags, "Unknown PDU");
    }

    // MBC: a RAS-accepted header with a checked continuation keeps RAS (not a failure) through the MBC's CRC scope.
    for (int ras = 1; ras >= 0; --ras) {
        const uint16_t lpcn = ras ? 291U : 292U;
        send_mbc_grant(opts, state, ras, lpcn);
        expect_event(state, slot, before++, 1, ras ? DSD_CRC_RAS_UNVERIFIED : 0U,
                     ras ? "Learned LCN 0291 -> 851.012500 MHz" : "Learned LCN 0292 -> 851.012500 MHz");
    }

    // The separate relaxed-CRC configuration accepts failures but never runs the RAS heuristic: the same RAS-shaped
    // header and block are plain failures there.
    opts->aggressive_framesync = 1;
    send_ras_header(opts, state, unknown_hdr);
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
    expect_event(state, slot, before++, 1, DSD_CRC_FAILED, "Unknown PDU");
    const int ras_block_ok[1] = {0};
    const int ras_block[1] = {1};
    send_confirmed(opts, state, 1, ras_block_ok, ras_block);
    expect_event(state, slot, before++, 1, DSD_CRC_FAILED, "Unknown PDU");
    opts->aggressive_framesync = 0;
    opts->dmr_crc_relaxed_default = 0;
}

// A RAS-accepted voice LC header publishes the call marked [RAS] at the voice row's ordinary severity, which the
// terminal colours by category; a checked header publishes it unmarked.
static void
test_ras_voice_row(void) {
    for (int ras = 0; ras <= 1; ++ras) {
        dsd_opts* opts = calloc(1, sizeof(*opts));
        dsd_state* state = calloc(1, sizeof(*state));
        assert(opts && state);
        state->event_history_s = calloc(2, sizeof(Event_History_I));
        assert(state->event_history_s);
        for (int slot = 0; slot < 2; ++slot) {
            init_event_history(&state->event_history_s[slot], 0, 255);
        }
        opts->aggressive_framesync = 0;
        state->currentslot = 0;
        state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
        // Group voice LC, TG 2002 / SRC 1001; RS(12,9) parity under the voice LC header mask.
        uint8_t word[12] = {0, 0, 0, 0, 7, 0xD2, 0, 3, 0xE9, 0, 0, 0};
        dmr_test_encode_rs_12_9(word, word + 9);
        for (size_t i = 9; i < 12; ++i) {
            word[i] ^= 0x96U;
        }
        if (ras) {
            // A RAS system's private check: zero parity under the mask is beyond what RS(12,9) corrects, so the
            // standard check fails outright (one flipped octet would just be corrected).
            word[9] = word[10] = word[11] = 0x96U;
        }
        uint8_t bits[96];
        dsd_unpack_bytes_to_bits(word, sizeof word, bits, sizeof bits, sizeof word);
        send_burst_bits(opts, state, bits, ras ? k_ras_reserved : k_no_reserved, 0x01);

        dsd_call_snapshot call;
        assert(dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
        assert(call.ota_source_id == 1001U && call.ota_target_id == 2002U);
        const uint8_t flags = ras ? DSD_CRC_RAS_UNVERIFIED : 0U;
        assert(call.crc_invalid == flags);
        const Event_History* row = &state->event_history_s[0].Event_History_Items[0];
        assert(strstr(row->event_string, "TGT: 00002002; SRC: 00001001;") != NULL);
        assert(row->category == DSD_EVENT_CATEGORY_VOICE);
        expect_row(row, flags, DSD_EVENT_SEVERITY_INFO);
        dsd_state_ext_free_all(state);
        free(state->event_history_s);
        free(state);
        free(opts);
    }
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    state->event_history_s = calloc(2, sizeof(Event_History_I));
    assert(state->event_history_s);
    for (int slot = 0; slot < 2; ++slot) {
        init_event_history(&state->event_history_s[slot], 0, 255);
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dmr-event-crc");
    assert(fd >= 0);
    assert(dsd_close(fd) == 0);
    DSD_SNPRINTF(opts->event_out_file, sizeof opts->event_out_file, "%s", path);

    opts->aggressive_framesync = 1;
    uint64_t before = state->event_history_s[0].push_seq;
    send_udt(opts, state, 1, 0);
    expect_event(state, 0, before, 0, 0, "");
    send_mnis(opts, state, 1, 1, 0);
    expect_event(state, 0, before, 0, 0, "");
    send_unknown(opts, state, 1, 0, 1);
    expect_event(state, 0, before, 0, 0, "");

    /* A CRC-valid next packet remains usable after strict rejection. */
    send_udt(opts, state, 1, 1);
    expect_event(state, 0, before, 1, 0, "HELLO");
    before++;
    send_mnis(opts, state, 1, 1, 1);
    expect_event(state, 0, before, 1, 0, "IP ID: 5501");
    before++;
    send_captured_mnis(opts, state, 0);
    expect_event(state, 0, before++, 1, 0, "IP ID: F732");
    send_captured_mnis(opts, state, 1);
    expect_event(state, 0, before, 0, 0, "");

    opts->aggressive_framesync = 0;
    send_udt(opts, state, 1, 0);
    expect_event(state, 0, before++, 1, 1, "HELLO");
    send_mnis(opts, state, 1, 1, 0);
    expect_event(state, 0, before++, 1, 1, "IP ID: 5501");
    send_mnis(opts, state, 0, 1, 1);
    expect_event(state, 0, before++, 1, 1, "IP ID: 5501");
    send_mnis(opts, state, 1, 0, 1);
    expect_event(state, 0, before++, 1, 1, "IP ID: 5501");
    send_udt(opts, state, 0, 1);
    expect_event(state, 0, before++, 1, 1, "HELLO");
    send_unknown(opts, state, 0, 1, 0);
    expect_event(state, 0, before++, 1, 1, "Unknown PDU");
    send_unknown(opts, state, 1, 0, 1);
    expect_event(state, 0, before++, 1, 1, "ENC PDU");
    send_udt(opts, state, 1, 1);
    expect_event(state, 0, before++, 1, 0, "HELLO");

    // Fully BPTC-encoded header 025000002A00001881000000, with reserved R bits = 4.
    // Its ordinary header CRC fails, but no FEC error prevents the relaxed RAS heuristic.
    // The following packet CRC passes: only the original header verdict can mark this event.
    static const uint8_t ras_burst[25] = {0x48, 0x14, 0x82, 0x24, 0x24, 0x3A, 0x08, 0xA8, 0x01, 0x60, 0x11, 0x21, 0x01,
                                          0x52, 0x8B, 0x85, 0x08, 0x60, 0x0C, 0x40, 0x19, 0x20, 0x46, 0x0C, 0xA0};
    uint8_t info[200];
    uint8_t block[12] = {0};
    InitAllFecFunction();
    assert(dmr_test_check_reference_burst());
    unpack(ras_burst, info, sizeof ras_burst);
    dmr_data_burst_handler(opts, state, info, 0x06, NULL);
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
    expect_event(state, 0, before++, 1, DSD_CRC_RAS_UNVERIFIED, "Unknown PDU");
    send_unknown(opts, state, 1, 1, 0);
    expect_event(state, 0, before++, 1, 0, "Unknown PDU");

    // A rejected replacement header must disarm an older, still-armed assembly.
    opts->aggressive_framesync = 1;
    uint8_t hdr[12] = {0x02, 0x50, 0, 0, 42, 0, 0, 24, 0x81, 0, 0, 0};
    header(opts, state, hdr, 1);
    header(opts, state, hdr, 0);
    dmr_block_assembler(opts, state, block, 12, 0x07, 1);
    expect_event(state, 0, before, 0, 0, "");
    opts->aggressive_framesync = 0;

    // Preserve RAS MBC decoding only with a checked continuation, without laundering its header.
    // This BPTC codeword carries C_ALOHA (190000000000000000000000), reserved bits = 4.
    static const uint8_t ras_mbc[25] = {0x40, 0x10, 0x00, 0x25, 0x00, 0x04, 0x00, 0x20, 0x00, 0x38, 0x01, 0x10, 0x01,
                                        0x80, 0x08, 0x80, 0x0D, 0x00, 0x04, 0x00, 0x28, 0x00, 0x24, 0x00, 0x40};
    for (int mode = 0; mode < 3; mode++) {
        opts->aggressive_framesync = mode == 1;
        state->dmr_t3_syscode = 0xFFFF;
        unpack(ras_mbc, info, sizeof ras_mbc);
        dmr_data_burst_handler(opts, state, info, 0x04, NULL);
        DSD_MEMSET(block, 0, sizeof block);
        block[0] = 0x80; // Last continuation.
        uint8_t bits[96];
        unpack(block, bits, sizeof block);
        const uint16_t crc = (uint16_t)dsd_crc_ccitt16_bits(bits, 80);
        block[10] = (uint8_t)(crc >> 8);
        block[11] = (uint8_t)(crc ^ (mode == 2));
        // The continuation goes through the real burst handler: it has no CRC of its own and must not classify
        // itself, so the header's RAS provenance and the continuation's CRC16 decide.
        unpack(block, bits, sizeof block);
        send_burst_bits(opts, state, bits, k_no_reserved, 0x05);
        assert(state->dmr_t3_syscode == (mode == 0 ? 0 : 0xFFFF));
    }
    opts->aggressive_framesync = 0;

    state->currentslot = 1;
    send_udt(opts, state, 1, 1);
    expect_event(state, 1, 0, 1, 0, "HELLO");
    /* The separately configured DMR relaxation has the same publication policy as -F. */
    opts->aggressive_framesync = 1;
    opts->dmr_crc_relaxed_default = 1;
    send_udt(opts, state, 1, 0);
    expect_event(state, 1, 1, 1, 1, "HELLO");

    FILE* log = fopen(path, "rb");
    assert(log);
    char line[4096];
    unsigned marked = 0;
    unsigned ras = 0;
    unsigned clean = 0;
    unsigned marked_text = 0;
    while (fgets(line, sizeof line, log)) {
        if (strstr(line, "[CRC ERR]")) {
            marked++;
            marked_text += strstr(line, "Text:") != NULL && strstr(line, "HELLO") != NULL;
        } else if (strstr(line, "[RAS]")) {
            ras++;
        } else {
            clean++;
        }
    }
    fclose(log);
    // The RAS-accepted header's event is [RAS] in -J, not [CRC ERR].
    assert(marked + ras + clean > 0U);
    assert(marked == 11 && ras == 1 && clean == 9 && marked_text == 3);
    assert(remove(path) == 0);
    opts->event_out_file[0] = '\0';
    test_ras_data_provenance(opts, state);
    test_ras_voice_row();
    int rc = test_mnis_location_file(opts, state);
    dsd_state_ext_free_all(state);
    free(state->event_history_s);
    free(state);
    free(opts);
    if (rc == 0) {
        puts("DMR_EVENT_CRC: PASS");
    }
    return rc;
}
