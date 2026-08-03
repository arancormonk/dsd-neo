#
# vcpkg dependency contract check (cross-platform, CMake-only).
#
# The Android and Windows presets both take Codec2 and libcurl from the vcpkg
# manifest and both require them. Codec2 and libcurl are otherwise silent
# auto-detections that still link when absent, so losing one degrades M17 voice
# or rdio uploads with nothing failing: on Android nothing on the build host can
# smoke-test the APK, and on Windows the same regression would only show up in
# a user's hands. Pin the manifest filters and the preset settings here.
#
# Usage:
#   cmake -P cmake/vcpkg_dependency_contract.cmake
#

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    message(
        FATAL_ERROR
        "VCPKG_DEPENDENCY_CONTRACT: must be run via 'cmake -P <script>'"
    )
endif()

get_filename_component(_ADC_SCRIPT_DIR "${CMAKE_SCRIPT_MODE_FILE}" DIRECTORY)
get_filename_component(_ADC_ROOT_DIR "${_ADC_SCRIPT_DIR}" DIRECTORY)

set(_ADC_ERRORS "")

function(_adc_error message)
    set(_ADC_ERRORS "${_ADC_ERRORS}\n  - ${message}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# vcpkg.json: the ports the Android cross build depends on
# ---------------------------------------------------------------------------
set(_ADC_MANIFEST "${_ADC_ROOT_DIR}/vcpkg.json")
if(NOT EXISTS "${_ADC_MANIFEST}")
    message(FATAL_ERROR "VCPKG_DEPENDENCY_CONTRACT: missing ${_ADC_MANIFEST}")
endif()
file(READ "${_ADC_MANIFEST}" _ADC_MANIFEST_JSON)

string(JSON _ADC_DEP_COUNT LENGTH "${_ADC_MANIFEST_JSON}" dependencies)
math(EXPR _ADC_DEP_LAST "${_ADC_DEP_COUNT} - 1")

# name -> platform expression ("" when unrestricted, "<absent>" when missing)
set(_ADC_CODEC2_PLATFORM "<absent>")
set(_ADC_CURL_PLATFORM "<absent>")

foreach(_adc_i RANGE 0 ${_ADC_DEP_LAST})
    string(JSON _adc_entry GET "${_ADC_MANIFEST_JSON}" dependencies ${_adc_i})
    string(JSON _adc_type TYPE "${_ADC_MANIFEST_JSON}" dependencies ${_adc_i})
    if(_adc_type STREQUAL "STRING")
        set(_adc_name "${_adc_entry}")
        set(_adc_platform "")
    else()
        string(JSON _adc_name GET "${_adc_entry}" name)
        string(
            JSON _adc_platform
            ERROR_VARIABLE _adc_platform_err
            GET "${_adc_entry}"
            platform
        )
        if(_adc_platform_err)
            set(_adc_platform "")
        endif()
    endif()

    if(_adc_name STREQUAL "codec2")
        set(_ADC_CODEC2_PLATFORM "${_adc_platform}")
    elseif(_adc_name STREQUAL "curl")
        set(_ADC_CURL_PLATFORM "${_adc_platform}")
    endif()
endforeach()

# Codec2 must reach Android and must stay off Linux/macOS, which use the system
# package. Windows shares the manifest, so it has to remain listed there too.
if(_ADC_CODEC2_PLATFORM STREQUAL "<absent>")
    _adc_error("vcpkg.json: codec2 is not declared; the Android and Windows builds require it")
elseif(NOT _ADC_CODEC2_PLATFORM MATCHES "android")
    _adc_error(
        "vcpkg.json: codec2 has platform \"${_ADC_CODEC2_PLATFORM}\", which does not mention android. "
        "Android needs codec2 for M17 voice (USE_CODEC2)."
    )
elseif(_ADC_CODEC2_PLATFORM MATCHES "!android")
    _adc_error(
        "vcpkg.json: codec2 platform \"${_ADC_CODEC2_PLATFORM}\" excludes android."
    )
endif()

# libcurl gates rdio-scanner uploads (USE_CURL). It carried "!android" during the
# initial port; a reintroduced filter would silently drop the feature.
if(_ADC_CURL_PLATFORM STREQUAL "<absent>")
    _adc_error("vcpkg.json: curl is not declared; the Android and Windows builds require it")
elseif(NOT _ADC_CURL_PLATFORM STREQUAL "")
    _adc_error(
        "vcpkg.json: curl must stay unrestricted, but carries platform \"${_ADC_CURL_PLATFORM}\". "
        "Android needs libcurl for rdio API uploads (USE_CURL)."
    )
endif()

# ---------------------------------------------------------------------------
# CMakePresets.json: every vcpkg-driven preset must demand what the manifest
# ships. Only the Android and Windows presets use the vcpkg toolchain; Linux and
# macOS resolve these from system packages and stay auto-detected.
# ---------------------------------------------------------------------------
set(_ADC_PRESETS "${_ADC_ROOT_DIR}/CMakePresets.json")
if(NOT EXISTS "${_ADC_PRESETS}")
    message(FATAL_ERROR "VCPKG_DEPENDENCY_CONTRACT: missing ${_ADC_PRESETS}")
endif()
file(READ "${_ADC_PRESETS}" _ADC_PRESETS_JSON)

string(JSON _ADC_PRESET_COUNT LENGTH "${_ADC_PRESETS_JSON}" configurePresets)
math(EXPR _ADC_PRESET_LAST "${_ADC_PRESET_COUNT} - 1")

set(_ADC_SEEN_PRESETS "")

foreach(_adc_i RANGE 0 ${_ADC_PRESET_LAST})
    string(
        JSON _adc_preset
        GET "${_ADC_PRESETS_JSON}"
        configurePresets
        ${_adc_i}
    )
    string(JSON _adc_preset_name GET "${_adc_preset}" name)
    if(NOT _adc_preset_name MATCHES "^(android|win-msvc)")
        continue()
    endif()
    list(APPEND _ADC_SEEN_PRESETS "${_adc_preset_name}")

    string(
        JSON _adc_cache
        ERROR_VARIABLE _adc_cache_err
        GET "${_adc_preset}"
        cacheVariables
    )
    if(_adc_cache_err)
        _adc_error(
            "CMakePresets.json: preset ${_adc_preset_name} has no cacheVariables"
        )
        continue()
    endif()

    foreach(_adc_var DSD_REQUIRE_CODEC2 DSD_REQUIRE_CURL)
        string(
            JSON _adc_value
            ERROR_VARIABLE _adc_value_err
            GET "${_adc_cache}"
            ${_adc_var}
        )
        if(_adc_value_err OR NOT _adc_value STREQUAL "ON")
            _adc_error(
                "CMakePresets.json: preset ${_adc_preset_name} must set ${_adc_var}=ON so a lost "
                "dependency fails configure instead of shipping a degraded build."
            )
        endif()
    endforeach()

    # The require options and the disable switch are mutually exclusive; setting
    # both in a preset would fail configure for a confusing reason.
    foreach(
        _adc_var
        CMAKE_DISABLE_FIND_PACKAGE_CODEC2
        CMAKE_DISABLE_FIND_PACKAGE_CURL
    )
        string(
            JSON _adc_value
            ERROR_VARIABLE _adc_value_err
            GET "${_adc_cache}"
            ${_adc_var}
        )
        if(NOT _adc_value_err)
            _adc_error(
                "CMakePresets.json: preset ${_adc_preset_name} must not set ${_adc_var}; it contradicts "
                "the require options."
            )
        endif()
    endforeach()
endforeach()

foreach(
    _adc_expected
    android-arm64-release
    android-app
    win-msvc-debug
    win-msvc-release
)
    if(NOT _adc_expected IN_LIST _ADC_SEEN_PRESETS)
        _adc_error(
            "CMakePresets.json: expected a vcpkg-driven preset named ${_adc_expected}"
        )
    endif()
endforeach()

if(NOT _ADC_ERRORS STREQUAL "")
    message(FATAL_ERROR "VCPKG_DEPENDENCY_CONTRACT violations:${_ADC_ERRORS}")
endif()

message(STATUS "VCPKG_DEPENDENCY_CONTRACT: OK")
