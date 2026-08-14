# Creating themes

A theme is a strict TOML file that maps the semantic roles emitted by modules to colors, fonts,
spacing, geometry, images, and motion. Themes are user-owned extensions: they do not need to be
added to the gisland repository.

## Install and select a theme

Copy the complete [default theme](../assets/themes/default.toml) as a starting point:

```bash
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/gisland/themes"
cp assets/themes/default.toml \
  "${XDG_CONFIG_HOME:-$HOME/.config}/gisland/themes/my-theme.toml"
```

When working from an installed package rather than a source checkout, obtain `default.toml` from
the project's repository and copy it to the same destination. Select the filename without `.toml`
in `~/.config/gisland/config.toml` (or the equivalent XDG path):

```toml
theme = "my-theme"
```

Apply and diagnose changes with:

```bash
gislandctl reload
journalctl --user -u gisland.service -f
```

User themes in `$XDG_CONFIG_HOME/gisland/themes` override a distributed theme with the same name.
Theme reload is transactional: invalid TOML, missing resources, or invalid render/layout values
reject the candidate and leave the current frame and theme active.

## Paths and semantic names

For a user theme, a relative font path is resolved from `$XDG_CONFIG_HOME/gisland`, not from the
`themes` subdirectory. This layout therefore works:

```text
~/.config/gisland/
├── config.toml
├── fonts/
│   ├── UI-Regular.ttf
│   └── Symbols.otf
└── themes/
    └── my-theme.toml
```

Theme tables are strict: unknown keys are rejected. Palette names, font IDs, typography roles, gap
and spacer tokens, icons, image roles, and compact style names are semantic identifiers. Modules
refer to those identifiers and the active theme supplies their visual representation. Preserve the
roles used by your enabled module views, or update those views at the same time.

Colors use `#RRGGBB` or `#RRGGBBAA`. Fields described as a theme color may instead reference a
declared palette role.

## Required structure

The default theme is the canonical, complete template. These tables are required:

| Table | Required content |
| --- | --- |
| `[palette]` | exactly `surface`, `foreground`, `muted`, `accent`, `success`, `warning`, `error` |
| `[fonts]` | at least one non-empty font ID and path |
| `[typography.<role>]` | at least `body`; every role needs `font` and positive `size` |
| `[gaps]`, `[spacers]` | non-negative pixel tokens; each needs `normal` |
| `[view.compact]`, `[view.expanded]` | padding, radius, border, and min/max dimensions |
| `[progress]` | ring and linear dimensions plus track color |
| `[shadow]` | offsets, blur, spread, and color |
| `[animation]` | transition durations/easing and a reduced-motion table |

Optional top-level tables are `[buttons]`, `[indicator]`, `[images.<role>]`, and
`[icons.<name>]`.

### Palette, fonts, and typography

```toml
[palette]
surface = "#111318"
foreground = "#F7F7F8"
muted = "#9096A2"
accent = "#78A9FF"
success = "#30D158"
warning = "#FFD60A"
error = "#FF453A"

[fonts]
regular = "fonts/UI-Regular.ttf"
semibold = "fonts/UI-Semibold.ttf"
icons = "fonts/Symbols.otf"

[typography.body]
font = "regular"
color = "foreground"
size = 16
weight = 400
line_height = 1.2

[typography.compact-primary]
font = "semibold"
color = "foreground"
size = 12
weight = 600
line_height = 1.0
```

`color` defaults to `foreground`, `weight` to `400`, and `line_height` to `1.2`. A font ID must
exist in `[fonts]`; a color role must exist in `[palette]`. Font weight is an integer from 1 to
1000. Add every typography role referenced by `text`, `rich_text`, or an icon role in your enabled
views.

### Gaps, spacers, and view geometry

```toml
[gaps]
xsmall = 4
small = 8
normal = 12
large = 20

[spacers]
xsmall = 2
small = 6
normal = 10
large = 18

[view.compact]
padding_horizontal = 14
padding_vertical = 4
radius = 16
border = 0
min_width = 230
max_width = 340
min_height = 32
max_height = 32

[view.expanded]
padding = 24
radius = 30
border = 0
min_width = 360
max_width = 432
min_height = 96
max_height = 344
```

`padding` is the uniform shorthand. Do not combine it with `padding_horizontal` or
`padding_vertical`; both axis-specific fields are required when the shorthand is absent. Maximum
dimensions must not be below minimum dimensions, and padding must leave positive content space.

Protocol-1.7 modules may request named compact geometry through
`presentation.compact_style`:

```toml
[view.compact.styles.hud-meter]
padding_horizontal = 12
padding_vertical = 0
radius = 18
border = 0
min_width = 220
max_width = 220
min_height = 36
max_height = 36
```

### Buttons, progress, indicators, and shadow

```toml
[buttons]
background = "surface"
disabled_background = "surface"
hover_overlay = "#FFFFFF14"

[progress]
ring_diameter = 32
ring_thickness = 4
linear_thickness = 5
compact_height = 48
track = "muted"
ring_track_opacity = 0.25

[indicator]
diameter = 7

[shadow]
offset_x = 0
offset_y = 6
blur = 18
spread = 0
color = "#00000066"
```

`[buttons]` is optional; when present, `background` and `disabled_background` are required and
`hover_overlay` is optional. Without the table, button backgrounds use `accent` and `muted` with a
subtle white hover overlay. `ring_track_opacity` is optional and ranges from 0 to 1. `[indicator]`
is optional and defaults to a 7-pixel diameter.

### Images

```toml
[images.notification-icon]
width = 24
height = 24
fit = "cover"
shape = "circle"
placement = "leading-cap"

[images.preview]
width = 96
height = 54
fit = "contain"
shape = "rounded"
radius = 10
```

`fit` is `contain` or `cover`. `shape` is `rectangle`, `circle`, or `rounded`; only a rounded shape
requires `radius`. Circular roles must be square. `placement` defaults to `flow`.
`leading-cap` requires a square circular image and is valid only when that image is the first child
of the root compact row. Image dimensions are positive and at most 512 pixels per axis.

### Animation and reduced motion

```toml
[animation]
compact_to_expanded_ms = 350
context_change_ms = 250
easing = "ease-in-out"

[animation.progress]
duration_ms = 270
easing = "ease-out"

[animation.reduced_motion]
compact_to_expanded_ms = 0
context_change_ms = 0

[animation.reduced_motion.progress]
duration_ms = 0
```

Easing values are `linear`, `ease-in`, `ease-out`, and `ease-in-out`. Durations are integer
milliseconds from 0 through 60000. The progress subtables are optional and default to the values
shown, but `[animation.reduced_motion]` itself is required.

### Icons

```toml
[icons.calendar]
font = "icons"
codepoint = 0xF133
```

The font must exist in `[fonts]`; `codepoint` is a valid Unicode scalar value provided by that
font. Scene icon names resolve through this table. The optional protocol-1.7 icon `role` controls
typography separately from the glyph lookup and must name a declared typography role.

## Safe editing workflow

Keep a terminal following the user-service journal, edit one semantic group at a time, and run
`gislandctl reload`. A successful reload applies to retained contexts without restarting modules.
If a module logs an unknown typography, gap, spacer, icon, image, state, or compact-style role, add
that semantic entry or change the module view. The previous valid theme remains usable after a
rejected reload.
