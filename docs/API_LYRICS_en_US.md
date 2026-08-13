# Ter-Music Lyrics API (English)

## Overview

Ter-Music exposes the currently playing lyrics over the session D-Bus so other
applications can read the active line and the next line without polling the
terminal UI.

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
| Interface | `org.yxzl.ter_music.Lyrics` |

### Methods

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetLyrics` | `() -> s` | Returns the current lyrics snapshot as a JSON string. |

### Signals

| Signal | Signature | Description |
| ------ | --------- | ----------- |
| `LyricsChanged` | `(s)` | Broadcast whenever the lyrics snapshot changes. |

The payload of `LyricsChanged` is the same JSON object returned by
`GetLyrics`.

## JSON snapshot

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
  "track_id": "/org/mpris/MediaPlayer2/Track_xxxxxxxx",
  "has_lyrics": true,
  "has_timestamps": true,
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
- `revision`: monotonically increasing revision number; consumers can use it
  to discard stale or duplicate updates.

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
- When no lyrics are loaded, `active_line` and `track_id` are `null`, and both
  line objects contain only `null` fields.
- `LyricsChanged` is emitted only when the snapshot content changes; normal
  playback-position ticks do not produce signals.
- The JSON string is UTF-8. Quotes, backslashes and control characters are
  escaped; no newline is appended to the D-Bus string payload.

## Examples

Read the current snapshot:

```bash
gdbus call --session \
  --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Lyrics.GetLyrics
```

Monitor lyric updates:

```bash
gdbus monitor --session --dest org.mpris.MediaPlayer2.ter_music
```

## Compatibility

- Full D-Bus Introspection is not implemented in this version; clients use the
  fixed interface, method, and signal names documented above.
- The interface name uses underscores (`org.yxzl.ter_music.Lyrics`) because
  D-Bus interface name components cannot contain hyphens.
- The API follows the existing MPRIS lifecycle: it is available only while the
  player owns the MPRIS bus name and is shut down together with the media
  session.
