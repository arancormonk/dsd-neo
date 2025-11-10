// SPDX-License-Identifier: GPL-2.0-or-later
/* UI DSP runtime command opcodes for UI_CMD_DSP_OP payloads. */
#pragma once

enum UiDspOp {
    UI_DSP_OP_TOGGLE_CQ = 2,
    UI_DSP_OP_TOGGLE_FLL = 3,
    UI_DSP_OP_TOGGLE_TED = 4,
    UI_DSP_OP_TOGGLE_LMS = 5,
    UI_DSP_OP_TOGGLE_MF = 6,
    UI_DSP_OP_TOGGLE_RRC = 7,
    UI_DSP_OP_RRC_ALPHA_DELTA = 8,   // a: delta (+/-)
    UI_DSP_OP_RRC_SPAN_DELTA = 9,    // a: delta (+/-)
    UI_DSP_OP_CQPSK_CMA_WARMUP = 10, // run warmup (~1500 samples)
    UI_DSP_OP_TOGGLE_WL = 11,
    UI_DSP_OP_TOGGLE_DFE = 12,
    UI_DSP_OP_CYCLE_DFT = 13,
    UI_DSP_OP_TAPS_TOGGLE_5_7 = 14,
    UI_DSP_OP_TOGGLE_DQPSK = 15,
    UI_DSP_OP_TOGGLE_IQBAL = 16,
    UI_DSP_OP_IQ_DC_TOGGLE = 17,
    UI_DSP_OP_IQ_DC_K_DELTA = 18, // a: delta (+/-)
    UI_DSP_OP_TED_SPS_SET = 19,   // a: sps
    UI_DSP_OP_TED_GAIN_SET = 20,  // a: gain
    UI_DSP_OP_C4FM_DD_TOGGLE = 21,
    UI_DSP_OP_C4FM_DD_TAPS_CYCLE = 22,
    UI_DSP_OP_C4FM_DD_MU_DELTA = 23, // a: delta (+/-)
    UI_DSP_OP_C4FM_CLK_CYCLE = 24,
    UI_DSP_OP_C4FM_CLK_SYNC_TOGGLE = 25,
    UI_DSP_OP_FM_CMA_TOGGLE = 26,
    UI_DSP_OP_FM_CMA_TAPS_CYCLE = 27,
    UI_DSP_OP_FM_CMA_MU_DELTA = 28, // a: delta (+/-)
    UI_DSP_OP_FM_CMA_STRENGTH_CYCLE = 29,
    UI_DSP_OP_FM_CMA_WARM_DELTA = 30, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_TOGGLE = 31,
    UI_DSP_OP_FM_LIMITER_TOGGLE = 32,
    UI_DSP_OP_FM_AGC_TARGET_DELTA = 33, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_MIN_DELTA = 34,    // a: delta (+/-)
    UI_DSP_OP_FM_AGC_ATTACK_DELTA = 35, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_DECAY_DELTA = 36,  // a: delta (+/-)
    UI_DSP_OP_BLANKER_TOGGLE = 37,
    UI_DSP_OP_BLANKER_THR_DELTA = 38, // a: delta (+/-)
    UI_DSP_OP_BLANKER_WIN_DELTA = 39, // a: delta (+/-)
    UI_DSP_OP_TUNER_AUTOGAIN_TOGGLE = 40,
    UI_DSP_OP_CQPSK_ACQ_FLL_TOGGLE = 41,
};

typedef struct {
    int op;
    int a;
    int b;
    int c;
    int d;
} UiDspPayload;
