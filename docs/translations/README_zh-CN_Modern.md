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

**文档：** [构建与打包](../BUILD_GUIDE.md) · [TUI 布局](../UI_LAYOUTS_en_US.md) · [D-Bus API](../API_DBUS_en_US.md) · [歌词 API](../API_LYRICS_en_US.md) · [发布流程](../RELEASE.md) · [26.4.1.0 发布说明](../RELEASE_NOTES_26.4.1.0.md)

## 概述

Ter-Music 在终端中播放本地与远程音乐。FFmpeg 解码，ncursesw 绘制界面，运行时自动选择 PipeWire、PulseAudio 或 ALSA。内置 SQLite 曲库；编译时启用 D-Bus 后，还提供 MPRIS 与 D-Bus 控制接口。

播放器分为两层：**核心**负责音频设备、传输控制和前端交给它的路径队列；**前端**（TUI 或 CLI）负责文件、曲库、歌单、历史与远程下载。没有核心时，前端会自动拉起一个：容器外它在后台 fork 一个核心，Linyaps 容器内不能 fork 脱离进程，改为请求 D-Bus 激活（见“命令行”一节的 Linyaps 说明）。

## 功能

**播放**
- MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、WavPack；FLAC/APE/WV 支持 CUE 分轨，自动识别 GBK、BIG5、Shift-JIS 编码。
- 17 种播放模式（顺序、循环、随机，以及文件夹/专辑/艺术家范围），6 档倍速（0.75x–3x），1.0x 下预载下一曲以实现无缝衔接。
- 10 段 ISO 均衡器（31 Hz–16 kHz，±12 dB），带预设和软限幅。
- 卡拉 OK（消人声）：使用随包内置的 ONNX 模型（UVR-MDX-NET 3），分不启用 / 人声较弱 / 消除人声三档；`[卡拉OK]` 按钮与三档始终可用——没有功能开关，也没有模型选择；档位非“不启用”时，无缓存曲目的首次起播会等待首段伴奏（见[卡拉 OK](#卡拉-ok)）。
- 内嵌或外部 LRC 歌词；时间戳允许时逐字高亮。

**内容**
- SQLite 曲库，FTS5 搜索、拼音匹配、递归扫描；按艺术家、专辑、流派浏览。
- 播放队列、自建歌单、收藏、播放历史、目录历史。
- SMB、SFTP、FTP、WebDAV、HTTP 远程源；前端逐曲下载到本地缓存，再把本地路径交给核心。SMB 需要 Samba 客户端，SFTP 需要 known_hosts 文件（见[远程音乐源](#远程音乐源)）。

**界面**
- 六个窗格（播放列表、信息、控制、频谱、歌词、黑胶），7 个 JSON 布局预设，强制平铺。
- 24 套主题调色板加 1 个自定义槽位；每个窗格可单独设置前景/背景色。
- 终端亮/暗自适应：自动探测终端背景（OSC 11、deepin-terminal 主题、XDG portal、COLORFGBG、TERM 提示），出厂配色与 7 个布局预设随亮/暗切换；在 设置 → 界面外观 中选择自动、亮色或暗色。
- 专辑封面以彩色半块渲染；黑胶窗格显示旋转的封面圆标。
- info 窗格随可用空间自适应：按内容分列、长字段省略、封面可缩小或让位给文字、空间不足按重要度裁剪行、宽而矮时最多分 3 个子列（见 [info 窗格](#info-窗格)）。
- 键盘与鼠标操作；内置 `zh_CN`、`en_US`、`zh-PQ` 三种语言。
- 统一文件/目录选择器：有图形会话时用桌面对话框（`zenity`/`kdialog`，优先经 XDG 桌面 portal），纯 TTY 则手输路径；打开文件夹、追加文件夹、语言包安装与默认音乐目录四处入口共用它。

**集成**
- CLI 与脱离终端的后台播放守护进程，可从任意终端控制。
- 编译时启用 D-Bus 后提供 MPRIS，以及 Info、Control、Queue、Config、Lyrics 接口。
- 支持 Linyaps 打包。

## 安装

### Arch Linux

AUR 配方已准备为 `cn.org.huansoft.ter-music`，但**尚未投递**；它固定一个显式源码提交（`_commit`，当前临时钉在 `32737f0`，即 `testing` 最新提交，投递前会改成最终发布提交）。请从下方源码构建，或使用 [BUILD_GUIDE.md](../BUILD_GUIDE.md) 描述的本地打包产物。配方把 `onnxruntime-cpu` 列为卡拉 OK 的可选依赖；AUR 包由源码构建，因此卡拉 OK 用系统 ONNX Runtime，而不是二进制包里的静态副本。

### 从源码构建

必需：CMake 3.10+、C99 编译器、pkg-config、ncursesw、FFmpeg 库（avcodec、avformat、avutil、avfilter、swresample、swscale）、libpng、libjpeg、libxml2、SQLite3、zlib 与 libcurl。libcurl 不是可选项：缺失时 CMake 会以明确错误终止构建。

可选：PipeWire、PulseAudio、ALSA 输出；D-Bus（MPRIS/API）。SMB 远程源另外需要运行期的外部程序 `smbclient`（Samba 客户端）；缺少它仍可构建，SMB 连接会明确提示未安装。卡拉 OK 需要 ONNX Runtime：DEB、RPM 与 AppImage 包把它静态链接进二进制，源码构建（含 AUR 包）则以 `dlopen` 加载系统的共享库（`libonnxruntime.so.1.21`、`.so.1` 或 `.so`；只用 CPU 执行后端）；消音模型由打包构建随包内置（见[从源码构建](#从源码构建)与[卡拉 OK](#卡拉-ok)）。

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

可执行文件为 `build/ter-music`（在构建目录内为 `./ter-music`）。本地产物：1 个 DEB（Debian 10–13 / Ubuntu 20.04–26.04）、4 个 RPM（EL7–10）、1 个 AppImage（glibc 2.28）与 1 个 AUR 包；UAB/layer 本次不发布（等上游 ll-box 修复释出后再发）；DEB/RPM 的源码包与调试包为可选项。见 [BUILD_GUIDE.md](../BUILD_GUIDE.md)。

未显式传入 `-DCMAKE_INSTALL_PREFIX` 时，默认前缀是 Linyaps 布局 `/opt/apps/cn.org.huansoft.ter-music/files`，因此 `sudo make install` 会把程序装到 `/opt` 下（显式传入的前缀，例如 `/usr`，会被尊重）。发布包全部采用该布局；deb、rpm、AUR 与 AppImage 包只在 `/usr` 下添加符号链接（Linyaps 装在容器内）。见 [BUILD_GUIDE.md](../BUILD_GUIDE.md)。

消音模型不从源码编译，而是构建期注入的数据文件。打包脚本会把它下载到 `.tmp/models/`（优先 `aria2c`，失败回退 `curl`/`wget`），校验大小与 SHA-256，坏文件直接拒绝。普通源码构建请显式传入已下载的文件：

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
  -DTER_MUSIC_BUNDLED_MODEL=/path/to/UVR_MDXNET_3_9662.onnx ..
```

CMake 会校验该文件（29,704,436 字节，SHA-256 `e02220e8…aadc`），并安装为 `<前缀>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`，同时安装 `models/NOTICE.md`。不传该选项则不安装模型：播放器仍可启动，档位非“不启用”时会提示“内置模型缺失”并播放原声。模型从不在运行期下载，也绝不写入用户数据目录。

## 快速开始

```bash
ter-music                        # 启动 TUI；没有核心时自动拉起（Linyaps 内为 D-Bus 激活）
ter-music -o ~/Music             # 打开目录或文件
ter-music -o ftp://host/music/   # 在 TUI 中打开远程目录
```

TUI 需要真实终端，不要把输出重定向到管道。按 `O` 打开目录，`↑`/`↓` 选择，`Enter` 或 `Space` 播放，`q` 退出。

打开文件夹（`O`）、追加文件夹（`I`）、在 `F7` 中添加语言包，以及“设置 → 默认路径”里的默认音乐目录，都走同一个选择器。有图形会话时打开 `zenity` 或 `kdialog`（`portal` 是“优先使用桌面对话框”的偏好；ter-music 没有自己实现 D-Bus `FileChooser` 客户端）；纯 TTY 下改为手输路径——空行取消，`~/` 会展开。`TERM_MUSIC_FILE_CHOOSER=portal|zenity|kdialog|tty` 可强制后端（未知值回退自动），auto 在没有 `DISPLAY`/`WAYLAND_DISPLAY` 时回退手输。AppImage 里 AppRun 会为捆绑库导出 `LD_LIBRARY_PATH`；现在外部的对话框（以及外部的 `smbclient`）在运行前会去掉 `LD_LIBRARY_PATH`/`LD_PRELOAD`/`LD_AUDIT`，改用宿主自己的库。若对话框仍启动失败或异常退出（用户取消除外），选择器会回退到手输路径并显示原因；取消不会回退，强制指定但不可用的后端仍会明确报错。取消，或选中的不是已存在的目录，都不会改动队列与配置。

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

鼠标：左键选择并执行，滚轮滚动，右键打开上下文菜单；会拦截右键的终端可用中键。均衡器页是例外：左键只选中频带、不改增益，滚轮把选中频带的增益增减 1 dB（选中预增益时同样；开关行不响应滚轮）。菜单滑入动画每个位移只合成一帧，慢终端与流光模式下自动降级或关闭，输入无需等待动画播完；可用 `TER_MUSIC_MENU_ANIM=full|reduced|off` 强制档位（默认自动）。布局随终端尺寸自动重排。

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
| `karaoke` | 卡拉 OK 档位（off、soft、full、0-2、cycle）；档位即开关，任何档位都会被接受 |
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

`ter-music help <command>` 与 `ter-music help all` 输出完整说明。`--lang <id>` 只影响本进程（CLI 子命令或 TUI）本次运行的语言，绝不写入 `config.xml`；优先级为 `--lang`/`TERM_MUSIC_CLI_LANG` > `ui_language` > locale。

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

根治在上游：PR #295 已合入 linyaps-box `release/2.3`（merge commit `31ddf04`，2026-11-20），转发在 `TryAgain` 且 `k > 0` 时改为消费已写出的字节；影响 ll-box ≤ 2.3.5。目前还没有包含该修复的发行版（最新发行版仍是 2.3.5），`master` 也尚未带上它，因此安装前应确认所用 tag 是否包含，不要假定下一个版本一定包含。ter-music 26.4.1.0 本次不发布 UAB 与 layer；UAB 产物计划等上游修复释出后再发布。`TER_MUSIC_TTY_REPAIR_BACKSTOP_MS` 是未文档化的调参变量，只用于调节被删除的计时兜底；它已随该兜底一并移除，现在设置它不会产生任何效果。

## 播放

| 分组 | 模式 |
| --- | --- |
| 基础（5） | 顺序、单曲循环、列表循环、随机一次、随机循环 |
| 文件夹（4） | 顺序、循环、随机、随机循环 |
| 专辑（4） | 顺序、循环、随机、随机循环 |
| 艺术家（4） | 顺序、循环、随机、随机循环 |

`1`-`5` 依次选择顺序、单曲循环、列表循环、随机循环、文件夹顺序。高级模式需要曲库元数据，在“设置 → 播放模式”中启用。

倍速：0.75x、1.0x、1.25x、1.5x、2x、3x，保持音高（FFmpeg atempo）。均衡器：31 Hz–16 kHz 共 10 段，预设见“设置 → 均衡器”。

卡拉 OK 基于内置模型：分不启用 / 人声较弱 / 消除人声三档，模型在后台线程里运行。没有功能开关，也没有模型选择；见[卡拉 OK](#卡拉-ok)了解内置模型、会话分段缓存与限制。

## 卡拉 OK

卡拉 OK（消人声）使用每个安装包都会装入的 ONNX 模型。没有功能开关，没有“启用卡拉 OK”步骤，也没有模型选择：控制窗格始终有 `[卡拉OK]` 按钮，设置 → 播放设置 只有三档（不启用、人声较弱、消除人声）。旧的逐频点中置抵消 DSP 已彻底删除——ONNX 模型是唯一的消人声路径。

### 随包内置的模型

每个安装包都会把模型装为 `<前缀>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`，并同时安装 `<前缀>/share/ter-music/models/NOTICE.md`（完整 MIT 声明）。核心就地使用它：运行期不下载、不复制到用户数据目录，也没有任何配置键用于选择模型。

运行期定位顺序：

1. `<前缀>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`——随包安装，优先使用；
2. `$XDG_DATA_HOME/ter-music/models/uvr-mdxnet-3-9662.onnx`（默认 `~/.local/share/ter-music/models/…`）——对早期版本手工安装模型的只读兼容；绝不复制、绝不写入。

文件在首次打开时只做路径定位与大小检查（一次 `stat`，微秒级）；整文件 SHA-256 在首次分离前由后台 worker 计算，并按路径、大小与修改时间在进程内缓存，因此开档/切档不会被 29,704,436 字节的哈希阻塞。缺失时提示“内置模型缺失”；损坏时（大小或 SHA-256 不符）提示“内置模型校验失败”，不会静默改用另一位置。两种情况都不影响设置档位：播放继续原声，状态栏用界面语言给出原因。修复或替换文件会改变其大小或修改时间，缓存随之失效，新文件在下次使用时重新校验，无需重启播放器。

ONNX Runtime 已内置进每个发布包：DEB、四个 RPM（含 EL7）与 AppImage 都静态链接由 Debian pool 源码构建的 ONNX Runtime 1.21，系统无需安装 `libonnxruntime`，`readelf -d` 里也没有 `libonnxruntime`。静态库的符号上限为 GLIBC_2.17，同一份库覆盖 EL7–10 与 Debian 10+。不带静态库的源码构建则以 `dlopen` 加载 `libonnxruntime.so.1.21`、`.so.1` 或 `.so`；只用 CPU 执行后端。缺少运行库时分离失败并继续播原声，不会静默改用其它算法。

源码构建只有传入 `-DTER_MUSIC_BUNDLED_MODEL=<文件>` 才会包含模型；不传则不安装模型，运行期会提示缺失（见[从源码构建](#从源码构建)）。发布包始终内置该模型。

### 支持的模型

只支持一个模型：**UVR-MDX-NET 3**。

| | |
| --- | --- |
| 显示名 | UVR-MDX-NET 3 |
| 安装文件名 | `uvr-mdxnet-3-9662.onnx`（上游文件名 `UVR_MDXNET_3_9662.onnx`） |
| 官方 URL | <https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/UVR_MDXNET_3_9662.onnx> |
| 大小 | 29,704,436 字节 |
| SHA-256 | `e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc` |
| 许可依据 | MIT，依据 Ultimate Vocal Remover GUI（UVR）项目 README：第三方使用其模型须署名 UVR 及其开发者；`TRvlvr/model_repo` 仓库本身没有独立的 LICENSE 文件 |

模型要求的署名为：“Vocal separation model: UVR-MDX-NET 3, from Ultimate Vocal Remover GUI (UVR) by Anjok07, TRvlvr and contributors — MIT, used with credit.” ter-music 把未经改动的模型作为数据文件与本 GPL 应用一同分发，完整声明见 `models/NOTICE.md`；文件本身仍受 UVR 项目声明的 MIT 条款约束。

### 三档与播放来源

| 档位 | 播放来源 |
| --- | --- |
| 不启用 | 原曲 |
| 人声较弱（soft） | 0.4 × 原曲混音 + 0.6 × 模型伴奏 |
| 消除人声（full） | 仅模型伴奏 |

单声道曲目不分离，继续按原声播放。档位保存在 `karaoke_mode`，可经控制栏弹层、设置 → 播放设置、`ter-music karaoke` 或 `Control.SetKaraoke` 修改；D-Bus 与 CLI 没有“功能未启用”的前置条件——0–2 一律接受（越界回 `false`），`GetKaraoke` 恒回当前档位。

### 缓存与后台分离

分离在播放核心的低优先级后台线程（nice 10）中进行，绝不在 UI 或播放线程里跑模型。档位非“不启用”时，30 分钟以内的立体声曲目若没有完整旧版缓存，会在当前会话里分离；全部段就绪后，同一会话内重播直接复用结果，不再跑模型；分离失败则下次起播重试。档位为“不启用”时不分离——档位即开关。播放中切到其它档位会立即为当前曲目请求分离，并在目标段就绪后自动按当前位置切换音源；位置保持连续，切换瞬间允许短暂停顿。

- **会话分段缓存：** 伴奏写在 `/tmp/ter-music-karaoke-<uid>-<pid>/<模型 id>/<曲目哈希>/`（目录权限 0700）下，按 15 秒一段保存为 `segNNN.inst.flac`（full 档）与 `segNNN.soft.flac`（soft 档），每段最后写入 `segNNN.ready` 就绪标记，另有 `manifest.txt` 清单。一次分离同时产出两份 stem，两个档位共用这一次推理。
- **生命周期：** 会话目录只在播放进程存活期间存在。正常退出时先停 worker，再删除整个目录；播放核心启动时会清理 `/tmp/ter-music-karaoke-<uid>-*` 中进程已不存在的残留目录（崩溃遗留）。这些分段不会跨重启保留，因此下次会话会重新分离该曲（除非命中旧版整曲缓存）。
- **键：** 曲目真实路径、大小与修改时间，加上模型 id、模型 SHA-256、格式版本、stem 版本、overlap 与 soft 混合系数。移动文件或重新编码会得到新的键。
- **旧版整曲缓存（只读，优先使用）：** 早期版本写在 `$XDG_CACHE_HOME/ter-music/karaoke/<模型 id>/<曲目哈希>/`（默认 `~/.cache/ter-music/karaoke/…`）下的完整 `inst.flac` + `soft.flac` + `meta.txt` 三元组仍会被读取。命中时直接从它起播，不写任何会话分段。缓存键包含当前 stem 版本，因此 stem 派生算法不同的早期构建所写缓存不会命中；这些目录原样保留，该曲改为在会话缓存里重新分离。启动时会清理该目录下残留的 `.part` 半成品；2048 MB 的 LRU 设置只针对这些整曲缓存。会话分段另有预算：默认每个会话 512 MB（`TER_MUSIC_KARAOKE_SEGMENT_MB` 可覆盖，`0`（或更小）关闭预算），超出时按目录 mtime 最旧优先淘汰；正在播放或正在分离的曲目受保护，且每次写段前做剩余空间检查（保留 32 MiB）。
- **未缓存曲目在非“不启用”档位的首次起播：** 播放会先等待首段伴奏就绪再起播——测试机上约 7–10 秒，硬上限 30 秒——状态栏显示准备进度。若首段没有等到（超时，或模型/缓存不可用等 worker 失败），则改为播放原声；单声道与超过 30 分钟不经等待、直接播原声；CUE 子曲目也不经等待，播原声或命中的旧版整曲缓存。若起播位置不为 0（恢复播放、改倍速），等待上限为 8 秒。
- **档位为“不启用”时**播原声，不做任何分离。播放中选择档位会为当前曲目开始分离，并在当前段就绪后自动切换；状态栏先显示等待提示，切换完成后显示“已切换到…（等待 N 秒）”。
- **播放过程中：** 下一段未就绪时，在段边界最多等待 8 秒，然后从该处起以原声播到曲终（只切换一次，不会曲中反复跳源）。seek 到未处理位置时会先请 worker 优先处理目标段并最多等待 8 秒；仍未就绪则从 seek 位置起以原声继续。worker 到达曲尾后会回填被 seek 跳过的段，只有全部段就绪才把清单标为完成。
- **切换档位**时，会为当前曲目请求分离，并在目标段就绪后自动按当前位置切换音源——测试机上首次切换约 9–15 秒；在晚位置（例如播放到 120 秒）启用约 9 秒，因为 worker 会被要求优先处理当前段。已就绪段上的 Soft ↔ Full 互切是即时的（不到 1 秒），不会重跑模型。切换瞬间允许短暂停顿，位置保持连续；暂停中切档同样生效（在暂停位置换源，仍保持暂停）。若分离失败，或 30 秒内没有任何段就绪，则继续播原声，状态栏给出原因，可再次切档重试。恢复有界：段边界回退原声后，每次切档最多重新排队恢复 3 次；因此极端慢机可能一直保持原声——此时状态栏会说明仍在原声、可再次切档重试，绝不会谎报已经切换。
- **预取：** 仅在 `seamless_preload=1` 时，当前曲目伴奏就绪后（且档位不是“不启用”），才会在后台分离队列下一曲；`seamless_preload=0` 不预取。CUE 子曲目从不预取。
- 分离固定使用 0.25 的 overlap；超过 30 分钟的曲目拒绝分离（状态栏提示失败并继续原声）。CUE 子曲目从不使用会话分段源：播放原声，或命中同一物理文件的旧版整曲缓存；因此在 CUE 子曲目上切档时，若没有匹配的旧版整曲对，会保持原声且不重启播放；命中旧版整曲对时仍立即换源；两种情况都不会谎报切换。内置模型文件属于安装内容；早期版本留下的整曲缓存会保留，要回收空间可在退出播放器后删除 `~/.cache/ter-music/karaoke/`（或某个模型的子目录）——`/tmp` 会话目录在退出时已删除。

### 与歌词无关

不依赖歌词，也没有歌词门控：带时间轴 LRC、纯文本歌词、内嵌歌词或完全没有歌词的曲目走完全相同的模型路径。

### 限制

- UVR-MDX-NET 3 是中低尺寸模型，不是完美分离：伴奏里可能残留人声，与人声相近的乐器或和声也可能被削弱。`soft` 档刻意保留 40% 原曲，是折中方案，不是干净的纯伴奏。
- 分离吃 CPU，成功时每曲每会话只跑一次模型：耗时取决于模型、机器与曲目长度；在 6 核 Ryzen 5 4500U 上，30 秒曲目端到端约 11 秒（每个约 6 秒的块约 1.1–1.5 秒），3 分钟曲目约 1 分钟；更弱的机器更慢。worker 的 ONNX 会话使核心峰值内存约 0.8 GiB（实测 795 MiB），分离结束后回落。
- 会话分段约占每分钟音频 15.5 MB（在 44.1 kHz 立体声曲目上实测；3 分钟曲目约 46 MB）。这比旧版整曲对（实测约 12 MB/分钟）高约 30%，因为每个 15 秒段都带独立的 FLAC 帧头与 ready/manifest 文件。播放器退出时该目录即被删除；运行期间上述会话预算会限制其总大小。Fedora/RHEL 默认把 `/tmp` 挂为 tmpfs，此时预算占用的是内存，空间紧张时请调低 `TER_MUSIC_KARAOKE_SEGMENT_MB`。
- 单声道曲目、超过 30 分钟的曲目不分离、按原声播放；CUE 子曲目不使用会话分段源。分离失败不会中断播放，状态栏会用界面语言给出原因（ONNX Runtime 自身打印的消息仍是英文技术串）。
- 只验证了 ONNX Runtime 1.21 的 CPU 执行后端；其它版本可能能加载，但不保证。DNNL 等其它执行后端刻意不启用。
- 档位非“不启用”时若 ONNX Runtime 或已安装模型变得不可用，分离会失败并继续播原声；界面不会静默改用其它算法。发布包已把运行库装进二进制；不带静态库的源码构建需要系统的 `libonnxruntime`。
- “karaoke”在本项目里还指一个布局预设和逐字歌词高亮，二者都与本功能无关。

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

布局编辑器在编辑期执行非破坏性的最小可读尺寸护栏：每次比例调整、分栏、更换窗格类型与交换都会按当前终端的实际单元格检查（一般窗格约 12×3；歌词 12×2、频谱 8×2、黑胶 8×3），低于下限的改动会吸附到最近的可读比例（无合法值时保持当前布局）并提示。该限制只作用于编辑器——已保存的更小布局仍照旧加载与渲染。

### info 窗格

info 窗格按实际可用尺寸排布。曲目信息块与音频格式块按各自内容宽度分列（每列至少 20 列）；某块没有行时把宽度让给另一块。长值以 `…` 结尾省略；窗格高度不足时按重要度保留行——元数据为状态、标题、艺术家、模式、专辑/队列，音频为采样率、位深、比特率、编码——显示顺序保持不变。封面保持 2:1，可缩到 3 行（最多 12 行）；在 设置 → 信息显示 → **封面/文字优先级** 中选择 自动（默认）、文字优先或封面优先。宽而矮的窗格会把一个块的行铺到最多 3 个子列。

### 终端亮/暗自适应

`F2` → 界面外观 → **默认外观模式** 可选 `自动`（默认）、`亮色`、`暗色`；选择存于 `config.xml` 的 `preferences.appearance_mode`（契约版本 10）。`自动` 模式在首帧前先向终端查询背景色（OSC 11），查不到再依次尝试终端自身主题（当前为 deepin-terminal 适配器，只读，最多每 2 秒检查一次文件变化）、XDG 系统外观、`COLORFGBG`、`TERM` 提示（名含 `light`/`dark`），最后保持原有暗色外观——不应答的终端/portal 观感与旧版一致（OSC 11 探测上限约 200ms、portal 读取上限约 120ms，且仅在 `自动` 模式发生）。终端应答会从按键流中移除、绝不进入界面；只有“本程序刚发过查询、仍在等待应答”的窗口内收到的完整应答（BEL 或 ST 终止）才会被当作应答，手工输入同样的字符仍会正常送达（该窗口内最多有界 80ms 的预读）。 `自动` 模式的探测优先级为 OSC 11（终端实际底色，权威）→ 终端主题适配器（当前仅 deepin-terminal，读取其自身配置/配色，优先于桌面偏好）→ XDG 桌面 portal 的 `org.freedesktop.appearance color-scheme`（系统偏好，通过 `SettingChanged` 运行期跟随）→ `COLORFGBG` → `TERM` 提示 → 原有暗色回退；设置页会显示探测来源。终端主题适配器只在 deepin-terminal 下启用，其它终端不会读取该配置、行为保持不变。身份识别优先使用 `TERM_PROGRAM`；仅当其未设置时才查看祖先进程链。前端本地 `~/.config/ter-music/appearance-terminals.json` 可按终端覆盖全局 `preferences.appearance_mode`（全局键仍是默认值，也是 `config.xml` 里唯一持久化的外观键）。内置预设直接适配；用户 `layouts.json` 中与内置预设同名的布局，只有前景色仍等于该预设出厂暗色值时才参与亮色替换——用户改过的值保留，`TER_MUSIC_LAYOUTS` 始终逐字生效。

亮色背景下，出厂主题色与全部 7 个内置预设切换到亮色安全配色，预设内各窗格颜色在 256/16/8 色终端上仍互不相同；暗色配色完全不变。只替换出厂值：你改过的主题色会保留。`~/.config/ter-music/layouts.json` 中的用户布局不会被改写；若其 id 与内置预设同名，且某窗格前景仍等于该预设出厂暗色值，则该前景随亮色变体。因此用户预设副本中未改的出厂值会继续适配（布局编辑器显示存储值，Settings 显示生效值）；你改过的值与 `TER_MUSIC_LAYOUTS` 原样使用。在布局编辑器中保存内置预设会先把当前屏幕上的颜色物化并固定。

终端支持焦点上报时（`自动` 模式启用 `\e[?1004h` 且 terminfo 定义了焦点键），离开再回到窗口会自动重探，无需重启；不支持的终端可用同页的 **重新探测终端背景**。设置页的颜色页会为亮色变体索引显示名称（炭灰、深青、暗琥珀……），出厂默认色显示为“自动（亮/暗）”。探测到的变体属于各前端进程，绝不写入 `config.xml`；外观模式属于共享配置，运行中的 TUI 经既有 `ConfigChanged` 重载路径生效。

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

卡拉 OK 模型随安装包装在 `<前缀>/share/ter-music/models/`（`uvr-mdxnet-3-9662.onnx` 与 `NOTICE.md`），就地使用。`$XDG_DATA_HOME/ter-music/models/uvr-mdxnet-3-9662.onnx`（默认 `~/.local/share/ter-music/models/…`）只是对早期版本手工安装模型的只读回退，绝不写入。旧版整曲伴奏缓存（若存在）位于 `$XDG_CACHE_HOME/ter-music/karaoke/<模型 id>/<曲目哈希>/`（默认 `~/.cache/ter-music/karaoke/`），只读，2048 MB 的 LRU 设置只覆盖它们。会话分段只写 `/tmp/ter-music-karaoke-<uid>-<pid>/`（默认会话预算 512 MB，`TER_MUSIC_KARAOKE_SEGMENT_MB`），进程退出即删除；见[卡拉 OK](#卡拉-ok)。

常用 `config.xml` 键：

| 键 | 取值 |
| --- | --- |
| `audio_backend` | 0 自动，1 PulseAudio，2 ALSA，3 PipeWire |
| `audio_latency_ms` | 输出延迟，20-250 |
| `volume_percent` | 默认音量，0-100 |
| `default_playback_speed` | 0.75-3.0 |
| `default_play_mode` | 0-16 |
| `ui_language` | `zh_CN`、`en_US`、`zh-PQ` 或已安装的语言包 ID |
| `karaoke_mode` | 0 不启用，1 人声较弱，2 消除人声；默认 0；档位即开关 |
| `appearance_mode` | 0 自动（跟随终端），1 亮色，2 暗色 |
| `resume_last_playback` | 恢复上次曲目与位置 |
| `stop_core_on_exit` | 1 退出时停止核心，0 继续播放 |
| `core_exit_when_no_frontend` | 最后一个前端离开 10 秒后退出核心 |
| `info_cover_priority` | 0 自动，1 文字优先，2 封面优先；默认 0；不提升配置契约版本 |

配置契约版本为 **12**。版本 12 **删除**了 `preferences.karaoke_feature` 与 `preferences.karaoke_model`：模型随包内置，不再有功能开关与模型选择。以仓库实际版本史为准：契约 4 = 标签 `v2.1.0`（根属性曾是字面量 `"2.2"`，因按整数解析为 2 仍可加载，并由迁移改写一次），7 = 常驻核心，8 = `stop_core_on_exit`，9 = `<keybindings>` 覆盖表**与** `karaoke_mode`（DSP 时代的档位），10 = `preferences.appearance_mode`，12 = 内置 ONNX 模型并删除 DSP 时代的卡拉 OK 键。仓库从未发布过契约 11 的构建，故已发布文件中不可能含这两个被删键。v10 或更早的 `config.xml` 会在新核心首次启动时迁移：文件以 `version="12"` 整份重写，其余键（包括 `karaoke_mode`、主题、路径、均衡器与按键绑定）原样保留；比 v10 更旧的文件还会补上更早迁移的一次性默认值（v4 以下背景 0→-1，v7 以下 `core_exit_when_no_frontend=0`，v8 以下 `stop_core_on_exit=1`，v10 以下 `appearance_mode=auto`）；第二次启动时文件字节级不变。`config get karaoke_feature` 与 `config set karaoke_feature …` 现在按未知键失败，`Config.Set` 会整体拒绝仍含这两个键的补丁。不支持降级：v12 之前的（契约 10）构建读取 v12 文件时（版本属性按整数解析，其自身的迁移块不命中）把被删键视为缺失——卡拉 OK 为不启用——并在下次保存时把版本属性写回 10。升级瞬间若新前端遇到仍在运行的 v12 之前核心，配置镜像按已知键回填（唯一的握手门是 `api_version` 7）；只有载荷仍带被删键时才会整体拒绝，而那只会来自未发布的契约 11 开发构建——重启核心（`ter-music daemon restart`）或重新登录即可自愈。

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

`Info.GetInfo` 返回 `core.api_version`（当前 7）。详见 [API_DBUS_en_US.md](../API_DBUS_en_US.md) 与 [API_LYRICS_en_US.md](../API_LYRICS_en_US.md)。`Control.SetKaraoke` 无条件接受 0–2 档（档位即开关，越界回 `false`），`Control.GetKaraoke` 恒回当前档位，`ter-music karaoke` 读取或修改它——见[卡拉 OK](#卡拉-ok)。

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

CI 对每次改动运行确定性门禁，包括下列各项；真实模型的全量卡拉 OK e2e 在独立的 advisory 作业中运行、不阻断合并，发布机与 nightly 才是最终质量判定（见 [BUILD_GUIDE.md](../BUILD_GUIDE.md) §9.5）：

| 门禁 | 规则 |
| --- | --- |
| `scripts/test/check-core-purity.sh` | 核心不含内容或远程源符号 |
| `scripts/test/check-ui-purity.sh` | UI 只能通过 `player` 门面访问播放 |
| `scripts/test/check-config-ownership.sh` | 核心是 `config.xml` 的唯一写入者 |
| `scripts/test/check-package-identity.sh` | 各打包位置的应用 ID 与版本一致 |
| `scripts/test/check-c-strings.sh` | C 代码不得硬编码中文，文案走 i18n key |
| `scripts/test/package-layout-check.sh`（`TM_LAYOUT_STATIC_ONLY=1`） | `/opt/apps` 前缀、`/usr` 链接、模型常量与接线，不构建软件包 |
| no-D-Bus 构建（无 D-Bus） | 可选 D-Bus 的构建可编译、链接并启动 |

## 故障排查

- **没有声音：** 确认 PipeWire、PulseAudio 或 ALSA 正在运行；后端按此顺序探测。显式选择的后端在播放失败时会回退到下一个可用后端。可在“设置 → 音频”中切换。
- **卡顿或爆音：** 在设置中调高输出延迟（20、40、60、80、100、120、150、200 或 250 ms）。
- **中文乱码：** 使用 UTF-8 locale 和 UTF-8 终端；`locale` 应显示 `LC_CTYPE=UTF-8`。
- **AppImage 里不弹文件对话框：** 打包的对话框起不来时会自动回退到手输路径；若仍看不到，检查终端日志（`GUI dialog failed` / `falling back to TTY`），在宿主机安装 `zenity` 或 `kdialog`，或用 `TERM_MUSIC_FILE_CHOOSER=zenity|kdialog|tty` 强制后端。
- **Linyaps 里 TUI 出现重复或错乱的行：** 即上文 Linyaps 一节所述 ll-box ≤ 2.3.5 冷启动重放。守卫在检测到写入突发或背压证据时会整屏重绘；没有证据时不再按计时器重绘，损坏可能要等到终端缩放、切换视图或出现新证据才被覆盖。
- **编译时找不到头文件：** 安装上面依赖的 `-devel`/`-dev` 包。
- **远程目录列不出来：** 检查协议、主机、端口与基础路径（SMB 要填 `共享名/子路径`）；SMB 需要安装 `smbclient`；SFTP 需要可读的 known_hosts；https/webdavs/ftps 需要打开 TLS。页面会显示本地化原因，连接动作菜单里有“重试列目录”。
- **远程曲目下载失败：** 页面显示 `失败 n` 与首条错误；排除原因（网络、权限、磁盘空间）后使用“重试失败曲目(n)”，已缓存的曲目不会重复下载。单文件超过 1 GiB 或缓存已满会在该曲停止；可在远程设备页提高上限或清理缓存。
- **远程目录下载到一半停了：** 单次打开最多下载 200 首；请进入子目录分别打开。
- **从目录历史重新打开远程 URL 时认证失败：** 入库的 URL 已脱敏（设计上不含密码）；需要密码的服务器请使用远程设备页里保存的连接。
- **CUE 分轨不显示：** `.cue` 必须与音频文件同名；文字乱码时在设置中改 CUE 编码。
- **曲库为空：** 按 `M` 打开曲库浏览器并扫描或重新扫描；数据库是配置目录下的 `library.db`。
- **状态栏提示内置模型不可用：** 安装包应已把模型装到 `<前缀>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`。源码构建未传 `-DTER_MUSIC_BUNDLED_MODEL` 时不安装模型；文件损坏则大小/SHA-256 校验不通过。发布包已把 ONNX Runtime 装进二进制；源码构建还需要系统库（`libonnxruntime.so.1.21`、`.so.1` 或 `.so`）。状态栏会用界面语言说明原因（缺失、校验失败、运行库不可用），档位仍可设置，播放继续原声。路径在打开时做大小检查，整文件 SHA-256 在后台校验；修复或替换文件会让缓存失效，无需重启播放器（使用系统库的源码构建须已安装该库）。不会回退到任何旧的内置算法。
- **首次起播会等待，或回退原声：** 档位非“不启用”且曲目没有缓存时，会先等首段伴奏就绪（约 7–10 秒，总计最多 30 秒；起播位置不为 0 时最多 8 秒），状态栏显示准备进度。等待超时或分离失败（模型/缓存不可用）则按原声播放；单声道、超过 30 分钟与 CUE 子曲目不经等待、直接播原声（CUE 命中旧版整曲缓存时仍用缓存）。
- **卡拉 OK 分离总是失败：** 超过 30 分钟的曲目会拒绝分离，单声道曲目与 CUE 子曲目不分离，内置模型缺失或损坏、或缺少 ONNX Runtime 也会失败；状态栏会说明原因。见[卡拉 OK](#卡拉-ok)。

## 许可

GPL-3.0-only，见 [LICENSE](../../LICENSE)。

随包内置的 UVR-MDX-NET 3 模型以 MIT 分发（见 [models/NOTICE.md](../../resources/models/NOTICE.md)）；发布二进制静态链接以 LGPL-2.1-or-later 发布的 FFmpeg 构建（未启用 `--enable-gpl`），官方包还静态链接以 MIT 发布的 ONNX Runtime；完整声明与源码获取方式见 `debian/copyright`。

Ter-Music 只是播放工具，不提供、不托管、不分发音频内容；请自行确保所播放文件的合法性。

## 作者

浣软科技（HuanSoft）· <yxzl666xx@outlook.com>

## 致谢

- **@guanzi008**：Debian 打包、MPRIS、UTF-8 输入、鼠标与界面修复。
- **@Zeta**：Arch Linux 打包。
- **Ultimate Vocal Remover GUI（UVR）** 及其作者 Anjok07、TRvlvr 与贡献者：ter-music 把未经改动的 UVR-MDX-NET 3 人声分离模型作为数据文件随包分发，按[卡拉 OK](#卡拉-ok)所述的 MIT 条款署名使用。
