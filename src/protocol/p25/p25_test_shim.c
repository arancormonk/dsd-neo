// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Lightweight test shim to exercise internal P25 functions without exposing
 * full dsd.h to unit tests that lack external deps (e.g., mbelib).
 */

#include <string.h>

#include <dsd-neo/core/dsd.h>

// Invoke the P25p1 MBT -> MAC Identifier Update bridge and report key state.
// Returns 0 on success.
int
p25_test_mbt_iden_bridge(const unsigned char* mbt, int mbt_len, long* out_base, int* out_spac, int* out_type,
                         int* out_tdma, long* out_freq) {
    (void)mbt_len; // unused; decode inspects header fields only

    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    // Run the trunking PDU decoder on provided MBT bytes
    p25_decode_pdu_trunking(&opts, &state, (unsigned char*)mbt);

    int iden = state.p25_chan_iden & 0xF;
    if (iden < 0 || iden > 15) {
        return -1;
    }

    if (out_type) {
        *out_type = state.p25_chan_type[iden] & 0xF;
    }
    if (out_tdma) {
        *out_tdma = state.p25_chan_tdma[iden] & 0x1;
    }
    if (out_spac) {
        *out_spac = state.p25_chan_spac[iden];
    }
    if (out_base) {
        *out_base = state.p25_base_freq[iden];
    }
    if (out_freq) {
        // Compute a simple test channel (channel number 10 on selected iden)
        int channel = (iden << 12) | 10;
        *out_freq = process_channel_to_freq(&opts, &state, channel);
    }
    return 0;
}
