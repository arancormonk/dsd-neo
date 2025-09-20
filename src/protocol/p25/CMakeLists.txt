add_library(dsd-neo_proto_p25)

target_sources(dsd-neo_proto_p25 PRIVATE
  p25_12.c
  p25_crc.c
  p25_frequency.c
  p25_lcw.c
  p25_trunk_sm.c
  phase1/p25p1_hdu.c
  phase1/p25p1_ldu.c
  phase1/p25p1_ldu1.c
  phase1/p25p1_ldu2.c
  phase1/p25p1_mdpu.c
  phase1/p25p1_pdu_data.c
  phase1/p25p1_pdu_trunking.c
  phase1/p25p1_tdu.c
  phase1/p25p1_tdulc.c
  phase1/p25p1_tsbk.c
  phase1/p25p1_check_hdu.cpp
  phase1/p25p1_check_ldu.cpp
  phase1/p25p1_check_nid.cpp
  phase1/p25p1_heuristics.c
  # New scaffolding modules
  p25_lsd.c
  phase1/p25p1_mbf34.c
  phase2/p25p2_frame.c
  phase2/p25p2_mac_table.c
  phase2/p25p2_vpdu.c
  phase2/p25p2_xcch.c
  ${PROJECT_SOURCE_DIR}/src/ez.cpp
)

target_include_directories(dsd-neo_proto_p25
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_proto_p25 PUBLIC dsd-neo_runtime dsd-neo_fec dsd-neo_crypto dsd-neo_dsp dsd-neo_ezpwd)
