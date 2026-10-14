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

DataRoot::DataRoot() {
    const QString root = json_store_root();
    m_root = root.isEmpty() ? QString() : QDir::cleanPath(root);
}

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
DataRoot::contains(const QString& path) const {
    if (!valid() || !QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path
        || !path.startsWith(m_root + QLatin1Char('/'))) {
        return false;
    }
    QString current = m_root;
    for (const QString& part : path.mid(m_root.size() + 1).split(QLatin1Char('/'))) {
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
DataRoot::removeFile(const QString& path) const {
    if (!entry_exists(path)) {
        return true;
    }
    if (!contains(path) || QFileInfo(path).isDir()) {
        return false;
    }
    QFile::remove(path);
    return !entry_exists(path);
}

bool
DataRoot::removeTree(const QString& path) const {
    if (!entry_exists(path)) {
        return true;
    }
    if (!contains(path) || !is_real_dir(path)) {
        return false;
    }
    // One level at a time, so nothing is reached through a link: removeEntry() unlinks a link and descends only into
    // real directories.
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    bool removed = true;
    for (const QFileInfo& entry : entries) {
        removed = removeEntry(entry.filePath()) && removed;
    }
    return removed && removeEmptyDir(path) && !entry_exists(path);
}

bool
DataRoot::removeLink(const QString& path) const {
    const QFileInfo info(path);
    const QString parent = info.absolutePath();
    const bool parentOk = parent == m_root ? valid() : contains(parent);
    if (!is_link(info) || !parentOk || QDir::cleanPath(path) != path || !path.startsWith(m_root + QLatin1Char('/'))) {
        return false;
    }
    // QFile::remove() unlinks a symlink; a directory symlink or junction on Windows needs rmdir, which removes the
    // link and leaves its target alone.
    if (!QFile::remove(path) && info.isDir()) {
        QDir().rmdir(path);
    }
    return !entry_exists(path);
}

bool
DataRoot::removeEntry(const QString& path) const {
    const QFileInfo info(path);
    if (is_link(info)) {
        return removeLink(path);
    }
    return info.isDir() ? removeTree(path) : removeFile(path);
}

bool
DataRoot::removeEmptyDir(const QString& path) const {
    if (!contains(path) || !is_real_dir(path)) {
        return false;
    }
    return QDir().rmdir(path);
}

void
DataRoot::pruneEmptyDirs(const QString& path) const {
    if (!contains(path) || !is_real_dir(path)) {
        return;
    }
    QStringList dirs;
    QDirIterator it(path, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        dirs.append(it.next());
    }
    // Deepest first, so a directory is empty by the time its turn comes when all below it were.
    std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
    dirs.append(path);
    for (const QString& dir : dirs) {
        removeEmptyDir(dir);
    }
}

bool
DataRoot::makeDirs(const QString& path) const {
    if (path == m_root) {
        return valid() && QDir().mkpath(path) && QFileInfo(path).isDir(); // the root itself may be a symlink
    }
    return contains(path) && QDir().mkpath(path) && is_real_dir(path);
}

bool
DataRoot::writeFile(const QString& path, const QByteArray& bytes) const {
    if (!contains(path) || QFileInfo(path).isDir() || !makeDirs(QFileInfo(path).absolutePath())) {
        return false;
    }
    QSaveFile file(path);
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
