// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * QPSK Costas loop (carrier recovery) interface.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct demod_state; /* fwd decl */

/* Run Costas loop rotation + loop update on interleaved I/Q in-place. */
void cqpsk_costas_mix_and_update(struct demod_state* d);

#ifdef __cplusplus
}
#endif
