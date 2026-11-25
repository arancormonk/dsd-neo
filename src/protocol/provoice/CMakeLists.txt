add_library(dsd-neo_proto_provoice STATIC)

target_sources(
    dsd-neo_proto_provoice
    PRIVATE provoice.c provoice_confirm.c provoice_frame.c
)

target_include_directories(
    dsd-neo_proto_provoice
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(
    dsd-neo_proto_provoice
    PUBLIC dsd-neo_runtime dsd-neo_fec dsd-neo_crypto dsd-neo_core
)

target_link_libraries(dsd-neo_proto_provoice PRIVATE dsd-neo_warnings)
