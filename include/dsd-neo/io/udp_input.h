// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UDP PCM16LE input backend */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start UDP input on bindaddr:port. Returns 0 on success, <0 on error.
int udp_input_start(dsd_opts* opts, const char* bindaddr, int port, int samplerate);

// Stop UDP input and free resources.
void udp_input_stop(dsd_opts* opts);

// Blocking read of one PCM16 sample from UDP ring. Returns 1 on success, 0 on shutdown.
int udp_input_read_sample(dsd_opts* opts, int16_t* out);

#ifdef __cplusplus
}
#endif
