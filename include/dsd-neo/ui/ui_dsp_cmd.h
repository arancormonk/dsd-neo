// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief UI DSP runtime command opcodes for `UI_CMD_DSP_OP` payloads.
 */
#pragma once

/** DSP control opcodes understood by the demod thread. */
enum UiDspOp {
    UI_DSP_OP_TOGGLE_CQ = 2,
    UI_DSP_OP_TOGGLE_FLL = 3,
    UI_DSP_OP_TOGGLE_TED = 4,
    UI_DSP_OP_TOGGLE_MF = 5,
    UI_DSP_OP_TOGGLE_RRC = 6,
    UI_DSP_OP_RRC_ALPHA_DELTA = 7, // a: delta (+/-)
    UI_DSP_OP_RRC_SPAN_DELTA = 8,  // a: delta (+/-)
    UI_DSP_OP_TOGGLE_IQBAL = 9,
    UI_DSP_OP_IQ_DC_TOGGLE = 10,
    UI_DSP_OP_IQ_DC_K_DELTA = 11, // a: delta (+/-)
    UI_DSP_OP_TED_SPS_SET = 12,   // a: sps
    UI_DSP_OP_TED_GAIN_SET = 13,  // a: gain
    UI_DSP_OP_C4FM_DD_TOGGLE = 14,
    UI_DSP_OP_C4FM_DD_TAPS_CYCLE = 15,
    UI_DSP_OP_C4FM_DD_MU_DELTA = 16, // a: delta (+/-)
    UI_DSP_OP_C4FM_CLK_CYCLE = 17,
    UI_DSP_OP_C4FM_CLK_SYNC_TOGGLE = 18,
    UI_DSP_OP_FM_AGC_TOGGLE = 19,
    UI_DSP_OP_FM_LIMITER_TOGGLE = 20,
    UI_DSP_OP_FM_AGC_TARGET_DELTA = 21, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_MIN_DELTA = 22,    // a: delta (+/-)
    UI_DSP_OP_FM_AGC_ATTACK_DELTA = 23, // a: delta (+/-)
    UI_DSP_OP_FM_AGC_DECAY_DELTA = 24,  // a: delta (+/-)
    UI_DSP_OP_BLANKER_TOGGLE = 25,
    UI_DSP_OP_BLANKER_THR_DELTA = 26, // a: delta (+/-)
    UI_DSP_OP_BLANKER_WIN_DELTA = 27, // a: delta (+/-)
    UI_DSP_OP_TUNER_AUTOGAIN_TOGGLE = 28,
    UI_DSP_OP_CQPSK_ACQ_FLL_TOGGLE = 29,
};

/**
 * @brief Payload wrapper for DSP opcodes (fields interpreted per opcode).
 */
typedef struct {
    int op;
    int a;
    int b;
    int c;
    int d;
} UiDspPayload;
