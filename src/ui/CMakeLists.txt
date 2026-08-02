if(DSD_ENABLE_TERMINAL_UI)
    add_subdirectory(terminal)
endif()

if(DSD_ENABLE_QT_UI)
    add_subdirectory(qt)
endif()
