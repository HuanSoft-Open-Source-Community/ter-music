# Ter-Music D-Bus API (English)

## Overview

Ter-Music publishes one object path with several interfaces, so any D-Bus
client can read playback data, drive the player, browse and edit the queue and
library, and change the configuration:

| Interface | Purpose |
| --------- | ------- |
| `org.yxzl.ter_music.Info` | Read-only snapshots: track, progress, cover art, lyric lines, visualizer, last status message, rendered info block |
| `org.yxzl.ter_music.Control` | Transport, seek, volume, speed, play mode, opening paths, front-end registration, quitting |
| `org.yxzl.ter_music.Lyrics` | A/B lyric line snapshot (see [API_LYRICS_en_US.md](API_LYRICS_en_US.md)) |
| `org.yxzl.ter_music.Playlist` | Playlist loading, sorting, filtering, tree browsing (paged) |
| `org.yxzl.ter_music.Queue` | Play queue contents and editing |
| `org.yxzl.ter_music.Library` | Music library browsing and rescanning (paged) |
| `org.yxzl.ter_music.Favorites` | Favorite tracks |
| `org.yxzl.ter_music.History` | Play history |
| `org.yxzl.ter_music.DirHistory` | Recently opened directories |
| `org.yxzl.ter_music.Config` | Configuration read/write (the only writer) |

`org.freedesktop.DBus.Introspectable`, `.Peer` and `.Properties` are
implemented as well, and the MPRIS interfaces (`org.mpris.MediaPlayer2`,
`org.mpris.MediaPlayer2.Player`) remain available for desktop media
integration.

## Availability

| Item | Value |
| ---- | ----- |
| Bus name (primary) | `org.mpris.MediaPlayer2.ter_music` |
| Object path | `/org/mpris/MediaPlayer2` |
| Fallback bus name | `org.mpris.MediaPlayer2.ter_music.instance<pid>` (secondary instances only) |

The interfaces exist while the process owns the primary bus name, which is the
case for both the TUI and the background daemon. CLI commands are thin clients
of exactly these interfaces.

## Conventions

- **JSON payloads.** Every method that returns structured data returns a single
  JSON **string**. Keys may be added in later versions: ignore what you do not
  know.
- **Payload cap.** A single response never exceeds 256 KB
  (`core.payload_max`). A method that would exceed it fails with
  `org.yxzl.ter_music.Error.TooLarge` instead of truncating.
- **Paging.** Methods that can return many rows take `offset` and `count`.
  `count` defaults to 200 (`core.page_default`) and may not exceed 1000
  (`core.page_max`); asking for more returns
  `org.yxzl.ter_music.Error.InvalidArgs`. An `offset` at or past the end
  returns an empty page, not an error.
- **Errors.**

  | Name | Meaning |
  | ---- | ------- |
  | `org.yxzl.ter_music.Error.InvalidArgs` | Malformed argument, unknown key or enum value, page limit exceeded |
  | `org.yxzl.ter_music.Error.OutOfRange` | Index or position outside the current data |
  | `org.yxzl.ter_music.Error.TooLarge` | Response would exceed the payload cap |
  | `org.yxzl.ter_music.Error.Busy` | A background job is already running, or a table is full |
  | `org.yxzl.ter_music.Error.Unsupported` | Known method, unsupported request (e.g. an unknown config key) |
  | `org.yxzl.ter_music.Error.Failed` | The operation ran and failed (scan, network, database) |

  Protocol-level problems still use the standard
  `org.freedesktop.DBus.Error.*` names.

- **Handshake.** `Info.GetInfo` carries a `core` object. Read
  `core.api_version` before using anything else; version 3 is described here.
  Version 2 added the `core` object and the Playlist/Queue/Library/Config
  surface; version 3 removed `Remote` (remote music sources are a front-end
  feature) and restricted every path argument to local files. Version 1 was the
  pre-`core` surface (Info/Control/Lyrics only, `schema` 1).

  ```json
  "core": {
    "api_version": 3, "payload_max": 262144,
    "page_default": 200, "page_max": 1000,
    "methods": ["Info.GetInfo", "Control.Play", "Playlist.GetPage", "…"]
  }
  ```

  `core.methods` lists every method the running core implements, so a client
  can degrade gracefully instead of discovering gaps one call at a time.
- **Local paths only.** Every path argument (`Playlist.Load`, `Playlist.Append`,
  `Control.OpenPath`, MPRIS `OpenUri`) must be a local path or a `file://` URI.
  Remote sources (SMB/SFTP/FTP/WebDAV/HTTP) belong to the front end: it lists
  and downloads them itself, then hands the resulting local file paths to the
  core. A remote URL is rejected with
  `org.yxzl.ter_music.Error.Unsupported`.
- **Blocking work.** Directory scans run on a background worker. Those methods
  return immediately and report progress through their `Status` method plus a
  change signal; the media loop itself never blocks.

## org.yxzl.ter_music.Info

### Methods

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetInfo` | `() -> s` | Aggregated JSON snapshot (see below), including `core` and the rendered `text` block |
| `GetTrackInfo` | `() -> s` | The `track` object only |
| `GetProgress` | `() -> s` | The `playback` object only |
| `GetLyricsLines` | `() -> s` | The `lyrics` object only (current + next line) |
| `InstanceInfo` | `() -> s` | `{"mode":"daemon"\|"tui","pid":n,"version":"v…","bus":"…","has_primary_name":b}` |
| `GetCoverArt` | `(s charset, i cols, i rows) -> s` | Text cover art. `charset` is `braille`, `ascii` or `half`; empty string, `0` for the sizes and `-1`/`""` for the charset mean "use the configured values". Empty result when there is no cover |
| `GetDisplay` | `(s options) -> s` | The exact multi-line text printed by `ter-music show`, rendered with the caller's overrides |
| `GetVisualizer` | `() -> s` | `{"revision":n,"bands":64,"levels":[0-255…],"peaks":[0-255…]}` |
| `GetStatus` | `() -> s` | `{"seq":n,"message":"…"}` — the most recent status message and a monotonic sequence number |

### Signals

| Signal | Signature | Description |
| ------ | --------- | ----------- |
| `InfoChanged` | `(s json)` | Anything except the playback position changed (track, state, mode, volume, speed, cover availability). Payload equals `GetInfo` |
| `ProgressChanged` | `(x position_us, x duration_us, s status)` | At most once per second while playing, and immediately on state changes |
| `CoverChanged` | `(s text, s charset, i cols, i rows)` | Text cover changed (track, size or charset) |
| `VisualizerFrame` | `(u revision, ay levels, ay peaks)` | At most every 50 ms and only while new audio samples arrive; a paused or idle core sends nothing |

### `GetDisplay` options

`options` is `""` (use the configured display settings) or a `key=value` list
separated by `;`. Keys may be combined; later keys win:

| Key | Values | Description |
| --- | ------ | ----------- |
| `preset` | `full`, `compact`, `custom` | Display preset |
| `fields` | `state,mode,index,queue,title,artist,album,format,path,volume,speed`, `all`, `none` | Basic info fields |
| `cover` | `0`/`1` | Text cover on/off |
| `cover_cols`, `cover_rows` | 4-40, 2-20 | Cover size |
| `cover_charset` | `braille`, `ascii`, `half` | Cover charset |
| `progress` | `0`/`1` | Progress line on/off |
| `progress_style` | `bar`, `time`, `percent`, `time+percent` | Progress line style |
| `lyrics` | `0`, `1`, `2` | Lyric lines: off / current / current + next |
| `width` | 40-400 | Output width in terminal columns |
| `one_line` | `0`/`1` | Join everything into a single line |

### JSON snapshot

```json
{
  "schema": 1,
  "revision": 7,
  "running": true,
  "instance": { "mode": "daemon", "pid": 1234, "version": "v2.3.0",
                "bus": "org.mpris.MediaPlayer2.ter_music", "has_primary_name": true },
  "core": { "api_version": 3, "payload_max": 262144, "page_default": 200,
            "page_max": 1000, "methods": ["Info.GetInfo", "…"] },
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
    "cue_track_number": 0,
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

Field notes:

- `playback.state` is `playing`, `paused` or `stopped`.
- `playback.play_mode` is a stable machine-readable name (`sequential`,
  `single_repeat`, `list_repeat`, `shuffle_once`, `shuffle_repeat`,
  `folder_*`, `album_*`, `artist_*`). `play_mode_name` is localized,
  `play_mode_index` the numeric enum value.
- `track.uri` is always a `file://` URI: the core only plays local files, so
  even a track downloaded from a remote source appears under its cached local
  path. `track.number` is 1-based;
  `queue_position` is `null` when the track is not in the queue.
- `cover.text` holds newline-separated lines ready to print; `half` uses
  `▀`/`▄`/`█`, `ascii` uses `#`, `braille` uses braille patterns.
- `lyrics.current`/`lyrics.next` are computed from the playback position, so
  they stay correct even while the TUI shows another view.
- `revision` increases when the non-position part of the snapshot changes; use
  it to discard stale data. Missing values are `null`.

## org.yxzl.ter_music.Lyrics

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetLyrics` | `() -> s` | The A/B line snapshot: `{"active_line":"A"\|"B"\|null,"line_a":{…},"line_b":{…},"track_id":"…"\|null,"has_lyrics":b,"has_timestamps":b,"revision":n}`. See [API_LYRICS_en_US.md](API_LYRICS_en_US.md) |
| `GetDocument` | `(i offset, i count) -> s` | A page of the whole lyric document: `{"revision":n,"track_id":"…","has_lyrics":b,"has_timestamps":b,"source":"embedded\|external\|none","total":n,"offset":n,"current_index":n\|null,"lines":[{"index":n,"timestamp":t\|null,"text":"…"}]}` — used by scrolling and karaoke views |

Signal `LyricsChanged(s json)` carries the same payload as `GetLyrics` whenever
the active lines or the lyric source change.

## org.yxzl.ter_music.Control

Transport methods return a boolean (`true` when the request was accepted).

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `Play` | `() -> b` | Resume, or start the selected/first track |
| `Pause` | `() -> b` | Pause playback |
| `PlayPause` | `() -> b` | Toggle play/pause |
| `Stop` | `() -> b` | Stop playback |
| `Next` / `Previous` | `() -> b` | Next / previous track |
| `SeekTo` | `(x position_us) -> b` | Absolute seek |
| `SeekBy` | `(x delta_us) -> b` | Relative seek |
| `SetVolume` / `GetVolume` | `(i percent) -> b` / `() -> i` | Volume (0-100) |
| `SetSpeed` / `GetSpeed` | `(d rate) -> b` / `() -> d` | Playback speed (0.5-3.0) |
| `SetPlayMode` / `GetPlayMode` / `GetPlayModeName` | `(i mode) -> b` / `() -> i` / `() -> s` | Play mode (0-16, stable name, localized name) |
| `OpenPath` | `(s path, b autoplay) -> b` | Load a local directory, audio file or `file://` URI and optionally start playing |
| `PlayIndex` | `(i index) -> b` | Play a track by 0-based playlist index |
| `GetPlaylist` | `() -> s` | `{"loaded":b,"count":n,"current_index":i,"folder":"…"}` |
| `ReloadConfig` | `() -> b` | Re-read `config.xml` (same as `SIGHUP`); `Config.Reload` is the newer equivalent |
| `Quit` | `() -> b` | Gracefully stop this instance (persists the playback session) |
| `Attach` | `(s role) -> s` | Register this front end. `role` is `tui`, `cli` or `app`. Returns `{"token":"…","role":"…","api_version":3,"ping_interval_ms":2000,"frontends":n}` |
| `Ping` | `(s token) -> b` | Keep the registration alive. `false` means the token is unknown or expired: attach again |
| `Detach` | `(s token) -> b` | Leave explicitly |
| `FrontendInfo` | `() -> s` | `{"frontends":[{"token":"…","role":"tui","pid":n,"last_ping_ms":n}],"count":n}` |

Signals:

| Signal | Signature | Description |
| ------ | --------- | ----------- |
| `StatusMessage` | `(u seq, s message)` | A new core status message (same `seq` as `Info.GetStatus`) |
| `Error` | `(s source, s name, s message)` | An asynchronous operation failed: `source` names the method or job, `name` is one of the error names above |

Front ends attach once and then ping every 2 s; a registration without a
heartbeat for 6 s is dropped, so `FrontendInfo` always reflects who is
actually watching. A token identifies a registration, it does not
authenticate: the session bus is a same-user trust domain.

## org.yxzl.ter_music.Playlist

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `Load` | `(s path, b append, b autoplay) -> b` | Start loading a local directory, file or `file://` URI in the background |
| `Append` | `(s path) -> b` | Same, appending to the current playlist |
| `Clear` | `() -> b` | Empty the playlist |
| `Sort` | `(s mode) -> b` | `default`, `title`, `artist`, `album` or `filename`; persisted and applied immediately |
| `SetFilter` | `(s query) -> b` | Restrict `GetPage` to matching tracks; empty string clears it |
| `Search` | `(s query, i offset, i count) -> s` | A page of search results without changing the current filter |
| `GetTree` | `() -> s` | `{"loaded":b,"count":n,"visible_count":n,"tree_mode":b,"folder":"…","sort":"…","filter":"…"\|null,"loading":b}` |
| `GetPage` | `(i offset, i count) -> s` | A page of visible rows (see below) |
| `ToggleExpand` | `(i tree_index) -> b` | Expand/collapse a directory node (shared core-side state) |
| `RevealIndex` | `(i track_index) -> i` | Expand the ancestors of a track and return its visible row |
| `Status` | `() -> s` | `{"state":"idle"\|"loading"\|"error","progress":n,"total":n,"path":"…","error":"…"}` |

`GetPage` returns render-ready rows, so a client draws a list without looking
up metadata per row:

```json
{ "total": 6, "offset": 0, "count": 3, "filter": null,
  "rows": [
    { "row": 0, "type": "dir", "depth": 0, "expanded": true,
      "tree_index": 0, "track_index": null, "name": "Music",
      "title": null, "artist": null, "album": null, "is_cue": false },
    { "row": 2, "type": "track", "depth": 2, "expanded": false,
      "tree_index": 2, "track_index": 0, "name": "song.flac",
      "title": "Song", "artist": "Artist", "album": "Album", "is_cue": false }
  ] }
```

While a filter is set, rows are a flat list of matching tracks (the tree is
not applied), which matches the search view in the TUI.

Signal `PlaylistChanged(s reason)` with `reason` one of `loading`, `loaded`,
`sorted`, `filtered`, `expanded`, `search`.

## org.yxzl.ter_music.Queue

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `Get` | `(i offset, i count) -> s` | `{"revision":n,"count":n,"current_position":n\|null,"offset":n,"rows":[{"position":n,"track_index":n,"title":"…","artist":"…","album":"…","is_cue":b}]}` |
| `Append` / `InsertAfter` | `(i track_index) -> b` | Add a track at the end / after the current one |
| `RemoveAt` | `(i position) -> b` | Remove one entry |
| `MoveUp` / `MoveDown` | `(i position) -> b` | Reorder one entry |
| `Clear` | `() -> b` | Empty the queue |
| `Rebuild` | `(i mode) -> b` | Rebuild from the playlist using a play mode (optional; current mode by default) |
| `Shuffle` | `() -> b` | Keep the current track first and shuffle the rest |
| `PlayAt` | `(i position) -> b` | Play the entry at a queue position |

Signal `QueueChanged(u revision, i count, i current_position)` after every
edit. Queue edits are synchronous: the queue is bounded by the playlist size.

## org.yxzl.ter_music.Library

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `Rescan` | `(s path) -> b` | Scan a directory in the background (incremental by mtime). An empty path is rejected: scanning every registered root is a synchronous in-process call and is not exposed |
| `Status` | `() -> s` | `{"available":b,"tracks":n,"scanning":b,"progress":n,"total":n}` |
| `GetTree` | `(s kind, s filter) -> s` | `{"kind":"…","item_count":n,"available":b,"filter":{…}}` |
| `GetPage` | `(s kind, s filter, i offset, i count) -> s` | A page of rows |
| `Search` | `(s filter) -> s` | `{"item_count":n}` — the number of matches for `{"query":"…"}` |

`kind` is `artists`, `albums`, `genres`, `tracks` or `search`. `filter` is a
JSON object (empty string means none):

```json
{ "artist": "…", "album": "…", "genre": "…", "query": "…" }
```

Rows: aggregate views return `{"name":"…","artist":"…","album":"…","track_count":n,"rowid":null,"path":null}`,
track and search views return `{"name":"<title>","artist":"…","album":"…","track_count":0,"rowid":n,"path":"…"}`.

Signal `LibraryChanged(s reason)` with `reason` one of `scan_started`,
`scan_progress`, `scan_done`, `favorites`, `history`, `dir_history`, `updated`.

## org.yxzl.ter_music.Favorites / .History / .DirHistory

| Interface | Method | Signature | Description |
| --------- | ------ | --------- | ----------- |
| `Favorites` | `Add` / `Remove` | `(s path) -> b` | Add/remove a favorite by track path |
| `Favorites` | `Has` | `(s path) -> b` | Is this path a favorite |
| `Favorites` | `List` | `(i offset, i count) -> s` | `{"total":n,"offset":n,"rows":[{"path":"…","title":"…","artist":"…","album":"…"}]}` |
| `History` | `Add` | `(s path, i position) -> b` | Record a play |
| `History` | `List` | `(i offset, i count) -> s` | Rows `{"path":"…","title":"…","artist":"…","play_time":n}` |
| `History` | `Clear` | `() -> b` | Remove all entries |
| `DirHistory` | `Add` / `Remove` | `(s path) -> b` | Add/remove a directory |
| `DirHistory` | `List` | `(i offset, i count) -> s` | Rows `{"path":"…","open_time":n}` |
| `DirHistory` | `Clear` | `() -> b` | Remove all entries |

All three broadcast `Library.LibraryChanged` with their own reason.

## org.yxzl.ter_music.Config

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetAll` | `() -> s` | The complete configuration (see below) |
| `Set` | `(s patch) -> b` | Apply a partial patch; unknown keys or wrong types fail the whole patch with `Error.Unsupported` and nothing changes |
| `Reload` | `() -> b` | Re-read `config.xml` (same as `SIGHUP`) |
| `Reset` | `() -> b` | Restore defaults, save and broadcast |

`GetAll` mirrors `config.xml`, using the same section and key names:

```json
{ "version": 5,
  "paths": { "default_startup_path": "…", "last_opened_path": "…" },
  "theme": { "playlist_fg": 7, "playlist_bg": -1, "…": 0 },
  "preferences": { "volume_percent": 100, "default_playback_speed": 1.00,
                   "info_preset": 0, "info_fields": 2047, "…": 0 },
  "equalizer": { "enabled": 0, "preamp": 0, "bands": [0,0,0,0,0,0,0,0,0,0] },
  "remote_connections": [
    { "index": 0, "name": "nas", "protocol": "sftp", "host": "…", "port": 22,
      "username": "…", "base_path": "/music", "private_key_path": "",
      "password_set": true, "password_encrypted": "1f3a…" }
  ] }
```

Notes:

- Numeric values outside their range are clamped, not rejected
  (`volume_percent: 999` becomes `100`).
- **Passwords never cross the bus in plaintext.** `GetAll` returns
  `password_set` plus the stored ciphertext (`password_encrypted`). `Set`
  accepts either `password` (plaintext; the core encrypts it on save) or
  `password_encrypted` (the value `GetAll` gave you; decrypted back so saving
  never double-encrypts). Omitting both keeps the stored password.
- `Set` applying to play mode, speed or volume is visible immediately, because
  the core re-applies runtime values after saving.

Signal `ConfigChanged(s patch)` carries the patch that was applied; `{}` means
"everything may have changed" (used by `Reset`).

## Removed: org.yxzl.ter_music.Remote

Removed in `core.api_version` 3. Remote music sources (SMB/SFTP/FTP/WebDAV/
HTTP) are a **front-end** feature: the front end stores its own server list
(outside the core configuration), lists and downloads remote directories, and
then hands the resulting local file paths to `Playlist.Load` / `Playlist.Append`.
The core never sees a remote URL, so a stale client calling `Remote.*` gets the
standard `org.freedesktop.DBus.Error.UnknownMethod`.

## Examples

```bash
# Handshake and snapshot
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Info.GetInfo

# A page of the playlist tree, then the queue
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Playlist.GetPage 0 50
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Queue.Get 0 50

# Change a setting (unknown keys fail without changing anything)
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Config.Set '{"preferences":{"volume_percent":40}}'

# Rescan a library root, then page the artists
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Library.Rescan "/home/user/Music"
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Library.GetPage artists "" 0 50

# Watch everything that changes
gdbus monitor --session --dest org.mpris.MediaPlayer2.ter_music

# Introspect the full surface
busctl --user introspect org.mpris.MediaPlayer2.ter_music /org/mpris/MediaPlayer2
```

`scripts/test/rpc_client.py` is a small Python client for cases the shell tools
cannot express (negative integer arguments, one connection across calls):

```bash
scripts/test/rpc_client.py attach tui 5                 # attach, ping, detach
scripts/test/rpc_client.py call org.yxzl.ter_music.Queue.Get 0 20
scripts/test/rpc_client.py monitor 3                    # list signals seen
```

## Compatibility

- Interface name components cannot contain hyphens, hence the underscores in
  `org.yxzl.ter_music.*`.
- `Info` JSON `schema` is `1`; the interface surface is versioned separately by
  `core.api_version` (currently `3`). Consumers must ignore unknown fields.
  Version 3 removed `org.yxzl.ter_music.Remote` and the `track.is_remote` field
  and made every path argument local-only; version 2 added the `core` object and
  the Playlist/Queue/Library/Favorites/History/DirHistory/Config interfaces.
- The API shares the MPRIS lifecycle: it exists while the process owns the bus
  name and is torn down on shutdown.
- Inside a Linyaps (如意玲珑) package the container joins the host session bus,
  so host applications see the same object path, interfaces and JSON schemas;
  the package ships a D-Bus activation file for
  `org.mpris.MediaPlayer2.ter_music` that starts the background player on
  demand.
