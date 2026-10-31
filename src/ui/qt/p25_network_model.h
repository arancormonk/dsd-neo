// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_SRC_UI_QT_P25_NETWORK_MODEL_H_
#define DSD_NEO_SRC_UI_QT_P25_NETWORK_MODEL_H_
// Export the container and element definitions used by the inline QVariantList getters.
#include <QList> // IWYU pragma: export
#include <QObject>
#include <QVariant> // IWYU pragma: export
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

    void setActive(bool enabled);
    void refresh(const dsd_state* snapshot);
    /**
     * @brief Publish a new scan target's network in one replacement (issue #782).
     *
     * With the sheet open, each list goes straight from the old target's rows to @p snapshot's, so the sheet keeps
     * the rows both list rather than emptying and refilling. Closed, or with no @p snapshot (no live session), the
     * lists are cleared: no old target's row survives into the new one either way.
     */
    void replaceForTarget(const dsd_state* snapshot);
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
