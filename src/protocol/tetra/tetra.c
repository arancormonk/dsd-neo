// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA NDB (Normal Downlink Burst) frame processor
 * ETSI EN 300 392-2 §9.4.3 / §9.4.5 / §23.4
 *
 * NDB structure (510 bits = 255 dibits):
 *
 *   [1d tail][← Block1: 108d/216b →][← NTS: 11d/22b (consumed by sync) →]
 *   [← Block2: 108d/216b →][1d tail][← CB: 5d/10b →]
 *
 * Phase 1+2 (original): read Block 2 + CB, run FEC for the live block.
 * Phase 4 (this file):  Block 1 hard dibits are recovered from the sync scan
 *   window by dsd_frame_sync.c and stored in state->tetra_b1_dibuf[].  Both
 *   blocks are now decoded independently; each NDB block is a self-contained
 *   TETRA TCH/HR or SCH-HD sub-frame (ETSI §9.4.3).
 *
 * CB field bit layout (ETSI EN 300 392-2 Table 9.36):
 *   bits 1-2  : Colour Code (CC, MSB first)
 *   bit  3    : Stealing Flag 1 – Block 1  (0=TCH, 1=SCH-HD)
 *   bit  4    : Stealing Flag 2 – Block 2  (0=TCH, 1=SCH-HD)
 *   bits 5-10 : reserved
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
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

/* Tail bits after Block 2 (1 dibit, discarded) */
#define TETRA_NDB_TAIL_DIBITS      1
/* Control Bits field (5 dibits = 10 bits) */
#define TETRA_NDB_CB_DIBITS        5

/* CB bit indices within the flat 10-bit unpacked array (0-indexed, MSB-first) */
#define CB_IDX_CC1   0   /* Colour Code bit 1 (MSB) */
#define CB_IDX_CC0   1   /* Colour Code bit 0 (LSB) */
#define CB_IDX_SF1   2   /* Stealing Flag 1 – Block 1 (0=TCH, 1=SCH-HD) */
#define CB_IDX_SF2   3   /* Stealing Flag 2 – Block 2 (0=TCH, 1=SCH-HD) */

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

/* -------------------------------------------------------------------------
 * tetra_prepare_block()
 *
 * Shared physical-layer pipeline for one 216-bit NDB block:
 * dibits → hard bits + soft costs → deinterleave → descramble.
 *
 * Returns the descrambled soft-decision costs in @out_soft (216 elements).
 * The caller uses these for either:
 *   - SCH-HD: per-block depuncture(rate 2/3) + Viterbi
 *   - TCH/FS: combine two blocks then depuncture(rate 292/432) + Viterbi
 *
 * @dibuf      – 108 hard dibits (values 0-3), one byte per dibit
 * @soft_in    – 108 soft floats per dibit, or NULL to use neutral costs
 * @cc         – Colour Code (for LFSR seed)
 * @state      – dsd_state (for LFSR seed lookup)
 * @out_soft   – output: 216 descrambled soft-decision costs
 * ------------------------------------------------------------------------- */
static void tetra_prepare_block(const uint8_t *dibuf, const float *soft_in,
                                int cc, dsd_state *state,
                                uint16_t *out_soft)
{
    /* Step 1: dibits → hard bits + soft costs. */
    uint8_t  hard_bits [TETRA_NDB_BLOCK_BITS];
    uint16_t soft_costs[TETRA_NDB_BLOCK_BITS];

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++) {
        hard_bits[i * 2 + 0] = (dibuf[i] >> 1) & 1u;
        hard_bits[i * 2 + 1] =  dibuf[i]        & 1u;

        if (soft_in) {
            soft_costs[i * 2 + 0] = soft_symbol_to_viterbi_cost(soft_in[i], state, 0);
            soft_costs[i * 2 + 1] = soft_symbol_to_viterbi_cost(soft_in[i], state, 1);
        } else {
            soft_costs[i * 2 + 0] = hard_bits[i * 2 + 0] ? 0xFFFFu : 0x0000u;
            soft_costs[i * 2 + 1] = hard_bits[i * 2 + 1] ? 0xFFFFu : 0x0000u;
        }
    }

    /* Step 2: Deinterleave (K=216, a=101). */
    uint8_t  deint_hard[TETRA_NDB_BLOCK_BITS];
    uint16_t deint_soft[TETRA_NDB_BLOCK_BITS];
    memset(deint_hard, 0, sizeof(deint_hard));
    for (int i = 0; i < TETRA_NDB_BLOCK_BITS; i++) deint_soft[i] = SOFT_NEUTRAL;

    tetra_block_deinterleave(hard_bits, deint_hard,
                             TETRA_NDB_BLOCK_BITS, TETRA_BLOCK_ROWS);
    tetra_block_deinterleave_soft(soft_costs, deint_soft,
                                  TETRA_NDB_BLOCK_BITS,
                                  TETRA_BLOCK_ROWS, TETRA_BLOCK_COLS);

    /* Step 3: Descramble. */
    uint32_t lfsr_seed;
    if (state->tetra_net_known) {
        lfsr_seed = state->tetra_lfsr_seed;
    } else {
        lfsr_seed = tetra_compute_scramb_seed(0u, 0u, (uint8_t)(cc & 0x3u));
    }
    tetra_descramble     (deint_hard, TETRA_NDB_BLOCK_BITS, lfsr_seed);
    tetra_descramble_soft(deint_soft, TETRA_NDB_BLOCK_BITS, lfsr_seed);

    memcpy(out_soft, deint_soft, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);
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
    tetra_rcpc_depuncture_by_id(punct_id, soft, TETRA_NDB_BLOCK_BITS,
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
 *   432 coded bits → depuncture(292/432) → ~1168 mother bits → Viterbi
 *   → 292 type-2 bits → class reorder → 2 × 137-bit ACELP codec frames
 *
 * ETSI EN 300 392-2 §8.3 Table 8.7 / EN 300 395-2 §6
 * ------------------------------------------------------------------------- */
#define TETRA_TCH_FS_CODED_BITS   432  /* 2 × 216 = combined blocks       */
#define TETRA_TCH_FS_DEPUNC_LEN  1168  /* 292 × 4 (rate 1/4 mother code)  */
#define TETRA_TCH_FS_TYPE2_BITS   292  /* Viterbi decoded info bits        */

/* -------------------------------------------------------------------------
 * tetra_decode_tch_fs()
 *
 * TCH/FS path: combine two prepared 216-bit blocks, depuncture with
 * rate 292/432, Viterbi decode → 292 type-2 bits, then ACELP reorder
 * and vocoder processing.
 *
 * @soft_b1 – 216 descrambled soft costs from Block 1
 * @soft_b2 – 216 descrambled soft costs from Block 2
 * @cc      – Colour Code
 * @opts    – dsd_opts
 * @state   – dsd_state
 * ------------------------------------------------------------------------- */
static void tetra_decode_tch_fs(const uint16_t *soft_b1, const uint16_t *soft_b2,
                                int cc,
                                dsd_opts *opts, dsd_state *state)
{
    /* Step 1: Concatenate Block 1 + Block 2 → 432 soft costs. */
    uint16_t combined[TETRA_TCH_FS_CODED_BITS];
    memcpy(combined,                          soft_b1, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);
    memcpy(combined + TETRA_NDB_BLOCK_BITS, soft_b2, sizeof(uint16_t) * TETRA_NDB_BLOCK_BITS);

    /* Step 2: Depuncture with rate 292/432 → 1168 mother bits. */
    uint16_t *depunc = (uint16_t *)malloc(sizeof(uint16_t) * TETRA_TCH_FS_DEPUNC_LEN);
    if (!depunc) {
        fprintf(stderr, "[TETRA TCH/FS] depunc malloc failed\n");
        return;
    }
    tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_292_432,
                                combined, TETRA_TCH_FS_CODED_BITS,
                                depunc, TETRA_TCH_FS_DEPUNC_LEN);

    /* Step 3: Viterbi decode → up to 292 type-2 bits. */
    uint8_t decoded[512];
    memset(decoded, 0, sizeof(decoded));
    int dec_len = tetra_viterbi_decode_soft(depunc, TETRA_TCH_FS_DEPUNC_LEN,
                                            decoded, (int)sizeof(decoded));
    free(depunc);

    if (dec_len > 0)
        state->tetra_decode_ok++;
    else
        state->tetra_decode_errors++;

    /* Step 4: ACELP reorder + vocoder.
     * tetra_acelp_process_tch() expects at least 272 type-2 bits
     * (100 class-0 + 112 class-1 + 60 class-2, interleaved for 2 frames). */
    if (dec_len >= TETRA_TCH_FS_TYPE2_BITS ||
        (dec_len > 0 && dec_len >= 272)) {
        tetra_acelp_process_tch(decoded, dec_len, 0, opts, state);
    }

    if (opts->payload || opts->errorbars) {
        fprintf(stderr, "[TETRA TCH/FS] CC=%d  dec_bits=%d  first16=", cc, dec_len);
        for (int i = 0; i < 16 && i < dec_len; i++)
            fprintf(stderr, "%d", decoded[i] & 1);
        fprintf(stderr, "\n");
    }
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

    /* ---------------------------------------------------------------
     * Read Block 2 live (108 dibits = 216 bits).
     * --------------------------------------------------------------- */
    uint8_t b2_dibuf[TETRA_NDB_BLOCK_DIBITS];
    float   b2_soft_in[TETRA_NDB_BLOCK_DIBITS];

    for (int i = 0; i < TETRA_NDB_BLOCK_DIBITS; i++)
        b2_dibuf[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &b2_soft_in[i]);

    /* Consume post-block tail dibit (no information). */
    skipDibit(opts, state, TETRA_NDB_TAIL_DIBITS);

    /* ---------------------------------------------------------------
     * Read CB (5 dibits = 10 bits), extract CC, SF1, SF2.
     * --------------------------------------------------------------- */
    uint8_t cb_dibuf[TETRA_NDB_CB_DIBITS];
    for (int i = 0; i < TETRA_NDB_CB_DIBITS; i++)
        cb_dibuf[i] = (uint8_t)getDibitSoft(opts, state, NULL);

    uint8_t cb_bits[10];
    for (int i = 0; i < TETRA_NDB_CB_DIBITS; i++) {
        cb_bits[i * 2 + 0] = (cb_dibuf[i] >> 1) & 1u;
        cb_bits[i * 2 + 1] =  cb_dibuf[i]        & 1u;
    }

    const int cc  = (int)((cb_bits[CB_IDX_CC1] << 1) | cb_bits[CB_IDX_CC0]);
    const int sf1 = (int)  cb_bits[CB_IDX_SF1];
    const int sf2 = (int)  cb_bits[CB_IDX_SF2];

    /* Update display fields (show Block 2 type; Block 1 type logged per-block). */
    snprintf(state->fsubtype, sizeof(state->fsubtype),
             sf2 ? " SCH-HD       " : " TCH          ");
    snprintf(state->ftype, sizeof(state->ftype), " TETRA");

    if (opts->errorbars) {
        fprintf(stderr, " [TETRA NDB  CC=%d  SF1=%d(%s)  SF2=%d(%s)]",
                cc,
                sf1, sf1 ? "SCH-HD" : "TCH",
                sf2, sf2 ? "SCH-HD" : "TCH");
    }

    /* ---------------------------------------------------------------
     * Phase 4: Process Block 1 using hard-dibit capture from scan window.
     * tetra_b1_valid is set by dsd_frame_sync.c at sync detection time.
     * --------------------------------------------------------------- */
    int b1_valid = state->tetra_b1_valid;
    uint16_t b1_soft[TETRA_NDB_BLOCK_BITS];
    if (b1_valid) {
        tetra_prepare_block(state->tetra_b1_dibuf,
                            NULL,   /* no soft – hard-only path */
                            cc, state, b1_soft);
        state->tetra_b1_valid = 0; /* consume; next frame will re-capture */
    }

    /* ---------------------------------------------------------------
     * Prepare Block 2 with full soft-decision data.
     * --------------------------------------------------------------- */
    uint16_t b2_prep[TETRA_NDB_BLOCK_BITS];
    tetra_prepare_block(b2_dibuf, b2_soft_in, cc, state, b2_prep);

    /* ---------------------------------------------------------------
     * Dispatch by stealing flags.
     *
     * TCH/FS (sf1==0 && sf2==0): Both blocks carry one combined voice
     *   codeword; concatenate 432 soft costs → depuncture(292/432) →
     *   Viterbi → 292 type-2 bits → ACELP reorder → 2 codec frames.
     *
     * SCH-HD (sf==1): Single-block signaling; per-block rate-2/3
     *   depuncture + Viterbi + MAC parser.
     *
     * Mixed (one TCH, one SCH-HD): The solo TCH half cannot form a
     *   complete speech frame. Only the SCH-HD block is decoded.
     * --------------------------------------------------------------- */
    if (sf1 == 0 && sf2 == 0 && b1_valid) {
        /* TCH/FS: combined FEC decode over both blocks */
        tetra_decode_tch_fs(b1_soft, b2_prep, cc, opts, state);
    } else {
        /* Process each SCH-HD block independently */
        if (sf1 == 1 && b1_valid)
            tetra_decode_schd(b1_soft, cc, 1, opts, state);
        if (sf2 == 1)
            tetra_decode_schd(b2_prep, cc, 2, opts, state);
        /* sf==0 without partner block → TCH data lost, skip */
    }

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
 *   60 dibits → 120 bits → deinterleave(K=120,a=11) → descramble(seed=3)
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
#define TETRA_CRC_OK           0x1D0Fu

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
        tetra_sm_tick(opts, state);
        return;
    }
    state->tetra_sb1_valid = 0; /* consume */

    /* Expand 60 dibits → 120 hard bits and preserve their decisions as
     * confident Viterbi costs. The sync cache has no amplitudes, but hard
     * decisions still carry information; neutral costs discard the BSCH. */
    uint8_t  hard[TETRA_SB_BSCH_BITS];
    uint16_t soft[TETRA_SB_BSCH_BITS];
    for (int i = 0; i < TETRA_SB_BSCH_DIBITS; i++) {
        uint8_t d = state->tetra_sb1_dibuf[i] & 3u;
        hard[i * 2 + 0] = (d >> 1) & 1u;
        hard[i * 2 + 1] =  d       & 1u;
    }
    tetra_hard_bits_to_soft(hard, soft, TETRA_SB_BSCH_BITS);

    /* ------------------------------------------------------------------
     * Step 3: Deinterleave (K=120, a=11).
     * ------------------------------------------------------------------ */
    uint8_t  deint_hard[TETRA_SB_BSCH_BITS];
    uint16_t deint_soft[TETRA_SB_BSCH_BITS];
    tetra_block_deinterleave(hard, deint_hard, TETRA_SB_BSCH_BITS, TETRA_SB_BSCH_ROWS);
    tetra_block_deinterleave_soft(soft, deint_soft,
                                  TETRA_SB_BSCH_BITS, TETRA_SB_BSCH_ROWS, 0);

    /* ------------------------------------------------------------------
     * Step 4: Descramble with SCRAMB_INIT (seed=3; MCC=MNC=colour=0).
     * ------------------------------------------------------------------ */
    tetra_descramble     (deint_hard, TETRA_SB_BSCH_BITS, TETRA_SB_SCRAMB_SEED);
    tetra_descramble_soft(deint_soft, TETRA_SB_BSCH_BITS, TETRA_SB_SCRAMB_SEED);

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
