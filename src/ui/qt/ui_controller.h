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

// Complete types are needed by inline Qt container and metatype instantiations.
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <type_traits> // IWYU pragma: keep

#include "decoder_host.h"

namespace dsd_qt {

class CallHistoryModel;
class MetricsModel;
class P25NetworkModel; // WP-F2
class DiagnosticsLogModel;
class TalkgroupListModel;
class CommandBridge;
class RecordingStatus;
class UnitActivityModel;
class PositionModel;

class UiController : public QObject {
    Q_OBJECT
    // WP-S2: QML supplies its complete overlay/pending-permission gate.
    Q_PROPERTY(bool autoStartBlocked MEMBER m_autoStartBlocked)
    // WP-D1: copied retained outcome, polled even without decoder redraw.
    Q_PROPERTY(QVariantMap talkgroupExportResult READ talkgroupExportResult NOTIFY talkgroupExportResultChanged)
    Q_PROPERTY(QVariantMap decryptionResult READ decryptionResult NOTIFY decryptionResultChanged)
    Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs NOTIFY pollIntervalChanged)

  public:
    UiController(DecoderHost* host, MetricsModel* metrics, CallHistoryModel* history, TalkgroupListModel* talkgroups,
                 QObject* parent = nullptr);
    ~UiController() override;
    void setCommandBridge(CommandBridge* commands);

    QVariantMap
    decryptionResult() const {
        return m_decryptionResult;
    }

    QVariantMap
    talkgroupExportResult() const {
        return m_talkgroupExportResult;
    }

    int pollIntervalMs() const;
    void setPollIntervalMs(int interval_ms);

    // WP-F5: process diagnostics refresh even when decoder redraw is idle.
    void
    setDiagnosticsLog(DiagnosticsLogModel* model) {
        m_diagnostics = model;
    }

    // WP-F2: sheet-only network refresh uses the same held snapshot.
    void
    setP25Network(P25NetworkModel* model) {
        m_network = model;
    }

    // The Activity screen's log: read from each redraw's held snapshot while a session runs, dropped at its edges.
    void
    setUnitActivity(UnitActivityModel* model) {
        m_unitActivity = model;
    }

    // Issue #800: the map's tracks, read on every tick (before the no-redraw return) and once more at Stop. Never in
    // clearLiveModels(): the tracks are kept for the app's life (D4).
    void
    setPositions(PositionModel* model) {
        m_positions = model;
    }

    // Issue #708: what call recording is doing, read every tick (the view stays readable when no redraw comes).
    void
    setRecordingStatus(RecordingStatus* status) {
        m_recordingStatus = status;
    }

    // WP-S2: called for a consumed host attachment, never on preference changes. @p attachedKind is the kind of
    // the receiver that attached ("usb" for an RTL-SDR, "airspy"); only a target on that kind starts (#647 D3a).
    // @p companions: the other lists a scan target was last started with (issue #697), passed on as they are; the
    // start resolves them.
    void requestAutoStart(bool enabled, bool onboardingDone, const QString& kind, const QString& uid,
                          const QVariantMap& target, const QString& attachedKind,
                          const QStringList& companions = QStringList());

    void start();
    void stop();

    /**
     * @brief Ingest the latest snapshot into the call history right now.
     *
     * For the one moment ordering matters: startSystem() is about to change the
     * session label, and any backlog the previous session committed since the
     * last tick must be attributed to that session, not the incoming one. Runs
     * on the Qt main thread like every other snapshot read, and leaves the
     * redraw flag alone so the regular tick still fires.
     */
    Q_INVOKABLE void flushHistory();

  Q_SIGNALS:
    void autoStartRequested(const QString& kind, const QString& uid, const QStringList& companions);
    void pollIntervalChanged();
    void talkgroupExportResultChanged();
    void decryptionResultChanged();

  private:
    bool sessionIsLive() const;
    void tick();
    /** @brief The redraw-driven part of tick(): metrics, network, history and talkgroups. */
    void refreshModels();
    /** @brief The models a redraw feeds from the held snapshot: network, history, talkgroups and unit activity. */
    void refreshRedrawModels(const dsd_opts* opts_snapshot, const dsd_state* snapshot, bool live, bool new_target);
    void pollTalkgroupExport();
    void pollDecryptionResult();
    /** @brief Reset the target-scoped models when the scan target changed; true when it did. */
    bool invalidateForTarget(const dsd_state* snapshot);
    void onSessionStateChanged();
    /** @brief Off screen (Suspended or Hidden) the GUI thread may read no snapshot for hours: its buffers keep no key
     *  until it is on screen again, reads made meanwhile included (withholdSnapshotKeys()). */
    void onApplicationStateChanged(Qt::ApplicationState state);
    /**
     * @brief Wipe the key material of the GUI thread's snapshot buffers (the state's keys, the options' API keys),
     * which this thread alone reads: a session stopped while it reads none (from the notification, with the app in the
     * background) would leave them there until its next read. That read copies them again while a session runs.
     */
    static void wipeSnapshotKeys();
    /**
     * @brief While @p withhold, the GUI thread's snapshot buffers hold no key material even across reads (a hidden
     * tick, another GUI-thread reader): dsd_app_snapshot_consumer_withhold_keys() and its options twin.
     */
    static void withholdSnapshotKeys(bool withhold);
    void clearLiveModels();

    bool m_autoStartBlocked = true; // Closed until Main.qml installs its binding.
    DecoderHost* m_host = nullptr;
    MetricsModel* m_metrics = nullptr;
    CallHistoryModel* m_history = nullptr;
    TalkgroupListModel* m_talkgroups = nullptr;
    DiagnosticsLogModel* m_diagnostics = nullptr;
    P25NetworkModel* m_network = nullptr; // WP-F2
    UnitActivityModel* m_unitActivity = nullptr;
    PositionModel* m_positions = nullptr;
    RecordingStatus* m_recordingStatus = nullptr;
    QTimer m_timer;
    QVariantMap m_talkgroupExportResult;
    quint64 m_talkgroupExportSequence = 0;
    QPointer<CommandBridge> m_commands;
    QVariantMap m_decryptionResult;
    quint64 m_decryptionSequence = 0;
    unsigned int m_active_ordinal = 0;
    DecoderHost::SessionState m_session = DecoderHost::Idle;
    /* A session ended since the last tick: the history is saved after that tick ingests. */
    bool m_historySaveOwed = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_UI_CONTROLLER_H_ */
