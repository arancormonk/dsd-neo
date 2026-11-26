# QuickDialogs2 backs `import QtQuick.Dialogs` (FileDialog), which only
# DocumentPicker.qml's desktop path imports (issue #829); every screen picks
# through DocumentPicker. It must be a declared, linked dependency:
# androiddeployqt packages the QML plugin only for Qt modules it can see, and an
# undeclared one ships an APK whose importing QML fails to load, even where the
# dialog never opens.
find_package(
    Qt6
    REQUIRED
    COMPONENTS Core Gui Qml Quick QuickControls2 QuickDialogs2
)

add_library(dsd-neo_ui_qt STATIC)

# The UI rejects NaN and infinite values (spectrum auto-range, saved preferences,
# scan targets, session arguments) and none of it is DSP-bound, so it keeps IEEE
# semantics even when the global fast-math option is on.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dsd-neo_ui_qt PRIVATE -fno-fast-math)
elseif(MSVC)
    target_compile_options(dsd-neo_ui_qt PRIVATE /fp:precise)
endif()

target_sources(
    dsd-neo_ui_qt
    PRIVATE
        app_prefs.cpp
        backup_apply.cpp
        backup_apply_fs.cpp
        backup_archive.cpp
        backup_archive_build.cpp
        backup_archive_incoming.cpp
        backup_archive_parse.cpp
        backup_archive_rules.cpp
        backup_typed_paths.cpp
        backup_model.cpp
        backup_stage.cpp
        call_history_filter.cpp
        call_history_heard.cpp # Issue #903
        call_history_model.cpp
        call_history_payload.cpp
        call_history_tap.cpp
        frequency_query.cpp
        heard_talkgroup_owners.cpp # Issue #903
        heard_talkgroup_store.cpp # Issue #903
        command_bridge.cpp
        decoder_host.cpp
        decryption_profiles_model.cpp
        diagnostics_log.cpp
        dmr_channels_model.cpp # Discussion #900
        document_import_transport.cpp
        document_save_transport.cpp
        imported_files_model.cpp
        import_names.cpp
        import_service.cpp
        input_commit.cpp
        csv_bundle_import.cpp
        json_store.cpp
        p25_network_model.cpp # WP-F2
        metrics_model.cpp
        qt_ui.cpp
        radio_reference_model.cpp
        rr_place_service.cpp
        recording_journal.cpp
        recording_status.cpp
        recording_store.cpp
        replay_store.cpp
        restart_closer.cpp # Issue #830
        site_groups.cpp # WP-D4
        site_roaming.cpp # Issue #786
        site_roaming_controller.cpp # Issue #786
        location_feed.cpp # Issue #800
        location_requests.cpp # Issue #800
        saved_system_cards.cpp
        saved_systems_model.cpp
        scan_avoid_list_model.cpp
        scan_lists_model.cpp
        scan_list_targets.cpp
        scan_list_starter.cpp
        scan_list_toggle_model.cpp
        spectrum_model.cpp
        system_search_query.cpp
        text_order.cpp
        call_history_export.cpp
        call_history_exporter.cpp
        call_history_playlist.cpp # Issue #909
        csv_text.cpp
        document_export_writer.cpp
        pending_export.cpp
        signal_search_export.cpp
        signal_search_model.cpp
        key_gate.cpp # Issue #902
        known_frequency_index.cpp
        listen_pins.cpp # Issue #801
        local_day_watch.cpp # Issue #902
        map_tile_cache.cpp # Issue #800
        map_tile_http.cpp # Issue #800
        map_tile_worker.cpp # Issue #800
        map_view_item.cpp # Issue #800
        map_view_paint.cpp # Issue #800
        spectrum_view_item.cpp
        spectrum_trace_geometry.cpp
        screen_reader_state.cpp # Issue #782
        session_args.cpp
        settings_recovery.cpp # Issue #828
        store_notices.cpp # Issue #828
        sign_in_autofill.cpp
        talkgroup_filter_model.cpp
        talkgroup_list_model.cpp
        ui_controller.cpp
        unit_activity_model.cpp
        position_model.cpp # Issue #800
        position_tap.cpp # Issue #800
)

qt_add_resources(
    dsd-neo_ui_qt
    "dsd_neo_qml"
    PREFIX
    "/dsdneo"
    FILES
        qml/qmldir
        qml/Theme.qml
        qml/Navigation.qml
        qml/LineBoxes.qml
        qml/NavigationLayer.qml
        qml/FocusFrame.qml
        qml/IconButton.qml
        qml/DiscardDialog.qml
        qml/ConfirmDialog.qml
        qml/ChoiceSheet.qml
        qml/DecryptionProfileEditor.qml
        qml/DecryptionProfileSelector.qml
        qml/DecryptionApplyFlow.qml
        qml/FailureCard.qml
        qml/BackupFlow.qml
        qml/BackupResultCard.qml
        qml/StoreNoticeCard.qml
        qml/CsvImportFlow.qml
        qml/DocumentPicker.qml
        qml/FileBrowser.qml
        qml/FileBrowserScreen.qml
        qml/FileBrowserLocations.qml
        qml/FileBrowserFolder.qml
        qml/FileBrowserLine.qml
        qml/FileBrowserCommand.qml
        qml/HistoryDetailSheet.qml
        qml/CallRecordingPlayer.qml
        qml/DetectionRecordingsSheet.qml
        qml/CallRecordingsScreen.qml
        qml/PlexComboBox.qml
        qml/PlexCheckBox.qml
        qml/PlexInput.qml
        qml/PlexToggle.qml
        qml/Util.js
        qml/Main.qml
        qml/PointerBarrier.qml
        qml/GatedTapHandler.qml
        # WP-D4 site chooser. Keep resource paths on their own lines for the audit.
        qml/SiteChooserSheet.qml
        qml/BandRail.qml
        qml/BottomNav.qml
        qml/CallRow.qml
        qml/PlayGlyph.qml
        qml/Caret.qml
        qml/CsvTypeBadge.qml
        qml/DashedActionButton.qml
        qml/DeviceStatus.qml
        qml/DecodeChip.qml
        qml/DiagnosticsScreen.qml
        qml/LicensesScreen.qml
        qml/RadioReferenceAccountEditor.qml
        qml/RadioReferenceAccountScreen.qml
        qml/DisclosureRow.qml
        qml/EncTag.qml
        qml/EmergencyTag.qml
        qml/PrivateTag.qml
        # WP-D2 encryption entry surfaces.
        qml/EncryptionEditor.qml
        qml/KeySheet.qml
        qml/RevealButton.qml
        qml/ExploreSetupScreen.qml
        qml/FilterPill.qml
        qml/FollowLatest.qml
        qml/KeepPlace.qml
        qml/FrequencyField.qml
        qml/GradientButton.qml
        qml/HistoryScreen.qml
        qml/NowPlayingBar.qml
        qml/PlaylistStatus.qml
        qml/ActivityScreen.qml
        qml/HomeScreen.qml
        qml/SavedSystemsControls.qml
        qml/ScanListScreen.qml
        qml/ScanListCard.qml
        qml/ScanEntryRow.qml
        qml/ImportKindPicker.qml
        qml/IncomingDocumentFlow.qml
        qml/ImportsScreen.qml
        qml/LevelMeter.qml
        qml/LogoMark.qml
        qml/MicroLabel.qml
        qml/ReadableText.qml
        qml/ExportMessage.qml
        qml/SpeechPacer.qml
        qml/LiveText.qml
        qml/ListenTabs.qml
        qml/StableRows.qml
        qml/ScreenReaderFocusRelease.qml
        qml/ModalSheet.qml
        qml/ActionMenu.qml
        qml/MonitorScreen.qml
        qml/NetworkSheet.qml
        qml/ChannelsSheet.qml
        qml/ChannelEditSheet.qml
        qml/OnboardingScreen.qml
        qml/OutlineButton.qml
        qml/PassbandSheet.qml
        qml/PinButton.qml
        qml/PinnedSheet.qml
        qml/ScanAvoidSheet.qml
        qml/ScanListsSheet.qml
        qml/ScanTogetherSheet.qml
        qml/PlayCircle.qml
        qml/PlexSwitch.qml
        qml/PlexFlickable.qml
            qml/PlexTextField.qml
        qml/RadioReferenceScreen.qml
        qml/AirspyControls.qml
        qml/RadioSheet.qml
        qml/RadioSettingsPersistence.qml
        qml/SiteSheet.qml
        qml/SegmentedControl.qml
        qml/GainEditor.qml
        qml/SquelchEditor.qml
        qml/BandwidthChoice.qml
        qml/SettingsScreen.qml
        qml/SpectrumScreen.qml
        qml/SignalSearchScreen.qml
        qml/SignalSearchRangeEditor.qml
        qml/SquelchSheet.qml
        qml/TalkgroupSaveFlow.qml
        qml/TalkgroupEditSheet.qml
        qml/TalkgroupCard.qml
        qml/TalkgroupsScreen.qml
        qml/ToggleRow.qml
        qml/ToneFilterSheet.qml
        qml/ReceivePolicySheet.qml
        qml/TunerMarker.qml
        # Issue #800: the map screen.
        qml/MapScreen.qml
        qml/MapList.qml
        qml/MapPositionSheet.qml
        qml/MapSettingsSheet.qml
        qml/MapLocator.qml
        qml/MapViewKeeper.qml
        qml/MapControlButton.qml
        qml/MapGlyph.qml
        qml/MapText.js
        qml/UiPanel.qml
        qml/WholeWordLabel.qml
        qml/WizardScreen.qml
        fonts/IBMPlexSans-Regular.ttf
        fonts/IBMPlexSans-SemiBold.ttf
        fonts/IBMPlexSans-Bold.ttf
        fonts/IBMPlexMono-Regular.ttf
        fonts/IBMPlexMono-Medium.ttf
)

# Qt looks this up at the resource root, so it cannot share the prefix above.
qt_add_resources(
    dsd-neo_ui_qt
    "dsd_neo_qtquickcontrols2_conf"
    PREFIX
    "/"
    FILES
        qtquickcontrols2.conf
)

target_include_directories(
    dsd-neo_ui_qt
    PUBLIC ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}
)

# Qt's unprefixed `signals`/`slots`/`emit` macros collide with project C headers
# (dsd_call_state_snapshot has a `slots` member). Use the Q_-prefixed spellings.
target_compile_definitions(dsd-neo_ui_qt PUBLIC QT_NO_KEYWORDS)

target_link_libraries(
    dsd-neo_ui_qt
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
    PRIVATE dsd-neo_app_control dsd-neo_core dsd-neo_crypto dsd-neo_runtime
)

set_target_properties(dsd-neo_ui_qt PROPERTIES AUTOMOC ON)

# Source-only QuickTest cases stay out of the shipped resource bundle. Listing
# them alongside the screens keeps new QML visible to the integration owner;
# CTest also requires these files so a missing case cannot silently disappear.
set_property(
    GLOBAL
    PROPERTY
        DSD_QT_FOUNDATION_QML_TESTS
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_scan_list_screen.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_home_scan_lists.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_monitor_scan_target.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_monitor_compact.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_session_initialized.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_diagnostics_screen.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_network_sheet.qml" # WP-F2
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_wizard_encryption.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_key_sheet.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_settings_backup.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_util_fixed.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_device_status.qml" # #647
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_main_receiver_notice.qml" # #647
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_monitor_receiver.qml" # #647
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_document_picker.qml" # #829
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_gain_editor.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_settings_tuner_gain.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_activity_screen.qml"
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_map_screen.qml" # #800
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_main_map.qml" # #800
            "${PROJECT_SOURCE_DIR}/tests/ui/qml/tst_map_prefs.qml" # #800
)
