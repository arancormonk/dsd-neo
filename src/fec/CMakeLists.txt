# FEC module library
add_library(dsd-neo_fec STATIC)

target_sources(
    dsd-neo_fec
    PRIVATE
        fec.c
        bptc.c
        dmr_late_entry.c
        rs-12-9.c
        trellis34.c
        hamming_10_6_3.cpp
        ez.cpp
        p25p2_isch.c
)

target_include_directories(
    dsd-neo_fec
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_fec PRIVATE dsd-neo_ezpwd)

target_link_libraries(dsd-neo_fec PRIVATE dsd-neo_warnings)
