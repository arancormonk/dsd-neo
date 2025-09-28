# FEC module library
add_library(dsd-neo_fec)

target_sources(dsd-neo_fec PRIVATE
  fec.c
  bptc.c
  rs-12-9.c
  Hamming.cpp
)

target_include_directories(dsd-neo_fec
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dsd-neo_fec PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    target_compile_options(dsd-neo_fec PRIVATE /W4)
  endif()
  target_compile_options(dsd-neo_fec PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                         -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                         -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                         -Wtautological-compare)
  if(DSD_WARNINGS_AS_ERRORS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_fec PRIVATE -Werror)
    elseif(MSVC)
      target_compile_options(dsd-neo_fec PRIVATE /WX)
    endif()
  endif()
endif()
