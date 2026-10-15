// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit tests: staging a validated backup and applying it at the next start (issue #580). The backup and the older
 * configuration it is restored over are both built by the real models. Covered: the staging layout and its library
 * dry run; promote, commit, discard and the staging sweep; a clean apply; the files an apply must keep (recordings,
 * unknown files, external, symlinked and non-owned references) and the free names incoming files take instead, except
 * for a bundle a channel map beside it names, which never moves; two restores in a row, the second owning exactly what
 * the first wrote; the containment check; a crash at every step of the apply and of its rollback; I/O failures that
 * roll back to a byte-identical state; Stuck; Failed before any change; Deferred; and the result file. Registered only
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
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QtEndian>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/app_control/signal_search.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/signal_search.h>
#include <functional>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <iterator>
#include <qflags.h>
#include <stdint.h>
#include <stdio.h>
#include <utility>
#include <vector>
#include "../test_support/qt_test_paths.h"
#include "app_prefs.h"
#include "backup_apply.h"
#include "decoder_host.h"
#include "decryption_profiles_model.h"
#include "imported_files_model.h"
#include "json_store.h"
#include "saved_systems_model.h"
#include "scan_lists_model.h"
#include "signal_search_model.h"

namespace {

using dsd_qt::backup::ApplyEnv;
using dsd_qt::backup::ApplyOutcome;
using dsd_qt::backup::ApplyResult;
using dsd_qt::backup::BackupError;
using dsd_qt::backup::Fault;

constexpr char kResultFile[] = "backup-restore-result.json";
constexpr char kAvoidsFile[] = "signal-search-history.v1.avoids.json";

int g_failures = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

void
expect(const QString& what, bool ok) {
    expect(qUtf8Printable(what), ok);
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

QString
root() {
    return dsd_qt::json_store_root();
}

QString
at(const QString& relative) {
    return root() + QLatin1Char('/') + relative;
}

QString
relative(const QString& path) {
    return path.mid(root().size() + 1);
}

QString
pending(const QString& relativePath = QString()) {
    return relativePath.isEmpty() ? at("restore-pending") : at("restore-pending/" + relativePath);
}

/**
 * The one way this test removes a directory tree: @p path must be a clean absolute path equal to or below the
 * isolated data root, itself clean, absolute, not "/" and named for this test. Anything else - an empty path is the
 * working directory - is refused and counted as a failure.
 */
void
remove_tree(const QString& path) {
    const QString base = root();
    const bool baseOk = base.size() > 1 && QDir::isAbsolutePath(base) && QDir::cleanPath(base) == base
                        && base.contains(QLatin1String("dsd-neo-backup-apply-"));
    const bool pathOk = !path.isEmpty() && QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path
                        && (path == base || path.startsWith(base + QLatin1Char('/')));
    if (baseOk && pathOk) {
        QDir(path).removeRecursively();
    } else {
        expect(QStringLiteral("only the isolated data root is removed: '%1'").arg(path), false);
    }
}

/**
 * The one way this test removes a single entry (a file or a symlink, never followed): the same rules as remove_tree(),
 * and the path must lie strictly below the isolated root.
 */
bool
remove_file(const QString& path) {
    const QString base = root();
    const bool ok = base.size() > 1 && QDir::isAbsolutePath(base) && QDir::cleanPath(base) == base
                    && base.contains(QLatin1String("dsd-neo-backup-apply-")) && !path.isEmpty()
                    && QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path
                    && path.startsWith(base + QLatin1Char('/'));
    if (!ok) {
        expect(QStringLiteral("only entries below the isolated data root are removed: '%1'").arg(path), false);
        return false;
    }
    return QFile::remove(path);
}

/** Every entry below a directory - files by content, symlinks by target, directories by name - and the settings. */
struct Tree {
    QMap<QString, QByteArray> files;
    QMap<QString, QString> links;
    QSet<QString> dirs;
    QVariantMap prefs;
};

Tree
capture_dir(const QString& directory) {
    Tree tree;
    QDirIterator it(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info(path);
        const QString name = path.mid(directory.size() + 1);
        if (info.isSymLink()) {
            tree.links.insert(name, info.symLinkTarget());
        } else if (info.isDir()) {
            tree.dirs.insert(name);
        } else {
            tree.files.insert(name, read_file(path));
        }
    }
    return tree;
}

/** The data root and the portable settings. */
Tree
capture() {
    Tree tree = capture_dir(root());
    tree.prefs = dsd_qt::AppPrefs::readPortable();
    return tree;
}

/** Replace the data root and the portable settings with @p tree. */
void
put_back(const Tree& tree) {
    remove_tree(root());
    QDir().mkpath(root());
    for (const QString& dir : tree.dirs) {
        QDir().mkpath(at(dir));
    }
    for (auto it = tree.files.cbegin(); it != tree.files.cend(); ++it) {
        write_file(at(it.key()), it.value());
    }
    for (auto it = tree.links.cbegin(); it != tree.links.cend(); ++it) {
        QFile::link(it.value(), at(it.key()));
    }
    dsd_qt::AppPrefs::replacePortable(tree.prefs);
}

QVariantMap
as_strings(const QVariantMap& map) {
    QVariantMap out;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        out.insert(it.key(), it.value().toString());
    }
    return out;
}

/** @p tree without the entries at or below each of @p prefixes. */
Tree
without(Tree tree, const QStringList& prefixes) {
    const auto ignored = [&prefixes](const QString& name) {
        for (const QString& prefix : prefixes) {
            if (name == prefix || name.startsWith(prefix + QLatin1Char('/'))) {
                return true;
            }
        }
        return false;
    };
    for (auto it = tree.files.begin(); it != tree.files.end();) {
        it = ignored(it.key()) ? tree.files.erase(it) : std::next(it);
    }
    for (auto it = tree.links.begin(); it != tree.links.end();) {
        it = ignored(it.key()) ? tree.links.erase(it) : std::next(it);
    }
    for (auto it = tree.dirs.begin(); it != tree.dirs.end();) {
        it = ignored(*it) ? tree.dirs.erase(it) : std::next(it);
    }
    return tree;
}

/** Whether two trees agree, each difference listed on stderr. Directories only when @p dirs is set. */
bool
same_tree(const Tree& gotTree, const Tree& wantTree, bool dirs, const QStringList& ignore = {}) {
    const Tree got = without(gotTree, ignore), want = without(wantTree, ignore);
    const auto& files = got.files;
    const auto& wantFiles = want.files;
    const QSet<QString> gotNames(files.keyBegin(), files.keyEnd()), wantNames(wantFiles.keyBegin(), wantFiles.keyEnd());
    for (const QString& name : gotNames - wantNames) {
        DSD_FPRINTF(stderr, "  unexpected file: %s\n", qUtf8Printable(name));
    }
    for (const QString& name : wantNames - gotNames) {
        DSD_FPRINTF(stderr, "  missing file: %s\n", qUtf8Printable(name));
    }
    bool ok = gotNames == wantNames;
    for (const QString& name : gotNames& wantNames) {
        if (files.value(name) != wantFiles.value(name)) {
            DSD_FPRINTF(stderr, "  different bytes: %s\n", qUtf8Printable(name));
            ok = false;
        }
    }
    if (got.links != want.links) {
        DSD_FPRINTF(stderr, "  symlinks differ\n");
        ok = false;
    }
    if (dirs && got.dirs != want.dirs) {
        for (const QString& name : got.dirs - want.dirs) {
            DSD_FPRINTF(stderr, "  unexpected directory: %s\n", qUtf8Printable(name));
        }
        for (const QString& name : want.dirs - got.dirs) {
            DSD_FPRINTF(stderr, "  missing directory: %s\n", qUtf8Printable(name));
        }
        ok = false;
    }
    if (as_strings(got.prefs) != as_strings(want.prefs)) {
        DSD_FPRINTF(stderr, "  settings differ\n");
        ok = false;
    }
    return ok;
}

/** The backup: a configuration the real models built, exported, and parsed for this data root. */
struct Backup {
    QByteArray archive;
    dsd_qt::backup::ParseResult parsed;
    QString groups, keys, bandplan, src, mapping, chan, chanRoot, targets, targetsRoot, extra;
};

QString
import_path(dsd_qt::ImportedFilesModel& library, const QString& source, const QString& name, const QString& type) {
    const QVariantMap result = library.importFile(source, name, type);
    expect("fixture import succeeds", result.value("ok").toBool());
    return result.value("path").toString();
}

void
import_bundles(dsd_qt::ImportedFilesModel& library, Backup& b) {
    QTemporaryDir source;
    expect("write channel bundle fixture",
           write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
               && write_file(source.filePath("map.csv"), "tg_dec,keyid_hex\n123,02\n")
               && write_file(source.filePath("channels.csv"),
                             "channel,frequency_hz,mode,keys_hex_csv,options\n1,461000000,dmr,keys.csv,"
                             "--dmr-tg-key-csv 'map.csv'\n"));
    QVariantMap result = library.importBundle(source.filePath("channels.csv"), "Bundle.csv", "chan", {});
    expect("channel bundle imports", result.value("ok").toBool());
    b.chan = result.value("path").toString();
    b.chanRoot = library.get(library.rowForPath(b.chan)).value("bundleRoot").toString();

    expect("write target bundle fixture",
           write_file(source.filePath("tkeys.csv"), "id,value\n01,ABCDEF0123\n")
               && write_file(source.filePath("tmap.csv"),
                             "channel,frequency,mode,keys_hex_csv\n1,851012500,p25,tkeys.csv\n")
               && write_file(source.filePath("targets.csv"),
                             "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                             "one,p25-trunk,851012500,tmap.csv,250,,\n"));
    result = library.importFile(source.filePath("targets.csv"), "Targets.csv", "trunkTargets");
    expect("target bundle imports", result.value("ok").toBool());
    b.targets = result.value("path").toString();
    b.targetsRoot = library.get(library.rowForPath(b.targets)).value("bundleRoot").toString();
}

/** A profile whose keys the model compiles into decryption/<uid>/. */
QVariantMap
managed_profile(const QString& uid) {
    const QVariantMap key{{"uid", uid + "-key"},
                          {"label", "Dispatch"},
                          {"keyId", 17},
                          {"kind", "aes128"},
                          {"material", "00112233445566778899aabbccddeeff"}};
    return {{"uid", uid}, {"label", uid}, {"protocol", "p25"}, {"mode", "automatic"}, {"keys", QVariantList{key}}};
}

void
add_systems_and_lists(Backup& b) {
    dsd_qt::SavedSystemsModel systems;
    expect("add trunked system", systems.add({{"name", "Metro"},
                                              {"sourceType", "usb"},
                                              {"freqMhz", "851.0125"},
                                              {"decodeFlag", "-ft"},
                                              {"trunking", true},
                                              {"chanCsvPath", b.chan},
                                              {"groupCsvPath", b.groups},
                                              {"keyCsvPath", b.keys},
                                              {"keyCsvHex", true},
                                              {"p25BandplanCsvPath", b.bandplan},
                                              {"srcCsvPath", b.src}}));
    expect("add replay system",
           systems.add({{"name", "Replay"}, {"sourceType", "file"}, {"filePath", at("recordings/capture.wav")}}));
    const QString systemUid = systems.get(0).value("uid").toString();

    dsd_qt::ScanListsModel scans;
    const QVariantList entries{QVariantMap{{"kind", "system"}, {"systemUid", systemUid}},
                               QVariantMap{{"kind", "freq"}, {"freqMhz", "462.5625"}, {"protocol", "nfm"}}};
    expect("add scan list",
           scans.add({{"name", "Patrol"}, {"groupCsvPath", b.groups}, {"srcCsvPath", b.src}, {"entries", entries}}));
    // A file in a subdirectory of imports/ that only a path field names.
    b.extra = at("imports/extra/notes.csv");
    expect("write a talkgroup list in a subdirectory", write_file(b.extra, "TG,Mode,Name\n7,A,Extra\n"));
    expect(
        "add target scan list",
        scans.add(
            {{"name", "Targets"}, {"targetSource", "csv"}, {"targetsCsvPath", b.targets}, {"groupCsvPath", b.extra}}));

    dsd_qt::DecryptionProfilesModel profiles;
    profiles.setReferences(&systems, &scans);
    expect("save CSV profile", profiles
                                   .saveProfile({{"uid", "csv-keys"},
                                                 {"label", "CSV keys"},
                                                 {"protocol", "p25"},
                                                 {"mode", "automatic"},
                                                 {"keySource", "csv"},
                                                 {"legacyCsvPath", b.keys},
                                                 {"legacyCsvHex", true}})
                                   .value("ok")
                                   .toBool());
    expect("save managed-key profile", profiles.saveProfile(managed_profile("managed")).value("ok").toBool());
    expect("save mapped profile", profiles
                                      .saveProfile({{"uid", "dmr-map"},
                                                    {"label", "DMR map"},
                                                    {"protocol", "dmr"},
                                                    {"mode", "automatic"},
                                                    {"keySource", "csv"},
                                                    {"legacyCsvPath", b.keys},
                                                    {"legacyCsvHex", true},
                                                    {"mappingCsvPath", b.mapping}})
                                      .value("ok")
                                      .toBool());
}

/** Configuration A, built by the models in an empty root, then exported and parsed. */
Backup
make_backup(TestHost& host) {
    Backup b;
    {
        dsd_qt::ImportedFilesModel library(&host);
        b.groups = import_path(library, example("group.csv"), "group.csv", "group");
        b.keys = import_path(library, example("multi_key_hex.csv"), "keys.csv", "keysHex");
        b.bandplan = import_path(library, example("p25_bandplan.csv"), "bandplan.csv", "p25Bandplan");
        b.src = import_path(library, example("src.csv"), "src.csv", "src");
        b.mapping = import_path(library, example("dmr_tg_key_map.csv"), "map.csv", "dmrTgKeys");
        import_bundles(library, b);
    }
    add_systems_and_lists(b);
    {
        dsd_qt::SignalSearchModel search(&host, nullptr);
        QVariantMap range = search.defaultRange();
        range.insert("name", "Marine");
        expect("save search range", search.saveRange(-1, range).isEmpty());
        dsd_app_signal_search_permanent_avoid(156800000U);
        dsd_app_signal_search_permanent_avoid(162550000U);
    }
    expect("write backup settings",
           dsd_qt::AppPrefs::replacePortable(
               {{"decode/extraArgs", "-Z"}, {"rr/username", "scanner"}, {"tuner/gainDb", "30"}}));
    const auto built = dsd_qt::backup::backup_build_archive(dsd_qt::backup::backup_take_snapshot(), nullptr);
    expect("backup builds", built.error == BackupError::None);
    b.archive = built.archive;
    b.parsed = dsd_qt::backup::backup_parse_archive(b.archive, root());
    expect("backup parses", b.parsed.error == BackupError::None);
    expect("backup carries both bundles and every library file", b.parsed.files.size() >= 11);
    return b;
}

/** The configuration a backup is restored over, and the entries every apply must keep. */
struct Old {
    Tree tree;
    QStringList kept;     // files every apply keeps byte-identical
    QString groupsDest;   // the free name the backup's talkgroup list takes
    QString srcDest;      // the free name the backup's radio-ID list takes
    QString newRevision;  // a companion of the bundle's newer revision
    QString extraLibrary; // a library file only the old configuration has
    QString notOwnedRow;  // a library row's file in a directory that is not an owned bundle
    QString externalFile; // a file outside the data root a live system names
    QString linkTarget;   // the external file a symlink under imports/ points at
    QString targetsDest;  // the fresh bundle the backup's target bundle moves to
};

QString
free_name(const QString& path) {
    const QFileInfo info(path);
    return info.path() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(" (1).") + info.suffix();
}

/** A newer revision of the channel bundle and a library file only the old configuration has. */
void
change_library(TestHost& host, const Backup& b, Old& old) {
    dsd_qt::ImportedFilesModel library(&host);
    QTemporaryDir source;
    expect("write newer bundle revision",
           write_file(source.filePath("keys2.csv"), "keyid,value\n03,FEDCB\n")
               && write_file(source.filePath("channels.csv"),
                             "channel,frequency_hz,mode,keys_hex_csv\n1,462000000,dmr,keys2.csv\n"));
    const QVariantMap updated =
        library.importBundle(source.filePath("channels.csv"), "Bundle.csv", "chan", {}, library.rowForPath(b.chan));
    expect("bundle updated in place",
           updated.value("ok").toBool() && library.get(library.rowForPath(b.chan)).value("bundleRoot") == b.chanRoot);
    for (QDirIterator it(b.chanRoot, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const QString file = it.next();
        if (read_file(file).contains("FEDCB")) {
            old.newRevision = file;
        }
    }
    expect("the bundle holds a newer revision", !old.newRevision.isEmpty());
    old.extraLibrary = import_path(library, example("group.csv"), "extra.csv", "group");

    // The talkgroup, radio-ID and target lists are removed from the library; something else then takes their names.
    for (const QString& path : {b.groups, b.src, b.targets}) {
        expect("library row removed", library.remove(library.rowForPath(path)));
    }
    expect("the target bundle went with its row", !QFileInfo::exists(b.targetsRoot));
}

/** Systems and lists the old configuration has instead: recordings, an external file, a symlink, a stray path. */
void
change_references(const Backup& b, Old& old, const QString& external) {
    dsd_qt::SavedSystemsModel systems;
    dsd_qt::ScanListsModel scans;
    for (const QString& path : {b.groups, b.src}) {
        systems.clearCsvPath(path);
    }
    expect("the target scan list drops its file", scans.clearCsvPath(b.targets));
    expect("an unreferenced directory where the target bundle was",
           write_file(b.targetsRoot + "/notes.txt", "not a bundle of ours\n"));
    QVariantMap patrol = scans.get(0);
    patrol.insert("groupCsvPath", "");
    patrol.insert("srcCsvPath", "");
    expect("the scan list drops the removed files", scans.update(0, patrol));
    // An unknown file where the talkgroup list was, and a recording a live system plays where the radio IDs were.
    expect("unknown file at an incoming name", write_file(b.groups, "my own notes\n"));
    expect("recording at an incoming name", write_file(b.src, "RIFF recording at an incoming name"));
    expect("add colliding replay system",
           systems.add({{"name", "Field"}, {"sourceType", "file"}, {"filePath", b.src}}));
    expect("referenced recording", write_file(at("recordings/capture.wav"), "RIFF referenced"));
    expect("unreferenced recording", write_file(at("recordings/loose.wav"), "RIFF loose"));
    expect("deleted system's recording", write_file(at("recordings/gone.wav"), "RIFF gone"));
    // A file a path field and a recording both name: the recording keeps it.
    expect("file named as a list and a recording", write_file(at("imports/both.csv"), "RIFF both\n"));
    expect("add system naming it as a list",
           systems.add({{"name", "Both"}, {"sourceType", "usb"}, {"groupCsvPath", at("imports/both.csv")}}));
    expect("add system naming it as a recording",
           systems.add({{"name", "BothReplay"}, {"sourceType", "file"}, {"filePath", at("imports/both.csv")}}));
    // The same, with the recording spelled another way: an unclean path, and the root through an alias.
    expect("files named by other spellings", write_file(at("imports/both2.csv"), "RIFF unclean\n")
                                                 && write_file(at("imports/both3.csv"), "RIFF alias\n")
                                                 && QFile::link(root(), external + "/rootalias"));
    expect("add systems naming them as lists",
           systems.add({{"name", "Both2"}, {"sourceType", "usb"}, {"groupCsvPath", at("imports/both2.csv")}})
               && systems.add({{"name", "Both3"}, {"sourceType", "usb"}, {"groupCsvPath", at("imports/both3.csv")}}));
    expect("add systems naming them as recordings, spelled otherwise",
           systems.add({{"name", "Both2Replay"}, {"sourceType", "file"}, {"filePath", at("imports/sub/../both2.csv")}})
               && systems.add({{"name", "Both3Replay"},
                               {"sourceType", "file"},
                               {"filePath", external + "/rootalias/imports/both3.csv"}}));
    // And in another case: the same file where storage folds case; a file kept to be safe where it does not.
    expect("file named in another case", write_file(at("imports/both4.csv"), "RIFF case\n"));
    expect(
        "add systems naming it in two cases",
        systems.add({{"name", "Both4"}, {"sourceType", "usb"}, {"groupCsvPath", at("imports/both4.csv")}})
            && systems.add({{"name", "Both4Replay"}, {"sourceType", "file"}, {"filePath", at("IMPORTS/BOTH4.CSV")}}));
    expect("add a system to delete",
           systems.add({{"name", "Gone"}, {"sourceType", "file"}, {"filePath", at("recordings/gone.wav")}}));
    systems.remove(systems.rowCount() - 1);

    old.externalFile = external + "/outside.csv";
    old.linkTarget = external + "/linked.csv";
    expect("external files", write_file(old.externalFile, "TG,Mode,Name\n1,A,Outside\n")
                                 && write_file(old.linkTarget, "TG,Mode,Name\n2,A,Linked\n")
                                 && write_file(at("notes/groups.csv"), "TG,Mode,Name\n3,A,Notes\n"));
    expect("symlink under imports", QFile::link(old.linkTarget, at("imports/linked.csv")));
    expect("add system with stray paths", systems.add({{"name", "Outside"},
                                                       {"sourceType", "usb"},
                                                       {"chanCsvPath", old.externalFile},
                                                       {"groupCsvPath", at("notes/groups.csv")},
                                                       {"srcCsvPath", at("imports/../notes/groups.csv")}}));
    expect("add scan list through a symlink",
           scans.add({{"name", "Linked"}, {"srcCsvPath", at("imports/linked.csv")}}));
}

/** A library row whose bundle root is not an owned bundle directory, written into the store as a user's might be. */
void
add_not_owned_row(Old& old) {
    old.notOwnedRow = at("imports/bundles/not owned/row.csv");
    expect("non-owned bundle files", write_file(old.notOwnedRow, "TG,Mode,Name\n4,A,Row\n")
                                         && write_file(at("imports/bundles/not owned/keep.csv"), "keep me\n"));
    QJsonArray library = dsd_qt::json_store_load_array("imported_files.json");
    library.append(QJsonObject{{"name", "Not owned"},
                               {"path", old.notOwnedRow},
                               {"type", "group"},
                               {"bundleRoot", at("imports/bundles/not owned")}});
    // Two rows claim roots whose names differ only by case: the first claim owns its tree, the second only its file.
    for (const char* id : {"caseroot", "CASEROOT"}) {
        const QString dir = at(QStringLiteral("imports/bundles/") + QLatin1String(id));
        expect("folded bundle files",
               write_file(dir + "/row.csv", "TG,Mode,Name\n5,A,Case\n") && write_file(dir + "/extra.txt", "extra\n"));
        library.append(QJsonObject{
            {"name", QLatin1String(id)}, {"path", dir + "/row.csv"}, {"type", "group"}, {"bundleRoot", dir}});
    }
    expect("store the non-owned row", dsd_qt::json_store_save_array("imported_files.json", library));
}

Old
make_old(TestHost& host, const Backup& b, const QString& external) {
    Old old;
    change_library(host, b, old);
    change_references(b, old, external);
    {
        dsd_qt::SavedSystemsModel systems;
        dsd_qt::ScanListsModel scans;
        dsd_qt::DecryptionProfilesModel profiles;
        profiles.setReferences(&systems, &scans);
        expect("save an old-only profile", profiles.saveProfile(managed_profile("old-only")).value("ok").toBool());
    }
    add_not_owned_row(old);
    expect("generated scan list file", write_file(at("scan_lists/old.csv"), "id,type\n"));
    expect("unknown files",
           write_file(at("imports/notes.txt"), "notes\n") && write_file(at("imports/.staged-left"), "left over\n")
               && write_file(at("diagnostics/log.txt"), "log\n") && write_file(at("unknown.dat"), "unknown\n"));
    {
        dsd_qt::SignalSearchModel search(&host, nullptr);
        dsd_app_signal_search_permanent_avoid(400000000U);
    }
    dsd_app_signal_search_store_close();
    expect("old settings", dsd_qt::AppPrefs::replacePortable({{"rr/username", "someone"}, {"tuner/gainDb", "12"}}));

    old.groupsDest = free_name(b.groups);
    old.srcDest = free_name(b.src);
    old.targetsDest = b.targetsRoot + "-1";
    old.kept = QStringList{relative(b.groups),
                           relative(b.src),
                           "recordings/capture.wav",
                           "recordings/loose.wav",
                           "recordings/gone.wav",
                           "notes/groups.csv",
                           "imports/bundles/not owned/keep.csv",
                           "imports/bundles/CASEROOT/extra.txt",
                           relative(b.targetsRoot) + "/notes.txt",
                           "imports/notes.txt",
                           "imports/both.csv",
                           "imports/both2.csv",
                           "imports/both3.csv",
                           "imports/both4.csv",
                           "imports/.staged-left",
                           "diagnostics/log.txt",
                           "unknown.dat"};
    old.tree = capture();
    expect("the unclean recording spelling is stored as written",
           old.tree.files.value("saved_systems.json").contains("imports/sub/../both2.csv"));
    if (old.tree.files.contains("signal-search-history.v1")) {
        old.kept.append("signal-search-history.v1");
    }
    expect("old configuration compiled its profiles",
           QFileInfo(at("decryption/managed")).isDir() && QFileInfo(at("decryption/old-only")).isDir());
    expect("old tree holds the newer revision", old.tree.files.contains(relative(old.newRevision)));
    return old;
}

/** Where the backup's file at @p archivePath lands over the old configuration. */
QString
destination(const Backup& b, const Old& old, const QString& archivePath) {
    if (at(archivePath) == b.groups) {
        return relative(old.groupsDest);
    }
    if (at(archivePath).startsWith(b.targetsRoot + '/')) {
        return relative(old.targetsDest) + archivePath.mid(relative(b.targetsRoot).size());
    }
    return at(archivePath) == b.src ? relative(old.srcDest) : archivePath;
}

/** The stores of the backup with the two remapped files followed. */
QMap<QString, QJsonArray>
remapped_stores(const Backup& b, const Old& old) {
    QMap<QString, QJsonArray> stores;
    for (auto it = b.parsed.stores.cbegin(); it != b.parsed.stores.cend(); ++it) {
        stores.insert(
            it.key(),
            dsd_qt::backup::map_typed_path_values(it.key(), it.value(), [&](const dsd_qt::backup::TypedPathValue& v) {
                return v.value == b.targetsRoot ? old.targetsDest : at(destination(b, old, relative(v.value)));
            }));
    }
    return stores;
}

/** The files a clean apply leaves (the result file aside) and the settings it writes. */
Tree
expected_applied(const Backup& b, const Old& old) {
    Tree tree;
    for (const QString& name : old.kept) {
        tree.files.insert(name, old.tree.files.value(name));
    }
    tree.links.insert("imports/linked.csv", old.linkTarget);
    for (const auto& file : b.parsed.files) {
        tree.files.insert(destination(b, old, file.path), b.archive.mid(file.offset, file.size));
    }
    const auto stores = remapped_stores(b, old);
    for (auto it = stores.cbegin(); it != stores.cend(); ++it) {
        tree.files.insert(it.key(), QJsonDocument(it.value()).toJson(QJsonDocument::Compact));
    }
    tree.files.insert(kAvoidsFile, "[156800000,162550000]\n");
    tree.prefs = b.parsed.prefs;
    return tree;
}

/** The test's fault plan: which call of which step crashes or fails, and every step seen. */
struct Faults {
    QString crashStep;
    int crashCall = 0;
    QString ioStep;
    int ioCall = 0;
    QString ioStep2;
    int ioCall2 = 0;
    QMap<QString, int> calls;
    QStringList log;
    bool crashed = false;

    Fault
    operator()(const char* step) {
        const QString name = QLatin1String(step);
        const int call = ++calls[name];
        log.append(name);
        if (name == crashStep && call == crashCall) {
            crashed = true;
            return Fault::Crash;
        }
        if ((name == ioStep && call == ioCall) || (name == ioStep2 && call == ioCall2)) {
            return Fault::IoError;
        }
        return Fault::None;
    }
};

ApplyResult
apply(Faults* faults = nullptr, bool busy = false, qint64 free = -1) {
    ApplyEnv env;
    env.engineBusy = [busy] { return busy; };
    env.freeBytes = [free](const QString& path) {
        (void)path;
        return free;
    };
    if (faults != nullptr) {
        env.fault = [faults](const char* step) { return (*faults)(step); };
    }
    return dsd_qt::backup::apply_pending_restore(env);
}

/** Stage, promote and commit @p parsed over a fresh copy of @p tree. */
bool
stage_over(const dsd_qt::backup::ParseResult& parsed, const QByteArray& archive, const Tree& tree) {
    put_back(tree);
    const QString dir = dsd_qt::backup::staging_dir_for_job("job-1");
    QString error;
    const bool ok = dsd_qt::backup::stage_restore(root(), parsed, archive, dir, &error)
                    && dsd_qt::backup::promote_staging(dir) && dsd_qt::backup::commit_pending();
    expect("stage, promote and commit", ok);
    return ok;
}

/** Stage, promote and commit the backup over a fresh copy of the old configuration. */
bool
prepare_tree(const Backup& b, const Tree& tree) {
    return stage_over(b.parsed, b.archive, tree);
}

bool
prepare(const Backup& b, const Old& old) {
    return prepare_tree(b, old.tree);
}

QString
outcome_name(ApplyOutcome outcome) {
    return dsd_qt::backup::apply_outcome_name(outcome);
}

QJsonObject
read_json_object(const QString& path) {
    return QJsonDocument::fromJson(read_file(path)).object();
}

/* ---- staging ---- */

void
test_staging_layout(const Backup& b) {
    expect("staging dir for a job", dsd_qt::backup::staging_dir_for_job("abc-1_X") == at("restore-staging-abc-1_X"));
    for (const char* bad : {"", "../x", "a/b", "a b", "x.y"}) {
        expect(QStringLiteral("no staging dir for job id '%1'").arg(bad),
               dsd_qt::backup::staging_dir_for_job(bad).isEmpty());
    }
    expect("no staging dir for a 64-character job id",
           dsd_qt::backup::staging_dir_for_job(QString(64, QLatin1Char('a'))).isEmpty());

    const QString dir = dsd_qt::backup::staging_dir_for_job("layout");
    QString error;
    expect("stage succeeds",
           dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, dir, &error) && error.isEmpty());
    bool filesOk = true;
    for (const auto& file : b.parsed.files) {
        filesOk = filesOk && read_file(dir + "/files/" + file.path) == b.archive.mid(file.offset, file.size);
    }
    expect("every carried file is staged byte-identical", filesOk);
    for (const QString& store : dsd_qt::backup::backup_store_names()) {
        expect(QStringLiteral("store staged: %1").arg(store),
               QJsonDocument::fromJson(read_file(dir + "/stores/" + store)).array() == b.parsed.stores.value(store));
    }
    expect("prefs staged", read_json_object(dir + "/prefs.json").toVariantMap() == b.parsed.prefs);
    const QJsonArray avoids = QJsonDocument::fromJson(read_file(dir + "/avoids.json")).array();
    expect("avoids staged", avoids == QJsonArray({156800000, 162550000}));

    const QJsonObject manifest = read_json_object(dir + "/manifest.json");
    expect("staging version", manifest.value("stagingVersion") == QJsonValue(1));
    expect("staged summary",
           manifest.value("summary").toObject().toVariantMap()
               == QJsonObject::fromVariantMap(dsd_qt::backup::summary_to_map(b.parsed.summary)).toVariantMap());
    QSet<QString> listed;
    bool hashes = true;
    for (const auto& value : manifest.value("files").toArray()) {
        const QJsonObject entry = value.toObject();
        const QByteArray bytes = read_file(dir + '/' + entry.value("path").toString());
        listed.insert(entry.value("path").toString());
        hashes = hashes && entry.value("size").toInteger() == bytes.size()
                 && entry.value("sha256").toString() == sha256_hex(bytes);
    }
    expect("manifest sizes and hashes match", hashes);
    QSet<QString> onDisk;
    bool privateFiles = true;
    for (QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); it.hasNext();) {
        const QString path = it.next();
        onDisk.insert(path.mid(dir.size() + 1));
        const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        privateFiles = privateFiles && (QFile::permissions(path) & others) == 0;
    }
    onDisk.remove("manifest.json");
    expect("the manifest lists every staged file", listed == onDisk);
    expect("every staged file is private", privateFiles);
    expect("restaging replaces the directory", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, dir, &error));
    remove_tree(dir);
}

/** @p b's archive with the file at @p path replaced by @p bytes, size and hash updated. */
QByteArray
with_file_bytes(const QByteArray& archive, const QString& path, const QByteArray& bytes) {
    const quint32 length = qFromBigEndian<quint32>(archive.constData() + 4);
    QJsonObject manifest = QJsonDocument::fromJson(archive.mid(8, static_cast<qsizetype>(length))).object();
    QJsonArray table = manifest.value("files").toArray();
    QByteArray data;
    qsizetype offset = 8 + static_cast<qsizetype>(length);
    for (qsizetype i = 0; i < table.size(); ++i) {
        QJsonObject entry = table.at(i).toObject();
        const qsizetype size = static_cast<qsizetype>(entry.value("size").toInteger());
        QByteArray file = archive.mid(offset, size);
        offset += size;
        if (entry.value("path").toString() == path) {
            file = bytes;
            entry.insert("size", file.size());
            entry.insert("sha256", sha256_hex(file));
            table.replace(i, entry);
        }
        data.append(file);
    }
    manifest.insert("files", table);
    const QByteArray json = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    QByteArray out("DNBA");
    char size[4];
    qToBigEndian<quint32>(static_cast<quint32>(json.size()), size);
    out.append(size, 4);
    out.append(json);
    out.append(data);
    return out;
}

/** The staged store @p name in @p dir. */
QJsonArray
staged_store(const QString& dir, const char* name) {
    return QJsonDocument::fromJson(read_file(dir + "/stores/" + QLatin1String(name))).array();
}

/**
 * A library row whose file this phone cannot read is left out, not the whole backup: the row and its bundle's files
 * are not staged, what named them is cleared, the summary lists it, and the rest restores.
 */
void
test_unreadable_row_left_out(TestHost& host, const Backup& b, const QByteArray& archive) {
    put_back(Tree());
    const auto broken = dsd_qt::backup::backup_parse_archive(archive, root());
    expect("the broken archive parses", broken.error == BackupError::None);
    QString name;
    for (const auto& value : broken.stores.value("imported_files.json")) {
        if (value.toObject().value("path") == b.targets) {
            name = value.toObject().value("name").toString();
        }
    }
    const QString dir = dsd_qt::backup::staging_dir_for_job("validate");
    QString error;
    dsd_qt::backup::Summary staged;
    expect("a library file its type cannot parse leaves the backup restorable",
           dsd_qt::backup::stage_restore(root(), broken, archive, dir, &error, &staged) && error.isEmpty());
    const QJsonArray library = staged_store(dir, "imported_files.json");
    expect("the unreadable row is left out", library.size() == broken.stores.value("imported_files.json").size() - 1
                                                 && !QJsonDocument(library).toJson().contains(b.targets.toUtf8()));
    const QJsonObject manifest = read_json_object(dir + "/manifest.json");
    bool listed = false;
    for (const auto& value : manifest.value("files").toArray()) {
        listed = listed || value.toObject().value("path").toString().contains(relative(b.targetsRoot));
    }
    expect("its bundle's files are neither staged nor listed",
           !listed && !QFileInfo::exists(dir + "/files/" + relative(b.targetsRoot) + "/targets.csv"));
    expect("what named it is cleared",
           staged_store(dir, "scan_lists.json").at(1).toObject().value("targetsCsvPath") == "");
    const QString line = name + ": couldn't be read on this phone";
    expect("the summary counts and lists it", !name.isEmpty() && staged.libraryFiles == broken.summary.libraryFiles - 1
                                                  && staged.notIncluded.contains(line)
                                                  && staged.systems == broken.summary.systems);
    expect("the staged manifest carries that summary",
           manifest.value("summary").toObject().toVariantMap()
               == QJsonObject::fromVariantMap(dsd_qt::backup::summary_to_map(staged)).toVariantMap());
    expect("the rest restores", dsd_qt::backup::promote_staging(dir) && dsd_qt::backup::commit_pending()
                                    && apply().outcome == ApplyOutcome::Applied);
    expect(
        "the restore's result lists it too",
        dsd_qt::backup::read_result_file().value("summary").toMap().value("notIncluded").toStringList().contains(line));
    dsd_qt::ImportedFilesModel restored(&host);
    expect("the library loads every other row",
           restored.rowCount() == library.size() && restored.takePrunedPaths().isEmpty());
    expect("no file of the left-out bundle was restored", !QFileInfo::exists(b.targetsRoot));
}

void
test_staging_validates_library_files(TestHost& host, const Backup& b) {
    QTemporaryDir scratch;
    const QByteArray notTargets = "not,a,target,list\n";
    const QByteArray emptyGroups = "TG,Mode,Name\n";
    int accepted = -1, skipped = -1;
    expect(
        "a broken target list fails the library's own validation",
        write_file(scratch.filePath("t.csv"), notTargets)
            && !dsd_qt::ImportedFilesModel::validate(scratch.filePath("t.csv"), "trunkTargets", &accepted, &skipped));
    expect("an empty talkgroup list passes it with no usable row",
           write_file(scratch.filePath("g.csv"), emptyGroups)
               && dsd_qt::ImportedFilesModel::validate(scratch.filePath("g.csv"), "group", &accepted, &skipped)
               && accepted == 0);

    const QString dir = dsd_qt::backup::staging_dir_for_job("validate");
    QString error;
    test_unreadable_row_left_out(host, b, with_file_bytes(b.archive, relative(b.targets), notTargets));

    const QByteArray emptyArchive = with_file_bytes(b.archive, relative(b.groups), emptyGroups);
    const auto empty = dsd_qt::backup::backup_parse_archive(emptyArchive, root());
    expect("a library file with no usable row is staged as the library keeps it",
           dsd_qt::backup::stage_restore(root(), empty, emptyArchive, dir, &error));
    remove_tree(dir);

    const auto elsewhere = dsd_qt::backup::backup_parse_archive(b.archive, "/restore/elsewhere");
    expect("a parse for another data root is refused",
           !dsd_qt::backup::stage_restore(root(), elsewhere, b.archive, dir, &error) && error == "Damaged"
               && !QFileInfo::exists(dir));
    dsd_qt::backup::ParseResult failed;
    failed.error = BackupError::Damaged;
    expect("a failed parse is refused", !dsd_qt::backup::stage_restore(root(), failed, b.archive, dir, &error));
    expect("a staging dir outside the root is refused",
           !dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, scratch.filePath("restore-staging-x"), &error)
               && error == "Io" && !QFileInfo::exists(scratch.filePath("restore-staging-x")));
    expect("a directory that is not a staging dir is refused",
           !dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, at("imports"), &error) && error == "Io");
}

/** Staging works on the data root its caller read (a worker gets the UI thread's), not on a later reading. */
void
test_staging_takes_the_callers_root(const Backup& b) {
    QTemporaryDir other;
    const QString otherRoot = QDir::cleanPath(other.path());
    const auto parsed = dsd_qt::backup::backup_parse_archive(b.archive, otherRoot);
    expect("the backup parses for another root", parsed.error == BackupError::None);
    const QString dir = otherRoot + "/restore-staging-caller";
    QString error;
    expect("a parse for the caller's root stages into it",
           dsd_qt::backup::stage_restore(otherRoot, parsed, b.archive, dir, &error) && error.isEmpty()
               && QFileInfo(dir + "/manifest.json").isFile());
    expect("nothing is staged in the current root",
           QDir(root()).entryList({"restore-staging-*"}, QDir::AllEntries | QDir::Hidden).isEmpty());
    const QString here = dsd_qt::backup::staging_dir_for_job("caller");
    expect("the same parse is refused for the current root",
           !dsd_qt::backup::stage_restore(root(), parsed, b.archive, here, &error) && error == "Damaged"
               && !QFileInfo::exists(here));
}

void
test_promote_commit_discard(const Backup& b, const Old& old) {
    put_back(old.tree);
    QString error;
    const QString first = dsd_qt::backup::staging_dir_for_job("first");
    const QString second = dsd_qt::backup::staging_dir_for_job("second");
    expect("nothing pending", !dsd_qt::backup::pending_exists() && !dsd_qt::backup::pending_committed());
    expect("commit without a pending restore is refused", !dsd_qt::backup::commit_pending());
    expect("discard without a pending restore succeeds", dsd_qt::backup::discard_pending());
    expect("promote a missing staging dir fails", !dsd_qt::backup::promote_staging(first));

    expect("stage first", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, first, &error));
    expect("promote first", dsd_qt::backup::promote_staging(first) && !QFileInfo::exists(first));
    expect("pending, not committed", dsd_qt::backup::pending_exists() && !dsd_qt::backup::pending_committed());
    QJsonObject manifest = read_json_object(pending("manifest.json"));
    manifest.insert("marker", "second");
    expect("stage second", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, second, &error)
                               && write_file(second + "/manifest.json", QJsonDocument(manifest).toJson()));
    expect("promote replaces an uncommitted restore", dsd_qt::backup::promote_staging(second));
    expect("the second stage is pending", read_json_object(pending("manifest.json")).value("marker") == "second");

    expect("commit", dsd_qt::backup::commit_pending() && dsd_qt::backup::pending_committed());
    expect("COMMITTED holds the manifest's sha256",
           read_file(pending("COMMITTED")) == sha256_hex(read_file(pending("manifest.json"))).toLatin1());
    expect("committing again succeeds", dsd_qt::backup::commit_pending());
    expect("stage third", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, first, &error));
    expect("promote never replaces a committed restore", !dsd_qt::backup::promote_staging(first));
    expect("the committed restore is untouched", read_json_object(pending("manifest.json")).value("marker") == "second"
                                                     && dsd_qt::backup::pending_committed()
                                                     && QFileInfo(first).isDir());

    // The sweep removes leftover staging directories and leaves a symlink of that name alone.
    QTemporaryDir outside;
    expect("leftover staging", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, second, &error)
                                   && QFileInfo(first).isDir() && write_file(at("restore-staging-loose"), "x")
                                   && write_file(outside.filePath("keep.txt"), "keep")
                                   && QFile::link(outside.path(), at("restore-staging-link")));
    dsd_qt::backup::sweep_stale_staging();
    expect("the sweep removes staging leftovers",
           !QFileInfo::exists(first) && !QFileInfo::exists(second) && !QFileInfo::exists(at("restore-staging-loose")));
    expect("the sweep leaves a symlink and its target",
           QFileInfo(at("restore-staging-link")).isSymLink() && read_file(outside.filePath("keep.txt")) == "keep");
    expect("the sweep leaves the pending restore", dsd_qt::backup::pending_committed());
    remove_file(at("restore-staging-link"));

    expect("discard before the apply starts", dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());

    // A started apply (a complete journal) cannot be discarded; a partial journal can.
    prepare(b, old);
    Faults partial;
    partial.crashStep = "journal.files";
    partial.crashCall = 2;
    apply(&partial);
    expect("a partial journal exists",
           QFileInfo(pending("journal")).isDir() && !QFileInfo::exists(pending("journal/COMPLETE")));
    expect("discard with a partial journal", dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());
    prepare(b, old);
    Faults started;
    started.crashStep = "forward.files";
    started.crashCall = 1;
    apply(&started);
    expect("the journal is complete", QFileInfo::exists(pending("journal/COMPLETE")));
    expect("discard refused once the apply started",
           !dsd_qt::backup::discard_pending() && dsd_qt::backup::pending_committed());
    expect("committing a started apply is refused", !dsd_qt::backup::commit_pending());
    expect("the started apply completes", apply().outcome == ApplyOutcome::Applied);
}

/* ---- the apply ---- */

void
check_applied_content(const Backup& b, const Old& old) {
    const Tree got = capture();
    expect("a clean apply leaves exactly the backup and the kept files",
           same_tree(got, expected_applied(b, old), false, {kResultFile}));
    expect("decryption/ and scan_lists/ are gone",
           !QFileInfo::exists(at("decryption")) && !QFileInfo::exists(at("scan_lists")));
    expect("the newer bundle revision is gone", !QFileInfo::exists(old.newRevision));
    expect("the old-only library file is gone", !QFileInfo::exists(old.extraLibrary));
    expect("a non-owned bundle keeps everything but its row's file",
           !QFileInfo::exists(old.notOwnedRow) && QFileInfo::exists(at("imports/bundles/not owned/keep.csv")));
    expect("external and symlinked files are untouched", read_file(old.externalFile).contains("Outside")
                                                             && read_file(old.linkTarget).contains("Linked")
                                                             && QFileInfo(at("imports/linked.csv")).isSymLink());
    expect("an owned root claimed first goes; a folded second claim keeps all but its row's file",
           !QFileInfo::exists(at("imports/bundles/caseroot"))
               && !QFileInfo::exists(at("imports/bundles/CASEROOT/row.csv"))
               && QFileInfo::exists(at("imports/bundles/CASEROOT/extra.txt")));
    expect("an unreferenced bundle directory keeps its id",
           read_file(b.targetsRoot + "/notes.txt") == "not a bundle of ours\n");
    expect("the incoming bundle takes a fresh id", QFileInfo(old.targetsDest + "/targets.csv").isFile());
    const QJsonObject targetsList = dsd_qt::json_store_load_array("scan_lists.json").at(1).toObject();
    expect("the target list follows the fresh bundle",
           targetsList.value("targetsCsvPath") == old.targetsDest + "/targets.csv");
    expect("a file a live recording names is kept", read_file(at("imports/both.csv")) == "RIFF both\n");
    expect("a file a recording names by an unclean path is kept",
           read_file(at("imports/both2.csv")) == "RIFF unclean\n");
    expect("a file a recording names through a root alias is kept",
           read_file(at("imports/both3.csv")) == "RIFF alias\n");
    expect("a file a recording names in another case is kept", read_file(at("imports/both4.csv")) == "RIFF case\n");
    expect("the unknown file keeps its name", read_file(b.groups) == "my own notes\n");
    expect("the recording keeps its name", read_file(b.src) == "RIFF recording at an incoming name");
    const QJsonArray systems = dsd_qt::json_store_load_array("saved_systems.json");
    const QJsonObject metro = systems.at(0).toObject();
    expect("references follow the free names",
           metro.value("groupCsvPath") == old.groupsDest && metro.value("srcCsvPath") == old.srcDest);
    const QJsonObject patrol = dsd_qt::json_store_load_array("scan_lists.json").at(0).toObject();
    expect("scan list references follow too",
           patrol.value("groupCsvPath") == old.groupsDest && patrol.value("srcCsvPath") == old.srcDest);
    expect("pending restore removed", !QFileInfo::exists(pending()));
}

/** The real models load the restored configuration, nothing pruned, profiles compiled again. */
void
check_models_load(TestHost& host, const Backup& b, const Old& old) {
    dsd_qt::ImportedFilesModel library(&host);
    expect("the library loads every restored row", library.rowCount() == 7 && library.takePrunedPaths().isEmpty());
    expect("the library follows the free names", library.rowForPath(old.groupsDest) >= 0
                                                     && library.rowForPath(old.srcDest) >= 0
                                                     && library.rowForPath(b.chan) >= 0);
    const int targets = library.rowForPath(old.targetsDest + "/targets.csv");
    int accepted = 0, skipped = 0;
    expect("the moved target bundle is a working library row",
           targets >= 0 && library.get(targets).value("bundleRoot") == old.targetsDest
               && dsd_qt::ImportedFilesModel::validate(old.targetsDest + "/targets.csv", "trunkTargets", &accepted,
                                                       &skipped)
               && accepted == 1);
    dsd_qt::SavedSystemsModel systems;
    expect("systems restored", systems.rowCount() == 2 && systems.get(1).value("filePath").toString().isEmpty());
    dsd_qt::ScanListsModel scans;
    expect("scan lists restored", scans.rowCount() == 2);
    dsd_qt::DecryptionProfilesModel profiles;
    expect("profiles restored and compiled again", profiles.rowCount() == 3
                                                       && QFileInfo(at("decryption/managed")).isDir()
                                                       && !QFileInfo::exists(at("decryption/old-only")));
    dsd_qt::SignalSearchModel search(&host, nullptr);
    std::vector<uint32_t> avoids(DSD_SEARCH_MAX_AVOIDS);
    const size_t count = dsd_app_signal_search_avoids_get(avoids.data(), avoids.size());
    expect("the native store loads the restored avoids",
           count == 2 && avoids[0] == 156800000U && avoids[1] == 162550000U);
    dsd_app_signal_search_store_close();
}

Tree
test_clean_apply(TestHost& host, const Backup& b, const Old& old) {
    prepare(b, old);
    Faults seen;
    const ApplyResult result = apply(&seen);
    expect("a clean apply applies", result.outcome == ApplyOutcome::Applied && result.reason.isEmpty());
    expect("the result carries the summary",
           result.summary.value("systems").toInt() == 2 && result.summary.value("libraryFiles").toInt() == 7);
    for (const char* step : {"journal.begin", "journal.files", "journal.prefs", "journal.manifest", "journal.complete",
                             "forward.delete", "forward.generated", "forward.files", "forward.stores", "forward.avoids",
                             "forward.prefs", "result.write", "marker.remove", "cleanup"}) {
        expect(QStringLiteral("step consulted: %1").arg(step), seen.calls.value(step) > 0);
    }
    expect("every store is written or removed", seen.calls.value("forward.stores") == 5);
    expect("every staged file is written",
           seen.calls.value("forward.files") == static_cast<int>(b.parsed.files.size()));
    const Tree reference = capture();
    check_applied_content(b, old);
    const QVariantMap file = dsd_qt::backup::read_result_file();
    expect("the result file says applied",
           file.value("outcome") == "applied" && file.value("summary").toMap().value("systems").toInt() == 2);
    expect("the result file is private",
           (QFile::permissions(at(kResultFile)) & (QFileDevice::ReadOther | QFileDevice::ReadGroup)) == 0);
    expect("nothing more to apply", apply().outcome == ApplyOutcome::None);
    check_models_load(host, b, old);
    expect("clear the result file", dsd_qt::backup::clear_result_file() && dsd_qt::backup::read_result_file().isEmpty()
                                        && dsd_qt::backup::clear_result_file());
    expect("outcome names",
           outcome_name(ApplyOutcome::None).isEmpty() && outcome_name(ApplyOutcome::Applied) == "applied"
               && outcome_name(ApplyOutcome::Failed) == "failed" && outcome_name(ApplyOutcome::Deferred) == "deferred"
               && outcome_name(ApplyOutcome::Stuck) == "stuck");
    return reference;
}

/** @p old with the result file a Failed apply writes. */
void
check_failed_state(const char* what, const Old& old) {
    expect(QStringLiteral("%1: old state byte-identical").arg(what),
           same_tree(capture(), old.tree, true, {kResultFile}));
    const QVariantMap file = dsd_qt::backup::read_result_file();
    expect(QStringLiteral("%1: result file says failed").arg(what),
           file.value("outcome") == "failed" && file.value("reason") == "Nothing was changed");
}

/** Every step a clean apply consults, by name and call, in order. */
QList<QPair<QString, int>>
step_calls(const QStringList& log) {
    QList<QPair<QString, int>> calls;
    QMap<QString, int> counts;
    for (const QString& step : log) {
        calls.append({step, ++counts[step]});
    }
    return calls;
}

void
test_crash_at_every_step(const Backup& b, const Old& old, const Tree& reference) {
    prepare(b, old);
    Faults clean;
    apply(&clean);
    int runs = 0;
    for (const auto& call : step_calls(clean.log)) {
        prepare(b, old);
        Faults faults;
        faults.crashStep = call.first;
        faults.crashCall = call.second;
        apply(&faults);
        const ApplyResult rerun = apply();
        const QString what = QStringLiteral("crash at %1 #%2").arg(call.first).arg(call.second);
        expect(what + ": crashed", faults.crashed);
        const bool done =
            rerun.outcome == ApplyOutcome::Applied || (call.first == "cleanup" && rerun.outcome == ApplyOutcome::None);
        expect(what + ": rerun completes", done);
        expect(what + ": end state equals a clean run", same_tree(capture(), reference, true));
        runs++;
    }
    expect("every step of a clean apply was crashed", runs == clean.log.size() && runs > 30);
}

void
test_crash_during_rollback(const Backup& b, const Old& old, const Tree& reference) {
    prepare(b, old);
    Faults failing;
    failing.ioStep = "forward.files";
    failing.ioCall = 3;
    const ApplyResult failed = apply(&failing);
    expect("an I/O failure rolls back",
           failed.outcome == ApplyOutcome::Failed && failed.reason == "Nothing was changed");
    const int forward = static_cast<int>(failing.log.indexOf("rollback.delete"));
    expect("the rollback ran", forward > 0);
    if (forward <= 0) {
        return;
    }
    const auto calls = step_calls(failing.log);
    int runs = 0;
    for (qsizetype i = forward; i < calls.size(); ++i) {
        prepare(b, old);
        Faults faults;
        faults.ioStep = "forward.files";
        faults.ioCall = 3;
        faults.crashStep = calls.at(i).first;
        faults.crashCall = calls.at(i).second;
        apply(&faults);
        const QString what = QStringLiteral("rollback crash at %1 #%2").arg(calls.at(i).first).arg(calls.at(i).second);
        expect(what + ": crashed", faults.crashed);
        const ApplyResult rerun = apply();
        if (calls.at(i).first == "cleanup") {
            // The marker was gone: the rollback's Failed is final.
            expect(what + ": rerun finds nothing", rerun.outcome == ApplyOutcome::None);
            check_failed_state(qUtf8Printable(what), old);
        } else {
            // The marker was still there: the next start rolls forward.
            expect(what + ": rerun rolls forward", rerun.outcome == ApplyOutcome::Applied);
            expect(what + ": end state equals a clean run", same_tree(capture(), reference, true));
        }
        runs++;
    }
    expect("every rollback step was crashed", runs > 10);
}

void
test_io_failures_roll_back(const Backup& b, const Old& old) {
    const QList<QPair<QString, int>> failures{{"forward.delete", 1}, {"forward.delete", 9}, {"forward.generated", 1},
                                              {"forward.files", 1},  {"forward.files", 7},  {"forward.stores", 1},
                                              {"forward.stores", 4}, {"forward.avoids", 1}, {"forward.prefs", 1}};
    for (const auto& failure : failures) {
        prepare(b, old);
        Faults faults;
        faults.ioStep = failure.first;
        faults.ioCall = failure.second;
        const ApplyResult result = apply(&faults);
        const QString what = QStringLiteral("I/O failure at %1 #%2").arg(failure.first).arg(failure.second);
        expect(what + ": Failed", result.outcome == ApplyOutcome::Failed && result.reason == "Nothing was changed");
        check_failed_state(qUtf8Printable(what), old);
        expect(what + ": no free name left behind",
               !QFileInfo::exists(old.groupsDest) && !QFileInfo::exists(old.srcDest));
        expect(what + ": nothing pending", !dsd_qt::backup::pending_exists());
    }
}

void
test_stuck(const Backup& b, const Old& old, const Tree& reference) {
    for (const char* rollbackStep : {"rollback.delete", "rollback.files", "rollback.prefs"}) {
        prepare(b, old);
        Faults faults;
        faults.ioStep = "forward.files";
        faults.ioCall = 4;
        faults.ioStep2 = rollbackStep;
        faults.ioCall2 = 1;
        const ApplyResult stuck = apply(&faults);
        const QString what = QStringLiteral("rollback failure at %1").arg(rollbackStep);
        expect(what + ": Stuck", stuck.outcome == ApplyOutcome::Stuck
                                     && stuck.reason == "Restore couldn't finish. Free up storage and reopen DSD-neo");
        expect(what + ": everything kept",
               dsd_qt::backup::pending_committed() && QFileInfo::exists(pending("journal/COMPLETE")));
        expect(what + ": next start completes", apply().outcome == ApplyOutcome::Applied);
        expect(what + ": end state equals a clean run", same_tree(capture(), reference, true));
    }
    prepare(b, old);
    Faults marker;
    marker.ioStep = "marker.remove";
    marker.ioCall = 1;
    expect("a commit point failure is Stuck", apply(&marker).outcome == ApplyOutcome::Stuck);
    expect("the commit point failure keeps the restore", dsd_qt::backup::pending_committed());
    expect("the next start completes it", apply().outcome == ApplyOutcome::Applied);
    expect("the completed end state equals a clean run", same_tree(capture(), reference, true));
}

void
test_failed_before_any_change(const Backup& b, const Old& old) {
    // An unsupported staging version, with a marker that matches it.
    prepare(b, old);
    QJsonObject manifest = read_json_object(pending("manifest.json"));
    manifest.insert("stagingVersion", 2);
    const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    write_file(pending("manifest.json"), bytes);
    write_file(pending("COMMITTED"), sha256_hex(bytes).toLatin1());
    ApplyResult result = apply();
    expect("an unsupported staging version fails", result.outcome == ApplyOutcome::Failed);
    check_failed_state("unsupported staging version", old);
    expect("the stage is removed", !dsd_qt::backup::pending_exists());

    prepare(b, old);
    write_file(pending("COMMITTED"), QByteArray(64, 'a'));
    expect("a marker for another manifest fails", apply().outcome == ApplyOutcome::Failed);
    check_failed_state("corrupt marker", old);

    prepare(b, old);
    write_file(pending("COMMITTED"), "not a hash");
    expect("a garbled marker fails", apply().outcome == ApplyOutcome::Failed);
    check_failed_state("garbled marker", old);

    // The failed stage loses its marker before its files: a stage that cannot be removed is never run again.
    prepare(b, old);
    write_file(pending("COMMITTED"), "not a hash");
    Faults stays;
    stays.ioStep = "cleanup";
    stays.ioCall = 1;
    expect("a failed stage whose removal fails is still Failed", apply(&stays).outcome == ApplyOutcome::Failed);
    expect("the failed stage is no longer committed",
           dsd_qt::backup::pending_exists() && !dsd_qt::backup::pending_committed());
    expect("the next start only removes it",
           apply().outcome == ApplyOutcome::None && !dsd_qt::backup::pending_exists());
    expect("the failed result stays", dsd_qt::backup::read_result_file().value("outcome") == "failed");

    for (const QString& staged :
         {QStringLiteral("files/") + b.parsed.files.first().path, QStringLiteral("stores/saved_systems.json"),
          QStringLiteral("prefs.json"), QStringLiteral("avoids.json")}) {
        prepare(b, old);
        QByteArray content = read_file(pending(staged));
        content[0] = static_cast<char>(content.at(0) ^ 0x20);
        write_file(pending(staged), content);
        expect(QStringLiteral("a corrupt staged %1 fails").arg(staged), apply().outcome == ApplyOutcome::Failed);
        check_failed_state(qUtf8Printable("corrupt " + staged), old);
    }
    prepare(b, old);
    remove_file(pending("files/" + b.parsed.files.last().path));
    expect("a missing staged file fails", apply().outcome == ApplyOutcome::Failed);
    check_failed_state("missing staged file", old);

    // A staging that no longer verifies after the journal is complete rolls back.
    prepare(b, old);
    Faults crash;
    crash.crashStep = "forward.files";
    crash.crashCall = 3;
    apply(&crash);
    write_file(pending("files/" + b.parsed.files.last().path), "changed");
    result = apply();
    expect("a staging that changed after the journal rolls back", result.outcome == ApplyOutcome::Failed);
    check_failed_state("staging changed after the journal", old);
    expect("nothing pending after the rollback", !dsd_qt::backup::pending_exists());

    // A staging that still verifies but is not the one the journal planned for rolls back too.
    prepare(b, old);
    Faults again;
    again.crashStep = "forward.files";
    again.crashCall = 3;
    apply(&again);
    QJsonObject replanned = read_json_object(pending("manifest.json"));
    QJsonArray entries = replanned.value("files").toArray();
    const QString dropped = entries.at(0).toObject().value("path").toString();
    entries.removeAt(0);
    replanned.insert("files", entries);
    const QByteArray replannedBytes = QJsonDocument(replanned).toJson(QJsonDocument::Compact);
    expect("re-plan the staging", dropped.startsWith("files/") && remove_file(pending(dropped))
                                      && write_file(pending("manifest.json"), replannedBytes)
                                      && write_file(pending("COMMITTED"), sha256_hex(replannedBytes).toLatin1()));
    expect("a staging the journal did not plan for rolls back", apply().outcome == ApplyOutcome::Failed);
    check_failed_state("staging not the journal's", old);
}

void
test_deferred(const Backup& b, const Old& old, const Tree& reference) {
    prepare(b, old);
    Tree before = capture();
    ApplyResult result = apply(nullptr, true);
    expect("a busy engine defers", result.outcome == ApplyOutcome::Deferred && !result.reason.isEmpty());
    expect("a deferred apply touches nothing", same_tree(capture(), before, true));
    expect("deferred keeps the summary", result.summary.value("systems").toInt() == 2);

    result = apply(nullptr, false, Q_INT64_C(1024) * 1024);
    expect("low free space defers", result.outcome == ApplyOutcome::Deferred);
    expect("a space deferral touches nothing", same_tree(capture(), before, true));

    Faults crash;
    crash.crashStep = "forward.stores";
    crash.crashCall = 2;
    apply(&crash);
    before = capture();
    expect("a busy engine defers a started apply too", apply(nullptr, true).outcome == ApplyOutcome::Deferred);
    expect("a deferred recovery touches nothing", same_tree(capture(), before, true));
    expect("free space is not asked once the journal is complete",
           apply(nullptr, false, 1).outcome == ApplyOutcome::Applied);
    expect("the deferred apply completes like a clean run", same_tree(capture(), reference, true));

    prepare(b, old);
    expect("plenty of space applies", apply(nullptr, false, Q_INT64_C(1) << 40).outcome == ApplyOutcome::Applied);
}

/** A journal that cannot be written changes nothing live: the restore is deferred and the next start tries again. */
void
test_journal_failures(const Backup& b, const Old& old, const Tree& reference) {
    for (const char* step :
         {"journal.begin", "journal.files", "journal.prefs", "journal.manifest", "journal.complete"}) {
        prepare(b, old);
        const Tree before = capture();
        Faults faults;
        faults.ioStep = step;
        faults.ioCall = 1;
        const ApplyResult result = apply(&faults);
        const QString what = QStringLiteral("journal failure at %1").arg(step);
        expect(what + ": Deferred for storage", result.outcome == ApplyOutcome::Deferred
                                                    && result.reason
                                                           == "Not enough free storage to restore. Free up storage and "
                                                              "reopen DSD-neo");
        expect(what + ": nothing changed, no journal left", same_tree(capture(), before, true));
        expect(what + ": the next start applies", apply().outcome == ApplyOutcome::Applied);
        expect(what + ": end state equals a clean run", same_tree(capture(), reference, true));
    }
}

/** The engine turning busy once the journal is written still stops the apply before the first live change. */
void
test_busy_after_journal(const Backup& b, const Old& old, const Tree& reference) {
    prepare(b, old);
    int calls = 0;
    ApplyEnv env;
    env.engineBusy = [&calls] { return ++calls > 1; };
    const ApplyResult result = dsd_qt::backup::apply_pending_restore(env);
    expect("busy after the journal defers", result.outcome == ApplyOutcome::Deferred && calls == 2);
    expect("a journal no change used is dropped",
           !QFileInfo::exists(pending("journal")) && dsd_qt::backup::pending_committed());
    expect("nothing live changed", same_tree(capture(), old.tree, true, {"restore-pending"}));
    Faults next;
    expect("the next start applies", apply(&next).outcome == ApplyOutcome::Applied);
    expect("the next start plans from the live configuration again", next.calls.value("journal.begin") == 1);
    expect("busy after the journal ends like a clean run", same_tree(capture(), reference, true));

    // A deferred session can be discarded: its journal is gone.
    prepare(b, old);
    calls = 0;
    dsd_qt::backup::apply_pending_restore(env);
    expect("a restore deferred after its journal can be discarded",
           dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());
}

void
test_unmarked_pending(const Backup& b, const Old& old) {
    put_back(old.tree);
    const QString dir = dsd_qt::backup::staging_dir_for_job("unmarked");
    QString error;
    expect("stage and promote", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, dir, &error)
                                    && dsd_qt::backup::promote_staging(dir));
    const ApplyResult result = apply();
    expect("an unmarked restore is removed", result.outcome == ApplyOutcome::None && !dsd_qt::backup::pending_exists());
    expect("an unmarked restore changes nothing", same_tree(capture(), old.tree, true));
    expect("nothing pending is None", apply().outcome == ApplyOutcome::None);
}

/** imports/bundles replaced by a symlink to a directory outside the root holding the same names. */
void
test_symlinked_component(const Backup& b, const Old& old) {
    QTemporaryDir outside;
    prepare(b, old);
    const QString bundles = at("imports/bundles");
    for (QDirIterator it(bundles, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); it.hasNext();) {
        const QString file = it.next();
        write_file(outside.path() + file.mid(bundles.size()), read_file(file));
    }
    remove_tree(bundles);
    expect("bundles directory is a symlink", QFile::link(outside.path(), bundles));
    const Tree before = capture();
    const Tree external = capture_dir(outside.path());
    expect("the external directory holds incoming names", external.files.size() >= 6);
    const ApplyResult result = apply();
    expect("a symlinked destination component fails the apply", result.outcome == ApplyOutcome::Failed);
    expect("the files outside are untouched", same_tree(capture_dir(outside.path()), external, true));
    expect("the old state is back", same_tree(capture(), before, false, {kResultFile, "restore-pending"}));
    expect("nothing pending after the refusal", !dsd_qt::backup::pending_exists());
    remove_file(bundles);
}

/** A store file that is a symlink is no store of ours: the apply cannot replace it, so it fails cleanly. */
void
test_symlinked_store(const Backup& b, const Old& old) {
    QTemporaryDir outside;
    prepare(b, old);
    const QString store = at("signal-search-ranges.json");
    expect("a symlinked store file", write_file(outside.filePath("ranges.json"), "[]") && remove_file(store)
                                         && QFile::link(outside.filePath("ranges.json"), store));
    const Tree before = capture();
    const ApplyResult result = apply();
    expect("a symlinked store fails the apply cleanly", result.outcome == ApplyOutcome::Failed);
    expect("the symlink and its target are untouched",
           QFileInfo(store).isSymLink() && read_file(outside.filePath("ranges.json")) == "[]");
    expect("the old state is back after a symlinked store",
           same_tree(capture(), before, true, {kResultFile, "restore-pending"}));
    expect("nothing pending after a symlinked store", !dsd_qt::backup::pending_exists());
}

/** After the commit point a journal is only a leftover: discarding it is allowed. */
void
test_discard_after_commit_point(const Backup& b, const Old& old) {
    prepare(b, old);
    Faults faults;
    faults.crashStep = "cleanup";
    faults.crashCall = 1;
    apply(&faults);
    expect("a crash in the cleanup leaves an unmarked journal", dsd_qt::backup::pending_exists()
                                                                    && !dsd_qt::backup::pending_committed()
                                                                    && QFileInfo::exists(pending("journal/COMPLETE")));
    expect("a leftover after the commit point can be discarded",
           dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());
}

/** Discarding drops the marker first: a stage it cannot remove whole is never applied, and one whose marker cannot be
    removed is left whole. */
void
test_discard_order(const Backup& b, const Old& old) {
    const auto lock = [](const QString& dir, bool locked) {
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner
                                       | (locked ? QFileDevice::Permissions() : QFileDevice::WriteOwner));
    };
    prepare(b, old);
    const QString files = pending("files");
    lock(files, true);
    QFile probe(files + "/probe");
    if (probe.open(QIODevice::WriteOnly)) {
        lock(files, false);
        DSD_FPRINTF(stderr, "note: directory permissions are not enforced here; discard-order cases skipped\n");
        return;
    }
    expect("a discard that cannot remove every file reports it", !dsd_qt::backup::discard_pending());
    expect("the stage it could not remove is no longer committed",
           dsd_qt::backup::pending_exists() && !dsd_qt::backup::pending_committed());
    lock(files, false);
    expect("the next start only removes it",
           apply().outcome == ApplyOutcome::None && !dsd_qt::backup::pending_exists());

    // The marker itself cannot go: nothing goes, so the stage is still the one the user confirmed.
    prepare(b, old);
    const Tree before = capture_dir(pending());
    lock(pending(), true);
    expect("a discard that cannot remove the marker reports it", !dsd_qt::backup::discard_pending());
    lock(pending(), false);
    expect("a stage whose marker stays is left whole", same_tree(capture_dir(pending()), before, true));
    expect("and it still applies", apply().outcome == ApplyOutcome::Applied);
}

/** Only a regular COMMITTED file marks a stage, as the apply reads it: anything else of that name marks nothing. */
void
test_marker_must_be_a_file(const Backup& b, const Old& old) {
    const auto unmark = [] {
        return remove_file(pending("COMMITTED")) && QDir().mkdir(pending("COMMITTED"))
               && QFileInfo(pending("COMMITTED")).isDir();
    };
    prepare(b, old);
    Faults faults;
    faults.crashStep = "forward.delete";
    faults.crashCall = 1;
    apply(&faults);
    expect("a complete journal, nothing live changed yet",
           dsd_qt::backup::pending_apply_started() && same_tree(capture(), old.tree, true, {"restore-pending"}));
    expect("the marker becomes a directory", unmark());
    expect("a stage whose marker is a directory is neither committed nor started",
           !dsd_qt::backup::pending_committed() && !dsd_qt::backup::pending_apply_started());
    expect("it is discarded whole", dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());

    put_back(old.tree);
    QString error;
    const QString dir = dsd_qt::backup::staging_dir_for_job("unmarked");
    expect("a stage whose marker is a directory",
           dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, dir, &error)
               && dsd_qt::backup::promote_staging(dir) && QDir().mkdir(pending("COMMITTED")));
    expect("a newer stage replaces it", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, dir, &error)
                                            && dsd_qt::backup::promote_staging(dir)
                                            && !QFileInfo::exists(pending("COMMITTED")));
    expect("discard it", dsd_qt::backup::discard_pending());
}

/** The archive bytes of the carried file at @p path. */
QByteArray
archive_bytes(const Backup& b, const QString& path) {
    for (const auto& file : b.parsed.files) {
        if (at(file.path) == path) {
            return b.archive.mid(file.offset, file.size);
        }
    }
    return {};
}

/** @p tree with the directory @p dir (and everything below it) replaced by a regular file. */
Tree
with_file_for_dir(Tree tree, const QString& dir) {
    tree = without(tree, {dir});
    tree.files.insert(dir, "an old file where a directory goes\n");
    return tree;
}

/** An old file where the backup needs a directory: incoming files go around it, and a rollback puts it back. */
void
test_old_file_where_a_directory_goes(const Backup& b, const Old& old) {
    const QString dir = relative(QFileInfo(b.extra).path());
    Tree variant = with_file_for_dir(old.tree, dir);
    QByteArray lists = variant.files.value("scan_lists.json");
    lists.replace(b.extra.toUtf8(), at(dir).toUtf8()); // a live path field names the file, so it is old configuration
    variant.files.insert("scan_lists.json", lists);

    prepare_tree(b, variant);
    expect("an old file where a directory goes: applied", apply().outcome == ApplyOutcome::Applied);
    const QString moved = at(dir + " (1)/notes.csv");
    expect("the incoming file goes around the old file", read_file(moved) == archive_bytes(b, b.extra));
    expect("the old file was old configuration", !QFileInfo::exists(at(dir)));
    expect("the list follows the moved file",
           dsd_qt::json_store_load_array("scan_lists.json").at(1).toObject().value("groupCsvPath") == moved);

    prepare_tree(b, variant);
    Faults faults;
    faults.ioStep = "forward.stores";
    faults.ioCall = 1;
    expect("an old file where a directory goes: rolled back", apply(&faults).outcome == ApplyOutcome::Failed);
    expect("the rollback restores the old file", same_tree(capture(), variant, true, {kResultFile}));

    // The same inside a bundle the old configuration owns: the incoming bundle takes a fresh id.
    QString revision;
    for (const auto& file : b.parsed.files) {
        const QString rest = at(file.path).mid(b.chanRoot.size() + 1);
        if (at(file.path).startsWith(b.chanRoot + '/') && rest.contains('/')) {
            revision = relative(b.chanRoot) + '/' + rest.section('/', 0, 0);
        }
    }
    expect("the channel bundle has a revision directory", !revision.isEmpty());
    const Tree bundled = with_file_for_dir(old.tree, revision);
    prepare_tree(b, bundled);
    expect("an old file inside a bundle path: applied", apply().outcome == ApplyOutcome::Applied);
    const QString movedChan = b.chanRoot + "-1" + b.chan.mid(b.chanRoot.size());
    expect("the incoming bundle goes around the old file", read_file(movedChan) == archive_bytes(b, b.chan));
    expect("the system follows the moved bundle",
           dsd_qt::json_store_load_array("saved_systems.json").at(0).toObject().value("chanCsvPath") == movedChan);
    prepare_tree(b, bundled);
    Faults bundleFaults;
    bundleFaults.ioStep = "forward.stores";
    bundleFaults.ioCall = 1;
    expect("an old file inside a bundle path: rolled back", apply(&bundleFaults).outcome == ApplyOutcome::Failed);
    expect("the rollback restores the bundle's old file", same_tree(capture(), bundled, true, {kResultFile}));

    // An old file at imports/bundles itself, which no bundle can move around: the restore replaces it (it is old
    // configuration), and a rollback removes the directory it made there before putting the file back.
    Tree flat = with_file_for_dir(old.tree, "imports/bundles");
    QByteArray named = flat.files.value("scan_lists.json");
    named.replace(b.extra.toUtf8(), at("imports/bundles").toUtf8());
    flat.files.insert("scan_lists.json", named);
    prepare_tree(b, flat);
    expect("an old file at imports/bundles: applied", apply().outcome == ApplyOutcome::Applied);
    expect("the bundles directory replaces the old file", read_file(b.chan) == archive_bytes(b, b.chan));
    prepare_tree(b, flat);
    Faults flatFaults;
    flatFaults.ioStep = "forward.stores";
    flatFaults.ioCall = 1;
    expect("an old file at imports/bundles: rolled back", apply(&flatFaults).outcome == ApplyOutcome::Failed);
    expect("the rollback puts the old file back at imports/bundles", same_tree(capture(), flat, true, {kResultFile}));
}

/** A journal that cannot be built for a reason other than storage fails with that reason; nothing is changed. */
void
test_journal_problems(const Backup& b, const Old& old) {
    prepare(b, old);
    const QString unreadable = at("imports/keys.csv");
    QFile::setPermissions(unreadable, QFileDevice::Permissions());
    QFile probe(unreadable);
    if (probe.open(QIODevice::ReadOnly)) {
        DSD_FPRINTF(stderr, "note: file permissions are not enforced here; unreadable-file case skipped\n");
    } else {
        const ApplyResult result = apply();
        expect("an unreadable old file fails the restore",
               result.outcome == ApplyOutcome::Failed
                   && result.reason == "A file of the current configuration couldn't be read. Nothing was changed");
        expect("an unreadable old file leaves nothing pending", !dsd_qt::backup::pending_exists());
        QFile::setPermissions(unreadable, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        expect("an unreadable old file changes nothing", same_tree(capture(), old.tree, true, {kResultFile}));
        expect("the result says why", dsd_qt::backup::read_result_file().value("reason") == result.reason);
    }
    QFile::setPermissions(unreadable, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    // Every free name for an incoming file taken.
    prepare(b, old);
    const QString base = QFileInfo(b.groups).completeBaseName();
    bool written = true;
    for (int n = 1; n < 10000 && written; ++n) {
        QFile file(QStringLiteral("%1/%2 (%3).csv").arg(QFileInfo(b.groups).path(), base).arg(n));
        written = file.open(QIODevice::WriteOnly);
    }
    expect("every free name taken", written);
    const ApplyResult names = apply();
    expect("no free name fails the restore",
           names.outcome == ApplyOutcome::Failed
               && names.reason == "No free name was left for a restored file. Nothing was changed");
    expect("no free name leaves nothing pending", !dsd_qt::backup::pending_exists());
    expect("no free name changes nothing",
           read_file(b.groups) == "my own notes\n"
               && read_file(at("saved_systems.json")) == old.tree.files.value("saved_systems.json"));
}

/** An unreadable journal is Stuck, and says so in the result file too. */
void
test_unreadable_journal(const Backup& b, const Old& old) {
    prepare(b, old);
    Faults crash;
    crash.crashStep = "forward.files";
    crash.crashCall = 2;
    apply(&crash);
    write_file(at(kResultFile), R"({"outcome":"applied","reason":"","summary":{}})");
    write_file(pending("journal/COMPLETE"), QByteArray(64, '0'));
    const ApplyResult result = apply();
    expect("an unreadable journal is Stuck", result.outcome == ApplyOutcome::Stuck);
    expect("an unreadable journal's result file says stuck",
           dsd_qt::backup::read_result_file().value("outcome") == "stuck");
    expect("an unreadable journal keeps everything", dsd_qt::backup::pending_committed());
}

/** Symlinks to an outside file and an outside directory inside @p dir. */
bool
add_links(const QString& dir, const QString& outside) {
    return QDir().mkpath(dir) && QFile::link(outside + "/keep.txt", dir + "/to-file")
           && QFile::link(outside + "/dir", dir + "/to-dir");
}

/** Trees a restore removes may hold symlinks: each link is unlinked, never followed, and its target never touched. */
void
test_links_inside_removed_trees(const Backup& b, const Old& old, const Tree& reference) {
    QTemporaryDir outsideDir;
    const QString outside = outsideDir.path();
    expect("outside files",
           write_file(outside + "/keep.txt", "keep\n") && write_file(outside + "/dir/inner.txt", "in\n"));
    const Tree external = capture_dir(outside);
    const auto untouched = [&](const QString& what) {
        expect(what + ": the link targets are untouched", same_tree(capture_dir(outside), external, true));
    };
    put_back(old.tree);
    QString error;
    const QString dir = dsd_qt::backup::staging_dir_for_job("linked");
    const QString other = dsd_qt::backup::staging_dir_for_job("other");
    expect("staging dirs for the link cases", !dir.isEmpty() && !other.isEmpty());
    if (dir.isEmpty() || other.isEmpty()) {
        return;
    }
    const auto stagePending = [&]() {
        return dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, dir, &error)
               && dsd_qt::backup::promote_staging(dir) && add_links(pending("files/imports"), outside);
    };
    expect("an unmarked restore holding links", stagePending());
    expect("an unmarked restore holding links is removed",
           apply().outcome == ApplyOutcome::None && !dsd_qt::backup::pending_exists());
    untouched("unmarked restore");

    expect("a restore to discard holding links", stagePending());
    expect("a restore holding links is discarded",
           dsd_qt::backup::discard_pending() && !dsd_qt::backup::pending_exists());
    untouched("discard");

    expect("a restore to replace holding links", stagePending());
    expect("a restore holding links is replaced",
           dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, other, &error)
               && dsd_qt::backup::promote_staging(other) && !QFileInfo::exists(pending("files/imports/to-dir")));
    untouched("promote");
    expect("discard the replacement", dsd_qt::backup::discard_pending());

    expect("a stale staging dir holding links",
           dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, other, &error)
               && add_links(other + "/files", outside));
    dsd_qt::backup::sweep_stale_staging();
    expect("the sweep removes a staging dir holding links", !QFileInfo::exists(other));
    expect("a staging dir holding links", dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, other, &error)
                                              && add_links(other + "/files", outside));
    expect("restaging over a staging dir holding links",
           dsd_qt::backup::stage_restore(root(), b.parsed, b.archive, other, &error)
               && !QFileInfo::exists(other + "/files/to-dir"));
    remove_tree(other);
    untouched("staging");

    // A journal left by an earlier start that holds links, or is a link or a file itself, is rebuilt.
    for (const QString& shape : {QStringLiteral("holding links"), QStringLiteral("a link"), QStringLiteral("a file")}) {
        prepare(b, old);
        Faults crash;
        crash.crashStep = "journal.files";
        crash.crashCall = 2;
        apply(&crash);
        const QString journal = pending("journal");
        bool made = false;
        if (shape == QLatin1String("holding links")) {
            made = add_links(journal + "/files", outside);
        } else {
            remove_tree(journal);
            made = shape == QLatin1String("a link") ? QFile::link(outside + "/dir", journal)
                                                    : write_file(journal, "not a directory\n");
        }
        const QString what = QStringLiteral("a journal ") + shape;
        expect(what + ": made", made);
        expect(what + ": rebuilt and applied", apply().outcome == ApplyOutcome::Applied);
        expect(what + ": ends like a clean run", same_tree(capture(), reference, true));
        untouched(what);
    }
}

/** A journal left over that cannot be removed fails the restore with that reason, not as a storage shortage. */
void
test_journal_leftover(const Backup& b, const Old& old) {
    prepare(b, old);
    Faults crash;
    crash.crashStep = "journal.files";
    crash.crashCall = 2;
    apply(&crash);
    const QString locked = pending("journal/files");
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QFile probe(locked + "/probe");
    if (probe.open(QIODevice::WriteOnly)) {
        DSD_FPRINTF(stderr, "note: directory permissions are not enforced here; leftover-journal case skipped\n");
    } else {
        const ApplyResult result = apply();
        expect("a journal that cannot be removed fails the restore",
               result.outcome == ApplyOutcome::Failed
                   && result.reason
                          == "Files left by an earlier restore attempt couldn't be removed. Nothing was changed");
        expect("the failed restore is no longer committed", !dsd_qt::backup::pending_committed());
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        expect("the leftover goes once it can",
               apply().outcome == ApplyOutcome::None && !dsd_qt::backup::pending_exists());
    }
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

/** An empty avoid list is written in the native format and loads in the native store. */
void
test_empty_avoids(TestHost& host, const Backup& b, const Old& old) {
    put_back(old.tree);
    dsd_qt::backup::ParseResult parsed = b.parsed;
    parsed.avoids.clear();
    const QString dir = dsd_qt::backup::staging_dir_for_job("empty");
    QString error;
    expect("stage an empty avoid list", dsd_qt::backup::stage_restore(root(), parsed, b.archive, dir, &error)
                                            && dsd_qt::backup::promote_staging(dir)
                                            && dsd_qt::backup::commit_pending());
    expect("apply it", apply().outcome == ApplyOutcome::Applied);
    expect("the avoid file holds an empty list", read_file(at(kAvoidsFile)) == "[]\n");
    dsd_qt::SignalSearchModel search(&host, nullptr);
    uint32_t avoids[4] = {};
    expect("the native store loads none", dsd_app_signal_search_avoids_get(avoids, 4) == 0);
    char problem[256] = {};
    dsd_signal_search_status status{};
    dsd_app_signal_search_status_get(&status, problem, sizeof problem);
    expect("the native store takes the empty list", problem[0] == '\0');
    dsd_app_signal_search_store_close();
}

/** A backup of one channel map kept beside its bundle, and the configuration it was exported from. */
struct Beside {
    QByteArray archive;
    dsd_qt::backup::ParseResult parsed;
    Tree tree;
    QString map, bundle;
};

/** A plain channel map updated in place with companions keeps its file in imports/ and names them by the bundle's id. */
Beside
make_beside(TestHost& host) {
    Beside beside;
    put_back(Tree());
    {
        dsd_qt::ImportedFilesModel library(&host);
        QTemporaryDir source;
        expect("write the beside fixtures",
               write_file(source.filePath("plain.csv"), "channel,frequency_hz,mode\n1,461000000,dmr\n")
                   && write_file(source.filePath("keys.csv"), "keyid,value\n02,ABCDE\n")
                   && write_file(source.filePath("channels.csv"),
                                 "channel,frequency_hz,mode,keys_hex_csv\n1,461000000,dmr,keys.csv\n"));
        QVariantMap result = library.importBundle(source.filePath("plain.csv"), "Plain.csv", "chan", {});
        const int row = library.rowForPath(result.value("path").toString());
        result = library.importBundle(source.filePath("channels.csv"), "Plain.csv", "chan", {}, row);
        expect("the plain map is updated with companions", result.value("ok").toBool());
        beside.map = result.value("path").toString();
        beside.bundle = library.get(row).value("bundleRoot").toString();
    }
    expect("the map stays beside its bundle and names it", !beside.bundle.isEmpty()
                                                               && !beside.map.startsWith(beside.bundle + '/')
                                                               && read_file(beside.map).contains("bundles/"));
    const auto built = dsd_qt::backup::backup_build_archive(dsd_qt::backup::backup_take_snapshot(), nullptr);
    beside.archive = built.archive;
    beside.parsed = dsd_qt::backup::backup_parse_archive(beside.archive, root());
    expect("the beside backup builds and parses",
           built.error == BackupError::None && beside.parsed.error == BackupError::None);
    beside.tree = capture();
    return beside;
}

/** Whether the restored map's companions load, as the library checks them. */
bool
map_loads(const Beside& beside) {
    int accepted = 0, skipped = 0;
    return dsd_qt::ImportedFilesModel::validate(beside.map, "chan", &accepted, &skipped) && accepted == 1;
}

/**
 * A bundle a map beside it names by its id cannot take a free name, since the restore never rewrites the map: when an
 * unowned entry holds its place the restore fails before any change. Where the place is free or owned, it restores.
 */
void
test_bundle_named_from_outside(TestHost& host, const Old& old) {
    const Beside beside = make_beside(host);

    stage_over(beside.parsed, beside.archive, beside.tree);
    expect("over its own configuration: applied", apply().outcome == ApplyOutcome::Applied);
    expect("over its own configuration: the map loads", map_loads(beside));

    stage_over(beside.parsed, beside.archive, old.tree);
    expect("over another configuration: applied", apply().outcome == ApplyOutcome::Applied);
    expect("over another configuration: the bundle keeps its id and the map loads",
           QFileInfo(beside.bundle).isDir() && map_loads(beside));

    Tree blocked = old.tree;
    blocked.dirs.insert(relative(beside.bundle));
    blocked.files.insert(relative(beside.bundle) + "/stray.txt", "left by an earlier import\n");
    stage_over(beside.parsed, beside.archive, blocked);
    const ApplyResult result = apply();
    const QString reason = "A folder left by an earlier import is in the way of a restored channel map. To restore "
                           "this backup, clear DSD-neo's storage first. Nothing was changed";
    expect("a stray entry where a named bundle goes fails the restore",
           result.outcome == ApplyOutcome::Failed && result.reason == reason);
    expect("the failed restore changes nothing", same_tree(capture(), blocked, true, {kResultFile}));
    expect("the failed restore leaves nothing pending", !dsd_qt::backup::pending_exists());
    expect("the result says why", dsd_qt::backup::read_result_file().value("reason") == reason);
    expect("no bundle took a fresh id", !QFileInfo::exists(beside.bundle + "-1"));
}

/**
 * Two restores in a row: B, exported from a restored A, is restored over it. The second restore's journal owns
 * exactly what the first restore wrote (the moved "-1" bundle and the files with free names included), the stores and
 * the generated folders; its only owned roots are A's two bundles, so no wider root gains deletion authority, and
 * every other file stays byte-identical.
 */
void
test_two_restores_in_a_row(const Backup& b, const Old& old, const Tree& restored) {
    put_back(restored);
    QSet<QString> written; // what the first restore wrote
    for (const auto& file : b.parsed.files) {
        written.insert(destination(b, old, file.path));
    }
    expect("the first restore moved a bundle and renamed files",
           written.contains(relative(old.groupsDest)) && written.contains(relative(old.srcDest))
               && written.contains(relative(old.targetsDest) + "/targets.csv"));
    {
        dsd_qt::DecryptionProfilesModel profiles; // the first start compiles the restored profiles into decryption/
    }
    const Tree restoredA = capture();
    expect("the restored profiles were compiled", QFileInfo(at("decryption/managed")).isDir());
    {
        dsd_qt::ScanListsModel scans;
        expect("B drops the list only a scan list named", scans.remove(1));
    }
    const auto built = dsd_qt::backup::backup_build_archive(dsd_qt::backup::backup_take_snapshot(), nullptr);
    const auto parsed = dsd_qt::backup::backup_parse_archive(built.archive, root());
    expect("B builds and parses", built.error == BackupError::None && parsed.error == BackupError::None);
    expect("B no longer carries the dropped list",
           std::none_of(parsed.files.cbegin(), parsed.files.cend(),
                        [&](const dsd_qt::backup::ArchiveFile& file) { return at(file.path) == b.extra; }));

    QSet<QString> expected = written;
    for (auto it = restoredA.files.cbegin(); it != restoredA.files.cend(); ++it) {
        const QString& name = it.key();
        if (name.startsWith("decryption/") || name.startsWith("scan_lists/")
            || QStringList{"saved_systems.json", "scan_lists.json", "imported_files.json", "decryption_profiles.json",
                           "signal-search-ranges.json", kAvoidsFile}
                   .contains(name)) {
            expected.insert(name);
        }
    }

    stage_over(parsed, built.archive, restoredA);
    Faults stop;
    stop.crashStep = "forward.delete";
    stop.crashCall = 1;
    apply(&stop);
    const QJsonObject journal = read_json_object(pending("journal/manifest.json"));
    QSet<QString> owned;
    for (const auto& value : journal.value("files").toArray()) {
        owned.insert(value.toObject().value("path").toString());
    }
    for (const QString& name : owned - expected) {
        DSD_FPRINTF(stderr, "  owned beyond the first restore: %s\n", qUtf8Printable(name));
    }
    for (const QString& name : expected - owned) {
        DSD_FPRINTF(stderr, "  not owned though the first restore wrote it: %s\n", qUtf8Printable(name));
    }
    expect("the second restore owns exactly what the first restore wrote", stop.crashed && owned == expected);
    QStringList roots;
    for (const auto& value : journal.value("ownedRoots").toArray()) {
        roots.append(value.toString());
    }
    roots.sort();
    QStringList wantRoots{relative(b.chanRoot), relative(old.targetsDest)};
    wantRoots.sort();
    expect("the only owned roots are the first restore's two bundles", roots == wantRoots);

    expect("the second restore applies", apply().outcome == ApplyOutcome::Applied);
    const Tree after = capture();
    bool kept = true;
    for (auto it = restoredA.files.cbegin(); it != restoredA.files.cend(); ++it) {
        if (!owned.contains(it.key()) && it.key() != QLatin1String(kResultFile)
            && after.files.value(it.key()) != it.value()) {
            DSD_FPRINTF(stderr, "  changed though not owned: %s\n", qUtf8Printable(it.key()));
            kept = false;
        }
    }
    expect("the second restore deletes or changes nothing else", kept && after.links == restoredA.links);
    expect("the second restore removed the list B dropped", !QFileInfo::exists(b.extra));
    expect("the unreferenced folder at the moved bundle's old id survives both restores",
           read_file(b.targetsRoot + "/notes.txt") == "not a bundle of ours\n");
    expect("B's files land where A's were", read_file(old.groupsDest) == restoredA.files.value(relative(old.groupsDest))
                                                && QFileInfo(old.targetsDest + "/targets.csv").isFile());
}

} // namespace

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    /* Isolated, disposable storage - nothing this test writes may touch a real profile (same arrangement as
     * test_ui_qt_persistence). */
    QCoreApplication::setOrganizationName(QStringLiteral("dsd-neo-test"));
    QCoreApplication::setApplicationName(
        QStringLiteral("dsd-neo-backup-apply-%1").arg(QCoreApplication::applicationPid()));
    dsd_test_qt_isolate_paths();
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir != root()) {
        DSD_FPRINTF(stderr, "the data root is not the app data directory\n");
        return 1;
    }
    remove_tree(dataDir);
    QDir().mkpath(dataDir);

    {
        TestHost host;
        QTemporaryDir external;
        const Backup backup = make_backup(host);
        const Old old = make_old(host, backup, external.path());
        test_staging_layout(backup);
        test_staging_validates_library_files(host, backup);
        test_staging_takes_the_callers_root(backup);
        test_promote_commit_discard(backup, old);
        const Tree reference = test_clean_apply(host, backup, old);
        test_crash_at_every_step(backup, old, reference);
        test_crash_during_rollback(backup, old, reference);
        test_io_failures_roll_back(backup, old);
        test_stuck(backup, old, reference);
        test_failed_before_any_change(backup, old);
        test_deferred(backup, old, reference);
        test_journal_failures(backup, old, reference);
        test_busy_after_journal(backup, old, reference);
        test_unmarked_pending(backup, old);
        test_symlinked_component(backup, old);
        test_symlinked_store(backup, old);
        test_discard_after_commit_point(backup, old);
        test_discard_order(backup, old);
        test_marker_must_be_a_file(backup, old);
        test_old_file_where_a_directory_goes(backup, old);
        test_journal_problems(backup, old);
        test_unreadable_journal(backup, old);
        test_links_inside_removed_trees(backup, old, reference);
        test_journal_leftover(backup, old);
        test_empty_avoids(host, backup, old);
        test_two_restores_in_a_row(backup, old, reference);
        test_bundle_named_from_outside(host, old);
    }
    dsd_app_signal_search_store_close();

    remove_tree(dataDir);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
