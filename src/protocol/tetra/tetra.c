// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA bursts after frame sync: the normal and synchronisation downlink bursts of EN 300 392-2 clauses 9.4.4.2.5 and
 * 9.4.4.2.6, the normal and control uplink bursts of clauses 9.4.4.2.4 and 9.4.4.2.1, and the direct-mode bursts of
 * EN 300 396-2 clause 9.4.
 *
 * A normal downlink burst (NDB) is 255 dibits. Relative to the start L of its training sequence:
 *
 *   block 1 [L-115, L-7) 108 dibits | AACH half 1 [L-7, L) | training [L, L+11) | AACH half 2 [L+11, L+19) |
 *   block 2 [L+19, L+127) 108 dibits
 *
 * Training sequence 1 (NTS1) marks one 432-bit logical channel over both blocks, scrambled as one block: SCH/F
 * (interleaver a = 103, rate 2/3, CRC-16) or full-slot speech. Training sequence 2 (NTS2) marks two half-slot channels,
 * each scrambled on its own: SCH/HD or STCH, the second half also half-slot speech after a stolen first half. The AACH,
 * not decoded here, would say which; the CRCs separate them instead.
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/tetra_publication.h>
#include <dsd-neo/dsp/tetra_training.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_log.h"
#include "tetra_state.h"
#include "tetra_voice_state.h"

enum {
    /* One block of a normal burst. */
    NB_BLOCK_DIBITS = 108,
    NB_BLOCK_BITS = 216,
    /* AACH half 2, between the training sequence and block 2 of an NDB. */
    NDB_AACH2_DIBITS = 8,
    /* SCH/HD, STCH and BNCH (EN 300 392-2 clause 8.3.1.4.1): 124 type-1 bits, CRC-16 and four tail bits at rate 2/3,
       interleaved with a = 101. Half-slot speech uses the same interleaver. */
    HD_TYPE1 = 124,
    HD_TYPE2 = 144,
    HD_A = 101,
    /* SCH/F (clause 8.3.1.4.5): 268 type-1 bits, CRC-16 and four tail bits at rate 2/3, interleaved with a = 103. */
    SCHF_BITS = 432,
    SCHF_TYPE1 = 268,
    SCHF_TYPE2 = 288,
    SCHF_A = 103,
    /* Synchronisation burst (clause 9.4.4.2.6): the BSCH block before the training sequence, then 15 dibits of
       broadcast block and 108 of block 2 after it. BSCH (clause 8.3.1.2): 60 type-1 bits, CRC-16 and four tail bits at
       rate 2/3, interleaved with a = 11. */
    SB_BSCH_DIBITS = 60,
    SB_BSCH_BITS = 120,
    SB_BB_DIBITS = 15,
    SB_BKN2_DIBITS = 108,
    SB_BSCH_A = 11,
    SB_TYPE1 = 60,
    SB_TYPE2 = 80,
    /* Control uplink burst (clause 9.4.4.2.1): 42 dibits, extended training sequence, 42 dibits; one SCH/HU block. */
    CB_HALF_DIBITS = 42,
    CB_BITS = 168,
    SCHHU_TYPE1 = 92,
};

/* The BSCH, and the SCH/S and SCH/H of a direct-mode synchronisation burst, are scrambled with the all-zero colour
   code (EN 300 392-2 clause 8.2.5.2, EN 300 396-2 clause 8.3.1.3). */
#define TETRA_SB_SCRAMB_SEED       3u

/* Most channel bits of a speech burst that may disagree with a re-encode of what was decoded from it, on top of its
   CRC: a speech burst received through the right scrambler leaves a handful (0..16 measured on real air), a
   signalling burst or one descrambled with the wrong colour code leaves 36 to 44 of the 330 full-slot bits. */
#define TETRA_TCH_MAX_MISMATCH     28
#define TETRA_HALF_MAX_MISMATCH    16

/* Direct-mode bursts since the last DSB after which the call's colour code is no longer trusted. */
#define TETRA_DMO_DCC_STALE_BURSTS 200u

/* The trunked-mode scrambling seed of every block but the BSCH (EN 300 392-2 clause 8.2.5.2): the extended colour code
   of the network, from a BSCH or --tetra-net, else all zero. */
static uint32_t
tetra_lfsr_seed_for(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (ts && ts->net_known) {
        return ts->lfsr_seed;
    }
    return tetra_compute_scramb_seed(0u, 0u, 0u);
}

/* The colour code the burst was descrambled with, which the MAC keeps fragments of different cells apart by: the
   network's once known, else 0. */
static int
tetra_burst_colour(const dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return (ts && ts->net_known) ? (int)(ts->colour & 0x3Fu) : 0;
}

/* Expand @p count dibits into 2 * @p count soft costs after undoing the slicer phase step and polarity @p inverted. An
   inverted discriminator swaps the positive and negative symbol pairs, which is dibit XOR 2. A burst whose training
   sequence matched only after a phase step had its soft values sliced in that rotated phase, so it falls back to
   confident hard decisions. Descrambling is the caller's. */
static void
tetra_expand_dibits_as(const dsd_state* state, int inverted, const uint8_t* dibits, const float* soft_in, int count,
                       uint16_t* out) {
    const int phase_rot = state->tetra_sync.phase_rot & 3;
    if (phase_rot != 0) {
        soft_in = NULL;
    }
    for (int i = 0; i < count; i++) {
        const uint8_t d = (uint8_t)((tetra_dibit_derotate(dibits[i], phase_rot) ^ (inverted ? 2 : 0)) & 3);
        if (soft_in) {
            out[(i * 2) + 0] = soft_symbol_to_viterbi_cost(soft_in[i], state, 0);
            out[(i * 2) + 1] = soft_symbol_to_viterbi_cost(soft_in[i], state, 1);
            if (inverted) {
                out[(i * 2) + 0] = (uint16_t)(0xFFFFu - out[(i * 2) + 0]);
            }
        } else {
            out[(i * 2) + 0] = ((d >> 1) & 1u) ? 0xFFFFu : 0x0000u;
            out[(i * 2) + 1] = (d & 1u) ? 0xFFFFu : 0x0000u;
        }
    }
}

/* tetra_expand_dibits_as() with the carrier's polarity, which a synchronisation burst or a control uplink burst set. */
static void
tetra_expand_dibits(const dsd_state* state, const uint8_t* dibits, const float* soft_in, int count, uint16_t* out) {
    tetra_expand_dibits_as(state, state->tetra_sync.polarity != 0, dibits, soft_in, count, out);
}

/* Block 1 of the burst, as the frame sync captured it. */
static void
tetra_expand_block1(const dsd_state* state, uint16_t* out) {
    const float* soft = state->tetra_sync.b1_soft_valid ? state->tetra_sync.b1_soft : NULL;
    tetra_expand_dibits(state, state->tetra_sync.b1_dibuf, soft, NB_BLOCK_DIBITS, out);
}

static void
tetra_read_dibits(dsd_opts* opts, dsd_state* state, uint8_t* dibits, float* soft, int count) {
    for (int i = 0; i < count; i++) {
        dibits[i] = (uint8_t)getDibitAndSoftSymbol(opts, state, &soft[i]);
    }
}

/* Block 2 of a normal burst whose block 1 sits against the training sequence (uplink, direct mode): first the dibits
   the frame sync already read past the training (dsd_tetra_sync_capture::b2_lead_count, an uplink lock), then the rest
   from the input. Returns the soft values to decode with: @p soft, or NULL when the lead came without soft values, so
   the block is decoded from its hard decisions alone. */
static const float*
tetra_read_block2(dsd_opts* opts, dsd_state* state, uint8_t* dibits, float* soft) {
    dsd_tetra_sync_capture* const sync = &state->tetra_sync;
    const int lead = sync->b2_lead_count <= sizeof sync->b2_lead_dibits ? (int)sync->b2_lead_count : 0;
    for (int i = 0; i < lead; i++) {
        dibits[i] = (uint8_t)(sync->b2_lead_dibits[i] & 3u);
        soft[i] = sync->b2_lead_soft_valid ? sync->b2_lead_soft[i] : 0.0F;
    }
    const int soft_ok = lead == 0 || sync->b2_lead_soft_valid;
    sync->b2_lead_count = 0;
    sync->b2_lead_soft_valid = 0;
    tetra_read_dibits(opts, state, dibits + lead, soft + lead, NB_BLOCK_DIBITS - lead);
    return soft_ok ? soft : NULL;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Channel decoders.
 * ------------------------------------------------------------------------------------------------------------- */

/* SCH/HD and STCH: one descrambled 216-bit block to 124 type-1 bits in @p decoded (HD_TYPE2 bytes). 1 when the
   CRC-16 passes. */
static int
tetra_hd_decode_bits(const uint16_t* soft, uint8_t* decoded) {
    uint16_t deint[NB_BLOCK_BITS];
    uint16_t depunc[HD_TYPE2 * 4];
    tetra_block_deinterleave_soft(soft, deint, NB_BLOCK_BITS, HD_A);
    (void)tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint, NB_BLOCK_BITS, depunc, HD_TYPE2 * 4);
    DSD_MEMSET(decoded, 0, HD_TYPE2);
    return tetra_viterbi_decode_soft(depunc, HD_TYPE2 * 4, decoded, HD_TYPE2) == HD_TYPE2
           && tetra_crc16_ccitt_bits(decoded, HD_TYPE1 + 16) == TETRA_CRC16_OK;
}

/* SCH/F: the 432 descrambled bits of an NTS1 burst to 268 type-1 bits in @p decoded (SCHF_TYPE2 bytes). 1 when the
   CRC-16 passes; otherwise the burst may be speech. */
static int
tetra_schf_decode_bits(const uint16_t* type4, uint8_t* decoded) {
    uint16_t deint[SCHF_BITS];
    uint16_t depunc[SCHF_TYPE2 * 4];
    tetra_block_deinterleave_soft(type4, deint, SCHF_BITS, SCHF_A);
    (void)tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint, SCHF_BITS, depunc, SCHF_TYPE2 * 4);
    DSD_MEMSET(decoded, 0, SCHF_TYPE2);
    return tetra_viterbi_decode_soft(depunc, SCHF_TYPE2 * 4, decoded, SCHF_TYPE2) == SCHF_TYPE2
           && tetra_crc16_ccitt_bits(decoded, SCHF_TYPE1 + 16) == TETRA_CRC16_OK;
}

/* Full-slot speech (EN 300 395-2 clause 5.5) in the 432 descrambled bits of an NTS1 burst that is not SCH/F. A frame
   is good when its CRC passed and it re-encodes like speech; a bad one reaches the codec as a bad frame. Returns 1 for
   a good frame. */
static int
tetra_decode_tch_fs(const uint16_t* type4, int cc, dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return 0;
    }
    uint16_t deint[SCHF_BITS];
    uint8_t bits[TETRA_SPEECH_FULL_BITS];
    int crc_ok = 0;
    int mismatches = -1;

    tetra_speech_deinterleave_soft(type4, deint);
    if (tetra_speech_full_decode(deint, bits, &crc_ok, &mismatches) != TETRA_SPEECH_FULL_BITS) {
        return 0;
    }
    const int good = crc_ok && mismatches <= TETRA_TCH_MAX_MISMATCH;
    if (good) {
        ts->decode_ok++;
    }
    TETRA_LOG_PAYLOAD(ts, "[TETRA TCH/FS] CC=%d  dec_bits=%d  crc=%s  mismatches=%d\n", cc, TETRA_SPEECH_FULL_BITS,
                      crc_ok ? "ok" : "bad", mismatches);
    tetra_voice_full_slot(opts, state, bits, good);
    return good;
}

/* Half-slot speech (EN 300 395-2 clause 5.6) in the second half of an NTS2 burst whose SCH/HD CRC failed. The 4-bit
   CRC alone passes one random block in 16, so the re-encode check carries more weight here. Returns 1 for a good
   frame. */
static int
tetra_decode_tch_half(const uint16_t* soft_block, dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return 0;
    }
    uint16_t deint[NB_BLOCK_BITS];
    uint8_t bits[TETRA_SPEECH_HALF_BITS];
    int crc_ok = 0;
    int mismatches = -1;

    tetra_block_deinterleave_soft(soft_block, deint, NB_BLOCK_BITS, HD_A);
    if (tetra_speech_half_decode(deint, bits, &crc_ok, &mismatches) != TETRA_SPEECH_HALF_BITS) {
        return 0;
    }
    const int good = crc_ok && mismatches <= TETRA_HALF_MAX_MISMATCH;
    if (good) {
        ts->decode_ok++;
        DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " %-13s", "STCH+TCH");
    }
    TETRA_LOG_PAYLOAD(ts, "[TETRA TCH/H] crc=%s  mismatches=%d\n", crc_ok ? "ok" : "bad", mismatches);
    tetra_voice_half_slot(opts, state, bits, good);
    return good;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Normal bursts: downlink NDB, direct-mode DNB and uplink NUB share their logical channels.
 * ------------------------------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t seed;      /* the extended colour code the burst is scrambled with */
    int cc;             /* its colour code, which the MAC keeps fragments of different cells apart by */
    int direct;         /* direct mode: signalling goes to the DMAC logger */
    const char* prefix; /* "", "DNB " or "NUB ": the burst kind shown in the frame subtype */
} tetra_nb_ctx;

static void
tetra_nb_subtype(dsd_state* state, const tetra_nb_ctx* ctx, const char* what) {
    char text[16];
    DSD_SNPRINTF(text, sizeof(text), "%s%s", ctx->prefix, what);
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " %-13s", text);
}

/* A CRC-passed signalling block: the MAC in trunked mode, the DMAC logger in direct mode. It counts as evidence about
   the carrier once it has been acted on, so a grant it carries is judged by the blocks before it. A trunked downlink MAC
   block proves a control channel, an uplink one the carrier for publication only (tetra_confirm.h), and a direct mode
   one the burst. */
static void
tetra_nb_signalling(dsd_opts* opts, dsd_state* state, const tetra_nb_ctx* ctx, const uint8_t* decoded, int len,
                    const char* kind) {
    if (ctx->direct) {
        (void)tetra_dmo_log_dnb_pdu(decoded, len, kind, opts, state);
        tetra_confirm_note_block(state);
        return;
    }
    tetra_mac_parse_schd(decoded, len, ctx->cc, opts, state);
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (ts && ts->uplink) {
        tetra_confirm_note_uplink_signalling(state);
    } else {
        tetra_confirm_note_signalling(state);
    }
}

/* A block that failed its signalling CRC. In trunked mode it may be an SCH block of its timeslot's control channel, so
   the MAC drops the partial TM-SDU it was rebuilding there (EN 300 392-2 clause 23.4.3.1.1 ii). */
static void
tetra_nb_block_failed(dsd_state* state, const tetra_nb_ctx* ctx) {
    if (!ctx->direct) {
        tetra_mac_block_failed(state);
    }
}

/* One half-slot block of an NTS2 burst: 1 when it was signalling. */
static int
tetra_nb_half(dsd_opts* opts, dsd_state* state, const tetra_nb_ctx* ctx, uint16_t* soft, const char* kind) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return 0;
    }
    uint8_t decoded[HD_TYPE2];
    /* Each half-slot restarts the scrambling sequence. */
    tetra_descramble_soft(soft, NB_BLOCK_BITS, ctx->seed);
    if (!tetra_hd_decode_bits(soft, decoded)) {
        tetra_nb_block_failed(state, ctx);
        return 0;
    }
    ts->decode_ok++;
    tetra_nb_signalling(opts, state, ctx, decoded, HD_TYPE1, kind);
    return 1;
}

/* NTS2: two half-slot channels. The second half that is not signalling may be speech after a stolen first half, but
   not in frame 18, which carries signalling only (tetra_tdma_control_frame()). A first half that moved the receiver (a
   grant) leaves the second half to the carrier that was left. */
static void
tetra_nb_nts2(dsd_opts* opts, dsd_state* state, const tetra_nb_ctx* ctx, const uint8_t* b2_dibits,
              const float* b2_soft) {
    uint16_t block[NB_BLOCK_BITS];
    if (state->tetra_sync.b1_valid) {
        tetra_expand_block1(state, block);
        (void)tetra_nb_half(opts, state, ctx, block, "STCH-1");
        if (!tetra_confirm_burst_current(state)) {
            return;
        }
    }
    tetra_expand_dibits(state, b2_dibits, b2_soft, NB_BLOCK_DIBITS, block);
    if (!tetra_nb_half(opts, state, ctx, block, "STCH-2") && !tetra_tdma_control_frame(tetra_st_peek(state))
        && tetra_decode_tch_half(block, opts, state)) {
        tetra_confirm_note_block(state);
    }
}

/* NTS1: one 432-bit channel scrambled across both blocks (EN 300 392-2 clause 8.2.5; EN 300 396-2 clause 8.3.2.4).
   SCH/F when its CRC passes, else full-slot speech, but not in frame 18, which carries signalling only
   (tetra_tdma_control_frame()). */
static void
tetra_nb_nts1(dsd_opts* opts, dsd_state* state, const tetra_nb_ctx* ctx, const uint8_t* b2_dibits,
              const float* b2_soft) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    uint16_t type4[SCHF_BITS];
    uint8_t decoded[SCHF_TYPE2];
    tetra_expand_block1(state, type4);
    tetra_expand_dibits(state, b2_dibits, b2_soft, NB_BLOCK_DIBITS, type4 + NB_BLOCK_BITS);
    tetra_descramble_soft(type4, SCHF_BITS, ctx->seed);
    if (!tetra_schf_decode_bits(type4, decoded)) {
        tetra_nb_block_failed(state, ctx);
        if (!tetra_tdma_control_frame(ts) && tetra_decode_tch_fs(type4, ctx->cc, opts, state)) {
            tetra_confirm_note_block(state);
        }
        return;
    }
    ts->decode_ok++;
    tetra_nb_subtype(state, ctx, "SCH/F");
    TETRA_LOG_PAYLOAD(ts, "[TETRA SCH/F] tn=%u CRC OK\n", (unsigned)ts->tn);
    tetra_nb_signalling(opts, state, ctx, decoded, SCHF_TYPE1, "SCH/F");
}

/* Both kinds of normal burst, once block 2 is read. NTS1 needs block 1 from the frame sync. A burst the state machine's
   settle at its entry left on the carrier the receiver moved from is not decoded (tetra_burst_enter()). */
static void
tetra_nb_decode(dsd_opts* opts, dsd_state* state, const tetra_nb_ctx* ctx, const uint8_t* b2_dibits,
                const float* b2_soft) {
    if (!tetra_confirm_burst_current(state)) {
        return;
    }
    if (state->tetra_sync.nts2) {
        tetra_nb_nts2(opts, state, ctx, b2_dibits, b2_soft);
    } else if (state->tetra_sync.b1_valid) {
        tetra_nb_nts1(opts, state, ctx, b2_dibits, b2_soft);
    }
}

/* The end of every burst: the frontends and the event watchdog. The engine ticks the trunking state machine. */
static void
tetra_burst_done(dsd_opts* opts, dsd_state* state) {
    if (dsd_opts_frontend_active(opts)) {
        dsd_telemetry_publish_both_and_redraw(opts, state);
    }
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
}

static void
tetra_block1_consumed(dsd_state* state) {
    state->tetra_sync.b1_valid = 0;
    state->tetra_sync.b1_soft_valid = 0;
}

/* The uplink scrambles with the cell's colour code, which only a BSCH or --tetra-net supplies. */
static int
tetra_uplink_net_known(tetra_state_t* ts) {
    if (ts->net_known) {
        return 1;
    }
    if (!ts->uplink_no_net_noted) {
        ts->uplink_no_net_noted = 1;
        LOG_WARN("TETRA uplink: no colour code to descramble with yet; it comes from a downlink BSCH or from "
                 "--tetra-net mcc,mnc,colour\n");
    }
    return 0;
}

/* Direct-mode normal burst (EN 300 396-2 clause 9.4.3.2.1): no AACH, so block 1 sits against the training sequence
   and block 2 follows it. Everything but the DSB is scrambled with the DM colour code tetra_dmo_apply_sync() derived
   from the call's DMAC-SYNC. */
static void
tetra_dmo_process_ndb(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    uint8_t b2_dibits[NB_BLOCK_DIBITS];
    float b2_soft[NB_BLOCK_DIBITS];
    const float* const soft = tetra_read_block2(opts, state, b2_dibits, b2_soft);

    const tetra_nb_ctx ctx = {ts->dmo_dcc_seed, 0, 1, "DNB "};
    tetra_nb_subtype(state, &ctx, state->tetra_sync.nts2 ? "NTS2" : "NTS1");
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), " TETRA DMO");

    if (ts->dmo_since_sync < 0xFFFFu) {
        ts->dmo_since_sync++;
    }
    if (ts->dmo_dcc_valid && ts->dmo_since_sync > TETRA_DMO_DCC_STALE_BURSTS) {
        ts->dmo_dcc_valid = 0;
    }
    TETRA_LOG_PAYLOAD(ts, " [TETRA DNB  NTS%d  pol=%d rot=%d dcc=%d]", state->tetra_sync.nts2 ? 2 : 1,
                      (int)state->tetra_sync.polarity, (int)(state->tetra_sync.phase_rot & 3), (int)ts->dmo_dcc_valid);

    if (ts->dmo_dcc_valid && state->tetra_sync.b1_valid) {
        tetra_nb_decode(opts, state, &ctx, b2_dibits, soft);
    }
    tetra_block1_consumed(state);
    tetra_burst_done(opts, state);
}

/* Normal uplink burst (EN 300 392-2 clause 9.4.4.2.4): a downlink NDB's logical channels and colour code, without the
   AACH between the blocks. */
static void
tetra_uplink_process_nub(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    uint8_t b2_dibits[NB_BLOCK_DIBITS];
    float b2_soft[NB_BLOCK_DIBITS];
    const float* const soft = tetra_read_block2(opts, state, b2_dibits, b2_soft);

    const tetra_nb_ctx ctx = {tetra_lfsr_seed_for(state), tetra_burst_colour(state), 0, "NUB "};
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), " TETRA UL");
    tetra_nb_subtype(state, &ctx, state->tetra_sync.nts2 ? "NTS2" : "NTS1");
    TETRA_LOG_PAYLOAD(ts, " [TETRA NUB  NTS%d  pol=%d rot=%d]", state->tetra_sync.nts2 ? 2 : 1,
                      (int)state->tetra_sync.polarity, (int)(state->tetra_sync.phase_rot & 3));

    if (tetra_uplink_net_known(ts)) {
        tetra_nb_decode(opts, state, &ctx, b2_dibits, soft);
    }
    tetra_block1_consumed(state);
    tetra_burst_done(opts, state);
}

/* Control uplink burst: its 168 bits are one SCH/HU block (EN 300 392-2 clause 8.3.1.4.3). Its extended training
   sequence (clause 9.4.4.3.3) is no other training sequence inverted, so the polarity it matched, which the frame sync
   gave as the sign of its synctype, is the burst's: the block is decoded with it, and once its CRC passed it is the
   carrier's for the normal bursts after it, whose NTS1 and NTS2 cannot tell it (and the uplink has no synchronisation
   burst to). A block that fails keeps the carrier's polarity: a normal-polarity NTS2 burst can match the inverted
   extended training sequence. An SCH/HU whose CRC passed also proves a trunked mode uplink, which no BSCH comes to
   say: direct mode, which a DSB set before, ends (tetra_dmo_leave()), so the normal bursts after it are decoded as
   NUBs again and not with a DM colour code. */
static void
tetra_uplink_process_cb(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    uint8_t b2_dibits[CB_HALF_DIBITS];
    float b2_soft[CB_HALF_DIBITS];
    uint16_t type4[CB_BITS];
    uint8_t mac[SCHHU_TYPE1];
    tetra_read_dibits(opts, state, b2_dibits, b2_soft, CB_HALF_DIBITS);

    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), " TETRA UL");
    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " %-13s", "CB SCH/HU");
    if (!tetra_uplink_net_known(ts) || !state->tetra_sync.b1_valid) {
        return;
    }
    if (!tetra_confirm_burst_current(state)) {
        /* The settle at the burst's entry moved the receiver (tetra_burst_enter()): the burst is not decoded. */
        tetra_block1_consumed(state);
        return;
    }
    const int inverted = DSD_SYNC_IS_INVERTED(state->lastsynctype) ? 1 : 0;
    tetra_expand_dibits_as(state, inverted, state->tetra_sync.b1_dibuf,
                           state->tetra_sync.b1_soft_valid ? state->tetra_sync.b1_soft : NULL, CB_HALF_DIBITS, type4);
    tetra_expand_dibits_as(state, inverted, b2_dibits, b2_soft, CB_HALF_DIBITS, type4 + ((size_t)CB_HALF_DIBITS * 2U));
    tetra_descramble_soft(type4, CB_BITS, ts->lfsr_seed);
    tetra_block1_consumed(state);

    if (!tetra_sch_hu_decode(type4, mac)) {
        return;
    }
    state->tetra_sync.polarity = (uint8_t)inverted;
    ts->decode_ok++;
    tetra_confirm_note_uplink_signalling(state);
    tetra_dmo_leave(opts, state);
    /* MAC-ACCESS (EN 300 392-2 clause 21.4.2.1): PDU type 0, fill bit, encryption mode, address type and address. */
    tetra_bit_reader r = tetra_bits_reader(mac, SCHHU_TYPE1, 0);
    if (tetra_bits_read(&r, 1) != 0) {
        DSD_FPRINTF(stderr, "[TETRA SCH/HU] MAC-END-HU\n");
        return;
    }
    (void)tetra_bits_skip(&r, 1);
    const uint32_t encrypted = tetra_bits_read(&r, 1);
    const uint32_t addr_type = tetra_bits_read(&r, 2);
    const uint32_t addr = tetra_bits_read(&r, addr_type == 1u ? 10 : 24);
    if (tetra_bits_ok(&r)) {
        DSD_FPRINTF(stderr, "[TETRA SCH/HU] MAC-ACCESS enc=%u addr=%u\n", (unsigned)encrypted, (unsigned)addr);
    }
}

void
tetra_tdma_advance_ndb(dsd_state* state) {
    /* Called straight after the burst's training sequence: its slot started TETRA_TDMA_NDB_SYNC_SYMBOL symbols ago
     * (EN 300 392-2 table 9.7). */
    if (state) {
        tetra_tdma_follow(state, state->symbolcnt - TETRA_TDMA_NDB_SYNC_SYMBOL);
    }
}

/* A normal burst: control uplink, normal uplink, direct mode, or the trunked downlink NDB. Block 1 was captured by the
   frame sync; the downlink reads AACH half 2 and block 2 here. The AACH is not decoded; the blocks are descrambled with
   the network's extended colour code (from the BSCH or --tetra-net), whose colour code the MAC is told. */
static void
tetra_process_ndb(dsd_opts* opts, dsd_state* state) {
    const tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    soft_symbol_frame_begin(state);
    /* Every TETRA lock sets the burst kind, and the publication reads it after the burst (tetra_pub_mode()). */
    if (state->tetra_sync.cb) {
        tetra_uplink_process_cb(opts, state);
        return;
    }
    if (ts->uplink && !ts->dmo) {
        tetra_uplink_process_nub(opts, state);
        return;
    }
    if (ts->dmo) {
        tetra_dmo_process_ndb(opts, state);
        return;
    }
    tetra_tdma_advance_ndb(state);

    uint8_t b2_dibits[NB_BLOCK_DIBITS];
    float b2_soft[NB_BLOCK_DIBITS];
    skipDibit(opts, state, NDB_AACH2_DIBITS);
    tetra_read_dibits(opts, state, b2_dibits, b2_soft, NB_BLOCK_DIBITS);

    const tetra_nb_ctx ctx = {tetra_lfsr_seed_for(state), tetra_burst_colour(state), 0, ""};
    tetra_nb_subtype(state, &ctx, state->tetra_sync.nts2 ? "NTS2" : "NTS1");
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), " TETRA");
    TETRA_LOG_PAYLOAD(ts, " [TETRA NDB  NTS%d  pol=%d rot=%d]", state->tetra_sync.nts2 ? 2 : 1,
                      (int)state->tetra_sync.polarity, (int)(state->tetra_sync.phase_rot & 3));

    tetra_nb_decode(opts, state, &ctx, b2_dibits, b2_soft);
    tetra_block1_consumed(state);
    tetra_burst_done(opts, state);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Synchronisation bursts: the trunked BSCH and the direct-mode DSB.
 * ------------------------------------------------------------------------------------------------------------- */

/* A DSB's SCH/H (EN 300 396-3 clause 9.1): its DMAC-SYNC message and addresses on one line, and the call it describes
 * to the call records (tetra_calls_dmo_pdu()). */
static void
tetra_dmo_log_sync(dsd_opts* opts, const tetra_state_t* ts, const tetra_dmo_sync_s_t* s, const tetra_dmo_sync_h_t* h,
                   int h_ok, dsd_state* state) {
    if (h_ok) {
        tetra_calls_dmo_pdu(opts, state, s->enc_state, h);
    }
    char line[384];
    int n = DSD_SNPRINTF(line, sizeof(line), "[TETRA DMO DSB] sys=%u comm=%u ch=%u slot=%u frame=%u enc=%u",
                         s->system_code, s->comm_type, s->ab_usage, s->slot, s->frame, s->enc_state);
    if (h_ok && h->has_relay && n > 0 && (size_t)n < sizeof(line)) {
        n += DSD_SNPRINTF(line + n, sizeof(line) - (size_t)n, " rep=%u %s", h->relay_addr,
                          s->ms_link_flag ? "master" : "slave");
    }
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        DSD_FPRINTF(stderr, "%s\n", line);
        return;
    }
    if (h_ok && h->has_addrs) {
        char desc[160];
        (void)tetra_dmo_describe_elems(h, desc, (int)sizeof(desc));
        DSD_SNPRINTF(line + n, sizeof(line) - (size_t)n, " | %s src=%u dst=%u MNI=%u/%u cd=%u%s%s%s",
                     tetra_dmo_msg_name(h->msg_type), h->src, h->dst, (h->mni >> 14) & 0x3FFu, h->mni & 0x3FFFu,
                     h->frame_countdown, ts->dmo_dcc_valid ? " dcc=ok" : "", desc[0] ? " | " : "", desc);
    } else {
        DSD_SNPRINTF(line + n, sizeof(line) - (size_t)n, " | SCH/H %s", h_ok ? "ok" : "CRC fail");
    }
    DSD_FPRINTF(stderr, "%s\n", line);
}

/* A DM-REP presence signal: its SCH/H is not a DMAC-SYNC, so it must not set a call colour code. */
static void
tetra_dmo_presence(dsd_state* state, tetra_dmo_sync_s_t* s, const uint8_t* sch_s, const uint8_t* sch_h) {
    tetra_dmo_pres_t pres;
    if (tetra_dmo_parse_presence(sch_s, sch_h, s, &pres)) {
        char desc[120];
        tetra_dmo_apply_sync(state, s, NULL);
        (void)tetra_dmo_describe_presence(&pres, desc, (int)sizeof(desc));
        DSD_FPRINTF(stderr, "[TETRA DMO PRES] sys=%u slot=%u frame=%u | %s\n", s->system_code, s->slot, s->frame, desc);
    } else {
        /* Direct mode all the same (managed direct mode, EN 300 396-10, is not read): the TMO clock stops. */
        tetra_dmo_apply_sync(state, s, NULL);
        DSD_FPRINTF(stderr, "[TETRA DMO PRES] sys=%u comm=%u | presence layout not parsed\n", s->system_code,
                    s->comm_type);
    }
}

/* A DSB says the carrier is a direct mode one (EN 300 396-2 clause 9.4.3.3). Entering direct mode from trunked mode,
   what the trunked carrier left goes, as at a BSCH of another cell: its partial TM-SDUs, what its timeslots' speech
   left with the heard or anonymous canonical call (tetra_calls_forget_trunked_carrier()), and the followed call's proof
   on the carrier; tetra_dmo_apply_sync() stops the TDMA clock. */
static void
tetra_enter_dmo(dsd_opts* opts, dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts || ts->dmo) {
        return;
    }
    tetra_mac_forget_fragments(state);
    tetra_calls_forget_trunked_carrier(opts, state);
    tetra_confirm_forget_followed_call(state);
}

/* Direct-mode synchronisation burst whose SCH/S (@p sch_s, CRC passed) carries a DMO system code. Its SCH/H follows
   the training sequence at once, with no broadcast block as in a trunked SB; it is read here and the broadcast
   block's 15 dibits skipped after it, so the burst consumes as many dibits as a trunked one. */
static void
tetra_dmo_process_dsb(dsd_opts* opts, dsd_state* state, const uint8_t* sch_s) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    tetra_enter_dmo(opts, state);
    uint8_t b2_dibits[SB_BKN2_DIBITS];
    float b2_soft[SB_BKN2_DIBITS];
    uint16_t b2_costs[NB_BLOCK_BITS];
    uint8_t sch_h[HD_TYPE2];
    tetra_dmo_sync_s_t s;
    tetra_dmo_sync_h_t h;

    tetra_read_dibits(opts, state, b2_dibits, b2_soft, SB_BKN2_DIBITS);
    skipDibit(opts, state, SB_BB_DIBITS);
    (void)tetra_dmo_parse_sync_s(sch_s, &s);

    tetra_expand_dibits(state, b2_dibits, b2_soft, SB_BKN2_DIBITS, b2_costs);
    tetra_descramble_soft(b2_costs, NB_BLOCK_BITS, TETRA_SB_SCRAMB_SEED);
    const int h_ok = tetra_hd_decode_bits(b2_costs, sch_h);
    if (h_ok) {
        ts->decode_ok++;
        tetra_confirm_note_block(state);
    }

    if (s.sync_pdu_type == TETRA_DMO_SYNC_PRES) {
        tetra_dmo_presence(state, &s, sch_s, h_ok ? sch_h : NULL);
    } else if (s.sync_pdu_type == TETRA_DMO_SYNC_DMAC) {
        if (h_ok) {
            (void)tetra_dmo_parse_sync_h(sch_h, s.comm_type, &h);
        }
        tetra_dmo_apply_sync(state, &s, h_ok ? &h : NULL);
        tetra_dmo_log_sync(opts, ts, &s, &h, h_ok, state);
        tetra_calls_dmo_protection(opts, state);
    } else {
        /* A reserved SYNC PDU type (EN 300 396-3 clause 9.3.28): its SCH/H has no layout to read, so nothing of it is
         * taken for a DMAC-SYNC's call or protection. */
        tetra_dmo_apply_sync(state, &s, NULL);
        DSD_FPRINTF(stderr, "[TETRA DMO DSB] sys=%u reserved SYNC PDU type %u\n", s.system_code, s.sync_pdu_type);
    }

    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " %-13s", "DSB");
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), " TETRA DMO");
    tetra_burst_done(opts, state);
}

/* The captured BSCH (or direct-mode SCH/S) block to SB_TYPE2 bits in @p decoded: descramble, deinterleave with
   a = 11, depuncture and Viterbi. With @p use_soft 0 only the captured hard dibits are used, which no live slicer
   threshold influences. 1 when the CRC-16 passes. */
static int
tetra_sb_decode_block(const dsd_state* state, int use_soft, uint8_t* decoded) {
    uint16_t costs[SB_BSCH_BITS];
    uint16_t deint[SB_BSCH_BITS];
    uint16_t depunc[SB_TYPE2 * 4];
    const float* soft = (use_soft && state->tetra_sync.sb1_soft_valid) ? state->tetra_sync.sb1_soft : NULL;
    tetra_expand_dibits(state, state->tetra_sync.sb1_dibuf, soft, SB_BSCH_DIBITS, costs);
    tetra_descramble_soft(costs, SB_BSCH_BITS, TETRA_SB_SCRAMB_SEED);
    tetra_block_deinterleave_soft(costs, deint, SB_BSCH_BITS, SB_BSCH_A);
    (void)tetra_rcpc_depuncture_by_id(TETRA_RCPC_PUNCT_2_3, deint, SB_BSCH_BITS, depunc, SB_TYPE2 * 4);
    DSD_MEMSET(decoded, 0, SB_TYPE2);
    return tetra_viterbi_decode_soft(depunc, SB_TYPE2 * 4, decoded, SB_TYPE2) == SB_TYPE2
           && tetra_crc16_ccitt_bits(decoded, SB_TYPE1 + 16) == TETRA_CRC16_OK;
}

static void
tetra_sb_log_bits(const dsd_state* state, const tetra_state_t* ts, const uint8_t* decoded) {
    if (!ts || !ts->log_payload) {
        return;
    }
    char first[13];
    for (int i = 0; i < 12; i++) {
        first[i] = (char)('0' + (decoded[i] & 1u));
    }
    first[12] = '\0';
    TETRA_LOG_PAYLOAD(ts, " [TETRA SB  rot=%d first12=%s]", (int)(state->tetra_sync.phase_rot & 3), first);
}

/* A direct-mode synchronisation burst is told from a trunked one by its SCH/S system code (EN 300 396-3 clause
   9.3.29), decoded from the hard decisions first: they do not consume symbols or depend on the slicer, so a trunked
   burst then takes exactly the path it would have taken. 1 when the burst was a DSB and is done. */
static int
tetra_sb_try_dmo(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return 0;
    }
    uint8_t pre[SB_TYPE2];
    if (!tetra_sb_decode_block(state, 0, pre) || !tetra_dmo_system_code_is_dmo(pre)) {
        return 0;
    }
    state->tetra_sync.sb1_soft_valid = 0;
    ts->decode_ok++;
    tetra_confirm_note_block(state);
    tetra_sb_log_bits(state, ts, pre);
    tetra_dmo_process_dsb(opts, state, pre);
    return 1;
}

/* The BSCH of a trunked synchronisation burst, read once the rest of the burst has been: 1 when its CRC passed. A DMO
   system code that only the soft decision decoded is a DSB whose SCH/H went by unread; timing and mode come from its
   SCH/S. A BSCH that failed its CRC still sits in the slot after the last burst's: the TDMA clock runs to it on the
   symbol count, as for a normal burst, so block 2 is decoded in its own timeslot rather than the last burst's. */
static int
tetra_sb_bsch(dsd_opts* opts, dsd_state* state, tetra_state_t* ts) {
    uint8_t decoded[SB_TYPE2];
    const int crc_ok = tetra_sb_decode_block(state, 1, decoded);
    state->tetra_sync.sb1_soft_valid = 0;
    tetra_sb_log_bits(state, ts, decoded);
    if (!crc_ok) {
        tetra_tdma_follow(state, state->symbolcnt - TETRA_TDMA_SB_PARSE_SYMBOL);
        return 0;
    }
    ts->decode_ok++;
    if (tetra_dmo_system_code_is_dmo(decoded)) {
        tetra_dmo_sync_s_t s;
        (void)tetra_dmo_parse_sync_s(decoded, &s);
        tetra_enter_dmo(opts, state);
        tetra_dmo_apply_sync(state, &s, NULL);
        tetra_calls_dmo_protection(opts, state);
        tetra_confirm_note_block(state);
        DSD_FPRINTF(stderr, "[TETRA DMO DSB] sys=%u comm=%u ch=%u slot=%u frame=%u enc=%u | SCH/H skipped\n",
                    s.system_code, s.comm_type, s.ab_usage, s.slot, s.frame, s.enc_state);
        return 1;
    }
    tetra_dmo_leave(opts, state);
    (void)tetra_bsch_parse(decoded, SB_TYPE1, opts, state);
    tetra_confirm_note_signalling(state);
    return 1;
}

/* Block 2 of a trunked synchronisation burst: the BNCH (EN 300 392-2 clause 9.5.2) or an SCH/HD, coded like an SCH/HD
   (clause 8.3.1.4.1) and scrambled with the network's extended colour code (clause 8.2.5.2), which the BSCH in block 1
   may just have set. Without a network identity there is nothing to descramble it with. */
static void
tetra_sb_block2(dsd_opts* opts, dsd_state* state, const uint8_t* dibits, const float* soft) {
    const tetra_state_t* const ts = tetra_st(state);
    if (!ts || !ts->net_known || ts->dmo) {
        return;
    }
    uint16_t block[NB_BLOCK_BITS];
    const tetra_nb_ctx ctx = {tetra_lfsr_seed_for(state), tetra_burst_colour(state), 0, ""};
    tetra_expand_dibits(state, dibits, soft, SB_BKN2_DIBITS, block);
    (void)tetra_nb_half(opts, state, &ctx, block, "BNCH");
}

/* A synchronisation burst (EN 300 392-2 clause 9.4.4.2.6): the BSCH block before the training sequence, which the frame
   sync captured, then the broadcast block (the AACH, not decoded) and block 2. Block 2 is read before the BSCH is
   parsed, so tetra_bsch_parse() sees the symbol count of the burst's end it expects. */
static void
tetra_process_sb(dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return;
    }
    soft_symbol_frame_begin(state);
    uint8_t b2_dibits[SB_BKN2_DIBITS];
    float b2_soft[SB_BKN2_DIBITS];
    if (!state->tetra_sync.sb1_valid) {
        skipDibit(opts, state, SB_BB_DIBITS);
        skipDibit(opts, state, SB_BKN2_DIBITS);
        TETRA_LOG_PAYLOAD(ts, "[TETRA SB] no BSCH block captured\n");
        state->tetra_sync.sb1_soft_valid = 0;
        return;
    }
    if (!tetra_confirm_burst_current(state)) {
        /* The settle at the burst's entry moved the receiver (tetra_burst_enter()): the burst's symbols go by
         * undecoded, its BSCH block with them. */
        skipDibit(opts, state, SB_BB_DIBITS);
        skipDibit(opts, state, SB_BKN2_DIBITS);
        state->tetra_sync.sb1_valid = 0;
        state->tetra_sync.sb1_soft_valid = 0;
        return;
    }
    state->tetra_sync.sb1_valid = 0;
    if (tetra_sb_try_dmo(opts, state)) {
        return;
    }
    skipDibit(opts, state, SB_BB_DIBITS);
    tetra_read_dibits(opts, state, b2_dibits, b2_soft, SB_BKN2_DIBITS);
    (void)tetra_sb_bsch(opts, state, ts);
    tetra_sb_block2(opts, state, b2_dibits, b2_soft);

    DSD_SNPRINTF(state->fsubtype, sizeof(state->fsubtype), " %-13s", "BSCH");
    DSD_SNPRINTF(state->ftype, sizeof(state->ftype), " TETRA");
    tetra_burst_done(opts, state);
}

/* The TETRA state this burst works on, with the command-line uplink setting and network identity applied; NULL
 * when it cannot be allocated. A network identity a BSCH has already supplied is kept. */
static tetra_state_t*
tetra_frame_enter(const dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts) {
        return NULL;
    }
    ts->uplink = (uint8_t)(opts->tetra_uplink != 0);
    ts->log_payload = (uint8_t)(opts->payload == 1);
    int verbose = opts->verbose > 0 ? opts->verbose : 0;
    if (verbose > 255) {
        verbose = 255;
    }
    ts->log_verbose = (uint8_t)verbose;
    if (opts->tetra_net_preset && !ts->net_known) {
        ts->mcc = opts->tetra_net_mcc;
        ts->mnc = opts->tetra_net_mnc;
        ts->colour = opts->tetra_net_colour;
        ts->lfsr_seed = tetra_compute_scramb_seed(ts->mcc, ts->mnc, ts->colour);
        ts->net_known = 1;
    }
    return ts;
}

/* The working mode of the burst just taken. The DMO communication type is the DMAC-SYNC field of EN 300 396-3:
 * 0 direct, 1 through a DM-REP, 2 through a DM-GATE, 3 through a DM-REP/GATE. */
static uint8_t
tetra_pub_mode(const dsd_state* state, const tetra_state_t* ts) {
    if (ts->dmo) {
        static const uint8_t dmo_modes[4] = {
            DSD_TETRA_MODE_DMO_DIRECT,
            DSD_TETRA_MODE_DMO_REPEATER,
            DSD_TETRA_MODE_DMO_GATEWAY,
            DSD_TETRA_MODE_DMO_REPEATER_GATEWAY,
        };
        return dmo_modes[ts->dmo_comm_type & 3u];
    }
    if (ts->uplink) {
        return state->tetra_sync.cb ? DSD_TETRA_MODE_TMO_CONTROL_UPLINK : DSD_TETRA_MODE_TMO_UPLINK;
    }
    return DSD_TETRA_MODE_TMO_DOWNLINK;
}

/* Air-interface encryption: in direct mode the security class of the current call (DM-2-A/B/C encrypt the air
 * interface; DM-1, with or without end-to-end encryption, leaves it clear); in trunked mode the encryption mode element
 * of the last addressed MAC-RESOURCE (EN 300 392-7 clause 6.5.1: 0 clear, 2 and 3 encrypted, 1 reserved and so
 * unknown), once one has been decoded. */
static uint8_t
tetra_pub_air_encryption(const tetra_state_t* ts) {
    if (ts->dmo) {
        switch ((tetra_dmo_crypt_e)ts->dmo_crypt) {
            case TETRA_DMO_CRYPT_AIR: return DSD_TETRA_AIR_ENC_ENCRYPTED;
            case TETRA_DMO_CRYPT_CLEAR:
            case TETRA_DMO_CRYPT_E2E: return DSD_TETRA_AIR_ENC_CLEAR;
            case TETRA_DMO_CRYPT_UNKNOWN:
            default: return DSD_TETRA_AIR_ENC_UNKNOWN;
        }
    }
    if (tetra_enc_mode_encrypted(ts->enc_mode)) {
        return DSD_TETRA_AIR_ENC_ENCRYPTED;
    }
    return (ts->frames_resource != 0 && ts->enc_mode == TETRA_ENC_MODE_NONE) ? DSD_TETRA_AIR_ENC_CLEAR
                                                                             : DSD_TETRA_AIR_ENC_UNKNOWN;
}

/* The codec library: not needed yet, not configured, refused, or loaded (and whether its last frame failed). */
static uint8_t
tetra_pub_codec_status(const tetra_voice_t* voice) {
    switch ((tetra_codec_status)voice->codec_status) {
        case TETRA_CODEC_STATUS_READY:
            return voice->last_decode_failed ? DSD_TETRA_CODEC_DECODE_ERRORS : DSD_TETRA_CODEC_READY;
        case TETRA_CODEC_STATUS_NONE: return DSD_TETRA_CODEC_MISSING;
        case TETRA_CODEC_STATUS_FAILED: return DSD_TETRA_CODEC_LOAD_FAILED;
        case TETRA_CODEC_STATUS_UNLOADED:
        default: return DSD_TETRA_CODEC_UNKNOWN;
    }
}

/* A downlink frequency the publication can carry: 1..DSD_TRUNK_FREQ_MAX_HZ, else 0 (not resolved). */
static uint32_t
tetra_pub_hz(long hz) {
    return (hz > 0L && hz <= DSD_TRUNK_FREQ_MAX_HZ) ? (uint32_t)hz : 0U;
}

/* The trunked network identity and its air-interface encryption: the network's TEA comes from the KSG an OTAR named
 * for it. A direct mode carrier belongs to no trunked network (EN 300 396-3: its colour code is the source's, clause
 * 8.2.4), so the identity the session keeps from the last BSCH or --tetra-net is not published there. */
static void
tetra_pub_network(const dsd_state* state, const tetra_state_t* ts, dsd_tetra_publication* pub) {
    const int trunked = ts->net_known && !ts->dmo;
    pub->net_known = (uint8_t)trunked;
    pub->mcc = trunked ? ts->mcc : 0U;
    pub->mnc = trunked ? ts->mnc : 0U;
    pub->colour = trunked ? (uint8_t)(ts->colour & 0x3Fu) : 0U;
    pub->air_encryption = tetra_pub_air_encryption(ts);
    const int ksg = trunked ? tetra_air_ksg_lookup(state, ts->mcc, ts->mnc) : -1;
    pub->tea = (ksg >= 0 && ksg < DSD_TETRA_TEA_MAX) ? (uint8_t)(ksg + 1) : 0U;
}

/* Call following, the SYSINFO main carrier and the traffic channel: the followed call's, else the channel last
 * allocated to a call still on record (tetra_calls_traffic()). */
static uint8_t
tetra_pub_trunk_state(const tetra_state_t* ts) {
    switch ((tetra_sm_state_e)ts->sm.state) {
        case TETRA_SM_TUNED: return DSD_TETRA_TRUNK_ON_TRAFFIC;
        case TETRA_SM_ON_CC: return DSD_TETRA_TRUNK_ON_CONTROL;
        case TETRA_SM_IDLE:
        default: return DSD_TETRA_TRUNK_IDLE;
    }
}

/* The cell's control channel and the traffic channel of its calls are trunked mode channels: a direct mode carrier
 * publishes neither. */
static void
tetra_pub_channels(const dsd_state* state, const tetra_state_t* ts, dsd_tetra_publication* pub) {
    pub->trunk_state = tetra_pub_trunk_state(ts);
    pub->cc_known = (uint8_t)(ts->frames_sysinfo != 0 && !ts->dmo);
    pub->cc_carrier = pub->cc_known ? (uint16_t)(ts->sysinfo_main_carrier & 0xFFFu) : 0U;
    pub->cc_freq_hz = pub->cc_known ? tetra_pub_hz(ts->dl_carrier_hz) : 0U;
    tetra_chan_alloc traffic = {0};
    pub->traffic_valid = (uint8_t)(!ts->dmo && tetra_calls_traffic(state, &traffic));
    pub->traffic_carrier = pub->traffic_valid ? (uint16_t)(traffic.carrier & 0xFFFu) : 0U;
    pub->traffic_slots = pub->traffic_valid ? (uint8_t)(traffic.timeslots & 0x0Fu) : 0U;
    pub->traffic_freq_hz = pub->traffic_valid ? tetra_pub_hz(traffic.freq_hz) : 0U;
}

void
tetra_publish(dsd_state* state) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (!ts) {
        return;
    }
    dsd_tetra_publication* const pub = &state->tetra_pub;
    /* Nothing reads as decoded until a block has passed its CRC: a sync word alone is weak enough for noise. */
    pub->mode = ts->decode_ok != 0 ? tetra_pub_mode(state, ts) : DSD_TETRA_MODE_NONE;
    tetra_pub_network(state, ts, pub);
    tetra_pub_channels(state, ts, pub);
    /* The D-MM STATUS is the trunked network's to its MSs (EN 300 392-2 clause 16.9.2.5). */
    pub->mm_status_valid = (uint8_t)(ts->mm_status_valid != 0 && !ts->dmo);
    pub->mm_status_code = pub->mm_status_valid ? (uint8_t)(ts->mm_status_code & 0x3Fu) : 0U;
    pub->codec_status = tetra_pub_codec_status(&ts->voice);
    pub->codec_frames = ts->voice.frames;
    pub->codec_errors = ts->voice.codec_errors;
}

/* Tell the frame sync where block 1 of the next normal burst sits: right against the training sequence in direct
 * mode and on the uplink, behind AACH half 1 on the downlink. Then publish what the frontends show. Returns 1 when the
 * burst decoded a block whose CRC passed. */
static int
tetra_frame_leave(dsd_state* state, const tetra_state_t* ts) {
    state->tetra_sync.block1_follows_training = (uint8_t)((ts->dmo || ts->uplink) ? 1 : 0);
    tetra_publish(state);
    return tetra_confirm_burst_end(state);
}

/* Open the burst the frame sync just locked, captured on the carrier of the current evidence epoch. The trunking state
 * machine settles a tune or return the backend answered first, once per burst and before any of its blocks
 * (tetra_sm_settle()); nothing inside the burst settles (tetra_trunk_sm.c), so every block of it is decoded against the
 * one state machine this left. A settle that moved the receiver (a tune or return completed) leaves the burst on the
 * carrier it was captured on, which the receiver has left: it is dropped whole, its symbols read and none of its
 * blocks decoded (tetra_confirm_burst_current(), tetra_nb_decode(), tetra_uplink_process_cb(), tetra_process_sb()). */
static void
tetra_burst_enter(dsd_opts* opts, dsd_state* state) {
    const uint32_t captured = tetra_confirm_carrier_epoch(state);
    (void)tetra_sm_settle(opts, state);
    tetra_confirm_burst_begin_captured(state, captured);
}

int
processTetraFrame(dsd_opts* opts, dsd_state* state) {
    const tetra_state_t* const ts = tetra_frame_enter(opts, state);
    if (!ts) {
        return 0;
    }
    tetra_burst_enter(opts, state);
    tetra_process_ndb(opts, state);
    return tetra_frame_leave(state, ts);
}

int
processTetraSBFrame(dsd_opts* opts, dsd_state* state) {
    const tetra_state_t* const ts = tetra_frame_enter(opts, state);
    if (!ts) {
        return 0;
    }
    tetra_burst_enter(opts, state);
    tetra_process_sb(opts, state);
    return tetra_frame_leave(state, ts);
}

void
tetra_forget_carrier(dsd_state* state) {
    /* A session that never decoded TETRA has nothing to forget, and gets no TETRA state for it. */
    if (!tetra_st_existing(state)) {
        return;
    }
    tetra_confirm_reset(state);
    tetra_mac_forget_fragments(state);
    tetra_tdma_stop(state);
    tetra_voice_reset(state);
    tetra_dmo_forget_carrier(state);
    tetra_calls_forget_carrier(state);
    /* The next carrier is trunked until its own DSB says otherwise: the frame sync finds block 1 of its first normal
     * burst behind AACH half 1 on the downlink, as tetra_frame_leave() would have told it (the uplink is its own). */
    state->tetra_sync.block1_follows_training = 0;
}
