// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #743: the input-spec predicates for the raw I/Q stream kinds (`iqtcp:` / `iqudp:`) and the tunability
 * predicates every "can this input retune?" decision keys on: an I/Q stream, fixed tuning (replay or stream), radio
 * tunability (DSD-neo moves the receiver), session tunability (the radio on a radio input, else the caller's rigctl
 * peer) and a source-fixed rate. Each reads the input type and device string in force, never the session's replay
 * flags, which outlive a replay that a config apply moved onto a live radio.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

typedef struct {
    const char* dev;
    int type;
    int radio_spec;   /* dsd_opts_audio_in_dev_is_radio_spec() */
    int stream;       /* dsd_opts_input_is_iq_stream() */
    int fixed;        /* dsd_opts_input_has_fixed_tuning(), and dsd_opts_input_rate_is_fixed() */
    int radio_tune;   /* dsd_opts_radio_can_tune() */
    int session_peer; /* dsd_opts_session_can_tune() with a peer that can tune; without one it is radio_tune */
} tunability_row;

static const tunability_row k_rows[] = {
    /* Radio inputs DSD-neo tunes. */
    {"rtl", AUDIO_IN_RTL, 1, 0, 0, 1, 1},
    {"rtl:0:851.0125M", AUDIO_IN_RTL, 1, 0, 0, 1, 1},
    {"rtltcp:127.0.0.1:1234", AUDIO_IN_RTL, 1, 0, 0, 1, 1},
    {"soapy:driver=airspy", AUDIO_IN_RTL, 1, 0, 0, 1, 1},
    {"airspy", AUDIO_IN_RTL, 1, 0, 0, 1, 1},
    /* Fixed tuning: a replay, and a stream another program sends. A peer beside them moves nothing the session hears. */
    {"iqreplay:capture.iq.json", AUDIO_IN_RTL, 1, 0, 1, 0, 0},
    {"iqreplay", AUDIO_IN_RTL, 1, 0, 1, 0, 0},
    {"iqtcp:127.0.0.1:5555:240k:cs16", AUDIO_IN_RTL, 1, 1, 1, 0, 0},
    {"iqtcp:sdr.local:5555:1.152M:cf32:851012500", AUDIO_IN_RTL, 1, 1, 1, 0, 0},
    {"iqtcp", AUDIO_IN_RTL, 1, 1, 1, 0, 0},
    {"iqudp:0.0.0.0:7356:48k:cu8", AUDIO_IN_RTL, 1, 1, 1, 0, 0},
    {"iqudp", AUDIO_IN_RTL, 1, 1, 1, 0, 0},
    /* Names that only start like a stream kind are none, and an unknown device under AUDIO_IN_RTL is a tunable RTL. */
    {"iqtcpx:127.0.0.1:5555:240k:cs16", AUDIO_IN_RTL, 0, 0, 0, 1, 1},
    {"iqudp6:0.0.0.0:7356:48k:cu8", AUDIO_IN_RTL, 0, 0, 0, 1, 1},
    {"", AUDIO_IN_RTL, 0, 0, 0, 1, 1},
    /* A spec the input type does not run is no input in force: a peer behind the audio answers. */
    {"iqtcp:127.0.0.1:5555:240k:cs16", AUDIO_IN_WAV, 1, 0, 0, 0, 1},
    {"iqudp:0.0.0.0:7356:48k:cu8", AUDIO_IN_PULSE, 1, 0, 0, 0, 1},
    {"iqreplay:capture.iq.json", AUDIO_IN_WAV, 1, 0, 0, 0, 1},
    /* PCM and symbol inputs. */
    {"pulse", AUDIO_IN_PULSE, 0, 0, 0, 0, 1},
    {"tcp:127.0.0.1:7355", AUDIO_IN_TCP, 0, 0, 0, 0, 1},
    {"udp:127.0.0.1:7355", AUDIO_IN_UDP, 0, 0, 0, 0, 1},
    {"m17udp:127.0.0.1:17000", AUDIO_IN_NULL, 0, 0, 0, 0, 1},
    {"-", AUDIO_IN_STDIN, 0, 0, 0, 0, 1},
    {"capture.wav", AUDIO_IN_WAV, 0, 0, 0, 0, 1},
    {"capture.bin", AUDIO_IN_SYMBOL_BIN, 0, 0, 0, 0, 1},
};

static int
expect_eq(const char* what, const tunability_row* row, int use_rigctl, int got, int want) {
    if (got == want) {
        return 0;
    }
    DSD_FPRINTF(stderr, "FAIL %s: dev \"%s\" type %d rigctl %d: got %d want %d\n", what, row->dev, row->type,
                use_rigctl, got, want);
    return 1;
}

/* The site's own peer test, as the control-channel hunt asks it: rigctl configured. */
static int
session_can_tune_at_hunt(const dsd_opts* opts) {
    return dsd_opts_session_can_tune(opts, opts->use_rigctl == 1);
}

static int
check_row(dsd_opts* opts, const tunability_row* row, int use_rigctl) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->audio_in_type = row->type;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", row->dev);
    opts->use_rigctl = use_rigctl;
    /* The replay flags a replay session keeps after it leaves the replay decide none of these. */
    opts->iq_replay_requested = 1;
    opts->iq_replay_active = 1;

    int rc = 0;
    rc |= expect_eq("radio spec", row, use_rigctl, dsd_opts_audio_in_dev_is_radio_spec(row->dev), row->radio_spec);
    rc |= expect_eq("iq stream", row, use_rigctl, dsd_opts_input_is_iq_stream(opts), row->stream);
    rc |= expect_eq("fixed tuning", row, use_rigctl, dsd_opts_input_has_fixed_tuning(opts), row->fixed);
    rc |= expect_eq("rate fixed", row, use_rigctl, dsd_opts_input_rate_is_fixed(opts), row->fixed);
    rc |= expect_eq("radio can tune", row, use_rigctl, dsd_opts_radio_can_tune(opts), row->radio_tune);
    /* Off a radio input the hunt's peer test answers: a configured rigctl peer. */
    int want_session = row->radio_tune;
    if (row->type != AUDIO_IN_RTL) {
        want_session = use_rigctl ? row->session_peer : 0;
    }
    rc |= expect_eq("session can tune", row, use_rigctl, session_can_tune_at_hunt(opts), want_session);
    rc |= expect_eq("session can tune (peer)", row, use_rigctl, dsd_opts_session_can_tune(opts, 1), row->session_peer);
    rc |= expect_eq("session can tune (no peer)", row, use_rigctl, dsd_opts_session_can_tune(opts, 0), row->radio_tune);
    return rc;
}

static int
test_predicate_table(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    assert(opts != NULL);
    int rc = 0;
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++) {
        rc |= check_row(opts, &k_rows[i], 0);
        rc |= check_row(opts, &k_rows[i], 1);
    }
    free(opts);
    return rc;
}

/* The spec predicates match the prefixes the network I/Q parser reads (`iqtcp:` / `iqudp:`), exactly or with a colon,
   and nothing else; NULL is no spec. */
static void
test_stream_spec_predicates(void) {
    assert(dsd_opts_audio_in_dev_is_iqtcp_spec("iqtcp:127.0.0.1:5555:240k:cs16") == 1);
    assert(dsd_opts_audio_in_dev_is_iqtcp_spec("iqtcp") == 1);
    assert(dsd_opts_audio_in_dev_is_iqtcp_spec("iqudp:0.0.0.0:7356:48k:cu8") == 0);
    assert(dsd_opts_audio_in_dev_is_iqtcp_spec("iqtcpx") == 0);
    assert(dsd_opts_audio_in_dev_is_iqtcp_spec("tcp:127.0.0.1:7355") == 0);
    assert(dsd_opts_audio_in_dev_is_iqtcp_spec(NULL) == 0);
    assert(dsd_opts_audio_in_dev_is_iqudp_spec("iqudp:0.0.0.0:7356:48k:cu8") == 1);
    assert(dsd_opts_audio_in_dev_is_iqudp_spec("iqudp") == 1);
    assert(dsd_opts_audio_in_dev_is_iqudp_spec("iqtcp:127.0.0.1:5555:240k:cs16") == 0);
    assert(dsd_opts_audio_in_dev_is_iqudp_spec("iqudp6") == 0);
    assert(dsd_opts_audio_in_dev_is_iqudp_spec("udp:127.0.0.1:7355") == 0);
    assert(dsd_opts_audio_in_dev_is_iqudp_spec(NULL) == 0);
    assert(dsd_opts_audio_in_dev_is_radio_spec(NULL) == 0);
    /* The PCM socket kinds are no stream kind, nor the reverse. */
    assert(dsd_opts_audio_in_dev_is_tcp_spec("iqtcp:127.0.0.1:5555:240k:cs16") == 0);
    assert(dsd_opts_audio_in_dev_is_udp_spec("iqudp:0.0.0.0:7356:48k:cu8") == 0);
}

static void
test_null_opts(void) {
    assert(dsd_opts_input_is_iq_stream(NULL) == 0);
    assert(dsd_opts_input_has_fixed_tuning(NULL) == 0);
    assert(dsd_opts_radio_can_tune(NULL) == 0);
    assert(dsd_opts_session_can_tune(NULL, 1) == 0);
    assert(dsd_opts_session_can_tune(NULL, 0) == 0);
    assert(dsd_opts_input_rate_is_fixed(NULL) == 0);
}

int
main(void) {
    test_stream_spec_predicates();
    test_null_opts();
    return test_predicate_table();
}
