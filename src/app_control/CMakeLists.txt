add_library(dsd-neo_app_control)

target_sources(
    dsd-neo_app_control
    PRIVATE
        actions/actions_audio.c
        actions/actions_radio.c
        actions/actions_trunk.c
        actions/actions_logging.c
        frontend.c
        ui_cmd_queue.c
        ui_snapshot.c
        ui_opts_snapshot.c
        ui_history.c
        telemetry_hooks_install.c
        menu_services.c
        tcp_audio_connect.c
)

target_include_directories(
    dsd-neo_app_control
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    dsd-neo_app_control
    PUBLIC
        dsd-neo_core
        dsd-neo_runtime
        dsd-neo_feature_radio
        dsd-neo_feature_rtlsdr
)

target_link_libraries(
    dsd-neo_app_control
    PRIVATE
        dsd-neo_engine
        dsd-neo_io_audio
        dsd-neo_io_control
        dsd-neo_io_radio
        dsd-neo_proto_dmr
        dsd-neo_proto_p25
        dsd-neo_crypto
        dsd-neo_warnings
)

install(
    TARGETS dsd-neo_app_control
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
