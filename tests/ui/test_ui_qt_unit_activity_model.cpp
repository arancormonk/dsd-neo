// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The Activity screen's model: rows newest first, a merge updated in place, one gap row however often a loss is
 * reported, the ring's size held, a new log or a stop starting over, names renamed in place when the name key moves
 * (each system's own across an A -> B -> A hop), and the screen's filter.
 *
 * Rows are synthetic (dsd_unit_activity_push()); all IDs are made up (WACN 0xBEE00, SYS 0x123 / 0x456, RID 42).
 */

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QModelIndex>
#include <QSignalSpy>
#include <QString>
#include <QVariant>
#include <QtGlobal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/alias_learn.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/unit_activity.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <initializer_list>
#include <memory>
#include "unit_activity_model.h"

namespace {

constexpr uint32_t kWacn = 0xBEE00;
constexpr uint16_t kSysA = 0x123;
constexpr uint16_t kSysB = 0x456;
constexpr uint32_t kRid = 42;
constexpr uint64_t kT0Ns = UINT64_C(1700000000000000000);

void
check_at(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "UI_QT_UNIT_ACTIVITY_MODEL: line %d: %s\n", line, what);
        std::abort();
    }
}

#define CHECK(cond) check_at((cond), #cond, __LINE__)

struct StateFree {
    void
    operator()(dsd_state* st) const {
        dsd_state_ext_free_all(st);
        std::free(st);
    }
};

using State = std::unique_ptr<dsd_state, StateFree>;

State
new_state() {
    auto* st = static_cast<dsd_state*>(std::calloc(1, sizeof(dsd_state)));
    CHECK(st != nullptr);
    return State(st);
}

void
tune_p25(dsd_state* st, uint16_t sysid) {
    dsd_rxid_reset(st);
    dsd_rxid_note_p25_network(st, kWacn, sysid);
}

dsd_unit_activity
make_row(uint8_t kind, uint8_t role, uint8_t result, uint32_t subject) {
    dsd_unit_activity r;
    DSD_MEMSET(&r, 0, sizeof(r));
    r.protocol = DSD_UA_PROTO_P25P1;
    r.kind = kind;
    r.role = role;
    r.result = result;
    r.subject.id = subject;
    r.subject.subtype = DSD_UA_ID_RADIO;
    r.subject.valid = 1U;
    return r;
}

dsd_unit_activity
registration(uint32_t subject) {
    return make_row(DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, subject);
}

dsd_unit_activity
joined(uint32_t subject, uint32_t group, const char* frozen) {
    dsd_unit_activity r = make_row(DSD_UA_GROUP_AFFILIATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_ACCEPTED, subject);
    r.group = group;
    r.group_valid = 1U;
    DSD_SNPRINTF(r.name_frozen, sizeof r.name_frozen, "%s", frozen);
    return r;
}

void
push(dsd_state* st, const dsd_unit_activity& r) {
    CHECK(dsd_unit_activity_push(st, &r) >= 0);
}

QVariant
role(const dsd_qt::UnitActivityModel& model, int row, int which) {
    return model.data(model.index(row, 0), which);
}

quint64
seq_at(const dsd_qt::UnitActivityModel& model, int row) {
    return role(model, row, dsd_qt::UnitActivityModel::SeqRole).toULongLong();
}

int
gap_rows(const dsd_qt::UnitActivityModel& model) {
    int gaps = 0;
    for (int i = 0; i < model.rowCount(); ++i) {
        gaps += role(model, i, dsd_qt::UnitActivityModel::GapRole).toBool() ? 1 : 0;
    }
    return gaps;
}

/* Every data row's seq is lower than the one above it. */
bool
newest_first(const dsd_qt::UnitActivityModel& model) {
    quint64 above = UINT64_MAX;
    for (int i = 0; i < model.rowCount(); ++i) {
        if (role(model, i, dsd_qt::UnitActivityModel::GapRole).toBool()) {
            continue;
        }
        const quint64 seq = seq_at(model, i);
        if (seq >= above) {
            return false;
        }
        above = seq;
    }
    return true;
}

/* Rows arrive at the top, newest first, with the view's texts in their roles; a kill row carries the warning tone. */
void
test_rows_newest_first() {
    State st = new_state();
    dsd_decode_clock_use_test(kT0Ns);
    tune_p25(st.get(), kSysA);
    dsd_qt::UnitActivityModel model;
    QSignalSpy counted(&model, &dsd_qt::UnitActivityModel::countChanged);
    model.refresh(st.get());
    CHECK(model.rowCount() == 0 && model.count() == 0 && counted.isEmpty());

    push(st.get(), registration(1234567));
    dsd_unit_activity deny = make_row(DSD_UA_DENY, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 1234568);
    deny.reason_code = 0x20;
    deny.reason_set = DSD_UA_REASON_P25_DENY;
    push(st.get(), deny);
    push(st.get(), make_row(DSD_UA_KILL, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 1234569));
    model.refresh(st.get());
    CHECK(model.count() == 3 && model.rowCount() == 3 && counted.count() == 3);
    CHECK(seq_at(model, 0) == 3U && seq_at(model, 1) == 2U && seq_at(model, 2) == 1U);
    using M = dsd_qt::UnitActivityModel;
    CHECK(role(model, 0, M::HeadlineRole).toString() == QStringLiteral("Kill sent"));
    CHECK(role(model, 0, M::WarningRole).toBool());
    CHECK((role(model, 0, M::FiltersRole).toUInt() & DSD_UA_FILTER_COMMANDS) != 0U);
    CHECK(role(model, 2, M::HeadlineRole).toString() == QStringLiteral("Registration accepted"));
    CHECK(!role(model, 2, M::WarningRole).toBool());
    CHECK(role(model, 2, M::SubjectRole).toString() == QStringLiteral("1234567"));
    CHECK(role(model, 2, M::ScopeRole).toString() == QStringLiteral("WACN BEE00 SYS 123"));
    CHECK(role(model, 2, M::ProtocolRole).toString() == QStringLiteral("P25 Phase 1"));
    CHECK(role(model, 2, M::RepeatRole).toInt() == 1);
    CHECK(!role(model, 2, M::GapRole).toBool());
    /* A deny's reason is in its headline, and still in its own role. */
    const QString denied = role(model, 1, M::HeadlineRole).toString();
    const QString reason = role(model, 1, M::ReasonRole).toString();
    CHECK(!reason.isEmpty() && denied.startsWith(QStringLiteral("Denied: ")) && denied.endsWith(reason));
    const QString when =
        QDateTime::fromSecsSinceEpoch(static_cast<qint64>(kT0Ns / 1000000000U)).toString(QStringLiteral("HH:mm:ss"));
    CHECK(role(model, 0, M::TimeTextRole).toString() == when);
    CHECK(model.rowForSeq(2) == 1 && model.rowForSeq(9) == -1 && model.rowForSeq(0) == -1);
    const auto names = model.roleNames();
    for (const char* name : {"seq", "gap", "timeText", "headline", "kindLabel", "filters", "name", "subject", "target",
                             "reason", "scope", "repeat", "protocol", "warning"}) {
        CHECK(names.values().contains(QByteArray(name)));
    }
}

/* A message merged into a row comes back with its seq: the row is updated where it is, never added again or moved. */
void
test_merge_updates_in_place() {
    State st = new_state();
    dsd_decode_clock_use_test(kT0Ns);
    tune_p25(st.get(), kSysA);
    dsd_qt::UnitActivityModel model;
    push(st.get(), registration(1234567));
    push(st.get(), registration(7654321));
    model.refresh(st.get());
    CHECK(model.count() == 2);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    push(st.get(), registration(1234567)); /* the older row's message again, inside the dedup window */
    model.refresh(st.get());
    CHECK(model.count() == 2 && inserted.isEmpty() && removed.isEmpty() && reset.isEmpty());
    CHECK(changed.count() == 1);
    const QModelIndex at = changed.at(0).at(0).value<QModelIndex>();
    const QList<int> roles = changed.at(0).at(2).value<QList<int>>();
    CHECK(at.row() == 1 && roles == QList<int>{dsd_qt::UnitActivityModel::RepeatRole});
    CHECK(seq_at(model, 1) == 1U && role(model, 1, dsd_qt::UnitActivityModel::RepeatRole).toInt() == 2);
    /* Nothing new: nothing changes. */
    model.refresh(st.get());
    CHECK(changed.count() == 1 && inserted.isEmpty());
}

/* Review focus 3: the app was away while a busy site wrapped the ring past the rows it had not read. The list shows
 * the newest rows and one "not kept" row below them, and stays at one however often the loss is reported again: the
 * re-read a name change makes, and a later loss. */
void
test_model_gap_row_once() {
    State st = new_state();
    dsd_decode_clock_use_test(kT0Ns);
    tune_p25(st.get(), kSysA);
    dsd_qt::UnitActivityModel model;
    for (uint32_t i = 0; i < 4U; ++i) {
        push(st.get(), registration(10U + i));
    }
    model.refresh(st.get());
    CHECK(model.count() == 4 && gap_rows(model) == 0);

    const uint32_t away = DSD_UNIT_ACTIVITY_CAPACITY + 5U;
    for (uint32_t i = 0; i < away; ++i) {
        push(st.get(), registration(100000U + i));
    }
    model.refresh(st.get());
    const int cap = dsd_qt::UnitActivityModel::kCapacity;
    CHECK(model.count() == cap && model.rowCount() == cap + 1);
    CHECK(gap_rows(model) == 1);
    CHECK(role(model, cap, dsd_qt::UnitActivityModel::GapRole).toBool()); /* below every row read after it */
    CHECK(model.rowForSeq(0) == cap);
    CHECK(seq_at(model, 0) == 4U + away && seq_at(model, cap - 1) == 4U + away - cap + 1U);
    CHECK(newest_first(model));

    /* Reported once by the ring; nothing more on the next read. */
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
    model.refresh(st.get());
    CHECK(gap_rows(model) == 1 && inserted.isEmpty() && removed.isEmpty());

    /* A name change reads every row again from the start, and that read reports the loss again: still one gap, no row
     * added or removed. */
    CHECK(dsd_alias_learn_radio(nullptr, st.get(), nullptr, kRid, "Alice", nullptr) == DSD_TG_POLICY_LEARN_ADDED);
    model.refresh(st.get());
    CHECK(gap_rows(model) == 1 && inserted.isEmpty() && removed.isEmpty() && model.count() == cap);

    /* Away again: still one gap, below the newest rows. */
    for (uint32_t i = 0; i < away; ++i) {
        push(st.get(), registration(200000U + i));
    }
    model.refresh(st.get());
    CHECK(gap_rows(model) == 1 && model.count() == cap && model.rowForSeq(0) == cap);
    CHECK(seq_at(model, 0) == 4U + (2U * away));
    CHECK(newest_first(model));
}

/* The ring evicts by revision, so an old row the system kept repeating survives a wrap: it stays below the gap, its
 * place by seq, updated where it is. A later loss moves the gap and drops what was below it: one gap, never a hidden
 * second loss between two rows. */
void
test_gap_with_rows_below() {
    State st = new_state();
    dsd_decode_clock_use_test(kT0Ns); /* frozen: a repeat inside the lookback always merges */
    tune_p25(st.get(), kSysA);
    dsd_qt::UnitActivityModel model;
    push(st.get(), registration(1U)); /* seq 1, repeated through the wrap below */
    model.refresh(st.get());
    const uint32_t away = DSD_UNIT_ACTIVITY_CAPACITY + 5U;
    for (uint32_t i = 0; i < away; ++i) {
        push(st.get(), registration(100000U + i));
        if (i % 8U == 7U) {
            push(st.get(), registration(1U));
        }
    }
    model.refresh(st.get());
    const int cap = dsd_qt::UnitActivityModel::kCapacity;
    CHECK(gap_rows(model) == 1 && model.count() == cap);
    const int gapAt = model.rowForSeq(0);
    CHECK(gapAt == model.rowCount() - 2); /* in the middle: the kept row below it */
    CHECK(model.rowForSeq(1) == model.rowCount() - 1);
    CHECK(role(model, model.rowCount() - 1, dsd_qt::UnitActivityModel::RepeatRole).toInt() > 1);
    CHECK(newest_first(model));

    for (uint32_t i = 0; i < away; ++i) {
        push(st.get(), registration(200000U + i));
    }
    model.refresh(st.get());
    CHECK(gap_rows(model) == 1 && model.count() == cap);
    CHECK(model.rowForSeq(0) == model.rowCount() - 1 && model.rowForSeq(1) == -1);
    CHECK(newest_first(model));
}

/* Read in time, a long session keeps the ring's size: the oldest rows leave the bottom and no gap is shown. */
void
test_capacity_without_loss() {
    State st = new_state();
    dsd_decode_clock_use_test(kT0Ns);
    tune_p25(st.get(), kSysA);
    dsd_qt::UnitActivityModel model;
    const uint32_t total = DSD_UNIT_ACTIVITY_CAPACITY + 300U;
    for (uint32_t i = 0; i < total; ++i) {
        push(st.get(), registration(5000U + i));
        if (i % 100U == 99U) {
            model.refresh(st.get());
        }
    }
    model.refresh(st.get());
    const int cap = dsd_qt::UnitActivityModel::kCapacity;
    CHECK(model.count() == cap && model.rowCount() == cap && gap_rows(model) == 0);
    CHECK(seq_at(model, 0) == total && seq_at(model, cap - 1) == total - cap + 1U);
}

/* A new log (the decoder restarted) replaces every held row; a stop drops them and the next read starts over. */
void
test_reset_on_new_log_and_stop() {
    dsd_decode_clock_use_test(kT0Ns);
    State first = new_state();
    tune_p25(first.get(), kSysA);
    push(first.get(), registration(1U));
    push(first.get(), registration(2U));
    dsd_qt::UnitActivityModel model;
    model.refresh(first.get());
    CHECK(model.count() == 2);

    State second = new_state();
    tune_p25(second.get(), kSysA);
    push(second.get(), registration(77U));
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.refresh(second.get());
    CHECK(reset.count() == 1 && model.count() == 1);
    CHECK(role(model, 0, dsd_qt::UnitActivityModel::SubjectRole).toString() == QStringLiteral("77"));

    model.clear();
    CHECK(model.count() == 0 && model.rowCount() == 0 && reset.count() == 2);
    model.clear(); /* nothing to drop: no reset */
    CHECK(reset.count() == 2);
    model.refresh(second.get()); /* from the start again */
    CHECK(model.count() == 1 && seq_at(model, 0) == 1U);
    model.refresh(nullptr);
    CHECK(model.count() == 1);
}

/* A late alias renames the held row of its own system in place; across A -> B -> A each row keeps its own system's
 * name (the view's rule), read through snapshot copies as the app reads them. */
void
test_names_rename_in_place() {
    dsd_decode_clock_use_test(kT0Ns);
    State live = new_state();
    State snap = new_state();
    auto publish = [&]() {
        snap->rx_id = live->rx_id;
        CHECK(dsd_unit_activity_copy_snapshot(snap.get(), live.get()) == 0);
        CHECK(dsd_tg_policy_copy_snapshot(snap.get(), live.get()) == 0);
        CHECK(dsd_source_alias_copy_snapshot(snap.get(), live.get()) == 0);
    };
    dsd_qt::UnitActivityModel model;
    tune_p25(live.get(), kSysA);
    push(live.get(), joined(kRid, 100U, "")); /* seq 1 on A, no name yet */
    publish();
    model.refresh(snap.get());
    CHECK(model.count() == 1 && role(model, 0, dsd_qt::UnitActivityModel::NameRole).toString().isEmpty());

    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    CHECK(dsd_alias_learn_radio(nullptr, live.get(), nullptr, kRid, "Alice", nullptr) == DSD_TG_POLICY_LEARN_ADDED);
    publish();
    model.refresh(snap.get());
    CHECK(inserted.isEmpty() && changed.count() == 1);
    CHECK(changed.at(0).at(2).value<QList<int>>() == QList<int>{dsd_qt::UnitActivityModel::NameRole});
    CHECK(role(model, 0, dsd_qt::UnitActivityModel::NameRole).toString() == QStringLiteral("Alice"));

    /* Heard again on A, now with its name as its producer stores it. */
    push(live.get(), joined(kRid, 101U, "Alice")); /* seq 2 on A */
    /* Hop to B, where radio 42 is Bob: A's rows show what was heard on A, B's row B's name. */
    tune_p25(live.get(), kSysB);
    CHECK(dsd_alias_learn_radio(nullptr, live.get(), nullptr, kRid, "Bob", nullptr) == DSD_TG_POLICY_LEARN_ADDED);
    push(live.get(), joined(kRid, 200U, "Bob")); /* seq 3 on B */
    publish();
    model.refresh(snap.get());
    CHECK(model.count() == 3);
    CHECK(model.rowForSeq(3) == 0 && model.rowForSeq(2) == 1);
    CHECK(role(model, 0, dsd_qt::UnitActivityModel::NameRole).toString() == QStringLiteral("Bob"));
    CHECK(role(model, 1, dsd_qt::UnitActivityModel::NameRole).toString() == QStringLiteral("Alice"));

    /* Back on A: A's rows are Alice from A's list, B's row keeps Bob. */
    tune_p25(live.get(), kSysA);
    publish();
    model.refresh(snap.get());
    CHECK(role(model, 2, dsd_qt::UnitActivityModel::NameRole).toString() == QStringLiteral("Alice"));
    CHECK(role(model, 1, dsd_qt::UnitActivityModel::NameRole).toString() == QStringLiteral("Alice"));
    CHECK(role(model, 0, dsd_qt::UnitActivityModel::NameRole).toString() == QStringLiteral("Bob"));
    CHECK(inserted.count() == 2); /* rows are only ever added for new messages */
}

/* The screen's filter: one DSD_UA_FILTER_* bit at a time, every row at 0, the gap row under every filter. */
void
test_filter_model() {
    State st = new_state();
    dsd_decode_clock_use_test(kT0Ns);
    tune_p25(st.get(), kSysA);
    dsd_qt::UnitActivityModel model;
    dsd_qt::UnitActivityFilterModel view;
    view.setSourceModel(&model);
    QSignalSpy counted(&view, &dsd_qt::UnitActivityFilterModel::countChanged);
    QSignalSpy filtered(&view, &dsd_qt::UnitActivityFilterModel::filterChanged);
    push(st.get(), registration(1U));                                                              /* seq 1 */
    push(st.get(), joined(2U, 100U, ""));                                                          /* seq 2 */
    push(st.get(), make_row(DSD_UA_INHIBIT, DSD_UA_ROLE_COMMAND, DSD_UA_RESULT_NA, 3U));           /* seq 3 */
    push(st.get(), make_row(DSD_UA_REGISTRATION, DSD_UA_ROLE_RESPONSE, DSD_UA_RESULT_DENIED, 4U)); /* seq 4 */
    push(st.get(), make_row(DSD_UA_STATUS, DSD_UA_ROLE_REPORT, DSD_UA_RESULT_NA, 5U));             /* seq 5 */
    model.refresh(st.get());
    CHECK(view.count() == 5 && !counted.isEmpty());
    view.setFilter(DSD_UA_FILTER_LOGINS);
    CHECK(view.count() == 2 && filtered.count() == 1); /* the accepted and the denied registration */
    CHECK(view.rowForSeq(4) == 0 && view.rowForSeq(1) == 1 && view.rowForSeq(3) == -1);
    view.setFilter(DSD_UA_FILTER_LOGINS);
    CHECK(filtered.count() == 1);
    view.setFilter(DSD_UA_FILTER_REFUSED);
    CHECK(view.count() == 1 && view.rowForSeq(4) == 0);
    view.setFilter(DSD_UA_FILTER_COMMANDS);
    CHECK(view.count() == 1 && view.rowForSeq(3) == 0);
    view.setFilter(DSD_UA_FILTER_TALKGROUPS);
    CHECK(view.count() == 1 && view.rowForSeq(2) == 0);
    view.setFilter(DSD_UA_FILTER_OTHER);
    CHECK(view.count() == 1 && view.rowForSeq(5) == 0);
    /* A row arriving under a filter it is not in stays out of the view. */
    push(st.get(), registration(6U));
    model.refresh(st.get());
    CHECK(model.count() == 6 && view.count() == 1);
    /* The gap row passes every filter. */
    for (uint32_t i = 0; i < DSD_UNIT_ACTIVITY_CAPACITY + 5U; ++i) {
        push(st.get(), registration(9000U + i));
    }
    model.refresh(st.get());
    CHECK(view.rowForSeq(0) >= 0);
    view.setFilter(0);
    CHECK(view.count() == model.rowCount() && view.rowForSeq(0) == model.rowForSeq(0));
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_rows_newest_first();
    test_merge_updates_in_place();
    test_model_gap_row_once();
    test_gap_with_rows_below();
    test_capacity_without_loss();
    test_reset_on_new_log_and_stop();
    test_names_rename_in_place();
    test_filter_model();
    std::puts("UI_QT_UNIT_ACTIVITY_MODEL: OK");
    return 0;
}
