// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA NDB (Normal Downlink Burst) frame processor
 * ETSI EN 300 392-2 §9.4.3 / §9.4.5 / §23.4
 *
 * NDB structure (510 bits = 255 dibits), dibits relative to the training
 * sequence start L (osmo-tetra NDB_BLK* / ETSI EN 300 392-2 §9.4.4.2.5):
 *
 *   Block1 [L-115, L-7) 108d | AACH1 [L-7, L) 7d | NTS [L, L+11) 11d
 *   AACH2 [L+11, L+19) 8d | Block2 [L+19, L+127) 108d
 *
 * NTS1 is one logical channel. Its 432 bits are one scrambled block: SCH/F
 * (deinterleave a=103, rate 2/3, CRC-16) or TCH/FS (speech matrix). NTS2 is
 * two half-slots, each scrambled from a fresh LFSR, and is not TCH/FS.
 * AACH, not a tail field after Block 2, carries the access-assign flags.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_acelp.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_sync.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/telemetry.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * NDB burst constants
 * ------------------------------------------------------------------------- */

/* Number of dibits in one NDB block */
#define TETRA_NDB_BLOCK_DIBITS 108
/* Number of encoded bits in one NDB block */
#define TETRA_NDB_BLOCK_BITS   216

/* Block permutation interleaver parameter for a 216-bit NDB/SB2 block.
 * ETSI EN 300 392-2 §8.2.4.1 / osmo-tetra tetra_blk_param[TPSAP_T_NDB]:
 *   K = 216 bits, a (interleave_a) = 101.
 * gcd(216, 101) = 1 ✓ (valid permutation).
 * The cols macro is kept for reference only; the ETSI formula π(i)=1+(a*i%K)
 * does not use a column count. */
#define TETRA_BLOCK_ROWS       101
#define TETRA_BLOCK_COLS       3 /* ceil(216/101) = 3, informational only */

/* Neutral soft cost used for punctured positions inserted by depuncturing.
 * 0x7FFF makes either candidate bit equally likely. Hard-only captured dibits
 * are mapped to confident 0x0000/0xFFFF costs below. */
#define SOFT_NEUTRAL           ((uint16_t)0x7FFFu)
#define TETRA_CRC_OK           0x1D0Fu

/* SCH/F: one 432-bit logical channel, interleaver a=103, rate 2/3.
 * gcd(432, 103) = 1. Type-2 is 268 info + 16 CRC + 4 tail. */
#define TETRA_SCHF_BITS        432
#define TETRA_SCHF_A           103
#define TETRA_SCHF_TYPE1       268
#define TETRA_SCHF_TYPE2       288
#define TETRA_SCHF_DEPUNC      (TETRA_SCHF_TYPE2 * 4)

static uint32_t
tetra_lfsr_seed_for(const dsd_state* state, int cc) {
    if (state->tetra_net_known) {
        return state->tetra_lfsr_seed;
    }
    return tetra_compute_scramb_seed(0u, 0u, (uint8_t)(cc & 0x3u));
}

/* -------------------------------------------------------------------------
 * tetra_prepare_block()
 *
 * Expand 108 dibits into 216 soft costs. Descrambling is optional: a
 * half-slot (SCH/HD) restarts the LFSR, but NTS1 is one 432-bit scrambled
 * block and must be descrambled only after the two halves are joined.
 * ------------------------------------------------------------------------- */
static void
tetra_prepare_block_seed(const uint8_t* dibuf, const float* soft_in, uint32_t seed, dsd_state* state,
                         uint16_t* out_soft, int descramble) {
    uint8_t hard_bits[TETRA_NDB_BLOCK_BITS];
    uint16_t soft_costs[TETRA_NDB_BLOCK_BITS];

    /* A non-zero Costas phase still correlates the training word, but the
     * soft amplitude was sliced in that rotated phase. Hard dibits are
     * derotated below; the soft costs would vote for the wrong bits. */
    const int phase_rot = state->tetra_phase_rot & 3;
    if (phase_rot != 0) {
        soft_in = NULL;
    }

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++) {
        /* An inverted discriminator swaps the positive and negative symbol
         * pairs, which is exactly dibit XOR 2: the MSB changes and the LSB
         * does not. Frame sync records that polarity; normalize it before
         * either hard- or soft-decision FEC sees the block. */
        const uint8_t d =
            (uint8_t)((tetra_dibit_derotate(dibuf[i], phase_rot) ^ (state->tetra_polarity ? 2u : 0u)) & 3u);
        hard_bits[i * 2 + 0] = (d >> 1) & 1u;
        hard_bits[i * 2 + 1] = d & 1u;

        if (soft_in) {
            soft_costs[i * 2 + 0] = soft_symbol_to_viterbi_cost(soft_in[i], state, 0);
            soft_costs[i * 2 + 1] = soft_symbol_to_viterbi_cost(soft_in[i], state, 1);
            if (state->tetra_polarity) {
                soft_costs[i * 2 + 0] = (uint16_t)(0xFFFFu - soft_costs[i * 2 + 0]);
            }
        } else {
            soft_costs[i * 2 + 0] = hard_bits[i * 2 + 0] ? 0xFFFFu : 0x0000u;
            soft_costs[i * 2 + 1] = hard_bits[i * 2 + 1] ? 0xFFFFu : 0x0000u;
        }
    }

    if (descramble) {
        tetra_descramble_soft(soft_costs, TETRA_NDB_BLOCK_BITS, seed);
    }

    memcpy(out_soft, soft_costs, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);
}

static void
tetra_prepare_block(const uint8_t* dibuf, const float* soft_in, int cc, dsd_state* state, uint16_t* out_soft,
                    int descramble) {
    tetra_prepare_block_seed(dibuf, soft_in, tetra_lfsr_seed_for(state, cc), state, out_soft, descramble);
}

/* -------------------------------------------------------------------------
 * tetra_decode_schd()
 *
 * SCH-HD path: depuncture a single 216-bit block with rate 2/3,
 * Viterbi decode, and dispatch to MAC parser.
 * ------------------------------------------------------------------------- */
/* Shared SCH/HD (and STCH) FEC: one descrambled 216-bit block -> 124 type-1
 * bits. Returns 1 when the CRC-16 passes; `decoded` needs 256 bytes. */
static int
tetra_hd_decode_bits(const uint16_t* soft, uint8_t* decoded) {
    const int type1_len = 124;
    const int type2_len = 144; /* 124 payload + 16 CRC + 4 zero tail */
    const int depunc_len = type2_len * 4;

    uint16_t* depunc = (uint16_t*)malloc(sizeof(uint16_t) * (size_t)depunc_len);
    if (!depunc) {
        return 0;
    }
    uint16_t deint_soft[TETRA_NDB_BLOCK_BITS];
    tetra_block_deinterleave_soft(soft, deint_soft, TETRA_NDB_BLOCK_BITS, TETRA_BLOCK_ROWS, TETRA_BLOCK_COLS);
    tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint_soft, TETRA_NDB_BLOCK_BITS, depunc, depunc_len);

    memset(decoded, 0, 256);
    int dec_len = tetra_viterbi_decode_soft(depunc, depunc_len, decoded, 256);
    free(depunc);

    return dec_len >= type2_len && tetra_crc16_ccitt_bits(decoded, type1_len + 16) == 0x1D0Fu;
}

static int
tetra_decode_schd(const uint16_t* soft, int cc, int block_idx, dsd_opts* opts, dsd_state* state) {
    const int type1_len = 124;
    uint8_t decoded[256];
    (void)block_idx;

    const int crc_ok = tetra_hd_decode_bits(soft, decoded);
    if (crc_ok) {
        state->tetra_decode_ok++;
    } else {
        state->tetra_decode_errors++;
    }

    if (crc_ok) {
        tetra_mac_parse_schd(decoded, type1_len, cc, opts, state);
    }
    return crc_ok;
}

static int tetra_decode_tch_half(const uint16_t* soft_block, dsd_opts* opts, dsd_state* state);

/* -------------------------------------------------------------------------
 * TCH/FS combined decode constants
 *
 * TETRA TCH/FS uses both NDB blocks as one codeword:
 *   432 coded bits → speech matrix deinterleave → class-specific RCPC
 *   → 274 type-2 bits → class reorder → 2 × 137-bit ACELP codec frames
 *
 * ETSI EN 300 392-2 §8.3 Table 8.7 / EN 300 395-2 §6
 * ------------------------------------------------------------------------- */
/* Max received-vs-re-encoded channel-bit disagreements (of 330) for a DMO
 * speech burst to be passed to the vocoder. Valid bursts measured 0..16 on
 * real air; wrong-scrambler/noise bursts 36..44. */
#define TETRA_DMO_TCH_MAX_MISMATCH 28

#define TETRA_TCH_FS_CODED_BITS    432 /* 2 × 216 = combined blocks       */
#define TETRA_TCH_FS_TYPE2_BITS    274 /* two complete 137-bit frames */

/* -------------------------------------------------------------------------
 * tetra_decode_tch_fs()
 *
 * TCH/FS path: combine two prepared 216-bit blocks, speech-deinterleave,
 * decode the class-specific channel code, then ACELP reorder and vocoder.
 *
 * @soft_b1 – first 216 descrambled type-4 soft costs
 * @soft_b2 – next 216; the LFSR must already have run across both halves
 * @cc      – Colour Code
 * @opts    – dsd_opts
 * @state   – dsd_state
 * ------------------------------------------------------------------------- */
static void
tetra_decode_tch_fs(const uint16_t* soft_b1, const uint16_t* soft_b2, int cc, dsd_opts* opts, dsd_state* state) {
    /* A weak CB can otherwise feed unrelated/incorrect colour-code bursts
     * into the same stateful speech decoder.  Keep this field filter opt-in;
     * networks with no fixed observed CB colour code retain old behaviour. */
    const char* cc_filter = getenv("TETRA_TCH_CC_FILTER");
    if (cc_filter && cc_filter[0] >= '0' && cc_filter[0] <= '3' && cc_filter[1] == '\0' && cc != cc_filter[0] - '0') {
        return;
    }

    /* Step 1: Concatenate Block 1 + Block 2 → 432 soft costs. */
    uint16_t combined[TETRA_TCH_FS_CODED_BITS];
    uint16_t deint[TETRA_TCH_FS_CODED_BITS];
    memcpy(combined, soft_b1, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);
    memcpy(combined + TETRA_NDB_BLOCK_BITS, soft_b2, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);

    tetra_speech_deinterleave_soft(combined, deint);

    /* Step 2: Decode the speech-specific class-0 / rate-1/3 RCPC layout. */
    uint8_t decoded[TETRA_TCH_FS_TYPE2_BITS];
    memset(decoded, 0, sizeof(decoded));
    int mismatches = -1;
    int dec_len = tetra_speech_channel_decode_q(deint, decoded, (int)sizeof(decoded), &mismatches);

    /* DMO: the speech code has no CRC, but with the right DM colour code a
     * burst re-encodes to within a few bits of what was received, while a
     * wrong-seed or noise burst disagrees in ~40 of 330 bits. Drop those
     * instead of feeding noise to the vocoder. TMO keeps its old behaviour. */
    if ((state->tetra_dmo || state->tetra_uplink) && mismatches > TETRA_DMO_TCH_MAX_MISMATCH) {
        state->tetra_decode_errors++;
        if (opts->errorbars) {
            fprintf(stderr, "[TETRA TCH/S] dropped: %d channel-bit mismatches\n", mismatches);
        }
        return;
    }

    if (dec_len > 0) {
        state->tetra_decode_ok++;
    } else {
        state->tetra_decode_errors++;
    }

    /* Step 3: ACELP reorder + vocoder. */
    if (dec_len >= TETRA_TCH_FS_TYPE2_BITS) {
        const char* bit_dump = getenv("TETRA_TCH_BIT_DUMP");
        if (bit_dump && bit_dump[0]) {
            static FILE* dump_fp;
            if (!dump_fp) {
                dump_fp = fopen(bit_dump, "w");
            }
            if (dump_fp) {
                int weak = 0;
                for (int i = 0; i < 102; i++) {
                    uint16_t c = deint[i];
                    uint16_t dist = c > 0x7fffu ? (uint16_t)(c - 0x7fffu) : (uint16_t)(0x7fffu - c);
                    if (dist < 0x1000u) {
                        weak++;
                    }
                }
                fprintf(dump_fp, "cc=%d tn=%u pol=%d nts2=%d weak=%d ", cc, state ? (unsigned)state->tetra_tn : 0u,
                        state ? (int)state->tetra_polarity : -1, state ? (int)state->tetra_nts2 : -1, weak);
                for (int i = 0; i < TETRA_TCH_FS_TYPE2_BITS; i++) {
                    fputc('0' + (decoded[i] & 1), dump_fp);
                }
                fputc('\n', dump_fp);
                fflush(dump_fp);
            }
        }
        tetra_air_decrypt_voice(state, decoded, dec_len, 0);
        tetra_acelp_process_tch(decoded, dec_len, 0, opts, state);
    }

    if (opts->payload || opts->errorbars) {
        fprintf(stderr, "[TETRA TCH/FS] CC=%d  dec_bits=%d  first16=", cc, dec_len);
        for (int i = 0; i < 16 && i < dec_len; i++) {
            fprintf(stderr, "%d", decoded[i] & 1);
        }
        fprintf(stderr, "\n");
    }
}

/* Half-slot TCH/S (EN 300 395-2 5.6): one speech frame in a 216-bit block while
 * the other half of the slot carried STCH. Returns 1 when a frame was passed
 * to the vocoder. The 4-bit class-2 CRC alone passes 1 in 16 random blocks, so
 * it is combined with the re-encode mismatch count (of 165 coded bits; random
 * blocks average ~23). */
#define TETRA_DMO_HALF_MAX_MISMATCH 16

static int
tetra_decode_tch_half(const uint16_t* soft_block, dsd_opts* opts, dsd_state* state) {
    uint16_t deint[TETRA_NDB_BLOCK_BITS];
    uint8_t bits[137];
    int crc_ok = 0, mismatches = -1;

    tetra_block_deinterleave_soft(soft_block, deint, TETRA_NDB_BLOCK_BITS, TETRA_BLOCK_ROWS, TETRA_BLOCK_COLS);
    if (tetra_speech_half_decode(deint, bits, &crc_ok, &mismatches) != 137) {
        return 0;
    }

    if (!crc_ok || mismatches > TETRA_DMO_HALF_MAX_MISMATCH) {
        state->tetra_decode_errors++;
        return 0;
    }
    state->tetra_decode_ok++;
    snprintf(state->fsubtype, sizeof(state->fsubtype), " STCH+TCH     ");
    if (opts->payload || opts->errorbars) {
        fprintf(stderr, "[TETRA TCH/H] crc=%d mismatches=%d\n", crc_ok, mismatches);
    }
    tetra_air_decrypt_voice(state, bits, 137, 137);
    tetra_acelp_process_tch_half(bits, 137, opts, state);
    return 1;
}

/* SCH/F uses the same 432 descrambled bits as TCH/FS, then a different
 * interleaver (a=103) and rate-2/3 code. A matching CRC-16 means this NTS1
 * burst is signalling: dispatch its MAC payload instead of discarding it. */
static int
tetra_schf_decode_bits(const uint16_t* type4, uint8_t* decoded) {
    uint16_t deint[TETRA_SCHF_BITS];
    uint16_t* depunc = (uint16_t*)malloc(sizeof(uint16_t) * TETRA_SCHF_DEPUNC);
    if (!depunc) {
        return 0;
    }

    tetra_block_deinterleave_soft(type4, deint, TETRA_SCHF_BITS, TETRA_SCHF_A, 0);
    tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint, TETRA_SCHF_BITS, depunc, TETRA_SCHF_DEPUNC);
    memset(decoded, 0, TETRA_SCHF_TYPE2);
    int dec_len = tetra_viterbi_decode_soft(depunc, TETRA_SCHF_DEPUNC, decoded, TETRA_SCHF_TYPE2);
    free(depunc);
    if (dec_len < TETRA_SCHF_TYPE2) {
        return 0;
    }
    if (tetra_crc16_ccitt_bits(decoded, TETRA_SCHF_TYPE1 + 16) != TETRA_CRC_OK) {
        return 0;
    }
    return 1;
}

static int
tetra_decode_schf(const uint16_t* type4, int cc, dsd_opts* opts, dsd_state* state) {
    uint8_t decoded[TETRA_SCHF_TYPE2];
    if (!tetra_schf_decode_bits(type4, decoded)) {
        return 0;
    }
    tetra_mac_parse_schd(decoded, TETRA_SCHF_TYPE1, cc, opts, state);
    return 1;
}

/* -------------------------------------------------------------------------
 * processTetraFrame()
 *
 * Called by the engine after NDB NTS sync is confirmed.
 * Phase 4: decode Block 1 (from scan-window capture) then Block 2 (live).
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * DMO normal burst (DNB), EN 300 396-2 clause 9.4.3.2.1.
 *
 * No AACH: block 1 is the 108 dibits right before the training sequence (the
 * frame-sync capture already picked those up for DMO) and block 2 starts
 * right after it. NTS1 (preamble P1) carries TCH or SCH/F in one 432-bit
 * block; NTS2 (P2) carries STCH + STCH or STCH + TCH in two 216-bit halves.
 * Everything except the DSB is scrambled with the DM colour code that
 * tetra_dmo_apply_sync() derived from the call's DMAC-SYNC.
 * ------------------------------------------------------------------------- */
#define TETRA_DMO_DCC_STALE_BURSTS 200u

static void
tetra_dmo_process_ndb(dsd_opts* opts, dsd_state* state) {
    uint8_t b2_dibuf[TETRA_NDB_BLOCK_DIBITS];
    float b2_soft_in[TETRA_NDB_BLOCK_DIBITS];
    const int b1_valid = state->tetra_b1_valid;

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++) {
        b2_dibuf[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &b2_soft_in[i]);
    }

    snprintf(state->fsubtype, sizeof(state->fsubtype), state->tetra_nts2 ? " DNB NTS2     " : " DNB NTS1     ");
    snprintf(state->ftype, sizeof(state->ftype), " TETRA DMO");

    if (state->tetra_dmo_since_sync < 0xFFFFu) {
        state->tetra_dmo_since_sync++;
    }
    if (state->tetra_dmo_dcc_valid && state->tetra_dmo_since_sync > TETRA_DMO_DCC_STALE_BURSTS) {
        state->tetra_dmo_dcc_valid = 0;
    }

    if (opts->errorbars) {
        fprintf(stderr, " [TETRA DNB  NTS%d  pol=%d rot=%d dcc=%d]", state->tetra_nts2 ? 2 : 1,
                (int)state->tetra_polarity, (int)(state->tetra_phase_rot & 3), (int)state->tetra_dmo_dcc_valid);
    }

    if (state->tetra_dmo_dcc_valid && b1_valid) {
        const uint32_t seed = state->tetra_dmo_dcc_seed;
        uint16_t b1_soft[TETRA_NDB_BLOCK_BITS];
        uint16_t b2_soft[TETRA_NDB_BLOCK_BITS];
        uint8_t decoded[TETRA_SCHF_TYPE2];
        const float* b1_soft_in = state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL;

        if (state->tetra_nts2) {
            /* Each half-slot restarts the scrambler. */
            tetra_prepare_block_seed(state->tetra_b1_dibuf, b1_soft_in, seed, state, b1_soft, 1);
            if (tetra_hd_decode_bits(b1_soft, decoded)) {
                state->tetra_decode_ok++;
                tetra_dmo_log_dnb_pdu(decoded, 124, "STCH-1", state);
            } else {
                state->tetra_decode_errors++;
            }
            tetra_prepare_block_seed(b2_dibuf, b2_soft_in, seed, state, b2_soft, 1);
            if (tetra_hd_decode_bits(b2_soft, decoded)) {
                state->tetra_decode_ok++;
                tetra_dmo_log_dnb_pdu(decoded, 124, "STCH-2", state);
            } else {
                /* Not STCH: the other half of a stolen slot is half-slot TCH/S. */
                (void)tetra_decode_tch_half(b2_soft, opts, state);
            }
        } else {
            uint16_t type4[TETRA_SCHF_BITS];
            tetra_prepare_block_seed(state->tetra_b1_dibuf, b1_soft_in, seed, state, b1_soft, 0);
            tetra_prepare_block_seed(b2_dibuf, b2_soft_in, seed, state, b2_soft, 0);
            memcpy(type4, b1_soft, sizeof(b1_soft));
            memcpy(type4 + TETRA_NDB_BLOCK_BITS, b2_soft, sizeof(b2_soft));
            /* One LFSR run covers block 1 and block 2 (EN 300 396-2 8.3.1.3, 8.3.2.4). */
            tetra_descramble_soft(type4, TETRA_SCHF_BITS, seed);

            if (tetra_schf_decode_bits(type4, decoded)) {
                state->tetra_decode_ok++;
                snprintf(state->fsubtype, sizeof(state->fsubtype), " DNB SCH/F     ");
                tetra_dmo_log_dnb_pdu(decoded, TETRA_SCHF_TYPE1, "SCH/F", state);
            } else {
                tetra_decode_tch_fs(type4, type4 + TETRA_NDB_BLOCK_BITS, 0, opts, state);
            }
        }
    }

    state->tetra_b1_valid = 0;
    state->tetra_b1_soft_valid = 0;

    tetra_sm_tick(opts, state);
    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}

/* Expand `count` dibits with the same phase and polarity correction as a
 * downlink block. Descrambling is left to the caller. */
static void
tetra_expand_dibits(const uint8_t* dibuf, const float* soft_in, int count, dsd_state* state, uint16_t* out_soft) {
    const int phase_rot = state->tetra_phase_rot & 3;
    if (phase_rot != 0) {
        soft_in = NULL;
    }
    for (int i = 0; i < count; i++) {
        const uint8_t d =
            (uint8_t)((tetra_dibit_derotate(dibuf[i], phase_rot) ^ (state->tetra_polarity ? 2u : 0u)) & 3u);
        const uint8_t b0 = (d >> 1) & 1u;
        const uint8_t b1 = d & 1u;
        if (soft_in) {
            out_soft[i * 2 + 0] = soft_symbol_to_viterbi_cost(soft_in[i], state, 0);
            out_soft[i * 2 + 1] = soft_symbol_to_viterbi_cost(soft_in[i], state, 1);
            if (state->tetra_polarity) {
                out_soft[i * 2 + 0] = (uint16_t)(0xFFFFu - out_soft[i * 2 + 0]);
            }
        } else {
            out_soft[i * 2 + 0] = b0 ? 0xFFFFu : 0x0000u;
            out_soft[i * 2 + 1] = b1 ? 0xFFFFu : 0x0000u;
        }
    }
}

/* Control uplink burst: 42 dibits, 15-dibit extended training, 42 dibits.
 * The 168 bits are one SCH/HU codeword (EN 300 392-2 8.3.1.4.3, table 9.3). */
static void
tetra_uplink_process_cb(dsd_opts* opts, dsd_state* state) {
    enum { HALF = 42, BITS = 168 };

    uint8_t b2[HALF];
    float b2_soft_in[HALF];
    uint16_t type4[BITS];
    uint8_t mac[92];
    static int warned = 0;

    for (int i = 0; i < HALF; i++) {
        b2[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &b2_soft_in[i]);
    }

    snprintf(state->ftype, sizeof(state->ftype), " TETRA UL");
    snprintf(state->fsubtype, sizeof(state->fsubtype), " CB SCH/HU    ");
    if (!state->tetra_net_known) {
        if (!warned) {
            fprintf(stderr, "[TETRA UL] no colour code yet; set TETRA_NET=mcc,mnc,colour\n");
            warned = 1;
        }
        return;
    }
    if (!state->tetra_b1_valid) {
        return;
    }

    tetra_expand_dibits(state->tetra_b1_dibuf, state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL, HALF, state,
                        type4);
    tetra_expand_dibits(b2, b2_soft_in, HALF, state, type4 + HALF * 2);
    tetra_descramble_soft(type4, BITS, state->tetra_lfsr_seed);
    state->tetra_b1_valid = 0;
    state->tetra_b1_soft_valid = 0;

    if (!tetra_sch_hu_decode(type4, mac)) {
        state->tetra_decode_errors++;
        return;
    }
    state->tetra_decode_ok++;
    if (opts->payload || opts->errorbars) {
        if ((mac[0] & 1u) == 0) {
            const unsigned addr_type = (unsigned)tetra_bits_to_uint(mac, 3, 2);
            const int addr_len = addr_type == 1u ? 10 : 24;
            fprintf(stderr, "[TETRA SCH/HU] MAC-ACCESS enc=%u addr=%u\n", mac[2] & 1u,
                    tetra_bits_to_uint(mac, 5, addr_len));
        } else {
            fprintf(stderr, "[TETRA SCH/HU] MAC-END-HU\n");
        }
    }
}

/* Normal uplink burst (table 9.5). Same logical channels as a downlink NDB,
 * scrambled with the same colour code, but with no AACH between the blocks. */
static void
tetra_uplink_process_nub(dsd_opts* opts, dsd_state* state) {
    uint8_t b2_dibuf[TETRA_NDB_BLOCK_DIBITS];
    float b2_soft_in[TETRA_NDB_BLOCK_DIBITS];
    const int b1_valid = state->tetra_b1_valid;
    const int cc = 0;
    static int warned = 0;

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++) {
        b2_dibuf[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &b2_soft_in[i]);
    }

    snprintf(state->ftype, sizeof(state->ftype), " TETRA UL");
    snprintf(state->fsubtype, sizeof(state->fsubtype), state->tetra_nts2 ? " NUB NTS2     " : " NUB NTS1     ");
    if (opts->errorbars) {
        fprintf(stderr, " [TETRA NUB  NTS%d  pol=%d rot=%d]", state->tetra_nts2 ? 2 : 1, (int)state->tetra_polarity,
                (int)(state->tetra_phase_rot & 3));
    }

    if (!state->tetra_net_known) {
        if (!warned) {
            fprintf(stderr, "[TETRA UL] no colour code yet; set TETRA_NET=mcc,mnc,colour\n");
            warned = 1;
        }
    } else if (state->tetra_nts2) {
        uint16_t block_soft[TETRA_NDB_BLOCK_BITS];
        if (b1_valid) {
            tetra_prepare_block(state->tetra_b1_dibuf, state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL, cc,
                                state, block_soft, 1);
            tetra_decode_schd(block_soft, cc, 1, opts, state);
        }
        tetra_prepare_block(b2_dibuf, b2_soft_in, cc, state, block_soft, 1);
        if (!tetra_decode_schd(block_soft, cc, 2, opts, state)) {
            (void)tetra_decode_tch_half(block_soft, opts, state);
        }
    } else if (b1_valid) {
        uint16_t b1_soft[TETRA_NDB_BLOCK_BITS];
        uint16_t b2_soft[TETRA_NDB_BLOCK_BITS];
        uint16_t type4[TETRA_SCHF_BITS];
        uint8_t decoded[TETRA_SCHF_TYPE2];

        tetra_prepare_block(state->tetra_b1_dibuf, state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL, cc, state,
                            b1_soft, 0);
        tetra_prepare_block(b2_dibuf, b2_soft_in, cc, state, b2_soft, 0);
        memcpy(type4, b1_soft, sizeof(b1_soft));
        memcpy(type4 + TETRA_NDB_BLOCK_BITS, b2_soft, sizeof(b2_soft));
        tetra_descramble_soft(type4, TETRA_SCHF_BITS, tetra_lfsr_seed_for(state, cc));

        if (tetra_schf_decode_bits(type4, decoded)) {
            state->tetra_decode_ok++;
            snprintf(state->fsubtype, sizeof(state->fsubtype), " NUB SCH/F     ");
            tetra_mac_parse_schd(decoded, TETRA_SCHF_TYPE1, cc, opts, state);
        } else {
            tetra_decode_tch_fs(type4, type4 + TETRA_NDB_BLOCK_BITS, cc, opts, state);
        }
    }

    state->tetra_b1_valid = 0;
    state->tetra_b1_soft_valid = 0;
    tetra_sm_tick(opts, state);
    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}

void
processTetraFrame(dsd_opts* opts, dsd_state* state) {
    soft_symbol_frame_begin(state);
    tetra_uplink_configure(state);
    if (state->tetra_cb) {
        state->tetra_cb = 0;
        tetra_uplink_process_cb(opts, state);
        return;
    }
    if (state->tetra_uplink && !state->tetra_dmo) {
        tetra_uplink_process_nub(opts, state);
        return;
    }
    if (state->tetra_dmo) {
        tetra_dmo_process_ndb(opts, state);
        return;
    }
    tetra_tdma_advance_ndb(state);

    /* AACH half 2 sits between the training sequence and Block 2. */
    skipDibit(opts, state, 8);

    /* ---------------------------------------------------------------
     * Read Block 2 live (108 dibits = 216 bits).
     * --------------------------------------------------------------- */
    uint8_t b2_dibuf[TETRA_NDB_BLOCK_DIBITS];
    float b2_soft_in[TETRA_NDB_BLOCK_DIBITS];

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++) {
        b2_dibuf[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &b2_soft_in[i]);
    }

    /* Colour code and stealing flags live in the Reed-Muller AACH, which is
     * not decoded yet. NTS1 is the single-channel burst, so try TCH/FS there.
     * The network colour from BSCH is not the 2-bit burst colour; pass 0 and
     * let the scrambler use tetra_lfsr_seed once the network is known. */
    const int cc = 0;

    snprintf(state->fsubtype, sizeof(state->fsubtype), state->tetra_nts2 ? " NTS2         " : " NTS1         ");
    snprintf(state->ftype, sizeof(state->ftype), " TETRA");

    if (opts->errorbars) {
        fprintf(stderr, " [TETRA NDB  NTS%d  pol=%d rot=%d]", state->tetra_nts2 ? 2 : 1, (int)state->tetra_polarity,
                (int)(state->tetra_phase_rot & 3));
    }

    /* NTS2 carries two independently scrambled half-slot channels. Try the
     * SCH/HD CRC on each half before dispatching its MAC payload. */
    int b1_valid = state->tetra_b1_valid;
    if (state->tetra_nts2) {
        uint16_t block_soft[TETRA_NDB_BLOCK_BITS];
        if (b1_valid) {
            tetra_prepare_block(state->tetra_b1_dibuf, state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL, cc,
                                state, block_soft, 1);
            tetra_decode_schd(block_soft, cc, 1, opts, state);
        }
        tetra_prepare_block(b2_dibuf, b2_soft_in, cc, state, block_soft, 1);
        if (!tetra_decode_schd(block_soft, cc, 2, opts, state)) {
            /* Not signalling: the second half of a stolen slot can carry one
             * half-slot speech frame (EN 300 395-2 5.6). */
            (void)tetra_decode_tch_half(block_soft, opts, state);
        }
    }

    /* NTS1 carries one 432-bit logical channel. Scrambling runs across
     * Block1||Block2; restarting the LFSR on Block 2 descrambles the second
     * half with the wrong keystream. SCH/F and TCH/FS share those bits and
     * are separated by the SCH/F CRC. NTS2 is two half-slots, not TCH/FS. */
    if (!state->tetra_nts2 && b1_valid) {
        uint16_t b1_soft[TETRA_NDB_BLOCK_BITS];
        uint16_t b2_soft[TETRA_NDB_BLOCK_BITS];
        uint16_t type4[TETRA_SCHF_BITS];

        tetra_prepare_block(state->tetra_b1_dibuf, state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL, cc, state,
                            b1_soft, 0);
        tetra_prepare_block(b2_dibuf, b2_soft_in, cc, state, b2_soft, 0);
        memcpy(type4, b1_soft, sizeof(b1_soft));
        memcpy(type4 + TETRA_NDB_BLOCK_BITS, b2_soft, sizeof(b2_soft));
        tetra_descramble_soft(type4, TETRA_SCHF_BITS, tetra_lfsr_seed_for(state, cc));

        if (tetra_decode_schf(type4, cc, opts, state)) {
            state->tetra_decode_ok++;
            snprintf(state->fsubtype, sizeof(state->fsubtype), " SCH/F         ");
            if (opts->payload || opts->errorbars) {
                fprintf(stderr, "[TETRA SCH/F] tn=%u CRC OK\n", (unsigned)state->tetra_tn);
            }
        } else {
            tetra_decode_tch_fs(type4, type4 + TETRA_NDB_BLOCK_BITS, cc, opts, state);
        }
    }
    state->tetra_b1_valid = 0;
    state->tetra_b1_soft_valid = 0;

    /* ---------------------------------------------------------------
     * Phase 8: event watchdog + ncurses UI refresh (same as DMR/D-STAR).
     * --------------------------------------------------------------- */
    tetra_sm_tick(opts, state);
    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}

/* -------------------------------------------------------------------------
 * processTetraSBFrame() — Phase 41
 *
 * Called by the engine when a TETRA Synchronisation Burst (SB) SSB is
 * detected.  At SSB detection time, dsd_frame_sync.c has already captured
 * the 60 BSCH dibits (SB Block 1) into state->tetra_sb1_dibuf[].
 *
 * SB burst layout (ETSI EN 300 392-2 §9.4.4):
 *   [tail][FC(40d)][BSCH(60d)][SSB(19d)] ← detection point
 *   [BB(15d)][BKN2(108d)][tail]           ← consumed here
 *
 * BSCH FEC pipeline (hard-input only):
 *   60 dibits → 120 bits → descramble(seed=3) → deinterleave(K=120,a=11)
 *   → RCPC depuncture(2/3, 120→320) → Viterbi(out=80b)
 *   → CRC-16 gate → tetra_bsch_parse(first 60b)
 * -------------------------------------------------------------------------*/

/* SB constants */
#define TETRA_SB_BSCH_DIBITS 60  /* dibits in SB1 block (BSCH) */
#define TETRA_SB_BSCH_BITS   120 /* physical bits after dibit expansion */
#define TETRA_SB_BB_DIBITS   15  /* broadcast bits (AACH) after SSB */
#define TETRA_SB_BKN2_DIBITS 108 /* BKN2 block (SB2, BNCH) after BB */
#define TETRA_SB_BSCH_ROWS   11  /* interleaver parameter a for K=120 */
#define TETRA_SB_SCRAMB_SEED 3u  /* SCRAMB_INIT = tetra_compute_scramb_seed(0,0,0) */
#define TETRA_SB_TYPE2_BITS  80
#define TETRA_SB_TYPE1_BITS  60
#define TETRA_SB_DEPUNC_LEN  (TETRA_SB_TYPE2_BITS * 4)

/* DMO DSB: the SCH/S block was already decoded (CRC ok, DMO system code).
 * Block 2 (SCH/H) follows the SSB immediately - there is no 15-dibit broadcast
 * block as in a TMO SB. It is scrambled with an all-zero colour code. The
 * same 123 dibits as the TMO path are consumed so burst timing is unchanged. */
static void
tetra_dmo_process_dsb(dsd_opts* opts, dsd_state* state, const uint8_t* sch_s) {
    uint8_t b2_dibuf[TETRA_SB_BKN2_DIBITS];
    float b2_soft_in[TETRA_SB_BKN2_DIBITS];
    uint16_t b2_soft[TETRA_NDB_BLOCK_BITS];
    uint8_t sch_h[256];
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;

    for (int i = 0; i < TETRA_SB_BKN2_DIBITS; i++) {
        b2_dibuf[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &b2_soft_in[i]);
    }
    skipDibit(opts, state, TETRA_SB_BB_DIBITS);

    tetra_dmo_parse_sync_s(sch_s, &s);

    tetra_prepare_block_seed(b2_dibuf, b2_soft_in, TETRA_SB_SCRAMB_SEED, state, b2_soft, 1);
    const int h_ok = tetra_hd_decode_bits(b2_soft, sch_h);
    if (h_ok) {
        state->tetra_decode_ok++;
    } else {
        state->tetra_decode_errors++;
    }

    if (s.sync_pdu_type == 1) {
        /* DM-REP presence signal. Its SCH/H is not a DMAC-SYNC, so it must not
         * be allowed to invent a call colour code. */
        tetra_dmo_pres_t pres;
        char desc[120];
        if (tetra_dmo_parse_presence(sch_s, h_ok ? sch_h : NULL, &s, &pres)) {
            tetra_dmo_apply_sync(state, &s, NULL);
            tetra_dmo_describe_presence(&pres, desc, (int)sizeof(desc));
            tetra_dmo_publish_presence(state, desc);
            fprintf(stderr, "[TETRA DMO PRES] sys=%u slot=%u frame=%u | %s\n", s.system_code, s.slot, s.frame, desc);
        } else {
            state->tetra_dmo = 1;
            fprintf(stderr, "[TETRA DMO PRES] sys=%u comm=%u | presence layout not parsed\n", s.system_code,
                    s.comm_type);
        }
    } else {
        if (h_ok) {
            tetra_dmo_parse_sync_h(sch_h, s.comm_type, &h);
        }
        tetra_dmo_apply_sync(state, &s, h_ok ? &h : NULL);

        fprintf(stderr, "[TETRA DMO DSB] sys=%u comm=%u ch=%u slot=%u frame=%u enc=%u", s.system_code, s.comm_type,
                s.ab_usage, s.slot, s.frame, s.enc_state);
        if (h_ok && h.has_relay) {
            fprintf(stderr, " rep=%u %s", h.relay_addr, s.ms_link_flag ? "master" : "slave");
        }
        if (h_ok && h.has_addrs) {
            char desc[160];
            tetra_dmo_describe_elems(&h, desc, (int)sizeof(desc));
            tetra_dmo_publish(state, h.msg_type, h.src, h.dst, desc);
            fprintf(stderr, " | %s src=%u dst=%u MNI=%u/%u cd=%u%s%s%s", tetra_dmo_msg_name(h.msg_type), h.src, h.dst,
                    (h.mni >> 14) & 0x3FFu, h.mni & 0x3FFFu, h.frame_countdown,
                    state->tetra_dmo_dcc_valid ? " dcc=ok" : "", desc[0] ? " | " : "", desc);
        } else {
            fprintf(stderr, " | SCH/H %s", h_ok ? "ok" : "CRC fail");
        }
        fprintf(stderr, "\n");
    }

    snprintf(state->fsubtype, sizeof(state->fsubtype), " DSB           ");
    snprintf(state->ftype, sizeof(state->ftype), " TETRA DMO");

    tetra_sm_tick(opts, state);
    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}

static void
tetra_sb_skip_tail(dsd_opts* opts, dsd_state* state) {
    skipDibit(opts, state, TETRA_SB_BB_DIBITS);   /* BB / AACH (15 dibits) */
    skipDibit(opts, state, TETRA_SB_BKN2_DIBITS); /* BKN2 block (108 dibits) */
}

/* Decode the 60 captured BSCH / SCH-S dibits into 80 type-2 bits.
 *
 * use_soft = 0 forces the hard-decision input, which depends only on the
 * captured dibits and therefore has no dependence on the live symbol-tracking
 * thresholds. Returns the Viterbi length, or -1 if memory is unavailable. */
static int
tetra_sb_decode_block(dsd_state* state, int use_soft, uint8_t* decoded, int decoded_len) {
    /* Expand 60 dibits → 120 bits. Prefer the analog decisions captured from
     * symbol history; retain the hard-decision fallback for symbol replay and
     * callers which do not populate history. */
    uint8_t hard[TETRA_SB_BSCH_BITS];
    uint16_t soft[TETRA_SB_BSCH_BITS];
    const int phase_rot = state->tetra_phase_rot & 3;
    for (int i = 0; i < TETRA_SB_BSCH_DIBITS; i++) {
        uint8_t d =
            (uint8_t)((tetra_dibit_derotate(state->tetra_sb1_dibuf[i], phase_rot) ^ (state->tetra_polarity ? 2u : 0u))
                      & 3u);
        hard[i * 2 + 0] = (d >> 1) & 1u;
        hard[i * 2 + 1] = d & 1u;
    }
    if (use_soft && phase_rot == 0 && state->tetra_sb1_soft_valid) {
        for (int i = 0; i < TETRA_SB_BSCH_DIBITS; i++) {
            soft[i * 2 + 0] = soft_symbol_to_viterbi_cost(state->tetra_sb1_soft[i], state, 0);
            soft[i * 2 + 1] = soft_symbol_to_viterbi_cost(state->tetra_sb1_soft[i], state, 1);
            if (state->tetra_polarity) {
                soft[i * 2 + 0] = (uint16_t)(0xFFFFu - soft[i * 2 + 0]);
            }
        }
    } else {
        tetra_hard_bits_to_soft(hard, soft, TETRA_SB_BSCH_BITS);
    }

    /* ------------------------------------------------------------------
     * Step 3: Descramble type-5 bits back to type-4.  Scrambling follows
     * interleaving in the transmit chain.
     * ------------------------------------------------------------------ */
    tetra_descramble(hard, TETRA_SB_BSCH_BITS, TETRA_SB_SCRAMB_SEED);
    tetra_descramble_soft(soft, TETRA_SB_BSCH_BITS, TETRA_SB_SCRAMB_SEED);

    /* ------------------------------------------------------------------
     * Step 4: Deinterleave (K=120, a=11).
     * ------------------------------------------------------------------ */
    uint8_t deint_hard[TETRA_SB_BSCH_BITS];
    uint16_t deint_soft[TETRA_SB_BSCH_BITS];
    tetra_block_deinterleave(hard, deint_hard, TETRA_SB_BSCH_BITS, TETRA_SB_BSCH_ROWS);
    tetra_block_deinterleave_soft(soft, deint_soft, TETRA_SB_BSCH_BITS, TETRA_SB_BSCH_ROWS, 0);

    /* ------------------------------------------------------------------
     * Step 5: RCPC depuncture (rate-2/3 → 240 mother bits).
     * ------------------------------------------------------------------ */
    uint16_t* depunc = (uint16_t*)malloc(sizeof(uint16_t) * TETRA_SB_DEPUNC_LEN);
    if (!depunc) {
        return -1;
    }
    tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint_soft, TETRA_SB_BSCH_BITS, depunc, TETRA_SB_DEPUNC_LEN);

    /* ------------------------------------------------------------------
     * Step 6: Viterbi decode → 60 type-2 bits.
     * ------------------------------------------------------------------ */
    memset(decoded, 0, (size_t)decoded_len);
    int dec_len = tetra_viterbi_decode_soft(depunc, TETRA_SB_DEPUNC_LEN, decoded, decoded_len);
    free(depunc);
    return dec_len;
}

static int
tetra_sb_crc_ok(const uint8_t* decoded, int dec_len) {
    return dec_len >= TETRA_SB_TYPE2_BITS && tetra_crc16_ccitt_bits(decoded, TETRA_SB_TYPE1_BITS + 16) == TETRA_CRC_OK;
}

void
processTetraSBFrame(dsd_opts* opts, dsd_state* state) {
    soft_symbol_frame_begin(state);

    if (!state->tetra_sb1_valid) {
        tetra_sb_skip_tail(opts, state);
        fprintf(stderr, "[TETRA SB] no SB1 capture available\n");
        state->tetra_sb1_soft_valid = 0;
        tetra_sm_tick(opts, state);
        return;
    }
    state->tetra_sb1_valid = 0; /* consume */

    /* ------------------------------------------------------------------
     * DMO pre-check. A DMO DSB shares the SCH/S coding with the TMO BSCH but
     * its system code is 1xxx (EN 300 396-3 clause 9.3.29). Its second block
     * (SCH/H) follows the SSB directly and must be read live, whereas a TMO SB
     * just skips it. The pre-check uses hard decisions only, so it neither
     * consumes symbols nor depends on the live slicer thresholds; TMO bursts
     * then take exactly the same path as before.
     * ------------------------------------------------------------------ */
    {
        uint8_t pre[128];
        const int pre_len = tetra_sb_decode_block(state, 0, pre, (int)sizeof(pre));
        if (pre_len > 0 && tetra_sb_crc_ok(pre, pre_len) && tetra_dmo_system_code_is_dmo(pre)) {
            state->tetra_sb1_soft_valid = 0;
            state->tetra_decode_ok++;
            if (opts->errorbars) {
                fprintf(stderr, " [TETRA SB  dec=%d rot=%d first12=", pre_len, (int)(state->tetra_phase_rot & 3));
                for (int i = 0; i < 12; i++) {
                    fprintf(stderr, "%d", pre[i] & 1);
                }
                fprintf(stderr, "]");
            }
            tetra_dmo_process_dsb(opts, state, pre);
            return;
        }
    }

    /* ------------------------------------------------------------------
     * Step 1: Consume remaining SB burst bits after SSB detection point.
     * ------------------------------------------------------------------ */
    tetra_sb_skip_tail(opts, state);

    /* ------------------------------------------------------------------
     * Step 2-6: BSCH FEC from the scan-window cache.
     * ------------------------------------------------------------------ */
    uint8_t decoded[128];
    const int dec_len = tetra_sb_decode_block(state, 1, decoded, (int)sizeof(decoded));
    state->tetra_sb1_soft_valid = 0;
    if (dec_len < 0) {
        fprintf(stderr, "[TETRA SB] depunc malloc failed\n");
        tetra_sm_tick(opts, state);
        return;
    }

    if (opts->errorbars) {
        fprintf(stderr, " [TETRA SB  dec=%d rot=%d first12=", dec_len, (int)(state->tetra_phase_rot & 3));
        for (int i = 0; i < 12 && i < dec_len; i++) {
            fprintf(stderr, "%d", decoded[i] & 1);
        }
        fprintf(stderr, "]");
    }

    /* Phase 39: quality counters (SB1 decode also counts). */
    const int crc_ok = tetra_sb_crc_ok(decoded, dec_len);
    if (crc_ok) {
        state->tetra_decode_ok++;
    } else {
        state->tetra_decode_errors++;
    }

    /* ------------------------------------------------------------------
     * Step 7: Parse BSCH PDU (60 type-1 bits) → update network identity.
     * ------------------------------------------------------------------ */
    if (crc_ok && tetra_dmo_system_code_is_dmo(decoded)) {
        /* Only the soft decode passed (the hard pre-check above did not), so
         * this DSB's SCH/H has already been consumed: take the timing and
         * mode from SCH/S alone. */
        tetra_dmo_sync_s_t s;
        tetra_dmo_parse_sync_s(decoded, &s);
        tetra_dmo_apply_sync(state, &s, NULL);
        fprintf(stderr, "[TETRA DMO DSB] sys=%u comm=%u ch=%u slot=%u frame=%u enc=%u | SCH/H skipped\n", s.system_code,
                s.comm_type, s.ab_usage, s.slot, s.frame, s.enc_state);
    } else if (crc_ok) {
        state->tetra_dmo = 0;
        tetra_bsch_parse(decoded, TETRA_SB_TYPE1_BITS, opts, state);
    }

    /* ------------------------------------------------------------------
     * Housekeeping.
     * ------------------------------------------------------------------ */
    snprintf(state->fsubtype, sizeof(state->fsubtype), " BSCH          ");
    snprintf(state->ftype, sizeof(state->ftype), " TETRA");

    tetra_sm_tick(opts, state);
    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}
