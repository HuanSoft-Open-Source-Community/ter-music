<div align="center">

# Ter-Music终端音乐播放器说明
![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
[![CI](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml/badge.svg)](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/HuanSoft-Open-Source-Community/ter-music?sort=semver)](https://github.com/HuanSoft-Open-Source-Community/ter-music/releases)
![Docker](https://img.shields.io/badge/Docker-支持-2496ED.svg)
![Python](https://img.shields.io/badge/Python-3.x-3776AB.svg)
![Shell](https://img.shields.io/badge/Shell-Bash-4EAA25.svg)
![Linyaps](https://img.shields.io/badge/Linyaps-支持-8A2BE2.svg)

</div>

**其他语言版本 / Other Languages:**
- [English](../README.md)
- [中文（文言版）](README_zh-CN_Legacy.md)
- [Lyrics API (English)](../API_LYRICS_en_US.md)
- [D-Bus Info & Control API (English)](../API_DBUS_en_US.md)

## 第一章 产品概述
### 一 核心功能
Ter-Music是一款简洁的终端音乐播放器，专门为Linux系统开发。它借助FFmpeg解码音频、支持PipeWire/PulseAudio/ALSA音频输出（运行时自动检测）、ncursesw构建终端界面，核心功能如下：
- 支持多种音频格式，包括MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、**WV（WavPack）**等，均能完美解码
- **CUE分轨支持**：支持FLAC/APE/WV的CUE分轨，自动检测编码（GBK/BIG5/Shift-JIS）
- 兼容LRC格式歌词，可精准跟随音频进度同步显示；**内嵌歌词**（FFmpeg/APE标签）优先于外部.lrc文件。可在歌词定位模式中切换来源（Ctrl+L → Tab）
- **10段图示均衡器**：ISO标准频段（31Hz-16kHz），双二阶IIR滤波器，±12dB调节范围，设置中可视化条形UI，支持**均衡器预设**和**软限幅**
- **17种播放模式**：从基础（顺序、单曲循环、列表循环、随机一次、随机重复）到高级（按文件夹/专辑/艺术家分组）
- 支持倍速播放，提供0.75x、1.0x、1.25x、1.5x、2.0x、3.0x六档速度调节，高效收听
- **音乐库**：SQLite数据库存储，FTS5全文搜索，按艺术家/专辑/流派浏览，支持**递归目录扫描**和增量跟踪
- **播放队列**：独立队列界面，显示序号、当前播放指示并支持排序；队列本身归前端（核心只执行收到的路径表），下次启动时由你的内容重建
- 支持歌单管理，可创建多个自定义歌单，灵活切换播放
- 支持收藏喜欢的歌曲，方便快速查找播放
- 自动记录播放历史，便于回顾听过的音乐
- 保存最近访问的音乐目录，无需重复查找
- **扩展调色板**：24套预设主题 + 1个自定义槽位，前后角色彩配对保护
- **持久化存储**：SQLite统一存储（收藏、历史、歌单），自动从v1 JSON迁移
- 支持专辑封面显示，可在终端中渲染显示封面图片（设置中可开关）
- 🖥️ **CLI 模式与后台播放**：提供完整的命令行子命令（`play`/`pause`/`seek`/`volume`/`speed`/`mode`/`show`/`daemon`），并可启动脱离终端的后台播放守护进程，关闭终端后音乐依旧继续播放——Linyaps 打包下同样可用，后台播放经 D-Bus 激活或随包提供的 systemd 用户服务启动
- 📋 **可配置信息显示**：`ter-music show` 输出的基本信息块、盲文/ASCII 字符封面、进度行与两行歌词（当前行 + 下一行）均可在 TUI 设置中配置
- **MPRIS 与歌词 API**：通过 D-Bus 提供桌面媒体控制、`mpris:artUrl` 专辑封面、供其他程序读取的开放歌词接口，以及供其他程序查询曲目、进度与字符封面的 `Info` 接口和驱动播放器的 `Control` 接口
- 纯键盘快捷键操作，响应迅速
- 实时显示音频进度条，流畅丝滑，可任意跳转播放位置

### 二 开发宗旨
本播放器的开发遵循**简洁、快速、原生**的核心原则：
- 极致简洁，不依赖复杂的运行环境，占用系统资源极少
- 基于终端原生开发，纯字符界面，适合无图形界面、嵌入式场景，以及习惯命令行操作的用户
- 代码模块化设计，结构清晰，易于维护和功能扩展
- 遵循Unix设计哲学，专注做好音乐播放一件事，可与其他工具无缝配合
- 无任何窥探行为，不记录用户隐私数据，充分尊重用户隐私
- 前后端各司其职：**核心**只提供播放服务（音频设备、传输命令、音量/倍速/播放模式，以及执行前端下发的路径队列）；**前端**掌管文件系统与内容（曲库、扫描、歌单、收藏、历史、远程源与界面）。详见 [5.2.3 前端与核心](#523-前端与核心)

### 三 核心优势
| 优势 | 说明 |
| --- | --- |
| 🚀 资源占用极低 | 内存占用始终不超过10MB，CPU使用率几乎可以忽略不计 |
| 🎨 界面美观 | 分栏布局，层次清晰，可自适应终端窗口大小 |
| 🌍 多语言兼容 | 完美支持UTF-8编码，可正常显示中文等各类字符 |
| 🔄 配置持久化 | 自定义设置、收藏内容、播放历史均自动保存至SQLite数据库，重启后仍保留 |
| 🎯 多视图切换 | 通过F2至F8快捷键，可快速切换设置、历史、歌单、音乐库、语言等视图 |
| ⚡ 响应流畅 | 每秒100帧刷新，音频进度条流转无卡顿 |
| 🔧 CMake构建 | 采用现代构建方式，跨系统兼容性好 |
| 🔊 音频后端 | 支持PipeWire、PulseAudio和ALSA输出，运行时自动检测（PipeWire > Pulse > ALSA） |
| 🎛️ 10段均衡器 | ISO标准图示均衡器，设置中可视化条形图界面 |
| ⏩ 倍速播放控制 | 六档速度调节（0.75x-3.0x），播放中可随时切换 |
| 📊 信息栏 | 实时显示当前音频的采样率、位深、比特率、编码格式 |
| 🌐 远程播放 | 支持SMB/SFTP/FTP/WebDAV/HTTP 音乐源；由**前端**列出并逐曲下载到本地缓存，再把本地文件交给核心播放 |
| 🎨 专辑封面 | 终端专辑封面显示，可在设置中开关 |
| 🎵 MPRIS / 歌词 API | 桌面媒体控制、`mpris:artUrl` 封面，以及基于 D-Bus 的 JSON 歌词接口 |
| 🖥️ CLI 模式 | `ter-music play/pause/next/seek/volume/speed/mode/show` 可直接控制正在运行的实例；`show` 打印可配置的信息块 |
| 🌙 后台播放 | `ter-music daemon start` 以无界面方式播放；可从任意终端控制，无需 TUI |
| 🖼️ 通过 D-Bus 输出字符封面 | `Info.GetCoverArt` 向其他应用程序返回盲文或 ASCII 字符封面 |
| 📦 Linyaps CLI | 同一套 CLI 可在 Linyaps 容器内经 `ll-cli run org.yxzl.ter-music -- ter-music …` 使用；后台播放支持按需 D-Bus 激活与常驻 systemd 用户服务 |

### 四 适用场景
- 无图形界面的Linux系统、嵌入式设备，需要播放音乐但没有窗口界面的场景
- 资源受限的嵌入式硬件，如低配Linux设备
- 习惯命令行操作的开发者，办公时无需切换窗口即可听音乐
- 追求简洁的用户，不喜欢繁琐的图形界面播放器
- 学习C语言、FFmpeg音频处理、ncurses终端界面开发的人群，本项目可作为学习参考

### 五 目标用户
- Linux系统资深用户、命令行爱好者
- 嵌入式开发工程师、系统运维人员
- 追求简洁体验的用户
- 无图形界面但需要播放音乐的使用者
- 学习C语言与音频处理技术的开发者

## 第二章 运行环境要求
### 一 支持的系统
- 兼容系统：Linux内核3.10及以上版本
- 推荐版本：Fedora 30+、Ubuntu 20.04+、Arch Linux最新版本
- 不支持：Windows、macOS（若有用户愿意移植，我们非常欢迎）

### 二 硬件要求
| 硬件 | 最低配置 | 推荐配置 |
| --- | --- | --- |
| **CPU** | 单核1GHz | 双核2GHz及以上 |
| **内存** | 64MB可用空间 | 128MB及以上可用空间 |
| **存储** | 200MB可用空间 | 1024MB及以上可用空间 |
| **声卡** | 需正常运行PulseAudio | 需正常运行PulseAudio |

### 三 编译工具
- **GCC**：7.0及以上版本
- **Clang**：6.0及以上版本
- **C语言标准**：C99及以上

### 四 构建工具
- **CMake**：3.10及以上版本
- **Make**：GNU Make 4.0及以上版本
- **pkg-config**：0.29及以上版本

## 第三章 依赖库与安装命令
### 一 必备依赖
| 依赖库 | 版本要求 | 功能 |
| --- | --- | --- |
| `ffmpeg-free-devel` | 4.0+ | 音声解绎（libavcodec, libavformat, libswresample, libavutil, libavfilter） |
| `libpng` | 1.6+ | 专辑封面显示（PNG格式支持） |
| `libjpeg` | 6b+ | 专辑封面显示（JPEG格式支持） |
| `pulseaudio-libs-devel` | 10.0+ | PulseAudio音频输出 |
| `ncurses-devel` | 6.0+ | 终端界面处理，支持宽字符 |
| `libcurl-devel` | 7.0+ | 远程音乐播放（SMB/SFTP/FTP/WebDAV） |
| `libxml2-devel` | 2.9+ | XML配置文件解析 |
| `sqlite-devel` | 3.20+ | 音乐库数据库（FTS5全文搜索） |
| `cmake` | 3.10+ | 项目构建（编译时必需） |
| `gcc` | 7.0+ | C语言编译器（编译时必需） |
| `make` | - | 构建工具（编译时必需） |
| `pkg-config` | - | 依赖检查（编译时必需） |

**可选依赖：**

| 依赖库 | 用途 |
| --------- | ---- |
| `pipewire-0.3-devel` | PipeWire音频后端（dlopen加载，编译时可选，运行时自动检测） |
| `alsa-lib-devel` | ALSA音频输出后端 |
| `dbus-devel` | MPRIS D-Bus 媒体会话、专辑封面与歌词 API 集成 |

### 二 Fedora / RHEL / CentOS 安装命令
```bash
sudo dnf install cmake gcc make pkg-config
sudo dnf install ffmpeg-free-devel libpng-devel libjpeg-turbo-devel pulseaudio-libs-devel ncurses-devel libcurl-devel libxml2-devel sqlite-devel
# 可选后端
sudo dnf install pipewire-devel alsa-lib-devel dbus-devel
```

### 三 Ubuntu / Debian / Linux Mint 安装命令
```bash
sudo apt update
sudo apt install cmake gcc make pkg-config
sudo apt install libavcodec-dev libavformat-dev libswresample-dev libswscale-dev libavutil-dev libavfilter-dev libpng-dev libjpeg-dev libpulse-dev libncursesw5-dev libcurl4-openssl-dev libxml2-dev libsqlite3-dev
# 可选后端
sudo apt install libpipewire-0.3-dev libasound2-dev libdbus-1-dev
```

**注意**：如果无法获取FFmpeg开发库，需先启用universe仓库：
```bash
sudo add-apt-repository universe
sudo apt update
```

### 四 Arch Linux 安装命令
**从 AUR 安装（推荐）：**
```bash
# 使用 yay（AUR 助手）
yay -S ter-music-cn

# 使用 paru（AUR 助手）
paru -S ter-music-cn
```

**使用 ZPM（MengXi OS 包管理器）安装：**
```bash
# 首先安装 ZPM（如果尚未安装）
git clone https://aur.archlinux.org/zetapm.git
cd zetapm
makepkg -si

# 然后使用 ZPM 安装 ter-music-cn
zpm -S ter-music-cn
```

**手动从 AUR 安装：**
```bash
git clone https://aur.archlinux.org/ter-music-cn.git
cd ter-music-cn
makepkg -si
```

**手动从源码构建：**
```bash
sudo pacman -S cmake gcc make pkg-config
sudo pacman -S ffmpeg libpng libjpeg pulseaudio ncurses libcurl libxml2 sqlite
# 可选后端
sudo pacman -S pipewire alsa-lib dbus
```

## 第四章 编译步骤
### 一 克隆源码
```bash
git clone https://github.com/HuanSoft-Open-Source-Community/ter-music.git
cd ter-music
```

### 二 创建构建目录
```bash
mkdir build
cd build
```

### 三 配置CMake
```bash
cmake ..
```

CMake会自动检查系统中的所有依赖库，若有缺失会明确提示错误。

**可选的CMake配置项**：
```bash
# 自定义安装前缀（默认：/usr/local）
cmake .. -DCMAKE_INSTALL_PREFIX=/usr

# 启用调试编译
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 启用编译优化
cmake .. -DCMAKE_BUILD_TYPE=Release
```

### 四 编译
```bash
make -j$(nproc)
```

`-j$(nproc)` 会调用CPU所有核心并行编译，提升编译速度。

### 五 安装（可选）
```bash
sudo make install
```

安装完成后，可在终端直接输入`ter-music`启动播放器。

### 六 卸载（若已安装）
```bash
cd build
sudo make uninstall
```

### 七 清理构建文件
```bash
cd build
make clean
# 或直接删除构建目录
rm -rf build
```

### 八 编译常见问题
**问题一：找不到PulseAudio库**
```
解决方法：安装pulseaudio-libs-devel（Fedora）或libpulse-dev（Ubuntu）
```

**问题二：找不到ncursesw库**
```
解决方法：安装ncurses-devel（Fedora）或libncursesw5-dev（Ubuntu）
```

**问题三：找不到FFmpeg头文件**
```
解决方法：安装ffmpeg-devel（Fedora）或libavcodec-dev、libavformat-dev等（Ubuntu）
```

### 九 构建脚本使用

本项目提供多种构建脚本，用于生成不同格式的安装包。详细使用方法请参考：

- [构建指南](../BUILD_GUIDE.md) - 构建与打包的详细说明

支持的打包格式：
- **AppImage** - 通用Linux包格式
- **便携包** - 包含所有依赖的自解压压缩包
- **RPM包** - 适用于Fedora/RHEL系发行版
- **DEB包** - 适用于Debian/Ubuntu系发行版
- **玲珑包** - 适用于deepin/UOS系统
- **Arch Linux包** - 适用于Arch Linux及其衍生发行版

> **非 x86 架构（arm64、loong64 等）：** 非 x86 架构的软件包由 OBS 构建服务器统一构建和维护。详见 [OBS 构建服务器](https://obs22.odata.cc/package/show/home:Admin:app/ter-music)。

**测试服务器工具：**
- **tools/start-server.py** - 交互式脚本，快速启动本地SMB/FTP/SFTP/WebDAV/HTTP服务器，用于测试远程音乐播放功能。
  > 该脚本为 Python 脚本，建议在 Conda 环境中运行。配置：`conda create -n ter-music python=3 && conda activate ter-music && pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r tools/requirements.txt` 然后执行 `python3 tools/start-server.py`
  > 同时也支持CLI模式：`python3 tools/start-server.py --protocol http --port 8080 --path /music/share` 或 `python3 tools/start-server.py --protocol sftp --port 2222 --username test --sftp-authorized-keys ~/.ssh/authorized_keys`

## 第五章 使用方法
### 一 启动播放器
**若已安装**：
```bash
ter-music
```

**若未安装，直接从构建目录运行**：
```bash
cd build
./ter-music
```

### 二 命令行参数
不带任何子命令直接运行 `ter-music` 仍会启动 TUI（行为不变）：

```bash
ter-music [OPTIONS]

选项：
  -o, --open <path>    启动时直接打开指定的音乐目录
  -d, --debug          启用调试日志（输出到 ter-music-debug.log）
  --frontend <模式>    播放通路：remote（默认，经 D-Bus 交给核心）
                       或 local（进程内播放，保留作回归基线）
  --attach-only        不自动拉起核心：没有核心在跑时以退出码 3 结束
  --bus <名称>         指定目标核心/实例的总线名（默认主实例）
  -h, --help           显示帮助信息
  -v, --version        显示版本信息
  tui [path]           显式启动 TUI
```

**示例**：
```bash
# 启动时打开我的音乐文件夹
ter-music -o ~/Music

# 打开远程FTP音乐目录
ter-music ftp://user:pass@host/path/to/music

# 打开远程WebDAV目录
ter-music --open http://webdav-server/music

# 显示帮助信息
ter-music --help
```

#### 5.2.1 CLI 模式

以下任意一个首参数都会切换到 CLI 模式。CLI 命令是轻量的 D-Bus 客户端：
它们与当前持有 `org.mpris.MediaPlayer2.ter_music` 的**播放核心**（由
`daemon start`、D-Bus 激活或 TUI 拉起）通信，因此在任意终端、脚本或窗口
管理器快捷键中都能使用。其中 `play` 还兼任前端：它在自己的进程里扫描路径，
再把生成的队列下发给核心。

| 命令 | 说明 |
| --- | --- |
| `play [PATH] [--index N] [--mode MODE] [--no-daemon]` | 在本进程扫描路径，把队列下发给核心并播放；无核心时先拉起核心 |
| `pause` / `resume` / `toggle` / `stop` / `next` / `prev` | 基础传输控制 |
| `seek <+SECONDS\|-SECONDS\|mm:ss\|N%>` | 相对、绝对或按百分比的跳转 |
| `volume [0-100\|+N\|-N]` | 查询或设置音量 |
| `speed [0.5-3.0]` | 查询或设置播放倍速 |
| `mode [NAME\|0-16]` | 查询或设置播放模式（支持 `list_repeat`、`folder_shuffle_repeat` 等稳定名称） |
| `show [OPTIONS]` | 打印当前信息块（基本信息 / 字符封面 / 进度 / 两行歌词） |
| `daemon start\|foreground\|stop\|restart\|status\|reload` | 播放核心管理 |
| `version` / `help` | 版本 / 用法 |

`show` 选项（每项都会在该次调用中覆盖已保存的 TUI 设置）：

| 选项 | 说明 |
| --- | --- |
| `--json` | 打印完整的 JSON 快照（`Info.GetInfo`） |
| `--watch[=MS]` | 实时刷新视图（默认 500 毫秒，按 `Ctrl+C` 退出，需要 TTY） |
| `--one-line` | 单行输出，适合状态栏使用 |
| `--full` / `--compact` / `--preset full\|compact\|custom` | 信息显示预设 |
| `--fields a,b,c` | 基本信息字段：`state,mode,index,queue,title,artist,album,format,path,volume,speed` |
| `--cover` / `--no-cover`、`--cover-size WxH`、`--charset braille\|ascii` | 字符封面选项（4-40 列、2-20 行） |
| `--progress bar\|time\|percent\|time+percent`、`--no-progress` | 进度行样式 |
| `--lyrics 0\|1\|2` | 歌词行：关闭 / 当前行 / 当前行 + 下一行 |
| `--width N` | 输出宽度（默认使用终端宽度） |
| `--bus NAME` | 指定目标实例的总线名称（默认为主实例） |

**退出码**：`0` 成功；`1` 用法错误；`3` 没有正在运行的实例；
`4` D-Bus 不可用；`5` 被实例拒绝。

**示例**：

```bash
# 启动后台播放并立即返回 shell
ter-music play ~/Music

# 播放单个文件（其所在目录会作为歌单载入）
ter-music play ~/Music/album/01.flac

# 当前曲目信息：盲文封面、进度条以及当前/下一行歌词
ter-music show

# 供状态栏使用的单行信息，每秒刷新一次
ter-music show --one-line --watch=1000

# 供脚本使用的原始 JSON
ter-music show --json | jq -r '.track.title'

# 传输控制
ter-music next
ter-music seek +10
ter-music volume +5
ter-music mode shuffle_repeat

# 后台守护进程管理
ter-music daemon start --open ~/Music
ter-music daemon status
ter-music daemon reload      # 重新读取 config.xml（信息显示设置、音量等）
ter-music daemon stop
```

注意事项：

- 不带子命令时，`ter-music <path>` 仍会打开 TUI。若要打开名称恰好为
  `play`/`show`/…… 的目录，请使用 `-o ./play` 或 `ter-music tui play`。
- `daemon start --open <目录>` 不再让核心去扫描：现在先起核心，再由这个 CLI
  进程（前端）扫描目录、生成队列并下发——与 `ter-music play <目录>` 同一条路。
  不带 `--open` 的 `daemon start` 只起一个空闲核心，队列由随后接入的前端下发。
- 总线名称同时只允许一个实例持有；核心在运行时 `daemon start` 会拒绝再起一个
  （可用 `--force` 强制以次级实例启动）。前端（TUI 与 CLI）都是客户端，可以
  同时存在多个。
- 除非显式指定 `--force`，`daemon stop` 会拒绝终止正在运行的 TUI。

#### 5.2.2 Linyaps（如意玲珑）打包环境

当 ter-music 以 Linyaps（如意玲珑）包安装时，二进制位于应用容器内部，不存在
宿主级的 `ter-music` 命令（Linyaps 无法把可执行文件导出到 `$PATH`）。请通过
`ll-cli` 运行 CLI 命令；每次调用都会加入同一个应用容器，因此 CLI、TUI 与播放
守护进程共享同一会话总线、同一配置目录与同一组 D-Bus 接口。

```bash
# 任意 CLI 命令
ll-cli run org.yxzl.ter-music -- ter-music show
ll-cli run org.yxzl.ter-music -- ter-music pause
ll-cli run org.yxzl.ter-music -- ter-music play ~/Music

# 容器已在运行时读取状态（见下方说明）
ll-cli enter org.yxzl.ter-music -- /opt/apps/org.yxzl.ter-music/files/bin/ter-music show

# 交互式 shell 的便捷包装（可写入 ~/.bashrc）
ter-music() { ll-cli run org.yxzl.ter-music -- ter-music "$@"; }
```

**后台播放。** 自我脱离的进程（`ter-music daemon start`）会随容器一起被回收，
因此在沙箱内被禁用：`daemon start` 与 `play` 改为请求会话总线激活后台播放，
当激活不可用时回退为带完整命令的提示信息。共有三种可用方式：

| 方式 | 用法 | 行为 |
| --- | --- | --- |
| D-Bus 按需激活 | `ter-music play <路径>` 或 `ter-music daemon start` | 会话总线在宿主上启动 `ll-cli run org.yxzl.ter-music -- ter-music daemon foreground --no-autoplay`，命令随后转发给它 |
| systemd 用户服务（常驻） | 在宿主执行 `systemctl --user enable --now org.yxzl.ter-music` | 播放器随会话启动，并在后台持续播放 |
| 前台运行 | `ll-cli run org.yxzl.ter-music -- ter-music play <路径> --foreground` | 在前台播放；容器与命令同生命周期（适合 tmux/screen） |

注意事项：

- 容器内不可访问 `systemctl --user`，因此必须在宿主 shell 中启用该服务；
  `ter-music help` 在检测到沙箱时会打印同样的提示。
- `ter-music daemon stop` 照常工作并停止实例；之后容器被回收，因此 `ll-cli ps`
  不再列出该应用。
- `ll-cli run` 会把所有失败退出码映射为 `255`；宿主脚本应改为解析
  `ter-music show --json` 的 `"running"` 字段，而不要依赖退出码。
- 当应用容器**已在运行**（后台 daemon 或 TUI）时，`ll-cli run … -- <命令>` 会把该命令
  的输出接到*正在运行的容器*的标准输出上，终端因此看不到任何内容。可用
  `journalctl --user -u org.yxzl.ter-music` 查看，或改用 `ll-cli enter`（终端保持连接）：
  `ll-cli enter org.yxzl.ter-music -- /opt/apps/org.yxzl.ter-music/files/bin/ter-music show`。
  该进入环境不传 `DBUS_SESSION_BUS_ADDRESS`，CLI 会自行解析会话总线
  （依次尝试 `$XDG_RUNTIME_DIR/bus`、`/run/user/<uid>/bus`）；显式设置的地址始终优先。
- 宿主路径在容器内原样可见（`$HOME`、`/tmp`、`/media`），提取出的封面通过
  `mpris:artUrl` 仍可被宿主应用读取。
- 设置、曲库与播放会话存储于 `$XDG_CONFIG_HOME/ter-music`。若 Linyaps 运行时
  重定向了 XDG 变量（参见 Linyaps FAQ「应用数据保存到哪里」），它们会落在
  `~/.linglong/org.yxzl.ter-music/…`，从而与 deb 安装互不干扰。
- `--watch` 需要终端（它使用 ANSI 光标控制）——请直接在终端中运行，不要经管道
  运行。
- `Info`/`Control` 就是普通的会话总线服务，因此宿主应用（`gdbus`、媒体组件、
  `busctl`）可以像普通安装那样读取与控制 Linyaps 实例。

#### 5.2.3 前端与核心

Ter-Music 分为两个角色，二者经会话总线通信：

| 角色 | 由谁运行 | 掌管什么 |
| --- | --- | --- |
| **核心**（播放服务） | `ter-music daemon start` / `daemon foreground` | 音频设备、播放状态与进度、传输命令、音量/倍速/播放模式、**执行前端下发的路径队列**、当前曲目信息（歌词、字符封面、频谱）、配置、前端注册与心跳 |
| **前端**（文件系统与内容） | TUI（`ter-music`、`ter-music tui`）、CLI（`ter-music play/show/…`）以及其它客户端 | 曲库（SQLite）、扫描与元数据、播放列表内容、用户歌单、收藏/历史/目录历史、排序/过滤/搜索、远程源与其下载缓存、界面 |

核心从不扫描目录、不持有曲库、不解析远程 URL：它只播放**本地路径**，顺序由前端
给定。前端从不打开音频设备：它只渲染从 D-Bus 读回的状态，并把内容交给核心。

**启动。** 在没有核心时直接运行 TUI，会自动拉起一个核心并把内容（`-o` 指定的
目录，或恢复出的上次会话）推给它，因此日常用法没有变化：

```bash
ter-music                 # 启动 TUI（无核心时会自动拉起核心）
ter-music -o ~/Music      # 同上，并先打开一个目录
```

需要核心已经存在时（脚本里，或不愿被悄悄启动播放时），用 `--attach-only`：

```bash
ter-music --attach-only   # 没有核心在跑则以退出码 3 结束
ter-music --bus org.yxzl.ter_music.instance1   # 指定要接入的实例
```

**退出前端。** 关闭 TUI **不会**停止播放：核心继续运行并继续播放，
`ter-music show` / `ter-music next` 在任何终端里依然指挥得动它。若希望最后一个
前端离开后核心自行退出，把 `core_exit_when_no_frontend` 设为 `true`
（`config.xml`，或经 D-Bus `Config.Set`）：前端全部离开并过 10 秒宽限期后核心
退出；从未有前端接入过的核心不会退出。

**断线重连。** 核心消失时（崩溃、被 `kill`、会话注销），前端不会退出：它进入
断线状态，按指数退避重试（1/2/4/8/15/30 秒），一旦有核心应答就重新接入，并把
自己的内容队列补推回去。在 TUI 里按 `R` 可以立即重启一个已死的核心并重推队列。

**兼容性。** 总线接口面为 `api_version 4`。内容类接口（`Playlist`、`Library`、
`Favorites`、`History`、`DirHistory`、`Remote`）已撤下：它们属于前端。第三方
客户端请使用 `Lyrics`、`Info`、`Control`、`Queue`、`Config` 五个接口，详见
[API_DBUS_en_US.md](../API_DBUS_en_US.md)。

### 三 界面布局
启动后界面分为三栏，布局如下：
```
┌────────────────────────────┬───────────────┐
│  Play List                 │  [Spectrum]   │
│                            ├───────────────┤
│  歌曲列表区域                │  Lyrics       │
│                            │               │
│                            │ 歌词显示区域    │
│                            │  (或黑胶唱片)   │
│                            │               │
├────────────────────────────┤               │
│   Controls                 │               │
│   [==========>-----]       │               │
│  [<<] [Play/Pause] [>>]    │               │
│  [Stop] [Loop:Off] [Volume]│               │
└────────────────────────────┴───────────────┘
Menu: 选项菜单
```

- **左上方**：歌单区域，显示当前目录下的所有音频文件
- **左下方**：控制栏，包含播放控制按钮和音频进度条
- **右侧**：歌词显示区域，同步显示当前播放歌曲的歌词；开启专辑封面后也在此显示点阵封面图
- **底部**：选项菜单，包含设置、播放历史、收藏、关于、退出等选项

### 四 常用操作方法
#### 焦点切换
| 按键 | 功能 |
| --- | --- |
| `C` | 将焦点切换到控制区 |
| `L` | 将焦点切换到歌单区 |
| `Ctrl+L` | 进入/退出歌词定位模式（然后用↑/↓导航） |
| `Tab` / `Shift+Tab` | 切换文件浏览/播放队列视图 |

**注意**：按 `Ctrl+L` 进入歌词定位模式；然后用 `↑`/`↓` 导航歌词，`Enter`/`Space` 跳转到选中歌词的播放位置。再次按 `Ctrl+L` 退出。从任意焦点模式均可使用。

#### 歌单区操作（焦点在歌单区域）
| 按键 | 功能 |
| --- | --- |
| `↑` / `↓` 或 `j` / `k` | 向上/向下选择歌曲 |
| `Space` / `Enter` | 播放选中的歌曲 |
| `O` / `o` | 打开新的音乐文件夹 |
| `F` / `f` | 将选中的歌曲加入收藏列表 |
| `a` | 将选中的歌曲追加到播放队列 |
| `A` | 将选中的歌曲加入自定义歌单（弹出选择列表） |
| `i` | 将选中歌曲插入为队列下一首 |
| `I` | 将文件夹音乐追加到当前歌单 |
| `d` | 从播放队列中移除选中曲目 |
| `D` | 清空整个播放队列 |
| `J` | 将选中曲目在队列中下移（重排） |
| `K` | 将选中曲目在队列中上移（重排） |
| `S` 或 `/` | 启用拼音搜索功能，检索歌曲名、歌手 |
| `M` | 切换音乐库浏览器（按艺术家/专辑/流派浏览） |
| `Tab` / `Shift+Tab` | 切换文件浏览/播放队列视图（两键功能相同） |
| `n` | 下一曲 |
| `p` | 上一曲 |
| `h` | 显示播放历史弹出窗口（最近10首） |
| `1`-`5` | 快速设置播放模式：1=顺序 2=单曲循环 3=列表循环 4=随机重复 5=文件夹顺序 |

> **注：** 音乐库浏览器内的键盘导航（方向键/回车进入子项）当前主要通过鼠标交互支持；键盘控制功能有限。按`M`切换，按`Esc`退出。

#### 控制区操作（焦点在控制栏）
| 按键 | 功能 |
| --- | --- |
| `←` / `→` | 向左/向右选择控制按钮 |
| `Space` | 触发选中的控制按钮 |
| `,` | 播放进度后退5秒 |
| `.` | 播放进度前进5秒 |
| `Ctrl+L` | 进入/退出歌词定位模式 |
| `-` / `_` | 降低音量 |
| `=` / `+` | 提高音量 |

**控制按钮说明**：
| 按钮名 | 功能 |
| --- | --- |
| `<<` | 上一曲 |
| `Play/Pause` | 播放/暂停 |
| `>>` | 下一曲 |
| `Stop` | 停止播放 |
| `Mode` | 切换播放模式（打开弹出菜单，按Enter从17种模式中选择） |
| `Speed` | 切换倍速播放（打开弹出菜单选择：0.75x → 1.0x → 1.25x → 1.5x → 2.0x → 3.0x） |
| `Progress` | 进度条（显示当前播放进度） |
| `Volume` | 音量调节（打开弹出滑动条，显示当前音量百分比） |

#### 歌词定位操作
| 按键 | 功能 |
| --- | --- |
| `↑` / `↓` | 向上/向下选择歌词行（定位模式中） |
| `Ctrl+L` 或 `Enter`/`Space` | 退出歌词定位 / 跳转到选中位置 |

#### 功能键（全局可用）
**功能键（F1-F9）**
| 按键 | 功能 |
| --- | --- |
| `F1` | 返回主界面 |
| `F2` | 打开设置视图 |
| `F3` | 打开播放历史视图 |
| `F4` | 打开歌单管理视图 |
| `F5` | 打开收藏视图 |
| `F6` | 打开关于视图 |
| `F7` | 语言选择（打开语言选择视图） |
| `F8` | 帮助（本页面） |
| `F9` | 退出播放器 |

**备用数字键（按Esc后3秒内输入）**
| 按键组合 | 功能 |
| --- | --- |
| `Esc` + `1` | 返回主界面 |
| `Esc` + `2` | 打开设置视图 |
| `Esc` + `3` | 打开播放历史视图 |
| `Esc` + `4` | 打开歌单管理视图 |
| `Esc` + `5` | 打开收藏视图 |
| `Esc` + `6` | 打开关于视图 |
| `Esc` + `7` | 语言选择（打开语言选择视图） |
| `Esc` + `8` | 帮助（本页面） |
| `Esc` + `9` | 退出播放器 |
| `q` | 退出播放器 |

### 五 播放模式说明

Ter-Music 拥有17种播放模式，分为5组，基础模式始终可用：
按Enter键打开控制栏的Mode弹出菜单进行选择。

#### 基础模式（始终可用）

| 模式名 | 说明 |
| -------- | ---- |
| `Sequential` | 顺序播放，播放到列表末尾停止 |
| `Single Repeat` | 单曲循环，重复播放当前歌曲 |
| `List Repeat` | 列表循环，播放完一轮后从头开始 |
| `Shuffle Once` | 随机一次，不重复随机播放列表中所有歌曲 |
| `Shuffle Repeat` | 随机重复，随机选择下一首歌曲 |

#### 高级模式（需要数据库库元数据）

| 分组 | 模式 | 说明 |
| ---- | ---- | ---- |
| `Folder` | 顺序 / 循环 / 随机 / 随机重复 | 限定在当前目录范围 |
| `Album` | 顺序 / 循环 / 随机 / 随机重复 | 按专辑标签范围 |
| `Artist` | 顺序 / 循环 / 随机 / 随机重复 | 按艺术家标签范围 |

**注意：** 高级模式使用SQLite音乐库数据库进行元数据查询。在设置 → 播放模式 → "启用高级播放模式"中开启。

### 六 倍速播放功能

Ter-Music支持倍速播放功能，可根据需要调整音频播放速度：

| 速度档位 | 说明 |
| -------- | ---- |
| `0.75x` | 慢速播放，适合仔细聆听或学习 |
| `1.0x` | 正常速度，默认播放速度 |
| `1.25x` | 稍快速度，适合加快收听 |
| `1.5x` | 快速播放，适合快速浏览内容 |
| `2.0x` | 双倍速，适合高效收听 |
| `3.0x` | 三倍速，最大速度，适合快速回顾 |

**使用方法：**
- 在控制区，使用`←`/`→`键选中倍速按钮，按`Space`键即可切换速度
- 当前速度会显示在倍速按钮上（如"倍速:1.50x"）
- 播放过程中可随时切换速度，音频会无缝过渡到新速度
- 默认倍速可在设置菜单（F2）中进行配置

**技术说明：** 倍速调节采用FFmpeg的atempo滤镜实现，可在改变播放速度的同时保持音调不变。

### 七 歌词显示
本播放器支持自动加载LRC格式歌词，同时支持内嵌歌词：
- **内嵌歌词优先**：播放器先从音频文件读取内嵌歌词（FFmpeg/APE标签），若无则回退加载外部.lrc文件
- 歌词文件需与音频文件放在同一目录
- 歌词文件名需与音频文件名相同，后缀为`.lrc`
- 示例：`song.mp3` 对应 `song.lrc`
- **切换歌词来源**：按`Ctrl+L`进入歌词定位模式，再按`Tab`在内嵌/外置歌词间切换
- 播放器会随播放进度自动高亮显示当前歌词行
- 若未找到歌词文件，歌词区会显示"No lyrics loaded"

其他程序可通过 D-Bus 歌词接口读取当前 A/B 两行歌词，详细说明见
[Lyrics API (English)](../API_LYRICS_en_US.md)。

### 八 MPRIS 与歌词 API

编译时启用 D-Bus（`libdbus-1`）后，播放器会在会话总线上注册 MPRIS 媒体会话：

- 主流桌面环境（GNOME Shell、KDE Plasma、Cinnamon、Budgie 等）可显示播放控制和曲目信息。
- 只要存在专辑封面，就会发布 `mpris:artUrl`，桌面媒体组件可显示封面图。
- 封面优先读取音频内嵌图片；若没有内嵌图片，则按 `cover`、`folder`、`front`、`album`（不区分大小写，扩展名支持 `.jpg`、`.jpeg`、`.png`、`.webp`）查找同目录封面。
- 提取后的封面统一保存为受管理的 `/tmp/ter-music-cover-*.jpg` 缓存文件，保留最近 10 首的 MRU 缓存，退出时自动清理。

同一 D-Bus 对象还提供开放歌词接口：接口 `org.yxzl.ter_music.Lyrics`，方法
`GetLyrics`、`GetDocument` 与 `SetSource`（在内嵌歌词与外部歌词之间切换），
信号 `LyricsChanged`。JSON 结构与调用示例见
[Lyrics API (English)](../API_LYRICS_en_US.md)。

同一对象路径上还额外发布了下列接口，便于其他应用程序读取曲目数据、字符封面与
播放进度，并驱动播放器：

- `org.yxzl.ter_music.Info`（只读）：`GetInfo`、`GetTrackInfo`、
  `GetProgress`、`GetLyricsLines`、`GetCoverArt(charset, cols, rows)`、
  `GetDisplay(options)`（即 `ter-music show` 打印的原文）、
  `InstanceInfo`，以及信号 `InfoChanged`、`ProgressChanged`（最高 1 Hz）
  和 `CoverChanged`。
- `org.yxzl.ter_music.Control`：传输控制、跳转、音量、倍速、播放模式、
  `ReloadConfig` 与 `Quit`。加载内容**不在**其中——内容由前端以队列形式下发。
- `org.yxzl.ter_music.Queue`：核心的**路径队列**——
  `Set`/`Append`/`InsertAfter`/`RemoveAt`/`MoveUp`/`MoveDown`/`Clear`/
  `Shuffle`/`PlayAt` 与分页读取 `Get(offset, count)`，并广播 `QueueChanged`
  （条目数、当前游标、版本号）。只接受本地路径；单次写入 ≤500 条、单次读取
  ≤1000 条。
- `org.yxzl.ter_music.Config`：核心配置的唯一写入口。远程服务器条目**不**在其中：
  它属于前端（见下文）。
- `Info.GetInfo` 通过 `core.api_version`（当前为 `4`）与已实现方法清单做版本握手，
  客户端可先校验兼容性再调用。版本 3 移除了 `Remote` 接口与 `track.is_remote`
  字段，并规定所有路径参数只接受本地路径；版本 4 用路径语义的 `Queue` 取代了
  内容接口（`Playlist`、`Library`、`Favorites`、`History`、`DirHistory`）——
  内容归前端。
- 已实现 `org.freedesktop.DBus.Introspectable` 与 `org.freedesktop.DBus.Peer`，
  因此 `busctl --user introspect` / `gdbus introspect` 可直接使用。
- MPRIS 元数据额外携带 `xesam:url`（恒为 `file://` URI，远程来源的曲目也是
  本地缓存路径）与 `xesam:trackNumber`；`OpenUri` 只接受本地文件。
- `CanQuit` 刻意保持为 `false`，以免桌面媒体组件直接结束播放器进程；
  如需退出，请使用 `ter-music daemon stop` 或 `Control.Quit`。

完整的方法列表与 JSON 结构见
[D-Bus Info & Control API (English)](../API_DBUS_en_US.md)。

当 ter-music 以 Linyaps 包运行时同样发布这些接口：容器使用宿主会话总线，因此
宿主应用与包内 CLI 看到的是同一对象路径与接口。

### 九 配置文件

配置文件存储在`~/.config/ter-music/config.xml`，播放器首次启动时会自动创建（如存在v1的config.json会自动迁移）。

**配置项**：
- `default_startup_path`：默认启动目录
- `auto_play_on_start`：启动时自动播放（0/1，0为关闭，1为开启）
- `remember_last_path`：记住上次访问的目录（0/1）
- `show_album_cover`：显示专辑封面（0/1）
- `show_lyrics_panel`：显示歌词面板（0/1）
- `default_playback_speed`：默认播放速度（0.75、1.0、1.25、1.5、2.0、3.0）
- `default_play_mode`：默认播放模式（0=顺序、1=单曲循环、2=列表循环、3=随机一次、4=随机重复……）
- `advanced_play_modes_enabled`：启用高级文件夹/专辑/艺术家播放模式（0/1）
- `lyrics_alignment`：歌词对齐方式（0=居左、1=居中、2=居右）
- `clear_history_on_startup`：启动时清空播放历史（0/1）
- `resume_last_playback`：从上次位置继续播放（0/1）
- `seamless_preload`：在当前曲目末尾预解码下一曲，实现无缝播放（0/1）
- `ui_language`：界面语言（字符串ID："zh_CN"、"en_US"等，通过F7语言视图设置）
- `volume_percent`：默认音量百分比（0-100）
- `audio_latency_ms`：输出时延（毫秒）
- `audio_backend`：音频后端（0=自动、1=PulseAudio、2=ALSA、3=PipeWire）
- `sort_mode`：排序模式（0=默认、1=标题、2=艺术家、3=专辑、4=文件名）
- `cue_encoding`：CUE文件字符编码（0=自动、1=UTF-8、2=GB18030、3=GBK、4=BIG5、5=Shift-JIS）
- `core_exit_when_no_frontend`：无人接入时让核心自行退出（0/1，默认 0）。
  默认关闭表示「关掉 TUI，音乐继续放」；置 1 时最后一个前端离开并过 10 秒
  宽限期后核心退出
- 远程服务器连接（SMB/SFTP/FTP/WebDAV/HTTP）**不**存在 `config.xml` 里：
  前端把它保存在同目录下自有的 `remote.xml`（服务器条目 + 密码密文，权限 0600），
  下载的曲目缓存在 `$XDG_CACHE_HOME/ter-music/remote/`
- 颜色主题设置：24套预设主题 + 1个自定义槽位，所有界面元素的前景色、背景色
- 均衡器设置：10段增益、前置放大、启用/禁用
- 信息显示（CLI / D-Bus）设置，可在**设置 → 信息显示**中编辑：
  - `info_preset`：预设（0=完整、1=紧凑、2=自定义）
  - `info_fields`：基本信息字段位掩码（1=状态、2=模式、4=序号、8=队列、16=标题、32=艺术家、64=专辑、128=格式、256=路径、512=音量、1024=倍速；2047=全部）
  - `info_show_cover`：打印盲文/ASCII 字符封面（0/1）
  - `info_cover_cols` / `info_cover_rows`：封面尺寸（字符列数/行数，4-40 / 2-20）
  - `info_cover_charset`：封面字符集（0=盲文、1=ASCII）
  - `info_show_progress`：打印进度行（0/1）
  - `info_progress_style`：进度样式（0=进度条+时间、1=时间、2=百分比、3=时间+百分比）
  - `info_lyrics_lines`：歌词行数（0=关闭、1=当前行、2=当前行+下一行）

播放器会自动保存配置，修改后立即生效。

### 5.9.1 语言包系统

Ter-Music 使用基于 XML 的国际化（i18n）系统。内置语言包位于源码树的 `data/lang/` 目录，安装后位于 `TER_MUSIC_DATA_DIR/lang/`。

**语言包格式：**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<lang id="en_US" name="English (US)">
  <string key="general.yes">On</string>
  <string key="general.no">Off</string>
  <!-- ... 更多字符串条目 ... -->
</lang>
```

- 根元素为 `<lang>`，属性 `id` 为语言标识（如 "zh_CN"），`name` 为显示名称。
- 每个可翻译字符串为一个 `<string>` 元素，`key` 属性为键名，元素内容为译文。
- 键名采用点号分层约定：`模块.子模块.名称`（如 `sidebar.settings.theme`、`menu.help`）。

**查找优先级（从高到低）：**

1. `~/.config/ter-music/lang/<id>.xml` — 用户自定义覆盖
2. `TER_MUSIC_DATA_DIR/lang/<id>.xml` — 编译期安装前缀
3. `/usr/share/ter-music/lang/<id>.xml` — 系统全局安装
4. `<exe_path>/../share/ter-music/lang/<id>.xml` — 相对于可执行文件
5. `data/lang/<id>.xml` — 开发/运行目录
6. 源码树 `data/lang/<id>.xml`

要添加新语言，按上述格式创建 `<id>.xml` 文件，放入任一搜索路径（推荐 `~/.config/ter-music/lang/`）。该语言将自动出现在 F7 语言选择视图中。

#### 5.9.2 tar.gz 语言包分发规范

语言包也可通过 `.tar.gz`（或 `.tgz`）压缩包形式分发，方便共享和一键安装。在语言选择视图中按 `A` 键安装，按 `D` 键删除已添加的用户语言包。

**压缩包内容：**

| 文件 | 必需 | 说明 |
|------|------|------|
| `lang.xml` | 是 | 语言数据文件（格式见 §5.9.1） |
| `help.txt` | 否 | 该语言的快速入门帮助文本 |

只有文件名称为 `lang.xml` 和 `help.txt` 的文件会被提取，其余文件将被自动忽略。文件仅按基本名称匹配，可位于 tar 包内的任意子目录中。

**规格要求：**

| 属性 | 值 |
|------|-----|
| 文件扩展名 | `.tar.gz` 或 `.tgz` |
| 归档格式 | POSIX/USTAR（标准 `tar` 格式） |
| 压缩方式 | gzip |
| 单文件大小上限 | 50 MB |
| 字符编码 | UTF-8 |
| 压缩级别 | 任意（gzip 兼容即可） |

**创建语言包：**

```bash
# 最小化——仅语言数据
tar -czf mylanguage.tar.gz lang.xml

# 附带帮助文本
tar -czf mylanguage.tar.gz lang.xml help.txt

# 文件可在子目录中，仅基本名称起作用
tar -czf mylanguage.tar.gz some/dir/lang.xml some/dir/help.txt
```

**安装路径：**

通过语言视图（`A` 键）安装后，提取的文件被放置到：
- `~/.config/ter-music/lang/<id>.xml` — 语言数据
- `~/.config/ter-music/help/help-quickstart-<id>.txt` — 帮助文本（若包含 `help.txt`）

语言 `<id>` 从 `lang.xml` 中 `<lang>` 根元素的 `id` 属性读取。

**校验流程：**

程序在导入时会执行以下验证：
1. 拒绝非普通文件和非 `.tar.gz`/`.tgz` 扩展名
2. 提取 `lang.xml` 并解析为 XML
3. 验证根元素为 `<lang>` 且 `id` 属性非空
4. 拒绝覆盖内置语言（`zh_CN`、`en_US`）的压缩包
5. 执行 50 MB 单文件大小上限检查

验证通过后，该语言将立即出现在语言选择视图中。用户安装的语言在界面上与内置语言有明确区分。

**注意：** 如果 `~/.config/ter-music/lang/` 中已存在相同 `<id>` 的语言包，安装新的 tar.gz 会静默覆盖。如需恢复被误删的内置语言，请重新安装程序。

### 十 数据存储位置

所有用户数据均存储在`~/.config/ter-music/`目录下：
```
~/.config/ter-music/
├── config.xml       # 配置文件（XML格式，libxml2解析，启动时迁移到当前版本）
├── library.db       # SQLite数据库（音乐库、收藏、歌单、历史）
├── remote.xml       # 远程服务器列表（前端自有，核心不读取）
├── lang/            # 用户语言包目录（覆盖内置翻译）
└── config.json.bak  # v1配置文件首次迁移时的自动备份（如有）
```

**注意：** 播放队列不再落盘为文件：队列属于内容，由前端在下次启动时从曲库 /
上次打开的目录重建，游标则在开启 `resume_last_playback` 时由核心恢复。
v1.0的JSON存储（config.json、独立的favorites、history、dir_history、playlists/目录）
已全部替换为SQLite数据库library.db。首次启动v2.0时会自动迁移。

专辑封面不再存于 `~/.config/ter-music/`。提取的封面是受管理的临时 JPEG
文件，位于 `/tmp/ter-music-cover-*.jpg`，保留最近 10 首，退出时删除。

### 十一 常用操作流程
**示例：初次使用方法**
1. 启动播放器：
   ```bash
   ter-music
   ```
2. 按`O`键打开文件夹，输入你的音乐目录路径，例如：
   ```
   /home/yourname/Music
   ```
3. 播放器会扫描目录中的所有音频文件，并显示在歌单区域
4. 用`↑` `↓`键选择想要播放的歌曲，按`Space`键开始播放
5. 若有对应的歌词文件，会自动加载并在右侧同步显示
6. 用`,`和`.`键可分别后退/前进5秒播放进度

**示例：将歌曲加入收藏**
1. 在歌单区选中想要收藏的歌曲
2. 按`F`键，底部状态栏会显示"Added to favorites!"
3. 按`F5`键可查看所有收藏的歌曲
4. 在收藏视图中，可选择歌曲播放

**示例：创建自定义歌单**
1. 按`F4`键进入歌单管理视图
2. 选择"Create New Playlist"
3. 输入歌单名称
4. 返回主界面，在歌单中选中歌曲，按`A`键加入自定义歌单（从弹出列表中选择）

**示例：浏览音乐库**
1. 按`M`键进入音乐库浏览器
2. 用`↑`/`↓`导航：首页 → 艺术家 → 专辑 → 曲目
3. 在艺术家上按Enter查看其专辑，在专辑上按Enter查看曲目
4. 在曲目上按Enter即可播放
5. 再次按`M`或按`Esc`返回文件夹浏览模式

**示例：管理播放队列**
1. 在文件浏览中选中歌曲，按`a`键追加到队列
2. 按`Tab`键切换至队列视图查看有序列表
3. 用`J`/`K`键重排曲目顺序，`d`键移除曲目，`D`清空全部
4. 在队列条目上按Enter即可播放
5. 再次按`Tab`返回文件浏览

### 十二 快捷键速览
| 分类 | 按键 | 功能 |
| --- | --- | --- |
| **全局** | `q` | 退出播放器 |
|  | `F1` | 返回主界面 |
|  | `F2` | 设置 |
|  | `F3` | 播放历史 |
|  | `F4` | 歌单管理 |
|  | `F5` | 收藏 |
|  | `F6` | 关于 |
|  | `F7` | 语言选择（打开语言选择视图） |
|  | `F8` | 帮助 |
|  | `F9` | 退出播放器 |
|  | `Esc` | 返回/后退 |
| **焦点** | `C` | 焦点切换到控制区 |
|  | `L` | 焦点切换到歌单区 |
|  | `Tab`/`Shift+Tab` | 切换文件/队列视图 |
| **歌单/浏览** | `↑`/`↓` 或 `j`/`k` | 选择上/下一曲 |
|  | `Space`/`Enter` | 播放选中的歌曲 |
|  | `O` / `o` | 打开文件夹 |
|  | `F` / `f` | 加入收藏列表 |
|  | `a` | 追加到队列 |
|  | `A` | 加入自定义歌单 |
|  | `i` | 插入为队列下一首 |
|  | `I` | 追加文件夹到歌单 |
|  | `d` | 从队列移除 |
|  | `D` | 清空整个队列 |
|  | `J` | 在队列中下移 |
|  | `K` | 在队列中上移 |
|  | `S` 或 `/` | 启用拼音搜索功能 |
|  | `M` | 切换音乐库浏览器 |
|  | `n` | 下一曲 |
|  | `p` | 上一曲 |
|  | `h` | 显示历史弹出窗口 |
|  | `1`-`5` | 快速设置播放模式 |
| **控制** | `←`/`→` | 选择控制按钮 |
|  | `Space` | 触发控制按钮/打开弹出菜单 |
|  | `,` | 后退5秒 |
|  | `.` | 前进5秒 |
|  | `Ctrl+L` | 进入/退出歌词定位模式 |
| **歌词** | `Ctrl+L` | 进入/退出歌词定位模式 |
|  | `↑`/`↓` | 选择上/下一句歌词（定位模式中） |
|  | `Enter`/`Space` | 跳转到选中的歌词行（定位模式中） |

### 十三 终端窗口大小调整
本播放器支持终端窗口大小调整，修改窗口尺寸时，播放器会自动重置布局并重新绘制界面。

### 十四 退出播放器
退出前端的方法有三种：
- 在主界面按`q`键
- 按`Ctrl+C`/`Ctrl+D`/`Ctrl+\`（播放器会优雅退出，支持SIGHUP/SIGTERM/SIGINT信号）
- 在选项菜单中选择"Exit"（即`F9`键）

退出 TUI 不会停止音乐：播放发生在核心里，当前曲目会继续播放，
`ter-music show` / `ter-music next` 在任意终端里依然有效。要显式停止请用
`ter-music daemon stop`；若希望核心自行退出，把 `core_exit_when_no_frontend`
设为 `true`（见 [5.2.3 前端与核心](#523-前端与核心)）。

## 第六章 技术架构

### 一 前端与核心

代码库围绕两个平面组织：二者共用同一份进程镜像，但职责从不重叠。同一次构建
既可以只跑核心（daemon），也可以只跑前端（TUI/CLI），它们只在 `player` 门面与
D-Bus 接口面相遇：

| 平面 | 进程 | 掌管什么 |
| --- | --- | --- |
| **核心 / 播放服务** | `ter-music daemon foreground`（由 `daemon start`、D-Bus 激活或 systemd 用户服务拉起） | 音频设备与解码、执行播放队列（**只认路径**）、传输控制、音量/倍速/播放模式、均衡器、当前曲目信息（歌词、字符封面、频谱）、发布 `Lyrics` / `Info` / `Control` / `Queue` / `Config` 接口、前端注册与心跳、配置 |
| **前端 / 内容客户端** | `ter-music`（TUI）、`ter-music play\|show\|…`（CLI） | 目录扫描与元数据、SQLite 曲库与 FTS5 搜索、用户歌单、收藏/历史/目录历史、排序/过滤、远程源与下载缓存、ncurses 界面 |

这条接缝刻意做得很薄，也便于测试：

- **`player/` 门面**：`player.c` 分发到 `player_local.c`（进程内播放，保留作迁移
  基线）或 `player_remote.c`（D-Bus 客户端，默认）。界面只调用门面，因此前端可以
  独立构建，同一套 TUI 也能驱动任一平面。
- **`queue/backend_queue.c`**：核心的路径队列——顺序、游标与版本号，外加条目
  元数据（路径、标题、艺术家、专辑、时长、CUE 偏移/轨号、歌词来源）。
  `audio/play_queue.c` 是进程内后端使用的轻量转发层，
  `playlist/playlist_queue.c` 是唯一的「内容 → 核心队列」桥。
- **D-Bus 接口面，`api_version 4`**：`Queue.Set/Append/InsertAfter/RemoveAt/
  MoveUp/MoveDown/Clear/Shuffle/PlayAt/Get` **只接受本地路径**（远程 URL 被拒绝），
  单次写入不超过 500 条，分页读取上限 `RPC_PAGE_MAX`（1000）条；`QueueChanged`
  广播条目数/当前游标/版本号，任意多个前端因此保持一致。
- **前端注册**：每个前端都要注册并心跳，`core_exit_when_no_frontend` 与
  `Info.GetInfo.frontends` 都基于它。断线不是致命的：前端按指数退避
  （1/2/4/8/15/30 秒）重连，并把内容队列补推回去——核心自己没有任何内容。

### 二 架构门禁

三个脚本守住这条分界，三者都已接入 CI：

| 门禁 | 命令 | 规则 |
| --- | --- | --- |
| 后端纯度 | `scripts/test/check-core-purity.sh` | 后端目录（`audio config core info lyrics media queue` 与 `cli/daemon.c`）不得出现任何远程源符号，也不得引用内容/界面（playlist、library、search、界面渲染、前端头文件） |
| 前端纯度 | `scripts/test/check-ui-purity.sh` | 界面访问播放面**只能**经 `player` 门面——不得直连引擎播放全局或播放命令 |
| 配置归属 | `scripts/test/check-config-ownership.sh` | `config.xml` 归核心独有。前端**不得**出现 `save_config()` / `config_save_to_xml()`：它只改自己的配置镜像，再经门面 `player_config_persist()` 提交差异（远端模式下即 `Config.Set`） |

与之配套的回归套件：`scripts/test/run-unit-tests.sh`（路径队列、播放队列契约、
歌词解析、JSON 读取器、配置差异），以及端到端脚本 `dbus-rpc-check.sh`、
`config-migration-check.sh`、`lifecycle-e2e.sh`、`offline-reconnect-e2e.sh`、
`multi-frontend-e2e.sh`、`paging-deepdir-e2e.sh`、`remote-frontend-e2e.sh`，
还有性能探针 `perf-check.sh`（命令到可见状态的时延与空闲 CPU，均带阈值；
CI 只记录数值、不做判定）。

### 三 模块地图

本播放器采用模块化设计。**源文件**位于 `src/org.yxzl.ter-music/<module>/` 目录；**公开头文件**位于 `include/org.yxzl.ter-music/<module>/` 目录。

核心代码模块如下：

- **main/main.c**: 程序入口、参数处理、前端启动分叉（远端模式走
  `frontend_init_config()`，本地基线走 `init_all_persistent_data()`）
- **player/**: 播放门面——`player.c`（分发）、`player_local.c`（进程内后端）、
  `player_remote.c`（D-Bus 客户端，默认），声明见
  `include/…/player/player_backend.h`
- **core/core.c**: 核心侧启动引导，供守护进程与进程内基线共用
- **cli/cli.c、cli/cli_client.c**: **前端** CLI 子命令分发，以及
  `play`/`pause`/`show`/…… 所用的轻量 D-Bus 客户端；`play` 在本进程扫描内容、
  生成路径队列并下发
- **cli/daemon.c**: **核心**进程——配置、播放与前端看门狗；它从不扫描目录
- **queue/backend_queue.c**: 核心路径队列（顺序、游标、版本号、本地路径校验、
  单次下发上限）与 CUE 前瞻
- **audio/**: 核心音频引擎——解码与播放线程、环形缓冲、`play_queue.c`
  （转发层 + 本地基线使用的界面镜像）、atempo 变速、10 段均衡器、FFT 频谱数据、
  `backend_ops.c` 与 PipeWire / PulseAudio / ALSA 输出
- **lyrics/**: 核心歌词引擎——发现、内嵌（FFmpeg）与外部 `.lrc` 解析、来源偏好、
  时间轴与分页构建
- **ui/**: **前端** ncurses 界面——事件循环、控制栏、设置、菜单、歌单/队列视图、
  收藏、历史、浏览视图、布局、进度、可视化绘制、`lyrics.c`（只负责渲染，引擎在
  核心里）、盲文点阵、图片加载、对话框、鼠标、滚动条与共享控件
- **media/**: 核心 D-Bus 会话——`session.c`（总线名、自省、分发）、
  `rpc_common.c`（回复助手、分页）、`rpc_info.c`、`rpc_control.c`、
  `rpc_queue.c`、`rpc_lyrics.c`、`rpc_config.c`，以及 MPRIS 媒体播放器接口
- **info/info.c**: 播放信息快照与渲染（文本、JSON、盲文/ASCII 封面缓存），供
  `ter-music show` 与 `Info` 接口共用
- **config/**: 配置子系统——`config.c`（libxml2 读写、版本迁移、默认值）、
  `config_json.c` + `migration.c`（v1 `config.json` → XML）、schema 常量、
  `crypto.c`（前端远程存储的密码加密解密）
- **playlist/**: **前端**歌单加载与元数据——递归目录扫描、FFmpeg + 原生 APEv2
  标签读取、CUE 检测与编码自动识别、专辑封面提取与 MRU 封面缓存、
  `playlist_queue.c`（内容 → 路径队列 的桥）
- **library/**: **前端** SQLite 曲库——数据库模式（tracks + FTS5、收藏、历史、
  歌单）、扫描引擎、CRUD，以及 `browser/browser.c`（艺术家 → 专辑 → 曲目导航）
- **remote/**: **前端**远程音乐源——`remote.c`（SMB/SFTP/FTP/WebDAV/HTTP，
  libcurl）、`remote_store.c`（服务器列表，存于 `<配置目录>/remote.xml`）、
  `remote_cache.c`（后台下载与本地缓存）；`ui/remote_view.c` 是
  **设置 → 远程设备**页
- **app/open.c**: 共享的路径打开与会话恢复原语，供 TUI 与 CLI `play` 使用
- **util/json.c**: 小型有界 JSON 读写器，供 `Lyrics`/`Info` 接口与队列载荷共用
- **util/utf8.c**: 两个平面都需要的 UTF-8 工具（核心歌词、CLI 输出）
- **search/search.c**: 异步搜索功能（支持拼音搜索，前端）
- **i18n/、logger/**: 语言包与日志记录子系统（共享）

## 第七章 开源协议
本项目遵循GNU General Public License v3.0开源协议。你可以自由使用、修改、分发本项目，但修改后的衍生作品必须同样遵循该协议开源，不得闭源。

## 第八章 免责声明

Ter-Music 是一款纯粹的音频播放工具，本身不提供、不托管、不分发任何音频文件或其他受版权保护的内容。用户必须自行提供合法获取的音频文件。本软件的本地播放与远程播放功能，仅设计用于播放用户合法获得的媒体文件。

与本软件所播放的音频内容相关的所有版权及知识产权，均归其各自权利人所有。因使用本软件播放音频内容而产生的任何版权纠纷，概由使用者自行承担全部责任。开发者对因使用本软件引起的任何版权或其他法律问题不承担任何责任。

## 第九章 开发者
- **开发者**：浣软科技（HuanSoft）
- **邮箱**：<yxzl666xx@outlook.com>
- **项目仓库**：<https://github.com/HuanSoft-Open-Source-Community/ter-music.git>

## 第九章 致谢
在此诚挚感谢以下贡献者的帮助：

- **@guanzi008** - 深入优化多项功能：添加Debian打包元数据、实现可选MPRIS媒体会话集成、完善DEB打包、修复UTF-8输入问题、优化设置引导、支持鼠标交互、规整歌单管理、优化目录排序、修复播放中断问题、提升性能、美化频谱显示、优化中文界面展示
- **@Zeta** - 拓展Arch Linux平台支持

## 第十章 贡献指引
如果你有问题或改进建议，欢迎提交Issue和Pull Request，我们非常欢迎。

## 第十一章 常见问题解答
**问题：没有声音输出**
- 音频后端按 PipeWire → PulseAudio → ALSA 顺序自动检测。运行 `pactl info` 或 `pw-cli info` 查看哪个服务正在运行
- 检查扬声器音量是否开启
- 如果使用PipeWire，确保 `pipewire` 和 `wireplumber` 服务正在运行
- 如果使用PulseAudio，执行 `systemctl status pulseaudio` 验证
- 也可以在设置界面（F2）→ 音频后端中手动切换

**问题：播放音质不佳、声音断断续续或有杂音**
- 不同机器的音频设备性能不同，默认的输出时延参数可能不适合您的设备
- 请尝试在设置界面（按`F2`进入设置）中调高"输出时延"的值，或直接编辑配置文件`~/.config/ter-music/config.xml`中的`audio_latency_ms`字段
- 时延值可调范围为20-250毫秒，建议每次增加10毫秒逐步测试，直至播放恢复正常
- 若调高时延后仍有问题，可检查PipeWire/PulseAudio配置或更新音频驱动

**问题：中文显示乱码，或CJK字符显示为方块**
- 确保终端使用UTF-8编码
- 检查系统locale设置：执行`locale`命令应显示`LC_CTYPE=UTF-8`相关内容
- 若在tty终端中CJK字符仍乱码，可更换为kmscon终端，其对东亚字符的支持更好

**问题：编译时找不到头文件**
- 确保安装了所有依赖的开发包（详见第三章）
- 多数系统会将开发包和运行包分开，需安装带*-devel或*-dev后缀的包

**问题：无法打开某些音频文件**
- 确认你的FFmpeg版本支持该音频格式
- 新版FFmpeg支持的格式更全面，建议升级

**问题：CUE分轨不显示**
- 确保.cue文件与音频文件同名（如 `album.flac` + `album.cue`）
- 如果CUE文字显示乱码，在设置 → CUE字符编码中更改编码（中文内容尝试GBK，日文尝试Shift-JIS）

**问题：音乐库没有显示我的所有音乐**
- 按`M`键进入音乐库浏览模式，检查 `~/.config/ter-music/` 下是否存在 `library.db`
- 音乐库在启动时扫描，如果你添加了新音乐，重启程序可触发重新扫描
- 音乐库现在支持**递归目录扫描**，可以嵌套子目录结构
