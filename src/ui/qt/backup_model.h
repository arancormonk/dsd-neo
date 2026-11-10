// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The backup screens' model (issue #580): export, restore inspection and commit, and the startup outcome.
 *
 * QML reaches it as the context property `backup`. Every step that derives a key or writes more than a marker runs on
 * one low-priority worker thread, one job at a time; a restore reads its document on the import worker first:
 *
 *  - Export: prepareExport() reads the configuration on the UI thread, then the worker builds and seals the backup
 *    into `<CacheLocation>/backup/<job>.dsdbak` (exportPrepared). The save picker opens only after that, and
 *    saveExport() writes the file into the chosen document through the document transport and removes it
 *    (exportFinished). discardExport() drops it.
 *  - Restore: inspectRestore() has the import worker copy the picked document into the job's own cache file
 *    (`<job>.in`, ImportService::readDocument()), then the worker opens, validates and stages it into
 *    `restore-staging-<job>/`; back on the UI thread the stage becomes `restore-pending/` (inspectFinished).
 *    commitRestore() marks it confirmed and asks the host to quit, so it applies at the next start.
 *
 * cancel() drops the running job: its result is never reported and its side effects stay inside its own cache file or
 * staging directory, which the model removes; a restore's document read is cancelled on the import worker, so the next
 * job never waits for it. Saving an export into a document is not cancellable. The model never trusts a document's
 * name: a restore identifies its files by the sealed header alone.
 *
 * The worker's jobs hold only state shared with the model (the queues, the transport, their own cancel flags), never
 * the model or the host. The destructor waits ImportService::kStopWaitMs for a job, then leaves it behind: a job that
 * ends without the model removes its own files and reports nothing.
 *
 * Result maps carry `ok` (bool) and `error`, empty on success, else a backup_error_name() ("NotBackup",
 * "NewerVersion", "WrongPassphrase", "TooLarge", "Damaged", "NoSpace", "SessionActive", "Io", "SourceChanged": a file
 * changed while an export read it, which a session can cause, so retrying helps; "StagingFailed": a restore could not
 * be prepared on this phone, while "Io" from an inspection means the picked document could not be read;
 * "Unreadable": an export found a file it carries unreadable, Snapshot::unreadable, and built nothing) or one of the
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
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic> // IWYU pragma: keep -- std::atomic<bool> is the cancel flag; <memory> need not declare it.
#include <dsd-neo/crypto/backup_seal.h>
#include <functional>
#include <memory>
#include <type_traits>    // IWYU pragma: keep
#include "backup_apply.h" // IWYU pragma: keep -- the constructor takes the startup ApplyResult.

class QThreadPool;

namespace dsd_qt {

class DecoderHost;
class DocumentImportTransport;
class ImportService;

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
     * @param host      Session state, quit and engine check; the destructor stops its document transfers. It must
     *                  outlive the model. The worker never calls it.
     * @param imports   The import worker a restore reads its document on. Held weakly: once it is gone (the app is
     *                  closing) an inspection is refused (Io).
     * @param transport Writes a saved export into its document, on the worker, which keeps it, so a save that ends
     *                  after the host and the model needs neither. Without one a save is refused (Io).
     * @param startup   What apply_pending_restore() returned at this start.
     */
    BackupModel(DecoderHost* host, ImportService* imports, std::shared_ptr<DocumentImportTransport> transport,
                const backup::ApplyResult& startup, QObject* parent = nullptr);
    /**
     * Drops the jobs that never started (their files go, and a save's destination is discarded), cancels the rest,
     * stops the host's document transfers and waits up to ImportService::kStopWaitMs for the worker. A job blocked past
     * that (a provider that ignores the abort) is left to finish on a worker that is never deleted, without the model.
     */
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

    /**
     * Whether an export can be made now, checked before the screen asks for a passphrase (issue #828). Answers
     * exportPrepared's map shape at once: ok, or "Unreadable" with `unreadable` the file names
     * backup_unreadable_files() lists, recorded as the last prepareExport result. prepareExport() checks again, for a
     * file that becomes unreadable in between.
     */
    Q_INVOKABLE QVariantMap checkExport();

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

    /**
     * Called on the worker as each inspection starts, before it reads the job's copy of the document. Empty in
     * production; tests hold an inspection there. The function must be safe to call from the worker thread.
     */
    void setInspectionHold(std::function<void()> hold);

    /**
     * Block until no job is left on the worker; its results stay queued for the event loop. False on timeout. A
     * restore's document read runs on the import worker, and its inspection is queued here only once that read's
     * commit has run on the UI thread.
     */
    bool waitForJobs(int timeoutMs = -1);

  Q_SIGNALS:
    void busyChanged();
    void flowActiveChanged();
    void restoreAllowedChanged();
    void restorePendingChanged();
    void restoreCancellableChanged();
    void startupOutcomeChanged();
    void lastResultChanged();
    /**
     * `{ok, error, summary, notIncluded, unreadable}`; `summary` as summary_to_map(), `notIncluded` a list of plain
     * lines, `unreadable` the file names an "Unreadable" export could not read (empty otherwise).
     */
    void exportPrepared(const QVariantMap& result);
    /** `{ok, error, summary, notIncluded, unreadable}`, the summary and lines from the export that was saved. */
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

    struct Shared;
    struct Work;

    struct Job {
        QString id;
        JobKind kind = JobKind::None;
        /** The job's own file: the sealed export (Prepare, Save) or the staging directory (Inspect). */
        QString path;
        /** Inspect: the job's copy of the document, `<CacheLocation>/backup/<job>.in`. */
        QString copyPath;
        /** Inspect: json_store_root() as the UI thread read it, for the worker's own cleanup. */
        QString dataRoot;
        /** Inspect: the import worker's read of the document while it runs, else 0. */
        qint64 read = 0;
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
    /** Cancel @p job's document read on the import worker, while it runs. */
    void cancelRead(const Job& job);
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

    /**
     * Queue @p run for the worker thread; the destructor runs @p drop instead when it never started. Neither may hold
     * the model or the host.
     */
    void post(std::function<void(Shared&)> run, std::function<void(Shared&)> drop);
    /** Worker side: run the oldest queued work. */
    static void runNext(const std::shared_ptr<Shared>& shared);
    /**
     * Worker side: queue @p deliver for the UI thread and ask the event loop to run it, or, once the model is gone,
     * run @p orphaned (optional) there instead: the job's files are its own to remove.
     */
    static void reply(Shared& shared, std::function<void(BackupModel&)> deliver, const std::function<void()>& orphaned);

    /** The import worker's read of @p job's document ended (UI thread); @p inspect is the inspection to queue. */
    void onDocumentRead(const Job& job, int copied, const std::function<void(Shared&)>& inspect);
    void onExportBuilt(const Job& job, const QVariantMap& result);
    void onExportSaved(const Job& job, const QVariantMap& result);
    void onInspected(const Job& job, const QVariantMap& result);

    DecoderHost* m_host;
    QPointer<ImportService> m_imports;
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
    std::function<void()> m_inspectionHold;
    /*
     * The only state both threads touch: work and replies cross through its queues under one std::mutex, so the
     * handoff does not depend on how Qt's own queues synchronize. It outlives the model while a job left behind runs.
     */
    std::shared_ptr<Shared> m_shared;
    std::unique_ptr<QThreadPool> m_pool;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_BACKUP_MODEL_H_ */
