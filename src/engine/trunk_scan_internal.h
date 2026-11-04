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
/* The targets of lists switched off that the visit cap's and the receive expiry's alternate walk has looked at, all
   walks counted. */
size_t trunk_scan_test_alternate_walk_off_list_steps(void);
#endif

#endif /* DSD_NEO_SRC_ENGINE_TRUNK_SCAN_INTERNAL_H_ */
