add_library(dsd-neo_api)

target_sources(
    dsd-neo_api
    PRIVATE api_commands.c api_feed.c api_response.c api_server.c json.c
)

target_include_directories(
    dsd-neo_api
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    dsd-neo_api
    PUBLIC dsd-neo_app_control dsd-neo_runtime dsd-neo_platform
    PRIVATE dsd-neo_warnings
)

if(NOT WIN32)
    target_link_libraries(dsd-neo_api PRIVATE m)
endif()

# The JSON writer turns a NaN or infinite telemetry value into null, and the
# parser refuses a number that overflows to infinity; both checks must survive
# the release presets' fast-math. Neither is on a per-sample path.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        json.c
        PROPERTIES COMPILE_OPTIONS "-fno-fast-math"
    )
elseif(MSVC)
    set_source_files_properties(json.c PROPERTIES COMPILE_OPTIONS "/fp:precise")
endif()

install(
    TARGETS dsd-neo_api
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
