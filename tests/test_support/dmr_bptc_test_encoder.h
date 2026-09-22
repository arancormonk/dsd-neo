// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_DMR_BPTC_TEST_ENCODER_H_
#define DSD_NEO_TEST_DMR_BPTC_TEST_ENCODER_H_
#include <stdint.h>
void dmr_test_encode_bptc_196x96(const uint8_t payload[96], const uint8_t reserved[3], uint8_t info[196]);
int dmr_test_check_reference_burst(void);
#endif
