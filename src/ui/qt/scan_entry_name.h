// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The name a scan-list entry or saved system goes by when it has none of its own (issue #782).
 *
 * One rule for every place that names an entry: the scan target CSV's `label` column (which names the target's calls
 * and the monitor's scan header) and the monitor's tuned-frequency line. A named entry is its name; an unnamed one is
 * its protocol, as the scan list's editor names it, and its frequency: "DMR · 455.5000 MHz". Never empty, and never an
 * internal UID.
 */

#ifndef DSD_NEO_SRC_UI_QT_SCAN_ENTRY_NAME_H_
#define DSD_NEO_SRC_UI_QT_SCAN_ENTRY_NAME_H_

#include <QLatin1Char>
#include <QMap>
#include <QString>

namespace dsd_qt {

/** The protocol a decode flag runs, as a scan list names it ("p25", "dmr", ...); empty for none it maps. */
inline QString
scan_entry_protocol_for_flag(const QString& decodeFlag) {
    static const QMap<QString, QString> protocols{
        {"-ft", "p25"},    {"-f1", "p25"},  {"-mq", "p25"},    {"-^", "p25"},     {"-fs", "dmr"},
        {"-fi", "nxdn48"}, {"-fn", "nxdn"}, {"-ft -^", "p25"}, {"-mq -^", "p25"}, {"-^ -ft", "p25"},
        {"-^ -mq", "p25"}, {"-fA", "nfm"},  {"-fM", "am"}};
    return protocols.value(decodeFlag.simplified());
}

/** @p name when it has one, else @p protocol (a scan-list protocol id) and @p freqMhz as the scan list shows them. */
inline QString
scan_entry_name(const QString& name, const QString& protocol, const QString& freqMhz) {
    const QString named = name.trimmed();
    if (!named.isEmpty()) {
        return named;
    }
    // The scan list editor's protocol names (ScanListScreen.qml protocolLabels).
    static const QMap<QString, QString> labels{{"p25", "P25"},     {"dmr", "DMR"},          {"nxdn48", "NXDN48"},
                                               {"nxdn", "NXDN96"}, {"nfm", "NFM (analog)"}, {"am", "AM (analog)"}};
    const QString mhz = freqMhz.trimmed();
    const QString label = labels.value(protocol, protocol.toUpper());
    if (mhz.isEmpty()) {
        return label;
    }
    return label.isEmpty() ? mhz + QStringLiteral(" MHz")
                           : label + QStringLiteral(" · ") + mhz + QStringLiteral(" MHz");
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SCAN_ENTRY_NAME_H_ */
