// SPDX-License-Identifier: GPL-3.0-or-later
/* Compare every survivor and the complete traceback with a scalar, 64-bit
 * polynomial oracle, including unreachable states and tied paths, and the
 * vector add-compare-select kernel with the scalar one step by step. */
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

/* A survivor metric the decoder can hold: INF, just below it, a tie-prone multiple of 32767.5, or any path cost. */
static uint32_t
next_metric(void) {
    const uint32_t inf = TETRA_VITERBI_INF;
    rng = rng * 1664525u + 1013904223u;
    const uint32_t r = rng >> 8;
    switch (rng >> 29) {
        case 0: return inf;
        case 1: return inf - (r & 0x3FFFFu);
        case 2: return (r & 7u) * 65535u / 2u;
        case 3: return r & 15u;
        default: return r & ((1u << 27) - 1u);
    }
}

static uint16_t
next_step_cost(void) {
    static const uint16_t edges[] = {0, 1, 32767, 32768, 65534, 65535};
    const uint16_t r = next_cost();
    return (r & 3u) ? edges[(r >> 2) % 6u] : next_cost();
}

/* The kernel conv_viterbi() runs, over one step from arbitrary metrics and over whole runs, against
   conv_acs_step(). */
static void
check_acs_kernel(const tetra_conv_code* code) {
    enum { RUN_MAX = 512 };

    static uint16_t costs[RUN_MAX * 4];
    static uint8_t want_pred[(RUN_MAX + 1) * 16], got_pred[(RUN_MAX + 1) * 16];
    int ties = 0, highs = 0, unreachable = 0;
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t prev[16], want[16], got[16];
        uint8_t pred[16];
        for (int state = 0; state < 16; state++) {
            prev[state] = next_metric();
            got[state] = prev[state];
        }
        for (int lane = 0; lane < code->lanes; lane++) {
            costs[lane] = next_step_cost();
        }
        uint32_t branch[16] = {0};
        for (int lane = 0; lane < code->lanes; lane++) {
            for (int to = 0; to < 16; to++) {
                branch[to] += ((code->incoming[to] >> lane) & 1u) ? 65535u - costs[lane] : costs[lane];
            }
        }
        /* Force ties: give some higher predecessors exactly the metric that matches the lower edge. */
        for (int to = 0; (trial & 3) == 0 && to < 16; to++) {
            const int low = code->shift_up ? (to >> 1) : ((to << 1) & 15);
            const int high = low + (code->shift_up ? 8 : 1);
            const int64_t tied =
                (int64_t)prev[low] + (int64_t)branch[to] - ((int64_t)code->lanes * 65535 - (int64_t)branch[to]);
            if ((next_cost() & 1u) && tied >= 0 && tied <= (int64_t)TETRA_VITERBI_INF) {
                prev[high] = (uint32_t)tied;
                got[high] = prev[high];
            }
        }
        conv_acs_step(code, prev, want, costs, pred);
        DSD_MEMSET(got_pred, 0xA5, 16);
        conv_acs_run(code, got, costs, 1, got_pred);
        assert(memcmp(want, got, sizeof(want)) == 0);
        assert(memcmp(pred, got_pred, 16) == 0);
        for (int to = 0; to < 16; to++) {
            const int low = code->shift_up ? (to >> 1) : ((to << 1) & 15);
            const int high = low + (code->shift_up ? 8 : 1);
            const uint32_t a = prev[low] + branch[to];
            const uint32_t b = prev[high] + ((uint32_t)code->lanes * 65535u - branch[to]);
            ties += a == b && a < TETRA_VITERBI_INF;
            highs += b < a && b < TETRA_VITERBI_INF;
            unreachable += want[to] == TETRA_VITERBI_INF;
        }
    }
    assert(ties > 1000 && highs > 1000 && unreachable > 1000);

    static const int lengths[] = {1, 2, 3, 7, 80, 94, 144, 184, 288, 511, 512};
    for (int trial = 0; trial < 6; trial++) {
        for (unsigned l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
            const int nsteps = lengths[l];
            uint32_t want[16], got[16];
            for (int state = 0; state < 16; state++) {
                want[state] = trial < 3 ? (state == 0 ? 0 : TETRA_VITERBI_INF) : next_metric();
                got[state] = want[state];
            }
            for (int i = 0; i < nsteps * code->lanes; i++) {
                costs[i] = trial == 1   ? (uint16_t)32767u
                           : trial == 2 ? (uint16_t)(next_cost() & 1u) * 65535u
                                        : next_step_cost();
            }
            DSD_MEMSET(want_pred, 0x5A, sizeof(want_pred));
            DSD_MEMSET(got_pred, 0xA5, sizeof(got_pred));
            conv_acs_run_scalar(code, want, costs, nsteps, want_pred);
            conv_acs_run(code, got, costs, nsteps, got_pred);
            assert(memcmp(want, got, sizeof(want)) == 0);
            assert(memcmp(want_pred, got_pred, (size_t)nsteps * 16u) == 0);
            assert(got_pred[(size_t)nsteps * 16u] == 0xA5);
        }
    }
}

int
main(void) {
    check_acs_kernel(&k_signalling_code);
    check_acs_kernel(&k_speech_code);
    check_code(&k_signalling_code);
    check_code(&k_speech_code);
    uint16_t costs[513 * 4] = {0};
    uint8_t output[513] = {0};
    assert(tetra_viterbi_decode_soft(costs, 513 * 4, output, 513) == -1);
    assert(tetra_viterbi_decode_soft(costs, 512 * 4, output, 511) == -1);
    assert(tetra_viterbi_decode_soft(costs, 512 * 4, output, 512) == 512);
    return 0;
}
