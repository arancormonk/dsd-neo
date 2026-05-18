// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/symbol_levels.h>

#include <math.h>

float
dsd_symbol_level_from_dibit(uint8_t dibit) {
    switch (dibit & 0x3u) {
        case 0: return 1.0f;  // +1
        case 1: return 3.0f;  // +3
        case 2: return -1.0f; // -1
        case 3: return -3.0f; // -3
        default: break;
    }
    return 0.0f;
}

static int
fsk_clamp_levels(int levels) {
    return (levels == 2) ? 2 : 4;
}

static float
fsk_nearest_ideal(float symbol, int levels) {
    if (levels == 2) {
        return (symbol >= 0.0f) ? 1.0f : -1.0f;
    }
    if (symbol >= 2.0f) {
        return 3.0f;
    }
    if (symbol >= 0.0f) {
        return 1.0f;
    }
    if (symbol >= -2.0f) {
        return -1.0f;
    }
    return -3.0f;
}

dsd_fsk_soft_symbol_metrics
dsd_fsk_soft_symbol_metrics_from_symbol(float symbol, int levels) {
    dsd_fsk_soft_symbol_metrics out;
    out.levels = fsk_clamp_levels(levels);
    out.symbol = symbol;
    out.ideal = fsk_nearest_ideal(symbol, out.levels);
    out.error = fabsf(symbol - out.ideal);
    float rel_error = out.error;
    if (rel_error > 1.0f) {
        rel_error = 1.0f;
    }
    int rel = (int)((1.0f - rel_error) * 255.0f + 0.5f);
    if (rel < 0) {
        rel = 0;
    } else if (rel > 255) {
        rel = 255;
    }
    out.reliability = (uint8_t)rel;
    out.clipped = (fabsf(symbol) >= ((out.levels == 2) ? 2.0f : 4.0f)) ? 1 : 0;
    return out;
}

uint8_t
dsd_fsk_symbol_reliability(float symbol, int levels) {
    return dsd_fsk_soft_symbol_metrics_from_symbol(symbol, levels).reliability;
}
