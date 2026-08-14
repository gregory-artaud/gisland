# Writing external modules

External modules are ordinary user processes supervised by gisland. They can be installed and
updated independently of gisland: no C++ changes or pull request to this repository are required.
A module communicates only through one JSON object per line on standard input and output. Lua 5.4
modules can use the supplied `gisland-lua-host` instead of implementing that protocol directly.

Module code is trusted and runs with the current user's permissions. It is not sandboxed. gisland
does validate every message and never executes an action string emitted by a module.

## Create a first Lua module

Create this package in the editable module root:

```text
~/.config/gisland/modules/hello/
├── module.toml
├── view.toml
└── hello.lua
```

`module.toml` identifies the package and its protocol:

```toml
id = "hello"
name = "Hello"
description = "A minimal external gisland module"
command = ["gisland-lua-host"]
entry = "hello.lua"
view = "view.toml"

[protocol]
major = 1
minimum_minor = 8
maximum_minor = 8
```

`view.toml` turns module data into compact and expanded scenes:

```toml
[compact]
type = "text"
value = { bind = "message" }
role = "compact-primary"

[expanded]
type = "column"
gap = "small"
children = [
  { type = "text", value = { bind = "message" }, role = "title" },
  { type = "button", action_id = "refresh", accessible_label = "Refresh greeting",
    content = { type = "text", value = "Refresh", role = "button" } }
]
```

`hello.lua` publishes a data snapshot and handles the semantic button action:

```lua
local message = "Hello from an external module"

return gisland.module {
  every = "1m",

  update = function()
    return { message = message }
  end,

  actions = {
    refresh = function(value)
      message = "Refreshed at " .. os.date("%H:%M:%S")
      gisland.data { message = message }
      return true
    end,
  },
}
```

Enable the package in `~/.config/gisland/config.toml`. The instance `id` is local configuration;
`module` is the package ID:

```toml
[[modules]]
id = "hello-main"
module = "hello"
enabled = true
restart = "on-failure"
```

Keep the other required top-level configuration from your existing file. Apply the change and
inspect the instance:

```bash
gislandctl reload
gislandctl modules
gislandctl activate-open hello-main
```

Use `journalctl --user -u gisland.service -f` for manifest, script, protocol, and stderr logs.

## Package discovery and distribution

Every package occupies `<root>/<package-id>/module.toml`. Roots are searched in this order:

1. `$XDG_CONFIG_HOME/gisland/modules` (normally `~/.config/gisland/modules`) for editable modules;
2. `$XDG_DATA_HOME/gisland/modules` (normally `~/.local/share/gisland/modules`) for installed user
   modules;
3. gisland's read-only distributed module directory.

The first root containing an ID wins, even if its manifest is invalid. A third-party project can be
distributed as a directory or archive that users place under either user root. Installing a package
does not enable it; that remains an explicit per-user configuration choice.

A package may contain:

```text
my-module/
├── module.toml       # required
├── config.toml       # optional package defaults
├── view.toml         # optional declarative scenes
├── main.lua          # Lua entry, or any other implementation files
└── lib/...
```

`entry`, `config`, and `view` are package-relative regular files. Absolute paths, `..`, missing
files, and symlinks escaping the package are rejected. A Lua `entry` is required for
`gisland-lua-host` and forbidden for other commands. Lua's package-local `?.lua` and `?/init.lua`
search paths are added automatically, so local `require` calls work.

## Manifest reference

The full manifest shape is:

```toml
id = "weather"
name = "Weather"
description = "Shows the current weather"
command = ["weather-module", "--jsonl"]
config = "config.toml" # optional
view = "view.toml"     # optional

[protocol]
major = 1
minimum_minor = 1
maximum_minor = 8

[options_schema.units]
type = "string"
required = true
allowed = ["metric", "imperial"]

[options_schema.refresh_seconds]
type = "integer"
minimum = 30
maximum = 3600
```

Required manifest fields are `id`, `name`, `command`, and `[protocol]`. `description` is optional.
The `id` must equal the package directory name. `command` is a non-empty argument vector and is
never interpreted by a shell. If its executable contains a relative path with `/`, it is resolved
from the package directory; a bare executable name is resolved through `PATH`.

Option types are `string`, `integer`, `number`, `boolean`, `array`, and `table`. `allowed` applies
only to strings; inclusive `minimum` and `maximum` apply only to numeric types. Put defaults in the
optional referenced `config.toml`:

```toml
[defaults]
units = "metric"
refresh_seconds = 300
```

Every default and instance override must be declared by `options_schema` and have the correct type.
Legacy inline `[defaults]` in `module.toml` is accepted only when `config` is absent. The resolved
values arrive in the JSONL `init.configuration` object, or as the Lua `init(config)` argument.

An instance can also set `arguments`, `restart` (`always`, `on-failure`, or `never`), an absolute
`working_directory`, string values under `[modules.environment]`, and positive millisecond lifecycle
values under `[modules.timings]`. Direct legacy instances with an explicit `command` instead of a
manifest reference remain supported, but packaged modules are easier to distribute safely.

## Declarative views

`view.toml` may define `[compact]`, `[expanded]`, or both. A configured
`[modules.view.compact]` or `[modules.view.expanded]` replaces the corresponding package slot; slots
are not deep-merged. An expanded template requires a compact template.

Literal values can be replaced by `{ bind = "path.to.value" }`. A binding walks an object returned
by `data`; a missing value or wrong type rejects only that snapshot and retains the last valid view.
Arrays can be expanded with a repeat child:

```toml
[expanded]
type = "column"
gap = "small"
children = [
  { repeat = "forecast", as = "day", template = {
    type = "text", value = { bind = "day.label" }, role = "body"
  } }
]
```

Supported template nodes are:

| Type | Important fields |
| --- | --- |
| `text` | `value`, `role`, optional `truncation` |
| `icon` | `name`, optional `accessible_label` |
| `row`, `column` | `children`, optional `alignment` and theme `gap` token |
| `spacer` | optional `flexible` and theme `size_token` |
| `progress` | `value` in `0..1`, optional `label`, `state`, `shape` (`linear` or `ring`) |
| `indicator` | `state`, `accessible_label` |
| `button` | `content`, `action_id`, optional `enabled`, `accessible_label` |

Names such as typography roles, gap tokens, icons, and semantic states are resolved by the active
global theme. Module views should express meaning, not hard-code fonts, colors, pixels, or animation.

## Lua host API

A Lua entry must return exactly one `gisland.module { ... }` definition. Its optional fields are:

- `every = "duration"`: run `update` periodically; durations are positive integer `ms`, `s`, `m`,
  or `h` values up to 24 hours.
- `init(config)`: run once with validated options before the module becomes ready.
- `update()`: return an object to emit `data`, or `nil` to emit nothing.
- `actions = { id = function(value) ... end }`: accept or reject semantic actions by returning
  `true`, `false`, or `false, "reason"`.
- `visibility(state)`: receive `hidden`, `compact-active`, or `expanded-active`.
- `shutdown()`: perform bounded cleanup during graceful shutdown.

Callbacks are serialized. `gisland.data(value)` emits a snapshot immediately.
`gisland.defer(callback)` schedules a callback for the next timer pass and
`gisland.after("500ms", callback)` schedules it later. Contiguous integer-keyed tables become JSON
arrays; use `gisland.array()` for an empty array. Other tables must have string keys.

For direct context publication, Lua also provides `gisland.publish(context)`,
`gisland.dismiss(context_id)`, and `gisland.log(level, message)`. `gisland.ui` constructors cover
`text`, `icon`, `image`, `rich_text`, `row`, `column`, `spacer`, `progress`, `indicator`, `button`,
and `action_region`. See the executable examples in
[`tests/fixtures/lua/example_data_module.lua`](../tests/fixtures/lua/example_data_module.lua) and
[`tests/fixtures/lua/example_action_module.lua`](../tests/fixtures/lua/example_action_module.lua).

## JSONL protocol for any language

Use this section when not using `gisland-lua-host`. Read stdin and stdout as UTF-8 JSON Lines: every
record is exactly one JSON object followed by `\n`. Do not write diagnostics to stdout; use stderr
or a protocol `log` record. Keep processing stdin until `shutdown` or EOF.

The core starts the handshake with `init`:

```json
{"type":"init","protocol":{"minimum":{"major":1,"minor":1},"maximum":{"major":1,"minor":8}},"instance_id":"weather-main","capabilities":["data-snapshots","context-images","rich-content","independent-views","ring-progress","status-indicator","compact-view-styles","icon-roles","progress-transitions"],"configuration":{"units":"metric"},"locale":"en_US.UTF-8","timezone":"Europe/Paris"}
```

Choose one version inside both the offered and manifest ranges, and echo only capabilities you will
actually use and that the core offered:

```json
{"type":"ready","protocol_major":1,"protocol_minor":1,"capabilities":["data-snapshots"]}
```

Do not send other records before `ready`. Feature introduction by protocol minor is:

| Version | Capability / behavior |
| --- | --- |
| 1.0 | `publish`, `dismiss`, actions, visibility, shutdown, and logs |
| 1.1 | `data-snapshots` and the `data` record |
| 1.2 | `context-images` with RGBA8 resources |
| 1.3 | `rich-content` (`rich_text`, links, inline images, `action_region`) |
| 1.4 | `independent-views` with `views` and expanded reveal presentation |
| 1.5 | `ring-progress` |
| 1.6 | `status-indicator` |
| 1.7 | `compact-view-styles`, `icon-roles`, and `progress-transitions` |
| 1.8 | correlated `action_result`, required for confirmed `gislandctl action` calls |

The core may then send:

```json
{"type":"visibility","visibility":"compact-active"}
{"type":"action","action_id":"refresh","value":{"force":true},"invocation_id":"42"}
{"type":"shutdown","reason":"reload","deadline_ms":1000}
```

`value` and `invocation_id` are optional. Invocation IDs are decimal strings and must be copied
unchanged into the corresponding result:

```json
{"type":"action_result","action_id":"refresh","invocation_id":"42","accepted":true}
```

For a rejection, set `accepted` to `false` and optionally add `message`. Other module-to-core
records are:

```json
{"type":"data","value":{"temperature":21.5}}
{"type":"dismiss","context_id":"weather-current"}
{"type":"log","level":"info","message":"weather updated"}
```

Log levels are `debug`, `info`, `warning`, and `error`. A `data.value` must be an object and requires
the negotiated `data-snapshots` capability plus a configured declarative view.

A direct scene publication can target the legacy coupled slots:

```json
{"type":"publish","context_id":"weather-current","priority":10,"expires_in_ms":300000,"compact":{"type":"text","value":"21 °C","role":"compact-primary"},"expanded":{"type":"column","gap":"small","children":[{"type":"text","value":"Sunny","role":"title"}]}}
```

With protocol 1.4 and `independent-views`, use `views.compact` and/or `views.expanded`. At least one
must be present. An optional expanded reveal lasts 1 through 60000 milliseconds:

```json
{"type":"publish","context_id":"alert","priority":50,"views":{"expanded":{"type":"text","value":"Rain starting","role":"body"}},"presentation":{"reveal":"expanded","duration_ms":3000}}
```

`priority` is an integer; higher priority wins, then newer publication. `expires_in_ms` removes the
context after a positive duration. Publishing the same `context_id` replaces that module's context.
`presentation.compact_style` selects a named compact geometry from the theme and requires protocol
1.7 with `compact-view-styles`.

Image resources require protocol 1.2 and are scoped to one publication. They are top-to-bottom,
tightly packed, sRGB, straight-alpha RGBA8 encoded as base64. Each axis is at most 512 pixels; a
context supports at most 16 resources and 4 MiB decoded data. The decoded length must be exactly
`width * height * 4`. Rich content and its capability are described with examples in the main
[README](../README.md#rich-content).

Malformed JSON, an out-of-range version, an unoffered capability, a capability used without
negotiation, or an invalid scene is a protocol violation for that process. Operational module
failure is logged and never terminates the graphical core; the instance restart policy applies.

## Reload and development loop

gisland watches enabled packages' `module.toml`, referenced `config.toml`, `view.toml`, and entry
file. Required Lua files are not watched transitively. Use `gislandctl reload` after changing those
files. Manifest, options, lifecycle, environment, and entry changes replace the process; a view-only
change keeps the process and reuses its last valid data. Invalid candidates are rejected
transactionally where possible, leaving the running configuration and last valid view intact.
