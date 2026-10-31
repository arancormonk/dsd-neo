// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Thread-safe environment overrides over the process environment (see env.h).
 *
 * The only reader of the process environment in the program: every other source copies values out through
 * dsd_env_copy(), and nothing writes the process environment. Readers never hold a pointer into the store, so a
 * replaced or cleared value is zeroed and freed at once, and a name with no override left frees its slot. The table
 * is small and every read site is cold (config initialisation, a stream start, a menu redraw, a preempt that would
 * displace a call); the one per-line reader, the Unicode check, caches by dsd_env_generation().
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h> // IWYU pragma: keep (dsd_strcasecmp on native Windows)
#include <dsd-neo/platform/threading.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* A name with at least one override. value[layer] is NULL with present[layer] set when the layer hides the name. */
typedef struct {
    char* name;
    char* value[DSD_ENV_LAYER_COUNT];
    unsigned char present[DSD_ENV_LAYER_COUNT];
} env_slot;

static env_slot g_slots[DSD_ENV_MAX_NAMES];
static size_t g_slot_count;
static dsd_mutex_t g_mu;
static atomic_int g_mu_state = 0; /* 0 = uninitialised, 1 = initialising, 2 = ready */
static atomic_int g_generation = 0;

static void
env_lock(void) {
    if (atomic_load(&g_mu_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_mu_state, &expected, 1)) {
            (void)dsd_mutex_init(&g_mu);
            atomic_store(&g_mu_state, 2);
        } else {
            while (atomic_load(&g_mu_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    (void)dsd_mutex_lock(&g_mu);
}

static void
env_unlock(void) {
    (void)dsd_mutex_unlock(&g_mu);
}

static int
env_name_valid(const char* name) {
    return name != NULL && name[0] != '\0' && strchr(name, '=') == NULL;
}

static int
env_layer_valid(dsd_env_layer layer) {
    return layer == DSD_ENV_LAYER_HOST || layer == DSD_ENV_LAYER_SESSION;
}

static char*
env_copy_text(const char* text) {
    const size_t len = strlen(text);
    char* copy = (char*)malloc(len + 1U);
    if (copy != NULL) {
        DSD_MEMCPY(copy, text, len + 1U);
    }
    return copy;
}

/* Zero a value before freeing it: an editor entry may be a key or token. */
static void
env_free_text(char* text) {
    if (text != NULL) {
        DSD_SECURE_ZERO(text, strlen(text));
        free(text);
    }
}

/* Whether two variable names are the same variable: case-insensitive on Windows, as the CRT environment the store
 * falls back to (and the editor's name check) are there, so an override set as dsd_neo_auto_ppm is read as
 * DSD_NEO_AUTO_PPM. */
static int
env_name_equal(const char* a, const char* b) {
#if DSD_PLATFORM_WIN_NATIVE
    return dsd_strcasecmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

/* Caller holds the lock. */
static env_slot*
env_find_locked(const char* name) {
    for (size_t i = 0; i < g_slot_count; i++) {
        if (env_name_equal(g_slots[i].name, name)) {
            return &g_slots[i];
        }
    }
    return NULL;
}

/* Caller holds the lock. NULL with errno ENOMEM when the table is full or out of memory. */
static env_slot*
env_find_or_add_locked(const char* name) {
    env_slot* slot = env_find_locked(name);
    if (slot != NULL) {
        return slot;
    }
    if (g_slot_count >= DSD_ENV_MAX_NAMES) {
        errno = ENOMEM;
        return NULL;
    }
    char* copy = env_copy_text(name);
    if (copy == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    slot = &g_slots[g_slot_count];
    DSD_MEMSET(slot, 0, sizeof *slot);
    slot->name = copy;
    g_slot_count++;
    return slot;
}

/* Drop the slot when no layer holds an override any more, so the table counts live names. Caller holds the lock. */
static void
env_release_if_empty_locked(env_slot* slot) {
    for (int layer = 0; layer < DSD_ENV_LAYER_COUNT; layer++) {
        if (slot->present[layer]) {
            return;
        }
    }
    free(slot->name);
    env_slot* last = &g_slots[g_slot_count - 1U];
    if (slot != last) {
        *slot = *last;
    }
    DSD_MEMSET(last, 0, sizeof *last);
    g_slot_count--;
}

/* The highest layer that has an override for the slot, or -1. Caller holds the lock. */
static int
env_top_layer_locked(const env_slot* slot) {
    if (slot == NULL) {
        return -1;
    }
    for (int layer = DSD_ENV_LAYER_COUNT - 1; layer >= 0; layer--) {
        if (slot->present[layer]) {
            return layer;
        }
    }
    return -1;
}

/* Caller holds the lock and has validated the arguments. */
static int
env_store_locked(dsd_env_layer layer, const char* name, const char* value) {
    char* copy = NULL;
    if (value != NULL) {
        copy = env_copy_text(value);
        if (copy == NULL) {
            errno = ENOMEM;
            return -1;
        }
    }
    env_slot* slot = env_find_or_add_locked(name);
    if (slot == NULL) {
        env_free_text(copy);
        return -1;
    }
    env_free_text(slot->value[layer]);
    slot->value[layer] = copy;
    slot->present[layer] = 1;
    (void)atomic_fetch_add(&g_generation, 1);
    return 0;
}

/* Copy `text` (NULL: unset) into buf as dsd_env_copy() documents. */
static size_t
env_copy_out(const char* text, char* buf, size_t cap) {
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
    if (text == NULL) {
        return 0;
    }
    const size_t len = strlen(text);
    if (buf != NULL && cap > 0) {
        const size_t n = len < cap ? len : cap - 1U;
        DSD_MEMCPY(buf, text, n);
        buf[n] = '\0';
    }
    return len + 1U;
}

size_t
dsd_env_copy(const char* name, char* buf, size_t cap) {
    if (!env_name_valid(name)) {
        return env_copy_out(NULL, buf, cap);
    }
    env_lock();
    const env_slot* slot = env_find_locked(name);
    const int top = env_top_layer_locked(slot);
    if (slot != NULL && top >= 0) {
        const size_t rc = env_copy_out(slot->value[top], buf, cap);
        env_unlock();
        return rc;
    }
    env_unlock();
    /* No override: the process environment, which nothing in the program writes. */
    return env_copy_out(getenv(name), buf, cap);
}

const char*
dsd_env_read(const char* name, char* buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        errno = EINVAL;
        return NULL;
    }
    const size_t need = dsd_env_copy(name, buf, cap);
    if (need == 0) {
        return NULL;
    }
    if (need > cap) {
        buf[0] = '\0';
        errno = ERANGE;
        return NULL;
    }
    return buf;
}

int
dsd_env_set(dsd_env_layer layer, const char* name, const char* value) {
    if (!env_layer_valid(layer) || !env_name_valid(name)) {
        errno = EINVAL;
        return -1;
    }
    env_lock();
    const int rc = env_store_locked(layer, name, value);
    env_unlock();
    return rc;
}

int
dsd_env_set_if_unset(dsd_env_layer layer, const char* name, const char* value) {
    if (!env_layer_valid(layer) || !env_name_valid(name) || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    env_lock();
    const env_slot* slot = env_find_locked(name);
    const int top = env_top_layer_locked(slot);
    int is_set = 0;
    if (slot != NULL && top >= 0) {
        is_set = slot->value[top] != NULL;
    } else {
        /* Under the lock, so no override of this name can land between the check and the write. */
        is_set = getenv(name) != NULL;
    }
    const int rc = is_set ? 0 : env_store_locked(layer, name, value);
    env_unlock();
    return rc;
}

void
dsd_env_clear_layer(dsd_env_layer layer) {
    if (!env_layer_valid(layer)) {
        return;
    }
    env_lock();
    /* Backwards: releasing a slot moves the last one into its place. */
    for (size_t i = g_slot_count; i > 0; i--) {
        env_slot* slot = &g_slots[i - 1U];
        env_free_text(slot->value[layer]);
        slot->value[layer] = NULL;
        slot->present[layer] = 0;
        env_release_if_empty_locked(slot);
    }
    (void)atomic_fetch_add(&g_generation, 1);
    env_unlock();
}

unsigned int
dsd_env_generation(void) {
    return (unsigned int)atomic_load(&g_generation);
}

void
dsd_env_note_process_change(void) {
    (void)atomic_fetch_add(&g_generation, 1);
}
