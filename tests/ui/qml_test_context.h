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
 * CallHistoryFilterModel, so the filter and its change signalling are under
 * test. Behind it sits CallLogStore, a stand-in for CallHistoryModel that
 * prepends rows on demand — the real store only grows by ingesting a decoder
 * snapshot ring, which is covered by UI_QT_CALL_HISTORY_MODEL instead. The
 * engine-facing objects (metrics, decoderHost, commands, prefs) are plain maps
 * rather than the production QObject models, which is what keeps this test off
 * the app-control boundary and clear of the engine libraries.
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
#include <QFontDatabase>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQuickTest>

#include "app_prefs.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "decode_mode_flag.h"
#include "decoder_host.h"
#include "imported_files_model.h"
#include "qml_spectrum_stub.h"
#include "saved_systems_model.h"
#include "session_args.h"
#include "spectrum_model.h"
#include "spectrum_view_item.h"

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
        return false;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        Q_UNUSED(argv)
        return false;
    }

    void
    stop() override {}
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

  public:
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
    tuneHz(unsigned int hz) {
        Q_UNUSED(hz)
        return true;
    }

    Q_INVOKABLE bool
    toggleMute() {
        return true;
    }

    Q_INVOKABLE bool
    holdTalkgroup(double) {
        return true;
    }

    Q_INVOKABLE bool
    lockoutSlot(int) {
        return true;
    }

    Q_INVOKABLE bool
    clearEncLockouts() {
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
    decodeModeForFlag(const QString& flag) {
        return dsd_qt::decode_mode_for_flag(flag);
    }

    Q_INVOKABLE int
    cycleHistoryMode() {
        return 0;
    }

    void
    reset() {
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

  private:
    int m_manual_tune_calls = 0;
    unsigned int m_last_manual_tune_hz = 0U;
    int m_release_tuner_calls = 0;
    int m_set_trunking_calls = 0;
    bool m_last_set_trunking = false;
    int m_gain_calls = 0;
    int m_last_gain_db = -1;
    double m_last_squelch_db = 0.0;
    int m_squelch_calls = 0;
    int m_last_modulation = -1;
    int m_last_decode_mode = -1;
    int m_last_ppm = 9999;
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
    Q_PROPERTY(QStringList systemLabels READ systemLabels NOTIFY countChanged)

  public:
    /** @brief One logged row, mirroring CallHistoryModel::Row's visible fields. */
    struct StoreRow {
        QString name;
        qulonglong tg = 0;
        qulonglong src = 0;
        bool enc = false;
        qint64 when = 0;
        int durationSecs = 4;
        QString systemName;
        QString dayLabel;
        QString timeText;
        int kind = CallHistoryModel::KindVoice;
        QString detail;
        QString channel;
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
            case CallHistoryModel::EncRole: return row.enc;
            case CallHistoryModel::WhenRole: return row.when;
            case CallHistoryModel::DurationSecsRole: return row.durationSecs;
            case CallHistoryModel::SystemNameRole: return row.systemName;
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
                {CallHistoryModel::EncRole, "enc"},
                {CallHistoryModel::WhenRole, "when"},
                {CallHistoryModel::DurationSecsRole, "durationSecs"},
                {CallHistoryModel::SystemNameRole, "systemName"},
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
        row.dayLabel = dayLabel.isEmpty() ? QStringLiteral("TODAY") : dayLabel;
        row.timeText = QStringLiteral("12:%1").arg(m_seq % 60, 2, 10, QLatin1Char('0'));
        beginInsertRows(QModelIndex(), 0, 0);
        m_rows.prepend(row);
        endInsertRows();
        Q_EMIT countChanged();
        return row.name;
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

  private:
    QList<StoreRow> m_rows;
    QString m_systemName = QStringLiteral("Test Site");
    QString m_sessionLabel = QStringLiteral("Test Site");
    int m_seq = 0;
    /* Fixed, ascending stamps: nothing here should depend on the wall clock. */
    qint64 m_clock = 1'700'000'000;
};

/** @brief Installs the context the screens expect before any QML is loaded. */
class Setup : public QObject {
    Q_OBJECT

  public:
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
            m_engine->rootContext()->setContextProperty(QStringLiteral("metrics"), m_metrics);
        }
    }

    /**
     * @brief Set one radioReference key, so a case can flip a stubbed reading.
     *
     * QML cannot mutate a QVariantMap in place, so without this a case could not
     * drive "the entry point appears once `available` turns true". Reads only:
     * any case that has to CALL radioReference.lookupZip(...) needs a small
     * Q_OBJECT recorder instead, the way CommandRecorder works.
     */
    Q_INVOKABLE void
    setRadioReference(const QString& key, const QVariant& value) {
        m_radio_reference[key] = value;
        if (m_engine != nullptr) {
            m_engine->rootContext()->setContextProperty(QStringLiteral("radioReference"), m_radio_reference);
        }
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
    spectrumPeakHz() const {
        return dsd_neo_qml_stub::spectrum_peak_hz();
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
        QStandardPaths::setTestModeEnabled(true);
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
        /* The spectrum's trace and waterfall are the only C++ types the QML
         * instantiates itself, so they need the same registration ui_load()
         * does before anything importing them is parsed. */
        qmlRegisterType<dsd_qt::SpectrumTraceItem>("DsdNeo", 1, 0, "SpectrumTrace");
        qmlRegisterType<dsd_qt::WaterfallItem>("DsdNeo", 1, 0, "Waterfall");

        auto* store = new CallLogStore();
        auto* historyView = new CallHistoryFilterModel(engine);
        historyView->setSourceModel(store);
        auto* monitorView = new CallHistoryFilterModel(engine);
        monitorView->setSourceModel(store);
        store->setParent(engine);

        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty(QStringLiteral("uiDir"), QStringLiteral(DSD_QML_UI_DIR));
        ctx->setContextProperty(QStringLiteral("callHistory"), store);
        ctx->setContextProperty(QStringLiteral("historyView"), historyView);
        ctx->setContextProperty(QStringLiteral("monitorView"), monitorView);
        ctx->setContextProperty(QStringLiteral("sansFontFamily"), QStringLiteral("IBM Plex Sans"));
        ctx->setContextProperty(QStringLiteral("monoFontFamily"), QStringLiteral("IBM Plex Mono"));

        /* Appearance 2 = dark, so the run does not depend on the host's colour
         * scheme; the token set is the only thing that reads it. */
        QVariantMap prefs;
        prefs[QStringLiteral("appearance")] = 2;
        prefs[QStringLiteral("onboardingDone")] = true;
        prefs[QStringLiteral("backgroundListening")] = false;
        prefs[QStringLiteral("keepScreenAwake")] = false;
        prefs[QStringLiteral("skipEncrypted")] = false;
        /* The radio defaults the settings screen and the explore setup edit.
         * AppPrefs' own defaults, so a case that reads one sees what a fresh
         * install would. */
        prefs[QStringLiteral("autoPpm")] = false;
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
        m_prefs = prefs;
        ctx->setContextProperty(QStringLiteral("prefs"), prefs);

        /* Every key the monitor reads, at rest with no call up. Add to this when a
         * screen grows a reading — missingContextKeys() is what says so. */
        QVariantMap metrics;
        metrics[QStringLiteral("uiMessage")] = QString();
        metrics[QStringLiteral("audioMuted")] = false;
        metrics[QStringLiteral("heldTg")] = 0;
        metrics[QStringLiteral("carrierLock")] = false;
        metrics[QStringLiteral("radioInput")] = true;
        metrics[QStringLiteral("streamActive")] = false;
        metrics[QStringLiteral("snrValid")] = false;
        metrics[QStringLiteral("snrDb")] = 0.0;
        metrics[QStringLiteral("cfoHz")] = 0.0;
        metrics[QStringLiteral("tunerGainText")] = QStringLiteral("auto");
        for (int slot = 1; slot <= 2; slot++) {
            const QString p = QStringLiteral("slot%1").arg(slot);
            metrics[p + QStringLiteral("CallState")] = 0;
            metrics[p + QStringLiteral("CallName")] = QString();
            // The scan channel the slot's call was heard on; empty when not scanning.
            metrics[p + QStringLiteral("Channel")] = QString();
            metrics[p + QStringLiteral("CallEnc")] = false;
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
        ctx->setContextProperty(QStringLiteral("metrics"), metrics);
        ctx->setContextProperty(QStringLiteral("testContext"), this);

        QVariantMap host;
        host[QStringLiteral("running")] = false;
        host[QStringLiteral("transitioning")] = false;
        host[QStringLiteral("statusText")] = QStringLiteral("idle");
        /* The spectrum view gates production on a live session, so the fixture
         * has to claim one or its frames would never start. */
        host[QStringLiteral("sessionActive")] = true;
        /* Why the last session stopped, empty while nothing has failed. */
        host[QStringLiteral("failureText")] = QString();
        /* The Android-only capabilities the settings screen hides rows on: a
         * desktop host brokers no USB device and cannot hold the screen awake,
         * which is the arrangement this offscreen run matches. */
        host[QStringLiteral("keepScreenAwakeSupported")] = false;
        host[QStringLiteral("localDeviceBrokered")] = false;
        host[QStringLiteral("localDeviceReady")] = false;
        host[QStringLiteral("localDeviceStatus")] = QString();
        m_host = host;
        ctx->setContextProperty(QStringLiteral("decoderHost"), host);

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
        ctx->setContextProperty(QStringLiteral("radioReference"), rr);

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
        auto* imported_files = new dsd_qt::ImportedFilesModel(m_import_host, engine);
        auto* saved_systems = new dsd_qt::SavedSystemsModel(engine);
        auto* app_prefs = new dsd_qt::AppPrefs(engine);
        auto* session_args = new dsd_qt::SessionArgsBuilder(app_prefs, engine);
        ctx->setContextProperty(QStringLiteral("importedFiles"), imported_files);
        ctx->setContextProperty(QStringLiteral("savedSystems"), saved_systems);
        ctx->setContextProperty(QStringLiteral("sessionArgs"), session_args);
        ctx->setContextProperty(QStringLiteral("appVersionText"), QStringLiteral("0.0.0-test"));
    }

  private:
    QVariantMap m_metrics;
    QVariantMap m_prefs;
    QVariantMap m_host;
    QVariantMap m_radio_reference;
    QQmlEngine* m_engine = nullptr;
    dsd_qt::SpectrumModel* m_spectrum = nullptr;
    CommandRecorder* m_commands = nullptr;
    ImportOnlyHost* m_import_host = nullptr;
};

#endif /* DSD_NEO_TESTS_UI_QML_TEST_CONTEXT_H_ */
