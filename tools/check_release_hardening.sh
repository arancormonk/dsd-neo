#!/usr/bin/env bash
set -euo pipefail

artifact=${1:-build/dev-release/apps/dsd-cli/dsd-neo}
build_dir=${2:-build/dev-release}
compile_db="${build_dir}/compile_commands.json"

if [[ ! -e "$artifact" ]]; then
  echo "release artifact not found: $artifact" >&2
  exit 2
fi

if [[ -f "$compile_db" ]]; then
  if ! grep -q -- '-D_FORTIFY_SOURCE=2' "$compile_db"; then
    echo "release compile commands are missing -D_FORTIFY_SOURCE=2" >&2
    exit 1
  fi
  if ! grep -q -- '-fstack-protector-strong' "$compile_db"; then
    echo "release compile commands are missing -fstack-protector-strong" >&2
    exit 1
  fi
fi

check_macho_hardening() {
  if ! command -v otool > /dev/null 2>&1; then
    echo "otool is required for Mach-O hardening checks." >&2
    exit 2
  fi

  local header
  header="$(otool -hv "$artifact")"
  if printf '%s\n' "$header" | grep -Eq '(^|[[:space:]])(MH_)?DYLIB([[:space:]]|$)'; then
    local install_name
    install_name="$(otool -D "$artifact" | sed -n '2p')"
    if [[ ! "$install_name" == @rpath/* ]]; then
      echo "Mach-O dylib install name must use @rpath; got '${install_name:-<none>}'." >&2
      exit 1
    fi
    return 0
  fi

  if printf '%s\n' "$header" | grep -q 'EXECUTE'; then
    if ! printf '%s\n' "$header" | grep -q 'PIE'; then
      echo "Mach-O executable is missing PIE flag." >&2
      exit 1
    fi
    return 0
  fi

  echo "Unsupported Mach-O artifact type in $artifact." >&2
  exit 1
}

case "$(uname -s)" in
  Linux) ;;
  Darwin)
    check_macho_hardening
    exit 0
    ;;
  *)
    echo "ELF hardening checks are Linux-only; compile flag checks completed."
    exit 0
    ;;
esac

if ! command -v readelf > /dev/null 2>&1; then
  echo "readelf is required for ELF hardening checks." >&2
  exit 2
fi

if ! readelf -h "$artifact" | grep -Eq 'Type:[[:space:]]*DYN'; then
  echo "release artifact is not position-independent (ELF type DYN expected)." >&2
  exit 1
fi

if ! readelf -l "$artifact" | grep -q 'GNU_RELRO'; then
  echo "release artifact is missing GNU_RELRO." >&2
  exit 1
fi

if ! readelf -d "$artifact" | grep -Eq 'BIND_NOW|FLAGS_1.*NOW'; then
  echo "release artifact is missing BIND_NOW/full RELRO." >&2
  exit 1
fi
