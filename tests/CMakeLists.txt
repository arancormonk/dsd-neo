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

# P25 P2 FACCH RS vectors
add_executable(dsd-neo_test_p25_p2_facch_rs protocol/p25/test_p25_p2_facch_rs.c)
target_include_directories(dsd-neo_test_p25_p2_facch_rs PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_facch_rs PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_FACCH_RS COMMAND dsd-neo_test_p25_p2_facch_rs)

# P25 P2 MAC header MCO -> length inference
add_executable(dsd-neo_test_p25_mac_segment protocol/p25/test_p25_mac_segment.c)
target_include_directories(dsd-neo_test_p25_mac_segment PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_mac_segment PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MAC_SEGMENT COMMAND dsd-neo_test_p25_mac_segment)

# P25 IDEN variants (TDMA denom + map override)
add_executable(dsd-neo_test_p25_iden_variants protocol/p25/test_p25_iden_variants.c)
target_include_directories(dsd-neo_test_p25_iden_variants PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_iden_variants PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_IDEN_VARIANTS COMMAND dsd-neo_test_p25_iden_variants)

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
target_include_directories(dsd-neo_test_p25_p1_pdu_json PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25 ${_PUBLIC_INCLUDES})
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo_test_p25_p1_pdu_json SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
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

# P25 P1 LCW gating (Packet/Encrypted SVC bits)
add_executable(dsd-neo_test_p25_p1_lcw_gating protocol/p25/test_p25_p1_lcw_gating.c)
target_include_directories(dsd-neo_test_p25_p1_lcw_gating PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p1_lcw_gating PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_LCW_GATING COMMAND dsd-neo_test_p25_p1_lcw_gating)

# P25 P2 trunk SM fuzzer (slot flips, MAC-like idle/signal, ENC pending)
add_executable(dsd-neo_test_p25_p2_sm_fuzz protocol/p25/test_p25_p2_sm_fuzz.c)
target_include_directories(dsd-neo_test_p25_p2_sm_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_sm_fuzz PRIVATE dsd-neo_proto_p25 dsd-neo_core)
add_test(NAME P25_P2_SM_FUZZ COMMAND dsd-neo_test_p25_p2_sm_fuzz)
# P25 LCW Call Termination (0x4F) -> SM release
add_executable(dsd-neo_test_p25_lcw_call_term protocol/p25/test_p25_lcw_call_term.c)
target_include_directories(dsd-neo_test_p25_lcw_call_term PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_lcw_call_term PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_LCW_CALL_TERM COMMAND dsd-neo_test_p25_lcw_call_term)

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
target_include_directories(dsd-neo_test_p25_p1_trunk_sm PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25 ${_PUBLIC_INCLUDES})
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo_test_p25_p1_trunk_sm SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
target_link_libraries(dsd-neo_test_p25_p1_trunk_sm PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_TRUNK_SM COMMAND dsd-neo_test_p25_p1_trunk_sm)

# P25 P2 audio gating (SIGNAL, explicit Release)
add_executable(dsd-neo_test_p25_p2_audio_gating protocol/p25/test_p25_p2_audio_gating.c)
target_include_directories(dsd-neo_test_p25_p2_audio_gating PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_audio_gating PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_AUDIO_GATING COMMAND dsd-neo_test_p25_p2_audio_gating)

# Ensure LCCH MAC_SIGNAL never flips P25p2 audio gates
add_executable(dsd-neo_test_p25_p2_lcch_signal_gate protocol/p25/test_p25_p2_lcch_signal_gate.c)
target_include_directories(dsd-neo_test_p25_p2_lcch_signal_gate PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_p2_lcch_signal_gate PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_LCCH_SIGNAL_GATE COMMAND dsd-neo_test_p25_p2_lcch_signal_gate)

# P25 P2 audio enable on PTT via SACCH
add_executable(dsd-neo_test_p25_p2_audio_enable protocol/p25/test_p25_p2_audio_enable.c)
target_include_directories(dsd-neo_test_p25_p2_audio_enable PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25 ${_PUBLIC_INCLUDES})
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo_test_p25_p2_audio_enable SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
target_link_libraries(dsd-neo_test_p25_p2_audio_enable PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_AUDIO_ENABLE COMMAND dsd-neo_test_p25_p2_audio_enable)

# (Optional) DMR Tier III MBC smoke test is disabled by default due to
# static-link ordering across platforms. Enable locally if needed.
# add_executable(dsd-neo_test_dmr_mbc protocol/dmr/test_mbc.c)
# target_include_directories(dsd-neo_test_dmr_mbc PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr)
# target_link_libraries(dsd-neo_test_dmr_mbc PRIVATE dsd-neo_proto_dmr)
# add_test(NAME DMR_MBC_SMOKE COMMAND dsd-neo_test_dmr_mbc)

# P25 malformed/edge-case frames
# NOTE: Malformed frame tests are available but disabled by default due to flakiness across platforms.
# add_executable(dsd-neo_test_p25_malformed_frames protocol/p25/test_p25_malformed_frames.c)
# target_include_directories(dsd-neo_test_p25_malformed_frames PRIVATE ${PROJECT_SOURCE_DIR}/include)
# target_link_libraries(dsd-neo_test_p25_malformed_frames PRIVATE dsd-neo_proto_p25)
# add_test(NAME P25_MALFORMED_FRAMES COMMAND dsd-neo_test_p25_malformed_frames)

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
target_include_directories(dsd-neo_test_p25_p1_tdulc PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25 ${_PUBLIC_INCLUDES})
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo_test_p25_p1_tdulc SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
target_link_libraries(dsd-neo_test_p25_p1_tdulc PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_TDULC COMMAND dsd-neo_test_p25_p1_tdulc)

# P25 P1 IMBE interleave schedule tests
add_executable(dsd-neo_test_p25_p1_interleave protocol/p25/test_p25_p1_interleave.c)
target_include_directories(dsd-neo_test_p25_p1_interleave PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25 ${_PUBLIC_INCLUDES})
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo_test_p25_p1_interleave SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
target_link_libraries(dsd-neo_test_p25_p1_interleave PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_INTERLEAVE COMMAND dsd-neo_test_p25_p1_interleave)

# P25 P2 AACH/CACH mapping and slot labeling (JSON xch + slot)
add_executable(dsd-neo_test_p25_p2_aach_cach_map protocol/p25/test_p25_p2_aach_cach_map.c)
target_include_directories(dsd-neo_test_p25_p2_aach_cach_map PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_aach_cach_map PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_AACH_CACH_MAP COMMAND dsd-neo_test_p25_p2_aach_cach_map)

# P25 P2 mixer gate honors per-slot gates
add_executable(dsd-neo_test_p25_p2_mixer_gate protocol/p25/test_p25_p2_mixer_gate.c)
target_include_directories(dsd-neo_test_p25_p2_mixer_gate PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_mixer_gate PRIVATE dsd-neo_core)
add_test(NAME P25_P2_MIXER_GATE COMMAND dsd-neo_test_p25_p2_mixer_gate)

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
target_include_directories(dsd-neo_test_p25_p1_pdu_es_ext PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25 ${_PUBLIC_INCLUDES})
if(RTLSDR_FOUND)
    target_include_directories(dsd-neo_test_p25_p1_pdu_es_ext SYSTEM PRIVATE ${RTLSDR_INCLUDE_DIRS})
endif()
target_link_libraries(dsd-neo_test_p25_p1_pdu_es_ext PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P1_PDU_ES_EXT COMMAND dsd-neo_test_p25_p1_pdu_es_ext)

# P25 P2 VPDU sequence (PTT->ACTIVE->END across calls)
add_executable(dsd-neo_test_p25_p2_vpdu_sequence protocol/p25/test_p25_p2_vpdu_sequence.c)
target_include_directories(dsd-neo_test_p25_p2_vpdu_sequence PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_vpdu_sequence PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_VPDU_SEQUENCE COMMAND dsd-neo_test_p25_p2_vpdu_sequence)

# P25 P2 VPDU ENC SVC gating should flush per-slot ring and optionally release
add_executable(dsd-neo_test_p25_p2_vpdu_enc_flush protocol/p25/test_p25_p2_vpdu_enc_flush.c)
target_include_directories(dsd-neo_test_p25_p2_vpdu_enc_flush PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_vpdu_enc_flush PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_VPDU_ENC_FLUSH COMMAND dsd-neo_test_p25_p2_vpdu_enc_flush)


# P25 trunk SM CC cache persist/load and candidate iteration
add_executable(dsd-neo_test_p25_trunk_cc_cache protocol/p25/test_p25_trunk_cc_cache.c)
target_include_directories(dsd-neo_test_p25_trunk_cc_cache PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_trunk_cc_cache PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_TRUNK_CC_CACHE COMMAND dsd-neo_test_p25_trunk_cc_cache)

# P25 CC cache path builder (identity and RFSS/SITE scoping)
add_executable(dsd-neo_test_p25_cc_cache_path protocol/p25/test_p25_cc_cache_path.c)
target_include_directories(dsd-neo_test_p25_cc_cache_path PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_cc_cache_path PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_CC_CACHE_PATH COMMAND dsd-neo_test_p25_cc_cache_path)

# Minimal call follower core transitions
add_executable(dsd-neo_test_p25_min_follower_core protocol/p25/test_p25_min_follower_core.c)
target_include_directories(dsd-neo_test_p25_min_follower_core PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_min_follower_core PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MIN_FOLLOWER_CORE COMMAND dsd-neo_test_p25_min_follower_core)

# P25 min follower: ensure its return forces trunk SM release past gating
add_executable(dsd-neo_test_p25_min_return_forces_release protocol/p25/test_p25_min_return_forces_release.c)
target_include_directories(dsd-neo_test_p25_min_return_forces_release PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_min_return_forces_release PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_MIN_RETURN_FORCES_RELEASE COMMAND dsd-neo_test_p25_min_return_forces_release)

# P25 P2 early ENC lockout per-slot gating (stay on VC when other slot active)
add_executable(dsd-neo_test_p25_p2_early_enc_lockout protocol/p25/test_p25_p2_early_enc_lockout.c)
target_include_directories(dsd-neo_test_p25_p2_early_enc_lockout PRIVATE ${PROJECT_SOURCE_DIR}/include ${_PUBLIC_INCLUDES})
target_link_libraries(dsd-neo_test_p25_p2_early_enc_lockout PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_EARLY_ENC_LOCKOUT COMMAND dsd-neo_test_p25_p2_early_enc_lockout)

# P25 P2 2V first-subframe gating should not decode when audio is disallowed
add_executable(dsd-neo_test_p25_p2_2v_first_frame_gate protocol/p25/test_p25_p2_2v_first_frame_gate.c)
target_include_directories(dsd-neo_test_p25_p2_2v_first_frame_gate PRIVATE ${PROJECT_SOURCE_DIR}/include ${_PUBLIC_INCLUDES})
target_link_libraries(dsd-neo_test_p25_p2_2v_first_frame_gate PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_2V_FIRST_FRAME_GATE COMMAND dsd-neo_test_p25_p2_2v_first_frame_gate)

# P25 P2 mid-call ENC flush behavior: ensure per-slot jitter buffer flush
add_executable(dsd-neo_test_p25_p2_midcall_enc_flush protocol/p25/test_p25_p2_midcall_enc_flush.c)
target_include_directories(dsd-neo_test_p25_p2_midcall_enc_flush PRIVATE ${PROJECT_SOURCE_DIR}/include ${_PUBLIC_INCLUDES})
target_link_libraries(dsd-neo_test_p25_p2_midcall_enc_flush PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_MIDCALL_ENC_FLUSH COMMAND dsd-neo_test_p25_p2_midcall_enc_flush)

# P25 IDEN trust promotion for current site
add_executable(dsd-neo_test_p25_iden_trust_promote protocol/p25/test_p25_iden_trust_promote.c)
# Include local P25 test stubs (e.g., mbelib.h) before project headers
target_include_directories(dsd-neo_test_p25_iden_trust_promote PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_iden_trust_promote PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_IDEN_TRUST_PROMOTE COMMAND dsd-neo_test_p25_iden_trust_promote)

# P25 channel suffix formatting
add_executable(dsd-neo_test_p25_format_chan_suffix protocol/p25/test_p25_format_chan_suffix.c)
target_include_directories(dsd-neo_test_p25_format_chan_suffix PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_format_chan_suffix PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_FORMAT_CHAN_SUFFIX COMMAND dsd-neo_test_p25_format_chan_suffix)

# P25 freq fallback when CC is TDMA but IDEN TDMA unknown
add_executable(dsd-neo_test_p25_freq_fallback_tdma protocol/p25/test_p25_freq_fallback_tdma.c)
target_include_directories(dsd-neo_test_p25_freq_fallback_tdma PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_freq_fallback_tdma PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_FREQ_FALLBACK_TDMA COMMAND dsd-neo_test_p25_freq_fallback_tdma)

# P25 P2 release gating semantics
add_executable(dsd-neo_test_p25_p2_release_gating protocol/p25/test_p25_p2_release_gating.c)
target_include_directories(dsd-neo_test_p25_p2_release_gating PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_release_gating PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_RELEASE_GATING COMMAND dsd-neo_test_p25_p2_release_gating)

# P25 regroup/patch tracking
add_executable(dsd-neo_test_p25_patch_tracking protocol/p25/test_p25_patch_tracking.c)
target_include_directories(dsd-neo_test_p25_patch_tracking PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_patch_tracking PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_PATCH_TRACKING COMMAND dsd-neo_test_p25_patch_tracking)

# P25 P2 SM should defer release when right slot has activity
add_executable(dsd-neo_test_p25_p2_sm_right_slot_active protocol/p25/test_p25_p2_sm_right_slot_active.c)
target_include_directories(dsd-neo_test_p25_p2_sm_right_slot_active PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_sm_right_slot_active PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_SM_RIGHT_SLOT_ACTIVE COMMAND dsd-neo_test_p25_p2_sm_right_slot_active)

# P25 P2 SM safety-net release after hangtime+extra
add_executable(dsd-neo_test_p25_p2_sm_safety_net_release protocol/p25/test_p25_p2_sm_safety_net_release.c)
target_include_directories(dsd-neo_test_p25_p2_sm_safety_net_release PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_p2_sm_safety_net_release PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_P2_SM_SAFETY_NET_RELEASE COMMAND dsd-neo_test_p25_p2_sm_safety_net_release)

# P25 grant trust clamp
add_executable(dsd-neo_test_p25_grant_trust_clamp protocol/p25/test_p25_grant_trust_clamp.c)
target_include_directories(dsd-neo_test_p25_grant_trust_clamp PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_grant_trust_clamp PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_GRANT_TRUST_CLAMP COMMAND dsd-neo_test_p25_grant_trust_clamp)

# P25 options gating on MAC VPDU grants
add_executable(dsd-neo_test_p25_options_gating protocol/p25/test_p25_options_gating.c)
target_include_directories(dsd-neo_test_p25_options_gating PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_options_gating PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_OPTIONS_GATING COMMAND dsd-neo_test_p25_options_gating)

# P25 CC cache disable via env
add_executable(dsd-neo_test_p25_cc_cache_disable protocol/p25/test_p25_cc_cache_disable.c)
target_include_directories(dsd-neo_test_p25_cc_cache_disable PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_cc_cache_disable PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_CC_CACHE_DISABLE COMMAND dsd-neo_test_p25_cc_cache_disable)

# P25 learned channel map persistence
add_executable(dsd-neo_test_p25_learned_chan_map protocol/p25/test_p25_learned_chan_map.c)
target_include_directories(dsd-neo_test_p25_learned_chan_map PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_learned_chan_map PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_LEARNED_CHAN_MAP COMMAND dsd-neo_test_p25_learned_chan_map)

# P25 trunk SM timing (TDMA vs FDMA)
add_executable(dsd-neo_test_p25_trunk_sm_timing protocol/p25/test_p25_trunk_sm_timing.c)
target_include_directories(dsd-neo_test_p25_trunk_sm_timing PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_trunk_sm_timing PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_TRUNK_SM_TIMING COMMAND dsd-neo_test_p25_trunk_sm_timing)

# P25 suffix formatting denom=4
add_executable(dsd-neo_test_p25_format_chan_suffix_denom4 protocol/p25/test_p25_format_chan_suffix_denom4.c)
target_include_directories(dsd-neo_test_p25_format_chan_suffix_denom4 PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_format_chan_suffix_denom4 PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_FORMAT_CHAN_SUFFIX_DENOM4 COMMAND dsd-neo_test_p25_format_chan_suffix_denom4)

# P25 next CC candidate empty/cc-only edges
add_executable(dsd-neo_test_p25_next_cc_candidate_edges protocol/p25/test_p25_next_cc_candidate_edges.c)
target_include_directories(dsd-neo_test_p25_next_cc_candidate_edges PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_next_cc_candidate_edges PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_NEXT_CC_CANDIDATE_EDGES COMMAND dsd-neo_test_p25_next_cc_candidate_edges)

# P25 neighbor update spam (bounded CC candidates + iteration)
add_executable(dsd-neo_test_p25_neighbor_spam protocol/p25/test_p25_neighbor_spam.c)
target_include_directories(dsd-neo_test_p25_neighbor_spam PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_neighbor_spam PRIVATE dsd-neo_proto_p25 dsd-neo_core)
add_test(NAME P25_NEIGHBOR_SPAM COMMAND dsd-neo_test_p25_neighbor_spam)

# P25 trunk SM fuzz (liveness under randomized sequences)
add_executable(dsd-neo_test_p25_trunk_sm_fuzz protocol/p25/test_p25_trunk_sm_fuzz.c)
# Include local P25 stubs first (e.g., mbelib.h)
target_include_directories(dsd-neo_test_p25_trunk_sm_fuzz PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/p25)
target_link_libraries(dsd-neo_test_p25_trunk_sm_fuzz PRIVATE dsd-neo_proto_p25 dsd-neo_core)
add_test(NAME P25_TRUNK_SM_FUZZ COMMAND dsd-neo_test_p25_trunk_sm_fuzz)

# DMR Tier III trunk SM tests (with local stubs; safe to enable by default)
add_executable(dsd-neo_test_dmr_t3_sm_clamp
  protocol/dmr/test_dmr_t3_sm_clamp.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_trunk_sm.c)
target_include_directories(dsd-neo_test_dmr_t3_sm_clamp PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr ${PROJECT_SOURCE_DIR}/src/protocol/dmr)
add_test(NAME DMR_T3_SM_CLAMP COMMAND dsd-neo_test_dmr_t3_sm_clamp)

add_executable(dsd-neo_test_dmr_t3_sm_release
  protocol/dmr/test_dmr_t3_sm_release.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_trunk_sm.c)
target_include_directories(dsd-neo_test_dmr_t3_sm_release PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr ${PROJECT_SOURCE_DIR}/src/protocol/dmr)
add_test(NAME DMR_T3_SM_RELEASE COMMAND dsd-neo_test_dmr_t3_sm_release)

# DMR Tier III end-to-end shim (neighbor → grant → release)
add_executable(dsd-neo_test_dmr_t3_shim
  protocol/dmr/test_dmr_t3_shim.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_trunk_sm.c)
target_include_directories(dsd-neo_test_dmr_t3_shim PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr ${PROJECT_SOURCE_DIR}/src/protocol/dmr)
add_test(NAME DMR_T3_SHIM COMMAND dsd-neo_test_dmr_t3_shim)

# DMR Tier III C_MOVE retune (grant -> move -> retune, gated on VC)
add_executable(dsd-neo_test_dmr_t3_cmove
  protocol/dmr/test_dmr_t3_cmove.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_trunk_sm.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_csbk.c)
target_include_directories(dsd-neo_test_dmr_t3_cmove PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr ${PROJECT_SOURCE_DIR}/src/protocol/dmr)
target_link_libraries(dsd-neo_test_dmr_t3_cmove PRIVATE m)
add_test(NAME DMR_T3_CMOVE COMMAND dsd-neo_test_dmr_t3_cmove)

# DMR Tier III forced release via TG hold override on P_CLEAR
add_executable(dsd-neo_test_dmr_t3_force_release
  protocol/dmr/test_dmr_t3_force_release.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_trunk_sm.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_csbk.c)
target_include_directories(dsd-neo_test_dmr_t3_force_release PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr ${PROJECT_SOURCE_DIR}/src/protocol/dmr)
target_link_libraries(dsd-neo_test_dmr_t3_force_release PRIVATE m)
add_test(NAME DMR_T3_FORCE_RELEASE COMMAND dsd-neo_test_dmr_t3_force_release)

# DMR Tier III move then release (grant -> move TS2->TS1 -> P_CLEAR -> return CC)
add_executable(dsd-neo_test_dmr_t3_move_release
  protocol/dmr/test_dmr_t3_move_release.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_trunk_sm.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_csbk.c)
target_include_directories(dsd-neo_test_dmr_t3_move_release PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr ${PROJECT_SOURCE_DIR}/src/protocol/dmr)
target_link_libraries(dsd-neo_test_dmr_t3_move_release PRIVATE m)
add_test(NAME DMR_T3_MOVE_RELEASE COMMAND dsd-neo_test_dmr_t3_move_release)

# DSP block tests
add_executable(dsd-neo_test_dsp_halfband dsp/test_dsp_halfband.cpp)
target_include_directories(dsd-neo_test_dsp_halfband PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_halfband PRIVATE dsd-neo_dsp)
add_test(NAME DSP_HALFBAND COMMAND dsd-neo_test_dsp_halfband)

add_executable(dsd-neo_test_dsp_polar_disc dsp/test_dsp_polar_disc.cpp)
target_include_directories(dsd-neo_test_dsp_polar_disc PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_polar_disc PRIVATE dsd-neo_dsp)
add_test(NAME DSP_POLAR_DISC COMMAND dsd-neo_test_dsp_polar_disc)

add_executable(dsd-neo_test_dsp_resampler dsp/test_dsp_resampler.cpp)
target_include_directories(dsd-neo_test_dsp_resampler PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_resampler PRIVATE dsd-neo_dsp)
add_test(NAME DSP_RESAMPLER COMMAND dsd-neo_test_dsp_resampler)

add_executable(dsd-neo_test_dsp_fll dsp/test_dsp_fll.cpp)
target_include_directories(dsd-neo_test_dsp_fll PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_fll PRIVATE dsd-neo_dsp)
add_test(NAME DSP_FLL COMMAND dsd-neo_test_dsp_fll)

add_executable(dsd-neo_test_dsp_ted dsp/test_dsp_ted.cpp)
target_include_directories(dsd-neo_test_dsp_ted PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_ted PRIVATE dsd-neo_dsp)
add_test(NAME DSP_TED COMMAND dsd-neo_test_dsp_ted)

add_executable(dsd-neo_test_dsp_simd_widen dsp/test_dsp_simd_widen.cpp)
target_include_directories(dsd-neo_test_dsp_simd_widen PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_simd_widen PRIVATE dsd-neo_dsp)
add_test(NAME DSP_SIMD_WIDEN COMMAND dsd-neo_test_dsp_simd_widen)

add_executable(dsd-neo_test_dsp_cqpsk_eq dsp/test_dsp_cqpsk_eq.cpp)
target_include_directories(dsd-neo_test_dsp_cqpsk_eq PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_cqpsk_eq PRIVATE dsd-neo_dsp)
add_test(NAME DSP_CQPSK_EQ COMMAND dsd-neo_test_dsp_cqpsk_eq)

add_executable(dsd-neo_test_dsp_cqpsk_path dsp/test_dsp_cqpsk_path.cpp)
target_include_directories(dsd-neo_test_dsp_cqpsk_path PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_cqpsk_path PRIVATE dsd-neo_dsp)
add_test(NAME DSP_CQPSK_PATH COMMAND dsd-neo_test_dsp_cqpsk_path)

add_executable(dsd-neo_test_dsp_demod_helpers dsp/test_dsp_demod_helpers.cpp)
target_include_directories(dsd-neo_test_dsp_demod_helpers PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_demod_helpers PRIVATE dsd-neo_dsp)
add_test(NAME DSP_DEMOD_HELPERS COMMAND dsd-neo_test_dsp_demod_helpers)

add_executable(dsd-neo_test_dsp_audio_filters dsp/test_dsp_audio_filters.cpp)
target_include_directories(dsd-neo_test_dsp_audio_filters PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_audio_filters PRIVATE dsd-neo_dsp)
add_test(NAME DSP_AUDIO_FILTERS COMMAND dsd-neo_test_dsp_audio_filters)

add_executable(dsd-neo_test_dsp_rrc dsp/test_dsp_rrc.cpp)
target_include_directories(dsd-neo_test_dsp_rrc PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_rrc PRIVATE dsd-neo_dsp)
add_test(NAME DSP_RRC COMMAND dsd-neo_test_dsp_rrc)

# Additional DSP coverage
add_executable(dsd-neo_test_dsp_hb_complex dsp/test_dsp_hb_complex.cpp)
target_include_directories(dsd-neo_test_dsp_hb_complex PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_hb_complex PRIVATE dsd-neo_dsp)
add_test(NAME DSP_HB_COMPLEX COMMAND dsd-neo_test_dsp_hb_complex)

# HB cascade alias rejection quantification
add_executable(dsd-neo_test_dsp_hb_alias dsp/test_dsp_hb_alias.cpp)
target_include_directories(dsd-neo_test_dsp_hb_alias PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_hb_alias PRIVATE dsd-neo_dsp)
add_test(NAME DSP_HB_ALIAS COMMAND dsd-neo_test_dsp_hb_alias)

add_executable(dsd-neo_test_dsp_iq_dc_block dsp/test_dsp_iq_dc_block.cpp)
target_include_directories(dsd-neo_test_dsp_iq_dc_block PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_iq_dc_block PRIVATE dsd-neo_dsp)
add_test(NAME DSP_IQ_DC_BLOCK COMMAND dsd-neo_test_dsp_iq_dc_block)

add_executable(dsd-neo_test_dsp_fm_agc_limit dsp/test_dsp_fm_agc_limit.cpp)
target_include_directories(dsd-neo_test_dsp_fm_agc_limit PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_fm_agc_limit PRIVATE dsd-neo_dsp)
add_test(NAME DSP_FM_AGC_LIMIT COMMAND dsd-neo_test_dsp_fm_agc_limit)

add_executable(dsd-neo_test_dsp_mf5 dsp/test_dsp_mf5.cpp)
target_include_directories(dsd-neo_test_dsp_mf5 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_mf5 PRIVATE dsd-neo_dsp)
add_test(NAME DSP_MF5 COMMAND dsd-neo_test_dsp_mf5)

add_executable(dsd-neo_test_dsp_squelch dsp/test_dsp_squelch.cpp)
target_include_directories(dsd-neo_test_dsp_squelch PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_squelch PRIVATE dsd-neo_dsp)
add_test(NAME DSP_SQUELCH COMMAND dsd-neo_test_dsp_squelch)

add_executable(dsd-neo_test_dsp_iq_balance dsp/test_dsp_iq_balance.cpp)
target_include_directories(dsd-neo_test_dsp_iq_balance PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_iq_balance PRIVATE dsd-neo_dsp)
add_test(NAME DSP_IQ_BALANCE COMMAND dsd-neo_test_dsp_iq_balance)

add_executable(dsd-neo_test_dsp_fm_demod_ref dsp/test_dsp_fm_demod_ref.cpp)
target_include_directories(dsd-neo_test_dsp_fm_demod_ref PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_fm_demod_ref PRIVATE dsd-neo_dsp)
add_test(NAME DSP_FM_DEMOD_REF COMMAND dsd-neo_test_dsp_fm_demod_ref)

add_executable(dsd-neo_test_dsp_fm_limiter dsp/test_dsp_fm_limiter.cpp)
target_include_directories(dsd-neo_test_dsp_fm_limiter PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_fm_limiter PRIVATE dsd-neo_dsp)
add_test(NAME DSP_FM_LIMITER COMMAND dsd-neo_test_dsp_fm_limiter)

add_executable(dsd-neo_test_dsp_demod_misc dsp/test_dsp_demod_misc.cpp)
target_include_directories(dsd-neo_test_dsp_demod_misc PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_demod_misc PRIVATE dsd-neo_dsp)
add_test(NAME DSP_DEMOD_MISC COMMAND dsd-neo_test_dsp_demod_misc)

# Post-demod audio polyphase decimator (M>2) smoke test
add_executable(dsd-neo_test_dsp_audio_polydecim dsp/test_dsp_audio_polydecim.cpp)
target_include_directories(dsd-neo_test_dsp_audio_polydecim PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_audio_polydecim PRIVATE dsd-neo_dsp)
add_test(NAME DSP_AUDIO_POLYDECIM COMMAND dsd-neo_test_dsp_audio_polydecim)

add_executable(dsd-neo_test_dsp_channel_filters dsp/test_dsp_channel_filters.cpp)
target_include_directories(dsd-neo_test_dsp_channel_filters PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dsp_channel_filters PRIVATE dsd-neo_dsp)
add_test(NAME DSP_CHANNEL_FILTERS COMMAND dsd-neo_test_dsp_channel_filters)

# DMR utility/CRC property tests
add_executable(dsd-neo_test_dmr_crc_props protocol/dmr/test_dmr_crc_props.c)
target_include_directories(dsd-neo_test_dmr_crc_props PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_crc_props PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_CRC_PROPS COMMAND dsd-neo_test_dmr_crc_props)

add_executable(dsd-neo_test_dmr_bit_utils protocol/dmr/test_dmr_bit_utils.c)
target_include_directories(dsd-neo_test_dmr_bit_utils PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_bit_utils PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_BIT_UTILS COMMAND dsd-neo_test_dmr_bit_utils)

add_executable(dsd-neo_test_dmr_confirmed_crc9 protocol/dmr/test_dmr_confirmed_crc9.c)
target_include_directories(dsd-neo_test_dmr_confirmed_crc9 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_confirmed_crc9 PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_CONFIRMED_CRC9 COMMAND dsd-neo_test_dmr_confirmed_crc9)

add_executable(dsd-neo_test_dmr_crc_masks protocol/dmr/test_dmr_crc_masks.c)
target_include_directories(dsd-neo_test_dmr_crc_masks PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests/protocol/dmr)
target_link_libraries(dsd-neo_test_dmr_crc_masks PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_CRC_MASKS COMMAND dsd-neo_test_dmr_crc_masks)

add_executable(dsd-neo_test_dmr_rc_crc7 protocol/dmr/test_dmr_rc_crc7.c)
target_include_directories(dsd-neo_test_dmr_rc_crc7 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_rc_crc7 PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_RC_CRC7 COMMAND dsd-neo_test_dmr_rc_crc7)

add_executable(dsd-neo_test_dmr_r34_stub protocol/dmr/test_dmr_r34_stub.c)
target_include_directories(dsd-neo_test_dmr_r34_stub PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_r34_stub PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_R34_STUB COMMAND dsd-neo_test_dmr_r34_stub)

add_executable(dsd-neo_test_dmr_r34_equiv protocol/dmr/test_dmr_r34_equiv.c)
target_include_directories(dsd-neo_test_dmr_r34_equiv PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_r34_equiv PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_R34_EQUIV COMMAND dsd-neo_test_dmr_r34_equiv)

add_executable(dsd-neo_test_dmr_r34_noise protocol/dmr/test_dmr_r34_noise.c)
target_include_directories(dsd-neo_test_dmr_r34_noise PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_r34_noise PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_R34_NOISE COMMAND dsd-neo_test_dmr_r34_noise)

# DMR SLCO single-fragment bits (LCSS=0) tests
add_executable(dsd-neo_test_dmr_slco_sfrag_bits protocol/dmr/test_dmr_slco_sfrag_bits.c)
target_include_directories(dsd-neo_test_dmr_slco_sfrag_bits PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_slco_sfrag_bits PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_SLCO_SFRAG_BITS COMMAND dsd-neo_test_dmr_slco_sfrag_bits)

# DMR SlotType Golay(20,8)
add_executable(dsd-neo_test_dmr_slottype_golay protocol/dmr/test_dmr_slottype_golay.c)
target_include_directories(dsd-neo_test_dmr_slottype_golay PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_dmr_slottype_golay PRIVATE dsd-neo_proto_dmr)
add_test(NAME DMR_SLOTTYPE_GOLAY COMMAND dsd-neo_test_dmr_slottype_golay)

# DMR relaxed header acceptance for SAP=4 (IP-based)
add_executable(dsd-neo_test_dmr_relaxed_header_ip
  protocol/dmr/test_dmr_relaxed_header_ip.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_block.c
  ${PROJECT_SOURCE_DIR}/src/protocol/dmr/dmr_utils.c
)
target_include_directories(dsd-neo_test_dmr_relaxed_header_ip PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/protocol/dmr)
target_compile_definitions(dsd-neo_test_dmr_relaxed_header_ip PRIVATE MBELIB_NO_HEADERS=1)
add_test(NAME DMR_RELAXED_HEADER_IP COMMAND dsd-neo_test_dmr_relaxed_header_ip)

# FEC core (Hamming/Golay/QR) block code tests
add_executable(dsd-neo_test_fec_block_codes fec/test_fec_block_codes.c)
target_include_directories(dsd-neo_test_fec_block_codes PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_fec_block_codes PRIVATE dsd-neo_fec)
add_test(NAME FEC_BLOCK_CODES COMMAND dsd-neo_test_fec_block_codes)

# FEC BPTC + RS(12,9) tests
add_executable(dsd-neo_test_fec_bptc_rs fec/test_fec_bptc_rs.c)
target_include_directories(dsd-neo_test_fec_bptc_rs PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_fec_bptc_rs PRIVATE dsd-neo_fec)
add_test(NAME FEC_BPTC_RS COMMAND dsd-neo_test_fec_bptc_rs)
# P25 SM core behaviors (monotonic/backoff/cc-hunt)
add_executable(dsd-neo_test_p25_sm_core protocol/p25/test_p25_sm_core.c)
target_include_directories(dsd-neo_test_p25_sm_core PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_sm_core PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_SM_CORE COMMAND dsd-neo_test_p25_sm_core)

# P25 SM candidate cooldown
add_executable(dsd-neo_test_p25_sm_cand_cooldown protocol/p25/test_p25_sm_cand_cooldown.c)
target_include_directories(dsd-neo_test_p25_sm_cand_cooldown PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_sm_cand_cooldown PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_SM_CAND_COOLDOWN COMMAND dsd-neo_test_p25_sm_cand_cooldown)

# P25 retune backoff respects TDMA slot (same RF)
add_executable(dsd-neo_test_p25_retune_backoff_slot protocol/p25/test_p25_retune_backoff_slot.c)
target_include_directories(dsd-neo_test_p25_retune_backoff_slot PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_retune_backoff_slot PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_RETUNE_BACKOFF_SLOT COMMAND dsd-neo_test_p25_retune_backoff_slot)

# P25 TG Hold gating
add_executable(dsd-neo_test_p25_tg_hold_gate protocol/p25/test_p25_tg_hold_gate.c)
target_include_directories(dsd-neo_test_p25_tg_hold_gate PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_tg_hold_gate PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_TG_HOLD_GATE COMMAND dsd-neo_test_p25_tg_hold_gate)

# P25 ENC override via regroup KEY=0
add_executable(dsd-neo_test_p25_enc_override_clear protocol/p25/test_p25_enc_override_clear.c)
target_include_directories(dsd-neo_test_p25_enc_override_clear PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_enc_override_clear PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_ENC_OVERRIDE_CLEAR COMMAND dsd-neo_test_p25_enc_override_clear)

# P25 individual grant gating
add_executable(dsd-neo_test_p25_indiv_gating protocol/p25/test_p25_indiv_gating.c)
target_include_directories(dsd-neo_test_p25_indiv_gating PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dsd-neo_test_p25_indiv_gating PRIVATE dsd-neo_proto_p25)
add_test(NAME P25_INDIV_GATING COMMAND dsd-neo_test_p25_indiv_gating)
