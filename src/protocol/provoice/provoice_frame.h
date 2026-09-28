// SPDX-License-Identifier: ISC
#ifndef DSD_NEO_SRC_PROTOCOL_PROVOICE_PROVOICE_FRAME_H_
#define DSD_NEO_SRC_PROTOCOL_PROVOICE_PROVOICE_FRAME_H_

#include <dsd-neo/core/vocoder.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_PROVOICE_IMBE_ROWS = 7,
    DSD_PROVOICE_IMBE_COLS = 24,
    DSD_PROVOICE_FRAME_PAIR_DIBITS = 286,
};

/* Reads the next two-level symbol: its hard bit (0 or 1) and the confidence in it (0..255). */
typedef int (*dsd_provoice_next_bit_fn)(void* user, int* out_bit, uint8_t* out_reliability);
/* An IMBE 7100x4400 frame as the soft decoder takes it. The 142 cells the interleave schedule reaches carry their
 * symbol's bit and reliability; the rest are bit 0 at reliability 0, which the decoder never reads. */
typedef dsd_vocoder_soft_bit dsd_provoice_imbe_frame[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS];

int dsd_provoice_load_imbe_frame_pair(dsd_provoice_next_bit_fn next_bit, void* user_ctx, dsd_provoice_imbe_frame frame1,
                                      dsd_provoice_imbe_frame frame2);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_PROVOICE_PROVOICE_FRAME_H_ */
