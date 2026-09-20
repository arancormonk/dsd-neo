// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#define DSD_NEO_UI_QT_SCAN_LIST_STARTER_H
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
class QString;
Q_MOC_INCLUDE("decoder_host.h")

namespace dsd_qt {
class AppPrefs;
class SavedSystemsModel;
class DecryptionProfileProvider;
class DecoderHost;

class ScanListStarter : public QObject {
    Q_OBJECT
  public:
    explicit ScanListStarter(const AppPrefs* prefs, const SavedSystemsModel* systems, QObject* parent = nullptr);
    QVariantMap build(const QVariantMap& list) const;
    Q_INVOKABLE QVariantMap start(const QVariantMap& list, DecoderHost* host) const;
    Q_INVOKABLE QVariantMap validate(const QVariantMap& list) const;

    void
    setDecryptionProfiles(DecryptionProfileProvider* profiles) {
        m_profiles = profiles;
    }

  private:
    QVariantList resolveSystems(const QVariantMap& list, QString* error) const;
    bool resolveEntryProfiles(QVariantMap& prepared, QString* error) const;
    QVariantMap prepare(const QVariantMap& list, bool materialize) const;
    const AppPrefs* m_prefs;
    const SavedSystemsModel* m_systems;
    DecryptionProfileProvider* m_profiles = nullptr;
};
} // namespace dsd_qt
#endif
