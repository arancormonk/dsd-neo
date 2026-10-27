// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArrayView>
#include <QChar>
#include <QFlags>
#include <QHash>
#include <QIODevice>
#include <QLatin1String>
#include <QVariant>
#include <Qt>
#include <stddef.h>
#include <utility>
#include "diagnostics_log.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <algorithm>
#include <chrono>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>
#include <limits>
#include "realtime_clock.h"

namespace dsd_qt {
namespace {
constexpr int tailLimit = 256 * 1024;
constexpr int ringLimit = 2000;
constexpr int queueLimit = 2000;
constexpr int flushResultLimit = 64;
// At most this many ring lines are kept at the front through eviction (see appendRingLocked()).
constexpr int pinnedLimit = 8;
const char kTailName[] = "tail.log";
const char kPrevName[] = "tail.prev.log";
constexpr QByteArrayView kProcessPrefix = "[process] [info] pid ";
constexpr QByteArrayView kSessionPrefix = "[session] [info] --- session starting";

struct HostDiagnostics {
    std::mutex mutex;
    QStringList pending;
    bool ready = false;
    DiagnosticsLog::PlatformFactsProvider provider = nullptr;
};

HostDiagnostics&
hostDiagnostics() {
    // JNI producers may arrive before Qt initialization and outlive its teardown.
    static auto* host = new HostDiagnostics;
    return *host;
}

/** @brief The newest @p limit bytes of @p text, cut at a line start. */
QByteArray
lineAlignedTail(QByteArray text, qint64 limit) {
    if (text.size() > limit) {
        text = text.right(limit);
        text.remove(0, text.indexOf('\n') + 1);
    }
    return text;
}

QByteArray
boundedTail(QByteArray text) {
    return lineAlignedTail(std::move(text), tailLimit);
}

/** @brief Drops an unterminated last record: what a crash part-way through an append leaves. */
QByteArray
withoutFragment(QByteArray text) {
    if (!text.isEmpty() && !text.endsWith('\n')) {
        text.truncate(text.lastIndexOf('\n') + 1);
    }
    return text;
}

/** @brief Up to @p limit trailing bytes of the file at @p path; empty if it cannot be read. */
QByteArray
readTail(const QString& path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    if (file.size() > limit && !file.seek(file.size() - limit)) {
        return {};
    }
    return file.readAll();
}

/** @brief Replaces @p path with @p bytes: data synced before the rename, the rename synced after it. */
bool
replaceFile(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        return false;
    }
    dsd_fsync_parent_dir(QFile::encodeName(path).constData());
    return true;
}

/** @brief The "boot N" field of a process record, or -1 when it has none. */
qint64
bootOf(const QByteArray& record) {
    static const QRegularExpression boot(QStringLiteral("\\bboot (\\d+)\\b"));
    const auto match = boot.match(QString::fromUtf8(record));
    return match.hasMatch() ? match.captured(1).toLongLong() : -1;
}

QByteArray
formatRecord(const QString& source, const QString& level, const QString& text) {
    auto line = DiagnosticsLog::redact(QStringLiteral("[%1] [%2] %3").arg(source, level, text)).toUtf8();
    if (line.size() > 512) {
        int end = 512;
        while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80) {
            --end;
        }
        line.truncate(end);
    }
    return line;
}

QString
baseProcessFacts() {
    QString facts = QStringLiteral("%1, %2 (%3)")
                        .arg(QCoreApplication::applicationPid())
                        .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    const QString version = QCoreApplication::applicationVersion();
    if (!version.isEmpty()) {
        facts += QStringLiteral(", app ") + version;
    }
    return facts;
}

/**
 * @brief The context lines at @p contextAt that the ring limit drops from @p lines, and the first kept line.
 *
 * Every slot a context line takes moves the cutoff, so the set is settled against the final cutoff.
 */
QStringList
droppedContext(const QList<QByteArray>& lines, std::initializer_list<qsizetype> contextAt, qsizetype* keepFrom) {
    QStringList context;
    for (int reserved = 0;;) {
        *keepFrom = std::max<qsizetype>(0, lines.size() - (ringLimit - 1 - reserved));
        context.clear();
        for (const qsizetype at : contextAt) {
            if (at >= 0 && at < *keepFrom) {
                context.append(DiagnosticsLog::redact(QString::fromUtf8(lines.at(at))));
            }
        }
        if (context.size() == reserved) {
            return context;
        }
        reserved = static_cast<int>(context.size());
    }
}

void
capture(dsd_neo_log_level_t level, const char* text, void* ctx) {
    static const char* levels[] = {"error", "warn", "info", "debug"};
    static_cast<DiagnosticsLog*>(ctx)->submit(QStringLiteral("decoder"),
                                              QString::fromLatin1(levels[std::min(static_cast<int>(level), 3)]),
                                              QString::fromUtf8(text));
}
} // namespace

DiagnosticsLog&
DiagnosticsLog::instance() {
    // Intentionally process-lifetime: producers and the installed tap can outlive Qt UI objects.
    static auto* log = new DiagnosticsLog(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                          + QStringLiteral("/diagnostics"));
    return *log;
}

void
DiagnosticsLog::setPlatformFactsProvider(PlatformFactsProvider provider) {
    auto& host = hostDiagnostics();
    std::lock_guard<std::mutex> lock(host.mutex);
    host.provider = provider;
}

void
DiagnosticsLog::installTap() {
    static std::once_flag installed;
    std::call_once(installed, [] {
        auto& host = hostDiagnostics();
        PlatformFactsProvider provider = nullptr;
        {
            std::lock_guard<std::mutex> lock(host.mutex);
            provider = host.provider;
        }
        // Outside the host lock: the Android provider calls into the platform.
        QString facts = baseProcessFacts();
        QStringList following;
        if (provider) {
            auto extra = provider();
            if (!extra.first.isEmpty()) {
                facts += QStringLiteral(", ") + extra.first;
            }
            following = std::move(extra.second);
        }
        std::lock_guard<std::mutex> lock(host.mutex);
        auto& log = instance();
        dsd_neo_log_set_tap(capture, &log);
        // The process line first: it is where this run starts in the saved tail.
        log.noteProcess(facts, following);
        for (const auto& text : host.pending) {
            log.submit(QStringLiteral("host"), QStringLiteral("info"), text);
        }
        host.pending.clear();
        host.ready = true;
    });
}

void
DiagnosticsLog::submitHostDiagnostic(const QString& text) {
    auto& host = hostDiagnostics();
    std::lock_guard<std::mutex> lock(host.mutex);
    if (!host.ready) {
        // Do not resolve AppDataLocation before Qt has its application identity.
        // Only bounded, redacted records can survive until tap installation.
        host.pending.append(redact(text).left(512));
        if (host.pending.size() > 2000) {
            host.pending.removeFirst();
        }
        return;
    }
    instance().submit(QStringLiteral("host"), QStringLiteral("info"), text);
}

QString
DiagnosticsLog::redact(QString text) {
    // Drop whole key-bearing records, including malformed values and labelled
    // short decimal/basic keys. Strip ANSI before matching to prevent evasion.
    static const QRegularExpression ansi(QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]"));
    text.remove(ansi);
    static const QRegularExpression labelled(QStringLiteral(
        "(?i)(key|secret|password|token|\\b(?:K[1-4]|RC4|AES|Hytera|scrambler)\\b|(?:^|\\s)-(?:H|b|R|1|!)\\S*)"));
    if (text.contains(labelled)) {
        return QStringLiteral("[redacted sensitive diagnostic]");
    }
    static const QRegularExpression hex(QStringLiteral("(?i)[0-9a-f]{10,}"));
    text.replace(hex, QStringLiteral("[redacted]"));
    text.replace(QChar('\r'), QChar(' '));
    text.replace(QChar('\n'), QChar(' '));
    return text;
}

DiagnosticsLog::DiagnosticsLog(const QString& directory) : DiagnosticsLog(directory, TestOptions{}) {}

DiagnosticsLog::DiagnosticsLog(const QString& directory, TestOptions options)
    : m_directory(directory), m_options(std::move(options)) {
    QDir().mkpath(directory);
    load();
    if (!m_options.manualDrain) {
        m_writer = std::thread(&DiagnosticsLog::writeLoop, this);
    }
}

DiagnosticsLog::~DiagnosticsLog() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_stop = true;
    if (m_options.manualDrain) {
        step(lock, true);
        return;
    }
    lock.unlock();
    m_ready.notify_one();
    m_writer.join();
}

QString
DiagnosticsLog::filePath(const char* name) const {
    return m_directory + QLatin1Char('/') + QLatin1String(name);
}

quint64
DiagnosticsLog::nowMs() const {
    return m_options.clockMs ? m_options.clockMs() : static_cast<quint64>(dsd_realtime_mono_ms());
}

bool
DiagnosticsLog::inStartupWindow(quint64 now) const {
    return now < m_startupUntil;
}

void
DiagnosticsLog::load() {
    cleanDirectory();
    seedRing(loadSegments());
}

void
DiagnosticsLog::cleanDirectory() const {
    QDir dir(m_directory);
    // Temporaries a kill or crash left behind: QSaveFile's from older builds' whole-tail rewrites and from
    // normalization below. Nothing else in the directory is touched.
    static const QRegularExpression orphan(QStringLiteral("^tail(\\.prev)?\\.log\\.[A-Za-z0-9]{6}$"));
    for (const auto& name : dir.entryList(QDir::Files | QDir::Hidden)) {
        if (orphan.match(name).hasMatch()) {
            dir.remove(name);
        }
    }
    for (const auto& path : {filePath(kPrevName), filePath(kTailName)}) {
        QFileInfo info(path);
        if (info.exists() && info.lastModified().secsTo(realtimeCurrentDateTime()) > 7LL * 24 * 3600) {
            QFile::remove(path);
        }
    }
}

QByteArray
DiagnosticsLog::loadSegments() const {
    const QString tailPath = filePath(kTailName);
    const QString prevPath = filePath(kPrevName);
    QByteArray prev = withoutFragment(readTail(prevPath, 2LL * tailLimit));
    QByteArray tail = withoutFragment(readTail(tailPath, 2LL * tailLimit));
    if (prev.size() > kSegmentLimit || tail.size() > kSegmentLimit) {
        // An older build's single tail (up to 256 KiB), or anything else over a segment: split at line starts
        // into the two bounded segments, keeping no more than before. A current segment over the cap is an
        // older build's tail that was never split (this writer rotates first), so it alone is the record: a
        // previous segment beside it can only be the prefix a split interrupted by a crash already wrote.
        const QByteArray all = boundedTail(tail.size() > kSegmentLimit ? tail : prev + tail);
        tail = lineAlignedTail(all, kSegmentLimit);
        prev = lineAlignedTail(all.left(all.size() - tail.size()), kSegmentLimit);
        // The current segment is rewritten only once the previous one holds its prefix: until then the unsplit tail
        // stays the record, and the next load splits it again.
        const bool prevWritten =
            prev.isEmpty() ? (!QFileInfo::exists(prevPath) || QFile::remove(prevPath)) : replaceFile(prevPath, prev);
        if (prevWritten) {
            (void)replaceFile(tailPath, tail);
        }
    }
    return boundedTail(prev + tail);
}

void
DiagnosticsLog::seedRing(const QByteArray& text) {
    QList<QByteArray> lines;
    for (const auto& line : text.split('\n')) {
        // NUL bytes are what a crash leaves where data the filesystem had sized was never written.
        if (!line.isEmpty() && !line.contains('\0')) {
            lines.append(line);
        }
    }
    if (lines.isEmpty()) {
        return;
    }
    // The previous run's identity, read before the ring limit can drop it. A session line belongs to the process line
    // before it, so a newer process line forgets an older run's session.
    qsizetype lastProcessAt = -1;
    qsizetype lastSessionAt = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(kProcessPrefix)) {
            lastProcessAt = i;
            lastSessionAt = -1;
        } else if (lines.at(i).startsWith(kSessionPrefix)) {
            lastSessionAt = i;
        }
    }
    if (lastProcessAt >= 0) {
        m_previousProcessLine = lines.at(lastProcessAt);
    }

    // Context the ring limit would drop goes back in right after the divider, so Copy and Share keep
    // which process, boot and session the remaining lines belong to.
    qsizetype keepFrom = 0;
    const QStringList context = droppedContext(lines, {lastProcessAt, lastSessionAt}, &keepFrom);
    // The divider and the previous run's identity (those very lines, wherever they sit) stay at the front while this
    // run's records arrive.
    m_protectedPrevious.append(pushRingLocked(QStringLiteral("--- previous run ---")));
    for (const auto& line : context) {
        m_protectedPrevious.append(pushRingLocked(line));
    }
    for (qsizetype i = keepFrom; i < lines.size(); ++i) {
        const quint64 id = pushRingLocked(redact(QString::fromUtf8(lines.at(i))));
        if (i == lastProcessAt || i == lastSessionAt) {
            m_protectedPrevious.append(id);
        }
    }
    m_generation = static_cast<quint64>(m_ring.size());
    m_pinned = static_cast<int>(1 + context.size());
}

void
DiagnosticsLog::enqueue(const QByteArray& record, Pin pin) {
    // Queue lock orders concurrent submit operations, but no I/O holds it.
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    {
        std::lock_guard<std::mutex> ringLock(m_ringMutex);
        appendRingLocked(QString::fromUtf8(record), pin);
    }
    // Bounded backpressure: retaining newest queued records matches ring semantics.
    if (m_queue.size() >= queueLimit) {
        m_queue.pop_front();
    }
    m_queue.push_back(record + '\n');
    m_ready.notify_one();
}

void
DiagnosticsLog::ringNote(const QString& line) {
    std::lock_guard<std::mutex> ringLock(m_ringMutex);
    appendRingLocked(line);
}

quint64
DiagnosticsLog::pushRingLocked(const QString& line) {
    m_ring.append(line);
    m_ringIds.append(++m_lastRingId);
    return m_lastRingId;
}

void
DiagnosticsLog::appendRingLocked(const QString& line, Pin pin) {
    const quint64 id = pushRingLocked(line);
    if (pin == Pin::Process) {
        m_protectedProcess = id;
    } else if (pin == Pin::Session) {
        m_protectedSession = id;
    }
    while (m_ring.size() > ringLimit) {
        // A pinned line no longer protected (a session line a newer one replaced) is the oldest line: it goes first.
        int released = -1;
        for (int i = 0; i < m_pinned && released < 0; ++i) {
            if (!isProtectedLocked(i)) {
                released = i;
            }
        }
        if (released >= 0) {
            m_ring.removeAt(released);
            m_ringIds.removeAt(released);
            --m_pinned;
            continue;
        }
        if (m_pinned >= m_ring.size()) {
            break;
        }
        // Otherwise evict the oldest unpinned line; a protected line reaching the front is pinned there instead.
        // Repeats of a protected line's text (the same source started again) are other lines, evicted like any other.
        if (m_pinned < pinnedLimit && isProtectedLocked(m_pinned)) {
            ++m_pinned;
            continue;
        }
        m_ring.removeAt(m_pinned);
        m_ringIds.removeAt(m_pinned);
    }
    ++m_generation;
}

bool
DiagnosticsLog::isProtectedLocked(int index) const {
    const quint64 id = m_ringIds.at(index);
    return id == m_protectedProcess || id == m_protectedSession || m_protectedPrevious.contains(id);
}

void
DiagnosticsLog::submit(const QString& source, const QString& level, const QString& text) {
    enqueue(formatRecord(source, level, text));
}

void
DiagnosticsLog::noteProcess(const QString& facts, const QStringList& followingLines) {
    const QByteArray record =
        formatRecord(QStringLiteral("process"), QStringLiteral("info"), QStringLiteral("pid ") + facts);
    QByteArray previous;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_processNoted) {
            return;
        }
        m_processNoted = true;
        m_processLine = record;
        previous = m_previousProcessLine;
    }
    enqueue(record, Pin::Process);
    for (const auto& line : followingLines) {
        submit(QStringLiteral("process"), QStringLiteral("info"), line);
    }
    // A fact about the log, not a cause: the boot counter moved between the previous run's process line and this one.
    const qint64 before = bootOf(previous);
    const qint64 now = bootOf(record);
    if (before >= 0 && now >= 0 && before != now) {
        submit(QStringLiteral("process"), QStringLiteral("info"),
               QStringLiteral("previous run: Android has restarted since (boot %1 -> %2)").arg(before).arg(now));
    }
}

void
DiagnosticsLog::noteSessionAttempt(const QString& context, AttemptStage stage) {
    const quint64 now = nowMs();
    const bool usbAccess = !context.isEmpty() && stage == AttemptStage::UsbAccess;
    bool mark = !usbAccess;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (context.isEmpty()) {
            // The plain marker of the Starting transition, unless an explicit start note just described this attempt.
            mark = !(m_attemptAbsorb && now - m_startNoteAt < kAttemptDedupeMs);
            m_attemptAbsorb = false;
        } else if (usbAccess) {
            m_attemptAbsorb = false;
        } else {
            m_startNoteAt = now;
            m_attemptAbsorb = true;
        }
        m_startupUntil = now + kStartupSyncWindowMs;
    }
    if (usbAccess) {
        // Not a session marker: the start that follows records its own, and a request whose start never comes (denied,
        // abandoned) leaves nothing a later start could be mistaken for.
        submit(QStringLiteral("session"), QStringLiteral("info"),
               QStringLiteral("usb access requested: %1").arg(context));
    } else if (mark) {
        const QByteArray record =
            formatRecord(QStringLiteral("session"), QStringLiteral("info"),
                         context.isEmpty() ? QStringLiteral("--- session starting ---")
                                           : QStringLiteral("--- session starting: %1 ---").arg(context));
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_sessionLine = record;
        }
        enqueue(record, Pin::Session);
    }
    // Best effort: a slow or failing store must never hold up listening.
    (void)flush(kAttemptFlushMs);
}

void
DiagnosticsLog::armStartupWindow() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_startupUntil = nowMs() + kStartupSyncWindowMs;
    m_ready.notify_one();
}

QStringList
DiagnosticsLog::snapshot(quint64* generation) const {
    std::lock_guard<std::mutex> lock(m_ringMutex);
    if (generation) {
        *generation = m_generation;
    }
    return m_ring;
}

quint64
DiagnosticsLog::clear() {
    // Use submit's lock order. The clear precedes every subsequently queued
    // record, including when an older batch is already being written.
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    std::lock_guard<std::mutex> ringLock(m_ringMutex);
    m_queue.clear();
    m_clearPending = true;
    // The cleared session's marker must not come back as a later segment's header.
    m_sessionLine.clear();
    m_ring.clear();
    m_ringIds.clear();
    m_pinned = 0;
    m_ready.notify_one();
    return ++m_generation;
}

bool
DiagnosticsLog::flush(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    const quint64 ticket = ++m_flushRequested;
    if (m_options.manualDrain) {
        step(lock, true);
        return flushResult(ticket);
    }
    m_ready.notify_one();
    const auto done = [this, ticket] { return m_flushDone >= ticket; };
    if (timeoutMs < 0) {
        m_flushed.wait(lock, done);
    } else if (!m_flushed.wait_for(lock, std::chrono::milliseconds(timeoutMs), done)) {
        return false;
    }
    return flushResult(ticket);
}

bool
DiagnosticsLog::flushResult(quint64 ticket) const {
    const auto covering =
        std::find_if(m_flushResults.cbegin(), m_flushResults.cend(),
                     [ticket](const std::pair<quint64, bool>& result) { return result.first >= ticket; });
    return covering != m_flushResults.cend() && covering->second;
}

void
DiagnosticsLog::completeFlushes(quint64 through, bool ok) {
    m_flushDone = through;
    m_flushResults.emplace_back(through, ok);
    if (m_flushResults.size() > static_cast<size_t>(flushResultLimit)) {
        m_flushResults.erase(m_flushResults.begin());
    }
    m_flushed.notify_all();
}

DiagnosticsLog::IoStats
DiagnosticsLog::ioStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

void
DiagnosticsLog::setIoFaultForTest(IoFault fault) {
    std::lock_guard<std::mutex> lock(m_stepMutex);
    m_fault = fault;
}

bool
DiagnosticsLog::dueLocked(quint64 now) const {
    const bool drainDue = !m_queue.empty() && (inStartupWindow(now) || now - m_lastDrain >= kWriteIntervalMs);
    const bool syncDue = m_dirtyBytes > 0 && now - m_lastSync >= kSyncIntervalMs && now >= m_syncRetryAt;
    return m_clearPending || m_flushRequested > m_flushDone || drainDue || syncDue;
}

bool
DiagnosticsLog::pump() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (!m_options.manualDrain || !dueLocked(nowMs())) {
        return false;
    }
    step(lock, false);
    return true;
}

void
DiagnosticsLog::trimFragment(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() == 0) {
        return;
    }
    char last = '\n';
    if (!file.seek(file.size() - 1) || !file.getChar(&last) || last == '\n') {
        return;
    }
    // A crash part-way through an append: cut back to the last whole record so the next one is not
    // joined to the fragment.
    const QByteArray data = readTail(path, file.size());
    file.close();
    (void)QFile::resize(path, data.lastIndexOf('\n') + 1);
}

void
DiagnosticsLog::closeCurrent() {
    // Bytes still unsynced in a file being given up on are past syncing: the failure, not a retry, is what the next
    // flush reports. Without this, a dirty count with no file kept the writer due forever.
    if (m_dirtyBytes > 0) {
        m_ioFailed = true;
        m_dirtyBytes = 0;
    }
    m_file.reset();
}

void
DiagnosticsLog::openCurrent() {
    if (m_file) {
        return;
    }
    const QString path = filePath(kTailName);
    const bool existed = QFile::exists(path);
    if (existed) {
        trimFragment(path);
    }
    auto file = std::make_unique<QFile>(path);
    // Unbuffered: a sync must cover every byte written, with nothing left in a userspace buffer.
    if (m_fault == IoFault::SyncAndOpen
        || !file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered)) {
        m_ioFailed = true;
        return;
    }
    m_fileSize = file->size();
    m_file = std::move(file);
    if (!existed) {
        dsd_fsync_parent_dir(QFile::encodeName(path).constData());
    }
}

bool
DiagnosticsLog::appendBytes(const QByteArray& bytes) {
    if (!m_file) {
        return false;
    }
    if (m_fault == IoFault::Write || m_file->write(bytes) != bytes.size()) {
        // Reopened (and any partial record trimmed) on the next append.
        m_ioFailed = true;
        closeCurrent();
        return false;
    }
    m_fileSize += bytes.size();
    m_dirtyBytes += static_cast<quint64>(bytes.size());
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.appends;
    m_stats.bytes += static_cast<quint64>(bytes.size());
    return true;
}

bool
DiagnosticsLog::syncCurrent() {
    if (!m_file) {
        return false;
    }
    // On Windows QFile's handle() is a C runtime descriptor over the file, so this is _commit(), a real flush to disk.
    if (m_fault == IoFault::Sync || m_fault == IoFault::SyncAndOpen || dsd_fsync(m_file->handle()) != 0) {
        m_ioFailed = true;
        // Retried after an interval, never in a loop: a store that keeps failing must not spin the writer.
        m_syncRetryAt = nowMs() + kSyncIntervalMs;
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++m_stats.syncFailures;
        return false;
    }
    m_dirtyBytes = 0;
    m_lastSync = nowMs();
    // Storage recovered: the startup window's per-append syncs must not wait out an earlier failure's backoff.
    m_syncRetryAt = 0;
    std::lock_guard<std::mutex> lock(m_statsMutex);
    ++m_stats.syncs;
    return true;
}

void
DiagnosticsLog::rotate(const QByteArray& header) {
    // The current segment's data must be on disk before it becomes the previous segment.
    if (m_dirtyBytes > 0) {
        (void)syncCurrent();
    }
    closeCurrent();
    const QString tailPath = filePath(kTailName);
    const QString prevPath = filePath(kPrevName);
    QFile::remove(prevPath);
    if (!QFile::rename(tailPath, prevPath)) {
        // Keep the bound: start the current segment over rather than let it grow.
        QFile file(tailPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            (void)dsd_fsync(file.handle());
        }
        if (!m_rotationFailureNoted) {
            m_rotationFailureNoted = true;
            ringNote(QStringLiteral("[host] [warn] diagnostics: the saved log could not be rotated; its older lines "
                                    "were dropped"));
        }
    }
    dsd_fsync_parent_dir(QFile::encodeName(tailPath).constData());
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++m_stats.rotations;
    }
    openCurrent();
    // Each segment starts with the process and session it belongs to.
    if (m_file && !header.isEmpty()) {
        (void)appendBytes(header);
    }
}

bool
DiagnosticsLog::clearFiles() {
    // What the cleared file held unsynced no longer matters: Clear removes it.
    m_dirtyBytes = 0;
    m_file.reset();
    const QString tailPath = filePath(kTailName);
    const QString prevPath = filePath(kPrevName);
    bool ok = !QFile::exists(prevPath) || QFile::remove(prevPath);
    QFile file(tailPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || m_fault == IoFault::Sync
        || dsd_fsync(file.handle()) != 0) {
        ok = false;
    }
    dsd_fsync_parent_dir(QFile::encodeName(tailPath).constData());
    m_fileSize = 0;
    m_dirtyBytes = 0;
    if (!ok) {
        // A clear that did not reach the disk is not durable, and the next flush says so.
        m_ioFailed = true;
    }
    return ok;
}

QByteArray
DiagnosticsLog::headerExcluding(const std::deque<QByteArray>::const_iterator& from,
                                const std::deque<QByteArray>::const_iterator& end) const {
    // A context line still waiting in the batch (the process or session line itself) is written once, as the
    // record it is, not again in the header ahead of it.
    QByteArray header;
    for (const auto& line : m_header.split('\n')) {
        const QByteArray entry = line + '\n';
        if (!line.isEmpty() && std::find(from, end, entry) == end) {
            header += entry;
        }
    }
    return header;
}

bool
DiagnosticsLog::writeBatch(const std::deque<QByteArray>& batch) {
    openCurrent();
    if (!m_file) {
        return false;
    }
    QByteArray pending;
    // An empty segment (a new tail, or the first one after Clear) starts with its context like a rotated one.
    if (m_fileSize == 0) {
        pending = headerExcluding(batch.cbegin(), batch.cend());
    }
    for (auto it = batch.cbegin(); it != batch.cend(); ++it) {
        const QByteArray& record = *it;
        if (m_fileSize + pending.size() + record.size() > kSegmentLimit && m_fileSize + pending.size() > 0) {
            if (!pending.isEmpty() && !appendBytes(pending)) {
                return false;
            }
            pending.clear();
            rotate(headerExcluding(it, batch.cend()));
            if (!m_file) {
                return false;
            }
        }
        pending += record;
    }
    return pending.isEmpty() || appendBytes(pending);
}

void
DiagnosticsLog::step(std::unique_lock<std::mutex>& lock, bool forceSync) {
    StepWork work;
    work.batch.swap(m_queue);
    work.clear = m_clearPending;
    m_clearPending = false;
    const quint64 through = m_flushRequested;
    work.flushing = through > m_flushDone;
    work.now = nowMs();
    work.window = inStartupWindow(work.now);
    m_lastDrain = work.now;
    for (const QByteArray* line : {&m_processLine, &m_sessionLine}) {
        if (!line->isEmpty()) {
            work.header += *line + '\n';
        }
    }
    lock.unlock();
    const bool ok = writeStep(work, forceSync);
    lock.lock();
    if (work.flushing && through > m_flushDone) {
        completeFlushes(through, ok);
    }
}

bool
DiagnosticsLog::writeStep(const StepWork& work, bool forceSync) {
    std::lock_guard<std::mutex> stepLock(m_stepMutex);
    m_header = work.header;
    bool ok = !work.clear || clearFiles();
    if (!work.batch.empty()) {
        ok = writeBatch(work.batch) && ok;
    }
    const bool syncDue = forceSync || work.window || work.clear || work.now - m_lastSync >= kSyncIntervalMs;
    if (m_dirtyBytes > 0 && syncDue && (forceSync || work.now >= m_syncRetryAt)) {
        ok = syncCurrent() && ok;
    }
    if (!work.flushing) {
        return ok;
    }
    // A flush is durable only if nothing failed since the last one completed.
    ok = ok && !m_ioFailed && m_dirtyBytes == 0;
    m_ioFailed = false;
    return ok;
}

void
DiagnosticsLog::writeLoop() {
    // Only this thread writes the persistent tail (outside manual-drain tests).
    std::unique_lock<std::mutex> lock(m_queueMutex);
    for (;;) {
        const quint64 now = nowMs();
        if (m_stop || dueLocked(now)) {
            const bool stopping = m_stop;
            step(lock, stopping || m_flushRequested > m_flushDone);
            if (stopping && m_queue.empty() && !m_clearPending && m_flushRequested == m_flushDone) {
                break;
            }
            continue;
        }
        quint64 waitMs = std::numeric_limits<quint64>::max();
        if (!m_queue.empty()) {
            waitMs = kWriteIntervalMs - (now - m_lastDrain);
        }
        if (m_dirtyBytes > 0) {
            const quint64 due = std::max<quint64>(m_lastSync + kSyncIntervalMs, m_syncRetryAt);
            waitMs = std::min<quint64>(waitMs, due > now ? due - now : 0U);
        }
        if (waitMs == std::numeric_limits<quint64>::max()) {
            m_ready.wait(lock);
        } else {
            m_ready.wait_for(lock, std::chrono::milliseconds(waitMs));
        }
    }
}

DiagnosticsLogModel::DiagnosticsLogModel(DiagnosticsLog* log, QObject* parent)
    : QAbstractListModel(parent), m_log(log ? log : &DiagnosticsLog::instance()) {}

int
DiagnosticsLogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
DiagnosticsLogModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    return role == Qt::UserRole + 1 || role == Qt::DisplayRole ? m_rows.at(index.row()) : QVariant();
}

QHash<int, QByteArray>
DiagnosticsLogModel::roleNames() const {
    return {{Qt::UserRole + 1, "line"}};
}

void
DiagnosticsLogModel::setPaused(bool value) {
    if (value == m_paused) {
        return;
    }
    m_paused = value;
    Q_EMIT pausedChanged();
    refresh();
}

void
DiagnosticsLogModel::markSessionStarting() {
    // Arms the startup window; the plain marker is skipped when the start path already noted this attempt.
    m_log->noteSessionAttempt(QString());
}

void
DiagnosticsLogModel::setActive(bool value) {
    if (value == m_active) {
        return;
    }
    m_active = value;
    Q_EMIT activeChanged();
    if (!m_active) {
        return;
    }
    if (m_stale) {
        // One reset for everything consumed while hidden. Paused, that is the lines as they were at the
        // pause (the pending count covers the rest); otherwise the newest lines.
        QStringList rows = m_held;
        if (!m_paused) {
            quint64 generation = 0;
            rows = m_log->snapshot(&generation);
            m_seen = generation;
        }
        showRows(rows);
    }
    refresh();
}

void
DiagnosticsLogModel::showRows(const QStringList& rows) {
    m_held.clear();
    m_stale = false;
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

void
DiagnosticsLogModel::refresh() {
    quint64 generation = 0;
    const auto rows = m_log->snapshot(&generation);
    const int pending = m_paused ? static_cast<int>(std::min<quint64>(generation - m_seen, 2000)) : 0;
    if (pending != m_pending) {
        m_pending = pending;
        Q_EMIT pendingCountChanged();
    }
    if (m_paused || generation == m_seen) {
        return;
    }
    m_seen = generation;
    if (!m_active) {
        // Consumed as an active model would (a pause counts from here), shown when a view opens.
        m_held = rows;
        m_stale = true;
        return;
    }
    showRows(rows);
}

QString
DiagnosticsLogModel::allText() const {
    return m_log->snapshot().join(QLatin1Char('\n'));
}

void
DiagnosticsLogModel::copyAll() const {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(allText());
    }
}

void
DiagnosticsLogModel::clear() {
    const quint64 clearedGeneration = m_log->clear();
    m_held.clear();
    m_stale = false;
    beginResetModel();
    m_rows.clear();
    m_seen = clearedGeneration;
    endResetModel();
    m_pending = 0;
    Q_EMIT pendingCountChanged();
}
} // namespace dsd_qt
