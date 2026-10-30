// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(performance-no-int-to-ptr)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dmr_confidence.h"
#include "dmr_mono_slot.h"
#include "dmr_vcrypt.h"
#include "dsd-neo/core/frontend_types.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

volatile uint8_t exitflag = 0;

static uint8_t g_dibits[144 * 8];
static int g_bootstrap_payload[90];
static size_t g_dibit_index = 0;
/* The read that adopts a retune its capture recorded, where the carrier boundary moves the carrier count
   (dsd_engine_carrier_boundary(), issue #575); SIZE_MAX for none. */
static size_t g_boundary_at = SIZE_MAX;
/* The read a Signal search's yield (or a stalled live read) interrupts: getSymbol() records the interruption there and
   stands erasures in for every symbol after it; SIZE_MAX for none. */
static size_t g_interrupt_at = SIZE_MAX;
static int g_continuous_voice;
static unsigned int g_open_left_calls;
static unsigned int g_open_right_calls;
static unsigned int g_close_left_calls;
static unsigned int g_close_right_calls;
static unsigned int g_process_mbe_calls;
static uint8_t g_mbe_slots[64]; /* the slot each decoded frame went to, in order */
static unsigned int g_play_ss3_calls;
static unsigned int g_ui_redraw_calls;
static unsigned int g_history_calls[2];
static unsigned int g_current_calls[2];
static unsigned int g_data_sync_calls;
static uint8_t g_data_sync_reliability[144];
static unsigned int g_debug_dump_calls;
static uint8_t g_debug_dump_last_slot;
static uint8_t g_debug_dump_last_type;
static unsigned int g_cach_calls;
static unsigned int g_reset_blocks_calls;
static unsigned int g_alg_reset_calls;
static unsigned int g_vcrypt_start_calls;
static unsigned int g_vcrypt_end_calls;
static int g_vcrypt_last_pos;
static unsigned int g_anchor_calls;
static unsigned int g_late_entry_calls;
static uint8_t g_late_entry_last_vc;
static unsigned int g_sm_voice_sync_calls;
static unsigned int g_sm_voice_sync_calls_at_first_mbe;
static int g_sm_voice_sync_last_slot;
static unsigned int g_sm_tick_calls;
static unsigned int g_confidence_reset_calls;
static unsigned int g_confidence_reset_slot_calls;
static unsigned int g_confidence_reset_last_slot;
static unsigned int g_confidence_voice_sync_calls;
static unsigned int g_confidence_voice_burst_calls;
static dmr_confidence_result g_confidence_result = DMR_CONFIDENCE_LOCKED;
static int g_voice_slot_open = 1;
static int g_any_voice_open = 0;
/* Whether the vocoder stub stages decoded media for the stereo mixes, as the vocoder does while the audible-audio stamp
   is armed (issue #574). */
static int g_stage_mix_media = 0;
/* A Capacity Plus or XPT rest channel's retune, which the SLCO in a CACH makes: the dmr_cach() call that accepts it
   (1-based; 0 for none) requests the received identity's drop of the access code, as the engine's trunk tuning does. */
static unsigned int g_cach_retune_at = 0;
/* The colour code each burst's checked EMB notes in the received identity, as the confidence gate's evidence does
   (dmr_confidence_note_evidence()); 0 entries note nothing. */
static uint8_t g_burst_colour_codes[8];
/* The received identity each frontend publication copied, in order. */
static dsd_received_identity g_published_rx_id[8];

static void
reset_spies(void) {
    g_dibit_index = 0;
    g_boundary_at = SIZE_MAX;
    g_interrupt_at = SIZE_MAX;
    g_continuous_voice = 0;
    g_open_left_calls = 0;
    g_open_right_calls = 0;
    g_close_left_calls = 0;
    g_close_right_calls = 0;
    g_process_mbe_calls = 0;
    DSD_MEMSET(g_mbe_slots, 0xFF, sizeof(g_mbe_slots));
    g_play_ss3_calls = 0;
    g_ui_redraw_calls = 0;
    g_history_calls[0] = 0;
    g_history_calls[1] = 0;
    g_current_calls[0] = 0;
    g_current_calls[1] = 0;
    g_data_sync_calls = 0;
    DSD_MEMSET(g_data_sync_reliability, 0, sizeof(g_data_sync_reliability));
    g_debug_dump_calls = 0;
    g_debug_dump_last_slot = 0xFFU;
    g_debug_dump_last_type = 0;
    g_cach_calls = 0;
    g_reset_blocks_calls = 0;
    g_alg_reset_calls = 0;
    g_vcrypt_start_calls = 0;
    g_vcrypt_end_calls = 0;
    g_vcrypt_last_pos = -2;
    g_anchor_calls = 0;
    g_late_entry_calls = 0;
    g_late_entry_last_vc = 0;
    g_sm_voice_sync_calls = 0;
    g_sm_voice_sync_calls_at_first_mbe = 0;
    g_sm_voice_sync_last_slot = -1;
    g_sm_tick_calls = 0;
    g_confidence_reset_calls = 0;
    g_confidence_reset_slot_calls = 0;
    g_confidence_reset_last_slot = 0xFFU;
    g_confidence_voice_sync_calls = 0;
    g_confidence_voice_burst_calls = 0;
    g_confidence_result = DMR_CONFIDENCE_LOCKED;
    g_voice_slot_open = 1;
    g_any_voice_open = 0;
    g_stage_mix_media = 0;
    g_cach_retune_at = 0;
    DSD_MEMSET(g_burst_colour_codes, 0, sizeof(g_burst_colour_codes));
    DSD_MEMSET(g_published_rx_id, 0, sizeof(g_published_rx_id));
    exitflag = 0;
}

static void
set_cach_tact_bit(size_t burst_start, uint8_t tact_index, uint8_t value) {
    static const int cach_interleave[24] = {
        0, 7, 8, 9, 1, 10, 11, 12, 2, 13, 14, 15, 3, 16, 4, 17, 18, 19, 5, 20, 21, 22, 6, 23,
    };
    for (size_t packed = 0; packed < 24U; packed++) {
        if (cach_interleave[packed] != tact_index) {
            continue;
        }
        uint8_t mask = (uint8_t)(packed % 2U == 0U ? 2U : 1U);
        if (value != 0U) {
            g_dibits[burst_start + (packed / 2U)] |= mask;
        } else {
            g_dibits[burst_start + (packed / 2U)] &= (uint8_t)~mask;
        }
        return;
    }
}

static void
set_sync_dibits(size_t burst_start, const char* sync) {
    const size_t sync_offset = burst_start + 12U + 36U + 18U;
    for (size_t i = 0; sync[i] != '\0'; i++) {
        g_dibits[sync_offset + i] = (sync[i] == '3') ? 2U : 0U;
    }
}

static void
load_single_burst_stream(uint8_t slot, const char* sync) {
    DSD_MEMSET(g_dibits, 0, sizeof(g_dibits));
    reset_spies();

    g_dibits[12U + 16U] = 1U;
    g_dibits[144U + 12U + 16U] = 1U;
    set_cach_tact_bit(0, 1, slot);
    set_sync_dibits(0, sync);
}

static void
load_voice_burst_stream(void) {
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
}

static void
load_bootstrap_voice_stream(uint8_t slot) {
    load_single_burst_stream(slot, DMR_BS_VOICE_SYNC);
    for (size_t i = 0; i < 90U; i++) {
        g_bootstrap_payload[i] = g_dibits[i];
    }
    DSD_MEMSET(g_dibits + 144U, 0, 144U);
    g_dibit_index = 90U;
}

int
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)out_analog_signal;
    if (state != NULL) {
        state->symbol_clock++;
    }
    if (g_continuous_voice) {
        /* Bound a broken escape so this regression fails instead of hanging. */
        assert(g_dibit_index < (size_t)144U * 80U);
        return g_dibits[g_dibit_index++ % 288U] & 0x3U;
    }
    if (g_dibit_index >= sizeof(g_dibits)) {
        return 0;
    }
    return g_dibits[g_dibit_index++] & 0x3U;
}

int
getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft) {
    (void)opts;
    if (state != NULL) {
        state->symbol_clock++;
    }
    if (g_dibit_index == g_boundary_at && state != NULL) {
        state->carrier_seq++;
    }
    if (g_dibit_index == g_interrupt_at && state != NULL) {
        state->input_interrupted = 1;
    }
    if (g_continuous_voice) {
        if (out_soft != NULL) {
            DSD_MEMSET(out_soft, 0, sizeof(*out_soft));
            out_soft->reliability = 255U;
        }
        /* Bound a broken escape so this regression fails instead of hanging. */
        assert(g_dibit_index < (size_t)144U * 80U);
        return g_dibits[g_dibit_index++ % 288U] & 0x3U;
    }
    if (g_dibit_index >= sizeof(g_dibits)) {
        if (out_soft != NULL) {
            DSD_MEMSET(out_soft, 0, sizeof(*out_soft));
        }
        return 0;
    }
    if (out_soft != NULL) {
        out_soft->reliability = (uint8_t)((g_dibit_index % 251U) + 1U);
        out_soft->llr[0] = 0;
        out_soft->llr[1] = 0;
    }
    return g_dibits[g_dibit_index++] & 0x3U;
}

bool
Hamming_7_4_decode(unsigned char* rxBits) {
    (void)rxBits;
    return true;
}

bool
QR_16_7_6_decode(unsigned char* rxBits) {
    (void)rxBits;
    return true;
}

void
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    exitflag = 1;
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    (void)format;
    DSD_SNPRINTF(out, out_size, "%s", "00:00:00");
    return 1;
}

FILE*
dsd_fopen_private(const char* path, const char* mode) {
    (void)path;
    (void)mode;
    return NULL;
}

/* The capture openers of the revision before issue #661, kept with their counters so this harness also links and runs
   against it: the first frame written creates the capture now, so a voice burst opens nothing here. */
// NOLINTNEXTLINE(misc-use-internal-linkage): main's dispatcher links against this stub.
void openMbeOutFile(dsd_opts* opts, dsd_state* state);
// NOLINTNEXTLINE(misc-use-internal-linkage): main's dispatcher links against this stub.
void openMbeOutFileR(dsd_opts* opts, dsd_state* state);

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_open_left_calls++;
    opts->mbe_out_f = (FILE*)(uintptr_t)1;
}

void
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_open_right_calls++;
    opts->mbe_out_fR = (FILE*)(uintptr_t)1;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_close_left_calls++;
    opts->mbe_out_f = NULL;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)state;
    g_close_right_calls++;
    opts->mbe_out_fR = NULL;
}

/* DMR closes only its own captures, named as DMR whatever state->synctype holds (issue #661); the owner filter is
   tested in CORE_MBE_FILE_IO, and this counts the closes as the closers above do. */
void
dsd_mbe_out_close_protocol(dsd_opts* opts, dsd_state* state, int slot_mask, int synctype) {
    assert(DSD_SYNC_IS_DMR(synctype));
    if ((slot_mask & DSD_MBE_OUT_SLOT1) != 0 && opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if ((slot_mask & DSD_MBE_OUT_SLOT2) != 0 && opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }
}

/* The vocoder's writer creates a slot's capture with the first frame it records (issue #661). */
void
// cppcheck-suppress constParameterPointer -- stub; signature fixed by dsd-neo/core/vocoder.h
processMbeFrame(dsd_opts* opts, dsd_state* state, dsd_voice_protocol protocol, char imbe_fr[8][23], char ambe_fr[4][24],
                char imbe7100_fr[7][24]) {
    assert(protocol == DSD_VOICE_PROTOCOL_DMR);
    if (opts->mbe_out_dir[0] != 0) {
        if (state->currentslot == 1 && opts->mbe_out_fR == NULL) {
            opts->mbe_out_fR = (FILE*)(uintptr_t)1;
        } else if (state->currentslot == 0 && opts->mbe_out_f == NULL) {
            opts->mbe_out_f = (FILE*)(uintptr_t)1;
        }
    }
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    if (g_process_mbe_calls == 0U) {
        g_sm_voice_sync_calls_at_first_mbe = g_sm_voice_sync_calls;
    }
    if (g_process_mbe_calls < sizeof(g_mbe_slots)) {
        g_mbe_slots[g_process_mbe_calls] = (uint8_t)state->currentslot;
    }
    g_process_mbe_calls++;
    if (g_stage_mix_media) {
        dsd_audio_dmr_mix_media_staged(state->currentslot, DSD_DMR_MIX_MEDIA_SHORT);
    }
}

void
playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_play_ss3_calls++;
}

int
dsd_telemetry_is_active(void) {
    return 1;
}

void
dsd_telemetry_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    if (state != NULL && g_ui_redraw_calls < sizeof(g_published_rx_id) / sizeof(g_published_rx_id[0])) {
        g_published_rx_id[g_ui_redraw_calls] = state->rx_id;
    }
    g_ui_redraw_calls++;
}

void
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    g_history_calls[slot & 1U]++;
    g_current_calls[slot & 1U]++;
}

void
tyt16_ambe2_codeword_keystream(const dsd_state* state, char ambe_fr[4][24], int fnum) {
    (void)state;
    (void)ambe_fr;
    (void)fnum;
}

void
csi72_ambe2_codeword_keystream(dsd_state* state, char ambe_fr[4][24]) {
    (void)state;
    (void)ambe_fr;
}

void
// cppcheck-suppress constParameterPointer -- stub; signature fixed by dsd-neo/protocol/dmr/dmr.h
dmr_data_sync(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    DSD_MEMCPY(g_data_sync_reliability, state->dmr_stereo_reliab, sizeof(g_data_sync_reliability));
    g_data_sync_calls++;
}

void
dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                       const uint8_t* reliab98) {
    (void)opts;
    (void)state;
    (void)info;
    (void)reliab98;
    (void)databurst;
}

void
dmr_debug_dump_burst(const dsd_opts* opts, const dsd_state* state, uint8_t slot_index, uint8_t burst_type) {
    (void)opts;
    (void)state;
    g_debug_dump_calls++;
    g_debug_dump_last_slot = slot_index;
    g_debug_dump_last_type = burst_type;
}

uint8_t
dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]) {
    (void)opts;
    (void)cach_bits;
    g_cach_calls++;
    if (g_cach_calls == g_cach_retune_at) {
        dsd_rxid_request_trunk_tune(state);
    }
    return 0;
}

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_reset_blocks_calls++;
}

void
dmr_alg_refresh(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dmr_alg_reset(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_alg_reset_calls++;
}

/* The voice crypto continuity (dmr_vcrypt.c) and the symbol anchor are their own units' business: these spies only
   see that the loop reports its voice syncs, VC6s and frame positions (issue #729). */
void
dmr_vcrypt_superframe_start(dsd_state* state, uint8_t slot) {
    (void)state;
    (void)slot;
    g_vcrypt_start_calls++;
}

void
dmr_vcrypt_hytera_vc6_step(dsd_state* state, uint8_t slot) {
    (void)state;
    (void)slot;
}

void
dmr_vcrypt_superframe_end(dsd_state* state, uint8_t slot) {
    (void)state;
    (void)slot;
    g_vcrypt_end_calls++;
}

void
dmr_vcrypt_set_frame(dsd_state* state, uint8_t slot, uint8_t vc, uint8_t frame) {
    (void)state;
    (void)slot;
    g_vcrypt_last_pos = (int)(vc - 1U) * 3 + (int)frame;
}

void
dmr_vcrypt_clear_frame(dsd_state* state, uint8_t slot) {
    (void)state;
    (void)slot;
}

void
dsd_symbol_arm_sync_anchor(dsd_state* state) {
    (void)state;
}

void
dsd_symbol_note_sync_anchor(const dsd_state* state, const char* sync_dibits, int count) {
    (void)state;
    (void)sync_dibits;
    (void)count;
    g_anchor_calls++;
}

void
hytera_enhanced_alg_refresh(dsd_state* state) {
    (void)state;
}

void
dmr_late_entry_mi_fragment(dsd_opts* opts, dsd_state* state, uint8_t vc, uint8_t ambe_fr[4][24],
                           uint8_t ambe_fr2[4][24], uint8_t ambe_fr3[4][24]) {
    (void)opts;
    (void)state;
    (void)vc;
    (void)ambe_fr;
    (void)ambe_fr2;
    (void)ambe_fr3;
    g_late_entry_calls++;
    g_late_entry_last_vc = vc;
}

void
dmr_sbrc(dsd_opts* opts, dsd_state* state, uint8_t power) {
    (void)opts;
    (void)state;
    (void)power;
}

void
dmr_sm_emit_voice_sync(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    g_sm_voice_sync_calls++;
    g_sm_voice_sync_last_slot = slot;
}

dmr_sm_ctx_t*
dmr_sm_get_ctx(void) {
    static dmr_sm_ctx_t ctx;
    return &ctx;
}

void
dmr_sm_tick_ctx(dmr_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    (void)ctx;
    (void)opts;
    (void)state;
    g_sm_tick_calls++;
}

void
dmr_confidence_reset(dsd_state* state) {
    (void)state;
    g_confidence_reset_calls++;
}

void
dmr_confidence_reset_slot(dsd_state* state, unsigned int slot) {
    (void)state;
    g_confidence_reset_slot_calls++;
    g_confidence_reset_last_slot = slot;
}

void
dmr_confidence_note_voice_sync(dsd_state* state, unsigned int slot) {
    (void)state;
    (void)slot;
    g_confidence_voice_sync_calls++;
}

dmr_confidence_result
dmr_confidence_note_voice_burst(dsd_state* state, unsigned int slot, unsigned int color_code) {
    (void)slot;
    (void)color_code;
    if (g_confidence_voice_burst_calls < sizeof(g_burst_colour_codes)
        && g_burst_colour_codes[g_confidence_voice_burst_calls] != 0U) {
        dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE,
                             g_burst_colour_codes[g_confidence_voice_burst_calls]);
    }
    g_confidence_voice_burst_calls++;
    return g_confidence_result;
}

int
dmr_confidence_voice_slot_open(const dsd_state* state, unsigned int slot) {
    (void)state;
    (void)slot;
    return g_voice_slot_open;
}

int
dmr_confidence_any_voice_open(const dsd_state* state) {
    (void)state;
    return g_any_voice_open;
}

/* The receive policy's verdict for the burst's slot (issue #637): admitted unless a test rejects it. */
static int g_receive_rejects;

int
dsd_receive_allows(const dsd_opts* opts, const dsd_state* state, int protocol, int slot) {
    (void)opts;
    (void)state;
    (void)protocol;
    (void)slot;
    return !g_receive_rejects;
}

/* Issue #637: the single-slot lane's over-the-air slot. A repeater's is proven by two bursts in a row naming it and
   stays proven; a direct-mode sync word names its own; any other burst names none and stays Checking. */
static void
test_mono_lane_slot_proof(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    dmr_mono_slot_begin(&state);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_PENDING);
    dmr_mono_slot_note(&state, 1, 0U); /* A corrupted TACT can name the other slot once. */
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_PENDING);
    dmr_mono_slot_note(&state, 1, 1U);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_PENDING);
    dmr_mono_slot_note(&state, 1, 1U);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS2);
    dmr_mono_slot_note(&state, 0, 0U); /* A failed TACT changes nothing. */
    dmr_mono_slot_begin(&state);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS2);
    /* A later call on the other timeslot: Checking at the first burst naming it, proven at the second. */
    dmr_mono_slot_note(&state, 1, 0U);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_PENDING);
    dmr_mono_slot_note(&state, 1, 0U);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS1);

    state.synctype = DSD_SYNC_DMR_MS_VOICE;
    state.dmr_dm_sync_slot = 2U;
    dmr_mono_slot_begin(&state);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS2);
    dmr_mono_slot_note(&state, 1, 0U); /* No CACH on a direct-mode carrier: nothing to note. */
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS2);
    state.dmr_dm_sync_slot = 1U;
    dmr_mono_slot_begin(&state);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_TS1);
    state.dmr_dm_sync_slot = 0U;
    dmr_mono_slot_begin(&state);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_PENDING);
    dmr_mono_slot_forget(&state);
    assert(state.dmr_mono_rx_slot == DSD_DMR_MONO_RX_NONE);
}

/* Issue #637: rejected voice keeps the carrier alive but does not hold the voice channel, so a channel only rejected
   traffic holds is let go at its hangtime, a CSBK-followed one included. */
static void
test_bs_rejected_voice_does_not_hold_the_channel(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    state.currentslot = 0;
    state.dmr_color_code = 16;
    load_voice_burst_stream();
    g_receive_rejects = 1;

    dmrBS(&opts, &state);

    g_receive_rejects = 0;
    assert(state.last_vc_sync_time == 0 && state.last_vc_sync_time_m == 0.0);
    assert(state.last_cc_sync_time > 0 && state.last_cc_sync_time_m > 0.0);
}

static void
test_bs_voice_sync_refreshes_when_trunk_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    state.currentslot = 1; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_voice_burst_stream();

    dmrBS(&opts, &state);

    assert(state.last_vc_sync_time > 0);
    assert(state.last_cc_sync_time > 0);
    assert(state.last_vc_sync_time_m > 0.0);
    assert(state.last_cc_sync_time_m > 0.0);
}

static void
test_bs_slot2_voice_routes_right_channel_and_post_skip_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "captures");
    /* No capture is open: the burst opens nothing, its first frame creates the slot 2 capture, and the end of the stream
       closes it (issue #661). */
    state.currentslot = 0; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_single_burst_stream(1, DMR_BS_VOICE_SYNC);
    g_any_voice_open = 1;

    dmrBS(&opts, &state);

    assert(state.dmrburstR == 16);
    assert(g_open_right_calls == 0U);
    assert(g_open_left_calls == 0U);
    assert(g_close_right_calls == 1U);
    assert(g_close_left_calls == 0U);
    assert(g_process_mbe_calls == 3U);
    // Float output (-y) plays through the same DMR stereo mix as 16-bit output (#652).
    assert(g_play_ss3_calls == 1U);
    assert(g_ui_redraw_calls == 1U);
    assert(g_history_calls[0] == 1U);
    assert(g_history_calls[1] == 1U);
    assert(g_current_calls[0] == 1U);
    assert(g_current_calls[1] == 1U);
    assert(g_debug_dump_calls == 1U);
    assert(g_debug_dump_last_slot == 1U);
    assert(g_debug_dump_last_type == 0x10U);
    assert(g_cach_calls == 1U);
    assert(g_late_entry_calls == 1U);
    assert(g_late_entry_last_vc == 1U);
    assert(g_sm_voice_sync_calls == 1U);
    assert(g_sm_voice_sync_last_slot == 1);
    assert(g_sm_tick_calls == 1U);
    assert(g_confidence_voice_sync_calls == 1U);
    assert(g_confidence_voice_burst_calls == 1U);
}

static void
test_bs_slot2_voice_integer_output_uses_ss3_playback(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 0;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 0; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_single_burst_stream(1, DMR_BS_VOICE_SYNC);
    g_any_voice_open = 1;

    dmrBS(&opts, &state);

    assert(state.dmrburstR == 16);
    assert(g_play_ss3_calls == 1U);
    assert(g_process_mbe_calls == 3U);
    // Burst A's three frames decode at superframe positions 0..2 (issue #729).
    assert(g_vcrypt_last_pos == 2);
    assert(g_sm_voice_sync_last_slot == 1);
}

static void
test_bs_data_sync_closes_slot_file_and_resets_error_state(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.mbe_out_fR = (FILE*)(uintptr_t)1;
    state.currentslot = 0; /* the bootstrap read the other slot's burst A */
    load_single_burst_stream(1, DMR_BS_DATA_SYNC);

    dmrBS(&opts, &state);

    assert(g_data_sync_calls == 1U);
    assert(g_data_sync_reliability[0] == 1U);
    assert(g_data_sync_reliability[60] == 61U);
    assert(g_data_sync_reliability[95] == 96U);
    assert(g_data_sync_reliability[143] == 144U);
    assert(g_close_right_calls == 1U);
    assert(opts.mbe_out_fR == NULL);
    assert(g_process_mbe_calls == 0U);
    assert(g_sm_voice_sync_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    // The error END takes no MI step of its own (issue #729): the next voice sync catches up what it costs.
    assert(g_vcrypt_start_calls == 0U);
    assert(g_vcrypt_end_calls == 0U);
    assert(g_anchor_calls == 1U); // the data sync placed the symbol grid
    assert(g_confidence_reset_calls == 1U);
}

static void
test_bs_confidence_reject_resets_slot_without_voice_decode(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
    g_confidence_result = DMR_CONFIDENCE_REJECT;
    g_voice_slot_open = 0; /* not yet qualified: a mismatching colour code is no evidence to go on */

    dmrBS(&opts, &state);

    assert(g_confidence_voice_sync_calls == 1U);
    assert(g_confidence_voice_burst_calls == 1U);
    assert(g_confidence_reset_slot_calls == 1U);
    assert(g_confidence_reset_last_slot == 0U);
    assert(g_debug_dump_calls == 0U);
    assert(g_process_mbe_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    // The rejected burst A still started the slot's superframe; the error END advanced nothing (issue #729).
    assert(g_vcrypt_start_calls == 1U);
    assert(g_vcrypt_end_calls == 0U);
    assert(g_confidence_reset_calls == 1U);
}

/* On a slot whose gate is open, one EMB naming another colour code is taken for the QR miscorrection it most likely is
   (issue #729): it counts as an EMB miss and the burst's voice still decodes, as for an undecodable EMB. */
static void
test_bs_single_colour_code_mismatch_on_open_gate_is_an_emb_miss(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
    g_confidence_result = DMR_CONFIDENCE_REJECT;
    g_voice_slot_open = 1;

    dmrBS(&opts, &state);

    assert(g_confidence_voice_burst_calls == 1U);
    assert(g_confidence_reset_slot_calls == 0U);
    assert(g_process_mbe_calls == 3U);
}

static void
test_bs_voice_gate_closed_skips_decode_but_keeps_loop_hooks(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.frontend_kind = DSD_FRONTEND_TERMINAL;
    state.currentslot = 1; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
    g_voice_slot_open = 0;

    dmrBS(&opts, &state);

    assert(g_confidence_voice_sync_calls == 1U);
    assert(g_confidence_voice_burst_calls == 1U);
    assert(g_debug_dump_calls == 0U);
    assert(g_process_mbe_calls == 0U);
    assert(g_late_entry_calls == 0U);
    assert(g_sm_voice_sync_calls == 0U);
    assert(g_ui_redraw_calls == 1U);
    assert(g_history_calls[0] == 1U);
    assert(g_history_calls[1] == 1U);
    assert(g_current_calls[0] == 1U);
    assert(g_current_calls[1] == 1U);
    assert(g_sm_tick_calls == 1U);
}

static void
test_bs_bootstrap_prefetched_voice_runs_first_frame_path(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    DSD_SNPRINTF(opts.mbe_out_dir, sizeof(opts.mbe_out_dir), "%s", "captures");
    /* No capture is open: the bootstrap opens nothing, and its first frame creates the slot 2 capture (issue #661). */
    state.currentslot = 1;
    state.dmr_color_code = 16;
    state.dmr_payload_p = g_bootstrap_payload + 90U;
    load_bootstrap_voice_stream(1);
    g_any_voice_open = 1;

    dmrBSBootstrap(&opts, &state);

    assert(g_debug_dump_calls >= 1U);
    assert(g_alg_reset_calls == 1U);
    assert(g_process_mbe_calls >= 3U);
    assert(g_sm_voice_sync_calls == 1U);
    assert(g_sm_voice_sync_calls_at_first_mbe == 1U);
    assert(g_sm_voice_sync_last_slot == 1);
    assert(g_cach_calls >= 1U);
    assert(g_late_entry_calls >= 1U);
    assert(g_open_right_calls == 0U);
    assert(g_close_right_calls >= 1U);
    assert(g_play_ss3_calls >= 1U);
    assert(state.last_vc_sync_time > 0);
    assert(state.last_vc_sync_time_m > 0.0);
}

/* Issue #575: the carrier boundary moved the carrier count while a burst was read (a replay read adopting a recorded
   retune). The burst straddles two carriers, and what it carries from the carrier left (the first AMBE frames, the EMB
   colour code, the CACH fragment that may complete a short LC) would decode after the boundary ended that carrier's
   calls: nothing in it decodes, what earlier bursts gathered goes, and the loop ends as on a sync loss, without
   reporting a decode error. */
static void
test_bs_burst_split_by_a_carrier_boundary_is_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 1; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_voice_burst_stream();
    g_any_voice_open = 1;
    g_boundary_at = 100U; /* AMBE frame 2's second half: the EMB and frame 1 were read before it */

    dmrBS(&opts, &state);

    assert(state.carrier_seq == 1U);
    assert(g_dibit_index == 144U);
    assert(g_process_mbe_calls == 0U);
    assert(g_confidence_voice_burst_calls == 0U);
    assert(g_cach_calls == 0U);
    assert(g_late_entry_calls == 0U);
    assert(g_sm_voice_sync_calls == 0U);
    assert(g_debug_dump_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    assert(g_vcrypt_end_calls == 0U);
    assert(g_confidence_reset_calls == 1U);
    assert(state.dmr_stereo == 0);
}

/* The same for the burst the frame sync found: its second half is read after the boundary moved. */
/* The frame sync accepted burst A with the oldest of its 90 dibits read back not the stream's (a cold start or an
   acquisition reset: dsd_state_sync_readback_invalid()). Its CACH is among them, and the CACH names the burst's slot,
   so nothing of the burst decodes: no voice sync for a slot, no frame, no CACH fragment, no late-entry fragment. Its
   second half is read past, and the loop takes the next burst. */
static void
test_bs_bootstrap_decodes_nothing_it_did_not_read_back(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 1;
    state.dmr_color_code = 16;
    state.dmr_payload_p = g_bootstrap_payload + 90U;
    load_bootstrap_voice_stream(1);
    g_any_voice_open = 1;
    dsd_state_note_sync_readback(&state, 60);

    dmrBSBootstrap(&opts, &state);

    assert(g_dibit_index >= 144U);
    assert(g_sm_voice_sync_calls == 0U);
    assert(g_process_mbe_calls == 0U);
    assert(g_cach_calls == 0U);
    assert(g_late_entry_calls == 0U);
    assert(g_alg_reset_calls == 0U);
    assert(state.last_vc_sync_time == 0);
}

static void
test_bs_bootstrap_split_by_a_carrier_boundary_is_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 1;
    state.dmr_color_code = 16;
    state.dmr_payload_p = g_bootstrap_payload + 90U;
    load_bootstrap_voice_stream(1);
    g_any_voice_open = 1;
    g_boundary_at = 120U;

    dmrBSBootstrap(&opts, &state);

    assert(state.carrier_seq == 1U);
    assert(g_dibit_index == 144U);
    assert(g_process_mbe_calls == 0U);
    assert(g_cach_calls == 0U);
    assert(g_late_entry_calls == 0U);
    assert(g_play_ss3_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    assert(g_vcrypt_end_calls == 0U);
    assert(g_confidence_reset_calls == 1U);
}

/* A Signal search that leaves the burst (its visit ended, or a command came), or a live read that gives up on a stalled
   input, cuts it: from the cut every symbol is an erasure getSymbol() stood in. The burst is dropped as one the carrier
   boundary splits is: nothing in it decodes or plays, and the loop ends without reporting a decode error. */
static void
test_bs_burst_cut_by_an_input_interruption_is_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 1; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_voice_burst_stream();
    g_any_voice_open = 1;
    g_interrupt_at = 100U;

    dmrBS(&opts, &state);

    assert(state.input_interrupted == 1);
    assert(state.carrier_seq == 0U);
    assert(g_dibit_index == 144U);
    assert(g_process_mbe_calls == 0U);
    assert(g_confidence_voice_burst_calls == 0U);
    assert(g_cach_calls == 0U);
    assert(g_late_entry_calls == 0U);
    assert(g_sm_voice_sync_calls == 0U);
    assert(g_debug_dump_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    assert(g_vcrypt_end_calls == 0U);
    assert(state.dmr_stereo == 0);
}

/* The same for the burst the frame sync found: its live second half is cut. */
static void
test_bs_bootstrap_cut_by_an_input_interruption_is_dropped(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 1;
    state.dmr_color_code = 16;
    state.dmr_payload_p = g_bootstrap_payload + 90U;
    load_bootstrap_voice_stream(1);
    g_any_voice_open = 1;
    g_interrupt_at = 120U;

    dmrBSBootstrap(&opts, &state);

    assert(state.input_interrupted == 1);
    assert(state.carrier_seq == 0U);
    assert(g_process_mbe_calls == 0U);
    assert(g_cach_calls == 0U);
    assert(g_late_entry_calls == 0U);
    assert(g_play_ss3_calls == 0U);
    assert(g_reset_blocks_calls == 1U);
    assert(g_vcrypt_end_calls == 0U);
}

/* Issue #574: the loop's end drops the audio staged for the stereo mixes, and with it each slot's fresh-media mark. The
   slot 1 burst decoded here never reached a mix (the mixes follow slot 2's bursts), and slot 2 still held a mark from
   before the loop: the first mix of the next loop, a skipped burst's with nothing decoded yet, would otherwise stamp
   audible audio for buffers that now hold zeros. A mix stamps only for the marks it takes
   (dsd_audio_dmr_mix_media_take()); the mixes themselves are stubs here. */
static void
test_bs_loop_end_discards_the_mix_media(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.floating_point = 1;
    opts.pulse_digi_rate_out = 8000;
    state.currentslot = 1; /* the bootstrap read the other slot's burst A */
    state.dmr_color_code = 16;
    load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
    g_any_voice_open = 1;
    g_stage_mix_media = 1;
    dsd_audio_dmr_mix_media_discard(0);
    dsd_audio_dmr_mix_media_staged(1, DSD_DMR_MIX_MEDIA_SHORT);

    dmrBS(&opts, &state);

    assert(g_process_mbe_calls == 3U);
    assert(g_play_ss3_calls == 0U);
    assert(dsd_audio_dmr_mix_media_take(0) == 0U);
    assert(dsd_audio_dmr_mix_media_take(1) == 0U);
    g_stage_mix_media = 0;
}

static int
yield_after_one_second(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    /* 4800 dibits per second; the engine's deadline hook is tested separately. */
    return g_dibit_index >= 4800U;
}

/* The queued commands that need the frame loop (issue #637): none, or from @p g_frame_loop_command_at dibits on. */
static size_t g_frame_loop_command_at = SIZE_MAX;

int
dsd_runtime_controls_need_frame_loop(void) {
    return g_dibit_index >= g_frame_loop_command_at;
}

static void
test_continuous_voice_yields_at_burst_boundary(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dmr_color_code = 16;
    load_voice_burst_stream();
    DSD_MEMCPY(g_dibits + 144U, g_dibits, 144U);
    set_cach_tact_bit(144U, 1, 1);
    /* Adjacent slots carry different payloads so the stuck-dibit guard stays open. */
    g_dibits[144U + 12U + 16U] = 2U;
    g_continuous_voice = 1;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){.scan_visit_should_yield = yield_after_one_second});
    dmrBS(&opts, &state);
    assert(g_dibit_index >= 4800U && g_dibit_index < 4800U + 144U);
    assert(g_dibit_index % 144U == 0U);
    assert(g_process_mbe_calls > 0U);
    assert(g_sm_tick_calls > 1U);
    assert(g_confidence_reset_calls == 1U);
    assert(g_vcrypt_end_calls == 0U);
    assert(state.dmr_stereo == 0);
    dsd_frame_sync_hooks_set(NULL);
    g_continuous_voice = 0;
}

/* A command the frame loop must apply (a scan advance, a retune) waits only for the next burst boundary, not for the
   carrier to end (issue #637); the loop leaves as it does for a scan departure. */
static void
test_continuous_voice_yields_to_a_frame_loop_command(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    state.dmr_color_code = 16;
    load_voice_burst_stream();
    DSD_MEMCPY(g_dibits + 144U, g_dibits, 144U);
    set_cach_tact_bit(144U, 1, 1);
    g_dibits[144U + 12U + 16U] = 2U;
    g_continuous_voice = 1;
    g_frame_loop_command_at = 4800U;
    dmrBS(&opts, &state);
    assert(g_dibit_index >= 4800U && g_dibit_index < 4800U + 144U);
    assert(g_dibit_index % 144U == 0U);
    assert(g_process_mbe_calls > 0U);
    assert(g_vcrypt_end_calls == 0U);
    g_frame_loop_command_at = SIZE_MAX;
    g_continuous_voice = 0;
}

/* Three bursts with distinct leading payloads, so the redundant-carrier check lets the loop read each: burst A of
   @p slot0 with a voice sync, then two EMB bursts whose TACTs name @p slot1 and @p slot2. */
static void
load_three_burst_stream(uint8_t slot0, uint8_t slot1, uint8_t slot2) {
    load_single_burst_stream(slot0, DMR_BS_VOICE_SYNC);
    g_dibits[144U + 12U + 16U] = 2U;
    g_dibits[288U + 12U + 16U] = 1U;
    g_dibits[288U + 12U + 17U] = 1U;
    set_cach_tact_bit(144U, 1, slot1);
    set_cach_tact_bit(288U, 1, slot2);
}

/* One TACT naming the slot alternation does not expect is the Hamming(7,4) miscorrection it most likely is: the burst
   stays with alternation from the bootstrap's burst (issue #729). */
static void
test_bs_one_tact_disagreement_keeps_alternation(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1; /* the bootstrap read slot 2's burst A */
    state.dmr_color_code = 16;
    load_three_burst_stream(0, 0, 0); /* the second burst's TACT is wrong */
    g_any_voice_open = 1;

    dmrBS(&opts, &state);

    assert(g_process_mbe_calls >= 9U);
    assert(g_mbe_slots[0] == 0U);
    assert(g_mbe_slots[3] == 1U);
    assert(g_mbe_slots[6] == 0U);
}

/* Two TACTs in a row against alternation say the alternation started wrong (the bootstrap's own TACT was miscorrected):
   it takes the TACT's slot from the second of them, so a wrong start costs one burst (issue #729). */
static void
test_bs_two_tact_disagreements_reanchor_alternation(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0; /* wrong: the bootstrap's burst was slot 2's */
    state.dmr_color_code = 16;
    load_three_burst_stream(0, 1, 0);
    g_any_voice_open = 1;

    dmrBS(&opts, &state);

    assert(g_process_mbe_calls >= 9U);
    assert(g_mbe_slots[0] == 1U); /* one disagreement: alternation holds */
    assert(g_mbe_slots[3] == 1U); /* the second: the TACT's slot */
    assert(g_mbe_slots[6] == 0U); /* and alternation from there */
}

/* Once two TACTs confirmed alternation, two miscorrected ones in a row no longer move it: only the start may be
   corrected by the TACT (issue #729). The bursts alternate slot 2, 1, 2, 1, 2; the third's and fourth's TACTs read
   wrong. */
static void
test_bs_confirmed_alternation_ignores_two_wrong_tacts(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 0; /* the bootstrap read slot 1's burst A */
    state.dmr_color_code = 16;
    load_single_burst_stream(1, DMR_BS_VOICE_SYNC);
    static const uint8_t tact[5] = {1U, 0U, 0U, 1U, 1U}; /* bursts 3 and 4 miscorrected */
    for (size_t b = 1; b < 5U; b++) {
        g_dibits[(b * 144U) + 12U + 16U] = (uint8_t)((b % 3U) + 1U);
        g_dibits[(b * 144U) + 12U + 17U] = (uint8_t)(b & 3U);
        set_cach_tact_bit(b * 144U, 1, tact[b]);
    }
    g_any_voice_open = 1;

    dmrBS(&opts, &state);

    assert(g_process_mbe_calls >= 15U);
    static const uint8_t want[5] = {1U, 0U, 1U, 0U, 1U};
    for (size_t b = 0; b < 5U; b++) {
        assert(g_mbe_slots[b * 3U] == want[b]);
    }
}

/* EMBs that decode but name another colour code are misses in a row on their slot: only an accepted EMB ends the
   streak, so the slot's second rejected EMB ends the loop as two undecodable ones do (issue #729). */
static void
test_bs_consecutive_colour_code_rejections_end_the_loop(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1;
    state.dmr_color_code = 16;
    load_three_burst_stream(0, 1, 0);
    g_confidence_result = DMR_CONFIDENCE_REJECT;
    g_voice_slot_open = 1;

    dmrBS(&opts, &state);

    /* Slot 1's burst A and slot 2's burst are each a first miss and decode; slot 1's next is its second. */
    assert(g_process_mbe_calls == 6U);
    assert(g_confidence_reset_slot_calls == 1U);
    assert(g_confidence_reset_last_slot == 0U);
}

/* Voice sync on four adjacent bursts with no voice gate open is data at the other polarity; the run counts only
   adjacent bursts, so burst A syncs a superframe apart (bursts B lost) never make one (issue #729). */
static void
test_bs_polarity_run_needs_adjacent_bursts(void) {
    static dsd_opts opts;
    static dsd_state state;
    for (int adjacent = 0; adjacent < 2; adjacent++) {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        state.currentslot = 1;
        state.dmr_color_code = 16;
        load_single_burst_stream(0, DMR_BS_VOICE_SYNC);
        g_voice_slot_open = 0;
        state.symbol_clock = 10000U;
        /* The burst's sync field ends 90 symbols in; three runs' worth of voice syncs came before it. */
        state.dmr_voice_sync_run = 3U;
        state.dmr_voice_sync_clock = 10000U + 90U - (adjacent ? 144U : 864U);

        dmrBS(&opts, &state);

        assert(opts.inverted_dmr == (adjacent ? 1 : 0));
        if (!adjacent) {
            assert(state.dmr_voice_sync_run >= 1U && state.dmr_voice_sync_run < 4U);
        }
    }
}

/* A Capacity Plus rest channel's SLCO in the first burst's CACH retunes the receiver, and the loop goes on to decode
   the bursts the carrier it reached sends, all within one dispatch (processFrame() marks it). The drop of the access
   code is applied at the retune: the publication after the retuning burst shows no colour code of the carrier left,
   and the next burst's checked colour code is the reached carrier's first, not another system on the carrier, so the
   system (the rest channel's LSN) and the identity's epoch stay, and the code stays noted when the loop ends. */
static void
test_bs_retune_in_a_burst_keeps_the_next_bursts_colour_code(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    state.currentslot = 1; /* the bootstrap read slot 2's burst A */
    state.dmr_color_code = 16;
    load_three_burst_stream(0, 1, 0);
    g_any_voice_open = 1;
    dsd_rxid_note_access(&state, DSD_RXID_FAMILY_DMR, (uint8_t)DSD_ACCESS_CODE_COLOR_CODE, 1U);
    dsd_rxid_note_dmr_rest_lsn(&state, 3U);
    const uint32_t epoch = state.rx_id.epoch;
    g_burst_colour_codes[0] = 1U; /* the carrier left */
    g_burst_colour_codes[1] = 2U; /* the carrier reached */
    g_burst_colour_codes[2] = 2U;
    g_cach_retune_at = 1U;
    state.frame_dispatch_active = 1;

    dmrBS(&opts, &state);

    state.frame_dispatch_active = 0;
    assert(g_confidence_voice_burst_calls >= 3U && g_ui_redraw_calls >= 3U);
    const dsd_received_identity* after_retune = &g_published_rx_id[0];
    assert((after_retune->valid & DSD_RXID_ACCESS) == 0U);
    assert((after_retune->valid & DSD_RXID_DMR_REST_LSN) != 0U && after_retune->dmr_rest_lsn == 3U);
    for (size_t i = 1; i < 3U; i++) {
        assert((g_published_rx_id[i].valid & DSD_RXID_ACCESS) != 0U && g_published_rx_id[i].access == 2U);
    }
    assert(state.rx_id_trunk_tune_pending == 0U);
    assert((state.rx_id.valid & DSD_RXID_ACCESS) != 0U && state.rx_id.access == 2U);
    assert((state.rx_id.valid & DSD_RXID_DMR_REST_LSN) != 0U && state.rx_id.dmr_rest_lsn == 3U);
    assert(state.rx_id.epoch == epoch);
}

int
main(void) {
    dsd_decode_clock_use_test(1234567890ULL);
    test_continuous_voice_yields_at_burst_boundary();
    test_bs_voice_sync_refreshes_when_trunk_tuned();
    test_bs_rejected_voice_does_not_hold_the_channel();
    test_mono_lane_slot_proof();
    test_bs_slot2_voice_routes_right_channel_and_post_skip_hooks();
    test_bs_slot2_voice_integer_output_uses_ss3_playback();
    test_bs_data_sync_closes_slot_file_and_resets_error_state();
    test_bs_confidence_reject_resets_slot_without_voice_decode();
    test_bs_single_colour_code_mismatch_on_open_gate_is_an_emb_miss();
    test_bs_voice_gate_closed_skips_decode_but_keeps_loop_hooks();
    test_bs_bootstrap_prefetched_voice_runs_first_frame_path();
    test_bs_burst_split_by_a_carrier_boundary_is_dropped();
    test_bs_bootstrap_split_by_a_carrier_boundary_is_dropped();
    test_bs_bootstrap_decodes_nothing_it_did_not_read_back();
    test_bs_burst_cut_by_an_input_interruption_is_dropped();
    test_bs_bootstrap_cut_by_an_input_interruption_is_dropped();
    test_bs_loop_end_discards_the_mix_media();
    test_bs_one_tact_disagreement_keeps_alternation();
    test_bs_two_tact_disagreements_reanchor_alternation();
    test_bs_confirmed_alternation_ignores_two_wrong_tacts();
    test_bs_consecutive_colour_code_rejections_end_the_loop();
    test_bs_polarity_run_needs_adjacent_bursts();
    test_bs_retune_in_a_burst_keeps_the_next_bursts_colour_code();
    reset_spies();
    test_continuous_voice_yields_to_a_frame_loop_command();
    dsd_decode_clock_use_system();
    printf("DMR BS sync times: OK\n");
    return 0;
}

// NOLINTEND(performance-no-int-to-ptr)
