add_library(dsd-neo_proto_dstar)

target_sources(dsd-neo_proto_dstar PRIVATE
  dstar.c
  dstar_header.c
)

target_include_directories(dsd-neo_proto_dstar
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_proto_dstar PUBLIC dsd-neo_runtime dsd-neo_fec dsd-neo_core dsd-neo_dsp)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dsd-neo_proto_dstar PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    target_compile_options(dsd-neo_proto_dstar PRIVATE /W4)
  endif()
  target_compile_options(dsd-neo_proto_dstar PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                         -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                         -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                         -Wtautological-compare)
  if(DSD_WARNINGS_AS_ERRORS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_proto_dstar PRIVATE -Werror)
    elseif(MSVC)
      target_compile_options(dsd-neo_proto_dstar PRIVATE /WX)
    endif()
  endif()
endif()
