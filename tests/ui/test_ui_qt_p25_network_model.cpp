// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <cassert>
#include <dsd-neo/core/state.h>
#include "p25_network_model.h"

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
    assert(model.radios().isEmpty() && model.neighbours().isEmpty() && model.patches().isEmpty()
           && model.affiliations().isEmpty());
    model.setActive(true);
    model.refresh(&s);
    assert(model.radios().size() == 100 && changes == 1);
    assert(model.radios()[0].toMap()["rid"].toUInt() == 256);
    model.refresh(&s);
    assert(changes == 1 && nbChanges == 1 && patchChanges == 1 && affChanges == 1);
    assert(model.neighbours()[0].toMap()["wacn"].toUInt() == 0xabcde);
    assert(model.patches()[0].toMap()["sgid"].toUInt() == 42);
    assert(model.affiliations().size() == 100);
    s.p25_aff_rid[255] = 999;
    assert(model.radios()[0].toMap()["rid"].toUInt() == 256); // Owned copy.
    model.setActive(false);
    model.refresh(&s);
    assert(changes == 1);
    model.clear();
    assert(model.radios().isEmpty() && changes == 2);
    model.clear();
    assert(changes == 2 && nbChanges == 2 && patchChanges == 2 && affChanges == 2);
    model.setActive(true);
    model.refresh(&s);
    model.refresh(nullptr);
    assert(model.radios().isEmpty());
    return 0;
}
