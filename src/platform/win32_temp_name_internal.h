// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Random temporary-file names for the Win32 platform layer.
 *
 * The CRT's _mktemp_s() fills XXXXXX with one letter and the process ID, trying
 * 'a' to 'z' and taking the first name not yet in use. A process therefore gets
 * at most 26 live names per template, and gets a name back as soon as its file is
 * removed or renamed, so a second temp file renamed to the same target collides.
 * dsd_mkstemp(), dsd_mkdtemp() and dsd_fopen_private_temp_for_replace() draw names
 * from dsd_nonce_fill() instead and retry when one is taken, as POSIX mkstemp()
 * does. Exclusive creation, not the name, is what keeps another file safe.
 */

#ifndef DSD_NEO_SRC_PLATFORM_WIN32_TEMP_NAME_INTERNAL_H
#define DSD_NEO_SRC_PLATFORM_WIN32_TEMP_NAME_INTERNAL_H

/**
 * @brief Create @p path exclusively under a fresh name.
 *
 * Fills the six characters at @p suffix (inside @p path) with [a-z0-9] (Windows names ignore case) and opens with
 * @p flags plus _O_CREAT | _O_EXCL, owner read/write, drawing a new name while the drawn one is taken.
 *
 * @return The descriptor, or -1 with errno from _open() (EEXIST once every attempt found its name taken).
 */
int dsd_win32_temp_open(char* path, char* suffix, int flags);

#endif /* DSD_NEO_SRC_PLATFORM_WIN32_TEMP_NAME_INTERNAL_H */
