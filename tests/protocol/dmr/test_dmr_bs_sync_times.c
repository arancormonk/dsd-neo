// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "dmr_confidence.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

volatile uint8_t exitflag = 0;

static uint8_t g_dibits[256];
static size_t g_dibit_index = 0;

static void
load_voice_burst_stream(void) {
    DSD_MEMSET(g_dibits, 0, sizeof(g_dibits));
    g_dibit_index = 0;

    g_dibits[12U + 16U] = 1U;
    g_dibits[144U + 12U + 16U] = 1U;

    const size_t sync_offset = 12U + 36U + 18U;
    for (size_t i = 0; DMR_BS_VOICE_SYNC[i] != '\0'; i++) {
        g_dibits[sync_offset + i] = (DMR_BS_VOICE_SYNC[i] == '3') ? 2U : 0U;
    }
}

int
getDibit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    if (g_dibit_index >= sizeof(g_dibits)) {
        return 0;
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

uint64_t
dsd_time_monotonic_ns(void) {
    return 1234567890ULL;
}

void
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    exitflag = 1;
}

void
getTimeC_buf(char out[9]) {
    DSD_SNPRINTF(out, 9, "%s", "00:00:00");
}

FILE*
dsd_fopen_private(const char* path, const char* mode) {
    (void)path;
    (void)mode;
    return NULL;
}

void
openMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
openMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
closeMbeOutFile(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
closeMbeOutFileR(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
}

void
playSynthesizedVoiceFS3(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
playSynthesizedVoiceSS3(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
ui_publish_both_and_redraw(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
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
dmr_data_sync(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dmr_data_burst_handler(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst) {
    (void)opts;
    (void)state;
    (void)info;
    (void)databurst;
}

uint8_t
dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]) {
    (void)opts;
    (void)state;
    (void)cach_bits;
    return 0;
}

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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
}

void
dmr_refresh_algids_on_error(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
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
}

void
dmr_sbrc(const dsd_opts* opts, dsd_state* state, uint8_t power) {
    (void)opts;
    (void)state;
    (void)power;
}

void
dmr_sm_emit_voice_sync(dsd_opts* opts, dsd_state* state, int slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
dmr_sm_tick(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dmr_confidence_reset(dsd_state* state) {
    (void)state;
}

void
dmr_confidence_reset_slot(dsd_state* state, unsigned int slot) {
    (void)state;
    (void)slot;
}

void
dmr_confidence_note_voice_sync(dsd_state* state, unsigned int slot) {
    (void)state;
    (void)slot;
}

dmr_confidence_result
dmr_confidence_note_voice_burst(dsd_state* state, unsigned int slot, unsigned int color_code) {
    (void)state;
    (void)slot;
    (void)color_code;
    return DMR_CONFIDENCE_LOCKED;
}

int
dmr_confidence_voice_slot_open(const dsd_state* state, unsigned int slot) {
    (void)state;
    (void)slot;
    return 1;
}

int
dmr_confidence_any_voice_open(const dsd_state* state) {
    (void)state;
    return 0;
}

static void
test_bs_voice_sync_refreshes_when_trunk_alias_tuned(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.trunk_is_tuned = 1;
    state.currentslot = 0;
    state.dmr_color_code = 16;
    load_voice_burst_stream();

    dmrBS(&opts, &state);

    assert(state.last_vc_sync_time > 0);
    assert(state.last_cc_sync_time > 0);
    assert(state.last_vc_sync_time_m > 0.0);
    assert(state.last_cc_sync_time_m > 0.0);
}

int
main(void) {
    test_bs_voice_sync_refreshes_when_trunk_alias_tuned();
    printf("DMR BS sync times: OK\n");
    return 0;
}
