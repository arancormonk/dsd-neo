add_library(dsd-neo_proto_dmr STATIC)

target_sources(
    dsd-neo_proto_dmr
    PRIVATE
        dmr_34_viterbi.c
        dmr_abs_params.c
        dmr_ars.c
        dmr_block.c
        dmr_block_crypto.c
        dmr_bs.c
        dmr_cach.c
        dmr_csbk_tables.c
        dmr_csbk_parse.c
        dmr_csbk.c
        dmr_confidence.c
        dmr_ras.c
        dmr_reason.c
        dmr_trunk_sm.c
        dmr_unit_activity.c
        dmr_data.c
        dmr_dburst.c
        dmr_enc_class.c
        dmr_flco.c
        dmr_hytera.c
        dmr_le.c
        dmr_vcrypt.c
        dmr_ms.c
        dmr_pdu.c
        dmr_rc.c
        dmr_rc_notify.c
        dmr_slot_reset.c
        dmr_t3_chan_learn.c
        dmr_t3_channels.c
        dmr_t3_overlay.c
        dmr_t3_grant.c
        dmr_text.c
        dmr_pi.c
        dmr_utils.c
)

target_include_directories(
    dsd-neo_proto_dmr
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(
    dsd-neo_proto_dmr
    PUBLIC
        dsd-neo_runtime
        dsd-neo_fec
        dsd-neo_crypto
        dsd-neo_dsp
        dsd-neo_core
        dsd-neo_feature_radio
        dsd-neo_feature_rtlsdr
)

target_link_libraries(dsd-neo_proto_dmr PRIVATE dsd-neo_warnings)
