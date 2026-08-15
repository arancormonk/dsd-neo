// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/io/iq_capture.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
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
#include "dsd-neo/core/safe_api.h"
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
    int queue_inited;
    int run;
    int writer_failed;

    dsd_mutex_t event_m;
    int event_m_inited;
    dsd_iq_event* events;
    size_t event_count;
    size_t event_cap;

    dsd_iq_capture_queue_entry* queue_entries;
    size_t q_head;
    size_t q_tail;
    size_t q_count;

    size_t* free_stack;
    size_t free_top;

    uint8_t* block_pool;
    size_t block_bytes;
    size_t block_count;

    /*
     * Staging block being packed by the producer. Submissions arrive far smaller
     * than block_bytes (an RTL dongle hands over ~8 KiB at a time), so bytes are
     * appended here and the block is published only once it is full. Without
     * this the queue would hold one submission per block regardless of size,
     * reducing an N-block pool to N submissions of buffering.
     */
    size_t fill_index;
    size_t fill_len;
    int fill_valid;
    /* Monotonic timestamp of the first byte staged into the current block, so
     * the writer thread can age out a block that stops filling. */
    uint64_t fill_started_ns;
    uint64_t flush_interval_ns;

    dsd_atomic_u64 accepted_bytes;
    dsd_atomic_u64 written_bytes;
    dsd_atomic_u64 dropped_bytes;
    dsd_atomic_u64 dropped_blocks;

    uint8_t cu8_carry;
    int cu8_carry_valid;

    /* Set on the submit path, which dsd_iq_capture_submit serialises on
     * event_m; read at close once the writer thread has been joined. */
    int size_limit_reached;

    uint64_t last_drop_warn_ns;
    char capture_started_utc[32];
};

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    writer_thread_fn(void* arg);

static void set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
set_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || err_buf_size == 0 || !fmt) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(err_buf, err_buf_size, fmt, ap);
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
    DSD_MEMCPY(dst, src, n + 1);
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
        DSD_MEMCPY(out_metadata_path, path, meta_len + 1);

        size_t data_len = meta_len - 5U;
        if (data_len == 0 || data_len + 1 > out_data_path_size) {
            set_error(err_buf, err_buf_size, "derived data path too long");
            return DSD_IQ_ERR_INVALID_ARG;
        }
        DSD_MEMCPY(out_data_path, path, data_len);
        out_data_path[data_len] = '\0';
    } else {
        size_t data_len = strlen(path);
        if (data_len + 1 > out_data_path_size) {
            set_error(err_buf, err_buf_size, "data path too long");
            return DSD_IQ_ERR_INVALID_ARG;
        }
        DSD_MEMCPY(out_data_path, path, data_len + 1);

        {
            const char* suffix = ".json";
            size_t meta_len = data_len + strlen(suffix);
            if (meta_len + 1 > out_metadata_path_size) {
                set_error(err_buf, err_buf_size, "metadata path too long");
                return DSD_IQ_ERR_INVALID_ARG;
            }
            DSD_SNPRINTF(out_metadata_path, out_metadata_path_size, "%s%s", path, suffix);
        }
    }
    return DSD_IQ_OK;
}

static int
json_simple_escape(unsigned char c, const char** out_esc) {
    if (!out_esc) {
        return -1;
    }
    *out_esc = NULL;
    switch (c) {
        case '\"': *out_esc = "\\\""; break;
        case '\\': *out_esc = "\\\\"; break;
        case '\b': *out_esc = "\\b"; break;
        case '\t': *out_esc = "\\t"; break;
        case '\n': *out_esc = "\\n"; break;
        case '\f': *out_esc = "\\f"; break;
        case '\r': *out_esc = "\\r"; break;
        default: break;
    }
    return 0;
}

static int
json_escape_write(FILE* out, const char* s) {
    if (!out || !s) {
        return -1;
    }
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; s[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)s[i];
        const char* esc = NULL;
        (void)json_simple_escape(c, &esc);
        if (esc) {
            if (fputs(esc, out) == EOF) {
                return -1;
            }
            continue;
        }
        if (c < 0x20U) {
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
            continue;
        }
        if (fputc((int)c, out) == EOF) {
            return -1;
        }
    }
    return 0;
}

static int
write_json_string_field(FILE* out, const char* key, const char* value, int is_last) {
    if (DSD_FPRINTF(out, "  \"%s\": \"", key) < 0) {
        return -1;
    }
    if (json_escape_write(out, value ? value : "") != 0) {
        return -1;
    }
    if (DSD_FPRINTF(out, "\"%s\n", is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_u64_field(FILE* out, const char* key, uint64_t value, int is_last) {
    if (DSD_FPRINTF(out, "  \"%s\": %" PRIu64 "%s\n", key, value, is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_i64_field(FILE* out, const char* key, int64_t value, int is_last) {
    if (DSD_FPRINTF(out, "  \"%s\": %" PRId64 "%s\n", key, value, is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_bool_field(FILE* out, const char* key, int value, int is_last) {
    if (DSD_FPRINTF(out, "  \"%s\": %s%s\n", key, value ? "true" : "false", is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static const char*
iq_event_kind_name(dsd_iq_event_kind kind) {
    switch (kind) {
        case DSD_IQ_EVENT_RETUNE: return "RETUNE";
        case DSD_IQ_EVENT_MUTE: return "MUTE";
        case DSD_IQ_EVENT_RESET: return "RESET";
        default: break;
    }
    return "UNKNOWN";
}

static int
write_json_event_frequency_fields(FILE* out, const dsd_iq_event* ev) {
    if (ev->kind != DSD_IQ_EVENT_RETUNE && ev->kind != DSD_IQ_EVENT_RESET) {
        return 0;
    }
    if (DSD_FPRINTF(out, "      \"center_frequency_hz\": %" PRIu64 ",\n", ev->center_frequency_hz) < 0) {
        return -1;
    }
    if (DSD_FPRINTF(out, "      \"capture_center_frequency_hz\": %" PRIu64 ",\n", ev->capture_center_frequency_hz)
        < 0) {
        return -1;
    }
    if (DSD_FPRINTF(out, "      \"sample_rate_hz\": %u,\n", ev->sample_rate_hz) < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_event_reason_field(FILE* out, const dsd_iq_event* ev) {
    if (DSD_FPRINTF(out, "      \"reason\": \"") < 0) {
        return -1;
    }
    if (json_escape_write(out, ev->reason) != 0) {
        return -1;
    }
    if (DSD_FPRINTF(out, "\"\n") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_event(FILE* out, const dsd_iq_event* ev, int is_last) {
    if (DSD_FPRINTF(out, "    {\n") < 0) {
        return -1;
    }
    if (DSD_FPRINTF(out, "      \"kind\": \"%s\",\n", iq_event_kind_name(ev->kind)) < 0) {
        return -1;
    }
    if (DSD_FPRINTF(out, "      \"byte_offset\": %" PRIu64 ",\n", ev->byte_offset) < 0) {
        return -1;
    }
    if (ev->kind == DSD_IQ_EVENT_MUTE) {
        if (DSD_FPRINTF(out, "      \"duration_bytes\": %" PRIu64 ",\n", ev->duration_bytes) < 0) {
            return -1;
        }
    }
    if (write_json_event_frequency_fields(out, ev) != 0) {
        return -1;
    }
    if (write_json_event_reason_field(out, ev) != 0) {
        return -1;
    }
    if (DSD_FPRINTF(out, "    }%s\n", is_last ? "" : ",") < 0) {
        return -1;
    }
    return 0;
}

static int
write_json_events_field(FILE* out, const dsd_iq_event* events, size_t event_count, int is_last) {
    if (DSD_FPRINTF(out, "  \"events\": [\n") < 0) {
        return -1;
    }
    for (size_t i = 0; i < event_count; i++) {
        if (write_json_event(out, &events[i], i + 1U == event_count) != 0) {
            return -1;
        }
    }
    if (DSD_FPRINTF(out, "  ]%s\n", is_last ? "" : ",") < 0) {
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

typedef struct {
    uint64_t data_bytes;
    uint64_t capture_drops;
    uint64_t capture_drop_blocks;
    uint64_t input_ring_drops;
    int contains_retunes;
    uint32_t capture_retune_count;
    int size_limit_reached;
} dsd_iq_capture_metadata_stats;

static const char*
metadata_data_basename(const char* path) {
    if (!path || path[0] == '\0') {
        return "";
    }
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    const char* base = slash;
    if (bslash && (!base || bslash > base)) {
        base = bslash;
    }
    if (!base || base[1] == '\0') {
        return path;
    }
    return base + 1;
}

static int
metadata_write_identity_fields(FILE* fp, const dsd_iq_capture_config* cfg, size_t event_count) {
    if (write_json_string_field(fp, "format", "dsd-neo-iq", 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "version", (event_count > 0U) ? 2U : 1U, 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "sample_format", dsd_iq_sample_format_name(cfg->format), 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "iq_order", "IQ", 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "endianness", (cfg->format == DSD_IQ_FORMAT_CU8) ? "none" : "little", 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "capture_stage", cfg->capture_stage, 0) != 0) {
        return -1;
    }
    return 0;
}

static int
metadata_write_chain_fields(FILE* fp, const dsd_iq_capture_config* cfg) {
    if (write_json_u64_field(fp, "sample_rate_hz", cfg->sample_rate_hz, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "center_frequency_hz", cfg->center_frequency_hz, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "capture_center_frequency_hz", cfg->capture_center_frequency_hz, 0) != 0) {
        return -1;
    }
    if (write_json_i64_field(fp, "ppm", cfg->ppm, 0) != 0) {
        return -1;
    }
    if (write_json_i64_field(fp, "tuner_gain_tenth_db", cfg->tuner_gain_tenth_db, 0) != 0) {
        return -1;
    }
    if (write_json_i64_field(fp, "rtl_dsp_bw_khz", cfg->rtl_dsp_bw_khz, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "base_decimation", cfg->base_decimation, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "post_downsample", cfg->post_downsample, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "demod_rate_hz", cfg->demod_rate_hz, 0) != 0) {
        return -1;
    }
    return 0;
}

static int
metadata_write_feature_fields(FILE* fp, const dsd_iq_capture_config* cfg, const dsd_iq_capture_metadata_stats* stats) {
    if (write_json_bool_field(fp, "offset_tuning_enabled", cfg->offset_tuning_enabled, 0) != 0) {
        return -1;
    }
    if (write_json_bool_field(fp, "fs4_shift_enabled", cfg->fs4_shift_enabled, 0) != 0) {
        return -1;
    }
    if (write_json_bool_field(fp, "combine_rotate_enabled", cfg->combine_rotate_enabled, 0) != 0) {
        return -1;
    }
    if (write_json_bool_field(fp, "muted_bytes_excluded", cfg->muted_bytes_excluded, 0) != 0) {
        return -1;
    }
    if (write_json_bool_field(fp, "contains_retunes", stats->contains_retunes, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "capture_retune_count", stats->capture_retune_count, 0) != 0) {
        return -1;
    }
    return 0;
}

static int
metadata_write_source_fields(FILE* fp, const dsd_iq_capture_config* cfg, const char* capture_started_utc) {
    if (write_json_string_field(fp, "source_backend", cfg->source_backend, 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "source_args", cfg->source_args, 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "capture_started_utc", capture_started_utc, 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "data_file", metadata_data_basename(cfg->data_path), 0) != 0) {
        return -1;
    }
    return 0;
}

static int
metadata_write_stats_fields(FILE* fp, const dsd_iq_capture_metadata_stats* stats, const char* notes,
                            size_t event_count) {
    if (write_json_u64_field(fp, "data_bytes", stats->data_bytes, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "capture_drops", stats->capture_drops, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "capture_drop_blocks", stats->capture_drop_blocks, 0) != 0) {
        return -1;
    }
    if (write_json_u64_field(fp, "input_ring_drops", stats->input_ring_drops, 0) != 0) {
        return -1;
    }
    /* Distinguishes "capture ended because the operator's size limit was
     * reached" from "capture is still running": both leave capture_drops at 0
     * and a file that simply stops growing. */
    if (write_json_bool_field(fp, "size_limit_reached", stats->size_limit_reached, 0) != 0) {
        return -1;
    }
    if (write_json_string_field(fp, "notes", notes ? notes : "", event_count == 0U) != 0) {
        return -1;
    }
    return 0;
}

static int
metadata_write_payload(FILE* fp, const dsd_iq_capture_config* cfg, const char* capture_started_utc,
                       const dsd_iq_capture_metadata_stats* stats, const char* notes, const dsd_iq_event* events,
                       size_t event_count) {
    if (DSD_FPRINTF(fp, "{\n") < 0) {
        return -1;
    }
    if (metadata_write_identity_fields(fp, cfg, event_count) != 0) {
        return -1;
    }
    if (metadata_write_chain_fields(fp, cfg) != 0) {
        return -1;
    }
    if (metadata_write_feature_fields(fp, cfg, stats) != 0) {
        return -1;
    }
    if (metadata_write_source_fields(fp, cfg, capture_started_utc) != 0) {
        return -1;
    }
    if (metadata_write_stats_fields(fp, stats, notes, event_count) != 0) {
        return -1;
    }
    if (event_count > 0U && write_json_events_field(fp, events, event_count, 1) != 0) {
        return -1;
    }
    if (DSD_FPRINTF(fp, "}\n") < 0) {
        return -1;
    }
    if (fflush(fp) != 0) {
        return -1;
    }
    return 0;
}

static int
metadata_write_file(const dsd_iq_capture_config* cfg, const char* capture_started_utc,
                    const dsd_iq_capture_metadata_stats* stats, const char* notes, const char* metadata_path,
                    const dsd_iq_event* events, size_t event_count, char* err_buf, size_t err_buf_size) {
    if (!cfg || !metadata_path || metadata_path[0] == '\0') {
        set_error(err_buf, err_buf_size, "invalid metadata writer arguments");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (!stats) {
        set_error(err_buf, err_buf_size, "invalid metadata writer stats");
        return DSD_IQ_ERR_INVALID_ARG;
    }

    char tmp_path[2304] = {0};
    FILE* fp = dsd_fopen_private_temp_for_replace(metadata_path, tmp_path, sizeof(tmp_path), "wb");
    if (!fp) {
        set_error(err_buf, err_buf_size, "failed to open metadata temp file for '%s': %s", metadata_path,
                  strerror(errno));
        return DSD_IQ_ERR_IO;
    }

    int write_rc = metadata_write_payload(fp, cfg, capture_started_utc, stats, notes, events, event_count);
    if (write_rc == 0) {
        int fd = dsd_fileno(fp);
        if (fd >= 0) {
            (void)dsd_fchmod(fd, 0600);
            if (dsd_fsync(fd) != 0) {
                write_rc = -1;
            }
        }
    }
    int close_rc = fclose(fp);
    if (close_rc != 0 || write_rc != 0) {
        (void)remove(tmp_path);
        set_error(err_buf, err_buf_size, "failed to write metadata '%s': %s", metadata_path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }

    if (dsd_replace_file_with_temp(tmp_path, metadata_path) != 0) {
        (void)remove(tmp_path);
        set_error(err_buf, err_buf_size, "failed to replace metadata '%s': %s", metadata_path, strerror(errno));
        return DSD_IQ_ERR_IO;
    }

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

/*
 * Remaining room under the configured size limit (--iq-capture-max-mb).
 *
 * Bytes refused because this budget is exhausted are deliberately NOT counted
 * as drops and do not raise the drop warning. They are not data loss: the file
 * is complete and contiguous up to the limit the operator asked for, and the
 * capture simply stops there. Counting them made a healthy capped capture
 * report hundreds of megabytes of "drops" for as long as the run continued
 * afterwards, which reads as a corrupt capture. capture_drops is reserved for
 * its one real meaning: the writer thread could not keep up.
 */
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
writer_has_budget_for(const struct dsd_iq_capture_writer* w, uint64_t needed) {
    uint64_t budget = writer_remaining_budget(w);
    return budget == UINT64_MAX || budget >= needed;
}

/*
 * Record that the size limit refused bytes for the first time.
 *
 * Reaching the limit is not data loss, so it must stay out of capture_drops --
 * but it does end the capture, and without its own signal an operator cannot
 * tell a completed capped capture from one that is still running. Called only
 * from the submit path, which dsd_iq_capture_submit serialises on event_m, so
 * the callback fires exactly once.
 */
static void
writer_note_size_limit(struct dsd_iq_capture_writer* w) {
    if (!w || w->size_limit_reached) {
        return;
    }
    w->size_limit_reached = 1;
    if (w->cfg.size_limit_cb) {
        w->cfg.size_limit_cb(w->cfg.size_limit_user, w->cfg.max_bytes);
    }
}

static int
writer_queue_init(struct dsd_iq_capture_writer* w) {
    if (!w || w->block_bytes == 0 || w->block_count == 0) {
        return DSD_IQ_ERR_QUEUE_INIT;
    }
    /* The pool is one flat allocation and every staging offset is derived from
     * the same product, so reject a geometry whose size would wrap. */
    if (w->block_count > (SIZE_MAX / w->block_bytes)) {
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    if (dsd_mutex_init(&w->q_m) != 0) {
        return DSD_IQ_ERR_QUEUE_INIT;
    }
    /* Monotonic: the writer thread waits on this with a deadline so it can age
     * out a staging block that has stopped filling. */
    if (dsd_cond_init_monotonic(&w->q_ready) != 0) {
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
        (void)dsd_cond_destroy(&w->q_ready);
        (void)dsd_mutex_destroy(&w->q_m);
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    w->q_head = 0;
    w->q_tail = 0;
    w->q_count = 0;
    w->fill_index = 0;
    w->fill_len = 0;
    w->fill_valid = 0;
    w->fill_started_ns = 0;
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
    w->fill_index = w->fill_len = 0;
    w->fill_valid = 0;
    w->fill_started_ns = 0;
    w->free_top = 0;
}

static void
writer_event_destroy(struct dsd_iq_capture_writer* w) {
    if (!w) {
        return;
    }
    if (w->event_m_inited) {
        (void)dsd_mutex_destroy(&w->event_m);
        w->event_m_inited = 0;
    }
    free(w->events);
    w->events = NULL;
    w->event_count = 0;
    w->event_cap = 0;
}

static int
writer_event_ensure_capacity(struct dsd_iq_capture_writer* w, size_t min_cap) {
    if (!w) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (w->event_cap >= min_cap) {
        return DSD_IQ_OK;
    }
    size_t new_cap = (w->event_cap == 0U) ? 8U : (w->event_cap * 2U);
    while (new_cap < min_cap) {
        if (new_cap > (SIZE_MAX / 2U)) {
            return DSD_IQ_ERR_ALLOC;
        }
        new_cap *= 2U;
    }
    dsd_iq_event* next = (dsd_iq_event*)realloc(w->events, new_cap * sizeof(*next));
    if (!next) {
        return DSD_IQ_ERR_ALLOC;
    }
    w->events = next;
    w->event_cap = new_cap;
    return DSD_IQ_OK;
}

static size_t
writer_event_find_retune_insert_at(const struct dsd_iq_capture_writer* w, const dsd_iq_event* event) {
    if (!w || !event || event->kind != DSD_IQ_EVENT_RETUNE || w->event_count == 0U) {
        return w ? w->event_count : 0U;
    }
    size_t insert_at = w->event_count;
    while (insert_at > 0U) {
        const dsd_iq_event* prev = &w->events[insert_at - 1U];
        if (prev->byte_offset != event->byte_offset || prev->kind != DSD_IQ_EVENT_MUTE) {
            break;
        }
        insert_at--;
    }
    return insert_at;
}

static int
writer_event_try_merge_mute(struct dsd_iq_capture_writer* w, const dsd_iq_event* event) {
    if (!w || !event || event->kind != DSD_IQ_EVENT_MUTE || w->event_count == 0U) {
        return 0;
    }
    dsd_iq_event* prev = &w->events[w->event_count - 1U];
    if (prev->kind != DSD_IQ_EVENT_MUTE || prev->byte_offset != event->byte_offset) {
        return 0;
    }
    if (UINT64_MAX - prev->duration_bytes < event->duration_bytes) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    prev->duration_bytes += event->duration_bytes;
    return 1;
}

static int
writer_event_insert_at(struct dsd_iq_capture_writer* w, const dsd_iq_event* event, size_t insert_at) {
    int rc = writer_event_ensure_capacity(w, w->event_count + 1U);
    if (rc != DSD_IQ_OK) {
        return rc;
    }
    if (insert_at < w->event_count) {
        DSD_MEMMOVE(&w->events[insert_at + 1U], &w->events[insert_at],
                    (w->event_count - insert_at) * sizeof(w->events[0]));
    }
    w->events[insert_at] = *event;
    w->event_count++;
    return DSD_IQ_OK;
}

static int
writer_event_append_locked(struct dsd_iq_capture_writer* w, const dsd_iq_event* event) {
    if (!w || !event) {
        return DSD_IQ_ERR_INVALID_ARG;
    }

    size_t insert_at = writer_event_find_retune_insert_at(w, event);
    if (insert_at < w->event_count) {
        return writer_event_insert_at(w, event, insert_at);
    }

    int merge_rc = writer_event_try_merge_mute(w, event);
    if (merge_rc < 0) {
        return merge_rc;
    }
    if (merge_rc > 0) {
        return DSD_IQ_OK;
    }

    return writer_event_insert_at(w, event, w->event_count);
}

static void
capture_open_cleanup(struct dsd_iq_capture_writer* w, int remove_data, int remove_meta) {
    if (!w) {
        return;
    }
    if (w->data_fp) {
        fclose(w->data_fp);
        w->data_fp = NULL;
    }
    if (remove_data && w->cfg.data_path[0] != '\0') {
        (void)remove(w->cfg.data_path);
    }
    if (remove_meta && w->cfg.metadata_path[0] != '\0') {
        (void)remove(w->cfg.metadata_path);
    }
    writer_event_destroy(w);
    writer_queue_destroy(w);
    free(w);
}

static int
capture_open_resolve_paths(struct dsd_iq_capture_writer* w, const dsd_iq_capture_config* cfg, char* err_buf,
                           size_t err_buf_size) {
    if (w->cfg.data_path[0] == '\0' && w->cfg.metadata_path[0] == '\0') {
        set_error(err_buf, err_buf_size, "capture paths are empty");
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (w->cfg.data_path[0] == '\0') {
        return dsd_iq_capture_derive_paths(w->cfg.metadata_path, w->cfg.data_path, sizeof(w->cfg.data_path),
                                           w->cfg.metadata_path, sizeof(w->cfg.metadata_path), err_buf, err_buf_size);
    }
    if (w->cfg.metadata_path[0] == '\0') {
        return dsd_iq_capture_derive_paths(w->cfg.data_path, w->cfg.data_path, sizeof(w->cfg.data_path),
                                           w->cfg.metadata_path, sizeof(w->cfg.metadata_path), err_buf, err_buf_size);
    }
    if (copy_string_checked(cfg->data_path, w->cfg.data_path, sizeof(w->cfg.data_path), err_buf, err_buf_size, "data")
            != DSD_IQ_OK
        || copy_string_checked(cfg->metadata_path, w->cfg.metadata_path, sizeof(w->cfg.metadata_path), err_buf,
                               err_buf_size, "metadata")
               != DSD_IQ_OK) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    return DSD_IQ_OK;
}

static int
capture_open_init_files(struct dsd_iq_capture_writer* w, char* err_buf, size_t err_buf_size) {
    dsd_iq_capture_metadata_stats initial_stats;
    DSD_MEMSET(&initial_stats, 0, sizeof(initial_stats));

    if (format_utc_now(w->capture_started_utc, sizeof(w->capture_started_utc)) != 0) {
        set_error(err_buf, err_buf_size, "failed to build UTC capture timestamp");
        return DSD_IQ_ERR_IO;
    }
    w->data_fp = dsd_fopen_private(w->cfg.data_path, "wb");
    if (!w->data_fp) {
        set_error(err_buf, err_buf_size, "failed to open capture data file '%s': %s", w->cfg.data_path,
                  strerror(errno));
        return DSD_IQ_ERR_IO;
    }
    return metadata_write_file(&w->cfg, w->capture_started_utc, &initial_stats, "", w->cfg.metadata_path, NULL, 0,
                               err_buf, err_buf_size);
}

static int
capture_open_start_worker(struct dsd_iq_capture_writer* w, char* err_buf, size_t err_buf_size) {
    if (dsd_thread_create(&w->writer_thread, writer_thread_fn, w) != 0) {
        set_error(err_buf, err_buf_size, "capture writer thread creation failed");
        return DSD_IQ_ERR_QUEUE_INIT;
    }
    w->writer_thread_started = 1;
    return DSD_IQ_OK;
}

static int
event_reason_has_control_bytes(const dsd_iq_event* event) {
    if (!event) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(event->reason) && event->reason[i] != '\0'; i++) {
        if ((unsigned char)event->reason[i] < 0x20U) {
            return 1;
        }
    }
    return 0;
}

static int
capture_event_kind_supported(dsd_iq_event_kind kind) {
    return kind == DSD_IQ_EVENT_RETUNE || kind == DSD_IQ_EVENT_MUTE || kind == DSD_IQ_EVENT_RESET;
}

static int
validate_mute_event_for_replay(const struct dsd_iq_capture_writer* writer, const dsd_iq_event* event) {
    size_t align = dsd_iq_sample_format_alignment_bytes(writer->cfg.format);
    if (event->duration_bytes == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (event_reason_has_control_bytes(event)) {
        return DSD_IQ_ERR_INVALID_META;
    }
    if (align > 0U && (event->duration_bytes % (uint64_t)align) != 0ULL) {
        return DSD_IQ_ERR_ALIGNMENT;
    }
    return DSD_IQ_OK;
}

static int
validate_retune_or_reset_event_for_replay(const struct dsd_iq_capture_writer* writer, const dsd_iq_event* event) {
    if (event_reason_has_control_bytes(event)) {
        return DSD_IQ_ERR_INVALID_META;
    }
    if (event->center_frequency_hz == 0 || event->capture_center_frequency_hz == 0 || event->sample_rate_hz == 0) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (event->sample_rate_hz != writer->cfg.sample_rate_hz) {
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    return DSD_IQ_OK;
}

static int
validate_capture_event_for_replay(const struct dsd_iq_capture_writer* writer, const dsd_iq_event* event) {
    if (!writer || !event) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (!capture_event_kind_supported(event->kind)) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (event->kind == DSD_IQ_EVENT_MUTE) {
        return validate_mute_event_for_replay(writer, event);
    }
    return validate_retune_or_reset_event_for_replay(writer, event);
}

/*
 * Return a block to the free stack. Caller holds w->q_m.
 *
 * Every block is in exactly one of the free stack, the published queue, the
 * writer thread's hand, or the staging slot, so free_top can never reach
 * block_count here. The bound is checked rather than assumed: if a future path
 * ever breaks that invariant this leaks a block instead of writing a size_t off
 * the end of free_stack on the shutdown path.
 */
static void
writer_pool_release_locked(struct dsd_iq_capture_writer* w, size_t block_index) {
    if (w->free_top < w->block_count) {
        w->free_stack[w->free_top++] = block_index;
    }
}

/* Blocks a byte count spans, for capture_drop_blocks. Every path that reports
 * dropped bytes without a real block behind them charges the same way, so the
 * counter keeps one meaning. */
static uint64_t
writer_blocks_for_bytes(const struct dsd_iq_capture_writer* w, uint64_t bytes) {
    if (bytes == 0 || w->block_bytes == 0) {
        return 0;
    }
    return (bytes / (uint64_t)w->block_bytes) + ((bytes % (uint64_t)w->block_bytes) != 0U ? 1U : 0U);
}

/* Hand the staging block to the writer thread. Caller holds w->q_m; the block
 * must be valid and non-empty, and a free queue slot is guaranteed because a
 * block can only be staged after being popped off the free stack. */
static void
writer_publish_fill_locked(struct dsd_iq_capture_writer* w) {
    w->queue_entries[w->q_tail].block_index = w->fill_index;
    w->queue_entries[w->q_tail].len = w->fill_len;
    w->q_tail = (w->q_tail + 1U) % w->block_count;
    w->q_count++;
    w->fill_valid = 0;
    w->fill_len = 0;
    dsd_cond_signal(&w->q_ready);
}

/*
 * Append a submission to the queue, packing it into the staging block so that
 * one pool block carries many submissions. Everything up to the point where the
 * pool is exhausted is accepted; the remainder is dropped as a single tail.
 *
 * q_m is taken and released once per block rather than held across the whole
 * submission, so the writer thread can dequeue and write while the producer is
 * still copying later blocks instead of being locked out for the length of the
 * submission. dsd_iq_capture_submit serialises submissions on event_m, so
 * releasing the lock between blocks cannot interleave two of them.
 *
 * This does not rescue a submission larger than the entire pool. The producer is
 * a real-time source callback and never blocks waiting for space -- blocking it
 * would stall the SDR pipeline and turn capture drops into input-ring drops, so
 * dropping the tail is the deliberate trade. An over-capacity submission is a
 * sign the pool is sized wrong (queue_block_bytes * queue_block_count), not
 * something this path can absorb.
 */
static int
writer_submit_bytes(struct dsd_iq_capture_writer* w, const uint8_t* data, size_t bytes) {
    if (!w || (!data && bytes > 0)) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (bytes == 0) {
        return DSD_IQ_OK;
    }

    uint64_t accepted = 0;
    uint64_t dropped_bytes = 0;
    uint64_t dropped_blocks = 0;
    int fatal = 0;

    while (bytes > 0) {
        dsd_mutex_lock(&w->q_m);
        if (w->writer_failed || !w->run) {
            fatal = 1;
            dsd_mutex_unlock(&w->q_m);
            break;
        }
        if (!w->fill_valid) {
            if (w->free_top == 0) {
                /* Writer thread is behind; the rest of this submission is lost. */
                dropped_bytes = (uint64_t)bytes;
                dropped_blocks = writer_blocks_for_bytes(w, dropped_bytes);
                dsd_mutex_unlock(&w->q_m);
                break;
            }
            w->fill_index = w->free_stack[--w->free_top];
            w->fill_len = 0;
            w->fill_valid = 1;
            w->fill_started_ns = dsd_time_monotonic_ns();
            /* Wake the writer thread so it swaps its indefinite wait for one
             * bounded by this block's flush deadline. */
            dsd_cond_signal(&w->q_ready);
        }

        size_t room = w->block_bytes - w->fill_len;
        size_t take = (bytes < room) ? bytes : room;
        DSD_MEMCPY(w->block_pool + (w->fill_index * w->block_bytes) + w->fill_len, data, take);
        w->fill_len += take;
        data += take;
        bytes -= take;
        accepted += (uint64_t)take;

        if (w->fill_len == w->block_bytes) {
            writer_publish_fill_locked(w);
        }
        dsd_mutex_unlock(&w->q_m);
    }

    if (accepted > 0) {
        (void)dsd_atomic_u64_fetch_add_relaxed(&w->accepted_bytes, accepted);
    }
    if (dropped_bytes > 0) {
        writer_count_drop(w, dropped_bytes, dropped_blocks);
    }
    if (fatal) {
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    return DSD_IQ_OK;
}

/*
 * Submit `bytes` clamped down to `alignment` and to the remaining size budget.
 * This is the one place the size limit is applied, so the rule below holds for
 * every format.
 *
 * *out_consumed, when requested, is how many input bytes the call took and the
 * caller must skip. A partial trailing sample -- `bytes` not being a whole
 * number of samples -- cannot be written and has no carry to hold it, so it is
 * real data loss and is counted as a drop. Bytes refused because the budget is
 * exhausted are not; see writer_remaining_budget().
 */
static int
writer_submit_aligned(struct dsd_iq_capture_writer* w, const uint8_t* data, size_t bytes, size_t alignment,
                      size_t* out_consumed) {
    size_t aligned_bytes = (size_t)truncate_to_alignment((uint64_t)bytes, alignment);
    size_t writable = aligned_bytes;
    uint64_t budget = writer_remaining_budget(w);

    if (out_consumed) {
        *out_consumed = 0;
    }
    if (budget != UINT64_MAX) {
        budget = truncate_to_alignment(budget, alignment);
        if ((uint64_t)writable > budget) {
            writable = (size_t)budget;
            writer_note_size_limit(w);
        }
    }

    if (writable > 0) {
        int rc = writer_submit_bytes(w, data, writable);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
    }
    if (out_consumed) {
        *out_consumed = writable;
    }
    if (bytes > aligned_bytes) {
        writer_count_drop(w, (uint64_t)(bytes - aligned_bytes), 1);
    }
    return DSD_IQ_OK;
}

static int
writer_submit_cu8(struct dsd_iq_capture_writer* w, const uint8_t* in, size_t bytes) {
    size_t consumed = 0;

    if (w->cu8_carry_valid) {
        if (bytes > 0) {
            uint8_t pair[2];
            pair[0] = w->cu8_carry;
            pair[1] = in[0];
            if (writer_has_budget_for(w, 2)) {
                int rc = writer_submit_bytes(w, pair, 2);
                if (rc != DSD_IQ_OK) {
                    return rc;
                }
            } else {
                /* Size limit reached, not a drop (see writer_remaining_budget). */
                writer_note_size_limit(w);
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
        /* Already a whole number of I/Q pairs, so writer_submit_aligned's
         * partial-sample drop cannot fire here; only the size budget clamps. */
        size_t aligned_available = (size_t)truncate_to_alignment((uint64_t)(bytes - consumed), 2);
        size_t accepted = 0;
        int rc = writer_submit_aligned(w, in + consumed, aligned_available, 2, &accepted);
        if (rc != DSD_IQ_OK) {
            return rc;
        }
        consumed += accepted;

        /* accepted < aligned_available means the budget clamped the write, so
         * everything after it is a size-limit truncation and in[consumed] is in
         * the middle of the buffer, not its odd tail: carrying it would splice
         * two non-adjacent samples together. */
        if (accepted == aligned_available && consumed < bytes) {
            if (writer_has_budget_for(w, 2)) {
                /* One genuine odd trailing byte: hold it to pair with the head
                 * of the next submission. */
                w->cu8_carry = in[consumed];
                w->cu8_carry_valid = 1;
            } else {
                writer_note_size_limit(w);
            }
        }
    }

    return DSD_IQ_OK;
}

/*
 * The writer can no longer reach the file. Stop the capture, hand every queued
 * and in-flight block back to the pool, and account for everything that will
 * now never be written.
 *
 * @param lost_bytes  Bytes already known lost, i.e. the tail of a short write;
 *                    queued blocks are added on top. Pass 0 when the amount is
 *                    not knowable here and dsd_iq_capture_close's reconciliation
 *                    against the file has to settle it.
 * @param in_hand     Block the writer thread is holding, or NULL.
 */
static void
writer_fail_and_drain(struct dsd_iq_capture_writer* w, uint64_t lost_bytes, uint64_t lost_blocks,
                      const dsd_iq_capture_queue_entry* in_hand) {
    dsd_mutex_lock(&w->q_m);
    w->writer_failed = 1;
    w->run = 0;

    while (w->q_count > 0) {
        dsd_iq_capture_queue_entry pending = w->queue_entries[w->q_head];
        w->q_head = (w->q_head + 1U) % w->block_count;
        w->q_count--;
        lost_bytes += (uint64_t)pending.len;
        lost_blocks += 1;
        writer_pool_release_locked(w, pending.block_index);
    }
    if (in_hand) {
        writer_pool_release_locked(w, in_hand->block_index);
    }
    dsd_cond_broadcast(&w->q_ready);
    dsd_mutex_unlock(&w->q_m);

    writer_count_drop(w, lost_bytes, lost_blocks);
}

/*
 * Publish a staging block that has stopped filling. Caller holds q_m.
 *
 * Packing means a partly filled block otherwise sits in memory for as long as
 * the stream stays quiet -- a muted channel or a reconfigure hold can park up to
 * block_bytes-1 bytes indefinitely, and every one of them is lost if the process
 * is killed. Ageing the block out bounds that window to flush_interval_ns.
 *
 * @return 1 if a block was published.
 */
static int
writer_flush_stale_fill_locked(struct dsd_iq_capture_writer* w, uint64_t now_ns) {
    if (!w->fill_valid || w->fill_len == 0) {
        return 0;
    }
    if (now_ns < w->fill_started_ns || (now_ns - w->fill_started_ns) < w->flush_interval_ns) {
        return 0;
    }
    writer_publish_fill_locked(w);
    return 1;
}

/* Wait for the next queued block. Caller holds q_m; returns 1 with *out_entry
 * populated, or 0 once the capture has stopped and the queue is drained. */
static int
writer_next_entry_locked(struct dsd_iq_capture_writer* w, dsd_iq_capture_queue_entry* out_entry) {
    for (;;) {
        if (w->q_count > 0) {
            *out_entry = w->queue_entries[w->q_head];
            w->q_head = (w->q_head + 1U) % w->block_count;
            w->q_count--;
            return 1;
        }
        if (!w->run) {
            return 0;
        }
        if (writer_flush_stale_fill_locked(w, dsd_time_monotonic_ns())) {
            continue;
        }
        if (w->fill_valid && w->fill_len > 0) {
            /* A block is filling: wake in time to age it out. */
            (void)dsd_cond_timedwait_monotonic(&w->q_ready, &w->q_m, w->fill_started_ns + w->flush_interval_ns);
        } else {
            /* Nothing staged, so nothing can go stale; the producer signals
             * q_ready when it opens a block as well as when it publishes one. */
            dsd_cond_wait(&w->q_ready, &w->q_m);
        }
    }
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
        int have_entry;
        DSD_MEMSET(&entry, 0, sizeof(entry));

        dsd_mutex_lock(&w->q_m);
        have_entry = writer_next_entry_locked(w, &entry);
        dsd_mutex_unlock(&w->q_m);

        if (!have_entry) {
            break;
        }

        {
            const uint8_t* src = w->block_pool + (entry.block_index * w->block_bytes);
            size_t n = fwrite(src, 1, entry.len, w->data_fp);

            /* n reaching entry.len only means the bytes are in the stdio buffer.
             * ferror() catches a failure that a previous buffered write hit, so
             * a capture that cannot reach the disk at all still stops here
             * instead of running to completion reporting zero drops. */
            if (n != entry.len || ferror(w->data_fp) != 0) {
                /* The n bytes fwrite() did take still belong in written_bytes;
                 * dsd_iq_capture_close reconciles the total against the file. */
                if (n > 0) {
                    (void)dsd_atomic_u64_fetch_add_relaxed(&w->written_bytes, (uint64_t)n);
                }
                writer_fail_and_drain(w, (uint64_t)(entry.len - n), 1, &entry);
                break;
            }
        }

        (void)dsd_atomic_u64_fetch_add_relaxed(&w->written_bytes, (uint64_t)entry.len);

        {
            int idle;
            dsd_mutex_lock(&w->q_m);
            writer_pool_release_locked(w, entry.block_index);
            idle = (w->q_count == 0);
            dsd_mutex_unlock(&w->q_m);

            /* Caught up, and a block was just written: push the stdio buffer out
             * too, so the bytes a crash can cost stay bounded by the same
             * interval that ages out the staging block, and so a write error
             * surfaces while there is still a capture left to stop. */
            if (idle) {
                if (fflush(w->data_fp) != 0 || ferror(w->data_fp) != 0) {
                    writer_fail_and_drain(w, 0, 0, NULL);
                    break;
                }
            }
        }
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
    if (w->cfg.queue_flush_interval_ms == 0) {
        /* A live stream fills a default 256 KiB block in well under 100 ms, so
         * this only ever fires on a stream that has actually gone quiet. */
        w->cfg.queue_flush_interval_ms = 500;
    }

    w->block_bytes = w->cfg.queue_block_bytes;
    w->block_count = w->cfg.queue_block_count;
    w->flush_interval_ns = (uint64_t)w->cfg.queue_flush_interval_ms * 1000000ULL;

    if (w->block_bytes == 0 || w->block_count == 0) {
        set_error(err_buf, err_buf_size, "invalid capture queue geometry");
        free(w);
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    {
        int rc = capture_open_resolve_paths(w, cfg, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            capture_open_cleanup(w, 0, 0);
            return rc;
        }
    }
    {
        int rc = capture_open_init_files(w, err_buf, err_buf_size);
        if (rc != DSD_IQ_OK) {
            capture_open_cleanup(w, 1, 0);
            return rc;
        }
    }

    dsd_atomic_u64_init(&w->accepted_bytes, 0);
    dsd_atomic_u64_init(&w->written_bytes, 0);
    dsd_atomic_u64_init(&w->dropped_bytes, 0);
    dsd_atomic_u64_init(&w->dropped_blocks, 0);

    if (dsd_mutex_init(&w->event_m) != 0) {
        set_error(err_buf, err_buf_size, "capture event mutex initialization failed");
        capture_open_cleanup(w, 1, 1);
        return DSD_IQ_ERR_QUEUE_INIT;
    }
    w->event_m_inited = 1;

    {
        int rc = writer_queue_init(w);
        if (rc != DSD_IQ_OK) {
            set_error(err_buf, err_buf_size, "capture queue initialization failed");
            capture_open_cleanup(w, 1, 1);
            return DSD_IQ_ERR_QUEUE_INIT;
        }
    }

    if (capture_open_start_worker(w, err_buf, err_buf_size) != DSD_IQ_OK) {
        w->run = 0;
        capture_open_cleanup(w, 1, 1);
        return DSD_IQ_ERR_QUEUE_INIT;
    }

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

    if (writer->event_m_inited) {
        dsd_mutex_lock(&writer->event_m);
    }

    int writer_failed = 0;
    dsd_mutex_lock(&writer->q_m);
    writer_failed = writer->writer_failed;
    dsd_mutex_unlock(&writer->q_m);
    if (writer_failed) {
        if (writer->event_m_inited) {
            dsd_mutex_unlock(&writer->event_m);
        }
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    int rc = DSD_IQ_OK;
    {
        const uint8_t* in = (const uint8_t*)data;
        switch (writer->cfg.format) {
            case DSD_IQ_FORMAT_CU8: rc = writer_submit_cu8(writer, in, bytes); break;
            case DSD_IQ_FORMAT_CF32: rc = writer_submit_aligned(writer, in, bytes, 8, NULL); break;
            case DSD_IQ_FORMAT_CS16: rc = writer_submit_aligned(writer, in, bytes, 4, NULL); break;
            default: rc = DSD_IQ_ERR_UNSUPPORTED_FMT; break;
        }
    }

    if (writer->event_m_inited) {
        dsd_mutex_unlock(&writer->event_m);
    }
    return rc;
}

int
dsd_iq_capture_record_event(dsd_iq_capture_writer* writer, const dsd_iq_event* event) {
    if (!writer || !event) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    if (!writer->event_m_inited) {
        return DSD_IQ_ERR_QUEUE_INIT;
    }

    int vrc = validate_capture_event_for_replay(writer, event);
    if (vrc != DSD_IQ_OK) {
        return vrc;
    }

    dsd_mutex_lock(&writer->event_m);
    if (writer->cfg.format == DSD_IQ_FORMAT_CU8 && writer->cu8_carry_valid) {
        writer_count_drop(writer, 1, 1);
        writer->cu8_carry_valid = 0;
    }

    dsd_iq_event stamped = *event;
    stamped.byte_offset = dsd_atomic_u64_load_relaxed(&writer->accepted_bytes);
    stamped.reason[sizeof(stamped.reason) - 1U] = '\0';
    int rc = writer_event_append_locked(writer, &stamped);
    dsd_mutex_unlock(&writer->event_m);
    return rc;
}

/*
 * Make data_bytes agree with the file that is actually on disk.
 *
 * fwrite() is buffered, so bytes it reported as written can still be lost when
 * the buffer is flushed, and how much of a partial flush reached the disk before
 * an ENOSPC is not observable from any return value. The file itself is the
 * authority: the shortfall is real loss and is charged as a drop. Without this,
 * a capture whose every write failed still advertises a full-size file and
 * capture_drops == 0.
 *
 * Only meaningful for a regular file. A device or FIFO has no size to compare
 * against, so its counters are left alone rather than zeroed.
 */
static void
writer_reconcile_written_bytes(struct dsd_iq_capture_writer* writer) {
    dsd_stat_t st;
    uint64_t written;
    uint64_t on_disk;

    if (dsd_stat_path(writer->cfg.data_path, &st) != 0 || !dsd_stat_is_regular(&st) || st.st_size < 0) {
        return;
    }
    written = dsd_atomic_u64_load_relaxed(&writer->written_bytes);
    on_disk = (uint64_t)st.st_size;
    if (on_disk >= written) {
        return;
    }
    dsd_atomic_u64_store_relaxed(&writer->written_bytes, on_disk);
    writer_count_drop(writer, written - on_disk, writer_blocks_for_bytes(writer, written - on_disk));
}

/*
 * Pin event offsets inside the bytes that actually reached the file.
 *
 * byte_offset is stamped from accepted_bytes, which runs ahead of written_bytes
 * whenever a staged block never made it to disk. iq_replay rejects the entire
 * metadata file for a single event past data_bytes ("byte_offset exceeds replay
 * bytes"), so the salvageable part of a truncated capture becomes unreadable
 * along with the rest. Clamping leaves every event before the truncation point
 * exact and pins the rest to the end of the data.
 *
 * Offsets have to stay sample-aligned and sorted; clamping a sorted sequence to
 * one aligned bound preserves both.
 *
 * @return 1 if any event was moved.
 */
static int
writer_clamp_event_offsets(struct dsd_iq_capture_writer* writer, uint64_t data_bytes) {
    size_t align = dsd_iq_sample_format_alignment_bytes(writer->cfg.format);
    uint64_t limit = (align > 0U) ? truncate_to_alignment(data_bytes, align) : data_bytes;
    int clamped = 0;

    for (size_t i = 0; i < writer->event_count; i++) {
        if (writer->events[i].byte_offset > limit) {
            writer->events[i].byte_offset = limit;
            clamped = 1;
        }
    }
    return clamped;
}

static void
writer_stop_thread(struct dsd_iq_capture_writer* writer) {
    if (!writer || !writer->queue_inited) {
        return;
    }
    uint64_t stranded = 0;

    dsd_mutex_lock(&writer->q_m);
    /* Publish the partially-filled staging block before the writer thread is
     * told to finish, otherwise the tail of the capture is silently discarded.
     * The thread only exits once the queue has drained, so the entry is written.
     * When the writer has already failed nothing more can reach the file, so the
     * staged bytes are counted as dropped instead. */
    if (writer->fill_valid && writer->fill_len > 0) {
        if (writer->writer_failed) {
            stranded = (uint64_t)writer->fill_len;
            writer_pool_release_locked(writer, writer->fill_index);
            writer->fill_valid = 0;
            writer->fill_len = 0;
        } else {
            writer_publish_fill_locked(writer);
        }
    }
    writer->run = 0;
    dsd_cond_broadcast(&writer->q_ready);
    dsd_mutex_unlock(&writer->q_m);

    if (writer->writer_thread_started) {
        dsd_thread_join(writer->writer_thread);
        writer->writer_thread_started = 0;
    }

    /* Only after the join: `stranded` is non-zero exactly when the writer thread
     * has just taken its own failure path, where it calls writer_count_drop
     * outside q_m. writer_maybe_emit_drop_warning's rate-limit timestamp is a
     * plain uint64_t, so the two calls must not overlap. */
    if (stranded > 0) {
        writer_count_drop(writer, stranded, 1);
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

    if (writer->event_m_inited) {
        dsd_mutex_lock(&writer->event_m);
    }
    if (writer->cu8_carry_valid) {
        writer_count_drop(writer, 1, 1);
        writer->cu8_carry_valid = 0;
    }
    if (writer->event_m_inited) {
        dsd_mutex_unlock(&writer->event_m);
    }

    writer_stop_thread(writer);

    if (writer->data_fp) {
        (void)fflush(writer->data_fp);
        (void)fclose(writer->data_fp);
        writer->data_fp = NULL;
    }
    /* After the close, so a failure that only surfaces there is still caught. */
    writer_reconcile_written_bytes(writer);

    {
        char err_buf[256];
        uint64_t written_bytes = dsd_atomic_u64_load_relaxed(&writer->written_bytes);
        uint64_t dropped_bytes;
        uint64_t dropped_blocks;
        int clamped_events = writer_clamp_event_offsets(writer, written_bytes);
        const char* notes = "";
        dsd_iq_capture_metadata_stats stats;

        if (writer->writer_failed) {
            notes = "capture ended early: the data file could not be fully written";
        } else if (clamped_events) {
            notes = "event byte_offsets clamped to data_bytes after capture drops";
        }

        dropped_bytes = dsd_atomic_u64_load_relaxed(&writer->dropped_bytes);
        dropped_blocks = dsd_atomic_u64_load_relaxed(&writer->dropped_blocks);

        DSD_MEMSET(&stats, 0, sizeof(stats));
        stats.data_bytes = written_bytes;
        stats.capture_drops = dropped_bytes;
        stats.capture_drop_blocks = dropped_blocks;
        stats.input_ring_drops = final_stats->input_ring_drops;
        stats.size_limit_reached = writer->size_limit_reached;
        uint32_t event_retune_count = 0;
        for (size_t i = 0; i < writer->event_count; i++) {
            if (writer->events[i].kind == DSD_IQ_EVENT_RETUNE && event_retune_count < UINT32_MAX) {
                event_retune_count++;
            }
        }
        uint32_t capture_retune_count = event_retune_count;
        if (final_stats->retune_count > capture_retune_count) {
            capture_retune_count = final_stats->retune_count;
        }
        stats.contains_retunes = capture_retune_count > 0 ? 1 : 0;
        stats.capture_retune_count = capture_retune_count;
        (void)metadata_write_file(&writer->cfg, writer->capture_started_utc, &stats, notes, writer->cfg.metadata_path,
                                  writer->events, writer->event_count, err_buf, sizeof(err_buf));
    }

    writer_queue_destroy(writer);
    writer_event_destroy(writer);
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
    writer_event_destroy(writer);
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
