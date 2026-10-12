// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>

#include <assert.h>
#include <dsd-neo/core/ambe_interleave.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <math.h>
#include <mbelib-neo/mbelib.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#ifdef DSD_YSF_TEST_REAL_VOCODER
/* The real-vocoder build (YSF_MBE_CAPTURE) runs only its capture cases; the stubbed-vocoder cases stay compiled. */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"
#include "ysf_frame.h"
#include "ysf_internal.h"

void LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state); // NOLINT(misc-use-internal-linkage)
int Connect(char* hostname, int portno);                               // NOLINT(misc-use-internal-linkage)
int __wrap_get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state,
                                       int* out_analog_signal); // NOLINT(misc-use-internal-linkage)
void __wrap_processMbeFrame(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23],
                            char ambe_fr[4][24], char imbe7100_fr[7][24]); // NOLINT(misc-use-internal-linkage)
int __wrap_mbe_processAmbe2450Dataf(float* aout_buf, mbe_process_result* result, const char ambe_d[49],
                                    mbe_parms* cur_mp, mbe_parms* prev_mp,
                                    mbe_parms* prev_mp_enhanced); // NOLINT(misc-use-internal-linkage)

static uint8_t g_dibit_stream[4096];
static size_t g_dibit_stream_len;
static size_t g_dibit_stream_pos;
/* The read that adopts a retune its capture recorded, where the carrier boundary moves the carrier count
   (dsd_engine_carrier_boundary(), issue #575); SIZE_MAX for none. */
static size_t g_boundary_at = SIZE_MAX;
static int g_mbe_call_count;
static int g_process_mbe_call_count;
static int g_process_mbe_synctype[5];
static dsd_voice_protocol g_process_mbe_protocol[5];
static int g_process_mbe_ambe_nonnull[5];
/* The AMBE frame each stubbed processMbeFrame() call received, for V/D1 (issue #660). */
static char g_process_mbe_ambe[5][4][24];
static int g_mbe_inbound_errs2[5];
/* The AMBE frame each stubbed mbelib call decoded, and the call that fails (-1: none), for the V/D2 capture. */
static char g_mbe_ambe[5][49];
static int g_mbe_fail_at = -1;

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
    (void)out_analog_signal;
    if (g_dibit_stream_pos == g_boundary_at && state != NULL) {
        /* As dsd_engine_carrier_boundary() does: the count moves and the carrier's YSF confirmation goes with it
           (carrier_boundary_forget_evidence()). */
        state->carrier_seq++;
        state->ysf_fich_confirmed = 0;
    }
    if (g_dibit_stream_pos >= g_dibit_stream_len) {
        return 0;
    }
    return g_dibit_stream[g_dibit_stream_pos++];
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_processMbeFrame(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23],
                       char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)imbe_fr;
    (void)imbe7100_fr;

    assert(g_process_mbe_call_count < 5);
    g_process_mbe_synctype[g_process_mbe_call_count] = state->synctype;
    g_process_mbe_protocol[g_process_mbe_call_count] = protocol;
    g_process_mbe_ambe_nonnull[g_process_mbe_call_count] = (ambe_fr != NULL);
    if (ambe_fr != NULL) {
        DSD_MEMCPY(g_process_mbe_ambe[g_process_mbe_call_count], ambe_fr, sizeof(g_process_mbe_ambe[0]));
    }
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
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;

    assert(g_mbe_call_count < 5);
    g_mbe_inbound_errs2[g_mbe_call_count] = result->total_errors;
    DSD_MEMCPY(g_mbe_ambe[g_mbe_call_count], ambe_d, sizeof(g_mbe_ambe[0]));
    if (g_mbe_call_count == g_mbe_fail_at) {
        g_mbe_call_count++;
        return -1;
    }
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
make_fich_bits_for_full_rate_voice(uint8_t fich_bits[48]) {
    DSD_MEMSET(fich_bits, 0, 48U);
    set_bits_msb(fich_bits, 0U, 2U, 1U);  // fi: communication channel
    set_bits_msb(fich_bits, 4U, 2U, 0U);  // cm: group/CQ
    set_bits_msb(fich_bits, 6U, 2U, 0U);  // bn
    set_bits_msb(fich_bits, 8U, 2U, 0U);  // bt
    set_bits_msb(fich_bits, 10U, 3U, 1U); // fn: not the CSD3 frame
    set_bits_msb(fich_bits, 13U, 3U, 5U); // ft
    set_bits_msb(fich_bits, 18U, 3U, 1U); // mr
    set_bits_msb(fich_bits, 21U, 1U, 0U); // vp
    set_bits_msb(fich_bits, 22U, 2U, 3U); // dt: voice full rate
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

/* Dibit i of a V/D1 frame's VCH `block`. Every block of a frame differs (a pattern that only shifted by the block would
   repeat block 0 as block 4), so a VCH decoded twice, out of order or not at all shows (issue #660). */
static uint8_t
vd_type1_voice_dibit(uint8_t voice_seed, size_t block, size_t i) {
    uint32_t x = ((uint32_t)voice_seed * 131U) + ((uint32_t)block * 977U) + ((uint32_t)i * 31U) + 1U;
    x ^= x >> 7U;
    x *= 0x9E3779B1U;
    return (uint8_t)((x >> 15U) & 3U);
}

static void
append_vd_type1_blocks(const uint8_t dch[180], uint8_t voice_seed) {
    uint8_t voice[DSD_AMBE_2450_DIBITS];

    for (size_t block = 0; block < 5U; block++) {
        append_dibits_to_stream(&dch[block * 36U], 36U);
        for (size_t i = 0; i < sizeof(voice); i++) {
            voice[i] = vd_type1_voice_dibit(voice_seed, block, i);
        }
        append_dibits_to_stream(voice, sizeof(voice));
    }
}

/* Each of the five processMbeFrame() calls carried its own VCH, in air order: the frame's AMBE bits, read back through
   the AMBE 2450 interleave, are that VCH's dibits. */
static void
assert_vd_type1_calls_carry_each_vch(uint8_t voice_seed) {
    for (size_t a = 0; a < 5U; a++) {
        for (size_t b = a + 1U; b < 5U; b++) {
            int same = 1;
            for (size_t i = 0; i < DSD_AMBE_2450_DIBITS; i++) {
                same &= vd_type1_voice_dibit(voice_seed, a, i) == vd_type1_voice_dibit(voice_seed, b, i);
            }
            assert(!same);
        }
    }

    assert(g_process_mbe_call_count == 5);
    for (size_t call = 0; call < 5U; call++) {
        assert(g_process_mbe_ambe_nonnull[call] == 1);
        for (size_t i = 0; i < DSD_AMBE_2450_DIBITS; i++) {
            const dsd_ambe_2450_dibit_map_entry* map = &dsd_ambe_2450_dibit_map[i];
            const uint8_t high = (uint8_t)g_process_mbe_ambe[call][map->high_row][map->high_col];
            const uint8_t low = (uint8_t)g_process_mbe_ambe[call][map->low_row][map->low_col];
            assert(high <= 1U && low <= 1U);
            assert((uint8_t)((high << 1U) | low) == vd_type1_voice_dibit(voice_seed, call, i));
        }
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
    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.kind == DSD_CALL_KIND_DATA);
    assert(strcmp(call.target_text, "PYSFDST001") == 0);
    assert(strcmp(call.source_text, "PYSFSRC002") == 0);
    assert(strcmp(call.route_text[0], "PYSFUPL003") == 0);
    assert(strcmp(call.route_text[1], "PYSFDNL004") == 0);
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
    DSD_MEMSET(g_process_mbe_ambe, 0, sizeof(g_process_mbe_ambe));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_process_mbe_call_count = 0;

    make_fich_bits_for_vd_type1(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);

    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);

    processYSF(&opts, &state);

    /* Issue #660: all five VCHs reach the vocoder, the fifth included, each with its own bits. */
    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_process_mbe_call_count == 5);
    for (size_t i = 0; i < 5U; i++) {
        assert(g_process_mbe_synctype[i] == DSD_SYNC_NXDN_POS);
        // The NXDN decoder is borrowed; the frame stays YSF's, so no NXDN cipher touches it (issue #656).
        assert(g_process_mbe_protocol[i] == DSD_VOICE_PROTOCOL_YSF);
        assert(g_process_mbe_ambe_nonnull[i] == 1);
    }
    assert_vd_type1_calls_carry_each_vch(1U);
    assert(state.synctype == 0);
    assert(state.ysf_fi == 1U);
    assert(state.ysf_dt == 0U);
    assert(state.ysf_cm == 0U);
    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(strcmp(call.target_text, "VD1DST0001") == 0);
    assert(strcmp(call.source_text, "VD1SRC0002") == 0);
}

/* Issue #575: the carrier boundary moved the carrier count while the V/D type 1 payload was read (a replay read adopting
   a recorded retune), in its last voice channel. The data channel and the first four voice channels were read whole
   before the move but decode only after the payload, so they would publish the carrier left's callsigns and play its
   voice after the boundary ended its call: nothing of the payload decodes, as on a sync loss. */
static void
test_process_ysf_vd_type1_split_by_a_carrier_boundary_publishes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_process_mbe_call_count = 0;

    make_fich_bits_for_vd_type1(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    g_boundary_at = 100U + (4U * 72U) + 36U + 10U;

    const int verdict = processYSF(&opts, &state);
    g_boundary_at = SIZE_MAX;

    assert(state.carrier_seq == 1U);
    /* The FICH that checked out was the carrier left's: the frame confirms nothing, and reports nothing decoded, so the
       boundary's restart of the verdict stands and the next carrier's bad-FICH frames are not taken as productive. */
    assert(verdict == 0);
    assert(state.ysf_fich_confirmed == 0);
    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_process_mbe_call_count == 0);
    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(call.target_text[0] == '\0');
    assert(call.source_text[0] == '\0');
}

static void
test_process_ysf_bad_fich_fallback_reopens_ysf_epoch(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    state.synctype = DSD_SYNC_YSF_NEG;

    make_fich_bits_for_vd_type1(fich_bits);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);

    encode_fich_input(fich_bits, 0, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    processYSF(&opts, &state);

    dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.protocol == DSD_SYNC_YSF_NEG);
    const uint64_t first_epoch = call.epoch;
    assert(dsd_call_state_end(&state, 0U, 0.0) == 1);

    encode_fich_input(fich_bits, 1, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    processYSF(&opts, &state);

    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_process_mbe_call_count == 5);
    for (size_t i = 0U; i < 5U; i++) {
        assert(g_process_mbe_synctype[i] == DSD_SYNC_NXDN_POS);
        // The NXDN decoder is borrowed; the frame stays YSF's, so no NXDN cipher touches it (issue #656).
        assert(g_process_mbe_protocol[i] == DSD_VOICE_PROTOCOL_YSF);
    }
    call = get_test_ysf_call(&state);
    assert(call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.epoch != first_epoch);
    assert(call.protocol == DSD_SYNC_YSF_NEG);
    assert(call.kind == DSD_CALL_KIND_VOICE);
    dsd_state_ext_free_all(&state);
}

/* #391: processYSF()'s verdict is sticky for the transmission, not per frame. A FICH failure
 * before any FICH has held means the ~460 dibits behind it were laid out by nothing read off
 * the air, so the SPS hunt must refuse them their dwell. After one has held, the same failure
 * falls back to a confirmed frame's dt/fi and still synthesizes and plays voice -- a frame
 * that produced audio must not report having validated nothing. */
static void
test_process_ysf_fich_verdict_is_sticky_per_transmission(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    state.synctype = DSD_SYNC_YSF_NEG;

    make_fich_bits_for_vd_type1(fich_bits);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);

    /* Nothing has confirmed yet, so a failed FICH validated nothing. */
    encode_fich_input(fich_bits, 1, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    assert(processYSF(&opts, &state) == 0);
    assert(state.ysf_fich_confirmed == 0U);

    /* A FICH that holds confirms the transmission. */
    encode_fich_input(fich_bits, 0, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    assert(processYSF(&opts, &state) == 1);
    assert(state.ysf_fich_confirmed == 1U);

    /* The same failure as the first frame, but the fallback dt/fi now come from a frame that
     * did check out, and the payload still reaches the vocoder. */
    encode_fich_input(fich_bits, 1, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    assert(processYSF(&opts, &state) == 1);
    assert(g_process_mbe_call_count == 5);

    /* The flag alone gates the verdict: cleared, the very frame that just reported productive
     * reports unproductive again, so a new transmission cannot inherit the last one's
     * confidence. This binary links only dsd-neo_proto_ysf, so noCarrier() -- which is what
     * clears the flag in the running decoder -- is not reachable here; ENGINE_NO_CARRIER_RESET
     * pins that reset against the real function. */
    state.ysf_fich_confirmed = 0U;
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    assert(processYSF(&opts, &state) == 0);

    dsd_state_ext_free_all(&state);
}

/* The output buffers the gain stage (processAudio()) writes, for a zeroed state: V/D2 frames run it in either output
   format (issue #652). */
static void
prepare_gain_stage(dsd_state* state) {
    static short out_buf[100 + (160 * 96)];
    state->audio_out_buf = out_buf;
    state->audio_out_buf_p = out_buf + 100;
    state->aout_max_buf_p = state->aout_max_buf;
    state->aout_max_buf_idx = 0;
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
    /* A fixed gain of 3, so the staged frame shows the gain stage ran exactly once (issue #652: float output skipped it;
       twice would stage 9x). */
    opts.audio_gain = 3.0F;
    state.aout_gain = 3.0F;
    prepare_gain_stage(&state);

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
    // The fifth stubbed frame (5.0), scaled to the int16 scale of the voice buffers and gained once (x3) into the short
    // frame the mixers play, in float output as in 16-bit output.
    const short once = (short)(3.0F * 5.0F * DSD_MBE_PCM16_PER_FLOAT);
    assert(state.s_l[0] == once);
    assert(state.s_l[159] == once);
    assert(fabsf(state.audio_out_temp_buf[0] - (3.0F * 5.0F * DSD_MBE_PCM16_PER_FLOAT)) <= 1e-6F);
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

/* The vocoder writes each voice frame's per-call WAV block (-P) behind its record
 * gate; YSF must not write a second copy of the same frame. The vocoder is stubbed
 * here, so nothing may reach the WAV. */
static void
test_process_ysf_ehr_voice_leaves_call_wav_to_vocoder(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_process_mbe_call_count = 0;

    char wav_path[] = "dsdneo_ysf_wav_XXXXXX";
    const int fd = dsd_mkstemp(wav_path);
    assert(fd >= 0);
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    opts.wav_out_f = sf_open(wav_path, SFM_WRITE, &info);
    assert(opts.wav_out_f != NULL);
    opts.dmr_stereo_wav = 1;

    make_fich_bits_for_vd_type1(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);

    processYSF(&opts, &state);
    assert(g_process_mbe_call_count == 5);

    sf_close(opts.wav_out_f);
    opts.wav_out_f = NULL;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* written = sf_open(wav_path, SFM_READ, &info);
    assert(written != NULL);
    sf_close(written);
    assert(info.frames == 0);
    (void)remove(wav_path);
    dsd_state_ext_free_all(&state);
}

/* V/D2 decodes straight through mbelib and never reaches the vocoder's per-call
 * WAV write, so YSF writes those frames itself, under the call's record policy:
 * an allowed call records, and an allow-list (-W) no YSF call can match does not. */
static void
test_process_ysf_vd_type2_writes_call_wav_under_record_policy(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch2[100];

    for (int blocked = 1; blocked >= 0; --blocked) {
        InitAllFecFunction();
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
        DSD_MEMSET(g_mbe_inbound_errs2, 0, sizeof(g_mbe_inbound_errs2));
        g_dibit_stream_len = 0;
        g_dibit_stream_pos = 0;
        g_mbe_call_count = 0;
        opts.floating_point = 1;
        prepare_gain_stage(&state);
        opts.dmr_stereo_wav = 1;
        opts.trunk_use_allow_list = blocked;

        char wav_path[] = "dsdneo_ysf_vd2_wav_XXXXXX";
        const int fd = dsd_mkstemp(wav_path);
        assert(fd >= 0);
        (void)dsd_close(fd);
        SF_INFO info;
        DSD_MEMSET(&info, 0, sizeof(info));
        info.samplerate = 8000;
        info.channels = 1;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        opts.wav_out_f = sf_open(wav_path, SFM_WRITE, &info);
        assert(opts.wav_out_f != NULL);

        make_fich_bits_for_vd_type2(fich_bits);
        encode_fich_input(fich_bits, 0, 0, fich_input);
        encode_dch_payload_to_input("VD2SRC4444", 10U, dch2, 96U, 5U, 0);
        append_dibits_to_stream(fich_input, 100U);
        for (size_t block = 0; block < 5U; block++) {
            append_dibits_to_stream(&dch2[block * 20U], 20U);
            append_type2_vech_dibits((block % 2U) == 0U ? 1U : 0U);
        }

        processYSF(&opts, &state);
        assert(g_mbe_call_count == 5);

        sf_close(opts.wav_out_f);
        opts.wav_out_f = NULL;
        DSD_MEMSET(&info, 0, sizeof(info));
        SNDFILE* written = sf_open(wav_path, SFM_READ, &info);
        assert(written != NULL);
        sf_close(written);
        assert(blocked ? info.frames == 0 : info.frames == (sf_count_t)5 * 160);
        (void)remove(wav_path);
        dsd_state_ext_free_all(&state);
    }
}

/* Issue #661: MBE capture of YSF frames. */
static char g_capture_dir[DSD_TEST_PATH_MAX];

typedef struct {
    int count;
    char name[256];
} capture_dir_ctx;

static int
capture_dir_entry(const char* name, void* user) {
    capture_dir_ctx* ctx = (capture_dir_ctx*)user;
    ctx->count++;
    DSD_SNPRINTF(ctx->name, sizeof ctx->name, "%s", name);
    return 0;
}

static int
remove_capture_dir_entry(const char* name, void* user) {
    char path[DSD_TEST_PATH_MAX * 2];
    DSD_SNPRINTF(path, sizeof path, "%s%c%s", (const char*)user, dsd_test_path_sep(), name);
    (void)remove(path);
    return 0;
}

static void
capture_dir_begin(dsd_opts* opts, const char* prefix) {
    assert(dsd_test_mkdtemp(g_capture_dir, sizeof g_capture_dir, prefix) != NULL);
    DSD_SNPRINTF(opts->mbe_out_dir, sizeof opts->mbe_out_dir, "%s", g_capture_dir);
}

static void
capture_dir_end(void) {
    (void)dsd_dir_list(g_capture_dir, remove_capture_dir_entry, (void*)g_capture_dir);
    (void)dsd_test_rmdir(g_capture_dir);
}

static int
capture_dir_count(char* name, size_t name_size) {
    capture_dir_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof ctx);
    (void)dsd_dir_list(g_capture_dir, capture_dir_entry, &ctx);
    if (name) {
        DSD_SNPRINTF(name, name_size, "%s", ctx.name);
    }
    return ctx.count;
}

static size_t
read_capture_file(const char* name, unsigned char* out, size_t out_size) {
    char path[DSD_TEST_PATH_MAX * 2];
    DSD_SNPRINTF(path, sizeof path, "%s%c%s", g_capture_dir, dsd_test_path_sep(), name);
    FILE* f = fopen(path, "rb");
    assert(f != NULL);
    size_t n = fread(out, 1, out_size, f);
    fclose(f);
    return n;
}

static void
append_vd_type2_frame(int corrupt_fich_crc) {
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch2[100];
    make_fich_bits_for_vd_type2(fich_bits);
    encode_fich_input(fich_bits, corrupt_fich_crc, 0, fich_input);
    encode_dch_payload_to_input("VD2SRC4444", 10U, dch2, 96U, 5U, 0);
    append_dibits_to_stream(fich_input, 100U);
    for (size_t block = 0; block < 5U; block++) {
        append_dibits_to_stream(&dch2[block * 20U], 20U);
        append_type2_vech_dibits((block % 2U) == 0U ? 1U : 0U);
    }
}

static void
reset_ysf_stream(void) {
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_mbe_call_count = 0;
    g_process_mbe_call_count = 0;
    g_mbe_fail_at = -1;
}

#ifndef DSD_YSF_TEST_REAL_VOCODER
/* V/D2 decodes straight through mbelib: each block is captured after the vocoder settled its error count (0 for a
   block mbelib refused), in the record format -r reads back. */
static void
test_process_ysf_vd_type2_captures_each_block(void) {
    static dsd_opts opts;
    static dsd_state state;
    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_ysf_stream();
    opts.floating_point = 1;
    prepare_gain_stage(&state);
    state.synctype = DSD_SYNC_YSF_POS;
    capture_dir_begin(&opts, "dsdneo_ysf_vd2_capture");
    g_mbe_fail_at = 3;

    append_vd_type2_frame(0);
    processYSF(&opts, &state);
    assert(g_mbe_call_count == 5);
    closeMbeOutFile(&opts, &state);

    char name[256];
    assert(capture_dir_count(name, sizeof name) == 1);
    unsigned char bytes[64];
    const size_t n = read_capture_file(name, bytes, sizeof bytes);
    assert(n == 4U + (5U * 8U));
    assert(memcmp(bytes, ".amb", 4) == 0);
    for (int block = 0; block < 5; block++) {
        const unsigned char* rec = &bytes[4 + (block * 8)];
        assert(rec[0] == (block == 3 ? 0U : (unsigned char)(4 + block)));
        unsigned char want[7];
        DSD_MEMSET(want, 0, sizeof want);
        for (int i = 0; i < 48; i++) {
            want[i / 8] = (unsigned char)((want[i / 8] << 1) | (unsigned char)(g_mbe_ambe[block][i] & 1));
        }
        want[6] = (unsigned char)g_mbe_ambe[block][48];
        assert(memcmp(&rec[1], want, sizeof want) == 0);
    }
    capture_dir_end();
    dsd_state_ext_free_all(&state);
}

/* A FICH that failed lays the payload out from the last one that passed, possibly an earlier transmission's: nothing
   is captured until a FICH verifies this transmission, its own frame included, and then the fallback frames are. */
static void
test_process_ysf_capture_waits_for_a_verified_fich(void) {
    static dsd_opts opts;
    static dsd_state state;
    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    prepare_gain_stage(&state);
    state.synctype = DSD_SYNC_YSF_POS;

    /* An earlier transmission leaves V/D2 as the fallback layout; its carrier then dropped. */
    reset_ysf_stream();
    append_vd_type2_frame(0);
    processYSF(&opts, &state);
    state.ysf_fich_confirmed = 0;

    capture_dir_begin(&opts, "dsdneo_ysf_fich_capture");
    reset_ysf_stream();
    append_vd_type2_frame(1);
    processYSF(&opts, &state);
    assert(g_mbe_call_count == 5);
    assert(capture_dir_count(NULL, 0) == 0);

    reset_ysf_stream();
    append_vd_type2_frame(0);
    processYSF(&opts, &state);
    reset_ysf_stream();
    append_vd_type2_frame(1);
    processYSF(&opts, &state);
    closeMbeOutFile(&opts, &state);

    char name[256];
    assert(capture_dir_count(name, sizeof name) == 1);
    unsigned char bytes[128];
    assert(read_capture_file(name, bytes, sizeof bytes) == 4U + (10U * 8U));
    capture_dir_end();
    dsd_state_ext_free_all(&state);
}

/* A verified header or terminator bounds the transmission: the capture ends there. */
static void
test_process_ysf_header_and_terminator_end_the_capture(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch0[180];
    uint8_t dch1[180];
    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    prepare_gain_stage(&state);
    state.synctype = DSD_SYNC_YSF_POS;
    capture_dir_begin(&opts, "dsdneo_ysf_end_capture");
    encode_dch_payload_to_input("TERMDST001TERMSRC002", 20U, dch0, 176U, 9U, 0);
    encode_dch_payload_to_input("TERMUPL003TERMDNL004", 20U, dch1, 176U, 9U, 0);

    for (int header = 1; header >= 0; --header) {
        reset_ysf_stream();
        append_vd_type2_frame(0);
        processYSF(&opts, &state);
        assert(opts.mbe_out_f != NULL);

        reset_ysf_stream();
        if (header) {
            make_fich_bits_for_full_rate_data(fich_bits);
        } else {
            make_fich_bits_for_terminator(fich_bits);
        }
        encode_fich_input(fich_bits, 0, 0, fich_input);
        append_dibits_to_stream(fich_input, 100U);
        append_interleaved_full_rate_dch_blocks(dch0, dch1);
        processYSF(&opts, &state);
        assert(opts.mbe_out_f == NULL);
    }
    assert(capture_dir_count(NULL, 0) == 2);
    capture_dir_end();
    dsd_state_ext_free_all(&state);
}

/* Issue #661: a voice block read across the carrier boundary is dropped with the rest of the frame, as V/D1 drops it,
   in V/D2 and in full rate alike. */
static void
test_process_ysf_voice_split_by_a_carrier_boundary_drops_the_rest(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    InitAllFecFunction();

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    prepare_gain_stage(&state);
    reset_ysf_stream();
    append_vd_type2_frame(0);
    /* Inside the third block's VeCH. */
    g_boundary_at = 100U + (2U * 72U) + 20U + 10U;
    (void)processYSF(&opts, &state);
    g_boundary_at = SIZE_MAX;
    assert(state.carrier_seq == 1U);
    assert(g_mbe_call_count == 2);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.floating_point = 1;
    prepare_gain_stage(&state);
    reset_ysf_stream();
    make_fich_bits_for_full_rate_voice(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    append_dibits_to_stream(fich_input, 100U);
    uint8_t zeros[72];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    for (int slot = 0; slot < 5; slot++) {
        append_dibits_to_stream(zeros, sizeof zeros);
    }
    /* Inside the third voice slot. */
    g_boundary_at = 100U + (2U * 72U) + 30U;
    (void)processYSF(&opts, &state);
    g_boundary_at = SIZE_MAX;
    assert(state.carrier_seq == 1U);
    assert(g_process_mbe_call_count == 2);
    dsd_state_ext_free_all(&state);
}
#else
/* Built with the real vocoder (YSF_MBE_CAPTURE): the frames YSF hands the borrowed NXDN and P25 Phase 1 decoders are
   captured, V/D1 in AMBE records and full rate in IMBE records. Driven through processYSF(), which knows no dispatch,
   as on the revision before issue #661, where nothing of either was captured. */
static void
append_vd_type1_frame(void) {
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];
    make_fich_bits_for_vd_type1(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
}

static void
append_full_rate_voice_frame(void) {
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    make_fich_bits_for_full_rate_voice(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    append_dibits_to_stream(fich_input, 100U);
    uint8_t zeros[72];
    DSD_MEMSET(zeros, 0, sizeof zeros);
    for (int slot = 0; slot < 5; slot++) {
        append_dibits_to_stream(zeros, sizeof zeros);
    }
}

/* Zeroed options and state with the vocoder parameters the real processMbeFrame() synthesizes into, float audio. */
static void
begin_real_vocoder_test(dsd_opts* opts, dsd_state* state) {
    static mbe_parms cur, prev, prev_enhanced, cur2, prev2, prev_enhanced2;
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    mbe_initMbeParms(&cur, &prev, &prev_enhanced);
    mbe_initMbeParms(&cur2, &prev2, &prev_enhanced2);
    state->cur_mp = &cur;
    state->prev_mp = &prev;
    state->prev_mp_enhanced = &prev_enhanced;
    state->cur_mp2 = &cur2;
    state->prev_mp2 = &prev2;
    state->prev_mp_enhanced2 = &prev_enhanced2;
    opts->floating_point = 1;
    prepare_gain_stage(state);
}

static void
test_process_ysf_borrowed_decoder_frames_are_captured(void) {
    static dsd_opts opts;
    static dsd_state state;
    InitAllFecFunction();

    for (int full_rate = 0; full_rate <= 1; full_rate++) {
        begin_real_vocoder_test(&opts, &state);
        state.synctype = DSD_SYNC_YSF_POS;
        capture_dir_begin(&opts, "dsdneo_ysf_borrowed_capture");
        reset_ysf_stream();
        if (full_rate) {
            append_full_rate_voice_frame();
        } else {
            append_vd_type1_frame();
        }
        processYSF(&opts, &state);
        closeMbeOutFile(&opts, &state);

        char name[256];
        assert(capture_dir_count(name, sizeof name) == 1);
        unsigned char bytes[128];
        const size_t n = read_capture_file(name, bytes, sizeof bytes);
        const size_t record = full_rate ? 12U : 8U;
        assert(memcmp(bytes, full_rate ? ".imb" : ".amb", 4) == 0);
        assert(n > 4U && ((n - 4U) % record) == 0U);
        /* Five frames a YSF frame in both: V/D1 lost its fifth VCH before issue #660. */
        assert((n - 4U) / record == 5U);
        if (!full_rate) {
            assert(g_mbe_call_count == 5);
        }
        capture_dir_end();
        dsd_state_ext_free_all(&state);
    }
}

/* Under processFrame()'s dispatch context the borrowed frames belong to YSF: one capture owner, rotated on the format
   change from V/D1 to full rate, and nothing before a FICH has verified the transmission. */
static void
test_process_ysf_borrowed_frames_belong_to_ysf(void) {
    static dsd_opts opts;
    static dsd_state state;
    InitAllFecFunction();
    begin_real_vocoder_test(&opts, &state);
    capture_dir_begin(&opts, "dsdneo_ysf_owner_capture");
    state.synctype = DSD_SYNC_YSF_POS;
    state.frame_dispatch_active = 1;
    state.frame_dispatch_synctype = DSD_SYNC_YSF_POS;
    state.frame_dispatch_carrier_seq = state.carrier_seq;

    reset_ysf_stream();
    append_vd_type1_frame();
    processYSF(&opts, &state);
    assert(opts.mbe_out_f != NULL);
    assert(opts.mbe_out_owner == dsd_mbe_out_owner_of(DSD_SYNC_YSF_NEG));
    assert(opts.mbe_out_format == DSD_MBE_OUT_FORMAT_AMBE);
    /* The verified FICH admits the frame's every VCH to the YSF capture (issue #660). */
    (void)fflush(opts.mbe_out_f);
    char vd1_name[256];
    assert(capture_dir_count(vd1_name, sizeof vd1_name) == 1);
    unsigned char vd1_bytes[128];
    assert(read_capture_file(vd1_name, vd1_bytes, sizeof vd1_bytes) == 4U + (5U * 8U));

    reset_ysf_stream();
    append_full_rate_voice_frame();
    processYSF(&opts, &state);
    assert(opts.mbe_out_owner == dsd_mbe_out_owner_of(DSD_SYNC_YSF_NEG));
    assert(opts.mbe_out_format == DSD_MBE_OUT_FORMAT_IMBE);
    closeMbeOutFile(&opts, &state);
    assert(capture_dir_count(NULL, 0) == 2);
    capture_dir_end();
    dsd_state_ext_free_all(&state);
}

/* Issue #660: a V/D1 frame is 100 ms of voice, so its five VCHs write five 160-sample blocks to the per-call WAV (-P)
   through the vocoder; before, the fifth was never synthesized and each frame recorded 80 ms. mbelib's synthesis is
   still the stub here, so this pins the frame count reaching the recording, not the speech. */
static void
test_process_ysf_vd_type1_records_five_frames_to_call_wav(void) {
    static dsd_opts opts;
    static dsd_state state;
    InitAllFecFunction();
    begin_real_vocoder_test(&opts, &state);
    opts.dmr_stereo_wav = 1;
    state.synctype = DSD_SYNC_YSF_POS;

    char wav_path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(wav_path, sizeof wav_path, "dsdneo_ysf_vd1_wav");
    assert(fd >= 0);
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    opts.wav_out_f = sf_open(wav_path, SFM_WRITE, &info);
    assert(opts.wav_out_f != NULL);

    reset_ysf_stream();
    append_vd_type1_frame();
    processYSF(&opts, &state);
    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_mbe_call_count == 5);

    sf_close(opts.wav_out_f);
    opts.wav_out_f = NULL;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* written = sf_open(wav_path, SFM_READ, &info);
    assert(written != NULL);
    sf_close(written);
    assert(info.frames == (sf_count_t)5 * 160);
    (void)remove(wav_path);
    dsd_state_ext_free_all(&state);
}
#endif

int
main(void) {
#ifdef DSD_YSF_TEST_REAL_VOCODER
    test_process_ysf_borrowed_decoder_frames_are_captured();
    test_process_ysf_borrowed_frames_belong_to_ysf();
    test_process_ysf_vd_type1_records_five_frames_to_call_wav();
    return 0;
#else
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
    test_process_ysf_vd_type1_split_by_a_carrier_boundary_publishes_nothing();
    test_process_ysf_bad_fich_fallback_reopens_ysf_epoch();
    test_process_ysf_fich_verdict_is_sticky_per_transmission();
    test_process_ysf_vd_type2_routes_dch2_voice_and_audio_errors();
    test_process_ysf_terminator_enriches_identity_before_call_end();
    test_process_ysf_ehr_voice_leaves_call_wav_to_vocoder();
    test_process_ysf_vd_type2_writes_call_wav_under_record_policy();
    test_process_ysf_vd_type2_captures_each_block();
    test_process_ysf_capture_waits_for_a_verified_fich();
    test_process_ysf_header_and_terminator_end_the_capture();
    test_process_ysf_voice_split_by_a_carrier_boundary_drops_the_rest();
    return 0;
#endif
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
