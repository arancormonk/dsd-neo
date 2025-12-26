add_library(dsd-neo_core)

target_sources(dsd-neo_core PRIVATE
  audio/dsd_audio.c
  audio/dsd_audio2.c
  audio/dsd_audio_gate.c
  audio/gain.c
  audio/mix.c
  audio/convert.c
  audio/dsd_upsample.c
  vocoder/dsd_mbe.c
  vocoder/dsd_mbe2.c
  frames/dsd_frame.c
  frames/dsd_dibit.c
  time/dsd_time.c
  util/dsd_cleanup.c
  util/dsd_misc.c
  util/dsd_init.c
  util/dsd_events.c
  util/dsd_alias.c
  util/dsd_reset.c
  util/synctype.c
  file/dsd_file.c
  file/dsd_import.c
  gps/dsd_gps.c
)

target_include_directories(dsd-neo_core
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_core PUBLIC dsd-neo_runtime dsd-neo_dsp dsd-neo_fec dsd-neo_crypto dsd-neo_feature_rtlsdr
                                           dsd-neo_feature_codec2 ${LIBSNDFILE_LIBRARIES})

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo_core PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(dsd-neo_core PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                               -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                               -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                               -Wtautological-compare)
    elseif(MSVC)
        target_compile_options(dsd-neo_core PRIVATE /W4)
    endif()
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo_core PRIVATE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo_core PRIVATE /WX)
        endif()
    endif()
endif()
