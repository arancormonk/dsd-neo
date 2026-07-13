// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Dibit/symbol consumption helpers.
 *
 * Declares the dibit reader and soft-decision helpers implemented in
 * `src/core/frames/dsd_dibit.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_DIBIT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_DIBIT_H_H

#include <dsd-neo/platform/platform.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

typedef struct {
    uint8_t reliability; /* 0=uncertain, 255=confident; min confidence across both bits. */
    int16_t llr[2];      /* [0]=MSB, [1]=LSB. Positive values favor bit 1. */
} dsd_dibit_soft_t;

enum DSD_ATTR_PACKED {
    DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN = 0,
    DSD_SYMBOL_REPLAY_FORMAT_LEGACY = 1,
    DSD_SYMBOL_REPLAY_FORMAT_SOFT = 2,
};

#define DSD_SYMBOL_CAPTURE_SOFT_MAGIC       "DSDNSYM2"
#define DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE 16
#define DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE 10

#ifdef __cplusplus
extern "C" {
#endif

int get_dibit_and_analog_signal(dsd_opts* opts, dsd_state* state, int* out_analog_signal);
int getDibitSoft(dsd_opts* opts, dsd_state* state, dsd_dibit_soft_t* out_soft);
int getDibitWithReliability(dsd_opts* opts, dsd_state* state, uint8_t* out_reliability);
int getDibitAndSoftSymbol(dsd_opts* opts, dsd_state* state, float* out_soft_symbol);
void write_symbol_capture_record(dsd_opts* opts, dsd_state* state, int dibit, float symbol);
uint8_t dmr_compute_reliability(const dsd_state* st, float sym);
void soft_symbol_frame_begin(dsd_state* state);
uint16_t soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state, int bit_position);
uint16_t gmsk_soft_symbol_to_viterbi_cost(float symbol, const dsd_state* state);
int digitize(const dsd_opts* opts, dsd_state* state, float symbol);
void skipDibit(dsd_opts* opts, dsd_state* state, int count);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_DIBIT_H_H */
