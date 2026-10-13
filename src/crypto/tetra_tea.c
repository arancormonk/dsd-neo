// SPDX-License-Identifier: Apache-2.0
#include <dsd-neo/crypto/tetra_tea.h>

#include <stdint.h>
#include <string.h>

#include <tetracrypto/common.h>
#include <tetracrypto/tea1.h>
#include <tetracrypto/tea2.h>
#include <tetracrypto/tea3.h>

void
tetra_tea_keystream(int alg, const uint8_t key[10], uint32_t iv, uint8_t* out, int nbytes) {
    uint8_t key_copy[10];
    if (!key || !out || nbytes <= 0) {
        return;
    }
    memcpy(key_copy, key, sizeof(key_copy));
    if (alg == 0) {
        tea1(iv, key_copy, (uint32_t)nbytes, out);
    } else if (alg == 1) {
        tea2(iv, key_copy, (uint32_t)nbytes, out);
    } else if (alg == 2) {
        tea3(iv, key_copy, (uint32_t)nbytes, out);
    }
}

uint32_t
tetra_tea_build_iv(uint8_t tn, uint8_t fn, uint8_t mn, uint16_t hn, uint8_t dir) {
    FrameNumbers frame;
    if (tn < 1 || tn > 4 || fn < 1 || fn > 18 || mn < 1 || mn > 60 || dir > 1) {
        return 0;
    }
    frame.tn = tn;
    frame.fn = fn;
    frame.mn = mn;
    frame.hn = hn;
    frame.dir = dir;
    return build_iv(&frame);
}
