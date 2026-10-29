// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The backup screens' model (issue #580): export, restore inspection and commit, and the startup outcome.
 *
 * QML reaches it as the context property `backup`. Every step that reads a document, derives a key or writes more
 * than a marker runs on one low-priority worker thread, one job at a time:
 *
 *  - Export: prepareExport() reads the configuration on the UI thread, then the worker builds and seals the backup
 *    into `<CacheLocation>/backup/<job>.dsdbak` (exportPrepared). The save picker opens only after that, and
 *    saveExport() copies the file into the chosen document and removes it (exportFinished). discardExport() drops it.
 *  - Restore: inspectRestore() copies the picked document into the job's own cache file, opens, validates and stages
 *    it into `restore-staging-<job>/`; back on the UI thread the stage becomes `restore-pending/` (inspectFinished).
 *    commitRestore() marks it confirmed and asks the host to quit, so it applies at the next start.
 *
 * cancel() drops the running job: its result is never reported and its side effects stay inside its own cache file or
 * staging directory, which the model removes. Saving an export into a document is not cancellable. The model never
 * trusts a document's name: a restore identifies its files by the sealed header alone.
 *
 * Result maps carry `ok` (bool) and `error`, empty on success, else a backup_error_name() ("NotBackup",
 * "NewerVersion", "WrongPassphrase", "TooLarge", "Damaged", "NoSpace", "SessionActive", "Io", "SourceChanged": a file
 * changed while an export read it, which a session can cause, so retrying helps; "StagingFailed": a restore could not
 * be prepared on this phone, while "Io" from an inspection means the picked document could not be read) or one of the
 * model's own names: "Busy" (another backup step is running), "PassphraseTooShort" (export only) and "RestorePending"
 * (a confirmed restore already waits for the next start). Text that comes from a backup (the app version, the
 * not-included lines) is plain data for QML to show as plain text.
 *
 * One instance per process: construction removes every leftover export file and staging directory.
 */

#ifndef DSD_NEO_SRC_UI_QT_BACKUP_MODEL_H_
#define DSD_NEO_SRC_UI_QT_BACKUP_MODEL_H_

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic> // IWYU pragma: keep -- std::atomic<bool> is the cancel flag; <memory> need not declare it.
#include <deque>
#include <dsd-neo/crypto/backup_seal.h>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>    // IWYU pragma: keep
#include "backup_apply.h" // IWYU pragma: keep -- the constructor takes the startup ApplyResult.

class QRunnable;

namespace dsd_qt {

class DecoderHost;

class BackupModel : public QObject {
    Q_OBJECT
    /** A job runs on the worker (preparing or saving an export, inspecting a restore). */
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /** Any backup step is open, pickers included; Main holds automatic starts while it is set. */
    Q_PROPERTY(bool flowActive READ flowActive NOTIFY flowActiveChanged)
    /** No session is on screen and no job runs. */
    Q_PROPERTY(bool restoreAllowed READ restoreAllowed NOTIFY restoreAllowedChanged)
    /**
     * A committed restore waits for the next start: confirmed here (the app may not have closed), or left by an earlier
     * start that deferred it or could not finish it. Follows the stage on disk after construction, commits and
     * discards.
     */
    Q_PROPERTY(bool restorePending READ restorePending NOTIFY restorePendingChanged)
    /**
     * The waiting restore can still be cancelled: it is pending and its apply has not started (no complete journal).
     * Once the apply has started only the next start can finish or undo it, so no screen offers to cancel it then.
     * Follows the stage on disk like restorePending.
     */
    Q_PROPERTY(bool restoreCancellable READ restoreCancellable NOTIFY restoreCancellableChanged)
    /**
     * What a restore applied at start did (this start's apply, else the result file an earlier start left until it
     * is acknowledged): `kind` ("" | "applied" | "failed" | "deferred" | "stuck"),
     * `message` (a sentence, may be empty), and the backup's counts: `systems`, `scanLists`, `libraryFiles`,
     * `profiles`, `ranges`, `avoids`, `recordingsToChoose`, `systemsWithExtraArgs` (ints), `appExtraArgs` (bool),
     * `createdAt` (seconds since the epoch), `app` (plain text) and `notIncluded` (plain lines: what the export could
     * not carry and the library files this phone could not read).
     */
    Q_PROPERTY(QVariantMap startupOutcome READ startupOutcome NOTIFY startupOutcomeChanged)
    Q_PROPERTY(int minPassphraseLength READ minPassphraseLength CONSTANT)
    /**
     * The last export or restore-inspection result, for the Settings card: the map exportPrepared (failures only),
     * exportFinished or inspectFinished carried, plus `action` ("prepareExport", "saveExport" or "inspectRestore").
     * Empty until one arrives, and emptied when a new export or inspection starts.
     */
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY lastResultChanged)

  public:
    /**
     * @param host    Session state, quit, engine check and document copies. It must outlive the model, because the
     *                worker calls its document copies.
     * @param startup What apply_pending_restore() returned at this start.
     */
    BackupModel(DecoderHost* host, const backup::ApplyResult& startup, QObject* parent = nullptr);
    ~BackupModel() override;

    bool
    busy() const {
        return m_job.kind != JobKind::None;
    }

    bool
    flowActive() const {
        return m_flowActive;
    }

    bool restoreAllowed() const;

    bool
    restorePending() const {
        return m_restorePending;
    }

    bool
    restoreCancellable() const {
        return m_restoreCancellable;
    }

    QVariantMap
    startupOutcome() const {
        return m_startupOutcome;
    }

    int
    minPassphraseLength() const {
        return backup::kMinPassphraseChars;
    }

    QVariantMap
    lastResult() const {
        return m_lastResult;
    }

    /** `dsd-neo-backup-YYYY-MM-DD.dsdbak` for today's date. */
    Q_INVOKABLE QString suggestedFileName() const;

    /**
     * Start an export: the configuration is read now, then built and sealed under @p passphrase on the worker.
     * Reports exportPrepared, refusals included (always after this call returns). An earlier unsaved export is
     * dropped. Allowed while a session runs.
     */
    Q_INVOKABLE void prepareExport(const QString& passphrase);

    /** Copy the prepared export into the document @p reference names, then remove it. Reports exportFinished. */
    Q_INVOKABLE void saveExport(const QString& reference);

    /** Drop the prepared export (or cancel its preparation). Never interrupts a save. */
    Q_INVOKABLE void discardExport();

    /**
     * Start inspecting the document @p reference as a backup sealed under @p passphrase. Refused while a session is
     * on screen (SessionActive) or a confirmed restore waits (RestorePending). Reports inspectFinished; on success
     * the backup is staged as the pending, unconfirmed restore.
     */
    Q_INVOKABLE void inspectRestore(const QString& reference, const QString& passphrase);

    /**
     * Confirm the pending restore and ask the host to close the app. Synchronous; refused while a job runs (Busy),
     * a session is on screen or the engine is busy (SessionActive), and when nothing could be marked (Io).
     *
     * @return `{ok, quitting, error}`: `quitting` false with `ok` means the app stays open. A commit sets
     *         restorePending either way.
     */
    Q_INVOKABLE QVariantMap commitRestore();

    /**
     * Remove the pending restore (cancelling an inspection that would stage one).
     *
     * @return `{ok, discarded, error}`: `ok` when nothing is left; `discarded` when the restore will not apply. Not
     *         `ok` but `discarded`: some of its files stay until the next start removes them. Neither: nothing was
     *         removed and the restore still applies at the next start; `error` is "RestoreInProgress" when its apply
     *         has started (only the next start can finish or undo it), else "Io".
     */
    Q_INVOKABLE QVariantMap discardRestore();

    /** Drop the running job, unless it is saving an export. */
    Q_INVOKABLE void cancel();

    Q_INVOKABLE void setFlowActive(bool active);

    /** Hide the startup outcome for good: it is cleared and the result file removed. */
    Q_INVOKABLE void acknowledgeStartupOutcome();

    /**
     * discardRestore() for the deferred startup card. Any discard that leaves no restore to apply also acknowledges a
     * deferred startup outcome, so its card goes.
     */
    Q_INVOKABLE QVariantMap discardDeferredRestore();

    /** scrypt cost for new exports. Production keeps dsd_backup_kdf_params_default(); tests pass cheaper ones. */
    void setKdfParams(const dsd_backup_kdf_params& kdf);

    /**
     * Free bytes on the data root's volume, asked on the worker before staging (a negative answer means unknown).
     * Empty: dsd_path_free_bytes(). The function must be safe to call from the worker thread.
     */
    void setFreeBytes(std::function<qint64(const QString&)> freeBytes);

    /** Block until no job is left on the worker; its results stay queued for the event loop. False on timeout. */
    bool waitForJobs(int timeoutMs = -1);

  Q_SIGNALS:
    void busyChanged();
    void flowActiveChanged();
    void restoreAllowedChanged();
    void restorePendingChanged();
    void restoreCancellableChanged();
    void startupOutcomeChanged();
    void lastResultChanged();
    /** `{ok, error, summary, notIncluded}`; `summary` as summary_to_map(), `notIncluded` a list of plain lines. */
    void exportPrepared(const QVariantMap& result);
    /** `{ok, error, summary, notIncluded}`, the last two from the export that was saved. */
    void exportFinished(const QVariantMap& result);
    /**
     * `{ok, error, summary}`; `summary` holds `createdAt`, `app`, `systems`, `scanLists`, `libraryFiles`, `profiles`,
     * `ranges`, `avoids`, `recordingsToChoose`, `systemsWithExtraArgs`, `appExtraArgs` and `notIncluded`, as staged:
     * a library row this phone cannot read is left out of the counts and listed in `notIncluded`.
     */
    void inspectFinished(const QVariantMap& result);

  private Q_SLOTS:
    /** Run the replies the worker queued (UI thread). */
    void deliverReplies();

  private:
    enum class JobKind { None, Prepare, Save, Inspect };

    struct Job {
        QString id;
        JobKind kind = JobKind::None;
        /** The job's own file: the sealed export (Prepare, Save) or the staging directory (Inspect). */
        QString path;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    /** An export built and sealed into the cache, not saved yet. */
    struct PreparedExport {
        QString path;
        QVariantMap summary;
    };

    Job beginJob(JobKind kind, const QString& id, const QString& path);
    /** Mark the current prepare or inspect job dropped, without announcing it; false when there is none. */
    bool dropJob();
    /** Clear the current job and announce idle. */
    void endJob();
    /** busyChanged and restoreAllowed, once the job's outcome is in place. */
    void announceIdle();
    /** Whether @p job's result must be dropped because it is no longer the current job; its files go then. */
    bool takeDropped(const Job& job);
    static void removeJobFiles(const Job& job);
    void setLastResult(const QString& action, const QVariantMap& result);
    void clearLastResult();
    void refuse(void (BackupModel::*signal)(const QVariantMap&), const QString& action, const QString& error);
    void dropExportFile();
    void updateRestoreAllowed();
    /** restorePending and restoreCancellable from the stage on disk. */
    void syncRestorePending();
    void setStartupOutcome(const QVariantMap& outcome);

    /** Queue @p work for the worker thread. */
    void post(std::function<void()> work);
    /** Worker side: queue @p handler for the UI thread and ask the event loop to run it. */
    void reply(std::function<void()> handler);
    /** Worker side: run the oldest queued work. */
    void runNext();

    void onExportBuilt(const Job& job, const QVariantMap& result);
    void onExportSaved(const Job& job, const QVariantMap& result);
    void onInspected(const Job& job, const QVariantMap& result);

    DecoderHost* m_host;
    Job m_job;
    /** Cancelled jobs whose results have not come back yet. */
    QList<Job> m_dropped;
    PreparedExport m_export;
    bool m_flowActive = false;
    bool m_restoreAllowed = false;
    bool m_restorePending = false;
    bool m_restoreCancellable = false;
    bool m_quitting = false;
    QVariantMap m_startupOutcome;
    QVariantMap m_lastResult;
    dsd_backup_kdf_params m_kdf{};
    std::function<qint64(const QString&)> m_freeBytes;
    /*
     * The only state both threads touch. Work and replies cross through these queues under one std::mutex, so the
     * handoff does not depend on how Qt's own queues synchronize. m_self and m_runner are set before the worker
     * thread first starts and never change.
     */
    std::mutex m_handoff;
    std::deque<std::function<void()>> m_work;
    std::deque<std::function<void()>> m_replies;
    QPointer<BackupModel> m_self;
    std::unique_ptr<QRunnable> m_runner;
    QThreadPool m_pool; // last: destroyed first
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_BACKUP_MODEL_H_ */
