// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_ENV_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_ENV_H_

/**
 * @file
 * @brief In-process environment overrides: the only way the program reads or changes an environment variable.
 *
 * The process environment is not thread-safe: on Android's bionic, adding a variable can reallocate and free the
 * `environ` array another thread's getenv() is walking, and every localtime_r() there reads TZ (#782). So no code
 * writes the process environment once threads run. A CLI option, an INI key, a menu toggle or an embedding host
 * that wants a variable to read differently sets an override here instead, and every in-process reader goes through
 * dsd_env_get(), which falls back to the process environment, so exported variables keep working as inputs.
 *
 * Two layers, searched session first:
 * - DSD_ENV_LAYER_HOST: what an embedding host sets for the life of the process (Android's nativeInit).
 * - DSD_ENV_LAYER_SESSION: what one session's options, config and menus set. dsd_runtime_bootstrap() clears it, so
 *   a session never inherits the previous session's values in a long-lived process.
 *
 * Every function is thread-safe. A value pointer stays valid for the life of the process: values are interned per
 * name and never freed, so memory grows only with the number of distinct (name, value) pairs ever set.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Override layers, searched from the highest value down. */
typedef enum {
    DSD_ENV_LAYER_HOST = 0,    /**< Set by an embedding host for the life of the process. */
    DSD_ENV_LAYER_SESSION = 1, /**< Set by one session; cleared at every dsd_runtime_bootstrap(). */
    DSD_ENV_LAYER_COUNT = 2
} dsd_env_layer;

/** @brief The most names that can hold an override at once; a set of a further name fails. */
#define DSD_ENV_MAX_NAMES 256

/**
 * @brief The variable's value: the session override, else the host override, else the process environment.
 *
 * @param name Variable name.
 * @return The value, or NULL when it is unset or an override hides it. An override's pointer stays valid for the
 *         life of the process; a process-environment pointer is the C library's.
 */
const char* dsd_env_get(const char* name);

/**
 * @brief Set or hide a variable in one layer. The process environment is never written.
 *
 * @param layer Layer to write.
 * @param name  Variable name: non-empty, without '='.
 * @param value New value, or NULL to hide the name, so it reads as unset even if a lower layer or the process
 *              environment has it.
 * @return 0 on success; -1 for an invalid argument, a full table (DSD_ENV_MAX_NAMES) or no memory.
 */
int dsd_env_set(dsd_env_layer layer, const char* name, const char* value);

/**
 * @brief Set a variable in one layer only when it reads as unset, as setenv(name, value, 0) does.
 *
 * Checks every layer and the process environment under the store's lock, so a reader never sees a value this call
 * should not have replaced. A name an override hides reads as unset and is written.
 *
 * @return 0 when written or when the name was already set; -1 for an invalid argument, a NULL value, a full table
 *         or no memory.
 */
int dsd_env_set_if_unset(dsd_env_layer layer, const char* name, const char* value);

/** @brief Drop every override in one layer, so each name reads from the layers below and the process environment. */
void dsd_env_clear_layer(dsd_env_layer layer);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_ENV_H_ */
