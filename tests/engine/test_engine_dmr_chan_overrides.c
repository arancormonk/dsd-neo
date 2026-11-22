// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * --dmr-chan-overrides at start (discussion #900 F3): the decoder applies the user's Tier III channels over the -C map
 * after the map loaded and before the first frame, so a grant in the first dispatched frame resolves to the user's
 * frequency, a hidden channel's grant is not followed, and RESTORE has the file's row to return to. A file that cannot
 * be read stops the start. Only a session that decodes DMR and runs no trunk scan takes it (review gate b-r2 C1): any
 * other starts without it, with one notice. Real engine run; the frame sync is wrapped and dispatches the grants
 * itself.
 */

#include <assert.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "test_support.h"

static const long CC_HZ = 451000000L;
static int g_sync_calls;
static long g_tuned_hz[1];
static int g_tunes;
/* Whether the run under test takes the file; and the "not applied" notices logged. */
static int g_expect_applied = 1;
static int g_skip_notices;
static char g_skip_notice[512];
static int g_overrides_refusals;
static int g_scan_start_errors;

static void
skip_log_tap(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    if (text != NULL && strncmp(text, "NOTICE: DMR Tier III channels in ", 33) == 0 && strstr(text, " not applied: ")) {
        g_skip_notices++;
        DSD_SNPRINTF(g_skip_notice, sizeof g_skip_notice, "%s", text);
    }
    if (text != NULL && strstr(text, "--dmr-chan-overrides cannot be combined") != NULL) {
        g_overrides_refusals++;
    }
    if (text != NULL && strstr(text, "--trunk-scan requires targets_csv") != NULL) {
        g_scan_start_errors++;
    }
}

// NOLINTBEGIN(misc-use-internal-linkage)
void printFrameInfo(dsd_opts* opts, dsd_state* state);
void LFSRN(const char* input, char* output, dsd_state* state);
void dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t* cs_pdu_bits, uint8_t* cs_pdu, uint32_t CRCCorrect,
               uint32_t IrrecoverableErrors);

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
LFSRN(const char* input, char* output, dsd_state* state) {
    (void)input;
    (void)output;
    (void)state;
}

void
processFrame(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    assert(0 && "the overrides test supplies no synced frame");
}

// NOLINTEND(misc-use-internal-linkage)

static dsd_trunk_tune_result
tune_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    if (g_tunes < 1) {
        g_tuned_hz[g_tunes] = freq;
    }
    g_tunes++;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
put_bits(uint8_t* bits, size_t start, uint32_t value, size_t nbits) {
    for (size_t i = 0; i < nbits; i++) {
        bits[start + i] = (uint8_t)((value >> (nbits - 1U - i)) & 1U);
    }
}

/* A checked TV_GRANT (opcode 49) for LPCN @p lpcn, slot 0, talkgroup 1001 from 2002. */
static void
send_grant(dsd_opts* opts, dsd_state* state, uint16_t lpcn) {
    uint8_t bits[256];
    uint8_t bytes[48];
    DSD_MEMSET(bits, 0, sizeof bits);
    DSD_MEMSET(bytes, 0, sizeof bytes);
    bits[0] = 1U;
    bytes[0] = (uint8_t)(0x80U | 49U);
    put_bits(bits, 2, 49U, 6);
    put_bits(bits, 16, lpcn & 0x0FFFU, 12);
    put_bits(bits, 32, 1001U, 24);
    put_bits(bits, 56, 2002U, 24);
    dmr_cspdu(opts, state, bits, bytes, 1U, 0U);
}

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_getFrameSync(dsd_opts* opts, dsd_state* state);

int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    ++g_sync_calls;
    if (!g_expect_applied) {
        /* Not this session's: no entry was made, and a map's rows are the map file's. */
        assert(dmr_t3_overlay_count(state) == 0U && dmr_t3_chan_origin(state, 12U) != DMR_CHAN_ORIGIN_USER);
        assert(opts->chan_in_file[0] == '\0'
               || (state->trunk_chan_map[12] == 451012500L && state->trunk_chan_map[13] == 451025000L));
        dsd_exitflag_store(1);
        return DSD_SYNC_NONE;
    }
    /* The first frame: the user's rows already lie over the file's. */
    assert(state->trunk_chan_map[12] == 451137500L && dmr_t3_chan_origin(state, 12U) == DMR_CHAN_ORIGIN_USER);
    assert(state->trunk_chan_map[13] == 0L && dmr_t3_chan_origin(state, 13U) == DMR_CHAN_ORIGIN_USER);
    assert(state->trunk_chan_map[14] == 451037500L && dmr_t3_chan_origin(state, 14U) == DMR_CHAN_ORIGIN_FILE);
    long base_hz = 0;
    assert(dmr_t3_overlay_get(state, 12U, NULL, &base_hz) == DMR_T3_OVERLAY_SET && base_hz == 451012500L);

    /* Grants in that frame: hidden 13 is not followed (nor recorded as missing), 12 resolves to the user's
     * frequency. */
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){.tune_to_freq_request = tune_request});
    state->synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    state->trunk_cc_freq = CC_HZ;
    opts->trunk_is_tuned = 0;
    send_grant(opts, state, 13U);
    assert(g_tunes == 0);
    send_grant(opts, state, 12U);
    assert(g_tunes == 1 && g_tuned_hz[0] == 451137500L);
    assert(dmr_t3_missing_count(state) == 0U);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_exitflag_store(1);
    return DSD_SYNC_NONE;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
write_file(char* path, size_t size, const char* prefix, const char* text) {
    const int fd = dsd_test_mkstemp(path, size, prefix);
    assert(fd >= 0);
    const size_t len = strlen(text);
    assert(dsd_write(fd, text, len) == (ssize_t)len);
    assert(dsd_close(fd) == 0);
}

static void
init_run(dsd_opts* opts, dsd_state* state, const char* map, const char* overrides) {
    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof(opts->audio_out_dev), "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    opts->trunk_enable = 1;
    opts->trunk_tune_group_calls = 1;
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", map);
    DSD_SNPRINTF(opts->dmr_chan_overrides_file, sizeof opts->dmr_chan_overrides_file, "%s", overrides);
}

static void
test_overrides_applied_before_first_frame(const char* map, const char* overrides) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    init_run(opts, state, map, overrides);
    g_sync_calls = 0;
    g_tunes = 0;
    assert(dsd_engine_run_with_lifecycle(opts, state, NULL) == 0);
    assert(g_sync_calls == 1);
    freeState(state);
    free(state);
    free(opts);
}

/* Review gate b-r2 C1: a session whose decode, after the config file and the command line, is not DMR (an INI's
   [mode] decode, -fp) starts without the file, with one notice; so does one that runs a trunk scan (a profile can turn
   it on), which has no global map: its start goes on to the trunk scan's own checks (this one has no targets or tuner,
   which stop it there). */
static void
test_overrides_skipped_without_dmr(const char* map, const char* overrides) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    init_run(opts, state, map, overrides);
    opts->frame_dmr = 0;
    opts->frame_p25p1 = 1;
    g_expect_applied = 0;
    g_sync_calls = 0;
    g_skip_notices = 0;
    assert(dsd_engine_run_with_lifecycle(opts, state, NULL) == 0);
    assert(g_sync_calls == 1 && g_skip_notices == 1);
    assert(strstr(g_skip_notice, "this session does not decode DMR") != NULL);
    g_expect_applied = 1;
    freeState(state);
    free(state);
    free(opts);
}

static void
test_overrides_skipped_under_trunk_scan(const char* overrides) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    init_run(opts, state, "", overrides);
    opts->trunk_scan_enabled = 1;
    g_sync_calls = 0;
    g_skip_notices = 0;
    g_overrides_refusals = 0;
    g_scan_start_errors = 0;
    assert(dsd_engine_run_with_lifecycle(opts, state, NULL) != 0);
    assert(g_skip_notices == 1 && g_overrides_refusals == 0 && g_scan_start_errors == 1 && g_sync_calls == 0);
    assert(strstr(g_skip_notice, "a trunk scan uses each target's own channel map") != NULL);
    assert(dmr_t3_overlay_count(state) == 0U);
    freeState(state);
    free(state);
    free(opts);
}

static void
test_unreadable_overrides_stop_the_start(const char* map, const char* missing) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    init_run(opts, state, map, missing);
    g_sync_calls = 0;
    assert(dsd_engine_run_with_lifecycle(opts, state, NULL) != 0);
    assert(g_sync_calls == 0);
    freeState(state);
    free(state);
    free(opts);
}

int
main(void) {
    char map[DSD_TEST_PATH_MAX];
    char overrides[DSD_TEST_PATH_MAX];
    write_file(map, sizeof map, "dmr_overrides_map", "channel,freq\n12,451012500\n13,451025000\n14,451037500\n");
    write_file(overrides, sizeof overrides, "dmr_overrides", "lpcn,frequency_hz\n12,451137500\n13,0\n");
    dsd_neo_log_set_tap(skip_log_tap, NULL);
    test_overrides_applied_before_first_frame(map, overrides);
    test_overrides_skipped_without_dmr(map, overrides);
    test_overrides_skipped_under_trunk_scan(overrides);
    (void)remove(overrides);
    test_unreadable_overrides_stop_the_start(map, overrides);
    (void)remove(map);
    printf("ENGINE_DMR_CHAN_OVERRIDES: OK\n");
    return 0;
}
