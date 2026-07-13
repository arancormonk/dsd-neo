// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/platform/sockets.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static size_t g_symbol_index;
static const char* g_sync_pattern = DMR_BS_DATA_SYNC;

dsd_socket_t
Connect(char* hostname, int portno) { // NOLINT(misc-use-internal-linkage)
    (void)hostname;
    (void)portno;
    return (dsd_socket_t)0;
}

int
openAudioInput(dsd_opts* opts) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    return -1;
}

int
dsd_audio_reconfigure_output_for_input_policy(dsd_opts* opts) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    return 0;
}

void
dsd_request_shutdown(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_audio_rescale_symbol_timing(dsd_state* state, int old_rate_hz, int new_rate_hz) {
    (void)state;
    (void)old_rate_hz;
    (void)new_rate_hz;
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out,
                          size_t out_size) { // NOLINT(misc-use-internal-linkage)
    (void)timestamp;
    (void)format;
    return out ? DSD_SNPRINTF(out, out_size, "%s", "00:00:00") >= 0 : 0;
}

void
printFrameInfo(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
dsd_mark_cc_sync(dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)state;
}

void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)slot;
}

void
write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol, const dsd_dibit_soft_t* soft) {
    (void)opts;
    (void)state;
    (void)dibit;
    (void)symbol;
    (void)soft;
}

uint8_t
dmr_compute_reliability(const dsd_state* st, float sym) {
    (void)st;
    (void)sym;
    return 255;
}

double
pwr_to_dB(double mean_power) { // NOLINT(misc-use-internal-linkage)
    (void)mean_power;
    return 0.0;
}

void
lpf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
hpf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
pbf_f(dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)state;
    (void)input;
    (void)len;
}

void
analog_gain_f(const dsd_opts* opts, dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

void
agsm_f(dsd_opts* opts, dsd_state* state, float* input, int len) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

static float
symbol_level_for_dibit(char dibit) {
    return (dibit == '3') ? -3.0f : 3.0f;
}

float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;

    const size_t pattern_len = strlen(g_sync_pattern);
    char dibit = (g_symbol_index < pattern_len) ? g_sync_pattern[g_symbol_index] : '1';
    g_symbol_index++;

    float symbol = symbol_level_for_dibit(dibit);
    dsd_symbol_history_push(state, symbol);
    return symbol;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_soft_buf);
    free(state->symbol_history);
    state->symbol_history = NULL;
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000000U, sizeof(int));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000000U, sizeof(dsd_dibit_soft_t));
    state->symbol_history = (float*)calloc(DSD_SYMBOL_HISTORY_SIZE, sizeof(float));
    if (!state->dibit_buf || !state->dmr_payload_buf || !state->dmr_soft_buf || !state->symbol_history) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    state->symbol_history_size = DSD_SYMBOL_HISTORY_SIZE;
    state->symbol_history_head = 0;
    state->symbol_history_count = 0;
    return 1;
}

typedef struct {
    const char* label;
    const char* pattern;
    const char* expected_ftype;
    int frame_dmr;
    int frame_dstar;
    int frame_provoice;
    int frame_x2tdma;
    int frame_ysf;
    int frame_dpmr;
    int frame_nxdn48;
    int frame_nxdn96;
    int inverted_x2tdma;
    int inverted_dpmr;
    int profile_index;
    int initial_lastsynctype;
    int expected_sync;
    int expected_inverted_ysf;
} SymbolSyncCase;

typedef struct {
    const char* label;
    const char* pattern;
    int inverted_dmr;
    int initial_lastsynctype;
    int expected_sync;
    int expected_directmode;
    int expected_firstframe;
    const char* expected_ftype;
} DmrSyncCase;

static void
init_common_wav_opts_state(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 96000;
    opts->wav_decimator = 48000;
    opts->mod_cli_lock = 1;
    opts->mod_gfsk = 1;
    opts->msize = 1;
    opts->ssize = 128;
    opts->frame_dstar = 1;
    opts->frame_x2tdma = 1;
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 1;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 1;
    opts->frame_provoice = 1;
    opts->frame_ysf = 1;
    opts->frame_m17 = 1;

    state->rf_mod = 2;
    state->samplesPerSymbol = 20;
    state->symbolCenter = 9;
    state->p25_p2_active_slot = -1;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;
}

static void
set_profile_timing(const dsd_opts* opts, dsd_state* state, int profile_index) {
    static const int symbol_rates[] = {4800, 2400, 9600, 6000, 4800};
    const int profile_count = (int)(sizeof(symbol_rates) / sizeof(symbol_rates[0]));
    const int safe_profile_index = (profile_index >= 0 && profile_index < profile_count) ? profile_index : 0;
    const int demod_rate = dsd_opts_current_input_timing_rate(opts);
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, symbol_rates[safe_profile_index], demod_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
}

static int
run_symbol_sync_case(const SymbolSyncCase* tc) {
    static dsd_opts opts;
    static dsd_state state;
    init_common_wav_opts_state(&opts, &state);

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate frame-sync state buffers\n");
        return 1;
    }

    opts.inverted_x2tdma = tc->inverted_x2tdma;
    opts.inverted_dpmr = tc->inverted_dpmr;
    state.sps_hunt_idx = tc->profile_index;
    set_profile_timing(&opts, &state, tc->profile_index);
    state.lastsynctype = tc->initial_lastsynctype;
    g_symbol_index = 0U;
    g_sync_pattern = tc->pattern;
    dsd_frame_sync_reset_mod_state();

    int sync = getFrameSync(&opts, &state);
    int rc = 0;
    if (sync != tc->expected_sync) {
        DSD_FPRINTF(stderr, "%s returned %d, expected %d\n", tc->label, sync, tc->expected_sync);
        rc = 1;
    }
    if (state.lastsynctype != tc->expected_sync) {
        DSD_FPRINTF(stderr, "%s lastsynctype %d, expected %d\n", tc->label, state.lastsynctype, tc->expected_sync);
        rc = 1;
    }
    if (state.carrier != 1) {
        DSD_FPRINTF(stderr, "%s did not set carrier lock\n", tc->label);
        rc = 1;
    }
    if (tc->expected_ftype && strcmp(state.ftype, tc->expected_ftype) != 0) {
        DSD_FPRINTF(stderr, "%s ftype '%s', expected '%s'\n", tc->label, state.ftype, tc->expected_ftype);
        rc = 1;
    }
    if (tc->frame_ysf && opts.inverted_ysf != tc->expected_inverted_ysf) {
        DSD_FPRINTF(stderr, "%s inverted_ysf %d, expected %d\n", tc->label, opts.inverted_ysf,
                    tc->expected_inverted_ysf);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
run_dmr_sync_case(const DmrSyncCase* tc) {
    static dsd_opts opts;
    static dsd_state state;
    init_common_wav_opts_state(&opts, &state);

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate DMR frame-sync state buffers\n");
        return 1;
    }

    opts.frame_dmr = 1;
    opts.inverted_dmr = tc->inverted_dmr;
    state.lastsynctype = tc->initial_lastsynctype;
    g_symbol_index = 0U;
    g_sync_pattern = tc->pattern;
    dsd_frame_sync_reset_mod_state();

    int sync = getFrameSync(&opts, &state);
    int rc = 0;
    if (sync != tc->expected_sync) {
        DSD_FPRINTF(stderr, "%s returned %d, expected %d\n", tc->label, sync, tc->expected_sync);
        rc = 1;
    }
    if (state.lastsynctype != tc->expected_sync) {
        DSD_FPRINTF(stderr, "%s lastsynctype %d, expected %d\n", tc->label, state.lastsynctype, tc->expected_sync);
        rc = 1;
    }
    if (state.directmode != tc->expected_directmode) {
        DSD_FPRINTF(stderr, "%s directmode %d, expected %d\n", tc->label, state.directmode, tc->expected_directmode);
        rc = 1;
    }
    if (state.firstframe != tc->expected_firstframe) {
        DSD_FPRINTF(stderr, "%s firstframe %d, expected %d\n", tc->label, state.firstframe, tc->expected_firstframe);
        rc = 1;
    }
    if (state.carrier != 1) {
        DSD_FPRINTF(stderr, "%s did not set carrier lock\n", tc->label);
        rc = 1;
    }
    if (tc->expected_ftype && strcmp(state.ftype, tc->expected_ftype) != 0) {
        DSD_FPRINTF(stderr, "%s ftype '%s', expected '%s'\n", tc->label, state.ftype, tc->expected_ftype);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
run_dmr_wav_rate_case(void) {
    SymbolSyncCase tc = {
        .label = "DMR WAV sync",
        .pattern = DMR_BS_DATA_SYNC,
        .frame_dmr = 1,
        .expected_sync = DSD_SYNC_DMR_BS_DATA_POS,
    };
    int rc = run_symbol_sync_case(&tc);
    if (rc != 0) {
        return rc;
    }

    static dsd_opts opts;
    static dsd_state state;
    init_common_wav_opts_state(&opts, &state);
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate DMR timing frame-sync state buffers\n");
        return 1;
    }
    opts.frame_dmr = 1;
    g_symbol_index = 0U;
    g_sync_pattern = DMR_BS_DATA_SYNC;
    dsd_frame_sync_reset_mod_state();
    (void)getFrameSync(&opts, &state);
    if (state.samplesPerSymbol != 20 || state.symbolCenter != 9) {
        DSD_FPRINTF(stderr, "DMR WAV timing changed to sps=%d center=%d, expected 20/9\n", state.samplesPerSymbol,
                    state.symbolCenter);
        rc = 1;
    }
    free_state_buffers(&state);
    return rc;
}

static int
run_dmr_sync_variant_matrix(void) {
    static const DmrSyncCase cases[] = {
        {
            .label = "DMR MS data",
            .pattern = DMR_MS_DATA_SYNC,
            .expected_sync = DSD_SYNC_DMR_MS_DATA,
            .expected_directmode = 0,
            .expected_firstframe = 0,
            .expected_ftype = "DMR MS",
        },
        {
            .label = "DMR MS voice inverted",
            .pattern = DMR_MS_VOICE_SYNC,
            .inverted_dmr = 1,
            .expected_sync = DSD_SYNC_DMR_MS_DATA,
            .expected_directmode = 0,
            .expected_firstframe = 0,
            .expected_ftype = "DMR MS",
        },
        {
            .label = "DMR BS voice",
            .pattern = DMR_BS_VOICE_SYNC,
            .initial_lastsynctype = DSD_SYNC_NONE,
            .expected_sync = DSD_SYNC_DMR_BS_VOICE_POS,
            .expected_directmode = 0,
            .expected_firstframe = 1,
            .expected_ftype = "DMR ",
        },
        {
            .label = "DMR BS voice inverted",
            .pattern = DMR_BS_VOICE_SYNC,
            .inverted_dmr = 1,
            .expected_sync = DSD_SYNC_DMR_BS_DATA_NEG,
            .expected_directmode = 0,
            .expected_firstframe = 0,
            .expected_ftype = "DMR ",
        },
        {
            .label = "DMR direct TS1 data",
            .pattern = DMR_DIRECT_MODE_TS1_DATA_SYNC,
            .expected_sync = DSD_SYNC_DMR_MS_DATA,
            .expected_directmode = 1,
            .expected_firstframe = 0,
            .expected_ftype = "DMR ",
        },
        {
            .label = "DMR direct TS2 data inverted",
            .pattern = DMR_DIRECT_MODE_TS2_DATA_SYNC,
            .inverted_dmr = 1,
            .initial_lastsynctype = DSD_SYNC_NONE,
            .expected_sync = DSD_SYNC_DMR_MS_VOICE,
            .expected_directmode = 1,
            .expected_firstframe = 1,
            .expected_ftype = "DMR ",
        },
        {
            .label = "DMR direct TS1 voice",
            .pattern = DMR_DIRECT_MODE_TS1_VOICE_SYNC,
            .initial_lastsynctype = DSD_SYNC_NONE,
            .expected_sync = DSD_SYNC_DMR_MS_VOICE,
            .expected_directmode = 1,
            .expected_firstframe = 1,
            .expected_ftype = "DMR ",
        },
        {
            .label = "DMR direct TS2 voice inverted",
            .pattern = DMR_DIRECT_MODE_TS2_VOICE_SYNC,
            .inverted_dmr = 1,
            .expected_sync = DSD_SYNC_DMR_MS_DATA,
            .expected_directmode = 1,
            .expected_firstframe = 0,
            .expected_ftype = "DMR ",
        },
    };

    int rc = 0;
    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        rc |= run_dmr_sync_case(&cases[i]);
    }
    return rc;
}

static int
run_wav_protocol_sync_matrix(void) {
    static const SymbolSyncCase cases[] = {
        {
            .label = "P25 Phase 1 positive",
            .pattern = P25P1_SYNC,
            .expected_sync = DSD_SYNC_P25P1_POS,
            .expected_ftype = "P25 Phase 1",
        },
        {
            .label = "P25 Phase 1 negative",
            .pattern = INV_P25P1_SYNC,
            .expected_sync = DSD_SYNC_P25P1_NEG,
            .expected_ftype = "P25 Phase 1",
        },
        {
            .label = "P25 Phase 2 positive",
            .pattern = P25P2_SYNC,
            .profile_index = 3,
            .expected_sync = DSD_SYNC_P25P2_POS,
        },
        {
            .label = "P25 Phase 2 negative",
            .pattern = INV_P25P2_SYNC,
            .profile_index = 3,
            .expected_sync = DSD_SYNC_P25P2_NEG,
        },
        {
            .label = "DSTAR voice positive",
            .pattern = DSTAR_SYNC,
            .frame_dstar = 1,
            .profile_index = 4,
            .expected_sync = DSD_SYNC_DSTAR_VOICE_POS,
            .expected_ftype = "DSTAR ",
        },
        {
            .label = "DSTAR voice negative",
            .pattern = INV_DSTAR_SYNC,
            .frame_dstar = 1,
            .profile_index = 4,
            .expected_sync = DSD_SYNC_DSTAR_VOICE_NEG,
            .expected_ftype = "DSTAR ",
        },
        {
            .label = "DSTAR header positive",
            .pattern = DSTAR_HD,
            .frame_dstar = 1,
            .profile_index = 4,
            .expected_sync = DSD_SYNC_DSTAR_HD_POS,
            .expected_ftype = "DSTAR_HD ",
        },
        {
            .label = "DSTAR header negative",
            .pattern = INV_DSTAR_HD,
            .frame_dstar = 1,
            .profile_index = 4,
            .expected_sync = DSD_SYNC_DSTAR_HD_NEG,
            .expected_ftype = " DSTAR_HD",
        },
        {
            .label = "ProVoice positive",
            .pattern = PROVOICE_SYNC,
            .frame_provoice = 1,
            .profile_index = 2,
            .expected_sync = DSD_SYNC_PROVOICE_POS,
            .expected_ftype = "ProVoice ",
        },
        {
            .label = "ProVoice negative",
            .pattern = INV_PROVOICE_SYNC,
            .frame_provoice = 1,
            .profile_index = 2,
            .expected_sync = DSD_SYNC_PROVOICE_NEG,
            .expected_ftype = "ProVoice ",
        },
        {
            .label = "EDACS positive",
            .pattern = INV_EDACS_SYNC,
            .frame_provoice = 1,
            .profile_index = 2,
            .expected_sync = DSD_SYNC_EDACS_POS,
        },
        {
            .label = "EDACS negative",
            .pattern = EDACS_SYNC,
            .frame_provoice = 1,
            .profile_index = 2,
            .expected_sync = DSD_SYNC_EDACS_NEG,
        },
    };

    int rc = 0;
    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        rc |= run_symbol_sync_case(&cases[i]);
    }
    return rc;
}

static int
run_wav_protocol_extended_sync_matrix(void) {
    static const SymbolSyncCase cases[] = {
        {
            .label = "X2-TDMA BS data",
            .pattern = X2TDMA_BS_DATA_SYNC,
            .frame_x2tdma = 1,
            .profile_index = 3,
            .expected_sync = DSD_SYNC_X2TDMA_DATA_POS,
            .expected_ftype = "X2-TDMA",
        },
        {
            .label = "X2-TDMA BS voice inverted maps to data-neg",
            .pattern = X2TDMA_BS_VOICE_SYNC,
            .frame_x2tdma = 1,
            .inverted_x2tdma = 1,
            .profile_index = 3,
            .expected_sync = DSD_SYNC_X2TDMA_DATA_NEG,
            .expected_ftype = "X2-TDMA",
        },
        {
            .label = "YSF positive",
            .pattern = FUSION_SYNC,
            .frame_ysf = 1,
            .initial_lastsynctype = DSD_SYNC_YSF_POS,
            .expected_sync = DSD_SYNC_YSF_POS,
            .expected_inverted_ysf = 0,
        },
        {
            .label = "YSF negative",
            .pattern = INV_FUSION_SYNC,
            .frame_ysf = 1,
            .initial_lastsynctype = DSD_SYNC_YSF_NEG,
            .expected_sync = DSD_SYNC_YSF_NEG,
            .expected_inverted_ysf = 1,
        },
        {
            .label = "dPMR FS2 positive",
            .pattern = DPMR_FRAME_SYNC_2,
            .frame_dpmr = 1,
            .profile_index = 1,
            .expected_sync = DSD_SYNC_DPMR_FS2_POS,
            .expected_ftype = "dPMR ",
        },
        {
            .label = "dPMR FS2 negative",
            .pattern = INV_DPMR_FRAME_SYNC_2,
            .frame_dpmr = 1,
            .inverted_dpmr = 1,
            .profile_index = 1,
            .expected_sync = DSD_SYNC_DPMR_FS2_NEG,
            .expected_ftype = "dPMR ",
        },
    };

    int rc = 0;
    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); i++) {
        rc |= run_symbol_sync_case(&cases[i]);
    }
    return rc;
}

static int
run_nxdn_two_pass_case(const char* label, const char* pattern, int frame_nxdn48, int frame_nxdn96, int expected_sync) {
    static dsd_opts opts;
    static dsd_state state;
    init_common_wav_opts_state(&opts, &state);

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate NXDN frame-sync state buffers\n");
        return 1;
    }

    state.sps_hunt_idx = frame_nxdn48 ? 1 : (frame_nxdn96 ? 0 : state.sps_hunt_idx);
    set_profile_timing(&opts, &state, state.sps_hunt_idx);
    dsd_frame_sync_reset_mod_state();

    g_symbol_index = 0U;
    g_sync_pattern = pattern;
    int sync = getFrameSync(&opts, &state);
    int rc = 0;
    if (sync != DSD_SYNC_NONE) {
        DSD_FPRINTF(stderr, "%s first pass returned %d, expected no confirmed sync\n", label, sync);
        rc = 1;
    }
    if (state.lastsynctype != expected_sync) {
        DSD_FPRINTF(stderr, "%s first pass lastsynctype %d, expected staged %d\n", label, state.lastsynctype,
                    expected_sync);
        rc = 1;
    }

    g_symbol_index = 0U;
    sync = getFrameSync(&opts, &state);
    if (sync != expected_sync) {
        DSD_FPRINTF(stderr, "%s second pass returned %d, expected %d\n", label, sync, expected_sync);
        rc = 1;
    }
    if (state.lastsynctype != expected_sync) {
        DSD_FPRINTF(stderr, "%s second pass lastsynctype %d, expected %d\n", label, state.lastsynctype, expected_sync);
        rc = 1;
    }
    if (state.offset <= 0) {
        DSD_FPRINTF(stderr, "%s did not record a positive sync offset\n", label);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
run_nxdn_sync_confirmation_matrix(void) {
    int rc = 0;
    rc |= run_nxdn_two_pass_case("NXDN48 positive FSW", NXDN_FSW, 1, 0, DSD_SYNC_NXDN_POS);
    rc |= run_nxdn_two_pass_case("NXDN96 negative FSW", INV_NXDN_FSW, 0, 1, DSD_SYNC_NXDN_NEG);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= run_dmr_wav_rate_case();
    rc |= run_dmr_sync_variant_matrix();
    rc |= run_wav_protocol_sync_matrix();
    rc |= run_wav_protocol_extended_sync_matrix();
    rc |= run_nxdn_sync_confirmation_matrix();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
