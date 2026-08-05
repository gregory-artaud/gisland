# gisland

A C++23 raylib application for Linux/X11.

## Requirements

- CMake 3.28 or newer
- Ninja
- GCC or Clang with C++23 support
- Git
- clang-format and clang-tidy for optional quality checks
- tzdata and the system locales selected for clock-calendar formatting
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
cmake --preset release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build --preset release
```

## Install

For a normal user-local installation or update, run:

```bash
./scripts/install-local.sh
```

The script builds the release before interrupting a running instance, installs under `$HOME/.local`,
reloads the systemd user manager, and enables and starts `gisland.service`. It does not use `sudo`,
update the source checkout, install system packages, or modify user configuration and modules.

For troubleshooting, the equivalent build and installation commands are:

```bash
cmake --preset release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build --preset release
systemctl --user stop gisland.service
cmake --install build/release
systemctl --user daemon-reload
systemctl --user import-environment DISPLAY XAUTHORITY
systemctl --user enable --now gisland.service
```

The installation owns binaries, the user service, and private distributed resources under
`$HOME/.local/share/gisland/distributed`. It never writes user configuration or custom modules under
`$XDG_CONFIG_HOME/gisland` or `$XDG_DATA_HOME/gisland`.

For direct startup, ensure the selected prefix is on `PATH`, then run `gisland`. For systemd startup:

```bash
systemctl --user import-environment DISPLAY XAUTHORITY
systemctl --user enable --now gisland.service
journalctl --user -u gisland.service -f
```

An i3 configuration can import its X11 environment, start the service, and bind controls without
global grabs inside gisland:

```i3
exec --no-startup-id systemctl --user import-environment DISPLAY XAUTHORITY
exec --no-startup-id systemctl --user start gisland.service
bindsym $mod+grave exec --no-startup-id gislandctl toggle
bindsym $mod+Shift+grave exec --no-startup-id gislandctl close
```

Equivalent sxhkd bindings are ordinary commands:

```text
super + grave
    gislandctl toggle

super + shift + grave
    gislandctl close
```

Update an existing installation with the same command after updating the checkout:

```bash
./scripts/install-local.sh
```

Rollback uses the same sequence after checking out the previous release. Before uninstalling,
review `build/release/install_manifest.txt`; it lists exactly the core-owned files to remove. Then
disable the service with `systemctl --user disable --now gisland.service` and reload systemd.

## Run

Run from an active X11 session:

```bash
./build/dev/gisland
```

`XDG_RUNTIME_DIR` must name an existing writable directory owned by the current user. gisland holds
`$XDG_RUNTIME_DIR/gisland.lock` for its lifetime and listens on the private
`$XDG_RUNTIME_DIR/gisland.sock` socket. A second gisland instance using the same runtime directory
is rejected.

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

## External Modules

gisland can supervise up to 32 trusted external module processes. Commands are launched as explicit
argument vectors without an implicit shell. Each module communicates through versioned JSONL on
stdin and stdout; stderr is captured as tagged, bounded log events. Module instances support
`always`, `on-failure`, and `never` restart policies with bounded exponential backoff, failure
lockout, and graceful shutdown escalation to process-group signals.

Installed modules are discovered from `$XDG_DATA_HOME/gisland/modules/` (defaulting to
`~/.local/share/gisland/modules/`) and the distributed `modules/` directory. Each module occupies
`<module-id>/module.toml`; a user directory overrides a distributed directory with the same ID.
Installing a manifest never enables it. Instances opt in by stable module ID:

```toml
[[modules]]
id = "clock"
module = "clock-calendar"
arguments = []
enabled = true
```

The manifest declares human-readable metadata, a command vector, its supported protocol range,
default options, and an option schema. Configured values are merged over defaults and validated
before any process starts. A missing, malformed, or protocol-incompatible referenced manifest
rejects startup or reload; malformed unreferenced manifests do not terminate gisland. Existing
instances with an explicit `command` remain supported and bypass discovery.

The shipped `gisland-clock-calendar` executable uses the same public protocol as third-party
modules. It publishes localized `HH:MM` time and a six-week monthly calendar, updates at minute
boundaries, and handles previous-month, next-month, and today actions. Locale and timezone come
from the process environment by default. Module options can override `locale`, `timezone`, and
`week_start` (`monday` or `sunday`).

When user configuration is absent, gisland loads the distributed `assets/config.toml`, which
selects this module and its declarative compact and expanded templates. A user
`$XDG_CONFIG_HOME/gisland/config.toml` continues to override the distributed default completely.

## Control

`gislandctl` controls the running process through the versioned local JSONL protocol:

```bash
./build/dev/gislandctl open
./build/dev/gislandctl close
./build/dev/gislandctl toggle
./build/dev/gislandctl status
./build/dev/gislandctl status --json
./build/dev/gislandctl modules
./build/dev/gislandctl reload
./build/dev/gislandctl module restart clock
./build/dev/gislandctl activate clock
./build/dev/gislandctl activate clock --duration 5s
./build/dev/gislandctl dismiss configured
```

Durations accept positive integer `ms`, `s`, `m`, or `h` units up to 24 hours. Scripts should use
`status --json`; its result has `format_version: 1`.

`reload` explicitly rereads the configuration path selected at startup and resolves the selected
theme from the same user and distributed roots. It is transactional through configuration, theme,
font, active layout, render-resource, monitor-placement, and supervisor-queue preflight. A rejection
returns `reload_rejected` and leaves the running configuration, visible frame, and module processes
unchanged.

gisland also watches the active configuration, selected theme, and referenced module manifests.
Exact-file events are debounced until a 100 ms quiet period has elapsed, then use the same
transactional reload path. Parent directories are watched so atomic editor replacements are
detected while unrelated temporary files are ignored. Invalid candidates are logged and retained
files remain watched for a later correction. If filesystem watching becomes unavailable, automatic
reload is disabled without terminating gisland; `gislandctl reload` remains available.

Unchanged module processes retain their PID and runtime state. Compact or expanded template-only
changes reuse the latest successful data snapshot without restarting the module. Changes to command,
options, restart policy, lifecycle timings, environment, or working directory gracefully replace the
affected process. Added and enabled modules start; removed and disabled modules stop.

## Theme Geometry

User themes live under `$XDG_CONFIG_HOME/gisland/themes/`, defaulting to
`~/.config/gisland/themes/`. View padding can be uniform or configured independently by axis:

```toml
[view.compact]
padding_horizontal = 14
padding_vertical = 4
radius = 16
border = 0
min_width = 230
max_width = 340
min_height = 32
max_height = 32
```

Existing themes may continue to use `padding = 14` as a uniform shorthand. Do not combine the
shorthand with axis-specific fields. Theme changes are validated and applied transactionally by the
existing hot-reload path.

## Dynamic Images

Protocol 1.2 adds the optional `context-images` capability. A module that negotiates it can attach
bounded RGBA8 resources directly to one `publish` and reference them from semantic image nodes:

```json
{
  "type": "publish",
  "context_id": "notification-42",
  "priority": 20,
  "resources": [
    {"id":"icon","format":"rgba8","width":1,"height":1,"data":"/wAA/w=="}
  ],
  "compact": {
    "type":"image",
    "resource_id":"icon",
    "role":"notification-icon",
    "accessible_label":"Application"
  }
}
```

RGBA8 data is top-to-bottom, tightly packed, sRGB, and uses straight alpha. The base64 payload must
decode to exactly `width * height * 4` bytes. Dimensions are limited to 512 pixels per axis, one
context may contain at most 16 resources and 4 MiB of decoded image data, and a complete JSONL
record is limited to 8 MiB. Resources are replaced and released with their context; they cannot be
referenced across contexts or modules. Modules must normalize files, icon-theme names, URIs, and
encoded formats before publishing them.

The scene does not control image geometry. Themes provide semantic roles instead:

```toml
[images.notification-icon]
width = 24
height = 24
fit = "cover"
shape = "circle"
```

`fit` accepts `contain` or `cover`. `shape` accepts `rectangle`, `circle`, or `rounded`; rounded
roles also require `radius`. Circular roles must be square. Changing these values restyles retained
context pixels through the normal transactional theme reload without restarting modules.

## Repository Layout

```text
cmake/        Project-specific CMake modules
include/      Public project headers
src/          Application implementation and process entry point
tests/        Display-independent tests
```

## License

Licensed under the GNU General Public License v3.0. See `LICENSE`.
