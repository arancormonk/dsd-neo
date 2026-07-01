if(DSD_ENABLE_TERMINAL_UI)
    add_subdirectory(terminal)
endif()

if(DSD_ENABLE_NATIVE_UI)
    add_subdirectory(native)
endif()
