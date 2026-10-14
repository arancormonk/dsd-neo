// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: the configuration backup archive (issue #580) - the typed path table, the snapshot, the export's
 * sanitizing, sealing with NFC passphrases, the restore-side validation and rebase of an untrusted archive - plus
 * DecoderHost's base document copies and json_store_root(). The stores come from the real models. Registered only
 * when the Qt frontend is enabled (DSD_ENABLE_QT_UI). */

#include <QByteArray>
#include <QChar>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtEndian>
#include <QtGlobal>
#include <atomic>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/crypto/backup_seal.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/radioreference_generate.h>
#include <functional>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <qanystringview.h>
#include <qflags.h>
#include <qsystemdetection.h>
#include <stdio.h>
#include <utility>
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "backup_archive.h"
#include "backup_archive_rules.h"
#include "decode_mode_flag.h"
#include "decoder_host.h"
#include "decryption_profiles_model.h"
#include "imported_files_model.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "signal_search_model.h"

namespace {

using dsd_qt::backup::BackupError;

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

class TestHost : public dsd_qt::DecoderHost {
  public:
    bool
    isRunning() const override {
        return false;
    }

    QString
    statusText() const override {
        return QString();
    }

    bool
    start(const QStringList& argv) override {
        (void)argv;
        return false;
    }

    void
    stop() override {}
};

bool
write_file(const QString& path, const QByteArray& contents) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray
read_file(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString
example(const char* name) {
    return QStringLiteral(DSD_NEO_TEST_EXAMPLES_DIR "/") + QLatin1String(name);
}

QString
sha256_hex(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

dsd_backup_kdf_params
test_kdf() {
    // The cheapest parameters a sealed backup may carry, so the suite stays quick.
    dsd_backup_kdf_params kdf{};
    kdf.log2_n = 14;
    kdf.r = 8;
    kdf.p = 1;
    return kdf;
}

/** Library paths and identities the real models produced. */
struct Fixture {
    QString groups, keys, bandplan, src, mapping, chan, chanRoot, targets, targetsRoot, updated, updatedRoot;
    QString systemUid, replayUid;
};

QString
import_path(dsd_qt::ImportedFilesModel& library, const QString& source, const QString& name, const QString& type) {
    const QVariantMap result = library.importFile(source, name, type);
    expect("fixture import succeeds", result.value("ok").toBool());
    return result.value("path").toString();
}

void
import_bundles(dsd_qt::ImportedFilesModel& library, Fixture& f) {
    QTemporaryDir source;
    expect("write channel bundle fixture",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(source.filePath("map.csv"), "tg_dec,keyid_hex\n123,02\n")
               && write_file(source.filePath("channels.csv"),
                             "channel,frequency_hz,mode,keys_hex_csv,options\n1,461000000,dmr,keys.csv,"
                             "--dmr-tg-key-csv 'map.csv'\n"));
    QVariantMap result = library.importBundle(source.filePath("channels.csv"), "Bundle.csv", "chan", {});
    expect("channel bundle imports", result.value("ok").toBool());
    f.chan = result.value("path").toString();
    f.chanRoot = library.get(library.rowForPath(f.chan)).value("bundleRoot").toString();

    expect("write target bundle fixture",
           write_file(source.filePath("tkeys.csv"), "id,value\n01,ABCDEF0123\n")
               && write_file(source.filePath("tmap.csv"),
                             "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,tkeys.csv\n")
               && write_file(source.filePath("targets.csv"),
                             "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                             "one,p25-trunk,851012500,tmap.csv,250,,\n"));
    result = library.importFile(source.filePath("targets.csv"), "Targets.csv", "trunkTargets");
    expect("target bundle imports", result.value("ok").toBool());
    f.targets = result.value("path").toString();
    f.targetsRoot = library.get(library.rowForPath(f.targets)).value("bundleRoot").toString();

    // A plain channel map later updated with companions keeps its file in imports/, beside its new bundle.
    expect("write plain channel map",
           write_file(source.filePath("plain.csv"), "channel,frequency_hz,mode\n1,461000000,dmr\n"));
    result = library.importBundle(source.filePath("plain.csv"), "Plain.csv", "chan", {});
    const int row = library.rowForPath(result.value("path").toString());
    result = library.importBundle(source.filePath("channels.csv"), "Plain.csv", "chan", {}, row);
    expect("plain channel map updated with companions", result.value("ok").toBool());
    f.updated = result.value("path").toString();
    f.updatedRoot = library.get(row).value("bundleRoot").toString();
    expect("updated map stays beside its bundle", !f.updatedRoot.isEmpty() && !f.updated.startsWith(f.updatedRoot + '/')
                                                      && f.updated.startsWith(dsd_qt::json_store_path("imports/")));
}

/** Systems, scan lists, the library, profiles, ranges, avoids and settings, all through the production models. */
Fixture
populate(TestHost& host) {
    Fixture f;
    dsd_qt::ImportedFilesModel library(&host);
    f.groups = import_path(library, example("group.csv"), "group.csv", "group");
    f.keys = import_path(library, example("multi_key_hex.csv"), "keys.csv", "keysHex");
    f.bandplan = import_path(library, example("p25_bandplan.csv"), "bandplan.csv", "p25Bandplan");
    f.src = import_path(library, example("src.csv"), "src.csv", "src");
    f.mapping = import_path(library, example("dmr_tg_key_map.csv"), "map.csv", "dmrTgKeys");
    import_bundles(library, f);

    dsd_qt::SavedSystemsModel systems;
    expect("add trunked system", systems.add({{"name", "Metro"},
                                              {"sourceType", "usb"},
                                              {"freqMhz", "851.0125"},
                                              {"decodeFlag", "-ft"},
                                              {"trunking", true},
                                              {"chanCsvPath", f.chan},
                                              {"groupCsvPath", f.groups},
                                              {"keyCsvPath", f.keys},
                                              {"keyCsvHex", true},
                                              {"p25BandplanCsvPath", f.bandplan},
                                              {"srcCsvPath", f.src},
                                              {"extraArgs", "-Z"}}));
    expect("add replay system", systems.add({{"name", "Replay"},
                                             {"sourceType", "file"},
                                             {"filePath", dsd_qt::json_store_path("recordings/capture.wav")}}));
    f.systemUid = systems.get(0).value("uid").toString();
    f.replayUid = systems.get(1).value("uid").toString();

    dsd_qt::ScanListsModel scans;
    const QVariantList entries{QVariantMap{{"kind", "system"}, {"systemUid", f.systemUid}},
                               QVariantMap{{"kind", "freq"}, {"freqMhz", "462.5625"}, {"protocol", "nfm"}}};
    expect("add scan list",
           scans.add({{"name", "Patrol"}, {"groupCsvPath", f.groups}, {"srcCsvPath", f.src}, {"entries", entries}}));
    expect("add target scan list",
           scans.add({{"name", "Targets"}, {"targetSource", "csv"}, {"targetsCsvPath", f.targets}}));

    dsd_qt::DecryptionProfilesModel profiles;
    profiles.setReferences(&systems, &scans);
    QVariantMap saved = profiles.saveProfile({{"uid", "csv-keys"},
                                              {"label", "CSV keys"},
                                              {"protocol", "p25"},
                                              {"mode", "automatic"},
                                              {"keySource", "csv"},
                                              {"legacyCsvPath", f.keys},
                                              {"legacyCsvHex", true}});
    expect("save CSV profile", saved.value("ok").toBool());
    saved = profiles.saveProfile({{"uid", "dmr-map"},
                                  {"label", "DMR map"},
                                  {"protocol", "dmr"},
                                  {"mode", "automatic"},
                                  {"keySource", "csv"},
                                  {"legacyCsvPath", f.keys},
                                  {"legacyCsvHex", true},
                                  {"mappingCsvPath", f.mapping}});
    expect("save mapped profile", saved.value("ok").toBool());

    dsd_qt::SignalSearchModel search(&host, nullptr);
    QVariantMap range = search.defaultRange();
    range.insert("name", "Marine");
    expect("save search range", search.saveRange(-1, range).isEmpty());
    dsd_app_signal_search_permanent_avoid(156800000U);
    dsd_app_signal_search_permanent_avoid(162550000U);

    expect("write portable settings",
           dsd_qt::AppPrefs::replacePortable(
               {{"decode/extraArgs", "-Z"}, {"rr/username", "scanner"}, {"tuner/gainDb", "30"}}));
    return f;
}

/** The archive split into its manifest and file bytes, so a case can forge one detail and reassemble it. */
struct Parts {
    QJsonObject manifest;
    QList<QByteArray> files;
};

Parts
split(const QByteArray& archive) {
    Parts parts;
    const quint32 length = qFromBigEndian<quint32>(archive.constData() + 4);
    parts.manifest = QJsonDocument::fromJson(archive.mid(8, static_cast<qsizetype>(length))).object();
    qsizetype offset = 8 + static_cast<qsizetype>(length);
    for (const auto& value : parts.manifest.value("files").toArray()) {
        const qsizetype size = static_cast<qsizetype>(value.toObject().value("size").toInteger());
        parts.files.append(archive.mid(offset, size));
        offset += size;
    }
    return parts;
}

QByteArray
assemble(const QJsonObject& manifest, const QList<QByteArray>& files) {
    const QByteArray json = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    QByteArray out("DNBA");
    char length[4];
    qToBigEndian<quint32>(static_cast<quint32>(json.size()), length);
    out.append(length, 4);
    out.append(json);
    for (const auto& file : files) {
        out.append(file);
    }
    return out;
}

QByteArray
assemble(const Parts& parts) {
    return assemble(parts.manifest, parts.files);
}

void
add_file(Parts& parts, const QString& path, const QByteArray& bytes = QByteArray()) {
    QJsonArray files = parts.manifest.value("files").toArray();
    files.append(QJsonObject{{"path", path}, {"size", bytes.size()}, {"sha256", sha256_hex(bytes)}});
    parts.manifest.insert("files", files);
    parts.files.append(bytes);
}

void
with_store(Parts& parts, const char* store, const std::function<void(QJsonArray&)>& edit) {
    QJsonObject stores = parts.manifest.value("stores").toObject();
    QJsonArray rows = stores.value(QLatin1String(store)).toArray();
    edit(rows);
    stores.insert(QLatin1String(store), rows);
    parts.manifest.insert("stores", stores);
}

void
with_row(Parts& parts, const char* store, int row, const std::function<void(QJsonObject&)>& edit) {
    with_store(parts, store, [&](QJsonArray& rows) {
        QJsonObject object = rows.at(row).toObject();
        edit(object);
        rows.replace(row, object);
    });
}

int
row_where(const QJsonArray& rows, const char* key, const QString& value) {
    for (int i = 0; i < rows.size(); ++i) {
        if (rows.at(i).toObject().value(QLatin1String(key)).toString() == value) {
            return i;
        }
    }
    return -1;
}

QString
all_stores_json(const QMap<QString, QJsonArray>& stores) {
    QByteArray text;
    for (auto it = stores.cbegin(); it != stores.cend(); ++it) {
        text += QJsonDocument(it.value()).toJson(QJsonDocument::Compact);
    }
    return QString::fromUtf8(text);
}

dsd_qt::backup::BuildResult
build(const dsd_qt::backup::Snapshot& snapshot) {
    auto result = dsd_qt::backup::backup_build_archive(snapshot, nullptr);
    if (result.error != BackupError::None) {
        DSD_FPRINTF(stderr, "build failed: %s\n", qUtf8Printable(result.detail));
    }
    return result;
}

void
refuse(const char* what, const QByteArray& archive, BackupError expected, const char* area) {
    const auto parsed = dsd_qt::backup::backup_parse_archive(archive, dsd_qt::json_store_root());
    const bool ok =
        parsed.error == expected && parsed.detail.startsWith(QLatin1String(area)) && parsed.stores.isEmpty();
    if (!ok) {
        DSD_FPRINTF(stderr, "  %s: got %s \"%s\"\n", what,
                    qUtf8Printable(dsd_qt::backup::backup_error_name(parsed.error)), qUtf8Printable(parsed.detail));
    }
    expect(what, ok);
}

void
test_json_store_root() {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    expect("json_store_root is the app data directory", dsd_qt::json_store_root() == root);
    expect("json_store_path builds on the root", dsd_qt::json_store_path("x/y.json") == root + "/x/y.json");
}

void
test_typed_path_table() {
    using dsd_qt::backup::PathKind;
    const QMap<QString, PathKind> want{{"saved_systems.json/chanCsvPath", PathKind::File},
                                       {"saved_systems.json/groupCsvPath", PathKind::File},
                                       {"saved_systems.json/keyCsvPath", PathKind::File},
                                       {"saved_systems.json/p25BandplanCsvPath", PathKind::File},
                                       {"saved_systems.json/srcCsvPath", PathKind::File},
                                       {"saved_systems.json/filePath", PathKind::Recording},
                                       {"scan_lists.json/groupCsvPath", PathKind::File},
                                       {"scan_lists.json/srcCsvPath", PathKind::File},
                                       {"scan_lists.json/targetsCsvPath", PathKind::File},
                                       {"imported_files.json/path", PathKind::File},
                                       {"imported_files.json/bundleRoot", PathKind::BundleRoot},
                                       {"decryption_profiles.json/legacyCsvPath", PathKind::File},
                                       {"decryption_profiles.json/mappingCsvPath", PathKind::File},
                                       {"decryption_profiles.json/vendorCsvPath", PathKind::File}};
    QMap<QString, PathKind> got;
    for (const auto& field : dsd_qt::backup::typed_path_fields()) {
        got.insert(QLatin1String(field.store) + '/' + QLatin1String(field.field), field.kind);
    }
    expect("typed path table is exactly the documented fields", got == want);
    expect("five stores in manifest order",
           dsd_qt::backup::backup_store_names()
               == QStringList({"saved_systems.json", "scan_lists.json", "imported_files.json",
                               "decryption_profiles.json", "signal-search-ranges.json"}));

    const QJsonArray rows{QJsonObject{{"name", "a"}, {"chanCsvPath", "/r/imports/a.csv"}, {"filePath", ""}},
                          QJsonValue(7), QJsonObject{{"groupCsvPath", 5}, {"keyCsvPath", "/r/imports/k.csv"}}};
    const auto values = dsd_qt::backup::typed_path_values("saved_systems.json", rows);
    expect("typed values skip empty, non-string and non-object entries",
           values.size() == 2 && values.at(0).row == 0 && QLatin1String(values.at(0).field.field) == "chanCsvPath"
               && values.at(1).row == 2 && values.at(1).value == "/r/imports/k.csv");
    const QJsonArray mapped = dsd_qt::backup::map_typed_path_values(
        "saved_systems.json", rows, [](const dsd_qt::backup::TypedPathValue& v) { return v.value.toUpper(); });
    expect("map rewrites typed values only", mapped.at(0).toObject().value("chanCsvPath") == "/R/IMPORTS/A.CSV"
                                                 && mapped.at(0).toObject().value("name") == "a"
                                                 && mapped.at(1) == QJsonValue(7)
                                                 && mapped.at(2).toObject().value("groupCsvPath") == 5
                                                 && mapped.at(2).toObject().value("keyCsvPath") == "/R/IMPORTS/K.CSV");
    expect("ranges have no typed paths",
           dsd_qt::backup::typed_path_values("signal-search-ranges.json", QJsonArray{QJsonObject{{"name", "/x"}}})
               .isEmpty());
}

QMap<QString, QJsonArray>
expected_after_export(const dsd_qt::backup::Snapshot& snapshot) {
    QMap<QString, QJsonArray> stores = snapshot.stores;
    QJsonArray profiles;
    for (const auto& value : stores.value("decryption_profiles.json")) {
        QJsonObject row = value.toObject();
        row.remove("compiled");
        row.remove("runtimeRef");
        row.remove("pastRuntimeRefs");
        profiles.append(row);
    }
    stores.insert("decryption_profiles.json", profiles);
    QJsonArray systems;
    for (const auto& value : stores.value("saved_systems.json")) {
        QJsonObject row = value.toObject();
        row.insert("filePath", "");
        systems.append(row);
    }
    stores.insert("saved_systems.json", systems);
    return stores;
}

void
check_round_trip_files(const dsd_qt::backup::ParseResult& parsed, const QByteArray& archive, const Fixture& f) {
    const QString root = dsd_qt::json_store_root();
    QSet<QString> paths;
    bool identical = !parsed.files.isEmpty();
    for (const auto& file : parsed.files) {
        paths.insert(root + '/' + file.path);
        const QByteArray bytes = archive.mid(file.offset, file.size);
        identical = identical && bytes == read_file(root + '/' + file.path) && sha256_hex(bytes) == file.sha256;
    }
    expect("carried file bytes are identical", identical);
    for (const QString& path : {f.groups, f.keys, f.bandplan, f.src, f.mapping, f.chan, f.targets, f.updated}) {
        expect("every library file is carried", paths.contains(path));
    }
    for (const QString& bundle : {f.chanRoot, f.targetsRoot, f.updatedRoot}) {
        QDirIterator it(bundle, QDir::Files, QDirIterator::Subdirectories);
        int seen = 0;
        while (it.hasNext()) {
            seen++;
            expect("every bundle file is carried", paths.contains(it.next()));
        }
        expect("bundles hold companions", seen >= 2);
    }
}

void
test_round_trip(const Fixture& f) {
    const auto snapshot = dsd_qt::backup::backup_take_snapshot();
    expect("snapshot root", snapshot.root == dsd_qt::json_store_root());
    expect("snapshot canonical root", snapshot.canonicalRoot == QFileInfo(snapshot.root).canonicalFilePath());
    expect("snapshot holds the five stores", snapshot.stores.keys().size() == 5);
    expect("snapshot portable settings", snapshot.prefs.value("rr/username").toString() == "scanner"
                                             && !snapshot.prefs.contains("ui/onboardingDone"));
    expect("snapshot avoids come from the in-memory list", snapshot.avoids == QList<quint32>({156800000U, 162550000U}));
    expect("snapshot app tag and time",
           snapshot.app == QString::fromUtf8(GIT_TAG) && snapshot.createdAt > 1700000000LL);

    const auto built = build(snapshot);
    expect("build succeeds", built.error == BackupError::None && built.archive.startsWith("DNBA"));
    const auto& s = built.summary;
    expect("export summary counts", s.systems == 2 && s.scanLists == 2 && s.libraryFiles == 8 && s.profiles == 2
                                        && s.ranges == 2 && s.avoids == 2);
    expect("export summary recordings and extra options",
           s.recordingsToChoose == 1 && s.systemsWithExtraArgs == 1 && s.appExtraArgs);
    expect("nothing left out of a clean export", s.notIncluded.isEmpty());

    int status = 0;
    const QByteArray sealed =
        dsd_qt::backup::backup_seal(built.archive, QStringLiteral("correct horse battery"), test_kdf(), &status);
    expect("seal succeeds", status == DSD_BACKUP_OK && sealed.startsWith("DSDNEOBK"));
    const QByteArray opened = dsd_qt::backup::backup_open(sealed, QStringLiteral("correct horse battery"), &status);
    expect("open returns the archive", status == DSD_BACKUP_OK && opened == built.archive);
    const QByteArray wrong = dsd_qt::backup::backup_open(sealed, QStringLiteral("correct horse batterY"), &status);
    expect("wrong passphrase is refused", wrong.isEmpty() && status == DSD_BACKUP_ERR_AUTH);

    const auto parsed = dsd_qt::backup::backup_parse_archive(opened, snapshot.root);
    expect("parse succeeds", parsed.error == BackupError::None);
    if (parsed.error != BackupError::None) {
        DSD_FPRINTF(stderr, "  parse: %s\n", qUtf8Printable(parsed.detail));
    }
    const auto want = expected_after_export(snapshot);
    for (const QString& store : dsd_qt::backup::backup_store_names()) {
        expect(qPrintable("store equal after rebase: " + store), parsed.stores.value(store) == want.value(store));
    }
    expect("profiles keep their definitions",
           parsed.stores.value("decryption_profiles.json").at(0).toObject().value("legacyCsvPath") == f.keys);
    expect("prefs equal", parsed.prefs == snapshot.prefs);
    expect("avoids equal", parsed.avoids == snapshot.avoids);
    check_round_trip_files(parsed, opened, f);
    const auto& p = parsed.summary;
    expect("restore summary counts", p.systems == 2 && p.scanLists == 2 && p.libraryFiles == 8 && p.profiles == 2
                                         && p.ranges == 2 && p.avoids == 2 && p.recordingsToChoose == 1
                                         && p.systemsWithExtraArgs == 1 && p.appExtraArgs);
    expect("restore summary origin", p.createdAt == snapshot.createdAt && p.app == snapshot.app);
    const QVariantMap map = dsd_qt::backup::summary_to_map(p);
    expect("summary map", map.value("systems").toInt() == 2 && map.value("appExtraArgs").toBool()
                              && map.value("app").toString() == snapshot.app && map.contains("notIncluded"));
}

void
test_foreign_root() {
    auto snapshot = dsd_qt::backup::backup_take_snapshot();
    // The production editor writes vendorCsvPath only for a vertex profile, whose CSV this suite does not carry;
    // give one row a library file there so every typed field takes part in the scan below.
    QJsonArray profiles = snapshot.stores.value("decryption_profiles.json");
    QJsonObject first = profiles.at(0).toObject();
    first.insert("vendorCsvPath", first.value("legacyCsvPath"));
    profiles.replace(0, first);
    snapshot.stores.insert("decryption_profiles.json", profiles);

    const auto built = build(snapshot);
    const QString target = QStringLiteral("/restore/elsewhere/files");
    const auto parsed = dsd_qt::backup::backup_parse_archive(built.archive, target);
    expect("foreign root parses", parsed.error == BackupError::None);
    QSet<QString> seen;
    bool rebased = true;
    for (auto it = parsed.stores.cbegin(); it != parsed.stores.cend(); ++it) {
        for (const auto& value : dsd_qt::backup::typed_path_values(it.key(), it.value())) {
            seen.insert(it.key() + '/' + QLatin1String(value.field.field));
            rebased = rebased && value.value.startsWith(target + "/imports/");
        }
    }
    expect("every typed value is rebased onto the target", rebased);
    int fields = 0;
    for (const auto& field : dsd_qt::backup::typed_path_fields()) {
        if (field.kind != dsd_qt::backup::PathKind::Recording) {
            fields++;
            expect(qPrintable(QStringLiteral("typed field exercised: %1/%2").arg(field.store, field.field)),
                   seen.contains(QLatin1String(field.store) + '/' + QLatin1String(field.field)));
        }
    }
    expect("all thirteen non-recording fields are exercised", fields == 13);
    const QString text = all_stores_json(parsed.stores);
    expect("no store names the source root", !text.contains(snapshot.root) && !text.contains(snapshot.canonicalRoot));
}

struct SanitizeSetup {
    QTemporaryDir outside;
    QString external, staged, deep, notOwned, absoluteMap;
};

void
prepare_sanitize(SanitizeSetup& setup, dsd_qt::backup::Snapshot& snapshot) {
    const QString root = snapshot.root;
    setup.external = setup.outside.filePath("external.csv");
    setup.staged = root + "/imports/.staged-abc.csv";
    setup.notOwned = root + "/imports/bundles/not owned";
    setup.absoluteMap = root + "/imports/absolute.csv";
    expect("write sanitize fixtures",
           write_file(setup.external, "DEC,Mode,Name,Tag\n1,A,x,y\n") && write_file(setup.staged, "x\n")
               && write_file(setup.notOwned + "/companion.csv", "x\n")
               && write_file(setup.absoluteMap, "channel,frequency_hz,mode,keys_hex_csv\n1,461000000,dmr,"
                                                    + setup.external.toUtf8() + "\n")
               && QFile::link(setup.external, root + "/imports/linked.csv"));

    // A symlink whose target is itself a carried import: still a symlink, still not carried.
    QJsonArray scanLists = snapshot.stores.value("scan_lists.json");
    QJsonObject patrol = scanLists.at(0).toObject();
    expect("link to an import",
           QFile::link(patrol.value("srcCsvPath").toString(), root + "/imports/internal-link.csv"));
    patrol.insert("srcCsvPath", root + "/imports/internal-link.csv");
    scanLists.replace(0, patrol);
    snapshot.stores.insert("scan_lists.json", scanLists);

    QJsonArray systems = snapshot.stores.value("saved_systems.json");
    QJsonObject metro = systems.at(0).toObject();
    metro.insert("chanCsvPath", root + "/imports/missing.csv");
    metro.insert("groupCsvPath", setup.external);
    metro.insert("keyCsvPath", root + "/imports/linked.csv");
    metro.insert("srcCsvPath", setup.staged);
    systems.replace(0, metro);
    snapshot.stores.insert("saved_systems.json", systems);

    QJsonArray library = snapshot.stores.value("imported_files.json");
    const int groupRow = row_where(library, "type", "group");
    QJsonObject groups = library.at(groupRow).toObject();
    groups.insert("bundleRoot", setup.notOwned);
    library.replace(groupRow, groups);
    QJsonObject gone = groups;
    gone.insert("path", root + "/imports/gone.csv");
    gone.insert("name", "gone.csv");
    gone.insert("bundleRoot", "");
    library.append(gone);
    QJsonObject absolute = groups;
    absolute.insert("path", setup.absoluteMap);
    absolute.insert("name", "absolute.csv");
    absolute.insert("type", "chan");
    absolute.insert("bundleRoot", "");
    library.append(absolute);
    snapshot.stores.insert("imported_files.json", library);
}

bool
lists(const QStringList& notIncluded, const QString& needle) {
    for (const auto& line : notIncluded) {
        if (line.contains(needle)) {
            return true;
        }
    }
    return false;
}

void
test_sanitize() {
    SanitizeSetup setup;
    auto snapshot = dsd_qt::backup::backup_take_snapshot();
    prepare_sanitize(setup, snapshot);
    const auto built = build(snapshot);
    expect("sanitized export succeeds", built.error == BackupError::None);
    const auto& missing = built.summary.notIncluded;
    expect("missing file listed", lists(missing, "Metro: missing.csv"));
    expect("external file listed", lists(missing, "Metro: external.csv"));
    expect("symlinked file listed", lists(missing, "Metro: linked.csv"));
    expect("staging leftover listed", lists(missing, "Metro: .staged-abc.csv"));
    expect("symlink to an import listed", lists(missing, "Patrol: internal-link.csv"));
    expect("dropped library row listed", lists(missing, "gone.csv"));
    expect("non-owned bundle listed", lists(missing, "group.csv: companion files"));
    expect("absolute channel-map reference listed", lists(missing, "absolute.csv: external.csv"));
    expect("replay recording counted", built.summary.recordingsToChoose == 1);
    expect("library keeps the rows it carries", built.summary.libraryFiles == 9);

    const auto parsed = dsd_qt::backup::backup_parse_archive(built.archive, snapshot.root);
    expect("sanitized backup passes restore validation", parsed.error == BackupError::None);
    const QJsonObject metro = parsed.stores.value("saved_systems.json").at(0).toObject();
    expect("uncarried references are cleared", metro.value("chanCsvPath") == "" && metro.value("groupCsvPath") == ""
                                                   && metro.value("keyCsvPath") == ""
                                                   && metro.value("srcCsvPath") == "");
    expect("carried references survive", !metro.value("p25BandplanCsvPath").toString().isEmpty());
    const QJsonArray library = parsed.stores.value("imported_files.json");
    expect("missing library row dropped", row_where(library, "name", "gone.csv") < 0);
    const QJsonObject groups = library.at(row_where(library, "type", "group")).toObject();
    expect("non-owned bundle root cleared, file carried alone",
           groups.value("bundleRoot") == "" && !groups.value("path").toString().isEmpty());
    bool bundleLeaked = false;
    for (const auto& file : parsed.files) {
        bundleLeaked = bundleLeaked || file.path.contains("not owned") || file.path.contains(".staged-");
    }
    expect("non-owned bundle and staging files are not carried", !bundleLeaked);
    expect("recording cleared", parsed.stores.value("saved_systems.json").at(1).toObject().value("filePath") == "");
    expect("symlink to an import cleared",
           parsed.stores.value("scan_lists.json").at(0).toObject().value("srcCsvPath") == "");
    QFile::remove(snapshot.root + "/imports/linked.csv");
    QFile::remove(snapshot.root + "/imports/internal-link.csv");
    QFile::remove(setup.staged);
    QFile::remove(setup.absoluteMap);
    QDir(setup.notOwned).removeRecursively();
}

/* Stores a model would repair or prune as it loads them: the export repairs and prunes the same way, so the backup
 * still passes its own validation instead of refusing to export. */
void
test_export_repairs() {
    auto snapshot = dsd_qt::backup::backup_take_snapshot();
    QJsonArray systems = snapshot.stores.value("saved_systems.json");
    const QString firstUid = systems.at(0).toObject().value("uid").toString();
    QJsonObject braced = systems.at(0).toObject();
    braced.insert("uid", '{' + firstUid + '}');
    systems.replace(0, braced);
    QJsonObject twin = systems.at(1).toObject();
    twin.insert("uid", firstUid);
    systems.replace(1, twin);
    systems.append(7);
    snapshot.stores.insert("saved_systems.json", systems);

    QJsonArray scanLists = snapshot.stores.value("scan_lists.json");
    QJsonObject patrol = scanLists.at(0).toObject();
    QJsonArray entries = patrol.value("entries").toArray();
    QJsonObject second = entries.at(1).toObject();
    second.insert("uid", entries.at(0).toObject().value("uid"));
    entries.replace(1, second);
    entries.append("junk");
    patrol.insert("entries", entries);
    scanLists.replace(0, patrol);
    QJsonObject copy = scanLists.at(1).toObject();
    copy.insert("uid", patrol.value("uid"));
    scanLists.replace(1, copy);
    snapshot.stores.insert("scan_lists.json", scanLists);

    QJsonArray profiles = snapshot.stores.value("decryption_profiles.json");
    QJsonObject escaping = profiles.at(1).toObject();
    escaping.insert("uid", "../escape");
    profiles.append(escaping);
    profiles.append(profiles.at(0));
    snapshot.stores.insert("decryption_profiles.json", profiles);

    QJsonArray ranges = snapshot.stores.value("signal-search-ranges.json");
    QJsonObject broken = ranges.at(0).toObject();
    broken.insert("startMHz", "abc");
    ranges.append(broken);
    snapshot.stores.insert("signal-search-ranges.json", ranges);

    QJsonArray library = snapshot.stores.value("imported_files.json");
    QJsonObject unknown = library.at(0).toObject();
    unknown.insert("type", "exe");
    unknown.insert("name", "unknown-type.csv");
    library.append(unknown);
    library.append(library.at(0));
    // A row whose file sits in one bundle directory while it names another as its bundle keeps neither bundle.
    const QString stray = snapshot.root + "/imports/bundles/stray-bundle/x.csv";
    const QString other = snapshot.root + "/imports/bundles/other-bundle";
    expect("write stray bundle fixtures",
           write_file(stray, "DEC,Mode,Name,Tag\n1,A,x,y\n") && write_file(other + "/0.csv", "x\n"));
    QJsonObject mismatched = library.at(row_where(library, "type", "group")).toObject();
    mismatched.insert("path", stray);
    mismatched.insert("name", "mismatched.csv");
    mismatched.insert("bundleRoot", other);
    library.append(mismatched);
    snapshot.stores.insert("imported_files.json", library);
    snapshot.prefs.insert("tuner/ppm", QVariantList{1, 2});

    const auto built = build(snapshot);
    expect("repairable stores export", built.error == BackupError::None);
    const auto parsed = dsd_qt::backup::backup_parse_archive(built.archive, snapshot.root);
    expect("repaired export parses", parsed.error == BackupError::None);
    const QJsonArray outSystems = parsed.stores.value("saved_systems.json");
    expect("systems keep every object row", outSystems.size() == 2);
    expect("a braced uid is stored brace-less", outSystems.at(0).toObject().value("uid") == firstUid);
    expect("a repeated system uid is replaced",
           outSystems.at(1).toObject().value("uid") != firstUid
               && !QUuid(outSystems.at(1).toObject().value("uid").toString()).isNull());
    const QJsonArray outLists = parsed.stores.value("scan_lists.json");
    expect("a repeated scan list uid is replaced",
           outLists.size() == 2 && outLists.at(0).toObject().value("uid") != outLists.at(1).toObject().value("uid"));
    const QJsonArray outEntries = outLists.at(0).toObject().value("entries").toArray();
    expect("scan entries repaired",
           outEntries.size() == 2
               && outEntries.at(0).toObject().value("uid") != outEntries.at(1).toObject().value("uid"));
    expect("profiles the loader drops are dropped", parsed.stores.value("decryption_profiles.json").size() == 2);
    expect("ranges the loader drops are dropped", parsed.stores.value("signal-search-ranges.json").size() == 2);
    expect("an unknown library type is left out",
           built.summary.libraryFiles == 9 && lists(built.summary.notIncluded, "unknown-type.csv"));
    expect("a non-scalar setting is left out", !parsed.prefs.contains("tuner/ppm"));
    const QJsonArray outLibrary = parsed.stores.value("imported_files.json");
    const int mismatchedRow = row_where(outLibrary, "name", "mismatched.csv");
    expect("a file outside its own bundle is carried without one",
           mismatchedRow >= 0 && outLibrary.at(mismatchedRow).toObject().value("bundleRoot") == ""
               && lists(built.summary.notIncluded, "mismatched.csv: companion files"));
    QDir(snapshot.root + "/imports/bundles/stray-bundle").removeRecursively();
    QDir(other).removeRecursively();
}

/* A row whose file lies in another row's bundle is dropped; the bundle it names of its own is not carried either, since
 * no kept row would claim it and a restore refuses files nothing claims. */
void
test_export_drops_foreign_bundle(const Fixture& f) {
    auto snapshot = dsd_qt::backup::backup_take_snapshot();
    const QString own = snapshot.root + "/imports/bundles/foreign-bundle";
    expect("write the foreign row's bundle", write_file(own + "/0.csv", "x\n"));
    QString companion;
    QDirIterator it(f.chanRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        companion = path != f.chan ? path : companion;
    }
    QJsonArray library = snapshot.stores.value("imported_files.json");
    QJsonObject foreign = library.at(row_where(library, "type", "group")).toObject();
    foreign.insert("path", companion);
    foreign.insert("name", "foreign.csv");
    foreign.insert("bundleRoot", own);
    library.append(foreign);
    snapshot.stores.insert("imported_files.json", library);

    const auto built = build(snapshot);
    const auto parsed = dsd_qt::backup::backup_parse_archive(built.archive, snapshot.root);
    expect("export with a foreign row succeeds", built.error == BackupError::None && parsed.error == BackupError::None);
    expect("the foreign row is dropped and listed",
           row_where(parsed.stores.value("imported_files.json"), "name", "foreign.csv") < 0
               && lists(built.summary.notIncluded, "Imported file foreign.csv"));
    bool carried = false;
    for (const auto& file : parsed.files) {
        carried = carried || file.path.startsWith("imports/bundles/foreign-bundle/");
    }
    expect("the dropped row's own bundle is not carried", !carried);
    QDir(own).removeRecursively();
}

/* The export answers the same folded question: a row whose file sits in a case variant of another row's bundle is
 * dropped, and a row whose file sits in a case variant of its own bundle keeps no bundle. */
void
test_export_folded_bundle_paths(const Fixture& f) {
    const QString id = f.chanRoot.mid(f.chanRoot.lastIndexOf('/') + 1);
    auto snapshot = dsd_qt::backup::backup_take_snapshot();
    const QString variant = snapshot.root + "/imports/Bundles/" + id + "/x.csv";
    expect("write a file under a case variant of a bundle", write_file(variant, "DEC,Mode,Name,Tag\n1,A,x,y\n"));

    QJsonArray library = snapshot.stores.value("imported_files.json");
    QJsonObject beside = library.at(row_where(library, "type", "group")).toObject();
    beside.insert("path", variant);
    beside.insert("name", "variant.csv");
    beside.insert("bundleRoot", "");
    library.append(beside);
    auto besideSnapshot = snapshot;
    besideSnapshot.stores.insert("imported_files.json", library);
    auto built = build(besideSnapshot);
    auto parsed = dsd_qt::backup::backup_parse_archive(built.archive, snapshot.root);
    expect("export with a row beside a folded bundle succeeds",
           built.error == BackupError::None && parsed.error == BackupError::None);
    expect("a row in a case variant of another row's bundle is dropped",
           row_where(parsed.stores.value("imported_files.json"), "name", "variant.csv") < 0
               && lists(built.summary.notIncluded, "Imported file variant.csv"));

    library = snapshot.stores.value("imported_files.json");
    const int chanRow = row_where(library, "path", f.chan);
    QJsonObject chan = library.at(chanRow).toObject();
    const QString chanName = chan.value("name").toString();
    chan.insert("path", variant);
    library.replace(chanRow, chan);
    auto ownSnapshot = snapshot;
    ownSnapshot.stores.insert("imported_files.json", library);
    built = build(ownSnapshot);
    parsed = dsd_qt::backup::backup_parse_archive(built.archive, snapshot.root);
    expect("export with a file under a case variant of its own bundle succeeds",
           built.error == BackupError::None && parsed.error == BackupError::None);
    const QJsonArray outLibrary = parsed.stores.value("imported_files.json");
    const int outRow = row_where(outLibrary, "path", variant);
    expect("that row keeps no bundle", outRow >= 0 && outLibrary.at(outRow).toObject().value("bundleRoot") == ""
                                           && lists(built.summary.notIncluded, chanName + ": companion files"));
    QDir(snapshot.root + "/imports/Bundles").removeRecursively();
}

void
test_build_caps_and_cancel() {
    auto snapshot = dsd_qt::backup::backup_take_snapshot();
    std::atomic<bool> cancel{true};
    const auto cancelled = dsd_qt::backup::backup_build_archive(snapshot, &cancel);
    expect("cancelled build stops", cancelled.cancelled && cancelled.archive.isEmpty());

    QJsonArray ranges;
    for (int i = 0; i <= dsd_qt::backup::kMaxRowsPerStore; ++i) {
        ranges.append(snapshot.stores.value("signal-search-ranges.json").at(0));
    }
    auto many = snapshot;
    many.stores.insert("signal-search-ranges.json", ranges);
    expect("too many rows is too large",
           dsd_qt::backup::backup_build_archive(many, nullptr).error == BackupError::TooLarge);

    const QString big = snapshot.root + "/imports/big.csv";
    QFile file(big);
    expect("create oversized file", file.open(QIODevice::WriteOnly) && file.resize(dsd_qt::backup::kMaxFileBytes + 1));
    file.close();
    auto oversized = snapshot;
    QJsonArray systems = oversized.stores.value("saved_systems.json");
    QJsonObject metro = systems.at(0).toObject();
    metro.insert("groupCsvPath", big);
    systems.replace(0, metro);
    oversized.stores.insert("saved_systems.json", systems);
    expect("oversized file is too large",
           dsd_qt::backup::backup_build_archive(oversized, nullptr).error == BackupError::TooLarge);
    QFile::remove(big);
}

void
test_strip_compiled(const QByteArray& archive) {
    Parts parts = split(archive);
    with_row(parts, "decryption_profiles.json", 0, [](QJsonObject& row) {
        row.insert("compiled", QJsonObject{{"decryptionVendorArgs", QJsonArray{"-5", "forged"}}});
        row.insert("runtimeRef", "forged-reference");
        row.insert("pastRuntimeRefs", QJsonArray{"older"});
    });
    with_row(parts, "decryption_profiles.json", 1, [](QJsonObject& row) { row.insert("compiled", QJsonObject()); });
    const auto parsed = dsd_qt::backup::backup_parse_archive(assemble(parts), dsd_qt::json_store_root());
    expect("forged compiled archive parses", parsed.error == BackupError::None);
    bool stripped = true;
    for (const auto& value : parsed.stores.value("decryption_profiles.json")) {
        const QJsonObject row = value.toObject();
        stripped =
            stripped && !row.contains("compiled") && !row.contains("runtimeRef") && !row.contains("pastRuntimeRefs");
    }
    expect("restored profiles carry no compiled form or runtime references", stripped);

    // Loaded as restored, the profiles compile from their definitions; the forged vendor arguments never reach argv.
    const QString store = dsd_qt::json_store_path("decryption_profiles.json");
    const QByteArray original = read_file(store);
    expect("write restored profiles",
           dsd_qt::json_store_save_array("decryption_profiles.json", parsed.stores.value("decryption_profiles.json")));
    {
        dsd_qt::DecryptionProfilesModel loaded;
        QString error;
        const QVariantMap first = loaded.configuration("csv-keys", &error);
        expect("restored profile recompiles", error.isEmpty() && !first.value("keysHexCsvPath").toString().isEmpty());
        expect("forged vendor arguments are gone", !first.contains("decryptionVendorArgs"));
        const QVariantMap second = loaded.configuration("dmr-map", &error);
        expect("empty compiled map does not survive", error.isEmpty() && second.contains("dmrTgKeyCsvPath"));
    }
    expect("restore original profiles", write_file(store, original));
}

void
refuse_archive_layout(const QByteArray& archive) {
    for (const char* target : {"", "relative/files", "/data/files/", "/data/../files"}) {
        const auto parsed = dsd_qt::backup::backup_parse_archive(archive, QLatin1String(target));
        expect("a malformed target root is refused",
               parsed.error == BackupError::Io && parsed.detail.startsWith("target:") && parsed.stores.isEmpty());
    }
    refuse("short archive", "DNB", BackupError::Damaged, "archive:");
    QByteArray magic = archive;
    magic[0] = 'X';
    refuse("bad magic", magic, BackupError::Damaged, "archive:");
    refuse("manifest past the end", archive.left(12), BackupError::Damaged, "archive:");
    QByteArray huge("DNBA");
    char length[4];
    qToBigEndian<quint32>(static_cast<quint32>(dsd_qt::backup::kMaxManifestBytes + 1), length);
    huge.append(length, 4);
    huge.append(QByteArray(static_cast<qsizetype>(dsd_qt::backup::kMaxManifestBytes + 1), ' '));
    refuse("oversized manifest", huge, BackupError::TooLarge, "manifest:");
    const Parts parts = split(archive);
    refuse("manifest is not JSON", assemble(QJsonObject(), {}).replace("{}", "{x"), BackupError::Damaged, "manifest:");
    Parts forged = parts;
    forged.manifest.insert("format", "other-backup");
    refuse("foreign format", assemble(forged), BackupError::NotBackup, "manifest:");
    forged = parts;
    forged.manifest.insert("version", 2);
    refuse("newer version", assemble(forged), BackupError::NewerVersion, "manifest:");
    forged.manifest.insert("version", 0);
    refuse("version zero", assemble(forged), BackupError::Damaged, "manifest:");
    forged.manifest.insert("version", "1");
    refuse("string version", assemble(forged), BackupError::Damaged, "manifest:");
    forged = parts;
    forged.manifest.insert("sourceRoots", QJsonArray{"files"});
    refuse("relative source root", assemble(forged), BackupError::Damaged, "manifest:");
    forged.manifest.insert("sourceRoots", QJsonArray{"/"});
    refuse("filesystem root as source root", assemble(forged), BackupError::Damaged, "manifest:");
    forged.manifest.remove("sourceRoots");
    refuse("missing source roots", assemble(forged), BackupError::Damaged, "manifest:");
    forged = parts;
    forged.manifest.insert("createdAt", "yesterday");
    refuse("non-numeric creation time", assemble(forged), BackupError::Damaged, "manifest:");
    refuse("trailing byte", archive + 'x', BackupError::Damaged, "files:");
    refuse("truncated file bytes", archive.left(archive.size() - 1), BackupError::Damaged, "files:");
    QByteArray flipped = archive;
    flipped[flipped.size() - 1] = static_cast<char>(flipped.at(flipped.size() - 1) ^ 1);
    refuse("file hash mismatch", flipped, BackupError::Damaged, "files:");
}

void
refuse_file_table(const QByteArray& archive) {
    const Parts parts = split(archive);
    Parts many = parts;
    for (int i = static_cast<int>(parts.files.size()); i <= dsd_qt::backup::kMaxFiles; ++i) {
        add_file(many, QStringLiteral("imports/many/%1.csv").arg(i));
    }
    refuse("too many files", assemble(many), BackupError::TooLarge, "files:");
    Parts big = parts;
    QJsonArray files = big.manifest.value("files").toArray();
    QJsonObject first = files.at(0).toObject();
    first.insert("size", static_cast<double>(dsd_qt::backup::kMaxFileBytes + 1));
    files.replace(0, first);
    big.manifest.insert("files", files);
    refuse("oversized file", assemble(big), BackupError::TooLarge, "files:");
    Parts badHash = parts;
    files = badHash.manifest.value("files").toArray();
    first = files.at(0).toObject();
    first.insert("sha256", "ABC");
    files.replace(0, first);
    badHash.manifest.insert("files", files);
    refuse("malformed hash", assemble(badHash), BackupError::Damaged, "files:");

    const QList<QPair<const char*, QStringList>> paths{
        {"absolute path", {"/imports/a.csv"}},
        {"dot-dot component", {"imports/../a.csv"}},
        {"dot component", {"imports/./a.csv"}},
        {"empty component", {"imports//a.csv"}},
        {"empty path", {""}},
        {"backslash", {"imports\\a.csv"}},
        {"NUL", {QStringLiteral("imports/a") + QChar(0) + QStringLiteral(".csv")}},
        {"control character", {QStringLiteral("imports/a\x01.csv")}},
        {"too deep", {"imports/1/2/3/4/5/6.csv"}},
        {"outside imports", {"decryption/a.csv"}},
        {"imports itself", {"imports"}},
        {"staging leftover", {"imports/bundles/x/.staged-1.csv"}},
        {"exact duplicate", {"imports/dup.csv", "imports/dup.csv"}},
        {"case duplicate", {"imports/Dup.csv", "imports/dUP.csv"}},
        {"NFC duplicate",
         {QStringLiteral("imports/caf") + QChar(0x00E9) + ".csv",
          QStringLiteral("imports/cafe") + QChar(0x0301) + ".csv"}},
        {"file and directory", {"imports/x", "imports/x/y.csv"}},
        {"directory and file", {"imports/X/y.csv", "imports/x"}},
    };
    for (const auto& item : paths) {
        Parts forged = parts;
        for (const QString& path : item.second) {
            add_file(forged, path);
        }
        refuse(item.first, assemble(forged), BackupError::Damaged, "path:");
    }
}

void
refuse_store_shapes(const QByteArray& archive) {
    const Parts parts = split(archive);
    Parts forged = parts;
    QJsonObject stores = forged.manifest.value("stores").toObject();
    stores.insert("scan_lists.json", QJsonObject());
    forged.manifest.insert("stores", stores);
    refuse("store is not an array", assemble(forged), BackupError::Damaged, "store:");
    forged = parts;
    with_store(forged, "scan_lists.json", [](QJsonArray& rows) { rows.append(5); });
    refuse("row is not an object", assemble(forged), BackupError::Damaged, "store:");
    forged = parts;
    with_store(forged, "signal-search-ranges.json", [](QJsonArray& rows) {
        const QJsonValue row = rows.at(0);
        while (rows.size() <= dsd_qt::backup::kMaxRowsPerStore) {
            rows.append(row);
        }
    });
    refuse("too many rows", assemble(forged), BackupError::TooLarge, "store:");

    const auto setRow = [&](const char* store, int row, const char* key, const QJsonValue& value) {
        Parts edited = parts;
        with_row(edited, store, row, [&](QJsonObject& object) { object.insert(QLatin1String(key), value); });
        return assemble(edited);
    };
    const QString firstSystem = parts.manifest.value("stores")
                                    .toObject()
                                    .value("saved_systems.json")
                                    .toArray()
                                    .at(0)
                                    .toObject()
                                    .value("uid")
                                    .toString();
    refuse("system uid not a UUID", setRow("saved_systems.json", 0, "uid", "not-a-uuid"), BackupError::Damaged,
           "store:");
    refuse("system uid braced", setRow("saved_systems.json", 0, "uid", '{' + firstSystem + '}'), BackupError::Damaged,
           "store:");
    refuse("system uid missing", setRow("saved_systems.json", 0, "uid", QJsonValue()), BackupError::Damaged, "store:");
    refuse("duplicate system uid", setRow("saved_systems.json", 1, "uid", firstSystem), BackupError::Damaged, "store:");
    refuse("profile uid escapes its directory", setRow("decryption_profiles.json", 0, "uid", "../x"),
           BackupError::Damaged, "store:");
    refuse("duplicate profile uid", setRow("decryption_profiles.json", 1, "uid", "csv-keys"), BackupError::Damaged,
           "store:");
    const QString firstList = parts.manifest.value("stores")
                                  .toObject()
                                  .value("scan_lists.json")
                                  .toArray()
                                  .at(0)
                                  .toObject()
                                  .value("uid")
                                  .toString();
    refuse("duplicate scan list uid", setRow("scan_lists.json", 1, "uid", firstList), BackupError::Damaged, "store:");
    refuse("empty scan list uid", setRow("scan_lists.json", 0, "uid", ""), BackupError::Damaged, "store:");
    forged = parts;
    with_row(forged, "scan_lists.json", 0, [](QJsonObject& row) {
        QJsonArray entries = row.value("entries").toArray();
        QJsonObject second = entries.at(1).toObject();
        second.insert("uid", entries.at(0).toObject().value("uid"));
        entries.replace(1, second);
        row.insert("entries", entries);
    });
    refuse("duplicate scan entry uid", assemble(forged), BackupError::Damaged, "store:");
    refuse("scan entries not an array", setRow("scan_lists.json", 0, "entries", 3), BackupError::Damaged, "store:");
    refuse("unknown library type", setRow("imported_files.json", 0, "type", "exe"), BackupError::Damaged, "store:");
    forged = parts;
    with_store(forged, "imported_files.json", [](QJsonArray& rows) { rows.append(rows.at(0)); });
    refuse("duplicate library path", assemble(forged), BackupError::Damaged, "store:");
    refuse("range out of shape", setRow("signal-search-ranges.json", 0, "startMHz", "abc"), BackupError::Damaged,
           "store:");
}

void
refuse_prefs_and_avoids(const QByteArray& archive) {
    const Parts parts = split(archive);
    Parts forged = parts;
    forged.manifest.insert("prefs", QJsonArray());
    refuse("prefs not an object", assemble(forged), BackupError::Damaged, "prefs:");
    forged = parts;
    QJsonObject prefs = forged.manifest.value("prefs").toObject();
    prefs.insert("tuner/gainDb", QJsonArray{1});
    forged.manifest.insert("prefs", prefs);
    refuse("non-scalar setting", assemble(forged), BackupError::Damaged, "prefs:");
    forged = parts;
    prefs = forged.manifest.value("prefs").toObject();
    prefs.insert("ui/onboardingDone", true);
    prefs.insert("evil/key", QJsonArray{1});
    forged.manifest.insert("prefs", prefs);
    const auto parsed = dsd_qt::backup::backup_parse_archive(assemble(forged), dsd_qt::json_store_root());
    expect("unknown and device-local settings are dropped",
           parsed.error == BackupError::None && !parsed.prefs.contains("ui/onboardingDone")
               && !parsed.prefs.contains("evil/key") && parsed.prefs.value("rr/username") == "scanner");

    for (const auto& value : {QJsonValue(0), QJsonValue(-1), QJsonValue(4294967296.0), QJsonValue(1.5),
                              QJsonValue("156800000"), QJsonValue(QJsonArray())}) {
        forged = parts;
        forged.manifest.insert("avoids", QJsonArray{value});
        refuse("bad avoid frequency", assemble(forged), BackupError::Damaged, "avoids:");
    }
    QJsonArray many;
    for (int i = 1; i <= 1025; ++i) {
        many.append(i);
    }
    forged = parts;
    forged.manifest.insert("avoids", many);
    refuse("too many avoids", assemble(forged), BackupError::Damaged, "avoids:");
    forged.manifest.insert("avoids", QJsonArray{4294967295.0});
    expect("largest avoid frequency is kept",
           dsd_qt::backup::backup_parse_archive(assemble(forged), dsd_qt::json_store_root()).avoids
               == QList<quint32>({4294967295U}));
}

QByteArray
set_row(const Parts& parts, const char* store, int row, const char* key, const QJsonValue& value) {
    Parts edited = parts;
    with_row(edited, store, row, [&](QJsonObject& object) { object.insert(QLatin1String(key), value); });
    return assemble(edited);
}

/* Whether a file lies in a bundle is a clash question, answered on folded paths: imports/Bundles/<id>/ and
 * imports/bundleſ/<id>/ are imports/bundles/<id>/ on case-insensitive storage. */
void
refuse_folded_bundle_paths(const Parts& parts, const Fixture& f) {
    const QString root = dsd_qt::json_store_root();
    const QJsonArray library = parts.manifest.value("stores").toObject().value("imported_files.json").toArray();
    const int chanRow = row_where(library, "path", f.chan);
    const int groupRow = row_where(library, "path", f.groups);
    const QString id = f.chanRoot.mid(f.chanRoot.lastIndexOf('/') + 1);
    for (const QString& variant :
         {QStringLiteral("imports/Bundles/"), QStringLiteral("imports/bundle") + QChar(0x017F) + QLatin1Char('/')}) {
        const QString file = variant + id + "/x.csv";
        Parts beside = parts;
        add_file(beside, file, "DEC,Mode,Name,Tag\n1,A,x,y\n");
        with_row(beside, "imported_files.json", groupRow,
                 [&](QJsonObject& row) { row.insert("path", root + '/' + file); });
        refuse(qPrintable("row file in a folded variant of another row's bundle: " + variant), assemble(beside),
               BackupError::Damaged, "typed: a library file lies in another row's bundle");
    }
    // A row's own file under a case variant of its own root is outside that root on case-sensitive storage.
    const QString file = "imports/Bundles/" + id + "/x.csv";
    Parts own = parts;
    add_file(own, file, "x\n");
    with_row(own, "imported_files.json", chanRow, [&](QJsonObject& row) { row.insert("path", root + '/' + file); });
    refuse("own bundled file under a case variant of its root", assemble(own), BackupError::Damaged,
           "typed: a bundled file lies outside its row's bundle");
}

void
refuse_bundle_roots(const Parts& parts, const Fixture& f) {
    const QString root = dsd_qt::json_store_root();
    const QJsonArray library = parts.manifest.value("stores").toObject().value("imported_files.json").toArray();
    const int chanRow = row_where(library, "path", f.chan);
    const int groupRow = row_where(library, "path", f.groups);
    const int bandplanRow = row_where(library, "path", f.bandplan);
    const char* notOwned = "typed: a bundle root is not an owned bundle directory";
    refuse("bundle root is imports/bundles",
           set_row(parts, "imported_files.json", chanRow, "bundleRoot", root + "/imports/bundles"),
           BackupError::Damaged, notOwned);
    refuse("bundle root is imports", set_row(parts, "imported_files.json", chanRow, "bundleRoot", root + "/imports"),
           BackupError::Damaged, notOwned);
    refuse("bundle root is the data root", set_row(parts, "imported_files.json", chanRow, "bundleRoot", root),
           BackupError::Damaged, notOwned);
    refuse("bundle root name not owned",
           set_row(parts, "imported_files.json", chanRow, "bundleRoot", root + "/imports/bundles/not owned"),
           BackupError::Damaged, notOwned);
    // Roots that hold carried files and overlap no other row: only the owned-directory rule refuses them.
    Parts badName = parts;
    add_file(badName, "imports/bundles/bad name/0.csv", "x\n");
    with_row(badName, "imported_files.json", groupRow,
             [&](QJsonObject& row) { row.insert("bundleRoot", root + "/imports/bundles/bad name"); });
    refuse("bundle root with a non-owned name", assemble(badName), BackupError::Damaged, notOwned);
    Parts outside = parts;
    add_file(outside, "imports/sub/0.csv", "x\n");
    with_row(outside, "imported_files.json", groupRow,
             [&](QJsonObject& row) { row.insert("bundleRoot", root + "/imports/sub"); });
    refuse("bundle root outside imports/bundles", assemble(outside), BackupError::Damaged, notOwned);
    refuse("bundle root without carried files",
           set_row(parts, "imported_files.json", groupRow, "bundleRoot", root + "/imports/bundles/empty-root"),
           BackupError::Damaged, "typed: a bundle root holds no carried file");
    // On case-sensitive storage imports/bundles/ABC is not the directory holding imports/bundles/abc/0.csv.
    Parts caseRoot = parts;
    add_file(caseRoot, "imports/bundles/abc/0.csv", "x\n");
    with_row(caseRoot, "imported_files.json", groupRow,
             [&](QJsonObject& row) { row.insert("bundleRoot", root + "/imports/bundles/ABC"); });
    refuse("bundle root differing in case from its files", assemble(caseRoot), BackupError::Damaged,
           "typed: a bundle root holds no carried file");
    Parts spare = parts;
    add_file(spare, "imports/bundles/spare/0.csv", "x\n");
    with_row(spare, "imported_files.json", chanRow,
             [&](QJsonObject& row) { row.insert("bundleRoot", root + "/imports/bundles/spare"); });
    refuse("bundle root not holding its row's bundled file", assemble(spare), BackupError::Damaged,
           "typed: a bundled file lies outside its row's bundle");
    Parts overlap = parts;
    with_row(overlap, "imported_files.json", groupRow, [&](QJsonObject& row) { row.insert("bundleRoot", f.chanRoot); });
    refuse("two rows share a bundle root", assemble(overlap), BackupError::Damaged,
           "typed: two library rows share a bundle root");
    // Two roots that differ only by case hold distinct files, yet a case-insensitive filesystem merges them.
    Parts caseTwins = parts;
    add_file(caseTwins, "imports/bundles/caseroot/0.csv", "x\n");
    add_file(caseTwins, "imports/bundles/CASEROOT/1.csv", "y\n");
    with_row(caseTwins, "imported_files.json", groupRow,
             [&](QJsonObject& row) { row.insert("bundleRoot", root + "/imports/bundles/caseroot"); });
    with_row(caseTwins, "imported_files.json", bandplanRow,
             [&](QJsonObject& row) { row.insert("bundleRoot", root + "/imports/bundles/CASEROOT"); });
    refuse("two rows' bundle roots differ only by case", assemble(caseTwins), BackupError::Damaged,
           "typed: two library rows share a bundle root");
    // A row without a bundle of its own whose file lies in another row's bundle: removing that row would delete it.
    QString companion;
    for (const auto& value : parts.manifest.value("files").toArray()) {
        const QString path = root + '/' + value.toObject().value("path").toString();
        if (path.startsWith(f.chanRoot + '/') && path != f.chan) {
            companion = path;
        }
    }
    expect("channel bundle carries a companion", !companion.isEmpty());
    refuse("row file inside another row's bundle", set_row(parts, "imported_files.json", groupRow, "path", companion),
           BackupError::Damaged, "typed: a library file lies in another row's bundle");
    refuse_folded_bundle_paths(parts, f);
}

/* Every carried file must be one a later restore counts as configuration and replaces: named exactly by a path field
 * or inside a claimed bundle, as a case-sensitive filesystem lays it out. */
void
refuse_uncovered_files(const Parts& parts, const Fixture& f) {
    const QString root = dsd_qt::json_store_root();
    const char* uncovered = "typed: a carried file is named by no path field and lies in no bundle";
    Parts extra = parts;
    add_file(extra, "imports/extra.csv");
    refuse("a carried file nothing names", assemble(extra), BackupError::Damaged, uncovered);
    with_row(extra, "saved_systems.json", 0,
             [&](QJsonObject& row) { row.insert("srcCsvPath", root + "/imports/extra.csv"); });
    expect("the same file parses once a path field names it",
           dsd_qt::backup::backup_parse_archive(assemble(extra), root).error == BackupError::None);
    const QString chanBundle = f.chanRoot.mid(root.size() + 1);
    Parts inBundle = parts;
    add_file(inBundle, chanBundle + "/extra.csv");
    expect("a file inside a claimed bundle parses",
           dsd_qt::backup::backup_parse_archive(assemble(inBundle), root).error == BackupError::None);
    const QString upper = QStringLiteral("imports/bundles/") + chanBundle.mid(16).toUpper();
    expect("the bundle id has letters to fold", upper != chanBundle);
    Parts caseVariant = parts;
    add_file(caseVariant, upper + "/extra.csv");
    refuse("a file in a case variant of a claimed bundle", assemble(caseVariant), BackupError::Damaged, uncovered);
}

void
refuse_typed_paths(const QByteArray& archive, const Fixture& f) {
    const Parts parts = split(archive);
    const QString root = dsd_qt::json_store_root();
    const QJsonArray library = parts.manifest.value("stores").toObject().value("imported_files.json").toArray();
    const int groupRow = row_where(library, "path", f.groups);
    const auto setRow = [&](const char* store, int row, const char* key, const QJsonValue& value) {
        return set_row(parts, store, row, key, value);
    };
    refuse("system names a file outside the root", setRow("saved_systems.json", 0, "chanCsvPath", "/etc/passwd"),
           BackupError::Damaged, "typed:");
    refuse("system names an uncarried import",
           setRow("saved_systems.json", 0, "groupCsvPath", root + "/imports/other.csv"), BackupError::Damaged,
           "typed:");
    refuse("system names a carried file through ..",
           setRow("saved_systems.json", 0, "groupCsvPath", root + "/imports/x/../" + QFileInfo(f.groups).fileName()),
           BackupError::Damaged, "typed:");
    refuse("typed value is not a string", setRow("saved_systems.json", 0, "keyCsvPath", 5), BackupError::Damaged,
           "typed:");
    refuse("recording carried", setRow("saved_systems.json", 1, "filePath", root + "/imports/x.wav"),
           BackupError::Damaged, "typed:");
    refuse("scan list targets outside", setRow("scan_lists.json", 1, "targetsCsvPath", "/sdcard/targets.csv"),
           BackupError::Damaged, "typed:");
    refuse("profile key CSV outside imports",
           setRow("decryption_profiles.json", 0, "legacyCsvPath", root + "/decryption/x/keys.csv"),
           BackupError::Damaged, "typed:");
    refuse("profile mapping outside", setRow("decryption_profiles.json", 1, "mappingCsvPath", "/data/map.csv"),
           BackupError::Damaged, "typed:");
    refuse("profile vendor CSV outside", setRow("decryption_profiles.json", 0, "vendorCsvPath", "/data/vertex.csv"),
           BackupError::Damaged, "typed:");
    refuse("library row names an uncarried file", setRow("imported_files.json", groupRow, "path", root + "/x.csv"),
           BackupError::Damaged, "typed:");
    refuse("library row with an empty path", setRow("imported_files.json", groupRow, "path", ""), BackupError::Damaged,
           "typed: a library row names no file");
    refuse_bundle_roots(parts, f);
    refuse_uncovered_files(parts, f);
    Parts sourceRoot = parts;
    sourceRoot.manifest.insert("sourceRoots", QJsonArray{"/data/other"});
    const auto unrebased = dsd_qt::backup::backup_parse_archive(assemble(sourceRoot), "/restore/elsewhere/files");
    expect("values outside every source root are not rebased",
           unrebased.error == BackupError::Damaged && unrebased.detail.startsWith("typed:"));
    expect("the same archive rebases from its real roots",
           dsd_qt::backup::backup_parse_archive(assemble(parts), "/restore/elsewhere/files").error
               == BackupError::None);
}

/* Every decode flag the app's own producers write, taken from those producers wherever they are callable. */
QStringList
app_produced_decode_flags(TestHost& host) {
    // The wizard's decode chips (qml/Util.js DECODE_MODES), the older saves it still labels (LEGACY_DECODE_LABELS), the
    // "-f1 -mq" chip saved_systems_model.cpp migrates on load, and a scan list's frequency entries
    // (scan_list_targets.cpp resolveSystem()): QML and file-local code a test cannot call, so listed here.
    QStringList flags{"", "-ft", "-mq", "-fs", "-fi", "-fn", "-fd", "-fy", "-fz", "-fA", "-fM", "-f1", "-f1 -mq"};
    // The RadioReference generator, through the function the import calls, for every protocol and answer.
    for (int protocol = 0; protocol < DSD_RR_PROTO_UNSUPPORTED; ++protocol) {
        for (int bits = 0; bits < 8; ++bits) {
            const char* flag =
                dsd_rr_decode_flag(static_cast<dsd_rr_protocol>(protocol), bits & 1, (bits >> 1) & 1, (bits >> 2) & 1);
            if (flag != nullptr) {
                flags << QString::fromLatin1(flag);
            }
        }
    }
    // Signal search detections, through the model's own system builder.
    const dsd_qt::SignalSearchModel search(&host, nullptr);
    for (const char* protocol :
         {"P25", "DMR", "NXDN", "D-STAR", "NXDN48", "NXDN96", "dPMR", "M17", "YSF", "ProVoice"}) {
        flags << search.systemForHit({{"confirmed", true}, {"protocol", protocol}}, {}).value("decodeFlag").toString();
    }
    for (const bool am : {false, true}) {
        flags << search.systemForHit({{"confirmed", false}, {"am", am}}, {}).value("decodeFlag").toString();
    }
    flags.removeDuplicates();
    return flags;
}

/* A decode flag is split into the decoder's arguments like the extra arguments: one the app itself writes is a decode
 * choice, and one carrying any other token passes decoder options of its own (controller rulings R8, R10). */
void
test_decode_flag_standard(TestHost& host) {
    const QStringList produced = app_produced_decode_flags(host);
    for (const char* sample : {"-ft -^", "-mq -^", "-ft -Y", "-mq -Y", "-fs -Y", "-fh", "-fH", "-fe", "-fE", "-fp"}) {
        expect(qPrintable(QStringLiteral("producer writes '%1'").arg(QLatin1String(sample))),
               produced.contains(QLatin1String(sample)));
    }
    for (const QString& flag : produced) {
        expect(qPrintable(QStringLiteral("app-produced flag is standard: '%1'").arg(flag)),
               dsd_qt::decode_flag_is_standard(flag) && dsd_qt::decode_flag_is_standard(" " + flag + "\t"));
        for (const char* extra : {"-w /sdcard/x.wav", "--rdio-api-key k", "-o pulse", "-Z"}) {
            const QString carrying = flag + QLatin1Char(' ') + QLatin1String(extra);
            expect(qPrintable(QStringLiteral("extra token counts: '%1'").arg(carrying)),
                   !dsd_qt::decode_flag_is_standard(carrying));
        }
    }
    for (const char* flag : {"-fx", "-f2", "-fq", "-fh344", "-f", "-ftt", "--fs", "-w", "-mqq", "-Y1", "^"}) {
        expect(qPrintable(QStringLiteral("unknown token counts: '%1'").arg(QLatin1String(flag))),
               !dsd_qt::decode_flag_is_standard(QLatin1String(flag)));
    }
}

void
test_decode_flag_counts(const QByteArray& archive) {
    const Parts parts = split(archive);
    const auto restored = [&](const QJsonValue& flag) {
        // Row 1 is the replay system, with no extra arguments of its own; row 0 has "-Z".
        return dsd_qt::backup::backup_parse_archive(set_row(parts, "saved_systems.json", 1, "decodeFlag", flag),
                                                    dsd_qt::json_store_root())
            .summary.systemsWithExtraArgs;
    };
    expect("an app-produced flag adds no extra options", restored("-fs") == 1 && restored("-mq") == 1
                                                             && restored("") == 1 && restored("-fs -Y") == 1
                                                             && restored("-mq -^") == 1 && restored("-fE") == 1);
    expect("a flag carrying arguments counts", restored("-fs -w /sdcard/x.wav") == 2);
    expect("a flag carrying an unknown switch counts", restored("-fs -Y --rdio-api-key k") == 2);
    expect("an unmapped selector counts", restored("-fq") == 2);
    expect("a mapped selector no producer writes counts", restored("-fx") == 2 && restored("-f2") == 2);
    expect("a numeric flag counts as its text", restored(5) == 2);

    auto snapshot = dsd_qt::backup::backup_take_snapshot();
    QJsonArray systems = snapshot.stores.value("saved_systems.json");
    QJsonObject replay = systems.at(1).toObject();
    replay.insert("decodeFlag", "-ft --dmr-key 1");
    systems.replace(1, replay);
    snapshot.stores.insert("saved_systems.json", systems);
    const auto built = build(snapshot);
    expect("the export counts a decode flag carrying arguments",
           built.error == BackupError::None && built.summary.systemsWithExtraArgs == 2);
}

void
test_unknown_store_name() {
    QJsonArray rows{QJsonObject{{"uid", "a"}}};
    QString why;
    expect("an unknown store name is refused",
           !dsd_qt::backup::detail::validate_store("other.json", rows, &why) && !why.isEmpty());
}

void
test_passphrases() {
    using dsd_qt::backup::backup_passphrase_ok;
    const QString acute = QString(QChar(0x0301));
    expect("seven characters are too short", !backup_passphrase_ok("1234567"));
    expect("eight characters are enough", backup_passphrase_ok("12345678"));
    expect("NFD counts after composition", !backup_passphrase_ok("abcdef" + QStringLiteral("e") + acute));
    expect("composed character counts once", backup_passphrase_ok("abcdefg" + QStringLiteral("e") + acute));
    const QString nfc = QStringLiteral("p") + QChar(0x00E4) + QStringLiteral("ssw") + QChar(0x00F6) + "rd-ok";
    const QString nfd = QStringLiteral("pa") + QChar(0x0308) + QStringLiteral("sswo") + QChar(0x0308) + "rd-ok";
    expect("NFC and NFD encode the same bytes",
           dsd_qt::backup::backup_passphrase_bytes(nfc) == dsd_qt::backup::backup_passphrase_bytes(nfd)
               && dsd_qt::backup::backup_passphrase_bytes(nfc) == nfc.toUtf8());
    // The normalized copy is wiped inside; the caller's string never is, whether normalized() shared it (already
    // NFC), built a new one (NFD) or the string is static data.
    const QString literal = QStringLiteral("static-passphrase");
    for (const QString& input : {nfc, nfd, literal}) {
        const QString before(input.constData(), input.size());
        const QByteArray bytes = dsd_qt::backup::backup_passphrase_bytes(input);
        const bool ok = backup_passphrase_ok(input);
        expect(qUtf8Printable(QStringLiteral("the caller's passphrase stays as it was: %1").arg(before)),
               input == before && ok && bytes == before.normalized(QString::NormalizationForm_C).toUtf8());
    }
    int status = -100;
    const QByteArray sealed = dsd_qt::backup::backup_seal("DNBA payload", nfc, test_kdf(), &status);
    expect("NFC seal", status == DSD_BACKUP_OK);
    expect("NFD opens an NFC seal",
           dsd_qt::backup::backup_open(sealed, nfd, &status) == "DNBA payload" && status == DSD_BACKUP_OK);
    expect("not a sealed backup",
           dsd_qt::backup::backup_open("plain text", nfc, &status).isEmpty() && status == DSD_BACKUP_ERR_FORMAT);

    using dsd_qt::backup::backup_error_from_seal_status;
    expect("status mapping", backup_error_from_seal_status(DSD_BACKUP_ERR_FORMAT) == BackupError::NotBackup
                                 && backup_error_from_seal_status(DSD_BACKUP_ERR_VERSION) == BackupError::NewerVersion
                                 && backup_error_from_seal_status(DSD_BACKUP_ERR_AUTH) == BackupError::WrongPassphrase
                                 && backup_error_from_seal_status(DSD_BACKUP_ERR_TOO_LARGE) == BackupError::TooLarge
                                 && backup_error_from_seal_status(DSD_BACKUP_ERR_PARAMS) == BackupError::Damaged
                                 && backup_error_from_seal_status(DSD_BACKUP_ERR_INTERNAL) == BackupError::Io
                                 && backup_error_from_seal_status(DSD_BACKUP_OK) == BackupError::None);
    expect("error names", dsd_qt::backup::backup_error_name(BackupError::WrongPassphrase) == "WrongPassphrase"
                              && dsd_qt::backup::backup_error_name(BackupError::SessionActive) == "SessionActive");
}

void
test_host_document_copies() {
    TestHost host;
    expect("base host cannot quit", !host.quitApp());
    expect("base host has no out-of-process engine", !host.engineBusy());
    QTemporaryDir dir;
    const QByteArray payload(1000, 'b');
    const QString source = dir.filePath("source.dsdbak");
    expect("write copy source", write_file(source, payload));

    const QString fromUrl = dir.filePath("from-url.bin");
    expect("file:// reference copies",
           host.copyDocumentToFile(QUrl::fromLocalFile(source).toString(), fromUrl, payload.size())
                   == dsd_qt::DecoderHost::DocumentCopied
               && read_file(fromUrl) == payload);
    expect("copy is private", !(QFile::permissions(fromUrl)
                                & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
    const QString fromPath = dir.filePath("from-path.bin");
    expect("plain path copies",
           host.copyDocumentToFile(source, fromPath, payload.size() + 5) == dsd_qt::DecoderHost::DocumentCopied
               && read_file(fromPath) == payload);
    const QString capped = dir.filePath("capped.bin");
    expect("one byte over the cap is too large",
           host.copyDocumentToFile(source, capped, payload.size() - 1) == dsd_qt::DecoderHost::DocumentTooLarge);
    expect("too large leaves no destination", !QFileInfo::exists(capped));
#ifdef Q_OS_LINUX
    // procfs reports no size, so only the cap counted while copying can refuse it.
    const QString proc = QStringLiteral("/proc/self/status");
    expect("procfs source reports no size", QFileInfo(proc).isFile() && QFileInfo(proc).size() == 0);
    const QString streamed = dir.filePath("streamed.bin");
    expect("the cap is counted while copying",
           host.copyDocumentToFile(proc, streamed, 16) == dsd_qt::DecoderHost::DocumentTooLarge);
    expect("a cap met while copying leaves no destination", !QFileInfo::exists(streamed));
    expect("an unsized source within the cap copies",
           host.copyDocumentToFile(proc, streamed, 1 << 20) == dsd_qt::DecoderHost::DocumentCopied
               && QFileInfo(streamed).size() > 16);
#endif
    const QString failed = dir.filePath("failed.bin");
    expect("missing source fails",
           host.copyDocumentToFile(dir.filePath("absent"), failed, 10) == dsd_qt::DecoderHost::DocumentFailed);
    expect("opaque reference fails",
           host.copyDocumentToFile("content://x/y", failed, 10) == dsd_qt::DecoderHost::DocumentFailed);
    expect("failure leaves no destination", !QFileInfo::exists(failed));

    const QString saved = dir.filePath("saved.dsdbak");
    expect("copy to a file:// document",
           host.copyFileToDocument(source, QUrl::fromLocalFile(saved).toString()) && read_file(saved) == payload);
    const QString plain = dir.filePath("plain.dsdbak");
    expect("copy to a plain path", host.copyFileToDocument(source, plain) && read_file(plain) == payload);
    const QString none = dir.filePath("none.dsdbak");
    expect("missing export fails", !host.copyFileToDocument(dir.filePath("absent"), none) && !QFileInfo::exists(none));
    expect("opaque document fails", !host.copyFileToDocument(source, "content://x/y"));
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    /* Isolated, disposable storage - nothing this test writes may touch a real profile (same arrangement as
     * test_ui_qt_persistence). */
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-backup-archive-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir(dataDir).removeRecursively();
    QDir().mkpath(dataDir);

    test_json_store_root();
    test_typed_path_table();
    test_unknown_store_name();
    test_passphrases();
    test_host_document_copies();
    {
        TestHost host;
        const Fixture fixture = populate(host);
        test_round_trip(fixture);
        test_foreign_root();
        const auto base = dsd_qt::backup::backup_build_archive(dsd_qt::backup::backup_take_snapshot(), nullptr);
        expect("base archive builds", base.error == BackupError::None);
        expect("base archive parses",
               dsd_qt::backup::backup_parse_archive(base.archive, dataDir).error == BackupError::None);
        test_strip_compiled(base.archive);
        refuse_archive_layout(base.archive);
        refuse_file_table(base.archive);
        refuse_store_shapes(base.archive);
        refuse_prefs_and_avoids(base.archive);
        refuse_typed_paths(base.archive, fixture);
        test_decode_flag_standard(host);
        test_decode_flag_counts(base.archive);
        test_export_repairs();
        test_export_drops_foreign_bundle(fixture);
        test_export_folded_bundle_paths(fixture);
        test_build_caps_and_cancel();
        test_sanitize();
    }
    dsd_app_signal_search_store_close();

    QDir(dataDir).removeRecursively();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
