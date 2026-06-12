#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/docker_linux_install_matrix.sh (--all | --distro ID [--distro ID ...]) [options]

Validate tools/install_linux.sh in pinned Linux container images.

Options:
  --all          Run every distro in the matrix.
  --distro ID   Run one distro. Repeat for multiple distros.
  --list        Print supported distro IDs.
  --jobs N      Build parallelism inside containers.
  -h, --help    Show this help.
USAGE
}

DISTROS=(
  ubuntu-26.04
  ubuntu-24.04
  debian-13
  debian-12
  fedora-44
  rocky-9
  almalinux-9
  centos-stream-9
  opensuse-leap-15.6
  opensuse-tumbleweed
  alpine-3.24
  arch
)

list_distros() {
  printf '%s\n' "${DISTROS[@]}"
}

# shellcheck source=tools/ci-dependency-pins.env
# shellcheck disable=SC1091
source "$ROOT_DIR/tools/ci-dependency-pins.env"

image_for_distro() {
  case "$1" in
    ubuntu-26.04) printf '%s\n' "${INSTALL_UBUNTU_2604_IMAGE:?}" ;;
    ubuntu-24.04) printf '%s\n' "${INSTALL_UBUNTU_2404_IMAGE:?}" ;;
    debian-13) printf '%s\n' "${INSTALL_DEBIAN_13_IMAGE:?}" ;;
    debian-12) printf '%s\n' "${INSTALL_DEBIAN_12_IMAGE:?}" ;;
    fedora-44) printf '%s\n' "${INSTALL_FEDORA_44_IMAGE:?}" ;;
    rocky-9) printf '%s\n' "${INSTALL_ROCKY_9_IMAGE:?}" ;;
    almalinux-9) printf '%s\n' "${INSTALL_ALMALINUX_9_IMAGE:?}" ;;
    centos-stream-9) printf '%s\n' "${INSTALL_CENTOS_STREAM_9_IMAGE:?}" ;;
    opensuse-leap-15.6) printf '%s\n' "${INSTALL_OPENSUSE_LEAP_156_IMAGE:?}" ;;
    opensuse-tumbleweed) printf '%s\n' "${INSTALL_OPENSUSE_TUMBLEWEED_IMAGE:?}" ;;
    alpine-3.24) printf '%s\n' "${INSTALL_ALPINE_324_IMAGE:?}" ;;
    arch) printf '%s\n' "${ARCHLINUX_BASE_DEVEL_IMAGE:?}" ;;
    *) return 1 ;;
  esac
}

validate_image_pin() {
  image=$1
  if [[ ! "$image" =~ @sha256:[0-9a-f]{64}$ ]]; then
    echo "Image must be pinned as image@sha256:<64 hex chars>: $image" >&2
    exit 1
  fi
}

list_archive_paths() {
  local path
  git ls-files -z --cached --others --exclude-standard |
    while IFS= read -r -d '' path; do
      if [[ -e "$path" || -L "$path" ]]; then
        printf '%s\0' "$path"
      fi
    done
}

SELECTED=()
JOBS=${DSD_NEO_BUILD_JOBS:-}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)
      SELECTED=("${DISTROS[@]}")
      shift
      ;;
    --distro)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --distro" >&2
        exit 2
      fi
      SELECTED+=("$2")
      shift 2
      ;;
    --jobs)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --jobs" >&2
        exit 2
      fi
      JOBS=$2
      shift 2
      ;;
    --list)
      list_distros
      exit 0
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ ${#SELECTED[@]} -eq 0 ]]; then
  usage >&2
  exit 2
fi

if ! command -v docker > /dev/null 2>&1; then
  echo "docker not found; required for install matrix validation." >&2
  exit 1
fi

if [[ -z "$JOBS" ]]; then
  if command -v nproc > /dev/null 2>&1; then
    JOBS=$(nproc)
  else
    JOBS=2
  fi
fi

run_one() {
  distro=$1
  image=$(image_for_distro "$distro") || {
    echo "Unknown distro: $distro" >&2
    echo "Known distros:" >&2
    list_distros >&2
    exit 2
  }
  validate_image_pin "$image"

  echo "==> Validating $distro with $image"

  list_archive_paths |
    tar --null -T - -cf - |
    docker run --rm -i \
      --env "DSD_NEO_BUILD_JOBS=$JOBS" \
      "$image" \
      sh -lc '
        set -eu
        if ! command -v tar > /dev/null 2>&1; then
          if command -v apt-get > /dev/null 2>&1; then
            export DEBIAN_FRONTEND=noninteractive
            apt-get update
            apt-get install -y --no-install-recommends tar
          elif command -v dnf > /dev/null 2>&1; then
            dnf install -y tar
          elif command -v zypper > /dev/null 2>&1; then
            zypper --non-interactive --gpg-auto-import-keys refresh
            zypper --non-interactive install --no-recommends tar
          elif command -v apk > /dev/null 2>&1; then
            apk add --no-cache tar
          elif command -v pacman > /dev/null 2>&1; then
            pacman -Syu --noconfirm --needed tar
          else
            echo "tar not found and no supported package manager is available" >&2
            exit 1
          fi
        fi
        mkdir -p /workspace
        tar -xf - -C /workspace
        cd /workspace
        chmod +x tools/install_linux.sh tools/fetch-pinned-git.sh
        tools/install_linux.sh \
          --yes \
          --prefix /usr/local \
          --deps-prefix /usr/local \
          --build-dir /tmp/dsd-neo-build \
          --destdir /tmp/dsd-neo-root \
          --radio required \
          --codec2 auto
        test -x /tmp/dsd-neo-root/usr/local/bin/dsd-neo
        /tmp/dsd-neo-root/usr/local/bin/dsd-neo -h > /dev/null
      '
}

for distro in "${SELECTED[@]}"; do
  run_one "$distro"
done
