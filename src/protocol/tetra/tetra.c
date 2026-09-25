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
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/telemetry.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * NDB burst constants
 * ------------------------------------------------------------------------- */

/* Number of dibits in one NDB block */
#define TETRA_NDB_BLOCK_DIBITS   108
/* Number of encoded bits in one NDB block */
#define TETRA_NDB_BLOCK_BITS     216

/* Block permutation interleaver parameter for a 216-bit NDB/SB2 block.
 * ETSI EN 300 392-2 §8.2.4.1 / osmo-tetra tetra_blk_param[TPSAP_T_NDB]:
 *   K = 216 bits, a (interleave_a) = 101.
 * gcd(216, 101) = 1 ✓ (valid permutation).
 * The cols macro is kept for reference only; the ETSI formula π(i)=1+(a*i%K)
 * does not use a column count. */
#define TETRA_BLOCK_ROWS  101
#define TETRA_BLOCK_COLS  3   /* ceil(216/101) = 3, informational only */

/* Neutral soft cost used for punctured positions inserted by depuncturing.
 * 0x7FFF makes either candidate bit equally likely. Hard-only captured dibits
 * are mapped to confident 0x0000/0xFFFF costs below. */
#define SOFT_NEUTRAL ((uint16_t)0x7FFFu)
#define TETRA_CRC_OK  0x1D0Fu

/* SCH/F: one 432-bit logical channel, interleaver a=103, rate 2/3.
 * gcd(432, 103) = 1. Type-2 is 268 info + 16 CRC + 4 tail. */
#define TETRA_SCHF_BITS      432
#define TETRA_SCHF_A         103
#define TETRA_SCHF_TYPE1     268
#define TETRA_SCHF_TYPE2     288
#define TETRA_SCHF_DEPUNC    (TETRA_SCHF_TYPE2 * 4)

static uint32_t tetra_lfsr_seed_for(const dsd_state *state, int cc)
{
    if (state->tetra_net_known)
        return state->tetra_lfsr_seed;
    return tetra_compute_scramb_seed(0u, 0u, (uint8_t)(cc & 0x3u));
}

/* -------------------------------------------------------------------------
 * tetra_prepare_block()
 *
 * Expand 108 dibits into 216 soft costs. Descrambling is optional: a
 * half-slot (SCH/HD) restarts the LFSR, but NTS1 is one 432-bit scrambled
 * block and must be descrambled only after the two halves are joined.
 * ------------------------------------------------------------------------- */
static void tetra_prepare_block(const uint8_t *dibuf, const float *soft_in,
                                int cc, dsd_state *state,
                                uint16_t *out_soft, int descramble)
{
    uint8_t  hard_bits [TETRA_NDB_BLOCK_BITS];
    uint16_t soft_costs[TETRA_NDB_BLOCK_BITS];

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++) {
        /* An inverted discriminator swaps the positive and negative symbol
         * pairs, which is exactly dibit XOR 2: the MSB changes and the LSB
         * does not. Frame sync records that polarity; normalize it before
         * either hard- or soft-decision FEC sees the block. */
        const uint8_t d = (uint8_t)((dibuf[i] ^ (state->tetra_polarity ? 2u : 0u)) & 3u);
        hard_bits[i * 2 + 0] = (d >> 1) & 1u;
        hard_bits[i * 2 + 1] =  d       & 1u;

        if (soft_in) {
            soft_costs[i * 2 + 0] = soft_symbol_to_viterbi_cost(soft_in[i], state, 0);
            soft_costs[i * 2 + 1] = soft_symbol_to_viterbi_cost(soft_in[i], state, 1);
            if (state->tetra_polarity)
                soft_costs[i * 2 + 0] = (uint16_t)(0xFFFFu - soft_costs[i * 2 + 0]);
        } else {
            soft_costs[i * 2 + 0] = hard_bits[i * 2 + 0] ? 0xFFFFu : 0x0000u;
            soft_costs[i * 2 + 1] = hard_bits[i * 2 + 1] ? 0xFFFFu : 0x0000u;
        }
    }

    if (descramble)
        tetra_descramble_soft(soft_costs, TETRA_NDB_BLOCK_BITS, tetra_lfsr_seed_for(state, cc));

    memcpy(out_soft, soft_costs, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);
}

/* -------------------------------------------------------------------------
 * tetra_decode_schd()
 *
 * SCH-HD path: depuncture a single 216-bit block with rate 2/3,
 * Viterbi decode, and dispatch to MAC parser.
 * ------------------------------------------------------------------------- */
static void tetra_decode_schd(const uint16_t *soft, int cc, int block_idx,
                              dsd_opts *opts, dsd_state *state)
{
    const int punct_id   = TETRA_RCPC_PUNCT_2_3;
    const int type1_len  = 124;
    const int type2_len  = 144; /* 124 payload + 16 CRC + 4 zero tail */
    const int depunc_len = type2_len * 4;

    uint16_t *depunc = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)depunc_len);
    if (!depunc) {
        fprintf(stderr, "[TETRA SCH B%d] depunc malloc failed\n", block_idx);
        return;
    }
    uint16_t deint_soft[TETRA_NDB_BLOCK_BITS];
    tetra_block_deinterleave_soft(soft, deint_soft,
                                  TETRA_NDB_BLOCK_BITS,
                                  TETRA_BLOCK_ROWS, TETRA_BLOCK_COLS);
    tetra_rcpc_depuncture_by_id(punct_id, deint_soft, TETRA_NDB_BLOCK_BITS,
                                depunc, depunc_len);

    uint8_t decoded[256];
    memset(decoded, 0, sizeof(decoded));
    int dec_len = tetra_viterbi_decode_soft(depunc, depunc_len,
                                            decoded, (int)sizeof(decoded));
    free(depunc);

    const int crc_ok = dec_len >= type2_len
                       && tetra_crc16_ccitt_bits(decoded, type1_len + 16) == 0x1D0Fu;
    if (crc_ok)
        state->tetra_decode_ok++;
    else
        state->tetra_decode_errors++;

    if (crc_ok)
        tetra_mac_parse_schd(decoded, type1_len, cc, opts, state);
}

/* -------------------------------------------------------------------------
 * TCH/FS combined decode constants
 *
 * TETRA TCH/FS uses both NDB blocks as one codeword:
 *   432 coded bits → speech matrix deinterleave → class-specific RCPC
 *   → 274 type-2 bits → class reorder → 2 × 137-bit ACELP codec frames
 *
 * ETSI EN 300 392-2 §8.3 Table 8.7 / EN 300 395-2 §6
 * ------------------------------------------------------------------------- */
#define TETRA_TCH_FS_CODED_BITS   432  /* 2 × 216 = combined blocks       */
#define TETRA_TCH_FS_TYPE2_BITS   274  /* two complete 137-bit frames */

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
static void tetra_decode_tch_fs(const uint16_t *soft_b1, const uint16_t *soft_b2,
                                int cc,
                                dsd_opts *opts, dsd_state *state)
{
    /* A weak CB can otherwise feed unrelated/incorrect colour-code bursts
     * into the same stateful speech decoder.  Keep this field filter opt-in;
     * networks with no fixed observed CB colour code retain old behaviour. */
    const char *cc_filter = getenv("TETRA_TCH_CC_FILTER");
    if (cc_filter && cc_filter[0] >= '0' && cc_filter[0] <= '3' &&
        cc_filter[1] == '\0' && cc != cc_filter[0] - '0')
        return;

    /* Step 1: Concatenate Block 1 + Block 2 → 432 soft costs. */
    uint16_t combined[TETRA_TCH_FS_CODED_BITS];
    uint16_t deint[TETRA_TCH_FS_CODED_BITS];
    memcpy(combined,                          soft_b1, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);
    memcpy(combined + TETRA_NDB_BLOCK_BITS, soft_b2, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);

    tetra_speech_deinterleave_soft(combined, deint);

    /* Step 2: Decode the speech-specific class-0 / rate-1/3 RCPC layout. */
    uint8_t decoded[TETRA_TCH_FS_TYPE2_BITS];
    memset(decoded, 0, sizeof(decoded));
    int dec_len = tetra_speech_channel_decode(deint, decoded, (int)sizeof(decoded));

    if (dec_len > 0)
        state->tetra_decode_ok++;
    else
        state->tetra_decode_errors++;

    /* Step 3: ACELP reorder + vocoder. */
    if (dec_len >= TETRA_TCH_FS_TYPE2_BITS) {
        const char *bit_dump = getenv("TETRA_TCH_BIT_DUMP");
        if (bit_dump && bit_dump[0]) {
            static FILE *dump_fp;
            if (!dump_fp)
                dump_fp = fopen(bit_dump, "w");
            if (dump_fp) {
                int weak = 0;
                for (int i = 0; i < 102; i++) {
                    uint16_t c = deint[i];
                    uint16_t dist = c > 0x7fffu ? (uint16_t)(c - 0x7fffu) : (uint16_t)(0x7fffu - c);
                    if (dist < 0x1000u)
                        weak++;
                }
                fprintf(dump_fp, "cc=%d tn=%u pol=%d nts2=%d weak=%d ", cc,
                        state ? (unsigned)state->tetra_tn : 0u,
                        state ? (int)state->tetra_polarity : -1,
                        state ? (int)state->tetra_nts2 : -1, weak);
                for (int i = 0; i < TETRA_TCH_FS_TYPE2_BITS; i++)
                    fputc('0' + (decoded[i] & 1), dump_fp);
                fputc('\n', dump_fp);
                fflush(dump_fp);
            }
        }
        tetra_acelp_process_tch(decoded, dec_len, 0, opts, state);
    }

    if (opts->payload || opts->errorbars) {
        fprintf(stderr, "[TETRA TCH/FS] CC=%d  dec_bits=%d  first16=", cc, dec_len);
        for (int i = 0; i < 16 && i < dec_len; i++)
            fprintf(stderr, "%d", decoded[i] & 1);
        fprintf(stderr, "\n");
    }
}

/* SCH/F uses the same 432 descrambled bits as TCH/FS, then a different
 * interleaver (a=103) and rate-2/3 code. A matching CRC-16 means this NTS1
 * burst is signalling and must not be fed to the speech vocoder. */
static int tetra_schf_crc_ok(const uint16_t *type4)
{
    uint16_t deint[TETRA_SCHF_BITS];
    uint8_t decoded[TETRA_SCHF_TYPE2];
    uint16_t *depunc = (uint16_t *)malloc(sizeof(uint16_t) * TETRA_SCHF_DEPUNC);
    if (!depunc)
        return 0;

    tetra_block_deinterleave_soft(type4, deint, TETRA_SCHF_BITS, TETRA_SCHF_A, 0);
    tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint, TETRA_SCHF_BITS,
                                depunc, TETRA_SCHF_DEPUNC);
    memset(decoded, 0, sizeof(decoded));
    int dec_len = tetra_viterbi_decode_soft(depunc, TETRA_SCHF_DEPUNC,
                                            decoded, (int)sizeof(decoded));
    free(depunc);
    if (dec_len < TETRA_SCHF_TYPE2)
        return 0;
    return tetra_crc16_ccitt_bits(decoded, TETRA_SCHF_TYPE1 + 16) == TETRA_CRC_OK;
}

/* -------------------------------------------------------------------------
 * processTetraFrame()
 *
 * Called by the engine after NDB NTS sync is confirmed.
 * Phase 4: decode Block 1 (from scan-window capture) then Block 2 (live).
 * ------------------------------------------------------------------------- */
void processTetraFrame(dsd_opts* opts, dsd_state* state)
{
    soft_symbol_frame_begin(state);
    tetra_tdma_advance_ndb(state);

    /* AACH half 2 sits between the training sequence and Block 2. */
    skipDibit(opts, state, 8);

    /* ---------------------------------------------------------------
     * Read Block 2 live (108 dibits = 216 bits).
     * --------------------------------------------------------------- */
    uint8_t b2_dibuf[TETRA_NDB_BLOCK_DIBITS];
    float   b2_soft_in[TETRA_NDB_BLOCK_DIBITS];

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++)
        b2_dibuf[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &b2_soft_in[i]);

    /* Colour code and stealing flags live in the Reed-Muller AACH, which is
     * not decoded yet. NTS1 is the single-channel burst, so try TCH/FS there.
     * The network colour from BSCH is not the 2-bit burst colour; pass 0 and
     * let the scrambler use tetra_lfsr_seed once the network is known. */
    const int cc = 0;

    snprintf(state->fsubtype, sizeof(state->fsubtype),
             state->tetra_nts2 ? " NTS2         " : " NTS1         ");
    snprintf(state->ftype, sizeof(state->ftype), " TETRA");

    if (opts->errorbars) {
        fprintf(stderr, " [TETRA NDB  NTS%d  pol=%d]",
                state->tetra_nts2 ? 2 : 1, (int)state->tetra_polarity);
    }

    /* NTS2 carries two independently scrambled half-slot channels. Try the
     * SCH/HD CRC on each half before dispatching its MAC payload. */
    int b1_valid = state->tetra_b1_valid;
    if (state->tetra_nts2) {
        uint16_t block_soft[TETRA_NDB_BLOCK_BITS];
        if (b1_valid) {
            tetra_prepare_block(state->tetra_b1_dibuf,
                                state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL,
                                cc, state, block_soft, 1);
            tetra_decode_schd(block_soft, cc, 1, opts, state);
        }
        tetra_prepare_block(b2_dibuf, b2_soft_in, cc, state, block_soft, 1);
        tetra_decode_schd(block_soft, cc, 2, opts, state);
    }

    /* NTS1 carries one 432-bit logical channel. Scrambling runs across
     * Block1||Block2; restarting the LFSR on Block 2 descrambles the second
     * half with the wrong keystream. SCH/F and TCH/FS share those bits and
     * are separated by the SCH/F CRC. NTS2 is two half-slots, not TCH/FS. */
    if (!state->tetra_nts2 && b1_valid) {
        uint16_t b1_soft[TETRA_NDB_BLOCK_BITS];
        uint16_t b2_soft[TETRA_NDB_BLOCK_BITS];
        uint16_t type4[TETRA_SCHF_BITS];

        tetra_prepare_block(state->tetra_b1_dibuf,
                            state->tetra_b1_soft_valid ? state->tetra_b1_soft : NULL,
                            cc, state, b1_soft, 0);
        tetra_prepare_block(b2_dibuf, b2_soft_in, cc, state, b2_soft, 0);
        memcpy(type4, b1_soft, sizeof(b1_soft));
        memcpy(type4 + TETRA_NDB_BLOCK_BITS, b2_soft, sizeof(b2_soft));
        tetra_descramble_soft(type4, TETRA_SCHF_BITS, tetra_lfsr_seed_for(state, cc));

        if (tetra_schf_crc_ok(type4)) {
            state->tetra_decode_ok++;
            snprintf(state->fsubtype, sizeof(state->fsubtype), " SCH/F         ");
            if (opts->payload || opts->errorbars)
                fprintf(stderr, "[TETRA SCH/F] tn=%u CRC OK\n", (unsigned)state->tetra_tn);
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
    if (dsd_opts_frontend_active(opts))
        dsd_telemetry_publish_both_and_redraw(opts, state);
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
#define TETRA_SB_BSCH_DIBITS   60    /* dibits in SB1 block (BSCH) */
#define TETRA_SB_BSCH_BITS     120   /* physical bits after dibit expansion */
#define TETRA_SB_BB_DIBITS     15    /* broadcast bits (AACH) after SSB */
#define TETRA_SB_BKN2_DIBITS   108   /* BKN2 block (SB2, BNCH) after BB */
#define TETRA_SB_BSCH_ROWS     11    /* interleaver parameter a for K=120 */
#define TETRA_SB_SCRAMB_SEED   3u    /* SCRAMB_INIT = tetra_compute_scramb_seed(0,0,0) */
#define TETRA_SB_TYPE2_BITS    80
#define TETRA_SB_TYPE1_BITS    60
#define TETRA_SB_DEPUNC_LEN    (TETRA_SB_TYPE2_BITS * 4)

void processTetraSBFrame(dsd_opts *opts, dsd_state *state)
{
    soft_symbol_frame_begin(state);

    /* ------------------------------------------------------------------
     * Step 1: Consume remaining SB burst bits after SSB detection point.
     * ------------------------------------------------------------------ */
    skipDibit(opts, state, TETRA_SB_BB_DIBITS);    /* BB / AACH (15 dibits) */
    skipDibit(opts, state, TETRA_SB_BKN2_DIBITS);  /* BKN2 block (108 dibits) */

    /* ------------------------------------------------------------------
     * Step 2: Process SB1 (BSCH) block from scan-window cache.
     * ------------------------------------------------------------------ */
    if (!state->tetra_sb1_valid) {
        fprintf(stderr, "[TETRA SB] no SB1 capture available\n");
        state->tetra_sb1_soft_valid = 0;
        tetra_sm_tick(opts, state);
        return;
    }
    state->tetra_sb1_valid = 0; /* consume */

    /* Expand 60 dibits → 120 bits. Prefer the analog decisions captured from
     * symbol history; retain the hard-decision fallback for symbol replay and
     * callers which do not populate history. */
    uint8_t  hard[TETRA_SB_BSCH_BITS];
    uint16_t soft[TETRA_SB_BSCH_BITS];
    for (int i = 0; i < TETRA_SB_BSCH_DIBITS; i++) {
        uint8_t d = (uint8_t)((state->tetra_sb1_dibuf[i]
                               ^ (state->tetra_polarity ? 2u : 0u)) & 3u);
        hard[i * 2 + 0] = (d >> 1) & 1u;
        hard[i * 2 + 1] =  d       & 1u;
    }
    if (state->tetra_sb1_soft_valid) {
        for (int i = 0; i < TETRA_SB_BSCH_DIBITS; i++) {
            soft[i * 2 + 0] = soft_symbol_to_viterbi_cost(state->tetra_sb1_soft[i], state, 0);
            soft[i * 2 + 1] = soft_symbol_to_viterbi_cost(state->tetra_sb1_soft[i], state, 1);
            if (state->tetra_polarity)
                soft[i * 2 + 0] = (uint16_t)(0xFFFFu - soft[i * 2 + 0]);
        }
    } else {
        tetra_hard_bits_to_soft(hard, soft, TETRA_SB_BSCH_BITS);
    }
    state->tetra_sb1_soft_valid = 0;

    /* ------------------------------------------------------------------
     * Step 3: Descramble type-5 bits back to type-4.  Scrambling follows
     * interleaving in the transmit chain.
     * ------------------------------------------------------------------ */
    tetra_descramble     (hard, TETRA_SB_BSCH_BITS, TETRA_SB_SCRAMB_SEED);
    tetra_descramble_soft(soft, TETRA_SB_BSCH_BITS, TETRA_SB_SCRAMB_SEED);

    /* ------------------------------------------------------------------
     * Step 4: Deinterleave (K=120, a=11).
     * ------------------------------------------------------------------ */
    uint8_t  deint_hard[TETRA_SB_BSCH_BITS];
    uint16_t deint_soft[TETRA_SB_BSCH_BITS];
    tetra_block_deinterleave(hard, deint_hard, TETRA_SB_BSCH_BITS, TETRA_SB_BSCH_ROWS);
    tetra_block_deinterleave_soft(soft, deint_soft,
                                  TETRA_SB_BSCH_BITS, TETRA_SB_BSCH_ROWS, 0);

    /* ------------------------------------------------------------------
     * Step 5: RCPC depuncture (rate-2/3 → 240 mother bits).
     * ------------------------------------------------------------------ */
    uint16_t *depunc = (uint16_t *)malloc(sizeof(uint16_t) * TETRA_SB_DEPUNC_LEN);
    if (!depunc) {
        fprintf(stderr, "[TETRA SB] depunc malloc failed\n");
        tetra_sm_tick(opts, state);
        return;
    }
    tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint_soft, TETRA_SB_BSCH_BITS,
                                depunc, TETRA_SB_DEPUNC_LEN);

    /* ------------------------------------------------------------------
     * Step 6: Viterbi decode → 60 type-2 bits.
     * ------------------------------------------------------------------ */
    uint8_t decoded[128];
    memset(decoded, 0, sizeof(decoded));
    int dec_len = tetra_viterbi_decode_soft(depunc, TETRA_SB_DEPUNC_LEN,
                                            decoded, (int)sizeof(decoded));
    free(depunc);

    if (opts->errorbars) {
        fprintf(stderr, " [TETRA SB  dec=%d  first12=", dec_len);
        for (int i = 0; i < 12 && i < dec_len; i++)
            fprintf(stderr, "%d", decoded[i] & 1);
        fprintf(stderr, "]");
    }

    /* Phase 39: quality counters (SB1 decode also counts). */
    const int crc_ok = dec_len >= TETRA_SB_TYPE2_BITS
                       && tetra_crc16_ccitt_bits(decoded, TETRA_SB_TYPE1_BITS + 16) == TETRA_CRC_OK;
    if (crc_ok)
        state->tetra_decode_ok++;
    else
        state->tetra_decode_errors++;

    /* ------------------------------------------------------------------
     * Step 7: Parse BSCH PDU (60 type-1 bits) → update network identity.
     * ------------------------------------------------------------------ */
    if (crc_ok)
        tetra_bsch_parse(decoded, TETRA_SB_TYPE1_BITS, opts, state);

    /* ------------------------------------------------------------------
     * Housekeeping.
     * ------------------------------------------------------------------ */
    snprintf(state->fsubtype, sizeof(state->fsubtype), " BSCH          ");
    snprintf(state->ftype,    sizeof(state->ftype),    " TETRA");

    tetra_sm_tick(opts, state);
    if (dsd_opts_frontend_active(opts))
        dsd_telemetry_publish_both_and_redraw(opts, state);
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}
