// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Path expansion for configuration values.
 *
 * Supports shell-like expansion:
 *   ~       -> $HOME or platform home directory
 *   $VAR    -> environment variable VAR
 *   ${VAR}  -> environment variable VAR (braced form)
 *
 * Missing variables expand to empty string (no error).
 */

#include <ctype.h>
#include <dsd-neo/runtime/config.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

/**
 * @brief Get the user's home directory.
 * @return Pointer to static buffer with home path, or NULL if unavailable.
 */
static const char*
get_home_dir(void) {
    static char home_buf[1024];
    static int cached = 0;

    if (cached) {
        return home_buf[0] ? home_buf : NULL;
    }

    home_buf[0] = '\0';
    cached = 1;

#if defined(_WIN32)
    /* Try USERPROFILE first, then HOMEDRIVE+HOMEPATH */
    const char* userprofile = getenv("USERPROFILE");
    if (userprofile && *userprofile) {
        DSD_SNPRINTF(home_buf, sizeof(home_buf), "%s", userprofile);
        home_buf[sizeof(home_buf) - 1] = '\0';
        return home_buf;
    }

    const char* homedrive = getenv("HOMEDRIVE");
    const char* homepath = getenv("HOMEPATH");
    if (homedrive && homepath) {
        DSD_SNPRINTF(home_buf, sizeof(home_buf), "%s%s", homedrive, homepath);
        home_buf[sizeof(home_buf) - 1] = '\0';
        return home_buf;
    }
#else
    /* Try $HOME first */
    const char* home = getenv("HOME");
    if (home && *home) {
        DSD_SNPRINTF(home_buf, sizeof(home_buf), "%s", home);
        home_buf[sizeof(home_buf) - 1] = '\0';
        return home_buf;
    }

    /* Fall back to passwd entry */
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir && *pw->pw_dir) {
        DSD_SNPRINTF(home_buf, sizeof(home_buf), "%s", pw->pw_dir);
        home_buf[sizeof(home_buf) - 1] = '\0';
        return home_buf;
    }
#endif

    return NULL;
}

/**
 * @brief Check if character is valid in an environment variable name.
 */
static int
is_var_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static int
copy_span_checked(char** dst, const char* dst_end, const char* src, size_t src_len) {
    if ((*dst + src_len) > dst_end) {
        return -1;
    }
    DSD_MEMCPY(*dst, src, src_len);
    *dst += src_len;
    return 0;
}

static int
is_tilde_expandable(const char* input, const char* src) {
    if (*src != '~') {
        return 0;
    }
    if (!(src == input || src[-1] == '/' || src[-1] == '\\')) {
        return 0;
    }

    char next = src[1];
    return (next == '\0' || next == '/' || next == '\\');
}

static int
try_expand_tilde(const char* input, const char** src, char** dst, const char* dst_end) {
    if (!is_tilde_expandable(input, *src)) {
        return 0;
    }

    const char* home = get_home_dir();
    if (home) {
        if (copy_span_checked(dst, dst_end, home, strlen(home)) != 0) {
            return -1;
        }
    }

    (*src)++;
    return 1;
}

static int
parse_env_reference(const char* src, const char** var_start, const char** var_end, int* braced) {
    *braced = 0;
    if (src[1] == '{') {
        *braced = 1;
        *var_start = src + 2;
        *var_end = strchr(*var_start, '}');
        if (!*var_end) {
            return 0;
        }
        return 1;
    }

    if (!is_var_char(src[1])) {
        return -1;
    }

    *var_start = src + 1;
    *var_end = *var_start;
    while (**var_end && is_var_char(**var_end)) {
        (*var_end)++;
    }
    return 1;
}

static int
copy_env_value(char** dst, const char* dst_end, const char* var_start, size_t var_len) {
    if (var_len == 0 || var_len >= 256) {
        return 0;
    }

    char var_name[256];
    DSD_MEMCPY(var_name, var_start, var_len);
    var_name[var_len] = '\0';

    const char* var_val = getenv(var_name);
    if (!var_val) {
        return 0;
    }

    return copy_span_checked(dst, dst_end, var_val, strlen(var_val));
}

static int
try_expand_env(const char** src, char** dst, const char* dst_end) {
    if (**src != '$') {
        return 0;
    }

    const char* var_start = NULL;
    const char* var_end = NULL;
    int braced = 0;
    int parsed = parse_env_reference(*src, &var_start, &var_end, &braced);
    if (parsed <= 0) {
        *(*dst)++ = *(*src)++;
        return 1;
    }

    if (copy_env_value(dst, dst_end, var_start, (size_t)(var_end - var_start)) != 0) {
        return -1;
    }

    *src = braced ? (var_end + 1) : var_end;
    return 1;
}

int
dsd_config_expand_path(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }

    const char* src = input;
    char* dst = output;
    char* dst_end = output + output_size - 1; /* leave room for NUL */

    while (*src && dst < dst_end) {
        int rc = try_expand_tilde(input, &src, &dst, dst_end);
        if (rc < 0) {
            output[output_size - 1] = '\0';
            return -1;
        }
        if (rc > 0) {
            continue;
        }

        rc = try_expand_env(&src, &dst, dst_end);
        if (rc < 0) {
            output[output_size - 1] = '\0';
            return -1;
        }
        if (rc > 0) {
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';

    /* Check for truncation */
    if (*src != '\0') {
        return -1;
    }

    return 0;
}
