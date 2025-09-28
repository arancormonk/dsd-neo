# IO module libraries

# Radio (RTL-SDR front-end and streaming)
if(RTLSDR_FOUND)
  add_library(dsd-neo_io_radio)
  target_sources(dsd-neo_io_radio PRIVATE
    radio/rtl_device.cpp
    radio/rtl_stream.cpp
    radio/rtl_stream_c.cpp
    radio/rtl_sdr_fm.cpp
  )
  target_include_directories(dsd-neo_io_radio
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${_PUBLIC_INCLUDES} ${RTLSDR_INCLUDE_DIRS}
  )
  target_link_libraries(dsd-neo_io_radio PUBLIC dsd-neo_dsp)

  # Warnings (configurable)
  if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_io_radio PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
      target_compile_options(dsd-neo_io_radio PRIVATE /W4)
    endif()
    target_compile_options(dsd-neo_io_radio PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                           -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                           -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                           -Wtautological-compare)
    if(DSD_WARNINGS_AS_ERRORS)
      if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(dsd-neo_io_radio PRIVATE -Werror)
      elseif(MSVC)
        target_compile_options(dsd-neo_io_radio PRIVATE /WX)
      endif()
    endif()
  endif()
else()
  # Provide a stub target so linking remains unconditional upstream
  add_library(dsd-neo_io_radio INTERFACE)
endif()

# Audio backends (PortAudio, PulseAudio)
add_library(dsd-neo_io_audio)
target_sources(dsd-neo_io_audio PRIVATE
  audio_backends/pa_devs.c
  audio_backends/pulse_devices.c
  audio_backends/udp_input.c
)
target_include_directories(dsd-neo_io_audio
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)
target_link_libraries(dsd-neo_io_audio PUBLIC dsd-neo_runtime)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dsd-neo_io_audio PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    target_compile_options(dsd-neo_io_audio PRIVATE /W4)
  endif()
  target_compile_options(dsd-neo_io_audio PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                         -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                         -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                         -Wtautological-compare)
  if(DSD_WARNINGS_AS_ERRORS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_io_audio PRIVATE -Werror)
    elseif(MSVC)
      target_compile_options(dsd-neo_io_audio PRIVATE /WX)
    endif()
  endif()
endif()

# Control interfaces (UDP control, rigctl, serial)
add_library(dsd-neo_io_control)
target_sources(dsd-neo_io_control PRIVATE
  control/udp_control.cpp
  control/dsd_rigctl.c
  control/dsd_serial.c
)
target_include_directories(dsd-neo_io_control
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${_PUBLIC_INCLUDES}
)
target_link_libraries(dsd-neo_io_control PUBLIC dsd-neo_runtime)

# Warnings (configurable)
if(DSD_ENABLE_WARNINGS)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dsd-neo_io_control PRIVATE -Wall -Wextra -Wpedantic)
  elseif(MSVC)
    target_compile_options(dsd-neo_io_control PRIVATE /W4)
  endif()
  target_compile_options(dsd-neo_io_control PRIVATE -Wunused-but-set-variable -Wunused-variable -Wunused-parameter
                         -Wempty-body -Wunused-label $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                         -Wmisleading-indentation -Wparentheses -Wunused-value -Wreturn-type
                         -Wtautological-compare)
  if(DSD_WARNINGS_AS_ERRORS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(dsd-neo_io_control PRIVATE -Werror)
    elseif(MSVC)
      target_compile_options(dsd-neo_io_control PRIVATE /WX)
    endif()
  endif()
endif()
