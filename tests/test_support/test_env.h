// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/*
 * Process-environment writers for tests only (#782).
 *
 * Production code never writes the process environment: a running program overrides a variable through
 * <dsd-neo/platform/env.h>, and Semgrep bans the writers outside tests. A test still seeds the real environment,
 * the input a user exports, before it starts any thread that reads it.
 */

#include <dsd-neo/platform/platform.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief setenv(): 0 on success, -1 on error. */
static inline int
dsd_test_setenv(const char* name, const char* value, int overwrite) {
    if (name == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
#if DSD_PLATFORM_WIN_NATIVE
    if (!overwrite) {
        size_t required = 0;
        if (getenv_s(&required, NULL, 0, name) == 0 && required > 0) {
            return 0;
        }
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
#else
    return setenv(name, value, overwrite);
#endif
}

/** @brief unsetenv(): 0 on success, -1 on error. */
static inline int
dsd_test_unsetenv(const char* name) {
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }
#if DSD_PLATFORM_WIN_NATIVE
    /* An empty value removes the variable. */
    return _putenv_s(name, "") == 0 ? 0 : -1;
#else
    return unsetenv(name);
#endif
}

#ifdef __cplusplus
}
#endif
