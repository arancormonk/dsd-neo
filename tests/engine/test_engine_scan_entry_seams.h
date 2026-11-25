// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The link seam of the scan entry test (test_engine_scan_entry.c, issue #890): its link wraps
   dsd_engine_scan_dsp_rate_hz (GNU ld --wrap), so the engine's reads of the DSP rate reach the test's wrapper
   (tests/test_support/link_seam.h). */
#ifndef DSD_NEO_TESTS_ENGINE_TEST_ENGINE_SCAN_ENTRY_SEAMS_H
#define DSD_NEO_TESTS_ENGINE_TEST_ENGINE_SCAN_ENTRY_SEAMS_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "../test_support/link_seam.h"

int scan_entry_dsp_rate_hz(const dsd_opts* opts, const dsd_state* state)
    DSD_TEST_LINK_NAME(__wrap_dsd_engine_scan_dsp_rate_hz);

#endif
