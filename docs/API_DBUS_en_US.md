# Ter-Music D-Bus Info & Control API (English)

## Overview

Ter-Music publishes three interfaces on the same D-Bus object path so that other
applications can read playback data and drive the player:

| Interface | Purpose |
| --------- | ------- |
| `org.yxzl.ter_music.Info` | Read-only snapshots: track info, progress, text cover art, lyric lines, rendered info block |
| `org.yxzl.ter_music.Control` | Transport, seek, volume, speed, play mode, opening paths, quitting |
| `org.yxzl.ter_music.Lyrics` | A/B lyric line snapshot (see [API_LYRICS_en_US.md](API_LYRICS_en_US.md)) |

The standard `org.freedesktop.DBus.Introspectable`, `org.freedesktop.DBus.Peer`
and `org.freedesktop.DBus.Properties` interfaces are implemented as well, and
the MPRIS interfaces (`org.mpris.MediaPlayer2`, `org.mpris.MediaPlayer2.Player`)
remain available for desktop media integration.

## Availability

| Item | Value |
| ---- | ----- |
| Bus name (primary) | `org.mpris.MediaPlayer2.ter_music` |
| Object path | `/org/mpris/MediaPlayer2` |
| Fallback bus name | `org.mpris.MediaPlayer2.ter_music.instance<pid>` (secondary instances only) |

The interfaces are available whenever the process owns the primary bus name,
which is the case for both the TUI and the background daemon:

```bash
ter-music                    # TUI instance
ter-music daemon start       # headless background instance
ter-music play ~/Music       # starts a daemon automatically when none is running
```

CLI commands (`ter-music show`, `ter-music pause`, ...) are thin clients of
exactly these interfaces, so anything the CLI can do can be done by any other
D-Bus client.

## org.yxzl.ter_music.Info

### Methods

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetInfo` | `() -> s` | Aggregated JSON snapshot (see below), including the rendered `text` block |
| `GetTrackInfo` | `() -> s` | The `track` object only |
| `GetProgress` | `() -> s` | The `playback` object only |
| `GetLyricsLines` | `() -> s` | The `lyrics` object only (current + next line) |
| `GetCoverArt` | `(s charset, i cols, i rows) -> s` | Text cover art (`braille` or `ascii`); empty string when unavailable. `charset` may be `""`, and `cols`/`rows` may be `0`, to use the configured values |
| `GetDisplay` | `(s options) -> s` | The exact multi-line text printed by `ter-music show`, rendered with the caller's overrides |
| `InstanceInfo` | `() -> s` | `{"mode":"daemon"\|"tui","pid":n,"version":"v…","bus":"…","has_primary_name":b}` |

### Signals

| Signal | Signature | Description |
| ------ | --------- | ----------- |
| `InfoChanged` | `(s json)` | Emitted when anything except the playback position changes (track, state, mode, volume, speed, cover availability). The payload equals `GetInfo` |
| `ProgressChanged` | `(x position_us, x duration_us, s status)` | Emitted at most once per second while playing, and immediately on state changes |
| `CoverChanged` | `(s text, s charset, i cols, i rows)` | Emitted when the text cover changes (track change, size or charset change) |

### `GetDisplay` options

`options` is an empty string (`""`, use the configured display settings) or a
`key=value` list separated by `;`. Keys may be combined; later keys win:

| Key | Values | Description |
| --- | ------ | ----------- |
| `preset` | `full`, `compact`, `custom` | Display preset (overrides the stored preset) |
| `fields` | `state,mode,index,queue,title,artist,album,format,path,volume,speed`, `all`, `none` | Basic info fields |
| `cover` | `0`/`1` | Text cover on/off |
| `cover_cols`, `cover_rows` | 4-40, 2-20 | Cover size |
| `cover_charset` | `braille`, `ascii` | Cover charset |
| `progress` | `0`/`1` | Progress line on/off |
| `progress_style` | `bar`, `time`, `percent`, `time+percent` | Progress line style |
| `lyrics` | `0`, `1`, `2` | Lyric lines: off / current / current + next |
| `width` | 40-400 | Output width in terminal columns |
| `one_line` | `0`/`1` | Join everything into a single line |

Example:

```bash
gdbus call --session \
  --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Info.GetDisplay \
  "preset=compact;lyrics=2;width=100"
```

## JSON snapshot

```json
{
  "schema": 1,
  "revision": 7,
  "running": true,
  "instance": { "mode": "daemon", "pid": 1234, "version": "v2.2.0",
                "bus": "org.mpris.MediaPlayer2.ter_music", "has_primary_name": true },
  "playback": {
    "state": "playing", "position_ms": 83400, "duration_ms": 296000,
    "position": "01:23", "duration": "04:56", "remaining": "03:33",
    "percent": 28.2, "volume_percent": 100, "speed": 1.0,
    "play_mode": "list_repeat", "play_mode_index": 2, "play_mode_name": "List Repeat",
    "loop_status": "Playlist", "shuffle": false
  },
  "track": {
    "id": "/org/mpris/MediaPlayer2/Track_0f1e…", "index": 2, "number": 3,
    "playlist_count": 12, "queue_position": 3, "queue_count": 12,
    "title": "Example Song", "artist": "Example Artist", "album": "Example Album",
    "path": "/home/user/Music/example.flac", "uri": "file:///home/user/Music/example.flac",
    "is_remote": false, "cue_track_number": 0,
    "format": { "codec": "flac", "sample_rate": 96000, "bit_depth": 24,
                "bit_rate": 1234000, "rate_display": "96000Hz",
                "depth_display": "24bit", "bitrate_display": "1234kbps" }
  },
  "cover": { "available": true, "art_url": "file:///tmp/ter-music-cover-….jpg",
             "charset": "braille", "cols": 16, "rows": 8, "text": "⣿⣀…\n…" },
  "lyrics": {
    "has_lyrics": true, "has_timestamps": true, "source": "embedded",
    "current": { "index": 3, "timestamp": 12.34, "text": "Current line" },
    "next":    { "index": 4, "timestamp": 15.67, "text": "Next line" }
  },
  "display": { "preset": "full", "fields": ["state", "mode", "title"],
               "show_cover": true, "cover_cols": 16, "cover_rows": 8,
               "cover_charset": "braille", "show_progress": true,
               "progress_style": "bar", "lyrics_lines": 2,
               "width": 80, "one_line": false },
  "text": "State: Playing\n…"
}
```

### Field notes

- `playback.state` is `playing`, `paused` or `stopped`.
- `playback.play_mode` is a stable machine-readable name:
  `sequential`, `single_repeat`, `list_repeat`, `shuffle_once`,
  `shuffle_repeat`, `folder_sequential`, `folder_repeat`, `folder_shuffle`,
  `folder_shuffle_repeat`, `album_sequential`, `album_repeat`, `album_shuffle`,
  `album_shuffle_repeat`, `artist_sequential`, `artist_repeat`,
  `artist_shuffle`, `artist_shuffle_repeat`. `play_mode_name` is the localized
  name, `play_mode_index` the numeric enum value.
- `track.uri` is a `file://` URI for local tracks and the original URL for
  remote (SMB/SFTP/FTP/WebDAV/HTTP) tracks; `is_remote` distinguishes them.
- `track.number` is 1-based; `queue_position` is `null` when the track is not in
  the play queue.
- `cover.text` contains newline-separated lines of braille (or `#`/space ASCII)
  characters, so consumers can print it directly. `cover.art_url` points at the
  extracted cover image when available.
- `lyrics.current`/`lyrics.next` are computed from the playback position, so
  they stay correct even while the TUI shows another view.
- `revision` increases when the non-position part of the snapshot changes; use
  it to discard stale data.
- Missing values are `null` (the text rendering uses `--` placeholders instead).

## org.yxzl.ter_music.Control

All methods return a boolean (`true` when the request was accepted).

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `Play` | `() -> b` | Resume, or start the selected/first track |
| `Pause` | `() -> b` | Pause playback |
| `PlayPause` | `() -> b` | Toggle play/pause |
| `Stop` | `() -> b` | Stop playback |
| `Next` / `Previous` | `() -> b` | Next / previous track |
| `SeekTo` | `(x position_us) -> b` | Absolute seek |
| `SeekBy` | `(x delta_us) -> b` | Relative seek |
| `SetVolume` / `GetVolume` | `(i percent) -> b` / `() -> i` | Volume control (0-100) |
| `SetSpeed` / `GetSpeed` | `(d rate) -> b` / `() -> d` | Playback speed (0.5-3.0) |
| `SetPlayMode` / `GetPlayMode` / `GetPlayModeName` | `(i mode) -> b` / `() -> i` / `() -> s` | Play mode (0-16, stable name, localized name) |
| `OpenPath` | `(s path, b autoplay) -> b` | Load a directory, audio file, `file://` URI or remote URL and optionally start playing |
| `PlayIndex` | `(i index) -> b` | Play a track by its 0-based playlist index |
| `GetPlaylist` | `() -> s` | `{"loaded":b,"count":n,"current_index":i,"folder":"…"}` |
| `ReloadConfig` | `() -> b` | Re-read `config.xml` (same as `SIGHUP`) |
| `Quit` | `() -> b` | Gracefully stop this instance (persists the playback session) |

## MPRIS additions

- `Metadata` now also publishes `xesam:url` (file URI / remote URL) and
  `xesam:trackNumber`.
- `OpenUri(uri)` is implemented and behaves like `Control.OpenPath(uri, true)`.
- `CanQuit` remains `false` and MPRIS `Quit` is not supported on purpose:
  desktop media widgets must not be able to terminate the player. Use
  `Control.Quit` or `ter-music daemon stop` for that.

## Examples

```bash
# Introspect everything
busctl --user introspect org.mpris.MediaPlayer2.ter_music /org/mpris/MediaPlayer2

# Track info, progress and cover
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Info.GetTrackInfo

gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Info.GetProgress

gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Info.GetCoverArt "braille" 16 8

# Control
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Control.OpenPath "/home/user/Music" true

# Monitor track / progress changes
gdbus monitor --session --dest org.mpris.MediaPlayer2.ter_music
```

## Compatibility

- Interface name components cannot contain hyphens, hence the underscores in
  `org.yxzl.ter_music.*`.
- `schema` is `1`; new keys may be added in later versions, so consumers should
  ignore unknown fields.
- The API shares the MPRIS lifecycle: it exists while the process owns the bus
  name and is torn down on shutdown.
- Inside a Linyaps (如意玲珑) package the application container joins the host
  session bus, so the same object path, interfaces and JSON schema are visible to
  host applications; the package additionally ships a D-Bus activation file for
  `org.mpris.MediaPlayer2.ter_music` that starts the background player on demand
  (`ter-music daemon start` and `ter-music play` use it inside the sandbox).
