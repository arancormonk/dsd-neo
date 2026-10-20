// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
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
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->receive_context_active = 1;
    state->receive_context_protocol = DSD_SYNC_YSF_POS;
    assert(dsd_receive_current_allows(opts, state, 1)); /* Borrowed vocoder keeps its owner's protocol. */
    state->receive_context_active = 0;
    assert(!dsd_receive_current_allows(opts, state, 1));
}

typedef struct {
    const char* dir;
    int files;
    int traveler;
    int dispatch;
} recording_check;

static int
check_recording(const char* name, void* user) {
    recording_check* check = user;
    assert(strstr(name, ".wav")); /* NFM's zero target must not fabricate an Rdio sidecar. */
    assert(strstr(name, "_NFM_"));
    assert(!strstr(name, "TEMP"));
    check->traveler += strstr(name, "_SVC_Traveler__West_") != NULL;
    check->dispatch += strstr(name, "_SVC_Dispatch") != NULL;
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
    assert(check.files == 3 && check.traveler == 1 && check.dispatch == 1);
    assert(dsd_test_rmdir(directory) == 0);
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
    test_events(opts, state);
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof(*state));
    state->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    test_analog(opts, state);
    dsd_state_ext_free_all(state);
    free(history);
    free(state);
    free(opts);
    return 0;
}
