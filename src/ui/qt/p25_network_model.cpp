// SPDX-License-Identifier: GPL-3.0-or-later
#include "p25_network_model.h"
#include <QVariantMap>
#include <dsd-neo/app_control/p25_network.h>

namespace dsd_qt {
void
P25NetworkModel::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

void
P25NetworkModel::clear() {
    if (!m_neighbours.isEmpty()) {
        m_neighbours.clear();
        Q_EMIT neighboursChanged();
    }
    if (!m_patches.isEmpty()) {
        m_patches.clear();
        Q_EMIT patchesChanged();
    }
    if (!m_affiliations.isEmpty()) {
        m_affiliations.clear();
        Q_EMIT affiliationsChanged();
    }
    if (!m_radios.isEmpty()) {
        m_radios.clear();
        Q_EMIT radiosChanged();
    }
}

void
P25NetworkModel::refresh(const dsd_state* snapshot) {
    if (!m_active) {
        return;
    }
    constexpr int cap = 100;
    QVariantList neighbours, patches, affiliations, radios;
    dsd_app_p25_neighbor nb[cap];
    int count = dsd_app_p25_neighbors(snapshot, nb, cap);
    for (int i = 0; i < count; ++i) {
        const auto& r = nb[i];
        neighbours.append(QVariantMap{{"freqHz", QVariant::fromValue<qlonglong>(r.freq_hz)},
                                      {"wacn", r.wacn},
                                      {"sysid", r.sysid},
                                      {"rfss", r.rfss},
                                      {"site", r.site},
                                      {"lra", r.lra},
                                      {"wacnValid", bool(r.wacn_valid)},
                                      {"lraValid", bool(r.lra_valid)},
                                      {"isCurrentCc", bool(r.is_current_cc)},
                                      {"isCandidate", bool(r.is_candidate)},
                                      {"cfvaText", QString::fromLatin1(r.cfva_text)},
                                      {"lastSeen", QVariant::fromValue<qlonglong>(r.last_seen)}});
    }
    dsd_app_p25_patch patch[8];
    count = dsd_app_p25_patches(snapshot, patch, 8);
    for (int i = 0; i < count; ++i) {
        const auto& r = patch[i];
        QVariantList groups, units;
        for (int j = 0; j < r.group_count; ++j) {
            groups.append(r.groups[j]);
        }
        for (int j = 0; j < r.radio_count; ++j) {
            units.append(r.radios[j]);
        }
        patches.append(QVariantMap{{"sgid", r.sgid},
                                   {"isPatch", bool(r.is_patch)},
                                   {"groups", groups},
                                   {"radios", units},
                                   {"lastSeen", QVariant::fromValue<qlonglong>(r.last_seen)}});
    }
    dsd_app_p25_affiliation aff[cap];
    count = dsd_app_p25_group_affiliations(snapshot, aff, cap);
    for (int i = 0; i < count; ++i) {
        affiliations.append(QVariantMap{
            {"rid", aff[i].rid}, {"tg", aff[i].tg}, {"lastSeen", QVariant::fromValue<qlonglong>(aff[i].last_seen)}});
    }
    count = dsd_app_p25_affiliated_rids(snapshot, aff, cap);
    for (int i = 0; i < count; ++i) {
        radios.append(QVariantMap{{"rid", aff[i].rid}, {"lastSeen", QVariant::fromValue<qlonglong>(aff[i].last_seen)}});
    }
    if (m_neighbours != neighbours) {
        m_neighbours = neighbours;
        Q_EMIT neighboursChanged();
    }
    if (m_patches != patches) {
        m_patches = patches;
        Q_EMIT patchesChanged();
    }
    if (m_affiliations != affiliations) {
        m_affiliations = affiliations;
        Q_EMIT affiliationsChanged();
    }
    if (m_radios != radios) {
        m_radios = radios;
        Q_EMIT radiosChanged();
    }
}
} // namespace dsd_qt
