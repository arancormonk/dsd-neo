add_library(dsd-neo_ui_terminal)

target_sources(
    dsd-neo_ui_terminal
    PRIVATE
        dsd_ncurses_handler.c
        dsd_ncurses_printer.c
        frontend_provider.c
        ncurses_utils.c
        ncurses_snr.c
        ui_snr_readout.c
        ncurses_init.c
        ncurses_dsp_display.c
        ncurses_dsp_status_format.c
        ncurses_visualizers.c
        ncurses_p25_display.c
        ncurses_trunk_display.c
        ui_prims.c
        ui_async.c
        menu_core.c
        menu_env.c
        menu_labels.c
        menu_callbacks.c
        menu_actions.c
        menu_items.c
        menu_prompts.c
        menu_render.c
        panels/header.c
        panels/footer.c
        menus/menu_defs.c
)

target_include_directories(
    dsd-neo_ui_terminal
    PUBLIC ${PROJECT_SOURCE_DIR}/include ${CURSES_INCLUDE_DIRS}
    PRIVATE
        ${_PUBLIC_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/app_control
)

target_link_libraries(
    dsd-neo_ui_terminal
    PUBLIC
        dsd-neo_core
        dsd-neo_runtime
        dsd-neo_app_control
        dsd-neo_feature_radio
        dsd-neo_feature_rtlsdr
)

target_link_libraries(
    dsd-neo_ui_terminal
    PRIVATE dsd-neo_warnings dsd-neo_feature_colors ${CURSES_LIBRARIES}
)

install(
    TARGETS dsd-neo_ui_terminal
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
