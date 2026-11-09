// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The files the app could not read this run, for Home's notice card (issue #828).
 *
 * One list for the whole process: the Qt JSON stores' and settings files' notices (json_store_notices()) and the
 * Signal search store's files, which SignalSearchModel reports. It is made once, on the GUI thread, and never
 * destroyed, so neither it nor what was dismissed goes away with a QML engine. json_store calls its sink on whichever
 * thread loaded or saved (a call history worker among them), so the sink only queues a re-read onto this object's
 * thread, and every change to the list happens there.
 *
 * Each notice says what the card words: the store it belongs to and what became of the file. Call history's seen store
 * (call_history_seen.json) is the app's own bookkeeping: it is logged, and never listed here.
 */

#ifndef DSD_NEO_SRC_UI_QT_STORE_NOTICES_H_
#define DSD_NEO_SRC_UI_QT_STORE_NOTICES_H_

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariant> // IWYU pragma: keep -- QVariantList's element; IWYU's answer differs by TU.
#include <QVariantList>
#include <QVariantMap>

namespace dsd_qt {

class StoreNotices : public QObject {
    Q_OBJECT
    /**
     * Not dismissed, one per file, json_store's in the order it posted them, then the Signal search store's: maps of
     *  - file: the file's own name ("saved_systems.json");
     *  - store: what Util.storeLabel() names it by: the file name, or "call-history-settings" for call history's
     *    settings file, whose name follows the application's;
     *  - state: "unreadable" (not moved yet: it is not written over), "kept" (moved aside as `kept`), "unsaved" (a
     *    settings file that could not be kept: this run's changes are not saved; `kept` is set when it was renamed
     *    aside before that failed), "defaults" (as "unsaved", with no folder for a scratch copy: this run uses
     *    default settings) or "leftInPlace" (the Signal search history of version 1, which is never moved);
     *  - kept: the name it was kept as, else "";
     *  - earlierKept: for "unreadable", the name an earlier copy of the same file was kept as this run, else "".
     */
    Q_PROPERTY(QVariantList notices READ notices NOTIFY noticesChanged)

  public:
    /** @brief The one instance, made by the first call, which must be on the GUI thread. Never destroyed. */
    static StoreNotices* instance();

    QVariantList notices() const;

    /**
     * @brief Hide the notice for @p fileName as it stands now. A later change to it (a file kept aside after all, or
     *        unreadable again after that) shows again.
     */
    Q_INVOKABLE void dismiss(const QString& fileName);

    /**
     * @brief The shown notices not yet read out, now marked as read out: each is announced once for the process, so a
     *        rebuilt Home (a new QML engine) does not announce again what an earlier one did. Same key as dismiss().
     */
    Q_INVOKABLE QVariantList takeUnheard();

    /**
     * @brief Take the Signal search store's files as SignalSearchModel::storeFiles() lists them ({file, state, kept}),
     *        replacing what it reported before. GUI thread.
     */
    void setSignalSearchFiles(const QVariantList& files);

  Q_SIGNALS:
    void noticesChanged();

  private:
    explicit StoreNotices(QObject* parent = nullptr);

    struct Notice {
        QString file;
        QString store;
        QString state;
        QString kept;
        QString earlierKept;
    };

    /** Re-read json_store_notices(). GUI thread. */
    void sync();
    /** Rebuild the shown list; emits noticesChanged() when it differs. */
    void publish();
    static QString key(const Notice& notice);
    static QVariantMap toMap(const Notice& notice);

    QList<Notice> m_stores;       // json_store's, in its order
    QList<Notice> m_signalSearch; // the Signal search store's
    QHash<QString, QString> m_lastKept;
    QSet<QString> m_dismissed; // key() of each dismissed notice
    QSet<QString> m_heard;     // key() of each notice read out
    QVariantList m_shown;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_STORE_NOTICES_H_ */
