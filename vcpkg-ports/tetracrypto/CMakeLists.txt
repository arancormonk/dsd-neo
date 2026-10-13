cmake_minimum_required(VERSION 3.20)
project(tetracrypto LANGUAGES C)

add_library(
    tetracrypto
    hurdle.c
    tea1.c
    tea2.c
    tea3.c
    taa1.c
    common.c
)
set_target_properties(
    tetracrypto
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        WINDOWS_EXPORT_ALL_SYMBOLS ON
)
target_include_directories(tetracrypto PUBLIC $<INSTALL_INTERFACE:include>)
if(MSVC)
    target_compile_options(
        tetracrypto
        PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/gcc_compat.h"
            /wd4018
            /wd4098
            /wd4244
    )
else()
    target_compile_options(
        tetracrypto PRIVATE "-include" "${CMAKE_CURRENT_SOURCE_DIR}/gcc_compat.h"
    )
endif()

include(GNUInstallDirs)
install(
    TARGETS tetracrypto
    EXPORT tetracryptoTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(
    FILES common.h hurdle.h taa1.h tea1.h tea2.h tea3.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tetracrypto
)
install(
    EXPORT tetracryptoTargets
    FILE tetracryptoTargets.cmake
    NAMESPACE tetracrypto::
    DESTINATION share/tetracrypto
)
file(
    WRITE "${CMAKE_CURRENT_BINARY_DIR}/tetracryptoConfig.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/tetracryptoTargets.cmake\")\n"
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/tetracryptoConfig.cmake"
    DESTINATION share/tetracrypto
)
