// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The link seams of the PCM stream mark test (test_platform_pcm_stream_mark.c): its link wraps dsd_fd_readable_bytes,
   dsd_socket_readable_bytes and dsd_time_monotonic_ms (GNU ld --wrap), so the platform library's calls of each reach the
   test's wrapper, which calls the real function through its other name (tests/test_support/link_seam.h). */
#ifndef DSD_NEO_TESTS_PLATFORM_TEST_PLATFORM_PCM_STREAM_MARK_SEAMS_H
#define DSD_NEO_TESTS_PLATFORM_TEST_PLATFORM_PCM_STREAM_MARK_SEAMS_H

#include <dsd-neo/platform/sockets.h>
#include <stdint.h>
#include "../test_support/link_seam.h"

long mark_fd_readable_bytes(int fd) DSD_TEST_LINK_NAME(__wrap_dsd_fd_readable_bytes);
long mark_real_fd_readable_bytes(int fd) DSD_TEST_LINK_NAME(__real_dsd_fd_readable_bytes);
long mark_socket_readable_bytes(dsd_socket_t sock) DSD_TEST_LINK_NAME(__wrap_dsd_socket_readable_bytes);
long mark_real_socket_readable_bytes(dsd_socket_t sock) DSD_TEST_LINK_NAME(__real_dsd_socket_readable_bytes);
uint64_t mark_time_monotonic_ms(void) DSD_TEST_LINK_NAME(__wrap_dsd_time_monotonic_ms);
uint64_t mark_real_time_monotonic_ms(void) DSD_TEST_LINK_NAME(__real_dsd_time_monotonic_ms);

#endif
