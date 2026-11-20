// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Remember @p path, a temp file or folder this process just created, so that it is removed if the test leaves it
 * behind (temp_paths.c).
 *
 * When the process ends, normally or through abort() (a failed assert()), each tracked path that still exists is
 * removed, a folder with everything in it, and a line on stderr names it. A test still removes what it creates; this
 * covers the exit paths that skip its cleanup, and a process that ends normally with a tracked path left exits with
 * EXIT_FAILURE, whatever main() returned. dsd_test_mkstemp() and dsd_test_mkdtemp() in test_support.h track what they
 * create, so a test calls this only for a temp path it makes another way. Not thread-safe: track from one thread at a
 * time.
 */
void dsd_test_temp_track(const char* path);

#ifdef __cplusplus
} /* extern "C" */
#endif
