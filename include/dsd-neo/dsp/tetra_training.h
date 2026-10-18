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
    const int d = dibit & 3;
    return (((d & 1) << 1) | (d >> 1)) ^ 1;
}

static inline int
tetra_dibit_from_symbol(int symbol) {
    /* ASCII '0'..'3' have the same low bits as numeric dibits. */
    return symbol & 3;
}

/* Map a sliced dibit back to the training-sequence phase. `rot` is how many
 * 90° steps the matcher needed. */
static inline int
tetra_dibit_derotate(int symbol, int rot) {
    int d = tetra_dibit_from_symbol(symbol);
    rot &= 3;
    if (rot & 1) {
        d = ((d & 1) << 1) | (d >> 1);
    }
    return d ^ (rot ^ (rot >> 1));
}

/* Compare the remaining dibits after the caller has matched the first one. */
static inline int
tetra_training_matches(const char* obs, const char* pat, int len, int rot) {
    int mismatch = 0;
    for (int i = 1; i < len; i++) {
        mismatch |= tetra_dibit_derotate((unsigned char)obs[i], rot) ^ (pat[i] - '0');
    }
    return mismatch == 0;
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
        /* The first dibit identifies at most one rotation for each word. */
        const int first = tetra_dibit_derotate((unsigned char)obs[0], rot);
        const int normal = first == pat[0] - '0' && tetra_training_matches(obs, pat, len, rot);
        const int inverted = pat_inv && first == pat_inv[0] - '0' && tetra_training_matches(obs, pat_inv, len, rot);
        if (normal || inverted) {
            if (out_rot) {
                *out_rot = rot;
            }
            if (out_inverted) {
                *out_inverted = normal ? 0 : 1;
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
