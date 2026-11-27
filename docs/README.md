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

**Docs:** [Build & packaging (中文)](BUILD_GUIDE.md) · [TUI layouts](UI_LAYOUTS_en_US.md) · [D-Bus API](API_DBUS_en_US.md) · [Lyrics API](API_LYRICS_en_US.md) · [Release process (中文)](RELEASE.md) · [26.4.1.0 release notes](RELEASE_NOTES_26.4.1.0.md)

## Overview

Ter-Music plays local and remote music in a terminal. FFmpeg decodes, ncursesw draws the UI, and PipeWire, PulseAudio or ALSA is selected at runtime. A SQLite library is built in; MPRIS and the D-Bus control API are available when D-Bus support is compiled in.

The player has two planes: a **core** that owns the audio device, transport and the path queue it is given, and a **front end** (TUI or CLI) that owns files, the library, playlists, history and remote downloads. A front end starts a core when none is running: outside a container it forks one in the background, while inside the Linyaps container it requests D-Bus activation instead because the container cannot fork a detached process (see the Linyaps notes under CLI).

## Features

**Playback**
- MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS and WavPack, plus CUE sheets for FLAC/APE/WV (GBK, BIG5 and Shift-JIS auto-detected).
- 17 play modes (sequential, repeat, shuffle; folder, album and artist scopes), six speeds (0.75x-3x) and gapless preload at 1.0x.
- 10-band ISO equalizer (31 Hz-16 kHz, ±12 dB) with presets and soft clipping.
- Karaoke vocal removal with the bundled ONNX model (UVR-MDX-NET 3): Off, Faint vocals or No vocals. The `[Karaoke]` button and the three levels are always available — there is no feature switch and no model picker; at a level other than Off the first play of an uncached track waits for the first accompaniment segment (see [Karaoke](#karaoke)).
- Embedded or external LRC lyrics, with per-character highlighting when timings allow.

**Content**
- SQLite library with FTS5 search, pinyin matching and recursive scanning; browse by artist, album or genre.
- Play queue, user playlists, favorites, playback history and folder history.
- SMB, SFTP, FTP, WebDAV and HTTP sources; the front end downloads each track into a local cache and hands those local paths to the core. SMB needs the Samba client, SFTP needs a known_hosts file (see [Remote music sources](#remote-music-sources)).

**Interface**
- Six panes (playlist, info, controls, spectrum, lyrics, vinyl), seven JSON layout presets and forced tiling.
- 24 theme palettes plus a custom slot; per-pane foreground and background colors.
- Terminal light/dark adaptation: detects the terminal background (OSC 11, deepin-terminal theme, XDG portal, COLORFGBG, TERM hint) and switches the factory colours and the seven layout presets; choose Auto, Light or Dark in Settings → Appearance.
- Album art as colored half-blocks; a vinyl pane with a rotating cover label.
- Keyboard and mouse operation; built-in languages `zh_CN`, `en_US` and `zh-PQ`.
- One chooser for files and folders: a desktop dialog (`zenity`/`kdialog`, with the XDG portal as the preferred route) when a graphical session is available, otherwise a typed path in a pure TTY. It is used by Open folder, Append folder, language-pack install and the default music directory.

**Integration**
- CLI and a detached playback daemon; control the player from any terminal.
- MPRIS plus the D-Bus interfaces Info, Control, Queue, Config and Lyrics (when built with D-Bus).
- Linyaps package support.

## Install

### Arch Linux

The AUR recipe is prepared as `cn.org.huansoft.ter-music` but is **not submitted yet**; it pins an explicit source commit (`_commit`, temporarily `32737f0`, the current `testing` tip, bumped to the release commit before submission). Build from source below, or use the locally built packages described in [BUILD_GUIDE.md](BUILD_GUIDE.md). The recipe lists `onnxruntime-cpu` as an optional dependency for karaoke; it builds from source, so karaoke uses the system ONNX Runtime instead of the static copy in the binary packages.

### Build from source

Required: CMake 3.10+, a C99 compiler, pkg-config, ncursesw, FFmpeg libraries (avcodec, avformat, avutil, avfilter, swresample, swscale), libpng, libjpeg, libxml2, SQLite3, zlib and libcurl. libcurl is not optional: the build stops with a clear error when it is missing.

Optional: PipeWire, PulseAudio and ALSA output; D-Bus (MPRIS/API). SMB sources additionally need the external `smbclient` program (Samba client) at run time; without it the build still works and SMB connections report that it is missing. Karaoke needs ONNX Runtime: the DEB, RPM and AppImage packages link it into the binary, while a source build (including the AUR package) loads a system shared library with `dlopen` (`libonnxruntime.so.1.21`, `.so.1` or `.so`; CPU execution provider only). The vocal-separation model is bundled by the package builds (see [Build from source](#build-from-source) and [Karaoke](#karaoke)).

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

The binary is `build/ter-music` (or `./ter-music` from the build directory). Locally built packages: one DEB (Debian 10–13 / Ubuntu 20.04–26.04), four RPMs (EL7–10), one AppImage (glibc 2.28) and one AUR package; UAB/layer are not published with this release (deferred until the upstream ll-box fix is released); DEB/RPM source and debug packages are optional. See [BUILD_GUIDE.md](BUILD_GUIDE.md).

When `-DCMAKE_INSTALL_PREFIX` is not given, the default prefix is the Linyaps layout `/opt/apps/cn.org.huansoft.ter-music/files`, so `sudo make install` installs the application under `/opt` (an explicit prefix, such as `/usr`, is respected). The release packages all use that layout; the deb, rpm, AUR and AppImage packages add only symbolic links under `/usr` (Linyaps installs inside its container). See [BUILD_GUIDE.md](BUILD_GUIDE.md).

The vocal-separation model is not built from source: it is a data file injected at build time. The packaging scripts download it into `.tmp/models/` (`aria2c` first, falling back to `curl`/`wget`), verify its size and SHA-256 and reject a damaged file. For a plain source build pass the downloaded file explicitly:

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
  -DTER_MUSIC_BUNDLED_MODEL=/path/to/UVR_MDXNET_3_9662.onnx ..
```

CMake checks the file (29,704,436 bytes, SHA-256 `e02220e8…aadc`) and installs it as `<prefix>/share/ter-music/models/uvr-mdxnet-3-9662.onnx` together with `models/NOTICE.md`. Without that option no model is installed: the player still starts, reports "built-in model missing" when karaoke is at a level other than Off, and plays the original audio. The model is never fetched at run time and is never written to the user's data directory.

## Quick start

```bash
ter-music                        # TUI; starts a core if none runs (Linyaps: D-Bus activation)
ter-music -o ~/Music             # open a folder or file
ter-music -o ftp://host/music/   # open a remote folder in the TUI
```

The TUI needs a real terminal; do not pipe its output. Press `O` to open a folder, `↑`/`↓` to select, `Enter` or `Space` to play, and `q` to quit.

Open folder (`O`), Append folder (`I`), adding a language pack in `F7` and the default music directory in Settings → Default Path all use the same chooser. On a graphical session it opens `zenity` or `kdialog` (`portal` is a preference for the desktop dialog; ter-music does not implement its own D-Bus `FileChooser` client); in a pure TTY it asks for a path instead — an empty line cancels and `~/` is expanded. `TERM_MUSIC_FILE_CHOOSER=portal|zenity|kdialog|tty` forces a backend (an unknown value falls back to auto), and auto falls back to the typed path when there is no `DISPLAY`/`WAYLAND_DISPLAY`. In the AppImage, the AppRun wrapper exports `LD_LIBRARY_PATH` for the bundled libraries; the external dialog (and the external `smbclient`) now runs without `LD_LIBRARY_PATH`/`LD_PRELOAD`/`LD_AUDIT`, so it uses the host's own libraries, and if the dialog still fails to start or exits abnormally (anything except the user cancelling) the chooser falls back to the typed path and shows the reason; cancelling never falls back, and a forced backend that is unavailable still reports an explicit error. Cancelling, or choosing a path that is not an existing directory, changes neither the queue nor the configuration.

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

Mouse: left click selects and activates, the wheel scrolls, and right click opens a context menu. Middle click opens the same menu on terminals that intercept right click. The Equalizer page is an exception: a left click only selects a band and changes no gain, and the wheel changes the selected band's gain by ±1 dB (the pre-amp the same way; the enable row does not respond to the wheel). Menu slide animations compose one frame per movement and degrade or switch off on slow terminals and in Aurora Flow; input is handled without waiting for an animation to finish. Force the tier with `TER_MUSIC_MENU_ANIM=full|reduced|off` (default auto). The layout follows terminal resizes.

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
| `karaoke` | Karaoke level (off, soft, full, 0-2, cycle); the level is the switch, so any level is accepted |
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

`ter-music help <command>` and `ter-music help all` print full details. `--lang <id>` selects the language for this process (the CLI subcommand or the TUI) for one run and is never written to `config.xml`; precedence is `--lang`/`TERM_MUSIC_CLI_LANG`, then `ui_language`, then the locale.

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

The root fix is upstream: PR #295 is merged into linyaps-box `release/2.3` (merge commit `31ddf04`, 2026-11-20) and makes the forwarder consume the bytes it already wrote when `TryAgain` is returned with `k > 0`; it covers ll-box ≤ 2.3.5. No release contains it yet (the latest release is still 2.3.5) and `master` does not carry it, so check that the tag you install contains it and do not assume the next release will. No UAB or layer is published with ter-music 26.4.1.0; UAB artefacts are planned to be published after the upstream fix is released. `TER_MUSIC_TTY_REPAIR_BACKSTOP_MS`, an undocumented tuning variable, was removed with the timer fallback; it only configured that fallback, so setting it now has no effect.

## Playback

| Group | Modes |
| --- | --- |
| Basic (5) | Sequential, Single Repeat, List Repeat, Shuffle Once, Shuffle Repeat |
| Folder (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |
| Album (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |
| Artist (4) | Sequential, Repeat, Shuffle, Shuffle Repeat |

Shortcuts `1`-`5` select Sequential, Single Repeat, List Repeat, Shuffle Repeat and Folder Sequential. Advanced modes need library metadata and are enabled in Settings → Play Mode.

Speed: 0.75x, 1.0x, 1.25x, 1.5x, 2x and 3x, pitch preserved (FFmpeg atempo). Equalizer: 10 bands from 31 Hz to 16 kHz, presets in Settings → Equalizer.

Karaoke is model-based: Off, Faint vocals or No vocals, with the bundled model running in a background thread. There is no feature switch and no model picker; see [Karaoke](#karaoke) for the bundled model, the session segment cache and the limits.

## Karaoke

Karaoke vocal removal uses the ONNX model that every package installs. There is no feature switch, no "enable karaoke" step and no model picker: the `[Karaoke]` button is always in the control pane, and Settings → Playback offers the three levels (Off, Faint vocals, No vocals). The old built-in centre-cancellation DSP has been removed — the ONNX model is the only vocal-removal path.

### The bundled model

Every package installs the model as `<prefix>/share/ter-music/models/uvr-mdxnet-3-9662.onnx` together with `<prefix>/share/ter-music/models/NOTICE.md` (the full MIT notice). The core uses it in place: nothing is downloaded at run time, nothing is copied into the user's data directory, and no configuration key selects it.

Resolution order at run time:

1. `<prefix>/share/ter-music/models/uvr-mdxnet-3-9662.onnx` — installed with the package, used first;
2. `$XDG_DATA_HOME/ter-music/models/uvr-mdxnet-3-9662.onnx` (default `~/.local/share/ter-music/models/…`) — read-only compatibility with models installed by earlier builds; it is never copied or written.

The path is resolved and its size checked on the UI/playback thread (a `stat` — microseconds); the whole-file SHA-256 runs in the background worker before the first separation and is cached per process by path, size and modification time, so opening or switching a track never waits for the 29,704,436-byte hash. A missing file reports "built-in model missing"; a damaged file (wrong size or SHA-256) reports "built-in model verification failed" and does not silently fall back to the other location. In both cases the level can still be set and changed, playback continues as the original, and the status line names the reason in the UI language. Repairing or replacing the file changes its size or modification time, which invalidates the cache, so the new file is verified on its next use; no restart is needed.

ONNX Runtime is built into every release package: the DEB, all four RPMs (including EL7) and the AppImage link a static ONNX Runtime 1.21 built from the Debian pool source, so no system `libonnxruntime` is needed and `readelf -d` lists no `libonnxruntime`. Static linking is capped at GLIBC_2.17, so the same library serves EL7–10 and Debian 10+. A source build without the static library loads `libonnxruntime.so.1.21`, `.so.1` or `.so` with `dlopen`; only the CPU execution provider is used. Without the runtime separation fails and the original keeps playing; nothing switches to an older algorithm.

A source build contains the model only when it is built with `-DTER_MUSIC_BUNDLED_MODEL=<file>`; without that option no model is installed and the runtime reports it as missing (see [Build from source](#build-from-source)). The release packages always include it.

### Supported model

Only one model is supported: **UVR-MDX-NET 3**.

| | |
| --- | --- |
| Display name | UVR-MDX-NET 3 |
| Installed file | `uvr-mdxnet-3-9662.onnx` (upstream file name `UVR_MDXNET_3_9662.onnx`) |
| Official URL | <https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/UVR_MDXNET_3_9662.onnx> |
| Size | 29,704,436 bytes |
| SHA-256 | `e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc` |
| License basis | MIT, per the Ultimate Vocal Remover GUI (UVR) project README, which asks third-party users of its models to credit UVR and its developers; the `TRvlvr/model_repo` repository itself has no separate LICENSE file |

The attribution required by the model is: "Vocal separation model: UVR-MDX-NET 3, from Ultimate Vocal Remover GUI (UVR) by Anjok07, TRvlvr and contributors — MIT, used with credit." ter-music ships the model unmodified as a data file together with this GPL application and the full notice in `models/NOTICE.md`; the file itself stays under the MIT terms stated by the UVR project.

### Levels and playback

| Level | Playback source |
| --- | --- |
| Off | the original track |
| Faint vocals (soft) | 0.4 × original mix + 0.6 × model accompaniment |
| No vocals (full) | the model accompaniment only |

Mono tracks are not separated and keep playing as the original. The level persists as `karaoke_mode` and is changed from the control-bar popup, Settings → Playback, `ter-music karaoke` or `Control.SetKaraoke`; D-Bus and CLI have no "feature off" precondition — every level in 0–2 is accepted (an out-of-range value replies `false`), and `GetKaraoke` returns the current level.

### Cache and background separation

Separation runs on the player core in a low-priority background thread (nice 10); it never runs on the UI or playback thread. At a level other than Off, a stereo track of 30 minutes or less started without a complete legacy cache is separated in the session; once all its segments are ready, replaying it in the same session reuses the result without running the model again. A failed separation is retried the next time the track starts. At Off nothing is separated — the level is the switch. Switching to another level while a track plays requests separation for the current track and switches the source automatically at the current position once the target segment is ready; the position stays continuous and a brief pause at the switch is expected.

- **Session segment cache:** the accompaniment is written under `/tmp/ter-music-karaoke-<uid>-<pid>/<model-id>/<track-hash>/` (directories mode 0700) as 15-second segments — `segNNN.inst.flac` (full) and `segNNN.soft.flac` (soft), each with a `segNNN.ready` marker written last — plus a `manifest.txt` index. One separation produces both stems, so the two levels share a single model run.
- **Lifetime:** the session directory exists only while the player process runs. A normal exit stops the worker and then deletes the whole directory; when a player core starts it removes leftover `/tmp/ter-music-karaoke-<uid>-*` directories whose process no longer exists (crash leftovers). Nothing there survives a restart, so a later session separates the track again unless a legacy whole-track cache is present.
- **Key:** the track's real path, size and modification time plus the model id, model SHA-256, format version, stem version, overlap and soft mix. Moving or re-encoding a file gives it a new key.
- **Legacy whole-track cache (read-only, takes priority):** a complete `inst.flac` + `soft.flac` + `meta.txt` pair written by an earlier build under `$XDG_CACHE_HOME/ter-music/karaoke/<model-id>/<track-hash>/` (default `~/.cache/ter-music/karaoke/…`) is still read. When it matches, the track starts from it immediately and no session segments are written. The key includes the current stem version, so whole-track caches written by a build with a different stem derivation do not match; they are left untouched and the track is separated into the session cache instead. Stale `.part` leftovers under that directory are cleaned at startup, and the 2048 MB LRU setting covers those whole-track caches only. Session segments have a separate budget: 512 MB per session by default (`TER_MUSIC_KARAOKE_SEGMENT_MB` overrides it; `0` or less disables the budget), with the oldest track directories evicted first; the track being played or separated is protected, and each segment write is preceded by a free-space check that keeps a 32 MiB reserve.
- **First play of an uncached track at a level other than Off:** playback waits for the first accompaniment segment before it starts — about 7–10 s on the test machine, with a hard cap of 30 s — and the status line shows a preparation percentage. If the segment does not arrive (timeout or a worker failure such as an unavailable model or cache), the original starts instead; mono tracks and tracks over 30 minutes skip the wait and start as the original immediately, and CUE sub-tracks skip the wait too (original or a matching legacy cache). When playback starts at a non-zero position (resume or speed change), the wait is limited to 8 s.
- **At Off** the original plays and nothing is separated. Choosing a level while a track plays starts separation for the current track and switches automatically when the current segment is ready; the status line first shows a waiting hint and then the switch with the wait time.
- **During playback:** when the next segment is not ready, playback waits up to 8 s at the segment boundary and then continues as the original from that point for the rest of the track (a single switch, not a mid-track hop). A seek to an unprocessed position asks the worker to prioritise that segment and waits up to 8 s; if it is still not ready, playback continues as the original from the seek position. After reaching the end of the track the worker backfills segments skipped by a seek, and the manifest reports completion only when every segment is ready.
- **Changing the level** while a track plays requests separation for the current track and switches the playback source automatically at the current position when the target segment is ready — about 9–15 s for the first switch on the test machine, and about 9 s when the level is enabled at a late position (for example 120 s into the track) because the worker is told to prioritise the current segment. Soft ↔ Full on segments that are already ready switches immediately (under a second) without running the model again. A brief pause at the switch is expected and the position is continuous; this also works while playback is paused (the source switches at the paused position and playback stays paused). If separation fails or nothing becomes ready within 30 s, the original keeps playing and the status line reports the reason, and switching again retries. Recovery is bounded: after the segment boundary falls back to the original, the core re-acquires the accompaniment at most 3 times per level change, so an extremely slow machine can stay on the original — the status line then says it is still the original and can be retried, and never reports a switch that did not happen.
- **Prefetch:** only with `seamless_preload=1`, once the current track's accompaniment is ready (and the level is not Off), the next queue entry is separated in the background; `seamless_preload=0` does not prefetch. CUE sub-tracks are never prefetched.
- Separation uses a fixed overlap of 0.25 and is refused for tracks longer than 30 minutes (the status line reports the failure and the original continues). CUE sub-tracks never use the session segment source: they play the original, or a legacy whole-track cache when one matches the physical file; a level change on a CUE sub-track therefore keeps the original and does not restart playback when no legacy cache matches, while a matching legacy whole-track pair switches immediately — neither case reports a switch. The bundled model file is part of the installation; a legacy whole-track cache left by an earlier build is kept, and to reclaim that space quit the player and delete `~/.cache/ter-music/karaoke/` (or one model's subdirectory) — the `/tmp` session directory is already removed on exit.

### No lyric dependency

There is no lyric dependency and no lyric gate: tracks with timed LRC, plain-text lyrics, embedded lyrics or no lyrics at all take exactly the same model path.

### Limits

- UVR-MDX-NET 3 is a mid/low-size model, not perfect separation: vocals can remain in the accompaniment, and instruments or backing vocals that resemble them can be attenuated. `soft` deliberately keeps 40 % of the original, so it is a compromise, not a clean instrumental.
- Separation is CPU-heavy and, when it succeeds, runs once per track per session; the cost depends on the model, the machine and the track length. On a 6-core Ryzen 5 4500U a 30-second track took about 11 seconds end to end (roughly 1.1–1.5 s per 6-second block) and a 3-minute track roughly a minute; weaker machines are slower. Driven by the worker's ONNX session, the core peaks around 0.8 GiB of RSS (795 MiB measured) and drops back after separation.
- The session segments cost about 15.5 MB per minute of audio (measured on a 44.1 kHz stereo track; a 3-minute track is about 46 MB). That is roughly 30 % more than a legacy whole-track pair (about 12 MB/min measured), because every 15-second segment carries its own FLAC headers plus ready and manifest files. The directory is deleted when the player exits, and while the player runs the session budget above bounds its total size; on systems where `/tmp` is a tmpfs (Fedora/RHEL default) that budget counts against RAM, so lower `TER_MUSIC_KARAOKE_SEGMENT_MB` if space is tight.
- Mono tracks and tracks longer than 30 minutes are not separated and play as the original; CUE sub-tracks never use the session segment source. A separation failure never stops playback and the status line reports the reason in the UI language (messages printed by the ONNX Runtime itself stay English technical text).
- Only ONNX Runtime 1.21 with the CPU execution provider was verified; other versions may load but are not guaranteed. DNNL and other execution providers are deliberately not used.
- If ONNX Runtime or the installed model becomes unavailable while the level is not Off, separation fails and the original keeps playing; nothing silently switches back to an older algorithm. The release packages carry the runtime inside the binary; a source build without the static library needs the system `libonnxruntime`.
- "karaoke" is also the name of a layout preset and of per-character lyric highlighting; neither is related to this feature.

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

### Terminal light/dark adaptation

`F2` → Appearance → **Default Appearance Mode** selects `Auto` (default), `Light` or `Dark`; the choice is stored as `preferences.appearance_mode` in `config.xml` (contract version 10). In `Auto` the TUI asks the terminal for its background colour (OSC 11) before the first frame, then falls back to the terminal's own theme (currently the deepin-terminal adapter, read-only and refreshed at most every 2 s), the XDG system appearance, `COLORFGBG`, a `TERM` hint (`*light*` / `*dark*`) and finally the legacy dark look — a terminal/portal that never answers looks exactly as before (the OSC 11 probe is capped at about 200 ms, the portal read at about 120 ms, and both only run in `Auto`). Terminal replies are removed from the key stream and never reach the UI; only a complete reply (BEL- or ST-terminated) received while an answer to the program's own query is still pending is treated as a reply, so typing the same characters by hand is delivered normally (at most a bounded 80 ms look-ahead in that window). In `Auto` the detection order is OSC 11 (the terminal's actual background, authoritative) → the terminal theme adapter (currently only deepin-terminal; it reads the terminal's own config/colorscheme before the desktop preference) → the XDG desktop portal `org.freedesktop.appearance color-scheme` (the system preference, followed live through `SettingChanged`) → `COLORFGBG` → a `TERM` hint → the legacy dark look; the Settings page shows the source. The terminal theme adapter is enabled only for deepin-terminal; every other terminal is never probed for it and keeps its previous behaviour. Identification uses `TERM_PROGRAM` when it is set; the ancestor process chain is consulted only when `TERM_PROGRAM` is unset. A per-terminal override in the front-end-local `~/.config/ter-music/appearance-terminals.json` wins over the global `preferences.appearance_mode` (the global key stays the default and the only value stored in `config.xml`). Built-in presets adapt directly; a user `layouts.json` entry with the same id adapts only for foregrounds that still equal that preset's shipped dark value — values you changed are kept, and `TER_MUSIC_LAYOUTS` stays verbatim.

On a light background the factory theme colours and all seven built-in presets switch to a light-safe palette whose pane colours stay distinct on 256-, 16- and 8-colour terminals; the dark palette is unchanged. Only factory values are substituted: a theme colour you changed is kept. A layout in `~/.config/ter-music/layouts.json` is never rewritten; if its id matches a built-in preset and a pane's explicit foreground still equals that preset's shipped dark value, that foreground follows the light variant. Unchanged factory values in a user preset copy therefore keep adapting (the layout editor shows stored values, Settings shows effective values), while values you changed and `TER_MUSIC_LAYOUTS` stay verbatim. Saving a built-in preset from the layout editor materialises the colours currently on screen and fixes them.

Where the terminal reports focus (`\e[?1004h` is enabled in `Auto` and terminfo defines focus keys), leaving and returning to the window re-detects the background without a restart; terminals that do not report focus can use **Re-detect Terminal Background** in the same page. The Settings colour page names the light-variant indices (Charcoal, Deep Teal, Dark Amber, …) and shows `Auto (light/dark)` for factory-default colours. The detected variant is per front-end process and is never written to `config.xml`; the mode is shared configuration and reaches a running TUI through the usual `ConfigChanged` reload path.

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

The karaoke model ships with the package under `<prefix>/share/ter-music/models/` (`uvr-mdxnet-3-9662.onnx` plus `NOTICE.md`) and is used in place. `$XDG_DATA_HOME/ter-music/models/uvr-mdxnet-3-9662.onnx` (default `~/.local/share/ter-music/models/…`) is only a read-only fallback for models installed by earlier builds and is never written. Legacy whole-track accompaniment caches, when present, live under `$XDG_CACHE_HOME/ter-music/karaoke/<model-id>/<track-hash>/` (default `~/.cache/ter-music/karaoke/`) and are read-only; the 2048 MB LRU setting covers them only. Session segments are written only to `/tmp/ter-music-karaoke-<uid>-<pid>/` (session budget 512 MB by default, `TER_MUSIC_KARAOKE_SEGMENT_MB`) and are deleted when the process exits; see [Karaoke](#karaoke).

Common `config.xml` keys:

| Key | Value |
| --- | --- |
| `audio_backend` | 0 auto, 1 PulseAudio, 2 ALSA, 3 PipeWire |
| `audio_latency_ms` | output latency, 20-250 |
| `volume_percent` | default volume, 0-100 |
| `default_playback_speed` | 0.75-3.0 |
| `default_play_mode` | 0-16 |
| `ui_language` | `zh_CN`, `en_US`, `zh-PQ` or an installed pack id |
| `karaoke_mode` | 0 off, 1 soft, 2 full; default 0; the level is the switch |
| `appearance_mode` | 0 auto (follow terminal), 1 light, 2 dark |
| `resume_last_playback` | resume the last track and position |
| `stop_core_on_exit` | 1 stop the core on exit, 0 keep playing |
| `core_exit_when_no_frontend` | exit 10 s after the last front end leaves |

The configuration contract is version **12**. Version 12 **removed** `preferences.karaoke_feature` and `preferences.karaoke_model`: the model is bundled and there is no feature switch or model selection. The version history, as the repository has it: contract 4 = tag `v2.1.0` (its root attribute was the literal `"2.2"`, which still loads because it parses as 2 and is rewritten once), 7 = resident core, 8 = `stop_core_on_exit`, 9 = the `<keybindings>` overrides **and** `karaoke_mode` (the DSP-era level), 10 = `preferences.appearance_mode`, 12 = bundled ONNX model and removal of the DSP-era karaoke keys. No contract-11 build was ever released, so no released file can contain the two removed elements. A v10 or earlier `config.xml` is migrated on the first start of the new core: the file is rewritten with `version="12"`, and every other key (including `karaoke_mode`, theme, paths, equalizer and key bindings) is kept; files older than v10 also receive the one-shot defaults of the earlier migrations (background 0→-1 below v4, `core_exit_when_no_frontend=0` below v7, `stop_core_on_exit=1` below v8, `appearance_mode=auto` below v10); a second start leaves the file byte-identical. `config get karaoke_feature` and `config set karaoke_feature …` now fail as unknown keys, and `Config.Set` rejects a patch that still contains them. A downgrade is not supported: a pre-v12 (contract 10) build reads a v12 file (the version attribute parses as an integer and its own migrations stay inactive), sees the removed keys as absent — karaoke off — and writes the attribute back to 10 on its next save. During an upgrade, a new front end that meets a still-running pre-v12 core applies the core's payload by key (`api_version` 7 is the only handshake gate); the mirror is rejected as a whole only if the payload carries the deleted keys, which only an unreleased contract-11 development build would send — restart the core (`ter-music daemon restart`) or log in again and it heals.

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

`Info.GetInfo` reports `core.api_version` (currently 7). See [API_DBUS_en_US.md](API_DBUS_en_US.md) and [API_LYRICS_en_US.md](API_LYRICS_en_US.md). `Control.SetKaraoke` accepts every level 0–2 unconditionally (the level is the switch; an out-of-range value replies `false`), `Control.GetKaraoke` returns the current level, and `ter-music karaoke` reads or changes it — see [Karaoke](#karaoke).

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

CI runs deterministic gates on every change, including the following; the full real-model karaoke suite runs in a separate advisory job that does not block merges, so the release machine and the nightly run are the final quality gate (see [BUILD_GUIDE.md](BUILD_GUIDE.md) §9.5):

| Gate | Rule |
| --- | --- |
| `scripts/test/check-core-purity.sh` | Core has no content or remote-source symbols |
| `scripts/test/check-ui-purity.sh` | UI reaches playback only through the `player` facade |
| `scripts/test/check-config-ownership.sh` | Core is the only writer of `config.xml` |
| `scripts/test/check-package-identity.sh` | App id and version agree across packages |
| `scripts/test/check-c-strings.sh` | No hard-coded Chinese literals in C code; strings go through i18n keys |
| `scripts/test/package-layout-check.sh` (`TM_LAYOUT_STATIC_ONLY=1`) | `/opt/apps` prefix, `/usr` links, model constants and wiring, without building packages |
| no-D-Bus build | The optional D-Bus build compiles, links and starts |

## Troubleshooting

- **No sound:** check that PipeWire, PulseAudio or ALSA is running; the backend is probed in that order. An explicitly chosen backend falls back to the next working one at play time. Switch it in Settings → Audio.
- **Stutter or crackle:** raise Output Latency in Settings (20, 40, 60, 80, 100, 120, 150, 200 or 250 ms).
- **Garbled CJK:** use a UTF-8 locale and a UTF-8 terminal; `locale` should show `LC_CTYPE=UTF-8`.
- **No file dialog appears in the AppImage:** the bundled dialog falls back to typed path input automatically when it cannot start; if you still see nothing, check the terminal log (`GUI dialog failed` / `falling back to TTY`), install `zenity` or `kdialog` on the host, or force the backend with `TERM_MUSIC_FILE_CHOOSER=zenity|kdialog|tty`.
- **Duplicated or garbled TUI lines inside Linyaps:** the ll-box ≤ 2.3.5 cold-start replay described under "Linyaps" above. The guard repaints the screen when it sees burst or back-pressure evidence; with no evidence it no longer repaints on a timer, so the damage may stay until a terminal resize, a view change or new evidence.
- **Missing headers at build time:** install the `-devel`/`-dev` variants of the dependencies above.
- **A remote folder will not list:** check protocol, host, port and base path (SMB needs `share/subpath`); install `smbclient` for SMB; create a readable known_hosts file for SFTP; turn on TLS for an https/webdavs/ftps server. The page shows the localized reason and the connection menu offers "Retry listing".
- **Remote tracks failed to download:** the page shows `failed n` and the first error; fix the cause (network, permissions, disk space) and use "Retry failed tracks (n)" — an already cached track is not downloaded again. A file over 1 GiB or a full cache stops at that track; raise the cache limit or clean the cache in Remote Devices.
- **A remote folder stopped after many tracks:** one open downloads at most 200 tracks; browse and open subfolders separately.
- **A remote URL pasted from the folder history fails to authenticate:** stored URLs are redacted (no password by design); open the saved connection in Remote Devices instead.
- **CUE tracks missing:** the `.cue` file must share the audio file basename; change CUE Encoding in Settings if the text is garbled.
- **Library is empty:** open the library browser (`M`) and scan or rescan; the database is `library.db` in the config directory.
- **The status line says the built-in model is unavailable:** the package should have installed `<prefix>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`. A source build without `-DTER_MUSIC_BUNDLED_MODEL` installs no model; a damaged file fails the size/SHA-256 check. The release packages carry ONNX Runtime inside the binary; a source build also needs the system library (`libonnxruntime.so.1.21`, `.so.1` or `.so`). The status line names the reason in the UI language (missing, verification failed, runtime unavailable), the level can still be set, and playback continues as the original. The size is checked when the path is opened and the whole-file SHA-256 is verified in the background; repairing or replacing the file invalidates the cached result, so no restart is needed (a source build that needs the system library must have it installed). There is no fallback to an older built-in algorithm.
- **The first play waits, or falls back to the original:** at a level other than Off a track without a cache waits for the first accompaniment segment (about 7–10 s, at most 30 s in total; 8 s when playback starts at a non-zero position) while the status line shows preparation progress. If the wait times out or the separation fails because the model or cache is unavailable, the track plays as the original; mono tracks, tracks over 30 minutes and CUE sub-tracks skip the wait and start as the original immediately (a matching legacy whole-track cache is still used).
- **Karaoke separation always fails:** tracks longer than 30 minutes are refused, mono tracks and CUE sub-tracks are not separated, and a missing or damaged bundled model or a missing ONNX Runtime fails; the status line names the reason. See [Karaoke](#karaoke).

## License

GPL-3.0-only. See [LICENSE](../LICENSE).

The bundled UVR-MDX-NET 3 model is distributed under MIT (see [models/NOTICE.md](../resources/models/NOTICE.md)); the release binaries link a static FFmpeg build under LGPL-2.1-or-later (configured without `--enable-gpl`) and, in the official packages, ONNX Runtime under MIT; `debian/copyright` carries the full notices and the source locations.

Ter-Music is a playback tool only: it does not provide, host or distribute audio content. You are responsible for the files you play.

## Author

HuanSoft (浣软科技) · <yxzl666xx@outlook.com>

## Acknowledgments

- **@guanzi008** — Debian packaging, MPRIS, UTF-8 input, mouse and UI fixes.
- **@Zeta** — Arch Linux packaging.
- **Ultimate Vocal Remover GUI (UVR)** by Anjok07, TRvlvr and contributors — ter-music ships the unmodified UVR-MDX-NET 3 vocal-separation model as a data file, used with credit under the MIT terms described in [Karaoke](#karaoke).
