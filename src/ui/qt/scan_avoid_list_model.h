// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The avoided -Y rows or --trunk-scan targets of the session, as the Monitor's avoid sheet lists them (#672).
 *
 * MetricsModel owns one and hands it each list the app-control scan avoid view builds from the held snapshot, only when
 * the view's key moved. apply() turns the old rows into the new ones by key, in runs of removes, inserts and moves, so
 * a put back removes one row and every other delegate keeps its item: the accessibility tree hears one row leave, not
 * the list rebuilt (issue #782), and a long list costs no more than the rows that changed.
 */

#ifndef DSD_NEO_SRC_UI_QT_SCAN_AVOID_LIST_MODEL_H_
#define DSD_NEO_SRC_UI_QT_SCAN_AVOID_LIST_MODEL_H_

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/app_control/commands.h>
#include <stddef.h>

namespace dsd_qt {

class ScanAvoidListModel : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        LabelRole,
    };

    /** One avoided entry: what tells it apart (keyOf() of its identity), how the view labels it, and its identity. */
    struct Entry {
        QString key;
        QString label;
        dsd_app_scan_avoid_remove_payload identity{};
    };

    explicit ScanAvoidListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /**
     * @brief Make the rows @p next, by key: runs of rows whose key left are removed, runs of new keys inserted, and a
     * row whose key moved is moved, never removed and inserted again. A row that keeps its key keeps its item and
     * reports a new label as a data change. Keys in @p next must be unique (identities are).
     *
     * Linear in the rows (one hash of the keys) apart from the array shifts each run costs; a reorder of the trunk
     * scan's target list searches forward for each moved run.
     */
    void apply(QList<Entry> next);
    /** @brief Remove every row. */
    void clear();

    /**
     * @brief The entry @p key names in the current rows, as CommandBridge::restoreScanAvoid() reads it: @c key,
     * @c label, @c kind (dsd_app_scan_avoid_kind), and the identity: @c instance, @c listRev and @c frequencyHz as
     * decimal strings (64-bit, which QML cannot hold as a number), @c row, @c session and @c targetId. Empty when no
     * row has that key.
     */
    QVariantMap entry(const QString& key) const;

    /** @brief A trunk-scan target with id @p targetId is avoided (issue #644: a paused session's rows are reached only
     *  when they are not). */
    Q_INVOKABLE bool avoidsTarget(const QString& targetId) const;

    /** @brief The key of an identity: "lcn:<instance>:<list rev>:<row>:<Hz>" or "trunk:<session>:<target id>". */
    static QString keyOf(const dsd_app_scan_avoid_remove_payload& identity);
    /** @brief The UTF-8 text of a fixed-size name field, up to its NUL or its end when it has none. */
    static QString fieldText(const char* field, size_t size);

  private:
    void removeLeftRows(const QHash<QString, qsizetype>& nextIndex);
    void takeEntry(qsizetype pos, const Entry& next);
    qsizetype insertNewRun(const QList<Entry>& next, qsizetype pos, const QSet<QString>& kept);
    void moveKeptRun(const QList<Entry>& next, qsizetype pos);
    void reindex();

    QList<Entry> m_rows;
    QHash<QString, qsizetype> m_index;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SCAN_AVOID_LIST_MODEL_H_ */
