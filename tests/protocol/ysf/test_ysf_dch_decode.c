// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <mbelib-neo/mbelib.h>
#include <stdint.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "ysf_frame.h"
#include "ysf_internal.h"

void LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state); // NOLINT(misc-use-internal-linkage)
int Connect(char* hostname, int portno);                               // NOLINT(misc-use-internal-linkage)
int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state,
                                       int* out_analog_signal); // NOLINT(misc-use-internal-linkage)
void __wrap_processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24],
                            char imbe7100_fr[7][24]); // NOLINT(misc-use-internal-linkage)
int __wrap_mbe_processAmbe2450Dataf(float* aout_buf, mbe_process_result* result, const char ambe_d[49],
                                    mbe_parms* cur_mp, mbe_parms* prev_mp,
                                    mbe_parms* prev_mp_enhanced); // NOLINT(misc-use-internal-linkage)

static uint8_t g_dibit_stream[512];
static size_t g_dibit_stream_len;
static size_t g_dibit_stream_pos;
static int g_mbe_call_count;
static int g_process_mbe_call_count;
static int g_process_mbe_synctype[5];
static int g_process_mbe_ambe_nonnull[5];
static int g_mbe_inbound_errs2[5];

static void
begin_test_ysf_call(dsd_state* state) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_YSF_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
}

static dsd_call_snapshot
get_test_ysf_call(const dsd_state* state) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    assert(dsd_call_state_get(state, 0U, &call) == 1);
    return call;
}

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    (void)out_analog_signal;
    if (g_dibit_stream_pos >= g_dibit_stream_len) {
        return 0;
    }
    return g_dibit_stream[g_dibit_stream_pos++];
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24],
                       char imbe7100_fr[7][24]) {
    (void)opts;
    (void)imbe_fr;
    (void)imbe7100_fr;

    assert(g_process_mbe_call_count < 5);
    g_process_mbe_synctype[g_process_mbe_call_count] = state->synctype;
    g_process_mbe_ambe_nonnull[g_process_mbe_call_count] = (ambe_fr != NULL);
    g_process_mbe_call_count++;
}

static int
parity32(uint32_t value) {
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value &= 0xFU;
    return (0x6996U >> value) & 1U;
}

static void
encode_k5_bits_to_dibits(const uint8_t* bits, size_t bit_count, uint8_t* dibits) {
    uint32_t reg = 0U;

    for (size_t i = 0; i < bit_count; i++) {
        reg = (reg << 1U) | (uint32_t)(bits[i] & 1U);
        uint8_t b0 = (uint8_t)parity32(reg & 0x19U);
        uint8_t b1 = (uint8_t)parity32(reg & 0x17U);
        dibits[i] = (uint8_t)((b0 << 1U) | b1);
    }
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

int
Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return -1;
}

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_mbe_processAmbe2450Dataf(float* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp,
                                mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    (void)ambe_d;
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;

    assert(g_mbe_call_count < 5);
    g_mbe_inbound_errs2[g_mbe_call_count] = result->total_errors;
    result->total_errors = 4 + g_mbe_call_count;
    result->protected_errors = result->total_errors;
    for (size_t i = 0; i < 160U; i++) {
        aout_buf[i] = (float)(g_mbe_call_count + 1);
    }
    g_mbe_call_count++;
    return result->total_errors;
}

static void
pack_bytes_to_bits(const char* bytes, size_t byte_count, uint8_t* bits) {
    for (size_t i = 0; i < byte_count; i++) {
        uint8_t value = (uint8_t)bytes[i];
        for (size_t bit = 0; bit < 8U; bit++) {
            bits[(i * 8U) + bit] = (uint8_t)((value >> (7U - bit)) & 1U);
        }
    }
}

static void
set_bits_msb(uint8_t* bits, size_t offset, size_t width, uint32_t value) {
    for (size_t i = 0; i < width; i++) {
        bits[offset + i] = (uint8_t)((value >> (width - 1U - i)) & 1U);
    }
}

static void
append_ysf_crc(uint8_t fich_bits[48]) {
    for (uint32_t crc = 0; crc <= 0xFFFFU; crc++) {
        for (size_t bit = 0; bit < 16U; bit++) {
            fich_bits[32U + bit] = (uint8_t)((crc >> (15U - bit)) & 1U);
        }
        if (ysf_crc16(fich_bits, 48) == 0U) {
            return;
        }
    }

    assert(!"reachable CRC value not found");
}

static uint8_t
test_ysf_pn95_bit(size_t bit_index) {
    uint16_t lfsr = 0x1C9U;
    bit_index %= 512U;

    for (size_t i = 0; i <= bit_index; i++) {
        uint8_t bit = (uint8_t)(lfsr & 1U);
        uint16_t feedback = (uint16_t)(((lfsr >> 4U) ^ lfsr) & 1U);
        lfsr = (uint16_t)((lfsr >> 1U) | (feedback << 8U));
        if (i == bit_index) {
            return bit;
        }
    }

    return 0U;
}

static void
append_crc_to_payload(uint8_t* bits, size_t total_bits) {
    assert(total_bits >= 16U);

    for (uint32_t crc = 0; crc <= 0xFFFFU; crc++) {
        for (size_t bit = 0; bit < 16U; bit++) {
            bits[(total_bits - 16U) + bit] = (uint8_t)((crc >> (15U - bit)) & 1U);
        }
        if (ysf_crc16(bits, (int)total_bits) == 0U) {
            return;
        }
    }

    assert(!"reachable DCH CRC value not found");
}

static void
encode_dch_payload_to_input(const char* payload, size_t byte_count, uint8_t* input, size_t total_bits, size_t columns,
                            int corrupt_crc) {
    uint8_t bits[180];
    uint8_t dibits[180];
    size_t payload_bits = byte_count * 8U;
    size_t interleaved_slots = columns * 20U;

    assert(total_bits <= sizeof(bits));
    assert(total_bits <= sizeof(dibits));
    assert(total_bits <= interleaved_slots);
    assert(interleaved_slots <= sizeof(dibits));

    DSD_MEMSET(bits, 0, sizeof(bits));
    DSD_MEMSET(dibits, 0, sizeof(dibits));
    DSD_MEMSET(input, 0, interleaved_slots);
    pack_bytes_to_bits(payload, byte_count, bits);

    for (size_t i = 0; i < payload_bits; i++) {
        bits[i] ^= test_ysf_pn95_bit(i);
    }
    append_crc_to_payload(bits, total_bits);
    if (corrupt_crc) {
        bits[3] ^= 1U;
    }

    encode_k5_bits_to_dibits(bits, interleaved_slots, dibits);
    for (size_t i = 0; i < 20U; i++) {
        for (size_t j = 0; j < columns; j++) {
            input[i + (j * 20U)] = dibits[j + (i * columns)];
        }
    }
}

static void
make_fich_bits(uint8_t fich_bits[48]) {
    DSD_MEMSET(fich_bits, 0, 48U);
    set_bits_msb(fich_bits, 0U, 2U, 1U);   // fi
    set_bits_msb(fich_bits, 4U, 2U, 3U);   // cm
    set_bits_msb(fich_bits, 6U, 2U, 2U);   // bn
    set_bits_msb(fich_bits, 8U, 2U, 1U);   // bt
    set_bits_msb(fich_bits, 10U, 3U, 5U);  // fn
    set_bits_msb(fich_bits, 13U, 3U, 6U);  // ft
    set_bits_msb(fich_bits, 18U, 3U, 4U);  // mr
    set_bits_msb(fich_bits, 21U, 1U, 1U);  // vp
    set_bits_msb(fich_bits, 22U, 2U, 2U);  // dt
    set_bits_msb(fich_bits, 24U, 1U, 1U);  // st
    set_bits_msb(fich_bits, 25U, 7U, 42U); // sc
    append_ysf_crc(fich_bits);
}

static void
make_fich_bits_for_full_rate_data(uint8_t fich_bits[48]) {
    DSD_MEMSET(fich_bits, 0, 48U);
    set_bits_msb(fich_bits, 0U, 2U, 0U);  // fi: header communication channel
    set_bits_msb(fich_bits, 4U, 2U, 0U);  // cm: group/CQ
    set_bits_msb(fich_bits, 6U, 2U, 0U);  // bn
    set_bits_msb(fich_bits, 8U, 2U, 1U);  // bt
    set_bits_msb(fich_bits, 10U, 3U, 0U); // fn
    set_bits_msb(fich_bits, 13U, 3U, 1U); // ft
    set_bits_msb(fich_bits, 18U, 3U, 1U); // mr
    set_bits_msb(fich_bits, 21U, 1U, 0U); // vp
    set_bits_msb(fich_bits, 22U, 2U, 1U); // dt: full-rate data
    set_bits_msb(fich_bits, 24U, 1U, 0U); // st
    set_bits_msb(fich_bits, 25U, 7U, 0U); // sc
    append_ysf_crc(fich_bits);
}

static void
make_fich_bits_for_vd_type1(uint8_t fich_bits[48]) {
    DSD_MEMSET(fich_bits, 0, 48U);
    set_bits_msb(fich_bits, 0U, 2U, 1U);  // fi: V/D mode
    set_bits_msb(fich_bits, 4U, 2U, 0U);  // cm: group/CQ
    set_bits_msb(fich_bits, 6U, 2U, 0U);  // bn
    set_bits_msb(fich_bits, 8U, 2U, 0U);  // bt
    set_bits_msb(fich_bits, 10U, 3U, 0U); // fn: destination/source
    set_bits_msb(fich_bits, 13U, 3U, 0U); // ft
    set_bits_msb(fich_bits, 18U, 3U, 1U); // mr
    set_bits_msb(fich_bits, 21U, 1U, 0U); // vp
    set_bits_msb(fich_bits, 22U, 2U, 0U); // dt: V/D Type 1
    set_bits_msb(fich_bits, 24U, 1U, 0U); // st
    set_bits_msb(fich_bits, 25U, 7U, 0U); // sc
    append_ysf_crc(fich_bits);
}

static void
make_fich_bits_for_vd_type2(uint8_t fich_bits[48]) {
    DSD_MEMSET(fich_bits, 0, 48U);
    set_bits_msb(fich_bits, 0U, 2U, 1U);  // fi: V/D mode
    set_bits_msb(fich_bits, 4U, 2U, 0U);  // cm: group/CQ
    set_bits_msb(fich_bits, 6U, 2U, 0U);  // bn
    set_bits_msb(fich_bits, 8U, 2U, 0U);  // bt
    set_bits_msb(fich_bits, 10U, 3U, 1U); // fn: source
    set_bits_msb(fich_bits, 13U, 3U, 5U); // ft
    set_bits_msb(fich_bits, 18U, 3U, 1U); // mr
    set_bits_msb(fich_bits, 21U, 1U, 0U); // vp
    set_bits_msb(fich_bits, 22U, 2U, 2U); // dt: V/D Type 2
    set_bits_msb(fich_bits, 24U, 1U, 0U); // st
    set_bits_msb(fich_bits, 25U, 7U, 0U); // sc
    append_ysf_crc(fich_bits);
}

static void
make_fich_bits_for_terminator(uint8_t fich_bits[48]) {
    DSD_MEMSET(fich_bits, 0, 48U);
    set_bits_msb(fich_bits, 0U, 2U, 2U);  // fi: terminator communication channel
    set_bits_msb(fich_bits, 4U, 2U, 0U);  // cm: group/CQ
    set_bits_msb(fich_bits, 6U, 2U, 0U);  // bn
    set_bits_msb(fich_bits, 8U, 2U, 0U);  // bt
    set_bits_msb(fich_bits, 10U, 3U, 0U); // fn
    set_bits_msb(fich_bits, 13U, 3U, 0U); // ft
    set_bits_msb(fich_bits, 18U, 3U, 1U); // mr
    set_bits_msb(fich_bits, 21U, 1U, 0U); // vp
    set_bits_msb(fich_bits, 22U, 2U, 0U); // dt: V/D Type 1
    set_bits_msb(fich_bits, 24U, 1U, 0U); // st
    set_bits_msb(fich_bits, 25U, 7U, 0U); // sc
    append_ysf_crc(fich_bits);
}

static void
encode_fich_input(const uint8_t fich_bits[48], int corrupt_payload_crc, int corrupt_golay, uint8_t input[100]) {
    uint8_t encoded_words[96];
    uint8_t source_bits[48];
    uint8_t dibits[100];

    for (size_t i = 0; i < 100U; i++) {
        input[i] = 0U;
        dibits[i] = 0U;
    }
    DSD_MEMCPY(source_bits, fich_bits, 48U);
    if (corrupt_payload_crc) {
        source_bits[7] ^= 1U;
    }

    for (size_t word = 0; word < 4U; word++) {
        uint8_t msg[12];
        for (size_t bit = 0; bit < 12U; bit++) {
            msg[bit] = source_bits[(word * 12U) + bit];
        }
        Golay_24_12_encode(msg, &encoded_words[word * 24U]);
    }

    if (corrupt_golay) {
        encoded_words[12] ^= 1U;
        encoded_words[13] ^= 1U;
        encoded_words[14] ^= 1U;
        encoded_words[15] ^= 1U;
        encoded_words[16] ^= 1U;
    }

    encode_k5_bits_to_dibits(encoded_words, 96U, dibits);
    for (size_t i = 0; i < 20U; i++) {
        for (size_t j = 0; j < 5U; j++) {
            input[i + (j * 20U)] = dibits[j + (i * 5U)];
        }
    }
}

static void
append_dibits_to_stream(const uint8_t* dibits, size_t count) {
    assert(g_dibit_stream_len + count <= sizeof(g_dibit_stream));
    DSD_MEMCPY(&g_dibit_stream[g_dibit_stream_len], dibits, count);
    g_dibit_stream_len += count;
}

static void
append_interleaved_full_rate_dch_blocks(const uint8_t dch0[180], const uint8_t dch1[180]) {
    for (size_t block = 0; block < 5U; block++) {
        append_dibits_to_stream(&dch0[block * 36U], 36U);
        append_dibits_to_stream(&dch1[block * 36U], 36U);
    }
}

static void
append_vd_type1_blocks(const uint8_t dch[180], uint8_t voice_seed) {
    uint8_t voice[36];

    for (size_t block = 0; block < 5U; block++) {
        append_dibits_to_stream(&dch[block * 36U], 36U);
        for (size_t i = 0; i < sizeof(voice); i++) {
            voice[i] = (uint8_t)((voice_seed + block + i) & 3U);
        }
        append_dibits_to_stream(voice, sizeof(voice));
    }
}

static void
append_type2_vech_dibits(uint8_t error_bit) {
    for (size_t i = 0; i < 52U; i++) {
        uint8_t msb_index = dsd_ysf_vd2_interleave_index(i * 2U);
        uint8_t lsb_index = dsd_ysf_vd2_interleave_index((i * 2U) + 1U);
        uint8_t desired_msb = 0U;
        uint8_t desired_lsb = (lsb_index == 103U) ? error_bit : 0U;
        uint8_t source_msb = desired_msb ^ test_ysf_pn95_bit(msb_index);
        uint8_t source_lsb = desired_lsb ^ test_ysf_pn95_bit(lsb_index);
        uint8_t dibit = (uint8_t)((source_msb << 1U) | source_lsb);
        append_dibits_to_stream(&dibit, 1U);
    }
}

static void
test_dch_csd1_tracks_destination_and_source(void) {
    static dsd_state state;
    uint8_t bits[160];
    const char payload[] = "DESTCALL01SRCCALL02";

    DSD_MEMSET(&state, 0, sizeof(state));
    begin_test_ysf_call(&state);
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bytes_to_bits(payload, 20U, bits);

    ysf_dch_decode(&state, 0, 0, 0, 0, 0, bits);

    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "DESTCALL01") == 0);
    assert(strcmp(call.source_text, "SRCCALL02") == 0);
}

static void
test_dch_rid_mode_preserves_target_and_source_state(void) {
    static dsd_state state;
    uint8_t bits[160];

    DSD_MEMSET(&state, 0, sizeof(state));
    begin_test_ysf_call(&state);
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bytes_to_bits("DSTR1SRCR2FULLSRC003", 20U, bits);

    ysf_dch_decode(&state, 0, 0, 0, 0, 1, bits);

    dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "DSTR1SRCR2") == 0);
    assert(strcmp(call.source_text, "FULLSRC003") == 0);
    const uint64_t first_epoch = call.epoch;

    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bytes_to_bits("RID01RID02", 10U, bits);

    ysf_dch_decode2(&state, 0, 0, 0, 5, 1, bits);

    call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "RID01RID02") == 0);
    assert(call.source_text[0] == '\0');
    assert(call.epoch != first_epoch);
}

static void
test_dch_csd2_tracks_uplink_and_downlink(void) {
    static dsd_state state;
    uint8_t bits[160];
    const char payload[] = "UPLINK0001DOWNLINK02";

    DSD_MEMSET(&state, 0, sizeof(state));
    begin_test_ysf_call(&state);
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bytes_to_bits(payload, 20U, bits);

    ysf_dch_decode(&state, 1, 0, 0, 0, 0, bits);

    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.route_text[0], "UPLINK0001") == 0);
    assert(strcmp(call.route_text[1], "DOWNLINK02") == 0);
}

static void
test_dch_text_clears_and_sanitizes_segments(void) {
    static dsd_state state;
    uint8_t bits[160];
    char payload[20] = {'A', '\n', 0x19, 0x7F, 'Z', ' ', '1', '2', '3', '4',
                        'b', 'a',  'd',  0x01, 'x', 'y', 'z', '!', '?', '~'};

    DSD_MEMSET(&state, 0x5A, sizeof(state));
    state.event_history_s = NULL;
    DSD_MEMSET(bits, 0, sizeof(bits));
    pack_bytes_to_bits(payload, sizeof(payload), bits);

    ysf_dch_decode(&state, 2, 0, 0, 1, 0, bits);

    assert(state.ysf_txt[0][0] == 'A');
    assert(state.ysf_txt[0][1] == ' ');
    assert(state.ysf_txt[0][2] == ' ');
    assert(state.ysf_txt[0][3] == ' ');
    assert(state.ysf_txt[0][4] == 'Z');
    assert(state.ysf_txt[0][13] == ' ');
    assert(state.ysf_txt[0][19] == '~');
}

static void
test_dch2_tracks_destination_source_links_and_remarks(void) {
    static dsd_state state;
    uint8_t bits[80];

    DSD_MEMSET(&state, 0, sizeof(state));
    begin_test_ysf_call(&state);

    pack_bytes_to_bits("DEST2CALL3", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 0, 5, 0, bits);
    dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "DEST2CALL3") == 0);

    pack_bytes_to_bits("SRC2CALL45", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 1, 5, 0, bits);
    call = get_test_ysf_call(&state);
    assert(strcmp(call.source_text, "SRC2CALL45") == 0);

    pack_bytes_to_bits("UP2CALL678", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 2, 5, 0, bits);
    call = get_test_ysf_call(&state);
    assert(strcmp(call.route_text[0], "UP2CALL678") == 0);

    pack_bytes_to_bits("DN2CALL901", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 3, 5, 0, bits);
    call = get_test_ysf_call(&state);
    assert(strcmp(call.route_text[1], "DN2CALL901") == 0);

    pack_bytes_to_bits("RM1AARM2BB", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 4, 5, 0, bits);
    assert(strcmp(state.ysf_rm1, "RM1AA") == 0);
    assert(strcmp(state.ysf_rm2, "RM2BB") == 0);

    pack_bytes_to_bits("RM3CCRM4DD", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 5, 5, 0, bits);
    assert(strcmp(state.ysf_rm3, "RM3CC") == 0);
    assert(strcmp(state.ysf_rm4, "RM4DD") == 0);
}

static void
test_ysf_crc16_known_values(void) {
    uint8_t bits[48];

    DSD_MEMSET(bits, 0, sizeof(bits));
    assert(ysf_crc16(bits, 0) == 0xFFFFU);
    assert(ysf_crc16(bits, 48) == 0xFFFFU);

    for (size_t i = 0; i < sizeof(bits); i++) {
        bits[i] = (uint8_t)((i * 3U) & 1U);
    }
    assert(ysf_crc16(bits, 48) == 0x2DF0U);
}

static void
test_ysf_fich_conv_decodes_fields_and_failures(void) {
    uint8_t fich_bits[48];
    uint8_t input[100];
    uint8_t dest[32];
    uint32_t v_error = UINT32_MAX;

    InitAllFecFunction();
    make_fich_bits(fich_bits);

    encode_fich_input(fich_bits, 0, 0, input);
    DSD_MEMSET(dest, 0xA5, sizeof(dest));
    assert(ysf_conv_fich(input, dest, &v_error) == 0);
    assert(v_error != UINT32_MAX);
    assert(memcmp(dest, fich_bits, 32U) == 0);
    assert(convert_bits_into_output(&dest[0], 2U) == 1U);
    assert(convert_bits_into_output(&dest[4], 2U) == 3U);
    assert(convert_bits_into_output(&dest[6], 2U) == 2U);
    assert(convert_bits_into_output(&dest[8], 2U) == 1U);
    assert(convert_bits_into_output(&dest[10], 3U) == 5U);
    assert(convert_bits_into_output(&dest[13], 3U) == 6U);
    assert(convert_bits_into_output(&dest[18], 3U) == 4U);
    assert(dest[21] == 1U);
    assert(convert_bits_into_output(&dest[22], 2U) == 2U);
    assert(dest[24] == 1U);
    assert(convert_bits_into_output(&dest[25], 7U) == 42U);

    encode_fich_input(fich_bits, 1, 0, input);
    assert(ysf_conv_fich(input, dest, NULL) == -2);

    encode_fich_input(fich_bits, 0, 1, input);
    assert(ysf_conv_fich(input, dest, NULL) != 0);
}

static void
test_ysf_type2_ambe_majority_and_tail_bits(void) {
    uint8_t vech_bits[104];
    uint8_t temp[512];
    char ambe_d[49];

    DSD_MEMSET(vech_bits, 0, sizeof(vech_bits));
    DSD_MEMSET(temp, 0xA5, sizeof(temp));
    DSD_MEMSET(ambe_d, 0x5A, sizeof(ambe_d));

    vech_bits[0] = 1U;
    vech_bits[1] = 1U;
    vech_bits[2] = 0U;
    vech_bits[3] = 1U;
    vech_bits[4] = 0U;
    vech_bits[5] = 0U;
    for (size_t i = 0; i < 22U; i++) {
        vech_bits[81U + i] = (uint8_t)(i & 1U);
    }

    ysf_build_type2_ambe(vech_bits, temp, ambe_d);

    assert(temp[0] == 1U);
    assert(temp[1] == 0U);
    for (size_t i = 0; i < 22U; i++) {
        assert(temp[27U + i] == (uint8_t)(i & 1U));
    }
    for (size_t i = 0; i < 49U; i++) {
        assert((uint8_t)ambe_d[i] == temp[i]);
    }
    assert(temp[49] == 0U);
}

static void
test_ysf_conv_dch_decodes_valid_csd1_and_rejects_crc_error(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t input[180];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    begin_test_ysf_call(&state);
    encode_dch_payload_to_input("DCHDST0001DCHSRC0002", 20U, input, 176U, 9U, 0);

    assert(ysf_conv_dch(&opts, &state, 0, 0, 0, 0, 0, input) == 0);
    dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "DCHDST0001") == 0);
    assert(strcmp(call.source_text, "DCHSRC0002") == 0);

    encode_dch_payload_to_input("BADDST0001BADSRC0002", 20U, input, 176U, 9U, 1);

    assert(ysf_conv_dch(&opts, &state, 0, 0, 0, 0, 0, input) == -2);
    call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "DCHDST0001") == 0);
    assert(strcmp(call.source_text, "DCHSRC0002") == 0);
}

static void
test_ysf_conv_dch2_decodes_valid_source_and_rejects_crc_error(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t input[100];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    begin_test_ysf_call(&state);
    encode_dch_payload_to_input("DCH2SRC789", 10U, input, 96U, 5U, 0);

    assert(ysf_conv_dch2(&opts, &state, 0, 0, 1, 5, 0, input) == 0);
    dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.source_text, "DCH2SRC789") == 0);

    encode_dch_payload_to_input("DCH2BAD789", 10U, input, 96U, 5U, 1);

    assert(ysf_conv_dch2(&opts, &state, 0, 0, 1, 5, 0, input) == -2);
    call = get_test_ysf_call(&state);
    assert(strcmp(call.source_text, "DCH2SRC789") == 0);
}

static void
test_process_ysf_full_rate_data_routes_fich_and_dch_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch0[180];
    uint8_t dch1[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;

    make_fich_bits_for_full_rate_data(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("PYSFDST001PYSFSRC002", 20U, dch0, 176U, 9U, 0);
    encode_dch_payload_to_input("PYSFUPL003PYSFDNL004", 20U, dch1, 176U, 9U, 0);

    append_dibits_to_stream(fich_input, 100U);
    append_interleaved_full_rate_dch_blocks(dch0, dch1);

    processYSF(&opts, &state);

    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(state.ysf_fi == 0U);
    assert(state.ysf_dt == 1U);
    assert(state.ysf_cm == 0U);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    assert(dsd_call_state_get(&state, 0U, &call) == 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
}

static void
test_process_ysf_vd_type1_routes_ehr_voice_and_dch_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    DSD_MEMSET(g_process_mbe_synctype, 0, sizeof(g_process_mbe_synctype));
    DSD_MEMSET(g_process_mbe_ambe_nonnull, 0, sizeof(g_process_mbe_ambe_nonnull));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_process_mbe_call_count = 0;

    make_fich_bits_for_vd_type1(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);

    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);

    processYSF(&opts, &state);

    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_process_mbe_call_count == 4);
    for (size_t i = 0; i < 4U; i++) {
        assert(g_process_mbe_synctype[i] == DSD_SYNC_NXDN_POS);
        assert(g_process_mbe_ambe_nonnull[i] == 1);
    }
    assert(state.synctype == 0);
    assert(state.ysf_fi == 1U);
    assert(state.ysf_dt == 0U);
    assert(state.ysf_cm == 0U);
    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "VD1DST0001") == 0);
    assert(strcmp(call.source_text, "VD1SRC0002") == 0);
}

static void
test_process_ysf_vd_type2_routes_dch2_voice_and_audio_errors(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch2[100];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    DSD_MEMSET(g_mbe_inbound_errs2, 0, sizeof(g_mbe_inbound_errs2));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_mbe_call_count = 0;
    opts.floating_point = 1;

    make_fich_bits_for_vd_type2(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD2SRC4444", 10U, dch2, 96U, 5U, 0);

    append_dibits_to_stream(fich_input, 100U);
    for (size_t block = 0; block < 5U; block++) {
        append_dibits_to_stream(&dch2[block * 20U], 20U);
        append_type2_vech_dibits((block % 2U) == 0U ? 1U : 0U);
    }

    processYSF(&opts, &state);

    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_mbe_call_count == 5);
    assert(g_mbe_inbound_errs2[0] == 1);
    assert(g_mbe_inbound_errs2[1] == 0);
    assert(g_mbe_inbound_errs2[2] == 1);
    assert(g_mbe_inbound_errs2[3] == 0);
    assert(g_mbe_inbound_errs2[4] == 1);
    assert(state.debug_audio_errors == 3);
    assert(state.ysf_fi == 1U);
    assert(state.ysf_dt == 2U);
    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.source_text, "VD2SRC4444") == 0);
    assert(strcmp(state.err_str, "========") == 0);
    assert(state.f_l[0] == 5.0F);
    assert(state.f_l[159] == 5.0F);
}

static void
test_process_ysf_terminator_enriches_identity_before_call_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    static Event_History_I event_history[2];
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch0[180];
    uint8_t dch1[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(event_history, 0, sizeof(event_history));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    state.event_history_s = event_history;
    init_event_history(&event_history[0], 0U, 255U);
    init_event_history(&event_history[1], 0U, 255U);
    begin_test_ysf_call(&state);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;

    make_fich_bits_for_terminator(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("TERMDST001TERMSRC002", 20U, dch0, 176U, 9U, 0);
    encode_dch_payload_to_input("TERMUPL003TERMDNL004", 20U, dch1, 176U, 9U, 0);

    append_dibits_to_stream(fich_input, 100U);
    append_interleaved_full_rate_dch_blocks(dch0, dch1);

    processYSF(&opts, &state);

    assert(g_dibit_stream_pos == g_dibit_stream_len);
    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(call.phase == DSD_CALL_PHASE_ENDED);
    assert(strcmp(call.target_text, "TERMDST001") == 0);
    assert(strcmp(call.source_text, "TERMSRC002") == 0);
    assert(strcmp(call.route_text[0], "TERMUPL003") == 0);
    assert(strcmp(call.route_text[1], "TERMDNL004") == 0);

    const Event_History* committed = &event_history[0].Event_History_Items[1];
    assert(strcmp(committed->tgt_str, "TERMDST001") == 0);
    assert(strcmp(committed->src_str, "TERMSRC002") == 0);
}

int
main(void) {
    test_dch_csd1_tracks_destination_and_source();
    test_dch_rid_mode_preserves_target_and_source_state();
    test_dch_csd2_tracks_uplink_and_downlink();
    test_dch_text_clears_and_sanitizes_segments();
    test_dch2_tracks_destination_source_links_and_remarks();
    test_ysf_crc16_known_values();
    test_ysf_fich_conv_decodes_fields_and_failures();
    test_ysf_type2_ambe_majority_and_tail_bits();
    test_ysf_conv_dch_decodes_valid_csd1_and_rejects_crc_error();
    test_ysf_conv_dch2_decodes_valid_source_and_rejects_crc_error();
    test_process_ysf_full_rate_data_routes_fich_and_dch_state();
    test_process_ysf_vd_type1_routes_ehr_voice_and_dch_state();
    test_process_ysf_vd_type2_routes_dch2_voice_and_audio_errors();
    test_process_ysf_terminator_enriches_identity_before_call_end();
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
