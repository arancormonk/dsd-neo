// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Cross-thread atomic mirrors of demod-thread state.
 *
 * Kept separate from rtl_stream_shared.hpp so translation units whose
 * functions take a local `demod` parameter (rtl_demod_config.cpp) can declare
 * the mirrors without pulling in the `demod` global and shadowing it. Keep
 * this header private to src/io/radio/.
 */

#ifndef DSD_NEO_SRC_IO_RADIO_RTL_STREAM_MIRRORS_HPP_
#define DSD_NEO_SRC_IO_RADIO_RTL_STREAM_MIRRORS_HPP_

#include <atomic>

/* Mirror of demod.channel_pwr: written by the demod thread after each block,
 * read by the main thread via dsd_rtl_stream_return_pwr(). */
extern std::atomic<float> g_channel_pwr;

#endif /* DSD_NEO_SRC_IO_RADIO_RTL_STREAM_MIRRORS_HPP_ */
