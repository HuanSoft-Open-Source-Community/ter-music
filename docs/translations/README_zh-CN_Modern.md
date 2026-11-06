<div align="center">

# Ter-Music

**Linux 终端音乐播放器**

![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
[![CI](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml/badge.svg)](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/HuanSoft-Open-Source-Community/ter-music?sort=semver)](https://github.com/HuanSoft-Open-Source-Community/ter-music/releases)
![Linyaps](https://img.shields.io/badge/Linyaps-支持-8A2BE2.svg)

</div>

**语言：** [English](../README.md) · [中文（文言）](README_zh-CN_Legacy.md)

**文档：** [构建与打包](../BUILD_GUIDE.md) · [TUI 布局](../UI_LAYOUTS_en_US.md) · [D-Bus API](../API_DBUS_en_US.md) · [歌词 API](../API_LYRICS_en_US.md) · [发布流程](../RELEASE.md)

## 概述

Ter-Music 在终端中播放本地与远程音乐。FFmpeg 解码，ncursesw 绘制界面，运行时自动选择 PipeWire、PulseAudio 或 ALSA。内置 SQLite 曲库；编译时启用 D-Bus 后，还提供 MPRIS 与 D-Bus 控制接口。

播放器分为两层：**核心**负责音频设备、传输控制和前端交给它的路径队列；**前端**（TUI 或 CLI）负责文件、曲库、歌单、历史与远程下载。没有核心时，启动 TUI 会自动拉起一个。

## 功能

**播放**
- MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、WavPack；FLAC/APE/WV 支持 CUE 分轨，自动识别 GBK、BIG5、Shift-JIS 编码。
- 17 种播放模式（顺序、循环、随机，以及文件夹/专辑/艺术家范围），6 档倍速（0.75x–3x），1.0x 下预载下一曲以实现无缝衔接。
- 10 段 ISO 均衡器（31 Hz–16 kHz，±12 dB），带预设和软限幅。
- 卡拉 OK（消人声）：关、弱、全三档，频带受限的中置消除；仅立体声。
- 内嵌或外部 LRC 歌词；时间戳允许时逐字高亮。

**内容**
- SQLite 曲库，FTS5 搜索、拼音匹配、递归扫描；按艺术家、专辑、流派浏览。
- 播放队列、自建歌单、收藏、播放历史、目录历史。
- SMB、SFTP、FTP、WebDAV、HTTP 远程源；前端逐曲下载到本地缓存。

**界面**
- 六个窗格（播放列表、信息、控制、频谱、歌词、黑胶），7 个 JSON 布局预设，强制平铺。
- 24 套主题调色板加 1 个自定义槽位；每个窗格可单独设置前景/背景色。
- 专辑封面以彩色半块渲染；黑胶窗格显示旋转的封面圆标。
- 键盘与鼠标操作；内置 `zh_CN`、`en_US`、`zh-PQ` 三种语言。

**集成**
- CLI 与脱离终端的后台播放守护进程，可从任意终端控制。
- 编译时启用 D-Bus 后提供 MPRIS，以及 Info、Control、Queue、Config、Lyrics 接口。
- 支持 Linyaps 打包。

## 安装

### Arch Linux

AUR 配方已准备为 `cn.org.huansoft.ter-music`，但**尚未投递**。请从下方源码构建，或使用 [BUILD_GUIDE.md](../BUILD_GUIDE.md) 描述的本地打包产物。

### 从源码构建

必需：CMake 3.10+、C99 编译器、pkg-config、ncursesw、FFmpeg 库（avcodec、avformat、avutil、avfilter、swresample、swscale）、libpng、libjpeg、libxml2、SQLite3、zlib。

可选：PipeWire、PulseAudio、ALSA 输出；D-Bus（MPRIS/API）；libcurl（远程源）。

**Fedora / RHEL**

```bash
sudo dnf install cmake gcc make pkg-config ncurses-devel \
  ffmpeg-free-devel libpng-devel libjpeg-turbo-devel libxml2-devel \
  sqlite-devel zlib-devel
# 可选
sudo dnf install pipewire-devel pulseaudio-libs-devel alsa-lib-devel \
  dbus-devel libcurl-devel
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential cmake pkg-config libncursesw5-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev libpng-dev libjpeg-dev libxml2-dev \
  libsqlite3-dev zlib1g-dev
# 可选
sudo apt install libpipewire-0.3-dev libpulse-dev libasound2-dev \
  libdbus-1-dev libcurl4-openssl-dev
```

**Arch Linux**

```bash
sudo pacman -S --needed base-devel cmake pkgconf ncurses ffmpeg \
  libpng libjpeg-turbo libxml2 sqlite zlib
# 可选
sudo pacman -S --needed libpipewire libpulse alsa-lib dbus curl
```

**构建**

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
sudo make install   # 可选
```

可执行文件为 `build/ter-music`（在构建目录内为 `./ter-music`）。本地产物：1 个 DEB（Debian 10–13 / Ubuntu 20.04–26.04）、4 个 RPM（EL7–10）、1 个 AppImage（glibc 2.28）、1 个 Linyaps UAB + layer；DEB/RPM 的源码包与调试包为可选项。见 [BUILD_GUIDE.md](../BUILD_GUIDE.md)。

## 快速开始

```bash
ter-music                  # 启动 TUI；没有核心时自动拉起
ter-music -o ~/Music       # 打开目录或文件
ter-music ftp://user:pass@host/music
```

TUI 需要真实终端，不要把输出重定向到管道。按 `O` 打开目录，`↑`/`↓` 选择，`Enter` 或 `Space` 播放，`q` 退出。

| 按键 | 功能 |
| --- | --- |
| `O` / `o` | 打开目录 |
| `↑` `↓` / `j` `k` | 选择 |
| `Enter` / `Space` | 播放或打开 |
| `n` / `p` | 下一曲 / 上一曲 |
| `,` / `.` | 后退 / 前进 5 秒 |
| `-` / `=` | 音量减 / 加 |
| `C` / `L` | 焦点切到控制栏 / 列表 |
| `Tab` | 文件浏览 ↔ 播放队列 |
| `Ctrl+L` | 歌词定位模式 |
| `M` | 曲库浏览器 |
| `/` 或 `S` | 搜索（支持拼音） |
| `a` / `i` | 追加 / 插入为下一首 |
| `I` | 追加整个目录 |
| `d` / `D` | 从队列移除 / 清空队列 |
| `F` / `f` | 切换收藏 |
| `A` | 加入歌单 |
| `h` | 播放历史弹窗 |
| `R` | 重启核心 |
| `F1`-`F9` | 切换视图与退出 |
| `q` | 退出 |

完整按键见 `F8` 与 `ter-music keys list`；在“设置 → 快捷键”中改键。`F3` 是目录历史，`h` 是播放历史弹窗。

鼠标：左键选择并执行，滚轮滚动，右键打开上下文菜单；会拦截右键的终端可用中键。布局随终端尺寸自动重排。

## 命令行

所有子命令都是运行中核心的 D-Bus 客户端；`play` 还会扫描给定路径并下发队列。

| 命令 | 用途 |
| --- | --- |
| `play [PATH]` | 扫描路径并播放 |
| `pause` `resume` `toggle` `stop` | 传输控制 |
| `next` `prev` | 切歌 |
| `seek` | 跳转：+秒、-秒、mm:ss 或百分比 |
| `volume` | 查询或设置音量（0-100、+N、-N） |
| `speed` | 查询或设置倍速（0.5-3.0） |
| `mode` | 查询或设置播放模式（名称或 0-16） |
| `karaoke` | 消人声档位（off、soft、full、0-2、cycle） |
| `queue` | 编辑队列（list、clear、shuffle、remove、append、insert） |
| `config` | 读取或修改配置（get、set、reload、reset） |
| `show` | 输出信息块 |
| `lyrics` `track` `cover` | 当前歌词、元数据或字符封面 |
| `keys` | 审计按键绑定（list、check） |
| `daemon` | 管理核心（start、foreground、stop、restart、status、reload） |
| `tui` `version` `help` | TUI、版本、帮助 |

```bash
ter-music play ~/Music
ter-music show --one-line --watch=1000
ter-music show --json | jq -r '.track.title'
ter-music daemon start --open ~/Music
ter-music daemon stop
```

退出码：`0` 成功，`1` 用法错误，`3` 没有运行中的核心，`4` D-Bus 不可用，`5` 被核心拒绝。

`ter-music help <command>` 与 `ter-music help all` 输出完整说明。`--lang <id>` 只影响本次运行的 CLI 语言。

**Linyaps。** 包内的可执行文件位于容器中：

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music show
ll-cli run cn.org.huansoft.ter-music -- ter-music play ~/Music
```

后台播放在容器内改用 D-Bus 激活或随包提供的 systemd 用户服务；也可在 `ll-cli run` 中加 `--foreground`；在宿主机上用 `systemctl --user enable --now cn.org.huansoft.ter-music` 启用该服务。容器已在运行时，用 `ll-cli enter` 或 `journalctl --user -u cn.org.huansoft.ter-music` 读取输出。

## 播放

| 分组 | 模式 |
| --- | --- |
| 基础（5） | 顺序、单曲循环、列表循环、随机一次、随机循环 |
| 文件夹（4） | 顺序、循环、随机、随机循环 |
| 专辑（4） | 顺序、循环、随机、随机循环 |
| 艺术家（4） | 顺序、循环、随机、随机循环 |

`1`-`5` 依次选择顺序、单曲循环、列表循环、随机循环、文件夹顺序。高级模式需要曲库元数据，在“设置 → 播放模式”中启用。

倍速：0.75x、1.0x、1.25x、1.5x、2x、3x，保持音高（FFmpeg atempo）。均衡器：31 Hz–16 kHz 共 10 段，预设见“设置 → 均衡器”。消人声：关、弱、全三档，仅立体声；可从控制栏弹层、“设置 → 播放”、`ter-music karaoke` 或 `Control.SetKaraoke` 修改。

## 歌词与封面

- 优先读取内嵌歌词（FFmpeg/APE 标签），没有时读取同名的外部 `.lrc`。
- LRC 带逐字、行首尾或插值时间戳时，按字高亮。
- `Ctrl+L` 进入歌词定位模式；`Tab` 在内嵌与外部歌词之间切换。
- 封面先取内嵌图片，再找同目录的 `cover`/`folder`/`front`/`album` 图片。
- 渲染从 256 色回退到 8 色再到黑白；`ter-music cover` 与 `Info.GetCoverArt` 提供盲文、ASCII 或半块字符封面。

## 布局与主题

布局是一棵二叉分栏树，叶子是六种窗格之一，因此主区域始终强制平铺；终端缩放时按千分比重算。预设为 `classic`、`vinyl`、`karaoke`、`visualizer`、`focus`、`stage`、`neon`。

在 `F2` → 界面外观 → 界面布局 中选择或编辑布局。文件：

- 内置：`<prefix>/share/ter-music/layouts/layouts.json`
- 用户覆盖：`~/.config/ter-music/layouts.json`
- 开发覆盖：`TER_MUSIC_LAYOUTS=/path/to/layouts.json`

24 套调色板加 1 个自定义颜色槽位负责侧边栏、高亮和边框；每个窗格也可单独设置前景/背景色。完整格式见 [UI_LAYOUTS_en_US.md](../UI_LAYOUTS_en_US.md)。

## 配置与数据

`$XDG_CONFIG_HOME/ter-music`（默认 `~/.config/ter-music`）：

```text
config.xml     核心配置
library.db     SQLite 曲库：曲目、收藏、历史、歌单、目录历史
remote.xml     远程服务器（仅前端）
layouts.json   用户布局覆盖
ui-layout      当前布局 id
lang/          用户语言包
help/          用户快速上手文本
```

远程曲目缓存在 `$XDG_CACHE_HOME/ter-music/remote/`；封面缓存在 `/tmp/ter-music-cover-*.jpg`，退出时清理。

常用 `config.xml` 键：

| 键 | 取值 |
| --- | --- |
| `audio_backend` | 0 自动，1 PulseAudio，2 ALSA，3 PipeWire |
| `audio_latency_ms` | 输出延迟，20-250 |
| `volume_percent` | 默认音量，0-100 |
| `default_playback_speed` | 0.75-3.0 |
| `default_play_mode` | 0-16 |
| `ui_language` | `zh_CN`、`en_US`、`zh-PQ` 或已安装的语言包 ID |
| `karaoke_mode` | 0 关，1 弱，2 全 |
| `resume_last_playback` | 恢复上次曲目与位置 |
| `stop_core_on_exit` | 1 退出时停止核心，0 继续播放 |
| `core_exit_when_no_frontend` | 最后一个前端离开 10 秒后退出核心 |

只有改动过的按键会写入 `config.xml`：

```xml
<keybindings>
  <binding action="track.next" key="]"/>
  <binding action="track.prev" key="["/>
</keybindings>
```

保留键为 `F1`-`F9`、`q`/`Q`、`Tab`/`Shift+Tab`。动作 ID 用 `ter-music keys list` 查，文件用 `ter-music keys check` 校验。

语言包是 XML 文件（`<lang id="..." name="...">`，内含 `<string key="...">`），放在 `~/.config/ter-music/lang/`，或在 `F7` 语言视图中用 `A` 安装 `.tar.gz`、用 `D` 删除。内置语言包：`zh_CN`、`en_US`、`zh-PQ`。

## 集成

编译时启用 D-Bus 后，MPRIS 以 `org.mpris.MediaPlayer2.ter_music` 发布在 `/org/mpris/MediaPlayer2`。同一对象还提供：

| 接口 | 用途 |
| --- | --- |
| `cn.org.huansoft.ter_music.Info` | 曲目、进度、封面、歌词行、信息块 |
| `cn.org.huansoft.ter_music.Control` | 传输、跳转、音量、倍速、模式、消人声、退出 |
| `cn.org.huansoft.ter_music.Queue` | 核心的本地路径队列 |
| `cn.org.huansoft.ter_music.Config` | 配置读写 |
| `cn.org.huansoft.ter_music.Lyrics` | 歌词行与逐字时间 |

`Info.GetInfo` 返回 `core.api_version`（当前 7）。详见 [API_DBUS_en_US.md](../API_DBUS_en_US.md) 与 [API_LYRICS_en_US.md](../API_LYRICS_en_US.md)。

## 开发

| 层 | 职责 |
| --- | --- |
| 核心（`daemon`） | 音频设备、解码、传输、路径队列、当前曲目信息、配置 |
| 前端（TUI/CLI） | 曲库、扫描、歌单、收藏、历史、远程源、界面 |

源码位于 `src/cn.org.huansoft.ter-music/<module>/`；公共头文件位于 `include/cn.org.huansoft.ter-music/<module>/`。

```bash
mkdir -p build/debug
cd build/debug
cmake -DCMAKE_BUILD_TYPE=Debug ../..
make -j"$(nproc)"
cd ../..
./scripts/test/run-unit-tests.sh
```

CI 运行四道门禁：

| 门禁 | 规则 |
| --- | --- |
| `scripts/test/check-core-purity.sh` | 核心不含内容或远程源符号 |
| `scripts/test/check-ui-purity.sh` | UI 只能通过 `player` 门面访问播放 |
| `scripts/test/check-config-ownership.sh` | 核心是 `config.xml` 的唯一写入者 |
| `scripts/test/check-package-identity.sh` | 各打包位置的应用 ID 与版本一致 |

## 故障排查

- **没有声音：** 确认 PipeWire、PulseAudio 或 ALSA 正在运行；后端按此顺序探测。显式选择的后端在播放失败时会回退到下一个可用后端。可在“设置 → 音频”中切换。
- **卡顿或爆音：** 在设置中调高输出延迟（20-250 ms，每次加 10 ms）。
- **中文乱码：** 使用 UTF-8 locale 和 UTF-8 终端；`locale` 应显示 `LC_CTYPE=UTF-8`。
- **编译时找不到头文件：** 安装上面依赖的 `-devel`/`-dev` 包。
- **CUE 分轨不显示：** `.cue` 必须与音频文件同名；文字乱码时在设置中改 CUE 编码。
- **曲库为空：** 按 `M` 打开曲库浏览器并扫描或重新扫描；数据库是配置目录下的 `library.db`。

## 许可

GPL-3.0-only，见 [LICENSE](../../LICENSE)。

Ter-Music 只是播放工具，不提供、不托管、不分发音频内容；请自行确保所播放文件的合法性。

## 作者

浣软科技（HuanSoft）· <yxzl666xx@outlook.com>

## 致谢

- **@guanzi008**：Debian 打包、MPRIS、UTF-8 输入、鼠标与界面修复。
- **@Zeta**：Arch Linux 打包。
