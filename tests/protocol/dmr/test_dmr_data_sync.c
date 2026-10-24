// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused coverage for the cached DMR data-sync path.
 */

#include "dsd-neo/core/safe_api.h"

#include <assert.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_confidence.h"

static int g_hamming_ok;
static int g_golay_ok;
static uint8_t g_decoded_slot;
static uint8_t g_decoded_color;
static uint8_t g_decoded_burst;
static int g_handler_calls;
static int g_sm_calls;
static int g_sm_last_slot;
static int g_reset_calls;
static int g_skip_calls;
static int g_handler_slot;
static int g_handler_mono_slot;
static int g_handler_moves_carrier;
static int g_handler_writes_fix;
/* The handler observes the lane-0 call as a voice LC header the single-slot output folds there does (issue #637):
   0 none, 1 a new epoch for g_handler_target, 2 that epoch again (a repeated header). */
static int g_handler_observes;
static uint32_t g_handler_target;
static int g_live_dibit_index;
static int g_live_dibits[64];
static uint8_t g_live_reliability[64];
static int g_cach_calls;
static int g_debug_format_calls;
static uint8_t g_handler_burst;
static uint8_t g_handler_info[196];
static uint8_t g_handler_reliab[98];
/* The received identity as a snapshot published while the burst is dispatched would carry it. */
static dsd_received_identity g_handler_rxid;
static dmr_confidence_result g_confidence_result;
/* The live read that adopts a retune its capture recorded, where the carrier boundary moves the carrier count
   (dsd_engine_carrier_boundary(), issue #575); -1 for none. */
static int g_boundary_at;
/* The live read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there
   and stands erasures in for every symbol after it; -1 for none. */
static int g_interrupt_at;
static int g_confidence_calls;

static void
reset_fixture(void) {
    g_interrupt_at = -1;
    g_confidence_calls = 0;
    g_hamming_ok = 1;
    g_golay_ok = 1;
    g_decoded_slot = 1;
    g_decoded_color = 5;
    g_decoded_burst = 7;
    g_handler_calls = 0;
    g_sm_calls = 0;
    g_sm_last_slot = -1;
    g_reset_calls = 0;
    g_skip_calls = 0;
    g_handler_slot = -1;
    g_live_dibit_index = 0;
    g_cach_calls = 0;
    g_debug_format_calls = 0;
    g_handler_burst = 0;
    g_confidence_result = DMR_CONFIDENCE_LOCKED;
    g_boundary_at = -1;
    for (size_t i = 0; i < 64U; i++) {
        g_live_dibits[i] = 0;
        g_live_reliability[i] = 0;
    }
    DSD_MEMSET(g_handler_info, 0, sizeof(g_handler_info));
    DSD_MEMSET(g_handler_reliab, 0, sizeof(g_handler_reliab));
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
Hamming_7_4_decode(unsigned char* rx_bits) {
    if (g_hamming_ok == 0) {
        return false;
    }
    DSD_MEMSET(rx_bits, 0, 7U);
    rx_bits[1] = g_decoded_slot;
    return true;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
Golay_20_8_decode(unsigned char* rx_bits) {
    if (g_golay_ok == 0) {
        return false;
    }
    DSD_MEMSET(rx_bits, 0, 20U);
    rx_bits[0] = (unsigned char)((g_decoded_color >> 3U) & 1U);
    rx_bits[1] = (unsigned char)((g_decoded_color >> 2U) & 1U);
    rx_bits[2] = (unsigned char)((g_decoded_color >> 1U) & 1U);
    rx_bits[3] = (unsigned char)(g_decoded_color & 1U);
    rx_bits[4] = (unsigned char)((g_decoded_burst >> 3U) & 1U);
    rx_bits[5] = (unsigned char)((g_decoded_burst >> 2U) & 1U);
    rx_bits[6] = (unsigned char)((g_decoded_burst >> 1U) & 1U);
    rx_bits[7] = (unsigned char)(g_decoded_burst & 1U);
    return true;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    if (g_boundary_at >= 0 && g_live_dibit_index == g_boundary_at && state != NULL) {
        state->carrier_seq++;
    }
    if (g_live_dibit_index == g_interrupt_at && state != NULL) {
        state->input_interrupted = 1;
    }
    int index = g_live_dibit_index;
    if (index >= (int)(sizeof(g_live_dibits) / sizeof(g_live_dibits[0]))) {
        index = (int)(sizeof(g_live_dibits) / sizeof(g_live_dibits[0])) - 1;
    }
    if (out_soft != NULL) {
        out_soft->reliability = g_live_reliability[index];
        out_soft->llr[0] = (int16_t)g_live_reliability[index];
        out_soft->llr[1] = (int16_t)g_live_reliability[index];
    }
    g_live_dibit_index++;
    return g_live_dibits[index] & 3;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    g_skip_calls += count;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                       const uint8_t* reliab98) {
    (void)opts;
    g_handler_calls++;
    g_handler_slot = state->currentslot;
    g_handler_mono_slot = state->dmr_mono_rx_slot;
    if (g_handler_moves_carrier) {
        state->carrier_seq++;
    }
    g_handler_burst = databurst;
    g_handler_rxid = state->rx_id;
    if (g_handler_observes) {
        const dsd_call_observation observation = {
            .protocol = DSD_SYNC_DMR_BS_DATA_POS,
            .slot = 0U,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = g_handler_target,
            .policy_target_id = g_handler_target,
            .ota_source_id = 1001U,
        };
        (void)dsd_call_state_observe(state, &observation,
                                     g_handler_observes == 1 ? DSD_CALL_BOUNDARY_BEGIN : DSD_CALL_BOUNDARY_CONTINUE);
        /* The header's crypto publication, which moves the revision again as the real one does. */
        const dsd_call_crypto_update crypto = {.classification = DSD_CALL_CRYPTO_CLEAR, .audio_permitted = 1};
        (void)dsd_call_state_update_crypto(state, 0U, &crypto);
    }
    if (g_handler_writes_fix) {
        DSD_SNPRINTF(state->dmr_lrrp_gps[state->currentslot & 1], sizeof state->dmr_lrrp_gps[0], "%s", "lrrp-fix");
    }
    DSD_MEMCPY(g_handler_info, info, sizeof(g_handler_info));
    DSD_MEMCPY(g_handler_reliab, reliab98, sizeof(g_handler_reliab));
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]) {
    (void)opts;
    (void)state;
    (void)cach_bits;
    g_cach_calls++;
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_reset_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_sm_emit_data_sync(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    g_sm_calls++;
    g_sm_last_slot = slot;
}

size_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_debug_format_burst(char* out, size_t out_size, const dsd_state* state, uint8_t slot_index, uint8_t burst_type) {
    (void)state;
    (void)slot_index;
    g_debug_format_calls++;
    if (out_size == 0U) {
        return 0U;
    }
    return DSD_SNPRINTF(out, out_size, "debug-burst-%u", burst_type);
}

dmr_confidence_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_confidence_note_data_burst(dsd_state* state, unsigned int color_code) {
    (void)state;
    (void)color_code;
    g_confidence_calls++;
    return g_confidence_result;
}

static int
sync_char_to_dibit(char c) {
    return c == '3' ? 2 : 0;
}

static void
fill_sync_payload(int payload[90], const char* sync) {
    for (size_t i = 0; i < 24U; i++) {
        payload[66U + i] = sync_char_to_dibit(sync[i]);
    }
}

static void
prepare_state(dsd_state* state, int payload[90], dsd_dibit_soft_t soft[90]) {
    DSD_MEMSET(state, 0, sizeof(*state));
    for (size_t i = 0; i < 90U; i++) {
        payload[i] = (int)(i & 3U);
        soft[i].reliability = (uint8_t)(40U + i);
    }
    fill_sync_payload(payload, DMR_BS_DATA_SYNC);

    state->dmr_payload_p = payload + 90;
    state->dmr_soft_buf = soft;
    state->dmr_soft_p = soft + 90;
    state->dmr_stereo = 1;
    for (size_t i = 0; i < 144U; i++) {
        state->dmr_stereo_payload[i] = (int)((i + 1U) & 3U);
        state->dmr_stereo_reliab[i] = (uint8_t)(200U - (i % 64U));
    }
    for (size_t i = 0; i < 90U; i++) {
        state->dmr_stereo_payload[i] = payload[i];
    }
}

static void
prepare_live_symbols(uint8_t reliability, int dibit) {
    for (size_t i = 0; i < 64U; i++) {
        g_live_dibits[i] = dibit;
        g_live_reliability[i] = reliability;
    }
}

static void
test_data_sync_dispatches_burst_and_reliability(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(g_handler_slot == 1);
    assert(g_handler_burst == 7U);
    assert(g_sm_calls == 1);
    assert(g_sm_last_slot == 1);
    assert(g_reset_calls == 0);
    assert(g_skip_calls == 0);
    assert(state.currentslot == 1);
    assert(state.color_code == 5);
    assert(state.dmrburstR == 7U);
    assert(strcmp(state.slot1light, " slot1 ") == 0);
    assert(strcmp(state.slot2light, "[slot2]") == 0);
    assert(g_handler_reliab[0] == state.dmr_stereo_reliab[12]);
    assert(g_handler_reliab[48] == state.dmr_stereo_reliab[60]);
    assert(g_handler_reliab[49] == state.dmr_stereo_reliab[95]);
    assert(g_handler_reliab[97] == state.dmr_stereo_reliab[143]);
    assert(g_cach_calls == 1);
}

static void
test_ms_data_sync_bypasses_confidence_and_cach(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();

    opts.dmr_mono = 1;
    state.dmr_ms_mode = 1;
    g_confidence_result = DMR_CONFIDENCE_REJECT;
    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(g_reset_calls == 0);
    assert(g_cach_calls == 0);
    assert(state.dmr_color_code == 5);
    /* The slot type's checked colour code is the carrier's before the burst it heads is dispatched. */
    assert(g_handler_rxid.family == (uint8_t)DSD_RXID_FAMILY_DMR && g_handler_rxid.valid == DSD_RXID_ACCESS);
    assert(g_handler_rxid.access == 5U);

    /* A slot type that failed its Golay(20,8) names nothing. */
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.dmr_ms_mode = 1;
    g_golay_ok = 0;
    dmr_data_sync(&opts, &state);
    assert(state.rx_id.valid == 0U);
}

/* MS/direct mode's slot type colour code is the carrier's: another one than a decoded one is another system sharing the
   carrier, whose RAS the last system's signature bursts prove nothing about (dmr_ras_reset()). The same code, or the
   carrier's first, keeps the evidence. */
static void
test_ms_colour_change_resets_ras(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));

    /* The carrier's first code (16, not decoded) and the same code keep a partial and a confirmed RAS. */
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.dmr_ms_mode = 1;
    state.dmr_color_code = 16;
    dmr_ras_note_signature(&state);
    dmr_data_sync(&opts, &state);
    assert(state.dmr_color_code == 5 && state.dmr_ras_signatures == 1U);
    dmr_ras_note_signature(&state);
    assert(dmr_ras_confirmed(&state) && (state.rx_id.valid & DSD_RXID_DMR_RAS) != 0U);
    prepare_live_symbols(0, 0);
    g_live_dibit_index = 0;
    dmr_data_sync(&opts, &state);
    assert(dmr_ras_confirmed(&state) && (state.rx_id.valid & DSD_RXID_DMR_RAS) != 0U);

    /* Another code drops the confirmation and the part. */
    g_decoded_color = 6;
    g_live_dibit_index = 0;
    dmr_data_sync(&opts, &state);
    assert(state.dmr_color_code == 6);
    assert(!dmr_ras_confirmed(&state) && (state.rx_id.valid & DSD_RXID_DMR_RAS) == 0U);

    /* One signature, another code, one signature: not confirmed. */
    dmr_ras_note_signature(&state);
    g_decoded_color = 7;
    g_live_dibit_index = 0;
    dmr_data_sync(&opts, &state);
    dmr_ras_note_signature(&state);
    assert(!dmr_ras_confirmed(&state) && (state.rx_id.valid & DSD_RXID_DMR_RAS) == 0U);
}

/* Issue #637: a standalone data burst is judged by its own CACH slot, not by the slot the single-slot decoder proved
   for its voice lane, which is back for the voice that follows; a data burst the single-slot decoder reads with no
   slot evidence (an MS-sourced one) stays Checking rather than being taken for TS1. */
static void
test_data_burst_receive_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    dmr_data_sync(&opts, &state);
    assert(g_handler_calls == 1 && g_handler_mono_slot == DSD_DMR_MONO_RX_NONE);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS1);

    /* A carrier boundary during the burst forgets the proof for good: it is not put back onto the next carrier. */
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    g_handler_moves_carrier = 1; /* A replay retune while the burst dispatches. */
    dmr_data_sync(&opts, &state);
    g_handler_moves_carrier = 0;
    assert(g_handler_calls == 1 && state.carrier_seq == 1U);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_NONE);

    /* A standalone TS2 burst the receive policy rejects leaves no position in its lane's cache, though the voice
       lane's proven TS1 (admitted) comes back after it: the terminal judges the lane by that proof. */
    prepare_state(&state, payload, reliab);
    reset_fixture();
    opts.receive.filters[DSD_RECEIVE_SLOT].enabled = 1;
    opts.receive.filters[DSD_RECEIVE_SLOT].block.bits[0] = UINT64_C(1) << 2; /* --slot-block 2 */
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    g_decoded_slot = 1;
    g_handler_writes_fix = 1;
    dmr_data_sync(&opts, &state);
    assert(g_handler_calls == 1 && state.dmr_lrrp_gps[1][0] == '\0');
    g_decoded_slot = 0;
    reset_fixture();
    prepare_state(&state, payload, reliab);
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    g_decoded_slot = 0;
    dmr_data_sync(&opts, &state);
    assert(strcmp(state.dmr_lrrp_gps[0], "lrrp-fix") == 0); /* An admitted TS1 fix stays. */
    g_handler_writes_fix = 0;
    DSD_MEMSET(&opts.receive, 0, sizeof opts.receive);

    /* The single-slot output (-nm) folds a standalone burst onto lane 0 (dmr_flco_ctx_init()): it is judged by its own
       CACH slot while it dispatches, never as the voice lane's proven TS1, and the proof is put back after. */
    prepare_state(&state, payload, reliab);
    reset_fixture();
    opts.dmr_mono = 1;
    state.dmr_stereo = 0;
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    g_decoded_slot = 1;
    dmr_data_sync(&opts, &state);
    assert(g_handler_calls == 1 && g_handler_mono_slot == DSD_DMR_MONO_RX_TS2);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS1);
    opts.dmr_mono = 0;

    /* A voice LC header folded onto the lane opens the call the lane carries next, on the header's slot: when the call
       took it, naming the other slot sends the lane back to Checking, with that slot the candidate the next burst
       proves, so the proof the previous call left does not end the call. One naming the proven slot keeps it proven. A
       PI header, a header the call did not take (its LC refused), idle and terminator bursts put the proof back. */
    static const struct {
        uint8_t burst;
        uint8_t lane;
        uint8_t slot;
        int observes;
        uint8_t proof;
        uint8_t candidate;
    } headers[] = {
        {1U, DSD_DMR_MONO_RX_TS1, 1U, 1, DSD_DMR_MONO_RX_PENDING, DSD_DMR_MONO_RX_TS2},
        {1U, DSD_DMR_MONO_RX_TS2, 0U, 1, DSD_DMR_MONO_RX_PENDING, DSD_DMR_MONO_RX_TS1},
        {1U, DSD_DMR_MONO_RX_TS1, 0U, 1, DSD_DMR_MONO_RX_TS1, DSD_DMR_MONO_RX_TS1},
        {1U, DSD_DMR_MONO_RX_NONE, 1U, 1, DSD_DMR_MONO_RX_PENDING, DSD_DMR_MONO_RX_TS2},
        {1U, DSD_DMR_MONO_RX_TS1, 1U, 0, DSD_DMR_MONO_RX_TS1, DSD_DMR_MONO_RX_NONE}, /* LC refused */
        {0U, DSD_DMR_MONO_RX_TS1, 1U, 0, DSD_DMR_MONO_RX_TS1, DSD_DMR_MONO_RX_NONE}, /* PI */
        {9U, DSD_DMR_MONO_RX_TS1, 1U, 0, DSD_DMR_MONO_RX_TS1, DSD_DMR_MONO_RX_NONE}, /* idle */
        {2U, DSD_DMR_MONO_RX_TS1, 1U, 0, DSD_DMR_MONO_RX_TS1, DSD_DMR_MONO_RX_NONE}, /* terminator */
    };

    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); i++) {
        prepare_state(&state, payload, reliab);
        reset_fixture();
        opts.dmr_mono = 1;
        state.dmr_stereo = 0;
        state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
        state.dmr_mono_rx_slot = headers[i].lane;
        state.dmr_mono_rx_candidate = DSD_DMR_MONO_RX_NONE;
        g_decoded_slot = headers[i].slot;
        g_decoded_burst = headers[i].burst;
        g_handler_observes = headers[i].observes;
        g_handler_target = 100U + (uint32_t)i;
        dmr_data_sync(&opts, &state);
        assert(g_handler_calls == 1 && g_handler_burst == headers[i].burst);
        assert(state.dmr_mono_rx_slot == headers[i].proof);
        assert(state.dmr_mono_rx_candidate == headers[i].candidate);
        dsd_state_ext_free_all(&state);
    }
    /* The call a header took is recognised by its epoch as well as its revision: a new epoch starts from a zeroed
       snapshot, so a header-only call can end on the very revision the call before it ended on. And a repeated header
       continues the epoch it opened. */
    for (int round = 0; round < 3; round++) {
        prepare_state(&state, payload, reliab);
        reset_fixture();
        opts.dmr_mono = 1;
        state.dmr_stereo = 0;
        state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
        g_handler_target = 501U;
        if (round > 0) {
            /* An earlier TS2 header-only call with the same revision a new epoch reaches, or the epoch to continue. */
            state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS2;
            g_decoded_slot = 1;
            g_decoded_burst = 1;
            g_handler_observes = 1;
            dmr_data_sync(&opts, &state);
            reset_fixture();
        }
        state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
        state.dmr_mono_rx_candidate = DSD_DMR_MONO_RX_NONE;
        g_decoded_slot = 1;
        g_decoded_burst = 1;
        g_handler_target = 501U;
        g_handler_observes = round == 2 ? 2 : 1;
        dmr_data_sync(&opts, &state);
        assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_PENDING && state.dmr_mono_rx_candidate == DSD_DMR_MONO_RX_TS2);
        dsd_state_ext_free_all(&state);
    }
    g_handler_observes = 0;
    /* A header that never reaches its handler (the confidence gate rejects it) or whose TACT fails proves nothing. */
    for (int failure = 0; failure < 2; failure++) {
        prepare_state(&state, payload, reliab);
        reset_fixture();
        opts.dmr_mono = 1;
        state.dmr_stereo = 0;
        state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
        state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
        g_decoded_slot = 1;
        g_decoded_burst = 1;
        if (failure == 0) {
            g_confidence_result = DMR_CONFIDENCE_REJECT;
        } else {
            g_hamming_ok = 0;
        }
        dmr_data_sync(&opts, &state);
        assert(g_handler_calls == 0);
        assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS1);
    }
    /* The two-slot output judges every burst by its own slot: no lane to send back to Checking. */
    prepare_state(&state, payload, reliab);
    reset_fixture();
    opts.dmr_mono = 0;
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    g_decoded_slot = 1;
    g_decoded_burst = 1;
    dmr_data_sync(&opts, &state);
    assert(g_handler_calls == 1 && state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS1);
    g_decoded_slot = 0;
    opts.dmr_mono = 0;

    prepare_state(&state, payload, reliab);
    reset_fixture();
    opts.dmr_mono = 1;
    state.dmr_ms_mode = 1;
    state.synctype = DSD_SYNC_DMR_MS_DATA;
    dmr_data_sync(&opts, &state);
    assert(g_handler_calls == 1 && g_handler_mono_slot == DSD_DMR_MONO_RX_PENDING);
    opts.dmr_mono = 0;
}

static void
test_mono_option_keeps_bs_confidence_and_cach(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();

    opts.dmr_mono = 1;
    g_confidence_result = DMR_CONFIDENCE_REJECT;
    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 0);
    assert(g_reset_calls == 1);
    assert(g_cach_calls == 0);

    prepare_state(&state, payload, reliab);
    reset_fixture();
    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(g_reset_calls == 0);
    assert(g_cach_calls == 1);
}

static void
test_cach_failure_resets_without_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    g_hamming_ok = 0;

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 0);
    assert(g_sm_calls == 0);
    assert(g_reset_calls == 1);
    assert(g_skip_calls == 0);
}

static void
test_golay_failure_resets_and_skips_live_tail(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    g_golay_ok = 0;
    state.dmr_stereo = 0;
    state.min = 0;
    state.lmid = 10;
    state.center = 20;
    state.umid = 30;
    state.max = 40;
    prepare_live_symbols(40U, 3);

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 0);
    assert(g_sm_calls == 0);
    assert(g_reset_calls == 1);
    assert(g_skip_calls == 66);
    assert(g_live_dibit_index == 5);
}

static void
test_live_second_half_reliability_and_debug_output(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.dmr_stereo = 0;
    state.min = 0;
    state.lmid = 10;
    state.center = 20;
    state.umid = 30;
    state.max = 40;
    opts.dmr_debug_burst = 1;
    prepare_live_symbols(173U, 3);

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(g_handler_burst == 7U);
    assert(g_handler_reliab[49] == 173U);
    assert(g_handler_reliab[97] == 173U);
    assert(state.dmr_stereo_reliab[90] == 173U);
    assert(state.dmr_stereo_reliab[95] == 173U);
    assert(state.dmr_stereo_reliab[143] == 173U);
    assert(g_skip_calls == 66);
    assert(g_live_dibit_index == 54);
    assert(g_debug_format_calls == 1);
    assert(g_cach_calls == 1);
}

/* Issue #575: the carrier boundary moved the carrier count while the burst's live half was read (a replay read adopting
   a recorded retune). The CACH read before it may complete a short LC of the carrier left, which would then decode
   after the boundary ended that carrier's calls: nothing in the burst dispatches, the blocks and CACH fragments gathered
   go, and no decode error is reported. */
static void
test_live_half_split_by_a_carrier_boundary_is_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.dmr_stereo = 0;
    state.min = 0;
    state.lmid = 10;
    state.center = 20;
    state.umid = 30;
    state.max = 40;
    prepare_live_symbols(173U, 3);
    g_boundary_at = 20;

    dmr_data_sync(&opts, &state);

    assert(state.carrier_seq == 1U);
    assert(g_live_dibit_index == 54);
    assert(g_handler_calls == 0);
    assert(g_sm_calls == 0);
    assert(g_cach_calls == 0);
    assert(g_reset_calls == 1);
    assert(g_skip_calls == 66);
}

/* A carrier boundary while the slot type's live dibits are read (a replay read adopting a recorded retune): the slot
   type straddles two carriers, so its colour code is neither the carrier's received identity nor confidence evidence,
   in either mode, and the burst dispatches nothing. */
static void
test_slot_type_split_by_a_carrier_boundary_notes_nothing(void) {
    for (int ms = 0; ms < 2; ms++) {
        static dsd_opts opts;
        static dsd_state state;
        static int payload[90];
        static dsd_dibit_soft_t reliab[90];
        DSD_MEMSET(&opts, 0, sizeof(opts));
        prepare_state(&state, payload, reliab);
        reset_fixture();
        state.dmr_stereo = 0;
        state.dmr_ms_mode = ms;
        state.color_code = 9;
        state.dmr_color_code = 9;
        state.min = 0;
        state.lmid = 10;
        state.center = 20;
        state.umid = 30;
        state.max = 40;
        prepare_live_symbols(173U, 3);
        g_boundary_at = 2;

        dmr_data_sync(&opts, &state);

        assert(state.carrier_seq == 1U);
        assert(state.rx_id.valid == 0U);
        assert(g_confidence_calls == 0);
        assert(state.color_code == 9 && state.dmr_color_code == 9);
        assert(g_handler_calls == 0);
        assert(g_reset_calls == 1);
    }
}

/* A Signal search that leaves the burst (its visit ended, or a command came), or a live read that gives up on a stalled
   input, cuts its live half: from the cut every symbol is an erasure getSymbol() stood in. The slot type decodes as
   soon as its five live dibits are read, so one the cut reached would set the colour code and feed the confidence that
   reports DMR to the search; a cut burst does neither, dispatches nothing, and is no FEC error. A cut in the second
   half leaves the slot type, read whole before it, as it was. */
static void
test_live_half_cut_by_an_input_interruption_is_dropped(void) {
    static const int cuts[3] = {0, 4, 20};
    static const int confidence_notes[3] = {0, 0, 1};
    for (int c = 0; c < 3; c++) {
        static dsd_opts opts;
        static dsd_state state;
        static int payload[90];
        static dsd_dibit_soft_t reliab[90];
        DSD_MEMSET(&opts, 0, sizeof(opts));
        prepare_state(&state, payload, reliab);
        reset_fixture();
        state.dmr_stereo = 0;
        state.color_code = 9;
        state.dmrburstL = 13;
        state.dmrburstR = 13;
        state.min = 0;
        state.lmid = 10;
        state.center = 20;
        state.umid = 30;
        state.max = 40;
        prepare_live_symbols(173U, 3);
        g_interrupt_at = cuts[c];

        dmr_data_sync(&opts, &state);

        assert(state.input_interrupted == 1);
        assert(state.carrier_seq == 0U);
        assert(g_live_dibit_index == 54);
        assert(g_confidence_calls == confidence_notes[c]);
        assert(state.color_code == (confidence_notes[c] != 0 ? 5 : 9));
        assert(g_handler_calls == 0);
        assert(g_sm_calls == 0);
        assert(g_cach_calls == 0);
        assert(g_reset_calls == 1);
        assert(g_skip_calls == 66);
    }
}

static uint8_t
run_live_reliability(uint8_t reliability) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.dmr_stereo = 0;
    state.min = 0;
    state.lmid = 10;
    state.center = 20;
    state.umid = 30;
    state.max = 40;
    prepare_live_symbols(reliability, 3);

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(g_live_dibit_index == 54);
    return g_handler_reliab[49];
}

static void
test_live_reliability_is_forwarded_unscaled(void) {
    assert(run_live_reliability(25U) == 25U);
    assert(run_live_reliability(128U) == 128U);
    assert(run_live_reliability(241U) == 241U);
}

static void
test_direct_mode_sync_overrides_cach_slot(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    fill_sync_payload(payload, DMR_DIRECT_MODE_TS1_DATA_SYNC);
    for (size_t i = 0; i < 24U; i++) {
        state.dmr_stereo_payload[66U + i] = payload[66U + i];
    }
    g_decoded_slot = 1;

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(g_handler_slot == 0);
    assert(g_sm_calls == 1);
    assert(g_sm_last_slot == 0);
    assert(state.currentslot == 0);
    assert(strcmp(state.slot1light, "[sLoT1]") == 0);
    assert(strcmp(state.slot2light, "[DMODE]") == 0);
}

static void
test_confidence_pending_and_reject_gate_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    state.dmr_stereo = 0;
    state.min = 0;
    state.lmid = 10;
    state.center = 20;
    state.umid = 30;
    state.max = 40;
    prepare_live_symbols(40U, 3);
    g_confidence_result = DMR_CONFIDENCE_PENDING;

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 0);
    assert(g_sm_calls == 0);
    assert(g_cach_calls == 0);
    assert(g_reset_calls == 0);
    assert(state.dmrburstR == 7U);

    prepare_state(&state, payload, reliab);
    reset_fixture();
    g_confidence_result = DMR_CONFIDENCE_REJECT;

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 0);
    assert(g_sm_calls == 0);
    assert(g_reset_calls == 1);
    assert(state.dmrburstR == 7U);
}

static void
test_connect_plus_idle_bursts_clear_tuned_sync_times(void) {
    static dsd_opts opts;
    static dsd_state state;
    static int payload[90];
    static dsd_dibit_soft_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();

    opts.trunk_enable = 1;
    opts.trunk_is_tuned = 1;
    state.is_con_plus = 1;
    state.dmrburstL = 9;
    state.last_cc_sync_time = 10;
    state.last_vc_sync_time = 20;
    state.last_cc_sync_time_m = 30.0;
    state.last_vc_sync_time_m = 40.0;
    g_decoded_slot = 1;
    g_decoded_burst = 9;

    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(state.dmrburstL == 9);
    assert(state.dmrburstR == 9);
    assert(state.last_cc_sync_time == 0);
    assert(state.last_vc_sync_time == 0);
    assert(state.last_cc_sync_time_m == 0.0);
    assert(state.last_vc_sync_time_m == 0.0);
}

int
main(void) {
    test_data_sync_dispatches_burst_and_reliability();
    test_ms_data_sync_bypasses_confidence_and_cach();
    test_ms_colour_change_resets_ras();
    test_data_burst_receive_slot();
    test_mono_option_keeps_bs_confidence_and_cach();
    test_cach_failure_resets_without_dispatch();
    test_golay_failure_resets_and_skips_live_tail();
    test_live_second_half_reliability_and_debug_output();
    test_live_half_split_by_a_carrier_boundary_is_dropped();
    test_live_half_cut_by_an_input_interruption_is_dropped();
    test_slot_type_split_by_a_carrier_boundary_notes_nothing();
    test_live_reliability_is_forwarded_unscaled();
    test_direct_mode_sync_overrides_cach_slot();
    test_confidence_pending_and_reject_gate_dispatch();
    test_connect_plus_idle_bursts_clear_tuned_sync_times();
    DSD_FPRINTF(stdout, "DMR_DATA_SYNC: OK\n");
    return 0;
}
