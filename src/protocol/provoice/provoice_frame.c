// SPDX-License-Identifier: ISC
#include "provoice_frame.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>

static int
provoice_load_interleave_segment(dsd_provoice_next_dibit_fn next_dibit, void* user,
                                 char frame[DSD_PROVOICE_IMBE_ROWS][DSD_PROVOICE_IMBE_COLS], const int** w,
                                 const int** x, int count) {
    int i;
    for (i = 0; i < count; i++) {
        int dibit = 0;
        if (next_dibit(user, &dibit) < 0) {
            return -1;
        }
        frame[**w][**x] = (char)dibit;
        (*w)++;
        (*x)++;
    }
    return count;
}

static int
provoice_skip_dibits(dsd_provoice_next_dibit_fn next_dibit, void* user, int count) {
    int i;
    for (i = 0; i < count; i++) {
        int ignored = 0;
        if (next_dibit(user, &ignored) < 0) {
            return -1;
        }
    }
    return count;
}

// Cppcheck 2.21 loses names after callback and adjusted-array typedef parameters in the matching declaration.
// cppcheck-suppress-begin funcArgNamesDifferentUnnamed
int
dsd_provoice_load_imbe_frame_pair(dsd_provoice_next_dibit_fn next_dibit, void* user_ctx, dsd_provoice_imbe_frame frame1,
                                  dsd_provoice_imbe_frame frame2) {
    int consumed = 0;
    int i;
    const int* w = provoice_interleave_w;
    const int* x = provoice_interleave_x;

    if (next_dibit == 0 || frame1 == 0 || frame2 == 0) {
        return -1;
    }

    DSD_MEMSET(frame1, 0, DSD_PROVOICE_IMBE_FRAME_BYTES);
    DSD_MEMSET(frame2, 0, DSD_PROVOICE_IMBE_FRAME_BYTES);

#define DSD_PROVOICE_LOAD_OR_FAIL(expr)                                                                                \
    do {                                                                                                               \
        int _loaded = (expr);                                                                                          \
        if (_loaded < 0) {                                                                                             \
            return -1;                                                                                                 \
        }                                                                                                              \
        consumed += _loaded;                                                                                           \
    } while (0)

    for (i = 0; i < 11; i++) {
        DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame1, &w, &x, 6));
        w -= 6;
        x -= 6;
        DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame2, &w, &x, 6));
    }

    DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame1, &w, &x, 6));
    w -= 6;
    x -= 6;
    DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame2, &w, &x, 4));

    DSD_PROVOICE_LOAD_OR_FAIL(provoice_skip_dibits(next_dibit, user_ctx, 2));
    DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame2, &w, &x, 2));

    for (i = 0; i < 3; i++) {
        DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame1, &w, &x, 6));
        w -= 6;
        x -= 6;
        DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame2, &w, &x, 6));
    }

    DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame1, &w, &x, 5));
    w -= 5;
    x -= 5;
    DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame2, &w, &x, 5));

    for (i = 0; i < 7; i++) {
        DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame1, &w, &x, 6));
        w -= 6;
        x -= 6;
        DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame2, &w, &x, 6));
    }

    DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame1, &w, &x, 5));
    w -= 5;
    x -= 5;
    DSD_PROVOICE_LOAD_OR_FAIL(provoice_load_interleave_segment(next_dibit, user_ctx, frame2, &w, &x, 5));

#undef DSD_PROVOICE_LOAD_OR_FAIL

    return consumed;
}

// cppcheck-suppress-end funcArgNamesDifferentUnnamed
