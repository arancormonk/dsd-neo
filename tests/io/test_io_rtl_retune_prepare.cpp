// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/log.h>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

extern "C" uint64_t rtl_device_test_coalesce_capture_mute_duration(uint64_t* pending_bytes, uint64_t duration_bytes,
                                                                   size_t alignment);
extern "C" int rtl_device_test_complete_fragmented_capture_discard(int byte_count, unsigned int partial_byte_count);
extern "C" void rtl_device_test_end_capture_reconfigure_with_odd_carry(int* out_hold, int* out_mute,
                                                                       int* out_mute_byte_phase);
extern "C" int rtl_device_test_begin_capture_reconfigure_without_writer(int* out_hold);
extern "C" int rtl_device_test_usb_reconfigure_discards_samples(size_t input_bytes, size_t* out_ring_used);
extern "C" void rtl_device_test_replay_dispatch_reset_event_state(int* phase, int* have_carry, uint8_t* carry_byte);
extern "C" int rtl_device_test_replay_event_boundary_drained(size_t ring_used, uint64_t submitted_gen,
                                                             uint64_t consumed_gen);
extern "C" int rtl_device_test_usb_apply_retry(int verify_enabled, int attempts, int apply_success_after,
                                               int verify_success_after, int* out_apply_calls, int* out_verify_calls,
                                               int* out_used_attempts);
extern "C" int rtl_device_test_usb_sample_rate_readback(uint32_t requested_rate, uint32_t actual_rate,
                                                        uint32_t* out_actual_rate);
extern "C" int rtl_device_test_usb_manual_gain_controls(int agc_rc, int gain_mode_rc, int gain_rc, int* out_agc_calls,
                                                        int* out_gain_mode_calls, int* out_gain_calls,
                                                        int* out_recorded_agc_rc);
extern "C" int rtl_device_test_usb_auto_gain_controls(int agc_rc, int gain_mode_rc, int* out_agc_calls,
                                                      int* out_gain_mode_calls, int* out_recorded_agc_rc);
extern "C" int rtl_device_test_u8_odd_carry_bridge(size_t* out_used, int* out_phase, int* out_carry_valid,
                                                   uint8_t* out_carry_byte, int* out_first_status,
                                                   int* out_second_status);
extern "C" int rtl_device_test_u8_full_ring_drop(size_t* out_used, uint64_t* out_drops, uint64_t* out_full_events,
                                                 int* out_phase, int* out_status);
extern "C" int rtl_device_test_u8_generation_stale_drop(uint64_t* out_drops, int* out_phase, int* out_dev_carry_valid,
                                                        int* out_local_carry_valid, int* out_status);
extern "C" int rtl_device_test_u8_moment_accounting(dsd_input_level_cu8_moments* out, size_t out_count);
extern "C" int rtl_device_test_replay_input_level_snapshot(int format, int backend, const char* capture_stage,
                                                           size_t raw_bytes, size_t scratch_cap_f32, int* out_rc,
                                                           int* out_source, uint64_t* out_count);

// Mirrors the internal RTL replay-conversion test hook request layout.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct rtl_device_test_replay_convert_block_request {
    int format;
    const char* capture_stage;
    int fs4_shift_enabled;
    int historical_cu8_two_pass;
    const uint8_t* raw_block;
    size_t raw_bytes;
    size_t out_cap_f32;
    int start_phase;
    int start_have_carry;
    uint8_t start_carry_byte;
};

extern "C" int rtl_device_test_replay_convert_block(const rtl_device_test_replay_convert_block_request* request,
                                                    float* out_f32, size_t out_f32_count, int* out_phase,
                                                    int* out_have_carry, uint8_t* out_carry_byte);
extern "C" int rtl_device_test_public_capture_policy(int* out_formats, size_t out_formats_len, uint32_t* out_counts,
                                                     size_t out_counts_len);
extern "C" int rtl_device_test_misc_string_helpers(char* tuner_labels, size_t tuner_labels_size, char* reason_small,
                                                   size_t reason_small_size, char* reason_null, size_t reason_null_size,
                                                   char* trimmed, size_t trimmed_size, size_t* rounded_pages,
                                                   size_t rounded_pages_len);
extern "C" int rtl_device_test_tcp_policy_helpers(size_t* bufsz_out, size_t bufsz_count, int* waitall_out,
                                                  size_t waitall_count, uint64_t* delta_out, size_t delta_count,
                                                  int* agc_out);

static int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size_eq(const char* label, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%zu want=%zu\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u64_eq(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
    return 0;
}

static int
expect_cu8_moments(const char* label, const dsd_input_level_cu8_moments* got, const uint8_t* raw, size_t raw_count) {
    dsd_input_level_cu8_moments want;
    dsd_input_level_cu8_moments_reset(&want);
    if (!got || dsd_input_level_cu8_moments_accumulate(&want, raw, raw_count) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s invalid test input\n", label);
        return 1;
    }
    if (got->count != want.count || got->sum != want.sum || got->sum_sq != want.sum_sq || got->clipped != want.clipped
        || got->min_sample != want.min_sample || got->max_sample != want.max_sample) {
        DSD_FPRINTF(stderr, "FAIL: %s got={%llu,%llu,%llu,%llu,%u,%u} want={%llu,%llu,%llu,%llu,%u,%u}\n", label,
                    (unsigned long long)got->count, (unsigned long long)got->sum, (unsigned long long)got->sum_sq,
                    (unsigned long long)got->clipped, (unsigned int)got->min_sample, (unsigned int)got->max_sample,
                    (unsigned long long)want.count, (unsigned long long)want.sum, (unsigned long long)want.sum_sq,
                    (unsigned long long)want.clipped, (unsigned int)want.min_sample, (unsigned int)want.max_sample);
        return 1;
    }
    return 0;
}

namespace {
struct TaggedTuneCompletionState {
    int calls;
    uint64_t request_id;
    rtl_stream_tune_result result;
};

void
tagged_tune_completion(uint64_t request_id, rtl_stream_tune_result result, void* user_data) {
    auto* state = static_cast<TaggedTuneCompletionState*>(user_data);
    state->calls++;
    state->request_id = request_id;
    state->result = result;
}
} // namespace

static int
call_replay_convert_block(int format, const char* capture_stage, int fs4_shift_enabled, int historical_cu8_two_pass,
                          const uint8_t* raw_block, size_t raw_bytes, size_t out_cap_f32, int start_phase,
                          int start_have_carry, uint8_t start_carry_byte, float* out_f32, size_t out_f32_count,
                          int* out_phase, int* out_have_carry, uint8_t* out_carry_byte) {
    rtl_device_test_replay_convert_block_request request{
        format,    capture_stage, fs4_shift_enabled, historical_cu8_two_pass, raw_block,
        raw_bytes, out_cap_f32,   start_phase,       start_have_carry,        start_carry_byte};
    return rtl_device_test_replay_convert_block(&request, out_f32, out_f32_count, out_phase, out_have_carry,
                                                out_carry_byte);
}

static int
expect_generation_eq(const char* label, uint32_t before, uint32_t after) {
    if (before != after) {
        DSD_FPRINTF(stderr, "FAIL: %s before=%u after=%u\n", label, before, after);
        return 1;
    }
    return 0;
}

static int
expect_generation_changed(const char* label, uint32_t before, uint32_t after) {
    if (before == after) {
        DSD_FPRINTF(stderr, "FAIL: %s before=%u after=%u\n", label, before, after);
        return 1;
    }
    return 0;
}

static int
expect_double_near(const char* label, double got, double want, double tolerance) {
    if (std::fabs(got - want) > tolerance) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%.9f want=%.9f tolerance=%.9f\n", label, got, want, tolerance);
        return 1;
    }
    return 0;
}

/* A reset puts the monitor state back to exact start values (0, or 1 for the squelch envelope), so the tolerance
 * only has to separate them from the stale values seeded before the retune, each at least 0.125 away. */
static const double kResetTolerance = 1e-12;

static_assert(sizeof(float) == sizeof(uint32_t), "float bit patterns are compared as uint32_t");

static uint32_t
float_bits(float v) {
    uint32_t bits = 0U;
    DSD_MEMCPY(&bits, &v, sizeof bits);
    return bits;
}

/* Bit identity, not closeness: an unchanged rate must leave a coefficient exactly as it was (the default path stays
 * bit-identical), which a tolerance would not prove. Compares the bit patterns, as DSP_CHANNEL_FILTERS does for taps. */
static int
expect_float_bits_equal(const char* label, float got, float want) {
    if (float_bits(got) != float_bits(want)) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%.9g (0x%08x) want=%.9g (0x%08x)\n", label, (double)got,
                    (unsigned)float_bits(got), (double)want, (unsigned)float_bits(want));
        return 1;
    }
    return 0;
}

/* The one-pole coefficients stream open derives from the output rate. */
static double
expected_deemph_alpha(int rate_hz, double tau_s) {
    const double a = std::exp(-1.0 / ((double)rate_hz * tau_s));
    int q15 = (int)std::lrint((1.0 - a) * 32768.0);
    q15 = q15 < 1 ? 1 : (q15 > 32767 ? 32767 : q15);
    return (double)q15 / 32768.0;
}

static double
expected_audio_lpf_alpha(int rate_hz, int cutoff_hz) {
    return 1.0 - std::exp(-2.0 * 3.14159265358979323846 * (double)cutoff_hz / (double)rate_hz);
}

/*
 * A retune on the analog monitor starts from clean filter state: de-emphasis,
 * DC, audio-LPF and squelch-envelope state and the channel/half-band/resampler
 * histories all return to their fresh-open values, and the rate-dependent
 * coefficients follow the output rate the device actually settled on.
 */
static int
test_audio_monitor_retune_reset(void) {
    int failed = 0;
    rtl_stream_test_audio_reset_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq("audio monitor retune hook", rtl_stream_test_audio_monitor_retune(48000, 78125, 0, &r), 0);
    failed |= expect_double_near("deemph state reset", r.deemph_avg, 0.0, kResetTolerance);
    failed |= expect_double_near("dc state reset", r.dc_avg, 0.0, kResetTolerance);
    failed |= expect_double_near("audio LPF state reset", r.audio_lpf_state, 0.0, kResetTolerance);
    failed |= expect_double_near("squelch envelope reopened", r.squelch_env, 1.0, kResetTolerance);
    failed |= expect_int_eq("channel LPF history cleared", r.channel_hist_cleared, 1);
    failed |= expect_int_eq("half-band history cleared", r.hb_hist_cleared, 1);
    failed |= expect_int_eq("resampler history cleared", r.resamp_hist_cleared, 1);
    failed |=
        expect_double_near("deemph seeded for 48 kHz", r.deemph_a_before, expected_deemph_alpha(48000, 75e-6), 1e-9);
    failed |= expect_double_near("deemph follows the forced 78125 Hz rate", r.deemph_a_after,
                                 expected_deemph_alpha(78125, 75e-6), 1e-9);
    failed |= expect_double_near("audio LPF seeded for 48 kHz", r.audio_lpf_alpha_before,
                                 expected_audio_lpf_alpha(48000, 3000), 1e-6);
    failed |= expect_double_near("audio LPF follows the forced rate", r.audio_lpf_alpha_after,
                                 expected_audio_lpf_alpha(78125, 3000), 1e-6);

    /* An unchanged rate keeps the coefficients exactly (the default path stays bit-identical). At 24 kHz the
     * monitor resamples to 48 kHz with the same ratio before and after, so its history is only clean if the
     * retune reset it. */
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq("same-rate retune hook", rtl_stream_test_audio_monitor_retune(24000, 24000, 0, &r), 0);
    failed |= expect_float_bits_equal("same-rate deemph unchanged", r.deemph_a_after, r.deemph_a_before);
    failed |=
        expect_float_bits_equal("same-rate audio LPF unchanged", r.audio_lpf_alpha_after, r.audio_lpf_alpha_before);
    failed |= expect_int_eq("same-ratio resampler history cleared", r.resamp_hist_cleared, 1);
    failed |= expect_double_near("same-rate deemph state reset", r.deemph_avg, 0.0, kResetTolerance);
    failed |= expect_double_near("same-rate squelch envelope reopened", r.squelch_env, 1.0, kResetTolerance);
    return failed;
}

/* Last error the stream logged, for the refusal text of a width a new rate cannot realize. */
static char g_last_error[512];

static void
capture_error_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_ERROR && text) {
        DSD_SNPRINTF(g_last_error, sizeof g_last_error, "%s", text);
    }
}

/*
 * A retune the device settles on another demod rate resolves the analog channel for that rate: the unset default
 * moves between the 16 kHz design and the legacy WIDE design, published as DSP-limited where the rate cannot fit
 * 16 kHz. An explicit width the new rate cannot realize is never run there without its channel filter: the retune is
 * refused with the validator's text, the capture goes back to the rate the width runs at, and the stream stays on the
 * centre it had, with that width still designed and published as filtered.
 */
static int
test_audio_monitor_retune_resolves_channel(void) {
    int failed = 0;
    rtl_stream_test_audio_reset_result r;

    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    failed |= expect_int_eq("default 48k -> 16k hook", rtl_stream_test_audio_monitor_retune(48000, 16000, 0, &r), 0);
    failed |= expect_int_eq("default retune lands", r.retune_refused, 0);
    failed |= expect_int_eq("default retune lands on its target", (int)(r.center_after == r.retune_target_hz), 1);
    failed |= expect_int_eq("default retune runs at 16 kHz", r.rate_out_after, 16000);
    failed |= expect_int_eq("default designs 16 kHz at 48 kHz", r.channel_lpf_width_before, 16000);
    failed |= expect_int_eq("default falls back to legacy WIDE at 16 kHz", r.channel_lpf_width_after, 0);
    failed |= expect_int_eq("legacy WIDE keeps the filter on", r.channel_lpf_enable_after, 1);
    failed |= expect_int_eq("legacy WIDE published as DSP-limited", r.published_lpf_on_after, 0);
    failed |= expect_int_eq("legacy WIDE publishes the width 16 kHz fits", r.published_width_after, 13200);
    failed |= expect_int_eq("default retune logs nothing", g_last_error[0] == '\0', 1);

    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq("default 48k -> 78125 hook", rtl_stream_test_audio_monitor_retune(48000, 78125, 0, &r), 0);
    failed |= expect_int_eq("default keeps 16 kHz at a forced 78125 Hz", r.channel_lpf_width_after, 16000);
    failed |= expect_int_eq("16 kHz at 78125 Hz is the width-driven filter", r.published_lpf_on_after, 1);
    failed |= expect_int_eq("16 kHz at 78125 Hz published", r.published_width_after, 16000);

    /* Past the 288-tap capacity (~102.7 kHz) the default takes the legacy WIDE plan again, which there is the 63-tap
     * fallback prototype: cut at a third of the rate, its passband at 128 kHz is 2 x (42667 - 3417) Hz, not the whole
     * 128 kHz DSP span an unfiltered channel would have. */
    DSD_MEMSET(&r, 0, sizeof r);
    failed |=
        expect_int_eq("default 48k -> 128000 hook", rtl_stream_test_audio_monitor_retune(48000, 128000, 0, &r), 0);
    failed |= expect_int_eq("default takes the legacy WIDE plan past the tap capacity", r.channel_lpf_width_after, 0);
    failed |= expect_int_eq("legacy WIDE keeps the filter on at 128000 Hz", r.channel_lpf_enable_after, 1);
    failed |= expect_int_eq("fallback prototype published as DSP-limited", r.published_lpf_on_after, 0);
    failed |= expect_int_eq("fallback prototype publishes the width it passes", r.published_width_after, 78499);

    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    failed |=
        expect_int_eq("explicit 48k -> 16k hook", rtl_stream_test_audio_monitor_retune(48000, 16000, 16000, &r), 0);
    failed |= expect_int_eq("explicit width designed at 48 kHz", r.channel_lpf_width_before, 16000);
    failed |= expect_int_eq("retune the explicit width does not fit is refused", r.retune_refused, 1);
    failed |= expect_int_eq("refused retune stays on the centre it had", (int)(r.center_after == r.center_before), 1);
    failed |= expect_int_eq("refused retune did not reach its target", (int)(r.center_after != r.retune_target_hz), 1);
    failed |= expect_int_eq("refused retune puts the 48 kHz rate back", r.rate_out_after, 48000);
    failed |= expect_int_eq("explicit width still designed", r.channel_lpf_width_after, 16000);
    failed |= expect_int_eq("explicit width keeps its filter on", r.channel_lpf_enable_after, 1);
    failed |= expect_int_eq("explicit width still published as filtered", r.published_lpf_on_after, 1);
    failed |= expect_int_eq("explicit width still published", r.published_width_after, 16000);
    failed |=
        expect_double_near("refused retune keeps the 48 kHz de-emphasis", r.deemph_a_after, r.deemph_a_before, 1e-9);
    failed |=
        expect_int_eq("refusal logged with the validator text",
                      std::strstr(g_last_error, "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate") != NULL, 1);
    failed |= expect_int_eq("refusal says the retune is refused",
                            std::strstr(g_last_error, "is refused; the front end stays on") != NULL, 1);
    if (failed != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }

    /* A width the new rate still fits lands with the retune, on the new rate. */
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    failed |=
        expect_int_eq("explicit 48k -> 24k hook", rtl_stream_test_audio_monitor_retune(48000, 24000, 12500, &r), 0);
    failed |= expect_int_eq("a width 24 kHz fits lands", r.retune_refused, 0);
    failed |= expect_int_eq("a width 24 kHz fits reaches its target", (int)(r.center_after == r.retune_target_hz), 1);
    failed |= expect_int_eq("a width 24 kHz fits runs at 24 kHz", r.rate_out_after, 24000);
    failed |= expect_int_eq("a width 24 kHz fits stays designed", r.channel_lpf_width_after, 12500);
    failed |= expect_int_eq("a width 24 kHz fits published as filtered", r.published_lpf_on_after, 1);
    failed |= expect_int_eq("a width 24 kHz fits logs nothing", g_last_error[0] == '\0', 1);

    /* An unchanged rate does not re-log or re-resolve anything. */
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    failed |=
        expect_int_eq("explicit same-rate hook", rtl_stream_test_audio_monitor_retune(48000, 48000, 12500, &r), 0);
    failed |= expect_int_eq("same-rate retune lands", r.retune_refused, 0);
    failed |= expect_int_eq("same-rate explicit width kept", r.channel_lpf_width_after, 12500);
    failed |= expect_int_eq("same-rate retune logs nothing", g_last_error[0] == '\0', 1);
    return failed;
}

/*
 * A retune profile for the target decides the analog channel the retune lands on, so the running monitor's explicit
 * width is held to the new rate only where the profile leaves the monitor as it is: a switch to the digital family, or
 * to an analog width the new rate fits, lands with the retune; an analog width that does not fit is refused on its own
 * and leaves the monitor's width, which the new rate does not fit either, so the retune is refused.
 */
static int
test_audio_monitor_retune_profile_decides(void) {
    int failed = 0;
    rtl_stream_test_retune_profile_landing_result r;

    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "digital profile hook",
        rtl_stream_test_audio_monitor_retune_with_profile(48000, 16000, 16000, DSD_RX_FAMILY_DIGITAL, 0, &r), 0);
    failed |= expect_int_eq("a switch to digital lands", r.retune_refused, 0);
    failed |= expect_int_eq("a switch to digital runs at the new rate", r.rate_out_after, 16000);
    failed |= expect_int_eq("a switch to digital leaves the analog family", r.analog_family_after, 0);

    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "fitting analog profile hook",
        rtl_stream_test_audio_monitor_retune_with_profile(48000, 16000, 16000, DSD_RX_FAMILY_ANALOG, 12500, &r), 0);
    failed |= expect_int_eq("an analog width the new rate fits lands", r.retune_refused, 0);
    failed |= expect_int_eq("that width runs at the new rate", r.rate_out_after, 16000);
    failed |= expect_int_eq("on the monitor", r.monitor_after, 1);
    failed |= expect_int_eq("with the profile's width", r.channel_lpf_width_after, 12500);

    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    failed |= expect_int_eq(
        "unfitting analog profile hook",
        rtl_stream_test_audio_monitor_retune_with_profile(48000, 16000, 16000, DSD_RX_FAMILY_ANALOG, 25000, &r), 0);
    failed |= expect_int_eq("an analog width the new rate does not fit is refused", r.retune_refused, 1);
    failed |= expect_int_eq("the 48 kHz rate is put back", r.rate_out_after, 48000);
    failed |= expect_int_eq("the monitor keeps running", r.monitor_after, 1);
    failed |= expect_int_eq("with its own width", r.channel_lpf_width_after, 16000);
    failed |=
        expect_int_eq("the refusal names the monitor's width",
                      std::strstr(g_last_error, "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate") != NULL, 1);
    failed |= expect_int_eq("a retune refused whole refuses no profile of its own", r.profile_refused, 0);

    /* A width the new rate does not fit over a monitor width it does (48 kHz to 24 kHz, a 25 kHz row after a 12.5 kHz
       one): the retune lands with the monitor as it was, and the refusal of its own profile is reported, so the
       retune fails and a scanner does not commit the row it asked for (issue #526). */
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "profile refused where it lands hook",
        rtl_stream_test_audio_monitor_retune_with_profile(48000, 24000, 12500, DSD_RX_FAMILY_ANALOG, 25000, &r), 0);
    failed |= expect_int_eq("the landing check passes the monitor's width", r.retune_refused, 0);
    failed |= expect_int_eq("the retune's own profile is refused", r.profile_refused, 1);
    failed |= expect_int_eq("at the rate it landed on", r.rate_out_after, 24000);
    failed |= expect_int_eq("the monitor keeps its width", r.channel_lpf_width_after, 12500);

    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "profile landing hook",
        rtl_stream_test_audio_monitor_retune_with_profile(48000, 24000, 12500, DSD_RX_FAMILY_ANALOG, 16000, &r), 0);
    failed |= expect_int_eq("a profile the rate fits is no refusal", r.profile_refused, 0);
    failed |= expect_int_eq("and runs its width", r.channel_lpf_width_after, 16000);
    return failed;
}

/*
 * A refused retune puts the capture back, but a device can still report another rate once it is reprogrammed. With no
 * receive profile left that runs the explicit width, the stream stops, as a start at that rate fails: the stop is
 * logged with the validator's text and reported as a configuration failure of the input, never run without the
 * channel filter the width asked for.
 */
static int
test_audio_monitor_rate_not_restored_stops(void) {
    int failed = 0;
    rtl_stream_test_rate_not_restored_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    failed |= expect_int_eq("rate not restored hook",
                            rtl_stream_test_audio_monitor_rate_not_restored(48000, 16000, 16000, &r), 0);
    failed |= expect_int_eq("the retune was refused first", r.retune_refused, 1);
    failed |= expect_int_eq("the stream is stopped", r.exit_requested, 1);
    failed |= expect_int_eq("reported as a configuration failure", r.input_failure_kind,
                            (int)DSD_INPUT_FAILURE_CONFIGURATION);
    failed |=
        expect_int_eq("stop logged with the validator text",
                      std::strstr(g_last_error, "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate") != NULL, 1);
    failed |= expect_int_eq("stop says the stream stops", std::strstr(g_last_error, "the stream stops") != NULL, 1);
    failed |= expect_int_eq("exit request cleared again", (int)dsd_exitflag_load(), 0);
    if (failed != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }
    return failed;
}

/* One refused retune whose device answers the capture put back with @p frequency_rc and @p rate_rc. */
static int
expect_refused_restore(const char* label, int frequency_rc, int rate_rc, int want_stop, int want_code) {
    int failed = 0;
    char what[160];
    rtl_stream_test_restore_failure_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    DSD_SNPRINTF(what, sizeof what, "%s: hook", label);
    failed |= expect_int_eq(
        what, rtl_stream_test_audio_monitor_restore_failure(48000, 16000, 16000, frequency_rc, rate_rc, &r), 0);
    DSD_SNPRINTF(what, sizeof what, "%s: the retune was refused", label);
    failed |= expect_int_eq(what, r.retune_refused, 1);
    DSD_SNPRINTF(what, sizeof what, "%s: the device was asked for the capture it ran", label);
    failed |= expect_int_eq(what, r.program_calls, 1);
    DSD_SNPRINTF(what, sizeof what, "%s: capture frequency put back", label);
    failed |= expect_int_eq(what, r.program_freq_hz == r.capture_freq_before_hz, 1);
    DSD_SNPRINTF(what, sizeof what, "%s: capture rate put back", label);
    failed |= expect_int_eq(what, r.program_rate_hz == r.capture_rate_before_hz && r.program_rate_hz > 0U, 1);
    DSD_SNPRINTF(what, sizeof what, "%s: the rate the width runs at is back", label);
    failed |= expect_int_eq(what, r.rate_out_after, 48000);
    DSD_SNPRINTF(what, sizeof what, "%s: stream stop", label);
    failed |= expect_int_eq(what, r.exit_requested, want_stop);
    DSD_SNPRINTF(what, sizeof what, "%s: input failure", label);
    failed |= expect_int_eq(what, r.input_failure_kind,
                            want_stop ? (int)DSD_INPUT_FAILURE_DEVICE : (int)DSD_INPUT_FAILURE_NONE);
    DSD_SNPRINTF(what, sizeof what, "%s: device return code reported", label);
    failed |= expect_int_eq(what, r.input_failure_code, want_code);
    if (want_stop) {
        DSD_SNPRINTF(what, sizeof what, "%s: stop logged", label);
        failed |= expect_int_eq(what,
                                std::strstr(g_last_error, "could not be put back") != NULL
                                    && std::strstr(g_last_error, "the stream stops") != NULL,
                                1);
    }
    DSD_SNPRINTF(what, sizeof what, "%s: exit request cleared again", label);
    failed |= expect_int_eq(what, (int)dsd_exitflag_load(), 0);
    if (failed != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }
    return failed;
}

/*
 * A refused retune has already programmed the device for the retune's capture, so it has to program the capture it
 * ran back. When the device refuses either call, it may still be on the retune's capture while the stream finalizes on
 * the centre and rate it kept, and nothing then holds the monitor's width to what the device delivers: the stream
 * stops, logged and reported as a device failure of the input with the device's return code. A device that takes both
 * back keeps the stream running on the capture it had.
 */
static int
test_audio_monitor_refused_restore_failure_stops(void) {
    int failed = 0;
    failed |= expect_refused_restore("device takes the capture back", 0, 0, 0, 0);
    failed |= expect_refused_restore("device refuses the frequency", -5, 0, 1, -5);
    failed |= expect_refused_restore("device refuses the rate", 0, -7, 1, -7);
    failed |= expect_refused_restore("device refuses both", -5, -7, 1, -5);
    return failed;
}

/*
 * A retune refused for the monitor's explicit width fails, whatever centre it finalized on. A retune whose target is
 * the centre the stream already runs on (or one made before any centre was applied, which the refusal keeps as the
 * target) finalizes on that very centre without its profile, and must not complete as tuned.
 */
static int
test_audio_monitor_refused_retune_completes_failed(void) {
    int failed = 0;
    rtl_stream_test_retune_completion_result r;
    const uint32_t running_hz = 460125000U;
    const uint32_t next_hz = 460150000U;

    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "same-centre refused retune hook",
        rtl_stream_test_audio_monitor_retune_completion(48000, 16000, 16000, running_hz, running_hz, &r), 0);
    failed |= expect_int_eq("same-centre retune refused", r.retune_refused, 1);
    failed |=
        expect_int_eq("same-centre refusal finalizes on the target centre", (int)(r.center_after == running_hz), 1);
    failed |= expect_int_eq("same-centre refusal completes failed", r.completion_result, RTL_STREAM_TUNE_FAILED);

    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq("no-centre refused retune hook",
                            rtl_stream_test_audio_monitor_retune_completion(48000, 16000, 16000, 0U, next_hz, &r), 0);
    failed |= expect_int_eq("no-centre retune refused", r.retune_refused, 1);
    failed |= expect_int_eq("no-centre refusal keeps the target centre", (int)(r.center_after == next_hz), 1);
    failed |= expect_int_eq("no-centre refusal completes failed", r.completion_result, RTL_STREAM_TUNE_FAILED);

    DSD_MEMSET(&r, 0, sizeof r);
    failed |=
        expect_int_eq("next-centre refused retune hook",
                      rtl_stream_test_audio_monitor_retune_completion(48000, 16000, 16000, running_hz, next_hz, &r), 0);
    failed |= expect_int_eq("next-centre retune refused", r.retune_refused, 1);
    failed |= expect_int_eq("next-centre refusal stays on the running centre", (int)(r.center_after == running_hz), 1);
    failed |= expect_int_eq("next-centre refusal completes failed", r.completion_result, RTL_STREAM_TUNE_FAILED);

    /* A same-centre retune the width still fits lands and completes as tuned. */
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "same-centre fitting retune hook",
        rtl_stream_test_audio_monitor_retune_completion(48000, 24000, 12500, running_hz, running_hz, &r), 0);
    failed |= expect_int_eq("same-centre fitting retune lands", r.retune_refused, 0);
    failed |= expect_int_eq("same-centre fitting retune completes ok", r.completion_result, RTL_STREAM_TUNE_OK);
    return failed;
}

/* The analog family, kind and width travel with a retune profile bound to its target frequency. */
static int
test_retune_profile_carries_analog_fields(void) {
    int failed = 0;
    rtl_stream_test_retune_analog_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "analog retune hook",
        rtl_stream_test_retune_analog_profile(853012500U, DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, &r), 0);
    failed |= expect_int_eq("analog profile queued", r.queued_rc, 0);
    failed |= expect_int_eq("analog profile taken for its target", r.taken, 1);
    failed |= expect_int_eq("profile carries the family", r.profile_family, DSD_RX_FAMILY_ANALOG);
    failed |= expect_int_eq("profile carries the kind", r.profile_kind, DSD_ANALOG_DEMOD_FM);
    failed |= expect_int_eq("profile carries the width", r.profile_width_hz, 12500);
    failed |= expect_int_eq("profile bound to its target", (int)r.profile_target_hz, 853012500);
    failed |= expect_int_eq("retune applied the analog family", r.applied_family, 1);
    failed |= expect_int_eq("retune applied the kind", r.applied_kind, DSD_ANALOG_DEMOD_FM);
    failed |= expect_int_eq("retune applied the width", r.applied_width_hz, 12500);
    failed |= expect_int_eq("retune switched to monitor output", r.applied_output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |= expect_int_eq("explicit width enabled the filter", r.applied_lpf_enable, 1);
    failed |= expect_int_eq("profile for another target left alone", r.other_target_left_alone, 1);

    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "refused analog retune hook",
        rtl_stream_test_retune_analog_profile(853012500U, DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_AM, 0, 0, &r), 0);
    failed |= expect_int_eq("AM retune profile refused", r.queued_rc, -1);
    failed |= expect_int_eq("refused profile not queued", r.taken, 0);

    /* A symbol profile queued for the same target first (the documented combined shape) must not pull the analog
     * row back off the monitor: the analog family has no symbol clock, so none of it applies. */
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "combined analog retune hook",
        rtl_stream_test_retune_analog_profile(853012500U, DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 1, &r), 0);
    failed |= expect_int_eq("combined profile queued", r.queued_rc, 0);
    failed |= expect_int_eq("combined profile taken", r.taken, 1);
    failed |= expect_int_eq("combined retune ends on the analog family", r.applied_family, 1);
    failed |=
        expect_int_eq("combined retune keeps monitor output", r.applied_output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |= expect_int_eq("combined retune leaves CQPSK off", r.applied_cqpsk_enable, 0);
    failed |= expect_int_eq("combined retune keeps the FM discriminator", r.applied_demod_is_fm, 1);
    failed |= expect_int_eq("combined retune keeps the width", r.applied_width_hz, 12500);

    /* A digital switch bound to the same target still applies its symbol profile after the family change. */
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq(
        "combined digital retune hook",
        rtl_stream_test_retune_analog_profile(853012500U, DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0, 1, &r), 0);
    failed |= expect_int_eq("combined digital profile taken", r.taken, 1);
    failed |= expect_int_eq("digital retune stays off the analog family", r.applied_family, 0);
    failed |= expect_int_eq("digital retune applies the CQPSK profile", r.applied_cqpsk_enable, 1);
    failed |= expect_int_eq("digital retune runs CQPSK output", r.applied_output_kind, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK);
    return failed;
}

/* A retune profile queued with no stream running is checked only against the kind and range rules. When it lands, the
 * demod rate is known: a width that rate cannot realize is refused there with the validator's text, once, and the
 * front end keeps its receive profile instead of running an analog channel with no filter. */
static int
test_retune_profile_width_checked_at_landing_rate(void) {
    int failed = 0;
    rtl_stream_test_retune_analog_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    failed |= expect_int_eq("12 kHz retune hook",
                            rtl_stream_test_retune_analog_profile_at_rate(853012500U, 12000, DSD_RX_FAMILY_ANALOG,
                                                                          DSD_ANALOG_DEMOD_FM, 16000, 0, &r),
                            0);
    failed |= expect_int_eq("no-stream profile queued", r.queued_rc, 0);
    failed |= expect_int_eq("no-stream profile taken", r.taken, 1);
    failed |= expect_int_eq("unrealizable width keeps the digital family", r.applied_family, 0);
    failed |= expect_int_eq("unrealizable width keeps the digital output",
                            r.applied_output_kind != DSD_DEMOD_OUTPUT_AUDIO_MONITOR, 1);
    failed |= expect_int_eq("no analog width installed", r.applied_width_hz, 0);
    failed |=
        expect_int_eq("landing refusal carries the validator text",
                      std::strstr(g_last_error, "NFM bandwidth 16 kHz does not fit the 12 kHz DSP rate") != NULL, 1);

    /* A scanner revisiting the same target does not repeat the message. */
    g_last_error[0] = '\0';
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq("repeat 12 kHz retune hook",
                            rtl_stream_test_retune_analog_profile_at_rate(853012500U, 12000, DSD_RX_FAMILY_ANALOG,
                                                                          DSD_ANALOG_DEMOD_FM, 16000, 0, &r),
                            0);
    failed |= expect_int_eq("repeat refusal keeps the digital family", r.applied_family, 0);
    failed |= expect_int_eq("repeat refusal is not logged again", g_last_error[0] == '\0', 1);

    /* A width the rate fits lands as usual. */
    DSD_MEMSET(&r, 0, sizeof r);
    failed |= expect_int_eq("fitting 12 kHz retune hook",
                            rtl_stream_test_retune_analog_profile_at_rate(853012500U, 12000, DSD_RX_FAMILY_ANALOG,
                                                                          DSD_ANALOG_DEMOD_FM, 8000, 0, &r),
                            0);
    failed |= expect_int_eq("fitting width applies the analog family", r.applied_family, 1);
    failed |= expect_int_eq("fitting width reaches the filter", r.applied_width_hz, 8000);
    return failed;
}

static int
expect_landing(const char* stage, const rtl_stream_test_retune_landing* r, int family, int width_hz, int output_kind) {
    char label[160];
    int failed = 0;
    DSD_SNPRINTF(label, sizeof label, "%s: profile queued", stage);
    failed |= expect_int_eq(label, r->queued_rc, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: profile taken", stage);
    failed |= expect_int_eq(label, r->taken, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: family", stage);
    failed |= expect_int_eq(label, r->applied_family, family);
    DSD_SNPRINTF(label, sizeof label, "%s: output", stage);
    failed |= expect_int_eq(label, r->applied_output_kind, output_kind);
    if (width_hz > 0) {
        DSD_SNPRINTF(label, sizeof label, "%s: channel width", stage);
        failed |= expect_int_eq(label, r->applied_width_hz, width_hz);
    }
    return failed;
}

/* Scan rows retune one after another on one running stream (issue #526), and each lands the receive family and width
 * its own retune profile carries: an nfm row with its own width, a digital row after it (its symbol profile on the
 * digital family), and an nfm row at the default width. */
static int
test_retune_profiles_land_each_rows_family_and_width(void) {
    const rtl_stream_test_retune_step steps[] = {
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NONE, 0},
        {DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0, 1, -1, -1, RTL_STREAM_TEST_QUEUED_NONE, 0},
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 0, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NONE, 0},
    };
    rtl_stream_test_retune_landing r[3];
    DSD_MEMSET(r, 0, sizeof r);
    int failed = expect_int_eq("row sequence hook", rtl_stream_test_retune_profile_sequence(steps, 3U, r), 0);
    failed |= expect_landing("nfm row at 12.5 kHz", &r[0], 1, 12500, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |= expect_landing("digital row after it", &r[1], 0, 0, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK);
    failed |= expect_int_eq("digital row runs its CQPSK profile", r[1].applied_cqpsk_enable, 1);
    failed |= expect_landing("nfm row at the default width", &r[2], 1, DSD_ANALOG_NFM_WIDTH_DEFAULT_HZ,
                             DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |= expect_int_eq("nfm row after a digital row leaves CQPSK off", r[2].applied_cqpsk_enable, 0);
    return failed;
}

/* A live family request made after a row's retune profile was taken, while that retune is still in flight on the
 * device, is newer than the profile: a scanner leaving meanwhile puts the configured family back that way. The retune
 * then lands on its channel without switching the front end to the family the row wanted, and without the symbol
 * profile queued with it. A request made before the profile was queued is older, and the profile lands over it. */
static int
test_retune_family_superseded_by_a_later_live_request(void) {
    /* A digital session leaves -Y while an nfm row's retune is in flight. */
    const rtl_stream_test_retune_step leave_to_digital[] = {
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, -1, DSD_RX_FAMILY_DIGITAL, RTL_STREAM_TEST_QUEUED_NONE,
         0},
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, DSD_RX_FAMILY_DIGITAL, -1, RTL_STREAM_TEST_QUEUED_NONE,
         0},
    };
    rtl_stream_test_retune_landing r[2];
    DSD_MEMSET(r, 0, sizeof r);
    int failed =
        expect_int_eq("leave-to-digital hook", rtl_stream_test_retune_profile_sequence(leave_to_digital, 2U, r), 0);
    failed |=
        expect_landing("nfm retune after a later digital request", &r[0], 0, 0, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR);
    failed |=
        expect_landing("nfm retune after an earlier digital request", &r[1], 1, 12500, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);

    /* An -fA session leaves -Y while a digital row's retune (after an nfm row) is in flight. */
    const rtl_stream_test_retune_step leave_to_analog[] = {
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NONE, 0},
        {DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0, 1, -1, DSD_RX_FAMILY_ANALOG, RTL_STREAM_TEST_QUEUED_NONE, 0},
    };
    DSD_MEMSET(r, 0, sizeof r);
    failed |= expect_int_eq("leave-to-analog hook", rtl_stream_test_retune_profile_sequence(leave_to_analog, 2U, r), 0);
    failed |= expect_landing("nfm row", &r[0], 1, 12500, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |=
        expect_landing("digital retune after a later analog request", &r[1], 1, 12500, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |= expect_int_eq("superseded symbol profile not applied", r[1].applied_cqpsk_enable, 0);
    return failed;
}

/* A width command the decoder drained just before the scanner advanced queues a live analog request the demod thread
 * has not taken when the next row's retune lands (issue #526). That request is older than the retune's family: the
 * retune retires it, so the demod thread's next block boundary does not put the front end back on the analog family
 * over the digital row now on air, and the request settles as replaced, never refused. A symbol profile queued after
 * the retune's profile is newer, and still applies. */
static int
test_retune_family_retires_older_queued_requests(void) {
    const rtl_stream_test_retune_step steps[] = {
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NONE, 0},
        {DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0, 1, -1, -1, RTL_STREAM_TEST_QUEUED_NFM_WIDTH, 16000},
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NONE, 0},
        {DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0, 1, -1, -1, RTL_STREAM_TEST_QUEUED_SYMBOL_AFTER, 16000},
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 11250, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NFM_WIDTH, 20000},
    };
    rtl_stream_test_retune_landing r[5];
    DSD_MEMSET(r, 0, sizeof r);
    int failed = expect_int_eq("queued request hook", rtl_stream_test_retune_profile_sequence(steps, 5U, r), 0);
    failed |= expect_landing("digital row over a queued width", &r[1], 0, 0, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK);
    failed |= expect_int_eq("still digital after the boundary", r[1].boundary_family, 0);
    failed |= expect_int_eq("still on its CQPSK profile", r[1].boundary_output_kind, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK);
    failed |= expect_int_eq("the queued width settled as replaced", r[1].queued_request_outcome,
                            RTL_STREAM_RX_REQUEST_SETTLED);

    failed |= expect_landing("digital row with a later symbol profile", &r[3], 0, 0, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK);
    failed |= expect_int_eq("the later profile keeps the digital family", r[3].boundary_family, 0);
    failed |=
        expect_int_eq("and applies at the boundary", r[3].boundary_output_kind, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR);
    failed |=
        expect_int_eq("the older width settled with it", r[3].queued_request_outcome, RTL_STREAM_RX_REQUEST_SETTLED);

    failed |= expect_landing("nfm row over a queued width", &r[4], 1, 11250, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |= expect_int_eq("keeps the row's width after the boundary", r[4].boundary_width_hz, 11250);
    failed |= expect_int_eq("on the monitor", r[4].boundary_output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    return failed;
}

/* A live request the decoder makes while a row's retune lands (issue #526), after the controller found the retune's
 * family not superseded and before it retired the requests older than that family, is newer than the family all the
 * same: a scan leave back to a digital session, or a width command. The retune lands no family then, as it would for a
 * request made before it landed, and the demod thread's next block boundary applies the request. */
static int
test_retune_family_superseded_while_it_lands(void) {
    const rtl_stream_test_retune_step steps[] = {
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NONE, 0},
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 11250, 0, -1, -1, RTL_STREAM_TEST_QUEUED_DIGITAL_AT_LANDING, 0},
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, -1, -1, RTL_STREAM_TEST_QUEUED_NFM_WIDTH_AT_LANDING,
         16000},
    };
    rtl_stream_test_retune_landing r[3];
    DSD_MEMSET(r, 0, sizeof r);
    int failed = expect_int_eq("landing request hook", rtl_stream_test_retune_profile_sequence(steps, 3U, r), 0);
    failed |= expect_landing("nfm row", &r[0], 1, 12500, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);

    failed |= expect_int_eq("leave while landing: the row's width not applied", r[1].applied_width_hz, 12500);
    failed |= expect_int_eq("leave while landing: digital at the boundary", r[1].boundary_family, 0);
    failed |= expect_int_eq("leave while landing: on the session's C4FM profile", r[1].boundary_output_kind,
                            DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR);
    failed |= expect_int_eq("leave while landing: the request settled", r[1].queued_request_outcome,
                            RTL_STREAM_RX_REQUEST_SETTLED);

    failed |= expect_int_eq("width while landing: the row's family not applied", r[2].applied_family, 0);
    failed |= expect_int_eq("width while landing: analog at the boundary", r[2].boundary_family, 1);
    failed |= expect_int_eq("width while landing: at the command's width", r[2].boundary_width_hz, 16000);
    failed |=
        expect_int_eq("width while landing: on the monitor", r[2].boundary_output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    failed |= expect_int_eq("width while landing: the request settled", r[2].queued_request_outcome,
                            RTL_STREAM_RX_REQUEST_SETTLED);
    return failed;
}

/* The live family requests the stream accepts are counted, running stream or not, and a refused one is not (issue
 * #526): the -Y scanner compares the count across a row's outstanding retune, whose family a request counted meanwhile
 * has superseded. */
static int
test_live_family_request_count(void) {
    const uint32_t before = rtl_stream_live_family_request_count();
    int failed = expect_int_eq("digital request accepted",
                               rtl_stream_request_analog_profile(DSD_RX_FAMILY_DIGITAL, DSD_ANALOG_DEMOD_FM, 0), 0);
    failed |= expect_int_eq("accepted request counted", (int)(rtl_stream_live_family_request_count() - before), 1);
    failed |= expect_int_eq("width below the NFM range refused",
                            rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 1000), -1);
    failed |= expect_int_eq("refused request not counted", (int)(rtl_stream_live_family_request_count() - before), 1);
    const rtl_stream_test_retune_step supersede[] = {
        {DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 12500, 0, DSD_RX_FAMILY_DIGITAL, DSD_RX_FAMILY_DIGITAL,
         RTL_STREAM_TEST_QUEUED_NONE, 0},
    };
    rtl_stream_test_retune_landing r[1];
    DSD_MEMSET(r, 0, sizeof r);
    failed |= expect_int_eq("supersede hook", rtl_stream_test_retune_profile_sequence(supersede, 1U, r), 0);
    failed |=
        expect_int_eq("requests around a retune counted", (int)(rtl_stream_live_family_request_count() - before), 3);
    return failed;
}

int
main(void) {
    dsd_neo_log_set_tap(capture_error_log, NULL);
    size_t used_after = 0U;
    size_t ring_pending = 0U;
    int cache_pending = 0;
    int drained = 0;
    uint32_t generation_before = 0U;
    uint32_t generation_after = 0U;

    /*
     * Retune preparation is split between output drain state, cached symbols,
     * and replay event boundaries. The helper calls below cover each state
     * transition without starting a live RTL worker thread.
     */
    int rc = rtl_stream_test_prepare_reconfigure_input(8U, &used_after, &generation_before, &generation_after);

    int failed = 0;
    failed |= expect_int_eq("prepare reconfigure input rc", rc, 0);
    failed |= expect_size_eq("retune gate clears queued output", used_after, 0U);
    failed |= expect_generation_changed("retune gate bumps output generation", generation_before, generation_after);

    rc = rtl_stream_test_retune_output_pending(0U, 3, &ring_pending, &cache_pending, &drained);
    failed |= expect_int_eq("retune pending helper rc", rc, 0);
    failed |= expect_size_eq("retune drain sees empty ring", ring_pending, 0U);
    failed |= expect_int_eq("retune drain sees cached symbols", cache_pending, 3);
    failed |= expect_int_eq("cached symbols do not block retune drain", drained, 1);

    rc = rtl_stream_test_retune_output_pending(5U, 0, &ring_pending, &cache_pending, &drained);
    failed |= expect_int_eq("retune ring pending helper rc", rc, 0);
    failed |= expect_size_eq("retune drain sees queued ring", ring_pending, 5U);
    failed |= expect_int_eq("retune drain sees empty cache", cache_pending, 0);
    failed |= expect_int_eq("queued ring keeps retune drain open", drained, 0);

    rc = rtl_stream_test_retune_output_pending(0U, 0, &ring_pending, &cache_pending, &drained);
    failed |= expect_int_eq("retune drained helper rc", rc, 0);
    failed |= expect_size_eq("retune drain sees drained ring", ring_pending, 0U);
    failed |= expect_int_eq("retune drain sees drained cache", cache_pending, 0);
    failed |= expect_int_eq("retune drain reports drained", drained, 1);

    cache_pending = -1;
    rc = rtl_stream_test_tune_result_output_drain(RTL_STREAM_TUNE_TIMEOUT, 5U, 3, &used_after, &cache_pending,
                                                  &generation_before, &generation_after);
    failed |= expect_int_eq("timeout tune drain helper rc", rc, 0);
    failed |= expect_size_eq("timeout tune leaves output for controller boundary", used_after, 5U);
    failed |= expect_int_eq("timeout tune leaves cache for controller boundary", cache_pending, 3);
    failed |= expect_generation_eq("timeout tune defers output generation", generation_before, generation_after);

    int read_while_pending = -1;
    int read_after_failed_completion = -1;
    int read_after_recovery = -1;
    size_t used_while_pending = 0U;
    generation_before = 0U;
    generation_after = 0U;
    rc = rtl_stream_test_tune_timeout_read_gate(5U, &read_while_pending, &used_while_pending,
                                                &read_after_failed_completion, &read_after_recovery, &generation_before,
                                                &generation_after);
    failed |= expect_int_eq("tune timeout read gate helper rc", rc, 0);
    failed |= expect_int_eq("tune timeout blocks queued output", read_while_pending, 0);
    failed |= expect_size_eq("tune timeout preserves queued output", used_while_pending, 5U);
    failed |=
        expect_generation_changed("tune timeout invalidates handed-off samples", generation_before, generation_after);
    failed |= expect_int_eq("failed completion reopens recovered output", read_after_failed_completion, 1);
    failed |= expect_int_eq("successful recovery reopens queued output", read_after_recovery, 1);

    cache_pending = -1;
    rc = rtl_stream_test_tune_result_output_drain(RTL_STREAM_TUNE_DEFERRED, 5U, 3, &used_after, &cache_pending,
                                                  &generation_before, &generation_after);
    failed |= expect_int_eq("deferred tune drain helper rc", rc, 0);
    failed |= expect_size_eq("deferred tune leaves queued output", used_after, 5U);
    failed |= expect_int_eq("deferred tune leaves cached symbols", cache_pending, 3);
    failed |= expect_generation_eq("deferred tune keeps output generation", generation_before, generation_after);
    failed |= expect_int_eq("ok completion result keeps tune ok",
                            dsd_rtl_stream_test_tune_completion_result(RTL_STREAM_TUNE_OK, RTL_STREAM_TUNE_OK),
                            RTL_STREAM_TUNE_OK);
    failed |= expect_int_eq("failed completion result maps tune failed",
                            dsd_rtl_stream_test_tune_completion_result(RTL_STREAM_TUNE_OK, RTL_STREAM_TUNE_FAILED),
                            RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("timeout completion result keeps timeout",
                            dsd_rtl_stream_test_tune_completion_result(RTL_STREAM_TUNE_TIMEOUT, RTL_STREAM_TUNE_FAILED),
                            RTL_STREAM_TUNE_TIMEOUT);
    failed |= expect_int_eq("committed retune failure reports tune ok",
                            dsd_rtl_stream_test_manual_retune_completion_result(-5, 1, 0, 855000000U, 855000000U),
                            RTL_STREAM_TUNE_OK);
    failed |= expect_int_eq("uncommitted retune failure reports tune failed",
                            dsd_rtl_stream_test_manual_retune_completion_result(-5, 1, 0, 855000000U, 851000000U),
                            RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("failed retune without reconfigure reports tune failed",
                            dsd_rtl_stream_test_manual_retune_completion_result(-5, 0, 0, 855000000U, 855000000U),
                            RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("retune refused for its analog width on its own centre reports tune failed",
                            dsd_rtl_stream_test_manual_retune_completion_result(-1, 1, 1, 855000000U, 855000000U),
                            RTL_STREAM_TUNE_FAILED);

    /*
     * Failed tune and capture-setting paths reconcile staged frequency/rate state
     * back to the applied RTL configuration, while accepted PPM changes survive a
     * later retune failure.
     */
    long int reconciled_opts_freq = 0;
    uint32_t reconciled_capture_freq = 0U;
    rc = dsd_rtl_stream_test_tune_failure_reconciles_applied(855000000U, 851000000U, &reconciled_opts_freq,
                                                             &reconciled_capture_freq);
    failed |= expect_int_eq("failed tune reconcile helper rc", rc, 0);
    failed |= expect_int_eq("failed tune rolls caller freq back to applied", (int)reconciled_opts_freq, 851000000);
    failed |= expect_int_eq("failed tune restores applied capture center", (int)reconciled_capture_freq, 851240000);

    int first_completion_result = -1;
    int second_completion_result = -1;
    rc = dsd_rtl_stream_test_retune_completion_result_binding(&first_completion_result, &second_completion_result);
    failed |= expect_int_eq("retune completion result binding helper rc", rc, 0);
    failed |= expect_int_eq("first completion keeps failed result", first_completion_result, RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("second completion keeps ok result", second_completion_result, RTL_STREAM_TUNE_OK);

    TaggedTuneCompletionState tagged_completion = {};
    uint32_t owner_freq_hz = 0U;
    uint32_t owner_profile_freq_hz = 0U;
    uint64_t owner_token = 0U;
    int owner_completion_result = RTL_STREAM_TUNE_OK;
    const uint64_t expected_owner_token = UINT64_C(0x1111222233334444);
    rtl_stream_register_tune_completion_callback(tagged_tune_completion, &tagged_completion);
    rc = rtl_stream_test_tagged_retune_ownership(expected_owner_token, 0U, RTL_STREAM_TUNE_FAILED, &owner_freq_hz,
                                                 &owner_profile_freq_hz, &owner_token, &owner_completion_result);
    rtl_stream_register_tune_completion_callback(nullptr, nullptr);
    failed |= expect_int_eq("tagged retune ownership helper rc", rc, 0);
    failed |= expect_int_eq("untagged contender preserves owner frequency", (int)owner_freq_hz, 855000000);
    failed |= expect_int_eq("untagged contender preserves owner profile", (int)owner_profile_freq_hz, 855000000);
    failed |= expect_u64_eq("untagged contender preserves owner token", owner_token, expected_owner_token);
    failed |= expect_int_eq("failed owner completion is retained", owner_completion_result, RTL_STREAM_TUNE_FAILED);
    failed |= expect_int_eq("failed owner completion is published once", tagged_completion.calls, 1);
    failed |= expect_u64_eq("failed owner completion keeps request token", tagged_completion.request_id,
                            expected_owner_token);
    failed |=
        expect_int_eq("failed owner callback keeps terminal result", tagged_completion.result, RTL_STREAM_TUNE_FAILED);

    failed |= expect_int_eq("retune without controller is rejected",
                            dsd_rtl_stream_test_retune_without_controller_rejected(), 1);

    uint32_t full_restore_freq_hz = 0U;
    uint32_t full_restore_rate_hz = 0U;
    int full_restore_rate_out_hz = 0;
    uint32_t partial_restore_freq_hz = 0U;
    uint32_t partial_restore_rate_hz = 0U;
    int partial_restore_rate_out_hz = 0;
    rc = dsd_rtl_stream_test_capture_settings_failure_restore(&full_restore_freq_hz, &full_restore_rate_hz,
                                                              &full_restore_rate_out_hz, &partial_restore_freq_hz,
                                                              &partial_restore_rate_hz, &partial_restore_rate_out_hz);
    failed |= expect_int_eq("capture settings restore helper rc", rc, 0);
    failed |= expect_int_eq("frequency failure restores applied frequency", (int)full_restore_freq_hz, 851000000);
    failed |= expect_int_eq("frequency failure restores staged rate", (int)full_restore_rate_hz, 960000);
    failed |= expect_int_eq("frequency failure restores demod rate", full_restore_rate_out_hz, 48000);
    failed |= expect_int_eq("partial retune keeps applied frequency", (int)partial_restore_freq_hz, 855000000);
    failed |= expect_int_eq("partial retune restores prior rate", (int)partial_restore_rate_hz, 960000);
    failed |= expect_int_eq("partial retune restores demod rate", partial_restore_rate_out_hz, 48000);

    int ppm_after_failure = 0;
    failed |= expect_int_eq("accepted ppm store helper rc",
                            dsd_rtl_stream_test_ppm_store_if_applied(0, -7, &ppm_after_failure), 0);
    failed |= expect_int_eq("accepted ppm survives retune failure", ppm_after_failure, -7);
    failed |= expect_int_eq("failed ppm store helper rc",
                            dsd_rtl_stream_test_ppm_store_if_applied(-5, 11, &ppm_after_failure), 0);
    failed |= expect_int_eq("failed ppm keeps previous runtime ppm", ppm_after_failure, 3);

    /*
     * Clear-output and reacquire helpers coordinate the sample ring, cached
     * symbol count, FSK modem history, and generation counters used by the
     * demodulator thread.
     */
    cache_pending = -1;
    rc = rtl_stream_test_clear_output(7U, 3, &used_after, &cache_pending, &generation_before, &generation_after);
    failed |= expect_int_eq("clear output helper rc", rc, 0);
    failed |= expect_generation_changed("clear output bumps generation", generation_before, generation_after);
    failed |= expect_size_eq("clear output clears queued ring", used_after, 0U);
    failed |= expect_int_eq("clear output resets cached symbols", cache_pending, 0);

    int have_prev_after_clear = -1;
    int reset_consumed = -1;
    int have_prev_after_consume = -1;
    rc = rtl_stream_test_clear_output_fsk_reset(7U, &have_prev_after_clear, &reset_consumed, &have_prev_after_consume);
    failed |= expect_int_eq("clear output fsk reset helper rc", rc, 0);
    failed |= expect_int_eq("clear output leaves fsk modem for demod thread", have_prev_after_clear, 1);
    failed |= expect_int_eq("clear output fsk reset consumed", reset_consumed, 1);
    failed |= expect_int_eq("clear output fsk reset clears modem history", have_prev_after_consume, 0);

    double fsk_cfo_hz = 0.0;
    int fsk_cfo_after_generation_bump = -1;
    int fsk_cfo_after_reset = -1;
    const double fsk_dc_rad_per_sample = 0.0125;
    rc = rtl_stream_test_fsk_cfo_snapshot(fsk_dc_rad_per_sample, 48000, &fsk_cfo_hz, &fsk_cfo_after_generation_bump,
                                          &fsk_cfo_after_reset);
    failed |= expect_int_eq("fsk cfo snapshot helper rc", rc, 0);
    failed |= expect_double_near(
        "fsk cfo snapshot conversion", fsk_cfo_hz,
        -static_cast<double>(static_cast<float>(fsk_dc_rad_per_sample)) * 48000.0 / 6.28318530717958647692, 1e-6);
    failed |= expect_int_eq("fsk cfo snapshot generation bump invalidates estimate", fsk_cfo_after_generation_bump, 0);
    failed |= expect_int_eq("fsk cfo snapshot reset invalidates estimate", fsk_cfo_after_reset, 0);

    int request_rc = -1;
    int consumed = -1;
    cache_pending = -1;
    rc = rtl_stream_test_fsk_reacquire(RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR, 9U, 4, &used_after, &cache_pending,
                                       &generation_before, &generation_after, &request_rc, &consumed);
    failed |= expect_int_eq("fsk discriminator reacquire helper rc", rc, 0);
    failed |= expect_int_eq("fsk discriminator reacquire request queued", request_rc, 1);
    failed |= expect_int_eq("fsk discriminator reacquire consumed", consumed, 1);
    failed |=
        expect_generation_changed("fsk discriminator reacquire bumps generation", generation_before, generation_after);
    failed |= expect_size_eq("fsk discriminator reacquire clears queued ring", used_after, 0U);
    failed |= expect_int_eq("fsk discriminator reacquire resets cached symbols", cache_pending, 0);

    request_rc = -1;
    consumed = -1;
    cache_pending = -1;
    rc = rtl_stream_test_fsk_reacquire(RTL_STREAM_OUTPUT_AUDIO_MONITOR, 9U, 4, &used_after, &cache_pending,
                                       &generation_before, &generation_after, &request_rc, &consumed);
    failed |= expect_int_eq("inactive fsk reacquire helper rc", rc, 0);
    failed |= expect_int_eq("inactive fsk reacquire no-op", request_rc, 0);
    failed |= expect_int_eq("inactive fsk reacquire not consumed", consumed, 0);
    failed |= expect_generation_eq("inactive fsk reacquire keeps generation", generation_before, generation_after);
    failed |= expect_size_eq("inactive fsk reacquire leaves queued ring", used_after, 9U);
    failed |= expect_int_eq("inactive fsk reacquire leaves cached symbols", cache_pending, 4);

    rtl_stream_test_cqpsk_reacquire_result cqpsk_reacquire = {};
    rc = rtl_stream_test_cqpsk_reacquire(1, 4800, 10, 11U, 5, &cqpsk_reacquire);
    failed |= expect_int_eq("cqpsk reacquire helper rc", rc, 0);
    failed |= expect_int_eq("cqpsk reacquire request queued", cqpsk_reacquire.request_rc, 1);
    failed |= expect_int_eq("cqpsk reacquire duplicate coalesced", cqpsk_reacquire.second_request_rc, 1);
    failed |= expect_int_eq("cqpsk reacquire consumed", cqpsk_reacquire.consumed, 1);
    failed |= expect_int_eq("cqpsk reacquire consumed once", cqpsk_reacquire.second_consumed, 0);
    failed |= expect_generation_changed("cqpsk reacquire bumps generation", cqpsk_reacquire.generation_before,
                                        cqpsk_reacquire.generation_after);
    failed |= expect_size_eq("cqpsk reacquire clears queued ring", cqpsk_reacquire.used_after, 0U);
    failed |= expect_int_eq("cqpsk reacquire resets cached symbols", cqpsk_reacquire.cache_pending_after, 0);
    failed |= expect_double_near("cqpsk reacquire preserves coarse fll frequency", cqpsk_reacquire.fll_freq_after,
                                 cqpsk_reacquire.fll_freq_before, 1e-7);
    failed |= expect_double_near("cqpsk reacquire resets fll phase", cqpsk_reacquire.fll_phase_after, 0.0, 1e-7);
    failed |=
        expect_double_near("cqpsk reacquire resets costas frequency", cqpsk_reacquire.costas_freq_after, 0.0, 1e-7);
    failed |= expect_double_near("cqpsk reacquire resets costas phase", cqpsk_reacquire.costas_phase_after, 0.0, 1e-7);
    failed |= expect_double_near("cqpsk reacquire resets costas error", cqpsk_reacquire.costas_error_after, 0.0, 1e-7);
    failed |= expect_double_near("cqpsk reacquire resets ted delay", cqpsk_reacquire.ted_delay_after, 0.0, 1e-7);
    failed |= expect_double_near("cqpsk reacquire re-arms ted phase", cqpsk_reacquire.ted_mu_after, 21.0, 1e-7);
    failed |=
        expect_double_near("cqpsk reacquire resets differential real", cqpsk_reacquire.diff_prev_r_after, 1.0, 1e-7);
    failed |=
        expect_double_near("cqpsk reacquire resets differential imag", cqpsk_reacquire.diff_prev_j_after, 0.0, 1e-7);
    failed |= expect_double_near("cqpsk reacquire preserves agc", cqpsk_reacquire.cqpsk_agc_after,
                                 cqpsk_reacquire.cqpsk_agc_before, 1e-7);
    failed |= expect_int_eq("cqpsk reacquire resets filters", cqpsk_reacquire.histories_cleared, 1);
    failed |= expect_int_eq("cqpsk reacquire resets resampler phase", cqpsk_reacquire.resamp_phase_after, 0);
    failed |= expect_int_eq("cqpsk reacquire preserves output kind", cqpsk_reacquire.output_kind_after,
                            RTL_STREAM_OUTPUT_SYMBOL_CQPSK);
    failed |= expect_int_eq("cqpsk reacquire preserves symbol rate", cqpsk_reacquire.symbol_rate_after, 4800);
    failed |= expect_int_eq("cqpsk reacquire preserves channel profile", cqpsk_reacquire.channel_profile_after,
                            RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    failed |= expect_int_eq("cqpsk reacquire preserves ted sps", cqpsk_reacquire.ted_sps_after, 10);

    cqpsk_reacquire = {};
    rc = rtl_stream_test_cqpsk_reacquire(1, 6000, 8, 7U, 3, &cqpsk_reacquire);
    failed |= expect_int_eq("P25P2 cqpsk reacquire helper rc", rc, 0);
    failed |= expect_int_eq("P25P2 cqpsk reacquire request queued", cqpsk_reacquire.request_rc, 1);
    failed |= expect_int_eq("P25P2 cqpsk reacquire consumed", cqpsk_reacquire.consumed, 1);
    failed |= expect_generation_changed("P25P2 cqpsk reacquire bumps generation", cqpsk_reacquire.generation_before,
                                        cqpsk_reacquire.generation_after);
    failed |= expect_size_eq("P25P2 cqpsk reacquire clears queued ring", cqpsk_reacquire.used_after, 0U);
    failed |= expect_int_eq("P25P2 cqpsk reacquire resets cached symbols", cqpsk_reacquire.cache_pending_after, 0);
    failed |= expect_double_near("P25P2 cqpsk reacquire re-arms ted phase", cqpsk_reacquire.ted_mu_after, 17.0, 1e-7);
    failed |= expect_int_eq("P25P2 cqpsk reacquire preserves symbol rate", cqpsk_reacquire.symbol_rate_after, 6000);
    failed |= expect_int_eq("P25P2 cqpsk reacquire preserves channel profile", cqpsk_reacquire.channel_profile_after,
                            RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    failed |= expect_int_eq("P25P2 cqpsk reacquire preserves ted sps", cqpsk_reacquire.ted_sps_after, 8);

    rtl_stream_test_fll_retune_result fll_retune = {};
    rc = rtl_stream_test_fll_retune_policy(770418750U, 769668750U, 48000, 48000, &fll_retune);
    failed |= expect_int_eq("near RF retune FLL policy helper rc", rc, 0);
    failed |= expect_int_eq("near RF retune resets prior-channel FLL", fll_retune.reset_retained_fll, 1);
    failed |= expect_int_eq("near RF retune has no cold target seed", fll_retune.restored_cached_fll, 0);
    failed |= expect_int_eq("near RF retune keeps normal frequency reason", fll_retune.distant_frequency_reason, 0);
    failed |= expect_double_near("near RF retune clears prior-channel FLL", fll_retune.fll_freq_after, 0.0, 1e-7);

    fll_retune = {};
    rc = rtl_stream_test_fll_retune_policy(769668750U, 770418750U, 48000, 48000, &fll_retune);
    failed |= expect_int_eq("return RF retune FLL policy helper rc", rc, 0);
    failed |= expect_int_eq("return RF retune resets voice-channel FLL", fll_retune.reset_retained_fll, 1);
    failed |= expect_double_near("return RF retune clears voice-channel FLL", fll_retune.fll_freq_after, 0.0, 1e-7);

    fll_retune = {};
    rc = rtl_stream_test_fll_retune_policy(770418750U, 770418750U, 48000, 60000, &fll_retune);
    failed |= expect_int_eq("same RF rate-change FLL policy helper rc", rc, 0);
    failed |= expect_int_eq("same RF rate change retains FLL", fll_retune.reset_retained_fll, 0);
    failed |= expect_double_near("same RF rate change scales FLL", fll_retune.fll_freq_after,
                                 fll_retune.fll_freq_before * 0.8, 1e-7);

    rtl_stream_test_fll_retune_cache_result fll_cache = {};
    rc = rtl_stream_test_fll_retune_cache_round_trip(&fll_cache);
    failed |= expect_int_eq("frequency-specific FLL cache helper rc", rc, 0);
    failed |= expect_int_eq("cold VC hop resets FLL", fll_cache.first_hop_reset, 1);
    failed |= expect_double_near("cold VC hop starts from zero", fll_cache.first_hop_fll_after, 0.0, 1e-7);
    failed |= expect_int_eq("return to CC restores CC-specific FLL", fll_cache.cc_restore_used_cache, 1);
    failed |= expect_double_near("return to CC uses prior CC FLL", fll_cache.cc_restore_fll_after,
                                 fll_cache.expected_cc_fll, 1e-7);
    failed |= expect_int_eq("return to VC restores VC-specific FLL", fll_cache.vc_restore_used_cache, 1);
    failed |= expect_double_near("return to VC uses prior VC FLL", fll_cache.vc_restore_fll_after,
                                 fll_cache.expected_vc_fll, 1e-7);

    cqpsk_reacquire = {};
    rc = rtl_stream_test_cqpsk_reacquire(0, 4800, 10, 9U, 4, &cqpsk_reacquire);
    failed |= expect_int_eq("inactive cqpsk reacquire helper rc", rc, 0);
    failed |= expect_int_eq("inactive cqpsk reacquire no-op", cqpsk_reacquire.request_rc, 0);
    failed |= expect_int_eq("inactive cqpsk reacquire not consumed", cqpsk_reacquire.consumed, 0);
    failed |= expect_generation_eq("inactive cqpsk reacquire keeps generation", cqpsk_reacquire.generation_before,
                                   cqpsk_reacquire.generation_after);
    failed |= expect_size_eq("inactive cqpsk reacquire leaves queued ring", cqpsk_reacquire.used_after, 9U);
    failed |= expect_int_eq("inactive cqpsk reacquire leaves cached symbols", cqpsk_reacquire.cache_pending_after, 4);

    int first_profile = -1;
    int second_profile = -1;
    uint32_t first_freq_hz = 0U;
    uint32_t second_freq_hz = 0U;
    uint32_t first_request_id = 0U;
    uint32_t second_request_id = 0U;
    rc = rtl_stream_test_retune_profile_request_binding(&first_profile, &second_profile, &first_freq_hz,
                                                        &second_freq_hz, &first_request_id, &second_request_id);
    failed |= expect_int_eq("retune profile request binding helper rc", rc, 0);
    failed |= expect_int_eq("first retune keeps CQPSK profile", first_profile, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    failed |= expect_int_eq("second retune keeps C4FM profile", second_profile, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM);
    failed |= expect_int_eq("first retune profile keeps frequency", (int)first_freq_hz, 855000000);
    failed |= expect_int_eq("second retune profile keeps frequency", (int)second_freq_hz, 851000000);
    failed |= expect_int_eq("first retune profile keeps request id", (int)first_request_id, 1);
    failed |= expect_int_eq("second retune profile keeps request id", (int)second_request_id, 2);

    /*
     * Retune requests also preserve profile-specific gain, autogain, and Soapy
     * settings fields so a coalesced manual retune cannot silently discard user
     * capture preferences.
     */
    int coalesced_profile = -1;
    uint32_t coalesced_profile_freq_hz = 0U;
    uint32_t coalesced_manual_freq_hz = 0U;
    uint32_t coalesced_request_id = 0U;
    uint32_t coalesced_returned_request_id = 0U;
    rc = rtl_stream_test_retune_profile_coalesced_no_profile(&coalesced_profile, &coalesced_profile_freq_hz,
                                                             &coalesced_manual_freq_hz, &coalesced_request_id,
                                                             &coalesced_returned_request_id);
    failed |= expect_int_eq("coalesced retune profile helper rc", rc, 0);
    failed |= expect_int_eq("coalesced no-profile retune keeps profile", coalesced_profile,
                            RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    failed |=
        expect_int_eq("coalesced no-profile retune keeps profile frequency", (int)coalesced_profile_freq_hz, 855000000);
    failed |=
        expect_int_eq("coalesced no-profile retune keeps manual frequency", (int)coalesced_manual_freq_hz, 855000000);
    failed |= expect_int_eq("coalesced no-profile retune keeps request id", (int)coalesced_request_id, 1);
    failed |= expect_int_eq("coalesced no-profile retune returns coalesced request id",
                            (int)coalesced_returned_request_id, 1);

    int gain_is_set = -1;
    int gain_tenth_db = -1;
    int gain_is_auto = -1;
    int autogain_is_set = -1;
    int autogain_on = -1;
    rc = rtl_stream_test_retune_profile_gain_binding(&gain_is_set, &gain_tenth_db, &gain_is_auto, &autogain_is_set,
                                                     &autogain_on);
    failed |= expect_int_eq("retune gain profile helper rc", rc, 0);
    failed |= expect_int_eq("retune gain profile keeps gain-is-set", gain_is_set, 1);
    failed |= expect_int_eq("retune gain profile keeps manual gain", gain_tenth_db, 270);
    failed |= expect_int_eq("retune gain profile keeps manual mode", gain_is_auto, 0);
    failed |= expect_int_eq("retune gain profile keeps autogain-is-set", autogain_is_set, 1);
    failed |= expect_int_eq("retune gain profile keeps autogain off", autogain_on, 0);

    /*
     * USB apply/readback checks keep retry behavior explicit: apply failures are
     * retried, readback failures are retried only when verification is enabled,
     * and zero readback remains invalid.
     */
    int apply_calls = -1;
    int verify_calls = -1;
    int used_attempts = -1;
    rc = rtl_device_test_usb_apply_retry(1, 10, 1, 1, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry first-attempt rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry first-attempt apply calls", apply_calls, 1);
    failed |= expect_int_eq("rtl usb retry first-attempt verify calls", verify_calls, 1);
    failed |= expect_int_eq("rtl usb retry first-attempt used attempts", used_attempts, 1);

    rc = rtl_device_test_usb_apply_retry(1, 10, 3, 1, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry apply succeeds late rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry apply succeeds late apply calls", apply_calls, 3);
    failed |= expect_int_eq("rtl usb retry apply succeeds late verify calls", verify_calls, 1);
    failed |= expect_int_eq("rtl usb retry apply succeeds late used attempts", used_attempts, 3);

    rc = rtl_device_test_usb_apply_retry(1, 10, 1, 3, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry verify succeeds late rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry verify succeeds late apply calls", apply_calls, 3);
    failed |= expect_int_eq("rtl usb retry verify succeeds late verify calls", verify_calls, 3);
    failed |= expect_int_eq("rtl usb retry verify succeeds late used attempts", used_attempts, 3);

    rc = rtl_device_test_usb_apply_retry(1, 10, 11, 1, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry exhausted rc", rc, -1);
    failed |= expect_int_eq("rtl usb retry exhausted apply calls", apply_calls, 10);
    failed |= expect_int_eq("rtl usb retry exhausted verify calls", verify_calls, 0);
    failed |= expect_int_eq("rtl usb retry exhausted used attempts", used_attempts, 10);

    rc = rtl_device_test_usb_apply_retry(0, 10, 1, 10, &apply_calls, &verify_calls, &used_attempts);
    failed |= expect_int_eq("rtl usb retry disabled rc", rc, 0);
    failed |= expect_int_eq("rtl usb retry disabled apply calls", apply_calls, 1);
    failed |= expect_int_eq("rtl usb retry disabled verify calls", verify_calls, 0);
    failed |= expect_int_eq("rtl usb retry disabled used attempts", used_attempts, 1);

    uint32_t actual_rate = 0U;
    rc = rtl_device_test_usb_sample_rate_readback(960000U, 960000U, &actual_rate);
    failed |= expect_int_eq("rtl sample-rate readback exact rc", rc, 0);
    failed |= expect_int_eq("rtl sample-rate readback exact actual", (int)actual_rate, 960000);
    actual_rate = 99U;
    rc = rtl_device_test_usb_sample_rate_readback(1024000U, 960000U, &actual_rate);
    failed |= expect_int_eq("rtl sample-rate readback accepts quantized rc", rc, 0);
    failed |= expect_int_eq("rtl sample-rate readback returns quantized actual", (int)actual_rate, 960000);
    rc = rtl_device_test_usb_sample_rate_readback(1024000U, 0U, &actual_rate);
    failed |= expect_int_eq("rtl sample-rate readback rejects zero rc", rc, -1);

    /*
     * Gain setup intentionally records non-fatal AGC failures while still treating
     * gain-mode failures as fatal, matching the live-device setup sequence.
     */
    int agc_calls = -1;
    int gain_mode_calls = -1;
    int gain_calls = -1;
    int recorded_agc_rc = 0;
    rc =
        rtl_device_test_usb_manual_gain_controls(-5, 0, 0, &agc_calls, &gain_mode_calls, &gain_calls, &recorded_agc_rc);
    failed |= expect_int_eq("manual gain ignores AGC failure rc", rc, 0);
    failed |= expect_int_eq("manual gain AGC call count", agc_calls, 1);
    failed |= expect_int_eq("manual gain mode still applied", gain_mode_calls, 1);
    failed |= expect_int_eq("manual gain value still applied", gain_calls, 1);
    failed |= expect_int_eq("manual gain records AGC failure", recorded_agc_rc, -5);

    rc =
        rtl_device_test_usb_manual_gain_controls(0, -8, 0, &agc_calls, &gain_mode_calls, &gain_calls, &recorded_agc_rc);
    failed |= expect_int_eq("manual gain mode failure remains fatal", rc, -8);
    failed |= expect_int_eq("manual gain mode failure AGC call count", agc_calls, 1);
    failed |= expect_int_eq("manual gain mode failure call count", gain_mode_calls, 1);
    failed |= expect_int_eq("manual gain skips value after mode failure", gain_calls, 0);
    failed |= expect_int_eq("manual gain mode failure records no AGC failure", recorded_agc_rc, 0);

    rc =
        rtl_device_test_usb_manual_gain_controls(0, 0, -9, &agc_calls, &gain_mode_calls, &gain_calls, &recorded_agc_rc);
    failed |= expect_int_eq("manual gain value failure remains fatal", rc, -9);
    failed |= expect_int_eq("manual gain value failure AGC call count", agc_calls, 1);
    failed |= expect_int_eq("manual gain value failure mode call count", gain_mode_calls, 1);
    failed |= expect_int_eq("manual gain value failure call count", gain_calls, 1);
    failed |= expect_int_eq("manual gain value failure records no AGC failure", recorded_agc_rc, 0);

    rc = rtl_device_test_usb_auto_gain_controls(-6, 0, &agc_calls, &gain_mode_calls, &recorded_agc_rc);
    failed |= expect_int_eq("auto gain ignores AGC failure rc", rc, 0);
    failed |= expect_int_eq("auto gain AGC call count", agc_calls, 1);
    failed |= expect_int_eq("auto gain mode still applied", gain_mode_calls, 1);
    failed |= expect_int_eq("auto gain records AGC failure", recorded_agc_rc, -6);

    rc = rtl_device_test_usb_auto_gain_controls(0, -7, &agc_calls, &gain_mode_calls, &recorded_agc_rc);
    failed |= expect_int_eq("auto gain mode failure remains fatal", rc, -7);
    failed |= expect_int_eq("auto gain mode failure call count", gain_mode_calls, 1);
    failed |= expect_int_eq("auto gain skips AGC after mode failure", agc_calls, 0);
    failed |= expect_int_eq("auto gain mode failure records no AGC failure", recorded_agc_rc, 0);

    // Fragmented mute spans are coalesced so capture metadata stays IQ-pair aligned.
    uint64_t pending_mute = 0U;
    uint64_t emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 1U, 2U);
    failed |= expect_size_eq("odd mute fragment held", (size_t)emitted, 0U);
    failed |= expect_size_eq("odd mute pending byte", (size_t)pending_mute, 1U);
    emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 1U, 2U);
    failed |= expect_size_eq("second odd mute completes pair", (size_t)emitted, 2U);
    failed |= expect_size_eq("completed mute clears pending", (size_t)pending_mute, 0U);
    emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 3U, 2U);
    failed |= expect_size_eq("larger odd mute emits aligned part", (size_t)emitted, 2U);
    failed |= expect_size_eq("larger odd mute retains one byte", (size_t)pending_mute, 1U);
    emitted = rtl_device_test_coalesce_capture_mute_duration(&pending_mute, 5U, 2U);
    failed |= expect_size_eq("fragmented mute coalesces with prior pending", (size_t)emitted, 6U);
    failed |= expect_size_eq("fragmented mute pending clears", (size_t)pending_mute, 0U);

    failed |= expect_int_eq("odd reconfigure hold completes empty mute",
                            rtl_device_test_complete_fragmented_capture_discard(0, 1U), 1);
    failed |= expect_int_eq("odd reconfigure hold extends even mute",
                            rtl_device_test_complete_fragmented_capture_discard(4, 1U), 5);
    failed |= expect_int_eq("odd reconfigure hold keeps odd mute",
                            rtl_device_test_complete_fragmented_capture_discard(5, 1U), 5);
    failed |= expect_int_eq("aligned reconfigure hold leaves mute",
                            rtl_device_test_complete_fragmented_capture_discard(4, 0U), 4);

    int hold = -1;
    int mute = -1;
    int mute_byte_phase = -1;
    rtl_device_test_end_capture_reconfigure_with_odd_carry(&hold, &mute, &mute_byte_phase);
    failed |= expect_int_eq("end reconfigure clears hold after completion", hold, 0);
    failed |= expect_int_eq("end reconfigure schedules odd carry mute byte", mute, 1);
    failed |= expect_int_eq("end reconfigure preserves carry until scheduled mute drains", mute_byte_phase, 1);

    hold = -1;
    failed |= expect_int_eq("begin reconfigure without writer helper",
                            rtl_device_test_begin_capture_reconfigure_without_writer(&hold), 0);
    failed |= expect_int_eq("begin reconfigure without writer activates hold", hold, 1);

    int native_formats[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t capture_counts[5] = {99U, 99U, 99U, 99U, 99U};
    rc = rtl_device_test_public_capture_policy(native_formats, 8U, capture_counts, 5U);
    failed |= expect_int_eq("public capture policy helper rc", rc, 0);
    failed |= expect_int_eq("native format null", native_formats[0], DSD_IQ_FORMAT_UNKNOWN);
    failed |= expect_int_eq("native format usb", native_formats[1], DSD_IQ_FORMAT_CU8);
    failed |= expect_int_eq("native format tcp", native_formats[2], DSD_IQ_FORMAT_CU8);
    failed |= expect_int_eq("native format Soapy CF32", native_formats[3], DSD_IQ_FORMAT_CF32);
    failed |= expect_int_eq("native format Soapy CS16", native_formats[4], DSD_IQ_FORMAT_CS16);
    failed |= expect_int_eq("native format Soapy none", native_formats[5], DSD_IQ_FORMAT_UNKNOWN);
    failed |= expect_int_eq("native format replay", native_formats[6], DSD_IQ_FORMAT_CS16);
    failed |= expect_int_eq("native format unknown backend", native_formats[7], DSD_IQ_FORMAT_UNKNOWN);
    failed |= expect_int_eq("capture count null", (int)capture_counts[0], 0);
    failed |= expect_int_eq("capture count starts zero", (int)capture_counts[1], 0);
    failed |= expect_int_eq("capture count ignores missing writer", (int)capture_counts[2], 0);
    failed |= expect_int_eq("capture count increments with writer", (int)capture_counts[3], 2);
    failed |= expect_int_eq("capture count detach resets", (int)capture_counts[4], 0);

    size_t held_ring_used = 99U;
    failed |= expect_int_eq("usb reconfigure discard helper",
                            rtl_device_test_usb_reconfigure_discards_samples(16U, &held_ring_used), 0);
    failed |= expect_size_eq("usb reconfigure discards samples", held_ring_used, 0U);

    // Replay RESET events clear phase/carry state only after pending output drains.
    int replay_phase = 3;
    int replay_have_carry = 1;
    uint8_t replay_carry_byte = 42U;
    rtl_device_test_replay_dispatch_reset_event_state(&replay_phase, &replay_have_carry, &replay_carry_byte);
    failed |= expect_int_eq("reset event clears replay fs4 phase", replay_phase, 0);
    failed |= expect_int_eq("reset event clears replay cu8 carry", replay_have_carry, 0);
    failed |= expect_int_eq("reset event clears replay carry byte", (int)replay_carry_byte, 0);
    failed |= expect_int_eq("reset event waits for queued ring samples",
                            rtl_device_test_replay_event_boundary_drained(2U, 3U, 3U), 0);
    failed |= expect_int_eq("reset event waits for reserved demod generation",
                            rtl_device_test_replay_event_boundary_drained(0U, 3U, 2U), 0);
    uint64_t discarded_span_consumed = rtl_stream_test_replay_acknowledge_discarded_span(3U, 2U);
    failed |= expect_u64_eq("discarded replay span acknowledges submit generation", discarded_span_consumed, 3U);
    failed |= expect_int_eq("rewind boundary accepts acknowledged discarded span",
                            rtl_device_test_replay_event_boundary_drained(0U, 3U, discarded_span_consumed), 1);
    failed |= expect_u64_eq("discarded replay acknowledgment remains monotonic",
                            rtl_stream_test_replay_acknowledge_discarded_span(3U, 4U), 4U);
    failed |=
        expect_int_eq("reset event boundary drained", rtl_device_test_replay_event_boundary_drained(0U, 3U, 3U), 1);

    /*
     * Replay input-level snapshots preserve source identity for USB/TCP CU8 and
     * accept CF32 only from the post-driver capture stage with aligned input and
     * sufficient scratch space.
     */
    const int rtl_test_backend_usb = 0;
    const int rtl_test_backend_tcp = 1;
    int input_level_rc = -99;
    int input_level_source = -99;
    uint64_t input_level_count = 0U;
    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CU8, rtl_test_backend_usb, "", 8U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CU8 USB snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CU8 USB snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CU8 USB source", input_level_source, DSD_INPUT_LEVEL_SOURCE_RTL_CU8);
    failed |= expect_size_eq("replay CU8 USB sample count", (size_t)input_level_count, 8U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CU8, rtl_test_backend_tcp, "", 8U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CU8 TCP snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CU8 TCP snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CU8 TCP source", input_level_source, DSD_INPUT_LEVEL_SOURCE_RTL_TCP_CU8);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb,
                                                     "post_driver_cf32_pre_ring", 16U, 4U, &input_level_rc,
                                                     &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CF32 source", input_level_source, DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32);
    failed |= expect_size_eq("replay CF32 sample count", (size_t)input_level_count, 4U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CS16, rtl_test_backend_usb, "", 8U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CS16 snapshot helper rc", rc, 0);
    failed |= expect_int_eq("replay CS16 snapshot rc", input_level_rc, 0);
    failed |= expect_int_eq("replay CS16 source", input_level_source, DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16);
    failed |= expect_size_eq("replay CS16 sample count", (size_t)input_level_count, 4U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CS16, rtl_test_backend_usb, "", 6U, 0U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CS16 rejects misaligned helper rc", rc, 0);
    failed |= expect_int_eq("replay CS16 rejects misaligned bytes", input_level_rc, -1);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb, "pre_ring", 16U, 4U,
                                                     &input_level_rc, &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 rejects wrong stage helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 rejects wrong stage", input_level_rc, -1);
    failed |= expect_int_eq("replay CF32 wrong stage clears source", input_level_source, -1);
    failed |= expect_size_eq("replay CF32 wrong stage clears count", (size_t)input_level_count, 0U);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb,
                                                     "post_driver_cf32_pre_ring", 10U, 4U, &input_level_rc,
                                                     &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 rejects misaligned helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 rejects misaligned bytes", input_level_rc, -1);

    rc = rtl_device_test_replay_input_level_snapshot(DSD_IQ_FORMAT_CF32, rtl_test_backend_usb,
                                                     "post_driver_cf32_pre_ring", 16U, 3U, &input_level_rc,
                                                     &input_level_source, &input_level_count);
    failed |= expect_int_eq("replay CF32 rejects small scratch helper rc", rc, 0);
    failed |= expect_int_eq("replay CF32 rejects small scratch", input_level_rc, -1);

    /*
     * Replay sample conversion is stateful across chunks: CU8 preserves odd
     * carry bytes, while CF32 post-driver blocks optionally apply J/4 rotation.
     */
    float converted[8] = {};
    int convert_phase = -1;
    int convert_have_carry = -1;
    uint8_t convert_carry_byte = 0U;
    const uint8_t cu8_with_carry[] = {128U, 130U, 132U};
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CU8, "", 0, 0, cu8_with_carry, sizeof(cu8_with_carry), 8U, 2, 1, 126U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CU8 carry conversion count", rc, 4);
    failed |= expect_double_near("replay CU8 carried I sample", converted[0], (126.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_double_near("replay CU8 carried Q sample", converted[1], (128.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_double_near("replay CU8 aligned I sample", converted[2], (130.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_double_near("replay CU8 aligned Q sample", converted[3], (132.0 - 127.5) / 127.5, 1e-6);
    failed |= expect_int_eq("replay CU8 no-rotate preserves phase", convert_phase, 2);
    failed |= expect_int_eq("replay CU8 carry consumed", convert_have_carry, 0);
    failed |= expect_int_eq("replay CU8 consumed carry byte unchanged", (int)convert_carry_byte, 126);

    const uint8_t cu8_odd_tail[] = {127U, 128U, 129U};
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CU8, "", 0, 0, cu8_odd_tail, sizeof(cu8_odd_tail), 8U, 0, 0, 0U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CU8 odd tail conversion count", rc, 2);
    failed |= expect_int_eq("replay CU8 odd tail retained", convert_have_carry, 1);
    failed |= expect_int_eq("replay CU8 odd tail byte", (int)convert_carry_byte, 129);

    const uint8_t historical_cu8[] = {10U, 11U, 20U, 21U};
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CU8, "", 1, 1, historical_cu8, sizeof(historical_cu8), 8U, 0, 0, 0U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("historical CU8 conversion count", rc, 4);
    failed |= expect_double_near("historical CU8 phase0 I", converted[0], (10.0 - 128.0) / 127.5, 1e-6);
    failed |= expect_double_near("historical CU8 phase0 Q", converted[1], (11.0 - 128.0) / 127.5, 1e-6);
    failed |= expect_double_near("historical CU8 phase1 I", converted[2], (234.0 - 128.0) / 127.5, 1e-6);
    failed |= expect_double_near("historical CU8 phase1 Q", converted[3], (20.0 - 128.0) / 127.5, 1e-6);
    failed |= expect_int_eq("historical CU8 phase advance", convert_phase, 2);

    float cf32_samples[] = {1.0f, 2.0f, 3.0f, 4.0f};
    uint8_t cf32_raw[sizeof(cf32_samples)] = {};
    DSD_MEMCPY(cf32_raw, cf32_samples, sizeof(cf32_samples));
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CF32, "post_driver_cf32_pre_ring", 1, 0, cf32_raw, sizeof(cf32_raw),
                                   8U, 1, 0, 0U, converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CF32 rotated conversion count", rc, 4);
    failed |= expect_double_near("replay CF32 phase1 rotated I", converted[0], -2.0, 1e-6);
    failed |= expect_double_near("replay CF32 phase1 rotated Q", converted[1], 1.0, 1e-6);
    failed |= expect_double_near("replay CF32 phase2 rotated I", converted[2], -3.0, 1e-6);
    failed |= expect_double_near("replay CF32 phase2 rotated Q", converted[3], -4.0, 1e-6);
    failed |= expect_int_eq("replay CF32 rotation advances phase", convert_phase, 3);
    failed |= expect_int_eq("replay CF32 leaves carry clear", convert_have_carry, 0);

    rc = call_replay_convert_block(DSD_IQ_FORMAT_CF32, "pre_ring", 1, 0, cf32_raw, sizeof(cf32_raw), 8U, 1, 0, 0U,
                                   converted, sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CF32 rejects wrong stage conversion", rc, -1);

    const int16_t cs16_samples[] = {8192, -16384, 32767, -32768};
    uint8_t cs16_raw[sizeof(cs16_samples)] = {};
    DSD_MEMCPY(cs16_raw, cs16_samples, sizeof(cs16_samples));
    rc = call_replay_convert_block(DSD_IQ_FORMAT_CS16, "post_mute_pre_widen", 0, 0, cs16_raw,
                                   sizeof(cs16_raw), 8U, 0, 0, 0U, converted,
                                   sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CS16 conversion count", rc, 4);
    failed |= expect_double_near("replay CS16 I0", converted[0], 0.25, 1e-6);
    failed |= expect_double_near("replay CS16 Q0", converted[1], -0.5, 1e-6);
    failed |= expect_double_near("replay CS16 I1", converted[2], 32767.0 / 32768.0, 1e-6);
    failed |= expect_double_near("replay CS16 Q1", converted[3], -1.0, 1e-6);
    failed |= expect_int_eq("replay CS16 preserves phase", convert_phase, 0);

    rc = call_replay_convert_block(DSD_IQ_FORMAT_CS16, "post_mute_pre_widen", 0, 0, cs16_raw,
                                   sizeof(cs16_raw) - 1U, 8U, 0, 0, 0U, converted,
                                   sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CS16 rejects partial complex sample", rc, -1);

    rc = call_replay_convert_block(DSD_IQ_FORMAT_CS16, "post_driver_cf32_pre_ring", 0, 0, cs16_raw,
                                   sizeof(cs16_raw), 8U, 0, 0, 0U, converted,
                                   sizeof(converted) / sizeof(converted[0]), &convert_phase,
                                   &convert_have_carry, &convert_carry_byte);
    failed |= expect_int_eq("replay CS16 rejects wrong capture stage", rc, -1);

    /*
     * CU8 callback chunking must preserve odd trailing bytes between callbacks,
     * and full-ring drops must keep both drop accounting and FS/4 phase aligned.
     */
    size_t u8_used = 0U;
    int u8_phase = -1;
    int u8_carry_valid = -1;
    uint8_t u8_carry_byte = 0U;
    int first_status = -1;
    int second_status = -1;
    rc = rtl_device_test_u8_odd_carry_bridge(&u8_used, &u8_phase, &u8_carry_valid, &u8_carry_byte, &first_status,
                                             &second_status);
    failed |= expect_int_eq("u8 odd-carry helper rc", rc, 0);
    failed |= expect_int_eq("u8 odd-carry first chunk accepted", first_status, 0);
    failed |= expect_int_eq("u8 odd-carry second chunk accepted", second_status, 0);
    failed |= expect_size_eq("u8 odd-carry bridges one IQ pair", u8_used, 2U);
    failed |= expect_int_eq("u8 odd-carry advances one FS/4 pair", u8_phase, 1);
    failed |= expect_int_eq("u8 odd-carry retains final byte", u8_carry_valid, 1);
    failed |= expect_int_eq("u8 odd-carry byte preserved", (int)u8_carry_byte, 126);

    uint64_t u8_drops = 0U;
    uint64_t full_events = 0U;
    int full_status = -1;
    rc = rtl_device_test_u8_full_ring_drop(&u8_used, &u8_drops, &full_events, &u8_phase, &full_status);
    failed |= expect_int_eq("u8 full-ring helper rc", rc, 0);
    failed |= expect_int_eq("u8 full-ring reports exhaustion", full_status, 1);
    failed |= expect_size_eq("u8 full-ring keeps one pair", u8_used, 2U);
    failed |= expect_size_eq("u8 full-ring drops aligned remainder", (size_t)u8_drops, 4U);
    failed |= expect_size_eq("u8 full-ring counts reserve exhaustion", (size_t)full_events, 1U);
    failed |= expect_int_eq("u8 full-ring advances phase over committed and dropped pairs", u8_phase, 3);

    int stale_dev_carry_valid = -1;
    int stale_local_carry_valid = -1;
    rc = rtl_device_test_u8_generation_stale_drop(&u8_drops, &u8_phase, &stale_dev_carry_valid,
                                                  &stale_local_carry_valid, &full_status);
    failed |= expect_int_eq("u8 stale-generation helper rc", rc, 0);
    failed |= expect_int_eq("u8 stale-generation reports stale", full_status, 1);
    failed |= expect_size_eq("u8 stale-generation drops produced and aligned remainder", (size_t)u8_drops, 6U);
    failed |= expect_int_eq("u8 stale-generation advances phase over aligned remainder", u8_phase, 1);
    failed |= expect_int_eq("u8 stale-generation clears device carry", stale_dev_carry_valid, 0);
    failed |= expect_int_eq("u8 stale-generation clears local carry", stale_local_carry_valid, 0);

    dsd_input_level_cu8_moments path_moments[9] = {};
    rc = rtl_device_test_u8_moment_accounting(path_moments, sizeof(path_moments) / sizeof(path_moments[0]));
    failed |= expect_int_eq("u8 moment accounting helper rc", rc, 0);
    const uint8_t common_raw[] = {0U, 1U, 2U, 128U, 254U, 255U};
    const uint8_t odd_first_raw[] = {3U};
    const uint8_t odd_second_raw[] = {4U, 5U};
    const uint8_t tcp_raw[] = {0U, 255U, 1U, 2U, 3U, 4U, 254U, 253U, 128U};
    const uint8_t replay_raw[] = {0U, 1U, 254U, 255U};
    if (rc == 0) {
        failed |= expect_cu8_moments("u8 contiguous ring moments", &path_moments[0], common_raw, sizeof(common_raw));
        failed |= expect_cu8_moments("u8 wrapped ring moments", &path_moments[1], common_raw, sizeof(common_raw));
        failed |=
            expect_cu8_moments("u8 odd first-block moments", &path_moments[2], odd_first_raw, sizeof(odd_first_raw));
        failed |= expect_cu8_moments("u8 odd second-block excludes old carry", &path_moments[3], odd_second_raw,
                                     sizeof(odd_second_raw));
        failed |=
            expect_cu8_moments("u8 full-ring dropped tail moments", &path_moments[4], common_raw, sizeof(common_raw));
        failed |= expect_cu8_moments("u8 stale-generation skipped tail moments", &path_moments[5], common_raw,
                                     sizeof(common_raw));
        failed |=
            expect_cu8_moments("rtl-tcp pending/direct/remainder moments", &path_moments[6], tcp_raw, sizeof(tcp_raw));
        failed |= expect_cu8_moments("modern replay excludes old carry and includes tail", &path_moments[7], replay_raw,
                                     sizeof(replay_raw));
        failed |= expect_cu8_moments("legacy replay uses raw pre-rotation bytes", &path_moments[8], replay_raw,
                                     sizeof(replay_raw));
    }

    /*
     * Miscellaneous RTL helper contracts are pure formatting/alignment rules:
     * tuner-type labels stay stable, capture event reasons are bounded, Soapy
     * setting text is trimmed, and rtl_tcp buffers are page-aligned upward.
     */
    char tuner_labels[96] = {};
    char reason_small[8] = {};
    char reason_null[4] = {'x', 'x', 'x', '\0'};
    char trimmed[32] = {};
    size_t rounded_pages[4] = {};
    rc = rtl_device_test_misc_string_helpers(tuner_labels, sizeof(tuner_labels), reason_small, sizeof(reason_small),
                                             reason_null, sizeof(reason_null), trimmed, sizeof(trimmed), rounded_pages,
                                             sizeof(rounded_pages) / sizeof(rounded_pages[0]));
    failed |= expect_int_eq("rtl misc string helper rc", rc, 0);
    if (std::strcmp(tuner_labels, "E4000|FC0012|FC0013|FC2580|R820T|R828D|unknown") != 0) {
        DSD_FPRINTF(stderr, "FAIL: tuner labels got='%s'\n", tuner_labels);
        failed = 1;
    }
    if (std::strcmp(reason_small, "capture") != 0) {
        DSD_FPRINTF(stderr, "FAIL: truncated reason got='%s'\n", reason_small);
        failed = 1;
    }
    if (std::strcmp(reason_null, "") != 0) {
        DSD_FPRINTF(stderr, "FAIL: null reason got='%s'\n", reason_null);
        failed = 1;
    }
    if (std::strcmp(trimmed, "gain = 12.5") != 0) {
        DSD_FPRINTF(stderr, "FAIL: trimmed setting got='%s'\n", trimmed);
        failed = 1;
    }
    failed |= expect_size_eq("round zero page", rounded_pages[0], 0U);
    failed |= expect_size_eq("round one page", rounded_pages[1], 4096U);
    failed |= expect_size_eq("round exact page", rounded_pages[2], 4096U);
    failed |= expect_size_eq("round next page", rounded_pages[3], 8192U);
    failed |= expect_int_eq("rtl misc helper rejects short arrays",
                            rtl_device_test_misc_string_helpers(tuner_labels, sizeof(tuner_labels), reason_small,
                                                                sizeof(reason_small), reason_null, sizeof(reason_null),
                                                                trimmed, sizeof(trimmed), rounded_pages, 3U),
                            -1);

    size_t tcp_bufsz[8] = {};
    int tcp_waitall[4] = {};
    uint64_t tcp_deltas[2] = {};
    int agc_want = -1;
    rc = rtl_device_test_tcp_policy_helpers(tcp_bufsz, sizeof(tcp_bufsz) / sizeof(tcp_bufsz[0]), tcp_waitall,
                                            sizeof(tcp_waitall) / sizeof(tcp_waitall[0]), tcp_deltas,
                                            sizeof(tcp_deltas) / sizeof(tcp_deltas[0]), &agc_want);
    failed |= expect_int_eq("rtl tcp policy helper rc", rc, 0);
    failed |= expect_size_eq("rtl tcp null default buffer", tcp_bufsz[0], 65536U);
    failed |= expect_size_eq("rtl tcp backend default buffer", tcp_bufsz[1], 16384U);
    failed |= expect_size_eq("rtl usb zero-rate default buffer", tcp_bufsz[2], 65536U);
    failed |= expect_size_eq("rtl low-rate minimum buffer", tcp_bufsz[3], 16384U);
    failed |= expect_size_eq("rtl rate-scaled buffer", tcp_bufsz[4], 38400U);
    failed |= expect_size_eq("rtl high-rate capped buffer", tcp_bufsz[5], 262144U);
    failed |= expect_size_eq("rtl config buffer override", tcp_bufsz[6], 12345U);
    failed |= expect_size_eq("rtl invalid config buffer falls back", tcp_bufsz[7], 262144U);
    failed |= expect_int_eq("rtl null waitall default", tcp_waitall[0], 1);
    failed |= expect_int_eq("rtl tcp waitall default", tcp_waitall[1], 0);
    failed |= expect_int_eq("rtl waitall explicit enable", tcp_waitall[2], 1);
    failed |= expect_int_eq("rtl waitall explicit disable", tcp_waitall[3], 0);
    failed |= expect_size_eq("rtl counter delta monotonic", (size_t)tcp_deltas[0], 60U);
    failed |= expect_size_eq("rtl counter delta clamps wrap/reorder", (size_t)tcp_deltas[1], 0U);
    failed |= expect_int_eq("rtl default agc config", agc_want, 1);

    failed |= expect_int_eq("rtl tcp policy helper rejects short buffers",
                            rtl_device_test_tcp_policy_helpers(tcp_bufsz, 7U, tcp_waitall,
                                                               sizeof(tcp_waitall) / sizeof(tcp_waitall[0]), tcp_deltas,
                                                               sizeof(tcp_deltas) / sizeof(tcp_deltas[0]), &agc_want),
                            -1);

    failed |= test_audio_monitor_retune_reset();
    failed |= test_audio_monitor_retune_resolves_channel();
    failed |= test_audio_monitor_retune_profile_decides();
    failed |= test_audio_monitor_rate_not_restored_stops();
    failed |= test_audio_monitor_refused_restore_failure_stops();
    failed |= test_audio_monitor_refused_retune_completes_failed();
    failed |= test_retune_profile_carries_analog_fields();
    failed |= test_retune_profile_width_checked_at_landing_rate();
    failed |= test_retune_profiles_land_each_rows_family_and_width();
    failed |= test_retune_family_superseded_by_a_later_live_request();
    failed |= test_retune_family_retires_older_queued_requests();
    failed |= test_retune_family_superseded_while_it_lands();
    failed |= test_live_family_request_count();

    return failed ? 1 : 0;
}
