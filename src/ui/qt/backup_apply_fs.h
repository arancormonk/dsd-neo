// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The data-root file operations a restore stages and applies with (issue #580). Module-private.
 *
 * Every write, delete and rename of the staging and apply code goes through DataRoot, whose one check (contains())
 * refuses a path outside the data root or one whose existing components below the root include a symlink or a
 * junction. The one exception is removeLink(), which unlinks a path whose last component is itself a link: it checks
 * the link's directory instead, and removes only the link, never its target.
 */

#ifndef DSD_NEO_SRC_UI_QT_BACKUP_APPLY_FS_H_
#define DSD_NEO_SRC_UI_QT_BACKUP_APPLY_FS_H_

#include <QByteArray> // IWYU pragma: keep -- file contents cross this interface.
#include <QString>
#include <QtGlobal>

namespace dsd_qt {
namespace backup {
namespace detail {

constexpr char kPendingDirName[] = "restore-pending";
constexpr char kStagingDirPrefix[] = "restore-staging-";
constexpr char kCommittedName[] = "COMMITTED";
constexpr char kManifestName[] = "manifest.json";
constexpr char kResultName[] = "backup-restore-result.json";
/** The staging manifest lists every staged file (at most kMaxFiles carried ones) and the summary. */
constexpr qint64 kMaxStagingManifestBytes = 8LL * 1024 * 1024;

/** Lower-case hex sha256 of @p bytes. */
QString sha256_hex(const QByteArray& bytes);

/** Whether @p text is 64 lower-case hex digits. */
bool is_sha256_hex(const QString& text);

/** Whether @p path is a directory itself, not a symlink or junction to one. */
bool is_real_dir(const QString& path);

/** Whether @p path is a regular file itself, not a symlink or junction to one. */
bool is_real_file(const QString& path);

/** Whether any entry is at @p path, a dangling symlink included. */
bool entry_exists(const QString& path);

/** Read the regular file @p path (at most @p cap bytes) into @p bytes. */
bool read_file(const QString& path, qint64 cap, QByteArray* bytes);

/** Stream the regular file @p path through sha256. */
bool hash_file(const QString& path, QString* sha256, qint64* size);

/** The data root (json_store_root()) and the contained operations on it. */
class DataRoot {
  public:
    /** json_store_root(), as it reads now. */
    DataRoot();

    /** @p root, a data root the caller read (a worker takes the one the UI thread read). */
    explicit DataRoot(const QString& root);

    /** A usable root: non-empty, absolute. */
    bool valid() const;

    const QString&
    path() const {
        return m_root;
    }

    /** The root with symlinks resolved; empty when it does not exist. */
    QString canonical() const;

    /** root + "/" + @p relative. */
    QString at(const QString& relative) const;

    /**
     * The containment check: @p location is a clean absolute path below the root, and none of its existing components
     * below the root (the path itself included) is a symlink or a junction.
     */
    bool contains(const QString& location) const;

    /** Leave no entry at @p location; true when none is left. Refuses a directory and an uncontained path. */
    bool removeFile(const QString& location) const;

    /**
     * Remove the directory @p location and everything below it, one level at a time. Nothing is followed: a symlink or
     * junction inside is unlinked itself (removeLink()), never descended into, and its target is never touched; a
     * file goes through removeFile(); a real subdirectory is removed by the same recursion, which ends, like this
     * call, in removeEmptyDir() once it is empty.
     */
    bool removeTree(const QString& location) const;

    /** Unlink the symlink or junction at @p location itself, never its target; its directory must pass contains(). */
    bool removeLink(const QString& location) const;

    /**
     * Remove whatever is at @p location without following it: a link (removeLink()), a tree (removeTree()) or a
     * file.
     */
    bool removeEntry(const QString& location) const;

    /** Remove @p location when it is an empty directory; false when it stays. */
    bool removeEmptyDir(const QString& location) const;

    /** Remove every empty directory at or below @p location, deepest first; directories with content stay. */
    void pruneEmptyDirs(const QString& location) const;

    bool makeDirs(const QString& location) const;

    /** Write @p bytes to @p location through QSaveFile with mode 0600, creating its directories. */
    bool writeFile(const QString& location, const QByteArray& bytes) const;

    /**
     * Copy the regular file @p from to @p to (QSaveFile, 0600). When @p expected is not empty the copy must hash to
     * it, else nothing is written. @p sha256 and @p size (optional) receive what was copied.
     */
    bool copyFile(const QString& from, const QString& to, const QString& expected, QString* sha256 = nullptr,
                  qint64* size = nullptr) const;

    /** Rename @p from to @p to, which must not exist. */
    bool rename(const QString& from, const QString& to) const;

  private:
    QString m_root;
};

} // namespace detail
} // namespace backup
} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_BACKUP_APPLY_FS_H_ */
