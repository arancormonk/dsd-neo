#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/install_linux.sh [options]

Install Linux build dependencies, build pinned mbelib-neo, build DSD-neo, and
install through CMake.

Options:
  --prefix DIR       DSD-neo install prefix (default: $HOME/.local)
  --deps-prefix DIR  Prefix for source-built dependencies
                     (default: --prefix, or build-local with --destdir)
  --build-dir DIR    DSD-neo build directory (default: build/install-linux)
  --destdir DIR      Stage DSD-neo install under DESTDIR instead of installing live
  --radio MODE       Radio backend setup: auto, required, off (default: auto)
  --codec2 MODE      Codec2 setup: auto, required, off (default: auto)
  --yes              Do not prompt before installing distro packages
  --dry-run          Print commands without executing them
  -h, --help         Show this help
USAGE
}

USER_HOME=${HOME:-}
if [ -n "$USER_HOME" ]; then
  PREFIX=$USER_HOME/.local
else
  PREFIX=/usr/local
fi
DEPS_PREFIX=
BUILD_DIR=build/install-linux
DESTDIR_VALUE=
RADIO_MODE=auto
CODEC2_MODE=auto
ASSUME_YES=0
DRY_RUN=0
ORIGINAL_LD_LIBRARY_PATH=
ORIGINAL_LD_LIBRARY_PATH_SET=0
if [ "${LD_LIBRARY_PATH+x}" = x ]; then
  ORIGINAL_LD_LIBRARY_PATH=$LD_LIBRARY_PATH
  ORIGINAL_LD_LIBRARY_PATH_SET=1
fi

while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix)
      [ "$#" -ge 2 ] || {
        echo "Missing value for --prefix" >&2
        exit 2
      }
      PREFIX=$2
      shift 2
      ;;
    --deps-prefix)
      [ "$#" -ge 2 ] || {
        echo "Missing value for --deps-prefix" >&2
        exit 2
      }
      DEPS_PREFIX=$2
      shift 2
      ;;
    --build-dir)
      [ "$#" -ge 2 ] || {
        echo "Missing value for --build-dir" >&2
        exit 2
      }
      BUILD_DIR=$2
      shift 2
      ;;
    --destdir)
      [ "$#" -ge 2 ] || {
        echo "Missing value for --destdir" >&2
        exit 2
      }
      DESTDIR_VALUE=$2
      shift 2
      ;;
    --radio)
      [ "$#" -ge 2 ] || {
        echo "Missing value for --radio" >&2
        exit 2
      }
      RADIO_MODE=$2
      shift 2
      ;;
    --codec2)
      [ "$#" -ge 2 ] || {
        echo "Missing value for --codec2" >&2
        exit 2
      }
      CODEC2_MODE=$2
      shift 2
      ;;
    --yes)
      ASSUME_YES=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
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

absolute_path() {
  case "$1" in
    /*) printf '%s\n' "$1" ;;
    *) printf '%s/%s\n' "$ROOT_DIR" "$1" ;;
  esac
}

if [ -z "$DEPS_PREFIX" ]; then
  if [ -n "$DESTDIR_VALUE" ]; then
    DEPS_PREFIX=$(absolute_path "$BUILD_DIR/deps-prefix")
  else
    DEPS_PREFIX=$PREFIX
  fi
fi

case "$RADIO_MODE" in
  auto | required | off) ;;
  *)
    echo "--radio must be auto, required, or off" >&2
    exit 2
    ;;
esac

case "$CODEC2_MODE" in
  auto | required | off) ;;
  *)
    echo "--codec2 must be auto, required, or off" >&2
    exit 2
    ;;
esac

if [ -r /etc/os-release ]; then
  # shellcheck source=/dev/null
  . /etc/os-release
else
  ID=unknown
  ID_LIKE=
fi

OS_ID=${ID:-unknown}
OS_LIKE=${ID_LIKE:-}
OS_NAME=${NAME:-}

is_centos_stream() {
  [ "$OS_ID" = centos ] || return 1
  case "$OS_NAME" in
    *"CentOS Stream"*) return 0 ;;
    *) return 1 ;;
  esac
}

PM=
if command -v apt-get > /dev/null 2>&1; then
  PM=apt
elif command -v dnf > /dev/null 2>&1; then
  PM=dnf
elif command -v zypper > /dev/null 2>&1; then
  PM=zypper
elif command -v apk > /dev/null 2>&1; then
  PM=apk
elif command -v pacman > /dev/null 2>&1; then
  PM=pacman
else
  echo "No supported package manager found (apt, dnf, zypper, apk, pacman)." >&2
  exit 1
fi

if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ] && ! command -v sudo > /dev/null 2>&1; then
  echo "sudo is required to install distro packages as a non-root user." >&2
  exit 1
fi

run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '+'
    for arg; do
      printf ' %s' "$arg"
    done
    printf '\n'
    return 0
  fi
  "$@"
}

run_root() {
  if [ "$(id -u)" -eq 0 ]; then
    run "$@"
  else
    run sudo "$@"
  fi
}

path_needs_root() {
  case "$1" in
    /tmp | /tmp/* | "$ROOT_DIR" | "$ROOT_DIR"/*) return 1 ;;
  esac
  if [ -n "$USER_HOME" ]; then
    case "$1" in
      "$USER_HOME" | "$USER_HOME"/*) return 1 ;;
    esac
  fi
  return 0
}

run_for_prefix() {
  prefix_path=$1
  shift
  if [ "$(id -u)" -ne 0 ] && path_needs_root "$prefix_path"; then
    run sudo "$@"
  else
    run "$@"
  fi
}

prompt_for_packages() {
  [ "$ASSUME_YES" -eq 0 ] || return 0
  [ "$DRY_RUN" -eq 0 ] || return 0
  if [ "$PM" = pacman ]; then
    printf 'Install build dependencies and run a full pacman system upgrade for %s? [y/N] ' "$OS_ID"
  else
    printf 'Install build dependencies with %s for %s? [y/N] ' "$PM" "$OS_ID"
  fi
  IFS= read -r answer
  case "$answer" in
    y | Y | yes | YES) ;;
    *)
      echo "Aborted."
      exit 1
      ;;
  esac
}

case "$PM" in
  apt)
    BASE_PACKAGES="bash build-essential cmake ninja-build pkg-config git ca-certificates libssl-dev libsndfile1-dev libpulse-dev libncurses-dev libusb-1.0-0-dev libfftw3-dev libblas-dev liblapack-dev gfortran libcurl4-openssl-dev"
    CODEC2_PACKAGES="libcodec2-dev"
    RADIO_PACKAGES="librtlsdr-dev libsoapysdr-dev"
    ;;
  dnf)
    BASE_PACKAGES="bash gcc gcc-c++ make cmake ninja-build pkgconf-pkg-config git ca-certificates openssl-devel libsndfile-devel pulseaudio-libs-devel ncurses-devel libusb1-devel fftw-devel blas-devel lapack-devel gcc-gfortran libcurl-devel"
    CODEC2_PACKAGES="codec2-devel"
    RADIO_PACKAGES="rtl-sdr-devel SoapySDR-devel"
    ;;
  zypper)
    BASE_PACKAGES="bash gcc gcc-c++ make cmake ninja pkgconf git ca-certificates libopenssl-devel libsndfile-devel libpulse-devel ncurses-devel libusb-1_0-devel fftw3-devel blas-devel lapack-devel gcc-fortran libcurl-devel"
    CODEC2_PACKAGES="codec2-devel"
    RADIO_PACKAGES="rtl-sdr-devel soapy-sdr-devel"
    ;;
  apk)
    BASE_PACKAGES="bash build-base cmake ninja pkgconf git ca-certificates openssl-dev libsndfile-dev pulseaudio-dev ncurses-dev libusb-dev fftw-dev blas-dev lapack-dev gfortran curl-dev"
    CODEC2_PACKAGES="codec2-dev"
    RADIO_PACKAGES="librtlsdr-dev soapy-sdr-dev"
    ;;
  pacman)
    BASE_PACKAGES="bash base-devel cmake ninja pkgconf git ca-certificates openssl libsndfile libpulse ncurses libusb fftw blas lapack gcc-fortran curl"
    CODEC2_PACKAGES="codec2"
    RADIO_PACKAGES="rtl-sdr soapysdr"
    ;;
esac

prepare_package_manager() {
  case "$PM" in
    apt)
      run_root env DEBIAN_FRONTEND=noninteractive apt-get update
      ;;
    dnf)
      run_root dnf -y makecache
      case " $OS_ID $OS_LIKE " in
        *" rhel "* | *" centos "* | *" rockylinux "* | *" almalinux "*)
          run_root dnf -y install dnf-plugins-core
          run_root dnf config-manager --set-enabled crb ||
            run_root dnf config-manager --set-enabled powertools ||
            true
          if package_available epel-release; then
            run_root dnf -y install epel-release
          fi
          if is_centos_stream && package_available epel-next-release; then
            run_root dnf -y install epel-next-release
          fi
          run_root dnf -y makecache
          ;;
      esac
      ;;
    zypper)
      run_root zypper --non-interactive refresh
      ;;
    apk)
      run_root apk update
      ;;
    pacman)
      run_root pacman -Syu --noconfirm
      ;;
  esac
}

package_available() {
  pkg=$1
  [ "$DRY_RUN" -eq 0 ] || return 0
  case "$PM" in
    apt) apt-cache show "$pkg" > /dev/null 2>&1 ;;
    dnf) dnf -q repoquery --qf '%{name}' "$pkg" 2> /dev/null | grep -Fx "$pkg" > /dev/null 2>&1 ;;
    zypper) zypper -q info -t package "$pkg" > /dev/null 2>&1 ;;
    apk) apk search -q "$pkg" | grep -Fx "$pkg" > /dev/null 2>&1 ;;
    pacman) pacman -Si "$pkg" > /dev/null 2>&1 ;;
  esac
}

install_packages() {
  [ "$#" -gt 0 ] || return 0
  case "$PM" in
    apt)
      run_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "$@"
      ;;
    dnf)
      run_root dnf -y install "$@"
      ;;
    zypper)
      run_root zypper --non-interactive install --no-recommends "$@"
      ;;
    apk)
      run_root apk add --no-cache "$@"
      ;;
    pacman)
      run_root pacman -S --noconfirm --needed "$@"
      ;;
  esac
}

all_packages_available() {
  for pkg; do
    if ! package_available "$pkg"; then
      return 1
    fi
  done
  return 0
}

install_optional_packages() {
  mode=$1
  label=$2
  shift 2

  [ "$mode" != off ] || return 1
  if all_packages_available "$@"; then
    install_packages "$@"
    return 0
  fi

  if [ "$mode" = required ]; then
    return 2
  fi

  echo "Skipping unavailable optional $label distro packages."
  return 1
}

jobs_count() {
  if [ -n "${DSD_NEO_BUILD_JOBS:-}" ]; then
    printf '%s\n' "$DSD_NEO_BUILD_JOBS"
  elif command -v nproc > /dev/null 2>&1; then
    nproc
  else
    getconf _NPROCESSORS_ONLN 2> /dev/null || printf '2\n'
  fi
}

export_dependency_environment() {
  export PKG_CONFIG_PATH="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  export CMAKE_PREFIX_PATH="$DEPS_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
  export LD_LIBRARY_PATH="$DEPS_PREFIX/lib:$DEPS_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
}

system_library_prefix() {
  case "$1" in
    /usr | /usr/ | /usr/local | /usr/local/) return 0 ;;
    *) return 1 ;;
  esac
}

ldconfig_command() {
  if command -v ldconfig > /dev/null 2>&1; then
    command -v ldconfig
  elif [ -x /sbin/ldconfig ]; then
    printf '%s\n' /sbin/ldconfig
  elif [ -x /usr/sbin/ldconfig ]; then
    printf '%s\n' /usr/sbin/ldconfig
  else
    return 1
  fi
}

refresh_system_linker_cache() {
  system_library_prefix "$DEPS_PREFIX" || return 0

  if ldconfig_bin=$(ldconfig_command); then
    if [ "$DRY_RUN" -eq 1 ]; then
      run_root "$ldconfig_bin" "$DEPS_PREFIX/lib" "$DEPS_PREFIX/lib64"
    elif [ -d "$DEPS_PREFIX/lib" ] && [ -d "$DEPS_PREFIX/lib64" ]; then
      run_root "$ldconfig_bin" "$DEPS_PREFIX/lib" "$DEPS_PREFIX/lib64"
    elif [ -d "$DEPS_PREFIX/lib" ]; then
      run_root "$ldconfig_bin" "$DEPS_PREFIX/lib"
    elif [ -d "$DEPS_PREFIX/lib64" ]; then
      run_root "$ldconfig_bin" "$DEPS_PREFIX/lib64"
    else
      run_root "$ldconfig_bin"
    fi
  else
    echo "ldconfig not found; refresh the dynamic linker cache before running installed binaries." >&2
  fi
}

installed_binary_path() {
  if [ -n "$DESTDIR_VALUE" ]; then
    printf '%s%s/bin/dsd-neo\n' "$DESTDIR_VALUE" "$PREFIX"
  else
    printf '%s/bin/dsd-neo\n' "$PREFIX"
  fi
}

run_installed_dsd_neo_help() {
  installed_binary=$1
  if system_library_prefix "$DEPS_PREFIX"; then
    if [ "$ORIGINAL_LD_LIBRARY_PATH_SET" -eq 1 ]; then
      env "LD_LIBRARY_PATH=$ORIGINAL_LD_LIBRARY_PATH" "$installed_binary" -h > /dev/null
    else
      env LD_LIBRARY_PATH= "$installed_binary" -h > /dev/null
    fi
  else
    "$installed_binary" -h > /dev/null
  fi
}

print_loader_fix_hint() {
  smoke_output=$1
  case "$smoke_output" in
    *libmbe-neo.so.2*)
      if system_library_prefix "$DEPS_PREFIX"; then
        echo "Installed dsd-neo could not load libmbe-neo.so.2; run sudo ldconfig and try again." >&2
      else
        cat >&2 << EOF
Installed dsd-neo could not load libmbe-neo.so.2; update this shell before running it:
  export LD_LIBRARY_PATH="$DEPS_PREFIX/lib:$DEPS_PREFIX/lib64\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
EOF
      fi
      ;;
  esac
}

validate_installed_dsd_neo() {
  installed_binary=$(installed_binary_path)
  if [ "$DRY_RUN" -eq 1 ]; then
    if system_library_prefix "$DEPS_PREFIX"; then
      if [ "$ORIGINAL_LD_LIBRARY_PATH_SET" -eq 1 ]; then
        run env "LD_LIBRARY_PATH=$ORIGINAL_LD_LIBRARY_PATH" "$installed_binary" -h
      else
        run env LD_LIBRARY_PATH= "$installed_binary" -h
      fi
    else
      run "$installed_binary" -h
    fi
    return 0
  fi

  if ! smoke_output=$(run_installed_dsd_neo_help "$installed_binary" 2>&1); then
    printf '%s\n' "$smoke_output" >&2
    echo "Installed dsd-neo smoke test failed: $installed_binary -h" >&2
    print_loader_fix_hint "$smoke_output"
    exit 1
  fi
}

build_mbelib_neo() {
  # shellcheck source=tools/ci-dependency-pins.env
  # shellcheck disable=SC1091
  . "$ROOT_DIR/tools/ci-dependency-pins.env"
  : "${MBELIB_NEO_SHA:?MBELIB_NEO_SHA is required}"

  dep_src=$BUILD_DIR/deps-src/mbelib-neo
  dep_build=$BUILD_DIR/deps-build/mbelib-neo
  run mkdir -p "$BUILD_DIR/deps-src" "$BUILD_DIR/deps-build"
  run "$ROOT_DIR/tools/fetch-pinned-git.sh" https://github.com/arancormonk/mbelib-neo "$MBELIB_NEO_SHA" "$dep_src"
  run cmake -S "$dep_src" -B "$dep_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX"
  run cmake --build "$dep_build" -j "$(jobs_count)"
  run_for_prefix "$DEPS_PREFIX" cmake --install "$dep_build"
}

build_codec2() {
  # shellcheck source=tools/ci-dependency-pins.env
  # shellcheck disable=SC1091
  . "$ROOT_DIR/tools/ci-dependency-pins.env"
  : "${CODEC2_SHA:?CODEC2_SHA is required}"

  dep_src=$BUILD_DIR/deps-src/codec2
  dep_build=$BUILD_DIR/deps-build/codec2
  run mkdir -p "$BUILD_DIR/deps-src" "$BUILD_DIR/deps-build"
  run "$ROOT_DIR/tools/fetch-pinned-git.sh" https://github.com/arancormonk/codec2 "$CODEC2_SHA" "$dep_src"
  run cmake -S "$dep_src" -B "$dep_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX"
  run cmake --build "$dep_build" -j "$(jobs_count)"
  run_for_prefix "$DEPS_PREFIX" cmake --install "$dep_build"
}

configure_build_install_dsd_neo() {
  cmake_args="-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DDSD_ENABLE_LTO=ON -DDSD_ENABLE_FAST_MATH=ON -DDSD_WARNINGS_AS_ERRORS=OFF"

  case "$RADIO_MODE" in
    off)
      cmake_args="$cmake_args -DDSD_ENABLE_RTLSDR=OFF -DDSD_ENABLE_SOAPYSDR=OFF"
      ;;
    required)
      cmake_args="$cmake_args -DDSD_ENABLE_RTLSDR=ON -DDSD_REQUIRE_RTLSDR=ON -DDSD_ENABLE_SOAPYSDR=ON -DDSD_REQUIRE_SOAPYSDR=ON"
      ;;
    auto)
      cmake_args="$cmake_args -DDSD_ENABLE_RTLSDR=ON -DDSD_ENABLE_SOAPYSDR=ON"
      ;;
  esac

  case "$CODEC2_MODE" in
    off)
      cmake_args="$cmake_args -DCMAKE_DISABLE_FIND_PACKAGE_CODEC2=ON"
      ;;
    auto | required)
      cmake_args="$cmake_args -DCMAKE_DISABLE_FIND_PACKAGE_CODEC2=OFF"
      ;;
  esac

  # shellcheck disable=SC2086
  run cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja $cmake_args

  if [ "$CODEC2_MODE" = required ] && [ "$DRY_RUN" -eq 0 ]; then
    if ! grep -q '^CODEC2_LIBRARY:FILEPATH=.*[^-]$' "$BUILD_DIR/CMakeCache.txt" ||
      grep -q '^CODEC2_LIBRARY:FILEPATH=.*NOTFOUND' "$BUILD_DIR/CMakeCache.txt"; then
      echo "Codec2 was required but was not found by CMake." >&2
      exit 1
    fi
  fi

  run cmake --build "$BUILD_DIR" -j "$(jobs_count)"

  binary=$BUILD_DIR/apps/dsd-cli/dsd-neo
  if [ "$DRY_RUN" -eq 0 ]; then
    "$binary" -h > /dev/null
  else
    run "$binary" -h
  fi

  if [ -n "$DESTDIR_VALUE" ]; then
    run env DESTDIR="$DESTDIR_VALUE" cmake --install "$BUILD_DIR" --prefix "$PREFIX"
  else
    run_for_prefix "$PREFIX" cmake --install "$BUILD_DIR" --prefix "$PREFIX"
  fi
  refresh_system_linker_cache
  validate_installed_dsd_neo
}

prompt_for_packages
prepare_package_manager

# shellcheck disable=SC2086
install_packages $BASE_PACKAGES

if [ "$RADIO_MODE" != off ]; then
  # shellcheck disable=SC2086
  if ! install_optional_packages "$RADIO_MODE" "radio" $RADIO_PACKAGES; then
    if [ "$RADIO_MODE" = required ]; then
      echo "Radio backends were required, but distro packages were unavailable." >&2
      exit 1
    fi
  fi
fi

build_codec2_from_source=0
if [ "$CODEC2_MODE" != off ]; then
  # shellcheck disable=SC2086
  if ! install_optional_packages "$CODEC2_MODE" "Codec2" $CODEC2_PACKAGES; then
    if [ "$CODEC2_MODE" = required ]; then
      build_codec2_from_source=1
    fi
  fi
fi

run mkdir -p "$BUILD_DIR"
build_mbelib_neo
if [ "$build_codec2_from_source" -eq 1 ]; then
  build_codec2
fi
refresh_system_linker_cache
export_dependency_environment
configure_build_install_dsd_neo

cat << EOF
DSD-neo install complete.

Prefix:      $PREFIX
Deps prefix: $DEPS_PREFIX
Build dir:   $BUILD_DIR
EOF

if [ -n "$DESTDIR_VALUE" ]; then
  echo "Staged under: $DESTDIR_VALUE"
fi

if [ "$PREFIX" != /usr ] && [ "$PREFIX" != /usr/local ]; then
  cat << EOF

For this shell, you may need:
  export PATH="$PREFIX/bin:\$PATH"
  export LD_LIBRARY_PATH="$DEPS_PREFIX/lib:$DEPS_PREFIX/lib64\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
EOF
fi
