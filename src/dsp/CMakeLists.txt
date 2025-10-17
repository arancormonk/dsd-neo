# DSP module library
add_library(dsd-neo_dsp)

target_sources(dsd-neo_dsp PRIVATE
  demod_pipeline.cpp
  cqpsk_equalizer.cpp
  cqpsk_path.cpp
  costas.cpp
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

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo_dsp PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(dsd-neo_dsp PRIVATE /W4)
    endif()
    target_compile_options(dsd-neo_dsp PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                           -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                           -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                           -Wtautological-compare)
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo_dsp PRIVATE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo_dsp PRIVATE /WX)
        endif()
    endif()
endif()
