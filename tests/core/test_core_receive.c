// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/receive_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/wav_recording.h>
#include <dsd-neo/dsp/analog_sink.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/receive_policy.h>
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
    /* The NFM name: no talkgroup, source or call type, and the sanitized service last (issue #637). */
    const int traveler = dsd_test_nfm_recording_name_is(name, "Traveler__West_");
    const int dispatch = dsd_test_nfm_recording_name_is(name, "Dispatch");
    const int unnamed = dsd_test_nfm_recording_name_is(name, NULL);
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
    check->traveler += dsd_test_nfm_recording_name_is(name, "Traveler__West_");
    check->dispatch += dsd_test_nfm_recording_name_is(name, "Dispatch");
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
    check->unnamed += dsd_test_nfm_recording_name_is(name, NULL);
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
    dsd_engine_analog_call_block(opts, state, block, 480, 24000, 1);
    assert(dsd_engine_analog_call_active(state));
    Event_History_I* history = state->event_history_s;
    assert(!strcmp(history[0].Event_History_Items[0].service_label, "Traveler /West?"));
    assert(history[0].Event_History_Items[0].target_id == 0);
    assert(strcmp(history[0].Event_History_Items[0].sysid_string, "NFM") == 0);
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    state->analog_rx.dcs_code = 047;
    state->analog_rx.dcs_inverted = 0;
    DSD_MEMSET(block, 0, sizeof block);
    dsd_engine_analog_call_block(opts, state, block, 480, 24000, 1);
    assert(!strcmp(history[0].Event_History_Items[1].service_label, "Traveler /West?"));
    assert(!strcmp(history[0].Event_History_Items[0].service_label, "Dispatch"));
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    for (int i = 0; i < 960; ++i) {
        block[i] = 2000.0f;
    }
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
       recording, never one under the later name. */
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state->analog_rx.ctcss_tenths_hz = 1413;
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state->analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    service_files services = {.dir = directory};
    assert(dsd_dir_list(directory, count_service_files, &services) == 0);
    assert(services.files == 2 && services.traveler == 1 && services.dispatch == 1);
    /* A rate within the converter's tolerance of 48 kHz is written as it is, never dropped. */
    state->analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    dsd_engine_analog_call_block(opts, state, block, 960, 47999, 1);
    dsd_engine_analog_call_block(opts, state, block, 960, 47999, 0);
    recording_frames tolerated = {.dir = directory};
    assert(dsd_dir_list(directory, count_recording_frames, &tolerated) == 0);
    assert(tolerated.files == 1 && tolerated.frames == 960);
    /* A teardown that skips the engine's finalize still publishes the open recording, never leaving its TEMP_ file. */
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    dsd_state_ext_free_all(state);
    teardown_check teardown = {.dir = directory};
    assert(dsd_dir_list(directory, check_teardown_recording, &teardown) == 0);
    /* With no history row left it is published as an unnamed NFM recording, not under the digital name. */
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
    found->traveler += dsd_test_nfm_recording_name_is(name, "Traveler");
    found->dispatch += dsd_test_nfm_recording_name_is(name, "Dispatch");
    found->files++;
    char path[DSD_TEST_PATH_MAX * 2];
    assert(dsd_test_path_join(path, sizeof path, found->dir, name) == 0);
    assert(remove(path) == 0);
    return 0;
}

/* A tone change the decoder's own sync of the slot sees first (the frame sync's partial tone tap runs ahead of the next
   output block) is the analog call's lifecycle boundary, not the call state's: the recording still closes against the
   call's own row, named and as NFM, never against the row the sync had already committed and cleared. */
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
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    state->analog_rx.ctcss_tenths_hz = 1413;
    dsd_event_sync_slot(opts, state, 0);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));
    tone_sync_recordings found = {.dir = directory};
    assert(dsd_dir_list(directory, check_tone_sync_recording, &found) == 0);
    assert(found.files == 2 && found.traveler == 1 && found.dispatch == 1);
    assert(dsd_test_rmdir(directory) == 0);
}

/* Issue #670: the NFM row names the frequency as the digital rows do, in MHz with six decimals, and leaves it out when
   there is none (an audio input: the tuned frequency is 0), in the history and in the -J event log alike. */
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
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    assert(strstr(row->event_string, " NFM; ") != NULL);
    assert(strstr(row->event_string, "FREQ:") == NULL && strstr(row->event_string, "Frequency:") == NULL);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));

    opts->audio_in_type = AUDIO_IN_RTL;
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
    assert(dsd_engine_analog_call_active(state));
    assert(strstr(row->event_string, " NFM; FREQ: 162.550000 MHz; ") != NULL);
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 0);
    assert(!dsd_engine_analog_call_active(state));

    char log[4096];
    FILE* f = dsd_fopen_existing_regular_file(log_path, "rb");
    assert(f != NULL);
    const size_t n = fread(log, 1U, sizeof log - 1U, f);
    fclose(f);
    log[n] = '\0';
    assert(strstr(log, " NFM; FREQ: 162.550000 MHz; ") != NULL);
    assert(strstr(log, "Frequency:") == NULL);
    assert(strstr(log, "FREQ: 0.") == NULL);
    /* Both calls are logged: the audio input's row with no frequency, then the radio's. */
    const char* first = strstr(log, " NFM; ");
    assert(first != NULL && strncmp(first + 6, "FREQ:", 5) != 0);
    assert(strstr(first + 6, " NFM; FREQ: 162.550000 MHz; ") != NULL);
    opts->event_out_file[0] = '\0';
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->rtlsdr_center_freq = 0U;
    static const char* const files[] = {"events.log", NULL};
    assert(dsd_test_remove_temp_dir(directory, files) == 0);
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

static double
hold_now_m(const hold_rig* r) {
    return (double)r->now_ns / 1e9;
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

/* Blocks of a carrier whose tone is still being checked. */
static void
hold_pending(hold_rig* r, int blocks) {
    r->state->analog_rx.carrier_open = 1;
    for (int i = 0; i < blocks; i++) {
        hold_feed(r, DSD_ANALOG_CALL_HOLDABLE, 0);
    }
}

/* The slot's last call ended, and its row spans @p started_m to @p ended_m on the decode clock. */
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
    found->named += dsd_test_nfm_recording_name_is(name, NULL);
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

/* Read and remove every recording in the rig's folder: each must be published, under the NFM name, never left TEMP_. */
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
    hold_receive(&r, 3, 1000);
    double first_m = (double)(k_hold_t0_ns + 20000000ULL) / 1e9;
    double last_m = hold_now_m(&r);
    hold_advance_ms(&r, 10000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 3, 1000);

    /* The tick: the carrier closed with no block since (hold off), then the input stopped past its deadline. */
    hold_receive(&r, 2, 1000);
    first_m = last_m + 10.0 + 0.020;
    last_m = hold_now_m(&r);
    r.state->analog_rx.carrier_open = 0;
    hold_advance_ms(&r, 5000);
    dsd_engine_analog_call_tick(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    hold_expect_one_burst(&r, 2, 1000);
    hold_receive(&r, 2, 1000);
    last_m = hold_now_m(&r);
    r.state->analog_rx.stale_after_ms = 1U;
    hold_advance_ms(&r, 5000);
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, last_m - 0.020, last_m);
    r.state->analog_rx.stale_after_ms = 0U;
    hold_expect_one_burst(&r, 2, 1000);

    /* The hangover's end with the hold off: one block earlier than main's "now". */
    hold_receive(&r, 2, 1000);
    last_m = hold_now_m(&r);
    hold_closed(&r, 1);
    hold_expect_span(&r, last_m - 0.020, last_m);
    hold_expect_one_burst(&r, 2, 1000);

    /* Held, then ended: the hold is not the call's. */
    r.opts->analog_record_max_gap_ms = 1000;
    hold_receive(&r, 2, 1000);
    last_m = hold_now_m(&r);
    hold_closed(&r, 5);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, last_m - 0.020, last_m);
    hold_expect_one_burst(&r, 2, 1000);
    hold_close(&r);
}

/* A recording write that takes decode time, as a slow disk does under a live clock: the clock moves 7 ms on. */
static long long
hold_slow_write(SNDFILE* file, const short* samples, long long count) {
    dsd_decode_clock_test_set_ns(dsd_decode_now_mono_ns() + 7000000ULL);
    return (long long)sf_write_short(file, samples, (sf_count_t)count);
}

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
    hold_receive(&r, 1, 1000);
    const double first_m = hold_now_m(&r);
    hold_receive(&r, 1, 1000);
    double last_m = hold_now_m(&r);
    hold_closed(&r, 3);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, first_m, last_m);
    hold_expect_one_burst(&r, 2, 1000);

    /* A resume as the call's last reception: three held blocks of silence are written before its own audio. */
    hold_receive(&r, 1, 1000);
    const double resumed_first_m = hold_now_m(&r);
    hold_receive(&r, 1, 1000);
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
        hold_receive(&r, 2, 1000);
        const double last_m = hold_now_m(&r);
        if (tick) {
            r.state->analog_rx.carrier_open = 0;
            dsd_engine_analog_call_tick(r.opts, r.state);
        } else {
            hold_closed(&r, 1);
        }
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_span(&r, last_m - 0.020, last_m);
    }
    hold_files found;
    hold_take(&r, &found, 0);

    for (int tick = 0; tick < 2; tick++) {
        r.opts->dmr_stereo_wav = 1;
        hold_receive(&r, 2, 1000);
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
        hold_expect_span(&r, last_m - 0.020, last_m);
    }
    r.opts->dmr_stereo_wav = 1;
    hold_receive(&r, 2, 2000);
    const double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, last_m - 0.020, last_m);
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
    hold_receive(&r, 1, 1000);
    double last_m = hold_now_m(&r);
    r.state->analog_rx.boundaries++;
    hold_receive(&r, 1, 2000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, last_m, last_m);
    hold_receive(&r, 1, 3000);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 2);
    assert(found.frames[hold_file_starting(&found, 1000)] == HOLD_BLOCK);
    assert(found.frames[hold_file_starting(&found, 3000)] == HOLD_BLOCK);
    hold_release(&found);

    /* Held: the move ends the held call without its pause, on a reception or on a held block. */
    r.opts->analog_record_max_gap_ms = 1000;
    hold_receive(&r, 1, 4000);
    last_m = hold_now_m(&r);
    hold_closed(&r, 3);
    r.state->analog_rx.boundaries++;
    hold_receive(&r, 1, 5000);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, last_m, last_m);
    hold_receive(&r, 1, 6000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_take(&r, &found, 2);
    assert(found.frames[hold_file_starting(&found, 4000)] == HOLD_BLOCK);
    assert(found.frames[hold_file_starting(&found, 6000)] == HOLD_BLOCK);
    hold_release(&found);
    hold_receive(&r, 1, 7000);
    hold_closed(&r, 2);
    r.state->analog_rx.boundaries++;
    hold_closed(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 1, 7000);
    hold_close(&r);
}

/* A held call is one call: one row from its first reception to its last, committed once (one VOICE_END: the commit of
   a call that ended, never of one held), and one recording with the pause as silence where it was. A pause of exactly
   the max gap is joined (the 200 ms hangover the blocks before it recorded, then 300 ms); one block more ends the call
   at that block, with nothing written for the pause. */
static void
test_hold_joins_a_pause_up_to_the_max_gap(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_48k", 500);
    const uint64_t commits = r.history[0].push_seq;
    hold_receive(&r, 5, 1000);
    const double first_m = (double)(k_hold_t0_ns + 20000000ULL) / 1e9;
    hold_closed(&r, 15);
    assert(dsd_engine_analog_call_active(r.state));
    assert(r.history[0].push_seq == commits);
    hold_receive(&r, 5, 2000);
    const double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, first_m, last_m);
    assert(fabs((last_m - first_m) - 0.480) < 1e-6);
    assert(r.history[0].push_seq == commits + 1U);
    assert(strcmp(r.history[0].Event_History_Items[1].sysid_string, "NFM") == 0);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 25L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 0, 5L * HOLD_BLOCK, 1000));
    assert(hold_run_is(found.samples[0], 5L * HOLD_BLOCK, 15L * HOLD_BLOCK, 0));
    assert(hold_run_is(found.samples[0], 20L * HOLD_BLOCK, 5L * HOLD_BLOCK, 2000));
    hold_release(&found);

    hold_receive(&r, 5, 3000);
    const double over_m = hold_now_m(&r);
    hold_closed(&r, 15);
    assert(dsd_engine_analog_call_active(r.state));
    hold_closed(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_span(&r, over_m - 0.080, over_m);
    hold_receive(&r, 5, 4000);
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
    hold_receive(r, blocks, 2000);
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
   blocks are joined, as one row from the first reception to the last, and one more ends the call at its last
   reception, a row of its own. */
static void
hold_check_rate(int rate, int max_gap_ms, int joined, long owed) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_rate", max_gap_ms);
    r.rate = rate;
    const double block_s = (double)HOLD_BLOCK / (double)rate;
    const long burst = hold_burst_frames(&r, 3);
    const uint64_t commits = r.history[0].push_seq;
    hold_receive(&r, 1, 2000);
    const double first_m = hold_now_m(&r);
    hold_receive(&r, 2, 2000);
    hold_closed(&r, joined);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 3, 2000);
    double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(r.history[0].push_seq == commits + 1U);
    hold_expect_span(&r, first_m, last_m);
    assert(fabs((last_m - first_m) - (double)(5 + joined) * block_s) < 1e-6);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 2 * burst + owed);
    assert(found.samples[0][burst - 1] != 0 && found.samples[0][found.frames[0] - 1] != 0);
    assert(hold_run_is(found.samples[0], burst, owed, 0));
    hold_release(&found);

    hold_receive(&r, 3, 2000);
    last_m = hold_now_m(&r);
    hold_closed(&r, joined + 1);
    assert(!dsd_engine_analog_call_active(r.state));
    assert(r.history[0].push_seq == commits + 2U);
    hold_expect_span(&r, last_m - 2.0 * block_s, last_m);
    hold_receive(&r, 3, 2000);
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
    hold_receive(&r, 5, 1000);
    for (int i = 0; i < 3; i++) {
        hold_closed(&r, 3);
        hold_pending(&r, 2);
    }
    assert(dsd_engine_analog_call_active(r.state));
    hold_pending(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 5, 1000);

    hold_receive(&r, 5, 1000);
    hold_pending(&r, 25);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 5, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 35L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 5L * HOLD_BLOCK, 25L * HOLD_BLOCK, 0));
    hold_release(&found);
    hold_receive(&r, 5, 1000);
    hold_pending(&r, 26);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 5, 1000);

    hold_receive(&r, 5, 1000);
    hold_pending(&r, 5);
    hold_closed(&r, 20);
    assert(dsd_engine_analog_call_active(r.state));
    hold_receive(&r, 1, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 31L * HOLD_BLOCK);
    assert(hold_run_is(found.samples[0], 5L * HOLD_BLOCK, 25L * HOLD_BLOCK, 0));
    hold_release(&found);
    hold_receive(&r, 5, 1000);
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
    const int dispatch = dsd_test_nfm_recording_name_is(name, "Dispatch");
    assert(dispatch || dsd_test_nfm_recording_name_is(name, "Traveler"));
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
   one ends the held call at its last reception, without the pause, and begins a new call with its first block. */
static void
test_hold_identity_on_resume(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_identity", 1000);
    assert(dsd_receive_names_parse("--tone-name '141.3=Traveler' --tone-name 'D023I=Dispatch'", &r.opts->receive.names,
                                   NULL, 0)
           == 0);
    hold_lock_tone(&r, 1413);
    for (int acquiring = 0; acquiring < 2; acquiring++) {
        hold_receive(&r, 2, 1000);
        hold_closed(&r, 5);
        r.state->analog_rx.tone_state = acquiring ? DSD_ANALOG_TONE_STATE_ACQUIRING : DSD_ANALOG_TONE_STATE_LOCKED;
        hold_receive(&r, 2, 2000);
        hold_lock_tone(&r, 1413);
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_services one = {.dir = r.dir};
        assert(dsd_dir_list(r.dir, take_hold_service, &one) == 0);
        assert(one.traveler == 1 && one.dispatch == 0 && one.frames[0] == 9L * HOLD_BLOCK);
    }
    hold_receive(&r, 2, 1000);
    const uint64_t commits = r.history[0].push_seq;
    hold_closed(&r, 5);
    hold_advance_ms(&r, 5000); /* No clock decides the hold, and the row ends at the last reception. */
    hold_lock_tone(&r, 0);
    hold_receive(&r, 2, 2000);
    assert(dsd_engine_analog_call_active(r.state) && r.history[0].push_seq == commits + 1U);
    const Event_History* held = &r.history[0].Event_History_Items[1];
    assert(!strcmp(held->service_label, "Traveler") && held->event_time - held->event_start_time == 0);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_services two = {.dir = r.dir};
    assert(dsd_dir_list(r.dir, take_hold_service, &two) == 0);
    assert(two.traveler == 1 && two.dispatch == 1 && two.frames[0] == 2L * HOLD_BLOCK
           && two.frames[1] == 2L * HOLD_BLOCK);
    hold_close(&r);
}

/* A recording close that takes decode time, as hold_slow_write() does. */
static int
hold_slow_close(SNDFILE* file) {
    dsd_decode_clock_test_set_ns(dsd_decode_now_mono_ns() + 7000000ULL);
    return sf_close(file);
}

/* D4 on a slow disk: a new identity on the resume ends the held call and closes its recording before the new call
   begins with the block, whose start is still the block's stamp. It was read after the close. */
static void
test_hold_new_identity_starts_at_the_block_stamp(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_identity_stamp", 1000);
    assert(dsd_receive_names_parse("--tone-name '141.3=Traveler' --tone-name 'D023I=Dispatch'", &r.opts->receive.names,
                                   NULL, 0)
           == 0);
    dsd_wav_test_set_io(hold_slow_write, hold_slow_close);
    hold_lock_tone(&r, 1413);
    hold_receive(&r, 2, 1000);
    hold_closed(&r, 3);
    hold_lock_tone(&r, 0);
    hold_receive(&r, 1, 2000);
    const double last_m = hold_now_m(&r);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ACTIVE, last_m, last_m);
    dsd_engine_analog_call_end(r.opts, r.state);
    dsd_wav_test_set_io(NULL, NULL);
    hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, last_m, last_m);
    hold_services two = {.dir = r.dir};
    assert(dsd_dir_list(r.dir, take_hold_service, &two) == 0);
    assert(two.traveler == 1 && two.dispatch == 1 && two.frames[0] == 2L * HOLD_BLOCK && two.frames[1] == HOLD_BLOCK);
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

/* On a slow disk, every end of a held call that closes its recording before the next reception begins a call (in that
   block or before it) leaves the new call's stamps the block's: the close's time is not the call's. The block's stamp
   was read after the reconciliation that opens the block closed the old recording. */
static void
test_hold_ends_before_a_begin_keep_its_stamp(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_end_stamp", 1000);
    dsd_wav_test_set_io(hold_slow_write, hold_slow_close);
    for (int end = 0; end < HOLD_END_CASES; end++) {
        r.opts->dmr_stereo_wav = 1;
        r.opts->analog_record_max_gap_ms = 1000;
        hold_receive(&r, 2, 1000);
        hold_closed(&r, 10); /* 400 ms */
        hold_end_before_a_reception(&r, end);
        hold_receive(&r, 1, 3000);
        const double block_m = hold_now_m(&r);
        hold_expect_stamps(&r, DSD_CALL_PHASE_ACTIVE, block_m, block_m);
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_expect_stamps(&r, DSD_CALL_PHASE_ENDED, block_m, block_m);
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
    hold_receive(&r, 2, 1000);
    double last_m = hold_now_m(&r);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_span(&r, last_m - 0.020, last_m);
    hold_expect_one_burst(&r, 2, 1000);
    for (int rejected = 0; rejected < 2; rejected++) {
        hold_receive(&r, 2, 1000);
        last_m = hold_now_m(&r);
        hold_closed(&r, 5);
        if (rejected) {
            hold_feed(&r, DSD_ANALOG_CALL_BREAK, 0);
        } else {
            dsd_engine_analog_call_end(r.opts, r.state);
        }
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_span(&r, last_m - 0.020, last_m);
        hold_expect_one_burst(&r, 2, 1000);
    }
    assert(r.history[0].push_seq == commits + 3U);
    hold_close(&r);
}

/* Receive two blocks and hold the call for three, returning the last reception's end. */
static double
hold_start_held(hold_rig* r) {
    hold_receive(r, 2, 1000);
    const double last_m = hold_now_m(r);
    hold_closed(r, 3);
    assert(dsd_engine_analog_call_active(r->state));
    return last_m;
}

static void
hold_expect_tick_ends(hold_rig* r, double last_m) {
    dsd_engine_analog_call_tick(r->opts, r->state);
    assert(!dsd_engine_analog_call_active(r->state));
    hold_expect_span(r, last_m - 0.020, last_m);
    hold_expect_one_burst(r, 2, 1000);
}

/* The tick, in order: what no hold survives ends a held call there (detection off, a REJECTED tone, a retune
   unresolved), the hold off ends a call on a closed carrier as on main, and the stale deadline (the input stopped)
   ends it held or not. A held call with input flowing needs no tick: the closed carrier waits for the next block. */
static void
test_hold_tick_precedence(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_tick", 1000);
    hold_receive(&r, 2, 1000);
    r.state->analog_rx.carrier_open = 0;
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(dsd_engine_analog_call_active(r.state));
    hold_closed(&r, 3);
    dsd_engine_analog_call_tick(r.opts, r.state);
    assert(dsd_engine_analog_call_active(r.state));
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_expect_one_burst(&r, 2, 1000);

    double last_m = hold_start_held(&r);
    r.opts->analog_only = 0;
    hold_expect_tick_ends(&r, last_m);
    r.opts->analog_only = 1;
    last_m = hold_start_held(&r);
    r.state->analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    hold_expect_tick_ends(&r, last_m);
    r.state->analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;
    last_m = hold_start_held(&r);
    (void)dsd_trunk_tuning_request_begin();
    hold_expect_tick_ends(&r, last_m);
    dsd_trunk_tuning_requests_reset();
    last_m = hold_start_held(&r);
    r.state->analog_rx.stale_after_ms = 1U;
    hold_expect_tick_ends(&r, last_m);
    r.state->analog_rx.stale_after_ms = 0U;
    /* Lowered below the gap so far (260 ms), the hold still on: the tick reconciles. Turned off, it ends as on main. */
    for (int off = 0; off < 2; off++) {
        r.opts->analog_record_max_gap_ms = 1000;
        last_m = hold_start_held(&r);
        r.opts->analog_record_max_gap_ms = off ? 200 : 259;
        hold_expect_tick_ends(&r, last_m);
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
        hold_receive(&r, 2, 1000);
        const double last_m = hold_now_m(&r);
        if (held) {
            hold_closed(&r, 3);
        }
        hold_reset_receiver(&r);
        hold_advance_ms(&r, 5000);
        hold_expect_tick_ends(&r, last_m);
        dsd_engine_analog_call_tick(r.opts, r.state);
        assert(!dsd_engine_analog_call_active(r.state));
    }
    r.opts->analog_record_max_gap_ms = 0;
    hold_receive(&r, 2, 1000);
    const double last_m = hold_now_m(&r);
    hold_reset_receiver(&r);
    hold_expect_tick_ends(&r, last_m);
    hold_close(&r);
}

/* D10, ending: every value at or under the hangover is off (a closed carrier ends an active call at once, as on main);
   turning the hold off, or lowering it below the gap so far, ends a held call at the next block, which a reception
   then begins anew; the first held block may already be past it (200 ms hangover + 20 ms). */
static void
test_hold_live_changes_end(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_live_end", 1000);
    static const int off[] = {0, 1, 200};
    for (size_t i = 0; i < sizeof off / sizeof off[0]; i++) {
        r.opts->analog_record_max_gap_ms = off[i];
        hold_receive(&r, 2, 1000);
        const double last_m = hold_now_m(&r);
        hold_closed(&r, 1);
        assert(!dsd_engine_analog_call_active(r.state));
        hold_expect_span(&r, last_m - 0.020, last_m);
        hold_expect_one_burst(&r, 2, 1000);
    }
    for (int lowered = 0; lowered < 2; lowered++) {
        r.opts->analog_record_max_gap_ms = 1000;
        hold_receive(&r, 2, 1000);
        hold_closed(&r, 10); /* 400 ms */
        r.opts->analog_record_max_gap_ms = lowered ? 399 : 0;
        hold_receive(&r, 1, 2000);
        assert(dsd_engine_analog_call_active(r.state));
        dsd_engine_analog_call_end(r.opts, r.state);
        hold_files found;
        hold_take(&r, &found, 2);
        assert(found.frames[hold_file_starting(&found, 1000)] == 2L * HOLD_BLOCK);
        assert(found.frames[hold_file_starting(&found, 2000)] == HOLD_BLOCK);
        hold_release(&found);
    }
    r.opts->analog_record_max_gap_ms = 219;
    hold_receive(&r, 2, 1000);
    hold_closed(&r, 1);
    assert(!dsd_engine_analog_call_active(r.state));
    hold_expect_one_burst(&r, 2, 1000);
    r.opts->analog_record_max_gap_ms = 220;
    hold_receive(&r, 2, 1000);
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
    hold_receive(&r, 2, 1000);
    hold_closed(&r, 10); /* 400 ms */
    r.opts->analog_record_max_gap_ms = 400;
    hold_receive(&r, 1, 2000);
    dsd_engine_analog_call_end(r.opts, r.state);
    hold_files found;
    hold_take(&r, &found, 1);
    assert(found.frames[0] == 13L * HOLD_BLOCK && hold_run_is(found.samples[0], 2L * HOLD_BLOCK, 10L * HOLD_BLOCK, 0));
    hold_release(&found);
    r.opts->analog_record_max_gap_ms = 300;
    hold_receive(&r, 2, 1000);
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
   BREAK each end the call, and each reception's blocks land in a file of their own as they came. */
static void
test_hold_off_keeps_main_payloads(void) {
    hold_rig r;
    hold_open(&r, "dsdneo_hold_golden", 200);
    const uint64_t commits = r.history[0].push_seq;
    hold_receive(&r, 1, 100);
    hold_receive(&r, 1, 200);
    hold_receive(&r, 1, 300);
    hold_closed(&r, 1);
    hold_receive(&r, 1, 400);
    hold_receive(&r, 1, 500);
    hold_pending(&r, 1);
    hold_receive(&r, 1, 600);
    hold_feed(&r, DSD_ANALOG_CALL_BREAK, 0);
    hold_receive(&r, 1, 700);
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

static long long
hold_seam_write(SNDFILE* file, const short* samples, long long count) {
    const int call = g_hold_writes++;
    return (long long)sf_write_short(file, samples, (sf_count_t)(call == g_hold_short_at ? count / 2 : count));
}

static void
count_log_warnings(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level == LOG_LEVEL_WARN && text && strstr(text, "NFM call: wrote") != NULL) {
        g_hold_write_warnings++;
    }
    if (level == LOG_LEVEL_WARN && text && strstr(text, "Rdio export") != NULL) {
        g_rdio_warnings++;
        g_rdio_failed += strstr(text, "Rdio export failed") != NULL;
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
    hold_receive(&r, 2, 1000);
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
    hold_receive(&r, 2, 1000);
    hold_closed(&r, 5);
    dsd_engine_analog_call_end(r.opts, r.state);
    assert(!strcmp(r.history[0].Event_History_Items[1].sysid_string, "NFM"));
    hold_expect_one_burst(&r, 2, 1000);
    hold_receive(&r, 2, 1000);
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
    hold_receive(&r, 1, 1000);
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
    dsd_engine_analog_call_block(opts, state, block, 960, 48000, 1);
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
    test_analog_rdio_skip_is_quiet(opts, state);
    dsd_state_ext_free_all(state);
    test_analog_hold();
    free(history);
    free(state);
    free(opts);
    return 0;
}
