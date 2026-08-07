// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The UI's single poll driver and the object graph QML binds to.
 *
 * Binding threading contract: every snapshot-backed read in the process happens on
 * this timer, on the Qt main thread. The app-control consume accessors hand back a
 * shared buffer after dropping their lock, so a second concurrent reader sees rows
 * mid-overwrite. One polling thread, no exceptions.
 */

#ifndef DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_
#define DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_

#include <QObject>
#include <QTimer>

#include "decoder_host.h"

namespace dsd_qt {

class CallHistoryModel;
class CommandBridge;
class EventLogModel;
class MetricsModel;

class UiController : public QObject {
    Q_OBJECT
    Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs NOTIFY pollIntervalChanged)

  public:
    UiController(DecoderHost* host, MetricsModel* metrics, EventLogModel* events, CallHistoryModel* history,
                 QObject* parent = nullptr);
    ~UiController() override;

    int pollIntervalMs() const;
    void setPollIntervalMs(int interval_ms);

    void start();
    void stop();

  Q_SIGNALS:
    void pollIntervalChanged();

  private:
    void tick();
    void onSessionStateChanged();

    DecoderHost* m_host = nullptr;
    MetricsModel* m_metrics = nullptr;
    EventLogModel* m_events = nullptr;
    CallHistoryModel* m_history = nullptr;
    QTimer m_timer;
    DecoderHost::SessionState m_session = DecoderHost::Idle;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_ */
