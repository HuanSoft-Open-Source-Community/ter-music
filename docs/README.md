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

The player has two planes: a **core** that owns the audio device, transport and the path queue it is given, and a **front end** (TUI or CLI) that owns files, the library, playlists, history and remote downloads. A front end starts a core when none is running: outside a container it forks one in the background, while inside the Linyaps container it requests D-Bus activation instead because the container cannot fork a detached process (see the Linyaps notes under CLI).

## Features

**Playback**
- MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS and WavPack, plus CUE sheets for FLAC/APE/WV (GBK, BIG5 and Shift-JIS auto-detected).
- 17 play modes (sequential, repeat, shuffle; folder, album and artist scopes), six speeds (0.75x-3x) and gapless preload at 1.0x.
- 10-band ISO equalizer (31 Hz-16 kHz, ±12 dB) with presets and soft clipping.
- Karaoke vocal removal: off, soft or full per-bin centre cancellation inside 200 Hz–5 kHz; stereo only, with about 9 ms of algorithmic latency while enabled.
- Embedded or external LRC lyrics, with per-character highlighting when timings allow.

**Content**
- SQLite library with FTS5 search, pinyin matching and recursive scanning; browse by artist, album or genre.
- Play queue, user playlists, favorites, playback history and folder history.
- SMB, SFTP, FTP, WebDAV and HTTP sources; the front end downloads each track into a local cache and hands those local paths to the core. SMB needs the Samba client, SFTP needs a known_hosts file (see [Remote music sources](#remote-music-sources)).

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

Required: CMake 3.10+, a C99 compiler, pkg-config, ncursesw, FFmpeg libraries (avcodec, avformat, avutil, avfilter, swresample, swscale), libpng, libjpeg, libxml2, SQLite3, zlib and libcurl. libcurl is not optional: the build stops with a clear error when it is missing.

Optional: PipeWire, PulseAudio and ALSA output; D-Bus (MPRIS/API). SMB sources additionally need the external `smbclient` program (Samba client) at run time; without it the build still works and SMB connections report that it is missing.

**Fedora / RHEL**

```bash
sudo dnf install cmake gcc make pkg-config ncurses-devel \
  ffmpeg-free-devel libpng-devel libjpeg-turbo-devel libxml2-devel \
  sqlite-devel zlib-devel libcurl-devel
# optional
sudo dnf install pipewire-devel pulseaudio-libs-devel alsa-lib-devel \
  dbus-devel
# SMB sources need the Samba client at run time
sudo dnf install samba-client
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential cmake pkg-config libncursesw5-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev libpng-dev libjpeg-dev libxml2-dev \
  libsqlite3-dev zlib1g-dev libcurl4-openssl-dev
# optional
sudo apt install libpipewire-0.3-dev libpulse-dev libasound2-dev \
  libdbus-1-dev
# SMB sources need the Samba client at run time
sudo apt install smbclient
```

**Arch Linux**

```bash
sudo pacman -S --needed base-devel cmake pkgconf ncurses ffmpeg \
  libpng libjpeg-turbo libxml2 sqlite zlib curl
# optional
sudo pacman -S --needed libpipewire libpulse alsa-lib dbus
# SMB sources need the Samba client at run time
sudo pacman -S --needed smbclient
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
ter-music                        # TUI; starts a core if none runs (Linyaps: D-Bus activation)
ter-music -o ~/Music             # open a folder or file
ter-music -o ftp://host/music/   # open a remote folder in the TUI
```

The TUI needs a real terminal; do not pipe its output. Press `O` to open a folder, `↑`/`↓` to select, `Enter` or `Space` to play, and `q` to quit.

A remote URL opened this way uses only the credentials embedded in the URL. For a server that needs a password, add a connection in `F2` → Remote Devices and open the folder there instead of writing `user:password@` into the URL: the URL stays visible in the shell history and process list. `ter-music play` and `ter-music daemon start --open` reject remote URLs because the core plays local paths only.

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

Mouse: left click selects and activates, the wheel scrolls, and right click opens a context menu. Middle click opens the same menu on terminals that intercept right click. Menu slide animations compose one frame per movement and degrade or switch off on slow terminals and in Aurora Flow; input is handled without waiting for an animation to finish. Force the tier with `TER_MUSIC_MENU_ANIM=full|reduced|off` (default auto). The layout follows terminal resizes.

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

The TUI cannot fork a detached core inside the container, so it asks the host to activate the service. When activation is not available, start the core on the host first and keep it in the foreground, then open the TUI:

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music daemon foreground
```

**ll-box cold-start replay.** ll-box ≤ 2.3.5 can replay bytes it has already written when its cold-start forwarder hits a full terminal queue, splicing duplicated output into the TUI. ter-music cannot stop that write; inside the container it measures each frame's terminal writes (`wchar` in `/proc/self/io`) and forces one full-screen repaint only on burst or back-pressure evidence or after a terminal resize. A bare-mode reset likewise needs that evidence (or a terminal resize), and the `endwin`+`refresh` fallback stays opt-in (`TER_MUSIC_TTY_HARD_RESET=1`, off by default). None of this runs outside the container, and the guard adds no configuration key, setting-page entry or user-visible switch. Known limitation: a repaint is no longer scheduled by a timer when there is no evidence, so damage that appears in a session with only small frames and a slow reader is not repainted proactively; it may remain until the next repaint-class event (a terminal resize, a view change or new burst/back-pressure evidence).

The root fix is upstream: PR #295 is merged into linyaps-box `release/2.3` (merge commit `31ddf04`, 2026-11-20) and makes the forwarder consume the bytes it already wrote when `TryAgain` is returned with `k > 0`; it covers ll-box ≤ 2.3.5. No release contains it yet (the latest release is still 2.3.5) and `master` does not carry it, so check that the tag you install contains it and do not assume the next release will. There is no plan to publish a UAB with the next ter-music Release; UAB artefacts are planned to be published after the upstream fix is released. `TER_MUSIC_TTY_REPAIR_BACKSTOP_MS`, an undocumented tuning variable, was removed with the timer fallback; it only configured that fallback, so setting it now has no effect.

## Playback

| Group | Modes |
| --- | --- |
| Basic (5) | Sequential, Single Repeat, List Repeat, Shuffle Once, Shuffle Repeat |
| Folder (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |
| Album (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |
| Artist (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |

Shortcuts `1`-`5` select Sequential, Single Repeat, List Repeat, Shuffle Repeat and Folder Sequential. Advanced modes need library metadata and are enabled in Settings → Play Mode.

Speed: 0.75x, 1.0x, 1.25x, 1.5x, 2x and 3x, pitch preserved (FFmpeg atempo). Equalizer: 10 bands from 31 Hz to 16 kHz, presets in Settings → Equalizer.

Karaoke: off, soft or full, stereo only. It cancels centred coherent content per frequency bin inside 200 Hz–5 kHz — soft leaves it about 8 dB down, full removes it — while pan-spread accompaniment and hard-panned transients are kept, so a hard-panned hi-hat or snare does not release the cancellation of the centred vocal. Mono tracks pass through untouched. While enabled, the processed stream is delayed by the window length minus one sample — 383 samples at 44.1 kHz (about 8.7 ms), at most 10 ms at every supported rate; turning the level off fades the cancellation out over about 0.35 s before bit-exact pass-through resumes. Change it from the controls popup, Settings → Playback, `ter-music karaoke` or `Control.SetKaraoke`.

## Remote music sources

Remote sources are front-end work. The front end lists the remote folder, downloads its audio files into a local cache and hands those local paths to the core; the core never opens a remote URL (the queue contract is in [API_DBUS_en_US.md](API_DBUS_en_US.md)).

Add a connection in `F2` → **Remote Devices** (the sixth Settings item): name, protocol (SMB, SFTP, FTP, WebDAV or HTTP), host, port (empty = the protocol default), username, password and base path. The base path is required: `/` is the server root, and SMB wants the share plus an optional subfolder — for example `music/albums`, not `smb://host/music/albums`. HTTP, WebDAV and FTP add a TLS switch (https/webdavs/ftps) and, once on, a "trust self-signed certificate" switch. SFTP adds a private-key path, a key passphrase and a known_hosts path. A directory path or URL does not need a trailing slash; the front end adds it when listing. TLS is an explicit per-connection switch, not a port rule: an HTTPS server on port 8443 needs TLS on and the port set to 8443. A port outside 1–65535 or an empty base path cannot be saved; deleting a connection asks for confirmation.

Protocol notes:

- **SMB** runs the external `smbclient` program (not libcurl) and passes it host, port, username and password; the password goes through a temporary mode-0600 auth file, never argv. Install the Samba client (package `smbclient`; the Debian, Arch and RPM metadata recommend it) or SMB connections fail with a "smbclient is missing" message. File names containing `;`, `!`, double quotes, backslashes or control characters are refused because `smbclient -c` would split them into commands; rename such files on the server.
- **SFTP** verifies host keys and fails closed: a readable known_hosts file (the connection field, otherwise `~/.ssh/known_hosts`) must already contain the server. There is no silent trust-on-first-use; add the key first, for example `ssh-keyscan HOST >> ~/.ssh/known_hosts`. A private key and passphrase are optional.
- **FTP** lists with MLSD and falls back to LIST. TLS means implicit FTPS (`ftps://`, default port 990).
- **WebDAV** sends `PROPFIND` with `Depth: 1` and accepts both namespaced (`<D:href>`, Apache/Nextcloud-style) and plain responses.
- **HTTP** reads HTML directory indexes (Apache/nginx/python style), follows the link target instead of the displayed text, and decodes HTML entities.
- HTTP and WebDAV negotiate Basic, Digest or NTLM authentication. With TLS on, certificates are verified unless "trust self-signed certificate" is set, and an HTTPS transfer is never redirected down to HTTP.

Opening a folder queues a background job that lists it and downloads its audio files (files directly in that folder, not subfolders) into the cache. Up to 200 tracks are downloaded per open; a larger folder reports how many of how many tracks were loaded, and subfolders can be opened separately. The remote page and the main status bar show progress (`Downloading done/total, failed n`), `ESC` cancels the running job, and the connection menu offers "Cancel current job", "Retry listing" and "Retry failed tracks (n)". Downloaded tracks are handed to the core as they finish: the queue is replaced and playback starts at the first track.

Guard rails: one remote file may be at most 1 GiB and one directory listing at most 32 MiB (200,000 entries); connecting times out after 10 s, listing after 120 s and each download after 600 s. A single file beyond a cap is skipped and the rest of the folder continues; an oversized listing or a full cache stops the job with an error. Downloads happen on demand for the folder you open; there is no prefetch.

The cache is `$XDG_CACHE_HOME/ter-music/remote/` (default `~/.cache/ter-music/remote/`), mirroring the connection and remote path. Remote Devices shows its size, sets a limit (default 2048 MB, `0` = unlimited) and cleans unprotected files; files that are playing, queued, favorited or in the history are kept. A `.lrc` next to a downloaded audio file is cached with it.

**Credentials.** The connection list lives in the front-end file `remote.xml` (mode 0600). Passwords are obfuscated with a fixed-salt XOR stream, not encrypted: this stops casual reading, not someone who can read the file. Logs, the core's `last_opened_path` and the library's folder history store a redacted URL, so a URL read back from history does not carry the password — use the saved connection for password-protected servers. Do not put `user:password@` into the URL of an ad-hoc remote open: the URL stays visible in the shell history and process list.

**Verified scope.** The automated suite exercises HTTP, WebDAV, FTP, the FTPS TLS handshake, HTTPS on non-443 ports, self-signed CA trust, 401/404 and slow-download cancellation. SMB is exercised through a fake `smbclient` that follows Samba's command-string parsing; SFTP covers the fail-closed known_hosts guard. A real Samba server, a real SSH server and the FTPS data channel were not available to the suite.

A remote URL can also be opened directly by the TUI:

```bash
ter-music -o webdav://host/music/
```

Accepted schemes are `http`, `https`, `ftp`, `ftps`, `sftp`, `smb`, `webdav`
and `webdavs` (`dav`/`davs` are aliases); the scheme is matched case-insensitively.

`ter-music play` and `ter-music daemon start --open` reject remote URLs because the core plays local paths only; open remote folders from the TUI.

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
remote.xml     remote servers and cache limit (front end only, mode 0600)
layouts.json   user layout overrides
ui-layout      selected layout id
lang/          user language packs
help/          user quick-start text
```

Downloaded remote tracks are cached under `$XDG_CACHE_HOME/ter-music/remote/` (default `~/.cache/ter-music/remote/`), mirroring the connection and remote path; a sibling `.lrc` is cached with the track. The cache limit and the clean action are in Settings → Remote Devices. Extracted covers are temporary files under `/tmp/ter-music-cover-*.jpg`.

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
- **Duplicated or garbled TUI lines inside Linyaps:** the ll-box ≤ 2.3.5 cold-start replay described under "Linyaps" above. The guard repaints the screen when it sees burst or back-pressure evidence; with no evidence it no longer repaints on a timer, so the damage may stay until a terminal resize, a view change or new evidence.
- **Missing headers at build time:** install the `-devel`/`-dev` variants of the dependencies above.
- **A remote folder will not list:** check protocol, host, port and base path (SMB needs `share/subpath`); install `smbclient` for SMB; create a readable known_hosts file for SFTP; turn on TLS for an https/webdavs/ftps server. The page shows the localized reason and the connection menu offers "Retry listing".
- **Remote tracks failed to download:** the page shows `failed n` and the first error; fix the cause (network, permissions, disk space) and use "Retry failed tracks (n)" — an already cached track is not downloaded again. A file over 1 GiB or a full cache stops at that track; raise the cache limit or clean the cache in Remote Devices.
- **A remote folder stopped after many tracks:** one open downloads at most 200 tracks; browse and open subfolders separately.
- **A remote URL pasted from the folder history fails to authenticate:** stored URLs are redacted (no password by design); open the saved connection in Remote Devices instead.
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
