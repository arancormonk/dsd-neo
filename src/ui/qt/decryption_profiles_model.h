// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_DECRYPTION_PROFILES_MODEL_H
#define DSD_NEO_UI_QT_DECRYPTION_PROFILES_MODEL_H
// Complete types are needed by inline Qt container and metatype instantiations.
#include <QAbstractListModel>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant> // IWYU pragma: keep
#include <QVariantList>
#include <QVariantMap>
#include "decryption_profile_provider.h"

namespace dsd_qt {
class SavedSystemsModel;
class ScanListsModel;

class DecryptionProfilesModel : public QAbstractListModel, public DecryptionProfileProvider {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
  public:
    explicit DecryptionProfilesModel(QObject* parent = nullptr);

    int
    count() const {
        return m_rows.size();
    }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setReferences(SavedSystemsModel* systems, ScanListsModel* scans);
    QVariantMap configuration(const QString& uid, QString* error) const override;
    Q_INVOKABLE QVariantMap get(const QString& uid) const;
    Q_INVOKABLE QVariantMap forRuntimeReference(const QString& reference) const;
    Q_INVOKABLE void retainForSession(const QString& uid) override;
    void releaseSessionReferences();
    Q_INVOKABLE QVariantList entries() const;
    Q_INVOKABLE QVariantMap saveProfile(const QVariantMap& draft);
    Q_INVOKABLE QVariantMap removeProfile(const QString& uid);
    Q_INVOKABLE QStringList profilesReferencingPath(const QString& path) const;
    Q_INVOKABLE int useCount(const QString& uid) const;
    Q_INVOKABLE QString newReference() const;
    Q_INVOKABLE QVariantList vendorTypes() const;
    Q_INVOKABLE QVariantList directTypes(const QString& protocol) const;
    /** Copy legacy material into a profile before updating the system reference. */
    bool migrateLegacySystems();
  Q_SIGNALS:
    void countChanged();

  private:
    int rowForUid(const QString& uid) const;
    static QVariantMap metadata(QVariantMap row);
    bool persist(const QList<QVariantMap>& rows) const;
    bool compile(QVariantMap& row, QString* error, QStringList* staged) const;
    QList<QVariantMap> m_rows;
    SavedSystemsModel* m_systems = nullptr;
    ScanListsModel* m_scans = nullptr;
    QSet<QString> m_sessionReferences;
};
} // namespace dsd_qt
#endif
