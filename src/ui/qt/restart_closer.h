// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Closes DSD-neo so that it can be opened again with new storage access (issue #830).
 *
 * Android lets a process read an invisible USB drive only when the process started while it held All files access,
 * so after the grant the file browser offers to close the app. Closing ends the process outright, so it must not cut a
 * copy short or leave a session to die with the engine: it is refused while an import, a replay copy or a backup job
 * runs, and a live session is stopped through the normal stop path and awaited, engine included, before the quit.
 */

#ifndef DSD_NEO_SRC_UI_QT_RESTART_CLOSER_H_
#define DSD_NEO_SRC_UI_QT_RESTART_CLOSER_H_

#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <dsd-neo/runtime/decode_clock.h>
#include <functional>

namespace dsd_qt {

class DecoderHost;

/**
 * @brief The close that precedes a restart; exposed to QML as `restartCloser`.
 *
 * begin() stops a live session and then polls every kPollMs until both sessionActive() and engineBusy() read false,
 * then asks the host to quit. A session still active kStopWaitMs after begin() ends the close as "stopFailed", and a
 * host that cannot quit ends it as "quitFailed"; either leaves the app open and the closer idle. While it is active,
 * Main holds every automatic start. Qt thread only.
 */
class RestartCloser : public QObject {
    Q_OBJECT
    /** True from begin() until a failure ends the close; a close that reached the quit stays active. */
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

  public:
    /** How often the session and engine are read while the close waits for them. */
    static constexpr int kPollMs = 100;
    /** How long a stopping session may take before the close gives up. */
    static constexpr uint64_t kStopWaitMs = 10000;

    /**
     * @param host the decoder host, which outlives this object.
     * @param importsBusy true while an import or a replay copy runs.
     * @param backupBusy true while a backup export or restore job runs.
     * @param nowMs the real-time clock the stop wait is measured on.
     */
    RestartCloser(DecoderHost* host, std::function<bool()> importsBusy, std::function<bool()> backupBusy,
                  QObject* parent = nullptr, std::function<uint64_t()> nowMs = &dsd_realtime_mono_ms);

    bool active() const;

    /** @brief Whether a close would be refused now: a copy or a backup job would be cut short. */
    Q_INVOKABLE bool refused() const;

    /**
     * @brief Begin closing: stop a live session, then quit once it and the engine have settled.
     * @return false, doing nothing, when refused() or a close is already active.
     */
    Q_INVOKABLE bool begin();

  Q_SIGNALS:
    void activeChanged();
    /** @brief How the close ended: "closing" (the app is going), "stopFailed" or "quitFailed" (it stays open). */
    void finished(const QString& outcome);

  private:
    void poll();
    void fail(const QString& outcome);

    DecoderHost* m_host;
    std::function<bool()> m_importsBusy;
    std::function<bool()> m_backupBusy;
    std::function<uint64_t()> m_nowMs;
    QTimer m_poll;
    uint64_t m_deadlineMs = 0;
    bool m_active = false;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_RESTART_CLOSER_H_
