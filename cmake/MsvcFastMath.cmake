# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)

# Apply the fast-math default after all targets and their IEEE exceptions exist.
# Source options come after target options, but MSVC warns (D9025) when those
# options select different /fp modes. Keep explicit modes and add the default
# only to C/C++ sources that have neither a source nor a target override.
function(dsd_msvc_fast_math directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        get_target_property(_type "${_target}" TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()

        # Evaluate in the consuming target's context: one source can be used
        # by both a fast-math target and a precise test in the same directory.
        # GENEX_EVAL also preserves configuration-specific modes, such as
        # PFFFT's /fp:strict in Debug.
        # Snapshot the target's modes instead of referencing COMPILE_OPTIONS
        # from a source option (CMake rejects that as a self-reference).
        get_target_property(_target_options "${_target}" COMPILE_OPTIONS)
        list(
            FILTER _target_options
            INCLUDE
            REGEX "[-/]fp:(fast|precise|strict)"
        )
        set_property(
            TARGET "${_target}"
            PROPERTY DSD_MSVC_FP_OPTIONS "${_target_options}"
        )
        set(_target_modes
            "$<FILTER:$<GENEX_EVAL:$<TARGET_PROPERTY:DSD_MSVC_FP_OPTIONS>>,INCLUDE,^[-/]fp:(fast|precise|strict)$>"
        )
        # Evaluate the language per compile so non-C/C++ sources get no option.
        set(_default
            "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<BOOL:${_target_modes}>>>:/fp:fast>"
        )
        get_target_property(_sources "${_target}" SOURCES)
        foreach(_source IN LISTS _sources)
            # Object-library references are already handled at their target.
            if(_source MATCHES "\\$<")
                continue()
            endif()
            # Resolve relative paths from the target's directory. Keep Windows
            # short-path components: get_filename_component(ABSOLUTE) expands
            # them, which would select a different source-property entry.
            cmake_path(
                ABSOLUTE_PATH _source
                BASE_DIRECTORY "${directory}"
                NORMALIZE
            )
            get_source_file_property(
                _options
                "${_source}"
                TARGET_DIRECTORY "${_target}"
                COMPILE_OPTIONS
            )
            if(
                _options MATCHES "(^|;)[-/]fp:(fast|precise|strict)(;|$)"
                OR "${_default}" IN_LIST _options
            )
                continue()
            endif()
            set_property(
                SOURCE "${_source}" TARGET_DIRECTORY "${_target}"
                APPEND
                PROPERTY COMPILE_OPTIONS "${_default}"
            )
        endforeach()
    endforeach()

    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        dsd_msvc_fast_math("${_child}")
    endforeach()
endfunction()
