find_package(
    Qt6
    REQUIRED
    COMPONENTS Core Gui Qml Quick QuickControls2
)

add_library(dsd-neo_ui_qt STATIC)

target_sources(
    dsd-neo_ui_qt
    PRIVATE
        command_bridge.cpp
        decoder_host.cpp
        event_log_model.cpp
        metrics_model.cpp
        qt_ui.cpp
        ui_controller.cpp
)

qt_add_resources(
    dsd-neo_ui_qt
    "dsd_neo_qml"
    PREFIX
    "/dsdneo"
    FILES
        qml/Main.qml
        qml/EventLogView.qml
        qml/MonitorPane.qml
        qml/SetupPane.qml
        qml/StatusCard.qml
        fonts/DejaVuSansMono.ttf
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
    PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    PRIVATE dsd-neo_app_control dsd-neo_core
)

set_target_properties(dsd-neo_ui_qt PROPERTIES AUTOMOC ON)
