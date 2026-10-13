// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA training-sequence matching for the frame sync, shared with the TETRA decoder.
 *
 * TETRA bursts carry their training sequence mid-burst. The frame sync matches it on the sliced dibits after
 * undoing up to three 90 degree steps of the CQPSK slicer, and the decoder applies the same derotation to the
 * payload dibits it captured.
 */

#ifndef DSD_NEO_DSP_TETRA_TRAINING_H
#define DSD_NEO_DSP_TETRA_TRAINING_H

#ifdef __cplusplus
extern "C" {
#endif

/* 90° constellation step used by the CQPSK slicer: 0→1→3→2→0.
 * A Costas loop on a bursty π/4-DQPSK carrier parks on any of the four
 * phases, so the training word is compared after 0..3 of these steps. */
static inline int
tetra_dibit_rotate90(int dibit) {
    switch (dibit & 3) {
        case 0: return 1;
        case 1: return 3;
        case 2: return 0;
        default: return 2;
    }
}

static inline int
tetra_dibit_from_symbol(int symbol) {
    if (symbol >= '0' && symbol <= '3') {
        return symbol - '0';
    }
    return symbol & 3;
}

/* Map a sliced dibit back to the training-sequence phase. `rot` is how many
 * 90° steps the matcher needed. */
static inline int
tetra_dibit_derotate(int symbol, int rot) {
    int d = tetra_dibit_from_symbol(symbol);
    for (int i = 0; i < (rot & 3); i++) {
        d = tetra_dibit_rotate90(d);
    }
    return d;
}

/* Exact match of a training word after a 0/90/180/270 derotation.
 * Returns 1 and writes the rotation and whether the inverted word matched. */
static inline int
tetra_training_derotation(const char* obs, const char* pat, const char* pat_inv, int len, int* out_rot,
                          int* out_inverted) {
    if (!obs || !pat || len <= 0) {
        return 0;
    }
    for (int rot = 0; rot < 4; rot++) {
        int mis = 0;
        int mis_inv = pat_inv ? 0 : len + 1;
        for (int i = 0; i < len; i++) {
            int d = tetra_dibit_derotate((unsigned char)obs[i], rot);
            if (d != (pat[i] - '0')) {
                mis++;
            }
            if (pat_inv && d != (pat_inv[i] - '0')) {
                mis_inv++;
            }
        }
        if (mis == 0 || mis_inv == 0) {
            if (out_rot) {
                *out_rot = rot;
            }
            if (out_inverted) {
                *out_inverted = (mis == 0) ? 0 : 1;
            }
            return 1;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_DSP_TETRA_TRAINING_H */
