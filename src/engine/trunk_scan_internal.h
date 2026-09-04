// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Private engine trunk-scan test support.
 */

#ifndef DSD_NEO_SRC_ENGINE_TRUNK_SCAN_INTERNAL_H_
#define DSD_NEO_SRC_ENGINE_TRUNK_SCAN_INTERNAL_H_

#if defined(DSD_TRUNK_SCAN_TEST_CLOCK)
void trunk_scan_test_set_now(double now_m);
void trunk_scan_test_clear_now(void);
#endif

#endif /* DSD_NEO_SRC_ENGINE_TRUNK_SCAN_INTERNAL_H_ */
