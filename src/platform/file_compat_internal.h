// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PLATFORM_FILE_COMPAT_INTERNAL_H
#define DSD_NEO_SRC_PLATFORM_FILE_COMPAT_INTERNAL_H

#include <dsd-neo/platform/platform.h>

#if !DSD_PLATFORM_WIN_NATIVE
/*
 * dsd_rename_noreplace()'s fallbacks where the filesystem has no no-replace rename: a hard link then an unlink, and
 * where it has no hard links either, a reservation (below). Module-private; their own entry points so tests reach the
 * fallbacks on filesystems whose no-replace rename works.
 */
int dsd_rename_noreplace_by_link(const char* from, const char* to, int* out_source_left);

/*
 * Claim @p to with an exclusive create (EEXIST when taken), then rename @p from onto that empty claim. Only a process
 * that deletes the claim before the rename can still be replaced. On a failed rename the claim is removed again.
 */
int dsd_rename_noreplace_by_reservation(const char* from, const char* to);
#endif

#endif /* DSD_NEO_SRC_PLATFORM_FILE_COMPAT_INTERNAL_H */
