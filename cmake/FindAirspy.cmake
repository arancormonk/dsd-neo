# SPDX-License-Identifier: GPL-3.0-or-later
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_AIRSPY QUIET libairspy)
endif()
find_path(
    AIRSPY_INCLUDE_DIR
    airspy.h
    HINTS ${PC_AIRSPY_INCLUDE_DIRS}
    PATH_SUFFIXES libairspy
)
find_library(AIRSPY_LIBRARY NAMES airspy HINTS ${PC_AIRSPY_LIBRARY_DIRS})
# libairspy.pc reports only the major/minor version, so check the required API
# rather than rejecting usable libraries based on the truncated pkg-config version.
if(AIRSPY_LIBRARY AND AIRSPY_INCLUDE_DIR)
    include(CMakePushCheckState)
    include(CheckCSourceCompiles)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_INCLUDES "${AIRSPY_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_LIBRARIES "${AIRSPY_LIBRARY}")
    unset(AIRSPY_API_OK CACHE)
    check_c_source_compiles(
        "#include <airspy.h>
int main(void) {
    struct airspy_device* device = 0;
    uint64_t serial = 0;
    uint32_t rates = 0;
    airspy_list_devices(&serial, 1);
    airspy_get_samplerates(device, &rates, 0);
    airspy_set_sensitivity_gain(device, 0);
    airspy_set_linearity_gain(device, 0);
    return airspy_set_sample_type(device, AIRSPY_SAMPLE_FLOAT32_IQ);
}"
        AIRSPY_API_OK
    )
    cmake_pop_check_state()
endif()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    Airspy
    REQUIRED_VARS AIRSPY_LIBRARY AIRSPY_INCLUDE_DIR AIRSPY_API_OK
)
if(Airspy_FOUND AND NOT TARGET Airspy::Airspy)
    add_library(Airspy::Airspy UNKNOWN IMPORTED)
    set_target_properties(
        Airspy::Airspy
        PROPERTIES
            IMPORTED_LOCATION "${AIRSPY_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AIRSPY_INCLUDE_DIR}"
    )
endif()
mark_as_advanced(AIRSPY_LIBRARY AIRSPY_INCLUDE_DIR)
