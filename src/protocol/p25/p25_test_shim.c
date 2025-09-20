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

// Decode a single MBT PDU with pre-seeded IDEN parameters and report key fields.
// - iden/type/tdma/spac/base configure the channel table used by the frequency calculator.
// - Returns 0 on success and fills out_cc (control channel Hz), out_wacn (20-bit), out_sysid (12-bit).
int
p25_test_decode_mbt_with_iden(const unsigned char* mbt, int mbt_len, int iden, int type, int tdma, long base, int spac,
                              long* out_cc, long* out_wacn, int* out_sysid) {
    (void)mbt_len;

    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    if (iden < 0 || iden > 15) {
        return -2;
    }
    state.p25_chan_iden = iden & 0xF;
    state.p25_chan_type[iden] = type & 0xF;
    state.p25_chan_tdma[iden] = tdma & 0x1;
    state.p25_chan_spac[iden] = spac;
    state.p25_base_freq[iden] = base;

    p25_decode_pdu_trunking(&opts, &state, (unsigned char*)mbt);

    if (out_cc) {
        *out_cc = state.p25_cc_freq;
    }
    if (out_wacn) {
        *out_wacn = (long)state.p2_wacn;
    }
    if (out_sysid) {
        *out_sysid = state.p2_sysid;
    }
    return 0;
}

/* (intentionally empty) */

// Lightweight wrapper to invoke the Phase 2 MAC VPDU handler from tests.
// Accepts a byte-oriented MAC buffer (up to 24 bytes) and channel type
// (0=FACCH, 1=SACCH). Emits JSON to stderr when DSD_NEO_PDU_JSON=1.
void
p25_test_process_mac_vpdu(int type, const unsigned char* mac_bytes, int mac_len) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    unsigned long long int MAC[24] = {0};
    int n = mac_len < 24 ? mac_len : 24;
    for (int i = 0; i < n; i++) {
        MAC[i] = mac_bytes[i];
    }

    // Let the VPDU handler compute lengths and optionally emit JSON
    process_MAC_VPDU(&opts, &state, type, MAC);
}
