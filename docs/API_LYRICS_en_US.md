# Ter-Music Lyrics API (English)

## Overview

Ter-Music exposes the currently playing lyrics over the session D-Bus so other
applications can read the active line, the next line and per-character progress
without polling the terminal UI.

The API is available whenever the MPRIS D-Bus integration is active:

- `libdbus-1` was detected at build time;
- the application successfully connected to the session bus;
- the MPRIS bus name was acquired.

There is no separate configuration switch for the lyrics API.

## D-Bus interface

| Item | Value |
| ---- | ----- |
| Bus name | `org.mpris.MediaPlayer2.ter_music` |
| Object path | `/org/mpris/MediaPlayer2` |
| Interface | `cn.org.huansoft.ter_music.Lyrics` |
| API version | `core.api_version = 7` (this document); the lyrics surface itself has not changed since version 5 — version 6 added the karaoke methods on `Control`, version 7 moved the interface namespace to `cn.org.huansoft.ter_music.*` — and that namespace move, unlike everything before it, is breaking for every client, lyric consumers included |

### Methods

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetLyrics` | `() -> s` | The A/B line snapshot plus the current per-character progress, as JSON. |
| `GetDocument` | `(i offset, i count) -> s` | A page of the whole lyric document with per-line timing. |
| `SetSource` | `(i source) -> b` | Switch the source preference and reload the current track: `0` = auto, `1` = embedded, `2` = external. `false` when there is no current track. |

### Signals

| Signal | Signature | Description |
| ------ | --------- | ----------- |
| `LyricsChanged` | `(s)` | Broadcast whenever the snapshot changes: active line, source, or per-character progress. |

The payload of `LyricsChanged` is the same JSON object returned by
`GetLyrics`. Karaoke playback can emit it at each character boundary (bounded
by the core tick, roughly 20–50 Hz); clients may throttle or redraw on every
`revision` change.

## JSON snapshot (`GetLyrics`)

```json
{
  "active_line": "A",
  "line_a": {
    "index": 3,
    "timestamp": 12.34,
    "text": "Current line"
  },
  "line_b": {
    "index": 4,
    "timestamp": 15.67,
    "text": "Next line"
  },
  "track_id": "/org/mpris/MediaPlayer2/Track_0f1e2d3c4b5a6978",
  "has_lyrics": true,
  "has_timestamps": true,
  "char_sung": 4,
  "char_total": 10,
  "revision": 7
}
```

### Fields

- `active_line`: `"A"`, `"B"`, or `null` when there is no active lyric line.
- `line_a` / `line_b`: objects describing the A and B slots.
  - `index`: line index in the loaded lyrics, or `null`.
  - `timestamp`: line start time in seconds, or `null` for lyrics without
    timestamps.
  - `text`: lyric text, or `null`.
- `track_id`: the same track id published through MPRIS
  (`mpris:trackid`), or `null` when no track is playing.
- `has_lyrics`: whether lyrics are loaded.
- `has_timestamps`: whether the lyrics contain LRC timestamps.
- `char_sung` / `char_total`: how many UTF-8 characters of the active line have
  been sung, and how many it contains. Both are `0` when the active line has no
  per-character timing (plain embedded lyrics, or a version 4 core).
- `revision`: monotonically increasing revision number; consumers can use it
  to discard stale or duplicate updates.

## Lyric document (`GetDocument`)

`GetDocument(offset, count)` returns a page of the whole loaded document. `count`
is clamped by the core's page limit; `offset` past the end returns an empty
`lines` array. A page that would exceed the 256 KB payload cap fails with
`cn.org.huansoft.ter_music.Error.TooLarge` instead of being truncated.

```json
{
  "revision": 7,
  "track_id": "/org/mpris/MediaPlayer2/Track_0f1e2d3c4b5a6978",
  "has_lyrics": true,
  "has_timestamps": true,
  "source": "external",
  "total": 64,
  "offset": 0,
  "current_index": 5,
  "lines": [
    {
      "index": 0,
      "timestamp": 0.000,
      "end": 6.970,
      "timing": "chars",
      "char_total": 13,
      "text": "梦回还 - 呦猫UNEKO",
      "chars": [0, 770, 1550, 2320, 3100, 3870, 4650, 5420, 6200, 6354, 6508, 6662, 6816]
    },
    {
      "index": 1,
      "timestamp": 16.350,
      "end": 20.010,
      "timing": "line",
      "char_total": 16,
      "text": "我经历一场革变 在沉睡的记忆里面"
    }
  ]
}
```

### Line fields

- `index`: zero-based line index in the document.
- `timestamp`: line start in seconds, or `null` for untimed lines.
- `end`: line end in seconds, or `null` when it cannot be derived.
- `timing`: one of:
  - `"chars"` — the LRC carried per-segment timestamps. `chars` contains one
    start offset per UTF-8 character, in **milliseconds relative to
    `timestamp`**. Characters inside a multi-character segment are distributed
    evenly across the segment's duration.
  - `"line"` — only the line start (and possibly an explicit end) is known.
    Spread the characters evenly across `[timestamp, end)`.
  - `"none"` — no usable timing (plain embedded lyrics, or a start without a
    derivable end).
- `char_total`: number of UTF-8 code points in `text` (0 for a placeholder
  line that has no lyric text). It is independent of `timing`; an untimed plain
  line still reports its character count here.
- `text`: lyric text.
- `chars`: only present for `timing == "chars"`; its length equals `char_total`.

A line with a start and an explicit end tag such as
`[00:16.35]text[00:20.01]` uses `"line"`. A line whose timestamps appear between
text segments such as `[00:00.00]梦[00:00.77]回[00:01.55]还[00:02.32]` uses
`"chars"`. A legacy line with only `[00:20.01]text` also uses `"line"`, with
`end` inferred from the next distinct line start (the last line reuses the
previous line's duration).

## A/B double-buffer semantics

The API keeps two lyric slots:

- while line A is playing, line B holds the next line;
- when playback advances to line B, the active slot becomes B and line A is
  refreshed with the next-next line;
- when playback advances to that line A, the active slot becomes A again and
  line B is refreshed, and so on.

Example with lines 0..4:

| Playing line | Active slot | Line A | Line B |
| ------------ | ----------- | ------ | ------ |
| 0 | A | 0 | 1 |
| 1 | B | 2 | 1 |
| 2 | A | 2 | 3 |
| 3 | B | 4 | 3 |
| 4 | A | 4 | null |

For lyrics without timestamps (plain embedded lyrics), the API exposes
line 0 as A and line 1 as B with `active_line: "A"`, and does not advance
automatically.

## Behavior notes

- A seek or track jump to a line that is not currently in either slot resets
  the double buffer: the target line becomes A, the following line becomes B,
  and `active_line` becomes `"A"`.
- A track change updates `track_id` and resets the double buffer.
- When no lyrics are loaded, `active_line` and `track_id` are `null`,
  `char_sung`/`char_total` are `0`, and both line objects contain only `null`
  fields.
- `LyricsChanged` is emitted only when the snapshot content changes; ordinary
  position ticks that stay inside one character do not produce signals.
- The JSON string is UTF-8. Quotes, backslashes and control characters are
  escaped; no newline is appended to the D-Bus string payload.

## Examples

Read the current snapshot:

```bash
gdbus call --session \
  --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method cn.org.huansoft.ter_music.Lyrics.GetLyrics
```

Read the first page of the document:

```bash
gdbus call --session \
  --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method cn.org.huansoft.ter_music.Lyrics.GetDocument 0 200
```

Monitor lyric updates:

```bash
gdbus monitor --session --dest org.mpris.MediaPlayer2.ter_music
```

## Rendering karaoke

1. Fetch the document once per track (cache it by `track_id`/`revision`).
2. For the active line, use `timing`/`end`/`chars` and the current playback
   position to compute the number of sung UTF-8 characters; or simply use
   `char_sung`/`char_total` from `GetLyrics`.
3. Render the first `char_sung` characters as the highlighted prefix and the
   rest as the unsung suffix.

## Compatibility

- The API is additive: `api_version 5` adds `char_sung`, `char_total`, and the
  per-line `end`/`timing`/`char_total`/`chars` fields. A version 4 client
  ignores them; a version 5 front end accepts a version 4 core and degrades to
  whole-line highlighting when the fields are absent. `api_version 6` left the
  lyrics fields exactly as version 5 defined them (it only adds
  `Control.SetKaraoke`/`Control.GetKaraoke`), so every rule above still held.
  `api_version 7` did not touch a single lyric field either, but it renamed the
  interface: `org.yxzl.ter_music.Lyrics` became
  `cn.org.huansoft.ter_music.Lyrics`, so a pre-7 client must switch its
  hard-coded interface name (or run the pre-7 core) — no fallback applies.
- The interface is part of the standard introspection tree, but clients may
  also use the fixed interface, method, and signal names documented above.
- The interface name uses underscores (`cn.org.huansoft.ter_music.Lyrics`) because
  D-Bus interface name components cannot contain hyphens.
- The API follows the existing MPRIS lifecycle: it is available only while the
  player owns the MPRIS bus name and is shut down together with the media
  session.
