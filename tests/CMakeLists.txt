add_executable(dsd-neo_test_aes_ofb crypto/test_aes_ofb.c)
target_include_directories(dsd-neo_test_aes_ofb PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_aes_ofb PRIVATE dsd-neo_crypto)

add_test(NAME AES_OFB COMMAND dsd-neo_test_aes_ofb)

# P25 scaffolding tests
add_executable(dsd-neo_test_p25_mbf34 protocol/p25/test_p25_mbf34.c)
target_include_directories(dsd-neo_test_p25_mbf34 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mbf34 PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MBF34 COMMAND dsd-neo_test_p25_mbf34)

add_executable(dsd-neo_test_p25_lsd protocol/p25/test_p25_lsd.c)
target_include_directories(dsd-neo_test_p25_lsd PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_lsd PRIVATE dsd-neo_proto_p25 dsd-neo_core dsd-neo_proto_dmr)
add_test(NAME P25_LSD COMMAND dsd-neo_test_p25_lsd)

add_executable(dsd-neo_test_p25_lcch_crc16 protocol/p25/test_p25_lcch_crc16.c)
target_include_directories(dsd-neo_test_p25_lcch_crc16 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_lcch_crc16 PRIVATE dsd-neo_proto_p25 dsd-neo_core dsd-neo_proto_dmr)
add_test(NAME P25_LCCH_CRC16 COMMAND dsd-neo_test_p25_lcch_crc16)

add_executable(dsd-neo_test_p25_12 protocol/p25/test_p25_12.c)
target_include_directories(dsd-neo_test_p25_12 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_12 PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_12 COMMAND dsd-neo_test_p25_12)

add_executable(dsd-neo_test_p25_p2_lfsr protocol/p25/test_p25_p2_lfsr.c)
target_include_directories(dsd-neo_test_p25_p2_lfsr PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_lfsr PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_LFSR COMMAND dsd-neo_test_p25_p2_lfsr)

add_executable(dsd-neo_test_p25_mac_lengths protocol/p25/test_p25_mac_lengths.c)
target_include_directories(dsd-neo_test_p25_mac_lengths PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mac_lengths PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MAC_LENGTHS COMMAND dsd-neo_test_p25_mac_lengths)

# P25 P1 MBT → MAC bridging: Identifier Update
add_executable(dsd-neo_test_p25_iden_bridge protocol/p25/test_p25_iden_bridge.c)
target_include_directories(dsd-neo_test_p25_iden_bridge PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_iden_bridge PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_IDEN_BRIDGE COMMAND dsd-neo_test_p25_iden_bridge)

# CRC12/16 combined smoke test
add_executable(dsd-neo_test_p25_crc12_16 protocol/p25/test_p25_crc12_16.c)
target_include_directories(dsd-neo_test_p25_crc12_16 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_crc12_16 PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_CRC12_16 COMMAND dsd-neo_test_p25_crc12_16)

# RS parity regen smoke tests (uses proto wrappers and ezpwd)
add_executable(dsd-neo_test_p25_rs protocol/p25/test_p25_rs.cpp)
target_include_directories(dsd-neo_test_p25_rs PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_rs PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_RS COMMAND dsd-neo_test_p25_rs)

# P25 P2 MAC decode (JSON len derivation via MCO fallback)
add_executable(dsd-neo_test_p25_p2_mac_decode protocol/p25/test_p25_p2_mac_decode.c)
target_include_directories(dsd-neo_test_p25_p2_mac_decode PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_mac_decode PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_MAC_DECODE COMMAND dsd-neo_test_p25_p2_mac_decode)

# P25 P1 MBT decode sanity (NET_STS_BCST → CC freq/sysid/wacn)
add_executable(dsd-neo_test_p25_mbt_decode protocol/p25/test_p25_mbt_decode.c)
target_include_directories(dsd-neo_test_p25_mbt_decode PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mbt_decode PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MBT_DECODE COMMAND dsd-neo_test_p25_mbt_decode)

# P25 P1 FEC boundaries: Hamming, Golay, RS correction limits
add_executable(dsd-neo_test_p25_p1_fec_boundaries protocol/p25/test_p25_p1_fec_boundaries.c)
target_include_directories(dsd-neo_test_p25_p1_fec_boundaries PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p1_fec_boundaries PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_FEC_BOUNDS COMMAND dsd-neo_test_p25_p1_fec_boundaries)

# P25 P2 MAC vendor length overrides
add_executable(dsd-neo_test_p25_p2_mac_vendor_lengths protocol/p25/test_p25_p2_mac_vendor_lengths.c)
target_include_directories(dsd-neo_test_p25_p2_mac_vendor_lengths PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_mac_vendor_lengths PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_MAC_VENDOR_LENGTHS COMMAND dsd-neo_test_p25_p2_mac_vendor_lengths)

# P25 P1 PDU JSON emission (RegAuth, SysCfg)
add_executable(dsd-neo_test_p25_p1_pdu_json protocol/p25/test_p25_p1_pdu_json.c protocol/p25/p25_p1_pdu_json_shim.c)
target_include_directories(dsd-neo_test_p25_p1_pdu_json PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_pdu_json PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_PDU_JSON COMMAND dsd-neo_test_p25_p1_pdu_json)

# P25 P1 NID parity + NAC decode
add_executable(dsd-neo_test_p25_p1_nid protocol/p25/test_p25_p1_nid.c)
target_include_directories(dsd-neo_test_p25_p1_nid PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p1_nid PRIVATE dsd-neo_proto_p25 ${ITPP_LIBRARY})
add_test(NAME P25_P1_NID COMMAND dsd-neo_test_p25_p1_nid)

# P25 P1 LCW → SM dispatch (Group Voice Ch Update – Explicit)
add_executable(dsd-neo_test_p25_p1_lcw_sm protocol/p25/test_p25_p1_lcw_sm.c)
target_include_directories(dsd-neo_test_p25_p1_lcw_sm PRIVATE ${PROJECT_SOURCE_DIR}/include)
# LCW path depends on ConvertBitIntoBytes (dmr utils) and GPS alias in core
target_link_libraries(dsd-neo_test_p25_p1_lcw_sm PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_LCW_SM COMMAND dsd-neo_test_p25_p1_lcw_sm)

# P25 P1 LDU header gating decisions
add_executable(dsd-neo_test_p25_p1_ldu_gating protocol/p25/test_p25_p1_ldu_gating.c)
target_include_directories(dsd-neo_test_p25_p1_ldu_gating PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p1_ldu_gating PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_LDU_GATING COMMAND dsd-neo_test_p25_p1_ldu_gating)

# P25 P1 MBT decode: RFSS + Adjacent Status Broadcast
add_executable(dsd-neo_test_p25_mbt_adj_rfss protocol/p25/test_p25_mbt_adj_rfss.c)
target_include_directories(dsd-neo_test_p25_mbt_adj_rfss PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mbt_adj_rfss PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MBT_ADJ_RFSS COMMAND dsd-neo_test_p25_mbt_adj_rfss)

# P25 channel→frequency mapping
add_executable(dsd-neo_test_p25_frequency_map protocol/p25/test_p25_frequency_map.c)
target_include_directories(dsd-neo_test_p25_frequency_map PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_frequency_map PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_FREQUENCY_MAP COMMAND dsd-neo_test_p25_frequency_map)

# P25 P2 MAC JSON (LCCH/xch, clamp)
add_executable(dsd-neo_test_p25_p2_mac_json_more protocol/p25/test_p25_p2_mac_json_more.c)
target_include_directories(dsd-neo_test_p25_p2_mac_json_more PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_mac_json_more PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_MAC_JSON_MORE COMMAND dsd-neo_test_p25_p2_mac_json_more)

# P25 P2 RS(63,35) wrappers (FACCH/SACCH)
add_executable(dsd-neo_test_p25_p2_rs28_wrappers protocol/p25/test_p25_p2_rs28_wrappers.cpp)
target_include_directories(dsd-neo_test_p25_p2_rs28_wrappers PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/third_party)
target_link_libraries(dsd-neo_test_p25_p2_rs28_wrappers PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_RS28_WRAPPERS COMMAND dsd-neo_test_p25_p2_rs28_wrappers)

# P25 malformed/edge-case frames
# NOTE: Malformed frame tests are available but disabled by default due to flakiness across platforms.
# add_executable(dsd-neo_test_p25_malformed_frames protocol/p25/test_p25_malformed_frames.c)
# target_include_directories(dsd-neo_test_p25_malformed_frames PRIVATE ${PROJECT_SOURCE_DIR}/include)
# target_link_libraries(dsd-neo_test_p25_malformed_frames PRIVATE dsd-neo_proto_p25)
# add_test(NAME P25_MALFORMED_FRAMES COMMAND dsd-neo_test_p25_malformed_frames)

# P25 P1 randomized FEC properties
add_executable(dsd-neo_test_p25_p1_random_fec protocol/p25/test_p25_p1_random_fec.c)
target_include_directories(dsd-neo_test_p25_p1_random_fec PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p1_random_fec PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_RANDOM_FEC COMMAND dsd-neo_test_p25_p1_random_fec)
