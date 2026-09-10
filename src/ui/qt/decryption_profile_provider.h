// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_DECRYPTION_PROFILE_PROVIDER_H
#define DSD_NEO_UI_QT_DECRYPTION_PROFILE_PROVIDER_H
#include <QString>
#include <QVariantMap>

namespace dsd_qt {
/** Private C++ interface. Configuration can contain key material and must never
 * be returned through an invokable, a model role, diagnostics or a snapshot. */
class DecryptionProfileProvider {
  public:
    virtual ~DecryptionProfileProvider() = default;
    virtual QVariantMap configuration(const QString& uid, QString* error) const = 0;

    virtual void
    retainForSession(const QString& uid) {
        (void)uid;
    }
};
} // namespace dsd_qt
#endif
