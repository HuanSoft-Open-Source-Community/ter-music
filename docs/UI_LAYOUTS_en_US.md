# TUI Layout Reference (English)

Ter-Music's main view is data-driven. A layout preset lists the panes that should
appear and gives each of them a per-mille rectangle. Every layout must **tile the
main area exactly**: the visible panes must not overlap, and their combined area
must be `1000 × 1000 = 1,000,000`. A layout with a hole, an overlap, or a wrong
area sum is rejected as a whole.

The main area is the terminal **minus the bottom menu bar**:

```
x = 0, y = 0, w = COLS, h = LINES - 1
```

The bottom row (`LINES - 1`) is reserved for the `F1:... F9:...` hint bar.

## Pane kinds

| id | Contents |
| --- | --- |
| `playlist` | Song list / play queue |
| `info` | Playback state, track metadata, audio parameters, colored half-block character cover (256/8/mono fallback) |
| `controls` | Progress bar and transport buttons |
| `spectrum` | Audio-reactive bar spectrum |
| `lyrics` | Current lyric lines |
| `vinyl` | Colored half-block vinyl disc with a rotating album-cover center label and colored spectrum bars (falls back to the glyph animation when colors are unavailable; split out of the lyrics pane) |

`lyrics` and `vinyl` have a special relationship:

- both visible in a layout → `lyrics` is hidden at runtime; `vinyl` is shown;
- `lyrics` only → a track without lyrics still gets the vinyl animation inside
  the lyrics pane;
- `vinyl` only → the vinyl pane is shown whether or not the track has lyrics.

The fallback happens **before** the tiling check. Therefore a layout that defines
both panes must give them suitable rectangles: after `lyrics` is hidden, the
remaining panes must still tile the whole main area.

## File locations

- Built-in presets: `<prefix>/share/ter-music/layouts/layouts.json`
  (in the source tree: `data/layouts/layouts.json`)
- User overrides: `~/.config/ter-music/layouts.json`
  (`$XDG_CONFIG_HOME` is respected; the same `id` overrides the built-in preset)
- Development/testing override: `TER_MUSIC_LAYOUTS=/path/to/layouts.json`
  (loaded last, highest priority)
- Selected layout id: `~/.config/ter-music/ui-layout` (one line, front-end local;
  it is **not** part of `config.xml` and is not synchronized over D-Bus)

Load order: built-in → user → `TER_MUSIC_LAYOUTS`.

## Top-level JSON

```json
{
  "version": 1,
  "default": "classic",
  "layouts": [
    {
      "id": "classic",
      "name": "Classic",
      "name_key": "layout.preset.classic",
      "panes": [
        { "id": "playlist", "x": 0,   "y": 0,   "w": 680, "h": 520 },
        { "id": "info",     "x": 0,   "y": 520, "w": 680, "h": 260 },
        { "id": "controls", "x": 0,   "y": 780, "w": 680, "h": 220 },
        { "id": "spectrum", "x": 680, "y": 0,   "w": 320, "h": 200 },
        { "id": "lyrics",   "x": 680, "y": 200, "w": 320, "h": 800 }
      ]
    }
  ]
}
```

## Fields

### Layout

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | yes | `[A-Za-z0-9_-]`, max 31 chars; unique within a catalog |
| `name` | no | display name, max 63 chars; falls back to `id` |
| `name_key` | no | i18n key; used when a translation exists, otherwise `name` |
| `panes` | yes | 1–6 unique pane objects |

### Pane

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | yes | `playlist`, `info`, `controls`, `spectrum`, `lyrics`, `vinyl` |
| `x`, `y`, `w`, `h` | yes | per-mille integers (0–1000); `w > 0`, `h > 0`; `x+w <= 1000`, `y+h <= 1000` |
| `visible` | no | `true`/`false` or `1`/`0`, default `true` |
| `fg`, `bg` | no | pane text/background colour |
| `border_fg`, `border_bg` | no | border colour; when omitted, it follows the body colour |

Colour values:

- `-2` or omitted: inherit the pane role's theme colour
  (playlist/info → playlist theme, controls → controls theme,
  spectrum/lyrics/vinyl → lyrics theme)
- `-1`: terminal default colour
- `0..255`: terminal palette index (values above the terminal's `COLORS-1` are
  clamped)

Unknown fields are ignored, which keeps older builds able to read newer files.
`version > 1` is accepted and known fields are still parsed.

## Tiling rules

A layout is accepted only when all of the following hold after the
lyrics/vinyl fallback:

1. at least one visible pane;
2. no two visible rectangles overlap (touching edges is allowed);
3. `Σ (w × h)` of all visible panes equals `1,000,000`.

Because non-overlapping rectangles have an area equal to their union, condition
3 means the union is the whole main area. There is no gutter and no padding.

At runtime the per-mille edges are rounded with one shared boundary formula:

```
start = origin + units * extent / 1000
end   = origin + (units + span) * extent / 1000
size  = end - start
```

Adjacent panes share the same boundary, so the pixel partition has no gaps and no
double-owned cells. A pane can become `1 × N` or `N × 1` on a very small terminal;
it is still created and filled with its background. A pane whose width or height
rounds to 0 owns no cells and gets no window; a neighbouring pane covers those
sub-pixel strips, so no blank cell appears.

If a positive-area window cannot be created, the runtime falls back to a
single full-screen `playlist` rescue layout, so the main area remains filled.

## Content bounds

Every renderer draws inside the pane's content rectangle (rows `1..h-2`, columns
`1..w-2`); writes never touch the border cells. Dynamic text is clipped to the
remaining content width, and a pane that is too small for a particular renderer
shows only its background and border instead of overflowing. The scrollbar is
clamped to the content height as well.

The info pane rearranges itself from its own size: wide panes put the metadata
column and the audio-parameter column side by side with the colored half-block cover on
the right, while narrow-and-tall panes stack the cover above the metadata and
audio blocks. The cover is dropped when it cannot keep a blank column before the
right border, and every block is clipped to the region chosen by the layout. The
TUI cover is drawn with colored half-block characters so its glyph advance matches the
terminal cell grid, and is colored with an automatic 256-color → 8-color →
monochrome fallback (a tight color-pair budget can reduce the palette further).
Color mode composes both halves with `▀` (foreground = upper half, background =
lower half); `█` is used only by the monochrome fallback. The CLI/D-Bus text
cover still honours the configured braille/ASCII/half-block charset. The controls-pane cover is bounded by the same
content rectangle, so no cover paints on a pane border. This keeps small or
unusually proportioned layouts visually intact.

The vinyl pane uses the same color tiers and pair-budget discipline. When
colored vinyl is enabled the remaining cover pairs are split between the info
cover, the controls cover and the vinyl label as 2:1:1, and 12 fixed pairs are
reserved for the disc's three shades and the bar ramp. The center label is the
current cover cropped to a circle and rotated with the disc. When the UI is
ASCII, colors are unavailable, or the usable pair budget is below the
colored-vinyl threshold, the whole vinyl animation stays on the original glyph
rendering and the info/controls budgets keep their old 2:1 split. A missing
cover keeps the colored disc and bars and leaves the center as a plain dark
disc.

## Built-in presets

| id | Panes | Notes |
| --- | --- | --- |
| `classic` | playlist, info, controls, spectrum, lyrics | default, familiar five-pane arrangement |
| `vinyl` | playlist, info, controls, spectrum, vinyl | lyrics replaced by the standalone vinyl pane |
| `dual` | all six; lyrics and vinyl overlap by definition | after fallback, lyrics is hidden and the remaining panes tile exactly |
| `karaoke` | playlist, info, controls, lyrics | lyrics column dominates; no spectrum/vinyl |
| `visualizer` | playlist, info, controls, spectrum, vinyl | large spectrum + vinyl, no lyrics pane |
| `focus` | playlist, info, controls | full-width rows; maps the old `show_lyrics_panel=0` default |
| `stage` | playlist, info, spectrum, lyrics, controls | top row + bottom control row + split middle; T-junctions but no gaps |
| `neon` | classic geometry + explicit per-pane colours | demonstrates colour overrides; clamps on 8-colour terminals |

## Custom layout example

Two panes side by side, with a coloured playlist pane:

```json
{
  "version": 1,
  "default": "split",
  "layouts": [
    {
      "id": "split",
      "name": "My Split",
      "panes": [
        { "id": "playlist", "x": 0,   "y": 0, "w": 500, "h": 1000,
          "fg": 51, "border_fg": 45 },
        { "id": "info",     "x": 500, "y": 0, "w": 500, "h": 1000 }
      ]
    }
  ]
}
```

Save it to `~/.config/ter-music/layouts.json`, choose
`F2 → Appearance → UI Layout → My Split`, or `Reload Layouts` after editing.

## Troubleshooting

- **My layout is missing from the list**: it was rejected. Check the log
  (`--debug`) for messages such as “does not tile the main area” or “overlap”.
- **I changed a pane width and the layout disappeared**: update the adjacent
  pane(s) so the area sum stays at 1,000,000.
- **Colours do not follow the theme**: the preset specifies explicit `fg`/`bg`.
  Remove those fields to inherit the theme again. Rainbow mode ignores explicit
  layout colours.
- **I selected a layout but the list is still the old one**: press
  `Reload Layouts` in Settings.
- **No layout files are installed**: the compiled-in `classic` rescue layout is
  used and the TUI still works.
