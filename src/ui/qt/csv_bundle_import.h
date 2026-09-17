// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_UI_QT_CSV_BUNDLE_IMPORT_H
#define DSD_NEO_UI_QT_CSV_BUNDLE_IMPORT_H
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace dsd_qt {
class DecoderHost;

struct CsvBundleImport {
    QString name;
    QString path;
    QString root;
    QString revision;
    QString error;
    QString detail;
    QStringList required;
    QVariantMap requiredLabels;
};

/** Stage a channel map and explicitly selected companion documents. Returned
 * paths are private temporary inputs until validation/registration succeeds. */
CsvBundleImport stage_csv_bundle(DecoderHost* host, const QString& reference, const QString& name,
                                 const QVariantMap& companions, const QString& existingRoot = QString());
CsvBundleImport stage_trunk_csv_bundle(DecoderHost* host, const QString& reference, const QString& name,
                                       const QVariantMap& companions, const QString& existingRoot = QString());
void discard_csv_bundle(const CsvBundleImport& bundle, bool existingRoot = false);
/** Reclaim unreferenced revision directories on idle startup, only for owned,
 * registered target bundles that still pass complete validation. */
void clean_trunk_csv_revisions(const QString& path, const QString& root);
} // namespace dsd_qt
#endif
