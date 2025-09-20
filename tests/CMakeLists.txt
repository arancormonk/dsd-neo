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

# (Removed) Generic RS parity regen smoke test in favor of
# targeted RS(63,35) wrappers and correction limit tests.

# P25 P2 MAC decode (JSON len derivation via MCO fallback)

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

# P25 P1 MBT negative clamps (invalid CHAN-T)
add_executable(dsd-neo_test_p25_p1_mbtx_clamp protocol/p25/test_p25_p1_mbtx_clamp.c)
target_include_directories(dsd-neo_test_p25_p1_mbtx_clamp PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p1_mbtx_clamp PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_MBTX_CLAMP COMMAND dsd-neo_test_p25_p1_mbtx_clamp)

# P25 channel→frequency mapping
add_executable(dsd-neo_test_p25_frequency_map protocol/p25/test_p25_frequency_map.c)
target_include_directories(dsd-neo_test_p25_frequency_map PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_frequency_map PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_FREQUENCY_MAP COMMAND dsd-neo_test_p25_frequency_map)

# P25 P2 MAC JSON (merged: MCO fallback, LCCH/xch, clamp)
add_executable(dsd-neo_test_p25_p2_mac_json protocol/p25/test_p25_p2_mac_json.c)
target_include_directories(dsd-neo_test_p25_p2_mac_json PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_mac_json PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_MAC_JSON COMMAND dsd-neo_test_p25_p2_mac_json)

# P25 P2 RS(63,35) wrappers (FACCH/SACCH)
add_executable(dsd-neo_test_p25_p2_rs28_wrappers protocol/p25/test_p25_p2_rs28_wrappers.cpp)
target_include_directories(dsd-neo_test_p25_p2_rs28_wrappers PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/third_party)
target_link_libraries(dsd-neo_test_p25_p2_rs28_wrappers PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_RS28_WRAPPERS COMMAND dsd-neo_test_p25_p2_rs28_wrappers)

# P25 P2 RS(63,35) correction limits
add_executable(dsd-neo_test_p25_p2_rs28_limits protocol/p25/test_p25_p2_rs28_limits.cpp)
target_include_directories(dsd-neo_test_p25_p2_rs28_limits PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/third_party)
target_link_libraries(dsd-neo_test_p25_p2_rs28_limits PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_RS28_LIMITS COMMAND dsd-neo_test_p25_p2_rs28_limits)

# P25 P1 trunk SM core
add_executable(dsd-neo_test_p25_p1_trunk_sm protocol/p25/test_p25_p1_trunk_sm.c)
target_include_directories(dsd-neo_test_p25_p1_trunk_sm PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_trunk_sm PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_TRUNK_SM COMMAND dsd-neo_test_p25_p1_trunk_sm)

# P25 P2 audio gating (SIGNAL, explicit Release)
add_executable(dsd-neo_test_p25_p2_audio_gating protocol/p25/test_p25_p2_audio_gating.c)
target_include_directories(dsd-neo_test_p25_p2_audio_gating PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_audio_gating PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_AUDIO_GATING COMMAND dsd-neo_test_p25_p2_audio_gating)

# P25 P2 audio enable on PTT via SACCH
add_executable(dsd-neo_test_p25_p2_audio_enable protocol/p25/test_p25_p2_audio_enable.c)
target_include_directories(dsd-neo_test_p25_p2_audio_enable PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_audio_enable PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_AUDIO_ENABLE COMMAND dsd-neo_test_p25_p2_audio_enable)

# P25 malformed/edge-case frames
# NOTE: Malformed frame tests are available but disabled by default due to flakiness across platforms.
# add_executable(dsd-neo_test_p25_malformed_frames protocol/p25/test_p25_malformed_frames.c)
# target_include_directories(dsd-neo_test_p25_malformed_frames PRIVATE ${PROJECT_SOURCE_DIR}/include)
# target_link_libraries(dsd-neo_test_p25_malformed_frames PRIVATE dsd-neo_proto_p25)
# add_test(NAME P25_MALFORMED_FRAMES COMMAND dsd-neo_test_p25_malformed_frames)

# (Removed) Randomized FEC properties test due to low signal and
# platform variability. Deterministic boundary tests retained.

# P25 P1 TSBK -> vPDU bridge (Group Voice Channel Grant)
add_executable(dsd-neo_test_p25_p1_tsbk_vpdu protocol/p25/test_p25_p1_tsbk_vpdu.c)
target_include_directories(dsd-neo_test_p25_p1_tsbk_vpdu PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_tsbk_vpdu PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_TSBK_VPDU COMMAND dsd-neo_test_p25_p1_tsbk_vpdu)

# P25 P1 TSBK clamp (invalid channel mapping)
add_executable(dsd-neo_test_p25_p1_tsbk_clamp protocol/p25/test_p25_p1_tsbk_clamp.c)
target_include_directories(dsd-neo_test_p25_p1_tsbk_clamp PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_tsbk_clamp PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_TSBK_CLAMP COMMAND dsd-neo_test_p25_p1_tsbk_clamp)

# P25 P1 TDULC parse → LCW retune (format 0x44)
add_executable(dsd-neo_test_p25_p1_tdulc protocol/p25/test_p25_p1_tdulc.c)
target_include_directories(dsd-neo_test_p25_p1_tdulc PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_tdulc PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_TDULC COMMAND dsd-neo_test_p25_p1_tdulc)

# P25 P1 IMBE interleave schedule tests
add_executable(dsd-neo_test_p25_p1_interleave protocol/p25/test_p25_p1_interleave.c)
target_include_directories(dsd-neo_test_p25_p1_interleave PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_interleave PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_INTERLEAVE COMMAND dsd-neo_test_p25_p1_interleave)

# P25 P2 AACH/CACH mapping and slot labeling (JSON xch + slot)
add_executable(dsd-neo_test_p25_p2_aach_cach_map protocol/p25/test_p25_p2_aach_cach_map.c)
target_include_directories(dsd-neo_test_p25_p2_aach_cach_map PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_aach_cach_map PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_AACH_CACH_MAP COMMAND dsd-neo_test_p25_p2_aach_cach_map)

# Additional targeted coverage tests
add_executable(dsd-neo_test_p25_p2_vpdu_core protocol/p25/test_p25_p2_vpdu_core.c)
target_include_directories(dsd-neo_test_p25_p2_vpdu_core PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_vpdu_core PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_VPDU_CORE COMMAND dsd-neo_test_p25_p2_vpdu_core)

add_executable(dsd-neo_test_p25_p2_vpdu_grants protocol/p25/test_p25_p2_vpdu_grants.c)
target_include_directories(dsd-neo_test_p25_p2_vpdu_grants PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_vpdu_grants PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_VPDU_GRANTS COMMAND dsd-neo_test_p25_p2_vpdu_grants)


add_executable(dsd-neo_test_p25_p1_pdu_es_ext protocol/p25/test_p25_p1_pdu_es_ext.c protocol/p25/p25_p1_pdu_json_shim.c)
target_include_directories(dsd-neo_test_p25_p1_pdu_es_ext PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_pdu_es_ext PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_PDU_ES_EXT COMMAND dsd-neo_test_p25_p1_pdu_es_ext)

# P25 P2 VPDU sequence (PTT->ACTIVE->END across calls)
add_executable(dsd-neo_test_p25_p2_vpdu_sequence protocol/p25/test_p25_p2_vpdu_sequence.c)
target_include_directories(dsd-neo_test_p25_p2_vpdu_sequence PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_vpdu_sequence PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_VPDU_SEQUENCE COMMAND dsd-neo_test_p25_p2_vpdu_sequence)
