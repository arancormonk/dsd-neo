// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Focused coverage for the cached DMR data-sync path.
 */

#include "dsd-neo/core/safe_api.h"

#include <assert.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/sync_patterns.h>
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
static int g_reset_calls;
static int g_skip_calls;
static int g_handler_slot;
static uint8_t g_handler_burst;
static uint8_t g_handler_info[196];
static uint8_t g_handler_reliab[98];

static void
reset_fixture(void) {
    g_hamming_ok = 1;
    g_golay_ok = 1;
    g_decoded_slot = 1;
    g_decoded_color = 5;
    g_decoded_burst = 7;
    g_handler_calls = 0;
    g_sm_calls = 0;
    g_reset_calls = 0;
    g_skip_calls = 0;
    g_handler_slot = -1;
    g_handler_burst = 0;
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
get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal) {
    (void)opts;
    (void)state;
    if (out_analog_signal != NULL) {
        *out_analog_signal = 0;
    }
    return 0;
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
dmr_data_burst_handler_ex(dsd_opts* opts, dsd_state* state, uint8_t info[196], uint8_t databurst,
                          const uint8_t* reliab98) {
    (void)opts;
    g_handler_calls++;
    g_handler_slot = state->currentslot;
    g_handler_burst = databurst;
    DSD_MEMCPY(g_handler_info, info, sizeof(g_handler_info));
    DSD_MEMCPY(g_handler_reliab, reliab98, sizeof(g_handler_reliab));
}

uint8_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]) {
    (void)opts;
    (void)state;
    (void)cach_bits;
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
    assert(slot == (int)g_decoded_slot);
}

size_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_debug_format_burst(char* out, size_t out_size, const dsd_state* state, uint8_t slot_index, uint8_t burst_type) {
    (void)state;
    (void)slot_index;
    (void)burst_type;
    if (out_size == 0U) {
        return 0U;
    }
    out[0] = '\0';
    return 0U;
}

dmr_confidence_result
// NOLINTNEXTLINE(misc-use-internal-linkage)
dmr_confidence_note_data_burst(dsd_state* state, unsigned int color_code, unsigned int burst) {
    (void)state;
    (void)color_code;
    (void)burst;
    return DMR_CONFIDENCE_LOCKED;
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
prepare_state(dsd_state* state, int payload[90], uint8_t reliab[90]) {
    DSD_MEMSET(state, 0, sizeof(*state));
    for (size_t i = 0; i < 90U; i++) {
        payload[i] = (int)(i & 3U);
        reliab[i] = (uint8_t)(40U + i);
    }
    fill_sync_payload(payload, DMR_BS_DATA_SYNC);

    state->dmr_payload_p = payload + 90;
    state->dmr_reliab_buf = reliab;
    state->dmr_reliab_p = reliab + 90;
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
test_data_sync_dispatches_burst_and_reliability(void) {
    static dsd_opts opts;
    static dsd_state state;
    int payload[90];
    uint8_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();

    opts.dmr_mono = 1;
    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 1);
    assert(g_handler_slot == 1);
    assert(g_handler_burst == 7U);
    assert(g_sm_calls == 1);
    assert(g_reset_calls == 0);
    assert(g_skip_calls == 0);
    assert(state.currentslot == 1);
    assert(state.color_code == 5);
    assert(state.dmr_color_code == 5);
    assert(state.dmrburstR == 7U);
    assert(strcmp(state.slot1light, " slot1 ") == 0);
    assert(strcmp(state.slot2light, "[slot2]") == 0);
    assert(g_handler_reliab[0] == reliab[12]);
    assert(g_handler_reliab[48] == reliab[60]);
    assert(g_handler_reliab[49] == state.dmr_stereo_reliab[95]);
    assert(g_handler_reliab[97] == state.dmr_stereo_reliab[143]);
}

static void
test_cach_failure_resets_without_dispatch(void) {
    static dsd_opts opts;
    static dsd_state state;
    int payload[90];
    uint8_t reliab[90];
    DSD_MEMSET(&opts, 0, sizeof(opts));
    prepare_state(&state, payload, reliab);
    reset_fixture();
    g_hamming_ok = 0;

    opts.dmr_mono = 1;
    dmr_data_sync(&opts, &state);

    assert(g_handler_calls == 0);
    assert(g_sm_calls == 0);
    assert(g_reset_calls == 1);
    assert(g_skip_calls == 0);
}

int
main(void) {
    test_data_sync_dispatches_burst_and_reliability();
    test_cach_failure_resets_without_dispatch();
    DSD_FPRINTF(stdout, "DMR_DATA_SYNC: OK\n");
    return 0;
}
