// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#include <string.h>
#include "x2tdma_frame.h"

static void
test_sync_classification_includes_mobile_data(void) {
    assert(dsd_x2tdma_sync_is_data(X2TDMA_BS_DATA_SYNC));
    assert(dsd_x2tdma_sync_is_data(X2TDMA_MS_DATA_SYNC));
    assert(!dsd_x2tdma_sync_is_data(X2TDMA_BS_VOICE_SYNC));
    assert(!dsd_x2tdma_sync_is_data(NULL));

    assert(dsd_x2tdma_sync_is_voice(X2TDMA_BS_VOICE_SYNC));
    assert(dsd_x2tdma_sync_is_voice(X2TDMA_MS_VOICE_SYNC));
    assert(!dsd_x2tdma_sync_is_voice(X2TDMA_MS_DATA_SYNC));
    assert(!dsd_x2tdma_sync_is_voice(NULL));
}

static void
test_mi_placeholder_is_terminated(void) {
    char mi[DSD_X2TDMA_MI_TEXT_LEN];
    dsd_x2tdma_init_mi_placeholder(mi);

    for (int i = 0; i < DSD_X2TDMA_MI_BITS; i++) {
        assert(mi[i] == '_');
    }
    assert(mi[DSD_X2TDMA_MI_BITS] == '\0');
    assert(strlen(mi) == DSD_X2TDMA_MI_BITS);

    dsd_x2tdma_init_mi_placeholder(NULL);
}

static void
test_ambe_interleave_schedule_shape(void) {
    enum { X2TDMA_AMBE_DIBITS = 36 };

    int seen[4][24] = {{0}};

    _Static_assert((int)(sizeof x2tdma_ambe_interleave_w / sizeof x2tdma_ambe_interleave_w[0]) == X2TDMA_AMBE_DIBITS,
                   "x2tdma_ambe_interleave_w length");
    _Static_assert((int)(sizeof x2tdma_ambe_interleave_x / sizeof x2tdma_ambe_interleave_x[0]) == X2TDMA_AMBE_DIBITS,
                   "x2tdma_ambe_interleave_x length");
    _Static_assert((int)(sizeof x2tdma_ambe_interleave_y / sizeof x2tdma_ambe_interleave_y[0]) == X2TDMA_AMBE_DIBITS,
                   "x2tdma_ambe_interleave_y length");
    _Static_assert((int)(sizeof x2tdma_ambe_interleave_z / sizeof x2tdma_ambe_interleave_z[0]) == X2TDMA_AMBE_DIBITS,
                   "x2tdma_ambe_interleave_z length");

    for (int i = 0; i < X2TDMA_AMBE_DIBITS; i++) {
        const int row_a = x2tdma_ambe_interleave_w[i];
        const int col_a = x2tdma_ambe_interleave_x[i];
        const int row_b = x2tdma_ambe_interleave_y[i];
        const int col_b = x2tdma_ambe_interleave_z[i];

        assert(row_a >= 0 && row_a < 4);
        assert(col_a >= 0 && col_a < 24);
        assert(row_b >= 0 && row_b < 4);
        assert(col_b >= 0 && col_b < 24);
        assert(!seen[row_a][col_a]);
        seen[row_a][col_a] = 1;
        assert(!seen[row_b][col_b]);
        seen[row_b][col_b] = 1;
    }

    assert(x2tdma_ambe_interleave_w[0] == 0);
    assert(x2tdma_ambe_interleave_x[0] == 23);
    assert(x2tdma_ambe_interleave_y[0] == 0);
    assert(x2tdma_ambe_interleave_z[0] == 5);
    assert(x2tdma_ambe_interleave_w[23] == 2);
    assert(x2tdma_ambe_interleave_x[23] == 10);
    assert(x2tdma_ambe_interleave_y[23] == 3);
    assert(x2tdma_ambe_interleave_z[23] == 6);
    assert(x2tdma_ambe_interleave_w[35] == 2);
    assert(x2tdma_ambe_interleave_x[35] == 4);
    assert(x2tdma_ambe_interleave_y[35] == 3);
    assert(x2tdma_ambe_interleave_z[35] == 0);
}

int
main(void) {
    test_sync_classification_includes_mobile_data();
    test_mi_placeholder_is_terminated();
    test_ambe_interleave_schedule_shape();
    return 0;
}
