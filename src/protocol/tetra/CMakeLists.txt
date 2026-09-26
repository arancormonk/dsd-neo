
add_library(dsd-neo_proto_tetra)

target_sources(dsd-neo_proto_tetra PRIVATE
  tetra.c
  tetra_fec.c
  tetra_acelp.c
  tetra_mac.c
  tetra_mle.c
  tetra_mm.c
  tetra_trunk_sm.c
  tetra_tch_slot_gate.c
  tetra_bsch.c
  tetra_bsch_fmt.c
  tetra_channel_info.c
  osmo_sources/tetra_conv_enc.c

)

target_include_directories(dsd-neo_proto_tetra
  PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(dsd-neo_proto_tetra PUBLIC dsd-neo_core)

# Small test executable to validate RCPC depuncture mapping (build with CMake)
add_executable(dsd-neo_tetra_depunct_test tetra_fec.c)
target_compile_definitions(dsd-neo_tetra_depunct_test PRIVATE TETRA_FEC_TEST_MAIN)
target_include_directories(dsd-neo_tetra_depunct_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_executable(dsd-neo_tetra_depunct_test_real tetra_deep_diag.c
  osmo_sources/osmo_viterbi_shim.c osmo_sources/osmo_shim_map.c)
target_include_directories(dsd-neo_tetra_depunct_test_real PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_tetra_depunct_test_real PRIVATE dsd-neo_core dsd-neo_proto_tetra dsd-neo_fec)

# Ensure the test is built into the same configuration output dir and provide
# a convenience target to run it via the build system.
add_custom_target(run-tetra-depunct-test
  COMMAND $<TARGET_FILE:dsd-neo_tetra_depunct_test>
  DEPENDS dsd-neo_tetra_depunct_test
  COMMENT "Run TETRA depuncture mapping test"
)
add_custom_target(run-tetra-depunct-test-real
  COMMAND $<TARGET_FILE:dsd-neo_tetra_depunct_test_real>
  DEPENDS dsd-neo_tetra_depunct_test_real
  COMMENT "Run TETRA depuncture mapping test (real viterbi)"
)
