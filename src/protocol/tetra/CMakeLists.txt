add_library(dsd-neo_proto_tetra STATIC)

target_sources(
    dsd-neo_proto_tetra
    PRIVATE
        tetra.c
        tetra_air.c
        tetra_air_sig.c
        tetra_bsch.c
        tetra_calls.c
        tetra_confirm.c
        tetra_dmo.c
        tetra_fec.c
        tetra_mac.c
        tetra_mle.c
        tetra_mm.c
        tetra_state.c
        tetra_trunk_sm.c
        tetra_ul.c
        tetra_unit_activity.c
        tetra_voice.c
        tetra_voice_gate.c
)

target_include_directories(
    dsd-neo_proto_tetra
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES}
)

# The LIP short report parser and its location-file row live in the core GPS module (dsd_gps.c), which uses the DMR
# text decoder: link it as D-STAR does.
target_link_libraries(
    dsd-neo_proto_tetra
    PUBLIC dsd-neo_runtime dsd-neo_fec dsd-neo_crypto dsd-neo_core
    PRIVATE dsd-neo_proto_dmr
)

target_link_libraries(dsd-neo_proto_tetra PRIVATE dsd-neo_warnings)
