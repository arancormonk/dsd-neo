// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics_log.h"
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <dsd-neo/runtime/log.h>

namespace dsd_qt {
namespace {
constexpr int tailLimit = 256 * 1024;

QByteArray
boundedTail(QByteArray text) {
    if (text.size() > tailLimit) {
        text = text.right(tailLimit);
        text.remove(0, text.indexOf('\n') + 1);
    }
    return text;
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
DiagnosticsLog::installTap() {
    static const bool installed = [] {
        dsd_neo_log_set_tap(capture, &instance());
        return true;
    }();
    (void)installed;
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

DiagnosticsLog::DiagnosticsLog(const QString& directory) : m_directory(directory) {
    QDir().mkpath(directory);
    QFile old(directory + QStringLiteral("/tail.log"));
    if (QFileInfo(old).lastModified().secsTo(QDateTime::currentDateTime()) > 7 * 24 * 3600) {
        old.remove();
    }
    if (old.open(QIODevice::ReadOnly)) {
        const auto data = boundedTail(old.read(tailLimit + 1));
        m_ring.append(QStringLiteral("--- previous run ---"));
        for (const auto& line : data.split('\n')) {
            if (!line.isEmpty()) {
                m_ring.append(redact(QString::fromUtf8(line)));
            }
        }
        while (m_ring.size() > 2000) {
            m_ring.removeAt(1);
        }
        m_generation = static_cast<quint64>(m_ring.size());
    }
    for (const auto& line : m_ring) {
        m_initialTail += line.toUtf8() + '\n';
    }
    m_writer = std::thread(&DiagnosticsLog::writeLoop, this);
}

DiagnosticsLog::~DiagnosticsLog() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
    }
    m_ready.notify_one();
    m_writer.join();
}

void
DiagnosticsLog::submit(const QString& source, const QString& level, const QString& text) {
    auto line = redact(QStringLiteral("[%1] [%2] %3").arg(source, level, text)).toUtf8();
    if (line.size() > 512) {
        int end = 512;
        while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80) {
            --end;
        }
        line.truncate(end);
    }
    // Queue lock orders concurrent submit operations, but no I/O holds it.
    std::lock_guard<std::mutex> queueLock(m_queueMutex);
    {
        std::lock_guard<std::mutex> ringLock(m_ringMutex);
        m_ring.append(QString::fromUtf8(line));
        if (m_ring.size() > 2000) {
            m_ring.removeFirst();
        }
        ++m_generation;
    }
    // Bounded backpressure: retaining newest queued records matches ring semantics.
    if (m_queue.size() >= 2000) {
        m_queue.pop_front();
    }
    m_queue.push_back(line + '\n');
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

void
DiagnosticsLog::clear() {
    std::lock_guard<std::mutex> lock(m_ringMutex);
    m_ring.clear();
    ++m_generation;
}

void
DiagnosticsLog::flush() {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_flushed.wait(lock, [this] { return m_queue.empty() && !m_writing; });
}

void
DiagnosticsLog::writeLoop() {
    // Only this worker writes the persistent tail. Previous runs are re-filtered
    // on load so older versions cannot introduce unredacted records to the UI.
    QByteArray tail = m_initialTail;
    for (;;) {
        std::deque<QByteArray> batch;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_ready.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop && m_queue.empty()) {
                break;
            }
            batch.swap(m_queue);
            m_writing = true;
        }
        for (const auto& line : batch) {
            tail += line;
        }
        tail = boundedTail(tail);
        QSaveFile file(m_directory + QStringLiteral("/tail.log"));
        if (file.open(QIODevice::WriteOnly)) {
            if (file.write(tail) == tail.size()) {
                file.commit();
            } else {
                file.cancelWriting();
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_writing = false;
        }
        m_flushed.notify_all();
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
    beginResetModel();
    m_rows = rows;
    m_seen = generation;
    endResetModel();
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
    m_log->clear();
    beginResetModel();
    m_rows.clear();
    m_log->snapshot(&m_seen);
    endResetModel();
    m_pending = 0;
    Q_EMIT pendingCountChanged();
}
} // namespace dsd_qt
