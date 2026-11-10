// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_CSV_BUNDLE_IMPORT_H
#define DSD_NEO_UI_QT_CSV_BUNDLE_IMPORT_H
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <functional>

template <class T>
class QSet;

namespace dsd_qt {

/** The most a bundle may copy in all, its primary and every companion together (#832). */
constexpr qint64 kMaxBundleImportBytes = 64LL * 1024 * 1024;

/** One document a bundle stage copied into its staging folder. */
struct ImportFetchResult {
    /** A DecoderHost::DocumentCopy value; only DocumentCopied leaves a file at @p path. */
    int code = 0;
    /** The copy, a fresh file in the request's staging folder. */
    QString path;
    /** The document's name: its provider's, else the hint (sanitized_import_name()). */
    QString name;
    qint64 bytes = 0;
    /** The document's size as its source declared it; -1 when unknown. */
    qint64 size = -1;
    /** DocumentLocalFailed because this device ran out of space. */
    bool noSpace = false;
};

/** Copy the document @p reference names, at most @p maxBytes of it, on the import worker. */
using ImportFetch =
    std::function<ImportFetchResult(const QString& reference, const QString& nameHint, qint64 maxBytes)>;

/**
 * Where a bundle stage builds its files: a mirror of the library (imports/) inside the request's staging folder, so
 * every relative reference resolves there exactly as it will once the files are moved into the library.
 */
struct BundleTarget {
    /** The mirror of imports/, in the request's staging folder. */
    QString mirrorRoot;
    /** The primary's final path relative to imports/ for a replace; empty for a new import (the stage chooses). */
    QString primaryRelative;
    /** The replaced row's bundle root relative to imports/ ("bundles/<id>"), or empty. */
    QString existingRootRelative;
    /** The bytes the bundle may copy in all; each copy is capped at min(32 MiB, what is left). */
    qint64 budget = kMaxBundleImportBytes;
    /**
     * Companions found by path stay in the primary's folder tree (issue #830, where DSD-neo browses the whole device):
     * a companion the CSV names is taken only by a relative name, as a regular file whose real path lies in the local
     * primary's real folder or below it, or, for a stored revision, in the replaced bundle's root. An absolute name, a
     * `../` or a link out of it stays required. Companions chosen explicitly are taken as they are. False: any file the
     * CSV names is found, as on the desktop.
     */
    bool confineLocalCompanions = false;
};

struct CsvBundleImport {
    QString name;
    /** "" on success, else too_large (limitBytes), read, local (noSpace), format (detail), companions (required),
     * path (a bundle root the app does not own) or cancelled. */
    QString error;
    QString detail;
    QStringList required;
    QVariantMap requiredLabels;
    QVariantMap requiredDetails; // opaque identity -> {name, type, selectionError?}; model adds library choices
    /** The primary's final path relative to imports/; the staged primary is mirrorRoot/primaryRelative. A new import
     * without companions is a plain file: it has no root, and the commit chooses its stored name. */
    QString primaryRelative;
    /** The bundle root relative to imports/, empty without companions. */
    QString rootRelative;
    /** The new revision directory relative to imports/, empty without companions. */
    QString revisionRelative;
    /** Every file the stage wrote in the mirror, relative to it: the companions, then the primary. */
    QStringList stagedRelative;
    /** The name the user knows each of them by (the picked document's), keyed by its stagedRelative entry. */
    QMap<QString, QString> stagedNames;
    /** Whether rootRelative is a new root (an existing root only gains a revision). */
    bool newRoot = false;
    qint64 copiedBytes = 0;
    qint64 limitBytes = 0;
    bool noSpace = false;
};

/** Stage a channel map and explicitly selected companion documents in @p target's mirror.
 * libraryEntries maps canonical stored paths to {name, type} metadata. */
CsvBundleImport stage_csv_bundle(const ImportFetch& fetch, const BundleTarget& target, const QString& reference,
                                 const QString& name, const QVariantMap& companions,
                                 const QVariantMap& libraryEntries = {});
CsvBundleImport stage_trunk_csv_bundle(const ImportFetch& fetch, const BundleTarget& target, const QString& reference,
                                       const QString& name, const QVariantMap& companions,
                                       const QVariantMap& libraryEntries = {});
/** User-facing role names and a diagnostic for an explicitly selected library type mismatch. */
QString csv_companion_type_name(const QString& type);
QString csv_companion_type_error(const QString& name, const QString& actual, const QString& expected);
/** Every companion file a channel map's key columns and option cells name, as written, each once. */
QStringList csv_companion_references(const QString& text);

/** Whether @p dir is a bundle root the app owns: imports/bundles/<owned name>, a real directory, no link. */
bool owned_bundle_root(const QString& dir);
/** Whether @p dir is a revision directory of an owned bundle root: <root>/<uuid>, a real directory, no link. */
bool owned_bundle_revision(const QString& dir);
/** Remove the owned bundle root @p root and all it holds; anything else is left alone. */
void remove_bundle_root(const QString& root);

/**
 * The files below @p root that @p path, a library row's primary of @p type ("chan" or "trunkTargets"), reaches, as
 * clean absolute paths spelled from @p path. False when the primary cannot be parsed: a caller then treats every file
 * as reached.
 */
bool bundle_reached_files(const QString& path, const QString& root, const QString& type, QSet<QString>* files);

/**
 * The revision directories (names) below @p root that @p path, a library row's primary of @p type ("chan" or
 * "trunkTargets"), reaches. False when the primary cannot be parsed: a caller then treats every revision as used.
 */
bool bundle_reached_revisions(const QString& path, const QString& root, const QString& type, QSet<QString>* names);

/**
 * Whether @p dir holds (at any depth) a file of @p named, the paths kept_library_files() lists; @p dir is compared both
 * as the file system resolves it and as spelled. A directory holding one is kept by every cleanup, since a saved
 * system, another store or another library row still needs it.
 */
bool holds_named_file(const QString& dir, const QStringList& named);

/** One library row's stored file and its type. */
struct LibraryFile {
    QString path;
    QString type;
};

/**
 * The files no cleanup may delete, canonical (through every link; a file that is gone by its clean absolute path): those
 * a typed path field names (backup::typed_file_paths())
 * and those the stored files of @p library reach (a channel map's key columns and option cells; a target list's channel
 * maps and theirs), whichever row or bundle they lie in. Rows of other types reach nothing, nor does a stored file that
 * is gone. False, with @p kept untouched, when a store or a stored file cannot be read or parsed: it may need anything,
 * so a caller deletes nothing.
 */
bool kept_library_files(const QList<LibraryFile>& library, QStringList* kept);

/**
 * The files the stored files of @p library reach, as kept_library_files() resolves them, without the typed path
 * fields. False, with @p reached untouched, when a stored file cannot be read or parsed.
 */
bool library_reached_files(const QList<LibraryFile>& library, QStringList* reached);

/** One registered bundle whose old revisions a cleanup may reclaim. */
struct BundleRevisions {
    QString path;
    QString root;
    QString type;
};

/**
 * Reclaim the revision directories of @p bundles that their primaries no longer reach (import worker only).
 *
 * Only owned, registered roots; never through a link. Kept: every revision when a primary cannot be parsed, every
 * revision a commit in flight is filling (@p journal lists it; read after the directories are listed and before the
 * primary is parsed, so a commit that finished in between is seen in the primary), and every revision holding a file
 * kept_library_files() keeps for @p library, the whole library. Nothing is removed when the journal, a store or a
 * stored file of @p library cannot be read.
 */
void clean_bundle_revisions(const QList<BundleRevisions>& bundles, const QList<LibraryFile>& library,
                            const std::function<QStringList(bool* ok)>& journal);
} // namespace dsd_qt
#endif
