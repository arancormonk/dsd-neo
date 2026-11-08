// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Library of imported CSV files (channel maps, talkgroups, keys, P25 band plans, radio IDs), persisted as JSON.
 *
 * The wizard's pickers and the imports screen share this model: importing copies
 * the picked document into a private staging folder on the import worker
 * (ImportService), dry-run validates it there for row counts, then publishes it
 * into durable app storage on the Qt thread and records one library row per
 * stored file. Saved systems reference stored paths, so one file can serve
 * several systems.
 */

#ifndef DSD_NEO_SRC_UI_QT_IMPORTED_FILES_MODEL_H_
#define DSD_NEO_SRC_UI_QT_IMPORTED_FILES_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.

namespace dsd_qt {
struct BundleRevisions;

class DecoderHost;
class ImportService;

class ImportedFilesModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /** Whether an import this model began has not finished yet. */
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged)
    /** Changes with every change to the library, a replaced file's counts included; bind entriesForType() to it. */
    Q_PROPERTY(int revision READ revision NOTIFY entriesChanged)

  public:
    /* Append new roles; never insert. A delegate that binds by number would
     * silently start reading a different column. */
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,       // absolute stored path; the row's identity
        TypeRole,       // "chan" | "group" | "keysDec" | "keysHex" | "p25Bandplan" | "src"
        ImportedAtRole, // seconds since epoch
        AcceptedRole,   // usable rows at the last validation
        SkippedRole,    // malformed rows at the last validation
        OriginRole,     // "" for a picked file, "radioreference" for a generated one
        RrSidRole,      // RadioReference system id
        RrKindRole,     // "group" | "chan"
        RrSiteIdsRole   // every selected siteId, comma-joined - what a refresh matches on
    };

    /**
     * @brief The library, importing through @p imports, the app's one import worker.
     *
     * The service may be destroyed first (the app's is); the model never calls it from its destructor.
     */
    ImportedFilesModel(DecoderHost* host, ImportService* imports, QObject* parent = nullptr);
    /** @brief The library with an import worker of its own, copying through @p host's transport (tests, fixtures). */
    explicit ImportedFilesModel(DecoderHost* host, QObject* parent = nullptr);
    ~ImportedFilesModel() override;

    /** @brief Every library row type the model can validate; a row of any other type cannot be used. */
    static QStringList knownTypes();

    /** @brief The folder the library stores its copies in; every row's file, bundles included, lies below it. */
    static QString libraryRoot();

    /**
     * @brief Dry-run validate @p path as @p type; false when it cannot be parsed.
     *
     * The rule every library row passed when it was imported (a file with no usable row still passes, with
     * @p accepted 0). A backup restore runs it on each staged library file before staging is accepted.
     */
    static bool validate(const QString& path, const QString& type, int* accepted, int* skipped,
                         QString* detail = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return static_cast<int>(m_rows.size());
    }

    bool
    importing() const {
        return !m_inFlight.isEmpty();
    }

    int
    revision() const {
        return static_cast<int>(m_revision & 0x7fffffffU);
    }

    /**
     * WP-D1: generate an app-owned destination without registering or writing a file. The path stays reserved for the
     * life of the process (the engine may still write it after the export flow gave up), so no import takes its name.
     */
    Q_INVOKABLE QString newTalkgroupListPath() const;
    /**
     * Register a successfully exported list in place; the engine keeps writing this path. False, with no row added,
     * when the library cannot be saved; the file stays where it is.
     */
    Q_INVOKABLE bool registerTalkgroupList(const QString& path);

    /**
     * @brief Import a picked document: copied and checked on the import worker, published on the Qt thread.
     *
     * @param reference   The picker's reference (a file:// URL, a path, or an Android content URI).
     * @param fileName    The picker's name hint; the provider's own name for the document wins over it.
     * @param type        The library type to import it as.
     * @param companions  A bundle's companion choices (channel maps and target lists).
     * @param replacePath The stored path of the row to replace, or empty for a new row. A replace commits only when
     *                    that row is still there unchanged (else "stale"); replacing a talkgroup list or a target
     *                    list is refused while the decoder runs ("busy"), at the start and again at the commit.
     * @return The request id (> 0); importFinished() answers it exactly once, queued, a refusal included. Result keys:
     *         ok, error, detail, path, name, type, accepted, skipped, limitBytes, noSpace. error is "" or "empty"
     *         (ok, no usable row) on success, else too_large (limitBytes), read, local (noSpace), format (detail),
     *         stale, busy, save (detail), path or cancelled.
     */
    Q_INVOKABLE int beginImport(const QString& reference, const QString& fileName, const QString& type,
                                const QVariantMap& companions, const QString& replacePath);
    /** @brief Cancel import @p id; it answers "cancelled" unless its commit already ran. */
    Q_INVOKABLE void cancelImport(int id);

    /**
     * @brief Copy a document into the library, validate it, record a row: beginImport()'s stage and commit, run at
     * once on the calling thread.
     *
     * @p fileName names the copy (a blank one leaves the document's own name); a pick goes through beginImport() or
     * importBundle(), where the name the document's provider reports wins over the picker's hint.
     *
     * @return beginImport()'s result. error "empty" means the file parsed but no row was usable — the row is kept, so
     *         the user can fix the file and update, but the UI should warn.
     */
    Q_INVOKABLE QVariantMap importFile(const QString& reference, const QString& fileName, const QString& type);
    Q_INVOKABLE QVariantMap importBundle(const QString& reference, const QString& fileName, const QString& type,
                                         const QVariantMap& companions, int replaceRow = -1);

    /**
     * @brief Re-pick flow: replace the row's stored file and re-validate, as beginImport() with a replacePath, run at
     * once on the calling thread.
     *
     * The pick is staged outside the library and validated BEFORE it is committed
     * over the row's file, so a file that is not parseable as the row's type
     * leaves the stored copy byte-identical. Every saved system referencing that
     * path keeps working.
     */
    Q_INVOKABLE QVariantMap updateFile(int row, const QString& reference, const QString& fileName);

    /**
     * @brief Adopt a file this process generated, recording where it came from.
     *
     * Same stage and commit as importFile(), but the source is a plain path we
     * wrote rather than a picker reference, and @p fileName names the copy.
     *
     * @param sourcePath Absolute path of the generated file.
     * @param fileName   Display name to store it under.
     * @param type       "chan" | "group" | "keysDec" | "keysHex" | "p25Bandplan" | "src".
     * @param origin     Provenance: {origin, rrSid, rrSiteIds, rrKind, rrPartialEnc}.
     * @return Same shape as importFile().
     */
    Q_INVOKABLE QVariantMap importGeneratedFile(const QString& sourcePath, const QString& fileName, const QString& type,
                                                const QVariantMap& origin);

    /**
     * @brief Replace a generated row's file in place, keeping its stored path.
     *
     * The path is preserved so saved systems pointing at it stay valid, and the
     * staging file is validated before the stored copy is touched: a refresh that
     * fetched a fault page or a truncated body must not destroy working local
     * data.
     *
     * @param row        Library row.
     * @param sourcePath Absolute path of the freshly generated file.
     * @return Same shape as importFile().
     */
    Q_INVOKABLE QVariantMap refreshGeneratedFile(int row, const QString& sourcePath);

    Q_INVOKABLE QVariantMap channelProfiles(int row) const;
    /**
     * @brief Delete the stored file and its bundle folder, then the row. Persists immediately.
     *
     * Returns false, keeping the row, when removal is refused (canRemove()), the file will not delete, or the index
     * cannot be saved; calling it again finishes the removal, and a file already gone counts as deleted. An index save
     * that fails after the delete leaves the row listed, naming a missing file, until a retry or the next load prunes
     * it (the startup cleanup then clears what names it). The bundle folder's delete is best effort: a failure there
     * is neither reported nor retried, and the row goes once the file has. References to the path are the caller's
     * to clear, before this call.
     */
    Q_INVOKABLE bool remove(int row);
    Q_INVOKABLE bool canRemove(int row) const;
    Q_INVOKABLE QVariantMap targetPreview(const QString& path) const;

    /** @brief One library row as a field map. */
    Q_INVOKABLE QVariantMap get(int row) const;

    /** @brief Rows of one type, for the wizard's picker sheets. */
    Q_INVOKABLE QVariantList entriesForType(const QString& type) const;

    /** @brief Row index whose stored path matches, or -1. */
    Q_INVOKABLE int rowForPath(const QString& path) const;

    /**
     * @brief Stored paths dropped by load() because the file was gone, then forgets them.
     *
     * Pruning a ghost row is only half the repair: saved systems reference stored
     * paths, and one left pointing at a deleted file makes the session fail to
     * start on `-G <missing>` with a parse error that names the input settings
     * rather than the CSV. The owner has to hand these to
     * SavedSystemsModel::clearCsvPath(), which is what the library's own remove
     * flow does. A signal cannot carry them: load() runs from the constructor,
     * before anything could have connected to one.
     */
    Q_INVOKABLE QStringList takePrunedPaths();

  Q_SIGNALS:
    void countChanged();
    /** Every library change: a row added, removed or replaced. */
    void entriesChanged();
    void importingChanged();
    void importFinished(int id, const QVariantMap& result);

  private:
    /** What a request imports into: a new row of a type, or the row a replace began against (its commit needs that row
     * still there, unchanged). */
    struct ImportTarget {
        QString path; // empty for a new row
        quint64 revision = 0;
        QString type;
        /* A RadioReference refresh: it keeps the row's provenance, and the decoder running does not hold it back. */
        bool refresh = false;
        /* A channel map or target list: built with its companions, as a bundle. */
        bool bundle = false;
    };

    QVariantMap companionDetails(const QVariantMap& requiredDetails) const;
    QVariantMap companionChoices(const QVariantMap& requirement) const;
    /** What a bundle stage needs from the library, taken on the Qt thread: its rows' types and the replaced row. */
    QVariantMap bundleRequest(const QString& reference, const QString& fileName, const QString& type,
                              const QVariantMap& companions, const ImportTarget& target) const;
    bool decoderBusy() const;
    /** Whether replacing a row of @p type must wait: the running engine writes talkgroup lists, reads target lists. */
    bool replacementBusy(const QString& type) const;
    /** beginImport()'s answer when it refuses at once, else empty with @p target filled. */
    QVariantMap importRefusal(const QString& type, const QString& replacePath, ImportTarget* target) const;
    /** Answer a request at once, through a queued importFinished(), under a fresh id. */
    int answerLater(const QVariantMap& result);
    /** Run a single-file stage and its commit at once on the calling thread; the commit's result. */
    QVariantMap importNow(const QString& reference, const QString& providerName, const QString& hint,
                          const QString& type, const ImportTarget& target, const QVariantMap& origin);
    /** Run a bundle's stage and its commit at once on the calling thread; the commit's result. */
    QVariantMap importBundleNow(const QVariantMap& request, const ImportTarget& target);
    void finishImport(qint64 id, const QVariantMap& staged, bool cancelled, const ImportTarget& target);
    QVariantMap commitStaged(const QVariantMap& staged, bool cancelled, const ImportTarget& target,
                             const QVariantMap& origin);
    QVariantMap commitNewFile(const QVariantMap& staged, const QVariantMap& origin);
    QVariantMap commitReplacement(const QVariantMap& staged, const ImportTarget& target);
    /** The row @p target began against, or -1 when it is gone or changed since. */
    int currentRow(const ImportTarget& target) const;
    /** A bundle's commit: its new directories journaled, made and filled, then the primary published or replaced. */
    QVariantMap commitBundle(const QVariantMap& staged, const ImportTarget& target);
    /** Record a new bundle whose directories @p created are filled: publish the primary, then save the row. */
    QVariantMap publishBundle(const QVariantMap& staged, const QStringList& created);
    QVariantMap replaceBundleRow(int row, const QVariantMap& staged, const QStringList& created);
    /**
     * Replace row @p row's file with the staged copy: the index saved first, then the file moved over. @p created are
     * the journaled directories the commit made for it: removed when nothing was published, dropped from the journal
     * once the file is in place, and left journaled when neither the move nor the rollback could be saved.
     */
    QVariantMap replaceRowFile(int row, const QVariantMap& staged, bool refresh, const QStringList& created = {});
    /** Reclaim @p bundles' unreached revisions on the import worker; nothing while the decoder runs. */
    void reclaimRevisions(const QList<BundleRevisions>& bundles);
    void updateImporting(bool before);

    struct Row {
        QString name;
        QString path;
        QString type;
        qint64 importedAt = 0;
        int accepted = 0;
        int skipped = 0;
        /* Provenance, absent on a picked file. Stores written before this existed
         * load unchanged: rowFromMap reads through QVariantMap::value, which
         * default-constructs a missing key. */
        QString origin;
        int rrSid = 0;
        QString rrKind;
        /* Every selected site as TrsSite.siteId, comma-joined in selection order
         * — a conventional import selects several repeaters, and a refresh driven
         * by only the first would silently shrink the scan list to one row.
         * siteId and never the RF site number: the number is NOT unique within a
         * system (the captured SARA network numbers its 35 sites
         * 1,1,10,10,10,10,10,20,…), so matching by it can regenerate from the
         * wrong tower. */
        QString rrSiteIds;
        /* The answer the original import was given, so a refresh reproduces it
         * rather than substituting the UI default. Without it a user who turned
         * this OFF gets every partly-encrypted talkgroup silently re-marked DE
         * — blocked from tuning — the first time they refresh. Defaults to true,
         * which is what a row written before this existed was generated with. */
        bool rrPartialEnc = true;
        int rrEncryptionPolicy = 0;
        QString bundleRoot;
        /* In memory only: the library revision of the row's last change (added, replaced, refreshed). A replace
         * commits only over the revision it began against. */
        quint64 revision = 0;
    };

    static Row rowFromMap(const QVariantMap& map);
    static QVariantMap mapFromRow(const Row& row);

    /**
     * @brief The RadioReference provenance roles.
     *
     * Split out of data() only to keep that switch under the project's
     * complexity ceiling; the roles have no behaviour of their own.
     */
    static QVariant provenanceRole(const Row& row, int role);

    /** @brief Append @p row, saved first: false, with nothing changed, when the index cannot be saved. */
    bool appendRow(Row row);
    /** @brief Take @p next, which replaced row @p row, as the library, and notify. */
    void applyReplacement(int row, QList<Row> next);
    static Row newRow(const QString& path, const QString& type, int accepted, int skipped, const QVariantMap& origin);
    /** @brief A successful import's result for @p row. */
    static QVariantMap rowResult(const Row& row);

    void load();
    /**
     * @brief Settle the commit journal a crash left (load() only, before the prune): remove each journaled directory
     * that @p rows do not reference (a row's bundle root, or a revision its primary reaches) and that lies under
     * imports/bundles/ with an owned name. A journal that cannot be read deletes nothing and is set aside.
     */
    static void recoverJournal(const QList<Row>& rows);
    bool save() const;
    bool saveRows(const QList<Row>& rows) const;

    DecoderHost* m_host = nullptr;
    QPointer<ImportService> m_imports;
    QList<Row> m_rows;
    QStringList m_pruned_paths;
    /* The library revision: every change takes the next one. */
    quint64 m_revision = 0;
    /* Imports begun through the worker whose commit has not run yet. */
    QSet<qint64> m_inFlight;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_IMPORTED_FILES_MODEL_H_ */
