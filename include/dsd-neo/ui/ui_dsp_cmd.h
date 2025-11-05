// SPDX-License-Identifier: GPL-2.0-or-later
/* UI DSP runtime command opcodes for UI_CMD_DSP_OP payloads. */
#pragma once

enum UiDspOp {
    UI_DSP_OP_LSM_SIMPLE_TOGGLE = 1,
    UI_DSP_OP_TOGGLE_CQ = 2,
    UI_DSP_OP_TOGGLE_FLL = 3,
    UI_DSP_OP_TOGGLE_TED = 4,
    UI_DSP_OP_TOGGLE_AUTO = 5,
    UI_DSP_OP_TOGGLE_LMS = 6,
    UI_DSP_OP_TOGGLE_MF = 7,
    UI_DSP_OP_TOGGLE_RRC = 8,
    UI_DSP_OP_RRC_ALPHA_DELTA = 9,   // a: delta (+/-)
    UI_DSP_OP_RRC_SPAN_DELTA = 10,   // a: delta (+/-)
    UI_DSP_OP_CQPSK_CMA_WARMUP = 11, // run warmup (~1500 samples)
    UI_DSP_OP_TOGGLE_WL = 12,
    UI_DSP_OP_TOGGLE_DFE = 13,
    UI_DSP_OP_CYCLE_DFT = 14,
    UI_DSP_OP_TAPS_TOGGLE_5_7 = 15,
    UI_DSP_OP_TOGGLE_DQPSK = 16,
    UI_DSP_OP_TOGGLE_IQBAL = 17,
    UI_DSP_OP_IQ_DC_TOGGLE = 18,
    UI_DSP_OP_IQ_DC_K_DELTA = 19, // a: delta (+/-)
    UI_DSP_OP_TED_SPS_SET = 20,   // a: sps
    UI_DSP_OP_TED_GAIN_SET = 21,  // a: gain
    UI_DSP_OP_C4FM_DD_TOGGLE = 22,
    UI_DSP_OP_C4FM_DD_TAPS_CYCLE = 23,
    UI_DSP_OP_C4FM_DD_MU_DELTA = 24, // a: delta (+/-)
    UI_DSP_OP_C4FM_CLK_CYCLE = 25,
    UI_DSP_OP_C4FM_CLK_SYNC_TOGGLE = 26,
    UI_DSP_OP_FM_CMA_TOGGLE = 27,
    UI_DSP_OP_FM_CMA_TAPS_CYCLE = 28,
    UI_DSP_OP_FM_CMA_MU_DELTA = 29, // a: delta (+/-)
    UI_DSP_OP_FM_CMA_STRENGTH_CYCLE = 30,
    UI_DSP_OP_FM_CMA_WARM_DELTA = 31, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_TOGGLE = 32,
    UI_DSP_OP_FM_AGC_AUTO_TOGGLE = 33,
    UI_DSP_OP_FM_LIMITER_TOGGLE = 34,
    UI_DSP_OP_FM_AGC_TARGET_DELTA = 35, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_MIN_DELTA = 36,    // a: delta (+/-)
    UI_DSP_OP_FM_AGC_ATTACK_DELTA = 37, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_DECAY_DELTA = 38,  // a: delta (+/-)
    UI_DSP_OP_BLANKER_TOGGLE = 39,
    UI_DSP_OP_BLANKER_THR_DELTA = 40, // a: delta (+/-)
    UI_DSP_OP_BLANKER_WIN_DELTA = 41, // a: delta (+/-)
    UI_DSP_OP_MANUAL_TOGGLE = 42,
    UI_DSP_OP_TUNER_AUTOGAIN_TOGGLE = 43,
};

typedef struct {
    int op;
    int a;
    int b;
    int c;
    int d;
} UiDspPayload;
