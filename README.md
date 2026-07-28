# gisland

A C++23 raylib application for Linux/X11.

## Requirements

- CMake 3.28 or newer
- Ninja
- GCC or Clang with C++23 support
- Git
- clang-format and clang-tidy for optional quality checks
- X11, OpenGL, and ALSA development libraries required by raylib

### Debian / Ubuntu

```bash
sudo apt install build-essential cmake ninja-build git clang-format clang-tidy \
  libasound2-dev libx11-dev libxrandr-dev libxi-dev libgl1-mesa-dev \
  libglu1-mesa-dev libxcursor-dev libxinerama-dev
```

### Fedora

```bash
sudo dnf install gcc-c++ clang cmake ninja-build git clang-tools-extra \
  alsa-lib-devel mesa-libGL-devel libX11-devel libXrandr-devel libXi-devel \
  libXcursor-devel libXinerama-devel libatomic
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel clang cmake ninja git alsa-lib mesa libx11 \
  libxrandr libxi libxcursor libxinerama
```

These commands are documentation only. Review packages before running privileged commands.

## Build

Configure and build the development preset:

```bash
cmake --preset dev
cmake --build --preset dev
```

raylib 6.0 is fetched automatically during the first configure.

For an optimized build:

```bash
cmake --preset release
cmake --build --preset release
```

## Run

Run from an active X11 session:

```bash
./build/dev/gisland
```

The manual smoke test passes when the window opens, remains responsive, and exits cleanly
through the window manager close control.

## Test

```bash
ctest --preset dev
```

Tests do not require an active graphical display.

## Sanitizers

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

The sanitizer preset enables AddressSanitizer and UndefinedBehaviorSanitizer for project-owned
targets.

## Formatting

Check formatting:

```bash
cmake --build --preset dev --target format-check
```

Apply formatting:

```bash
cmake --build --preset dev --target format
```

## Static Analysis

```bash
cmake --preset tidy
cmake --build --preset tidy
ctest --preset tidy
```

clang-tidy is opt-in and is not required for normal builds.

## Foundation Architecture

External TOML configuration and JSONL module messages stop at explicit parser boundaries. Valid
input becomes typed configuration, protocol, and scene values before later process, layout, or
rendering layers consume it. Scene validation enforces bounded depth, node count, text size,
progress values, and action IDs. Context selection is independent of raylib and uses an injected
monotonic time point for deterministic priority, recency, expiration, dismissal, and default
fallback behavior.

## Repository Layout

```text
cmake/        Project-specific CMake modules
include/      Public project headers
src/          Application implementation and process entry point
tests/        Display-independent tests
```

## License

Licensed under the GNU General Public License v3.0. See `LICENSE`.
