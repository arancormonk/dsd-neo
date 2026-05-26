function(dsd_neo_add_fuzzer target_name test_name source_file corpus_dir)
    set(output_corpus_dir "${CMAKE_CURRENT_BINARY_DIR}/corpus/${corpus_dir}")
    file(MAKE_DIRECTORY "${output_corpus_dir}")

    add_executable(${target_name} ${source_file})
    target_include_directories(
        ${target_name}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/tests/test_support
            ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(
        ${target_name}
        PRIVATE dsd-neo_warnings dsd-neo_test_support ${ARGN}
    )
    target_compile_options(
        ${target_name}
        PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-fsanitize=fuzzer-no-link>
            $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=fuzzer-no-link>
    )
    target_link_options(${target_name} PRIVATE -fsanitize=fuzzer)
    add_test(
        NAME ${test_name}
        COMMAND
            ${target_name} -runs=100 -max_len=65536 -timeout=10
            -rss_limit_mb=2048 ${output_corpus_dir}
            ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${corpus_dir}
    )
    set_tests_properties(${test_name} PROPERTIES LABELS fuzz)
endfunction()

dsd_neo_add_fuzzer(
  dsd-neo_fuzz_runtime_config_user
  FUZZ_RUNTIME_CONFIG_USER
  fuzz_runtime_config_user.cpp
  runtime_config_user
  dsd-neo_runtime
)

dsd_neo_add_fuzzer(
  dsd-neo_fuzz_runtime_freq_parse
  FUZZ_RUNTIME_FREQ_PARSE
  fuzz_runtime_freq_parse.c
  runtime_freq_parse
  dsd-neo_runtime
)

dsd_neo_add_fuzzer(
  dsd-neo_fuzz_io_iq_metadata
  FUZZ_IO_IQ_METADATA
  fuzz_io_iq_metadata.c
  io_iq_metadata
  dsd-neo_io_iq
)

dsd_neo_add_fuzzer(
  dsd-neo_fuzz_core_csv_import
  FUZZ_CORE_CSV_IMPORT
  fuzz_core_csv_import.c
  core_csv_import
  dsd-neo_runtime
  dsd-neo_crypto
)
target_sources(
    dsd-neo_fuzz_core_csv_import
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/core/file/dsd_import.c
        ${PROJECT_SOURCE_DIR}/src/core/util/talkgroup_policy.c
)

dsd_neo_add_fuzzer(
  dsd-neo_fuzz_protocol_m17_lsf
  FUZZ_PROTOCOL_M17_LSF
  fuzz_protocol_m17_lsf.c
  protocol_m17_lsf
  dsd-neo_proto_m17
)

dsd_neo_add_fuzzer(
  dsd-neo_fuzz_protocol_p25p2_mac
  FUZZ_PROTOCOL_P25P2_MAC
  fuzz_protocol_p25p2_mac.c
  protocol_p25p2_mac
  dsd-neo_proto_p25
)
