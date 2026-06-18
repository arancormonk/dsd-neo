// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/symbol_levels.h>
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

float
// NOLINTNEXTLINE(misc-use-internal-linkage)
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    (void)opts;
    (void)have_sync;
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
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ns(uint64_t ns) {
    (void)ns;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_us(uint64_t us) {
    (void)us;
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    static dsdneoRuntimeConfig cfg;
    return &cfg;
}

uint8_t
dsd_fsk_symbol_reliability(float symbol, int levels) {
    (void)symbol;
    (void)levels;
    return 255;
}

static void
free_state_buffers(dsd_state* state) {
    free(state->dibit_buf);
    free(state->dmr_payload_buf);
    free(state->dmr_reliab_buf);
    free(state->dmr_soft_buf);
}

static int
init_state_buffers(dsd_state* state) {
    state->dibit_buf = (int*)calloc(1000, sizeof(int));
    state->dmr_payload_buf = (int*)calloc(1000, sizeof(int));
    state->dmr_reliab_buf = (uint8_t*)calloc(1000000, sizeof(uint8_t));
    state->dmr_soft_buf = (dsd_dibit_soft_t*)calloc(1000000, sizeof(dsd_dibit_soft_t));
    if (state->dibit_buf == NULL || state->dmr_payload_buf == NULL || state->dmr_reliab_buf == NULL
        || state->dmr_soft_buf == NULL) {
        free_state_buffers(state);
        return 0;
    }
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->dmr_reliab_p = state->dmr_reliab_buf + 200;
    state->dmr_soft_p = state->dmr_soft_buf + 200;
    return 1;
}

static int
llr_matches_bit(int16_t llr, int bit) {
    return bit ? (llr > 0) : (llr < 0);
}

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
    if (state.dmr_reliab_p[-1] != 255) {
        DSD_FPRINTF(stderr, "dibit %d: previous reliability buffer was not rebuilt\n", dibit);
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
    opts.symbol_capture_format = DSD_SYMBOL_CAPTURE_FORMAT_SOFT;
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

    state.dmr_soft_p[0].reliability = 7;
    state.dmr_soft_p++;
    opts.symbol_capture_format = DSD_SYMBOL_CAPTURE_FORMAT_SOFT;
    opts.symbol_out_f = f;
    write_symbol_capture_record(&opts, &state, 3, -3.0f);

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
    } else if (rec[0] != 3 || rec[1] != 255) {
        DSD_FPRINTF(stderr, "direct soft capture record mismatch dibit=%u reliability=%u\n", rec[0], rec[1]);
        rc = 1;
    } else if (state.symbol_capture_soft_records != 1) {
        DSD_FPRINTF(stderr, "direct soft capture counter mismatch %u\n", state.symbol_capture_soft_records);
        rc = 1;
    }

    f = tmpfile();
    if (f == NULL) {
        DSD_FPRINTF(stderr, "tmpfile failed\n");
        free_state_buffers(&state);
        return 1;
    }
    opts.symbol_capture_format = DSD_SYMBOL_CAPTURE_FORMAT_LEGACY;
    opts.symbol_out_f = f;
    write_symbol_capture_record(&opts, &state, 0xFF, 0.0f);
    int c = EOF;
    if (seek_start(f, "legacy capture record") == 0) {
        c = fgetc(f);
    }
    fclose(f);
    if (c != 0xFF) {
        DSD_FPRINTF(stderr, "legacy capture byte mismatch %d\n", c);
        rc = 1;
    } else if (state.symbol_capture_soft_records != 1) {
        DSD_FPRINTF(stderr, "legacy capture changed soft counter %u\n", state.symbol_capture_soft_records);
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
    rc |= test_direct_symbol_capture_writer_formats();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
