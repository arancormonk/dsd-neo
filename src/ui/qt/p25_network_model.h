// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_SRC_UI_QT_P25_NETWORK_MODEL_H_
#define DSD_NEO_SRC_UI_QT_P25_NETWORK_MODEL_H_
#include <QObject>
#include <QVariantList>
#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {
/** Owned network values from UiController's held snapshot; no independent reader. */
class P25NetworkModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QVariantList neighbours READ neighbours NOTIFY neighboursChanged)
    Q_PROPERTY(QVariantList patches READ patches NOTIFY patchesChanged)
    Q_PROPERTY(QVariantList affiliations READ affiliations NOTIFY affiliationsChanged)
    Q_PROPERTY(QVariantList radios READ radios NOTIFY radiosChanged)
  public:
    explicit P25NetworkModel(QObject* parent = nullptr) : QObject(parent) {}

    bool
    active() const {
        return m_active;
    }

    void setActive(bool active);
    void refresh(const dsd_state* snapshot);
    void clear();

    QVariantList
    neighbours() const {
        return m_neighbours;
    }

    QVariantList
    patches() const {
        return m_patches;
    }

    QVariantList
    affiliations() const {
        return m_affiliations;
    }

    QVariantList
    radios() const {
        return m_radios;
    }

  Q_SIGNALS:
    void activeChanged();
    void neighboursChanged();
    void patchesChanged();
    void affiliationsChanged();
    void radiosChanged();

  private:
    bool m_active = false;
    QVariantList m_neighbours;
    QVariantList m_patches;
    QVariantList m_affiliations;
    QVariantList m_radios;
};
} // namespace dsd_qt
#endif
