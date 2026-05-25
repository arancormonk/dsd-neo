// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PLATFORM_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PLATFORM_H_H

/*
 * Platform detection macros for DSD-neo
 */

/* Detect Windows */
#if defined(_WIN32) || defined(_WIN64)
#define DSD_PLATFORM_WINDOWS 1
#else
#define DSD_PLATFORM_WINDOWS 0
#endif

/* Detect Linux */
#if defined(__linux__)
#define DSD_PLATFORM_LINUX 1
#else
#define DSD_PLATFORM_LINUX 0
#endif

/* Detect macOS */
#if defined(__APPLE__) && defined(__MACH__)
#define DSD_PLATFORM_MACOS 1
#else
#define DSD_PLATFORM_MACOS 0
#endif

/* Detect POSIX-like systems */
#if DSD_PLATFORM_LINUX || DSD_PLATFORM_MACOS || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define DSD_PLATFORM_POSIX 1
#else
#define DSD_PLATFORM_POSIX 0
#endif

/* Native Windows */
#if defined(_WIN32)
#define DSD_PLATFORM_WIN_NATIVE 1
#else
#define DSD_PLATFORM_WIN_NATIVE 0
#endif

/* Compiler detection */
#if defined(_MSC_VER)
#define DSD_COMPILER_MSVC 1
#else
#define DSD_COMPILER_MSVC 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define DSD_COMPILER_GCC 1
#else
#define DSD_COMPILER_GCC 0
#endif

#if defined(__clang__)
#define DSD_COMPILER_CLANG 1
#else
#define DSD_COMPILER_CLANG 0
#endif

/* Compiler attribute compatibility */
#if DSD_COMPILER_MSVC
#define DSD_ATTR_UNUSED
#define DSD_ATTR_USED
#define DSD_ATTR_NORETURN __declspec(noreturn)
#define DSD_ATTR_PACKED
#define DSD_ATTR_WEAK
#define DSD_ATTR_FORMAT(archetype, string_index, first_to_check)
#else
#define DSD_ATTR_UNUSED   __attribute__((unused))
#define DSD_ATTR_USED     __attribute__((used))
#define DSD_ATTR_NORETURN __attribute__((noreturn))
#define DSD_ATTR_PACKED   __attribute__((packed))
#define DSD_ATTR_WEAK     __attribute__((weak))
#define DSD_ATTR_FORMAT(archetype, string_index, first_to_check)                                                       \
    __attribute__((format(archetype, string_index, first_to_check)))
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_PLATFORM_H_H */
