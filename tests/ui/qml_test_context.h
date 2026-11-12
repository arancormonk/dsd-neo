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
#include <QAccessible>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QIODevice>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QList>
#include <QMetaEnum>
#include <QMetaObject>
#include <QMetaProperty>
#include <QPoint>
#include <QPointF>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QXmlStreamReader>
#include <QtQuickTest>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <ctime>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_list_view.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/trunk_scan_lists.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include "../test_support/qt_library_import.h"
#include "../test_support/qt_test_paths.h"
#include "decryption_profiles_model.h"

#include "app_prefs.h"
#include "auto_start_attachment.h"
#include "auto_start_policy.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "decode_mode_flag.h"
#include "decoder_host.h"
#include "diagnostics_log.h"
#include "document_import_transport.h"
#include "imported_files_model.h"
#include "input_commit.h"
#include "json_store.h"
#include "known_frequency_index.h"
#include "listen_pins.h"
#include "p25_network_model.h" // WP-F2
#include "qml_backup_recorder.h"
#include "qml_gc_policy.h"
#include "qml_spectrum_stub.h"
#include "realtime_clock.h"
#include "saved_system_cards.h"
#include "saved_systems_model.h"
#include "scan_avoid_list_model.h"
#include "scan_list_starter.h"
#include "scan_list_toggle_model.h"
#include "scan_lists_model.h"
#include "screen_reader_state.h"
#include "session_args.h"
#include "site_groups.h"
#include "spectrum_model.h"
#include "spectrum_view_item.h"
#include "store_notices.h"
#include "talkgroup_filter_model.h"
#include "talkgroup_list_model.h"
#include "text_order.h"

using dsd_qt::CallHistoryFilterModel;
using dsd_qt::CallHistoryModel;

/**
 * @brief The switch that holds import copies (#832) and document probes (#831): while held, every copy or probe waits
 * before it starts until the switch opens or its request is cancelled, so a case can see an import in flight and
 * cancel it there. Held even through a cancel, a cancelled copy waits too, so a case can see the flow while it waits
 * for that answer.
 */
struct ImportCopyGate {
    std::mutex lock;
    std::condition_variable changed;
    bool held = false;          // guarded by lock
    bool holdCancelled = false; // guarded by lock
    std::set<qint64> cancelled; // guarded by lock
    int waiting = 0;            // guarded by lock: the copies held right now
    QStringList references;     // guarded by lock: every reference a copy was asked for, oldest first

    void
    hold(bool on, bool evenCancelled) {
        {
            const std::lock_guard<std::mutex> guard(lock);
            held = on;
            holdCancelled = on && evenCancelled;
        }
        changed.notify_all();
    }

    /* Whether @p request may copy now; the caller holds the lock. */
    bool
    opens(qint64 request) const {
        return !held || (!holdCancelled && cancelled.count(request) != 0);
    }
};

/** @brief The base transport behind an ImportCopyGate: the copy and probe themselves are the production local ones. */
class HeldImportTransport : public dsd_qt::DocumentImportTransport {
  public:
    explicit HeldImportTransport(std::shared_ptr<ImportCopyGate> gate) : m_gate(std::move(gate)) {}

    int
    copy(qint64 request, const QString& reference, const QString& destPath, qint64 maxBytes) override {
        {
            const std::lock_guard<std::mutex> guard(m_gate->lock);
            m_gate->references.append(reference);
        }
        wait(request);
        return DocumentImportTransport::copy(request, reference, destPath, maxBytes);
    }

    /* #831: a document probe waits at the same gate, so a case can see an incoming document while it is read. */
    int
    probe(qint64 request, const QString& reference, dsd_qt::ImportProbe* out, qint64 maxCount) override {
        wait(request);
        return DocumentImportTransport::probe(request, reference, out, maxCount);
    }

    void
    cancel(qint64 request) override {
        DocumentImportTransport::cancel(request);
        {
            const std::lock_guard<std::mutex> guard(m_gate->lock);
            m_gate->cancelled.insert(request);
        }
        m_gate->changed.notify_all();
    }

  private:
    /* Wait at the gate until it lets @p request go on. */
    void
    wait(qint64 request) {
        std::unique_lock<std::mutex> guard(m_gate->lock);
        ++m_gate->waiting;
        m_gate->changed.wait(guard, [&] { return m_gate->opens(request); });
        --m_gate->waiting;
    }

    std::shared_ptr<ImportCopyGate> m_gate;
};

/**
 * @brief Minimal DecoderHost so ImportedFilesModel has one to copy through.
 *
 * Only its import transport is used here: the base local-file copy, behind a gate the cases can hold. The lifecycle
 * members exist because the base class is abstract; the `decoderHost` the QML reads is still the plain map below,
 * which is what keeps the screens' session bindings drivable from a case.
 */
class ImportOnlyHost : public dsd_qt::DecoderHost {
    Q_OBJECT

  public:
    ImportOnlyHost() = default;
    ImportOnlyHost(const ImportOnlyHost&) = delete;
    ImportOnlyHost& operator=(const ImportOnlyHost&) = delete;

    /* A copy still held at teardown goes on, so the import worker can end. */
    ~ImportOnlyHost() override { importGate->hold(false, false); }

    /* #832: holdImports() holds the copies of every model built over this host. */
    std::shared_ptr<ImportCopyGate> importGate = std::make_shared<ImportCopyGate>();

    std::shared_ptr<dsd_qt::DocumentImportTransport>
    importTransport() const override {
        return keepImportTransport(std::make_shared<HeldImportTransport>(importGate));
    }

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
        lastArgv = argv;
        // #647: as Android's, the start replaces the last session's receiver ending before the platform is asked,
        // so a start the platform then refuses has cleared it too.
        if (health.value(QStringLiteral("state")).toString() != QStringLiteral("ok")) {
            health = healthyReceiver();
            Q_EMIT runResultChanged();
        }
        if (acceptStart) {
            phase = startRunning ? Running : Starting;
            m_running = true;
            Q_EMIT runningChanged();
            Q_EMIT sessionStateChanged();
        }
        return acceptStart;
    }

    /* The argv of the last start asked for, accepted or not: what the session-args builder made of the system. */
    QStringList lastArgv;
    qint64 locationRequested = 0;
    qint64 locationCancelRequested = 0;

    void
    requestCurrentLocation(qint64 id) override {
        locationRequested = id;
    }

    void
    cancelLocationRequest(qint64 id) override {
        locationCancelRequested = id;
    }

    /* Issue #829: the picker as the decoderHost map plays it (see ReadingMap); a case answers the requests through
     * testContext. The ids are the base host's own. */
    bool pickerSupported = true;
    QVariantMap lastDocumentRequest;
    QVariantList documentCalls;
    QMap<QString, QString> savedDocumentNames;

    bool
    documentPickerSupported() const override {
        return pickerSupported;
    }

    qint64
    requestOpenDocument() override {
        const qint64 id = nextDocumentRequestId();
        lastDocumentRequest = documentRequest(id, QStringLiteral("open"), QString(), QString());
        documentCalls.append(documentCall(QStringLiteral("requestOpenDocument"), lastDocumentRequest));
        return id;
    }

    qint64
    requestSaveDocument(const QString& suggestedName, const QString& mimeType) override {
        const qint64 id = nextDocumentRequestId();
        lastDocumentRequest = documentRequest(id, QStringLiteral("save"), suggestedName, mimeType);
        documentCalls.append(documentCall(QStringLiteral("requestSaveDocument"), lastDocumentRequest));
        return id;
    }

    void
    cancelDocumentRequest(qint64 requestId) override {
        documentCalls.append(
            documentCall(QStringLiteral("cancelDocumentRequest"), {{QStringLiteral("id"), requestId}}));
    }

    QString
    savedDocumentName(const QString& reference) const override {
        return savedDocumentNames.value(reference);
    }

    void
    discardPendingDownload(const QString& reference) override {
        documentCalls.append(
            documentCall(QStringLiteral("discardPendingDownload"), {{QStringLiteral("reference"), reference}}));
    }

    /* Issue #831: a picker request in flight, as a case sets it (testContext.setDocumentRequestPending()); the requests
     * above leave it alone. A case offers an incoming document through testContext.emitIncomingDocument(). */
    bool requestPending = false;

    bool
    documentRequestPending() const override {
        return requestPending;
    }

    void
    setDocumentRequestPending(bool pending) {
        if (requestPending != pending) {
            requestPending = pending;
            Q_EMIT documentRequestChanged();
        }
    }

    using dsd_qt::DecoderHost::offerIncomingDocument;

    /* Issue #831: whether the session state is known yet, as Android's host reports it after its first poll; a case
     * sets it through testContext.setSessionStateKnown(). */
    bool stateKnown = true;

    bool
    sessionStateKnown() const override {
        return stateKnown;
    }

    void
    setSessionStateKnown(bool known) {
        if (stateKnown != known) {
            stateKnown = known;
            Q_EMIT sessionStateKnownChanged();
        }
    }

    /* Issue #831: a recording save or export that runs on after its sheet closed. While held, a transfer begun through
     * saveRecording()/exportRecordings() stays active until endHeldTransfer(); otherwise the base host's runs. */
    bool holdTransfers = false;

    /* The host offers exports, as Android's does, for its whole life: the property is CONSTANT, so the answer never
     * follows holdTransfers. An export that is not held goes to the base host, which has no zip and fails it. */
    bool
    recordingExportSupported() const override {
        return true;
    }

    void
    endHeldTransfer() {
        if (recordingTransferActive()) {
            finishRecordingTransfer(TransferDone, {});
        }
    }

  protected:
    bool
    beginRecordingTransfer(const QStringList& paths, const QStringList& names, const QString& reference,
                           bool zip) override {
        return holdTransfers || dsd_qt::DecoderHost::beginRecordingTransfer(paths, names, reference, zip);
    }

  public:
    /** @brief What lastDocumentRequest() reports: {id, kind ("open" or "save"), name, mime}. */
    static QVariantMap
    documentRequest(qint64 id, const QString& kind, const QString& name, const QString& mime) {
        return {{QStringLiteral("id"), id},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("name"), name},
                {QStringLiteral("mime"), mime}};
    }

    /** @brief A recorded picker call, shaped as ReadingMap's calls are: {name, ...}. */
    static QVariantMap
    documentCall(const QString& name, QVariantMap args) {
        args.insert(QStringLiteral("name"), name);
        return args;
    }

    /* Issue #830: the file browser as the decoderHost map plays it (see ReadingMap). Each call records itself in
     * fileCalls; a request returns a new id from the picker's sequence and is never answered here: a case answers it
     * through testContext (answerFileRoots() and the like). */
    bool browserSupported = false;
    QVariantMap access = fileAccessReading();
    QVariantList fileCalls;

    bool
    fileBrowserSupported() const override {
        return browserSupported;
    }

    QVariantMap
    fileAccess() const override {
        return access;
    }

    void
    refreshFileAccess() override {
        fileCalls.append(fileCall(QStringLiteral("refreshFileAccess"), {}));
    }

    qint64
    openFileBrowserSession() override {
        const qint64 id = nextDocumentRequestId();
        fileCalls.append(fileCall(QStringLiteral("openFileBrowserSession"),
                                  {{QStringLiteral("id"), id}, {QStringLiteral("session"), id}}));
        return id;
    }

    void
    closeFileBrowserSession(qint64 sessionId) override {
        fileCalls.append(fileCall(QStringLiteral("closeFileBrowserSession"), {{QStringLiteral("session"), sessionId}}));
    }

    qint64
    requestFileRoots(qint64 sessionId) override {
        return recordFileRequest(QStringLiteral("requestFileRoots"), {{QStringLiteral("session"), sessionId}});
    }

    qint64
    requestFolderListing(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& path,
                         const QVariantMap& filter) override {
        return recordFileRequest(QStringLiteral("requestFolderListing"),
                                 folderListingArgs(sessionId, rootKey, generation, path, filter));
    }

    qint64
    requestMorePage(qint64 sessionId, qint64 snapshot, int offset) override {
        return recordFileRequest(QStringLiteral("requestMorePage"), morePageArgs(sessionId, snapshot, offset));
    }

    qint64
    requestFileHandoff(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& reference) override {
        return recordFileRequest(QStringLiteral("requestFileHandoff"),
                                 fileHandoffArgs(sessionId, rootKey, generation, reference));
    }

    qint64
    requestFileAccess(qint64 sessionId) override {
        return recordFileRequest(QStringLiteral("requestFileAccess"), {{QStringLiteral("session"), sessionId}});
    }

    void
    cancelFileRequest(qint64 requestId) override {
        fileCalls.append(fileCall(QStringLiteral("cancelFileRequest"), {{QStringLiteral("id"), requestId}}));
    }

    /** @brief The fileAccess reading the fakes start with: All files access declared, not granted (API 36). */
    static QVariantMap
    fileAccessReading() {
        return {{QStringLiteral("declared"), true},
                {QStringLiteral("granted"), false},
                {QStringLiteral("route"), QStringLiteral("settings")},
                {QStringLiteral("apiLevel"), 36},
                {QStringLiteral("rootsGeneration"), 1}};
    }

    /**
     * @brief A recorded file browser call: {name, id, session, rootKey, generation, path, filter, snapshot, offset,
     * reference}, each key there, the ones the call has no value for empty or 0.
     */
    static QVariantMap
    fileCall(const QString& name, const QVariantMap& args) {
        QVariantMap call{{QStringLiteral("name"), name},
                         {QStringLiteral("id"), qint64(0)},
                         {QStringLiteral("session"), qint64(0)},
                         {QStringLiteral("rootKey"), QString()},
                         {QStringLiteral("generation"), qint64(0)},
                         {QStringLiteral("path"), QString()},
                         {QStringLiteral("filter"), QVariantMap()},
                         {QStringLiteral("snapshot"), qint64(0)},
                         {QStringLiteral("offset"), 0},
                         {QStringLiteral("reference"), QString()}};
        for (auto it = args.cbegin(); it != args.cend(); ++it) {
            call.insert(it.key(), it.value());
        }
        return call;
    }

    static QVariantMap
    folderListingArgs(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& path,
                      const QVariantMap& filter) {
        return {{QStringLiteral("session"), sessionId},
                {QStringLiteral("rootKey"), rootKey},
                {QStringLiteral("generation"), generation},
                {QStringLiteral("path"), path},
                {QStringLiteral("filter"), filter}};
    }

    static QVariantMap
    morePageArgs(qint64 sessionId, qint64 snapshot, int offset) {
        return {{QStringLiteral("session"), sessionId},
                {QStringLiteral("snapshot"), snapshot},
                {QStringLiteral("offset"), offset}};
    }

    static QVariantMap
    fileHandoffArgs(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& reference) {
        return {{QStringLiteral("session"), sessionId},
                {QStringLiteral("rootKey"), rootKey},
                {QStringLiteral("generation"), generation},
                {QStringLiteral("reference"), reference}};
    }

    /** @brief The file browser calls a host records, by name. */
    static const QStringList&
    fileCallNames() {
        static const QStringList names = {
            QStringLiteral("refreshFileAccess"),       QStringLiteral("openFileBrowserSession"),
            QStringLiteral("closeFileBrowserSession"), QStringLiteral("requestFileRoots"),
            QStringLiteral("requestFolderListing"),    QStringLiteral("requestMorePage"),
            QStringLiteral("requestFileHandoff"),      QStringLiteral("requestFileAccess"),
            QStringLiteral("cancelFileRequest")};
        return names;
    }

    qreal keyboardBoundary = -1;

    qreal
    keyboardTop() const override {
        return keyboardBoundary;
    }

    bool delayedStop = false; // WP-D4: stop acknowledgement is a separate edge.

    Q_INVOKABLE int
    stopCalls() const {
        return m_stop_calls;
    }

    Q_INVOKABLE int
    backgroundCalls() const {
        return m_background_calls;
    }

    /* #782: Main's failure card reports its dismissal, so the host can give up the attempt behind it. */
    void
    dismissFailure() override {
        ++m_dismiss_calls;
    }

    Q_INVOKABLE int
    dismissCalls() const {
        return m_dismiss_calls;
    }

    bool
    moveToBackground() override {
        ++m_background_calls;
        // Simulate Android consuming Back so the fixture window stays open for assertions.
        return true;
    }

    void
    stop() override {
        ++m_stop_calls;
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

    /* How the last file replay ended (1 completed, 2 stopped), as the native session reports it (issue #801). */
    int terminal = 0;

    int
    terminalReason() const override {
        return terminal;
    }

    void
    setPhase(SessionState value) {
        phase = value;
        Q_EMIT sessionStateChanged();
    }

    QString failure;

    QString
    failureText() const override {
        return failure;
    }

    int failureKind = 0;

    int
    inputFailureKind() const override {
        return failureKind;
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

    // #647: what the last request asked for, and Android's acknowledgement of an ended session's receiver.
    void
    requestLocalDeviceAccessForSource(const QString& source, const QString& serial = QString()) override {
        accessSource = source;
        accessSerial = serial;
        ++accessForSourceRequests;
        requestLocalDeviceAccess();
    }

    QString accessSource;
    QString accessSerial;
    int accessForSourceRequests = 0;

    void
    requestLocalDeviceAccess() override {
        if (!sessionActive() && health.value(QStringLiteral("state")).toString() != QStringLiteral("ok")) {
            // A Failed session the receiver ended is retired with its notice, the failure first (Android's
            // acknowledgeReceiverEnding()).
            if (phase == Failed) {
                phase = Idle;
                failure.clear();
                Q_EMIT sessionStateChanged();
            }
            health = healthyReceiver();
            Q_EMIT runResultChanged();
        }
        ++usbRequests;
        // As Android does: the permission prompt is up until it is answered.
        if (requestPends) {
            usbPending = true;
            Q_EMIT localDeviceChanged();
        }
    }

    bool requestPends = false;

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

    // The receiver kind plugged in, as the Android host reads it off the bus.
    QString
    attachedDeviceSource() const override {
        return attachedSource;
    }

    QString attachedSource;

    // #647: the connected receiver's kind. "usb" until a case sets it, which is
    // what the base host answers, so cases that only connect an RTL-SDR read as before.
    QString
    localDeviceSource() const override {
        return usbSource;
    }

    bool
    localDeviceRequestPending() const override {
        return usbPending;
    }

    QString
    localDeviceRequestedSource() const override {
        return usbRequestedSource;
    }

    QVariantMap
    receiverHealth() const override {
        return health;
    }

    QString usbSource = QStringLiteral("usb");
    bool usbPending = false;
    QString usbRequestedSource;
    QVariantMap health = healthyReceiver();

    bool usbBrokered = false;
    bool usbReady = false;
    int usbRequests = 0;

    // Issue #574: the Settings screen's Screen rows gate on this capability.
    bool
    screenPolicySupported() const override {
        return screenPolicy;
    }

    bool screenPolicy = false;

    bool acceptStart = false;
    bool startRunning = false;

    bool
    notificationPermissionNeeded() const override {
        return m_notification_permission_needed;
    }

    void
    setNotificationPermissionNeeded(bool needed) {
        if (m_notification_permission_needed != needed) {
            m_notification_permission_needed = needed;
            Q_EMIT notificationPermissionChanged();
        }
    }

  private:
    bool m_running = false;
    int m_stop_calls = 0;
    int m_background_calls = 0;
    int m_dismiss_calls = 0;
    bool m_notification_permission_needed = false;

    /* Record file request @p name with @p args under a new id, and return the id; nothing answers it. */
    qint64
    recordFileRequest(const QString& name, QVariantMap args) {
        const qint64 id = nextDocumentRequestId();
        args.insert(QStringLiteral("id"), id);
        fileCalls.append(fileCall(name, args));
        return id;
    }
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

/**
 * @brief The `replayFiles` the wizard copies a picked replay through (#832), scripted by the case.
 *
 * The production ReplayStore copies content URIs, and local files too where the host has DSD-neo's file browser
 * (elsewhere a local file is answered with its own path); this desktop suite makes neither copy. So a case drives the copy's progress and its answer itself, through
 * testContext.replayProgress() and finishReplay(); the surface QML reads is the store's.
 */
class ReplayFilesFake : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool copying READ copying NOTIFY progressChanged)
    Q_PROPERTY(QString copyName READ copyName NOTIFY progressChanged)
    Q_PROPERTY(qint64 copyBytes READ copyBytes NOTIFY progressChanged)
    Q_PROPERTY(qint64 copyTotal READ copyTotal NOTIFY progressChanged)

  public:
    using QObject::QObject;

    /* As the store's: a copy still running is cancelled first. Where the host has no file browser (the desktop), a
     * local file (a file:// URL or an absolute path) is never copied: it is answered later with its own path, and no
     * copy runs meanwhile. With one (#830, copiesLocalFiles), a local file copies like a provider's document. */
    Q_INVOKABLE int
    beginReplayImport(const QString& reference, const QString& fileName) {
        if (m_current != 0) {
            dropCurrent();
        }
        ++begins;
        lastReference = reference;
        const QString local = copiesLocalFiles ? QString() : dsd_qt::local_document_path(reference);
        if (!local.isEmpty()) {
            const int id = ++m_next;
            const QVariantMap answer{{QStringLiteral("ok"), true},
                                     {QStringLiteral("path"), QDir::cleanPath(local)},
                                     {QStringLiteral("name"), QFileInfo(local).fileName()}};
            QMetaObject::invokeMethod(
                this, [this, id, answer]() { Q_EMIT replayImportFinished(id, answer); }, Qt::QueuedConnection);
            return id;
        }
        m_current = ++m_next;
        m_name = fileName;
        m_bytes = 0;
        m_total = -1;
        Q_EMIT progressChanged();
        return m_current;
    }

    /* As the store's: the running copy stops copying and answers "cancelled" later, from the event loop, and never
     * again; finishReplay() then has no copy to answer. */
    Q_INVOKABLE void
    cancelReplayImport() {
        if (m_current != 0) {
            ++cancels;
            dropCurrent();
        }
    }

    bool
    copying() const {
        return m_current != 0;
    }

    QString
    copyName() const {
        return m_name;
    }

    qint64
    copyBytes() const {
        return m_bytes;
    }

    qint64
    copyTotal() const {
        return m_total;
    }

    void
    progress(qint64 bytes, qint64 total) {
        m_bytes = bytes;
        m_total = total;
        Q_EMIT progressChanged();
    }

    /* Answer the running copy with @p result (its name filled in when the case leaves it out); false with none. */
    bool
    finish(QVariantMap result) {
        if (m_current == 0) {
            return false;
        }
        const int id = m_current;
        if (!result.contains(QStringLiteral("name"))) {
            result.insert(QStringLiteral("name"), m_name);
        }
        m_current = 0;
        Q_EMIT progressChanged();
        Q_EMIT replayImportFinished(id, result);
        return true;
    }

    /* As the store's: the copy this session's migration moved @p path to (that uid's first, else any), else @p path. */
    Q_INVOKABLE QString
    migratedPath(const QString& uid, const QString& path) const {
        QString any;
        for (const auto& move : m_migrated) {
            if (move.value(1) == path && move.value(0) == uid) {
                return move.value(2);
            }
            if (move.value(1) == path && any.isEmpty()) {
                any = move.value(2);
            }
        }
        return any.isEmpty() ? path : any;
    }

    /* The migration repointed the system @p uid from @p from to @p to, as the store's commit does. */
    void
    migrate(const QString& uid, const QString& from, const QString& to) {
        m_migrated.append({uid, from, to});
        Q_EMIT replaysMigrated();
    }

    void
    reset() {
        m_migrated.clear();
        m_current = 0;
        m_name.clear();
        m_bytes = 0;
        m_total = -1;
        begins = 0;
        cancels = 0;
        lastReference.clear();
        Q_EMIT progressChanged();
    }

    /* The host has DSD-neo's file browser (testContext.setFileBrowserSupported()), so local picks copy too. */
    bool copiesLocalFiles = false;
    int begins = 0;
    int cancels = 0;
    QString lastReference;

  Q_SIGNALS:
    void replayImportFinished(int id, const QVariantMap& result);
    void progressChanged();
    void replaysMigrated();

  private:
    /* The running copy answers "cancelled", queued, and no copy runs from here on. */
    void
    dropCurrent() {
        const int previous = m_current;
        const QVariantMap cancelled{{QStringLiteral("ok"), false},
                                    {QStringLiteral("error"), QStringLiteral("cancelled")},
                                    {QStringLiteral("name"), m_name}};
        QMetaObject::invokeMethod(
            this, [this, previous, cancelled]() { Q_EMIT replayImportFinished(previous, cancelled); },
            Qt::QueuedConnection);
        m_current = 0;
        Q_EMIT progressChanged();
    }

    int m_current = 0;
    int m_next = 0;
    QString m_name;
    qint64 m_bytes = 0;
    qint64 m_total = -1;
    QList<QStringList> m_migrated; // {uid, from, to}
};

// The history model itself is already real in these fixtures; the controller
// stub only acknowledges the documented same-thread flush before a start.
/**
 * @brief The `restartCloser` (#830) Main and the file browser call, scripted by the case.
 *
 * begin() is refused as the production closer's is (refused() or already active) and otherwise only counts and turns
 * active; finish() ends it as the closer would: "closing" keeps it active, any other outcome clears it.
 */
class RestartCloserFake : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
  public:
    using QObject::QObject;

    bool
    active() const {
        return m_active;
    }

    Q_INVOKABLE bool
    refused() const {
        return closeRefused;
    }

    Q_INVOKABLE bool
    begin() {
        if (closeRefused || m_active) {
            return false;
        }
        ++begins;
        setActive(true);
        return true;
    }

    void
    finish(const QString& outcome) {
        if (outcome != QStringLiteral("closing")) {
            setActive(false);
        }
        Q_EMIT finished(outcome);
    }

    bool closeRefused = false;
    int begins = 0;

  Q_SIGNALS:
    void activeChanged();
    void finished(const QString& outcome);

  private:
    void
    setActive(bool on) {
        if (m_active != on) {
            m_active = on;
            Q_EMIT activeChanged();
        }
    }

    bool m_active = false;
};

/**
 * @brief The `rrPlaces` service (RrPlaceService): its QML surface, recorded rather than run.
 *
 * The properties are exactly the real service's (UI_QT_RR_PLACE_SERVICE pins that list), so missingContextKeys() holds
 * a screen's reads to what production publishes. A case scripts it through the C++ members.
 */
class RrPlacesFake : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool updating READ updating NOTIFY updatingChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressTextChanged)
    Q_PROPERTY(QString summaryText READ summaryText NOTIFY summaryTextChanged)
    Q_PROPERTY(int unplacedCount READ unplacedCount NOTIFY unplacedCountChanged)
    Q_PROPERTY(QString unplacedText READ unplacedText NOTIFY unplacedCountChanged)
  public:
    using QObject::QObject;

    bool
    updating() const {
        return m_updating;
    }

    QString
    progressText() const {
        return m_updating ? QStringLiteral("Updating 1 of %1").arg(updateUids.size()) : QString();
    }

    QString
    summaryText() const {
        return m_summary;
    }

    int
    unplacedCount() const {
        return m_unplaced;
    }

    QString
    unplacedText() const {
        if (m_unplaced <= 0) {
            return QString();
        }
        return m_unplaced == 1 ? QStringLiteral("1 saved system has no place yet")
                               : QStringLiteral("%1 saved systems have no place yet").arg(m_unplaced);
    }

    Q_INVOKABLE void
    resolveStates(const QVariantList& stids) {
        resolveRequests.append(QVariant(stids));
    }

    /** @brief As the service: the rows of the cards unplacedCount counts; here, what the case scripted. */
    Q_INVOKABLE QStringList
    unplacedUids() const {
        return unplacedUidList;
    }

    /** @brief As the service: false, changing nothing, while an update runs or while signed out; otherwise an update
     *         runs until finish(). Every call is counted. */
    Q_INVOKABLE bool
    updateFromRadioReference(const QStringList& uids) {
        ++updateCalls;
        if (m_updating || !signedIn) {
            return false;
        }
        updateUids = uids;
        m_updating = true;
        Q_EMIT updatingChanged();
        Q_EMIT progressTextChanged();
        return true;
    }

    Q_INVOKABLE void
    cancelUpdate() {
        if (m_updating) {
            ++cancels;
            finish(0, 0);
        }
    }

    /** @brief End the running update with the service's summary and signal. */
    void
    finish(int updated, int unreachable) {
        m_updating = false;
        m_summary =
            updated == 1 ? QStringLiteral("Updated 1 system.") : QStringLiteral("Updated %1 systems.").arg(updated);
        if (unreachable > 0) {
            m_summary += QStringLiteral(" %1 could not be reached.").arg(unreachable);
        }
        Q_EMIT updatingChanged();
        Q_EMIT progressTextChanged();
        Q_EMIT summaryTextChanged();
        Q_EMIT updateFinished(updated, unreachable);
    }

    /** @brief No summary, as before any update. */
    void
    clearSummary() {
        if (!m_summary.isEmpty()) {
            m_summary.clear();
            Q_EMIT summaryTextChanged();
        }
    }

    void
    setUnplacedCount(int count) {
        if (m_unplaced != count) {
            m_unplaced = count;
            Q_EMIT unplacedCountChanged();
        }
    }

    bool signedIn = true;
    QStringList unplacedUidList;
    QStringList updateUids;
    QVariantList resolveRequests;
    int cancels = 0;
    int updateCalls = 0;

  Q_SIGNALS:
    void updatingChanged();
    void progressTextChanged();
    void summaryTextChanged();
    void unplacedCountChanged();
    void updateFinished(int updated, int unreachable);

  private:
    bool m_updating = false;
    QString m_summary;
    int m_unplaced = 0;
};

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

    /* Issue #697: @p companions, the other lists a scan target was last started with, ride along as the real
       controller's do. */
    Q_INVOKABLE void
    requestAutoStart(const QString& kind, const QString& uid, const QStringList& companions = QStringList()) {
        Q_EMIT autoStartRequested(kind, uid, companions);
    }

    Q_INVOKABLE void
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    flushHistory() {}

  Q_SIGNALS:
    void autoStartRequested(const QString& kind, const QString& uid, const QStringList& companions);
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
    /* Issue #518: the session-edit fields and actions, as CommandBridge publishes them. */
    Q_PROPERTY(int scanRowFieldSquelch READ scanRowFieldSquelch CONSTANT)
    Q_PROPERTY(int scanRowFieldWidth READ scanRowFieldWidth CONSTANT)
    Q_PROPERTY(int scanRowFieldTone READ scanRowFieldTone CONSTANT)
    Q_PROPERTY(int scanRowFieldGain READ scanRowFieldGain CONSTANT)
    Q_PROPERTY(int scanRowEditSet READ scanRowEditSet CONSTANT)
    Q_PROPERTY(int scanRowEditInherit READ scanRowEditInherit CONSTANT)
    Q_PROPERTY(int scanRowEditReset READ scanRowEditReset CONSTANT)
    /* The squelch modes and the auto squelch's margin range, as CommandBridge publishes them. */
    Q_PROPERTY(int squelchModeLevel READ squelchModeLevel CONSTANT)
    Q_PROPERTY(int squelchModeAuto READ squelchModeAuto CONSTANT)
    Q_PROPERTY(int squelchModeNoise READ squelchModeNoise CONSTANT)
    Q_PROPERTY(int squelchMarginMinDb READ squelchMarginMinDb CONSTANT)
    Q_PROPERTY(int squelchMarginMaxDb READ squelchMarginMaxDb CONSTANT)
    Q_PROPERTY(int squelchMarginDefaultDb READ squelchMarginDefaultDb CONSTANT)
    quint64 m_next_key_request = 1;
    QString m_last_key_request;
    QVariantList m_last_selection;

  public:
    static int
    scanRowFieldSquelch() {
        return DSD_SCAN_ROW_FIELD_SQUELCH;
    }

    static int
    scanRowFieldWidth() {
        return DSD_SCAN_ROW_FIELD_WIDTH;
    }

    static int
    scanRowFieldTone() {
        return DSD_SCAN_ROW_FIELD_TONE;
    }

    static int
    scanRowFieldGain() {
        return DSD_SCAN_ROW_FIELD_GAIN;
    }

    static int
    scanRowEditSet() {
        return DSD_SCAN_ROW_EDIT_SET;
    }

    static int
    scanRowEditInherit() {
        return DSD_SCAN_ROW_EDIT_INHERIT;
    }

    static int
    scanRowEditReset() {
        return DSD_SCAN_ROW_EDIT_RESET;
    }

    static int
    squelchModeLevel() {
        return DSD_SQUELCH_MODE_LEVEL;
    }

    static int
    squelchModeAuto() {
        return DSD_SQUELCH_MODE_AUTO;
    }

    static int
    squelchModeNoise() {
        return DSD_SQUELCH_MODE_NOISE;
    }

    static int
    squelchMarginMinDb() {
        return DSD_SQUELCH_MARGIN_MIN_DB;
    }

    static int
    squelchMarginMaxDb() {
        return DSD_SQUELCH_MARGIN_MAX_DB;
    }

    static int
    squelchMarginDefaultDb() {
        return DSD_SQUELCH_MARGIN_DEFAULT_DB;
    }

    /* CommandBridge::receiveError(): what a receive filter or service names editor would refuse. The receive policy
     * sheets the Monitor hosts read it while open; every edit is valid here (tst_receive_policy_sheet.qml checks the
     * refusals with a bridge of its own). */
    Q_INVOKABLE QString
    receiveError(int, bool, const QString&, const QString&, const QVariantList&) const {
        return {};
    }

    /* The row on air an editor captures (setScanRowContext()), and the session edits it then sends. */
    Q_INVOKABLE QVariantMap
    scanRowContext() const {
        return m_scan_row_context;
    }

    void
    setScanRowContext(const QVariantMap& context) {
        m_scan_row_context = context;
    }

    Q_INVOKABLE bool
    editScanRow(const QVariantMap& context, int field, int action, const QVariantMap& value) {
        m_last_scan_row_edit = {{"context", context}, {"field", field}, {"action", action}, {"value", value}};
        m_scan_row_edit_calls++;
        return true;
    }

    int
    scanRowEditCalls() const {
        return m_scan_row_edit_calls;
    }

    QVariantMap
    lastScanRowEdit() const {
        return m_last_scan_row_edit;
    }

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
    applyDecryptionProfile(const QString&, int scope, const QVariantMap& context) {
        m_last_profile_scope = scope;
        m_last_profile_target = context.value(QStringLiteral("target")).toString();
        m_last_key_request = QString::number(m_next_key_request++);
        return m_last_key_request;
    }

    /** @brief The scope the last applyDecryptionProfile() sent (0 session defaults, 1 scan target); -1 for none. */
    Q_INVOKABLE int
    lastProfileScope() const {
        return m_last_profile_scope;
    }

    /** @brief The scan target ID in the context the last applyDecryptionProfile() sent; empty for none. */
    Q_INVOKABLE QString
    lastProfileTarget() const {
        return m_last_profile_target;
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

    /* The talkgroup list and channel map commands, as "<method> <path>" in the order the screens posted them, so a
     * case can tell a reload from an import and see a clear posted alongside one. */
    Q_INVOKABLE bool
    importGroupList(const QString& path) {
        return recordListCommand(QStringLiteral("importGroupList"), path);
    }

    Q_INVOKABLE bool
    clearGroupList() {
        return recordListCommand(QStringLiteral("clearGroupList"), QString());
    }

    Q_INVOKABLE bool
    reloadGroupList(const QString& path) {
        return recordListCommand(QStringLiteral("reloadGroupList"), path);
    }

    Q_INVOKABLE bool
    importChannelMap(const QString& path) {
        return recordListCommand(QStringLiteral("importChannelMap"), path);
    }

    Q_INVOKABLE bool
    clearChannelMap() {
        return recordListCommand(QStringLiteral("clearChannelMap"), QString());
    }

    Q_INVOKABLE bool
    reloadChannelMap(const QString& path) {
        return recordListCommand(QStringLiteral("reloadChannelMap"), path);
    }

    Q_INVOKABLE QStringList
    listCommands() const {
        return m_list_commands;
    }

    /* Whether the queue takes the commands above (CommandBridge's "queued"); a refused one is still recorded. */
    Q_INVOKABLE void
    setListCommandsAccepted(bool accepted) {
        m_list_commands_accepted = accepted;
    }

    Q_INVOKABLE bool
    manualTuneHz(unsigned int hz) {
        m_manual_tune_calls++;
        m_last_manual_tune_hz = hz;
        return true;
    }

    /* The override tune the spectrum's Go to sends (issue #671): counted apart from the taps' manualTuneHz(), so a
     * case can tell which of the two a screen asked for. */
    Q_INVOKABLE bool
    tuneHz(unsigned int hz) {
        m_tune_hz_calls++;
        m_last_tune_hz = hz;
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
    lockoutSlot(int slot) {
        ++m_lockout_slot_calls;
        m_last_lockout_slot = slot;
        return true;
    }

    Q_INVOKABLE bool
    skipSlot(int slot) {
        ++m_skip_slot_calls;
        m_last_skip_slot = slot;
        return true;
    }

    Q_INVOKABLE bool
    setPersistTgLockouts(bool value) {
        m_lockout_requests.append(value);
        return m_lockout_accepted;
    }

    /* Relaxed CRC set to a value (DSD_APP_CMD_AGGR_SYNC_TOGGLE with a payload). */
    Q_INVOKABLE bool
    setRelaxedCrc(bool on) {
        m_relaxed_crc_requests.append(on);
        return m_relaxed_crc_accepted;
    }

    Q_INVOKABLE void
    setRelaxedCrcAccepted(bool accepted) {
        m_relaxed_crc_accepted = accepted;
    }

    Q_INVOKABLE QVariantList
    relaxedCrcRequests() const {
        return m_relaxed_crc_requests;
    }

    /* Issue #708: Record calls and unknown-only applied to a running session, each request as {kind, on} (and
     * unknownOnly, analogMaxGapMs and analogPrerollMs for Record calls); issue #756's analog pause hold and issue #668's
     * NFM pre-roll as {kind, ms}. */
    Q_INVOKABLE bool
    setRecordCalls(bool on, bool unknownOnly, int analogMaxGapMs, int analogPrerollMs) {
        m_recording_requests.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("recordCalls")},
                                                {QStringLiteral("on"), on},
                                                {QStringLiteral("unknownOnly"), unknownOnly},
                                                {QStringLiteral("analogMaxGapMs"), analogMaxGapMs},
                                                {QStringLiteral("analogPrerollMs"), analogPrerollMs}});
        return recordingAnswer();
    }

    Q_INVOKABLE bool
    setAnalogRecordMaxGap(int ms) {
        m_recording_requests.append(
            QVariantMap{{QStringLiteral("kind"), QStringLiteral("analogMaxGap")}, {QStringLiteral("ms"), ms}});
        return recordingAnswer();
    }

    Q_INVOKABLE bool
    setAnalogRecordPreroll(int ms) {
        m_recording_requests.append(
            QVariantMap{{QStringLiteral("kind"), QStringLiteral("analogPreroll")}, {QStringLiteral("ms"), ms}});
        return recordingAnswer();
    }

    Q_INVOKABLE bool
    setRecordUnknownOnly(bool on) {
        m_recording_requests.append(
            QVariantMap{{QStringLiteral("kind"), QStringLiteral("recordUnknownOnly")}, {QStringLiteral("on"), on}});
        return recordingAnswer();
    }

    Q_INVOKABLE void
    setRecordingAccepted(bool accepted) {
        m_recording_accepted = accepted;
    }

    /**
     * @brief Refuse the next switches for @p reason, noting it in `recordingStatus` as CommandBridge does
     * (RecordingStatus::noteRefusal(): the reason, and one more refusal counted); empty: a refused switch notes nothing,
     * as a submit the queue would not take.
     */
    Q_INVOKABLE void
    setRecordingRefusal(const QString& reason) {
        m_recording_refusal = reason;
    }

    /* The `recordingStatus` readings a refusal is noted in. */
    QQmlPropertyMap* recordingStatus = nullptr;

    Q_INVOKABLE QVariantList
    recordingRequests() const {
        return m_recording_requests;
    }

    bool
    recordingAnswer() {
        if (!m_recording_accepted && !m_recording_refusal.isEmpty() && recordingStatus != nullptr) {
            recordingStatus->insert(QStringLiteral("refusal"), m_recording_refusal);
            recordingStatus->insert(QStringLiteral("refusalCount"),
                                    recordingStatus->value(QStringLiteral("refusalCount")).toInt() + 1);
        }
        return m_recording_accepted;
    }

    Q_INVOKABLE bool
    clearTemporaryTgAvoids(const QString& context) {
        m_avoid_clear_requests.append(context);
        return m_lockout_accepted;
    }

    Q_INVOKABLE void
    setLockoutAccepted(bool accepted) {
        m_lockout_accepted = accepted;
    }

    Q_INVOKABLE QVariantList
    lockoutRequests() const {
        return m_lockout_requests;
    }

    Q_INVOKABLE QStringList
    avoidClearRequests() const {
        return m_avoid_clear_requests;
    }

    // WP-D1: retain the captured version and requested fields for edit-sheet tests.
    Q_INVOKABLE bool
    setTalkgroupPolicy(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                       const QVariantMap& changes, int tableIndex = -1, const QString& pattern = QString()) {
        m_talkgroupEdit = {start, end, context, generation, changes, tableIndex, pattern};
        return true;
    }

    Q_INVOKABLE bool
    renameTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                    const QString& name, int tableIndex = -1, const QString& pattern = QString()) {
        m_talkgroupEdit = {start, end, context, generation, name, tableIndex, pattern};
        return true;
    }

    Q_INVOKABLE bool
    addTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                 const QString& name, bool listen, int priority, bool preempt, bool record = true) {
        m_talkgroupEdit = {start, end, context, generation, name, listen, priority, preempt, record};
        return true;
    }

    Q_INVOKABLE bool
    removeTalkgroup(unsigned int start, unsigned int end, const QString& context, unsigned int generation,
                    int tableIndex = -1, const QString& pattern = QString()) {
        m_talkgroupEdit = {start, end, context, generation, tableIndex, pattern};
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
    setTalkgroupListening(double idStart, double idEnd, bool listen, const QString& context = QString(),
                          unsigned int generation = 0, int tableIndex = -1, const QString& pattern = QString()) {
        m_talkgroup_listen_calls++;
        m_last_talkgroup_id_start = idStart;
        m_last_talkgroup_id_end = idEnd;
        m_last_talkgroup_listen = listen;
        m_last_talkgroup_listen_token = {
            {"context", context}, {"generation", generation}, {"tableIndex", tableIndex}, {"pattern", pattern}};
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

    /* Issue #672: each Put back, by the key it names. */
    Q_INVOKABLE bool
    restoreScanAvoid(const QString& key) {
        m_restored_scan_avoids << key;
        return true;
    }

    Q_INVOKABLE bool
    previousChannel() {
        m_previous_channel_calls++;
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
        return m_release_tuner_accepted;
    }

    /** @brief Whether releaseTuner() reports the command queued, so a case can refuse it. */
    Q_INVOKABLE void
    setReleaseTunerAccepted(bool accepted) {
        m_release_tuner_accepted = accepted;
    }

    Q_INVOKABLE bool
    resumeScan() {
        m_resume_scan_calls++;
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
    setAirspy(const QString& key, const QString& value) {
        (void)key;
        (void)value;
        m_airspy_calls++;
        return true;
    }

    Q_INVOKABLE bool
    setSquelchDb(double db) {
        m_last_squelch_db = db;
        m_squelch_calls++;
        return m_radio_accepted;
    }

    /* Refuses a margin out of range, as CommandBridge does. */
    Q_INVOKABLE bool
    setSquelchAuto(int marginDb) {
        if (marginDb < DSD_SQUELCH_MARGIN_MIN_DB || marginDb > DSD_SQUELCH_MARGIN_MAX_DB) {
            return false;
        }
        m_last_squelch_margin_db = marginDb;
        m_squelch_auto_calls++;
        return m_radio_accepted;
    }

    /* Refuses a margin out of range, as CommandBridge does. */
    Q_INVOKABLE bool
    setSquelchNoise(int marginDb) {
        if (marginDb < DSD_SQUELCH_MARGIN_MIN_DB || marginDb > DSD_SQUELCH_MARGIN_MAX_DB) {
            return false;
        }
        m_last_squelch_margin_db = marginDb;
        m_squelch_noise_calls++;
        return m_radio_accepted;
    }

    Q_INVOKABLE bool
    restoreSquelchLevel() {
        m_restore_squelch_calls++;
        return m_radio_accepted;
    }

    Q_INVOKABLE bool
    setPpm(int ppm) {
        m_last_ppm = ppm;
        return true;
    }

    Q_INVOKABLE bool
    setNfmBandwidthHz(int hz) {
        m_last_nfm_bandwidth_hz = hz;
        m_nfm_bandwidth_calls++;
        return true;
    }

    Q_INVOKABLE bool
    setAmBandwidthHz(int hz) {
        m_last_am_bandwidth_hz = hz;
        m_am_bandwidth_calls++;
        return true;
    }

    Q_INVOKABLE bool
    setModulation(int modulation) {
        m_last_modulation = modulation;
        return m_radio_accepted;
    }

    Q_INVOKABLE void
    setRadioAccepted(bool accepted) {
        m_radio_accepted = accepted;
    }

    /* The live tone-filter editor (#527): what it sends, and the production validator's answer for its inline check,
     * so a case sees the message the decoder would refuse with. */
    Q_INVOKABLE bool
    setToneFilter(int mode, const QString& list) {
        m_last_tone_filter_mode = mode;
        m_last_tone_filter_list = list;
        m_tone_filter_calls++;
        return true;
    }

    Q_INVOKABLE QString
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    toneFilterError(int mode, const QString& list) const {
        const QByteArray utf8 = list.toUtf8();
        dsd_tone_set set;
        char err[DSD_TONE_LIST_ERROR_SIZE] = "";
        return dsd_tone_filter_check(mode, utf8.constData(), &set, err, sizeof err) == 0 ? QString()
                                                                                         : QString::fromUtf8(err);
    }

    Q_INVOKABLE bool
    setDecodeMode(int mode) {
        m_last_decode_mode = mode;
        return m_radio_accepted;
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
        m_lockout_slot_calls = 0;
        m_last_lockout_slot = -1;
        m_skip_slot_calls = 0;
        m_last_skip_slot = -1;
        m_hold_calls = 0;
        m_last_hold_tg = 0;
        m_key_apply_calls = 0;
        m_last_profile_scope = -1;
        m_last_profile_target.clear();
        m_key_payload_valid = false;
        m_key_accepted = true;
        m_last_force_mode = -1;
        m_src_import_calls = 0;
        m_src_clear_calls = 0;
        m_last_src_path.clear();
        m_list_commands.clear();
        m_list_commands_accepted = true;
        m_manual_tune_calls = 0;
        m_last_manual_tune_hz = 0U;
        m_release_tuner_calls = 0;
        m_set_trunking_calls = 0;
        m_last_set_trunking = false;
        m_gain_calls = 0;
        m_airspy_calls = 0;
        m_last_gain_db = -1;
        m_last_squelch_db = 0.0;
        m_radio_accepted = true;
        m_squelch_calls = 0;
        m_last_squelch_margin_db = 0;
        m_squelch_auto_calls = 0;
        m_squelch_noise_calls = 0;
        m_restore_squelch_calls = 0;
        m_last_nfm_bandwidth_hz = -1;
        m_nfm_bandwidth_calls = 0;
        m_last_modulation = -1;
        m_last_decode_mode = -1;
        m_last_ppm = 9999;
        m_last_am_bandwidth_hz = -1;
        m_am_bandwidth_calls = 0;
        m_last_tone_filter_mode = -1;
        m_last_tone_filter_list.clear();
        m_tone_filter_calls = 0;
        m_scan_row_context = {{"active", false}};
        m_last_scan_row_edit.clear();
        m_scan_row_edit_calls = 0;
        m_lockout_accepted = true;
        m_lockout_requests.clear();
        m_relaxed_crc_accepted = true;
        m_relaxed_crc_requests.clear();
        m_recording_accepted = true;
        m_recording_refusal.clear();
        m_recording_requests.clear();
        m_avoid_clear_requests.clear();
        m_scan_hold_calls = 0;
        m_scan_avoid_calls = 0;
        m_scan_avoid_clear_calls = 0;
        m_restored_scan_avoids.clear();
        m_previous_channel_calls = 0;
        m_next_channel_calls = 0;
        m_resume_scan_calls = 0;
        m_tune_hz_calls = 0;
        m_last_tune_hz = 0;
        m_release_tuner_accepted = true;
        m_talkgroupEdit.clear();
        m_talkgroup_listen_calls = 0;
        m_last_talkgroup_id_start = 0.0;
        m_last_talkgroup_id_end = 0.0;
        m_last_talkgroup_listen = false;
        m_last_talkgroup_listen_token.clear();
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

    QStringList
    restoredScanAvoids() const {
        return m_restored_scan_avoids;
    }

    int
    previousChannelCalls() const {
        return m_previous_channel_calls;
    }

    int
    nextChannelCalls() const {
        return m_next_channel_calls;
    }

    int
    resumeScanCalls() const {
        return m_resume_scan_calls;
    }

    int
    tuneHzCalls() const {
        return m_tune_hz_calls;
    }

    double
    lastTuneHz() const {
        return static_cast<double>(m_last_tune_hz);
    }

    int
    gainCalls() const {
        return m_gain_calls;
    }

    int
    airspyCalls() const {
        return m_airspy_calls;
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
    lastSquelchMarginDb() const {
        return m_last_squelch_margin_db;
    }

    int
    squelchAutoCalls() const {
        return m_squelch_auto_calls;
    }

    int
    squelchNoiseCalls() const {
        return m_squelch_noise_calls;
    }

    int
    restoreSquelchCalls() const {
        return m_restore_squelch_calls;
    }

    int
    lastNfmBandwidthHz() const {
        return m_last_nfm_bandwidth_hz;
    }

    int
    nfmBandwidthCalls() const {
        return m_nfm_bandwidth_calls;
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
    lastAmBandwidthHz() const {
        return m_last_am_bandwidth_hz;
    }

    int
    amBandwidthCalls() const {
        return m_am_bandwidth_calls;
    }

    int
    lastToneFilterMode() const {
        return m_last_tone_filter_mode;
    }

    QString
    lastToneFilterList() const {
        return m_last_tone_filter_list;
    }

    int
    toneFilterCalls() const {
        return m_tone_filter_calls;
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
    lockoutSlotCalls() const {
        return m_lockout_slot_calls;
    }

    int
    lastLockoutSlot() const {
        return m_last_lockout_slot;
    }

    int
    skipSlotCalls() const {
        return m_skip_slot_calls;
    }

    int
    lastSkipSlot() const {
        return m_last_skip_slot;
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

    QVariantMap
    lastTalkgroupListenToken() const {
        return m_last_talkgroup_listen_token;
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
    int m_lockout_slot_calls = 0;
    int m_last_lockout_slot = -1;
    int m_skip_slot_calls = 0;
    int m_last_skip_slot = -1;
    int m_hold_calls = 0;
    double m_last_hold_tg = 0;
    int m_key_apply_calls = 0;
    int m_last_profile_scope = -1;
    QString m_last_profile_target;
    bool m_key_payload_valid = false;
    bool m_key_accepted = true;
    int m_last_force_mode = -1;
    int m_src_import_calls = 0;
    int m_src_clear_calls = 0;
    QString m_last_src_path;
    QStringList m_list_commands;
    bool m_list_commands_accepted = true;

    bool
    recordListCommand(const QString& method, const QString& path) {
        m_list_commands.append(path.isEmpty() ? method : method + QLatin1Char(' ') + path);
        return m_list_commands_accepted;
    }

    int m_manual_tune_calls = 0;
    unsigned int m_last_manual_tune_hz = 0U;
    int m_release_tuner_calls = 0;
    int m_set_trunking_calls = 0;
    bool m_last_set_trunking = false;
    int m_gain_calls = 0;
    int m_airspy_calls = 0;
    QVariantMap m_scan_row_context{{"active", false}};
    QVariantMap m_last_scan_row_edit;
    int m_scan_row_edit_calls = 0;
    bool m_lockout_accepted = true;
    QVariantList m_lockout_requests;
    bool m_relaxed_crc_accepted = true;
    QVariantList m_relaxed_crc_requests;
    bool m_recording_accepted = true;
    QString m_recording_refusal;
    QVariantList m_recording_requests;
    QStringList m_avoid_clear_requests;
    int m_scan_hold_calls = 0;
    int m_scan_avoid_calls = 0;
    int m_scan_avoid_clear_calls = 0;
    QStringList m_restored_scan_avoids;
    int m_previous_channel_calls = 0;
    int m_next_channel_calls = 0;
    int m_resume_scan_calls = 0;
    int m_tune_hz_calls = 0;
    unsigned int m_last_tune_hz = 0;
    bool m_release_tuner_accepted = true;
    int m_last_gain_db = -1;
    double m_last_squelch_db = 0.0;
    bool m_radio_accepted = true;
    int m_squelch_calls = 0;
    int m_last_squelch_margin_db = 0;
    int m_squelch_noise_calls = 0;
    int m_squelch_auto_calls = 0;
    int m_restore_squelch_calls = 0;
    int m_last_nfm_bandwidth_hz = -1;
    int m_nfm_bandwidth_calls = 0;
    int m_last_modulation = -1;
    int m_last_decode_mode = -1;
    int m_last_ppm = 9999;
    int m_last_am_bandwidth_hz = -1;
    int m_am_bandwidth_calls = 0;
    int m_last_tone_filter_mode = -1;
    QString m_last_tone_filter_list;
    int m_tone_filter_calls = 0;
    QVariantList m_talkgroupEdit;
    int m_talkgroup_listen_calls = 0;
    double m_last_talkgroup_id_start = 0.0;
    double m_last_talkgroup_id_end = 0.0;
    bool m_last_talkgroup_listen = false;
    QVariantMap m_last_talkgroup_listen_token;
    int m_all_talkgroups_listen_calls = 0;
    bool m_last_all_talkgroups_listen = false;
    QString m_last_all_talkgroups_tag;
};

/**
 * @brief Newest-first call log the tests drive directly.
 *
 * Same roles and the same newest-first prepend as CallHistoryModel, with
 * granular insert/reset signals — the screens' scroll behaviour is a reaction to
 * those signals, so a reset-everything stand-in would test nothing. Rows carry the
 * decode session they were logged in, as CallHistoryModel's do, and beginSession()
 * moves it on the way a start does.
 */
class CallLogStore : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString sessionLabel READ sessionLabel WRITE setSessionLabel NOTIFY sessionLabelChanged)
    Q_PROPERTY(QString sessionUid READ sessionUid WRITE setSessionUid NOTIFY sessionUidChanged)
    Q_PROPERTY(qlonglong session READ session NOTIFY sessionChanged)
    Q_PROPERTY(QStringList systemLabels READ systemLabels NOTIFY countChanged)
    Q_PROPERTY(bool saveFailing READ saveFailing NOTIFY saveFailingChanged)

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
        qint64 session = 0;
        qint64 freqHz = 0;
        QString accessCode;
        QString accessCodeText;
        /* The decoded service name, the received tone ("CTCSS 100.0 Hz") and the site line (issue #799), each empty
         * without. */
        QString serviceName;
        QString receivedTone;
        QString siteText;
        /* Issue #667: an analog reception, and its mode ("NFM", "AM"; empty when not known). */
        bool analog = false;
        QString analogMode;
        /* Issue #708: the stable row identity and the row's call recordings (the provider's maps), oldest first. */
        qint64 uid = 0;
        QVariantList recordings;
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

    qlonglong
    session() const {
        return m_session;
    }

    /** @brief A start: the rows pushed from here on are the new session's. */
    Q_INVOKABLE void
    beginSession() {
        m_session++;
        Q_EMIT sessionChanged();
    }

    /** @brief A start on a system (CallHistoryModel::beginSessionOn()). */
    Q_INVOKABLE void
    beginSessionOn(const QString& label, const QString& uid) {
        setSessionLabel(label);
        setSessionUid(uid);
        beginSession();
    }

    /** @brief The session's system, the session kept (CallHistoryModel::setSessionSystem()). */
    Q_INVOKABLE void
    setSessionSystem(const QString& label, const QString& uid) {
        setSessionLabel(label);
        setSessionUid(uid);
    }

    /** @brief The scan channels' names (CallHistoryModel::setChannelNames(), issue #671), kept for a case to read. */
    Q_INVOKABLE void
    setChannelNames(const QVariantMap& names) {
        m_channelNames = names;
    }

    Q_INVOKABLE QVariantMap
    channelNames() const {
        return m_channelNames;
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
            case CallHistoryModel::SessionRole: return row.session;
            case CallHistoryModel::FreqHzRole: return row.freqHz;
            case CallHistoryModel::AccessCodeRole: return row.accessCode;
            case CallHistoryModel::AccessCodeTextRole: return row.accessCodeText;
            case CallHistoryModel::ServiceNameRole: return row.serviceName;
            case CallHistoryModel::ReceivedToneRole: return row.receivedTone;
            case CallHistoryModel::SiteTextRole: return row.siteText;
            case CallHistoryModel::AnalogRole: return row.analog;
            case CallHistoryModel::AnalogModeRole: return row.analogMode;
            case CallHistoryModel::UidRole: return row.uid;
            case CallHistoryModel::RecordingsRole: return row.recordings;
            case CallHistoryModel::HasRecordingRole: return !row.recordings.isEmpty();
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
                {CallHistoryModel::ChannelRole, "channel"},
                {CallHistoryModel::SessionRole, "session"},
                {CallHistoryModel::FreqHzRole, "freqHz"},
                {CallHistoryModel::AccessCodeRole, "accessCode"},
                {CallHistoryModel::AccessCodeTextRole, "accessCodeText"},
                {CallHistoryModel::ServiceNameRole, "serviceName"},
                {CallHistoryModel::ReceivedToneRole, "receivedTone"},
                {CallHistoryModel::SiteTextRole, "siteText"},
                {CallHistoryModel::AnalogRole, "analog"},
                {CallHistoryModel::AnalogModeRole, "analogMode"},
                {CallHistoryModel::UidRole, "uid"},
                {CallHistoryModel::RecordingsRole, "recordings"},
                {CallHistoryModel::HasRecordingRole, "hasRecording"}};
    }

    /** @brief CallHistoryModel::rowForUid(): where the row with @p uid is now, or -1. */
    Q_INVOKABLE int
    rowForUid(qint64 uid) const {
        for (int i = 0; uid > 0 && i < m_rows.size(); i++) {
            if (m_rows.at(i).uid == uid) {
                return i;
            }
        }
        return -1;
    }

    /** @brief CallHistoryModel::recordingsForUid(). */
    Q_INVOKABLE QVariantList
    recordingsForUid(qint64 uid) const {
        const int row = rowForUid(uid);
        return row >= 0 ? m_rows.at(row).recordings : QVariantList();
    }

    /**
     * @brief CallHistoryModel::uidForFragment(): the uid of the row holding @p fragment now, or 0 when none does (never
     * held, or its row trimmed or cleared).
     */
    Q_INVOKABLE double
    uidForFragment(const QString& fragment) const {
        const qint64 uid = m_fragmentUids.value(fragment);
        return rowForUid(uid) >= 0 ? static_cast<double>(uid) : 0.0;
    }

    /**
     * @brief The row with @p uid holds @p fragment from now on, as a fragment logged, folded into that row or merged
     * into it from another does: dataChanged on that row. False when no row has @p uid.
     */
    Q_INVOKABLE bool
    setFragmentRow(const QString& fragment, qint64 uid) {
        const int row = rowForUid(uid);
        if (row < 0) {
            return false;
        }
        m_fragmentUids.insert(fragment, uid);
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx);
        return true;
    }

    /** @brief Trim the row with @p uid from the log, as CallHistoryModel::trimToCapacity() does: a row removal. */
    Q_INVOKABLE bool
    trimRow(qint64 uid) {
        const int row = rowForUid(uid);
        if (row < 0) {
            return false;
        }
        beginRemoveRows(QModelIndex(), row, row);
        m_rows.removeAt(row);
        endRemoveRows();
        Q_EMIT countChanged();
        Q_EMIT rowPositionsChanged();
        return true;
    }

    /** @brief CallHistoryModel::recordFor(): the detail sheet's keys for the row with @p uid now, or an empty map. */
    Q_INVOKABLE QVariantMap
    recordFor(const QString& uid) const {
        static const struct {
            const char* key;
            int role;
        } kRecordRoles[] = {
            {"uid", CallHistoryModel::UidRole},
            {"name", CallHistoryModel::NameRole},
            {"when", CallHistoryModel::WhenRole},
            {"systemName", CallHistoryModel::SystemNameRole},
            {"systemUid", CallHistoryModel::SystemUidRole},
            {"channel", CallHistoryModel::ChannelRole},
            {"tg", CallHistoryModel::TgRole},
            {"src", CallHistoryModel::SrcRole},
            {"sourceName", CallHistoryModel::SourceNameRole},
            {"emergency", CallHistoryModel::EmergencyRole},
            {"enc", CallHistoryModel::EncRole},
            {"durationSecs", CallHistoryModel::DurationSecsRole},
            {"detail", CallHistoryModel::DetailRole},
            {"freqHz", CallHistoryModel::FreqHzRole},
            {"accessCode", CallHistoryModel::AccessCodeRole},
            {"accessCodeText", CallHistoryModel::AccessCodeTextRole},
            {"serviceName", CallHistoryModel::ServiceNameRole},
            {"receivedTone", CallHistoryModel::ReceivedToneRole},
            {"siteText", CallHistoryModel::SiteTextRole},
            {"analog", CallHistoryModel::AnalogRole},
            {"analogMode", CallHistoryModel::AnalogModeRole},
        };

        bool parsed = false;
        const qint64 id = uid.toLongLong(&parsed);
        const int row = parsed ? rowForUid(id) : -1;
        QVariantMap record;
        if (row < 0) {
            return record;
        }
        const QModelIndex idx = index(row);
        for (const auto& entry : kRecordRoles) {
            record.insert(QLatin1String(entry.key), data(idx, entry.role));
        }
        return record;
    }

    /**
     * @brief Change the row with @p uid in place, as a fragment folded into it does: each of @p fields (by role name:
     * name, srcName, durationSecs, freqHz, serviceName, receivedTone, siteText, analogMode) is set, then one
     * dataChanged on that row names the roles set. False when no row has @p uid or a field is not one of those.
     */
    Q_INVOKABLE bool
    updateRow(qint64 uid, const QVariantMap& fields) {
        const int row = rowForUid(uid);
        if (row < 0) {
            return false;
        }
        StoreRow& stored = m_rows[row];
        const QHash<int, QByteArray> names = roleNames();
        QList<int> roles;
        for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
            const int role = names.key(it.key().toUtf8(), -1);
            if (!setRowField(stored, role, it.value())) {
                return false;
            }
            roles.append(role);
        }
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, roles);
        return true;
    }

    bool
    saveFailing() const {
        return m_saveFailing;
    }

    /** @brief Report History's saves failing, or working again (CallHistoryModel::saveFailing()). */
    Q_INVOKABLE void
    setSaveFailing(bool failing) {
        if (failing != m_saveFailing) {
            m_saveFailing = failing;
            Q_EMIT saveFailingChanged();
        }
    }

    /** @brief The newest row's uid, 0 with no rows. */
    Q_INVOKABLE qint64
    newestUid() const {
        return m_rows.isEmpty() ? 0 : m_rows.first().uid;
    }

    /**
     * @brief Give the row with @p uid the call recordings @p recordings, as the store's news does (a recording
     * journaled, a fragment folded into the row, a recording deleted): dataChanged on that row's two roles.
     */
    Q_INVOKABLE bool
    setRowRecordings(qint64 uid, const QVariantList& recordings) {
        const int row = rowForUid(uid);
        if (row < 0) {
            return false;
        }
        m_rows[row].recordings = recordings;
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, {CallHistoryModel::RecordingsRole, CallHistoryModel::HasRecordingRole});
        return true;
    }

    /** @brief Prepend one call with @p recordings; returns its uid. */
    Q_INVOKABLE qint64
    pushWithRecordings(const QVariantList& recordings) {
        push(QStringLiteral("TODAY"));
        setRowRecordings(m_rows.first().uid, recordings);
        return m_rows.first().uid;
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
        row.session = m_session;
        row.dayLabel = dayLabel.isEmpty() ? QStringLiteral("TODAY") : dayLabel;
        row.timeText = QStringLiteral("12:%1").arg(m_seq % 60, 2, 10, QLatin1Char('0'));
        row.uid = ++m_lastUid;
        beginInsertRows(QModelIndex(), 0, 0);
        m_rows.prepend(row);
        endInsertRows();
        Q_EMIT countChanged();
        Q_EMIT rowPositionsChanged();
        return row.name;
    }

    Q_INVOKABLE QString
    pushWithIds(qulonglong tg, qulonglong src) {
        const QString call = push(QStringLiteral("TODAY"));
        m_rows[0].tg = tg;
        m_rows[0].src = src;
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx, {CallHistoryModel::TgRole, CallHistoryModel::SrcRole});
        return call;
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
     * @brief Prepend one call with ids @p tg / @p src heard on @p channel, at @p freqHz under access code
     * @p accessCode (long form @p accessCodeText), as the model's rows carry them (issue #575).
     * @return The row's name, so a test can find that one row.
     */
    Q_INVOKABLE QString
    pushTuned(qulonglong tg, qulonglong src, const QString& channel, qint64 freqHz, const QString& accessCode,
              const QString& accessCodeText) {
        const QString name = push(QStringLiteral("TODAY"));
        tuneNewest(tg, src, channel, freqHz, accessCode, accessCodeText);
        return name;
    }

    /** @brief pushTuned() for an emergency call: the meta line's EMERGENCY and the row's badge. */
    Q_INVOKABLE QString
    pushTunedEmergency(qulonglong tg, qulonglong src, const QString& channel, qint64 freqHz) {
        const QString name = push(QStringLiteral("TODAY"));
        m_rows[0].emergency = true;
        tuneNewest(tg, src, channel, freqHz, QString(), QString());
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx, {CallHistoryModel::EmergencyRole});
        return name;
    }

    /**
     * @brief The same, as a data/control notice carrying @p detail: notice rows keep their own meta rules.
     * @return The row's name.
     */
    Q_INVOKABLE QString
    pushTunedNotice(qulonglong tg, qulonglong src, const QString& detail, const QString& channel, qint64 freqHz,
                    const QString& accessCode, const QString& accessCodeText) {
        const QString name = push(QStringLiteral("TODAY"));
        m_rows[0].kind = CallHistoryModel::KindNotice;
        m_rows[0].detail = detail;
        m_rows[0].durationSecs = -1;
        tuneNewest(tg, src, channel, freqHz, accessCode, accessCodeText);
        return name;
    }

    /**
     * @brief Prepend a conventional analog reception (issue #667) as the model logs one: no ids and no access code,
     * heard in @p analogMode ("NFM", "AM", or empty when not known) with @p receivedTone ("CTCSS 100.0 Hz", or
     * empty), named @p name, on @p channel at @p freqHz.
     * @return The row's name.
     */
    Q_INVOKABLE QString
    pushAnalog(const QString& name, const QString& analogMode, const QString& receivedTone, const QString& channel,
               qint64 freqHz) {
        push(QStringLiteral("TODAY"));
        StoreRow& row = m_rows[0];
        row.name = name;
        row.analog = true;
        row.analogMode = analogMode;
        row.receivedTone = receivedTone;
        tuneNewest(0, 0, channel, freqHz, QString(), QString());
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx,
                           {CallHistoryModel::NameRole, CallHistoryModel::AnalogRole, CallHistoryModel::AnalogModeRole,
                            CallHistoryModel::ReceivedToneRole});
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

    /**
     * @brief Prepend one clear voice call stamped @p when, as a replay's calls are
     * stamped with the capture's time.
     * @return The row's name.
     */
    Q_INVOKABLE QString
    pushAt(qint64 when) {
        const QString name = push(QStringLiteral("TODAY"));
        m_rows[0].when = when;
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx, {CallHistoryModel::WhenRole});
        return name;
    }

    /** @brief The newest row's `when`, 0 with no rows: the stamps are fixed, so a case ages rows against it. */
    Q_INVOKABLE qint64
    newestWhen() const {
        return m_rows.isEmpty() ? 0 : m_rows.first().when;
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
        // Removed, not reset, as CallHistoryModel::clearAll() does.
        if (!m_rows.isEmpty()) {
            beginRemoveRows(QModelIndex(), 0, static_cast<int>(m_rows.size()) - 1);
            m_rows.clear();
            endRemoveRows();
        }
        Q_EMIT countChanged();
        Q_EMIT rowPositionsChanged();
    }

  Q_SIGNALS:
    void countChanged();
    void sessionLabelChanged();
    void sessionUidChanged();
    void sessionChanged();
    void saveFailingChanged();
    void rowPositionsChanged();

  private:
    /* updateRow()'s fields; false for a role it does not set. */
    static bool
    setRowField(StoreRow& row, int role, const QVariant& value) {
        switch (role) {
            case CallHistoryModel::NameRole: row.name = value.toString(); return true;
            case CallHistoryModel::SourceNameRole: row.sourceName = value.toString(); return true;
            case CallHistoryModel::DurationSecsRole: row.durationSecs = value.toInt(); return true;
            case CallHistoryModel::FreqHzRole: row.freqHz = value.toLongLong(); return true;
            case CallHistoryModel::ServiceNameRole: row.serviceName = value.toString(); return true;
            case CallHistoryModel::ReceivedToneRole: row.receivedTone = value.toString(); return true;
            case CallHistoryModel::SiteTextRole: row.siteText = value.toString(); return true;
            case CallHistoryModel::AnalogModeRole: row.analogMode = value.toString(); return true;
            default: return false;
        }
    }

    /* Stamps the newest row with its ids, channel, frequency and access code, and announces every role it set. */
    void
    tuneNewest(qulonglong tg, qulonglong src, const QString& channel, qint64 freqHz, const QString& accessCode,
               const QString& accessCodeText) {
        StoreRow& row = m_rows[0];
        row.tg = tg;
        row.src = src;
        row.channel = channel;
        row.freqHz = freqHz;
        row.accessCode = accessCode;
        row.accessCodeText = accessCodeText;
        const QModelIndex idx = index(0);
        Q_EMIT dataChanged(idx, idx,
                           {CallHistoryModel::KindRole, CallHistoryModel::DetailRole,
                            CallHistoryModel::DurationSecsRole, CallHistoryModel::TgRole, CallHistoryModel::SrcRole,
                            CallHistoryModel::ChannelRole, CallHistoryModel::FreqHzRole,
                            CallHistoryModel::AccessCodeRole, CallHistoryModel::AccessCodeTextRole});
    }

    QList<StoreRow> m_rows;
    /* uidForFragment()'s index; an entry whose row is gone resolves to none, as the model's trim and clear make it. */
    QHash<QString, qint64> m_fragmentUids;
    QString m_systemName = QStringLiteral("Test Site");
    QVariantMap m_channelNames;
    QString m_sessionUid = QStringLiteral("test-system");
    QString m_sessionLabel = QStringLiteral("Test Site");
    qint64 m_session = 0;
    int m_seq = 0;
    /* Never reused, as the model's are not. */
    qint64 m_lastUid = 0;
    bool m_saveFailing = false;
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

    Q_INVOKABLE QString
    licenseNotices() const {
        return QStringLiteral("License notices fixture");
    }

    /* Issue #830: what each copy asked to put on the clipboard, oldest first (resetHostPlayback() forgets them); the
     * readings have no clipboard, so every copy answers false. */
    Q_INVOKABLE bool
    copyText(const QString& text) const {
        copiedTexts.append(text);
        return false;
    }

    /* Main's failure card tells the host it was dismissed (#782); the readings have no attempt to give up. */
    Q_INVOKABLE void
    dismissFailure() {}

    /* Issue #671: Main hands the metrics the scan channels' names; kept as the "channelNames" reading for a case. */
    Q_INVOKABLE void
    setChannelNames(const QVariantMap& names) {
        insert(QStringLiteral("channelNames"), names);
    }

    /* Issue #708: the call recording player, export and share, as the decoderHost map plays them. A play opens at
     * once (Preparing), pause and resume flip the state, a seek moves the position and a stop clears the path, as a
     * platform host publishes them; a case scripts the rest (Playing, the end, a failure) with setHostReading(). Every
     * call is kept, oldest first, as {name, ...}. */
    Q_INVOKABLE void
    playRecording(const QString& path) {
        noteCall(QStringLiteral("playRecording"), {{QStringLiteral("path"), path}});
        publishPlayback(path, 1, 0, 0, QString());
    }

    Q_INVOKABLE void
    pausePlayback() {
        noteCall(QStringLiteral("pausePlayback"), {});
        publishPlayback(value(QStringLiteral("playbackPath")).toString(), 3,
                        value(QStringLiteral("playbackPositionMs")).toLongLong(),
                        value(QStringLiteral("playbackDurationMs")).toLongLong(), QString());
    }

    Q_INVOKABLE void
    resumePlayback() {
        noteCall(QStringLiteral("resumePlayback"), {});
        publishPlayback(value(QStringLiteral("playbackPath")).toString(), 2,
                        value(QStringLiteral("playbackPositionMs")).toLongLong(),
                        value(QStringLiteral("playbackDurationMs")).toLongLong(), QString());
    }

    Q_INVOKABLE void
    seekPlayback(qint64 positionMs) {
        noteCall(QStringLiteral("seekPlayback"), {{QStringLiteral("ms"), positionMs}});
        if (!seekApplied) {
            return;
        }
        insert(QStringLiteral("playbackPositionMs"), positionMs);
        Q_EMIT playbackPositionChanged();
    }

    Q_INVOKABLE void
    stopPlayback() {
        noteCall(QStringLiteral("stopPlayback"), {});
        publishPlayback(QString(), 0, 0, 0, QString());
    }

    Q_INVOKABLE bool
    exportRecordings(const QVariantMap& selection, const QString& reference) {
        return beginTransfer(QStringLiteral("exportRecordings"), selection, reference);
    }

    Q_INVOKABLE bool
    saveRecording(const QVariantMap& selection, const QString& reference) {
        return beginTransfer(QStringLiteral("saveRecording"), selection, reference);
    }

    Q_INVOKABLE void
    cancelRecordingTransfer() {
        noteCall(QStringLiteral("cancelRecordingTransfer"), {});
    }

    Q_INVOKABLE bool
    shareRecording(const QString& path, const QString& title) {
        noteCall(QStringLiteral("shareRecording"), {{QStringLiteral("path"), path}, {QStringLiteral("title"), title}});
        return shareAccepted;
    }

    /* Listen's Nearest order: a location request only records itself, {name, id}; a case answers it with the map's
     * own locationResult() or locationCancelled(), as a platform host would later. */
    Q_INVOKABLE void
    requestSiteLocation(qint64 requestId) {
        noteCall(QStringLiteral("requestSiteLocation"), {{QStringLiteral("id"), requestId}});
    }

    Q_INVOKABLE void
    cancelLocationRequest(qint64 requestId) {
        noteCall(QStringLiteral("cancelLocationRequest"), {{QStringLiteral("id"), requestId}});
    }

    /* Issue #829: the document picker. A request only records itself and returns a new id; a case answers it through
     * testContext (answerDocumentPicked() and the like), as a platform host would later. Calls are kept in `calls`
     * with the rest; the `documentPickerSupported` reading is in the map. */
    Q_INVOKABLE qint64
    requestOpenDocument() {
        lastDocumentRequest =
            ImportOnlyHost::documentRequest(++m_documentRequestId, QStringLiteral("open"), QString(), QString());
        noteCall(QStringLiteral("requestOpenDocument"), lastDocumentRequest);
        return m_documentRequestId;
    }

    Q_INVOKABLE qint64
    requestSaveDocument(const QString& suggestedName, const QString& mimeType) {
        lastDocumentRequest =
            ImportOnlyHost::documentRequest(++m_documentRequestId, QStringLiteral("save"), suggestedName, mimeType);
        noteCall(QStringLiteral("requestSaveDocument"), lastDocumentRequest);
        return m_documentRequestId;
    }

    Q_INVOKABLE void
    cancelDocumentRequest(qint64 requestId) {
        noteCall(QStringLiteral("cancelDocumentRequest"), {{QStringLiteral("id"), requestId}});
    }

    Q_INVOKABLE QString
    savedDocumentName(const QString& reference) const {
        return savedDocumentNames.value(reference);
    }

    Q_INVOKABLE void
    discardPendingDownload(const QString& reference) {
        noteCall(QStringLiteral("discardPendingDownload"), {{QStringLiteral("reference"), reference}});
    }

    /* Issue #831: the incoming document slot and the picker-in-flight reading, as DecoderHost publishes them; a case
     * drives both through testContext (emitIncomingDocument(), setDocumentRequestPending()). */
    Q_INVOKABLE QString
    takeIncomingDocument() {
        if (m_incomingDocument.isEmpty()) {
            return QString();
        }
        QString reference;
        reference.swap(m_incomingDocument);
        insert(QStringLiteral("incomingDocumentPending"), false);
        Q_EMIT incomingDocumentChanged();
        return reference;
    }

    void
    offerIncomingDocument(const QString& reference) {
        if (reference.isEmpty()) {
            return;
        }
        m_incomingDocument = reference;
        insert(QStringLiteral("incomingDocumentPending"), true);
        Q_EMIT incomingDocumentChanged();
    }

    void
    setDocumentRequestPending(bool pending) {
        if (value(QStringLiteral("documentRequestPending")).toBool() != pending) {
            insert(QStringLiteral("documentRequestPending"), pending);
            Q_EMIT documentRequestChanged();
        }
    }

    void
    setSessionStateKnown(bool known) {
        if (value(QStringLiteral("sessionStateKnown")).toBool() != known) {
            insert(QStringLiteral("sessionStateKnown"), known);
            Q_EMIT sessionStateKnownChanged();
        }
    }

    /* Issue #830: the file browser. Each call is kept in `calls` as ImportOnlyHost::fileCall() shapes it; a request
     * returns a new id from the picker's sequence and a case answers it through testContext. The
     * `fileBrowserSupported` and `fileAccess` readings are in the map. */
    Q_INVOKABLE void
    refreshFileAccess() {
        noteFileCall(QStringLiteral("refreshFileAccess"), {});
    }

    Q_INVOKABLE qint64
    openFileBrowserSession() {
        const qint64 id = ++m_documentRequestId;
        noteFileCall(QStringLiteral("openFileBrowserSession"),
                     {{QStringLiteral("id"), id}, {QStringLiteral("session"), id}});
        return id;
    }

    Q_INVOKABLE void
    closeFileBrowserSession(qint64 sessionId) {
        noteFileCall(QStringLiteral("closeFileBrowserSession"), {{QStringLiteral("session"), sessionId}});
    }

    Q_INVOKABLE qint64
    requestFileRoots(qint64 sessionId) {
        return noteFileRequest(QStringLiteral("requestFileRoots"), {{QStringLiteral("session"), sessionId}});
    }

    Q_INVOKABLE qint64
    requestFolderListing(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& path,
                         const QVariantMap& filter) {
        return noteFileRequest(QStringLiteral("requestFolderListing"),
                               ImportOnlyHost::folderListingArgs(sessionId, rootKey, generation, path, filter));
    }

    Q_INVOKABLE qint64
    requestMorePage(qint64 sessionId, qint64 snapshot, int offset) {
        return noteFileRequest(QStringLiteral("requestMorePage"),
                               ImportOnlyHost::morePageArgs(sessionId, snapshot, offset));
    }

    Q_INVOKABLE qint64
    requestFileHandoff(qint64 sessionId, const QString& rootKey, qint64 generation, const QString& reference) {
        return noteFileRequest(QStringLiteral("requestFileHandoff"),
                               ImportOnlyHost::fileHandoffArgs(sessionId, rootKey, generation, reference));
    }

    Q_INVOKABLE qint64
    requestFileAccess(qint64 sessionId) {
        return noteFileRequest(QStringLiteral("requestFileAccess"), {{QStringLiteral("session"), sessionId}});
    }

    Q_INVOKABLE void
    cancelFileRequest(qint64 requestId) {
        noteFileCall(QStringLiteral("cancelFileRequest"), {{QStringLiteral("id"), requestId}});
    }

    void
    publishPlayback(const QString& path, int state, qint64 positionMs, qint64 durationMs, const QString& error) {
        insert(QStringLiteral("playbackPath"), path);
        insert(QStringLiteral("playbackState"), state);
        insert(QStringLiteral("playbackPositionMs"), positionMs);
        insert(QStringLiteral("playbackDurationMs"), durationMs);
        insert(QStringLiteral("playbackError"), error);
        Q_EMIT playbackChanged();
        Q_EMIT playbackPositionChanged();
    }

    QVariantList calls;
    mutable QStringList copiedTexts;
    QVariantMap lastDocumentRequest;
    QMap<QString, QString> savedDocumentNames;
    bool shareAccepted = true;
    /* Issue #782: false for a host that ignores a seek (a platform player that clamps it or drops it). */
    bool seekApplied = true;

  Q_SIGNALS:
    void playbackChanged();
    void playbackPositionChanged();
    void recordingTransferChanged();
    void recordingTransferFinished(int selection, int result, int missing);
    void recordingFilesMissing(const QStringList& paths);
    void recordingShareFailed(const QString& path);
    void tunerChanged();
    void controlChanged();
    void siteChanged();
    void qualityChanged();
    void runningChanged();
    void sessionStateChanged();
    void sessionStateKnownChanged();
    void sessionInitialized();
    void localDeviceChanged();
    void typographyChanged();
    void keyboardChanged();
    void backRequested();
    void runResultChanged();
    void locationResult(qint64 requestId, bool fixOk, double lat, double lon, double accuracyM, qint64 fixAtMs,
                        bool geocodeOk, const QString& postal, const QString& country, const QString& error);
    void locationCancelled(qint64 requestId);
    void documentPicked(qint64 requestId, const QString& reference, const QString& displayName, bool inDownloads);
    void documentPickCancelled(qint64 requestId);
    void documentPickUnavailable(qint64 requestId, const QString& reason);
    void documentRequestChanged();
    void incomingDocumentChanged();
    void fileAccessChanged();
    void fileRootsListed(qint64 requestId, const QVariantMap& result);
    void folderListed(qint64 requestId, const QVariantMap& result);
    void fileHandoffChecked(qint64 requestId, const QVariantMap& result);
    void fileAccessRequestDone(qint64 requestId, const QString& outcome);

  private:
    qint64 m_documentRequestId = 0;
    QString m_incomingDocument;

    void
    noteFileCall(const QString& name, const QVariantMap& args) {
        noteCall(name, ImportOnlyHost::fileCall(name, args));
    }

    qint64
    noteFileRequest(const QString& name, QVariantMap args) {
        args.insert(QStringLiteral("id"), ++m_documentRequestId);
        noteFileCall(name, args);
        return m_documentRequestId;
    }

    void
    noteCall(const QString& name, QVariantMap args) {
        args.insert(QStringLiteral("name"), name);
        calls.append(args);
    }

    bool
    beginTransfer(const QString& name, const QVariantMap& selection, const QString& reference) {
        noteCall(name, {{QStringLiteral("selection"), selection}, {QStringLiteral("reference"), reference}});
        insert(QStringLiteral("recordingTransferActive"), true);
        insert(QStringLiteral("recordingTransferBytes"), 0);
        insert(QStringLiteral("recordingTransferTotalBytes"), selection.value(QStringLiteral("bytes")));
        Q_EMIT recordingTransferChanged();
        return true;
    }
};

/**
 * @brief Issue #708: the `recordings` store the screens read, as a map a case writes (`recordings.lowSpace = true`)
 * plus its calls, kept oldest first as {name, ...}.
 *
 * The selections hand back what a case set with setSelection(): its count, size and paths, under a new selection
 * number each time (0 when the count is 0, as the store answers a selection that matched nothing).
 *
 * Issue #809: a detection's recordings are what a case seeded with setDetectionRecordings(), each map completed with
 * the store's keys; every seeding, and a reset that drops seeded ones, moves `revision` up and emits
 * revisionChanged(), as the store does. That signal is
 * declared here: a property map's own change signals are not reachable by name, so `onRevisionChanged` would miss.
 */
class RecordingsFake : public QQmlPropertyMap {
    Q_OBJECT
  public:
    explicit RecordingsFake(QObject* parent) : QQmlPropertyMap(this, parent) { reset(); }

    /** @brief Every key at rest: an empty, indexed store on available storage with the default limit. */
    static QVariantMap
    rest() {
        return {{QStringLiteral("ready"), true},        {QStringLiteral("indexed"), true},
                {QStringLiteral("available"), true},    {QStringLiteral("usedBytes"), 0},
                {QStringLiteral("recordingCount"), 0},  {QStringLiteral("unfinishedCount"), 0},
                {QStringLiteral("unfinishedBytes"), 0}, {QStringLiteral("folders"), QVariantList()},
                {QStringLiteral("freeBytes"), -1},      {QStringLiteral("lowSpace"), false},
                {QStringLiteral("removedByLimit"), 0},  {QStringLiteral("deletesWaiting"), 0},
                {QStringLiteral("deleteFailures"), 0},  {QStringLiteral("lastDeleteError"), QString()},
                {QStringLiteral("limitMb"), 1000},      {QStringLiteral("revision"), 0.0}};
    }

    /** @brief A recordingsForDetection() map with every key the store gives, each at a blank value. */
    static QVariantMap
    detectionEntry() {
        return {{QStringLiteral("id"), QString()},
                {QStringLiteral("path"), QString()},
                {QStringLiteral("fileName"), QString()},
                {QStringLiteral("folderUid"), QString()},
                {QStringLiteral("start"), qint64{0}},
                {QStringLiteral("durationMs"), qint64{0}},
                {QStringLiteral("rate"), 0},
                {QStringLiteral("bytes"), qint64{0}},
                {QStringLiteral("failed"), false},
                {QStringLiteral("available"), true},
                {QStringLiteral("system"), QString()},
                {QStringLiteral("target"), QString()},
                {QStringLiteral("source"), QString()},
                {QStringLiteral("fragment"), QString()}};
    }

    Q_INVOKABLE void
    reset() {
        /* The revision only rises, as the store's does: a reset that drops seeded recordings moves it on. */
        const double revision = value(QStringLiteral("revision")).toDouble();
        const QVariantMap values = rest();
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            insert(it.key(), it.key() == QLatin1String("revision") ? QVariant(revision) : it.value());
        }
        m_calls.clear();
        if (!m_detections.isEmpty()) {
            m_detections.clear();
            bumpRevision();
        }
        m_selectionCount = 0;
        m_selectionBytes = 0;
        m_selectionPaths.clear();
        Q_EMIT changed();
    }

    Q_INVOKABLE void
    deleteRecordings(const QStringList& ids) {
        note(QStringLiteral("deleteRecordings"), {{QStringLiteral("ids"), ids}});
    }

    Q_INVOKABLE void
    deleteRecording(const QString& id) {
        note(QStringLiteral("deleteRecording"), {{QStringLiteral("id"), id}});
    }

    Q_INVOKABLE void
    deleteFolder(const QString& uid) {
        note(QStringLiteral("deleteFolder"), {{QStringLiteral("uid"), uid}});
    }

    Q_INVOKABLE void
    deleteUnfinished() {
        note(QStringLiteral("deleteUnfinished"), {});
    }

    Q_INVOKABLE void
    deleteAll() {
        note(QStringLiteral("deleteAll"), {});
    }

    Q_INVOKABLE QVariantMap
    selectForExport(const QString& folderUid, const QString& preset) {
        note(QStringLiteral("selectForExport"),
             {{QStringLiteral("folderUid"), folderUid}, {QStringLiteral("preset"), preset}});
        return selection();
    }

    Q_INVOKABLE QVariantMap
    selectRecordings(const QStringList& ids) {
        note(QStringLiteral("selectRecordings"), {{QStringLiteral("ids"), ids}});
        return selection();
    }

    Q_INVOKABLE void
    releaseSelection(int selection) {
        note(QStringLiteral("releaseSelection"), {{QStringLiteral("selection"), selection}});
    }

    Q_INVOKABLE void
    recheckRecordings(const QStringList& ids) {
        note(QStringLiteral("recheckRecordings"), {{QStringLiteral("ids"), ids}});
    }

    /** @brief RecordingStore::acknowledgeNotices(): the counts the notices show start again. */
    Q_INVOKABLE void
    acknowledgeNotices() {
        note(QStringLiteral("acknowledgeNotices"), {});
        insert(QStringLiteral("removedByLimit"), 0);
        insert(QStringLiteral("deleteFailures"), 0);
        insert(QStringLiteral("lastDeleteError"), QString());
    }

    /** @brief What the next selections resolve to. */
    Q_INVOKABLE void
    setSelection(int count, qint64 bytes, const QStringList& paths) {
        m_selectionCount = count;
        m_selectionBytes = bytes;
        m_selectionPaths = paths;
    }

    Q_INVOKABLE QVariantList
    calls() const {
        return m_calls;
    }

    /** @brief RecordingStore::recordingCountForDetection(): how many recordings the detection @p key has. */
    Q_INVOKABLE int
    recordingCountForDetection(const QString& key) const {
        return static_cast<int>(m_detections.value(key).size());
    }

    /** @brief RecordingStore::recordingsForDetection(): the detection @p key's recordings, as seeded (newest first). */
    Q_INVOKABLE QVariantList
    recordingsForDetection(const QString& key) const {
        return m_detections.value(key);
    }

    /**
     * @brief The detection @p key's recordings from now on, newest first: each of @p recordings' maps over
     * detectionEntry(), so a case names only the keys it reads. An empty list leaves the detection none. Moves
     * `revision`, as a journal change does.
     */
    Q_INVOKABLE void
    setDetectionRecordings(const QString& key, const QVariantList& recordings) {
        QVariantList entries;
        for (const QVariant& recording : recordings) {
            QVariantMap entry = detectionEntry();
            const QVariantMap given = recording.toMap();
            for (auto it = given.cbegin(); it != given.cend(); ++it) {
                entry.insert(it.key(), it.value());
            }
            entries.append(entry);
        }
        if (entries.isEmpty()) {
            m_detections.remove(key);
        } else {
            m_detections.insert(key, entries);
        }
        bumpRevision();
    }

    Q_INVOKABLE void
    emitLimitReached(int count) {
        Q_EMIT limitReached(count);
    }

  Q_SIGNALS:
    void changed();
    void limitMbChanged();
    void limitReached(int count);
    void revisionChanged();

  private:
    void
    note(const QString& name, QVariantMap args) {
        args.insert(QStringLiteral("name"), name);
        m_calls.append(args);
    }

    void
    bumpRevision() {
        insert(QStringLiteral("revision"), value(QStringLiteral("revision")).toDouble() + 1.0);
        Q_EMIT revisionChanged();
    }

    QVariantMap
    selection() {
        if (m_selectionCount <= 0) {
            return {{QStringLiteral("selection"), 0},
                    {QStringLiteral("paths"), QStringList()},
                    {QStringLiteral("names"), QStringList()},
                    {QStringLiteral("count"), 0},
                    {QStringLiteral("bytes"), 0}};
        }
        return {{QStringLiteral("selection"), ++m_lastSelection},
                {QStringLiteral("paths"), m_selectionPaths},
                {QStringLiteral("names"), m_selectionPaths},
                {QStringLiteral("count"), m_selectionCount},
                {QStringLiteral("bytes"), m_selectionBytes}};
    }

    QVariantList m_calls;
    QHash<QString, QVariantList> m_detections;
    int m_selectionCount = 0;
    qint64 m_selectionBytes = 0;
    QStringList m_selectionPaths;
    int m_lastSelection = 0;
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

    /* The sign-in form's two model calls. The readings (hasPassword, credentialsReady, accountState) stay the case's
     * to set, as every other reading here is, and only the checks are counted. A case that needs the model to answer
     * uses FakeRadioReference.qml. */
    Q_INVOKABLE void
    setPassword(const QString& password) {
        (void)password;
    }

    Q_INVOKABLE void
    checkAccount() {
        accountChecks++;
    }

    int nearbyCalls = 0;
    int accountChecks = 0;
  Q_SIGNALS:
    void locationRequestAllocated(qint64 requestId);
};

/**
 * @brief The `signInAutofill` context object: records what the sign-in forms ask of the relay.
 *
 * Tokens grow from 1 and begin() retires the live one, as SignInAutofill's do. Every call is logged as a map with its
 * `op` ("begin", "end", "track", "note"), its `token`, and for track the `field`, the `input` item and `visible`, for
 * note the `field` and `text`. A case fires a fill with fill(); like the production relay, the values are the case's
 * to apply first (prefs, the model), and the signal carries only which fields were filled.
 */
class SignInAutofillRecorder : public QObject {
    Q_OBJECT

  public:
    explicit SignInAutofillRecorder(QObject* parent) : QObject(parent) {}

    Q_INVOKABLE int
    begin() {
        m_live = ++m_last;
        log({{QStringLiteral("op"), QStringLiteral("begin")}, {QStringLiteral("token"), m_live}});
        return m_live;
    }

    Q_INVOKABLE void
    end(int token) {
        log({{QStringLiteral("op"), QStringLiteral("end")}, {QStringLiteral("token"), token}});
        if (token == m_live) {
            m_live = 0;
        }
    }

    Q_INVOKABLE void
    track(int token, int field, QObject* input, bool visible) {
        log({{QStringLiteral("op"), QStringLiteral("track")},
             {QStringLiteral("token"), token},
             {QStringLiteral("field"), field},
             {QStringLiteral("input"), QVariant::fromValue(input)},
             {QStringLiteral("visible"), visible}});
    }

    Q_INVOKABLE void
    note(int token, int field, const QString& text) {
        log({{QStringLiteral("op"), QStringLiteral("note")},
             {QStringLiteral("token"), token},
             {QStringLiteral("field"), field},
             {QStringLiteral("text"), text}});
    }

    /** @brief Every call since the last clear(), oldest first. */
    Q_INVOKABLE QVariantList
    calls() const {
        return m_calls;
    }

    Q_INVOKABLE void
    clear() {
        m_calls.clear();
    }

    /** @brief The live token (0 with none). */
    Q_INVOKABLE int
    liveToken() const {
        return m_live;
    }

    /** @brief A fill for @p token, as the relay announces one it applied. */
    Q_INVOKABLE void
    fill(int token, bool hasUsername, bool hasPassword) {
        Q_EMIT filled(token, hasUsername, hasPassword);
    }

  Q_SIGNALS:
    void filled(int token, bool hasUsername, bool hasPassword);

  private:
    void
    log(const QVariantMap& call) {
        m_calls.append(call);
    }

    QVariantList m_calls;
    int m_last = 0;
    int m_live = 0;
};

/**
 * @brief Issue #782: counts the accessibility events the screens raise, for the tests in tests/ui/qml_a11y.
 *
 * On Android every ObjectShow and ObjectHide costs the main thread a runnable that makes eight blocking calls into the
 * Qt thread, so a screen that raises them on every metrics tick floods the platform bridge. The counter is a
 * QAccessible update handler, which Qt calls for every event whether or not accessibility is active; it records the
 * event and hands it on exactly as Qt would have, to the previous handler or else to the platform.
 *
 * Each event is counted by its type; by the objectName of the nearest named object, the emitter included (`owners`);
 * by the named chain up from it, for failure messages (`chains`); and under every object on its parent chain, so a
 * case can ask for the events raised anywhere inside a sheet, named or not (countWithin()). The walk reads only C++
 * state (objectName, parentItem, parent): the handler runs inside object construction, where reading a QML property
 * can run deferred creation and recurse.
 *
 * The per-object counts are keyed by address. A destroyed delegate's address can be reused by a later object, so ask
 * them only for objects that outlive the count, such as a sheet.
 *
 * The handler is a plain function pointer, so the counts live in one process-wide record.
 */
class AccessibilityCounter {
  public:
    static void
    start() {
        Record& r = record();
        if (!r.installed) {
            r.previous = QAccessible::installUpdateHandler(&AccessibilityCounter::handle);
            r.installed = true;
        }
        r.total = 0;
        r.types.clear();
        r.owners.clear();
        r.scopes.clear();
        r.chains.clear();
        r.sequence.clear();
    }

    /** @brief Put the handler that was installed before start() back. The counts stay readable. */
    static void
    stop() {
        Record& r = record();
        if (!r.installed) {
            return;
        }
        QAccessible::installUpdateHandler(r.previous);
        r.previous = nullptr;
        r.installed = false;
    }

    /**
     * @brief The counts since start(): {total, types: {Type: n}, owners: {name: {Type: n}},
     * chains: {"Type name < named ancestor < ...": n}, sequence: ["Type name < ...", ...]}: the sequence is the
     * events in the order raised (the first kMaxSequence), for a case that asserts on an order. Emitters with no
     * named object on their chain count under "<unnamed>".
     */
    static QVariantMap
    counts() {
        const Record& r = record();
        QVariantMap out;
        out.insert(QStringLiteral("total"), r.total);
        out.insert(QStringLiteral("types"), toMap(r.types));
        out.insert(QStringLiteral("owners"), nested(r.owners));
        out.insert(QStringLiteral("chains"), toMap(r.chains));
        out.insert(QStringLiteral("sequence"), r.sequence);
        return out;
    }

    /** @brief How many events of @p type (an enum key such as "ObjectHide") @p scope or anything under it raised. */
    static qint64
    countWithin(const QObject* scope, const QString& type) {
        const Record& r = record();
        const auto it = r.scopes.constFind(scope);
        return it == r.scopes.cend() ? 0 : it->value(type);
    }

  private:
    struct Record {
        bool installed = false;
        QAccessible::UpdateHandler previous = nullptr;
        qint64 total = 0;
        QHash<QString, qint64> types;
        QHash<QString, QHash<QString, qint64>> owners;
        QHash<const QObject*, QHash<QString, qint64>> scopes;
        QHash<QString, qint64> chains;
        QStringList sequence;
    };

    /* Distinct chains kept for failure messages; the totals above are never capped. */
    static constexpr int kMaxChains = 2000;
    /* Events kept in order; the totals above are never capped. */
    static constexpr int kMaxSequence = 2000;
    /* Far deeper than any screen nests; it only bounds the walk. */
    static constexpr int kMaxDepth = 256;

    static Record&
    record() {
        static Record r;
        return r;
    }

    static QVariantMap
    toMap(const QHash<QString, qint64>& counts) {
        QVariantMap out;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            out.insert(it.key(), it.value());
        }
        return out;
    }

    static QVariantMap
    nested(const QHash<QString, QHash<QString, qint64>>& counts) {
        QVariantMap out;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            out.insert(it.key(), toMap(it.value()));
        }
        return out;
    }

    static QString
    typeName(QAccessible::Event type) {
        const char* key = QMetaEnum::fromType<QAccessible::Event>().valueToKey(static_cast<int>(type));
        return key != nullptr ? QString::fromLatin1(key) : QStringLiteral("0x%1").arg(static_cast<uint>(type), 0, 16);
    }

    /* The visual parent where there is one, else the object parent: a popup's content item has no parent item while
     * closed, and its object parent leads back to the control that owns it. */
    static QObject*
    up(QObject* object) {
        if (auto* item = qobject_cast<QQuickItem*>(object)) {
            if (item->parentItem() != nullptr) {
                return item->parentItem();
            }
        }
        return object->parent();
    }

    static void
    handle(QAccessibleEvent* event) {
        Record& r = record();
        const QString type = typeName(event->type());
        ++r.total;
        ++r.types[type];
        QStringList names;
        int depth = 0;
        for (QObject* object = event->object(); object != nullptr && depth < kMaxDepth; object = up(object)) {
            const QString name = object->objectName();
            if (!name.isEmpty() && !names.contains(name)) {
                names.append(name);
            }
            ++r.scopes[object][type];
            ++depth;
        }
        ++r.owners[names.isEmpty() ? QStringLiteral("<unnamed>") : names.constFirst()][type];
        const QString chain = type + QLatin1Char(' ')
                              + (names.isEmpty() ? QStringLiteral("<unnamed>") : names.join(QStringLiteral(" < ")));
        if (r.chains.contains(chain) || r.chains.size() < kMaxChains) {
            ++r.chains[chain];
        }
        if (r.sequence.size() < kMaxSequence) {
            r.sequence.append(chain);
        }
        forward(event);
    }

    /* Qt's own path: with no handler installed, updateAccessibility() gives the event to the platform. Reaching the
     * platform that way replays the whole call, including the part that ran before this handler: for
     * TableModelChanged that part has already applied the change to the table interface, and a replay would apply it
     * a second time. That event is not replayed, so while counting it does not reach the platform; the offscreen
     * platform these tests run on has no bridge to tell. While a replay runs the counter is not installed, so an
     * event raised synchronously from inside it goes to the platform uncounted; with no bridge, none is. */
    static void
    forward(QAccessibleEvent* event) {
        Record& r = record();
        if (r.previous != nullptr) {
            r.previous(event);
            return;
        }
        if (event->type() == QAccessible::TableModelChanged) {
            return;
        }
        QAccessible::installUpdateHandler(nullptr);
        QAccessible::updateAccessibility(event);
        QAccessible::installUpdateHandler(&AccessibilityCounter::handle);
    }
};

/** @brief Installs the context the screens expect before any QML is loaded. */
class Setup : public QObject {
    Q_OBJECT

  public:
    /* QUICK_TEST_MAIN_WITH_SETUP builds this object before the application and every engine, which
     * is when the collector policy has to be in place. */
    Setup() { dsd_qt::applyQmlGcPolicy(); }

    ~Setup() override {
        dsd_state_ext_free_all(m_talkgroup_state.get());
        for (auto& table : m_target_tables) {
            if (table) {
                dsd_state_ext_free_all(table.get());
            }
        }
    }

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
        m_applied_table = nullptr;
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
        return true;
    }

    Q_INVOKABLE bool
    pushPatternTalkgroup(const QString& pattern, const QString& mode, const QString& name, const QString& tags) {
        dsd_tg_policy_entry entry{};
        if (dsd_tg_policy_make_exact_entry(0, mode.toUtf8().constData(), name.toUtf8().constData(),
                                           DSD_TG_POLICY_SOURCE_IMPORTED, &entry)
            != 0) {
            return false;
        }
        entry.kind = DSD_TG_POLICY_ROW_PATTERN;
        DSD_SNPRINTF(entry.pattern, sizeof(entry.pattern), "%s", pattern.toUtf8().constData());
        DSD_SNPRINTF(entry.tags, sizeof(entry.tags), "%s", tags.toUtf8().constData());
        if (dsd_tg_policy_add_pattern_entry(m_talkgroup_state.get(), &entry) != 0) {
            return false;
        }
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
        return true;
    }

    /**
     * @brief Issue #782: rename the fixture table's row for @p id in place, as an edit does: the rows keep their
     * bounds, so the list changes that row's data and is not reset.
     */
    Q_INVOKABLE bool
    renameFixtureTalkgroup(double id, const QString& mode, const QString& name, const QString& tags) {
        dsd_tg_policy_entry entry{};
        if (dsd_tg_policy_make_exact_entry(static_cast<uint32_t>(id), mode.toUtf8().constData(),
                                           name.toUtf8().constData(), DSD_TG_POLICY_SOURCE_IMPORTED, &entry)
            != 0) {
            return false;
        }
        DSD_SNPRINTF(entry.tags, sizeof(entry.tags), "%s", tags.toUtf8().constData());
        if (dsd_tg_policy_upsert_exact(m_talkgroup_state.get(), &entry, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) != 0) {
            return false;
        }
        m_applied_table = nullptr;
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
        return true;
    }

    /**
     * @brief Issue #782: show scan target @p target's talkgroup table, as the poll tick does after a hop.
     *
     * Targets 0 and 1 are two prepared policy tables, A and B, each with its own policy context and with different
     * rows and categories, so a switch between them resets the list and changes the categories. Any other value
     * goes back to the fixture's own table, the one pushTalkgroup() fills.
     */
    Q_INVOKABLE bool
    applyTalkgroupTable(int target) {
        dsd_state* table = m_talkgroup_state.get();
        if (target >= 0 && target < static_cast<int>(m_target_tables.size())) {
            table = targetTable(static_cast<size_t>(target));
            if (table == nullptr) {
                return false;
            }
        }
        m_applied_table = table == m_talkgroup_state.get() ? nullptr : table;
        m_talkgroups->refresh(m_talkgroup_opts.get(), table);
        return true;
    }

    /**
     * @brief Issue #782: whether the shown table takes an edit captured at (@p context, @p generation).
     *
     * The same check the decoder makes before any talkgroup edit (tg_edit_check_version()); false is the stale
     * refusal.
     */
    Q_INVOKABLE bool
    talkgroupEditVersionValid(const QString& context, unsigned int generation) const {
        bool ok = false;
        const qulonglong id = context.toULongLong(&ok);
        const dsd_state* table = m_applied_table != nullptr ? m_applied_table : m_talkgroup_state.get();
        return ok && dsd_tg_policy_edit_version_valid(table, id, generation) != 0;
    }

    Q_INVOKABLE void
    setGroupFileConfigured(bool configured) {
        DSD_SNPRINTF(m_talkgroup_opts->group_in_file, sizeof(m_talkgroup_opts->group_in_file), "%s",
                     configured ? "fixture-talkgroups.csv" : "");
        m_applied_table = nullptr;
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
    }

    Q_INVOKABLE bool
    clearTalkgroups() {
        if (dsd_tg_policy_clear(m_talkgroup_state.get()) != 0) {
            return false;
        }
        m_applied_table = nullptr;
        m_talkgroups->refresh(m_talkgroup_opts.get(), m_talkgroup_state.get());
        return true;
    }

    Q_INVOKABLE int
    lockoutSlotCalls() const {
        return m_commands->lockoutSlotCalls();
    }

    Q_INVOKABLE int
    lastLockoutSlot() const {
        return m_commands->lastLockoutSlot();
    }

    Q_INVOKABLE int
    skipSlotCalls() const {
        return m_commands->skipSlotCalls();
    }

    Q_INVOKABLE int
    lastSkipSlot() const {
        return m_commands->lastSkipSlot();
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

    /** The row token the last Listening toggle submitted: context, generation, tableIndex, pattern. */
    Q_INVOKABLE QVariantMap
    lastTalkgroupListenToken() const {
        return m_commands != nullptr ? m_commands->lastTalkgroupListenToken() : QVariantMap();
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
     * @brief Change several metrics readings and send one controlChanged, as one decoder snapshot reaches the real
     * model (issue #671).
     *
     * What listens to the signal (Main's operator-hold sync) sees the owner and the scan change together. Bindings on
     * the readings themselves still re-evaluate key by key as the property map takes each one, so a screen-level case
     * whose binding must never see a state between two keys still orders its setters.
     */
    Q_INVOKABLE void
    setMetrics(const QVariantMap& values) {
        bool tuner = false;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            m_metrics[it.key()] = it.value();
            if (m_engine != nullptr) {
                m_metric_readings->insert(it.key(), it.value());
            }
            tuner = tuner || it.key() == QStringLiteral("centerFreqHz");
        }
        if (m_engine != nullptr) {
            if (tuner) {
                Q_EMIT m_metric_readings->tunerChanged();
            }
            Q_EMIT m_metric_readings->controlChanged();
        }
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
            Q_EMIT m_metric_readings->controlChanged();
        }
    }

    /**
     * @brief Issue #782: a metrics tick that changes nothing, @p times over.
     *
     * Re-setting the `metrics` context property to the same object signals it every time, so every `metrics.*`
     * binding re-evaluates with the values it already had: what each poll tick does to a binding whose notify signal
     * is shared with a reading that moved. setMetric() cannot stand in, as a map signals nothing for an equal value.
     */
    Q_INVOKABLE void
    tickMetrics(int times) {
        if (m_engine == nullptr) {
            return;
        }
        for (int i = 0; i < times; ++i) {
            m_engine->rootContext()->setContextProperty(QStringLiteral("metrics"), m_metric_readings);
        }
    }

    /**
     * @brief Issue #782: a trunk-scan hop to a target whose control channel lists the neighbour sites @p sites, the
     * network published to p25Network as UiController publishes it on a hop (P25NetworkModel::replaceForTarget). Site
     * @c n is on 851 MHz plus @c n channels of 12.5 kHz; the list is in the order given, the first heard last.
     */
    Q_INVOKABLE void
    hopNetwork(const QVariantList& sites) {
        if (m_network == nullptr) {
            return;
        }
        if (!m_network_state) {
            m_network_state = std::make_unique<dsd_state>();
        }
        dsd_state* state = m_network_state.get();
        DSD_MEMSET(state->p25_nb_entries, 0, sizeof(state->p25_nb_entries));
        const int count = static_cast<int>(std::min<qsizetype>(sites.size(), P25_NB_MAX));
        for (int i = 0; i < count; ++i) {
            const int site = sites.at(i).toInt();
            p25_nb_entry_t& entry = state->p25_nb_entries[i];
            entry.freq = 851000000L + site * 12500L;
            entry.sysid = 0x123;
            entry.rfss = 1;
            entry.site = static_cast<uint8_t>(site);
            entry.last_seen = static_cast<time_t>(count - i);
        }
        state->p25_nb_count = count;
        m_network->replaceForTarget(state);
    }

    /**
     * @brief Issue #782: publish a scan target's readings in one go, as MetricsModel does on a target change.
     *
     * Every key in @p readings is written in one batch and the reading groups are signalled once afterwards, with no
     * defaults in between: one publication per target change.
     */
    Q_INVOKABLE void
    publishTarget(const QVariantMap& readings) {
        if (m_engine == nullptr) {
            return;
        }
        QVariantHash batch;
        for (auto it = readings.cbegin(); it != readings.cend(); ++it) {
            m_metrics[it.key()] = it.value();
            batch.insert(it.key(), it.value());
        }
        m_metric_readings->insert(batch);
        Q_EMIT m_metric_readings->tunerChanged();
        Q_EMIT m_metric_readings->controlChanged();
        Q_EMIT m_metric_readings->siteChanged();
        Q_EMIT m_metric_readings->qualityChanged();
    }

    /** @brief Issue #782: every ancestor's interface lists the one below it, the walk the Android bridge makes. */
    static bool
    accessibleInTree(QAccessibleInterface* iface) {
        QAccessibleInterface* child = iface;
        for (QAccessibleInterface* parent = child->parent(); parent != nullptr; parent = parent->parent()) {
            if (parent->indexOfChild(child) < 0) {
                return false;
            }
            child = parent;
        }
        return true;
    }

    /** @brief Issue #782: the lower-case words (letters and digits) of @p text, markup and entities taken out. */
    static QStringList
    readableWords(QString text, bool markup) {
        if (markup) {
            static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
            text.replace(tag, QStringLiteral(" "));
            text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
            text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
            text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
            text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        }
        static const QRegularExpression word(QStringLiteral("[\\p{L}\\p{N}]+"));
        QStringList words;
        for (auto it = word.globalMatch(text); it.hasNext();) {
            words.append(it.next().captured(0).toLower());
        }
        return words;
    }

    static bool
    containsAllWords(const QStringList& haystack, const QStringList& needles) {
        for (const QString& needle : needles) {
            if (!haystack.contains(needle)) {
                return false;
            }
        }
        return true;
    }

    /** @brief Issue #782: the words a platform bridge reads for @p iface: its name, description and value. */
    static QStringList
    spokenWords(QAccessibleInterface* iface) {
        return readableWords(iface->text(QAccessible::Name) + QLatin1Char(' ') + iface->text(QAccessible::Description)
                                 + QLatin1Char(' ') + iface->text(QAccessible::Value),
                             false);
    }

    /** @brief Issue #782: whether some in-tree accessible node from @p item up carries all of @p words. */
    static bool
    wordsReachable(QQuickItem* item, const QStringList& words) {
        for (QQuickItem* at = item; at != nullptr; at = at->parentItem()) {
            QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(at);
            if (iface == nullptr || !iface->isValid() || !accessibleInTree(iface)) {
                continue;
            }
            if (containsAllWords(spokenWords(iface), words)) {
                return true;
            }
        }
        return false;
    }

    static void
    collectUnreachedTexts(QQuickItem* item, const QList<const QObject*>& skip, QVariantList& out) {
        if (!item->isVisible() || qFuzzyIsNull(item->opacity()) || skip.contains(item)) {
            return;
        }
        if (item->inherits("QQuickText")) {
            const QString text = item->property("text").toString();
            // Text.PlainText is 0; every other format may carry markup.
            const bool markup = item->property("textFormat").toInt() != 0 && text.contains(QLatin1Char('<'));
            const QStringList words = readableWords(text, markup);
            if (!words.isEmpty() && !wordsReachable(item, words)) {
                const QQmlContext* context = qmlContext(item);
                out.append(QVariantMap{
                    {QStringLiteral("item"), QVariant::fromValue(static_cast<QObject*>(item))},
                    {QStringLiteral("text"), text},
                    {QStringLiteral("file"), context != nullptr ? context->baseUrl().fileName() : QString()},
                    {QStringLiteral("id"), context != nullptr ? context->nameForObject(item) : QString()},
                    {QStringLiteral("objectName"), item->objectName()},
                });
            }
        }
        const QList<QQuickItem*> children = item->childItems();
        for (QQuickItem* child : children) {
            collectUnreachedTexts(child, skip, out);
        }
    }

    /** @brief Issue #782: whether accessibility is active (DSD_QML_TEST_ACCESSIBILITY=1 turns it on at startup). */
    Q_INVOKABLE bool
    accessibilityActive() const {
        return QAccessible::isActive();
    }

    /**
     * @brief Issue #782: how the accessibility tree presents @p item, as a platform bridge reads it.
     *
     * `exists` is whether the item has an accessible interface. `inTree` is whether it can be reached from the top:
     * every ancestor's interface lists the one below it among its children, which is how the Android bridge builds
     * its node tree (childIdListForAccessibleObject). `role` is the QAccessible::Role value; `name`, `description`
     * and `value` are the interface's texts; `invisible`, `checkable`, `checked` and `focusable` are its state flags.
     * `currentValue` is the value interface's reading (a progress bar's, which the Android bridge reads after the
     * name), or undefined when it has none. `actions` are its action names: the Android bridge makes a node
     * clickable only with a press or toggle action, and a static text or heading never (#831).
     */
    Q_INVOKABLE QVariantMap
    accessibleNode(QObject* item) const {
        QAccessibleInterface* iface = item != nullptr ? QAccessible::queryAccessibleInterface(item) : nullptr;
        if (iface == nullptr || !iface->isValid()) {
            return {{QStringLiteral("exists"), false}, {QStringLiteral("inTree"), false}};
        }
        QAccessibleValueInterface* value = iface->valueInterface();
        return {{QStringLiteral("exists"), true},
                {QStringLiteral("inTree"), accessibleInTree(iface)},
                {QStringLiteral("role"), static_cast<int>(iface->role())},
                {QStringLiteral("name"), iface->text(QAccessible::Name)},
                {QStringLiteral("description"), iface->text(QAccessible::Description)},
                {QStringLiteral("value"), iface->text(QAccessible::Value)},
                {QStringLiteral("invisible"), static_cast<bool>(iface->state().invisible)},
                {QStringLiteral("checkable"), static_cast<bool>(iface->state().checkable)},
                {QStringLiteral("checked"), static_cast<bool>(iface->state().checked)},
                {QStringLiteral("focusable"), static_cast<bool>(iface->state().focusable)},
                {QStringLiteral("childCount"), iface->childCount()},
                {QStringLiteral("currentValue"), value != nullptr ? value->currentValue() : QVariant()},
                {QStringLiteral("actions"),
                 iface->actionInterface() != nullptr ? iface->actionInterface()->actionNames() : QStringList()}};
    }

    /** @brief #831: perform @p action (an action name such as "Toggle") on @p item; false when it does not offer it. */
    Q_INVOKABLE bool
    accessibleDoAction(QObject* item, const QString& action) const {
        QAccessibleInterface* iface = item != nullptr ? QAccessible::queryAccessibleInterface(item) : nullptr;
        QAccessibleActionInterface* actions = iface != nullptr && iface->isValid() ? iface->actionInterface() : nullptr;
        if (actions == nullptr || !actions->actionNames().contains(action)) {
            return false;
        }
        actions->doAction(action);
        return true;
    }

    /**
     * @brief #831: a screen reader's double tap on @p item, as the Android bridge performs it (clickAction_helper):
     * the press action, else the toggle action. False when the item offers neither, as the bridge reports.
     */
    Q_INVOKABLE bool
    accessibleClick(QObject* item) const {
        QAccessibleInterface* iface = item != nullptr ? QAccessible::queryAccessibleInterface(item) : nullptr;
        QAccessibleActionInterface* actions = iface != nullptr && iface->isValid() ? iface->actionInterface() : nullptr;
        if (actions == nullptr) {
            return false;
        }
        const QStringList names = actions->actionNames();
        if (names.contains(QAccessibleActionInterface::pressAction())) {
            actions->doAction(QAccessibleActionInterface::pressAction());
            return true;
        }
        if (names.contains(QAccessibleActionInterface::toggleAction())) {
            actions->doAction(QAccessibleActionInterface::toggleAction());
            return true;
        }
        return false;
    }

    /**
     * @brief Issue #782: the node a screen reader's touch lands on at (@p x, @p y) in @p item: from the window's
     * interface down through childAt(), the walk explore-by-touch makes. Gives the node's object, role, name and
     * childCount, or `exists` false when nothing is there.
     */
    Q_INVOKABLE QVariantMap
    accessibleNodeAt(QObject* item, qreal x, qreal y) const {
        auto* quick = qobject_cast<QQuickItem*>(item);
        if (quick == nullptr || quick->window() == nullptr) {
            return {{QStringLiteral("exists"), false}};
        }
        const QPoint global = quick->window()->mapToGlobal(quick->mapToScene(QPointF(x, y))).toPoint();
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(quick->window());
        while (iface != nullptr) {
            QAccessibleInterface* child = iface->childAt(global.x(), global.y());
            if (child == nullptr) {
                break;
            }
            iface = child;
        }
        if (iface == nullptr || !iface->isValid()) {
            return {{QStringLiteral("exists"), false}};
        }
        return {{QStringLiteral("exists"), true},
                {QStringLiteral("object"), QVariant::fromValue(iface->object())},
                {QStringLiteral("role"), static_cast<int>(iface->role())},
                {QStringLiteral("name"), iface->text(QAccessible::Name)},
                {QStringLiteral("childCount"), iface->childCount()}};
    }

    /**
     * @brief Issue #782: the visible text under @p root that no screen reader can reach.
     *
     * A plain `Text` has no accessible interface, so the Android bridge makes no node for it; its words reach a
     * screen reader only through an accessible node on its own ancestor chain: itself named, or a button, row or
     * group whose name, description or value carries them. This walks every visible, unfaded `Text` (a Controls
     * `Label` included) under @p root and reports each one whose words are not all among the words of a single such
     * node that is in the tree (accessibleNode's `inTree`). A text with no letters or digits (a glyph, a separator)
     * has nothing to read and is skipped. The subtrees of @p skipped (sheets checked on their own) are left out. Each
     * entry gives the item, its text, the QML file that declares it, its id there and its objectName, for an allowlist
     * key and a failure message.
     */
    Q_INVOKABLE QVariantList
    unreachedTexts(QObject* root, const QVariantList& skipped = {}) const {
        QVariantList out;
        QList<const QObject*> skip;
        for (const QVariant& entry : skipped) {
            skip.append(entry.value<QObject*>());
        }
        if (auto* item = qobject_cast<QQuickItem*>(root)) {
            collectUnreachedTexts(item, skip, out);
        }
        return out;
    }

    /**
     * @brief Issue #782: every item under @p root that has an accessible interface, in or out of the tree, as
     * {item, objectName, file, role, name, inTree, focusable, invisible}: what a platform bridge can still read of a
     * node it learned of earlier, if nothing told it the node left the tree.
     */
    Q_INVOKABLE QVariantList
    accessibleNodesUnder(QObject* root) const {
        QVariantList out;
        if (auto* item = qobject_cast<QQuickItem*>(root)) {
            collectAccessibleNodes(item, out);
        }
        return out;
    }

    static void
    collectAccessibleNodes(QQuickItem* item, QVariantList& out) {
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(item);
        if (iface != nullptr && iface->isValid() && iface->object() == item) {
            const QQmlContext* context = qmlContext(item);
            out.append(
                QVariantMap{{QStringLiteral("item"), QVariant::fromValue(static_cast<QObject*>(item))},
                            {QStringLiteral("objectName"), item->objectName()},
                            {QStringLiteral("file"), context != nullptr ? context->baseUrl().fileName() : QString()},
                            {QStringLiteral("role"), static_cast<int>(iface->role())},
                            {QStringLiteral("name"), iface->text(QAccessible::Name)},
                            {QStringLiteral("inTree"), accessibleInTree(iface)},
                            {QStringLiteral("focusable"), static_cast<bool>(iface->state().focusable)},
                            {QStringLiteral("invisible"), static_cast<bool>(iface->state().invisible)}});
        }
        const QList<QQuickItem*> children = item->childItems();
        for (QQuickItem* child : children) {
            collectAccessibleNodes(child, out);
        }
    }

    /** @brief Issue #782: the role of @p item's accessible parent (QAccessible::Role), -1 when it has none. */
    Q_INVOKABLE int
    accessibleParentRole(QObject* item) const {
        QAccessibleInterface* iface = item != nullptr ? QAccessible::queryAccessibleInterface(item) : nullptr;
        QAccessibleInterface* parent = iface != nullptr && iface->isValid() ? iface->parent() : nullptr;
        return parent != nullptr && parent->isValid() ? static_cast<int>(parent->role()) : -1;
    }

    /** @brief Issue #782: retune the canned spectrum frames to @p hz (0 puts back the default centre). */
    Q_INVOKABLE void
    setSpectrumCenterHz(double hz) const {
        dsd_neo_qml_stub::set_spectrum_center_hz(hz > 0 ? static_cast<unsigned int>(hz)
                                                        : dsd_neo_qml_stub::kSpectrumCenterHz);
    }

    /**
     * @brief Issue #782: each focusable heading node in the tree under @p root, as {item, name}: what TalkBack's
     * heading navigation and swipes stop on.
     */
    Q_INVOKABLE QVariantList
    focusableHeadings(QObject* root) const {
        QVariantList out;
        if (auto* item = qobject_cast<QQuickItem*>(root)) {
            collectFocusableHeadings(item, out);
        }
        return out;
    }

    static void
    collectFocusableHeadings(QQuickItem* item, QVariantList& out) {
        if (!item->isVisible()) {
            return;
        }
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(item);
        if (iface != nullptr && iface->isValid() && iface->object() == item && iface->role() == QAccessible::Heading
            && accessibleInTree(iface) && iface->state().focusable) {
            out.append(QVariantMap{{QStringLiteral("item"), QVariant::fromValue(static_cast<QObject*>(item))},
                                   {QStringLiteral("name"), iface->text(QAccessible::Name)}});
        }
        const QList<QQuickItem*> children = item->childItems();
        for (QQuickItem* child : children) {
            collectFocusableHeadings(child, out);
        }
    }

    /**
     * @brief Issue #782: the headings under @p root that TalkBack cannot move to: each visible item whose accessible
     * node is in the tree with the Heading role and is not focusable. Qt reports a Heading node focusable only when the
     * item takes Tab focus or sets `Accessible.focusable` (QAccessibleQuickItem::state(): text roles are focusable,
     * Heading is not one), the Android bridge passes that on (setFocusable), and TalkBack's linear navigation skips a
     * text node that is neither focusable nor clickable. Disabled items are left out. Each entry gives the item, its name
     * and the declaring file.
     */
    Q_INVOKABLE QVariantList
    unfocusableHeadings(QObject* root) const {
        QVariantList out;
        if (auto* item = qobject_cast<QQuickItem*>(root)) {
            collectUnfocusableHeadings(item, out);
        }
        return out;
    }

    static void
    collectUnfocusableHeadings(QQuickItem* item, QVariantList& out) {
        if (!item->isVisible()) {
            return;
        }
        QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(item);
        // A disabled item is never focusable (QAccessibleQuickItem::state()); the screens disable themselves under an
        // open sheet, where the navigation gate takes them out of the tree on a phone.
        if (iface != nullptr && iface->isValid() && iface->object() == item && item->isEnabled()
            && iface->role() == QAccessible::Heading && accessibleInTree(iface) && !iface->state().focusable) {
            const QQmlContext* context = qmlContext(item);
            out.append(QVariantMap{
                {QStringLiteral("item"), QVariant::fromValue(static_cast<QObject*>(item))},
                {QStringLiteral("name"), iface->text(QAccessible::Name)},
                {QStringLiteral("file"), context != nullptr ? context->baseUrl().fileName() : QString()},
            });
        }
        const QList<QQuickItem*> children = item->childItems();
        for (QQuickItem* child : children) {
            collectUnfocusableHeadings(child, out);
        }
    }

    /**
     * @brief Issue #782: whether @p carrier is a node in the tree whose name, description or value holds every word
     * of @p text: the check behind an allowlist entry that says another item reads a text aloud.
     */
    Q_INVOKABLE bool
    nodeCarries(QObject* carrier, const QString& text) const {
        QAccessibleInterface* iface = carrier != nullptr ? QAccessible::queryAccessibleInterface(carrier) : nullptr;
        if (iface == nullptr || !iface->isValid() || !accessibleInTree(iface)) {
            return false;
        }
        return containsAllWords(spokenWords(iface), readableWords(text, false));
    }

    /**
     * @brief Issue #782: press @p item as a screen reader does: TalkBack's click on a node runs the interface's press
     * action. Returns false, without acting, when the node offers no press action (Android then makes it unclickable,
     * except for a Link).
     */
    Q_INVOKABLE bool
    pressAccessible(QObject* item) const {
        QAccessibleInterface* iface = item != nullptr ? QAccessible::queryAccessibleInterface(item) : nullptr;
        QAccessibleActionInterface* actions = iface != nullptr && iface->isValid() ? iface->actionInterface() : nullptr;
        if (actions == nullptr || !actions->actionNames().contains(QAccessibleActionInterface::pressAction())) {
            return false;
        }
        actions->doAction(QAccessibleActionInterface::pressAction());
        return true;
    }

    /**
     * @brief Issue #782: the window of @p item loses (@p in false) or regains the focus, as the platform tells Qt Quick
     * when a system dialog takes the focus and gives it back (QQuickWindow::focusOutEvent/focusInEvent).
     */
    Q_INVOKABLE void
    sendWindowFocus(QObject* item, bool in) const {
        auto* quick = qobject_cast<QQuickItem*>(item);
        if (quick == nullptr || quick->window() == nullptr) {
            return;
        }
        QFocusEvent event(in ? QEvent::FocusIn : QEvent::FocusOut, Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(quick->window(), &event);
    }

    /**
     * @brief A key event, pressed (@p press) or released, for the active focus item of @p item's window, as the
     * platform sends one: @p autoRepeat marks a press the key repeats while held (QKeyEvent::isAutoRepeat()), which
     * QtTest's keyPress() cannot send. On Android it is also what reaches the app when the first press went elsewhere
     * (TalkBack took Alt+Enter). Returns whether the window accepted it.
     */
    Q_INVOKABLE bool
    sendKey(QObject* item, int key, int modifiers, bool press, bool autoRepeat) const {
        auto* quick = qobject_cast<QQuickItem*>(item);
        if (quick == nullptr || quick->window() == nullptr) {
            return false;
        }
        QString text;
        if (key == Qt::Key_Space) {
            text = QStringLiteral(" ");
        } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            text = QStringLiteral("\r");
        }
        QKeyEvent event(press ? QEvent::KeyPress : QEvent::KeyRelease, key, Qt::KeyboardModifiers(modifiers), text,
                        autoRepeat);
        QCoreApplication::sendEvent(quick->window(), &event);
        return event.isAccepted();
    }

    /**
     * @brief Issue #782: focus @p item as TalkBack does: Qt's Android bridge carries out ACTION_ACCESSIBILITY_FOCUS with
     * the node's set-focus action (androidjniaccessibility.cpp focusAction_helper). Returns false when the node has
     * none.
     */
    Q_INVOKABLE bool
    focusAccessible(QObject* item) const {
        QAccessibleInterface* iface = item != nullptr ? QAccessible::queryAccessibleInterface(item) : nullptr;
        QAccessibleActionInterface* actions = iface != nullptr && iface->isValid() ? iface->actionInterface() : nullptr;
        if (actions == nullptr || !actions->actionNames().contains(QAccessibleActionInterface::setFocusAction())) {
            return false;
        }
        actions->doAction(QAccessibleActionInterface::setFocusAction());
        return true;
    }

    /**
     * @brief Issue #782: from now on, http and https URLs that QML opens (Qt.openUrlExternally) are recorded here
     * rather than handed to a browser; openedUrls() lists them. releaseOpenedUrls() hands them back.
     */
    Q_INVOKABLE void
    captureOpenedUrls() {
        m_opened_urls.clear();
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "recordOpenedUrl");
        QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "recordOpenedUrl");
    }

    Q_INVOKABLE QStringList
    openedUrls() const {
        return m_opened_urls;
    }

    Q_INVOKABLE void
    releaseOpenedUrls() {
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
    }

    /** @brief Issue #782: the URL handler captureOpenedUrls() installs. */
    Q_INVOKABLE void
    recordOpenedUrl(const QUrl& url) {
        m_opened_urls.append(url.toString());
    }

    /** @brief Issue #782: count accessibility events from now on, forgetting earlier counts. */
    Q_INVOKABLE void
    startAccessibilityCount() {
        AccessibilityCounter::start();
    }

    /** @brief Issue #782: the counts since startAccessibilityCount(); see AccessibilityCounter::counts(). */
    Q_INVOKABLE QVariantMap
    accessibilityCount() const {
        return AccessibilityCounter::counts();
    }

    /** @brief Issue #782: how many events of @p type @p scope or anything under it raised since the count started. */
    Q_INVOKABLE double
    accessibilityCountWithin(QObject* scope, const QString& type) const {
        return static_cast<double>(AccessibilityCounter::countWithin(scope, type));
    }

    /** @brief Issue #782: stop counting and put the earlier update handler back. */
    Q_INVOKABLE void
    stopAccessibilityCount() {
        AccessibilityCounter::stop();
    }

    /** @brief Set one recordingStatus reading (issue #708), as setMetric() does for metrics. */
    Q_INVOKABLE void
    setRecordingStatus(const QString& key, const QVariant& value) {
        if (m_recording_readings != nullptr) {
            m_recording_readings->insert(key, value);
        }
    }

    /** @brief Every recordingStatus reading back at rest (issue #708). */
    Q_INVOKABLE void
    resetRecordingStatus() {
        for (auto it = m_recording_status.cbegin(); it != m_recording_status.cend(); ++it) {
            setRecordingStatus(it.key(), it.value());
        }
    }

    /**
     * @brief Set one decoderHost map reading (issue #708: the player's and the transfer's), announcing it as the
     * host's change signals do.
     */
    Q_INVOKABLE void
    setHostReading(const QString& key, const QVariant& value) {
        m_host[key] = value;
        m_host_readings->insert(key, value);
        if (key.startsWith(QStringLiteral("playback"))) {
            Q_EMIT m_host_readings->playbackChanged();
            Q_EMIT m_host_readings->playbackPositionChanged();
        } else if (key.startsWith(QStringLiteral("recordingTransfer"))) {
            Q_EMIT m_host_readings->recordingTransferChanged();
        }
    }

    /** @brief The player, export and share calls the screens made on the decoderHost map, oldest first. */
    Q_INVOKABLE QVariantList
    hostCalls() const {
        return m_host_readings->calls;
    }

    /**
     * @brief Forget them, and put the player and the transfer back at rest (nothing open, nothing running). The
     * document picker's records go too, on every host: no lastDocumentRequest() from before the reset and no name set
     * through setSavedDocumentName(); and so do the file browser's calls (fileCalls()) and the references the import
     * copies were asked for (importReferences()). So do an incoming document not taken, a picker request set in
     * flight, a session state set unknown and a held recording transfer (#831), so none reaches a later case.
     */
    Q_INVOKABLE void
    resetHostPlayback() {
        m_host_readings->calls.clear();
        m_host_readings->copiedTexts.clear();
        m_host_readings->lastDocumentRequest.clear();
        m_host_readings->savedDocumentNames.clear();
        (void)m_host_readings->takeIncomingDocument();
        m_host_readings->setDocumentRequestPending(false);
        m_host_readings->setSessionStateKnown(true);
        for (ImportOnlyHost* host : {m_import_host, m_initializing_host}) {
            host->lastDocumentRequest.clear();
            host->documentCalls.clear();
            host->savedDocumentNames.clear();
            (void)host->takeIncomingDocument();
            host->setDocumentRequestPending(false);
            host->setSessionStateKnown(true);
            host->holdTransfers = false;
            host->endHeldTransfer();
            host->fileCalls.clear();
            const std::lock_guard<std::mutex> guard(host->importGate->lock);
            host->importGate->references.clear();
        }
        m_host_readings->shareAccepted = true;
        m_host_readings->seekApplied = true;
        m_host_readings->publishPlayback(QString(), 0, 0, 0, QString());
        setHostReading(QStringLiteral("recordingTransferActive"), false);
        setHostReading(QStringLiteral("recordingTransferBytes"), 0);
        setHostReading(QStringLiteral("recordingTransferTotalBytes"), 0);
    }

    /** @brief Publish the player's whole state at once, as a platform host does (DecoderHost::publishPlayback()). */
    Q_INVOKABLE void
    publishPlayback(const QString& path, int state, qint64 positionMs, qint64 durationMs, const QString& error) {
        m_host_readings->publishPlayback(path, state, positionMs, durationMs, error);
    }

    /** @brief What the decoderHost map's shareRecording() answers. */
    Q_INVOKABLE void
    setShareAccepted(bool accepted) {
        m_host_readings->shareAccepted = accepted;
    }

    /** @brief Issue #782: whether the decoderHost map's seekPlayback() moves the position (else it only records it). */
    Q_INVOKABLE void
    setSeekApplied(bool applied) {
        m_host_readings->seekApplied = applied;
    }

    /** @brief End the running export or save of @p selection with @p result (DecoderHost::RecordingTransferResult). */
    Q_INVOKABLE void
    finishRecordingTransfer(int selection, int result, int missing) {
        setHostReading(QStringLiteral("recordingTransferActive"), false);
        Q_EMIT m_host_readings->recordingTransferFinished(selection, result, missing);
    }

    /** @brief A share the host took could not be handed over after all (DecoderHost::recordingShareFailed()). */
    Q_INVOKABLE void
    failRecordingShare(const QString& path) {
        Q_EMIT m_host_readings->recordingShareFailed(path);
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

    /** @brief How many times QML submitted an account check to the recorder (the model would dedupe; this does not). */
    Q_INVOKABLE int
    radioReferenceAccountChecks() const {
        return m_radio_reference_recorder != nullptr ? m_radio_reference_recorder->accountChecks : 0;
    }

    /** Use an offline QML model for complete account/import navigation flows. */
    Q_INVOKABLE void
    useRadioReferenceModel(QObject* model) {
        m_engine->rootContext()->setContextProperty(QStringLiteral("radioReference"),
                                                    model != nullptr ? model : m_radio_reference_recorder);
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

    /** @brief Issue #580: every call the screens made on `backup`, oldest first, as `{name, args}` maps. */
    Q_INVOKABLE QVariantList
    backupCalls() const {
        return m_backup != nullptr ? m_backup->calls() : QVariantList();
    }

    /** @brief Back to a fresh `backup`: idle, restore allowed, nothing pending, no outcome, no calls. */
    Q_INVOKABLE void
    resetBackup() {
        if (m_backup != nullptr) {
            m_backup->reset();
        }
    }

    /**
     * @brief Set one `backup` property (busy, flowActive, restorePending, restoreCancellable, startupOutcome,
     * lastResult) or the files its checkExport() refuses over (unreadableFiles); restorePending sets restoreCancellable
     * alike (see BackupRecorder::setState()).
     */
    Q_INVOKABLE bool
    setBackup(const QString& key, const QVariant& value) {
        return m_backup != nullptr && m_backup->setState(key, value);
    }

    /**
     * @brief Issue #828: post a store notice for @p fileName (kept as @p keptName, "" while not moved) from a thread of
     *        its own, as a call history worker's save does; `storeNotices` takes it once the event loop runs.
     */
    Q_INVOKABLE void
    postStoreNotice(const QString& fileName, const QString& keptName) {
        std::thread poster(
            [fileName, keptName] { dsd_qt::json_store_post_notice({fileName, QStringLiteral("test"), keptName}); });
        poster.join();
    }

    /** @brief Issue #828: the Signal search store's files as SignalSearchModel::storeFiles() reports them. */
    Q_INVOKABLE void
    setSignalSearchStoreFiles(const QVariantList& files) {
        dsd_qt::StoreNotices::instance()->setSignalSearchFiles(files);
    }

    /** @brief What `backup`'s commitRestore(), discardRestore() or discardDeferredRestore() answers next. */
    Q_INVOKABLE void
    setBackupReply(const QString& call, const QVariantMap& reply) {
        if (m_backup != nullptr) {
            m_backup->setReply(call, reply);
        }
    }

    /** @brief Deliver exportPrepared, exportFinished or inspectFinished with @p result. */
    Q_INVOKABLE bool
    emitBackup(const QString& signal, const QVariantMap& result) {
        return m_backup != nullptr && m_backup->emitResult(signal, result);
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
     * its import transport and parsing what landed, and QML cannot
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

    /** @brief A disposable fixture of @p bytes zero bytes (sparse where it can be), for the import size limits. */
    Q_INVOKABLE QString
    writeSizedFixture(const QString& name, double bytes) const {
        const QString path = writeFixtureCsv(name, QString());
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadWrite) || !file.resize(static_cast<qint64>(bytes))) {
            return QString();
        }
        return path;
    }

    /**
     * @brief Make @p path impossible to replace or delete as a file, or (@p blocked false) put it back.
     *
     * The file is moved aside and a non-empty directory takes its place, so a store saved there fails and a file
     * removed there stays, whatever the permissions the suite runs with (issue #801). Answers whether it worked.
     */
    Q_INVOKABLE bool
    blockPath(const QString& path, bool blocked) const {
        if (blocked) {
            return QFile::rename(path, path + QStringLiteral(".held")) && QDir().mkpath(path + QStringLiteral("/held"));
        }
        return QDir(path).removeRecursively() && QFile::rename(path + QStringLiteral(".held"), path);
    }

    /**
     * @brief Block the saved systems store (blockPath()) from inside the @p call-th place fill savedSystems runs while
     * it builds new rows, so a save fails part way through a batch whose rows carry a place; 0 removes the fill.
     * Unblock with blockPath(storePath("saved_systems.json"), false).
     */
    Q_INVOKABLE void
    blockSystemsStoreAtPlaceFill(int call) {
        if (call <= 0) {
            m_saved_systems->setPlaceNameCache({});
            return;
        }
        auto fills = std::make_shared<int>(0);
        const QString store = storePath(QStringLiteral("saved_systems.json"));
        // A case checks the store is blocked with fileExists(), false once a folder stands there.
        m_saved_systems->setPlaceNameCache([this, fills, call, store](QVariantMap&) {
            if (++*fills == call) {
                blockPath(store, true);
            }
        });
    }

    /**
     * @brief Import @p path as @p type through the library's own async import, and answer its result once it is in
     * (#832); a fixture's one-line import. The stored copy takes the file's own name.
     */
    Q_INVOKABLE QVariantMap
    importNow(const QString& path, const QString& type) {
        return dsd_test_import_and_wait(*m_imported_files, path, type);
    }

    /** @brief importNow() of a copy of @p path picked under @p name, so the stored copy takes that name. */
    Q_INVOKABLE QVariantMap
    importNamed(const QString& path, const QString& name, const QString& type) {
        return dsd_test_import_named_and_wait(*m_imported_files, path, name, type);
    }

    /**
     * @brief Hold every import copy and document probe before it starts (#832, #831), or let the held ones go on. A
     * cancel lets its copy go on (to answer "cancelled") unless @p evenCancelled.
     */
    Q_INVOKABLE void
    holdImports(bool held, bool evenCancelled = false) {
        m_import_host->importGate->hold(held, evenCancelled);
    }

    /** @brief How many import copies are held right now: a case waits for its copy to reach the gate. */
    Q_INVOKABLE int
    heldImports() const {
        const std::lock_guard<std::mutex> guard(m_import_host->importGate->lock);
        return m_import_host->importGate->waiting;
    }

    /** @brief Every reference the import copies were asked for, oldest first (#830: a pick reaches them as given). */
    Q_INVOKABLE QStringList
    importReferences() const {
        const std::lock_guard<std::mutex> guard(m_import_host->importGate->lock);
        return m_import_host->importGate->references;
    }

    /** @brief The running replay copy has made @p bytes of @p total (-1: size unknown). */
    Q_INVOKABLE void
    replayProgress(double bytes, double total) {
        m_replay_files->progress(static_cast<qint64>(bytes), static_cast<qint64>(total));
    }

    /** @brief Answer the running replay copy with @p result; false when none runs. */
    Q_INVOKABLE bool
    finishReplay(const QVariantMap& result) {
        return m_replay_files->finish(result);
    }

    /** @brief The startup migration moved system @p uid's cached replay @p from to its copy @p to. */
    Q_INVOKABLE void
    migrateReplay(const QString& uid, const QString& from, const QString& to) {
        m_replay_files->migrate(uid, from, to);
    }

    /** @brief What the wizard asked of `replayFiles`: begins, cancels and the last reference. */
    Q_INVOKABLE QVariantMap
    replayCalls() const {
        return {{QStringLiteral("begins"), m_replay_files->begins},
                {QStringLiteral("cancels"), m_replay_files->cancels},
                {QStringLiteral("reference"), m_replay_files->lastReference}};
    }

    /** @brief Forget every replay copy and call, for the next case. */
    Q_INVOKABLE void
    resetReplayFiles() {
        m_replay_files->reset();
    }

    /**
     * @brief Replace the contents of the existing file @p path behind the library's back, as the decoder's in-place
     *        rewrite of a talkgroup list does: the library row keeps the counts of its import.
     */
    Q_INVOKABLE bool
    rewriteFile(const QString& path, const QString& contents) const {
        QFile file(path);
        if (!QFileInfo(path).isFile() || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        const QByteArray bytes = contents.toUtf8();
        return file.write(bytes) == bytes.size();
    }

    /** @brief The absolute path of app store @p name, for blockPath(). */
    Q_INVOKABLE QString
    storePath(const QString& name) const {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/") + name;
    }

    /** @brief Whether @p path names an existing file (not a directory). */
    Q_INVOKABLE bool
    fileExists(const QString& path) const {
        return QFileInfo(path).isFile();
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
            if (m_backup != nullptr) {
                m_backup->setHost(m_lifecycle_host); // the model's session gate follows the host the case runs on
            }
            m_lifecycle_host->startRunning = start_running;
            m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_lifecycle_host);
            m_engine->rootContext()->setContextProperty(QStringLiteral("prefs"), m_app_prefs);
        } else {
            m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
            m_engine->rootContext()->setContextProperty(QStringLiteral("prefs"), m_prefs);
        }
    }

    /** Publish the `signalSearch` model Main.qml reads, so a case can load Main over a search the decoder already
     * owns, as a recreated Activity does. Null withdraws it: the name then reads as undefined again, which is how
     * Main and every other case see a fixture without the model. */
    Q_INVOKABLE void
    setSignalSearchModel(QObject* model) {
        m_engine->rootContext()->setContextProperty(QStringLiteral("signalSearch"),
                                                    model != nullptr ? QVariant::fromValue(model) : QVariant());
    }

    // Qt.platform.os is read-only. Load the production shell with only that
    // platform reading substituted so desktop tests exercise the Android gate.
    // Reject source drift instead of silently changing unrelated platform checks.
    Q_INVOKABLE QString
    androidMainQml() const {
        QFile file(QStringLiteral(DSD_QML_UI_DIR "/Main.qml"));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        QString text = QString::fromUtf8(file.readAll());
        file.close();
        const QString pattern = QStringLiteral("Qt.platform.os");
        if (text.count(pattern) != 1) {
            return QString();
        }
        return text.replace(pattern, QStringLiteral("\"android\""));
    }

    /** The argv the lifecycle host was last asked to start with. */
    Q_INVOKABLE QStringList
    lastStartArgs() const {
        return m_lifecycle_host->lastArgv;
    }

    Q_INVOKABLE void
    setNotificationPermissionNeeded(bool needed) {
        m_lifecycle_host->setNotificationPermissionNeeded(needed);
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
    setLifecycleFailure(const QString& text) {
        m_lifecycle_host->failure = text;
        Q_EMIT m_lifecycle_host->sessionStateChanged();
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
    // #647: as Android's takeAttachment() words it, "<kind>:<device name>", for an RTL-SDR unless @p kind says.
    Q_INVOKABLE void
    emitLocalDeviceAttached(const QString& kind = QStringLiteral("usb")) {
        Q_EMIT m_lifecycle_host->localDeviceAttached(kind + QStringLiteral(":fixture"));
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

    /** @brief Make the lifecycle host broker USB access (as Android does) or not (as the desktop does). */
    Q_INVOKABLE void
    setUsbBrokered(bool brokered) {
        m_lifecycle_host->usbBrokered = brokered;
        Q_EMIT m_lifecycle_host->localDeviceChanged();
    }

    /** @brief Plug in a receiver kind ("usb", "airspy", "both" or empty) on both lifecycle hosts. */
    Q_INVOKABLE void
    setAttachedDeviceSource(const QString& source) {
        for (ImportOnlyHost* host : {m_import_host, m_initializing_host}) {
            host->attachedSource = source;
            Q_EMIT host->localDeviceChanged();
        }
    }

    /**
     * @brief Publish the rest of the brokered USB record (#647): the connected receiver's kind ("" while none is
     * connected, as Android reports it), whether an access request awaits its answer, and the kind it asked for.
     */
    Q_INVOKABLE void
    setLocalDeviceRecord(const QString& source, bool pending, const QString& requestedSource) {
        m_lifecycle_host->usbSource = source;
        m_lifecycle_host->usbPending = pending;
        m_lifecycle_host->usbRequestedSource = requestedSource;
        Q_EMIT m_lifecycle_host->localDeviceChanged();
    }

    /** @brief Make an access request leave a permission prompt pending, as Android's does (#647). */
    Q_INVOKABLE void
    setDeviceRequestPends(bool pends) {
        m_lifecycle_host->requestPends = pends;
    }

    /** @brief Report the last session's receiver as "ok", "lost" or "failed", with its kind (#647). */
    Q_INVOKABLE void
    setReceiverHealth(const QString& state, const QString& source) {
        QVariantMap health = dsd_qt::DecoderHost::healthyReceiver();
        if (state != QStringLiteral("ok")) {
            health[QStringLiteral("state")] = state;
            health[QStringLiteral("source")] = source;
            health[QStringLiteral("session")] = qlonglong(1);
        }
        m_lifecycle_host->health = health;
        Q_EMIT m_lifecycle_host->runResultChanged();
    }

    Q_INVOKABLE int
    dongleRetryRequests() const {
        return m_lifecycle_host->usbRequests;
    }

    /** @brief The access requests that named a receiver, and the last one's kind and serial (#647). */
    Q_INVOKABLE QVariantMap
    lastDeviceAccess() const {
        return {{QStringLiteral("count"), m_lifecycle_host->accessForSourceRequests},
                {QStringLiteral("source"), m_lifecycle_host->accessSource},
                {QStringLiteral("serial"), m_lifecycle_host->accessSerial}};
    }

    Q_INVOKABLE void
    resetDeviceAccess() {
        m_lifecycle_host->accessForSourceRequests = 0;
        m_lifecycle_host->accessSource.clear();
        m_lifecycle_host->accessSerial.clear();
    }

    /** @brief How the last replay ended, as DecoderHost::terminalReason() reports it: 1 completed, 2 stopped. */
    Q_INVOKABLE void
    setTerminalReason(int reason) {
        m_lifecycle_host->terminal = reason;
        Q_EMIT m_lifecycle_host->runResultChanged();
    }

    /** @brief The last session's input failure kind, as DecoderHost::inputFailureKind() reports it. */
    Q_INVOKABLE void
    setLifecycleFailureKind(int kind) {
        m_lifecycle_host->failureKind = kind;
        Q_EMIT m_lifecycle_host->runResultChanged();
    }

    // WP-D3: platform capability gate for the nearby search button.
    Q_INVOKABLE void
    setLocationSupported(bool supported) {
        m_host[QStringLiteral("locationSupported")] = supported;
        m_host_readings->insert(QStringLiteral("locationSupported"), supported);
        if (m_saved_system_cards != nullptr) {
            m_saved_system_cards->setLocationSupported(supported); // as qt_ui.cpp sets it from the host
        }
        m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
    }

    /**
     * @brief Claim or deny the screen policy (#574) on every host a case can use.
     *
     * The map and both lifecycle hosts all answer, so it holds with or without
     * useLifecycleHost(). Production reads it as a CONSTANT, so set it before
     * loading the screen under test.
     */
    Q_INVOKABLE void
    setScreenPolicySupported(bool supported) {
        m_import_host->screenPolicy = supported;
        m_initializing_host->screenPolicy = supported;
        m_host[QStringLiteral("screenPolicySupported")] = supported;
        m_host_readings->insert(QStringLiteral("screenPolicySupported"), supported);
        QQmlContext* ctx = m_engine->rootContext();
        if (ctx->contextProperty(QStringLiteral("decoderHost")).value<QObject*>() == m_host_readings) {
            ctx->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
        }
    }

    /**
     * @brief Claim or deny the app's document picker (#829) on every host a case can use; true until a case says.
     *
     * Production reads it as a CONSTANT, so set it before loading the screen under test.
     */
    Q_INVOKABLE void
    setDocumentPickerSupported(bool supported) {
        m_import_host->pickerSupported = supported;
        m_initializing_host->pickerSupported = supported;
        m_host[QStringLiteral("documentPickerSupported")] = supported;
        m_host_readings->insert(QStringLiteral("documentPickerSupported"), supported);
        if (hostReadingsInstalled()) {
            m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
        }
    }

    /** @brief The installed host's last document request, {id, kind ("open" or "save"), name, mime}; {} before one. */
    Q_INVOKABLE QVariantMap
    lastDocumentRequest() const {
        return hostReadingsInstalled() ? m_host_readings->lastDocumentRequest : m_lifecycle_host->lastDocumentRequest;
    }

    /** @brief The installed host's picker calls (requests, cancels, discards), oldest first, as {name, ...}. */
    Q_INVOKABLE QVariantList
    documentCalls() const {
        if (!hostReadingsInstalled()) {
            return m_lifecycle_host->documentCalls;
        }
        static const QStringList names = {QStringLiteral("requestOpenDocument"), QStringLiteral("requestSaveDocument"),
                                          QStringLiteral("cancelDocumentRequest"),
                                          QStringLiteral("discardPendingDownload")};
        QVariantList picker;
        for (const QVariant& call : m_host_readings->calls) {
            if (names.contains(call.toMap().value(QStringLiteral("name")).toString())) {
                picker.append(call);
            }
        }
        return picker;
    }

    /** @brief Answer request @p id on the installed host with a document, as a platform picker does. */
    Q_INVOKABLE void
    answerDocumentPicked(qint64 id, const QString& reference, const QString& displayName, bool inDownloads) {
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->documentPicked(id, reference, displayName, inDownloads);
        } else {
            Q_EMIT m_lifecycle_host->documentPicked(id, reference, displayName, inDownloads);
        }
    }

    /** @brief Answer request @p id on the installed host: no document (backed out, or superseded). */
    Q_INVOKABLE void
    answerDocumentCancelled(qint64 id) {
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->documentPickCancelled(id);
        } else {
            Q_EMIT m_lifecycle_host->documentPickCancelled(id);
        }
    }

    /** @brief Answer request @p id on the installed host: the picker could not ask, for @p reason. */
    Q_INVOKABLE void
    answerDocumentUnavailable(qint64 id, const QString& reason) {
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->documentPickUnavailable(id, reason);
        } else {
            Q_EMIT m_lifecycle_host->documentPickUnavailable(id, reason);
        }
    }

    /**
     * @brief Offer @p reference on the installed host as a document another app opened with this one (#831), as the
     * Android host's poll does: it replaces one not taken, and incomingDocumentChanged() follows.
     */
    Q_INVOKABLE void
    emitIncomingDocument(const QString& reference) {
        if (hostReadingsInstalled()) {
            m_host_readings->offerIncomingDocument(reference);
        } else {
            m_lifecycle_host->offerIncomingDocument(reference);
        }
    }

    /** @brief Whether the installed host has a picker request in flight (documentRequestPending, #831). */
    Q_INVOKABLE void
    setDocumentRequestPending(bool pending) {
        if (hostReadingsInstalled()) {
            m_host_readings->setDocumentRequestPending(pending);
        } else {
            m_lifecycle_host->setDocumentRequestPending(pending);
        }
    }

    /**
     * @brief Hold the lifecycle host's next recording save or export open (#831), or (@p held false) end the one held,
     * as done, and run them again as the base host does.
     */
    Q_INVOKABLE void
    holdRecordingTransfers(bool held) {
        m_lifecycle_host->holdTransfers = held;
        if (!held) {
            m_lifecycle_host->endHeldTransfer();
        }
    }

    /** @brief Whether the installed host knows its session state yet (sessionStateKnown, #831). */
    Q_INVOKABLE void
    setSessionStateKnown(bool known) {
        if (hostReadingsInstalled()) {
            m_host_readings->setSessionStateKnown(known);
        } else {
            m_lifecycle_host->setSessionStateKnown(known);
        }
    }

    /** @brief The Download folder name every host reports for @p reference through savedDocumentName(). */
    Q_INVOKABLE void
    setSavedDocumentName(const QString& reference, const QString& name) {
        m_import_host->savedDocumentNames.insert(reference, name);
        m_initializing_host->savedDocumentNames.insert(reference, name);
        m_host_readings->savedDocumentNames.insert(reference, name);
    }

    /**
     * @brief Claim or deny the file browser (#830) on every host a case can use; false until a case says.
     *
     * Production reads it as a CONSTANT, so set it before loading the screen under test.
     */
    Q_INVOKABLE void
    setFileBrowserSupported(bool supported) {
        m_import_host->browserSupported = supported;
        m_initializing_host->browserSupported = supported;
        m_host[QStringLiteral("fileBrowserSupported")] = supported;
        m_host_readings->insert(QStringLiteral("fileBrowserSupported"), supported);
        m_replay_files->copiesLocalFiles = supported;
        if (hostReadingsInstalled()) {
            m_engine->rootContext()->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);
        }
    }

    /** @brief Publish @p access as every host's fileAccess, and emit fileAccessChanged() on the installed one. */
    Q_INVOKABLE void
    setFileAccess(const QVariantMap& access) {
        m_import_host->access = access;
        m_initializing_host->access = access;
        m_host[QStringLiteral("fileAccess")] = access;
        m_host_readings->insert(QStringLiteral("fileAccess"), access);
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->fileAccessChanged();
        } else {
            Q_EMIT m_lifecycle_host->fileAccessChanged();
        }
    }

    /** @brief The installed host's file browser calls, oldest first, as ImportOnlyHost::fileCall() shapes them. */
    Q_INVOKABLE QVariantList
    fileCalls() const {
        if (!hostReadingsInstalled()) {
            return m_lifecycle_host->fileCalls;
        }
        QVariantList browser;
        for (const QVariant& call : m_host_readings->calls) {
            if (ImportOnlyHost::fileCallNames().contains(call.toMap().value(QStringLiteral("name")).toString())) {
                browser.append(call);
            }
        }
        return browser;
    }

    /** @brief The installed host's last file browser call named @p name; {} before one. */
    Q_INVOKABLE QVariantMap
    lastFileCall(const QString& name) const {
        const QVariantList calls = fileCalls();
        for (auto it = calls.crbegin(); it != calls.crend(); ++it) {
            const QVariantMap call = it->toMap();
            if (call.value(QStringLiteral("name")).toString() == name) {
                return call;
            }
        }
        return {};
    }

    /** @brief Answer roots request @p id on the installed host with @p result ({status, generation, roots}). */
    Q_INVOKABLE void
    answerFileRoots(qint64 id, const QVariantMap& result) {
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->fileRootsListed(id, result);
        } else {
            Q_EMIT m_lifecycle_host->fileRootsListed(id, result);
        }
    }

    /** @brief Answer listing or page request @p id on the installed host with @p result. */
    Q_INVOKABLE void
    answerFolderListing(qint64 id, const QVariantMap& result) {
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->folderListed(id, result);
        } else {
            Q_EMIT m_lifecycle_host->folderListed(id, result);
        }
    }

    /** @brief Answer hand-off request @p id on the installed host with @p result ({ok, reference, name, reason}). */
    Q_INVOKABLE void
    answerFileHandoff(qint64 id, const QVariantMap& result) {
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->fileHandoffChecked(id, result);
        } else {
            Q_EMIT m_lifecycle_host->fileHandoffChecked(id, result);
        }
    }

    /** @brief Answer access request @p id on the installed host with @p outcome. */
    Q_INVOKABLE void
    answerFileAccess(qint64 id, const QString& outcome) {
        if (hostReadingsInstalled()) {
            Q_EMIT m_host_readings->fileAccessRequestDone(id, outcome);
        } else {
            Q_EMIT m_lifecycle_host->fileAccessRequestDone(id, outcome);
        }
    }

    /** @brief What the readings host's copyText() was asked to copy since the last resetHostPlayback(), oldest first. */
    Q_INVOKABLE QStringList
    copiedTexts() const {
        return m_host_readings->copiedTexts;
    }

    /** @brief Issue #830: whether `restartCloser.refused()` says a copy would be cut short. */
    Q_INVOKABLE void
    setCloseRefused(bool refused) {
        m_restart_closer->closeRefused = refused;
    }

    /** @brief How many closes `restartCloser.begin()` has begun on this engine. */
    Q_INVOKABLE int
    closeBegins() const {
        return m_restart_closer->begins;
    }

    /**
     * @brief End the close `restartCloser` is running with @p outcome ("closing", "stopFailed", "refused" or
     * "quitFailed").
     */
    Q_INVOKABLE void
    finishClose(const QString& outcome) {
        m_restart_closer->finish(outcome);
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

    /** @brief Issue #672: the keys the avoid sheet's Put back buttons sent, oldest first. */
    Q_INVOKABLE QStringList
    restoredScanAvoids() const {
        return (m_commands != nullptr) ? m_commands->restoredScanAvoids() : QStringList();
    }

    /**
     * @brief Issue #672: make the avoid list (the real ScanAvoidListModel `metrics.scanAvoidModel` is) the entries
     * @p entries, each {key, label} and an optional targetId (issue #644), in order, as MetricsModel hands it a new
     * list: by key, so a row whose key stays keeps its delegate. The readings beside it (scanAvoidCount and the rest)
     * are the case's to set.
     */
    Q_INVOKABLE void
    setScanAvoids(const QVariantList& entries) {
        if (m_scan_avoid_model == nullptr) {
            return;
        }
        QList<dsd_qt::ScanAvoidListModel::Entry> next;
        for (const QVariant& value : entries) {
            const QVariantMap entry = value.toMap();
            dsd_qt::ScanAvoidListModel::Entry row{entry.value("key").toString(), entry.value("label").toString(), {}};
            row.identity.kind = DSD_APP_SCAN_AVOID_KIND_TRUNK_SCAN;
            const QByteArray target = entry.value("targetId").toString().toUtf8();
            DSD_SNPRINTF(row.identity.target_id, sizeof row.identity.target_id, "%s", target.constData());
            next.append(row);
        }
        m_scan_avoid_model->apply(std::move(next));
    }

    /**
     * @brief Issue #697: hand the Scan lists sheet's model (the real ScanListToggleModel `metrics.scanListModel` is) a
     * frame: @p view is {session, seq, paused, activeList, memberships, lists: [{name, on, targets, onAir}]}, as the
     * coordinator publishes it. `activeList` is the list that names the target on air (0 or absent: none); `onAir`
     * marks each list holding it. `memberships` maps a membership mask (bit N-1: list N, as a number) to the distinct
     * targets in exactly those lists, from which the sheet totals the lists on; without it, no list shares a target and
     * each list's `targets` are its own mask's. The readings beside it (scanListCount and the rest) are the case's to
     * set. Results answered with answerScanList() are read here.
     */
    Q_INVOKABLE void
    setScanLists(const QVariantMap& view) {
        dsd_app_scan_list_view next{};
        next.session = view.value("session", 7).toUInt();
        next.lists_seq = view.value("seq", 1).toUInt();
        const bool paused = view.value("paused").toBool();
        const QVariantList lists = view.value("lists").toList();
        next.list_count = static_cast<uint8_t>(qMin(lists.size(), qsizetype{DSD_TRUNK_SCAN_MAX_LISTS}));
        next.running = paused ? 0U : 1U;
        next.paused = paused ? 1U : 0U;
        next.toggleable = !paused && next.list_count >= 2 ? 1U : 0U;
        next.active_list = static_cast<uint8_t>(view.value("activeList", 0).toUInt());
        const bool memberships = view.contains("memberships");
        for (int i = 0; i < next.list_count; ++i) {
            const QVariantMap list = lists.at(i).toMap();
            dsd_app_scan_list_entry& entry = next.lists[i];
            entry.number = static_cast<uint8_t>(i + 1);
            entry.enabled = list.value("on", true).toBool() ? 1U : 0U;
            entry.targets = static_cast<uint16_t>(list.value("targets").toUInt());
            entry.has_active = list.value("onAir").toBool() ? 1U : 0U;
            const QByteArray name = list.value("name").toString().toUtf8();
            DSD_SNPRINTF(entry.name, sizeof entry.name, "%s", name.constData());
            if (!memberships) {
                next.membership_counts[(1U << i) - 1U] = entry.targets;
            }
        }
        const QVariantMap counts = view.value("memberships").toMap();
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            const unsigned int mask = it.key().toUInt();
            if (mask >= 1U && mask <= DSD_TRUNK_SCAN_MEMBERSHIP_MASKS) {
                next.membership_counts[mask - 1U] = static_cast<uint16_t>(it.value().toUInt());
            }
        }
        m_scan_list_view = next;
        if (m_scan_list_model != nullptr) {
            m_scan_list_model->refresh(m_scan_list_view);
        }
    }

    /** @brief Issue #697: where a manual scan list's generated targets are written (scan_list_csv_path()). */
    Q_INVOKABLE QString
    scanListCsvPath(const QString& uid, const QString& name) const {
        return dsd_qt::scan_list_csv_path(uid, name, 1);
    }

    /** @brief The switches the sheet sent, oldest first, as {request, session, list, enabled}. */
    Q_INVOKABLE QVariantList
    scanListRequests() const {
        QVariantList out;
        for (const auto& payload : m_scan_list_sent) {
            out.append(QVariantMap{{"request", payload.request},
                                   {"session", payload.session},
                                   {"list", payload.list},
                                   {"enabled", payload.enabled}});
        }
        return out;
    }

    /** @brief Answer switch @p request with @p code (dsd_app_scan_list_set_code), the lists then at @p listsSeq, and
     *  hand the model the last frame again, as the next refresh would. */
    Q_INVOKABLE void
    answerScanList(int request, int code, int listsSeq) {
        dsd_app_scan_list_result result{};
        result.sequence = ++m_scan_list_latest;
        result.request = static_cast<uint32_t>(request);
        result.session = m_scan_list_view.session;
        result.code = code;
        result.lists_seq = static_cast<uint32_t>(listsSeq);
        m_scan_list_results.append(result);
        if (m_scan_list_model != nullptr) {
            m_scan_list_model->refresh(m_scan_list_view);
        }
    }

    /** @brief Whether the command queue refuses the next switches, as a full queue does. */
    Q_INVOKABLE void
    setScanListQueueFull(bool full) {
        m_scan_list_queue_full = full;
    }

    /** @brief Issue #801: no pinned Listen shortcuts, through the real AppPrefs `listenPins` reads. */
    Q_INVOKABLE void
    resetPins() {
        if (m_app_prefs != nullptr) {
            m_app_prefs->setPinned({});
        }
    }

    /** @brief Issue #646: Listen back on its default tab, through the real AppPrefs. A Home already built keeps the
     *         tab it opened on. */
    Q_INVOKABLE void
    resetListenSection() {
        if (m_app_prefs != nullptr) {
            m_app_prefs->setListenSection(QStringLiteral("systems"));
        }
    }

    /** @brief The saved-systems order back on its default, through the real AppPrefs. */
    Q_INVOKABLE void
    resetSystemsSort() {
        if (m_app_prefs != nullptr) {
            m_app_prefs->setSystemsSort(QStringLiteral("added"));
        }
    }

    /** @brief Age the stored location fix past its 24 h behind AppPrefs, with no signal: as a phone that slept through
     *         the expiry timer finds it, dropped only at the next read. */
    Q_INVOKABLE void
    lapseLocationFix() {
        QSettings(QSettings::IniFormat, QSettings::UserScope, dsd_qt::AppPrefs::settingsOrganization(),
                  dsd_qt::AppPrefs::settingsApplication())
            .setValue(QStringLiteral("location/lastFixAt"), dsd_qt::realtimeMSecsSinceEpoch() - 25LL * 60 * 60 * 1000);
    }

    /** @brief The saved-system cards as a fresh Home finds them: no search, no screen reader, nothing held. Location
     *         support is left as setLocationSupported() last set it on the host and the cards together. */
    Q_INVOKABLE void
    resetSavedSystemCards() {
        if (m_saved_system_cards != nullptr) {
            m_saved_system_cards->setQuery(QString());
            m_saved_system_cards->setScreenReaderActive(false);
            m_saved_system_cards->setHoldReorders(false);
        }
    }

    /** @brief `prefs` as the real AppPrefs (true) or the plain map (false), the `decoderHost` left as it is: a case
     *         that needs both the map's host readings and the production preferences (Listen's sort and location). */
    Q_INVOKABLE void
    useAppPrefs(bool on) {
        m_engine->rootContext()->setContextProperty(
            QStringLiteral("prefs"), on ? QVariant::fromValue<QObject*>(m_app_prefs) : QVariant(m_prefs));
    }

    /** @brief The `rrPlaces` fake as a fresh run has it: signed in, nothing unplaced, no update, nothing recorded. */
    Q_INVOKABLE void
    resetRrPlaces() {
        if (m_rr_places == nullptr) {
            return;
        }
        m_rr_places->cancelUpdate();
        m_rr_places->signedIn = true;
        m_rr_places->setUnplacedCount(0);
        m_rr_places->unplacedUidList.clear();
        m_rr_places->updateUids.clear();
        m_rr_places->resolveRequests.clear();
        m_rr_places->cancels = 0;
        m_rr_places->updateCalls = 0;
        m_rr_places->clearSummary();
    }

    /** @brief Whether the `rrPlaces` fake answers as signed in to RadioReference. */
    Q_INVOKABLE void
    setRrPlacesSignedIn(bool signedIn) {
        m_rr_places->signedIn = signedIn;
    }

    /** @brief How many saved systems the `rrPlaces` fake reports without a place (its unplacedText follows). */
    Q_INVOKABLE void
    setRrPlacesUnplaced(int count) {
        m_rr_places->setUnplacedCount(count);
    }

    /** @brief The saved systems' uids the `rrPlaces` fake answers unplacedUids() with. */
    Q_INVOKABLE void
    setRrPlacesUnplacedUids(const QStringList& uids) {
        m_rr_places->unplacedUidList = uids;
    }

    /** @brief The uids the running (or last) Update from RadioReference was asked for. */
    Q_INVOKABLE QStringList
    rrPlacesUpdateUids() const {
        return m_rr_places->updateUids;
    }

    /** @brief How many times QML called updateFromRadioReference(), refused calls included. */
    Q_INVOKABLE int
    rrPlacesUpdateCalls() const {
        return m_rr_places->updateCalls;
    }

    /** @brief How many running updates QML cancelled. */
    Q_INVOKABLE int
    rrPlacesCancels() const {
        return m_rr_places->cancels;
    }

    /** @brief End the running Update from RadioReference with the service's summary. */
    Q_INVOKABLE void
    finishRrPlacesUpdate(int updated, int unreachable) {
        m_rr_places->finish(updated, unreachable);
    }

    /** @brief Issue #646: store @p value under the settings key @p key as it is, past AppPrefs' checks, as a damaged
     *         file or another build would leave it. The real AppPrefs reads the same file. */
    Q_INVOKABLE void
    storeRawPref(const QString& key, const QVariant& value) {
        QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("dsd-neo"), QStringLiteral("dsd-neo-app"))
            .setValue(key, value);
    }

    /** @brief No scan lists: an empty frame, and every switch and answer forgotten. */
    Q_INVOKABLE void
    resetScanLists() {
        m_scan_list_sent.clear();
        m_scan_list_results.clear();
        m_scan_list_queue_full = false;
        setScanLists({{"session", 0}, {"lists", QVariantList()}});
    }

    /** @brief Issue #672: @p count entries "k0".., labelled "#n  target-n  851.012500 MHz", for a long list. */
    Q_INVOKABLE void
    setManyScanAvoids(int count) {
        QVariantList entries;
        for (int i = 0; i < count; ++i) {
            entries.append(QVariantMap{{"key", QStringLiteral("k%1").arg(i)},
                                       {"label", QStringLiteral("#%1  target-%1  851.012500 MHz").arg(i + 1)}});
        }
        setScanAvoids(entries);
    }

    /** @brief How many times the screens asked to step back one scan channel (issue #666). */
    Q_INVOKABLE int
    previousChannelCalls() const {
        return (m_commands != nullptr) ? m_commands->previousChannelCalls() : -1;
    }

    Q_INVOKABLE int
    nextChannelCalls() const {
        return (m_commands != nullptr) ? m_commands->nextChannelCalls() : -1;
    }

    /** @brief How many times the screens asked to resume a paused scan (issue #671). */
    Q_INVOKABLE int
    resumeScanCalls() const {
        return (m_commands != nullptr) ? m_commands->resumeScanCalls() : -1;
    }

    /** @brief How many override tunes (Go to) the screens have asked for, and the last one's frequency. */
    Q_INVOKABLE int
    tuneHzCalls() const {
        return (m_commands != nullptr) ? m_commands->tuneHzCalls() : -1;
    }

    Q_INVOKABLE double
    lastTuneHz() const {
        return (m_commands != nullptr) ? m_commands->lastTuneHz() : 0.0;
    }

    /** @brief Refuse (false) or admit the next tuner releases, as a full command queue would. */
    Q_INVOKABLE void
    setReleaseTunerAccepted(bool accepted) {
        if (m_commands != nullptr) {
            m_commands->setReleaseTunerAccepted(accepted);
        }
    }

    /** @brief The row on air the "this channel" editors capture, and the session edits they send (issue #518). */
    Q_INVOKABLE void
    setScanRowContext(const QVariantMap& context) {
        if (m_commands != nullptr) {
            m_commands->setScanRowContext(context);
        }
    }

    Q_INVOKABLE int
    scanRowEditCalls() const {
        return (m_commands != nullptr) ? m_commands->scanRowEditCalls() : -1;
    }

    Q_INVOKABLE QVariantMap
    lastScanRowEdit() const {
        return (m_commands != nullptr) ? m_commands->lastScanRowEdit() : QVariantMap();
    }

    /** @brief What the radio panel last asked the engine for. */
    Q_INVOKABLE int
    gainCalls() const {
        return (m_commands != nullptr) ? m_commands->gainCalls() : -1;
    }

    Q_INVOKABLE int
    airspyCalls() const {
        return (m_commands != nullptr) ? m_commands->airspyCalls() : -1;
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

    /** @brief The last auto squelch margin the Radio sheet asked for, and how many times it asked. */
    Q_INVOKABLE int
    lastSquelchMarginDb() const {
        return (m_commands != nullptr) ? m_commands->lastSquelchMarginDb() : -1;
    }

    Q_INVOKABLE int
    squelchAutoCalls() const {
        return (m_commands != nullptr) ? m_commands->squelchAutoCalls() : -1;
    }

    /** @brief How many times the Radio sheet asked for the noise squelch (its margin is lastSquelchMarginDb()). */
    Q_INVOKABLE int
    squelchNoiseCalls() const {
        return (m_commands != nullptr) ? m_commands->squelchNoiseCalls() : -1;
    }

    /** @brief How many times the Radio sheet asked for the configured level back from Auto. */
    Q_INVOKABLE int
    restoreSquelchCalls() const {
        return (m_commands != nullptr) ? m_commands->restoreSquelchCalls() : -1;
    }

    /** @brief The last NFM channel width the Radio sheet asked for (issue #525), and how many times it asked. */
    Q_INVOKABLE int
    lastNfmBandwidthHz() const {
        return (m_commands != nullptr) ? m_commands->lastNfmBandwidthHz() : -1;
    }

    Q_INVOKABLE int
    nfmBandwidthCalls() const {
        return (m_commands != nullptr) ? m_commands->nfmBandwidthCalls() : -1;
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

    /** @brief The last AM channel width the Radio sheet asked for (issue #524), and how many times it asked. */
    Q_INVOKABLE int
    lastAmBandwidthHz() const {
        return (m_commands != nullptr) ? m_commands->lastAmBandwidthHz() : -1;
    }

    Q_INVOKABLE int
    amBandwidthCalls() const {
        return (m_commands != nullptr) ? m_commands->amBandwidthCalls() : -1;
    }

    /** @brief The last tone-filter edit the editor sent (#527), and how many it sent. */
    Q_INVOKABLE int
    lastToneFilterMode() const {
        return (m_commands != nullptr) ? m_commands->lastToneFilterMode() : -1;
    }

    Q_INVOKABLE QString
    lastToneFilterList() const {
        return (m_commands != nullptr) ? m_commands->lastToneFilterList() : QString();
    }

    Q_INVOKABLE int
    toneFilterCalls() const {
        return (m_commands != nullptr) ? m_commands->toneFilterCalls() : -1;
    }

    Q_INVOKABLE QFont
    applicationFont() const {
        return QGuiApplication::font();
    }

    Q_INVOKABLE void
    setApplicationFont(const QFont& font) {
        QGuiApplication::setFont(font);
    }

    /**
     * @brief Give the focused field composing text, as an Android keyboard does mid-word.
     *
     * The event carries only a preedit string and no commit, which is exactly
     * what the platform input method sends while a word is being typed: the
     * field shows it underlined and leaves it out of `text`. Returns whether the
     * focused item now holds it as its preedit.
     */
    Q_INVOKABLE bool
    composeText(const QString& preedit) {
        QObject* focus = QGuiApplication::focusObject();
        if (focus == nullptr) {
            return false;
        }
        QInputMethodEvent event(preedit, {});
        QCoreApplication::sendEvent(focus, &event);
        return focus->property("preeditText").toString() == preedit;
    }

    Q_INVOKABLE QStringList
    missingContextKeys(const QStringList& qmlFiles) const {
        /* `backup`, `rrPlaces` and `savedSystemCards` are QObjects, whose properties answer an unknown name with
         * `undefined` just as a map does; their keys are the properties the fakes (or the real cards model) publish. */
        const auto propertyKeys = [](const QMetaObject& meta) {
            QVariantMap keys;
            for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
                keys.insert(QString::fromLatin1(meta.property(i).name()), true);
            }
            return keys;
        };
        const QVariantMap backupKeys = propertyKeys(BackupRecorder::staticMetaObject);
        const QHash<QString, QVariantMap> maps = {
            {QStringLiteral("metrics"), m_metrics},
            {QStringLiteral("prefs"), m_prefs},
            {QStringLiteral("decoderHost"), m_host},
            {QStringLiteral("radioReference"), m_radio_reference},
            {QStringLiteral("backup"), backupKeys},
            {QStringLiteral("rrPlaces"), propertyKeys(RrPlacesFake::staticMetaObject)},
            {QStringLiteral("savedSystemCards"), propertyKeys(dsd_qt::SavedSystemCards::staticMetaObject)},
            {QStringLiteral("recordings"), RecordingsFake::rest()},
            {QStringLiteral("recordingStatus"), m_recording_status}};
        /* Group 3 captures the "(" that marks a call rather than a read. The name must not follow a "." or a word
         * character: `model.recordings.length` is a role's length, not a read of the `recordings` store. */
        static const QRegularExpression read(
            QStringLiteral("(?<![.\\w])(metrics|prefs|decoderHost|radioReference|backup|rrPlaces|savedSystemCards|"
                           "recordings|recordingStatus)"
                           "\\.([A-Za-z_][A-Za-z0-9_]*)\\s*(\\()?"));

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

        /* Issue #782: the qml_a11y cases measure what an accessibility client would receive, so accessibility has to
         * be on before any engine or item exists. Controls attach their accessible objects only once it is on, and
         * items raise LocationChanged only while it is on; with it off those cases would count nothing and pass. The
         * offscreen platform's accessibility tells the activation observers, as a platform bridge would. */
        if (qEnvironmentVariable("DSD_QML_TEST_ACCESSIBILITY") == QStringLiteral("1")) {
            QAccessible::setActive(true);
            if (!QAccessible::isActive()) {
                qFatal("DSD_QML_TEST_ACCESSIBILITY=1: QAccessible::setActive(true) did not take on platform '%s'",
                       qPrintable(QGuiApplication::platformName()));
            }
            m_accessibility_activated = true;
        }
    }

    /** @brief Issue #782: undo what applicationAvailable() turned on, once every test file has run. */
    void
    cleanupTestCase() {
        AccessibilityCounter::stop();
        if (m_accessibility_activated) {
            QAccessible::setActive(false);
            m_accessibility_activated = false;
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
        /* Issue #574: AppPrefs' defaults, System default and 30 s. */
        prefs[QStringLiteral("screenMode")] = 0;
        prefs[QStringLiteral("screenDelaySec")] = 30;
        prefs[QStringLiteral("screenDelayChoices")] = QVariantList({10, 30, 60, 120, 300});
        prefs[QStringLiteral("skipEncrypted")] = false;
        prefs[QStringLiteral("persistTgLockouts")] = true;
        prefs[QStringLiteral("relaxedCrc")] = false;
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
        /* Empty too: Signal search then seeds from Explore's answer, else USB. */
        prefs[QStringLiteral("searchSourceType")] = QString();
        prefs[QStringLiteral("searchHost")] = QString();
        prefs[QStringLiteral("searchPort")] = 1234;
        /* Closed, as on a fresh install (#718). */
        prefs[QStringLiteral("monitorDetailsExpanded")] = false;
        /* Issue #646: Listen opens on Systems, as on a fresh install. */
        prefs[QStringLiteral("listenSection")] = QStringLiteral("systems");
        prefs[QStringLiteral("systemsSort")] = QStringLiteral("added");
        /* Issue #830: Android's picker first, as on a fresh install. */
        prefs[QStringLiteral("chooseFilesInApp")] = false;
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
        prefs[QStringLiteral("lastStartedCompanions")] = QStringList();
        prefs[QStringLiteral("acceptedStartCompanions")] = QStringList();
        prefs[QStringLiteral("acceptedStartKind")] = QString();
        prefs[QStringLiteral("acceptedStartUid")] = QString();
        /* Issue #644: the accepted scan session's targets, none before a start. */
        prefs[QStringLiteral("acceptedStartTargets")] = QVariantList();
        prefs[QStringLiteral("lastLat")] = 0.0;
        prefs[QStringLiteral("lastLon")] = 0.0;
        prefs[QStringLiteral("lastFixAt")] = 0;
        prefs[QStringLiteral("lastAccuracyM")] = 0.0;
        /* Issue #708: AppPrefs' defaults, Record calls off and the 1 GB limit, with its choices. */
        prefs[QStringLiteral("recordCalls")] = false;
        prefs[QStringLiteral("recordUnknownOnly")] = false;
        prefs[QStringLiteral("recordingLimitMb")] = 1000;
        prefs[QStringLiteral("recordingLimitChoices")] = QVariantList({250, 500, 1000, 2000, 5000, 0});
        /* Issue #756: the analog pause hold off, as AppPrefs' default, with its choices. */
        prefs[QStringLiteral("analogMaxGapMs")] = 200;
        prefs[QStringLiteral("analogMaxGapChoices")] = QVariantList({200, 500, 1000, 2000, 3000, 5000, 10000});
        /* Issue #668: the NFM pre-roll at AppPrefs' default, with its choices. */
        prefs[QStringLiteral("analogPrerollMs")] = DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS;
        prefs[QStringLiteral("analogPrerollChoices")] = QVariantList({0, 100, 200, 300, 500, 1000});
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
        metrics[QStringLiteral("directKeys")] = false;
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
        metrics[QStringLiteral("p25Phase2Seed")] = QString();
        metrics[QStringLiteral("p25Phase2SeedSet")] = false;
        metrics[QStringLiteral("dmrColorCode")] = -1;
        metrics[QStringLiteral("dmrSiteText")] = QString();
        metrics[QStringLiteral("dmrRestLsn")] = 0;
        metrics[QStringLiteral("dmrXptFreeLcn")] = -1;
        metrics[QStringLiteral("nxdnRan")] = -1;
        metrics[QStringLiteral("nxdnLocationCategory")] = QString();
        metrics[QStringLiteral("nxdnSysCode")] = 0;
        metrics[QStringLiteral("nxdnSiteCode")] = 0;
        metrics[QStringLiteral("edacsSiteText")] = QString();
        metrics[QStringLiteral("edacsSystem")] = -1;
        metrics[QStringLiteral("edacsArea")] = -1;
        metrics[QStringLiteral("edacsCcLcn")] = -1;
        metrics[QStringLiteral("dmrBrand")] = QString();
        metrics[QStringLiteral("dmrT3Syscode")] = -1;
        metrics[QStringLiteral("siteAccessCode")] = -1;
        metrics[QStringLiteral("tetraSiteMcc")] = -1;
        metrics[QStringLiteral("tetraSiteMnc")] = -1;
        metrics[QStringLiteral("tetraSiteLa")] = -1;
        metrics[QStringLiteral("ccFreqHz")] = 0;
        metrics[QStringLiteral("vcFreqHz")] = 0;
        metrics[QStringLiteral("siteLine")] = QString();
        metrics[QStringLiteral("siteConfirmed")] = false;
        metrics[QStringLiteral("carrierIdentityTokens")] = QStringList();
        metrics[QStringLiteral("carrierIdentityProtocol")] = QString();
        metrics[QStringLiteral("carrierIdentityText")] = QString();
        metrics[QStringLiteral("carrierIdentityLive")] = false;
        // DMR RAS on the carrier and the Relaxed CRC policy (app_control/dmr_ras_view.h, crc_policy_view.h).
        metrics[QStringLiteral("dmrRasDetected")] = false;
        metrics[QStringLiteral("dmrRasNeedsRelaxed")] = false;
        metrics[QStringLiteral("relaxedCrcConfigured")] = false;
        metrics[QStringLiteral("relaxedCrcEffective")] = false;
        metrics[QStringLiteral("relaxedCrcRowOverride")] = false;

        metrics[QStringLiteral("uiMessage")] = QString();
        // The decode clock's wall-clock now that recent-call ages read: the fixture's decoder is
        // idle, so it is simply the wall clock the screens used to read.
        metrics[QStringLiteral("decodeNowMs")] = QDateTime::currentMSecsSinceEpoch();
        metrics[QStringLiteral("audioMuted")] = false;
        metrics[QStringLiteral("heldTg")] = 0;
        metrics[QStringLiteral("carrierLock")] = false;
        metrics[QStringLiteral("radioInput")] = true;
        // Whether the input in force is an I/Q replay (issue #575): live input at rest, so tuning stays offered.
        metrics[QStringLiteral("replayInput")] = false;
        metrics[QStringLiteral("streamActive")] = false;
        metrics[QStringLiteral("snrValid")] = false;
        metrics[QStringLiteral("snrDb")] = 0.0;
        metrics[QStringLiteral("cfoHz")] = 0.0;
        metrics[QStringLiteral("airspy")] = QVariantMap();
        // The receiver the session runs on, as the Qt start path names it; none at rest.
        metrics[QStringLiteral("radioSource")] = QVariantMap();
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
            // Issue #773: the call kind (dsd_call_kind, 0 unknown), the parties' own labels and the protocol's name.
            metrics[p + QStringLiteral("CallKind")] = 0;
            metrics[p + QStringLiteral("TgLabel")] = QString();
            metrics[p + QStringLiteral("SrcLabel")] = QString();
            metrics[p + QStringLiteral("ProtocolLabel")] = QString();
            metrics[p + QStringLiteral("EncText")] = QString();
            metrics[p + QStringLiteral("TgId")] = 0;
        }
        // Which slot the hero headlines, one-based, 0 for neither. Derived natively by
        // dsd_app_lead_slot() in the real model; here it is a plain reading a case sets
        // alongside the slotNCallState it is meant to agree with.
        metrics[QStringLiteral("leadSlot")] = 0;
        // Whether the carrier is two-slot TDMA (dsd_state::tdma_carrier): false at rest, as on an FDMA carrier.
        metrics[QStringLiteral("twoSlotCarrier")] = false;
        // Targets the encrypted lockout is skipping; 0 is the at-rest value.
        metrics[QStringLiteral("encLockoutCount")] = 0;
        metrics[QStringLiteral("persistTgLockouts")] = true;
        metrics[QStringLiteral("temporaryTgAvoidCount")] = 0;
        metrics[QStringLiteral("callSkipCount")] = 0;
        metrics[QStringLiteral("tgPolicyContext")] = QStringLiteral("0");
        // On-the-fly scan controls (#380): no rotation running at rest.
        metrics[QStringLiteral("scanRotationActive")] = false;
        // Issue #671: no scan, nothing paused, and the tuner never taken from an owner.
        metrics[QStringLiteral("scanKind")] = 0;
        metrics[QStringLiteral("scanPaused")] = false;
        metrics[QStringLiteral("scanResumable")] = false;
        metrics[QStringLiteral("scanEntryCount")] = 0;
        metrics[QStringLiteral("operatorTuned")] = false;
        metrics[QStringLiteral("optionsKnown")] = false;
        metrics[QStringLiteral("scanHold")] = false;
        metrics[QStringLiteral("scanEntryHold")] = false;
        metrics[QStringLiteral("scanTargetId")] = QString();
        metrics[QStringLiteral("scanTargetLabel")] = QString();
        metrics[QStringLiteral("scanTargetOrdinal")] = 0;
        metrics[QStringLiteral("scanTargetCount")] = 0;
        metrics[QStringLiteral("scanAvoidCount")] = 0;
        // Issue #672: the avoid list, as MetricsModel publishes it at rest: whole, empty, nothing to manage.
        metrics[QStringLiteral("scanAvoidsAvailable")] = true;
        metrics[QStringLiteral("scanAvoidsTotalKnown")] = true;
        metrics[QStringLiteral("scanAvoidsManageable")] = false;
        m_scan_avoid_model = new dsd_qt::ScanAvoidListModel(engine);
        metrics[QStringLiteral("scanAvoidModel")] = QVariant::fromValue(static_cast<QObject*>(m_scan_avoid_model));
        // Issue #697: one list or none at rest; the sheet's rows are the real toggle model, answered by the case.
        metrics[QStringLiteral("scanListCount")] = 0;
        metrics[QStringLiteral("scanActiveList")] = 0;
        metrics[QStringLiteral("scanActiveListOff")] = false;
        /* Issue #644: the engine publishes none until a trunk scan runs. */
        metrics[QStringLiteral("scanTargetEligible")] = QVariantList();
        metrics[QStringLiteral("scanTargetOnAir")] = -1;
        metrics[QStringLiteral("scanRotationOrdinal")] = 0;
        metrics[QStringLiteral("scanRotationCount")] = 0;
        m_scan_list_model = new dsd_qt::ScanListToggleModel(engine);
        m_scan_list_model->setBackend(scanListBackend());
        metrics[QStringLiteral("scanListModel")] = QVariant::fromValue(static_cast<QObject*>(m_scan_list_model));
        metrics[QStringLiteral("scanTargetAvoided")] = false;
        // Why the rotation is staying and how long is left (#508). Nothing is
        // published at rest, so the row is down and every budget reads 0.
        metrics[QStringLiteral("scanTimingVisible")] = false;
        metrics[QStringLiteral("scanStayReason")] = 0;
        metrics[QStringLiteral("scanStayPhrase")] = QString();
        metrics[QStringLiteral("scanTimerLive")] = false;
        metrics[QStringLiteral("scanTimerRemainingDs")] = 0;
        metrics[QStringLiteral("scanTimerSpanMs")] = 0;
        metrics[QStringLiteral("scanDwellMs")] = 0;
        metrics[QStringLiteral("scanDwellState")] = 0;
        metrics[QStringLiteral("scanHoldMs")] = 0;
        metrics[QStringLiteral("scanHangMs")] = 0;
        // The per-visit cap (#507): no cap in force at rest, so nothing to print.
        metrics[QStringLiteral("scanVisitMs")] = 0;
        metrics[QStringLiteral("scanVisitLive")] = false;
        metrics[QStringLiteral("scanVisitRemainingDs")] = 0;
        // The received sub-audible tone (#522): no analog FM monitor at rest, so the row is
        // down. The configured policy is its own reading and says "off" with no policy set.
        metrics[QStringLiteral("rxToneVisible")] = false;
        metrics[QStringLiteral("rxToneStatus")] = 0;
        metrics[QStringLiteral("rxToneText")] = QString();
        metrics[QStringLiteral("rxToneKind")] = 0;
        metrics[QStringLiteral("rxToneTenthsHz")] = 0;
        metrics[QStringLiteral("rxToneDcsCode")] = 0;
        metrics[QStringLiteral("rxToneDcsInverted")] = false;
        metrics[QStringLiteral("rxToneDcsAliasCode")] = 0;
        metrics[QStringLiteral("rxToneDcsAliasInverted")] = false;
        metrics[QStringLiteral("rxToneCarrier")] = false;
        metrics[QStringLiteral("rxToneConfiguredText")] = QStringLiteral("off");
        // The Tone filter row (#527): hidden with no policy in force, and no verdict.
        metrics[QStringLiteral("receiveStatus")] = QString();
        metrics[QStringLiteral("toneFilterVisible")] = false;
        metrics[QStringLiteral("toneFilterGate")] = 0;
        metrics[QStringLiteral("toneFilterStatusText")] = QString();
        // Its live editor: unreachable with no detection running, opening on no policy, and no scan row shadowing it.
        metrics[QStringLiteral("toneFilterEditable")] = false;
        metrics[QStringLiteral("toneFilterRowOverride")] = false;
        metrics[QStringLiteral("toneFilterConfiguredMode")] = 0;
        metrics[QStringLiteral("toneFilterConfiguredList")] = QString();
        // No scan row on air for the "this channel" editors (#518).
        metrics[QStringLiteral("scanRowActive")] = false;
        metrics[QStringLiteral("scanRowLabel")] = QString();
        metrics[QStringLiteral("scanRowKey")] = QString();
        metrics[QStringLiteral("scanRowEditable")] = 0;
        metrics[QStringLiteral("scanRowEdited")] = 0;
        metrics[QStringLiteral("scanRowListed")] = 0;
        metrics[QStringLiteral("scanRowSynced")] = true;
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
        metrics[QStringLiteral("configuredTunerGainDb")] = 30;
        metrics[QStringLiteral("tunerGainRowOverride")] = false;
        metrics[QStringLiteral("squelchDb")] = -120.0;
        metrics[QStringLiteral("squelchOff")] = false;
        // #521: a scan row's own squelch. 0 is off in these two, as in rtl_sql.
        metrics[QStringLiteral("configuredSquelchDb")] = -120.0;
        metrics[QStringLiteral("effectiveSquelchDb")] = -120.0;
        metrics[QStringLiteral("configuredSquelchOff")] = false;
        metrics[QStringLiteral("effectiveSquelchOff")] = false;
        metrics[QStringLiteral("squelchRowOverride")] = false;
        metrics[QStringLiteral("squelchReadout")] = QStringLiteral("-120.0 dB");
        // The dynamic squelches: whether each setting is AUTO or NOISE, its margin (kept under a level; 0 here, which
        // the sheet reads as the default), whether the default may be NOISE, and the status of the one in force (""
        // under a level).
        metrics[QStringLiteral("configuredSquelchAuto")] = false;
        metrics[QStringLiteral("configuredSquelchNoise")] = false;
        metrics[QStringLiteral("effectiveSquelchNoise")] = false;
        metrics[QStringLiteral("squelchNoiseOffered")] = true;
        metrics[QStringLiteral("configuredSquelchMarginDb")] = 0;
        metrics[QStringLiteral("effectiveSquelchAuto")] = false;
        metrics[QStringLiteral("effectiveSquelchMarginDb")] = 0;
        metrics[QStringLiteral("squelchAutoStatus")] = QString();
        metrics[QStringLiteral("squelchGateOpen")] = false;
        metrics[QStringLiteral("configuredSquelchLevelOff")] = false;
        // #628: whether the session runs on audio input, where the monitor's Squelch row edits the squelch.
        metrics[QStringLiteral("squelchAudioInput")] = false;
        // #525: the analog channel width in force (0 outside the analog preset and on PCM input),
        // whether the DSP rate bounds it, the configured width the control edits (0 = default),
        // the widest width the running stream's DSP rate filters (0 = not known), and the
        // reading as app_control's analog width view spells it.
        metrics[QStringLiteral("analogBandwidthHz")] = 0;
        metrics[QStringLiteral("analogBandwidthDspLimited")] = false;
        metrics[QStringLiteral("analogBandwidthConfiguredHz")] = 0;
        metrics[QStringLiteral("analogBandwidthSettingHz")] = 0;
        // #524: the configured NFM and AM widths whichever preset runs (0 = default), for the
        // width of the analog kind the configured preset does not run.
        metrics[QStringLiteral("nfmBandwidthConfiguredHz")] = 0;
        metrics[QStringLiteral("amBandwidthConfiguredHz")] = 0;
        // #524: whether the engine offers each kind's width for editing (dsd_app_analog_width_offered()).
        metrics[QStringLiteral("nfmBandwidthOffered")] = false;
        metrics[QStringLiteral("amBandwidthOffered")] = false;
        metrics[QStringLiteral("analogBandwidthMaxHz")] = 0;
        metrics[QStringLiteral("analogBandwidthReading")] = QString();
        // #526: an nfm scan row on air (its width is in force on any session), and whether it sets its own.
        metrics[QStringLiteral("analogBandwidthRowActive")] = false;
        metrics[QStringLiteral("analogBandwidthRowOverride")] = false;
        // #524: whether the width the analogBandwidth* keys describe is AM's (the preset's kind, or a row's on air).
        metrics[QStringLiteral("analogBandwidthAm")] = false;
        // #621: audio input with a rigctl peer that demodulates it, where the width is the passband the peer is asked
        // for, and what an unset width of each kind stands for ("-B 12.5 kHz" or "peer's own" for NFM on a peer
        // session; "default", 16 kHz NFM and 6 kHz AM elsewhere).
        metrics[QStringLiteral("peerPassband")] = false;
        // #621: whether the reading's kind runs now on a peer session (not while a typed digital row keeps it at -B).
        metrics[QStringLiteral("passbandInForce")] = false;
        metrics[QStringLiteral("nfmBandwidthUnsetText")] = QStringLiteral("default");
        metrics[QStringLiteral("amBandwidthUnsetText")] = QStringLiteral("default");
        metrics[QStringLiteral("nfmBandwidthUnsetHz")] = 16000;
        metrics[QStringLiteral("amBandwidthUnsetHz")] = 6000;
        metrics[QStringLiteral("ppm")] = 0;
        // The TETRA rows (app_control/tetra_view.h): hidden at rest, until the TETRA preset has decoded a block.
        metrics[QStringLiteral("tetraVisible")] = false;
        metrics[QStringLiteral("tetraModeText")] = QString();
        metrics[QStringLiteral("tetraEncrypted")] = false;
        metrics[QStringLiteral("tetraNetworkText")] = QString();
        metrics[QStringLiteral("tetraSecurityText")] = QString();
        metrics[QStringLiteral("tetraTrunkText")] = QString();
        metrics[QStringLiteral("tetraOnTraffic")] = false;
        metrics[QStringLiteral("tetraControlText")] = QString();
        metrics[QStringLiteral("tetraTrafficText")] = QString();
        metrics[QStringLiteral("tetraMmStatusText")] = QString();
        metrics[QStringLiteral("tetraCodecText")] = QString();
        metrics[QStringLiteral("tetraCodecFault")] = false;
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
        host[QStringLiteral("notificationPermissionNeeded")] = false;
        host[QStringLiteral("usesPlatformFontScaling")] = false;
        /* Why the last session stopped, empty while nothing has failed. */
        host[QStringLiteral("failureText")] = QString();
        /* The Android-only capabilities the settings screen hides rows on: a
         * desktop host brokers no USB device and applies no screen policy,
         * which is the arrangement this offscreen run matches. */
        host[QStringLiteral("screenPolicySupported")] = false;
        host[QStringLiteral("localDeviceBrokered")] = false;
        host[QStringLiteral("localDeviceSource")] = QStringLiteral("usb");
        /* Nothing plugged in, which is also what a desktop host answers. */
        host[QStringLiteral("attachedDeviceSource")] = QString();
        host[QStringLiteral("localDeviceSerial")] = QString();
        host[QStringLiteral("localDeviceReady")] = false;
        host[QStringLiteral("localDeviceStatus")] = QString();
        host[QStringLiteral("locationSupported")] = false;
        host[QStringLiteral("shareSupported")] = false;
        /* Issue #829: unlike the base host's, true, so the screens take the app's picker, which a case answers
         * through testContext; setDocumentPickerSupported(false) puts Qt's FileDialog back. */
        host[QStringLiteral("documentPickerSupported")] = true;
        /* Issue #831: no picker request in flight and no incoming document; a case drives both through testContext. */
        host[QStringLiteral("documentRequestPending")] = false;
        host[QStringLiteral("incomingDocumentPending")] = false;
        /* Issue #830: no file browser, as the base host; setFileBrowserSupported(true) gives the screens one. Its
         * access reading is an API 36 sideload's before the grant (ImportOnlyHost::fileAccessReading()). */
        host[QStringLiteral("fileBrowserSupported")] = false;
        host[QStringLiteral("fileAccess")] = ImportOnlyHost::fileAccessReading();
        host[QStringLiteral("localDeviceFailureKind")] = 0;
        host[QStringLiteral("localDeviceRequestPending")] = false;
        host[QStringLiteral("localDeviceRequestedSource")] = QString();
        host[QStringLiteral("receiverHealth")] = dsd_qt::DecoderHost::healthyReceiver();
        host[QStringLiteral("sessionState")] = 0;
        /* As every desktop host: the state above is the decoder's own; a case clears it through testContext. */
        host[QStringLiteral("sessionStateKnown")] = true;
        host[QStringLiteral("keyboardTop")] = -1;
        /* Issue #708: no player and no zip, as the base host; a case scripts the playback and transfer states. */
        host[QStringLiteral("playbackSupported")] = false;
        host[QStringLiteral("playbackPath")] = QString();
        host[QStringLiteral("playbackState")] = 0;
        host[QStringLiteral("playbackPositionMs")] = 0;
        host[QStringLiteral("playbackDurationMs")] = 0;
        host[QStringLiteral("playbackError")] = QString();
        host[QStringLiteral("recordingExportSupported")] = false;
        host[QStringLiteral("recordingTransferActive")] = false;
        host[QStringLiteral("recordingTransferBytes")] = 0;
        host[QStringLiteral("recordingTransferTotalBytes")] = 0;
        m_host = host;
        m_host_readings = new ReadingMap(engine);
        for (auto it = host.cbegin(); it != host.cend(); ++it) {
            m_host_readings->insert(it.key(), it.value());
        }
        ctx->setContextProperty(QStringLiteral("decoderHost"), m_host_readings);

        /* Issue #708: RecordingStatus at rest, a decoder run that records nothing and has storage; a case sets the
         * readings it needs (an applying switch, a refusal, failures). */
        QVariantMap recording;
        recording[QStringLiteral("published")] = true;
        recording[QStringLiteral("perCall")] = false;
        recording[QStringLiteral("slotFiles")] = 0;
        recording[QStringLiteral("unknownOnly")] = false;
        recording[QStringLiteral("analogMaxGapMs")] = 200;
        recording[QStringLiteral("analogPrerollMs")] = DSD_ANALOG_RECORD_PREROLL_DEFAULT_MS;
        recording[QStringLiteral("staticWav")] = false;
        recording[QStringLiteral("storageNamed")] = true;
        recording[QStringLiteral("journal")] = QStringLiteral("on");
        recording[QStringLiteral("command")] = QString();
        recording[QStringLiteral("commandState")] = QStringLiteral("none");
        recording[QStringLiteral("failures")] = 0;
        recording[QStringLiteral("failureCounts")] = QVariantMap();
        recording[QStringLiteral("lastError")] = QString();
        recording[QStringLiteral("refusal")] = QString();
        recording[QStringLiteral("refusalCount")] = 0;
        m_recording_status = recording;
        m_recording_readings = new ReadingMap(engine);
        for (auto it = recording.cbegin(); it != recording.cend(); ++it) {
            m_recording_readings->insert(it.key(), it.value());
        }
        ctx->setContextProperty(QStringLiteral("recordingStatus"), m_recording_readings);
        /* Issue #708: the recording store, empty and indexed; a case writes the readings it needs. */
        m_recordings = new RecordingsFake(engine);
        ctx->setContextProperty(QStringLiteral("recordings"), m_recordings);

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
        /* The account check, at rest: no session password, nothing checked (AccountNone). */
        rr[QStringLiteral("hasPassword")] = false;
        rr[QStringLiteral("accountState")] = 0;
        rr[QStringLiteral("accountMessage")] = QString();
        rr[QStringLiteral("checkingAccount")] = false;
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
        /* The password-manager relay the sign-in forms drive; see SignInAutofillRecorder. */
        ctx->setContextProperty(QStringLiteral("signInAutofill"), new SignInAutofillRecorder(engine));

        /* The real SpectrumModel over the canned getter in qml_spectrum_stub.cpp:
         * the polling, viewport and tap-snapping under test are the production
         * ones, only the frames are synthetic. */
        m_spectrum = new dsd_qt::SpectrumModel(engine);
        ctx->setContextProperty(QStringLiteral("spectrum"), m_spectrum);

        m_commands = new CommandRecorder();
        m_commands->setParent(engine);
        m_commands->recordingStatus = m_recording_readings;
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
        m_saved_systems = saved_systems;
        auto* app_prefs = new dsd_qt::AppPrefs(engine);
        m_app_prefs = app_prefs;
        auto* session_args = new dsd_qt::SessionArgsBuilder(app_prefs, engine);
        session_args->setSavedSystems(saved_systems);
        ctx->setContextProperty(QStringLiteral("importedFiles"), imported_files);
        m_imported_files = imported_files;
        m_replay_files = new ReplayFilesFake(engine);
        ctx->setContextProperty(QStringLiteral("replayFiles"), m_replay_files);
        m_network = new dsd_qt::P25NetworkModel(engine);
        ctx->setContextProperty(QStringLiteral("p25Network"), m_network); // WP-F2
        // Issue #782: the production state, following QAccessible (on in the DSD_QML_TEST_ACCESSIBILITY=1 run).
        ctx->setContextProperty(QStringLiteral("screenReader"), new dsd_qt::ScreenReaderState(engine));
        ctx->setContextProperty(QStringLiteral("diagnosticsLog"), new dsd_qt::DiagnosticsLogModel(nullptr, engine));
        auto* controller = new TestUiController(engine);
        auto* scan_lists = new dsd_qt::ScanListsModel(engine);
        // WP-S2: use the production pure policy for host attachment delivery in QML tests.
        // The real controller's emission/validation contract is covered by UI_QT_CONTROLLER; the #647 D3a kind rule
        // and the reading of the "<kind>:<device name>" attachment are production's own helpers.
        for (ImportOnlyHost* attachmentHost : {m_import_host, static_cast<ImportOnlyHost*>(m_initializing_host)}) {
            QObject::connect(
                attachmentHost, &dsd_qt::DecoderHost::localDeviceAttached, controller, [=](const QString& attachment) {
                    const QString attachedKind = dsd_qt::attachedReceiverKind(attachment);
                    const QString kind = app_prefs->lastStartedKind();
                    const QString uid = app_prefs->lastStartedUid();
                    const QVariantMap target = kind == QStringLiteral("saved")  ? saved_systems->getByUid(uid)
                                               : kind == QStringLiteral("scan") ? scan_lists->getByUid(uid)
                                                                                : QVariantMap();
                    const bool kindMatches = dsd_qt::autoStartKindMatches(
                        target.value(QStringLiteral("sourceType")).toString(), attachedKind);
                    if (dsd_qt::autoStartAllowed(app_prefs->autoStartOnAttach(), app_prefs->onboardingDone(),
                                                 attachmentHost->sessionState(), controller->autoStartBlocked,
                                                 !target.isEmpty(), kindMatches)) {
                        controller->requestAutoStart(kind, uid,
                                                     kind == QStringLiteral("scan") ? app_prefs->lastStartedCompanions()
                                                                                    : QStringList());
                    }
                });
        }
        ctx->setContextProperty(QStringLiteral("uiController"), controller);
        ctx->setContextProperty(QStringLiteral("savedSystems"), saved_systems);
        ctx->setContextProperty(QStringLiteral("scanLists"), scan_lists);
        // Issue #644: the production index over them, so Main offers what a case saves on the tuned channel.
        ctx->setContextProperty(QStringLiteral("knownFrequencies"),
                                new dsd_qt::KnownFrequencyIndex(saved_systems, scan_lists, engine));
        /* Issue #801: the production pins over the real AppPrefs (the settings file every case shares, whichever
         * `prefs` a case reads), cleared for each test file; resetPins() clears them between cases. */
        app_prefs->setPinned({});
        /* Issue #646: the Listen tab too, on the same shared file; resetListenSection() between cases. */
        app_prefs->setListenSection(QStringLiteral("systems"));
        app_prefs->setSystemsSort(QStringLiteral("added"));
        ctx->setContextProperty(QStringLiteral("listenPins"),
                                new dsd_qt::ListenPins(app_prefs, saved_systems, scan_lists, engine));
        /* Listen's saved-system cards, the production model over the same stores; resetSavedSystemCards() between
         * cases. Nearest follows setLocationSupported(), off as the host map starts. */
        m_saved_system_cards = new dsd_qt::SavedSystemCards(saved_systems, app_prefs, imported_files, engine);
        ctx->setContextProperty(QStringLiteral("savedSystemCards"), m_saved_system_cards);
        auto* profiles = new dsd_qt::DecryptionProfilesModel(engine);
        profiles->setReferences(saved_systems, scan_lists);
        auto* starter = new dsd_qt::ScanListStarter(app_prefs, saved_systems, engine);
        scan_lists->setAcceptedRowsLookup([imported_files](const QString& path) {
            const int row = imported_files->rowForPath(path);
            return row >= 0 ? imported_files->get(row).value(QStringLiteral("accepted")).toInt() : -1;
        });
        starter->setTargetFileLookup([imported_files](const QString& path) {
            const int row = imported_files->rowForPath(path);
            return row >= 0 && imported_files->get(row).value("type") == "trunkTargets";
        });
        // As qt_ui.cpp's wire_library_replace(), an update lets the saved systems naming the file go of their site.
        imported_files->setBeforeUserReplace(
            [saved_systems](const QString& path) { return saved_systems->invalidatePathProvenance(path); });
        starter->setDecryptionProfiles(profiles);
        session_args->setDecryptionProfiles(profiles);
        ctx->setContextProperty(QStringLiteral("decryptionProfiles"), profiles);
        ctx->setContextProperty(QStringLiteral("scanListStarter"), starter);
        ctx->setContextProperty(QStringLiteral("sessionArgs"), session_args);
        ctx->setContextProperty(QStringLiteral("appVersionText"), QStringLiteral("0.0.0-test"));
        // Issue #580: the backup model's surface, recorded rather than run.
        m_backup = new BackupRecorder(engine);
        m_backup->setHost(m_lifecycle_host);
        ctx->setContextProperty(QStringLiteral("backup"), m_backup);
        // Issue #830: the close before a restart, scripted (setCloseRefused(), finishClose()).
        m_restart_closer = new RestartCloserFake(engine);
        ctx->setContextProperty(QStringLiteral("restartCloser"), m_restart_closer);
        // RadioReference place names and Update from RadioReference, recorded (RrPlacesFake).
        m_rr_places = new RrPlacesFake(engine);
        ctx->setContextProperty(QStringLiteral("rrPlaces"), m_rr_places);
        // Issue #718: the production composing-text commit, on its fallback path here.
        ctx->setContextProperty(QStringLiteral("inputCommit"), new dsd_qt::InputCommit(engine));
        // The production name order and fold (ICU-free), as the RadioReference lists use it.
        ctx->setContextProperty(QStringLiteral("textOrder"), new dsd_qt::TextOrder(engine));
        // Issue #828: the production list, which outlives every engine here as it does in the app.
        ctx->setContextProperty(QStringLiteral("storeNotices"), dsd_qt::StoreNotices::instance());
    }

  private:
    /* Whether `decoderHost` is the readings map now, rather than a lifecycle host useLifecycleHost() put in. */
    bool
    hostReadingsInstalled() const {
        return m_engine->rootContext()->contextProperty(QStringLiteral("decoderHost")).value<QObject*>()
               == m_host_readings;
    }

    /* Issue #782: set when applicationAvailable() turned accessibility on, so teardown turns it off again. */
    bool m_accessibility_activated = false;
    /* Issue #782: the URLs QML opened while captureOpenedUrls() held the handler. */
    QStringList m_opened_urls;
    dsd_qt::AppPrefs* m_app_prefs = nullptr;
    ReadingMap* m_metric_readings = nullptr;
    ReadingMap* m_host_readings = nullptr;
    /* Issue #708: the `recordingStatus` QML reads, its readings at rest, and the `recordings` store. */
    ReadingMap* m_recording_readings = nullptr;
    QVariantMap m_recording_status;
    RecordingsFake* m_recordings = nullptr;
    QVariantMap m_metrics;
    QVariantMap m_prefs;
    QVariantMap m_host;
    QVariantMap m_radio_reference;
    RadioReferenceRecorder* m_radio_reference_recorder = nullptr;
    BackupRecorder* m_backup = nullptr;
    RestartCloserFake* m_restart_closer = nullptr;
    RrPlacesFake* m_rr_places = nullptr;
    QQmlEngine* m_engine = nullptr;
    dsd_qt::SpectrumModel* m_spectrum = nullptr;
    /* Issue #672: the Monitor's avoid list, the production model, filled by setScanAvoids(). */
    dsd_qt::ScanAvoidListModel* m_scan_avoid_model = nullptr;
    /* Issue #697: the Scan lists sheet's model, the production one, over a scripted app-control side: requests from
       101, every switch kept, answers queued by answerScanList(). */
    dsd_qt::ScanListToggleModel* m_scan_list_model = nullptr;
    dsd_app_scan_list_view m_scan_list_view{};
    QList<dsd_app_scan_list_set_payload> m_scan_list_sent;
    QList<dsd_app_scan_list_result> m_scan_list_results;
    uint64_t m_scan_list_latest = 0;
    uint32_t m_scan_list_next_request = 101;
    bool m_scan_list_queue_full = false;

    dsd_qt::ScanListToggleModel::Backend
    scanListBackend() {
        dsd_qt::ScanListToggleModel::Backend backend;
        backend.requestNext = [this] { return m_scan_list_next_request++; };
        backend.submit = [this](const dsd_app_scan_list_set_payload& payload) {
            if (m_scan_list_queue_full) {
                return static_cast<int>(DSD_APP_COMMAND_SUBMIT_REJECTED);
            }
            m_scan_list_sent.append(payload);
            return static_cast<int>(DSD_APP_COMMAND_SUBMIT_QUEUED);
        };
        backend.resultsSince = [this](uint64_t after, dsd_app_scan_list_result* out, int cap) {
            int n = 0;
            for (const auto& result : m_scan_list_results) {
                if (result.sequence > after && n < cap) {
                    out[n++] = result;
                }
            }
            return n;
        };
        backend.latestSequence = [this] { return m_scan_list_latest; };
        return backend;
    }

    CommandRecorder* m_commands = nullptr;
    ImportOnlyHost* m_import_host = nullptr;
    dsd_qt::ImportedFilesModel* m_imported_files = nullptr;
    dsd_qt::SavedSystemsModel* m_saved_systems = nullptr;
    dsd_qt::SavedSystemCards* m_saved_system_cards = nullptr;
    ReplayFilesFake* m_replay_files = nullptr;
    ImportOnlyHost* m_initializing_host = nullptr;
    ImportOnlyHost* m_lifecycle_host = nullptr;
    std::unique_ptr<dsd_opts> m_talkgroup_opts = std::make_unique<dsd_opts>();
    std::unique_ptr<dsd_state> m_talkgroup_state = std::make_unique<dsd_state>();
    /* Issue #782: two scan targets' policy tables (applyTalkgroupTable()), built on first use. */
    std::array<std::unique_ptr<dsd_state>, 2> m_target_tables;
    /* The target table on show, or null for m_talkgroup_state. */
    const dsd_state* m_applied_table = nullptr;
    dsd_qt::TalkgroupListModel* m_talkgroups = nullptr;
    dsd_qt::P25NetworkModel* m_network = nullptr;
    /* Issue #782: the snapshot hopNetwork() publishes, built on first use. */
    std::unique_ptr<dsd_state> m_network_state;

    struct TableRow {
        uint32_t id;
        const char* mode;
        const char* name;
        const char* tags;
    };

    /* Target 0 (A) and target 1 (B): no row bounds and no category in common. */
    dsd_state*
    targetTable(size_t target) {
        if (m_target_tables[target]) {
            return m_target_tables[target].get();
        }
        static const TableRow kTableA[] = {
            {1001, "A", "Fire Dispatch", "FIRE"},
            {1002, "B", "Fire Tactical", "FIRE"},
            {2001, "A", "EMS Dispatch", "EMS"},
        };
        static const TableRow kTableB[] = {
            {3001, "A", "Police Dispatch", "POLICE"},
            {3002, "B", "Police Tactical", "POLICE"},
            {4001, "A", "Public Works", "WORKS"},
            {4002, "B", "Water Department", "WORKS"},
        };
        const TableRow* rows = target == 0 ? kTableA : kTableB;
        const size_t count = target == 0 ? std::size(kTableA) : std::size(kTableB);
        auto table = std::make_unique<dsd_state>();
        for (size_t i = 0; i < count; ++i) {
            dsd_tg_policy_entry entry{};
            if (dsd_tg_policy_make_exact_entry(rows[i].id, rows[i].mode, rows[i].name, DSD_TG_POLICY_SOURCE_IMPORTED,
                                               &entry)
                    != 0
                || DSD_SNPRINTF(entry.tags, sizeof(entry.tags), "%s", rows[i].tags) < 0
                || dsd_tg_policy_append_exact(table.get(), &entry) != 0) {
                dsd_state_ext_free_all(table.get());
                return nullptr;
            }
        }
        m_target_tables[target] = std::move(table);
        return m_target_tables[target].get();
    }
};

#endif /* DSD_NEO_TESTS_UI_QML_TEST_CONTEXT_H_ */
