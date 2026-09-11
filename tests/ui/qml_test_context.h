// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Test doubles and QML context for UI_QT_QML_CALL_LISTS.
 *
 * The Q_OBJECT classes live in a header rather than beside main() so AUTOMOC
 * emits their meta-object code as its own generated translation unit. A
 * `#include "*.moc"` would pull that generated code into the test's own
 * translation unit, where the project's clang-tidy configuration would then
 * analyse moc output nobody can fix.
 *
 * What is real here and what is not: the view models are the production
 * CallHistoryFilterModel, TalkgroupListModel and TalkgroupFilterModel, so their
 * filtering and change signalling are under test. Behind them sits CallLogStore, a stand-in for CallHistoryModel that
 * prepends rows on demand — the real store only grows by ingesting a decoder
 * snapshot ring, which is covered by UI_QT_CALL_HISTORY_MODEL instead. The
 * engine-facing readings (metrics and the default decoderHost) use QObject-backed maps
 * rather than the production models, which is what keeps this test off
 * live engine lifecycle. WP-S1 additionally links the real CSV-validation facade
 * for pre-start scan-list checks; it never opens a tuner.
 *
 * That last choice gives up one guarantee, and missingContextKeys() buys it back:
 * reading a key a QVariantMap does not carry yields `undefined` with no warning
 * and no error, so an incomplete fixture would not fail on its own — a screen
 * that grew a binding on a reading missing here would pass this suite and render
 * `undefined` on the phone. tst_context_fixture.qml closes that by checking the
 * maps against the reads the screens under test actually contain.
 */

#ifndef DSD_NEO_TESTS_UI_QML_TEST_CONTEXT_H_
#define DSD_NEO_TESTS_UI_QML_TEST_CONTEXT_H_

#include <QAbstractListModel>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QXmlStreamReader>
#include <QtQuickTest>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <memory>
#include "../test_support/qt_test_paths.h"
#include "decryption_profiles_model.h"

#include "app_prefs.h"
#include "auto_start_policy.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "decode_mode_flag.h"
#include "decoder_host.h"
#include "diagnostics_log.h"
#include "imported_files_model.h"
#include "p25_network_model.h" // WP-F2
#include "qml_spectrum_stub.h"
#include "saved_systems_model.h"
#include "scan_list_starter.h"
#include "scan_lists_model.h"
#include "session_args.h"
#include "site_groups.h"
#include "spectrum_model.h"
#include "spectrum_view_item.h"
#include "talkgroup_filter_model.h"
#include "talkgroup_list_model.h"

using dsd_qt::CallHistoryFilterModel;
using dsd_qt::CallHistoryModel;

/**
 * @brief Minimal DecoderHost so ImportedFilesModel has one to copy through.
 *
 * Only importDocument() is exercised here, and that is DecoderHost's own
 * non-virtual desktop implementation. The lifecycle members exist because the
 * base class is abstract; the `decoderHost` the QML reads is still the plain map
 * below, which is what keeps the screens' session bindings drivable from a case.
 */
class ImportOnlyHost : public dsd_qt::DecoderHost {
    Q_OBJECT

  public:
    bool
    isRunning() const override {
        return m_running;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        if (acceptStart) {
            phase = startRunning ? Running : Starting;
            m_running = true;
            Q_EMIT runningChanged();
            Q_EMIT sessionStateChanged();
        }
        return acceptStart;
    }

    qint64 locationRequested = 0;
    qint64 locationCancelled = 0;

    void
    requestCurrentLocation(qint64 id) override {
        locationRequested = id;
    }

    void
    cancelLocationRequest(qint64 id) override {
        locationCancelled = id;
    }

    qreal keyboardBoundary = -1;

    qreal
    keyboardTop() const override {
        return keyboardBoundary;
    }

    bool delayedStop = false; // WP-D4: stop acknowledgement is a separate edge.

    void
    stop() override {
        phase = delayedStop ? Stopping : Idle;
        m_running = false;
        Q_EMIT runningChanged();
        Q_EMIT sessionStateChanged();
    }

    SessionState phase = Idle;

    SessionState
    sessionState() const override {
        return phase;
    }

    void
    setPhase(SessionState value) {
        phase = value;
        Q_EMIT sessionStateChanged();
    }

    // WP-S1: exercise the same brokered-USB gate as saved-system starts.
    bool
    localDeviceBrokered() const override {
        return usbBrokered;
    }

    bool
    localDeviceReady() const override {
        return usbReady;
    }

    void
    requestLocalDeviceAccess() override {
        ++usbRequests;
    }

    // WP-D5: rendering fixture; native error propagation has its own test.
    QString
    localDeviceStatus() const override {
        return usbStatus;
    }

    int
    localDeviceFailureKind() const override {
        return usbFailureKind;
    }

    QString usbStatus;
    int usbFailureKind = 0;

    bool usbBrokered = false;
    bool usbReady = false;
    int usbRequests = 0;

    bool acceptStart = false;
    bool startRunning = false;

  private:
    bool m_running = false;
};

// Matches Android's contract: Running can be observed before initialization.
// The ordinary import host inherits DecoderHost's fallback capability unchanged.
class InitializingHost : public ImportOnlyHost {
  public:
    bool
    signalsSessionInitialized() const override {
        return true;
    }
};

// The history model itself is already real in these fixtures; the controller
// stub only acknowledges the documented same-thread flush before a start.
class TestUiController : public QObject {
    Q_OBJECT
    // WP-S2: exercise Main.qml signal routing and overlay binding.
    Q_PROPERTY(bool autoStartBlocked MEMBER autoStartBlocked)
    // WP-D1: retained export result starts empty, like the real controller.
    Q_PROPERTY(QVariantMap talkgroupExportResult READ talkgroupExportResult CONSTANT)
    Q_PROPERTY(QVariantMap decryptionResult MEMBER decryptionResult NOTIFY decryptionResultChanged)
  public:
    using QObject::QObject;

    QVariantMap
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    talkgroupExportResult() const {
        return {};
    }

    QVariantMap decryptionResult;

    Q_INVOKABLE void
    finishDecryption(const QString& id, int status) {
        decryptionResult = {{"requestId", id}, {"session", "1"}, {"status", status}, {"scope", 0}};
        Q_EMIT decryptionResultChanged();
    }

    bool autoStartBlocked = true;

    Q_INVOKABLE void
    requestAutoStart(const QString& kind, const QString& uid) {
        Q_EMIT autoStartRequested(kind, uid);
    }

    Q_INVOKABLE void
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    flushHistory() {}

  Q_SIGNALS:
    void autoStartRequested(const QString& kind, const QString& uid);
    void decryptionResultChanged();
};

/**
 * @brief Stand-in for CommandBridge that records instead of submitting.
 *
 * The whole point of a tap-to-tune test is what the screen asks for, so the
 * command surface has to be observable. It carries every method the screens
 * call — an unimplemented one would only fail when some future case triggered
 * it, which is exactly the kind of gap this suite exists to close.
 *
 * The tune parameters are `unsigned int` because CommandBridge's are: QML hands
 * these methods a JavaScript number, and taking a double here would quietly
 * accept a fractional, negative or out-of-range frequency that production
 * truncates or wraps on its way to the tuner. Recording what production would
 * actually submit is the point.
 */
class CommandRecorder : public QObject {
    Q_OBJECT
    quint64 m_next_key_request = 1;
    QString m_last_key_request;
    QVariantList m_last_selection;

  public:
    Q_INVOKABLE QVariantMap
    decryptionContext(const QString& target, const QString& epoch) const {
        return {{"session", "1"}, {"tuning", "1"}, {"target", target}, {"keyEpoch", epoch}};
    }

    Q_INVOKABLE QString
    lastDecryptionRequest() const {
        return m_last_key_request;
    }

    Q_INVOKABLE QString
    applyDecryptionDraft(const QString& type, const QString& value, bool forceChanged, int force, const QVariantMap&) {
        if (!type.isEmpty() && !applyEncryptionKey(type, value)) {
            return {};
        }
        if (forceChanged && !setForceKeyMode(force == 33 ? 2 : force)) {
            return {};
        }
        m_last_key_request = QString::number(m_next_key_request++);
        return m_last_key_request;
    }

    Q_INVOKABLE QString
    applyDecryptionProfile(const QString&, int, const QVariantMap&) {
        m_last_key_request = QString::number(m_next_key_request++);
        return m_last_key_request;
    }

    Q_INVOKABLE QString
    applyDmrKeyMap(const QString&, int, const QVariantMap&) {
        m_last_key_request = QString::number(m_next_key_request++);
        return m_last_key_request;
    }

    // WP-D2: record only non-secret command outcomes, never key text.
    Q_INVOKABLE bool
    applyEncryptionKey(const QString& type, const QString& value) {
        ++m_key_apply_calls;
        m_key_payload_valid = dsd_qt::session_args_key_valid(type, value);
        return m_key_accepted;
    }

    Q_INVOKABLE bool
    setForceKeyMode(int mode) {
        m_last_force_mode = mode;
        return m_key_accepted;
    }

    Q_INVOKABLE int
    keyApplyCalls() const {
        return m_key_apply_calls;
    }

    Q_INVOKABLE bool
    keyPayloadValid() const {
        return m_key_payload_valid;
    }

    Q_INVOKABLE int
    lastForceMode() const {
        return m_last_force_mode;
    }

    Q_INVOKABLE void
    setKeyAccepted(bool accepted) {
        m_key_accepted = accepted;
    }

    Q_INVOKABLE bool
    importSrcList(const QString& path) {
        m_src_import_calls++;
        m_last_src_path = path;
        return true;
    }

    Q_INVOKABLE bool
    clearSrcList() {
        m_src_clear_calls++;
        m_last_src_path.clear();
        return true;
    }

    Q_INVOKABLE int
    srcImportCalls() const {
        return m_src_import_calls;
    }

    Q_INVOKABLE int
    srcClearCalls() const {
        return m_src_clear_calls;
    }

    Q_INVOKABLE QString
    lastSrcPath() const {
        return m_last_src_path;
    }

    Q_INVOKABLE bool
    manualTuneHz(unsigned int hz) {
        m_manual_tune_calls++;
        m_last_manual_tune_hz = hz;
        return true;
    }

    /* Accepted and discarded: nothing under test asserts on the settings-menu
     * tune, only on the spectrum's manualTuneHz(). Counting it would be state no
     * assertion can ever fail on. */
    Q_INVOKABLE bool
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    tuneHz(unsigned int hz) const {
        (void)hz;
        return true;
    }

    Q_INVOKABLE bool
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    toggleMute() const {
        return true;
    }

    Q_INVOKABLE bool
    holdTalkgroup(double tg) {
        ++m_hold_calls;
        m_last_hold_tg = tg;
        return true;
    }

    Q_INVOKABLE bool
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    lockoutSlot(int) const {
        return true;
    }

    // WP-D1: retain the captured version and requested fields for edit-sheet tests.
    Q_INVOKABLE bool
    setTalkgroupPolicy(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                       const QVariantMap& changes) {
        m_talkgroupEdit = {start, end, context, generation, changes};
        return true;
    }

    Q_INVOKABLE bool
    renameTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                    const QString& name) {
        m_talkgroupEdit = {start, end, context, generation, name};
        return true;
    }

    Q_INVOKABLE bool
    addTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                 const QString& name, bool listen, int priority, bool preempt) {
        m_talkgroupEdit = {start, end, context, generation, name, listen, priority, preempt};
        return true;
    }

    Q_INVOKABLE bool
    removeTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation) {
        m_talkgroupEdit = {start, end, context, generation};
        return true;
    }

    Q_INVOKABLE bool
    saveTalkgroupList(const QString& context, unsigned int generation, const QString& path) {
        m_talkgroupEdit = {context, generation, path};
        return true;
    }

    Q_INVOKABLE QVariantList
    lastTalkgroupEdit() const {
        return m_talkgroupEdit;
    }

    Q_INVOKABLE bool
    setTalkgroupListening(double idStart, double idEnd, bool listen) {
        m_talkgroup_listen_calls++;
        m_last_talkgroup_id_start = idStart;
        m_last_talkgroup_id_end = idEnd;
        m_last_talkgroup_listen = listen;
        return true;
    }

    Q_INVOKABLE bool
    setTalkgroupSelection(bool listen, const QString&, unsigned int, const QVariantList& rows) {
        m_last_selection = rows;
        return setAllTalkgroupsListening(listen, QString());
    }

    Q_INVOKABLE QVariantList
    selectionRows() const {
        return m_last_selection;
    }

    Q_INVOKABLE bool
    setAllTalkgroupsListening(bool listen, const QString& tag) {
        m_all_talkgroups_listen_calls++;
        m_last_all_talkgroups_listen = listen;
        m_last_all_talkgroups_tag = tag;
        return true;
    }

    Q_INVOKABLE bool
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    clearEncLockouts() const {
        return true;
    }

    /* On-the-fly scan controls (#380). Counted, so a case can assert that the
     * monitor's buttons send the command they are labelled with. */
    Q_INVOKABLE bool
    toggleScanHold() {
        m_scan_hold_calls++;
        return true;
    }

    Q_INVOKABLE bool
    avoidCurrentChannel() {
        m_scan_avoid_calls++;
        return true;
    }

    Q_INVOKABLE bool
    clearScanAvoids() {
        m_scan_avoid_clear_calls++;
        return true;
    }

    Q_INVOKABLE bool
    nextChannel() {
        m_next_channel_calls++;
        return true;
    }

    Q_INVOKABLE bool
    releaseTuner() {
        m_release_tuner_calls++;
        return true;
    }

    Q_INVOKABLE bool
    setTrunking(bool on) {
        m_set_trunking_calls++;
        m_last_set_trunking = on;
        return true;
    }

    Q_INVOKABLE bool
    setTunerGain(int gain_db) {
        m_last_gain_db = gain_db;
        m_gain_calls++;
        return true;
    }

    Q_INVOKABLE bool
    setSquelchDb(double db) {
        m_last_squelch_db = db;
        m_squelch_calls++;
        return true;
    }

    Q_INVOKABLE bool
    setPpm(int ppm) {
        m_last_ppm = ppm;
        return true;
    }

    Q_INVOKABLE bool
    setModulation(int modulation) {
        m_last_modulation = modulation;
        return true;
    }

    Q_INVOKABLE bool
    setDecodeMode(int mode) {
        m_last_decode_mode = mode;
        return true;
    }

    /* The production mapping itself, not a stand-in for it. It used to be a
     * stand-in answering numbers no dsdneoUserDecodeMode has -- 5 for DMR (which
     * is 4) and 13 for "the rest" (which is ANALOG) -- so the case asserting that
     * the DMR chip sends DMR was really asserting the double's own arithmetic. */
    Q_INVOKABLE int
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    decodeModeForFlag(const QString& flag) {
        return dsd_qt::decode_mode_for_flag(flag);
    }

    Q_INVOKABLE int
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    cycleHistoryMode() {
        return 0;
    }

    void
    reset() {
        m_hold_calls = 0;
        m_last_hold_tg = 0;
        m_key_apply_calls = 0;
        m_key_payload_valid = false;
        m_key_accepted = true;
        m_last_force_mode = -1;
        m_src_import_calls = 0;
        m_src_clear_calls = 0;
        m_last_src_path.clear();
        m_manual_tune_calls = 0;
        m_last_manual_tune_hz = 0U;
        m_release_tuner_calls = 0;
        m_set_trunking_calls = 0;
        m_last_set_trunking = false;
        m_gain_calls = 0;
        m_last_gain_db = -1;
        m_last_squelch_db = 0.0;
        m_squelch_calls = 0;
        m_last_modulation = -1;
        m_last_decode_mode = -1;
        m_last_ppm = 9999;
        m_scan_hold_calls = 0;
        m_scan_avoid_calls = 0;
        m_scan_avoid_clear_calls = 0;
        m_next_channel_calls = 0;
        m_talkgroupEdit.clear();
        m_talkgroup_listen_calls = 0;
        m_last_talkgroup_id_start = 0.0;
        m_last_talkgroup_id_end = 0.0;
        m_last_talkgroup_listen = false;
        m_all_talkgroups_listen_calls = 0;
        m_last_all_talkgroups_listen = false;
        m_last_all_talkgroups_tag.clear();
    }

    int
    scanHoldCalls() const {
        return m_scan_hold_calls;
    }

    int
    scanAvoidCalls() const {
        return m_scan_avoid_calls;
    }

    int
    scanAvoidClearCalls() const {
        return m_scan_avoid_clear_calls;
    }

    int
    nextChannelCalls() const {
        return m_next_channel_calls;
    }

    int
    gainCalls() const {
        return m_gain_calls;
    }

    int
    lastGainDb() const {
        return m_last_gain_db;
    }

    double
    lastSquelchDb() const {
        return m_last_squelch_db;
    }

    /* Lets a case assert that a step which cannot move made no request at all. */
    Q_INVOKABLE int
    squelchCalls() const {
        return m_squelch_calls;
    }

    int
    lastModulation() const {
        return m_last_modulation;
    }

    int
    lastDecodeMode() const {
        return m_last_decode_mode;
    }

    int
    lastPpm() const {
        return m_last_ppm;
    }

    int
    manualTuneCalls() const {
        return m_manual_tune_calls;
    }

    int
    releaseTunerCalls() const {
        return m_release_tuner_calls;
    }

    int
    setTrunkingCalls() const {
        return m_set_trunking_calls;
    }

    bool
    lastSetTrunking() const {
        return m_last_set_trunking;
    }

    /** @brief The recorded frequency, widened for QML's arithmetic. */
    double
    lastManualTuneHz() const {
        return static_cast<double>(m_last_manual_tune_hz);
    }

    int
    holdCalls() const {
        return m_hold_calls;
    }

    double
    lastHoldTg() const {
        return m_last_hold_tg;
    }

    int
    talkgroupListenCalls() const {
        return m_talkgroup_listen_calls;
    }

    double
    lastTalkgroupListenIdStart() const {
        return m_last_talkgroup_id_start;
    }

    double
    lastTalkgroupListenIdEnd() const {
        return m_last_talkgroup_id_end;
    }

    bool
    lastTalkgroupListenOn() const {
        return m_last_talkgroup_listen;
    }

    int
    allTalkgroupsListenCalls() const {
        return m_all_talkgroups_listen_calls;
    }

    bool
    lastAllTalkgroupsListenOn() const {
        return m_last_all_talkgroups_listen;
    }

    QString
    lastAllTalkgroupsTag() const {
        return m_last_all_talkgroups_tag;
    }

  private:
    int m_hold_calls = 0;
    double m_last_hold_tg = 0;
    int m_key_apply_calls = 0;
    bool m_key_payload_valid = false;
    bool m_key_accepted = true;
    int m_last_force_mode = -1;
    int m_src_import_calls = 0;
    int m_src_clear_calls = 0;
    QString m_last_src_path;
    int m_manual_tune_calls = 0;
    unsigned int m_last_manual_tune_hz = 0U;
    int m_release_tuner_calls = 0;
    int m_set_trunking_calls = 0;
    bool m_last_set_trunking = false;
    int m_gain_calls = 0;
    int m_scan_hold_calls = 0;
    int m_scan_avoid_calls = 0;
    int m_scan_avoid_clear_calls = 0;
    int m_next_channel_calls = 0;
    int m_last_gain_db = -1;
    double m_last_squelch_db = 0.0;
    int m_squelch_calls = 0;
    int m_last_modulation = -1;
    int m_last_decode_mode = -1;
    int m_last_ppm = 9999;
    QVariantList m_talkgroupEdit;
    int m_talkgroup_listen_calls = 0;
    double m_last_talkgroup_id_start = 0.0;
    double m_last_talkgroup_id_end = 0.0;
    bool m_last_talkgroup_listen = false;
    int m_all_talkgroups_listen_calls = 0;
    bool m_last_all_talkgroups_listen = false;
    QString m_last_all_talkgroups_tag;
};

/**
 * @brief Newest-first call log the tests drive directly.
 *
 * Same roles and the same newest-first prepend as CallHistoryModel, with
 * granular insert/reset signals — the screens' scroll behaviour is a reaction to
 * those signals, so a reset-everything stand-in would test nothing.
 */
class CallLogStore : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString sessionLabel READ sessionLabel WRITE setSessionLabel NOTIFY sessionLabelChanged)
    Q_PROPERTY(QString sessionUid READ sessionUid WRITE setSessionUid NOTIFY sessionUidChanged)
    Q_PROPERTY(QStringList systemLabels READ systemLabels NOTIFY countChanged)

  public:
    /** @brief One logged row, mirroring CallHistoryModel::Row's visible fields. */
    struct StoreRow {
        QString name;
        qulonglong tg = 0;
        qulonglong src = 0;
        bool enc = false;
        bool emergency = false;
        qint64 when = 0;
        int durationSecs = 4;
        QString systemName;
        QString systemUid;
        QString dayLabel;
        QString timeText;
        int kind = CallHistoryModel::KindVoice;
        QString detail;
        QString channel;
        QString sourceName;
    };

    int
    rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int
    count() const {
        return rowCount();
    }

    QString
    sessionLabel() const {
        return m_sessionLabel;
    }

    void
    setSessionLabel(const QString& label) {
        if (label == m_sessionLabel) {
            return;
        }
        m_sessionLabel = label;
        Q_EMIT sessionLabelChanged();
    }

    QString
    sessionUid() const {
        return m_sessionUid;
    }

    void
    setSessionUid(const QString& uid) {
        if (uid == m_sessionUid) {
            return;
        }
        m_sessionUid = uid;
        Q_EMIT sessionUidChanged();
    }

    QStringList
    systemLabels() const {
        return m_rows.isEmpty() ? QStringList() : QStringList{m_systemName};
    }

    QVariant
    data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }
        const StoreRow& row = m_rows.at(index.row());
        switch (role) {
            case CallHistoryModel::NameRole: return row.name;
            case CallHistoryModel::TgRole: return row.tg;
            case CallHistoryModel::SrcRole: return row.src;
            case CallHistoryModel::SourceNameRole: return row.sourceName;
            case CallHistoryModel::EmergencyRole: return row.emergency;
            case CallHistoryModel::EncRole: return row.enc;
            case CallHistoryModel::WhenRole: return row.when;
            case CallHistoryModel::DurationSecsRole: return row.durationSecs;
            case CallHistoryModel::SystemNameRole: return row.systemName;
            case CallHistoryModel::SystemUidRole: return row.systemUid;
            case CallHistoryModel::DayLabelRole: return row.dayLabel;
            case CallHistoryModel::TimeTextRole: return row.timeText;
            case CallHistoryModel::KindRole: return row.kind;
            case CallHistoryModel::DetailRole: return row.detail;
            case CallHistoryModel::ChannelRole: return row.channel;
            default: return {};
        }
    }

    QHash<int, QByteArray>
    roleNames() const override {
        return {{CallHistoryModel::NameRole, "name"},
                {CallHistoryModel::TgRole, "tg"},
                {CallHistoryModel::SrcRole, "src"},
                {CallHistoryModel::SourceNameRole, "srcName"},
                {CallHistoryModel::EncRole, "enc"},
                {CallHistoryModel::EmergencyRole, "emergency"},
                {CallHistoryModel::WhenRole, "when"},
                {CallHistoryModel::DurationSecsRole, "durationSecs"},
                {CallHistoryModel::SystemNameRole, "systemName"},
                {CallHistoryModel::SystemUidRole, "systemUid"},
                {CallHistoryModel::DayLabelRole, "dayLabel"},
                {CallHistoryModel::TimeTextRole, "timeText"},
                {CallHistoryModel::KindRole, "kind"},
                {CallHistoryModel::DetailRole, "detail"},
                {CallHistoryModel::ChannelRole, "channel"}};
    }

    /**
     * @brief Prepend one clear voice call under @p dayLabel.
     * @return The row's name, so a test can follow that one row up the list.
     */
    Q_INVOKABLE QString
    push(const QString& dayLabel) {
        StoreRow row;
        m_seq++;
        row.name = QStringLiteral("CALL %1").arg(m_seq, 3, 10, QLatin1Char('0'));
        row.tg = 1000 + static_cast<qulonglong>(m_seq);
        row.src = 200000 + static_cast<qulonglong>(m_seq);
        row.when = m_clock++;
        row.systemName = m_systemName;
        row.systemUid = m_sessionUid;
        row.dayLabel = dayLabel.isEmpty() ? QStringLiteral("TODAY") : dayLabel;
        row.timeText = QStringLiteral("12:%1").arg(m_seq % 60, 2, 10, QLatin1Char('0'));
        beginInsertRows(QModelIndex(), 0, 0);
        m_rows.prepend(row);
        endInsertRows();
        Q_EMIT countChanged();
        return row.name;
    }

    Q_INVOKABLE QString
    pushWithSourceName(const QString& name) {
        const QString call = push(QStringLiteral("TODAY"));
        m_rows[0].sourceName = name;
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx, {CallHistoryModel::SourceNameRole});
        return call;
    }

    Q_INVOKABLE QString
    pushEmergency() {
        const QString call = push(QStringLiteral("TODAY"));
        m_rows[0].emergency = true;
        const auto first = index(0);
        Q_EMIT dataChanged(first, first, {CallHistoryModel::EmergencyRole});
        return call;
    }

    /**
     * @brief Prepend one clear voice call heard on scan channel @p channel.
     * @return The row's name, so a test can find that one row.
     */
    Q_INVOKABLE QString
    pushOnChannel(const QString& dayLabel, const QString& channel) {
        const QString name = push(dayLabel);
        m_rows[0].channel = channel;
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx, {CallHistoryModel::ChannelRole});
        return name;
    }

    /**
     * @brief Prepend a call that decoded no talkgroup, named by the scan channel it
     * was heard on — encrypted traffic on a conventional list looks like this.
     * @return The row's name (the channel).
     */
    Q_INVOKABLE QString
    pushUnnamedOnChannel(const QString& dayLabel, const QString& channel) {
        push(dayLabel);
        m_rows[0].tg = 0;
        m_rows[0].name = channel;
        m_rows[0].channel = channel;
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx,
                           {CallHistoryModel::TgRole, CallHistoryModel::NameRole, CallHistoryModel::ChannelRole});
        return channel;
    }

    /** @brief Prepend @p n calls, oldest first, so the list reads newest-first. */
    Q_INVOKABLE void
    pushMany(int n, const QString& dayLabel) {
        for (int i = 0; i < n; i++) {
            push(dayLabel);
        }
    }

    /**
     * @brief Prepend one call and then stamp it encrypted, for the kind filter.
     *
     * Two steps rather than one, because that is the order the real thing happens
     * in: a row is logged and the ENC header lands on it afterwards. It also puts
     * the filter's dataChanged path under test, not only its insert path.
     */
    Q_INVOKABLE void
    pushEncrypted(const QString& dayLabel) {
        push(dayLabel);
        m_rows[0].enc = true;
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx, {CallHistoryModel::EncRole});
    }

    Q_INVOKABLE void
    clearAll() {
        beginResetModel();
        m_rows.clear();
        endResetModel();
        Q_EMIT countChanged();
    }

  Q_SIGNALS:
    void countChanged();
    void sessionLabelChanged();
    void sessionUidChanged();

  private:
    QList<StoreRow> m_rows;
    QString m_systemName = QStringLiteral("Test Site");
    QString m_sessionUid = QStringLiteral("test-system");
    QString m_sessionLabel = QStringLiteral("Test Site");
    int m_seq = 0;
    /* Fixed, ascending stamps: nothing here should depend on the wall clock. */
    qint64 m_clock = 1'700'000'000;
};

// WP-D3: keep the fixture readings and record the nearby button's actual invocation.
/** QObject-backed readings: Connections cannot target QVariantMap values.
 * Lifecycle tests still switch to the dedicated DecoderHost implementation. */
class ReadingMap : public QQmlPropertyMap {
    Q_OBJECT
  public:
    explicit ReadingMap(QObject* parent) : QQmlPropertyMap(this, parent) {}

  Q_SIGNALS:
    void tunerChanged();
    void controlChanged();
    void siteChanged();
    void qualityChanged();
    void runningChanged();
    void sessionStateChanged();
    void sessionInitialized();
    void localDeviceChanged();
    void typographyChanged();
    void keyboardChanged();
    void backRequested();
    void runResultChanged();
    void locationResult(qint64 requestId, bool fixOk, double lat, double lon, double accuracyM, qint64 fixAtMs,
                        bool geocodeOk, const QString& postal, const QString& country, const QString& error);
};

class RadioReferenceRecorder : public QQmlPropertyMap {
    Q_OBJECT

  public:
    explicit RadioReferenceRecorder(QObject* parent) : QQmlPropertyMap(this, parent) {}

    Q_INVOKABLE void
    lookupNearby() {
        nearbyCalls++;
        (void)nextLocationRequestId();
        insert(QStringLiteral("busy"), true);
    }

    Q_INVOKABLE qint64
    nextLocationRequestId() {
        cancel();
        static qint64 nextId = 0;
        Q_EMIT locationRequestAllocated(++nextId);
        return nextId;
    }

    Q_INVOKABLE void
    cancel() {
        insert(QStringLiteral("busy"), false);
    }

    int nearbyCalls = 0;
  Q_SIGNALS:
    void locationRequestAllocated(qint64 requestId);
};

/** @brief Installs the context the screens expect before any QML is loaded. */
class Setup : public QObject {
    Q_OBJECT

  public:
    ~Setup() override { dsd_state_ext_free_all(m_talkgroup_state.get()); }

    Q_INVOKABLE bool
    retainedKeyMatches(const QString& uid, const QString& expected) const {
        const dsd_qt::SavedSystemsModel systems;
        return systems.keyValueForUid(uid) == expected;
    }

    Q_INVOKABLE void
    pushDiagnostic(const QString& text) {
        dsd_qt::DiagnosticsLog::instance().submit(QStringLiteral("fixture"), QStringLiteral("info"), text);
    }

    Q_INVOKABLE bool
    pushTalkgroup(double id, const QString& mode, const QString& name, const QString& tags) {
        dsd_tg_policy_entry entry{};
        if (dsd_tg_policy_make_exact_entry(static_cast<uint32_t>(id), mode.toUtf8().constData(),
                                           name.toUtf8().constData(), DSD_TG_POLICY_SOURCE_IMPORTED, &entry)
            != 0) {
            return false;
        }
        DSD_SNPRINTF(entry.tags, sizeof(entry.tags), "%s", tags.toUtf8().constData());
        if (dsd_tg_policy_append_exact(m_talkgroup_state.get(), &entry) != 0) {
            return false;
        }
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
        return true;
    }

    Q_INVOKABLE void
    setGroupFileConfigured(bool configured) {
        DSD_SNPRINTF(m_talkgroup_opts->group_in_file, sizeof(m_talkgroup_opts->group_in_file), "%s",
                     configured ? "fixture-talkgroups.csv" : "");
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
    }

    Q_INVOKABLE bool
    clearTalkgroups() {
        if (dsd_tg_policy_clear(m_talkgroup_state.get()) != 0) {
            return false;
        }
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
        return true;
    }

    Q_INVOKABLE int
    holdCalls() const {
        return m_commands->holdCalls();
    }

    Q_INVOKABLE double
    lastHoldTg() const {
        return m_commands->lastHoldTg();
    }

    Q_INVOKABLE int
    talkgroupListenCalls() const {
        return m_commands != nullptr ? m_commands->talkgroupListenCalls() : -1;
    }

    Q_INVOKABLE double
    lastTalkgroupListenIdStart() const {
        return m_commands != nullptr ? m_commands->lastTalkgroupListenIdStart() : 0.0;
    }

    Q_INVOKABLE double
    lastTalkgroupListenIdEnd() const {
        return m_commands != nullptr ? m_commands->lastTalkgroupListenIdEnd() : 0.0;
    }

    Q_INVOKABLE bool
    lastTalkgroupListenOn() const {
        return m_commands != nullptr && m_commands->lastTalkgroupListenOn();
    }

    Q_INVOKABLE int
    allTalkgroupsListenCalls() const {
        return m_commands != nullptr ? m_commands->allTalkgroupsListenCalls() : -1;
    }

    Q_INVOKABLE bool
    lastAllTalkgroupsListenOn() const {
        return m_commands != nullptr && m_commands->lastAllTalkgroupsListenOn();
    }

    Q_INVOKABLE QString
    lastAllTalkgroupsTag() const {
        return m_commands != nullptr ? m_commands->lastAllTalkgroupsTag() : QString();
    }

    /**
     * @brief Change one engine reading and republish it.
     *
     * The engine-facing mocks are plain maps, which QML cannot mutate in place;
     * re-setting the context property is what re-evaluates the bindings that read
     * it. Exposed to the tests as `testContext` so a case can drive a reading to
     * a value and back rather than asserting only whatever the fixture started at.
     */
    Q_INVOKABLE void
    setMetric(const QString& key, const QVariant& value) {
        m_metrics[key] = value;
        if (m_engine != nullptr) {
            m_metric_readings->insert(key, value);
            if (key == QStringLiteral("centerFreqHz")) {
                Q_EMIT m_metric_readings->tunerChanged();
            }
        }
    }

    /**
     * @brief Set one radioReference key, so a case can flip a stubbed reading.
     *
     * The property map updates bindings and records lookupNearby calls. Keep
     * the audit's QVariantMap in sync with the values QML actually reads.
     */
    Q_INVOKABLE void
    setRadioReference(const QString& key, const QVariant& value) {
        m_radio_reference[key] = value;
        if (m_radio_reference_recorder != nullptr) {
            m_radio_reference_recorder->insert(key, value);
        }
    }

    Q_INVOKABLE int
    radioReferenceNearbyCalls() const {
        return m_radio_reference_recorder != nullptr ? m_radio_reference_recorder->nearbyCalls : 0;
    }

    /**
     * @brief Set one prefs key, for the same reason setRadioReference() exists.
     *
     * The fixture prefs are a plain map too, so a screen that gates on a stored
     * preference (the Settings application-key row reads prefs.rrAppKey) could
     * otherwise only ever be tested at the fixture's defaults. Reads only, like
     * the rest of the map: a QML write to prefs.* still no-ops here.
     */
    Q_INVOKABLE void
    setPrefs(const QString& key, const QVariant& value) {
        m_prefs[key] = value;
        if (m_engine != nullptr) {
            m_engine->rootContext()->setContextProperty(QStringLiteral("prefs"), m_prefs);
        }
    }

    /**
     * @brief Reads in @p qmlFiles that name a context-property key the fixture lacks.
     *
     * Returns "metrics.someReading" style entries, empty when the maps below cover
     * every read. Exists because a QVariantMap answers an unknown key with
     * `undefined` rather than an error (see the file comment): without this the
     * fixture could fall behind the screens silently.
     *
     * Literal `name.key` reads only. A key assembled at run time
     * (`metrics["slot" + n + "TgText"]`) is invisible here and still has to be
     * added to the maps by hand. Method calls are skipped — `decoderHost.stop()`
     * is a command, not a reading — and `commands` is left out entirely for the
     * same reason: every use of it is a call on a user action no case triggers.
     *
     * @param qmlFiles File names under src/ui/qt/qml, as the tests load them.
     */
    /** @brief Frequency of the canned spectrum's peak, so a case need not hard-code it. */
    Q_INVOKABLE double
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    spectrumPeakHz() const {
        return dsd_neo_qml_stub::spectrum_peak_hz();
    }

    /** @brief Read the packaged USB IDs using Android's attribute names and wildcard defaults. */
    Q_INVOKABLE QVariantList
    androidUsbDeviceFilters() const {
        QFile file(QStringLiteral(DSD_QML_UI_DIR "/../../../../android/package/res/xml/device_filter.xml"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        QXmlStreamReader xml(&file);
        QVariantList filters;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != QLatin1String("usb-device")) {
                continue;
            }
            QVariantMap filter;
            // Android treats missing or unrecognized ID attributes as wildcards.
            for (const auto* name : {"vendor-id", "product-id"}) {
                bool ok = false;
                const int value = xml.attributes().value(QLatin1String(name)).toInt(&ok);
                filter[QLatin1String(name)] = ok ? value : -1;
            }
            filters.append(filter);
        }
        return xml.hasError() ? QVariantList() : filters;
    }

    /**
     * @brief Write @p contents to a disposable CSV and answer its path.
     *
     * The imports library only grows a row by copying a real file through
     * DecoderHost::importDocument() and parsing what landed, and QML cannot
     * create one. The file goes under the same disposable app data tree the
     * library itself persists to, so a run leaves nothing behind.
     */
    Q_INVOKABLE QString
    writeFixtureCsv(const QString& name, const QString& contents) const {
        QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/fixtures"));
        if (!dir.mkpath(QStringLiteral("."))) {
            return QString();
        }
        const QString path = dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return QString();
        }
        if (file.write(contents.toUtf8()) < 0) {
            return QString();
        }
        file.close();
        return path;
    }

    /** Use real QObject signals and QSettings for initialization bookkeeping tests;
     * plain context maps deliberately cannot validate writes or lifecycle edges. */
    Q_INVOKABLE void
    useLifecycleHost(bool on, bool signals_initialized = false, bool start_running = false) {
        m_lifecycle_host->stop();
        m_lifecycle_host->acceptStart = false;
        if (on) {
            m_lifecycle_host = signals_initialized ? m_initializing_host : m_import_host;
            m_lifecycle_host->acceptStart = true;
            m_lifecycle_host->startRunning = start_running;
            m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_lifecycle_host);
            m_engine->rootContext()->setContextProperty(QStringLiteral("prefs"), m_app_prefs);
        } else {
            m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
            m_engine->rootContext()->setContextProperty(QStringLiteral("prefs"), m_prefs);
        }
    }

    Q_INVOKABLE void
    setKeyboardBoundary(qreal value) {
        m_lifecycle_host->keyboardBoundary = value;
        Q_EMIT m_lifecycle_host->keyboardChanged();
    }

    Q_INVOKABLE void
    setDelayedStop(bool enabled) {
        m_lifecycle_host->delayedStop = enabled;
    }

    Q_INVOKABLE void
    setLifecyclePhase(int value) {
        m_lifecycle_host->setPhase(static_cast<dsd_qt::DecoderHost::SessionState>(value));
    }

    Q_INVOKABLE void
    setScanTestUsb(bool brokered, bool ready) {
        m_import_host->usbBrokered = brokered;
        m_import_host->usbReady = ready;
        Q_EMIT m_import_host->localDeviceChanged();
    }

    Q_INVOKABLE void
    setScanTestAcceptStart(bool accept) {
        m_import_host->acceptStart = accept;
    }

    // WP-S2: deliver a consumed host event, rather than bypassing policy with a start signal.
    Q_INVOKABLE void
    emitLocalDeviceAttached() {
        Q_EMIT m_lifecycle_host->localDeviceAttached(QStringLiteral("fixture-usb"));
    }

    Q_INVOKABLE void
    emitSessionInitialized() {
        Q_EMIT m_lifecycle_host->sessionInitialized();
    }

    // WP-D5: drive the brokered diagnostic and count Retry requests.
    Q_INVOKABLE void
    setDongleStatus(bool ready, int kind, const QString& text) {
        m_lifecycle_host->usbBrokered = true;
        m_lifecycle_host->usbReady = ready;
        m_lifecycle_host->usbFailureKind = kind;
        m_lifecycle_host->usbStatus = text;
        Q_EMIT m_lifecycle_host->localDeviceChanged();
    }

    Q_INVOKABLE int
    dongleRetryRequests() const {
        return m_lifecycle_host->usbRequests;
    }

    // WP-D3: platform capability gate for the nearby search button.
    Q_INVOKABLE void
    setLocationSupported(bool supported) {
        m_host[QStringLiteral("locationSupported")] = supported;
        m_host_readings->insert(QStringLiteral("locationSupported"), supported);
        m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
    }

    /** @brief Publish a live/idle host so cases can exercise session-only actions. */
    Q_INVOKABLE void
    setHostRunning(bool running) {
        m_host[QStringLiteral("running")] = running;
        m_host_readings->insert(QStringLiteral("running"), running);
        Q_EMIT m_host_readings->runningChanged();
        if (m_engine != nullptr) {
            m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
        }
    }

    /** @brief Forget every recorded command. */
    Q_INVOKABLE void
    resetCommands() {
        if (m_commands != nullptr) {
            m_commands->reset();
        }
    }

    /** @brief How many manual tunes the screens have asked for. */
    Q_INVOKABLE int
    manualTuneCalls() const {
        return (m_commands != nullptr) ? m_commands->manualTuneCalls() : -1;
    }

    /** @brief The frequency of the most recent manual tune request. */
    Q_INVOKABLE double
    lastManualTuneHz() const {
        return (m_commands != nullptr) ? m_commands->lastManualTuneHz() : 0.0;
    }

    /** @brief How many times the screens have asked to be given the tuner. */
    Q_INVOKABLE int
    releaseTunerCalls() const {
        return (m_commands != nullptr) ? m_commands->releaseTunerCalls() : -1;
    }

    /** @brief How many times the screens have asked to hand the tuner to trunking. */
    Q_INVOKABLE int
    setTrunkingCalls() const {
        return (m_commands != nullptr) ? m_commands->setTrunkingCalls() : -1;
    }

    /** @brief Which way the most recent trunking request went. */
    Q_INVOKABLE bool
    lastSetTrunking() const {
        return (m_commands != nullptr) ? m_commands->lastSetTrunking() : false;
    }

    /** @brief What the monitor's scan controls asked the engine for. */
    Q_INVOKABLE int
    scanHoldCalls() const {
        return (m_commands != nullptr) ? m_commands->scanHoldCalls() : -1;
    }

    Q_INVOKABLE int
    scanAvoidCalls() const {
        return (m_commands != nullptr) ? m_commands->scanAvoidCalls() : -1;
    }

    Q_INVOKABLE int
    scanAvoidClearCalls() const {
        return (m_commands != nullptr) ? m_commands->scanAvoidClearCalls() : -1;
    }

    Q_INVOKABLE int
    nextChannelCalls() const {
        return (m_commands != nullptr) ? m_commands->nextChannelCalls() : -1;
    }

    /** @brief What the radio panel last asked the engine for. */
    Q_INVOKABLE int
    gainCalls() const {
        return (m_commands != nullptr) ? m_commands->gainCalls() : -1;
    }

    Q_INVOKABLE int
    lastGainDb() const {
        return (m_commands != nullptr) ? m_commands->lastGainDb() : -1;
    }

    Q_INVOKABLE double
    lastSquelchDb() const {
        return (m_commands != nullptr) ? m_commands->lastSquelchDb() : 0.0;
    }

    Q_INVOKABLE int
    squelchCalls() const {
        return (m_commands != nullptr) ? m_commands->squelchCalls() : -1;
    }

    Q_INVOKABLE int
    lastModulation() const {
        return (m_commands != nullptr) ? m_commands->lastModulation() : -1;
    }

    Q_INVOKABLE int
    lastDecodeMode() const {
        return (m_commands != nullptr) ? m_commands->lastDecodeMode() : -1;
    }

    Q_INVOKABLE int
    lastPpm() const {
        return (m_commands != nullptr) ? m_commands->lastPpm() : 9999;
    }

    Q_INVOKABLE QFont
    applicationFont() const {
        return QGuiApplication::font();
    }

    Q_INVOKABLE void
    setApplicationFont(const QFont& font) {
        QGuiApplication::setFont(font);
    }

    Q_INVOKABLE QStringList
    missingContextKeys(const QStringList& qmlFiles) const {
        const QHash<QString, QVariantMap> maps = {{QStringLiteral("metrics"), m_metrics},
                                                  {QStringLiteral("prefs"), m_prefs},
                                                  {QStringLiteral("decoderHost"), m_host},
                                                  {QStringLiteral("radioReference"), m_radio_reference}};
        /* Group 3 captures the "(" that marks a call rather than a read. */
        static const QRegularExpression read(
            QStringLiteral("\\b(metrics|prefs|decoderHost|radioReference)\\.([A-Za-z_][A-Za-z0-9_]*)\\s*(\\()?"));

        QStringList missing;
        for (const QString& name : qmlFiles) {
            QFile file(QStringLiteral(DSD_QML_UI_DIR "/") + name);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                missing.append(name + QStringLiteral(" (unreadable)"));
                continue;
            }
            const QString text = QString::fromUtf8(file.readAll());
            QRegularExpressionMatchIterator it = read.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                if (!match.captured(3).isEmpty()) {
                    continue;
                }
                const QString object = match.captured(1);
                const QString key = match.captured(2);
                const QString entry = object + QLatin1Char('.') + key;
                if (!maps.value(object).contains(key) && !missing.contains(entry)) {
                    missing.append(entry);
                }
            }
        }
        missing.sort();
        return missing;
    }

  public Q_SLOTS:

    /**
     * @brief Load the bundled faces once the QGuiApplication exists.
     *
     * Not in the constructor: QUICK_TEST_MAIN_WITH_SETUP builds this object
     * before the application, and QFontDatabase crashes without one.
     */
    void
    applicationAvailable() {
        /* The library and saved-systems models persist to the app data tree, so
         * point that at disposable test storage before either is constructed --
         * a run must not read or write a real profile (same arrangement as
         * UI_QT_PERSISTENCE and UI_QT_IMPORTED_FILES). */
        QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
        QCoreApplication::setApplicationName(QStringLiteral("dsd-neo-qml-%1").arg(QCoreApplication::applicationPid()));
        dsd_test_qt_isolate_paths();
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();

        for (const QString& file :
             {QStringLiteral("IBMPlexSans-Regular.ttf"), QStringLiteral("IBMPlexSans-SemiBold.ttf"),
              QStringLiteral("IBMPlexSans-Bold.ttf"), QStringLiteral("IBMPlexMono-Regular.ttf"),
              QStringLiteral("IBMPlexMono-Medium.ttf")}) {
            QFontDatabase::addApplicationFont(QStringLiteral(DSD_QML_FONT_DIR "/") + file);
        }
    }

    void
    qmlEngineAvailable(QQmlEngine* engine) {
        /* Match ui_load() registrations before parsing QML importers. */
        // WP-D4: cancellation observes pointer, keyboard and shortcut input.
        qmlRegisterType<dsd_qt::SiteInteractionGuard>("DsdNeo", 1, 0, "SiteInteractionGuard");
        qmlRegisterType<dsd_qt::SpectrumTraceItem>("DsdNeo", 1, 0, "SpectrumTrace");
        qmlRegisterType<dsd_qt::WaterfallItem>("DsdNeo", 1, 0, "Waterfall");

        auto* store = new CallLogStore();
        auto* historyView = new CallHistoryFilterModel(engine);
        historyView->setSourceModel(store);
        auto* monitorView = new CallHistoryFilterModel(engine);
        monitorView->setSourceModel(store);
        store->setParent(engine);
        m_talkgroups = new dsd_qt::TalkgroupListModel(store, engine);
        auto* talkgroupView = new dsd_qt::TalkgroupFilterModel(engine);
        talkgroupView->setSourceModel(m_talkgroups);
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());

        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty(QStringLiteral("uiDir"), QStringLiteral(DSD_QML_UI_DIR));
        ctx->setContextProperty(QStringLiteral("callHistory"), store);
        ctx->setContextProperty(QStringLiteral("historyView"), historyView);
        ctx->setContextProperty(QStringLiteral("monitorView"), monitorView);
        ctx->setContextProperty(QStringLiteral("talkgroups"), m_talkgroups);
        ctx->setContextProperty(QStringLiteral("talkgroupView"), talkgroupView);
        ctx->setContextProperty(QStringLiteral("sansFontFamily"), QStringLiteral("IBM Plex Sans"));
        ctx->setContextProperty(QStringLiteral("monoFontFamily"), QStringLiteral("IBM Plex Mono"));

        /* Appearance 2 = dark, so the run does not depend on the host's colour
         * scheme; the token set is the only thing that reads it. */
        QVariantMap prefs;
        prefs[QStringLiteral("appearance")] = 2;
        prefs[QStringLiteral("metricUnits")] = false;
        prefs[QStringLiteral("onboardingDone")] = true;
        prefs[QStringLiteral("backgroundListening")] = false;
        prefs[QStringLiteral("notificationExplained")] = true;
        prefs[QStringLiteral("keepScreenAwake")] = false;
        prefs[QStringLiteral("skipEncrypted")] = false;
        /* The radio defaults the settings screen and the explore setup edit.
         * AppPrefs' own defaults, so a case that reads one sees what a fresh
         * install would. */
        prefs[QStringLiteral("autoPpm")] = false;
        prefs[QStringLiteral("hangtimeSec")] = 2.0;
        prefs[QStringLiteral("gainDb")] = 30;
        prefs[QStringLiteral("ppm")] = 0;
        prefs[QStringLiteral("bandwidthKhz")] = 48;
        prefs[QStringLiteral("biasTee")] = false;
        prefs[QStringLiteral("extraArgs")] = QString();
        /* Empty on purpose: it is what makes the explore setup's first tap ask
         * for a source rather than start something that cannot tune. */
        prefs[QStringLiteral("exploreSourceType")] = QString();
        prefs[QStringLiteral("exploreHost")] = QString();
        prefs[QStringLiteral("explorePort")] = 1234;
        prefs[QStringLiteral("exploreFreqMhz")] = QString();
        /* RadioReference account: empty on purpose, so the screen's first state
         * is the credentials gate a fresh install shows. This map answers reads
         * only -- it is a plain QVariantMap, not the production AppPrefs, so QML
         * that WRITES prefs.rrUsername silently no-ops here while persisting in
         * production. Never assert persistence through it. */
        prefs[QStringLiteral("rrUsername")] = QString();
        prefs[QStringLiteral("rrAppKey")] = QString();
        prefs[QStringLiteral("autoStartOnAttach")] = false;
        prefs[QStringLiteral("lastStartedKind")] = QString();
        prefs[QStringLiteral("lastStartedUid")] = QString();
        prefs[QStringLiteral("lastLat")] = 0.0;
        prefs[QStringLiteral("lastLon")] = 0.0;
        prefs[QStringLiteral("lastFixAt")] = 0;
        m_prefs = prefs;
        ctx->setContextProperty(QStringLiteral("prefs"), prefs);

        /* Every key the monitor reads, at rest with no call up. Add to this when a
         * screen grows a reading — missingContextKeys() is what says so. */
        QVariantMap metrics;
        metrics[QStringLiteral("configuredForce")] = 0;
        metrics[QStringLiteral("effectiveForce")] = 0;
        metrics[QStringLiteral("keyProfileRef")] = QString();
        metrics[QStringLiteral("keyEpoch")] = QStringLiteral("1");
        metrics[QStringLiteral("automaticKeys")] = false;
        metrics[QStringLiteral("decryptionSlots")] = QVariantList();
        // WP-F1: site identity fixture keys.
        metrics[QStringLiteral("siteProtocol")] = QString();
        metrics[QStringLiteral("p25NacValid")] = false;
        metrics[QStringLiteral("p25Nac")] = 0;
        metrics[QStringLiteral("p25WacnValid")] = false;
        metrics[QStringLiteral("p25Wacn")] = 0;
        metrics[QStringLiteral("p25SysIdValid")] = false;
        metrics[QStringLiteral("p25SysId")] = 0;
        metrics[QStringLiteral("p25Rfss")] = 0;
        metrics[QStringLiteral("p25Site")] = 0;
        metrics[QStringLiteral("p25LraValid")] = false;
        metrics[QStringLiteral("p25Lra")] = 0;
        metrics[QStringLiteral("p25Phase2ParamsReady")] = false;
        metrics[QStringLiteral("dmrColorCode")] = -1;
        metrics[QStringLiteral("dmrSiteText")] = QString();
        metrics[QStringLiteral("dmrRestLsn")] = 0;
        metrics[QStringLiteral("nxdnRan")] = -1;
        metrics[QStringLiteral("nxdnLocationCategory")] = QString();
        metrics[QStringLiteral("nxdnSysCode")] = 0;
        metrics[QStringLiteral("nxdnSiteCode")] = 0;
        metrics[QStringLiteral("edacsSiteText")] = QString();
        metrics[QStringLiteral("ccFreqHz")] = 0;
        metrics[QStringLiteral("vcFreqHz")] = 0;
        metrics[QStringLiteral("siteLine")] = QString();
        metrics[QStringLiteral("siteConfirmed")] = false;

        metrics[QStringLiteral("uiMessage")] = QString();
        metrics[QStringLiteral("audioMuted")] = false;
        metrics[QStringLiteral("heldTg")] = 0;
        metrics[QStringLiteral("carrierLock")] = false;
        metrics[QStringLiteral("radioInput")] = true;
        metrics[QStringLiteral("streamActive")] = false;
        metrics[QStringLiteral("snrValid")] = false;
        metrics[QStringLiteral("snrDb")] = 0.0;
        metrics[QStringLiteral("cfoHz")] = 0.0;
        metrics[QStringLiteral("airspy")] = QVariantMap();
        metrics[QStringLiteral("tunerGainText")] = QStringLiteral("auto");
        // WP-F3 decode quality: validity is independent of radioInput.
        metrics[QStringLiteral("qualityValid")] = false;
        metrics[QStringLiteral("voiceErrsValid")] = false;
        metrics[QStringLiteral("voiceErrsPerFrame")] = 0.0;
        metrics[QStringLiteral("voiceErrsSamples")] = 0;
        metrics[QStringLiteral("slot1VoiceErrsValid")] = false;
        metrics[QStringLiteral("slot1VoiceErrsPerFrame")] = 0.0;
        metrics[QStringLiteral("slot1VoiceErrsSamples")] = 0;
        metrics[QStringLiteral("slot2VoiceErrsValid")] = false;
        metrics[QStringLiteral("slot2VoiceErrsPerFrame")] = 0.0;
        metrics[QStringLiteral("slot2VoiceErrsSamples")] = 0;
        metrics[QStringLiteral("ccFecValid")] = false;
        metrics[QStringLiteral("ccFecOkPct")] = 0.0;
        metrics[QStringLiteral("voiceFecValid")] = false;
        metrics[QStringLiteral("voiceFecOkPct")] = 0.0;
        metrics[QStringLiteral("rsValid")] = false;
        metrics[QStringLiteral("rsOkPct")] = 0.0;
        metrics[QStringLiteral("ccFecOk")] = 0;
        metrics[QStringLiteral("ccFecErr")] = 0;
        metrics[QStringLiteral("lastFrameErrsValid")] = false;
        metrics[QStringLiteral("lastFrameErrs")] = 0;
        metrics[QStringLiteral("lastFrameErrs2")] = 0;

        for (int slot = 1; slot <= 2; slot++) {
            const QString p = QStringLiteral("slot%1").arg(slot);
            metrics[p + QStringLiteral("CallState")] = 0;
            metrics[p + QStringLiteral("CallName")] = QString();
            // The scan channel the slot's call was heard on; empty when not scanning.
            metrics[p + QStringLiteral("Channel")] = QString();
            metrics[p + QStringLiteral("CallEnc")] = false;
            metrics[p + QStringLiteral("CallEmergency")] = false;
            metrics[p + QStringLiteral("CallPriority")] = 0;
            metrics[p + QStringLiteral("CallSeconds")] = 0;
            metrics[p + QStringLiteral("TgText")] = QString();
            metrics[p + QStringLiteral("SrcText")] = QString();
            metrics[p + QStringLiteral("EncText")] = QString();
            metrics[p + QStringLiteral("TgId")] = 0;
        }
        // Which slot the hero headlines, one-based, 0 for neither. Derived natively by
        // dsd_app_lead_slot() in the real model; here it is a plain reading a case sets
        // alongside the slotNCallState it is meant to agree with.
        metrics[QStringLiteral("leadSlot")] = 0;
        // Targets the encrypted lockout is skipping; 0 is the at-rest value.
        metrics[QStringLiteral("encLockoutCount")] = 0;
        // On-the-fly scan controls (#380): no rotation running at rest.
        metrics[QStringLiteral("scanRotationActive")] = false;
        metrics[QStringLiteral("optionsKnown")] = false;
        metrics[QStringLiteral("scanHold")] = false;
        metrics[QStringLiteral("scanTargetId")] = QString();
        metrics[QStringLiteral("scanTargetOrdinal")] = 0;
        metrics[QStringLiteral("scanTargetCount")] = 0;
        metrics[QStringLiteral("scanAvoidCount")] = 0;
        metrics[QStringLiteral("scanTargetAvoided")] = false;
        // Whether an automatic controller owns the tuner, which one, and where it
        // points. The two named owners word a message; tunerControlled is the gate.
        metrics[QStringLiteral("tunerControlled")] = false;
        metrics[QStringLiteral("trunkingEnabled")] = false;
        metrics[QStringLiteral("scannerMode")] = false;
        metrics[QStringLiteral("centerFreqHz")] = static_cast<double>(dsd_neo_qml_stub::kSpectrumCenterHz);
        // Width of the channel being demodulated; 12.5 kHz is the P25/DMR case.
        metrics[QStringLiteral("channelBandwidthHz")] = 12500;
        // Whether the decoder has found anything since the tuner last moved. False
        // at rest, which is what lets a sweep keep stepping until a case says so.
        metrics[QStringLiteral("syncedHere")] = false;
        metrics[QStringLiteral("syncLabel")] = QString();
        metrics[QStringLiteral("trunkableSync")] = false;
        // What the radio panel shows and changes. DSDCFG_MODE_AUTO is 1.
        metrics[QStringLiteral("decodeMode")] = 1;
        metrics[QStringLiteral("modulation")] = 0;
        metrics[QStringLiteral("tunerGainDb")] = 30;
        metrics[QStringLiteral("squelchDb")] = -120.0;
        metrics[QStringLiteral("squelchOff")] = false;
        metrics[QStringLiteral("ppm")] = 0;
        m_metrics = metrics;
        m_engine = engine;
        m_metric_readings = new ReadingMap(engine);
        for (auto it = metrics.cbegin(); it != metrics.cend(); ++it) {
            m_metric_readings->insert(it.key(), it.value());
        }
        ctx->setContextProperty(QStringLiteral("metrics"), m_metric_readings);
        ctx->setContextProperty(QStringLiteral("testContext"), this);

        QVariantMap host;
        host[QStringLiteral("running")] = false;
        host[QStringLiteral("transitioning")] = false;
        host[QStringLiteral("statusText")] = QStringLiteral("idle");
        /* The spectrum view gates production on a live session, so the fixture
         * has to claim one or its frames would never start. */
        host[QStringLiteral("sessionActive")] = true;
        host[QStringLiteral("signalsSessionInitialized")] = false;
        host[QStringLiteral("fontRevision")] = 0;
        host[QStringLiteral("inputFailureKind")] = 0;
        host[QStringLiteral("inputFailureCode")] = 0;
        host[QStringLiteral("terminalReason")] = 0;
        host[QStringLiteral("audioRoute")] = QStringLiteral("System default");
        host[QStringLiteral("usesPlatformFontScaling")] = false;
        /* Why the last session stopped, empty while nothing has failed. */
        host[QStringLiteral("failureText")] = QString();
        /* The Android-only capabilities the settings screen hides rows on: a
         * desktop host brokers no USB device and cannot hold the screen awake,
         * which is the arrangement this offscreen run matches. */
        host[QStringLiteral("keepScreenAwakeSupported")] = false;
        host[QStringLiteral("localDeviceBrokered")] = false;
        host[QStringLiteral("localDeviceSource")] = QStringLiteral("usb");
        host[QStringLiteral("localDeviceSerial")] = QString();
        host[QStringLiteral("localDeviceReady")] = false;
        host[QStringLiteral("localDeviceStatus")] = QString();
        host[QStringLiteral("locationSupported")] = false;
        host[QStringLiteral("shareSupported")] = false;
        host[QStringLiteral("localDeviceFailureKind")] = 0;
        host[QStringLiteral("sessionState")] = 0;
        host[QStringLiteral("keyboardTop")] = -1;
        m_host = host;
        m_host_readings = new ReadingMap(engine);
        for (auto it = host.cbegin(); it != host.cend(); ++it) {
            m_host_readings->insert(it.key(), it.value());
        }
        ctx->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);

        /* Every property key the RadioReference screen reads, at rest. `available`
         * is false so an ungated entry point shows up as a visible row rather
         * than passing silently: an unregistered context property would raise a
         * ReferenceError, leave the binding at its default, and `visible`
         * defaults to true. */
        QVariantMap rr;
        rr[QStringLiteral("available")] = false;
        rr[QStringLiteral("hasAppKey")] = false;
        /* Whether the binary bakes an application key in. False, like a source
         * build with DSD_RR_APP_KEY unset, so the key field is offered. */
        rr[QStringLiteral("buildHasAppKey")] = false;
        rr[QStringLiteral("credentialsReady")] = false;
        rr[QStringLiteral("busy")] = false;
        rr[QStringLiteral("statusText")] = QString();
        rr[QStringLiteral("errorText")] = QString();
        rr[QStringLiteral("errorKind")] = 0;
        rr[QStringLiteral("errorIsAuth")] = false;
        rr[QStringLiteral("errorIsSubscription")] = false;
        /* Both, and independently: the real model answers false to both for a
         * system type it cannot import, so a fixture that derived one from the
         * other could not express the case that mis-selects. */
        rr[QStringLiteral("conventional")] = false;
        rr[QStringLiteral("trunked")] = true;
        rr[QStringLiteral("countries")] = QVariantList();
        rr[QStringLiteral("states")] = QVariantList();
        rr[QStringLiteral("counties")] = QVariantList();
        rr[QStringLiteral("systems")] = QVariantList();
        rr[QStringLiteral("sites")] = QVariantList();
        rr[QStringLiteral("systemDetails")] = QVariantMap();
        rr[QStringLiteral("talkgroupSummary")] = QVariantMap();
        m_radio_reference = rr;
        m_radio_reference_recorder = new RadioReferenceRecorder(engine);
        for (auto it = rr.cbegin(); it != rr.cend(); ++it) {
            m_radio_reference_recorder->insert(it.key(), it.value());
        }
        ctx->setContextProperty(QStringLiteral("radioReference"), m_radio_reference_recorder);

        /* The real SpectrumModel over the canned getter in qml_spectrum_stub.cpp:
         * the polling, viewport and tap-snapping under test are the production
         * ones, only the frames are synthetic. */
        m_spectrum = new dsd_qt::SpectrumModel(engine);
        ctx->setContextProperty(QStringLiteral("spectrum"), m_spectrum);

        m_commands = new CommandRecorder();
        m_commands->setParent(engine);
        ctx->setContextProperty(QStringLiteral("commands"), m_commands);

        /* Production models, not stand-ins. Two of these back a ListView whose
         * delegate declares `required property` per role, so a stand-in would
         * have to mirror all nineteen roles exactly or the delegate would fail
         * to instantiate -- and a mirror that drifted would fail as the real
         * thing passing. They persist under the disposable app data tree set up
         * in applicationAvailable(), and start empty there. Without them the
         * home screen, the imports library, the wizard's pickers and the explore
         * setup all raised ReferenceError and rendered nothing. */
        m_import_host = new ImportOnlyHost();
        m_import_host->setParent(engine);
        m_initializing_host = new InitializingHost();
        m_initializing_host->setParent(engine);
        m_lifecycle_host = m_import_host;
        auto* imported_files = new dsd_qt::ImportedFilesModel(m_import_host, engine);
        auto* saved_systems = new dsd_qt::SavedSystemsModel(engine);
        auto* app_prefs = new dsd_qt::AppPrefs(engine);
        m_app_prefs = app_prefs;
        auto* session_args = new dsd_qt::SessionArgsBuilder(app_prefs, engine);
        session_args->setSavedSystems(saved_systems);
        ctx->setContextProperty(QStringLiteral("importedFiles"), imported_files);
        ctx->setContextProperty(QStringLiteral("p25Network"), new dsd_qt::P25NetworkModel(engine)); // WP-F2
        ctx->setContextProperty(QStringLiteral("diagnosticsLog"), new dsd_qt::DiagnosticsLogModel(nullptr, engine));
        auto* controller = new TestUiController(engine);
        auto* scan_lists = new dsd_qt::ScanListsModel(engine);
        // WP-S2: use the production pure policy for host attachment delivery in QML tests.
        // The real controller's emission/validation contract is covered by UI_QT_CONTROLLER.
        for (ImportOnlyHost* attachmentHost : {m_import_host, static_cast<ImportOnlyHost*>(m_initializing_host)}) {
            QObject::connect(
                attachmentHost, &dsd_qt::DecoderHost::localDeviceAttached, controller, [=](const QString&) {
                    const QString kind = app_prefs->lastStartedKind();
                    const QString uid = app_prefs->lastStartedUid();
                    const QVariantMap target = kind == QStringLiteral("saved")  ? saved_systems->getByUid(uid)
                                               : kind == QStringLiteral("scan") ? scan_lists->getByUid(uid)
                                                                                : QVariantMap();
                    if (dsd_qt::autoStartAllowed(
                            app_prefs->autoStartOnAttach(), app_prefs->onboardingDone(), attachmentHost->sessionState(),
                            controller->autoStartBlocked, !target.isEmpty(),
                            target.value(QStringLiteral("sourceType")).toString() == QStringLiteral("usb"))) {
                        controller->requestAutoStart(kind, uid);
                    }
                });
        }
        ctx->setContextProperty(QStringLiteral("uiController"), controller);
        ctx->setContextProperty(QStringLiteral("savedSystems"), saved_systems);
        ctx->setContextProperty(QStringLiteral("scanLists"), scan_lists);
        auto* profiles = new dsd_qt::DecryptionProfilesModel(engine);
        profiles->setReferences(saved_systems, scan_lists);
        auto* starter = new dsd_qt::ScanListStarter(app_prefs, saved_systems, engine);
        starter->setDecryptionProfiles(profiles);
        session_args->setDecryptionProfiles(profiles);
        ctx->setContextProperty(QStringLiteral("decryptionProfiles"), profiles);
        ctx->setContextProperty(QStringLiteral("scanListStarter"), starter);
        ctx->setContextProperty(QStringLiteral("sessionArgs"), session_args);
        ctx->setContextProperty(QStringLiteral("appVersionText"), QStringLiteral("0.0.0-test"));
    }

  private:
    dsd_qt::AppPrefs* m_app_prefs = nullptr;
    ReadingMap* m_metric_readings = nullptr;
    ReadingMap* m_host_readings = nullptr;
    QVariantMap m_metrics;
    QVariantMap m_prefs;
    QVariantMap m_host;
    QVariantMap m_radio_reference;
    RadioReferenceRecorder* m_radio_reference_recorder = nullptr;
    QQmlEngine* m_engine = nullptr;
    dsd_qt::SpectrumModel* m_spectrum = nullptr;
    CommandRecorder* m_commands = nullptr;
    ImportOnlyHost* m_import_host = nullptr;
    ImportOnlyHost* m_initializing_host = nullptr;
    ImportOnlyHost* m_lifecycle_host = nullptr;
    std::unique_ptr<dsd_opts> m_talkgroup_opts = std::make_unique<dsd_opts>();
    std::unique_ptr<dsd_state> m_talkgroup_state = std::make_unique<dsd_state>();
    dsd_qt::TalkgroupListModel* m_talkgroups = nullptr;
};

#endif /* DSD_NEO_TESTS_UI_QML_TEST_CONTEXT_H_ */
