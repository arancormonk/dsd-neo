// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the import copies' base transport (#832), its probe and save (#831), and the host's hand-out and abort of
 * it. */

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
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using dsd_qt::DecoderHost;
using dsd_qt::DocumentImportTransport;
using dsd_qt::ImportCopyProgress;
using dsd_qt::ImportProbe;
using dsd_qt::kProbeHeaderBytes;

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

/* A document that starts like a backup: the 8-byte magic, then @p size - 8 more bytes. */
QByteArray
backup_like(int size) {
    QByteArray bytes("DSDNEOBK");
    bytes.append(QByteArray(size - bytes.size(), 'b'));
    return bytes;
}

/*
 * The base transport's probe (#831): a document's first kProbeHeaderBytes and name; its length, counted on the bytes
 * and capped at maxCount + 1, only after the backup magic; read-only.
 */
void
test_probe_local() {
    QTemporaryDir dir;
    CHECK(dir.isValid(), "a scratch directory exists");
    DocumentImportTransport transport;
    ImportProbe probe;

    const QString csv = dir.filePath(QStringLiteral("groups.csv"));
    const QByteArray csvBytes = QByteArray("TG,Mode,Name\n") + QByteArray(500, 'c');
    write_file(csv, csvBytes);
    CHECK(transport.probe(1, csv, &probe, 1000) == DecoderHost::DocumentCopied, "a CSV probes");
    CHECK(probe.header == csvBytes.left(kProbeHeaderBytes) && probe.total == -1
              && probe.displayName == QStringLiteral("groups.csv"),
          "a document with no backup magic gives its header and name, and no length");
    CHECK(transport.progress(1).displayName == QStringLiteral("groups.csv"), "progress names the probed document");

    const QString shortFile = dir.filePath(QStringLiteral("short.txt"));
    write_file(shortFile, "1,a\n");
    CHECK(transport.probe(2, QUrl::fromLocalFile(shortFile).toString(), &probe, 1000) == DecoderHost::DocumentCopied
              && probe.header == "1,a\n" && probe.total == -1,
          "a file shorter than a header (a file URL) gives what it holds");
    const QString empty = dir.filePath(QStringLiteral("empty"));
    write_file(empty, QByteArray());
    CHECK(transport.probe(3, empty, &probe, 1000) == DecoderHost::DocumentCopied && probe.header.isEmpty()
              && probe.total == -1,
          "an empty file probes empty");

    const QString backup = dir.filePath(QStringLiteral("app.dsdbak"));
    write_file(backup, backup_like(1000));
    CHECK(transport.probe(4, backup, &probe, 5000) == DecoderHost::DocumentCopied
              && probe.header == backup_like(1000).left(kProbeHeaderBytes) && probe.total == 1000
              && probe.displayName == QStringLiteral("app.dsdbak"),
          "a document with the backup magic is counted to its end");
    CHECK(transport.probe(5, backup, &probe, 1000) == DecoderHost::DocumentCopied && probe.total == 1000,
          "a document exactly at the cap counts whole");
    CHECK(transport.probe(6, backup, &probe, 999) == DecoderHost::DocumentCopied && probe.total == 1000,
          "one byte over the cap counts to cap + 1");
    CHECK(transport.probe(7, backup, &probe, 100) == DecoderHost::DocumentCopied && probe.total == 101,
          "a document far over the cap stops counting at cap + 1");
    const QString cut = dir.filePath(QStringLiteral("cut.dsdbak"));
    write_file(cut, backup_like(20));
    CHECK(transport.probe(8, cut, &probe, 1000) == DecoderHost::DocumentCopied && probe.header == backup_like(20)
              && probe.total == 20,
          "a backup cut inside its header gives its whole length");

    CHECK(only_entries(dir.path(), {QStringLiteral("app.dsdbak"), QStringLiteral("cut.dsdbak"), QStringLiteral("empty"),
                                    QStringLiteral("groups.csv"), QStringLiteral("short.txt")}),
          "a probe writes nothing");
    CHECK(read_file(backup) == backup_like(1000), "and changes nothing it reads");
}

void
test_probe_refusals() {
    QTemporaryDir dir;
    CHECK(dir.isValid(), "a scratch directory exists");
    const QString source = dir.filePath(QStringLiteral("chan.csv"));
    write_file(source, "1,2\n");
    DocumentImportTransport transport;
    ImportProbe probe;
    probe.total = 42;

    CHECK(transport.probe(1, QStringLiteral("content://com.example.documents/document/chan.csv"), &probe, 64)
              == DecoderHost::DocumentFailed,
          "the base transport cannot probe a content URI");
    CHECK(probe.total == -1 && probe.header.isEmpty(), "a probe starts from an empty result");
    CHECK(transport.probe(2, QStringLiteral("chan.csv"), &probe, 64) == DecoderHost::DocumentFailed,
          "a relative path is no document");
    CHECK(transport.probe(3, dir.filePath(QStringLiteral("missing.csv")), &probe, 64) == DecoderHost::DocumentFailed,
          "a missing document fails");
    CHECK(transport.probe(4, dir.path(), &probe, 64) == DecoderHost::DocumentFailed, "a directory is no document");
    CHECK(transport.probe(5, source, &probe, -1) == DecoderHost::DocumentFailed
              && transport.probe(6, source, nullptr, 64) == DecoderHost::DocumentFailed,
          "a negative cap or no result fails");

    transport.cancel(7);
    CHECK(transport.probe(7, source, &probe, 64) == DecoderHost::DocumentCancelled && probe.header.isEmpty(),
          "a request cancelled before its probe never reads");
    CHECK(transport.copy(7, source, dir.filePath(QStringLiteral("x.csv")), 64) == DecoderHost::DocumentCancelled,
          "and copies nothing later either");
    transport.forget(7);
    CHECK(transport.probe(7, source, &probe, 64) == DecoderHost::DocumentCopied && probe.header == "1,2\n",
          "a forgotten id probes again");

    transport.abort();
    CHECK(transport.probe(8, source, &probe, 64) == DecoderHost::DocumentFailed && probe.header.isEmpty(),
          "an aborted transport probes nothing");
}

/* The base transport's save (#831): a local file written whole into a file URL or a plain path, private, or nothing. */
void
test_write_from_file() {
    QTemporaryDir dir;
    CHECK(dir.isValid(), "a scratch directory exists");
    const QString source = dir.filePath(QStringLiteral("export.dsdbak"));
    write_file(source, "sealed");
    DocumentImportTransport transport;

    const QString saved = dir.filePath(QStringLiteral("saved.dsdbak"));
    CHECK(transport.writeFromFile(source, QUrl::fromLocalFile(saved).toString()) && read_file(saved) == "sealed",
          "a file URL is written whole");
    CHECK((QFile::permissions(saved) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0,
          "the saved document is the owner's alone");
    const QString plain = dir.filePath(QStringLiteral("plain.dsdbak"));
    CHECK(transport.writeFromFile(source, plain) && read_file(plain) == "sealed", "a plain path is written whole");
    const QString none = dir.filePath(QStringLiteral("none.dsdbak"));
    CHECK(!transport.writeFromFile(dir.filePath(QStringLiteral("absent")), none) && !QFileInfo::exists(none),
          "a missing source writes nothing");
    CHECK(!transport.writeFromFile(source, QStringLiteral("content://x/y")), "an opaque document is refused");
    transport.discardWrite(QStringLiteral("content://x/y")); // nothing to drop: the base transport has no rows

    transport.abort();
    const QString late = dir.filePath(QStringLiteral("late.dsdbak"));
    CHECK(!transport.writeFromFile(source, late) && !QFileInfo::exists(late), "an aborted transport writes nothing");
    CHECK(only_entries(dir.path(), {QStringLiteral("export.dsdbak"), QStringLiteral("plain.dsdbak"),
                                    QStringLiteral("saved.dsdbak")}),
          "no failed save leaves a file behind");
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
    /* A write after the copy closed its end must fail with EPIPE, not kill the test. */
    std::signal(SIGPIPE, SIG_IGN);
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
    /* The copy is either still blocked in its read, which one more byte wakes, or it has already seen the stop and
       closed the FIFO, in which case this write meets EPIPE. Both are the expected end, so the result is not checked;
       what the case asserts is the copy's outcome below. */
    (void)feed.write("y", 1);
    (void)feed.flush();
    feed.close();
    worker.join();
    if (stop == Stop::Cancel) {
        CHECK(result == DecoderHost::DocumentCancelled, "a copy cancelled while it runs is cancelled");
    } else {
        CHECK(result == DecoderHost::DocumentFailed, "a copy aborted while it runs fails");
    }
    CHECK(only_entries(dst.path(), {}), "a stopped copy leaves nothing in the destination directory");
}

/* Write @p bytes into the FIFO a probe reads; false on failure. */
bool
feed_fifo(QFile& feed, const QByteArray& bytes) {
    return feed.write(bytes) == bytes.size() && feed.flush();
}

/*
 * A probe reads no further than a document's header unless it starts with the backup magic: from a FIFO whose writer
 * stays open (no end in sight), a probe without the magic returns after the header, while one with it reads on.
 */
void
test_probe_stops_without_magic() {
    QTemporaryDir src;
    CHECK(src.isValid(), "a scratch directory exists");
    const QString fifo = src.filePath(QStringLiteral("feed.csv"));
    CHECK(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "a FIFO is made");
    std::signal(SIGPIPE, SIG_IGN);
    DocumentImportTransport transport;
    ImportProbe probe;
    int result = -1;
    std::thread worker([&transport, &fifo, &probe, &result]() { result = transport.probe(1, fifo, &probe, 1 << 20); });

    QFile feed;
    const int writer = open_fifo_writer(fifo);
    CHECK(writer >= 0 && feed.open(writer, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle),
          "the FIFO opens for writing once the probe opens it");
    /* The header in small writes, as a provider's short reads give it, and a few bytes past it; the stream never
     * ends, since the writer stays open. The last write is the one that completes the header, so each lands before the
     * probe can stop reading. */
    const QByteArray csv = QByteArray("TG,Mode,Name\n") + QByteArray(57, 'c');
    for (int at = 0; at < csv.size(); at += 10) {
        CHECK(feed_fifo(feed, csv.mid(at, 10)), "a piece is fed");
    }
    worker.join(); // Hangs here if the probe read on to an end that never comes.
    CHECK(result == DecoderHost::DocumentCopied && probe.header == csv.left(kProbeHeaderBytes) && probe.total == -1,
          "a probe without the magic stops after the header, however it arrives");
    feed.close();
}

/* A probe stopped from another thread while it counts a backup: it reads the header, then waits in the count. */
void
stop_mid_probe(Stop stop) {
    QTemporaryDir src;
    CHECK(src.isValid(), "a scratch directory exists");
    const QString fifo = src.filePath(QStringLiteral("feed.dsdbak"));
    CHECK(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "a FIFO is made");
    std::signal(SIGPIPE, SIG_IGN);
    DocumentImportTransport transport;
    ImportProbe probe;
    int result = -1;
    std::thread worker([&transport, &fifo, &probe, &result]() { result = transport.probe(5, fifo, &probe, 1 << 20); });

    QFile feed;
    const int writer = open_fifo_writer(fifo);
    CHECK(writer >= 0 && feed.open(writer, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle),
          "the FIFO opens for writing once the probe opens it");
    CHECK(feed_fifo(feed, backup_like(kProbeHeaderBytes)), "a backup's header is fed");
    CHECK(await_bytes(transport, 5, kProbeHeaderBytes), "the probe reads the header and goes on to count");
    if (stop == Stop::Cancel) {
        transport.cancel(5);
    } else {
        transport.abort();
    }
    /* As in stop_mid_copy(): a byte wakes a read still blocked; a write after the probe closed meets EPIPE. */
    (void)feed.write("y", 1);
    (void)feed.flush();
    feed.close();
    worker.join();
    if (stop == Stop::Cancel) {
        CHECK(result == DecoderHost::DocumentCancelled, "a probe cancelled while it counts is cancelled");
    } else {
        CHECK(result == DecoderHost::DocumentFailed, "a probe aborted while it counts fails");
    }
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
    test_write_from_file();
    test_probe_local();
    test_probe_refusals();
    test_host_abort();
#ifndef _WIN32
    stop_mid_copy(Stop::Cancel);
    stop_mid_copy(Stop::Abort);
    test_probe_stops_without_magic();
    stop_mid_probe(Stop::Cancel);
    stop_mid_probe(Stop::Abort);
#endif
    test_full_disk();
    return failures ? 1 : 0;
}
