// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#define DSD_NEO_THREADING_NO_INLINE_CREATE
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>

#if DSD_PLATFORM_WIN_NATIVE

#include <errno.h>
#include <limits.h>
#include <process.h>

int dsd_thread_create_impl(dsd_thread_t* thread, void* arg, dsd_thread_fn func);

/* The wrappers report errno values, as their POSIX counterparts do. Map the Windows error a failed call left behind to
 * the nearest one; anything without a closer match is reported as an invalid request. */
static int
dsd_win32_thread_errno(DWORD err) {
    switch (err) {
        case ERROR_TIMEOUT: return ETIMEDOUT;
        case ERROR_ACCESS_DENIED: return EPERM;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY: return ENOMEM;
        default: return EINVAL;
    }
}

/*============================================================================
 * Thread Functions
 *============================================================================*/

int
dsd_thread_create_impl(dsd_thread_t* thread, void* arg, dsd_thread_fn func) {
    if (!thread || !func) {
        return EINVAL;
    }

    /* _beginthreadex returns 0 on failure, handle otherwise */
    uintptr_t h = _beginthreadex(NULL, 0, func, arg, 0, NULL);
    if (h == 0) {
        return errno ? errno : EAGAIN;
    }
    *thread = (HANDLE)h;
    return 0;
}

int
dsd_thread_join(dsd_thread_t thread) {
    if (thread == NULL || thread == INVALID_HANDLE_VALUE) {
        return EINVAL;
    }
    DWORD result = WaitForSingleObject(thread, INFINITE);
    if (result == WAIT_OBJECT_0) {
        CloseHandle(thread);
        return 0;
    }
    return EINVAL;
}

dsd_thread_t
dsd_thread_self(void) {
    return GetCurrentThread();
}

/*============================================================================
 * Mutex Functions (using CRITICAL_SECTION for performance)
 *============================================================================*/

int
dsd_mutex_init(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    InitializeCriticalSection(mutex);
    return 0;
}

int
dsd_mutex_destroy(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    DeleteCriticalSection(mutex);
    return 0;
}

int
dsd_mutex_lock(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    EnterCriticalSection(mutex);
    return 0;
}

int
dsd_mutex_unlock(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    LeaveCriticalSection(mutex);
    return 0;
}

/*============================================================================
 * Condition Variable Functions (Vista+ CONDITION_VARIABLE)
 *============================================================================*/

int
dsd_cond_init(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    InitializeConditionVariable(cond);
    return 0;
}

int
dsd_cond_destroy(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    /* A Windows condition variable holds no resource to release. Leave it as InitializeConditionVariable() leaves a new
       one, so a destroyed variable is in a defined state. */
    InitializeConditionVariable(cond);
    return 0;
}

int
dsd_cond_wait(dsd_cond_t* cond, dsd_mutex_t* mutex) {
    if (!cond || !mutex) {
        return EINVAL;
    }
    if (!SleepConditionVariableCS(cond, mutex, INFINITE)) {
        return dsd_win32_thread_errno(GetLastError());
    }
    return 0;
}

int
dsd_cond_timedwait(dsd_cond_t* cond, dsd_mutex_t* mutex, unsigned int timeout_ms) {
    if (!cond || !mutex) {
        return EINVAL;
    }
    if (!SleepConditionVariableCS(cond, mutex, timeout_ms)) {
        return dsd_win32_thread_errno(GetLastError());
    }
    return 0;
}

int
dsd_cond_init_monotonic(dsd_cond_t* cond) {
    return dsd_cond_init(cond);
}

int
dsd_cond_timedwait_monotonic(dsd_cond_t* cond, dsd_mutex_t* mutex, uint64_t deadline_ns) {
    if (!cond || !mutex) {
        return EINVAL;
    }

    uint64_t now_ns = dsd_time_monotonic_ns();
    if (deadline_ns <= now_ns) {
        return ETIMEDOUT;
    }
    uint64_t remaining_ns = deadline_ns - now_ns;
    uint64_t wait_ms_u64 = (remaining_ns + 999999ULL) / 1000000ULL;
    if (wait_ms_u64 > 0xFFFFFFFFULL) {
        wait_ms_u64 = 0xFFFFFFFFULL;
    }
    DWORD timeout_ms = (DWORD)wait_ms_u64;

    if (!SleepConditionVariableCS(cond, mutex, timeout_ms)) {
        return dsd_win32_thread_errno(GetLastError());
    }
    return 0;
}

int
dsd_cond_signal(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    WakeConditionVariable(cond);
    return 0;
}

int
dsd_cond_broadcast(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    WakeAllConditionVariable(cond);
    return 0;
}

/*============================================================================
 * Thread Priority / Scheduling
 *============================================================================*/

int
dsd_thread_set_realtime_priority(int priority) {
    /*
     * Windows priority mapping:
     * priority < 0  -> THREAD_PRIORITY_BELOW_NORMAL
     * priority == 0 -> THREAD_PRIORITY_NORMAL
     * priority > 0  -> THREAD_PRIORITY_ABOVE_NORMAL to TIME_CRITICAL
     */
    int win_priority;

    if (priority <= -2) {
        win_priority = THREAD_PRIORITY_LOWEST;
    } else if (priority == -1) {
        win_priority = THREAD_PRIORITY_BELOW_NORMAL;
    } else if (priority == 0) {
        win_priority = THREAD_PRIORITY_NORMAL;
    } else if (priority == 1) {
        win_priority = THREAD_PRIORITY_ABOVE_NORMAL;
    } else if (priority == 2) {
        win_priority = THREAD_PRIORITY_HIGHEST;
    } else {
        win_priority = THREAD_PRIORITY_TIME_CRITICAL;
    }

    if (!SetThreadPriority(GetCurrentThread(), win_priority)) {
        return dsd_win32_thread_errno(GetLastError());
    }
    return 0;
}

int
dsd_thread_set_affinity(int cpu_index) {
    /* A thread's affinity mask holds one bit per CPU of its processor group; shifting past it is undefined. */
    if (cpu_index < 0 || cpu_index >= (int)(sizeof(DWORD_PTR) * CHAR_BIT)) {
        return EINVAL;
    }

    DWORD_PTR mask = (DWORD_PTR)1 << cpu_index;
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
        return dsd_win32_thread_errno(GetLastError());
    }
    return 0;
}

void
dsd_thread_yield(void) {
    /* SwitchToThread rather than Sleep(0): it will yield to a *lower*-priority
     * thread on this core, which is exactly the thread a spin is waiting for. */
    (void)SwitchToThread();
}

#endif /* DSD_PLATFORM_WIN_NATIVE */
