<div align="center">

# Ter-Music

**Terminal music player for Linux**

![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
[![CI](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml/badge.svg)](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/HuanSoft-Open-Source-Community/ter-music?sort=semver)](https://github.com/HuanSoft-Open-Source-Community/ter-music/releases)
![Linyaps](https://img.shields.io/badge/Linyaps-Supported-8A2BE2.svg)

</div>

**Languages:** [简体中文（现代）](translations/README_zh-CN_Modern.md) · [中文（文言）](translations/README_zh-CN_Legacy.md)

**Docs:** [Build & packaging (中文)](BUILD_GUIDE.md) · [TUI layouts](UI_LAYOUTS_en_US.md) · [D-Bus API](API_DBUS_en_US.md) · [Lyrics API](API_LYRICS_en_US.md) · [Release process (中文)](RELEASE.md)

## Overview

Ter-Music plays local and remote music in a terminal. FFmpeg decodes, ncursesw draws the UI, and PipeWire, PulseAudio or ALSA is selected at runtime. A SQLite library is built in; MPRIS and the D-Bus control API are available when D-Bus support is compiled in.

The player has two planes: a **core** that owns the audio device, transport and the path queue it is given, and a **front end** (TUI or CLI) that owns files, the library, playlists, history and remote downloads. Starting the TUI starts a core when none is running.

## Features

**Playback**
- MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS and WavPack, plus CUE sheets for FLAC/APE/WV (GBK, BIG5 and Shift-JIS auto-detected).
- 17 play modes (sequential, repeat, shuffle; folder, album and artist scopes), six speeds (0.75x-3x) and gapless preload at 1.0x.
- 10-band ISO equalizer (31 Hz-16 kHz, ±12 dB) with presets and soft clipping.
- Karaoke vocal removal: off, soft or full band-limited centre cancellation; stereo only.
- Embedded or external LRC lyrics, with per-character highlighting when timings allow.

**Content**
- SQLite library with FTS5 search, pinyin matching and recursive scanning; browse by artist, album or genre.
- Play queue, user playlists, favorites, playback history and folder history.
- SMB, SFTP, FTP, WebDAV and HTTP sources; the front end downloads each track into a local cache.

**Interface**
- Six panes (playlist, info, controls, spectrum, lyrics, vinyl), seven JSON layout presets and forced tiling.
- 24 theme palettes plus a custom slot; per-pane foreground and background colors.
- Album art as colored half-blocks; a vinyl pane with a rotating cover label.
- Keyboard and mouse operation; built-in languages `zh_CN`, `en_US` and `zh-PQ`.

**Integration**
- CLI and a detached playback daemon; control the player from any terminal.
- MPRIS plus the D-Bus interfaces Info, Control, Queue, Config and Lyrics (when built with D-Bus).
- Linyaps package support.

## Install

### Arch Linux

The AUR recipe is prepared as `cn.org.huansoft.ter-music` but is **not published yet**. Build from source below, or use the locally built packages described in [BUILD_GUIDE.md](BUILD_GUIDE.md).

### Build from source

Required: CMake 3.10+, a C99 compiler, pkg-config, ncursesw, FFmpeg libraries (avcodec, avformat, avutil, avfilter, swresample, swscale), libpng, libjpeg, libxml2, SQLite3 and zlib.

Optional: PipeWire, PulseAudio and ALSA output; D-Bus (MPRIS/API); libcurl (remote sources).

**Fedora / RHEL**

```bash
sudo dnf install cmake gcc make pkg-config ncurses-devel \
  ffmpeg-free-devel libpng-devel libjpeg-turbo-devel libxml2-devel \
  sqlite-devel zlib-devel
# optional
sudo dnf install pipewire-devel pulseaudio-libs-devel alsa-lib-devel \
  dbus-devel libcurl-devel
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential cmake pkg-config libncursesw5-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev libpng-dev libjpeg-dev libxml2-dev \
  libsqlite3-dev zlib1g-dev
# optional
sudo apt install libpipewire-0.3-dev libpulse-dev libasound2-dev \
  libdbus-1-dev libcurl4-openssl-dev
```

**Arch Linux**

```bash
sudo pacman -S --needed base-devel cmake pkgconf ncurses ffmpeg \
  libpng libjpeg-turbo libxml2 sqlite zlib
# optional
sudo pacman -S --needed libpipewire libpulse alsa-lib dbus curl
```

**Build**

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
sudo make install   # optional
```

The binary is `build/ter-music` (or `./ter-music` from the build directory). Locally built packages: one DEB (Debian 10–13 / Ubuntu 20.04–26.04), four RPMs (EL7–10), one AppImage (glibc 2.28), and one Linyaps UAB + layer; DEB/RPM source and debug packages are optional. See [BUILD_GUIDE.md](BUILD_GUIDE.md).

## Quick start

```bash
ter-music                  # TUI; starts a core if none is running
ter-music -o ~/Music       # open a folder or file
ter-music ftp://user:pass@host/music
```

The TUI needs a real terminal; do not pipe its output. Press `O` to open a folder, `↑`/`↓` to select, `Enter` or `Space` to play, and `q` to quit.

| Key | Action |
| --- | --- |
| `O` / `o` | Open folder |
| `↑` `↓` / `j` `k` | Select |
| `Enter` / `Space` | Play or open |
| `n` / `p` | Next / previous |
| `,` / `.` | Seek back / forward 5 s |
| `-` / `=` | Volume down / up |
| `C` / `L` | Focus controls / list |
| `Tab` | File browser ↔ play queue |
| `Ctrl+L` | Lyric seek mode |
| `M` | Library browser |
| `/` or `S` | Search (pinyin) |
| `a` / `i` | Append / insert next |
| `I` | Append folder |
| `d` / `D` | Remove / clear queue |
| `F` / `f` | Toggle favorite |
| `A` | Add to playlist |
| `h` | Playback history popup |
| `R` | Restart the core |
| `F1`-`F9` | Views and quit |
| `q` | Quit |

The full list is in `F8` and `ter-music keys list`. Rebind actions in Settings → Hotkeys. `F3` is folder history; `h` is the playback-history popup.

Mouse: left click selects and activates, the wheel scrolls, and right click opens a context menu. Middle click opens the same menu on terminals that intercept right click. The layout follows terminal resizes.

## CLI

Every subcommand is a D-Bus client of the running core; `play` also scans the given path and delivers the resulting queue.

| Command | Purpose |
| --- | --- |
| `play [PATH]` | Scan a path and play it |
| `pause` `resume` `toggle` `stop` | Transport |
| `next` `prev` | Change track |
| `seek` | Seek: +seconds, -seconds, mm:ss or percent |
| `volume` | Query or set volume (0-100, +N, -N) |
| `speed` | Query or set speed (0.5-3.0) |
| `mode` | Query or set play mode (name or 0-16) |
| `karaoke` | Vocal-removal level (off, soft, full, 0-2, cycle) |
| `queue` | Edit the queue (list, clear, shuffle, remove, append, insert) |
| `config` | Read or change configuration (get, set, reload, reset) |
| `show` | Print the info block |
| `lyrics` `track` `cover` | Current lyrics, metadata or text cover |
| `keys` | Audit key bindings (list, check) |
| `daemon` | Manage the core (start, foreground, stop, restart, status, reload) |
| `tui` `version` `help` | TUI, version, help |

```bash
ter-music play ~/Music
ter-music show --one-line --watch=1000
ter-music show --json | jq -r '.track.title'
ter-music daemon start --open ~/Music
ter-music daemon stop
```

Exit codes: `0` success, `1` usage error, `3` no running core, `4` D-Bus unavailable, `5` rejected by the core.

`ter-music help <command>` and `ter-music help all` print full details. `--lang <id>` selects the CLI language for one run.

**Linyaps.** Inside the package the binary lives in the container:

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music show
ll-cli run cn.org.huansoft.ter-music -- ter-music play ~/Music
```

Background playback uses D-Bus activation, the shipped systemd user service, or `--foreground` inside `ll-cli run`; enable the service from the host with `systemctl --user enable --now cn.org.huansoft.ter-music`. When the container is already running, read output with `ll-cli enter` or `journalctl --user -u cn.org.huansoft.ter-music`.

## Playback

| Group | Modes |
| --- | --- |
| Basic (5) | Sequential, Single Repeat, List Repeat, Shuffle Once, Shuffle Repeat |
| Folder (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |
| Album (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |
| Artist (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |

Shortcuts `1`-`5` select Sequential, Single Repeat, List Repeat, Shuffle Repeat and Folder Sequential. Advanced modes need library metadata and are enabled in Settings → Play Mode.

Speed: 0.75x, 1.0x, 1.25x, 1.5x, 2x and 3x, pitch preserved (FFmpeg atempo). Equalizer: 10 bands from 31 Hz to 16 kHz, presets in Settings → Equalizer. Karaoke: off, soft or full, stereo only; change it from the controls popup, Settings → Playback, `ter-music karaoke` or `Control.SetKaraoke`.

## Lyrics and covers

- Embedded lyrics (FFmpeg/APE tags) are preferred over an external `.lrc` with the same basename.
- Per-character highlighting is used when the LRC carries character, line-start/end or interpolated timings.
- `Ctrl+L` enters lyric seek mode; `Tab` switches between embedded and external lyrics.
- Covers come from embedded art first, then `cover`/`folder`/`front`/`album` images in the same directory.
- Rendering falls back from 256 colors to 8 colors to monochrome; `ter-music cover` and `Info.GetCoverArt` expose braille, ASCII or half-block text covers.

## Layouts and themes

A layout is a binary split tree. Its leaves name one of six panes, so the main area is always force-tiled; resizing keeps the per-mille ratios. The presets are `classic`, `vinyl`, `karaoke`, `visualizer`, `focus`, `stage` and `neon`.

Select or edit layouts in `F2` → Appearance → UI Layout. Files:

- built-in: `<prefix>/share/ter-music/layouts/layouts.json`
- user override: `~/.config/ter-music/layouts.json`
- development override: `TER_MUSIC_LAYOUTS=/path/to/layouts.json`

24 theme palettes and a custom color slot color the sidebar, highlight and border; each pane can also carry its own foreground/background. The full schema is in [UI_LAYOUTS_en_US.md](UI_LAYOUTS_en_US.md).

## Configuration and data

`$XDG_CONFIG_HOME/ter-music` (default `~/.config/ter-music`):

```text
config.xml     core configuration
library.db     SQLite library: tracks, favorites, history, playlists, folder history
remote.xml     remote servers (front end only)
layouts.json   user layout overrides
ui-layout      selected layout id
lang/          user language packs
help/          user quick-start text
```

Downloaded remote tracks are cached under `$XDG_CACHE_HOME/ter-music/remote/`; extracted covers are temporary files under `/tmp/ter-music-cover-*.jpg`.

Common `config.xml` keys:

| Key | Value |
| --- | --- |
| `audio_backend` | 0 auto, 1 PulseAudio, 2 ALSA, 3 PipeWire |
| `audio_latency_ms` | output latency, 20-250 |
| `volume_percent` | default volume, 0-100 |
| `default_playback_speed` | 0.75-3.0 |
| `default_play_mode` | 0-16 |
| `ui_language` | `zh_CN`, `en_US`, `zh-PQ` or an installed pack id |
| `karaoke_mode` | 0 off, 1 soft, 2 full |
| `resume_last_playback` | resume the last track and position |
| `stop_core_on_exit` | 1 stop the core on exit, 0 keep playing |
| `core_exit_when_no_frontend` | exit 10 s after the last front end leaves |

Only changed key bindings are written to `config.xml`:

```xml
<keybindings>
  <binding action="track.next" key="]"/>
  <binding action="track.prev" key="["/>
</keybindings>
```

The reserved keys are `F1`-`F9`, `q`/`Q` and `Tab`/`Shift+Tab`. Use `ter-music keys list` for action IDs and `ter-music keys check` to validate the file.

Language packs are XML files (`<lang id="..." name="...">` with `<string key="...">` entries) installed under `~/.config/ter-music/lang/` or as `.tar.gz` archives from the `F7` language view (`A` to install, `D` to delete). Built-in packs: `zh_CN`, `en_US`, `zh-PQ`.

## Integrations

When built with D-Bus, MPRIS is published as `org.mpris.MediaPlayer2.ter_music` on `/org/mpris/MediaPlayer2`. The same object carries:

| Interface | Purpose |
| --- | --- |
| `cn.org.huansoft.ter_music.Info` | Track, progress, cover, lyric lines, rendered info block |
| `cn.org.huansoft.ter_music.Control` | Transport, seek, volume, speed, mode, karaoke, quit |
| `cn.org.huansoft.ter_music.Queue` | The core's local-path queue |
| `cn.org.huansoft.ter_music.Config` | Configuration read and write |
| `cn.org.huansoft.ter_music.Lyrics` | Lyric lines and per-character timing |

`Info.GetInfo` reports `core.api_version` (currently 7). See [API_DBUS_en_US.md](API_DBUS_en_US.md) and [API_LYRICS_en_US.md](API_LYRICS_en_US.md).

## Development

| Plane | Owns |
| --- | --- |
| Core (`daemon`) | Audio device, decoding, transport, path queue, current-track info, configuration |
| Front end (TUI/CLI) | Library, scanning, playlists, favorites, history, remote sources, UI |

Source lives under `src/cn.org.huansoft.ter-music/<module>/`; public headers under `include/cn.org.huansoft.ter-music/<module>/`.

```bash
mkdir -p build/debug
cd build/debug
cmake -DCMAKE_BUILD_TYPE=Debug ../..
make -j"$(nproc)"
cd ../..
./scripts/test/run-unit-tests.sh
```

Four gates run in CI:

| Gate | Rule |
| --- | --- |
| `scripts/test/check-core-purity.sh` | Core has no content or remote-source symbols |
| `scripts/test/check-ui-purity.sh` | UI reaches playback only through the `player` facade |
| `scripts/test/check-config-ownership.sh` | Core is the only writer of `config.xml` |
| `scripts/test/check-package-identity.sh` | App id and version agree across packages |

## Troubleshooting

- **No sound:** check that PipeWire, PulseAudio or ALSA is running; the backend is probed in that order. An explicitly chosen backend falls back to the next working one at play time. Switch it in Settings → Audio.
- **Stutter or crackle:** raise Output Latency in Settings (20-250 ms, in 10 ms steps).
- **Garbled CJK:** use a UTF-8 locale and a UTF-8 terminal; `locale` should show `LC_CTYPE=UTF-8`.
- **Missing headers at build time:** install the `-devel`/`-dev` variants of the dependencies above.
- **CUE tracks missing:** the `.cue` file must share the audio file basename; change CUE Encoding in Settings if the text is garbled.
- **Library is empty:** open the library browser (`M`) and scan or rescan; the database is `library.db` in the config directory.

## License

GPL-3.0-only. See [LICENSE](../LICENSE).

Ter-Music is a playback tool only: it does not provide, host or distribute audio content. You are responsible for the files you play.

## Author

HuanSoft (浣软科技) · <yxzl666xx@outlook.com>

## Acknowledgments

- **@guanzi008** — Debian packaging, MPRIS, UTF-8 input, mouse and UI fixes.
- **@Zeta** — Arch Linux packaging.
