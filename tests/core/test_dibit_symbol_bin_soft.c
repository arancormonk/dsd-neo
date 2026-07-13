// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_next_dibit;
static int g_get_symbol_calls;
static uint64_t g_now_ns;
static int g_sleep_ms_calls;
static int g_sleep_ns_calls;
static uint64_t g_last_sleep_ns;

float
// NOLINTNEXTLINE(misc-use-internal-linkage)
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    (void)opts;
    (void)have_sync;
    g_get_symbol_calls++;
    state->symbolc = g_next_dibit;
    switch (g_next_dibit & 3) {
        case 0: return 1.0f;
        case 1: return 3.0f;
        case 2: return -1.0f;
        case 3: return -3.0f;
        default: break;
    }
    return 0.0f;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return g_now_ns;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
    g_sleep_ms_calls++;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ns(uint64_t ns) {
    g_sleep_ns_calls++;
    g_last_sleep_ns = ns;
}

void
dsd_neo_config_init(void) {}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    static dsdneoRuntimeConfig cfg;
    return &cfg;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_soft_buf);
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000001, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000001, sizeof(int));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000001, sizeof(dsd_dibit_soft_t));
    if (state->dibit_buf == NULL || state->dmr_payload_buf == NULL || state->dmr_soft_buf == NULL) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    return 1;
}

static void
reset_timing_stubs(uint64_t now_ns) {
    g_now_ns = now_ns;
    g_sleep_ms_calls = 0;
    g_sleep_ns_calls = 0;
    g_last_sleep_ns = 0;
}

static int
llr_matches_bit(int16_t llr, int bit) {
    return bit ? (llr > 0) : (llr < 0);
}

static void set_standard_thresholds(dsd_state* state);

static int
test_symbol_bin_soft_matches_returned_dibit(int dibit) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;

    state.synctype = DSD_SYNC_P25P1_NEG;
    state.rf_mod = 0;
    state.center = 0.0f;
    state.min = -3.0f;
    state.max = 3.0f;
    state.lmid = -2.0f;
    state.umid = 2.0f;

    g_next_dibit = dibit;

    dsd_dibit_soft_t soft;
    DSD_MEMSET(&soft, 0, sizeof(soft));
    int got = getDibitSoft(&opts, &state, &soft);

    int rc = 0;
    if (got != dibit) {
        DSD_FPRINTF(stderr, "dibit %d: got returned dibit %d\n", dibit, got);
        rc = 1;
    }
    if (!llr_matches_bit(soft.llr[0], (dibit >> 1) & 1) || !llr_matches_bit(soft.llr[1], dibit & 1)) {
        DSD_FPRINTF(stderr, "dibit %d: soft signs do not match returned dibit (%d,%d)\n", dibit, soft.llr[0],
                    soft.llr[1]);
        rc = 1;
    }
    if (soft.reliability != 255) {
        DSD_FPRINTF(stderr, "dibit %d: expected symbol-bin reliability 255, got %u\n", dibit, soft.reliability);
        rc = 1;
    }
    const dsd_dibit_soft_t previous = state.dmr_soft_p[-1];
    if (previous.llr[0] != soft.llr[0] || previous.llr[1] != soft.llr[1] || previous.reliability != soft.reliability) {
        DSD_FPRINTF(stderr, "dibit %d: previous soft buffer does not match returned soft metric\n", dibit);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
seek_start(FILE* f, const char* label) {
    if (fseek(f, 0L, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "%s seek failed\n", label);
        return 1;
    }
    return 0;
}

static int
test_soft_symbol_capture_record(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }

    FILE* f = tmpfile();
    if (f == NULL) {
        DSD_FPRINTF(stderr, "tmpfile failed\n");
        free_state_buffers(&state);
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    opts.symbol_out_f = f;
    state.synctype = DSD_SYNC_P25P1_NEG;
    state.rf_mod = 0;
    state.center = 0.0f;
    state.min = -3.0f;
    state.max = 3.0f;
    state.lmid = -2.0f;
    state.umid = 2.0f;

    g_next_dibit = 2;
    dsd_dibit_soft_t soft;
    DSD_MEMSET(&soft, 0, sizeof(soft));
    int got = getDibitSoft(&opts, &state, &soft);

    unsigned char rec[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    size_t n = 0;
    if (seek_start(f, "soft capture record") == 0) {
        n = fread(rec, 1, sizeof(rec), f);
    }
    fclose(f);

    int rc = 0;
    if (got != 2 || n != sizeof(rec)) {
        DSD_FPRINTF(stderr, "soft capture record missing got=%d bytes=%zu\n", got, n);
        rc = 1;
    } else if (rec[0] != 2 || rec[1] != 255) {
        DSD_FPRINTF(stderr, "soft capture record dibit/reliability mismatch %u/%u\n", rec[0], rec[1]);
        rc = 1;
    } else if (state.symbol_capture_soft_records != 1) {
        DSD_FPRINTF(stderr, "soft capture counter mismatch %u\n", state.symbol_capture_soft_records);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_symbol_replay_soft_metric_overrides_fallback(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    state.synctype = DSD_SYNC_P25P1_NEG;
    state.rf_mod = 0;

    state.symbol_replay_has_soft = 1;
    state.symbol_replay_soft.reliability = 17;
    state.symbol_replay_soft.llr[0] = 123;
    state.symbol_replay_soft.llr[1] = -456;
    g_next_dibit = 1;

    dsd_dibit_soft_t soft;
    DSD_MEMSET(&soft, 0, sizeof(soft));
    int got = getDibitSoft(&opts, &state, &soft);

    int rc = 0;
    if (got != 1) {
        DSD_FPRINTF(stderr, "soft replay override: got returned dibit %d\n", got);
        rc = 1;
    } else if (soft.reliability != 17 || soft.llr[0] != 123 || soft.llr[1] != -456) {
        DSD_FPRINTF(stderr, "soft replay override mismatch rel=%u llr=(%d,%d)\n", soft.reliability, soft.llr[0],
                    soft.llr[1]);
        rc = 1;
    } else if (state.symbol_replay_has_soft != 0) {
        DSD_FPRINTF(stderr, "soft replay override flag was not consumed\n");
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_symbol_bin_replay_throttle_paces_from_timing_rate(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }

    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    opts.wav_sample_rate = 48000;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.rf_mod = 0;
    state.use_throttle = 1;
    state.samplesPerSymbol = 12;
    state.lastsynctype = -1;
    set_standard_thresholds(&state);

    int rc = 0;
    reset_timing_stubs(2000000ULL);
    g_next_dibit = 1;
    int got = get_dibit_and_analog_signal(&opts, &state, NULL);
    if (got != 1 || state.symbol_replay_next_deadline_ns != 2250000ULL || g_sleep_ns_calls != 0
        || g_sleep_ms_calls != 0) {
        DSD_FPRINTF(stderr, "initial auto throttle mismatch got=%d deadline=%llu ns_calls=%d ms_calls=%d\n", got,
                    (unsigned long long)state.symbol_replay_next_deadline_ns, g_sleep_ns_calls, g_sleep_ms_calls);
        rc = 1;
    }

    reset_timing_stubs(2100000ULL);
    g_next_dibit = 2;
    got = get_dibit_and_analog_signal(&opts, &state, NULL);
    if (got != 2 || g_sleep_ns_calls != 1 || g_last_sleep_ns != 150000ULL
        || state.symbol_replay_next_deadline_ns != 2500000ULL) {
        DSD_FPRINTF(stderr, "paced auto throttle mismatch got=%d ns_calls=%d ns=%llu deadline=%llu\n", got,
                    g_sleep_ns_calls, (unsigned long long)g_last_sleep_ns,
                    (unsigned long long)state.symbol_replay_next_deadline_ns);
        rc = 1;
    }

    reset_timing_stubs(253000001ULL);
    g_next_dibit = 3;
    got = get_dibit_and_analog_signal(&opts, &state, NULL);
    if (got != 3 || g_sleep_ns_calls != 0 || state.symbol_replay_next_deadline_ns != 253250001ULL) {
        DSD_FPRINTF(stderr, "stalled auto throttle rebase mismatch got=%d ns_calls=%d deadline=%llu\n", got,
                    g_sleep_ns_calls, (unsigned long long)state.symbol_replay_next_deadline_ns);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_reader_wraps_ring_buffers_before_store(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }
    set_standard_thresholds(&state);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.lastsynctype = -1;
    state.dibit_buf_p = state.dibit_buf + 900001;
    state.dmr_payload_p = state.dmr_payload_buf + 900001;
    state.dmr_soft_p = state.dmr_soft_buf + 900001;
    g_next_dibit = 1;

    dsd_dibit_soft_t soft;
    DSD_MEMSET(&soft, 0, sizeof(soft));
    int got = getDibitSoft(&opts, &state, &soft);

    int rc = 0;
    if (got != 1 || state.dibit_buf_p != state.dibit_buf + 201 || state.dmr_payload_p != state.dmr_payload_buf + 201
        || state.dmr_soft_p != state.dmr_soft_buf + 201) {
        DSD_FPRINTF(stderr, "ring wrap pointer mismatch got=%d d=%td p=%td s=%td\n", got,
                    state.dibit_buf_p - state.dibit_buf, state.dmr_payload_p - state.dmr_payload_buf,
                    state.dmr_soft_p - state.dmr_soft_buf);
        rc = 1;
    } else if (state.dibit_buf[200] != 1 || state.dmr_payload_buf[200] != 1
               || state.dmr_soft_buf[200].reliability != soft.reliability) {
        DSD_FPRINTF(stderr, "ring wrap stored values mismatch d=%d p=%d s=%u soft=%u\n", state.dibit_buf[200],
                    state.dmr_payload_buf[200], state.dmr_soft_buf[200].reliability, soft.reliability);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_get_dibit_soft_falls_back_to_hard_dibit(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }
    set_standard_thresholds(&state);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.lastsynctype = -1;
    free(state.dmr_soft_buf);
    state.dmr_soft_buf = NULL;
    state.dmr_soft_p = NULL;
    g_next_dibit = 2;

    dsd_dibit_soft_t soft;
    DSD_MEMSET(&soft, 0, sizeof(soft));
    int got = getDibitSoft(&opts, &state, &soft);

    int rc = 0;
    if (got != 2 || soft.reliability != 255 || !llr_matches_bit(soft.llr[0], 1) || !llr_matches_bit(soft.llr[1], 0)) {
        DSD_FPRINTF(stderr, "soft fallback mismatch got=%d rel=%u llr=(%d,%d)\n", got, soft.reliability, soft.llr[0],
                    soft.llr[1]);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_direct_symbol_capture_writer_formats(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }

    FILE* f = tmpfile();
    if (f == NULL) {
        DSD_FPRINTF(stderr, "tmpfile failed\n");
        free_state_buffers(&state);
        return 1;
    }

    const dsd_dibit_soft_t soft = {.reliability = 7, .llr = {-7, 7}};
    opts.symbol_out_f = f;
    write_symbol_capture_record(&opts, &state, 3, -3.0f, &soft);

    unsigned char rec[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    size_t n = 0;
    if (seek_start(f, "direct soft capture record") == 0) {
        n = fread(rec, 1, sizeof(rec), f);
    }
    fclose(f);

    int rc = 0;
    if (n != sizeof(rec)) {
        DSD_FPRINTF(stderr, "direct soft capture record missing bytes=%zu\n", n);
        rc = 1;
    } else if (rec[0] != 3 || rec[1] != 7) {
        DSD_FPRINTF(stderr, "direct soft capture record mismatch dibit=%u reliability=%u\n", rec[0], rec[1]);
        rc = 1;
    } else if (state.symbol_capture_soft_records != 1) {
        DSD_FPRINTF(stderr, "direct soft capture counter mismatch %u\n", state.symbol_capture_soft_records);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static void
set_standard_thresholds(dsd_state* state) {
    state->rf_mod = 0;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
}

static int
test_digitize_public_threshold_paths(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }
    set_standard_thresholds(&state);

    int rc = 0;
    if (digitize(NULL, &state, 0.0f) != -1 || digitize(&opts, NULL, 0.0f) != -1) {
        DSD_FPRINTF(stderr, "digitize NULL guard failed\n");
        rc = 1;
    }

    state.synctype = DSD_SYNC_DSTAR_VOICE_POS;
    if (digitize(&opts, &state, 1.0f) != 0 || state.dibit_buf_p[-1] != 1) {
        DSD_FPRINTF(stderr, "positive two-level high symbol mismatch\n");
        rc = 1;
    }
    if (digitize(&opts, &state, -1.0f) != 1 || state.dibit_buf_p[-1] != 3) {
        DSD_FPRINTF(stderr, "positive two-level low symbol mismatch\n");
        rc = 1;
    }

    state.synctype = DSD_SYNC_DSTAR_VOICE_NEG;
    if (digitize(&opts, &state, 1.0f) != 1 || state.dibit_buf_p[-1] != 1) {
        DSD_FPRINTF(stderr, "negative two-level high symbol mismatch\n");
        rc = 1;
    }
    if (digitize(&opts, &state, -1.0f) != 0 || state.dibit_buf_p[-1] != 3) {
        DSD_FPRINTF(stderr, "negative two-level low symbol mismatch\n");
        rc = 1;
    }

    state.synctype = DSD_SYNC_P25P1_POS;
    const float pos_symbols[4] = {1.0f, 3.0f, -1.0f, -3.0f};
    for (int expected = 0; expected < 4; expected++) {
        int got = digitize(&opts, &state, pos_symbols[expected]);
        if (got != expected || state.dibit_buf_p[-1] != expected || state.last_dibit != expected) {
            DSD_FPRINTF(stderr, "positive four-level symbol %d got=%d stored=%d last=%d\n", expected, got,
                        state.dibit_buf_p[-1], state.last_dibit);
            rc = 1;
        }
    }

    state.synctype = DSD_SYNC_P25P1_NEG;
    const int neg_expected[4] = {2, 3, 0, 1};
    for (int i = 0; i < 4; i++) {
        int got = digitize(&opts, &state, pos_symbols[i]);
        if (got != neg_expected[i] || state.last_dibit != neg_expected[i]) {
            DSD_FPRINTF(stderr, "negative four-level symbol %d got=%d last=%d\n", i, got, state.last_dibit);
            rc = 1;
        }
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_reader_apis_and_soft_symbol_ring(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }
    set_standard_thresholds(&state);
    state.synctype = DSD_SYNC_P25P1_POS;
    state.lastsynctype = -1;

    int analog = 0;
    g_next_dibit = 0;
    int got = get_dibit_and_analog_signal(&opts, &state, &analog);
    int rc = 0;
    if (got != 0 || analog != 1) {
        DSD_FPRINTF(stderr, "analog reader mismatch got=%d analog=%d\n", got, analog);
        rc = 1;
    }

    g_next_dibit = 3;
    got = get_dibit_and_analog_signal(&opts, &state, NULL);
    if (got != 3) {
        DSD_FPRINTF(stderr, "dibit reader mismatch got=%d\n", got);
        rc = 1;
    }

    dsd_dibit_soft_t dibit_soft;
    g_next_dibit = 1;
    got = getDibitSoft(&opts, &state, &dibit_soft);
    if (got != 1 || dibit_soft.reliability == 0) {
        DSD_FPRINTF(stderr, "soft reader mismatch got=%d rel=%u\n", got, dibit_soft.reliability);
        rc = 1;
    }

    state.soft_symbol_head = 511;
    float soft_symbol = 0.0f;
    g_next_dibit = 2;
    got = getDibitAndSoftSymbol(&opts, &state, &soft_symbol);
    if (got != 2 || soft_symbol != -1.0f || state.soft_symbol_buf[511] != -1.0f || state.soft_symbol_head != 0) {
        DSD_FPRINTF(stderr, "soft symbol ring mismatch got=%d sym=%.1f stored=%.1f head=%d\n", got, soft_symbol,
                    state.soft_symbol_buf[511], state.soft_symbol_head);
        rc = 1;
    }

    soft_symbol_frame_begin(&state);
    if (state.soft_symbol_frame_start != state.soft_symbol_head) {
        DSD_FPRINTF(stderr, "soft symbol frame start mismatch %d/%d\n", state.soft_symbol_frame_start,
                    state.soft_symbol_head);
        rc = 1;
    }

    if (getDibitAndSoftSymbol(NULL, &state, NULL) != -1 || getDibitAndSoftSymbol(&opts, NULL, NULL) != -1) {
        DSD_FPRINTF(stderr, "soft symbol reader NULL guard failed\n");
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

static int
test_viterbi_metric_public_edges(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    set_standard_thresholds(&state);

    int rc = 0;
    if (soft_symbol_to_viterbi_cost(0.0f, NULL, 0) != 32768U
        || gmsk_soft_symbol_to_viterbi_cost(0.0f, NULL) != 32768U) {
        DSD_FPRINTF(stderr, "viterbi NULL guard failed\n");
        rc = 1;
    }

    uint16_t positive_msb = soft_symbol_to_viterbi_cost(3.0f, &state, 0);
    uint16_t negative_msb = soft_symbol_to_viterbi_cost(-3.0f, &state, 0);
    uint16_t outer_lsb = soft_symbol_to_viterbi_cost(3.0f, &state, 1);
    uint16_t inner_lsb = soft_symbol_to_viterbi_cost(1.0f, &state, 1);
    if (!(positive_msb < 2000U && negative_msb > 63500U && inner_lsb < outer_lsb && inner_lsb < 20000U
          && outer_lsb > 56000U)) {
        DSD_FPRINTF(stderr, "four-level viterbi costs unexpected msb=%u/%u lsb=%u/%u\n", positive_msb, negative_msb,
                    outer_lsb, inner_lsb);
        rc = 1;
    }

    uint16_t low_bit = gmsk_soft_symbol_to_viterbi_cost(-3.0f, &state);
    uint16_t high_bit = gmsk_soft_symbol_to_viterbi_cost(3.0f, &state);
    if (!(low_bit < 2000U && high_bit > 63500U)) {
        DSD_FPRINTF(stderr, "gmsk viterbi costs unexpected low=%u high=%u\n", low_bit, high_bit);
        rc = 1;
    }

    DSD_MEMSET(&state, 0, sizeof(state));
    state.center = 0.0f;
    uint16_t degenerate = soft_symbol_to_viterbi_cost(0.5f, &state, 0);
    uint16_t degenerate_gmsk = gmsk_soft_symbol_to_viterbi_cost(0.5f, &state);
    if (degenerate == 32768U || degenerate_gmsk == 32768U) {
        DSD_FPRINTF(stderr, "degenerate viterbi fallback stayed neutral %u/%u\n", degenerate, degenerate_gmsk);
        rc = 1;
    }

    return rc;
}

static int
test_skip_dibit_consumes_requested_symbols(void) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "failed to allocate state buffers\n");
        return 1;
    }
    set_standard_thresholds(&state);
    state.synctype = DSD_SYNC_P25P1_POS;
    g_next_dibit = 3;
    g_get_symbol_calls = 0;

    skipDibit(&opts, &state, 3);

    int rc = 0;
    if (g_get_symbol_calls != 3) {
        DSD_FPRINTF(stderr, "skipDibit consumed %d symbols, want 3\n", g_get_symbol_calls);
        rc = 1;
    } else if (state.last_dibit != 3 || state.dibit_buf_p[-1] != 3) {
        DSD_FPRINTF(stderr, "skipDibit final dibit mismatch last=%d stored=%d\n", state.last_dibit,
                    state.dibit_buf_p[-1]);
        rc = 1;
    }

    free_state_buffers(&state);
    return rc;
}

int
main(void) {
    int rc = 0;
    for (int dibit = 0; dibit < 4; dibit++) {
        rc |= test_symbol_bin_soft_matches_returned_dibit(dibit);
    }
    rc |= test_soft_symbol_capture_record();
    rc |= test_symbol_replay_soft_metric_overrides_fallback();
    rc |= test_symbol_bin_replay_throttle_paces_from_timing_rate();
    rc |= test_reader_wraps_ring_buffers_before_store();
    rc |= test_get_dibit_soft_falls_back_to_hard_dibit();
    rc |= test_direct_symbol_capture_writer_formats();
    rc |= test_digitize_public_threshold_paths();
    rc |= test_reader_apis_and_soft_symbol_ring();
    rc |= test_viterbi_metric_public_edges();
    rc |= test_skip_dibit_consumes_requested_symbols();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
