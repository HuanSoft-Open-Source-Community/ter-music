<div align="center">

# Ter-Music

**麟纳克斯端闱乐部**

![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
[![CI](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml/badge.svg)](https://github.com/HuanSoft-Open-Source-Community/ter-music/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/HuanSoft-Open-Source-Community/ter-music?sort=semver)](https://github.com/HuanSoft-Open-Source-Community/ter-music/releases)
![Linyaps](https://img.shields.io/badge/Linyaps-承应-8A2BE2.svg)

</div>

**他语之版：** [English](../README.md) · [中文（现代版）](README_zh-CN_Modern.md)

**典册：** [营构与封装](../BUILD_GUIDE.md) · [TUI 局度](../UI_LAYOUTS_en_US.md) · [D-Bus API](../API_DBUS_en_US.md) · [歌辞 API](../API_LYRICS_en_US.md) · [发布之程](../RELEASE.md)

## 一、本志叙略

Ter-Music者，端闱之乐部也。以斐氏FFmpeg解声，纽氏ncursesw绘界，运行时择PipeWire、PulseAudio或ALSA而用之。内置SQLite曲库；编译时通D-Bus者，兼有MPRIS与D-Bus控制之接口。

其体分二层：**核心**主音声之器、传输之令与前端所授之路径队列；**前端**（TUI或CLI）主文件、曲库、曲帙、往迹与远程之下载。无核心时，启TUI则自起其一。

## 二、功用

**播弄**
- MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、WavPack之属；FLAC/APE/WV通CUE分轨，自识GBK、BIG5、Shift-JIS之编码。
- 十七种播弄之制（序进、回环、乱序，及文件夹、专辑、艺术家之域），六档迅疾（0.75x–3x），1.0x时预载次曲，接续无间。
- 十段ISO均衡器（31 Hz–16 kHz，±12 dB），有预设与软限幅。
- 卡拉OK（汰聲）：勿汰、微汰、盡汰三档，限频带而削居中者；惟兩聲並列者行之。
- 内嵌或外之LRC歌辞；时签可依者，逐字昭焕。

**内容**
- SQLite曲库，FTS5搜检、拼音匹配、递归扫描；按艺术家、专辑、流派览之。
- 播弄队列、自建曲帙、珍存、播弄往迹、目录往迹。
- SMB、SFTP、FTP、WebDAV、HTTP之远源；前端逐曲下载于本地之藏。

**文界**
- 六格（曲帙、信息、控御、频谱、歌辞、黑胶），七种JSON局度预设，强制平铺。
- 二十四套调色之谱，加一自定槽位；每格可别立前景、背景之色。
- 专辑封面以彩色半块绘之；黑胶格有旋转之封面圆标。
- 键鼠并用；内置`zh_CN`、`en_US`、`zh-PQ`三语。

**通合**
- CLI与离脱端闱之后台播弄役使，可于任意端闱御之。
- 编译时通D-Bus者，有MPRIS及Info、Control、Queue、Config、Lyrics诸接口。
- 承应Linyaps封装。

## 三、纳置

### Arch Linux

```bash
yay -S cn.org.huansoft.ter-music
# 或
paru -S cn.org.huansoft.ter-music
```

### 由源码营构

必需：CMake 3.10+、C99之译器、pkg-config、ncursesw、FFmpeg诸库（avcodec、avformat、avutil、avfilter、swresample、swscale）、libpng、libjpeg、libxml2、SQLite3、zlib。

可选：PipeWire、PulseAudio、ALSA之出；D-Bus（MPRIS/API）；libcurl（远源）。

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

**营构**

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
sudo make install   # 可择
```

可执行在`build/ter-music`（于营构之舍则`./ter-music`）。DEB、RPM、AppImage、可移植之包与Linyaps封装，见 [BUILD_GUIDE.md](../BUILD_GUIDE.md)。

## 四、速上手

```bash
ter-music                  # 启TUI；无核心时自起其一
ter-music -o ~/Music       # 开目录或文件
ter-music ftp://user:pass@host/music
```

文界须真端闱，勿以管道引其出。按`O`开目录，`↑`/`↓`择曲，`Enter`或`Space`播之，`q`退。

| 键 | 事 |
| --- | --- |
| `O` / `o` | 开目录 |
| `↑` `↓` / `j` `k` | 择曲 |
| `Enter` / `Space` | 播或开 |
| `n` / `p` | 次曲 / 前曲 |
| `,` / `.` | 退 / 进五秒 |
| `-` / `=` | 音减 / 音增 |
| `C` / `L` | 焦点在控御 / 曲帙 |
| `Tab` | 文件 / 播弄队列 |
| `Ctrl+L` | 歌辞定位 |
| `M` | 曲库 |
| `/` 或 `S` | 搜检（拼音） |
| `a` / `i` | 附 / 次 |
| `I` | 附目录 |
| `d` / `D` | 除 / 清空 |
| `F` / `f` | 珍存 |
| `A` | 入曲帙 |
| `h` | 播弄往迹 |
| `R` | 重起核心 |
| `F1`-`F9` | 诸视与退 |
| `q` | 退 |

全键见`F8`与`ter-music keys list`；于设置→快捷键改键。`F3`为目录往迹，`h`为播弄往迹之弹层。

鼠：左键择而行之，轮滚，右键开上下文之菜单；截右键之端闱可用中键。局度随端闱之尺寸自适。

## 五、符令

诸子命令皆运行中核心之D-Bus客户端；`play`亦扫其径而下队列。

| 令 | 用 |
| --- | --- |
| `play [PATH]` | 扫径而播 |
| `pause` `resume` `toggle` `stop` | 传输 |
| `next` `prev` | 易曲 |
| `seek` | 跳转：+秒、-秒、mm:ss或百分比 |
| `volume` | 查询或设音量（0-100、+N、-N） |
| `speed` | 查询或设迅疾（0.5-3.0） |
| `mode` | 查询或设播弄之制（名或0-16） |
| `karaoke` | 汰聲之档（off、soft、full、0-2、cycle） |
| `queue` | 治队列（list、clear、shuffle、remove、append、insert） |
| `config` | 读改节度（get、set、reload、reset） |
| `show` | 出信息之块 |
| `lyrics` `track` `cover` | 当前歌辞、元数据或字符封面 |
| `keys` | 审键绑（list、check） |
| `daemon` | 治核心（start、foreground、stop、restart、status、reload） |
| `tui` `version` `help` | TUI、版、助 |

```bash
ter-music play ~/Music
ter-music show --one-line --watch=1000
ter-music show --json | jq -r '.track.title'
ter-music daemon start --open ~/Music
ter-music daemon stop
```

退出之码：`0`成，`1`用法误，`3`无运行之核心，`4`D-Bus不可用，`5`为核心所拒。

`ter-music help <command>`与`ter-music help all`出全说。`--lang <id>`惟易本次CLI之语。

**Linyaps。** 包中之可执行在容器内：

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music show
ll-cli run cn.org.huansoft.ter-music -- ter-music play ~/Music
```

后台播弄于容器内改用D-Bus激活或随包之systemd用户役使；亦可在`ll-cli run`中加`--foreground`；于宿主以`systemctl --user enable --now cn.org.huansoft.ter-music`启其常驻之役。容器既行时，以`ll-cli enter`或`journalctl --user -u cn.org.huansoft.ter-music`读其出。

## 六、播弄

| 组 | 制 |
| --- | --- |
| 基础（5） | 序进、单曲回环、全帙周流、乱序一度、乱序杂陈 |
| 文件夹（4） | 序进、回环、乱序、乱序回环 |
| 专辑（4） | 序进、回环、乱序、乱序回环 |
| 艺术家（4） | 序进、回环、乱序、乱序回环 |

`1`-`5`次第择序进、单曲回环、全帙周流、乱序杂陈、文件夹序进。进阶之制需曲库元数据，于设置→播弄之制启之。

迅疾：0.75x、1.0x、1.25x、1.5x、2x、3x，存其音高（FFmpeg atempo）。均衡器：31 Hz–16 kHz共十段，预设见设置→均衡器。汰聲：勿汰、微汰、盡汰三档，惟兩聲並列；可于控御之弹层、设置→播弄、`ter-music karaoke`或`Control.SetKaraoke`改之。

## 七、歌辞与封面

- 先取内嵌歌辞（FFmpeg/APE之签），无则取同名之`.lrc`。
- LRC有逐字、行首尾或插值之时签者，按字昭焕。
- `Ctrl+L`入歌辞定位；`Tab`易内嵌与外之来源。
- 封面先取内嵌之图，次取同目录之`cover`/`folder`/`front`/`album`。
- 色自二百五十六而降于八、终归黑白；`ter-music cover`与`Info.GetCoverArt`出点阵、ASCII或半块之封面。

## 八、局度与色采

局度者，二叉分栏之树也。其叶为六格之一，故主区常强制平铺；端闱修广之变，按千分比复算。预设：`classic`、`vinyl`、`karaoke`、`visualizer`、`focus`、`stage`、`neon`。

于`F2`→界面外观→界面布局择而编之。其文：

- 内置：`<prefix>/share/ter-music/layouts/layouts.json`
- 用户覆盖：`~/.config/ter-music/layouts.json`
- 开发覆盖：`TER_MUSIC_LAYOUTS=/path/to/layouts.json`

二十四套调色之谱加一自定槽位，主侧栏、高亮、边框；每格亦可别立前景/背景。全式见 [UI_LAYOUTS_en_US.md](../UI_LAYOUTS_en_US.md)。

## 九、节度与数据

`$XDG_CONFIG_HOME/ter-music`（默`~/.config/ter-music`）：

```text
config.xml     核心节度
library.db     SQLite曲库：曲目、珍存、往迹、曲帙、目录往迹
remote.xml     远服（惟前端）
layouts.json   用户局度之覆
ui-layout      今局度之id
lang/          用户语言包
help/          用户速上之文
```

远曲之藏于`$XDG_CACHE_HOME/ter-music/remote/`；封面之藏于`/tmp/ter-music-cover-*.jpg`，退时清之。

常用`config.xml`之键：

| 键 | 值 |
| --- | --- |
| `audio_backend` | 0自择，1 PulseAudio，2 ALSA，3 PipeWire |
| `audio_latency_ms` | 输出之迟，20-250 |
| `volume_percent` | 默音量，0-100 |
| `default_playback_speed` | 0.75-3.0 |
| `default_play_mode` | 0-16 |
| `ui_language` | `zh_CN`、`en_US`、`zh-PQ`或已装之语言包ID |
| `karaoke_mode` | 0勿汰，1微汰，2盡汰 |
| `resume_last_playback` | 复上次之曲与位 |
| `stop_core_on_exit` | 1退时止核心，0续播 |
| `core_exit_when_no_frontend` | 末前端去后十秒核心自退 |

惟改过之键乃书于`config.xml`：

```xml
<keybindings>
  <binding action="track.next" key="]"/>
  <binding action="track.prev" key="["/>
</keybindings>
```

保留之键：`F1`-`F9`、`q`/`Q`、`Tab`/`Shift+Tab`。动作ID以`ter-music keys list`查，文以`ter-music keys check`校。

语言包为XML之文（`<lang id="..." name="...">`，内含`<string key="...">`），置于`~/.config/ter-music/lang/`，或于`F7`语言之视以`A`装`.tar.gz`、以`D`删之。内置：`zh_CN`、`en_US`、`zh-PQ`。

## 十、通合

编译时通D-Bus者，MPRIS以`org.mpris.MediaPlayer2.ter_music`布于`/org/mpris/MediaPlayer2`。同对象亦具：

| 接口 | 用 |
| --- | --- |
| `cn.org.huansoft.ter_music.Info` | 曲目、进度、封面、歌辞行、信息块 |
| `cn.org.huansoft.ter_music.Control` | 传输、跳转、音量、迅疾、制、汰聲、退 |
| `cn.org.huansoft.ter_music.Queue` | 核心之本地路径队列 |
| `cn.org.huansoft.ter_music.Config` | 节度读写 |
| `cn.org.huansoft.ter_music.Lyrics` | 歌辞行与逐字之时 |

`Info.GetInfo`出`core.api_version`（今七）。详见 [API_DBUS_en_US.md](../API_DBUS_en_US.md) 与 [API_LYRICS_en_US.md](../API_LYRICS_en_US.md)。

## 十一、营缮

| 层 | 职 |
| --- | --- |
| 核心（`daemon`） | 音声之器、解码、传输、路径队列、当前曲目、节度 |
| 前端（TUI/CLI） | 曲库、扫描、曲帙、珍存、往迹、远源、文界 |

源码在`src/cn.org.huansoft.ter-music/<module>/`；公共头在`include/cn.org.huansoft.ter-music/<module>/`。

```bash
mkdir -p build/debug
cd build/debug
cmake -DCMAKE_BUILD_TYPE=Debug ../..
make -j"$(nproc)"
cd ../..
./scripts/test/run-unit-tests.sh
```

CI行四门：

| 门 | 规 |
| --- | --- |
| `scripts/test/check-core-purity.sh` | 核心无内容与远源之符号 |
| `scripts/test/check-ui-purity.sh` | UI惟经`player`门面而及播弄 |
| `scripts/test/check-config-ownership.sh` | 核心为`config.xml`之独书者 |
| `scripts/test/check-package-identity.sh` | 诸封装之应用ID与版相合 |

## 十二、祛疑

- **无声：** 察PipeWire、PulseAudio或ALSA之行；后端依此序探之。显择之后端播而不成，则退就次可用者。可于设置→音声易之。
- **滞或爆音：** 于设置增高输出之迟（20-250 ms，每次加十毫秒）。
- **华文乱码：** 用UTF-8之locale与UTF-8之端闱；`locale`宜见`LC_CTYPE=UTF-8`。
- **营构时无头文：** 装上所依之`-devel`/`-dev`包。
- **CUE分轨不显：** `.cue`须与音声同名；文乱则于设置改CUE之编码。
- **曲库空：** 按`M`开曲库而扫之或复扫；库为配置目录下之`library.db`。

## 十三、律例

GPL-3.0-only，见 [LICENSE](../../LICENSE)。

Ter-Music惟播弄之器，不供、不藏、不布音声之文；所播之文，君自备之，当合律令。

## 十四、撰者

浣软科技（HuanSoft）· <yxzl666xx@outlook.com>

## 十五、鸣谢

- **@guanzi008**：Debian之封装、MPRIS、UTF-8之入、鼠与文界之修。
- **@Zeta**：Arch Linux之封装。
