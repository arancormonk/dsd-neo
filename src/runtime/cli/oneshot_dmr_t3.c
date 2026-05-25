// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR TIII LCN calculator one-shot utility - moved from apps/dsd-cli/main.c
 *        to make runtime CLI self-contained.
 */

#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// --- Local helpers -----------------------------------------------------------

static int
cmp_long(const void* a, const void* b) {
    long aa = *(const long*)a;
    long bb = *(const long*)b;
    if (aa < bb) {
        return -1;
    }
    if (aa > bb) {
        return 1;
    }
    return 0;
}

static long
nearest_125(long hz) {
    // round to nearest 125 Hz
    long q = (hz + (hz >= 0 ? 62 : -62)) / 125;
    return q * 125;
}

static long
infer_step_125(const long* f, int n) {
    // Infer channel spacing, snapped to 125 Hz grid.
    // Use the smallest positive rounded diff as conservative step.
    long best = 0;
    for (int i = 1; i < n; i++) {
        long d = f[i] - f[i - 1];
        if (d <= 0) {
            continue;
        }
        long r = nearest_125(d);
        if (r <= 0) {
            continue;
        }
        if (best == 0 || r < best) {
            best = r;
        }
    }
    return best;
}

static int
line_has_ascii_digit(const char* line) {
    if (!line) {
        return 0;
    }
    for (const char* p = line; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            return 1;
        }
    }
    return 0;
}

static char*
line_find_numeric_start(char* line) {
    if (!line) {
        return NULL;
    }
    while (*line && !((*line >= '0' && *line <= '9') || *line == '+' || *line == '-')) {
        line++;
    }
    return *line ? line : NULL;
}

static int
line_parse_frequency_hz(char* line, long* hz_out) {
    if (!hz_out || !line_has_ascii_digit(line)) {
        return 0;
    }

    char* start = line_find_numeric_start(line);
    if (!start) {
        return 0;
    }

    char* end = start;
    double val = strtod(start, &end);
    if (end == start) {
        return 0;
    }

    long hz = (long)llround(val);
    if (val < 1e5) {
        hz = (long)llround(val * 1000000.0);
    }
    if (hz <= 0) {
        return 0;
    }

    *hz_out = hz;
    return 1;
}

static int
read_frequency_rows(FILE* fp, long* freqs, int capacity) {
    if (!fp || !freqs || capacity <= 0) {
        return 0;
    }

    int count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        long hz = 0;
        if (!line_parse_frequency_hz(line, &hz)) {
            continue;
        }
        if (count < capacity) {
            freqs[count++] = hz;
        }
    }
    return count;
}

static int
dedupe_sorted_frequencies(long* freqs, int count) {
    if (!freqs || count <= 1) {
        return count;
    }

    int unique = 1;
    for (int i = 1; i < count; i++) {
        if (freqs[i] != freqs[unique - 1]) {
            freqs[unique++] = freqs[i];
        }
    }
    return unique;
}

static int
emit_lcn_frequency_rows(const long* freqs, int count, long base_freq, long base_lcn, long step) {
    if (!freqs || count <= 0 || step <= 0) {
        LOG_ERROR("LCN calc: invalid mapping arguments.\n");
        return 3;
    }

    printf("lcn,freq\n");
    for (int i = 0; i < count; i++) {
        long delta = freqs[i] - base_freq;
        long idx = (long)llround((double)delta / (double)step);
        long lcn = base_lcn + idx;
        printf("%ld,%ld\n", lcn, freqs[i]);
    }
    return 0;
}

static const dsdneoRuntimeConfig*
resolve_runtime_config(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg) {
        return cfg;
    }
    dsd_neo_config_init(NULL);
    return dsd_neo_get_config();
}

static long
dmr_t3_start_lcn_or_default(const dsdneoRuntimeConfig* cfg) {
    return (cfg && cfg->dmr_t3_start_lcn > 0) ? cfg->dmr_t3_start_lcn : 1;
}

static int
resolve_channel_step_hz(const dsdneoRuntimeConfig* cfg, const long* freqs, int count, long* step_out) {
    if (!step_out) {
        LOG_ERROR("LCN calc: invalid step output pointer.\n");
        return 3;
    }

    long step = 0;
    if (cfg && cfg->dmr_t3_step_hz_is_set && cfg->dmr_t3_step_hz > 0) {
        step = cfg->dmr_t3_step_hz;
    } else {
        step = infer_step_125(freqs, count);
    }

    if (step <= 0) {
        LOG_ERROR("LCN calc: could not infer channel step. Provide DSD_NEO_DMR_T3_STEP_HZ.\n");
        return 3;
    }
    *step_out = step;
    return 0;
}

static int
resolve_base_lcn_with_anchor(const dsdneoRuntimeConfig* cfg, long base_freq, long step, long* base_lcn_out) {
    if (!base_lcn_out) {
        LOG_ERROR("LCN calc: invalid base_lcn output pointer.\n");
        return 3;
    }
    if (step <= 0) {
        LOG_ERROR("LCN calc: invalid step (<=0) during anchor alignment.\n");
        return 3;
    }

    long base_lcn = dmr_t3_start_lcn_or_default(cfg);
    long cc_freq = (cfg && cfg->dmr_t3_cc_freq_is_set) ? cfg->dmr_t3_cc_freq_hz : 0;
    long cc_lcn = (cfg && cfg->dmr_t3_cc_lcn_is_set) ? cfg->dmr_t3_cc_lcn : 0;

    if (cc_freq > 0 && cc_lcn > 0) {
        long delta = cc_freq - base_freq;
        long idx = (long)llround((double)delta / (double)step);
        base_lcn = cc_lcn - idx;
    }

    *base_lcn_out = base_lcn;
    return 0;
}

// --- Public API --------------------------------------------------------------

int
dsd_cli_calc_dmr_t3_lcn_from_csv(const char* path) {
    const dsdneoRuntimeConfig* cfg = resolve_runtime_config();

    FILE* fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("LCN calc: unable to open '%s'\n", path);
        return 1;
    }
    long freqs[4096];
    int nf = read_frequency_rows(fp, freqs, (int)(sizeof(freqs) / sizeof(freqs[0])));
    fclose(fp);

    if (nf < 1) {
        LOG_ERROR("LCN calc: no frequencies parsed from '%s'\n", path);
        return 2;
    }

    qsort(freqs, nf, sizeof(long), cmp_long);
    nf = dedupe_sorted_frequencies(freqs, nf);

    if (nf == 1) {
        printf("lcn,freq\n%ld,%ld\n", dmr_t3_start_lcn_or_default(cfg), freqs[0]);
        return 0;
    }

    long step = 0;
    if (resolve_channel_step_hz(cfg, freqs, nf, &step) != 0) {
        return 3;
    }
    long base_freq = freqs[0];
    long base_lcn = 0;
    if (resolve_base_lcn_with_anchor(cfg, base_freq, step, &base_lcn) != 0) {
        return 3;
    }
    return emit_lcn_frequency_rows(freqs, nf, base_freq, base_lcn, step);
}
