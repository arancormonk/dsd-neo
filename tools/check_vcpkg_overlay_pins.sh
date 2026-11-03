#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$repo_root"

usage() {
  cat << 'USAGE'
Usage: tools/check_vcpkg_overlay_pins.sh [--fix]

Checks that vcpkg overlay GitHub source pins match tools/ci-dependency-pins.env,
and that the airspy and rtlsdr overlays apply dsd-neo's patch series
(tools/usb_dependency_patches.sh) with its current digest.
With --fix, rewrites mirrored REF and SHA512 lines and the recorded patch digests
in the overlay portfiles.
USAGE
}

fix=0
if [[ $# -gt 0 ]]; then
  case "$1" in
    --fix)
      fix=1
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
  esac
fi

if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

pins_file="tools/ci-dependency-pins.env"
if [[ ! -f "$pins_file" ]]; then
  echo "Missing ${pins_file}." >&2
  exit 1
fi

# shellcheck source=tools/ci-dependency-pins.env
# shellcheck disable=SC1091
source "$pins_file"

failed=0
fixed=0

validate_hex() {
  local label="$1"
  local value="$2"
  local length="$3"

  if [[ ! "$value" =~ ^[0-9a-f]{$length}$ ]]; then
    echo "${label} must be ${length} lowercase hex characters; got '${value}'." >&2
    failed=1
    return 1
  fi
  return 0
}

extract_port_value() {
  local key="$1"
  local portfile="$2"

  awk -v key="$key" '
    $1 == key {
      print $2
      found = 1
      exit
    }
    END {
      if (!found) {
        exit 1
      }
    }
  ' "$portfile"
}

rewrite_portfile() {
  local portfile="$1"
  local ref="$2"
  local sha512="$3"
  local tmp=""

  tmp=$(mktemp)
  awk -v ref="$ref" -v sha512="$sha512" '
    $1 == "REF" && !ref_done {
      sub(/REF[[:space:]]+[[:alnum:]_.${}\/-]+/, "REF " ref)
      ref_done = 1
    }
    $1 == "SHA512" && !sha_done {
      sub(/SHA512[[:space:]]+[0-9A-Fa-f]+/, "SHA512 " sha512)
      sha_done = 1
    }
    { print }
  ' "$portfile" > "$tmp"
  mv "$tmp" "$portfile"
}

check_port() {
  local port="$1"
  local repo="$2"
  local ref_var="$3"
  local sha512_var="$4"
  local portfile="${5:-vcpkg-ports/${port}/portfile.cmake}"
  local want_ref="${!ref_var:-}"
  local want_sha512="${!sha512_var:-}"
  local port_repo=""
  local port_ref=""
  local port_sha512=""
  local port_failed=0
  local pins_valid=1
  local fixable=1

  if [[ ! -f "$portfile" ]]; then
    echo "Missing ${portfile}." >&2
    failed=1
    return
  fi

  validate_hex "$ref_var" "$want_ref" 40 || pins_valid=0
  validate_hex "$sha512_var" "$want_sha512" 128 || pins_valid=0
  if [[ $pins_valid -eq 0 ]]; then
    fixable=0
  fi

  if ! port_repo=$(extract_port_value REPO "$portfile"); then
    echo "${portfile}: missing REPO line." >&2
    failed=1
    port_failed=1
    fixable=0
  elif [[ "$port_repo" != "$repo" ]]; then
    echo "${portfile}: expected REPO ${repo}." >&2
    failed=1
    port_failed=1
    fixable=0
  fi

  if ! port_ref=$(extract_port_value REF "$portfile"); then
    echo "${portfile}: missing REF line." >&2
    failed=1
    port_failed=1
    fixable=0
  elif [[ "$port_ref" != "$want_ref" ]]; then
    echo "${portfile}: REF ${port_ref} does not match ${ref_var}=${want_ref}." >&2
    if [[ $fix -eq 0 ]]; then
      failed=1
    fi
    port_failed=1
  fi

  if ! port_sha512=$(extract_port_value SHA512 "$portfile"); then
    echo "${portfile}: missing SHA512 line." >&2
    failed=1
    port_failed=1
    fixable=0
  elif [[ "$port_sha512" != "$want_sha512" ]]; then
    echo "${portfile}: SHA512 ${port_sha512} does not match ${sha512_var}=${want_sha512}." >&2
    if [[ $fix -eq 0 ]]; then
      failed=1
    fi
    port_failed=1
  fi

  if [[ $fix -eq 1 && $port_failed -eq 1 && $fixable -eq 1 ]]; then
    rewrite_portfile "$portfile" "$want_ref" "$want_sha512"
    echo "Updated ${portfile} from ${pins_file}."
    fixed=1
  elif [[ $fix -eq 1 && $port_failed -eq 1 ]]; then
    echo "Not updating ${portfile}: ${pins_file} has invalid pin values." >&2
  fi
}

# The words of a portfile's set(<name> ...) call, up to its closing parenthesis.
extract_port_set() {
  local name="$1"
  local portfile="$2"

  awk -v name="$name" '
    !in_set && $1 == "set(" name {
      in_set = 1
      found = 1
      $1 = ""
    }
    in_set {
      for (i = 1; i <= NF; i++) {
        word = $i
        done = sub(/\)$/, "", word)
        if (word != "") {
          print word
        }
        if (done) {
          exit
        }
      }
    }
    END {
      if (!found) {
        exit 1
      }
    }
  ' "$portfile"
}

rewrite_port_patch_digest() {
  local portfile="$1"
  local old_digest="$2"
  local new_digest="$3"
  local tmp=""

  tmp=$(mktemp)
  sed "s/${old_digest}/${new_digest}/" "$portfile" > "$tmp"
  mv "$tmp" "$portfile"
}

check_port_patches() {
  local port="$1"
  local lib="$2"
  local portfile="vcpkg-ports/${port}/portfile.cmake"
  local want_series=""
  local want_digest=""
  local port_series=""
  local port_digest=""

  want_series=$(tools/usb_dependency_patches.sh series "$lib")
  want_digest=$(tools/usb_dependency_patches.sh digest "$lib")
  if ! port_series=$(extract_port_set DSD_NEO_PATCH_SERIES "$portfile"); then
    echo "${portfile}: missing set(DSD_NEO_PATCH_SERIES ...); it must apply ${lib}'s series." >&2
    failed=1
    return
  fi
  if [[ "$port_series" != "$want_series" ]]; then
    echo "${portfile}: DSD_NEO_PATCH_SERIES does not list tools/usb_dependency_patches.sh series ${lib}:" >&2
    printf '%s\n' "$want_series" >&2
    failed=1
    return
  fi
  if ! port_digest=$(extract_port_set DSD_NEO_PATCH_SERIES_SHA256 "$portfile"); then
    echo "${portfile}: missing set(DSD_NEO_PATCH_SERIES_SHA256 ...)." >&2
    failed=1
    return
  fi
  if [[ "$port_digest" != "$want_digest" ]]; then
    echo "${portfile}: DSD_NEO_PATCH_SERIES_SHA256 ${port_digest} does not match the ${lib} series (${want_digest})." >&2
    if [[ $fix -eq 1 && "$port_digest" =~ ^[0-9a-f]{64}$ ]]; then
      rewrite_port_patch_digest "$portfile" "$port_digest" "$want_digest"
      echo "Updated ${portfile} patch digest."
      fixed=1
    else
      failed=1
    fi
  fi
}

check_port "mbe-neo" "arancormonk/mbelib-neo" "MBELIB_NEO_SHA" "MBELIB_NEO_TARBALL_SHA512"
check_port "mbe-neo" "arancormonk/pffft" "MBELIB_PFFFT_SHA" "MBELIB_PFFFT_TARBALL_SHA512" "vcpkg-ports/mbe-neo/pffft.cmake"
check_port "codec2" "arancormonk/codec2" "CODEC2_SHA" "CODEC2_TARBALL_SHA512"
check_port "rtlsdr" "arancormonk/rtl-sdr" "RTL_SDR_SHA" "RTL_SDR_TARBALL_SHA512"
check_port "airspy" "airspy/airspyone_host" "AIRSPY_SHA" "AIRSPY_TARBALL_SHA512"
check_port_patches "rtlsdr" "librtlsdr"
check_port_patches "airspy" "libairspy"

if [[ $fix -eq 1 && $fixed -eq 1 && $failed -eq 0 ]]; then
  exit 0
fi

exit "$failed"
