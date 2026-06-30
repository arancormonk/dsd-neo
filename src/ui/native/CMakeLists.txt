add_library(dsd-neo_ui_native)

target_sources(dsd-neo_ui_native PRIVATE frontend_provider.c)

target_include_directories(
    dsd-neo_ui_native
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(
    dsd-neo_ui_native
    PUBLIC dsd-neo_app_control dsd-neo_runtime dsd-neo_platform
)

target_link_libraries(dsd-neo_ui_native PRIVATE dsd-neo_warnings)

install(
    TARGETS dsd-neo_ui_native
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
