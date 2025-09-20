add_executable(dsd-neo_test_aes_ofb test_aes_ofb.c)
target_include_directories(dsd-neo_test_aes_ofb PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_aes_ofb PRIVATE dsd-neo_crypto)

add_test(NAME AES_OFB COMMAND dsd-neo_test_aes_ofb)

# P25 scaffolding tests
add_executable(dsd-neo_test_p25_mbf34 test_p25_mbf34.c)
target_include_directories(dsd-neo_test_p25_mbf34 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mbf34 PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MBF34 COMMAND dsd-neo_test_p25_mbf34)

add_executable(dsd-neo_test_p25_lsd test_p25_lsd.c)
target_include_directories(dsd-neo_test_p25_lsd PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_lsd PRIVATE dsd-neo_proto_p25 dsd-neo_core dsd-neo_proto_dmr)
add_test(NAME P25_LSD COMMAND dsd-neo_test_p25_lsd)

add_executable(dsd-neo_test_p25_lcch_crc16 test_p25_lcch_crc16.c)
target_include_directories(dsd-neo_test_p25_lcch_crc16 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_lcch_crc16 PRIVATE dsd-neo_proto_p25 dsd-neo_core dsd-neo_proto_dmr)
add_test(NAME P25_LCCH_CRC16 COMMAND dsd-neo_test_p25_lcch_crc16)

add_executable(dsd-neo_test_p25_12 test_p25_12.c)
target_include_directories(dsd-neo_test_p25_12 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_12 PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_12 COMMAND dsd-neo_test_p25_12)

add_executable(dsd-neo_test_p25_p2_lfsr test_p25_p2_lfsr.c)
target_include_directories(dsd-neo_test_p25_p2_lfsr PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_lfsr PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_LFSR COMMAND dsd-neo_test_p25_p2_lfsr)

add_executable(dsd-neo_test_p25_mac_lengths test_p25_mac_lengths.c)
target_include_directories(dsd-neo_test_p25_mac_lengths PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mac_lengths PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MAC_LENGTHS COMMAND dsd-neo_test_p25_mac_lengths)

# P25 P1 MBT → MAC bridging: Identifier Update
add_executable(dsd-neo_test_p25_iden_bridge test_p25_iden_bridge.c)
target_include_directories(dsd-neo_test_p25_iden_bridge PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_iden_bridge PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_IDEN_BRIDGE COMMAND dsd-neo_test_p25_iden_bridge)

# CRC12/16 combined smoke test
add_executable(dsd-neo_test_p25_crc12_16 test_p25_crc12_16.c)
target_include_directories(dsd-neo_test_p25_crc12_16 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_crc12_16 PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_CRC12_16 COMMAND dsd-neo_test_p25_crc12_16)

# RS parity regen smoke tests (uses proto wrappers and ezpwd)
add_executable(dsd-neo_test_p25_rs test_p25_rs.cpp)
target_include_directories(dsd-neo_test_p25_rs PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_rs PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_RS COMMAND dsd-neo_test_p25_rs)

# P25 P2 MAC decode (JSON len derivation via MCO fallback)
add_executable(dsd-neo_test_p25_p2_mac_decode test_p25_p2_mac_decode.c)
target_include_directories(dsd-neo_test_p25_p2_mac_decode PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_mac_decode PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_MAC_DECODE COMMAND dsd-neo_test_p25_p2_mac_decode)

# P25 P1 MBT decode sanity (NET_STS_BCST → CC freq/sysid/wacn)
add_executable(dsd-neo_test_p25_mbt_decode test_p25_mbt_decode.c)
target_include_directories(dsd-neo_test_p25_mbt_decode PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mbt_decode PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MBT_DECODE COMMAND dsd-neo_test_p25_mbt_decode)

# P25 P1 FEC boundaries: Hamming, Golay, RS correction limits
add_executable(dsd-neo_test_p25_p1_fec_boundaries test_p25_p1_fec_boundaries.c)
target_include_directories(dsd-neo_test_p25_p1_fec_boundaries PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p1_fec_boundaries PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_FEC_BOUNDS COMMAND dsd-neo_test_p25_p1_fec_boundaries)

# P25 P2 MAC vendor length overrides
add_executable(dsd-neo_test_p25_p2_mac_vendor_lengths test_p25_p2_mac_vendor_lengths.c)
target_include_directories(dsd-neo_test_p25_p2_mac_vendor_lengths PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_mac_vendor_lengths PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_MAC_VENDOR_LENGTHS COMMAND dsd-neo_test_p25_p2_mac_vendor_lengths)

# P25 P1 PDU JSON emission (RegAuth, SysCfg)
add_executable(dsd-neo_test_p25_p1_pdu_json test_p25_p1_pdu_json.c p25_p1_pdu_json_shim.c)
target_include_directories(dsd-neo_test_p25_p1_pdu_json PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(dsd-neo_test_p25_p1_pdu_json PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_PDU_JSON COMMAND dsd-neo_test_p25_p1_pdu_json)
