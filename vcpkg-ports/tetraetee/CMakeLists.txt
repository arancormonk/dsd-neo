cmake_minimum_required(VERSION 3.20)
project(tetraetee LANGUAGES C)

find_package(OpenSSL REQUIRED)

add_library(
    tetraetee
    etee_efuncs.c
    etee_efuncs_aes.c
    etee.c
    common.c
)
set_target_properties(
    tetraetee
    PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        WINDOWS_EXPORT_ALL_SYMBOLS ON
)
target_compile_definitions(tetraetee PRIVATE OPENSSL_SUPPRESS_DEPRECATED)
target_include_directories(tetraetee PUBLIC $<INSTALL_INTERFACE:include>)
target_link_libraries(tetraetee PUBLIC OpenSSL::Crypto)

include(GNUInstallDirs)
install(
    TARGETS tetraetee
    EXPORT tetraeteeTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(
    FILES common.h etee.h etee_efuncs.h etee_efuncs_aes.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tetraetee
)
install(
    EXPORT tetraeteeTargets
    FILE tetraeteeTargets.cmake
    NAMESPACE tetraetee::
    DESTINATION share/tetraetee
)
file(
    WRITE "${CMAKE_CURRENT_BINARY_DIR}/tetraeteeConfig.cmake"
    [=[
include(CMakeFindDependencyMacro)
find_dependency(OpenSSL)
include("${CMAKE_CURRENT_LIST_DIR}/tetraeteeTargets.cmake")
]=]
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/tetraeteeConfig.cmake"
    DESTINATION share/tetraetee
)
