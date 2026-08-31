// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Conformance of the dPMR CCH pipeline against external reference vectors.
 *
 * Every stage here was inherited from the dsd-fme fork and had only ever been checked
 * against itself: the CRC-7 test pinned values the implementation produced, and the
 * voice-bridge test stubs Hamming_12_8_decode() so its CRC assertion holds on any
 * polynomial. Nothing would have failed had the taps, bit order, interleave direction
 * or parity matrix been wrong -- which mattered, because the CCH CRC-7 passes on no
 * superframe of the committed dpmr I/Q fixture (#407).
 *
 * The vectors in fixtures/dpmr_reference_vectors.h come from an independent model of
 * the ETSI TS 102 658 encode direction, cross-derived from DSDcc. This test decodes
 * them with the real FEC linked in, first stage by stage so a failure names the stage,
 * then end to end through processdPMRvoice() itself.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dpmr/dpmr_data.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dpmr_internal.h"
#include "dpmr_reference_vectors.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* The dibit stream processdPMRvoice() reads, and how far into it we are. */
static const uint8_t* g_dibits;
static size_t g_dibit_count;
static size_t g_dibit_pos;
static size_t g_mbe_calls;

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    if (out_analog_signal != NULL) {
        *out_analog_signal = 0;
    }
    if (g_dibits == NULL || g_dibit_pos >= g_dibit_count) {
        return 0;
    }
    return (int)g_dibits[g_dibit_pos++];
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    g_mbe_calls++;
}

void
playSynthesizedVoiceMS(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
playSynthesizedVoiceFM(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

static int g_failures;

static void
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %ld want %ld\n", tag, got, want);
        g_failures++;
    }
}

static void
expect_bits(const char* tag, const uint8_t* got, const uint8_t* want, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if ((got[i] & 1U) != (want[i] & 1U)) {
            DSD_FPRINTF(stderr, "%s[%zu]: got %u want %u\n", tag, i, (unsigned)got[i], (unsigned)want[i]);
            g_failures++;
            return;
        }
    }
}

/* Descramble the reference keystream out of a buffer, or back into one. */
static void
apply_keystream(const uint8_t* in, uint8_t* out, size_t count) {
    for (size_t i = 0; i < count; i++) {
        out[i] = (uint8_t)((in[i] ^ DPMR_REF_SCRAMBLER_KEYSTREAM[i]) & 1U);
    }
}

static void
test_scrambler_matches_the_reference_keystream(void) {
    uint8_t zeros[72];
    uint8_t masked[72];
    DSD_MEMSET(zeros, 0, sizeof(zeros));
    DSD_MEMSET(masked, 0, sizeof(masked));

    uint32_t lfsr = DPMR_REF_SCRAMBLER_SEED;
    dpmr_scrambled_pmr_bits(&lfsr, zeros, masked, 72U);

    expect_bits("scrambler-keystream", masked, DPMR_REF_SCRAMBLER_KEYSTREAM, 72U);
    expect_int("scrambler-final-state", (long)lfsr, (long)DPMR_REF_SCRAMBLER_FINAL_STATE);

    /* Masking twice with the same seed is the identity: the scrambler is its own inverse. */
    uint8_t restored[72];
    lfsr = DPMR_REF_SCRAMBLER_SEED;
    dpmr_scrambled_pmr_bits(&lfsr, masked, restored, 72U);
    expect_bits("scrambler-round-trip", restored, zeros, 72U);
}

static void
test_deinterleave_matches_the_reference_permutation(void) {
    uint8_t input[72];
    uint8_t output[72];
    for (uint32_t i = 0; i < 72U; i++) {
        input[i] = (uint8_t)(i & 1U);
        output[i] = 0U;
    }

    /* Index identity: bit i of the deinterleaved buffer is bit DPMR_REF_INTERLEAVE_INDEX[i]
       of the on-air buffer. Checked over a basis so a permutation error cannot cancel out. */
    for (uint32_t bit = 0; bit < 72U; bit++) {
        DSD_MEMSET(input, 0, sizeof(input));
        input[bit] = 1U;
        dpmr_deinterleave_6x12(input, output);
        for (uint32_t i = 0; i < 72U; i++) {
            const uint8_t want = (DPMR_REF_INTERLEAVE_INDEX[i] == bit) ? 1U : 0U;
            if (output[i] != want) {
                DSD_FPRINTF(stderr, "deinterleave-basis[%u][%u]: got %u want %u\n", bit, i, (unsigned)output[i],
                            (unsigned)want);
                g_failures++;
                return;
            }
        }
    }
}

static void
test_hamming_decodes_the_reference_codewords(void) {
    for (int v = 0; v < DPMR_REF_HAMMING_VECTOR_COUNT; v++) {
        const dpmr_ref_hamming_vector* vector = &DPMR_REF_HAMMING_VECTORS[v];
        uint8_t rx[12];
        uint8_t decoded[8];
        char tag[64];

        /* A clean codeword decodes to its data, untouched. */
        DSD_MEMCPY(rx, vector->codeword, sizeof(rx));
        DSD_MEMSET(decoded, 0xFF, sizeof(decoded));
        DSD_SNPRINTF(tag, sizeof(tag), "hamming-clean-%d", v);
        expect_int(tag, Hamming_12_8_decode(rx, decoded, 1) ? 1 : 0, 1);
        DSD_SNPRINTF(tag, sizeof(tag), "hamming-clean-data-%d", v);
        expect_bits(tag, decoded, vector->data, sizeof(decoded));

        /* And a single bit error in any position is corrected back to it. */
        for (uint32_t position = 0; position < 12U; position++) {
            DSD_MEMCPY(rx, vector->codeword, sizeof(rx));
            rx[position] ^= 1U;
            DSD_MEMSET(decoded, 0xFF, sizeof(decoded));
            DSD_SNPRINTF(tag, sizeof(tag), "hamming-correct-%d-%u", v, position);
            expect_int(tag, Hamming_12_8_decode(rx, decoded, 1) ? 1 : 0, 1);
            DSD_SNPRINTF(tag, sizeof(tag), "hamming-correct-data-%d-%u", v, position);
            expect_bits(tag, decoded, vector->data, sizeof(decoded));
        }
    }

    /* Sweep every syndrome the decoder can see. Flipping parity bit k of a valid codeword
       sets syndrome bit k, so the four parity positions reach all 16 syndromes directly.
       Exactly 13 must be accepted -- the zero syndrome plus the 12 placeable single-bit
       errors -- and the remaining three refused. That 13/16 is the arithmetic behind #407:
       a random block passes about 81% of the time, so the Hamming flags carry no weight on
       their own, and it is the CRC below that has to do the work. */
    int accepted = 0;
    for (uint32_t syndrome = 0; syndrome < 16U; syndrome++) {
        uint8_t rx[12];
        uint8_t decoded[8];
        DSD_MEMCPY(rx, DPMR_REF_HAMMING_VECTORS[0].codeword, sizeof(rx));
        for (uint32_t bit = 0; bit < 4U; bit++) {
            if ((syndrome >> (3U - bit)) & 1U) {
                rx[8U + bit] ^= 1U;
            }
        }
        const bool correctable = Hamming_12_8_decode(rx, decoded, 1);

        int want_uncorrectable = 0;
        for (int u = 0; u < 3; u++) {
            if (DPMR_REF_HAMMING_UNCORRECTABLE_SYNDROMES[u] == syndrome) {
                want_uncorrectable = 1;
            }
        }
        char tag[64];
        DSD_SNPRINTF(tag, sizeof(tag), "hamming-syndrome-%u", syndrome);
        expect_int(tag, correctable ? 1 : 0, want_uncorrectable ? 0 : 1);
        if (correctable) {
            accepted++;
        }
    }
    expect_int("hamming-accepted-syndromes", accepted, 13);
}

static void
test_crc7_matches_the_reference_vectors(void) {
    for (int v = 0; v < DPMR_REF_CRC_VECTOR_COUNT; v++) {
        const dpmr_ref_crc_vector* vector = &DPMR_REF_CRC_VECTORS[v];
        char tag[64];
        DSD_SNPRINTF(tag, sizeof(tag), "crc7-vector-%d", v);
        expect_int(tag, dpmr_crc7(vector->payload, 41U), vector->crc);

        /* The defining property: the remainder over message||crc is zero. Nothing in the
           tree checked this before, so a wrong seed or a missing final inversion could not
           have been told from a right one. */
        uint8_t augmented[48];
        DSD_MEMCPY(augmented, vector->payload, sizeof(vector->payload));
        for (uint32_t bit = 0; bit < 7U; bit++) {
            augmented[41U + bit] = (uint8_t)((vector->crc >> (6U - bit)) & 1U);
        }
        DSD_SNPRINTF(tag, sizeof(tag), "crc7-append-property-%d", v);
        expect_int(tag, dpmr_crc7(augmented, 48U), 0);

        /* And the bit order the decoder reads the transmitted CRC back in. */
        DSD_SNPRINTF(tag, sizeof(tag), "crc7-extract-%d", v);
        expect_int(tag, dpmr_extract_cch_crc(augmented), vector->crc);
    }
}

/* Decode one CCH half the way dpmr_decode_cch_frames() does, using only the reference
   keystream and the real FEC, so a stage failure is attributable before the loopback. */
static void
decode_cch_half(const uint8_t on_air[72], uint8_t decoded48[48], bool hamming_ok[6]) {
    uint8_t descrambled[72];
    uint8_t deinterleaved[72];
    apply_keystream(on_air, descrambled, 72U);
    dpmr_deinterleave_6x12(descrambled, deinterleaved);
    for (uint32_t block = 0; block < 6U; block++) {
        hamming_ok[block] = Hamming_12_8_decode(&deinterleaved[(size_t)block * 12U], &decoded48[(size_t)block * 8U], 1);
    }
}

static void
test_cch_stages_decode_the_reference_frames(void) {
    for (int f = 0; f < DPMR_REF_FRAME_COUNT; f++) {
        const dpmr_ref_frame* frame = &DPMR_REF_FRAMES[f];
        /* Frame body layout, in dibits: CCH #0 at 0 (36), the first TCH group (144), the
           channel code at 180 (12), CCH #1 at 192 (36), the second TCH group (144). */
        static const size_t k_half_offset[2] = {0U, 192U};

        for (int half = 0; half < 2; half++) {
            uint8_t on_air[72];
            for (uint32_t i = 0; i < 36U; i++) {
                const uint8_t dibit = frame->body_dibits[k_half_offset[half] + i];
                on_air[(size_t)i * 2U] = (uint8_t)((dibit >> 1) & 1U);
                on_air[((size_t)i * 2U) + 1U] = (uint8_t)(dibit & 1U);
            }

            uint8_t decoded48[48];
            bool hamming_ok[6];
            DSD_MEMSET(decoded48, 0, sizeof(decoded48));
            decode_cch_half(on_air, decoded48, hamming_ok);

            char tag[96];
            for (uint32_t block = 0; block < 6U; block++) {
                DSD_SNPRINTF(tag, sizeof(tag), "%s-half%d-hamming-block%u", frame->name, half, block);
                expect_int(tag, hamming_ok[block] ? 1 : 0, 1);
            }
            DSD_SNPRINTF(tag, sizeof(tag), "%s-half%d-decoded-bits", frame->name, half);
            expect_bits(tag, decoded48, frame->cch_decoded[half], 48U);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-half%d-crc-extract", frame->name, half);
            expect_int(tag, dpmr_extract_cch_crc(decoded48), frame->crc[half]);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-half%d-crc-compute", frame->name, half);
            expect_int(tag, dpmr_crc7(decoded48, 41U), frame->crc[half]);
        }
    }
}

/* Run a full frame body through the real decoder. */
static void
run_frame(dsd_opts* opts, dsd_state* state, const uint8_t* body, size_t count) {
    g_dibits = body;
    g_dibit_count = count;
    g_dibit_pos = 0;
    g_mbe_calls = 0;
    processdPMRvoice(opts, state);
}

static void
test_reference_frames_decode_through_process_dpmr_voice(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_POS;

    for (int f = 0; f < DPMR_REF_FRAME_COUNT; f++) {
        const dpmr_ref_frame* frame = &DPMR_REF_FRAMES[f];
        char tag[96];

        run_frame(&opts, &state, frame->body_dibits, sizeof(frame->body_dibits));

        DSD_SNPRINTF(tag, sizeof(tag), "%s-dibits-consumed", frame->name);
        expect_int(tag, (long)g_dibit_pos, 372);

        for (uint32_t i = 0; i < NB_OF_DPMR_VOICE_FRAME_TO_DECODE; i++) {
            /* The check this whole issue turns on. */
            DSD_SNPRINTF(tag, sizeof(tag), "%s-crc-ok-%u", frame->name, i);
            expect_int(tag, (long)state.dPMRVoiceFS2Frame.CCHDataCrcOk[i], 1);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-hamming-ok-%u", frame->name, i);
            expect_int(tag, (long)state.dPMRVoiceFS2Frame.CCHDataHammingOk[i], 1);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-crc-value-%u", frame->name, i);
            expect_int(tag, (long)state.dPMRVoiceFS2Frame.CCHDataCRC[i], frame->crc[i]);
            /* The whole 48-bit block, so the fields nothing else asserts are covered too. */
            DSD_SNPRINTF(tag, sizeof(tag), "%s-cch-bits-%u", frame->name, i);
            expect_bits(tag, state.dPMRVoiceFS2Frame.CCHData[i], frame->cch_decoded[i], 48U);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-frame-number-%u", frame->name, i);
            expect_int(tag, (long)state.dPMRVoiceFS2Frame.FrameNumbering[i], frame->frame_number[i]);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-comm-mode-%u", frame->name, i);
            expect_int(tag, (long)state.dPMRVoiceFS2Frame.CommunicationMode[i], frame->communication_mode[i]);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-version-%u", frame->name, i);
            expect_int(tag, (long)state.dPMRVoiceFS2Frame.Version[i], frame->version[i]);
            DSD_SNPRINTF(tag, sizeof(tag), "%s-comms-format-%u", frame->name, i);
            expect_int(tag, (long)state.dPMRVoiceFS2Frame.CommsFormat[i], frame->comms_format[i]);
        }

        DSD_SNPRINTF(tag, sizeof(tag), "%s-colour-code", frame->name);
        expect_int(tag, (int)state.dPMRVoiceFS2Frame.ColorCode[0], DPMR_REF_COLOUR_CODE);
        DSD_SNPRINTF(tag, sizeof(tag), "%s-voice-frames", frame->name);
        expect_int(tag, (long)g_mbe_calls, 8);
    }

    /* Frame numbers 0/1 published the called party, 2/3 the calling party. */
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect_int("call-present", dsd_call_state_get(&state, 0U, &call) > 0, 1);
    expect_int("called-id", strcmp(call.target_text, DPMR_REF_FRAMES[0].id_string), 0);
    expect_int("calling-id", strcmp(call.source_text, DPMR_REF_FRAMES[1].id_string), 0);
}

static void
test_a_corrected_bit_error_still_passes_and_two_do_not(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint8_t body[372];

    /* One flipped on-air bit inside the first CCH half: Hamming places it, CRC still passes. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    DSD_MEMCPY(body, DPMR_REF_FRAMES[0].body_dibits, sizeof(body));
    body[3] ^= 1U; /* low bit of dibit 3, i.e. on-air bit 7 of CCH half 0 */
    run_frame(&opts, &state, body, sizeof(body));
    expect_int("one-error-crc-ok", (long)state.dPMRVoiceFS2Frame.CCHDataCrcOk[0], 1);
    expect_int("one-error-hamming-ok", (long)state.dPMRVoiceFS2Frame.CCHDataHammingOk[0], 1);
    expect_bits("one-error-cch-bits", state.dPMRVoiceFS2Frame.CCHData[0], DPMR_REF_FRAMES[0].cch_decoded[0], 48U);

    /* The interleave sends the bits of one Hamming block six apart on air, so two flips
       six on-air bits apart land in the same codeword and defeat it -- which is the point
       of interleaving, and why the CRC is what stands behind it. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    DSD_MEMCPY(body, DPMR_REF_FRAMES[0].body_dibits, sizeof(body));
    body[0] ^= 2U; /* on-air bit 0 -> block 0 */
    body[3] ^= 2U; /* on-air bit 6 -> block 0 */
    run_frame(&opts, &state, body, sizeof(body));
    expect_int("two-errors-crc-fails", (long)state.dPMRVoiceFS2Frame.CCHDataCrcOk[0], 0);
    /* The second half was untouched and must be unaffected. */
    expect_int("two-errors-other-half-ok", (long)state.dPMRVoiceFS2Frame.CCHDataCrcOk[1], 1);
}

int
main(void) {
    Hamming_12_8_init();

    test_scrambler_matches_the_reference_keystream();
    test_deinterleave_matches_the_reference_permutation();
    test_hamming_decodes_the_reference_codewords();
    test_crc7_matches_the_reference_vectors();
    test_cch_stages_decode_the_reference_frames();
    test_reference_frames_decode_through_process_dpmr_voice();
    test_a_corrected_bit_error_still_passes_and_two_do_not();

    if (g_failures == 0) {
        printf("DPMR_REFERENCE_VECTORS: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
