#!/usr/bin/env bash
# Install the tetracrypto and tetraetee CMake packages for builds that do not
# use the vcpkg manifest (Linux and macOS). Windows and Android presets install
# the same ports through vcpkg.json.
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <prefix> [cmake-args...]" >&2
  exit 2
fi

prefix=$1
shift
extra=("$@")

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck source=tools/ci-dependency-pins.env
# shellcheck disable=SC1091
. "$root/tools/ci-dependency-pins.env"

: "${TETRA_CRYPTO_SHA:?TETRA_CRYPTO_SHA is required}"
: "${TETRA_ETEE_SHA:?TETRA_ETEE_SHA is required}"

stamp="$prefix/.tetra-crypto-deps"
want="crypto=$TETRA_CRYPTO_SHA etee=$TETRA_ETEE_SHA"
if [[ -f "$stamp" ]] && [[ $(cat "$stamp") == "$want" ]]; then
  echo "TETRA crypto dependencies already installed in $prefix"
  exit 0
fi

install_port() {
  local name=$1
  local url=$2
  local sha=$3
  local src
  src=$(mktemp -d)
  "$root/tools/fetch-pinned-git.sh" "$url" "$sha" "$src"
  cp "$root/vcpkg-ports/$name/CMakeLists.txt" "$src/CMakeLists.txt"
  if [[ -f "$root/vcpkg-ports/$name/gcc_compat.h" ]]; then
    cp "$root/vcpkg-ports/$name/gcc_compat.h" "$src/gcc_compat.h"
  fi
  cmake -S "$src" -B "$src/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    "${extra[@]}"
  cmake --build "$src/build" --parallel
  cmake --install "$src/build"
  rm -rf "$src"
}

install_port tetracrypto "https://github.com/MidnightBlueLabs/TETRA_crypto.git" "$TETRA_CRYPTO_SHA"
install_port tetraetee "https://github.com/MidnightBlueLabs/TETRA_etee.git" "$TETRA_ETEE_SHA"

mkdir -p "$prefix"
printf '%s\n' "$want" > "$stamp"
