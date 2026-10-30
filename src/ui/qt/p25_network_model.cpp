// SPDX-License-Identifier: GPL-3.0-or-later
#include <QMap>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>
#include <dsd-neo/app_control/p25_network.h>
#include <dsd-neo/core/state_fwd.h>
#include <utility>
#include "p25_network_model.h"

namespace dsd_qt {
void
P25NetworkModel::setActive(bool enabled) {
    if (m_active == enabled) {
        return;
    }
    m_active = enabled;
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
P25NetworkModel::replaceForTarget(const dsd_state* snapshot) {
    if (m_active && snapshot != nullptr) {
        refresh(snapshot);
    } else {
        clear();
    }
}

/* Issue #782: a row carries only what the Network sheet shows. The engine's last-seen times order the rows but are
 * not shown, and kept in the rows they made every refresh a change (each second some row's time moves), which made
 * the sheet rebuild rows a screen reader is told about. */
void
P25NetworkModel::refresh(const dsd_state* snapshot) {
    if (!m_active) {
        return;
    }
    constexpr int cap = 100;
    QVariantList neighbourRows, patchRows, affiliationRows, radioRows;
    dsd_app_p25_neighbor nb[cap];
    int count = dsd_app_p25_neighbors(snapshot, nb, cap);
    for (int i = 0; i < count; ++i) {
        const auto& r = nb[i];
        neighbourRows.append(QVariantMap{{"freqHz", QVariant::fromValue<qlonglong>(r.freq_hz)},
                                         {"wacn", r.wacn},
                                         {"sysid", r.sysid},
                                         {"rfss", r.rfss},
                                         {"site", r.site},
                                         {"lra", r.lra},
                                         {"wacnValid", bool(r.wacn_valid)},
                                         {"lraValid", bool(r.lra_valid)},
                                         {"isCurrentCc", bool(r.is_current_cc)},
                                         {"isCandidate", bool(r.is_candidate)},
                                         {"cfvaText", QString::fromLatin1(r.cfva_text)}});
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
        patchRows.append(
            QVariantMap{{"sgid", r.sgid}, {"isPatch", bool(r.is_patch)}, {"groups", groups}, {"radios", units}});
    }
    dsd_app_p25_affiliation aff[cap];
    count = dsd_app_p25_group_affiliations(snapshot, aff, cap);
    for (int i = 0; i < count; ++i) {
        affiliationRows.append(QVariantMap{{"rid", aff[i].rid}, {"tg", aff[i].tg}});
    }
    count = dsd_app_p25_affiliated_rids(snapshot, aff, cap);
    for (int i = 0; i < count; ++i) {
        radioRows.append(QVariantMap{{"rid", aff[i].rid}});
    }
    if (m_neighbours != neighbourRows) {
        m_neighbours = neighbourRows;
        Q_EMIT neighboursChanged();
    }
    if (m_patches != patchRows) {
        m_patches = patchRows;
        Q_EMIT patchesChanged();
    }
    if (m_affiliations != affiliationRows) {
        m_affiliations = affiliationRows;
        Q_EMIT affiliationsChanged();
    }
    if (m_radios != radioRows) {
        m_radios = radioRows;
        Q_EMIT radiosChanged();
    }
}
} // namespace dsd_qt
