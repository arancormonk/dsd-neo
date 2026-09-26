// SPDX-License-Identifier: ISC
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <osmocom/core/conv.h>

/* Adapter implementing osmo_conv_decode for the project build.
 * Maps signed 8-bit soft inputs into the project's 16-bit soft format
 * and forwards to the native viterbi implementation.
 */

extern uint32_t viterbi_native_impl(uint8_t* out, const uint16_t* in, const uint16_t len);

static inline uint16_t clamp_u16(int32_t v)
{
    if (v < 0) return 0;
    if (v > 0xFFFF) return 0xFFFF;
    return (uint16_t)v;
}

int osmo_conv_decode(struct osmo_conv_code *code, const int8_t *input, uint8_t *output)
{
    if (!code || !input || !output) return -1;

    int len = code->len;
    if (len <= 0) return -1;

    uint16_t *umsg = malloc(sizeof(uint16_t) * len);
    if (!umsg) return -1;

    for (int i = 0; i < len; i++) {
        int8_t s = input[i];
        if (s == 0) {
            umsg[i] = 0x7FFF;
        } else {
            int32_t scaled = ((int32_t)s * 0x7FFF) / 127;
            int32_t v = (int32_t)0x7FFF + scaled;
            umsg[i] = clamp_u16(v);
        }
    }

    /* Call native impl; viterbi_native_impl expects length in coded bits */
    uint32_t cost = viterbi_native_impl(output, umsg, (uint16_t)len);
    free(umsg);
    return (int)cost;
}
