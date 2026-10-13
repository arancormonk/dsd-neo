add_library(dsd-neo_proto_tetra)

target_sources(
    dsd-neo_proto_tetra
    PRIVATE
        tetra.c
        tetra_air.c
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
        tetra_voice.c
        tetra_voice_gate.c
)

target_include_directories(
    dsd-neo_proto_tetra
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(
    dsd-neo_proto_tetra
    PUBLIC dsd-neo_runtime dsd-neo_fec dsd-neo_crypto dsd-neo_core
)

target_link_libraries(dsd-neo_proto_tetra PRIVATE dsd-neo_warnings)

install(
    TARGETS dsd-neo_proto_tetra
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
