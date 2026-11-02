// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/receive_view.h>
#include <dsd-neo/core/analog_mode.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/wav_journal.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/dsp/analog_rx.h>
#include <dsd-neo/dsp/analog_sink.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "test_support.h"
#include "wav_segment_test_support.h"

/* A radio input's analog calls need a squelch above the startup default (issue #667 D2): -60 dB, a level a test's
   blocks never meet, since the tracker trusts the inputs it is handed. */
static void
set_radio_squelch(dsd_opts* opts) {
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-60.0);
}

static void
set_filter(dsd_opts* opts, int kind, const char* allow, const char* block) {
    char error[160];
    assert(dsd_receive_edit_parse(&opts->receive, kind, 1, allow, block, "", error, sizeof error) == 0);
}

static void
test_protocol_context(dsd_opts* opts, dsd_state* state) {
    set_filter(opts, DSD_RECEIVE_SLOT, "1", "");
    set_filter(opts, DSD_RECEIVE_NAC, "293", "");
    state->nac = 0x293;
    assert(dsd_receive_allows(opts, state, DSD_SYNC_P25P1_POS, 1)); /* FDMA has no slot. */
    state->p2_cc = 0x293;
    state->p2_cc_verified = 0;
    assert(dsd_receive_status(opts, state, DSD_SYNC_P25P2_POS, 0) == DSD_RECEIVE_CHECKING);
    state->p2_cc_verified = 1;
    assert(dsd_receive_allows(opts, state, DSD_SYNC_P25P2_POS, 0));
    assert(!dsd_receive_allows(opts, state, DSD_SYNC_P25P2_POS, 1));
    assert(dsd_receive_grant_allows(opts, DSD_SYNC_P25P1_POS, 1));
    assert(!dsd_receive_grant_allows(opts, DSD_SYNC_P25P2_POS, 1));
    set_filter(opts, DSD_RECEIVE_RAN, "", "63");
    state->nxdn_last_ran = 0;
    state->nxdn_last_ran_stand_in = 1;
    assert(dsd_receive_status(opts, state, DSD_SYNC_NXDN_POS, 0) == DSD_RECEIVE_CHECKING);
    state->nxdn_last_ran_stand_in = 0;
    assert(dsd_receive_allows(opts, state, DSD_SYNC_NXDN_POS, 0));
    state->nxdn_last_ran = 64;
    assert(dsd_receive_status(opts, state, DSD_SYNC_NXDN_POS, 0) == DSD_RECEIVE_CHECKING);
    state->nxdn_last_ran = 63;
    assert(!dsd_receive_allows(opts, state, DSD_SYNC_NXDN_POS, 0));
    /* The single-slot DMR decoder (-fr/-nm on a repeater) decodes every burst on lane 0: the receive slot is the one
       its CACH proved, never lane 0's own number, and a slot not proven yet is Checking (issue #637). */
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS2;
    assert(!dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, 0));
    dsd_receive_identity mono;
    dsd_receive_identity_current(state, DSD_SYNC_DMR_BS_VOICE_POS, 0, &mono);
    assert(mono.slot == 2 && !mono.slot_pending);
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    assert(dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, 0));
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_PENDING;
    assert(dsd_receive_status(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, 0) == DSD_RECEIVE_CHECKING);
    /* The shared status text names the one slot the single-slot lane carries, never both lanes as one. */
    char status[128];
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS2;
    dsd_app_receive_status_text(opts, state, status, sizeof status);
    assert(strcmp(status, "Slot 2: Rejected by receive filter") == 0);
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_NONE;
    dsd_app_receive_status_text(opts, state, status, sizeof status);
    assert(strcmp(status, "Slot 1: Allowed; slot 2: Rejected by receive filter") == 0);
    state->synctype = DSD_SYNC_NONE;
    assert(!dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, 1)); /* Two-slot decoding: the burst's slot. */
    assert(dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, 0));
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->receive_context_active = 1;
    state->receive_context_protocol = DSD_SYNC_YSF_POS;
    assert(dsd_receive_current_allows(opts, state, 1)); /* Borrowed vocoder keeps its owner's protocol. */
    state->receive_context_active = 0;
    assert(!dsd_receive_current_allows(opts, state, 1));
    /* YSF and scrambled dPMR play through NXDN's decoder under its synctype: playback is judged as the owner's, and
       the owner's context ends where its playback does. */
    state->synctype = DSD_SYNC_NXDN_POS;
    state->nxdn_last_ran = 63;
    assert(!dsd_receive_current_allows(opts, state, 0));
    const dsd_receive_context outer = dsd_receive_context_begin(state, DSD_SYNC_YSF_POS);
    assert(dsd_receive_current_allows(opts, state, 0));
    const dsd_receive_context inner = dsd_receive_context_begin(state, DSD_SYNC_NXDN_POS);
    assert(!dsd_receive_current_allows(opts, state, 0));
    dsd_receive_context_end(state, inner);
    assert(dsd_receive_current_allows(opts, state, 0));
    dsd_receive_context_end(state, outer);
    assert(!dsd_receive_current_allows(opts, state, 0) && !state->receive_context_active);
}

typedef struct {
    const char* dir;
    int files;
    int traveler;
    int dispatch;
    int unnamed;
} recording_check;

static int
check_recording(const char* name, void* user) {
    recording_check* check = user;
    assert(strstr(name, ".wav")); /* NFM's zero target must not fabricate an Rdio sidecar. */
    assert(!strstr(name, "TEMP"));
    /* The analog name, ANALOG on this audio input (issue #667): no talkgroup, source or call type, and the sanitized
       service last (issue #637). */
    const int traveler = dsd_test_analog_recording_name_is(name, "ANALOG", "Traveler__West_");
    const int dispatch = dsd_test_analog_recording_name_is(name, "ANALOG", "Dispatch");
    const int unnamed = dsd_test_analog_recording_name_is(name, "ANALOG", NULL);
    assert(traveler || dispatch || unnamed);
    check->traveler += traveler;
    check->dispatch += dispatch;
    check->unnamed += unnamed;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, check->dir, name) == 0);
    SF_INFO info = {0};
    SNDFILE* wav = sf_open(path, SFM_READ, &info);
    assert(wav && info.samplerate == 48000 && info.channels == 1);
    assert(info.frames == 960);
    short samples[960];
    assert(sf_read_short(wav, samples, 960) == 960);
    if (strstr(name, "_SVC_Dispatch")) {
        /* No resampler tail from the previous service, even on an uninterrupted carrier. */
        for (int i = 0; i < 960; i++) {
            assert(samples[i] == 0);
        }
    } else {
        assert(samples[480] != 0); /* Muted speakers still record accepted PCM. */
    }
    assert(sf_close(wav) == 0);
    assert(remove(path) == 0);
    check->files++;
    return 0;
}

typedef struct {
    const char* dir;
    int files;
    int traveler;
    int dispatch;
} service_files;

static int
count_service_files(const char* name, void* user) {
    service_files* check = user;
    check->traveler += dsd_test_analog_recording_name_is(name, "ANALOG", "Traveler__West_");
    check->dispatch += dsd_test_analog_recording_name_is(name, "ANALOG", "Dispatch");
    check->files++;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, check->dir, name) == 0);
    assert(remove(path) == 0);
    return 0;
}

typedef struct {
    const char* dir;
    int files;
    long frames;
} recording_frames;

static int
count_recording_frames(const char* name, void* user) {
    recording_frames* check = user;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, check->dir, name) == 0);
    SF_INFO info = {0};
    SNDFILE* wav = sf_open(path, SFM_READ, &info);
    assert(wav);
    check->frames += (long)info.frames;
    assert(sf_close(wav) == 0);
    assert(remove(path) == 0);
    check->files++;
    return 0;
}

typedef struct {
    const char* dir;
    int files;
    int temp;
    int unnamed;
} teardown_check;

static int
check_teardown_recording(const char* name, void* user) {
    teardown_check* check = user;
    check->temp += strstr(name, "TEMP") != NULL;
    check->unnamed += dsd_test_analog_recording_name_is(name, "ANALOG", NULL);
    check->files++;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, check->dir, name) == 0);
    assert(remove(path) == 0);
    return 0;
}

/* Issue #637: enrichment (an alias, GPS, text) never reaches the history row of a call the receive policy is still
   checking or rejects. */
static void
test_rejected_call_takes_no_enrichment(dsd_opts* opts, dsd_state* state) {
    const dsd_receive_policy saved = opts->receive;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    set_filter(opts, DSD_RECEIVE_DCC, "", "7");
    state->dmr_color_code = 7;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 1,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_source_id = 11,
        .ota_target_id = 22,
        .policy_target_id = 22,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_state_refresh_receive(opts, state, 1U);
    dsd_event_sync_slot(opts, state, 1U);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, 1U, &call) > 0 && call.receive_verdict == DSD_RECEIVE_REJECTED);
    assert(dsd_event_enrich_alias(state, 1U, call.epoch, "Unit 11") == 0);
    dsd_event_sync_slot(opts, state, 1U);
    assert(dsd_call_state_end(state, 1U, 0.0) == 1);
    dsd_event_sync_slot(opts, state, 1U);
    for (int i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        assert(strstr(state->event_history_s[1].Event_History_Items[i].alias, "Unit 11") == NULL);
    }
    opts->receive = saved;
    state->dmr_color_code = 16;
}

/* Issue #637: a call the policy admitted and a live edit then rejects still commits the segment it staged while
   admitted: the row reaches history when the call ends. */
static void
test_admitted_segment_commits_after_rejection(dsd_opts* opts, dsd_state* state) {
    const dsd_receive_policy saved = opts->receive;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    set_filter(opts, DSD_RECEIVE_DCC, "7", "");
    state->dmr_color_code = 7;
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 1,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_source_id = 31,
        .ota_target_id = 32,
        .policy_target_id = 32,
    };
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_state_refresh_receive(opts, state, 1U);
    dsd_event_sync_slot(opts, state, 1U);
    assert(state->event_history_s[1].Event_History_Items[0].target_id == 32);
    set_filter(opts, DSD_RECEIVE_DCC, "", "7");
    dsd_call_state_refresh_receive(opts, state, 1U);
    assert(dsd_call_state_end(state, 1U, 0.0) == 1);
    dsd_event_sync_slot(opts, state, 1U);
    assert(state->event_history_s[1].Event_History_Items[1].target_id == 32);
    opts->receive = saved;
    state->dmr_color_code = 16;
}

/* Issue #637: carrier-wide admission judges both over-the-air slots, whichever one the single-slot DMR decoder proved
   for its voice lane: standalone data on the other slot is that slot's, so a -Y scan does not stay on rejected TS2
   traffic after a TS1 call. */
/* Issue #637: a call reacquired after a sync loss, before its code decodes again, keeps the service its IDs and crypto
   were seeded from, so a different code decoded next ends it instead of relabelling the old call's identity. */
static void
test_reacquired_call_keeps_its_service(dsd_opts* opts, dsd_state* state) {
    const dsd_receive_policy saved = opts->receive;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    state->dmr_color_code = 7;
    const dsd_call_observation voice = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS,
                                        .slot = 0,
                                        .kind = DSD_CALL_KIND_GROUP_VOICE,
                                        .ota_target_id = 100,
                                        .ota_source_id = 200,
                                        .policy_target_id = 100};
    assert(dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_BEGIN) == 1);
    assert(dsd_call_state_end_ex(state, 0U, 0.0, DSD_CALL_END_SYNC_LOSS) == 1);
    state->dmr_color_code = 16; /* The no-carrier pass forgot it. */
    const dsd_call_observation anonymous = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS, .slot = 0, .kind = DSD_CALL_KIND_VOICE};
    assert(dsd_call_state_observe(state, &anonymous, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(call.ota_target_id == 100 && call.receive_identity.known && call.receive_identity.value == 7);
    state->dmr_color_code = 8;
    dsd_call_state_refresh_receive(opts, state, 0U);
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.phase != DSD_CALL_PHASE_ACTIVE);
    state->dmr_color_code = 16;
    opts->receive = saved;
}

static void
test_carrier_admission_judges_both_slots(dsd_opts* opts) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof state);
    const dsd_receive_policy saved = opts->receive;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    assert(dsd_receive_carrier_allows(opts, &state)); /* No filter: everything. */
    set_filter(opts, DSD_RECEIVE_SLOT, "1", "");
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 7;
    state.dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    assert(dsd_receive_allows(opts, &state, DSD_SYNC_DMR_BS_DATA_POS, 1)); /* The mono lane's proven TS1. */
    assert(!dsd_receive_carrier_allows(opts, &state));
    set_filter(opts, DSD_RECEIVE_SLOT, "1/2", "");
    assert(dsd_receive_carrier_allows(opts, &state));
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    set_filter(opts, DSD_RECEIVE_DCC, "", "7");
    state.synctype = DSD_SYNC_NXDN_POS;
    state.nxdn_last_ran = 9;
    assert(dsd_receive_carrier_allows(opts, &state)); /* FDMA: the protocol's own code. */
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    assert(!dsd_receive_carrier_allows(opts, &state));
    opts->receive = saved;
}

/* Issue #637: the single-slot DMR decoder carries both over-the-air slots on lane 0, so a proven move from TS1 to TS2
   with the same colour code is a new service: its call ends rather than taking TS2's name. While the next slot is
   still pending the call keeps the slot it had proven. */
static void
test_mono_slot_change_is_a_boundary(dsd_opts* opts, dsd_state* state) {
    const dsd_receive_policy saved = opts->receive;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    assert(dsd_receive_names_parse("--slot-name '1=Service A' --slot-name '2=Service B'", &opts->receive.names, NULL, 0)
           == 0);
    state->dmr_color_code = 7;
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    const dsd_call_observation voice = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS, .slot = 0, .kind = DSD_CALL_KIND_VOICE};
    assert(dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_state_refresh_receive(opts, state, 0U);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.receive_identity.slot == 1);
    const uint64_t epoch = call.epoch;
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_PENDING;
    (void)dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_CONTINUE);
    dsd_call_state_refresh_receive(opts, state, 0U);
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE && call.epoch == epoch);
    assert(call.receive_identity.slot == 1 && !strcmp(call.service_label, "Service A"));
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS2;
    dsd_call_state_refresh_receive(opts, state, 0U);
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.phase != DSD_CALL_PHASE_ACTIVE);
    assert(dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_CONTINUE) == 1);
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.epoch != epoch && call.receive_identity.slot == 2);
    /* An observation alone (no refresh in between) splits the call too. */
    const uint64_t ts2_epoch = call.epoch;
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_TS1;
    (void)dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_CONTINUE);
    assert(dsd_call_state_get(state, 0U, &call) > 0 && call.epoch != ts2_epoch && call.receive_identity.slot == 1);
    assert(dsd_call_state_end(state, 0U, 0.0) == 1);
    state->dmr_mono_rx_slot = DSD_DMR_MONO_RX_NONE;
    state->dmr_color_code = 16;
    opts->receive = saved;
}

static void
test_events(dsd_opts* opts, dsd_state* state) {
    set_filter(opts, DSD_RECEIVE_DCC, "7/8", "8");
    assert(dsd_receive_names_parse("--slot-name '1=Dispatch' --dcc-name '7=Hospital' --dcc-name '8=Blocked'",
                                   &opts->receive.names, NULL, 0)
           == 0);
    state->synctype = state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->dmr_color_code = 16;
    dsd_call_observation voice = {.protocol = DSD_SYNC_DMR_BS_VOICE_POS,
                                  .slot = 0,
                                  .kind = DSD_CALL_KIND_GROUP_VOICE,
                                  .ota_target_id = 10,
                                  .ota_source_id = 20,
                                  .policy_target_id = 10};
    (void)dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(opts, state, 0);
    Event_History_I* history = state->event_history_s;
    assert(!history[0].Event_History_Items[0].event_string[0]);
    const dsd_call_observation data = dsd_call_observation_data(DSD_SYNC_DMR_BS_DATA_POS, 0, 20, 10);
    (void)dsd_event_emit_data_notice(opts, state, 0, &data, "Denied data");
    assert(!history[0].Event_History_Items[1].event_string[0]);
    dsd_event_note_current_call(opts, state, 0, "Denied detail");
    assert(!history[0].Event_History_Items[0].internal_str[0]);
    assert(dsd_event_set_open_call_detail(state, 0, DSD_EVENT_DETAIL_GPS, "Denied GPS") == 0);
    state->dmr_color_code = 7;
    dsd_event_sync_slot(opts, state, 0); /* A late validated identifier admits this call. */
    assert(!strcmp(history[0].Event_History_Items[0].service_label, "Hospital"));
    assert(strstr(history[0].Event_History_Items[0].event_string, "Hospital"));
    assert(!history[0].Event_History_Items[0].gps_s[0]);
    voice.slot = 1;
    (void)dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_BEGIN);
    dsd_event_sync_slot(opts, state, 1);
    assert(!history[1].Event_History_Items[0].event_string[0]);
    state->dmr_color_code = 8;
    dsd_event_sync_slot(opts, state, 0); /* Close the accepted service under its old name. */
    assert(!strcmp(history[0].Event_History_Items[1].service_label, "Hospital"));
    assert(history[0].Event_History_Items[1].receive_identity.value == 7);
    voice.slot = 0;
    (void)dsd_call_state_observe(state, &voice, DSD_CALL_BOUNDARY_CONTINUE);
    dsd_event_sync_slot(opts, state, 0);
    (void)dsd_event_emit_data_notice(opts, state, 0, &data, "Denied data");
    assert(!history[0].Event_History_Items[0].event_string[0]);
    assert(!history[0].Event_History_Items[2].event_string[0]);
}

/* A reception becomes a call only once it ran DSD_ANALOG_CALL_MIN_MS (issue #667). The tone check before a new call,
   as the symbol path hands it over under a tone filter: a call still running ends at the first of these blocks (these
   tests run with the hold off), and every one of them, that first one too, is the new reception's lead-in, recorded
   nowhere, enough that a block of @p count samples at @p rate after them brings it to the minimum. */
static void
analog_lead_in(dsd_opts* opts, dsd_state* state, size_t count, int rate) {
    static const float checked[960] = {0};
    const uint64_t need = (uint64_t)DSD_ANALOG_CALL_MIN_MS * 48U;
    for (uint64_t fed = 0; (fed + count) * 48000U / (uint64_t)rate < need; fed += 960U) {
        dsd_engine_analog_call_block(opts, state, checked, 960, rate, DSD_ANALOG_CALL_PENDING);
        assert(!dsd_engine_analog_call_active(state));
    }
}

static void
test_analog(dsd_opts* opts, dsd_state* state) {
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->audio_out = 0;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    char directory[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(directory, sizeof directory, "dsdneo_receive_wav"));
    DSD_SNPRINTF(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", directory);
    opts->dmr_stereo_wav = 1;
    assert(dsd_analog_tone_detection_active(opts));
    assert(dsd_receive_names_parse("--tone-name '141.3=Traveler /West?' --tone-name 'D023I=Dispatch'",
                                   &opts->receive.names, NULL, 0)
           == 0);
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1413;
    float block[960] = {0};
    for (int i = 0; i < 960; ++i) {
        block[i] = 2000.0f; /* Monitor blocks use signed-16 PCM units in floats. */
    }
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));
    analog_lead_in(opts, state, 480, 24000);
    dsd_engine_analog_call_block(opts, state, block, 480, 24000, 1);
    assert(dsd_engine_analog_call_active(state));
    Event_History_I* history = state->event_history_s;
    assert(!strcmp(history[0].Event_History_Items[0].service_label, "Traveler /West?"));
    assert(history[0].Event_History_Items[0].target_id == 0);
    /* An audio input's analog call names no mode: another receiver demodulated it (issue #667). */
    assert(strcmp(history[0].Event_History_Items[0].sysid_string, "ANALOG") == 0);
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    state->analog_rx.dcs_code = 047;
    state->analog_rx.dcs_inverted = 0;
    DSD_MEMSET(block, 0, sizeof block);
    /* The new code's check ends the call (the carrier stays up), and its first block begins the next one. */
    analog_lead_in(opts, state, 480, 24000);
    dsd_engine_analog_call_block(opts, state, block, 480, 24000, 1);
    assert(!strcmp(history[0].Event_History_Items[1].service_label, "Traveler /West?"));
    assert(!strcmp(history[0].Event_History_Items[0].service_label, "Dispatch"));
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    for (int i = 0; i < 960; ++i) {
        block[i] = 2000.0f;
    }
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(!strcmp(history[0].Event_History_Items[1].service_label, "Dispatch"));
    assert(!history[0].Event_History_Items[0].service_label[0]);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));
    assert(history[0].Event_History_Items[1].receive_identity.kind == DSD_RECEIVE_NONE);
    assert(history[0].Event_History_Items[3].receive_identity.value == 1413);
    recording_check check = {.dir = directory};
    assert(dsd_dir_list(directory, check_recording, &check) == 0);
    assert(check.files == 3 && check.traveler == 1 && check.dispatch == 1 && check.unnamed == 1);
    /* A tone still being acquired between two confirmed ones does not hide the change: each service gets its own
       recording, never one under the later name. The new code's reception runs the minimum to be a call of its own. */
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1413;
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    for (int i = 0; i < DSD_ANALOG_CALL_MIN_MS / 20; i++) {
        dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    }
    assert(dsd_engine_analog_call_active(state));
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    service_files services = {.dir = directory};
    assert(dsd_dir_list(directory, count_service_files, &services) == 0);
    assert(services.files == 2 && services.traveler == 1 && services.dispatch == 1);
    /* A rate within the converter's tolerance of 48 kHz is written as it is, never dropped. */
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    analog_lead_in(opts, state, 960, 47999);
    dsd_engine_analog_call_block(opts, state, block, 960, 47999, 1);
    dsd_engine_analog_call_block(opts, state, block, 960, 47999, 0);
    recording_frames tolerated = {.dir = directory};
    assert(dsd_dir_list(directory, count_recording_frames, &tolerated) == 0);
    assert(tolerated.files == 1 && tolerated.frames == 960);
    /* A teardown that skips the engine's finalize still publishes the open recording, never leaving its TEMP_ file. */
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    dsd_state_ext_free_all(state);
    teardown_check teardown = {.dir = directory};
    assert(dsd_dir_list(directory, check_teardown_recording, &teardown) == 0);
    /* With no history row left it is published as an unnamed analog recording under the call's mode (ANALOG on this
       audio input), not under the digital name. */
    assert(teardown.files == 1 && teardown.temp == 0 && teardown.unnamed == 1);
    assert(dsd_test_rmdir(directory) == 0);
}

typedef struct {
    const char* dir;
    int files;
    int traveler;
    int dispatch;
} tone_sync_recordings;

static int
check_tone_sync_recording(const char* name, void* user) {
    tone_sync_recordings* found = user;
    assert(!strstr(name, "TEMP"));
    found->traveler += dsd_test_analog_recording_name_is(name, "ANALOG", "Traveler");
    found->dispatch += dsd_test_analog_recording_name_is(name, "ANALOG", "Dispatch");
    found->files++;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, found->dir, name) == 0);
    assert(remove(path) == 0);
    return 0;
}

/* A tone change the decoder's own sync of the slot sees first (the frame sync's partial tone tap runs ahead of the next
   output block) is the analog call's lifecycle boundary, not the call state's: the recording still closes against the
   call's own row, named and as analog, never against the row the sync had already committed and cleared. */
static void
test_analog_tone_change_seen_first_by_a_sync(dsd_opts* opts, dsd_state* state) {
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->audio_out = 0;
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    char directory[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(directory, sizeof directory, "dsdneo_receive_tone_sync"));
    DSD_SNPRINTF(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", directory);
    opts->dmr_stereo_wav = 1;
    assert(dsd_receive_names_parse("--tone-name '100.0=Traveler' --tone-name '141.3=Dispatch'", &opts->receive.names,
                                   NULL, 0)
           == 0);
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1000;
    float block[960];
    for (int i = 0; i < 960; ++i) {
        block[i] = 2000.0f;
    }
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    state->analog_rx.ctcss_tenths_hz = 1413;
    dsd_event_sync_slot(opts, state, 0);
    /* The new tone's reception, as long as a call must be (issue #667). */
    for (int i = 0; i < DSD_ANALOG_CALL_MIN_MS / 20; i++) {
        dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    }
    assert(dsd_engine_analog_call_active(state));
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));
    tone_sync_recordings found = {.dir = directory};
    assert(dsd_dir_list(directory, check_tone_sync_recording, &found) == 0);
    assert(found.files == 2 && found.traveler == 1 && found.dispatch == 1);
    assert(dsd_test_rmdir(directory) == 0);
}

/* Issue #670: the NFM row names the frequency as the digital rows do, in MHz with six decimals, and leaves it out when
   there is none (an audio input: the tuned frequency is 0), in the history and in the -J event log alike. The audio
   input's row names no mode either (issue #667). */
static void
test_analog_event_frequency(dsd_opts* opts, dsd_state* state) {
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->audio_out = 0;
    opts->dmr_stereo_wav = 0;
    opts->wav_out_dir[0] = '\0';
    char directory[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(directory, sizeof directory, "dsdneo_receive_nfm_freq"));
    char log_path[DSD_TEST_PATH_MAX];
    assert(dsd_test_path_join(log_path, sizeof log_path, directory, "events.log") == 0);
    DSD_SNPRINTF(opts->event_out_file, sizeof opts->event_out_file, "%s", log_path);
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    float block[960];
    for (int i = 0; i < 960; ++i) {
        block[i] = 2000.0f;
    }
    const Event_History* row = &state->event_history_s[0].Event_History_Items[0];

    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
    opts->rtlsdr_center_freq = 162550000U; /* not the receiver's on an audio input */
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    assert(strstr(row->event_string, " ANALOG; ") != NULL && strstr(row->event_string, "NFM") == NULL);
    assert(strstr(row->event_string, "FREQ:") == NULL && strstr(row->event_string, "Frequency:") == NULL);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));

    opts->audio_in_type = AUDIO_IN_RTL;
    set_radio_squelch(opts);
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    assert(strstr(row->event_string, " ANALOG NFM; FREQ: 162.550000 MHz; ") != NULL);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));

    char log[4096];
    FILE* f = dsd_fopen_existing_regular_file(log_path, "rb");
    assert(f != NULL);
    const size_t n = fread(log, 1U, sizeof log - 1U, f);
    fclose(f);
    log[n] = '\0';
    assert(strstr(log, " ANALOG NFM; FREQ: 162.550000 MHz; ") != NULL);
    assert(strstr(log, "Frequency:") == NULL);
    assert(strstr(log, "FREQ: 0.") == NULL);
    /* Both calls are logged: the audio input's row with no frequency, then the radio's. */
    const char* first = strstr(log, " ANALOG; ");
    assert(first != NULL && strncmp(first + 9, "FREQ:", 5) != 0);
    assert(strstr(first + 9, " ANALOG NFM; FREQ: 162.550000 MHz; ") != NULL);
    opts->event_out_file[0] = '\0';
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->rtlsdr_center_freq = 0U;
    static const char* const files[] = {"events.log", NULL};
    assert(dsd_test_remove_temp_dir(directory, files) == 0);
}

/* The tone the analog tracker sees on its next block (issue #667): still acquiring (none confirmed), a confirmed untoned
   carrier, or a locked CTCSS tone or DCS code. */
typedef enum { ROW_TONE_ACQUIRING, ROW_TONE_NONE, ROW_TONE_CTCSS, ROW_TONE_DCS } row_tone;

static void
row_set_tone(dsd_state* state, row_tone tone, int value, int inverted) {
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    if (tone == ROW_TONE_ACQUIRING) {
        state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    } else if (tone == ROW_TONE_NONE) {
        state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    }
    state->analog_rx.tone_kind = tone == ROW_TONE_DCS ? DSD_ANALOG_TONE_KIND_DCS : DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = tone == ROW_TONE_CTCSS ? value : 0;
    state->analog_rx.dcs_code = tone == ROW_TONE_DCS ? value : 0;
    state->analog_rx.dcs_inverted = inverted;
}

/* One analog call through the tracker under @p tone, ended by a BREAK block: its committed row. A second tone, unless
   it equals the first, follows on a second block (a tone lost before the end). */
static const Event_History*
row_one_call(dsd_opts* opts, dsd_state* state, row_tone tone, int value, int inverted, row_tone then) {
    float block[960];
    for (int i = 0; i < 960; ++i) {
        block[i] = 2000.0f;
    }
    row_set_tone(state, tone, value, inverted);
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    if (then != tone) {
        row_set_tone(state, then, 0, 0);
        dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
        assert(dsd_engine_analog_call_active(state));
    }
    state->analog_rx.carrier_open = 0;
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));
    const Event_History* row = &state->event_history_s[0].Event_History_Items[1];
    assert(row->systype == DSD_SYNC_ANALOG);
    return row;
}

/* The row text after its "YYYY-MM-DD HH:MM:SS " stamp. */
static const char*
row_text(const Event_History* row) {
    assert(strlen(row->event_string) > 20U);
    return row->event_string + 20;
}

/* Issue #667 G1/G2: an analog row names its mode (NFM as the demodulator ran on a radio input, none on audio input), the
   frequency as before, and the CTCSS tone or DCS code its carrier was confirmed on, spelled as Call Info's Rx tone line
   spells it (both DCS spellings, the canonical one first). An untoned or never confirmed call names none, and a tone
   lost before the end leaves the confirmed one. The system string is the mode, ANALOG when unknown. */
static void
test_analog_row_text(dsd_opts* opts, dsd_state* state) {
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->audio_out = 0;
    opts->dmr_stereo_wav = 0;
    opts->wav_out_dir[0] = '\0';
    opts->wav_sample_rate = 48000;
    opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    opts->audio_in_type = AUDIO_IN_RTL;
    set_radio_squelch(opts);
    opts->rtlsdr_center_freq = 154430000U;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive); /* no service names: the row's own tokens only */
    assert(dsd_analog_tone_detection_active(opts));

    const Event_History* row = row_one_call(opts, state, ROW_TONE_CTCSS, 1000, 0, ROW_TONE_CTCSS);
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; CTCSS 100.0 Hz; ") == 0);
    assert(strcmp(row->sysid_string, "NFM") == 0 && row->analog_mode == DSD_ANALOG_MODE_NFM);
    row = row_one_call(opts, state, ROW_TONE_DCS, 023, 0, ROW_TONE_DCS);
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; DCS D023N / D047I; ") == 0);
    row = row_one_call(opts, state, ROW_TONE_DCS, 047, 1, ROW_TONE_DCS); /* the same signal, its other spelling */
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; DCS D023N / D047I; ") == 0);
    row = row_one_call(opts, state, ROW_TONE_DCS, 023, 1, ROW_TONE_DCS); /* inverted: the canonical normal code first */
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; DCS D047N / D023I; ") == 0);
    row = row_one_call(opts, state, ROW_TONE_NONE, 0, 0, ROW_TONE_NONE);
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; ") == 0);
    row = row_one_call(opts, state, ROW_TONE_ACQUIRING, 0, 0, ROW_TONE_ACQUIRING);
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; ") == 0);
    row = row_one_call(opts, state, ROW_TONE_CTCSS, 1413, 0, ROW_TONE_ACQUIRING); /* a reverse burst at the end */
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; CTCSS 141.3 Hz; ") == 0);

    /* Audio input: no mode word and no frequency, the tone still named. */
    opts->audio_in_type = AUDIO_IN_WAV;
    row = row_one_call(opts, state, ROW_TONE_CTCSS, 1000, 0, ROW_TONE_CTCSS);
    assert(strcmp(row_text(row), "ANALOG; CTCSS 100.0 Hz; ") == 0);
    assert(strcmp(row->sysid_string, "ANALOG") == 0 && row->analog_mode == DSD_ANALOG_MODE_UNKNOWN);
    row = row_one_call(opts, state, ROW_TONE_NONE, 0, 0, ROW_TONE_NONE);
    assert(strcmp(row_text(row), "ANALOG; ") == 0);
    opts->rtlsdr_center_freq = 0U;
    DSD_MEMSET(&state->analog_rx, 0, sizeof state->analog_rx);
}

/* Issue #667 D7: the canonical call takes its mode from the observation that begins it and keeps it, and the row and
   its system string follow it: AM (which the tracker admits only later) as NFM, unknown as ANALOG. */
static void
test_analog_mode_rows(dsd_opts* opts, dsd_state* state) {
    static const struct {
        uint8_t mode;
        int64_t freq_hz;
        const char* text;
        const char* sysid;
    } cases[] = {
        {DSD_ANALOG_MODE_AM, 119100000, "ANALOG AM; FREQ: 119.100000 MHz; ", "AM"},
        {DSD_ANALOG_MODE_NFM, 154430000, "ANALOG NFM; FREQ: 154.430000 MHz; ", "NFM"},
        {DSD_ANALOG_MODE_UNKNOWN, 0, "ANALOG; ", "ANALOG"},
    };

    DSD_MEMSET(&state->analog_rx, 0, sizeof state->analog_rx);
    double now_m = 100.0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_call_observation observation = {
            .protocol = DSD_SYNC_ANALOG,
            .slot = 0,
            .kind = DSD_CALL_KIND_VOICE,
            .frequency_hz = cases[i].freq_hz,
            .analog_mode = cases[i].mode,
            .observed_m = now_m,
        };
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        dsd_event_sync_slot(opts, state, 0);
        /* A later observation in another mode leaves the call's. */
        observation.analog_mode = cases[i].mode == DSD_ANALOG_MODE_NFM ? DSD_ANALOG_MODE_AM : DSD_ANALOG_MODE_NFM;
        observation.observed_m = now_m + 0.5;
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
        dsd_call_snapshot call;
        assert(dsd_call_state_get(state, 0, &call) > 0 && call.analog_mode == cases[i].mode);
        assert(dsd_call_state_end_ex(state, 0, now_m + 1.0, DSD_CALL_END_EXPLICIT) == 1);
        dsd_event_sync_slot(opts, state, 0);
        const Event_History* row = &state->event_history_s[0].Event_History_Items[1];
        assert(row->systype == DSD_SYNC_ANALOG && row->analog_mode == cases[i].mode);
        assert(strcmp(row_text(row), cases[i].text) == 0);
        assert(strcmp(row->sysid_string, cases[i].sysid) == 0);
        now_m += 10.0;
    }
}

/* An analog BEGIN on slot 0, 154.430 MHz in NFM, observed at @p observed_m and seeded with @p seed unless it is NULL. */
static dsd_call_observation
analog_begin(const dsd_receive_identity* seed, double observed_m) {
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_ANALOG,
        .slot = 0,
        .kind = DSD_CALL_KIND_VOICE,
        .frequency_hz = 154430000,
        .analog_mode = DSD_ANALOG_MODE_NFM,
        .observed_m = observed_m,
    };
    if (seed) {
        observation.has_receive_seed = 1;
        observation.receive_seed = *seed;
    }
    return observation;
}

/* Begin a call with @p observation, render it and end it @p length_s later: its committed row. */
static const Event_History*
analog_seeded_call(dsd_opts* opts, dsd_state* state, const dsd_call_observation* observation, double length_s) {
    assert(dsd_call_state_observe(state, observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(opts, state, 0);
    assert(dsd_call_state_end_ex(state, 0, observation->observed_m + length_s, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(opts, state, 0);
    const Event_History* row = &state->event_history_s[0].Event_History_Items[1];
    assert(row->systype == DSD_SYNC_ANALOG);
    return row;
}

static int
same_identity(const dsd_receive_identity* identity, int kind, int value) {
    return identity->known && identity->kind == kind && identity->value == value;
}

/* Issue #667: a BEGIN seeded with the identity the tracker confirmed is heard with it, whatever the receiver publishes
   as the call begins (nothing confirmed yet, a closed carrier, another tone), and is named by it, on the call and on its
   row. A confirmed untoned seed names no tone over a live one. Without a seed the live identity is the call's, as
   before, and a CONTINUE's seed is ignored. */
static void
test_analog_seeded_begin(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    assert(dsd_receive_names_parse("--tone-name '100.0=Traveler' --tone-name '141.3=Dispatch'", &opts->receive.names,
                                   NULL, 0)
           == 0);
    const dsd_receive_identity traveler = {.value = 1000, .kind = DSD_RECEIVE_CTCSS, .known = 1};
    const dsd_receive_identity untoned = {.kind = DSD_RECEIVE_NONE, .known = 1};

    static const struct {
        row_tone live;
        int carrier_open;
    } k_live[] = {{ROW_TONE_ACQUIRING, 1}, {ROW_TONE_CTCSS, 0}, {ROW_TONE_CTCSS, 1}};

    double now_m = 500.0;
    dsd_call_snapshot call;
    for (size_t i = 0; i < sizeof k_live / sizeof k_live[0]; i++) {
        row_set_tone(state, k_live[i].live, 1413, 0);
        state->analog_rx.carrier_open = k_live[i].carrier_open;
        const dsd_call_observation observation = analog_begin(&traveler, now_m);
        assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
        dsd_event_sync_slot(opts, state, 0);
        assert(dsd_call_state_get(state, 0, &call) > 0);
        assert(same_identity(&call.receive_identity, DSD_RECEIVE_CTCSS, 1000));
        assert(strcmp(call.service_label, "Traveler") == 0);
        assert(dsd_call_state_end_ex(state, 0, now_m + 1.0, DSD_CALL_END_EXPLICIT) == 1);
        dsd_event_sync_slot(opts, state, 0);
        const Event_History* row = &state->event_history_s[0].Event_History_Items[1];
        assert(strcmp(row_text(row), "[Traveler] ANALOG NFM; FREQ: 154.430000 MHz; CTCSS 100.0 Hz; ") == 0);
        assert(strcmp(row->service_label, "Traveler") == 0);
        assert(same_identity(&row->receive_identity, DSD_RECEIVE_CTCSS, 1000));
        now_m += 10.0;
    }

    /* Live CTCSS 141.3 Hz throughout: a confirmed untoned seed, then no seed. */
    row_set_tone(state, ROW_TONE_CTCSS, 1413, 0);
    dsd_call_observation observation = analog_begin(&untoned, now_m);
    const Event_History* row = analog_seeded_call(opts, state, &observation, 1.0);
    assert(strcmp(row_text(row), "ANALOG NFM; FREQ: 154.430000 MHz; ") == 0);
    assert(row->service_label[0] == '\0' && same_identity(&row->receive_identity, DSD_RECEIVE_NONE, 0));
    observation = analog_begin(NULL, now_m + 10.0);
    row = analog_seeded_call(opts, state, &observation, 1.0);
    assert(strcmp(row_text(row), "[Dispatch] ANALOG NFM; FREQ: 154.430000 MHz; CTCSS 141.3 Hz; ") == 0);

    /* A CONTINUE carrying a seed leaves the call's identity: only a BEGIN is seeded. */
    row_set_tone(state, ROW_TONE_ACQUIRING, 0, 0);
    observation = analog_begin(&traveler, now_m + 20.0);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    observation.receive_seed.value = 1413;
    observation.observed_m += 0.5;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE) == 0);
    assert(dsd_call_state_get(state, 0, &call) > 0 && same_identity(&call.receive_identity, DSD_RECEIVE_CTCSS, 1000));
    assert(dsd_call_state_end_ex(state, 0, now_m + 21.0, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(opts, state, 0);
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    DSD_MEMSET(&state->analog_rx, 0, sizeof state->analog_rx);
}

/* Whether @p row's text starts with the "YYYY-MM-DD HH:MM:SS " of @p t. */
static int
row_text_stamped(const Event_History* row, time_t t) {
    char date[16];
    char clock[16];
    char want[40];
    (void)dsd_format_local_datetime(t, DSD_LOCAL_DATETIME_DATE_HYPHEN, date, sizeof date);
    (void)dsd_format_local_datetime(t, DSD_LOCAL_DATETIME_TIME_COLON, clock, sizeof clock);
    DSD_SNPRINTF(want, sizeof want, "%s %s ", date, clock);
    return strncmp(row->event_string, want, strlen(want)) == 0;
}

static void
set_decode_wall(time_t t) {
    dsd_decode_clock_test_set_ns((uint64_t)t * 1000000000ULL);
}

/* Issue #667: an analog call begun with its first sample's wall anchor is stamped from it, fractional to the end. It
   starts in the anchor's second and ends in the second of the anchor plus its length, whenever it renders: one at
   x.9 s that runs 0.6 s ends in the next second, where start + (time_t)elapsed put it a second early. An ended analog
   row's text reads its end, not the render's clock (A2: a commit an input stall delayed), anchored or not. A digital
   row is stamped as before, an anchor given or not, and its text reads the render's clock. */
static void
test_analog_wall_anchored_stamps(dsd_opts* opts, dsd_state* state) {
    const time_t t0 = (time_t)1700000000;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    DSD_MEMSET(&state->analog_rx, 0, sizeof state->analog_rx);
    const Event_History* staged = &state->event_history_s[0].Event_History_Items[0];
    const Event_History* row = &state->event_history_s[0].Event_History_Items[1];

    /* Rendered 30 s late throughout: the stamps are the anchor's. */
    dsd_decode_clock_use_test((uint64_t)(t0 + 30) * 1000000000ULL);
    dsd_call_observation observation = analog_begin(NULL, 600.0);
    observation.start_wall = (double)t0 + 0.9;
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(opts, state, 0);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(state, 0, &call) > 0 && fabs(call.start_wall - ((double)t0 + 0.9)) < 1e-6);
    assert(staged->event_start_time == t0 && staged->event_time == t0);
    assert(dsd_call_state_end_ex(state, 0, 600.6, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(opts, state, 0);
    assert(row->event_start_time == t0 && row->event_time == t0 + 1);
    assert(row_text_stamped(row, t0 + 1));

    /* No anchor: the start from the first render as before; the ended row's text still reads its end. */
    set_decode_wall(t0 + 100);
    observation = analog_begin(NULL, 700.0);
    assert(dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(opts, state, 0);
    assert(staged->event_start_time == t0 + 100);
    set_decode_wall(t0 + 140);
    assert(dsd_call_state_end_ex(state, 0, 702.5, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(opts, state, 0);
    assert(row->event_start_time == t0 + 100 && row->event_time == t0 + 102);
    assert(row_text_stamped(row, t0 + 102));

    /* Digital: an anchor is not its stamps, and its text keeps the render's clock. */
    set_decode_wall(t0 + 200);
    const dsd_call_observation digital = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 100U,
        .policy_target_id = 100U,
        .start_wall = (double)t0 + 0.9,
        .observed_m = 800.0,
    };
    assert(dsd_call_state_observe(state, &digital, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(opts, state, 0);
    assert(staged->event_start_time == t0 + 200);
    set_decode_wall(t0 + 230);
    assert(dsd_call_state_end_ex(state, 0, 801.6, DSD_CALL_END_EXPLICIT) == 1);
    dsd_event_sync_slot(opts, state, 0);
    assert(row->systype == DSD_SYNC_P25P1_POS);
    assert(row->event_start_time == t0 + 200 && row->event_time == t0 + 201);
    assert(row_text_stamped(row, t0 + 230));
    dsd_decode_clock_use_system();
}

/* A trunking context remembers the access code verified on each voice channel and refuses a grant to one the policy in
   force rejects; a channel never verified, or one the policy admits again after an edit, is followed. */
static void
test_carrier_memory(dsd_opts* opts, dsd_state* state) {
    const dsd_receive_policy saved = opts->receive;
    const int saved_nac = state->nac;
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    set_filter(opts, DSD_RECEIVE_NAC, "", "293");
    dsd_receive_carriers carriers = {0};
    state->nac = 0;
    dsd_receive_carriers_note(&carriers, state, DSD_SYNC_P25P1_POS, 851012500);
    assert(!dsd_receive_carriers_rejects(&carriers, opts, 851012500)); /* Nothing verified yet. */
    state->nac = 0x293;
    dsd_receive_carriers_note(&carriers, state, DSD_SYNC_P25P1_POS, 851012500);
    assert(dsd_receive_carriers_rejects(&carriers, opts, 851012500));
    assert(!dsd_receive_carriers_rejects(&carriers, opts, 851037500));
    state->nac = 0x370; /* The newest verified code on the channel replaces the remembered one. */
    dsd_receive_carriers_note(&carriers, state, DSD_SYNC_P25P1_POS, 851012500);
    assert(!dsd_receive_carriers_rejects(&carriers, opts, 851012500));
    state->nac = 0x293;
    dsd_receive_carriers_note(&carriers, state, DSD_SYNC_P25P1_POS, 851012500);
    set_filter(opts, DSD_RECEIVE_NAC, "", "370"); /* Judged by the policy in force, not the one it was noted under. */
    assert(!dsd_receive_carriers_rejects(&carriers, opts, 851012500));
    set_filter(opts, DSD_RECEIVE_NAC, "", "293");
    for (long i = 1; i <= DSD_RECEIVE_CARRIER_MEMORY; i++) { /* A full memory reuses its oldest entry. */
        dsd_receive_carriers_note(&carriers, state, DSD_SYNC_P25P1_POS, 851012500 + i * 12500);
    }
    assert(!dsd_receive_carriers_rejects(&carriers, opts, 851012500));
    assert(dsd_receive_carriers_rejects(&carriers, opts, 851012500 + DSD_RECEIVE_CARRIER_MEMORY * 12500));
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    assert(!dsd_receive_carriers_rejects(&carriers, opts, 851012500 + 12500));
    opts->receive = saved;
    state->nac = saved_nac;
}

/* --- Issue #756: an NFM call held through a pause up to the max gap. --- */

enum { HOLD_BLOCK = 960, HOLD_FILES = 4 };

static const uint64_t k_hold_t0_ns = 1700000000ULL * 1000000000ULL;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    Event_History_I* history;
    char dir[DSD_TEST_PATH_MAX];
    uint64_t now_ns;
    int rate;
} hold_rig;

static void
hold_open(hold_rig* r, const char* prefix, int max_gap_ms) {
    DSD_MEMSET(r, 0, sizeof(*r));
    r->opts = calloc(1, sizeof(*r->opts));
    r->state = calloc(1, sizeof(*r->state));
    r->history = calloc(2, sizeof(*r->history));
    assert(r->opts && r->state && r->history);
    r->state->event_history_s = r->history;
    init_event_history(&r->history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&r->history[1], 0, DSD_EVENT_HISTORY_LEN);
    r->opts->analog_only = 1;
    r->opts->monitor_input_audio = 1;
    r->opts->audio_out = 0;
    r->opts->audio_in_type = AUDIO_IN_WAV;
    r->opts->wav_sample_rate = 48000;
    r->opts->dmr_stereo_wav = 1;
    r->opts->analog_record_max_gap_ms = max_gap_ms;
    assert(dsd_test_mkdtemp(r->dir, sizeof r->dir, prefix));
    DSD_SNPRINTF(r->opts->wav_out_dir, sizeof r->opts->wav_out_dir, "%s", r->dir);
    r->state->analog_rx.carrier_open = 1;
    r->state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    r->rate = 48000;
    r->now_ns = k_hold_t0_ns;
    dsd_decode_clock_use_test(r->now_ns);
}

/* Every test ends its calls and takes its recordings: the folder must be empty here. */
static void
hold_close(hold_rig* r) {
    dsd_state_ext_free_all(r->state);
    assert(dsd_test_rmdir(r->dir) == 0);
    free(r->history);
    free(r->state);
    free(r->opts);
    dsd_decode_clock_use_system();
}

/* The rig's time in seconds, as the decode clock converts it. Fast-math may turn a division by 1e9 here into a
   multiplication by its rounded reciprocal, one ulp off the clock's exact division, so the clock does the conversion. */
static double
hold_now_m(const hold_rig* r) {
    const uint64_t live_ns = dsd_decode_now_mono_ns();
    dsd_decode_clock_test_set_ns(r->now_ns);
    const double now_m = dsd_decode_now_mono_s();
    dsd_decode_clock_test_set_ns(live_ns);
    return now_m;
}

static void
hold_advance_ms(hold_rig* r, uint64_t ms) {
    r->now_ns += ms * 1000000ULL;
    dsd_decode_clock_test_set_ns(r->now_ns);
}

/* One monitor block of @p value as the symbol path hands it over: every sink's block begins (a withheld one's too, so
   the call WAV lane starts over at the next block written), the block's span passes on the decode clock, and the call
   sees the block with @p input. */
static void
hold_feed(hold_rig* r, int input, int value) {
    static float block[HOLD_BLOCK];
    for (int i = 0; i < HOLD_BLOCK; i++) {
        block[i] = (float)value;
    }
    dsd_analog_sink_block_begin(r->state);
    r->now_ns += (uint64_t)HOLD_BLOCK * 1000000000ULL / (uint64_t)r->rate;
    dsd_decode_clock_test_set_ns(r->now_ns);
    dsd_engine_analog_call_block(r->opts, r->state, block, HOLD_BLOCK, r->rate, input);
}

static void
hold_receive(hold_rig* r, int blocks, int value) {
    r->state->analog_rx.carrier_open = 1;
    for (int i = 0; i < blocks; i++) {
        hold_feed(r, DSD_ANALOG_CALL_RECEPTION, value);
    }
}

/* Blocks after the carrier's hangover ran out. */
static void
hold_closed(hold_rig* r, int blocks) {
    r->state->analog_rx.carrier_open = 0;
    for (int i = 0; i < blocks; i++) {
        hold_feed(r, DSD_ANALOG_CALL_HOLDABLE, 0);
    }
}

/* Blocks of a carrier whose tone is still being checked: PENDING, which holds a call like any HOLDABLE block. */
static void
hold_pending(hold_rig* r, int blocks) {
    r->state->analog_rx.carrier_open = 1;
    for (int i = 0; i < blocks; i++) {
        hold_feed(r, DSD_ANALOG_CALL_PENDING, 0);
    }
}

/* How many lead-in blocks bring a reception to DSD_ANALOG_CALL_MIN_MS at its first RECEPTION block, at the rig's rate:
   the tracker counts the spans in 48 kHz samples with the remainder carried, so the total's whole samples. */
static int
hold_lead_in_blocks(const hold_rig* r) {
    const uint64_t need = (uint64_t)DSD_ANALOG_CALL_MIN_MS * 48U;
    int blocks = 0;
    while ((uint64_t)(blocks + 1) * HOLD_BLOCK * 48000U / (uint64_t)r->rate < need) {
        blocks++;
    }
    return blocks;
}

/* Where a reception whose first block is the one just fed starts, computed as the tracker computes it: that block's
   stamp less its span at the block rate (issue #667), so the two agree to the last bit. */
static double
hold_block_start_m(const hold_rig* r) {
    return hold_now_m(r) - (double)HOLD_BLOCK / (double)r->rate;
}

/* A new call (issue #667): the tone check before it (a lead-in of PENDING blocks, recorded nowhere) and @p blocks
   RECEPTION blocks of @p value, the first of which makes it a call. Returns where it starts: the lead-in's first sample.
   No call may be running, or one whose hold ends at the lead-in's first block, which then begins the lead-in. */
static double
hold_begin(hold_rig* r, int blocks, int value) {
    const int lead_in = hold_lead_in_blocks(r);
    assert(lead_in > 0);
    hold_pending(r, 1);
    const double start_m = hold_block_start_m(r);
    hold_pending(r, lead_in - 1);
    assert(!dsd_engine_analog_call_active(r->state));
    hold_receive(r, 1, value);
    assert(dsd_engine_analog_call_active(r->state));
    hold_receive(r, blocks - 1, value);
    return start_m;
}

/* The slot's last call ended, and its row spans @p started_m to @p ended_m on the decode clock. A start is its first
   block's stamp less the block's span (issue #667), within an ulp of the rig's own conversion. */
static void
hold_expect_span(const hold_rig* r, double started_m, double ended_m) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    assert(dsd_call_state_get(r->state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ENDED);
    assert(fabs(call.started_m - started_m) < 1e-6);
    assert(fabs(call.ended_m - ended_m) < 1e-6);
}

typedef struct {
    const char* dir;
    int files;
    int temp;
    int named;
    long frames[HOLD_FILES];
    short* samples[HOLD_FILES];
} hold_files;

static int
take_hold_file(const char* name, void* user) {
    hold_files* found = user;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, found->dir, name) == 0);
    found->temp += strstr(name, "TEMP") != NULL;
    found->named += dsd_test_analog_recording_name_is(name, "ANALOG", NULL);
    assert(found->files < HOLD_FILES);
    SF_INFO info = {0};
    SNDFILE* wav = sf_open(path, SFM_READ, &info);
    assert(wav && info.samplerate == 48000 && info.channels == 1);
    const int i = found->files++;
    found->frames[i] = (long)info.frames;
    found->samples[i] = calloc((size_t)info.frames + 1U, sizeof(short));
    assert(found->samples[i]);
    assert(sf_read_short(wav, found->samples[i], info.frames) == info.frames);
    assert(sf_close(wav) == 0);
    assert(remove(path) == 0);
    return 0;
}

/* Read and remove every recording in the rig's folder: each must be published, under the analog name (ANALOG on the
   rig's audio input), never left TEMP_. */
static void
hold_take(const hold_rig* r, hold_files* found, int files) {
    DSD_MEMSET(found, 0, sizeof(*found));
    found->dir = r->dir;
    assert(dsd_dir_list(r->dir, take_hold_file, found) == 0);
    assert(found->files == files && found->temp == 0 && found->named == files);
}

static void
hold_release(hold_files* found) {
    for (int i = 0; i < found->files; i++) {
        free(found->samples[i]);
    }
}

/* The recording whose first sample is @p value (each burst of a test writes its own value). */
static int
hold_file_starting(const hold_files* found, short value) {
    int index = -1;
    for (int i = 0; i < found->files; i++) {
        if (found->frames[i] > 0 && found->samples[i][0] == value) {
            assert(index < 0);
            index = i;
        }
    }
    assert(index >= 0);
    return index;
}

/* @p count samples of @p samples from @p from are all @p value. */
static int
hold_run_is(const short* samples, long from, long count, short value) {
    for (long i = from; i < from + count; i++) {
        if (samples[i] != value) {
            return 0;
        }
    }
    return 1;
}

/* One recording of @p count blocks of @p value, and nothing else (no silence for a pause the call did not span). */
static void
hold_expect_one_burst(const hold_rig* r, int blocks, short value) {
    hold_files found;
    hold_take(r, &found, 1);
    assert(found.frames[0] == (long)blocks * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 0, found.frames[0], value));
    hold_release(&found);
}

/* B1: an NFM call's row ends at its last reception, whatever ends the call later: an explicit end after the input
   stalled (stdin and TCP block in the read, so the end comes only when data returns), a tick, the block after the
   hangover with the hold off, and an end while held. Main ended the row when the end ran ("now"). */
static void
test_hold_row_ends_at_the_last_reception(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_b1", 0);
    double first_m = hold_begin(&r, 3, 1000);
    double last_m = hold_now_m(&r);
    hold_advance_ms(&r, 10000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 3, 1000);

    /* The tick: the carrier closed with no block since (hold off), then the input stopped past its deadline. */
    first_m = hold_begin(&r, 2, 1000);
    last_m = hold_now_m(&r);
    r.state->analog_rx.carrier_open = 0;
    hold_advance_ms(&r, 5000);
    dsd_engine_analog_call_tick(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 2, 1000);
    first_m = hold_begin(&r, 2, 1000);
    last_m = hold_now_m(&r);
    r.state->analog_rx.stale_after_ms = 1U;
    hold_advance_ms(&r, 5000);
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, first_m, last_m);
    r.state->analog_rx.stale_after_ms = 0U;
    hold_expect_one_burst(&r, 2, 1000);

    /* The hangover's end with the hold off: one block earlier than main's "now". */
    first_m = hold_begin(&r, 2, 1000);
    last_m = hold_now_m(&r);
    hold_closed(&r, 1);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 2, 1000);

    /* Held, then ended: the hold is not the call's. */
    r.opts->analog_record_max_gap_ms = 1000;
    first_m = hold_begin(&r, 2, 1000);
    last_m = hold_now_m(&r);
    hold_closed(&r, 5);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 2, 1000);
    hold_close(&r);
}

/* A recording write that takes decode time, as a slow disk does under a live clock: the clock moves 7 ms on. */
static long long
hold_slow_write(SNDFILE* file, const short* samples, long long count) {
    dsd_decode_clock_test_set_ns(dsd_decode_now_mono_ns() + 7000000ULL);
    return (long long)sf_write_short(file, samples, (sf_count_t)count);
}

/* The call's stamps are exactly the ones given: a start as hold_block_start_m() computes it, the others the blocks'. */
static void
hold_expect_stamps(const hold_rig* r, int phase, double started_m, double last_m) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    assert(dsd_call_state_get(r->state, 0U, &call) > 0 && (int)call.phase == phase);
    assert(fabs(call.started_m - started_m) < 1e-9);
    assert(fabs(call.media_updated_m - last_m) < 1e-9);
    assert(phase != DSD_CALL_PHASE_ENDED || fabs(call.ended_m - last_m) < 1e-9);
}

/* The call's stamps are the block's, read once per block before any of its file work: the time a recording write takes
   is not the call's. The end read the clock again after the block's write, and a resumed block's stamp was read after
   the pause's silence was written. */
static void
test_hold_row_end_is_the_block_stamp(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_stamp", 1000);
    dsd_wav_test_set_io(hold_slow_write, NULL);
    const double first_m = hold_begin(&r, 2, 1000);
    double last_m = hold_now_m(&r);
    hold_closed(&r, 3);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, first_m, last_m);
    hold_expect_one_burst(&r, 2, 1000);

    /* A resume as the call's last reception: three held blocks of silence are written before its own audio. */
    const double resumed_first_m = hold_begin(&r, 2, 1000);
    hold_closed(&r, 3);
    hold_receive(&r, 1, 2000);
    last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, resumed_first_m, last_m);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 6L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 0, 2L * HOLD_BLOCK, 1000));
    assert(hold_run_is(found.samples[0], 2L * HOLD_BLOCK, 3L * HOLD_BLOCK, 0));
    assert(hold_run_is(found.samples[0], 5L * HOLD_BLOCK, HOLD_BLOCK, 2000));
    hold_release(&found);
    dsd_wav_test_set_io(NULL, NULL);
    hold_close(&r);
}

/* The hold is a recording setting: it holds a call only while per-call recording is on. With recording off, a closed
   carrier ends the call at its last reception on the next block or tick, as with the hold off. Recording stopped while
   a call is held publishes the recording, and the next block or tick ends the call there, with no silence; a start
   after that begins a new call. */
static void
test_hold_needs_recording(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_unrecorded", 1000);
    r.opts->dmr_stereo_wav = 0;
    for (int tick = 0; tick < 2; tick++) {
        const double first_m = hold_begin(&r, 2, 1000);
        const double last_m = hold_now_m(&r);
        if (tick) {
            r.state->analog_rx.carrier_open = 0;
            dsd_engine_analog_call_tick(r.opts, r.state);
        } else {
            hold_closed(&r, 1);
        }
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_span(&r, first_m, last_m);
    }
    hold_files found;
    hold_take(&r, &found, 0);

    for (int tick = 0; tick < 2; tick++) {
        r.opts->dmr_stereo_wav = 1;
        const double first_m = hold_begin(&r, 2, 1000);
        const double last_m = hold_now_m(&r);
        hold_closed(&r, 3);
        r.opts->dmr_stereo_wav = 0;
        dsd_engine_analog_call_stop_recording(r.opts, r.state);
        assert(dsd_engine_analog_call_active(r.state));
        hold_expect_one_burst(&r, 2, 1000);
        if (tick) {
            dsd_engine_analog_call_tick(r.opts, r.state);
        } else {
            hold_closed(&r, 1);
        }
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_span(&r, first_m, last_m);
    }
    r.opts->dmr_stereo_wav = 1;
    const double first_m = hold_begin(&r, 2, 2000);
    const double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 2, 2000);
    hold_close(&r);
}

/* B6: the receiver crossed a hard boundary (a stream pause, a reconnect, a retune) since the call began, and the block
   that carries the moved epoch holds the old reception's end and the new one's start: the call ends at its last
   reception, that block is not written, and the next block begins a new call. Main wrote both receptions into one. */
static void
test_hold_boundary_move_ends_the_call(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_b6", 0);
    double first_m = hold_begin(&r, 1, 1000);
    double last_m = hold_now_m(&r);
    r.state->analog_rx.boundaries++;
    hold_receive(&r, 1, 2000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, first_m, last_m);
    (void)hold_begin(&r, 1, 3000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 2);
    assert(found.frames[hold_file_starting(&found, 1000)] == HOLD_BLOCK);
    assert(found.frames[hold_file_starting(&found, 3000)] == HOLD_BLOCK);
    hold_release(&found);

    /* Held: the move ends the held call without its pause, on a reception or on a held block. */
    r.opts->analog_record_max_gap_ms = 1000;
    first_m = hold_begin(&r, 1, 4000);
    last_m = hold_now_m(&r);
    hold_closed(&r, 3);
    r.state->analog_rx.boundaries++;
    hold_receive(&r, 1, 5000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, first_m, last_m);
    (void)hold_begin(&r, 1, 6000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_take(&r, &found, 2);
    assert(found.frames[hold_file_starting(&found, 4000)] == HOLD_BLOCK);
    assert(found.frames[hold_file_starting(&found, 6000)] == HOLD_BLOCK);
    hold_release(&found);
    (void)hold_begin(&r, 1, 7000);
    hold_closed(&r, 2);
    r.state->analog_rx.boundaries++;
    hold_closed(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 1, 7000);
    hold_close(&r);
}

/* A held call is one call: one row from its start (its tone check, issue #667) to its last reception, committed once
   (one VOICE_END: the commit of a call that ended, never of one held), and one recording with the pause as silence
   where it was. A pause of exactly the max gap is joined (the 200 ms hangover the blocks before it recorded, then
   300 ms); one block more ends the call at that block, with nothing written for the pause. */
static void
test_hold_joins_a_pause_up_to_the_max_gap(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_48k", 500);
    const uint64_t commits = r.history[0].push_seq;
    const double first_m = hold_begin(&r, 5, 1000);
    hold_closed(&r, 15);
    assert(dsd_engine_analog_call_active(r.state));
    assert(r.history[0].push_seq == commits);
    hold_receive(&r, 5, 2000);
    const double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    /* The 480 ms tone check, then 5 + 15 + 5 blocks. */
    assert(fabs((last_m - first_m) - (0.480 + 0.500)) < 1e-6);
    assert(r.history[0].push_seq == commits + 1U);
    assert(strcmp(r.history[0].Event_History_Items[1].sysid_string, "ANALOG") == 0); /* audio input: no mode */
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 25L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 0, 5L * HOLD_BLOCK, 1000));
    assert(hold_run_is(found.samples[0], 5L * HOLD_BLOCK, 15L * HOLD_BLOCK, 0));
    assert(hold_run_is(found.samples[0], 20L * HOLD_BLOCK, 5L * HOLD_BLOCK, 2000));
    hold_release(&found);

    const double over_first_m = hold_begin(&r, 5, 3000);
    const double over_m = hold_now_m(&r);
    hold_closed(&r, 15);
    assert(dsd_engine_analog_call_active(r.state));
    hold_closed(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, over_first_m, over_m);
    (void)hold_begin(&r, 5, 4000);
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(r.history[0].push_seq == commits + 3U);
    hold_take(&r, &found, 2);
    assert(found.frames[hold_file_starting(&found, 3000)] == 5L * HOLD_BLOCK);
    assert(found.frames[hold_file_starting(&found, 4000)] == 5L * HOLD_BLOCK);
    hold_release(&found);
    hold_close(&r);
}

/* The frames a call of @p blocks blocks at the rig's rate writes from a fresh start (the converter's own output). */
static long
hold_burst_frames(hold_rig* r, int blocks) {
    (void)hold_begin(r, blocks, 2000);
    dsd_engine_analog_call_end(r->opts, r->state);
    hold_files found;
    hold_take(r, &found, 1);
    const long frames = found.frames[0];
    hold_release(&found);
    assert(frames > 0);
    return frames;
}

/* A block rate other than 48 kHz (staged PCM rates, RTL output): the gap is counted at 48 kHz with the block rate's
   remainder carried, and the owed silence lands between the converter's outputs for the two bursts. @p joined held
   blocks are joined, as one row from the call's start (its tone check) to the last reception, and one more ends the
   call at its last reception, a row of its own. */
static void
hold_check_rate(int rate, int max_gap_ms, int joined, long owed) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_rate", max_gap_ms);
    r.rate = rate;
    const double block_s = (double)HOLD_BLOCK / (double)rate;
    const long burst = hold_burst_frames(&r, 3);
    const uint64_t commits = r.history[0].push_seq;
    double first_m = hold_begin(&r, 3, 2000);
    hold_closed(&r, joined);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 3, 2000);
    double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(r.history[0].push_seq == commits + 1U);
    hold_expect_span(&r, first_m, last_m);
    /* The tone check, then 3 + joined + 3 blocks. */
    assert(fabs((last_m - first_m) - (double)(hold_lead_in_blocks(&r) + 6 + joined) * block_s) < 1e-6);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 2 * burst + owed);
    assert(found.samples[0][burst - 1] != 0 && found.samples[0][found.frames[0] - 1] != 0);
    assert(hold_run_is(found.samples[0], burst, owed, 0));
    hold_release(&found);

    first_m = hold_begin(&r, 3, 2000);
    last_m = hold_now_m(&r);
    hold_closed(&r, joined + 1);
    assert(!dsd_engine_analog_call_active(r.state));
    assert(r.history[0].push_seq == commits + 2U);
    hold_expect_span(&r, first_m, last_m);
    (void)hold_begin(&r, 3, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(r.history[0].push_seq == commits + 3U);
    hold_take(&r, &found, 2);
    assert(found.frames[0] == burst && found.frames[1] == burst);
    hold_release(&found);
    hold_close(&r);
}

static void
test_hold_counts_the_gap_at_the_block_rate(void) {
    /* 960 samples at 8192 Hz are 117.1875 ms, 5625 samples at 48 kHz: 200 + 6 x 117.2 = 903 ms joins under 1000 ms,
       a seventh block (1020 ms) ends the call. */
    hold_check_rate(8192, 1000, 6, 6L * 5625L);
    /* At 44100 Hz a block is 1044.9 samples at 48 kHz: 200 ms + 4 blocks (287 ms) joins under 300 ms, a fifth (309 ms)
       ends. Four blocks owe 4179 samples, the carried remainder's one more than 4 x 1044. */
    hold_check_rate(44100, 300, 4, 4179L);
}

/* D5: a tone check after the pause (the carrier back, its tone not yet admitted) ages the gap like the closed carrier,
   so a carrier that keeps returning under a tone filter never holds the call past the max gap. A hold that begins
   with a tone check under a continuing carrier gets no hangover credit, nor does the carrier closing during it. */
static void
test_hold_tone_checks_age_the_gap(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_pending", 500);
    (void)hold_begin(&r, 5, 1000);
    for (int i = 0; i < 3; i++) {
        hold_closed(&r, 3);
        hold_pending(&r, 2);
    }
    assert(dsd_engine_analog_call_active(r.state));
    hold_pending(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 5, 1000);

    (void)hold_begin(&r, 5, 1000);
    hold_pending(&r, 25);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 5, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 35L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 5L * HOLD_BLOCK, 25L * HOLD_BLOCK, 0));
    hold_release(&found);
    (void)hold_begin(&r, 5, 1000);
    hold_pending(&r, 26);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 5, 1000);

    (void)hold_begin(&r, 5, 1000);
    hold_pending(&r, 5);
    hold_closed(&r, 20);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 1, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 31L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 5L * HOLD_BLOCK, 25L * HOLD_BLOCK, 0));
    hold_release(&found);
    (void)hold_begin(&r, 5, 1000);
    hold_pending(&r, 5);
    hold_closed(&r, 21);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 5, 1000);
    hold_close(&r);
}

/* Lock @p tenths_hz CTCSS, or DCS 047 normal (named "Dispatch") when 0. */
static void
hold_lock_tone(hold_rig* r, int tenths_hz) {
    r->state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    r->state->analog_rx.tone_kind = tenths_hz ? DSD_ANALOG_TONE_KIND_CTCSS : DSD_ANALOG_TONE_KIND_DCS;
    r->state->analog_rx.ctcss_tenths_hz = tenths_hz;
    r->state->analog_rx.dcs_code = 047;
    r->state->analog_rx.dcs_inverted = 0;
}

typedef struct {
    const char* dir;
    int traveler;
    int dispatch;
    long frames[2];
} hold_services;

static int
take_hold_service(const char* name, void* user) {
    hold_services* found = user;
    const int dispatch = dsd_test_analog_recording_name_is(name, "ANALOG", "Dispatch");
    assert(dispatch || dsd_test_analog_recording_name_is(name, "ANALOG", "Traveler"));
    found->traveler += !dispatch;
    found->dispatch += dispatch;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, found->dir, name) == 0);
    SF_INFO info = {0};
    SNDFILE* wav = sf_open(path, SFM_READ, &info);
    assert(wav && sf_close(wav) == 0);
    found->frames[dispatch] = (long)info.frames;
    assert(remove(path) == 0);
    return 0;
}

/* D4: a resumed carrier continues the held call unless its identity is confirmed different (known against known, the
   rule inside one carrier): the same tone or one still being acquired goes on, with the pause written; a different
   one ends the held call at its last reception, without the pause, and begins a new reception with its first block,
   a call once it ran the minimum (issue #667). */
static void
test_hold_identity_on_resume(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_identity", 1000);
    assert(dsd_receive_names_parse("--tone-name '141.3=Traveler' --tone-name 'D023I=Dispatch'", &r.opts->receive.names,
                                   NULL, 0)
           == 0);
    hold_lock_tone(&r, 1413);
    for (int acquiring = 0; acquiring < 2; acquiring++) {
        (void)hold_begin(&r, 2, 1000);
        hold_closed(&r, 5);
        r.state->analog_rx.tone_state = acquiring ? DSD_ANALOG_TONE_STATE_ACQUIRING : DSD_ANALOG_TONE_STATE_LOCKED;
        hold_receive(&r, 2, 2000);
        hold_lock_tone(&r, 1413);
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_services one = {.dir = r.dir};
        assert(dsd_dir_list(r.dir, take_hold_service, &one) == 0);
        assert(one.traveler == 1 && one.dispatch == 0 && one.frames[0] == 9L * HOLD_BLOCK);
    }
    const double traveler_first_m = hold_begin(&r, 2, 1000);
    const double traveler_last_m = hold_now_m(&r);
    const uint64_t commits = r.history[0].push_seq;
    hold_closed(&r, 5);
    hold_advance_ms(&r, 5000); /* No clock decides the hold, and the row ends at the last reception. */
    hold_lock_tone(&r, 0);
    hold_receive(&r, 2, 2000);
    /* The held call ended as the new code came; the new reception is not a call until it ran the minimum. */
    assert(!dsd_engine_analog_call_active(r.state) && r.history[0].push_seq == commits + 1U);
    hold_receive(&r, DSD_ANALOG_CALL_MIN_MS / 20 - 2, 2000);
    assert(dsd_engine_analog_call_active(r.state) && r.history[0].push_seq == commits + 1U);
    const Event_History* held = &r.history[0].Event_History_Items[1];
    assert(!strcmp(held->service_label, "Traveler"));
    assert(held->event_start_time == (time_t)floor(traveler_first_m)
           && held->event_time == (time_t)floor(traveler_last_m));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_services two = {.dir = r.dir};
    assert(dsd_dir_list(r.dir, take_hold_service, &two) == 0);
    assert(two.traveler == 1 && two.dispatch == 1 && two.frames[0] == 2L * HOLD_BLOCK
           && two.frames[1] == (long)(DSD_ANALOG_CALL_MIN_MS / 20) * HOLD_BLOCK);
    hold_close(&r);
}

/* A recording close that takes decode time, as hold_slow_write() does. */
static int
hold_slow_close(SNDFILE* file) {
    dsd_decode_clock_test_set_ns(dsd_decode_now_mono_ns() + 7000000ULL);
    return sf_close(file);
}

/* D4 on a slow disk: a new identity on the resume ends the held call and closes its recording before the new reception
   begins with the block, whose start is still the block's first sample by its stamp. It was read after the close. */
static void
test_hold_new_identity_starts_at_the_block_stamp(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_identity_stamp", 1000);
    assert(dsd_receive_names_parse("--tone-name '141.3=Traveler' --tone-name 'D023I=Dispatch'", &r.opts->receive.names,
                                   NULL, 0)
           == 0);
    dsd_wav_test_set_io(hold_slow_write, hold_slow_close);
    hold_lock_tone(&r, 1413);
    (void)hold_begin(&r, 2, 1000);
    hold_closed(&r, 3);
    hold_lock_tone(&r, 0);
    const double first_m = hold_now_m(&r);
    hold_receive(&r, 1, 2000);
    /* The new code's reception runs the minimum to be a call (issue #667). */
    hold_receive(&r, DSD_ANALOG_CALL_MIN_MS / 20 - 1, 2000);
    const double last_m = hold_now_m(&r);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ACTIVE, first_m, last_m);
    dsd_engine_analog_call_end(r.opts, r.state);
    dsd_wav_test_set_io(NULL, NULL);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, first_m, last_m);
    hold_services two = {.dir = r.dir};
    assert(dsd_dir_list(r.dir, take_hold_service, &two) == 0);
    assert(two.traveler == 1 && two.dispatch == 1 && two.frames[0] == 2L * HOLD_BLOCK
           && two.frames[1] == (long)(DSD_ANALOG_CALL_MIN_MS / 20) * HOLD_BLOCK);
    hold_close(&r);
}

/* What ends a held call before the next reception begins one, in test_hold_ends_before_a_begin_keep_its_stamp(). */
enum {
    HOLD_END_LOWERED,    /* the max gap lowered below the gap so far: the block's reconciliation ends the call */
    HOLD_END_OFF,        /* the hold turned off: the same */
    HOLD_END_UNRECORDED, /* per-call recording off with its file still open: the reconciliation closes it */
    HOLD_END_STOPPED,    /* per-call recording stopped first, as -P off does: the file closes at the stop */
    HOLD_END_EPOCH,      /* the epoch moved: the block that carries it ends the call and is not written */
    HOLD_END_EXPIRED,    /* the pause ran past the max gap: a held block ends the call */
    HOLD_END_CASES,
};

static void
hold_end_before_a_reception(hold_rig* r, int end) {
    switch (end) {
        case HOLD_END_LOWERED: r->opts->analog_record_max_gap_ms = 399; break;
        case HOLD_END_OFF: r->opts->analog_record_max_gap_ms = 0; break;
        case HOLD_END_UNRECORDED: r->opts->dmr_stereo_wav = 0; break;
        case HOLD_END_STOPPED:
            r->opts->dmr_stereo_wav = 0;
            dsd_engine_analog_call_stop_recording(r->opts, r->state);
            break;
        case HOLD_END_EPOCH:
            r->state->analog_rx.boundaries++;
            hold_receive(r, 1, 2000);
            assert(!dsd_engine_analog_call_active(r->state));
            break;
        default:
            hold_closed(r, 31); /* 1020 ms */
            assert(!dsd_engine_analog_call_active(r->state));
            break;
    }
}

/* On a slow disk, every end of a held call that closes its recording before the next reception begins (in that block
   or before it) leaves the new call's stamps the blocks': the close's time is not the call's. The block's stamp was
   read after the reconciliation that opens the block closed the old recording. The next reception begins with its
   tone check (issue #667), whose first block now carries that reconciliation. */
static void
test_hold_ends_before_a_begin_keep_its_stamp(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_end_stamp", 1000);
    dsd_wav_test_set_io(hold_slow_write, hold_slow_close);
    for (int end = 0; end < HOLD_END_CASES; end++) {
        r.opts->dmr_stereo_wav = 1;
        r.opts->analog_record_max_gap_ms = 1000;
        (void)hold_begin(&r, 2, 1000);
        hold_closed(&r, 10); /* 400 ms */
        hold_end_before_a_reception(&r, end);
        const double start_m = hold_begin(&r, 1, 3000);
        const double block_m = hold_now_m(&r);
        hold_expect_stamps(&r, DSD_CALL_PHASE_ACTIVE, start_m, block_m);
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, start_m, block_m);
        const int recorded = end != HOLD_END_UNRECORDED && end != HOLD_END_STOPPED;
        hold_files found;
        hold_take(&r, &found, recorded ? 2 : 1);
        assert(found.frames[hold_file_starting(&found, 1000)] == 2L * HOLD_BLOCK);
        assert(!recorded || found.frames[hold_file_starting(&found, 3000)] == HOLD_BLOCK);
        hold_release(&found);
    }
    dsd_wav_test_set_io(NULL, NULL);
    hold_close(&r);
}

/* Every end but the hold's resume writes no silence: an explicit end (a scan leave, a retune, Stop: they all end the
   call through dsd_engine_analog_call_end()) and a BREAK (a REJECTED tone) end a held call at its last reception.
   With no call, an end commits nothing. */
static void
test_hold_ends_without_the_pause(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_end", 1000);
    const uint64_t commits = r.history[0].push_seq;
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(r.history[0].push_seq == commits);
    double first_m = hold_begin(&r, 2, 1000);
    double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 2, 1000);
    for (int rejected = 0; rejected < 2; rejected++) {
        first_m = hold_begin(&r, 2, 1000);
        last_m = hold_now_m(&r);
        hold_closed(&r, 5);
        if (rejected) {
            hold_feed(&r, DSD_ANALOG_CALL_BREAK, 0);
        } else {
            dsd_engine_analog_call_end(r.opts, r.state);
        }
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_span(&r, first_m, last_m);
        hold_expect_one_burst(&r, 2, 1000);
    }
    assert(r.history[0].push_seq == commits + 3U);
    hold_close(&r);
}

/* A call's start and its last reception's end. */
typedef struct {
    double first_m;
    double last_m;
} hold_span;

/* Begin a call of two blocks and hold it for three. */
static hold_span
hold_start_held(hold_rig* r) {
    hold_span span;
    span.first_m = hold_begin(r, 2, 1000);
    span.last_m = hold_now_m(r);
    hold_closed(r, 3);
    assert(dsd_engine_analog_call_active(r->state));
    return span;
}

static void
hold_expect_tick_ends(hold_rig* r, hold_span span) {
    dsd_engine_analog_call_tick(r->opts, r->state);
    assert(!dsd_engine_analog_call_active(r->state));
    hold_expect_span(r, span.first_m, span.last_m);
    hold_expect_one_burst(r, 2, 1000);
}

/* The tick, in order: what no hold survives ends a held call there (detection off, a REJECTED tone, a retune
   unresolved), the hold off ends a call on a closed carrier as on main, and the stale deadline (the input stopped)
   ends it held or not. A held call with input flowing needs no tick: the closed carrier waits for the next block. */
static void
test_hold_tick_precedence(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_tick", 1000);
    (void)hold_begin(&r, 2, 1000);
    r.state->analog_rx.carrier_open = 0;
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(dsd_engine_analog_call_active(r.state));
    hold_closed(&r, 3);
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_one_burst(&r, 2, 1000);

    hold_span span = hold_start_held(&r);
    r.opts->analog_only = 0;
    hold_expect_tick_ends(&r, span);
    r.opts->analog_only = 1;
    span = hold_start_held(&r);
    r.state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    hold_expect_tick_ends(&r, span);
    r.state->analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;
    span = hold_start_held(&r);
    (void)dsd_trunk_tuning_request_begin();
    hold_expect_tick_ends(&r, span);
    dsd_trunk_tuning_requests_reset();
    span = hold_start_held(&r);
    r.state->analog_rx.stale_after_ms = 1U;
    hold_expect_tick_ends(&r, span);
    r.state->analog_rx.stale_after_ms = 0U;
    /* Lowered below the gap so far (260 ms), the hold still on: the tick reconciles. Turned off, it ends as on main. */
    for (int off = 0; off < 2; off++) {
        r.opts->analog_record_max_gap_ms = 1000;
        span = hold_start_held(&r);
        r.opts->analog_record_max_gap_ms = off ? 200 : 259;
        hold_expect_tick_ends(&r, span);
    }
    hold_close(&r);
}

/* The receiver reset between blocks as dsd_analog_rx_reset() does it: the epoch moved, the carrier closed, and no
   stale deadline until the input delivers again. */
static void
hold_reset_receiver(hold_rig* r) {
    r->state->analog_rx.boundaries++;
    r->state->analog_rx.carrier_open = 0;
    r->state->analog_rx.stale_after_ms = 0U;
}

/* B6 on the tick: a reset with no block after it (a TCP reconnect that yields to a queued command, then a stream that
   delivers no whole block) ends the call, active or held, at its last reception, publishes its recording with no
   silence, and leaves no TEMP_ file or open row behind. Before, only a block saw the move, so with the hold on the
   call stayed open as long as no block came. The hold off ends it too. */
static void
test_hold_tick_ends_a_moved_epoch(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_tick_b6", 1000);
    for (int held = 0; held < 2; held++) {
        hold_span span;
        span.first_m = hold_begin(&r, 2, 1000);
        span.last_m = hold_now_m(&r);
        if (held) {
            hold_closed(&r, 3);
        }
        hold_reset_receiver(&r);
        /* The reset cleared the stale deadline, so the move is a wake of its own (issue #667 D12): a silent hunt read
           gives way to this tick with nothing queued. */
        assert(dsd_engine_analog_call_due(r.state));
        hold_advance_ms(&r, 5000);
        hold_expect_tick_ends(&r, span);
        assert(!dsd_engine_analog_call_due(r.state));
        dsd_engine_analog_call_tick(r.opts, r.state);
        assert(!dsd_engine_analog_call_active(r.state));
    }
    r.opts->analog_record_max_gap_ms = 0;
    hold_span span;
    span.first_m = hold_begin(&r, 2, 1000);
    span.last_m = hold_now_m(&r);
    hold_reset_receiver(&r);
    hold_expect_tick_ends(&r, span);
    hold_close(&r);
}

/* D10, ending: every value at or under the hangover is off (a closed carrier ends an active call at once, as on main);
   turning the hold off, or lowering it below the gap so far, ends a held call at the next block (here the next call's
   tone check, issue #667), which a reception then begins anew; the first held block may already be past it (200 ms
   hangover + 20 ms). */
static void
test_hold_live_changes_end(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_live_end", 1000);
    static const int off[] = {0, 1, 200};
    for (size_t i = 0; i < sizeof off / sizeof off[0]; i++) {
        r.opts->analog_record_max_gap_ms = off[i];
        const double first_m = hold_begin(&r, 2, 1000);
        const double last_m = hold_now_m(&r);
        hold_closed(&r, 1);
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_span(&r, first_m, last_m);
        hold_expect_one_burst(&r, 2, 1000);
    }
    for (int lowered = 0; lowered < 2; lowered++) {
        r.opts->analog_record_max_gap_ms = 1000;
        (void)hold_begin(&r, 2, 1000);
        hold_closed(&r, 10); /* 400 ms */
        r.opts->analog_record_max_gap_ms = lowered ? 399 : 0;
        (void)hold_begin(&r, 1, 2000);
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_files found;
        hold_take(&r, &found, 2);
        assert(found.frames[hold_file_starting(&found, 1000)] == 2L * HOLD_BLOCK);
        assert(found.frames[hold_file_starting(&found, 2000)] == HOLD_BLOCK);
        hold_release(&found);
    }
    r.opts->analog_record_max_gap_ms = 219;
    (void)hold_begin(&r, 2, 1000);
    hold_closed(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 2, 1000);
    r.opts->analog_record_max_gap_ms = 220;
    (void)hold_begin(&r, 2, 1000);
    hold_closed(&r, 1);
    assert(dsd_engine_analog_call_active(r.state));
    hold_closed(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 2, 1000);
    hold_close(&r);
}

/* D10, going on: lowered to exactly the gap so far still joins it, and raised while held extends the hold. */
static void
test_hold_live_changes_extend(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_live_extend", 1000);
    (void)hold_begin(&r, 2, 1000);
    hold_closed(&r, 10); /* 400 ms */
    r.opts->analog_record_max_gap_ms = 400;
    hold_receive(&r, 1, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 13L * HOLD_BLOCK && hold_run_is(found.samples[0], 2L * HOLD_BLOCK, 10L * HOLD_BLOCK, 0));
    hold_release(&found);
    r.opts->analog_record_max_gap_ms = 300;
    (void)hold_begin(&r, 2, 1000);
    hold_closed(&r, 5); /* 300 ms */
    r.opts->analog_record_max_gap_ms = 1000;
    hold_closed(&r, 15); /* 600 ms */
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 1, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 23L * HOLD_BLOCK && hold_run_is(found.samples[0], 2L * HOLD_BLOCK, 20L * HOLD_BLOCK, 0));
    hold_release(&found);
    hold_close(&r);
}

/* The default (200 ms: the hold off) writes what main wrote, block for block: a closed carrier, a tone check and a
   BREAK each end the call, and each reception's blocks land in a file of their own as they came. Each call begins
   with its own tone check (issue #667), which records nothing. */
static void
test_hold_off_keeps_main_payloads(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_golden", 200);
    const uint64_t commits = r.history[0].push_seq;
    (void)hold_begin(&r, 1, 100);
    hold_receive(&r, 1, 200);
    hold_receive(&r, 1, 300);
    hold_closed(&r, 1);
    (void)hold_begin(&r, 1, 400);
    hold_receive(&r, 1, 500);
    hold_pending(&r, 1);
    (void)hold_begin(&r, 1, 600);
    hold_feed(&r, DSD_ANALOG_CALL_BREAK, 0);
    (void)hold_begin(&r, 1, 700);
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(r.history[0].push_seq == commits + 4U);
    hold_files found;
    hold_take(&r, &found, 4);
    const short* first = found.samples[hold_file_starting(&found, 100)];
    assert(found.frames[hold_file_starting(&found, 100)] == 3L * HOLD_BLOCK);
    assert(hold_run_is(first, 0, HOLD_BLOCK, 100) && hold_run_is(first, HOLD_BLOCK, HOLD_BLOCK, 200)
           && hold_run_is(first, 2L * HOLD_BLOCK, HOLD_BLOCK, 300));
    const short* second = found.samples[hold_file_starting(&found, 400)];
    assert(found.frames[hold_file_starting(&found, 400)] == 2L * HOLD_BLOCK);
    assert(hold_run_is(second, 0, HOLD_BLOCK, 400) && hold_run_is(second, HOLD_BLOCK, HOLD_BLOCK, 500));
    assert(found.frames[hold_file_starting(&found, 600)] == HOLD_BLOCK);
    assert(found.frames[hold_file_starting(&found, 700)] == HOLD_BLOCK);
    hold_release(&found);
    hold_close(&r);
}

static int g_hold_writes;
static int g_hold_short_at = -1;
static int g_hold_write_warnings;
static int g_rdio_warnings;
static int g_rdio_failed;
static int g_calls_off_fm;
static int g_calls_off_am;
static int g_calls_back_on;

static long long
hold_seam_write(SNDFILE* file, const short* samples, long long count) {
    const int call = g_hold_writes++;
    return (long long)sf_write_short(file, samples, (sf_count_t)(call == g_hold_short_at ? count / 2 : count));
}

static void
count_log_warnings(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    /* The hold rig's audio input names its calls by no mode (issue #667). */
    if (level == LOG_LEVEL_WARN && text && strstr(text, "Analog call: wrote") != NULL) {
        g_hold_write_warnings++;
    }
    if (level == LOG_LEVEL_WARN && text && strstr(text, "Rdio export") != NULL) {
        g_rdio_warnings++;
        g_rdio_failed += strstr(text, "Rdio export failed") != NULL;
    }
    /* Issue #667 D2's notices: paused with the FM monitor's remedies, or AM's (no --tone-allow), and back on. */
    if (level == LOG_LEVEL_INFO && text && strstr(text, "Analog call history and recordings are paused") != NULL) {
        /* The condition as the rule has it: any level at or below the default pauses, not the default alone. */
        assert(strstr(text, "the squelch is off or at -110 dB or below") != NULL);
        assert(strstr(text, "--squelch <dB>") != NULL && strstr(text, "--squelch auto") != NULL);
        const int fm = strstr(text, "--tone-allow") != NULL;
        assert(fm == (strstr(text, "or noise") != NULL));
        g_calls_off_fm += fm;
        g_calls_off_am += !fm;
    }
    if (level == LOG_LEVEL_INFO && text && strstr(text, "Analog call history and recordings are back on") != NULL) {
        g_calls_back_on++;
    }
}

/* A silence write that falls short latches the recording failed, as any short write does: nothing more reaches it (the
   rest of the pause, the resumed block), and it is published with what was written. */
static void
test_hold_silence_write_failure_latches(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_latch", 1000);
    dsd_wav_recording_session_reset();
    dsd_wav_test_set_io(hold_seam_write, NULL);
    (void)hold_begin(&r, 2, 1000);
    const int writes = g_hold_writes;
    hold_closed(&r, 20);
    g_hold_short_at = g_hold_writes;
    hold_receive(&r, 2, 2000);
    assert(g_hold_writes == writes + 1 && g_hold_write_warnings == 1);
    dsd_engine_analog_call_end(r.opts, r.state);
    dsd_wav_test_set_io(NULL, NULL);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 2L * HOLD_BLOCK + HOLD_BLOCK / 2);
    assert(hold_run_is(found.samples[0], 2L * HOLD_BLOCK, HOLD_BLOCK / 2, 0));
    hold_release(&found);
    dsd_wav_recording_session_reset();
    hold_close(&r);
}

/* A teardown while held: the engine's finalize ends the call first (dsd_engine_analog_call_end()), which publishes it
   under its row without the pause; one that skipped finalize still publishes it (unnamed, never as TEMP_). */
static void
test_hold_teardown_while_held(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_teardown", 1000);
    (void)hold_begin(&r, 2, 1000);
    hold_closed(&r, 5);
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(!strcmp(r.history[0].Event_History_Items[1].sysid_string, "ANALOG")); /* audio input: no mode */
    hold_expect_one_burst(&r, 2, 1000);
    (void)hold_begin(&r, 2, 1000);
    hold_closed(&r, 5);
    dsd_state_ext_free_all(r.state);
    hold_expect_one_burst(&r, 2, 1000);
    hold_close(&r);
}

/* The largest silence one resume writes: a hold that is all tone check (no hangover credit) at the 10 s maximum. */
static void
test_hold_largest_silence_write(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_flush", 10000);
    (void)hold_begin(&r, 1, 1000);
    hold_pending(&r, 500);
    const uint64_t began_ns = dsd_realtime_mono_ns();
    hold_receive(&r, 1, 2000);
    const uint64_t took_ns = dsd_realtime_mono_ns() - began_ns;
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 502L * HOLD_BLOCK && hold_run_is(found.samples[0], HOLD_BLOCK, 500L * HOLD_BLOCK, 0));
    hold_release(&found);
    printf("CORE_RECEIVE: a 10 s pause written as silence in %.2f ms\n", (double)took_ns / 1e6);
    hold_close(&r);
}

/* --- Issue #667 D1: a reception becomes a call only once it ran DSD_ANALOG_CALL_MIN_MS. --- */

/* The recording journal (issue #708), in a folder of its own: the rig's folder holds only recordings. */
typedef struct {
    char dir[DSD_TEST_PATH_MAX];
    char path[DSD_TEST_PATH_MAX * 2];
} hold_journal;

static void
hold_journal_open(hold_journal* j) {
    assert(dsd_test_mkdtemp(j->dir, sizeof j->dir, "dsdneo_hold_journal"));
    assert(dsd_test_path_join(j->path, sizeof j->path, j->dir, "calls.jsonl") == 0);
    assert(dsd_wav_journal_open(j->path) == 0);
}

static void
hold_journal_close(const hold_journal* j) {
    dsd_wav_journal_close();
    (void)remove(j->path);
    assert(dsd_test_rmdir(j->dir) == 0);
}

/* The @p nth line of the journal of @p type ("publish", ...; any line when NULL) into @p line. Returns 0 when there is
   none. */
static int
hold_journal_line(const hold_journal* j, const char* type, int nth, char* line, size_t size) {
    static char text[65536];
    FILE* f = dsd_fopen_existing_regular_file(j->path, "rb");
    if (!f) {
        return 0;
    }
    const size_t n = fread(text, 1U, sizeof text - 1U, f);
    (void)fclose(f);
    text[n] = '\0';
    char want[64];
    DSD_SNPRINTF(want, sizeof want, "\"type\":\"%s\"", type ? type : "");
    for (char* at = text; *at != '\0';) {
        char* end = strchr(at, '\n');
        const size_t len = end ? (size_t)(end - at) : strlen(at);
        if (len > 0U && len < size) {
            DSD_MEMCPY(line, at, len);
            line[len] = '\0';
            if ((!type || strstr(line, want) != NULL) && nth-- == 0) {
                return 1;
            }
        }
        at += len + (end ? 1U : 0U);
    }
    return 0;
}

static int
hold_journal_count(const hold_journal* j, const char* type) {
    char line[4096];
    int count = 0;
    while (hold_journal_line(j, type, count, line, sizeof line)) {
        count++;
    }
    return count;
}

/* The integer @p key of the journal's @p nth publish line. */
static long long
hold_journal_publish_field(const hold_journal* j, int nth, const char* key) {
    char line[4096];
    assert(hold_journal_line(j, "publish", nth, line, sizeof line));
    char want[64];
    DSD_SNPRINTF(want, sizeof want, "\"%s\":", key);
    const char* at = strstr(line, want);
    assert(at != NULL);
    char* end = NULL;
    const long long value = strtoll(at + strlen(want), &end, 10);
    assert(end != at + strlen(want));
    return value;
}

/* Nothing of a reception that never became a call: no canonical call (so no row, live call or alert), no recording, no
   journal line, and no commit since @p commits. */
static void
hold_expect_nothing(const hold_rig* r, const hold_journal* j, uint64_t commits) {
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof call);
    assert(!dsd_engine_analog_call_active(r->state));
    assert(dsd_call_state_get(r->state, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
    assert(r->history[0].push_seq == commits);
    hold_files found;
    hold_take(r, &found, 0);
    assert(!j || hold_journal_count(j, NULL) == 0);
}

/* D1: a reception counts its RECEPTION blocks, the hangover's included (the symbol path hands those over as RECEPTION
   too), and becomes a call at the block that brings it to 500 ms, never before: 280 ms of signal and its 200 ms
   hangover leave nothing, 300 ms and the hangover make a call from its first sample, and so does more. The spans are
   counted at the block rate in 48 kHz samples: at 44100 Hz the 23rd 960-sample block reaches 24000, the 22nd does
   not. */
static void
test_candidate_qualifies_at_the_minimum(void) {
    hold_rig r;
    hold_journal j;
    hold_open(&r, "dsdneo_cand_min", 0);
    hold_journal_open(&j);
    const uint64_t commits = r.history[0].push_seq;
    hold_receive(&r, 24, 1000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_closed(&r, 1);
    hold_expect_nothing(&r, &j, commits);

    for (int extra = 0; extra < 2; extra++) {
        const double first_m = hold_now_m(&r);
        hold_receive(&r, 24, 1000);
        assert(!dsd_engine_analog_call_active(r.state));
        hold_receive(&r, 1 + extra, 1000);
        assert(dsd_engine_analog_call_active(r.state));
        const double last_m = hold_now_m(&r);
        hold_closed(&r, 1);
        hold_expect_span(&r, first_m, last_m);
        assert(r.history[0].push_seq == commits + 1U + (uint64_t)extra);
        hold_expect_one_burst(&r, 25 + extra, 1000);
        assert(hold_journal_count(&j, "publish") == 1 + extra);
        assert(hold_journal_publish_field(&j, extra, "duration_ms") == 500 + 20 * extra);
    }
    hold_journal_close(&j);
    hold_close(&r);

    hold_open(&r, "dsdneo_cand_min_44k", 0);
    r.rate = 44100;
    hold_receive(&r, 22, 1000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 1, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 1);
    hold_release(&found);
    hold_close(&r);
}

/* D1 through a tone check that returns after the reception began (the carrier up, its tone checked again): with the
   hold off it ends the reception, which leaves nothing, and the check that goes on is the next reception's lead-in.
   With the hold on, the check holds it as it would a call, its span counted toward the gap but not the minimum, and
   owed as silence; a check that ends rejected leaves nothing. Nothing is published before the reception is a call. */
static void
test_candidate_through_a_tone_check(void) {
    hold_rig r;
    hold_journal j;
    hold_open(&r, "dsdneo_cand_pending", 0);
    hold_journal_open(&j);
    const uint64_t commits = r.history[0].push_seq;
    hold_receive(&r, 10, 1000);
    hold_pending(&r, 1);
    const double lead_m = hold_block_start_m(&r);
    hold_expect_nothing(&r, &j, commits);
    /* The check that ended it is the lead-in's first block (5 + 20 = 25), as when the reconciliation ends it. */
    hold_pending(&r, 4);
    hold_receive(&r, 19, 2000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 1, 2000);
    assert(dsd_engine_analog_call_active(r.state));
    const double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, lead_m, last_m);
    hold_expect_one_burst(&r, 20, 2000);
    assert(hold_journal_count(&j, "publish") == 1);
    hold_journal_close(&j);
    hold_close(&r);

    hold_open(&r, "dsdneo_cand_pending_held", 1000);
    hold_journal_open(&j);
    const uint64_t held_commits = r.history[0].push_seq;
    const double first_m = hold_now_m(&r);
    hold_receive(&r, 10, 1000);
    hold_pending(&r, 5);
    hold_receive(&r, 14, 2000);
    assert(!dsd_engine_analog_call_active(r.state) && hold_journal_count(&j, NULL) == 0);
    assert(r.history[0].push_seq == held_commits);
    hold_receive(&r, 1, 2000);
    assert(dsd_engine_analog_call_active(r.state));
    const double held_last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, held_last_m);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 30L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 0, 10L * HOLD_BLOCK, 1000));
    assert(hold_run_is(found.samples[0], 10L * HOLD_BLOCK, 5L * HOLD_BLOCK, 0));
    assert(hold_run_is(found.samples[0], 15L * HOLD_BLOCK, 15L * HOLD_BLOCK, 2000));
    hold_release(&found);

    const uint64_t rejected_commits = r.history[0].push_seq;
    const int lines = hold_journal_count(&j, NULL);
    hold_receive(&r, 10, 1000);
    hold_pending(&r, 5);
    hold_feed(&r, DSD_ANALOG_CALL_BREAK, 0);
    assert(hold_journal_count(&j, NULL) == lines);
    hold_expect_nothing(&r, NULL, rejected_commits);
    hold_journal_close(&j);
    hold_close(&r);
}

/* A reception starts at its first block's first sample, the lead-in's: a call whose tone check began at x.51 s starts
   in that second although its first RECEPTION block ends in the next. Each recording segment is stamped from its first
   block's first sample, read before the open: one opened on a block from x.99 s to the next second is filed under x,
   and so is the segment a -P toggle opens later in the call. Main stamped both from the block's end. */
static void
test_candidate_start_crosses_a_second(void) {
    hold_rig r;
    hold_journal j;
    hold_open(&r, "dsdneo_cand_second", 0);
    hold_journal_open(&j);
    const time_t t0 = (time_t)(k_hold_t0_ns / 1000000000ULL);
    hold_advance_ms(&r, 510);
    hold_pending(&r, hold_lead_in_blocks(&r)); /* to x.99 s */
    hold_receive(&r, 1, 1000);                 /* x.99 s to x+1.01 s */
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 49, 1000); /* to x+1.99 s */
    r.opts->dmr_stereo_wav = 0;
    dsd_engine_analog_call_stop_recording(r.opts, r.state);
    r.opts->dmr_stereo_wav = 1;
    hold_receive(&r, 2, 2000); /* x+1.99 s to x+2.03 s */
    dsd_engine_analog_call_end(r.opts, r.state);
    const Event_History* row = &r.history[0].Event_History_Items[1];
    assert(row->systype == DSD_SYNC_ANALOG && row->event_start_time == t0 && row->event_time == t0 + 2);
    assert(hold_journal_count(&j, "publish") == 2);
    assert(hold_journal_publish_field(&j, 0, "start") == (long long)t0);
    assert(hold_journal_publish_field(&j, 1, "start") == (long long)t0 + 1);
    hold_files found;
    hold_take(&r, &found, 2);
    assert(found.frames[hold_file_starting(&found, 1000)] == 50L * HOLD_BLOCK);
    assert(found.frames[hold_file_starting(&found, 2000)] == 2L * HOLD_BLOCK);
    hold_release(&found);
    hold_journal_close(&j);
    hold_close(&r);
}

/* The one recording in the rig's folder: its name, then removed. */
typedef struct {
    const char* dir;
    int files;
    char name[256];
} hold_named;

static int
take_hold_named(const char* name, void* user) {
    hold_named* found = user;
    DSD_SNPRINTF(found->name, sizeof found->name, "%s", name);
    found->files++;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, found->dir, name) == 0);
    assert(remove(path) == 0);
    return 0;
}

/* Identity before a reception is a call (D4 within D1): a tone confirmed and then lost still names it, at its BEGIN,
   though the receiver no longer publishes it then; a tone confirmed only after it began does not start it over; a
   confirmed change (to another code, or to no tone at all) ends it, leaving nothing, and begins the next reception
   with its block. */
static void
test_candidate_identity(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_cand_identity", 0);
    assert(dsd_receive_names_parse("--tone-name '141.3=Traveler' --tone-name 'D023I=Dispatch'", &r.opts->receive.names,
                                   NULL, 0)
           == 0);
    for (int unknown_first = 0; unknown_first < 2; unknown_first++) {
        if (unknown_first) {
            r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
        } else {
            hold_lock_tone(&r, 1413);
        }
        const double first_m = hold_now_m(&r);
        hold_receive(&r, 10, 1000);
        if (unknown_first) {
            hold_lock_tone(&r, 1413);
        } else {
            r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
        }
        hold_receive(&r, 14, 1000);
        assert(!dsd_engine_analog_call_active(r.state));
        hold_receive(&r, 1, 1000);
        assert(dsd_engine_analog_call_active(r.state));
        dsd_call_snapshot call;
        assert(dsd_call_state_get(r.state, 0U, &call) > 0 && strcmp(call.service_label, "Traveler") == 0);
        assert(same_identity(&call.receive_identity, DSD_RECEIVE_CTCSS, 1413));
        const double last_m = hold_now_m(&r);
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_expect_span(&r, first_m, last_m);
        const Event_History* row = &r.history[0].Event_History_Items[1];
        assert(strcmp(row->service_label, "Traveler") == 0 && strstr(row->event_string, "CTCSS 141.3 Hz; ") != NULL);
        hold_services one = {.dir = r.dir};
        assert(dsd_dir_list(r.dir, take_hold_service, &one) == 0);
        assert(one.traveler == 1 && one.dispatch == 0 && one.frames[0] == 25L * HOLD_BLOCK);
    }
    for (int untoned = 0; untoned < 2; untoned++) {
        const uint64_t commits = r.history[0].push_seq;
        hold_lock_tone(&r, 1413);
        hold_receive(&r, 10, 1000);
        if (untoned) {
            r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
        } else {
            hold_lock_tone(&r, 0);
        }
        const double first_m = hold_now_m(&r);
        hold_receive(&r, 24, 2000);
        assert(!dsd_engine_analog_call_active(r.state) && r.history[0].push_seq == commits);
        hold_receive(&r, 1, 2000);
        assert(dsd_engine_analog_call_active(r.state));
        const double last_m = hold_now_m(&r);
        dsd_engine_analog_call_end(r.opts, r.state);
        assert(r.history[0].push_seq == commits + 1U);
        hold_expect_span(&r, first_m, last_m);
        const Event_History* row = &r.history[0].Event_History_Items[1];
        assert(strcmp(row->service_label, untoned ? "" : "Dispatch") == 0);
        assert(untoned ? same_identity(&row->receive_identity, DSD_RECEIVE_NONE, 0)
                       : row->receive_identity.kind == DSD_RECEIVE_DCS);
        if (untoned) {
            hold_expect_one_burst(&r, 25, 2000);
        } else {
            hold_services one = {.dir = r.dir};
            assert(dsd_dir_list(r.dir, take_hold_service, &one) == 0);
            assert(one.traveler == 0 && one.dispatch == 1 && one.frames[1] == 25L * HOLD_BLOCK);
        }
    }
    hold_close(&r);
}

/* Every way a reception can end before it is a call (D1). */
enum {
    CAND_END_EXPLICIT,      /* dsd_engine_analog_call_end(): a retune, a scan leave, an input or mode change, Stop */
    CAND_END_BREAK,         /* a BREAK block: a rejected tone, digital sync, a straddled boundary */
    CAND_END_CLOSED,        /* the block after the hangover, the hold off */
    CAND_END_EPOCH,         /* a block that carries a moved boundaries epoch */
    CAND_END_TICK_EPOCH,    /* the tick: the epoch moved with no block after it */
    CAND_END_TICK_DETECT,   /* the tick: tone detection off */
    CAND_END_TICK_REJECTED, /* the tick: the tone rejected */
    CAND_END_TICK_CLOSED,   /* the tick: the carrier closed, the hold off */
    CAND_END_TICK_STALE,    /* the tick: the input stopped past its deadline */
    CAND_END_HELD,          /* the hold on: a pause past the max gap */
    CAND_END_LEAD_IN,       /* a lead-in that a closed carrier ends before any RECEPTION block */
    CAND_END_TEARDOWN,      /* a teardown that skipped the engine's finalize */
    CAND_END_CASES,
};

static void
hold_end_candidate(hold_rig* r, int end) {
    switch (end) {
        case CAND_END_EXPLICIT: dsd_engine_analog_call_end(r->opts, r->state); break;
        case CAND_END_BREAK: hold_feed(r, DSD_ANALOG_CALL_BREAK, 0); break;
        case CAND_END_CLOSED:
        case CAND_END_LEAD_IN: hold_closed(r, 1); break;
        case CAND_END_EPOCH:
            r->state->analog_rx.boundaries++;
            hold_receive(r, 1, 2000);
            break;
        case CAND_END_TICK_EPOCH:
            hold_reset_receiver(r);
            assert(dsd_engine_analog_call_due(r->state)); /* a candidate the reset cut wakes the tick too (D12) */
            break;
        case CAND_END_TICK_DETECT: r->opts->analog_only = 0; break; /* the monitor tap off */
        case CAND_END_TICK_REJECTED: r->state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED; break;
        case CAND_END_TICK_CLOSED: r->state->analog_rx.carrier_open = 0; break;
        case CAND_END_TICK_STALE:
            r->state->analog_rx.stale_after_ms = 1U;
            assert(dsd_engine_analog_call_due(r->state)); /* a reception that is no call yet is due too */
            break;
        case CAND_END_HELD: hold_closed(r, 41); break; /* 200 ms hangover + 820 ms > 1000 ms */
        default: dsd_state_ext_free_all(r->state); break;
    }
    if (end >= CAND_END_TICK_EPOCH && end <= CAND_END_TICK_STALE) {
        dsd_engine_analog_call_tick(r->opts, r->state);
    }
}

/* D1: a reception that ends before it is a call, whatever ends it, leaves nothing: no row, live call or alert, and its
   temporary recording is discarded unpublished, with no journal line. */
static void
test_candidate_ends_leave_nothing(void) {
    for (int end = 0; end < CAND_END_CASES; end++) {
        hold_rig r;
        hold_journal j;
        hold_open(&r, "dsdneo_cand_end", end == CAND_END_HELD ? 1000 : 0);
        hold_journal_open(&j);
        const uint64_t commits = r.history[0].push_seq;
        if (end == CAND_END_LEAD_IN) {
            hold_pending(&r, 10);
        } else {
            hold_receive(&r, 10, 1000);
        }
        assert(!dsd_engine_analog_call_active(r.state));
        hold_end_candidate(&r, end);
        hold_expect_nothing(&r, &j, commits);
        r.opts->analog_only = 1;
        r.state->analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;
        r.state->analog_rx.stale_after_ms = 0U;
        hold_journal_close(&j);
        hold_close(&r);
    }
}

/* D1: per-call recording stopped (-P off) or moved to another folder before the reception is a call discards what it
   recorded so far, unpublished; the reception goes on and, once it is a call, publishes what it records from then on
   (nothing with recording off). */
static void
test_candidate_recording_stopped_or_moved(void) {
    for (int moved = 0; moved < 2; moved++) {
        hold_rig r;
        hold_journal j;
        char other[DSD_TEST_PATH_MAX];
        assert(dsd_test_mkdtemp(other, sizeof other, "dsdneo_cand_moved"));
        hold_open(&r, "dsdneo_cand_stop", 0);
        hold_journal_open(&j);
        hold_receive(&r, 10, 1000);
        r.opts->dmr_stereo_wav = 0;
        dsd_engine_analog_call_stop_recording(r.opts, r.state);
        if (moved) {
            r.opts->dmr_stereo_wav = 1;
            DSD_SNPRINTF(r.opts->wav_out_dir, sizeof r.opts->wav_out_dir, "%s", other);
        }
        hold_files found;
        hold_take(&r, &found, 0);
        assert(hold_journal_count(&j, NULL) == 0);
        hold_receive(&r, 15, 2000);
        assert(dsd_engine_analog_call_active(r.state));
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_take(&r, &found, 0);
        assert(hold_journal_count(&j, "publish") == moved);
        if (moved) {
            hold_rig there = r;
            DSD_SNPRINTF(there.dir, sizeof there.dir, "%s", other);
            hold_expect_one_burst(&there, 15, 2000);
        }
        assert(dsd_test_rmdir(other) == 0);
        hold_journal_close(&j);
        hold_close(&r);
    }
}

/* D1 with the hold on: a reception held before it is a call writes the pause into its recording on resuming, as a call
   does (the frames in order: the first burst, the pause's silence at 48 kHz from the held blocks' spans at their rate,
   the second burst), and once it is a call publishes it, its journal duration the whole file's. At 48 kHz and at
   44100 Hz, whose spans carry a remainder. */
static void
hold_check_held_candidate(int rate, long owed) {
    hold_rig r;
    hold_journal j;
    hold_open(&r, "dsdneo_cand_held", 1000);
    r.rate = rate;
    const long first = hold_burst_frames(&r, 5);
    const long second = hold_burst_frames(&r, 20);
    hold_journal_open(&j);
    hold_receive(&r, 5, 1000);
    hold_closed(&r, 10);
    assert(!dsd_engine_analog_call_active(r.state));
    const int lead = hold_lead_in_blocks(&r);
    /* lead + 1 RECEPTION blocks reach the minimum: 5, then lead - 5 more are one short, and the next makes the call. */
    hold_receive(&r, lead - 5, 2000);
    assert(!dsd_engine_analog_call_active(r.state) && hold_journal_count(&j, NULL) == 0);
    hold_receive(&r, 1, 2000);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 20 - (lead - 4), 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == first + owed + second);
    assert(found.samples[0][first - 1] != 0 && found.samples[0][found.frames[0] - 1] != 0);
    assert(hold_run_is(found.samples[0], first, owed, 0));
    assert(rate != 48000
           || (hold_run_is(found.samples[0], 0, first, 1000)
               && hold_run_is(found.samples[0], first + owed, second, 2000)));
    assert(hold_journal_count(&j, "publish") == 1);
    assert(hold_journal_publish_field(&j, 0, "duration_ms") == (long long)found.frames[0] * 1000LL / 48000LL);
    hold_release(&found);
    hold_journal_close(&j);
    hold_close(&r);
}

static void
test_held_candidate_owes_silence(void) {
    hold_check_held_candidate(48000, 10L * HOLD_BLOCK);
    /* Ten 960-sample blocks at 44100 Hz are 10448.98 samples at 48 kHz. */
    hold_check_held_candidate(44100, 10448L);
}

/* D1: a BEGIN that fails (the call state could not be allocated) leaves the reception a candidate, unpublished, and the
   next RECEPTION block tries again: the call then starts at the reception's first sample, its recording whole. One
   that ends before a BEGIN succeeds leaves nothing. Main ignored the failed BEGIN and went on as a call. */
static void
test_candidate_begin_failure(void) {
    for (int ends = 0; ends < 2; ends++) {
        hold_rig r;
        hold_open(&r, "dsdneo_cand_begin", 0);
        const uint64_t commits = r.history[0].push_seq;
        const double first_m = hold_now_m(&r);
        dsd_call_state_test_alloc_fail_after(0);
        hold_receive(&r, 25, 1000);
        assert(!dsd_engine_analog_call_active(r.state));
        dsd_call_snapshot call;
        assert(dsd_call_state_get(r.state, 0U, &call) <= 0);
        dsd_call_state_test_alloc_reset();
        if (ends) {
            hold_closed(&r, 1);
            hold_expect_nothing(&r, NULL, commits);
        } else {
            hold_receive(&r, 1, 1000);
            assert(dsd_engine_analog_call_active(r.state));
            const double last_m = hold_now_m(&r);
            dsd_engine_analog_call_end(r.opts, r.state);
            hold_expect_span(&r, first_m, last_m);
            assert(r.history[0].push_seq == commits + 1U);
            hold_expect_one_burst(&r, 26, 1000);
        }
        hold_close(&r);
    }
}

/* A teardown that skipped the engine's finalize: a reception that is no call yet leaves nothing, and a call's recording
   is published under the mode it began in, NFM on a radio input. */
static void
test_teardown_in_both_states(void) {
    for (int radio = 0; radio < 2; radio++) {
        hold_rig r;
        hold_open(&r, "dsdneo_cand_teardown", 0);
        if (radio) {
            r.opts->audio_in_type = AUDIO_IN_RTL;
            set_radio_squelch(r.opts);
        }
        hold_receive(&r, 24, 1000);
        dsd_state_ext_free_all(r.state);
        hold_files found;
        hold_take(&r, &found, 0);
        (void)hold_begin(&r, 2, 1000);
        dsd_state_ext_free_all(r.state);
        hold_named named = {.dir = r.dir};
        assert(dsd_dir_list(r.dir, take_hold_named, &named) == 0);
        assert(named.files == 1 && dsd_test_analog_recording_name_is(named.name, radio ? "NFM" : "ANALOG", NULL));
        hold_close(&r);
    }
}

/* D1: a lead-in is a tone check under an open carrier, so the tick leaves it, and its span counts: ten checked blocks
   and fifteen received make a call. A closed carrier ends it (it has no hold), and the count starts over. */
static void
test_lead_in_through_the_tick(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_cand_lead_tick", 0);
    const double first_m = hold_now_m(&r);
    hold_pending(&r, 10);
    dsd_engine_analog_call_tick(r.opts, r.state);
    hold_receive(&r, 14, 1000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 1, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    const double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 15, 1000);

    hold_pending(&r, 10);
    hold_closed(&r, 1);
    hold_receive(&r, 24, 1000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 1, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_one_burst(&r, 25, 1000);
    hold_close(&r);
}

/* A tone check under the carrier that ends a held call (here past a 1000 ms max gap) is the next reception's lead-in,
   whether the hold ends it on its own block or the reconciliation before that block does (a max gap lowered under it):
   one rule, so the next call starts at that block's first sample and qualifies on the same block either way. */
static void
test_lead_in_after_a_hold_ends(void) {
    for (int lowered = 0; lowered < 2; lowered++) {
        hold_rig r;
        hold_open(&r, "dsdneo_cand_lead_after_hold", 1000);
        const double first_m = hold_begin(&r, 2, 1000);
        const double last_m = hold_now_m(&r);
        hold_pending(&r, 50); /* 1000 ms of checks under the carrier: a gap the max gap still joins */
        assert(dsd_engine_analog_call_active(r.state));
        if (lowered) {
            r.opts->analog_record_max_gap_ms = 500;
        }
        hold_pending(&r, 1);
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, first_m, last_m);
        const double lead_m = hold_block_start_m(&r);
        hold_pending(&r, hold_lead_in_blocks(&r) - 1);
        assert(!dsd_engine_analog_call_active(r.state));
        hold_receive(&r, 1, 2000);
        assert(dsd_engine_analog_call_active(r.state));
        const double end_m = hold_now_m(&r);
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, lead_m, end_m);
        hold_files found;
        hold_take(&r, &found, 2);
        assert(found.frames[hold_file_starting(&found, 1000)] == 2L * HOLD_BLOCK);
        assert(found.frames[hold_file_starting(&found, 2000)] == HOLD_BLOCK);
        hold_release(&found);
        hold_close(&r);
    }
}

/* A call seeded at its BEGIN with a tone the receiver no longer publishes (confirmed, then checked again) goes on while
   the tone is unknown. A block whose tone is confirmed different ends it before that block's CONTINUE: one row, under
   the seeded tone, from the call's first sample to the last block before the change, never split inside the call
   state. The block begins the next reception, which needs its own minimum. */
static void
test_seeded_call_ends_on_a_new_tone(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_cand_seed_change", 0);
    assert(dsd_receive_names_parse("--tone-name '141.3=Traveler' --tone-name 'D023I=Dispatch'", &r.opts->receive.names,
                                   NULL, 0)
           == 0);
    const uint64_t commits = r.history[0].push_seq;
    hold_lock_tone(&r, 1413);
    const double first_m = hold_now_m(&r);
    hold_receive(&r, 10, 1000);
    r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    hold_receive(&r, 15, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 5, 1000);
    dsd_call_snapshot call;
    assert(dsd_call_state_get(r.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE);
    assert(same_identity(&call.receive_identity, DSD_RECEIVE_CTCSS, 1413));
    const double last_m = hold_now_m(&r);

    hold_lock_tone(&r, 0);
    hold_receive(&r, 1, 2000);
    const double next_m = hold_block_start_m(&r);
    assert(!dsd_engine_analog_call_active(r.state));
    assert(r.history[0].push_seq == commits + 1U);
    hold_expect_span(&r, first_m, last_m);
    const Event_History* row = &r.history[0].Event_History_Items[1];
    assert(strcmp(row->service_label, "Traveler") == 0 && strstr(row->event_string, "CTCSS 141.3 Hz; ") != NULL);
    assert(same_identity(&row->receive_identity, DSD_RECEIVE_CTCSS, 1413));

    hold_receive(&r, 23, 2000);
    assert(!dsd_engine_analog_call_active(r.state) && r.history[0].push_seq == commits + 1U);
    hold_receive(&r, 1, 2000);
    assert(dsd_engine_analog_call_active(r.state));
    const double next_last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, next_m, next_last_m);
    assert(r.history[0].push_seq == commits + 2U);
    assert(strcmp(r.history[0].Event_History_Items[1].service_label, "Dispatch") == 0);
    hold_services services = {.dir = r.dir};
    assert(dsd_dir_list(r.dir, take_hold_service, &services) == 0);
    assert(services.traveler == 1 && services.dispatch == 1);
    assert(services.frames[0] == 30L * HOLD_BLOCK && services.frames[1] == 25L * HOLD_BLOCK);
    hold_close(&r);
}

/* Read and remove the rig's one recording, published under @p mode. */
static void
hold_take_one_named(const hold_rig* r, const char* mode) {
    hold_named named = {.dir = r->dir};
    assert(dsd_dir_list(r->dir, take_hold_named, &named) == 0);
    assert(named.files == 1 && dsd_test_analog_recording_name_is(named.name, mode, NULL));
}

/* @p blocks RECEPTION blocks of @p value, each read by the real analog tap first, as the symbol path hands it over, so
   the tap publishes the carrier and the input's own stale deadline. */
static void
hold_tapped(hold_rig* r, int blocks, int value) {
    static float block[HOLD_BLOCK];
    for (int i = 0; i < HOLD_BLOCK; i++) {
        block[i] = (float)value;
    }
    for (int i = 0; i < blocks; i++) {
        dsd_analog_rx_tap(r->opts, r->state, block, HOLD_BLOCK);
        hold_feed(r, DSD_ANALOG_CALL_RECEPTION, value);
    }
}

/* Issue #667 D8: an AM reception on a radio input (-fM) is a call as an NFM one is, once it ran the minimum. Its row
   names AM and the frequency and no tone (dsd-neo detects none on AM), its system string is AM and its recording is
   named _AM. The AM tap reads its blocks and sets the live input's stale deadline, as the FM one does; once the stream
   gives nothing (the read yields) and the deadline passes, the tick ends the call at its last reception, and a carrier
   that comes back begins a new call. */
static void
test_am_call(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_am_call", 0);
    r.opts->audio_in_type = AUDIO_IN_RTL;
    r.opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    r.opts->rtlsdr_center_freq = 119100000U;
    set_radio_squelch(r.opts);
    r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_INACTIVE; /* the AM tap publishes no tone */
    assert(dsd_analog_monitor_tap_active(r.opts) && !dsd_analog_tone_detection_active(r.opts));
    r.opts->rtl_pwr = 1.0; /* the channel power the level squelch compares: over -60 dB */
    const uint64_t commits = r.history[0].push_seq;
    const double first_m = hold_now_m(&r);
    hold_tapped(&r, 24, 1000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_tapped(&r, 1, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    const double last_m = hold_now_m(&r);
    /* The live stream then gives nothing (the read yields), so no block comes. The tap's deadline is on the real clock
       (an input that stops is a wait), and the tick leaves the call until it is past. */
    const uint64_t deadline_ms = r.state->analog_rx.stale_after_ms;
    assert(deadline_ms != 0U);
    hold_advance_ms(&r, 5000);
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(dsd_engine_analog_call_active(r.state) || dsd_realtime_mono_ms() > deadline_ms);
    while (dsd_realtime_mono_ms() <= deadline_ms) {
        dsd_sleep_ms(10U);
    }
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, first_m, last_m);
    assert(r.history[0].push_seq == commits + 1U);
    const Event_History* row = &r.history[0].Event_History_Items[1];
    assert(row->systype == DSD_SYNC_ANALOG && row->analog_mode == DSD_ANALOG_MODE_AM);
    assert(strcmp(row_text(row), "ANALOG AM; FREQ: 119.100000 MHz; ") == 0);
    assert(strcmp(row->sysid_string, "AM") == 0);
    hold_take_one_named(&r, "AM");

    /* The stream comes back: the tap's read past its deadline starts its reception over, and a new call begins. */
    hold_tapped(&r, 24, 1000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_tapped(&r, 1, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(r.history[0].push_seq == commits + 2U);
    hold_take_one_named(&r, "AM");
    hold_close(&r);
}

/* @p blocks RECEPTION blocks make no call, and nothing is committed or recorded. */
static void
hold_expect_no_call(hold_rig* r, int blocks) {
    const uint64_t commits = r->history[0].push_seq;
    hold_receive(r, blocks, 1000);
    assert(!dsd_engine_analog_call_active(r->state));
    hold_closed(r, 1);
    assert(r->history[0].push_seq == commits);
    hold_named named = {.dir = r->dir};
    assert(dsd_dir_list(r->dir, take_hold_named, &named) == 0 && named.files == 0);
}

/* The minimum's blocks make a call, published under @p mode as its end commits it. */
static void
hold_expect_a_call(hold_rig* r, const char* mode) {
    const uint64_t commits = r->history[0].push_seq;
    hold_receive(r, 24, 1000);
    assert(!dsd_engine_analog_call_active(r->state));
    hold_receive(r, 1, 1000);
    assert(dsd_engine_analog_call_active(r->state));
    dsd_engine_analog_call_end(r->opts, r->state);
    assert(r->history[0].push_seq == commits + 1U);
    hold_take_one_named(r, mode);
}

static void
hold_expect_notices(int off_fm, int off_am, int back_on) {
    assert(g_calls_off_fm == off_fm && g_calls_off_am == off_am && g_calls_back_on == back_on);
}

/* Issue #667 D2: on a radio input the startup squelch (-110 dB) and an off one open for the receiver's own noise, so no
   reception becomes a call there and a running one ends: on its next block, or with no further sample at the tick. A
   level above the default, a dynamic squelch or, on the FM monitor, a tone allow list lifts it; on AM a list does not
   (there is no tone to check). Audio input is never paused. Each change is said once: paused, with the remedies that
   work for the monitor in force, and back on. */
static void
test_analog_calls_need_a_squelch(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_calls_squelch", 0);
    g_calls_off_fm = g_calls_off_am = g_calls_back_on = 0;
    r.opts->audio_in_type = AUDIO_IN_RTL;
    r.opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    r.opts->rtl_squelch_level = dB_to_pwr(DSD_SQUELCH_STARTUP_DB);
    hold_expect_no_call(&r, 30);
    hold_expect_notices(1, 0, 0);
    r.opts->rtl_squelch_level = 0.0;
    hold_expect_no_call(&r, 30);
    hold_expect_notices(1, 0, 0);

    set_radio_squelch(r.opts);
    const double first_m = hold_now_m(&r);
    hold_receive(&r, 30, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    hold_expect_notices(1, 0, 1);
    const double last_m = hold_now_m(&r);
    r.opts->rtl_squelch_level = dB_to_pwr(DSD_SQUELCH_STARTUP_DB);
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, first_m, last_m);
    hold_expect_notices(2, 0, 1);
    hold_take_one_named(&r, "NFM");
    /* A later tick with nothing running says nothing new; a change seen there is said at once. */
    dsd_engine_analog_call_tick(r.opts, r.state);
    hold_expect_notices(2, 0, 1);

    /* A running call ends at its next block too. */
    set_radio_squelch(r.opts);
    dsd_engine_analog_call_tick(r.opts, r.state);
    hold_expect_notices(2, 0, 2);
    hold_receive(&r, 25, 1000);
    assert(dsd_engine_analog_call_active(r.state));
    r.opts->rtl_squelch_level = 0.0;
    hold_receive(&r, 1, 1000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_notices(3, 0, 2);
    hold_take_one_named(&r, "NFM");

    /* An allow list on the FM monitor passes only its tones, so it lifts the pause; a block list does not. */
    r.opts->analog_tone_filter = DSD_TONE_FILTER_BLOCK;
    assert(dsd_tone_set_parse("100.0", &r.opts->analog_tone_set, NULL, 0) == 0);
    hold_expect_no_call(&r, 30);
    hold_expect_notices(3, 0, 2);
    r.opts->analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    r.state->analog_rx.gate = DSD_ANALOG_TONE_GATE_ALLOWED;
    hold_lock_tone(&r, 1000);
    hold_expect_a_call(&r, "NFM");
    hold_expect_notices(3, 0, 3);

    /* AM has no tone to list: the list leaves it paused, and the notice names no --tone-allow. A dynamic squelch lifts
       it there too. */
    r.opts->analog_demod = DSD_ANALOG_DEMOD_AM;
    r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_INACTIVE;
    hold_expect_no_call(&r, 30);
    hold_expect_notices(3, 1, 3);
    r.opts->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    hold_expect_a_call(&r, "AM");
    hold_expect_notices(3, 1, 4);

    /* Audio input, whatever its squelch: another receiver may squelch it, and its silence closes the carrier. */
    r.opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    r.opts->rtl_squelch_level = 0.0;
    r.opts->analog_tone_filter = DSD_TONE_FILTER_OFF;
    r.opts->analog_demod = DSD_ANALOG_DEMOD_FM;
    r.opts->audio_in_type = AUDIO_IN_WAV;
    r.state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    hold_expect_a_call(&r, "ANALOG");
    hold_expect_notices(3, 1, 4);
    hold_close(&r);
}

/* D2 reads the squelch in force, a -Y row's own included (scan_mode.c installs it over dsd_opts): a row's --squelch -60
   lifts the pause while the row is on air, and its leave puts the startup default back, which pauses calls again. */
static void
test_analog_calls_follow_a_rows_squelch(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_calls_row_squelch", 0);
    r.opts->audio_in_type = AUDIO_IN_RTL;
    r.opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    r.opts->rtl_squelch_level = dB_to_pwr(DSD_SQUELCH_STARTUP_DB);
    assert(dsd_scan_mode_begin(r.opts, r.state) == 0);
    assert(dsd_scan_mode_enter(r.opts, r.state, DSD_SCAN_MODE_NFM) == 0);
    dsd_scan_option_values row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.present = DSD_SCAN_OPT_SQUELCH;
    row.squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    row.squelch_db = -60;
    assert(dsd_scan_mode_options(r.opts, r.state, &row) == 0);
    hold_expect_a_call(&r, "NFM");
    dsd_scan_mode_leave(r.opts, r.state);
    assert(r.opts->rtl_squelch_level <= dB_to_pwr(DSD_SQUELCH_STARTUP_DB) * (1.0 + 1e-9));
    hold_expect_no_call(&r, 30);
    hold_close(&r);
}

static void
test_analog_hold(void) {
    test_hold_row_ends_at_the_last_reception();
    test_hold_row_end_is_the_block_stamp();
    test_hold_needs_recording();
    test_hold_boundary_move_ends_the_call();
    test_hold_off_keeps_main_payloads();
    test_hold_joins_a_pause_up_to_the_max_gap();
    test_hold_counts_the_gap_at_the_block_rate();
    test_hold_tone_checks_age_the_gap();
    test_hold_identity_on_resume();
    test_hold_new_identity_starts_at_the_block_stamp();
    test_hold_ends_before_a_begin_keep_its_stamp();
    test_hold_ends_without_the_pause();
    test_hold_tick_precedence();
    test_hold_tick_ends_a_moved_epoch();
    test_hold_live_changes_end();
    test_hold_live_changes_extend();
    test_hold_silence_write_failure_latches();
    test_hold_teardown_while_held();
    test_hold_largest_silence_write();
    test_candidate_qualifies_at_the_minimum();
    test_candidate_through_a_tone_check();
    test_candidate_start_crosses_a_second();
    test_candidate_identity();
    test_candidate_ends_leave_nothing();
    test_candidate_recording_stopped_or_moved();
    test_held_candidate_owes_silence();
    test_candidate_begin_failure();
    test_teardown_in_both_states();
    test_lead_in_through_the_tick();
    test_lead_in_after_a_hold_ends();
    test_seeded_call_ends_on_a_new_tone();
    test_am_call();
    test_analog_calls_need_a_squelch();
    test_analog_calls_follow_a_rows_squelch();
}

static int
remove_recording(const char* name, void* user) {
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, (const char*)user, name) == 0);
    assert(remove(path) == 0);
    return 0;
}

/* With --rdio-mode set, a conventional NFM recording has no talkgroup and is simply not exported: no "skipped" warning
   and no "export failed" line. A digital row with no talkgroup keeps both (issue #756). */
static void
test_analog_rdio_skip_is_quiet(dsd_opts* opts, dsd_state* state) {
    char directory[DSD_TEST_PATH_MAX];
    assert(dsd_test_mkdtemp(directory, sizeof directory, "dsdneo_receive_rdio"));
    DSD_MEMSET(opts->wav_out_dir, 0, sizeof opts->wav_out_dir);
    DSD_SNPRINTF(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", directory);
    opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts->dmr_stereo_wav = 1;
    state->analog_rx.carrier_open = 1;
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    float block[960];
    for (int i = 0; i < 960; ++i) {
        block[i] = 2000.0f;
    }
    g_rdio_warnings = 0;
    dsd_wav_recording_session_reset();
    dsd_analog_sink_block_begin(state);
    analog_lead_in(opts, state, 960, 48000);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));
    assert(g_rdio_warnings == 0 && g_rdio_failed == 0);

    /* The same close for a digital row with no talkgroup still warns, twice: the skip and the failure. */
    char wav_path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(wav_path, sizeof wav_path, directory, "digital.wav") == 0);
    SF_INFO info = {0};
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* wav = sf_open(wav_path, SFM_WRITE, &info);
    assert(wav);
    short pcm[160] = {0};
    assert(sf_write_short(wav, pcm, 160) == 160);
    static Event_History_I row;
    DSD_MEMSET(&row, 0, sizeof row);
    row.Event_History_Items[0].systype = DSD_SYNC_DMR_BS_VOICE_POS;
    row.Event_History_Items[0].event_time = (time_t)1700000000;
    const dsd_wav_owner owner = {DSD_WAV_OWNER_NONE, 0U, 0U, 0U};
    assert(close_and_rename_wav_file_ex(wav, opts, wav_path, directory, &row, 0, 1, &owner) == NULL);
    assert(g_rdio_warnings == 2 && g_rdio_failed == 1);
    opts->rdio_mode = DSD_RDIO_MODE_OFF;
    assert(dsd_dir_list(directory, remove_recording, directory) == 0);
    assert(dsd_test_rmdir(directory) == 0);
}

/* Issue #667 B6: under --rdio-mode an analog call's recording stays local, through real publication, NFM and AM
   alike, in DirWatch mode and with the API upload on too (both). Its row has no talkgroup to name an export by, so the
   WAV is published in the recording folder under its mode and linked to its row in the journal, and nothing goes to
   rdio-scanner: no DirWatch sidecar beside it (the folder holds the WAV alone), no upload queued (the journal's "rdio"
   is false) and no export warning. */
static void
test_analog_recordings_stay_local(void) {
    static const struct {
        int demod;
        const char* mode;
    } cases[] = {
        {DSD_ANALOG_DEMOD_FM, "NFM"},
        {DSD_ANALOG_DEMOD_AM, "AM"},
    };

    static const int modes[] = {DSD_RDIO_MODE_DIRWATCH, DSD_RDIO_MODE_BOTH};
    for (size_t k = 0; k < sizeof cases / sizeof cases[0] * 2U; k++) {
        const size_t i = k / 2U;
        hold_rig r;
        hold_journal j;
        hold_open(&r, "dsdneo_analog_local", 0);
        hold_journal_open(&j);
        r.opts->audio_in_type = AUDIO_IN_RTL;
        r.opts->analog_demod = cases[i].demod;
        r.opts->rtlsdr_center_freq = 154430000U;
        set_radio_squelch(r.opts);
        r.opts->rdio_mode = modes[k % 2U];
        r.opts->rdio_system_id = 7;
        DSD_SNPRINTF(r.opts->rdio_api_url, sizeof r.opts->rdio_api_url, "%s", "http://127.0.0.1:9");
        g_rdio_warnings = 0;
        g_rdio_failed = 0;
        hold_expect_a_call(&r, cases[i].mode);
        assert(r.history[0].Event_History_Items[1].analog_mode
               == (cases[i].demod == DSD_ANALOG_DEMOD_AM ? DSD_ANALOG_MODE_AM : DSD_ANALOG_MODE_NFM));
        assert(g_rdio_warnings == 0 && g_rdio_failed == 0);
        /* Published pending its row, then linked to it: the link names the publish line's id. */
        char line[4096];
        char link[4096];
        assert(hold_journal_count(&j, "publish") == 1 && hold_journal_count(&j, "link") == 1);
        assert(hold_journal_count(&j, NULL) == 2);
        assert(hold_journal_line(&j, "publish", 0, line, sizeof line));
        assert(hold_journal_line(&j, "link", 0, link, sizeof link));
        assert(strstr(line, "\"rdio\":false") != NULL && strstr(line, "\"failed\":false") != NULL);
        assert(strstr(line, "\"pending\":true") != NULL);
        const char* id = strstr(line, "\"id\":\"");
        assert(id != NULL);
        const char* id_end = strchr(id + 6, '"');
        assert(id_end != NULL);
        char want_id[128];
        DSD_SNPRINTF(want_id, sizeof want_id, "%.*s", (int)(id_end + 1 - id), id);
        assert(strstr(link, want_id) != NULL && strstr(link, "\"row\":") != NULL);
        hold_journal_close(&j);
        hold_close(&r);
    }
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    Event_History_I* history = calloc(2, sizeof(*history));
    assert(opts && state && history);
    dsd_neo_log_set_tap(count_log_warnings, NULL);
    state->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    test_protocol_context(opts, state);
    test_carrier_memory(opts, state);
    test_rejected_call_takes_no_enrichment(opts, state);
    test_admitted_segment_commits_after_rejection(opts, state);
    test_mono_slot_change_is_a_boundary(opts, state);
    test_reacquired_call_keeps_its_service(opts, state);
    test_carrier_admission_judges_both_slots(opts);
    test_events(opts, state);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    test_analog(opts, state);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&opts->receive, 0, sizeof opts->receive);
    state->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    test_analog_tone_change_seen_first_by_a_sync(opts, state);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    test_analog_event_frequency(opts, state);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    test_analog_row_text(opts, state);
    test_analog_mode_rows(opts, state);
    test_analog_seeded_begin(opts, state);
    test_analog_wall_anchored_stamps(opts, state);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    test_analog_rdio_skip_is_quiet(opts, state);
    dsd_state_ext_free_all(state);
    test_analog_recordings_stay_local();
    test_analog_hold();
    free(history);
    free(state);
    free(opts);
    return 0;
}
