# Build and Installation Policy

DSD-neo uses CMake as its primary build and installation system.

## Standard Build Variables

CMake honors standard compiler and linker selection mechanisms. Users may pass
toolchain choices through environment variables or CMake cache variables,
including:

- `CC`
- `CXX`
- `CFLAGS`
- `CXXFLAGS`
- `LDFLAGS`
- `CMAKE_C_COMPILER`
- `CMAKE_CXX_COMPILER`
- `CMAKE_C_FLAGS`
- `CMAKE_CXX_FLAGS`
- `CMAKE_EXE_LINKER_FLAGS`
- `CMAKE_SHARED_LINKER_FLAGS`

The project may add warning, sanitizer, SIMD, LTO, or platform flags based on
explicit CMake options, but should not discard user-supplied compiler or linker
settings.

## Debug Information

The install rules do not strip binaries. If users request debug information
through their compiler flags or build type, the build and installation process
should preserve it.

## Non-Recursive Build Structure

The project uses one top-level CMake build graph with explicit source lists. It
does not rely on recursive make invocations to build cross-dependent
subdirectories.

## Repeatable Builds

To repeat a build from the same source tree, toolchain, options, and
environment:

```sh
rm -rf build/repeatable
cmake -S . -B build/repeatable -DCMAKE_BUILD_TYPE=Release
cmake --build build/repeatable -j
ctest --test-dir build/repeatable --output-on-failure
```

For release packages, use the tagged release workflows or reproduce their
documented steps from `.github/workflows/`.

Known limits:

- Floating-point optimization, SIMD choices, SDR driver versions, and system
  audio stacks can affect generated code and output across architectures.
- Binary identity across different compilers, operating systems, CMake
  generators, or vcpkg baselines is not guaranteed.
- Release artifacts should be compared within the same source, toolchain,
  platform, build options, and packaging environment.

## Installation Conventions

Install with CMake:

```sh
cmake --install build/dev-release --prefix /usr/local
```

On POSIX systems, staged packaging can use `DESTDIR`:

```sh
DESTDIR="$PWD/pkgroot" cmake --install build/dev-release --prefix /usr
```

Uninstall from the same build directory:

```sh
cmake --build build/dev-release --target uninstall
```

## Developer Setup

For local development:

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug -j
ctest --preset dev-debug --output-on-failure
```

Optional local hooks:

```sh
tools/install-git-hooks.sh
```
