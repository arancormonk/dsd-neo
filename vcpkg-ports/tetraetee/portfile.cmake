# Pinned for reproducible vcpkg builds. See docs/supply-chain-guardrails.md
# for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MidnightBlueLabs/TETRA_etee
    REF 0660c85c35b654fb1cad43c2a299c7f81e9d8681
    SHA512 7911aff95c360718237108f34c182a1f0ca2959e51607d45c5b4c25eb5ee6c820e543aa46abaae1aaede85e90b31c77f767ced9e4b24d06f4e1abf69cf2fe9f4
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

# Upstream is GNU C. MSVC rejects the statement-expression min() and the
# variable-length arrays. Patch once here: debug and release configure in
# parallel and must not rewrite the same sources. The sizes are the maxima
# already enforced by the algorithm table (16-byte blocks, 32-byte keys,
# 160-byte KMM buffers) and by the TEK seal length (at most 48 bytes).
file(READ "${SOURCE_PATH}/common.h" _tetraetee_common)
string(
    REGEX REPLACE
    "#define min\\(a,b\\)[^}]*\\}\\)"
    "#ifdef min\n#undef min\n#endif\n#define min(a, b) ((a) < (b) ? (a) : (b))"
    _tetraetee_common
    "${_tetraetee_common}"
)
if(_tetraetee_common MATCHES "__typeof__")
    message(FATAL_ERROR "tetraetee common.h still contains the GNU min() macro")
endif()
file(WRITE "${SOURCE_PATH}/common.h" "${_tetraetee_common}")

file(READ "${SOURCE_PATH}/etee.c" _tetraetee_src)
macro(tetraetee_replace from to)
    string(FIND "${_tetraetee_src}" "${from}" _tetraetee_found)
    if(_tetraetee_found EQUAL -1)
        message(FATAL_ERROR "tetraetee etee.c is missing: ${from}")
    endif()
    string(REPLACE "${from}" "${to}" _tetraetee_src "${_tetraetee_src}")
endmacro()
tetraetee_replace("uint8_t abBuf[lpstAlg->bBlockSize];" "uint8_t abBuf[16];")
tetraetee_replace("uint8_t abUnsealed[lpTekAlg->bKeySize + 4];" "uint8_t abUnsealed[48];")
tetraetee_replace("uint8_t abBuf[lpCtx->dwBufLen];" "uint8_t abBuf[160];")
tetraetee_replace("uint8_t abXorredTek[lpstAlg->bKeySize];" "uint8_t abXorredTek[32];")
tetraetee_replace("uint8_t abMacKey[lpstAlg->bKeySize];" "uint8_t abMacKey[32];")
tetraetee_replace(
    "uint8_t abMacKey[dwKeylenBytes], abXorredTek[dwKeylenBytes];"
    "uint8_t abMacKey[32], abXorredTek[32];"
)
file(WRITE "${SOURCE_PATH}/etee.c" "${_tetraetee_src}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME tetraetee CONFIG_PATH share/tetraetee)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
