// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The backup screens' model (issue #580). A job copies everything it needs when it starts on the UI thread: the
 * snapshot, the paths, the host, its own copy of the passphrase. On the worker it touches only its own cache file and
 * staging directory, through calls that hold no shared mutable state: the archive and staging functions, the host's
 * document copies, and ImportedFilesModel::validate() inside stage_restore() (each CSV validator parses into its own
 * heap state). The data root is read once, on the UI thread, and handed to the parse and the staging.
 *
 * Work goes to the worker and replies come back through two queues behind one std::mutex, and a capture-free queued
 * call tells the UI thread to drain the replies. A reply for a job that is no longer the current one is dropped there,
 * and that job's files are removed. */

#include "backup_model.h"

#include <QByteArray>
#include <QChar>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1String>
#include <QRunnable>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <QVariant>
#include <Qt>
#include <atomic>
#include <deque>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/backup_seal.h>
#include <dsd-neo/platform/file_compat.h>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <mutex>
#include <qflags.h>
#include <stddef.h>
#include <utility>
#include "decoder_host.h"
#include "json_store.h"
#include "realtime_clock.h"
#include "teardown_trace.h"

namespace dsd_qt {

namespace {

using backup::BackupError;

/** Free space a staging needs beyond twice the backup's size. */
constexpr qint64 kStagingMarginBytes = 1024LL * 1024;

constexpr char kBusy[] = "Busy";
constexpr char kPassphraseTooShort[] = "PassphraseTooShort";
constexpr char kRestorePending[] = "RestorePending";
constexpr char kRestoreInProgress[] = "RestoreInProgress";

QString
error_name(BackupError error) {
    return backup::backup_error_name(error);
}

/** A copy of a passphrase that only one job holds, so it can be wiped once used. */
class Passphrase {
  public:
    explicit Passphrase(const QString& text) : m_text(text.constData(), text.size()) {}

    ~Passphrase() { wipe(); }

    Passphrase(const Passphrase&) = delete;
    Passphrase& operator=(const Passphrase&) = delete;

    const QString&
    text() const {
        return m_text;
    }

    void
    wipe() {
        if (!m_text.isEmpty()) {
            DSD_SECURE_ZERO(m_text.data(), static_cast<size_t>(m_text.size()) * sizeof(QChar));
            m_text.clear();
        }
    }

  private:
    QString m_text;
};

void
wipe(QByteArray& bytes) {
    if (!bytes.isEmpty()) {
        DSD_SECURE_ZERO(bytes.data(), static_cast<size_t>(bytes.size()));
    }
    bytes.clear();
}

QString
new_job_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/** `<CacheLocation>/backup`, where exports wait to be saved and picked documents are copied. */
QString
cache_dir() {
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return cache.isEmpty() ? QString() : cache + QStringLiteral("/backup");
}

/** Remove every file (or link, never followed) a job left in the cache folder. */
void
sweep_cache(const QString& dir) {
    if (dir.isEmpty()) {
        return;
    }
    const QFileInfoList entries =
        QDir(dir).entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        QFile::remove(entry.filePath());
    }
}

bool
write_private(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
           && file.write(bytes) == bytes.size() && file.commit();
}

bool
read_sealed(const QString& path, QByteArray* bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > backup::kMaxSealedBytes) {
        return false;
    }
    *bytes = file.readAll();
    return file.error() == QFileDevice::NoError && bytes->size() <= backup::kMaxSealedBytes;
}

/** `{ok, error, summary}`; ok when @p error is empty. */
QVariantMap
result_map(const QString& error, const QVariantMap& summary = QVariantMap()) {
    return {{QStringLiteral("ok"), error.isEmpty()},
            {QStringLiteral("error"), error},
            {QStringLiteral("summary"), summary}};
}

/** result_map() plus the export's not-included lines beside its summary. */
QVariantMap
export_map(const QString& error, const QVariantMap& summary = QVariantMap()) {
    QVariantMap map = result_map(error, summary);
    map.insert(QStringLiteral("notIncluded"), summary.value(QStringLiteral("notIncluded")).toStringList());
    return map;
}

/** startupOutcome's shape: kind, message and the backup's counts, all plain data. */
QVariantMap
outcome_map(const QString& kind, const QString& message, const QVariantMap& summary) {
    QVariantMap map{{QStringLiteral("kind"), kind}, {QStringLiteral("message"), message}};
    for (const char* key : {"systems", "scanLists", "libraryFiles", "profiles", "ranges", "avoids",
                            "recordingsToChoose", "systemsWithExtraArgs"}) {
        map.insert(QLatin1String(key), summary.value(QLatin1String(key)).toInt());
    }
    map.insert(QStringLiteral("appExtraArgs"), summary.value(QStringLiteral("appExtraArgs")).toBool());
    map.insert(QStringLiteral("createdAt"), summary.value(QStringLiteral("createdAt")).toLongLong());
    map.insert(QStringLiteral("app"), summary.value(QStringLiteral("app")).toString());
    map.insert(QStringLiteral("notIncluded"), summary.value(QStringLiteral("notIncluded")).toStringList());
    return map;
}

/** This start's apply, or else the result an earlier start's apply left in its file. */
QVariantMap
startup_outcome(const backup::ApplyResult& startup) {
    if (startup.outcome != backup::ApplyOutcome::None) {
        return outcome_map(backup::apply_outcome_name(startup.outcome), startup.reason, startup.summary);
    }
    const QVariantMap file = backup::read_result_file();
    const QString kind = file.value(QStringLiteral("outcome")).toString();
    if (kind != QLatin1String("applied") && kind != QLatin1String("failed") && kind != QLatin1String("deferred")
        && kind != QLatin1String("stuck")) {
        return outcome_map(QString(), QString(), QVariantMap());
    }
    return outcome_map(kind, file.value(QStringLiteral("reason")).toString(),
                       file.value(QStringLiteral("summary")).toMap());
}

/** Everything an export job needs, copied on the UI thread. */
struct ExportWork {
    backup::Snapshot snapshot;
    dsd_backup_kdf_params kdf{};
    std::shared_ptr<Passphrase> passphrase;
    std::shared_ptr<std::atomic<bool>> cancel;
    QString path;
};

/**
 * Build and seal on the worker, into the job's own cache file, stopping early once cancelled. Returns
 * exportPrepared's map; a cancelled job's map is dropped and its file removed on the UI thread.
 */
QVariantMap
run_export(const ExportWork& work) {
    const std::atomic<bool>& cancel = *work.cancel;
    QString error;
    backup::BuildResult built;
    if (!cancel.load()) {
        built = backup::backup_build_archive(work.snapshot, work.cancel.get());
        error = built.error == BackupError::None ? QString() : error_name(built.error);
    }
    if (error.isEmpty() && !cancel.load() && !built.cancelled) {
        int status = DSD_BACKUP_OK;
        const QByteArray sealed = backup::backup_seal(built.archive, work.passphrase->text(), work.kdf, &status);
        wipe(built.archive);
        if (sealed.isEmpty()) {
            error = error_name(backup::backup_error_from_seal_status(status));
        } else if (!cancel.load() && !write_private(work.path, sealed)) {
            error = error_name(BackupError::Io);
        }
    }
    wipe(built.archive);
    work.passphrase->wipe();
    return export_map(error, error.isEmpty() ? backup::summary_to_map(built.summary) : QVariantMap());
}

/** Everything an inspection job needs, copied on the UI thread. */
struct InspectWork {
    DecoderHost* host = nullptr;
    QString reference;
    QString copyPath;   // the job's own copy of the document
    QString stagingDir; // the job's own staging directory
    QString root;       // json_store_root()
    std::shared_ptr<Passphrase> passphrase;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::function<qint64(const QString&)> freeBytes;
};

/** One inspection on the worker: fetch, open, validate, check space, stage. */
class Inspection {
  public:
    explicit Inspection(InspectWork work) : m_work(std::move(work)) {}

    ~Inspection() { wipe(m_archive); }

    Inspection(const Inspection&) = delete;
    Inspection& operator=(const Inspection&) = delete;

    /**
     * inspectFinished's map. A cancelled job stops at the next step; its map is dropped and its staging removed on
     * the UI thread.
     */
    QVariantMap
    run() {
        QString error = fetch();
        if (error.isEmpty() && !cancelled()) {
            error = parse();
        }
        if (error.isEmpty() && !cancelled()) {
            error = stage();
        }
        wipe(m_archive);
        m_work.passphrase->wipe();
        return result_map(error, error.isEmpty() ? backup::summary_to_map(m_staged) : QVariantMap());
    }

  private:
    bool
    cancelled() const {
        return m_work.cancel->load();
    }

    /** Copy the document into the job's file, read and remove it, and open it. */
    QString
    fetch() {
        if (cancelled()) {
            return QString();
        }
        const int copied = m_work.host->copyDocumentToFile(m_work.reference, m_work.copyPath, backup::kMaxSealedBytes);
        QByteArray sealed;
        const bool read = copied == DecoderHost::DocumentCopied && read_sealed(m_work.copyPath, &sealed);
        QFile::remove(m_work.copyPath);
        // Only a document that could not be read is the file's fault (Io); writing the job's copy of it, or reading
        // that copy back, is this phone's.
        if (copied == DecoderHost::DocumentTooLarge) {
            return error_name(BackupError::TooLarge);
        }
        if (copied == DecoderHost::DocumentLocalFailed) {
            return error_name(BackupError::StagingFailed);
        }
        if (copied != DecoderHost::DocumentCopied) {
            return error_name(BackupError::Io);
        }
        if (!read) {
            return error_name(BackupError::StagingFailed);
        }
        if (cancelled()) {
            return QString();
        }
        // Every passphrase is tried, whatever its length: a backup is sealed under whatever minimum its export
        // enforced, which may not be today's.
        int status = DSD_BACKUP_OK;
        m_archive = backup::backup_open(sealed, m_work.passphrase->text(), &status);
        // The envelope refuses an empty passphrase as an argument error; no backup can be sealed under one.
        if (status == DSD_BACKUP_ERR_ARGS && m_work.passphrase->text().isEmpty()) {
            status = DSD_BACKUP_ERR_AUTH;
        }
        m_work.passphrase->wipe();
        return status == DSD_BACKUP_OK ? QString() : error_name(backup::backup_error_from_open_status(status));
    }

    /** Validate the archive for this data root, and require room to stage and later apply it. */
    QString
    parse() {
        m_parsed = backup::backup_parse_archive(m_archive, m_work.root);
        if (m_parsed.error == BackupError::Io) {
            return error_name(BackupError::StagingFailed); // this phone's data root, not the file
        }
        if (m_parsed.error != BackupError::None) {
            return error_name(m_parsed.error);
        }
        const qint64 free = m_work.freeBytes
                                ? m_work.freeBytes(m_work.root)
                                : static_cast<qint64>(dsd_path_free_bytes(m_work.root.toUtf8().constData()));
        if (free >= 0 && free < 2 * static_cast<qint64>(m_archive.size()) + kStagingMarginBytes) {
            return error_name(BackupError::NoSpace);
        }
        return QString();
    }

    QString
    stage() {
        QString error;
        // The staging's summary: the parse's, less any library row this phone cannot read. A cancel stops the staging
        // between dry runs, so teardown never waits on a long library.
        if (backup::stage_restore(m_work.root, m_parsed, m_archive, m_work.stagingDir, &error, &m_staged,
                                  [this]() { return !cancelled(); })) {
            return QString();
        }
        if (cancelled()) {
            return QString(); // the result is dropped and the job's staging directory removed on the UI thread
        }
        return error.isEmpty() ? error_name(BackupError::StagingFailed) : error;
    }

    InspectWork m_work;
    QByteArray m_archive;
    backup::ParseResult m_parsed;
    backup::Summary m_staged;
};

} // namespace

BackupModel::BackupModel(DecoderHost* host, const backup::ApplyResult& startup, QObject* parent)
    : QObject(parent), m_host(host), m_self(this), m_runner(QRunnable::create([this]() { runNext(); })) {
    dsd_backup_kdf_params_default(&m_kdf);
    // One worker: jobs never overlap, so a cancelled job finishes before the next one starts. The one runner is
    // started once per queued job and runs them in order.
    m_runner->setAutoDelete(false);
    m_pool.setMaxThreadCount(1);
    m_pool.setThreadPriority(QThread::LowPriority);
    // Nothing runs yet, so whatever a job left behind (a kill, a crash) is stale.
    sweep_cache(cache_dir());
    backup::sweep_stale_staging();
    m_startupOutcome = startup_outcome(startup);
    // The startup apply has run: a stage still committed is one it deferred or could not finish.
    m_restorePending = backup::pending_committed();
    m_restoreCancellable = m_restorePending && !backup::pending_apply_started();
    m_restoreAllowed = restoreAllowed();
    if (m_host != nullptr) {
        connect(m_host, &DecoderHost::sessionStateChanged, this, &BackupModel::updateRestoreAllowed);
    }
}

BackupModel::~BackupModel() {
    const TeardownStage stage("~BackupModel");
    // Every job still on the worker is the current one or a dropped one. A document copy cannot be cancelled, and a
    // slow document provider must not hold the UI thread while the app closes, so the host stops its copies first.
    if (m_host != nullptr && (m_job.kind != JobKind::None || !m_dropped.isEmpty())) {
        m_host->abortDocumentTransfers();
    }
    if (m_job.kind != JobKind::None && m_job.kind != JobKind::Save) {
        m_job.cancel->store(true);
        m_dropped.append(m_job);
    }
    m_pool.waitForDone();
    {
        // Replies still queued never run now; the lock orders their destruction after the worker's last write.
        const std::lock_guard<std::mutex> lock(m_handoff);
        m_replies.clear();
        m_work.clear();
    }
    // The queued results of dropped jobs will never arrive; their files go now.
    for (const Job& job : m_dropped) {
        removeJobFiles(job);
    }
    if (!m_export.path.isEmpty()) {
        QFile::remove(m_export.path);
    }
}

bool
BackupModel::restoreAllowed() const {
    return !busy() && (m_host == nullptr || !m_host->sessionActive());
}

QString
BackupModel::suggestedFileName() const {
    return QStringLiteral("dsd-neo-backup-%1.dsdbak").arg(realtimeCurrentDate().toString(Qt::ISODate));
}

void
BackupModel::prepareExport(const QString& passphrase) {
    const QString dir = cache_dir();
    QString refusal;
    if (busy()) {
        refusal = QLatin1String(kBusy);
    } else if (!backup::backup_passphrase_ok(passphrase)) {
        refusal = QLatin1String(kPassphraseTooShort);
    } else if (dir.isEmpty() || !QDir().mkpath(dir)) {
        refusal = error_name(BackupError::Io);
    }
    if (!refusal.isEmpty()) {
        refuse(&BackupModel::exportPrepared, QStringLiteral("prepareExport"), refusal);
        return;
    }
    dropExportFile();
    clearLastResult();
    const QString id = new_job_id();
    const Job job = beginJob(JobKind::Prepare, id, dir + QLatin1Char('/') + id + QStringLiteral(".dsdbak"));
    const ExportWork work{backup::backup_take_snapshot(), m_kdf, std::make_shared<Passphrase>(passphrase), job.cancel,
                          job.path};
    post([this, job, work]() {
        const QVariantMap result = run_export(work);
        reply([this, job, result]() { onExportBuilt(job, result); });
    });
}

void
BackupModel::saveExport(const QString& reference) {
    QString refusal;
    if (busy()) {
        refusal = QLatin1String(kBusy);
    } else if (m_export.path.isEmpty() || m_host == nullptr) {
        refusal = error_name(BackupError::Io); // nothing prepared
    }
    if (!refusal.isEmpty()) {
        refuse(&BackupModel::exportFinished, QStringLiteral("saveExport"), refusal);
        return;
    }
    const Job job = beginJob(JobKind::Save, new_job_id(), m_export.path);
    const QVariantMap summary = m_export.summary;
    const DecoderHost* host = m_host;
    post([this, job, summary, host, reference]() {
        // Not cancellable: the copy runs to its end, and the cache file goes either way.
        const bool saved = host->copyFileToDocument(job.path, reference);
        QFile::remove(job.path);
        const QVariantMap result = export_map(saved ? QString() : error_name(BackupError::Io), summary);
        reply([this, job, result]() { onExportSaved(job, result); });
    });
}

void
BackupModel::discardExport() {
    if (m_job.kind == JobKind::Save) {
        return;
    }
    const bool dropped = m_job.kind == JobKind::Prepare && dropJob();
    dropExportFile();
    if (dropped) {
        announceIdle();
    }
}

void
BackupModel::inspectRestore(const QString& reference, const QString& passphrase) {
    const QString dir = cache_dir();
    const QString id = new_job_id();
    const QString stagingDir = backup::staging_dir_for_job(id);
    QString refusal;
    if (busy()) {
        refusal = QLatin1String(kBusy);
    } else if (m_host != nullptr && m_host->sessionActive()) {
        refusal = error_name(BackupError::SessionActive);
    } else if (backup::pending_committed()) {
        refusal = QLatin1String(kRestorePending);
    } else if (m_host == nullptr || dir.isEmpty() || stagingDir.isEmpty() || !QDir().mkpath(dir)) {
        refusal = error_name(BackupError::StagingFailed); // no cache or staging place on this phone
    }
    if (!refusal.isEmpty()) {
        refuse(&BackupModel::inspectFinished, QStringLiteral("inspectRestore"), refusal);
        return;
    }
    clearLastResult();
    const Job job = beginJob(JobKind::Inspect, id, stagingDir);
    InspectWork work;
    work.host = m_host;
    work.reference = reference;
    work.copyPath = dir + QLatin1Char('/') + id + QStringLiteral(".in");
    work.stagingDir = stagingDir;
    work.root = json_store_root();
    work.passphrase = std::make_shared<Passphrase>(passphrase);
    work.cancel = job.cancel;
    work.freeBytes = m_freeBytes;
    post([this, job, work]() {
        Inspection inspection(work);
        const QVariantMap result = inspection.run();
        reply([this, job, result]() { onInspected(job, result); });
    });
}

QVariantMap
BackupModel::commitRestore() {
    QString error;
    if (busy()) {
        error = QLatin1String(kBusy);
    } else if (m_host != nullptr && (m_host->sessionActive() || m_host->engineBusy())) {
        error = error_name(BackupError::SessionActive);
    } else if (!backup::commit_pending()) {
        error = error_name(BackupError::Io);
    }
    if (!error.isEmpty()) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("quitting"), false}, {QStringLiteral("error"), error}};
    }
    // Asked once: a second confirmation while the app is already closing changes nothing.
    if (!m_quitting && m_host != nullptr && m_host->quitApp()) {
        m_quitting = true;
    }
    syncRestorePending();
    return {
        {QStringLiteral("ok"), true}, {QStringLiteral("quitting"), m_quitting}, {QStringLiteral("error"), QString()}};
}

QVariantMap
BackupModel::discardRestore() {
    // A running inspection would stage a pending restore again. It is dropped now, and idle is announced once the
    // discard has settled.
    const bool dropped = m_job.kind == JobKind::Inspect && dropJob();
    const bool removed = backup::discard_pending();
    // A false answer means one of two things, and only the marker tells which.
    const bool discarded = removed || !backup::pending_committed();
    syncRestorePending();
    if (discarded && m_startupOutcome.value(QStringLiteral("kind")) == QLatin1String("deferred")) {
        acknowledgeStartupOutcome();
    }
    if (dropped) {
        announceIdle();
    }
    QString error;
    if (!removed) {
        error = backup::pending_apply_started() ? QLatin1String(kRestoreInProgress) : error_name(BackupError::Io);
    }
    return {
        {QStringLiteral("ok"), removed}, {QStringLiteral("discarded"), discarded}, {QStringLiteral("error"), error}};
}

void
BackupModel::cancel() {
    if (dropJob()) {
        announceIdle();
    }
}

void
BackupModel::setFlowActive(bool active) {
    if (m_flowActive != active) {
        m_flowActive = active;
        Q_EMIT flowActiveChanged();
    }
}

void
BackupModel::acknowledgeStartupOutcome() {
    (void)backup::clear_result_file();
    setStartupOutcome(outcome_map(QString(), QString(), QVariantMap()));
}

QVariantMap
BackupModel::discardDeferredRestore() {
    return discardRestore(); // which also retires the deferred card once the restore is discarded
}

void
BackupModel::setKdfParams(const dsd_backup_kdf_params& kdf) {
    m_kdf = kdf;
}

void
BackupModel::setFreeBytes(std::function<qint64(const QString&)> freeBytes) {
    m_freeBytes = std::move(freeBytes);
}

bool
BackupModel::waitForJobs(int timeoutMs) {
    return m_pool.waitForDone(timeoutMs);
}

void
BackupModel::post(std::function<void()> work) {
    {
        const std::lock_guard<std::mutex> lock(m_handoff);
        m_work.push_back(std::move(work));
    }
    m_pool.start(m_runner.get());
}

void
BackupModel::runNext() {
    std::function<void()> work;
    {
        const std::lock_guard<std::mutex> lock(m_handoff);
        if (m_work.empty()) {
            return;
        }
        work = std::move(m_work.front());
        m_work.pop_front();
    }
    work();
}

void
BackupModel::reply(std::function<void()> handler) {
    {
        const std::lock_guard<std::mutex> lock(m_handoff);
        m_replies.push_back(std::move(handler));
    }
    // The destructor's waitForDone() keeps the model alive while the worker runs; the guard and Qt drop the queued
    // call if the model is gone by the time the event loop gets to it.
    if (BackupModel* model = m_self.data()) {
        QMetaObject::invokeMethod(model, "deliverReplies", Qt::QueuedConnection);
    }
}

void
BackupModel::deliverReplies() {
    for (;;) {
        std::function<void()> next;
        {
            const std::lock_guard<std::mutex> lock(m_handoff);
            if (m_replies.empty()) {
                return;
            }
            next = std::move(m_replies.front());
            m_replies.pop_front();
        }
        next();
    }
}

BackupModel::Job
BackupModel::beginJob(JobKind kind, const QString& id, const QString& path) {
    m_job = Job{id, kind, path, std::make_shared<std::atomic<bool>>(false)};
    Q_EMIT busyChanged();
    updateRestoreAllowed();
    return m_job;
}

bool
BackupModel::dropJob() {
    if (m_job.kind == JobKind::None || m_job.kind == JobKind::Save) {
        return false;
    }
    m_job.cancel->store(true);
    m_dropped.append(m_job);
    m_job = Job();
    return true;
}

void
BackupModel::endJob() {
    m_job = Job();
    announceIdle();
}

void
BackupModel::announceIdle() {
    Q_EMIT busyChanged();
    updateRestoreAllowed();
}

bool
BackupModel::takeDropped(const Job& job) {
    if (job.id == m_job.id) {
        return false;
    }
    for (qsizetype i = 0; i < m_dropped.size(); ++i) {
        if (m_dropped.at(i).id == job.id) {
            m_dropped.removeAt(i);
            break;
        }
    }
    removeJobFiles(job);
    return true;
}

void
BackupModel::removeJobFiles(const Job& job) {
    if (job.kind == JobKind::Prepare) {
        QFile::remove(job.path);
    } else if (job.kind == JobKind::Inspect) {
        backup::discard_staging(job.path);
    }
}

void
BackupModel::setLastResult(const QString& action, const QVariantMap& result) {
    m_lastResult = result;
    m_lastResult.insert(QStringLiteral("action"), action);
    Q_EMIT lastResultChanged();
}

void
BackupModel::clearLastResult() {
    if (!m_lastResult.isEmpty()) {
        m_lastResult.clear();
        Q_EMIT lastResultChanged();
    }
}

void
BackupModel::refuse(void (BackupModel::*signal)(const QVariantMap&), const QString& action, const QString& error) {
    const QVariantMap result = signal == &BackupModel::inspectFinished ? result_map(error) : export_map(error);
    // Reported after the call returns, like every job's result, so QML has one path for both.
    QMetaObject::invokeMethod(
        this,
        [this, signal, action, result]() {
            setLastResult(action, result);
            Q_EMIT(this->*signal)(result);
        },
        Qt::QueuedConnection);
}

void
BackupModel::dropExportFile() {
    if (!m_export.path.isEmpty()) {
        QFile::remove(m_export.path);
    }
    m_export = PreparedExport();
}

void
BackupModel::updateRestoreAllowed() {
    const bool allowed = restoreAllowed();
    if (allowed != m_restoreAllowed) {
        m_restoreAllowed = allowed;
        Q_EMIT restoreAllowedChanged();
    }
}

void
BackupModel::syncRestorePending() {
    const bool pending = backup::pending_committed();
    const bool cancellable = pending && !backup::pending_apply_started();
    const bool pendingChanged = pending != m_restorePending;
    const bool cancellableChanged = cancellable != m_restoreCancellable;
    m_restorePending = pending;
    m_restoreCancellable = cancellable;
    // Both settle before either is announced, so a binding on one sees the other's new value.
    if (pendingChanged) {
        Q_EMIT restorePendingChanged();
    }
    if (cancellableChanged) {
        Q_EMIT restoreCancellableChanged();
    }
}

void
BackupModel::setStartupOutcome(const QVariantMap& outcome) {
    if (outcome != m_startupOutcome) {
        m_startupOutcome = outcome;
        Q_EMIT startupOutcomeChanged();
    }
}

/*
 * The handlers below settle every piece of state a job changes before announcing idle, and report last. Whoever
 * reacts to busy turning false (a picker opening, the Settings card) already sees the outcome.
 */

void
BackupModel::onExportBuilt(const Job& job, const QVariantMap& result) {
    if (takeDropped(job)) {
        return; // cancelled: its file is gone
    }
    if (result.value(QStringLiteral("ok")).toBool()) {
        m_export = PreparedExport{job.path, result.value(QStringLiteral("summary")).toMap()};
    } else {
        QFile::remove(job.path);
        setLastResult(QStringLiteral("prepareExport"), result);
    }
    endJob();
    Q_EMIT exportPrepared(result);
}

void
BackupModel::onExportSaved(const Job& job, const QVariantMap& result) {
    if (job.id != m_job.id) {
        return; // a save is never dropped; this cannot happen
    }
    m_export = PreparedExport();
    setLastResult(QStringLiteral("saveExport"), result);
    endJob();
    Q_EMIT exportFinished(result);
}

void
BackupModel::onInspected(const Job& job, const QVariantMap& result) {
    if (takeDropped(job)) {
        return; // cancelled: only its own staging directory was removed
    }
    QVariantMap report = result;
    if (report.value(QStringLiteral("ok")).toBool() && !backup::promote_staging(job.path)) {
        backup::discard_staging(job.path);
        report = result_map(backup::pending_committed() ? QLatin1String(kRestorePending)
                                                        : error_name(BackupError::StagingFailed));
    }
    setLastResult(QStringLiteral("inspectRestore"), report);
    endJob();
    Q_EMIT inspectFinished(report);
}

} // namespace dsd_qt
