// SPDX-License-Identifier: ISC
/* Native Viterbi implementation extracted from dsd_misc.c
 * This file provides the in-tree Viterbi decoder used by default.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <dsd-neo/fec/viterbi.h>

// Ripped from libM17
#define K          5              //constraint length
#define NUM_STATES (1 << (K - 1)) //number of states

static uint32_t prevMetrics[NUM_STATES];
static uint32_t currMetrics[NUM_STATES];
static uint32_t prevMetricsData[NUM_STATES];
static uint32_t currMetricsData[NUM_STATES];
static uint16_t viterbi_history[244];

/* Viterbi trace support: parse env var VITERBI_TRACE_POS="p,p,..." to dump
 * metrics and decisions at the specified trellis positions. Useful for
 * debugging failing symbols mapped to specific coded-bit positions.
 */
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
    viterbi_parse_trace_env();

    if (len > 244 * 2) {
        fprintf(stderr, "Input size exceeds max history\n");
    }

    viterbi_reset();

    size_t pos = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t s0 = in[i];
        uint16_t s1 = in[i + 1];

        viterbi_decode_bit(s0, s1, pos);
        pos++;
    }
    uint32_t err = viterbi_chainback(out, pos, len / 2);
    return err;
}

uint32_t
viterbi_decode_punctured(uint8_t* out, const uint16_t* in, const uint8_t* punct, const uint16_t in_len,
                         const uint16_t p_len) {
    if (in_len > 244 * 2) {
        fprintf(stderr, "Input size exceeds max history\n");
    }

    uint16_t umsg[244 * 2] = {0}; //unpunctured message
    uint8_t p = 0;                //puncturer matrix entry
    uint16_t u = 0;               //bits count - unpunctured message
    uint16_t i = 0;               //bits read from the input message

    while (i < in_len) {
        if (punct[p]) {
            umsg[u] = in[i];
            i++;
        } else {
            umsg[u] = 0x7FFF;
        }

        u++;
        p++;
        p %= p_len;
    }

    return viterbi_decode(out, umsg, u) - (u - in_len) * 0x7FFF;
}

void
viterbi_decode_bit(uint16_t s0, uint16_t s1, const size_t pos) {
    static const uint16_t COST_TABLE_0[] = {0, 0, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    static const uint16_t COST_TABLE_1[] = {0, 0xFFFF, 0xFFFF, 0, 0, 0xFFFF, 0xFFFF, 0};

    int do_trace = viterbi_trace_check(pos);
    if (do_trace) {
        fprintf(stderr, "[VITERBI TRACE] pos=%zu s0=%u s1=%u\n", pos, s0, s1);
        fprintf(stderr, "[VITERBI TRACE] prevMetrics: ");
        for (int ti = 0; ti < NUM_STATES; ti++) fprintf(stderr, "%u%s", prevMetrics[ti], (ti+1==NUM_STATES)?"\n":" ");
    }

    for (int i = 0; i < NUM_STATES / 2; i++) {
        uint32_t metric = q_abs_diff(COST_TABLE_0[i], s0) + q_abs_diff(COST_TABLE_1[i], s1);

        uint32_t m0 = prevMetrics[i] + metric;
        uint32_t m1 = prevMetrics[i + NUM_STATES / 2] + (0x1FFFE - metric);

        uint32_t m2 = prevMetrics[i] + (0x1FFFE - metric);
        uint32_t m3 = prevMetrics[i + NUM_STATES / 2] + metric;

        int i0 = 2 * i;
        int i1 = i0 + 1;

        if (m0 > m1) {
            viterbi_history[pos] |= (1 << i0);
            currMetrics[i0] = m1;
        } else {
            viterbi_history[pos] &= ~(1 << i0);
            currMetrics[i0] = m0;
        }

        if (m2 > m3) {
            viterbi_history[pos] |= (1 << i1);
            currMetrics[i1] = m3;
        } else {
            viterbi_history[pos] &= ~(1 << i1);
            currMetrics[i1] = m2;
        }

    if (do_trace) {
        fprintf(stderr, "[VITERBI TRACE] currMetrics: ");
        for (int ti = 0; ti < NUM_STATES; ti++) fprintf(stderr, "%u%s", currMetrics[ti], (ti+1==NUM_STATES)?"\n":" ");
        fprintf(stderr, "[VITERBI TRACE] history[pos]=0x%04x\n", viterbi_history[pos]);
    }
    }

    //swap
    uint32_t tmp[NUM_STATES];
    for (int i = 0; i < NUM_STATES; i++) {
        tmp[i] = currMetrics[i];
    }
    for (int i = 0; i < NUM_STATES; i++) {
        currMetrics[i] = prevMetrics[i];
        prevMetrics[i] = tmp[i];
    }
}

uint32_t
viterbi_chainback(uint8_t* out, size_t pos, uint16_t len) {
    uint8_t state = 0;
    size_t bitPos = len + 4;

    memset(out, 0, (len - 1) / 8 + 1);

    while (pos > 0) {
        bitPos--;
        pos--;
        uint16_t bit = viterbi_history[pos] & ((1 << (state >> 4)));
        state >>= 1;
        if (bit) {
            state |= 0x80;
            out[bitPos / 8] |= 1 << (7 - (bitPos % 8));
        }
    }

    uint32_t cost = prevMetrics[0];

    for (size_t i = 0; i < NUM_STATES; i++) {
        uint32_t m = prevMetrics[i];
        if (m < cost) {
            cost = m;
        }
    }

    return cost;
}

void
viterbi_reset(void) {
    memset((uint8_t*)viterbi_history, 0, sizeof(viterbi_history));
    memset((uint8_t*)currMetrics, 0, sizeof(currMetrics));
    memset((uint8_t*)prevMetrics, 0, sizeof(prevMetrics));
    memset((uint8_t*)currMetricsData, 0, sizeof(currMetricsData));
    memset((uint8_t*)prevMetricsData, 0, sizeof(prevMetricsData));
}

uint16_t
q_abs_diff(const uint16_t v1, const uint16_t v2) {
    if (v2 > v1) {
        return v2 - v1;
    }
    return v1 - v2;
}
