add_executable(dsd-neo)

target_sources(dsd-neo PRIVATE main.c frontend.c)

# Link against modular libraries
target_link_libraries(
    dsd-neo
    PRIVATE dsd-neo_engine dsd-neo_feature_rtlsdr dsd-neo_feature_codec2 ${LIBS}
)

if(DSD_ENABLE_TERMINAL_UI)
    target_compile_definitions(dsd-neo PRIVATE DSD_CLI_HAS_TERMINAL_UI=1)
    target_link_libraries(dsd-neo PRIVATE dsd-neo_ui_terminal)
else()
    target_compile_definitions(dsd-neo PRIVATE DSD_CLI_HAS_TERMINAL_UI=0)
endif()

if(DSD_ENABLE_NATIVE_UI)
    target_compile_definitions(dsd-neo PRIVATE DSD_CLI_HAS_NATIVE_UI=1)
    target_link_libraries(dsd-neo PRIVATE dsd-neo_ui_native)
else()
    target_compile_definitions(dsd-neo PRIVATE DSD_CLI_HAS_NATIVE_UI=0)
endif()

# Target include directories (prefer target-scoped over global)
target_include_directories(dsd-neo SYSTEM PRIVATE ${_PUBLIC_INCLUDES})
target_include_directories(dsd-neo PRIVATE "${PROJECT_SOURCE_DIR}/include")
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
if(CODEC2_FOUND)
    target_include_directories(dsd-neo SYSTEM PRIVATE ${CODEC2_INCLUDE_DIRS})
endif()

# mbelib-neo is always used (no legacy mbelib fallback)
target_compile_definitions(dsd-neo PRIVATE USE_MBELIB_NEO)

target_link_libraries(dsd-neo PRIVATE dsd-neo_warnings)

if(BUILD_TESTING)
    add_test(
        NAME CLI_HELP_SMOKE
        COMMAND
            ${CMAKE_COMMAND} -D DSD_NEO_CLI=$<TARGET_FILE:dsd-neo> -D
            DSD_NEO_CLI_SMOKE_MODE=help -P
            ${PROJECT_SOURCE_DIR}/tests/cmake/RunCliSmoke.cmake
    )

    add_test(
        NAME CLI_INVALID_OPTION_SMOKE
        COMMAND
            ${CMAKE_COMMAND} -D DSD_NEO_CLI=$<TARGET_FILE:dsd-neo> -D
            DSD_NEO_CLI_SMOKE_MODE=invalid-option -P
            ${PROJECT_SOURCE_DIR}/tests/cmake/RunCliSmoke.cmake
    )
endif()

## Fast-math now configured globally when enabled

## LTO/IPO now configured globally for Release when enabled

include(GNUInstallDirs)
install(TARGETS dsd-neo DESTINATION ${CMAKE_INSTALL_BINDIR})

# man page from executable help output
find_program(HELP2MAN_FOUND help2man)
if(HELP2MAN_FOUND)
    add_custom_command(
        TARGET dsd-neo
        POST_BUILD
        COMMAND help2man
        ARGS
            -n "Digital Speech Decoder" --version-string=${GIT_TAG} -o
            ${CMAKE_CURRENT_BINARY_DIR}/dsd-neo.1 --no-info
            $<TARGET_FILE:dsd-neo>
    )
    install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/dsd-neo.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    )
endif()
