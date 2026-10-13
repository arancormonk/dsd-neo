# A TETRA speech codec library for dsd-neo's --tetra-codec, built from a locally obtained copy of the ETSI
# EN 300 395-2 reference C code. This is a separate project: the dsd-neo build never compiles it. See README.md.
cmake_minimum_required(VERSION 3.20)
project(tetra_codec_etsi C)

set(ETSI_TETRA_C_CODE
    ""
    CACHE PATH
    "Directory containing the locally obtained ETSI C-CODE sources"
)
option(
    BUILD_REFERENCE_SDECODER
    "Also build the ETSI file decoder, to compare PCM locally"
    OFF
)

# The ETSI archive ships upper-case file names; a tree renamed to lower case works too.
function(etsi_tetra_source out_var name)
    string(TOLOWER "${name}" _lower)
    if(EXISTS "${ETSI_TETRA_C_CODE}/${name}")
        set(${out_var} "${ETSI_TETRA_C_CODE}/${name}" PARENT_SCOPE)
    elseif(EXISTS "${ETSI_TETRA_C_CODE}/${_lower}")
        set(${out_var} "${ETSI_TETRA_C_CODE}/${_lower}" PARENT_SCOPE)
    else()
        message(
            FATAL_ERROR
            "ETSI_TETRA_C_CODE must name the ETSI C-CODE directory: ${name} was not found in '${ETSI_TETRA_C_CODE}'"
        )
    endif()
endfunction()

set(_etsi_decoder_sources)
foreach(
    _name
    SDEC_TET.C
    SUB_SC_D.C
    SUB_DSP.C
    FBAS_TET.C
    FEXP_TET.C
    FMAT_TET.C
    TETRA_OP.C
)
    etsi_tetra_source(_path "${_name}")
    list(APPEND _etsi_decoder_sources "${_path}")
endforeach()
etsi_tetra_source(_etsi_header SOURCE.H)
set_source_files_properties(${_etsi_decoder_sources} PROPERTIES LANGUAGE C)

# libtetra-codec.so, libtetra-codec.dylib or tetra-codec.dll: the file to pass to --tetra-codec.
add_library(tetra-codec SHARED tetra_codec_etsi.c ${_etsi_decoder_sources})
target_include_directories(
    tetra-codec
    PRIVATE
        "${ETSI_TETRA_C_CODE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
)
set_target_properties(tetra-codec PROPERTIES C_VISIBILITY_PRESET hidden)
get_filename_component(_etsi_header_name "${_etsi_header}" NAME)
target_compile_definitions(
    tetra-codec
    PRIVATE "ETSI_SOURCE_HEADER=\"${_etsi_header_name}\""
)
if(MSVC)
    target_compile_definitions(tetra-codec PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(BUILD_REFERENCE_SDECODER)
    etsi_tetra_source(_etsi_sdecoder SDECODER.C)
    set_source_files_properties("${_etsi_sdecoder}" PROPERTIES LANGUAGE C)
    add_executable(
        tetra-reference-sdecoder
        "${_etsi_sdecoder}"
        ${_etsi_decoder_sources}
    )
    target_include_directories(
        tetra-reference-sdecoder
        PRIVATE "${ETSI_TETRA_C_CODE}"
    )
    if(MSVC)
        target_compile_definitions(
            tetra-reference-sdecoder
            PRIVATE _CRT_SECURE_NO_WARNINGS
        )
    endif()
endif()
