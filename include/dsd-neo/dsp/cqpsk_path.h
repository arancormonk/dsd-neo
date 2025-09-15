// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#include <stdint.h>

struct demod_state; /* fwd decl */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize CQPSK path state associated with a demod_state. */
void cqpsk_init(struct demod_state* s);

/*
 * Process one block of interleaved I/Q baseband for QPSK/LSM robustness.
 *
 * This runs after FLL mixing and before FM discrimination. Initial
 * implementation is a safe pass-through; subsequent versions will apply a
 * tiny decision-directed equalizer.
 */
void cqpsk_process_block(struct demod_state* s);

#ifdef __cplusplus
}
#endif
