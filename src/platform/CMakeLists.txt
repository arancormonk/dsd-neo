# Platform abstraction library

add_library(dsd-neo_platform)

# Platform-specific sources
if(WIN32)
    target_sources(dsd-neo_platform PRIVATE
        threading_win32.c
        sockets_win32.c
        timing_win32.c
        file_compat_win32.c
        posix_compat_win32.c
    )
    target_link_libraries(dsd-neo_platform PUBLIC ws2_32)
else()
    target_sources(dsd-neo_platform PRIVATE
        threading_posix.c
        sockets_posix.c
        timing_posix.c
        file_compat_posix.c
        posix_compat_posix.c
    )
    find_package(Threads REQUIRED)
    target_link_libraries(dsd-neo_platform PUBLIC Threads::Threads)
endif()

# Audio backend sources
if(DSD_USE_PORTAUDIO)
    target_sources(dsd-neo_platform PRIVATE
        audio_portaudio.c
    )
    target_link_libraries(dsd-neo_platform PUBLIC PortAudio::PortAudio)
    target_include_directories(dsd-neo_platform PUBLIC ${PORTAUDIO_INCLUDE_DIRS})
else()
    target_sources(dsd-neo_platform PRIVATE
        audio_pulse.c
    )
    target_link_libraries(dsd-neo_platform PUBLIC
        ${PULSEAUDIO_LIBRARY}
        ${PULSEAUDIO_SIMPLE_LIBRARY}
    )
    target_include_directories(dsd-neo_platform PUBLIC ${PULSEAUDIO_INCLUDE_DIRS})
endif()

target_include_directories(dsd-neo_platform
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

install(TARGETS dsd-neo_platform
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
