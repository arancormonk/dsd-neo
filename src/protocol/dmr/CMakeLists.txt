add_library(dsd-neo_proto_dmr)

target_sources(dsd-neo_proto_dmr PRIVATE
  dmr_34.c
  dmr_34_viterbi.c
  dmr_block.c
  dmr_bs.c
  dmr_csbk.c
  dmr_trunk_sm.c
  dmr_data.c
  dmr_dburst.c
  dmr_flco.c
  dmr_le.c
  dmr_ms.c
  dmr_pdu.c
  dmr_pi.c
  dmr_utils.c
)

target_include_directories(dsd-neo_proto_dmr
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_proto_dmr PUBLIC dsd-neo_runtime dsd-neo_fec dsd-neo_crypto dsd-neo_dsp dsd-neo_core)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dsd-neo_proto_dmr PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    target_compile_options(dsd-neo_proto_dmr PRIVATE /W4)
  endif()
  target_compile_options(dsd-neo_proto_dmr PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                         -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                         -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                         -Wtautological-compare)
  if(DSD_WARNINGS_AS_ERRORS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_proto_dmr PRIVATE -Werror)
    elseif(MSVC)
      target_compile_options(dsd-neo_proto_dmr PRIVATE /WX)
    endif()
  endif()
endif()
