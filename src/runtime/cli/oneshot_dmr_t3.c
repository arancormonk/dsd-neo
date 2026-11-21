// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief DMR TIII LCN calculator one-shot utility.
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/path_policy.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    errno = 0;
    double val = strtod(start, &end);
    if (end == start || errno != 0) {
        return 0;
    }

    /* Values under 1e5 are MHz. Bounded before rounding: llround() of a value past long long's range is unspecified,
       and the result becomes a trunk frequency, so it must also fit DSD_TRUNK_FREQ_MAX_HZ. */
    const double hz = (val < 1e5) ? val * 1000000.0 : val;
    if (!(hz >= 0.5 && hz <= (double)DSD_TRUNK_FREQ_MAX_HZ)) {
        return 0;
    }

    *hz_out = (long)llround(hz);
    return 1;
}

/* Returns the row count, -1 when the file holds more usable rows than @p capacity (a silent cut would shift the
   sorted map), or -2 when a line is longer than the read buffer (its fragments would parse as extra rows); the
   1-based line number is stored in @p bad_line. */
static int
read_frequency_rows(FILE* fp, long* freqs, int capacity, int* bad_line) {
    if (!fp || !freqs || capacity <= 0) {
        return 0;
    }

    int count = 0;
    char line[1024];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        if (!strchr(line, '\n') && getc(fp) != EOF) {
            *bad_line = line_no;
            return -2;
        }
        long hz = 0;
        if (!line_parse_frequency_hz(line, &hz)) {
            continue;
        }
        if (count >= capacity) {
            return -1;
        }
        freqs[count++] = hz;
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

/* LCN offset of @p freq from the reference, on the channel grid. Fails when the frequency is off the grid. */
static int
grid_index(long long freq, long long ref, long long step, long long* idx_out) {
    const long long delta = freq - ref;
    if (delta == 0) {
        *idx_out = 0;
        return 0;
    }
    if (step <= 0 || delta % step != 0) {
        return -1;
    }
    *idx_out = delta / step;
    return 0;
}

/* Computes every LCN before anything is printed. Sorted unique inputs on one grid give strictly rising LCNs, so the
   range check also rules out duplicates; the compare below keeps that explicit. */
static int
compute_lcns(const long* freqs, int count, long long ref_freq, long long ref_lcn, long long step, long long* lcns) {
    for (int i = 0; i < count; i++) {
        long long idx = 0;
        if (grid_index(freqs[i], ref_freq, step, &idx) != 0) {
            LOG_ERROR("LCN calc: %ld Hz is not on the channel grid (step %lld Hz from %lld Hz).\n", freqs[i], step,
                      ref_freq);
            return 3;
        }
        const long long lcn = ref_lcn + idx;
        if (lcn < 1 || lcn > 4094) {
            LOG_ERROR("LCN calc: %ld Hz maps to LCN %lld, outside 1..4094.\n", freqs[i], lcn);
            return 3;
        }
        if (i > 0 && lcn <= lcns[i - 1]) {
            LOG_ERROR("LCN calc: %ld Hz maps to a duplicate LCN %lld.\n", freqs[i], lcn);
            return 3;
        }
        lcns[i] = lcn;
    }
    return 0;
}

static const dsdneoRuntimeConfig*
resolve_runtime_config(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (cfg) {
        return cfg;
    }
    dsd_neo_config_init();
    return dsd_neo_get_config();
}

static long
dmr_t3_start_lcn_or_default(const dsdneoRuntimeConfig* cfg) {
    return (cfg && cfg->dmr_t3_start_lcn > 0) ? cfg->dmr_t3_start_lcn : 1;
}

/* The configured step, else the one inferred from two or more inputs. A single input needs none unless it must be
   placed away from the anchor, which the grid check reports. */
static int
resolve_step_hz(const dsdneoRuntimeConfig* cfg, const long* freqs, int count, long* step_out) {
    if (cfg && cfg->dmr_t3_step_hz_is_set && cfg->dmr_t3_step_hz > 0) {
        *step_out = cfg->dmr_t3_step_hz;
    } else if (count > 1) {
        *step_out = infer_step_125(freqs, count);
        if (*step_out <= 0) {
            LOG_ERROR("LCN calc: could not infer channel step. Provide DSD_NEO_DMR_T3_STEP_HZ.\n");
            return 3;
        }
    }
    return 0;
}

/* The anchor is both settings or neither: half of one cannot place the grid. */
static int
apply_anchor(const dsdneoRuntimeConfig* cfg, long long* ref_freq, long long* ref_lcn) {
    if (!cfg) {
        return 0;
    }
    if (cfg->dmr_t3_cc_freq_is_set != cfg->dmr_t3_cc_lcn_is_set) {
        LOG_ERROR("LCN calc: set both DSD_NEO_DMR_T3_CC_FREQ and DSD_NEO_DMR_T3_CC_LCN, or neither.\n");
        return 3;
    }
    if (cfg->dmr_t3_cc_freq_is_set) {
        *ref_freq = cfg->dmr_t3_cc_freq_hz;
        *ref_lcn = cfg->dmr_t3_cc_lcn;
    }
    return 0;
}

// --- Public API --------------------------------------------------------------

int
dsd_cli_calc_dmr_t3_lcn_from_csv(const char* path) {
    const dsdneoRuntimeConfig* cfg = resolve_runtime_config();

    char opened_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, opened_path, sizeof opened_path);
    if (!fp) {
        LOG_ERROR("LCN calc: unable to open '%s'\n", path);
        return 1;
    }
    long freqs[4096];
    long long lcns[4096];
    int bad_line = 0;
    int nf = read_frequency_rows(fp, freqs, (int)(sizeof(freqs) / sizeof(freqs[0])), &bad_line);
    fclose(fp);

    if (nf == -2) {
        LOG_ERROR("LCN calc: row %d of '%s' is longer than 1023 bytes.\n", bad_line, path);
        return 3;
    }

    if (nf < 0) {
        LOG_ERROR("LCN calc: '%s' has more than %d frequency rows.\n", path, (int)(sizeof(freqs) / sizeof(freqs[0])));
        return 3;
    }
    if (nf < 1) {
        LOG_ERROR("LCN calc: no frequencies parsed from '%s'\n", path);
        return 2;
    }

    qsort(freqs, nf, sizeof(long), cmp_long);
    nf = dedupe_sorted_frequencies(freqs, nf);

    long step = 0;
    if (resolve_step_hz(cfg, freqs, nf, &step) != 0) {
        return 3;
    }

    long long ref_freq = freqs[0];
    long long ref_lcn = dmr_t3_start_lcn_or_default(cfg);
    if (apply_anchor(cfg, &ref_freq, &ref_lcn) != 0) {
        return 3;
    }

    const int rc = compute_lcns(freqs, nf, ref_freq, ref_lcn, step, lcns);
    if (rc != 0) {
        return rc;
    }
    printf("lcn,freq\n");
    for (int i = 0; i < nf; i++) {
        printf("%lld,%ld\n", lcns[i], freqs[i]);
    }
    return 0;
}
