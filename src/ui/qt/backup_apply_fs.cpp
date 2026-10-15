// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "backup_apply_fs.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QChar>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QSaveFile>
#include <QStringList>
#include <Qt>
#include <algorithm>
#include <qflags.h>
#include "backup_archive_rules.h"
#include "json_store.h"

namespace dsd_qt {
namespace backup {
namespace detail {

namespace {

constexpr qint64 kChunkBytes = 64LL * 1024;

bool
is_link(const QFileInfo& info) {
    return info.isSymLink() || info.isJunction();
}

bool
open_private(QSaveFile& file) {
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

} // namespace

QString
sha256_hex(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool
is_sha256_hex(const QString& text) {
    return text.size() == 64 && std::all_of(text.cbegin(), text.cend(), [](QChar c) {
               return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
           });
}

bool
is_real_dir(const QString& path) {
    const QFileInfo info(path);
    return !path.isEmpty() && info.isDir() && !is_link(info);
}

bool
is_real_file(const QString& path) {
    const QFileInfo info(path);
    return !path.isEmpty() && info.isFile() && !is_link(info);
}

bool
entry_exists(const QString& path) {
    const QFileInfo info(path);
    return info.exists() || is_link(info);
}

bool
read_file(const QString& path, qint64 cap, QByteArray* bytes) {
    QFile file(path);
    if (!is_real_file(path) || !file.open(QIODevice::ReadOnly) || file.size() > cap) {
        return false;
    }
    *bytes = file.read(cap + 1);
    return bytes->size() <= cap && file.error() == QFileDevice::NoError;
}

bool
hash_file(const QString& path, QString* sha256, qint64* size) {
    QFile file(path);
    if (!is_real_file(path) || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(kChunkBytes, Qt::Uninitialized);
    qint64 total = 0, n = 0;
    while ((n = file.read(chunk.data(), chunk.size())) > 0) {
        hash.addData(QByteArrayView(chunk.constData(), n));
        total += n;
    }
    wipe(chunk);
    *sha256 = QString::fromLatin1(hash.result().toHex());
    *size = total;
    return n == 0;
}

DataRoot::DataRoot() : DataRoot(json_store_root()) {}

DataRoot::DataRoot(const QString& root) : m_root(root.isEmpty() ? QString() : QDir::cleanPath(root)) {}

bool
DataRoot::valid() const {
    return m_root.size() > 1 && QDir::isAbsolutePath(m_root);
}

QString
DataRoot::canonical() const {
    return QFileInfo(m_root).canonicalFilePath();
}

QString
DataRoot::at(const QString& relative) const {
    return m_root + QLatin1Char('/') + relative;
}

bool
DataRoot::contains(const QString& location) const {
    if (!valid() || !QDir::isAbsolutePath(location) || QDir::cleanPath(location) != location
        || !location.startsWith(m_root + QLatin1Char('/'))) {
        return false;
    }
    QString current = m_root;
    for (const QString& part : location.mid(m_root.size() + 1).split(QLatin1Char('/'))) {
        current += QLatin1Char('/') + part;
        const QFileInfo info(current);
        if (is_link(info)) {
            return false;
        }
        if (!info.exists()) {
            break; // nothing exists below a missing component
        }
    }
    return true;
}

bool
DataRoot::removeFile(const QString& location) const {
    if (!entry_exists(location)) {
        return true;
    }
    if (!contains(location) || QFileInfo(location).isDir()) {
        return false;
    }
    QFile::remove(location);
    return !entry_exists(location);
}

bool
DataRoot::removeTree(const QString& location) const {
    if (!entry_exists(location)) {
        return true;
    }
    if (!contains(location) || !is_real_dir(location)) {
        return false;
    }
    // One level at a time, so nothing is reached through a link: removeEntry() unlinks a link and descends only into
    // real directories.
    const QFileInfoList entries =
        QDir(location).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    bool removed = true;
    for (const QFileInfo& entry : entries) {
        removed = removeEntry(entry.filePath()) && removed;
    }
    return removed && removeEmptyDir(location) && !entry_exists(location);
}

bool
DataRoot::removeLink(const QString& location) const {
    if (QDir::cleanPath(location) != location || !location.startsWith(m_root + QLatin1Char('/'))) {
        return false;
    }
    // The directory, taken from the clean path itself: QFileInfo could spell it differently (a Windows drive letter).
    const QString parent = location.left(location.lastIndexOf(QLatin1Char('/')));
    const QFileInfo info(location);
    if (!is_link(info) || !(parent == m_root ? valid() : contains(parent))) {
        return false;
    }
    // QFile::remove() unlinks a symlink; a directory symlink or junction on Windows needs rmdir, which removes the
    // link and leaves its target alone.
    if (!QFile::remove(location) && info.isDir()) {
        QDir().rmdir(location);
    }
    return !entry_exists(location);
}

bool
DataRoot::removeEntry(const QString& location) const {
    const QFileInfo info(location);
    if (is_link(info)) {
        return removeLink(location);
    }
    return info.isDir() ? removeTree(location) : removeFile(location);
}

bool
DataRoot::removeEmptyDir(const QString& location) const {
    if (!contains(location) || !is_real_dir(location)) {
        return false;
    }
    return QDir().rmdir(location);
}

void
DataRoot::pruneEmptyDirs(const QString& location) const {
    if (!contains(location) || !is_real_dir(location)) {
        return;
    }
    QStringList dirs;
    QDirIterator it(location, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        dirs.append(it.next());
    }
    // Deepest first, so a directory is empty by the time its turn comes when all below it were.
    std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
    dirs.append(location);
    for (const QString& dir : dirs) {
        removeEmptyDir(dir);
    }
}

bool
DataRoot::makeDirs(const QString& location) const {
    if (location == m_root) {
        return valid() && QDir().mkpath(location) && QFileInfo(location).isDir(); // the root itself may be a symlink
    }
    return contains(location) && QDir().mkpath(location) && is_real_dir(location);
}

bool
DataRoot::writeFile(const QString& location, const QByteArray& bytes) const {
    if (!contains(location) || QFileInfo(location).isDir() || !makeDirs(QFileInfo(location).absolutePath())) {
        return false;
    }
    QSaveFile file(location);
    return open_private(file) && file.write(bytes) == bytes.size() && file.commit();
}

bool
DataRoot::copyFile(const QString& from, const QString& to, const QString& expected, QString* sha256,
                   qint64* size) const {
    QFile source(from);
    if (!contains(from) || !is_real_file(from) || !contains(to) || QFileInfo(to).isDir()
        || !makeDirs(QFileInfo(to).absolutePath()) || !source.open(QIODevice::ReadOnly)) {
        return false;
    }
    QSaveFile target(to);
    if (!open_private(target)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(kChunkBytes, Qt::Uninitialized);
    qint64 total = 0, n = 0;
    bool written = true;
    while (written && (n = source.read(chunk.data(), chunk.size())) > 0) {
        hash.addData(QByteArrayView(chunk.constData(), n));
        written = target.write(chunk.constData(), n) == n;
        total += n;
    }
    wipe(chunk); // the configuration holds keys
    const QString copied = QString::fromLatin1(hash.result().toHex());
    if (!written || n != 0 || (!expected.isEmpty() && copied != expected)) {
        target.cancelWriting();
        return false;
    }
    if (sha256 != nullptr) {
        *sha256 = copied;
    }
    if (size != nullptr) {
        *size = total;
    }
    return target.commit();
}

bool
DataRoot::rename(const QString& from, const QString& to) const {
    return contains(from) && contains(to) && entry_exists(from) && !entry_exists(to) && QDir().rename(from, to);
}

} // namespace detail
} // namespace backup
} // namespace dsd_qt
