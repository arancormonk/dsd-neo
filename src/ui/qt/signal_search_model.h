// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_QT_SIGNAL_SEARCH_MODEL_H
#define DSD_NEO_QT_SIGNAL_SEARCH_MODEL_H
// QVariantList's template instantiations need QList in some consumers.
#include <QList> // IWYU pragma: keep
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <dsd-neo/runtime/signal_search.h>
#include <stdint.h>

namespace dsd_qt {
class DecoderHost;
class KnownFrequencyIndex;

class SignalSearchModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList ranges READ ranges NOTIFY rangesChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantList avoids READ avoids NOTIFY avoidsChanged)
    Q_PROPERTY(QVariantList sessionAvoids READ sessionAvoids NOTIFY avoidsChanged)
    Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
  public:
    explicit SignalSearchModel(DecoderHost* host, KnownFrequencyIndex* index, QObject* parent = nullptr);
    ~SignalSearchModel() override;

    QVariantList
    ranges() const {
        return m_ranges;
    }

    QVariantList
    history() const {
        return m_history;
    }

    QVariantList
    avoids() const {
        return m_avoids;
    }

    QVariantList
    sessionAvoids() const {
        return m_sessionAvoids;
    }

    QVariantMap
    status() const {
        return m_status;
    }

    QString
    error() const {
        return m_error.isEmpty() ? m_persistenceError : m_error;
    }

    Q_INVOKABLE QVariantMap defaultRange() const;
    Q_INVOKABLE QString saveRange(int row, const QVariantMap& value);
    Q_INVOKABLE void removeRange(int row);
    Q_INVOKABLE void moveRange(int row, int direction);
    Q_INVOKABLE QString arm();
    Q_INVOKABLE void cancelArm();
    Q_INVOKABLE bool control(int action, const QString& session, const QString& hit);
    Q_INVOKABLE void removeAvoid(double frequency);
    Q_INVOKABLE bool removeSessionAvoid(double frequency);
    Q_INVOKABLE QVariantMap systemForHit(const QVariantMap& hit, const QVariantMap& source) const;
    void refresh();
  Q_SIGNALS:
    void rangesChanged();
    void changed();
    void historyChanged();
    void avoidsChanged();

  private:
    DecoderHost* m_host;
    KnownFrequencyIndex* m_index;
    QVariantList m_ranges, m_history, m_avoids, m_sessionAvoids;
    QVariantMap m_status;
    QString m_error, m_persistenceError;
    QTimer m_timer;
    dsd_signal_search_config m_pending{};
    /* A start waiting for the decoder to take it, and whether the session it waits for has been seen up. */
    bool m_armed = false, m_armSessionSeen = false;
    uint64_t m_revision = UINT64_MAX, m_session = 0;
    uint64_t m_historyRevision = UINT64_MAX, m_avoidsRevision = UINT64_MAX;
    bool m_owned = false;
    void followSession();
    bool armSessionUp() const;
    void submitArm();
    void disarm();
    bool persistRanges(const QVariantList& list);
    bool refreshAvoids();
    bool refreshHistory();
    bool unavoid(uint32_t frequency);
    QVariantMap hitMap(const dsd_signal_search_hit& h) const;
};
} // namespace dsd_qt
#endif
