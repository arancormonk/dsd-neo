// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest

Item {
    id: mainRoot
    property bool transitioning: false
    property bool showFailure: false
    width: 420
    height: 900
    Loader {
        id: loader
        source: uiDir + "/WizardScreen.qml"
    }
    Loader {
        id: home
        source: uiDir + "/HomeScreen.qml"
        active: false
    }
    Component {
        id: savedSpy
        SignalSpy {
            signalName: "saved"
        }
    }
    TestCase {
        name: "WizardSites"
        when: windowShown
        function init() {
            while (savedSystems.count)
                savedSystems.remove(0);
            loader.item.openForAdd(false);
        }
        property string libraryPath: ""
        property var libraryPaths: []
        function cleanup() {
            home.active = false;
            testContext.blockSystemsStoreAtPlaceFill(0);
            while (savedSystems.count)
                savedSystems.remove(0);
            if (libraryPath.length)
                importedFiles.remove(importedFiles.rowForPath(libraryPath));
            libraryPath = "";
            for (var path of libraryPaths)
                importedFiles.remove(importedFiles.rowForPath(path));
            libraryPaths = [];
        }
        function test_legacy_hint_and_manual_rows() {
            var path = testContext.writeFixtureCsv("site-provenance.csv", "Decimal,Mode,Alpha Tag\n123,A,Dispatch\n");
            var result = importedFiles.importGeneratedFile(path, "Site provenance.csv", "group", {
                origin: "radioreference",
                rrSid: 12,
                rrSiteIds: "16863",
                rrKind: "group"
            });
            verify(result.ok);
            libraryPath = result.path;
            savedSystems.add({
                name: "Legacy",
                trunking: true,
                groupCsvPath: libraryPath
            });
            savedSystems.add({
                name: "Manual"
            });
            home.active = true;
            verify(home.item.needsSiteRefresh(0));
            verify(!home.item.needsSiteRefresh(1));
            compare(savedSystems.siteCount(0), 0);
            compare(savedSystems.siteCount(1), 0);
            savedSystems.add({
                name: "Conventional",
                trunking: false,
                groupCsvPath: libraryPath
            });
            verify(!home.item.needsSiteRefresh(2));
        }
        function site(id, freq) {
            return {
                name: "Site " + id,
                rrSid: 12,
                rrSiteId: id,
                siteName: "Site " + id,
                freqMhz: freq,
                decodeFlag: "-f1",
                trunking: true,
                chanCsvPath: "",
                groupCsvPath: ""
            };
        }
        function test_batch_saves_all_sites_with_source() {
            loader.item.applyRadioReference({
                ok: true,
                rows: [site(16863, "851"), site(48391, "852")]
            });
            loader.item.commit();
            compare(savedSystems.count, 2);
            compare(savedSystems.get(0).rrSiteId, 16863);
            compare(savedSystems.get(1).rrSiteId, 48391);
            compare(savedSystems.get(1).freqMhz, "852");
            compare(savedSystems.get(0).sourceType, savedSystems.get(1).sourceType);
            verify(savedSystems.get(0).sourceType.length > 0);
        }
        function test_explore_after_batch_data() {
            return [
                {
                    tag: "abandoned",
                    save: false
                },
                {
                    tag: "saved",
                    save: true
                }
            ];
        }
        function test_explore_after_batch(data) {
            loader.item.applyRadioReference({
                ok: true,
                rows: [site(16863, "851"), site(48391, "852")]
            });
            if (data.save)
                loader.item.commit();
            var before = savedSystems.count;
            loader.item.openForFound({
                sourceType: "rtltcp",
                host: "127.0.0.1",
                port: 1234
            }, "853.25");
            loader.item.pickDecodeFlag("-f1");
            loader.item.commit();
            compare(savedSystems.count, before + 1);
            compare(savedSystems.get(before).freqMhz, "853.25");
            compare(savedSystems.get(before).rrSiteId, 0);
        }
        function openKeyedBatch() {
            savedSystems.add({
                name: "Earlier row"
            });
            var source = site(16863, "851");
            source.sourceType = "rtltcp";
            source.host = "127.0.0.1";
            source.port = 1234;
            source.encKeyType = "basic";
            source.encKeyValue = String(17 * 3);
            savedSystems.add(source);
            loader.item.openForEdit(1);
            // Capture the source UID before reindexing and before batch import clears editRow.
            savedSystems.remove(0);
            loader.item.applyRadioReference({
                ok: true,
                rows: [site(16863, "851"), site(48391, "852")]
            });
        }
        function test_batch_from_keyed_row_keeps_private_key() {
            openKeyedBatch();
            loader.item.commit();
            compare(savedSystems.count, 3);
            for (var row = 1; row < 3; ++row) {
                var sys = savedSystems.get(row);
                verify(!("encKeyValue" in sys));
                verify(!("encKeyValue" in savedSystems.getByUid(sys.uid)));
                verify(sys.uid !== savedSystems.get(0).uid);
                var built = sessionArgs.build(sys);
                verify(built.ok, "Imported site's retained key must build a valid session");
                verify(!("args" in built));
                verify(testContext.retainedKeyMatches(sys.uid, String(17 * 3)), "Imported site retains the source key");
            }
        }
        function test_batch_missing_key_source_requires_replace_or_clear() {
            openKeyedBatch();
            savedSystems.remove(0);
            loader.item.commit();
            compare(savedSystems.count, 0);
            compare(loader.item.csvNotice, "The retained key is unavailable. Choose Replace or Clear before saving.");
            verify(loader.item.csvNoticeIsProblem);
            var editor = findChild(loader.item, "wizardEncryptionEditor");
            editor.keyAction = "clear";
            loader.item.commit();
            compare(savedSystems.count, 2);
            verify(!savedSystems.get(0).encKeyConfigured && !savedSystems.get(1).encKeyConfigured);
        }
        function test_batch_replaces_key_when_requested() {
            openKeyedBatch();
            var editor = findChild(loader.item, "wizardEncryptionEditor");
            editor.keyAction = "replace";
            editor.keyType = "rc4";
            loader.item.encKeyValue = String(19 * 3);
            loader.item.commit();
            compare(savedSystems.count, 3);
            for (var row = 1; row < 3; ++row) {
                var sys = savedSystems.get(row);
                verify(!("encKeyValue" in sys));
                var built = sessionArgs.build(sys);
                verify(built.ok);
                verify(!("args" in built));
                verify(testContext.retainedKeyMatches(sys.uid, String(19 * 3)), "Batch honors Replace");
            }
        }
        function test_grouped_edit_clears_provenance() {
            savedSystems.add(site(16863, "851"));
            loader.item.openForEdit(0);
            loader.item.freqText = "853";
            loader.item.commit();
            compare(savedSystems.get(0).rrSid, 0);
            compare(savedSystems.get(0).rrSiteId, 0);
        }
        function test_batch_first_site_edit_is_not_lost() {
            loader.item.applyRadioReference({
                ok: true,
                rows: [site(16863, "851"), site(48391, "852")]
            });
            loader.item.freqText = "853";
            loader.item.commit();
            compare(savedSystems.get(0).freqMhz, "853");
            compare(savedSystems.get(0).rrSiteId, 0);
            compare(savedSystems.get(1).rrSiteId, 48391);
        }

        // A site's place, as a RadioReference import answers it.
        function placeOf(ctid) {
            return {
                states: [{
                        stid: 19
                    }],
                counties: [{
                        ctid: ctid,
                        stid: 19
                    }],
                candidateStates: [19]
            };
        }
        function placedSite(id, freq) {
            var placed = site(id, freq);
            placed.siteLat = 41.65;
            placed.siteLon = -91.6;
            placed.hasSitePos = true;
            placed.avoidSite = true;
            placed.place = placeOf(id);
            return placed;
        }
        function placeCounty(row) {
            var counties = savedSystems.get(row).place.counties || [];
            return counties.length === 1 ? counties[0].ctid : 0;
        }

        // B6: a per-site import saves every site or none. A site the model
        // refused part way used to leave the sites before it saved, and a
        // second Save then added those again.
        function test_batch_refused_site_saves_nothing() {
            var refused = site(70001, "853");
            refused.radioSettings = {
                unknown: true
            };
            loader.item.applyRadioReference({
                ok: true,
                rows: [site(16863, "851"), site(48391, "852"), refused]
            });
            loader.item.commit();
            compare(savedSystems.count, 0, "the sites before the refused one stayed saved");
            compare(loader.item.csvNotice, "Could not save this imported system.");
            verify(loader.item.csvNoticeIsProblem);
        }

        // The store fails part way: blocked while the second site is built,
        // after the first one used to be saved on its own.
        function test_batch_failed_save_saves_nothing_and_retry_adds_once() {
            // The store file exists, so blockPath() can move it aside.
            savedSystems.add({
                name: "Store"
            });
            savedSystems.remove(savedSystems.count - 1);
            var store = testContext.storePath("saved_systems.json");
            loader.item.applyRadioReference({
                ok: true,
                rows: [placedSite(16863, "851"), placedSite(48391, "852"), placedSite(70001, "853")]
            });
            testContext.blockSystemsStoreAtPlaceFill(2);
            try {
                loader.item.commit();
                verify(!testContext.fileExists(store), "the store was not blocked part way");
            } finally {
                testContext.blockSystemsStoreAtPlaceFill(0);
                if (!testContext.fileExists(store))
                    verify(testContext.blockPath(store, false), "could not unblock the store");
            }
            compare(savedSystems.count, 0, "the sites saved before the failure stayed");
            compare(loader.item.csvNotice, "Could not save this imported system.");
            verify(loader.item.csvNoticeIsProblem);
            loader.item.commit();
            compare(savedSystems.count, 3, "the retry saved each site once");
            compare(savedSystems.get(0).rrSiteId, 16863);
            compare(savedSystems.get(1).rrSiteId, 48391);
            compare(savedSystems.get(2).rrSiteId, 70001);
        }

        function test_batch_reports_first_inserted_row() {
            savedSystems.add({
                name: "Earlier row"
            });
            var spy = savedSpy.createObject(null, {
                target: loader.item
            });
            loader.item.applyRadioReference({
                ok: true,
                rows: [site(16863, "851"), site(48391, "852")]
            });
            loader.item.commit();
            compare(savedSystems.count, 3);
            compare(spy.count, 1);
            compare(spy.signalArguments[0][0], 1, "saved() names the first site's row");
            spy.destroy();
        }

        // An edited draft site no longer describes the imported site: none of
        // its RadioReference details are saved, only the ids were before.
        function test_batch_edited_site_drops_its_site_details() {
            loader.item.applyRadioReference({
                ok: true,
                rows: [placedSite(16863, "851"), placedSite(48391, "852")]
            });
            loader.item.freqText = "853";
            loader.item.commit();
            compare(savedSystems.count, 2);
            var edited = savedSystems.get(0);
            compare(edited.rrSid, 0);
            compare(edited.rrSiteId, 0);
            compare(edited.siteName, "", "the edited site kept its site name");
            verify(!edited.avoidSite, "the edited site kept its avoid");
            verify(!edited.hasSitePos, "the edited site kept its position");
            compare(edited.siteLat, 0);
            compare(edited.siteLon, 0);
            compare(Object.keys(edited.place).length, 0, "the edited site kept its place");
            var kept = savedSystems.get(1);
            compare(kept.rrSiteId, 48391);
            compare(kept.siteName, "Site 48391");
            verify(kept.hasSitePos);
            compare(placeCounty(1), 48391, "an unchanged site keeps its place");
        }

        // A single import's place is saved with it while its tuning is what
        // the import answered; a conventional multi-repeater import has a
        // place and no site id.
        function test_single_import_place_data() {
            var conventional = {
                ok: true,
                name: "County fire",
                freqMhz: "154.43",
                decodeFlag: "-f1",
                trunking: false,
                place: placeOf(1201)
            };
            var trunked = placedSite(16863, "851");
            trunked.ok = true;
            return [
                {
                    tag: "conventional",
                    result: conventional,
                    edit: "",
                    county: 1201,
                    siteId: 0
                },
                {
                    tag: "conventional edited",
                    result: conventional,
                    edit: "154.44",
                    county: 0,
                    siteId: 0
                },
                {
                    tag: "site",
                    result: trunked,
                    edit: "",
                    county: 16863,
                    siteId: 16863
                },
                {
                    tag: "site edited",
                    result: trunked,
                    edit: "852",
                    county: 0,
                    siteId: 0
                }
            ];
        }
        function test_single_import_place(data) {
            loader.item.applyRadioReference(data.result);
            if (data.edit.length > 0)
                loader.item.freqText = data.edit;
            loader.item.commit();
            compare(savedSystems.count, 1);
            compare(placeCounty(0), data.county);
            compare(savedSystems.get(0).rrSiteId, data.siteId);
            compare(savedSystems.get(0).hasSitePos, data.siteId > 0);
        }

        // Issue #803: stale files on adoption. A library file the way a
        // RadioReference import adopts one, or the way the CSV picker does.
        function libraryFile(name, kind, generated) {
            // Distinct contents, so the library keeps every file as its own row.
            var n = libraryPaths.length + 1;
            var text = kind === "chan" ? "channel,frequency,mode,options\n" + n + ",85" + n + "012500,p25,\n" : "Decimal,Mode,Alpha Tag\n" + (100 + n) + ",A,Dispatch\n";
            var path = testContext.writeFixtureCsv(name, text);
            var result = generated ? importedFiles.importGeneratedFile(path, name, kind, {
                origin: "radioreference",
                rrSid: 6673,
                rrSiteIds: "16863",
                rrKind: kind
            }) : testContext.importNow(path, kind);
            verify(result.ok, "could not add " + name + " to the library");
            libraryPaths = libraryPaths.concat([result.path]);
            return result.path;
        }
        function p25Import() {
            return {
                name: "County radio",
                freqMhz: "851.0125",
                decodeFlag: "-f1",
                trunking: true,
                chanCsvPath: libraryFile("county-chan.csv", "chan", true),
                groupCsvPath: libraryFile("county-group.csv", "group", true),
                rrSid: 6673,
                rrSiteId: 16863,
                siteName: "Central"
            };
        }
        // performImport()'s answer for a TETRA system that lists no
        // talkgroups: no file at all, so neither path key is present.
        function tetraSite(id) {
            return {
                ok: true,
                name: "KorKor — Cell " + id,
                freqMhz: "390.0125",
                decodeFlag: "-fT",
                trunking: true,
                rrSid: 10880,
                rrSiteId: id,
                siteName: "Cell " + id
            };
        }
        function compareFiles(row, chan, group, why) {
            compare(savedSystems.get(row).chanCsvPath, chan, why + ": channel map");
            compare(savedSystems.get(row).groupCsvPath, group, why + ": talkgroup list");
        }

        // Regression (#803): adopting a no-file import after an earlier one in
        // the same wizard kept the earlier system's talkgroup list and channel
        // map, and the mismatch then dropped the new import's RadioReference
        // identity on save.
        function test_adoption_replaces_files_of_an_earlier_import() {
            loader.item.applyRadioReference(p25Import());
            verify(loader.item.groupCsvPath.length > 0);
            loader.item.applyRadioReference(tetraSite(5));
            compare(loader.item.chanCsvPath, "", "an earlier import's channel map stayed");
            compare(loader.item.groupCsvPath, "", "an earlier import's talkgroup list stayed");
            loader.item.commit();
            compare(savedSystems.count, 1);
            compareFiles(0, "", "", "the TETRA system was saved with another system's files");
            compare(savedSystems.get(0).decodeFlag, "-fT");
            compare(savedSystems.get(0).rrSid, 10880, "the import lost its RadioReference identity");
            compare(savedSystems.get(0).rrSiteId, 5);
        }

        // The same, as a per-site batch: every site's files are its own, and a
        // site with none must not take the wizard's (site 0) or the earlier
        // import's (the rest).
        function test_batch_adoption_replaces_files_of_an_earlier_import() {
            loader.item.applyRadioReference(p25Import());
            loader.item.applyRadioReference({
                ok: true,
                rows: [tetraSite(5), tetraSite(6)]
            });
            loader.item.commit();
            compare(savedSystems.count, 2);
            for (var row = 0; row < 2; ++row) {
                compareFiles(row, "", "", "site " + row + " was saved with another system's files");
                compare(savedSystems.get(row).rrSiteId, 5 + row, "site " + row + " lost its RadioReference identity");
            }
        }

        // Editing a saved system and importing it again: a file the user picked
        // for it stays when the import supplies none (the import does not know
        // about it), and a file an earlier import generated for it goes. A
        // stale bandwidth that cannot decode TETRA goes too: an empty field
        // runs it at 72 kHz.
        function test_editing_keeps_picked_files_and_drops_generated_ones() {
            savedSystems.add({
                name: "Mine",
                sourceType: "usb",
                freqMhz: "851.0125",
                decodeFlag: "-f1",
                trunking: true,
                bandwidthKhz: 48,
                chanCsvPath: libraryFile("old-chan.csv", "chan", true),
                groupCsvPath: libraryFile("picked-group.csv", "group", false)
            });
            var picked = savedSystems.get(0).groupCsvPath;
            loader.item.openForEdit(0);
            compare(loader.item.bwText, "48");
            loader.item.applyRadioReference(tetraSite(5));
            compare(loader.item.groupCsvPath, picked, "a file picked for the system being edited was dropped");
            compare(loader.item.chanCsvPath, "", "a file generated by an earlier import stayed");
            compare(loader.item.bwText, "", "a bandwidth TETRA cannot decode at stayed");
            verify(loader.item.stepValid());
            loader.item.commit();
            compare(savedSystems.count, 1);
            compareFiles(0, "", picked, "the edit");
            compare(savedSystems.get(0).bandwidthKhz, -1);
        }

        // Not the same system: a new one being added, or a per-site batch,
        // which saves new systems. A picked file is then the earlier answer to
        // a question the import has now answered.
        function test_picked_files_go_when_not_editing_the_same_system() {
            loader.item.assignCsvPath("group", libraryFile("picked-new.csv", "group", false), false);
            loader.item.applyRadioReference(tetraSite(5));
            compare(loader.item.groupCsvPath, "", "a new system kept a file the import replaced");

            savedSystems.add({
                name: "Mine",
                sourceType: "usb",
                freqMhz: "851.0125",
                decodeFlag: "-f1",
                trunking: true,
                groupCsvPath: libraryFile("picked-batch.csv", "group", false)
            });
            loader.item.openForEdit(0);
            loader.item.applyRadioReference({
                ok: true,
                rows: [tetraSite(5), tetraSite(6)]
            });
            loader.item.commit();
            compare(savedSystems.count, 3);
            compareFiles(1, "", "", "a batch site took the edited system's picked file");
            compareFiles(2, "", "", "a batch site took the edited system's picked file");
        }

        // A file the import does supply replaces whatever was there, picked or
        // not, and a later picker choice is the user's own again.
        function test_supplied_files_replace_and_a_pick_after_is_kept() {
            savedSystems.add({
                name: "Mine",
                sourceType: "usb",
                freqMhz: "851.0125",
                decodeFlag: "-f1",
                trunking: true,
                groupCsvPath: libraryFile("picked-replaced.csv", "group", false)
            });
            loader.item.openForEdit(0);
            var imported = p25Import();
            loader.item.applyRadioReference(imported);
            compare(loader.item.groupCsvPath, imported.groupCsvPath);
            var picked = libraryFile("picked-after.csv", "group", false);
            loader.item.assignCsvPath("group", picked, false);
            loader.item.applyRadioReference(tetraSite(5));
            compare(loader.item.groupCsvPath, picked, "a file picked after the first import was dropped");
            compare(loader.item.chanCsvPath, "", "the first import's channel map stayed");
        }
    }
}
