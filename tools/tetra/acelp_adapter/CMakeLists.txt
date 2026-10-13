cmake_minimum_required(VERSION 3.20)
project(tetra_acelp_adapter C)

set(ETSI_TETRA_C_CODE "" CACHE PATH "Directory containing the locally obtained ETSI C-CODE sources")
if(NOT EXISTS "${ETSI_TETRA_C_CODE}/SOURCE.H")
  message(FATAL_ERROR "Set ETSI_TETRA_C_CODE to the ETSI C-CODE directory (prepared for this compiler)")
endif()

add_executable(tetra-acelp-adapter
  main.c
  "${ETSI_TETRA_C_CODE}/SDEC_TET.C"
  "${ETSI_TETRA_C_CODE}/SUB_SC_D.C"
  "${ETSI_TETRA_C_CODE}/SUB_DSP.C"
  "${ETSI_TETRA_C_CODE}/FBAS_TET.C"
  "${ETSI_TETRA_C_CODE}/FEXP_TET.C"
  "${ETSI_TETRA_C_CODE}/FMAT_TET.C"
  "${ETSI_TETRA_C_CODE}/TETRA_OP.C")
option(BUILD_REFERENCE_SDECODER "Build ETSI file decoder for local PCM comparison" OFF)
if(BUILD_REFERENCE_SDECODER)
  set_source_files_properties("${ETSI_TETRA_C_CODE}/SDECODER.C" PROPERTIES LANGUAGE C)
  add_executable(tetra-reference-sdecoder
    "${ETSI_TETRA_C_CODE}/SDECODER.C"
    "${ETSI_TETRA_C_CODE}/SDEC_TET.C"
    "${ETSI_TETRA_C_CODE}/SUB_SC_D.C"
    "${ETSI_TETRA_C_CODE}/SUB_DSP.C"
    "${ETSI_TETRA_C_CODE}/FBAS_TET.C"
    "${ETSI_TETRA_C_CODE}/FEXP_TET.C"
    "${ETSI_TETRA_C_CODE}/FMAT_TET.C"
    "${ETSI_TETRA_C_CODE}/TETRA_OP.C")
  target_include_directories(tetra-reference-sdecoder PRIVATE "${ETSI_TETRA_C_CODE}")
  if(MSVC)
    target_compile_definitions(tetra-reference-sdecoder PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
endif()
set_source_files_properties(
  "${ETSI_TETRA_C_CODE}/SDEC_TET.C" "${ETSI_TETRA_C_CODE}/SUB_SC_D.C"
  "${ETSI_TETRA_C_CODE}/SUB_DSP.C" "${ETSI_TETRA_C_CODE}/FBAS_TET.C"
  "${ETSI_TETRA_C_CODE}/FEXP_TET.C" "${ETSI_TETRA_C_CODE}/FMAT_TET.C"
  "${ETSI_TETRA_C_CODE}/TETRA_OP.C" PROPERTIES LANGUAGE C)
target_include_directories(tetra-acelp-adapter PRIVATE "${ETSI_TETRA_C_CODE}")
if(MSVC)
  target_compile_definitions(tetra-acelp-adapter PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
