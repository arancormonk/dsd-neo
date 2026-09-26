// SPDX-License-Identifier: GPL-3.0-or-later
/* TCH/FS uses the tracked burst TN rather than its coding-block index. */
#include <dsd-neo/protocol/tetra/tetra_acelp.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    dsd_opts *opts = calloc(1, sizeof(*opts));
    dsd_state *st = calloc(1, sizeof(*st));
    if (!opts || !st)
        return 1;
    if (!tetra_acelp_channel_gate_passes(opts)) {
        fputs("FAIL: conventional monitoring should pass speech\n", stderr);
        return 1;
    }
    opts->trunk_enable = 1;
    if (tetra_acelp_channel_gate_passes(opts)) {
        fputs("FAIL: trunk control channel must not play speech\n", stderr);
        return 1;
    }
    opts->trunk_is_tuned = 1;
    if (!tetra_acelp_channel_gate_passes(opts)) {
        fputs("FAIL: acquired traffic channel must pass speech\n", stderr);
        return 1;
    }
    if (!tetra_acelp_slot_gate_passes(0, st)) {
        fputs("FAIL: monitoring without an allocation must remain permissive\n", stderr);
        return 1;
    }

    st->tetra_vc_assignment_valid = 1;
    st->tetra_tdma_valid = 0;
    st->tetra_vc_timeslot_bitmap = 4;
    if (!tetra_acelp_slot_gate_passes(0, st)) {
        fputs("FAIL: allocation without TDMA timing must remain permissive\n", stderr);
        return 1;
    }

    st->tetra_tdma_valid = 1;
    for (unsigned tn = 1; tn <= 4; ++tn) {
        st->tetra_tn = (uint8_t)tn;
        for (unsigned assigned = 1; assigned <= 4; ++assigned) {
            st->tetra_vc_timeslot_bitmap = (uint8_t)(1u << (4u - assigned));
            int expected = tn == assigned;
            if (tetra_acelp_slot_gate_passes(0, st) != expected ||
                tetra_acelp_slot_gate_passes(2, st) != expected) {
                fprintf(stderr, "FAIL: TN%u assignment TN%u was gated incorrectly\n",
                        tn, assigned);
                free(st);
                return 1;
            }
        }
    }

    st->tetra_vc_timeslot_bitmap = 0;
    if (tetra_acelp_slot_gate_passes(0, st)) {
        fputs("FAIL: released zero-slot allocation must suppress audio\n", stderr);
        return 1;
    }
    if (!tetra_acelp_slot_gate_passes(0, NULL)) {
        fputs("FAIL: NULL state must remain permissive\n", stderr);
        return 1;
    }
    free(st);
    free(opts);
    puts("PASS test_tetra_tch_slot_gate");
    return 0;
}
