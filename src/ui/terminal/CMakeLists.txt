add_library(dsd-neo_ui_terminal)

target_sources(dsd-neo_ui_terminal PRIVATE
  dsd_ncurses_handler.c
  dsd_ncurses_menu.c
  dsd_ncurses_printer.c
  menu_core.c
  menu_services.c
)

target_include_directories(dsd-neo_ui_terminal
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_ui_terminal PUBLIC dsd-neo_core dsd-neo_runtime)
