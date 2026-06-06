// SPDX-License-Identifier: ISC
#ifndef DSD_NEO_SRC_PROTOCOL_PROVOICE_PROVOICE_FRAME_H_
#define DSD_NEO_SRC_PROTOCOL_PROVOICE_PROVOICE_FRAME_H_

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_PROVOICE_IMBE_ROWS = 7,
    DSD_PROVOICE_IMBE_COLS = 24,
    DSD_PROVOICE_IMBE_FRAME_BYTES = DSD_PROVOICE_IMBE_ROWS * DSD_PROVOICE_IMBE_COLS,
    DSD_PROVOICE_FRAME_PAIR_DIBITS = 286,
};

typedef int (*dsd_provoice_next_dibit_fn)(void* user, int* out_dibit);
typedef char dsd_provoice_imbe_frame[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS];

int dsd_provoice_load_imbe_frame_pair_impl(void* user, dsd_provoice_imbe_frame frame1, dsd_provoice_imbe_frame frame2,
                                           dsd_provoice_next_dibit_fn next_dibit);

#define dsd_provoice_load_imbe_frame_pair(next_dibit, user, frame1, frame2)                                            \
    dsd_provoice_load_imbe_frame_pair_impl((user), (frame1), (frame2), (next_dibit))

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_PROVOICE_PROVOICE_FRAME_H_ */
