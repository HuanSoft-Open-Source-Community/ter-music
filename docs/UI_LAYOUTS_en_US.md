# TUI Layout Reference (English)

Ter-Music's main view is data-driven. A layout is a **binary split tree**: each
split cuts its rectangle into two children, so the result is always a forced
tiling with no gaps and no overlaps. A leaf names one pane kind. The tree is the
authoritative model; the old flat rectangle list is still accepted and converted
automatically.

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

`lyrics` and `vinyl` may **coexist in one layout** (they are separate panes):

- the editor offers both kinds as long as the layout does not already use them;
- when a layout contains both, the current track decides the screen: with lyrics
  both panes are drawn (lyric lines, plus the disc in its own pane); without
  lyrics the **lyrics pane steps aside for that track** — its leaf is removed
  from the resolved tree, the sibling panes take over its area, and it comes
  back as soon as a track with lyrics is playing. The stored layout, the colour
  settings and the user layout file are not modified by this;
- a `lyrics`-only layout still falls back to the vinyl animation inside the
  lyrics pane for a track without lyrics; the fallback skips its spectrum bars
  when the layout already has a dedicated `spectrum` pane;
- a `vinyl`-only layout always shows the vinyl pane.

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

## v2 JSON schema

```json
{
  "version": 2,
  "default": "classic",
  "layouts": [
    {
      "id": "classic",
      "name": "Classic",
      "name_key": "layout.preset.classic",
      "tree": {
        "split": "row",
        "ratio": 680,
        "children": [
          {
            "split": "col",
            "ratio": 520,
            "children": [
              { "pane": "playlist" },
              {
                "split": "col",
                "ratio": 542,
                "children": [
                  { "pane": "info" },
                  { "pane": "controls" }
                ]
              }
            ]
          },
          {
            "split": "col",
            "ratio": 200,
            "children": [
              { "pane": "spectrum" },
              { "pane": "lyrics" }
            ]
          }
        ]
      },
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

### Layout object

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | yes | `[A-Za-z0-9_-]`, max 31 chars; unique within a catalog |
| `name` | no | display name, max 63 chars; falls back to `id` |
| `name_key` | no | i18n key; used when a translation exists, otherwise `name` |
| `tree` | v2 yes | one split/leaf tree with 1–5 unique pane leaves |
| `panes` | v2 mirror | derived per-mille rectangles; kept so older builds can read v2 files |

`tree` is authoritative. Hand-editing `panes` without changing `tree` has no
effect. The serializer always writes both.

### Split node

| Field | Required | Meaning |
| --- | --- | --- |
| `split` | yes | `row` = children side by side (left/right); `col` = stacked (top/bottom) |
| `ratio` | no | first child's share of this node's axis, per-mille `1..999`; default `500` |
| `children` | yes | array of exactly two nodes; extra nodes are ignored and the layout is auto-fitted |

A split's two children share the same cut coordinate, so they cannot overlap or
leave a gap.

### Leaf node

| Field | Required | Meaning |
| --- | --- | --- |
| `pane` | yes | `playlist`, `info`, `controls`, `spectrum`, `lyrics`, `vinyl` |
| `fg`, `bg` | no | pane text/background colour; editable per pane under Settings → Appearance |
| `border_fg`, `border_bg` | no | border colour; when omitted, it follows the body colour |

Colour values:

- `-2` or omitted: inherit the pane role's theme colour
  (playlist/info → playlist theme, controls → controls theme,
  spectrum/lyrics/vinyl → lyrics theme)
- `-1`: terminal default colour
- `0..255`: terminal palette index (values above the terminal's `COLORS-1` are
  clamped)

Out-of-range values are clamped, not rejected. Unknown fields are ignored.

## Auto-fitting (no more whole-layout rejection)

The parser only skips data it cannot interpret structurally (broken top-level
JSON, missing layout array, layout without a valid `id`). Everything else is
fitted to a valid tree:

| Input problem | Result |
| --- | --- |
| v1 `panes` with a guillotine cut | converted to an exact split tree; geometry preserved |
| v1 `panes` with a hole, overlap, or out-of-bounds rect | rects clamped/filled and the layout is marked auto-fitted |
| v1 layout with no full guillotine cut (e.g. a windmill) | panes reflowed in reading order, marked auto-fitted |
| `tree` with an unknown `split` | treated as `row`, marked auto-fitted |
| `ratio` outside `1..999` | clamped, marked auto-fitted |
| unknown / duplicate leaf, extra children | leaf/children ignored, marked auto-fitted |
| `tree` empty or unusable | falls back to the v1 `panes` mirror; if that is unusable, classic geometry is used while preserving `id`/`name` |
| both `lyrics` and `vinyl` | both kept (they are separate panes; see above) |
| a normalized tree that still fails the tiling invariant (defensive; e.g. duplicate leaves) | the smallest pane is dropped until the tree validates; if nothing works, classic geometry is used |

A layout that was auto-fitted has `LayoutSpec.normalized = 1` internally. The
settings reload message reports how many layouts were auto-fitted, and the
editor shows a notice when it opens such a layout. Saving writes the canonical
v2 tree, so the notice disappears next time.

## v1 compatibility

Old files keep working:

```json
{
  "version": 1,
  "layouts": [
    {
      "id": "split",
      "name": "My Split",
      "panes": [
        { "id": "playlist", "x": 0,   "y": 0, "w": 500, "h": 1000 },
        { "id": "info",     "x": 500, "y": 0, "w": 500, "h": 1000 }
      ]
    }
  ]
}
```

The parser looks for a full guillotine cut, choosing the most balanced one and
preferring a vertical cut on ties. It encodes the cut as
`ceil(offset * 1000 / span)`, which reproduces the original edge under the
runtime's floor formula. If no cut exists, panes are reflowed in reading order.
The next editor save rewrites the file as v2 with a `tree` and a `panes` mirror.

## Runtime geometry

The per-mille rectangles are expanded from the tree, then mapped to terminal
cells with one shared boundary formula:

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
If the selected tree somehow fails validation, the compiled-in classic tree is
used; this is a defensive fallback, not a user-facing rejection.

## Built-in presets

| id | Panes | Tree (first child share) |
| --- | --- | --- |
| `classic` | playlist, info, controls, spectrum, lyrics | `row 680 [ col 520 [playlist, col 542 [info, controls]], col 200 [spectrum, lyrics] ]` |
| `vinyl` | playlist, info, controls, spectrum, vinyl | classic tree with `vinyl` in place of `lyrics` |
| `karaoke` | playlist, info, controls, lyrics | `row 600 [ col 560 [playlist, col 546 [info, controls]], lyrics ]` |
| `visualizer` | playlist, info, controls, spectrum, vinyl | `row 560 [ col 600 [playlist, col 500 [info, controls]], col 640 [spectrum, vinyl] ]` |
| `focus` | playlist, info, controls | `col 520 [playlist, col 542 [info, controls]]` |
| `stage` | playlist, info, spectrum, lyrics, controls | `col 450 [playlist, col 637 [row 500 [info, col 429 [spectrum, lyrics]], controls]]` |
| `neon` | classic geometry, brightest palette | classic tree with a vivid per-pane palette |

`classic` is the default.

### Preset colours

Every preset carries an explicit foreground colour for **every** pane, and every
pane background is `-1` (transparent, i.e. whatever the terminal draws). Within
one preset no two panes share a colour — borders inherit the pane colour, so they
differ too. Colours are picked so the guarantee survives the fallback chain:

| Preset | playlist | info | controls | spectrum | lyrics / vinyl |
| --- | --- | --- | --- | --- | --- |
| `classic` | 15 bright white | 37 teal | 208 orange | 10 bright green | lyrics 198 pink |
| `vinyl` | 15 bright white | 37 teal | 208 orange | 10 bright green | vinyl 203 coral |
| `karaoke` | 208 orange | 15 bright white | 198 pink | — | lyrics 203 coral |
| `visualizer` | 15 bright white | 75 sky blue | 118 lime | 10 bright green | vinyl 93 purple |
| `focus` | 15 bright white | 37 teal | 208 orange | — | — |
| `stage` | 208 orange | 15 bright white | 37 teal | 10 bright green | lyrics 198 pink |
| `neon` | 15 bright white | 14 bright cyan | 11 bright yellow | 10 bright green | lyrics 13 bright magenta |

The values map down without collapsing: on a 16-colour terminal each preset still
has distinct colours, and on an 8-colour terminal they land on distinct base
colours (white / cyan / yellow / green / magenta / red). Every value is also one
of the colours the Settings palette offers, so the name shown there ("bright
white", "teal", …) is meaningful. On a monochrome terminal pane colours are not
enabled at all — that is the third fallback level.

Because the presets colour every pane they contain, Settings shows each of those
rows as `[layout]` while a preset is selected (a pane the preset does not contain
reads `[theme] …`, `no pane` when it is absent); pick `Follow theme` on a row to
hand that field back to the global theme.

## Custom tree example

Two panes side by side, with a coloured playlist pane:

```json
{
  "version": 2,
  "default": "split",
  "layouts": [
    {
      "id": "split",
      "name": "My Split",
      "name_key": "",
      "tree": {
        "split": "row",
        "ratio": 500,
        "children": [
          { "pane": "playlist", "fg": 51, "border_fg": 45 },
          { "pane": "info" }
        ]
      },
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

## Layout editor

Settings → Appearance offers two action items:

- **Edit Layout** (`settings.opt.layout_edit`): opens the editor on the current
  layout. Editing a built-in preset keeps its `id`; saving writes a user
  override file that shadows the preset by the same id.
- **New Layout** (`settings.opt.layout_new`): seeds the editor with the current
  layout as a template and generates a unique `user_N` id.

The editor always works on a split tree, so every operation produces a valid
forced tiling. The left side lists the panes (localized name + share of the main
area); the right side is a live schematic preview; the bottom shows the current
split and three lines of key hints.

### Main keys

| Key | Action |
| --- | --- |
| `↑`/`↓`/`←`/`→` | select the adjacent pane by geometry |
| `Tab` / `Shift+Tab` | cycle panes in tree order |
| `[` / `]` | shrink/grow the selected pane **horizontally** by 1% (nearest row split) |
| `{` / `}` | shrink/grow the selected pane **vertically** by 1% (nearest column split) |
| `PgUp` / `PgDn` | adjust the horizontal ratio by 5% |
| `=` | restore the nearest parent split to 50/50 |
| `|` | split side by side; then choose the new pane type |
| `-` | split stacked; then choose the new pane type |
| `x` | remove the selected pane (at least one remains; the sibling takes over) |
| `t` | change the selected pane's type (any kind the layout does not already use) |
| `w` | swap types with the next pane in tree order |
| `c` | open the colour panel |
| `n` | rename the layout |
| `Enter` | open the action menu |
| `?` / `F1` | open the full key help |
| `S` / `Ctrl+S` | save |
| `Esc` / `q` | back/close (press twice when changes are unsaved) |

`Esc` and `q` are handled by the editor itself; they do not exit the settings
view or the application while the editor is open. `Ctrl+C` remains the
emergency quit.

### Action menu

`Enter` opens: split side by side, split stacked, change pane type, edit
colours, restore 50/50, swap with next pane, remove pane, rename layout.
`↑`/`↓` select, `Enter` runs, `Esc` returns.

### Colour panel

| Key | Action |
| --- | --- |
| `↑`/`↓` | select fg / bg / border_fg / border_bg |
| `←`/`→` | cycle `-2 (inherit) → -1 (terminal default) → 0..255` |
| `PgUp`/`PgDn` | cycle by 10 |
| `Home` / `End` | set inherit / terminal default |
| `r` | reset the current field to inherit |
| `R` | reset all four fields to inherit |
| `Enter` / `Esc` | back to the main screen |

### Limits and feedback

- The editor needs at least 60 columns × 16 rows.
- A pane must keep at least 1% of each axis (`LAYOUT_MIN_PANE_UNITS = 10`
  per-mille). Operations that would violate this are refused with a status
  message and the layout is unchanged. Ratio nudges are the exception: a step
  that would break the limit is shortened (half, quarter, …, down to one unit) so
  the split keeps moving until it really is at the limit; only then does the
  status line say the ratio is already at its limit.
- A split never offers a kind that is already used in the layout; `lyrics` and
  `vinyl` are ordinary kinds and can both appear. Changing type with `t` only
  offers unused kinds as well (the leaf's own kind is not a candidate, since
  replacing a kind with itself would be a no-op).
- The footer status line reports the last action or error. There is no
  "validation failed, cannot save" path for normal edits; `S` saves directly.
- Saving writes the user file atomically, reloads the catalog, selects the
  saved layout immediately and closes the editor.
- Editing a built-in preset keeps its localised name: the preset's `name_key`
  is written into the override, so the layout menu keeps showing the translated
  name. Renaming with `n` replaces it with your own text (the override then
  stores `name` only). Older overrides that lost their `name_key` while keeping
  the preset's original name get the key back when the catalog is loaded.

### User file management

The user file `~/.config/ter-music/layouts.json` is rewritten atomically
(write-to-temp + rename) and preserves the other user layouts it contains.
Notes:

- Up to `LAYOUT_MAX_CATALOG` (24) user layouts; beyond that the save fails
  with a message.
- The file is re-serialized from the parsed catalog, so hand-added unknown
  fields are dropped by an editor save (the parser itself ignores them too).
- If the file exists but is unreadable/corrupt, an editor save rebuilds it
  from scratch (a warning is logged).
- Deleting the user file (or removing a layout from it) restores the built-in
  preset's original definition.

## Content bounds

Every renderer draws inside the pane's content rectangle (rows `1..h-2`, columns
`1..w-2`); writes never touch the border cells. Dynamic text is clipped to the
remaining content width, and a pane that is too small for a particular renderer
shows only its background and border instead of overflowing. The scrollbar is
clamped to the content height as well.

The info pane rearranges itself from its own size: wide panes put the metadata
column and the audio-parameter column side by side with the colored half-block cover on
the right, while narrow-and-tall panes stack the cover above the metadata and
audio blocks. The content block is **vertically centred** in the pane (cover and
text move together in stacked mode), so a tall info pane no longer pins the text
to the title row. The cover is dropped when it cannot keep a blank column before the
right border, and every block is clipped to the region chosen by the layout. The
TUI cover is drawn with colored half-block characters so its glyph advance matches the
terminal cell grid, and is colored with an automatic 256-color → 8-color →
monochrome fallback (a tight color-pair budget can reduce the palette further).
Color mode composes both halves with `▀` (foreground = upper half, background =
lower half); `█` is used only by the monochrome fallback. The CLI/D-Bus text
cover still honours the configured braille/ASCII/half-block charset. The controls-pane cover is bounded by the same
content rectangle, so no cover paints on a pane border. This keeps small or
unusually proportioned layouts visually intact.

## Colour precedence

Rainbow mode → per-pane explicit colours (tree leaves / user file) →
theme colours from Settings (`playlist`/`controls`/`lyrics` fg+bg). To hand a
pane back to the theme, use the layout editor (`c` → `r`/`R`) or the Settings
colour page (choose `Follow theme` in the palette). Rainbow mode still ignores
explicit layout colours.

### Settings colour page (Appearance)

Colours live in two layers, and the Settings page exposes both:

| Rows | Bound to | Stored in |
| --- | --- | --- |
| 12 pane rows: playlist, info, controls, spectrum, lyrics, vinyl (fg + bg each) | the **current layout**'s `panes[].fg` / `panes[].bg` | `~/.config/ter-music/layouts.json` |
| 6 interface rows: sidebar, highlight, border (fg + bg each) | the global theme | `config.xml` (`<theme>`) |

Every pane row is tagged `[layout]` when the current layout specifies that
field explicitly, and `[theme]` when the field is still inherited. The stock
presets colour every pane they contain, so those rows read `[layout]`; a user
layout that omits a colour shows `[theme]` and inherits the playlist / lyrics
theme colour.

Changing a row writes to exactly one layer, following this order:

1. the layout already specifies the field → write the layout (the `[layout]`
   tag stays);
2. the field is inherited **and** the pane owns a theme slot (playlist,
   controls, lyrics) → write that theme slot, i.e. the old global behaviour:
   every layout that inherits it changes together and the tag stays `[theme]`;
3. the field is inherited **and** the pane has no theme slot of its own (info,
   spectrum, vinyl) → write an explicit layout override, so the pane really
   becomes independent (the tag flips to `[layout]`);
4. sidebar / highlight / border are interface colours → always the theme.

Writing the layout persists first and only then mutates memory, so a failed
write leaves the interface untouched:

- A built-in preset or a user layout is saved back under the **same id** as a
  user override. Repeated colour tweaks never add rows to the user file.
- If the current layout comes from `TER_MUSIC_LAYOUTS`, a same-id override would
  be hidden again by that file on the next reload, so the change is stored as an
  auto-incremented copy (`<id>-2`, `<id>-3`, …) named `<preset name> N`, and the
  player switches to it. The status line says so.
- Colours of a pane that the current layout does not contain (for example
  `vinyl` while `lyrics` is used, or a layout without `spectrum`) cannot be
  persisted — the serializer only writes tree leaves. Such a row is tagged
  `no pane` after its source tag (`[theme no pane]`, or `[layout no pane]` when
  it already carries an override) and adjusting it only reports that the pane is
  not in this layout; switch or edit the layout first.
- Terminals with `COLOR_PAIRS <= 26` cannot allocate the per-pane colour pairs,
  so explicit layout colours are ignored there; the page still shows the layout
  values with the `[layout]` tag.

The section needs 22 list rows plus the header and three pinned hint lines, so on
terminals shorter than 32 rows the list scrolls with the selection and draws a
scrollbar in the reserved column; the three hint lines stay at the bottom.

## Troubleshooting

- **My old layout has a different shape after upgrading**: it contained a gap,
  overlap, out-of-bounds rect, or a non-guillotine arrangement and was
  auto-fitted. The reload message reports the count; open the editor to see the
  notice and adjust the split tree. It is no longer removed from the list.
- **The editor refuses a split / ratio change**: the result would make a pane
  smaller than 1% of an axis. The status line says so and the layout is
  unchanged. Pressing `{` / `}` on a pane whose ancestors are all horizontal
  splits reports that there is no split on that axis — the pane genuinely has no
  vertical neighbour to resize.
- **I edited `panes` by hand and nothing changed**: `tree` is authoritative.
  Edit `tree`, or use the editor and let it write both.
- **Colours do not follow the theme**: the pane carries explicit `fg`/`bg`
  values, which take precedence over the theme. Reset them with `c` → `r`/`R`
  in the editor, or choose `Follow theme` for that row in Settings → Appearance
  (the row is tagged `[layout]` while an override is in effect).
- **A row I changed also moved another pane, or a preset kept its own colours**:
  the pane rows describe the *current layout*. A `[theme]` row writes the global
  theme, so playlist and info (or spectrum, lyrics and vinyl) move together;
  a `[layout]` row writes only this layout, so a preset that ships its own
  colours (`neon`) keeps them until you pick `Follow theme`.
- **The lyrics pane disappeared although the layout has one**: the layout also
  has a `vinyl` pane and the current track has no lyrics, so the lyrics pane is
  stepping aside for that track and the other panes fill its area. Start a track
  with lyrics (or remove the `vinyl` pane) to get it back; the layout itself is
  untouched.
- **I selected a layout but the list is still the old one**: press
  `Reload Layouts` in Settings.
- **No layout files are installed**: the compiled-in `classic` tree is used and
  the TUI still works.
