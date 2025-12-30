#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ALLOW_FILE="${ROOT}/tools/arch_guardrails.allow"

declare -A ALLOWLIST=()
if [[ -f "${ALLOW_FILE}" ]]; then
  while IFS= read -r raw || [[ -n "${raw}" ]]; do
    line="${raw%%#*}"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "${line}" ]] && continue
    ALLOWLIST["${line}"]=1
  done < "${ALLOW_FILE}"
fi

print_rules() {
  cat >&2 <<'EOF'
Architecture guardrails (dependency direction by convention):
  1) src/dsp/** and include/dsd-neo/dsp/** must not include <dsd-neo/protocol/...>
  2) src/protocol/** and include/dsd-neo/protocol/** must not include <dsd-neo/ui/...>
  3) include/dsd-neo/core/** must not include <dsd-neo/protocol/...> (allowlist exceptions only)
Allowlist: tools/arch_guardrails.allow (one repo-relative path per line; '#' comments).
EOF
}

collect_files() {
  local dir="$1"
  [[ -d "${dir}" ]] || return 0
  find "${dir}" -type f \( \
    -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o \
    -name '*.h' -o -name '*.hpp' \
  \) -print0
}

scan_rule() {
  local rule_name="$1"
  local grep_regex="$2"
  shift 2

  local -a roots=("$@")
  local -a files=()

  for root in "${roots[@]}"; do
    while IFS= read -r -d '' file; do
      rel="${file#${ROOT}/}"
      if [[ -n "${ALLOWLIST[${rel}]:-}" ]]; then
        continue
      fi
      files+=("${file}")
    done < <(collect_files "${root}")
  done

  [[ "${#files[@]}" -eq 0 ]] && return 0

  matches="$(LC_ALL=C grep -nH -E "${grep_regex}" "${files[@]}" || true)"
  if [[ -n "${matches}" ]]; then
    if [[ "${ANY_VIOLATION}" -eq 0 ]]; then
      echo "arch_guardrails: FAIL" >&2
      print_rules
      echo >&2
      ANY_VIOLATION=1
    fi

    echo "Violation: ${rule_name}" >&2
    echo "${matches}" | sed "s|^${ROOT}/||" >&2
    echo >&2
  fi
}

ANY_VIOLATION=0

scan_rule \
  "DSP must not include protocol headers" \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*<dsd-neo/protocol/' \
  "${ROOT}/src/dsp" \
  "${ROOT}/include/dsd-neo/dsp"

scan_rule \
  "Protocol must not include UI headers" \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*<dsd-neo/ui/' \
  "${ROOT}/src/protocol" \
  "${ROOT}/include/dsd-neo/protocol"

scan_rule \
  "Core public headers must not include protocol headers" \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*<dsd-neo/protocol/' \
  "${ROOT}/include/dsd-neo/core"

if [[ "${ANY_VIOLATION}" -ne 0 ]]; then
  exit 1
fi

echo "arch_guardrails: OK"
