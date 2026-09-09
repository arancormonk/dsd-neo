// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_DIAGNOSTICS_LOG_H
#define DSD_NEO_DIAGNOSTICS_LOG_H
#include <QAbstractListModel>
#include <QStringList>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace dsd_qt {
// Process diagnostics survive decoder session transitions. QString copies cannot
// promise erasure: only redacted text may enter the ring or the writer queue.
class DiagnosticsLog {
  public:
    static DiagnosticsLog& instance();
    static void installTap();
    // Explicit directory supports isolated stores in restart/retention tests.
    explicit DiagnosticsLog(const QString& directory);
    ~DiagnosticsLog();
    void submit(const QString& source, const QString& level, const QString& text);
    QStringList snapshot(quint64* generation = nullptr) const;
    void clear();
    void flush();
    static QString redact(QString text);

  private:
    void writeLoop();
    mutable std::mutex m_ringMutex;
    QStringList m_ring;
    quint64 m_generation = 0;
    std::mutex m_queueMutex;
    std::condition_variable m_ready, m_flushed;
    std::deque<QByteArray> m_queue;
    bool m_stop = false, m_writing = false;
    QByteArray m_initialTail;
    QString m_directory;
    std::thread m_writer;
};

class DiagnosticsLogModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingCountChanged)
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

    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString allText() const;
    Q_INVOKABLE void copyAll() const;
    Q_INVOKABLE void clear();
  Q_SIGNALS:
    void pausedChanged();
    void pendingCountChanged();

  private:
    DiagnosticsLog* m_log;
    QStringList m_rows;
    quint64 m_seen = 0;
    bool m_paused = false;
    int m_pending = 0;
};
} // namespace dsd_qt
#endif
