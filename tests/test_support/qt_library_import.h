// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_TEST_SUPPORT_QT_LIBRARY_IMPORT_H
#define DSD_NEO_TEST_SUPPORT_QT_LIBRARY_IMPORT_H

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>

#include "imported_files_model.h"

/*
 * A fixture's one-call import (#832): begin it through the library's async API and spin a local event loop until its
 * importFinished() answer, or 10 s ({ok: false, error: "timeout"}). A local file is stored under its own name.
 */
inline QVariantMap
dsd_test_import_and_wait(dsd_qt::ImportedFilesModel& library, const QString& reference, const QString& type,
                         const QString& replacePath = QString(), const QVariantMap& companions = QVariantMap()) {
    QVariantMap result{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("timeout")}};
    int wanted = 0;
    QEventLoop loop;
    // importFinished() is queued, so it cannot arrive before the id is known.
    const QMetaObject::Connection answered = QObject::connect(&library, &dsd_qt::ImportedFilesModel::importFinished,
                                                              &loop, [&](int id, const QVariantMap& answer) {
                                                                  if (id == wanted) {
                                                                      result = answer;
                                                                      loop.quit();
                                                                  }
                                                              });
    wanted = library.beginImport(reference, QFileInfo(reference).fileName(), type, companions, replacePath);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(answered);
    return result;
}

/*
 * dsd_test_import_and_wait() of a copy of @p source picked under @p name: a copy keeps the picked file's own name, so
 * a fixture stored under a chosen name is picked under it. {ok: false, error: "fixture"} when the copy cannot be made.
 */
inline QVariantMap
dsd_test_import_named_and_wait(dsd_qt::ImportedFilesModel& library, const QString& source, const QString& name,
                               const QString& type) {
    QTemporaryDir picked;
    const QString named = picked.filePath(name);
    if (!picked.isValid() || !QFile::copy(source, named)) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("fixture")}};
    }
    return dsd_test_import_and_wait(library, named, type);
}

#endif
