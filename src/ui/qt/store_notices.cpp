// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "store_notices.h"

#include <QChar>
#include <QCoreApplication>
#include <QFileInfo>
#include <QLatin1String>
#include <QMap>
#include <QVariantMap>
#include <Qt>
#include <utility>

#include "json_store.h"
#include "settings_recovery.h"

namespace dsd_qt {

namespace {

/* Call history's seen store: the app's own bookkeeping, logged and kept aside like the others but never shown. */
constexpr const char kSeenStoreFileName[] = "call_history_seen.json";

/* What the card names a settings or store file by: call history's settings file is named after the application. */
QString
store_key(const QString& fileName) {
    if (fileName != settings_recovery_prefs_file_name()
        && QFileInfo(fileName).completeBaseName() == QCoreApplication::applicationName()) {
        return QStringLiteral("call-history-settings");
    }
    return fileName;
}

} // namespace

StoreNotices*
StoreNotices::instance() {
    // Intentionally process-lifetime: json_store's sink can run on a worker after every QML engine is gone.
    static StoreNotices* notices = [] {
        auto* made = new StoreNotices();
        json_store_set_notice_sink(
            [made] { QMetaObject::invokeMethod(made, &StoreNotices::sync, Qt::QueuedConnection); });
        made->sync();
        return made;
    }();
    return notices;
}

StoreNotices::StoreNotices(QObject* parent) : QObject(parent) {}

QVariantList
StoreNotices::notices() const {
    return m_shown;
}

QString
StoreNotices::key(const Notice& notice) {
    return notice.file + QLatin1Char('\n') + notice.state + QLatin1Char('\n') + notice.kept + QLatin1Char('\n')
           + notice.earlierKept;
}

void
StoreNotices::dismiss(const QString& fileName) {
    for (const QList<Notice>* list : {&m_stores, &m_signalSearch}) {
        for (const Notice& notice : *list) {
            if (notice.file == fileName) {
                m_dismissed.insert(key(notice));
            }
        }
    }
    publish();
}

void
StoreNotices::sync() {
    QList<Notice> stores;
    for (const JsonStoreNotice& posted : json_store_notices()) {
        if (posted.fileName == QLatin1String(kSeenStoreFileName)) {
            continue;
        }
        Notice notice{posted.fileName, store_key(posted.fileName), QString(), posted.keptName, QString()};
        if (settings_recovery_scratch(posted.fileName)) {
            notice.state = QStringLiteral("unsaved");
        } else if (!posted.keptName.isEmpty()) {
            notice.state = QStringLiteral("kept");
        } else {
            notice.state = QStringLiteral("unreadable");
        }
        /* A file read cleanly, saved, then found unreadable again names no kept file any more: the copy kept before is
           still there, so it is still named. */
        if (!posted.keptName.isEmpty()) {
            m_lastKept.insert(posted.fileName, posted.keptName);
        } else if (notice.state == QLatin1String("unreadable")) {
            notice.earlierKept = m_lastKept.value(posted.fileName);
        }
        stores.append(notice);
    }
    m_stores = std::move(stores);
    publish();
}

void
StoreNotices::setSignalSearchFiles(const QVariantList& files) {
    QList<Notice> reported;
    for (const QVariant& value : files) {
        const QVariantMap file = value.toMap();
        const QString name = file.value(QStringLiteral("file")).toString();
        const QString state = file.value(QStringLiteral("state")).toString();
        if (state == QLatin1String("unreadable") || state == QLatin1String("kept")
            || state == QLatin1String("leftInPlace")) {
            reported.append({name, name, state, file.value(QStringLiteral("kept")).toString(), QString()});
        }
    }
    m_signalSearch = std::move(reported);
    publish();
}

void
StoreNotices::publish() {
    QVariantList shown;
    for (const QList<Notice>* list : {&m_stores, &m_signalSearch}) {
        for (const Notice& notice : *list) {
            if (m_dismissed.contains(key(notice))) {
                continue;
            }
            shown.append(QVariantMap{
                {QStringLiteral("file"), notice.file},
                {QStringLiteral("store"), notice.store},
                {QStringLiteral("state"), notice.state},
                {QStringLiteral("kept"), notice.kept},
                {QStringLiteral("earlierKept"), notice.earlierKept},
            });
        }
    }
    if (shown != m_shown) {
        m_shown = std::move(shown);
        Q_EMIT noticesChanged();
    }
}

} // namespace dsd_qt
