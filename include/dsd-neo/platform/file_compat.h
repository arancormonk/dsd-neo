// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_FILE_COMPAT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_FILE_COMPAT_H_H

/**
 * @file
 * @brief Cross-platform file descriptor compatibility for DSD-neo.
 */

#include <dsd-neo/platform/platform.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#if DSD_PLATFORM_WIN_NATIVE
#include <sys/stat.h>
typedef struct _stat dsd_stat_t;
#else
#include <sys/stat.h>
typedef struct stat dsd_stat_t;
#endif

/* ssize_t definition for Windows */
#if DSD_PLATFORM_WIN_NATIVE && !defined(_SSIZE_T_DEFINED)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

/**
 * @brief Get file descriptor from FILE stream.
 *
 * @param fp    FILE pointer.
 * @return File descriptor, or -1 on error.
 */
int dsd_fileno(FILE* fp);

/**
 * @brief Check if file descriptor refers to a terminal.
 *
 * @param fd    File descriptor.
 * @return Non-zero if terminal, 0 otherwise.
 */
int dsd_isatty(int fd);

/**
 * @brief Duplicate a file descriptor.
 *
 * @param oldfd     File descriptor to duplicate.
 * @return New file descriptor, or -1 on error.
 */
int dsd_dup(int oldfd);

/**
 * @brief Duplicate a file descriptor to a specific number.
 *
 * @param oldfd     Source file descriptor.
 * @param newfd     Target file descriptor number.
 * @return newfd on success, -1 on error.
 */
int dsd_dup2(int oldfd, int newfd);

/**
 * @brief Close a file descriptor.
 *
 * @param fd    File descriptor to close.
 * @return 0 on success, -1 on error.
 */
int dsd_close(int fd);

/**
 * @brief Flush file data to disk.
 *
 * @param fd    File descriptor.
 * @return 0 on success, -1 on error.
 */
int dsd_fsync(int fd);

/**
 * @brief Get file status.
 *
 * @param fd    File descriptor.
 * @param st    Output stat struct.
 * @return 0 on success, -1 on error.
 */
int dsd_fstat(int fd, dsd_stat_t* st);

/**
 * @brief Get file status for a path.
 *
 * @param path  File path.
 * @param st    Output stat struct.
 * @return 0 on success, -1 on error.
 */
int dsd_stat_path(const char* path, dsd_stat_t* st);

/**
 * @brief Check whether file status describes a regular file.
 *
 * @param st    File status struct.
 * @return Non-zero if regular file, 0 otherwise.
 */
int dsd_stat_is_regular(const dsd_stat_t* st);

/**
 * @brief Set file permissions (best effort on Windows).
 *
 * @param fd    File descriptor.
 * @param mode  POSIX permission bits.
 * @return 0 on success, -1 on error.
 */
int dsd_fchmod(int fd, int mode);

/**
 * @brief Open a file for writing/appending with owner-only permissions when created.
 *
 * Supports standard write/append modes such as "w", "wb", "a", "ab", "w+",
 * and "a+". Read-only modes fall back to normal fopen semantics.
 *
 * @param path  File path.
 * @param mode  fopen-compatible mode string.
 * @return FILE stream, or NULL on error.
 */
FILE* dsd_fopen_private(const char* path, const char* mode);

/**
 * @brief Create a private temporary sibling file for atomic replacement.
 *
 * The temporary file is created in the same directory as @p final_path with
 * owner-only permissions and exclusive creation. On success, @p tmp_path
 * receives the created file path and the returned stream owns the descriptor.
 *
 * @param final_path     Destination path that will later be replaced.
 * @param tmp_path       Output buffer for the created temporary path.
 * @param tmp_path_size  Output buffer size.
 * @param mode           fopen-compatible write mode such as "w" or "wb".
 * @return FILE stream, or NULL on error with errno set.
 */
FILE* dsd_fopen_private_temp_for_replace(const char* final_path, char* tmp_path, size_t tmp_path_size,
                                         const char* mode);

/**
 * @brief Atomically replace a destination path with a sibling temporary file.
 *
 * @param tmp_path    Existing temporary file created in the destination directory.
 * @param final_path  Destination path to replace.
 * @return 0 on success, -1 on error with errno set.
 */
int dsd_replace_file_with_temp(const char* tmp_path, const char* final_path);

/**
 * @brief Resolve a user-supplied local file name to an existing file in the current directory.
 *
 * The requested name must be a bare file name: no path separators, absolute
 * paths, empty names, or ".." segments. On success, @p out receives the
 * directory entry name for the existing regular file.
 *
 * @param requested  User-supplied bare file name.
 * @param out        Output buffer.
 * @param out_size   Output buffer size.
 * @return 0 on success, -1 on error with errno set.
 */
int dsd_resolve_existing_local_file(const char* requested, char* out, size_t out_size);

/**
 * @brief Open an existing regular file from the current directory by bare local file name.
 *
 * The requested name must be a bare file name: no path separators, absolute
 * paths, empty names, or ".." segments. On success, @p out receives the
 * directory entry name used to open the file.
 *
 * @param requested  User-supplied bare file name.
 * @param out        Output buffer.
 * @param out_size   Output buffer size.
 * @return Read stream for the existing regular file, or NULL with errno set.
 */
FILE* dsd_fopen_existing_local_file(const char* requested, char* out, size_t out_size);

/**
 * @brief Open an existing regular file by path for read-only access.
 *
 * The final path is opened without following symlinks/reparse points where the
 * platform exposes that protection. Only read modes such as "r" and "rb" are
 * accepted.
 *
 * @param path  Existing file path.
 * @param mode  fopen-compatible read-only mode.
 * @return Read stream for the existing regular file, or NULL with errno set.
 */
FILE* dsd_fopen_existing_regular_file(const char* path, const char* mode);

/**
 * @brief Read from a file descriptor.
 *
 * @param fd        File descriptor.
 * @param buf       Output buffer.
 * @param count     Bytes to read.
 * @return Number of bytes read, 0 on EOF, or -1 on error.
 */
ssize_t dsd_read(int fd, void* buf, size_t count);

/**
 * @brief Write to a file descriptor.
 *
 * @param fd        File descriptor.
 * @param buf       Data buffer.
 * @param count     Bytes to write.
 * @return Number of bytes written, or -1 on error.
 */
ssize_t dsd_write(int fd, const void* buf, size_t count);

/**
 * @brief Get the null device path for this platform.
 *
 * @return "/dev/null" on POSIX, "NUL" on Windows.
 */
const char* dsd_null_device(void);

/**
 * @brief Standard file descriptor numbers.
 */
#if DSD_PLATFORM_WIN_NATIVE
#include <io.h>
#define DSD_STDIN_FILENO  0
#define DSD_STDOUT_FILENO 1
#define DSD_STDERR_FILENO 2
#else
#include <unistd.h>
#define DSD_STDIN_FILENO  STDIN_FILENO
#define DSD_STDOUT_FILENO STDOUT_FILENO
#define DSD_STDERR_FILENO STDERR_FILENO
#endif

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_FILE_COMPAT_H_H */
