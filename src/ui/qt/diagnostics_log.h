// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_DIAGNOSTICS_LOG_H
#define DSD_NEO_DIAGNOSTICS_LOG_H
#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class QFile;

namespace dsd_qt {
// Process diagnostics survive decoder session transitions. QString copies cannot
// promise erasure: only redacted text may enter the ring or the writer queue.
//
// The persistent tail is two append-only segments (tail.prev.log, then tail.log), each at
// most kSegmentLimit bytes. Records are appended, never rewritten, and synced at most once a
// second, except during the startup window after a session attempt, when every append is
// synced so a phone that falls over seconds after Play keeps its start sequence (issue #745).
class DiagnosticsLog {
  public:
    static constexpr qint64 kSegmentLimit = 128 * 1024;
    static constexpr int kWriteIntervalMs = 250;
    static constexpr int kSyncIntervalMs = 1000;
    static constexpr int kStartupSyncWindowMs = 10000;
    static constexpr int kAttemptFlushMs = 200;
    static constexpr int kShutdownFlushMs = 500;

    struct IoStats {
        quint64 appends = 0;
        quint64 syncs = 0;
        quint64 bytes = 0;
        quint64 rotations = 0;
    };

    // Test seam: a monotonic millisecond clock, and a manual mode in which nothing is written
    // until pump() or flush() runs the writer step on the calling thread.
    struct TestOptions {
        std::function<quint64()> clockMs;
        bool manualDrain = false;
    };

    enum class IoFault { None, Write, Sync };

    // Extra platform facts for the process line (Android: boot count, device) and any lines that
    // follow it (Android: recent app exits). Registered before installTap().
    using PlatformFactsProvider = std::pair<QString, QStringList> (*)();

    static DiagnosticsLog& instance();
    static void installTap();
    static void setPlatformFactsProvider(PlatformFactsProvider provider);
    // Safe before Qt/nativeInit: bounded redacted records wait for installTap().
    static void submitHostDiagnostic(const QString& text);
    // Explicit directory supports isolated stores in restart/retention tests.
    explicit DiagnosticsLog(const QString& directory);
    DiagnosticsLog(const QString& directory, TestOptions options);
    ~DiagnosticsLog();
    void submit(const QString& source, const QString& level, const QString& text);
    QStringList snapshot(quint64* generation = nullptr) const;
    // Return the generation of the cleared ring while still holding its lock.
    quint64 clear();
    // True once everything submitted before the call is appended and synced. A negative timeout
    // waits for the writer; on timeout or a write/sync failure it returns false.
    bool flush(int timeoutMs = -1);
    // Records the process line (also the head of every later segment). Once per instance.
    void noteProcess(const QString& facts, const QStringList& followingLines = {});
    // A session attempt is about to touch the device: records the marker (once per attempt context
    // within kAttemptDedupeMs), arms the startup window and waits up to kAttemptFlushMs for it to be durable.
    void noteSessionAttempt(const QString& context);
    // Re-arms the startup window at a device boundary without a new marker.
    void armStartupWindow();
    IoStats ioStats() const;
    // Manual-drain mode only: run one writer step now, as the writer thread would at this clock reading.
    void pump();
    void setIoFaultForTest(IoFault fault);
    static QString redact(QString text);

  private:
    static constexpr int kAttemptDedupeMs = 60000;

    void load();
    void openCurrent();
    bool appendBytes(const QByteArray& bytes);
    bool syncCurrent();
    void rotate(const QByteArray& header);
    void clearFiles();
    void trimFragment(const QString& path) const;
    bool writeBatch(const std::deque<QByteArray>& batch);
    void enqueue(const QByteArray& record);
    void ringNote(const QString& line);
    void step(std::unique_lock<std::mutex>& lock, bool forceSync);
    bool dueLocked(quint64 now) const;
    void completeFlushes(quint64 through, bool ok);
    bool flushResult(quint64 ticket) const;
    QString filePath(const char* name) const;
    void writeLoop();
    quint64 nowMs() const;
    bool inStartupWindow(quint64 now) const;

    mutable std::mutex m_ringMutex;
    QStringList m_ring;
    quint64 m_generation = 0;

    // Queue state, under m_queueMutex.
    std::mutex m_queueMutex;
    std::condition_variable m_ready, m_flushed;
    std::deque<QByteArray> m_queue;
    bool m_stop = false;
    bool m_clearPending = false;
    quint64 m_flushRequested = 0;
    quint64 m_flushDone = 0;
    std::vector<std::pair<quint64, bool>> m_flushResults;
    quint64 m_startupUntil = 0;
    quint64 m_lastDrain = 0;
    QByteArray m_processLine;
    QByteArray m_sessionLine;
    QString m_lastAttemptContext;
    quint64 m_lastAttemptAt = 0;
    // An explicit attempt note absorbs the plain marker of the Starting transition that follows it.
    bool m_attemptAbsorb = false;
    bool m_processNoted = false;
    QByteArray m_previousProcessLine;

    // Writer state: touched only by the writer step, which runs on one thread at a time.
    std::mutex m_stepMutex;
    std::unique_ptr<QFile> m_file;
    qint64 m_fileSize = 0;
    quint64 m_dirtyBytes = 0;
    quint64 m_lastSync = 0;
    bool m_ioFailed = false;
    bool m_rotationFailureNoted = false;
    QByteArray m_header;
    IoFault m_fault = IoFault::None;

    mutable std::mutex m_statsMutex;
    IoStats m_stats;

    QString m_directory;
    TestOptions m_options;
    std::thread m_writer;
};

class DiagnosticsLogModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingCountChanged)
    // Whether a view shows the rows (Main.qml binds the diagnostics screen's visibility). While
    // inactive, ticks reset nothing; becoming active resets once (issue #718).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
  public:
    explicit DiagnosticsLogModel(DiagnosticsLog* log = nullptr, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool
    paused() const {
        return m_paused;
    }

    void setPaused(bool value);

    int
    pendingCount() const {
        return m_pending;
    }

    bool
    active() const {
        return m_active;
    }

    void setActive(bool value);

    void markSessionStarting();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString allText() const;
    Q_INVOKABLE void copyAll() const;
    Q_INVOKABLE void clear();
  Q_SIGNALS:
    void pausedChanged();
    void pendingCountChanged();
    void activeChanged();

  private:
    void showRows(const QStringList& rows);

    DiagnosticsLog* m_log;
    QStringList m_rows;
    quint64 m_seen = 0;
    bool m_paused = false;
    int m_pending = 0;
    bool m_active = true;
    // While inactive: the rows consumed (m_seen) but not shown yet, and whether there are any.
    QStringList m_held;
    bool m_stale = false;
};
} // namespace dsd_qt
#endif
