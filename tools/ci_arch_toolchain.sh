#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat << 'USAGE'
Usage: tools/ci_arch_toolchain.sh <command> [args...]

Run a command in an Arch Linux container with the rolling C/C++ quality
toolchain used by local preflight checks.

Environment:
  CI_ARCH_IMAGE              Container image to use (default: pinned ARCHLINUX_BASE_DEVEL_IMAGE).
  CI_ARCH_EXTRA_PACKAGES     Extra pacman packages to install before running.
  CI_ARCH_IWYU_SHA           Expected include-what-you-use commit SHA.
  CI_ARCH_TOOLCHAIN_PREFIX   Container path for cached Arch-only tools.
USAGE
}

if [[ $# -eq 0 ]]; then
  usage >&2
  exit 2
fi

if ! command -v docker > /dev/null 2>&1; then
  echo "docker not found; required for Arch toolchain CI wrapper." >&2
  exit 1
fi

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
# shellcheck source=tools/ci-dependency-pins.env
# shellcheck disable=SC1091
source "$ROOT_DIR/tools/ci-dependency-pins.env"
DEFAULT_ARCH_IMAGE="${ARCHLINUX_BASE_DEVEL_IMAGE:?ARCHLINUX_BASE_DEVEL_IMAGE is required}"
IMAGE="${CI_ARCH_IMAGE:-$DEFAULT_ARCH_IMAGE}"
ARCH_TOOLCHAIN_PREFIX="${CI_ARCH_TOOLCHAIN_PREFIX:-/workspace/.deps-arch-toolchain}"

ARCH_PACKAGES=(
  git
  base-devel
  clang
  cppcheck
  cmake
  ninja
  pkgconf
  ccache
  llvm
  python
  ripgrep
  libsndfile
  libpulse
  ncurses
  libusb
  fftw
  blas-openblas
  gcc-fortran
  curl
  codec2
  rtl-sdr
  soapysdr
)

if [[ -n "${CI_ARCH_EXTRA_PACKAGES:-}" ]]; then
  # shellcheck disable=SC2206
  ARCH_PACKAGES+=(${CI_ARCH_EXTRA_PACKAGES})
fi

NEED_IWYU="${CI_ARCH_ENABLE_IWYU:-0}"
case " $* " in
  *"tools/iwyu.sh"* | *"include-what-you-use"*) NEED_IWYU=1 ;;
esac

ENV_ARGS=(
  --env "CI_HOST_UID=$(id -u)"
  --env "CI_HOST_GID=$(id -g)"
  --env "CI_ARCH_ENABLE_IWYU=$NEED_IWYU"
  --env "CI_ARCH_IWYU_SHA=${CI_ARCH_IWYU_SHA:-}"
  --env "CI_ARCH_TOOLCHAIN_PREFIX=$ARCH_TOOLCHAIN_PREFIX"
  --env "HOME=/home/ci"
  --env "GITHUB_WORKSPACE=/workspace"
  --env "DEPS_PREFIX=/workspace/.deps"
  --env "PKG_CONFIG_PATH=/workspace/.deps/lib/pkgconfig:/workspace/.deps/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
  --env "CMAKE_PREFIX_PATH=/workspace/.deps:${CMAKE_PREFIX_PATH:-}"
  --env "LD_LIBRARY_PATH=/workspace/.deps/lib:/workspace/.deps/lib64:${LD_LIBRARY_PATH:-}"
  --env "CCACHE_DIR=/workspace/.ccache"
  --env "CCACHE_BASEDIR=/workspace"
  --env "CCACHE_NOHASHDIR=true"
  --env "CCACHE_SLOPPINESS=time_macros"
)

for name in GITHUB_EVENT_NAME CPPCHECK_BUILD_DIR; do
  if [[ -n "${!name:-}" ]]; then
    ENV_ARGS+=(--env "$name=${!name}")
  fi
done

docker run --rm \
  --volume "$ROOT_DIR:/workspace" \
  --workdir /workspace \
  "${ENV_ARGS[@]}" \
  --env "CI_ARCH_PACKAGES=${ARCH_PACKAGES[*]}" \
  "$IMAGE" \
  bash -lc '
    set -euo pipefail

    pacman -Syu --noconfirm --needed ${CI_ARCH_PACKAGES}

    group_name=ci
    if getent group "$CI_HOST_GID" >/dev/null 2>&1; then
      group_name=$(getent group "$CI_HOST_GID" | cut -d: -f1)
    else
      groupadd --gid "$CI_HOST_GID" "$group_name"
    fi

    if ! id -u ci >/dev/null 2>&1; then
      useradd --uid "$CI_HOST_UID" --gid "$CI_HOST_GID" --create-home --shell /bin/bash ci
    fi

    mkdir -p /workspace/.deps /workspace/.ccache "$CI_ARCH_TOOLCHAIN_PREFIX"
    chown -R "$CI_HOST_UID:$CI_HOST_GID" /workspace/.deps /workspace/.ccache "$CI_ARCH_TOOLCHAIN_PREFIX"

    runuser --user ci --preserve-environment -- git config --global --add safe.directory /workspace

    export PATH="$CI_ARCH_TOOLCHAIN_PREFIX/bin:$PATH"
    if [[ -z "${CI_ARCH_IWYU_SHA:-}" && -f /workspace/tools/ci-dependency-pins.env ]]; then
      source /workspace/tools/ci-dependency-pins.env
      CI_ARCH_IWYU_SHA="${IWYU_SHA:-}"
      export CI_ARCH_IWYU_SHA
    fi
    if [[ "${CI_ARCH_ENABLE_IWYU:-0}" == "1" ]]; then
      : "${CI_ARCH_IWYU_SHA:?CI_ARCH_IWYU_SHA is required when IWYU is enabled}"
      iwyu_manifest="$CI_ARCH_TOOLCHAIN_PREFIX/.iwyu-manifest"
      rebuild_iwyu=0
      if [[ ! -x "$CI_ARCH_TOOLCHAIN_PREFIX/bin/include-what-you-use" ]]; then
        rebuild_iwyu=1
      elif [[ -n "${CI_ARCH_IWYU_SHA:-}" ]]; then
        if [[ ! -f "$iwyu_manifest" ]] || ! grep -q "^iwyu_sha=${CI_ARCH_IWYU_SHA}$" "$iwyu_manifest"; then
          echo "Cached include-what-you-use does not match desired SHA; rebuilding"
          rebuild_iwyu=1
        fi
      fi

      if [[ "$rebuild_iwyu" == "1" ]]; then
        runuser --user ci --preserve-environment -- bash -lc "
        set -euxo pipefail
        rm -rf \"\$CI_ARCH_TOOLCHAIN_PREFIX\" /tmp/include-what-you-use
        mkdir -p \"\$CI_ARCH_TOOLCHAIN_PREFIX\"
        export PATH=\"\$CI_ARCH_TOOLCHAIN_PREFIX/bin:\$PATH\"

        /workspace/tools/fetch-pinned-git.sh https://github.com/include-what-you-use/include-what-you-use \"\$CI_ARCH_IWYU_SHA\" /tmp/include-what-you-use
        cmake -S /tmp/include-what-you-use -B /tmp/include-what-you-use/build -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH=/usr/lib/cmake/llvm \
          -DCMAKE_INSTALL_PREFIX=\"\$CI_ARCH_TOOLCHAIN_PREFIX\"
        cmake --build /tmp/include-what-you-use/build -j \"\$(nproc)\"
        cmake --install /tmp/include-what-you-use/build

        installed_sha=\$(git -C /tmp/include-what-you-use rev-parse HEAD)
        {
          echo \"iwyu_sha=\$installed_sha\"
          echo \"updated=\$(date -u +%FT%TZ)\"
        } > \"\$CI_ARCH_TOOLCHAIN_PREFIX/.iwyu-manifest\"
      "
      fi
    fi

    echo "Arch toolchain versions:"
    runuser --user ci --preserve-environment -- clang-format --version
    runuser --user ci --preserve-environment -- bash -lc "clang-tidy --version | sed -n '\''1,2p'\''"
    runuser --user ci --preserve-environment -- bash -lc "command -v include-what-you-use >/dev/null 2>&1 && include-what-you-use --version || echo '\''include-what-you-use: not installed for this job'\''"
    runuser --user ci --preserve-environment -- cppcheck --version
    runuser --user ci --preserve-environment -- bash -lc "gcc --version | sed -n '\''1p'\''"
    runuser --user ci --preserve-environment -- bash -lc "cmake --version | sed -n '\''1p'\''"
    runuser --user ci --preserve-environment -- ninja --version

    exec runuser --user ci --preserve-environment -- "$@"
  ' bash "$@"
