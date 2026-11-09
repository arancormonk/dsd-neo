// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the import copies' base transport (#832) and the host's hand-out and abort of it. */

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QtGlobal>
#include <cstdio>
#include <memory>
#include <qflags.h>
#include <qtenvironmentvariables.h>
#include <thread>

#include "decoder_host.h"
#include "document_import_transport.h"

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using dsd_qt::DecoderHost;
using dsd_qt::DocumentImportTransport;
using dsd_qt::ImportCopyProgress;

namespace {

int failures = 0;

void
check_at(bool ok, int line, const char* why) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "test_ui_qt_import_transport.cpp:%d: %s\n", line, why);
    }
}

#define CHECK(cond, why) check_at((cond), __LINE__, (why))

void
write_file(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "a fixture file is written");
}

QByteArray
read_file(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/* Nothing but @p expected in @p dir: no copy and no leftover temp file. */
bool
only_entries(const QString& dir, const QStringList& expected) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name) == expected;
}

void
test_local_copy_and_cap() {
    QTemporaryDir src;
    QTemporaryDir dst;
    CHECK(src.isValid() && dst.isValid(), "scratch directories exist");
    const QString source = src.filePath(QStringLiteral("talkgroups.csv"));
    const QByteArray bytes(100000, 'g');
    write_file(source, bytes);
    DocumentImportTransport transport;

    CHECK(transport.progress(1).bytes == -1 && transport.progress(1).displayName.isEmpty(),
          "a request with no copy has no progress");

    const QString exact = dst.filePath(QStringLiteral("exact.csv"));
    CHECK(transport.copy(1, source, exact, bytes.size()) == DecoderHost::DocumentCopied && read_file(exact) == bytes,
          "a source exactly at the cap copies whole");
    const ImportCopyProgress done = transport.progress(1);
    CHECK(done.displayName == QStringLiteral("talkgroups.csv") && done.bytes == bytes.size()
              && done.total == bytes.size() && !done.noSpace,
          "progress names the source file, its size and the bytes copied");
#ifndef _WIN32
    CHECK((QFileInfo(exact).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0,
          "the copy is private to the owner");
#endif

    const QString over = dst.filePath(QStringLiteral("over.csv"));
    CHECK(transport.copy(2, source, over, bytes.size() - 1) == DecoderHost::DocumentTooLarge,
          "a source one byte over the cap is too large");
    CHECK(transport.progress(2).total == bytes.size(), "a refused source still shows its size");

    const QString viaUrl = dst.filePath(QStringLiteral("url.csv"));
    CHECK(transport.copy(3, QUrl::fromLocalFile(source).toString(), viaUrl, bytes.size()) == DecoderHost::DocumentCopied
              && read_file(viaUrl) == bytes,
          "a file URL copies like a plain path");
    CHECK(only_entries(dst.path(), {QStringLiteral("exact.csv"), QStringLiteral("url.csv")}),
          "a refused copy leaves nothing in the destination directory");
}

void
test_refusals() {
    QTemporaryDir dir;
    CHECK(dir.isValid(), "a scratch directory exists");
    const QString source = dir.filePath(QStringLiteral("chan.csv"));
    write_file(source, "1,2\n");
    const QString dest = dir.filePath(QStringLiteral("dest.csv"));
    DocumentImportTransport transport;

    CHECK(transport.copy(1, QStringLiteral("content://com.example.documents/document/primary%3Achan.csv"), dest, 64)
              == DecoderHost::DocumentFailed,
          "the base transport cannot read a content URI");
    CHECK(transport.copy(2, QStringLiteral("chan.csv"), dest, 64) == DecoderHost::DocumentFailed,
          "a relative path is no document");
    CHECK(transport.copy(3, dir.filePath(QStringLiteral("missing.csv")), dest, 64) == DecoderHost::DocumentFailed,
          "a missing source fails");
    CHECK(transport.copy(4, dir.path(), dest, 64) == DecoderHost::DocumentFailed, "a directory is no document");
    CHECK(transport.copy(5, source, QString(), 64) == DecoderHost::DocumentFailed
              && transport.copy(6, source, dest, -1) == DecoderHost::DocumentFailed,
          "an empty destination or a negative cap fails");
    CHECK(!QFileInfo::exists(dest), "a refused copy writes nothing");

    const QString nowhere = dir.filePath(QStringLiteral("missing-dir/dest.csv"));
    CHECK(transport.copy(7, source, nowhere, 64) == DecoderHost::DocumentLocalFailed,
          "a destination directory that is missing is this device's failure");
    CHECK(!transport.progress(7).noSpace, "a missing directory is not a full disk");
}

void
test_cancel_before_and_forget() {
    QTemporaryDir dir;
    CHECK(dir.isValid(), "a scratch directory exists");
    const QString source = dir.filePath(QStringLiteral("group.csv"));
    write_file(source, "1,a\n");
    const QString dest = dir.filePath(QStringLiteral("dest.csv"));
    DocumentImportTransport transport;

    transport.cancel(9);
    CHECK(transport.copy(9, source, dest, 64) == DecoderHost::DocumentCancelled && !QFileInfo::exists(dest),
          "a request cancelled before its copy never copies");
    CHECK(transport.copy(9, source, dest, 64) == DecoderHost::DocumentCancelled,
          "every later copy of a cancelled request is cancelled too");
    CHECK(transport.copy(10, source, dest, 64) == DecoderHost::DocumentCopied, "another request still copies");
    QFile::remove(dest);

    transport.forget(9);
    CHECK(transport.progress(9).bytes == -1, "a forgotten request has no record");
    CHECK(transport.copy(9, source, dest, 64) == DecoderHost::DocumentCopied,
          "a forgotten id starts again without its cancel");
}

/* The base host, made concrete: the hand-out and abort under test are all its own. */
class TestHost : public DecoderHost {
  public:
    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}
};

void
test_host_abort() {
    QTemporaryDir dir;
    CHECK(dir.isValid(), "a scratch directory exists");
    const QString source = dir.filePath(QStringLiteral("keys.csv"));
    write_file(source, "1,2\n");

    TestHost host;
    const std::shared_ptr<DocumentImportTransport> kept = host.importTransport();
    std::shared_ptr<DocumentImportTransport> dropped = host.importTransport();
    CHECK(kept != nullptr && dropped != nullptr && kept != dropped, "each hand-out is a transport of its own");
    CHECK(!kept->aborted(), "a transport starts running");
    dropped.reset();

    host.abortDocumentTransfers();
    CHECK(kept->aborted(), "the host's abort reaches a transport it handed out");
    CHECK(kept->copy(1, source, dir.filePath(QStringLiteral("late.csv")), 64) == DecoderHost::DocumentFailed,
          "an aborted transport fails every later copy");
    const std::shared_ptr<DocumentImportTransport> after = host.importTransport();
    CHECK(after->aborted(), "a transport handed out after the abort starts aborted");
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("late.csv"))), "an aborted copy writes nothing");

    /* The transport outlives the host. */
    std::shared_ptr<DocumentImportTransport> outliving;
    {
        TestHost shortLived;
        outliving = shortLived.importTransport();
    }
    CHECK(outliving->copy(2, source, dir.filePath(QStringLiteral("kept.csv")), 64) == DecoderHost::DocumentCopied,
          "a transport still copies after its host is gone");
}

#ifndef _WIN32
/* Wait until request @p id's running copy has reached @p bytes; false after about five seconds. */
bool
await_bytes(const DocumentImportTransport& transport, qint64 id, qint64 bytes) {
    for (int i = 0; i < 5000; ++i) {
        if (transport.progress(id).bytes == bytes) {
            return true;
        }
        QThread::msleep(1);
    }
    return false;
}

/* Open @p fifo for writing once a reader has it open (blocking writes from then on); -1 after about five seconds
   without one, so a copy that never opens its source fails the case rather than hanging it. */
int
open_fifo_writer(const QString& fifo) {
    const QByteArray path = QFile::encodeName(fifo);
    for (int i = 0; i < 5000; ++i) {
        const int fd = ::open(path.constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            const int flags = fcntl(fd, F_GETFL);
            if (flags >= 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0) {
                return fd;
            }
            ::close(fd);
            return -1;
        }
        if (errno != ENXIO) {
            return -1;
        }
        QThread::msleep(1);
    }
    return -1;
}

enum class Stop { Cancel, Abort };

/*
 * A copy stopped from another thread while it runs: the source is a FIFO the test feeds, so the copy waits in its read
 * at a known point. One full chunk (64 KiB) is read, then the copy is stopped and one more byte released.
 */
void
stop_mid_copy(Stop stop) {
    QTemporaryDir src;
    QTemporaryDir dst;
    CHECK(src.isValid() && dst.isValid(), "scratch directories exist");
    const QString fifo = src.filePath(QStringLiteral("feed.csv"));
    CHECK(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "a FIFO is made");
    const QString dest = dst.filePath(QStringLiteral("dest.csv"));
    DocumentImportTransport transport;
    int result = -1;
    std::thread worker([&transport, &fifo, &dest, &result]() { result = transport.copy(4, fifo, dest, 1 << 20); });

    QFile feed;
    const int writer = open_fifo_writer(fifo);
    CHECK(writer >= 0 && feed.open(writer, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle),
          "the FIFO opens for writing once the copy opens it");
    const QByteArray chunk(1 << 16, 'x');
    CHECK(feed.write(chunk) == chunk.size() && feed.flush(), "one chunk is fed");
    CHECK(await_bytes(transport, 4, chunk.size()), "the copy reports the chunk it copied");
    const ImportCopyProgress running = transport.progress(4);
    CHECK(running.displayName == QStringLiteral("feed.csv") && running.total == -1, "a stream has a name but no size");
    CHECK(transport.copy(4, fifo, dst.filePath(QStringLiteral("second.csv")), 64) == DecoderHost::DocumentFailed,
          "a second copy of a request that is copying is refused");

    if (stop == Stop::Cancel) {
        transport.cancel(4);
    } else {
        transport.abort();
    }
    CHECK(feed.write("y", 1) == 1 && feed.flush(), "one more byte is fed");
    feed.close();
    worker.join();
    if (stop == Stop::Cancel) {
        CHECK(result == DecoderHost::DocumentCancelled, "a copy cancelled while it runs is cancelled");
    } else {
        CHECK(result == DecoderHost::DocumentFailed, "a copy aborted while it runs fails");
    }
    CHECK(only_entries(dst.path(), {}), "a stopped copy leaves nothing in the destination directory");
}
#endif

/*
 * A copy onto a full disk. It runs only when DSD_TEST_FULL_DIR names a directory on a filesystem with less than 1 MiB
 * free (for instance a small tmpfs in a user and mount namespace: unshare -rm).
 */
void
test_full_disk() {
    const QString full = qEnvironmentVariable("DSD_TEST_FULL_DIR");
    if (full.isEmpty()) {
        return;
    }
    QTemporaryDir src;
    CHECK(src.isValid(), "a scratch directory exists");
    const QString source = src.filePath(QStringLiteral("big.csv"));
    write_file(source, QByteArray(1 << 20, 'z'));
    DocumentImportTransport transport;
    CHECK(transport.copy(1, source, QDir(full).filePath(QStringLiteral("big.csv")), 1 << 21)
              == DecoderHost::DocumentLocalFailed,
          "a copy onto a full disk is this device's failure");
    CHECK(transport.progress(1).noSpace, "and says the device ran out of space");
    std::fprintf(stderr, "full-disk case ran in %s\n", qPrintable(full));
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_local_copy_and_cap();
    test_refusals();
    test_cancel_before_and_forget();
    test_host_abort();
#ifndef _WIN32
    stop_mid_copy(Stop::Cancel);
    stop_mid_copy(Stop::Abort);
#endif
    test_full_disk();
    return failures ? 1 : 0;
}
