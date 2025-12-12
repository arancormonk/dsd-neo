# Crypto module library
add_library(dsd-neo_crypto)

target_sources(dsd-neo_crypto PRIVATE
  crypt-aes.c
  crypt-des.c
  crypt-etc.c
  crypt-pc4.c
  crypt-rc2.c
  crypt-rc4.c
  crypt-tyt.c
)

target_include_directories(dsd-neo_crypto
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dsd-neo_crypto PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(dsd-neo_crypto PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                           -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                           -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                           -Wtautological-compare)
  elseif(MSVC)
    target_compile_options(dsd-neo_crypto PRIVATE /W4)
  endif()
  if(DSD_WARNINGS_AS_ERRORS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_crypto PRIVATE -Werror)
    elseif(MSVC)
      target_compile_options(dsd-neo_crypto PRIVATE /WX)
    endif()
  endif()
endif()
