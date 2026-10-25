// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The TETRA rows every frontend shows (app_control/tetra_view.h), read from a snapshot of the decoder's publication.
 */

#include <assert.h>
#include <dsd-neo/app_control/tetra_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/received_identity.h>
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
    /* An encrypted trunked network that named no algorithm says it does not broadcast one; direct mode, whose
     * DMAC-SYNC would name it, says only that it is encrypted. */
    pub->air_encryption = DSD_TETRA_AIR_ENC_ENCRYPTED;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (TEA not broadcast)") == 0 && view.encrypted == 1U);
    pub->mode = DSD_TETRA_MODE_DMO_DIRECT;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "DMO direct" MID "encrypted") == 0);
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    pub->ksg_mask = 1U << 1;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (TEA2)") == 0);
    pub->air_encryption = DSD_TETRA_AIR_ENC_UNKNOWN;
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink - TEA2") == 0);
    /* The network's algorithm shows whatever the last PDU's encryption. */
    pub->air_encryption = DSD_TETRA_AIR_ENC_CLEAR;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "clear" MID "TEA2") == 0);
    pub->air_encryption = DSD_TETRA_AIR_ENC_ENCRYPTED;
    pub->ksg_mask = (1U << 0) | (1U << 1);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (TEA1/TEA2)") == 0);
    pub->ksg_mask = 1U << 9;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (proprietary KSG 9)") == 0);
    pub->ksg_mask = (1U << 3) | (1U << 5);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (TEA4/KSG 5)") == 0);

    /* More algorithms than the row holds: it stays whole, and says some were left out. */
    static const struct {
        uint8_t mode;
        uint16_t mask;
    } k_cut[] = {
        {DSD_TETRA_MODE_TMO_DOWNLINK, 0x0707U},
        {DSD_TETRA_MODE_DMO_REPEATER_GATEWAY, 0xFFFFU},
    };

    for (size_t i = 0; i < sizeof k_cut / sizeof k_cut[0]; i++) {
        pub->mode = k_cut[i].mode;
        pub->ksg_mask = k_cut[i].mask;
        assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
        const size_t len = strlen(view.mode_text);
        assert(len < sizeof view.mode_text && strstr(view.mode_text, "(TEA1/TEA2/") != NULL);
        assert(len > 5U && strcmp(view.mode_text + len - 5U, "/...)") == 0);
    }
    /* The last name is never traded for the marker when it fits. */
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    pub->ksg_mask = 0x0307U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (TEA1/TEA2/TEA3/proprietary KSG 8/proprietary KSG 9)")
           == 0);
    fixture_close(&f);
}

/* The algorithms radios proposed in clear uplink registrations come last, as "radios propose", beside whatever the
 * network named or the encryption says, never in place of it; never in direct mode. They take what room the row has
 * left, the last name to the exact byte, and drop whole when not one name fits. */
static void
test_mode_line_proposed(void) {
    fixture f = fixture_open();
    dsd_tetra_publication* const pub = &f.state->tetra_pub;
    dsd_app_tetra view;
    pub->mode = DSD_TETRA_MODE_TMO_CONTROL_UPLINK;
    pub->air_encryption = DSD_TETRA_AIR_ENC_CLEAR;
    pub->ksg_requested_mask = 1U << 0;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO control uplink" MID "clear" MID "radios propose TEA1") == 0);

    pub->air_encryption = DSD_TETRA_AIR_ENC_ENCRYPTED;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO control uplink" MID "encrypted (TEA not broadcast)" MID "radios propose TEA1")
           == 0);
    /* The longest fixed form leaves room for two names and the terminator, in UTF-8 and in ASCII. */
    pub->ksg_requested_mask = (1U << 0) | (1U << 1);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(
        strcmp(view.mode_text, "TMO control uplink" MID "encrypted (TEA not broadcast)" MID "radios propose TEA1/TEA2")
        == 0);
    assert(strlen(view.mode_text) + 1U == sizeof view.mode_text);
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.mode_text, "TMO control uplink - encrypted (TEA not broadcast) - radios propose TEA1/TEA2")
           == 0);
    pub->ksg_requested_mask = (1U << 0) | (1U << 1) | (1U << 2);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(
        strcmp(view.mode_text, "TMO control uplink" MID "encrypted (TEA not broadcast)" MID "radios propose TEA1/...")
        == 0);

    /* Beside the network's own algorithm; and the last name fills its room to the byte. */
    pub->mode = DSD_TETRA_MODE_TMO_UPLINK;
    pub->ksg_mask = 1U << 1;
    pub->ksg_requested_mask = 1U << 0;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO uplink" MID "encrypted (TEA2)" MID "radios propose TEA1") == 0);
    pub->mode = DSD_TETRA_MODE_TMO_CONTROL_UPLINK;
    pub->ksg_mask = (1U << 0) | (1U << 9);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(
        strcmp(view.mode_text, "TMO control uplink" MID "encrypted (TEA1/proprietary KSG 9)" MID "radios propose TEA1")
        == 0);
    assert(strlen(view.mode_text) + 1U == sizeof view.mode_text);
    /* No room for one name: the proposal is left out whole. */
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    pub->ksg_mask = 0x0307U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "TMO downlink" MID "encrypted (TEA1/TEA2/TEA3/proprietary KSG 8/proprietary KSG 9)")
           == 0);

    /* Direct mode belongs to no trunked network's uplink. */
    pub->mode = DSD_TETRA_MODE_DMO_DIRECT;
    pub->ksg_mask = 0U;
    pub->air_encryption = DSD_TETRA_AIR_ENC_CLEAR;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.mode_text, "DMO direct" MID "clear") == 0);
    fixture_close(&f);
}

/* The Security row: the cell's broadcast security class(es), its SCKN and authentication, whether it offers air
 * encryption at all, and the loaded key that applies to the carrier being received; in direct mode the transmission's
 * security class and SCKN. */
static void
test_security(void) {
    fixture f = fixture_open();
    dsd_tetra_publication* const pub = &f.state->tetra_pub;
    dsd_app_tetra view;
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.security_known == 0U && view.security_text[0] == '\0');

    pub->aie = DSD_TETRA_AIE_UNAVAILABLE;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.security_known == 1U && strcmp(view.security_text, "no air encryption") == 0);
    pub->aie = DSD_TETRA_AIE_AVAILABLE;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.security_text, "air encryption available") == 0);
    pub->sec_known = 1U;
    pub->sec_sckn = 3U;
    pub->sec_auth = 1U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.security_text, "class 2" MID "SCKN 3" MID "authentication") == 0);
    pub->sec_class1 = 1U;
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.security_text, "classes 1 and 2 - SCKN 3 - authentication") == 0);
    pub->sec_class3 = 1U;
    pub->sec_sckn = 0U;
    pub->sec_auth = 0U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.security_text, "classes 1 and 3") == 0);
    pub->sec_class1 = 0U;
    pub->key_kind = DSD_TETRA_KEY_CK;
    pub->key_tea = 2U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.security_text, "class 3" MID "TEA2 key") == 0);
    pub->key_kind = DSD_TETRA_KEY_TEA1_REDUCED;
    pub->key_tea = 1U;
    pub->aie = DSD_TETRA_AIE_UNKNOWN; /* no D-MLE-SYSINFO yet: the class bits mean nothing without it */
    pub->sec_auth = 1U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.security_text, "authentication" MID "TEA1 reduced key") == 0);
    pub->aie = DSD_TETRA_AIE_UNAVAILABLE;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.security_text, "no air encryption" MID "authentication" MID "TEA1 reduced key") == 0);

    /* Direct mode: the transmission's class, only. */
    DSD_MEMSET(pub, 0, sizeof *pub);
    pub->mode = DSD_TETRA_MODE_DMO_DIRECT;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1 && view.security_known == 0U);
    pub->dmo_class = DSD_TETRA_DMO_CLASS_DM1;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1 && strcmp(view.security_text, "DM-1") == 0);

    static const struct {
        uint8_t dmo_class;
        const char* text;
    } k_classes[] = {
        {DSD_TETRA_DMO_CLASS_DM2A, "DM-2-A" MID "SCKN 5"},
        {DSD_TETRA_DMO_CLASS_DM2B, "DM-2-B" MID "SCKN 5"},
        {DSD_TETRA_DMO_CLASS_DM2C, "DM-2-C" MID "SCKN 5"},
    };

    pub->dmo_sckn = 5U;
    for (size_t i = 0; i < sizeof k_classes / sizeof k_classes[0]; i++) {
        pub->dmo_class = k_classes[i].dmo_class;
        assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
        assert(strcmp(view.security_text, k_classes[i].text) == 0);
    }
    pub->dmo_class = 9U; /* a class this build does not know */
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1 && view.security_known == 0U);
    fixture_close(&f);
}

/* The network a CRC-checked BSCH on this carrier sent (the received identity record), its colour code named as the call
 * history does ("CC 25", decimal). A --tetra-net preset or a network the decoder kept is configuration: no row. */
static void
test_network(void) {
    fixture f = fixture_open();
    dsd_tetra_publication* const pub = &f.state->tetra_pub;
    dsd_app_tetra view;
    pub->mode = DSD_TETRA_MODE_DMO_DIRECT;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.network_known == 0U && view.network_text[0] == '\0');
    /* What a --tetra-net preset leaves in the publication. */
    pub->mode = DSD_TETRA_MODE_TMO_DOWNLINK;
    pub->net_known = 1U;
    pub->mcc = 262U;
    pub->mnc = 1001U;
    pub->colour = 0x19U;
    pub->la_known = 1U;
    pub->la = 77U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.network_known == 0U && view.network_text[0] == '\0');
    dsd_rxid_note_tetra_cell(f.state, 262U, 1001U, 0x19U);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.network_known == 1U);
    assert(strcmp(view.network_text, "MCC 262" MID "MNC 1001" MID "CC 25") == 0);
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.network_text, "MCC 262 - MNC 1001 - CC 25") == 0);
    dsd_rxid_note_tetra_cell(f.state, 1023U, 16383U, 63U);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.network_text, "MCC 1023" MID "MNC 16383" MID "CC 63") == 0);
    /* The location area and the carrier being received, once known: every field a TEA1 reduced key is bound to. */
    dsd_rxid_note_tetra_la(f.state, 16383U);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.network_text, "MCC 1023" MID "MNC 16383" MID "LA 16383" MID "CC 63") == 0);
    pub->rx_carrier_known = 1U;
    pub->rx_carrier = 4095U;
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(strcmp(view.network_text, "MCC 1023" MID "MNC 16383" MID "LA 16383" MID "CC 63" MID "carrier 4095") == 0);
    /* An accepted trunking retune keeps the network and drops the colour code until this carrier's BSCH. */
    dsd_rxid_trunk_tune(f.state);
    assert(dsd_app_tetra_view(f.opts, f.state, DSD_APP_TETRA_ASCII, &view) == 1);
    assert(strcmp(view.network_text, "MCC 1023 - MNC 16383 - LA 16383 - carrier 4095") == 0);
    /* A checked direct mode burst: no trunked network. */
    dsd_rxid_note_tetra_direct_mode(f.state);
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.network_known == 0U && view.network_text[0] == '\0');
    /* A carrier boundary forgets the network. */
    dsd_rxid_note_tetra_cell(f.state, 262U, 1001U, 0x19U);
    dsd_rxid_reset(f.state);
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
    assert(dsd_app_tetra_view(f.opts, f.state, 0U, &view) == 1);
    assert(view.codec_fault == 1U && strcmp(view.codec_text, "decode errors") == 0);
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
    test_mode_line_proposed();
    test_security();
    test_network();
    test_channels();
    test_mm_status_and_codec();
    test_publication_reset();
    return 0;
}
