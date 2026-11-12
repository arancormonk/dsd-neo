// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for P25 Phase 1 sync constants and dispatch routing.
 */

#include <mbelib-neo/mbelib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/p25p1_sync_anchor.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/protocol/p25/p25.h>
#include <dsd-neo/protocol/p25/p25_status_symbol.h>
#include <dsd-neo/protocol/p25/p25p1_check_nid.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "frame_sync_internal.h"
#include "p25_search_site.h"

int dsd_dispatch_matches_p25p1(int synctype);
dsd_frame_verdict dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state);

static uint8_t g_test_duid = 0x3U;
static int g_check_result = NID_OK;
static int g_new_nac = 0x293;
/* Whether the stand-in decoder reports a NID decoded only with the observed NAC put in its NAC field. */
static int g_nac_borrowed;
static int g_error_count;
static int g_last_observed_nac;
static int g_open_mbe_calls;
static int g_close_mbe_calls;
static int g_close_mbe_r_calls;
static int g_resume_calls;
static int g_status_classify_calls;
static int g_dibit_reads;
static int g_nid_decode_calls;
/* The NID read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there
   and stands erasures in for that symbol and every one after it; -1 for none. */
static int g_interrupt_at_dibit = -1;
/* The NID read a carrier boundary splits (a replay read adopting a recorded retune moves state->carrier_seq); -1 for
   none. */
static int g_boundary_at_dibit = -1;

/* The P25 proofs the dispatcher gave the CQPSK timing loop, and the tuning generation of the last. */
static int g_p25_proofs;
static uint64_t g_p25_proof_generation;
/* processTSBK() completes a tune (a grant's) when set, as a handler that follows a grant does. */
static int g_tsbk_tunes;
/* processTSBK() reads past its first block across a carrier boundary when set to 1 (a replay read adopting a recorded
   retune: the carrier count moves, the tuning generation does not), across a stream flush when set to 2 (the clock's
   epoch moves), onto another input when set to 3, or into a gap in the radio stream when set to 4. */
static int g_tsbk_stream_moves;
/* What processTSBK() reports: whether a TSBK of the TSDU passed its CRC. */
static int g_tsbk_crc_passed = 1;
/* Where the stand-in decoders past the NID leave the symbol clock, in dibits from their unit's sync word (g_sync_clock);
   0 leaves it where the NID left it. */
static uint32_t g_unit_read;
static uint64_t g_sync_clock;
/* The length a stand-in TSDU or PDU decoder reports (dsd_state::p25_p1_unit_dibits); 0 for none. */
static uint32_t g_unit_reported;
/* The symbol grid's side (src/dsp/symbol_anchor.c): the recordings the dispatcher's report armed for the next data
   unit's sync word, and the words the frame sync named to it. */
static int g_grid_arms;
static int g_grid_notes;
static char g_grid_note[DSD_SYMBOL_ANCHOR_MAX_SYMS + 1];

static void
reset_stub_state(void) {
    g_dibit_reads = 0;
    g_nid_decode_calls = 0;
    g_interrupt_at_dibit = -1;
    g_boundary_at_dibit = -1;
    g_test_duid = 0x3U;
    g_check_result = NID_OK;
    g_new_nac = 0x293;
    g_nac_borrowed = 0;
    g_error_count = 0;
    g_last_observed_nac = -1;
    g_open_mbe_calls = 0;
    g_close_mbe_calls = 0;
    g_close_mbe_r_calls = 0;
    g_resume_calls = 0;
    g_status_classify_calls = 0;
    g_p25_proofs = 0;
    g_p25_proof_generation = 0U;
    g_tsbk_tunes = 0;
    g_tsbk_stream_moves = 0;
    g_tsbk_crc_passed = 1;
    g_unit_read = 0U;
    g_sync_clock = 0U;
    g_unit_reported = 0U;
    g_grid_arms = 0;
    g_grid_notes = 0;
    g_grid_note[0] = '\0';
}

void
dsd_symbol_arm_hunt_sync_anchor(dsd_state* state) {
    (void)state;
    g_grid_arms++;
}

void
dsd_symbol_note_p25p1_sync_anchor(const dsd_state* state, const char* sync_dibits, int count) {
    (void)state;
    assert(count > 0 && count <= DSD_SYMBOL_ANCHOR_MAX_SYMS);
    g_grid_notes++;
    DSD_MEMCPY(g_grid_note, sync_dibits, (size_t)count);
    g_grid_note[count] = '\0';
}

/* A stand-in decoder past the NID read its unit up to g_unit_read dibits from the sync. */
static void
stand_in_reads_unit(dsd_state* state) {
    if (g_unit_read != 0U) {
        state->symbol_clock = g_sync_clock + g_unit_read;
    }
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    if (g_dibit_reads == g_boundary_at_dibit && state != NULL) {
        state->carrier_seq++;
    }
    if (g_dibit_reads++ == g_interrupt_at_dibit && state != NULL) {
        state->input_interrupted = 1;
    }
    if (out_soft != NULL) {
        out_soft->reliability = 255;
        out_soft->llr[0] = -255;
        out_soft->llr[1] = -255;
    }
    return 0;
}

struct p25p1_nid_result
p25p1_nid_decode(const char bch_code[63], const uint8_t reliab63[63], int observed_nac, unsigned char parity,
                 uint8_t parity_reliab) {
    (void)bch_code;
    (void)reliab63;
    (void)parity;
    (void)parity_reliab;
    g_nid_decode_calls++;
    g_last_observed_nac = observed_nac;
    struct p25p1_nid_result result = {
        (enum NidResult)g_check_result, g_new_nac, g_test_duid, g_error_count, g_nac_borrowed,
    };
    return result;
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

/* The frame sync publishes the learned modulation a decoded NID stamps, for the P25 watchdog's tunes. */
void
dsd_frame_sync_p25p1_mod_compare_publish(const dsd_state* state) {
    (void)state;
}

void
dsd_rtl_stream_metrics_hook_note_p25_proven(uint64_t tune_generation) {
    g_p25_proofs++;
    g_p25_proof_generation = tune_generation;
}

/* The real P25p1 sync anchor (frame_sync_p25p1_anchor.c) reads whether an off-thread tune awaits reconciling. */
int
dsd_frame_sync_off_thread_tune_pending(void) {
    return 0;
}

/* The capture opener of the revision before issue #661, kept with its counter so this harness also links and runs
   against it: the first frame written creates the capture now. */
// NOLINTNEXTLINE(misc-use-internal-linkage): main's dispatcher links against this stub.
void openMbeOutFile(dsd_opts* opts, dsd_state* state);

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    ++g_open_mbe_calls;
    opts->mbe_out_f = stdout;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    ++g_close_mbe_calls;
    opts->mbe_out_f = NULL;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    ++g_close_mbe_r_calls;
    opts->mbe_out_fR = NULL;
}

/* P25 closes only its own captures (issue #661); the owner filter is tested in CORE_MBE_FILE_IO. This counts the
   closes as the closers above do. */
void
dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype) {
    assert(DSD_SYNC_IS_P25(synctype));
    if ((slot_mask & DSD_MBE_OUT_SLOT1) != 0 && opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if ((slot_mask & DSD_MBE_OUT_SLOT2) != 0 && opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }
}

void
resumeScan(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    ++g_resume_calls;
}

void
mbe_initMbeParms(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    (void)cur_mp;
    (void)prev_mp;
    (void)prev_mp_enhanced;
}

void
p25_status_accum_reset(dsd_state* state) {
    (void)state;
}

void
p25_status_accum_add(dsd_state* state, int dibit_value) {
    (void)state;
    (void)dibit_value;
}

void
p25_status_accum_classify(dsd_state* state) {
    (void)state;
    ++g_status_classify_calls;
}

void
processHDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    stand_in_reads_unit(state);
    state->lastp25type = 2;
}

void
processLDU1(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    stand_in_reads_unit(state);
    state->lastp25type = 1;
}

void
processLDU2(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    stand_in_reads_unit(state);
    state->lastp25type = 2;
}

void
processTDULC(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    stand_in_reads_unit(state);
    state->lastp25type = 0;
}

void
processTDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    stand_in_reads_unit(state);
    state->lastp25type = 0;
}

/* The frame NAC verdict the TSDU's TSBKs see, which a network status among them pairs its WACN and SYSID with (issue
   #804); -1 before any TSDU. */
static int g_tsbk_frame_nac_valid = -1;
/* The scrambling codes the TSDU's network status (WACN BEE00, SYSID 1A3) gave the Signal search, and the last one. */
static int g_code_notes;
static uint64_t g_code;

/* The Signal search side of the code hook (the runtime is not linked here). */
void
dsd_signal_search_note_p25p2_code(const dsd_state* state, uint64_t code, int source) {
    (void)state;
    assert(source == DSD_P25P2_CODE_P1_NETWORK);
    g_code_notes++;
    g_code = code;
}

/* A TSDU whose block is a network status that passed its CRC: it pairs its system with the frame's NAC as
   tsbk_handle_network_status() does. With g_tsbk_crc_passed clear it stands for a TSDU whose blocks all failed theirs
   (only the CRC verdict it reports is read then). */
int
processTSBK(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 3;
    g_tsbk_frame_nac_valid = state->p25_frame_nac_valid;
    p25_search_note_p1_network_code(state, 0xBEE00U, 0x1A3U);
    if (g_tsbk_tunes) {
        dsd_trunk_tuning_generation_advance();
    }
    if (g_tsbk_stream_moves == 1) {
        state->carrier_seq++;
    } else if (g_tsbk_stream_moves == 2) {
        state->symbol_clock_epoch++;
    } else if (g_tsbk_stream_moves == 3) {
        state->symbol_clock_input++;
    } else if (g_tsbk_stream_moves == 4) {
        dsd_state_raise_input_interrupt(state, DSD_INTERRUPT_STREAM_GAP);
    }
    stand_in_reads_unit(state);
    state->p25_p1_unit_dibits = g_unit_reported;
    return g_tsbk_crc_passed;
}

void
processMPDU(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    state->lastp25type = 4;
    stand_in_reads_unit(state);
    state->p25_p1_unit_dibits = g_unit_reported;
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(P25P1_SYNC) == 25U, "P25P1_SYNC length");
    _Static_assert(sizeof(INV_P25P1_SYNC) == 25U, "INV_P25P1_SYNC length");
}

static void
test_synctype_helpers(void) {
    _Static_assert(DSD_SYNC_IS_P25P1(DSD_SYNC_P25P1_POS), "P25P1 positive synctype");
    _Static_assert(DSD_SYNC_IS_P25P1(DSD_SYNC_P25P1_NEG), "P25P1 negative synctype");
    _Static_assert(DSD_SYNC_IS_P25(DSD_SYNC_P25P1_POS), "P25P1 positive is P25");
    _Static_assert(DSD_SYNC_IS_P25(DSD_SYNC_P25P1_NEG), "P25P1 negative is P25");
    assert(dsd_dispatch_matches_p25p1(DSD_SYNC_P25P1_POS));
    assert(dsd_dispatch_matches_p25p1(DSD_SYNC_P25P1_NEG));

    _Static_assert(!DSD_SYNC_IS_P25P1(DSD_SYNC_P25P2_POS), "P25P2 is not P25P1");
    _Static_assert(!DSD_SYNC_IS_P25P1(DSD_SYNC_DMR_BS_DATA_POS), "DMR BS data is not P25P1");
    _Static_assert(!DSD_SYNC_IS_P25P1(DSD_SYNC_NXDN_POS), "NXDN is not P25P1");
    assert(!dsd_dispatch_matches_p25p1(DSD_SYNC_P25P2_POS));
    assert(!dsd_dispatch_matches_p25p1(DSD_SYNC_DMR_BS_DATA_POS));
    assert(!dsd_dispatch_matches_p25p1(DSD_SYNC_NXDN_POS));
}

static void
test_simple_tdu_dispatch_defaults(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    g_test_duid = 0x3U;
    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.nac == 0x293);
    assert(state.lastp25type == 0);
    assert(strcmp(state.fsubtype, " TDU          ") == 0);
}

static const char k_valid_embedded_gps[] = "GPS: 41.12345N 087.12345W (41.12345, -87.12345) Current Fix";

static void
seed_stale_data_call_display(dsd_state* state) {
    DSD_SNPRINTF(state->dmr_embedded_gps[0], sizeof(state->dmr_embedded_gps[0]), "%s", k_valid_embedded_gps);
    DSD_SNPRINTF(state->dmr_lrrp_gps[0], sizeof(state->dmr_lrrp_gps[0]),
                 "Data Call: Mobile Radio Statistics; SAP:24; LLID: 393226; ");
}

static void
expect_stale_data_call_display_cleared(uint8_t duid, unsigned int expected_burst, const char* expected_subtype) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    seed_stale_data_call_display(&state);

    g_test_duid = duid;
    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.dmrburstL == expected_burst);
    assert(strcmp(state.dmr_embedded_gps[0], k_valid_embedded_gps) == 0);
    assert(state.dmr_lrrp_gps[0][0] == '\0');
    assert(strcmp(state.fsubtype, expected_subtype) == 0);
}

static void
test_p25p1_dispatch_clears_stale_data_call_display(void) {
    static const struct {
        uint8_t duid;
        unsigned int expected_burst;
        const char* expected_subtype;
    } cases[] = {
        {0x0U, 25, " HDU          "}, {0x5U, 26, " LDU1         "}, {0xAU, 27, " LDU2         "},
        {0xFU, 28, " TDULC        "}, {0x3U, 28, " TDU          "}, {0x7U, 29, " TSBK         "},
        {0xCU, 29, " MPDU         "}, {0xFFU, 0, "              "},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        expect_stale_data_call_display_cleared(cases[i].duid, cases[i].expected_burst, cases[i].expected_subtype);
    }
}

static void
expect_observed_nac(const char* label, unsigned long long p2_cc, int p2_hardset, int nac, int expected_observed_nac) {
    (void)label;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    state.p2_cc = p2_cc;
    state.p2_hardset = p2_hardset;
    state.nac = nac;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_last_observed_nac == expected_observed_nac);
}

static void
test_p25p1_observed_nac_priority(void) {
    expect_observed_nac("hardset-p2-cc", 0x123ULL, 1, 0x234, 0x123);
    expect_observed_nac("decoded-nac", 0x123ULL, 0, 0x234, 0x234);
    expect_observed_nac("fallback-p2-cc", 0x456ULL, 0, 0, 0x456);
    expect_observed_nac("invalid-values", 0xFFFULL, 0, 0xFFF, 0);
}

static void
test_p25p1_nid_correction_and_failure_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    g_check_result = NID_PARITY_OVERRIDE;
    g_error_count = 3;
    g_new_nac = 0x321;
    g_test_duid = 0x3U;

    /* A decoded NID is taken at its word: the BCH held, so the burst behind it counts -- and
     * proves the profile carried it, whatever fraction of the slot the handler read (#400). */
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);

    assert(state.nid_corrections_total == 3U);
    assert(state.nid_parity_overrides == 1U);
    assert(state.nac == 0x321);
    assert(state.p2_cc == 0x321ULL);
    assert(state.debug_header_errors == 2U);
    assert(state.nid_failures_total == 0U);
    /* The NAC a BCH-accepted NID carries is the carrier's received access code. */
    assert(state.rx_id.family == (uint8_t)DSD_RXID_FAMILY_P25 && state.rx_id.valid == DSD_RXID_ACCESS);
    assert(state.rx_id.access_kind == (uint8_t)DSD_ACCESS_CODE_NAC && state.rx_id.access == 0x321U);
    /* An unchanged repeat proves it again: a stale record is fresh once more. */
    dsd_rxid_mark_stale(&state);
    reset_stub_state();
    g_new_nac = 0x321;
    (void)dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.rx_id.fresh == DSD_RXID_ACCESS && state.rx_id.access == 0x321U);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    g_test_duid = 0x5U;

    /* #391: the 63-bit BCH failed, the DUID is invalid and nothing is decoded past it, so
     * the 33 dibits already read validated nothing. Nothing has decoded here to vouch for
     * it either -- the zeroed state carries no evidence (#400). */
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);

    assert(state.nid_failures_total == 1U);
    assert(state.debug_header_critical_errors == 1U);
    assert(state.lastp25type == 0);
    assert(strcmp(state.fsubtype, "              ") == 0);
    assert(g_status_classify_calls == 1);
    assert(state.rx_id.valid == 0U); /* a failed NID names no NAC */
}

static void
test_p25p1_nac_update_guards(void) {
    static const int invalid_nacs[] = {0, 0xFFF};

    for (size_t i = 0; i < sizeof(invalid_nacs) / sizeof(invalid_nacs[0]); i++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_stub_state();

        state.synctype = DSD_SYNC_P25P1_POS;
        state.nac = 0x2AA;
        state.p2_cc = 0x2AAULL;
        g_new_nac = invalid_nacs[i];
        g_error_count = 2;
        g_test_duid = 0x0U;

        dsd_dispatch_handle_p25p1(&opts, &state);

        assert(g_last_observed_nac == 0x2AA);
        assert(state.nac == 0x2AA);
        assert(state.p2_cc == 0x2AAULL);
        assert(state.nid_corrections_total == 2U);
        assert(state.debug_header_errors == 1U);
        assert(state.nid_failures_total == 0U);
        assert(state.rx_id.valid == 0U); /* dsd-neo takes no NAC 000 or FFF */
    }

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x234;
    state.p2_cc = 0x123ULL;
    state.p2_hardset = 1;
    g_new_nac = 0x321;
    g_error_count = 1;
    g_test_duid = 0x0U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_last_observed_nac == 0x123);
    assert(state.nac == 0x321);
    assert(state.p2_cc == 0x123ULL);
    /* The record names the NAC received, never the hand-set seed's. */
    assert(state.rx_id.valid == DSD_RXID_ACCESS && state.rx_id.access == 0x321U);
    assert(state.nid_corrections_total == 1U);
    assert(state.debug_header_errors == 1U);
    assert(state.nid_failures_total == 0U);
}

/* Issue #804 (F10): a seed NAC cleared by hand (the menu's P2 parameters) on the carrier being decoded comes back from
   the next valid NID, although state->nac already holds that NAC. A NID repeating the NAC of a proven seed leaves the
   proof alone. */
static void
test_nid_restores_cleared_p2_cc(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    state.nac = 0x293;
    state.p2_cc = 0ULL;
    state.p2_hardset = 0;
    g_new_nac = 0x293;
    g_test_duid = 0x7U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.nac == 0x293);
    assert(state.p2_cc == 0x293ULL);
    assert(state.p2_cc_verified == 0U);
    /* The DUID read counts one header error; the NAC, the one state->nac holds, none. */
    assert(state.debug_header_errors == 1U);

    reset_stub_state();
    g_new_nac = 0x293;
    state.p2_cc_verified = 1U;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p2_cc == 0x293ULL);
    assert(state.p2_cc_verified == 1U);

    /* A NID naming another NAC than state->nac holds (a new carrier, or the first NID after a boundary zeroed it)
       replaces a seed NAC that differs, and its proof. */
    reset_stub_state();
    g_new_nac = 0x293;
    state.nac = 0;
    state.p2_cc = 0x111ULL;
    state.p2_cc_verified = 1U;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.nac == 0x293);
    assert(state.p2_cc == 0x293ULL);
    assert(state.p2_cc_verified == 0U);
}

/* Issue #804: on a system whose Phase 1 control channel NAC differs from its Phase 2 colour code, a network status that
   passed its CRC sets the seed's NAC to the colour code. Repeated NIDs of the control channel, their NAC the one
   state->nac already holds, leave that seed and its proof alone. */
static void
test_nid_keeps_nsb_colour_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    g_new_nac = 0x293;
    g_test_duid = 0x7U;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.nac == 0x293 && state.p2_cc == 0x293ULL);

    /* What p25p2_vpdu_apply_nsb_identity() leaves after a CRC-passed network status naming colour code 0x111. */
    state.p2_wacn = 0xBEE00ULL;
    state.p2_sysid = 0x164ULL;
    state.p2_cc = 0x111ULL;
    state.p2_cc_verified = 1U;

    for (int i = 0; i < 3; i++) {
        reset_stub_state();
        g_new_nac = 0x293;
        g_test_duid = 0x7U;
        dsd_dispatch_handle_p25p1(&opts, &state);
        assert(state.nac == 0x293);
        assert(state.p2_cc == 0x111ULL);
        assert(state.p2_cc_verified == 1U);
        /* The NID still names the carrier's NAC and the frame's. */
        assert(state.rx_id.access == 0x293U);
        assert(state.p25_frame_nac_valid == 1U && state.p25_frame_nac == 0x293U);
    }
}

static void
expect_frame_nac(int decoded_nac, int expected_valid) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    state.synctype = DSD_SYNC_P25P1_POS;
    /* The previous frame's NAC, which this frame's NID replaces or withdraws. */
    state.p25_frame_nac = 0x555U;
    state.p25_frame_nac_valid = 1U;
    g_new_nac = decoded_nac;
    g_test_duid = 0x7U;
    g_tsbk_frame_nac_valid = -1;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.p25_frame_nac_valid == (uint8_t)expected_valid);
    /* The TSDU's TSBKs run after the NID: they see this frame's verdict, never the previous frame's NAC. */
    assert(g_tsbk_frame_nac_valid == expected_valid);
    if (expected_valid) {
        assert(state.p25_frame_nac == (uint16_t)decoded_nac);
    }
}

/* Issue #804: the NAC of the frame being decoded, which a network status in it is paired with. It is valid only from a
   NID the BCH accepted naming no reserved NAC, and never outlives a NID that decoded nothing. */
static void
test_frame_nac_validity(void) {
    expect_frame_nac(0x000, 0);
    expect_frame_nac(0xFFF, 0);
    expect_frame_nac(0x001, 1);
    expect_frame_nac(0xFFE, 1);

    static dsd_opts opts;
    static dsd_state state;

    /* A NID the BCH rejected. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.p25_frame_nac = 0x555U;
    state.p25_frame_nac_valid = 1U;
    g_check_result = NID_DECODE_FAIL;
    (void)dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p25_frame_nac_valid == 0U);

    /* A NID an input interruption cut. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.p25_frame_nac = 0x555U;
    state.p25_frame_nac_valid = 1U;
    g_interrupt_at_dibit = 15;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_WITHHELD);
    assert(state.p25_frame_nac_valid == 0U);

    /* A NID a carrier boundary split: the BCH accepted it, but its NAC is no carrier's. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.frame_dispatch_active = 1;
    state.frame_dispatch_carrier_seq = state.carrier_seq;
    g_boundary_at_dibit = 10;
    (void)dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.carrier_seq == 1U);
    assert(state.p25_frame_nac_valid == 0U);
}

/* Issue #804: a NID that decoded only once the observed NAC stood in for its received NAC field (here the seed's NAC
   0x293 a Phase 2 carrier left, after the carrier boundary zeroed the received NAC) names no NAC: neither the frame's,
   so the TSDU's network status pairs nothing, nor the received NAC, the received identity or the seed's. Its DUID
   stands. The same NID read off the air pairs its own NAC. */
static void
test_borrowed_nac_names_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.synctype = DSD_SYNC_P25P1_POS;
    state.nac = 0;
    state.p2_cc = 0x293ULL;
    state.p25_frame_nac = 0x555U;
    state.p25_frame_nac_valid = 1U;
    g_test_duid = 0x7U;
    g_new_nac = 0x293;
    g_nac_borrowed = 1;
    g_tsbk_frame_nac_valid = -1;
    g_code_notes = 0;

    (void)dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_last_observed_nac == 0x293);
    assert(state.lastp25type == 3);
    assert(g_tsbk_frame_nac_valid == 0);
    assert(g_code_notes == 0);
    assert(state.p25_frame_nac_valid == 0U);
    assert(state.nac == 0);
    assert(state.rx_id.valid == 0U);
    assert(state.p2_cc == 0x293ULL);

    /* The same NID decoded from what was on the air: NAC 0x2C1 pairs with the network status. */
    reset_stub_state();
    g_test_duid = 0x7U;
    g_new_nac = 0x2C1;
    (void)dsd_dispatch_handle_p25p1(&opts, &state);
    assert(g_tsbk_frame_nac_valid == 1);
    assert(g_code_notes == 1);
    assert(g_code == ((0xBEE00ULL << 24) | (0x1A3ULL << 12) | 0x2C1ULL));
    assert(state.nac == 0x2C1);
}

static void
test_p25p1_mbe_output_and_resume_side_effects(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();

    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "captures");
    opts.mbe_out_f = (FILE*)0x1;
    opts.mbe_out_fR = (FILE*)0x2;
    g_test_duid = 0x0U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    /* A Phase 1 header ends the P25 capture on both slots: a Phase 2 slot 2 segment left on the carrier is over
       (issue #661). */
    assert(g_close_mbe_calls == 1);
    assert(g_close_mbe_r_calls == 1);
    assert(g_open_mbe_calls == 0);
    assert(opts.mbe_out_f == NULL);
    assert(opts.mbe_out_fR == NULL);
    assert(state.lastp25type == 2);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "captures");
    opts.mbe_out_f = (FILE*)0x1;
    opts.mbe_out_fR = (FILE*)0x2;
    opts.resume = 1;
    g_test_duid = 0x7U;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_close_mbe_calls == 1);
    assert(g_close_mbe_r_calls == 1);
    assert(g_resume_calls == 1);
    assert(opts.mbe_out_f == NULL);
    assert(opts.mbe_out_fR == NULL);
    assert(state.lastp25type == 3);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    /* A capture a caller attached, with no -d folder: the terminator still ends it on both slots. */
    opts.mbe_out_f = (FILE*)0x1;
    opts.mbe_out_fR = (FILE*)0x2;
    opts.resume = 1;
    state.numtdulc = 1;
    g_test_duid = 0xFU;

    dsd_dispatch_handle_p25p1(&opts, &state);

    assert(g_close_mbe_calls == 1);
    assert(g_close_mbe_r_calls == 1);
    assert(opts.mbe_out_f == NULL);
    assert(opts.mbe_out_fR == NULL);
    assert(g_resume_calls == 1);
    assert(state.numtdulc == 2);
    assert(state.lastp25type == 0);
}

/*
 * Issue #423: a decoded NID is the only place that knows which modulation actually carried a
 * P25p1 frame -- the sync word is identical on C4FM and CQPSK -- so it is where the trunking
 * retunes and the SPS hunt learn what to restore.
 */
static void
test_p25p1_valid_nid_learns_modulation(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* A frame that decoded through the CQPSK chain stamps CQPSK. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.rf_mod = 1;
    state.p25_p1_validated_rf_mod = -1;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p25_p1_validated_rf_mod == 1);

    /* And one that decoded through C4FM stamps C4FM, so a wrong guess self-corrects. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.rf_mod = 0;
    state.p25_p1_validated_rf_mod = 1;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p25_p1_validated_rf_mod == 0);

    /* A failed NID validated nothing, so it teaches nothing. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    state.rf_mod = 0;
    state.p25_p1_validated_rf_mod = 1;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p25_p1_validated_rf_mod == 1);

    /* An explicit CLI modulation lock owns the decision outright. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    opts.mod_cli_lock = 1;
    state.rf_mod = 1;
    state.p25_p1_validated_rf_mod = -1;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p25_p1_validated_rf_mod == -1);

    /* GFSK is not a P25p1 modulation; nothing to learn from it. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.rf_mod = 2;
    state.p25_p1_validated_rf_mod = -1;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p25_p1_validated_rf_mod == -1);
}

/*
 * Issue #400: consumption credit is bounded by what the handler read, so a control channel
 * decoding 134 symbols of a ~180-symbol TSDU slot loses ground on every frame it decodes and
 * the SPS hunt rotates off it. A decoded NID proves the profile instead of paying for the
 * frame, and vouches for the failures around it for a bounded window.
 */
static void
test_p25p1_verdict_follows_the_nid_evidence_window(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* A decoded NID stamps the window and proves the profile. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.symbolcnt = 5000U;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.p25_p1_nid_evidence == 1);
    assert(state.p25_p1_nid_evidence_symbolcnt == 5000U);

    /* The stamp speaks for the profile, not the demodulator, so neither of the gates the
     * modulation stamp answers to applies: a CLI modulation lock still stamps it. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    opts.mod_cli_lock = 1;
    state.rf_mod = 1;
    state.p25_p1_validated_rf_mod = -1;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.p25_p1_nid_evidence == 1);
    assert(state.p25_p1_validated_rf_mod == -1);

    /* And so does a frame that arrived through a chain the modulation stamp will not speak for. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.rf_mod = 2;
    state.p25_p1_validated_rf_mod = -1;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.p25_p1_nid_evidence == 1);
    assert(state.p25_p1_validated_rf_mod == -1);

    /* A failed NID inside the window is still evidence about the profile: P25p1 was decoding
     * here a moment ago. It learns no modulation from it, and does not move the window. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    state.rf_mod = 0;
    state.p25_p1_validated_rf_mod = 1;
    state.p25_p1_nid_evidence = 1;
    state.p25_p1_nid_evidence_symbolcnt = 1000U;
    state.symbolcnt = 1000U + 9599U;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.p25_p1_nid_evidence_symbolcnt == 1000U);
    assert(state.p25_p1_validated_rf_mod == 1);

    /* One symbol past the window it speaks only for itself again, so a channel that stops
     * decoding stops being vouched for. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    state.p25_p1_nid_evidence = 1;
    state.p25_p1_nid_evidence_symbolcnt = 1000U;
    state.symbolcnt = 1000U + 9600U;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);

    /* The flag is what opens the window, not the stamp: a state that has never decoded a NID
     * reads as no evidence rather than as evidence at symbol zero. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    state.p25_p1_nid_evidence = 0;
    state.p25_p1_nid_evidence_symbolcnt = 0U;
    state.symbolcnt = 12U;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);

    /* The window is measured modularly, so it stays exact across the symbol counter's rollover. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    state.p25_p1_nid_evidence = 1;
    state.p25_p1_nid_evidence_symbolcnt = UINT32_MAX - 100U;
    state.symbolcnt = 100U;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
}

/* A Signal search that leaves the frame (its visit ended, or a command came), or a live read that gives up on a stalled
   input, can cut the NID: getSymbol() stands 0 in for every symbol from there, and when 0 slices to dibit 0 the BCH
   takes the all-zero codeword (NAC 0, an HDU). A cut NID decodes nothing: no NAC, no data unit, no capture boundary,
   and no evidence about the demodulator or the profile; the verdict is withheld, as the engine's own decision. */
static void
test_p25p1_nid_cut_by_an_input_interruption_decodes_nothing(void) {
    static const int cuts[3] = {0, 15, 32};
    for (int c = 0; c < 3; c++) {
        static dsd_opts opts;
        static dsd_state state;
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_stub_state();
        opts.mbe_out_f = stdout;
        g_test_duid = 0x0U; /* what the all-zero codeword decodes to: an HDU */
        g_new_nac = 0x293;
        state.nac = 0x111;
        state.lastp25type = 9;
        state.rf_mod = 0;
        state.p25_p1_validated_rf_mod = -1;
        state.symbolcnt = 5000U;
        g_interrupt_at_dibit = cuts[c];

        assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_WITHHELD);

        assert(g_dibit_reads == 33);
        assert(g_nid_decode_calls == 0);
        assert(state.nac == 0x111);
        assert(state.lastp25type == 9);
        assert(state.p25_p1_nid_evidence == 0);
        assert(state.p25_p1_validated_rf_mod == -1);
        assert(state.nid_failures_total == 0U);
        assert(g_close_mbe_calls == 0);
        assert(opts.mbe_out_f == stdout);
    }
}

/* A carrier boundary while the NID is read (the frame dispatched on the carrier left): the BCH may still accept the
   split word, but its NAC is no carrier's, so the received identity the boundary reset for the next carrier takes
   nothing from it. */
static void
test_p25p1_nid_split_by_a_carrier_boundary_notes_no_nac(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_new_nac = 0x293;
    state.nac = 0x111;
    state.p2_cc = 0x111ULL;
    state.frame_dispatch_active = 1;
    state.frame_dispatch_carrier_seq = state.carrier_seq;
    g_boundary_at_dibit = 10;

    (void)dsd_dispatch_handle_p25p1(&opts, &state);

    assert(state.carrier_seq == 1U);
    assert(state.rx_id.valid == 0U && state.rx_id.family == (uint8_t)DSD_RXID_FAMILY_NONE);
    /* Nor does the received NAC or the seed's take it (issue #804). */
    assert(state.nac == 0x111);
    assert(state.p2_cc == 0x111ULL);
}

/* A trunked system's return from a Phase 2 voice channel to its Phase 1 control channel crosses no carrier boundary:
   the decoded NID is what says the carrier is FDMA again, so the Monitor's second-slot strip goes. A NID that failed
   (inside the evidence window or not) or that an input interruption cut says nothing about the carrier. */
static void
test_p25p1_decoded_nid_ends_the_two_slot_carrier(void) {
    static dsd_opts opts;
    static dsd_state state;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.tdma_carrier = 1U;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.tdma_carrier == 0U);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    state.tdma_carrier = 1U;
    state.p25_p1_nid_evidence = 1;
    state.p25_p1_nid_evidence_symbolcnt = 1000U;
    state.symbolcnt = 1100U;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.tdma_carrier == 1U);

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    opts.mbe_out_f = stdout;
    g_test_duid = 0x0U;
    state.tdma_carrier = 1U;
    g_interrupt_at_dibit = 15;
    assert(dsd_dispatch_handle_p25p1(&opts, &state) == DSD_FRAME_VERDICT_WITHHELD);
    assert(state.tdma_carrier == 1U);
}

/* Issue #802: a Phase 1 NID's NAC, which the received identity record holds, is never a Phase 2 proof. One that changes
   the seed drops the Phase 2 proofs with it, and one before any Phase 2 proof leaves a Phase 2 call reading nothing. */
static void
test_p25p1_nid_is_no_phase2_proof(void) {
    static dsd_opts opts;
    static dsd_state state;
    uint16_t value = 0U;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p2_cc = 0x123ULL;
    state.p2_cc_verified = 1U;
    state.p2_proven_nac = 0x123;
    g_new_nac = 0x321;
    g_error_count = 0;
    g_test_duid = 0x0U;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.p2_cc == 0x321ULL);
    assert(state.p2_cc_verified == 0U && state.p2_proven_nac == -1);
    assert(state.rx_id.valid == DSD_RXID_ACCESS && state.rx_id.access == 0x321U);
    assert(dsd_access_code_current(&state, DSD_SYNC_P25P2_POS, 0U, 0U, NULL, &value) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    dsd_state_forget_p2_proof(&state);
    reset_stub_state();
    state.synctype = DSD_SYNC_P25P1_POS;
    g_new_nac = 0x321;
    g_test_duid = 0x0U;
    dsd_dispatch_handle_p25p1(&opts, &state);
    assert(state.rx_id.access == 0x321U);
    assert(dsd_access_code_current(&state, DSD_SYNC_P25P1_POS, 0U, 0U, NULL, &value) == 1 && value == 0x321U);
    assert(dsd_access_code_current(&state, DSD_SYNC_P25P2_POS, 0U, 0U, NULL, &value) == 0);
}

/* Dispatch one P25p1 frame with DUID @p duid on @p state, whose NID decodes as the stub's defaults say, its sync
   accepted exactly just before as the frame sync accepts it (frame_sync_p25p1_anchor_note_sync()), and return the P25
   proofs the CQPSK timing loop was given. */
static int
dispatch_p25p1_frame(dsd_opts* opts, dsd_state* state, uint8_t duid) {
    state->synctype = DSD_SYNC_P25P1_POS;
    state->rf_mod = 1;
    state->symbol_clock += 1000U;
    frame_sync_p25p1_anchor_note_sync(state, DSD_SYNC_P25P1_POS, -1, 0);
    g_test_duid = duid;
    const int before = g_p25_proofs;
    (void)dsd_dispatch_handle_p25p1(opts, state);
    return g_p25_proofs - before;
}

/* Only a validated TSDU (p25p1_nid_verdict_validates(), read whole) with a TSBK that passed its CRC proves the carrier
   P25 to the CQPSK timing loop, on the tune it was synced on, so the Gardner may step to its P25 post-lock gain. A voice
   dwell never does, however many of its frames validate: on a simulcast voice carrier the slower gain lags the timing
   swing of the sites' beat and has no symbol slips to save in so short a dwell. Neither does a data unit, a TSDU whose
   TSBKs all failed their CRC, or a TSDU whose NID failed, was borrowed, named NAC FFF or was cut, nor one read on a
   stream that is no longer the one its sync was accepted on: a handler that tunes away (a grant), a carrier boundary
   inside the TSDU after a block that passed (a replay read adopting a recorded retune, which moves no tuning
   generation), a flush. */
static void
test_p25p1_tsbk_crc_proves_p25(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* A TSDU whose TSBK passed its CRC proves. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    const uint64_t generation = dsd_trunk_tuning_generation();
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 1);
    assert(g_p25_proof_generation == generation);

    /* A voice dwell, every frame validated: HDU, LDU1, LDU2, LDU1, LDU2, TDULC, TDU. Then a data unit. */
    static const uint8_t k_voice_dwell[] = {0x0U, 0x5U, 0xAU, 0x5U, 0xAU, 0xFU, 0x3U};
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    for (size_t i = 0; i < sizeof(k_voice_dwell); i++) {
        assert(dispatch_p25p1_frame(&opts, &state, k_voice_dwell[i]) == 0);
        assert(state.p25_p1_nid_evidence == 1);
    }
    assert(dispatch_p25p1_frame(&opts, &state, 0xCU) == 0);

    /* A validated TSDU whose TSBKs all failed their CRC proves nothing; the same TSDU passing proves. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_tsbk_crc_passed = 0;
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 0);
    assert(state.lastp25type == 3);
    g_tsbk_crc_passed = 1;
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 1);

    /* A TSDU whose TSBK passed behind a NID that did not validate: failed, borrowed, NAC FFF, cut. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.nac = 0x293;
    g_nac_borrowed = 1;
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_new_nac = 0xFFF;
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_interrupt_at_dibit = 15;
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 0);

    /* A TSDU whose grant tuned away proves nothing: a proof of the tune it was synced on would be stale on the channel
       reached. (This used to give that stale proof; none is given now.) */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_tsbk_tunes = 1;
    const uint64_t synced_on = dsd_trunk_tuning_generation();
    assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 0);
    assert(dsd_trunk_tuning_generation() == synced_on + 1U);

    /* A carrier boundary inside the TSDU after a block that passed, or a flush there: the tuning generation stands, so
       a proof would look current on the stream reached. None is given; the next TSDU read whole proves again. */
    for (int moves = 1; moves <= 2; moves++) {
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_stub_state();
        g_tsbk_stream_moves = moves;
        const uint64_t generation_before = dsd_trunk_tuning_generation();
        assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 0);
        assert(dsd_trunk_tuning_generation() == generation_before);
        g_tsbk_stream_moves = 0;
        assert(dispatch_p25p1_frame(&opts, &state, 0x7U) == 1);
        assert(g_p25_proof_generation == generation_before);
    }

    /* No sync context captured (an off-thread tune was pending at the sync): nothing says which stream the frame was
       read on, so it proves nothing. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.synctype = DSD_SYNC_P25P1_POS;
    state.rf_mod = 1;
    g_test_duid = 0x7U;
    (void)dsd_dispatch_handle_p25p1(&opts, &state);
    assert(g_p25_proofs == 0);
}

/* Dispatch one P25p1 frame read on the C4FM chain, DUID @p duid, its sync accepted exactly at a fresh position, whose
   decoders past the NID leave the symbol clock @p read dibits past that sync (0: where the NID left it). Returns the
   recordings the report armed for the next unit's sync word. */
static int
dispatch_c4fm_frame(dsd_opts* opts, dsd_state* state, uint8_t duid, uint32_t read) {
    state->synctype = DSD_SYNC_P25P1_POS;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->rf_mod = 0;
    state->symbol_clock += 2000U;
    frame_sync_p25p1_anchor_note_sync(state, DSD_SYNC_P25P1_POS, -1, 0);
    g_sync_clock = state->symbol_clock - 24U;
    g_unit_read = read;
    g_test_duid = duid;
    const int before = g_grid_arms;
    (void)dsd_dispatch_handle_p25p1(opts, state);
    return g_grid_arms - before;
}

/* On the C4FM chain the frame sync names each exact P25p1 word to the symbol grid's anchor from its second symbol (the
   recording starts on the first, and the measurement needs a symbol of margin), either polarity. A word within the
   tolerance names nothing before its NID validates, and the CQPSK chain's grid takes one sample a symbol. */
static void
test_p25p1_c4fm_sync_names_the_grid_word(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.symbol_clock = 1000U;

    state.rf_mod = 0;
    frame_sync_p25p1_anchor_note_sync(&state, DSD_SYNC_P25P1_POS, -1, 0);
    assert(g_grid_notes == 1 && strcmp(g_grid_note, &P25P1_SYNC[1]) == 0);
    frame_sync_p25p1_anchor_note_sync(&state, DSD_SYNC_P25P1_NEG, -1, 0);
    assert(g_grid_notes == 2 && strcmp(g_grid_note, &INV_P25P1_SYNC[1]) == 0);
    frame_sync_p25p1_anchor_note_sync(&state, DSD_SYNC_P25P1_POS, 0x1A6, 3);
    assert(g_grid_notes == 2);

    state.rf_mod = 1;
    frame_sync_p25p1_anchor_note_sync(&state, DSD_SYNC_P25P1_POS, -1, 0);
    assert(g_grid_notes == 2);
}

/* The arming rows (plan C1): a C4FM frame that validated, whose length on air is known, read on the stream its sync was
   accepted on, whose decoders stopped at the unit's end or inside its null pad, arms the grid's recording of the next
   unit's sync word, so that word places the grid for the unit behind it. Every other way a frame ends arms nothing, and
   the hunt's crossing loop keeps the grid as before. */
static void
test_p25p1_c4fm_frame_arms_the_grid_by_its_rows(void) {
    static dsd_opts opts;
    static dsd_state state;

    /* The units, by the length the dispatcher knows and where their decoders stop. */
    static const struct {
        uint8_t duid;
        uint32_t reported; /* the TSDU's or PDU's length as its decoder read it */
        uint32_t read;
        int arms;
        const char* row;
    } k_units[] = {
        {0x0U, 0U, 396U, 1, "HDU"},
        {0x5U, 0U, 864U, 1, "LDU1"},
        {0xAU, 0U, 864U, 1, "LDU2"},
        {0x3U, 0U, 72U, 1, "TDU"},
        {0xFU, 0U, 216U, 1, "TDULC"},
        {0x7U, 180U, 158U, 1, "TSDU, a last block that passed its CRC, its null pad left to the hunt"},
        {0x7U, 360U, 360U, 1, "three-block TSDU: no pad"},
        {0x7U, 0U, 158U, 0, "TSDU the trailing-null rule or a failed block ended"},
        {0xCU, 288U, 260U, 1, "PDU read to its CRC-valid header's length"},
        {0xCU, 576U, 260U, 0, "PDU whose header announced blocks its reader did not collect"},
        {0xCU, 180U, 260U, 0, "PDU read past what its recovered header names"},
        {0xCU, 0U, 260U, 0, "PDU whose header failed its CRC"},
        {0x5U, 0U, 865U, 0, "LDU read past its end"},
        {0x5U, 0U, 828U, 0, "LDU left a segment short"},
        {0x9U, 0U, 0U, 0, "undefined DUID"},
    };

    for (size_t i = 0; i < sizeof(k_units) / sizeof(k_units[0]); i++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_stub_state();
        g_unit_reported = k_units[i].reported;
        const int arms = dispatch_c4fm_frame(&opts, &state, k_units[i].duid, k_units[i].read);
        if (arms != k_units[i].arms) {
            DSD_FPRINTF(stderr, "grid row %s: %d armed\n", k_units[i].row, arms);
        }
        assert(arms == k_units[i].arms);
    }

    /* The NID's rows: failed, held (another NAC at a costly correction count), borrowed, NAC FFF, cut. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_check_result = NID_DECODE_FAIL;
    assert(dispatch_c4fm_frame(&opts, &state, 0x5U, 864U) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    assert(dispatch_c4fm_frame(&opts, &state, 0x5U, 864U) == 1); /* establishes the carrier's NAC */
    g_new_nac = 0x2A1;
    g_error_count = 6;
    assert(dispatch_c4fm_frame(&opts, &state, 0x5U, 864U) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.nac = 0x293;
    g_nac_borrowed = 1;
    assert(dispatch_c4fm_frame(&opts, &state, 0x5U, 864U) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_new_nac = 0xFFF;
    assert(dispatch_c4fm_frame(&opts, &state, 0x5U, 864U) == 0);

    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    g_interrupt_at_dibit = 15;
    assert(dispatch_c4fm_frame(&opts, &state, 0x5U, 864U) == 0);

    /* The stream's rows, inside a TSDU whose last block passed: a grant tuned away (the tuning generation moves, as a
       release's or a RESET's tune moves it), a carrier boundary, a flush, another input, a gap in the radio stream. The
       next TSDU read whole arms again. */
    for (int moves = 0; moves <= 4; moves++) {
        DSD_MEMSET(&state, 0, sizeof(state));
        reset_stub_state();
        g_unit_reported = 180U;
        if (moves == 0) {
            g_tsbk_tunes = 1;
        }
        g_tsbk_stream_moves = moves;
        assert(dispatch_c4fm_frame(&opts, &state, 0x7U, 158U) == 0);
        g_tsbk_tunes = 0;
        g_tsbk_stream_moves = 0;
        state.input_interrupted = 0;
        assert(dispatch_c4fm_frame(&opts, &state, 0x7U, 158U) == 1);
    }

    /* The CQPSK chain's grid is one sample a symbol: nothing to place. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.synctype = DSD_SYNC_P25P1_POS;
    state.rf_mod = 1;
    state.symbol_clock = 2000U;
    frame_sync_p25p1_anchor_note_sync(&state, DSD_SYNC_P25P1_POS, -1, 0);
    g_sync_clock = state.symbol_clock - 24U;
    g_unit_read = 864U;
    g_test_duid = 0x5U;
    (void)dsd_dispatch_handle_p25p1(&opts, &state);
    assert(g_grid_arms == 0);

    /* No sync context captured (an off-thread tune was pending at the sync): nothing says where the next sync starts on
       the stream read. */
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.synctype = DSD_SYNC_P25P1_POS;
    state.rf_mod = 0;
    g_test_duid = 0x5U;
    (void)dsd_dispatch_handle_p25p1(&opts, &state);
    assert(g_grid_arms == 0);
}

typedef void (*grid_stream_change_fn)(dsd_state* state);

static void
grid_no_change(dsd_state* state) {
    (void)state;
}

static void
grid_tune(dsd_state* state) {
    (void)state;
    dsd_trunk_tuning_generation_advance();
}

static void
grid_carrier(dsd_state* state) {
    state->carrier_seq++;
}

static void
grid_flush(dsd_state* state) {
    state->symbol_clock_epoch++;
}

static void
grid_input(dsd_state* state) {
    state->symbol_clock_input++;
}

static void
grid_histories(dsd_state* state) {
    state->frame_sync_carry.drops++;
}

static void
grid_profile(dsd_state* state) {
    state->sps_hunt_idx++;
}

static void
grid_cqpsk(dsd_state* state) {
    state->rf_mod = 1;
}

static void
grid_gfsk(dsd_state* state) {
    state->rf_mod = 2;
}

/* Each arming row's own verdict (dsd_frame_sync_p25p1_frame_decoded()): a C4FM sync at 2000 on the symbol clock, then
   @p change, the decoder at @p read dibits past the sync, a unit of @p unit dibits. */
static dsd_p25p1_grid_verdict
grid_verdict_for(grid_stream_change_fn change, int capture, uint32_t unit, uint32_t read) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    reset_stub_state();
    state.rf_mod = 0;
    state.symbol_clock = 2024U;
    if (capture) {
        frame_sync_p25p1_anchor_note_sync(&state, DSD_SYNC_P25P1_POS, -1, 0);
    }
    change(&state);
    state.symbol_clock = 2000U + read;
    const int arms = g_grid_arms;
    const dsd_p25p1_grid_verdict verdict = dsd_frame_sync_p25p1_frame_decoded(&state, 0x293, unit);
    assert((g_grid_arms - arms == 1) == (verdict == DSD_P25P1_GRID_ARMED));
    return verdict;
}

/* The arming table's rows by the verdict each returns, in their order: the first row that refuses names the reason. */
static void
test_p25p1_grid_arming_rows_name_their_verdicts(void) {
    /* Known length, read to the end or inside the null pad (35 dibits short at most): armed. */
    assert(grid_verdict_for(grid_no_change, 1, 864U, 864U) == DSD_P25P1_GRID_ARMED);
    assert(grid_verdict_for(grid_no_change, 1, 180U, 158U) == DSD_P25P1_GRID_ARMED);
    assert(grid_verdict_for(grid_no_change, 1, 180U, 145U) == DSD_P25P1_GRID_ARMED);
    /* Another chain: CQPSK, or GFSK when the votes flap, whatever else holds. */
    assert(grid_verdict_for(grid_cqpsk, 1, 864U, 864U) == DSD_P25P1_GRID_NOT_C4FM);
    assert(grid_verdict_for(grid_gfsk, 1, 0U, 864U) == DSD_P25P1_GRID_NOT_C4FM);
    /* No length: a NID that did not validate, a cut read (both report 0), a TSDU of uncertain end, a PDU whose header
       failed; checked before the stream. */
    assert(grid_verdict_for(grid_no_change, 1, 0U, 158U) == DSD_P25P1_GRID_NO_LENGTH);
    assert(grid_verdict_for(grid_tune, 1, 0U, 158U) == DSD_P25P1_GRID_NO_LENGTH);
    /* The stream: no context captured, a handler's tune, a carrier boundary, a flush, another input, dropped sync
       histories, a profile step. */
    static const grid_stream_change_fn k_moves[] = {grid_tune,  grid_carrier,   grid_flush,
                                                    grid_input, grid_histories, grid_profile};
    assert(grid_verdict_for(grid_no_change, 0, 864U, 864U) == DSD_P25P1_GRID_NO_STREAM);
    for (size_t i = 0; i < sizeof(k_moves) / sizeof(k_moves[0]); i++) {
        assert(grid_verdict_for(k_moves[i], 1, 864U, 864U) == DSD_P25P1_GRID_NO_STREAM);
    }
    /* Read into the next unit, then short of it by more than a pad. */
    assert(grid_verdict_for(grid_no_change, 1, 864U, 865U) == DSD_P25P1_GRID_READ_PAST);
    assert(grid_verdict_for(grid_no_change, 1, 180U, 144U) == DSD_P25P1_GRID_SHORT_READ);
    assert(grid_verdict_for(grid_no_change, 1, 576U, 260U) == DSD_P25P1_GRID_SHORT_READ);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_synctype_helpers();
    test_simple_tdu_dispatch_defaults();
    test_p25p1_dispatch_clears_stale_data_call_display();
    test_p25p1_observed_nac_priority();
    test_p25p1_nid_correction_and_failure_state();
    test_p25p1_nac_update_guards();
    test_p25p1_nid_is_no_phase2_proof();
    test_nid_restores_cleared_p2_cc();
    test_nid_keeps_nsb_colour_code();
    test_frame_nac_validity();
    test_borrowed_nac_names_nothing();
    test_p25p1_mbe_output_and_resume_side_effects();
    test_p25p1_valid_nid_learns_modulation();
    test_p25p1_verdict_follows_the_nid_evidence_window();
    test_p25p1_nid_cut_by_an_input_interruption_decodes_nothing();
    test_p25p1_nid_split_by_a_carrier_boundary_notes_no_nac();
    test_p25p1_decoded_nid_ends_the_two_slot_carrier();
    test_p25p1_tsbk_crc_proves_p25();
    test_p25p1_c4fm_sync_names_the_grid_word();
    test_p25p1_c4fm_frame_arms_the_grid_by_its_rows();
    test_p25p1_grid_arming_rows_name_their_verdicts();
    printf("P25_P1_SYNC_DISPATCH: OK\n");
    return 0;
}
