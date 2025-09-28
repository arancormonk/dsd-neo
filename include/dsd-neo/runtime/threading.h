// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Lightweight pthread condition helpers shared across runtime components.
 */

#pragma once

#include <pthread.h>

#define safe_cond_signal(n, m)                                                                                         \
    pthread_mutex_lock(m);                                                                                             \
    pthread_cond_signal(n);                                                                                            \
    pthread_mutex_unlock(m)

#define safe_cond_wait(n, m)                                                                                           \
    pthread_mutex_lock(m);                                                                                             \
    pthread_cond_wait(n, m);                                                                                           \
    pthread_mutex_unlock(m)
