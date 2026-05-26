// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"

#ifdef USE_CURL
#include <curl/curl.h>
#endif

#define DSD_RDIO_PATH_MAX         2048
#define DSD_RDIO_UPLOAD_QUEUE_MAX 128U

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
    char wav_path[DSD_RDIO_PATH_MAX];
    char meta_path[DSD_RDIO_PATH_MAX];
    struct dsd_rdio_upload_job* next;
} dsd_rdio_upload_job;

static dsd_mutex_t g_rdio_upload_mutex;
static dsd_cond_t g_rdio_upload_cond;
static dsd_thread_t g_rdio_upload_worker;
static dsd_rdio_upload_job* g_rdio_upload_head = NULL;
static dsd_rdio_upload_job* g_rdio_upload_tail = NULL;
static size_t g_rdio_upload_depth = 0;
static int g_rdio_upload_stop_requested = 0;
static atomic_int g_rdio_upload_state = 0; // 0=uninitialized, 1=initializing, 2=ready, 3=failed, 4=stopping
static atomic_int g_rdio_delete_after_upload_dirwatch_warned = 0;

static int dsd_rdio_upload_trunk_recorder(const dsd_rdio_api_config* api, const char* wav_path, const char* meta_path);

static void
dsd_rdio_remove_after_upload(const char* path, const char* kind) {
    if (!path || path[0] == '\0') {
        return;
    }
    if (remove(path) != 0 && errno != ENOENT) {
        LOG_WARN("Rdio API upload: failed to delete %s %s: %s\n", kind ? kind : "file", path, strerror(errno));
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

        if (g_rdio_upload_head == NULL && g_rdio_upload_stop_requested) {
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
        dsd_mutex_unlock(&g_rdio_upload_mutex);

#ifdef USE_CURL
        if (dsd_rdio_upload_trunk_recorder(&job->api, job->wav_path, job->meta_path) == 0) {
            if (job->remove_meta_on_success) {
                dsd_rdio_remove_after_upload(job->meta_path, "metadata");
            }
            if (job->remove_wav_on_success) {
                dsd_rdio_remove_after_upload(job->wav_path, "audio");
            }
        }
#else
        (void)dsd_rdio_upload_trunk_recorder(&job->api, job->wav_path, job->meta_path);
#endif
        free(job);
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
        if (dsd_mutex_init(&g_rdio_upload_mutex) != 0) {
            atomic_store(&g_rdio_upload_state, 3);
            return -1;
        }
        if (dsd_cond_init(&g_rdio_upload_cond) != 0) {
            (void)dsd_mutex_destroy(&g_rdio_upload_mutex);
            atomic_store(&g_rdio_upload_state, 3);
            return -1;
        }

        g_rdio_upload_head = NULL;
        g_rdio_upload_tail = NULL;
        g_rdio_upload_depth = 0;
        g_rdio_upload_stop_requested = 0;

        if (dsd_thread_create(&g_rdio_upload_worker, dsd_rdio_upload_worker_thread, NULL) != 0) {
            (void)dsd_cond_destroy(&g_rdio_upload_cond);
            (void)dsd_mutex_destroy(&g_rdio_upload_mutex);
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

static void
dsd_rdio_clear_upload_queue_locked(void) {
    while (g_rdio_upload_head != NULL) {
        dsd_rdio_upload_job* next = g_rdio_upload_head->next;
        free(g_rdio_upload_head);
        g_rdio_upload_head = next;
    }
    g_rdio_upload_tail = NULL;
    g_rdio_upload_depth = 0;
}

void
dsd_rdio_upload_shutdown(void) {
    for (;;) {
        int state = atomic_load(&g_rdio_upload_state);
        if (state == 0 || state == 3) {
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
        LOG_ERROR("Rdio export: failed to join upload worker during shutdown\n");
        atomic_store(&g_rdio_upload_state, 3);
        return;
    }

    dsd_mutex_lock(&g_rdio_upload_mutex);
    dsd_rdio_clear_upload_queue_locked();
    g_rdio_upload_stop_requested = 0;
    dsd_mutex_unlock(&g_rdio_upload_mutex);

    (void)dsd_cond_destroy(&g_rdio_upload_cond);
    (void)dsd_mutex_destroy(&g_rdio_upload_mutex);

    atomic_store(&g_rdio_upload_state, 0);
}

static void
dsd_rdio_copy_api_config(dsd_rdio_api_config* out, const dsd_opts* opts) {
    if (!out) {
        return;
    }

    DSD_MEMSET(out, 0, sizeof(*out));
    if (!opts) {
        return;
    }

    out->system_id = opts->rdio_system_id;
    out->upload_timeout_ms = opts->rdio_upload_timeout_ms;
    out->upload_retries = opts->rdio_upload_retries;
    DSD_SNPRINTF(out->api_url, sizeof(out->api_url), "%s",
                 opts->rdio_api_url[0] ? opts->rdio_api_url : "http://127.0.0.1:3000");
    out->api_url[sizeof(out->api_url) - 1] = '\0';
    DSD_SNPRINTF(out->api_key, sizeof(out->api_key), "%s", opts->rdio_api_key);
    out->api_key[sizeof(out->api_key) - 1] = '\0';
}

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
        LOG_WARN("Rdio API upload skipped: missing system ID\n");
        return -1;
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
        free(job);
        return -1;
    }
    *out_job = job;
    return 0;
}

static int
dsd_rdio_enqueue_upload_job(dsd_rdio_upload_job* job, const char* wav_path) {
    if (!job || !wav_path) {
        return -1;
    }
    dsd_mutex_lock(&g_rdio_upload_mutex);
    if (g_rdio_upload_stop_requested) {
        dsd_mutex_unlock(&g_rdio_upload_mutex);
        LOG_WARN("Rdio export: shutdown in progress, dropping %s\n", wav_path);
        return -1;
    }

    if (g_rdio_upload_depth >= DSD_RDIO_UPLOAD_QUEUE_MAX) {
        dsd_mutex_unlock(&g_rdio_upload_mutex);
        LOG_WARN("Rdio export: upload queue full, dropping %s\n", wav_path);
        return -1;
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

static int
dsd_rdio_enqueue_api_upload(const dsd_opts* opts, const char* wav_path, const char* meta_path,
                            int remove_meta_on_success, int remove_wav_on_success) {
    if (!opts || !wav_path || !meta_path) {
        return -1;
    }

    dsd_rdio_api_config api;
    dsd_rdio_copy_api_config(&api, opts);
    if (dsd_rdio_validate_api_config(&api) != 0) {
        return -1;
    }

    if (dsd_rdio_upload_worker_init() != 0) {
        LOG_ERROR("Rdio export: failed to initialize background upload worker\n");
        return -1;
    }

    dsd_rdio_upload_job* job = NULL;
    if (dsd_rdio_build_upload_job(&api, wav_path, meta_path, remove_meta_on_success, remove_wav_on_success, &job)
        != 0) {
        return -1;
    }
    if (dsd_rdio_enqueue_upload_job(job, wav_path) != 0) {
        free(job);
        return -1;
    }
    return 0;
}
#else
void
dsd_rdio_upload_shutdown(void) {}
#endif

static int
dsd_rdio_mode_wants_dirwatch(int mode) {
    return mode == DSD_RDIO_MODE_DIRWATCH || mode == DSD_RDIO_MODE_BOTH;
}

static int
dsd_rdio_mode_wants_api(int mode) {
    return mode == DSD_RDIO_MODE_API || mode == DSD_RDIO_MODE_BOTH;
}

static int
dsd_rdio_make_sidecar_path(const char* wav_path, char* out_meta_path, size_t out_meta_path_size) {
    if (!wav_path || !out_meta_path || out_meta_path_size == 0) {
        return -1;
    }

    char base_path[DSD_RDIO_PATH_MAX];
    DSD_SNPRINTF(base_path, sizeof(base_path), "%s", wav_path);
    base_path[sizeof(base_path) - 1] = '\0';

    char* dot = strrchr(base_path, '.');
    if (dot && dsd_strcasecmp(dot, ".wav") == 0) {
        *dot = '\0';
    }

    int n = DSD_SNPRINTF(out_meta_path, out_meta_path_size, "%s.json", base_path);
    if (n < 0 || (size_t)n >= out_meta_path_size) {
        return -1;
    }

    return 0;
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
    if (bytes > (uint32_t)LONG_MAX) {
        return -1;
    }
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

    FILE* fp = fopen(wav_path, "rb");
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
dsd_rdio_json_write_escaped(FILE* fp, const char* s) {
    const char* text = s ? s : "";
    for (size_t i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];
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
}

typedef struct {
    time_t start_time;
    uint32_t talkgroup;
    uint32_t source;
    uint32_t freq_hz;
    int encrypted;
    const char* talkgroup_tag;
    const char* short_name;
} dsd_rdio_meta_fields;

static void
dsd_rdio_meta_fields_from_event(const Event_History* event, dsd_rdio_meta_fields* out) {
    if (!out) {
        return;
    }
    out->start_time = time(NULL);
    out->talkgroup = 0;
    out->source = 0;
    out->freq_hz = 0;
    out->encrypted = 0;
    out->talkgroup_tag = "";
    out->short_name = "";

    if (!event) {
        return;
    }
    if (event->event_time > 0) {
        out->start_time = event->event_time;
    }
    out->talkgroup = event->target_id;
    out->source = event->source_id;
    out->freq_hz = event->channel;
    out->encrypted = event->enc ? 1 : 0;
    out->short_name = event->sysid_string;

    if (event->t_name[0] != '\0') {
        out->talkgroup_tag = event->t_name;
    } else if (event->tgt_str[0] != '\0') {
        out->talkgroup_tag = event->tgt_str;
    } else {
        out->talkgroup_tag = event->gi ? "PRIVATE" : "GROUP";
    }
}

static int
dsd_rdio_open_temp_meta_file(const char* out_meta_path, char* temp_meta_path, size_t temp_meta_path_size,
                             FILE** out_fp) {
    if (!out_meta_path || !temp_meta_path || temp_meta_path_size == 0 || !out_fp) {
        return -1;
    }
    int tn = DSD_SNPRINTF(temp_meta_path, temp_meta_path_size, "%s.tmp", out_meta_path);
    if (tn < 0 || (size_t)tn >= temp_meta_path_size) {
        LOG_ERROR("Rdio export: sidecar temp path too long\n");
        return -1;
    }

    FILE* fp = dsd_fopen_private(temp_meta_path, "wb");
    if (!fp) {
        LOG_ERROR("Rdio export: unable to open %s: %s\n", temp_meta_path, strerror(errno));
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
    DSD_FPRINTF(fp, "  \"freq\": %u,\n", fields->freq_hz > 1000000U ? fields->freq_hz : 0U);
    DSD_FPRINTF(fp, "  \"system\": %d,\n", opts->rdio_system_id > 0 ? opts->rdio_system_id : 0);
    DSD_FPRINTF(fp, "  \"short_name\": \"");
    dsd_rdio_json_write_escaped(fp, fields->short_name);
    DSD_FPRINTF(fp, "\",\n");
    DSD_FPRINTF(fp, "  \"emergency\": false,\n");
    DSD_FPRINTF(fp, "  \"encrypted\": %s\n", fields->encrypted ? "true" : "false");
    DSD_FPRINTF(fp, "}\n");
}

static int
dsd_rdio_finalize_meta_file(FILE* fp, const char* temp_meta_path, const char* out_meta_path) {
    if (!fp || !temp_meta_path || !out_meta_path) {
        return -1;
    }
    if (fclose(fp) != 0) {
        LOG_ERROR("Rdio export: failed closing %s: %s\n", temp_meta_path, strerror(errno));
        (void)remove(temp_meta_path);
        return -1;
    }

    if (rename(temp_meta_path, out_meta_path) != 0) {
        LOG_ERROR("Rdio export: failed renaming %s -> %s: %s\n", temp_meta_path, out_meta_path, strerror(errno));
        (void)remove(temp_meta_path);
        return -1;
    }
    return 0;
}

static int
dsd_rdio_write_trunk_recorder_meta(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path,
                                   char* out_meta_path, size_t out_meta_path_size) {
    if (!opts || !wav_path || wav_path[0] == '\0') {
        return -1;
    }

    if (dsd_rdio_make_sidecar_path(wav_path, out_meta_path, out_meta_path_size) != 0) {
        LOG_ERROR("Rdio export: failed to build sidecar path for %s\n", wav_path);
        return -1;
    }

    const Event_History* event = event_struct ? &event_struct->Event_History_Items[0] : NULL;
    dsd_rdio_meta_fields fields;
    dsd_rdio_meta_fields_from_event(event, &fields);
    if (fields.talkgroup == 0U) {
        LOG_WARN("Rdio export: skipped %s (missing talkgroup/target ID)\n", wav_path);
        return -1;
    }
    if (fields.start_time <= 0) {
        fields.start_time = time(NULL);
    }

    unsigned int duration_s = dsd_rdio_wav_duration_s(wav_path);
    time_t stop_time = fields.start_time + (time_t)duration_s;

    char temp_meta_path[DSD_RDIO_PATH_MAX];
    FILE* fp = NULL;
    if (dsd_rdio_open_temp_meta_file(out_meta_path, temp_meta_path, sizeof(temp_meta_path), &fp) != 0) {
        return -1;
    }

    dsd_rdio_write_meta_json(fp, opts, &fields, stop_time);
    return dsd_rdio_finalize_meta_file(fp, temp_meta_path, out_meta_path);
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

static int
dsd_rdio_ensure_curl_global_ready(void) {
    static int curl_global_state = 0; // 0=uninitialized, 1=ready, -1=failed
    if (curl_global_state == 0) {
        CURLcode gc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (gc != CURLE_OK) {
            LOG_ERROR("Rdio API upload: curl_global_init failed: %s\n", curl_easy_strerror(gc));
            curl_global_state = -1;
            return -1;
        }
        curl_global_state = 1;
    }
    return (curl_global_state < 0) ? -1 : 0;
}

static void
dsd_rdio_configure_curl_request(CURL* curl, curl_mime* mime, const char* endpoint, int timeout_ms) {
    curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dsd-neo/rdio-export");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dsd_rdio_discard_write_cb);
}

static void
dsd_rdio_add_mime_parts(curl_mime* mime, const dsd_rdio_api_config* api, const char* system_str, const char* wav_path,
                        const char* meta_path) {
    curl_mimepart* part = NULL;

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "key");
    curl_mime_data(part, api->api_key, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "system");
    curl_mime_data(part, system_str, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "audio");
    curl_mime_filedata(part, wav_path);
    curl_mime_type(part, "audio/wav");

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "meta");
    curl_mime_filedata(part, meta_path);
    curl_mime_type(part, "application/json");
}

static int
dsd_rdio_upload_attempt_once(const dsd_rdio_api_config* api, const char* endpoint, const char* wav_path,
                             const char* meta_path, int timeout_ms, const char* system_str, CURLcode* out_res,
                             long* out_http_code) {
    if (!api || !endpoint || !wav_path || !meta_path || !system_str || !out_res || !out_http_code) {
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

    dsd_rdio_add_mime_parts(mime, api, system_str, wav_path, meta_path);
    dsd_rdio_configure_curl_request(curl, mime, endpoint, timeout_ms);

    *out_res = curl_easy_perform(curl);
    *out_http_code = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);

    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return 0;
}

static int
dsd_rdio_upload_trunk_recorder(const dsd_rdio_api_config* api, const char* wav_path, const char* meta_path) {
    if (!api || !wav_path || !meta_path) {
        return -1;
    }

    if (dsd_rdio_validate_api_config(api) != 0) {
        return -1;
    }

    char endpoint[1024];
    if (dsd_rdio_build_api_endpoint(api, endpoint, sizeof(endpoint)) != 0) {
        LOG_ERROR("Rdio API upload: endpoint too long\n");
        return -1;
    }

    int timeout_ms = 0;
    int attempts = 0;
    if (dsd_rdio_normalize_upload_options(api, &timeout_ms, &attempts) != 0) {
        return -1;
    }

    if (dsd_rdio_ensure_curl_global_ready() != 0) {
        return -1;
    }

    char system_str[32];
    DSD_SNPRINTF(system_str, sizeof(system_str), "%d", api->system_id);

    for (int attempt = 1; attempt <= attempts; attempt++) {
        CURLcode res = CURLE_FAILED_INIT;
        long http_code = 0;
        if (dsd_rdio_upload_attempt_once(api, endpoint, wav_path, meta_path, timeout_ms, system_str, &res, &http_code)
            != 0) {
            return -1;
        }

        if (res == CURLE_OK && http_code >= 200 && http_code < 300) {
            return 0;
        }

        LOG_WARN("Rdio API upload attempt %d/%d failed (curl=%d, http=%ld)\n", attempt, attempts, (int)res, http_code);

        if (attempt < attempts) {
            dsd_sleep_ms(150U);
        }
    }

    return -1;
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
dsd_rdio_export_call(const dsd_opts* opts, const Event_History_I* event_struct, const char* wav_path) {
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

    char meta_path[DSD_RDIO_PATH_MAX];
    if (dsd_rdio_write_trunk_recorder_meta(opts, event_struct, wav_path, meta_path, sizeof(meta_path)) != 0) {
        return -1;
    }

    if (dsd_rdio_mode_wants_api(mode)) {
#ifndef USE_CURL
        LOG_WARN("Rdio API upload requested but this build was compiled without libcurl support\n");
        return -1;
#else
        int wants_dirwatch = dsd_rdio_mode_wants_dirwatch(mode);
        int remove_meta_on_success = !wants_dirwatch;
        int remove_wav_on_success = opts->rdio_api_delete_after_upload && !wants_dirwatch;
        if (opts->rdio_api_delete_after_upload && wants_dirwatch
            && atomic_exchange(&g_rdio_delete_after_upload_dirwatch_warned, 1) == 0) {
            LOG_WARN("Rdio API delete-after-upload ignored when DirWatch export is active\n");
        }
        if (dsd_rdio_enqueue_api_upload(opts, wav_path, meta_path, remove_meta_on_success, remove_wav_on_success)
            != 0) {
            return -1;
        }
#endif
    }

    return 0;
}
