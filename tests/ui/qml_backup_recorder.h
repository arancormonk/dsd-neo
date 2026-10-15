// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Recording stand-in for BackupModel (issue #580), the `backup` the QML tests run against.
 *
 * Its own header, on QtCore and DecoderHost only, so UI_QT_BACKUP_MODEL can hold its meta-object against the real
 * model's: a property, invokable or signal the model gains or renames fails there before a screen can bind to a name
 * this fake lacks.
 */

#ifndef DSD_NEO_TESTS_UI_QML_BACKUP_RECORDER_H_
#define DSD_NEO_TESTS_UI_QML_BACKUP_RECORDER_H_

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "decoder_host.h"

/**
 * @brief Stand-in for BackupModel that records instead of working.
 *
 * Mirrors the QML-facing surface of src/ui/qt/backup_model.h: the same read-only properties, invokables and result
 * signals. A case drives it through `testContext` (setBackup(), setBackupReply(), emitBackup()) rather than by writing
 * the properties, which stay read-only as they are in production, so a screen that wrote one would fail here as it
 * would on the phone.
 *
 * Only the state changes the screens rely on are mimicked:
 * - `restoreAllowed` is the model's rule, no job running and no session on the host (setHost());
 * - a job sets `busy`, and its result clears it before the signal;
 * - a successful commit leaves `restorePending`, as a committed restore does;
 * - a discard that leaves no restore to apply clears it, and a deferred card with it;
 * - acknowledging clears the startup outcome.
 */
class BackupRecorder : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool flowActive READ flowActive NOTIFY flowActiveChanged)
    Q_PROPERTY(bool restoreAllowed READ restoreAllowed NOTIFY restoreAllowedChanged)
    Q_PROPERTY(bool restorePending READ restorePending NOTIFY restorePendingChanged)
    Q_PROPERTY(QVariantMap startupOutcome READ startupOutcome NOTIFY startupOutcomeChanged)
    Q_PROPERTY(int minPassphraseLength READ minPassphraseLength CONSTANT)
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY lastResultChanged)

  public:
    explicit BackupRecorder(QObject* parent) : QObject(parent) { reset(); }

    bool
    busy() const {
        return m_busy;
    }

    bool
    flowActive() const {
        return m_flow_active;
    }

    bool
    restoreAllowed() const {
        return !m_busy && (m_host.isNull() || !m_host->sessionActive());
    }

    bool
    restorePending() const {
        return m_restore_pending;
    }

    QVariantMap
    startupOutcome() const {
        return m_startup_outcome;
    }

    static int
    minPassphraseLength() {
        return 8;
    }

    QVariantMap
    lastResult() const {
        return m_last_result;
    }

    /* A fixed name: the real one stamps today's date, which a case cannot pin. */
    Q_INVOKABLE QString
    // cppcheck-suppress functionStatic // Qt meta-object entry point must remain an instance method.
    suggestedFileName() const {
        return QStringLiteral("dsd-neo-backup-2026-10-14.dsdbak");
    }

    Q_INVOKABLE void
    prepareExport(const QString& passphrase) {
        record(QStringLiteral("prepareExport"), {passphrase});
        setBusy(true);
    }

    Q_INVOKABLE void
    saveExport(const QString& reference) {
        record(QStringLiteral("saveExport"), {reference});
        setBusy(true);
    }

    Q_INVOKABLE void
    discardExport() {
        record(QStringLiteral("discardExport"), {});
    }

    Q_INVOKABLE void
    inspectRestore(const QString& reference, const QString& passphrase) {
        record(QStringLiteral("inspectRestore"), {reference, passphrase});
        setBusy(true);
    }

    Q_INVOKABLE QVariantMap
    commitRestore() {
        record(QStringLiteral("commitRestore"), {});
        const QVariantMap reply = m_replies.value(QStringLiteral("commitRestore")).toMap();
        if (reply.value(QStringLiteral("ok")).toBool()) {
            setState(QStringLiteral("restorePending"), true);
        }
        return reply;
    }

    Q_INVOKABLE QVariantMap
    discardRestore() {
        record(QStringLiteral("discardRestore"), {});
        return applyDiscard(QStringLiteral("discardRestore"),
                            m_startup_outcome.value(QStringLiteral("kind")).toString() == QStringLiteral("deferred"));
    }

    Q_INVOKABLE QVariantMap
    discardDeferredRestore() {
        record(QStringLiteral("discardDeferredRestore"), {});
        return applyDiscard(QStringLiteral("discardDeferredRestore"), true);
    }

    Q_INVOKABLE void
    cancel() {
        record(QStringLiteral("cancel"), {});
        setBusy(false);
    }

    Q_INVOKABLE void
    setFlowActive(bool active) {
        record(QStringLiteral("setFlowActive"), {active});
        setState(QStringLiteral("flowActive"), active);
    }

    Q_INVOKABLE void
    acknowledgeStartupOutcome() {
        record(QStringLiteral("acknowledgeStartupOutcome"), {});
        setState(QStringLiteral("startupOutcome"), QVariantMap{{QStringLiteral("kind"), QString()}});
    }

    /** @brief The host whose session gates a restore, as the model's is; null allows it. */
    void
    setHost(dsd_qt::DecoderHost* host) {
        QObject::disconnect(m_session);
        m_host = host;
        if (host != nullptr) {
            m_session = QObject::connect(host, &dsd_qt::DecoderHost::sessionStateChanged, this,
                                         &BackupRecorder::updateRestoreAllowed);
        }
        updateRestoreAllowed();
    }

    /** @brief Every call so far, oldest first, as `{name, args}` maps. */
    QVariantList
    calls() const {
        return m_calls;
    }

    /** @brief Back to a fresh model: idle, nothing pending, no outcome, no calls, default replies. */
    void
    reset() {
        m_calls.clear();
        const QVariantMap commit_reply{
            {QStringLiteral("ok"), true}, {QStringLiteral("quitting"), false}, {QStringLiteral("error"), QString()}};
        const QVariantMap discard_reply{
            {QStringLiteral("ok"), true}, {QStringLiteral("discarded"), true}, {QStringLiteral("error"), QString()}};
        m_replies = {{QStringLiteral("commitRestore"), commit_reply},
                     {QStringLiteral("discardRestore"), discard_reply},
                     {QStringLiteral("discardDeferredRestore"), discard_reply}};
        setState(QStringLiteral("busy"), false);
        setState(QStringLiteral("flowActive"), false);
        setState(QStringLiteral("restorePending"), false);
        setState(QStringLiteral("startupOutcome"), QVariantMap{{QStringLiteral("kind"), QString()}});
        setState(QStringLiteral("lastResult"), QVariantMap());
    }

    /**
     * @brief Set one property by name, announcing a change: busy, flowActive, restorePending, startupOutcome or
     * lastResult. False for any other name; restoreAllowed follows busy and the host's session.
     */
    bool
    setState(const QString& key, const QVariant& value) {
        if (key == QStringLiteral("busy")) {
            setBusy(value.toBool());
            return true;
        }
        if (key == QStringLiteral("flowActive")) {
            assign(m_flow_active, value.toBool(), &BackupRecorder::flowActiveChanged);
            return true;
        }
        if (key == QStringLiteral("restorePending")) {
            assign(m_restore_pending, value.toBool(), &BackupRecorder::restorePendingChanged);
            return true;
        }
        if (key == QStringLiteral("startupOutcome")) {
            m_startup_outcome = value.toMap();
            Q_EMIT startupOutcomeChanged();
            return true;
        }
        if (key == QStringLiteral("lastResult")) {
            m_last_result = value.toMap();
            Q_EMIT lastResultChanged();
            return true;
        }
        return false;
    }

    /** @brief What commitRestore(), discardRestore() or discardDeferredRestore() answers next. */
    void
    setReply(const QString& call, const QVariantMap& reply) {
        m_replies.insert(call, reply);
    }

    /** @brief Deliver one result signal the way the model does: idle first, then the signal. */
    bool
    emitResult(const QString& signal, const QVariantMap& result) {
        setBusy(false);
        if (signal == QStringLiteral("exportPrepared")) {
            Q_EMIT exportPrepared(result);
        } else if (signal == QStringLiteral("exportFinished")) {
            Q_EMIT exportFinished(result);
        } else if (signal == QStringLiteral("inspectFinished")) {
            Q_EMIT inspectFinished(result);
        } else {
            return false;
        }
        return true;
    }

  Q_SIGNALS:
    void busyChanged();
    void flowActiveChanged();
    void restoreAllowedChanged();
    void restorePendingChanged();
    void startupOutcomeChanged();
    void lastResultChanged();
    void exportPrepared(const QVariantMap& result);
    void exportFinished(const QVariantMap& result);
    void inspectFinished(const QVariantMap& result);

  private:
    void
    record(const QString& name, const QVariantList& args) {
        m_calls.append(QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("args"), args}});
    }

    void
    setBusy(bool busy) {
        assign(m_busy, busy, &BackupRecorder::busyChanged);
        updateRestoreAllowed();
    }

    void
    updateRestoreAllowed() {
        const bool allowed = restoreAllowed();
        if (allowed != m_restore_allowed) {
            m_restore_allowed = allowed;
            Q_EMIT restoreAllowedChanged();
        }
    }

    void
    assign(bool& field, bool value, void (BackupRecorder::*changed)()) {
        if (field != value) {
            field = value;
            Q_EMIT(this->*changed)();
        }
    }

    /* The model's discard: when the restore will never apply, nothing is pending and a deferred card goes. */
    QVariantMap
    applyDiscard(const QString& call, bool clearsOutcome) {
        const QVariantMap reply = m_replies.value(call).toMap();
        if (reply.value(QStringLiteral("discarded")).toBool()) {
            setState(QStringLiteral("restorePending"), false);
            if (clearsOutcome) {
                setState(QStringLiteral("startupOutcome"), QVariantMap{{QStringLiteral("kind"), QString()}});
            }
        }
        return reply;
    }

    bool m_busy = false;
    bool m_flow_active = false;
    /* The last restoreAllowed() announced, so a change is signalled once. */
    bool m_restore_allowed = true;
    bool m_restore_pending = false;
    QPointer<dsd_qt::DecoderHost> m_host;
    QMetaObject::Connection m_session;
    QVariantMap m_startup_outcome;
    QVariantMap m_last_result;
    QVariantMap m_replies;
    QVariantList m_calls;
};

#endif /* DSD_NEO_TESTS_UI_QML_BACKUP_RECORDER_H_ */
