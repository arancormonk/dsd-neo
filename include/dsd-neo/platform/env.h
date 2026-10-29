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
 * that wants a variable to read differently sets an override here instead, and every in-process reader copies the
 * value out through dsd_env_read() (or dsd_env_value in C++), which falls back to the process environment, so
 * exported variables keep working as inputs.
 *
 * Two layers, searched session first:
 * - DSD_ENV_LAYER_HOST: what an embedding host sets for the life of the process (Android's nativeInit).
 * - DSD_ENV_LAYER_SESSION: what one session's options, config and menus set. dsd_runtime_bootstrap() clears it, so
 *   a session never inherits the previous session's values in a long-lived process.
 *
 * Every function is thread-safe. A reader always gets a copy, never a pointer into the store, so the store owns its
 * values outright: a value that is replaced, hidden or cleared is zeroed (DSD_SECURE_ZERO) and freed at once, which
 * keeps a key or token typed into the terminal's environment editor from lingering in the heap.
 */

#include <stddef.h>

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

/** @brief Override layers, searched from the highest value down. */
typedef enum {
    DSD_ENV_LAYER_HOST = 0,    /**< Set by an embedding host for the life of the process. */
    DSD_ENV_LAYER_SESSION = 1, /**< Set by one session; cleared at every dsd_runtime_bootstrap(). */
    DSD_ENV_LAYER_COUNT = 2
} dsd_env_layer;

/**
 * @brief The most names that can hold an override at once, in either layer.
 *
 * A name whose overrides are all cleared frees its place. A set of a further name fails with ENOMEM.
 */
#define DSD_ENV_MAX_NAMES 256

/**
 * @brief Copy the variable's value: the session override, else the host override, else the process environment.
 *
 * @param name Variable name.
 * @param buf  Destination, always NUL-terminated when @p cap > 0. May be NULL with @p cap 0 to size a value.
 * @param cap  Size of @p buf in bytes.
 * @return 0 when the name is unset or an override hides it (and @p buf is ""); otherwise the value's length plus one.
 *         A result above @p cap means the value was cut to @p cap - 1 bytes.
 */
size_t dsd_env_copy(const char* name, char* buf, size_t cap);

/**
 * @brief The variable's value in @p buf, or NULL when it is unset or hidden.
 *
 * A value too long for @p buf also gives NULL, with errno set to ERANGE: size @p buf for the longest value the caller
 * accepts (a number needs little, a path DSD_ENV_PATH_MAX).
 */
const char* dsd_env_read(const char* name, char* buf, size_t cap);

/** @brief A buffer size for path-valued variables (HOME, DSD_NEO_CONFIG, ...). */
#define DSD_ENV_PATH_MAX 4096

/**
 * @brief Set or hide a variable in one layer. The process environment is never written.
 *
 * @param layer Layer to write.
 * @param name  Variable name: non-empty, without '='.
 * @param value New value, or NULL to hide the name, so it reads as unset even if a lower layer or the process
 *              environment has it.
 * @return 0 on success; -1 with errno EINVAL for an invalid layer or name, or ENOMEM for a full table
 *         (DSD_ENV_MAX_NAMES) or no memory. On failure the name reads as it did.
 */
int dsd_env_set(dsd_env_layer layer, const char* name, const char* value);

/**
 * @brief Set a variable in one layer only when it reads as unset, as setenv(name, value, 0) does.
 *
 * Checks every layer and the process environment under the store's lock, so a reader never sees a value this call
 * should not have replaced. A name an override hides reads as unset and is written.
 *
 * @return 0 when written or when the name was already set; -1 as for dsd_env_set() (EINVAL also for a NULL value).
 */
int dsd_env_set_if_unset(dsd_env_layer layer, const char* name, const char* value);

/** @brief Drop every override in one layer, so each name reads from the layers below and the process environment. */
void dsd_env_clear_layer(dsd_env_layer layer);

/**
 * @brief A counter that changes whenever an override changes; reading it takes no lock.
 *
 * A hot reader caches what it read with the generation it read at, and reads again only when the generation moved.
 * Nothing in the program writes the process environment; a test that does calls dsd_env_note_process_change().
 */
unsigned int dsd_env_generation(void);

/** @brief Move the generation after a test wrote the process environment, so cached reads are refreshed. */
void dsd_env_note_process_change(void);

#ifdef __cplusplus
}

/** @brief A copy of a variable's value for C++ readers: c_str() is NULL when the variable is unset or hidden. */
class dsd_env_value {
  public:
    explicit dsd_env_value(const char* name) {
        char small[128];
        size_t need = dsd_env_copy(name, small, sizeof small);
        if (need == 0) {
            return;
        }
        if (need <= sizeof small) {
            text_.assign(small, need - 1U);
            set_ = true;
            return;
        }
        /* Longer than the stack buffer: size and copy again until the value stops changing under us. */
        while (need > 0) {
            text_.assign(need, '\0');
            const size_t got = dsd_env_copy(name, &text_[0], text_.size());
            if (got == 0) {
                text_.clear();
                return;
            }
            if (got <= need) {
                text_.resize(got - 1U);
                set_ = true;
                return;
            }
            need = got;
        }
    }

    /** @brief The value, or NULL when unset or hidden. Valid as long as this object. */
    const char*
    c_str() const {
        return set_ ? text_.c_str() : nullptr;
    }

  private:
    std::string text_;
    bool set_ = false;
};
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_ENV_H_ */
