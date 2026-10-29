// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Thread-safe environment overrides over the process environment (see env.h).
 *
 * The only reader of the process environment in the program: every other source reads through dsd_env_get(), and
 * nothing writes the process environment. The table is small and every read site is cold (config initialisation, a
 * stream start, a menu redraw, a grant's preempt check), so a mutex-guarded scan costs about what getenv()'s walk of
 * the whole environment did.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/threading.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* One interned value of a name. Never freed: a reader may still hold its text. */
typedef struct env_value {
    struct env_value* next;
    char* text;
} env_value;

/* A name that has held an override. Slots are never removed, so a name's pointer stays put too. */
typedef struct {
    char* name;
    env_value* interned;
    const char* value[DSD_ENV_LAYER_COUNT]; /* NULL with present set: hidden. */
    unsigned char present[DSD_ENV_LAYER_COUNT];
} env_slot;

static env_slot g_slots[DSD_ENV_MAX_NAMES];
static size_t g_slot_count;
static dsd_mutex_t g_mu;
static atomic_int g_mu_state = 0; /* 0 = uninitialised, 1 = initialising, 2 = ready */

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

/* Caller holds the lock. */
static env_slot*
env_find_locked(const char* name) {
    for (size_t i = 0; i < g_slot_count; i++) {
        if (strcmp(g_slots[i].name, name) == 0) {
            return &g_slots[i];
        }
    }
    return NULL;
}

/* Caller holds the lock. NULL when the table is full or out of memory. */
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

/* The slot's stored copy of `text`, made once. Caller holds the lock. NULL when out of memory. */
static const char*
env_intern_locked(env_slot* slot, const char* text) {
    for (const env_value* v = slot->interned; v != NULL; v = v->next) {
        if (strcmp(v->text, text) == 0) {
            return v->text;
        }
    }
    env_value* node = (env_value*)malloc(sizeof *node);
    char* copy = env_copy_text(text);
    if (node == NULL || copy == NULL) {
        free(node);
        free(copy);
        errno = ENOMEM;
        return NULL;
    }
    node->text = copy;
    node->next = slot->interned;
    slot->interned = node;
    return copy;
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
    env_slot* slot = env_find_or_add_locked(name);
    if (slot == NULL) {
        return -1;
    }
    const char* stored = NULL;
    if (value != NULL) {
        stored = env_intern_locked(slot, value);
        if (stored == NULL) {
            return -1;
        }
    }
    slot->value[layer] = stored;
    slot->present[layer] = 1;
    return 0;
}

const char*
dsd_env_get(const char* name) {
    if (!env_name_valid(name)) {
        return NULL;
    }
    env_lock();
    const env_slot* slot = env_find_locked(name);
    const int top = env_top_layer_locked(slot);
    const int overridden = slot != NULL && top >= 0;
    const char* value = overridden ? slot->value[top] : NULL;
    env_unlock();
    if (overridden) {
        return value;
    }
    return getenv(name);
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
    for (size_t i = 0; i < g_slot_count; i++) {
        g_slots[i].value[layer] = NULL;
        g_slots[i].present[layer] = 0;
    }
    env_unlock();
}
