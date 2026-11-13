// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * p25p2_isch.c
 * P25 Phase 2 I-ISCH codeword table, moved from ez.cpp
 *
 * original copyrights for portions used below (OP25, EZPWD)
 *
 * LWVMOBILE
 * 2022-09 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

/* The P25 Phase 2 ISCH code (<dsd-neo/fec/p25p2_isch.h>). The codewords are the table OP25 decodes I-ISCHs with. */

#include <dsd-neo/fec/p25p2_isch.h>
#include <dsd-neo/platform/posix_compat.h>
#include <stddef.h>
#include <stdint.h>

const uint64_t dsd_p25p2_i_isch_codewords[DSD_P25P2_I_ISCH_CODEWORDS] = {
    0x184229d461ULL, 0x18761451f6ULL, 0x181ae27e2fULL, 0x182edffbb8ULL, 0x18df8a7510ULL, 0x18ebb7f087ULL,
    0x188741df5eULL, 0x18b37c5ac9ULL, 0x1146a44f13ULL, 0x117299ca84ULL, 0x111e6fe55dULL, 0x112a5260caULL,
    0x11db07ee62ULL, 0x11ef3a6bf5ULL, 0x1183cc442cULL, 0x11b7f1c1bbULL, 0x1a4a2e239eULL, 0x1a7e13a609ULL,
    0x1a12e589d0ULL, 0x1a26d80c47ULL, 0x1ad78d82efULL, 0x1ae3b00778ULL, 0x1a8f4628a1ULL, 0x1abb7bad36ULL,
    0x134ea3b8ecULL, 0x137a9e3d7bULL, 0x13166812a2ULL, 0x1322559735ULL, 0x13d300199dULL, 0x13e73d9c0aULL,
    0x138bcbb3d3ULL, 0x13bff63644ULL, 0x1442f705efULL, 0x1476ca8078ULL, 0x141a3cafa1ULL, 0x142e012a36ULL,
    0x14df54a49eULL, 0x14eb692109ULL, 0x14879f0ed0ULL, 0x14b3a28b47ULL, 0x1d467a9e9dULL, 0x1d72471b0aULL,
    0x1d1eb134d3ULL, 0x1d2a8cb144ULL, 0x1ddbd93fecULL, 0x1defe4ba7bULL, 0x1d831295a2ULL, 0x1db72f1035ULL,
    0x164af0f210ULL, 0x167ecd7787ULL, 0x16123b585eULL, 0x162606ddc9ULL, 0x16d7535361ULL, 0x16e36ed6f6ULL,
    0x168f98f92fULL, 0x16bba57cb8ULL, 0x1f4e7d6962ULL, 0x1f7a40ecf5ULL, 0x1f16b6c32cULL, 0x1f228b46bbULL,
    0x1fd3dec813ULL, 0x1fe7e34d84ULL, 0x1f8b15625dULL, 0x1fbf28e7caULL, 0x084d62c339ULL, 0x08795f46aeULL,
    0x0815a96977ULL, 0x082194ece0ULL, 0x08d0c16248ULL, 0x08e4fce7dfULL, 0x08880ac806ULL, 0x08bc374d91ULL,
    0x0149ef584bULL, 0x017dd2dddcULL, 0x011124f205ULL, 0x0125197792ULL, 0x01d44cf93aULL, 0x01e0717cadULL,
    0x018c875374ULL, 0x01b8bad6e3ULL, 0x0a456534c6ULL, 0x0a7158b151ULL, 0x0a1dae9e88ULL, 0x0a29931b1fULL,
    0x0ad8c695b7ULL, 0x0aecfb1020ULL, 0x0a800d3ff9ULL, 0x0ab430ba6eULL, 0x0341e8afb4ULL, 0x0375d52a23ULL,
    0x03192305faULL, 0x032d1e806dULL, 0x03dc4b0ec5ULL, 0x03e8768b52ULL, 0x038480a48bULL, 0x03b0bd211cULL,
    0x044dbc12b7ULL, 0x0479819720ULL, 0x041577b8f9ULL, 0x04214a3d6eULL, 0x04d01fb3c6ULL, 0x04e4223651ULL,
    0x0488d41988ULL, 0x04bce99c1fULL, 0x0d493189c5ULL, 0x0d7d0c0c52ULL, 0x0d11fa238bULL, 0x0d25c7a61cULL,
    0x0dd49228b4ULL, 0x0de0afad23ULL, 0x0d8c5982faULL, 0x0db864076dULL, 0x0645bbe548ULL, 0x06718660dfULL,
    0x061d704f06ULL, 0x06294dca91ULL, 0x06d8184439ULL, 0x06ec25c1aeULL, 0x0680d3ee77ULL, 0x06b4ee6be0ULL,
    0x0f41367e3aULL, 0x0f750bfbadULL, 0x0f19fdd474ULL, 0x0f2dc051e3ULL, 0x0fdc95df4bULL, 0x0fe8a85adcULL,
    0x0f845e7505ULL, 0x0fb063f092ULL,
};

int
dsd_p25p2_isch_nearest(uint64_t word, int* out_bits) {
    word &= 0xFFFFFFFFFFULL;
    int nearest = DSD_P25P2_ISCH_NEAREST_S_ISCH;
    int bits = dsd_popcount64(word ^ DSD_P25P2_S_ISCH);
    for (int i = 0; i < DSD_P25P2_I_ISCH_CODEWORDS; i++) {
        const int d = dsd_popcount64(word ^ dsd_p25p2_i_isch_codewords[i]);
        if (d < bits) {
            bits = d;
            nearest = i;
        }
    }
    if (out_bits != NULL) {
        *out_bits = bits;
    }
    return nearest;
}
