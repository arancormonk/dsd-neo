// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** @file Seams of DecoderHost, built only with DSD_NEO_TEST_HOOKS: a recording that goes between a play's file check
 * and its start has no portable trigger. */
#ifndef DSD_NEO_SRC_UI_QT_DECODER_HOST_TEST_HOOKS_H_
#define DSD_NEO_SRC_UI_QT_DECODER_HOST_TEST_HOOKS_H_

#include <QString>
#include <functional>

namespace dsd_qt {

/** @brief Called with the path each time a recording play's file check finds the file there; empty for none. */
void decoder_host_test_after_recording_check(std::function<void(const QString& path)> hook);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_DECODER_HOST_TEST_HOOKS_H_ */
