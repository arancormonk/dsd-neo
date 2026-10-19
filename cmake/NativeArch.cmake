# SPDX-License-Identifier: GPL-3.0-or-later
include_guard(GLOBAL)

# Host CPU tuning for Release-like builds (DSD_ENABLE_NATIVE).
#
# A local Release build is tuned for the machine that compiles it: x86 gets
# -march=native, ARM/AArch64 -mcpu=native, and MSVC/clang-cl the highest
# /arch: level a configure-time CPUID probe finds. Tuning is skipped, with the
# reason in a STATUS message, whenever the binary may run on another machine or
# the user already chose a target ISA. Packagers and CI that ship binaries pass
# -DDSD_ENABLE_NATIVE=OFF.

set(_DSD_NATIVE_ARCH_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Configurations that get host tuning, as a generator-expression condition.
set(DSD_NATIVE_ARCH_CONFIGS "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
set(_DSD_NATIVE_ARCH_TUNED_CONFIGS RELEASE RELWITHDEBINFO MINSIZEREL)

# Flag forms that pick a target ISA. A user choice wins over host tuning.
set(_DSD_NATIVE_ARCH_USER_ISA_REGEX
    "(^|[ \t])(-march=|-mcpu=|[-/][Aa][Rr][Cc][Hh]:|-arch[ \t])"
)

# dsd_native_arch_skip_reason(<out-var>)
#
# Sets <out-var> to why host tuning must not apply, or to "" when it may. Only
# reads variables, so it also runs in script mode (cmake -P) for tests.
function(dsd_native_arch_skip_reason out_var)
    set(_reason "")
    if(CMAKE_CROSSCOMPILING)
        set(_reason "cross-compiling for ${CMAKE_SYSTEM_NAME}")
    elseif(
        CMAKE_SYSTEM_NAME
            MATCHES
            "^(Android|iOS|tvOS|watchOS|visionOS|Emscripten)$"
    )
        set(_reason "target system ${CMAKE_SYSTEM_NAME}")
    elseif(EMSCRIPTEN)
        set(_reason "target system Emscripten")
    endif()

    if(_reason STREQUAL "" AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        list(LENGTH CMAKE_OSX_ARCHITECTURES _osx_count)
        if(_osx_count GREATER 1)
            set(_reason
                "CMAKE_OSX_ARCHITECTURES names several architectures (${CMAKE_OSX_ARCHITECTURES})"
            )
        elseif(
            _osx_count EQUAL 1
            AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR
        )
            set(_reason
                "CMAKE_OSX_ARCHITECTURES (${CMAKE_OSX_ARCHITECTURES}) is not the host architecture (${CMAKE_HOST_SYSTEM_PROCESSOR})"
            )
        endif()
    endif()

    if(_reason STREQUAL "")
        foreach(_lang IN ITEMS C CXX)
            if(CMAKE_${_lang}_COMPILER_TARGET)
                set(_reason
                    "CMAKE_${_lang}_COMPILER_TARGET selects ${CMAKE_${_lang}_COMPILER_TARGET}"
                )
                break()
            endif()
            foreach(_suffix IN ITEMS "" _RELEASE _RELWITHDEBINFO _MINSIZEREL)
                set(_var "CMAKE_${_lang}_FLAGS${_suffix}")
                if("${${_var}}" MATCHES "${_DSD_NATIVE_ARCH_USER_ISA_REGEX}")
                    set(_reason "${_var} already selects a target ISA")
                    break()
                endif()
            endforeach()
            if(NOT _reason STREQUAL "")
                break()
            endif()
        endforeach()
    endif()
    set(${out_var} "${_reason}" PARENT_SCOPE)
endfunction()

# Highest /arch: level the build host runs, from a CPUID/XGETBV probe built
# with the project's own C compiler. Cached so it runs once per build tree.
function(_dsd_native_arch_msvc_level out_var)
    if(NOT DEFINED DSD_NATIVE_ARCH_MSVC_PROBE)
        set(_bindir "${CMAKE_BINARY_DIR}/CMakeFiles/dsd_native_arch_probe")
        try_run(
            _run_result
            _compile_result
            "${_bindir}"
            "${_DSD_NATIVE_ARCH_DIR}/native_arch_probe.c"
            COMPILE_OUTPUT_VARIABLE _compile_output
            RUN_OUTPUT_VARIABLE _run_output
        )
        set(_level "NONE")
        if(_compile_result AND _run_result EQUAL 0)
            string(STRIP "${_run_output}" _run_output)
            if(_run_output MATCHES "^(AVX512|AVX2|AVX|NONE)$")
                set(_level "${_run_output}")
            endif()
        endif()
        set(DSD_NATIVE_ARCH_MSVC_PROBE
            "${_level}"
            CACHE INTERNAL
            "Host /arch: level found by the native CPU probe"
        )
    endif()
    set(${out_var} "${DSD_NATIVE_ARCH_MSVC_PROBE}" PARENT_SCOPE)
endfunction()

# dsd_native_arch_flags(<flags-var> <reason-var>)
#
# Sets <flags-var> to the host-tuning flags for the enabled C/C++ compilers, or
# to "" with <reason-var> saying why none apply.
function(dsd_native_arch_flags flags_var reason_var)
    set(_flags "")
    dsd_native_arch_skip_reason(_reason)

    get_property(_enabled GLOBAL PROPERTY ENABLED_LANGUAGES)
    set(_langs "")
    foreach(_lang IN ITEMS C CXX)
        if(_lang IN_LIST _enabled)
            list(APPEND _langs ${_lang})
        endif()
    endforeach()
    if(_reason STREQUAL "" AND NOT _langs)
        set(_reason "no C or C++ compiler enabled")
    endif()

    if(_reason STREQUAL "")
        list(GET _langs 0 _lang0)
        set(_id "${CMAKE_${_lang0}_COMPILER_ID}")
        set(_frontend "${CMAKE_${_lang0}_COMPILER_FRONTEND_VARIANT}")
        set(_processor "${CMAKE_SYSTEM_PROCESSOR}")
        if(MSVC OR _frontend STREQUAL "MSVC")
            set(_arch_id "${CMAKE_${_lang0}_COMPILER_ARCHITECTURE_ID}")
            if(NOT _arch_id)
                set(_arch_id "${MSVC_${_lang0}_ARCHITECTURE_ID}")
            endif()
            if(NOT _arch_id MATCHES "^(x64|X86|x86)$")
                set(_reason
                    "MSVC host tuning covers x86/x64 targets only (target ${_arch_id})"
                )
            else()
                _dsd_native_arch_msvc_level(_level)
                if(_level STREQUAL "NONE")
                    set(_reason
                        "the CPU probe found no AVX support on this host"
                    )
                else()
                    set(_flags "/arch:${_level}")
                endif()
            endif()
        elseif(_id MATCHES "^(GNU|Clang|AppleClang|IntelLLVM)$")
            if(_processor MATCHES "^([xX]86_64|AMD64|amd64|i[3-6]86|x86)$")
                set(_flags "-march=native")
            elseif(_processor MATCHES "^(aarch64|arm64|ARM64|arm.*)$")
                set(_flags "-mcpu=native")
            else()
                set(_reason "no host tuning rule for processor ${_processor}")
            endif()
        else()
            set(_reason "no host tuning rule for compiler ${_id}")
        endif()
    endif()

    # The compiler must accept the flag without a warning (the checks treat
    # "unsupported"/"unused argument" diagnostics as failure).
    if(_reason STREQUAL "" AND _flags)
        include(CheckCCompilerFlag)
        include(CheckCXXCompilerFlag)
        string(MAKE_C_IDENTIFIER "${_flags}" _flag_id)
        foreach(_lang IN LISTS _langs)
            set(_result "DSD_NATIVE_ARCH_${_lang}_${_flag_id}")
            if(_lang STREQUAL "C")
                check_c_compiler_flag("${_flags}" ${_result})
            else()
                check_cxx_compiler_flag("${_flags}" ${_result})
            endif()
            if(NOT ${_result})
                set(_reason "the ${_lang} compiler rejects ${_flags}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT _reason STREQUAL "")
        set(_flags "")
    endif()
    set(${flags_var} "${_flags}" PARENT_SCOPE)
    set(${reason_var} "${_reason}" PARENT_SCOPE)
endfunction()

# dsd_native_arch_apply(<enabled>)
#
# Adds the host-tuning flags as directory-scoped compile options for C and C++
# sources in Release, RelWithDebInfo and MinSizeRel. Debug builds stay on the
# compiler's baseline target. Sets DSD_NATIVE_ARCH_FLAGS and
# DSD_NATIVE_ARCH_SKIP_REASON in the caller's scope; sources that add their own
# /arch: (MSVC allows one per compile) leave it out in DSD_NATIVE_ARCH_CONFIGS
# when DSD_NATIVE_ARCH_FLAGS holds one.
function(dsd_native_arch_apply enabled)
    set(_flags "")
    if(NOT enabled)
        set(_reason "DSD_ENABLE_NATIVE=OFF")
    else()
        dsd_native_arch_flags(_flags _reason)
    endif()

    if(_flags)
        foreach(_flag IN LISTS _flags)
            add_compile_options(
                "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,${DSD_NATIVE_ARCH_CONFIGS}>:${_flag}>"
            )
        endforeach()
        get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type)
        if(
            NOT _multi
            AND NOT _build_type IN_LIST _DSD_NATIVE_ARCH_TUNED_CONFIGS
        )
            set(_this "this ${CMAKE_BUILD_TYPE} build")
            if(_build_type STREQUAL "")
                set(_this "a build without CMAKE_BUILD_TYPE")
            endif()
            message(
                STATUS
                "Native CPU tuning: ${_flags} for Release-like configs, not ${_this}"
            )
        else()
            message(
                STATUS
                "Native CPU tuning: ${_flags} (Release, RelWithDebInfo, MinSizeRel)"
            )
        endif()
    else()
        message(STATUS "Native CPU tuning: off (${_reason})")
    endif()
    set(DSD_NATIVE_ARCH_FLAGS "${_flags}" PARENT_SCOPE)
    set(DSD_NATIVE_ARCH_SKIP_REASON "${_reason}" PARENT_SCOPE)
endfunction()
