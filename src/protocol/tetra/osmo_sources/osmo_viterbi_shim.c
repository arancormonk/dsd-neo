/* Shim to adapt osmo-tetra viterbi wrappers to in-tree viterbi_decode()
 * Converts int8 soft-symbols to our 16-bit soft-costs and calls viterbi_decode().
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <dsd-neo/fec/viterbi.h>
#include "osmo_shim_map.h"

/* Convert osmo int8_t soft inputs to our 16-bit viterbi cost representation.
 * Mapping chosen:
 *   in > 0  -> 0xFFFF (strong '1')
 *   in == 0 -> 0x0000 (strong '0')
 *   otherwise -> 0x7FFF (neutral/unknown)
 */
static void osmo_to_depunc_costs(const int8_t *in, uint16_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        out[i] = osmo_soft_to_cost(in[i]);
    }
}

int conv_cch_decode(int8_t *input, uint8_t *output, int n)
{
    if (!input || !output) return -1;
    int coded_bits = n * 4; /* osmo conv CCH emits 4 output bits per input symbol */
    uint16_t *costs = (uint16_t*)malloc(sizeof(uint16_t) * coded_bits);
    if (!costs) return -2;
    osmo_to_depunc_costs(input, costs, coded_bits);
    viterbi_decode(output, costs, (uint16_t)coded_bits);
    free(costs);
    return 0;
}

int conv_tch_decode(int8_t *input, uint8_t *output, int n)
{
    if (!input || !output) return -1;
    int coded_bits = n * 4;
    uint16_t *costs = (uint16_t*)malloc(sizeof(uint16_t) * coded_bits);
    if (!costs) return -2;
    osmo_to_depunc_costs(input, costs, coded_bits);
    viterbi_decode(output, costs, (uint16_t)coded_bits);
    free(costs);
    return 0;
}

void viterbi_dec_sb1_wrapper(const uint8_t *in, uint8_t *out, unsigned int sym_count)
{
    if (!in || !out) return;
    int total = sym_count * 4;
    int8_t *vit_inp = (int8_t*)malloc((size_t)total);
    if (!vit_inp) return;
    for (int i = 0; i < total; i++) {
        uint8_t v = in[i];
        switch (v) {
            case 0: vit_inp[i] = 127; break;
            case 0xff: vit_inp[i] = 0; break;
            default: vit_inp[i] = -127; break;
        }
    }
    conv_cch_decode(vit_inp, out, sym_count);
    free(vit_inp);
}
