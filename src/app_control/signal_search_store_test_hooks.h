// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** @file Failure seams of the Signal search store, built only with DSD_NEO_TEST_HOOKS: an open or read error is the
 * only portable way to get one (Windows has no unreadable mode, and root reads everything). @p which is a
 * dsd_app_search_store_file. */
#ifndef DSD_NEO_APP_CONTROL_SIGNAL_SEARCH_STORE_TEST_HOOKS_H
#define DSD_NEO_APP_CONTROL_SIGNAL_SEARCH_STORE_TEST_HOOKS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** The next opens of @p which fail with @p err; 0 opens normally again. */
void dsd_app_signal_search_store_test_fail_open(int which, int err);
/** Reads of @p which fail once @p after bytes were read; -1 reads normally again. */
void dsd_app_signal_search_store_test_fail_read(int which, long after);
/** Checking kept name @p n ("….unreadable-<n>") cannot tell whether it exists; 0 checks normally again. */
void dsd_app_signal_search_store_test_fail_probe(int n);
/** The folder the temp sweep lists for a store file at @p path, into @p out. */
void dsd_app_signal_search_store_test_folder_of(const char* path, char* out, size_t cap);
/** Called on the worker right after a file was renamed aside, with its path and its kept path; NULL for none. */
void dsd_app_signal_search_store_test_after_aside(void (*fn)(const char* path, const char* kept));
#ifdef __cplusplus
}
#endif
#endif
