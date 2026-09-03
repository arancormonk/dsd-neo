// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/trunk_scan.h>
#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#endif
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
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
#include "dsd-neo/core/enc_lockout.h"
#include "dsd-neo/core/key_set.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "dsd-neo/protocol/p25/p25_cc_candidates.h"
#if defined(DSD_TRUNK_SCAN_TEST_CLOCK)
#include "trunk_scan_internal.h"
#include "trunk_scan_test_support.h"
#endif

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
    time_t p25_last_cc_msg_time;
    time_t last_vc_sync_time;
    time_t p25_last_vc_tune_time;
    time_t last_t3_tune_time;
    double last_cc_sync_time_m;
    double p25_last_cc_msg_time_m;
    double last_vc_sync_time_m;
    double p25_last_vc_tune_time_m;
    double last_t3_tune_time_m;
    long int p25_vc_freq[2];
    long int trunk_vc_freq[2];
    time_t p25_patch_last_update[8];
    long int trunk_lcn_freq[DSD_TRUNK_LCN_EMBEDDED];
    long int* trunk_lcn_freq_ext;
    size_t trunk_lcn_freq_ext_count;
    size_t trunk_lcn_freq_ext_capacity;
    dsd_trunk_cc_candidates cc_candidates;
    p25_nb_entry_t p25_nb_entries[P25_NB_MAX];
    p25_secondary_cc_entry_t p25_secondary_cc_entries[P25_SECONDARY_CC_MAX];
    p25_pending_announcement_t p25_pending_announcements[P25_PENDING_ANNOUNCEMENT_MAX];
    p25_iden_entry_t p25_iden_fdma[16];
    p25_iden_entry_t p25_iden_tdma[16];
    /* The target's own user band plan (its p25_bandplan_csv), re-seeded by
     * p25_update_system_identity() once the target's WACN/SYS resolves. */
    p25_bandplan_row_t p25_bandplan_rows[DSD_P25_BANDPLAN_MAX_ROWS];
    int p25_bandplan_row_count;
    dsd_enc_lockout_entry enc_lockout_entries[DSD_ENC_LOCKOUT_MAX];
    time_t p25_aff_last_seen[256];
    time_t p25_ga_last_seen[512];
    /* Sparse channel map: only the channels state->trunk_chan_map_used[] lists, held in the same
     * ascending order, with chan_map_freq[i] the frequency for chan_map_chan[i]. Dense copies of
     * trunk_chan_map[] and trunk_chan_map_used[] were 655 KB of the 738 KB every parked target
     * cost, and each rotation memcpy'd all of it; real sites map a few hundred channels. Grown on
     * demand by trunk_scan_snapshot_chan_map_reserve() and released by trunk_scan_snapshot_clear()
     * and trunk_scan_coord_free(). trunk_chan_map_used_count is the number of valid entries. */
    uint16_t* chan_map_chan;
    long int* chan_map_freq;
    size_t chan_map_capacity;
    uint32_t trunk_chan_map_used_count;
    uint32_t p25_sys_services_available;
    uint32_t p25_sys_services_supported;
    unsigned int dmr_color_code;
    int p25_chan_iden;
    int p25_cc_is_tdma;
    int p25_sys_is_tdma;
    int p25_vc_cqpsk_pref;
    int p25_vc_cqpsk_override;
    int p25_p1_validated_rf_mod;
    int p25_sm_mode;
    int nac;
    int samplesPerSymbol;
    int symbolCenter;
    int rf_mod;
    int p25_patch_count;
    int p25_aff_count;
    int p25_ga_count;
    int p25_nb_count;
    int p25_secondary_cc_count;
    int p25_pending_announcement_count;
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
    unsigned int dmr_t3_syscode;
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
    uint8_t p25_prot_valid;
    uint8_t p25_prot_algid;
    uint8_t p25_cc_prot_valid;
    uint8_t p25_cc_prot_algid;
    uint8_t p25_sys_time_valid;
    uint8_t p25_sys_time_offset_valid;
    uint8_t p25_sys_services_valid;
    uint8_t p25_sys_services_request_priority;
    uint8_t p25_site_lra_valid;
    uint8_t p25_site_lra;
    uint8_t p25_site_network_active_valid;
    uint8_t p25_site_network_active;
    uint8_t p25_cc_cache_loaded;
    uint8_t p25_patch_is_patch[8];
    uint8_t p25_patch_active[8];
    uint8_t p25_patch_wgid_count[8];
    uint8_t p25_patch_wuid_count[8];
    uint8_t p25_patch_alg[8];
    uint8_t p25_patch_ssn[8];
    uint8_t p25_patch_key_valid[8];
    uint8_t dmr_confidence_locked;
    dsd_call_context_snapshot call_context;
    Event_History event_current[DSD_CALL_STATE_SLOT_COUNT];
    uint8_t dmr_confidence_color_code;
    uint8_t dmr_confidence_candidate_cc;
    uint8_t dmr_confidence_candidate_count;
    uint8_t dmr_confidence_voice_sync_seen[2];
    uint8_t dmr_confidence_voice_open[2];
    uint8_t dmr_confidence_voice_count[2];
    uint8_t dmr_confidence_mismatch_count;
    uint8_t p25_chan_tdma_explicit[16];
    uint8_t dmr_lcn_trust[0x1000];
    uint16_t nxdn_grant_chan;
    long int nxdn_grant_freq;
    unsigned int nxdn_last_ran;
    uint32_t nxdn_location_sys_code;
    uint16_t nxdn_location_site_code;
    char nxdn_location_category[14];
    uint8_t nxdn_rcn;
    uint8_t nxdn_base_freq;
    uint8_t nxdn_step;
    uint8_t nxdn_bw;
    nxdn_trunk_diag_ledger nxdn_diag;
} dsd_trunk_scan_snapshot;

typedef struct {
    dsd_trunk_scan_target target;
    dsd_trunk_scan_snapshot snapshot;
    /* Static per-target key configuration, loaded at init. Not part of the
     * snapshot: the switch applies it through the scan key swap instead. */
    dsd_key_set keys;
    p25_sm_ctx_t p25_ctx;
    dmr_sm_ctx_t dmr_ctx;
    double parked_since_m;
    double idle_since_m;
    double retry_until_m;
    double last_allowed_activity_m;
    uint64_t tune_request_id;
    int tune_pending;
    int avoided; /* operator avoid for the session: skipped by the rotation (#380) */
    /* Identity the peer IDEN share last ran against (#402): the tick re-runs it only when the
     * live WACN/SYS differs, so a target that resolves its identity while parked gets its
     * peers' plan without waiting for the next rotation. */
    unsigned long long iden_share_wacn;
    unsigned long long iden_share_sysid;
} dsd_trunk_scan_target_runtime;

/*
 * Enforced while parsing, not only where the array is allocated: an unbounded row count would ask
 * for one calloc large enough that Linux overcommit grants it and the process is OOM-killed while
 * faulting the pages in, instead of failing with a message. It also bounds the parser's per-row
 * duplicate scans, which are linear in the rows accepted so far.
 */
size_t
dsd_trunk_scan_max_targets(void) {
    const size_t max = (size_t)DSD_TRUNK_SCAN_TARGET_MEMORY_BUDGET_BYTES / sizeof(dsd_trunk_scan_target_runtime);
    return max > 0 ? max : 1U;
}

typedef struct {
    dsd_trunk_scan_target_runtime* targets;
    dsd_trunk_scan_snapshot scratch_snapshot;
    size_t count;
    size_t active;
    int hold_active; /* operator hold: the idle dwell never expires while set (#380) */
    int saved_trunk_enable;
    int saved_trunk_is_tuned;
    int saved_mod_c4fm;
    int saved_mod_qpsk;
    int saved_mod_gfsk;
    int saved_mod_p25p2_c4fm;
    int saved_mod_p25p2_profile_lock;
    int saved_mod_cli_lock;
    int saved_rtl_gain_value;
    int saved_tuner_autogain_on;
    int saved_tuner_autogain_is_set;
    uint64_t last_trunk_chan_map_seq;
} dsd_trunk_scan_coord;

static dsd_trunk_scan_coord* g_trunk_scan_coord;
#if defined(DSD_TRUNK_SCAN_TEST_CLOCK)
static int g_trunk_scan_now_override;
static double g_trunk_scan_now_m;
#endif
static const char k_trunk_scan_csv_header[] = "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes";

enum {
    DSD_TRUNK_SCAN_REQUIRED_CSV_FIELDS = 7,
    DSD_TRUNK_SCAN_MAX_CSV_FIELDS = 32,
};

static double
trunk_scan_now_m(void) {
#if defined(DSD_TRUNK_SCAN_TEST_CLOCK)
    return g_trunk_scan_now_override ? g_trunk_scan_now_m : dsd_time_now_monotonic_s();
#else
    return dsd_time_now_monotonic_s();
#endif
}

#if defined(DSD_TRUNK_SCAN_TEST_CLOCK)
void
trunk_scan_test_set_now(double now_m) {
    g_trunk_scan_now_override = 1;
    g_trunk_scan_now_m = now_m;
}

void
trunk_scan_test_clear_now(void) {
    g_trunk_scan_now_override = 0;
    g_trunk_scan_now_m = 0.0;
}
#endif

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

/* Single home for the type axes the coordinator dispatches on, so a new target type is
 * classified in one place instead of in every `type == A || type == B` chain. Each axis is an
 * exhaustive switch with no default: adding a target type must fail the build here rather than
 * silently inherit whichever answer the chain happened to fall through to. */
static int
trunk_scan_type_is_conventional(dsd_trunk_scan_target_type type) {
    switch (type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK: return 0;
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL: return 1;
    }
    return 0;
}

/* Target types whose control channel is anchored in state->p25_cc_freq. NXDN trunking reads and
 * writes that field the same way P25 does; DMR trunking keeps it at 0 and uses trunk_cc_freq
 * alone, which is what stops a stray NXDN element from moving a DMR target's control channel. */
static int
trunk_scan_type_anchors_p25_cc_freq(dsd_trunk_scan_target_type type) {
    switch (type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK: return 1;
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL: return 0;
    }
    return 0;
}

/* Symbol rate of a target's four-level GFSK demod profile, or 0 for the P25 class. DMR and NXDN96
 * share 4800 sym/s; NXDN48 runs the same four-level GFSK demodulator at 2400 sym/s, which is the
 * only axis that separates it from an nxdn-conventional target. */
static int
trunk_scan_type_gfsk_symbol_rate(dsd_trunk_scan_target_type type) {
    switch (type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK: return 0;
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL: return 4800;
        case DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL: return 2400;
    }
    return 0;
}

static int
trunk_scan_type_is_gfsk_family(dsd_trunk_scan_target_type type) {
    return trunk_scan_type_gfsk_symbol_rate(type) > 0;
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
    if (strcmp(s, "nxdn-trunk") == 0) {
        *out = DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK;
        return 0;
    }
    if (strcmp(s, "nxdn-conventional") == 0) {
        *out = DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL;
        return 0;
    }
    if (strcmp(s, "nxdn48-conventional") == 0) {
        *out = DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL;
        return 0;
    }
    return -1;
}

static int
scan_split_csv_fields(char* line, char** fields, size_t max_fields, size_t* out_count) {
    if (!line || !fields || max_fields == 0 || !out_count) {
        return -1;
    }
    char* p = line;
    size_t count = 0;
    for (;;) {
        if (count >= max_fields) {
            return -1;
        }
        fields[count++] = p;
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
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    *out_count = count;
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
    int modulation_idx;
    int rtl_gain_idx;
    int keys_hex_idx;
    int keys_dec_idx;
    int p25_bandplan_idx;
    unsigned int row;
    char* err;
    size_t err_sz;
} dsd_trunk_scan_row_parse;

static const char*
scan_optional_field(char** fields, size_t field_count, int idx) {
    if (idx < 0 || (size_t)idx >= field_count) {
        return "";
    }
    return scan_unquote(fields[idx]);
}

static int
scan_parse_modulation(const char* s, dsd_trunk_scan_target_type type, dsd_trunk_scan_modulation* out) {
    if (!s || !out) {
        return -1;
    }
    *out = DSD_TRUNK_SCAN_MODULATION_UNSET;
    if (s[0] == '\0') {
        return 0;
    }
    if (strcmp(s, "auto") == 0) {
        *out = DSD_TRUNK_SCAN_MODULATION_AUTO;
        return 0;
    }
    if (strcmp(s, "c4fm") == 0 && type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        *out = DSD_TRUNK_SCAN_MODULATION_C4FM;
        return 0;
    }
    if (strcmp(s, "cqpsk") == 0 && type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        *out = DSD_TRUNK_SCAN_MODULATION_CQPSK;
        return 0;
    }
    if (strcmp(s, "gfsk") == 0 && trunk_scan_type_is_gfsk_family(type)) {
        *out = DSD_TRUNK_SCAN_MODULATION_GFSK;
        return 0;
    }
    return -1;
}

static int
scan_parse_rtl_gain_field(const char* s, dsd_trunk_scan_target* target) {
    if (!target) {
        return -1;
    }
    target->rtl_gain_is_set = 0;
    target->rtl_gain_db = 0;
    if (!s || s[0] == '\0') {
        return 0;
    }
    if (strcmp(s, "auto") == 0) {
        target->rtl_gain_is_set = 1;
        return 0;
    }
    uint32_t parsed = 0;
    if (scan_parse_u32_decimal(s, 0U, 49U, &parsed) != 0) {
        return -1;
    }
    target->rtl_gain_is_set = 1;
    target->rtl_gain_db = (int)parsed;
    return 0;
}

static int
scan_parse_target_base_fields(char** fields, const dsd_trunk_scan_target_list* parsed,
                              const dsd_trunk_scan_row_parse* parse, dsd_trunk_scan_target* target,
                              const char** out_chan_csv, const char** out_dwell_s, const char** out_hold_s) {
    const char* id = scan_unquote(fields[0]);
    if (id[0] == '\0' || strlen(id) >= sizeof(parsed->targets[0].id)) {
        scan_set_error(parse->err, parse->err_sz, "row %u has an empty or too-long id", parse->row);
        return -1;
    }
    if (scan_has_duplicate_id(parsed, id)) {
        scan_set_error(parse->err, parse->err_sz, "row %u duplicates trunk scan target id '%s'", parse->row, id);
        return -1;
    }

    DSD_SNPRINTF(target->id, sizeof target->id, "%s", id);
    const char* type_s = scan_unquote(fields[1]);
    if (scan_parse_type(type_s, &target->type) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid target type '%s'", parse->row, type_s);
        return -1;
    }

    const char* freq_s = scan_unquote(fields[2]);
    if (scan_parse_u32_decimal(freq_s, 1U, DSD_TRUNK_SCAN_MAX_FREQUENCY_HZ, &target->frequency_hz) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid frequency_hz '%s'", parse->row, freq_s);
        return -1;
    }

    *out_chan_csv = scan_unquote(fields[3]);
    *out_dwell_s = scan_unquote(fields[4]);
    *out_hold_s = scan_unquote(fields[5]);
    return 0;
}

static int
scan_parse_target_overrides(dsd_trunk_scan_target* target, const dsd_trunk_scan_row_parse* parse,
                            const char* modulation_s, const char* rtl_gain_s) {
    if (scan_parse_modulation(modulation_s, target->type, &target->modulation) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid modulation '%s'", parse->row, modulation_s);
        return -1;
    }
    if (scan_parse_rtl_gain_field(rtl_gain_s, target) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid rtl_gain '%s'", parse->row, rtl_gain_s);
        return -1;
    }
    return 0;
}

static int
scan_target_list_reserve(dsd_trunk_scan_target_list* list, size_t needed) {
    if (needed <= list->capacity) {
        return 0;
    }
    size_t capacity = list->capacity > 0 ? list->capacity : 32;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(dsd_trunk_scan_target)) {
        return -1;
    }
    dsd_trunk_scan_target* targets = (dsd_trunk_scan_target*)realloc(list->targets, capacity * sizeof *targets);
    if (!targets) {
        return -1;
    }
    list->targets = targets;
    list->capacity = capacity;
    return 0;
}

/*
 * Resolve the row's file references -- the trunking channel map and the per-row key CSVs -- against
 * the directory holding the target CSV itself, so a target list stays relocatable as a unit.
 */
static int
scan_parse_target_paths(dsd_trunk_scan_target* target, const dsd_trunk_scan_row_parse* parse, const char* chan_csv,
                        const char* keys_hex_s, const char* keys_dec_s, const char* p25_bandplan_s) {
    if (p25_bandplan_s[0] != '\0') {
        if (trunk_scan_type_is_conventional(target->type)) {
            scan_set_error(parse->err, parse->err_sz, "row %u sets p25_bandplan_csv for a conventional target",
                           parse->row);
            return -1;
        }
        if (dsd_path_resolve_relative_to_file(parse->resolved_path, p25_bandplan_s, target->p25_bandplan_csv,
                                              sizeof target->p25_bandplan_csv)
            != 0) {
            scan_set_error(parse->err, parse->err_sz, "row %u p25_bandplan_csv path is too long or invalid",
                           parse->row);
            return -1;
        }
    }
    if (chan_csv[0] != '\0') {
        if (trunk_scan_type_is_conventional(target->type)) {
            scan_set_error(parse->err, parse->err_sz, "row %u sets chan_csv for a conventional target", parse->row);
            return -1;
        }
        if (dsd_path_resolve_relative_to_file(parse->resolved_path, chan_csv, target->chan_csv, sizeof target->chan_csv)
            != 0) {
            scan_set_error(parse->err, parse->err_sz, "row %u chan_csv path is too long or invalid", parse->row);
            return -1;
        }
    }
    if (keys_hex_s[0] != '\0'
        && dsd_path_resolve_relative_to_file(parse->resolved_path, keys_hex_s, target->keys_hex_csv,
                                             sizeof target->keys_hex_csv)
               != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u keys_hex_csv path is too long or invalid", parse->row);
        return -1;
    }
    if (keys_dec_s[0] != '\0'
        && dsd_path_resolve_relative_to_file(parse->resolved_path, keys_dec_s, target->keys_dec_csv,
                                             sizeof target->keys_dec_csv)
               != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u keys_dec_csv path is too long or invalid", parse->row);
        return -1;
    }
    return 0;
}

static int
scan_parse_target_row(char* line, dsd_trunk_scan_target_list* parsed, const dsd_trunk_scan_row_parse* parse) {
    char* fields[DSD_TRUNK_SCAN_MAX_CSV_FIELDS] = {0};
    size_t field_count = 0;
    if (scan_split_csv_fields(line, fields, DSD_TRUNK_SCAN_MAX_CSV_FIELDS, &field_count) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has too many CSV fields", parse->row);
        return -1;
    }
    if (field_count < DSD_TRUNK_SCAN_REQUIRED_CSV_FIELDS) {
        scan_set_error(parse->err, parse->err_sz, "row %u must contain at least 7 CSV fields", parse->row);
        return -1;
    }

    const char* modulation_s = scan_optional_field(fields, field_count, parse->modulation_idx);
    const char* rtl_gain_s = scan_optional_field(fields, field_count, parse->rtl_gain_idx);
    const char* keys_hex_s = scan_optional_field(fields, field_count, parse->keys_hex_idx);
    const char* keys_dec_s = scan_optional_field(fields, field_count, parse->keys_dec_idx);
    const char* p25_bandplan_s = scan_optional_field(fields, field_count, parse->p25_bandplan_idx);

    dsd_trunk_scan_target target;
    DSD_MEMSET(&target, 0, sizeof(target));
    const char* chan_csv = "";
    const char* dwell_s = "";
    const char* hold_s = "";
    if (scan_parse_target_base_fields(fields, parsed, parse, &target, &chan_csv, &dwell_s, &hold_s) != 0
        || scan_parse_target_overrides(&target, parse, modulation_s, rtl_gain_s) != 0) {
        return -1;
    }
    if (scan_has_duplicate_type_freq(parsed, target.type, target.frequency_hz)) {
        scan_set_error(parse->err, parse->err_sz, "row %u duplicates target type/frequency", parse->row);
        return -1;
    }

    if (scan_parse_target_paths(&target, parse, chan_csv, keys_hex_s, keys_dec_s, p25_bandplan_s) != 0) {
        return -1;
    }
    if (scan_parse_ms_field(dwell_s, parse->default_dwell_ms, &target.dwell_ms) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid dwell_ms '%s'", parse->row, dwell_s);
        return -1;
    }
    if (scan_parse_ms_field(hold_s, parse->default_hold_ms, &target.activity_hold_ms) != 0) {
        scan_set_error(parse->err, parse->err_sz, "row %u has invalid activity_hold_ms '%s'", parse->row, hold_s);
        return -1;
    }

    if (parsed->count >= dsd_trunk_scan_max_targets()) {
        scan_set_error(parse->err, parse->err_sz,
                       "row %u exceeds the trunk scan target budget: each target reserves %zu KB of "
                       "decoder state, so %zu targets is the most that fits in %d MB",
                       parse->row, sizeof(dsd_trunk_scan_target_runtime) / 1024U, dsd_trunk_scan_max_targets(),
                       DSD_TRUNK_SCAN_TARGET_MEMORY_BUDGET_BYTES / (1024 * 1024));
        return -1;
    }
    if (scan_target_list_reserve(parsed, parsed->count + 1) != 0) {
        scan_set_error(parse->err, parse->err_sz, "out of memory loading trunk scan targets");
        return -1;
    }
    parsed->targets[parsed->count++] = target;
    return 0;
}

/* Optional columns are matched by header name; naming one twice rejects the file. */
static int
scan_match_optional_header(dsd_trunk_scan_row_parse* parse, const char* name, size_t index) {
    const struct {
        const char* name;
        int* idx;
    } cols[] = {
        {"modulation", &parse->modulation_idx},         {"rtl_gain", &parse->rtl_gain_idx},
        {"keys_hex_csv", &parse->keys_hex_idx},         {"keys_dec_csv", &parse->keys_dec_idx},
        {"p25_bandplan_csv", &parse->p25_bandplan_idx},
    };

    for (size_t k = 0; k < sizeof cols / sizeof cols[0]; k++) {
        if (strcmp(name, cols[k].name) != 0) {
            continue;
        }
        if (*cols[k].idx >= 0) {
            scan_set_error(parse->err, parse->err_sz, "trunk scan target CSV header duplicates '%s'", cols[k].name);
            return -1;
        }
        *cols[k].idx = (int)index;
        return 0;
    }
    return 0;
}

static int
scan_read_target_csv_header(FILE* fp, char* line, size_t line_sz, dsd_trunk_scan_row_parse* parse) {
    if (!fgets(line, line_sz, fp)) {
        scan_set_error(parse->err, parse->err_sz, "trunk scan target CSV is empty");
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';

    char* fields[DSD_TRUNK_SCAN_MAX_CSV_FIELDS] = {0};
    size_t field_count = 0;
    if (scan_split_csv_fields(line, fields, DSD_TRUNK_SCAN_MAX_CSV_FIELDS, &field_count) != 0
        || field_count < DSD_TRUNK_SCAN_REQUIRED_CSV_FIELDS) {
        scan_set_error(parse->err, parse->err_sz, "trunk scan target CSV header must start with '%s'",
                       k_trunk_scan_csv_header);
        return -1;
    }

    static const char* const required[DSD_TRUNK_SCAN_REQUIRED_CSV_FIELDS] = {
        "id", "type", "frequency_hz", "chan_csv", "dwell_ms", "activity_hold_ms", "notes",
    };
    for (size_t i = 0; i < DSD_TRUNK_SCAN_REQUIRED_CSV_FIELDS; i++) {
        const char* name = scan_unquote(fields[i]);
        if (strcmp(name, required[i]) != 0) {
            scan_set_error(parse->err, parse->err_sz, "trunk scan target CSV header must start with '%s'",
                           k_trunk_scan_csv_header);
            return -1;
        }
    }

    parse->modulation_idx = -1;
    parse->rtl_gain_idx = -1;
    parse->keys_hex_idx = -1;
    parse->keys_dec_idx = -1;
    parse->p25_bandplan_idx = -1;
    for (size_t i = DSD_TRUNK_SCAN_REQUIRED_CSV_FIELDS; i < field_count; i++) {
        if (scan_match_optional_header(parse, scan_unquote(fields[i]), i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int
scan_target_list_has_rtl_gain_override(const dsd_trunk_scan_target_list* list) {
    if (!list) {
        return 0;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (list->targets[i].rtl_gain_is_set) {
            return 1;
        }
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
    dsd_trunk_scan_row_parse parse;
    parse.resolved_path = resolved_path;
    parse.default_dwell_ms =
        scan_default_ms(opts ? opts->trunk_scan_idle_dwell_ms : 0, DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS);
    parse.default_hold_ms =
        scan_default_ms(opts ? opts->trunk_scan_activity_hold_ms : 0, DSD_TRUNK_SCAN_ACTIVITY_HOLD_DEFAULT_MS);
    parse.err = err;
    parse.err_sz = err_sz;
    parse.modulation_idx = -1;
    parse.rtl_gain_idx = -1;

    if (scan_read_target_csv_header(fp, line, sizeof line, &parse) != 0) {
        fclose(fp);
        return -1;
    }

    int rows_rc = scan_load_target_csv_rows(fp, line, sizeof line, &parsed, &parse);
    fclose(fp);
    if (rows_rc != 0) {
        dsd_trunk_scan_target_list_reset(&parsed);
        return -1;
    }
    if (parsed.count == 0) {
        scan_set_error(err, err_sz, "trunk scan target CSV has no targets");
        dsd_trunk_scan_target_list_reset(&parsed);
        return -1;
    }
    *out = parsed;
    return 0;
}

void
dsd_trunk_scan_target_list_reset(dsd_trunk_scan_target_list* list) {
    if (!list) {
        return;
    }
    free(list->targets);
    list->targets = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int
trunk_scan_snapshot_lcn_ext_reserve(dsd_trunk_scan_snapshot* snapshot, size_t ext_needed) {
    if (ext_needed <= snapshot->trunk_lcn_freq_ext_capacity) {
        return 0;
    }
    size_t capacity = snapshot->trunk_lcn_freq_ext_capacity > 0 ? snapshot->trunk_lcn_freq_ext_capacity : 8;
    while (capacity < ext_needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = ext_needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(long int)) {
        return -1;
    }
    long int* ext = (long int*)realloc(snapshot->trunk_lcn_freq_ext, capacity * sizeof *ext);
    if (!ext) {
        return -1;
    }
    DSD_MEMSET(ext + snapshot->trunk_lcn_freq_ext_capacity, 0,
               (capacity - snapshot->trunk_lcn_freq_ext_capacity) * sizeof *ext);
    snapshot->trunk_lcn_freq_ext = ext;
    snapshot->trunk_lcn_freq_ext_capacity = capacity;
    return 0;
}

/*
 * Grow the sparse channel-map arrays to hold at least @p needed entries, doubling like
 * trunk_scan_snapshot_lcn_ext_reserve(). Both arrays are grown together so an index is valid in
 * either or in neither; on failure the snapshot keeps the arrays it already had.
 */
static int
trunk_scan_snapshot_chan_map_reserve(dsd_trunk_scan_snapshot* snapshot, size_t needed) {
    if (needed <= snapshot->chan_map_capacity) {
        return 0;
    }
    size_t capacity = snapshot->chan_map_capacity > 0 ? snapshot->chan_map_capacity : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(long int)) {
        return -1;
    }
    uint16_t* chans = (uint16_t*)realloc(snapshot->chan_map_chan, capacity * sizeof *chans);
    if (!chans) {
        return -1;
    }
    snapshot->chan_map_chan = chans;
    long int* freqs = (long int*)realloc(snapshot->chan_map_freq, capacity * sizeof *freqs);
    if (!freqs) {
        /* chan_map_chan already grew; capacity stays at the old value so the two arrays are
         * never both indexed past the shorter one. */
        return -1;
    }
    snapshot->chan_map_freq = freqs;
    snapshot->chan_map_capacity = capacity;
    return 0;
}

/*
 * Capture state's channel map as (channel, frequency) pairs. state->trunk_chan_map_used[] is
 * already the exact sorted set of mapped channels, so this walks it directly rather than
 * scanning 64K slots.
 */
static void
trunk_scan_save_chan_map_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    uint32_t used = state->trunk_chan_map_used_count;
    if (used > (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE) {
        used = (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE;
    }
    if (used > 0 && trunk_scan_snapshot_chan_map_reserve(snapshot, (size_t)used) != 0) {
        LOG_WARN("trunk scan: could not capture %u mapped channels for the parked target; its "
                 "channel map will be relearned\n",
                 (unsigned int)used);
        used = 0;
    }
    for (uint32_t i = 0; i < used; i++) {
        const uint16_t chan = state->trunk_chan_map_used[i];
        snapshot->chan_map_chan[i] = chan;
        snapshot->chan_map_freq[i] = state->trunk_chan_map[chan];
    }
    snapshot->trunk_chan_map_used_count = used;
    snapshot->trunk_chan_map_seq = state->trunk_chan_map_seq;
}

/*
 * Reinstate a saved channel map. Only the outgoing target's mapped channels are cleared - a
 * DSD_MEMSET of the whole 64K-entry map would cost half a megabyte on every rotation.
 */
static void
trunk_scan_restore_chan_map_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    uint32_t stale = state->trunk_chan_map_used_count;
    if (stale > (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE) {
        stale = (uint32_t)DSD_TRUNK_CHAN_MAP_SIZE;
    }
    for (uint32_t i = 0; i < stale; i++) {
        state->trunk_chan_map[state->trunk_chan_map_used[i]] = 0;
    }

    /* Bounded by all three of: what the snapshot claims, what it actually allocated, and what
     * state->trunk_chan_map_used[] can hold - this loop writes into that fixed-size array. */
    size_t used = snapshot->trunk_chan_map_used_count;
    if (!snapshot->chan_map_chan || !snapshot->chan_map_freq) {
        used = 0;
    }
    if (used > snapshot->chan_map_capacity) {
        used = snapshot->chan_map_capacity;
    }
    if (used > (size_t)DSD_TRUNK_CHAN_MAP_SIZE) {
        used = (size_t)DSD_TRUNK_CHAN_MAP_SIZE;
    }
    for (size_t i = 0; i < used; i++) {
        const uint16_t chan = snapshot->chan_map_chan[i];
        state->trunk_chan_map[chan] = snapshot->chan_map_freq[i];
        state->trunk_chan_map_used[i] = chan;
    }
    state->trunk_chan_map_used_count = (uint32_t)used;
    state->trunk_chan_map_seq = snapshot->trunk_chan_map_seq;
}

/*
 * nxdn_trunk_diag_chan_freq_fn over a snapshot's sparse channel map. chan_map_chan[] is kept in
 * the ascending order state->trunk_chan_map_used[] maintains, so this binary-searches it.
 */
static long int
trunk_scan_snapshot_chan_lookup(const void* ctx, uint16_t channel) {
    const dsd_trunk_scan_snapshot* snapshot = (const dsd_trunk_scan_snapshot*)ctx;
    if (!snapshot || !snapshot->chan_map_chan || !snapshot->chan_map_freq) {
        return 0;
    }
    size_t lo = 0;
    size_t hi = snapshot->trunk_chan_map_used_count;
    if (hi > snapshot->chan_map_capacity) {
        hi = snapshot->chan_map_capacity;
    }
    while (lo < hi) {
        const size_t mid = lo + ((hi - lo) / 2U);
        const uint16_t at = snapshot->chan_map_chan[mid];
        if (at == channel) {
            return snapshot->chan_map_freq[mid];
        }
        if (at < channel) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static void
trunk_scan_snapshot_clear(dsd_trunk_scan_snapshot* snapshot) {
    free(snapshot->trunk_lcn_freq_ext);
    free(snapshot->chan_map_chan);
    free(snapshot->chan_map_freq);
    DSD_MEMSET(snapshot, 0, sizeof(*snapshot));
    snapshot->dmr_mfid = -1;
    snapshot->dmr_color_code = 16;
    snapshot->dmr_confidence_color_code = 16;
    snapshot->dmr_confidence_candidate_cc = 16;
    snapshot->dmr_rest_channel = -1;
    snapshot->p25_cc_is_tdma = 2;
    snapshot->p25_vc_cqpsk_pref = -1;
    snapshot->p25_vc_cqpsk_override = -1;
    snapshot->p25_p1_validated_rf_mod = -1;
    /* NXDN "not decoded yet" sentinels, matching initState() (src/core/util/dsd_init.c) and
     * noCarrier(): RAN 0 is a legal value, so a zeroed snapshot would publish a fabricated
     * RAN 0 for a target that has never decoded one. */
    snapshot->nxdn_last_ran = (unsigned int)-1;
    snapshot->nxdn_location_category[0] = ' ';
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
trunk_scan_save_call_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    (void)dsd_call_context_copy_snapshot(state, &snapshot->call_context);
    if (state->event_history_s != NULL) {
        dsd_event_history_transaction transaction;
        dsd_event_history_transaction_begin((dsd_state*)state, &transaction);
        for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
            DSD_MEMCPY(&snapshot->event_current[slot], &state->event_history_s[slot].Event_History_Items[0],
                       sizeof(snapshot->event_current[slot]));
        }
        dsd_event_history_transaction_end(&transaction);
    } else {
        DSD_MEMSET(snapshot->event_current, 0, sizeof(snapshot->event_current));
    }
    snapshot->dmr_so = state->dmr_so;
    snapshot->dmr_soR = state->dmr_soR;
}

static void
trunk_scan_restore_call_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    (void)dsd_call_context_restore_snapshot(state, &snapshot->call_context);
    if (state->event_history_s != NULL) {
        dsd_event_history_transaction transaction;
        dsd_event_history_transaction_begin(state, &transaction);
        for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
            Event_History* current = &state->event_history_s[slot].Event_History_Items[0];
            // Saved rows are exact byte copies, so padding bytes have defined snapshot values.
            // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c)
            if (memcmp(current, &snapshot->event_current[slot], sizeof(*current)) != 0) {
                DSD_MEMCPY(current, &snapshot->event_current[slot], sizeof(*current));
                dsd_event_history_mark_dirty(&state->event_history_s[slot]);
            }
        }
        dsd_event_history_transaction_end(&transaction);
    }
    state->dmr_so = snapshot->dmr_so;
    state->dmr_soR = snapshot->dmr_soR;
}

// The encrypted-target lockout ledger is snapshot per scan target so one
// system's lockouts never bleed into another. The global key epoch is
// deliberately NOT snapshot: entries restored from before a key import carry
// a stale epoch and re-verify with one probe, exactly like live entries. The
// eviction sequence counter is global for the same reason -- restored entries
// keep their older ticket and so evict ahead of freshly confirmed ones.
static void
trunk_scan_save_enc_lockout_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    DSD_MEMCPY(snapshot->enc_lockout_entries, state->enc_lockout_entries, sizeof(snapshot->enc_lockout_entries));
}

static void
trunk_scan_restore_enc_lockout_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    DSD_MEMCPY(state->enc_lockout_entries, snapshot->enc_lockout_entries, sizeof(state->enc_lockout_entries));
}

static void
trunk_scan_save_p25_identity_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    snapshot->p2_wacn = state->p2_wacn;
    snapshot->p2_sysid = state->p2_sysid;
    snapshot->p2_cc = state->p2_cc;
    snapshot->p2_siteid = state->p2_siteid;
    snapshot->p2_rfssid = state->p2_rfssid;
    snapshot->p25_cc_freq = state->p25_cc_freq;
    snapshot->trunk_cc_freq = state->trunk_cc_freq;
    DSD_MEMCPY(snapshot->p25_vc_freq, state->p25_vc_freq, sizeof(snapshot->p25_vc_freq));
    DSD_MEMCPY(snapshot->trunk_vc_freq, state->trunk_vc_freq, sizeof(snapshot->trunk_vc_freq));
    trunk_scan_save_enc_lockout_snapshot(state, snapshot);
    DSD_MEMCPY(snapshot->trunk_lcn_freq, state->trunk_lcn_freq, sizeof(snapshot->trunk_lcn_freq));
    if (state->lcn_freq_count > DSD_TRUNK_LCN_EMBEDDED) {
        const size_t ext_count = (size_t)state->lcn_freq_count - (size_t)DSD_TRUNK_LCN_EMBEDDED;
        if (trunk_scan_snapshot_lcn_ext_reserve(snapshot, ext_count) == 0) {
            DSD_MEMCPY(snapshot->trunk_lcn_freq_ext, state->trunk_lcn_freq_ext,
                       ext_count * sizeof(snapshot->trunk_lcn_freq_ext[0]));
            snapshot->trunk_lcn_freq_ext_count = ext_count;
        } else {
            /* Tail capture failed; the dmr save clamps the snapshot count to
             * the embedded slots so the snapshot stays self-consistent. */
            snapshot->trunk_lcn_freq_ext_count = 0;
        }
    } else {
        snapshot->trunk_lcn_freq_ext_count = 0;
    }
    trunk_scan_save_chan_map_snapshot(state, snapshot);
    DSD_MEMCPY(snapshot->dmr_lcn_trust, state->dmr_lcn_trust, sizeof(snapshot->dmr_lcn_trust));
    DSD_MEMCPY(snapshot->p25_chan_tdma_explicit, state->p25_chan_tdma_explicit,
               sizeof(snapshot->p25_chan_tdma_explicit));
    snapshot->p25_chan_iden = state->p25_chan_iden;
    DSD_MEMCPY(snapshot->p25_iden_fdma, state->p25_iden_fdma, sizeof(snapshot->p25_iden_fdma));
    DSD_MEMCPY(snapshot->p25_iden_tdma, state->p25_iden_tdma, sizeof(snapshot->p25_iden_tdma));
    DSD_MEMCPY(snapshot->p25_bandplan_rows, state->p25_bandplan_rows, sizeof(snapshot->p25_bandplan_rows));
    snapshot->p25_bandplan_row_count = state->p25_bandplan_row_count;
    snapshot->p25_cc_is_tdma = state->p25_cc_is_tdma;
    snapshot->p25_sys_is_tdma = state->p25_sys_is_tdma;
    snapshot->p25_vc_cqpsk_pref = state->p25_vc_cqpsk_pref;
    snapshot->p25_vc_cqpsk_override = state->p25_vc_cqpsk_override;
    snapshot->p25_p1_validated_rf_mod = state->p25_p1_validated_rf_mod;
    snapshot->p25_sm_mode = state->p25_sm_mode;
    snapshot->nac = state->nac;
    snapshot->samplesPerSymbol = state->samplesPerSymbol;
    snapshot->symbolCenter = state->symbolCenter;
    snapshot->rf_mod = state->rf_mod;
}

static void
trunk_scan_restore_p25_identity_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    state->p2_wacn = snapshot->p2_wacn;
    state->p2_sysid = snapshot->p2_sysid;
    state->p2_cc = snapshot->p2_cc;
    state->p2_siteid = snapshot->p2_siteid;
    state->p2_rfssid = snapshot->p2_rfssid;
    state->p25_cc_freq = snapshot->p25_cc_freq;
    state->trunk_cc_freq = snapshot->trunk_cc_freq;
    DSD_MEMCPY(state->p25_vc_freq, snapshot->p25_vc_freq, sizeof(state->p25_vc_freq));
    DSD_MEMCPY(state->trunk_vc_freq, snapshot->trunk_vc_freq, sizeof(state->trunk_vc_freq));
    trunk_scan_restore_enc_lockout_snapshot(state, snapshot);
    DSD_MEMCPY(state->trunk_lcn_freq, snapshot->trunk_lcn_freq, sizeof(state->trunk_lcn_freq));
    trunk_scan_restore_chan_map_snapshot(state, snapshot);
    DSD_MEMCPY(state->dmr_lcn_trust, snapshot->dmr_lcn_trust, sizeof(state->dmr_lcn_trust));
    DSD_MEMCPY(state->p25_chan_tdma_explicit, snapshot->p25_chan_tdma_explicit, sizeof(state->p25_chan_tdma_explicit));
    state->p25_chan_iden = snapshot->p25_chan_iden;
    DSD_MEMCPY(state->p25_iden_fdma, snapshot->p25_iden_fdma, sizeof(state->p25_iden_fdma));
    DSD_MEMCPY(state->p25_iden_tdma, snapshot->p25_iden_tdma, sizeof(state->p25_iden_tdma));
    DSD_MEMCPY(state->p25_bandplan_rows, snapshot->p25_bandplan_rows, sizeof(state->p25_bandplan_rows));
    state->p25_bandplan_row_count = snapshot->p25_bandplan_row_count;
    state->p25_cc_is_tdma = snapshot->p25_cc_is_tdma;
    state->p25_sys_is_tdma = snapshot->p25_sys_is_tdma;
    state->p25_vc_cqpsk_pref = snapshot->p25_vc_cqpsk_pref;
    state->p25_vc_cqpsk_override = snapshot->p25_vc_cqpsk_override;
    state->p25_p1_validated_rf_mod = snapshot->p25_p1_validated_rf_mod;
    state->p25_sm_mode = snapshot->p25_sm_mode;
    state->nac = snapshot->nac;
    state->samplesPerSymbol = snapshot->samplesPerSymbol;
    state->symbolCenter = snapshot->symbolCenter;
    state->rf_mod = snapshot->rf_mod;
}

static void
trunk_scan_save_p25_metadata_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    snapshot->p25_prot_valid = state->p25_prot_valid;
    snapshot->p25_prot_algid = state->p25_prot_algid;
    snapshot->p25_prot_kid = state->p25_prot_kid;
    snapshot->p25_cc_prot_valid = state->p25_cc_prot_valid;
    snapshot->p25_cc_prot_algid = state->p25_cc_prot_algid;
    snapshot->p25_sys_time_valid = state->p25_sys_time_valid;
    snapshot->p25_sys_time = state->p25_sys_time;
    snapshot->p25_sys_time_offset_valid = state->p25_sys_time_offset_valid;
    snapshot->p25_sys_time_offset = state->p25_sys_time_offset;
    snapshot->p25_sys_services_valid = state->p25_sys_services_valid;
    snapshot->p25_sys_services_available = state->p25_sys_services_available;
    snapshot->p25_sys_services_supported = state->p25_sys_services_supported;
    snapshot->p25_sys_services_request_priority = state->p25_sys_services_request_priority;
    snapshot->p25_site_lra_valid = state->p25_site_lra_valid;
    snapshot->p25_site_lra = state->p25_site_lra;
    snapshot->p25_site_network_active_valid = state->p25_site_network_active_valid;
    snapshot->p25_site_network_active = state->p25_site_network_active;
}

static void
trunk_scan_restore_p25_metadata_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    state->p25_prot_valid = snapshot->p25_prot_valid;
    state->p25_prot_algid = snapshot->p25_prot_algid;
    state->p25_prot_kid = snapshot->p25_prot_kid;
    state->p25_cc_prot_valid = snapshot->p25_cc_prot_valid;
    state->p25_cc_prot_algid = snapshot->p25_cc_prot_algid;
    state->p25_sys_time_valid = snapshot->p25_sys_time_valid;
    state->p25_sys_time = snapshot->p25_sys_time;
    state->p25_sys_time_offset_valid = snapshot->p25_sys_time_offset_valid;
    state->p25_sys_time_offset = snapshot->p25_sys_time_offset;
    state->p25_sys_services_valid = snapshot->p25_sys_services_valid;
    state->p25_sys_services_available = snapshot->p25_sys_services_available;
    state->p25_sys_services_supported = snapshot->p25_sys_services_supported;
    state->p25_sys_services_request_priority = snapshot->p25_sys_services_request_priority;
    state->p25_site_lra_valid = snapshot->p25_site_lra_valid;
    state->p25_site_lra = snapshot->p25_site_lra;
    state->p25_site_network_active_valid = snapshot->p25_site_network_active_valid;
    state->p25_site_network_active = snapshot->p25_site_network_active;
}

static void
trunk_scan_save_p25_patch_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
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
}

static void
trunk_scan_restore_p25_patch_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
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
}

static void
trunk_scan_save_p25_catalog_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    snapshot->p25_aff_count = state->p25_aff_count;
    DSD_MEMCPY(snapshot->p25_aff_rid, state->p25_aff_rid, sizeof(snapshot->p25_aff_rid));
    DSD_MEMCPY(snapshot->p25_aff_last_seen, state->p25_aff_last_seen, sizeof(snapshot->p25_aff_last_seen));
    snapshot->p25_ga_count = state->p25_ga_count;
    DSD_MEMCPY(snapshot->p25_ga_rid, state->p25_ga_rid, sizeof(snapshot->p25_ga_rid));
    DSD_MEMCPY(snapshot->p25_ga_tg, state->p25_ga_tg, sizeof(snapshot->p25_ga_tg));
    DSD_MEMCPY(snapshot->p25_ga_last_seen, state->p25_ga_last_seen, sizeof(snapshot->p25_ga_last_seen));
    snapshot->p25_nb_count = state->p25_nb_count;
    DSD_MEMCPY(snapshot->p25_nb_entries, state->p25_nb_entries, sizeof(snapshot->p25_nb_entries));
    snapshot->p25_secondary_cc_count = state->p25_secondary_cc_count;
    DSD_MEMCPY(snapshot->p25_secondary_cc_entries, state->p25_secondary_cc_entries,
               sizeof(snapshot->p25_secondary_cc_entries));
    snapshot->p25_pending_announcement_count = state->p25_pending_announcement_count;
    DSD_MEMCPY(snapshot->p25_pending_announcements, state->p25_pending_announcements,
               sizeof(snapshot->p25_pending_announcements));
    snapshot->p25_src_nid = state->p25_src_nid;
}

static void
trunk_scan_restore_p25_catalog_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    state->p25_aff_count = snapshot->p25_aff_count;
    DSD_MEMCPY(state->p25_aff_rid, snapshot->p25_aff_rid, sizeof(state->p25_aff_rid));
    DSD_MEMCPY(state->p25_aff_last_seen, snapshot->p25_aff_last_seen, sizeof(state->p25_aff_last_seen));
    state->p25_ga_count = snapshot->p25_ga_count;
    DSD_MEMCPY(state->p25_ga_rid, snapshot->p25_ga_rid, sizeof(state->p25_ga_rid));
    DSD_MEMCPY(state->p25_ga_tg, snapshot->p25_ga_tg, sizeof(state->p25_ga_tg));
    DSD_MEMCPY(state->p25_ga_last_seen, snapshot->p25_ga_last_seen, sizeof(state->p25_ga_last_seen));
    state->p25_nb_count = snapshot->p25_nb_count;
    DSD_MEMCPY(state->p25_nb_entries, snapshot->p25_nb_entries, sizeof(state->p25_nb_entries));
    state->p25_secondary_cc_count = snapshot->p25_secondary_cc_count;
    DSD_MEMCPY(state->p25_secondary_cc_entries, snapshot->p25_secondary_cc_entries,
               sizeof(state->p25_secondary_cc_entries));
    state->p25_pending_announcement_count = snapshot->p25_pending_announcement_count;
    DSD_MEMCPY(state->p25_pending_announcements, snapshot->p25_pending_announcements,
               sizeof(state->p25_pending_announcements));
    state->p25_src_nid = snapshot->p25_src_nid;
}

static void
trunk_scan_save_p25_eval_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    trunk_scan_save_call_snapshot(state, snapshot);
    snapshot->p25_cc_eval_freq = state->p25_cc_eval_freq;
    snapshot->p25_cc_eval_start_m = state->p25_cc_eval_start_m;
    snapshot->p25_cc_cache_loaded = state->p25_cc_cache_loaded;
}

static void
trunk_scan_restore_p25_eval_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    trunk_scan_restore_call_snapshot(state, snapshot);
    state->p25_cc_eval_freq = snapshot->p25_cc_eval_freq;
    state->p25_cc_eval_start_m = snapshot->p25_cc_eval_start_m;
    state->p25_cc_cache_loaded = snapshot->p25_cc_cache_loaded;
}

static void
trunk_scan_save_dmr_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    snapshot->dmr_mfid = state->dmr_mfid;
    snapshot->dmr_fid = state->dmr_fid;
    snapshot->dmr_so = state->dmr_so;
    snapshot->dmr_fidR = state->dmr_fidR;
    snapshot->dmr_soR = state->dmr_soR;
    snapshot->dmr_t3_syscode = state->dmr_t3_syscode;
    trunk_scan_save_dmr_confidence_snapshot(state, snapshot);
    DSD_MEMCPY(snapshot->dmr_branding, state->dmr_branding, sizeof(snapshot->dmr_branding));
    DSD_MEMCPY(snapshot->dmr_branding_sub, state->dmr_branding_sub, sizeof(snapshot->dmr_branding_sub));
    DSD_MEMCPY(snapshot->dmr_site_parms, state->dmr_site_parms, sizeof(snapshot->dmr_site_parms));
    snapshot->dmr_rest_channel = state->dmr_rest_channel;
    snapshot->lcn_freq_count = state->lcn_freq_count;
    snapshot->lcn_freq_roll = state->lcn_freq_roll;
    /* Compare against the tail actually captured rather than testing it for 0: the clamp then
     * holds whatever order the per-protocol savers run in, instead of depending on
     * trunk_scan_save_p25_identity_snapshot() having refreshed ext_count first. */
    if (snapshot->lcn_freq_count > DSD_TRUNK_LCN_EMBEDDED
        && snapshot->trunk_lcn_freq_ext_count < (size_t)(snapshot->lcn_freq_count - DSD_TRUNK_LCN_EMBEDDED)) {
        snapshot->lcn_freq_count = DSD_TRUNK_LCN_EMBEDDED;
        snapshot->lcn_freq_roll = 0;
    }
    snapshot->is_con_plus = state->is_con_plus;
}

static void
trunk_scan_restore_dmr_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    state->dmr_mfid = snapshot->dmr_mfid;
    state->dmr_fid = snapshot->dmr_fid;
    state->dmr_so = snapshot->dmr_so;
    state->dmr_fidR = snapshot->dmr_fidR;
    state->dmr_soR = snapshot->dmr_soR;
    state->dmr_t3_syscode = snapshot->dmr_t3_syscode;
    trunk_scan_restore_dmr_confidence_snapshot(state, snapshot);
    DSD_MEMCPY(state->dmr_branding, snapshot->dmr_branding, sizeof(state->dmr_branding));
    DSD_MEMCPY(state->dmr_branding_sub, snapshot->dmr_branding_sub, sizeof(state->dmr_branding_sub));
    DSD_MEMCPY(state->dmr_site_parms, snapshot->dmr_site_parms, sizeof(state->dmr_site_parms));
    state->dmr_rest_channel = snapshot->dmr_rest_channel;
    state->lcn_freq_roll = snapshot->lcn_freq_roll;
    state->lcn_freq_count = snapshot->lcn_freq_count;
    if (state->lcn_freq_count > DSD_TRUNK_LCN_EMBEDDED) {
        /* The tail the snapshot actually captured is authoritative, not lcn_freq_count:
         * the save path clamps the count when the capture failed, but bounding the copy
         * here means no save ordering can make this read past the snapshot buffer. */
        const size_t ext_count = (size_t)state->lcn_freq_count - (size_t)DSD_TRUNK_LCN_EMBEDDED;
        if (snapshot->trunk_lcn_freq_ext != NULL && ext_count <= snapshot->trunk_lcn_freq_ext_count
            && dsd_state_trunk_lcn_reserve(state, (size_t)state->lcn_freq_count) == 0) {
            DSD_MEMCPY(state->trunk_lcn_freq_ext, snapshot->trunk_lcn_freq_ext,
                       ext_count * sizeof(state->trunk_lcn_freq_ext[0]));
        } else {
            LOG_WARN("WARNING: Trunk scan could not restore %d scan-list entries; keeping the first %d\n",
                     snapshot->lcn_freq_count, (int)DSD_TRUNK_LCN_EMBEDDED);
            state->lcn_freq_count = DSD_TRUNK_LCN_EMBEDDED;
            state->lcn_freq_roll = 0;
        }
    }
    state->is_con_plus = snapshot->is_con_plus;
}

static void
trunk_scan_save_nxdn_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    snapshot->nxdn_grant_chan = state->nxdn_grant_chan;
    snapshot->nxdn_grant_freq = state->nxdn_grant_freq;
    snapshot->nxdn_last_ran = state->nxdn_last_ran;
    snapshot->nxdn_location_sys_code = state->nxdn_location_sys_code;
    snapshot->nxdn_location_site_code = state->nxdn_location_site_code;
    DSD_MEMCPY(snapshot->nxdn_location_category, state->nxdn_location_category,
               sizeof(snapshot->nxdn_location_category));
    snapshot->nxdn_rcn = state->nxdn_rcn;
    snapshot->nxdn_base_freq = state->nxdn_base_freq;
    snapshot->nxdn_step = state->nxdn_step;
    snapshot->nxdn_bw = state->nxdn_bw;
    // Channels seen without a mapping are a property of the site, not of the decoder, so each
    // target keeps its own ledger rather than accumulating every target's misses in one.
    nxdn_trunk_diag_ledger_save(state, &snapshot->nxdn_diag);
}

static void
trunk_scan_restore_nxdn_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    state->nxdn_grant_chan = snapshot->nxdn_grant_chan;
    state->nxdn_grant_freq = snapshot->nxdn_grant_freq;
    state->nxdn_last_ran = snapshot->nxdn_last_ran;
    state->nxdn_location_sys_code = snapshot->nxdn_location_sys_code;
    state->nxdn_location_site_code = snapshot->nxdn_location_site_code;
    DSD_MEMCPY(state->nxdn_location_category, snapshot->nxdn_location_category, sizeof(state->nxdn_location_category));
    state->nxdn_rcn = snapshot->nxdn_rcn;
    state->nxdn_base_freq = snapshot->nxdn_base_freq;
    state->nxdn_step = snapshot->nxdn_step;
    state->nxdn_bw = snapshot->nxdn_bw;
    nxdn_trunk_diag_ledger_restore(state, &snapshot->nxdn_diag);
}

static void
trunk_scan_save_timing_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    snapshot->last_cc_sync_time = state->last_cc_sync_time;
    snapshot->p25_last_cc_msg_time = state->p25_last_cc_msg_time;
    snapshot->last_vc_sync_time = state->last_vc_sync_time;
    snapshot->p25_last_vc_tune_time = state->p25_last_vc_tune_time;
    snapshot->last_t3_tune_time = state->last_t3_tune_time;
    snapshot->last_cc_sync_time_m = state->last_cc_sync_time_m;
    snapshot->p25_last_cc_msg_time_m = state->p25_last_cc_msg_time_m;
    snapshot->last_vc_sync_time_m = state->last_vc_sync_time_m;
    snapshot->p25_last_vc_tune_time_m = state->p25_last_vc_tune_time_m;
    snapshot->last_t3_tune_time_m = state->last_t3_tune_time_m;
}

static void
trunk_scan_restore_timing_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    state->last_cc_sync_time = snapshot->last_cc_sync_time;
    state->p25_last_cc_msg_time = snapshot->p25_last_cc_msg_time;
    state->last_vc_sync_time = snapshot->last_vc_sync_time;
    state->p25_last_vc_tune_time = snapshot->p25_last_vc_tune_time;
    state->last_t3_tune_time = snapshot->last_t3_tune_time;
    state->last_cc_sync_time_m = snapshot->last_cc_sync_time_m;
    state->p25_last_cc_msg_time_m = snapshot->p25_last_cc_msg_time_m;
    state->last_vc_sync_time_m = snapshot->last_vc_sync_time_m;
    state->p25_last_vc_tune_time_m = snapshot->p25_last_vc_tune_time_m;
    state->last_t3_tune_time_m = snapshot->last_t3_tune_time_m;
}

static void
trunk_scan_save_cc_candidate_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    const dsd_trunk_cc_candidates* cc_candidates = dsd_trunk_cc_candidates_peek(state);
    snapshot->has_cc_candidates = cc_candidates ? 1 : 0;
    if (cc_candidates) {
        snapshot->cc_candidates = *cc_candidates;
    } else {
        DSD_MEMSET(&snapshot->cc_candidates, 0, sizeof(snapshot->cc_candidates));
    }
}

static void
trunk_scan_restore_cc_candidate_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
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
trunk_scan_save_snapshot(const dsd_state* state, dsd_trunk_scan_snapshot* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    trunk_scan_save_p25_identity_snapshot(state, snapshot);
    trunk_scan_save_p25_metadata_snapshot(state, snapshot);
    trunk_scan_save_p25_patch_snapshot(state, snapshot);
    trunk_scan_save_p25_catalog_snapshot(state, snapshot);
    trunk_scan_save_p25_eval_snapshot(state, snapshot);
    trunk_scan_save_dmr_snapshot(state, snapshot);
    trunk_scan_save_nxdn_snapshot(state, snapshot);
    trunk_scan_save_timing_snapshot(state, snapshot);
    trunk_scan_save_cc_candidate_snapshot(state, snapshot);
}

static void
trunk_scan_restore_snapshot(dsd_state* state, const dsd_trunk_scan_snapshot* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    trunk_scan_restore_p25_identity_snapshot(state, snapshot);
    trunk_scan_restore_p25_metadata_snapshot(state, snapshot);
    trunk_scan_restore_p25_patch_snapshot(state, snapshot);
    trunk_scan_restore_p25_catalog_snapshot(state, snapshot);
    trunk_scan_restore_p25_eval_snapshot(state, snapshot);
    trunk_scan_restore_dmr_snapshot(state, snapshot);
    trunk_scan_restore_nxdn_snapshot(state, snapshot);
    trunk_scan_restore_timing_snapshot(state, snapshot);
    trunk_scan_restore_cc_candidate_snapshot(state, snapshot);
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
trunk_scan_get(const dsd_state* state) {
    return DSD_STATE_EXT_GET_AS(dsd_trunk_scan_coord, state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
}

/* The published id is the target id verbatim, so the two buffers have to agree. */
_Static_assert(sizeof(((dsd_trunk_scan_target*)0)->id) <= DSD_CHANNEL_LABEL_SIZE,
               "a trunk scan target id must fit the published channel label");

/* Take the published label back down. The coordinator is not the owner of dsd_state, so the
 * sites that detach it -- shutdown, and initState for a fresh run -- clear the publication;
 * trunk_scan_free() gets no state pointer and cannot. */
static void
trunk_scan_clear_published_target(dsd_state* state) {
    if (!state) {
        return;
    }
    state->trunk_scan_active_id[0] = '\0';
    state->trunk_scan_active_ordinal = 0U;
    state->trunk_scan_target_count = 0U;
    state->trunk_scan_hold = 0U;
    state->trunk_scan_active_avoided = 0U;
    state->trunk_scan_avoided_count = 0U;
}

static size_t
trunk_scan_avoided_count(const dsd_trunk_scan_coord* coord) {
    size_t avoided = 0;
    for (size_t i = 0; i < coord->count; i++) {
        avoided += coord->targets[i].avoided ? 1U : 0U;
    }
    return avoided;
}

/* Publish which target the receiver is parked on, for the frontends that label the channel
 * being heard. Written on the decoder thread beside every other dsd_state field, so it
 * reaches the UI through the snapshot rather than through the coordinator. */
static void
trunk_scan_publish_active_target(dsd_state* state, const dsd_trunk_scan_coord* coord) {
    if (!state || !coord || coord->active >= coord->count) {
        trunk_scan_clear_published_target(state);
        return;
    }
    const size_t ordinal = coord->active + 1U;
    DSD_SNPRINTF(state->trunk_scan_active_id, sizeof state->trunk_scan_active_id, "%s",
                 coord->targets[coord->active].target.id);
    state->trunk_scan_active_ordinal = (uint16_t)(ordinal > UINT16_MAX ? UINT16_MAX : ordinal);
    state->trunk_scan_target_count = (uint16_t)(coord->count > UINT16_MAX ? UINT16_MAX : coord->count);
    const size_t avoided = trunk_scan_avoided_count(coord);
    state->trunk_scan_hold = coord->hold_active ? 1U : 0U;
    state->trunk_scan_active_avoided = coord->targets[coord->active].avoided ? 1U : 0U;
    state->trunk_scan_avoided_count = (uint16_t)(avoided > UINT16_MAX ? UINT16_MAX : avoided);
}

// A user purge clears the live ledger; every parked target keeps its own copy,
// so scrub those as well or the next target switch restores entries the user
// just forgot. The key epoch is global and intentionally untouched.
static void
trunk_scan_clear_enc_lockout_snapshots(const dsd_state* state) {
    dsd_trunk_scan_coord* coord = trunk_scan_get(state);
    if (!coord) {
        return;
    }
    for (size_t i = 0; i < coord->count; i++) {
        DSD_MEMSET(coord->targets[i].snapshot.enc_lockout_entries, 0,
                   sizeof(coord->targets[i].snapshot.enc_lockout_entries));
    }
    DSD_MEMSET(coord->scratch_snapshot.enc_lockout_entries, 0, sizeof(coord->scratch_snapshot.enc_lockout_entries));
}

static const dsd_trunk_scan_coord*
trunk_scan_get_const(const dsd_state* state) {
    return (const dsd_trunk_scan_coord*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
}

static int
trunk_scan_target_is_p25(const dsd_trunk_scan_target* target) {
    return target && target->type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK;
}

static int
trunk_scan_p25_sm_mode_from_ctx(const p25_sm_ctx_t* ctx) {
    switch (p25_sm_get_state(ctx)) {
        case P25_SM_ON_CC: return DSD_P25_SM_MODE_ON_CC;
        case P25_SM_TUNED:
            if (ctx->slots[0].voice_active || ctx->slots[1].voice_active) {
                return DSD_P25_SM_MODE_FOLLOW;
            }
            return ctx->vc_activity_seen ? DSD_P25_SM_MODE_HANG : DSD_P25_SM_MODE_ARMED;
        case P25_SM_HUNTING: return DSD_P25_SM_MODE_HUNTING;
        case P25_SM_IDLE: break;
    }
    return DSD_P25_SM_MODE_UNKNOWN;
}

static void
trunk_scan_sync_active_sm_mode(dsd_state* state, const dsd_trunk_scan_target_runtime* rt) {
    if (!state || !rt) {
        return;
    }
    state->p25_sm_mode =
        trunk_scan_target_is_p25(&rt->target) ? trunk_scan_p25_sm_mode_from_ctx(&rt->p25_ctx) : DSD_P25_SM_MODE_UNKNOWN;
}

static int
trunk_scan_is_iq_replay(const dsd_opts* opts) {
    return opts && (opts->iq_replay_requested != 0 || opts->iq_replay_active != 0);
}

static int
trunk_scan_has_open_rtl_stream(const dsd_opts* opts, const dsd_state* state) {
    return opts && state && opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx;
}

static int
trunk_scan_has_tuning_backend(const dsd_opts* opts, const dsd_state* state) {
    return opts && !trunk_scan_is_iq_replay(opts)
           && (opts->use_rigctl == 1 || trunk_scan_has_open_rtl_stream(opts, state));
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
trunk_scan_gfsk_sps(const dsd_opts* opts, const dsd_state* state, dsd_trunk_scan_target_type type) {
    (void)state;
    int demod_rate = trunk_scan_demod_rate(opts);
    return dsd_opts_compute_sps_rate(opts, trunk_scan_type_gfsk_symbol_rate(type), demod_rate);
}

/* The four-level GFSK family spans two symbol rates, and the SPS hunt profile is the only thing
 * that tells the NXDN sync search which one to look for (dsd_frame_sync_active_nxdn_variant()
 * reads it back to label the variant when -fa enables both). */
static int
trunk_scan_gfsk_sps_hunt_profile(int symbol_rate_hz) {
    return symbol_rate_hz == 2400 ? DSD_FRAME_SYNC_SPS_PROFILE_2400_4 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
}

static void
trunk_scan_apply_p25_target_demod(const dsd_opts* opts, dsd_state* state, const dsd_trunk_scan_target* target) {
    state->sps_hunt_idx =
        state->p25_cc_is_tdma == 1 ? DSD_FRAME_SYNC_SPS_PROFILE_6000_4 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->sps_hunt_counter = 0;
    int p25_sps = trunk_scan_p25_cc_sps(opts, state);
    state->samplesPerSymbol = p25_sps;
    state->symbolCenter = dsd_opts_symbol_center(p25_sps);
    if (target->modulation == DSD_TRUNK_SCAN_MODULATION_CQPSK) {
        state->rf_mod = 1;
    } else if (target->modulation == DSD_TRUNK_SCAN_MODULATION_C4FM) {
        state->rf_mod = 0;
    } else if (target->modulation == DSD_TRUNK_SCAN_MODULATION_AUTO || !opts->mod_cli_lock) {
        state->rf_mod = (state->p25_cc_is_tdma == 1 || state->p25_p1_validated_rf_mod == 1) ? 1 : 0;
    }
}

static void
trunk_scan_apply_gfsk_class_target_demod(const dsd_opts* opts, dsd_state* state, const dsd_trunk_scan_target* target) {
    state->sps_hunt_idx = trunk_scan_gfsk_sps_hunt_profile(trunk_scan_type_gfsk_symbol_rate(target->type));
    state->sps_hunt_counter = 0;
    int gfsk_sps = trunk_scan_gfsk_sps(opts, state, target->type);
    state->samplesPerSymbol = gfsk_sps;
    state->symbolCenter = dsd_opts_symbol_center(gfsk_sps);
    if (target->modulation == DSD_TRUNK_SCAN_MODULATION_GFSK || target->modulation == DSD_TRUNK_SCAN_MODULATION_AUTO
        || !opts->mod_cli_lock) {
        state->rf_mod = 2;
    }
}

static void
trunk_scan_apply_target_demod(const dsd_opts* opts, dsd_state* state, const dsd_trunk_scan_target* target) {
    if (!opts || !state || !target) {
        return;
    }
    if (trunk_scan_target_is_p25(target)) {
        trunk_scan_apply_p25_target_demod(opts, state, target);
        return;
    }
    if (trunk_scan_type_is_gfsk_family(target->type)) {
        trunk_scan_apply_gfsk_class_target_demod(opts, state, target);
    }
}

static void
trunk_scan_restore_saved_mod_gain_opts(dsd_opts* opts, const dsd_trunk_scan_coord* coord) {
    if (!opts || !coord) {
        return;
    }
    opts->mod_c4fm = coord->saved_mod_c4fm;
    opts->mod_qpsk = coord->saved_mod_qpsk;
    opts->mod_gfsk = coord->saved_mod_gfsk;
    opts->mod_p25p2_c4fm = coord->saved_mod_p25p2_c4fm;
    opts->mod_p25p2_profile_lock = coord->saved_mod_p25p2_profile_lock;
    opts->mod_cli_lock = coord->saved_mod_cli_lock;
    opts->rtl_gain_value = coord->saved_rtl_gain_value;
}

static void
trunk_scan_apply_target_mod_opts(dsd_opts* opts, const dsd_trunk_scan_target* target) {
    if (!opts || !target || target->modulation == DSD_TRUNK_SCAN_MODULATION_UNSET) {
        return;
    }
    opts->mod_p25p2_c4fm = 0;
    opts->mod_p25p2_profile_lock = 0;
    switch (target->modulation) {
        case DSD_TRUNK_SCAN_MODULATION_AUTO:
            if (target->type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
                opts->mod_c4fm = 1;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 0;
            } else {
                opts->mod_c4fm = 0;
                opts->mod_qpsk = 0;
                opts->mod_gfsk = 1;
            }
            opts->mod_cli_lock = 0;
            break;
        case DSD_TRUNK_SCAN_MODULATION_C4FM:
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            opts->mod_cli_lock = 1;
            break;
        case DSD_TRUNK_SCAN_MODULATION_CQPSK:
            opts->mod_c4fm = 0;
            opts->mod_qpsk = 1;
            opts->mod_gfsk = 0;
            opts->mod_cli_lock = 1;
            break;
        case DSD_TRUNK_SCAN_MODULATION_GFSK:
            opts->mod_c4fm = 0;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 1;
            opts->mod_cli_lock = 1;
            break;
        case DSD_TRUNK_SCAN_MODULATION_UNSET: break;
    }
}

static void
trunk_scan_apply_target_gain_opts(dsd_opts* opts, const dsd_trunk_scan_target* target) {
    if (!opts || !target || !target->rtl_gain_is_set) {
        return;
    }
    opts->rtl_gain_value = target->rtl_gain_db;
}

static void
trunk_scan_apply_target_opts(dsd_opts* opts, const dsd_trunk_scan_coord* coord, const dsd_trunk_scan_target* target) {
    if (!opts || !coord || !target) {
        return;
    }
    trunk_scan_restore_saved_mod_gain_opts(opts, coord);
    trunk_scan_apply_target_mod_opts(opts, target);
    trunk_scan_apply_target_gain_opts(opts, target);
    opts->trunk_is_tuned = 0;
    switch (target->type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK: opts->trunk_enable = 1; break;
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL: opts->trunk_enable = 0; break;
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

    if (trunk_scan_type_anchors_p25_cc_freq(target->type)) {
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
    /* A chan_csv `name` column is accepted but never shown under trunk scan: the per-target
     * snapshot carries the positional scan list and not the names, so a kept name would end
     * up over the next target's list. Drop them before that can happen. */
    dsd_state_trunk_lcn_name_free(state);
    /* Per-row key columns are likewise discarded: keys arrive per trunk-scan target, not
     * per chan_csv row, and a kept set would install on the wrong target's hop. */
    dsd_state_trunk_lcn_keys_free(state);
    free(tmp_opts);
    if (import_rc != 0) {
        scan_set_error(err, err_sz, "failed to import chan_csv '%s' for trunk scan target '%s'", target->chan_csv,
                       target->id);
        return -1;
    }
    return 0;
}

/* The importer writes the store and seeds the live tables in place, and the build loop snapshots
 * both right after, so each target's plan travels with its snapshot and never reaches a peer. */
static int
trunk_scan_import_target_p25_bandplan(dsd_state* state, const dsd_trunk_scan_target* target, char* err, size_t err_sz) {
    if (!target->p25_bandplan_csv[0]) {
        return 0;
    }
    if (csvP25BandplanImportPath(target->p25_bandplan_csv, state) != 0) {
        scan_set_error(err, err_sz, "failed to import p25_bandplan_csv '%s' for trunk scan target '%s'",
                       target->p25_bandplan_csv, target->id);
        return -1;
    }
    return 0;
}

static int
trunk_scan_build_target_runtime(dsd_trunk_scan_coord* coord, dsd_opts* opts, dsd_state* state,
                                const dsd_trunk_scan_target_list* list, char* err, size_t err_sz) {
    dsd_trunk_scan_snapshot* empty_snapshot = &coord->scratch_snapshot;
    trunk_scan_seed_empty_snapshot(empty_snapshot, opts, state);
    double now_m = trunk_scan_now_m();

    /* coord->count is what coord->targets was sized to, so it -- not the caller's list --
     * bounds the fill. */
    const size_t build_count = list->count < coord->count ? list->count : coord->count;
    for (size_t i = 0; i < build_count; i++) {
        dsd_trunk_scan_target_runtime* rt = &coord->targets[i];
        rt->target = list->targets[i];
        trunk_scan_restore_snapshot(state, empty_snapshot);
        trunk_scan_apply_target_opts(opts, coord, &rt->target);
        trunk_scan_apply_target_demod(opts, state, &rt->target);
        trunk_scan_seed_target_state(state, &rt->target, now_m);
        if (trunk_scan_import_target_chan_csv(opts, state, &rt->target, err, err_sz) != 0) {
            return -1;
        }
        if (trunk_scan_import_target_p25_bandplan(state, &rt->target, err, err_sz) != 0) {
            return -1;
        }
        if (rt->target.keys_hex_csv[0] != '\0' || rt->target.keys_dec_csv[0] != '\0') {
            const char* key_src =
                rt->target.keys_hex_csv[0] != '\0' ? rt->target.keys_hex_csv : rt->target.keys_dec_csv;
            if (dsd_key_set_load_csv(&rt->keys, rt->target.keys_hex_csv[0] != '\0' ? rt->target.keys_hex_csv : NULL,
                                     rt->target.keys_dec_csv[0] != '\0' ? rt->target.keys_dec_csv : NULL,
                                     opts->show_keys)
                != 0) {
                scan_set_error(err, err_sz, "failed to import keys for trunk scan target '%s' from '%s'", rt->target.id,
                               key_src);
                return -1;
            }
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
    if (!trunk_scan_type_is_conventional(target->type)) {
        return trunk_scan_dmr_retune_freq(state, target);
    }
    return (long int)target->frequency_hz;
}

static dsd_trunk_tune_result
trunk_scan_retune_active(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_target_runtime* rt,
                         uint64_t* out_request_id) {
    if (out_request_id) {
        *out_request_id = 0U;
    }
    const long int freq = trunk_scan_retune_freq(state, &rt->target);
    if (trunk_scan_type_is_conventional(rt->target.type)) {
        return dsd_engine_scan_tune_to_freq(opts, state, freq, trunk_scan_gfsk_sps(opts, state, rt->target.type),
                                            out_request_id);
    }
    /* Trunk targets re-park on their control channel; only the two axes differ per type. */
    state->p25_cc_freq = trunk_scan_type_anchors_p25_cc_freq(rt->target.type) ? freq : 0;
    state->trunk_cc_freq = freq;
    const int cc_sps = trunk_scan_target_is_p25(&rt->target) ? trunk_scan_p25_cc_sps(opts, state)
                                                             : trunk_scan_gfsk_sps(opts, state, rt->target.type);
    return dsd_trunk_tuning_hook_tune_to_cc(opts, state, freq, cc_sps, out_request_id);
}

/*
 * Per-target keys ride the scan key swap: a keyed target installs its set, an
 * unkeyed one hands the foreground keyring back to the globals. Trunk scan
 * never bumps the lockout key epoch -- every target carries its own lockout
 * ledger snapshot, so no global invalidation is owed on a switch.
 */
static void
trunk_scan_apply_target_keys(dsd_state* state, const dsd_trunk_scan_target_runtime* rt) {
    if (!state || !rt) {
        return;
    }
    if (rt->keys.present) {
        (void)dsd_scan_keys_enter(state, &rt->keys);
    } else {
        dsd_scan_keys_leave(state);
    }
}

/*
 * Peer IDEN sharing (#402). Two scan targets that are sites of one P25 system (same WACN/SYS)
 * announce the same IDEN_UP tables, so an identifier one target has already learned is a valid
 * seed for the other. Copy only into empty live slots, only from a parked peer's snapshot entry
 * that carries this system's identity, and only at trust 1: the over-the-air announcement on
 * this site confirms it, and an entry from a different system (or with no provenance at all)
 * never crosses. Everything else in the snapshots stays isolated per target.
 */
static int
trunk_scan_share_peer_iden_slot(const dsd_trunk_scan_coord* coord, size_t self, unsigned long long wacn,
                                unsigned long long sysid, int iden, int is_tdma, p25_iden_entry_t* live) {
    for (size_t j = 0; j < coord->count; j++) {
        if (j == self) {
            continue;
        }
        const dsd_trunk_scan_snapshot* peer = &coord->targets[j].snapshot;
        const p25_iden_entry_t* src = is_tdma ? &peer->p25_iden_tdma[iden] : &peer->p25_iden_fdma[iden];
        if (!src->populated || src->base_freq == 0 || src->chan_spac == 0) {
            continue;
        }
        if ((src->wacn == 0ULL && src->sysid == 0ULL) || src->wacn != wacn || src->sysid != sysid) {
            continue;
        }
        *live = *src;
        live->trust = 1;
        live->populated = 1;
        return 1;
    }
    return 0;
}

static void
trunk_scan_share_peer_idens(const dsd_trunk_scan_coord* coord, dsd_state* state, dsd_trunk_scan_target_runtime* rt) {
    if (!coord || !state || !rt) {
        return;
    }
    rt->iden_share_wacn = state->p2_wacn;
    rt->iden_share_sysid = state->p2_sysid;
    if (!trunk_scan_target_is_p25(&rt->target) || coord->count < 2
        || (state->p2_wacn == 0ULL && state->p2_sysid == 0ULL)) {
        return;
    }
    const size_t self = (size_t)(rt - coord->targets);
    int shared = 0;
    for (int iden = 0; iden < 16; iden++) {
        if (!state->p25_iden_fdma[iden].populated
            && trunk_scan_share_peer_iden_slot(coord, self, state->p2_wacn, state->p2_sysid, iden, 0,
                                               &state->p25_iden_fdma[iden])) {
            state->p25_chan_tdma_explicit[iden] |= 0x01;
            shared++;
        }
        if (!state->p25_iden_tdma[iden].populated
            && trunk_scan_share_peer_iden_slot(coord, self, state->p2_wacn, state->p2_sysid, iden, 1,
                                               &state->p25_iden_tdma[iden])) {
            state->p25_chan_tdma_explicit[iden] |= 0x02;
            shared++;
        }
    }
    if (shared > 0) {
        LOG_INFO("NOTICE: Trunk scan target '%s' seeded %d P25 identifier(s) from peer targets on WACN %05llX SYS "
                 "%03llX\n",
                 rt->target.id, shared, state->p2_wacn, state->p2_sysid);
    }
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
    /* Before the retune, not after: a retune that fails still leaves the receiver parked
     * here until the caller decides otherwise, and the label has to say so. */
    trunk_scan_publish_active_target(state, coord);
    dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    trunk_scan_restore_target_snapshot(coord, state, rt);
    trunk_scan_share_peer_idens(coord, state, rt);
    trunk_scan_apply_target_opts(opts, coord, &rt->target);
    trunk_scan_apply_target_keys(state, rt);
    trunk_scan_apply_target_demod(opts, state, &rt->target);
    trunk_scan_sync_active_sm_mode(state, rt);

    double now_m = trunk_scan_now_m();
    rt->parked_since_m = now_m;
    rt->idle_since_m = now_m;
    uint64_t tune_request_id = 0U;
    dsd_trunk_tune_result tune_result = trunk_scan_retune_active(opts, state, rt, &tune_request_id);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        rt->retry_until_m = now_m + 2.0;
        LOG_WARN("WARNING: Trunk scan target '%s' retune failed; cooling down briefly\n", rt->target.id);
        return -1;
    }

    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        if (tune_result == DSD_TRUNK_TUNE_RESULT_PENDING && tune_request_id != 0U) {
            (void)p25_sm_await_pending_cc_tune(&rt->p25_ctx, opts, state, tune_request_id, "scan-retune");
            /* The P25 SM can observe completion before the scan coordinator's
             * next tick. Track the same request here so dwell still restarts. */
            rt->tune_request_id = tune_request_id;
            rt->tune_pending = 1;
        } else {
            rt->tune_request_id = 0U;
            rt->tune_pending = 0;
            double completed_m = 0.0;
            (void)dsd_trunk_tuning_request_status(tune_request_id, &completed_m);
            if (completed_m <= 0.0) {
                completed_m = trunk_scan_now_m();
            }
            (void)p25_sm_restart_pending_cc_acquisition(&rt->p25_ctx, opts, state, completed_m, "scan-retune");
        }
    } else if (tune_result == DSD_TRUNK_TUNE_RESULT_PENDING && tune_request_id != 0U) {
        rt->tune_request_id = tune_request_id;
        rt->tune_pending = 1;
    } else {
        rt->tune_request_id = 0U;
        rt->tune_pending = 0;
    }
    rt->retry_until_m = 0.0;
    LOG_INFO("NOTICE: Trunk scan target '%s' at %ld Hz\n", rt->target.id, trunk_scan_retune_freq(state, &rt->target));
    return 0;
}

static void
trunk_scan_advance(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord) {
    if (!coord || coord->count < 2) {
        return;
    }
    double now_m = trunk_scan_now_m();
    dsd_trunk_scan_snapshot* original_snapshot = &coord->scratch_snapshot;
    trunk_scan_save_snapshot(state, original_snapshot);
    size_t original_active = coord->active;
    int save_current = 1;
    int attempted_alternate_retune = 0;
    int tried = 0;

    for (size_t attempts = 0; attempts < coord->count; attempts++) {
        size_t next = (original_active + 1U + attempts) % coord->count;
        if (next == original_active && !attempted_alternate_retune) {
            continue;
        }
        if (coord->targets[next].retry_until_m > now_m) {
            continue;
        }
        /* An operator avoid is for the session, not a cooldown. The original is not
         * exempt: when every alternate fails the receiver stays on it and the
         * publication says so. */
        if (coord->targets[next].avoided && next != original_active) {
            continue;
        }
        tried = 1;
        if (trunk_scan_switch_to(opts, state, coord, next, save_current) == 0) {
            return;
        }
        if (next != original_active) {
            attempted_alternate_retune = 1;
        }
        save_current = 0;
    }

    coord->active = original_active;
    if (!tried) {
        /* Nothing was eligible (every alternate avoided or cooling down): the receiver
         * never left, so there is nothing to restore. Re-arm the dwell rather than
         * re-running this walk on every tick until something changes. */
        coord->targets[original_active].idle_since_m = now_m;
        trunk_scan_publish_active_target(state, coord);
        return;
    }
    /* The loop above published every target it tried, and the last one it tried is not
     * necessarily this one -- the original is skipped when its own retry cooldown is still
     * running. Republish, or the label names a target the receiver never reached. */
    trunk_scan_publish_active_target(state, coord);
    trunk_scan_restore_snapshot(state, original_snapshot);
    trunk_scan_apply_target_opts(opts, coord, &coord->targets[coord->active].target);
    trunk_scan_apply_target_keys(state, &coord->targets[coord->active]);
    trunk_scan_apply_target_demod(opts, state, &coord->targets[coord->active].target);
    trunk_scan_sync_active_sm_mode(state, &coord->targets[coord->active]);
}

/* A single-target list has nowhere else to go, and a held target is not allowed to go
 * anywhere: both retry the parked target in place once its cooldown ends. */
static void
trunk_scan_retry_active_if_due(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord, double now_m) {
    if (!coord || coord->active >= coord->count) {
        return;
    }
    if (coord->count != 1 && !coord->hold_active) {
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
    if (rt->tune_pending) {
        return 1;
    }
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        return (rt->p25_ctx.cc_tune_pending || opts->trunk_is_tuned == 1
                || p25_sm_get_state(&rt->p25_ctx) == P25_SM_TUNED);
    }
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_DMR_TRUNK) {
        return (opts->trunk_is_tuned == 1 || dmr_sm_get_state(&rt->dmr_ctx) == DMR_SM_TUNED);
    }
    if (rt->target.type == DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK) {
        return opts->trunk_is_tuned == 1;
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
    trunk_scan_restore_saved_mod_gain_opts(opts, coord);
    opts->trunk_enable = coord->saved_trunk_enable;
    opts->trunk_is_tuned = coord->saved_trunk_is_tuned;
}

static void
trunk_scan_capture_saved_opts(dsd_trunk_scan_coord* coord, const dsd_opts* opts) {
    if (!coord || !opts) {
        return;
    }
    coord->saved_trunk_enable = opts->trunk_enable;
    coord->saved_trunk_is_tuned = opts->trunk_is_tuned;
    coord->saved_mod_c4fm = opts->mod_c4fm;
    coord->saved_mod_qpsk = opts->mod_qpsk;
    coord->saved_mod_gfsk = opts->mod_gfsk;
    coord->saved_mod_p25p2_c4fm = opts->mod_p25p2_c4fm;
    coord->saved_mod_p25p2_profile_lock = opts->mod_p25p2_profile_lock;
    coord->saved_mod_cli_lock = opts->mod_cli_lock;
    coord->saved_rtl_gain_value = opts->rtl_gain_value;
#ifdef USE_RADIO
    coord->saved_tuner_autogain_on = rtl_stream_get_tuner_autogain() ? 1 : 0;
    coord->saved_tuner_autogain_is_set = 1;
#endif
}

static void
trunk_scan_warn_ignored_target_gain(const dsd_opts* opts, const dsd_state* state,
                                    const dsd_trunk_scan_target_list* list) {
    if (!opts || !state || trunk_scan_has_open_rtl_stream(opts, state)
        || !scan_target_list_has_rtl_gain_override(list)) {
        return;
    }
    LOG_WARN("WARNING: Trunk scan rtl_gain target overrides require RTL-family input; ignoring target gain settings\n");
}

typedef enum {
    TRUNK_SCAN_DECODER_P25 = 0,
    TRUNK_SCAN_DECODER_DMR = 1,
    TRUNK_SCAN_DECODER_NXDN96 = 2,
    TRUNK_SCAN_DECODER_NXDN48 = 3,
    TRUNK_SCAN_DECODER_COUNT = 4,
} trunk_scan_decoder_class;

/* Exhaustive switch with no default: adding a target type must fail the build here rather than
 * silently classing the new type as "always decodable". */
static trunk_scan_decoder_class
trunk_scan_target_decoder_class(dsd_trunk_scan_target_type type) {
    switch (type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK: return TRUNK_SCAN_DECODER_P25;
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL: return TRUNK_SCAN_DECODER_DMR;
        case DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL: return TRUNK_SCAN_DECODER_NXDN96;
        case DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL: return TRUNK_SCAN_DECODER_NXDN48;
    }
    return TRUNK_SCAN_DECODER_P25;
}

static void
trunk_scan_warn_disabled_target_decoders(const dsd_opts* opts, const dsd_trunk_scan_target_list* list) {
    if (!opts || !list) {
        return;
    }

    static const struct {
        const char* name;
        const char* hint;
    } k_decoders[TRUNK_SCAN_DECODER_COUNT] = {
        {"P25", "-ft, -f1, -f2, or -fa"},
        {"DMR", "-fs, -ft, or -fa"},
        {"NXDN96", "-fn or -fa"},
        {"NXDN48", "-fi or -fa"},
    };

    /* P25 mirrors the engine's own no_carrier_p25_frames_enabled(): a TDMA control channel is
     * followed with -f2 alone, which leaves frame_p25p1 clear. The two NXDN variants are separate
     * gates because -fn and -fi each enable only one of them; -fa enables both. */
    const int enabled[TRUNK_SCAN_DECODER_COUNT] = {
        (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1),
        (opts->frame_dmr == 1),
        (opts->frame_nxdn96 == 1),
        (opts->frame_nxdn48 == 1),
    };

    /* One line per decoder class, not per target: scan lists are unbounded. */
    size_t missing[TRUNK_SCAN_DECODER_COUNT] = {0};
    const char* first_id[TRUNK_SCAN_DECODER_COUNT] = {0};
    for (size_t i = 0; i < list->count; i++) {
        const trunk_scan_decoder_class cls = trunk_scan_target_decoder_class(list->targets[i].type);
        if (enabled[cls]) {
            continue;
        }
        if (missing[cls]++ == 0) {
            first_id[cls] = list->targets[i].id;
        }
    }
    for (int cls = 0; cls < (int)TRUNK_SCAN_DECODER_COUNT; cls++) {
        if (missing[cls] == 0) {
            continue;
        }
        LOG_WARN("WARNING: %zu trunk scan target(s) have no enabled %s decoder (first: '%s'); use %s to decode them\n",
                 missing[cls], k_decoders[cls].name, first_id[cls], k_decoders[cls].hint);
    }
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

/* Return 1 when the P25 SM already recovered, 0 while it still owns recovery,
 * and -1 when no P25 recovery superseded the failed coordinator request. */
static int
trunk_scan_reconcile_p25_retune_recovery(dsd_trunk_scan_target_runtime* rt, uint64_t failed_request_id) {
    if (!rt || rt->target.type != DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        return -1;
    }

    if (rt->p25_ctx.cc_tune_pending) {
        const uint64_t sm_request_id = rt->p25_ctx.cc_tune_request_id;
        if (sm_request_id == failed_request_id) {
            /* The coordinator observed the backend result first. Leave the
             * request with the P25 SM so its next tick owns recovery. */
            return 0;
        }
        if (sm_request_id > failed_request_id) {
            rt->tune_request_id = sm_request_id;
            rt->tune_pending = 1;
            LOG_INFO("Trunk scan target '%s' adopted P25 recovery retune request %llu\n", rt->target.id,
                     (unsigned long long)sm_request_id);
            return 0;
        }
    }

    const p25_sm_state_e sm_state = p25_sm_get_state(&rt->p25_ctx);
    if (sm_state == P25_SM_ON_CC || sm_state == P25_SM_TUNED) {
        /* A replacement tune completed synchronously (or was decoded and
         * followed) before the coordinator revisited the failed request. */
        rt->tune_request_id = 0U;
        rt->tune_pending = 0;
        rt->retry_until_m = 0.0;
        rt->idle_since_m = -1.0;
        return 1;
    }
    return -1;
}

static int
trunk_scan_resolve_pending_retune(dsd_state* state, dsd_trunk_scan_target_runtime* rt, double now_m) {
    if (!rt || !rt->tune_pending) {
        return 1;
    }
    const uint64_t request_id = rt->tune_request_id;
    dsd_trunk_tune_result result = dsd_trunk_tuning_request_status(request_id, NULL);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return 0;
    }

    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        rt->tune_request_id = 0U;
        rt->tune_pending = 0;
        rt->idle_since_m = -1.0;
        return 1;
    }

    const int p25_recovery = trunk_scan_reconcile_p25_retune_recovery(rt, request_id);
    if (p25_recovery >= 0) {
        return p25_recovery;
    }

    rt->tune_request_id = 0U;
    rt->tune_pending = 0;
    rt->retry_until_m = now_m + 2.0;
    LOG_WARN("WARNING: Trunk scan target '%s' asynchronous retune failed; cooling down briefly\n", rt->target.id);
    (void)state;
    return -1;
}

static void
trunk_scan_tick_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord) {
    double now_m = trunk_scan_now_m();
    dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    int pending_status = trunk_scan_resolve_pending_retune(state, rt, now_m);
    if (pending_status == 0) {
        return;
    }
    if (pending_status < 0) {
        if (coord->count > 1 && !coord->hold_active) {
            trunk_scan_advance(opts, state, coord);
        }
        return;
    }
    trunk_scan_retry_active_if_due(opts, state, coord, now_m);
    rt = &coord->targets[coord->active];
    trunk_scan_tick_active_target_sm(opts, state, rt);
    if (state->p2_wacn != rt->iden_share_wacn || state->p2_sysid != rt->iden_share_sysid) {
        trunk_scan_share_peer_idens(coord, state, rt);
    }
    if (trunk_scan_active_is_held(opts, coord)) {
        rt->idle_since_m = -1.0;
        return;
    }
    /* The operator hold only suspends the idle dwell: the state machine above keeps
     * following calls on the held system. Disarming the dwell here means a release
     * re-arms it on the next tick and the target gets a full dwell before rotation. */
    if (coord->hold_active) {
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

static size_t
trunk_scan_usable_count(const dsd_trunk_scan_coord* coord) {
    return coord->count - trunk_scan_avoided_count(coord);
}

/* Advance under the same guard the tick takes: trunk_scan_switch_to() drives the P25 SM.
 * Returns 0 when the receiver moved, 1 when it stayed. */
static int
trunk_scan_control_advance_locked(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord) {
    const size_t original = coord->active;
    trunk_scan_advance(opts, state, coord);
    return coord->active != original ? 0 : 1;
}

static int
trunk_scan_control_hold_toggle(dsd_state* state, dsd_trunk_scan_coord* coord) {
    coord->hold_active = coord->hold_active ? 0 : 1;
    trunk_scan_publish_active_target(state, coord);
    return coord->hold_active;
}

static int
trunk_scan_control_avoid_clear(dsd_state* state, dsd_trunk_scan_coord* coord) {
    const size_t cleared = trunk_scan_avoided_count(coord);
    for (size_t i = 0; i < coord->count; i++) {
        coord->targets[i].avoided = 0;
    }
    trunk_scan_publish_active_target(state, coord);
    return cleared > INT_MAX ? INT_MAX : (int)cleared;
}

static int
trunk_scan_control_avoid_active(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord) {
    if (trunk_scan_usable_count(coord) <= 1) {
        return DSD_TRUNK_SCAN_CONTROL_REFUSED;
    }
    if (!p25_sm_tick_guard_try_enter()) {
        return DSD_TRUNK_SCAN_CONTROL_BUSY;
    }
    coord->targets[coord->active].avoided = 1;
    const int rc = trunk_scan_control_advance_locked(opts, state, coord);
    trunk_scan_publish_active_target(state, coord);
    p25_sm_tick_guard_leave();
    return rc;
}

static int
trunk_scan_control_advance(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord) {
    if (coord->count < 2) {
        return DSD_TRUNK_SCAN_CONTROL_REFUSED;
    }
    if (!p25_sm_tick_guard_try_enter()) {
        return DSD_TRUNK_SCAN_CONTROL_BUSY;
    }
    const int rc = trunk_scan_control_advance_locked(opts, state, coord);
    p25_sm_tick_guard_leave();
    return rc;
}

int
dsd_engine_trunk_scan_control(dsd_opts* opts, dsd_state* state, int op) {
    dsd_trunk_scan_coord* coord = trunk_scan_get(state);
    if (!opts || !state || !coord || coord->count == 0 || coord->active >= coord->count) {
        return DSD_TRUNK_SCAN_CONTROL_UNAVAILABLE;
    }
    switch (op) {
        case DSD_TRUNK_SCAN_CONTROL_HOLD_TOGGLE: return trunk_scan_control_hold_toggle(state, coord);
        case DSD_TRUNK_SCAN_CONTROL_AVOID_CLEAR: return trunk_scan_control_avoid_clear(state, coord);
        case DSD_TRUNK_SCAN_CONTROL_AVOID_ACTIVE: return trunk_scan_control_avoid_active(opts, state, coord);
        case DSD_TRUNK_SCAN_CONTROL_ADVANCE: return trunk_scan_control_advance(opts, state, coord);
        default: return DSD_TRUNK_SCAN_CONTROL_REFUSED;
    }
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

const char*
dsd_engine_trunk_scan_active_chan_csv(const dsd_state* state) {
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    if (!coord || coord->active >= coord->count) {
        return NULL;
    }
    const char* chan_csv = coord->targets[coord->active].target.chan_csv;
    return (chan_csv[0] != '\0') ? chan_csv : NULL;
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

typedef enum {
    TRUNK_SCAN_CONVENTIONAL_FAMILY_DMR = 0,
    TRUNK_SCAN_CONVENTIONAL_FAMILY_NXDN = 1,
} trunk_scan_conventional_family;

/* Which conventional family a target belongs to, i.e. which protocol's activity reports may claim
 * its park. Exhaustive switch with no default: the NXDN protocol hooks cannot tell NXDN48 from
 * NXDN96 -- the sync word and every element are identical, only the symbol rate differs -- so both
 * NXDN conventional types answer to the same entry point, and a new type must be classified here
 * rather than silently belonging to no family. */
static int
trunk_scan_type_in_conventional_family(dsd_trunk_scan_target_type type, trunk_scan_conventional_family family) {
    switch (type) {
        case DSD_TRUNK_SCAN_TARGET_P25_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_DMR_TRUNK:
        case DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK: return 0;
        case DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL: return family == TRUNK_SCAN_CONVENTIONAL_FAMILY_DMR;
        case DSD_TRUNK_SCAN_TARGET_NXDN_CONVENTIONAL:
        case DSD_TRUNK_SCAN_TARGET_NXDN48_CONVENTIONAL: return family == TRUNK_SCAN_CONVENTIONAL_FAMILY_NXDN;
    }
    return 0;
}

/* Shared body for every conventional target type: the hold decision is protocol-independent,
 * so the per-protocol entry points below differ only in which family may claim the hold. */
static void
trunk_scan_conventional_activity(trunk_scan_conventional_family family, const dsd_opts* opts, const dsd_state* state,
                                 uint32_t target, uint32_t source, int is_private, int encrypted, int data_call) {
    dsd_trunk_scan_coord* coord = trunk_scan_get(state);
    if (!opts || !state || !coord || coord->count == 0) {
        return;
    }
    dsd_trunk_scan_target_runtime* rt = &coord->targets[coord->active];
    if (!trunk_scan_type_in_conventional_family(rt->target.type, family)) {
        return;
    }

    dsd_tg_policy_decision decision;
    int rc = 0;
    if (is_private) {
        rc = dsd_tg_policy_evaluate_private_call(opts, state, source, target, encrypted, data_call, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, target, source, encrypted, data_call, &decision);
    }
    if (rc == 0 && decision.tune_allowed) {
        rt->last_allowed_activity_m = trunk_scan_now_m();
        rt->idle_since_m = -1.0;
    }
}

void
dsd_engine_trunk_scan_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                uint32_t source, int is_private, int encrypted, int data_call) {
    trunk_scan_conventional_activity(TRUNK_SCAN_CONVENTIONAL_FAMILY_DMR, opts, state, target, source, is_private,
                                     encrypted, data_call);
}

void
dsd_engine_trunk_scan_nxdn_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                 uint32_t source, int is_private, int encrypted, int data_call) {
    trunk_scan_conventional_activity(TRUNK_SCAN_CONVENTIONAL_FAMILY_NXDN, opts, state, target, source, is_private,
                                     encrypted, data_call);
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
trunk_scan_coord_free(dsd_trunk_scan_coord* coord) {
    if (!coord) {
        return;
    }
    for (size_t i = 0; i < coord->count; i++) {
        free(coord->targets[i].snapshot.trunk_lcn_freq_ext);
        free(coord->targets[i].snapshot.chan_map_chan);
        free(coord->targets[i].snapshot.chan_map_freq);
        dsd_key_set_free(&coord->targets[i].keys);
    }
    free(coord->scratch_snapshot.trunk_lcn_freq_ext);
    free(coord->scratch_snapshot.chan_map_chan);
    free(coord->scratch_snapshot.chan_map_freq);
    free(coord->targets);
    free(coord);
}

static void
trunk_scan_free(void* ptr) {
    trunk_scan_uninstall_runtime_hooks((const dsd_trunk_scan_coord*)ptr);
    trunk_scan_coord_free((dsd_trunk_scan_coord*)ptr);
}

static void
trunk_scan_install_runtime_hooks(dsd_trunk_scan_coord* coord) {
    g_trunk_scan_coord = coord;
    dsd_trunk_scan_hooks hooks = {0};
    hooks.p25_ctx = dsd_engine_trunk_scan_active_p25_ctx;
    hooks.dmr_ctx = dsd_engine_trunk_scan_active_dmr_ctx;
    hooks.tick = dsd_engine_trunk_scan_tick;
    hooks.dmr_conventional_activity = dsd_engine_trunk_scan_dmr_conventional_activity;
    hooks.nxdn_conventional_activity = dsd_engine_trunk_scan_nxdn_conventional_activity;
    hooks.active_chan_csv = dsd_engine_trunk_scan_active_chan_csv;
    hooks.enc_lockout_clear_snapshots = trunk_scan_clear_enc_lockout_snapshots;
    hooks.control = dsd_engine_trunk_scan_control;
    dsd_trunk_scan_hooks_set(hooks);
}

static int
trunk_scan_init_validate(const dsd_opts* opts, const dsd_state* state, char* err, size_t err_sz) {
    if (opts->scanner_mode == 1) {
        scan_set_error(err, err_sz, "--trunk-scan cannot be combined with -Y scanner mode");
        return -1;
    }
    if (opts->chan_in_file[0] != '\0') {
        scan_set_error(err, err_sz, "--trunk-scan cannot use a global channel map; use per-target chan_csv values");
        return -1;
    }
    if (opts->p25_bandplan_in_file[0] != '\0') {
        scan_set_error(err, err_sz,
                       "--trunk-scan cannot use a global P25 band plan; use per-target p25_bandplan_csv values");
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
    if (!trunk_scan_has_tuning_backend(opts, state)) {
        scan_set_error(err, err_sz, "--trunk-scan requires an open RTL input or rigctl tuning");
        return -1;
    }
    return 0;
}

static dsd_trunk_scan_coord*
trunk_scan_coord_create(const dsd_trunk_scan_target_list* list, const dsd_opts* opts, char* err, size_t err_sz) {
    dsd_trunk_scan_coord* coord = (dsd_trunk_scan_coord*)calloc(1, sizeof(*coord));
    if (!coord) {
        scan_set_error(err, err_sz, "failed to allocate trunk scan coordinator");
        return NULL;
    }
    if (list->count > dsd_trunk_scan_max_targets()) {
        scan_set_error(err, err_sz, "%zu trunk scan targets exceed the %d MB target budget (%zu KB each; %zu maximum)",
                       list->count, DSD_TRUNK_SCAN_TARGET_MEMORY_BUDGET_BYTES / (1024 * 1024),
                       sizeof(dsd_trunk_scan_target_runtime) / 1024U, dsd_trunk_scan_max_targets());
        trunk_scan_coord_free(coord);
        return NULL;
    }
    /* count is what trunk_scan_coord_free() walks, so publish it only once the array
     * it indexes exists. */
    coord->targets = (dsd_trunk_scan_target_runtime*)calloc(list->count, sizeof *coord->targets);
    if (!coord->targets) {
        scan_set_error(err, err_sz, "failed to allocate trunk scan targets");
        trunk_scan_coord_free(coord);
        return NULL;
    }
    coord->count = list->count;
    trunk_scan_capture_saved_opts(coord, opts);
    return coord;
}

/* Undo a partially-attached init: restore the global opts the coordinator
 * captured, release the coordinator and its snapshots, and drop the target
 * list's owned storage. */
static void
trunk_scan_init_release(dsd_opts* opts, dsd_state* state, dsd_trunk_scan_coord* coord,
                        dsd_trunk_scan_target_list* list) {
    dsd_scan_keys_leave(state);
    trunk_scan_restore_saved_opts(opts, coord);
    trunk_scan_coord_free(coord);
    dsd_trunk_scan_target_list_reset(list);
}

int
dsd_engine_trunk_scan_init(dsd_opts* opts, dsd_state* state, char* err, size_t err_sz) {
    if (!opts || !state || !opts->trunk_scan_enabled) {
        return 0;
    }
    if (trunk_scan_init_validate(opts, state, err, err_sz) != 0) {
        return -1;
    }

    dsd_trunk_scan_target_list list;
    if (dsd_trunk_scan_load_targets_csv(opts->trunk_scan_targets_csv, opts, &list, err, err_sz) != 0) {
        return -1;
    }
    trunk_scan_warn_ignored_target_gain(opts, state, &list);
    trunk_scan_warn_disabled_target_decoders(opts, &list);

    dsd_trunk_scan_coord* coord = trunk_scan_coord_create(&list, opts, err, err_sz);
    if (!coord) {
        dsd_trunk_scan_target_list_reset(&list);
        return -1;
    }

    if (trunk_scan_build_target_runtime(coord, opts, state, &list, err, err_sz) != 0) {
        trunk_scan_init_release(opts, state, coord, &list);
        return -1;
    }

    if (dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN, coord, trunk_scan_free) != 0) {
        trunk_scan_init_release(opts, state, coord, &list);
        scan_set_error(err, err_sz, "failed to attach trunk scan coordinator");
        return -1;
    }
    dsd_trunk_scan_target_list_reset(&list);
    trunk_scan_install_runtime_hooks(coord);
    if (trunk_scan_switch_to(opts, state, coord, 0, 0) != 0 && coord->count > 1) {
        trunk_scan_advance(opts, state, coord);
    }
    LOG_INFO("NOTICE: Trunk scan enabled with %zu targets\n", coord->count);
    return 0;
}

// Each target carries its own missing-channel ledger and channel map, so the exit summary the
// engine logs once for a single system becomes one line per target with a channel map here.
static void
trunk_scan_log_nxdn_diag_summaries(dsd_trunk_scan_coord* coord, const dsd_state* state) {
    if (!coord || coord->count == 0) {
        return;
    }
    if (coord->active < coord->count) {
        trunk_scan_save_target_snapshot(coord, state, &coord->targets[coord->active]);
    }
    for (size_t i = 0; i < coord->count; i++) {
        const dsd_trunk_scan_target_runtime* rt = &coord->targets[i];
        if (rt->target.type != DSD_TRUNK_SCAN_TARGET_NXDN_TRUNK) {
            continue;
        }
        nxdn_trunk_diag_log_summary_for(rt->target.chan_csv, &rt->snapshot.nxdn_diag, trunk_scan_snapshot_chan_lookup,
                                        &rt->snapshot);
    }
}

void
dsd_engine_trunk_scan_shutdown(dsd_opts* opts, dsd_state* state) {
    dsd_trunk_scan_coord* coord = trunk_scan_get(state);
    if (!coord) {
        return;
    }
    trunk_scan_log_nxdn_diag_summaries(coord, state);
    dsd_scan_keys_leave(state);
    trunk_scan_restore_saved_opts(opts, coord);
    trunk_scan_uninstall_runtime_hooks(coord);
    trunk_scan_clear_published_target(state);
    (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN, NULL, NULL);
}

#ifdef DSD_TRUNK_SCAN_TEST_CLOCK
size_t
dsd_engine_trunk_scan_active_index(const dsd_state* state) {
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    return coord ? coord->active : (size_t)-1;
}
#endif

size_t
dsd_engine_trunk_scan_target_count(const dsd_state* state) {
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    return coord ? coord->count : 0;
}

int
dsd_engine_trunk_scan_append_p25_idens(const dsd_state* state, struct p25_bandplan_row* rows, int count, int cap) {
    const dsd_trunk_scan_coord* coord = state ? trunk_scan_get_const(state) : NULL;
    if (!coord || !rows) {
        return count;
    }
    for (size_t i = 0; i < coord->count; i++) {
        if (i == coord->active) {
            continue; /* live in state: the caller collects those */
        }
        const dsd_trunk_scan_snapshot* snapshot = &coord->targets[i].snapshot;
        count = dsd_p25_bandplan_append_tables(rows, count, cap, snapshot->p25_iden_fdma, snapshot->p25_iden_tdma);
    }
    return count;
}

int
dsd_engine_trunk_scan_active_gfsk_symbol_rate(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    if (!coord || coord->active >= coord->count) {
        return 0;
    }
    return trunk_scan_type_gfsk_symbol_rate(coord->targets[coord->active].target.type);
}

int
dsd_engine_trunk_scan_active_p25_cqpsk_request(const dsd_state* state, int* out_enable) {
    if (!state || !out_enable) {
        return 0;
    }
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    if (!coord || coord->active >= coord->count) {
        return 0;
    }
    const dsd_trunk_scan_target* target = &coord->targets[coord->active].target;
    if (target->type != DSD_TRUNK_SCAN_TARGET_P25_TRUNK) {
        return 0;
    }
    if (target->modulation == DSD_TRUNK_SCAN_MODULATION_C4FM) {
        *out_enable = 0;
        return 1;
    }
    if (target->modulation == DSD_TRUNK_SCAN_MODULATION_CQPSK) {
        *out_enable = 1;
        return 1;
    }
    if (target->modulation == DSD_TRUNK_SCAN_MODULATION_AUTO) {
        *out_enable = (state->p25_cc_is_tdma == 1 || state->p25_p1_validated_rf_mod == 1) ? 1 : 0;
        return 1;
    }
    return 0;
}

int
dsd_engine_trunk_scan_saved_tuner_autogain(const dsd_state* state, int* out_on) {
    const dsd_trunk_scan_coord* coord = trunk_scan_get_const(state);
    if (out_on) {
        *out_on = 0;
    }
    if (!coord || !coord->saved_tuner_autogain_is_set) {
        return 0;
    }
    if (out_on) {
        *out_on = coord->saved_tuner_autogain_on ? 1 : 0;
    }
    return 1;
}
