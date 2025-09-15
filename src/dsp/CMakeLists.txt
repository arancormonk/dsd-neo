# DSP module library
add_library(dsd-neo_dsp)

target_sources(dsd-neo_dsp PRIVATE
  demod_pipeline.cpp
  cqpsk_equalizer.cpp
  cqpsk_path.cpp
  resampler.cpp
  halfband.cpp
  polar_disc.cpp
  fll.cpp
  ted.cpp
  simd_widen.cpp
  dsd_filters.c
  dsd_symbol.c
  dsd_frame_sync.c
)

target_include_directories(dsd-neo_dsp
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)

target_link_libraries(dsd-neo_dsp PUBLIC dsd-neo_runtime)
