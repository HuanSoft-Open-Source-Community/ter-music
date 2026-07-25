<div align="center">

# Ter-Music - Terminal Music Player

![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
![Docker](https://img.shields.io/badge/Docker-Supported-2496ED.svg)
![Python](https://img.shields.io/badge/Python-3.x-3776AB.svg)
![Shell](https://img.shields.io/badge/Shell-Bash-4EAA25.svg)
![Linyaps](https://img.shields.io/badge/Linyaps-Supported-8A2BE2.svg)

</div>

**Other Languages:**
- [中文（现代版）](translations/README_zh-CN_Modern.md)
- [中文（文言版）](translations/README_zh-CN_Legacy.md)

## 1. Project Introduction

### 1.1 Core Features

Ter-Music is a lightweight, terminal-based command-line music player designed for Linux systems. It utilizes FFmpeg for audio decoding, supports PipeWire/PulseAudio/ALSA audio output (auto-detected at runtime), and provides a beautiful text-based user interface through ncursesw.

**Key Features:**

- 🌐 **Remote Music Playback**: Supports SMB, SFTP, FTP, WebDAV, HTTP protocols for playing music from remote servers and NAS devices
- 🎵 **Supports Multiple Audio Formats**: MP3, WAV, FLAC, OGG, M4A, AAC, WMA, APE, OPUS, **WV (WavPack)** and other popular formats
- 🎼 **CUE Split-track Support**: CUE sheet parsing for FLAC/APE/WV, with auto-detect encoding (GBK/BIG5/Shift-JIS)
- 📝 **LRC Lyrics Synchronization**: Automatically loads and synchronizes lyrics, highlights current line with playback progress; **embedded lyrics** (FFmpeg/APE) take priority over external .lrc files. Switch between embedded/external sources in lyric seek mode (Ctrl+L → Tab)
- 🎛️ **10-band Graphic Equalizer**: ISO frequencies (31Hz-16kHz) with biquad IIR DSP, ±12dB range, visual bar UI in settings, with **EQ presets** and **soft-clip** support
- 🎶 **17 Playback Modes**: From basic (Sequential, Single Repeat, List Repeat, Shuffle) to advanced (Folder/Album/Artist-based variants)
- ⚡ **Playback Speed Control**: Supports 0.75x, 1.0x, 1.25x, 1.5x, 2.0x, 3.0x speed adjustment for efficient listening
- 📚 **Music Library**: SQLite-backed music library with FTS5 full-text search, **recursive directory scan**, browse by artist/album/genre, incremental tracking
- 📋 **Play Queue**: Dedicated queue UI with sequence numbers, now-playing indicator, reordering and persistence
- 🗂️ **Playlist Management**: Supports user-defined creation of multiple playlists
- ❤️ **Favorites Feature**: Bookmark favorite songs for quick access
- 🕒 **Playback History**: Automatically records playback history for easy review
- 📂 **Directory History**: Records recently visited music directories
- 🎨 **Expanded Color Palette**: 24 preset themes + 1 custom slot with paired-color guard
- 💾 **Persistent Storage**: SQLite-based storage (favorites, history, playlists) with automatic JSON migration from v1
- ⌨️ **Keyboard Shortcuts**: Full keyboard operation, efficient and convenient
- 📊 **Real-time Progress Bar**: Smooth playback progress display and seeking
- 🎨 **Album Cover Display**: Supports album art rendering in terminal (PNG/JPEG via braille art or chafa), can be toggled on/off in Settings

### 1.2 Design Philosophy

Ter-Music follows the **simple, efficient, native** design philosophy:

- **Lightweight**: No dependency on heavy desktop environments, extremely low resource usage
- **Terminal Native**: Completely text-based UI, suitable for servers, embedded devices, and users who prefer terminal workflows
- **Modular Design**: Clear module separation, easy to maintain and extend
- **Unix Philosophy**: Do one thing well, work well with other tools
- **No Tracking**: Does not collect any user data, respects privacy

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
| 🌐 **Remote Playback**: Play music via SMB/SFTP/FTP/WebDAV remote protocols | <br /> |
| 🎨 **Album Cover**: Terminal album art display, toggleable in Settings | <br /> |

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
| `libcurl-devel` | 7.0+ | Remote music playback (SMB/SFTP/FTP/WebDAV) |
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
| `dbus-devel` | MPRIS D-Bus media session integration |

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
git clone https://github.com/yanxizhulin/ter-music.git
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

- [Build Guide](BUILD_GUIDE.md) - Detailed build script usage guide
- [Scripts README](../scripts/README.md) - Quick reference for all build scripts

The following formats are supported:
- **AppImage** - Universal Linux package format
- **Portable Package** - Self-contained archive with all dependencies
- **RPM Package** - For Fedora/RHEL-based distributions
- **DEB Package** - For Debian/Ubuntu-based distributions
- **Linyaps Package** - For deepin/UOS systems
- **Arch Linux Package** - For Arch Linux and derivatives

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

```bash
ter-music [OPTIONS]

Options:
  -o, --open <path>    Open specified music directory directly on startup
  -d, --debug          Enable debug logging (outputs to ter-music-debug.log)
  -h, --help           Show help information
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
| `F7` | Toggle language (Chinese/English) |
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
| `Esc` + `7` | Toggle language (Chinese/English) |
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

### 5.8 Configuration File

### 5.8 Configuration File

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
- `remote_connections`: Saved remote server connections (SMB/SFTP/FTP/WebDAV)
- Color theme configuration: 24 preset themes + 1 custom slot, foreground and background colors for all UI elements
- Equalizer configuration: 10-band gains, pre-amp, enable/disable

The program automatically saves configuration; changes take effect immediately after modification.

### 5.9 Data Storage Location

All user data is stored in the `~/.config/ter-music/` directory:

```
~/.config/ter-music/
├── config.xml       # Configuration file (v2.2 XML format, parsed via libxml2)
├── library.db       # SQLite database (music library, favorites, playlists, history)
├── queue.txt        # Playback queue persistence
├── album_cover_cache/   # Album cover image cache
└── config.json.bak # Auto-backup of v1 config on first migration (if present)
```

**Note:** The v1.0 JSON-based storage (`config.json`, separate `favorites`, `history`, `dir_history`, `playlists/`) has been fully replaced by the SQLite database `library.db`. Migration is automatic on first v2.0 startup.

### 5.10 Basic Usage Flow

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

### 5.11 Shortcut Cheat Sheet

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

### 5.12 Terminal Resizing

Ter-Music supports terminal window resizing. When you resize the terminal, the program will automatically readjust the layout and redraw the interface.

### 5.13 Exit the Program

There are three ways to exit:

- Press `q` in the main interface
- Press `Ctrl+C` / `Ctrl+D` / `Ctrl+\` (the program handles SIGHUP/SIGTERM/SIGINT gracefully and exits cleanly)
- Select "Exit" in the options menu (which is the `F9` key)

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

Ter-Music adopts a modular design, main modules include:

> **Source files** are organized under `src/org.yxzl.ter-music/<module>/`; **public headers** under `include/org.yxzl.ter-music/<module>/`.

- **main.c**: Program entry, command-line argument processing
- **ui/**: User interface subsystem — rendering, layout, input handling
  - **ui.c**: Main event loop, view switching, input dispatch
  - **controls.c**: Control bar (play/pause/next/prev/volume/speed/mode popups)
  - **settings.c**: Settings view with sidebar and right-side selection menus
  - **menus.c**: Menu bar, function key handling (F1-F9), popup management
  - **playlist_render.c**: File browser and queue view rendering
  - **playlist_view.c**: Playlist management view
  - **favorites.c**: Favorites view
  - **history.c**: Playback history view
  - **info_view.c**: About/info view
  - **help_view.c**: Help view
  - **language_view.c**: Language selection view (i18n language pack browser)
  - **layout.c**: Terminal layout management (resize handling)
  - **progress_ui.c**: Progress bar rendering (suspend on popup active)
  - **visualizer.c**: Audio spectrum visualizer
  - **lyrics.c**: Lyrics loading, parsing, synchronized display (embedded lyrics support)
  - **braille_art.c**: Braille art rendering for album cover display in terminal
  - **image_loader.c**: Album cover image loading and processing (PNG/JPEG)
  - **dialog.c**: Dialog boxes
  - **mouse.c**: Mouse interaction handling
  - **scrollbar.c**: Reusable scrollbar module for UI panes
  - **utf8.c**: UTF-8 string utilities
  - **util.c**: Shared UI utilities (sidebar, palette, etc.)
- **audio/**: Audio engine — decoding, playback, DSP
  - **audio.c**: Core audio control, volume, play mode cycling, audio backend management
  - **playback_thread.c**: Dedicated playback thread, FFmpeg decoding loop, EOS handling
  - **segment_buffer.c**: Ring buffer for PCM data, bounds for RSS (~20MB)
  - **play_queue.c**: Play queue with Fisher-Yates shuffle, 17 play mode navigation
  - **atempo.c**: FFmpeg atempo filter for speed adjustment
  - **equalizer.c**: 10-band ISO graphic equalizer with biquad IIR filters
  - **audio_visualizer.c**: FFT-based spectrum data extraction for visualizer
  - **backend_ops.c**: Unified backend operations (volume, latency, device init)
  - **backend/pipewire.c**: PipeWire audio output (dlopen-based, no compile-time dep)
  - **backend/pulse.c**: PulseAudio audio output
  - **backend/alsa.c**: ALSA audio output
- **playlist/**: Playlist loading, metadata, CUE parsing
  - **playlist.c**: Directory scanning (**recursive** sub-directory scan), metadata reading (FFmpeg + native APEv2 tags),
    CUE sheet detection
  - **cue_parser.c**: CUE sheet line-by-line parser for split-track support
  - **encoding.c**: CUE file encoding auto-detect and conversion (iconv)
  - **ape_tag.c**: Native APEv2 tag parser for enhanced metadata extraction
- **library/**: SQLite music library
  - **library.c**: Database schema (tracks with FTS5, favorites, history, playlists),
    scan engine, CRUD operations
  - **browser/browser.c**: Library browser UI (artists → albums → tracks navigation)
- **config/**: Configuration subsystem
  - **config.c**: XML config load/save via libxml2, schema v2.2
  - **migration.c**: v1 config.json → v2 config.xml migration
  - **schema.h**: XML element/attribute constants
  - **crypto.c**: Remote connection password encryption/decryption
- **remote.c**: Remote music playback support (SMB/SFTP/FTP/WebDAV/HTTP protocols)
- **media_session.c**: MPRIS D-Bus media session integration (optional)
- **search.c**: Async search with pinyin support
- **logger.c**: Logging subsystem

## 8. License

This project is licensed under the [GNU General Public License v3.0](LICENSE) open source license. You are free to use, modify, and distribute this software, but modified derivative works must also be open sourced under the same license.

## 9. Disclaimer

Ter-Music is a pure audio playback tool that does not provide, host, or distribute any audio files or other copyrighted content. Users must provide their own legally obtained audio files. The software's local and remote playback features are designed solely for playing media files that users have lawfully acquired.

All copyright and intellectual property rights related to audio content played using this software belong to their respective owners. Any copyright disputes arising from the use of this software to play audio content are solely the responsibility of the user. The developer assumes no liability for any copyright or other legal issues arising from the use of this software.

## 10. Author

- **Author**: Yan Xi Zhu Lin
- **Email**: <yxzl666xx@outlook.com>

## 11. Acknowledgments

Special thanks to the following contributors for their valuable contributions:

- **@guanzi008** - For extensive improvements including Debian packaging metadata, optional MPRIS media session integration, DEB packaging optimization, UTF-8 input fixes, settings navigation fixes, mouse interactions, playlist state management, directory queue support, playback resume, performance optimizations, audio visualizer enhancements, and Chinese UI improvements
- **@Zeta** - For adding Arch Linux support
