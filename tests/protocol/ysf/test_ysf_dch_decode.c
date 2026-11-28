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
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/ysf/ysf.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <math.h>
#include <mbelib-neo/mbelib.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio/voice_chain_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#ifdef DSD_YSF_TEST_REAL_VOCODER
/* The real-vocoder build (YSF_MBE_CAPTURE) runs only its capture cases; the stubbed-vocoder cases stay compiled. */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#ifndef DSD_YSF_TEST_REAL_VOCODER
#include "dsp/symbol_anchor_internal.h" /* the stubbed-vocoder cases' arm checks */
#endif
#include "scan_activity_capture.h"
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
/* The read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there and
   stands erasures in for every symbol after it; SIZE_MAX for none. The stub goes on handing out the frame's real
   dibits, the worst case of erasures that pass the FICH's Golay and CRC. */
static size_t g_interrupt_at = SIZE_MAX;
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
/* Synthesis flags each stubbed call reports. */
static unsigned g_mbe_flags[5];

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
    if (g_dibit_stream_pos == g_interrupt_at && state != NULL) {
        state->input_interrupted = 1;
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
    result->flags |= g_mbe_flags[g_mbe_call_count];
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

/* CSD2 is the downlink repeater's callsign, then the uplink's (Yaesu "Amateur Radio Digital Standards" Table 4-2;
   MMDVMHost and DSDcc read it so). route_text[0] is the uplink, route_text[1] the downlink. Before, the first ten bytes
   were taken for the uplink, so the two read swapped. */
static void
test_dch_csd2_tracks_uplink_and_downlink(void) {
    static dsd_state state;
    uint8_t bits[160];
    const char payload[] = "DOWNLINK02UPLINK0001";

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

    /* V/D type 2 sends the downlink in frame 2 and the uplink in frame 3 (Table 4-4). */
    pack_bytes_to_bits("DN2CALL901", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 2, 5, 0, bits);
    call = get_test_ysf_call(&state);
    assert(strcmp(call.route_text[1], "DN2CALL901") == 0);

    pack_bytes_to_bits("UP2CALL678", 10U, bits);
    ysf_dch_decode2(&state, 0, 0, 3, 5, 0, bits);
    call = get_test_ysf_call(&state);
    assert(strcmp(call.route_text[0], "UP2CALL678") == 0);

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

/* What the signal search's confirmation can read when the evidence hook fires: the app-control observer copies the
 * active call's callsigns inside the hook, from state, and only on that confirmation. The last report a frame makes is
 * the freshest view the search keeps. */
static struct {
    int calls;
    int call_active;
    char source_text[DSD_CALL_IDENTITY_TEXT_SIZE];
    char target_text[DSD_CALL_IDENTITY_TEXT_SIZE];
} g_search_evidence;

static void
capture_search_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                        uint32_t source, const char* access) {
    (void)sync;
    (void)slot;
    (void)target;
    (void)source;
    (void)access;
    assert(protocol != NULL && strcmp(protocol, "YSF") == 0);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    g_search_evidence.calls++;
    const int active = dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                       && call.protocol == state->synctype;
    g_search_evidence.call_active = active;
    DSD_SNPRINTF(g_search_evidence.source_text, sizeof(g_search_evidence.source_text), "%s",
                 active ? call.source_text : "");
    DSD_SNPRINTF(g_search_evidence.target_text, sizeof(g_search_evidence.target_text), "%s",
                 active ? call.target_text : "");
}

/* Call notes a signal search hears, and the source the slot-0 call record named when each arrived. */
static int g_call_notes;
static char g_call_note_source[64];

static void
capture_call_note(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    g_call_notes++;
    g_call_note_source[0] = '\0';
    if (slot == 0U && dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE) {
        DSD_SNPRINTF(g_call_note_source, sizeof(g_call_note_source), "%s", call.source_text);
    }
}

/* A V/D2 DCH whose CRC passes reports to the search after its callsign is on the call, so the last DCH of a
 * transmission still names the station; a damaged one reports nothing. */
static void
test_ysf_conv_dch2_search_evidence_follows_the_callsign(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t input[100];

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_search_evidence, 0, sizeof(g_search_evidence));
    state.synctype = DSD_SYNC_YSF_POS;
    begin_test_ysf_call(&state);
    dsd_signal_search_evidence_set(capture_search_evidence);
    dsd_signal_search_call_set(capture_call_note);
    g_call_notes = 0;

    encode_dch_payload_to_input("DCH2BAD789", 10U, input, 96U, 5U, 1);
    assert(ysf_conv_dch2(&opts, &state, 0, 0, 1, 5, 0, input) == -2);
    assert(g_search_evidence.calls == 0 && g_call_notes == 0);

    encode_dch_payload_to_input("DCH2SRC789", 10U, input, 96U, 5U, 0);
    assert(ysf_conv_dch2(&opts, &state, 0, 0, 1, 5, 0, input) == 0);
    dsd_signal_search_evidence_set(NULL);
    dsd_signal_search_call_set(NULL);

    assert(g_search_evidence.calls == 1);
    assert(g_call_notes == 1 && strcmp(g_call_note_source, "DCH2SRC789") == 0);
    assert(g_search_evidence.call_active == 1);
    assert(strcmp(g_search_evidence.source_text, "DCH2SRC789") == 0);
    dsd_state_ext_free_all(&state);
}

/* A header frame's FICH reports before the frame opens its call, and the callsigns arrive in its CRC-checked DCH: each
 * DCH reports to the search once its callsigns are on the call, so a header followed by nothing decodable still
 * names the stations. */
static void
test_process_ysf_header_search_evidence_follows_the_callsigns(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch0[180];
    uint8_t dch1[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&g_search_evidence, 0, sizeof(g_search_evidence));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    state.synctype = DSD_SYNC_YSF_POS;

    make_fich_bits_for_full_rate_data(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("SRCHDST001SRCHSRC002", 20U, dch0, 176U, 9U, 0);
    encode_dch_payload_to_input("SRCHDNL004SRCHUPL003", 20U, dch1, 176U, 9U, 0);
    append_dibits_to_stream(fich_input, 100U);
    append_interleaved_full_rate_dch_blocks(dch0, dch1);

    dsd_signal_search_evidence_set(capture_search_evidence);
    processYSF(&opts, &state);
    dsd_signal_search_evidence_set(NULL);

    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_search_evidence.calls >= 1);
    assert(g_search_evidence.call_active == 1);
    assert(strcmp(g_search_evidence.target_text, "SRCHDST001") == 0);
    assert(strcmp(g_search_evidence.source_text, "SRCHSRC002") == 0);
    dsd_state_ext_free_all(&state);
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
    encode_dch_payload_to_input("PYSFDNL004PYSFUPL003", 20U, dch1, 176U, 9U, 0);

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

static int g_ysf_search_evidence;

static void
count_ysf_search_evidence(const dsd_state* state, const char* protocol, int sync, int slot, uint32_t target,
                          uint32_t source, const char* access) {
    (void)sync;
    (void)slot;
    (void)state;
    (void)target;
    (void)source;
    (void)access;
    assert(strcmp(protocol, "YSF") == 0);
    g_ysf_search_evidence++;
}

/* A Signal search that leaves the frame (its visit ended, or a command came), or a live read that gives up on a stalled
   input, cuts it: from the cut every symbol is an erasure getSymbol() stood in. A cut FICH decodes nothing, so it
   reports no YSF evidence to the search and observes no call on the fallback dt/fi, and nothing of the payload plays.
   A cut later in the frame leaves the FICH, read whole before it, as it was, and drops the payload. */
static void
test_process_ysf_frame_cut_by_an_input_interruption_publishes_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_YSF_POS;
    g_ysf_search_evidence = 0;
    dsd_signal_search_evidence_set(count_ysf_search_evidence);
    make_fich_bits_for_vd_type1(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch, 176U, 9U, 0);

    /* A whole frame confirms the transmission and plays; its call then ends. */
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    assert(processYSF(&opts, &state) == 1);
    assert(g_process_mbe_call_count == 5);
    const int whole_frame_evidence = g_ysf_search_evidence;
    assert(whole_frame_evidence > 0);
    assert(dsd_call_state_end(&state, 0U, 0.0) == 1);

    static const size_t cuts[2] = {50U, 100U + (4U * 72U) + 36U + 10U}; /* inside the FICH; in the last voice channel */
    static const int fich_evidence[2] = {0, 1};
    for (size_t c = 0; c < 2U; c++) {
        state.input_interrupted = 0;
        g_dibit_stream_len = 0U;
        g_dibit_stream_pos = 0U;
        g_process_mbe_call_count = 0;
        g_ysf_search_evidence = 0;
        append_dibits_to_stream(fich_input, 100U);
        append_vd_type1_blocks(dch, 1U);
        g_interrupt_at = cuts[c];

        (void)processYSF(&opts, &state);
        g_interrupt_at = SIZE_MAX;

        assert(state.input_interrupted == 1);
        assert(state.carrier_seq == 0U);
        assert(g_process_mbe_call_count == 0);
        assert(g_ysf_search_evidence == fich_evidence[c]);
        if (c == 0U) {
            const dsd_call_snapshot call = get_test_ysf_call(&state);
            assert(call.phase != DSD_CALL_PHASE_ACTIVE);
        }
    }
    /* A carrier boundary inside the FICH (issue #575) drops the frame the same way, its evidence too. */
    state.input_interrupted = 0;
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    g_ysf_search_evidence = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
    g_boundary_at = 50U;
    (void)processYSF(&opts, &state);
    g_boundary_at = SIZE_MAX;
    assert(state.carrier_seq == 1U);
    assert(g_process_mbe_call_count == 0);
    assert(g_ysf_search_evidence == 0);
    dsd_signal_search_evidence_set(NULL);
    dsd_state_ext_free_all(&state);
}

/* Issue #834: the PROVEN content notes a scan quiet check hears through the content-evidence hook. */
static int g_quiet_proven;
static int g_quiet_other;

static void
record_quiet_note(dsd_state* state, int level, uint32_t hold_ms) {
    (void)state;
    (void)hold_ms;
    if (level == DSD_CONTENT_EVIDENCE_PROVEN) {
        g_quiet_proven++;
    } else {
        g_quiet_other++;
    }
}

/* A header frame through processYSF(), skip-silent @p on, cut by an input interruption at dibit @p cut (SIZE_MAX for
   none): the PROVEN notes it made. */
static int
quiet_ysf_header(int on, size_t cut) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch0[180];
    uint8_t dch1[180];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    opts.scan_skip_silent = on;
    state.synctype = DSD_SYNC_YSF_POS;
    make_fich_bits_for_full_rate_data(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("QYSFDST001QYSFSRC002", 20U, dch0, 176U, 9U, 0);
    encode_dch_payload_to_input("QYSFDNL004QYSFUPL003", 20U, dch1, 176U, 9U, 0);
    append_dibits_to_stream(fich_input, 100U);
    append_interleaved_full_rate_dch_blocks(dch0, dch1);
    g_quiet_proven = 0;
    g_quiet_other = 0;
    g_interrupt_at = cut;
    (void)processYSF(&opts, &state);
    g_interrupt_at = SIZE_MAX;
    dsd_state_ext_free_all(&state);
    assert(g_quiet_other == 0);
    return g_quiet_proven;
}

/* A FICH that passed its Golay and CRC-16, and each DCH whose CRC passed, is PROVEN content to a scan quiet check; a
   DCH whose CRC failed and a FICH an input interruption cut are none, and nothing is noted with the setting off. */
static void
test_ysf_checked_fich_and_dch_are_proven_content(void) {
    InitAllFecFunction();
    dsd_frame_sync_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.content_evidence = record_quiet_note;
    dsd_frame_sync_hooks_set(&hooks);
    assert(quiet_ysf_header(1, SIZE_MAX) >= 2); /* the FICH and the DCH */
    assert(quiet_ysf_header(0, SIZE_MAX) == 0);
    assert(quiet_ysf_header(1, 50U) == 0);

    static dsd_opts opts;
    static dsd_state state;
    uint8_t input[100];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.scan_skip_silent = 1;
    state.synctype = DSD_SYNC_YSF_POS;
    begin_test_ysf_call(&state);
    g_quiet_proven = 0;
    encode_dch_payload_to_input("DCH2BAD789", 10U, input, 96U, 5U, 1);
    assert(ysf_conv_dch2(&opts, &state, 0, 0, 1, 5, 0, input) == -2);
    assert(g_quiet_proven == 0);
    encode_dch_payload_to_input("DCH2SRC789", 10U, input, 96U, 5U, 0);
    assert(ysf_conv_dch2(&opts, &state, 0, 0, 1, 5, 0, input) == 0);
    assert(g_quiet_proven == 1);
    dsd_frame_sync_hooks_set(NULL);
    dsd_state_ext_free_all(&state);
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

/* The FICH-failure fallback is each carrier's own (issue #890): two decoder states interleaved, A confirmed on V/D1
 * voice and B on full-rate data. A loses a FICH and lays its frame out as voice, not as the data layout B last
 * verified. */
static void
test_process_ysf_fich_fallback_is_per_decoder_state(void) {
    static dsd_opts opts;
    static dsd_state state_a;
    static dsd_state state_b;
    uint8_t fich_vd1[48];
    uint8_t fich_data[48];
    uint8_t fich_input[100];
    uint8_t dch_vd1[180];
    uint8_t dch_data0[180];
    uint8_t dch_data1[180];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state_a, 0, sizeof(state_a));
    DSD_MEMSET(&state_b, 0, sizeof(state_b));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    state_a.synctype = DSD_SYNC_YSF_NEG;
    state_b.synctype = DSD_SYNC_YSF_NEG;

    make_fich_bits_for_vd_type1(fich_vd1);
    make_fich_bits_for_full_rate_data(fich_data);
    encode_dch_payload_to_input("VD1DST0001VD1SRC0002", 20U, dch_vd1, 176U, 9U, 0);
    encode_dch_payload_to_input("PYSFDST001PYSFSRC002", 20U, dch_data0, 176U, 9U, 0);
    encode_dch_payload_to_input("PYSFUPL003PYSFDNL004", 20U, dch_data1, 176U, 9U, 0);

    /* A verifies a V/D1 FICH. */
    encode_fich_input(fich_vd1, 0, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch_vd1, 1U);
    assert(processYSF(&opts, &state_a) == 1);
    assert(state_a.ysf_dt == 0U);

    /* B verifies a full-rate data FICH. */
    encode_fich_input(fich_data, 0, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    append_dibits_to_stream(fich_input, 100U);
    append_interleaved_full_rate_dch_blocks(dch_data0, dch_data1);
    assert(processYSF(&opts, &state_b) == 1);
    assert(state_b.ysf_dt == 1U);

    /* A's next FICH fails: its own V/D1 layout plays, not B's data layout. */
    encode_fich_input(fich_vd1, 1, 0, fich_input);
    g_dibit_stream_len = 0U;
    g_dibit_stream_pos = 0U;
    g_process_mbe_call_count = 0;
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch_vd1, 1U);
    assert(processYSF(&opts, &state_a) == 1);
    assert(g_dibit_stream_pos == g_dibit_stream_len);
    assert(g_process_mbe_call_count == 5);
    assert(state_a.ysf_dt == 0U);
    assert(state_b.ysf_dt == 1U);

    dsd_state_ext_free_all(&state_a);
    dsd_state_ext_free_all(&state_b);
}

/* The output buffers the gain stage (dsd_voice_chain_run()) and short staging write, for a zeroed state: V/D2 frames
   run it in either output format (issue #652). */
static void
prepare_gain_stage(dsd_state* state) {
    static short out_buf[100 + (160 * 96)];
    state->audio_out_buf = out_buf;
    state->audio_out_buf_p = out_buf + 100;
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
    /* Issue #890: with no scan running nobody reads checked media, so the voice path keeps none. */
    assert(call.checked_media_updated_m <= 0.0);
    assert(strcmp(state.err_str, "========") == 0);
    // The fifth stubbed frame (5.0), scaled to the int16 scale of the voice buffers and gained once (x3) into the short
    // frame the mixers play, in float output as in 16-bit output.
    const short once = (short)(3.0F * 5.0F * DSD_MBE_PCM16_PER_FLOAT);
    assert(state.s_l[0] == once);
    assert(state.s_l[159] == once);
    assert(fabsf(state.audio_out_temp_buf[0] - (3.0F * 5.0F * DSD_MBE_PCM16_PER_FLOAT)) <= 1e-6F);
}

/* What V/D2 tells the voice chain about each frame it decodes itself, the vocoder bypassed: the media mbelib's result
   says (speech, a tone, a repeat, a mute, and a frame whose synthesis failed), decodable (YSF voice is never
   encrypted) and admitted. */
static dsd_voice_frame_info g_ysf_info_seen[5];
static int g_ysf_info_count;
static int g_ysf_info_bad_calls;

static void
ysf_info_observe(void* ctx, int slot, const float* frame, const dsd_voice_frame_info* info) {
    (void)ctx;
    (void)frame;
    if (slot != 0 || !info || g_ysf_info_count >= 5) {
        g_ysf_info_bad_calls++;
        return;
    }
    g_ysf_info_seen[g_ysf_info_count++] = *info;
}

/* @p refused: V/D2 decodes inside a receive context whose verdict refuses the traffic. YSF itself carries no identity
   the receive policy names, so the policy admits YSF's own traffic; a context naming NXDN with every RAN blocked
   stands in for a refusal, to show V/D2 passes on the receive verdict it is given. */
static void
run_vd_type2_frame_info(int refused) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch2[100];

    InitAllFecFunction();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_mbe_call_count = 0;
    opts.audio_gain = 1.0F;
    prepare_gain_stage(&state);
    g_mbe_flags[0] = MBE_PROCESS_FLAG_C0_VALID;
    g_mbe_flags[1] = MBE_PROCESS_FLAG_TONE;
    g_mbe_flags[2] = MBE_PROCESS_FLAG_REPEAT;
    g_mbe_flags[3] = MBE_PROCESS_FLAG_REPEAT | MBE_PROCESS_FLAG_MUTE;
    g_mbe_fail_at = 4;
    if (refused) {
        opts.receive.filters[DSD_RECEIVE_RAN].enabled = 1;
        DSD_MEMSET(&opts.receive.filters[DSD_RECEIVE_RAN].block, 0xFF, sizeof(opts.receive.filters[0].block));
        (void)dsd_receive_context_begin(&state, DSD_SYNC_NXDN_POS);
    }

    make_fich_bits_for_vd_type2(fich_bits);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input("VD2SRC4444", 10U, dch2, 96U, 5U, 0);
    append_dibits_to_stream(fich_input, 100U);
    for (size_t block = 0; block < 5U; block++) {
        append_dibits_to_stream(&dch2[block * 20U], 20U);
        append_type2_vech_dibits(0U);
    }

    g_ysf_info_count = 0;
    g_ysf_info_bad_calls = 0;
    opts.trunk_scan_enabled = 1; /* a scan that reads checked media (issue #890) */
    begin_test_ysf_call(&state);
    dsd_voice_chain_set_observer_for_test(ysf_info_observe, NULL);
    processYSF(&opts, &state);
    dsd_voice_chain_set_observer_for_test(NULL, NULL);
    DSD_MEMSET(g_mbe_flags, 0, sizeof(g_mbe_flags));
    g_mbe_fail_at = -1;

    assert(g_mbe_call_count == 5);
    assert(g_ysf_info_count == 5 && g_ysf_info_bad_calls == 0);
    static const uint8_t want_media[5] = {DSD_VOICE_MEDIA_SPEECH, DSD_VOICE_MEDIA_TONE, DSD_VOICE_MEDIA_CONCEALED,
                                          DSD_VOICE_MEDIA_CONCEALED, DSD_VOICE_MEDIA_NONE};
    for (int i = 0; i < 5; i++) {
        assert(g_ysf_info_seen[i].media == want_media[i]);
        assert(g_ysf_info_seen[i].decodable == 1U);
        assert(g_ysf_info_seen[i].admitted == (refused ? 0U : 1U));
        assert(g_ysf_info_seen[i].talkspurt == 0U);
    }
    /* Issue #890: only the speech mbelib-neo returned is the call's checked media, the scan's voice credit, and only
       for admitted traffic; the tone, the repeat, the mute and the failed frame are none, though they marked media. */
    const dsd_call_snapshot call = get_test_ysf_call(&state);
    assert(call.media_updated_m > 0.0);
    if (refused) {
        assert(call.checked_media_updated_m <= 0.0);
    } else {
        assert(call.checked_media_started_m > 0.0);
        assert(fabs(call.checked_media_updated_m - call.checked_media_started_m) < 1e-9);
    }
    dsd_state_ext_free_all(&state);
}

static void
test_process_ysf_vd_type2_tells_the_chain_each_frame(void) {
    run_vd_type2_frame_info(0);
    run_vd_type2_frame_info(1);
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
    encode_dch_payload_to_input("TERMDNL004TERMUPL003", 20U, dch1, 176U, 9U, 0);

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

/* A FICH with fields @p fi, @p cm, @p fn, @p ft and @p dt, the rest as a V/D type 1 frame's. */
static void
make_ysf_fich(uint8_t fich_bits[48], uint8_t fi, uint8_t cm, uint8_t fn, uint8_t ft, uint8_t dt) {
    make_fich_bits_for_vd_type1(fich_bits);
    set_bits_msb(fich_bits, 0U, 2U, fi);
    set_bits_msb(fich_bits, 4U, 2U, cm);
    set_bits_msb(fich_bits, 10U, 3U, fn);
    set_bits_msb(fich_bits, 13U, 3U, ft);
    set_bits_msb(fich_bits, 22U, 2U, dt);
    append_ysf_crc(fich_bits);
}

/* The header (@p fi 0) or terminator (2) of a V/D type 1 call in call mode @p cm, carrying CSD1 @p csd1 (destination,
   source) and CSD2 @p csd2 (downlink, uplink), twenty characters each; with @p corrupt_dch, both DCH blocks fail their
   CRC while the FICH verifies. */
static void
append_ysf_bound_frame_ex(uint8_t fi, uint8_t cm, const char* csd1, const char* csd2, int corrupt_dch) {
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch0[180];
    uint8_t dch1[180];
    make_ysf_fich(fich_bits, fi, cm, 0U, 1U, 0U);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input(csd1, 20U, dch0, 176U, 9U, corrupt_dch);
    encode_dch_payload_to_input(csd2, 20U, dch1, 176U, 9U, corrupt_dch);
    append_dibits_to_stream(fich_input, 100U);
    append_interleaved_full_rate_dch_blocks(dch0, dch1);
}

static void
append_ysf_bound_frame(uint8_t fi, uint8_t cm, const char* csd1, const char* csd2) {
    append_ysf_bound_frame_ex(fi, cm, csd1, csd2, 0);
}

/* V/D type 1 voice frame @p fn of a call in call mode @p cm, its DCH @p dch_text (twenty characters): CSD1 in frame 0,
   CSD2 in frame 1. */
static void
append_ysf_vd1_frame(uint8_t cm, uint8_t fn, const char* dch_text) {
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch[180];
    make_ysf_fich(fich_bits, 1U, cm, fn, 6U, 0U);
    encode_fich_input(fich_bits, 0, 0, fich_input);
    encode_dch_payload_to_input(dch_text, 20U, dch, 176U, 9U, 0);
    append_dibits_to_stream(fich_input, 100U);
    append_vd_type1_blocks(dch, 1U);
}

/* A YSF decoder on the heap, its history kept, with --rdio-unknown-talkgroup 4242. */
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    Event_History_I* history;
} ysf_call_rig;

static void
ysf_call_rig_open(ysf_call_rig* rig) {
    InitAllFecFunction();
    rig->opts = (dsd_opts*)calloc(1, sizeof(*rig->opts));
    rig->state = (dsd_state*)calloc(1, sizeof(*rig->state));
    rig->history = (Event_History_I*)calloc(2, sizeof(*rig->history));
    assert(rig->opts != NULL && rig->state != NULL && rig->history != NULL);
    init_event_history(&rig->history[0], 0U, 255U);
    init_event_history(&rig->history[1], 0U, 255U);
    rig->state->event_history_s = rig->history;
    rig->opts->rdio_unknown_talkgroup = 4242;
}

static void
ysf_call_rig_close(ysf_call_rig* rig) {
    dsd_state_ext_free_all(rig->state);
    free(rig->history);
    free(rig->state);
    free(rig->opts);
}

/* Plays the frame the stream holds and syncs the slot's history, as the frame sync does before the next frame, then
   empties the stream. */
static void
ysf_call_rig_play(ysf_call_rig* rig) {
    processYSF(rig->opts, rig->state);
    assert(g_dibit_stream_pos == g_dibit_stream_len);
    dsd_event_sync_slot(rig->opts, rig->state, 0U);
    reset_ysf_stream();
}

/* The talkgroup the export gives committed row @p n (1 the latest), the fallback allowed: 4242 when the row takes
   it. */
static uint32_t
ysf_committed_talkgroup(const ysf_call_rig* rig, int n) {
    return dsd_rdio_call_talkgroup(rig->opts, &rig->history[0].Event_History_Items[n], 1);
}

/* Issue #888: an ordinary V/D type 1 call through a repeater, between a header and a terminator: frame 1's DCH (CSD2)
   names the repeaters, which the call keeps as its route; its endpoints stay the caller's. The call is one row, the
   caller's, and, ended by its terminator, it takes the fallback talkgroup. Before, every V/D type 1 frame's DCH was
   read as CSD1, so frame 1 named the repeaters as the call's endpoints and began another call. */
static void
test_process_ysf_repeater_call_is_one_row_that_takes_the_fallback(void) {
    ysf_call_rig rig;
    ysf_call_rig_open(&rig);
    reset_ysf_stream();
    append_ysf_bound_frame(0U, 0U, "CALLDST001CALLSRC002", "RPTDNL0004RPTUPL0003");
    ysf_call_rig_play(&rig);
    append_ysf_vd1_frame(0U, 0U, "CALLDST001CALLSRC002");
    ysf_call_rig_play(&rig);
    append_ysf_vd1_frame(0U, 1U, "RPTDNL0004RPTUPL0003");
    ysf_call_rig_play(&rig);
    const dsd_call_snapshot call = get_test_ysf_call(rig.state);
    assert(strcmp(call.source_text, "CALLSRC002") == 0);
    assert(strcmp(call.target_text, "CALLDST001") == 0);
    assert(strcmp(call.route_text[0], "RPTUPL0003") == 0);
    append_ysf_vd1_frame(0U, 0U, "CALLDST001CALLSRC002");
    ysf_call_rig_play(&rig);
    append_ysf_bound_frame(2U, 0U, "CALLDST001CALLSRC002", "RPTDNL0004RPTUPL0003");
    ysf_call_rig_play(&rig);
    const dsd_call_snapshot ended = get_test_ysf_call(rig.state);
    assert(ended.epoch == call.epoch && ended.phase == DSD_CALL_PHASE_ENDED);
    assert(rig.history[0].push_seq == 1U);
    assert(strcmp(rig.history[0].Event_History_Items[1].src_str, "CALLSRC002") == 0);
    assert(ysf_committed_talkgroup(&rig, 1) == 4242U);
    ysf_call_rig_close(&rig);
}

/* Issue #888: a YSF call takes the fallback talkgroup only when it has ended on a decoded terminator, as the export
   reads its row. A call ended by a sync loss, or by a change of call mode in band (a group call, then a private one
   with no header or terminator between them), does not: its recording may also hold the start of what came next. The
   private call, ended by its terminator, does. Before, every committed YSF row took the fallback. */
static void
test_process_ysf_call_takes_the_fallback_only_after_a_terminator(void) {
    for (int sync_loss = 1; sync_loss >= 0; sync_loss--) {
        ysf_call_rig rig;
        ysf_call_rig_open(&rig);
        reset_ysf_stream();
        append_ysf_bound_frame(0U, 0U, "CALLDST001CALLSRC002", "RPTDNL0004RPTUPL0003");
        ysf_call_rig_play(&rig);
        append_ysf_vd1_frame(0U, 0U, "CALLDST001CALLSRC002");
        ysf_call_rig_play(&rig);
        if (sync_loss) {
            assert(dsd_call_state_end_ex(rig.state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) > 0);
            dsd_event_sync_slot(rig.opts, rig.state, 0U);
            assert(rig.history[0].push_seq == 1U);
            assert(ysf_committed_talkgroup(&rig, 1) == 0U);
        } else {
            append_ysf_vd1_frame(3U, 0U, "CALLDST001CALLSRC002");
            ysf_call_rig_play(&rig);
            assert(get_test_ysf_call(rig.state).kind == DSD_CALL_KIND_PRIVATE_VOICE);
            assert(rig.history[0].push_seq == 1U);
            assert(rig.history[0].Event_History_Items[1].gi == 0);
            assert(ysf_committed_talkgroup(&rig, 1) == 0U);
            append_ysf_bound_frame(2U, 3U, "CALLDST001CALLSRC002", "RPTDNL0004RPTUPL0003");
            ysf_call_rig_play(&rig);
            assert(rig.history[0].push_seq == 2U);
            assert(rig.history[0].Event_History_Items[1].gi == 1);
            assert(ysf_committed_talkgroup(&rig, 1) == 4242U);
        }
        ysf_call_rig_close(&rig);
    }
}

/* Issue #888: a YSF call takes the fallback talkgroup only on its own terminator, decoded by YSF. A call that another
   protocol's terminator ends on the slot (as a P25 Phase 1 TDU heard next in Auto mode ends slot 0 on a terminator,
   whatever call it holds) does not. Before, any terminator end let the YSF row take the fallback. */
static void
test_process_ysf_call_takes_the_fallback_only_on_its_own_terminator(void) {
    ysf_call_rig rig;
    ysf_call_rig_open(&rig);
    reset_ysf_stream();
    append_ysf_bound_frame(0U, 0U, "CALLDST001CALLSRC002", "RPTDNL0004RPTUPL0003");
    ysf_call_rig_play(&rig);
    append_ysf_vd1_frame(0U, 0U, "CALLDST001CALLSRC002");
    ysf_call_rig_play(&rig);
    assert(dsd_call_state_end_ex(rig.state, 0U, 0.0, DSD_CALL_END_TERMINATOR) > 0);
    dsd_event_sync_slot(rig.opts, rig.state, 0U);
    const dsd_call_snapshot ended = get_test_ysf_call(rig.state);
    assert(ended.phase == DSD_CALL_PHASE_ENDED && ended.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
    assert(rig.history[0].push_seq == 1U);
    assert(strcmp(rig.history[0].Event_History_Items[1].src_str, "CALLSRC002") == 0);
    assert(ysf_committed_talkgroup(&rig, 1) == 0U);
    ysf_call_rig_close(&rig);
}

/* Issue #888: a YSF terminator is the call's own only when its verified FICH's call mode is the one the call's own
   verified FICHs last named. An open group call (CM 0), then a terminator whose FICH verifies but whose DCH fails, so no
   callsign names another call: a private call's (CM 3) or a Radio ID call's (CM 1). The group call ends on that
   terminator, but its row does not take the fallback talkgroup. The group call's own terminator (CM 0), its DCH failed
   too, still gives it the fallback. Before, only the terminator's protocol was checked, so the group row took the
   fallback on the other call's terminator; and then only the call kind its mode maps to, so a Radio ID terminator,
   generic voice, still did. */
static void
test_process_ysf_call_takes_the_fallback_only_on_a_terminator_of_its_call_mode(void) {
    static const uint8_t k_modes[] = {3U, 1U, 0U};
    for (size_t m = 0; m < sizeof k_modes; m++) {
        const uint8_t mode = k_modes[m];
        ysf_call_rig rig;
        ysf_call_rig_open(&rig);
        reset_ysf_stream();
        append_ysf_bound_frame(0U, 0U, "CALLDST001CALLSRC002", "RPTDNL0004RPTUPL0003");
        ysf_call_rig_play(&rig);
        append_ysf_vd1_frame(0U, 0U, "CALLDST001CALLSRC002");
        ysf_call_rig_play(&rig);
        const dsd_call_snapshot call = get_test_ysf_call(rig.state);
        assert(call.kind == DSD_CALL_KIND_GROUP_VOICE);
        append_ysf_bound_frame_ex(2U, mode, "CALLDST009CALLSRC008", "RPTDNL0007RPTUPL0006", 1);
        ysf_call_rig_play(&rig);
        const dsd_call_snapshot ended = get_test_ysf_call(rig.state);
        assert(ended.epoch == call.epoch && ended.phase == DSD_CALL_PHASE_ENDED);
        assert(ended.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
        assert(ended.own_terminator == (mode == 0U ? 1U : 0U));
        assert(rig.history[0].push_seq == 1U);
        assert(strcmp(rig.history[0].Event_History_Items[1].src_str, "CALLSRC002") == 0);
        assert(ysf_committed_talkgroup(&rig, 1) == (mode == 0U ? 4242U : 0U));
        ysf_call_rig_close(&rig);
    }
}

/* Issue #888: a terminator heard after a call faded is that call's own only when its verified callsigns, where it has
   them, are the call's. A group call from CALLSRC002 ends on a sync loss (its YSF strings cleared, as noCarrier() does),
   then a group terminator (CM 0) arrives whose verified DCH names another caller, CALLSRC008: the faded call's end
   becomes a terminator end, but not its own. The caller's own terminator, and one whose DCH failed (no callsign to
   compare), still mark it as its own. Before, the faded call was marked by any terminator of its call mode. */
static void
test_process_ysf_faded_call_takes_no_other_callers_terminator(void) {
    static const struct {
        const char* csd1;
        int corrupt_dch;
        uint8_t own;
    } k_cases[] = {
        {"CALLDST001CALLSRC008", 0, 0U},
        {"CALLDST001CALLSRC002", 0, 1U},
        {"CALLDST001CALLSRC008", 1, 1U},
    };

    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        ysf_call_rig rig;
        ysf_call_rig_open(&rig);
        reset_ysf_stream();
        append_ysf_bound_frame(0U, 0U, "CALLDST001CALLSRC002", "RPTDNL0004RPTUPL0003");
        ysf_call_rig_play(&rig);
        append_ysf_vd1_frame(0U, 0U, "CALLDST001CALLSRC002");
        ysf_call_rig_play(&rig);
        const dsd_call_snapshot call = get_test_ysf_call(rig.state);
        assert(strcmp(call.source_text, "CALLSRC002") == 0);
        assert(dsd_call_state_end_ex(rig.state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) > 0);
        dsd_event_sync_slot(rig.opts, rig.state, 0U);
        rig.state->ysf_dt = 9U;
        rig.state->ysf_fi = 9U;
        rig.state->ysf_cm = 9U;
        rig.state->ysf_fich_confirmed = 0U;
        append_ysf_bound_frame_ex(2U, 0U, k_cases[c].csd1, "RPTDNL0004RPTUPL0003", k_cases[c].corrupt_dch);
        ysf_call_rig_play(&rig);
        const dsd_call_snapshot ended = get_test_ysf_call(rig.state);
        assert(ended.epoch == call.epoch && ended.phase == DSD_CALL_PHASE_ENDED);
        assert(strcmp(ended.source_text, "CALLSRC002") == 0);
        assert(ended.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
        if (ended.own_terminator != k_cases[c].own) {
            DSD_FPRINTF(stderr, "faded call, terminator %s (DCH %s): own %u, want %u\n", k_cases[c].csd1,
                        k_cases[c].corrupt_dch ? "failed" : "verified", (unsigned)ended.own_terminator,
                        (unsigned)k_cases[c].own);
        }
        assert(ended.own_terminator == k_cases[c].own);
        ysf_call_rig_close(&rig);
    }
}

#ifndef DSD_YSF_TEST_REAL_VOCODER
/* V/D2 decodes straight through mbelib: each block is captured after the vocoder settled its error counts (0 for a
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
        /* The record packs the C0 and total counts (DSD_MBE_AMB_ERR_FLAG); without C0 context both are the total. */
        const unsigned total = block == 3 ? 0U : (unsigned)(4 + block);
        const unsigned c0 = total > (unsigned)DSD_MBE_AMB_ERR_C0_MAX ? (unsigned)DSD_MBE_AMB_ERR_C0_MAX : total;
        assert(rec[0] == (unsigned char)(DSD_MBE_AMB_ERR_FLAG | (c0 << DSD_MBE_AMB_ERR_C0_SHIFT) | total));
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

/* A FICH that failed lays the payload out from the last one that passed, which is this transmission's only once one of
   its own FICHs verified. Before that the layout is an earlier transmission's: a YSF sync word matched on noise after a
   V/D2 call played that call's layout as five frames of voice, and opened a call. Such a frame lays out, plays, captures
   and publishes nothing, and the hunt reads on through it; once a FICH verifies this transmission, its own frame
   included, the fallback frames play and are captured. */
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

    assert(dsd_call_state_end(&state, 0U, 0.0) == 1);

    capture_dir_begin(&opts, "dsdneo_ysf_fich_capture");
    reset_ysf_stream();
    append_vd_type2_frame(1);
    assert(processYSF(&opts, &state) == 0);
    assert(g_mbe_call_count == 0);
    assert(g_dibit_stream_pos == 100U);
    assert(capture_dir_count(NULL, 0) == 0);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);

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
    encode_dch_payload_to_input("TERMDNL004TERMUPL003", 20U, dch1, 176U, 9U, 0);

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

/* Whether the frame just read armed the recording of the next frame's sync word (ysf_arm_next_sync_anchor()). */
static int
next_word_armed(dsd_state* state) {
    const dsd_symbol_anchor* anchor = DSD_STATE_EXT_GET_AS(dsd_symbol_anchor, state, DSD_STATE_EXT_DSP_SYMBOL_ANCHOR);
    return anchor != NULL && dsd_symbol_anchor_recording(anchor);
}

/* Issue #661: a voice block read across the carrier boundary is dropped with the rest of the frame, as V/D1 drops it,
   in V/D2 and in full rate alike. Issue #938: the cut frame arms no recording of a next word, which does not follow it
   (the hunt goes on inside the new carrier); a whole frame arms one. */
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
    (void)processYSF(&opts, &state);
    assert(state.carrier_seq == 0U);
    assert(next_word_armed(&state));
    dsd_state_ext_free_all(&state);

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
    assert(!next_word_armed(&state));
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
    assert(!next_word_armed(&state));
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

/* One full-rate data or V/D type 1 frame with FICH fields @p fi, @p dt and @p cm (its CRC broken when @p bad_fich),
   decoded on a row of scan class @p row (INHERIT: none entered): the PROVEN notes a quiet check heard, the -Y content
   it moved (@p content_moved), and the trunk scan reports it made (g_test_scan_activity). */
static int
ysf_scan_frame(uint32_t fi, uint32_t dt, uint32_t cm, int bad_fich, dsd_scan_mode row, uint32_t* content_moved) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t fich_bits[48];
    uint8_t fich_input[100];
    uint8_t dch0[180];
    uint8_t dch1[180];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(g_dibit_stream, 0, sizeof(g_dibit_stream));
    g_dibit_stream_len = 0;
    g_dibit_stream_pos = 0;
    g_process_mbe_call_count = 0;
    if (row != DSD_SCAN_MODE_INHERIT) {
        assert(dsd_scan_mode_enter(&opts, &state, row) == 0);
    }
    opts.scan_skip_silent = 1;
    state.synctype = DSD_SYNC_YSF_POS;
    make_fich_bits_for_full_rate_data(fich_bits);
    set_bits_msb(fich_bits, 0U, 2U, fi);
    set_bits_msb(fich_bits, 4U, 2U, cm);
    set_bits_msb(fich_bits, 22U, 2U, dt);
    /* Some words this fixture's FICH encoder does not round-trip (its convolutional tail): the squelch code, which no
       case reads, is stepped until the word decodes. */
    for (uint32_t sc = 0U; sc < 128U; sc++) {
        set_bits_msb(fich_bits, 25U, 7U, sc);
        append_ysf_crc(fich_bits);
        encode_fich_input(fich_bits, 0, 0, fich_input);
        uint8_t probe[32];
        if (ysf_conv_fich(fich_input, probe, NULL) == 0) {
            break;
        }
    }
    encode_fich_input(fich_bits, bad_fich, 0, fich_input);
    encode_dch_payload_to_input("SCANDST001SCANSRC002", 20U, dch0, 176U, 9U, 0);
    encode_dch_payload_to_input("SCANUPL003SCANDNL004", 20U, dch1, 176U, 9U, 0);
    append_dibits_to_stream(fich_input, 100U);
    if (fi == 1U && dt == 0U) {
        append_vd_type1_blocks(dch0, 3U);
    } else {
        append_interleaved_full_rate_dch_blocks(dch0, dch1);
    }
    dsd_frame_sync_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.content_evidence = record_quiet_note;
    dsd_frame_sync_hooks_set(&hooks);
    dsd_test_scan_activity_install();
    g_quiet_proven = 0;
    const uint32_t content = state.scan_y_content_seq;
    (void)processYSF(&opts, &state);
    *content_moved = state.scan_y_content_seq - content;
    dsd_frame_sync_hooks_set(NULL);
    dsd_trunk_scan_hooks_set(NULL);
    dsd_state_ext_free_all(&state);
    return g_quiet_proven;
}

/* Issue #890: a FICH whose CRC passed reports its frame to trunk scan's policy table when it is a header (FI 0) or a
   communication frame (FI 1): DT 1 a data call, CM 3 an individual one, CM 1 (radio ID) and CM 2 of unknown party,
   never encrypted. A terminator (FI 2), a test frame (FI 3) and a failed FICH report nothing. */
static void
test_process_ysf_reports_checked_calls_to_trunk_scan(void) {
    InitAllFecFunction();
    uint32_t moved = 0U;
    (void)ysf_scan_frame(0U, 1U, 3U, 0, DSD_SCAN_MODE_INHERIT, &moved);
    assert(g_test_scan_activity.count == 1 && DSD_SYNC_IS_YSF(g_test_scan_activity.protocol));
    assert(g_test_scan_activity.slot == -1);
    const dsd_trunk_scan_activity_identity* id = &g_test_scan_activity.last;
    assert(id->known
           == (DSD_SCAN_ACTIVITY_KNOWN_DATA | DSD_SCAN_ACTIVITY_KNOWN_ENCRYPTED | DSD_SCAN_ACTIVITY_KNOWN_PARTY));
    assert(id->data_call == 1U && id->is_private == 1U && id->encrypted == 0U);
    (void)ysf_scan_frame(1U, 0U, 0U, 0, DSD_SCAN_MODE_INHERIT, &moved);
    assert(g_test_scan_activity.count == 1 && g_test_scan_activity.last.data_call == 0U
           && g_test_scan_activity.last.is_private == 0U);
    (void)ysf_scan_frame(0U, 1U, 2U, 0, DSD_SCAN_MODE_INHERIT, &moved);
    assert(g_test_scan_activity.count == 1 && (g_test_scan_activity.last.known & DSD_SCAN_ACTIVITY_KNOWN_PARTY) == 0U);
    /* Radio ID mode (CM 1) addresses a destination radio ID: neither a group nor an individual call. */
    (void)ysf_scan_frame(0U, 1U, 1U, 0, DSD_SCAN_MODE_INHERIT, &moved);
    assert(g_test_scan_activity.count == 1 && (g_test_scan_activity.last.known & DSD_SCAN_ACTIVITY_KNOWN_PARTY) == 0U);
    (void)ysf_scan_frame(2U, 1U, 0U, 0, DSD_SCAN_MODE_INHERIT, &moved);
    assert(g_test_scan_activity.count == 0);
    (void)ysf_scan_frame(3U, 1U, 0U, 0, DSD_SCAN_MODE_INHERIT, &moved);
    assert(g_test_scan_activity.count == 0);
    (void)ysf_scan_frame(0U, 1U, 0U, 1, DSD_SCAN_MODE_INHERIT, &moved);
    assert(g_test_scan_activity.count == 0);
}

/* Issue #890 F3: a test frame (FI 3) is channel activity to an untyped row, its FICH and each DCH PROVEN, but to a
   row of the ysf class it proves the profile only: no PROVEN note and no -Y content. A header on that row is content as
   ever. */
static void
test_process_ysf_test_frame_is_no_traffic_for_a_typed_row(void) {
    InitAllFecFunction();
    uint32_t moved = 0U;
    assert(ysf_scan_frame(3U, 1U, 0U, 0, DSD_SCAN_MODE_INHERIT, &moved) >= 2 && moved >= 2U);
    assert(ysf_scan_frame(3U, 1U, 0U, 0, DSD_SCAN_MODE_YSF, &moved) == 0 && moved == 0U);
    assert(ysf_scan_frame(0U, 1U, 0U, 0, DSD_SCAN_MODE_YSF, &moved) >= 2 && moved >= 2U);
}

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
    test_ysf_conv_dch2_search_evidence_follows_the_callsign();
    test_process_ysf_header_search_evidence_follows_the_callsigns();
    test_process_ysf_full_rate_data_routes_fich_and_dch_state();
    test_process_ysf_vd_type1_routes_ehr_voice_and_dch_state();
    test_process_ysf_vd_type1_split_by_a_carrier_boundary_publishes_nothing();
    test_process_ysf_frame_cut_by_an_input_interruption_publishes_nothing();
    test_process_ysf_bad_fich_fallback_reopens_ysf_epoch();
    test_ysf_checked_fich_and_dch_are_proven_content();
    test_process_ysf_reports_checked_calls_to_trunk_scan();
    test_process_ysf_test_frame_is_no_traffic_for_a_typed_row();
    test_process_ysf_fich_verdict_is_sticky_per_transmission();
    test_process_ysf_fich_fallback_is_per_decoder_state();
    test_process_ysf_vd_type2_routes_dch2_voice_and_audio_errors();
    test_process_ysf_vd_type2_tells_the_chain_each_frame();
    test_process_ysf_terminator_enriches_identity_before_call_end();
    test_process_ysf_repeater_call_is_one_row_that_takes_the_fallback();
    test_process_ysf_call_takes_the_fallback_only_after_a_terminator();
    test_process_ysf_call_takes_the_fallback_only_on_its_own_terminator();
    test_process_ysf_call_takes_the_fallback_only_on_a_terminator_of_its_call_mode();
    test_process_ysf_faded_call_takes_no_other_callers_terminator();
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
