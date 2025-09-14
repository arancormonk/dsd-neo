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
