add_library(dsd-neo_api)

target_sources(
    dsd-neo_api
    PRIVATE
        api_commands.c
        api_feed.c
        api_server.c
        json.c
)

target_include_directories(
    dsd-neo_api
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    dsd-neo_api
    PUBLIC
        dsd-neo_app_control
        dsd-neo_runtime
        dsd-neo_platform
    PRIVATE
        dsd-neo_warnings
)

if(NOT WIN32)
    target_link_libraries(dsd-neo_api PRIVATE m)
endif()

install(
    TARGETS dsd-neo_api
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
