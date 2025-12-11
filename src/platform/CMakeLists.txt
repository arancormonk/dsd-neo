# Platform abstraction library

add_library(dsd-neo_platform INTERFACE)

# Platform-specific sources will be added in later phases
# For now, this is a header-only interface library

target_include_directories(dsd-neo_platform
    INTERFACE ${PROJECT_SOURCE_DIR}/include
)

# Platform-specific link dependencies
if(WIN32 AND NOT CYGWIN)
    target_link_libraries(dsd-neo_platform INTERFACE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(dsd-neo_platform INTERFACE Threads::Threads)
endif()
