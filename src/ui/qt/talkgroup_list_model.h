// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Effective talkgroup policy plus talkgroups heard during this session.
 *
 * Refreshed from the single UI poll tick (see ui_controller.h), on the Qt main
 * thread — never from another thread or a second snapshot reader.
 */

#ifndef DSD_NEO_SRC_UI_QT_TALKGROUP_LIST_MODEL_H_
#define DSD_NEO_SRC_UI_QT_TALKGROUP_LIST_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
// Qt template conversions require this in some consumers; IWYU's answer depends on instantiation.
#include <type_traits> // IWYU pragma: keep

template <class T>
class QSet;

namespace dsd_qt {

class TalkgroupListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int notTunedCount READ notTunedCount NOTIFY countChanged)
    Q_PROPERTY(QStringList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(bool allowListMode READ allowListMode NOTIFY policyChanged)
    Q_PROPERTY(bool persistent READ persistent NOTIFY policyChanged)
    Q_PROPERTY(qint64 sinceWhen READ sinceWhen WRITE setSinceWhen NOTIFY sinceWhenChanged)

  public:
    enum Role {
        IdStartRole = Qt::UserRole + 1,
        IdEndRole,
        IdTextRole,
        NameRole,
        TagsRole,
        ListeningRole,
        ListedRole,
    };

    explicit TalkgroupListModel(QAbstractItemModel* history, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return static_cast<int>(m_rows.size());
    }

    int
    notTunedCount() const {
        return m_notTunedCount;
    }

    QStringList
    categories() const {
        return m_categories;
    }

    bool
    allowListMode() const {
        return m_allowListMode;
    }

    bool
    persistent() const {
        return m_persistent;
    }

    qint64
    sinceWhen() const {
        return m_sinceWhen;
    }

    void setSinceWhen(qint64 when);
    void refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot);
    /** @brief Drop snapshot state, retaining the session's history cutoff. */
    void clear();

  Q_SIGNALS:
    void countChanged();
    void categoriesChanged();
    void policyChanged();
    void sinceWhenChanged();

  private:
    struct Row {
        uint32_t idStart = 0;
        uint32_t idEnd = 0;
        QString name;
        QString tags;
        bool listening = false;
        bool listed = false;
    };

    static QVector<Row> listedRows(const dsd_state* snapshot, QSet<QString>& categoryTags);
    void appendHeardRows(QVector<Row>& rows, bool allowListedOnly) const;
    void replaceRows(QVector<Row> rows);
    void updateCategories(const QSet<QString>& categoryTags);

    QPointer<QAbstractItemModel> m_history;
    int m_tgRole = -1;
    int m_whenRole = -1;
    int m_kindRole = -1;
    QVector<Row> m_rows;
    QStringList m_categories;
    int m_notTunedCount = 0;
    bool m_allowListMode = false;
    bool m_persistent = false;
    qint64 m_sinceWhen = 0;
    uint64_t m_contextId = 0;
    unsigned int m_generation = 0;
    bool m_heardDirty = true;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_TALKGROUP_LIST_MODEL_H_ */
