// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PLATFORM_FILE_COMPAT_INTERNAL_H
#define DSD_NEO_SRC_PLATFORM_FILE_COMPAT_INTERNAL_H

#include <dsd-neo/platform/platform.h>

#if !DSD_PLATFORM_WIN_NATIVE
/*
 * dsd_rename_noreplace()'s fallback where the filesystem has no no-replace rename: a hard link then an unlink, and
 * where it has no hard links either, a check then a rename. Module-private; its own entry point so tests reach the
 * fallback on filesystems whose no-replace rename works.
 */
int dsd_rename_noreplace_by_link(const char* from, const char* to, int* out_source_left);
#endif

#endif /* DSD_NEO_SRC_PLATFORM_FILE_COMPAT_INTERNAL_H */
