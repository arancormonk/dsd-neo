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

#if DSD_PLATFORM_WIN_NATIVE
#include <stddef.h>
#include <wchar.h>

/*
 * dsd_dir_list()'s search patterns: "<dir>\*", with no second separator when @p dir already ends in one, as a drive
 * root ("C:\") does. Module-private; their own entry points so tests check the pattern, which Windows would otherwise
 * hide by folding a doubled separator.
 */

/* The pattern for a folder named in UTF-16; NULL when memory runs out. The caller frees it. */
wchar_t* dsd_dir_list_wide_pattern(const wchar_t* wide_dir);

/* The pattern for a folder named in the ANSI code page, whose last character is told apart from a double-byte one's
   trail byte (0x5C, the backslash, is one in code page 932). 0, or -1 with errno ENAMETOOLONG when @p out is too
   small. */
int dsd_dir_list_ansi_pattern(const char* dir, char* out, size_t out_size);
#endif

#endif /* DSD_NEO_SRC_PLATFORM_FILE_COMPAT_INTERNAL_H */
