// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused checks for dPMR sync constants and dispatch matching.
 */

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int dsd_dispatch_matches_dpmr(int synctype);
dsd_frame_verdict dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state);

static int g_close_calls;
static int g_open_calls;
static int g_voice_calls;
static int g_close_protocol_synctype = DSD_SYNC_NONE;

/* How many CCH halves the next processdPMRvoice() reports passing their CRC-7. */
static int g_voice_crc_halves = 2;

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_close_calls++;
}

/* The capture opener the dispatcher called on every FS2 sync before issue #661, kept with its counter so this harness
   also links and runs against that revision: the dispatcher opens nothing now, the first frame written creates it. */
// NOLINTNEXTLINE(misc-use-internal-linkage): main's dispatcher links against this stub.
void openMbeOutFile(dsd_opts* opts, dsd_state* state);

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_open_calls++;
}

/* dPMR closes only its own capture (issue #661); the owner filter is tested in CORE_MBE_FILE_IO. This records the
   protocol the dispatcher names and counts the close as the closer above does. */
void
dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype) {
    g_close_protocol_synctype = synctype;
    if ((slot_mask & DSD_MBE_OUT_SLOT1) != 0 && opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
}

int
processdPMRvoice(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    g_voice_calls++;
    /* The real decoder confirms the transmission from the same evidence it returns. */
    if (g_voice_crc_halves > 0) {
        state->dpmr_confirmed = 1;
    }
    return g_voice_crc_halves;
}

/* Put @p count bits of @p value, most significant first, at @p at of @p bits. */
static void
put_bits(uint8_t* bits, int at, int count, uint32_t value) {
    for (int i = 0; i < count; i++) {
        bits[at + i] = (uint8_t)((value >> (count - 1 - i)) & 1U);
    }
}

/* What the frame sync leaves behind when it accepts an FS1 on a header that passed its check (dsd_dpmr_frame_sync):
   a header of message type @p mt, Communications Mode @p mode and Version @p version, called ID 1464100 ("1000000"),
   calling ID 2928210 ("200000*"), behind the channel code of colour code 12. */
static void
plant_checked_header(dsd_state* state, uint32_t mt, uint32_t mode, uint32_t version) {
    dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    DSD_MEMSET(sync->checked_bits, 0, sizeof(sync->checked_bits));
    put_bits(sync->checked_bits, 0, 4, mt);
    put_bits(sync->checked_bits, 4, 24, 1464100U);
    put_bits(sync->checked_bits, 28, 24, 2928210U);
    put_bits(sync->checked_bits, 52, 3, mode);
    put_bits(sync->checked_bits, 55, 2, version);
    put_bits(sync->checked_bits, 57, 2, 1U);
    put_bits(sync->checked_cc, 0, 24, 0x5D5D55U);
    sync->checked_kind = DSD_DPMR_SYNC_FRAME_HEADER;
    sync->checked_copies = DSD_DPMR_SYNC_COPY_0 | DSD_DPMR_SYNC_COPY_1;
    sync->checked_clock = state->symbol_clock;
    sync->checked_epoch = state->symbol_clock_epoch;
    state->synctype = DSD_SYNC_DPMR_FS1_POS;
}

/* The same for an FS3 on an end frame that passed its check: a normal end frame. */
static void
plant_checked_end(dsd_state* state) {
    dsd_dpmr_frame_sync* sync = &state->dpmr_sync;
    DSD_MEMSET(sync->checked_bits, 0, sizeof(sync->checked_bits));
    sync->checked_kind = DSD_DPMR_SYNC_FRAME_END;
    sync->checked_copies = DSD_DPMR_SYNC_COPY_1;
    sync->checked_clock = state->symbol_clock;
    sync->checked_epoch = state->symbol_clock_epoch;
    state->synctype = DSD_SYNC_DPMR_FS3_POS;
}

/* An open dPMR voice call, as an earlier confirmed frame published it. */
static void
open_voice_call(dsd_state* state) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DPMR_FS2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_VOICE,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

static void
test_sync_pattern_lengths(void) {
    _Static_assert(sizeof(DPMR_FRAME_SYNC_1) == 25U, "DPMR_FRAME_SYNC_1 length");
    _Static_assert(sizeof(DPMR_FRAME_SYNC_2) == 13U, "DPMR_FRAME_SYNC_2 length");
    _Static_assert(sizeof(DPMR_FRAME_SYNC_3) == 13U, "DPMR_FRAME_SYNC_3 length");
    _Static_assert(sizeof(DPMR_FRAME_SYNC_4) == 25U, "DPMR_FRAME_SYNC_4 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_1) == 25U, "INV_DPMR_FRAME_SYNC_1 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_2) == 13U, "INV_DPMR_FRAME_SYNC_2 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_3) == 13U, "INV_DPMR_FRAME_SYNC_3 length");
    _Static_assert(sizeof(INV_DPMR_FRAME_SYNC_4) == 25U, "INV_DPMR_FRAME_SYNC_4 length");
}

static void
test_sync_pattern_pairs(void) {
    assert(strcmp(DPMR_FRAME_SYNC_1, INV_DPMR_FRAME_SYNC_4) == 0);
    assert(strcmp(DPMR_FRAME_SYNC_4, INV_DPMR_FRAME_SYNC_1) == 0);
    assert(strcmp(DPMR_FRAME_SYNC_2, INV_DPMR_FRAME_SYNC_2) != 0);
    assert(strcmp(DPMR_FRAME_SYNC_3, INV_DPMR_FRAME_SYNC_3) != 0);
}

static void
test_synctype_helpers(void) {
    for (int synctype = DSD_SYNC_DPMR_FS1_POS; synctype <= DSD_SYNC_DPMR_FS4_NEG; synctype++) {
        assert(DSD_SYNC_IS_DPMR(synctype));
        assert(dsd_dispatch_matches_dpmr(synctype));
    }

    _Static_assert(!DSD_SYNC_IS_DPMR(DSD_SYNC_DSTAR_HD_NEG), "D-STAR header is not dPMR");
    _Static_assert(!DSD_SYNC_IS_DPMR(DSD_SYNC_NXDN_POS), "NXDN is not dPMR");
    assert(!dsd_dispatch_matches_dpmr(DSD_SYNC_DSTAR_HD_NEG));
    assert(!dsd_dispatch_matches_dpmr(DSD_SYNC_NXDN_POS));
}

/* Issue #890 (A6): only a header that opens a call (checked, Communications_Start, voice or data) and a checked end
   frame close dPMR's capture; an FS1 or FS3 no checked frame stands behind, a header of another message type and an
   FS4 close nothing. */
static void
test_dispatch_close_only_syncs(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    g_close_calls = 0;

    opts.mbe_out_f = stdout;
    state.synctype = DSD_SYNC_DPMR_FS1_POS;
    /* FDMA voice is slot 0, whatever slot a TDMA decode left current. */
    state.currentslot = 1;
    dsd_dispatch_handle_dpmr(&opts, &state);
    assert(state.currentslot == 0);
    assert(g_close_calls == 0);

    plant_checked_header(&state, 0x2U /* Disconnect_Request */, 0U, 0U);
    dsd_dispatch_handle_dpmr(&opts, &state);
    assert(g_close_calls == 0);

    plant_checked_header(&state, 0x0U, 0U, 0U);
    dsd_dispatch_handle_dpmr(&opts, &state);
    assert(g_close_calls == 1);
    assert(DSD_SYNC_IS_DPMR(g_close_protocol_synctype));
    assert(g_voice_calls == 0);

    state.synctype = DSD_SYNC_DPMR_FS3_NEG;
    dsd_dispatch_handle_dpmr(&opts, &state);
    assert(g_close_calls == 1);
    plant_checked_end(&state);
    dsd_dispatch_handle_dpmr(&opts, &state);
    assert(g_close_calls == 2);

    state.synctype = DSD_SYNC_DPMR_FS4_POS;
    dsd_dispatch_handle_dpmr(&opts, &state);
    assert(g_close_calls == 2);
}

/* Issue #890 (B19): the dispatcher publishes nothing before the frame's CCH is read; the frame publishes what its
   checked half says (DPMR_VOICE_BRIDGE). With the decoder stubbed here, a confirmed transmission's FS2 sync leaves no
   call row of its own. */
static void
test_dispatch_voice_sync_continues_a_confirmed_call_and_processes_voice(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "captures");
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    state.nac = 123;
    state.dpmr_confirmed = 1;
    const int voice_before = g_voice_calls;

    dsd_dispatch_handle_dpmr(&opts, &state);

    /* The voice decoder creates the capture with the first frame it records; the sync opens nothing (issue #661). */
    assert(g_open_calls == 0);
    assert(g_voice_calls == voice_before + 1);
    assert(state.nac == 0);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) <= 0);
    assert(strcmp(state.fsubtype, " VOICE        ") == 0);
}

/* Issue #890 (A6): a checked Communications_Start header opens its call with the identity and service it carries, its
   channel code and its scrambling, ending the call before it; a checked header of another message type ends nothing
   and opens nothing; Communications Mode 110 opens nothing either. */
static void
test_dispatch_checked_header_opens_its_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_call_snapshot call;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_color_code = -1;

    open_voice_call(&state);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    const uint64_t before = call.epoch;

    plant_checked_header(&state, 0x3U /* T_ACK */, 0U, 0U);
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch == before && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(state.dpmr_confirmed == 1); /* checked content all the same */

    plant_checked_header(&state, 0x0U, 6U /* service by MI_TYPE */, 0U);
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch == before && call.phase == DSD_CALL_PHASE_ACTIVE);

    state.dpmr_scrambled = 0U;
    plant_checked_header(&state, 0x0U, 2U /* Type 1 data */, 3U);
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.epoch != before && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.kind == DSD_CALL_KIND_DATA);
    assert(strcmp(call.target_text, "1000000") == 0);
    assert(strcmp(call.source_text, "200000*") == 0);
    assert(call.has_service_metadata == 1U);
    assert(call.service_options == ((3U << 8U) | (2U << 4U) | 1U));
    assert(state.dpmr_color_code == 12);
    assert(state.dpmr_scrambled == 2U);
    assert(strcmp(state.fsubtype, " HEADER       ") == 0);
    /* Taken once: the same sync dispatched again has no header behind it. */
    const uint64_t opened = call.epoch;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch == opened);

    /* A voice header opens a voice call. */
    state.symbol_clock += 500U;
    plant_checked_header(&state, 0x0U, 1U, 0U);
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch != opened && call.kind == DSD_CALL_KIND_VOICE);
    assert(state.dpmr_scrambled == 1U);

    /* A header taken at another symbol than the sync's is not this sync's. */
    plant_checked_header(&state, 0x0U, 1U, 0U);
    state.symbol_clock++;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
}

/* Issue #661: an FS2 sync on noise, before any CCH has passed its CRC, leaves no capture file behind. */
static void
test_dispatch_unconfirmed_voice_sync_opens_no_capture(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    g_open_calls = 0;

    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "captures");
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    g_voice_crc_halves = 0;
    dsd_dispatch_handle_dpmr(&opts, &state);
    g_voice_crc_halves = 2;

    assert(state.dpmr_confirmed == 0);
    assert(g_open_calls == 0);
    assert(opts.mbe_out_f == NULL);
}

// An FS3 end frame decoding after a sync-loss end must still reach the canonical retract path:
// the recoverable end tightens to TERMINATOR, so the event layer can keep the audible epoch's
// row and the reacquisition window closes. A phase guard in the dispatch would make the retract
// path unreachable for dPMR alone among the converted protocols.
static void
test_dispatch_end_sync_tightens_sync_loss_end(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    open_voice_call(&state);
    assert(dsd_call_state_end_ex(&state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) > 0);

    plant_checked_end(&state);
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);

    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    assert(call.phase == DSD_CALL_PHASE_ENDED);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
}

/* Issue #890 (A6, B9): only an end frame that passed its CRC-7 ends the call; an FS3 no checked END stands behind leaves
   the call, its scrambling and its capture alone. */
static void
test_dispatch_unchecked_end_keeps_the_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    open_voice_call(&state);
    state.dpmr_scrambled = 2U;

    state.synctype = DSD_SYNC_DPMR_FS3_POS;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(state.dpmr_scrambled == 2U);

    state.dpmr_confirmed = 1;
    state.dpmr_confirm_weak_streak = 1;
    plant_checked_end(&state);
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    assert(call.end_reason == (uint8_t)DSD_CALL_END_TERMINATOR);
    assert(state.dpmr_scrambled == 0U);
    /* A checked end frame is a proven end of the transmission: its confirmation ends with it (DPMR_VOICE_BRIDGE shows a
       weak FS2 frame after it opens nothing until the next transmission proves itself). */
    assert(state.dpmr_confirmed == 0 && state.dpmr_confirm_weak_streak == 0);
    /* An unchecked FS3 leaves the confirmation standing. */
    state.dpmr_confirmed = 1;
    state.synctype = DSD_SYNC_DPMR_FS3_NEG;
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(state.dpmr_confirmed == 1);
}

// A passing CCH CRC-7 is the only thing dPMR can offer the SPS hunt as proof its profile is
// right, and it has to keep offering it: 372 symbols read behind a 12-symbol matcher is more
// than noise can be allowed to buy (#391, #407).
static void
test_dispatch_voice_verdicts_follow_the_cch_crc(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_POS;

    // A frame whose CCH decodes proves the profile, and stamps when it did.
    g_voice_crc_halves = 2;
    state.symbolcnt = 1000U;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.dpmr_cch_evidence == 1);
    assert(state.dpmr_cch_evidence_symbolcnt == 1000U);

    // One half is still a decode.
    g_voice_crc_halves = 1;
    state.symbolcnt = 1400U;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.dpmr_cch_evidence_symbolcnt == 1400U);

    // A frame that decodes nothing still rides the window a recent decode opened, and
    // must not extend it -- otherwise a single decode could hold the profile forever.
    g_voice_crc_halves = 0;
    state.symbolcnt = 1400U + 4799U;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
    assert(state.dpmr_cch_evidence_symbolcnt == 1400U);

    // Past the window, the profile is given up.
    state.symbolcnt = 1400U + 4800U;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);

    // And with no evidence at all, a sync alone never proves anything -- including at
    // symbol zero, where a stamp-only test would read as evidence.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_NEG;
    g_voice_crc_halves = 0;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_UNPRODUCTIVE);

    // FS1/FS3 with no checked frame behind them, and FS4, run no check, so they report the default rather than a
    // failure they never measured (protocol_dispatch.h).
    state.synctype = DSD_SYNC_DPMR_FS1_POS;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    state.synctype = DSD_SYNC_DPMR_FS3_POS;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
    state.synctype = DSD_SYNC_DPMR_FS4_NEG;
    assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);

    g_voice_crc_halves = 2;
}

// The sync word alone used to open a call row before the voice path ran at all, so noise
// clearing the 12-symbol matcher put rows on the air with no decode behind them (#407).
static void
test_dispatch_voice_sync_alone_opens_no_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DPMR_FS2_POS;

    g_voice_crc_halves = 0;
    dsd_dispatch_handle_dpmr(&opts, &state);

    dsd_call_snapshot call;
    assert(dsd_call_state_get(&state, 0U, &call) <= 0);

    // Nor once the transmission has proved itself: the frame publishes, from its checked half (issue #890).
    state.dpmr_confirmed = 1;
    dsd_dispatch_handle_dpmr(&opts, &state);
    assert(dsd_call_state_get(&state, 0U, &call) <= 0);

    g_voice_crc_halves = 2;
}

/* Issue #655: a CRC-failed half is played by the scrambling the transmission's last verified half signalled, so every
   transmission boundary the dispatcher sees forgets it before the next call: a header that opens a call (which then
   sets its own) and a checked end frame (issue #890). A frame that checked nothing is no boundary. */
static void
test_dispatch_boundaries_forget_the_scrambling(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const int unchecked[] = {DSD_SYNC_DPMR_FS1_POS, DSD_SYNC_DPMR_FS1_NEG, DSD_SYNC_DPMR_FS3_POS,
                                    DSD_SYNC_DPMR_FS3_NEG, DSD_SYNC_DPMR_FS4_POS, DSD_SYNC_DPMR_FS4_NEG};
    for (size_t i = 0; i < sizeof(unchecked) / sizeof(unchecked[0]); i++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        state.dpmr_scrambled = 2U;
        state.synctype = unchecked[i];
        (void)dsd_dispatch_handle_dpmr(&opts, &state);
        assert(state.dpmr_scrambled == 2U);
    }
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_scrambled = 2U;
    plant_checked_end(&state);
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(state.dpmr_scrambled == 0U);
    /* A clear voice header: the old verdict goes, the header's own (clear) comes. */
    state.dpmr_scrambled = 2U;
    plant_checked_header(&state, 0x0U, 0U, 0U);
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(state.dpmr_scrambled == 1U);
    // A voice frame keeps the transmission's verdict.
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dpmr_scrambled = 2U;
    state.synctype = DSD_SYNC_DPMR_FS2_POS;
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(state.dpmr_scrambled == 2U);
}

/* Issue #834: the content notes a scan quiet check hears through the content-evidence hook. */
static int g_quiet_proven;
static int g_quiet_partial;
static uint32_t g_quiet_hold_ms;

static void
record_quiet_note(dsd_state* state, int level, uint32_t hold_ms) {
    (void)state;
    if (level == DSD_CONTENT_EVIDENCE_PROVEN) {
        g_quiet_proven++;
    } else if (level == DSD_CONTENT_EVIDENCE_PARTIAL) {
        g_quiet_partial++;
        g_quiet_hold_ms = hold_ms;
    }
}

static void
quiet_notes_install(int on) {
    dsd_frame_sync_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof hooks);
    hooks.content_evidence = record_quiet_note;
    dsd_frame_sync_hooks_set(on ? &hooks : NULL);
    g_quiet_proven = 0;
    g_quiet_partial = 0;
    g_quiet_hold_ms = 0U;
}

/* Issue #834: an FS1 or FS3 with no checked frame behind it, and FS4, carry no check of their own, so to a scan quiet
   check each is a frame waiting for its first CCH verdict: PARTIAL for two FS2 spans (320 ms), once a frame, nothing
   PROVEN; nothing with the setting off. A checked header or end frame is PROVEN (issue #890). */
static void
test_dispatch_unchecked_frames_are_partial(void) {
    static dsd_opts opts;
    static dsd_state state;
    static const int frames[] = {DSD_SYNC_DPMR_FS1_POS, DSD_SYNC_DPMR_FS1_NEG, DSD_SYNC_DPMR_FS3_POS,
                                 DSD_SYNC_DPMR_FS3_NEG, DSD_SYNC_DPMR_FS4_POS, DSD_SYNC_DPMR_FS4_NEG};
    for (int on = 0; on <= 1; on++) {
        for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            opts.scan_skip_silent = on;
            state.synctype = frames[i];
            quiet_notes_install(1);
            assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PRODUCTIVE);
            assert(g_quiet_proven == 0);
            assert(g_quiet_partial == on);
            assert(!on || g_quiet_hold_ms == 320U);
            quiet_notes_install(0);
        }
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.scan_skip_silent = on;
        quiet_notes_install(1);
        plant_checked_header(&state, 0x3U, 0U, 0U);
        (void)dsd_dispatch_handle_dpmr(&opts, &state);
        plant_checked_header(&state, 0x0U, 0U, 0U);
        (void)dsd_dispatch_handle_dpmr(&opts, &state);
        plant_checked_end(&state);
        (void)dsd_dispatch_handle_dpmr(&opts, &state);
        assert(g_quiet_proven == (on ? 3 : 0));
        assert(g_quiet_partial == 0);
        quiet_notes_install(0);
    }
}

/* Issue #890: a radio waking powersaving receivers sends its Communications_Start header nine times back to back, one
   message frame (192 symbols) apart: eight powersave headers (MI_TYPE 111, WU 8 down to 1, TS 102 658 5.5.19.1 Table
   5.71) and the normal one, as the off-air dpmr fixture does. That is one call, not nine: one row, its capture closed
   once. A header one frame on that names another caller, and the same header after a gap, open calls of their own. */
static void
test_dispatch_powersave_headers_open_one_call(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_call_snapshot call;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.mbe_out_f = stdout;
    state.dpmr_color_code = -1;
    g_close_calls = 0;
    uint64_t epoch = 0U;
    for (int h = 0; h < 9; h++) {
        state.symbol_clock = 1000U + (192U * (uint64_t)h);
        plant_checked_header(&state, 0x0U, 0U, 0U);
        put_bits(state.dpmr_sync.checked_bits, 61, 3, h < 8 ? 7U : 0U);
        put_bits(state.dpmr_sync.checked_bits, 64, 8, h < 8 ? (uint32_t)(8 - h) : 0U);
        assert(dsd_dispatch_handle_dpmr(&opts, &state) == DSD_FRAME_VERDICT_PROFILE_PROVEN);
        assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
        if (h == 0) {
            epoch = call.epoch;
        }
        assert(call.epoch == epoch);
        assert(g_close_calls == 1);
    }

    /* One frame on, another caller: a call of its own. */
    state.symbol_clock += 192U;
    plant_checked_header(&state, 0x0U, 0U, 0U);
    put_bits(state.dpmr_sync.checked_bits, 28, 24, 11206075U);
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch != epoch);
    assert(strcmp(call.source_text, "7654321") == 0);
    epoch = call.epoch;

    /* The same header again after a gap: a new key-up. */
    state.symbol_clock += 192U * 5U;
    plant_checked_header(&state, 0x0U, 0U, 0U);
    put_bits(state.dpmr_sync.checked_bits, 28, 24, 11206075U);
    (void)dsd_dispatch_handle_dpmr(&opts, &state);
    assert(dsd_call_state_get(&state, 0U, &call) > 0 && call.epoch != epoch);
    assert(g_close_calls == 3);
}

int
main(void) {
    test_sync_pattern_lengths();
    test_sync_pattern_pairs();
    test_synctype_helpers();
    test_dispatch_close_only_syncs();
    test_dispatch_voice_sync_continues_a_confirmed_call_and_processes_voice();
    test_dispatch_unconfirmed_voice_sync_opens_no_capture();
    test_dispatch_end_sync_tightens_sync_loss_end();
    test_dispatch_unchecked_end_keeps_the_call();
    test_dispatch_checked_header_opens_its_call();
    test_dispatch_powersave_headers_open_one_call();
    test_dispatch_voice_verdicts_follow_the_cch_crc();
    test_dispatch_voice_sync_alone_opens_no_call();
    test_dispatch_boundaries_forget_the_scrambling();
    test_dispatch_unchecked_frames_are_partial();
    printf("DPMR_SYNC_DISPATCH: OK\n");
    return 0;
}
