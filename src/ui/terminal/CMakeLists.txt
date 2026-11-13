add_library(dsd-neo_ui_terminal)

target_sources(
    dsd-neo_ui_terminal
    PRIVATE
        dsd_ncurses_handler.c
        dsd_ncurses_printer.c
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
        ui_row.c
        ui_text.c
        ui_text_draw.c
        ui_mouse.c
        ui_async.c
        ui_read_key.c
        menu_core.c
        menu_env.c
        menu_labels.c
        menu_callbacks.c
        menu_actions.c
        menu_receive.c
        menu_items.c
        menu_prompts.c
        menu_render.c
        rr_wizard_core.c
        rr_panel.c
        rr_panel_format.c
        rr_library.c
        scan_list_modal.c
        csv_picker.c
        panels/header.c
        panels/footer.c
        menus/menu_defs.c
)

# The SNR meter treats a NaN or infinite reading as no signal, the menu's
# environment readers reject "nan"/"inf" through dsd_parse_double_strict(), and
# the gain prompts refuse a value outside the gain ranges
# (<dsd-neo/core/gain_setting.h>) and the gain labels read a NaN setting as
# auto; all run once per redraw or edit, so they keep IEEE semantics even when
# global fast-math is on.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        ncurses_snr.c
        menu_env.c
        menu_callbacks.c
        menu_labels.c
        PROPERTIES COMPILE_OPTIONS "-fno-fast-math"
    )
elseif(MSVC)
    set_source_files_properties(
        ncurses_snr.c
        menu_env.c
        menu_callbacks.c
        menu_labels.c
        PROPERTIES COMPILE_OPTIONS "/fp:precise"
    )
endif()

target_include_directories(
    dsd-neo_ui_terminal
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${_PUBLIC_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/app_control
)
# curses is a system library: its headers stay system headers even when the
# wide one is in a subdirectory (Debian and Ubuntu's ncursesw/curses.h is a link
# to the root header). As a plain -I directory, GCC reports warnings inside the
# curses macros, and GCC 9 then flags a false maybe-uninitialized in
# mvwaddnstr(), which -Werror makes fatal.
target_include_directories(
    dsd-neo_ui_terminal
    SYSTEM
    PUBLIC ${CURSES_INCLUDE_DIRS}
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
