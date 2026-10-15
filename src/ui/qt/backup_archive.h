// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The plaintext archive a configuration backup seals (issue #580), and its validation.
 *
 * Layout, inside the sealed envelope and not compressed: ASCII "DNBA", the manifest length as a big-endian u32, the
 * manifest JSON (UTF-8), then the bytes of every carried file concatenated in manifest order. The manifest holds the
 * format and version, the app tag, the creation time, the data roots the paths were exported from, the portable
 * settings, the five JSON stores, the signal-search avoid list, the export's "not included" list and the file table
 * (path, size, sha256).
 *
 * A restored archive is untrusted: anyone can hand a user a crafted backup and its passphrase. backup_parse_archive()
 * therefore validates everything before anything is written, and the stores it returns name only files the archive
 * carries, under the restoring root's imports/ directory. backup_build_archive() sanitizes its copy of the stores so
 * every backup passes that same validation, and checks that it does before returning.
 *
 * Only backup_take_snapshot() touches app state (settings, stores, the in-memory avoid list) and must run on the UI
 * thread. Every other function is free of shared state and safe to call from a worker thread.
 */

#ifndef DSD_NEO_SRC_UI_QT_BACKUP_ARCHIVE_H_
#define DSD_NEO_SRC_UI_QT_BACKUP_ARCHIVE_H_

#include <QByteArray>
#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <atomic>
#include <dsd-neo/crypto/backup_seal.h>
#include <functional>

namespace dsd_qt {
namespace backup {

constexpr int kFormatVersion = 1;
/** Largest sealed backup a restore reads. An archive must fit with the envelope's header and tag. */
constexpr qint64 kMaxSealedBytes = 64LL * 1024 * 1024;
constexpr int kMaxFiles = 4096;
constexpr qint64 kMaxFileBytes = 32LL * 1024 * 1024;
constexpr int kMaxRowsPerStore = 4096;
constexpr qint64 kMaxManifestBytes = 4LL * 1024 * 1024;
constexpr int kMinPassphraseChars = 8;
/** Largest archive: a sealed file of kMaxSealedBytes minus the envelope's header and tag. */
constexpr qint64 kMaxArchiveBytes = kMaxSealedBytes - DSD_BACKUP_HEADER_BYTES - DSD_BACKUP_TAG_BYTES;

constexpr const char kSavedSystemsStore[] = "saved_systems.json";
constexpr const char kScanListsStore[] = "scan_lists.json";
constexpr const char kImportedFilesStore[] = "imported_files.json";
constexpr const char kDecryptionProfilesStore[] = "decryption_profiles.json";
constexpr const char kSignalSearchRangesStore[] = "signal-search-ranges.json";
/** The native signal-search avoid list, beside the stores (signal_search_store.cpp writes it). */
constexpr const char kAvoidsFileName[] = "signal-search-history.v1.avoids.json";

/** The five JSON stores a backup carries, in manifest order. */
QStringList backup_store_names();

/** Why a backup step failed. Shared by the export, restore staging, apply and model layers. */
enum class BackupError {
    None,
    NotBackup,
    NewerVersion,
    WrongPassphrase,
    TooLarge,
    Damaged,
    NoSpace,
    SessionActive,
    Io,
    /** A carried file changed while the export read it (a session appends to its talkgroup lists, say). */
    SourceChanged,
    /** A restore could not be prepared on this phone (its cache, staging or promotion failed), whatever the file. */
    StagingFailed
};

/** The enumerator's name ("None", "NotBackup", …), for result maps handed to QML. */
QString backup_error_name(BackupError error);

/**
 * The BackupError for a ::dsd_backup_status: FORMAT → NotBackup, VERSION → NewerVersion, AUTH → WrongPassphrase,
 * TOO_LARGE → TooLarge, PARAMS → Damaged (a header this build cannot open is not something the user can fix by
 * retyping), ARGS and INTERNAL → Io.
 */
BackupError backup_error_from_seal_status(int status);

/**
 * The BackupError a restore reports for a backup_open() status: backup_error_from_seal_status(), except ARGS and
 * INTERNAL. The open reaches those only once the header has parsed, so they are this phone's crypto library or memory
 * failing, not the file: StagingFailed. (An empty passphrase, the other ARGS, is the caller's to tell apart.)
 */
BackupError backup_error_from_open_status(int status);

/** What a typed path field holds. */
enum class PathKind {
    File,       /**< A carried file under the data root's imports/, or empty. */
    Recording,  /**< A replay recording: never carried, always exported and restored empty. */
    BundleRoot, /**< An imported bundle's directory, imports/bundles/<id>, or empty. */
};

/** One store field that names a filesystem path. */
struct TypedPathField {
    const char* store;
    const char* field;
    PathKind kind;
};

/** Every typed path field of every store. The one table export, restore validation and apply share. */
QList<TypedPathField> typed_path_fields();

/** One present, non-empty string value of a typed path field. */
struct TypedPathValue {
    int row = 0;
    TypedPathField field{nullptr, nullptr, PathKind::File};
    QString value;
};

/** The typed path values in @p rows of @p store (rows that are not objects and non-string values are skipped). */
QList<TypedPathValue> typed_path_values(const QString& store, const QJsonArray& rows);

/** @p rows with every typed path value (as typed_path_values() lists them) replaced by what @p map returns for it. */
QJsonArray map_typed_path_values(const QString& store, const QJsonArray& rows,
                                 const std::function<QString(const TypedPathValue&)>& map);

/** Counts shown before a restore and after an export. */
struct Summary {
    int systems = 0;
    int scanLists = 0;
    int libraryFiles = 0;
    int profiles = 0;
    int ranges = 0;
    int avoids = 0;
    /** Replay systems: a backup never carries recordings, so each needs its recording chosen again. */
    int recordingsToChoose = 0;
    /** Saved systems that pass decoder options of their own: extra arguments, or a decode flag with a token the app
        itself never writes (decode_flag_is_standard()), since both are split into the decoder's arguments. */
    int systemsWithExtraArgs = 0;
    /** Whether the app-wide extra decoder options are not empty. */
    bool appExtraArgs = false;
    /** Display lines for references the export could not carry. */
    QStringList notIncluded;
    qint64 createdAt = 0;
    QString app;
};

/** @p summary as a QVariantMap with the field names above. */
QVariantMap summary_to_map(const Summary& summary);

/** Everything a backup is built from, read on the UI thread. */
struct Snapshot {
    QString root;          /**< json_store_root(). */
    QString canonicalRoot; /**< The root with symlinks resolved; empty when it does not exist. */
    QVariantMap prefs;     /**< AppPrefs::readPortable(). */
    QMap<QString, QJsonArray> stores;
    QList<quint32> avoids; /**< The in-memory permanent avoid list. */
    QString app;           /**< GIT_TAG. */
    qint64 createdAt = 0;  /**< realtimeSecsSinceEpoch(). */
};

/** Read the current configuration. UI thread only. */
Snapshot backup_take_snapshot();

struct BuildResult {
    BackupError error = BackupError::None;
    /** Set when @c cancel was raised; nothing else is meaningful then. */
    bool cancelled = false;
    /** Holds key material in the clear: seal it, then wipe it with DSD_SECURE_ZERO. */
    QByteArray archive;
    Summary summary;
    /** Why a build failed, for logs; never key material. */
    QString detail;
};

/**
 * Build the archive for @p snapshot. Worker-safe: it reads only the filesystem. Each carried file is read twice
 * (sized and hashed, then copied straight into the archive and checked again), so the archive is the only large
 * buffer it holds.
 *
 * The snapshot's stores are copied and sanitized: a typed File value that is not a regular, non-symlink file whose
 * real path lies inside the root's imports/ (and is not a staging leftover) is cleared and listed in notIncluded; a
 * library row whose file is not carried is dropped and listed; a bundle root that is not owned is cleared and its
 * row's file carried alone, while an owned one carries its whole subtree (regular, non-symlink files only); every
 * Recording value is cleared; profiles lose their compiled form and runtime references; rows the models' loaders
 * would drop are dropped and identities they would regenerate are regenerated. A file no kept value names and no kept
 * row's bundle holds is not carried. Carried channel maps that name companion files by absolute path are listed too.
 * Exceeding a cap fails with TooLarge.
 *
 * @param cancel Optional; checked between files. A raised flag ends the build with BuildResult::cancelled set.
 */
BuildResult backup_build_archive(const Snapshot& snapshot, const std::atomic<bool>* cancel = nullptr);

/** One carried file inside an archive. */
struct ArchiveFile {
    QString path; /**< Relative archive path, "imports/…". */
    qint64 offset = 0;
    qint64 size = 0;
    QString sha256; /**< Lower-case hex. */
};

struct ParseResult {
    BackupError error = BackupError::None;
    /** The stores present in the archive, rebased onto the target root, by file name. Profiles carry definitions
        only; signal-search ranges are in the model's stored shape. */
    QMap<QString, QJsonArray> stores;
    /** Portable settings, unknown keys dropped. */
    QVariantMap prefs;
    QList<quint32> avoids;
    QList<ArchiveFile> files;
    Summary summary;
    /** Why a parse failed, prefixed with the rule's area ("archive:", "manifest:", "files:", "path:", "store:",
        "prefs:", "avoids:", "typed:"); for logs and tests, never shown as is. */
    QString detail;
};

/**
 * Validate @p archive and rebase it onto @p targetRoot, before anything is written. Pure: it reads no files.
 *
 * Any failure returns only an error and a detail. On success every File value of the returned stores is empty or
 * names a carried file (targetRoot + "/" + path); every bundle root is empty or an owned imports/bundles/<id> that
 * holds carried files and that no other row's root or file lies in; a row's file under imports/bundles/ lies in its
 * own bundle (a channel map updated in place keeps its file in imports/, beside its bundle); every Recording is
 * empty; and every carried file is named exactly by a File value or lies, exactly, inside a claimed bundle root - the
 * files a later restore counts as this configuration and replaces. @p targetRoot must be a clean absolute path (else
 * Io).
 */
ParseResult backup_parse_archive(const QByteArray& archive, const QString& targetRoot);

/** The passphrase as NFC-normalized UTF-8; the caller wipes it with DSD_SECURE_ZERO. */
QByteArray backup_passphrase_bytes(const QString& passphrase);

/** Whether @p passphrase has at least kMinPassphraseChars code points after NFC normalization. */
bool backup_passphrase_ok(const QString& passphrase);

/**
 * Seal @p archive under @p passphrase. Returns the sealed bytes, or empty on failure; @p status (optional) receives
 * the ::dsd_backup_status. The passphrase bytes are wiped before returning.
 */
QByteArray backup_seal(const QByteArray& archive, const QString& passphrase, const dsd_backup_kdf_params& kdf,
                       int* status);

/**
 * Open @p sealed with @p passphrase. Returns the archive, or empty on failure (any partial output wiped); @p status
 * (optional) receives the ::dsd_backup_status. The passphrase bytes are wiped before returning.
 */
QByteArray backup_open(const QByteArray& sealed, const QString& passphrase, int* status);

} // namespace backup
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_BACKUP_ARCHIVE_H_ */
