// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/* The link seams of the PCM stream mark test (test_platform_pcm_stream_mark.c): its link wraps dsd_fd_readable_bytes,
   dsd_socket_readable_bytes, dsd_time_monotonic_ms, dsd_read, dsd_mutex_init and dsd_mutex_lock (GNU ld --wrap), so the platform library's calls of
   each reach the test's wrapper, which calls the real function through its other name
   (tests/test_support/link_seam.h). */
#ifndef DSD_NEO_TESTS_PLATFORM_TEST_PLATFORM_PCM_STREAM_MARK_SEAMS_H
#define DSD_NEO_TESTS_PLATFORM_TEST_PLATFORM_PCM_STREAM_MARK_SEAMS_H

#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <stddef.h>
#include <stdint.h>
#include "../test_support/link_seam.h"

long mark_fd_readable_bytes(int fd) DSD_TEST_LINK_NAME(__wrap_dsd_fd_readable_bytes);
long mark_real_fd_readable_bytes(int fd) DSD_TEST_LINK_NAME(__real_dsd_fd_readable_bytes);
long mark_socket_readable_bytes(dsd_socket_t sock) DSD_TEST_LINK_NAME(__wrap_dsd_socket_readable_bytes);
long mark_real_socket_readable_bytes(dsd_socket_t sock) DSD_TEST_LINK_NAME(__real_dsd_socket_readable_bytes);
uint64_t mark_time_monotonic_ms(void) DSD_TEST_LINK_NAME(__wrap_dsd_time_monotonic_ms);
uint64_t mark_real_time_monotonic_ms(void) DSD_TEST_LINK_NAME(__real_dsd_time_monotonic_ms);
ssize_t mark_read(int fd, void* buf, size_t count) DSD_TEST_LINK_NAME(__wrap_dsd_read);
ssize_t mark_real_read(int fd, void* buf, size_t count) DSD_TEST_LINK_NAME(__real_dsd_read);
int mark_mutex_init(dsd_mutex_t* mutex) DSD_TEST_LINK_NAME(__wrap_dsd_mutex_init);
int mark_real_mutex_init(dsd_mutex_t* mutex) DSD_TEST_LINK_NAME(__real_dsd_mutex_init);
int mark_mutex_lock(dsd_mutex_t* mutex) DSD_TEST_LINK_NAME(__wrap_dsd_mutex_lock);
int mark_real_mutex_lock(dsd_mutex_t* mutex) DSD_TEST_LINK_NAME(__real_dsd_mutex_lock);

#endif
