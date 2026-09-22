// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_DMR_RS_TEST_ENCODER_H_
#define DSD_NEO_TEST_DMR_RS_TEST_ENCODER_H_
#include <stdint.h>
void dmr_test_encode_rs_12_9(const uint8_t data[9], uint8_t parity[3]);
#endif
