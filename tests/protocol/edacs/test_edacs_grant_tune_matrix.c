// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// or invalid-value negative vectors to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * EDACS/ProVoice grant tune matrix.
 *
 * Drives the private canonical valid-frame dispatcher with already-decoded
 * 28-bit message words. Grant cases are
 * intentionally digital so the shared tune path is exercised without entering
 * the analog audio loop, except the analog-call cases (issue #625), which play
 * an analog call on a fake RTL stream and check when its squelch ends it.
 */

#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/dsp/rate_converter.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/protocol/edacs/edacs.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdlib.h>
#endif
#include <math.h>
#include <sndfile.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "edacs_internal.h"
#include "test_support.h"

typedef struct {
    const char* name;
    unsigned long long int msg_1;
    unsigned long long int msg_2;
    long freq_hz;
    int ea_mode;
    int lcn;
    int expected_flags;
    int expected_lasttg;
    int expected_lastsrc;
} edacs_grant_case;

typedef enum {
    EDACS_GUARD_GROUP_DISABLED = 0,
    EDACS_GUARD_PRIVATE_DISABLED,
    EDACS_GUARD_ALLOWLIST_BLOCK,
    EDACS_GUARD_MISSING_FREQUENCY,
    EDACS_GUARD_MISSING_CC_LCN,
    EDACS_GUARD_TRUNK_DISABLED,
} edacs_no_tune_guard;

static dsd_opts g_opts;
static dsd_state g_state;
static dsd_trunk_tune_result g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
static dsd_trunk_tune_result g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_vc_tune_count = 0;
static int g_cc_tune_count = 0;
static int g_skip_dibit_count = 0;
static long g_last_vc_freq = 0;
static long g_last_cc_freq = 0;
static long g_rigctl_current_freq = 0;
static int g_tcp_read_count = 0;
static int g_tcp_close_count = 0;
static int g_tcp_fail_at = -1;
static int g_udp_read_count = 0;
static int g_udp_fail_every = 0;
static int g_udp_blast_count = 0;
static size_t g_udp_blast_bytes[3];
static short g_udp_blast_first[3];

/* Issue #633: what the fake TCP input carries, and the events it fires at given samples. */
enum { EDACS_TCP_RAMP = 0, EDACS_TCP_DOTTING = 1, EDACS_TCP_TONE = 2 };

static int g_tcp_signal = EDACS_TCP_RAMP;
static int g_tcp_signal_rate_hz = 48000;
/* With EDACS_TCP_TONE: dotting below g_tcp_dotting_until and from g_tcp_dotting_from, silence from g_tcp_silence_from. */
static int g_tcp_dotting_until = 0;
static int g_tcp_dotting_from = -1;
static int g_tcp_silence_from = -1;
static int g_tcp_retune_at = -1;
static int g_tcp_publish_at = -1;
static dsd_trunk_tune_result g_tcp_publish_result = DSD_TRUNK_TUNE_RESULT_OK;
static uint64_t g_last_vc_request = 0U;
static int g_retune_after_blast = -1;
/* Every sample the UDP analog socket was handed, up to 12 blocks. */
static short g_udp_blast_samples[12][960];
static Event_History_I g_eot_history[2];
static max_align_t g_wav_sentinel;
static int g_close_wav_count = 0;
static int g_open_wav_count = 0;
#ifdef USE_RADIO
static int g_rtl_read_count = 0;
/* When above 0, every fake RTL read returns this sample instead of the counting ramp. */
static float g_rtl_read_value = 0.0f;
static int g_rtl_return_pwr_count = 0;
static int g_rtl_fail_at = -1;
#endif

// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_skipDibit(dsd_opts* opts, dsd_state* state, int count);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __wrap_watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
SNDFILE* __wrap_close_and_rename_wav_file(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename,
                                          const char* dir, const Event_History_I* event_struct, time_t opened);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
SNDFILE* __wrap_close_and_rename_wav_file_ex(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename,
                                             const char* dir, const Event_History_I* event_struct, time_t opened,
                                             int export_call);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
SNDFILE* __wrap_open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate,
                              uint8_t ext);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames);

/* Issue #574: the blocks written to the local output while g_audio_write_capture is set. */
static int g_audio_write_capture = 0;
static int g_audio_write_count = 0;

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_skipDibit(dsd_opts* opts, dsd_state* state, int count) {
    (void)opts;
    (void)state;
    g_skip_dibit_count += count;
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

void
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)slot;
}

SNDFILE*
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_close_and_rename_wav_file(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename, const char* dir,
                                 const Event_History_I* event_struct, time_t opened) {
    (void)wav_file;
    (void)opts;
    (void)wav_out_filename;
    (void)dir;
    (void)event_struct;
    (void)opened;
    g_close_wav_count++;
    return NULL;
}

SNDFILE*
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_close_and_rename_wav_file_ex(SNDFILE* wav_file, const dsd_opts* opts, const char* wav_out_filename,
                                    const char* dir, const Event_History_I* event_struct, time_t opened,
                                    int export_call) {
    (void)export_call;
    return __wrap_close_and_rename_wav_file(wav_file, opts, wav_out_filename, dir, event_struct, opened);
}

SNDFILE*
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_open_wav_file(char* dir, char* temp_filename, size_t temp_filename_size, uint16_t sample_rate, uint8_t ext) {
    (void)dir;
    (void)temp_filename;
    (void)temp_filename_size;
    (void)sample_rate;
    (void)ext;
    g_open_wav_count++;
    return (SNDFILE*)&g_wav_sentinel;
}

int
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
__wrap_dsd_audio_write(dsd_audio_stream* stream, const int16_t* buffer, size_t frames) {
    if (!g_audio_write_capture) {
        return __real_dsd_audio_write(stream, buffer, frames);
    }
    g_audio_write_count++;
    return (int)frames;
}

static dsd_trunk_tune_result
edacs_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    g_last_vc_request = request_id;
    (void)ted_sps;
    g_vc_tune_count++;
    g_last_vc_freq = freq;
    if (dsd_trunk_tune_result_is_ok(g_vc_result)) {
        if (opts) {
            opts->trunk_is_tuned = 1;
        }
        if (state) {
            state->p25_vc_freq[0] = freq;
            state->p25_vc_freq[1] = freq;
            state->trunk_vc_freq[0] = freq;
            state->trunk_vc_freq[1] = freq;
        }
    }
    return g_vc_result;
}

static dsd_trunk_tune_result
edacs_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)ted_sps;
    g_cc_tune_count++;
    g_last_cc_freq = freq;
    if (dsd_trunk_tune_result_is_ok(g_cc_result) && state) {
        state->trunk_cc_freq = freq;
    }
    return g_cc_result;
}

static long int
edacs_hook_get_current_freq_hz(const dsd_opts* opts) {
    (void)opts;
    return g_rigctl_current_freq;
}

static void
edacs_install_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = edacs_hook_tune_to_freq;
    hooks.tune_to_cc_request = edacs_hook_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){
        .get_current_freq_hz = edacs_hook_get_current_freq_hz,
    });
}

/* EDACS dotting at 9600 baud, sampled at @p rate_hz: alternating bits, each 1/9600 s long. */
static int16_t
edacs_dotting_sample(int index, int rate_hz) {
    return (int16_t)((((int64_t)index * 9600) / rate_hz) % 2 == 0 ? 8000 : -8000);
}

static int
edacs_fake_tcp_read_sample(tcp_input_ctx* ctx, int16_t* out) {
    (void)ctx;
    const int index = g_tcp_read_count++;
    if (out == NULL || (g_tcp_fail_at >= 0 && index == g_tcp_fail_at)) {
        return 0;
    }
    if (g_tcp_retune_at >= 0 && index == g_tcp_retune_at) {
        dsd_trunk_tuning_generation_advance();
    }
    if (g_tcp_publish_at >= 0 && index == g_tcp_publish_at) {
        dsd_trunk_tuning_request_publish(g_last_vc_request, g_tcp_publish_result);
    }
    switch (g_tcp_signal) {
        case EDACS_TCP_DOTTING: *out = edacs_dotting_sample(index, g_tcp_signal_rate_hz); break;
        case EDACS_TCP_TONE:
            if (index < g_tcp_dotting_until || (g_tcp_dotting_from >= 0 && index >= g_tcp_dotting_from)) {
                *out = edacs_dotting_sample(index, g_tcp_signal_rate_hz);
            } else if (g_tcp_silence_from >= 0 && index >= g_tcp_silence_from) {
                *out = 0;
            } else {
                *out = (int16_t)lround(
                    8000.0 * sin(2.0 * 3.14159265358979323846 * 700.0 * (double)index / (double)g_tcp_signal_rate_hz));
            }
            break;
        default: *out = (int16_t)(1000 + index); break;
    }
    return 1;
}

static void
edacs_fake_tcp_close(tcp_input_ctx* ctx) {
    (void)ctx;
    g_tcp_close_count++;
}

static int
edacs_fake_udp_read_sample(dsd_opts* opts, int16_t* out) {
    (void)opts;
    const int index = g_udp_read_count++;
    if (out == NULL || (g_udp_fail_every > 0 && (index % g_udp_fail_every) == 0)) {
        return 0;
    }
    *out = (int16_t)(2000 + index);
    return 1;
}

static void
edacs_install_net_audio_hooks(void) {
    dsd_net_audio_input_hooks hooks = {0};
    hooks.tcp_read_sample = edacs_fake_tcp_read_sample;
    hooks.tcp_close = edacs_fake_tcp_close;
    hooks.udp_read_sample = edacs_fake_udp_read_sample;
    dsd_net_audio_input_hooks_set(hooks);
}

static void
edacs_fake_blast_analog(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    if (g_udp_blast_count < 3) {
        g_udp_blast_bytes[g_udp_blast_count] = nsam;
        g_udp_blast_first[g_udp_blast_count] = data != NULL ? ((const short*)data)[0] : 0;
    }
    if (g_udp_blast_count < 12 && data != NULL && nsam == sizeof(g_udp_blast_samples[0])) {
        DSD_MEMCPY(g_udp_blast_samples[g_udp_blast_count], data, nsam);
    }
    g_udp_blast_count++;
    if (g_retune_after_blast >= 0 && g_udp_blast_count == g_retune_after_blast) {
        dsd_trunk_tuning_generation_advance();
    }
}

static void
edacs_install_udp_output_hooks(void) {
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){
        .blast_analog = edacs_fake_blast_analog,
    });
}

#ifdef USE_RADIO
static int
edacs_fake_rtl_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    (void)rtl_ctx;
    const int index = g_rtl_read_count++;
    if (out_got != NULL) {
        *out_got = 0;
    }
    if (out == NULL || out_got == NULL || count != 1U || (g_rtl_fail_at >= 0 && index == g_rtl_fail_at)) {
        return -1;
    }
    out[0] = g_rtl_read_value > 0.0f ? g_rtl_read_value : 100.0f + (float)index;
    *out_got = 1;
    return 0;
}

static double
edacs_fake_rtl_return_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    g_rtl_return_pwr_count++;
    return 77.25;
}

static void
edacs_install_rtl_stream_hooks(void) {
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = edacs_fake_rtl_read,
        .return_pwr = edacs_fake_rtl_return_pwr,
    });
}

static unsigned int g_rtl_output_rate_hz = 0U;
static uint32_t g_rtl_generation = 1U;
/* The read of the output rate (counted from 1) that lands a retune to g_rtl_switch_rate_hz, a new stream generation,
   as the rate is read (issue #633); 0 lands none. */
static int g_rtl_rate_reads = 0;
static int g_rtl_switch_at_rate_read = 0;
static unsigned int g_rtl_switch_rate_hz = 0U;

static unsigned int
edacs_fake_rtl_output_rate_hz(void) {
    const unsigned int rate = g_rtl_output_rate_hz;
    if (++g_rtl_rate_reads == g_rtl_switch_at_rate_read) {
        g_rtl_output_rate_hz = g_rtl_switch_rate_hz;
        g_rtl_generation++;
    }
    return rate;
}

/* The first generation read after this many RTL reads publishes the call tune's completion: it lands right after a
   triplet was collected, as an asynchronous completion can (issue #633). 0 publishes none. */
static int g_rtl_publish_after_reads = 0;

static uint32_t
edacs_fake_rtl_generation(void) {
    if (g_rtl_publish_after_reads > 0 && g_rtl_read_count >= g_rtl_publish_after_reads) {
        g_rtl_publish_after_reads = 0;
        dsd_trunk_tuning_request_publish(g_last_vc_request, DSD_TRUNK_TUNE_RESULT_OK);
    }
    return g_rtl_generation;
}

/* The RTL stream's output rate EDACS reads analog voice at (issue #633), and its generation; 0 removes the hooks. */
static void
edacs_install_rtl_rate_hook(unsigned int rate_hz) {
    g_rtl_output_rate_hz = rate_hz;
    g_rtl_rate_reads = 0;
    g_rtl_switch_at_rate_read = 0;
    g_rtl_publish_after_reads = 0;
    dsd_rtl_stream_metrics_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    if (rate_hz > 0U) {
        hooks.output_rate_hz = edacs_fake_rtl_output_rate_hz;
        hooks.stream_generation = edacs_fake_rtl_generation;
    }
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}
#endif

static void
edacs_reset_audio_hook_state(void) {
    g_tcp_read_count = 0;
    g_tcp_close_count = 0;
    g_tcp_fail_at = -1;
    g_tcp_signal = EDACS_TCP_RAMP;
    g_tcp_signal_rate_hz = 48000;
    g_tcp_dotting_until = 0;
    g_tcp_dotting_from = -1;
    g_tcp_silence_from = -1;
    g_tcp_retune_at = -1;
    g_tcp_publish_at = -1;
    g_tcp_publish_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_retune_after_blast = -1;
    DSD_MEMSET(g_udp_blast_samples, 0, sizeof(g_udp_blast_samples));
    g_udp_read_count = 0;
    g_udp_fail_every = 0;
    g_udp_blast_count = 0;
    DSD_MEMSET(g_udp_blast_bytes, 0, sizeof(g_udp_blast_bytes));
    DSD_MEMSET(g_udp_blast_first, 0, sizeof(g_udp_blast_first));
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
#ifdef USE_RADIO
    g_rtl_read_count = 0;
    g_rtl_return_pwr_count = 0;
    g_rtl_fail_at = -1;
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
#endif
    dsd_exitflag_store(0);
}

static int
edacs_expect(int cond, const char* test_case, const char* result_name, const char* check) {
    if (cond) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL case=%s result=%s check=%s\n", test_case, result_name, check);
    return 1;
}

static int
edacs_expect_recent(int lcn, dsd_call_kind kind, uint64_t target, uint64_t source, uint16_t service_options,
                    const char* test_case, const char* result_name) {
    dsd_recent_activity_snapshot recent = {0};
    int rc = 0;
    rc |= edacs_expect(lcn >= 0 && lcn < DSD_RECENT_ACTIVITY_COUNT, test_case, result_name,
                       "recent activity index is valid");
    if (lcn < 0 || lcn >= DSD_RECENT_ACTIVITY_COUNT) {
        return rc;
    }
    rc |= edacs_expect(dsd_recent_activity_copy_snapshot(&g_state, &recent) > 0, test_case, result_name,
                       "recent activity snapshot exists");
    const dsd_recent_activity_entry* entry = &recent.entries[lcn];
    rc |= edacs_expect(entry->notice[0] != '\0', test_case, result_name, "recent activity notice exists");
    rc |= edacs_expect(entry->observation.channel == (uint32_t)lcn, test_case, result_name,
                       "recent activity channel matches LCN");
    rc |= edacs_expect(entry->observation.kind == kind, test_case, result_name, "recent activity kind");
    rc |= edacs_expect(entry->observation.ota_target_id == target && entry->observation.ota_source_id == source,
                       test_case, result_name, "recent activity target/source");
    rc |= edacs_expect(entry->observation.service_options == service_options, test_case, result_name,
                       "recent activity service flags");
    rc |= edacs_expect(entry->observation.emergency == ((service_options & EDACS_IS_EMERGENCY) != 0), test_case,
                       result_name, "recent activity emergency flag");
    return rc;
}

static int
edacs_expect_active(const edacs_grant_case* test_case, int active, const char* result_name) {
    dsd_call_snapshot call = {0};
    const int got = dsd_call_state_get(&g_state, 0U, &call);
    if (!active) {
        return edacs_expect(got <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE, test_case->name, result_name,
                            "grant did not create an active call");
    }

    const dsd_call_kind kind =
        (test_case->expected_flags & EDACS_IS_GROUP) != 0
            ? DSD_CALL_KIND_GROUP_VOICE
            : ((test_case->expected_flags & EDACS_IS_INDIVIDUAL) != 0 ? DSD_CALL_KIND_PRIVATE_VOICE
                                                                      : DSD_CALL_KIND_VOICE);
    int rc = 0;
    rc |= edacs_expect(got > 0 && call.phase == DSD_CALL_PHASE_ACTIVE, test_case->name, result_name,
                       "accepted grant created an active call");
    rc |= edacs_expect(call.kind == kind, test_case->name, result_name, "active call kind");
    rc |= edacs_expect(call.channel == (uint32_t)test_case->lcn && call.frequency_hz == test_case->freq_hz,
                       test_case->name, result_name, "active call channel/frequency");
    rc |= edacs_expect(call.ota_target_id == (uint64_t)test_case->expected_lasttg
                           && call.ota_source_id == (uint64_t)test_case->expected_lastsrc,
                       test_case->name, result_name, "active call target/source");
    rc |= edacs_expect(call.service_options == (uint16_t)test_case->expected_flags, test_case->name, result_name,
                       "active call service flags");
    return rc;
}

static int
edacs_expect_recent_only(int lcn, dsd_call_kind kind, uint64_t target, uint64_t source, uint16_t service_options,
                         const char* test_case) {
    int rc = edacs_expect_recent(lcn, kind, target, source, service_options, test_case, "state");
    dsd_call_snapshot call = {0};
    rc |= edacs_expect(dsd_call_state_get(&g_state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE, test_case,
                       "state", "control-channel activity did not create a voice call");
    return rc;
}

static unsigned long long int
edacs_standard_group_msg1(int mt_a, int lcn, int group, int source_lid) {
    unsigned long long int msg = ((unsigned long long int)(mt_a & 0x7) << 25U);
    msg |= ((unsigned long long int)(source_lid & 0x3F80) << 11U);
    msg |= ((unsigned long long int)(lcn & 0x1F) << 12U);
    msg |= (1ULL << 11U);
    msg |= (unsigned long long int)(group & 0x7FF);
    return msg;
}

static unsigned long long int
edacs_standard_group_msg2(int source_lid) {
    return (unsigned long long int)(source_lid & 0x7F) << 17U;
}

static unsigned long long int
edacs_standard_individual_msg1(int lcn, int target, int is_digital) {
    unsigned long long int msg = (7ULL << 25U) | (5ULL << 22U);
    msg |= 1ULL << 21U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_digital ? 1U : 0U) << 14U;
    msg |= (unsigned long long int)(target & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_extended_group_msg1(int mt1, int lcn, int group) {
    unsigned long long int msg = (unsigned long long int)(mt1 & 0x1F) << 23U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 17U;
    msg |= (unsigned long long int)(group & 0xFFFF);
    return msg;
}

static unsigned long long int
edacs_extended_group_update_msg1(int mt1, int lcn, int group, int is_update) {
    unsigned long long int msg = edacs_extended_group_msg1(mt1, lcn, group);
    msg |= (unsigned long long int)(is_update ? 1U : 0U) << 16U;
    return msg;
}

static unsigned long long int
edacs_extended_group_msg2(int source, int is_emergency, int is_tx_trunking) {
    unsigned long long int msg = (unsigned long long int)(source & 0xFFFFF);
    msg |= (unsigned long long int)(is_emergency ? 1U : 0U) << 20U;
    msg |= (unsigned long long int)(is_tx_trunking ? 1U : 0U) << 21U;
    return msg;
}

static unsigned long long int
edacs_extended_icall_msg1(int target) {
    unsigned long long int msg = 0x10ULL << 23U;
    msg |= 1ULL << 21U;
    msg |= (unsigned long long int)(target & 0xFFFFF);
    return msg;
}

static unsigned long long int
edacs_extended_icall_msg2(int lcn, int source) {
    unsigned long long int msg = (unsigned long long int)(lcn & 0x1F) << 20U;
    msg |= (unsigned long long int)(source & 0xFFFFF);
    return msg;
}

static unsigned long long int
edacs_standard_data_msg1(int is_individual_call, int lcn, int target, int is_individual_id) {
    unsigned long long int msg = 5ULL << 25U;
    msg |= (unsigned long long int)(is_individual_call ? 1U : 0U) << 24U;
    msg |= 1ULL << 23U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_individual_id ? 1U : 0U) << 14U;
    msg |= (unsigned long long int)(target & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_standard_data_msg2(int port) {
    return (unsigned long long int)(port & 0x7) << 20U;
}

static unsigned long long int
edacs_standard_interconnect_msg1(int mt_c, int lcn, int target, int is_individual_id) {
    unsigned long long int msg = (7ULL << 25U) | (1ULL << 22U);
    msg |= (unsigned long long int)(mt_c & 0x3) << 20U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_individual_id ? 1U : 0U) << 14U;
    msg |= (unsigned long long int)(target & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_standard_channel_update_msg1(int mt_c, int lcn, int group, int is_emergency) {
    unsigned long long int msg = (7ULL << 25U) | (3ULL << 22U);
    msg |= (unsigned long long int)(mt_c & 0x3) << 20U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= (unsigned long long int)(is_emergency ? 1U : 0U) << 13U;
    msg |= (unsigned long long int)(group & 0x7FF);
    return msg;
}

static unsigned long long int
edacs_standard_channel_update_individual_msg1(int mt_c, int lcn, int lid) {
    unsigned long long int msg = (7ULL << 25U) | (3ULL << 22U);
    msg |= (unsigned long long int)(mt_c & 0x3) << 20U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 15U;
    msg |= 1ULL << 14U;
    msg |= (unsigned long long int)(lid & 0x3FFF);
    return msg;
}

static unsigned long long int
edacs_standard_mt_d_msg1(int mt_d) {
    return (7ULL << 25U) | (7ULL << 22U) | ((unsigned long long int)(mt_d & 0x1F) << 17U);
}

static unsigned long long int
edacs_standard_site_id_msg1(int cc_lcn, int priority, int site_id, int is_auxiliary) {
    unsigned long long int msg = edacs_standard_mt_d_msg1(0x08);
    msg |= (unsigned long long int)(cc_lcn & 0x1F) << 12U;
    msg |= (unsigned long long int)(priority & 0x7) << 9U;
    msg |= (unsigned long long int)(is_auxiliary ? 1U : 0U) << 5U;
    msg |= (unsigned long long int)(site_id & 0x1F);
    return msg;
}

static unsigned long long int
edacs_standard_all_call_msg1(int lcn, int is_digital, int lid) {
    unsigned long long int msg = edacs_standard_mt_d_msg1(0x0F);
    msg |= (unsigned long long int)(lcn & 0x1F) << 12U;
    msg |= (unsigned long long int)(is_digital ? 1U : 0U) << 11U;
    msg |= 1ULL << 9U;
    msg |= (unsigned long long int)(lid & 0x7F);
    return msg;
}

static unsigned long long int
edacs_standard_all_call_msg2(int lid) {
    return (unsigned long long int)((lid >> 7U) & 0x7F) << 1U;
}

static unsigned long long int
edacs_extended_mt2_msg1(int mt2) {
    return (0x1FULL << 23U) | ((unsigned long long int)(mt2 & 0xF) << 19U);
}

static unsigned long long int
edacs_extended_system_info_msg1(int system) {
    return edacs_extended_mt2_msg1(0x8) | (unsigned long long int)(system & 0xFFFF);
}

static unsigned long long int
edacs_extended_site_id_msg1(int site_id, int area) {
    unsigned long long int msg = edacs_extended_mt2_msg1(0xA);
    msg |= (unsigned long long int)(site_id & 0xE0) << 7U;
    msg |= (unsigned long long int)(area & 0x7F) << 5U;
    msg |= (unsigned long long int)(site_id & 0x1F);
    return msg;
}

static unsigned long long int
edacs_extended_test_call_msg1(int cc_lcn, int wc_lcn) {
    unsigned long long int msg = edacs_extended_mt2_msg1(0x0);
    msg |= (unsigned long long int)(cc_lcn & 0x1F) << 13U;
    msg |= (unsigned long long int)(wc_lcn & 0x1F) << 7U;
    return msg;
}

static unsigned long long int
edacs_extended_channel_assignment_msg2(int lcn, int source) {
    unsigned long long int msg = (unsigned long long int)(lcn & 0x1F) << 20U;
    msg |= (unsigned long long int)(source & 0xFFFFF);
    return msg;
}

static unsigned long long int
edacs_extended_all_call_msg1(int lcn, int is_digital, int is_update) {
    unsigned long long int msg = 0x16ULL << 23U;
    msg |= (unsigned long long int)(lcn & 0x1F) << 17U;
    msg |= (unsigned long long int)(is_digital ? 1U : 0U) << 16U;
    msg |= (unsigned long long int)(is_update ? 1U : 0U) << 15U;
    return msg;
}

static void
edacs_setup_fixture(const edacs_grant_case* test_case) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));

    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.trunk_tune_private_calls = 1;
    g_opts.trunk_hangtime = 1.0f;
    g_state.ea_mode = test_case->ea_mode;
    g_state.edacs_cc_lcn = 1;
    g_state.edacs_tuned_lcn = -1;
    g_state.trunk_lcn_freq[0] = 851012500L;
    g_state.p25_cc_freq = g_state.trunk_lcn_freq[0];
    g_state.trunk_cc_freq = g_state.trunk_lcn_freq[0];
    g_state.trunk_lcn_freq[test_case->lcn - 1] = test_case->freq_hz;

    g_vc_tune_count = 0;
    g_cc_tune_count = 0;
    g_skip_dibit_count = 0;
    g_last_vc_freq = 0;
    g_last_cc_freq = 0;
    g_rigctl_current_freq = 0;
}

static void
edacs_setup_state_fixture(int ea_mode) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));

    g_opts.trunk_enable = 1;
    g_state.ea_mode = ea_mode;
    g_state.edacs_cc_lcn = 1;
    g_state.edacs_tuned_lcn = -1;
    g_state.trunk_lcn_freq[0] = 851012500L;
    g_state.trunk_lcn_freq[3] = 852762500L;
    g_state.trunk_lcn_freq[8] = 854012500L;
    g_state.p25_cc_freq = g_state.trunk_lcn_freq[0];
    g_state.trunk_cc_freq = g_state.trunk_lcn_freq[0];

    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_vc_tune_count = 0;
    g_cc_tune_count = 0;
    g_skip_dibit_count = 0;
    g_last_vc_freq = 0;
    g_last_cc_freq = 0;
    g_rigctl_current_freq = 0;
    edacs_install_hooks();
}

static int
edacs_run_inverted_polarity_cases(const edacs_grant_case* voice_case) {
    int rc = 0;
    dsd_call_snapshot call = {0};
    dsd_recent_activity_snapshot recent = {0};

    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(voice_case);
    g_state.synctype = DSD_SYNC_EDACS_NEG;
    edacs_install_hooks();
    edacs_process_valid_frame(&g_opts, &g_state, voice_case->msg_1, voice_case->msg_2);

    rc |= edacs_expect(dsd_call_state_get(&g_state, 0U, &call) > 0, "inverted-polarity", "voice",
                       "digital grant created canonical call");
    rc |= edacs_expect(call.protocol == DSD_SYNC_PROVOICE_NEG, "inverted-polarity", "voice",
                       "canonical digital grant preserved inverted polarity");
    rc |= edacs_expect(dsd_recent_activity_copy_snapshot(&g_state, &recent) > 0, "inverted-polarity", "voice",
                       "digital grant published recent activity");
    rc |= edacs_expect(recent.entries[voice_case->lcn].observation.protocol == DSD_SYNC_PROVOICE_NEG,
                       "inverted-polarity", "voice", "recent digital grant preserved inverted polarity");

    edacs_setup_state_fixture(0);
    g_state.synctype = DSD_SYNC_EDACS_NEG;
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_data_msg1(0, 4, 777, 0), edacs_standard_data_msg2(5));

    DSD_MEMSET(&recent, 0, sizeof(recent));
    rc |= edacs_expect(dsd_recent_activity_copy_snapshot(&g_state, &recent) > 0, "inverted-polarity", "data",
                       "data grant published recent activity");
    rc |= edacs_expect(recent.entries[4].observation.protocol == DSD_SYNC_EDACS_NEG, "inverted-polarity", "data",
                       "recent data grant preserved inverted polarity");
    return rc;
}

static int
edacs_run_grant_result_case(const edacs_grant_case* test_case, dsd_trunk_tune_result result, const char* result_name) {
    g_vc_result = result;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_install_hooks();

    edacs_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    const int accepted = dsd_trunk_tune_result_is_ok(result);
    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 1, test_case->name, result_name, "voice tune attempted once");
    rc |= edacs_expect(g_last_vc_freq == test_case->freq_hz, test_case->name, result_name,
                       "voice tune frequency matches LCN map");
    const dsd_call_kind kind =
        (test_case->expected_flags & EDACS_IS_GROUP) != 0 ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE;
    rc |= edacs_expect_recent(test_case->lcn, kind, (uint64_t)test_case->expected_lasttg,
                              (uint64_t)test_case->expected_lastsrc, (uint16_t)test_case->expected_flags,
                              test_case->name, result_name);
    rc |= edacs_expect_active(test_case, accepted, result_name);

    if (accepted) {
        rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, result_name,
                           "accepted tune set tuned LCN");
        rc |= edacs_expect(g_opts.trunk_is_tuned == 1, test_case->name, result_name, "accepted tune set tuned flags");
        rc |=
            edacs_expect(g_state.trunk_vc_freq[0] == test_case->freq_hz && g_state.p25_vc_freq[0] == test_case->freq_hz,
                         test_case->name, result_name, "accepted tune set VC frequencies");
    } else {
        rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, result_name,
                           "rejected tune left tuned LCN clear");
        rc |= edacs_expect(g_opts.trunk_is_tuned == 0, test_case->name, result_name,
                           "rejected tune left tuned flags clear");
        rc |= edacs_expect(g_state.trunk_vc_freq[0] == 0 && g_state.p25_vc_freq[0] == 0, test_case->name, result_name,
                           "rejected tune left VC frequencies clear");
    }
    return rc;
}

static void
edacs_apply_no_tune_guard(const edacs_grant_case* test_case, edacs_no_tune_guard guard) {
    switch (guard) {
        case EDACS_GUARD_GROUP_DISABLED: g_opts.trunk_tune_group_calls = 0; break;
        case EDACS_GUARD_PRIVATE_DISABLED: g_opts.trunk_tune_private_calls = 0; break;
        case EDACS_GUARD_ALLOWLIST_BLOCK: g_opts.trunk_use_allow_list = 1; break;
        case EDACS_GUARD_MISSING_FREQUENCY: g_state.trunk_lcn_freq[test_case->lcn - 1] = 0; break;
        case EDACS_GUARD_MISSING_CC_LCN: g_state.edacs_cc_lcn = 0; break;
        case EDACS_GUARD_TRUNK_DISABLED: g_opts.trunk_enable = 0; break;
    }
}

static int
edacs_run_no_tune_guard_case(const edacs_grant_case* test_case, edacs_no_tune_guard guard, const char* guard_name) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_apply_no_tune_guard(test_case, guard);
    edacs_install_hooks();

    edacs_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 0, test_case->name, guard_name, "guard did not attempt tune");
    rc |= edacs_expect(g_last_vc_freq == 0, test_case->name, guard_name, "guard left tune frequency clear");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, guard_name, "guard left tuned LCN clear");
    rc |= edacs_expect(g_opts.trunk_is_tuned == 0, test_case->name, guard_name, "guard left tuned flags clear");
    rc |= edacs_expect(g_state.trunk_vc_freq[0] == 0 && g_state.p25_vc_freq[0] == 0, test_case->name, guard_name,
                       "guard left VC frequencies clear");
    const dsd_call_kind kind =
        (test_case->expected_flags & EDACS_IS_GROUP) != 0 ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE;
    rc |= edacs_expect_recent(test_case->lcn, kind, (uint64_t)test_case->expected_lasttg,
                              (uint64_t)test_case->expected_lastsrc, (uint16_t)test_case->expected_flags,
                              test_case->name, guard_name);
    rc |= edacs_expect_active(test_case, 0, guard_name);
    return rc;
}

static int
edacs_run_retry_after_reject_case(const edacs_grant_case* test_case, dsd_trunk_tune_result first_result,
                                  const char* result_name) {
    g_vc_result = first_result;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_install_hooks();

    edacs_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 1, test_case->name, result_name, "rejected tune attempted once");
    rc |=
        edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, result_name, "rejected tune left tuned LCN clear");
    rc |=
        edacs_expect(g_opts.trunk_is_tuned == 0, test_case->name, result_name, "rejected tune left tuned flags clear");

    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    rc |= edacs_expect(g_vc_tune_count == 2, test_case->name, result_name, "later grant retried tune");
    rc |= edacs_expect(g_last_vc_freq == test_case->freq_hz, test_case->name, result_name,
                       "retried tune frequency matches LCN map");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, result_name,
                       "retried tune set tuned LCN");
    rc |= edacs_expect(g_opts.trunk_is_tuned == 1, test_case->name, result_name, "retried tune set tuned flags");
    rc |= edacs_expect(g_state.trunk_vc_freq[0] == test_case->freq_hz && g_state.p25_vc_freq[0] == test_case->freq_hz,
                       test_case->name, result_name, "retried tune set VC frequencies");
    return rc;
}

static void
edacs_setup_eot_fixture(void) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));

    g_opts.trunk_enable = 1;
    g_opts.trunk_is_tuned = 1;
    g_state.p25_cc_freq = 851012500L;
    g_state.trunk_cc_freq = 851012500L;
    g_state.p25_vc_freq[0] = 852012500L;
    g_state.p25_vc_freq[1] = 852012500L;
    g_state.trunk_vc_freq[0] = 852012500L;
    g_state.trunk_vc_freq[1] = 852012500L;
    g_state.edacs_tuned_lcn = 5;
    g_state.payload_algid = 0x84;
    g_state.payload_keyid = 0x1234;
    g_state.payload_miP = 0x5678;

    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_PROVOICE_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .policy_target_id = 1201U,
        .ota_source_id = 42001U,
        .channel = 5U,
        .frequency_hz = 852012500L,
        .service_options = EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_DIGITAL,
        .has_service_metadata = 1U,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    (void)dsd_call_state_update_crypto(&g_state, 0U,
                                       &(dsd_call_crypto_update){
                                           .classification = DSD_CALL_CRYPTO_ENCRYPTED,
                                           .algid = 0x84,
                                           .kid = 0x1234,
                                           .mi = 0x5678,
                                       });
    (void)dsd_recent_activity_publish(&g_state, 5U, &observation, "EDACS active grant", 0U);

    g_vc_tune_count = 0;
    g_cc_tune_count = 0;
    g_skip_dibit_count = 0;
    g_last_vc_freq = 0;
    g_last_cc_freq = 0;
}

static int
edacs_run_eot_result_case(dsd_trunk_tune_result result, const char* result_name) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = result;
    edacs_setup_eot_fixture();
    edacs_install_hooks();

    eot_cc(&g_opts, &g_state);

    const int accepted = dsd_trunk_tune_result_is_ok(result);
    int rc = 0;
    rc |= edacs_expect(g_cc_tune_count == 1, "eot-cc", result_name, "CC tune attempted once");
    rc |= edacs_expect(g_last_cc_freq == 851012500L, "eot-cc", result_name, "CC tune frequency");
    rc |= edacs_expect(g_skip_dibit_count == (240 * 8), "eot-cc", result_name, "EOT dibit skip was bounded");
    dsd_call_snapshot call = {0};
    rc |= edacs_expect(dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED, "eot-cc",
                       result_name, "EOT ended canonical call before retuning");
    rc |= edacs_expect(call.ota_target_id == 1201U && call.ota_source_id == 42001U, "eot-cc", result_name,
                       "ended call retained identity");
    rc |= edacs_expect_recent(5, DSD_CALL_KIND_GROUP_VOICE, 1201U, 42001U,
                              EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_DIGITAL, "eot-cc", result_name);

    if (accepted) {
        rc |= edacs_expect(g_opts.trunk_is_tuned == 0, "eot-cc", result_name, "accepted CC return cleared tuned flags");
        rc |=
            edacs_expect(g_state.edacs_tuned_lcn == -1, "eot-cc", result_name, "accepted CC return cleared tuned LCN");
        rc |= edacs_expect(g_state.p25_vc_freq[0] == 0 && g_state.trunk_vc_freq[0] == 0, "eot-cc", result_name,
                           "accepted CC return cleared VC frequencies");
        rc |= edacs_expect(g_state.payload_algid == 0 && g_state.payload_keyid == 0 && g_state.payload_miP == 0,
                           "eot-cc", result_name, "accepted CC return cleared payload metadata");
    } else {
        rc |=
            edacs_expect(g_opts.trunk_is_tuned == 1, "eot-cc", result_name, "rejected CC return preserved tuned flags");
        rc |=
            edacs_expect(g_state.edacs_tuned_lcn == 5, "eot-cc", result_name, "rejected CC return preserved tuned LCN");
        rc |= edacs_expect(g_state.p25_vc_freq[0] == 852012500L && g_state.trunk_vc_freq[0] == 852012500L, "eot-cc",
                           result_name, "rejected CC return preserved VC frequencies");
        rc |= edacs_expect(g_state.payload_algid == 0x84 && g_state.payload_keyid == 0x1234
                               && g_state.payload_miP == 0x5678,
                           "eot-cc", result_name, "rejected CC return preserved payload metadata");
    }
    return rc;
}

static int
edacs_run_eot_retry_after_reject_case(dsd_trunk_tune_result first_result, const char* result_name) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = first_result;
    edacs_setup_eot_fixture();
    edacs_install_hooks();

    eot_cc(&g_opts, &g_state);

    int rc = 0;
    rc |= edacs_expect(g_cc_tune_count == 1, "eot-retry", result_name, "rejected CC tune attempted once");
    rc |= edacs_expect(g_opts.trunk_is_tuned == 1, "eot-retry", result_name, "rejected CC tune preserved tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == 5, "eot-retry", result_name, "rejected CC tune preserved tuned LCN");

    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    eot_cc(&g_opts, &g_state);

    rc |= edacs_expect(g_cc_tune_count == 2, "eot-retry", result_name, "later EOT retried CC tune");
    rc |= edacs_expect(g_opts.trunk_is_tuned == 0, "eot-retry", result_name, "retried CC tune cleared tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, "eot-retry", result_name, "retried CC tune cleared tuned LCN");
    rc |= edacs_expect(g_state.p25_vc_freq[0] == 0 && g_state.trunk_vc_freq[0] == 0, "eot-retry", result_name,
                       "retried CC tune cleared VC frequencies");
    return rc;
}

static int
edacs_run_eot_wav_rotation_case(void) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_eot_fixture();
    edacs_install_hooks();

    DSD_MEMSET(g_eot_history, 0, sizeof(g_eot_history));
    init_event_history(&g_eot_history[0], 0U, 255U);
    init_event_history(&g_eot_history[1], 0U, 255U);
    g_state.event_history_s = g_eot_history;
    g_opts.dmr_stereo_wav = 1;
    g_opts.static_wav_file = 0;
    g_opts.wav_out_f = (SNDFILE*)&g_wav_sentinel;
    g_close_wav_count = 0;
    g_open_wav_count = 0;

    eot_cc(&g_opts, &g_state);

    int rc = edacs_expect(g_close_wav_count == 1, "eot-wav", "dynamic", "EOT closed the call WAV once");
    rc |= edacs_expect(g_open_wav_count == 1, "eot-wav", "dynamic", "EOT opened the next WAV once");
    rc |= edacs_expect(g_opts.wav_out_f == (SNDFILE*)&g_wav_sentinel, "eot-wav", "dynamic",
                       "EOT retained the newly opened WAV");
    return rc;
}

static int
edacs_run_retune_after_eot_case(const edacs_grant_case* test_case) {
    g_vc_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    edacs_setup_fixture(test_case);
    edacs_install_hooks();

    edacs_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);

    int rc = 0;
    rc |= edacs_expect(g_vc_tune_count == 1, test_case->name, "retune-after-eot", "initial grant tuned once");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, "retune-after-eot",
                       "initial grant set tuned LCN");

    eot_cc(&g_opts, &g_state);
    rc |= edacs_expect(g_cc_tune_count == 1, test_case->name, "retune-after-eot", "EOT returned to CC once");
    rc |= edacs_expect(g_opts.trunk_is_tuned == 0, test_case->name, "retune-after-eot", "EOT cleared tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == -1, test_case->name, "retune-after-eot", "EOT cleared tuned LCN");

    edacs_process_valid_frame(&g_opts, &g_state, test_case->msg_1, test_case->msg_2);
    rc |= edacs_expect(g_vc_tune_count == 2, test_case->name, "retune-after-eot", "post-EOT grant retuned");
    rc |= edacs_expect(g_last_vc_freq == test_case->freq_hz, test_case->name, "retune-after-eot",
                       "post-EOT tune frequency matches LCN map");
    rc |=
        edacs_expect(g_opts.trunk_is_tuned == 1, test_case->name, "retune-after-eot", "post-EOT grant set tuned flags");
    rc |= edacs_expect(g_state.edacs_tuned_lcn == test_case->lcn, test_case->name, "retune-after-eot",
                       "post-EOT grant set tuned LCN");
    return rc;
}

static int
edacs_run_standard_state_cases(void) {
    int rc = 0;

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_data_msg1(0, 4, 777, 0), edacs_standard_data_msg2(5));
    rc |= edacs_expect_recent_only(4, DSD_CALL_KIND_DATA, 777U, 0U, 0U, "standard-data-group");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_data_msg1(1, 9, 12345, 1), edacs_standard_data_msg2(3));
    rc |= edacs_expect_recent_only(9, DSD_CALL_KIND_DATA, 12345U, 0U, 0U, "standard-data-individual");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_interconnect_msg1(3, 4, 3210, 1), 0);
    rc |= edacs_expect_recent_only(4, DSD_CALL_KIND_VOICE, 0U, 3210U,
                                   EDACS_IS_VOICE | EDACS_IS_INTERCONNECT | EDACS_IS_DIGITAL, "standard-interconnect");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_msg1(1, 4, 654, 1), 0x2AAAU);
    rc |= edacs_expect_recent_only(4, DSD_CALL_KIND_GROUP_VOICE, 654U, 0U,
                                   EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_DIGITAL | EDACS_IS_EMERGENCY,
                                   "standard-channel-update");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_group_msg1(1, 6, 0x080, 3000),
                              edacs_standard_group_msg2(3000));
    rc |= edacs_expect_recent_only(6, DSD_CALL_KIND_GROUP_VOICE, 0x080U, 3000U,
                                   EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_EMERGENCY | EDACS_IS_AGENCY_CALL,
                                   "standard-analog-agency-emergency");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_msg1(0, 7, 0x088, 0), 0);
    rc |= edacs_expect_recent_only(7, DSD_CALL_KIND_GROUP_VOICE, 0x088U, 0U,
                                   EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_FLEET_CALL,
                                   "standard-channel-update-fleet");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_individual_msg1(3, 8, 4321), 1234);
    rc |= edacs_expect_recent_only(8, DSD_CALL_KIND_PRIVATE_VOICE, 4321U, 1234U,
                                   EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL | EDACS_IS_DIGITAL,
                                   "standard-channel-update-individual");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_channel_update_individual_msg1(0, 9, 0), 0);
    rc |= edacs_expect_recent_only(9, DSD_CALL_KIND_VOICE, 0U, 0U, EDACS_IS_VOICE | EDACS_IS_TEST_CALL,
                                   "standard-channel-update-test-call");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_individual_msg1(10, 0, 1), 0);
    rc |= edacs_expect_recent_only(10, DSD_CALL_KIND_DATA, 0U, 0U, 0U, "standard-individual-test-call");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(4, 2, 0x1B, 0), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0x1B, "standard-site-id", "state", "tracked site id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 4, "standard-site-id", "state", "tracked CC LCN");
    rc |= edacs_expect(g_state.p25_cc_freq == 852762500L && g_state.trunk_cc_freq == 852762500L, "standard-site-id",
                       "state", "updated CC frequency from LCN map");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(9, 3, 0x0C, 1), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0x0C, "standard-aux-site-id", "state", "tracked auxiliary site id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 1, "standard-aux-site-id", "state",
                       "auxiliary site did not replace CC LCN");
    rc |= edacs_expect(g_state.trunk_cc_freq == 851012500L, "standard-aux-site-id", "state",
                       "auxiliary site preserved CC frequency");

    edacs_setup_state_fixture(0);
    g_opts.use_rigctl = 1;
    g_rigctl_current_freq = 855262500L;
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(12, 1, 0x12, 0), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0x12, "standard-site-id-rigctl-capture", "state", "tracked site id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 12, "standard-site-id-rigctl-capture", "state", "tracked rigctl CC LCN");
    rc |= edacs_expect(g_state.trunk_lcn_freq[11] == 855262500L, "standard-site-id-rigctl-capture", "state",
                       "captured missing LCN frequency from rigctl");
    rc |= edacs_expect(g_state.p25_cc_freq == 855262500L && g_state.trunk_cc_freq == 855262500L,
                       "standard-site-id-rigctl-capture", "state", "updated CC frequency from captured rigctl LCN");

    edacs_setup_state_fixture(0);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.rtlsdr_center_freq = 856012500U;
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_site_id_msg1(13, 1, 0x13, 0), 0);
    rc |= edacs_expect(g_state.edacs_cc_lcn == 13, "standard-site-id-rtl-capture", "state", "tracked RTL CC LCN");
    rc |= edacs_expect(g_state.trunk_lcn_freq[12] == 856012500L, "standard-site-id-rtl-capture", "state",
                       "captured missing LCN frequency from RTL center frequency");
    rc |= edacs_expect(g_state.p25_cc_freq == 856012500L && g_state.trunk_cc_freq == 856012500L,
                       "standard-site-id-rtl-capture", "state", "updated CC frequency from captured RTL LCN");

    edacs_setup_state_fixture(0);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_all_call_msg1(9, 1, 1010),
                              edacs_standard_all_call_msg2(1010));
    rc |= edacs_expect_recent_only(9, DSD_CALL_KIND_VOICE, 0U, 1010U,
                                   EDACS_IS_VOICE | EDACS_IS_ALL_CALL | EDACS_IS_DIGITAL, "standard-all-call");

    return rc;
}

static int
edacs_run_extended_state_cases(void) {
    int rc = 0;

    edacs_setup_state_fixture(1);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_extended_system_info_msg1(0x4567), 9);
    rc |= edacs_expect(g_state.edacs_sys_id == 0x4567, "extended-system-info", "state", "tracked system id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 9, "extended-system-info", "state", "tracked CC LCN");
    rc |= edacs_expect(g_state.p25_cc_freq == 854012500L && g_state.trunk_cc_freq == 854012500L, "extended-system-info",
                       "state", "updated CC frequency from LCN map");

    edacs_setup_state_fixture(1);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_extended_site_id_msg1(0xA5, 0x34), 0);
    rc |= edacs_expect(g_state.edacs_site_id == 0xA5, "extended-site-id", "state", "tracked site id");
    rc |= edacs_expect(g_state.edacs_area_code == 0x34, "extended-site-id", "state", "tracked area code");

    edacs_setup_state_fixture(1);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_extended_test_call_msg1(4, 9), 0);
    rc |= edacs_expect_recent_only(9, DSD_CALL_KIND_DATA, 0U, 0U, 0U, "extended-test-call");

    edacs_setup_state_fixture(1);
    edacs_process_valid_frame(&g_opts, &g_state, 0x12ULL << 23U, edacs_extended_channel_assignment_msg2(4, 0xABCDE));
    rc |= edacs_expect_recent_only(4, DSD_CALL_KIND_DATA, 0U, 0xABCDEU, 0U, "extended-channel-assignment");

    edacs_setup_state_fixture(1);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_extended_all_call_msg1(4, 1, 1), 0xBCDEF);
    rc |= edacs_expect_recent_only(4, DSD_CALL_KIND_VOICE, 0U, 0xBCDEFU,
                                   EDACS_IS_VOICE | EDACS_IS_ALL_CALL | EDACS_IS_DIGITAL, "extended-all-call");

    edacs_setup_state_fixture(1);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_extended_group_update_msg1(0x6, 5, 0x3456, 1),
                              edacs_extended_group_msg2(0x45678, 1, 1));
    rc |= edacs_expect_recent_only(5, DSD_CALL_KIND_GROUP_VOICE, 0x3456U, 0x45678U,
                                   EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_EMERGENCY,
                                   "extended-analog-group-emergency-update");

    edacs_setup_state_fixture(1);
    edacs_process_valid_frame(&g_opts, &g_state, edacs_extended_group_msg1(0x3, 0, 0x2222),
                              edacs_extended_group_msg2(0, 0, 1));
    rc |=
        edacs_expect_recent_only(0, DSD_CALL_KIND_GROUP_VOICE, 0x2222U, 0U,
                                 EDACS_IS_VOICE | EDACS_IS_GROUP | EDACS_IS_DIGITAL, "extended-zero-lcn-source-group");

    edacs_setup_state_fixture(1);
    g_state.edacs_sys_id = 0x1111;
    g_state.edacs_cc_lcn = 6;
    g_state.p25_cc_freq = 852512500L;
    g_state.trunk_cc_freq = 852512500L;
    edacs_process_valid_frame(&g_opts, &g_state, edacs_extended_system_info_msg1(0x7777), 0);
    rc |= edacs_expect(g_state.edacs_sys_id == 0x1111, "extended-system-info-zero-lcn", "state",
                       "zero LCN preserved previous system id");
    rc |= edacs_expect(g_state.edacs_cc_lcn == 6, "extended-system-info-zero-lcn", "state",
                       "zero LCN preserved previous CC LCN");
    rc |= edacs_expect(g_state.p25_cc_freq == 852512500L && g_state.trunk_cc_freq == 852512500L,
                       "extended-system-info-zero-lcn", "state", "zero LCN preserved CC frequencies");

    return rc;
}

static int
edacs_run_helper_contract_cases(void) {
    int rc = 0;
    static dsd_state state;
    static dsd_opts volume_opts;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&volume_opts, 0, sizeof(volume_opts));

    rc |= edacs_expect(strcmp(edacs_lcn_status_string(26), "[Reserved LCN Status]") == 0, "helpers", "lcn-status",
                       "reserved status label");
    rc |= edacs_expect(strcmp(edacs_lcn_status_string(28), "[Convert To Callee]") == 0, "helpers", "lcn-status",
                       "convert-to-callee label");
    rc |= edacs_expect(strcmp(edacs_lcn_status_string(31), "[Call Denied]") == 0, "helpers", "lcn-status",
                       "call-denied label");
    rc |= edacs_expect(edacs_lcn_status_string(12)[0] == '\0', "helpers", "lcn-status",
                       "ordinary LCN has no status label");

    volume_opts.input_volume_multiplier = 0;
    rc |= edacs_expect(edacs_apply_input_volume(&volume_opts, 1234) == 1234, "helpers", "input-volume",
                       "disabled multiplier preserves sample");
    volume_opts.input_volume_multiplier = 3;
    rc |= edacs_expect(edacs_apply_input_volume(&volume_opts, 12000) == 32767, "helpers", "input-volume",
                       "positive multiplier clamps high");
    volume_opts.input_volume_multiplier = 4;
    rc |= edacs_expect(edacs_apply_input_volume(&volume_opts, -12000) == -32768, "helpers", "input-volume",
                       "positive multiplier clamps low");
    volume_opts.input_volume_multiplier = 2;
    rc |= edacs_expect(edacs_apply_input_volume(&volume_opts, 1000) == 2000, "helpers", "input-volume",
                       "positive multiplier scales in range");

    const unsigned long long int frame_a = 0x00F0F0F0F0ULL;
    const unsigned long long int frame_b_inverted = (~0x00F0F0F0F1ULL) & 0xFFFFFFFFFFULL;
    const unsigned long long int frame_c = 0x00F0F0F0F0ULL;
    rc |= edacs_expect(edacs_vote_frames(frame_a, frame_b_inverted, frame_c) == frame_a, "helpers", "vote",
                       "majority vote repairs inverted copy bit");

    rc |= edacs_expect(edacs_update_squelch_count(1.0, 2.0, 5) == 4, "helpers", "squelch",
                       "below-squelch decrements countdown");
    rc |= edacs_expect(edacs_update_squelch_count(3.0, 2.0, 1) == 5, "helpers", "squelch",
                       "above-squelch resets countdown");

    rc |= edacs_expect(edacs_should_release_voice(0xAAAAAAAAAAAAAAAAULL, 0, time(NULL), 20.0) == 1, "helpers",
                       "release", "dotting sequence releases voice");
    rc |= edacs_expect(edacs_should_release_voice(0x0000000000000000ULL, 0, time(NULL), 20.0) == 0, "helpers",
                       "release", "non-dotting with squelch enabled stays active");
    rc |= edacs_expect(edacs_should_release_voice(0x0000000000000000ULL, 1, time(NULL) - 30, 20.0) == 1, "helpers",
                       "release", "disabled-squelch watchdog releases voice");

    edacs_update_lcn_count(&state, 5);
    rc |= edacs_expect(state.edacs_lcn_count == 5, "helpers", "lcn-count", "valid LCN raises count");
    edacs_update_lcn_count(&state, 31);
    rc |= edacs_expect(state.edacs_lcn_count == 5, "helpers", "lcn-count", "reserved status LCN does not raise count");
    edacs_update_lcn_count(&state, 4);
    rc |= edacs_expect(state.edacs_lcn_count == 5, "helpers", "lcn-count", "lower LCN preserves count");

    static const unsigned long long int words[6] = {
        0x0123456789ULL, 0x0FEDCBA987ULL, 0x055AA55AA5ULL, 0x0AA55AA55AULL, 0x0000000000ULL, 0x0FFFFFFFFFULL,
    };
    int edacs_bit[241];
    DSD_MEMSET(edacs_bit, 0, sizeof(edacs_bit));
    for (size_t word = 0U; word < 6U; word++) {
        for (int bit = 0; bit < 40; bit++) {
            edacs_bit[(word * 40U) + (size_t)bit] = (int)((words[word] >> (39 - bit)) & 1ULL);
        }
    }

    unsigned long long int fr_1 = 0ULL;
    unsigned long long int fr_2 = 0ULL;
    unsigned long long int fr_3 = 0ULL;
    unsigned long long int fr_4 = 0ULL;
    unsigned long long int fr_5 = 0ULL;
    unsigned long long int fr_6 = 0ULL;
    edacs_build_raw_frames(edacs_bit, &fr_1, &fr_2, &fr_3, &fr_4, &fr_5, &fr_6);
    rc |= edacs_expect(fr_1 == words[0], "helpers", "raw-frame-build", "first raw frame packed from bits");
    rc |= edacs_expect(fr_2 == words[1], "helpers", "raw-frame-build", "second raw frame packed from bits");
    rc |= edacs_expect(fr_3 == words[2], "helpers", "raw-frame-build", "third raw frame packed from bits");
    rc |= edacs_expect(fr_4 == words[3], "helpers", "raw-frame-build", "fourth raw frame packed from bits");
    rc |= edacs_expect(fr_5 == words[4], "helpers", "raw-frame-build", "fifth raw frame packed from bits");
    rc |= edacs_expect(fr_6 == words[5], "helpers", "raw-frame-build", "sixth raw frame packed from bits");

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    static int dibit_buf[900256];
    static int payload_buf[900256];
    DSD_MEMSET(dibit_buf, 0, sizeof(dibit_buf));
    DSD_MEMSET(payload_buf, 0, sizeof(payload_buf));
    state.dibit_buf = dibit_buf;
    state.dmr_payload_buf = payload_buf;
    state.synctype = DSD_SYNC_EDACS_POS;
    state.center = 0;
    state.dibit_buf_p = state.dibit_buf;
    state.dmr_payload_p = state.dmr_payload_buf;
    static short analog[960];
    unsigned long long int expected_sr = 0ULL;
    for (int sample = 0; sample < 960; sample++) {
        analog[sample] = 0;
    }
    for (int bit = 0; bit < 192; bit++) {
        const int value = (bit % 3) == 0 ? 700 : -700;
        analog[bit * 5] = (short)value;
        expected_sr = (expected_sr << 1U) | (unsigned long long int)(value > state.center ? 0U : 1U);
    }

    const unsigned long long int sr = edacs_build_symbol_register(&opts, &state, analog);
    rc |= edacs_expect(sr == expected_sr, "helpers", "symbol-register", "register packed digitized analog symbols");
    rc |= edacs_expect(state.dibit_buf_p == state.dibit_buf + 192, "helpers", "symbol-register",
                       "digitizer advanced dibit buffer once per symbol");
    for (int bit = 0; bit < 192; bit++) {
        const int want_stored_dibit = analog[bit * 5] > state.center ? 1 : 3;
        rc |= edacs_expect(state.dibit_buf[bit] == want_stored_dibit, "helpers", "symbol-register",
                           "stored two-level EDACS dibit");
    }
    rc |= edacs_expect(edacs_build_symbol_register(NULL, &state, analog) == 0ULL, "helpers", "symbol-register",
                       "null opts guard returns zero");

    state.dibit_buf_p = state.dibit_buf + 900001;
    state.dmr_payload_p = state.dmr_payload_buf + 900005;
    edacs_reset_digitize_overflow(&state);
    rc |= edacs_expect(state.dibit_buf_p == state.dibit_buf + 200, "helpers", "overflow-reset",
                       "large dibit pointer reset to guard offset");
    rc |= edacs_expect(state.dmr_payload_p == state.dmr_payload_buf + 200, "helpers", "overflow-reset",
                       "large payload pointer reset to guard offset");
    state.dibit_buf_p = state.dibit_buf + 199;
    state.dmr_payload_p = state.dmr_payload_buf + 199;
    edacs_reset_digitize_overflow(&state);
    rc |= edacs_expect(state.dibit_buf_p == state.dibit_buf + 199, "helpers", "overflow-reset",
                       "in-range dibit pointer preserved");
    rc |= edacs_expect(state.dmr_payload_p == state.dmr_payload_buf + 199, "helpers", "overflow-reset",
                       "in-range payload pointer preserved");

    return rc;
}

static int
edacs_run_analog_loop_helper_cases(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static short analog1[960];
    static short analog2[960];
    static short analog3[960];
    double pwr = -1.0;
    static int tcp_ctx_token;
    tcp_ctx_token = 0xEAA5;

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(analog1, 0, sizeof(analog1));
    DSD_MEMSET(analog2, 0, sizeof(analog2));
    DSD_MEMSET(analog3, 0, sizeof(analog3));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.input_volume_multiplier = 2;
    opts.tcp_in_ctx = (tcp_input_ctx*)&tcp_ctx_token;
    edacs_install_net_audio_hooks();

    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 1,
                       "analog-helpers", "tcp-collect", "TCP triplet collection succeeded");
    rc |= edacs_expect(g_tcp_read_count == 2880, "analog-helpers", "tcp-collect", "TCP read exactly three blocks");
    rc |= edacs_expect(g_tcp_close_count == 0, "analog-helpers", "tcp-collect", "TCP success did not close input");
    rc |= edacs_expect(analog1[0] == 2000 && analog1[959] == 3918 && analog2[0] == 3920 && analog3[959] == 7758,
                       "analog-helpers", "tcp-collect", "TCP samples preserved block ordering and volume scaling");
    rc |= edacs_expect(pwr > 0.0, "analog-helpers", "tcp-collect", "TCP collection updated power");

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_UDP;
    edacs_install_net_audio_hooks();
    pwr = -1.0;
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 1,
                       "analog-helpers", "udp-collect", "UDP triplet collection succeeded");
    rc |= edacs_expect(g_udp_read_count == 2880, "analog-helpers", "udp-collect", "UDP read exactly three blocks");
    rc |= edacs_expect(analog1[0] == 2000 && analog1[1] == 2001 && analog2[0] == 2960, "analog-helpers", "udp-collect",
                       "UDP samples preserve block ordering");
    rc |= edacs_expect(pwr > 0.0, "analog-helpers", "udp-collect", "UDP collection updated power");

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_UDP;
    g_udp_fail_every = 4;
    edacs_install_net_audio_hooks();
    pwr = -1.0;
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 0,
                       "analog-helpers", "udp-stop", "stopped UDP input is rejected");
    rc |= edacs_expect(g_udp_read_count == 1 && dsd_exitflag_load() != 0, "analog-helpers", "udp-stop",
                       "stopped UDP input requests shutdown immediately");

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    for (size_t i = 0; i < 960U; i++) {
        analog1[i] = 111;
        analog2[i] = 222;
        analog3[i] = 333;
    }
    opts.audio_in_type = AUDIO_IN_WAV;
    pwr = -1.0;
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 0,
                       "analog-helpers", "unsupported-input", "unsupported input is rejected");
    rc |= edacs_expect(analog1[0] == 111 && analog2[0] == 222 && analog3[0] == 333 && pwr == -1.0, "analog-helpers",
                       "unsupported-input", "rejected input leaves outputs unchanged");

#ifdef USE_RADIO
    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_volume_multiplier = 2;
    state.rtl_ctx = (struct RtlSdrContext*)&tcp_ctx_token;
    edacs_install_rtl_stream_hooks();
    pwr = -1.0;
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 1,
                       "analog-helpers", "rtl-collect", "RTL triplet collection succeeded");
    rc |= edacs_expect(g_rtl_read_count == 2880, "analog-helpers", "rtl-collect", "RTL read exactly three blocks");
    rc |= edacs_expect(g_rtl_return_pwr_count == 1 && pwr == 77.25, "analog-helpers", "rtl-collect",
                       "RTL collection used squelch power hook");
    rc |= edacs_expect(analog1[0] == 100 && analog2[0] == 1060 && analog3[959] == 2979, "analog-helpers", "rtl-collect",
                       "RTL samples preserved block ordering, with no volume trim on the FSK output");
    /* The FSK discriminator output peaks near +/-30000, which fits int16: the volume trim (2 here) used to double it
       and clip the upper half of the waveform before the analog audio chain saw it (issue #616). */
    edacs_reset_audio_hook_state();
    g_rtl_read_value = 30000.0f;
    edacs_install_rtl_stream_hooks();
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 1,
                       "analog-helpers", "rtl-unclipped", "RTL triplet collection succeeded");
    rc |= edacs_expect(analog1[0] == 30000 && analog2[480] == 30000 && analog3[959] == 30000, "analog-helpers",
                       "rtl-unclipped", "an FSK peak near full scale reaches the chain unclipped");
    g_rtl_read_value = 0.0f;
#endif

    static short wav_src[960];
    static short wav_out[320];
    for (int i = 0; i < 960; i++) {
        wav_src[i] = (short)i;
    }
    DSD_MEMSET(wav_out, 0, sizeof(wav_out));
    rc |= edacs_expect(edacs_build_static_wav_block(wav_src, wav_out, 320U) == 0, "analog-helpers", "static-wav",
                       "static WAV downsample helper accepted full output");
    rc |= edacs_expect(wav_out[0] == 0 && wav_out[1] == 0 && wav_out[2] == 6 && wav_out[3] == 6 && wav_out[318] == 954
                           && wav_out[319] == 954,
                       "analog-helpers", "static-wav", "static WAV helper picked every sixth sample as stereo");
    wav_out[0] = -123;
    rc |= edacs_expect(edacs_build_static_wav_block(wav_src, wav_out, 319U) == -1 && wav_out[0] == -123,
                       "analog-helpers", "static-wav", "short output buffer is rejected without mutation");

    rc |= edacs_expect(edacs_no_sql_watchdog_window(0.5) == 20.0, "analog-helpers", "watchdog",
                       "No-SQL watchdog clamps low hangtime");
    rc |= edacs_expect(edacs_no_sql_watchdog_window(4.5) == 45.0, "analog-helpers", "watchdog",
                       "No-SQL watchdog preserves midrange hangtime");
    rc |= edacs_expect(edacs_no_sql_watchdog_window(8.0) == 60.0, "analog-helpers", "watchdog",
                       "No-SQL watchdog clamps high hangtime");
    rc |= edacs_expect(edacs_should_release_voice(0x0000000000000000ULL, 1, time(NULL) - 5, 20.0) == 0,
                       "analog-helpers", "watchdog", "No-SQL watchdog does not release before window");

    static short out1[960];
    static short out2[960];
    static short out3[960];
    for (int i = 0; i < 960; i++) {
        out1[i] = (short)(11 + i);
        out2[i] = (short)(22 + i);
        out3[i] = (short)(33 + i);
    }

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    edacs_install_udp_output_hooks();
    edacs_emit_analog_audio(&opts, &state, out1, out2, out3, 1);
    rc |= edacs_expect(g_udp_blast_count == 3, "analog-helpers", "udp-output", "UDP output emitted three blocks");
    rc |= edacs_expect(g_udp_blast_bytes[0] == 1920U && g_udp_blast_bytes[1] == 1920U && g_udp_blast_bytes[2] == 1920U,
                       "analog-helpers", "udp-output", "UDP output emitted 960 short samples per block");
    rc |= edacs_expect(g_udp_blast_first[0] == 11 && g_udp_blast_first[1] == 22 && g_udp_blast_first[2] == 33,
                       "analog-helpers", "udp-output", "UDP output preserved block order");

    char raw_path[] = "dsdneo_edacs_raw_XXXXXX";
    int raw_fd = dsd_mkstemp(raw_path);
    if (raw_fd < 0) {
        rc |= edacs_expect(0, "analog-helpers", "raw-output", "created temporary raw output");
    } else {
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.audio_out_type = 1;
        opts.audio_out = 1;
        opts.floating_point = 0;
        opts.slot1_on = 1;
        opts.audio_out_fd = raw_fd;
        edacs_emit_analog_audio(&opts, &state, out1, out2, out3, 1);
        dsd_stat_t st;
        DSD_MEMSET(&st, 0, sizeof(st));
        rc |= edacs_expect(dsd_fstat(raw_fd, &st) == 0 && (long long)st.st_size == 5760LL, "analog-helpers",
                           "raw-output", "raw fd output wrote three 960-sample blocks");
        (void)dsd_close(raw_fd);
        FILE* fp = fopen(raw_path, "rb");
        if (fp == NULL) {
            rc |= edacs_expect(0, "analog-helpers", "raw-output", "reopened raw output for verification");
        } else {
            short first1 = 0;
            short first2 = 0;
            short first3 = 0;
            rc |= edacs_expect(fread(&first1, sizeof(first1), 1U, fp) == 1U, "analog-helpers", "raw-output",
                               "read first raw block sample");
            rc |= edacs_expect(fseek(fp, (long)(960U * sizeof(short)), SEEK_SET) == 0
                                   && fread(&first2, sizeof(first2), 1U, fp) == 1U,
                               "analog-helpers", "raw-output", "read second raw block sample");
            rc |= edacs_expect(fseek(fp, (long)(1920U * sizeof(short)), SEEK_SET) == 0
                                   && fread(&first3, sizeof(first3), 1U, fp) == 1U,
                               "analog-helpers", "raw-output", "read third raw block sample");
            rc |= edacs_expect(first1 == 11 && first2 == 22 && first3 == 33, "analog-helpers", "raw-output",
                               "raw fd output preserved block order");
            (void)fclose(fp);
        }
        (void)remove(raw_path);
    }

    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_TCP;
    opts.tcp_in_ctx = (tcp_input_ctx*)&tcp_ctx_token;
    g_tcp_fail_at = 5;
    edacs_install_net_audio_hooks();
    pwr = 123.0;
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 0,
                       "analog-helpers", "tcp-cleanup", "TCP read failure aborts collection");
    rc |= edacs_expect(g_tcp_read_count == 6 && g_tcp_close_count == 1 && opts.tcp_in_ctx == NULL, "analog-helpers",
                       "tcp-cleanup", "TCP read failure closes and clears input context");
    rc |=
        edacs_expect(dsd_exitflag_load() == 1, "analog-helpers", "tcp-cleanup", "TCP read failure requested shutdown");

#ifdef USE_RADIO
    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_volume_multiplier = 1;
    state.rtl_ctx = (struct RtlSdrContext*)&tcp_ctx_token;
    g_rtl_fail_at = 3;
    edacs_install_rtl_stream_hooks();
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, NULL, analog1, analog2, analog3, &pwr) == 0,
                       "analog-helpers", "rtl-cleanup", "RTL read failure aborts collection");
    rc |= edacs_expect(g_rtl_read_count == 4 && dsd_exitflag_load() == 1, "analog-helpers", "rtl-cleanup",
                       "RTL read failure requested shutdown after bounded reads");
#endif

    edacs_reset_audio_hook_state();
    return rc;
}

/* Analog voice never passes the vocoder output gates, so EDACS applies the
 * talkgroup policy at its own output: a blocked call is neither played nor
 * recorded, an allowed one is both. */
static int
test_edacs_analog_media_honors_talkgroup_policy(void) {
    static dsd_opts opts;
    static dsd_state state;
    static short analog1[960];
    static short analog2[960];
    static short analog3[960];
    for (int i = 0; i < 960; i++) {
        analog1[i] = (short)(11 + i);
        analog2[i] = (short)(22 + i);
        analog3[i] = (short)(33 + i);
    }
    int rc = 0;
    for (int blocked = 1; blocked >= 0; --blocked) {
        edacs_reset_audio_hook_state();
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.audio_out = 1;
        opts.audio_out_type = 8;
        opts.slot1_on = 1;
        edacs_install_udp_output_hooks();
        const dsd_call_observation call = {.protocol = DSD_SYNC_EDACS_POS,
                                           .slot = 0U,
                                           .kind = DSD_CALL_KIND_GROUP_VOICE,
                                           .ota_target_id = 321U,
                                           .policy_target_id = 321U,
                                           .ota_source_id = 1234U};
        rc |= edacs_expect(dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1, "analog-policy", "call",
                           "seeded the analog call");
        if (blocked) {
            rc |= edacs_expect(dsd_tg_policy_set_mode(&state, 321, 321, "B") == 0, "analog-policy", "row",
                               "blocked the analog talkgroup");
        }
        edacs_emit_analog_audio(&opts, &state, analog1, analog2, analog3, 1);
        rc |= edacs_expect(g_udp_blast_count == (blocked ? 0 : 3), "analog-policy", "audio",
                           blocked ? "blocked analog call is not played" : "allowed analog call plays");

        char wav_path[] = "dsdneo_edacs_policy_wav_XXXXXX";
        const int wav_fd = dsd_mkstemp(wav_path);
        if (wav_fd < 0) {
            rc |= edacs_expect(0, "analog-policy", "wav", "created temporary wav");
            dsd_state_ext_free_all(&state);
            continue;
        }
        (void)dsd_close(wav_fd);
        SF_INFO info;
        DSD_MEMSET(&info, 0, sizeof(info));
        info.samplerate = 48000;
        info.channels = 1;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        opts.wav_out_f = sf_open(wav_path, SFM_WRITE, &info);
        opts.dmr_stereo_wav = 1;
        rc |= edacs_expect(opts.wav_out_f != NULL, "analog-policy", "wav", "opened temporary wav");
        if (opts.wav_out_f != NULL) {
            edacs_write_analog_wav(&opts, &state, analog1, analog2, analog3);
            sf_close(opts.wav_out_f);
            opts.wav_out_f = NULL;
            DSD_MEMSET(&info, 0, sizeof(info));
            SNDFILE* written = sf_open(wav_path, SFM_READ, &info);
            if (written != NULL) {
                sf_close(written);
            }
            rc |= edacs_expect((written != NULL && info.frames > 0) == !blocked, "analog-policy", "wav",
                               blocked ? "blocked analog call is not recorded" : "allowed analog call records");
        }
        (void)remove(wav_path);
        dsd_state_ext_free_all(&state);
    }
    edacs_reset_audio_hook_state();
    return rc;
}

/* Issue #574: EDACS analog voice stamps audible audio when it emits a triplet carrying squelch-open samples, after its
   talkgroup, mute and slot gates and before it picks the output: the local stream, the raw fd and UDP alike. A triplet
   whose every sample the squelch closed is still written, as silence, and stamps nothing. */
static int
test_edacs_analog_emit_stamps_open_reception(void) {
    static const struct {
        const char* tag;
        int squelch_open;
        int blocked;
        int audio_out;
        int slot1_on;
        int want_blocks;
        int want_stamp;
    } cases[] = {
        {"open triplet", 1, 0, 1, 1, 3, 1},      {"closed triplet", 0, 0, 1, 1, 3, 0},
        {"blocked talkgroup", 1, 1, 1, 1, 0, 0}, {"muted", 1, 0, 0, 1, 0, 0},
        {"slot off", 1, 0, 1, 0, 0, 0},
    };

    static const int out_types[] = {0, 1, 8};
    static dsd_opts opts;
    static dsd_state state;
    static short analog1[960];
    static short analog2[960];
    static short analog3[960];
    int rc = 0;
    dsd_audio_activity_arm();
    for (size_t o = 0U; o < sizeof(out_types) / sizeof(out_types[0]); o++) {
        for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
            edacs_reset_audio_hook_state();
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            opts.audio_out = cases[c].audio_out;
            opts.slot1_on = cases[c].slot1_on;
            opts.audio_out_type = out_types[o];
            /* The local stream is open; the capture takes its writes. */
            opts.audio_raw_out = out_types[o] == 0 ? (dsd_audio_stream*)&g_audio_write_capture : NULL;
            edacs_install_udp_output_hooks();
            if (cases[c].blocked) {
                const dsd_call_observation call = {.protocol = DSD_SYNC_EDACS_POS,
                                                   .slot = 0U,
                                                   .kind = DSD_CALL_KIND_GROUP_VOICE,
                                                   .ota_target_id = 321U,
                                                   .policy_target_id = 321U,
                                                   .ota_source_id = 1234U};
                rc |= edacs_expect(dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1, "analog-stamp",
                                   "call", "seeded the analog call");
                rc |= edacs_expect(dsd_tg_policy_set_mode(&state, 321, 321, "B") == 0, "analog-stamp", "row",
                                   "blocked the analog talkgroup");
            }
            char raw_path[DSD_TEST_PATH_MAX];
            int raw_fd = -1;
            if (out_types[o] == 1) {
                raw_fd = dsd_test_mkstemp(raw_path, sizeof(raw_path), "dsdneo_edacs_stamp");
                if (raw_fd < 0) {
                    rc |= edacs_expect(0, "analog-stamp", "fd", "created temporary raw output");
                    dsd_state_ext_free_all(&state);
                    continue;
                }
                opts.audio_out_fd = raw_fd;
            }
            g_audio_write_count = 0;
            g_audio_write_capture = out_types[o] == 0;
            dsd_audio_activity_reset();

            edacs_emit_analog_audio(&opts, &state, analog1, analog2, analog3, cases[c].squelch_open);

            g_audio_write_capture = 0;
            int blocks = 0;
            if (out_types[o] == 0) {
                blocks = g_audio_write_count;
            } else if (out_types[o] == 8) {
                blocks = g_udp_blast_count;
            } else {
                dsd_stat_t st;
                DSD_MEMSET(&st, 0, sizeof(st));
                if (dsd_fstat(raw_fd, &st) == 0) {
                    blocks = (int)((long long)st.st_size / (long long)(960U * sizeof(short)));
                }
                (void)dsd_close(raw_fd);
                (void)remove(raw_path);
            }
            uint64_t stamp = 0U;
            dsd_audio_activity_read(&stamp, NULL);
            if (blocks != cases[c].want_blocks || (stamp != 0U) != cases[c].want_stamp) {
                DSD_FPRINTF(stderr, "output %d %s: %d blocks (want %d), stamp %d (want %d)\n", out_types[o],
                            cases[c].tag, blocks, cases[c].want_blocks, stamp != 0U, cases[c].want_stamp);
            }
            rc |= edacs_expect(blocks == cases[c].want_blocks, "analog-stamp", cases[c].tag,
                               "the triplet reaches the output its gates allow");
            rc |= edacs_expect((stamp != 0U) == cases[c].want_stamp, "analog-stamp", cases[c].tag,
                               "only an emitted triplet with open samples stamps");
            dsd_state_ext_free_all(&state);
        }
    }
    edacs_reset_audio_hook_state();
    dsd_audio_activity_reset();
    return rc;
}

/* Issue #574: an open triplet stamps only where an output receives it. The raw fd takes 16-bit samples only, so with
   float output selected (floating_point 1) it writes nothing and nothing stamps, while the local stream and UDP write
   and stamp in either format; an output type none of the three serves (the null output, 9) writes and stamps nothing
   either, nor does a local stream that is not open, whose writes the platform refuses. */
static int
test_edacs_analog_stamp_follows_the_writer(void) {
    static const struct {
        int out_type;
        int floating_point;
        int stream_open;
        int want_blocks;
    } cases[] = {
        {0, 0, 1, 3}, {0, 1, 1, 3}, {0, 0, 0, 0}, {1, 0, 1, 3}, {1, 1, 1, 0}, {8, 0, 1, 3}, {8, 1, 1, 3}, {9, 0, 1, 0},
    };

    static dsd_opts opts;
    static dsd_state state;
    static short analog1[960];
    static short analog2[960];
    static short analog3[960];
    int rc = 0;
    dsd_audio_activity_arm();
    for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
        edacs_reset_audio_hook_state();
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.audio_out = 1;
        opts.slot1_on = 1;
        opts.audio_out_type = cases[c].out_type;
        opts.floating_point = cases[c].floating_point;
        /* An open local stream's writes go to the capture; with none, the platform's own write refuses them. */
        opts.audio_raw_out = cases[c].stream_open ? (dsd_audio_stream*)&g_audio_write_capture : NULL;
        edacs_install_udp_output_hooks();
        char raw_path[DSD_TEST_PATH_MAX];
        int raw_fd = -1;
        if (cases[c].out_type == 1) {
            raw_fd = dsd_test_mkstemp(raw_path, sizeof(raw_path), "dsdneo_edacs_writer");
            if (raw_fd < 0) {
                rc |= edacs_expect(0, "analog-stamp-writer", "fd", "created temporary raw output");
                continue;
            }
            opts.audio_out_fd = raw_fd;
        }
        g_audio_write_count = 0;
        g_audio_write_capture = cases[c].out_type == 0 && cases[c].stream_open;
        dsd_audio_activity_reset();

        edacs_emit_analog_audio(&opts, &state, analog1, analog2, analog3, 1);

        g_audio_write_capture = 0;
        int blocks = g_audio_write_count + g_udp_blast_count;
        if (raw_fd >= 0) {
            dsd_stat_t st;
            DSD_MEMSET(&st, 0, sizeof(st));
            if (dsd_fstat(raw_fd, &st) == 0) {
                blocks += (int)((long long)st.st_size / (long long)(960U * sizeof(short)));
            }
            (void)dsd_close(raw_fd);
            (void)remove(raw_path);
        }
        uint64_t stamp = 0U;
        dsd_audio_activity_read(&stamp, NULL);
        const int want_stamp = cases[c].want_blocks > 0;
        if (blocks != cases[c].want_blocks || (stamp != 0U) != want_stamp) {
            DSD_FPRINTF(stderr,
                        "output %d, floating point %d, stream open %d: %d blocks (want %d), stamp %d (want %d)\n",
                        cases[c].out_type, cases[c].floating_point, cases[c].stream_open, blocks, cases[c].want_blocks,
                        stamp != 0U, want_stamp);
        }
        rc |= edacs_expect(blocks == cases[c].want_blocks, "analog-stamp-writer", "blocks",
                           "the triplet reaches each output that takes its format");
        rc |= edacs_expect((stamp != 0U) == want_stamp, "analog-stamp-writer", "stamp",
                           "only a triplet some output writes stamps");
        dsd_state_ext_free_all(&state);
    }
    edacs_reset_audio_hook_state();
    dsd_audio_activity_reset();
    return rc;
}

/* Issue #574: the mixers write to no output type while muted (audio_out 0) or while slot 1 is switched off
   (dsd_output_*_block() and the mono mixers), so each of EDACS's analog outputs -- the local stream, the raw fd and the
   UDP socket -- applies both, as one rule. */
static int
test_edacs_analog_output_honors_mute_and_slot(void) {
    static const struct {
        const char* tag;
        int audio_out;
        int slot1_on;
    } gates[] = {
        {"on", 1, 1},
        {"muted", 0, 1},
        {"slot off", 1, 0},
    };

    static dsd_opts opts;
    static dsd_state state;
    static short analog1[960];
    static short analog2[960];
    static short analog3[960];
    for (int i = 0; i < 960; i++) {
        analog1[i] = (short)(11 + i);
        analog2[i] = (short)(22 + i);
        analog3[i] = (short)(33 + i);
    }
    static const int out_types[] = {0, 1, 8};
    int rc = 0;
    for (size_t o = 0U; o < sizeof(out_types) / sizeof(out_types[0]); o++) {
        for (size_t g = 0U; g < sizeof(gates) / sizeof(gates[0]); g++) {
            edacs_reset_audio_hook_state();
            DSD_MEMSET(&opts, 0, sizeof(opts));
            DSD_MEMSET(&state, 0, sizeof(state));
            opts.audio_out = gates[g].audio_out;
            opts.slot1_on = gates[g].slot1_on;
            opts.audio_out_type = out_types[o];
            edacs_install_udp_output_hooks();
            char raw_path[DSD_TEST_PATH_MAX];
            int raw_fd = -1;
            if (out_types[o] == 1) {
                raw_fd = dsd_test_mkstemp(raw_path, sizeof(raw_path), "dsdneo_edacs_gate");
                if (raw_fd < 0) {
                    rc |= edacs_expect(0, "analog-574", "gate", "created temporary raw output");
                    continue;
                }
                opts.audio_out_fd = raw_fd;
            }
            g_audio_write_count = 0;
            g_audio_write_capture = out_types[o] == 0;

            edacs_emit_analog_audio(&opts, &state, analog1, analog2, analog3, 1);

            g_audio_write_capture = 0;
            int blocks = 0;
            if (out_types[o] == 0) {
                blocks = g_audio_write_count;
            } else if (out_types[o] == 8) {
                blocks = g_udp_blast_count;
            } else {
                dsd_stat_t st;
                DSD_MEMSET(&st, 0, sizeof(st));
                if (dsd_fstat(raw_fd, &st) == 0) {
                    blocks = (int)((long long)st.st_size / (long long)(960U * sizeof(short)));
                }
                (void)dsd_close(raw_fd);
                (void)remove(raw_path);
            }
            const int want = gates[g].audio_out == 1 && gates[g].slot1_on == 1 ? 3 : 0;
            if (blocks != want) {
                DSD_FPRINTF(stderr, "output %d %s: %d blocks, want %d\n", out_types[o], gates[g].tag, blocks, want);
            }
            rc |= edacs_expect(blocks == want, "analog-574", gates[g].tag,
                               "each analog output plays only while unmuted with slot 1 on");
            dsd_state_ext_free_all(&state);
        }
    }
    edacs_reset_audio_hook_state();
    return rc;
}

/* The analog-call squelch helpers (issue #625). */
static int
edacs_run_analog_sql_helper_cases(void) {
    int rc = 0;
    const uint8_t c = DSD_SQUELCH_FLAG_CLOSED;
    const uint8_t open_then_closed[4] = {0U, c, c, c};
    const uint8_t closed2[2] = {c, c};
    rc |= edacs_expect(edacs_gate_closed_run(9U, open_then_closed, 4U) == 3U, "analog-sql", "run",
                       "an open sample restarts the closed run");
    rc |= edacs_expect(edacs_gate_closed_run(5U, closed2, 2U) == 7U, "analog-sql", "run",
                       "closed samples carry the run across triplets");
    rc |= edacs_expect(edacs_gate_closed_run(5U, NULL, 2U) == 0U, "analog-sql", "run", "no flags, no run");
    const size_t triplet = (size_t)EDACS_ANALOG_TRIPLET_SAMPLES;
    rc |= edacs_expect(edacs_gate_hold_samples(48000) == 4U * triplet - 4080U, "analog-sql", "hold",
                       "four triplets less 85 ms at 48 kHz");
    rc |= edacs_expect(edacs_gate_hold_samples(24000) == 4U * triplet - 2040U, "analog-sql", "hold",
                       "four triplets less 85 ms at 24 kHz");
    rc |= edacs_expect(edacs_gate_hold_samples(0) == 4U * triplet, "analog-sql", "hold", "no rate, no delay");
    rc |= edacs_expect(edacs_gate_hold_samples(125000) == 10000U, "analog-sql", "hold",
                       "at 125 kHz the floor holds two windows (80 ms)");
    rc |= edacs_expect(edacs_gate_hold_samples(1000000) == 80000U, "analog-sql", "hold", "never under two windows");
    const size_t hold = edacs_gate_hold_samples(48000);
    rc |= edacs_expect(edacs_gate_count(0U, hold) == 5 && edacs_gate_count(triplet, hold) == 4
                           && edacs_gate_count(hold - 1U, hold) == 3 && edacs_gate_count(hold, hold) == 0,
                       "analog-sql", "count", "the level path's count from the closed run");

    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    rc |= edacs_expect(edacs_analog_sql_kind(&opts, 1) == EDACS_ANALOG_SQL_GATE, "analog-sql", "kind",
                       "AUTO on a radio input whose stream runs the gate");
    rc |= edacs_expect(edacs_analog_sql_kind(&opts, 0) == EDACS_ANALOG_SQL_NONE, "analog-sql", "kind",
                       "AUTO with no gate running: none");
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    rc |= edacs_expect(edacs_analog_sql_kind(&opts, 1) == EDACS_ANALOG_SQL_GATE, "analog-sql", "kind",
                       "NOISE runs as AUTO's gate");
    opts.audio_in_type = AUDIO_IN_UDP;
    rc |= edacs_expect(edacs_analog_sql_kind(&opts, 1) == EDACS_ANALOG_SQL_NONE, "analog-sql", "kind",
                       "a dynamic setting on audio input: none");
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_level = dB_to_pwr(-60.0);
    rc |= edacs_expect(edacs_analog_sql_kind(&opts, 0) == EDACS_ANALOG_SQL_LEVEL, "analog-sql", "kind",
                       "a level above 0");
    opts.rtl_squelch_level = 0.0;
    rc |=
        edacs_expect(edacs_analog_sql_kind(&opts, 1) == EDACS_ANALOG_SQL_NONE, "analog-sql", "kind", "level off: none");
    rc |= edacs_expect(edacs_analog_sql_kind(NULL, 1) == EDACS_ANALOG_SQL_NONE, "analog-sql", "kind", "NULL");
    return rc;
}

/* The hashes of a 48 kHz call's outputs, from the code before issue #633 (edacs_golden_call()). */
#define EDACS_GOLDEN_UDP_WAV    0xA172D3D3E27E68FBULL
#define EDACS_GOLDEN_STATIC_WAV 0x9CD69F783969D5FEULL
#define EDACS_GOLDEN_FD         0xDEF38AC945E89533ULL

/* --- Issue #633: EDACS analog voice runs at 48 kHz, whatever rate it is read at --- */

static int g_edacs_dibit_buf[900256];
static int g_edacs_payload_buf[900256];
static int g_edacs_tcp_token = 0xEAA5;

static void
edacs_attach_dibit_buffers(dsd_state* state) {
    state->dibit_buf = g_edacs_dibit_buf;
    state->dmr_payload_buf = g_edacs_payload_buf;
    state->dibit_buf_p = state->dibit_buf + 200;
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    state->synctype = DSD_SYNC_EDACS_POS;
    state->center = 0;
}

/* The converting collector reads the input at its own rate and hands back three blocks of 960 at 48 kHz: half as many
   reads at 24 kHz, 2646 at 44.1 kHz (the ramp input comes out with the slope scaled to match), and the 9600-baud
   dotting the release register looks for survives the conversion from 44.1 and 96 kHz. */
static int
edacs_run_converted_collect_cases(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    static short analog1[960];
    static short analog2[960];
    static short analog3[960];

    static const struct {
        int rate_hz;
        int want_reads;
        double want_slope;
    } ramps[] = {{24000, 1440, 0.5}, {44100, 2646, 44100.0 / 48000.0}};

    for (size_t r = 0; r < sizeof(ramps) / sizeof(ramps[0]); r++) {
        edacs_reset_audio_hook_state();
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        opts.audio_in_type = AUDIO_IN_TCP;
        opts.wav_sample_rate = ramps[r].rate_hz;
        opts.tcp_in_ctx = (tcp_input_ctx*)&g_edacs_tcp_token;
        edacs_install_net_audio_hooks();
        rc |= edacs_expect(edacs_analog_input_rate_hz(&opts) == ramps[r].rate_hz, "analog-633", "input-rate",
                           "TCP analog voice is read at the raw input rate, not the staged one");
        dsd_rate_converter conv;
        dsd_rate_converter_init(&conv);
        rc |= edacs_expect(dsd_rate_converter_configure(&conv, ramps[r].rate_hz, EDACS_ANALOG_RATE_HZ)
                               == DSD_RATE_CONVERTER_CONVERTING,
                           "analog-633", "converter", "the input rate converts to 48 kHz");
        double pwr = -1.0;
        rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, &conv, analog1, analog2, analog3, &pwr) == 1,
                           "analog-633", "collect", "converted triplet collected");
        rc |= edacs_expect(g_tcp_read_count == ramps[r].want_reads, "analog-633", "collect",
                           "three 48 kHz blocks read as many inputs as they span");
        const double slope = (double)(analog2[600] - analog2[100]) / 500.0;
        rc |= edacs_expect(fabs(slope - ramps[r].want_slope) < 0.01, "analog-633", "collect",
                           "the input ramp comes out at the 48 kHz slope");
        rc |= edacs_expect(pwr > 0.0, "analog-633", "collect", "converted collection updated power");
        dsd_rate_converter_free(&conv);
    }

    static const int dotting_rates[] = {44100, 96000};
    for (size_t r = 0; r < sizeof(dotting_rates) / sizeof(dotting_rates[0]); r++) {
        edacs_reset_audio_hook_state();
        DSD_MEMSET(&opts, 0, sizeof(opts));
        DSD_MEMSET(&state, 0, sizeof(state));
        edacs_attach_dibit_buffers(&state);
        opts.audio_in_type = AUDIO_IN_TCP;
        opts.wav_sample_rate = dotting_rates[r];
        opts.tcp_in_ctx = (tcp_input_ctx*)&g_edacs_tcp_token;
        g_tcp_signal = EDACS_TCP_DOTTING;
        g_tcp_signal_rate_hz = dotting_rates[r];
        edacs_install_net_audio_hooks();
        dsd_rate_converter conv;
        dsd_rate_converter_init(&conv);
        (void)dsd_rate_converter_configure(&conv, dotting_rates[r], EDACS_ANALOG_RATE_HZ);
        double pwr = 0.0;
        rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, &conv, analog1, analog2, analog3, &pwr) == 1,
                           "analog-633", "dotting", "converted dotting collected");
        const unsigned long long int sr = edacs_build_symbol_register(&opts, &state, analog2);
        rc |= edacs_expect(edacs_should_release_voice(sr, 0, time(NULL), 20.0) == 1, "analog-633", "dotting",
                           "dotting read at 44.1 or 96 kHz still releases the call");
        dsd_rate_converter_free(&conv);
    }

    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.audio_in_type = AUDIO_IN_PULSE;
    opts.pulse_digi_rate_in = 48000;
    rc |= edacs_expect(edacs_analog_input_rate_hz(&opts) == EDACS_ANALOG_RATE_HZ, "analog-633", "input-rate",
                       "Pulse analog voice is read at 48 kHz");
#ifdef USE_RADIO
    edacs_reset_audio_hook_state();
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_RTL;
    state.rtl_ctx = (struct RtlSdrContext*)&g_edacs_tcp_token;
    edacs_install_rtl_stream_hooks();
    edacs_install_rtl_rate_hook(24000U);
    rc |= edacs_expect(edacs_analog_input_rate_hz(&opts) == 24000, "analog-633", "rtl-rate",
                       "RTL analog voice is read at the stream's output rate");
    dsd_rate_converter conv;
    dsd_rate_converter_init(&conv);
    (void)dsd_rate_converter_configure(&conv, edacs_analog_input_rate_hz(&opts), EDACS_ANALOG_RATE_HZ);
    double pwr = 0.0;
    rc |= edacs_expect(edacs_collect_analog_triplet(&opts, &state, &conv, analog1, analog2, analog3, &pwr) == 1
                           && g_rtl_read_count == 1440,
                       "analog-633", "rtl-rate", "a 24 kHz RTL stream reads 1440 samples a triplet");
    dsd_rate_converter_free(&conv);
    edacs_install_rtl_rate_hook(0U);
#endif
    edacs_reset_audio_hook_state();
    return rc;
}

/* An analog individual call on LCN 6 over TCP input at @p rate_hz, through the grant: the tune lands with
   @p tune_result. edacs_start_tcp_analog_call() sends the grant; edacs_analog() then reads triplets until it releases,
   leaves, or the input ends at g_tcp_fail_at. Between the two a case may adjust g_opts. */
static const edacs_grant_case*
edacs_prepare_tcp_analog_call(int rate_hz, dsd_trunk_tune_result tune_result) {
    static edacs_grant_case call;
    call = (edacs_grant_case){"analog-individual",
                              edacs_standard_individual_msg1(6, 4321, 0),
                              2345ULL,
                              852512500L,
                              0,
                              6,
                              EDACS_IS_VOICE | EDACS_IS_INDIVIDUAL,
                              4321,
                              2345};
    g_vc_result = tune_result;
    g_cc_result = DSD_TRUNK_TUNE_RESULT_OK;
    dsd_trunk_tuning_requests_reset();
    edacs_setup_fixture(&call);
    edacs_install_hooks();
    edacs_attach_dibit_buffers(&g_state);
    g_opts.audio_in_type = AUDIO_IN_TCP;
    g_opts.wav_sample_rate = rate_hz;
    g_opts.tcp_in_ctx = (tcp_input_ctx*)&g_edacs_tcp_token;
    g_opts.audio_out = 1;
    g_opts.audio_out_type = 8;
    g_opts.slot1_on = 1;
    edacs_install_net_audio_hooks();
    edacs_install_udp_output_hooks();
    return &call;
}

static void
edacs_start_tcp_analog_call(const edacs_grant_case* call) {
    edacs_process_valid_frame(&g_opts, &g_state, call->msg_1, call->msg_2);
}

static void
edacs_run_tcp_analog_call(int rate_hz, dsd_trunk_tune_result tune_result) {
    edacs_start_tcp_analog_call(edacs_prepare_tcp_analog_call(rate_hz, tune_result));
}

static int
edacs_blast_is_silent(int blast) {
    for (int i = 0; i < 960; i++) {
        if (g_udp_blast_samples[blast][i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* End to end at 96 kHz: the dotting that ends the call is found in the first triplet, which plays as three 48 kHz
   blocks, and the call ends there. Before the fix the release register read the 96 kHz samples as 48 kHz ones, found
   no dotting, and the call ran on until the input ended. TCP input staged before the call is dropped when it ends. */
static int
edacs_run_analog_call_cases(void) {
    int rc = 0;
    edacs_reset_audio_hook_state();
    g_tcp_signal = EDACS_TCP_DOTTING;
    g_tcp_signal_rate_hz = 96000;
    g_tcp_fail_at = 4 * 5760;
    const edacs_grant_case* call = edacs_prepare_tcp_analog_call(96000, DSD_TRUNK_TUNE_RESULT_OK);
    g_opts.input_upsample_len = 3;
    g_opts.input_upsample_pos = 1;
    g_opts.input_upsample_prev_valid = 1;
    edacs_start_tcp_analog_call(call);
    rc |= edacs_expect(g_udp_blast_count == 3, "analog-633", "96k-call", "the first triplet played, then released");
    rc |= edacs_expect(g_udp_blast_bytes[0] == 1920U && g_udp_blast_bytes[1] == 1920U && g_udp_blast_bytes[2] == 1920U,
                       "analog-633", "96k-call", "each block went out as 960 samples at 48 kHz");
    rc |= edacs_expect(g_tcp_read_count >= 5759 && g_tcp_read_count <= 5760, "analog-633", "96k-call",
                       "the call read one triplet's worth of 96 kHz input");
    rc |= edacs_expect(g_opts.input_upsample_len == 0 && g_opts.input_upsample_prev_valid == 0, "analog-633", "staging",
                       "the staging held from before the call was dropped");

    /* The same when the input fails part-way through the first triplet. */
    edacs_reset_audio_hook_state();
    g_tcp_fail_at = 100;
    call = edacs_prepare_tcp_analog_call(24000, DSD_TRUNK_TUNE_RESULT_OK);
    g_opts.input_upsample_len = 2;
    g_opts.input_upsample_prev_valid = 1;
    edacs_start_tcp_analog_call(call);
    rc |=
        edacs_expect(g_udp_blast_count == 0 && g_opts.input_upsample_len == 0 && g_opts.input_upsample_prev_valid == 0,
                     "analog-633", "staging", "the staging is dropped on a failed read too");
    edacs_reset_audio_hook_state();
    return rc;
}

/* A retune that lands while a converted triplet is read discards that triplet, and the converter starts the next one
   from silence: no tail of the old channel. The same for a retune that lands between triplets. */
static int
edacs_run_analog_retune_cases(void) {
    int rc = 0;
    edacs_reset_audio_hook_state();
    g_tcp_signal = EDACS_TCP_TONE;
    g_tcp_signal_rate_hz = 44100;
    g_tcp_silence_from = 2646;
    g_tcp_retune_at = 2646 - 100;
    g_tcp_fail_at = 2 * 2646 + 10;
    edacs_run_tcp_analog_call(44100, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= edacs_expect(g_udp_blast_count == 3, "analog-633", "late-retune", "only the triplet after the retune played");
    rc |= edacs_expect(edacs_blast_is_silent(0) && edacs_blast_is_silent(1) && edacs_blast_is_silent(2), "analog-633",
                       "late-retune", "the triplet after the retune carries no old-channel tail");

    edacs_reset_audio_hook_state();
    g_tcp_signal = EDACS_TCP_TONE;
    g_tcp_signal_rate_hz = 44100;
    g_tcp_silence_from = 2646;
    g_retune_after_blast = 3;
    g_tcp_fail_at = 2 * 2646 + 10;
    edacs_run_tcp_analog_call(44100, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= edacs_expect(g_udp_blast_count == 6, "analog-633", "between-retune", "both triplets played");
    rc |= edacs_expect(!edacs_blast_is_silent(2), "analog-633", "between-retune", "the first triplet carried the tone");
    rc |= edacs_expect(edacs_blast_is_silent(3) && edacs_blast_is_silent(4) && edacs_blast_is_silent(5), "analog-633",
                       "between-retune", "the triplet after the retune starts from silence");
    edacs_reset_audio_hook_state();
    return rc;
}

/* A tune still in flight plays nothing, and one that fails leaves the call: the radio reads the previous channel again,
   so nothing it delivers is the new call's. Before the fix EDACS played whatever arrived. A tune that lands plays from
   the triplet after the one it landed in. A triplet a retune cut through decides nothing: the old channel's dotting in
   it does not end the new call. */
static int
edacs_run_analog_tune_gate_cases(void) {
    int rc = 0;
    edacs_reset_audio_hook_state();
    g_tcp_publish_at = 1000;
    g_tcp_publish_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    g_tcp_fail_at = 4 * 2880;
    edacs_run_tcp_analog_call(48000, DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= edacs_expect(g_udp_blast_count == 0, "analog-633", "failed-tune", "nothing of the failed tune played");
    rc |= edacs_expect(g_tcp_read_count == 2880, "analog-633", "failed-tune",
                       "the call left at the first triplet after the failure");
    rc |= edacs_expect(dsd_trunk_tuning_pending_request() != 0U, "analog-633", "failed-tune",
                       "the failed tune still holds the retune gate");

    edacs_reset_audio_hook_state();
    g_tcp_publish_at = 4000;
    g_tcp_publish_result = DSD_TRUNK_TUNE_RESULT_OK;
    g_tcp_fail_at = 3 * 2880 + 10;
    edacs_run_tcp_analog_call(48000, DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= edacs_expect(g_udp_blast_count == 3, "analog-633", "pending-tune",
                       "only the triplet after the tune landed played");

    edacs_reset_audio_hook_state();
    g_tcp_signal = EDACS_TCP_TONE;
    g_tcp_dotting_until = 960;
    g_tcp_retune_at = 2 * 960 + 100;
    g_tcp_fail_at = 2 * 2880 + 10;
    edacs_run_tcp_analog_call(48000, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= edacs_expect(g_tcp_read_count > 2880, "analog-633", "discarded-triplet",
                       "the old channel's dotting in a cut triplet did not end the call");
    rc |= edacs_expect(g_udp_blast_count == 3, "analog-633", "discarded-triplet", "the cut triplet did not play");
    dsd_trunk_tuning_requests_reset();
    edacs_reset_audio_hook_state();
    return rc;
}

#ifdef USE_RADIO
/* An analog call on a fake RTL stream (issue #625): a 1 kHz tone up to g_call_drop samples, noise after, with each
   sample's squelch flag closed from g_call_drop + g_call_flag_delay (or from g_call_flags), except over a blip that
   inverts it. Reads past g_call_cap fail, which shuts the loop down: a call no squelch ends runs to it. */
static long g_call_drop = 0;
static long g_call_cap = 0;
static long g_call_flag_delay = 0;
static const uint8_t* g_call_flags = NULL;
static long g_call_flags_len = 0;
static long g_call_blip_at = -1;
static long g_call_blip_len = 0;
static int g_call_status_active = 1;
static unsigned int g_call_rate_hz = 48000U;
static uint32_t g_call_noise = 1U;
/* An open carrier with no modulation: silence up to g_call_drop. */
static int g_call_unmodulated = 0;
static long g_call_blasted = 0;
static long g_call_loud_after_close = 0;
static long g_call_loud_before_drop = 0;

static uint8_t
edacs_call_flag(long i) {
    if (g_call_flags) {
        return i < g_call_flags_len ? g_call_flags[i] : (uint8_t)DSD_SQUELCH_FLAG_CLOSED;
    }
    int closed = i >= g_call_drop + g_call_flag_delay;
    if (g_call_blip_at >= 0 && i >= g_call_blip_at && i < g_call_blip_at + g_call_blip_len) {
        closed = !closed;
    }
    return closed ? (uint8_t)DSD_SQUELCH_FLAG_CLOSED : 0U;
}

static float
edacs_call_sample(long i) {
    if (i < g_call_drop) {
        if (g_call_unmodulated) {
            return 0.0f;
        }
        return 8000.0f * (float)sin(2.0 * 3.14159265358979323846 * 1000.0 * (double)i / (double)g_call_rate_hz);
    }
    g_call_noise = (g_call_noise * 1664525U) + 1013904223U;
    return (float)((int)(g_call_noise >> 16) - 32768) * 0.25f;
}

static int
edacs_call_read_ex(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got) {
    (void)rtl_ctx;
    const long index = (long)g_rtl_read_count;
    if (out_got != NULL) {
        *out_got = 0;
    }
    if (out == NULL || out_got == NULL || count != 1U || index >= g_call_cap) {
        return -1;
    }
    g_rtl_read_count++;
    out[0] = edacs_call_sample(index);
    if (flags != NULL) {
        flags[0] = edacs_call_flag(index);
    }
    *out_got = 1;
    return 0;
}

static int
edacs_call_read(void* rtl_ctx, float* out, size_t count, int* out_got) {
    return edacs_call_read_ex(rtl_ctx, out, NULL, count, out_got);
}

static double
edacs_call_return_pwr(const void* rtl_ctx) {
    (void)rtl_ctx;
    return (long)g_rtl_read_count > g_call_drop ? 1e-9 : 1.0;
}

static int
edacs_call_squelch_status(const void* rtl_ctx, dsd_rtl_squelch_status* out) {
    (void)rtl_ctx;
    DSD_MEMSET(out, 0, sizeof(*out));
    out->active = g_call_status_active;
    out->plan_valid = 1;
    return 0;
}

static unsigned int
edacs_call_output_rate_hz(void) {
    return g_call_rate_hz;
}

/* The UDP analog output: each 960-sample block in order, whether it played anything after the gate closed, and
   whether the call before the drop played at all. The output runs at 48 kHz whatever rate the stream is read at (issue
   #633), so output sample @p at is input time at * rate / 48000. */
static void
edacs_call_blast_analog(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    const short* block = (const short*)data;
    const long count = (long)(nsam / sizeof(short));
    for (long k = 0; k < count && block != NULL; k++) {
        const int64_t at = (int64_t)(g_call_blasted + k) * (int64_t)g_call_rate_hz;
        const int loud = block[k] != 0;
        if (loud && at >= (int64_t)(g_call_drop + g_call_flag_delay) * EDACS_ANALOG_RATE_HZ && g_call_blip_len == 0
            && g_call_flags == NULL) {
            g_call_loud_after_close++;
        }
        if (loud && at < (int64_t)g_call_drop * EDACS_ANALOG_RATE_HZ) {
            g_call_loud_before_drop++;
        }
    }
    g_call_blasted += count;
}

typedef struct {
    long reads;
    int released;
} edacs_call_result;

/* One analog group call under squelch @p mode, granted on LCN 4 and played to its end. */
static edacs_call_result
edacs_run_analog_call(int mode) {
    static int call_dibit_buf[900256];
    static int call_payload_buf[900256];
    static int rtl_ctx_token;
    edacs_reset_audio_hook_state();
    edacs_setup_state_fixture(0);
    g_opts.trunk_tune_group_calls = 1;
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.audio_out = 1;
    g_opts.audio_out_type = 8;
    g_opts.slot1_on = 1;
    g_opts.rtl_squelch_mode = mode;
    g_opts.rtl_squelch_margin_db = 10;
    g_opts.rtl_squelch_level = mode == DSD_SQUELCH_MODE_LEVEL ? dB_to_pwr(-60.0) : 0.0;
    g_state.rtl_ctx = (struct RtlSdrContext*)&rtl_ctx_token;
    g_state.synctype = DSD_SYNC_EDACS_POS;
    g_state.dibit_buf = call_dibit_buf;
    g_state.dmr_payload_buf = call_payload_buf;
    g_state.dibit_buf_p = call_dibit_buf + 200;
    g_state.dmr_payload_p = call_payload_buf + 200;
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){
        .read = edacs_call_read,
        .return_pwr = edacs_call_return_pwr,
        .read_ex = edacs_call_read_ex,
        .squelch_status = edacs_call_squelch_status,
    });
    dsd_rtl_stream_metrics_hooks rates;
    DSD_MEMSET(&rates, 0, sizeof(rates));
    rates.output_rate_hz = edacs_call_output_rate_hz;
    dsd_rtl_stream_metrics_hooks_set(&rates);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast_analog = edacs_call_blast_analog});
    g_call_noise = 1U;
    g_call_blasted = 0;
    g_call_loud_after_close = 0;
    g_call_loud_before_drop = 0;

    edacs_process_valid_frame(&g_opts, &g_state, edacs_standard_group_msg1(0, 4, 321, 1234),
                              edacs_standard_group_msg2(1234));
    edacs_call_result result = {(long)g_rtl_read_count, !dsd_exitflag_load()};
    dsd_exitflag_store(0);
    dsd_rtl_stream_metrics_hooks_set(NULL);
    edacs_reset_audio_hook_state();
    dsd_state_ext_free_all(&g_state);
    return result;
}

/* The stream samples a triplet reads at @p rate_hz: a triplet is 2880 samples at EDACS_ANALOG_RATE_HZ, the rate EDACS
   analog voice runs at whatever rate it is read at (issue #633), so 1440 at 24 kHz. */
static long
edacs_triplet_reads(unsigned int rate_hz) {
    return ((long)EDACS_ANALOG_TRIPLET_SAMPLES * (long)rate_hz) / (long)EDACS_ANALOG_RATE_HZ;
}

/* The stream samples a closed run of @p run samples at EDACS_ANALOG_RATE_HZ spans at @p rate_hz. */
static long
edacs_run_reads(long run, unsigned int rate_hz) {
    return (run * (long)rate_hz) / (long)EDACS_ANALOG_RATE_HZ;
}

/* Where the level squelch releases a call read at @p rate_hz (24 or 48 kHz, where a triplet reads a whole number of
   samples) whose carrier drops at sample @p drop: it reads the power once per triplet's end, and five closed readings
   end the call. */
static long
edacs_level_release(long drop, unsigned int rate_hz) {
    const long t = edacs_triplet_reads(rate_hz);
    return ((drop / t) + 5) * t;
}

static void
edacs_call_defaults(long drop, unsigned int rate_hz) {
    g_call_drop = drop;
    g_call_cap = drop + (12L * EDACS_ANALOG_TRIPLET_SAMPLES);
    g_call_flag_delay = 0;
    g_call_flags = NULL;
    g_call_flags_len = 0;
    g_call_blip_at = -1;
    g_call_blip_len = 0;
    g_call_status_active = 1;
    g_call_rate_hz = rate_hz;
    g_call_unmodulated = 0;
}

/* Under AUTO and NOISE an analog call ends on the gate within the level squelch's time of its carrier dropping, at
   every drop phase and closing delay up to the bound, and its tail plays silence. A 24 kHz stream runs at 48 kHz too
   (issue #633): its triplets, the gate's hold and the level squelch's readings all count 48 kHz samples. */
static int
edacs_run_analog_call_release_cases(void) {
    int rc = 0;
    const double phases[4] = {0.0, 0.25, 0.5, 0.99};
    const unsigned int rates[2] = {24000U, 48000U};
    const int modes[2] = {DSD_SQUELCH_MODE_AUTO, DSD_SQUELCH_MODE_NOISE};
    for (int r = 0; r < 2; r++) {
        const long t = edacs_triplet_reads(rates[r]);
        const long bound = (DSD_SQUELCH_CLOSE_DELAY_MS * (long)rates[r]) / 1000L;
        const long hold = edacs_run_reads((long)edacs_gate_hold_samples(EDACS_ANALOG_RATE_HZ), rates[r]);
        for (int f = 0; f < 4; f++) {
            const long drop = (4L * t) + (long)(phases[f] * (double)t);
            edacs_call_defaults(drop, rates[r]);
            const edacs_call_result level = edacs_run_analog_call(DSD_SQUELCH_MODE_LEVEL);
            rc |= edacs_expect(level.released && level.reads == edacs_level_release(drop, rates[r]), "analog-call",
                               "level", "the level squelch ends the call on its fifth closed reading");
            for (int m = 0; m < 2; m++) {
                for (int d = 0; d < 3; d++) {
                    edacs_call_defaults(drop, rates[r]);
                    g_call_flag_delay = (bound * d) / 2;
                    const edacs_call_result gate = edacs_run_analog_call(modes[m]);
                    const int in_time =
                        gate.released && gate.reads >= drop + g_call_flag_delay + hold && gate.reads <= level.reads;
                    if (!in_time) {
                        DSD_FPRINTF(stderr,
                                    "  %s at %u Hz, drop %ld, delay %ld: released %d after %ld samples (level %ld)\n",
                                    modes[m] == DSD_SQUELCH_MODE_NOISE ? "noise" : "auto", rates[r], drop,
                                    g_call_flag_delay, gate.released, gate.reads, level.reads);
                    }
                    rc |= edacs_expect(in_time, "analog-call", modes[m] == DSD_SQUELCH_MODE_NOISE ? "noise" : "auto",
                                       "the gate ends the call within the level squelch's time");
                    rc |= edacs_expect(g_call_loud_before_drop > 0 && g_call_loud_after_close == 0, "analog-call",
                                       "audio", "the call plays and its closed tail is silence");
                }
            }
        }
    }
    return rc;
}

/* Issue #574: through a whole analog call the stamp follows the squelch the call runs, never the audio's level: a
   granted channel whose every sample the dynamic squelch closes (an empty channel) stamps nothing, nor does one whose
   power stays under the level squelch, while an open carrier with no modulation stamps as a modulated one does. */
static int
edacs_run_analog_call_stamp_cases(void) {
    static const struct {
        const char* tag;
        int mode;
        long drop_triplets;
        int unmodulated;
        int want_stamp;
    } cases[] = {
        {"empty channel under the dynamic squelch", DSD_SQUELCH_MODE_AUTO, 0, 0, 0},
        {"empty channel under the level squelch", DSD_SQUELCH_MODE_LEVEL, 0, 0, 0},
        {"unmodulated carrier under the dynamic squelch", DSD_SQUELCH_MODE_AUTO, 4, 1, 1},
        {"unmodulated carrier under the level squelch", DSD_SQUELCH_MODE_LEVEL, 4, 1, 1},
        {"modulated carrier under the dynamic squelch", DSD_SQUELCH_MODE_AUTO, 4, 0, 1},
    };

    int rc = 0;
    dsd_audio_activity_arm();
    for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
        edacs_call_defaults(cases[c].drop_triplets * EDACS_ANALOG_TRIPLET_SAMPLES, 48000U);
        g_call_unmodulated = cases[c].unmodulated;
        dsd_audio_activity_reset();
        const edacs_call_result call = edacs_run_analog_call(cases[c].mode);
        uint64_t stamp = 0U;
        dsd_audio_activity_read(&stamp, NULL);
        rc |= edacs_expect(call.released, "analog-call-stamp", cases[c].tag, "the call ended on its squelch");
        if ((stamp != 0U) != cases[c].want_stamp) {
            DSD_FPRINTF(stderr, "  %s: stamp %d, want %d\n", cases[c].tag, stamp != 0U, cases[c].want_stamp);
        }
        rc |= edacs_expect((stamp != 0U) == cases[c].want_stamp, "analog-call-stamp", cases[c].tag,
                           "the call stamps exactly when its squelch opened");
    }
    edacs_call_defaults(0, 48000U);
    dsd_audio_activity_reset();
    return rc;
}

/* A false open in the tail restarts the hold; a false close inside the call never ends it; a stream whose status
   says no gate runs ends nothing on its flags. */
static int
edacs_run_analog_call_disturbance_cases(void) {
    int rc = 0;
    const long t = EDACS_ANALOG_TRIPLET_SAMPLES;
    const long ms = 48;
    const long hold = (long)edacs_gate_hold_samples(48000);
    const long blips[2] = {20L * ms, 40L * ms};
    for (int b = 0; b < 2; b++) {
        const long drop = (4L * t) + (t / 2);
        edacs_call_defaults(drop, 48000U);
        g_call_blip_at = drop + t;
        g_call_blip_len = blips[b];
        const edacs_call_result tail = edacs_run_analog_call(DSD_SQUELCH_MODE_AUTO);
        rc |= edacs_expect(tail.released && tail.reads >= g_call_blip_at + g_call_blip_len + hold, "analog-call",
                           "tail-burst", "an open burst in the tail restarts the hold");

        const long late_drop = 9L * t;
        edacs_call_defaults(late_drop, 48000U);
        g_call_blip_at = 3L * t;
        g_call_blip_len = blips[b];
        const edacs_call_result mid = edacs_run_analog_call(DSD_SQUELCH_MODE_AUTO);
        rc |= edacs_expect(mid.released && mid.reads >= late_drop + hold, "analog-call", "mid-blip",
                           "a closed blip inside the call does not end it");
    }

    edacs_call_defaults((4L * t) + (t / 2), 48000U);
    g_call_status_active = 0;
    const edacs_call_result inactive = edacs_run_analog_call(DSD_SQUELCH_MODE_AUTO);
    rc |= edacs_expect(!inactive.released && inactive.reads == g_call_cap, "analog-call", "no-gate",
                       "with no gate running the flags end nothing (the watchdog's case)");
    return rc;
}

/* One call on the flags the real tracker makes at @p rate_hz on a carrier that lands while it is still learning (the
   landing mid-carrier a call always starts with) and drops @p phase_ms into one of its 40 ms windows. The call ends no
   later than the same call under the level squelch, also on an unresampled 125 kHz replay, which EDACS runs at 48 kHz
   too (issue #633). Never before the drop: the call's own first window, closed while the tracker decides, is not its
   end. */
static int
edacs_run_analog_call_real_tracker_case(int rate, int phase_ms) {
    const long t = edacs_triplet_reads((unsigned int)rate);
    dsd_squelch_floor_plan plan;
    if (dsd_squelch_floor_plan_design(&plan, NULL, 0, NULL, 0, rate) != 0) {
        return edacs_expect(0, "analog-call", "tracker", "designed the plan");
    }
    const long drop = (long)rate + ((long)phase_ms * rate) / 1000L;
    const long total = drop + (8L * t) + ((long)rate / 4L);
    float* iq = (float*)calloc((size_t)total * 2U, sizeof(float));
    uint8_t* flags = (uint8_t*)calloc((size_t)total, sizeof(uint8_t));
    dsd_squelch_floor* tracker = (dsd_squelch_floor*)calloc(1U, sizeof(dsd_squelch_floor));
    if (!iq || !flags || !tracker) {
        free(iq);
        free(flags);
        free(tracker);
        return edacs_expect(0, "analog-call", "tracker", "allocated");
    }
    uint32_t lcg = 7U + (uint32_t)phase_ms + (uint32_t)rate;
    for (long i = 0; i < total; i++) {
        /* Complex Gaussian noise (Box-Muller), as a receiver's noise is behind its channel filter: 20 dB under the
           carrier. */
        lcg = (lcg * 1664525U) + 1013904223U;
        const double u1 = ((double)(lcg >> 8) + 1.0) / 16777217.0;
        lcg = (lcg * 1664525U) + 1013904223U;
        const double u2 = (double)(lcg >> 8) / 16777216.0;
        const double radius = 0.1 * sqrt(-log(u1));
        double re = radius * cos(2.0 * 3.14159265358979323846 * u2);
        double im = radius * sin(2.0 * 3.14159265358979323846 * u2);
        if (i < drop) {
            const double ph = 2.5 * sin(2.0 * 3.14159265358979323846 * 1000.0 * (double)i / (double)rate);
            re += cos(ph);
            im += sin(ph);
        }
        iq[(size_t)i * 2U] = (float)re;
        iq[((size_t)i * 2U) + 1U] = (float)im;
    }
    dsd_squelch_floor_set_plan(tracker, &plan);
    dsd_squelch_floor_set_margin(tracker, 10);
    dsd_squelch_floor_reset(tracker);
    dsd_squelch_floor_process(tracker, iq, (int)total, flags);
    free(iq);
    free(tracker);
    edacs_call_defaults(drop, (unsigned int)rate);
    g_call_cap = total;
    const edacs_call_result level = edacs_run_analog_call(DSD_SQUELCH_MODE_LEVEL);
    edacs_call_defaults(drop, (unsigned int)rate);
    g_call_cap = total;
    g_call_flags = flags;
    g_call_flags_len = total;
    const edacs_call_result gate = edacs_run_analog_call(DSD_SQUELCH_MODE_AUTO);
    free(flags);
    g_call_flags = NULL;
    const long latest = level.released ? level.reads : 0L;
    if (!(gate.released && gate.reads <= latest && gate.reads >= drop)) {
        DSD_FPRINTF(stderr, "  real tracker at %d Hz, drop at %d ms of a window: released %d after %ld (latest %ld)\n",
                    rate, phase_ms, gate.released, gate.reads, latest);
        return 1;
    }
    return 0;
}

static int
edacs_run_analog_call_real_tracker_cases(void) {
    int rc = 0;
    for (int phase = 0; phase < 40; phase += 2) {
        rc |= edacs_run_analog_call_real_tracker_case(48000, phase);
    }
    /* An unresampled 125 kHz replay (DSD_NEO_RESAMP=off), converted to 48 kHz. */
    for (int phase = 0; phase < 40; phase += 8) {
        rc |= edacs_run_analog_call_real_tracker_case(125000, phase);
    }
    return edacs_expect(rc == 0, "analog-call", "tracker",
                        "the real tracker's flags end the call within the level squelch's time at 48 and 125 kHz");
}
#endif

#ifdef USE_RADIO
/* A retune that lands as the RTL output rate is read, from 48 to 96 kHz (issue #633): the triplet read then is
   discarded, whatever ratio the converter ran, and the next one is converted at the new rate. The reception is taken
   before the rate, so the retune always shows in it. Before, the converter could stay at the old ratio for a triplet
   read at the new rate and play it at half speed. */
static int
edacs_run_rtl_rate_switch_case(void) {
    static int rtl_token = 0x5A;
    edacs_reset_audio_hook_state();
    const edacs_grant_case* call = edacs_prepare_tcp_analog_call(48000, DSD_TRUNK_TUNE_RESULT_OK);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.tcp_in_ctx = NULL;
    g_state.rtl_ctx = (struct RtlSdrContext*)&rtl_token;
    g_rtl_read_value = 0.0f;
    edacs_install_rtl_stream_hooks();
    edacs_install_rtl_rate_hook(48000U);
    g_rtl_switch_at_rate_read = 2;
    g_rtl_switch_rate_hz = 96000U;
    g_rtl_fail_at = 2880 + (2 * 5760);
    edacs_start_tcp_analog_call(call);
    int rc = 0;
    rc |= edacs_expect(g_udp_blast_count == 6, "analog-633", "rtl-rate-switch",
                       "the triplet read across the retune did not play");
    rc |= edacs_expect(g_rtl_read_count == 2880 + (2 * 5760) + 1, "analog-633", "rtl-rate-switch",
                       "the triplets after the retune read 96 kHz input");
    edacs_install_rtl_rate_hook(0U);
    edacs_reset_audio_hook_state();
    return rc;
}

/* A pending tune that completes right after a triplet was collected under it: the triplet still decides nothing. The
   gate is read before the reception, so a completion after the gate shows as a moved reception. Before, the reception
   was read first, and a completion between the two let the triplet collected while tuning play and decide. */
static int
edacs_run_tune_completion_race_case(void) {
    static int rtl_token = 0x5B;
    edacs_reset_audio_hook_state();
    const edacs_grant_case* call = edacs_prepare_tcp_analog_call(48000, DSD_TRUNK_TUNE_RESULT_PENDING);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.tcp_in_ctx = NULL;
    g_state.rtl_ctx = (struct RtlSdrContext*)&rtl_token;
    g_rtl_read_value = 0.0f;
    edacs_install_rtl_stream_hooks();
    edacs_install_rtl_rate_hook(48000U);
    g_rtl_publish_after_reads = 2880;
    g_rtl_fail_at = 2 * 2880;
    edacs_start_tcp_analog_call(call);
    int rc = edacs_expect(g_udp_blast_count == 3, "analog-633", "tune-completion-race",
                          "only the triplet after the tune completed played");
    edacs_install_rtl_rate_hook(0U);
    dsd_trunk_tuning_requests_reset();
    edacs_reset_audio_hook_state();
    return rc;
}
#endif

/* FNV-1a over @p n bytes. */
static uint64_t
edacs_fnv1a(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t
edacs_fnv1a_file(uint64_t h, const char* path) {
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return h ^ 0xDEADULL;
    }
    unsigned char buf[4096];
    for (;;) {
        const size_t got = fread(buf, 1U, sizeof(buf), fp);
        h = edacs_fnv1a(h, buf, got);
        if (got < sizeof(buf)) {
            break;
        }
    }
    (void)fclose(fp);
    return h;
}

/* A 48 kHz call that ends on its own (dotting in the third triplet), with output @p out: 0 the UDP socket and a
   per-call WAV, 1 the static WAV, 2 the stdout fd. Returns the hash of everything it wrote. */
static uint64_t
edacs_golden_call(int out) {
    edacs_reset_audio_hook_state();
    g_tcp_signal = EDACS_TCP_TONE;
    g_tcp_dotting_from = 2 * 2880;
    g_tcp_fail_at = 6 * 2880;
    const edacs_grant_case* call = edacs_prepare_tcp_analog_call(48000, DSD_TRUNK_TUNE_RESULT_OK);
    char path[] = "dsdneo_edacs_golden_XXXXXX";
    const int fd = dsd_mkstemp(path);
    if (fd < 0) {
        return 0ULL;
    }
    if (out == 2) {
        g_opts.audio_out_type = 1;
        g_opts.slot1_on = 1;
        g_opts.audio_out_fd = fd;
    } else {
        (void)dsd_close(fd);
        SF_INFO info;
        DSD_MEMSET(&info, 0, sizeof(info));
        info.samplerate = out == 0 ? 48000 : 8000;
        info.channels = out == 0 ? 1 : 2;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        g_opts.wav_out_f = sf_open(path, SFM_WRITE, &info);
        g_opts.dmr_stereo_wav = out == 0 ? 1 : 0;
        g_opts.static_wav_file = out == 1 ? 1 : 0;
    }
    edacs_start_tcp_analog_call(call);
    if (out == 2) {
        (void)dsd_close(fd);
    } else if (g_opts.wav_out_f != NULL) {
        sf_close(g_opts.wav_out_f);
        g_opts.wav_out_f = NULL;
    }
    uint64_t h = 1469598103934665603ULL;
    const int blasts = g_udp_blast_count;
    h = edacs_fnv1a(h, &blasts, sizeof(blasts));
    h = edacs_fnv1a(h, &g_tcp_read_count, sizeof(g_tcp_read_count));
    h = edacs_fnv1a(h, g_udp_blast_samples, sizeof(g_udp_blast_samples));
    h = edacs_fnv1a_file(h, path);
    (void)remove(path);
    dsd_state_ext_free_all(&g_state);
    edacs_reset_audio_hook_state();
    return h;
}

/* At 48 kHz nothing changes: the audio, the per-call and static WAVs and the stdout stream of a whole call are
   byte-for-byte what they were before issue #633 (hashes taken from the code before it). */
static int
edacs_run_golden_48k_cases(void) {
    static const uint64_t want[3] = {EDACS_GOLDEN_UDP_WAV, EDACS_GOLDEN_STATIC_WAV, EDACS_GOLDEN_FD};
    int rc = 0;
    for (int out = 0; out < 3; out++) {
        const uint64_t got = edacs_golden_call(out);
        if (got != want[out]) {
            DSD_FPRINTF(stderr, "golden %d: got 0x%016llXULL\n", out, (unsigned long long)got);
        }
        rc |= edacs_expect(got == want[out], "analog-633", "golden-48k", "a 48 kHz call is byte-identical");
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    const int std_group = 321;
    const int std_group_src = 1234;
    const int std_indiv_target = 4321;
    const int std_indiv_src = 2345;
    const int ea_group = 54321;
    const int ea_group_src = 34567;
    const int ea_icall_target = 654321;
    const int ea_icall_src = 45678;

    static const struct {
        const char* name;
        dsd_trunk_tune_result result;
    } results[] = {
        {"ok", DSD_TRUNK_TUNE_RESULT_OK},
        {"pending", DSD_TRUNK_TUNE_RESULT_PENDING},
        {"deferred", DSD_TRUNK_TUNE_RESULT_DEFERRED},
        {"failed", DSD_TRUNK_TUNE_RESULT_FAILED},
        {"timeout", DSD_TRUNK_TUNE_RESULT_TIMEOUT},
    };

    const edacs_grant_case cases[] = {
        {"standard-digital-group", edacs_standard_group_msg1(2, 5, std_group, std_group_src),
         edacs_standard_group_msg2(std_group_src), 852012500L, 0, 5, EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_GROUP,
         std_group, std_group_src},
        {"standard-digital-individual", edacs_standard_individual_msg1(6, std_indiv_target, 1),
         (unsigned long long int)std_indiv_src, 852512500L, 0, 6,
         EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_INDIVIDUAL, std_indiv_target, std_indiv_src},
        {"ea-digital-group", edacs_extended_group_msg1(3, 7, ea_group), (unsigned long long int)ea_group_src,
         853012500L, 1, 7, EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_GROUP, ea_group, ea_group_src},
        {"ea-digital-icall", edacs_extended_icall_msg1(ea_icall_target), edacs_extended_icall_msg2(8, ea_icall_src),
         853512500L, 1, 8, EDACS_IS_VOICE | EDACS_IS_DIGITAL | EDACS_IS_INDIVIDUAL, ea_icall_target, ea_icall_src},
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        for (size_t r = 0; r < sizeof(results) / sizeof(results[0]); r++) {
            rc |= edacs_run_grant_result_case(&cases[c], results[r].result, results[r].name);
        }
    }

    static const struct {
        size_t case_index;
        edacs_no_tune_guard guard;
        const char* name;
    } guard_cases[] = {
        {0U, EDACS_GUARD_GROUP_DISABLED, "group-disabled"},
        {1U, EDACS_GUARD_PRIVATE_DISABLED, "private-disabled"},
        {2U, EDACS_GUARD_GROUP_DISABLED, "ea-group-disabled"},
        {3U, EDACS_GUARD_PRIVATE_DISABLED, "ea-private-disabled"},
        {0U, EDACS_GUARD_ALLOWLIST_BLOCK, "group-allowlist-block"},
        {1U, EDACS_GUARD_ALLOWLIST_BLOCK, "private-allowlist-block"},
        {2U, EDACS_GUARD_ALLOWLIST_BLOCK, "ea-group-allowlist-block"},
        {3U, EDACS_GUARD_ALLOWLIST_BLOCK, "ea-private-allowlist-block"},
        {2U, EDACS_GUARD_MISSING_FREQUENCY, "missing-frequency"},
        {2U, EDACS_GUARD_MISSING_CC_LCN, "missing-cc-lcn"},
        {3U, EDACS_GUARD_TRUNK_DISABLED, "trunk-disabled"},
    };

    for (size_t g = 0; g < sizeof(guard_cases) / sizeof(guard_cases[0]); g++) {
        rc |=
            edacs_run_no_tune_guard_case(&cases[guard_cases[g].case_index], guard_cases[g].guard, guard_cases[g].name);
    }

    rc |= edacs_run_retry_after_reject_case(&cases[0], DSD_TRUNK_TUNE_RESULT_DEFERRED, "retry-after-deferred");
    rc |= edacs_run_retry_after_reject_case(&cases[1], DSD_TRUNK_TUNE_RESULT_FAILED, "retry-after-failed");
    rc |= edacs_run_retry_after_reject_case(&cases[2], DSD_TRUNK_TUNE_RESULT_TIMEOUT, "retry-after-timeout");
    rc |= edacs_run_inverted_polarity_cases(&cases[0]);

    for (size_t r = 0; r < sizeof(results) / sizeof(results[0]); r++) {
        rc |= edacs_run_eot_result_case(results[r].result, results[r].name);
    }
    rc |= edacs_run_eot_retry_after_reject_case(DSD_TRUNK_TUNE_RESULT_DEFERRED, "deferred-then-ok");
    rc |= edacs_run_eot_retry_after_reject_case(DSD_TRUNK_TUNE_RESULT_FAILED, "failed-then-ok");
    rc |= edacs_run_eot_retry_after_reject_case(DSD_TRUNK_TUNE_RESULT_TIMEOUT, "timeout-then-ok");
    rc |= edacs_run_eot_wav_rotation_case();
    rc |= edacs_run_retune_after_eot_case(&cases[3]);
    rc |= edacs_run_standard_state_cases();
    rc |= edacs_run_extended_state_cases();
    rc |= edacs_run_helper_contract_cases();
    rc |= edacs_run_analog_loop_helper_cases();
    rc |= test_edacs_analog_media_honors_talkgroup_policy();
    rc |= test_edacs_analog_output_honors_mute_and_slot();
    rc |= test_edacs_analog_emit_stamps_open_reception();
    rc |= test_edacs_analog_stamp_follows_the_writer();
    rc |= edacs_run_analog_sql_helper_cases();
    rc |= edacs_run_converted_collect_cases();
    rc |= edacs_run_analog_call_cases();
    rc |= edacs_run_analog_retune_cases();
    rc |= edacs_run_analog_tune_gate_cases();
    rc |= edacs_run_golden_48k_cases();
#ifdef USE_RADIO
    rc |= edacs_run_analog_call_release_cases();
    rc |= edacs_run_analog_call_disturbance_cases();
    rc |= edacs_run_analog_call_stamp_cases();
    rc |= edacs_run_analog_call_real_tracker_cases();
    rc |= edacs_run_rtl_rate_switch_case();
    rc |= edacs_run_tune_completion_race_case();
#endif

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_net_audio_input_hooks_set((dsd_net_audio_input_hooks){0});
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
#ifdef USE_RADIO
    dsd_rtl_stream_io_hooks_set((dsd_rtl_stream_io_hooks){0});
#endif
    if (rc == 0) {
        printf("EDACS_GRANT_TUNE_MATRIX: OK\n");
    }
    dsd_state_ext_free_all(&g_state);
    return rc;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
