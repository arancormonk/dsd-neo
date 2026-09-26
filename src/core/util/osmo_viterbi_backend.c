// SPDX-License-Identifier: ISC
/* Osmo Viterbi backend skeleton.
 * This file provides a swappable implementation entrypoint `viterbi_decode()` when
 * the CMake option `USE_OSMO_VITERBI` is enabled. Currently this skeleton duplicates
 * the native implementation so it builds and exercises the switch; later this file
 * can be replaced with a true osmo-derived implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <dsd-neo/fec/viterbi.h>

// Constraint and state sizes must match the in-tree expectations
#define K          5
#define NUM_STATES (1 << (K - 1))

static uint32_t prevMetrics[NUM_STATES];
static uint32_t currMetrics[NUM_STATES];
static uint32_t prevMetricsData[NUM_STATES];
static uint32_t currMetricsData[NUM_STATES];
static uint16_t viterbi_history[244];

/* Minimal trace env parsing (copied from native code) */
static int viterbi_trace_pos[256];
static int viterbi_trace_count = 0;
static int viterbi_trace_inited = 0;

static void viterbi_parse_trace_env(void) {
    if (viterbi_trace_inited) return;
    viterbi_trace_inited = 1;
    const char *ev = getenv("VITERBI_TRACE_POS");
    if (!ev) return;
    char *buf = strdup(ev);
    if (!buf) return;
    char *tok = strtok(buf, ",");
    while (tok && viterbi_trace_count < (int)(sizeof(viterbi_trace_pos)/sizeof(viterbi_trace_pos[0]))) {
        int v = atoi(tok);
        if (v >= 0) viterbi_trace_pos[viterbi_trace_count++] = v;
        tok = strtok(NULL, ",");
    }
    free(buf);
}

static int viterbi_trace_check(size_t pos) {
    if (viterbi_trace_count == 0) return 0;
    for (int i = 0; i < viterbi_trace_count; i++) if (viterbi_trace_pos[i] == (int)pos) return 1;
    return 0;
}

uint32_t viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len) {
    /* If a real Osmo implementation was provided and compiled in, call it.
     * Otherwise, fall back to the in-tree native algorithm embedded below.
     */
    viterbi_parse_trace_env();
#ifdef HAVE_OSMO_VITERBI_IMPL
    extern uint32_t osmo_viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len);
    return osmo_viterbi_decode(out, in, len);
#else
    return viterbi_native_impl(out, in, len);
#endif
}

/* Exposed native fallback implementation so shims can call it directly. */
uint32_t viterbi_native_impl(uint8_t* out, const uint16_t* in, const uint16_t len)
{
    if (len > 244 * 2) fprintf(stderr, "Input size exceeds max history\n");
    memset(viterbi_history, 0, sizeof(viterbi_history));
    memset(currMetrics, 0, sizeof(currMetrics));
    memset(prevMetrics, 0, sizeof(prevMetrics));

    size_t pos = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t s0 = in[i];
        uint16_t s1 = in[i + 1];

        static const uint16_t COST_TABLE_0[] = {0,0,0,0,0xFFFF,0xFFFF,0xFFFF,0xFFFF};
        static const uint16_t COST_TABLE_1[] = {0,0xFFFF,0xFFFF,0,0,0xFFFF,0xFFFF,0};

        int do_trace = viterbi_trace_check(pos);
        if (do_trace) fprintf(stderr, "[VITERBI TRACE] pos=%zu s0=%u s1=%u\n", pos, s0, s1);

        for (int si = 0; si < NUM_STATES/2; si++) {
            uint32_t metric = (uint32_t)( (COST_TABLE_0[si] > s0)?(COST_TABLE_0[si]-s0):(s0-COST_TABLE_0[si]) )
                              + (uint32_t)( (COST_TABLE_1[si] > s1)?(COST_TABLE_1[si]-s1):(s1-COST_TABLE_1[si]) );
            uint32_t m0 = prevMetrics[si] + metric;
            uint32_t m1 = prevMetrics[si + NUM_STATES/2] + (0x1FFFE - metric);
            uint32_t m2 = prevMetrics[si] + (0x1FFFE - metric);
            uint32_t m3 = prevMetrics[si + NUM_STATES/2] + metric;
            int i0 = si*2;
            int i1 = i0+1;
            if (m0 > m1) { viterbi_history[pos] |= (1<<i0); currMetrics[i0]=m1; } else { viterbi_history[pos] &= ~(1<<i0); currMetrics[i0]=m0; }
            if (m2 > m3) { viterbi_history[pos] |= (1<<i1); currMetrics[i1]=m3; } else { viterbi_history[pos] &= ~(1<<i1); currMetrics[i1]=m2; }
        }

        uint32_t tmp[NUM_STATES];
        for (int i2=0;i2<NUM_STATES;i2++) tmp[i2]=currMetrics[i2];
        for (int i2=0;i2<NUM_STATES;i2++) { currMetrics[i2]=prevMetrics[i2]; prevMetrics[i2]=tmp[i2]; }
        pos++;
    }

    uint8_t state = 0;
    size_t bitPos = (len/2) + 4;
    memset(out, 0, (len/2 - 1)/8 + 1);
    while (pos > 0) {
        bitPos--; pos--;
        uint16_t bit = viterbi_history[pos] & ((1 << (state >> 4)));
        state >>= 1;
        if (bit) { state |= 0x80; out[bitPos/8] |= 1 << (7 - (bitPos % 8)); }
    }

    uint32_t cost = prevMetrics[0];
    for (size_t i3=0;i3<NUM_STATES;i3++) if (prevMetrics[i3] < cost) cost = prevMetrics[i3];
    return cost;
}
