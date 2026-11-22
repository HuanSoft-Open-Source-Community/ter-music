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

其体分二层：**核心**主音声之器、传输之令与前端所授之路径队列；**前端**（TUI或CLI）主文件、曲库、曲帙、往迹与远程之下载。无核心时，前端自起其一：容器之外，则自后台析一核心；若在玲珑之器，器不能析离脱之役，乃请D-Bus激活（详见「符令」一节之玲珑说）。

## 二、功用

**播弄**
- MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、WavPack之属；FLAC/APE/WV通CUE分轨，自识GBK、BIG5、Shift-JIS之编码。
- 十七种播弄之制（序进、回环、乱序，及文件夹、专辑、艺术家之域），六档迅疾（0.75x–3x），1.0x时预载次曲，接续无间。
- 十段ISO均衡器（31 Hz–16 kHz，±12 dB），有预设与软限幅。
- 卡拉OK（汰聲）：勿汰、微汰、盡汰三档，於 200 Hz–5 kHz 间逐频而削居中者；惟兩聲並列者行之，启之则迟约九毫秒。
- 内嵌或外之LRC歌辞；时签可依者，逐字昭焕。

**内容**
- SQLite曲库，FTS5搜检、拼音匹配、递归扫描；按艺术家、专辑、流派览之。
- 播弄队列、自建曲帙、珍存、播弄往迹、目录往迹。
- SMB、SFTP、FTP、WebDAV、HTTP之远源；前端逐曲下载于本地之藏，乃以藏中之路授核心。SMB须Samba之用户端，SFTP须known_hosts之文（见[远源](#七远源)）。

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

AUR之方已备为`cn.org.huansoft.ter-music`，然**尚未投递**。可由下方源码营构，或用 [BUILD_GUIDE.md](../BUILD_GUIDE.md) 所述本地封装之产。

### 由源码营构

必需：CMake 3.10+、C99之译器、pkg-config、ncursesw、FFmpeg诸库（avcodec、avformat、avutil、avfilter、swresample、swscale）、libpng、libjpeg、libxml2、SQLite3、zlib与libcurl。libcurl非可择：缺之则CMake以明误止其营构。

可选：PipeWire、PulseAudio、ALSA之出；D-Bus（MPRIS/API）。SMB之远源别须运行期之外器`smbclient`（Samba之用户端）；缺之仍可营构，SMB之联则明言未装。

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

可执行在`build/ter-music`（于营构之舍则`./ter-music`）。本地产物：DEB一（Debian 10–13 / Ubuntu 20.04–26.04）、RPM四（EL7–10）、AppImage一（glibc 2.28）、Linyaps之UAB与layer各一；DEB/RPM之源码包与调试包为可择。见 [BUILD_GUIDE.md](../BUILD_GUIDE.md)。

## 四、速上手

```bash
ter-music                        # 启TUI；无核心时自起其一（玲珑之内为D-Bus激活）
ter-music -o ~/Music             # 开目录或文件
ter-music -o ftp://host/music/   # 于TUI中开远程目录
```

文界须真端闱，勿以管道引其出。按`O`开目录，`↑`/`↓`择曲，`Enter`或`Space`播之，`q`退。

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

鼠：左键择而行之，轮滚，右键开上下文之菜单；截右键之端闱可用中键。菜单滑入之动画，每移一格只合一遍；慢端闱与流光时自降，或止而不为，输入不待动画之毕；可用 `TER_MUSIC_MENU_ANIM=full|reduced|off` 强定其档，不设则自择。局度随端闱之尺寸自适。

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

TUI于容器内不能析离脱之核心，故请宿主激活其役。若激活不可用，宜先于宿主以前台之式起核心，而后开TUI：

```bash
ll-cli run cn.org.huansoft.ter-music -- ter-music daemon foreground
```

**ll-box冷启之重放。** ll-box ≤ 2.3.5之冷启转发，遇端闱队列既满，则重放其已写之字节，以重出之文混入端闱之流。应用不能止其写；容器之内，故逐帧计其入端闱之字节（`/proc/self/io`之`wchar`），惟见骤写或背压之据、或端闱易其修广，乃一整屏重绘。裸模式之重置亦准此据（或易其修广），`endwin`+`refresh`之兜底仍须显启（`TER_MUSIC_TTY_HARD_RESET=1`，默闭）。容器之外不行此事，守卫亦未增节度之键、设置之目、用户可见之开关。已知之阙：无据之时，不复以时计排重绘；帧微而读者迟缓之会，若见损伤，不即重绘，或俟端闱易其修广、易其视，或复见骤写/背压之据，而后覆之。

根治在上游：PR #295已合入linyaps-box之`release/2.3`（合并之提交`31ddf04`，2026-11-20），其转发于`TryAgain`且`k > 0`时改为消费已写之字节；所及为ll-box ≤ 2.3.5。今尚无含此修之发行（最新发行仍2.3.5），`master`亦未载之，故装之时宜核所用tag果含此修，勿遽谓次版必含。ter-music次一Release不拟发布UAB；UAB之产物拟俟上游之修既出而后发布。`TER_MUSIC_TTY_REPAIR_BACKSTOP_MS`者，未载于典册之调参也，惟司已删之计兜底；已随其去，今设之无验。

## 六、播弄

| 组 | 制 |
| --- | --- |
| 基础（5） | 序进、单曲回环、全帙周流、乱序一度、乱序杂陈 |
| 文件夹（4） | 序进、回环、乱序、乱序回环 |
| 专辑（4） | 序进、回环、乱序、乱序回环 |
| 艺术家（4） | 序进、回环、乱序、乱序回环 |

`1`-`5`次第择序进、单曲回环、全帙周流、乱序杂陈、文件夹序进。进阶之制需曲库元数据，于设置→播弄之制启之。

迅疾：0.75x、1.0x、1.25x、1.5x、2x、3x，存其音高（FFmpeg atempo）。均衡器：31 Hz–16 kHz共十段，预设见设置→均衡器。

汰聲：勿汰、微汰、盡汰三档，惟兩聲並列者行之。其法於 200 Hz–5 kHz 间逐频而较 side 与 mid 之幅比，削居中相干者——微汰餘约八分贝，盡汰尽去带内居中者——声场展开之伴奏与偏于一侧之瞬态存焉，故偏侧之镲、鼓不復使居中之人声随原声漏出。单声之曲直通。启之则迟窗长减一之数（44.1 kHz 下三百八十三样本，约八点七毫秒；诸采样率皆不过十毫秒）；止之则约三百五十毫秒渐隐，而后逐位直通。可于控御之弹层、设置→播弄、`ter-music karaoke`或`Control.SetKaraoke`改之。

## 七、远源

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

## 八、歌辞与封面

- 先取内嵌歌辞（FFmpeg/APE之签），无则取同名之`.lrc`。
- LRC有逐字、行首尾或插值之时签者，按字昭焕。
- `Ctrl+L`入歌辞定位；`Tab`易内嵌与外之来源。
- 封面先取内嵌之图，次取同目录之`cover`/`folder`/`front`/`album`。
- 色自二百五十六而降于八、终归黑白；`ter-music cover`与`Info.GetCoverArt`出点阵、ASCII或半块之封面。

## 九、局度与色采

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

## 十、节度与数据

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
| `appearance_mode` | 0自（從端闈），1明，2暗 |
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

## 十一、通合

编译时通D-Bus者，MPRIS以`org.mpris.MediaPlayer2.ter_music`布于`/org/mpris/MediaPlayer2`。同对象亦具：

| 接口 | 用 |
| --- | --- |
| `cn.org.huansoft.ter_music.Info` | 曲目、进度、封面、歌辞行、信息块 |
| `cn.org.huansoft.ter_music.Control` | 传输、跳转、音量、迅疾、制、汰聲、退 |
| `cn.org.huansoft.ter_music.Queue` | 核心之本地路径队列 |
| `cn.org.huansoft.ter_music.Config` | 节度读写 |
| `cn.org.huansoft.ter_music.Lyrics` | 歌辞行与逐字之时 |

`Info.GetInfo`出`core.api_version`（今七）。详见 [API_DBUS_en_US.md](../API_DBUS_en_US.md) 与 [API_LYRICS_en_US.md](../API_LYRICS_en_US.md)。

## 十二、营缮

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

## 十三、祛疑

- **无声：** 察PipeWire、PulseAudio或ALSA之行；后端依此序探之。显择之后端播而不成，则退就次可用者。可于设置→音声易之。
- **滞或爆音：** 于设置增高输出之迟（20-250 ms，每次加十毫秒）。
- **华文乱码：** 用UTF-8之locale与UTF-8之端闱；`locale`宜见`LC_CTYPE=UTF-8`。
- **Linyaps中文界出重复错乱之行：** 即上文Linyaps一节所述ll-box ≤ 2.3.5冷启之重放。守卫见骤写或背压之据则整屏重绘；无据之时不复以时计重绘，其损或俟端闱易修广、易视，或复见据而后覆。
- **营构时无头文：** 装上所依之`-devel`/`-dev`包。
- **远目录不能列：** 察协议、主机、端口与基途（SMB须填`共享名/子径`）；SMB须装`smbclient`；SFTP须可读之known_hosts；https/webdavs/ftps须启TLS。其页示本地化之由，联之动作单有「复列目录」。
- **远曲下载败：** 其页示`失败 n`与首误；去其因（网络、权限、磁盘）而后用「复试败曲（n）」，已藏之曲不复下载。单卷逾1 GiB或藏已满，则于此曲止；可于远程设备页增上限或清藏。
- **远目录下载至半而止：** 一次至多下载二百曲；宜入子夹别开。
- **自目录往迹复开远URL而认证败：** 入库之URL已脱敏（本不含密码）；须密码之伺服宜用远程设备页所存之联。
- **CUE分轨不显：** `.cue`须与音声同名；文乱则于设置改CUE之编码。
- **曲库空：** 按`M`开曲库而扫之或复扫；库为配置目录下之`library.db`。

## 十四、律例

GPL-3.0-only，见 [LICENSE](../../LICENSE)。

Ter-Music惟播弄之器，不供、不藏、不布音声之文；所播之文，君自备之，当合律令。

## 十五、撰者

浣软科技（HuanSoft）· <yxzl666xx@outlook.com>

## 十六、鸣谢

- **@guanzi008**：Debian之封装、MPRIS、UTF-8之入、鼠与文界之修。
- **@Zeta**：Arch Linux之封装。
