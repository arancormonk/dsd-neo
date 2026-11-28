// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "heard_talkgroup_owners.h"

#include <QAbstractItemModel>
#include <QDebug>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <memory>

#include "backup_apply.h"
#include "heard_talkgroup_store.h"
#include "saved_systems_model.h"

namespace dsd_qt {

bool
heard_talkgroups_may_purge(const SavedSystemsModel& systems, backup::ApplyOutcome restore) {
    return systems.storeLoaded() && backup::restore_settled(restore);
}

void
heard_talkgroups_follow_systems(HeardTalkgroupStore* store, SavedSystemsModel* systems, backup::ApplyOutcome restore) {
    auto remembering = std::make_shared<QSet<QString>>(systems->rememberingUids());
    store->setOwnerPolicy([remembering](const QString& uid) { return remembering->contains(uid); });
    const auto refresh = [remembering, systems]() { *remembering = systems->rememberingUids(); };
    QObject::connect(systems, &QAbstractItemModel::modelReset, store, refresh);
    QObject::connect(systems, &QAbstractItemModel::rowsInserted, store, refresh);
    QObject::connect(systems, &QAbstractItemModel::rowsRemoved, store, refresh);
    QObject::connect(systems, &QAbstractItemModel::dataChanged, store, refresh);
    QObject::connect(systems, &SavedSystemsModel::systemRemoved, store, &HeardTalkgroupStore::dropOwner);
    if (heard_talkgroups_may_purge(*systems, restore)) {
        store->purgeMissingOwners(systems->uids());
    }
}

} // namespace dsd_qt
