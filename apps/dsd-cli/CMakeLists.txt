add_executable(dsd-neo)

target_sources(dsd-neo PRIVATE main.c)

# Link against modular libraries
target_link_libraries(dsd-neo PRIVATE
  dsd-neo_ezpwd
  dsd-neo_fec
  dsd-neo_crypto
  dsd-neo_dsp
  dsd-neo_runtime
  dsd-neo_io_audio
  dsd-neo_io_radio
  dsd-neo_io_control
  dsd-neo_core
  dsd-neo_ui_terminal
  dsd-neo_proto_dstar
  dsd-neo_proto_provoice
  dsd-neo_proto_m17
  dsd-neo_proto_ysf
  dsd-neo_proto_edacs
  dsd-neo_proto_x2tdma
  dsd-neo_proto_dpmr
  dsd-neo_proto_dmr
  dsd-neo_proto_nxdn
  dsd-neo_proto_p25
  # System/third-party libraries last to satisfy static lib resolution order
  ${LIBS})

# Target include directories (prefer target-scoped over global)
target_include_directories(dsd-neo SYSTEM PRIVATE ${_PUBLIC_INCLUDES})
target_include_directories(dsd-neo PRIVATE "${PROJECT_SOURCE_DIR}/include")
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
if(CODEC2_FOUND)
    target_include_directories(dsd-neo SYSTEM PRIVATE ${CODEC2_INCLUDE_DIRS})
endif()

# Expose which MBE implementation we linked against to the code
if(USE_MBELIB_NEO)
    target_compile_definitions(dsd-neo PRIVATE USE_MBELIB_NEO)
else()
    target_compile_definitions(dsd-neo PRIVATE USE_LEGACY_MBELIB)
endif()

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(dsd-neo PRIVATE /W4)
    endif()
    target_compile_options(dsd-neo PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                           -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                           -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                           -Wtautological-compare)
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo PRIVATE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo PRIVATE /WX)
        endif()
    endif()
endif()

## Fast-math now configured globally when enabled

# Sanitizers in Debug
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(_dsd_san_cflags "")
    set(_dsd_san_ldflags "")
    if(DSD_ENABLE_ASAN AND (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"))
        list(APPEND _dsd_san_cflags -fsanitize=address -fno-omit-frame-pointer)
        list(APPEND _dsd_san_ldflags -fsanitize=address)
    endif()
    if(DSD_ENABLE_UBSAN AND (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"))
        list(APPEND _dsd_san_cflags -fsanitize=undefined)
        list(APPEND _dsd_san_ldflags -fsanitize=undefined)
    endif()
    if(_dsd_san_cflags)
        target_compile_options(dsd-neo PRIVATE ${_dsd_san_cflags})
    endif()
    if(_dsd_san_ldflags)
        target_link_options(dsd-neo BEFORE PRIVATE ${_dsd_san_ldflags})
    endif()
endif()

## LTO/IPO now configured globally for Release when enabled

include(GNUInstallDirs)
install(TARGETS dsd-neo DESTINATION ${CMAKE_INSTALL_BINDIR})

# man page from executable help output
find_program(HELP2MAN_FOUND help2man)
if (HELP2MAN_FOUND)
    add_custom_command(TARGET dsd-neo POST_BUILD
        COMMAND help2man
        ARGS -n "Digital Speech Decoder"
            --version-string=${GIT_TAG}
            -o ${CMAKE_CURRENT_BINARY_DIR}/dsd-neo.1
            --no-info
            $<TARGET_FILE:dsd-neo>
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/dsd-neo.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()
