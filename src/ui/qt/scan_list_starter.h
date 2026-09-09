// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#define DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#include <QObject>
#include <QVariantMap>

namespace dsd_qt {
class AppPrefs;
class SavedSystemsModel;

class ScanListStarter : public QObject {
    Q_OBJECT
  public:
    explicit ScanListStarter(const AppPrefs* prefs, const SavedSystemsModel* systems, QObject* parent = nullptr);
    Q_INVOKABLE QVariantMap build(const QVariantMap& list) const;

  private:
    const AppPrefs* m_prefs;
    const SavedSystemsModel* m_systems;
};
} // namespace dsd_qt
#endif
