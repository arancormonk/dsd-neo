// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Replay captures picked in the wizard (#832): copied on the import worker into `<AppData>/replays/`, and
 * reclaimed at a later start when no saved system kept them.
 *
 * A capture runs to gigabytes, so a copy reports its progress, can be cancelled, and must leave 512 MiB free. The
 * manifest `replay_copies.json` lists every copy this store made, each **pending** (nothing saved names it yet) or
 * **kept** (a saved system named it). Every write errs toward keeping: a copy is recorded pending before QML sees it;
 * SavedSystemsModel adopts one (pending → kept) before it saves a system naming it and releases one (kept → pending)
 * only after the save that dropped the last system naming it. At startup only a pending copy no saved system names is
 * deleted, and only when both stores read cleanly, the decoder is idle and no restore is waiting to finish. A file the
 * manifest does not list is never deleted.
 */

#ifndef DSD_NEO_SRC_UI_QT_REPLAY_STORE_H_
#define DSD_NEO_SRC_UI_QT_REPLAY_STORE_H_

#include <QMap> // IWYU pragma: keep -- QVariantMap is a QMap; replayImportFinished() carries it.
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtGlobal>
#include <functional>
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.

namespace dsd_qt {

class DecoderHost;
class ImportService;
class SavedSystemsModel;

class ReplayStore : public QObject {
    Q_OBJECT
    /** A copy is running (from beginReplayImport() until its replayImportFinished()). */
    Q_PROPERTY(bool copying READ copying NOTIFY progressChanged)
    /** The running copy's document name. */
    Q_PROPERTY(QString copyName READ copyName NOTIFY progressChanged)
    /** Bytes the running copy has made so far. */
    Q_PROPERTY(qint64 copyBytes READ copyBytes NOTIFY progressChanged)
    /** The running copy's size; -1 while unknown. */
    Q_PROPERTY(qint64 copyTotal READ copyTotal NOTIFY progressChanged)

  public:
    /** What a copy must leave free on the device (D1). */
    static constexpr qint64 kReserveBytes = 512LL * 1024 * 1024;
    /** How often a running copy's progress is read. */
    static constexpr int kProgressIntervalMs = 250;

    /** The bytes free on the volume holding @p folder, or -1 when unknown; called on the import worker. */
    using FreeSpace = std::function<qint64(const QString& folder)>;

    /** @p imports runs the copies; it is destroyed before this store, which never calls it from its destructor. */
    ReplayStore(DecoderHost* host, ImportService* imports, QObject* parent = nullptr);
    ~ReplayStore() override;

    ReplayStore(const ReplayStore&) = delete;
    ReplayStore& operator=(const ReplayStore&) = delete;

    /** @brief `<AppData>/replays`: every copy this store makes, and nothing else. */
    static QString replaysRoot();
    /** @brief `<AppData>/replay_copies.json`. */
    static QString manifestPath();

    /**
     * @brief Make @p reference a file the engine can replay; answered by replayImportFinished(id, …), queued, once.
     *
     * A local reference (a file:// URL or an absolute path) is answered with its path and never copied. Anything
     * else (a content URI) is copied into replaysRoot() under a free form of its provider's name (else @p fileName),
     * at most the free space less kReserveBytes, and recorded pending. A copy still running is cancelled first: one
     * copy runs at a time.
     *
     * The result holds ok, path and name, else error: "too_large" (neededBytes: the size plus kReserveBytes, -1 when
     * the size is unknown; freeBytes; reserveBytes, kReserveBytes, so a message can name the size and the reserve),
     * "local" (noSpace: the device filled during the copy), "read" or "cancelled".
     *
     * @return The request id, positive.
     */
    Q_INVOKABLE int beginReplayImport(const QString& reference, const QString& fileName);
    /** @brief Cancel the running copy; it answers "cancelled" unless its commit already ran. */
    Q_INVOKABLE void cancelReplayImport();

    bool copying() const;
    QString copyName() const;
    qint64 copyBytes() const;
    qint64 copyTotal() const;

    /**
     * @brief Mark the copy at @p path kept: a saved system is about to name it. Call it before that save.
     *
     * Static, like the manifest it writes (Qt thread only); SavedSystemsModel calls it only when it has a store.
     * @return true when the copy is kept now, or @p path is no copy this store recorded; false when the manifest
     *         could not be read or written (the caller must not save a system naming the copy).
     */
    static bool adopt(const QString& path);
    /**
     * @brief Mark the copy at @p path pending again: the save that dropped the last saved system naming it is done.
     * @return false when the manifest could not be read or written; the copy then stays kept.
     */
    static bool release(const QString& path);

    /**
     * @brief At startup: delete every pending copy that no saved system names, and drop it from the manifest.
     *
     * Nothing is deleted unless @p systemsLoaded (the saved systems store read cleanly or is absent), the manifest
     * reads cleanly and the decoder is idle. A manifest that cannot be read is set aside to replay_copies.json.bad
     * (replacing an older one) so later copies are recorded again; what it listed stays where it is.
     */
    void reclaim(const SavedSystemsModel& systems, bool systemsLoaded);

    /**
     * @brief At startup: copy every replay a saved system keeps in the OS cache into replaysRoot(), on the import
     * worker, and point the systems at the copies.
     *
     * Each cached file is copied through the transport (never the local-file shortcut), at most what leaves
     * kReserveBytes free, and published pending. Each is adopted just before the save that repoints only the rows
     * whose uid still names the cached path; a copy no row takes (the row was edited, removed or reseeded meanwhile,
     * or the save failed) is released to pending. The cached file stays: it is the OS's cache, and a draft or session
     * may still hold it. Until the job finishes, and after a crash, the rows keep the cached path, which still plays,
     * and the next start reclaims any copy left pending and tries again.
     */
    void migrateCachedReplays(SavedSystemsModel* systems);

    /** @brief Whether @p paths holds @p path, as written or as the same file (canonical paths). */
    static bool names(const QStringList& paths, const QString& path);

    /** @brief Replace how free space is read (tests). */
    void setFreeSpace(FreeSpace freeSpace);

  Q_SIGNALS:
    /**
     * @brief The answer to beginReplayImport() @p id: ok, path, name, error, noSpace, neededBytes, freeBytes,
     * reserveBytes.
     */
    void replayImportFinished(int id, const QVariantMap& result);
    void progressChanged();

  private:
    void commitImport(qint64 id, const QVariantMap& staged, bool cancelled);
    /** Answer @p id with @p result later, from the event loop. */
    void answerQueued(qint64 id, const QVariantMap& result);
    void pollProgress();
    void endCopy(qint64 id);

    DecoderHost* m_host = nullptr;
    QPointer<ImportService> m_imports;
    FreeSpace m_freeSpace;
    QTimer m_progressTimer;
    qint64 m_current = 0;
    QString m_copyName;
    qint64 m_copyBytes = 0;
    qint64 m_copyTotal = -1;
};

} // namespace dsd_qt

#endif // DSD_NEO_SRC_UI_QT_REPLAY_STORE_H_
