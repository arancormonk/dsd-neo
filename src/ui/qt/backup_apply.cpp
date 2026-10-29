// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Applying a committed restore at the start of ui_load() (issue #580). The undo journal is written before the first
 * change and the roll forward reads only the journal and the staging, so a crash anywhere re-runs to the same end
 * state; an I/O failure rolls back from the journal. Staging lives in backup_stage.cpp. */

#include "backup_apply.h"

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/signal_search.h>
#include <iterator>
#include <qflags.h>
#include <stdint.h>
#include <utility>
#include "app_prefs.h"
#include "backup_apply_fs.h"
#include "backup_archive.h"
#include "backup_archive_rules.h"

namespace dsd_qt {
namespace backup {

namespace {

using namespace detail;

constexpr int kJournalVersion = 1;
constexpr qint64 kSpaceMargin = 1024LL * 1024;
constexpr qint64 kMaxStagedJson = kMaxSealedBytes;
constexpr int kMaxFreeNames = 10000;

QString
nothing_changed() {
    return QCoreApplication::translate("BackupApply", "Nothing was changed");
}

QString
stuck_reason() {
    return QCoreApplication::translate("BackupApply", "Restore couldn't finish. Free up storage and reopen DSD-neo");
}

QString
busy_reason() {
    return QCoreApplication::translate("BackupApply", "The decoder was busy, so the restore waits for the next start");
}

QString
names_reason() {
    return QCoreApplication::translate("BackupApply", "No free name was left for a restored file. Nothing was changed");
}

QString
unreadable_reason() {
    return QCoreApplication::translate("BackupApply",
                                       "A file of the current configuration couldn't be read. Nothing was changed");
}

QString
leftover_reason() {
    return QCoreApplication::translate(
        "BackupApply", "Files left by an earlier restore attempt couldn't be removed. Nothing was changed");
}

QString
bundle_reason() {
    return QCoreApplication::translate(
        "BackupApply", "A folder left by an earlier import is in the way of a restored channel map. To restore this "
                       "backup, clear DSD-neo's storage first. Nothing was changed");
}

QString
space_reason() {
    return QCoreApplication::translate("BackupApply",
                                       "Not enough free storage to restore. Free up storage and reopen DSD-neo");
}

enum class Status { Ok, Failed, Crashed };

/** Why a journal could not be built: storage (the next start tries again) or a cause retrying cannot fix. */
enum class JournalProblem { Space, Names, Unreadable, Leftover, BundleInTheWay };

/** One file the staging or the journal lists: a path relative to its directory, its size and sha256. */
struct Listed {
    QString path;
    qint64 size = 0;
    QString sha256;
};

/** The verified staging: the carried files and what the stores, settings and avoid list become. */
struct Staging {
    QStringList carried;               // archive paths, manifest order
    QMap<QString, QString> carriedSha; // archive path → sha256
    QMap<QString, QJsonArray> stores;
    QVariantMap prefs;
    QList<quint32> avoids;
    QVariantMap summary;
    qint64 bytes = 0;
};

/** The undo journal: the old configuration, and where every staged file goes. */
struct Journal {
    QList<Listed> files;                 // old managed files, relative to the root
    QStringList stores;                  // which of the six store files existed
    QStringList ownedRoots;              // old owned bundle roots
    QMap<QString, QString> destinations; // archive path → destination, for every staged file
    QMap<QString, QString> bundles;      // staged bundle root → the fresh one it was moved to
    QStringList createdDirs;             // directories the roll forward may create
    QVariantMap prefs;
    QString prefsSha256; // of journal/prefs.json, so a rollback never restores settings it cannot trust
};

QStringList
six_store_files() {
    return backup_store_names() << QLatin1String(kAvoidsFileName);
}

/** A relative path a journal or staging may list: clean, not absolute, no '..'. */
bool
clean_relative(const QString& path) {
    return !path.isEmpty() && !path.startsWith(QLatin1Char('/')) && QDir::cleanPath(path) == path
           && !path.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

QByteArray
json_bytes(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject
listed_json(const Listed& file) {
    return QJsonObject{{QStringLiteral("path"), file.path},
                       {QStringLiteral("size"), file.size},
                       {QStringLiteral("sha256"), file.sha256}};
}

bool
listed_from(const QJsonValue& value, Listed* file) {
    const QJsonObject entry = value.toObject();
    const QJsonValue size = entry.value(QLatin1String("size"));
    file->path = entry.value(QLatin1String("path")).toString();
    file->size = size.isDouble() ? size.toInteger(-1) : -1;
    file->sha256 = entry.value(QLatin1String("sha256")).toString();
    return clean_relative(file->path) && file->size >= 0 && is_sha256_hex(file->sha256);
}

QStringList
string_list(const QJsonValue& value) {
    QStringList out;
    for (const auto& item : value.toArray()) {
        out.append(item.toString());
    }
    return out;
}

QJsonObject
map_json(const QMap<QString, QString>& map) {
    QJsonObject object;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return object;
}

QMap<QString, QString>
json_map(const QJsonValue& value) {
    QMap<QString, QString> map;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        map.insert(it.key(), it.value().toString());
    }
    return map;
}

/** The directories between the root and @p path, outermost first. */
QStringList
ancestors(const QString& path) {
    QStringList out;
    for (qsizetype at = path.indexOf(QLatin1Char('/')); at > 0; at = path.indexOf(QLatin1Char('/'), at + 1)) {
        out.append(path.left(at));
    }
    return out;
}

/**
 * Whether @p relative names a file the app generates, the only files of decryption/ and scan_lists/ that count as
 * configuration: a compiled profile's key file, decryption/<uid>/<uuid>.(hex|dec|map).csv (DecryptionProfilesModel's
 * ProfileCompiler, a uid its validId() accepts), or a scan list's CSV, scan_lists/<uid>.csv (ScanListStarter, the same
 * uid rule). Anything else there is someone else's and survives a restore.
 */
bool
generated_file(const QString& relative) {
    static const QRegularExpression pattern(
        QStringLiteral("^(decryption/[A-Za-z0-9_-]{1,63}/"
                       "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(hex|dec|map)\\.csv"
                       "|scan_lists/[A-Za-z0-9_-]{1,63}\\.csv)$"));
    return pattern.match(relative).hasMatch();
}

/** The native avoid store's own format (signal_search_store.cpp save_avoids()): "[a,b]\n", "[]\n" when empty. */
QByteArray
native_avoids(const QList<quint32>& avoids) {
    QByteArray out("[");
    for (qsizetype i = 0; i < avoids.size(); ++i) {
        if (i != 0) {
            out.append(',');
        }
        out.append(QByteArray::number(avoids.at(i)));
    }
    out.append("]\n");
    return out;
}

/** Reads and checks the staging against its marker and manifest. */
class StagingReader {
  public:
    StagingReader(const QString& dir, Staging* staging) : m_dir(dir), m_staging(staging) {}

    bool
    read() {
        QByteArray manifest, marker;
        if (!read_file(m_dir + QLatin1Char('/') + QLatin1String(kManifestName), kMaxStagingManifestBytes, &manifest)
            || !read_file(m_dir + QLatin1Char('/') + QLatin1String(kCommittedName), 256, &marker)
            || QString::fromLatin1(marker.trimmed()) != sha256_hex(manifest)) {
            return false;
        }
        QJsonParseError error{};
        const QJsonObject object = QJsonDocument::fromJson(manifest, &error).object();
        const QJsonValue version = object.value(QLatin1String("stagingVersion"));
        if (error.error != QJsonParseError::NoError || !version.isDouble() || version.toInteger(-1) != kStagingVersion
            || !object.value(QLatin1String("summary")).isObject() || !object.value(QLatin1String("files")).isArray()) {
            return false;
        }
        m_staging->summary = object.value(QLatin1String("summary")).toObject().toVariantMap();
        const QJsonArray files = object.value(QLatin1String("files")).toArray();
        return std::all_of(files.cbegin(), files.cend(), [this](const QJsonValue& v) { return readEntry(v); })
               && m_seen.contains(QStringLiteral("prefs.json")) && m_seen.contains(QStringLiteral("avoids.json"));
    }

  private:
    bool
    readEntry(const QJsonValue& value) {
        Listed file;
        if (!listed_from(value, &file) || m_seen.contains(file.path)) {
            return false;
        }
        m_seen.insert(file.path);
        m_staging->bytes += file.size;
        const QString path = m_dir + QLatin1Char('/') + file.path;
        if (file.path.startsWith(QLatin1String("files/"))) {
            const QString carried = file.path.mid(6);
            QString sha;
            qint64 size = -1;
            if (!archive_path_problem(carried).isEmpty() || !hash_file(path, &sha, &size) || size != file.size
                || sha != file.sha256) {
                return false;
            }
            m_staging->carried.append(carried);
            m_staging->carriedSha.insert(carried, sha);
            return true;
        }
        QByteArray bytes;
        const bool ok = read_file(path, kMaxStagedJson, &bytes) && bytes.size() == file.size
                        && sha256_hex(bytes) == file.sha256 && readJson(file.path, bytes);
        wipe(bytes); // stores hold keys
        return ok;
    }

    bool
    readJson(const QString& name, const QByteArray& bytes) {
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError) {
            return false;
        }
        if (name == QLatin1String("prefs.json")) {
            m_staging->prefs = document.object().toVariantMap();
            return document.isObject();
        }
        if (name == QLatin1String("avoids.json")) {
            return document.isArray() && readAvoids(document.array());
        }
        const QString store = name.mid(7);
        if (!name.startsWith(QLatin1String("stores/")) || !backup_store_names().contains(store)
            || !document.isArray()) {
            return false;
        }
        m_staging->stores.insert(store, document.array());
        return true;
    }

    bool
    readAvoids(const QJsonArray& list) {
        if (list.size() > DSD_SEARCH_MAX_AVOIDS) {
            return false;
        }
        for (const auto& value : list) {
            const qint64 frequency = value.isDouble() ? value.toInteger(-1) : -1;
            if (frequency < 1 || frequency > static_cast<qint64>(UINT32_MAX)) {
                return false;
            }
            m_staging->avoids.append(static_cast<quint32>(frequency));
        }
        return true;
    }

    QString m_dir;
    Staging* m_staging;
    QSet<QString> m_seen;
};

/**
 * The old configuration, positively identified from the live stores: the six store files, every regular file under
 * imports/ a File value names, every owned bundle root a library row claims (first claim of a folded name only), and
 * the files under decryption/ and scan_lists/ the app generates (generated_file()). Nothing reached through a symlink
 * counts.
 */
class LiveScan {
  public:
    LiveScan(const DataRoot& root, Journal* journal)
        : m_root(root), m_canonical(root.canonical()), m_journal(journal) {}

    qint64
    run() {
        if (m_canonical.isEmpty()) {
            return 0;
        }
        for (const QString& name : six_store_files()) {
            if (add(name)) {
                m_journal->stores.append(name);
            }
        }
        for (const QString& store : backup_store_names()) {
            scanStore(store);
        }
        addTree(QStringLiteral("decryption"), generated_file);
        addTree(QStringLiteral("scan_lists"), generated_file);
        // A file a live recording names is a recording, whatever else names it and however the recording spells it.
        // (A store file is always replaced, so it stays.)
        const QStringList stores = six_store_files();
        for (auto it = m_files.begin(); it != m_files.end();) {
            it = !stores.contains(*it) && isRecording(*it) ? m_files.erase(it) : std::next(it);
        }
        QStringList files(m_files.cbegin(), m_files.cend());
        std::sort(files.begin(), files.end());
        for (const QString& file : files) {
            m_journal->files.append({file, QFileInfo(m_root.at(file)).size(), QString()});
            m_bytes += m_journal->files.last().size;
        }
        return m_bytes;
    }

  private:
    /** Whether @p relative is a regular file reached through no symlink. */
    bool
    regular(const QString& relative) const {
        const QFileInfo info(m_root.at(relative));
        return is_real_file(info.filePath()) && info.canonicalFilePath() == m_canonical + QLatin1Char('/') + relative;
    }

    bool
    add(const QString& relative) {
        if (!clean_relative(relative) || !regular(relative)) {
            return false;
        }
        m_files.insert(relative);
        return true;
    }

    /** The regular files below @p relative, every one or only those @p owned accepts. */
    void
    addTree(const QString& relative, bool (*owned)(const QString&) = nullptr) {
        const QFileInfo info(m_root.at(relative));
        if (!is_real_dir(info.filePath()) || info.canonicalFilePath() != m_canonical + QLatin1Char('/') + relative) {
            return;
        }
        QDirIterator it(info.filePath(), QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next().mid(m_root.path().size() + 1);
            if (owned == nullptr || owned(file)) {
                add(file);
            }
        }
    }

    /** The store's rows, read only from a regular store file (a symlinked store names nothing). */
    QJsonArray
    rows(const QString& store) const {
        QByteArray bytes;
        if (!regular(store) || !read_file(m_root.at(store), kMaxStagedJson, &bytes)) {
            return {};
        }
        const QJsonArray array = QJsonDocument::fromJson(bytes).array();
        wipe(bytes);
        return array;
    }

    /** Note a live Recording value, by the file it names (when it exists) and by its folded, cleaned spelling. */
    void
    noteRecording(const QString& value) {
        if (!QDir::isAbsolutePath(value)) {
            return;
        }
        const QString cleaned = QDir::cleanPath(value);
        m_recordingKeys.insert(path_key(cleaned));
        const QString canonical = QFileInfo(cleaned).canonicalFilePath();
        if (!canonical.isEmpty()) {
            m_recordingFiles.insert(canonical);
        }
    }

    /** Whether a live Recording value names the old file at @p relative, in any spelling. */
    bool
    isRecording(const QString& relative) const {
        const QString path = m_root.at(relative);
        return m_recordingKeys.contains(path_key(path))
               || m_recordingFiles.contains(QFileInfo(path).canonicalFilePath());
    }

    /** Claim an owned bundle root's tree: the first claim of its folded name, a real directory reached through no
        symlink. */
    void
    claimBundle(const QString& value, const QString& relative) {
        const QString bundles = m_root.at(QStringLiteral("imports/bundles/"));
        if (!value.startsWith(bundles) || !owned_name(value.mid(bundles.size()))
            || m_claimed.contains(path_key(relative))) {
            return;
        }
        if (is_real_dir(value) && QFileInfo(value).canonicalFilePath() == m_canonical + QLatin1Char('/') + relative) {
            m_claimed.insert(path_key(relative));
            m_journal->ownedRoots.append(relative);
            addTree(relative);
        }
    }

    void
    scanStore(const QString& store) {
        const QString imports = m_root.at(QStringLiteral("imports/"));
        for (const TypedPathValue& value : typed_path_values(store, rows(store))) {
            if (value.field.kind == PathKind::Recording) {
                noteRecording(value.value);
                continue;
            }
            if (!value.value.startsWith(m_root.path() + QLatin1Char('/'))
                || QDir::cleanPath(value.value) != value.value) {
                continue;
            }
            const QString relative = value.value.mid(m_root.path().size() + 1);
            if (value.value.startsWith(imports)) {
                if (value.field.kind == PathKind::File) {
                    add(relative);
                } else {
                    claimBundle(value.value, relative);
                }
            }
        }
    }

    const DataRoot& m_root;
    QString m_canonical;
    Journal* m_journal;
    QSet<QString> m_files;
    QSet<QString> m_claimed;
    QSet<QString> m_recordingKeys;  // folded, cleaned spellings of live Recording values
    QSet<QString> m_recordingFiles; // canonical paths of the files live Recording values name
    qint64 m_bytes = 0;
};

/**
 * Where every staged file goes. A destination that exists and is not part of the old configuration (an unknown
 * file, a recording, an unreferenced bundle directory) is never overwritten: a bundle moves to a fresh id, a loose
 * file takes the first free "name (n).ext" in its directory, and the stores' references follow when they are written.
 * A directory the backup needs where something other than a directory is (an old file included: a rollback must be
 * able to put it back) is never written through either: a bundle moves, and a loose file's directory takes a free
 * "dir (n)".
 *
 * One bundle cannot move: one whose row keeps its file outside it (a channel map updated in place stays beside its
 * bundle), since that file names the companions by the bundle's id, in contents a restore never rewrites. When such a
 * bundle's place is taken, the plan fails instead.
 */
class Destinations {
  public:
    Destinations(const DataRoot& root, const Staging& staging, Journal* journal)
        : m_root(root), m_staging(staging), m_journal(journal) {
        for (const Listed& file : journal->files) {
            m_old.insert(file.path);
        }
    }

    /** False with @p problem set (Names or BundleInTheWay) when no plan exists; @p problem is untouched otherwise. */
    bool
    plan(JournalProblem* problem) {
        bool inTheWay = false;
        if (placeAll(&inTheWay)) {
            return true;
        }
        *problem = inTheWay ? JournalProblem::BundleInTheWay : JournalProblem::Names;
        return false;
    }

  private:
    bool
    placeAll(bool* inTheWay) {
        const QStringList roots = stagedRoots();
        const QSet<QString> named = namedFromOutside();
        reserve(roots);
        for (const QString& root : roots) {
            if (!bundleBlocked(root)) {
                continue;
            }
            if (named.contains(root)) {
                *inTheWay = true;
                return false;
            }
            if (!moveBundle(root)) {
                return false;
            }
        }
        QMap<QString, QString> blocked; // staged file → the path it would take before a free name
        for (const QString& file : m_staging.carried) {
            const QString bundle = bundle_of(file);
            if (m_journal->bundles.contains(bundle)) {
                take(file, m_journal->bundles.value(bundle) + file.mid(bundle.size()));
            } else if (!bundle.isEmpty() && roots.contains(bundle)) {
                take(file, file);
            } else if (!placeLoose(file, &blocked)) {
                return false;
            }
        }
        for (auto it = blocked.cbegin(); it != blocked.cend(); ++it) {
            const QString name = freeName(it.value());
            if (name.isEmpty()) {
                return false;
            }
            take(it.key(), name);
        }
        collectCreatedDirs();
        return true;
    }

    QStringList
    stagedRoots() const {
        const QString bundles = m_root.at(QStringLiteral("imports/bundles/"));
        QStringList roots;
        for (const auto& value : m_staging.stores.value(QLatin1String(kImportedFilesStore))) {
            const QString root = value.toObject().value(QLatin1String("bundleRoot")).toString();
            if (root.startsWith(bundles) && owned_name(root.mid(bundles.size()))) {
                roots.append(root.mid(m_root.path().size() + 1));
            }
        }
        roots.removeDuplicates();
        std::sort(roots.begin(), roots.end());
        return roots;
    }

    /** The staged bundle roots whose row's file lies outside them. */
    QSet<QString>
    namedFromOutside() const {
        const QString prefix = m_root.path() + QLatin1Char('/');
        QSet<QString> roots;
        for (const auto& value : m_staging.stores.value(QLatin1String(kImportedFilesStore))) {
            const QJsonObject row = value.toObject();
            const QString root = row.value(QLatin1String("bundleRoot")).toString();
            if (root.startsWith(prefix)
                && !row.value(QLatin1String("path")).toString().startsWith(root + QLatin1Char('/'))) {
                roots.insert(root.mid(prefix.size()));
            }
        }
        return roots;
    }

    /** No free name may take a path a staged file, or a directory it needs, has by itself. */
    void
    reserve(const QStringList& roots) {
        for (const QString& root : roots) {
            m_reserved.insert(path_key(root));
        }
        for (const QString& file : m_staging.carried) {
            m_reserved.insert(path_key(file));
            for (const QString& dir : ancestors(file)) {
                m_reserved.insert(path_key(dir));
            }
        }
    }

    /** Whether something the old configuration does not own is at @p relative. */
    bool
    leafBlocked(const QString& relative) const {
        return entry_exists(m_root.at(relative)) && !m_old.contains(relative);
    }

    /** Whether a directory cannot be written through at @p relative: a symlink, a junction, or not a directory. */
    bool
    dirBlocked(const QString& relative) const {
        const QFileInfo info(m_root.at(relative));
        return info.isSymLink() || info.isJunction() || (info.exists() && !info.isDir());
    }

    /** Whether writing @p relative would replace something the old configuration does not own, or pass through a
        directory that cannot be written through (only the directories longer than @p from are looked at). */
    bool
    blocked_at(const QString& relative, qsizetype from) const {
        if (leafBlocked(relative)) {
            return true;
        }
        const QStringList dirs = ancestors(relative);
        return std::any_of(dirs.cbegin(), dirs.cend(),
                           [&](const QString& dir) { return dir.size() > from && dirBlocked(dir); });
    }

    /** @p file with its first blocked directory (below imports/) moved to a free name; empty when none is free. */
    QString
    aroundBlockedDirs(const QString& file) {
        for (const QString& dir : ancestors(file)) {
            if (!dir.contains(QLatin1Char('/'))) {
                continue; // imports/ itself is never renamed
            }
            const auto moved = m_dirMoves.constFind(dir);
            if (moved != m_dirMoves.constEnd()) {
                return *moved + file.mid(dir.size());
            }
            if (dirBlocked(dir)) {
                const QString fresh = freeName(dir);
                if (fresh.isEmpty()) {
                    return {};
                }
                m_dirMoves.insert(dir, fresh);
                m_taken.insert(path_key(fresh));
                return fresh + file.mid(dir.size());
            }
        }
        return file;
    }

    /** Place a file in no staged bundle: as is, around blocked directories, or (in @p blocked) for a free name. */
    bool
    placeLoose(const QString& file, QMap<QString, QString>* blocked) {
        const QString around = aroundBlockedDirs(file);
        if (around.isEmpty()) {
            return false;
        }
        if (around == file ? leafBlocked(file) : !isFree(around)) {
            blocked->insert(file, around);
        } else {
            take(file, around);
        }
        return true;
    }

    bool
    bundleBlocked(const QString& root) const {
        if (entry_exists(m_root.at(root)) && !m_journal->ownedRoots.contains(root)) {
            return true;
        }
        return std::any_of(m_staging.carried.cbegin(), m_staging.carried.cend(), [&](const QString& file) {
            return bundle_of(file) == root && blocked_at(file, root.size());
        });
    }

    bool
    isFree(const QString& relative) const {
        return !entry_exists(m_root.at(relative)) && !m_taken.contains(path_key(relative))
               && !m_reserved.contains(path_key(relative));
    }

    bool
    moveBundle(const QString& root) {
        const QString id = root.mid(root.lastIndexOf(QLatin1Char('/')) + 1);
        for (int n = 1; n < kMaxFreeNames; ++n) {
            const QString suffix = QLatin1Char('-') + QString::number(n);
            const QString fresh = QStringLiteral("imports/bundles/") + id.left(63 - suffix.size()) + suffix;
            if (isFree(fresh)) {
                m_journal->bundles.insert(root, fresh);
                m_taken.insert(path_key(fresh));
                return true;
            }
        }
        return false;
    }

    QString
    freeName(const QString& file) const {
        const qsizetype slash = file.lastIndexOf(QLatin1Char('/'));
        const QFileInfo leaf(file.mid(slash + 1));
        const QString base = leaf.completeBaseName(), suffix = leaf.suffix();
        for (int n = 1; n < kMaxFreeNames; ++n) {
            const QString number = QStringLiteral(" (%1)").arg(n);
            const QString name = base.isEmpty()
                                     ? leaf.fileName() + number
                                     : base + number + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
            const QString candidate = file.left(slash + 1) + name;
            if (isFree(candidate)) {
                return candidate;
            }
        }
        return {};
    }

    void
    take(const QString& file, const QString& destination) {
        m_journal->destinations.insert(file, destination);
        m_taken.insert(path_key(destination));
    }

    void
    collectCreatedDirs() {
        QSet<QString> created;
        for (const QString& destination : m_journal->destinations) {
            for (const QString& dir : ancestors(destination)) {
                if (!is_real_dir(m_root.at(dir))) {
                    created.insert(dir); // missing, or something a rollback must find gone to put it back
                }
            }
        }
        m_journal->createdDirs = QStringList(created.cbegin(), created.cend());
        std::sort(m_journal->createdDirs.begin(), m_journal->createdDirs.end());
    }

    const DataRoot& m_root;
    const Staging& m_staging;
    Journal* m_journal;
    QSet<QString> m_old;
    QSet<QString> m_taken;             // folded destinations chosen so far
    QSet<QString> m_reserved;          // folded staged paths, their directories and bundle roots
    QMap<QString, QString> m_dirMoves; // blocked directory → the free one loose files go to instead
};

class Applier {
  public:
    explicit Applier(const ApplyEnv& env)
        : m_env(env), m_pending(m_root.at(QLatin1String(kPendingDirName))),
          m_journalDir(m_pending + QStringLiteral("/journal")) {}

    ApplyResult
    run() {
        if (!m_root.valid() || !is_real_dir(m_pending)) {
            return {};
        }
        if (!is_real_file(pendingPath(QLatin1String(kCommittedName)))) {
            m_root.removeTree(m_pending); // never confirmed, or left over after the commit point
            return {};
        }
        // Reading the staging changes nothing, and gives a deferred restore its summary.
        const bool verified = StagingReader(m_pending, &m_staging).read();
        if (busy()) {
            return result(ApplyOutcome::Deferred, busy_reason());
        }
        if (!entry_exists(m_journalDir + QStringLiteral("/COMPLETE"))) {
            return startFresh(verified);
        }
        if (!loadJournal()) {
            return stuck();
        }
        return verified && consistent() ? forward() : rollBackAndFinish();
    }

  private:
    QString
    pendingPath(const QString& name) const {
        return m_pending + QLatin1Char('/') + name;
    }

    bool
    busy() const {
        return m_env.engineBusy && m_env.engineBusy();
    }

    Status
    step(const char* name, const std::function<bool()>& op) const {
        switch (m_env.fault ? m_env.fault(name) : Fault::None) {
            case Fault::Crash: return Status::Crashed;
            case Fault::IoError: return Status::Failed;
            case Fault::None: break;
        }
        return op() ? Status::Ok : Status::Failed;
    }

    template <typename List, typename Op>
    Status
    each(const char* name, const List& items, Op op) const {
        for (const auto& item : items) {
            const Status status = step(name, [&] { return op(item); });
            if (status != Status::Ok) {
                return status;
            }
        }
        return Status::Ok;
    }

    ApplyResult
    result(ApplyOutcome outcome, const QString& reason) const {
        if (outcome == ApplyOutcome::Applied) {
            LOG_INFO("Backup restore applied at start\n");
        } else if (outcome != ApplyOutcome::None) {
            LOG_WARN("Backup restore at start: %s: %s\n", qUtf8Printable(apply_outcome_name(outcome)),
                     qUtf8Printable(reason));
        }
        return {outcome, reason, m_staging.summary};
    }

    /** Stuck: everything is kept for the next start, and the result file says so (best effort). */
    ApplyResult
    stuck() const {
        writeResult(ApplyOutcome::Stuck, stuck_reason());
        return result(ApplyOutcome::Stuck, stuck_reason());
    }

    /**
     * Failed before any change: the same ending as after a rollback. The marker goes first, so a stage whose files
     * cannot all be removed is never run again (the next start removes what is left); a marker that cannot be removed
     * repeats this outcome at the next start, which changes nothing either.
     */
    ApplyResult
    failUnchanged(const QString& reason) const {
        const Status written = step("result.write", [&] {
            writeResult(ApplyOutcome::Failed, reason); // best effort
            return true;
        });
        if (written == Status::Crashed
            || step("marker.remove", [this] { return m_root.removeFile(pendingPath(QLatin1String(kCommittedName))); })
                   == Status::Crashed
            || step("cleanup", [this] { return m_root.removeTree(m_pending); }) == Status::Crashed) {
            return interrupted();
        }
        return result(ApplyOutcome::Failed, reason);
    }

    /** What a crash leaves: the caller sees nothing, as if the process had died. */
    static ApplyResult
    interrupted() {
        return {ApplyOutcome::None, QStringLiteral("interrupted"), {}};
    }

    void
    writeResult(ApplyOutcome outcome, const QString& reason) const {
        const QJsonObject object{{QStringLiteral("outcome"), apply_outcome_name(outcome)},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("summary"), QJsonObject::fromVariantMap(m_staging.summary)}};
        if (!m_root.writeFile(m_root.at(QLatin1String(kResultName)), json_bytes(object))) {
            LOG_WARN("Backup restore result could not be saved\n");
        }
    }

    /* ---- steps 1 and 2: before any change ---- */

    ApplyResult
    startFresh(bool verified) {
        if (!verified) {
            return failUnchanged(nothing_changed()); // the stage cannot be trusted
        }
        const qint64 oldBytes = LiveScan(m_root, &m_journal).run();
        const qint64 free = m_env.freeBytes
                                ? m_env.freeBytes(m_root.path())
                                : static_cast<qint64>(dsd_path_free_bytes(m_root.path().toUtf8().constData()));
        if (free >= 0 && free < m_staging.bytes + oldBytes + kSpaceMargin) {
            return result(ApplyOutcome::Deferred, space_reason());
        }
        const Status built = buildJournal();
        if (built == Status::Crashed) {
            return interrupted();
        }
        if (built == Status::Failed) {
            m_root.removeEntry(m_journalDir); // nothing live was changed
            switch (m_problem) {
                case JournalProblem::Names: return failUnchanged(names_reason());
                case JournalProblem::Unreadable: return failUnchanged(unreadable_reason());
                case JournalProblem::Leftover: return failUnchanged(leftover_reason());
                case JournalProblem::BundleInTheWay: return failUnchanged(bundle_reason());
                case JournalProblem::Space: break;
            }
            return result(ApplyOutcome::Deferred, space_reason()); // the next start tries again
        }
        if (busy()) {
            // Nothing live was changed either: a journal kept now would be replayed after a whole session on the old
            // configuration. The next start plans again from the configuration as it is then.
            m_root.removeFile(m_journalDir + QStringLiteral("/COMPLETE"));
            m_root.removeTree(m_journalDir);
            return result(ApplyOutcome::Deferred, busy_reason());
        }
        return forward();
    }

    QByteArray
    journalManifest() const {
        QJsonArray files;
        for (const Listed& file : m_journal.files) {
            files.append(listed_json(file));
        }
        return json_bytes({{QStringLiteral("journalVersion"), kJournalVersion},
                           {QStringLiteral("files"), files},
                           {QStringLiteral("stores"), QJsonArray::fromStringList(m_journal.stores)},
                           {QStringLiteral("ownedRoots"), QJsonArray::fromStringList(m_journal.ownedRoots)},
                           {QStringLiteral("destinations"), map_json(m_journal.destinations)},
                           {QStringLiteral("bundles"), map_json(m_journal.bundles)},
                           {QStringLiteral("createdDirs"), QJsonArray::fromStringList(m_journal.createdDirs)},
                           {QStringLiteral("prefsSha256"), m_journal.prefsSha256}});
    }

    Status
    buildJournal() {
        if (!Destinations(m_root, m_staging, &m_journal).plan(&m_problem)) {
            return Status::Failed;
        }
        Status status = step("journal.begin", [this] {
            if (!m_root.removeEntry(m_journalDir)) {
                m_problem = JournalProblem::Leftover; // an earlier attempt's journal: not a matter of space
                return false;
            }
            return m_root.makeDirs(m_journalDir + QStringLiteral("/files"));
        });
        for (qsizetype i = 0; status == Status::Ok && i < m_journal.files.size(); ++i) {
            Listed& file = m_journal.files[i];
            status = step("journal.files", [&] {
                QFile source(m_root.at(file.path));
                if (!source.open(QIODevice::ReadOnly)) {
                    m_problem = JournalProblem::Unreadable; // not a matter of space: retrying would not help
                    return false;
                }
                return m_root.copyFile(m_root.at(file.path), m_journalDir + QStringLiteral("/files/") + file.path,
                                       QString(), &file.sha256, &file.size);
            });
        }
        if (status == Status::Ok) {
            m_journal.prefs = AppPrefs::readPortable();
            const QByteArray prefs = json_bytes(QJsonObject::fromVariantMap(m_journal.prefs));
            m_journal.prefsSha256 = sha256_hex(prefs);
            status = step("journal.prefs",
                          [&] { return m_root.writeFile(m_journalDir + QStringLiteral("/prefs.json"), prefs); });
        }
        const QByteArray manifest = journalManifest();
        if (status == Status::Ok) {
            status = step("journal.manifest",
                          [&] { return m_root.writeFile(m_journalDir + QStringLiteral("/manifest.json"), manifest); });
        }
        if (status == Status::Ok) {
            status = step("journal.complete", [&] {
                return m_root.writeFile(m_journalDir + QStringLiteral("/COMPLETE"), sha256_hex(manifest).toLatin1());
            });
        }
        return status;
    }

    bool
    loadJournal() {
        QByteArray manifest, complete, prefs;
        if (!read_file(m_journalDir + QStringLiteral("/manifest.json"), kMaxStagedJson, &manifest)
            || !read_file(m_journalDir + QStringLiteral("/COMPLETE"), 256, &complete)
            || QString::fromLatin1(complete.trimmed()) != sha256_hex(manifest)
            || !read_file(m_journalDir + QStringLiteral("/prefs.json"), kMaxStagedJson, &prefs)) {
            return false;
        }
        const QJsonObject object = QJsonDocument::fromJson(manifest).object();
        if (object.value(QLatin1String("journalVersion")).toInteger(-1) != kJournalVersion) {
            return false;
        }
        for (const auto& value : object.value(QLatin1String("files")).toArray()) {
            Listed file;
            if (!listed_from(value, &file)) {
                return false;
            }
            m_journal.files.append(file);
        }
        m_journal.stores = string_list(object.value(QLatin1String("stores")));
        m_journal.ownedRoots = string_list(object.value(QLatin1String("ownedRoots")));
        m_journal.destinations = json_map(object.value(QLatin1String("destinations")));
        m_journal.bundles = json_map(object.value(QLatin1String("bundles")));
        m_journal.createdDirs = string_list(object.value(QLatin1String("createdDirs")));
        // The settings a rollback would restore must be the ones the journal recorded: a prefs.json that does not match
        // the manifest's hash, or is not one JSON object, leaves the journal unreadable (Stuck), never an empty map
        // that would clear the old settings.
        m_journal.prefsSha256 = object.value(QLatin1String("prefsSha256")).toString();
        QJsonParseError prefsError{};
        const QJsonDocument prefsDocument = QJsonDocument::fromJson(prefs, &prefsError);
        if (!is_sha256_hex(m_journal.prefsSha256) || sha256_hex(prefs) != m_journal.prefsSha256
            || prefsError.error != QJsonParseError::NoError || !prefsDocument.isObject()) {
            return false;
        }
        m_journal.prefs = prefsDocument.object().toVariantMap();
        const QStringList paths =
            m_journal.destinations.values() + m_journal.bundles.values() + m_journal.ownedRoots + m_journal.createdDirs;
        return std::all_of(paths.cbegin(), paths.cend(), clean_relative);
    }

    /** Whether the staging is the one the journal planned for. */
    bool
    consistent() const {
        QStringList carried = m_staging.carried;
        std::sort(carried.begin(), carried.end());
        return carried == m_journal.destinations.keys();
    }

    /* ---- step 3: roll forward ---- */

    /** A typed value with the staged files' and bundles' destinations followed. */
    QString
    remapped(const TypedPathValue& value) const {
        const QString prefix = m_root.path() + QLatin1Char('/');
        if (!value.value.startsWith(prefix)) {
            return value.value;
        }
        const QString relative = value.value.mid(prefix.size());
        const auto& map = value.field.kind == PathKind::BundleRoot ? m_journal.bundles : m_journal.destinations;
        const auto it = map.constFind(relative);
        return it == map.constEnd() ? value.value : prefix + *it;
    }

    bool
    writeStore(const QString& store) const {
        const auto it = m_staging.stores.constFind(store);
        if (it == m_staging.stores.constEnd()) {
            return m_root.removeFile(m_root.at(store));
        }
        const QJsonArray rows =
            map_typed_path_values(store, *it, [this](const TypedPathValue& value) { return remapped(value); });
        QByteArray bytes = QJsonDocument(rows).toJson(QJsonDocument::Compact);
        const bool ok = m_root.writeFile(m_root.at(store), bytes);
        wipe(bytes); // profiles hold keys
        return ok;
    }

    Status
    rollForwardSteps() const {
        Status status = each("forward.delete", m_journal.files,
                             [this](const Listed& file) { return m_root.removeFile(m_root.at(file.path)); });
        if (status != Status::Ok) {
            return status;
        }
        for (const QString& root : m_journal.ownedRoots) {
            m_root.pruneEmptyDirs(m_root.at(root));
        }
        status = step("forward.generated", [this] {
            // Their generated files were part of the old set; a directory that held one goes once it is empty, and
            // whatever else is there is not ours and stays, with the directories it is in.
            pruneGeneratedDirs();
            return true;
        });
        if (status == Status::Ok) {
            status = each("forward.files", m_staging.carried, [this](const QString& file) {
                return m_root.copyFile(pendingPath(QStringLiteral("files/") + file),
                                       m_root.at(m_journal.destinations.value(file)), m_staging.carriedSha.value(file));
            });
        }
        if (status == Status::Ok) {
            status = each("forward.stores", backup_store_names(),
                          [this](const QString& store) { return writeStore(store); });
        }
        if (status == Status::Ok) {
            status = step("forward.avoids", [this] {
                return m_root.writeFile(m_root.at(QLatin1String(kAvoidsFileName)), native_avoids(m_staging.avoids));
            });
        }
        if (status == Status::Ok) {
            status = step("forward.prefs", [this] { return AppPrefs::replacePortable(m_staging.prefs); });
        }
        return status;
    }

    /** Remove the directories that held the old set's generated files once they are empty, deepest first. */
    void
    pruneGeneratedDirs() const {
        QSet<QString> dirs;
        for (const Listed& file : m_journal.files) {
            if (generated_file(file.path)) {
                for (const QString& dir : ancestors(file.path)) {
                    dirs.insert(dir);
                }
            }
        }
        QStringList deepestFirst(dirs.cbegin(), dirs.cend());
        std::sort(deepestFirst.begin(), deepestFirst.end(),
                  [](const QString& a, const QString& b) { return a.size() > b.size(); });
        for (const QString& dir : deepestFirst) {
            m_root.removeEmptyDir(m_root.at(dir));
        }
    }

    ApplyResult
    forward() const {
        const Status status = rollForwardSteps();
        if (status == Status::Crashed) {
            return interrupted();
        }
        return status == Status::Failed ? rollBackAndFinish() : finish(ApplyOutcome::Applied, QString());
    }

    /* ---- steps 4 to 6: the ending ---- */

    ApplyResult
    finish(ApplyOutcome outcome, const QString& reason) const {
        Status status = step("result.write", [&] {
            writeResult(outcome, reason); // best effort: the outcome is also returned
            return true;
        });
        if (status == Status::Crashed) {
            return interrupted();
        }
        // The commit point: once the marker is gone, nothing will run this restore again.
        status =
            step("marker.remove", [this] { return m_root.removeFile(pendingPath(QLatin1String(kCommittedName))); });
        if (status == Status::Crashed) {
            return interrupted();
        }
        if (status == Status::Failed) {
            // Stuck, not rolled back: a rollback ends here too, with this same marker to remove, so it could never
            // complete either, and it would only undo a configuration already in place. The marker and the journal
            // stay; the next start repeats the roll forward, which is idempotent (it reads only the journal and the
            // staging), and removes the marker then.
            return stuck();
        }
        // An unmarked leftover is removed at the next start.
        if (step("cleanup", [this] { return m_root.removeTree(m_pending); }) == Status::Crashed) {
            return interrupted();
        }
        return result(outcome, reason);
    }

    /**
     * Delete @p relative when this apply could have written it. Everything it writes is a regular file reached through
     * no symlink, so anything else at a destination (a symlink, a directory, a path through a symlink) was never its
     * own and stays.
     */
    bool
    removeWritten(const QString& relative) const {
        const QString path = m_root.at(relative);
        return !m_root.contains(path) || !is_real_file(path) || m_root.removeFile(path);
    }

    Status
    rollBackSteps() const {
        QStringList written = m_journal.destinations.values() + six_store_files();
        written.removeDuplicates();
        std::sort(written.begin(), written.end());
        Status status = each("rollback.delete", written, [this](const QString& path) { return removeWritten(path); });
        if (status != Status::Ok) {
            return status;
        }
        QStringList created = m_journal.createdDirs;
        std::sort(created.begin(), created.end(),
                  [](const QString& a, const QString& b) { return a.size() > b.size(); });
        for (const QString& dir : created) {
            m_root.removeEmptyDir(m_root.at(dir));
        }
        status = each("rollback.files", m_journal.files, [this](const Listed& file) {
            return m_root.copyFile(m_journalDir + QStringLiteral("/files/") + file.path, m_root.at(file.path),
                                   file.sha256);
        });
        if (status == Status::Ok) {
            status = step("rollback.prefs", [this] { return AppPrefs::replacePortable(m_journal.prefs); });
        }
        return status;
    }

    ApplyResult
    rollBackAndFinish() const {
        const Status status = rollBackSteps();
        if (status == Status::Crashed) {
            return interrupted();
        }
        if (status == Status::Failed) {
            return stuck(); // the next start rolls forward again
        }
        return finish(ApplyOutcome::Failed, nothing_changed());
    }

    const ApplyEnv& m_env;
    DataRoot m_root;
    QString m_pending, m_journalDir;
    Staging m_staging;
    Journal m_journal;
    JournalProblem m_problem = JournalProblem::Space;
};

} // namespace

QString
apply_outcome_name(ApplyOutcome outcome) {
    switch (outcome) {
        case ApplyOutcome::None: return QString();
        case ApplyOutcome::Applied: return QStringLiteral("applied");
        case ApplyOutcome::Failed: return QStringLiteral("failed");
        case ApplyOutcome::Deferred: return QStringLiteral("deferred");
        case ApplyOutcome::Stuck: return QStringLiteral("stuck");
    }
    return QString();
}

ApplyResult
apply_pending_restore(const ApplyEnv& env) {
    Applier applier(env);
    return applier.run();
}

} // namespace backup
} // namespace dsd_qt
