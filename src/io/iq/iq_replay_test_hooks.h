// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Test seams of the replay metadata reader (iq_replay.c), built only with DSD_NEO_TEST_HOOKS. */

#ifndef DSD_NEO_IO_IQ_REPLAY_TEST_HOOKS_H
#define DSD_NEO_IO_IQ_REPLAY_TEST_HOOKS_H

#ifdef DSD_NEO_TEST_HOOKS

#ifdef __cplusplus
extern "C" {
#endif

/* Make one sniff read of a file's first bytes fail as an I/O error does (EIO), after @p after sniff reads succeed; -1
 * disarms. Both sniffs read through it: the given path's, and the data_file a sidecar names. */
void dsd_iq_replay_test_fail_sniff_read(int after);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TEST_HOOKS */

#endif /* DSD_NEO_IO_IQ_REPLAY_TEST_HOOKS_H */
