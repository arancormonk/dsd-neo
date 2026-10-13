// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The TETRA rows every frontend shows (app_control/tetra_view.h), read from a snapshot of the decoder's publication.
 */

#include <assert.h>
#include <dsd-neo/app_control/tetra_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/tetra_publication.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MID " \xC2\xB7 "

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} fixture;

static fixture
fixture_open(void) {
    fixture f;
    f.opts = (dsd_opts*)calloc(1U, sizeof(*f.opts));
    f.state = (dsd_state*)calloc(1U, sizeof(*f.state));
    assert(f.opts != NULL && f.state != NULL);
    f.opts->frame_tetra = 1;
    return f;
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->state);
    free(f->state);
    free(f->opts);
}

/* Hidden without arguments, outside the TETRA preset, and until a block has decoded (mode NONE). */
static void
test_visibility(void) {
    fixture f = fixture_open();
    dsd_app_tetra view;
    assert(dsd_app_tetra_view(NULL, f.state, 0U, &view) == -1 && view.visible == 0U);
    assert(dsd_app_tetra_view(f.opts, NULL, 0U, &view) == -1);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, NULL) == -1);

    f.state->tetra_pub.generation = 3U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 0);
    assert(view.visible == 0U && view.generation == 3U && view.mode_text[0] == '\0');

    f.state->tetra_pub.mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1 && view.visible == 1U);
    assert(strcmp(view.mode_text, "TMO downlink") == 0 && view.trunked == 1U);

    f.opts->frame_tetra = 0; /* another preset: a stale publication stays off screen */
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 0 && view.visible == 0U);

    f.opts->frame_tetra = 1;
    f.state->tetra_pub.mode = 200U; /* a mode this build does not know */
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 0);
    fixture_close(&f);
}

/* Every working mode by name; trunked only in TMO; the encryption phrase in each of its cases. */
static void
test_mode_line(void) {
    fixture f = fixture_open();
    dsd_tetra_publication* const pub = &f.state->tetra_pub;
    dsd_app_tetra view;

    static const struct {
        const char* text;
        uint8_t mode;
        uint8_t trunked;
    } modes[] = {
        {"TMO downlink", DSD_TETRA_MODE_TMO_DOWNLINK, 1U},
        {"TMO uplink", DSD_TETRA_MODE_TMO_UPLINK, 1U},
        {"TMO control uplink", DSD_TETRA_MODE_TMO_CONTROL_UPLINK, 1U},
        {"DMO direct", DSD_TETRA_MODE_DMO_DIRECT, 0U},
        {"DMO via repeater", DSD_TETRA_MODE_DMO_REPEATER, 0U},
        {"DMO via gateway", DSD_TETRA_MODE_DMO_GATEWAY, 0U},
        {"DMO via repeater/gateway", DSD_TETRA_MODE_DMO_REPEATER_GATEWAY, 0U},
    };

    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        pub->mode = modes[i].mode;
        assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
        assert(strcmp(view.mode_text, modes[i].text) == 0);
        assert(view.trunked == modes[i].trunked && view.mode == modes[i].mode);
    }

    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    pub->air_encryption = DSD_TETRA_AIR_ENC_CLEAR;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "clear") == 0 && view.encrypted == 0U);
    pub->air_encryption = DSD_TETRA_AIR_ENC_ENCRYPTED;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted") == 0 && view.encrypted == 1U);
    pub->tea = 2U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (TEA2)") == 0);
    pub->air_encryption = DSD_TETRA_AIR_ENC_UNKNOWN;
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink - TEA2") == 0);
    pub->tea = 5U; /* no such TEA: named nowhere */
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink") == 0);
    fixture_close(&f);
}

/* The network names its colour code as the call history does ("CC 25", decimal), whatever --tetra-net spelled. */
static void
test_network(void) {
    fixture f = fixture_open();
    dsd_tetra_publication* const pub = &f.state->tetra_pub;
    dsd_app_tetra view;
    pub->mode = DSD_TETRA_MODE_DMO_DIRECT;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.network_known == 0U && view.network_text[0] == '\0');
    pub->net_known = 1U;
    pub->mcc = 262U;
    pub->mnc = 1001U;
    pub->colour = 0x19U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.network_known == 1U);
    assert(strcmp(view.network_text, "MCC 262" MID "MNC 1001" MID "CC 25") == 0);
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.network_text, "MCC 262 - MNC 1001 - CC 25") == 0);
    pub->mcc = 1023U;
    pub->mnc = 16383U;
    pub->colour = 63U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.network_text, "MCC 1023" MID "MNC 16383" MID "CC 63") == 0);
    pub->colour = 64U; /* not a colour code: no network row rather than a wrong one */
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.network_known == 0U && view.network_text[0] == '\0');
    fixture_close(&f);
}

/* The trunk row only while trunking follows calls on a downlink; the channels whenever known in TMO; timeslots TN1
   first; frequencies to five decimals, a trailing zero dropped. */
static void
test_channels(void) {
    fixture f = fixture_open();
    dsd_tetra_publication* const pub = &f.state->tetra_pub;
    dsd_app_tetra view;
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    pub->trunk_state = DSD_TETRA_TRUNK_ON_TRAFFIC;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.trunk_visible == 0U && view.trunk_text[0] == '\0' && view.on_traffic == 0U);
    f.opts->trunk_enable = 1;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.trunk_visible == 1U && view.on_traffic == 1U && strcmp(view.trunk_text, "traffic channel") == 0);
    pub->trunk_state = DSD_TETRA_TRUNK_ON_CONTROL;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.on_traffic == 0U && view.trunk_state == DSD_TETRA_TRUNK_ON_CONTROL);
    assert(strcmp(view.trunk_text, "control channel") == 0);
    pub->trunk_state = DSD_TETRA_TRUNK_IDLE;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1 && strcmp(view.trunk_text, "idle") == 0);
    pub->trunk_state = 9U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1 && strcmp(view.trunk_text, "idle") == 0);

    pub->cc_known = 1U;
    pub->cc_carrier = 321U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.control_known == 1U && strcmp(view.control_text, "carrier 321") == 0);
    pub->cc_freq_hz = 769768750U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.control_text, "carrier 321" MID "769.76875 MHz") == 0);
    pub->cc_freq_hz = 392012500U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.control_text, "carrier 321" MID "392.0125 MHz") == 0);

    pub->traffic_valid = 1U;
    pub->traffic_carrier = 654U;
    pub->traffic_slots = 0x08U; /* bit 3: timeslot 1 */
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.traffic_known == 1U && strcmp(view.traffic_text, "carrier 654" MID "TS 1") == 0);
    pub->traffic_slots = 0x0FU;
    pub->traffic_freq_hz = 392125000U;
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.traffic_text, "carrier 654 - TS 1,2,3,4 - 392.1250 MHz") == 0);
    pub->traffic_slots = 0x05U; /* timeslots 2 and 4 */
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.traffic_text, "carrier 654 - TS 2,4 - 392.1250 MHz") == 0);

    /* Direct mode has no trunking and no SYSINFO channels. */
    pub->mode = DSD_TETRA_MODE_DMO_DIRECT;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.trunk_visible == 0U && view.control_known == 0U && view.traffic_known == 0U);
    /* The uplink carries channels but no call following. */
    pub->mode = DSD_TETRA_MODE_TMO_UPLINK;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.trunk_visible == 0U && view.control_known == 1U && view.traffic_known == 1U);
    fixture_close(&f);
}

static void
test_mm_status_and_codec(void) {
    fixture f = fixture_open();
    dsd_tetra_publication* const pub = &f.state->tetra_pub;
    dsd_app_tetra view;
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.mm_status_known == 0U && view.codec_known == 0U && view.codec_text[0] == '\0');

    pub->mm_status_valid = 1U;
    pub->mm_status_code = 7U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.mm_status_known == 1U && strcmp(view.mm_status_text, "7" MID "MS frequency bands request") == 0);
    pub->mm_status_code = 63U;
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.mm_status_text, "63 - reserved") == 0);

    pub->codec_status = DSD_TETRA_CODEC_READY;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.codec_known == 1U && view.codec_fault == 0U && strcmp(view.codec_text, "ready") == 0);
    pub->codec_frames = 18U;
    pub->codec_errors = 2U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.codec_text, "ready" MID "18 frames" MID "2 errors") == 0);
    pub->codec_status = DSD_TETRA_CODEC_DECODE_ERRORS;
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(view.codec_fault == 1U && view.codec_status == DSD_TETRA_CODEC_DECODE_ERRORS);
    assert(strcmp(view.codec_text, "decode errors - 18 frames - 2 errors") == 0);
    pub->codec_frames = 0U;
    pub->codec_errors = 0U;
    pub->codec_status = DSD_TETRA_CODEC_MISSING;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.codec_fault == 1U && strcmp(view.codec_text, "missing") == 0);
    pub->codec_status = DSD_TETRA_CODEC_LOAD_FAILED;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.codec_fault == 1U && strcmp(view.codec_text, "failed to load") == 0);
    pub->codec_status = 9U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.codec_known == 0U && view.codec_text[0] == '\0');
    fixture_close(&f);
}

/* A reset clears everything but the generation, which it bumps; NULL is ignored. */
static void
test_publication_reset(void) {
    dsd_tetra_publication pub;
    DSD_MEMSET(&pub, 0xA5, sizeof pub);
    pub.generation = 41U;
    dsd_tetra_publication_reset(&pub);
    assert(pub.generation == 42U && pub.mode == DSD_TETRA_MODE_NONE && pub.net_known == 0U);
    assert(pub.codec_frames == 0U && pub.traffic_freq_hz == 0U && pub.mm_status_code == 0U);
    dsd_tetra_publication_reset(NULL);
}

int
main(void) {
    test_visibility();
    test_mode_line();
    test_network();
    test_channels();
    test_mm_status_and_codec();
    test_publication_reset();
    return 0;
}
