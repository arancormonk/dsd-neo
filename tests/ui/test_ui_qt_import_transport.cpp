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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <qflags.h>
#include <qtenvironmentvariables.h>
#include <thread>
#include <utility>

#include "decoder_host.h"
#include "document_import_transport.h"

#ifndef _WIN32
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

/* What a GatedSource serves past its first chunk: one more byte, once the test lets it go. */
struct SourceGate {
    std::mutex lock;
    std::condition_variable changed;
    bool released = false;
};

/*
 * A source that serves one full chunk (64 KiB), then waits in its read until the gate opens and serves one byte more:
 * the copy reading it stops at a known point. The wait is bounded, so a case that never opens the gate fails rather
 * than hangs.
 */
class GatedSource : public QIODevice {
  public:
    explicit GatedSource(std::shared_ptr<SourceGate> gate) : m_gate(std::move(gate)) {}

    bool
    isSequential() const override {
        return true;
    }

  protected:
    qint64
    readData(char* data, qint64 maxSize) override {
        if (m_served < kChunk) {
            const qint64 n = qMin(maxSize, kChunk - m_served);
            std::fill(data, data + n, 'x');
            m_served += n;
            return n;
        }
        if (m_served > kChunk || maxSize < 1) {
            return 0;
        }
        std::unique_lock<std::mutex> guard(m_gate->lock);
        if (!m_gate->changed.wait_for(guard, std::chrono::seconds(10), [this] { return m_gate->released; })) {
            return -1;
        }
        data[0] = 'y';
        ++m_served;
        return 1;
    }

    qint64
    writeData(const char* data, qint64 maxSize) override {
        (void)data;
        (void)maxSize;
        return -1;
    }

  private:
    static constexpr qint64 kChunk = 1 << 16;
    std::shared_ptr<SourceGate> m_gate;
    qint64 m_served = 0;
};

/* The base transport, copying from a GatedSource in place of the file it was named: the rest is production code. */
class GatedTransport : public DocumentImportTransport {
  public:
    explicit GatedTransport(std::shared_ptr<SourceGate> gate) : m_gate(std::move(gate)) {}

  protected:
    std::unique_ptr<QIODevice>
    openSource(const QString& path) override {
        (void)path;
        auto source = std::make_unique<GatedSource>(m_gate);
        if (!source->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            return nullptr;
        }
        return source;
    }

  private:
    std::shared_ptr<SourceGate> m_gate;
};

enum class Stop { Cancel, Abort };

/*
 * A copy stopped from another thread while it runs: the source serves one full chunk (64 KiB) and then waits in its
 * read, so the copy is at a known point; the copy is stopped there and one more byte released. The file named is a
 * regular one of 65537 bytes, the size the source serves.
 */
void
stop_mid_copy(Stop stop) {
    QTemporaryDir src;
    QTemporaryDir dst;
    CHECK(src.isValid() && dst.isValid(), "scratch directories exist");
    const QString source = src.filePath(QStringLiteral("feed.csv"));
    write_file(source, QByteArray((1 << 16) + 1, 'x'));
    const QString dest = dst.filePath(QStringLiteral("dest.csv"));
    const auto gate = std::make_shared<SourceGate>();
    GatedTransport transport(gate);
    int result = -1;
    std::thread worker([&transport, &source, &dest, &result]() { result = transport.copy(4, source, dest, 1 << 20); });

    CHECK(await_bytes(transport, 4, 1 << 16), "the copy reports the chunk it copied");
    const ImportCopyProgress running = transport.progress(4);
    CHECK(running.displayName == QStringLiteral("feed.csv") && running.total == 65537,
          "a running copy shows the file's name and size");
    CHECK(transport.copy(4, source, dst.filePath(QStringLiteral("second.csv")), 64) == DecoderHost::DocumentFailed,
          "a second copy of a request that is copying is refused");

    if (stop == Stop::Cancel) {
        transport.cancel(4);
    } else {
        transport.abort();
    }
    {
        const std::lock_guard<std::mutex> guard(gate->lock);
        gate->released = true;
    }
    gate->changed.notify_all();
    worker.join();
    if (stop == Stop::Cancel) {
        CHECK(result == DecoderHost::DocumentCancelled, "a copy cancelled while it runs is cancelled");
    } else {
        CHECK(result == DecoderHost::DocumentFailed, "a copy aborted while it runs fails");
    }
    CHECK(only_entries(dst.path(), {}), "a stopped copy leaves nothing in the destination directory");
}

#ifndef _WIN32

/*
 * Copy @p source (a FIFO, or a name that leads to one) with nothing writing to it. The copy must refuse it without
 * waiting for a writer: one that blocks is let go after 2 s by a writer that opens and closes at once, and fails the
 * case.
 */
void
expect_fifo_refused(const QString& source, const QString& fifo, const char* why) {
    QTemporaryDir dst;
    CHECK(dst.isValid(), "a scratch directory exists");
    DocumentImportTransport transport;
    std::atomic<bool> returned{false};
    int result = -1;
    std::thread worker([&] {
        result = transport.copy(9, source, dst.filePath(QStringLiteral("dest.csv")), 1 << 20);
        returned.store(true);
    });
    for (int i = 0; i < 2000 && !returned.load(); ++i) {
        QThread::msleep(1);
    }
    if (!returned.load()) {
        const int writer = ::open(QFile::encodeName(fifo).constData(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (writer >= 0) {
            ::close(writer);
        }
        worker.join();
        CHECK(false, "a FIFO with no writer is refused without blocking");
    } else {
        worker.join();
        CHECK(result == DecoderHost::DocumentFailed, why);
    }
    CHECK(only_entries(dst.path(), {}), "a refused FIFO leaves nothing in the destination directory");
}

/* #830 B1: a FIFO is no document. Opening one with no writer waited for a writer forever, holding the import worker
   and every import queued behind it. */
void
test_fifo_without_writer_is_refused() {
    QTemporaryDir src;
    CHECK(src.isValid(), "a scratch directory exists");
    const QString fifo = src.filePath(QStringLiteral("feed.csv"));
    CHECK(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "a FIFO is made");
    expect_fifo_refused(fifo, fifo, "a FIFO is refused as no document");
}

/* The same through a symbolic link: the link's name looks like a CSV file, the file it leads to is a FIFO. */
void
test_symlink_to_fifo_is_refused() {
    QTemporaryDir src;
    CHECK(src.isValid(), "a scratch directory exists");
    const QString fifo = src.filePath(QStringLiteral("feed.csv"));
    const QString link = src.filePath(QStringLiteral("link.csv"));
    CHECK(mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0, "a FIFO is made");
    CHECK(QFile::link(fifo, link), "a link to the FIFO is made");
    expect_fifo_refused(link, fifo, "a link to a FIFO is refused as no document");
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
    stop_mid_copy(Stop::Cancel);
    stop_mid_copy(Stop::Abort);
#ifndef _WIN32
    test_fifo_without_writer_is_refused();
    test_symlink_to_fifo_is_refused();
#endif
    test_full_disk();
    return failures ? 1 : 0;
}
