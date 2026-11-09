// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Private engine trunk-scan test support.
 */

#ifndef DSD_NEO_SRC_ENGINE_TRUNK_SCAN_INTERNAL_H_
#define DSD_NEO_SRC_ENGINE_TRUNK_SCAN_INTERNAL_H_

#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>

#if defined(DSD_TRUNK_SCAN_TEST_CLOCK)
void trunk_scan_test_set_now(double now_m);
void trunk_scan_test_clear_now(void);
int trunk_scan_test_target_embedded_keys_cleared(const dsd_state* state, size_t index);
/* How many times the visit cap's and the receive expiry's alternate walk (trunk_scan_list_has_alternate()) has looked
   at a target that no list switched on holds, all walks counted since the process started: one step per such look,
   whoever owns the target. A target some list that is on holds is never counted, its owner off or not, so a walk that
   keeps to the lists on adds nothing here. */
size_t trunk_scan_test_alternate_walk_off_list_steps(void);

/* A loaded list's layout (issue #697), as the coordinator keeps it: its own run of targets and every target its file
   holds (row order). */
typedef struct {
    size_t first;
    size_t count;
    const size_t* members;
    size_t member_count;
} trunk_scan_test_layout;

/* List @p l's (0-based) layout into @p out; -1 without a coordinator or that list. */
int trunk_scan_test_list_layout(const dsd_state* state, int l, trunk_scan_test_layout* out);
/* The lists target @p index is in (bit l for list l) and its owner list (0-based) into @p owner; -1 without it. */
int trunk_scan_test_target_lists(const dsd_state* state, size_t index, unsigned int* owner);
#endif

#endif /* DSD_NEO_SRC_ENGINE_TRUNK_SCAN_INTERNAL_H_ */
