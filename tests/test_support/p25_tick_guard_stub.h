// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_P25_TICK_GUARD_STUB_H
#define DSD_NEO_TEST_P25_TICK_GUARD_STUB_H

/* How many times a thread that held the stub's P25 SM tick guard entered it again: the real guard is not re-entrant, so
   each one would have been a hang. */
int dsd_test_p25_tick_guard_reentries(void);

#endif /* DSD_NEO_TEST_P25_TICK_GUARD_STUB_H */
