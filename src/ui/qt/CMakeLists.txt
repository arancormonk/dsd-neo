# QuickDialogs2 backs WizardScreen.qml's `import QtQuick.Dialogs` (FileDialog).
# It must be a declared, linked dependency: androiddeployqt packages the QML
# plugin only for Qt modules it can see, and an undeclared one ships an APK
# whose WizardScreen fails to load.
find_package(
    Qt6
    REQUIRED
    COMPONENTS Core Gui Qml Quick QuickControls2 QuickDialogs2
)

add_library(dsd-neo_ui_qt STATIC)

target_sources(
    dsd-neo_ui_qt
    PRIVATE
        app_prefs.cpp
        call_history_filter.cpp
        call_history_model.cpp
        command_bridge.cpp
        decoder_host.cpp
        imported_files_model.cpp
        json_store.cpp
        metrics_model.cpp
        qt_ui.cpp
        radio_reference_model.cpp
        saved_systems_model.cpp
        spectrum_model.cpp
        spectrum_view_item.cpp
        session_args.cpp
        ui_controller.cpp
)

qt_add_resources(
    dsd-neo_ui_qt
    "dsd_neo_qml"
    PREFIX
    "/dsdneo"
    FILES
        qml/qmldir
        qml/Theme.qml
        qml/Util.js
        qml/Main.qml
        qml/BandRail.qml
        qml/BottomNav.qml
        qml/CallRow.qml
        qml/Caret.qml
        qml/CsvTypeBadge.qml
        qml/DashedActionButton.qml
        qml/DecodeChip.qml
        qml/DisclosureRow.qml
        qml/EncTag.qml
        qml/ExploreSetupScreen.qml
        qml/FilterPill.qml
        qml/FollowLatest.qml
        qml/FrequencyField.qml
        qml/GradientButton.qml
        qml/HistoryScreen.qml
        qml/HomeScreen.qml
        qml/ImportsScreen.qml
        qml/LevelMeter.qml
        qml/LogoMark.qml
        qml/MicroLabel.qml
        qml/ModalSheet.qml
        qml/MonitorScreen.qml
        qml/OnboardingScreen.qml
        qml/OutlineButton.qml
        qml/PlayCircle.qml
        qml/PlexSwitch.qml
        qml/PlexTextField.qml
        qml/RadioReferenceScreen.qml
        qml/RadioSheet.qml
        qml/SegmentedControl.qml
        qml/SettingsScreen.qml
        qml/SpectrumScreen.qml
        qml/ToggleRow.qml
        qml/TunerMarker.qml
        qml/UiPanel.qml
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
    PRIVATE dsd-neo_app_control dsd-neo_core dsd-neo_runtime
)

set_target_properties(dsd-neo_ui_qt PROPERTIES AUTOMOC ON)
