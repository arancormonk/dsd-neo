// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Lightweight pthread condition helpers shared across runtime components.
 */

#pragma once

#include <pthread.h>

/** @brief Signal a condition variable while holding its mutex. */
#define safe_cond_signal(n, m)                                                                                         \
    pthread_mutex_lock(m);                                                                                             \
    pthread_cond_signal(n);                                                                                            \
    pthread_mutex_unlock(m)

/** @brief Wait on a condition variable while holding its mutex. */
#define safe_cond_wait(n, m)                                                                                           \
    pthread_mutex_lock(m);                                                                                             \
    pthread_cond_wait(n, m);                                                                                           \
    pthread_mutex_unlock(m)
