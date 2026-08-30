// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Yaesu System Fusion (YSF) protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_YSF_YSF_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_YSF_YSF_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode one YSF frame.
 *
 * @return 1 once any FICH in this transmission has decoded -- Golay corrected and the
 *         CRC-16 over the corrected bits held -- and 0 until then. A FICH failure before
 *         the first success means the ~460 dibits this call goes on to consume were laid
 *         out by nothing read off the air, and the SPS hunt refuses them the dwell they
 *         would otherwise buy; a failure after it falls back to dt/fi a confirmed frame
 *         supplied and still produces audio, which is a decode (#391). The flag lives in
 *         dsd_state::ysf_fich_confirmed and clears with the rest of the YSF state.
 */
int processYSF(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_YSF_YSF_H_ */
