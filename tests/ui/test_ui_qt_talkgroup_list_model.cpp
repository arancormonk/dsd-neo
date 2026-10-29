// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QAbstractListModel>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <Qt>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <utility>
#include "../test_support/qt_test_paths.h"

#include "call_history_model.h"
#include "talkgroup_filter_model.h"
#include "talkgroup_list_model.h"

using dsd_qt::CallHistoryModel;
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

    int
    rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return !parent.isValid() && present ? 1 : 0;
    }

    QHash<int, QByteArray>
    roleNames() const override {
        return {{Qt::UserRole + 71, "tg"}, {Qt::UserRole + 72, "session"}, {Qt::UserRole + 73, "kind"}};
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
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&]() { ++resets; });
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
    expect("another pattern resets the rows", resets == 1);
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
    test_replay_heard_again_lists_its_talkgroups();
    test_heard_rows_read_history_storage_directly();
    test_overlapping_ranges_cover_heard_talkgroups();
    test_pattern_rows();
    test_catch_all_keeps_heard_talkgroups();
    test_pattern_row_equality();
    QDir(dataDir).removeRecursively();
    if (failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
