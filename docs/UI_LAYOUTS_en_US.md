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
| `controls` | Progress bar and transport buttons; the loop-mode, speed and volume buttons open a pop-up list. The karaoke (vocal removal) button is always shown and clickable: there is no feature switch and no model picker, its pop-up offers Off / Faint vocals / No vocals, and Settings → Playback exposes the same three levels plus the bundled model's attribution |
| `spectrum` | Audio-reactive bar spectrum |
| `lyrics` | Current lyric lines |
| `vinyl` | Colored half-block vinyl disc with a rotating album-cover center label and colored spectrum bars (falls back to the glyph animation when colors are unavailable; split out of the lyrics pane) |

`lyrics` and `vinyl` may **coexist in one layout** (they are separate panes):

- the editor offers both kinds as long as the layout does not already use them;
- when a layout contains both, the current track decides the screen: with lyrics
  both panes are drawn (lyric lines, plus the disc in its own pane); without
  lyrics the **lyrics pane steps aside for that track** — its leaf is removed
  from the resolved tree, the sibling panes take over its area, and it comes
  back automatically as soon as a track with lyrics is playing (no view switch
  or terminal resize is needed). The stored layout, the colour settings and the
  user layout file are not modified by this;
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
| `tree` | v2 yes | one split/leaf tree with 1–6 unique pane leaves |
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
colours (white / cyan / yellow / green / magenta / red). Every dark-table value
is also one of the colours the Settings palette offers, so the name shown there
("bright white", "teal", …) is meaningful. The light-variant indices have their
own Settings names too (Charcoal, Deep Teal, Dark Amber, Dark Green, Dark
Magenta, Deep Blue, Dark Pink) even though they are not palette entries. On a
monochrome terminal pane colours are not enabled at all — that is the third
fallback level.

Because the presets colour every pane they contain, Settings shows each of those
rows as `[layout]` while a preset is selected (a pane the preset does not contain
reads `[theme] …`, `no pane` when it is absent); pick `Follow theme` on a row to
hand that field back to the global theme.

### Light/dark variants

All seven built-in presets have a compile-time **light variant** in
`ui/layout_spec.c` (`layout_preset_light_colour`). It is applied at render time
only when all of these hold:

- `preferences.appearance_mode` is `0` (auto) and the terminal background was
  detected as light, or the mode is explicitly `1` (light);
- the current layout comes from the built-in file (or the compiled-in classic
  fallback) — `layout_id_origin() == BUILTIN`;
- the pane has an explicit foreground for that field.

User layouts (`~/.config/ter-music/layouts.json`) that use the **same id as a
built-in preset** participate in a narrowly-scoped value match: only a pane's
explicit foreground (never background or border) is replaced by the preset's
light value when it still equals that preset's shipped dark value
(`layout_preset_dark_colour`). A value the user changed is kept verbatim, and
`TER_MUSIC_LAYOUTS` (origin ENV) always stays verbatim for deterministic
development tests. The user's `layouts.json` is never rewritten by the
adaptation. Saving a preset from the editor first materialises the colours
currently on screen into the copy and saves it as a fixed user layout: saving
under a dark terminal writes the shipped dark values (so the copy still matches
the value rule later), while saving under a light terminal writes the light
values (a fixed light copy that no longer adapts).

Light values use only the ANSI black / red / green / blue / magenta / cyan
buckets (0, 1, 2, 4, 5, 6 — never bright white 7 or yellow 3), so 256-colour,
16-colour and 8-colour terminals quantize to the same distinct colours within a
preset. The Settings colour page names these indices instead of showing
"unknown", and an auto row displays `Auto (light/dark)`; opening its palette
puts the cursor on the stored factory value (for example `White (7)`), not on
the Custom slot. Every light value has at least 4.5:1 contrast against a pure white
background. Backgrounds stay `-1` (transparent) and borders keep inheriting the
pane foreground, so the "no two panes share a colour" guarantee and the
`COLOR_PAIRS`-tight fallback to theme pairs are unchanged. Monochrome terminals
still disable pane colours entirely.

The dark palette is byte-for-byte identical to earlier releases: on a dark
terminal (or an unsupporting terminal, where the fallback is dark) nothing
changes.

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
| &#124; | split side by side; then choose the new pane type |
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

`Enter` opens (centred): split side by side, split stacked, change pane type,
edit colours, restore 50/50, swap with next pane, remove pane, rename layout,
save layout, editor help, close editor, and the four ±10 ratio adjustments.
`↑`/`↓` select, `Enter` runs, `Esc` returns; the menu scrolls when the terminal
is too short for every entry.

A **right click** anywhere in the editor opens the same action menu anchored at
the pointer instead of a second menu system. It is clamped to the screen exactly
like the other context menus (flips up/left at the edges, never covers the bottom
hint row), so every destructive editor operation stays reachable by mouse. While
a menu slide animation is running, mouse hit-testing follows the menu's current
visible rectangle (not its final position), and the animation degrades or stops
on slow terminals so input does not wait for an animation to finish.

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
  per-mille) for loading, parsing and validation; that schema rule is unchanged.
  In addition, the editor checks every edit against the current terminal's
  minimum readable size — about 12×3 content cells, 12×2 for lyrics, 8×2 for
  spectrum and 8×3 for vinyl — and snaps to the nearest readable ratio (leaving the
  current layout untouched when no readable result exists) with a status
  message instead of writing an unreadable layout. Ratio nudges are
  shortened to the largest readable step. Older layouts below the editor minimum
  still load and render unchanged; the editor only logs a debug line for them.
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

The info pane rearranges itself from its own size. Wide panes put the metadata
block and the audio-parameter block side by side with the colored half-block
cover on the right; their widths follow the actual text width (20 columns
minimum each, and a block with no rows hands its width to the other), while
narrow-and-tall panes stack the cover above the metadata and audio blocks. Its
rows are driven by Settings -> Info Display: the metadata block uses the enabled
state/mode/title/artist/album-or-queue fields, and the audio block appears only
while `format` is enabled (all four audio parameters belong to that bit).
Unsupported `index`/`path`/`volume`/`speed` bits are CLI/D-Bus only. Long
values are elided with `…` (text at the end, the path field in the middle), a
wide but short pane flows a block's rows into up to three sub-columns, and when
rows are scarce the layout keeps lines by importance — state, title, artist,
mode, then album/queue; sample rate, bit depth, bitrate, then codec — while the
display order stays unchanged. The content block is **vertically centred** in
the pane (cover and text move together in stacked mode), so a tall info pane no
longer pins the text to the title row. The cover keeps its 2:1 ratio and shrinks
to as few as 3 rows (12 at most); it is hidden when it cannot keep a blank column
before the right border or when the cover/text priority asks for text
(`preferences.info_cover_priority`: 0 auto, 1 text-first, 2 cover-first), and
every block is clipped to the region chosen by the layout. The
TUI cover is drawn with colored half-block characters so its glyph advance matches the
terminal cell grid, and is colored with an automatic 256-color → 8-color →
monochrome fallback (a tight color-pair budget can reduce the palette further).
Color mode composes both halves with `▀` (foreground = upper half, background =
lower half); `█` is used only by the monochrome fallback. The CLI/D-Bus text
cover still honours the configured braille/ASCII/half-block charset. The info-
pane cover is requested only when both `info_show_cover` and the playback
`show_album_cover` master switch are on; the controls-pane cover follows the
master switch. Both covers are bounded by the same
content rectangle, so no cover paints on a pane border. This keeps small or
unusually proportioned layouts visually intact.

## Colour precedence

Terminal light/dark variant (render-time only; never written to a file) →
per-pane explicit colours (tree leaves / user file) → theme colours from
Settings (`playlist`/`controls`/`lyrics` fg+bg).

The variant layer substitutes a colour only when the stored value is the
factory default (and the layout comes from the built-in file); anything you
changed stays exactly as stored. To hand a pane back to the
theme, use the layout editor (`c` → `r`/`R`) or the Settings colour page
(choose `Follow theme` in the palette).

The Konami easter egg ("aurora flow") is **not** a colour layer and does not
take part in this precedence. While it is active it tints only the foreground
of ordinary cells with a slow diagonal gradient; every rainbow pair uses a
transparent background (black when the terminal has no default-colour
support), so text stays readable and no gradient wash is painted behind it.
Colours exclude pure black, pure white and **neutral** gray on the black↔white
axis, but keep colour-tinted dark/light grays from colour→black and
colour→white transitions so adjacent steps stay continuous. In 256-colour mode
this is the xterm 6×6×6 cube; terminals with at least 8 but fewer than 256
colours (including 16/88-colour terminals) use only ANSI 1..6. Monochrome
terminals are left untouched and do not activate the effect.

Ordinary cells take the flow colour as foreground on a transparent background.
Cells carrying `A_REVERSE`/`A_STANDOUT` (the playlist cursor row, menu/selection
rows, popup selection) are not skipped: the flow colour moves to the background
and the text uses a black/white contrast colour — the normal foreground and
background are swapped, so the highlight bar itself flows. The reverse
attribute is kept across repaints (standout is normalised to reverse), so a
second rainbow pass cannot reinterpret the highlight as an ordinary
foreground-only cell; this keeps the highlight stable instead of flickering
between the two colour roles. Cover-art and
spectrum cells keep their own colours; the vinyl disc is recoloured except for
its centre album label and spectrum bars, preserving the disc's three shading
levels and rotating highlight. The effect never rewrites `config.xml` or
`layouts.json`; stored colours come back as soon as it is switched off. Modal
text prompts block the event loop, so the animation pauses there and resumes
when they close.

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

The same page starts with two terminal-appearance rows:

| Row | Effect |
| --- | --- |
| `Default Appearance Mode` | `Auto (follow terminal)`, `Light` or `Dark`; stored as `preferences.appearance_mode` (0/1/2) in `config.xml` (contract v10). `←`/`→` cycles, `ENTER` opens the picker. This is the global default. |
| `Re-detect Terminal Background` | Runs the full detection chain (terminal theme adapter; XDG portal up to 250 ms when the adapter is unknown; OSC 11 up to 250 ms; `COLORFGBG`; a `TERM` hint) and reports the light/dark/unknown conclusion **and its source** in one status message. The row itself keeps a live `background: light (terminal reply)` suffix; the source is one of terminal theme / terminal reply / system appearance / COLORFGBG / TERM hint / fallback. Use it when the terminal does not report focus. The probe that runs once at startup is cheaper and does not wait as long (OSC 11 up to 200 ms, portal up to 120 ms). |

After the colour rows, the same page has one more appearance row:

| Row | Effect |
| --- | --- |
| `This Terminal's Appearance` | Per-terminal override: `Inherit (Auto)`, `Auto`, `Light` or `Dark`; `Inherit` removes the entry. Stored in the front-end-local `~/.config/ter-music/appearance-terminals.json` (version 1, atomic write), keyed by a stable terminal identity (TERM_PROGRAM, then an allowlisted terminal in the ancestor process chain, then TMUX/STY/SSH/TERM/COLORTERM fallbacks — see below). It never writes `config.xml`; the override wins over the global default. |

In `Auto` the detection order is fixed: OSC 11 (the terminal's actual
background, authoritative) → the terminal theme adapter (currently only
deepin-terminal; see below) → the XDG desktop portal's
`org.freedesktop.appearance color-scheme` (1 dark / 2 light / 0 no preference;
`SettingChanged` is followed live without blocking the TUI) → `COLORFGBG` → a
`TERM` hint → the legacy dark look. A started portal connection is private to
this front end and is shut down with it; no bus, no portal, timeouts and
`HAVE_DBUS`-disabled builds fall back to the previous behaviour. Once OSC 11 has
answered, an XDG change does not override it: a single debounced, non-blocking
OSC 11 re-probe decides instead.

**Terminal theme adapter (deepin-terminal).** When the front end identifies the
current terminal as deepin-terminal (`$TERM_PROGRAM=deepin-terminal`; only
when `TERM_PROGRAM` is unset, a deepin-terminal entry in the ancestor process
chain), it reads the terminal's
own theme read-only: `$XDG_CONFIG_HOME/deepin/deepin-terminal/config.conf`
(`[basic.interface.theme] value=Light|Dark`,
`[basic.interface.expand_theme] value`), the matching
`terminalwidget6/color-schemes/<name>.colorscheme` under
`$XDG_DATA_HOME`/`$XDG_DATA_DIRS`/`/usr/share`, or
`customTheme.colorscheme` in the config directory when `expand_theme` is
`customTheme`. The scheme's `[Background] Color=R, G, B` is classified with the
same WCAG rule; missing/corrupt files mean "unknown" and the original chain
continues. The result sits **above** XDG (the desktop preference) and below
OSC 11. While `Auto` is active the adapter stats the config/theme files at most
once every 2 s (mtime + size, reading only when they change), so switching
deepin-terminal's own theme follows within a couple of seconds without a
restart, focus event or portal signal. Other terminals are never probed.

**Terminal identity degradation.** `TERM_PROGRAM` is preferred when set. If it
is not (deepin-terminal does not set it), the front end looks for an allowlisted
terminal emulator in its ancestor process chain and takes the outermost match
(`anc:deepin-terminal`). Without a usable `/proc` the key falls back to
`tmux:<TERM>`, `screen:<TERM>`, `ssh:<TERM>`, `term:<TERM>` or
`ct:<COLORTERM>`, and finally `default` (no entry is written). SSH clients that
report the same `TERM` therefore share one override, and a tmux server started
outside the pane's terminal lineage falls back to `tmux:<TERM>`; the Settings
hint line shows the exact key in use.

While a row's stored value is the factory default, its value is shown as
`Auto (light/dark)` and follows the detected variant; picking any other colour
in the palette fixes it (`Auto (light/dark)` disappears). This applies to the
adaptive theme slots only (playlist/controls/lyrics/sidebar/highlight/border
foregrounds and `highlight_bg`); backgrounds are transparent and do not adapt.

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

The section has **25 list rows** (2 appearance + 12 pane colours + 6 interface
colours + 1 per-terminal override + 4 layout actions), a two-line explanation
header and **five pinned hint lines**. The list viewport is `LINES - 12` rows,
so with 25 rows a terminal needs at least 37 lines to avoid scrolling
(terminals shorter than 37 rows scroll with the selection and draw a scrollbar
in the reserved column); a 24-line terminal shows 12 option rows, and the five
hint lines stay at the bottom.

## Remote Devices page

`F2` → **Remote Devices** (`SETTINGS_PAGE_REMOTE`, the sixth Settings sidebar
item) is the front-end remote-source page. It is not a layout pane: it uses the
settings content area and keeps its own four-mode state. Selecting the page from
the sidebar always resets it to the connection list, so a browser session does
not leak into the next visit.

### Connection list

Rows, in this order:

- one row per saved connection, `name [PROTOCOL(+TLS)] host` (at most 20);
- `↓ Add New Connection`;
- the cache block: cache title, `Used N MB / limit: M MB` (or `unlimited`), a
  prefetch-unavailable note, `Clean cache (…)`, `Cache limit (MB, 0=unlimited)`.

Keys: `↑`/`↓` select, ENTER opens the connection menu, starts the add form,
asks before cleaning the cache, or opens the cache-limit prompt; `←`/ESC leaves
the page. Deleting a connection and cleaning the cache need a second ENTER
(ESC cancels); the confirmation text is drawn under the list.

### Connection action menu

Rows: Browse Folder, Load to Playlist, Refresh Connection, Edit Connection,
Delete Connection, plus dynamic rows when relevant: Cancel current job (a
remote job runs), Retry listing (the last listing failed), Retry failed tracks
(n) (the last session had failures for this connection).

Keys: `↑`/`↓` select, ENTER runs the row, `←`/ESC returns to the connection
list. The delete confirmation replaces the rows: ENTER deletes, ESC keeps the
connection.

### Add / edit form

Fields appear by protocol, in this order: Name, Protocol, Host, Port, Username,
Password; for HTTP/WebDAV/FTP a TLS field and — when TLS is on — a "trust
self-signed certificate" field; for SFTP private key, key passphrase and
known_hosts path; then Base path (labelled "SMB share/subpath" for SMB).

Keys: `↑`/`↓` select a field, ENTER edits a text field (on Protocol it
advances the enum), `+`/`-` cycle the protocol or toggle TLS / the self-signed
switch, `S` saves, ESC cancels. An invalid port (outside 1–65535) or an empty base path is rejected
with a status message and is not saved.

### Folder browser

The first row is always `↓ Load into Playlist` (loads the current folder);
folder entries are prefixed `[dir]`. Keys: `↑`/`↓` select, ENTER enters a
selected directory or loads the folder, `→` also enters the selected directory,
ESC goes up one level (at the server root it returns to the action menu). The
selection follows the scroll window, so it is never scrolled out of sight.

### Status line

A status line under the list reports the job: `Listing remote directory…`,
`Downloading done/total, failed n`, the localized failure reason followed by
`Retry listing`, `Download complete done/total, failed n`, `Cancelled`, or the
truncation notice `only done of total tracks loaded`. A finished or failed
session keeps its summary for a while. The same progress is mirrored in the
main view's status bar, so leaving the settings page does not hide it. Errors
are localized into authentication, connection, not-found, timeout, TLS,
protocol, missing smbclient, unsafe SMB path, SFTP known_hosts, too large, no
audio, cache limit, parse and cancelled categories; unmatched errors keep the
English detail as a suffix.

### Mouse

A left click selects and runs a row (same as ENTER); the wheel moves the
selection; a right click selects the row without running it and opens the
context menu (Activate Remote Item / Increase Remote Value / Decrease Remote
Value / Save Remote Config, the last one enabled only inside the form).
Rendering records which selection value each drawn row maps to, so clicks in
the scrolled folder browser hit the visible row.

Remote playback and cache behaviour are documented in the README's
[Remote music sources](README.md#remote-music-sources).

## Troubleshooting

- **My old layout has a different shape after upgrading**: it contained a gap,
  overlap, out-of-bounds rect, or a non-guillotine arrangement and was
  auto-fitted. The reload message reports the count; open the editor to see the
  notice and adjust the split tree. It is no longer removed from the list.
- **The editor refuses a split / ratio change**: the result would make a pane
  smaller than the minimum readable size for the current terminal (about 12×3
  content cells, 8–12 columns depending on pane type). The editor snaps to the
  nearest readable ratio and the status line says so; if no readable result
  exists it leaves the layout unchanged. Pressing `{` / `}` on a pane whose
  ancestors are all horizontal splits reports that there is no split on that
  axis — the pane genuinely has no vertical neighbour to resize.
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
  with lyrics and the pane comes back automatically (no view switch or terminal
  resize is needed); the layout itself is untouched.
- **I selected a layout but the list is still the old one**: press
  `Reload Layouts` in Settings.
- **No layout files are installed**: the compiled-in `classic` tree is used and
  the TUI still works.
