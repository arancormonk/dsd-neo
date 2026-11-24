// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QAbstractListModel>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/heard_talkgroup.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <tuple>
#include <utility>
#include "../test_support/qt_test_paths.h"

#include "call_history_model.h"
#include "heard_talkgroup_store.h"
#include "json_store.h"
#include "talkgroup_filter_model.h"
#include "talkgroup_list_model.h"

using dsd_qt::CallHistoryModel;
using dsd_qt::HeardTalkgroupStore;
using dsd_qt::TalkgroupFilterModel;
using dsd_qt::TalkgroupListModel;

namespace {

int failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Fixture {
    dsd_opts* opts = static_cast<dsd_opts*>(calloc(1, sizeof(dsd_opts)));
    dsd_state* state = static_cast<dsd_state*>(calloc(1, sizeof(dsd_state)));
    Event_History_I* rings = static_cast<Event_History_I*>(calloc(2, sizeof(Event_History_I)));

    /* Each fixture is a new ring, with an identity of its own as initState() draws one. */
    Fixture() {
        static uint64_t nextInstance = 0x7A160000ULL;
        if (!opts || !state || !rings) {
            abort();
        }
        nextInstance++;
        state->event_history_s = rings;
        for (int slot = 0; slot < 2; ++slot) {
            rings[slot].revision = 1;
            rings[slot].commit_rev = 1;
            rings[slot].instance = nextInstance;
        }
    }

    ~Fixture() {
        dsd_state_ext_free_all(state);
        free(rings);
        free(state);
        free(opts);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void
    append(uint32_t start, uint32_t end, const char* mode, const char* name, const char* tags,
           dsd_tg_policy_entry_source source = DSD_TG_POLICY_SOURCE_IMPORTED) {
        dsd_tg_policy_entry entry;
        expect("create policy row", dsd_tg_policy_make_exact_entry(start, mode, name, source, &entry) == 0);
        entry.id_end = end;
        entry.kind = start != end ? DSD_TG_POLICY_ROW_RANGE : DSD_TG_POLICY_ROW_EXACT;
        DSD_SNPRINTF(entry.tags, sizeof(entry.tags), "%s", tags);
        expect("append policy row",
               (entry.kind == DSD_TG_POLICY_ROW_RANGE ? dsd_tg_policy_add_range_entry(state, &entry)
                                                      : dsd_tg_policy_append_exact(state, &entry))
                   == 0);
    }

    void
    appendPattern(const char* pattern, const char* mode, const char* name, const char* tags) {
        dsd_tg_policy_entry entry;
        expect("create pattern row",
               dsd_tg_policy_make_exact_entry(0, mode, name, DSD_TG_POLICY_SOURCE_IMPORTED, &entry) == 0);
        entry.kind = DSD_TG_POLICY_ROW_PATTERN;
        DSD_SNPRINTF(entry.pattern, sizeof(entry.pattern), "%s", pattern);
        DSD_SNPRINTF(entry.tags, sizeof(entry.tags), "%s", tags);
        expect("append pattern row", dsd_tg_policy_add_pattern_entry(state, &entry) == 0);
    }

    void
    commit(uint32_t tg, time_t when, bool voice = true) {
        commitOn(tg, when, nullptr, false, false, voice);
    }

    /* A call heard on scan target @p channel (none: off any scan unless @p scanMember), as the producer stamps it. */
    void
    commitOn(uint32_t tg, time_t when, const char* channel, bool scanMember, bool operatorTuned, bool voice = true) {
        Event_History_I* ring = &rings[0];
        DSD_MEMMOVE(&ring->Event_History_Items[2], &ring->Event_History_Items[1],
                    sizeof(Event_History) * (DSD_EVENT_HISTORY_LEN - 2));
        Event_History* row = &ring->Event_History_Items[1];
        DSD_MEMSET(row, 0, sizeof(*row));
        row->category = voice ? DSD_EVENT_CATEGORY_VOICE : DSD_EVENT_CATEGORY_DATA;
        row->target_id = tg;
        row->source_id = 42;
        row->event_start_time = when;
        row->event_time = when + 1;
        DSD_SNPRINTF(row->event_string, sizeof(row->event_string), "test call");
        if (channel != nullptr) {
            DSD_SNPRINTF(row->channel_label, sizeof(row->channel_label), "%s", channel);
        }
        row->channel_label_resolved = 1U;
        row->scan_member = scanMember ? 1U : 0U;
        row->operator_tuned = operatorTuned ? 1U : 0U;
        ++ring->push_seq;
        ++ring->commit_rev;
        ++ring->revision;
    }
};

QModelIndex
find(const TalkgroupListModel& model, uint32_t id) {
    for (int i = 0; i < model.count(); ++i) {
        const QModelIndex idx = model.index(i, 0);
        if (model.data(idx, TalkgroupListModel::IdStartRole).toULongLong() == id) {
            return idx;
        }
    }
    return {};
}

void
test_policy_and_heard_rows() {
    Fixture fixture;
    fixture.append(2001, 2001, "DE", "EMS dispatch", "ems");
    fixture.append(1300, 1399, "B", "Fire range", "FIRE");
    fixture.append(1001, 1001, "A", "Fire dispatch", "FIRE");
    fixture.append(9001, 9001, "D", "Radio alias", "Ignored");
    fixture.append(9002, 9002, "A", "Learned alias", "Ignored", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    const time_t now = time(nullptr);
    // A call the previous session logged; the session's own calls follow its start.
    fixture.commit(6001, now - 60);
    history.refresh(fixture.state);
    history.beginSession();
    model.setHistorySession(history.session());
    fixture.commit(4001, now);
    fixture.commit(4001, now + 5);
    fixture.commit(1350, now + 10);
    fixture.commit(1001, now + 15);
    fixture.commit(7001, now + 20, false);
    history.refresh(fixture.state);
    model.refresh(fixture.opts, fixture.state);

    expect("listed rows plus unique uncovered session voice target", model.count() == 4);
    expect("aliases excluded", !find(model, 9001).isValid() && !find(model, 9002).isValid());
    expect("old calls and notices excluded", !find(model, 6001).isValid() && !find(model, 7001).isValid());
    expect("range covers heard target", !find(model, 1350).isValid());
    expect("numeric ordering", model.data(model.index(0, 0), TalkgroupListModel::IdStartRole).toULongLong() == 1001);
    expect("range label uses en dash",
           model.data(find(model, 1300), TalkgroupListModel::IdTextRole).toString() == QStringLiteral("1300–1399"));
    expect("B and DE are not tuned", !model.data(find(model, 1300), TalkgroupListModel::ListeningRole).toBool()
                                         && !model.data(find(model, 2001), TalkgroupListModel::ListeningRole).toBool()
                                         && model.notTunedCount() == 2);
    expect("categories unique and case insensitive sorted",
           model.categories() == QStringList({QStringLiteral("ems"), QStringLiteral("FIRE")}));
    expect("heard target unlisted and listening",
           !model.data(find(model, 4001), TalkgroupListModel::ListedRole).toBool()
               && model.data(find(model, 4001), TalkgroupListModel::ListeningRole).toBool());

    int resets = 0;
    int changes = 0;
    int changedId = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&]() { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&](const QModelIndex& first, const QModelIndex& last) {
                         ++changes;
                         changedId = model.data(first, TalkgroupListModel::IdStartRole).toInt();
                         expect("row-local update", first == last);
                     });
    model.refresh(fixture.opts, fixture.state);
    expect("unchanged snapshot emits no row signals", resets == 0 && changes == 0);
    expect("set row mode", dsd_tg_policy_set_mode(fixture.state, 1001, 1001, "B") == 0);
    model.refresh(fixture.opts, fixture.state);
    expect("one mode edit does not reset or update neighbours", resets == 0 && changes == 1 && changedId == 1001);
    expect("row edit updates count and retains label",
           model.notTunedCount() == 3
               && model.data(find(model, 1001), TalkgroupListModel::NameRole).toString()
                      == QStringLiteral("Fire dispatch"));

    changes = 0;
    fixture.opts->trunk_use_allow_list = 1;
    model.refresh(fixture.opts, fixture.state);
    expect("allowlist changes heard target without resetting",
           model.allowListMode() && resets == 0 && changes == 1 && changedId == 4001
               && !model.data(find(model, 4001), TalkgroupListModel::ListeningRole).toBool()
               && model.notTunedCount() == 4);

    TalkgroupFilterModel view;
    view.setSourceModel(&model);
    view.setFilterTag(QStringLiteral("FIRE"));
    expect("tag filters listed rows only", view.count() == 2);
    view.setFilterText(QStringLiteral("DISPATCH"));
    expect("search intersects selected category case insensitively",
           view.count() == 1 && view.data(view.index(0, 0), TalkgroupListModel::IdStartRole).toULongLong() == 1001);
    view.setFilterText(QStringLiteral("1399"));
    expect("search matches range text",
           view.count() == 1 && view.data(view.index(0, 0), TalkgroupListModel::IdStartRole).toULongLong() == 1300);
    view.setFilterTag(QStringLiteral("fire"));
    expect("category match remains exact", view.count() == 0);
    expect("a range filtered away has no row", view.rowForTalkgroup(1300, 1399) == -1);
    view.setFilterTag(QString());
    view.setFilterText(QString());
    expect("each listed range is found at its view row", [&view]() {
        for (int i = 0; i < view.count(); ++i) {
            const QModelIndex at = view.index(i, 0);
            const qulonglong first = view.data(at, TalkgroupListModel::IdStartRole).toULongLong();
            const qulonglong last = view.data(at, TalkgroupListModel::IdEndRole).toULongLong();
            if (view.rowForTalkgroup(first, last) != i) {
                return false;
            }
        }
        return view.count() > 0;
    }());
    expect("a range that is not listed has no row", view.rowForTalkgroup(999999, 999999) == -1);
    expect("a start with another end is another row", view.rowForTalkgroup(1300, 1300) == -1);

    changes = 0;
    DSD_SNPRINTF(fixture.opts->group_in_file, sizeof(fixture.opts->group_in_file), "groups.csv");
    model.refresh(fixture.opts, fixture.state);
    expect("configured file changes persistence without rebuilding rows",
           model.persistent() && resets == 0 && changes == 0);
    history.beginSession();
    model.setHistorySession(history.session());
    model.refresh(fixture.opts, fixture.state);
    expect("a new session removes heard rows even with unchanged policy",
           model.count() == 3 && !find(model, 4001).isValid());
    model.refresh(nullptr, fixture.state);
    expect("missing snapshot clears policy and rows", model.count() == 0 && model.notTunedCount() == 0
                                                          && model.categories().isEmpty() && !model.allowListMode()
                                                          && !model.persistent());
    model.refresh(fixture.opts, fixture.state);
    expect("clear retains the session selection", model.count() == 3 && model.historySession() == history.session());
}

/* The heard talkgroups follow the history's session. A capture replayed in a fresh state pushes the
 * same rows as the first replay, which the history takes in as calls heard again: their logged rows
 * join the new session, and the talkgroups are heard in it. */
void
test_replay_heard_again_lists_its_talkgroups() {
    const time_t capture = 1262304000; // 2010-01-01Z: a replay's calls carry the capture's time
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    auto replay = [](Fixture& fixture) {
        fixture.commit(9301, capture);
        fixture.commit(9302, capture + 30);
    };
    Fixture first;
    history.beginSession();
    model.setHistorySession(history.session());
    replay(first);
    history.refresh(first.state);
    model.refresh(first.opts, first.state);
    expect("the first replay's talkgroups are heard", model.count() == 2);

    Fixture second; // the next start's fresh state
    history.beginSession();
    model.setHistorySession(history.session());
    replay(second);
    history.refresh(second.state);
    model.refresh(second.opts, second.state);
    expect("a replayed capture's talkgroups are heard in the next session",
           model.count() == 2 && find(model, 9301).isValid() && find(model, 9302).isValid());
}

void
test_edit_fields_and_version() {
    Fixture fixture;
    fixture.append(42, 42, "A", "Dispatch", "Fire");
    TalkgroupListModel model(nullptr);
    model.refresh(fixture.opts, fixture.state);
    const QString context = model.property("policyContext").toString();
    const unsigned int generation = model.property("policyGeneration").toUInt();
    expect("context is a lossless decimal string", !context.isEmpty() && context != "0");
    expect("a listening row records", model.data(find(model, 42), model.roleNames().key("record", -1)).toBool());
    dsd_tg_policy_entry values = {};
    values.priority = 50;
    values.preempt = 1;
    values.record = 0;
    expect("edit priority, preempt and record",
           dsd_tg_policy_set_fields(fixture.state, 42, 42, &values,
                                    DSD_TG_POLICY_FIELD_PRIORITY | DSD_TG_POLICY_FIELD_PREEMPT
                                        | DSD_TG_POLICY_FIELD_RECORD)
               == 0);
    int changes = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model, [&]() { ++changes; });
    model.refresh(fixture.opts, fixture.state);
    const auto roles = model.roleNames();
    expect("priority copied", model.data(find(model, 42), roles.key("priority", -1)).toInt() == 50);
    expect("preempt copied", model.data(find(model, 42), roles.key("preempt", -1)).toBool());
    expect("record copied (issue #708)", !model.data(find(model, 42), roles.key("record", -1)).toBool());
    expect("field edit notifies", changes == 1);
    expect("version advances", model.property("policyContext").toString() == context
                                   && model.property("policyGeneration").toUInt() != generation);
    model.clear();
    expect("version cleared",
           model.property("policyContext").toString() == "0" && model.property("policyGeneration").toUInt() == 0);
}

/* Deliberately unrelated numeric roles: the QML fixture is not CallHistoryModel. */
class AlternateHistory : public QAbstractListModel {
  public:
    bool present = false;
    uint32_t tg = 8001;
    /* A "gi" role (who the call was to), only when set before the model reads the roles. */
    bool withGi = false;
    int gi = -1;

    int
    rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return !parent.isValid() && present ? 1 : 0;
    }

    QHash<int, QByteArray>
    roleNames() const override {
        QHash<int, QByteArray> roles = {
            {Qt::UserRole + 71, "tg"}, {Qt::UserRole + 72, "session"}, {Qt::UserRole + 73, "kind"}};
        if (withGi) {
            roles.insert(Qt::UserRole + 74, "gi");
        }
        return roles;
    }

    QVariant
    data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || !present) {
            return {};
        }
        switch (role) {
            case Qt::UserRole + 71: return tg;
            case Qt::UserRole + 72: return 100;
            case Qt::UserRole + 73: return 0;
            case Qt::UserRole + 74: return gi;
            default: return {};
        }
    }

    void
    insert() {
        beginInsertRows({}, 0, 0);
        present = true;
        endInsertRows();
    }

    void
    change() {
        tg = 8002;
        Q_EMIT dataChanged(index(0, 0), index(0, 0));
    }

    void
    remove() {
        beginRemoveRows({}, 0, 0);
        present = false;
        endRemoveRows();
    }

    void
    reset() {
        beginResetModel();
        present = true;
        endResetModel();
    }
};

void
test_history_role_names_and_mutations() {
    Fixture fixture;
    AlternateHistory history;
    TalkgroupListModel model(&history);
    model.refresh(fixture.opts, fixture.state);
    history.insert();
    model.refresh(fixture.opts, fixture.state);
    expect("inserted history resolves roles by name", model.count() == 1 && find(model, 8001).isValid());
    history.change();
    model.refresh(fixture.opts, fixture.state);
    expect("history edits invalidate heard set", model.count() == 1 && find(model, 8002).isValid());
    history.remove();
    model.refresh(fixture.opts, fixture.state);
    expect("history removal invalidates heard set", model.count() == 0);
    history.reset();
    model.refresh(fixture.opts, fixture.state);
    expect("history reset invalidates heard set", model.count() == 1 && find(model, 8002).isValid());
}

/* Re-review r1 (issue #800): a private call's target is a radio, never a talkgroup heard; a call whose kind is not
 * known still counts, as before. */
void
test_private_calls_are_not_heard_talkgroups() {
    Fixture fixture;
    AlternateHistory history;
    history.withGi = true;
    history.gi = 1;
    TalkgroupListModel model(&history);
    history.insert();
    model.refresh(fixture.opts, fixture.state);
    expect("r1: a private call's radio gets no talkgroup card", model.count() == 0);
    history.gi = -1;
    history.change();
    model.refresh(fixture.opts, fixture.state);
    expect("r1: not knowing the kind, it is heard as before", model.count() == 1 && find(model, 8002).isValid());
}

void
test_temporary_avoid_status() {
    Fixture fixture;
    fixture.append(100, 100, "A", "Exact", "Fire");
    fixture.append(200, 299, "A", "Range", "Fire");
    TalkgroupListModel model(nullptr);
    model.refresh(fixture.opts, fixture.state);
    const auto before = model.policyGeneration();
    expect("temporary exact added", dsd_tg_policy_session_avoid_add(fixture.state, 100) == 0);
    expect("temporary range member one", dsd_tg_policy_session_avoid_add(fixture.state, 220) == 0);
    expect("temporary range member two", dsd_tg_policy_session_avoid_add(fixture.state, 230) == 0);
    model.refresh(fixture.opts, fixture.state);
    const auto exact = find(model, 100);
    const auto range = find(model, 200);
    expect("temporary mutation publishes", model.policyGeneration() != before);
    expect("saved listening stays enabled", model.data(exact, TalkgroupListModel::ListeningRole).toBool());
    expect("exact avoid badge", model.data(exact, TalkgroupListModel::TemporaryAvoidCountRole).toULongLong() == 1);
    expect("range avoid count", model.data(range, TalkgroupListModel::TemporaryAvoidCountRole).toULongLong() == 2);
    dsd_tg_policy_session_avoid_clear(fixture.state);
    model.refresh(fixture.opts, fixture.state);
    expect("clear refreshes unchanged rows",
           model.data(exact, TalkgroupListModel::TemporaryAvoidCountRole).toULongLong() == 0);
}

/* Counts the history reads that go through the item-model interface. */
class CountingHistory : public CallHistoryModel {
  public:
    mutable int dataCalls = 0;

    QVariant
    data(const QModelIndex& index, int role) const override {
        ++dataCalls;
        return CallHistoryModel::data(index, role);
    }
};

/* Issue #718: the heard set is rebuilt on every history change. With a full 1000-row log, three QVariant
 * data() lookups per row cost the UI thread ~14 ms per call commit, so the call history's own rows are
 * read directly. */
void
test_heard_rows_read_history_storage_directly() {
    Fixture fixture;
    CountingHistory history;
    TalkgroupListModel model(&history);
    history.beginSession();
    model.setHistorySession(history.session());
    const time_t now = time(nullptr);
    for (int i = 0; i < 200; ++i) {
        fixture.commit(5000 + static_cast<uint32_t>(i % 50), now + static_cast<time_t>(120) * i);
    }
    history.refresh(fixture.state);
    expect("the history logged the calls", history.count() >= 50);
    history.dataCalls = 0;
    model.refresh(fixture.opts, fixture.state);
    expect("every distinct heard talkgroup is listed", model.count() == 50);
    expect("the heard walk reads the history without data() lookups", history.dataCalls == 0);
}

/* Coverage by overlapping listed ranges: a range that starts early and ends late covers a talkgroup past
 * the later, shorter rows that start before it. */
void
test_overlapping_ranges_cover_heard_talkgroups() {
    Fixture fixture;
    fixture.append(100, 500, "A", "Wide", "");
    fixture.append(200, 210, "A", "Narrow", "");
    fixture.append(300, 300, "A", "Exact", "");
    fixture.append(600, 700, "A", "Second wide", "");
    fixture.append(650, 650, "A", "Second exact", "");
    fixture.append(800, 900, "D", "Deleted range", "");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    history.beginSession();
    model.setHistorySession(history.session());
    const time_t now = time(nullptr);
    const uint32_t heard[] = {50, 150, 400, 505, 650, 699, 701, 300, 850, 100, 500, 600, 700};
    int offset = 0;
    for (uint32_t tg : heard) {
        fixture.commit(tg, now + static_cast<time_t>(120) * offset++);
    }
    history.refresh(fixture.state);
    model.refresh(fixture.opts, fixture.state);
    expect("five listed rows plus the uncovered heard talkgroups", model.count() == 9);
    for (uint32_t tg : {50U, 505U, 701U, 850U}) {
        expect("an uncovered talkgroup is listed as heard",
               find(model, tg).isValid() && !model.data(find(model, tg), TalkgroupListModel::ListedRole).toBool());
    }
    for (uint32_t tg : {150U, 400U, 699U, 500U, 700U}) {
        expect("a covered talkgroup adds no heard row", !find(model, tg).isValid());
    }
}

QModelIndex
findText(const TalkgroupListModel& model, const QString& text) {
    for (int i = 0; i < model.count(); ++i) {
        const QModelIndex idx = model.index(i, 0);
        if (model.data(idx, TalkgroupListModel::IdTextRole).toString() == text) {
            return idx;
        }
    }
    return {};
}

/* A pattern row is listed with its canonical text as its ID, covers the heard talkgroups it matches, is found by
 * any typed number it matches, counts the session avoids it matches, and names its row (kind, pattern, table index)
 * for the edits a card submits. */
void
test_pattern_rows() {
    Fixture fixture;
    fixture.append(1001, 1001, "A", "Dispatch", "Fire");
    fixture.appendPattern("12*", "B", "Twelves", "Fire");
    fixture.append(1300, 1399, "A", "Range", "");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    history.beginSession();
    model.setHistorySession(history.session());
    const time_t now = time(nullptr);
    fixture.commit(1234, now);
    fixture.commit(5000, now + 120);
    fixture.commit(12, now + 240);
    history.refresh(fixture.state);
    expect("avoid 1234", dsd_tg_policy_session_avoid_add(fixture.state, 1234) == 0);
    expect("avoid 1299", dsd_tg_policy_session_avoid_add(fixture.state, 1299) == 0);
    expect("avoid 5000", dsd_tg_policy_session_avoid_add(fixture.state, 5000) == 0);
    model.refresh(fixture.opts, fixture.state);

    const QModelIndex pattern = findText(model, QStringLiteral("12*"));
    const auto roles = model.roleNames();
    expect("pattern row listed with its text as the ID", pattern.isValid());
    expect("listed rows plus the uncovered heard talkgroup", model.count() == 4);
    expect("a heard talkgroup the pattern matches is not listed separately",
           !find(model, 1234).isValid() && !findText(model, QStringLiteral("12")).isValid());
    expect("an unmatched heard talkgroup is listed", find(model, 5000).isValid());
    expect("pattern row kind", model.data(pattern, roles.key("kind", -1)).toString() == QStringLiteral("pattern"));
    expect("pattern row text", model.data(pattern, roles.key("pattern", -1)).toString() == QStringLiteral("12*"));
    expect("pattern row table index", model.data(pattern, roles.key("tableIndex", -1)).toInt() == 1);
    expect("pattern row listed and not tuned", model.data(pattern, TalkgroupListModel::ListedRole).toBool()
                                                   && !model.data(pattern, TalkgroupListModel::ListeningRole).toBool());
    expect("exact and range kinds",
           model.data(find(model, 1001), roles.key("kind", -1)).toString() == QStringLiteral("exact")
               && model.data(find(model, 1300), roles.key("kind", -1)).toString() == QStringLiteral("range")
               && model.data(find(model, 1300), roles.key("pattern", -1)).toString().isEmpty()
               && model.data(find(model, 1300), roles.key("tableIndex", -1)).toInt() == 2);
    expect("heard row has no table index", model.data(find(model, 5000), roles.key("tableIndex", -1)).toInt() == -1);
    expect("avoids the pattern matches are counted",
           model.data(pattern, TalkgroupListModel::TemporaryAvoidCountRole).toULongLong() == 2);
    expect("the range counts its own avoids",
           model.data(find(model, 1300), TalkgroupListModel::TemporaryAvoidCountRole).toULongLong() == 0);

    TalkgroupFilterModel view;
    view.setSourceModel(&model);
    view.setFilterText(QStringLiteral("1234"));
    expect("a typed number finds the pattern that matches it",
           view.count() == 1
               && view.data(view.index(0, 0), TalkgroupListModel::IdTextRole).toString() == QStringLiteral("12*"));
    view.setFilterText(QStringLiteral("12*"));
    expect("the pattern text finds its row", view.count() == 1);
    view.setFilterText(QStringLiteral("77"));
    expect("a number the pattern does not match leaves it out", view.count() == 0);
    view.setFilterText(QString());
    const QVariantList selection = view.snapshotSelection();
    bool patternSelected = false;
    for (const QVariant& value : selection) {
        const QVariantMap row = value.toMap();
        patternSelected |= row.value("index").toInt() == 1 && row.value("pattern").toString() == QStringLiteral("12*")
                           && row.value("first").toULongLong() == 0 && row.value("last").toULongLong() == 0;
    }
    expect("the selection names the pattern row", patternSelected);
}

/* The `*` catch-all covers no heard talkgroup: each keeps its own card beside the catch-all row, as it does with no
 * pattern at all, while a narrower pattern still covers the talkgroups it matches. */
void
test_catch_all_keeps_heard_talkgroups() {
    Fixture fixture;
    fixture.appendPattern("12*", "A", "Twelves", "");
    fixture.appendPattern("*", "A", "Other", "");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    history.beginSession();
    model.setHistorySession(history.session());
    const time_t now = time(nullptr);
    fixture.commit(1234, now);
    fixture.commit(5000, now + 120);
    fixture.commit(7311, now + 240);
    history.refresh(fixture.state);
    model.refresh(fixture.opts, fixture.state);
    expect("the catch-all row is listed", findText(model, QStringLiteral("*")).isValid());
    expect("heard talkgroups only the catch-all matches keep their cards",
           find(model, 5000).isValid() && find(model, 7311).isValid());
    expect("a narrower pattern still covers its talkgroups", !find(model, 1234).isValid());
    expect("two pattern rows and two heard talkgroups", model.count() == 4);
    expect("a listening catch-all shows its heard talkgroups listening and recording",
           model.data(find(model, 5000), TalkgroupListModel::ListeningRole).toBool()
               && model.data(find(model, 5000), TalkgroupListModel::RecordRole).toBool()
               && !model.data(find(model, 5000), TalkgroupListModel::ListedRole).toBool());
}

/* A heard talkgroup's card shows what the deciding catch-all row does with it, so its first tap changes the decoder's
 * verdict: `*,B` blocks it (shown not tuned, recording nothing), and under the allow list `*,A` admits it. */
void
test_catch_all_decides_heard_card_state() {
    struct Case {
        const char* tag;
        const char* mode;
        int allowList;
        bool listening;
    };

    const Case cases[] = {{"blocking catch-all", "B", 0, false},
                          {"catch-all under the allow list", "A", 1, true},
                          {"blocking catch-all under the allow list", "B", 1, false}};
    for (const Case& c : cases) {
        Fixture fixture;
        fixture.opts->trunk_use_allow_list = c.allowList;
        fixture.appendPattern("*", c.mode, "Other", "");
        CallHistoryModel history;
        TalkgroupListModel model(&history);
        history.beginSession();
        model.setHistorySession(history.session());
        fixture.commit(5000, time(nullptr));
        history.refresh(fixture.state);
        model.refresh(fixture.opts, fixture.state);
        const QModelIndex heard = find(model, 5000);
        expect(c.tag, heard.isValid() && !model.data(heard, TalkgroupListModel::ListedRole).toBool());
        expect(c.tag, model.data(heard, TalkgroupListModel::ListeningRole).toBool() == c.listening);
        expect(c.tag, model.data(heard, TalkgroupListModel::RecordRole).toBool() == c.listening);
    }
}

/* Row equality includes kind and pattern: another pattern in the same place is a different row, never a field edit of
 * the old one (both have 0/0 bounds). */
void
test_pattern_row_equality() {
    Fixture fixture;
    fixture.appendPattern("12*", "A", "Rule", "");
    TalkgroupListModel model(nullptr);
    model.refresh(fixture.opts, fixture.state);
    expect("first pattern listed", findText(model, QStringLiteral("12*")).isValid());
    int resets = 0;
    int removes = 0;
    int inserts = 0;
    int changes = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&]() { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model, [&]() { ++removes; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model, [&]() { ++inserts; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model, [&]() { ++changes; });
    uint64_t context = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(fixture.state, &context, &generation);
    dsd_tg_policy_row_key key = {};
    key.table_index = 0;
    key.kind = DSD_TG_POLICY_ROW_PATTERN;
    DSD_SNPRINTF(key.pattern, sizeof(key.pattern), "%s", "12*");
    expect("remove the pattern row", dsd_tg_policy_remove_key(fixture.state, context, generation, &key) == 0);
    fixture.appendPattern("13*", "A", "Rule", "");
    model.refresh(fixture.opts, fixture.state);
    expect("the other pattern is listed", model.count() == 1 && findText(model, QStringLiteral("13*")).isValid());
    // Issue #903: a removal and an insertion, never a field edit of the old row, and never a reset.
    expect("another pattern replaces the row", resets == 0 && removes == 1 && inserts == 1 && changes == 0);
}

/* Issue #782: an exact row and a range can share a start ("Exact matches beat ranges"), so the Talkgroups screen
 * finds the card it keeps the reader on by both bounds. */
void
test_filter_finds_a_row_by_both_bounds() {
    Fixture fixture;
    fixture.append(1200, 1299, "A", "Range", "");
    fixture.append(1200, 1200, "A", "Exact", "");
    fixture.append(1300, 1300, "A", "After", "");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    history.beginSession();
    model.setHistorySession(history.session());
    model.refresh(fixture.opts, fixture.state);
    TalkgroupFilterModel view;
    view.setSourceModel(&model);
    expect("three listed rows", view.count() == 3);
    const int exact = view.rowForTalkgroup(1200, 1200);
    const int range = view.rowForTalkgroup(1200, 1299);
    expect("the exact row and the range are different rows", exact >= 0 && range >= 0 && exact != range);
    expect("the range is found at its own row",
           view.data(view.index(range, 0), TalkgroupListModel::IdEndRole).toULongLong() == 1299
               && view.data(view.index(range, 0), TalkgroupListModel::NameRole).toString() == QStringLiteral("Range"));
    expect("the exact row is found at its own row",
           view.data(view.index(exact, 0), TalkgroupListModel::IdEndRole).toULongLong() == 1200
               && view.data(view.index(exact, 0), TalkgroupListModel::NameRole).toString() == QStringLiteral("Exact"));
}

/* Issue #782: every pattern row has the bounds 0-0 and a table can list the same bounds twice, so the card the
 * Talkgroups screen keeps the reader on is found by its bounds, pattern and table index, the token its edits carry. */
void
test_filter_finds_a_card_by_its_row_token() {
    Fixture fixture;
    fixture.appendPattern("10*", "A", "Tens", "");
    fixture.appendPattern("25*", "A", "Twenty-fives", "");
    fixture.appendPattern("39*", "A", "Thirty-nines", "");
    fixture.append(4000, 4000, "A", "First", "");
    fixture.append(4000, 4000, "A", "Second", "");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    history.beginSession();
    model.setHistorySession(history.session());
    model.refresh(fixture.opts, fixture.state);
    TalkgroupFilterModel view;
    view.setSourceModel(&model);
    expect("five listed rows", view.count() == 5);
    auto token = [&view](int row) {
        const QModelIndex at = view.index(row, 0);
        return std::make_tuple(view.data(at, TalkgroupListModel::IdStartRole).toULongLong(),
                               view.data(at, TalkgroupListModel::IdEndRole).toULongLong(),
                               view.data(at, TalkgroupListModel::PatternRole).toString(),
                               view.data(at, TalkgroupListModel::TableIndexRole).toInt());
    };
    expect("each card is found at its own row", [&view, &token]() {
        for (int i = 0; i < view.count(); ++i) {
            const auto t = token(i);
            if (view.rowForCard(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::get<3>(t)) != i) {
                return false;
            }
        }
        return true;
    }());
    int twentyFives = -1;
    for (int i = 0; i < view.count(); ++i) {
        if (std::get<2>(token(i)) == QStringLiteral("25*")) {
            twentyFives = i;
        }
    }
    expect("the 25* card is listed", twentyFives >= 0);
    const auto t = token(twentyFives);
    expect("a card whose index moved is found by its pattern",
           view.rowForCard(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::get<3>(t) + 7) == twentyFives);
    expect("a pattern no row has is not found", view.rowForCard(0, 0, QStringLiteral("77*"), 0) == -1);
}

/* Issue #903: the model's signals since the last take(). */
struct Signals {
    int resets = 0;
    int inserts = 0;
    int removes = 0;
    int changes = 0;
    int insertedRows = 0;
    int removedRows = 0;

    explicit Signals(QAbstractItemModel& model) {
        QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [this]() { ++resets; });
        QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                         [this](const QModelIndex&, int first, int last) {
                             ++inserts;
                             insertedRows += last - first + 1;
                         });
        QObject::connect(&model, &QAbstractItemModel::rowsRemoved, &model,
                         [this](const QModelIndex&, int first, int last) {
                             ++removes;
                             removedRows += last - first + 1;
                         });
        QObject::connect(&model, &QAbstractItemModel::dataChanged, &model, [this]() { ++changes; });
    }

    bool
    none() const {
        return resets == 0 && inserts == 0 && removes == 0 && changes == 0;
    }

    void
    take() {
        *this = Signals();
    }

  private:
    Signals() = default;
};

/* A store of its own for each case (json_store name in the isolated app data folder), learning for any owner. */
struct Store {
    HeardTalkgroupStore store;

    explicit Store(const char* name) : store(fresh(name)) {
        store.setOwnerPolicy([](const QString&) { return true; });
    }

    static QString
    fresh(const char* name) {
        QFile::remove(dsd_qt::json_store_path(QLatin1String(name)));
        return QLatin1String(name);
    }
};

// Views, not QStrings: a constant initialiser that cannot throw.
constexpr QLatin1String kSysA("sys-a");
constexpr QLatin1String kSysB("sys-b");

qint64
lastHeard(const TalkgroupListModel& model, uint32_t id) {
    return model.data(find(model, id), TalkgroupListModel::LastHeardRole).toLongLong();
}

bool
remembered(const TalkgroupListModel& model, uint32_t id) {
    return model.data(find(model, id), TalkgroupListModel::RememberedRole).toBool();
}

/* The saved system's remembered talkgroups are its cards from the session's start, before any call. */
void
test_remembered_cards_at_session_start() {
    Store held("tg_list_start.json");
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 501, 1000);
    held.store.observe(kSysA, DSD_HEARD_TG_DMR, 502, 2000);
    held.store.observe(kSysB, DSD_HEARD_TG_P25, 601, 3000);
    Fixture fixture;
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    model.setHeardStore(&held.store);
    history.beginSession();
    history.setSessionOwners(history.session(), kSysA, {});
    model.setHistorySession(history.session());
    model.refresh(fixture.opts, fixture.state);
    expect("903 start: the running saved system is viewed", model.viewedOwner() == kSysA && model.remembering());
    expect("903 start: its remembered talkgroups are cards before any call",
           model.count() == 2 && remembered(model, 501) && remembered(model, 502));
    expect("903 start: each card's last heard is its entry's",
           lastHeard(model, 501) == 1000 && lastHeard(model, 502) == 2000);
    expect("903 start: not listed, and tuned as an unlisted talkgroup is",
           !model.data(find(model, 501), TalkgroupListModel::ListedRole).toBool()
               && model.data(find(model, 501), TalkgroupListModel::ListeningRole).toBool());
    expect("903 start: another system's are not", !find(model, 601).isValid());
    expect("903 start: the role names",
           model.roleNames().value(TalkgroupListModel::LastHeardRole) == "lastHeard"
               && model.roleNames().value(TalkgroupListModel::RememberedRole) == "remembered");
    // A call this session heard after the entry's last: the card shows the newer time.
    fixture.commit(501, 4000);
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 start: a newer call moves last heard", lastHeard(model, 501) == 4001 && remembered(model, 501));
    // Allow-list mode: a remembered card is an unlisted talkgroup, not tuned, and nothing is written to the table.
    fixture.opts->trunk_use_allow_list = 1;
    model.refresh(fixture.opts, fixture.state);
    expect("903 allow list: remembered cards are not tuned",
           model.count() == 2 && !model.data(find(model, 501), TalkgroupListModel::ListeningRole).toBool()
               && !model.data(find(model, 502), TalkgroupListModel::ListeningRole).toBool()
               && model.notTunedCount() == 2 && remembered(model, 501));
    expect("903 allow list: nothing is added to the policy table", dsd_tg_policy_entry_count(fixture.state) == 0);
    fixture.opts->trunk_use_allow_list = 0;
    model.refresh(fixture.opts, fixture.state);
    expect("903 allow list off: tuned again", model.notTunedCount() == 0);
    // A system remembering nothing new still shows what it remembers.
    held.store.setOwnerPolicy([](const QString&) { return false; });
    model.refresh(fixture.opts, fixture.state);
    expect("903 start: remembering off keeps the cards", !model.remembering() && model.count() == 2);
}

/* Under a scan list the cards are the target on air's system: its remembered talkgroups and only the calls this session
 * heard on it. Explore has its own context: the calls no saved system owns. */
void
test_transient_follows_the_target_on_air() {
    Store held("tg_list_hop.json");
    held.store.observe(kSysB, DSD_HEARD_TG_P25, 650, 1000);
    Fixture fixture;
    fixture.opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-a");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    model.setHeardStore(&held.store);
    history.beginSession();
    history.setSessionOwners(history.session(), QString(),
                             {{QStringLiteral("t-a"), kSysA}, {QStringLiteral("t-b"), kSysB}});
    model.setHistorySession(history.session());
    const time_t now = time(nullptr);
    fixture.commitOn(701, now, "t-a", true, false);
    fixture.commitOn(702, now + 5, "t-b", true, false);
    fixture.commitOn(703, now + 10, "t-z", true, false); // a target no saved system owns
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 hop: target A's system is viewed", model.viewedOwner() == kSysA);
    expect("903 hop: only A's call is a card", model.count() == 1 && find(model, 701).isValid());
    expect("903 hop: a session-only card is not remembered",
           !remembered(model, 701) && lastHeard(model, 701) == now + 1);
    // The scan moves to B and B hears another call, all before the next poll.
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-b");
    fixture.commitOn(704, now + 20, "t-b", true, false);
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 hop: target B's system is viewed", model.viewedOwner() == kSysB);
    expect("903 hop: B's remembered and session calls, none of A's",
           model.count() == 3 && find(model, 650).isValid() && find(model, 702).isValid() && find(model, 704).isValid()
               && !find(model, 701).isValid());
    // A relaunched UI reads the saved log: each call keeps the owner it was heard for.
    history.saveNow();
    history.waitForSaveForTest();
    {
        CallHistoryModel reloaded;
        TalkgroupListModel again(&reloaded);
        again.setHistorySession(history.session());
        again.refresh(fixture.opts, fixture.state);
        expect("903 relaunch: the saved log keeps each call's owner", again.viewedOwner() == kSysB && again.count() == 2
                                                                          && find(again, 702).isValid()
                                                                          && find(again, 704).isValid());
    }
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-z");
    model.refresh(fixture.opts, fixture.state);
    expect("903 hop: an unowned target views no system", model.viewedOwner().isEmpty() && !model.remembering());
    expect("903 hop: its context is the calls no system owns", model.count() == 1 && find(model, 703).isValid());

    // A saved-system start: an operator tune is an Explore stretch with its own cards, gone once back on the system.
    Fixture single;
    history.beginSession();
    history.setSessionOwners(history.session(), kSysA, {});
    model.setHistorySession(history.session());
    single.commit(710, now + 30);
    single.state->operator_tuned = 1U;
    single.commitOn(711, now + 40, nullptr, false, true);
    history.refresh(single.state, single.opts);
    model.refresh(single.opts, single.state);
    expect("903 explore: an operator tune views no system", model.viewedOwner().isEmpty());
    expect("903 explore: the tune's own calls only", model.count() == 1 && find(model, 711).isValid());
    single.state->operator_tuned = 0U;
    model.refresh(single.opts, single.state);
    expect("903 explore: back on the system, its calls only",
           model.viewedOwner() == kSysA && model.count() == 1 && find(model, 710).isValid());
}

/* Issue #903 (PR2 review r1, P2): the session's heard cards are the scan target's on air, not only its owner's. Two
 * targets no saved system owns (imported or frequency-only) share no cards, nor do two entries of one saved system,
 * which share only its remembered list. */
void
test_transient_scoped_to_the_target() {
    Store held("tg_list_target_scope.json");
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 660, 1000);
    Fixture fixture;
    fixture.opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-y");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    model.setHeardStore(&held.store);
    history.beginSession();
    history.setSessionOwners(history.session(), QString(),
                             {{QStringLiteral("t-a1"), kSysA}, {QStringLiteral("t-a2"), kSysA}});
    model.setHistorySession(history.session());
    const time_t now = time(nullptr);
    fixture.commitOn(42, now, "t-y", true, false);
    fixture.commitOn(721, now + 5, "t-a1", true, false);
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 target scope: the unowned target's call is its card", model.count() == 1 && find(model, 42).isValid());
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-z");
    model.refresh(fixture.opts, fixture.state);
    expect("903 target scope: another unowned target heard nothing",
           model.viewedOwner().isEmpty() && model.count() == 0);
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-a2");
    model.refresh(fixture.opts, fixture.state);
    expect("903 target scope: a system's other entry shows its remembered list, not the first entry's calls",
           model.viewedOwner() == kSysA && model.count() == 1 && find(model, 660).isValid()
               && !find(model, 721).isValid());
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-a1");
    model.refresh(fixture.opts, fixture.state);
    expect("903 target scope: the first entry shows its own call too",
           model.count() == 2 && find(model, 660).isValid() && find(model, 721).isValid());
}

/* One card per talkgroup number: every variant of it (and an NXDN Type-D id by its 11 bits) is one card, and Forget
 * removes them all, and this session's calls of it, until a later call. */
void
test_variants_one_card_and_forget() {
    Store held("tg_list_forget.json");
    const quint32 typeD = (3U << 11) | 77U; // the store keeps Type-D's full id; the card is its 11-bit id
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 77, 1000);
    held.store.observe(kSysA, DSD_HEARD_TG_DMR, 77, 2000);
    held.store.observe(kSysA, DSD_HEARD_TG_NXDN_D, typeD, 3000);
    held.store.observe(kSysA, DSD_HEARD_TG_P25, typeD, 500); // a P25 group whose number is Type-D's full id
    Fixture fixture;
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    model.setHeardStore(&held.store);
    history.beginSession();
    history.setSessionOwners(history.session(), kSysA, {});
    model.setHistorySession(history.session());
    fixture.commit(77, 2500);
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 variants: one card for the number, one for the other group",
           model.count() == 2 && find(model, 77).isValid() && find(model, typeD).isValid());
    expect("903 variants: the card's last heard is its newest variant's", lastHeard(model, 77) == 3000);
    expect("903 has remembered: the viewed system remembers some", model.hasRemembered());
    int hasRememberedSignals = 0;
    QObject::connect(&model, &TalkgroupListModel::hasRememberedChanged, &model,
                     [&hasRememberedSignals]() { ++hasRememberedSignals; });
    Signals sig(model);
    model.forgetTalkgroup(77);
    expect("903 has remembered: a forget that leaves an entry keeps it",
           model.hasRemembered() && hasRememberedSignals == 0);
    expect("903 forget: every variant of the number goes from the store", [&]() {
        const auto entries = held.store.entriesFor(kSysA);
        return entries.size() == 1 && entries.at(0).variant == DSD_HEARD_TG_P25 && entries.at(0).id == typeD;
    }());
    expect("903 forget: the card goes at once, one removal",
           model.count() == 1 && !find(model, 77).isValid() && sig.removes == 1 && sig.resets == 0);
    sig.take();
    model.refresh(fixture.opts, fixture.state);
    expect("903 forget: this session's call of it does not bring it back", !find(model, 77).isValid() && sig.none());
    fixture.commit(77, 5000);
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 forget: a later call shows it again", find(model, 77).isValid() && !remembered(model, 77)
                                                          && lastHeard(model, 77) == 5001 && sig.inserts == 1
                                                          && sig.resets == 0);
    // Clear: every card of the system, and this session's calls so far.
    sig.take();
    model.clearRemembered();
    expect("903 clear: the store holds nothing for the system", held.store.entriesFor(kSysA).isEmpty());
    expect("903 clear: no card is left, without a reset", model.count() == 0 && sig.resets == 0 && sig.removes >= 1);
    expect("903 has remembered: a clear ends it at once", !model.hasRemembered() && hasRememberedSignals == 1);
    model.refresh(fixture.opts, fixture.state);
    expect("903 clear: none comes back from this session's calls", model.count() == 0);
    // Type-D end to end: the store's full id and the call's 11-bit number are one card, and Forget takes both.
    const quint32 typeD99 = (5U << 11) | 99U;
    held.store.observe(kSysA, DSD_HEARD_TG_NXDN_D, typeD99, 1000);
    fixture.commit(99, 6000);
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 type-d: the remembered entry and the call are one card", model.count() == 1 && remembered(model, 99)
                                                                             && lastHeard(model, 99) == 6001
                                                                             && !find(model, typeD99).isValid());
    expect("903 has remembered: an entry read again", model.hasRemembered() && hasRememberedSignals == 2);
    model.forgetTalkgroup(99);
    expect("903 has remembered: forgetting the last entry ends it at once", !model.hasRemembered());
    model.refresh(fixture.opts, fixture.state);
    expect("903 type-d: forget takes the entry and hides the call",
           model.count() == 0 && held.store.entriesFor(kSysA).isEmpty() && !model.hasRemembered());
    // An edit sheet that captured the owner forgets for it even after the view moved on.
    held.store.observe(kSysB, DSD_HEARD_TG_P25, 88, 1000);
    model.forgetTalkgroupOf(kSysB, 88);
    expect("903 forget: for the captured owner", held.store.entriesFor(kSysB).isEmpty());
}

/* Listed rows still cover remembered talkgroups (exact, range, pattern); `*` covers none. */
void
test_listed_rows_cover_remembered() {
    Store held("tg_list_cover.json");
    for (const quint32 id : {900U, 915U, 925U, 930U}) {
        held.store.observe(kSysA, DSD_HEARD_TG_P25, id, 1000 + id);
    }
    Fixture fixture;
    fixture.append(900, 900, "A", "Exact", "");
    fixture.append(910, 919, "A", "Range", "");
    fixture.appendPattern("92*", "A", "Pattern", "");
    fixture.appendPattern("*", "B", "Everything else", "");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    model.setHeardStore(&held.store);
    history.beginSession();
    history.setSessionOwners(history.session(), kSysA, {});
    model.setHistorySession(history.session());
    model.refresh(fixture.opts, fixture.state);
    expect("903 cover: the listed rows and one card", model.count() == 5);
    expect("903 cover: the exact row stands for its talkgroup",
           model.data(find(model, 900), TalkgroupListModel::ListedRole).toBool() && !remembered(model, 900)
               && lastHeard(model, 900) == 0);
    expect("903 cover: range and pattern cover theirs", !find(model, 915).isValid() && !find(model, 925).isValid());
    expect("903 cover: `*` keeps the card, deciding its listening",
           find(model, 930).isValid() && remembered(model, 930)
               && !model.data(find(model, 930), TalkgroupListModel::ListeningRole).toBool());
    // A row added for the card is the same card, listed now: one change, its place kept.
    Signals sig(model);
    fixture.append(930, 930, "A", "Named", "");
    model.refresh(fixture.opts, fixture.state);
    expect("903 cover: adding a row for the card changes it in place",
           model.count() == 5 && model.data(find(model, 930), TalkgroupListModel::ListedRole).toBool()
               && sig.resets == 0 && sig.inserts == 0 && sig.removes == 0);
}

/* Granular updates: a last heard that moves is one dataChanged; an idle tick reads and signals nothing; a busy poll
 * reads the history again and signals only what moved. */
void
test_incremental_updates() {
    Store held("tg_list_updates.json");
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 501, 1000);
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 502, 1000);
    Fixture fixture;
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    model.setHeardStore(&held.store);
    history.beginSession();
    history.setSessionOwners(history.session(), kSysA, {});
    model.setHistorySession(history.session());
    model.refresh(fixture.opts, fixture.state);
    Signals sig(model);
    int changedRow = -1;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&changedRow](const QModelIndex& first, const QModelIndex& last) {
                         changedRow = first.row() == last.row() ? first.row() : -2;
                     });
    const auto before = model.workCountsForTest();
    for (int i = 0; i < 10; ++i) {
        model.refresh(fixture.opts, fixture.state);
    }
    const auto idle = model.workCountsForTest();
    expect("903 idle: ten ticks read nothing",
           idle.listedReads == before.listedReads && idle.rememberedReads == before.rememberedReads
               && idle.historyReads == before.historyReads && idle.wholeListUpdates == before.wholeListUpdates);
    expect("903 idle: and signal nothing", sig.none());
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 501, 5000);
    model.refresh(fixture.opts, fixture.state);
    expect("903 update: a later sighting is one dataChanged on its row",
           sig.changes == 1 && changedRow == find(model, 501).row() && sig.resets == 0 && sig.inserts == 0
               && sig.removes == 0 && lastHeard(model, 501) == 5000);
    sig.take();
    fixture.commit(502, 6000);
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    const auto busy = model.workCountsForTest();
    expect("903 busy: a call on a card is one dataChanged", sig.changes == 1 && changedRow == find(model, 502).row()
                                                                && sig.resets == 0 && lastHeard(model, 502) == 6001);
    expect("903 busy: the history is read, the list is not rebuilt",
           busy.historyReads == idle.historyReads + 1 && busy.listedReads == idle.listedReads
               && busy.wholeListUpdates == idle.wholeListUpdates);
}

/* 2000 remembered talkgroups (the cap): listed at the start, then a call, an update and an eviction each touch one row. */
void
test_two_thousand_remembered() {
    Store held("tg_list_cap.json");
    for (quint32 i = 0; i < 2000U; ++i) {
        held.store.observe(kSysA, DSD_HEARD_TG_P25, 10000U + i, 100000 + static_cast<qint64>(i));
    }
    Fixture fixture;
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    model.setHeardStore(&held.store);
    history.beginSession();
    history.setSessionOwners(history.session(), kSysA, {});
    model.setHistorySession(history.session());
    Signals sig(model);
    model.refresh(fixture.opts, fixture.state);
    expect("903 cap: 2000 cards in order",
           model.count() == 2000
               && model.data(model.index(0, 0), TalkgroupListModel::IdStartRole).toULongLong() == 10000U
               && model.data(model.index(1999, 0), TalkgroupListModel::IdStartRole).toULongLong() == 11999U);
    expect("903 cap: inserted as one run, no reset", sig.inserts == 1 && sig.insertedRows == 2000 && sig.resets == 0);
    sig.take();
    // Review r1: one sighting of a remembered talkgroup is one store lookup, one number merged and one row signal.
    int changedRow = -1;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model,
                     [&changedRow](const QModelIndex& first, const QModelIndex&) { changedRow = first.row(); });
    const auto quiet = model.workCountsForTest();
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 10500, 150000);
    model.refresh(fixture.opts, fixture.state);
    const auto one = model.workCountsForTest();
    expect("903 cap: a sighting is one dataChanged on its card",
           sig.changes == 1 && sig.inserts == 0 && sig.removes == 0 && sig.resets == 0
               && changedRow == find(model, 10500).row() && lastHeard(model, 10500) == 150000);
    expect("903 cap: a sighting reads one entry and merges one number, no whole read",
           one.entriesRead == quiet.entriesRead + 1 && one.numbersMerged == quiet.numbersMerged + 1
               && one.rememberedReads == quiet.rememberedReads && one.listedReads == quiet.listedReads
               && one.wholeListUpdates == quiet.wholeListUpdates && one.historyReads == quiet.historyReads);
    // A timing smoke check: 500 sightings, each followed by a poll (generous: a debug build on a loaded host).
    const uint64_t began = dsd_realtime_mono_ms();
    for (quint32 i = 0; i < 500U; ++i) {
        held.store.observe(kSysA, DSD_HEARD_TG_P25, 10000U + (i * 7U) % 2000U, 160000 + static_cast<qint64>(i));
        model.refresh(fixture.opts, fixture.state);
    }
    const uint64_t spent = dsd_realtime_mono_ms() - began;
    const auto many = model.workCountsForTest();
    expect("903 cap: 500 sightings take one entry and one number each",
           many.entriesRead == one.entriesRead + 500 && many.numbersMerged == one.numbersMerged + 500
               && many.rememberedReads == one.rememberedReads && many.wholeListUpdates == one.wholeListUpdates);
    if (spent > 2000U) {
        DSD_FPRINTF(stderr, "903 cap: 500 sightings took %llu ms\n", static_cast<unsigned long long>(spent));
    }
    expect("903 cap: 500 sightings in under 2 s", spent <= 2000U);
    sig.take();
    const auto start = model.workCountsForTest();
    fixture.commit(5, time(nullptr));
    history.refresh(fixture.state, fixture.opts);
    model.refresh(fixture.opts, fixture.state);
    expect("903 cap: a new talkgroup heard is one insert",
           sig.inserts == 1 && sig.insertedRows == 1 && sig.removes == 0 && sig.resets == 0 && sig.changes == 0
               && find(model, 5).row() == 0 && model.workCountsForTest().wholeListUpdates == start.wholeListUpdates);
    expect("903 cap: a call merges only its own number, the store is not read",
           model.workCountsForTest().numbersMerged == start.numbersMerged + 1
               && model.workCountsForTest().entriesRead == start.entriesRead && model.notTunedCount() == 0);
    sig.take();
    const quint32 oldest = held.store.entriesFor(kSysA).constLast().id; // what the cap evicts next
    held.store.observe(kSysA, DSD_HEARD_TG_P25, 12000, 200000);
    model.refresh(fixture.opts, fixture.state);
    expect("903 cap: an eviction is one remove and one insert",
           sig.removes == 1 && sig.inserts == 1 && sig.resets == 0 && !find(model, oldest).isValid()
               && find(model, 12000).isValid() && model.count() == 2001);
    sig.take();
    for (int i = 0; i < 5; ++i) {
        model.refresh(fixture.opts, fixture.state);
    }
    expect("903 cap: idle ticks signal nothing", sig.none());
}

/* The edit sheet's encryption lookup is the viewed owner's: a talkgroup observed encrypted on A says nothing on B. */
void
test_crypto_lookup_scoped_to_owner() {
    Fixture fixture;
    fixture.opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-a");
    CallHistoryModel history;
    TalkgroupListModel model(&history);
    history.beginSession();
    history.setSessionOwners(history.session(), QString(),
                             {{QStringLiteral("t-a"), kSysA}, {QStringLiteral("t-b"), kSysB}});
    model.setHistorySession(history.session());
    dsd_call_observation call = {};
    call.protocol = DSD_SYNC_P25P1_POS;
    call.slot = 0U;
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = 55;
    call.policy_target_id = 55;
    call.ota_source_id = 1;
    call.observed_m = dsd_decode_now_mono_s();
    expect("903 crypto: a call on the air", dsd_call_state_observe(fixture.state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    model.refresh(fixture.opts, fixture.state);
    expect("903 crypto: observed for A",
           model.viewedOwner() == kSysA && model.observedDecryption(55).value("lastObserved").toBool());
    dsd_state_ext_free_all(fixture.state); // the call is gone from the air (the policy table had none)
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-b");
    model.refresh(fixture.opts, fixture.state);
    expect("903 crypto: nothing for B", model.viewedOwner() == kSysB && model.observedDecryption(55).isEmpty());
    DSD_SNPRINTF(fixture.state->trunk_scan_active_id, sizeof(fixture.state->trunk_scan_active_id), "%s", "t-a");
    model.refresh(fixture.opts, fixture.state);
    expect("903 crypto: A's is still A's", model.observedDecryption(55).value("lastObserved").toBool());
}
} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-talkgroups-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    test_edit_fields_and_version();
    test_temporary_avoid_status();
    test_policy_and_heard_rows();
    test_history_role_names_and_mutations();
    test_private_calls_are_not_heard_talkgroups();
    test_replay_heard_again_lists_its_talkgroups();
    test_heard_rows_read_history_storage_directly();
    test_overlapping_ranges_cover_heard_talkgroups();
    test_pattern_rows();
    test_filter_finds_a_row_by_both_bounds();
    test_filter_finds_a_card_by_its_row_token();
    test_catch_all_keeps_heard_talkgroups();
    test_catch_all_decides_heard_card_state();
    test_pattern_row_equality();
    test_remembered_cards_at_session_start();
    test_transient_follows_the_target_on_air();
    test_transient_scoped_to_the_target();
    test_variants_one_card_and_forget();
    test_listed_rows_cover_remembered();
    test_incremental_updates();
    test_two_thousand_remembered();
    test_crypto_lookup_scoped_to_owner();
    QDir(dataDir).removeRecursively();
    if (failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
