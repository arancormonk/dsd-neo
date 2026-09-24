// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A live receive-family switch must leave the demodulator exactly where a fresh
 * stream open of the new mode would: digital -> analog -> digital ends on the
 * fresh digital configuration, the analog leg matches a fresh -fA open, the
 * output ring is cleared and the stream generation bumps at each switch, and a
 * request made while the stream runs waits for the demod thread to consume it.
 * The session being left has its carrier and timing loops pulled well away from
 * their start values, its monitor audio (de-emphasis, DC, audio LPF, squelch
 * envelope) and channel, half-band and resampler delay lines filled with stale
 * values, and its squelch dwell one block short of a multi-frequency hop first,
 * so each switch has to reset them the way an open does. The fresh
 * baselines run the same demodulator configuration functions as
 * dsd_rtl_stream_open() (see family_test_seed_open()), including at a demod rate
 * the device forces, where the digital resampler follows the symbol profile.
 *
 * Leaving analog lands on the symbol profile queued after the family request,
 * even when the demod thread reaches a block boundary between the two requests,
 * and even when an older CQPSK toggle was still queued before the family request.
 *
 * A width-only change on a running analog stream stays inside the family: it
 * drops the channel plan and the channel/half-band histories and nothing else.
 *
 * While a stream runs, a width its published demod rate (or a replay's post-demod
 * decimation) cannot realize is refused before it is queued, as a live request
 * and as a retune profile, and the refusal is logged with the validator's text.
 *
 * A session that started with -fA and then switches to a digital mode lands on
 * a fresh open of that mode too. Throughout, the stream keeps the options
 * snapshot it opened with, as a real session's orchestrator copy does: the
 * family requests are all it learns of a mode change, so after a switch its own
 * record of the family, not that snapshot, decides where a symbol profile without
 * CQPSK lands (a CQPSK profile and back returns to the FSK discriminator). Every
 * switch also returns the I/Q DC and balance estimates and a replay's post-demod
 * decimator to their start.
 *
 * A typed digital scan row on an analog session (a -fA open, or a DMR open
 * switched to analog) keeps the monitor output with the row's channel profile and
 * the analog family flag still set; republishing the row's symbol profile changes
 * nothing, and the row's leave restores the analog monitor. A digital family
 * request still leaves the analog family from there, and from a CQPSK toggle
 * under -fA: a -fA session that had either applied before a digital mode is
 * picked lands on the same fresh open of that mode.
 *
 * A live request accepted at the published rate is held again to the rate a
 * retune landed the stream on before the demod thread consumed it.
 */

#include <cstdio>
#include <cstring>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

static char g_last_error[512];
static int g_error_count;

static void
capture_error_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_ERROR && text) {
        DSD_SNPRINTF(g_last_error, sizeof g_last_error, "%s", text);
        g_error_count++;
    }
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_fields_equal(const char* label, const rtl_stream_test_demod_fields& got,
                    const rtl_stream_test_demod_fields& want) {
    int rc = 0;
    char name[160];
#define FIELD(f)                                                                                                       \
    do {                                                                                                               \
        DSD_SNPRINTF(name, sizeof name, "%s: " #f, label);                                                             \
        rc |= expect_int(name, got.f, want.f);                                                                         \
    } while (0)
    FIELD(output_kind);
    FIELD(cqpsk_enable);
    FIELD(demod_is_fm);
    FIELD(demod_is_qpsk);
    FIELD(deemph);
    FIELD(deemph_a_q15);
    FIELD(audio_lpf_enable);
    FIELD(channel_lpf_enable);
    FIELD(channel_lpf_profile);
    FIELD(channel_lpf_width_hz);
    FIELD(analog_family);
    FIELD(analog_demod);
    FIELD(symbol_rate_hz);
    FIELD(symbol_levels);
    FIELD(ted_enabled);
    FIELD(ted_sps);
    FIELD(resamp_enabled);
    FIELD(resamp_l);
    FIELD(resamp_m);
    FIELD(output_rate);
    FIELD(fsk_sample_rate_hz);
    FIELD(fsk_symbol_rate_hz);
    FIELD(fsk_levels);
    FIELD(fsk_channel_profile);
    FIELD(costas_freq_urad);
    FIELD(costas_phase_urad);
    FIELD(fll_freq_urad);
    FIELD(fll_phase_urad);
    FIELD(ted_awaiting_init);
    FIELD(deemph_avg_u);
    FIELD(dc_avg_u);
    FIELD(audio_lpf_state_u);
    FIELD(squelch_env_u);
    FIELD(squelch_gate_open);
    FIELD(squelch_hits);
    FIELD(channel_hist_clear);
    FIELD(hb_hist_clear);
    FIELD(resamp_hist_clear);
    FIELD(iq_correction_clear);
    FIELD(post_decim_clear);
#undef FIELD
    return rc;
}

/* The analog leg: the symbol clock and FSK modem are unused on the monitor path, so
 * only the fields that shape monitor audio are held to the fresh -fA open. */
static int
expect_analog_fields_equal(const char* label, const rtl_stream_test_demod_fields& got,
                           const rtl_stream_test_demod_fields& want) {
    int rc = 0;
    char name[160];
#define FIELD(f)                                                                                                       \
    do {                                                                                                               \
        DSD_SNPRINTF(name, sizeof name, "%s: " #f, label);                                                             \
        rc |= expect_int(name, got.f, want.f);                                                                         \
    } while (0)
    FIELD(output_kind);
    FIELD(cqpsk_enable);
    FIELD(demod_is_fm);
    FIELD(demod_is_qpsk);
    FIELD(deemph);
    FIELD(deemph_a_q15);
    FIELD(audio_lpf_enable);
    FIELD(channel_lpf_enable);
    FIELD(channel_lpf_profile);
    FIELD(channel_lpf_width_hz);
    FIELD(analog_family);
    FIELD(analog_demod);
    FIELD(ted_enabled);
    FIELD(resamp_enabled);
    FIELD(resamp_l);
    FIELD(resamp_m);
    FIELD(output_rate);
    FIELD(costas_freq_urad);
    FIELD(costas_phase_urad);
    FIELD(fll_freq_urad);
    FIELD(fll_phase_urad);
    FIELD(ted_awaiting_init);
    FIELD(deemph_avg_u);
    FIELD(dc_avg_u);
    FIELD(audio_lpf_state_u);
    FIELD(squelch_env_u);
    FIELD(squelch_gate_open);
    FIELD(squelch_hits);
    FIELD(channel_hist_clear);
    FIELD(hb_hist_clear);
    FIELD(resamp_hist_clear);
    FIELD(iq_correction_clear);
    FIELD(post_decim_clear);
#undef FIELD
    return rc;
}

namespace {

struct family_case {
    const char* name;
    void (*configure)(dsd_opts*);
    rtl_stream_test_digital_request request;
};

} // namespace

static void
p25_c4fm(dsd_opts* o) {
    o->frame_p25p1 = 1;
    o->mod_c4fm = 1;
}

static void
p25_cqpsk(dsd_opts* o) {
    o->frame_p25p1 = 1;
    o->mod_qpsk = 1;
}

static void
dmr(dsd_opts* o) {
    o->frame_dmr = 1;
    o->mod_c4fm = 1;
}

static void
nxdn48(dsd_opts* o) {
    o->frame_nxdn48 = 1;
    o->mod_c4fm = 1;
}

static void
dpmr(dsd_opts* o) {
    o->frame_dpmr = 1;
    o->mod_c4fm = 1;
}

static int
run_case(const family_case& c, int rate_hz, int forced_rate_out_hz, int nfm_width_hz) {
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;
    analog.analog_nfm_bandwidth_hz = nfm_width_hz;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    int rc = expect_int(
        c.name, rtl_stream_test_analog_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &c.request, &r),
        0);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    char label[128];

    DSD_SNPRINTF(label, sizeof label, "%s@%d -> analog", c.name, demod_rate_hz);
    rc |= expect_int(label, r.analog_request_rc, 0);
    rc |= expect_int("request waits for the demod thread", r.analog_deferred_until_consume, 1);
    rc |= expect_analog_fields_equal(label, r.switched_analog, r.fresh_analog);
    rc |= expect_int("analog switch bumps the generation", r.generation_after_analog != r.generation_before, 1);
    rc |= expect_int("analog switch clears the ring", (int)r.used_after_analog, 0);
    rc |= expect_int("seeded ring was not empty", r.used_before > 0U, 1);
    rc |= expect_int("analog profile published", r.published_analog_rc, 1);
    rc |= expect_int("published kind", r.published_kind, DSD_ANALOG_DEMOD_FM);

    DSD_SNPRINTF(label, sizeof label, "%s@%d -> digital", c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("digital switch bumps the generation", r.generation_after_digital != r.generation_after_analog, 1);
    rc |= expect_int("digital switch clears the ring", (int)r.used_after_digital, 0);
    rc |= expect_int("analog profile withdrawn", r.published_after_digital_rc, 0);
    if (c.request.boundary_between_requests) {
        rc |= expect_int("digital family waits for its symbol profile", r.digital_held_until_profile, 1);
    }
    /* The decoder sets symbol timing before a deferred switch lands, from this prediction. */
    rc |=
        expect_int("predicted analog output rate", (int)r.predicted_analog_output_rate, r.switched_analog.output_rate);
    rc |= expect_int("predicted digital output rate", (int)r.predicted_digital_output_rate,
                     r.switched_digital.output_rate);
    rc |= expect_int("CQPSK and back returns to the FSK discriminator", r.output_kind_after_cqpsk_round_trip,
                     RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    return rc;
}

/* A session started with -fA, where the operator then picks a digital mode: the digital state it lands on must equal
 * a fresh open of that mode, with nothing the -fA open chose carried over. */
static int
run_analog_start_case(const family_case& c, int rate_hz, int forced_rate_out_hz) {
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "-fA start, %s@%d run", c.name, demod_rate_hz);
    int rc = expect_int(
        label,
        rtl_stream_test_analog_start_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &c.request, &r), 0);
    rc |= expect_int("-fA start runs the monitor", r.fresh_analog.output_kind, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    rc |= expect_int("-fA start publishes the analog profile", r.published_analog_rc, 1);

    DSD_SNPRINTF(label, sizeof label, "-fA start, %s@%d -> digital", c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("-fA start: digital switch bumps the generation",
                     r.generation_after_digital != r.generation_after_analog, 1);
    rc |= expect_int("-fA start: digital switch clears the ring", (int)r.used_after_digital, 0);
    rc |= expect_int("-fA start: seeded ring was not empty", r.used_before > 0U, 1);
    rc |= expect_int("-fA start: analog profile withdrawn", r.published_after_digital_rc, 0);
    if (c.request.boundary_between_requests) {
        rc |= expect_int("-fA start: digital family waits for its symbol profile", r.digital_held_until_profile, 1);
    }
    rc |= expect_int("-fA start: predicted digital output rate", (int)r.predicted_digital_output_rate,
                     r.switched_digital.output_rate);
    /* The -fA session's options snapshot names no digital mode: after the switch, the stream's own record of it has
     * to keep a symbol profile without CQPSK on the FSK discriminator. */
    rc |= expect_int("-fA start: CQPSK and back returns to the FSK discriminator", r.output_kind_after_cqpsk_round_trip,
                     RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    rc |= expect_int("-fA start: the digital family is not the analog family", r.family_active_after_digital, 0);
    return rc;
}

/* A -fA session that had moved its front end off the monitor output with a symbol profile applied on its own, before
 * the operator picks a digital mode: the analog family flag is still set, and the analog profile is no longer
 * published. The digital family request must leave the analog family from there all the same, onto a fresh open of
 * the mode picked, instead of letting that mode's symbol profile land on the -fA session (where a profile without
 * CQPSK puts the front end back on monitor audio). */
static int
run_under_analog_case(const family_case& c, int profile_under_analog, int rate_hz, int forced_rate_out_hz) {
    family_case under = c;
    under.request.profile_under_analog = profile_under_analog;
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    const char* under_name =
        profile_under_analog == RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE ? "CQPSK toggle" : "typed DMR row";
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "-fA %s, %s@%d run", under_name, c.name, demod_rate_hz);
    int rc = expect_int(
        label,
        rtl_stream_test_analog_start_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &under.request, &r),
        0);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: off the monitor output", under_name);
    if (profile_under_analog == RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE) {
        rc |= expect_int(label, r.under_analog_output_kind, RTL_STREAM_OUTPUT_SYMBOL_CQPSK);
    } else {
        rc |= expect_int(label, r.under_analog_channel_profile, RTL_STREAM_CHANNEL_PROFILE_12K5);
    }
    DSD_SNPRINTF(label, sizeof label, "-fA %s: still the analog family", under_name);
    rc |= expect_int(label, r.under_analog_family, 1);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: no analog profile published", under_name);
    rc |= expect_int(label, r.under_analog_published, 0);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: the decoder still sees the analog family", under_name);
    rc |= expect_int(label, r.under_analog_family_active, 1);

    DSD_SNPRINTF(label, sizeof label, "-fA %s, %s@%d -> digital", under_name, c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: digital switch bumps the generation", under_name);
    rc |= expect_int(label, r.generation_after_digital != r.generation_after_analog, 1);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: digital switch clears the ring", under_name);
    rc |= expect_int(label, (int)r.used_after_digital, 0);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: seeded ring was not empty", under_name);
    rc |= expect_int(label, r.used_before > 0U, 1);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: analog family left", under_name);
    rc |= expect_int(label, r.family_active_after_digital, 0);
    if (under.request.boundary_between_requests) {
        DSD_SNPRINTF(label, sizeof label, "-fA %s: digital family waits for its symbol profile", under_name);
        rc |= expect_int(label, r.digital_held_until_profile, 1);
    }
    DSD_SNPRINTF(label, sizeof label, "-fA %s: predicted digital output rate", under_name);
    rc |= expect_int(label, (int)r.predicted_digital_output_rate, r.switched_digital.output_rate);
    DSD_SNPRINTF(label, sizeof label, "-fA %s: CQPSK and back returns to the FSK discriminator", under_name);
    rc |= expect_int(label, r.output_kind_after_cqpsk_round_trip, RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR);
    return rc;
}

/* The operator toggles CQPSK on a -fA session and picks a digital mode before the demod thread reaches a block
 * boundary, so the toggle is still queued when the family request arrives (both commands drain in one pass of the
 * decoder's command queue). The switch must land on the digital mode's own symbol profile, not on that older toggle,
 * even when a boundary falls between the family request and the profile: at a forced 78125 Hz, a DMR switch decided
 * for the toggle's CQPSK would leave the discriminator unresampled at 78125 Hz, where a fresh DMR open resamples it to
 * 48 kHz, and the DMR profile that follows does not revisit the resampler. */
static int
run_queued_toggle_case(const family_case& c, int rate_hz, int forced_rate_out_hz) {
    family_case queued = c;
    queued.request.profile_under_analog = RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE_QUEUED;
    queued.request.boundary_between_requests = 1;
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    c.configure(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_demod = DSD_ANALOG_DEMOD_FM;

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    const int demod_rate_hz = forced_rate_out_hz > 0 ? forced_rate_out_hz : rate_hz;
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "-fA queued CQPSK toggle, %s@%d run", c.name, demod_rate_hz);
    int rc = expect_int(
        label,
        rtl_stream_test_analog_start_family_switch(&digital, &analog, rate_hz, forced_rate_out_hz, &queued.request, &r),
        0);
    rc |= expect_int("-fA queued CQPSK toggle: not yet consumed", r.under_analog_output_kind,
                     RTL_STREAM_OUTPUT_AUDIO_MONITOR);

    DSD_SNPRINTF(label, sizeof label, "-fA queued CQPSK toggle, %s@%d -> digital", c.name, demod_rate_hz);
    rc |= expect_int(label, r.digital_request_rc, 0);
    rc |= expect_fields_equal(label, r.switched_digital, r.fresh_digital);
    rc |= expect_int("-fA queued CQPSK toggle: digital family waits for its symbol profile",
                     r.digital_held_until_profile, 1);
    rc |= expect_int("-fA queued CQPSK toggle: digital switch bumps the generation",
                     r.generation_after_digital != r.generation_after_analog, 1);
    rc |= expect_int("-fA queued CQPSK toggle: digital switch clears the ring", (int)r.used_after_digital, 0);
    rc |= expect_int("-fA queued CQPSK toggle: analog family left", r.family_active_after_digital, 0);
    rc |= expect_int("-fA queued CQPSK toggle: predicted digital output rate", (int)r.predicted_digital_output_rate,
                     r.switched_digital.output_rate);
    return rc;
}

/* A typed DMR scan row on an analog session queues only its symbol profile. The stream's options snapshot is the one
 * it opened with (a -fA open, or a DMR open the operator had switched to analog), so the row's profile keeps the
 * monitor output as it always did, but puts the row's channel profile in place of the analog channel: the analog
 * family flag stays set, the width-driven filter and the published analog profile stand down, and the row filters with
 * its own profile. A scoped command republishes only the row's symbol profile (the decoder's configured mode is still
 * analog), which neither clears the ring, bumps the generation nor moves the output rate in the middle of the row, and
 * the row's leave brings the analog monitor back. A digital family request there is a real family switch (the operator
 * picked a digital mode), so it waits for its symbol profile like any other. */
static int
expect_digital_row_on_analog_session(const char* name, int start_digital) {
    rtl_stream_test_digital_row_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[160];
#define ROW_EXPECT(text, got, want)                                                                                    \
    do {                                                                                                               \
        DSD_SNPRINTF(label, sizeof label, "%s: %s", name, text);                                                       \
        rc |= expect_int(label, (got), (want));                                                                        \
    } while (0)
    int rc = 0;
    ROW_EXPECT("digital row run", rtl_stream_test_digital_row_on_analog_session(24000, start_digital, &r), 0);
    ROW_EXPECT("digital row open", r.open_rc, 0);
    ROW_EXPECT("row keeps the monitor output", r.row_output_kind, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    ROW_EXPECT("row keeps the analog family flag", r.row_analog_family, 1);
    ROW_EXPECT("row publishes no analog profile", r.row_published_family, 0);
    ROW_EXPECT("row keeps the monitor's 48 kHz output", r.row_output_rate, 48000);

    ROW_EXPECT("lone digital request accepted", r.lone_request_rc, 0);
    ROW_EXPECT("lone digital request waits for its symbol profile", r.lone_request_held, 1);

    ROW_EXPECT("row republish accepted", r.republish_rc, 0);
    ROW_EXPECT("row republish keeps the generation", r.generation_after == r.generation_before, 1);
    ROW_EXPECT("row republish keeps the ring", (int)r.used_after, (int)r.used_before);
    ROW_EXPECT("seeded ring was not empty", r.used_before > 0U, 1);
    ROW_EXPECT("row republish keeps the monitor output", r.output_kind_after, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    ROW_EXPECT("row republish keeps the output rate", r.output_rate_after, r.row_output_rate);
    ROW_EXPECT("row republish keeps the resampler",
               r.resamp_l_after == r.row_resamp_l && r.resamp_m_after == r.row_resamp_m, 1);

    ROW_EXPECT("row leave accepted", r.restore_rc, 0);
    ROW_EXPECT("row leave restores the monitor", r.restored_output_kind, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    ROW_EXPECT("row leave publishes the analog profile", r.restored_published_family, 1);
    ROW_EXPECT("row leave keeps the monitor rate", r.restored_output_rate, 48000);
    ROW_EXPECT("row leave bumps the generation", r.generation_after_restore != r.generation_after, 1);
#undef ROW_EXPECT
    return rc;
}

static int
test_digital_row_on_analog_session(void) {
    int rc = expect_digital_row_on_analog_session("-fA session", 0);
    rc |= expect_digital_row_on_analog_session("DMR session switched to analog", 1);
    return rc;
}

/* A width-only request on a running analog stream: the new width reaches the filter at the next block boundary, the
 * old plan and the channel/half-band delay lines are dropped, and nothing a family switch does happens (the ring and
 * the output generation are untouched). Asking again for the width already running changes nothing. */
static int
test_width_only_change(void) {
    rtl_stream_test_width_change_result w;
    DSD_MEMSET(&w, 0, sizeof w);
    int rc = expect_int("width change run", rtl_stream_test_analog_width_change(48000, 16000, 12500, &w), 0);
    rc |= expect_int("width request accepted", w.request_rc, 0);
    rc |= expect_int("width request waits for the demod thread", w.deferred_until_consume, 1);
    rc |= expect_int("new width reaches the filter", w.width_after, 12500);
    rc |= expect_int("explicit width keeps the filter on", w.lpf_enable_after, 1);
    rc |= expect_int("still the monitor output", w.output_kind_after, RTL_STREAM_OUTPUT_AUDIO_MONITOR);
    rc |= expect_int("still the analog family", w.analog_family_after, 1);
    rc |= expect_int("old plan dropped", w.plan_invalidated, 1);
    rc |= expect_int("channel history cleared", w.channel_hist_cleared, 1);
    rc |= expect_int("half-band history cleared", w.hb_hist_cleared, 1);
    rc |= expect_int("new width published", w.published_width_hz, 12500);
    rc |= expect_int("new width published as filtered", w.published_lpf_on, 1);
    rc |= expect_int("width change keeps the generation", w.generation_after == w.generation_before, 1);
    rc |= expect_int("width change keeps the ring", (int)w.used_after, (int)w.used_before);
    rc |= expect_int("seeded ring was not empty", w.used_before > 0U, 1);
    rc |= expect_int("same width accepted", w.same_width_rc, 0);
    rc |= expect_int("same width keeps the histories", w.same_width_kept_histories, 1);
    rc |= expect_int("same width keeps the plan", w.same_width_kept_plan, 1);

    /* From the unset default (legacy enable rule) to an explicit width at 24 kHz. */
    DSD_MEMSET(&w, 0, sizeof w);
    rc |= expect_int("default to explicit run", rtl_stream_test_analog_width_change(24000, 0, 8000, &w), 0);
    rc |= expect_int("default to explicit width", w.width_after, 8000);
    rc |= expect_int("default to explicit plan dropped", w.plan_invalidated, 1);
    rc |= expect_int("default to explicit histories cleared", w.channel_hist_cleared && w.hb_hist_cleared, 1);
    rc |= expect_int("default to explicit published", w.published_width_hz, 8000);
    return rc;
}

/* With no stream there is no published rate to check a width against: the next open validates it against the rate it
 * actually delivers, so a width a previous session's rate could not fit is not refused, while the kind and range
 * rules still apply. */
static int
test_requests_without_stream(void) {
    int retune_rc = 99;
    int rc =
        expect_int("no-stream width ignores a stale 12 kHz rate",
                   rtl_stream_test_analog_request_without_stream(12000, DSD_ANALOG_DEMOD_FM, 16000, &retune_rc), 0);
    rc |= expect_int("no-stream retune width ignores a stale 12 kHz rate", retune_rc, 0);
    retune_rc = 99;
    rc |= expect_int("no-stream out-of-range width refused",
                     rtl_stream_test_analog_request_without_stream(48000, DSD_ANALOG_DEMOD_FM, 30000, &retune_rc), -1);
    rc |= expect_int("no-stream out-of-range retune width refused", retune_rc, -1);
    retune_rc = 99;
    rc |= expect_int("no-stream AM refused",
                     rtl_stream_test_analog_request_without_stream(48000, DSD_ANALOG_DEMOD_AM, 0, &retune_rc), -1);
    rc |= expect_int("no-stream AM retune refused", retune_rc, -1);
    return rc;
}

namespace {

struct live_request_case {
    const char* name;
    int rate_hz;
    int analog_stream; /* 0: a DMR session asked to switch to analog */
    int post_downsample;
    int refused_width_hz;  /* in range for NFM, but the running stream cannot realize it */
    int accepted_width_hz; /* what the same stream does realize (0 = the unset default) */
    int accepted_filter_width_hz;
    const char* refusal_text; /* the validator's text the refusal logs */
};

} // namespace

static int
expect_live_refusal(const live_request_case& c) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[160];
    g_last_error[0] = '\0';
    g_error_count = 0;
    DSD_SNPRINTF(label, sizeof label, "%s: refused %d Hz run", c.name, c.refused_width_hz);
    int rc = expect_int(label,
                        rtl_stream_test_analog_request_with_stream(c.rate_hz, c.analog_stream, c.post_downsample,
                                                                   DSD_ANALOG_DEMOD_FM, c.refused_width_hz, &r),
                        0);
    DSD_SNPRINTF(label, sizeof label, "%s: live request refused", c.name);
    rc |= expect_int(label, r.request_rc, -1);
    /* The refusal is reported, not silent: the live request logs the validator's text and what stays in place, and
     * the retune profile for the same width and rate that follows it is not logged a second time. */
    DSD_SNPRINTF(label, sizeof label, "%s: one refusal logged for the width and rate", c.name);
    rc |= expect_int(label, g_error_count, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: refusal logs the validator's text", c.name);
    rc |= expect_int(label, std::strstr(g_last_error, c.refusal_text) != NULL, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: refusal says the front end keeps its profile", c.name);
    rc |= expect_int(label, std::strstr(g_last_error, "The front end keeps its current receive profile.") != NULL, 1);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }
    DSD_SNPRINTF(label, sizeof label, "%s: nothing queued", c.name);
    rc |= expect_int(label, r.request_queued, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: family unchanged", c.name);
    rc |= expect_int(label, r.family_after, r.family_before);
    DSD_SNPRINTF(label, sizeof label, "%s: filter width unchanged", c.name);
    rc |= expect_int(label, r.width_after, r.width_before);
    DSD_SNPRINTF(label, sizeof label, "%s: channel plan kept", c.name);
    rc |= expect_int(label, r.plan_kept, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: published width unchanged", c.name);
    rc |= expect_int(label, r.published_width_after, r.published_width_before);
    DSD_SNPRINTF(label, sizeof label, "%s: retune profile refused", c.name);
    rc |= expect_int(label, r.retune_rc, -1);
    DSD_SNPRINTF(label, sizeof label, "%s: no retune profile queued", c.name);
    rc |= expect_int(label, r.retune_queued, 0);
    return rc;
}

static int
expect_live_acceptance(const live_request_case& c) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "%s: accepted %d Hz run", c.name, c.accepted_width_hz);
    int rc = expect_int(label,
                        rtl_stream_test_analog_request_with_stream(c.rate_hz, c.analog_stream, c.post_downsample,
                                                                   DSD_ANALOG_DEMOD_FM, c.accepted_width_hz, &r),
                        0);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable request accepted", c.name);
    rc |= expect_int(label, r.request_rc, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable request queued", c.name);
    rc |= expect_int(label, r.request_queued, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: lands on the analog family", c.name);
    rc |= expect_int(label, r.family_after, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable width reaches the filter", c.name);
    rc |= expect_int(label, r.width_after, c.accepted_filter_width_hz);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable retune profile accepted", c.name);
    rc |= expect_int(label, r.retune_rc, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: realizable retune profile queued", c.name);
    rc |= expect_int(label, r.retune_queued, 1);
    return rc;
}

/* While a stream runs, each request is checked against the demod rate that stream publishes, and against the
 * post-demod decimation an I/Q replay sidecar can set: a width in NFM's range that the running stream cannot realize is
 * refused before anything is queued, as a live request and as a retune profile alike, and the running channel stays as
 * it was. Were such a width queued, the demodulator would have no filter design for it and run the channel unfiltered.
 * Beside each refusal a width the same stream does realize is accepted, so the refusal is the rate's doing. */
static int
test_requests_against_running_stream(void) {
    const live_request_case cases[] = {
        /* 24 kHz fits up to 20.4 kHz: 25 kHz is in range, but its 13.1 kHz cutoff is above 0.45 x 24 kHz. */
        {"24 kHz analog stream", 24000, 1, 1, 25000, 20000, 20000,
         "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate (the largest width it fits is 20.4 kHz)"},
        /* 16 kHz fits up to 13.2 kHz, so even the 16 kHz default width is out of reach when asked for explicitly. */
        {"16 kHz analog stream", 16000, 1, 1, 16000, 13000, 13000,
         "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate (the largest width it fits is 13.2 kHz)"},
        /* A digital session switching to analog is held to the rate it runs at, too. */
        {"24 kHz DMR stream", 24000, 0, 1, 25000, 20000, 20000,
         "NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate (the largest width it fits is 20.4 kHz)"},
        /* A replay decimating by 2 after demod runs the channel filter at twice the published rate: every requested
         * width is refused there, while the unset default keeps its legacy design (16 kHz at 48 kHz). */
        {"48 kHz replay with post_downsample 2", 48000, 1, 2, 12500, 0, 16000,
         "NFM bandwidth 12.5 kHz cannot be applied to this I/Q replay: post_downsample 2 runs the channel filter at "
         "96000 Hz"},
    };
    int rc = 0;
    for (const live_request_case& c : cases) {
        rc |= expect_live_refusal(c);
        rc |= expect_live_acceptance(c);
    }
    return rc;
}

/* A live request is checked against the rate the stream published when it was made; a retune that settles the device
 * on another rate can land before the demod thread consumes it. The width is held to the rate it lands on: one that
 * rate cannot realize is refused there, logged with the validator's text, and the channel stays as the retune left it,
 * rather than running unfiltered. One the landed rate still fits is applied. */
static int
test_request_across_rate_change(void) {
    rtl_stream_test_live_request_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    g_last_error[0] = '\0';
    g_error_count = 0;
    int rc = expect_int("16 kHz across 48 -> 16 kHz run",
                        rtl_stream_test_analog_request_across_rate_change(48000, 16000, 16000, &r), 0);
    rc |= expect_int("16 kHz accepted at the published 48 kHz", r.request_rc, 0);
    rc |= expect_int("16 kHz queued", r.request_queued, 1);
    rc |= expect_int("16 kHz refused at the landed rate: width unchanged", r.width_after, r.width_before);
    rc |= expect_int("16 kHz refused at the landed rate: family unchanged", r.family_after, r.family_before);
    rc |= expect_int("16 kHz refused at the landed rate: plan kept", r.plan_kept, 1);
    rc |= expect_int("16 kHz refused at the landed rate: published width unchanged", r.published_width_after,
                     r.published_width_before);
    rc |= expect_int("landed-rate refusal logged once", g_error_count, 1);
    rc |= expect_int("landed-rate refusal logs the validator's text",
                     std::strstr(g_last_error, "NFM bandwidth 16 kHz does not fit the 16 kHz DSP rate (the largest "
                                               "width it fits is 13.2 kHz)")
                         != NULL,
                     1);
    rc |= expect_int("landed-rate refusal says the front end keeps its profile",
                     std::strstr(g_last_error, "The front end keeps its current receive profile.") != NULL, 1);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "  logged: \"%s\"\n", g_last_error);
    }

    DSD_MEMSET(&r, 0, sizeof r);
    rc |= expect_int("12.5 kHz across 48 -> 16 kHz run",
                     rtl_stream_test_analog_request_across_rate_change(48000, 16000, 12500, &r), 0);
    rc |= expect_int("12.5 kHz accepted", r.request_rc, 0);
    rc |= expect_int("12.5 kHz still fits the landed rate and applies", r.width_after, 12500);
    rc |= expect_int("12.5 kHz published", r.published_width_after, 12500);
    return rc;
}

int
main(void) {
    dsd_neo_log_set_tap(capture_error_log, NULL);
    dsd_neo_config_init();
    const family_case cases[] = {
        {"P25 C4FM",
         p25_c4fm,
         {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_C4FM, 10, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"P25 CQPSK",
         p25_cqpsk,
         {1, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK, 10, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"DMR", dmr, {0, 4800, 4, RTL_STREAM_CHANNEL_PROFILE_12K5, 10, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"NXDN48", nxdn48, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
        {"dPMR", dpmr, {0, 2400, 4, RTL_STREAM_CHANNEL_PROFILE_6K25, 20, 0, RTL_STREAM_TEST_UNDER_ANALOG_NONE}},
    };
    int rc = 0;
    for (const family_case& c : cases) {
        rc |= run_case(c, 48000, 0, 0);
    }
    /* 24 kHz: the analog leg resamples its audio to 48 kHz while the digital
     * discriminator stream does not, so the output rate has to follow the family. */
    family_case dmr24 = cases[2];
    dmr24.request.ted_sps = 5;
    rc |= run_case(dmr24, 24000, 0, 0);
    /* An explicit width travels with the analog request. */
    rc |= run_case(cases[0], 48000, 0, 12500);

    /* Rates a device forces (Airspy 2.5 MS/s -> 78125 Hz; a 60 kHz grid). The digital
     * resampler decision depends on the symbol profile the switch lands on, so it has to be
     * made for that profile, not for the analog monitor's placeholder: CQPSK never resamples,
     * a 2400 sym/s profile divides 60 kHz evenly, and a 4800 sym/s one at 78125 Hz resamples
     * to 48 kHz with the same ratio the analog monitor already runs. */
    family_case cqpsk78 = cases[1];
    cqpsk78.request.ted_sps = 16;
    rc |= run_case(cqpsk78, 48000, 78125, 0);
    family_case c4fm78 = cases[0];
    rc |= run_case(c4fm78, 48000, 78125, 0);
    family_case nxdn60 = cases[3];
    nxdn60.request.ted_sps = 25;
    rc |= run_case(nxdn60, 48000, 60000, 0);
    family_case dpmr60 = cases[4];
    dpmr60.request.ted_sps = 25;
    rc |= run_case(dpmr60, 48000, 60000, 0);

    /* The family request and its symbol profile are two requests, and the demod thread can reach a block boundary
     * between them. The switch must still land on the profile: at 60 kHz a 4800 sym/s placeholder would resample a
     * 2400 sym/s stream to 48 kHz, and at 78125 Hz it would resample a CQPSK stream that never resamples. */
    family_case nxdn60_split = nxdn60;
    nxdn60_split.request.boundary_between_requests = 1;
    rc |= run_case(nxdn60_split, 48000, 60000, 0);
    family_case dpmr60_split = dpmr60;
    dpmr60_split.request.boundary_between_requests = 1;
    rc |= run_case(dpmr60_split, 48000, 60000, 0);
    family_case cqpsk78_split = cqpsk78;
    cqpsk78_split.request.boundary_between_requests = 1;
    rc |= run_case(cqpsk78_split, 48000, 78125, 0);
    family_case dmr48_split = cases[2];
    dmr48_split.request.boundary_between_requests = 1;
    rc |= run_case(dmr48_split, 48000, 0, 0);

    /* The same switches from a session that started with -fA rather than from a digital open, so nothing the digital
     * open left behind can stand in for what the switch must set: each mode, the 24 kHz and forced-rate resampler
     * decisions, and the boundary that falls between the family request and its symbol profile. */
    for (const family_case& c : cases) {
        rc |= run_analog_start_case(c, 48000, 0);
    }
    rc |= run_analog_start_case(dmr24, 24000, 0);
    rc |= run_analog_start_case(cqpsk78, 48000, 78125);
    rc |= run_analog_start_case(c4fm78, 48000, 78125);
    rc |= run_analog_start_case(nxdn60, 48000, 60000);
    rc |= run_analog_start_case(dpmr60_split, 48000, 60000);

    /* The same switch from a -fA session a CQPSK toggle or a typed DMR row had moved off the monitor output, including
     * the forced-rate resampler decisions and a boundary between the family request and its profile. */
    const int under_analog[] = {RTL_STREAM_TEST_UNDER_ANALOG_CQPSK_TOGGLE, RTL_STREAM_TEST_UNDER_ANALOG_TYPED_ROW};
    for (int under : under_analog) {
        for (const family_case& c : cases) {
            rc |= run_under_analog_case(c, under, 48000, 0);
        }
        rc |= run_under_analog_case(dmr24, under, 24000, 0);
        rc |= run_under_analog_case(cqpsk78, under, 48000, 78125);
        rc |= run_under_analog_case(c4fm78, under, 48000, 78125);
        rc |= run_under_analog_case(dpmr60_split, under, 48000, 60000);
        rc |= run_under_analog_case(dmr48_split, under, 48000, 0);
    }

    /* A CQPSK toggle still queued when the digital mode is picked, with a boundary between the family request and the
     * mode's symbol profile: each mode, the 24 kHz and forced-rate resampler decisions. */
    for (const family_case& c : cases) {
        rc |= run_queued_toggle_case(c, 48000, 0);
    }
    rc |= run_queued_toggle_case(dmr24, 24000, 0);
    rc |= run_queued_toggle_case(cases[2], 48000, 78125);
    rc |= run_queued_toggle_case(c4fm78, 48000, 78125);
    rc |= run_queued_toggle_case(nxdn60, 48000, 60000);

    rtl_stream_test_family_switch_result r;
    DSD_MEMSET(&r, 0, sizeof r);
    static dsd_opts digital;
    static dsd_opts analog;
    DSD_MEMSET(&digital, 0, sizeof digital);
    DSD_MEMSET(&analog, 0, sizeof analog);
    dmr(&digital);
    analog.analog_only = 1;
    analog.monitor_input_audio = 1;
    analog.analog_nfm_bandwidth_hz = 12500;
    rc |= expect_int("explicit width run",
                     rtl_stream_test_analog_family_switch(&digital, &analog, 48000, 0, &cases[2].request, &r), 0);
    rc |= expect_int("explicit width reaches the filter", r.switched_analog.channel_lpf_width_hz, 12500);
    rc |= expect_int("explicit width published", r.published_width_hz, 12500);
    rc |= expect_int("explicit width published as filtered", r.published_lpf_on, 1);

    rc |= test_digital_row_on_analog_session();
    rc |= test_width_only_change();
    rc |= test_requests_without_stream();
    rc |= test_requests_against_running_stream();
    rc |= test_request_across_rate_change();

    /* Requests the front end cannot honour are refused up front. */
    rc |= expect_int("AM refused", rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_AM, 0), -1);
    rc |= expect_int("bad family refused", rtl_stream_request_analog_profile(7, DSD_ANALOG_DEMOD_FM, 0), -1);
    rc |= expect_int("out-of-range width refused",
                     rtl_stream_request_analog_profile(DSD_RX_FAMILY_ANALOG, DSD_ANALOG_DEMOD_FM, 30000), -1);
    if (rc == 0) {
        std::printf("IO_RTL_ANALOG_FAMILY_SWITCH: OK\n");
    }
    return rc;
}
