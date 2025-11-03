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

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo_ui_terminal PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(dsd-neo_ui_terminal PRIVATE /W4)
    endif()
    target_compile_options(dsd-neo_ui_terminal PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                           -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                           -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                           -Wtautological-compare)
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo_ui_terminal PRIVATE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo_ui_terminal PRIVATE /WX)
        endif()
    endif()
endif()

# Many menu actions are conditionally referenced; suppress unused-function just for these sources
set_source_files_properties(menu_core.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
set_source_files_properties(dsd_ncurses_menu.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
