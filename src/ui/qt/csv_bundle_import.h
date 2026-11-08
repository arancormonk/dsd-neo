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
 * a typed path field names. Nothing is removed when the journal or a store cannot be read.
 */
void clean_bundle_revisions(const QList<BundleRevisions>& bundles, const std::function<QStringList(bool* ok)>& journal);
} // namespace dsd_qt
#endif
