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
  # grep -q leaves at its first match, so it reads from a here-string: a pipe
  # writer still writing would end with SIGPIPE, which pipefail reports.
  if grep -Eq '(^|[[:space:]])(MH_)?DYLIB([[:space:]]|$)' <<< "$header"; then
    local install_name
    install_name="$(otool -D "$artifact" | sed -n '2p')"
    if [[ ! "$install_name" == @rpath/* ]]; then
      echo "Mach-O dylib install name must use @rpath; got '${install_name:-<none>}'." >&2
      exit 1
    fi
    return 0
  fi

  if grep -q 'EXECUTE' <<< "$header"; then
    if ! grep -q 'PIE' <<< "$header"; then
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

# Each listing is read in full before grep -q looks at it: grep -q leaves at
# its first match, and a readelf still writing would end with SIGPIPE, which
# pipefail turns into a failed check. A readelf that fails takes the check's
# own failure branch.
if ! elf_header=$(readelf -h "$artifact") || ! grep -Eq 'Type:[[:space:]]*DYN' <<< "$elf_header"; then
  echo "release artifact is not position-independent (ELF type DYN expected)." >&2
  exit 1
fi

if ! program_headers=$(readelf -l "$artifact") || ! grep -q 'GNU_RELRO' <<< "$program_headers"; then
  echo "release artifact is missing GNU_RELRO." >&2
  exit 1
fi

if ! dynamic_section=$(readelf -d "$artifact") || ! grep -Eq 'BIND_NOW|FLAGS_1.*NOW' <<< "$dynamic_section"; then
  echo "release artifact is missing BIND_NOW/full RELRO." >&2
  exit 1
fi
