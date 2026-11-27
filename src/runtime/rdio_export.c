// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/utf16.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/nonce.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "curl_common.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"
#include "rdio_export_internal.h"

#ifdef USE_CURL
#include <curl/curl.h>
#include <curl/system.h>
#endif

#define DSD_RDIO_PATH_MAX         2048
#define DSD_RDIO_UPLOAD_QUEUE_MAX 128U

/* A path's final component under @p win's rules: the file name the publisher made. */
static const char*
dsd_rdio_path_file_name(const char* path, int win) {
    const char* name = path;
    for (const char* p = path; *p != '\0'; p++) {
        if (*p == '/' || (win && *p == '\\')) {
            name = p + 1;
        }
    }
    return name;
}

int
dsd_rdio_upload_names_same_file(const char* job_path, const char* path, int win) {
    if (job_path == NULL || path == NULL) {
        return 0;
    }
    const unsigned char* a = (const unsigned char*)dsd_rdio_path_file_name(job_path, win);
    const unsigned char* b = (const unsigned char*)dsd_rdio_path_file_name(path, win);
    if (*b == '\0') {
        return 0;
    }
    for (;; a++, b++) {
        unsigned char ca = *a;
        unsigned char cb = *b;
        if (win) {
            ca = (ca >= 'A' && ca <= 'Z') ? (unsigned char)(ca - 'A' + 'a') : ca;
            cb = (cb >= 'A' && cb <= 'Z') ? (unsigned char)(cb - 'A' + 'a') : cb;
        }
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
    }
}

static int dsd_rdio_path_with_suffix(const char* wav_path, const char* suffix, char* out, size_t out_size);

/* A call not uploaded for want of a system ID is no export failure: said once a decoder run (dsd_rdio_session_reset()),
   whether this build uploads or not (issue #931). */
static atomic_int g_rdio_no_system_warned = 0;

static void
dsd_rdio_note_no_system(void) {
    if (atomic_exchange(&g_rdio_no_system_warned, 1) == 0) {
        LOG_WARN("Rdio API upload skipped: no system ID (set --rdio-system-id); later calls without one are "
                 "skipped silently\n");
    }
}

#ifdef USE_CURL
typedef struct {
    int system_id;
    int upload_timeout_ms;
    int upload_retries;
    char api_url[1024];
    char api_key[256];
} dsd_rdio_api_config;

typedef struct dsd_rdio_upload_job {
    dsd_rdio_api_config api;
    int remove_meta_on_success;
    int remove_wav_on_success;
    /* 1 when wav_path and meta_path are the job's own private inputs (--rdio-mode both), which the job's release
       removes however it ends; the two removals on success are then off. */
    int private_inputs;
    /* What the upload reads: the published pair, or the job's private copies of it. */
    char wav_path[DSD_RDIO_PATH_MAX];
    char meta_path[DSD_RDIO_PATH_MAX];
    /* The published WAV, the file dsd_rdio_upload_holds() names whatever the upload reads. */
    char public_wav_path[DSD_RDIO_PATH_MAX];
    struct dsd_rdio_upload_job* next;
} dsd_rdio_upload_job;

/* Seen by every upload job release (dsd_rdio_upload_job_free()), after the job's API key and URL copies are wiped and
 * before it is freed; NULL but in tests (rdio_export_internal.h). */
static dsd_rdio_upload_release_observer g_rdio_upload_release_observer = NULL;

void
dsd_rdio_upload_set_release_observer(dsd_rdio_upload_release_observer observer) {
    g_rdio_upload_release_observer = observer;
}

static void
dsd_rdio_remove_after_upload(const char* path, const char* kind) {
    if (!path || path[0] == '\0') {
        return;
    }
    if (dsd_remove_file(path) != 0 && errno != ENOENT) {
        LOG_WARN("Rdio API upload: failed to delete %s %s: %s\n", kind ? kind : "file", path, strerror(errno));
    }
}

/* Once a decoder run (dsd_rdio_session_reset()): a private input left behind, delete-after-upload ignored beside
   DirWatch; and, in every build, a call not uploaded for want of a system ID (g_rdio_no_system_warned). */
static atomic_int g_rdio_private_remove_warned = 0;
static atomic_int g_rdio_delete_after_upload_dirwatch_warned = 0;

void
dsd_rdio_session_reset(void) {
    atomic_store(&g_rdio_private_remove_warned, 0);
    atomic_store(&g_rdio_no_system_warned, 0);
    atomic_store(&g_rdio_delete_after_upload_dirwatch_warned, 0);
}

/* A private input of a job that is over, removed. One that cannot be (on Windows, while a DirWatch still holds the
   public WAV open without delete sharing, its hard link too) is left: no retry, said once a run, with how to remove
   such files by hand. */
static void
dsd_rdio_remove_private_input(const char* path) {
    if (path[0] == '\0' || dsd_remove_file(path) == 0 || errno == ENOENT) {
        return;
    }
    if (atomic_exchange(&g_rdio_private_remove_warned, 1) == 0) {
        LOG_WARN("Rdio API upload: failed to delete the private upload input %s: %s; files ending in "
                 ".rdio-upload-audio or .rdio-upload-meta in a recording folder no dsd-neo is exporting to can be "
                 "deleted\n",
                 path, strerror(errno));
    }
}

/* An upload job's end, sent, failed, dropped or abandoned at the stop, or never queued: its private inputs go
 * (dsd_rdio_remove_private_input()), and its copies of the API key and URL (a URL can carry credentials) are wiped
 * before the memory goes back. */
static void
dsd_rdio_upload_job_free(dsd_rdio_upload_job* job) {
    if (!job) {
        return;
    }
    if (job->private_inputs) {
        dsd_rdio_remove_private_input(job->wav_path);
        dsd_rdio_remove_private_input(job->meta_path);
    }
    DSD_SECURE_ZERO(&job->api, sizeof job->api);
    if (g_rdio_upload_release_observer) {
        g_rdio_upload_release_observer(job->api.api_key, sizeof job->api.api_key, job->api.api_url,
                                       sizeof job->api.api_url);
    }
    free(job);
}

/* The queue's mutex lives for the whole process, unlike the worker and its condition variable: the recording store
 * asks dsd_rdio_upload_holds() from its own thread at any time, a shutdown included. 0 = not initialized,
 * 1 = initializing, 2 = ready, 3 = its init failed, for good: no worker starts and nothing is held. */
static dsd_mutex_t g_rdio_upload_mutex;
static atomic_int g_rdio_upload_mutex_state = 0;
static dsd_cond_t g_rdio_upload_cond;
static dsd_thread_t g_rdio_upload_worker;
static dsd_rdio_upload_job* g_rdio_upload_head = NULL;
static dsd_rdio_upload_job* g_rdio_upload_tail = NULL;
static size_t g_rdio_upload_depth = 0;
/* The job the worker took off the queue, from then until its upload and any delete after it are over. */
static const dsd_rdio_upload_job* g_rdio_upload_in_flight = NULL;
static int g_rdio_upload_stop_requested = 0;
static atomic_int g_rdio_upload_state = 0; // 0=uninitialized, 1=initializing, 2=ready, 3=failed, 4=stopping
/* #718 D4: the user's Stop abandons the pending uploads instead of draining them. Atomic and lock-free, because the
 * request may come from any thread at any time, including while shutdown is joining the worker or before the worker's
 * mutex exists. The worker loop, the retry loop and the transfer's progress callback poll it. */
static atomic_int g_rdio_upload_cancel = 0;
/* Uploads the cancel cut off mid-attempt. Written by the worker, read by shutdown after the join. */
static size_t g_rdio_upload_aborted = 0;

#define DSD_RDIO_UPLOAD_CANCELLED (-2)
/* A job the queue does not admit: the shutdown has begun, or the queue is full. */
#define DSD_RDIO_UPLOAD_REFUSED   (-3)
/* A call with no rdio-scanner system to upload to: skipped, no failure (dsd_rdio_validate_api_config()). */
#define DSD_RDIO_UPLOAD_NO_SYSTEM (-4)
/* The pause between two attempts of one upload. */
#define DSD_RDIO_RETRY_BACKOFF_MS 150U

/* The files one upload reads, and the file names its form parts carry: NULL for the file's own name. */
typedef struct {
    const char* wav_path;
    const char* meta_path;
    const char* wav_name;
    const char* meta_name;
} dsd_rdio_upload_inputs;

static int dsd_rdio_upload_trunk_recorder(const dsd_rdio_api_config* api, const dsd_rdio_upload_inputs* inputs);

/* 0 when the queue's mutex is ready, -1 when its init failed. */
static int
dsd_rdio_upload_mutex_ensure(void) {
    int state = atomic_load(&g_rdio_upload_mutex_state);
    if (state == 2) {
        return 0;
    }
    if (state == 3) {
        return -1;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_rdio_upload_mutex_state, &expected, 1)) {
        const int ok = dsd_mutex_init(&g_rdio_upload_mutex) == 0;
        atomic_store(&g_rdio_upload_mutex_state, ok ? 2 : 3);
        return ok ? 0 : -1;
    }
    while ((state = atomic_load(&g_rdio_upload_mutex_state)) == 1) {
        dsd_thread_yield();
    }
    return state == 2 ? 0 : -1;
}

/* What @p job uploads. Private inputs go up under the published pair's names, so the server files the call as it would
   the pair itself; @p meta_name_buf holds the sidecar's. */
static void
dsd_rdio_job_upload_inputs(const dsd_rdio_upload_job* job, dsd_rdio_upload_inputs* out, char* meta_name_buf,
                           size_t meta_name_buf_size) {
    const int win = DSD_PLATFORM_WIN_NATIVE;
    out->wav_path = job->wav_path;
    out->meta_path = job->meta_path;
    out->wav_name = NULL;
    out->meta_name = NULL;
    if (!job->private_inputs) {
        return;
    }
    out->wav_name = dsd_rdio_path_file_name(job->public_wav_path, win);
    if (dsd_rdio_path_with_suffix(job->public_wav_path, ".json", meta_name_buf, meta_name_buf_size) == 0) {
        out->meta_name = dsd_rdio_path_file_name(meta_name_buf, win);
    }
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    dsd_rdio_upload_worker_thread(void* arg) {
    (void)arg;

    for (;;) {
        dsd_mutex_lock(&g_rdio_upload_mutex);
        while (g_rdio_upload_head == NULL && !g_rdio_upload_stop_requested) {
            dsd_cond_wait(&g_rdio_upload_cond, &g_rdio_upload_mutex);
        }

        /* A cancelled worker takes no further jobs; shutdown counts and frees what is left after the join. The cancel
         * needs no wakeup of its own: with jobs queued the worker is not waiting, and with none there is nothing to
         * abandon and shutdown's broadcast ends the wait. */
        if (atomic_load(&g_rdio_upload_cancel) != 0 || (g_rdio_upload_head == NULL && g_rdio_upload_stop_requested)) {
            dsd_mutex_unlock(&g_rdio_upload_mutex);
            break;
        }

        dsd_rdio_upload_job* job = g_rdio_upload_head;
        g_rdio_upload_head = job->next;
        if (g_rdio_upload_head == NULL) {
            g_rdio_upload_tail = NULL;
        }
        if (g_rdio_upload_depth > 0) {
            g_rdio_upload_depth--;
        }
        g_rdio_upload_in_flight = job;
        dsd_mutex_unlock(&g_rdio_upload_mutex);

        char meta_name[DSD_RDIO_PATH_MAX];
        dsd_rdio_upload_inputs inputs;
        dsd_rdio_job_upload_inputs(job, &inputs, meta_name, sizeof meta_name);
        const int upload_rc = dsd_rdio_upload_trunk_recorder(&job->api, &inputs);
        if (upload_rc == 0) {
            if (job->remove_meta_on_success) {
                dsd_rdio_remove_after_upload(job->meta_path, "metadata");
            }
            if (job->remove_wav_on_success) {
                dsd_rdio_remove_after_upload(job->wav_path, "audio");
            }
        } else if (upload_rc == DSD_RDIO_UPLOAD_CANCELLED) {
            g_rdio_upload_aborted++;
        }
        dsd_mutex_lock(&g_rdio_upload_mutex);
        g_rdio_upload_in_flight = NULL;
        dsd_mutex_unlock(&g_rdio_upload_mutex);
        dsd_rdio_upload_job_free(job);
    }

    DSD_THREAD_RETURN;
}

static int
dsd_rdio_upload_worker_init(void) {
    int state = atomic_load(&g_rdio_upload_state);
    if (state == 2) {
        return 0;
    }
    if (state == 3 || state == 4) {
        return -1;
    }

    int expected = 0;
    if (atomic_compare_exchange_strong(&g_rdio_upload_state, &expected, 1)) {
        if (dsd_rdio_upload_mutex_ensure() != 0 || dsd_cond_init(&g_rdio_upload_cond) != 0) {
            atomic_store(&g_rdio_upload_state, 3);
            return -1;
        }

        dsd_mutex_lock(&g_rdio_upload_mutex);
        g_rdio_upload_head = NULL;
        g_rdio_upload_tail = NULL;
        g_rdio_upload_depth = 0;
        g_rdio_upload_in_flight = NULL;
        g_rdio_upload_stop_requested = 0;
        g_rdio_upload_aborted = 0;
        dsd_mutex_unlock(&g_rdio_upload_mutex);

        if (dsd_thread_create(&g_rdio_upload_worker, dsd_rdio_upload_worker_thread, NULL) != 0) {
            (void)dsd_cond_destroy(&g_rdio_upload_cond);
            atomic_store(&g_rdio_upload_state, 3);
            return -1;
        }

        atomic_store(&g_rdio_upload_state, 2);
        return 0;
    }

    for (;;) {
        state = atomic_load(&g_rdio_upload_state);
        if (state == 2) {
            return 0;
        }
        if (state == 3) {
            return -1;
        }
        if (state == 4) {
            return -1;
        }
        dsd_sleep_ms(1U);
    }
}

/* Empty the queue, handing back its jobs for dsd_rdio_free_upload_jobs() after the unlock: their release removes
   files, which the recording store's dsd_rdio_upload_holds() should not wait behind. */
static dsd_rdio_upload_job*
dsd_rdio_detach_upload_queue_locked(void) {
    dsd_rdio_upload_job* jobs = g_rdio_upload_head;
    g_rdio_upload_head = NULL;
    g_rdio_upload_tail = NULL;
    g_rdio_upload_depth = 0;
    return jobs;
}

static void
dsd_rdio_free_upload_jobs(dsd_rdio_upload_job* jobs) {
    while (jobs != NULL) {
        dsd_rdio_upload_job* next = jobs->next;
        dsd_rdio_upload_job_free(jobs);
        jobs = next;
    }
}

void
dsd_rdio_upload_cancel(void) {
    atomic_store(&g_rdio_upload_cancel, 1);
}

void
dsd_rdio_upload_cancel_reset(void) {
    atomic_store(&g_rdio_upload_cancel, 0);
}

void
dsd_rdio_upload_shutdown(void) {
    for (;;) {
        int state = atomic_load(&g_rdio_upload_state);
        if (state == 0 || state == 3) {
            /* No worker to stop: the session that a cancel was meant for is over all the same. */
            atomic_store(&g_rdio_upload_cancel, 0);
            return;
        }
        if (state == 1 || state == 4) {
            dsd_sleep_ms(1U);
            continue;
        }

        int expected = 2;
        if (atomic_compare_exchange_strong(&g_rdio_upload_state, &expected, 4)) {
            break;
        }
    }

    dsd_mutex_lock(&g_rdio_upload_mutex);
    g_rdio_upload_stop_requested = 1;
    dsd_cond_broadcast(&g_rdio_upload_cond);
    dsd_mutex_unlock(&g_rdio_upload_mutex);

    if (dsd_thread_join(g_rdio_upload_worker) != 0) {
        /* Not expected: the handle comes from the create that set the state to 2, only the shutdown that moved it to 4
           joins, and the worker never joins itself. If it happens the worker may still be running, so nothing it can
           reach unlocked is touched: the job in flight and the worker's own stack copy of the endpoint keep the key and
           URL until it ends them. The queued jobs are the worker's only through the queue, which it pops under the
           mutex: detached under it, they are released here (their private inputs, holds and API copies), and the
           worker finds nothing more to take. */
        LOG_ERROR("Rdio export: failed to join upload worker during shutdown\n");
        dsd_mutex_lock(&g_rdio_upload_mutex);
        dsd_rdio_upload_job* queued = dsd_rdio_detach_upload_queue_locked();
        dsd_mutex_unlock(&g_rdio_upload_mutex);
        dsd_rdio_free_upload_jobs(queued);
        atomic_store(&g_rdio_upload_state, 3);
        atomic_store(&g_rdio_upload_cancel, 0);
        return;
    }

    dsd_mutex_lock(&g_rdio_upload_mutex);
    /* After a drain the queue is empty; anything left was abandoned by a cancel. */
    const size_t not_sent = g_rdio_upload_depth + g_rdio_upload_aborted;
    dsd_rdio_upload_job* dropped = dsd_rdio_detach_upload_queue_locked();
    g_rdio_upload_stop_requested = 0;
    g_rdio_upload_aborted = 0;
    dsd_mutex_unlock(&g_rdio_upload_mutex);
    dsd_rdio_free_upload_jobs(dropped);
    if (not_sent > 0U) {
        LOG_WARN("Rdio API upload: stopped by the user; %lu pending upload(s) not sent, their files kept\n",
                 (unsigned long)not_sent);
    }
    atomic_store(&g_rdio_upload_cancel, 0);

    (void)dsd_cond_destroy(&g_rdio_upload_cond);

    atomic_store(&g_rdio_upload_state, 0);
}

int
dsd_rdio_upload_holds(const char* path) {
    const int win = DSD_PLATFORM_WIN_NATIVE;
    if (path == NULL || dsd_rdio_path_file_name(path, win)[0] == '\0' || dsd_rdio_upload_mutex_ensure() != 0) {
        return 0;
    }
    dsd_mutex_lock(&g_rdio_upload_mutex);
    int held = g_rdio_upload_in_flight != NULL
               && dsd_rdio_upload_names_same_file(g_rdio_upload_in_flight->public_wav_path, path, win);
    for (const dsd_rdio_upload_job* job = g_rdio_upload_head; job != NULL && !held; job = job->next) {
        held = dsd_rdio_upload_names_same_file(job->public_wav_path, path, win);
    }
    dsd_mutex_unlock(&g_rdio_upload_mutex);
    return held;
}

/* @p system_id is the call's own system (dsd_rdio_resolve_call_system()), resolved before the settings are validated: an
   analog override uploads even when the base system is 0. */
static void
dsd_rdio_copy_api_config(dsd_rdio_api_config* out, const dsd_opts* opts, int system_id) {
    if (!out) {
        return;
    }

    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts) {
        return;
    }

    out->system_id = system_id;
    out->upload_timeout_ms = opts->rdio_upload_timeout_ms;
    out->upload_retries = opts->rdio_upload_retries;
    DSD_SNPRINTF(out->api_url, sizeof(out->api_url), "%s",
                 opts->rdio_api_url[0] ? opts->rdio_api_url : "http://127.0.0.1:3000");
    out->api_url[sizeof(out->api_url) - 1] = '\0';
    DSD_SNPRINTF(out->api_key, sizeof(out->api_key), "%s", opts->rdio_api_key);
    out->api_key[sizeof(out->api_key) - 1] = '\0';
}

/* 0 when @p api can upload; -1 without an API key; DSD_RDIO_UPLOAD_NO_SYSTEM without a system ID, said once a run: a
   call skipped that way is no export failure. */
static int
dsd_rdio_validate_api_config(const dsd_rdio_api_config* api) {
    if (!api) {
        return -1;
    }

    if (api->api_key[0] == '\0') {
        LOG_WARN("Rdio API upload skipped: missing API key\n");
        return -1;
    }

    if (api->system_id <= 0) {
        dsd_rdio_note_no_system();
        return DSD_RDIO_UPLOAD_NO_SYSTEM;
    }

    return 0;
}

static int
dsd_rdio_copy_upload_paths(dsd_rdio_upload_job* job, const char* wav_path, const char* meta_path) {
    if (!job || !wav_path || !meta_path) {
        return -1;
    }
    int wn = DSD_SNPRINTF(job->wav_path, sizeof(job->wav_path), "%s", wav_path);
    int mn = DSD_SNPRINTF(job->meta_path, sizeof(job->meta_path), "%s", meta_path);
    (void)DSD_SNPRINTF(job->public_wav_path, sizeof(job->public_wav_path), "%s", wav_path);
    if (wn < 0 || mn < 0 || (size_t)wn >= sizeof(job->wav_path) || (size_t)mn >= sizeof(job->meta_path)) {
        LOG_ERROR("Rdio export: upload path too long\n");
        return -1;
    }
    return 0;
}

static int
dsd_rdio_build_upload_job(const dsd_rdio_api_config* api, const char* wav_path, const char* meta_path,
                          int remove_meta_on_success, int remove_wav_on_success, dsd_rdio_upload_job** out_job) {
    if (!api || !wav_path || !meta_path || !out_job) {
        return -1;
    }

    dsd_rdio_upload_job* job = (dsd_rdio_upload_job*)calloc(1, sizeof(*job));
    if (!job) {
        LOG_ERROR("Rdio export: unable to allocate upload job\n");
        return -1;
    }

    job->api = *api;
    job->remove_meta_on_success = remove_meta_on_success ? 1 : 0;
    job->remove_wav_on_success = remove_wav_on_success ? 1 : 0;
    if (dsd_rdio_copy_upload_paths(job, wav_path, meta_path) != 0) {
        dsd_rdio_upload_job_free(job);
        return -1;
    }
    *out_job = job;
    return 0;
}

/* Whether the queue admits one more job: 0, DSD_RDIO_UPLOAD_CANCELLED after the user's Stop (the worker would never
   send it), or DSD_RDIO_UPLOAD_REFUSED, said for @p wav_path, when the shutdown has begun or the queue is full. Under
   the queue's mutex. */
static int
dsd_rdio_upload_admits_locked(const char* wav_path) {
    if (g_rdio_upload_stop_requested) {
        LOG_WARN("Rdio export: shutdown in progress, dropping %s\n", wav_path);
        return DSD_RDIO_UPLOAD_REFUSED;
    }
    if (atomic_load(&g_rdio_upload_cancel) != 0) {
        return DSD_RDIO_UPLOAD_CANCELLED;
    }
    if (g_rdio_upload_depth >= DSD_RDIO_UPLOAD_QUEUE_MAX) {
        LOG_WARN("Rdio export: upload queue full, dropping %s\n", wav_path);
        return DSD_RDIO_UPLOAD_REFUSED;
    }
    return 0;
}

/* dsd_rdio_upload_admits_locked(), asked ahead of the private inputs' I/O, which runs with the mutex free: the enqueue
   asks again, and has the last word. */
static int
dsd_rdio_upload_admits(const char* wav_path) {
    dsd_mutex_lock(&g_rdio_upload_mutex);
    const int admits = dsd_rdio_upload_admits_locked(wav_path);
    dsd_mutex_unlock(&g_rdio_upload_mutex);
    return admits;
}

/* 0 when @p job is queued; else dsd_rdio_upload_admits_locked()'s refusal, or -1, and the job is still the caller's. */
static int
dsd_rdio_enqueue_upload_job(dsd_rdio_upload_job* job, const char* wav_path) {
    if (!job || !wav_path) {
        return -1;
    }
    dsd_mutex_lock(&g_rdio_upload_mutex);
    const int admits = dsd_rdio_upload_admits_locked(wav_path);
    if (admits != 0) {
        dsd_mutex_unlock(&g_rdio_upload_mutex);
        return admits;
    }

    if (g_rdio_upload_tail) {
        g_rdio_upload_tail->next = job;
    } else {
        g_rdio_upload_head = job;
    }
    g_rdio_upload_tail = job;
    g_rdio_upload_depth++;
    dsd_cond_signal(&g_rdio_upload_cond);
    dsd_mutex_unlock(&g_rdio_upload_mutex);
    return 0;
}

#define DSD_RDIO_PRIVATE_AUDIO_SUFFIX ".rdio-upload-audio"
#define DSD_RDIO_PRIVATE_META_SUFFIX  ".rdio-upload-meta"
#define DSD_RDIO_PRIVATE_NAME_TRIES   64U

/* This process's random id in its private input names: 64 random bits drawn at first use, as 16 uppercase hex digits,
   so another run exporting to the same folder makes other names. */
static char g_rdio_private_token[17];
static atomic_int g_rdio_private_token_state = 0; /* 0 none, 1 drawing, 2 ready */
/* The number of the next private input pair, one per try. */
static atomic_int g_rdio_private_seq = 0;

static const char*
dsd_rdio_private_token(void) {
    if (atomic_load(&g_rdio_private_token_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_rdio_private_token_state, &expected, 1)) {
            uint64_t token = 0;
            dsd_nonce_fill(&token, sizeof token);
            DSD_SNPRINTF(g_rdio_private_token, sizeof g_rdio_private_token, "%016llX", (unsigned long long)token);
            atomic_store(&g_rdio_private_token_state, 2);
        } else {
            while (atomic_load(&g_rdio_private_token_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    return g_rdio_private_token;
}

/* errno for a copy the user's Stop cut short. */
#ifdef ECANCELED
#define DSD_RDIO_ECANCELED ECANCELED
#else
#define DSD_RDIO_ECANCELED EINTR
#endif

/* Copy @p in to @p out in 16 KiB chunks: 0, or the errno of what stopped it, DSD_RDIO_ECANCELED for the user's Stop
   (dsd_rdio_upload_cancel()), checked between chunks. */
static int
dsd_rdio_copy_chunks(FILE* in, FILE* out) {
    unsigned char buf[16384];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (atomic_load(&g_rdio_upload_cancel) != 0) {
            return DSD_RDIO_ECANCELED;
        }
        if (fwrite(buf, 1, n, out) != n) {
            return errno != 0 ? errno : EIO;
        }
    }
    return ferror(in) ? (errno != 0 ? errno : EIO) : 0;
}

/* A copy of the file at @p src, created at @p dst, which must not exist (EEXIST); owner-only. The user's Stop ends it
   between chunks with DSD_RDIO_ECANCELED (dsd_rdio_copy_chunks()). On a failure a @p dst this copy created is removed
   (dsd_rdio_remove_private_input(), which says a removal that fails), never one that was there. */
static int
dsd_rdio_copy_file(const char* src, const char* dst) {
    FILE* in = dsd_fopen_existing_regular_file(src, "rb");
    if (!in) {
        return -1;
    }
    int opened = 0;
    FILE* out = dsd_fopen_private_ex(dst, "wbx", &opened);
    if (!out) {
        const int err = errno;
        if (opened) {
            dsd_rdio_remove_private_input(dst);
        }
        (void)fclose(in);
        errno = err;
        return -1;
    }
    int err = dsd_rdio_copy_chunks(in, out);
    if (fclose(out) != 0 && err == 0) {
        err = errno != 0 ? errno : EIO;
    }
    (void)fclose(in);
    if (err != 0) {
        /* A removal that fails is said (once a run), and the copy's own error is the one returned. */
        dsd_rdio_remove_private_input(dst);
        errno = err == EEXIST ? EIO : err;
        return -1;
    }
    return 0;
}

/* The job's own name for the published WAV at @p src: a hard link, or a copy where the file system has none. */
static int
dsd_rdio_link_or_copy(const char* src, const char* dst) {
    if (dsd_hard_link(src, dst) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return -1;
    }
    return dsd_rdio_copy_file(src, dst);
}

/* The folder part of @p path with its trailing separator, "" for a bare file name. */
static int
dsd_rdio_path_folder(const char* path, char* out, size_t out_size) {
    const size_t len = (size_t)(dsd_rdio_path_file_name(path, DSD_PLATFORM_WIN_NATIVE) - path);
    if (len >= out_size) {
        return -1;
    }
    DSD_MEMCPY(out, path, len);
    out[len] = '\0';
    return 0;
}

/* The names of a private pair beside @p wav_path, numbered @p seq: "<id>.<seq>.rdio-upload-audio" and
   "<id>.<seq>.rdio-upload-meta", with this process's id (dsd_rdio_private_token()). At most 45 characters whatever
   the published name, so any recording whose public names fit can have them. */
static int
dsd_rdio_private_input_paths(dsd_rdio_upload_job* job, const char* wav_path, unsigned int seq) {
    char folder[DSD_RDIO_PATH_MAX];
    if (dsd_rdio_path_folder(wav_path, folder, sizeof folder) != 0) {
        return -1;
    }
    const char* token = dsd_rdio_private_token();
    const int wn = DSD_SNPRINTF(job->wav_path, sizeof job->wav_path, "%s%s.%u%s", folder, token, seq,
                                DSD_RDIO_PRIVATE_AUDIO_SUFFIX);
    const int mn = DSD_SNPRINTF(job->meta_path, sizeof job->meta_path, "%s%s.%u%s", folder, token, seq,
                                DSD_RDIO_PRIVATE_META_SUFFIX);
    return (wn < 0 || mn < 0 || (size_t)wn >= sizeof job->wav_path || (size_t)mn >= sizeof job->meta_path) ? -1 : 0;
}

/* --rdio-mode both: the job's own inputs, a copy of the sidecar (from @p meta_src, its temp file, before the public one
   is in place) and a hard link (or copy) of the published WAV, under names no rdio-scanner watch reads: neither suffix
   is one a watch looks at (6.6.3 and v8 read the last extension only: ".json" for a trunk-recorder watch, ".mp3" for
   SDRTrunk and DSDPlus, the configured one, ".wav" by default). A DirWatch that deletes the published pair after
   taking it then leaves this upload its bytes. A name in use moves on to the next number. On a failure nothing is left
   behind. 0, -1, or DSD_RDIO_UPLOAD_CANCELLED when the user's Stop came first or cut the copy short. */
static int
dsd_rdio_make_private_inputs(dsd_rdio_upload_job* job, const char* wav_path, const char* meta_src) {
    int err = EEXIST;
    for (unsigned int tries = 0; tries < DSD_RDIO_PRIVATE_NAME_TRIES && err == EEXIST; tries++) {
        if (atomic_load(&g_rdio_upload_cancel) != 0) {
            return DSD_RDIO_UPLOAD_CANCELLED;
        }
        const unsigned int seq = (unsigned int)atomic_fetch_add(&g_rdio_private_seq, 1);
        if (dsd_rdio_private_input_paths(job, wav_path, seq) != 0) {
            err = ENAMETOOLONG;
            break;
        }
        if (dsd_rdio_copy_file(meta_src, job->meta_path) != 0) {
            err = errno;
            continue;
        }
        if (dsd_rdio_link_or_copy(wav_path, job->wav_path) == 0) {
            job->private_inputs = 1;
            return 0;
        }
        err = errno;
        dsd_rdio_remove_private_input(job->meta_path);
    }
    if (err == DSD_RDIO_ECANCELED && atomic_load(&g_rdio_upload_cancel) != 0) {
        return DSD_RDIO_UPLOAD_CANCELLED;
    }
    LOG_ERROR("Rdio export: unable to make the upload's own copy of %s: %s\n", wav_path, strerror(err));
    return -1;
}

/* The upload job of @p wav_path and @p meta_path, its API config checked, into @p out_job: 0, or -1 with none
   (DSD_RDIO_UPLOAD_NO_SYSTEM, with none and nothing made, for a call with no system ID). With
   @p private_meta_src (--rdio-mode both: the sidecar's temp file) the job uploads from its own copies of the pair
   (dsd_rdio_make_private_inputs()), made here, before the public sidecar is in place, and only once the queue would
   admit the job: a job it would refuse (DSD_RDIO_UPLOAD_REFUSED: a full queue, the shutdown) costs no WAV copy. After
   the user's Stop none are made, and DSD_RDIO_UPLOAD_CANCELLED says so. */
static int
dsd_rdio_prepare_api_job(const dsd_opts* opts, int system_id, const char* wav_path, const char* meta_path,
                         const char* private_meta_src, int remove_meta_on_success, int remove_wav_on_success,
                         dsd_rdio_upload_job** out_job) {
    *out_job = NULL;
    dsd_rdio_api_config api;
    dsd_rdio_copy_api_config(&api, opts, system_id);
    int rc = dsd_rdio_validate_api_config(&api);
    if (rc == 0) {
        rc = -1;
        if (dsd_rdio_upload_worker_init() != 0) {
            LOG_ERROR("Rdio export: failed to initialize background upload worker\n");
        } else {
            rc = dsd_rdio_build_upload_job(&api, wav_path, meta_path, remove_meta_on_success, remove_wav_on_success,
                                           out_job);
        }
    }
    /* The job holds its own copy now: this one goes. */
    DSD_SECURE_ZERO(&api, sizeof api);
    if (rc == 0 && private_meta_src) {
        rc = dsd_rdio_upload_admits(wav_path);
        if (rc == 0) {
            rc = dsd_rdio_make_private_inputs(*out_job, wav_path, private_meta_src);
        }
        if (rc != 0) {
            dsd_rdio_upload_job_free(*out_job);
            *out_job = NULL;
        }
    }
    return rc;
}
#else
void
dsd_rdio_session_reset(void) {
    atomic_store(&g_rdio_no_system_warned, 0);
}

void
dsd_rdio_upload_shutdown(void) {}

int
dsd_rdio_upload_holds(const char* path) {
    (void)path;
    return 0;
}

void
dsd_rdio_upload_cancel(void) {}

void
dsd_rdio_upload_cancel_reset(void) {}

void
dsd_rdio_upload_set_release_observer(dsd_rdio_upload_release_observer observer) {
    (void)observer;
}
#endif

static int
dsd_rdio_mode_wants_dirwatch(int mode) {
    return mode == DSD_RDIO_MODE_DIRWATCH || mode == DSD_RDIO_MODE_BOTH;
}

static int
dsd_rdio_mode_wants_api(int mode) {
    return mode == DSD_RDIO_MODE_API || mode == DSD_RDIO_MODE_BOTH;
}

/* @p wav_path without a final ".wav" (any case), then @p suffix. */
static int
dsd_rdio_path_with_suffix(const char* wav_path, const char* suffix, char* out, size_t out_size) {
    if (!wav_path || !suffix || !out || out_size == 0) {
        return -1;
    }

    char base_path[DSD_RDIO_PATH_MAX];
    DSD_SNPRINTF(base_path, sizeof(base_path), "%s", wav_path);
    base_path[sizeof(base_path) - 1] = '\0';

    char* dot = strrchr(base_path, '.');
    if (dot && dsd_strcasecmp(dot, ".wav") == 0) {
        *dot = '\0';
    }

    int n = DSD_SNPRINTF(out, out_size, "%s%s", base_path, suffix);
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }

    return 0;
}

static int
dsd_rdio_make_sidecar_path(const char* wav_path, char* out_meta_path, size_t out_meta_path_size) {
    return dsd_rdio_path_with_suffix(wav_path, ".json", out_meta_path, out_meta_path_size);
}

static uint16_t
dsd_rdio_read_u16_le(const unsigned char* in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t
dsd_rdio_read_u32_le(const unsigned char* in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static unsigned int
dsd_rdio_wav_compute_duration(uint32_t sample_rate, uint16_t block_align, uint32_t data_bytes) {
    if (sample_rate == 0U || block_align == 0U) {
        return 0;
    }
    uint64_t frame_count = (uint64_t)data_bytes / (uint64_t)block_align;
    return (unsigned int)(frame_count / (uint64_t)sample_rate);
}

static int
dsd_rdio_wav_skip_bytes(FILE* fp, uint32_t bytes) {
    if (!fp) {
        return -1;
    }
#if LONG_MAX < 4294967295LL
    if (bytes > (uint32_t)LONG_MAX) {
        return -1;
    }
#endif
    return fseek(fp, (long)bytes, SEEK_CUR);
}

static int
dsd_rdio_wav_read_fmt_chunk(FILE* fp, uint32_t chunk_size, uint32_t* sample_rate, uint16_t* block_align,
                            int* have_fmt) {
    if (!fp || !sample_rate || !block_align || !have_fmt) {
        return -1;
    }
    if (chunk_size < 16U) {
        return -1;
    }

    unsigned char fmt_hdr[16];
    if (fread(fmt_hdr, 1, sizeof(fmt_hdr), fp) != sizeof(fmt_hdr)) {
        return -1;
    }

    *sample_rate = dsd_rdio_read_u32_le(fmt_hdr + 4);
    *block_align = dsd_rdio_read_u16_le(fmt_hdr + 12);
    *have_fmt = 1;

    uint32_t remaining = chunk_size - 16U;
    if (remaining > 0U && dsd_rdio_wav_skip_bytes(fp, remaining) != 0) {
        return -1;
    }
    return 0;
}

static int
dsd_rdio_wav_read_chunk(FILE* fp, const unsigned char chunk_hdr[8], uint32_t chunk_size, uint32_t* sample_rate,
                        uint16_t* block_align, uint32_t* data_bytes, int* have_fmt, int* have_data) {
    if (!fp || !chunk_hdr || !sample_rate || !block_align || !data_bytes || !have_fmt || !have_data) {
        return -1;
    }
    if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
        if (dsd_rdio_wav_read_fmt_chunk(fp, chunk_size, sample_rate, block_align, have_fmt) != 0) {
            return -1;
        }
    } else if (memcmp(chunk_hdr, "data", 4) == 0) {
        *data_bytes = chunk_size;
        *have_data = 1;
        return 0;
    } else if (dsd_rdio_wav_skip_bytes(fp, chunk_size) != 0) {
        return -1;
    }

    if ((chunk_size & 1U) != 0U && dsd_rdio_wav_skip_bytes(fp, 1U) != 0) {
        return -1;
    }
    return 0;
}

static unsigned int
dsd_rdio_wav_duration_s(const char* wav_path) {
    if (!wav_path || wav_path[0] == '\0') {
        return 0;
    }

    FILE* fp = dsd_fopen_existing_regular_file(wav_path, "rb");
    if (!fp) {
        return 0;
    }

    unsigned char riff_hdr[12];
    if (fread(riff_hdr, 1, sizeof(riff_hdr), fp) != sizeof(riff_hdr)) {
        fclose(fp);
        return 0;
    }

    if (memcmp(riff_hdr, "RIFF", 4) != 0 || memcmp(riff_hdr + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return 0;
    }

    uint32_t sample_rate = 0;
    uint16_t block_align = 0;
    uint32_t data_bytes = 0;
    int have_fmt = 0;
    int have_data = 0;

    while (!have_data) {
        unsigned char chunk_hdr[8];
        if (fread(chunk_hdr, 1, sizeof(chunk_hdr), fp) != sizeof(chunk_hdr)) {
            break;
        }

        uint32_t chunk_size = dsd_rdio_read_u32_le(chunk_hdr + 4);
        if (dsd_rdio_wav_read_chunk(fp, chunk_hdr, chunk_size, &sample_rate, &block_align, &data_bytes, &have_fmt,
                                    &have_data)
            != 0) {
            break;
        }
    }

    fclose(fp);

    if (!have_fmt || !have_data) {
        return 0;
    }
    return dsd_rdio_wav_compute_duration(sample_rate, block_align, data_bytes);
}

static void
dsd_rdio_json_write_ascii(FILE* fp, unsigned char c) {
    switch (c) {
        case '"': fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\b': fputs("\\b", fp); break;
        case '\f': fputs("\\f", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (iscntrl(c)) {
                DSD_FPRINTF(fp, "\\u%04x", (unsigned int)c);
            } else {
                fputc((int)c, fp);
            }
            break;
    }
}

/* The bytes of the maximal ill-formed run at @p s (Unicode's "maximal subpart"): a lead byte and the continuation
   bytes after it that could still begin its character, else the one byte. At least 1; @p avail is at least 1. */
static size_t
dsd_rdio_utf8_ill_formed_len(const char* s, size_t avail) {
    unsigned char lo;
    unsigned char hi;
    const size_t len = dsd_utf8_lead((unsigned char)s[0], &lo, &hi);
    size_t n = 1;
    while (n < len && n < avail) {
        const unsigned char c = (unsigned char)s[n];
        if (c < (n == 1U ? lo : 0x80U) || c > (n == 1U ? hi : 0xBFU)) {
            break;
        }
        n++;
    }
    return n;
}

/* A JSON string's content (issue #888 B11): well-formed UTF-8 as it is, ASCII escaped where JSON needs it, and each
   ill-formed run (a name cut mid-character) as one U+FFFD, so the sidecar is always valid UTF-8. */
static void
dsd_rdio_json_write_escaped(FILE* fp, const char* s) {
    const char* text = s ? s : "";
    const size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x80U) {
            dsd_rdio_json_write_ascii(fp, c);
            i++;
            continue;
        }
        const size_t n = dsd_utf8_sequence_len(text + i, len - i);
        if (n > 0U) {
            (void)fwrite(text + i, 1, n, fp);
            i += n;
        } else {
            fputs("\xEF\xBF\xBD", fp);
            i += dsd_rdio_utf8_ill_formed_len(text + i, len - i);
        }
    }
}

/* Whether @p tag names something rdio-scanner keeps as a label (issue #888 B10): not empty and not "-", which both
   servers discard. A "-" tag gives way to the next candidate. */
static int
dsd_rdio_tag_usable(const char* tag) {
    return tag[0] != '\0' && strcmp(tag, "-") != 0;
}

typedef struct {
    time_t start_time;
    uint32_t talkgroup;
    uint32_t source;
    uint32_t freq_hz; /* dsd_rdio_usable_freq_hz(): 0 when unknown or not storable */
    int system_id;
    int encrypted;
    int emergency;
    const char* talkgroup_tag;
    const char* short_name;
    char generated_tag[64]; /* an analog call's frequency tag, or a fallback call's generated one (issue #888 D3) */
    char system_name[24];   /* a frequency system's short_name, "System <kHz>" (issue #931) */
} dsd_rdio_meta_fields;

/* The Hz the sidecar's freq and freqList carry (issue #888 B5, B13, D8): from 1 MHz (included) up to what the signed
   32-bit frequency columns of rdio-scanner's MySQL/MariaDB and PostgreSQL databases store; a frequency outside that is
   unknown (0), so the whole call is not refused for it. */
static uint32_t
dsd_rdio_usable_freq_hz(int64_t freq_hz) {
    return (freq_hz >= 1000000 && freq_hz <= (int64_t)INT32_MAX) ? (uint32_t)freq_hz : 0U;
}

/* The protocol's word for a digital row's sync type (issue #888 B8, D3), "" for analog and any other sync type. P25's
   two phases share one word. */
static const char*
dsd_rdio_protocol_word(int systype) {
    if (DSD_SYNC_IS_P25(systype)) {
        return "P25";
    }
    if (DSD_SYNC_IS_DMR(systype)) {
        return "DMR";
    }
    if (DSD_SYNC_IS_NXDN(systype)) {
        return "NXDN";
    }
    if (DSD_SYNC_IS_DSTAR(systype)) {
        return "D-STAR";
    }
    if (DSD_SYNC_IS_YSF(systype)) {
        return "YSF";
    }
    if (DSD_SYNC_IS_DPMR(systype)) {
        return "dPMR";
    }
    if (DSD_SYNC_IS_M17(systype)) {
        return "M17";
    }
    if (DSD_SYNC_IS_X2TDMA(systype)) {
        return "X2-TDMA";
    }
    if (DSD_SYNC_IS_PROVOICE(systype)) {
        return "ProVoice";
    }
    if (DSD_SYNC_IS_EDACS_ONLY(systype)) {
        return "EDACS";
    }
    if (DSD_SYNC_IS_TETRA(systype)) {
        return "TETRA";
    }
    return "";
}

/* The protocol's word for @p event (dsd_rdio_protocol_word()), but "EDACS" for a ProVoice call an EDACS grant named
   (issue #888): its row carries the grant's LCN, a channel no standalone ProVoice call has. An EDACS channel's digital
   calls then share its analog calls' word, as they share its kHz talkgroup. Standalone ProVoice keeps "ProVoice". */
static const char*
dsd_rdio_row_protocol_word(const Event_History* event) {
    if (DSD_SYNC_IS_PROVOICE(event->systype) && event->channel != 0U) {
        return "EDACS";
    }
    return dsd_rdio_protocol_word(event->systype);
}

/* The row a recording is exported from: the slot's staged row, or none. */
static const Event_History*
dsd_rdio_staged_row(const Event_History_I* event_struct) {
    return event_struct ? &event_struct->Event_History_Items[0] : NULL;
}

uint32_t
dsd_rdio_analog_talkgroup(const dsd_opts* opts, const Event_History* event) {
    if (!event || event->systype != DSD_SYNC_ANALOG) {
        return 0U;
    }
    // rdio-scanner's own convention for a conventional channel: its frequency in kHz. A frequency under 1 kHz names
    // no channel, and one whose kHz do not fit the field is as unknown as none.
    const int64_t khz = event->freq_hz / 1000;
    if (khz > 0 && khz <= (int64_t)UINT32_MAX) {
        return (uint32_t)khz;
    }
    return (opts && opts->rdio_analog_talkgroup > 0) ? (uint32_t)opts->rdio_analog_talkgroup : 0U;
}

/* A digital row's frequency in whole kHz, rounded down, when that fits rdio-scanner's signed 32-bit talkgroup columns
   (1..2147483647, issue #888 D2), else 0. The analog rule keeps its own range (dsd_rdio_analog_talkgroup()). */
static uint32_t
dsd_rdio_digital_khz(int64_t freq_hz) {
    const int64_t khz = freq_hz / 1000;
    return (khz > 0 && khz <= (int64_t)INT32_MAX) ? (uint32_t)khz : 0U;
}

int
dsd_rdio_call_fallback_eligible(const Event_History* event, int allow_fallback) {
    // Fail closed on a row of no known protocol: its label would be a bare frequency, and a v8 DirWatch, which resolves
    // the system from short_name, could pick none.
    return allow_fallback && event != NULL && event->systype != DSD_SYNC_ANALOG && event->target_id == 0U
           && event->category == DSD_EVENT_CATEGORY_VOICE && event->fallback_blocked == 0U
           && dsd_rdio_row_protocol_word(event)[0] != '\0';
}

uint32_t
dsd_rdio_call_talkgroup(const dsd_opts* opts, const Event_History* event, int allow_fallback) {
    if (!event) {
        return 0U;
    }
    if (event->systype == DSD_SYNC_ANALOG) {
        return dsd_rdio_analog_talkgroup(opts, event);
    }
    if (event->target_id > 0U) {
        return event->target_id;
    }
    if (!dsd_rdio_call_fallback_eligible(event, allow_fallback)) {
        return 0U;
    }
    const uint32_t khz = dsd_rdio_digital_khz(event->freq_hz);
    if (khz > 0U) {
        return khz;
    }
    return (opts && opts->rdio_unknown_talkgroup > 0) ? (uint32_t)opts->rdio_unknown_talkgroup : 0U;
}

dsd_rdio_system_choice
dsd_rdio_resolve_call_system(const dsd_opts* opts, const Event_History* event) {
    dsd_rdio_system_choice choice = {0, DSD_RDIO_SYSTEM_RULE_BASE};
    if (!opts) {
        return choice;
    }
    if (event && event->systype == DSD_SYNC_ANALOG && opts->rdio_analog_system_id > 0) {
        choice.id = opts->rdio_analog_system_id;
        choice.rule = DSD_RDIO_SYSTEM_RULE_ANALOG;
        return choice;
    }
    // The digital kHz rule (1..INT32_MAX) keeps the number in the int every server's system field is: the analog
    // talkgroup's wider range would not fit.
    const uint32_t khz = (opts->rdio_system_by_frequency && event) ? dsd_rdio_digital_khz(event->freq_hz) : 0U;
    if (khz > 0U) {
        choice.id = (int)khz;
        choice.rule = DSD_RDIO_SYSTEM_RULE_FREQUENCY;
        return choice;
    }
    choice.id = opts->rdio_system_id;
    return choice;
}

int
dsd_rdio_call_system_id(const dsd_opts* opts, const Event_History* event) {
    return dsd_rdio_resolve_call_system(opts, event).id;
}

time_t
dsd_rdio_recording_start(const Event_History* event, time_t opened, time_t started) {
    // A recorder that stamps its recording's first sample itself (the NFM call, issue #668) knows the start better than
    // the row does: the row starts when its first block was accepted, a block's span after the audio began.
    if (started > 0) {
        return started;
    }
    // The recording is filed by when it began: when its call did, or when the WAV was opened if that came later (per-
    // call WAV switched on mid-call). Both are decode-clock stamps. event_time is the row's last activity -- the call's
    // end by the time its WAV closes -- so it stands in only for a row with no start of its own, such as a --playfiles
    // row, whose event_time the file reader stamped from the recording; that stamp is on the recording's clock, so the
    // open time is not compared with it. stop_time adds the WAV length to the start, and is never derived by moving a
    // fallback stamp back: a played file's stamp need not be an end.
    if (event && event->event_start_time > 0) {
        return opened > event->event_start_time ? opened : event->event_start_time;
    }
    if (event && event->event_time > 0) {
        return event->event_time;
    }
    return dsd_decode_time();
}

/* "<MHz> MHz" for a positive @p freq_hz: whole 100 Hz, rounded, as 4 decimals of MHz. */
static void
dsd_rdio_format_mhz(int64_t freq_hz, char* out, size_t out_size) {
    const int64_t units = freq_hz / 100 + (freq_hz % 100 >= 50 ? 1 : 0);
    DSD_SNPRINTF(out, out_size, "%lld.%04lld MHz", (long long)(units / 10000), (long long)(units % 10000));
}

/* An access code as the app's views spell it (src/app_control/access_code_view.c, which runtime cannot call): "RAN 9",
   "CC 3", "CAN 15" in decimal, "NAC 2A3" as three uppercase hex digits, each only within the values its protocol
   carries (a NAC from 001 to FFE: dsd-neo takes no 000 or FFF). "" for a code out of range or of a kind not known. */
static void
dsd_rdio_format_access_code(uint8_t kind, uint16_t code, char* out, size_t out_size) {
    out[0] = '\0';
    const unsigned value = (unsigned)code;
    switch (kind) {
        case DSD_ACCESS_CODE_COLOR_CODE:
            if (value <= 63U) {
                DSD_SNPRINTF(out, out_size, "CC %u", value);
            }
            break;
        case DSD_ACCESS_CODE_NAC:
            if (value >= 0x001U && value <= 0xFFEU) {
                DSD_SNPRINTF(out, out_size, "NAC %03X", value);
            }
            break;
        case DSD_ACCESS_CODE_RAN:
            if (value <= 63U) {
                DSD_SNPRINTF(out, out_size, "RAN %u", value);
            }
            break;
        case DSD_ACCESS_CODE_CAN:
            if (value <= 15U) {
                DSD_SNPRINTF(out, out_size, "CAN %u", value);
            }
            break;
        default: break;
    }
}

/* Append @p part to the space-separated words in @p out; an empty part adds nothing. */
static void
dsd_rdio_tag_append(char* out, size_t out_size, const char* part) {
    if (part[0] == '\0') {
        return;
    }
    const size_t used = strlen(out);
    if (used + 1U >= out_size) {
        return;
    }
    DSD_SNPRINTF(out + used, out_size - used, "%s%s", used > 0U ? " " : "", part);
}

/* The label of a call exported under the fallback number (issue #888 D3): its service name, else its channel label,
   else "<protocol> <access code> <MHz> MHz" with the unknown parts left out ("NXDN RAN 9", "D-STAR 145.6700 MHz",
   "X2-TDMA"). Never per-call identity text or PRIVATE/GROUP: rdio-scanner can relabel a talkgroup with each call's tag,
   and every unnumbered call on one frequency shares its talkgroup. */
static void
dsd_rdio_fallback_tag(const Event_History* event, dsd_rdio_meta_fields* out) {
    if (dsd_rdio_tag_usable(event->service_label)) {
        out->talkgroup_tag = event->service_label;
        return;
    }
    if (dsd_rdio_tag_usable(event->channel_label)) {
        out->talkgroup_tag = event->channel_label;
        return;
    }
    char code[16];
    dsd_rdio_format_access_code(event->access_code_kind, event->access_code, code, sizeof code);
    out->generated_tag[0] = '\0';
    dsd_rdio_tag_append(out->generated_tag, sizeof out->generated_tag, dsd_rdio_row_protocol_word(event));
    dsd_rdio_tag_append(out->generated_tag, sizeof out->generated_tag, code);
    if (dsd_rdio_digital_khz(event->freq_hz) > 0U) {
        char mhz[32];
        dsd_rdio_format_mhz(event->freq_hz, mhz, sizeof mhz);
        dsd_rdio_tag_append(out->generated_tag, sizeof out->generated_tag, mhz);
    }
    out->talkgroup_tag = out->generated_tag;
}

/* A numbered call's label: its first name (service, talkgroup name, target text, channel label; "-" is none, B10),
   else PRIVATE for a private call (gi 1), GROUP for a group call (gi 0). Target text that only spells the numeric
   talkgroup, an NXDN Type-D group's "pp-iiii" (dsd_call_target_text_is_name(), issue #903), ranks below the channel
   label, as a bare number does. A call of no known kind (gi -1: NXDN's other call types, EDACS without a group or
   individual flag, TETRA before its setup, X2-TDMA) keeps an empty tag, which rdio-scanner ignores as a label: it names
   the talkgroup by its number rather than calling every such call private. */
static const char*
dsd_rdio_numbered_tag(const Event_History* event) {
    if (dsd_rdio_tag_usable(event->service_label)) {
        return event->service_label;
    }
    if (dsd_rdio_tag_usable(event->t_name)) {
        return event->t_name;
    }
    if (dsd_rdio_tag_usable(event->tgt_str)
        && dsd_call_target_text_is_name(event->systype, event->target_id, event->tgt_str)) {
        return event->tgt_str;
    }
    if (dsd_rdio_tag_usable(event->channel_label)) {
        // A numeric talkgroup with no name of its own -- no CSV entry for it -- heard while the receiver was parked on
        // a named channel. That name is what the operator would recognise the upload by; "GROUP" is not.
        return event->channel_label;
    }
    if (dsd_rdio_tag_usable(event->tgt_str)) {
        return event->tgt_str;
    }
    if (event->gi == 1) {
        return "PRIVATE";
    }
    return event->gi == 0 ? "GROUP" : "";
}

/* A digital call: the decoded target, or with @p allow_fallback a VOICE row's fallback number
   (dsd_rdio_call_talkgroup()), which takes the fallback label; the decoded source and encryption. */
static void
dsd_rdio_digital_meta_fields(const dsd_opts* opts, const Event_History* event, int allow_fallback,
                             dsd_rdio_meta_fields* out) {
    out->talkgroup = dsd_rdio_call_talkgroup(opts, event, allow_fallback);
    out->source = event->source_id;
    out->encrypted = event->enc ? 1 : 0;
    if (event->target_id == 0U && out->talkgroup != 0U) {
        dsd_rdio_fallback_tag(event, out);
    } else {
        out->talkgroup_tag = dsd_rdio_numbered_tag(event);
    }
}

/* A conventional analog call (issue #668): a kHz talkgroup, or the user's for a call with no frequency, labelled by the
   channel's name, else by its frequency, else by the mode alone. Never by the per-tone service name: rdio-scanner can
   relabel a talkgroup with each call's tag, and the services on one frequency share its talkgroup. No radio, never
   encrypted. */
static void
dsd_rdio_analog_meta_fields(const dsd_opts* opts, const Event_History* event, dsd_rdio_meta_fields* out) {
    out->talkgroup = dsd_rdio_analog_talkgroup(opts, event);
    out->source = 0U;
    out->encrypted = 0;
    const char* mode = event->sysid_string[0] != '\0' ? event->sysid_string : "NFM";
    if (dsd_rdio_tag_usable(event->channel_label)) {
        out->talkgroup_tag = event->channel_label;
    } else if (event->freq_hz > 0) {
        char mhz[32];
        dsd_rdio_format_mhz(event->freq_hz, mhz, sizeof mhz);
        DSD_SNPRINTF(out->generated_tag, sizeof out->generated_tag, "%s %s", mode, mhz);
        out->talkgroup_tag = out->generated_tag;
    } else {
        out->talkgroup_tag = mode;
    }
}

/* The sidecar fields of the call @p event, filed under @p system, the export's one choice of system. */
static void
dsd_rdio_meta_fields_from_event(const dsd_opts* opts, const Event_History* event, const dsd_rdio_system_choice* system,
                                time_t opened, time_t started, int allow_fallback, dsd_rdio_meta_fields* out) {
    if (!out) {
        return;
    }
    out->start_time = dsd_decode_time();
    out->talkgroup = 0;
    out->source = 0;
    out->freq_hz = 0;
    out->system_id = system->id;
    out->encrypted = 0;
    out->emergency = 0;
    out->talkgroup_tag = "";
    out->short_name = "";
    out->generated_tag[0] = '\0';
    out->system_name[0] = '\0';

    if (!event) {
        return;
    }
    out->start_time = dsd_rdio_recording_start(event, opened, started);
    // The frequency the call was heard on. Never event->channel: that is a trunk channel number,
    // an LCN or a dPMR colour code, not Hz.
    out->freq_hz = dsd_rdio_usable_freq_hz(event->freq_hz);
    // A row with no system string (an X2-TDMA call) is named by its protocol, so a v8 DirWatch, which resolves the
    // system from short_name, still finds one.
    out->short_name = event->sysid_string[0] != '\0' ? event->sysid_string : dsd_rdio_row_protocol_word(event);
    if (system->rule == DSD_RDIO_SYSTEM_RULE_FREQUENCY) {
        // A system per frequency is named as rdio-scanner 6 names one it creates, so a v8 DirWatch, which picks the
        // system by short_name, files each frequency apart (issue #931). The row's own system string is untouched: the
        // WAV name, the journal and the event feed keep it.
        DSD_SNPRINTF(out->system_name, sizeof out->system_name, "System %d", system->id);
        out->short_name = out->system_name;
    }
    out->emergency = event->emergency ? 1 : 0;
    if (event->systype == DSD_SYNC_ANALOG) {
        dsd_rdio_analog_meta_fields(opts, event, out);
        return;
    }
    dsd_rdio_digital_meta_fields(opts, event, allow_fallback, out);
}

static int
dsd_rdio_open_temp_meta_file(const char* out_meta_path, char* temp_meta_path, size_t temp_meta_path_size,
                             FILE** out_fp) {
    if (!out_meta_path || !temp_meta_path || temp_meta_path_size == 0 || !out_fp) {
        return -1;
    }

    FILE* fp = dsd_fopen_private_temp_for_replace(out_meta_path, temp_meta_path, temp_meta_path_size, "wb");
    if (!fp) {
        LOG_ERROR("Rdio export: unable to open temp sidecar for %s: %s\n", out_meta_path, strerror(errno));
        return -1;
    }
    *out_fp = fp;
    return 0;
}

static void
dsd_rdio_write_meta_json(FILE* fp, const dsd_opts* opts, const dsd_rdio_meta_fields* fields, time_t stop_time) {
    if (!fp || !opts || !fields) {
        return;
    }
    DSD_FPRINTF(fp, "{\n");
    DSD_FPRINTF(fp, "  \"start_time\": %lld,\n", (long long)fields->start_time);
    DSD_FPRINTF(fp, "  \"stop_time\": %lld,\n", (long long)stop_time);
    DSD_FPRINTF(fp, "  \"talkgroup\": %u,\n", fields->talkgroup);
    DSD_FPRINTF(fp, "  \"talkgroup_tag\": \"");
    dsd_rdio_json_write_escaped(fp, fields->talkgroup_tag);
    DSD_FPRINTF(fp, "\",\n");
    DSD_FPRINTF(fp, "  \"srcList\": [");
    if (fields->source > 0) {
        DSD_FPRINTF(fp, "{\"pos\":0,\"src\":%u}", fields->source);
    }
    DSD_FPRINTF(fp, "],\n");
    DSD_FPRINTF(fp, "  \"freq\": %u,\n", fields->freq_hz);
    if (fields->freq_hz > 0U) {
        // rdio-scanner v8 reads the frequency from the list only; 6.6.3 reads either.
        DSD_FPRINTF(fp, "  \"freqList\": [{\"pos\": 0, \"freq\": %u}],\n", fields->freq_hz);
    }
    DSD_FPRINTF(fp, "  \"system\": %d,\n", fields->system_id > 0 ? fields->system_id : 0);
    DSD_FPRINTF(fp, "  \"short_name\": \"");
    dsd_rdio_json_write_escaped(fp, fields->short_name);
    DSD_FPRINTF(fp, "\",\n");
    DSD_FPRINTF(fp, "  \"emergency\": %s,\n", fields->emergency ? "true" : "false");
    DSD_FPRINTF(fp, "  \"encrypted\": %s\n", fields->encrypted ? "true" : "false");
    DSD_FPRINTF(fp, "}\n");
}

static dsd_rdio_sidecar_published_observer g_rdio_sidecar_published_observer = NULL;

void
dsd_rdio_set_sidecar_published_observer(dsd_rdio_sidecar_published_observer observer) {
    g_rdio_sidecar_published_observer = observer;
}

/* Flush, sync and close the temp sidecar @p fp at @p temp_meta_path; on a failure it is removed. */
static int
dsd_rdio_close_temp_meta_file(FILE* fp, const char* temp_meta_path) {
    if (!fp || !temp_meta_path) {
        return -1;
    }
    if (fflush(fp) != 0) {
        LOG_ERROR("Rdio export: failed flushing %s: %s\n", temp_meta_path, strerror(errno));
        (void)fclose(fp);
        (void)dsd_remove_file(temp_meta_path);
        return -1;
    }
    int fd = dsd_fileno(fp);
    if (fd >= 0) {
        (void)dsd_fchmod(fd, 0600);
        if (dsd_fsync(fd) != 0) {
            LOG_ERROR("Rdio export: failed syncing %s: %s\n", temp_meta_path, strerror(errno));
            (void)fclose(fp);
            (void)dsd_remove_file(temp_meta_path);
            return -1;
        }
    }
    if (fclose(fp) != 0) {
        LOG_ERROR("Rdio export: failed closing %s: %s\n", temp_meta_path, strerror(errno));
        (void)dsd_remove_file(temp_meta_path);
        return -1;
    }
    return 0;
}

/* Put the closed temp sidecar @p temp_meta_path in place under its public name @p out_meta_path, where a DirWatch can
   take the pair; on a failure the temp file is removed. */
static int
dsd_rdio_publish_temp_meta_file(const char* temp_meta_path, const char* out_meta_path) {
    if (dsd_replace_file_with_temp(temp_meta_path, out_meta_path) != 0) {
        LOG_ERROR("Rdio export: failed renaming %s -> %s: %s\n", temp_meta_path, out_meta_path, strerror(errno));
        (void)dsd_remove_file(temp_meta_path);
        return -1;
    }
    if (g_rdio_sidecar_published_observer) {
        g_rdio_sidecar_published_observer(out_meta_path);
    }
    return 0;
}

/* Write the sidecar @p fields of @p wav_path's call to a closed temp file, @p temp_meta_path, beside its public name
   @p out_meta_path, which dsd_rdio_publish_temp_meta_file() then puts it under. */
static int
dsd_rdio_write_trunk_recorder_meta(const dsd_opts* opts, dsd_rdio_meta_fields* fields, const char* wav_path,
                                   char* out_meta_path, size_t out_meta_path_size, char* temp_meta_path,
                                   size_t temp_meta_path_size) {
    if (!opts || !wav_path || wav_path[0] == '\0') {
        return -1;
    }

    if (dsd_rdio_make_sidecar_path(wav_path, out_meta_path, out_meta_path_size) != 0) {
        LOG_ERROR("Rdio export: failed to build sidecar path for %s\n", wav_path);
        return -1;
    }

    // A backstop: the WAV close asks dsd_rdio_call_talkgroup() first, and exports only a call it gives a number.
    if (fields->talkgroup == 0U) {
        LOG_WARN("Rdio export: skipped %s (missing talkgroup/target ID)\n", wav_path);
        return -1;
    }
    if (fields->start_time <= 0) {
        fields->start_time = dsd_decode_time();
    }

    unsigned int duration_s = dsd_rdio_wav_duration_s(wav_path);
    time_t stop_time = fields->start_time + (time_t)duration_s;

    FILE* fp = NULL;
    if (dsd_rdio_open_temp_meta_file(out_meta_path, temp_meta_path, temp_meta_path_size, &fp) != 0) {
        return -1;
    }

    dsd_rdio_write_meta_json(fp, opts, fields, stop_time);
    return dsd_rdio_close_temp_meta_file(fp, temp_meta_path);
}

#ifdef USE_CURL
static size_t
dsd_rdio_discard_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static int
dsd_rdio_build_api_endpoint(const dsd_rdio_api_config* api, char* out, size_t out_size) {
    const char* base = "http://127.0.0.1:3000";
    if (api && api->api_url[0] != '\0') {
        base = api->api_url;
    }

    if (strstr(base, "/api/trunk-recorder-call-upload") != NULL) {
        int n = DSD_SNPRINTF(out, out_size, "%s", base);
        return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
    }

    size_t len = strlen(base);
    if (len > 0 && base[len - 1] == '/') {
        int n = DSD_SNPRINTF(out, out_size, "%sapi/trunk-recorder-call-upload", base);
        return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
    }

    int n = DSD_SNPRINTF(out, out_size, "%s/api/trunk-recorder-call-upload", base);
    return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
}

static int
dsd_rdio_normalize_upload_options(const dsd_rdio_api_config* api, int* timeout_ms, int* attempts) {
    if (!api || !timeout_ms || !attempts) {
        return -1;
    }

    *timeout_ms = api->upload_timeout_ms;
    if (*timeout_ms <= 0) {
        *timeout_ms = 5000;
    }

    *attempts = api->upload_retries;
    if (*attempts <= 0) {
        *attempts = 1;
    }
    if (*attempts > 10) {
        *attempts = 10;
    }
    return 0;
}

/* Aborts the transfer in flight once the user's Stop cancels the uploads. libcurl calls this while a transfer runs,
 * but not while a connect or TLS handshake stalls; dsd_rdio_cancel_pred() covers those phases. */
static int
dsd_rdio_xferinfo_cb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)userdata;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return atomic_load(&g_rdio_upload_cancel) != 0 ? 1 : 0;
}

/* Cancel predicate for dsd_curl_perform_cancellable(): it sees the user's Stop within about 100 ms. The attempt then
 * returns as promptly, except for a name lookup under libcurl older than 8.20, which waits for the system resolver. */
static int
dsd_rdio_cancel_pred(const void* ctx) {
    (void)ctx;
    return atomic_load(&g_rdio_upload_cancel) != 0 ? 1 : 0;
}

static void
dsd_rdio_configure_curl_request(CURL* curl, curl_mime* mime, const char* endpoint, int timeout_ms) {
    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    /* Both timeouts take the same value here, which is what this call has always done. */
    dsd_curl_apply_hardening(curl, timeout_ms, timeout_ms, "dsd-neo/rdio-export");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dsd_rdio_discard_write_cb);
    /* CURLOPT_NOPROGRESS defaults to 1, and while it is set libcurl never calls the xferinfo callback. */
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dsd_rdio_xferinfo_cb);
}

static void
dsd_rdio_add_mime_parts(curl_mime* mime, const dsd_rdio_api_config* api, const char* system_str,
                        const dsd_rdio_upload_inputs* inputs) {
    curl_mimepart* part = NULL;

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "key");
    curl_mime_data(part, api->api_key, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "system");
    curl_mime_data(part, system_str, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "audio");
    curl_mime_filedata(part, inputs->wav_path);
    if (inputs->wav_name) {
        curl_mime_filename(part, inputs->wav_name);
    }
    curl_mime_type(part, "audio/wav");

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "meta");
    curl_mime_filedata(part, inputs->meta_path);
    if (inputs->meta_name) {
        curl_mime_filename(part, inputs->meta_name);
    }
    curl_mime_type(part, "application/json");
}

static int
dsd_rdio_upload_attempt_once(const dsd_rdio_api_config* api, const char* endpoint, const dsd_rdio_upload_inputs* inputs,
                             int timeout_ms, const char* system_str, CURLcode* out_res, long* out_http_code) {
    if (!api || !endpoint || !inputs || !system_str || !out_res || !out_http_code) {
        return -1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Rdio API upload: curl_easy_init failed\n");
        return -1;
    }

    curl_mime* mime = curl_mime_init(curl);
    if (!mime) {
        curl_easy_cleanup(curl);
        LOG_ERROR("Rdio API upload: curl_mime_init failed\n");
        return -1;
    }

    dsd_rdio_add_mime_parts(mime, api, system_str, inputs);
    dsd_rdio_configure_curl_request(curl, mime, endpoint, timeout_ms);

    *out_res = dsd_curl_perform_cancellable(curl, dsd_rdio_cancel_pred, NULL);
    *out_http_code = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return 0;
}

/* 0 = delivered, DSD_RDIO_UPLOAD_CANCELLED = aborted, 1 = failed (retry). */
static int
dsd_rdio_upload_attempt_outcome(CURLcode res, long http_code) {
    if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
        return 0;
    }
    if (res == CURLE_ABORTED_BY_CALLBACK) {
        return DSD_RDIO_UPLOAD_CANCELLED;
    }
    return 1;
}

/* The upload's attempts to @p endpoint (built from the API URL), as dsd_rdio_upload_trunk_recorder() returns them. */
static int
dsd_rdio_upload_attempts(const dsd_rdio_api_config* api, const char* endpoint, const dsd_rdio_upload_inputs* inputs) {
    int timeout_ms = 0;
    int attempts = 0;
    if (dsd_rdio_normalize_upload_options(api, &timeout_ms, &attempts) != 0) {
        return -1;
    }

    if (dsd_curl_global_ready() != 0) {
        return -1;
    }

    char system_str[32];
    DSD_SNPRINTF(system_str, sizeof(system_str), "%d", api->system_id);

    for (int attempt = 1; attempt <= attempts; attempt++) {
        if (atomic_load(&g_rdio_upload_cancel) != 0) {
            return DSD_RDIO_UPLOAD_CANCELLED;
        }
        CURLcode res = CURLE_FAILED_INIT;
        long http_code = 0;
        if (dsd_rdio_upload_attempt_once(api, endpoint, inputs, timeout_ms, system_str, &res, &http_code) != 0) {
            return -1;
        }

        int outcome = dsd_rdio_upload_attempt_outcome(res, http_code);
        if (outcome != 1) {
            return outcome;
        }

        LOG_WARN("Rdio API upload attempt %d/%d failed (curl=%d, http=%ld)\n", attempt, attempts, (int)res, http_code);

        if (attempt < attempts) {
            dsd_sleep_ms(DSD_RDIO_RETRY_BACKOFF_MS);
        }
    }

    return -1;
}

static int
dsd_rdio_upload_trunk_recorder(const dsd_rdio_api_config* api, const dsd_rdio_upload_inputs* inputs) {
    if (!api || !inputs || !inputs->wav_path || !inputs->meta_path) {
        return -1;
    }

    if (dsd_rdio_validate_api_config(api) != 0) {
        return -1;
    }

    /* Built from the API URL, which can carry credentials: wiped on every exit. */
    char endpoint[1024];
    int rc = -1;
    if (dsd_rdio_build_api_endpoint(api, endpoint, sizeof(endpoint)) != 0) {
        LOG_ERROR("Rdio API upload: endpoint too long\n");
    } else {
        rc = dsd_rdio_upload_attempts(api, endpoint, inputs);
    }
    DSD_SECURE_ZERO(endpoint, sizeof endpoint);
    return rc;
}
#endif

int
dsd_rdio_mode_from_string(const char* s, int* mode_out) {
    if (!mode_out) {
        return -1;
    }
    if (!s || s[0] == '\0') {
        *mode_out = DSD_RDIO_MODE_OFF;
        return 0;
    }

    if (dsd_strcasecmp(s, "off") == 0) {
        *mode_out = DSD_RDIO_MODE_OFF;
        return 0;
    }
    if (dsd_strcasecmp(s, "dirwatch") == 0) {
        *mode_out = DSD_RDIO_MODE_DIRWATCH;
        return 0;
    }
    if (dsd_strcasecmp(s, "api") == 0) {
        *mode_out = DSD_RDIO_MODE_API;
        return 0;
    }
    if (dsd_strcasecmp(s, "both") == 0) {
        *mode_out = DSD_RDIO_MODE_BOTH;
        return 0;
    }

    return -1;
}

const char*
dsd_rdio_mode_to_string(int mode) {
    switch (mode) {
        case DSD_RDIO_MODE_OFF: return "off";
        case DSD_RDIO_MODE_DIRWATCH: return "dirwatch";
        case DSD_RDIO_MODE_API: return "api";
        case DSD_RDIO_MODE_BOTH: return "both";
        default: return "off";
    }
}

int
dsd_rdio_journal_keeps_uploads(const dsd_opts* opts) {
    return opts && opts->wav_journal_file[0] != '\0' && opts->rdio_api_delete_after_upload ? 1 : 0;
}

int
dsd_rdio_warn_journal_keeps_uploads(const dsd_opts* opts, int kept_before) {
    if (kept_before || !dsd_rdio_journal_keeps_uploads(opts)) {
        return 0;
    }
    LOG_WARN("WARNING: Rdio API uploads keep their files despite delete-after-upload: this run keeps a call recording "
             "journal (--wav-journal), whose owner manages the recordings' retention.\n");
    return 1;
}

int
dsd_rdio_export_call(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path, time_t opened) {
    return dsd_rdio_export_call_ex(opts, event_struct, wav_path, opened, 0, NULL);
}

#ifdef USE_CURL
/* Whether an API upload removes its WAV once uploaded: never beside DirWatch, nor when a recording journal's owner
   manages retention. The job carries the decision, made here on the caller's thread: the worker never reads the
   options. */
static int
rdio_api_removes_wav(const dsd_opts* opts, int wants_dirwatch) {
    return opts->rdio_api_delete_after_upload && !wants_dirwatch && !dsd_rdio_journal_keeps_uploads(opts);
}
#endif

/* The API side of an export whose sidecar waits, written and closed, at @p temp_meta_path: the sidecar is put in place
   under @p meta_path whatever becomes of the upload, then the upload to @p system_id, the system the sidecar names, is
   queued (*@p api_queued set). Beside DirWatch
   the job uploads from its own copies of the pair, made before the public sidecar appears, so a watch that takes and
   deletes the pair the moment its sidecar is there cannot take them first; a job whose copies cannot be made is not
   queued. A call with no system ID is not uploaded, and is no failure (dsd_rdio_validate_api_config()). */
static int
dsd_rdio_export_api(const dsd_opts* opts, int system_id, const char* wav_path, const char* meta_path,
                    const char* temp_meta_path, int* api_queued) {
#ifndef USE_CURL
    (void)wav_path;
    (void)api_queued;
    if (dsd_rdio_publish_temp_meta_file(temp_meta_path, meta_path) != 0) {
        return -1;
    }
    /* As with libcurl: the API key is checked first, and a call with no system ID is skipped, no failure (issue #931).
       Only an upload this build would have to send fails for want of libcurl. */
    if (opts->rdio_api_key[0] != '\0' && system_id <= 0) {
        dsd_rdio_note_no_system();
        return 0;
    }
    LOG_WARN("Rdio API upload requested but this build was compiled without libcurl support\n");
    return -1;
#else
    const int wants_dirwatch = dsd_rdio_mode_wants_dirwatch(opts->rdio_mode);
    if (opts->rdio_api_delete_after_upload && wants_dirwatch
        && atomic_exchange(&g_rdio_delete_after_upload_dirwatch_warned, 1) == 0) {
        LOG_WARN("Rdio API delete-after-upload ignored when DirWatch export is active\n");
    }
    dsd_rdio_upload_job* job = NULL;
    const int prepared =
        dsd_rdio_prepare_api_job(opts, system_id, wav_path, meta_path, wants_dirwatch ? temp_meta_path : NULL,
                                 !wants_dirwatch, rdio_api_removes_wav(opts, wants_dirwatch), &job);
    if (dsd_rdio_publish_temp_meta_file(temp_meta_path, meta_path) != 0) {
        dsd_rdio_upload_job_free(job);
        return -1;
    }
    const int queued = prepared == 0 ? dsd_rdio_enqueue_upload_job(job, wav_path) : prepared;
    if (queued != 0) {
        /* Not queued: the job goes (its private inputs, its hold), the public pair stays DirWatch's. */
        dsd_rdio_upload_job_free(job);
    }
    if (queued == DSD_RDIO_UPLOAD_CANCELLED) {
        /* The user's Stop, before or after the job was made: no upload of this run is sent any more. */
        LOG_INFO("Rdio API upload of %s not queued: uploads were stopped\n", wav_path);
        return 0;
    }
    if (queued == DSD_RDIO_UPLOAD_NO_SYSTEM) {
        return 0;
    }
    if (queued != 0) {
        return -1;
    }
    if (api_queued) {
        *api_queued = 1;
    }
    return 0;
#endif
}

int
dsd_rdio_export_call_ex(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path, time_t opened,
                        time_t started, int* api_queued) {
    return dsd_rdio_export_call_fallback(opts, event_struct, wav_path, opened, started, 0, api_queued);
}

int
dsd_rdio_export_call_fallback(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path,
                              time_t opened, time_t started, int allow_fallback, int* api_queued) {
    if (api_queued) {
        *api_queued = 0;
    }
    if (!opts || !wav_path || wav_path[0] == '\0') {
        return -1;
    }

    int mode = opts->rdio_mode;
    if (mode == DSD_RDIO_MODE_OFF) {
        return 0;
    }

    if (!dsd_rdio_mode_wants_dirwatch(mode) && !dsd_rdio_mode_wants_api(mode)) {
        return 0;
    }

    // One choice of system for the whole export: the sidecar (and so the API meta part) and the form's system part.
    const Event_History* event = dsd_rdio_staged_row(event_struct);
    const dsd_rdio_system_choice system = dsd_rdio_resolve_call_system(opts, event);
    dsd_rdio_meta_fields fields;
    dsd_rdio_meta_fields_from_event(opts, event, &system, opened, started, allow_fallback, &fields);
    char meta_path[DSD_RDIO_PATH_MAX];
    char temp_meta_path[DSD_RDIO_PATH_MAX];
    if (dsd_rdio_write_trunk_recorder_meta(opts, &fields, wav_path, meta_path, sizeof(meta_path), temp_meta_path,
                                           sizeof(temp_meta_path))
        != 0) {
        return -1;
    }
    if (dsd_rdio_mode_wants_api(mode)) {
        return dsd_rdio_export_api(opts, system.id, wav_path, meta_path, temp_meta_path, api_queued);
    }
    return dsd_rdio_publish_temp_meta_file(temp_meta_path, meta_path);
}
