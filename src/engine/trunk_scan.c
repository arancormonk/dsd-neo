// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/path_policy.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/protocol/p25/p25_cc_candidates.h"

#if LONG_MAX < 4294967295LL
#define DSD_TRUNK_SCAN_MAX_FREQUENCY_HZ ((uint32_t)LONG_MAX)
#else
#define DSD_TRUNK_SCAN_MAX_FREQUENCY_HZ UINT32_MAX
#endif

typedef struct {
    unsigned long long p2_wacn;
    unsigned long long p2_sysid;
    unsigned long long p2_cc;
    unsigned long long p2_siteid;
    unsigned long long p2_rfssid;
    long int p25_cc_freq;
    long int trunk_cc_freq;
    uint64_t trunk_chan_map_seq;
    time_t p25_sys_time;
    long p25_cc_eval_freq;
    double p25_cc_eval_start_m;
    time_t last_cc_sync_time;
    time_t last_vc_sync_time;
    time_t p25_last_vc_tune_time;
    time_t last_t3_tune_time;
    double last_cc_sync_time_m;
    double last_vc_sync_time_m;
    double p25_last_vc_tune_time_m;
    double last_t3_tune_time_m;
    long int p25_vc_freq[2];
    long int trunk_vc_freq[2];
    time_t p25_patch_last_update[8];
    long int trunk_lcn_freq[26];
    dsd_trunk_cc_candidates cc_candidates;
    p25_nb_entry_t p25_nb_entries[P25_NB_MAX];
    p25_iden_entry_t p25_iden_fdma[16];
    p25_iden_entry_t p25_iden_tdma[16];
    time_t p25_aff_last_seen[256];
    time_t p25_ga_last_seen[512];
    long int trunk_chan_map[DSD_TRUNK_CHAN_MAP_SIZE];
    uint32_t trunk_chan_map_used_count;
    unsigned int dmr_color_code;
    int p25_chan_iden;
    int p25_cc_is_tdma;
    int p25_sys_is_tdma;
    int p25_vc_cqpsk_pref;
    int p25_vc_cqpsk_override;
    int samplesPerSymbol;
    int symbolCenter;
    int rf_mod;
    int p25_patch_count;
    int p25_aff_count;
    int p25_ga_count;
    int p25_nb_count;
    uint32_t p25_src_nid;
    int dmr_mfid;
    int dmr_rest_channel;
    int lcn_freq_count;
    int lcn_freq_roll;
    int is_con_plus;
    int has_cc_candidates;
    unsigned int dmr_fid;
    unsigned int dmr_so;
    unsigned int dmr_fidR;
    unsigned int dmr_soR;
    char dmr_site_parms[200];
    char dmr_branding[20];
    char dmr_branding_sub[80];
    uint32_t p25_patch_wuid[8][8];
    uint32_t p25_aff_rid[256];
    uint32_t p25_ga_rid[512];
    uint16_t p25_prot_kid;
    int16_t p25_sys_time_offset;
    uint16_t p25_patch_sgid[8];
    uint16_t p25_patch_key[8];
    uint16_t p25_patch_wgid[8][8];
    uint16_t p25_ga_tg[512];
    uint16_t trunk_chan_map_used[DSD_TRUNK_CHAN_MAP_SIZE];
    uint8_t p25_prot_valid;
    uint8_t p25_prot_algid;
    uint8_t p25_sys_time_valid;
    uint8_t p25_sys_time_offset_valid;
    uint8_t p25_cc_cache_loaded;
    uint8_t p25_call_emergency[2];
    uint8_t p25_call_priority[2];
    uint8_t p25_call_is_packet[2];
    uint8_t p25_patch_is_patch[8];
    uint8_t p25_patch_active[8];
    uint8_t p25_patch_wgid_count[8];
    uint8_t p25_patch_wuid_count[8];
    uint8_t p25_patch_alg[8];
    uint8_t p25_patch_ssn[8];
    uint8_t p25_patch_key_valid[8];
    uint8_t dmr_confidence_locked;
    uint8_t dmr_confidence_color_code;
    uint8_t dmr_confidence_candidate_cc;
    uint8_t dmr_confidence_candidate_count;
    uint8_t dmr_confidence_voice_sync_seen[2];
    uint8_t dmr_confidence_voice_open[2];
    uint8_t dmr_confidence_voice_count[2];
    uint8_t dmr_confidence_mismatch_count;
    uint8_t p25_chan_tdma_explicit[16];
    uint8_t dmr_lcn_trust[0x1000];
} dsd_trunk_scan_snapshot;

typedef struct {
    dsd_trunk_scan_target target;
    dsd_trunk_scan_snapshot snapshot;
    p25_sm_ctx_t p25_ctx;
    dmr_sm_ctx_t dmr_ctx;
    double parked_since_m;
    double idle_since_m;
    double retry_until_m;
    double last_allowed_activity_m;
} dsd_trunk_scan_target_runtime;

typedef struct {
    dsd_trunk_scan_target_runtime targets[DSD_TRUNK_SCAN_MAX_TARGETS];
    size_t count;
    size_t active;
    int saved_p25_trunk;
    int saved_trunk_enable;
    int saved_p25_is_tuned;
    int saved_trunk_is_tuned;
    uint64_t last_trunk_chan_map_seq;
} dsd_trunk_scan_coord;

static dsd_trunk_scan_coord* g_trunk_scan_coord;
static int g_trunk_scan_now_override;
static double g_trunk_scan_now_m;
static const char k_trunk_scan_csv_header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes";

static double
trunk_scan_now_m(void) {
    return g_trunk_scan_now_override ? g_trunk_scan_now_m : dsd_time_now_monotonic_s();
}

void
dsd_engine_trunk_scan_test_set_now(double now_m) {
    g_trunk_scan_now_override = 1;
    g_trunk_scan_now_m = now_m;
}

void
dsd_engine_trunk_scan_test_clear_now(void) {
    g_trunk_scan_now_override = 0;
    g_trunk_scan_now_m = 0.0;
}

static void scan_set_error(char* err, size_t err_sz, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
scan_set_error(char* err, size_t err_sz, const char* fmt, ...) {
    if (!err || err_sz == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(err, err_sz, fmt, ap);
    va_end(ap);
    err[err_sz - 1] = '\0';
}

static char*
scan_trim(char* s) {
    if (!s) {
        return s;
    }
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

static char*
scan_unquote(char* s) {
    s = scan_trim(s);
    size_t n = s ? strlen(s) : 0;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        s++;
    }
    return s;
}

static int
scan_parse_u32_decimal(const char* s, uint32_t min_value, uint32_t max_value, uint32_t* out) {
    if (!s || !out || s[0] == '\0') {
        return -1;
    }
    for (const char* p = s; *p; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
    }
    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || (end && *end != '\0') || value < (unsigned long)min_value
        || value > (unsigned long)max_value) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int
scan_default_ms(int configured, int fallback) {
    if (configured < DSD_TRUNK_SCAN_DWELL_MIN_MS || configured > DSD_TRUNK_SCAN_DWELL_MAX_MS) {
        return fallback;
    }
    return configured;
}

static int
scan_parse_ms_field(const char* s, int default_ms, int* out) {
    uint32_t parsed = 0;
    if (!s || s[0] == '\0') {
        *out = default_ms;
        return 0;
    }
    if (scan_parse_u32_decimal(s, DSD_TRUNK_SCAN_DWELL_MIN_MS, DSD_TRUNK_SCAN_DWELL_MAX_MS, &parsed) != 0) {
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

static int
scan_parse_type(const char* s, dsd_trunk_scan_target_type* out) {
    if (!s || !out) {
        return -1;
    }
    if (strcmp(s, "p25-trunk") == 0) {
        *out = DSD_TRUNK_SCAN_TARGET_P25_TRUNK;
        return 0;
    }
    if (strcmp(s, "dmr-trunk") == 0) {
        *out = DSD_TRUNK_SCAN_TARGET_DMR_TRUNK;
        return 0;
    }
    if (strcmp(s, "dmr-conventional") == 0) {
        *out = DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL;
        return 0;
    }
    return -1;
}

static int
scan_split_row(char* line, char* fields[7]) {
    char* p = line;
    for (int i = 0; i < 6; i++) {
        char* comma = NULL;
        int in_quote = 0;
        for (char* q = p; *q; q++) {
            if (*q == '"') {
                in_quote = !in_quote;
            } else if (*q == ',' && !in_quote) {
                comma = q;
                break;
            }
        }
        if (!comma) {
            return -1;
        }
        *comma = '\0';
        fields[i] = p;
        p = comma + 1;
    }
    fields[6] = p;
    return 0;
}

static int
scan_has_duplicate_id(const dsd_trunk_scan_target_list* list, const char* id) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->targets[i].id, id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
scan_has_duplicate_type_freq(const dsd_trunk_scan_target_list* list, dsd_trunk_scan_target_type type,
                             uint32_t frequency_hz) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->targets[i].type == type && list->targets[i].frequency_hz == frequency_hz) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    const char* resolved_path;
    int default_dwell_ms;
    int default_hold_ms;
    unsigned int row;
    char* err;
    size_t err_sz;
} dsd_trunk_scan_row_parse;

static int
scan_parse_target_row(char* line, dsd_trunk_scan_target_list* parsed, const dsd_trunk_scan_row_parse* parse) {
    char* fields[7] = {0};
    if (scan_split_row(line, fields) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u must contain 7 CSV fields", parse->row);
        return -1;
    }

    const char* id = scan_unquote(fields[0]);
    const char* type_s = scan_unquote(fields[1]);
    const char* freq_s = scan_unquote(fields[2]);
    const char* chan_csv = scan_unquote(fields[3]);
    const char* dwell_s = scan_unquote(fields[4]);
    const char* hold_s = scan_unquote(fields[5]);

    if (id[0] == '\0' || strlen(id) >= sizeof(parsed->targets[0].id)) {
        scan_set_error(parse->err, parse->err_sz, "row %u has an empty or too-long id", parse->row);
        return -1;
    }
    if (scan_has_duplicate_id(parsed, id)) {
        scan_set_error(parse->err, parse->err_sz, "row %u duplicates trunk scan target id '%s'", parse->row, id);
        return -1;
    }

    dsd_trunk_scan_target target;
    DSD_MEMSET(&target, 0, sizeof(target));
    DSD_SNPRINTF(target.id, sizeof target.id, "%s", id);
    if (scan_parse_type(type_s, &target.type) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid target type '%s'", parse->row, type_s);
        return -1;
    }
    if (scan_parse_u32_decimal(freq_s, 1U, DSD_TRUNK_SCAN_MAX_FREQUENCY_HZ, &target.frequency_hz) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid frequency_hz '%s'", parse->row, freq_s);
        return -1;
    }
    if (scan_has_duplicate_type_freq(parsed, target.type, target.frequency_hz)) {
        scan_set_error(parse->err, parse->err_sz, "row %u duplicates target type/frequency", parse->row);
        return -1;
    }

    if (chan_csv[0] != '\0') {
        if (target.type == DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL) {
            scan_set_error(parse->err, parse->err_sz, "row %u sets chan_csv for conventional DMR target", parse->row);
            return -1;
        }
        if (dsd_path_resolve_relative_to_file(parse->resolved_path, chan_csv, target.chan_csv, sizeof target.chan_csv)
            != 0) {
            scan_set_error(parse->err, parse->err_sz, "row %u chan_csv path is too long or invalid", parse->row);
            return -1;
        }
    }
    if (scan_parse_ms_field(dwell_s, parse->default_dwell_ms, &target.dwell_ms) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid dwell_ms '%s'", parse->row, dwell_s);
        return -1;
    }
    if (scan_parse_ms_field(hold_s, parse->default_hold_ms, &target.activity_hold_ms) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid activity_hold_ms '%s'", parse->row, hold_s);
        return -1;
    }

    parsed->targets[parsed->count++] = target;
    return 0;
}

static int
scan_read_target_csv_header(FILE* fp, char* line, size_t line_sz, char* err, size_t err_sz) {
    if (!fgets(line, line_sz, fp)) {
        scan_set_error(err, err_sz, "trunk scan target CSV is empty");
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';

    size_t header_len = strlen(k_trunk_scan_csv_header);
    if (strncmp(line, k_trunk_scan_csv_header, header_len) != 0
        || (line[header_len] != '\0' && line[header_len] != ',')) {
        scan_set_error(err, err_sz, "trunk scan target CSV header must start with '%s'", k_trunk_scan_csv_header);
        return -1;
    }
    return 0;
}

static int
scan_load_target_csv_rows(FILE* fp, char* line, size_t line_sz, dsd_trunk_scan_target_list* parsed,
                          dsd_trunk_scan_row_parse* parse) {
    unsigned int row = 1;
    while (fgets(line, line_sz, fp)) {
        row++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            scan_set_error(parse->err, parse->err_sz, "row %u is too long", row);
            return -1;
        }
        line[strcspn(line, "\r\n")] = '\0';
        char* trimmed_line = scan_trim(line);
        if (trimmed_line[0] == '\0') {
            continue;
        }
        if (parsed->count >= DSD_TRUNK_SCAN_MAX_TARGETS) {
            scan_set_error(parse->err, parse->err_sz, "too many trunk scan targets (max %d)",
                           DSD_TRUNK_SCAN_MAX_TARGETS);
            return -1;
        }

        parse->row = row;
        if (scan_parse_target_row(trimmed_line, parsed, parse) != 0) {
            return -1;
        }
    }
    return 0;
}

int
dsd_trunk_scan_load_targets_csv(const char* path, const dsd_opts* opts, dsd_trunk_scan_target_list* out, char* err,
                                size_t err_sz) {
    dsd_trunk_scan_target_list parsed;
    DSD_MEMSET(&parsed, 0, sizeof(parsed));
    if (!path || path[0] == '\0' || !out) {
        scan_set_error(err, err_sz, "trunk scan target CSV path is required");
        return -1;
    }

    char resolved_path[2048];
    FILE* fp = dsd_path_fopen_user_read_file(path, resolved_path, sizeof resolved_path);
    if (!fp) {
        scan_set_error(err, err_sz, "failed to open trunk scan target CSV '%s'", path);
        return -1;
    }

    char line[4096];
    if (scan_read_target_csv_header(fp, line, sizeof line, err, err_sz) != 0) {
        fclose(fp);
        return -1;
    }

    dsd_trunk_scan_row_parse parse;
    parse.resolved_path = resolved_path;
    parse.default_dwell_ms =
        scan_default_ms(opts ? opts->trunk_scan_idle_dwell_ms : 0, DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS);
    parse.default_hold_ms =
        scan_default_ms(opts ? opts->trunk_scan_activity_hold_ms : 0, DSD_TRUNK_SCAN_ACTIVITY_HOLD_DEFAULT_MS);
    parse.err = err;
    parse.err_sz = err_sz;

    int rows_rc = scan_load_target_csv_rows(fp, line, sizeof line, &parsed, &parse);
    fclose(fp);
    if (rows_rc != 0) {
        return -1;
    }
    if (parsed.count == 0) {
        scan_set_error(err, err_sz, "trunk scan target CSV has no targets");
        return -1;
    }
    *out = parsed;
    return 0;
}

static void
trunk_scan_snapshot_clear(dsd_trunk_scan_snapshot* snapshot) {
    DSD_MEMSET(snapshot, 0, sizeof(*snapshot));
    snapshot->dmr_mfid = -1;
    snapshot->dmr_color_code = 16;
    snapshot->dmr_confidence_color_code = 16;
    snapshot->dmr_confidence_candidate_cc = 16;
    snapshot->dmr_rest_channel = -1;
    snapshot->p25_cc_is_tdma = 2;
    snapshot->p25_vc_cqpsk_pref = -1;
    snapshot->p25_vc_cqpsk_override = -1;
}

static void
trunk_scan_save_dmr_confidence_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    snapshot->dmr_color_code = state->dmr_color_code;
    snapshot->dmr_confidence_locked = state->dmr_confidence_locked;
    snapshot->dmr_confidence_color_code = state->dmr_confidence_color_code;
    snapshot->dmr_confidence_candidate_cc = state->dmr_confidence_candidate_cc;
    snapshot->dmr_confidence_candidate_count = state->dmr_confidence_candidate_count;
    DSD_MEMCPY(snapshot->dmr_confidence_voice_sync_seen, state->dmr_confidence_voice_sync_seen,
               sizeof(snapshot->dmr_confidence_voice_sync_seen));
    DSD_MEMCPY(snapshot->dmr_confidence_voice_open, state->dmr_confidence_voice_open,
               sizeof(snapshot->dmr_confidence_voice_open));
    DSD_MEMCPY(snapshot->dmr_confidence_voice_count, state->dmr_confidence_voice_count,
               sizeof(snapshot->dmr_confidence_voice_count));
    snapshot->dmr_confidence_mismatch_count = state->dmr_confidence_mismatch_count;
}

static void
trunk_scan_restore_dmr_confidence_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    state->dmr_color_code = snapshot->dmr_color_code;
    state->dmr_confidence_locked = snapshot->dmr_confidence_locked;
    state->dmr_confidence_color_code = snapshot->dmr_confidence_color_code;
    state->dmr_confidence_candidate_cc = snapshot->dmr_confidence_candidate_cc;
    state->dmr_confidence_candidate_count = snapshot->dmr_confidence_candidate_count;
    DSD_MEMCPY(state->dmr_confidence_voice_sync_seen, snapshot->dmr_confidence_voice_sync_seen,
               sizeof(state->dmr_confidence_voice_sync_seen));
    DSD_MEMCPY(state->dmr_confidence_voice_open, snapshot->dmr_confidence_voice_open,
               sizeof(state->dmr_confidence_voice_open));
    DSD_MEMCPY(state->dmr_confidence_voice_count, snapshot->dmr_confidence_voice_count,
               sizeof(state->dmr_confidence_voice_count));
    state->dmr_confidence_mismatch_count = snapshot->dmr_confidence_mismatch_count;
}

static void
trunk_scan_save_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    snapshot->p2_wacn = state->p2_wacn;
    snapshot->p2_sysid = state->p2_sysid;
    snapshot->p2_cc = state->p2_cc;
    snapshot->p2_siteid = state->p2_siteid;
    snapshot->p2_rfssid = state->p2_rfssid;
    snapshot->p25_cc_freq = state->p25_cc_freq;
    snapshot->trunk_cc_freq = state->trunk_cc_freq;
    DSD_MEMCPY(snapshot->p25_vc_freq, state->p25_vc_freq, sizeof(snapshot->p25_vc_freq));
    DSD_MEMCPY(snapshot->trunk_vc_freq, state->trunk_vc_freq, sizeof(snapshot->trunk_vc_freq));
    DSD_MEMCPY(snapshot->trunk_lcn_freq, state->trunk_lcn_freq, sizeof(snapshot->trunk_lcn_freq));
    DSD_MEMCPY(snapshot->trunk_chan_map, state->trunk_chan_map, sizeof(snapshot->trunk_chan_map));
    DSD_MEMCPY(snapshot->trunk_chan_map_used, state->trunk_chan_map_used, sizeof(snapshot->trunk_chan_map_used));
    snapshot->trunk_chan_map_used_count = state->trunk_chan_map_used_count;
    snapshot->trunk_chan_map_seq = state->trunk_chan_map_seq;
    DSD_MEMCPY(snapshot->dmr_lcn_trust, state->dmr_lcn_trust, sizeof(snapshot->dmr_lcn_trust));
    DSD_MEMCPY(snapshot->p25_chan_tdma_explicit, state->p25_chan_tdma_explicit,
               sizeof(snapshot->p25_chan_tdma_explicit));
    snapshot->p25_chan_iden = state->p25_chan_iden;
    DSD_MEMCPY(snapshot->p25_iden_fdma, state->p25_iden_fdma, sizeof(snapshot->p25_iden_fdma));
    DSD_MEMCPY(snapshot->p25_iden_tdma, state->p25_iden_tdma, sizeof(snapshot->p25_iden_tdma));
    snapshot->p25_cc_is_tdma = state->p25_cc_is_tdma;
    snapshot->p25_sys_is_tdma = state->p25_sys_is_tdma;
    snapshot->p25_vc_cqpsk_pref = state->p25_vc_cqpsk_pref;
    snapshot->p25_vc_cqpsk_override = state->p25_vc_cqpsk_override;
    snapshot->samplesPerSymbol = state->samplesPerSymbol;
    snapshot->symbolCenter = state->symbolCenter;
    snapshot->rf_mod = state->rf_mod;
    snapshot->p25_prot_valid = state->p25_prot_valid;
    snapshot->p25_prot_algid = state->p25_prot_algid;
    snapshot->p25_prot_kid = state->p25_prot_kid;
    snapshot->p25_sys_time_valid = state->p25_sys_time_valid;
    snapshot->p25_sys_time = state->p25_sys_time;
    snapshot->p25_sys_time_offset_valid = state->p25_sys_time_offset_valid;
    snapshot->p25_sys_time_offset = state->p25_sys_time_offset;
    snapshot->p25_patch_count = state->p25_patch_count;
    DSD_MEMCPY(snapshot->p25_patch_sgid, state->p25_patch_sgid, sizeof(snapshot->p25_patch_sgid));
    DSD_MEMCPY(snapshot->p25_patch_is_patch, state->p25_patch_is_patch, sizeof(snapshot->p25_patch_is_patch));
    DSD_MEMCPY(snapshot->p25_patch_active, state->p25_patch_active, sizeof(snapshot->p25_patch_active));
    DSD_MEMCPY(snapshot->p25_patch_last_update, state->p25_patch_last_update, sizeof(snapshot->p25_patch_last_update));
    DSD_MEMCPY(snapshot->p25_patch_wgid_count, state->p25_patch_wgid_count, sizeof(snapshot->p25_patch_wgid_count));
    DSD_MEMCPY(snapshot->p25_patch_wgid, state->p25_patch_wgid, sizeof(snapshot->p25_patch_wgid));
    DSD_MEMCPY(snapshot->p25_patch_wuid_count, state->p25_patch_wuid_count, sizeof(snapshot->p25_patch_wuid_count));
    DSD_MEMCPY(snapshot->p25_patch_wuid, state->p25_patch_wuid, sizeof(snapshot->p25_patch_wuid));
    DSD_MEMCPY(snapshot->p25_patch_key, state->p25_patch_key, sizeof(snapshot->p25_patch_key));
    DSD_MEMCPY(snapshot->p25_patch_alg, state->p25_patch_alg, sizeof(snapshot->p25_patch_alg));
    DSD_MEMCPY(snapshot->p25_patch_ssn, state->p25_patch_ssn, sizeof(snapshot->p25_patch_ssn));
    DSD_MEMCPY(snapshot->p25_patch_key_valid, state->p25_patch_key_valid, sizeof(snapshot->p25_patch_key_valid));
    snapshot->p25_aff_count = state->p25_aff_count;
    DSD_MEMCPY(snapshot->p25_aff_rid, state->p25_aff_rid, sizeof(snapshot->p25_aff_rid));
    DSD_MEMCPY(snapshot->p25_aff_last_seen, state->p25_aff_last_seen, sizeof(snapshot->p25_aff_last_seen));
    snapshot->p25_ga_count = state->p25_ga_count;
    DSD_MEMCPY(snapshot->p25_ga_rid, state->p25_ga_rid, sizeof(snapshot->p25_ga_rid));
    DSD_MEMCPY(snapshot->p25_ga_tg, state->p25_ga_tg, sizeof(snapshot->p25_ga_tg));
    DSD_MEMCPY(snapshot->p25_ga_last_seen, state->p25_ga_last_seen, sizeof(snapshot->p25_ga_last_seen));
    snapshot->p25_nb_count = state->p25_nb_count;
    DSD_MEMCPY(snapshot->p25_nb_entries, state->p25_nb_entries, sizeof(snapshot->p25_nb_entries));
    snapshot->p25_src_nid = state->p25_src_nid;
    DSD_MEMCPY(snapshot->p25_call_emergency, state->p25_call_emergency, sizeof(snapshot->p25_call_emergency));
    DSD_MEMCPY(snapshot->p25_call_priority, state->p25_call_priority, sizeof(snapshot->p25_call_priority));
    DSD_MEMCPY(snapshot->p25_call_is_packet, state->p25_call_is_packet, sizeof(snapshot->p25_call_is_packet));
    snapshot->p25_cc_eval_freq = state->p25_cc_eval_freq;
    snapshot->p25_cc_eval_start_m = state->p25_cc_eval_start_m;
    snapshot->p25_cc_cache_loaded = state->p25_cc_cache_loaded;
    snapshot->dmr_mfid = state->dmr_mfid;
    snapshot->dmr_fid = state->dmr_fid;
    snapshot->dmr_so = state->dmr_so;
    snapshot->dmr_fidR = state->dmr_fidR;
    snapshot->dmr_soR = state->dmr_soR;
    trunk_scan_save_dmr_confidence_snapshot(state, snapshot);
    DSD_MEMCPY(snapshot->dmr_branding, state->dmr_branding, sizeof(snapshot->dmr_branding));
    DSD_MEMCPY(snapshot->dmr_branding_sub, state->dmr_branding_sub, sizeof(snapshot->dmr_branding_sub));
    DSD_MEMCPY(snapshot->dmr_site_parms, state->dmr_site_parms, sizeof(snapshot->dmr_site_parms));
    snapshot->dmr_rest_channel = state->dmr_rest_channel;
    snapshot->lcn_freq_count = state->lcn_freq_count;
    snapshot->lcn_freq_roll = state->lcn_freq_roll;
    snapshot->is_con_plus = state->is_con_plus;
    snapshot->last_cc_sync_time = state->last_cc_sync_time;
    snapshot->last_vc_sync_time = state->last_vc_sync_time;
    snapshot->p25_last_vc_tune_time = state->p25_last_vc_tune_time;
    snapshot->last_t3_tune_time = state->last_t3_tune_time;
    snapshot->last_cc_sync_time_m = state->last_cc_sync_time_m;
    snapshot->last_vc_sync_time_m = state->last_vc_sync_time_m;
    snapshot->p25_last_vc_tune_time_m = state->p25_last_vc_tune_time_m;
    snapshot->last_t3_tune_time_m = state->last_t3_tune_time_m;

    const dsd_trunk_cc_candidates* cc_candidates = dsd_trunk_cc_candidates_peek(state);
    snapshot->has_cc_candidates = cc_candidates ? 1 : 0;
    if (cc_candidates) {
        snapshot->cc_candidates = *cc_candidates;
    } else {
        DSD_MEMSET(&snapshot->cc_candidates, 0, sizeof(snapshot->cc_candidates));
    }
}

static void
trunk_scan_restore_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    state->p2_wacn = snapshot->p2_wacn;
    state->p2_sysid = snapshot->p2_sysid;
    state->p2_cc = snapshot->p2_cc;
    state->p2_siteid = snapshot->p2_siteid;
    state->p2_rfssid = snapshot->p2_rfssid;
    state->p25_cc_freq = snapshot->p25_cc_freq;
    state->trunk_cc_freq = snapshot->trunk_cc_freq;
    DSD_MEMCPY(state->p25_vc_freq, snapshot->p25_vc_freq, sizeof(state->p25_vc_freq));
    DSD_MEMCPY(state->trunk_vc_freq, snapshot->trunk_vc_freq, sizeof(state->trunk_vc_freq));
    DSD_MEMCPY(state->trunk_lcn_freq, snapshot->trunk_lcn_freq, sizeof(state->trunk_lcn_freq));
    DSD_MEMCPY(state->trunk_chan_map, snapshot->trunk_chan_map, sizeof(state->trunk_chan_map));
    DSD_MEMCPY(state->trunk_chan_map_used, snapshot->trunk_chan_map_used, sizeof(state->trunk_chan_map_used));
    state->trunk_chan_map_used_count = snapshot->trunk_chan_map_used_count;
    state->trunk_chan_map_seq = snapshot->trunk_chan_map_seq;
    DSD_MEMCPY(state->dmr_lcn_trust, snapshot->dmr_lcn_trust, sizeof(state->dmr_lcn_trust));
    DSD_MEMCPY(state->p25_chan_tdma_explicit, snapshot->p25_chan_tdma_explicit, sizeof(state->p25_chan_tdma_explicit));
    state->p25_chan_iden = snapshot->p25_chan_iden;
    DSD_MEMCPY(state->p25_iden_fdma, snapshot->p25_iden_fdma, sizeof(state->p25_iden_fdma));
    DSD_MEMCPY(state->p25_iden_tdma, snapshot->p25_iden_tdma, sizeof(state->p25_iden_tdma));
    state->p25_cc_is_tdma = snapshot->p25_cc_is_tdma;
    state->p25_sys_is_tdma = snapshot->p25_sys_is_tdma;
    state->p25_vc_cqpsk_pref = snapshot->p25_vc_cqpsk_pref;
    state->p25_vc_cqpsk_override = snapshot->p25_vc_cqpsk_override;
    state->samplesPerSymbol = snapshot->samplesPerSymbol;
    state->symbolCenter = snapshot->symbolCenter;
    state->rf_mod = snapshot->rf_mod;
    state->p25_prot_valid = snapshot->p25_prot_valid;
    state->p25_prot_algid = snapshot->p25_prot_algid;
    state->p25_prot_kid = snapshot->p25_prot_kid;
    state->p25_sys_time_valid = snapshot->p25_sys_time_valid;
    state->p25_sys_time = snapshot->p25_sys_time;
    state->p25_sys_time_offset_valid = snapshot->p25_sys_time_offset_valid;
    state->p25_sys_time_offset = snapshot->p25_sys_time_offset;
    state->p25_patch_count = snapshot->p25_patch_count;
    DSD_MEMCPY(state->p25_patch_sgid, snapshot->p25_patch_sgid, sizeof(state->p25_patch_sgid));
    DSD_MEMCPY(state->p25_patch_is_patch, snapshot->p25_patch_is_patch, sizeof(state->p25_patch_is_patch));
    DSD_MEMCPY(state->p25_patch_active, snapshot->p25_patch_active, sizeof(state->p25_patch_active));
    DSD_MEMCPY(state->p25_patch_last_update, snapshot->p25_patch_last_update, sizeof(state->p25_patch_last_update));
    DSD_MEMCPY(state->p25_patch_wgid_count, snapshot->p25_patch_wgid_count, sizeof(state->p25_patch_wgid_count));
    DSD_MEMCPY(state->p25_patch_wgid, snapshot->p25_patch_wgid, sizeof(state->p25_patch_wgid));
    DSD_MEMCPY(state->p25_patch_wuid_count, snapshot->p25_patch_wuid_count, sizeof(state->p25_patch_wuid_count));
    DSD_MEMCPY(state->p25_patch_wuid, snapshot->p25_patch_wuid, sizeof(state->p25_patch_wuid));
    DSD_MEMCPY(state->p25_patch_key, snapshot->p25_patch_key, sizeof(state->p25_patch_key));
    DSD_MEMCPY(state->p25_patch_alg, snapshot->p25_patch_alg, sizeof(state->p25_patch_alg));
    DSD_MEMCPY(state->p25_patch_ssn, snapshot->p25_patch_ssn, sizeof(state->p25_patch_ssn));
    DSD_MEMCPY(state->p25_patch_key_valid, snapshot->p25_patch_key_valid, sizeof(state->p25_patch_key_valid));
    state->p25_aff_count = snapshot->p25_aff_count;
    DSD_MEMCPY(state->p25_aff_rid, snapshot->p25_aff_rid, sizeof(state->p25_aff_rid));
    DSD_MEMCPY(state->p25_aff_last_seen, snapshot->p25_aff_last_seen, sizeof(state->p25_aff_last_seen));
    state->p25_ga_count = snapshot->p25_ga_count;
    DSD_MEMCPY(state->p25_ga_rid, snapshot->p25_ga_rid, sizeof(state->p25_ga_rid));
    DSD_MEMCPY(state->p25_ga_tg, snapshot->p25_ga_tg, sizeof(state->p25_ga_tg));
    DSD_MEMCPY(state->p25_ga_last_seen, snapshot->p25_ga_last_seen, sizeof(state->p25_ga_last_seen));
    state->p25_nb_count = snapshot->p25_nb_count;
    DSD_MEMCPY(state->p25_nb_entries, snapshot->p25_nb_entries, sizeof(state->p25_nb_entries));
    state->p25_src_nid = snapshot->p25_src_nid;
    DSD_MEMCPY(state->p25_call_emergency, snapshot->p25_call_emergency, sizeof(state->p25_call_emergency));
    DSD_MEMCPY(state->p25_call_priority, snapshot->p25_call_priority, sizeof(state->p25_call_priority));
    DSD_MEMCPY(state->p25_call_is_packet, snapshot->p25_call_is_packet, sizeof(state->p25_call_is_packet));
    state->p25_cc_eval_freq = snapshot->p25_cc_eval_freq;
    state->p25_cc_eval_start_m = snapshot->p25_cc_eval_start_m;
    state->p25_cc_cache_loaded = snapshot->p25_cc_cache_loaded;
    state->dmr_mfid = snapshot->dmr_mfid;
    state->dmr_fid = snapshot->dmr_fid;
    state->dmr_so = snapshot->dmr_so;
    state->dmr_fidR = snapshot->dmr_fidR;
    state->dmr_soR = snapshot->dmr_soR;
    trunk_scan_restore_dmr_confidence_snapshot(state, snapshot);
    DSD_MEMCPY(state->dmr_branding, snapshot->dmr_branding, sizeof(state->dmr_branding));
    DSD_MEMCPY(state->dmr_branding_sub, snapshot->dmr_branding_sub, sizeof(state->dmr_branding_sub));
    DSD_MEMCPY(state->dmr_site_parms, snapshot->dmr_site_parms, sizeof(state->dmr_site_parms));
    state->dmr_rest_channel = snapshot->dmr_rest_channel;
    state->lcn_freq_count = snapshot->lcn_freq_count;
    state->lcn_freq_roll = snapshot->lcn_freq_roll;
    state->is_con_plus = snapshot->is_con_plus;
    state->last_cc_sync_time = snapshot->last_cc_sync_time;
    state->last_vc_sync_time = snapshot->last_vc_sync_time;
    state->p25_last_vc_tune_time = snapshot->p25_last_vc_tune_time;
    state->last_t3_tune_time = snapshot->last_t3_tune_time;
    state->last_cc_sync_time_m = snapshot->last_cc_sync_time_m;
    state->last_vc_sync_time_m = snapshot->last_vc_sync_time_m;
    state->p25_last_vc_tune_time_m = snapshot->p25_last_vc_tune_time_m;
    state->last_t3_tune_time_m = snapshot->last_t3_tune_time_m;

    if (snapshot->has_cc_candidates) {
        dsd_trunk_cc_candidates* cc_candidates = dsd_trunk_cc_candidates_get(state);
        if (cc_candidates) {
            *cc_candidates = snapshot->cc_candidates;
        }
    } else {
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_CC_CANDIDATES, NULL, NULL);
    }
}

static void
trunk_scan_note_chan_map_seq(dsd_trunk_scan_coord* coord, uint64_t seq) {
    if (coord && seq > coord->last_trunk_chan_map_seq) {
        coord->last_trunk_chan_map_seq = seq;
    }
}

static uint64_t
trunk_scan_next_chan_map_seq(dsd_trunk_scan_coord* coord, uint64_t restored_seq) {
    if (!coord) {
        return restored_seq;
    }
    trunk_scan_note_chan_map_seq(coord, restored_seq);
    if (coord->last_trunk_chan_map_seq == UINT64_MAX) {
        coord->last_trunk_chan_map_seq = 0;
    }
    return ++coord->last_trunk_chan_map_seq;
}

static void
trunk_scan_restore_target_snapshot(dsd_trunk_scan_coord* coord, dsd_state* state, dsd_trunk_scan_target_runtime* rt) {
    if (!rt) {
        return;
    }
    trunk_scan_restore_snapshot(state, &rt->snapshot);
    state->trunk_chan_map_seq = trunk_scan_next_chan_map_seq(coord, state->trunk_chan_map_seq);
    rt->snapshot.trunk_chan_map_seq = state->trunk_chan_map_seq;
}

static void
trunk_scan_save_target_snapshot(dsd_trunk_scan_coord* coord, const dsd_state* state,
                                dsd_trunk_scan_target_runtime* rt) {
    if (!rt) {
        return;
    }
    trunk_scan_save_snapshot(state, &rt->snapshot);
    trunk_scan_note_chan_map_seq(coord, rt->snapshot.trunk_chan_map_seq);
}

static dsd_trunk_scan_coord*
trunk_scan_get(dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(dsd_trunk_scan_coord, state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
}

static const dsd_trunk_scan_coord*
trunk_scan_get_const(const dsd_state* state) {
    return (const dsd_trunk_scan_coord*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
}

static int
trunk_scan_target_is_dmr(const dsd_trunk_scan_target* target) {
    return target
           && (target->type == DSD_TRUNK_SCAN_TARGET_DMR_TRUNK
               || target->type == DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL);
}

static int
trunk_scan_target_is_p25(const dsd_trunk_scan_target* target) {
    return target && target->type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK;
}

static int
trunk_scan_is_iq_replay(const dsd_opts* opts) {
    return opts && (opts->iq_replay_requested != 0 || opts->iq_replay_active != 0);
}

static int
trunk_scan_has_tuning_backend(const dsd_opts* opts) {
    return opts && !trunk_scan_is_iq_replay(opts) && (opts->audio_in_type == AUDIO_IN_RTL || opts->use_rigctl == 1);
}

static int
trunk_scan_demod_rate(const dsd_opts* opts) {
    if (opts && opts->audio_in_type == AUDIO_IN_RTL) {
        int rtl_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
        if (rtl_rate > 0) {
            return rtl_rate;
        }
    }
    return dsd_opts_current_input_timing_rate(opts);
}

static int
trunk_scan_p25_cc_sps(const dsd_opts* opts, const dsd_state* state) {
    int sym_rate = (state && state->p25_cc_is_tdma == 1) ? 6000 : 4800;
    int demod_rate = trunk_scan_demod_rate(opts);
    return dsd_opts_compute_sps_rate(opts, sym_rate, demod_rate);
}

static int
trunk_scan_dmr_sps(const dsd_opts* opts, const dsd_state* state) {
    (void)state;
    int demod_rate = trunk_scan_demod_rate(opts);
    return dsd_opts_compute_sps_rate(opts, 4800, demod_rate);
}

static void
trunk_scan_apply_target_demod(const dsd_opts* opts, dsd_state* state, const dsd_trunk_scan_target* target) {
    if (!opts || !state || !target) {
        return;
    }
    if (trunk_scan_target_is_p25(target)) {
        int p25_sps = trunk_scan_p25_cc_sps(opts, state);
        state->samplesPerSymbol = p25_sps;
        state->symbolCenter = dsd_opts_symbol_center(p25_sps);
        return;
    }
    if (!trunk_scan_target_is_dmr(target)) {
        return;
    }
    int dmr_sps = trunk_scan_dmr_sps(opts, state);
    state->samplesPerSymbol = dmr_sps;
    state->symbolCenter = dsd_opts_symbol_center(dmr_sps);
    if (!opts->mod_cli_lock) {
        state->rf_mod = 2;
    }
}

static void
trunk_scan_apply_target_opts(dsd_opts* opts, const dsd_trunk_scan_target* target) {
    if (!opts || !target) {
        return;
    }
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    switch (target->type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK:
            opts->p25_trunk = 1;
            opts->trunk_enable = 1;
            break;
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK:
            opts->p25_trunk = 0;
            opts->trunk_enable = 1;
            break;
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL:
            opts->p25_trunk = 0;
            opts->trunk_enable = 0;
            break;
    }
}

static void
trunk_scan_seed_target_state(dsd_state* state, const dsd_trunk_scan_target* target, double now_m) {
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = now_m;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    state->p25_last_vc_tune_time = 0;
    state->p25_last_vc_tune_time_m = 0.0;
    state->dmr_rest_channel = -1;

    if (target->type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        state->p25_cc_freq = (long int)target->frequency_hz;
        state->trunk_cc_freq = (long int)target->frequency_hz;
        state->trunk_lcn_freq[0] = (long int)target->frequency_hz;
        state->lcn_freq_count = state->lcn_freq_count < 1 ? 1 : state->lcn_freq_count;
    } else if (target->type == DSD_TRUNK_SCAN_TARGET_DMR_TRUNK) {
        state->p25_cc_freq = 0;
        state->trunk_cc_freq = (long int)target->frequency_hz;
        state->trunk_lcn_freq[0] = (long int)target->frequency_hz;
        state->lcn_freq_count = state->lcn_freq_count < 1 ? 1 : state->lcn_freq_count;
    } else {
        state->p25_cc_freq = 0;
        state->trunk_cc_freq = 0;
    }
}

static void
trunk_scan_seed_empty_snapshot(dsd_trunk_scan_snapshot* snapshot, const dsd_opts* opts, const dsd_state* state) {
    if (!snapshot) {
        return;
    }
    trunk_scan_snapshot_clear(snapshot);
    if (!opts || !state || !opts->mod_cli_lock) {
        return;
    }
    snapshot->samplesPerSymbol = state->samplesPerSymbol;
    snapshot->symbolCenter = state->symbolCenter;
    snapshot->rf_mod = state->rf_mod;
}

static int
trunk_scan_import_target_chan_csv(const dsd_opts* opts, dsd_state* state, const dsd_trunk_scan_target* target,
                                  char* err, size_t err_sz) {
    if (!target->chan_csv[0]) {
        return 0;
    }
    dsd_opts* tmp_opts = (dsd_opts*)calloc(1, sizeof(*tmp_opts));
    if (!tmp_opts) {
        scan_set_error(err, err_sz, "failed to allocate channel import options for trunk scan target '%s'", target->id);
        return -1;
    }
    *tmp_opts = *opts;
    DSD_SNPRINTF(tmp_opts->chan_in_file, sizeof tmp_opts->chan_in_file, "%s", target->chan_csv);
    tmp_opts->chan_in_file[sizeof tmp_opts->chan_in_file - 1] = '\0';
    int import_rc = csvChanImport(tmp_opts, state);
    free(tmp_opts);
    if (import_rc != 0) {
        scan_set_error(err, err_sz, "failed to import chan_csv '%s' for trunk scan target '%s'", target->chan_csv,
                       target->id);
        return -1;
    }
    return 0;
}

static int
trunk_scan_build_target_runtime(dsd_trunk_scan_coord* coord, dsd_opts* opts, dsd_state* state,
                                const dsd_trunk_scan_target_list* list, char* err, size_t err_sz) {
    dsd_trunk_scan_snapshot empty_snapshot;
    trunk_scan_seed_empty_snapshot(&empty_snapshot, opts, state);
    double now_m = trunk_scan_now_m();

    for (size_t i = 0; i < list->count; i++) {
        dsd_trunk_scan_target_runtime* rt = &coord->targets[i];
        rt->target = list->targets[i];
        trunk_scan_restore_snapshot(state, &empty_snapshot);
        trunk_scan_apply_target_opts(opts, &rt->target);
        trunk_scan_apply_target_demod(opts, state, &rt->target);
        trunk_scan_seed_target_state(state, &rt->target, now_m);
        if (trunk_scan_import_target_chan_csv(opts, state, &rt->target, err, err_sz) != 0) {
            return -1;
        }
        p25_sm_init_ctx(&rt->p25_ctx, opts, state);
        dmr_sm_init_ctx(&rt->dmr_ctx, opts, state);
        trunk_scan_save_snapshot(state, &rt->snapshot);
        trunk_scan_note_chan_map_seq(coord, rt->snapshot.trunk_chan_map_seq);
        rt->parked_since_m = now_m;
        rt->idle_since_m = now_m;
    }
    return 0;
}

static long int
trunk_scan_p25_retune_freq(const dsd_state* state, const dsd_trunk_scan_target* target) {
    if (state) {
        if (state->p25_cc_freq > 0) {
            return state->p25_cc_freq;
        }
        if (state->trunk_cc_freq > 0) {
            return state->trunk_cc_freq;
        }
    }
    return (long int)target->frequency_hz;
}

static long int
trunk_scan_dmr_retune_freq(const dsd_state* state, const dsd_trunk_scan_target* target) {
    if (state && state->trunk_cc_freq > 0) {
        return state->trunk_cc_freq;
    }
    return (long int)target->frequency_hz;
}

static long int
trunk_scan_retune_freq(const dsd_state* state, const dsd_trunk_scan_target* target) {
    if (!target) {
        return 0;
    }
    if (target->type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        return trunk_scan_p25_retune_freq(state, target);
    }
    if (target->type == DSD_TRUNK_SCAN_TARGET_DMR_TRUNK) {
        return trunk_scan_dmr_retune_freq(state, target);
    }
    return (long int)target->frequency_hz;
}

static dsd_trunk_tune_result
trunk_scan_retune_active(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_target_runtime* rt) {
    const long int freq = trunk_scan_retune_freq(state, &rt->target);
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        state->p25_cc_freq = freq;
        state->trunk_cc_freq = freq;
        return dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, trunk_scan_p25_cc_sps(opts, state));
    }
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_DMR_TRUNK) {
        state->p25_cc_freq = 0;
        state->trunk_cc_freq = freq;
        return dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, trunk_scan_dmr_sps(opts, state));
    }
    return dsd_engine_scan_tune_to_freq(opts, state, freq, trunk_scan_dmr_sps(opts, state));
}

static int
trunk_scan_switch_to(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord, size_t next, int save_current) {
    if (!coord || next >= coord->count) {
        return -1;
    }
    if (save_current && coord->active < coord->count) {
        trunk_scan_save_target_snapshot(coord, state, &coord->targets[coord->active]);
    }

    coord->active = next;
    dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    trunk_scan_restore_target_snapshot(coord, state, rt);
    trunk_scan_apply_target_opts(opts, &rt->target);
    trunk_scan_apply_target_demod(opts, state, &rt->target);

    double now_m = trunk_scan_now_m();
    rt->parked_since_m = now_m;
    rt->idle_since_m = now_m;
    dsd_trunk_tune_result tune_result = trunk_scan_retune_active(opts, state, rt);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        rt->retry_until_m = now_m + 2.0;
        LOG_WARNING("Trunk scan target '%s' retune failed; cooling down briefly\n", rt->target.id);
        return -1;
    }

    rt->retry_until_m = 0.0;
    LOG_NOTICE("Trunk scan target '%s' at %ld Hz\n", rt->target.id, trunk_scan_retune_freq(state, &rt->target));
    return 0;
}

static void
trunk_scan_advance(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord) {
    if (!coord || coord->count < 2) {
        return;
    }
    double now_m = trunk_scan_now_m();
    dsd_trunk_scan_snapshot original_snapshot;
    trunk_scan_save_snapshot(state, &original_snapshot);
    size_t original_active = coord->active;
    int save_current = 1;
    int attempted_alternate_retune = 0;

    for (size_t attempts = 0; attempts < coord->count; attempts++) {
        size_t next = (original_active + 1U + attempts) % coord->count;
        if (next == original_active && !attempted_alternate_retune) {
            continue;
        }
        if (coord->targets[next].retry_until_m > now_m) {
            continue;
        }
        if (trunk_scan_switch_to(opts, state, coord, next, save_current) == 0) {
            return;
        }
        if (next != original_active) {
            attempted_alternate_retune = 1;
        }
        save_current = 0;
    }

    coord->active = original_active;
    trunk_scan_restore_snapshot(state, &original_snapshot);
    trunk_scan_apply_target_opts(opts, &coord->targets[coord->active].target);
    trunk_scan_apply_target_demod(opts, state, &coord->targets[coord->active].target);
}

static void
trunk_scan_retry_active_if_due(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord, double now_m) {
    if (!coord || coord->count != 1 || coord->active >= coord->count) {
        return;
    }
    const dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    if (rt->retry_until_m <= 0.0 || rt->retry_until_m > now_m) {
        return;
    }
    (void)trunk_scan_switch_to(opts, state, coord, coord->active, 0);
}

static int
trunk_scan_active_is_held(const dsd_opts* opts, const dsd_trunk_scan_coord* coord) {
    const dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        return (opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1 || p25_sm_get_state(&rt->p25_ctx) == P25_SM_TUNED);
    }
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_DMR_TRUNK) {
        return (opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1 || dmr_sm_get_state(&rt->dmr_ctx) == DMR_SM_TUNED);
    }
    double now_m = trunk_scan_now_m();
    double hold_s = (double)rt->target.activity_hold_ms / 1000.0;
    return rt->last_allowed_activity_m > 0.0 && (now_m - rt->last_allowed_activity_m) < hold_s;
}

static void
trunk_scan_restore_saved_opts(dsd_opts* opts, const dsd_trunk_scan_coord* coord) {
    if (!opts || !coord) {
        return;
    }
    opts->p25_trunk = coord->saved_p25_trunk;
    opts->trunk_enable = coord->saved_trunk_enable;
    opts->p25_is_tuned = coord->saved_p25_is_tuned;
    opts->trunk_is_tuned = coord->saved_trunk_is_tuned;
}

static void
trunk_scan_tick_active_target_sm(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_target_runtime* rt) {
    if (!opts || !state || !rt) {
        return;
    }
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_DMR_TRUNK) {
        dmr_sm_tick_ctx(&rt->dmr_ctx, opts, state);
    }
}

static void
trunk_scan_tick_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord) {
    double now_m = trunk_scan_now_m();
    trunk_scan_retry_active_if_due(opts, state, coord, now_m);
    dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    trunk_scan_tick_active_target_sm(opts, state, rt);
    if (trunk_scan_active_is_held(opts, coord)) {
        rt->idle_since_m = -1.0;
        return;
    }
    if (rt->idle_since_m < 0.0) {
        rt->idle_since_m = now_m;
        return;
    }
    double dwell_s = (double)rt->target.dwell_ms / 1000.0;
    if ((now_m - rt->idle_since_m) >= dwell_s) {
        trunk_scan_advance(opts, state, coord);
    }
}

void
dsd_engine_trunk_scan_tick(dsd_opts* opts, dsd_state* state) {
    dsd_trunk_scan_coord* coord = trunk_scan_get(state);
    if (!opts || !state || !coord || coord->count == 0) {
        return;
    }
    if (!p25_sm_tick_guard_try_enter()) {
        return;
    }
    trunk_scan_tick_locked(opts, state, coord);
    p25_sm_tick_guard_leave();
}

void*
dsd_engine_trunk_scan_active_p25_ctx(void) {
    if (!g_trunk_scan_coord || g_trunk_scan_coord->count == 0) {
        return NULL;
    }
    dsd_trunk_scan_target_runtime* rt = &g_trunk_scan_coord->targets[g_trunk_scan_coord->active];
    if (rt->target.type != DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        return NULL;
    }
    return &rt->p25_ctx;
}

void*
dsd_engine_trunk_scan_active_dmr_ctx(void) {
    if (!g_trunk_scan_coord || g_trunk_scan_coord->count == 0) {
        return NULL;
    }
    dsd_trunk_scan_target_runtime* rt = &g_trunk_scan_coord->targets[g_trunk_scan_coord->active];
    if (rt->target.type != DSD_TRUNK_SCAN_TARGET_DMR_TRUNK) {
        return NULL;
    }
    return &rt->dmr_ctx;
}

void
dsd_engine_trunk_scan_dmr_conventional_activity(const dsd_opts* opts, dsd_state* state, uint32_t target,
                                                uint32_t source, int is_private, int encrypted, int data_call) {
    dsd_trunk_scan_coord* coord = trunk_scan_get(state);
    if (!opts || !state || !coord || coord->count == 0) {
        return;
    }
    dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    if (rt->target.type != DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL) {
        return;
    }

    dsd_tg_policy_decision decision;
    int rc = 0;
    if (is_private) {
        rc = dsd_tg_policy_evaluate_private_call(opts, state, source, target, encrypted, data_call,
                                                 DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                 DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, target, source, encrypted, data_call,
                                               DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision);
    }
    if (rc == 0 && decision.tune_allowed) {
        rt->last_allowed_activity_m = trunk_scan_now_m();
        rt->idle_since_m = -1.0;
    }
}

static void
trunk_scan_uninstall_runtime_hooks(const dsd_trunk_scan_coord* coord) {
    if (g_trunk_scan_coord != coord) {
        return;
    }
    g_trunk_scan_coord = NULL;
    dsd_trunk_scan_hooks hooks = {0};
    dsd_trunk_scan_hooks_set(hooks);
}

static void
trunk_scan_free(void* ptr) {
    trunk_scan_uninstall_runtime_hooks((const dsd_trunk_scan_coord*)ptr);
    free(ptr);
}

static void
trunk_scan_install_runtime_hooks(dsd_trunk_scan_coord* coord) {
    g_trunk_scan_coord = coord;
    dsd_trunk_scan_hooks hooks = {0};
    hooks.p25_ctx = dsd_engine_trunk_scan_active_p25_ctx;
    hooks.dmr_ctx = dsd_engine_trunk_scan_active_dmr_ctx;
    hooks.tick = dsd_engine_trunk_scan_tick;
    hooks.dmr_conventional_activity = dsd_engine_trunk_scan_dmr_conventional_activity;
    dsd_trunk_scan_hooks_set(hooks);
}

int
dsd_engine_trunk_scan_init(dsd_opts* opts, dsd_state* state, char* err, size_t err_sz) {
    if (!opts || !state || !opts->trunk_scan_enabled) {
        return 0;
    }
    if (opts->scanner_mode == 1) {
        scan_set_error(err, err_sz, "--trunk-scan cannot be combined with -Y scanner mode");
        return -1;
    }
    if (opts->chan_in_file[0] != '\0') {
        scan_set_error(err, err_sz, "--trunk-scan cannot use a global channel map; use per-target chan_csv values");
        return -1;
    }
    if (opts->trunk_scan_targets_csv[0] == '\0') {
        scan_set_error(err, err_sz, "--trunk-scan requires targets_csv");
        return -1;
    }
    if (trunk_scan_is_iq_replay(opts)) {
        scan_set_error(err, err_sz, "--trunk-scan cannot use IQ replay input because replay cannot retune");
        return -1;
    }
    if (!trunk_scan_has_tuning_backend(opts)) {
        scan_set_error(err, err_sz, "--trunk-scan requires RTL input or rigctl tuning");
        return -1;
    }

    dsd_trunk_scan_target_list list;
    if (dsd_trunk_scan_load_targets_csv(opts->trunk_scan_targets_csv, opts, &list, err, err_sz) != 0) {
        return -1;
    }

    dsd_trunk_scan_coord* coord = (dsd_trunk_scan_coord*)calloc(1, sizeof(*coord));
    if (!coord) {
        scan_set_error(err, err_sz, "failed to allocate trunk scan coordinator");
        return -1;
    }
    coord->count = list.count;
    coord->saved_p25_trunk = opts->p25_trunk;
    coord->saved_trunk_enable = opts->trunk_enable;
    coord->saved_p25_is_tuned = opts->p25_is_tuned;
    coord->saved_trunk_is_tuned = opts->trunk_is_tuned;

    if (trunk_scan_build_target_runtime(coord, opts, state, &list, err, err_sz) != 0) {
        trunk_scan_restore_saved_opts(opts, coord);
        free(coord);
        return -1;
    }

    if (dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN, coord, trunk_scan_free) != 0) {
        trunk_scan_restore_saved_opts(opts, coord);
        free(coord);
        scan_set_error(err, err_sz, "failed to attach trunk scan coordinator");
        return -1;
    }
    trunk_scan_install_runtime_hooks(coord);
    if (trunk_scan_switch_to(opts, state, coord, 0, 0) != 0 && coord->count > 1) {
        trunk_scan_advance(opts, state, coord);
    }
    LOG_NOTICE("Trunk scan enabled with %zu targets\n", coord->count);
    return 0;
}

void
dsd_engine_trunk_scan_shutdown(dsd_opts* opts, dsd_state* state) {
    const dsd_trunk_scan_coord* coord = trunk_scan_get(state);
    if (!coord) {
        return;
    }
    trunk_scan_restore_saved_opts(opts, coord);
    trunk_scan_uninstall_runtime_hooks(coord);
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN, NULL, NULL);
}

size_t
dsd_engine_trunk_scan_active_index(const dsd_state* state) {
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    return coord ? coord->active : (size_t)-1;
}

size_t
dsd_engine_trunk_scan_target_count(const dsd_state* state) {
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    return coord ? coord->count : 0;
}
