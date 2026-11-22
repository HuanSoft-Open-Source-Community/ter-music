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

播放器分为两层：**核心**负责音频设备、传输控制和前端交给它的路径队列；**前端**（TUI 或 CLI）负责文件、曲库、歌单、历史与远程下载。没有核心时，前端会自动拉起一个：容器外它在后台 fork 一个核心，Linyaps 容器内不能 fork 脱离进程，改为请求 D-Bus 激活（见“命令行”一节的 Linyaps 说明）。

## 功能

**播放**
- MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、WavPack；FLAC/APE/WV 支持 CUE 分轨，自动识别 GBK、BIG5、Shift-JIS 编码。
- 17 种播放模式（顺序、循环、随机，以及文件夹/专辑/艺术家范围），6 档倍速（0.75x–3x），1.0x 下预载下一曲以实现无缝衔接。
- 10 段 ISO 均衡器（31 Hz–16 kHz，±12 dB），带预设和软限幅。
- 卡拉 OK（消人声）：关、弱、全三档，在 200 Hz–5 kHz 内逐频点做中置抵消；仅立体声，启用时约 9 ms 算法延迟。
- 内嵌或外部 LRC 歌词；时间戳允许时逐字高亮。

**内容**
- SQLite 曲库，FTS5 搜索、拼音匹配、递归扫描；按艺术家、专辑、流派浏览。
- 播放队列、自建歌单、收藏、播放历史、目录历史。
- SMB、SFTP、FTP、WebDAV、HTTP 远程源；前端逐曲下载到本地缓存，再把本地路径交给核心。SMB 需要 Samba 客户端，SFTP 需要 known_hosts 文件（见[远程音乐源](#远程音乐源)）。

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

必需：CMake 3.10+、C99 编译器、pkg-config、ncursesw、FFmpeg 库（avcodec、avformat、avutil、avfilter、swresample、swscale）、libpng、libjpeg、libxml2、SQLite3、zlib 与 libcurl。libcurl 不是可选项：缺失时 CMake 会以明确错误终止构建。

可选：PipeWire、PulseAudio、ALSA 输出；D-Bus（MPRIS/API）。SMB 远程源另外需要运行期的外部程序 `smbclient`（Samba 客户端）；缺少它仍可构建，SMB 连接会明确提示未安装。

**Fedora / RHEL**

```bash
sudo dnf install cmake gcc make pkg-config ncurses-devel \
  ffmpeg-free-devel libpng-devel libjpeg-turbo-devel libxml2-devel \
  sqlite-devel zlib-devel libcurl-devel
# 可选
sudo dnf install pipewire-devel pulseaudio-libs-devel alsa-lib-devel \
  dbus-devel
# SMB 远程源需要运行期的 Samba 客户端
sudo dnf install samba-client
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential cmake pkg-config libncursesw5-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev libpng-dev libjpeg-dev libxml2-dev \
  libsqlite3-dev zlib1g-dev libcurl4-openssl-dev
# 可选
sudo apt install libpipewire-0.3-dev libpulse-dev libasound2-dev \
  libdbus-1-dev
# SMB 远程源需要运行期的 Samba 客户端
sudo apt install smbclient
```

**Arch Linux**

```bash
sudo pacman -S --needed base-devel cmake pkgconf ncurses ffmpeg \
  libpng libjpeg-turbo libxml2 sqlite zlib curl
# 可选
sudo pacman -S --needed libpipewire libpulse alsa-lib dbus
# SMB 远程源需要运行期的 Samba 客户端
sudo pacman -S --needed smbclient
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
ter-music                        # 启动 TUI；没有核心时自动拉起（Linyaps 内为 D-Bus 激活）
ter-music -o ~/Music             # 打开目录或文件
ter-music -o ftp://host/music/   # 在 TUI 中打开远程目录
```

TUI 需要真实终端，不要把输出重定向到管道。按 `O` 打开目录，`↑`/`↓` 选择，`Enter` 或 `Space` 播放，`q` 退出。

这样打开的远程 URL 只使用 URL 里携带的凭据。需要密码的服务器，请在 `F2` → 远程设备 里保存连接再打开目录，不要把 `user:password@` 写进 URL：URL 会留在 shell 历史和进程列表里。`ter-music play` 与 `ter-music daemon start --open` 会拒绝远程 URL，因为核心只播放本地路径。

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

鼠标：左键选择并执行，滚轮滚动，右键打开上下文菜单；会拦截右键的终端可用中键。菜单滑入动画每个位移只合成一帧，慢终端与流光模式下自动降级或关闭，输入无需等待动画播完；可用 `TER_MUSIC_MENU_ANIM=full|reduced|off` 强制档位（默认自动）。布局随终端尺寸自动重排。

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

TUI 在容器内不能 fork 脱离的核心，因此会请求宿主机激活服务。若激活不可用，请先在宿主机以前台方式启动核心，再打开 TUI：

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music daemon foreground
```

**ll-box 冷启动重放。** ll-box ≤ 2.3.5 的冷启动转发在终端队列被灌满时，会把已经写出的字节再重放一遍，把重复输出拼进 TUI。应用无法阻止这次写入；容器内因此逐帧度量真正写进终端的字节数（`/proc/self/io` 的 `wchar`），只有在检测到写入突发或背压证据，或发生终端缩放后，才执行一次整屏强制重绘。裸模式重置同样只在出现该证据（或终端缩放）后执行，`endwin`+`refresh` 兜底仍须显式开启（`TER_MUSIC_TTY_HARD_RESET=1`，默认关闭）。容器外不会运行这些逻辑，守卫也没有新增配置键、设置页条目或用户可见的开关。已知缺口：无证据时不再按计时器排期重绘；因此只输出小帧、终端读取又慢的会话里若出现损坏，不会被主动重绘，可能要等到下一次重绘类事件（终端缩放、切换视图，或出现新的突发/背压证据）才被覆盖。

根治在上游：PR #295 已合入 linyaps-box `release/2.3`（merge commit `31ddf04`，2026-11-20），转发在 `TryAgain` 且 `k > 0` 时改为消费已写出的字节；影响 ll-box ≤ 2.3.5。目前还没有包含该修复的发行版（最新发行版仍是 2.3.5），`master` 也尚未带上它，因此安装前应确认所用 tag 是否包含，不要假定下一个版本一定包含。ter-music 的下一个 Release 计划不包含 UAB；UAB 产物计划等上游修复释出后再发布。`TER_MUSIC_TTY_REPAIR_BACKSTOP_MS` 是未文档化的调参变量，只用于调节被删除的计时兜底；它已随该兜底一并移除，现在设置它不会产生任何效果。

## 播放

| 分组 | 模式 |
| --- | --- |
| 基础（5） | 顺序、单曲循环、列表循环、随机一次、随机循环 |
| 文件夹（4） | 顺序、循环、随机、随机循环 |
| 专辑（4） | 顺序、循环、随机、随机循环 |
| 艺术家（4） | 顺序、循环、随机、随机循环 |

`1`-`5` 依次选择顺序、单曲循环、列表循环、随机循环、文件夹顺序。高级模式需要曲库元数据，在“设置 → 播放模式”中启用。

倍速：0.75x、1.0x、1.25x、1.5x、2x、3x，保持音高（FFmpeg atempo）。均衡器：31 Hz–16 kHz 共 10 段，预设见“设置 → 均衡器”。

消人声：关、弱、全三档，仅立体声生效。它在 200 Hz–5 kHz 内逐频点判断 side/mid 幅度比，抵消居中的相干内容——弱档残留约 -8 dB，全档彻底消除；声场展开的伴奏与声像硬偏一侧的瞬态保留，因此硬偏一侧的镲或军鼓不会再把居中的人声抵消放掉。单声道曲目原样直通。启用后处理流延迟为窗长减 1 个样本（44.1 kHz 下 383 个样本，约 8.7 ms；所有支持采样率均不超过 10 ms）；关档时抵消约 0.35 s 淡出后才恢复逐位直通。可从控制栏弹层、“设置 → 播放”、`ter-music karaoke` 或 `Control.SetKaraoke` 修改。

## 远程音乐源

远程源由**前端**负责：前端列出远程目录，把其中的音频逐曲下载到本地缓存，再把缓存里的本地路径交给核心播放；核心从不打开远程 URL（队列契约见 [API_DBUS_en_US.md](../API_DBUS_en_US.md)）。

在 `F2` → **远程设备**（设置页第 6 项）中添加连接：名称、协议（SMB、SFTP、FTP、WebDAV、HTTP）、主机、端口（留空用协议默认端口）、用户名、密码和基础路径。基础路径必填：`/` 表示服务器根目录；SMB 要填共享名加可选子目录，例如 `music/albums`，而不是 `smb://host/music/albums`。HTTP、WebDAV、FTP 有 TLS 开关（https/webdavs/ftps），开启后还会出现“信任自签名证书”开关。SFTP 另有私钥路径、私钥口令和 known_hosts 路径。目录路径/URL 不需要手工补尾斜杠，前端列目录时会自动补上。TLS 是每条连接的显式开关，不按端口判断：非 443 端口的 HTTPS 服务器需要在连接里打开 TLS 并把端口填成实际端口。端口不在 1–65535 或基础路径为空时不能保存；删除连接需要二次确认。

协议说明：

- **SMB** 调用外部 `smbclient` 程序（不走 libcurl），主机、端口、用户名、密码都会传给客户端；密码经临时 0600 认证文件传递，绝不进入 argv。需要安装 Samba 客户端（包名 `smbclient`；Debian/Arch/RPM 的打包元数据将其列为推荐/可选依赖），否则 SMB 连接会明确提示缺少 `smbclient`。文件名含 `;`、`!`、双引号、反斜杠或控制字符时会被拒绝下载，因为 `smbclient -c` 会把它们拆成命令；请在服务器端改名。
- **SFTP** 校验主机密钥且 fail-closed：必须存在可读的 known_hosts 文件（连接里填的路径，否则 `~/.ssh/known_hosts`），且其中已有该服务器。不会静默地首次信任（TOFU）；请先用 `ssh-keyscan HOST >> ~/.ssh/known_hosts` 写入。私钥与口令可选。
- **FTP** 先以 MLSD 列目录，失败则退回 LIST。TLS 指隐式 FTPS（`ftps://`，默认端口 990）。
- **WebDAV** 发送 `PROPFIND`（`Depth: 1`），同时接受带命名空间的 `<D:href>`（Apache/Nextcloud 形状）与无命名空间的响应。
- **HTTP** 读取 HTML 目录索引（Apache/nginx/python 形状），以链接目标（href）为准而不是显示文本，并解码 HTML 实体。
- HTTP 与 WebDAV 的认证由 libcurl 协商 Basic、Digest 或 NTLM。开启 TLS 时默认校验证书，除非打开“信任自签名证书”；HTTPS 传输不会被重定向降级为 HTTP。

打开目录会启动后台任务：列出该目录并下载其中的音频文件（只含该目录下的文件，不含子目录），最多每次打开下载 200 首；目录更大时会提示已加载/总数，子目录可分别打开。远程设备页与主界面状态栏都显示进度（`下载中 done/total，失败 n`），`ESC` 可取消当前任务，连接动作菜单提供“取消当前任务”“重试列目录”“重试失败曲目(n)”。每首下载完成后立即交给核心：队列会被这批曲目替换，并从第一首开始播放。

限制：单个远程文件最大 1 GiB，单次列目录最多 32 MiB（20 万条）；连接超时 10 秒，列目录 120 秒，单曲下载 600 秒。单个文件超过上限会被跳过，其余曲目继续；列目录过大或缓存已满则以错误终止任务。下载按需进行（打开哪个目录就下载哪个），没有预取。

缓存位于 `$XDG_CACHE_HOME/ter-music/remote/`（默认 `~/.cache/ter-music/remote/`），按连接与远端路径镜像存放。远程设备页显示缓存用量，可设置上限（默认 2048 MB，`0` = 不限制）并清理未受保护的文件；正在播放、在队列中、已收藏或在历史中的文件会保留。音频旁的 `.lrc` 会随音频一起缓存。

**凭据。** 连接表保存在前端自己的 `remote.xml`（权限 0600）。密码用固定盐 XOR 流做混淆，不是真正加密：能防肉眼查看，防不住能读该文件的人。日志、核心的 `last_opened_path` 和曲库的目录历史只保存脱敏后的 URL，因此从历史里复取的 URL 不再带密码——需要密码的服务器请使用已保存的连接。临时打开远程源时不要把 `user:password@` 写进 URL：URL 会留在 shell 历史与进程列表里。

**已验证范围。** 自动化测试覆盖 HTTP、WebDAV、FTP、FTPS 的 TLS 握手、非 443 端口的 HTTPS、自签名 CA 信任、401/404 以及慢速下载取消。SMB 通过遵循 Samba 命令串解析的假 `smbclient` 验证；SFTP 只验证 known_hosts 的 fail-closed 保护。真实 Samba 服务器、真实 SSH 服务器与 FTPS 数据通道没有条件在测试中覆盖。

TUI 也可以直接打开远程 URL：

```bash
ter-music -o webdav://host/music/
```

接受的 scheme 有 `http`、`https`、`ftp`、`ftps`、`sftp`、`smb`、`webdav`、
`webdavs`（`dav`/`davs` 是别名），且大小写不敏感。

`ter-music play` 与 `ter-music daemon start --open` 会拒绝远程 URL，因为核心只播放本地路径；请在 TUI 中打开远程目录。

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
remote.xml     远程服务器与缓存上限（仅前端，权限 0600）
layouts.json   用户布局覆盖
ui-layout      当前布局 id
lang/          用户语言包
help/          用户快速上手文本
```

远程曲目缓存在 `$XDG_CACHE_HOME/ter-music/remote/`（默认 `~/.cache/ter-music/remote/`），按连接与远端路径镜像存放；同名 `.lrc` 会随曲目缓存。缓存上限与清理入口在“设置 → 远程设备”。封面缓存在 `/tmp/ter-music-cover-*.jpg`，退出时清理。

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
- **Linyaps 里 TUI 出现重复或错乱的行：** 即上文 Linyaps 一节所述 ll-box ≤ 2.3.5 冷启动重放。守卫在检测到写入突发或背压证据时会整屏重绘；没有证据时不再按计时器重绘，损坏可能要等到终端缩放、切换视图或出现新证据才被覆盖。
- **编译时找不到头文件：** 安装上面依赖的 `-devel`/`-dev` 包。
- **远程目录列不出来：** 检查协议、主机、端口与基础路径（SMB 要填 `共享名/子路径`）；SMB 需要安装 `smbclient`；SFTP 需要可读的 known_hosts；https/webdavs/ftps 需要打开 TLS。页面会显示本地化原因，连接动作菜单里有“重试列目录”。
- **远程曲目下载失败：** 页面显示 `失败 n` 与首条错误；排除原因（网络、权限、磁盘空间）后使用“重试失败曲目(n)”，已缓存的曲目不会重复下载。单文件超过 1 GiB 或缓存已满会在该曲停止；可在远程设备页提高上限或清理缓存。
- **远程目录下载到一半停了：** 单次打开最多下载 200 首；请进入子目录分别打开。
- **从目录历史重新打开远程 URL 时认证失败：** 入库的 URL 已脱敏（设计上不含密码）；需要密码的服务器请使用远程设备页里保存的连接。
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
