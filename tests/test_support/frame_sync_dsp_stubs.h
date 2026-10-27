// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#ifndef DSD_NEO_TEST_FRAME_SYNC_DSP_STUBS_H
#define DSD_NEO_TEST_FRAME_SYNC_DSP_STUBS_H

/* The shutdown requests the shared stubs counted (frame_sync_dsp_stubs.c's dsd_request_shutdown()); nothing else
 * happens on one, so a test can tell a reader asked for one. */
extern int g_frame_sync_dsp_stub_shutdown_requests;

#endif
