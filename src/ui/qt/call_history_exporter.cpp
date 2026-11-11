// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_exporter.h"

#include <QList>
#include <QModelIndex>
#include <QStandardPaths>
#include <QVariant>
#include <utility>

#include "call_history_export.h"
#include "call_history_filter.h"
#include "call_history_model.h"
#include "decoder_host.h"
#include "document_save_transport.h"
#include "pending_export.h"

namespace dsd_qt {

CallHistoryExporter::CallHistoryExporter(const DecoderHost* host, CallHistoryModel* model, QObject* parent)
    : CallHistoryExporter(
          host != nullptr ? host->saveTransport() : std::make_shared<DocumentSaveTransport>(),
          QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/history-export"), model,
          [] { return QTimeZone::systemTimeZone(); }, parent) {}

CallHistoryExporter::CallHistoryExporter(std::shared_ptr<DocumentSaveTransport> transport, const QString& cacheRoot,
                                         CallHistoryModel* model, std::function<QTimeZone()> zone, QObject* parent)
    : QObject(parent), m_model(model), m_zone(std::move(zone)) {
    /* The app's exporter lives as long as the QML engine, so its writer never goes before the app's teardown: a writer
       destroyed with a save running aborts its transport, which on Android stops every document copy in the process.
       The writer's job holds only the file's bytes, never this model or the recording store. */
    m_export = std::make_unique<PendingExport>(
        std::move(transport), cacheRoot, this, [this](int token, bool ok) { Q_EMIT finished(token, ok); },
        [this] { Q_EMIT busyChanged(); });
}

/* First: a save still running is aborted and waited for (DocumentExportWriter), and replies to nothing after. */
CallHistoryExporter::~CallHistoryExporter() { m_export.reset(); }

QVariantMap
CallHistoryExporter::prepare(QObject* view) {
    m_export->reset();
    const auto* proxy = qobject_cast<const CallHistoryFilterModel*>(view);
    if (proxy == nullptr || m_model == nullptr || proxy->sourceModel() != m_model) {
        return {{QStringLiteral("token"), 0}, {QStringLiteral("count"), -1}};
    }
    /* The proxy never sorts, so its rows come in the model's order, which is the list's. */
    const int shown = proxy->rowCount();
    QList<int> sourceRows;
    sourceRows.reserve(shown);
    for (int row = 0; row < shown; row++) {
        const QModelIndex source = proxy->mapToSource(proxy->index(row, 0));
        if (source.isValid()) {
            sourceRows.append(source.row());
        }
    }
    const QList<CallHistoryModel::ExportRow> rows = m_model->exportRows(sourceRows);
    if (rows.isEmpty()) {
        return {{QStringLiteral("token"), 0}, {QStringLiteral("count"), 0}};
    }
    const int token = m_export->hold(call_history_export::file(rows, m_zone ? m_zone() : QTimeZone::systemTimeZone()));
    return {{QStringLiteral("token"), token},
            {QStringLiteral("count"), static_cast<int>(rows.size())},
            {QStringLiteral("total"), m_model->count()}};
}

bool
CallHistoryExporter::save(int token, const QString& reference) {
    return m_export->save(token, reference);
}

void
CallHistoryExporter::discard(int token) {
    m_export->discard(token);
}

bool
CallHistoryExporter::busy() const {
    return m_export->busy();
}

} // namespace dsd_qt
