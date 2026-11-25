// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Whether a call recording transfer's selection still stands (issue #902), checked on the thread that reads its
 * files: RecordingStore::selectionToken() makes one, DecoderHost's transfers check it.
 */

#ifndef DSD_NEO_SRC_UI_QT_RECORDING_SELECTION_TOKEN_H_
#define DSD_NEO_SRC_UI_QT_RECORDING_SELECTION_TOKEN_H_

#include <QtGlobal>
#include <atomic>
#include <memory>
#include <utility>

namespace dsd_qt {

/**
 * @brief Stands while the store that made it has not started reading storage again since, and goes stale once it
 * has, or once the store is gone: it holds the store's count of those reads, never the store. A default one always
 * stands (a host with no store).
 */
class RecordingSelectionToken {
  public:
    RecordingSelectionToken() = default;

    RecordingSelectionToken(std::shared_ptr<const std::atomic<quint64>> reads, quint64 seen)
        : m_reads(std::move(reads)), m_seen(seen) {}

    bool
    current() const {
        return !m_reads || m_reads->load(std::memory_order_acquire) == m_seen;
    }

  private:
    std::shared_ptr<const std::atomic<quint64>> m_reads;
    quint64 m_seen = 0U;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_RECORDING_SELECTION_TOKEN_H_ */
