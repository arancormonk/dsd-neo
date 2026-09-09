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

    Fixture() {
        if (!opts || !state || !rings) {
            abort();
        }
        state->event_history_s = rings;
        for (int slot = 0; slot < 2; ++slot) {
            rings[slot].revision = 1;
            rings[slot].commit_rev = 1;
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
        entry.is_range = start != end;
        DSD_SNPRINTF(entry.tags, sizeof(entry.tags), "%s", tags);
        expect("append policy row", (entry.is_range ? dsd_tg_policy_add_range_entry(state, &entry)
                                                    : dsd_tg_policy_append_exact(state, &entry))
                                        == 0);
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
    model.setSinceWhen(now);
    fixture.commit(6001, now - 60);
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
    model.setSinceWhen(now + 6);
    model.refresh(fixture.opts, fixture.state);
    expect("moving cutoff removes heard rows even with unchanged policy",
           model.count() == 3 && !find(model, 4001).isValid());
    model.refresh(nullptr, fixture.state);
    expect("missing snapshot clears policy and rows", model.count() == 0 && model.notTunedCount() == 0
                                                          && model.categories().isEmpty() && !model.allowListMode()
                                                          && !model.persistent());
    model.refresh(fixture.opts, fixture.state);
    expect("clear retains session cutoff", model.count() == 3 && model.sinceWhen() == now + 6);
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
    dsd_tg_policy_entry values = {};
    values.priority = 50;
    values.preempt = 1;
    expect("edit priority and preempt",
           dsd_tg_policy_set_fields(fixture.state, 42, 42, &values,
                                    DSD_TG_POLICY_FIELD_PRIORITY | DSD_TG_POLICY_FIELD_PREEMPT)
               == 0);
    int changes = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &model, [&]() { ++changes; });
    model.refresh(fixture.opts, fixture.state);
    const auto roles = model.roleNames();
    expect("priority copied", model.data(find(model, 42), roles.key("priority", -1)).toInt() == 50);
    expect("preempt copied", model.data(find(model, 42), roles.key("preempt", -1)).toBool());
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
        return {{Qt::UserRole + 71, "tg"}, {Qt::UserRole + 72, "when"}, {Qt::UserRole + 73, "kind"}};
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

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-talkgroups-%1").arg(QCoreApplication::applicationPid()));
    QStandardPaths::setTestModeEnabled(true);
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        return 1;
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    test_edit_fields_and_version();
    test_policy_and_heard_rows();
    test_history_role_names_and_mutations();
    QDir(dataDir).removeRecursively();
    if (failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
