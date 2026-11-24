# Platform abstraction library

add_library(dsd-neo_platform STATIC)

# Platform-specific sources
if(WIN32)
    target_sources(
        dsd-neo_platform
        PRIVATE
            threading_win32.c
            sockets_win32.c
            timing_win32.c
            file_compat_win32.c
            posix_compat_win32.c
    )
    target_link_libraries(dsd-neo_platform PUBLIC ws2_32)
else()
    target_sources(
        dsd-neo_platform
        PRIVATE
            threading_posix.c
            sockets_posix.c
            timing_posix.c
            file_compat_posix.c
            file_identity_posix.c
            posix_compat_posix.c
    )
    # File identities read a recording over 2 GiB or dated past 2038 on a 32-bit build too: this one file takes 64-bit
    # file offsets and time (it hands no struct stat, off_t or time_t to any other file; file_compat_posix.c does,
    # through dsd_stat_t, and checks its descriptors through this file instead).
    set_source_files_properties(
        file_identity_posix.c
        PROPERTIES COMPILE_DEFINITIONS "_FILE_OFFSET_BITS=64;_TIME_BITS=64"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(dsd-neo_platform PUBLIC Threads::Threads)
endif()

if(ANDROID)
    # liblog carries __android_log_* for the runtime's platform log sink.
    target_link_libraries(dsd-neo_platform PUBLIC log)
endif()

# Platform-independent sources (compiled on all platforms including host tests)
target_sources(
    dsd-neo_platform
    PRIVATE
        audio_concealment.c
        env.c
        input_backlog.c
        nonce.c
        pcm_stream.c
        write_frames.c
        ${PROJECT_SOURCE_DIR}/src/io/radio/tcp_quality_metrics.cpp
        ${PROJECT_SOURCE_DIR}/src/runtime/input_ring_watermark.cpp
)

# Audio backend sources
if(DSD_AUDIO_BACKEND STREQUAL "portaudio")
    target_sources(dsd-neo_platform PRIVATE audio_portaudio.c)
    target_link_libraries(dsd-neo_platform PUBLIC PortAudio::PortAudio)
    target_include_directories(
        dsd-neo_platform
        PUBLIC ${PORTAUDIO_INCLUDE_DIRS}
    )
elseif(DSD_AUDIO_BACKEND STREQUAL "pulse")
    target_sources(dsd-neo_platform PRIVATE audio_pulse.c audio_pulse_record.c)
    target_link_libraries(
        dsd-neo_platform
        PUBLIC ${PULSEAUDIO_LIBRARY} ${PULSEAUDIO_SIMPLE_LIBRARY}
    )
    target_include_directories(
        dsd-neo_platform
        PUBLIC ${PULSEAUDIO_INCLUDE_DIRS}
    )
elseif(DSD_AUDIO_BACKEND STREQUAL "aaudio")
    target_sources(dsd-neo_platform PRIVATE audio_aaudio.c)
    target_link_libraries(dsd-neo_platform PUBLIC aaudio log)
else()
    target_sources(dsd-neo_platform PRIVATE audio_null.c)
endif()

target_include_directories(
    dsd-neo_platform
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(dsd-neo_platform PRIVATE dsd-neo_warnings)
