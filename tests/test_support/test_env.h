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
 * the input a user exports, before it starts any thread that reads it. Each write moves the store's generation
 * (dsd_env_note_process_change()), so a reader that caches by it reads again.
 */

#include <dsd-neo/platform/env.h>
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
    const int rc = _putenv_s(name, value) == 0 ? 0 : -1;
#else
    const int rc = setenv(name, value, overwrite);
#endif
    dsd_env_note_process_change();
    return rc;
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
    const int rc = _putenv_s(name, "") == 0 ? 0 : -1;
#else
    const int rc = unsetenv(name);
#endif
    dsd_env_note_process_change();
    return rc;
}

#ifdef __cplusplus
}
#endif
