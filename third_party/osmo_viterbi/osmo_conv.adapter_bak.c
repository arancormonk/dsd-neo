/* Minimal adapter implementation of osmo_conv_decode.
 * Converts signed int8 soft metrics to the project's 16-bit soft format
 * and forwards to the native Viterbi implementation `viterbi_native_impl`.
 * This avoids vendoring the full osmocom core library while providing
 * compatibility for the osmo viterbi helper files.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <osmocom/core/conv.h>

/* Native viterbi fallback exposed by the osmo shim/backend.
 * Signature: `uint32_t viterbi_native_impl(uint8_t* out, const uint16_t* in, const uint16_t len);`
 */
extern uint32_t viterbi_native_impl(uint8_t* out, const uint16_t* in, const uint16_t len);

int osmo_conv_decode(struct osmo_conv_code *code, const int8_t *input, uint8_t *output)
{
    if (!code || !input || !output) return -1;

    int len = code->len;
    if (len <= 0) return -1;

    /* Allocate temporary buffer for 16-bit soft metrics. */
    uint16_t *soft = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)len);
    if (!soft) return -1;

    /* Map signed int8 (-128..127) -> uint16 (0..65535).
     * We map -128 -> 0 and 127 -> 65535 linearly.
     */
    for (int i = 0; i < len; i++) {
        int v = (int)input[i] + 128; /* 0..255 */
        /* scale to 0..65535 without floating point */
        uint16_t s = (uint16_t)((v * 65535) / 255);
        soft[i] = s;
    }

    /* Forward to native viterbi implementation.
     * `viterbi_native_impl` expects pairs of soft symbols (s0,s1) and a
     * length equal to the number of soft symbols; this matches `len`.
     */
    uint32_t cost = viterbi_native_impl(output, soft, (uint16_t)len);

    free(soft);
    return (int)cost;
}
