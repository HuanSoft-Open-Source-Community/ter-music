<div align="center">

# Ter-Music - Terminal Music Player

![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
[![CI](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml/badge.svg)](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/HuanSoft-Open-Source-Community/ter-music?sort=semver)](https://github.com/HuanSoft-Open-Source-Community/ter-music/releases)
![Docker](https://img.shields.io/badge/Docker-Supported-2496ED.svg)
![Python](https://img.shields.io/badge/Python-3.x-3776AB.svg)
![Shell](https://img.shields.io/badge/Shell-Bash-4EAA25.svg)
![Linyaps](https://img.shields.io/badge/Linyaps-Supported-8A2BE2.svg)

</div>

**Other Languages:**
- [中文（现代版）](translations/README_zh-CN_Modern.md)
- [中文（文言版）](translations/README_zh-CN_Legacy.md)
- [Lyrics API (English)](API_LYRICS_en_US.md)
- [D-Bus Info & Control API (English)](API_DBUS_en_US.md)
- [TUI Layout Reference (English)](UI_LAYOUTS_en_US.md)

## 1. Project Introduction

### 1.1 Core Features

Ter-Music is a lightweight, terminal-based command-line music player designed for Linux systems. It utilizes FFmpeg for audio decoding, supports PipeWire/PulseAudio/ALSA audio output (auto-detected at runtime), and provides a beautiful text-based user interface through ncursesw.

**Key Features:**

- 🌐 **Remote Music Playback**: SMB, SFTP, FTP, WebDAV and HTTP sources. The **front end** lists them and downloads each track into a local cache, then hands the local files to the core (which only ever plays local files)
- 🎵 **Supports Multiple Audio Formats**: MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS, **WV (WavPack)** and other popular formats
- 🎼 **CUE Split-track Support**: CUE sheet parsing for FLAC/APE/WV, with auto-detect encoding (GBK/BIG5/Shift-JIS)
- 📝 **LRC Lyrics Synchronization**: Automatically loads and synchronizes lyrics, highlights the current line and, when the LRC carries timing, lights up each character karaoke-style (per-character tags, line start/end tags, or interpolation between consecutive line timestamps); **embedded lyrics** (FFmpeg/APE) take priority over external .lrc files. Switch between embedded/external sources in lyric seek mode (Ctrl+L → Tab)
- 🎛️ **10-band Graphic Equalizer**: ISO frequencies (31Hz-16kHz) with biquad IIR DSP, ±12dB range, visual bar UI in settings, with **EQ presets** and **soft-clip** support
- 🎤 **Karaoke Mode (vocal removal)**: Three levels — off, soft (a faint centre vocal is kept) and full. Band-limited centre cancellation with a similarity gate: a 4th-order Butterworth crossover (24 dB/oct, 200 Hz / 5 kHz) carves the band out of the mid channel, so bass and the airy top end of the accompaniment are never touched, and a 1 kHz half-band split feeds 15 ms envelope detectors that compare mid against side: where the band lives in the stereo field (decorrelated pads, strings, cymbals) the cancellation fades to zero, so pure accompaniment passages now stay at 0 dB instead of being dragged down; the gate takes the stronger evidence of the two half-bands, so a centred vocal keeps its full removal depth even when accompaniment plays at the same time. Level changes ramp the cancellation coefficient over a 30 ms exponential curve (no hard step) and switch to bit-exact passthrough only once a fade to zero has completed; a level change issued via CLI/D-Bus becomes audible within the output backlog — under about 0.4 s on PipeWire at the default latency (the old build could lag ~3 s there). Stereo only — mono tracks are passed through bit-exactly. Available in the main controls pane (pop-up), Settings → Playback, the CLI (`ter-music karaoke`) and D-Bus (`Control.SetKaraoke`). Not to be confused with the *karaoke-style* per-character lyric highlighting above, or with the `karaoke` layout preset (a lyrics-first arrangement)
- 🎶 **17 Playback Modes**: From basic (Sequential, Single Repeat, List Repeat, Shuffle) to advanced (Folder/Album/Artist-based variants)
- ⚡ **Playback Speed Control**: Supports 0.75x, 1.0x, 1.25x, 1.5x, 2.0x, 3.0x speed adjustment for efficient listening
- 📚 **Music Library**: SQLite-backed music library with FTS5 full-text search, **recursive directory scan**, browse by artist/album/genre, incremental tracking
- 📋 **Play Queue**: Dedicated queue UI with sequence numbers, now-playing indicator and reordering; the queue itself belongs to the front end (the core only executes the path list it is given), and it is rebuilt from your content on the next start
- 🗂️ **Playlist Management**: Supports user-defined creation of multiple playlists
- ❤️ **Favorites Feature**: Bookmark favorite songs for quick access
- 🕒 **Playback History**: Automatically records playback history for easy review
- 📂 **Directory History**: Records recently visited music directories
- 🎨 **Expanded Color Palette**: 24 preset themes + 1 custom slot with paired-color guard; Settings → Appearance lists all six panes (playlist / info / controls / spectrum / lyrics / vinyl) foreground and background separately, so pane colours follow the layout while sidebar / highlight / border stay global theme colours
- 🧩 **Data-driven Layouts**: 7 JSON layout presets expressed as split trees; forced tiling is structural, old flat geometry is auto-fitted instead of rejected; the lyrics and vinyl panes can coexist, and a track without lyrics makes the lyrics pane step aside so the other panes fill its area, while it returns automatically as soon as a track with lyrics is playing — no view switch or terminal resize is needed (a lyrics-only layout keeps the in-pane vinyl fallback)
- 💾 **Persistent Storage**: SQLite-based storage (favorites, history, playlists) with automatic JSON migration from v1
- ⌨️ **Keyboard Shortcuts**: Full keyboard operation, efficient and convenient
- 📊 **Real-time Progress Bar**: Smooth playback progress display and seeking
- 🎨 **Album Cover Display**: Supports album art rendering in terminal (PNG/JPEG as colored half-block character art with automatic 256→8→monochrome fallback; the CLI also supports braille/ASCII/half-block). The controls-pane cover follows the playback master switch; the info-pane cover also has its own switch in Settings → Info Display, and the standalone vinyl pane reuses the current cover as a rotating circular center label
- 🖥️ **CLI Mode & Background Playback**: Full command-line subcommands (`play`/`pause`/`seek`/`volume`/`speed`/`mode`/`karaoke`/`show`/`daemon`) with a detached background playback daemon that keeps playing after you close the terminal — including inside Linyaps packages, where background playback is started through D-Bus activation or the shipped systemd user service
- 📋 **Configurable Info Display**: The main info pane (fields, preset and cover), plus the braille/ASCII/half-block text cover, progress line and two lyric lines (current + next) printed by `ter-music show`, are configurable in the TUI settings
- 🎵 **MPRIS & Lyrics API**: Desktop media controls with album art, an open D-Bus lyrics API, plus a `Info` interface (track / progress / text cover) and a `Control` interface for other programs

### 1.2 Design Philosophy

Ter-Music follows the **simple, efficient, native** design philosophy:

- **Lightweight**: No dependency on heavy desktop environments, extremely low resource usage
- **Terminal Native**: Completely text-based UI, suitable for servers, embedded devices, and users who prefer terminal workflows
- **Modular Design**: Clear module separation, easy to maintain and extend
- **Unix Philosophy**: Do one thing well, work well with other tools
- **No Tracking**: Does not collect any user data, respects privacy
- **Two Planes, One Job Each**: The **core** is a playback service (audio device, transport, volume/speed/mode, and a path queue handed to it by the front end); the **front end** owns the file system and the content (library, scanning, playlists, favorites, history, remote sources, UI). See [5.2.3 Front End and Core](#523-front-end-and-core)

### 1.3 Key Features

| Feature                                      | Description                                 |
| ------------------------------------------ | ------------------------------------------- |
| 🚀 **Low Resource Usage**                     | Memory usage usually < 10MB, extremely low CPU usage  |
| 🎨 **Beautiful TUI**                         | JSON-defined split-tree layouts (7 presets), colored interface, structural forced tiling and terminal size adaptation |
| 🌍 **UTF-8 Chinese Support**                  | Perfect UTF-8 encoding support, correctly displays Chinese song metadata |
| 🔄 **Persistent Storage**                     | SQLite-backed storage — configuration, library, favorites, playlists, and history, all in one database |
| 🎯 **Multiple View Switching**: Quickly switch between settings, history, playlist, library, language and other views via F2-F8 function keys | <br /> |
| ⚡ **Responsive UI**: 100 FPS refresh rate, smooth progress bar updates | <br /> |
| 🔧 **CMake Build**: Modern build system, good cross-platform compatibility | <br /> |
| 🔊 **Audio Backend**: Supports PipeWire, PulseAudio and ALSA output. Auto mode probes each candidate at startup (PipeWire > Pulse > ALSA — service connectable, or for ALSA: the default device really opens and configures); a backend whose probe fails is neither auto-selected nor listed in Settings. If the chosen backend cannot open the stream at play time, the core re-probes the remaining candidates and falls back, announcing the switch in the status line | <br /> |
| 🎛️ **10-band Equalizer**: ISO graphic equalizer with visual bar chart UI in settings | <br /> |
| 🎤 **Karaoke Mode**: Three-level vocal removal (off / soft / full) by band-limited, similarity-gated centre cancellation; stereo only, stereo-field accompaniment kept; smooth ramps on level change; controls pop-up, Settings → Playback, `ter-music karaoke`, `Control.SetKaraoke` | <br /> |
| ⏩ **Playback Speed Control**: 6 levels of speed adjustment (0.75x-3.0x), switchable during playback | <br /> |
| 📊 **Info Pane**: Displays state/mode, current-track metadata, sample rate, bit depth, bitrate and codec | <br /> |
| 🌐 **Remote Playback**: SMB/SFTP/FTP/WebDAV/HTTP sources, browsed and downloaded by the front end into a local cache | <br /> |
| 🎨 **Album Cover**: Colored half-block album art (256→8→mono fallback), toggleable in Settings | <br /> |
| 🎵 **MPRIS / Lyrics API**: Desktop media controls, album art via `mpris:artUrl`, and a JSON lyrics API over D-Bus | <br /> |
| 🖥️ **CLI Mode**: `ter-music play/pause/next/seek/volume/speed/mode/karaoke/show` works against the running instance; `show` prints the configurable info block | <br /> |
| 🌙 **Background Playback**: `ter-music daemon start` runs headless playback; control it from any terminal, no TUI needed | <br /> |
| 🖼️ **Text Cover over D-Bus**: `Info.GetCoverArt` returns braille or ASCII cover art for other applications | <br /> |
| 📦 **Linyaps CLI**: the same CLI runs inside the Linyaps container (`ll-cli run cn.org.huansoft.ter-music -- ter-music …`); on-demand background playback via D-Bus activation, always-on via a systemd user service | <br /> |

### 1.4 Use Cases

- **Servers/Headless Systems**: Play music on servers without a graphical interface
- **Embedded Devices**: Run on resource-limited embedded Linux devices
- **Developers**: Code and listen to music while working in the terminal, no need to switch windows
- **Minimalists**: Users who prefer simple software and don't need complex graphical interfaces
- **Learning Reference**: Excellent example project for learning C programming, FFmpeg, and ncurses

### 1.5 Target Audience

- Advanced Linux users and command-line enthusiasts
- Embedded developers and system administrators
- Users pursuing minimalism
- Users who need to play music in environments without a graphical interface
- Developers learning C programming and multimedia programming

## 2. Build Environment Requirements

### 2.1 Operating System

- **Supported Systems**: Linux kernel 3.10 or higher
- **Recommended Distributions**: Fedora 30+, Ubuntu 20.04+, Arch Linux latest
- **Not Supported**: Windows, macOS (contributions for porting are welcome)

### 2.2 Hardware Requirements

| Component | Minimum Requirements | Recommended |
| ------- | --------------- | --------------- |
| **CPU** | Single-core 1GHz | Dual-core 2GHz or higher |
| **Memory** | 64MB available | 128MB available or higher |
| **Storage** | 200MB available disk space | 1024MB available disk space |
| **Sound Card** | PulseAudio service running | PulseAudio service running |

### 2.3 Compiler Versions

- **GCC**: GCC 7.0 or higher
- **Clang**: Clang 6.0 or higher
- **C Standard**: C99 or higher

### 2.4 Build Tools

- **CMake**: 3.10 or higher
- **Make**: GNU Make 4.0 or higher
- **pkg-config**: 0.29 or higher

## 3. Dependencies and Installation Commands

### 3.1 Required Dependencies

| Dependency Library | Version | Purpose |
| ----------------- | ----- | ------------------------------------------------------- |
| `ffmpeg-free-devel` | 4.0+ | Audio decoding (libavcodec, libavformat, libswresample, libavutil, libavfilter) |
| `libpng` | 1.6+ | Album cover display (PNG format support) |
| `libjpeg` | 6b+ | Album cover display (JPEG format support) |
| `pulseaudio-libs-devel` | 10.0+ | PulseAudio audio output |
| `ncurses-devel` | 6.0+ | Text user interface, wide character support |
| `libcurl-devel` | 7.0+ | Remote music sources (SMB/SFTP/FTP/WebDAV/HTTP) — used by the front end |
| `libxml2-devel` | 2.9+ | XML config file parsing |
| `sqlite-devel` | 3.20+ | Music library database (FTS5 for full-text search) |
| `cmake` | 3.10+ | Build system (required for compilation) |
| `gcc` | 7.0+ | C compiler (required for compilation) |
| `make` | - | Build tool (required for compilation) |
| `pkg-config` | - | Dependency detection (required for compilation) |

**Optional Dependencies:**

| Dependency Library | Purpose |
| ----------------- | ------- |
| `pipewire-0.3-devel` | PipeWire audio backend (dlopen-based — optional at compile time, auto-detected at runtime) |
| `alsa-lib-devel` | ALSA audio output backend |
| `dbus-devel` | MPRIS D-Bus media session, album art, and lyrics API integration |

### 3.2 Fedora / RHEL / CentOS

```bash
sudo dnf install cmake gcc make pkg-config
sudo dnf install ffmpeg-free-devel libpng-devel libjpeg-turbo-devel pulseaudio-libs-devel ncurses-devel libcurl-devel libxml2-devel sqlite-devel
# Optional backends
sudo dnf install pipewire-devel alsa-lib-devel dbus-devel
```

### 3.3 Ubuntu / Debian / Linux Mint

```bash
sudo apt update
sudo apt install cmake gcc make pkg-config
sudo apt install libavcodec-dev libavformat-dev libswresample-dev libswscale-dev libavutil-dev libavfilter-dev libpng-dev libjpeg-dev libpulse-dev libncursesw5-dev libcurl4-openssl-dev libxml2-dev libsqlite3-dev
# Optional backends
sudo apt install libpipewire-0.3-dev libasound2-dev libdbus-1-dev
```

**Note**: If you can't find the ffmpeg development packages, you may need to enable the universe repository first:

```bash
sudo add-apt-repository universe
sudo apt update
```

### 3.4 Arch Linux

**Install from AUR (Recommended):**

```bash
# Using yay (AUR helper)
yay -S cn.org.huansoft.ter-music

# Using paru (AUR helper)
paru -S cn.org.huansoft.ter-music
```

**Install using ZPM (MengXi OS Package Manager):**

```bash
# First, install ZPM if not already installed
git clone https://aur.archlinux.org/zetapm.git
cd zetapm
makepkg -si

# Then install cn.org.huansoft.ter-music using ZPM
zpm -S cn.org.huansoft.ter-music
```

**Manual Installation from AUR:**

```bash
git clone https://aur.archlinux.org/cn.org.huansoft.ter-music.git
cd cn.org.huansoft.ter-music
makepkg -si
```

> The Arch package was renamed from `ter-music-cn` in the organization
> migration. The AUR recipe is kept in this repository, but the commands above
> only work once the AUR project itself has been created or renamed by a
> maintainer; until then use the manual build below with the in-repository
> `PKGBUILD`.

**Manual Build from Source:**

```bash
sudo pacman -S cmake gcc make pkg-config
sudo pacman -S ffmpeg libpng libjpeg pulseaudio ncurses libcurl libxml2 sqlite
# Optional backends
sudo pacman -S pipewire alsa-lib dbus
```

## 4. Compilation Steps

### 4.1 Get Source Code

```bash
git clone https://github.com/HuanSoft-Open-Source-Community/ter-music.git
cd ter-music
```

### 4.2 Create Build Directory

```bash
mkdir build
cd build
```

### 4.3 Configure CMake

```bash
cmake ..
```

CMake will automatically detect all dependencies in your system. If any dependencies are missing, it will display a clear error message.

**Optional CMake Parameters:**

```bash
# Custom installation prefix (default: /usr/local)
cmake .. -DCMAKE_INSTALL_PREFIX=/usr

# Enable debug compilation
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Enable compilation optimizations
cmake .. -DCMAKE_BUILD_TYPE=Release
```

### 4.4 Compile

```bash
make -j$(nproc)
```

`-j$(nproc)` will use all available CPU cores for parallel compilation, speeding up the build process.

### 4.5 Install (Optional)

```bash
sudo make install
```

After installation, you can launch the program by simply typing `ter-music` in your terminal.

### 4.6 Uninstall (if installed)

```bash
cd build
sudo make uninstall
```

### 4.7 Clean Build Files

```bash
cd build
make clean
# Or completely remove the build directory
rm -rf build
```

### 4.8 Common Compilation Issues

**Issue 1: Cannot find PulseAudio library**

```
Solution: Install pulseaudio-libs-devel (Fedora) or libpulse-dev (Ubuntu)
```

**Issue 2: Cannot find ncursesw library**

```
Solution: Install ncurses-devel (Fedora) or libncursesw5-dev (Ubuntu)
```

**Issue 3: Cannot find ffmpeg header files**

```
Solution: Install ffmpeg-devel (Fedora) or libavcodec-dev libavformat-dev libswresample-dev libavutil-dev libavfilter-dev (Ubuntu)
Note: Also ensure libavfilter-dev is installed for audio filter support
```

### 4.9 Using Build Scripts

Ter-Music provides multiple build scripts for creating packages in different formats, as well as a test server tool for verifying remote playback functionality. For detailed usage instructions, please refer to:

- [Build Guide](BUILD_GUIDE.md) - Detailed build and packaging guide

The following formats are supported:
- **AppImage** - Universal Linux package format
- **Portable Package** - Self-contained archive with all dependencies
- **RPM Package** - For Fedora/RHEL-based distributions
- **DEB Package** - For Debian/Ubuntu-based distributions
- **Linyaps Package** - For deepin/UOS systems
- **Arch Linux Package** - For Arch Linux and derivatives

> **Non-x86 Architectures (arm64, loong64, etc.):** Packages for non-x86 architectures are built and maintained via the OBS build server. See [OBS Build Server](https://obs22.odata.cc/package/show/home:Admin:app/ter-music).

**Test Server Tools:**
- **tools/start-server.py** - Interactive script to start local SMB/FTP/SFTP/WebDAV/HTTP servers for testing remote playback feature.
  > This Python script should be run in a Conda environment. Setup: `conda create -n ter-music python=3 && conda activate ter-music && pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r tools/requirements.txt` then `python3 tools/start-server.py`
  > **CLI mode also available:** `python3 tools/start-server.py --protocol http --port 8080 --path /music/share` or `python3 tools/start-server.py --protocol sftp --port 2222 --username test --sftp-authorized-keys ~/.ssh/authorized_keys`

## 5. Usage

### 5.1 Launch the Program

**If installed:**

```bash
ter-music
```

**If not installed, run directly from build directory:**

```bash
cd build
./ter-music
```

### 5.2 Command Line Arguments

Running `ter-music` without a command starts the TUI (unchanged behaviour):

```bash
ter-music [OPTIONS]

Options:
  -o, --open <path>    Open specified music directory directly on startup
  -d, --debug          Enable debug logging (outputs to ter-music-debug.log)
  --frontend <mode>    Playback path: remote (default, talk to the core over D-Bus)
                       or local (in-process playback, kept as a regression baseline)
  --attach-only        Never start a core automatically: exit with code 3 when none runs
  --bus <name>         Address a specific core/instance bus name (default: the primary one)
  --lang <lang-id>     Use this language for this run (e.g. zh_CN, en_US, zh-PQ); not saved to config
  -h, --help           Show help information
  -v, --version        Show version information
  tui [path]           Explicitly start the TUI
```

**Examples:**

```bash
# Open my music folder on startup
ter-music -o ~/Music

# Open a remote FTP music directory
ter-music ftp://user:pass@host/path/to/music

# Open a remote SFTP directory
ter-music sftp://host/path

# Open a WebDAV music directory
ter-music --open http://webdav-server/music

# Show help
ter-music --help
```

#### 5.2.1 CLI Mode

Any of the following first arguments switches to CLI mode. CLI commands are
thin D-Bus clients: they talk to the playback core that currently owns
`org.mpris.MediaPlayer2.ter_music` (started by `daemon start` or by the TUI;
the Linyaps package can also start it on demand through D-Bus activation, which
other package formats do not ship), so they work from any terminal, script or
window manager shortcut. `play` additionally acts as a front end: it scans the
path in its own process and delivers the resulting queue to the core.

| Command | Description |
| ------- | ----------- |
| `play [PATH] [--index N] [--mode MODE] [--no-daemon] [--foreground] [--quiet]` | Scan the path, deliver the queue to the core and play; starts the core when none is running (`--foreground` runs as the foreground core, `--quiet` is kept for compatibility) |
| `pause` / `resume` / `toggle` / `stop` / `next` / `prev` | Basic transport control |
| `seek <+SECONDS\|-SECONDS\|mm:ss\|N%>` | Relative, absolute or percentage seek |
| `volume [0-100\|+N\|-N]` | Query or set volume |
| `speed [0.5-3.0]` | Query or set playback speed |
| `mode [NAME\|0-16]` | Query or set the play mode (stable names such as `list_repeat`, `folder_shuffle_repeat`) |
| `karaoke [off\|soft\|full\|0-2\|cycle]` | Query or set the karaoke vocal-removal level (`cycle` steps 0→1→2→0); stereo only; the level is echoed back immediately and applied along a 30 ms ramp, stereo-spread accompaniment being gated out |
| `show [OPTIONS]` | Print the current info block (basic info / text cover / progress / two lyric lines) |
| `queue list [--offset N] [--count N]` / `queue clear` / `queue shuffle` / `queue remove <pos>` / `queue append <path>[,<path>...]` / `queue insert <pos> <path>` | Inspect or edit the live queue held by the core |
| `config get [KEY]` / `config set <KEY> <VALUE>` / `config reload` / `config reset --yes` | Read or change the core configuration (keys are `config.xml` names; `reset` requires `--yes`) |
| `lyrics [--lines N]` | Current lyric line context, or up to N timestamped lines |
| `track` | Human-readable metadata of the current track |
| `cover [--width N] [--height N] [--charset braille\|ascii\|half]` | Text cover of the current track |
| `keys list\|check [--json]` | Read-only key-binding audit; does not need a running core |
| `daemon start\|foreground\|stop\|restart\|status\|reload` | Playback core management (`daemon stop --all` also stops `--force` secondary instances) |
| `version` / `help` | Version / help (`help all` prints the complete help) |

`show` options (all of them override the stored TUI settings for that call):

| Option | Description |
| ------ | ----------- |
| `--json` | Print the full JSON snapshot (`Info.GetInfo`) |
| `--watch[=MS]` | Live refreshing view (default 500 ms, `Ctrl+C` to exit, requires a TTY) |
| `--one-line` | Single-line output, suitable for status bars |
| `--full` / `--compact` / `--preset full\|compact\|custom` | Info display preset |
| `--fields a,b,c` | Basic info fields: `state,mode,index,queue,title,artist,album,format,path,volume,speed` |
| `--cover` / `--no-cover`, `--cover-size WxH`, `--charset braille\|ascii\|half` | Text cover options (4-40 columns, 2-20 rows) |
| `--progress bar\|time\|percent\|time+percent`, `--no-progress` | Progress line style |
| `--lyrics 0\|1\|2` | Lyric lines: off / current line / current + next line |
| `--width N` | Output width (defaults to the terminal width) |
| `--bus NAME` | Target a specific instance bus name (defaults to the primary instance) |

**Exit codes:** `0` success; `1` usage error (including an unknown help
target); `3` no running instance; `4` D-Bus unavailable; `5` rejected by the
instance.

**Help and paging:** `ter-music help`, `--help` and `-h` print a short
overview. `ter-music help <command>` — or `<command> --help` for the commands
that parse options (`play`, `show`, `daemon`, `queue`, `config`, `lyrics`,
`track`, `cover`, `keys`) — prints first-level details;
`ter-music help <command> <subcommand>` and `<command> <subcommand> --help`
print second-level details; `ter-music help all` prints the complete help. An
unknown help target exits with code `1`.

Help output wraps to the terminal width. When stdin and stdout are both TTYs
and the text is taller than the screen, a built-in pager is used:
`Space`/`PageDown` next page, `b`/`PageUp` previous page, `j`/`k` scroll one
line, `g`/`G` jump to the first/last line, `q` quits. Non-TTY output,
`TERM=dumb` and `TER_MUSIC_NO_PAGER=1` print the whole text directly, with no
ANSI escape sequences.

**CLI language:** local help, errors, tables and hints follow `--lang` >
`TER_MUSIC_CLI_LANG` > the configured `ui_language` > `LC_ALL` >
`LC_MESSAGES` > `LANG` > `zh_CN` (see §5.9.1). `--lang` and
`TER_MUSIC_CLI_LANG` may appear anywhere on the command line; `C`, `POSIX` and
an empty locale mean "no preference" and continue with the configuration. An
uninstalled `--lang` ID warns and falls back to `zh_CN`. The bodies printed by
`show`, `track`, `lyrics` and `cover` are rendered by the core's
`ui_language`; `--lang` only changes the CLI's own text.

**Examples:**

```bash
# Start background playback and return to the shell immediately
ter-music play ~/Music

# Play a single file (its directory is loaded as the playlist)
ter-music play ~/Music/album/01.flac

# Current track info: braille cover, progress bar and the current/next lyric lines
ter-music show

# Single-line status for a status bar, refreshed every second
ter-music show --one-line --watch=1000

# Raw JSON for scripts
ter-music show --json | jq -r '.track.title'

# Transport control
ter-music next
ter-music seek +10
ter-music volume +5
ter-music mode shuffle_repeat
ter-music karaoke soft    # keep a faint centre vocal (level 1)
ter-music karaoke full    # remove it as far as possible (level 2), stereo-spread accompaniment untouched

# Background daemon management
ter-music daemon start --open ~/Music
ter-music daemon status
ter-music daemon reload      # re-read config.xml (info display settings, volume, ...)
ter-music daemon stop
```

Notes:

- Without a command, `ter-music <path>` still opens the TUI. To open a
  directory literally named `play`/`show`/..., use `-o ./play` or `ter-music tui play`.
- `daemon start --open <path>` no longer makes the core scan anything: the core
  is started first, then the CLI process (as a front end) scans the directory,
  builds the queue and hands it over — the same path `ter-music play <path>`
  takes. `daemon start` without `--open` starts an idle core whose queue is
  delivered later by whichever front end attaches.
- Only one instance owns the bus name; `daemon start` refuses to start a second
  one while the core is running (use `--force` to start as a secondary instance
  anyway). Front ends — TUI and CLI alike — are clients and can be many at once.
  A secondary instance is never silent: `daemon status` names every one of them
  (pid + bus name) on stderr, `daemon start --force` prints the bus name it just
  created, `daemon stop --all` stops the primary and every secondary in one go,
  and any CLI subcommand can be pointed at one instance by putting `--bus <name>`
  **after** the subcommand (`ter-music daemon stop --bus <name>`).
- A core keeps its bus name until playback has really stopped (the playback
  thread has exited and the backend stream has been destroyed), so a front end
  started right after `q` finds that name still online and does not launch a
  second core — two cores playing the same music at once is what used to happen
  when the name was released before the audio stopped. The flip side is that
  `daemon stop`/`restart` may see the instance disappear up to roughly the
  playback-thread shutdown timeout (2s) plus the stream teardown later than
  before; the CLI's own wait budgets (5s/10s) still cover it.
- `daemon stop` refuses to terminate a running TUI unless `--force` is given.
- The core also exits by itself when its session bus goes away: with no bus no
  front end can reach it any more, and a headless player that keeps holding the
  audio device would be impossible to stop. A core that never had a bus (started
  by a supervisor without a session bus) is unaffected.

#### 5.2.2 Linyaps (Linglong) Package Environment

When ter-music is installed as a Linyaps (如意玲珑) package, the binaries live
inside the application container and no host-wide `ter-music` command exists
(Linyaps cannot export executables to `$PATH`). Run CLI commands through
`ll-cli`; every invocation joins the same application container, so the CLI,
the TUI and the playback daemon share one session bus, one configuration
directory and one set of D-Bus interfaces.

```bash
# Any CLI command
ll-cli run cn.org.huansoft.ter-music -- ter-music show
ll-cli run cn.org.huansoft.ter-music -- ter-music pause
ll-cli run cn.org.huansoft.ter-music -- ter-music play ~/Music

# Reading state while a container is already running (see the notes below)
ll-cli enter cn.org.huansoft.ter-music -- /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music show

# Convenience wrapper for interactive shells (put it in ~/.bashrc)
ter-music() { ll-cli run cn.org.huansoft.ter-music -- ter-music "$@"; }
```

**Background playback.** A self-detached process (`ter-music daemon start`)
would be recycled together with the container, so it is disabled inside the
sandbox: `daemon start` and `play` instead ask the session bus to activate the
background player, and fall back to an explanatory message with the exact
commands when activation is unavailable. Three supported ways exist:

| Option | How to use | Behaviour |
| ------ | ---------- | --------- |
| D-Bus activation (on demand) | `ter-music play <path>` or `ter-music daemon start` | The session bus starts `ll-cli run cn.org.huansoft.ter-music -- ter-music daemon foreground --no-autoplay` on the host and the command is forwarded to it |
| systemd user service (always on) | `systemctl --user enable --now cn.org.huansoft.ter-music` (on the host) | The player starts with the session and keeps playing in the background |
| Foreground | `ll-cli run cn.org.huansoft.ter-music -- ter-music play <path> --foreground` | Plays in the foreground; the container lives as long as the command runs (tmux/screen friendly) |

Notes:

- `systemctl --user` is not reachable from inside the container, so the service
  must be enabled from the host shell; `play` and `daemon` print the same hints
  when they detect the sandbox.
- `ter-music daemon stop` works as usual and stops the instance; the container
  is reclaimed afterwards, so `ll-cli ps` no longer lists the application.
- `ll-cli run` maps every failure exit code to `255`; host-side scripts should
  inspect `ter-music show --json` (field `"running"`) instead of relying on
  exit codes.
- When an application container is **already running** (the background daemon, or
  the TUI), `ll-cli run … -- <command>` attaches that command's output to the
  *running container's* stdout, so your terminal stays empty. Read it with
  `journalctl --user -u cn.org.huansoft.ter-music`, or use `ll-cli enter`, which keeps
  the terminal attached:
  `ll-cli enter cn.org.huansoft.ter-music -- /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music show`.
  That entered environment carries no `DBUS_SESSION_BUS_ADDRESS`, so the CLI
  resolves the session bus itself (`$XDG_RUNTIME_DIR/bus`, then
  `/run/user/<uid>/bus`); an explicitly set address always wins.
- Host paths are visible inside the container as-is (`$HOME`, `/tmp`, `/media`),
  and the extracted cover art stays readable by host applications through
  `mpris:artUrl`.
- Settings, library and the playback session are stored in
  `$XDG_CONFIG_HOME/ter-music`. If the Linyaps runtime redirects the XDG
  variables (see the Linyaps FAQ, "where is application data saved"), they land
  in `~/.linglong/cn.org.huansoft.ter-music/…` instead, keeping Linyaps and non-Linyaps
  installations independent.
- `--watch` requires a terminal (it uses ANSI cursor control) — run it directly
  in a terminal instead of piping it.
- The `Info`/`Control` interfaces are ordinary session-bus services, so host
  applications (`gdbus`, media widgets, `busctl`) can read and control the
  Linyaps instance exactly as they do for a regular install.

#### 5.2.3 Front End and Core

Ter-Music is split into two roles that talk over the session bus:

| Role | Who runs it | What it owns |
| ---- | ----------- | ------------ |
| **Core** (playback service) | `ter-music daemon start` / `daemon foreground` | Audio device, playback state and position, transport commands, volume / speed / play mode, the **path queue handed to it by the front end**, current-track info (lyrics, text cover, spectrum), configuration, front-end registry and heartbeat |
| **Front end** (content and file system) | The TUI (`ter-music`, `ter-music tui`), the CLI (`ter-music play/show/…`), other clients | Music library (SQLite), scanning and metadata, playlist content, user playlists, favorites / history / directory history, sorting / filtering / search, remote sources and their download cache, the UI |

The core never scans a directory, never keeps a library, and never resolves a
remote URL: it plays **local paths**, listed in the order the front end sends
them. The front end never opens an audio device; it renders state it reads back
over D-Bus and hands over content.

**Starting up.** Running the TUI with no core around starts one automatically and
pushes the content (the opened directory, or the restored session) into it, so
the everyday workflow is unchanged:

```bash
ter-music                 # TUI starts (and starts a core if none is running)
ter-music -o ~/Music      # same, opening a directory first
```

Use `--attach-only` when a core must already exist — useful in scripts, and in
sessions where playback should not be started behind your back:

```bash
ter-music --attach-only   # exit code 3 when no core is running
ter-music --bus org.mpris.MediaPlayer2.ter_music.instance12345   # address a specific instance
```

**Leaving the front end.** Whether closing the TUI stops playback is decided by
`preferences.stop_core_on_exit` **in the front end's own configuration mirror**
(`0`/`1`, default `1`; toggle it on the settings page, in `config.xml`, or with
`Config.Set` / `ter-music config set` over D-Bus): the mirror is read from disk
at start-up, and the settings page pushes every change to the core right away, so
the exit path itself never stops to ask the core. With the default, every
front-end exit path (`q`, F9, Ctrl+C, SIGTERM, a lost TTY) sends `Control.Quit`:
the core persists its session/resume state, stops playback and exits. With `0`
the front end only sends `Control.Detach` — the core keeps running and playing,
and `ter-music show` / `ter-music next` still reach it from any terminal. Either
message is sent only when this front end really did attach to the core
(handshake completed and an `Attach` token held); a front end that never got that
far exits without touching the bus and leaves somebody else's core alone.
Independent of that, set
`core_exit_when_no_frontend` to `true` in `config.xml` (or with `Config.Set`
over D-Bus) when the core should exit by itself after the last front end has
been gone for a grace period (10 s); a core that no front end ever attached to
stays up.

**Reconnect.** When the core disappears (crash, `kill`, logout), the front end
does not exit: it marks itself offline, retries with exponential backoff
(1/2/4/8/15/30 s), and re-attaches — and re-sends its content queue — as soon as
a core answers again. Pressing `R` in the TUI restarts a dead core and re-pushes
the queue immediately.

**Compatibility.** The bus surface is `api_version 7`. Version 7 only moved the
interface namespace from `org.yxzl.ter_music.*` to `cn.org.huansoft.ter_music.*`,
and that is breaking rather than additive: a core and its clients must be
upgraded together, since neither can address the other across the rename. The
methods themselves are the surface that grew additively over versions 4 and 5
(version 6 added the karaoke controls). Content interfaces
(`Playlist`, `Library`, `Favorites`, `History`, `DirHistory`, `Remote`) are
gone: they belong to the front end. Third-party clients should use the
`Lyrics`, `Info`, `Control`, `Queue` and `Config` interfaces — see
[API_DBUS_en_US.md](API_DBUS_en_US.md).

### 5.3 Interface Layout

The main view is **data-driven**: every layout is a binary split tree whose
leaves name pane kinds. A split cuts its rectangle into two children, so every
layout is a forced tiling of the whole main area (the terminal minus the bottom
menu bar) — no gaps, no overlaps, and no blank cells. Ratios are per-mille, so a
layout keeps its proportions when the terminal is resized. Old flat-rectangle
files are still read and auto-fitted instead of being rejected.

Six pane kinds are available:

- `playlist`: the song list / play queue (Tab switches views)
- `info`: playback state, current-track metadata, audio parameters and the colored half-block character cover (256→8→mono fallback); the field set follows Settings → Info Display, rows compact when fields are disabled, and the whole block is vertically centred in the pane
- `controls`: progress bar and transport buttons
- `spectrum`: the audio-reactive bar spectrum
- `lyrics`: the current lyric lines
- `vinyl`: the colored half-block vinyl pane — dark concentric-groove disc, rotating album-cover center label and colored spectrum bars; the label follows the cover's 256→8 color fallback, and the whole animation falls back to the previous glyph animation when colors or the color-pair budget are unavailable (split out of the lyrics pane)

The vinyl pane and the lyrics pane have a deliberate fallback relationship:

- `lyrics` and `vinyl` may coexist in the same layout. When the current track has
  lyrics both panes render (lyric lines + the disc in its own pane); when it has
  none, the **lyrics pane steps aside** for that track — its leaf leaves the
  resolved tree, the other panes take over the area, and it returns automatically
  with the next track that has lyrics, without a view switch or a terminal
  resize. The stored layout and all colour settings are unchanged.
- A `lyrics`-only layout still falls back to the vinyl animation inside the lyrics
  pane whenever the current track has no lyrics.
- A `vinyl`-only layout shows the vinyl pane whether or not the track has lyrics.

The vinyl artwork is drawn with colored half-block cells when the terminal supports
colors and has enough color pairs: the disc grooves use a fixed dark palette, the
spectrum bars use a level-based green/yellow/red ramp, and the center label is the
current album cover cropped to a circle and rotated with the disc (one revolution
per five seconds). If the cover cannot be loaded the colored disc and bars are
still drawn with a plain center. On a monochrome terminal, an ASCII UI, or a
terminal whose color-pair budget is below the colored-vinyl threshold, the pane
keeps the original glyph animation and the info/controls covers keep their
previous pair budget.

**Choosing a layout:** `F2` → `Appearance` → `UI Layout` (`←`/`→` cycles,
`Enter` opens the full list). `Reload Layouts` re-reads the layout files. The
choice is stored in `~/.config/ter-music/ui-layout`; it is a front-end local
preference, not part of the core configuration and not sent over D-Bus.

**Layout files:**

- built-in presets: `<prefix>/share/ter-music/layouts/layouts.json` (source tree: `data/layouts/layouts.json`)
- user overrides: `~/.config/ter-music/layouts.json` (same `id` overrides the built-in preset)
- development override: `TER_MUSIC_LAYOUTS=/path/to/layouts.json`

The seven built-in presets are `classic`, `vinyl`, `karaoke`, `visualizer`,
`focus`, `stage` and `neon`. `classic` is the default and keeps the familiar
five-pane arrangement; every preset colours each pane differently (transparent
backgrounds), and `neon` is the brightest of them. Geometry,
colours, unknown panes, gaps, overlaps and non-guillotine old layouts are
auto-fitted to a forced tiling instead of being rejected; the reload message
reports how many layouts were adjusted.
Pane content, including the info-pane album cover, is clipped to the inside of
its border (the border cells are never overwritten); a pane too small for its
renderer shows only its background and border. The info pane adapts to its own
size: wide panes keep metadata and audio parameters side by side with the cover
on the right, while narrow-and-tall panes stack the cover above the metadata and
audio blocks; the cover is hidden when it cannot keep a blank column before the
border. These rows are driven by Settings → Info Display: the pane uses the
fields it can display, compacts when fields are disabled, and drops the audio
block when `format` is off.

The layout editor (`F2 → Appearance → Edit Layout / New Layout`) edits the split
tree directly: arrows select by geometry, Tab cycles, `[`/`]` (and PgUp/PgDn)
adjust the width, `{`/`}` adjust the height, `|`/`-` split, `x` removes, `t`
changes type, `c` edits colours,
`w` swaps, `Enter` opens the action menu, `?` shows help, `S` saves, and
`Esc`/`q` closes. Every operation preserves forced tiling; there is no
save-time geometry rejection.

See [UI_LAYOUTS_en_US.md](UI_LAYOUTS_en_US.md) for the complete schema, colour
rules and a custom-layout example.

### 5.4 Basic Operations

#### Focus Switching

| Key | Function |
| --- | -------- |
| `C` | Switch focus to control area |
| `L` | Switch focus to list area |
| `Ctrl+L` | Toggle lyric seek mode (then ↑/↓ to navigate lyrics) |
| `Tab` / `Shift+Tab` | Toggle between file browser and playback queue views |

- **Note**: Press `Ctrl+L` to enter lyric seek mode; then use `↑`/`↓` to navigate lyrics and `Enter`/`Space` to jump to the selected line's playback position. Press `Ctrl+L` again to exit. Works from any focus mode.

#### List Area Operations (Focus on Playlist)

| Key | Function |
| ----------------- | ------------------ |
| `↑` / `↓` or `j` / `k` | Select song up/down |
| `Space` / `Enter` | Play selected song |
| `O` / `o` | Open new music folder |
| `F` / `f` | Add selected song to favorites |
| `a` | Append selected song to playback queue |
| `A` | Add selected song to a custom playlist (opens selection popup) |
| `i` | Insert selected song as next track in play queue |
| `I` | Append a folder's music to the current playlist |
| `d` | Remove selected track from play queue |
| `D` | Clear the entire play queue |
| `J` | Move selected track down in play queue (reorder) |
| `K` | Move selected track up in play queue (reorder) |
| `S` or `/` | Activate search functionality, supports pinyin search for song titles and artists |
| `M` | Toggle music library browser (browse by artist/album/genre) |
| `Tab` / `Shift+Tab` | Toggle between file browser and playback queue (both keys do the same) |
| `n` | Next track |
| `p` | Previous track |
| `h` | Show playback history popup (last 10 tracks) |
| `1`-`5` | Quick set play mode: 1=Sequential, 2=Single Repeat, 3=List Repeat, 4=Shuffle Repeat, 5=Folder Sequential |

> **Note:** Library browser keyboard navigation (Up/Down/Enter for drill-down) is currently available for mouse interaction; keyboard control within library sub-views is limited. Use `M` to toggle, `Esc` to exit.

**Queue Operations (in either file browser or queue view):**

| Key | Function |
| -------- | ----------------- |
| `a` | Append selected track to end of queue |
| `i` | Insert selected track to play next (after current position) |
| `d` | Remove selected track from queue |
| `D` | Clear the entire play queue |
| `J` | Move selected track down in queue |
| `K` | Move selected track up in queue |
| `Enter` | Play selected track from queue position |
| `Tab` | Toggle to queue view to see full ordered list |

#### Control Area Operations (Focus on Control Bar)

| Key | Function |
| --------- | --------------- |
| `←` / `→` | Select control button left/right |
| `Space` | Activate currently selected button |
| `,` (comma) | Seek backward 5 seconds |
| `.` (period) | Seek forward 5 seconds |
| `Ctrl+L` | Toggle lyric seek mode |
| `-` / `_` | Decrease volume |
| `=` / `+` | Increase volume |

**Control Button Description:**

| Button | Function |
| ------------ | ------------------------------------ |
| `<<` | Previous track |
| `Play/Pause` | Play/Pause |
| `>>` | Next track |
| `Stop` | Stop playback |
| `Mode` | Cycle play mode (opens popup selection — press ENTER to choose from 17 modes) |
| `Speed` | Playback speed (opens popup — press ENTER to select: 0.75x → 1.0x → 1.25x → 1.5x → 2.0x → 3.0x) |
| `Progress` | Progress bar (shows current playback progress) |
| `Volume` | Volume control (opens popup slider, shows current volume percentage) |

#### Lyric Seek Operations

| Key | Function |
| ----------------------------- | --------------- |
| `↑` / `↓` | Select lyrics line up/down |
| `Ctrl+L` or `Enter`/`Space` | Exit lyric seek / jump to selected position |

#### Function Keys (Globally Available)

**Function Keys (F1-F9)**

| Key | Function |
| ----------------- | ------------------ |
| `F1` | Return to main interface |
| `F2` | Open settings view |
| `F3` | Open playback history view |
| `F4` | Open playlist management view |
| `F5` | Open favorites view |
| `F6` | Open about view |
| `F7` | Language selection (opens language view) |
| `F8` | Help (this page) |
| `F9` | Quit |

**Alternative Number Keys (Enter within 3 seconds after Esc)**

| Key | Function |
| ----------------- | ------------------ |
| `Esc` + `1` | Return to main interface |
| `Esc` + `2` | Open settings view |
| `Esc` + `3` | Open playback history view |
| `Esc` + `4` | Open playlist management view |
| `Esc` + `5` | Open favorites view |
| `Esc` + `6` | Open about view |
| `Esc` + `7` | Language selection (opens language view) |
| `Esc` + `8` | Help (this page) |
| `Esc` + `9` | Quit |
| `q` | Exit program |

#### Mouse Operations

The whole interface listens to the left and right mouse buttons and the wheel.
One mouse event belongs to exactly one owner — either the popup that is currently
open or the pane the pointer sits in (see "Popups and menus" below). No event is
ever handled twice.

| Where | Click | Wheel (one notch) |
| ----- | ----- | ----------------- |
| Bottom function-key bar | Fires that key's action, exactly as pressing the key | Nothing (the event is still consumed here) |
| Playlist pane | Select the row; a click plays that track, or toggles a directory in the file tree, just like `Enter` | Move the selection one row up/down — works on search results, the play queue and the file tree alike, and never starts playback |
| Library browser (it replaces the playlist pane while open, `M`) | Select the entry and run it (drill down); `[S]`/`[R]` lines trigger scan/rescan | Move the selection (same as `↑`/`↓`) |
| Controls pane | Press the button under the cursor. The Volume button and the progress row only take the focus, like the keyboard | Progress row: seek ∓5 s; Volume button: ±5 %; Speed button: step through the speed presets (wraps at both ends); other buttons — the Karaoke one included, it is pop-up only —: nothing |
| Lyrics pane | Jump to the line under the cursor | Seek to the previous / next lyric line (it does not switch lyric seek mode on or off) |
| Info, spectrum and vinyl panes | Nothing defined there (the event is consumed, so it never leaks into another pane) | Nothing (consumed) |
| Sub-view sidebar (Settings, History, Playlist, Favorites, Info, Help, Language) | Select the menu entry and run it (same as `Enter`) | Move the sidebar selection |
| Sub-view content area | Select that row and run it (same as `Enter`) | Move the content selection. On the Settings page the wheel follows the section you are in: option list, hotkey list, remote-device list; on the Equalizer page it moves **between bands only** — the wheel never changes a gain value; on the Help page it scrolls the document one line |

A left click is always one action: it selects the row and runs the same action
as `Enter`. A double or triple click carries no extra meaning; ncurses coalesces
it into one event and the program treats it as that single click, so it can never
fire twice. Right clicks are still selection-only and are described below.

The selection highlight follows the pointer: scrolling or clicking in the content
column also moves the keyboard focus there, so what you see highlighted is what
the next `Enter` will act on. Two exceptions have nothing to move — the About
view's content area is read-only text, and the Language view's sidebar holds a
single "back" entry; there the wheel is only claimed.

Right clicks are not listed in the table above: they select the row under the
pointer (never play it) and open the context menu described below. The main-view
info pane has no right-click menu; right clicks on the controls, lyrics, spectrum,
vinyl, info pane and hint-bar regions are consumed without opening anything.

**Popups and menus (the case that used to double-fire).** While a popup is open —
the control-bar popups (play mode, speed, volume), the settings option picker
(including colour and Equalizer presets, and the multi-select info-field picker),
or the layout editor's modal menus — the popup owns every mouse event:

- the wheel scrolls the popup, wherever the pointer happens to be;
- a click inside selects that entry and runs it immediately, exactly like `Enter`
  (in the multi-select picker it ticks that field, like `Space`); a double or
  triple click is treated as one click, so it never fires twice;
- a click outside just closes the popup. That click is swallowed and never reaches
  the pane behind it, so a single click can no longer "close the popup *and* play
  the row under it".

The settings option picker opens where its option is: its first row is the
option's row and its left edge is the value column — the column right after
the option's label and colon separator — so the option name stays visible to
the left of the box instead of being covered by it. Near the bottom edge it
flips above and is clamped; near the right edge the box shrinks to the space
left after that value column, and only when even that space is unusably narrow
does it fall back to starting at the option text's column. The whole box stays
inside the terminal — clear of the status line, the bottom key-hint row, the
scrollbar column and the right border. The same rule applies to every settings
section with option rows, including the colour palette, playback pickers and
info-display pickers; the Equalizer preset picker keeps its frequency-band
column anchor.

**Menu motion and graceful fallbacks.** Drop-down selection menus (the
control-bar popups and the settings option picker, including the colour,
Equalizer-preset and info-field pickers) slide in vertically and slide back out
when dismissed. Right-click context menus slide in diagonally, level by level,
and retract diagonally; the same applies when the menu is opened by the middle
button or by Ctrl/Alt+left click (see the fallback triggers below). Inside the
layout editor, the action/kind/colour/help modal menus animate too — vertically
when centred (`Enter`, `?`, `t`, `c`) and diagonally when the actions menu is
opened at the pointer by right-click; the editor's own close and the rename
prompt take over instantly. The motion is a front-end, session-local effect; it
writes no configuration.

Whenever the interface itself changes — F1–F8/page switches, a terminal resize,
a layout rebuild, configuration or language reload, opening or closing the
layout editor, or a modal text input taking over the screen — any open overlay
is closed instantly and no exit animation is played. While a menu animates, the
event loop polls input every 1 ms and composes a frame at every changed cell
position (no artificial frame cap); the other pane animations (spectrum, vinyl,
lyrics, progress) are paused for that moment so they cannot tear the moving
menu. Menus are logically usable immediately: keyboard input works at once, and
mouse hit-testing uses the settled (logical) rectangle rather than the moving
pixels. Any key or mouse event arriving during an exit animation snaps it to
completion before the event is dispatched, so input is never blocked.

If the terminal cannot render the full effect the motion degrades gracefully:
full movement → a shorter one-row/one-column slide (reduced) → no animation at
all (off). Terminals without colour or using the ASCII fallback take the
reduced tier; a non-TTY, `TERM=dumb`, a very small terminal, or a measured frame
cost above the budget disables menu motion. With the Konami aurora flow active,
the moving and exiting menu layers are composed by the flow's full-screen pass
(one `doupdate` per frame), and the flow's own vinyl/lyric phase re-render is
paused while a menu animation runs; toggling the flow re-probes the animation
tier.

Text input boxes are modal (search, scan directory, layout rename, hotkey capture,
numeric colour entry): mouse events there are ignored outright, so a stray wheel
event cannot corrupt the text being typed. Modifier keys change behaviour in
exactly one place: **Ctrl+left click and Alt+left click open the right-click
context menu** (see "Fallback trigger" below), but only on terminals that
forward modifier bits; a plain left click is never affected. The **middle mouse
button** opens the same menu without any modifier. Everything else is classified
by its button bits alone: a modified right click (Ctrl/Alt/Shift+right) or a
modified middle click is deliberately **not** claimed by ter-music and is left to
the terminal/window manager. Only the wheel up/down is requested; horizontal
wheel and pointer motion (hover) are not reported to the program.

**Right-click context menus.** A right click opens a menu anchored at the pointer;
the layout is chosen so the whole menu stays on screen (it flips up/left at the
edges and never covers the bottom key-hint row):

- at most one overlay exists at a time. While a control-bar popup, a settings
  picker or a layout-editor modal menu is open, a right click only dismisses it
  and is swallowed; it does not stack a second menu underneath. The same holds
  while a context menu is open: a right click (or a left click outside) closes the
  menu and that event never reaches the pane behind it;
- inside a menu, a left click selects and runs that entry; a submenu entry opens
  the next level, and the wheel moves the selection. `Up`/`Down` (the same bindings
  as popups), `Enter`, `Esc` and `Right` work from the keyboard; any other key
  closes and swallows the menu instead of acting on the view;
- entries are disabled when their precondition does not hold (empty queue, no
  track under the pointer, no playlists), and a checked mark shows the active
  choice (play mode, library browser, list mode);
- the list pane's menu covers queue management, favourites, adding to a playlist,
  folder opening, search, play history, playback controls, play mode and page
  navigation, and always includes **switch the list pane between file mode and
  queue mode**. A right click never plays the row under the pointer; it only
  selects it before opening the menu;
- every sub-view (Settings, History, Playlist, Favorites, Help, Language) exposes
  its own key operations in the same way. The layout editor's right click opens
  its existing "pane actions" menu at the pointer instead of a second menu system.
  The **F6 Info page** has exactly one entry: start/stop the Konami aurora flow.
  The main-view info pane deliberately has **no** right-click menu (a right click
  there is consumed); the controls, lyrics, spectrum, vinyl and hint-bar regions
  likewise have no right-click menu — their left-click/wheel operations already
  cover them;
- menus are non-blocking and are re-composed every event-loop turn, so playback,
  animations and the aurora overlay keep running while a menu is open. When a pane
  that is being repainted overlaps the menu, its refresh is staged with
  `wnoutrefresh`, the menu backing windows are re-asserted, and only one
  `doupdate` follows — the terminal never receives a frame with pane pixels inside
  the menu rectangle. Windows that do not intersect the menu keep the plain
  `wrefresh` path. View
  switches, terminal resizes, config/language reloads and layout rebuilds close
  the menu before their redraw; if a queue, history or library row changes after
  the menu was opened, row-specific actions are rejected with "the list changed;
  please try again" instead of acting on a shifted row.

**Fallback triggers on hostile terminals.** Some terminal emulators hard-wire the
bare (or a modified) right button to their own context menu and either never
forward it or forward it while their menu is also shown. ter-music cannot suppress
another program's menu, so the context menu has two alternate mouse triggers:

- **Middle mouse button.** Pressing the middle button (the wheel) opens exactly
  the same menu as a right click, at the pointer, and the row under the pointer is
  only selected, never played. This is the reliable trigger on deepin-terminal:
  it forwards middle clicks to the application and does not show its own menu for
  them. (Terminals that do not forward middle clicks simply never deliver it;
  nothing else changes.)
- **Ctrl+left click or Alt+left click**, on terminals that forward keyboard
  modifier bits to the application (xterm, VTE-based terminals, kitty, ...). The
  event is turned into a right click before any hit-testing, so the very same menu
  opens at the pointer at every right-click entry point listed above, and the row
  under the pointer is only selected, never played. **deepin-terminal 6.5.40
  strips the modifier bits before forwarding** (its `mouseSignal` carries the
  button number only), so there Ctrl/Alt+left is indistinguishable from a plain
  left click — it selects/plays instead of opening the menu. Use the middle
  button on that terminal.

While a menu is open, the middle button or Ctrl/Alt+left click closes it exactly
like another right click does; a plain left click outside also closes it. Do
**not** add Shift to these chords: many terminals use Shift+left for text
selection and never forward it. A modified right click (Ctrl/Alt/Shift+right) is
deliberately **not** claimed by ter-music, and neither is a modified middle click
— a terminal that owns such a chord would otherwise show a second menu, and the
TUI overlay would get out of sync with what is on screen. A bare right click
still opens the menu wherever the terminal forwards it without showing a menu of
its own; if it also shows its own menu (deepin-terminal does), use the middle
button. If the middle button is unavailable and the terminal does not forward
modifiers either, there is no clean mouse trigger left: use another terminal or
request a keyboard fallback.

### 5.5 Play Mode Description

Ter-Music features 17 play modes organized into 5 groups, with basic modes always available:
Press ENTER on the Mode button in the control bar to open the popup selection menu.

#### Basic Modes (always available)

| Mode | Description |
| -------- | --------------- |
| `Sequential` | Sequential playback, stops at end of list |
| `Single Repeat` | Single repeat, repeats current song |
| `List Repeat` | List repeat, starts from beginning after playing all |
| `Shuffle Once` | Shuffle playing, plays each track once without repeating |
| `Shuffle Repeat` | Full shuffle with repeat, randomly selects next song |

#### Advanced Modes (require database library metadata)

| Group | Modes | Description |
| ----- | ----- | ----------- |
| `Folder` | Sequential / Repeat / Shuffle / Shuffle Repeat | Scoped to current folder |
| `Album` | Sequential / Repeat / Shuffle / Shuffle Repeat | Scoped by album tag |
| `Artist` | Sequential / Repeat / Shuffle / Shuffle Repeat | Scoped by artist tag |

**Note:** Advanced modes use the SQLite library database for metadata lookups. Enable them in Settings → Play Mode → "Enable Advanced Play Modes".

### 5.6 Playback Speed Control

Ter-Music supports playback speed adjustment, allowing you to listen to audio at different speeds:

| Speed | Description |
| ----- | ----------- |
| `0.75x` | Slow speed, suitable for detailed listening or learning |
| `1.0x` | Normal speed, default playback speed |
| `1.25x` | Slightly fast, suitable for faster listening |
| `1.5x` | Fast speed, suitable for quickly browsing content |
| `2.0x` | Double speed, suitable for efficient listening |
| `3.0x` | Triple speed, maximum speed for rapid review |

**How to use:**
- In the control area, use `←`/`→` to select the Speed button, then press `Space` to switch speeds
- The current speed will be displayed on the Speed button (e.g., "Speed:1.50x")
- Speed can be changed during playback; the audio will seamlessly transition to the new speed
- The default speed setting can be configured in the settings menu (F2)

**Note:** Speed adjustment is implemented using FFmpeg's atempo filter, which maintains audio pitch while changing playback speed.

### 5.6.1 Karaoke Mode (Vocal Removal)

Karaoke mode attenuates the centre-placed lead vocal while leaving the accompaniment
practically intact, so you can sing along with an ordinary recording. It is a
*playback* feature: nothing is written to the file, and the original audio on disk
never changes.

| Level | Config value | What it does |
| ----- | ------------ | ------------ |
| Off | `0` | No processing at all — the buffer is passed through unchanged |
| Soft | `1` | Subtracts about 60 % of the *gated* centre band, so a faint vocal remains |
| Full | `2` | Subtracts the gated centre band completely (the vocal is as gone as this method allows) |

**How it works** (band-limited, similarity-gated centre cancellation):

1. The mid channel `m = (L + R) / 2` is split by two 4th-order Butterworth sections
   (24 dB/octave): a low-pass at 200 Hz and a high-pass at 5 kHz. Only the band
   between those corners — where a lead vocal mainly lives — can be taken away, so
   bass and the airy top end are never touched: `band = m - lo - hi`.
2. A similarity gate protects the accompaniment. The same filter chain also runs on
   the side channel `d = (L - R) / 2`, and each of `band` and `band_d` is split into
   two half-bands at 1 kHz. 15 ms envelope followers compare them: when a half-band
   is strong in the side channel and weak in the mid (`r = e_d / e_m ≥ 0.8`) the
   content is stereo-spread — pads, strings, wide synths — and the gate closes to 0;
   when it is centred (`r ≤ 0.45`) the gate is fully open; in between it tapers
   linearly. The two half-band gates are combined by taking the stronger one, and the
   cancellation subtracts `m' = m - α · g · band` (α = 0 / 0.6 / 1.0 per level).
3. The side channel itself is untouched and the stereo pair is rebuilt as
   `L' = m' + d`, `R' = m' - d`.

Keeping the bass and the top end intact is the point of the band limit, and the gate
is the point of the half-band split: subtracting an *ungated* band drags decorrelated
accompaniment down by up to ~3.5 dB even when nothing centred is playing; gated,
those passages now stay at 0 dB. When the level changes while playing, α ramps over
30 ms instead of jumping, so no click is audible; entering a level starts from α = 0
with the detectors and crossover states cleared, and returning to Off rides the same
curve down to zero before the DSP fast-passthrough re-engages (bit-exact once α
reaches 0). The request is published by `SetKaraoke` on the caller's thread and
consumed by the playback thread at the head of the next write batch, and the PipeWire
output is now paced so its backlog stays within a clamped window (about 0.4–1.2 s
depending on the negotiated quantum, ≈0.4 s by default) — previously the 1 MB ring
could sit ~3 s ahead, which is what made level changes feel delayed. The output is
soft-limited only when a sample would exceed the digital full scale.

**Limitations, stated plainly:**

- Stereo only. A mono track has no centre/side decomposition to work with, so it is
  passed through bit-exactly (`output_channels == 1` never touches the DSP path).
- Anything panned centre is removed *with* the vocal — a centred bass drum, a snare,
  or a mono-panned guitar. This is inherent to the method, not a bug to fix.
- When a centred vocal and stereo accompaniment share the same half-band, the gate is
  open for the vocal and the accompaniment there is cancelled along with it (typically
  a few dB); purely accompaniment-only passages are the ones the gate rescues.
- The flip side: vocals that are themselves doubled or stereo-spread look like
  accompaniment to the detector and survive more than before. Doubled or wide-panned
  vocals were never in the mid channel to begin with.
- The DSP runs on the decoded samples before the equalizer, so EQ, spectrum and
  visualiser all reflect the karaoke output.

**How to change the level:**

- Main controls pane: the Karaoke button shows the current level (e.g. "Karaoke:No vocals").
  Press `Enter`/`Space` on it (or click it) to open the three-item pop-up; the status
  line under the buttons confirms what was applied.
- Settings (F2) → Playback: "Karaoke (vocal removal)" — `Enter` opens the same choice list.
- CLI: `ter-music karaoke` to query, `ter-music karaoke soft` (or `full`/`off`/`1`/`cycle`) to set.
- D-Bus: `Control.SetKaraoke` / `Control.GetKaraoke`, or `Config.Set` with
  `preferences.karaoke_mode` (`api_version` 6; the namespace moved at 7).
  `GetKaraoke` and the CLI echo report the requested level immediately — the DSP
  catches up along the ramp described above.
- The level is persisted in `config.xml` as `<karaoke_mode>` and is restored on the
  next start. Values outside 0–2 clamp back to Off.

**Name collision, on purpose:** *karaoke* also refers to the per-character lyric
highlighting (`Lyrics.GetDocument` with `timing="chars"`) and to the `karaoke`
layout preset, which is a lyrics-first arrangement. None of the three are the same
feature; this section is only about vocal removal.

### 5.7 Lyrics Display

Ter-Music supports automatic loading of LRC format lyrics files:

- **Embedded lyrics take priority**: The player first reads embedded lyrics from the audio file (FFmpeg/APE tags). If none are found, it falls back to external `.lrc` files. Recognized keys include `lyrics`, `LYRICS`, `lyrics-eng`, `UNSYNCEDLYRICS` and `©lyr`; this covers ID3 `USLT`/iTunes `lyrics-eng` tags as well as plain `lyrics`/`UNSYNCEDLYRICS`.
- Lyrics files should be placed in the same directory as the audio file
- Lyrics filename should match the audio filename, with extension `.lrc`
- Example: `song.mp3` → `song.lrc`
- The program automatically highlights current lyrics based on playback time
- **Per-character (karaoke) highlighting**: LRC files with per-character or
  per-segment timestamps are sung character by character; a line with start and
  end timestamps spreads its characters evenly across `[start, end)`; a legacy
  line with only a start timestamp is spread up to the next line's start (the
  last line reuses the previous line's duration). Long lines that do not fit the
  pane keep the whole-line highlight instead
- **Switch lyrics source**: Press `Ctrl+L` to enter lyric seek mode, then press `Tab` to switch between embedded/external lyrics
- If no lyrics file is found, the lyrics pane keeps the vinyl animation (colored blocks when colors are available, otherwise the original glyph animation); the separate spectrum pane keeps showing the audio-reactive bars

Other applications can read the current A/B lyric lines through the D-Bus
lyrics API; see [Lyrics API (English)](API_LYRICS_en_US.md).

### 5.8 MPRIS & Lyrics API

When built with D-Bus (`libdbus-1`), Ter-Music registers an MPRIS media
session on the session bus:

- Desktop environments (GNOME Shell, KDE Plasma, Cinnamon, Budgie, and others)
  can show playback controls and track metadata.
- `mpris:artUrl` is published whenever album art is available, so desktop
  media widgets can display the cover.
- Album art is extracted from embedded tags first. If none is found, common
  same-directory cover files are used (`cover`, `folder`, `front`, `album`,
  case-insensitive, with `.jpg`, `.jpeg`, `.png`, or `.webp` extensions).
- Extracted covers are managed JPEG cache files under
  `/tmp/ter-music-cover-*.jpg`, kept as a recent-N (10) MRU cache and cleaned
  up when the player exits.

An open lyrics API is available on the same D-Bus object: interface
`cn.org.huansoft.ter_music.Lyrics` with the methods `GetLyrics`, `GetDocument` and
`SetSource` (switch between embedded and external lyrics), and the signal
`LyricsChanged`. Since `api_version 5` the A/B snapshot carries
`char_sung`/`char_total`, and each `GetDocument` line carries `end`, `timing`,
`char_total` and (for per-character LRC) a `chars` array of relative
millisecond offsets, so third-party karaoke clients can render the same
highlight. See [Lyrics API (English)](API_LYRICS_en_US.md) for the JSON schema
and examples.

Four more interfaces are published on the same object path so that other
applications can read track data, text cover art and progress, and drive the
player:

- `cn.org.huansoft.ter_music.Info` (read-only): `GetInfo`, `GetTrackInfo`,
  `GetProgress`, `GetLyricsLines`, `GetCoverArt(charset, cols, rows)`,
  `GetDisplay(options)` (the exact text printed by `ter-music show`),
  `InstanceInfo`, plus the signals `InfoChanged`, `ProgressChanged` (at most
  1 Hz) and `CoverChanged`.
- `cn.org.huansoft.ter_music.Control`: transport, seek, volume, speed, play mode,
  `ReloadConfig` and `Quit`. Loading content is **not** part of it — the front
  end delivers a queue instead.
- `cn.org.huansoft.ter_music.Queue`: the core's **path queue** —
  `Set`/`Append`/`InsertAfter`/`RemoveAt`/`MoveUp`/`MoveDown`/`Clear`/
  `Shuffle`/`PlayAt` and paged `Get(offset, count)`, with the `QueueChanged`
  signal (entries, current position, revision). Local paths only; at most 500
  entries per write and 1000 per read.
- `cn.org.huansoft.ter_music.Config`: read and patch the core configuration (the only
  writer). Remote server entries are **not** part of it: they belong to the
  front end (see below).
- `Info.GetInfo` advertises `core.api_version` (currently `7`) plus the
  implemented method list, so clients can check compatibility before using
  anything else. Version 3 removed the `Remote` interface and the
  `track.is_remote` field and made every path argument local-only; version 4
  replaced the content interfaces (`Playlist`, `Library`, `Favorites`,
  `History`, `DirHistory`) with the path-based `Queue` — content now lives in
  the front end. Version 5 added per-character lyric timing (`char_sung`,
  `char_total`, `end`, `timing`, `chars`) additively: the new front end accepts
  a version 4 core (whole-line fallback) and a version 4 front end accepts a
  version 5 core. Version 6 added karaoke vocal removal
  (`Control.SetKaraoke`/`Control.GetKaraoke` and the `karaoke_mode` config key),
  again additively — a version 5 client never calls them, and a version 6 front
  end on a version 5 core just reports the level as off.
- `org.freedesktop.DBus.Introspectable` and `org.freedesktop.DBus.Peer` are
  implemented, so `busctl --user introspect` / `gdbus introspect` work.
- MPRIS metadata additionally carries `xesam:url` (always a `file://` URI,
  including for tracks downloaded from a remote source) and
  `xesam:trackNumber`; `OpenUri` answers
  `org.freedesktop.DBus.Error.NotSupported` because the core no longer loads
  content. Open files through `ter-music %F` (desktop entry) or the TUI instead.
- `CanQuit` stays `false` on purpose so desktop media widgets cannot kill the
  player; use `ter-music daemon stop` or `Control.Quit` instead.

See [D-Bus Info & Control API (English)](API_DBUS_en_US.md) for the complete
method list and JSON schema.

The same interfaces are published when ter-music runs from a Linyaps package:
the container uses the host session bus, so host applications and the packaged
CLI see the very same object path and interfaces.

### 5.9 Configuration File

The configuration file is stored at `~/.config/ter-music/config.xml`. The program will automatically create it on first run (and auto-migrate from v1 `config.json` if present).

**Configuration options include:**

- `default_startup_path`: Default startup directory
- `auto_play_on_start`: Auto-play on startup (0/1)
- `remember_last_path`: Remember last opened directory (0/1)
- `show_album_cover`: controls-pane album cover master switch and the info-pane cover master gate (0/1; the vinyl center label is independent)
- `show_lyrics_panel`: **deprecated for the TUI**. Layout visibility now comes from the selected layout preset. This key is only used as a first-run hint when `~/.config/ter-music/ui-layout` does not exist (`1` → `classic`, `0` → `focus`). It remains in `Config.GetAll` for compatibility
- `default_playback_speed`: Default playback speed (0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
- `default_play_mode`: Default play mode (0=Sequential, 1=Single Repeat, 2=List Repeat, 3=Shuffle Once, 4=Shuffle Repeat, ...)
- `advanced_play_modes_enabled`: Enable advanced folder/album/artist play modes (0/1)
- `lyrics_alignment`: Lyrics text alignment (0=Left, 1=Center, 2=Right)
- `clear_history_on_startup`: Clear the directory history on startup (0/1)
- `resume_last_playback`: Resume playback from last position (0/1)
- `seamless_preload`: Pre-decode next track at end of current for gapless playback (0/1)
- `ui_language`: Interface language ID. Built-in packs are `zh_CN`, `en_US` and `zh-PQ` (language name 中文（先秦）: pre-Qin classical Chinese, traditional characters; the app's translated display name is 端闈樂部); any third-party ID installed through the F7 language view is also valid (e.g. `ja_JP`). An invalid ID falls back to `zh_CN`.
- `volume_percent`: Default volume percentage (0-100)
- `audio_latency_ms`: Output latency in milliseconds
- `audio_backend`: Audio output backend (0=Auto, 1=PulseAudio, 2=ALSA, 3=PipeWire). Auto probes every candidate at startup; if an explicitly chosen backend turns out unavailable it falls back to the next working one at play time and the switch is announced in the status line
- `sort_mode`: Playlist sort mode (0=Default, 1=Title, 2=Artist, 3=Album, 4=Filename)
- `cue_encoding`: CUE file character encoding (0=Auto, 1=UTF-8, 2=GB18030, 3=GBK, 4=BIG5, 5=Shift-JIS)
- `karaoke_mode`: Karaoke vocal-removal level (0=off, 1=soft, 2=full; see 5.6.1). Stereo only; anything outside 0-2 is reset to 0. Added with the application's `api_version` 6 — `api_version` 7 renamed the D-Bus namespace but left this key alone — and the configuration file format itself did **not** change version (still 9) — older builds simply ignore the element, and a missing element means 0
- `core_exit_when_no_frontend`: let the core exit by itself once no front end has been attached for 10 s (0/1, default 0)
- `stop_core_on_exit`: stop the playback core when a front end exits — the front end sends `Control.Quit` instead of `Control.Detach`, and only when it really attached to that core (handshake done, `Attach` token held) (0/1, default 1 — with 0, closing the TUI keeps the music playing in the background). The key arrived with configuration contract version 8; the `version` attribute of `config.xml` now carries the current contract number (9) instead of the old `2.2`; downgrading to an older build does not fail, but that build ignores the key and writes `2.2` back on its next save, so the explicit value is lost and the upgrade re-applies the default
- Keybinding overrides live in the optional `<keybindings>` section (contract v9). Only overrides are stored; actions without an entry use their built-in keys. See [5.12 Shortcut Cheat Sheet](#512-shortcut-cheat-sheet) for the XML example and the key-name grammar
- Remote server connections (SMB/SFTP/FTP/WebDAV/HTTP) are **not** stored in
  `config.xml`: the front end keeps them in its own `remote.xml` next to it
  (server entries plus password ciphertext, file mode 0600), and caches
  downloaded tracks under `$XDG_CACHE_HOME/ter-music/remote/`
- Colour configuration is layered. The global theme (24 preset themes + 1 custom
  slot, paired-colour guard) covers the interface chrome — sidebar, highlight and
  border foreground/background — and is stored in `config.xml`. Pane colours are
  part of the **layout**: Settings → Appearance shows one foreground/background
  row per pane (`playlist`, `info`, `controls`, `spectrum`, `lyrics`, `vinyl`),
  reads and writes the current layout's `panes[].fg` / `panes[].bg`, and marks each
  row `[layout]` or `[theme]` to show which layer currently decides the colour.
  The stock presets colour every pane with a distinct foreground and a transparent
  background, so each row for a pane the preset contains reads `[layout]`; the
  values are chosen to stay
  distinct after the 256 → 8 → monochrome fallback (no colour at all on a
  monochrome terminal). Editing a `[theme]` row of a pane that owns a theme slot
  (playlist / controls / lyrics) still changes the theme, exactly as before; every
  other edit is saved as a layout override. See [UI_LAYOUTS_en_US.md](UI_LAYOUTS_en_US.md)
- TUI layout selection: `~/.config/ter-music/ui-layout` (one layout id written by the front end); layout definitions: built-in `layouts/layouts.json` plus optional `~/.config/ter-music/layouts.json`. Layout selection never writes `config.xml`; among the colour settings, only a pane-colour change writes the user layout file (the layout editor writes it too, when you save there)
- Equalizer configuration: 10-band gains, pre-amp, enable/disable
- Info display configuration (main info pane + CLI/D-Bus), editable in **Settings → Info Display**:
  - `info_preset`: preset (0=Full, 1=Compact, 2=Custom)
  - `info_fields`: bitmask of the basic info fields (1=State, 2=Mode, 4=Index, 8=Queue, 16=Title, 32=Artist, 64=Album, 128=Format, 256=Path, 512=Volume, 1024=Speed; 2047=all). The main info pane uses the subset it can display (state/mode/title/artist/album/queue/format); `index`/`path`/`volume`/`speed` are CLI/D-Bus only and are marked "CLI only" in the field picker
  - `info_show_cover`: info-pane cover and CLI text cover (0/1); the TUI pane additionally requires the playback cover master switch `show_album_cover`
  - `info_cover_cols` / `info_cover_rows`: CLI text-cover size in character columns/rows (4-40 / 2-20)
  - `info_cover_charset`: CLI text-cover charset (0=Braille, 1=ASCII, 2=Half-block)
  - `info_show_progress`: print the CLI progress line (0/1)
  - `info_progress_style`: CLI progress style (0=Bar+Time, 1=Time, 2=Percent, 3=Time+Percent)
  - `info_lyrics_lines`: CLI lyric lines (0=Off, 1=Current, 2=Current+Next)

The program automatically saves configuration; changes take effect immediately after modification.

### 5.9.1 Language Pack System

Ter-Music uses an XML-based internationalization (i18n) system. Built-in language packs are located at `data/lang/` in the source tree and installed to `TER_MUSIC_DATA_DIR/lang/`.

**Language Pack Format:**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<lang id="en_US" name="English (US)">
  <string key="general.yes">On</string>
  <string key="general.no">Off</string>
  <!-- ... more string entries ... -->
</lang>
```

- The root element is `<lang>` with attributes `id` (language identifier, e.g. "zh_CN") and `name` (display name).
- Each translatable string is a `<string>` element with a `key` attribute and the translated text as content.
- String keys follow a dotted hierarchical convention: `module.section.name` (e.g. `sidebar.settings.theme`, `menu.help`).

**Lookup Priority (highest to lowest):**

1. `~/.config/ter-music/lang/<id>.xml` — User custom overrides
2. `TER_MUSIC_DATA_DIR/lang/<id>.xml` — Compile-time install prefix
3. `/usr/share/ter-music/lang/<id>.xml` — System-wide install
4. `<exe_path>/../share/ter-music/lang/<id>.xml` — Relative to executable
5. `data/lang/<id>.xml` — Development/runtime directory
6. Source tree `data/lang/<id>.xml`

To add a new language, create an `<id>.xml` file following the format above and place it in one of the search paths (user override at `~/.config/ter-music/lang/` is recommended). The language will appear in the F7 language selection view automatically.

`ui_language` is not restricted to the built-in `zh_CN`/`en_US`/`zh-PQ` packs: selecting
a third-party language in F7 stores its ID, and the configuration validator
accepts any ID that is safe as a file name. An ID that is not installed, or one
that fails validation, falls back to `zh_CN`.

**CLI language selection.** CLI-local text (help, errors, tables and prompts)
uses this priority, highest first:

1. `--lang <id>`
2. `TER_MUSIC_CLI_LANG`
3. `ui_language` from `config.xml` (the F7 choice)
4. `LC_ALL`
5. `LC_MESSAGES`
6. `LANG`
7. `zh_CN`

`--lang` and `TER_MUSIC_CLI_LANG` may appear anywhere on the command line
(including before the subcommand) and are normalized like locale names
(`en_US.UTF-8` → `en_US`, `zh-*` → `zh_CN`; the built-in `zh-PQ` is kept as
`zh-PQ`). `C`, `POSIX` and an empty locale
value mean "no preference" and continue down the chain, so a configured
`ui_language` still wins over them. An explicit language that is not installed
prints a warning and falls back to `zh_CN`.

`--lang` only affects the CLI's own strings. The payload printed by `show`,
`track`, `lyrics` and `cover` is rendered by the core with its
`ui_language`, so those bodies stay in the TUI language unless the core
configuration is changed.

#### 5.9.2 tar.gz Language Pack Distribution

Language packs can also be distributed as `.tar.gz` (or `.tgz`) archives for easy sharing and one-click installation via the language selection view (press `A` to install, `D` to delete a user-added language).

**Archive Contents:**

| File | Required | Description |
|------|----------|-------------|
| `lang.xml` | Yes | Language data file (see §5.9.1 for format) |
| `help.txt` | No | Quick-start help text for this language |

Only files named `lang.xml` and `help.txt` are extracted from the archive; all other files are silently ignored. Files are matched by basename only, so they may reside at any depth within the tarball.

**Specification:**

| Property | Value |
|----------|-------|
| File extension | `.tar.gz` or `.tgz` |
| Archive format | POSIX/USTAR (standard `tar` format) |
| Compression | gzip |
| Per-file size limit | 50 MB |
| Character encoding | UTF-8 |
| Compression level | Any (gzip compatible) |

**Creating a Language Pack:**

```bash
# Minimal — language data only
tar -czf mylanguage.tar.gz lang.xml

# With optional help text
tar -czf mylanguage.tar.gz lang.xml help.txt

# Files may be in a subdirectory; only the basename matters
tar -czf mylanguage.tar.gz path/to/lang.xml path/to/help.txt
```

**Installation Paths:**

Upon installation via the language view (`A` key), the extracted files are placed:
- `~/.config/ter-music/lang/<id>.xml` — Language data
- `~/.config/ter-music/help/help-quickstart-<id>.txt` — Help text (if `help.txt` was included)

The language `<id>` is read from the `id` attribute of the `<lang>` root element in `lang.xml`.

**Validation:**

The program validates the archive on import:
1. Rejects non-regular files and non-`.tar.gz`/`.tgz` extensions
2. Extracts `lang.xml` and parses it as XML
3. Verifies the root element is `<lang>` with a non-empty `id` attribute
4. Rejects archives that would overwrite built-in languages (`zh_CN`, `en_US`, `zh-PQ`)
5. Enforces the 50 MB per-file size limit

After successful validation, the language appears in the language selection view immediately. User-installed languages are marked distinctly from built-in languages in the UI.

**Note:** If a language with the same `<id>` already exists in `~/.config/ter-music/lang/`, installing a new tar.gz will silently overwrite it. Reinstall the program to restore built-in languages if they were accidentally deleted.

### 5.10 Data Storage Location

All user data is stored in the `~/.config/ter-music/` directory:

```
~/.config/ter-music/
├── config.xml       # Configuration file (XML, parsed via libxml2; migrated to the current version on start)
├── library.db       # SQLite database (music library, favorites, playlists, history)
├── remote.xml       # Remote server list (front-end only; not read by the core)
├── lang/            # User language pack directory (overrides built-in translations)
└── config.json.bak  # Auto-backup of v1 config on first migration (if present)
```

**Note:** The playback queue is **not** persisted as a file any more: it is
content, so the front end rebuilds it from your library / last opened directory
on the next start, while the core restores the cursor when
`resume_last_playback` is enabled. The v1.0 JSON-based storage (`config.json`,
separate `favorites`, `history`, `dir_history`, `playlists/`) has been fully
replaced by the SQLite database `library.db`; migration is automatic on first
v2.0 startup.

Album covers are not stored in `~/.config/ter-music/`. Extracted covers are
temporary managed JPEG files under `/tmp/ter-music-cover-*.jpg`, kept for the
last 10 tracks and removed when the player exits.

### 5.11 Basic Usage Flow

**Example: First time use**

1. Launch the program:
   ```bash
   ter-music
   ```
2. Press `O` to open folder, enter your music directory path, for example:
   ```
   /home/yourname/Music
   ```
3. The program will scan all audio files in the directory and display them in the playlist
4. Use `↑` `↓` to select the song you want to listen to, press `Space` to start playback
5. If a lyrics file exists, lyrics will be automatically loaded and synchronized on the right side
6. Use `,` and `.` to seek backward/forward 5 seconds

**Example: Add song to favorites**

1. Select the desired song in the list area
2. Press `F`, the bottom status bar will display "Added to favorites!"
3. Press `F5` to view all favorited songs
4. You can select and play favorited songs in the favorites view

**Example: Create custom playlist**

1. Press `F4` to enter playlist management view
2. Select "Create New Playlist"
3. Enter playlist name
4. Return to main interface, select songs in the list, press `A` to add to a custom playlist (select from the popup)

**Example: Browse music library**

1. Press `M` to enter library browser view
2. Use `↑`/`↓` to navigate: Home → Artists → Albums → Tracks
3. Press `Enter` on an artist to see their albums, on an album to see tracks
4. Press `Enter` on a track to play it
5. Press `M` again or `Esc` to return to folder browsing

**Example: Manage playback queue**

1. Select a track in the file browser, press `a` to append it to the queue
2. Press `Tab` to switch to queue view and see the ordered list
3. Use `J`/`K` to re-order tracks, `d` to remove a track, `D` to clear all
4. Press `Enter` on any queue entry to play it
5. Press `Tab` again to return to file browser

### 5.12 Shortcut Cheat Sheet

| Group | Keys | Function |
| ------ | ------------------- | -------- |
| **Global** | `q`/`Q` | Exit program |
| <br /> | `F1` | Return to main |
| <br /> | `F2` | Settings |
| <br /> | `F3` | Playback history |
| <br /> | `F4` | Playlist management |
| <br /> | `F5` | Favorites |
| <br /> | `F6` | About |
| <br /> | `F7` | Language selection (opens language view) |
| <br /> | `F8` | Help |
| <br /> | `F9` | Quit |
| <br /> | `Esc` | Return to main / back |
| **Focus** | `C` | Focus to control |
| <br /> | `L` | Focus to list |
| <br /> | `Tab`/`Shift+Tab` | Toggle file/queue view |
| **List/Browser** | `↑`/`↓` or `j`/`k` | Select prev/next |
| <br /> | `Space`/`Enter` | Play selected |
| <br /> | `O` / `o` | Open folder |
| <br /> | `F` / `f` | Add to favorites |
| <br /> | `a` | Append track to queue |
| <br /> | `A` | Add to custom playlist |
| <br /> | `i` | Insert as next in queue |
| <br /> | `I` | Append folder to playlist |
| <br /> | `d` | Remove from queue |
| <br /> | `D` | Clear entire queue |
| <br /> | `J` | Move down in queue |
| <br /> | `K` | Move up in queue |
| <br /> | `S` or `/` | Activate pinyin search |
| <br /> | `M` | Toggle library browser |
| <br /> | `n` | Next track |
| <br /> | `p` | Previous track |
| <br /> | `h` | Show history popup |
| <br /> | `1`-`5` | Quick set play mode (1=Seq...5=Folder Seq) |
| **Control** | `←`/`→` | Select control |
| <br /> | `Space` | Activate control / open popup |
| <br /> | `,` | Back 5 sec |
| <br /> | `.` | Forward 5 sec |
| <br /> | `Ctrl+L` | Toggle lyric seek mode |
| <br /> | `-`/`_` | Decrease volume |
| <br /> | `=`/`+` | Increase volume |
| <br /> | `R` | Restart the playback core |
| **Lyrics** | `Ctrl+L` | Toggle lyric seek mode |
| <br /> | `↑`/`↓` | Select prev/next line (in seek mode) |

#### Custom Keybindings

Every non-text action above **except the system-reserved keys** can be rebound.
The reserved keys are fixed and do not appear in the settings editor:

- `F1`–`F9`: view/quit function keys
- `q` / `Q`: quit
- `Tab` / `Shift+Tab`: file/queue tab switch

For all other actions, the built-in keys are defaults; after an action is
rebound, **all** of its old default aliases stop working. `Esc` still
cancels/returns.

**Editing on the settings page:** open `F2` → `Settings → Shortcuts`. Use
`↑`/`↓` to select an action, press `Enter` and then the new key. `Esc` cancels
capture. A key already used by another action in the same context is rejected
with a message and the old key is kept. `r` restores the selected action,
`R` restores all actions, and `s` saves (nothing is written before saving).

**config.xml:** only overrides are written, sorted by action id:

```xml
<ter-music-config version="9">
  <keybindings>
    <binding action="track.next" key="]"/>
    <binding action="track.prev" key="["/>
  </keybindings>
</ter-music-config>
```

Overrides for system-reserved actions are ignored at runtime (the file entry is
kept and `ter-music keys check` reports a warning); `Config.Set` rejects new
bindings for them, while the value `"default"` is accepted for cleanup.

**Key-name grammar (v1):**

- Single printable ASCII character, case-sensitive: `a`–`z`, `A`–`Z`, `0`–`9`,
  `!`–`~`. A space must be written as `space`.
- `ctrl+a` … `ctrl+z`, except the ambiguous `ctrl+c` (SIGINT), `ctrl+h`
  (Backspace), `ctrl+i` (Tab), `ctrl+j` (LF), `ctrl+m` (CR) and `ctrl+z`
  (SIGTSTP).
- Named keys: `up` `down` `left` `right` `home` `end` `pgup` `pgdn` `ins`
  `del` `backspace` `tab` `btab` (Shift+Tab) `enter` `space` and `f1`–`f12`.
  Named keys and the `ctrl+` prefix are lowercase; printable characters keep
  their case.
- Not supported: `alt+…`, multi-key sequences such as `gg`, non-ASCII
  characters, bare control characters and empty/over-long names. `Esc` is
  reserved and cannot be used as a binding value.

**Auditing:** `ter-music keys list [--json]` prints every action with its
context, label, effective keys, defaults, override flag and shadowing relations.
`ter-music keys check [--json]` validates the configured file and exits non-zero
on unknown actions, invalid keys, conflicts or actions without defaults.
Missing terminfo capabilities for the current `$TERM` are reported as warnings
only, so the same configuration stays portable across terminals.

**Restoring defaults:** delete the action's `<binding>` from `<keybindings>`
(remove the section when it becomes empty), use `r`/`R` on the settings page, or
send `Config.Set` with `{"keybindings":{"action.id":"default"}}`.

**Compatibility:** builds from before contract v9 ignore `<keybindings>` and
still start, but saving from such a build drops the overrides. A front end older
than v9 also does not recognize the `keybindings` object in `Config.GetAll`
from a v9 core, so upgrade the front end and the core together. Keeping the
file on v9 and using a current build preserves the overrides.

### 5.13 Terminal Resizing

Ter-Music supports terminal window resizing. The selected layout is recomputed from its 1000×1000 proportions; every cell of the new main area is still owned by exactly one pane, and the bottom menu bar is redrawn last.

### 5.14 Exit the Program

There are three ways to exit the front end:

- Press `q` in the main interface
- Press `Ctrl+C` / `Ctrl+D` / `Ctrl+\` (the program handles SIGHUP/SIGTERM/SIGINT gracefully and exits cleanly)
- Select "Exit" in the options menu (which is the `F9` key)

By default, leaving the TUI also stops the music: `preferences.stop_core_on_exit`
is on (`1`) in the front end's own configuration mirror, so every exit path above
sends `Control.Quit` — provided the front end had actually attached to the core
(handshake done, `Attach` token held) — and the core persists its session/resume
state, stops playback and exits. Set the option to `0` to
keep the music playing: the front end then only sends `Control.Detach`, the
current track keeps playing and `ter-music show` / `ter-music next` still work
from any terminal. Stop a running core explicitly with `ter-music daemon stop`,
make any core exit on its own after the last front end has been gone for 10 s by
setting `core_exit_when_no_frontend` to `true`, or let it notice that it can no
longer be reached at all — the core exits by itself when its session bus
disappears (see
[5.2.3 Front End and Core](#523-front-end-and-core)).

## 6. Frequently Asked Questions

**No sound output**
- The audio backend is auto-detected in order: PipeWire → PulseAudio → ALSA. Auto mode probes each candidate at startup (for ALSA this really opens the default device), retries briefly when nothing answers, and falls back to the next working backend if the chosen one cannot open the stream — the switch is shown in the status line and in the log. Run `pactl info` or `pw-cli info` to check which service is active
- A backend whose probe fails is not listed in Settings (F2) → Audio; an explicitly chosen but unavailable backend is replaced by the fallback above rather than silently failing
- Check that your speaker volume is not muted
- If using PipeWire, ensure `pipewire` and `wireplumber` services are running
- If using PulseAudio, run `systemctl status pulseaudio` to verify
- Manually switch the audio backend in Settings (F2) → Audio

**Poor audio quality, choppy/stuttering playback, or crackling noise**
- Audio device performance varies across different machines, so the default audio latency setting may not be optimal for your hardware
- Try increasing the "Output Latency" value in the settings menu (press `F2` to enter settings), or directly edit the `audio_latency_ms` field in the configuration file at `~/.config/ter-music/config.xml`
- The latency range is 20-250 milliseconds. It is recommended to increase it by 10 ms at a time and test until playback is smooth
- If issues persist after increasing latency, check your audio server configuration (PipeWire/PulseAudio) or update your audio drivers

**Chinese characters display as garbled text or squares**
- Make sure your terminal uses UTF-8 encoding
- Check your system locale settings: run `locale` and verify it shows `LC_CTYPE=UTF-8`
- If CJK characters still display incorrectly in a tty terminal, try using the kmscon terminal instead, which has better East Asian character support

**Header files not found during compilation**
- Make sure all development dependency packages are installed (see Section 3)
- Most systems split runtime and development packages — you need to install the `*-devel` or `*-dev` variants

**Cannot open certain audio files**
- Verify that your FFmpeg version supports the audio format
- Newer FFmpeg versions support more formats, so upgrading is recommended

**CUE split tracks not showing**
- Ensure the .cue file has the same name as the audio file (e.g., `album.flac` + `album.cue`)
- If CUE text appears garbled, change the encoding setting in Settings → CUE Encoding (try GBK for Chinese, Shift-JIS for Japanese)

**Music library not showing all my music**
- Press `M` to enter library browser mode, then check if `library.db` exists in `~/.config/ter-music/`
- The library scans on startup — if you added new music, restart the program to trigger a rescan
- The library now supports **recursive directory scanning** for nested folder structures

## 7. Technical Architecture

### 7.1 Front End and Core

The code base is organized around two planes that share one process image but
never share responsibilities. A build can run either plane (the daemon is core
only, the TUI/CLI is front end only), and they meet exclusively at the `player`
facade and the D-Bus surface:

| Plane | Process | Owns |
| ----- | ------- | ---- |
| **Core / playback service** | `ter-music daemon foreground` (started by `daemon start` or by a TUI front end; the Linyaps package can also start it via D-Bus activation or its systemd user service — deb/RPM ship neither file) | Audio device and decoding, play queue execution (**paths only**), transport, volume / speed / play mode, karaoke vocal removal, equalizer, current-track info (lyrics, text cover, spectrum), the published `Lyrics` / `Info` / `Control` / `Queue` / `Config` interfaces, front-end registry and heartbeat, configuration |
| **Front end / content client** | `ter-music` (TUI), `ter-music play\|show\|…` (CLI) | Directory scanning and metadata, SQLite library and FTS5 search, user playlists, favorites / history / directory history, sorting / filtering, remote sources and their download cache, the ncurses UI |

The seam is deliberately thin and testable:

- **`player/` facade** — `player.c` dispatches to `player_local.c` (in-process
  playback, kept as a migration baseline) or `player_remote.c` (D-Bus client,
  the default). The UI only ever calls the facade, which is what makes the
  front end independently buildable and lets the same TUI drive either plane.
- **`queue/backend_queue.c`** — the core's path queue: order, cursor and
  revision, plus the entry metadata (path, title, artist, album, duration, CUE
  offset/track number, lyric source). `audio/play_queue.c` is a thin forwarder
  used by the in-process backend, and `playlist/playlist_queue.c` is the only
  bridge that renders front-end content into a core queue.
- **D-Bus surface, `api_version 7`** — `Queue.Set/Append/InsertAfter/RemoveAt/
  MoveUp/MoveDown/Clear/Shuffle/PlayAt/Get` accept **local paths only** (remote
  URLs are rejected), batch at most 500 entries per call and page reads at
  `RPC_PAGE_MAX` (1000) entries; `QueueChanged` announces count / current
  position / revision so any number of front ends stay in sync. The lyrics
  interface additionally exposes per-character timing (`GetDocument`
  `end`/`timing`/`chars`, `GetLyrics` `char_sung`/`char_total`).
- **Front-end registration** — every front end registers and heartbeats, which
  is what feeds `core_exit_when_no_frontend` and `Info.GetInfo.frontends`.
  Disconnects are not fatal: the front end backs off (1/2/4/8/15/30 s),
  re-attaches, and re-pushes its content queue, because the core holds no
  content of its own.

### 7.2 Architecture Gates

Four scripts keep the split honest, and all of them run in CI:

| Gate | Command | Rule |
| ---- | ------- | ---- |
| Backend purity | `scripts/test/check-core-purity.sh` | Core directories (`audio config core info lyrics media queue` + `cli/daemon.c`) must contain **no** remote-source symbol and no content/UI reference (playlist, library, search, UI rendering, front-end headers) |
| Front-end purity | `scripts/test/check-ui-purity.sh` | The UI must reach the playback surface **only** through the `player` facade — no engine playback globals or engine playback commands |
| Config ownership | `scripts/test/check-config-ownership.sh` | The core owns `config.xml`. The front end must not call `save_config()` / `config_save_to_xml()` at all: it changes its own mirror and persists the difference through the facade (`player_config_persist()`, which is `Config.Set` in remote mode) |
| Package identity | `scripts/test/check-package-identity.sh` | The packaging identity must agree everywhere: one application id across `debian/control`, `PKGBUILD`, `.SRCINFO`, `scripts/build/*` and the systemd unit; one version across `APP_VERSION` (the source of truth), CMake `project VERSION`, `pkgver` and the top `debian/changelog` entry, in the rolling `<yy>.<q>.x.y` form; `TER_MUSIC_API_VERSION` must equal what `dbus-rpc-check.sh` asserts; no old `org.yxzl` namespace may survive in includes, interface string literals or the packaging chain; and the deliberately unchanged identity (binary `ter-music`, MPRIS bus name, config dir) must not have been "helpfully" renamed |

Regression suites accompany them: `scripts/test/run-unit-tests.sh` (path queue,
play-queue contract, lyrics parsing, JSON reader, configuration diff, layout
schema/geometry/state, menu-animation geometry/easing/tier fallback, the
ncurses-free mouse-event routing table, including a
per-cell rasterisation check that every
preset tiles without blanks) plus the
end-to-end scripts `dbus-rpc-check.sh`, `config-migration-check.sh`,
`lifecycle-e2e.sh`, `offline-reconnect-e2e.sh`, `multi-frontend-e2e.sh`,
`paging-deepdir-e2e.sh` and `remote-frontend-e2e.sh`, `karaoke-e2e.sh`
(from the `karaoke` CLI and `Control.GetKaraoke` down to `config.xml` persistence and the
restart re-apply, including out-of-range clamping and the missing-key default),
`mouse-tui-e2e.sh`
(whose pty feeds real SGR click/wheel bytes to the TUI and asserts both the
D-Bus outcome and which pane owned each event — including the popup-expanded
case where an outside click must dismiss the popup without reaching through
to the list behind it), `lyrics-pane-e2e.sh` (which replays a real pty capture
through `vt_grid.py` and asserts that the lyrics pane appears on startup, steps
aside for a track without lyrics, and returns automatically after a track switch
without any keypress), `context-menu-e2e.sh` (which injects real right-click SGR
bytes, asserts the menu anchor/clamping and nested submenus, executes queue and
page actions through the menu, checks the info page has exactly the aurora entry,
and verifies stale-row rejection after an external queue change), and the
performance probe
`perf-check.sh` (command-to-visible-state latency and idle CPU, both with
thresholds; CI records the numbers without gating on them).

### 7.3 Module Map

Ter-Music adopts a modular design, main modules include:

> **Source files** are organized under `src/cn.org.huansoft.ter-music/<module>/`; **public headers** under `include/cn.org.huansoft.ter-music/<module>/`.

- **main/main.c**: Program entry, argument handling, front-end startup split
  (`frontend_init_config()` for the remote mode, `init_all_persistent_data()`
  for the local baseline)
- **player/**: Playback facade — `player.c` (dispatch), `player_local.c`
  (in-process backend), `player_remote.c` (D-Bus client, default), declared in
  `include/…/player/player_backend.h`
- **core/core.c**: Core-side bootstrap shared by the daemon and the in-process
  baseline
- **cli/cli.c, cli/cli_client.c**: **Front end** CLI dispatch and the thin D-Bus
  client behind `play`/`pause`/`show`/…; `play` scans content locally, builds
  the path queue and delivers it
- **cli/daemon.c**: The **core** process — configuration, playback and the
  front-end watchdog; it never scans a directory
- **queue/backend_queue.c**: Core path queue (order, cursor, revision,
  local-path guard, per-call size limit) plus CUE look-ahead
- **audio/**: Core audio engine — decoding and playback thread, ring buffer,
  `play_queue.c` (forwarder + UI mirror used by the local baseline), atempo
  speed control, karaoke vocal removal (`karaoke.c`, band-limited centre
  cancellation, runs before the equalizer), 10-band equalizer, FFT visualizer
  data, `backend_ops.c` and the
  PipeWire / PulseAudio / ALSA outputs
- **lyrics/**: Core lyrics engine — discovery, embedded (FFmpeg) and external
  `.lrc` parsing, source preference, timeline and page building
- **ui/**: **Front end** ncurses UI — event loop, controls, settings, menus,
  playlist/queue views, favorites, history, browse views, layout
  (`layout.c`, `layout_spec.c`, `layout_geometry.c`, `layout_state.c`),
  `vinyl.c` (shared colored/glyph vinyl animation and the standalone pane), progress,
  visualizer drawing, `lyrics.c` (rendering only — the engine lives in the
  core), the text-cover module `ui/cover/` (`text_cover.c`: braille/ASCII/half-block
  character covers for `ter-music cover` and `Info.GetCoverArt`; `cover_image.c`:
  PNG/JPEG decode, grayscale and resize; `album_cover.h`: current-track cover path
  and cache declarations, defined in `playlist/playlist.c`), dialogs, mouse
  (`mouse.c`: ncurses edge layer that measures the panes; `mouse_routing.c`:
  ncurses-free ownership rules, unit-tested in `scripts/test/test_mouse_routing.c`;
  `context_menu.c`/`context_menu_main.c`: right-click menu stack, rendering and
  the main-list menu; `context_menu_layout.c`: ncurses-free placement/scroll
  maths, unit-tested in `scripts/test/test_context_menu_layout.c`; `menu_anim.c`:
  menu enter/exit state machine, ghost snapshots and overlay composition;
  `menu_anim_math.c`: ncurses-free motion geometry/easing/tier maths, unit-tested
  in `scripts/test/test_menu_anim_math.c`),
  scrollbar, shared widgets
- **media/**: Core D-Bus session — `session.c` (bus name, introspection,
  dispatch), `rpc_common.c` (reply helpers, paging), `rpc_info.c`,
  `rpc_control.c`, `rpc_queue.c`, `rpc_lyrics.c`, `rpc_config.c`, plus the
  MPRIS media player interface
- **info/info.c**: Playback info snapshot and rendering (text, JSON, braille /
  ASCII / half-block cover cache) shared by `ter-music show`, the `Info`
  interface and the TUI info pane's field rows
- **config/**: Configuration subsystem — `config.c` (XML load/save via libxml2,
  versioned migrations, defaults), `config_json.c` + `migration.c` (v1
  `config.json` → XML), schema constants, `crypto.c` (password encryption for
  the front-end remote store)
- **playlist/**: **Front end** playlist loading and metadata — recursive
  directory scan, FFmpeg + native APEv2 tag reading, CUE sheet detection and
  encoding auto-detect, album art extraction with MRU cover cache,
  `playlist_queue.c` (the content → path-queue bridge)
- **library/**: **Front end** SQLite library — schema (tracks with FTS5,
  favorites, history, playlists), scan engine, CRUD, and `browser/browser.c`
  (artists → albums → tracks navigation)
- **remote/**: **Front end** remote music sources — `remote.c` (SMB/SFTP/FTP/
  WebDAV/HTTP via libcurl), `remote_store.c` (server list in
  `<configdir>/remote.xml`), `remote_cache.c` (background downloader + local
  cache); `ui/remote_view.c` is the *Settings → Remote Device* page
- **app/open.c**: Shared path-opening and session-restore primitives used by
  the TUI and by CLI `play`
- **util/json.c**: Small bounded JSON reader/writer shared by the
  `Lyrics`/`Info` interfaces and the queue payloads
- **util/utf8.c**: UTF-8 helpers needed by both planes (core lyrics, CLI output)
- **search/search.c**: Async search with pinyin support (front end)
- **i18n/**, **logger/**: Language packs and the logging subsystem (shared)

## 8. License

This project is licensed under the [GNU General Public License v3.0](LICENSE) open source license. You are free to use, modify, and distribute this software, but modified derivative works must also be open sourced under the same license.

## 9. Disclaimer

Ter-Music is a pure audio playback tool that does not provide, host, or distribute any audio files or other copyrighted content. Users must provide their own legally obtained audio files. The software's local and remote playback features are designed solely for playing media files that users have lawfully acquired.

All copyright and intellectual property rights related to audio content played using this software belong to their respective owners. Any copyright disputes arising from the use of this software to play audio content are solely the responsibility of the user. The developer assumes no liability for any copyright or other legal issues arising from the use of this software.

## 10. Author

- **Author**: 浣软科技（HuanSoft）
- **Email**: <yxzl666xx@outlook.com>

## 11. Acknowledgments

Special thanks to the following contributors for their valuable contributions:

- **@guanzi008** - For extensive improvements including Debian packaging metadata, optional MPRIS media session integration, DEB packaging optimization, UTF-8 input fixes, settings navigation fixes, mouse interactions, playlist state management, directory queue support, playback resume, performance optimizations, audio visualizer enhancements, and Chinese UI improvements
- **@Zeta** - For adding Arch Linux support
