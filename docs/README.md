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

## 1. Project Introduction

### 1.1 Core Features

Ter-Music is a lightweight, terminal-based command-line music player designed for Linux systems. It utilizes FFmpeg for audio decoding, supports PipeWire/PulseAudio/ALSA audio output (auto-detected at runtime), and provides a beautiful text-based user interface through ncursesw.

**Key Features:**

- 🌐 **Remote Music Playback**: SMB, SFTP, FTP, WebDAV and HTTP sources. The **front end** lists them and downloads each track into a local cache, then hands the local files to the core (which only ever plays local files)
- 🎵 **Supports Multiple Audio Formats**: MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS, **WV (WavPack)** and other popular formats
- 🎼 **CUE Split-track Support**: CUE sheet parsing for FLAC/APE/WV, with auto-detect encoding (GBK/BIG5/Shift-JIS)
- 📝 **LRC Lyrics Synchronization**: Automatically loads and synchronizes lyrics, highlights current line with playback progress; **embedded lyrics** (FFmpeg/APE) take priority over external .lrc files. Switch between embedded/external sources in lyric seek mode (Ctrl+L → Tab)
- 🎛️ **10-band Graphic Equalizer**: ISO frequencies (31Hz-16kHz) with biquad IIR DSP, ±12dB range, visual bar UI in settings, with **EQ presets** and **soft-clip** support
- 🎶 **17 Playback Modes**: From basic (Sequential, Single Repeat, List Repeat, Shuffle) to advanced (Folder/Album/Artist-based variants)
- ⚡ **Playback Speed Control**: Supports 0.75x, 1.0x, 1.25x, 1.5x, 2.0x, 3.0x speed adjustment for efficient listening
- 📚 **Music Library**: SQLite-backed music library with FTS5 full-text search, **recursive directory scan**, browse by artist/album/genre, incremental tracking
- 📋 **Play Queue**: Dedicated queue UI with sequence numbers, now-playing indicator and reordering; the queue itself belongs to the front end (the core only executes the path list it is given), and it is rebuilt from your content on the next start
- 🗂️ **Playlist Management**: Supports user-defined creation of multiple playlists
- ❤️ **Favorites Feature**: Bookmark favorite songs for quick access
- 🕒 **Playback History**: Automatically records playback history for easy review
- 📂 **Directory History**: Records recently visited music directories
- 🎨 **Expanded Color Palette**: 24 preset themes + 1 custom slot with paired-color guard
- 💾 **Persistent Storage**: SQLite-based storage (favorites, history, playlists) with automatic JSON migration from v1
- ⌨️ **Keyboard Shortcuts**: Full keyboard operation, efficient and convenient
- 📊 **Real-time Progress Bar**: Smooth playback progress display and seeking
- 🎨 **Album Cover Display**: Supports album art rendering in terminal (PNG/JPEG via braille art or chafa), can be toggled on/off in Settings
- 🖥️ **CLI Mode & Background Playback**: Full command-line subcommands (`play`/`pause`/`seek`/`volume`/`speed`/`mode`/`show`/`daemon`) with a detached background playback daemon that keeps playing after you close the terminal — including inside Linyaps packages, where background playback is started through D-Bus activation or the shipped systemd user service
- 📋 **Configurable Info Display**: The basic info block, braille/ASCII text cover, progress line and two lyric lines (current + next) printed by `ter-music show` are configurable in the TUI settings
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
| 🎨 **Beautiful TUI**                         | Split-column layout, colored interface, supports terminal size adaptation |
| 🌍 **UTF-8 Chinese Support**                  | Perfect UTF-8 encoding support, correctly displays Chinese song metadata |
| 🔄 **Persistent Storage**                     | SQLite-backed storage — configuration, library, favorites, playlists, and history, all in one database |
| 🎯 **Multiple View Switching**: Quickly switch between settings, history, playlist, library, language and other views via F2-F8 function keys | <br /> |
| ⚡ **Responsive UI**: 100 FPS refresh rate, smooth progress bar updates | <br /> |
| 🔧 **CMake Build**: Modern build system, good cross-platform compatibility | <br /> |
| 🔊 **Audio Backend**: Supports PipeWire, PulseAudio and ALSA output, auto-detected at runtime (PipeWire > Pulse > ALSA) | <br /> |
| 🎛️ **10-band Equalizer**: ISO graphic equalizer with visual bar chart UI in settings | <br /> |
| ⏩ **Playback Speed Control**: 6 levels of speed adjustment (0.75x-3.0x), switchable during playback | <br /> |
| 📊 **Info Bar**: Displays sample rate, bit depth, bitrate and codec of current track | <br /> |
| 🌐 **Remote Playback**: SMB/SFTP/FTP/WebDAV/HTTP sources, browsed and downloaded by the front end into a local cache | <br /> |
| 🎨 **Album Cover**: Terminal album art display, toggleable in Settings | <br /> |
| 🎵 **MPRIS / Lyrics API**: Desktop media controls, album art via `mpris:artUrl`, and a JSON lyrics API over D-Bus | <br /> |
| 🖥️ **CLI Mode**: `ter-music play/pause/next/seek/volume/speed/mode/show` works against the running instance; `show` prints the configurable info block | <br /> |
| 🌙 **Background Playback**: `ter-music daemon start` runs headless playback; control it from any terminal, no TUI needed | <br /> |
| 🖼️ **Text Cover over D-Bus**: `Info.GetCoverArt` returns braille or ASCII cover art for other applications | <br /> |
| 📦 **Linyaps CLI**: the same CLI runs inside the Linyaps container (`ll-cli run org.yxzl.ter-music -- ter-music …`); on-demand background playback via D-Bus activation, always-on via a systemd user service | <br /> |

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
yay -S ter-music-cn

# Using paru (AUR helper)
paru -S ter-music-cn
```

**Install using ZPM (MengXi OS Package Manager):**

```bash
# First, install ZPM if not already installed
git clone https://aur.archlinux.org/zetapm.git
cd zetapm
makepkg -si

# Then install ter-music-cn using ZPM
zpm -S ter-music-cn
```

**Manual Installation from AUR:**

```bash
git clone https://aur.archlinux.org/ter-music-cn.git
cd ter-music-cn
makepkg -si
```

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
`org.mpris.MediaPlayer2.ter_music` (started by `daemon start`, by D-Bus
activation or by the TUI), so they work from any terminal, script or window
manager shortcut. `play` additionally acts as a front end: it scans the path in
its own process and delivers the resulting queue to the core.

| Command | Description |
| ------- | ----------- |
| `play [PATH] [--index N] [--mode MODE] [--no-daemon]` | Scan the path, deliver the queue to the core and play; starts the core when none is running |
| `pause` / `resume` / `toggle` / `stop` / `next` / `prev` | Basic transport control |
| `seek <+SECONDS\|-SECONDS\|mm:ss\|N%>` | Relative, absolute or percentage seek |
| `volume [0-100\|+N\|-N]` | Query or set volume |
| `speed [0.5-3.0]` | Query or set playback speed |
| `mode [NAME\|0-16]` | Query or set the play mode (stable names such as `list_repeat`, `folder_shuffle_repeat`) |
| `show [OPTIONS]` | Print the current info block (basic info / text cover / progress / two lyric lines) |
| `daemon start\|foreground\|stop\|restart\|status\|reload` | Playback core management (`daemon stop --all` also stops `--force` secondary instances) |
| `version` / `help` | Version / usage |

`show` options (all of them override the stored TUI settings for that call):

| Option | Description |
| ------ | ----------- |
| `--json` | Print the full JSON snapshot (`Info.GetInfo`) |
| `--watch[=MS]` | Live refreshing view (default 500 ms, `Ctrl+C` to exit, requires a TTY) |
| `--one-line` | Single-line output, suitable for status bars |
| `--full` / `--compact` / `--preset full\|compact\|custom` | Info display preset |
| `--fields a,b,c` | Basic info fields: `state,mode,index,queue,title,artist,album,format,path,volume,speed` |
| `--cover` / `--no-cover`, `--cover-size WxH`, `--charset braille\|ascii` | Text cover options (4-40 columns, 2-20 rows) |
| `--progress bar\|time\|percent\|time+percent`, `--no-progress` | Progress line style |
| `--lyrics 0\|1\|2` | Lyric lines: off / current line / current + next line |
| `--width N` | Output width (defaults to the terminal width) |
| `--bus NAME` | Target a specific instance bus name (defaults to the primary instance) |

**Exit codes:** `0` success; `1` usage error; `3` no running instance;
`4` D-Bus unavailable; `5` rejected by the instance.

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
ll-cli run org.yxzl.ter-music -- ter-music show
ll-cli run org.yxzl.ter-music -- ter-music pause
ll-cli run org.yxzl.ter-music -- ter-music play ~/Music

# Reading state while a container is already running (see the notes below)
ll-cli enter org.yxzl.ter-music -- /opt/apps/org.yxzl.ter-music/files/bin/ter-music show

# Convenience wrapper for interactive shells (put it in ~/.bashrc)
ter-music() { ll-cli run org.yxzl.ter-music -- ter-music "$@"; }
```

**Background playback.** A self-detached process (`ter-music daemon start`)
would be recycled together with the container, so it is disabled inside the
sandbox: `daemon start` and `play` instead ask the session bus to activate the
background player, and fall back to an explanatory message with the exact
commands when activation is unavailable. Three supported ways exist:

| Option | How to use | Behaviour |
| ------ | ---------- | --------- |
| D-Bus activation (on demand) | `ter-music play <path>` or `ter-music daemon start` | The session bus starts `ll-cli run org.yxzl.ter-music -- ter-music daemon foreground --no-autoplay` on the host and the command is forwarded to it |
| systemd user service (always on) | `systemctl --user enable --now org.yxzl.ter-music` (on the host) | The player starts with the session and keeps playing in the background |
| Foreground | `ll-cli run org.yxzl.ter-music -- ter-music play <path> --foreground` | Plays in the foreground; the container lives as long as the command runs (tmux/screen friendly) |

Notes:

- `systemctl --user` is not reachable from inside the container, so the service
  must be enabled from the host shell; `ter-music help` prints the same hints
  when it detects the sandbox.
- `ter-music daemon stop` works as usual and stops the instance; the container
  is reclaimed afterwards, so `ll-cli ps` no longer lists the application.
- `ll-cli run` maps every failure exit code to `255`; host-side scripts should
  inspect `ter-music show --json` (field `"running"`) instead of relying on
  exit codes.
- When an application container is **already running** (the background daemon, or
  the TUI), `ll-cli run … -- <command>` attaches that command's output to the
  *running container's* stdout, so your terminal stays empty. Read it with
  `journalctl --user -u org.yxzl.ter-music`, or use `ll-cli enter`, which keeps
  the terminal attached:
  `ll-cli enter org.yxzl.ter-music -- /opt/apps/org.yxzl.ter-music/files/bin/ter-music show`.
  That entered environment carries no `DBUS_SESSION_BUS_ADDRESS`, so the CLI
  resolves the session bus itself (`$XDG_RUNTIME_DIR/bus`, then
  `/run/user/<uid>/bus`); an explicitly set address always wins.
- Host paths are visible inside the container as-is (`$HOME`, `/tmp`, `/media`),
  and the extracted cover art stays readable by host applications through
  `mpris:artUrl`.
- Settings, library and the playback session are stored in
  `$XDG_CONFIG_HOME/ter-music`. If the Linyaps runtime redirects the XDG
  variables (see the Linyaps FAQ, "where is application data saved"), they land
  in `~/.linglong/org.yxzl.ter-music/…` instead, keeping Linyaps and non-Linyaps
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
ter-music --bus org.yxzl.ter_music.instance1   # address a specific instance
```

**Leaving the front end.** Closing the TUI does **not** stop playback: the core
keeps running and playing, and `ter-music show` / `ter-music next` still reach
it from any terminal. Set `core_exit_when_no_frontend` to `true` in
`config.xml` (or with `Config.Set` over D-Bus) when the core should instead exit
by itself after the last front end has been gone for a grace period (10 s); a
core that no front end ever attached to stays up.

**Reconnect.** When the core disappears (crash, `kill`, logout), the front end
does not exit: it marks itself offline, retries with exponential backoff
(1/2/4/8/15/30 s), and re-attaches — and re-sends its content queue — as soon as
a core answers again. Pressing `R` in the TUI restarts a dead core and re-pushes
the queue immediately.

**Compatibility.** The bus surface is `api_version 4`. Content interfaces
(`Playlist`, `Library`, `Favorites`, `History`, `DirHistory`, `Remote`) are
gone: they belong to the front end. Third-party clients should use the
`Lyrics`, `Info`, `Control`, `Queue` and `Config` interfaces — see
[API_DBUS_en_US.md](API_DBUS_en_US.md).

### 5.3 Interface Layout

After launching, you will see a three-column layout:

```
┌────────────────────────────┬───────────────┐
│  Play List                 │  [Spectrum]   │
│                            ├───────────────┤
│  Song List Area            │  Lyrics       │
│                            │               │
│                            │ Lyrics Display│
│                            │  (Vinyl)      │
│                            │               │
├────────────────────────────┤               │
│   Controls                 │               │
│   [==========>-----]       │               │
│  [<<] [Play/Pause] [>>]    │               │
│  [Stop] [Loop:Off] [Volume]│               │
└────────────────────────────┴───────────────┘
Menu: Options Menu
```

- **Top Left**: Playlist area, displays audio files (use Tab to toggle between file browser and playback queue)
- **Bottom Left**: Control bar, contains playback control buttons and progress bar
- **Right**: Lyrics display area, synchronously displays lyrics for the currently playing song; also shows album cover (braille art) when enabled in Settings
- **Bottom**: Options menu, includes settings, playback history, favorites, about, exit, etc.

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

### 5.7 Lyrics Display

Ter-Music supports automatic loading of LRC format lyrics files:

- **Embedded lyrics take priority**: The player first reads embedded lyrics from the audio file (FFmpeg/APE tags). If none are found, it falls back to external `.lrc` files.
- Lyrics files should be placed in the same directory as the audio file
- Lyrics filename should match the audio filename, with extension `.lrc`
- Example: `song.mp3` → `song.lrc`
- The program automatically highlights current lyrics based on playback time
- **Switch lyrics source**: Press `Ctrl+L` to enter lyric seek mode, then press `Tab` to switch between embedded/external lyrics
- If no lyrics file is found, the lyrics area will display "No lyrics loaded"

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
`org.yxzl.ter_music.Lyrics` with the methods `GetLyrics`, `GetDocument` and
`SetSource` (switch between embedded and external lyrics), and the signal
`LyricsChanged`. See [Lyrics API (English)](API_LYRICS_en_US.md) for the JSON
schema and examples.

Four more interfaces are published on the same object path so that other
applications can read track data, text cover art and progress, and drive the
player:

- `org.yxzl.ter_music.Info` (read-only): `GetInfo`, `GetTrackInfo`,
  `GetProgress`, `GetLyricsLines`, `GetCoverArt(charset, cols, rows)`,
  `GetDisplay(options)` (the exact text printed by `ter-music show`),
  `InstanceInfo`, plus the signals `InfoChanged`, `ProgressChanged` (at most
  1 Hz) and `CoverChanged`.
- `org.yxzl.ter_music.Control`: transport, seek, volume, speed, play mode,
  `ReloadConfig` and `Quit`. Loading content is **not** part of it — the front
  end delivers a queue instead.
- `org.yxzl.ter_music.Queue`: the core's **path queue** —
  `Set`/`Append`/`InsertAfter`/`RemoveAt`/`MoveUp`/`MoveDown`/`Clear`/
  `Shuffle`/`PlayAt` and paged `Get(offset, count)`, with the `QueueChanged`
  signal (entries, current position, revision). Local paths only; at most 500
  entries per write and 1000 per read.
- `org.yxzl.ter_music.Config`: read and patch the core configuration (the only
  writer). Remote server entries are **not** part of it: they belong to the
  front end (see below).
- `Info.GetInfo` advertises `core.api_version` (currently `4`) plus the
  implemented method list, so clients can check compatibility before using
  anything else. Version 3 removed the `Remote` interface and the
  `track.is_remote` field and made every path argument local-only; version 4
  replaced the content interfaces (`Playlist`, `Library`, `Favorites`,
  `History`, `DirHistory`) with the path-based `Queue` — content now lives in
  the front end.
- `org.freedesktop.DBus.Introspectable` and `org.freedesktop.DBus.Peer` are
  implemented, so `busctl --user introspect` / `gdbus introspect` work.
- MPRIS metadata additionally carries `xesam:url` (always a `file://` URI,
  including for tracks downloaded from a remote source) and
  `xesam:trackNumber`; `OpenUri` accepts local files only.
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
- `show_album_cover`: Show album cover art in the lyrics panel (0/1)
- `show_lyrics_panel`: Show lyrics panel (0/1)
- `default_playback_speed`: Default playback speed (0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
- `default_play_mode`: Default play mode (0=Sequential, 1=Single Repeat, 2=List Repeat, 3=Shuffle Once, 4=Shuffle Repeat, ...)
- `advanced_play_modes_enabled`: Enable advanced folder/album/artist play modes (0/1)
- `lyrics_alignment`: Lyrics text alignment (0=Left, 1=Center, 2=Right)
- `clear_history_on_startup`: Clear playback history on startup (0/1)
- `resume_last_playback`: Resume playback from last position (0/1)
- `seamless_preload`: Pre-decode next track at end of current for gapless playback (0/1)
- `ui_language`: Interface language (string ID: "zh_CN", "en_US", etc. Set via F7 language view)
- `volume_percent`: Default volume percentage (0-100)
- `audio_latency_ms`: Output latency in milliseconds
- `audio_backend`: Audio output backend (0=Auto, 1=PulseAudio, 2=ALSA, 3=PipeWire)
- `sort_mode`: Playlist sort mode (0=Default, 1=Title, 2=Artist, 3=Album, 4=Filename)
- `cue_encoding`: CUE file character encoding (0=Auto, 1=UTF-8, 2=GB18030, 3=GBK, 4=BIG5, 5=Shift-JIS)
- `core_exit_when_no_frontend`: let the core exit by itself once no front end has been attached for 10 s (0/1, default 0 — closing the TUI keeps the music playing)
- Remote server connections (SMB/SFTP/FTP/WebDAV/HTTP) are **not** stored in
  `config.xml`: the front end keeps them in its own `remote.xml` next to it
  (server entries plus password ciphertext, file mode 0600), and caches
  downloaded tracks under `$XDG_CACHE_HOME/ter-music/remote/`
- Color theme configuration: 24 preset themes + 1 custom slot, foreground and background colors for all UI elements
- Equalizer configuration: 10-band gains, pre-amp, enable/disable
- Info display (CLI / D-Bus) configuration, editable in **Settings → Info Display**:
  - `info_preset`: preset (0=Full, 1=Compact, 2=Custom)
  - `info_fields`: bitmask of the basic info fields (1=State, 2=Mode, 4=Index, 8=Queue, 16=Title, 32=Artist, 64=Album, 128=Format, 256=Path, 512=Volume, 1024=Speed; 2047=all)
  - `info_show_cover`: print the braille/ASCII text cover (0/1)
  - `info_cover_cols` / `info_cover_rows`: cover size in character columns/rows (4-40 / 2-20)
  - `info_cover_charset`: cover charset (0=Braille, 1=ASCII)
  - `info_show_progress`: print the progress line (0/1)
  - `info_progress_style`: progress style (0=Bar+Time, 1=Time, 2=Percent, 3=Time+Percent)
  - `info_lyrics_lines`: lyric lines (0=Off, 1=Current, 2=Current+Next)

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
4. Rejects archives that would overwrite built-in languages (`zh_CN`, `en_US`)
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
| **Global** | `q` | Exit program |
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
| **Lyrics** | `Ctrl+L` | Toggle lyric seek mode |
| <br /> | `↑`/`↓` | Select prev/next line (in seek mode) |
| <br /> | `Enter`/`Space` | Jump to selected line (in seek mode) |

### 5.13 Terminal Resizing

Ter-Music supports terminal window resizing. When you resize the terminal, the program will automatically readjust the layout and redraw the interface.

### 5.14 Exit the Program

There are three ways to exit the front end:

- Press `q` in the main interface
- Press `Ctrl+C` / `Ctrl+D` / `Ctrl+\` (the program handles SIGHUP/SIGTERM/SIGINT gracefully and exits cleanly)
- Select "Exit" in the options menu (which is the `F9` key)

Leaving the TUI does not stop the music: playback lives in the core, so the
current track keeps playing and `ter-music show` / `ter-music next` still work
from any terminal. Stop it explicitly with `ter-music daemon stop`, make the
core exit on its own by setting `core_exit_when_no_frontend` to `true`, or let
it notice that it can no longer be reached at all — the core exits by itself
when its session bus disappears (see
[5.2.3 Front End and Core](#523-front-end-and-core)).

## 6. Frequently Asked Questions

**No sound output**
- The audio backend auto-detects in order: PipeWire → PulseAudio → ALSA. Run `pactl info` or `pw-cli info` to check which service is active
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
| **Core / playback service** | `ter-music daemon foreground` (started by `daemon start`, by D-Bus activation or by a systemd user service) | Audio device and decoding, play queue execution (**paths only**), transport, volume / speed / play mode, equalizer, current-track info (lyrics, text cover, spectrum), the published `Lyrics` / `Info` / `Control` / `Queue` / `Config` interfaces, front-end registry and heartbeat, configuration |
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
- **D-Bus surface, `api_version 4`** — `Queue.Set/Append/InsertAfter/RemoveAt/
  MoveUp/MoveDown/Clear/Shuffle/PlayAt/Get` accept **local paths only** (remote
  URLs are rejected), batch at most 500 entries per call and page reads at
  `RPC_PAGE_MAX` (1000) entries; `QueueChanged` announces count / current
  position / revision so any number of front ends stay in sync.
- **Front-end registration** — every front end registers and heartbeats, which
  is what feeds `core_exit_when_no_frontend` and `Info.GetInfo.frontends`.
  Disconnects are not fatal: the front end backs off (1/2/4/8/15/30 s),
  re-attaches, and re-pushes its content queue, because the core holds no
  content of its own.

### 7.2 Architecture Gates

Three scripts keep the split honest, and all of them run in CI:

| Gate | Command | Rule |
| ---- | ------- | ---- |
| Backend purity | `scripts/test/check-core-purity.sh` | Core directories (`audio config core info lyrics media queue` + `cli/daemon.c`) must contain **no** remote-source symbol and no content/UI reference (playlist, library, search, UI rendering, front-end headers) |
| Front-end purity | `scripts/test/check-ui-purity.sh` | The UI must reach the playback surface **only** through the `player` facade — no engine playback globals or engine playback commands |
| Config ownership | `scripts/test/check-config-ownership.sh` | The core owns `config.xml`. The front end must not call `save_config()` / `config_save_to_xml()` at all: it changes its own mirror and persists the difference through the facade (`player_config_persist()`, which is `Config.Set` in remote mode) |

Regression suites accompany them: `scripts/test/run-unit-tests.sh` (path queue,
play-queue contract, lyrics parsing, JSON reader, configuration diff) plus the
end-to-end scripts `dbus-rpc-check.sh`, `config-migration-check.sh`,
`lifecycle-e2e.sh`, `offline-reconnect-e2e.sh`, `multi-frontend-e2e.sh`,
`paging-deepdir-e2e.sh` and `remote-frontend-e2e.sh`, and the performance probe
`perf-check.sh` (command-to-visible-state latency and idle CPU, both with
thresholds; CI records the numbers without gating on them).

### 7.3 Module Map

Ter-Music adopts a modular design, main modules include:

> **Source files** are organized under `src/org.yxzl.ter-music/<module>/`; **public headers** under `include/org.yxzl.ter-music/<module>/`.

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
  speed control, 10-band equalizer, FFT visualizer data, `backend_ops.c` and the
  PipeWire / PulseAudio / ALSA outputs
- **lyrics/**: Core lyrics engine — discovery, embedded (FFmpeg) and external
  `.lrc` parsing, source preference, timeline and page building
- **ui/**: **Front end** ncurses UI — event loop, controls, settings, menus,
  playlist/queue views, favorites, history, browse views, layout, progress,
  visualizer drawing, `lyrics.c` (rendering only — the engine lives in the
  core), braille art, image loader, dialogs, mouse, scrollbar, shared widgets
- **media/**: Core D-Bus session — `session.c` (bus name, introspection,
  dispatch), `rpc_common.c` (reply helpers, paging), `rpc_info.c`,
  `rpc_control.c`, `rpc_queue.c`, `rpc_lyrics.c`, `rpc_config.c`, plus the
  MPRIS media player interface
- **info/info.c**: Playback info snapshot and rendering (text, JSON, braille /
  ASCII cover cache) shared by `ter-music show` and the `Info` interface
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
