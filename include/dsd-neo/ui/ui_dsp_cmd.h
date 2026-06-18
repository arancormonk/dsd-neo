// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief UI DSP runtime command opcodes for `UI_CMD_DSP_OP` payloads.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_UI_UI_DSP_CMD_H_
#define DSD_NEO_INCLUDE_DSD_NEO_UI_UI_DSP_CMD_H_

/** DSP control opcodes understood by the demod thread. */
enum UiDspOp {
    UI_DSP_OP_TOGGLE_CQ = 2,
    UI_DSP_OP_TOGGLE_IQBAL = 5,
    UI_DSP_OP_IQ_DC_TOGGLE = 6,
    UI_DSP_OP_IQ_DC_K_DELTA = 7, // a: delta (+/-)
    UI_DSP_OP_TED_GAIN_SET = 9,  // a: CQPSK timing gain
    UI_DSP_OP_TUNER_AUTOGAIN_TOGGLE = 18,
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

#endif /* DSD_NEO_INCLUDE_DSD_NEO_UI_UI_DSP_CMD_H_ */
