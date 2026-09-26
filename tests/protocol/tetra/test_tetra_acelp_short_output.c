// SPDX-License-Identifier: GPL-3.0-or-later
/* A short vocoder response must never be routed as a partial audio frame. */
#include <dsd-neo/protocol/tetra/tetra_acelp.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>

#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
    static const char path[] = "tetra_acelp_short_output_test.wav";
    SF_INFO info = {0};
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    remove(path);
    SNDFILE *wav = sf_open(path, SFM_WRITE, &info);
    dsd_opts *opts = calloc(1, sizeof(*opts));
    dsd_state *state = calloc(1, sizeof(*state));
    if (!wav || !opts || !state) {
        if (wav) sf_close(wav);
        free(opts);
        free(state);
        remove(path);
        return 1;
    }

    opts->wav_out_f = wav;
    opts->dmr_stereo_wav = 1;
    opts->audio_out_fd = -1;
    uint8_t type2[292] = {0};
    tetra_acelp_process_tch(type2, 271, 0, opts, state);
    int rejected_activity_ok = state->last_vc_sync_time == 0
                            && state->last_vc_sync_time_m == 0.0;
    state->tetra_vc_assignment_valid = 1;
    state->tetra_vc_timeslot_bitmap = 8; /* TN1 assigned, receiving TN2. */
    state->tetra_tdma_valid = 1;
    state->tetra_tn = 2;
    tetra_acelp_process_tch(type2, 292, 0, opts, state);
    rejected_activity_ok = rejected_activity_ok && state->last_vc_sync_time == 0
                        && state->last_vc_sync_time_m == 0.0;
    state->tetra_tn = 1;
    tetra_acelp_process_tch(type2, 292, 0, opts, state);
    tetra_vocoder_close();
    sf_write_sync(wav);
    sf_count_t frames = sf_seek(wav, 0, SEEK_CUR);
    const char *cmd = getenv("TETRA_VOCODER_CMD");
    uint8_t expected_status = (!cmd || !cmd[0])
                                ? TETRA_VOCODER_STATUS_COMMAND_MISSING
                                : (strstr(cmd, "--hang")
                                     ? TETRA_VOCODER_STATUS_TIMEOUT
                                     : TETRA_VOCODER_STATUS_SHORT_OUTPUT);
    uint32_t expected_errors = expected_status == TETRA_VOCODER_STATUS_COMMAND_MISSING ? 0u : 1u;
    int status_ok = rejected_activity_ok && state->tetra_vocoder_status == expected_status
                 && state->last_vc_sync_time > 0
                 && state->last_vc_sync_time_m > 0.0
                 && state->tetra_vocoder_frames == 0
                 && state->tetra_vocoder_errors == expected_errors;
    sf_close(wav);
    free(opts);
    free(state);
    remove(path);

    if (frames != 0 || !status_ok) {
        fprintf(stderr, "FAIL: partial vocoder response wrote %lld PCM frames\n",
                (long long)frames);
        return 1;
    }
    puts("PASS test_tetra_acelp_short_output");
    return 0;
}
