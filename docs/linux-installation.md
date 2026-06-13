# Linux Source Installation

DSD-neo's canonical installer is still CMake. For Linux users who want a
copy/paste bootstrap path, `tools/install_linux.sh` installs distro build
dependencies, builds the pinned `mbelib-neo` dependency, builds this checkout,
smoke-tests `dsd-neo -h`, and installs through CMake. The bootstrap build
disables warnings-as-errors so source installs are not broken by warning drift
in newer distro compilers; developer and CI presets still enforce warnings.

Arch Linux users should normally use the AUR packages linked from `README.md`.
The pacman path exists for source-build validation and Arch-family derivatives
such as Manjaro.

## Quick Start

Install to the default user prefix:

```sh
tools/install_linux.sh --yes
```

Install into `/usr/local`:

```sh
tools/install_linux.sh --yes --prefix /usr/local --deps-prefix /usr/local
```

Stage an install tree for packaging checks:

```sh
tools/install_linux.sh --yes --prefix /usr --destdir "$PWD/pkgroot"
```

When staging with `--destdir`, source-built dependencies default to a
build-local prefix under `--build-dir` so packaging checks do not install them
into the live system. Pass `--deps-prefix` explicitly when validating a
specific dependency prefix.

## Runtime Loader Setup

For `/usr` or `/usr/local` dependency installs, the script refreshes the Linux
dynamic linker cache with `ldconfig`. If you install `mbelib-neo` manually into
one of those prefixes and `dsd-neo` reports that `libmbe-neo.so.2` cannot be
opened, run:

```sh
sudo ldconfig
```

For non-system prefixes such as `$HOME/.local`, use environment variables
instead of `ldconfig`:

```sh
export PATH="$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/.local/lib:$HOME/.local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

Useful options:

- `--radio auto|required|off` controls RTL-SDR and SoapySDR package setup.
  `required` fails configure if either backend is unavailable.
- `--codec2 auto|required|off` controls Codec2 setup. Alpine does not ship a
  Codec2 development package in the validated image, so `required` builds the
  pinned source fallback there.
- `--build-dir DIR` chooses the CMake build directory.
- `--dry-run` prints package, dependency, build, and install commands.

## Distro Coverage

The installer has package-manager backends for apt, dnf, zypper, apk, and
pacman. The Docker matrix validates the bootstrap path on pinned images for:

- Ubuntu 26.04 and 24.04; Linux Mint and Pop!_OS follow this apt path.
- Debian 13 and 12.
- Fedora 44.
- Rocky Linux 9, AlmaLinux 9, and CentOS Stream 9 with EPEL/CRB enabled.
- openSUSE Leap 15.6 and Tumbleweed.
- Alpine 3.24.
- Arch Linux base-devel for source-build validation; use AUR for normal Arch
  installs.

Run one Docker validation target:

```sh
tools/docker_linux_install_matrix.sh --distro ubuntu-26.04
```

Run the full local matrix:

```sh
tools/docker_linux_install_matrix.sh --all
```

The Docker wrapper copies the current checkout into each container instead of
mounting the repo writable, so validation does not leave root-owned build
outputs in the working tree. Container image pins live in
`tools/ci-dependency-pins.env` and are checked by
`tools/check_workflow_download_pins.sh`.
