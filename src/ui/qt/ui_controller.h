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
class HeardTalkgroupStore;
class MetricsModel;
class P25NetworkModel; // WP-F2
class DmrChannelsModel;
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
    // WP-D1: copied retained outcomes, polled even without decoder redraw. Issue #903: every export completion the
    // last poll read, oldest first ({results: [...], missedThrough}), so two finishing between polls both arrive;
    // missedThrough is the newest sequence that poll could not read (more finished than app-control keeps), else "0".
    // Each result's tuningMoved says the receiver's tuning had changed since the run began as the export ran
    // (dsd_app_tg_export_result::tuning_moved): its list was not heard on the session's start tuning.
    Q_PROPERTY(QVariantMap talkgroupExportResults READ talkgroupExportResults NOTIFY talkgroupExportResultsChanged)
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
    talkgroupExportResults() const {
        return m_talkgroupExportResults;
    }

    /** @brief Issue #903: the sequence of the newest talkgroup list export result app-control holds now ("0": none), never
     * older than the last poll read. An export queued after this read completes with a newer one. */
    Q_INVOKABLE QString talkgroupExportSequenceNow() const;

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

    // Discussion #900 F3: the Channels sheet's rows, read as the network is (only while the sheet is open), and read
    // from the held snapshot at once when the sheet opens.
    void setDmrChannels(DmrChannelsModel* model);

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

    // Issue #903: the remembered talkgroups, written at most every 30 s of real time from the tick and at once after a
    // session's last rows are ingested at Stop. It must outlive this controller.
    void
    setHeardTalkgroups(HeardTalkgroupStore* store) {
        m_heardTalkgroups = store;
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

    /**
     * @brief flushHistory() for Main's accepted start, before it begins the new session (issue #903): a ring no session
     * owns yet stays unbound (CallHistoryModel::refresh()), since it can only be the starting run's.
     */
    Q_INVOKABLE void flushHistoryBeforeStart();

  Q_SIGNALS:
    void autoStartRequested(const QString& kind, const QString& uid, const QStringList& companions);
    void pollIntervalChanged();
    void talkgroupExportResultsChanged();
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
    /** @brief The tick's write of the remembered talkgroups: at once when @p sessionEnded, else when one is due. */
    void flushHeardTalkgroups(bool sessionEnded);
    /** @brief Reset the target-scoped models when the scan target changed; true when it did. */
    bool invalidateForTarget(const dsd_opts* opts_snapshot, const dsd_state* snapshot);
    /** @brief The retained command answers read on every tick and at the session's end: the export, decryption and
     *  channel-set ones. */
    void pollCommandResults();
    /** @brief The Channels sheet opened: its rows from the held snapshot now, not at the next redraw. */
    void onDmrChannelsActiveChanged();
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
    DmrChannelsModel* m_dmrChannels = nullptr;
    UnitActivityModel* m_unitActivity = nullptr;
    PositionModel* m_positions = nullptr;
    RecordingStatus* m_recordingStatus = nullptr;
    HeardTalkgroupStore* m_heardTalkgroups = nullptr;
    QTimer m_timer;
    QVariantMap m_talkgroupExportResults;
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
