// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "rtl_tuner_gains.h"

#include <stddef.h>

/* Tenths of a dB, ascending, copied from osmocom rtl-sdr src/librtlsdr.c rtlsdr_get_tuner_gains() at commit
 * 797f8143266d983c56d8f35d2d442527529dd8a5. rtl_tcp's header carries that function's count for the dongle's tuner. */
static const int kE4000Gains[] = {-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420};
static const int kFc0012Gains[] = {-99, -40, 71, 179, 192};
static const int kFc0013Gains[] = {-99, -73, -65, -63, -60, -58, -54, 58,  61,  63,  65, 67,
                                   68,  70,  71,  179, 181, 182, 184, 186, 188, 191, 197};
static const int kFc2580Gains[] = {0};
static const int kR82xxGains[] = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                                  280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};

#define RTL_TUNER_GAINS_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* librtlsdr's enum rtlsdr_tuner values, as the rtl_tcp header sends them. */
enum {
    RTL_TUNER_GAINS_E4000 = 1,
    RTL_TUNER_GAINS_FC0012 = 2,
    RTL_TUNER_GAINS_FC0013 = 3,
    RTL_TUNER_GAINS_FC2580 = 4,
    RTL_TUNER_GAINS_R820T = 5,
    RTL_TUNER_GAINS_R828D = 6,
};

/* The table of @p tuner_type, its size in @p out_len; NULL for a tuner librtlsdr does not know. */
static const int*
rtl_tuner_gain_table(uint32_t tuner_type, size_t* out_len) {
    switch (tuner_type) {
        case RTL_TUNER_GAINS_E4000: *out_len = RTL_TUNER_GAINS_LEN(kE4000Gains); return kE4000Gains;
        case RTL_TUNER_GAINS_FC0012: *out_len = RTL_TUNER_GAINS_LEN(kFc0012Gains); return kFc0012Gains;
        case RTL_TUNER_GAINS_FC0013: *out_len = RTL_TUNER_GAINS_LEN(kFc0013Gains); return kFc0013Gains;
        case RTL_TUNER_GAINS_FC2580: *out_len = RTL_TUNER_GAINS_LEN(kFc2580Gains); return kFc2580Gains;
        case RTL_TUNER_GAINS_R820T:
        case RTL_TUNER_GAINS_R828D: *out_len = RTL_TUNER_GAINS_LEN(kR82xxGains); return kR82xxGains;
        default: *out_len = 0U; return NULL;
    }
}

static long long
rtl_tuner_gain_distance(int request, int step) {
    const long long d = (long long)request - (long long)step;
    return (d < 0) ? -d : d;
}

int
dsd_rtl_tuner_gain_nearest(uint32_t tuner_type, uint32_t header_gain_count, int request_tenth_db,
                           int* out_selected_tenth_db) {
    size_t len = 0U;
    const int* table = rtl_tuner_gain_table(tuner_type, &len);
    if (!table || !out_selected_tenth_db || (size_t)header_gain_count != len) {
        return 0;
    }
    int nearest = table[0];
    for (size_t i = 1U; i < len; i++) {
        /* Strictly closer only: of two steps as near, the lower (earlier) one stays. */
        if (rtl_tuner_gain_distance(request_tenth_db, table[i]) < rtl_tuner_gain_distance(request_tenth_db, nearest)) {
            nearest = table[i];
        }
    }
    *out_selected_tenth_db = nearest;
    return 1;
}
