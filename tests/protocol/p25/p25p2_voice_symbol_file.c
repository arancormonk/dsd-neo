// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Generator for tests/p25p2_float_symbol_ambe_check.cmake: a P25 Phase 2 voice channel as a float symbol file
 * (.raw), the symbols at their nominal levels +-1 and +-3 with a little deterministic noise.
 *
 *   dsd-neo_test_p25p2_voice_symbol_file
 *
 * Run in the test's work directory; writes the one fixed file symbols.raw. Every superframe carries two voice channels
 * (timeslots 0-9, each channel four 4V bursts and a 2V), then two SACCHs (10, 11), scrambled with the code
 * BEE00/164/161 (-X BEE00164161), the ISCHs as a site sends them (p25p2_burst_builder.c). The voice bits are
 * pseudo-random: their AMBE frames are no speech, and Golay decoding finds errors in most, which only reliabilities
 * the slicer measured can count.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"
#include "p25p2_burst_builder.h"

enum {
    SUPERFRAMES = 8,
    LEAD_SYMBOLS = 360,
    WINDOW_BITS = 1440,
    TIMESLOT_BITS = 360,
    BURST_BITS = 320,
};

static const uint64_t k_code = (0xBEE00ULL << 24) | (0x164ULL << 12) | 0x161ULL;
static const int k_duid_offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};

static uint32_t g_seed = 0x2545F491u;

static uint32_t
next_random(void) {
    g_seed = (g_seed * 1103515245u) + 12345u;
    return (g_seed >> 16) & 0x7FFFu;
}

static int
is_duid_bit(int b) {
    for (int i = 0; i < 8; i++) {
        if (k_duid_offsets[i] == b) {
            return 1;
        }
    }
    return 0;
}

/* The timeslot's data unit: 4V (0) in voice pairs 0-3, 2V (6) in pair 4, an unscrambled SACCH (12) in pair 5. */
static int
duid_for_timeslot(int timeslot) {
    if (timeslot >= 10) {
        return 12;
    }
    return timeslot >= 8 ? 6 : 0;
}

/* A level of +-1/+-3 for the dibit, as the CQPSK slicer reads it (cqpsk_slice()), with noise of up to +-0.15. */
static float
symbol_for_dibit(int dibit) {
    static const float levels[4] = {1.0f, 3.0f, -1.0f, -3.0f};
    const float noise = ((float)(int)(next_random() % 31u) - 15.0f) * 0.01f;
    return levels[dibit & 3] + noise;
}

/* The float symbol file and, of the same dibits, a soft symbol capture at full confidence. */
typedef struct {
    FILE* raw;
    FILE* capture;
} outputs;

static int
write_symbol(const outputs* out, int dibit) {
    const float symbol = symbol_for_dibit(dibit);
    unsigned char record[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    const int16_t msb = (dibit & 2) ? 255 : -255;
    const int16_t lsb = (dibit & 1) ? 255 : -255;
    record[0] = (unsigned char)(dibit & 3);
    record[1] = 255U;
    record[2] = (unsigned char)((uint16_t)msb & 0xFFU);
    record[3] = (unsigned char)(((uint16_t)msb >> 8) & 0xFFU);
    record[4] = (unsigned char)((uint16_t)lsb & 0xFFU);
    record[5] = (unsigned char)(((uint16_t)lsb >> 8) & 0xFFU);
    uint32_t bits = 0;
    DSD_MEMCPY(&bits, &symbol, sizeof(bits));
    for (int i = 0; i < 4; i++) {
        record[6 + i] = (unsigned char)((bits >> (8 * i)) & 0xFFU);
    }
    const int raw_ok = fwrite(&symbol, sizeof(symbol), 1, out->raw) == 1;
    const int capture_ok = fwrite(record, 1, sizeof(record), out->capture) == sizeof(record);
    return raw_ok && capture_ok ? 0 : 1;
}

static int
write_window(const outputs* out, int t0) {
    p25p2_bb_slot slots[4];
    DSD_MEMSET(slots, 0, sizeof(slots));
    for (int ts = 0; ts < 4; ts++) {
        slots[ts].duid = duid_for_timeslot(t0 + ts);
        slots[ts].fill_crc = 1;
    }
    uint8_t bits[WINDOW_BITS];
    p25p2_bb_window(k_code, t0, slots, bits);
    for (int ts = 0; ts < 4; ts++) {
        if (slots[ts].duid == 12) {
            continue;
        }
        uint8_t* burst = bits + ((ptrdiff_t)ts * TIMESLOT_BITS);
        for (int b = 0; b < BURST_BITS; b++) {
            if (!is_duid_bit(b)) {
                burst[b] ^= (uint8_t)(next_random() & 1u);
            }
        }
    }
    int rc = 0;
    for (int i = 0; i < WINDOW_BITS; i += 2) {
        rc |= write_symbol(out, (bits[i] << 1) | bits[i + 1]);
    }
    return rc;
}

int
main(void) {
    outputs out;
    out.raw = dsd_fopen_private("symbols.raw", "wb");
    out.capture = dsd_fopen_private("capture.bin", "wb");
    if (!out.raw || !out.capture) {
        DSD_FPRINTF(stderr, "p25p2 voice symbol file: cannot create symbols.raw and capture.bin\n");
        if (out.raw) {
            (void)fclose(out.raw);
        }
        if (out.capture) {
            (void)fclose(out.capture);
        }
        return 1;
    }
    unsigned char header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE] = {0};
    DSD_MEMCPY(header, DSD_SYMBOL_CAPTURE_SOFT_MAGIC, 8);
    header[8] = 2U;
    header[9] = DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE;
    int rc = fwrite(header, 1, sizeof(header), out.capture) == sizeof(header) ? 0 : 1;
    /* Random symbols ahead of the first superframe, for the level tracking to settle on. */
    for (int i = 0; i < LEAD_SYMBOLS; i++) {
        rc |= write_symbol(&out, (int)(next_random() & 3u));
    }
    for (int sf = 0; sf < SUPERFRAMES; sf++) {
        for (int t0 = 0; t0 < 12; t0 += 4) {
            rc |= write_window(&out, t0);
        }
    }
    const int closed_raw = fclose(out.raw);
    const int closed_capture = fclose(out.capture);
    if (closed_raw != 0 || closed_capture != 0 || rc != 0) {
        DSD_FPRINTF(stderr, "p25p2 voice symbol file: write error\n");
        return 1;
    }
    return 0;
}
