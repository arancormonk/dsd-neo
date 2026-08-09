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
#include <QDateTime>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQuickTest>

#include "call_history_filter.h"
#include "call_history_model.h"
#include "qml_spectrum_stub.h"
#include "spectrum_model.h"
#include "spectrum_view_item.h"

using dsd_qt::CallHistoryFilterModel;
using dsd_qt::CallHistoryModel;

/**
 * @brief Stand-in for CommandBridge that records instead of submitting.
 *
 * The whole point of a tap-to-tune test is what the screen asks for, so the
 * command surface has to be observable. It carries every method the screens
 * call — an unimplemented one would only fail when some future case triggered
 * it, which is exactly the kind of gap this suite exists to close.
 */
class CommandRecorder : public QObject {
    Q_OBJECT

  public:
    Q_INVOKABLE bool
    manualTuneHz(double hz) {
        m_manual_tune_calls++;
        m_last_manual_tune_hz = hz;
        return true;
    }

    Q_INVOKABLE bool
    tuneHz(double hz) {
        m_tune_calls++;
        m_last_tune_hz = hz;
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
    setTunerGain(int) {
        return true;
    }

    Q_INVOKABLE int
    cycleHistoryMode() {
        return 0;
    }

    void
    reset() {
        m_manual_tune_calls = 0;
        m_last_manual_tune_hz = 0.0;
        m_tune_calls = 0;
        m_last_tune_hz = 0.0;
    }

    int
    manualTuneCalls() const {
        return m_manual_tune_calls;
    }

    double
    lastManualTuneHz() const {
        return m_last_manual_tune_hz;
    }

  private:
    int m_manual_tune_calls = 0;
    double m_last_manual_tune_hz = 0.0;
    int m_tune_calls = 0;
    double m_last_tune_hz = 0.0;
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
                {CallHistoryModel::DetailRole, "detail"}};
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

    Q_INVOKABLE QStringList
    missingContextKeys(const QStringList& qmlFiles) const {
        const QHash<QString, QVariantMap> maps = {{QStringLiteral("metrics"), m_metrics},
                                                  {QStringLiteral("prefs"), m_prefs},
                                                  {QStringLiteral("decoderHost"), m_host}};
        /* Group 3 captures the "(" that marks a call rather than a read. */
        static const QRegularExpression read(
            QStringLiteral("\\b(metrics|prefs|decoderHost)\\.([A-Za-z_][A-Za-z0-9_]*)\\s*(\\()?"));

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
            metrics[p + QStringLiteral("CallEnc")] = false;
            metrics[p + QStringLiteral("CallSeconds")] = 0;
            metrics[p + QStringLiteral("TgText")] = QString();
            metrics[p + QStringLiteral("SrcText")] = QString();
            metrics[p + QStringLiteral("EncText")] = QString();
            metrics[p + QStringLiteral("TgId")] = 0;
        }
        // Targets the encrypted lockout is skipping; 0 is the at-rest value.
        metrics[QStringLiteral("encLockoutCount")] = 0;
        // Whether the trunking controller owns the tuner, and where it points.
        metrics[QStringLiteral("trunkingEnabled")] = false;
        metrics[QStringLiteral("centerFreqHz")] = static_cast<double>(dsd_neo_qml_stub::kSpectrumCenterHz);
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
        m_host = host;
        ctx->setContextProperty(QStringLiteral("decoderHost"), host);

        /* The real SpectrumModel over the canned getter in qml_spectrum_stub.cpp:
         * the polling, viewport and tap-snapping under test are the production
         * ones, only the frames are synthetic. */
        m_spectrum = new dsd_qt::SpectrumModel(engine);
        ctx->setContextProperty(QStringLiteral("spectrum"), m_spectrum);

        m_commands = new CommandRecorder();
        m_commands->setParent(engine);
        ctx->setContextProperty(QStringLiteral("commands"), m_commands);
    }

  private:
    QVariantMap m_metrics;
    QVariantMap m_prefs;
    QVariantMap m_host;
    QQmlEngine* m_engine = nullptr;
    dsd_qt::SpectrumModel* m_spectrum = nullptr;
    CommandRecorder* m_commands = nullptr;
};

#endif /* DSD_NEO_TESTS_UI_QML_TEST_CONTEXT_H_ */
