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

**典册：** [营构与封装](../BUILD_GUIDE.md) · [TUI 局度](../UI_LAYOUTS_en_US.md) · [D-Bus API](../API_DBUS_en_US.md) · [歌辞 API](../API_LYRICS_en_US.md) · [发布之程](../RELEASE.md) · [26.4.1.0 发布之记](../RELEASE_NOTES_26.4.1.0.md)

## 一、本志叙略

Ter-Music者，端闱之乐部也。以斐氏FFmpeg解声，纽氏ncursesw绘界，运行时择PipeWire、PulseAudio或ALSA而用之。内置SQLite曲库；编译时通D-Bus者，兼有MPRIS与D-Bus控制之接口。

其体分二层：**核心**主音声之器、传输之令与前端所授之路径队列；**前端**（TUI或CLI）主文件、曲库、曲帙、往迹与远程之下载。无核心时，前端自起其一：容器之外，则自后台析一核心；若在玲珑之器，器不能析离脱之役，乃请D-Bus激活（详见「符令」一节之玲珑说）。

## 二、功用

**播弄**
- MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、WavPack之属；FLAC/APE/WV通CUE分轨，自识GBK、BIG5、Shift-JIS之编码。
- 十七种播弄之制（序进、回环、乱序，及文件夹、专辑、艺术家之域），六档迅疾（0.75x–3x），1.0x时预载次曲，接续无间。
- 十段ISO均衡器（31 Hz–16 kHz，±12 dB），有预设与软限幅。
- 汰聲（卡拉OK）：以隨包內置 ONNX 之型（UVR-MDX-NET 3）為之，分勿汰、微汰、盡汰三档；`[卡拉OK]` 之鈕與三档常顯可用——別無功能之開關，亦無擇型之事；等非勿汰者，無藏之曲首播必候首段伴奏而後起（見[汰聲](#七汰聲)）。
- 内嵌或外之LRC歌辞；时签可依者，逐字昭焕。

**内容**
- SQLite曲库，FTS5搜检、拼音匹配、递归扫描；按艺术家、专辑、流派览之。
- 播弄队列、自建曲帙、珍存、播弄往迹、目录往迹。
- SMB、SFTP、FTP、WebDAV、HTTP之远源；前端逐曲下载于本地之藏，乃以藏中之路授核心。SMB须Samba之用户端，SFTP须known_hosts之文（见[远源](#八远源)）。

**文界**
- 六格（曲帙、信息、控御、频谱、歌辞、黑胶），七种JSON局度预设，强制平铺。
- 二十四套调色之谱，加一自定槽位；每格可别立前景、背景之色。
- 端闈明暗之自適：自探端闈之底（OSC 11、deepin-terminal之題、XDG案牘、COLORFGBG、TERM之示），出廠之色與七預設隨之而易；於设置→界面外观擇自、明、暗。
- 专辑封面以彩色半块绘之；黑胶格有旋转之封面圆标。
- 键鼠并用；内置`zh_CN`、`en_US`、`zh-PQ`三语。
- 一統之擇文/擇目器：有圖形之會則用案牘之器（`zenity`/`kdialog`，先經 XDG 之門戶），純端闈則手書其途；開目錄、附目錄、裝語言包與默樂目錄四處共之。

**通合**
- CLI与离脱端闱之后台播弄役使，可于任意端闱御之。
- 编译时通D-Bus者，有MPRIS及Info、Control、Queue、Config、Lyrics诸接口。
- 承应Linyaps封装。

## 三、纳置

### Arch Linux

AUR之方已备为`cn.org.huansoft.ter-music`，然**尚未投递**；其固定一显式之源提交（`_commit`，今暂钉于`32737f0`，即`testing`之最新，投递之前当改为终发之提交）。可由下方源码营构，或用 [BUILD_GUIDE.md](../BUILD_GUIDE.md) 所述本地封装之产。其方列 `onnxruntime-cpu` 为汰聲之可择依；AUR 之包由源營構，故汰聲用系統之 ONNX Runtime，而非二進制包中之靜本。

### 由源码营构

必需：CMake 3.10+、C99之译器、pkg-config、ncursesw、FFmpeg诸库（avcodec、avformat、avutil、avfilter、swresample、swscale）、libpng、libjpeg、libxml2、SQLite3、zlib与libcurl。libcurl非可择：缺之则CMake以明误止其营构。

可选：PipeWire、PulseAudio、ALSA之出；D-Bus（MPRIS/API）。SMB之远源别须运行期之外器`smbclient`（Samba之用户端）；缺之仍可营构，SMB之联则明言未装。汰聲須 ONNX Runtime：DEB、RPM 與 AppImage 之包靜鏈之於二進制，由源營構者（含 AUR 之包）則以 `dlopen` 載系統之共庫（`libonnxruntime.so.1.21`、`.so.1` 或 `.so`；惟用 CPU 執行之後端）；消音之型由封裝之營構隨包內置（見[由源码营构](#由源码营构)與[汰聲](#七汰聲)）。

**Fedora / RHEL**

```bash
sudo dnf install cmake gcc make pkg-config ncurses-devel \
  ffmpeg-free-devel libpng-devel libjpeg-turbo-devel libxml2-devel \
  sqlite-devel zlib-devel libcurl-devel
# 可择
sudo dnf install pipewire-devel pulseaudio-libs-devel alsa-lib-devel \
  dbus-devel
# SMB之远源须运行期之Samba用户端
sudo dnf install samba-client
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential cmake pkg-config libncursesw5-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
  libswresample-dev libswscale-dev libpng-dev libjpeg-dev libxml2-dev \
  libsqlite3-dev zlib1g-dev libcurl4-openssl-dev
# 可择
sudo apt install libpipewire-0.3-dev libpulse-dev libasound2-dev \
  libdbus-1-dev
# SMB之远源须运行期之Samba用户端
sudo apt install smbclient
```

**Arch Linux**

```bash
sudo pacman -S --needed base-devel cmake pkgconf ncurses ffmpeg \
  libpng libjpeg-turbo libxml2 sqlite zlib curl
# 可择
sudo pacman -S --needed libpipewire libpulse alsa-lib dbus
# SMB之远源须运行期之Samba用户端
sudo pacman -S --needed smbclient
```

**营构**

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
sudo make install   # 可择
```

可执行在`build/ter-music`（于营构之舍则`./ter-music`）。本地产物：DEB一（Debian 10–13 / Ubuntu 20.04–26.04）、RPM四（EL7–10）、AppImage一（glibc 2.28）、AUR一；Linyaps之UAB與layer本次不發（俟上游 ll-box 之修釋出而後發）；DEB/RPM之源码包与调试包为可择。见 [BUILD_GUIDE.md](../BUILD_GUIDE.md)。

未顯傳 `-DCMAKE_INSTALL_PREFIX` 者，默之前綴為玲瓏之局 `/opt/apps/cn.org.huansoft.ter-music/files`，故 `sudo make install` 置於 `/opt` 之下（顯傳之前綴，如 `/usr`，則尊之）。發布之包皆用此局；deb、rpm、AUR 與 AppImage 惟於 `/usr` 之下置符鏈（Linyaps 裝於其容器之內）。見 [BUILD_GUIDE.md](../BUILD_GUIDE.md)。

消音之型非由源營，乃營構之時所注之數據文也。封裝之腳本下之於 `.tmp/models/`（先 `aria2c`，不成則退 `curl`/`wget`），校其大小與 SHA-256，壞者拒之。凡由源營構者，宜顯傳所下之文：

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
  -DTER_MUSIC_BUNDLED_MODEL=/path/to/UVR_MDXNET_3_9662.onnx ..
```

CMake 校其文（29,704,436 bytes，SHA-256 `e02220e8…aadc`），裝為 `<前綴>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`，並裝 `models/NOTICE.md`。不傳此選則不裝型：樂部仍可啟，等非勿汰時示「內置模型缺失」而播原聲。型未嘗於運行時下之，亦絕不書於用戶之數據目。

## 四、速上手

```bash
ter-music                        # 启TUI；无核心时自起其一（玲珑之内为D-Bus激活）
ter-music -o ~/Music             # 开目录或文件
ter-music -o ftp://host/music/   # 于TUI中开远程目录
```

文界须真端闱，勿以管道引其出。按`O`开目录，`↑`/`↓`择曲，`Enter`或`Space`播之，`q`退。

開目錄（`O`）、附目錄（`I`）、於 `F7` 裝語言包，及 節度 → 默途 之默樂目錄，皆用一擇器。有圖形之會則啟 `zenity` 或 `kdialog`（`portal` 乃「先用案牘之器」之意；ter-music 未自為 D-Bus `FileChooser` 之客）；純端闈則令手書其途——空行則罷，`~/` 為之展。`TERM_MUSIC_FILE_CHOOSER=portal|zenity|kdialog|tty` 可強定其後端（未知之值歸於自擇），auto 無 `DISPLAY`/`WAYLAND_DISPLAY` 時退於手書。AppImage 之 AppRun 為所攜之庫而布 `LD_LIBRARY_PATH`；今外框（及外器 `smbclient`）未行之前先去 `LD_LIBRARY_PATH`/`LD_PRELOAD`/`LD_AUDIT`，而用宿主自有之庫。若其框仍不能起、或異常而退（非用戶自罷者），則擇器退於手書其途而示其故；自罷則不退，強定而不可用之後端仍明白報誤。罷之，或所擇非已存之目，皆不改隊列與節度。

如此所开之远程URL，惟用URL中所携之凭据。须密码之伺服，宜于`F2` → 远程设备存其联而后开目录，勿以`user:password@`入URL：URL留于shell之史与进程之表。`ter-music play`与`ter-music daemon start --open`拒远程URL，盖核心惟播本地之路。

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

鼠：左键择而行之，轮滚，右键开上下文之菜单；截右键之端闱可用中键。均衡器之页则异：左键惟择其柱，不改其值；轮乃损益其值，每格一 dB；预增益同之；开关之行不应于轮。菜单滑入之动画，每移一格只合一遍；慢端闱与流光时自降，或止而不为，输入不待动画之毕；可用 `TER_MUSIC_MENU_ANIM=full|reduced|off` 强定其档，不设则自择。局度随端闱之尺寸自适。

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
| `karaoke` | 汰聲之档（off、soft、full、0-2、cycle）；档即開關，凡档皆受 |
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

`ter-music help <command>`与`ter-music help all`出全说。`--lang <id>`惟易本進程（CLI之子令或TUI）本次之語，絕不書於`config.xml`；其序為`--lang`/`TERM_MUSIC_CLI_LANG` > `ui_language` > locale。

**Linyaps。** 包中之可执行在容器内：

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music show
ll-cli run cn.org.huansoft.ter-music -- ter-music play ~/Music
```

后台播弄于容器内改用D-Bus激活或随包之systemd用户役使；亦可在`ll-cli run`中加`--foreground`；于宿主以`systemctl --user enable --now cn.org.huansoft.ter-music`启其常驻之役。容器既行时，以`ll-cli enter`或`journalctl --user -u cn.org.huansoft.ter-music`读其出。

TUI于容器内不能析离脱之核心，故请宿主激活其役。若激活不可用，宜先于宿主以前台之式起核心，而后开TUI：

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music daemon foreground
```

**ll-box冷启之重放。** ll-box ≤ 2.3.5之冷启转发，遇端闱队列既满，则重放其已写之字节，以重出之文混入端闱之流。应用不能止其写；容器之内，故逐帧计其入端闱之字节（`/proc/self/io`之`wchar`），惟见骤写或背压之据、或端闱易其修广，乃一整屏重绘。裸模式之重置亦准此据（或易其修广），`endwin`+`refresh`之兜底仍须显启（`TER_MUSIC_TTY_HARD_RESET=1`，默闭）。容器之外不行此事，守卫亦未增节度之键、设置之目、用户可见之开关。已知之阙：无据之时，不复以时计排重绘；帧微而读者迟缓之会，若见损伤，不即重绘，或俟端闱易其修广、易其视，或复见骤写/背压之据，而后覆之。

根治在上游：PR #295已合入linyaps-box之`release/2.3`（合并之提交`31ddf04`，2026-11-20），其转发于`TryAgain`且`k > 0`时改为消费已写之字节；所及为ll-box ≤ 2.3.5。今尚无含此修之发行（最新发行仍2.3.5），`master`亦未载之，故装之时宜核所用tag果含此修，勿遽谓次版必含。ter-music 26.4.1.0本次不發UAB與layer；UAB之產物擬俟上游之修既出而後發布。`TER_MUSIC_TTY_REPAIR_BACKSTOP_MS`者，未载于典册之调参也，惟司已删之计兜底；已随其去，今设之无验。

## 六、播弄

| 组 | 制 |
| --- | --- |
| 基础（5） | 序进、单曲回环、全帙周流、乱序一度、乱序杂陈 |
| 文件夹（4） | 序进、回环、乱序、乱序回环 |
| 专辑（4） | 序进、回环、乱序、乱序回环 |
| 艺术家（4） | 序进、回环、乱序、乱序回环 |

`1`-`5`次第择序进、单曲回环、全帙周流、乱序杂陈、文件夹序进。进阶之制需曲库元数据，于设置→播弄之制启之。

迅疾：0.75x、1.0x、1.25x、1.5x、2x、3x，存其音高（FFmpeg atempo）。均衡器：31 Hz–16 kHz共十段，预设见设置→均衡器。

汰聲以隨包內置之型為之：勿汰則播原聲，微汰則四分原聲、六分型之樂，盡汰則純型之樂；單聲之曲不析，仍播原聲。型行於後臺之緒。舊逐頻而削居中者之法已盡除，今惟以此 ONNX 之型為之；別無功能之開關，亦無擇型之事；所支持之型、會話分段之藏與所限，並見[汰聲](#七汰聲)。

## 七、汰聲

汰聲者，以諸包皆裝之 ONNX 型為之。別無功能之開關，無「啟汰聲」之步，亦無擇型之事：控御之格常有 `[卡拉OK]` 之鈕，節度 → 播弄之度 惟有三档（勿汰、微汰、盡汰）。舊逐頻而削居中者之 DSP 已盡除——ONNX 之型乃今惟一之消人聲路。

### 內置之型

諸包皆裝其型為 `<前綴>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`，並裝 `<前綴>/share/ter-music/models/NOTICE.md`（MIT 之全聲明）。核心即其處而用之：運行時不下之，不錄於用戶之數據目，亦無節度之鍵以擇之。

運行時定位之序：

一、`<前綴>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`——隨包而裝，先用之；
二、`$XDG_DATA_HOME/ter-music/models/uvr-mdxnet-3-9662.onnx`（默 `~/.local/share/ter-music/models/…`）——舊版手裝之型，惟讀相容；絕不錄之，絕不書之。

其文之初啟，惟定其途、校其大小（一 `stat`，微秒而已）；全卷之 SHA-256 則於首析之前，由後臺之役計之，按途、大小、修改之時而存於進程，故開档、易档不為二十九·七 MB 之雜湊所阻。亡則示「內置模型缺失」；損則（大小或 SHA-256 不合）示「內置模型校驗失敗」，不默然他就。二者皆不礙設档：播弄仍用原聲，狀態行以界面之語具言其故。修之或易之，則大小、修改之時必變，所存者隨之而廢，新文於次用之時復校，不須重啟樂部。

ONNX Runtime 已內置於每發布之包：DEB、四 RPM（含 EL7）與 AppImage 皆靜鏈由 Debian pool 之源自營之 ONNX Runtime 1.21，系統不須別裝 `libonnxruntime`，`readelf -d` 亦不見 `libonnxruntime`。靜庫之符，上限為 GLIBC_2.17，一庫而覆 EL7–10 與 Debian 10+。不由靜庫營構者，則以 `dlopen` 載 `libonnxruntime.so.1.21`、`.so.1` 或 `.so`；惟用 CPU 之後端。缺之則析敗而仍播原聲，不默然改就他法。

由源營構者，惟傳 `-DTER_MUSIC_BUNDLED_MODEL=<文>` 乃含其型；不傳則不裝型，運行時示其缺失（見[由源码营构](#由源码营构)）。發布之包常內置之。

### 所支持之型

惟支持一型：**UVR-MDX-NET 3**。

| | |
| --- | --- |
| 顯示之名 | UVR-MDX-NET 3 |
| 所裝之文 | `uvr-mdxnet-3-9662.onnx`（上游之文名 `UVR_MDXNET_3_9662.onnx`） |
| 官下之 URL | <https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/UVR_MDXNET_3_9662.onnx> |
| 大小 | 29,704,436 bytes |
| SHA-256 | `e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc` |
| 許可之據 | MIT，據 Ultimate Vocal Remover GUI（UVR）之 README：第三方用其型者，須署 UVR 及其撰者；`TRvlvr/model_repo` 之庫別無 LICENSE 之文 |

其型所須之署曰：「Vocal separation model: UVR-MDX-NET 3, from Ultimate Vocal Remover GUI (UVR) by Anjok07, TRvlvr and contributors — MIT, used with credit.」ter-music 以其型未改，作為數據之文，與本 GPL 之應用同布，全聲明在 `models/NOTICE.md`；其文仍守 UVR 所陳之 MIT 條款。

### 三等與所播之源

| 等 | 所播 |
| --- | --- |
| 勿汰 | 原聲 |
| 微汰（soft） | 四分原聲、六分型之樂 |
| 盡汰（full） | 純型之樂 |

其等藏於 `karaoke_mode`，可於控御之彈層、節度 → 播弄之度、`ter-music karaoke` 或 `Control.SetKaraoke` 易之；D-Bus 與 CLI 無「能未開」之前提——凡 0–2 皆受（越界回 `false`），`GetKaraoke` 恆回今档。

### 藏與後臺之析

析者，行於核心之後臺、低先之緒（nice 10），未嘗行於文界或播弄之緒。等非勿汰時，三十分之內之立體曲而無完備舊藏者，析於此會話；諸段既就，同會話中復播徑用其成，不復勞型；析敗者，下次起播復試之。等為勿汰則不析——档即開關。方播而易他档者，即為今曲請析；俟其段既就，乃按今位自易其源，位不絕而暫頓可容。

- **會話分段之藏：** 伴奏書於 `/tmp/ter-music-karaoke-<uid>-<pid>/<型 id>/<曲之雜湊>/`（諸目之權 0700），每十五息為一段：`segNNN.inst.flac`（盡汰）與 `segNNN.soft.flac`（微汰），段末乃書 `segNNN.ready` 之徵，另有 `manifest.txt` 之目。一析而二份俱出，二等共享此一度。
- **存亡：** 會話之目，惟存於播弄之進程未退之時。常退則先止 worker，而後盡刪其目；核心啟時則清 `/tmp/ter-music-karaoke-<uid>-*` 中進程已亡之殘目（崩潰所遺）。此段不越重啟而存，故下次會話必復析其曲（若中舊版整曲之藏則否）。
- **其鍵：** 曲之實途、大小、改易之時，加型 id、型之 SHA-256、格式之版、stem 之版、overlap 與微汰之係數。移其文或更其編，則得新鍵。
- **舊版整曲之藏（惟讀，優先用之）：** 舊構所書於 `$XDG_CACHE_HOME/ter-music/karaoke/<型 id>/<曲之雜湊>/`（默 `~/.cache/ter-music/karaoke/…`）之全璧 `inst.flac`、`soft.flac`、`meta.txt` 三者，今仍讀之。中則徑由其起播，不書會話之段。其鍵含今 stem 之版，故 stem 之派生有異者不復中；其目仍舊不動，而此曲改於會話之藏重析。啟時清其目下殘餘之 `.part`；2048 MB 之 LRU，惟此整曲之藏是計。會話之段別有預算：每會話默 512 MB（`TER_MUSIC_KARAOKE_SEGMENT_MB` 可覆，`0`（或更小）則無預算），逾則按目之 mtime 最舊者先汰；正在播、正在析之曲受護，且每書一段之前必校其餘地（留 32 MiB）。
- **無藏之曲，等非勿汰，其首播：** 播弄先候首段伴奏既就而後起——於試器約七至十息，硬限三十息——狀態行示其準備之分。若首段不至（逾時，或型或藏不可用等 worker 之敗），則改播原聲；單聲與逾三十分不待而徑播原聲；CUE 之子曲亦不待，播原聲或所中舊版整曲之藏。若起播之位不為零（復播、易其疾），則候限八息。
- **等在勿汰時**，播原聲，不析其曲。方播而擇其等者，即為今曲請析；俟其段既就而自易之，狀態行先示候，既易乃示「已切換至…（候 N 息）」。
- **方播之時：** 次段未就者，於段之界至多候八息，而後自此以原聲播至曲終（惟一易，不於曲中屢遷其源）。seek 至未析之位者，先請 worker 優為其段，至多候八息；猶未就，則自 seek 之位以原聲續。worker 至曲尾，乃補 seek 所闕之段；諸段皆就，其目乃書「complete」。
- **易其等者**，即為今曲請析；俟其段既就，乃按今位自易其源——試器之上首易約九至十五息；晚位（如播至百二十息）而易者約九息，蓋令 worker 先為今段。已就之段上 Soft ↔ Full 互易即時（不及一息），不復勞型。易時暫頓可容，位仍相續；暫停而易者亦效（於暫位易源，仍暫）。析敗，或三十息內無段可就者，仍播原聲，狀態行具言其故，可再易而試。復有界：段界退回原聲後，每易一次至多復排三次；故極慢之器或常守原聲——此時狀態行明言仍在原聲、可再易而試，絕不妄稱已易。
- **預取：** 惟 `seamless_preload=1` 時，今曲之伴奏既就（且等非勿汰），乃於後臺析隊列之次曲；`seamless_preload=0` 則不預取。CUE 之子曲，未嘗預取。
- 析用 0.25 之 overlap，定而不易；逾三十分鐘之曲則拒析（狀態行言其敗，仍播原聲）。CUE 之子曲不用會話分段之源：播原聲，或中同一物理文件之舊版整曲藏；故於 CUE 之子曲易等者，無舊版整曲之對則仍原聲、不重起播弄；中其對者仍即易；二者皆不妄稱已易。內置之型屬安裝之文；舊版所遺整曲之藏則留，欲手清其藏，退而刪 `~/.cache/ter-music/karaoke/`（或一型之子夾）可也——`/tmp` 之會話目，退時已刪。

### 不係於歌辭

不係歌辭，亦無歌辭之閘：有定時之 LRC、純文之歌辭、內嵌之歌辭，或竟無歌辭者，皆行同一型之路。

### 所限

- UVR-MDX-NET 3 乃中低之型，非完美之析：伴奏或餘人聲，似人聲之器與和聲或為所損。`soft` 等故留四分原聲，乃折中之法，非純淨之伴奏。
- 析者勞 CPU，成則每曲每會話一度：其遲速因型、器與曲之长短而異；於六核 Ryzen 5 4500U，三十秒之曲端末約十一秒（每約六秒之塊一·一至一·五秒），三分之曲約一分；弱器更遲。worker 之 ONNX 會使核心之峰約 0.8 GiB（實測 795 MiB），析畢則落。
- 會話之段，約每分音聲十五·五 MB（於 44.1 kHz 立體之曲實測；三分之曲約四十六 MB）。此較舊版整曲之對（實測約每分十二 MB）多約三成，蓋每十五息之段各具 FLAC 之首與 ready/manifest 之文。播放器退時，其目即刪；行時則上述之預算限其總量。Fedora/RHEL 默以 `/tmp` 為 tmpfs，此時預算所耗者乃內存，地不足則宜減 `TER_MUSIC_KARAOKE_SEGMENT_MB`。
- 單聲之曲、逾三十分鐘之曲不析，仍播原聲；CUE 之子曲不用會話分段之源。析敗不絕播弄，狀態行以界面之語具言其故（ONNX Runtime 自出之文仍為英文術語）。
- 惟驗 ONNX Runtime 1.21 之 CPU 後端；他版或可載，然不保。DNNL 諸後端刻意不用。
- 等非勿汰時，若 ONNX Runtime 或所裝之型不可用，則析敗而仍播原聲；界面不默然他就。發布之包已以運行庫入於二進制；不由靜庫營構者，則須系統之 `libonnxruntime`。
- 「karaoke」在本項目亦為一局度預設、為逐字歌辭之高亮，二者皆與此能無涉。

## 八、远源

远源者，前端之务也。前端列其目录，逐曲下载于本地之藏，乃以藏中之路授核心；核心未尝自开远方之URL（本地路径之约见 [API_DBUS_en_US.md](../API_DBUS_en_US.md)）。

于`F2` → **远程设备**（设置之第六目）立一联：名、协议（SMB、SFTP、FTP、WebDAV、HTTP）、主机、端口（空则用默）、用户名、密码与基途。基途不可阙：`/`为伺服之根；SMB则填共享名与其子径，如`music/albums`，勿用`smb://host/music/albums`。HTTP、WebDAV、FTP有TLS之启（https/webdavs/ftps），启之则见「信自签之证」；SFTP另有私钥、口令与known_hosts之径。目录之径/URL不必自补尾斜，前端列目录时自补之。TLS为每联显式之启，不以端口为断：非443之HTTPS，须启TLS而填其实端口。端口不在1–65535、基途空者，不可存；删联则先请确认。

协议之要：

- **SMB**以外行之`smbclient`为之，非libcurl；主机、端口、用户名、密码皆授之，密码经临时0600认证之文而非argv。宜装Samba之用户端（包名`smbclient`；Debian、Arch、RPM之封装列为荐），否则SMB之联明报缺失。文件名含`;`、`!`、双引号、反斜或制字者不收——`smbclient -c`将析之为令；宜于伺服端改名。
- **SFTP**验主机之钥，且fail-closed：须有可读之known_hosts（联中所填之径，否则`~/.ssh/known_hosts`），且已载伺服之钥。无默然初信（TOFU）之事；先以`ssh-keyscan HOST >> ~/.ssh/known_hosts`载之。私钥与口令为可择。
- **FTP**先以MLSD列之，不成则退用LIST。TLS者，隐式FTPS（`ftps://`，默端口990）。
- **WebDAV**以`PROPFIND`、`Depth: 1`行之，带命名之`<D:href>`（Apache/Nextcloud之形）与无命名者皆受。
- **HTTP**读HTML之目录索引（Apache/nginx/python之形），以链接所指为准而非所显之文，并解HTML之实体。
- HTTP与WebDAV之认证，Basic、Digest、NTLM皆由libcurl议之；启TLS时验证书，惟「信自签之证」则否；自HTTPS之重定向，永不降为HTTP。

开一夹，则后台列其目录，逐曲下载其音声之卷（惟此夹之卷，不及子夹）于藏；一次至多二百曲，多者示已载若干、共若干，子夹可别开。远器之页与主界之状态行皆示其进（`下载中 done/total，失败 n`）；`ESC`罢今务，联之动作单有「罢今务」「复列目录」「复试败曲（n）」。每曲下载既毕则授核心：队列易为所下之曲，自首曲始播。

其限：一远卷至多1 GiB，一次列目录至多32 MiB（二十万条）；连接逾十秒、列目录逾百二十秒、每曲下载逾六百秒则罢。一卷逾限则弃之而他曲续；目录过大或藏已满，则以误止。下载随需而为，无预取。

藏于`$XDG_CACHE_HOME/ter-music/remote/`（默`~/.cache/ter-music/remote/`），依联与远径而镜之。其页示其用量，可立上限（默2048 MB，`0`则不限）而清其未护者；正在播、在队列、在珍存、在往迹者不删。音卷旁之`.lrc`亦随之而藏。

**凭据。**联表在前端之`remote.xml`（权0600）。密码以定盐XOR之流蒙之，非真加密也：可防肉眼，不能防能读其文者。日志、核心之`last_opened_path`、曲库之目录往迹，皆存脱敏之URL；故自往迹复取之URL已无密码，有密之伺服宜用联表。临时开远源，勿以`user:password@`入URL：URL见于shell之史与进程之表。

**已验之域。**自动之试及HTTP、WebDAV、FTP、FTPS之TLS握手、非443之HTTPS、自签CA之信、401/404、慢速下载之罢。SMB以仿Samba令析之假`smbclient`试之；SFTP惟验known_hosts之fail-closed。真Samba、真SSH与FTPS之数据道，未得其器而未试。

TUI亦可径开远方之URL：

```bash
ter-music -o webdav://host/music/
```

所受之scheme：`http`、`https`、`ftp`、`ftps`、`sftp`、`smb`、`webdav`、
`webdavs`（`dav`/`davs`为别名），且大小写无别。

`ter-music play`与`ter-music daemon start --open`不受远方之URL：核心惟播本地之路，欲开远源，用TUI。

## 九、歌辞与封面

- 先取内嵌歌辞（FFmpeg/APE之签），无则取同名之`.lrc`。
- LRC有逐字、行首尾或插值之时签者，按字昭焕。
- `Ctrl+L`入歌辞定位；`Tab`易内嵌与外之来源。
- 封面先取内嵌之图，次取同目录之`cover`/`folder`/`front`/`album`。
- 色自二百五十六而降于八、终归黑白；`ter-music cover`与`Info.GetCoverArt`出点阵、ASCII或半块之封面。

## 十、局度与色采

局度者，二叉分栏之树也。其叶为六格之一，故主区常强制平铺；端闱修广之变，按千分比复算。预设：`classic`、`vinyl`、`karaoke`、`visualizer`、`focus`、`stage`、`neon`。

于`F2`→界面外观→界面布局择而编之。其文：

- 内置：`<prefix>/share/ter-music/layouts/layouts.json`
- 用户覆盖：`~/.config/ter-music/layouts.json`
- 开发覆盖：`TER_MUSIC_LAYOUTS=/path/to/layouts.json`

二十四套调色之谱加一自定槽位，主侧栏、高亮、边框；每格亦可别立前景/背景。全式见 [UI_LAYOUTS_en_US.md](../UI_LAYOUTS_en_US.md)。

### 端闈明暗之自適

`F2`→界面外观→**通體色式**，可擇`自`（默）、`明`、`暗`；藏於`config.xml`之`preferences.appearance_mode`（契約十）。`自`者，先於首帧前問端闈之底色（OSC 11）；不答，則次端闈之本題（今惟 deepin-terminal 之適，讀而不寫，至多每二秒察其變）、次 XDG 系統之式、次`COLORFGBG`、次`TERM`之示（名含`light`/`dark`），終守舊暗之貌——故不答之端闈/案牘，其觀與舊版無異（OSC 11 探之限約二百毫秒，案牘讀之限約百二十毫秒，且惟`自`者行之）。端闈之答自鍵流而去，不入界面；惟本程序方發問、猶待答之窗內，得全答（BEL 或 ST 終）乃以為答；手書同文，仍得逕達（窗內預讀至多八十毫秒）。 `自`者之序：先 OSC 11（端闈實底，為準）→ 次端闈本題之適（今惟 deepin-terminal，讀其本題之配，先於案牘之好）→ 次 XDG 案牘之 `org.freedesktop.appearance color-scheme`（系統之好，以 `SettingChanged` 隨之）→ 次 `COLORFGBG` → 次 `TERM` 之示 → 終守舊暗；設置之頁顯其源。端闈本題之適，惟 deepin-terminal 用之；他端不讀其文，其行如故。識之也，先`TERM_PROGRAM`；未設，乃察祖之鏈。前端本地 `~/.config/ter-music/appearance-terminals.json` 可按端闈覆通體 `preferences.appearance_mode`（通體之鍵仍為默，亦 `config.xml` 中惟一存者）。內置預設徑自適；用戶 `layouts.json` 與預設同名者，惟其前景猶等預設出廠暗值乃易明色——君所改者存，`TER_MUSIC_LAYOUTS` 則如其文而用。

底色既明，則出廠主題之色與七預設皆易為明底之安色；一預設之內，諸格之色於二百五十六、十六、八色之端闈仍各不相同。暗色之譜一仍其舊。所易者惟出廠之值：君已改之主題色則存。`~/.config/ter-music/layouts.json`之用戶局度不為改寫；若其 id 與內置預設同名，而某格之前景猶等預設出廠暗值，則此前景隨明色之變。故用戶預設副本中未改之出廠值仍自適（局度修器顯所存之值，設置顯其效之值）；君所改之值與`TER_MUSIC_LAYOUTS`則如其文而用。於局度修器存內置預設，則先物化今屏之色而後定。

端闈能報其見者（`自`時啟`\e[?1004h`，且terminfo有焦點之鍵），去而復歸，即自復探，不須重啟；不能者，用同頁之**復探端闈之底**。節度頁之色名亦及明色之目（炭、深青、暗珀……）；出廠默認之色，書曰「自（明暗）」。所探之明暗屬各前端之進程，**絕不書於`config.xml`**；色式則共享之節度，今行之TUI經舊有`ConfigChanged`重載之路而效。

## 十一、节度与数据

`$XDG_CONFIG_HOME/ter-music`（默`~/.config/ter-music`）：

```text
config.xml     核心节度
library.db     SQLite曲库：曲目、珍存、往迹、曲帙、目录往迹
remote.xml     远服与藏之上限（惟前端，权0600）
layouts.json   用户局度之覆
ui-layout      今局度之id
lang/          用户语言包
help/          用户速上之文
```

远曲之藏于`$XDG_CACHE_HOME/ter-music/remote/`（默`~/.cache/ter-music/remote/`），依联与远径而镜之；同名之`.lrc`亦随曲而藏。藏之上限与清理在设置→远程设备。封面之藏于`/tmp/ter-music-cover-*.jpg`，退时清之。

汰聲之型隨包而裝於 `<前綴>/share/ter-music/models/`（`uvr-mdxnet-3-9662.onnx` 與 `NOTICE.md`），即其處而用之。`$XDG_DATA_HOME/ter-music/models/uvr-mdxnet-3-9662.onnx`（默 `~/.local/share/ter-music/models/…`）惟舊版手裝之型之只讀退路，絕不書之。舊版整曲伴奏之藏（若有）在`$XDG_CACHE_HOME/ter-music/karaoke/<型 id>/<曲之雜湊>/`（默`~/.cache/ter-music/karaoke/`），惟讀，2048 MB 之 LRU 惟此是計。會話之分段惟書於`/tmp/ter-music-karaoke-<uid>-<pid>/`（默會話預算 512 MB，`TER_MUSIC_KARAOKE_SEGMENT_MB`），進程退則刪；見[汰聲](#七汰聲)。

常用`config.xml`之键：

| 键 | 值 |
| --- | --- |
| `audio_backend` | 0自择，1 PulseAudio，2 ALSA，3 PipeWire |
| `audio_latency_ms` | 输出之迟，20-250 |
| `volume_percent` | 默音量，0-100 |
| `default_playback_speed` | 0.75-3.0 |
| `default_play_mode` | 0-16 |
| `ui_language` | `zh_CN`、`en_US`、`zh-PQ`或已装之语言包ID |
| `karaoke_mode` | 0勿汰，1微汰，2盡汰；默0；档即開關 |
| `appearance_mode` | 0自（從端闈），1明，2暗 |
| `resume_last_playback` | 复上次之曲与位 |
| `stop_core_on_exit` | 1退时止核心，0续播 |
| `core_exit_when_no_frontend` | 末前端去后十秒核心自退 |

節度之契約為十二版。十二版**刪** `preferences.karaoke_feature` 與 `preferences.karaoke_model` 二鍵：型隨包內置，無復功能之開關與擇型之事。以倉庫之實史為準：契約四 = 標籤 `v2.1.0`（其根屬嘗為字面 `"2.2"`，按整數析之為二，故仍可載，而由遷改書一次），七 = 常駐核心，八 = `stop_core_on_exit`，九 = `<keybindings>` 覆表**與** `karaoke_mode`（DSP 時之档），十 = `preferences.appearance_mode`，十二 = 內置 ONNX 之型並刪 DSP 時之卡拉 OK 鍵。倉庫未嘗發布契約十一之構，故已發之文必不可含此二刪鍵。v10 或更早之 `config.xml`，於新核心首啟之時遷之：其文以 `version="12"` 整卷重書，諸餘之鍵（含 `karaoke_mode`、主題、途徑、均音與鍵綁）皆如其舊；早於 v10 之文亦受前遷之一時默值（v4 以下背景 0→-1，v7 以下 `core_exit_when_no_frontend=0`，v8 以下 `stop_core_on_exit=1`，v10 以下 `appearance_mode=auto`）；再啟則其文字節不變。`config get karaoke_feature` 與 `config set karaoke_feature …` 今以未知之鍵而敗，`Config.Set` 遇尚含二鍵之補則舉而拒之。降版不為之支持：v12 之前（契約十）之構讀 v12 之文（版本之屬按整數而析，其舊遷之塊皆不中），視二刪鍵為亡——汰聲為勿汰——而於次存之時以版本之屬書回十。升級之際，新前端遇尚行之 v12 前核心，其節度之鏡按已知之鍵而填（惟一之握手門為 `api_version` 七）；惟載荷尚帶刪鍵乃舉而拒之，而此惟未發布之契約十一開發構所為——重啟核心（`ter-music daemon restart`）或復登即自癒。

惟改过之键乃书于`config.xml`：

```xml
<keybindings>
  <binding action="track.next" key="]"/>
  <binding action="track.prev" key="["/>
</keybindings>
```

保留之键：`F1`-`F9`、`q`/`Q`、`Tab`/`Shift+Tab`。动作ID以`ter-music keys list`查，文以`ter-music keys check`校。

语言包为XML之文（`<lang id="..." name="...">`，内含`<string key="...">`），置于`~/.config/ter-music/lang/`，或于`F7`语言之视以`A`装`.tar.gz`、以`D`删之。内置：`zh_CN`、`en_US`、`zh-PQ`。

## 十二、通合

编译时通D-Bus者，MPRIS以`org.mpris.MediaPlayer2.ter_music`布于`/org/mpris/MediaPlayer2`。同对象亦具：

| 接口 | 用 |
| --- | --- |
| `cn.org.huansoft.ter_music.Info` | 曲目、进度、封面、歌辞行、信息块 |
| `cn.org.huansoft.ter_music.Control` | 传输、跳转、音量、迅疾、制、汰聲、退 |
| `cn.org.huansoft.ter_music.Queue` | 核心之本地路径队列 |
| `cn.org.huansoft.ter_music.Config` | 节度读写 |
| `cn.org.huansoft.ter_music.Lyrics` | 歌辞行与逐字之时 |

`Info.GetInfo`出`core.api_version`（今七）。详见 [API_DBUS_en_US.md](../API_DBUS_en_US.md) 与 [API_LYRICS_en_US.md](../API_LYRICS_en_US.md)。`Control.SetKaraoke` 無前提而受 0–2 之档（档即開關，越界回 `false`），`Control.GetKaraoke` 恆回今档，`ter-music karaoke` 讀而改之——見[汰聲](#七汰聲)。

## 十三、营缮

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

CI行諸定門於每易，如下所列；真型之全量汰聲e2e別為advisory之役，不阻合并，發布之機與nightly乃終判（見 [BUILD_GUIDE.md](../BUILD_GUIDE.md) §9.5）：

| 门 | 规 |
| --- | --- |
| `scripts/test/check-core-purity.sh` | 核心无内容与远源之符号 |
| `scripts/test/check-ui-purity.sh` | UI惟经`player`门面而及播弄 |
| `scripts/test/check-config-ownership.sh` | 核心为`config.xml`之独书者 |
| `scripts/test/check-package-identity.sh` | 诸封装之应用ID与版相合 |
| `scripts/test/check-c-strings.sh` | C文不得直書華文，皆由i18n之鍵 |
| `scripts/test/package-layout-check.sh`（`TM_LAYOUT_STATIC_ONLY=1`） | `/opt/apps`之前綴、`/usr`之鏈、型之常量與接線，不營封裝 |
| no-D-Bus 之構 | 可擇 D-Bus 之構可譯、可鏈、可啟 |

## 十四、祛疑

- **无声：** 察PipeWire、PulseAudio或ALSA之行；后端依此序探之。显择之后端播而不成，则退就次可用者。可于设置→音声易之。
- **滞或爆音：** 于设置增高输出之迟（20、40、60、80、100、120、150、200 或 250 ms）。
- **华文乱码：** 用UTF-8之locale与UTF-8之端闱；`locale`宜见`LC_CTYPE=UTF-8`。
- **AppImage 之包不彈文檔之框：** 所攜之框不能起，則自退於手書其途；若猶不見，察端闈之誌（`GUI dialog failed` / `falling back to TTY`），於宿主裝 `zenity` 或 `kdialog`，或以 `TERM_MUSIC_FILE_CHOOSER=zenity|kdialog|tty` 強定其後端。
- **Linyaps中文界出重复错乱之行：** 即上文Linyaps一节所述ll-box ≤ 2.3.5冷启之重放。守卫见骤写或背压之据则整屏重绘；无据之时不复以时计重绘，其损或俟端闱易修广、易视，或复见据而后覆。
- **营构时无头文：** 装上所依之`-devel`/`-dev`包。
- **远目录不能列：** 察协议、主机、端口与基途（SMB须填`共享名/子径`）；SMB须装`smbclient`；SFTP须可读之known_hosts；https/webdavs/ftps须启TLS。其页示本地化之由，联之动作单有「复列目录」。
- **远曲下载败：** 其页示`失败 n`与首误；去其因（网络、权限、磁盘）而后用「复试败曲（n）」，已藏之曲不复下载。单卷逾1 GiB或藏已满，则于此曲止；可于远程设备页增上限或清藏。
- **远目录下载至半而止：** 一次至多下载二百曲；宜入子夹别开。
- **自目录往迹复开远URL而认证败：** 入库之URL已脱敏（本不含密码）；须密码之伺服宜用远程设备页所存之联。
- **CUE分轨不显：** `.cue`须与音声同名；文乱则于设置改CUE之编码。
- **曲库空：** 按`M`开曲库而扫之或复扫；库为配置目录下之`library.db`。
- **狀態行示內置之型不可用：** 其包宜已裝 `<前綴>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`。由源營構而未傳 `-DTER_MUSIC_BUNDLED_MODEL` 者不裝型；文損則大小/SHA-256 之校不過。發布之包已以 ONNX Runtime 入於二進制；由源營構者亦須系統之庫（`libonnxruntime.so.1.21`、`.so.1` 或 `.so`）。狀態行以界面之語具言其故（缺失、校驗失敗、運行庫不可用），档仍可設，播弄仍用原聲。其途之初啟校其大小，全卷 SHA-256 則於後臺校之；修之或易之則所存者廢，不須重啟（用系統庫之構，須已裝其庫）。不回就舊有之內法。
- **首播或候，或還原聲：** 等非勿汰而曲無藏者，先候首段伴奏既就（約七至十息，總計至多三十息；起播之位不為零者至多八息），狀態行示其準備之進。若候逾時或析不成（型或藏不可用），則徑播原聲；單聲、逾三十分與 CUE 之子曲不待而徑播原聲（CUE 中舊版整曲之藏者仍用之）。
- **汰聲之析常敗：** 逾三十分鐘之曲拒析，單聲之曲與 CUE 之子曲不析，內置之型亡或損、或無 ONNX Runtime 亦敗；狀態行具言其故。見[汰聲](#七汰聲)。

## 十五、律例

GPL-3.0-only，见 [LICENSE](../../LICENSE)。

隨包內置之 UVR-MDX-NET 3 型以 MIT 分發（見 [models/NOTICE.md](../../resources/models/NOTICE.md)）；發布之二進制靜鏈以 LGPL-2.1-or-later 發布之 FFmpeg 構（未啟 `--enable-gpl`），官包亦靜鏈以 MIT 發布之 ONNX Runtime；全聲明與源之取法見 `debian/copyright`。

Ter-Music惟播弄之器，不供、不藏、不布音声之文；所播之文，君自备之，当合律令。

## 十六、撰者

浣软科技（HuanSoft）· <yxzl666xx@outlook.com>

## 十七、鸣谢

- **@guanzi008**：Debian之封装、MPRIS、UTF-8之入、鼠与文界之修。
- **@Zeta**：Arch Linux之封装。
- **Ultimate Vocal Remover GUI（UVR）**及撰者 Anjok07、TRvlvr 与诸 contributors：ter-music 以 UVR-MDX-NET 3 人聲分離之型未改，作為數據之文隨包而布，依[汰聲](#七汰聲)所述之 MIT 條款署名而用。
