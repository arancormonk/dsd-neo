// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/receive_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/receive_policy.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_support.h"

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

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    Event_History_I* history = calloc(2, sizeof(*history));
    assert(opts && state && history);
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
    free(history);
    free(state);
    free(opts);
    return 0;
}
