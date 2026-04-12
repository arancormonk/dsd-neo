// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dsd-neo/io/iq_types.h"

#if DSD_PLATFORM_WIN_NATIVE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

typedef struct {
    size_t block_index;
    size_t len;
} dsd_iq_capture_queue_entry;

struct dsd_iq_capture_writer {
    dsd_iq_capture_config cfg;
    FILE* data_fp;
    dsd_thread_t writer_thread;
    int writer_thread_started;

    dsd_mutex_t q_m;
    dsd_cond_t q_ready;
    dsd_cond_t q_space;
    int queue_inited;
    int run;
    int writer_failed;

    dsd_iq_capture_queue_entry* queue_entries;
    size_t q_head;
    size_t q_tail;
    size_t q_count;

    size_t* free_stack;
    size_t free_top;

    uint8_t* block_pool;
    size_t block_bytes;
    size_t block_count;

    dsd_atomic_u64 accepted_bytes;
    dsd_atomic_u64 written_bytes;
    dsd_atomic_u64 dropped_bytes;
    dsd_atomic_u64 dropped_blocks;

    uint8_t cu8_carry;
    int cu8_carry_valid;

    uint64_t last_drop_warn_ns;
    char capture_started_utc[32];
};

static void
set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || err_buf_size == 0 || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
    (void)vsnprintf(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
    err_buf[err_buf_size - 1] = '\0';
}

static int
copy_string_checked(const char* src, char* dst, size_t dst_size, char* err_buf, size_t err_buf_size, const char* name) {
    if (!dst || dst_size == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (!src) {
        src = "";
    }
    size_t n = strlen(src);
    if (n + 1 > dst_size) {
        set_error(err_buf, err_buf_size, "%s path too long", (name) ? name : "string");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    memcpy(dst, src, n + 1);
    return DSD_IQ_OK;
}

static int
has_suffix(const char* s, const char* suffix) {
    if (!s || !suffix) {
        return 0;
    }
    size_t s_len = strlen(s);
    size_t suf_len = strlen(suffix);
    if (s_len < suf_len) {
        return 0;
    }
    return strcmp(s + (s_len - suf_len), suffix) == 0;
}

int
dsd_iq_capture_derive_paths(const char* path, char* out_data_path, size_t out_data_path_size, char* out_metadata_path,
                            size_t out_metadata_path_size, char* err_buf, size_t err_buf_size) {
    if (!path || path[0] == '\0' || !out_data_path || out_data_path_size == 0 || !out_metadata_path
        || out_metadata_path_size == 0) {
        set_error(err_buf, err_buf_size, "invalid capture path arguments");
        return DSD_IQ_ERR_INVALID_ARG;
    }

    if (has_suffix(path, ".json")) {
        size_t meta_len = strlen(path);
        if (meta_len + 1 > out_metadata_path_size) {
            set_error(err_buf, err_buf_size, "metadata path too long");
            return DSD_IQ_ERR_INVALID_ARG;
        }
        memcpy(out_metadata_path, path, meta_len + 1);

        size_t data_len = meta_len - 5U;
        if (data_len == 0 || data_len + 1 > out_data_path_size) {
            set_error(err_buf, err_buf_size, "derived data path too long");
            return DSD_IQ_ERR_INVALID_ARG;
        }
        memcpy(out_data_path, path, data_len);
        out_data_path[data_len] = '\0';
    } else {
        size_t data_len = strlen(path);
        if (data_len + 1 > out_data_path_size) {
            set_error(err_buf, err_buf_size, "data path too long");
            return DSD_IQ_ERR_INVALID_ARG;
        }
        memcpy(out_data_path, path, data_len + 1);

        {
            const char* suffix = ".json";
            size_t meta_len = data_len + strlen(suffix);
            if (meta_len + 1 > out_metadata_path_size) {
                set_error(err_buf, err_buf_size, "metadata path too long");
                return DSD_IQ_ERR_INVALID_ARG;
            }
            snprintf(out_metadata_path, out_metadata_path_size, "%s%s", path, suffix);
        }
    }
    return DSD_IQ_OK;
}

static int
json_escape_write(FILE* out, const char* s) {
    static const char* hex = "0123456789abcdef";
    if (!out || !s) {
        return -1;
    }
    for (size_t i = 0; s[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\"':
                if (fputs("\\\"", out) == EOF) {
                    return -1;
                }
                break;
            case '\\':
                if (fputs("\\\\", out) == EOF) {
                    return -1;
                }
                break;
            case '\b':
                if (fputs("\\b", out) == EOF) {
                    return -1;
                }
                break;
            case '\t':
                if (fputs("\\t", out) == EOF) {
                    return -1;
                }
                break;
            case '\n':
                if (fputs("\\n", out) == EOF) {
                    return -1;
                }
                break;
            case '\f':
                if (fputs("\\f", out) == EOF) {
                    return -1;
                }
                break;
            case '\r':
                if (fputs("\\r", out) == EOF) {
                    return -1;
                }
                break;
            default:
                if (c < 0x20) {
                    char tmp[7];
                    tmp[0] = '\\';
                    tmp[1] = 'u';
                    tmp[2] = '0';
                    tmp[3] = '0';
                    tmp[4] = hex[(c >> 4) & 0x0F];
                    tmp[5] = hex[c & 0x0F];
                    tmp[6] = '\0';
                    if (fputs(tmp, out) == EOF) {
                        return -1;
                    }
                } else {
                    if (fputc((int)c, out) == EOF) {
                        return -1;
                    }
                }
                break;
        }
    }
    return 0;
}

static int
write_json_string_field(FILE* out, const char* key, const char* value, int is_last) {
    if (fprintf(out, "  \"%s\": \"", key) < 0) {
        return -1;
    }
    if (json_escape_write(out, value ? value : "") != 0) {
        return -1;
    }
    if (fprintf(out, "\"%s\n", is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_u64_field(FILE* out, const char* key, uint64_t value, int is_last) {
    if (fprintf(out, "  \"%s\": %" PRIu64 "%s\n", key, value, is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_i64_field(FILE* out, const char* key, int64_t value, int is_last) {
    if (fprintf(out, "  \"%s\": %" PRId64 "%s\n", key, value, is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_bool_field(FILE* out, const char* key, int value, int is_last) {
    if (fprintf(out, "  \"%s\": %s%s\n", key, value ? "true" : "false", is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
format_utc_now(char* out, size_t out_size) {
    if (!out || out_size < 21) {
        return -1;
    }

    time_t now = time(NULL);
    struct tm tm_utc;
#if DSD_PLATFORM_WIN_NATIVE
    if (gmtime_s(&tm_utc, &now) != 0) {
        return -1;
    }
#else
    if (!gmtime_r(&now, &tm_utc)) {
        return -1;
    }
#endif
    if (strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        return -1;
    }
    return 0;
}

static int
metadata_write_file(const dsd_iq_capture_config* cfg, const char* capture_started_utc, uint64_t data_bytes,
                    uint64_t capture_drops, uint64_t capture_drop_blocks, uint64_t input_ring_drops,
                    int contains_retunes, uint32_t capture_retune_count, const char* notes, const char* metadata_path,
                    char* err_buf, size_t err_buf_size) {
    if (!cfg || !metadata_path || metadata_path[0] == '\0') {
        set_error(err_buf, err_buf_size, "invalid metadata writer arguments");
        return DSD_IQ_ERR_INVALID_ARG;
    }

    char tmp_path[2304];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", metadata_path) >= (int)sizeof(tmp_path)) {
        set_error(err_buf, err_buf_size, "metadata temp path too long");
        return DSD_IQ_ERR_INVALID_ARG;
    }

    FILE* fp = fopen(tmp_path, "wb");
    if (!fp) {
        set_error(err_buf, err_buf_size, "failed to open metadata temp file '%s': %s", tmp_path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }

    int io_err = 0;
    if (fprintf(fp, "{\n") < 0) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "format", "dsd-neo-iq", 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "version", 1, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "sample_format", dsd_iq_sample_format_name(cfg->format), 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "iq_order", "IQ", 0)) {
        io_err = 1;
    }
    if (!io_err
        && write_json_string_field(fp, "endianness", (cfg->format == DSD_IQ_FORMAT_CU8) ? "none" : "little", 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "capture_stage", cfg->capture_stage, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "sample_rate_hz", cfg->sample_rate_hz, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "center_frequency_hz", cfg->center_frequency_hz, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "capture_center_frequency_hz", cfg->capture_center_frequency_hz, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_i64_field(fp, "ppm", cfg->ppm, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_i64_field(fp, "tuner_gain_tenth_db", cfg->tuner_gain_tenth_db, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_i64_field(fp, "rtl_dsp_bw_khz", cfg->rtl_dsp_bw_khz, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "base_decimation", cfg->base_decimation, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "post_downsample", cfg->post_downsample, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "demod_rate_hz", cfg->demod_rate_hz, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_bool_field(fp, "offset_tuning_enabled", cfg->offset_tuning_enabled, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_bool_field(fp, "fs4_shift_enabled", cfg->fs4_shift_enabled, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_bool_field(fp, "combine_rotate_enabled", cfg->combine_rotate_enabled, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_bool_field(fp, "muted_bytes_excluded", cfg->muted_bytes_excluded, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_bool_field(fp, "contains_retunes", contains_retunes, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "capture_retune_count", capture_retune_count, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "source_backend", cfg->source_backend, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "source_args", cfg->source_args, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "capture_started_utc", capture_started_utc, 0)) {
        io_err = 1;
    }

    {
        const char* data_basename = cfg->data_path;
        const char* slash = strrchr(cfg->data_path, '/');
        const char* bslash = strrchr(cfg->data_path, '\\');
        if (slash || bslash) {
            const char* base = slash;
            if (bslash && (!base || bslash > base)) {
                base = bslash;
            }
            if (base && base[1] != '\0') {
                data_basename = base + 1;
            }
        }
        if (!io_err && write_json_string_field(fp, "data_file", data_basename, 0)) {
            io_err = 1;
        }
    }

    if (!io_err && write_json_u64_field(fp, "data_bytes", data_bytes, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "capture_drops", capture_drops, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "capture_drop_blocks", capture_drop_blocks, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_u64_field(fp, "input_ring_drops", input_ring_drops, 0)) {
        io_err = 1;
    }
    if (!io_err && write_json_string_field(fp, "notes", notes ? notes : "", 1)) {
        io_err = 1;
    }
    if (!io_err && fprintf(fp, "}\n") < 0) {
        io_err = 1;
    }
    if (fflush(fp) != 0) {
        io_err = 1;
    }

    if (fclose(fp) != 0 || io_err) {
        (void)remove(tmp_path);
        set_error(err_buf, err_buf_size, "failed to write metadata '%s': %s", metadata_path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }

#if DSD_PLATFORM_WIN_NATIVE
    if (!MoveFileExA(tmp_path, metadata_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        (void)remove(tmp_path);
        set_error(err_buf, err_buf_size, "failed to replace metadata '%s'", metadata_path);
        return DSD_IQ_ERR_IO;
    }
#else
    if (rename(tmp_path, metadata_path) != 0) {
        (void)remove(tmp_path);
        set_error(err_buf, err_buf_size, "failed to replace metadata '%s': %s", metadata_path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }
#endif

    return DSD_IQ_OK;
}

static int
validate_capture_cfg(const dsd_iq_capture_config* cfg, char* err_buf, size_t err_buf_size) {
    if (!cfg) {
        set_error(err_buf, err_buf_size, "capture config is null");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (cfg->sample_rate_hz == 0 || cfg->center_frequency_hz == 0 || cfg->capture_center_frequency_hz == 0) {
        set_error(err_buf, err_buf_size, "invalid capture frequency or sample rate");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (cfg->base_decimation == 0 || (cfg->base_decimation & (cfg->base_decimation - 1U)) != 0) {
        set_error(err_buf, err_buf_size, "base_decimation must be a power of two");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (cfg->post_downsample == 0) {
        set_error(err_buf, err_buf_size, "post_downsample must be > 0");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (cfg->demod_rate_hz == 0) {
        set_error(err_buf, err_buf_size, "demod_rate_hz must be > 0");
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    {
        uint64_t derived =
            (uint64_t)cfg->sample_rate_hz / (uint64_t)cfg->base_decimation / (uint64_t)cfg->post_downsample;
        if (derived != cfg->demod_rate_hz) {
            set_error(err_buf, err_buf_size,
                      "demod_rate_hz (%u) inconsistent with sample_rate/base_decimation/post_downsample",
                      cfg->demod_rate_hz);
            return DSD_IQ_ERR_RATE_CHAIN;
        }
    }
    if (cfg->format != DSD_IQ_FORMAT_CU8 && cfg->format != DSD_IQ_FORMAT_CF32 && cfg->format != DSD_IQ_FORMAT_CS16) {
        set_error(err_buf, err_buf_size, "unsupported sample format");
        return DSD_IQ_ERR_UNSUPPORTED_FMT;
    }
    if (cfg->capture_stage[0] == '\0') {
        set_error(err_buf, err_buf_size, "capture_stage is required");
        return DSD_IQ_ERR_INVALID_META;
    }
    if (cfg->source_backend[0] == '\0') {
        set_error(err_buf, err_buf_size, "source_backend is required");
        return DSD_IQ_ERR_INVALID_META;
    }
    return DSD_IQ_OK;
}

static uint64_t
truncate_to_alignment(uint64_t value, size_t align) {
    if (align == 0) {
        return 0;
    }
    return value - (value % (uint64_t)align);
}

static void
writer_maybe_emit_drop_warning(struct dsd_iq_capture_writer* w) {
    if (!w || !w->cfg.drop_warning_cb) {
        return;
    }
    uint64_t now_ns = dsd_time_monotonic_ns();
    if (w->last_drop_warn_ns != 0 && now_ns > w->last_drop_warn_ns && (now_ns - w->last_drop_warn_ns) < 1000000000ULL) {
        return;
    }
    w->last_drop_warn_ns = now_ns;
    w->cfg.drop_warning_cb(w->cfg.drop_warning_user, dsd_atomic_u64_load_relaxed(&w->dropped_bytes),
                           dsd_atomic_u64_load_relaxed(&w->dropped_blocks));
}

static void
writer_count_drop(struct dsd_iq_capture_writer* w, uint64_t dropped_bytes, uint64_t dropped_blocks) {
    if (!w || dropped_bytes == 0) {
        return;
    }
    (void)dsd_atomic_u64_fetch_add_relaxed(&w->dropped_bytes, dropped_bytes);
    if (dropped_blocks > 0) {
        (void)dsd_atomic_u64_fetch_add_relaxed(&w->dropped_blocks, dropped_blocks);
    }
    writer_maybe_emit_drop_warning(w);
}

static uint64_t
writer_remaining_budget(const struct dsd_iq_capture_writer* w) {
    if (!w || w->cfg.max_bytes == 0) {
        return UINT64_MAX;
    }
    {
        uint64_t used = dsd_atomic_u64_load_relaxed(&w->accepted_bytes);
        if (used >= w->cfg.max_bytes) {
            return 0;
        }
        return w->cfg.max_bytes - used;
    }
}

static int
writer_queue_init(struct dsd_iq_capture_writer* w) {
    if (!w || w->block_bytes == 0 || w->block_count == 0) {
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    if (dsd_mutex_init(&w->q_m) != 0) {
        return DSD_IQ_ERR_QUEUE_INIT;
    }
    if (dsd_cond_init(&w->q_ready) != 0) {
        (void)dsd_mutex_destroy(&w->q_m);
        return DSD_IQ_ERR_QUEUE_INIT;
    }
    if (dsd_cond_init(&w->q_space) != 0) {
        (void)dsd_cond_destroy(&w->q_ready);
        (void)dsd_mutex_destroy(&w->q_m);
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    w->queue_entries = (dsd_iq_capture_queue_entry*)calloc(w->block_count, sizeof(dsd_iq_capture_queue_entry));
    w->free_stack = (size_t*)calloc(w->block_count, sizeof(size_t));
    w->block_pool = (uint8_t*)malloc(w->block_count * w->block_bytes);

    if (!w->queue_entries || !w->free_stack || !w->block_pool) {
        free(w->queue_entries);
        w->queue_entries = NULL;
        free(w->free_stack);
        w->free_stack = NULL;
        free(w->block_pool);
        w->block_pool = NULL;
        (void)dsd_cond_destroy(&w->q_space);
        (void)dsd_cond_destroy(&w->q_ready);
        (void)dsd_mutex_destroy(&w->q_m);
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    w->q_head = 0;
    w->q_tail = 0;
    w->q_count = 0;
    w->free_top = w->block_count;
    for (size_t i = 0; i < w->block_count; i++) {
        w->free_stack[i] = w->block_count - 1U - i;
    }
    w->queue_inited = 1;
    w->run = 1;
    w->writer_failed = 0;
    return DSD_IQ_OK;
}

static void
writer_queue_destroy(struct dsd_iq_capture_writer* w) {
    if (!w) {
        return;
    }
    if (w->queue_inited) {
        (void)dsd_cond_destroy(&w->q_space);
        (void)dsd_cond_destroy(&w->q_ready);
        (void)dsd_mutex_destroy(&w->q_m);
        w->queue_inited = 0;
    }
    free(w->queue_entries);
    w->queue_entries = NULL;
    free(w->free_stack);
    w->free_stack = NULL;
    free(w->block_pool);
    w->block_pool = NULL;
    w->q_head = w->q_tail = w->q_count = 0;
    w->free_top = 0;
}

static int
writer_enqueue_chunk(struct dsd_iq_capture_writer* w, const uint8_t* data, size_t len) {
    if (!w || !data || len == 0) {
        return -1;
    }

    dsd_mutex_lock(&w->q_m);
    if (w->writer_failed || !w->run) {
        dsd_mutex_unlock(&w->q_m);
        return -1;
    }
    if (w->free_top == 0) {
        dsd_mutex_unlock(&w->q_m);
        return 0;
    }

    size_t block_index = w->free_stack[--w->free_top];
    uint8_t* dst = w->block_pool + (block_index * w->block_bytes);
    memcpy(dst, data, len);

    w->queue_entries[w->q_tail].block_index = block_index;
    w->queue_entries[w->q_tail].len = len;
    w->q_tail = (w->q_tail + 1U) % w->block_count;
    w->q_count++;

    dsd_cond_signal(&w->q_ready);
    dsd_mutex_unlock(&w->q_m);

    (void)dsd_atomic_u64_fetch_add_relaxed(&w->accepted_bytes, (uint64_t)len);
    return 1;
}

static int
writer_submit_bytes(struct dsd_iq_capture_writer* w, const uint8_t* data, size_t bytes) {
    if (!w || (!data && bytes > 0)) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (bytes == 0) {
        return DSD_IQ_OK;
    }

    uint64_t dropped_bytes = 0;
    uint64_t dropped_blocks = 0;

    while (bytes > 0) {
        size_t chunk = bytes;
        if (chunk > w->block_bytes) {
            chunk = w->block_bytes;
        }

        int rc = writer_enqueue_chunk(w, data, chunk);
        if (rc == 1) {
            data += chunk;
            bytes -= chunk;
            continue;
        }
        if (rc == 0) {
            dropped_bytes += (uint64_t)chunk;
            dropped_blocks += 1;
            data += chunk;
            bytes -= chunk;
            continue;
        }

        if (dropped_bytes > 0) {
            writer_count_drop(w, dropped_bytes, dropped_blocks);
        }
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    if (dropped_bytes > 0) {
        writer_count_drop(w, dropped_bytes, dropped_blocks);
    }

    return DSD_IQ_OK;
}

static int
writer_submit_aligned(struct dsd_iq_capture_writer* w, const uint8_t* data, size_t bytes, size_t alignment) {
    uint64_t budget = writer_remaining_budget(w);
    size_t writable = bytes;
    if (budget != UINT64_MAX) {
        budget = truncate_to_alignment(budget, alignment);
        if ((uint64_t)writable > budget) {
            writable = (size_t)budget;
        }
    }
    writable = (size_t)truncate_to_alignment((uint64_t)writable, alignment);

    if (writable > 0) {
        int rc = writer_submit_bytes(w, data, writable);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
    }
    if (writable < bytes) {
        writer_count_drop(w, (uint64_t)(bytes - writable), 1);
    }
    return DSD_IQ_OK;
}

static int
writer_submit_cu8(struct dsd_iq_capture_writer* w, const uint8_t* in, size_t bytes) {
    size_t consumed = 0;

    if (w->cu8_carry_valid) {
        if (bytes > 0) {
            uint8_t pair[2];
            uint64_t budget = writer_remaining_budget(w);
            pair[0] = w->cu8_carry;
            pair[1] = in[0];
            if (budget >= 2 || budget == UINT64_MAX) {
                int rc = writer_submit_bytes(w, pair, 2);
                if (rc != DSD_IQ_OK) {
                    return rc;
                }
            } else {
                writer_count_drop(w, 2, 1);
            }
            w->cu8_carry_valid = 0;
            consumed = 1;
        } else {
            return DSD_IQ_OK;
        }
    }

    if (consumed > bytes) {
        consumed = bytes;
    }

    {
        size_t remaining = bytes - consumed;
        size_t aligned_available = remaining & ~(size_t)1U;
        size_t aligned = aligned_available;

        uint64_t budget = writer_remaining_budget(w);
        if (budget != UINT64_MAX) {
            budget = truncate_to_alignment(budget, 2);
            if ((uint64_t)aligned > budget) {
                aligned = (size_t)budget;
            }
        }

        if (aligned > 0) {
            int rc = writer_submit_bytes(w, in + consumed, aligned);
            if (rc != DSD_IQ_OK) {
                return rc;
            }
        }
        if (aligned < aligned_available) {
            writer_count_drop(w, (uint64_t)(aligned_available - aligned), 1);
        }
        consumed += aligned;
    }

    if (consumed < bytes) {
        uint64_t budget = writer_remaining_budget(w);
        if (budget >= 2 || budget == UINT64_MAX) {
            w->cu8_carry = in[consumed];
            w->cu8_carry_valid = 1;
        } else {
            writer_count_drop(w, 1, 1);
        }
    }

    return DSD_IQ_OK;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    writer_thread_fn(void* arg) {
    struct dsd_iq_capture_writer* w = (struct dsd_iq_capture_writer*)arg;
    if (!w) {
        DSD_THREAD_RETURN;
    }

    for (;;) {
        dsd_iq_capture_queue_entry entry;
        memset(&entry, 0, sizeof(entry));

        dsd_mutex_lock(&w->q_m);
        while (w->q_count == 0 && w->run) {
            dsd_cond_wait(&w->q_ready, &w->q_m);
        }
        if (w->q_count == 0 && !w->run) {
            dsd_mutex_unlock(&w->q_m);
            break;
        }

        entry = w->queue_entries[w->q_head];
        w->q_head = (w->q_head + 1U) % w->block_count;
        w->q_count--;
        dsd_mutex_unlock(&w->q_m);

        {
            uint8_t* src = w->block_pool + (entry.block_index * w->block_bytes);
            size_t n = fwrite(src, 1, entry.len, w->data_fp);
            if (n != entry.len) {
                uint64_t dropped_bytes = (uint64_t)(entry.len - n);
                uint64_t dropped_blocks = 1;

                dsd_mutex_lock(&w->q_m);
                w->writer_failed = 1;
                w->run = 0;

                while (w->q_count > 0) {
                    dsd_iq_capture_queue_entry pending = w->queue_entries[w->q_head];
                    w->q_head = (w->q_head + 1U) % w->block_count;
                    w->q_count--;
                    dropped_bytes += (uint64_t)pending.len;
                    dropped_blocks += 1;
                    w->free_stack[w->free_top++] = pending.block_index;
                }
                w->free_stack[w->free_top++] = entry.block_index;
                dsd_cond_broadcast(&w->q_space);
                dsd_cond_broadcast(&w->q_ready);
                dsd_mutex_unlock(&w->q_m);

                writer_count_drop(w, dropped_bytes, dropped_blocks);
                break;
            }
        }

        (void)dsd_atomic_u64_fetch_add_relaxed(&w->written_bytes, (uint64_t)entry.len);

        dsd_mutex_lock(&w->q_m);
        w->free_stack[w->free_top++] = entry.block_index;
        dsd_cond_signal(&w->q_space);
        dsd_mutex_unlock(&w->q_m);
    }

    DSD_THREAD_RETURN;
}

int
dsd_iq_capture_open(const dsd_iq_capture_config* cfg, dsd_iq_capture_writer** out, char* err_buf, size_t err_buf_size) {
    if (!cfg || !out) {
        set_error(err_buf, err_buf_size, "invalid capture open arguments");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    *out = NULL;

    {
        int vrc = validate_capture_cfg(cfg, err_buf, err_buf_size);
        if (vrc != DSD_IQ_OK) {
            return vrc;
        }
    }

    struct dsd_iq_capture_writer* w = (struct dsd_iq_capture_writer*)calloc(1, sizeof(*w));
    if (!w) {
        set_error(err_buf, err_buf_size, "capture writer allocation failed");
        return DSD_IQ_ERR_ALLOC;
    }
    w->cfg = *cfg;

    if (w->cfg.queue_block_bytes == 0) {
        w->cfg.queue_block_bytes = 262144;
    }
    if (w->cfg.queue_block_count == 0) {
        w->cfg.queue_block_count = 16;
    }

    w->block_bytes = w->cfg.queue_block_bytes;
    w->block_count = w->cfg.queue_block_count;

    if (w->block_bytes == 0 || w->block_count == 0) {
        set_error(err_buf, err_buf_size, "invalid capture queue geometry");
        free(w);
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    if (w->cfg.data_path[0] == '\0' && w->cfg.metadata_path[0] == '\0') {
        set_error(err_buf, err_buf_size, "capture paths are empty");
        free(w);
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (w->cfg.data_path[0] == '\0') {
        int rc = dsd_iq_capture_derive_paths(w->cfg.metadata_path, w->cfg.data_path, sizeof(w->cfg.data_path),
                                             w->cfg.metadata_path, sizeof(w->cfg.metadata_path), err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            free(w);
            return rc;
        }
    } else if (w->cfg.metadata_path[0] == '\0') {
        int rc = dsd_iq_capture_derive_paths(w->cfg.data_path, w->cfg.data_path, sizeof(w->cfg.data_path),
                                             w->cfg.metadata_path, sizeof(w->cfg.metadata_path), err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            free(w);
            return rc;
        }
    } else {
        if (copy_string_checked(cfg->data_path, w->cfg.data_path, sizeof(w->cfg.data_path), err_buf, err_buf_size,
                                "data")
                != DSD_IQ_OK
            || copy_string_checked(cfg->metadata_path, w->cfg.metadata_path, sizeof(w->cfg.metadata_path), err_buf,
                                   err_buf_size, "metadata")
                   != DSD_IQ_OK) {
            free(w);
            return DSD_IQ_ERR_INVALID_ARG;
        }
    }

    if (format_utc_now(w->capture_started_utc, sizeof(w->capture_started_utc)) != 0) {
        set_error(err_buf, err_buf_size, "failed to build UTC capture timestamp");
        free(w);
        return DSD_IQ_ERR_IO;
    }

    w->data_fp = fopen(w->cfg.data_path, "wb");
    if (!w->data_fp) {
        set_error(err_buf, err_buf_size, "failed to open capture data file '%s': %s", w->cfg.data_path,
                  strerror(errno));
        free(w);
        return DSD_IQ_ERR_IO;
    }

    {
        int mrc = metadata_write_file(&w->cfg, w->capture_started_utc, 0, 0, 0, 0, 0, 0, "", w->cfg.metadata_path,
                                      err_buf, err_buf_size);
        if (mrc != DSD_IQ_OK) {
            fclose(w->data_fp);
            w->data_fp = NULL;
            (void)remove(w->cfg.data_path);
            free(w);
            return mrc;
        }
    }

    dsd_atomic_u64_init(&w->accepted_bytes, 0);
    dsd_atomic_u64_init(&w->written_bytes, 0);
    dsd_atomic_u64_init(&w->dropped_bytes, 0);
    dsd_atomic_u64_init(&w->dropped_blocks, 0);

    {
        int qrc = writer_queue_init(w);
        if (qrc != DSD_IQ_OK) {
            set_error(err_buf, err_buf_size, "capture queue initialization failed");
            fclose(w->data_fp);
            w->data_fp = NULL;
            (void)remove(w->cfg.data_path);
            (void)remove(w->cfg.metadata_path);
            writer_queue_destroy(w);
            free(w);
            return DSD_IQ_ERR_QUEUE_INIT;
        }
    }

    if (dsd_thread_create(&w->writer_thread, (dsd_thread_fn)writer_thread_fn, w) != 0) {
        set_error(err_buf, err_buf_size, "capture writer thread creation failed");
        w->run = 0;
        writer_queue_destroy(w);
        fclose(w->data_fp);
        w->data_fp = NULL;
        (void)remove(w->cfg.data_path);
        (void)remove(w->cfg.metadata_path);
        free(w);
        return DSD_IQ_ERR_QUEUE_INIT;
    }
    w->writer_thread_started = 1;

    *out = w;
    return DSD_IQ_OK;
}

int
dsd_iq_capture_submit(dsd_iq_capture_writer* writer, const void* data, size_t bytes) {
    if (!writer || (!data && bytes > 0)) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (!writer->data_fp || bytes == 0) {
        return DSD_IQ_OK;
    }

    {
        int writer_failed = 0;
        dsd_mutex_lock(&writer->q_m);
        writer_failed = writer->writer_failed;
        dsd_mutex_unlock(&writer->q_m);
        if (writer_failed) {
            return DSD_IQ_ERR_QUEUE_INIT;
        }
    }

    {
        const uint8_t* in = (const uint8_t*)data;
        switch (writer->cfg.format) {
            case DSD_IQ_FORMAT_CU8: return writer_submit_cu8(writer, in, bytes);
            case DSD_IQ_FORMAT_CF32: return writer_submit_aligned(writer, in, bytes, 8);
            case DSD_IQ_FORMAT_CS16: return writer_submit_aligned(writer, in, bytes, 4);
            default: return DSD_IQ_ERR_UNSUPPORTED_FMT;
        }
    }
}

static void
writer_stop_thread(struct dsd_iq_capture_writer* writer) {
    if (!writer || !writer->queue_inited) {
        return;
    }
    dsd_mutex_lock(&writer->q_m);
    writer->run = 0;
    dsd_cond_broadcast(&writer->q_ready);
    dsd_cond_broadcast(&writer->q_space);
    dsd_mutex_unlock(&writer->q_m);

    if (writer->writer_thread_started) {
        dsd_thread_join(writer->writer_thread);
        writer->writer_thread_started = 0;
    }
}

void
dsd_iq_capture_close(dsd_iq_capture_writer* writer, const dsd_iq_capture_final_stats* final_stats) {
    if (!writer) {
        return;
    }
    if (!final_stats) {
        dsd_iq_capture_abort(writer);
        return;
    }

    if (writer->cu8_carry_valid) {
        writer_count_drop(writer, 1, 1);
        writer->cu8_carry_valid = 0;
    }

    writer_stop_thread(writer);

    if (writer->data_fp) {
        (void)fflush(writer->data_fp);
        (void)fclose(writer->data_fp);
        writer->data_fp = NULL;
    }

    {
        char err_buf[256];
        uint64_t written_bytes = dsd_atomic_u64_load_relaxed(&writer->written_bytes);
        uint64_t dropped_bytes = dsd_atomic_u64_load_relaxed(&writer->dropped_bytes);
        uint64_t dropped_blocks = dsd_atomic_u64_load_relaxed(&writer->dropped_blocks);
        (void)metadata_write_file(&writer->cfg, writer->capture_started_utc, written_bytes, dropped_bytes,
                                  dropped_blocks, final_stats->input_ring_drops, final_stats->retune_count > 0 ? 1 : 0,
                                  final_stats->retune_count, "", writer->cfg.metadata_path, err_buf, sizeof(err_buf));
    }

    writer_queue_destroy(writer);
    free(writer);
}

void
dsd_iq_capture_abort(dsd_iq_capture_writer* writer) {
    if (!writer) {
        return;
    }

    writer_stop_thread(writer);

    if (writer->data_fp) {
        (void)fclose(writer->data_fp);
        writer->data_fp = NULL;
    }
    (void)remove(writer->cfg.metadata_path);

    writer_queue_destroy(writer);
    free(writer);
}

size_t
dsd_iq_sample_format_alignment_bytes(dsd_iq_sample_format format) {
    switch (format) {
        case DSD_IQ_FORMAT_CU8: return 2U;
        case DSD_IQ_FORMAT_CF32: return 8U;
        case DSD_IQ_FORMAT_CS16: return 4U;
        default: break;
    }
    return 0U;
}

const char*
dsd_iq_sample_format_name(dsd_iq_sample_format format) {
    switch (format) {
        case DSD_IQ_FORMAT_CU8: return "cu8";
        case DSD_IQ_FORMAT_CF32: return "cf32";
        case DSD_IQ_FORMAT_CS16: return "cs16";
        default: break;
    }
    return "unknown";
}
