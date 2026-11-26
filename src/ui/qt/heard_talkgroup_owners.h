// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief How the remembered talkgroups follow the saved systems (issue #903).
 *
 * The store learns only for a saved system that exists with rememberTalkgroups on, loses a system's list when that
 * system is deleted, and drops the lists of systems that are gone, but only when the saved systems loaded and no
 * restore may still replace them.
 */

#ifndef DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_OWNERS_H_
#define DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_OWNERS_H_

namespace dsd_qt {

namespace backup {
enum class ApplyOutcome;
} // namespace backup

class HeardTalkgroupStore;
class SavedSystemsModel;

/**
 * @brief Whether owners missing from @p systems may be purged: saved_systems.json read cleanly or was confirmed absent
 * (SavedSystemsModel::storeLoaded()), and this start's restore has settled (backup::restore_settled()).
 */
bool heard_talkgroups_may_purge(const SavedSystemsModel& systems, backup::ApplyOutcome restore);

/**
 * @brief Make @p store follow @p systems.
 *
 * The owner policy reads a copy of systems->rememberingUids(), refreshed whenever the systems' rows change, so it still
 * answers after @p systems is destroyed (History's teardown ingest comes later). A saved removal clears that system's
 * list. The owners @p systems does not hold are purged once, now, when heard_talkgroups_may_purge() allows it.
 */
void heard_talkgroups_follow_systems(HeardTalkgroupStore* store, SavedSystemsModel* systems,
                                     backup::ApplyOutcome restore);

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_HEARD_TALKGROUP_OWNERS_H_ */
