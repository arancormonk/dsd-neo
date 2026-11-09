// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** @file Failure seams of json_store, built only with DSD_NEO_TEST_HOOKS: a read error part way through a file has no
 * portable trigger, and a replacement that fails after the move needs the path changed between the two steps. */
#ifndef DSD_NEO_SRC_UI_QT_JSON_STORE_TEST_HOOKS_H_
#define DSD_NEO_SRC_UI_QT_JSON_STORE_TEST_HOOKS_H_

#include <QString>
#include <functional>

namespace dsd_qt {

/** @brief Reads of @p fileName report a read error after reading the whole file; an empty name reads normally. */
void json_store_test_fail_read(const QString& fileName);

/**
 * @brief Called right after a marked file was renamed aside, with its path and its kept path; empty for none.
 *
 * It runs with json_store's registry lock held, so it must not call any json_store function.
 */
void json_store_test_after_aside(std::function<void(const QString& path, const QString& keptPath)> hook);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_JSON_STORE_TEST_HOOKS_H_ */
