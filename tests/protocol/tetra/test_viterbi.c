// SPDX-License-Identifier: GPL-3.0-or-later
/* Compare every survivor and the complete traceback with a scalar, 64-bit
 * polynomial oracle, including unreachable states and tied paths. */
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <string.h>

// NOLINTNEXTLINE(bugprone-suspicious-include): inspect private survivor metrics without exporting a test-only API.
#include "../../../src/protocol/tetra/tetra_fec.c"

static uint32_t rng = 0x8471ab31u;

static uint16_t
next_cost(void) {
    rng = rng * 1664525u + 1013904223u;
    return (uint16_t)(rng >> 16);
}

static unsigned
oracle_parity(unsigned value) {
    unsigned parity = 0;
    for (int bit = 0; bit < 5; bit++) {
        parity ^= (value >> bit) & 1u;
    }
    return parity;
}

static void
check_code(const tetra_conv_code* code) {
    static const int lengths[] = {1, 2, 3, 4, 5, 11, 64, 112, 184, 432, 512};
    const uint64_t unreachable = UINT64_MAX / 4u;
    for (int trial = 0; trial < 8; trial++) {
        for (unsigned l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
            const int nbits = lengths[l];
            uint16_t costs[512 * 4];
            uint8_t expected[512], actual[512];
            uint8_t predecessors[512][16] = {{0}};
            uint64_t reference[2][16];
            uint32_t metric[2][16];
            for (int i = 0; i < nbits * code->lanes; i++) {
                const uint16_t r = next_cost();
                costs[i] = trial == 0   ? 0
                           : trial == 1 ? 65535
                           : trial == 2 ? 32768
                           : trial == 3 ? (uint16_t)(r & 1u) * 65535u
                           : trial == 4 ? (uint16_t)(32767u + (r & 1u))
                                        : r;
            }
            for (int state = 0; state < 16; state++) {
                reference[0][state] = state == 0 ? 0 : unreachable;
                metric[0][state] = state == 0 ? 0 : UINT32_MAX / 4u;
            }
            for (int pos = 0; pos < nbits; pos++) {
                uint64_t* next = reference[(pos + 1) & 1];
                const uint64_t* prev = reference[pos & 1];
                for (int state = 0; state < 16; state++) {
                    next[state] = unreachable;
                }
                for (int state = 0; state < 16; state++) {
                    if (prev[state] == unreachable) {
                        continue;
                    }
                    for (int bit = 0; bit < 2; bit++) {
                        const int to = code->shift_up ? ((2 * state + bit) % 16) : (8 * bit + state / 2);
                        uint64_t cost = prev[state];
                        for (int lane = 0; lane < code->lanes; lane++) {
                            const unsigned p = oracle_parity((unsigned)(16 * bit + state) & code->taps[lane]);
                            const uint16_t got = costs[pos * code->lanes + lane];
                            cost += p ? 65535u - got : got;
                        }
                        if (cost < next[to]) {
                            next[to] = cost;
                            predecessors[pos][to] = (uint8_t)state;
                        }
                    }
                }
                uint8_t pred[16];
                DSD_MEMSET(pred, 0xA5, sizeof(pred));
                conv_acs_step(code, metric[pos & 1], metric[(pos + 1) & 1], costs + (size_t)pos * (size_t)code->lanes,
                              pred);
                for (int state = 0; state < 16; state++) {
                    const uint32_t want = next[state] == unreachable ? UINT32_MAX / 4u : (uint32_t)next[state];
                    assert(metric[(pos + 1) & 1][state] == want);
                    assert(pred[state] == predecessors[pos][state]);
                }
            }
            int state = 0;
            for (int pos = nbits - 1; pos >= 0; pos--) {
                expected[pos] = (uint8_t)(code->shift_up ? state % 2 : state / 8);
                state = predecessors[pos][state];
            }
            conv_viterbi(code, costs, nbits, actual);
            assert(memcmp(expected, actual, (size_t)nbits) == 0);
        }
    }
}

int
main(void) {
    check_code(&k_signalling_code);
    check_code(&k_speech_code);
    uint16_t costs[513 * 4] = {0};
    uint8_t output[513] = {0};
    assert(tetra_viterbi_decode_soft(costs, 513 * 4, output, 513) == -1);
    assert(tetra_viterbi_decode_soft(costs, 512 * 4, output, 511) == -1);
    assert(tetra_viterbi_decode_soft(costs, 512 * 4, output, 512) == 512);
    return 0;
}
