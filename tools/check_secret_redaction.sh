#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$repo_root"

if ! command -v rg > /dev/null 2>&1; then
  echo "ripgrep is required for secret redaction guardrail checks." >&2
  exit 2
fi

secret_terms='[Kk]ey|KEY|KS|keystream|Scrambler|Encryption|Privacy|RC4|DES|AES|K[0-9]'
format_spec='%[#0 +*.0-9-]*(ll|l|z|j)?[Xxdu]'
output_call='LOG_[A-Z]+|DSD_FPRINTF|\b(f?printf|printw|mvprintw|wprintw)\s*\('

violations=$(
  rg -n \
    -e "(${output_call}).*(${secret_terms}).*${format_spec}" \
    -e "(${output_call}).*${format_spec}.*(${secret_terms})" \
    src include apps --glob '!src/third_party/**' |
    grep -Ev \
      'DSD_SECRET_REDACTED|KEY ID|Key ID|key ID|key id|ALG ID|MI\(|LFSR MI|DSD_KEY_SPEC|Scrambler - Type|Scrambler - %d|invalid key|duplicate key|out-of-range|expected key_hex|key->keystream mappings|empty key or keystream|exceeds capacity|MK:|UNK[0-9]|BPARMS|BParms|Reserved Value|parse failed|SAP|AN=%d|payload_keyid|payload_algid|Cipher: %u; Key ID' ||
    true
)

if [[ -n "$violations" ]]; then
  echo "Potential secret material is printed without DSD_SECRET_REDACTED:" >&2
  printf '%s\n' "$violations" >&2
  exit 1
fi
