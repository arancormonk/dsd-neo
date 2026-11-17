// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QMap>
#include <QObject>
#include <QString>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include "dsd-neo/core/state_fwd.h"
#include "p25_network_model.h"

static void
check(bool condition) {
    if (!condition) {
        std::fputs("network model assertion failed\n", stderr);
        std::abort();
    }
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    static dsd_state s;
    dsd_qt::P25NetworkModel model;
    int changes = 0;
    QObject::connect(&model, &dsd_qt::P25NetworkModel::radiosChanged, [&]() { ++changes; });
    for (int i = 0; i < 256; ++i) {
        s.p25_aff_rid[i] = i + 1;
        s.p25_aff_last_seen[i] = i;
    }
    s.p25_nb_count = 1;
    s.p25_nb_entries[0].freq = 851000000;
    s.p25_nb_entries[0].wacn = 0xabcde;
    s.p25_nb_entries[0].wacn_valid = 1;
    s.p25_patch_count = 1;
    s.p25_patch_sgid[0] = 42;
    s.p25_patch_active[0] = 1;
    for (int i = 0; i < 512; ++i) {
        s.p25_ga_rid[i] = i + 1;
        s.p25_ga_tg[i] = 42;
    }
    int nbChanges = 0, patchChanges = 0, affChanges = 0;
    QObject::connect(&model, &dsd_qt::P25NetworkModel::neighboursChanged, [&]() { ++nbChanges; });
    QObject::connect(&model, &dsd_qt::P25NetworkModel::patchesChanged, [&]() { ++patchChanges; });
    QObject::connect(&model, &dsd_qt::P25NetworkModel::affiliationsChanged, [&]() { ++affChanges; });
    model.refresh(&s);
    check(model.radios().isEmpty() && model.neighbours().isEmpty() && model.patches().isEmpty()
          && model.affiliations().isEmpty());
    model.setActive(true);
    model.refresh(&s);
    check(model.radios().size() == 100 && changes == 1);
    check(model.radios()[0].toMap()["rid"].toUInt() == 256);
    model.refresh(&s);
    check(changes == 1 && nbChanges == 1 && patchChanges == 1 && affChanges == 1);
    check(model.neighbours()[0].toMap()["wacn"].toUInt() == 0xabcde);
    check(model.patches()[0].toMap()["sgid"].toUInt() == 42);
    check(model.affiliations().size() == 100);
    /* Issue #782: last-seen times that move without changing the order change nothing shown, so no row list changes
     * (a change rebuilt the Network sheet's rows). */
    for (int i = 0; i < 256; ++i) {
        s.p25_aff_last_seen[i] = i + 1000;
    }
    s.p25_nb_entries[0].last_seen = 1234;
    model.refresh(&s);
    check(changes == 1 && nbChanges == 1 && affChanges == 1);
    s.p25_aff_rid[255] = 999;
    check(model.radios()[0].toMap()["rid"].toUInt() == 256); // Owned copy.
    model.setActive(false);
    model.refresh(&s);
    check(changes == 1);
    model.clear();
    check(model.radios().isEmpty() && changes == 2);
    model.clear();
    check(changes == 2 && nbChanges == 2 && patchChanges == 2 && affChanges == 2);
    model.setActive(true);
    model.refresh(&s);
    model.refresh(nullptr);
    check(model.radios().isEmpty());
    /* Affiliation and radio rows carry their names (the facade's lookup); a name learned later renames the row. */
    model.refresh(&s);
    check(model.radios()[0].toMap()["rid"].toUInt() == 999 && model.radios()[0].toMap()["name"].toString().isEmpty());
    const int before = changes;
    dsd_source_alias_store* names = dsd_source_alias_store_create();
    dsd_source_alias_entry alice = {};
    alice.id_start = alice.id_end = 999; // the newest radio row (rid 256's slot was reused above)
    DSD_SNPRINTF(alice.name, sizeof alice.name, "%s", "Alice");
    check(names != nullptr && dsd_source_alias_store_append(names, &alice) == 0);
    dsd_source_alias_install(&s, names);
    model.refresh(&s);
    check(changes == before + 1 && model.radios()[0].toMap()["name"].toString() == QStringLiteral("Alice"));
    const QVariantMap affiliation = model.affiliations()[0].toMap();
    check(affiliation.contains("name") && affiliation.contains("tgName"));
    dsd_state_ext_free_all(&s);
    return 0;
}
