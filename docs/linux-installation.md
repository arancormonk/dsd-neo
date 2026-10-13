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

The bootstrap installer embeds source-built dependency directories in the
installed binary's runtime search path, including for `$HOME/.local`. Its
installed-binary smoke test removes the installer's temporary library-path
environment so a missing runtime dependency fails the installation instead of
appearing only in a new shell. No `LD_LIBRARY_PATH` setup is needed.

For `/usr` or `/usr/local` dependency installs, the script also refreshes the Linux
dynamic linker cache with `ldconfig`. If you install `mbelib-neo` manually into
one of those prefixes and `dsd-neo` reports that `libmbe-neo.so.2` cannot be
opened, run:

```sh
sudo ldconfig
```

For a user-prefix install, add the executable directory to `PATH` if your
distribution does not already include it, or run `$HOME/.local/bin/dsd-neo`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Useful options:

- `--radio auto|required|off` controls RTL-SDR and SoapySDR package setup.
  `required` fails configure if either backend is unavailable.
- `--codec2 auto|required|off` controls Codec2 setup (default: `required`).
  Missing development packages trigger a pinned source build, including on
  Alpine, so the default installation retains M17 voice support. Explicit
  `auto` permits a build without Codec2; `off` disables it.
- libcurl and expat development packages are installed and required at
  configure time, preserving rdio uploads and RadioReference import.
- A package installation failure is fatal, including for optional radio or
  Codec2 packages. `auto` tolerates unavailable packages, not failed transactions.
- `--build-dir DIR` chooses the CMake build directory.
- `--dry-run` prints package, dependency, build, and install commands.

## Distro Coverage

The installer has package-manager backends for apt, dnf, zypper, apk, and
pacman. The Docker matrix validates the bootstrap path on pinned images for:

- Ubuntu 26.04 and 24.04; Linux Mint and Pop!_OS follow this apt path.
- Debian 13 and 12.
- Fedora 44.
- Rocky Linux 9, AlmaLinux 9, and CentOS Stream 9 with EPEL/CRB enabled.
- openSUSE Leap 16.0 and Tumbleweed.
- Alpine 3.24.
- Arch Linux base-devel for source-build validation; use AUR for normal Arch
  installs.

openSUSE Leap 16.0 provides RTL-SDR but no SoapySDR development package in its
standard repositories. Its default `--radio auto` installation includes RTL-SDR
and omits SoapySDR; `--radio required` requires both and fails there.

Run one Docker validation target:

```sh
tools/docker_linux_install_matrix.sh --distro ubuntu-26.04
```

Run the full local matrix:

```sh
tools/docker_linux_install_matrix.sh --all
```

The Docker wrapper pulls each pinned image and starts a fresh, automatically
removed container. It copies the current checkout, including uncommitted source
changes, instead of mounting the repo writable, so validation does not leave
root-owned build outputs in the working tree. Containers carry the
`org.dsd-neo.install-matrix` label to distinguish them from unrelated workloads.

Each image validates the default user-prefix installation first, then an
independent `/usr/local` build staged with `DESTDIR`. Both installed binaries
must start outside the installer environment. Codec2 is required on every image;
both radio backends are required except for Leap's documented SoapySDR gap.
A separate Debug build runs the CTest suite; use `--no-tests` to run only the two
installation checks. `--jobs N` limits build/test parallelism inside each container.

Container image pins live in `tools/ci-dependency-pins.env` and are checked by
`tools/check_workflow_download_pins.sh`. Pulling a pinned image does not advance
its version: refreshing a supported tag requires updating its recorded digest.
