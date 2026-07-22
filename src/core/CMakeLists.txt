add_library(dsd-neo_core)

target_compile_definitions(
    dsd-neo_core
    PUBLIC DSD_NEO_EVENT_HISTORY_TRANSACTIONS
)

target_sources(
    dsd-neo_core
    PRIVATE
        audio/dsd_audio.c
        audio/dsd_audio2.c
        audio/dispatch.c
        audio/dsd_audio_gate.c
        audio/gain.c
        audio/mix.c
        audio/convert.c
        audio/dsd_upsample.c
        vocoder/keyring.c
        vocoder/dsd_mbe.c
        vocoder/dsd_mbe_purge.c
        frames/dsd_frame.c
        frames/dsd_dibit.c
        time/dsd_time.c
        time/dsd_time_state.c
        util/dsd_misc.c
        util/bit_packing.c
        util/dsd_init.c
        util/dsd_events.c
        util/call_state.c
        util/edacs_afs.c
        util/dsd_alias.c
        util/talkgroup_policy.c
        util/synctype.c
        file/dsd_file.c
        file/dsd_import.c
        file/p25_sm_log.c
        gps/dsd_gps.c
)

target_include_directories(
    dsd-neo_core
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src/core ${_PUBLIC_INCLUDES}
)

target_link_libraries(
    dsd-neo_core
    PUBLIC
        dsd-neo_runtime
        dsd-neo_dsp
        dsd-neo_fec
        dsd-neo_crypto
        dsd-neo_feature_radio
        dsd-neo_feature_rtlsdr
        dsd-neo_feature_codec2
        dsd-neo_feature_pvc
        ${MBE_LINK_TARGET}
        ${LIBSNDFILE_LIBRARIES}
)

target_link_libraries(dsd-neo_core PRIVATE dsd-neo_warnings dsd-neo_feature_lz)

install(
    TARGETS dsd-neo_core
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
