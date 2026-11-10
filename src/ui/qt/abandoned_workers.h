// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Where a teardown leaves a worker pool whose job did not stop in time.
 */

#ifndef DSD_NEO_SRC_UI_QT_ABANDONED_WORKERS_H_
#define DSD_NEO_SRC_UI_QT_ABANDONED_WORKERS_H_

#include <QList>

class QThreadPool;

namespace dsd_qt {

/**
 * @brief Keep @p pool, whose job ignored every stop, for the rest of the process.
 *
 * Deleting a pool waits for its job, so a teardown that stopped waiting hands the pool here instead: it stays,
 * reachable from nothing else and never deleted, until its job ends and the process exits. The import worker, the
 * backup's worker and the Android recording transfers share this one policy. Call it on the Qt thread.
 */
inline void
abandon_worker(QThreadPool* pool) {
    static auto* workers = new QList<QThreadPool*>();
    workers->append(pool);
}

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_ABANDONED_WORKERS_H_
