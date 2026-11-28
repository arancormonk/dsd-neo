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

#include <QLatin1String>
#include <QString>
#include "scan_protocol_catalog.h"

namespace dsd_qt {

/** The protocol a decode flag runs, as a scan list names it ("p25", "tetra_dmo", ...; scan_protocol_for_flag()); empty
    for none it maps. */
inline QString
scan_entry_protocol_for_flag(const QString& decodeFlag) {
    const ScanProtocol* protocol = scan_protocol_for_flag(decodeFlag);
    return protocol ? QString::fromLatin1(protocol->id) : QString();
}

/** @p protocol (a scan-list protocol id) as the scan list editor names it (ScanProtocol::label); empty for an id the
    catalog does not have. */
inline QString
scan_entry_protocol_label(const QString& protocol) {
    const ScanProtocol* found = scan_protocol_by_id(protocol);
    return found ? QString::fromLatin1(found->label) : QString();
}

/** The label of the protocol engine target type @p type names ("tetra-dmo-conventional" is "TETRA DMO";
    scan_protocol_for_type()); empty for a type the catalog does not have. */
inline QString
scan_entry_type_label(const QString& type) {
    const ScanProtocol* found = scan_protocol_for_type(type);
    return found ? QString::fromLatin1(found->label) : QString();
}

/** The label a saved system's decode flag @p decodeFlag goes by (issue #748): the scan list's for the protocol it runs,
    except TETRA's -fT, which decodes trunked and direct mode cells alike and so is "TETRA". Only a scan list's own TETRA
    rows, which scan direct mode alone (issue #890), are "TETRA DMO". Empty for a flag the catalog does not map. */
inline QString
scan_entry_system_label(const QString& decodeFlag) {
    const QString protocol = scan_entry_protocol_for_flag(decodeFlag);
    return protocol == QLatin1String("tetra_dmo") ? QStringLiteral("TETRA") : scan_entry_protocol_label(protocol);
}

/** @p name when it has one, else @p label (a protocol label) and @p freqMhz: "DMR · 455.5000 MHz". */
inline QString
scan_entry_labelled_name(const QString& name, const QString& label, const QString& freqMhz) {
    QString named = name.trimmed();
    if (!named.isEmpty()) {
        return named;
    }
    const QString mhz = freqMhz.trimmed();
    if (mhz.isEmpty()) {
        return label;
    }
    return label.isEmpty() ? mhz + QStringLiteral(" MHz")
                           : label + QStringLiteral(" · ") + mhz + QStringLiteral(" MHz");
}

/** @p name when it has one, else @p protocol (a scan-list protocol id) and @p freqMhz as the scan list shows them. */
inline QString
scan_entry_name(const QString& name, const QString& protocol, const QString& freqMhz) {
    return scan_entry_labelled_name(name, scan_entry_protocol_label(protocol), freqMhz);
}

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SCAN_ENTRY_NAME_H_ */
