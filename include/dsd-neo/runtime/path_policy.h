// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_RUNTIME_PATH_POLICY_H
#define DSD_NEO_RUNTIME_PATH_POLICY_H

/**
 * @file
 * @brief Filesystem path policy for explicit user-selected paths.
 *
 * CLI arguments, environment variables, UI prompts, and config file values are
 * explicit local user intent. Protocol and network payloads must not be routed
 * here unless a user option first selected them as paths.
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Expand a user-selected path using config-style expansion.
 *
 * Supports the same ~, $VAR, and ${VAR} expansion as config path values.
 *
 * @param requested User-supplied path.
 * @param out       Output buffer for the expanded path.
 * @param out_size  Output buffer size.
 * @return 0 on success, -1 on error with errno set.
 */
int dsd_path_expand_user(const char* requested, char* out, size_t out_size);

/**
 * @brief Return non-zero when path is absolute on the current platform.
 */
int dsd_path_is_absolute(const char* path);

/**
 * @brief Resolve a possibly relative path against the directory of a base file.
 *
 * Absolute paths remain absolute. Relative paths are joined to the directory of
 * base_file_path when it names a directory-containing path; otherwise they stay
 * relative to the current working directory.
 *
 * @param base_file_path Path of the containing file.
 * @param requested      User-supplied path to resolve.
 * @param out            Output buffer.
 * @param out_size       Output buffer size.
 * @return 0 on success, -1 on error with errno set.
 */
int dsd_path_resolve_relative_to_file(const char* base_file_path, const char* requested, char* out, size_t out_size);

/**
 * @brief Resolve a user path to an existing regular file.
 *
 * The final path component is opened without following symlinks/reparse points
 * where the platform exposes that protection.
 *
 * @param requested User-supplied path.
 * @param out       Output buffer for the expanded path.
 * @param out_size  Output buffer size.
 * @return 0 on success, -1 on error with errno set.
 */
int dsd_path_resolve_user_read_file(const char* requested, char* out, size_t out_size);

/**
 * @brief Open an existing regular file selected by the user.
 *
 * @param requested User-supplied path.
 * @param out       Output buffer for the expanded path actually opened.
 * @param out_size  Output buffer size.
 * @return Read stream, or NULL on error with errno set.
 */
FILE* dsd_path_fopen_user_read_file(const char* requested, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_PATH_POLICY_H */
