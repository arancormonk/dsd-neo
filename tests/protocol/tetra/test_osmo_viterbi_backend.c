// SPDX-License-Identifier: ISC
#include <stdio.h>
#include <stdint.h>
#include <dsd-neo/fec/viterbi.h>

int main(void) {
#ifdef HAVE_OSMO_VITERBI_IMPL
    uint16_t in[8] = {0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF};
    uint8_t out[4] = {0};
    uint32_t cost = viterbi_decode(out, in, 8);
    printf("OSMO_BACKEND_RUN cost=%u\n", cost);
    return 0;
#else
    printf("SKIPPED: no osmo backend compiled in\n");
    return 0;
#endif
}
