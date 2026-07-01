#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

stage=${1:-dist/dsd-neo-macos}
exe="$stage/bin/dsd-neo"
lib_dir="$stage/lib"
manifest="$stage/dylibs-manifest.txt"

find_otool() {
  if command -v otool > /dev/null 2>&1; then
    command -v otool
    return 0
  fi
  if command -v llvm-otool > /dev/null 2>&1; then
    command -v llvm-otool
    return 0
  fi
  return 1
}

otool_bin=$(find_otool) || {
  echo "otool or llvm-otool is required for macOS portable dependency checks." >&2
  exit 2
}

if [[ ! -f "$exe" ]]; then
  echo "macOS portable executable not found: $exe" >&2
  exit 2
fi
if [[ ! -d "$lib_dir" ]]; then
  echo "macOS portable library directory not found: $lib_dir" >&2
  exit 2
fi

list_deps() {
  "$otool_bin" -L "$1" | tail -n +2 | awk '{print $1}'
}

list_rpaths() {
  "$otool_bin" -l "$1" | awk '
    /cmd LC_RPATH/ { in_rpath = 1; next }
    in_rpath && /^[[:space:]]*path / { print $2; in_rpath = 0 }
  '
}

resolve_rpath_dep() {
  local file=$1
  local dep=$2
  local suffix=${dep#@rpath/}
  local loader_dir
  local rpath
  local resolved
  local candidate

  loader_dir=$(dirname "$file")
  while IFS= read -r rpath; do
    case "$rpath" in
      @loader_path)
        resolved=$loader_dir
        ;;
      @loader_path/*)
        resolved="$loader_dir/${rpath#@loader_path/}"
        ;;
      @executable_path)
        resolved="$stage/bin"
        ;;
      @executable_path/*)
        resolved="$stage/bin/${rpath#@executable_path/}"
        ;;
      *)
        resolved=$rpath
        ;;
    esac
    candidate="$resolved/$suffix"
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done < <(list_rpaths "$file")

  return 1
}

resolve_token_path() {
  local file=$1
  local dep=$2
  local rel

  case "$dep" in
    @loader_path/*)
      rel=${dep#@loader_path/}
      printf '%s/%s\n' "$(dirname "$file")" "$rel"
      ;;
    @executable_path/*)
      rel=${dep#@executable_path/}
      printf '%s/%s\n' "$stage/bin" "$rel"
      ;;
    *)
      return 1
      ;;
  esac
}

is_system_dep() {
  case "$1" in
    /usr/lib/* | /System/*) return 0 ;;
    *) return 1 ;;
  esac
}

errors=0
report_error() {
  echo "ERROR: $*" >&2
  errors=$((errors + 1))
}

verify_code_signature() {
  local file=$1
  local rel=$2

  case "$(uname -s)" in
    Darwin)
      if ! command -v codesign > /dev/null 2>&1; then
        echo "codesign is required for macOS portable signature checks." >&2
        exit 2
      fi
      if ! codesign --verify --strict --verbose=2 "$file" > /dev/null 2>&1; then
        report_error "$rel has an invalid code signature"
      fi
      ;;
  esac
}

macho_files=("$exe")
while IFS= read -r -d '' lib; do
  macho_files+=("$lib")
done < <(find "$lib_dir" -maxdepth 1 -type f -name '*.dylib' -print0)

for file in "${macho_files[@]}"; do
  rel=${file#"$stage"/}
  verify_code_signature "$file" "$rel"

  while IFS= read -r rpath; do
    [[ -z "$rpath" ]] && continue
    case "$rpath" in
      /*) report_error "$rel has non-portable LC_RPATH: $rpath" ;;
    esac
  done < <(list_rpaths "$file")

  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    if is_system_dep "$dep"; then
      continue
    fi

    case "$dep" in
      @rpath/*)
        base=${dep##*/}
        if [[ "$file" == "$lib_dir/"* && "$base" == "$(basename "$file")" ]]; then
          continue
        fi
        if [[ ! -f "$lib_dir/$base" ]]; then
          report_error "$rel loads $dep but $lib_dir/$base is missing"
        elif ! resolve_rpath_dep "$file" "$dep" > /dev/null; then
          report_error "$rel loads $dep but no LC_RPATH resolves it to the staged lib directory"
        fi
        ;;
      @loader_path/* | @executable_path/*)
        resolved=$(resolve_token_path "$file" "$dep")
        if [[ ! -f "$resolved" ]]; then
          report_error "$rel loads $dep but $resolved is missing"
        fi
        ;;
      /*)
        report_error "$rel has non-system absolute dependency: $dep"
        ;;
      *)
        report_error "$rel has unsupported dependency path: $dep"
        ;;
    esac
  done < <(list_deps "$file")
done

if [[ -f "$manifest" ]]; then
  manifest_expected=$(mktemp)
  manifest_actual=$(mktemp)
  trap 'rm -f "$manifest_expected" "$manifest_actual"' EXIT
  sort -f "$manifest" > "$manifest_expected"
  find "$lib_dir" -maxdepth 1 -type f -name '*.dylib' -exec basename {} \; | sort -f > "$manifest_actual"
  if ! diff -u "$manifest_expected" "$manifest_actual" >&2; then
    report_error "dylibs-manifest.txt does not match staged lib directory"
  fi
else
  report_error "missing dylibs manifest: $manifest"
fi

if ((errors > 0)); then
  echo "macOS portable dependency check failed with $errors issue(s)." >&2
  exit 1
fi

echo "macOS portable dependency check passed: $stage"
