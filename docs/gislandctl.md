# Using `gislandctl`

`gislandctl` is the stable command-line interface for users, key bindings, and local automation. It
connects to the running user's private `$XDG_RUNTIME_DIR/gisland.sock`; it does not start gisland and
does not contact a remote service.

Run `gislandctl help` or `gislandctl --help` for the installed command summary.

## Commands

| Command | Effect |
| --- | --- |
| `gislandctl open` | Open the expanded island. |
| `gislandctl close` | Return to compact mode. |
| `gislandctl toggle` | Toggle compact/expanded mode. |
| `gislandctl status [--json]` | Show mode, compact/expanded owners, module states, and socket. |
| `gislandctl modules` | List configured module instances and their state. |
| `gislandctl reload` | Transactionally reload configuration, theme, and module package files. |
| `gislandctl module restart <instance>` | Gracefully replace one enabled instance. |
| `gislandctl activate <instance> [--duration <duration>]` | Make an instance's context active and return to compact mode. |
| `gislandctl activate-open <instance>` | Atomically activate an instance and open the island. |
| `gislandctl dismiss <context>` | Dismiss a context by context ID. |
| `gislandctl action <instance> <action> [--value <json>]` | Invoke and await a module action. |

An *instance* is the `id` in one `[[modules]]` configuration entry, not necessarily the package ID.
A *context* is the `context_id` published by a module. Use `status --json` to discover the currently
selected context IDs and `modules` to discover instance IDs.

Durations are positive unsigned integers followed by `ms`, `s`, `m`, or `h`, from 1 ms through 24
hours. Examples: `250ms`, `5s`, `10m`, `1h`. Fractional values such as `1.5s` are invalid.

## Status and shell automation

Human-readable output is intended for terminals. Scripts should use `status --json`, whose current
schema has `format_version: 2`:

```json
{
  "format_version": 2,
  "mode": "compact",
  "compact": {"instance_id":"clock","context_id":"clock","priority":0},
  "expanded": {"instance_id":"clock","context_id":"clock","priority":0},
  "modules": [
    {"id":"clock","state":"running","available":true}
  ],
  "socket": "/run/user/1000/gisland.sock"
}
```

`compact` or `expanded` is `null` when that slot has no selected context. Module states are
`disabled`, `stopped`, `starting`, `running`, `backoff`, `stopping`, or `failed`. `available` means
the instance can currently participate in controls; it is distinct from the lifecycle state.

Example with `jq`:

```bash
if gislandctl status --json | jq -e '.modules[] | select(.id == "audio" and .available)'>/dev/null
then
  gislandctl action audio toggle-mute
fi
```

Commands exit with status 0 only after a successful response. Argument errors, a missing/unusable
runtime directory or socket, rejected controls, and protocol errors write a message to stderr and
exit nonzero. This makes ordinary shell conditionals sufficient; do not parse human-readable error
sentences as a stable interface.

Only `status` accepts `--json`. `modules` has a stable human-readable listing but no JSON flag at
present.

## Module actions and JSON values

`action` routes the same semantic action used by rendered buttons, links, and action regions. The
module must be running and must have negotiated protocol 1.8 so gisland can correlate the reply.
The command waits for acceptance or rejection:

```bash
gislandctl action audio toggle-mute
gislandctl action audio set-volume --value 42
gislandctl action weather select-city --value '"Paris"'
gislandctl action player seek --value '{"seconds":30,"relative":true}'
```

The text after `--value` must be exactly one valid JSON value. Shell quoting is therefore important:
numbers and booleans can be passed directly, JSON strings need both shell and JSON quotes, and
objects/arrays should normally use single quotes around the JSON. Omitting `--value` is different
from explicitly passing `--value null`.

Action failures are nonzero and include unknown/unavailable instances, a module protocol older than
1.8, delivery failure, timeout, cancellation, and a module's explicit rejection. An action is never
executed by the gisland core; only the addressed module interprets its semantic ID and value.

## Activation, opening, and dismissal

`activate <instance>` selects the instance's available context and returns to compact mode.
`--duration` makes that activation temporary. `activate-open` performs selection and opening as one
control, which avoids intermediate state in a key binding.

`dismiss` takes a context ID, not an instance ID. Dismissal removes that context from arbitration;
the module may publish it again later.

## Reload and restart

`reload` rereads the exact configuration path selected when gisland started, then the selected
theme and enabled package files. Validation and render preflight are transactional. A rejected
reload reports `reload_rejected` and keeps the existing configuration, frame, and processes.

Unchanged instances preserve their PID and runtime state. Changes to command, options, restart
policy, lifecycle timing, environment, working directory, manifest, configuration, or entry replace
the affected process. A package view-only change reuses its last valid data without a restart.

`module restart <instance>` is an explicit process replacement. It is useful after changing an
unwatched dependency such as a Lua file loaded through `require`.

## Window-manager examples

i3 bindings are ordinary process commands:

```i3
bindsym $mod+grave exec --no-startup-id gislandctl toggle
bindsym $mod+Shift+grave exec --no-startup-id gislandctl close
bindsym XF86AudioMute exec --no-startup-id gislandctl action audio toggle-mute
bindsym XF86AudioRaiseVolume exec --no-startup-id gislandctl action audio volume-up
bindsym XF86AudioLowerVolume exec --no-startup-id gislandctl action audio volume-down
```

For `sxhkd`:

```text
super + grave
    gislandctl toggle

super + shift + grave
    gislandctl close
```

Ensure the window manager exports the same X11 and runtime environment used by the user service.
For a systemd user service, the usual session setup is:

```bash
systemctl --user import-environment DISPLAY XAUTHORITY
systemctl --user start gisland.service
```

## Troubleshooting

- `XDG_RUNTIME_DIR is unset`: run the command in the logged-in user's session; do not invent a
  shared runtime directory.
- Socket connection failure: verify `systemctl --user status gisland.service` and follow
  `journalctl --user -u gisland.service -f`.
- `unknown_instance`: use the configured `[[modules]].id` shown by `gislandctl modules`.
- `unknown_context`: inspect `compact.context_id` and `expanded.context_id` in `status --json`.
- `reload_rejected`: the journal identifies the TOML path, package file, theme resource, or render
  preflight error; fix it and retry.
- `unsupported_module_protocol` for an action: update the module to protocol 1.8 and return the
  correlated `invocation_id` in `action_result`.
