# gisland

A C++23 raylib application for Linux/X11.

## User and extension documentation

- [Write and distribute an external module](docs/modules.md)
- [Create and install a theme](docs/themes.md)
- [Control and script gisland with `gislandctl`](docs/gislandctl.md)

These guides describe the installed extension points. A module or theme can live entirely in the
user's XDG directories and does not need to be added to this repository.

## Requirements

- CMake 3.28 or newer
- Ninja
- GCC or Clang with C++23 support
- Git
- clang-format and clang-tidy for optional quality checks
- Lua 5.4 interpreter and development files for Lua modules and tests
- A Lua 5.4-compatible `lgi`, plus GLib, Gio, Json-GLib 1.0, GdkPixbuf 2.0, and
  librsvg 2.0, and GTK 3.0 typelibs, for Lua modules
- Python 3 and PyGObject for the D-Bus contract tests
- `pactl` for default output mute and volume controls
- `timeout` for bounded audio module commands
- UPower for event-driven battery status and charge alerts
- tzdata and the system locales selected for clock-calendar formatting
- X11, OpenGL, and ALSA development libraries required by raylib

### Debian / Ubuntu

```bash
sudo apt install build-essential cmake ninja-build git clang-format clang-tidy \
  libasound2-dev libx11-dev libxrandr-dev libxi-dev libgl1-mesa-dev \
  libglu1-mesa-dev libxcursor-dev libxinerama-dev libcairo2-dev \
  libpango1.0-dev libfontconfig1-dev lua5.4 liblua5.4-dev python3 python3-gi \
  gir1.2-json-1.0 gir1.2-gdkpixbuf-2.0 gir1.2-rsvg-2.0 gir1.2-gtk-3.0 \
  lua-lgi pulseaudio-utils
```

### Fedora

```bash
sudo dnf install gcc-c++ clang cmake ninja-build git clang-tools-extra \
  alsa-lib-devel mesa-libGL-devel libX11-devel libXrandr-devel libXi-devel \
  libXcursor-devel libXinerama-devel libatomic cairo-devel pango-devel \
  fontconfig-devel lua-devel lua-lgi json-glib gdk-pixbuf2 librsvg2 gtk3 \
  python3 python3-gobject pulseaudio-utils
```

### Arch Linux

```bash
sudo pacman -S --needed base-devel clang cmake ninja git alsa-lib mesa libx11 \
  libxrandr libxi libxcursor libxinerama cairo pango fontconfig python \
  lua54 lua54-lgi glib2 json-glib gdk-pixbuf2 librsvg gtk3 python-gobject libpulse
```

These commands are documentation only. Review packages before running privileged commands.

## Build

Configure and build the development preset:

```bash
cmake --preset dev
cmake --build --preset dev
```

raylib 6.0 is fetched automatically during the first configure.

`lgi` is normally loaded from the Lua 5.4 system search path. For tests against an unpacked or
staged installation, pass `-DGISLAND_LGI_ROOT=/path/to/root`; the root may contain either
`share/lua/5.4` directly or under `usr/`. This test-only override is not compiled into installed
binaries.

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

For troubleshooting or as the basis of a fresh custom-prefix installation, the underlying CMake
commands are:

```bash
cmake --preset release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build --preset release
systemctl --user stop gisland.service
cmake --install build/release
systemctl --user daemon-reload
systemctl --user import-environment DISPLAY XAUTHORITY
systemctl --user enable --now gisland.service
```

This manual sequence is not an equivalent upgrade path: CMake installs current files but cannot
safely identify and remove files owned by older releases. Use `./scripts/install-local.sh` for
user-local updates that require legacy clock, audio, or battery migration and cleanup. For another
prefix, review and remove stale release-owned files explicitly before using the manual sequence.

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
bindsym $mod+m exec --no-startup-id gislandctl action audio toggle-mute
bindsym XF86AudioMute exec --no-startup-id gislandctl action audio toggle-mute
bindsym XF86AudioRaiseVolume exec --no-startup-id gislandctl action audio volume-up
bindsym XF86AudioLowerVolume exec --no-startup-id gislandctl action audio volume-down
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

Editable personal modules are discovered from `$XDG_CONFIG_HOME/gisland/modules/` (defaulting to
`~/.config/gisland/modules/`). Installed user modules are discovered from
`$XDG_DATA_HOME/gisland/modules/` (defaulting to `~/.local/share/gisland/modules/`), followed by the
distributed `modules/` directory. Each module occupies `<module-id>/module.toml`; the first directory
in that precedence order claims an ID, including when its manifest is invalid. Installing a manifest
never enables it. Instances opt in by stable module ID:

```toml
[[modules]]
id = "clock"
module = "clock-calendar"
arguments = []
enabled = true
```

Compact and expanded slots use independent enabled-instance fallbacks:

```toml
[defaults]
compact = "clock"
expanded = "clock"
```

The legacy `default_module = "clock"` form remains supported and sets both fallbacks.

Each package requires `module.toml`. It may also contain package-local default options in
`config.toml`, declarative scene templates in `view.toml`, and implementation files. The manifest
declares human-readable metadata, a command vector, its supported protocol range, an option schema,
and optional package file references:

```toml
id = "example-clock"
name = "Example clock"
description = "Publishes local time as template data"
command = ["gisland-lua-host"]
entry = "example.lua"
config = "config.toml"
view = "view.toml"

[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8

[options_schema.format]
type = "string"
```

`entry`, `config`, and `view` are package-relative regular-file paths. Absolute paths, `..`, missing
files, and symlink escapes outside the package are rejected. `entry` is required when the command's
executable name is `gisland-lua-host`, is invalid for other commands, and is appended once as an
absolute canonical argument by gisland. Other command arguments remain explicit array elements and
are never interpreted by a shell.

`config.toml`, when referenced, has exactly one `[defaults]` table. Every key must exist in
`options_schema` and satisfy its type, allowed values, and optional inclusive numeric `minimum` and
`maximum`. Legacy inline manifest `[defaults]` remains valid only when `config` is absent. Resolution
applies package defaults first and global instance `[modules.options]` overrides second, then validates
the result before sending it as `init.configuration`.

`view.toml` may define `[compact]`, `[expanded]`, or both using the same declarative template grammar
as `[modules.view.compact]` and `[modules.view.expanded]`. A global instance view replaces the complete
matching package slot; trees are not deep-merged, and the other package slot remains unchanged.
Templates bind data with values such as `{ bind = "time" }`. Packages use semantic roles such as
`compact-primary`, `body`, and `warning`; concrete colors, fonts, geometry, spacing, and animation
remain global-theme responsibilities.

Configured values and views are validated before any process starts. A missing, malformed, or
protocol-incompatible referenced manifest rejects startup or reload; malformed unreferenced manifests
do not terminate gisland. Existing instances with an explicit `command` remain supported and bypass
discovery.

## Lua Modules

`gisland-lua-host` runs trusted Lua 5.4 modules as external processes. Lua code and native Lua
libraries have the current user's full permissions; gisland does not sandbox them. Each configured
module instance gets its own supervised host process, Lua state, timers, and failure lifecycle. A
blocking callback delays only that module instance, not rendering or other modules, but modules
should still keep synchronous work bounded.

Lua packages name the host in `command` and the script separately in `entry`:

```toml
id = "example-clock"
name = "Example clock"
description = "Publishes local time as template data"
command = ["gisland-lua-host"]
entry = "example.lua"
config = "config.toml"
view = "view.toml"

[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
```

The host accepts exactly one final entry-script argument after gisland resolves the manifest. It
prepends the entry directory's `?.lua` and `?/init.lua` patterns to inherited Lua 5.4 `package.path`,
so package-local `require` works before standard locations. Native `package.cpath` remains unchanged.
The self-contained distributed example under `assets/modules/lua-example` demonstrates package-local
defaults, bound compact and expanded views, and a one-second `os.date` update. Installation discovers
it but does not enable it. A global configuration enables it without duplicating its options or view:

```toml
[[modules]]
id = "example"
module = "lua-example"
enabled = true
```

A script must return exactly one definition produced by `gisland.module`:

```lua
return gisland.module {
  every = "1s",
  init = function(config, metadata) end,
  update = function() return { value = 42 } end,
  actions = {
    refresh = function(value) return true end,
  },
  fallback_action = function(action_id, value) return false end,
  visibility = function(state) end,
  shutdown = function() end,
}
```

All fields and callbacks are optional. `init(config, metadata)` runs once after protocol
initialization; metadata contains the core-supplied instance ID, locale, and timezone. `ready` is
emitted only if it succeeds. `update()` runs at the bounded `every` interval and emits one
`data` record when it returns a non-nil JSON-compatible value. `visibility(state)` receives the
current visibility string. `shutdown()` runs during graceful shutdown. Callbacks run serially.

The data-oriented API pairs Lua values with declarative views configured by the core. A minimal
module is available at `tests/fixtures/lua/example_data_module.lua`:

```lua
return gisland.module {
  every = "1s",
  update = function()
    return { time = os.date("%H:%M"), date = os.date("%A %d") }
  end,
}
```

`gisland.data(value)` emits data explicitly. Lua tables with contiguous integer keys become arrays;
string-keyed tables become objects. Empty tables are objects unless created with
`gisland.array()`. Values are bounded and must be JSON-compatible. Core-to-Lua values, action and
configuration values, scene values, and `update()` return values allow at most 256 table items.
Explicit `gisland.data` output allows at most 512 items for larger declarative snapshots; the same
depth and serialized-size limits still apply.

Context-oriented modules call `gisland.publish(context)`, `gisland.dismiss(context_id)`, and
`gisland.log(level, message)`. The `gisland.ui` constructors cover `text`, `icon`, `image`,
`rich_text`, `row`, `column`, `spacer`, `progress`, `indicator`, `button`, and `action_region`.
Modules provide semantic roles and action IDs, while the core remains responsible for protocol
validation, styling, layout, capabilities, and rendering. See the executable counter example in
`tests/fixtures/lua/example_action_module.lua`.

Rendered interactions and `gislandctl action` both dispatch the same semantic action callback. The
callback receives the optional JSON-compatible value and returns `true`, `false`, or
`false, "reason"`. The host creates the protocol 1.8 correlated `action_result`; invocation IDs are
never exposed to Lua. A missing handler, invalid return, or thrown action error rejects and logs only
that invocation, leaving the module ready. A module may provide
`fallback_action(action_id, value)` when unknown actions need module-specific rejection behavior.

Timers use the same positive `ms`, `s`, `m`, or `h` duration syntax as `every`, up to 24 hours:

```lua
gisland.defer(function() gisland.data { ready = true } end)
gisland.after("500ms", function() gisland.dismiss("temporary") end)
```

Timer callbacks run serially and are cancelled on shutdown. Script-load, `init`, periodic `update`,
timer, visibility, shutdown, transport, queue, and value-conversion errors terminate only that host
process. gisland removes its contexts and applies the manifest's restart policy and backoff. Scene
records rejected by the core follow the normal last-valid-context behavior.

The shipped clock-calendar is a self-contained protocol-1.8 Lua package hosted by
`gisland-lua-host`. It publishes localized `HH:MM` time and a six-week monthly calendar, updates at
minute boundaries, and handles previous-month, next-month, and today actions. Locale and timezone
come from core initialization by default without changing process `TZ`. Module options can override
`locale`, `timezone`, and `week_start` (`monday` or `sunday`). Its default compact and expanded views
live in the package's `view.toml` rather than the global configuration.

The shipped battery module is also a self-contained protocol-1.8 Lua package. It creates Gio UPower
proxies synchronously during initialization and then reacts only to UPower property-change signals on
the host's shared GLib main context; it does not poll. Its package-local defaults control warning,
persistent, critical, semantic-color thresholds, and temporary preview duration. The package view
shows normalized charge, the active charge estimate, battery health, and power draw. An unavailable
UPower service is logged, while absent or nonfinite readings are ignored; neither terminates the
graphical core.

Battery percentages use strict ordering: `0 < critical_percent < persistent_percent <
warning_percent <= 100` and `0 < red_percent < yellow_percent <= 100`. Manifests validate each
option's integer range but cannot express relations between options, so the Lua module rejects an
invalid combination during initialization.

Low-battery thresholds fire once per discharge cycle. Plugging in replaces an active persistent or
critical alert, and unplugging starts a new cycle. Threshold state is stored under
`$XDG_STATE_HOME/gisland/battery-cycle.json`, falling back to
`$HOME/.local/state/gisland/battery-cycle.json`; a persistent alert can be dismissed without
re-enabling duplicate alerts in the same cycle.

When user configuration is absent, gisland loads the distributed `assets/config.toml`, which
selects this module and enables the desktop
notification module beside it. A user
`$XDG_CONFIG_HOME/gisland/config.toml` continues to override the distributed default completely.

## Desktop Notifications

The shipped single-entry `notifications.lua` module owns `org.freedesktop.Notifications` on the user
session bus and exposes the standard freedesktop notification interface. It runs through
`gisland-lua-host` as an ordinary supervised protocol-1.8 module. A missing lgi or typelib dependency
stops only this module and does not terminate the graphical core. Another notification daemon must
not already own the bus name.

The daemon supports application names and icons, summaries, freedesktop body markup, default and
named actions, resident notifications, urgency, replacement IDs, and local image data. Body markup
is converted to typed rich text rather than passed to the renderer. Images may come from RGB8 or
RGBA8 `image-data`, local paths or `file:` URIs, desktop entries, and local icon names. GdkPixbuf and
librsvg decode raster and SVG files. Images are normalized to
straight-alpha RGBA8 and downscaled to at most 512 pixels per axis. Remote image URLs are rejected.

An explicit positive timeout is honored and zero disables automatic expiration. A negative timeout
uses 5 seconds for low urgency, 8 seconds for normal urgency, and no automatic expiration for
critical urgency. Closing, expiration, and non-resident actions emit the standard D-Bus signals;
resident actions leave their notification visible.

Notifications publish an expanded-only view and request a one-second reveal through the generic
protocol presentation intent. Compact content remains owned by the configured compact fallback.
Reveal expiration returns to compact mode unless the pointer is hovering or the overlay was opened
explicitly.

The reveal duration and bounded external history are module options:

```toml
[[modules]]
id = "notifications"
module = "notifications"
enabled = true
restart = "on-failure"

[modules.options]
reveal_duration_ms = 1000
history_limit = 100
history_visible_limit = 5
```

`reveal_duration_ms` accepts `0..60000`, where zero disables automatic reveal. `history_limit`
accepts `1..1000`; `history_visible_limit` accepts `1..5` and cannot exceed the retained limit.
History stores bounded plain-text notification content under
`$XDG_STATE_HOME/gisland/notifications-history.json`, falling back to
`$HOME/.local/state/gisland/notifications-history.json`. It does not retain actions, links, images,
or arbitrary hints.

The public history action ID is `show-more`. Run `gislandctl action notifications show-more`, then
atomically select and open it with `gislandctl activate-open notifications` after the correlated
action succeeds. Repeating the pair adds one older entry below
it, up to `history_visible_limit`. Rendered entries use
`history:<session>:hide:<sequence>` and the close icon uses
`history:<session>:close-all`; these remain module-owned scene actions. Clicking an entry masks it for the
current opening without deleting persisted history; the close icon masks the complete current stack.
Masking the final visible entry, using the close icon, or waiting eight seconds without interaction
closes the overlay. Every invocation and click resets that inactivity deadline. The next opening
restores all session-masked entries and starts again with one entry. A direct i3 binding is:

```i3
bindsym $mod+n exec --no-startup-id sh -c 'gislandctl action notifications show-more && gislandctl activate-open notifications'
```

Links are opened through Gio only for `http`, `https`, and `mailto` URIs. The daemon never invokes a
shell or executes action strings supplied by applications. Live notification actions remain in
memory only; history is non-interactive and storage failures remain logs-only.

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
./build/dev/gislandctl action audio volume-up
./build/dev/gislandctl action audio set-volume --value 42
```

`gislandctl action <instance> <action> [--value <json>]` routes a semantic action to one running
module instance and waits for its accepted or rejected result. The optional value is parsed as JSON,
so it preserves null, booleans, numbers, strings, arrays, and objects. This uses the same module
`action` message as buttons, links, and other rendered interaction targets; modules do not need a
separate control service. Confirmed external actions require a module negotiating protocol 1.8.

Durations accept positive integer `ms`, `s`, `m`, or `h` units up to 24 hours. Scripts should use
`status --json`; its result has `format_version: 2` with separate `compact` and `expanded` owners.

`reload` explicitly rereads the configuration path selected at startup and resolves the selected
theme from the same user and distributed roots. It is transactional through configuration, theme,
font, active layout, render-resource, monitor-placement, and supervisor-queue preflight. A rejection
returns `reload_rejected` and leaves the running configuration, visible frame, and module processes
unchanged.

gisland also watches the active configuration, selected theme, and each enabled package's resolved
`module.toml`, `config.toml`, `view.toml`, and entry file. Transitively required Lua files are not
watched. Exact-file events are debounced until a 100 ms quiet period has elapsed, then use the same
reload path. Parent directories are watched so atomic editor replacements are detected while
unrelated temporary files are ignored. If filesystem watching becomes unavailable, automatic reload
is disabled without terminating gisland; `gislandctl reload` remains available.

Static TOML parsing, path containment, schema validation, and view instantiation are transactional:
an invalid candidate retains the running process and last valid view, and retained files stay watched
for correction. Manifest, package configuration, and entry changes replace affected processes.
View-only changes reuse the latest valid data snapshot without restart and independently retain a
slot that cannot be instantiated. Lua syntax, package `require`, and `init` execute only in the
replacement process; a runtime script failure therefore occurs after the old process stops and uses
normal supervisor failure and restart backoff rather than transactional retention.

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

Button backgrounds can reference palette roles or use explicit colors independently from the accent:

```toml
[buttons]
background = "surface"
disabled_background = "surface"
hover_overlay = "#FFFFFF14"
```

Themes without this table retain the original `accent` and `muted` button backgrounds. The optional
hover overlay appears immediately over enabled buttons; omit it to use the subtle white default.

## Status Indicator Effects

Protocol 1.9 adds the optional `indicator-effects` capability. After negotiating it, a module may
request any combination of `shadow`, `glow`, and `breathe` on an `indicator` while continuing to
provide only a semantic state:

```json
{"type":"indicator","state":"success","accessible_label":"Running",
 "effects":["glow","breathe"]}
```

Unknown and duplicate effects, effects sent before protocol 1.9, and effects sent without the
negotiated capability are protocol violations. They reject only the publication according to the
normal module-violation policy. An omitted or empty `effects` array preserves the original
indicator geometry and rendering.

Modules cannot provide radii, colors, opacity, timing, or easing. Those values belong to the
selected theme under `indicator.shadow`, `indicator.glow`, `indicator.breathe`, and
`indicator.reduced_motion`. Effect extents participate in layout and clipping. Setting
`interaction.reduced_motion = true` replaces the animated breathe cycle with the theme's static
reduced-motion intensity and opacity.

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
  "compact": {"type":"row","gap":"small","children":[
    {"type":"image","resource_id":"icon","role":"notification-icon",
     "accessible_label":"Application"},
    {"type":"text","value":"Image ready","role":"compact-primary"}
  ]}
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
placement = "leading-cap"
```

`fit` accepts `contain` or `cover`. `shape` accepts `rectangle`, `circle`, or `rounded`; rounded
roles also require `radius`. Circular roles must be square. `placement` defaults to `flow`.
`leading-cap` is restricted to the first child of the root compact row and centers a square circular
image in the capsule's leading cap. Changing these values restyles retained context pixels through
the normal transactional theme reload without restarting modules.

## Rich Content

Protocol 1.3 adds the optional `rich-content` capability. After negotiating it, modules may publish
bounded `rich_text` and invisible `action_region` scene nodes. Rich text is a flat semantic sequence
of text spans, links, and inline images; raw HTML, Pango markup, CSS, URIs, and executable action
strings are not accepted by the core:

```json
{
  "type":"rich_text",
  "role":"notification-body",
  "content":[
    {"type":"text","value":"The file "},
    {"type":"text","value":"archive.tar.gz","emphasis":["bold"]},
    {"type":"text","value":" is ready.\n"},
    {"type":"link","value":"Open folder","action_id":"open-folder",
     "accessible_label":"Open the download folder"},
    {"type":"inline_image","resource_id":"preview",
     "role":"notification-inline-image","accessible_label":"Image preview"}
  ]
}
```

Text and links support combined `bold`, `italic`, and `underline` emphasis. PangoCairo shapes and
wraps the typed content using private theme fonts. Link rectangles and action regions emit only
semantic action IDs back to the owning module, which remains responsible for interpreting actions
or opening URIs. An inline image also requires the protocol-1.2 `context-images` capability and
references a resource from the same publication.

An `action_region` adopts its child's geometry without adding decoration or padding. Nested links,
buttons, and action regions take hit-test priority over their parent, allowing one default action to
cover a notification while preserving close, link, and named-action controls.

## Content Transitions

Protocol 1.9 adds the optional `content-transitions` capability. A `publish` or `data` record may
attach a semantic transition independently to either updated view:

```json
{
  "type": "data",
  "value": {"month_label": "August 2026"},
  "transitions": {"expanded": "slide-left"}
}
```

The closed catalogue is `crossfade`, `slide-left`, and `slide-right`. Omitting `transitions` keeps
the default crossfade. Modules choose only the semantic type; the theme owns duration, horizontal
distance, easing, and reduced-motion duration. The core captures, interpolates, and clips outgoing
and incoming content. Set `GISLAND_REDUCED_MOTION=1` (or `true`) to select the theme's
reduced-motion values.

```toml
[animation.content_transition]
duration_ms = 250
distance = 48
easing = "ease-in-out"

[animation.reduced_motion.content_transition]
duration_ms = 0
```

## Repository Layout

```text
cmake/        Project-specific CMake modules
include/      Public project headers
src/          Application implementation and process entry point
tests/        Display-independent tests
```

## License

Licensed under the GNU General Public License v3.0. See `LICENSE`.
