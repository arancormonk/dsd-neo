# Crypto module library
add_library(dsd-neo_crypto)

target_sources(dsd-neo_crypto PRIVATE
  crypt-aes.c
  crypt-csi72.c
  crypt-des.c
  crypt-etc.c
  crypt-kirisun.c
  crypt-pc4.c
  crypt-pc5.c
  crypt-rc2.c
  crypt-rc4.c
  crypt-tyt.c
)

target_include_directories(dsd-neo_crypto
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_crypto PRIVATE dsd-neo_warnings)

install(TARGETS dsd-neo_crypto
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
