# Ter-Music D-Bus API (English)

## Overview

Ter-Music publishes one object path with several interfaces, so any D-Bus
client can read playback data, drive the player, browse and edit the play queue,
and change the configuration:

| Interface | Purpose |
| --------- | ------- |
| `org.yxzl.ter_music.Info` | Read-only snapshots: track, progress, cover art, lyric lines, visualizer, last status message, rendered info block |
| `org.yxzl.ter_music.Control` | Transport, seek, volume, speed, play mode, opening paths, front-end registration, quitting |
| `org.yxzl.ter_music.Lyrics` | A/B lyric line snapshot (see [API_LYRICS_en_US.md](API_LYRICS_en_US.md)) |
| `org.yxzl.ter_music.Queue` | Play queue contents and editing |
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

The interfaces exist while a process owns the primary bus name, and that process
is the **core**: a `daemon` instance, a `play --foreground` process, or a TUI
started with `--frontend=local`. The default TUI is a front end only — it does
not claim the name; it starts such a core (or attaches to a running one) and
talks to it. CLI subcommands are thin clients of exactly these interfaces.

## Conventions

- **JSON payloads.** Every method that returns structured data returns a single
  JSON **string**. Keys may be added in later versions: ignore what you do not
  know.
- **Payload cap.** A single response never exceeds 256 KB
  (`core.payload_max`). A method that would exceed it fails with
  `org.yxzl.ter_music.Error.TooLarge` instead of truncating.
- **Paging.** Methods that can return many rows take `offset` and `count`.
  `count` of `0` means "default page" (200 rows, `core.page_default`); the
  largest page is 1000 rows (`core.page_max`). Asking for more — or passing a
  negative `count` — returns `org.yxzl.ter_music.Error.InvalidArgs` instead of
  silently truncating. A negative `offset` is clamped to `0`, and an `offset` at
  or past the end returns an empty page, not an error. If the rendered page
  still does not fit the payload cap, the call fails with `Error.TooLarge`
  rather than returning half a JSON document.
- **Errors.**

  | Name | Meaning |
  | ---- | ------- |
  | `org.yxzl.ter_music.Error.InvalidArgs` | Malformed argument, unknown key or enum value, page limit exceeded |
  | `org.yxzl.ter_music.Error.OutOfRange` | Index or position outside the current data |
  | `org.yxzl.ter_music.Error.TooLarge` | Response would exceed the payload cap |
  | `org.yxzl.ter_music.Error.Busy` | A fixed-size table is full — today that means `Control.Attach` finding every front-end registration slot taken (the registry holds a fixed maximum; detach an unused front end first) |
  | `org.yxzl.ter_music.Error.Unsupported` | Known method, unsupported request (e.g. an unknown config key or section in a `Config.Set` patch) |
  | `org.yxzl.ter_music.Error.Failed` | The operation ran and failed inside the core (e.g. applying an MPRIS rate, rendering the configuration). The asynchronous `Control.Error` signal also defaults to this name |

  Protocol-level problems still use the standard
  `org.freedesktop.DBus.Error.*` names.

- **Handshake.** `Info.GetInfo` carries a `core` object. Read
  `core.api_version` before using anything else; version 4 is described here.
  Version 4 handed **all content** to the front end: the `Playlist`, `Library`,
  `Favorites`, `History` and `DirHistory` interfaces, the background scan job and
  `Control.OpenPath`/`Control.PlayIndex`/`Control.GetPlaylist`/`Queue.Rebuild`
  are gone, and MPRIS `OpenUri` reports `NotSupported`. The core keeps only the
  playback surface, and the front end delivers a queue of local paths.
  Version 3 removed the Remote interface and changed `Queue` from playlist
  indices to **local file paths**: `Queue.Set` was added, and
  `Queue.Append`/`InsertAfter` now take a JSON payload instead of a track index.
  Version 2 added the `core` object and the Playlist/Queue/Library/Config
  surface; version 3 removed `Remote` (remote music sources are a front-end
  feature) and restricted every path argument to local files. Version 1 was the
  pre-`core` surface (Info/Control/Lyrics only, `schema` 1).

  ```json
  "core": {
    "api_version": 4, "payload_max": 262144,
    "page_default": 200, "page_max": 1000,
    "methods": ["Info.GetInfo", "Control.Play", "Queue.Set", "…"]
  }
  ```

  `core.methods` lists every method the running core implements, so a client
  can degrade gracefully instead of discovering gaps one call at a time.
  `Control.Attach` echoes the same `api_version` back.

  The list is the audited surface, not a free-form hint. Three checks keep the
  four places that describe it in step:

  * `scripts/test/dbus-rpc-check.sh` cross-checks `core.methods` against the
    introspection XML **in both directions** — a declared method that is not
    introspectable fails, and an introspectable `org.yxzl.ter_music.*` method
    missing from the list fails too (the standard `org.freedesktop.DBus.*` and
    `org.mpris.*` surfaces are exempt from the reverse check).
  * The same script asserts the interface set itself: the five `Info`,
    `Control`, `Lyrics`, `Queue`, `Config` interfaces must be published and the
    removed content interfaces (`Playlist`, `Library`, `Favorites`, `History`,
    `DirHistory`, `Remote`) must not appear.
  * `scripts/test/doc_api_check.py` compares the method tables of this document
    with `core.methods`, so neither can drift ahead of the other.

  Adding, renaming or removing a method therefore means editing the handler,
  the interface's introspection fragment, `k_rpc_methods` in
  `media/rpc_common.c` and this document together.
- **Local paths only.** Every path in a queue payload must be a local path or a
  `file://` URI; anything with another scheme is rejected with
  `org.yxzl.ter_music.Error.InvalidArgs`. Remote sources (SMB/SFTP/FTP/WebDAV/
  HTTP) belong to the front end: it lists and downloads them itself, then hands
  the resulting local file paths to the core through `Queue.Set`.
- **No background jobs.** The core runs no asynchronous jobs of its own any more
  (the directory scan job went away with `api_version` 4 — scanning is the front
  end's work). Every method answers inside the call that carries it, progress and
  failures still surface through `Info.GetStatus` / `Control.StatusMessage` and
  the change signals, and the media loop never blocks on a client call.

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
| `CoverChanged` | `(s text, s charset, i cols, i rows)` | The cover changed (new track, size or charset). `text` is deliberately **empty** on a track change: the core no longer renders character art inside the media tick, it generates it lazily and caches it per (cover, cols, rows, charset). Callers that want to draw fetch `GetCoverArt(charset, cols, rows)`; `charset`/`cols`/`rows` in the signal are the configured values to ask for |
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
  "core": { "api_version": 4, "payload_max": 262144, "page_default": 200,
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
- `cover.text` holds newline-separated lines ready to print (`half` uses
  `▀`/`▄`/`█`, `ascii` uses `#`, `braille` uses braille patterns). It is `null`
  when the configured `info_show_cover` is off or the track has no cover. The
  text comes from the same lazily generated, per-size cache `GetCoverArt` uses,
  so asking for two different sizes never re-decodes the image twice per size.
- `lyrics.current`/`lyrics.next` are computed from the playback position, so
  they stay correct even while the TUI shows another view.
- `revision` increases when the non-position part of the snapshot changes; use
  it to discard stale data. Missing values are `null`.

## org.yxzl.ter_music.Lyrics

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetLyrics` | `() -> s` | The A/B line snapshot: `{"active_line":"A"\|"B"\|null,"line_a":{…},"line_b":{…},"track_id":"…"\|null,"has_lyrics":b,"has_timestamps":b,"revision":n}`. See [API_LYRICS_en_US.md](API_LYRICS_en_US.md) |
| `GetDocument` | `(i offset, i count) -> s` | A page of the whole lyric document: `{"revision":n,"track_id":"…","has_lyrics":b,"has_timestamps":b,"source":"embedded\|external\|none","total":n,"offset":n,"current_index":n\|null,"lines":[{"index":n,"timestamp":t\|null,"text":"…"}]}` — used by scrolling and karaoke views |
| `SetSource` | `(i source) -> b` | Switch the lyric source of the current track and reload: `1` = embedded, `2` = external (same action as `Ctrl+L` → `Tab` in the TUI). `false` when there is no current track. The **preference belongs to the front end's content store**: the core only switches and reloads — a front end that changes it is expected to record it in its own library |

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
| `ReloadConfig` | `() -> b` | Re-read `config.xml` (same as `SIGHUP`); `Config.Reload` is the newer equivalent |
| `Quit` | `() -> b` | Gracefully stop this instance (persists the playback session) |
| `Attach` | `(s role) -> s` | Register this front end. `role` is `tui`, `cli` or `app`. Returns `{"token":"…","role":"…","api_version":4,"ping_interval_ms":2000,"frontends":n}` |
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

The registry also drives the core's optional watchdog: with
`core_exit_when_no_frontend` set in `config.xml`, a primary core that once had a
front end quits 10 s after the last registration disappears (a reconnect within
that grace window resets the clock). A core that never had any front end, and a
secondary instance, never self-terminate.

Separate from the watchdog, remote/daemon-mode front ends branch on
`preferences.stop_core_on_exit` in **their own configuration mirror** when they
shut down. That mirror is filled from disk at start-up (`load_config()`) and kept
up to date by the settings page, which pushes every change to the core
immediately with `player_config_persist()`; the exit path therefore adds no
synchronous read of the core's value — no `Config.GetAll` or any other query is
issued on the way out, only the one `Quit`/`Detach` call described below. Every
front-end exit path (`q`, F9, Ctrl+C, SIGTERM,
a lost TTY) converges on the same shutdown routine: with the option at `1` — the
default — it sends `Control.Quit`, and the core persists its session/resume
state, stops playback and exits; with `0` it sends `Control.Detach` instead and
the core keeps playing in the background. Both messages presuppose that this
front end actually attached to the core — handshake completed and an `Attach`
token held. A front end that never got that far (failed handshake, offline
back-off, `Attach` never succeeded) tears down its own connection and sends not
a single D-Bus message, so it can never take somebody else's core down with it.
While the option is `1`, an exit from an attached front end stops the core
unconditionally; remaining front ends are not counted. Local-mode front ends
(in-process audio) are unaffected. The `Quit` call is best effort: it is
deliberately **not** part of the required handshake methods, a core that lacks
it still connects, and a failed call only logs — it never blocks the front end's
exit.

## org.yxzl.ter_music.Queue

The queue is a list of **local file paths** executed by the core. The front end
owns content: it scans directories, extracts metadata and hands the result over.
The core never scans anything by itself and never sees a content-list index —
queue positions are the only cursor it knows.

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `Get` | `(i offset, i count) -> s` | One page of the queue: `{"revision":n,"count":n,"current_position":n\|null,"offset":n,"rows":[{"position":n,"path":"…","title":"…","artist":"…","album":"…","is_cue":b}]}`. `count` follows the paging contract (0 = 200, max 1000, more or negative → `Error.InvalidArgs`); a page that would not fit the payload cap fails with `Error.TooLarge` |
| `Set` | `(s json) -> i` | Replace the whole queue; returns the number of entries written |
| `Append` | `(s json) -> i` | Append entries; returns the number written |
| `InsertAfter` | `(i position, s json) -> i` | Insert entries after a position; returns the number written. Called with only a payload, the position defaults to the currently playing entry |
| `RemoveAt` | `(i position) -> b` | Remove one entry |
| `MoveUp` / `MoveDown` | `(i position) -> b` | Reorder one entry |
| `Clear` | `() -> b` | Empty the queue |
| `Shuffle` | `() -> b` | Keep the current entry first and shuffle the rest |
| `PlayAt` | `(i position) -> b` | Move the cursor to a position and start playing |

Payload for `Set` / `Append` / `InsertAfter` — either `{"entries":[…]}` or a bare
array:

```json
{"entries":[
  {"path":"/music/a/01.flac","title":"…","artist":"…","album":"…",
   "cue_offset":0,"cue_track_number":0,"is_cue":false,"lyrics_source":0}
]}
```

Rules:

* A single `Set`, `Append` or `InsertAfter` call carries at most **500** entries
  (`BQ_SET_MAX`). A payload with more is rejected **as a whole** with
  `org.yxzl.ter_music.Error.InvalidArgs` — nothing is stored and no partial
  prefix is accepted — and the error text tells the caller to chunk it: one
  `Queue.Set` with at most 500 entries, then `Queue.Append` for the rest. That
  is exactly what the front end does (`playlist_queue_foreach_chunk()` walks the
  content list in ≤500-entry blocks, first block `Set`, later blocks `Append`).
* Array order **is** execution order. `Control.SetPlayMode` re-applies a play mode to
  the delivered queue (folder/album/artist and shuffle) while keeping the current entry.
* `path` must be local: `smb://`, `ftp://`, `http(s)://` and friends are
  rejected with `Error.InvalidArgs`. The front end downloads remote content and
  passes the local cache path.
* Metadata travels with each entry, so the core does not have to re-read tags.
  `duration_seconds` is deliberately sent as `0`: the core reads the duration
  from the file itself, so front end and core never disagree.
* Positions are checked: `RemoveAt`, `MoveUp`, `MoveDown` and `PlayAt` outside
  `0 … count-1` answer `org.yxzl.ter_music.Error.OutOfRange`.

Signal `QueueChanged(u revision, i count, i current_position)` after every edit.
The revision is `bq_revision()`, the same counter `Get` reports, so a client can
drop a stale page just by comparing numbers. Internal core changes that do not go
through these methods (the playback thread advancing, a shuffle rebuild) are
picked up by comparing that revision each media tick, so the signal never falls
behind.

Every method in this section, `Queue.Get` included, is published in the object's
introspection XML **and** listed in `core.methods`; `scripts/test/dbus-rpc-check.sh`
diffs the two in both directions, so the read path cannot quietly disappear from
one of them.

## org.yxzl.ter_music.Config

| Method | Signature | Description |
| ------ | --------- | ----------- |
| `GetAll` | `() -> s` | The complete configuration (see below) |
| `Set` | `(s patch) -> b` | Apply a partial patch; unknown keys or wrong types fail the whole patch with `Error.Unsupported` and nothing changes |
| `Reload` | `() -> b` | Re-read `config.xml` (same as `SIGHUP`) |
| `Reset` | `() -> b` | Restore defaults, save and broadcast |

`GetAll` mirrors `config.xml`, using the same section and key names:

```json
{ "version": 8,
  "paths": { "default_startup_path": "…", "last_opened_path": "…" },
  "theme": { "playlist_fg": 7, "playlist_bg": -1, "…": 0 },
  "preferences": { "volume_percent": 100, "default_playback_speed": 1.00,
                   "stop_core_on_exit": 1,
                   "info_preset": 0, "info_fields": 2047, "…": 0 },
  "equalizer": { "enabled": 0, "preamp": 0, "bands": [0,0,0,0,0,0,0,0,0,0] } }
```

Notes:

- Numeric values outside their range are clamped, not rejected
  (`volume_percent: 999` becomes `100`).
- `GetAll`'s `version` is the configuration contract version
  (`CONFIG_CURRENT_VERSION`, currently `8`). The `version` attribute of
  `config.xml` is now written with that same number (`"8"`); it used to read
  `2.2`. Historical `2.2` files still load — the attribute parses as `2`, which
  is above the minimum supported version — and the first run after an upgrade
  migrates them and rewrites the attribute to the number. Sending `version`
  back inside a patch is accepted and ignored, so a front end can re-apply a
  full snapshot.
  **Downgrade:** a build from before contract v8 reads the attribute with the
  same integer parse, so `version="8"` loads without error and its own migration
  blocks (`file_version < 4`, `< 7`) correctly stay inactive. But that build has
  no `stop_core_on_exit` field at all — it ignores the key, exits by detaching
  (the core keeps playing), and on its next save it writes the attribute back as
  `2.2` and drops the v8 key from `<preferences>`. Downgrading therefore costs
  the explicit value: after moving back to the current build, the one-shot v8
  migration re-applies the default `1`.
- `preferences.stop_core_on_exit` is a boolean that defaults to `1`. Contract
  version 8 added it: existing files below v8 are migrated once and receive the
  default `1`, flipping the upgrade-time behaviour from "the core keeps playing
  when the front end exits" to "an attached front-end exit stops the core" (see
  the Control section for the exit branching and the attach precondition; the
  switch is also on the settings page).
- A `Set` that touches volume, play mode or speed is visible immediately: after
  saving, the core re-applies those runtime fields straight from the in-memory
  mirror (`core_config_apply_runtime()`), so `Control.GetVolume`,
  `Info.GetInfo` and MPRIS all report the new value at once — a patch is never
  only a file write.
- Remote server entries and their passwords are **not** part of the core
  configuration. They belong to the front end, which keeps them in its own
  file; a patch containing `remote_connections` fails with
  `Error.Unsupported`.
- `Set` is how a front end persists its settings, and `config.xml` belongs to
  the core alone: a front end must never write that file itself. It changes its
  own mirror, then sends the **difference** against what it last knew, so a
  patch stays minimal (an unchanged configuration produces `{}` and no write).
  In remote mode the facade computes that difference and applies the reply back
  onto the mirror, which also makes the write safe when several front ends share
  one core. `scripts/test/check-config-ownership.sh` enforces the rule by
  rejecting `save_config()` / `config_save_to_xml()` anywhere under the front-end
  directories (`ui/`, `app/`, `main/`).

Signal `ConfigChanged(s patch)` carries the patch that was applied; `{}` means
"everything may have changed" (used by `Reset`).

## MPRIS surface

`org.mpris.MediaPlayer2` and `org.mpris.MediaPlayer2.Player` are served from the
same snapshot as the interfaces above, so a desktop widget and `ter-music show`
never disagree. Property access (the standard
`org.freedesktop.DBus.Properties.Get/GetAll/Set` trio):

- PlaybackStatus — read only — `Playing` / `Paused` / `Stopped`.
- LoopStatus — read/write — `None` / `Track` / `Playlist`.
- Shuffle — read/write — boolean.
- Rate — read/write — the real playback speed.
- MinimumRate / MaximumRate — read only — `0.5` / `3.0`.
- Volume — read/write — `0.0`–`1.0`, values outside are clamped.
- Position, Metadata, Can* — read only — µs positions; `Metadata` carries
  `mpris:artUrl` when the track has a cover.

- **Rate is genuinely read/write.** `Get` and `GetAll` report the live speed (and
  `GetAll` includes `Rate`, `MinimumRate` and `MaximumRate`, which an earlier
  build dropped), `PropertiesChanged` fires when it changes, and `Set` applies it
  to the running stream. A rate outside `MinimumRate…MaximumRate` answers
  `org.yxzl.ter_music.Error.OutOfRange`; a rate the core cannot apply answers
  `Error.Failed`. `Control.SetSpeed`/`GetSpeed` are the same thing on the
  ter-music interface.
- **LoopStatus and Shuffle switch whole play modes** rather than storing two
  cosmetic flags, and they keep the current group (folder/album/artist) and the
  current repeat level. Writing a value that the current mode already expresses
  does nothing — no queue rebuild, no config write.

  | Write | Resulting play mode |
  | ----- | ----------------- |
  | `LoopStatus = "Track"` | single repeat |
  | `LoopStatus = "Playlist"` | repeating variant of the same group: sequential → list repeat, shuffle once → shuffle repeat, folder/album/artist sequential → the matching `*_repeat`, `*_shuffle` → `*_shuffle_repeat` |
  | `LoopStatus = "None"` (or an unknown value) | non-repeating variant of the same group: list repeat → sequential, shuffle repeat → shuffle once, `*_repeat` → `*_sequential`, `*_shuffle_repeat` → `*_shuffle`; single repeat falls back to sequential |
  | `Shuffle = true` | shuffling variant, repeat level kept: sequential → shuffle once, list/single repeat → shuffle repeat, `*_sequential` → `*_shuffle`, `*_repeat` → `*_shuffle_repeat` |
  | `Shuffle = false` | ordered variant, repeat level kept: shuffle once → sequential, shuffle repeat → list repeat, `*_shuffle` → `*_sequential`, `*_shuffle_repeat` → `*_repeat` |

  Reading is the projection of the same rule: `LoopStatus` is `Track` for single
  repeat, `Playlist` for any repeating mode and `None` otherwise, and `Shuffle`
  is true for the eight shuffling modes. `Info.GetInfo` exposes both as
  `playback.loop_status` and `playback.shuffle`.
- **`OpenUri` always answers `org.freedesktop.DBus.Error.NotSupported`.** Since
  `api_version` 4 the core loads no content by itself: it neither scans a
  directory nor builds a playlist, so a client must load the content and deliver
  it through `Queue.Set` plus `Queue.PlayAt`. A non-local URI is rejected with its
  own message ("remote sources belong to the front end").
- On the root interface `Raise` is accepted as a no-op and `Quit` answers
  `NotSupported`. To stop an instance use `org.yxzl.ter_music.Control.Quit` (or
  `ter-music daemon stop`), which also persists the playback session.

## Removed in api_version 4: the content interfaces

`Playlist`, `Library`, `Favorites`, `History` and `DirHistory` are gone. The
library, the playlists, the user song lists, the favourites and the history are
**front-end** data: the front end owns its SQLite database, scans directories on
its own threads and renders the results itself. The core publishes playback only.

Consequences for a client:

* `Control.OpenPath`, `Control.PlayIndex`, `Control.GetPlaylist` and
  `Queue.Rebuild` were removed. Open content locally, then call `Queue.Set`
  followed by `Queue.PlayAt`.
* MPRIS `OpenUri` answers `org.freedesktop.DBus.Error.NotSupported`: the core
  cannot load a URI by itself any more.
* Calls to a removed interface or method now get
  `org.freedesktop.DBus.Error.UnknownMethod` immediately instead of a timeout.

## Removed: org.yxzl.ter_music.Remote

Removed in `core.api_version` 3. Remote music sources (SMB/SFTP/FTP/WebDAV/
HTTP) are a **front-end** feature: the front end stores its own server list
(outside the core configuration), lists and downloads remote directories, and
then hands the resulting local file paths to `Queue.Set` / `Queue.Append`.
The core never sees a remote URL, so a stale client calling `Remote.*` gets the
standard `org.freedesktop.DBus.Error.UnknownMethod`.

## Examples

```bash
# Handshake and snapshot
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Info.GetInfo

# Deliver a queue (the front end scanned this list itself), then play entry 1
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Queue.Set \
  '{"entries":[{"path":"/home/user/Music/a.flac","title":"A","artist":"X","album":"AL"}]}'
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Queue.PlayAt 1
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Queue.Get 0 50

# Change a setting (unknown keys fail without changing anything)
gdbus call --session --dest org.mpris.MediaPlayer2.ter_music \
  --object-path /org/mpris/MediaPlayer2 \
  --method org.yxzl.ter_music.Config.Set '{"preferences":{"volume_percent":40}}'

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
  `core.api_version` (currently `4`). Consumers must ignore unknown fields.
  Version 3 removed `org.yxzl.ter_music.Remote` and the `track.is_remote` field
  and made every path argument local-only; version 2 added the `core` object and
  the Playlist/Queue/Library/Favorites/History/DirHistory/Config interfaces.
- The API shares the MPRIS lifecycle: it exists while the process owns the bus
  name and is torn down on shutdown.
- Inside a Linyaps (如意玲珑) package the container joins the host session bus,
  so host applications see the same object path, interfaces and JSON schemas.
  The D-Bus activation file for `org.mpris.MediaPlayer2.ter_music` and the
  systemd user unit are installed **only** by the Linyaps build, which configures
  `cmake -DINSTALL_LINYAPS_INTEGRATION=ON` (the option is `OFF` by default, see
  `CMakeLists.txt`); deb, RPM/AUR and plain `make install` builds ship neither.
  On-demand activation therefore only exists in the Linyaps package: elsewhere
  the CLI starts a background core itself when none is running, and inside the
  container — where detaching a process is impossible — `daemon start`, `play`
  and the front end ask the bus to activate and print host-side guidance when
  that is unavailable.
