<div align="center">

# Ter-Music端闱乐部志
![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
![Docker](https://img.shields.io/badge/Docker-承应-2496ED.svg)
![Python](https://img.shields.io/badge/Python-3.x-3776AB.svg)
![Shell](https://img.shields.io/badge/Shell-Bash-4EAA25.svg)
![Linyaps](https://img.shields.io/badge/Linyaps-承应-8A2BE2.svg)

</div>

**他语之版 / Other Languages:**
- [English](../README.md)
- [中文（现代版）](README_zh-CN_Modern.md)
- [Lyrics API (English)](../API_LYRICS_en_US.md)
- [D-Bus Info & Control API (English)](../API_DBUS_en_US.md)
- [TUI 局度之考（English）](../UI_LAYOUTS_en_US.md)

## 卷一 本志叙略
### 一 枢要功用
Ter-Music者，清简端闱之符令乐部也，专为麟纳克斯御统而造。凭斐氏FFmpeg以解绎音声，通PipeWire/PulseAudio/ALSA以传布乐响（运行时自动检测），纽氏ncursesw以营文墨之界。其用赅备，列于左：
- 赅众音之制式，凡MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、**WV（WavPack）**之伦，罔不洞达
- **CUE分轨之能**：通FLAC/APE/WV之CUE析辞，自动检知编码（GBK/BIG5/Shift-JIS）
- **内嵌歌辞之能**：斐氏AVDictionary与APE标签之文辞，优先于外.lrc文件
- **10段图示均衡器**：ISO准频（31Hz-16kHz），双二阶IIR滤波，±12dB调幅，设中可视化条图，具**均衡器预设**与**软限幅**之能
- **卡拉OK之制（汰聲）**：等有三——曰勿汰、曰微汰、曰盡汰。其术乃限频带而削居中者：以四阶Butterworth分频（每倍频廿四分贝，一分于180Hz、一分于6kHz），惟削此两分之间，故樂聲之下沉与清越者俱存，不相位抵消也。惟兩聲並列者行之；單聲之曲一仍其旧，逐位不差。其所自出有四：主控之格弹层、节度页之「播弄之度」、符令 `ter-music karaoke`、D-Bus `Control.SetKaraoke`。又：此名与逐字歌辞之highlighting、与名曰 `karaoke` 之局度预设（歌辞优先）皆非一事，读者辨之
- **17种播弄之制**：自基础（序进、单曲回环、全帙周流、乱序一度、乱序杂陈）至进阶（按文件夹/专辑/艺术家分组）
- 契LRC歌辞之文，循音程以同辉，逐字以昭焕（逐字之签、行首尾之签、相邻行时之插值，三体皆通）；**内嵌歌辞**（斐氏AVDictionary与APE标签）先于外.lrc文件。可于歌词定位模式中切换来源（Ctrl+L → Tab）
- 迅疾之度有六：曰迟、曰常、曰稍疾、曰疾、曰倍、曰三倍，任君节度
- **音乐库**：SQLite库存储，FTS5全文搜检，按艺术家/专辑/流派览之，兼**递归子目录搜索**与增量跟踪
- **播弄队列**：独立队列之界，显序号、当下播弄之标，可排序；队列归前端（核心惟行所授之径表），再启之时由乐目重建
- 乐目营理之能，任君创置多组曲帙，随宜调遣
- 远程播乐之能，通SMB、SFTP、FTP、WebDAV、HTTP诸般远器之约，以传远方服器之乐；此乃**前端**之职——列其目、逐曲下载于本地之藏，而后以本地文卷付于核心（核心惟识本地文卷）
- 珍存所好之章，便疾取览
- 自动录纪播弄之迹，便于回溯
- 志录近所临之乐籍目录，无烦复寻
- **色采之谱**：24套预设主题 + 1个自定槽位，前后色彩配对保护；**设置 → 界面外观**为六格（列表、信息、控制、频谱、歌辞、黑胶）各立前景、背景之目，格之色随其所用之局而定，侧栏、高亮、边框之色仍为通体之饰
- **数据驭局**：7 个 JSON 预设以分栏树定诸窗格之位、比例与色；强制平铺为结构之保，旧几何自动适配而不见拒；黑胶与歌辞之格可并存，曲无歌辞则歌辞格暂退而余格补其位，更至有歌辞之曲则其还自至（不待易视，亦不待改其尺寸；惟存歌辞一格者，仍于格内退作黑胶之戏）
- **恒存之储**：SQLite一统（珍存、往迹、曲帙），自动从v1 JSON迁移
- 专辑封面显明之能，可于格中以彩色半块之字绘封面之图，色自二百五十六而降于八、终归于黑白；控制区之封面由播弄总开关主之，信息窗格之封面别有节度开关，黑胶之格取其面裁为圆标，随碟而转
- **符令行之制与背景播弄**：诸符令子目咸备（`play`/`pause`/`seek`/`volume`/`speed`/`mode`/`karaoke`/`show`/`daemon`），兼有离脱背景之播弄役使，端闱既闭而乐声不绝；Linyaps 封缄之态亦可用——背景播弄或经 D-Bus 按需唤起，或凭随包之 systemd 用户役使常驻
- **信息显明之可自定**：主页信息窗格之诸域、预设与封面，及 `ter-music show` 所出之基本信息、点阵／ASCII／半块文书封面、音程之行与歌辞二行（当前及其次），俱可于文界节度中厘定
- **MPRIS 与歌词 API**：经D-Bus而通桌面媒体之制、`mpris:artUrl`封面、开放歌辞之接口，兼有 `Info`（曲目／音程／文书封面）与 `Control` 二接口，以惠他器
- 全凭键符捷操，迅疾无伦
- 音程条贯实时昭显，流转顺滑，可任意跳转

### 二 造作之本旨
本器之造，恪守**清简、捷疾、元本**之宗：
- 体至清简，不藉重轩峻宇之境，所占资源至微
- 端闱元造，纯以文墨为界，宜乎无图之器、幽隐之设，与夫耽符令之流者
- 分曹列伍，部伍明晰，易于缮治增益
- 遵西土Unix之哲，专一事而工，与他器协契无间
- 略无窥伺，不录用户毫末之迹，深敬私隐
- 前后二面，各司其职：**核心**惟供播弄之役（音声之器、传输之令、音量／迅疾／播弄之制、卡拉OK汰聲，及奉行前端所授之径表）；**前端**掌文件之统与内容（曲库、扫描、曲帙、珍存、往迹、远方之源、文墨之界）。详见 [二之三 前端与核心](#二之三-前端与核心)

### 三 殊胜之德
| 殊德 | 诠解 |
| --- | --- |
| 🚀 耗损至微 | 内存所占，恒不逾十兆，CPU之所役，几于无迹 |
| 🎨 文界焕丽 | 分栏列局，绚然有章，随端闱之修广而自适 |
| 🌍 华夷毕达 | UTF-8之文，靡不洞照，中夏之字，咸得显明 |
| 🔄 恒存其制 | SQLite库一统（节度、所珍、往迹、曲帙），器重启而如故 |
| 🎯 众视迁转 | 以F2至F8键符，迅疾迁转节度、往迹、曲帙、音乐库、语言诸视 |
| ⚡ 应感无滞 | 百帧每秒之焕新，音程条贯流转顺滑 |
| 🔧 CMake营构 | 当世营构之制，跨御统之性尤善 |
| 🔊 音声传布 | 通PipeWire、PulseAudio、ALSA三枢；自动之式启时历探诸枢（PipeWire > Pulse > ALSA，ALSA者实启常器而调其度），探之不中者不选、设界面亦不列之；播时建流不遂，则复探余枢而退就，状语明其所迁 |
| 🎛️ 10段均衡器 | ISO准图示均衡器，设中可视化条图 |
| 🎤 卡拉OK之制 | 汰聲三等（勿汰／微汰／盡汰），限频带而削居中者；惟兩聲並列者有效，樂聲不改；弹层、节度页「播弄之度」、`ter-music karaoke`、`Control.SetKaraoke` |
| ⏩ 迅疾节度 | 六档迅迟之度，播弄中可随意迁转 |
| 🌐 远程播乐 | 通SMB/SFTP/FTP/WebDAV/HTTP诸般远器之约，由前端览之、下载于本地之藏 |
| 🎨 专辑封面 | 端闱中显乐集之面，彩色半块之字，色竭则归于黑白；控制区由播弄总开关主之，信息窗格别有节度开关，黑胶之格取封面为旋转圆标 |
| 🎵 MPRIS / 歌词 API | 桌面媒体之制、`mpris:artUrl`封面，及基于D-Bus之JSON歌辞接口 |
| 🖥️ 符令行之制 | `ter-music play/pause/next/seek/volume/speed/mode/karaoke/show` 诸令皆役当下所行之实例；`show` 则出可自定之信息块 |
| 🌙 背景播弄 | `ter-music daemon start` 行无文界之播弄，任于何端闱皆得御之，不待文墨之界 |
| 🖼️ 经D-Bus出文书封面 | `Info.GetCoverArt` 为他器出点阵或ASCII之封面 |
| 📦 Linyaps 符令 | 同一套符令可于 Linyaps 容器内经 `ll-cli run cn.org.huansoft.ter-music -- ter-music …` 行之；背景播弄有 D-Bus 按需唤起与常驻 systemd 用户役使二途 |

### 四 施用之境
- 无图之御宇、幽隐之服器，无轩窗界面而欲播乐者
- 微末之嵌合机括，资源至隘之麟纳克斯器用
- 操符令之工师，临案之际，不假他窗，而得闻乐
- 清简自守之幽人，无取乎繁冗轩窗之制者
- 问学C语、斐氏音术、纽氏文界之造者，此为津梁

### 五 所向之人
- 麟纳克斯之达者、耽符令之幽人
- 嵌合机括之工师、御统之守吏
- 清简自守之流
- 无轩窗界面而欲播乐者
- 问学C语与音声术法之造作者

## 卷二 译纂之境阈
### 一 所御之统
- 通融之统：麟纳克斯内核3.10以上
- 荐举之版：Fedora 30+、Ubuntu 20.04+、Arch Linux新制
- 不通之域：Windows、macOS（若君能移植，不胜忻幸）

### 二 器用之限
| 部伍 | 至卑之限 | 荐举之制 |
| --- | --- | --- |
| **CPU** | 单核1GHz | 双核2GHz以上 |
| **内存** | 64MB可用 | 128MB可用以上 |
| **存储** | 200MB可用 | 1024MB可用以上 |
| **声卡** | 波氏役使运行 | 波氏役使运行 |

### 三 译语之器
- **GCC**：7.0以上
- **Clang**：6.0以上
- **C言典则**：C99以上

### 四 营构之具
- **CMake**：3.10以上
- **Make**：GNU Make 4.0以上
- **pkg-config**：0.29以上

## 卷三 凭藉之属与纳置之符令
### 一 必需之凭藉
| 凭藉之库 | 版限 | 所司之事 |
| --- | --- | --- |
| `ffmpeg-free-devel` | 4.0+ | 音声解绎（libavcodec, libavformat, libswresample, libavutil, libavfilter） |
| `libpng` | 1.6+ | 专辑封面显示（PNG格式支持） |
| `libjpeg` | 6b+ | 专辑封面显示（JPEG格式支持） |
| `pulseaudio-libs-devel` | 10.0+ | 波氏音声传布 |
| `ncurses-devel` | 6.0+ | 文墨之界，宽字符之持 |
| `libcurl-devel` | 7.0+ | 远程乐源（SMB/SFTP/FTP/WebDAV/HTTP），为前端所用 |
| `libxml2-devel` | 2.9+ | XML节度文件解析 |
| `sqlite-devel` | 3.20+ | 音乐库数据库（FTS5全文搜检） |
| `cmake` | 3.10+ | 营构之统（译纂时必需） |
| `gcc` | 7.0+ | C言译器（译纂时必需） |
| `make` | - | 营构之具（译纂时必需） |
| `pkg-config` | - | 凭藉检核（译纂时必需） |

**可选凭藉：**

| 凭藉之库 | 所司之事 |
| --------- | -------- |
| `pipewire-0.3-devel` | PipeWire音声后端（dlopen加载，译纂时可缺，运行时自动检知） |
| `alsa-lib-devel` | ALSA音声输出后端 |
| `dbus-devel` | MPRIS D-Bus媒体会话、专辑封面与歌词API之耦 |

### 二 Fedora / RHEL / CentOS 纳置之令
```bash
sudo dnf install cmake gcc make pkg-config
sudo dnf install ffmpeg-free-devel libpng-devel libjpeg-turbo-devel pulseaudio-libs-devel ncurses-devel libcurl-devel libxml2-devel sqlite-devel
# 可选后端
sudo dnf install pipewire-devel alsa-lib-devel dbus-devel
```

### 三 Ubuntu / Debian / Linux Mint 纳置之令
```bash
sudo apt update
sudo apt install cmake gcc make pkg-config
sudo apt install libavcodec-dev libavformat-dev libswresample-dev libswscale-dev libavutil-dev libavfilter-dev libpng-dev libjpeg-dev libpulse-dev libncursesw5-dev libcurl4-openssl-dev libxml2-dev libsqlite3-dev
# 可选后端
sudo apt install libpipewire-0.3-dev libasound2-dev libdbus-1-dev
```

**注**：若斐氏开发之库不可得，当先启universe仓廪：
```bash
sudo add-apt-repository universe
sudo apt update
```

### 四 Arch Linux 纳置之令
**自 AUR 纳置（荐举）：**
```bash
# 用 yay（AUR 助手）
yay -S cn.org.huansoft.ter-music

# 用 paru（AUR 助手）
paru -S cn.org.huansoft.ter-music
```

**用 ZPM（MengXi OS 包管器）纳置：**
```bash
# 先纳置 ZPM（若未纳置）
git clone https://aur.archlinux.org/zetapm.git
cd zetapm
makepkg -si

# 继用 ZPM 纳置 cn.org.huansoft.ter-music
zpm -S cn.org.huansoft.ter-music
```

**自 AUR 手工纳置：**
```bash
git clone https://aur.archlinux.org/cn.org.huansoft.ter-music.git
cd cn.org.huansoft.ter-music
makepkg -si
```

> 说：今徙组织之名，Arch 之包自 `ter-music-cn` 更为 `cn.org.huansoft.ter-music`。
> `PKGBUILD` 具在库中，然 AUR 之库必俟守者新之，上命乃可行；未及此，请以下手工
> 营构之法，即用库中之 `PKGBUILD`。

**自源本手工营构：**
```bash
sudo pacman -S cmake gcc make pkg-config
sudo pacman -S ffmpeg libpng libjpeg pulseaudio ncurses libcurl libxml2 sqlite
# 可选后端
sudo pacman -S pipewire alsa-lib dbus
```

## 卷四 译纂之程叙
### 一 索其源本
```bash
git clone https://github.com/HuanSoft-Open-Source-Community/ter-music.git
cd ter-music
```

### 二 立营构之舍
```bash
mkdir build
cd build
```

### 三 节度CMake
```bash
cmake ..
```

CMake将自动检核系统中所有凭藉之库，若有阙失，必明告其误。

**可择之CMake节度**：
```bash
# 自定纳置之前缀（默：/usr/local）
cmake .. -DCMAKE_INSTALL_PREFIX=/usr

# 启调试译纂
cmake .. -DCMAKE_BUILD_TYPE=Debug

# 启译纂优化
cmake .. -DCMAKE_BUILD_TYPE=Release
```

### 四 译纂
```bash
make -j$(nproc)
```

`-j$(nproc)` 尽发CPU所有核芯并行译纂，以速其功。

### 五 纳置（可择）
```bash
sudo make install
```

纳置既毕，君可于端闱直书`ter-music`以启其器。

### 六 除纳（若已纳置）
```bash
cd build
sudo make uninstall
```

### 七 清营构之文
```bash
cd build
make clean
# 或尽除营构之舍
rm -rf build
```

### 八 译纂常患
**患一：波氏之库不可得**
```
解：纳pulseaudio-libs-devel（Fedora）或libpulse-dev（Ubuntu）
```

**患二：ncursesw之库不可得**
```
解：纳ncurses-devel（Fedora）或libncursesw5-dev（Ubuntu）
```

**患三：斐氏之首文不可得**
```
解：纳ffmpeg-devel（Fedora）或libavcodec-dev libavformat-dev...（Ubuntu）
```

### 九 营构脚本之用

本器备诸营构之脚本，以造异式之可执行文。详悉用法，请参阅：

- [营构指南](../BUILD_GUIDE.md) - 营构与封包的详悉说明

所持之格式：
- **AppImage** - 通于诸种麟纳克斯发行版
- **可携包** - 尽纳必需之凭藉库
- **RPM包** - 宜于Fedora/RHEL之统
- **DEB包** - 宜于Debian/Ubuntu之统
- **玲珑包** - 宜于deepin/UOS之统
- **Arch Linux包** - 宜于Arch Linux及其衍生之统

> **非 x86 架构（arm64、loong64 等）：** 凡非 x86 之架构（arm64、loong64 之属），其软件包悉托 OBS 构建服务器以总其成。详见 [OBS 构建服务器](https://obs22.odata.cc/package/show/home:Admin:app/ter-music)。

**测试服器之具：**
- **tools/start-server.py** - 交互相应之脚本，速启本地SMB/FTP/SFTP/WebDAV/HTTP服器，以验远程播乐之功。
  > 此乃 Python 之策，宜在 Conda 玄境中行。先置：`conda create -n ter-music python=3 && conda activate ter-music && pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r tools/requirements.txt` 然后行 `python3 tools/start-server.py`
  > 亦支符令行式：`python3 tools/start-server.py --protocol http --port 8080 --path /music/share` 或 `python3 tools/start-server.py --protocol sftp --port 2222 --username test --sftp-authorized-keys ~/.ssh/authorized_keys`

## 卷五 施用之法
### 一 启其器
**若已纳置**：
```bash
ter-music
```

**若未纳置，直从营构之舍运行**：
```bash
cd build
./ter-music
```

### 二 符令行之参数

不附子目而径行 `ter-music` 者，仍入文墨之界（与旧无异）：

```bash
ter-music [OPTIONS]

选项：
  -o, --open <path>    启器时直开指定乐籍目录
  -d, --debug          启调拭志录（录于 ter-music-debug.log）
  --frontend <模式>    播弄之途：remote（默，经 D-Bus 付于核心）
                       或 local（进程内播弄，留作回归基线）
  --attach-only        不自动拉起核心：无核心在行时以退去之码 3 终
  --bus <名称>         指特定核心／实例之总线名（默指主实例）
  --lang <语言ID>      本次行止用此语言（如 zh_CN、en_US、zh-PQ），不写入节度
  -h, --help           显助益之文
  -v, --version        显版本之文
  tui [path]           明入文墨之界
```

**示例**：
```bash
# 启器时开吾之乐籍文件夹
ter-music -o ~/Music

# 开远程FTP音声目录
ter-music ftp://user:pass@host/path/to/music

# 开远程WebDAV目录
ter-music --open http://webdav-server/music

# 显助益之文
ter-music --help
```

#### 二之一 CLI 模式

凡首参为下列诸目之一者，皆入CLI模式。CLI之令皆薄客也，凭D-Bus而与当下主 `org.mpris.MediaPlayer2.ter_music` 之**播弄核心**（或由 `daemon start` 所启，或由文墨之界拉起；惟 Linyaps 封缄之包得以 D-Bus 按需唤起，他般封缄不随包予唤起之文）相语，故任于何端闱、脚本、轩窗营理之捷键皆可行之。其中 `play` 兼为前端：于自进程之中扫描其径，而以所成之队列付于核心。

| 令目 | 所司之事 |
| --- | --- |
| `play [PATH] [--index N] [--mode MODE] [--no-daemon] [--foreground] [--quiet]` | 于本进程扫描其径，以队列付于核心而播之；无核心在行则先拉起核心（`--foreground` 为前台核心，`--quiet` 存以兼容） |
| `pause` / `resume` / `toggle` / `stop` / `next` / `prev` | 基础传输之御 |
| `seek <+SECONDS\|-SECONDS\|mm:ss\|N%>` | 相对、绝对或按百分率而跳转 |
| `volume [0-100\|+N\|-N]` | 查询或设音量 |
| `speed [0.5-3.0]` | 查询或设迅疾之度 |
| `mode [NAME\|0-16]` | 查询或设播弄之制（如 `list_repeat`、`folder_shuffle_repeat` 之定名） |
| `karaoke [off\|soft\|full\|0-2\|cycle]` | 询或设汰聲之等（`cycle` 则 0→1→2→0 迭进）；惟兩聲並列者有效，樂聲不改 |
| `show [OPTIONS]` | 出当下之信息块（基本信息／文书封面／音程／歌辞二行） |
| `queue list [--offset N] [--count N]` / `queue clear` / `queue shuffle` / `queue remove <pos>` / `queue append <path>[,<path>...]` / `queue insert <pos> <path>` | 察改核心所执之活队列 |
| `config get [KEY]` / `config set <KEY> <VALUE>` / `config reload` / `config reset --yes` | 读改核心节度（键名同 `config.xml`；`reset` 必带 `--yes`） |
| `lyrics [--lines N]` | 当前歌辞之上下文，或至多 N 行带时之辞 |
| `track` | 当前曲目之人读元数据 |
| `cover [--width N] [--height N] [--charset braille\|ascii\|half]` | 当前曲目之文书封面 |
| `keys list\|check [--json]` | 只读之键位审，无需核心在行 |
| `daemon start\|foreground\|stop\|restart\|status\|reload` | 播弄核心之管摄（`daemon stop --all` 兼停以 `--force` 所启之次实例） |
| `version` / `help` | 版本／助益（`help all` 出全量助文） |

`show` 之选项（用之，则一时盖过节度所存之设）：

| 选项 | 所司之事 |
| --- | --- |
| `--json` | 出全JSON之快照（`Info.GetInfo`） |
| `--watch[=MS]` | 实时焕新之视（默500千分秒，`Ctrl+C`退之，必在TTY） |
| `--one-line` | 单行而出，宜于状态之栏 |
| `--full` / `--compact` / `--preset full\|compact\|custom` | 信息显明之预设 |
| `--fields a,b,c` | 基本信息之域：`state,mode,index,queue,title,artist,album,format,path,volume,speed` |
| `--cover` / `--no-cover`、`--cover-size WxH`、`--charset braille\|ascii\|half` | 文书封面之选项（宽4-40列，高2-20行） |
| `--progress bar\|time\|percent\|time+percent`、`--no-progress` | 音程之行式 |
| `--lyrics 0\|1\|2` | 歌辞之行：无／仅当前句／当前及其次 |
| `--width N` | 输出之修广（默取端闱之广） |
| `--bus NAME` | 指特定实例之总线名（默指主实例） |

**退去之码：** `0` 成；`1` 用法有误（含未知之助目）；`3` 无实例在行；`4` D-Bus不可用；`5` 为实例所拒。

**助文与分页：** `ter-music help`、`--help`、`-h` 出简短之概。`ter-music help <令>`——或于解析选项之令（`play`、`show`、`daemon`、`queue`、`config`、`lyrics`、`track`、`cover`、`keys`）用 `<令> --help`——出一级之详；`ter-music help <令> <子令>` 与 `<令> <子令> --help` 出二级之详；`ter-music help all` 出全量助文。未知之助目以退去之码 `1` 终。

助文依端闱之广而折行。stdin 与 stdout 皆为 TTY 且文逾一屏者，启内置之分页：`空格`／`PageDown` 下页，`b`／`PageUp` 上页，`j`／`k` 逐行卷之，`g`／`G` 至首／尾，`q` 退。非 TTY 之出、`TERM=dumb` 及 `TER_MUSIC_NO_PAGER=1` 者，径出全文，不生 ANSI 转义之序。

**CLI 择语：** CLI 本地之助文、谬误、表列与提示，依 `--lang` > `TER_MUSIC_CLI_LANG` > 节度之 `ui_language` > `LC_ALL` > `LC_MESSAGES` > `LANG` > `zh_CN` 取之（详 §5.9.1）。`--lang` 与 `TER_MUSIC_CLI_LANG` 可置于命令行之任意处；`C`、`POSIX` 与空 locale 视为无偏好，仍用节度。`--lang` 所指语言包未纳置者，示警而回退 `zh_CN`。`show`、`track`、`lyrics`、`cover` 之正文由核心依其 `ui_language` 渲染，`--lang` 惟改 CLI 自身之辞。

**示例**：

```bash
# 启背景播弄，即归符令之界
ter-music play ~/Music

# 播单曲（其所在之目录即为曲帙）
ter-music play ~/Music/album/01.flac

# 当下曲目之文：点阵封面、音程条贯与当前／次句歌辞
ter-music show

# 单行之状态，宜于状态栏，每秒焕新
ter-music show --one-line --watch=1000

# 供脚本用之原始JSON
ter-music show --json | jq -r '.track.title'

# 传输之御
ter-music next
ter-music seek +10
ter-music volume +5
ter-music mode shuffle_repeat
ter-music karaoke soft    # 微汰其聲，居中者存其概

# 背景役使之管摄
ter-music daemon start --open ~/Music
ter-music daemon status
ter-music daemon reload      # 重读 config.xml（信息显明之设、音量等）
ter-music daemon stop
```

注意：

- 不附子目者，`ter-music <path>` 仍入文墨之界。欲开恰名 `play`／`show` 之目录，当用 `-o ./play` 或 `ter-music tui play`。
- `daemon start --open <目录>` 不复令核心扫描：今先启核心，再由此CLI进程（为前端）扫描其目录、成其队列而下付——与 `ter-music play <目录>` 同一途也。不带 `--open` 之 `daemon start` 惟启一空闲之核心，队列由随后接入之前端下付。
- 总线之名同时惟许一实例执之；核心在行之时，`daemon start` 拒不再启（欲以次实例强启者，可用 `--force`）。前端（文墨之界与符令行）皆客也，可并存多者。次实例不隐：`daemon status` 以 stderr 历举其 pid 与总线之名，`daemon start --force` 即报新创之名，欲御之者，以 `--bus <名>` 缀于子命令之后（如 `ter-music daemon stop --bus <名>`），欲尽停之则以 `daemon stop --all`（主实例与诸次实例一举而尽）。
- 核心收尾之际，声未绝则名不释：必待播弄之线程既退、后枢之流既毁，乃释总线之名。故 `q` 之后旋启前端者，所见者此将退之核心，不复别启其二——向者名先释而声犹在，遂有二核心并播一乐之患。其代价者，`daemon stop`／`restart` 见实例之亡，迟于旧时，至多迟一播弄线程收尾之限（二秒）加后枢流毁之顷；然符令行待其离线的度（五秒／十秒）犹足以容之。
- `daemon stop` 于在行之文墨之界则拒之，非 `--force` 不得终也。
- 核心所执之会话总线既亡，则核心**自退**：总线既亡，前端无可复联，犹据声器而不舍者，徒成一莫能止之播弄之进程耳。若核心自始未得总线（托管者于无会话总线之境启之），则不在此列。

#### 二之二 Linyaps（如意玲珑）封缄之态

ter-music 以 Linyaps（如意玲珑）之包纳置者，其可执行之文居于应用容器之内，宿主无 `ter-music` 之令（玲珑不导出可执行之文于 `$PATH`），故当假 `ll-cli` 而行符令。每行一次，皆入同一应用容器，是以符令、文墨之界与背景役使共一会话总线、一节度之文、一组 D-Bus 接口。

```bash
# 诸符令皆如是行之
ll-cli run cn.org.huansoft.ter-music -- ter-music show
ll-cli run cn.org.huansoft.ter-music -- ter-music pause
ll-cli run cn.org.huansoft.ter-music -- ter-music play ~/Music

# 容器既行之时读取其状（说见下）
ll-cli enter cn.org.huansoft.ter-music -- /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music show

# 交互端闱之便（置于 ~/.bashrc）
ter-music() { ll-cli run cn.org.huansoft.ter-music -- ter-music "$@"; }
```

**背景播弄。** 自离之进程（`ter-music daemon start`）将随容器而收，故于沙箱中禁之：`daemon start` 与 `play` 改为请会话总线唤起背景之播弄者，不可唤起，则退而以具列命令之告示示人。可循之途有三：

| 途 | 用之法 | 所成之事 |
| --- | --- | --- |
| D-Bus 按需唤起 | `ter-music play <path>` 或 `ter-music daemon start` | 会话总线于宿主行 `ll-cli run cn.org.huansoft.ter-music -- ter-music daemon foreground --no-autoplay`，而符令归于彼 |
| systemd 用户役使（常驻） | `systemctl --user enable --now cn.org.huansoft.ter-music`（于宿主行之） | 播弄之器随会话而启，常驻于背景，乐声不绝 |
| 前台运行 | `ll-cli run cn.org.huansoft.ter-music -- ter-music play <path> --foreground` | 于前台而播；符令行一日未毕，则容器一日存（宜于 tmux／screen） |

注意：

- 容器之内不达 `systemctl --user`，故役使必于宿主之端闱启之；`play` 与 `daemon` 察知沙箱者，亦印此提示。
- `ter-music daemon stop` 如常而行，终其实例；事毕容器见收，故 `ll-cli ps` 不复列其应用。
- `ll-cli run` 于诸般失败之码，尽以 `255` 为归；宿主之脚本宜改读 `ter-music show --json` 之 `"running"` 字段，而不恃退去之码。
- 若应用之器**已行**（背景之役使，或文墨之界），则 `ll-cli run … -- <符令>` 尽以其所出归于*方行之容器*之常出，端闱遂空无一字。可假 `journalctl --user -u cn.org.huansoft.ter-music` 观之，或改由 `ll-cli enter`（端闱犹接）：
  `ll-cli enter cn.org.huansoft.ter-music -- /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music show`。
  所入之境不传 `DBUS_SESSION_BUS_ADDRESS`，符令乃自寻会话总线（先试 `$XDG_RUNTIME_DIR/bus`，次试 `/run/user/<uid>/bus`）；若明设其址，则明设者为先。
- 宿主之径（`$HOME`、`/tmp`、`/media`）于容器之内如故可见；所提之封面，凭 `mpris:artUrl` 为宿主之器所读。
- 节度之文、曲库与会话存于 `$XDG_CONFIG_HOME/ter-music`。若玲珑之运行时重定 XDG 之变数（见玲珑之 FAQ「应用数据保存到哪里」），则落于 `~/.linglong/cn.org.huansoft.ter-music/…`，与 deb 之装各不相犯。
- `--watch` 须有端闱（用 ANSI 光标之制），勿经管道而行，宜于端闱之中直行之。
- `Info`／`Control` 即寻常会话总线之役，故宿主之器（`gdbus`、媒体之件、`busctl`）可视可御其玲珑之实例，与寻常之装无异。

#### 二之三 前端与核心

Ter-Music 分为二役，二者经会话总线而相语：

| 役 | 谁行之 | 所掌之事 |
| --- | --- | --- |
| **核心**（播弄之役） | `ter-music daemon start` ／ `daemon foreground` | 音声之器、播弄之状与音程、传输之令、音量／迅疾／播弄之制、卡拉OK汰聲、**奉行前端所授之径表**、当下曲目之文（歌辞、文书封面、频谱）、节度之文、前端之登记与心跳 |
| **前端**（文件之统与内容） | 文墨之界（`ter-music`、`ter-music tui`）、符令行（`ter-music play/show/…`）及他客 | 曲库（SQLite）、扫描与元数据、曲帙之内容、自定曲帙、珍存／往迹／目录往迹、排序／筛择／搜求、远方之源与其下载之藏、文墨之界 |

核心绝不扫描目录、绝不藏曲库、绝不解析远方之URL：其所播者惟**本地之径**，次第由前端所授。前端绝不启音声之器：其所绘者，乃自 D-Bus 读回之状，而以内容付于核心。

**启其器。** 无核心之时径行文墨之界，将自拉起一核心，而以内容（`-o` 所指定之目录，或所复之上次会话）付之，故日常之用无异：

```bash
ter-music                 # 启文墨之界（无核心者，自拉起一核心）
ter-music -o ~/Music      # 同上，且先开一目录
```

须核心已存者（用于脚本，或不欲暗中启播者），当用 `--attach-only`：

```bash
ter-music --attach-only   # 无核心在行，则以退去之码 3 终
ter-music --bus org.mpris.MediaPlayer2.ter_music.instance12345   # 指所接入之实例
```

**退其前端。** 闭文墨之界止不止播弄，决于前端自守之节度副本（镜像）里 `preferences.stop_core_on_exit`（`0`/`1`，默 `1`；可于设置页之开关易之，或于 `config.xml`、经 D-Bus `Config.Set`／`ter-music config set` 设之）。副本者，启器之时已取之于策，设置页一有改易亦即时下于核心，故退之际不暇复问核心。默既启，前端凡退之道（`q`、F9、Ctrl+C、SIGTERM、端闱之断）皆发 `Control.Quit`：核心先籍会话／resume 于策，乃止乐而退。置 `0` 则前端惟发 `Control.Detach`——核心仍行仍播，乐声不辍，`ter-music show` ／ `ter-music next` 于任何端闱皆能御之。然此二者，必其前端实已接入核心（握手既成，执 `Attach` 之凭符）乃发；未尝接入者，退时一札不传，绝不妄动他人所御之核心。别有所谓 `core_exit_when_no_frontend` 者：若欲最后一个前端既去而核心自退，则于 `config.xml` 中设为 `true`（或经 D-Bus `Config.Set` 设之）——前端尽去而后逾十秒之宽限，核心乃退；若核心自始未有前端接入，则终不退出。又：核心所执之会话总线既亡，亦自退——其时已无前端可联故也。

**断而复续。** 核心亡时（崩溃、见 `kill`、会话注销），前端不退：入于断线之状，按指数退避而重试（1／2／4／8／15／30 秒），一有核心应答即复接入，并补推其内容之队列。于文墨之界中按 `R`，可即重启已亡之核心而重推其队列。

**相容之度。** 总线之面为 `api_version 7`。第七版徙接口之名，自 `org.yxzl.ter_music.*` 更为 `cn.org.huansoft.ter_music.*`；此非增益，乃破故也——旧客触新核心，但得 `UnknownInterface`；新客触旧核心，并 `Control.Attach` 而不可即。故核心与前端必同时并易，不能偏其一。其方法之面，犹第四、五版累增之旧（第六版增益卡拉OK之目）。内容之接口（`Playlist`、`Library`、`Favorites`、`History`、`DirHistory`、`Remote`）已撤：此皆前端之职也。他客当用 `Lyrics`、`Info`、`Control`、`Queue`、`Config` 五接口，详见 [API_DBUS_en_US.md](../API_DBUS_en_US.md)。

### 三 文界局度
主界之局，今以**分栏之树**为定：每局皆为一二叉之树，叶者窗格之类也。分栏之节，剖其矩为二子，故局必为强制平铺，铺满主区（终端去底部菜单栏后之全域）——不许空白、不许缝隙、不许重叠。比例以千分比为度，终端伸缩则按比例重算，形制不漂。旧自由矩之文仍可读，且自动适配，不复全份见拒。

可用之格凡六：

- `playlist`：乐目曲帙／播弄队列（Tab 易视）
- `info`：播弄之状、当前曲目之文、音声之数与字符封面；其文整块居中于格
- `controls`：音程条贯与播弄控御之键
- `spectrum`：音声柱状之谱
- `lyrics`：当前歌辞之行
- `vinyl`：黑胶之戏——彩色半块碟面、旋转封面圆标与彩色频谱柱，色不可用则退字符之戏（自歌辞格中析出）

黑胶与歌辞二格有定退之则：

- `lyrics` 与 `vinyl` 可并立于一局：曲有歌辞则二格各陈其事；曲无歌辞则
  **歌辞格暂退**（去其叶于解析之树，余格补其位），更至有歌辞之曲则自还
  （其还自至，不待易视，亦不待改其尺寸）；局与色皆不改；
- 局中惟有 `lyrics`，则无歌辞之曲仍于该格退显黑胶之戏；
- 局中惟有 `vinyl`，则无论有歌辞与否，皆显黑胶之格。

彩色黑胶须颜色与色对俱足：碟面以定色沟槽，频谱柱按声之高低分绿、黄、红三档，中心为封面内切之圆，随碟五秒一周；封面不可得则仍绘彩色碟面与柱，中留纯色。单色之端、ASCII 之界或色对不足者，仍守旧字符之戏，信息、控制二区封面亦守旧预算。

**择局之法**：`F2` → `界面外观` → `界面布局`（`←`／`→` 循环，`Enter` 开全目）；`重新加载布局` 复读布局之文。所选存于 `~/.config/ter-music/ui-layout`，乃前端本地之好，不书核心 `config.xml`，亦不经 D-Bus 同步。改一格之前景、背景（`界面外观` 之十二目）则书 `~/.config/ter-music/layouts.json`：同 id 覆之，惟本局是变，不通于他局；局若来自 `TER_MUSIC_LAYOUTS`，覆之不见容，乃自增为副本（`<局 id>-2`、`-3`……）而改用之。

**布局之文**：

- 内置预设：`<prefix>/share/ter-music/layouts/layouts.json`（源码树为 `data/layouts/layouts.json`）
- 用户之覆：`~/.config/ter-music/layouts.json`（同 `id` 者覆内置）
- 开发之覆：`TER_MUSIC_LAYOUTS=/path/to/layouts.json`

内置七局：`classic`、`vinyl`、`karaoke`、`visualizer`、`focus`、`stage`、`neon`。`classic` 为默，存旧五格之制；七局皆逐格设色，同局之内无二格同色，底皆透明（`-1`），色自二百五十六而降于八、终归黑白而不混；`neon` 其色最艳。歌辞与黑胶并置者，曲无歌辞则歌辞格暂退，余格补位，更至有歌辞之曲则其还自至，局与色俱不改。凡几何、色、未知之格、空洞、重叠及非 guillotine 之旧局，皆自动适配为强制平铺，不复全份见拒；重载之提示报其适配之数。窗格之文（含信息格之专辑封面）严裁于边框之内（不犯其框）；格小不能容者，惟见其底与框。信息之格随其广狭自陈：广则元文与音数并列、封面居右；狭而高则封面居上，元文、音数在下；封面与边框之间必留空列，地不足则隐其封面。其域由节度→信息显示所主：惟用格所能容者，闭其域则行减，闭“制式”则音数之块亦隐。

布局编辑器（`F2 → 界面外观 → 编辑布局 / 新建布局`）直治分栏之树：方向键按位择格，Tab 循环，`[`／`]` 调其宽（就近之左右分栏）、`{`／`}` 调其高（就近之上下分栏），PgUp/PgDn 调宽五厘，`=` 复其半，`|`／`-` 分栏，`x` 删除，`t` 易类，`c` 改色，`w` 交换，`Enter` 开操作之目，`?` 见帮助，`S` 存之，`Esc`／`q` 返。凡操作皆守强制平铺，无存时几何校验之拒。

其详制、色律与自定之例，见 [UI_LAYOUTS_en_US.md](../UI_LAYOUTS_en_US.md)。

### 四 常操之法
#### 焦点迁转
| 键符 | 所司之事 |
| --- | --- |
| `C` | 迁焦点于控御之区 |
| `L` | 迁焦点于曲帙之区 |
| `Ctrl+L` | 进入/退出歌词定位模式（然后用↑/↓导航） |
| `Tab` / `Shift+Tab` | 切换文件浏览/播弄队列之视 |

**注**：按 `Ctrl+L` 进入歌词定位模式；然后用 `↑`/`↓` 导航歌词，`Enter`/`Space` 跳转到选中歌词的播放位置。再次按 `Ctrl+L` 退出。从任意焦点模式均可使用。

#### 曲帙区之操（焦点在乐目曲帙）
| 键符 | 所司之事 |
| --- | --- |
| `↑` / `↓` 或 `j` / `k` | 上下择曲 |
| `Space` / `Enter` | 播所选之曲 |
| `O` / `o` | 开新乐籍文件夹 |
| `F` / `f` | 以所选之曲入珍存之帙 |
| `a` | 以所选之曲入播弄队列 |
| `A` | 以所选之曲入自定曲帙（弹出择单） |
| `i` | 插入所选之曲为播弄队列之次曲 |
| `I` | 追加文件夹中曲目至当前曲帙 |
| `d` | 从播弄队列移除 |
| `D` | 清空整个播弄队列 |
| `J` | 在队列中下移（重排） |
| `K` | 在队列中上移（重排） |
| `S` 或 `/` | 啟音聲搜求之術，憑拼音字首檢索曲名歌者 |
| `M` | 切换音乐库浏览器（按艺术家/专辑/流派浏览） |
| `Tab` / `Shift+Tab` | 切换文件浏览/播弄队列之视（两键同功） |
| `n` | 下一曲 |
| `p` | 上一曲 |
| `h` | 显播弄往迹弹出窗（近10首） |
| `1`-`5` | 快速设播弄之制 |

> **注：** 音乐库览器内之键导（方向键/Enter深入）今主要凭鼠迹交互；键控之能有限。`M`键启闭，`Esc`退之。

#### 控御区之操（焦点在控御之栏）
| 键符 | 所司之事 |
| --- | --- |
| `←` / `→` | 左右择控御之键 |
| `Space` | 启当前所选之键 |
| `,` | 退五秒 |
| `.` | 进五秒 |
| `Ctrl+L` | 进入/退出歌词定位模式 |
| `-` / `_` | 减音量 |
| `=` / `+` | 增音量 |

**控御键诠解**：
| 键名 | 所司之事 |
| --- | --- |
| `<<` | 上一曲 |
| `Play/Pause` | 播/停 |
| `>>` | 下一曲 |
| `Stop` | 止播 |
| `Mode` | 切换播弄之制（开弹出菜单，按Enter从17种中择） |
| `Speed` | 迁转迅疾之度（开弹出菜单：迟 → 常 → 稍疾 → 疾 → 倍 → 三倍） |
| `Progress` | 音程条贯（显当前播弄之进度） |
| `Volume` | 音量节度（开弹出滑动条，显当前音量百分占比） |

#### 歌辞区之操（焦点在歌辞之域）
| 键符 | 所司之事 |
| --- | --- |
| `↑` / `↓` | 上下择歌辞之行 |
| `Ctrl+L` | 退出歌词定位模式，复其滚转 |

#### 鼠迹之操

通界皆应鼠之左键、右键与滚轮。一鼠之役惟归**一**主——或方开之幕，或指针所在之格（详下「幕与择单」），断无两处同受之理。

| 所在 | 单击 | 轮之一格 |
| --- | --- | --- |
| 底下功能键之栏 | 发其键之事，与亲按其键无异 | 无所为（然其役已于此销之） |
| 曲帙之格 | 择其行；一击则弄其曲，于文件之树则如 `Enter` 之启闭其目录 | 所选上下迁一行；索得之目、播弄之队列、文件之树皆同此，且**不**发播弄 |
| 乐库之览（开则代曲帙之格，`M` 启闭） | 择其目；再击其所择或双发之则入其内；`[S]`／`[R]` 之行致扫描重扫 | 迁其所择（与 `↑`／`↓` 同） |
| 控御之格 | 按其光标所在之钮；音量之钮与音程之行惟移其焦，与键无异 | 音程之行：进退五秒；音量之钮：损益百分之五；迅疾之钮：历预设之速而周流不殆；余钮——汰聲之钮与焉，其等惟由弹层择之——无所为 |
| 歌辞之格 | 跃至光标所在之一句 | 进至次句／却至上句（不改歌辞定位之启闭） |
| 信息、频谱、黑胶三格 | 其处无定之事（役已销之，故不漏入他格） | 无所为（销之） |
| 子视之侧栏（节度、往迹、曲帙、珍存、关于、助览、语音） | 择其目；再击其所择或双发之等于 `Enter` | 迁侧栏之所择 |
| 子视内容之区 | 择其行；再击其所择之行或双发之等于 `Enter` | 迁内容之所择。节度之视随其分区而异：或任事之目，或键位之目，或远程之目；均衡之部惟历**频带**而不移增益——轮未尝改其数；助览之部则卷其文进行一行 |

高亮随指针而迁：于内容之区滚、击者，并其键盘之焦而移焉，故目所见之明标，即次一 `Enter` 所加者也。
无所迁者二处：关于之视，其内容惟可读之文；语音之视，侧栏惟一「返回」之目——此二处之轮但收之，不迁任何选中。

**幕与择单（向者一役两受之弊）**。幕方开之际——控御栏之弹出菜单（播弄之制、迅疾之度、音量、汰聲之等）、节度页之择目幕（兼颜色、均衡预设与可多选者信息显目）、局度编辑之模态菜单——凡鼠之役皆归于幕：

- 轮惟滚其幕，不问指针在屏之何处；
- 击于幕内则择其目；再击其所择或双发之等于 `Enter`（多选之幕则等于 `Space`，勾其目也）；
- 击于幕外惟敛其幕。此一击没焉，绝不及幕后之格，故不复有「一击既敛其幕，又弄其下之行」者矣。

文字之入皆模态也（搜索、扫目录、局改名、键之捕获、数色之入）：其中鼠役一切弃之，误滚不能败其方书之文。不察鼠之兼键——按 Shift/Ctrl 而击与常击无异。惟请轮之上下，横向之轮与指针之移（悬停）皆不上闻于程序。

**右键之单**。右击则于指针之处启一单，其位必尽在屏内（贴边则上翻／左翻，末行提示之栏不覆）：

- 一 overlay 为限：控御栏之弹单、节度之择目幕、局度编辑之模态幕方张，右击惟敛之而吞其役，不更叠单；单方张时右击（或击其外）惟敛而吞之，绝不透于后格；
- 单中左击者择，再击所择或双发者行，轮者移其择，子单之目侧启其次；`↑`／`↓`（与弹单同键）、`Enter`、`Esc`、`→` 可御；他键则敛而吞之，不落于视图；
- 事不可行者其目禁（队空、指针之下无曲、无歌单），今所行者著勾（播弄之制、乐库之览、列表之式）；
- 曲帙之单举队之治、珍存、入歌单、启目录、索、往迹、控御、播弄之制与页面之迁，且必有一目易其文件／队列之式；右击终不发播弄，惟择其行而后启单；
- 诸子视（节度、往迹、曲帙、珍存、助览、语音）各以其键之行列单；局度编辑者右击即启其旧「窗格之操作」单于指针；信息之页（F6）惟「科乐美流光」一目，主界信息之格不设右键之单；控御、歌辞、频谱、黑胶、信息之格与底下提示之栏皆不设右键之单，其左击／轮之节已尽其事；
- 单不塞事件之环，每轮重为合成，故播弄、动画与流光不绝；凡与单相交之格方刷，必先入虚屏，复以单之诸层压还，终一 doupdate，故终端终不见单域为格纹所覆之帧；其不与单交者，仍循 wrefresh 之旧。易视、改尺寸、重载配置／语音、重建局度，皆先敛单而后重绘；队列、往迹、曲库之行既变，则行之事告以「列表已变化，请重试」，不妄下于他行。

#### 功能键（全域可用）
**功能键（F1-F9）**
| 键符 | 所司之事 |
| --- | --- |
| `F1` | 归主界 |
| `F2` | 开节度之视 |
| `F3` | 开播弄往迹之视 |
| `F4` | 开曲帙营理之视 |
| `F5` | 开珍存之视 |
| `F6` | 开关于之视 |
| `F7` | 迁语言之视（开语言选择界面） |
| `F8` | 开助益之视 |
| `F9` | 退其器 |

**备用水数键（Esc后三息内输入）**
| 键符 | 所司之事 |
| --- | --- |
| `Esc` + `1` | 归主界 |
| `Esc` + `2` | 开节度之视 |
| `Esc` + `3` | 开播弄往迹之视 |
| `Esc` + `4` | 开曲帙营理之视 |
| `Esc` + `5` | 开珍存之视 |
| `Esc` + `6` | 开关于之视 |
| `Esc` + `7` | 迁语言之视（开语言选择界面） |
| `Esc` + `8` | 开助益之视 |
| `Esc` + `9` | 退其器 |
| `q` | 退其器 |

### 五 播弄之制诠解

Ter-Music 凡17种播弄之制，分为5组，基础者始终可用：
按Enter键开控御栏之Mode弹出菜单以择之。

#### 基础之制（始终可用）
| 制名 | 诠解 |
| -------- | ---- |
| `Sequential` | 序进，播至曲帙之末则止 |
| `Single Repeat` | 单曲回环，重播当前之曲 |
| `List Repeat` | 全帙周流，播毕一轮则从头复始 |
| `Shuffle Once` | 乱序一度，不重复乱播列表中所有曲目 |
| `Shuffle Repeat` | 乱序杂陈，随机择下一曲 |

#### 进阶之制（需音乐库元数据）
| 分组 | 制式 | 诠解 |
| ---- | ---- | ---- |
| `Folder` | 序进/回环/乱序/乱序杂陈 | 限当前目录 |
| `Album` | 序进/回环/乱序/乱序杂陈 | 按专辑标签 |
| `Artist` | 序进/回环/乱序/乱序杂陈 | 按艺术家标签 |

**注：** 进阶之制用SQLite音乐库数据库检索元数据。于节度 → 播弄之制 → "启用高级播放模式"中开启。

### 六 迅疾节度之能

Ter-Music具迅疾节度之能，可依需调音程之迟疾：

| 迅疾之档 | 诠解 |
| -------- | ---- |
| `0.75x` | 迟缓之度，宜细聆或问学 |
| `1.0x` | 常度，默认播弄之速 |
| `1.25x` | 稍疾之度，宜略速收听 |
| `1.5x` | 疾速，宜速览内容 |
| `2.0x` | 倍速，宜高效收听 |
| `3.0x` | 三倍速，至极之速，宜速回顾 |

**用之法：**
- 于制区，以`←`/`→`键选迅疾之钮，按`Space`键即可迁转速度
- 当前速度显于迅疾之钮（如"迅疾:1.50x"）
- 播弄中可随时迁转速度，音声无缝过渡至新速
- 默认迅疾可于节度菜单（F2）中配置

**术之解：** 迅疾节度凭斐氏FFmpeg之atempo滤镜而成，可变速而音调不变。

### 六之一 卡拉OK之制（汰聲）

卡拉OK之制者，削樂聲居中的人聲而存其群音，使学者得与原曲同歌。此乃**播弄之时**之治也：不返写于文件，盘中本聲未尝改一字节。

| 其等 | 节度之值 | 所为 |
| ---- | -------- | ---- |
| 勿汰 | `0` | 一不治之，样本直过 |
| 微汰 | `1` | 去中置频带者十之六，故犹有人聲如薄雾 |
| 盡汰 | `2` | 中置频带去至此术所能去之极 |

**其术如何（限频带而削居中者）：**

1. 中置之道 `m = (L + R) / 2` 以两段四阶Butterworth（每倍频廿四分贝）分之：低通于180Hz，高通于6kHz。
2. 惟此两拐点之间——人聲多居之——自中置道减去：`m' = m - α · (m - lo - hi)`；三等之 α 为 0 / 0.6 / 1.0。
3. 侧置之道 `d = (L - R) / 2` 一仍其旧，立体复成于 `L' = m' + d`、`R' = m' - d`。

所以必限频带者，正为下听与清越之声也。若尽削中置之道，则低音与钹镲同归乌有矣。播弄之际易其等，α 于三十毫秒之内渐移而不骤变，故不闻爆音；同时分频二段之延时线皆以当下之聲重新起测；输出惟样本逾数字满量程者乃以tanh软限之。

**其限也（不敢隐）：**

- 惟兩聲並列者行之。單聲之曲无中置侧置之可分，逐位直过（引擎中 `channels != 2` 即返）。
- 凡居中者皆与人聲同削——居中底鼓、军鼓、单声道铺满之吉他皆然。此术之固然，非可修之失。
- 双轨叠唱或大左右分置者不去，以其不在中置道也。
- 汰聲治解码之样本，序在均衡器**之先**；故均衡、频谱、可视化所见皆既治之聲。

**易等之门有四：**

- 主控之格：卡拉OK之钮显其等（如「汰聲:盡汰」）。焦点在钮而按 `Enter`／`Space`（或以鼠击之），则弹三者之择单；钮下狀行回报其实效者。
- 节度（F2）→「播弄之度」：曰「汰聲（卡拉OK）」，按 `Enter` 开同一择单。←/→ 不改此目，必由择单。
- 符令：`ter-music karaoke` 询其等；`ter-music karaoke soft`（或 `full`／`off`／`1`／`cycle`）设其等。
- D-Bus：`Control.SetKaraoke`／`Control.GetKaraoke`；或以 `Config.Set` 写 `preferences.karaoke_mode`（`api_version` 6；命名空间徙于第七版）。
- 其等存于 `config.xml` 之 `<karaoke_mode>`，次启自回灌于引擎；0–2 之外者钳归于0。

**同名之异指：** *karaoke* 在此器复有二义——逐字歌辞之highlighting（`Lyrics.GetDocument` 之 `timing="chars"`），与名曰 `karaoke` 之局度预设（歌辞优先之排布）。三者非一事，本节所言惟汰聲而已。

### 七 歌辞显明
本器通自动加载LRC式歌辞之文，且支持内嵌歌词：
- **内嵌歌词优先**：器先读取音声文件中的内嵌歌词（FFmpeg/APE标签），其键有 `lyrics`、`LYRICS`、`lyrics-eng`、`UNSYNCEDLYRICS`、`©lyr`，兼覆 ID3 `USLT`／iTunes `lyrics-eng` 与常键；若不得，则回退加载外部`.lrc`文件
- 歌辞之文必与音声之文同置一目录
- 歌辞之名必与音声之名同，后缀为`.lrc`
- 例：`song.mp3` → `song.lrc`
- 器将随播弄之时，自动焕明当前歌辞之行
- **逐字（卡拉OK）之辉**：LRC有逐字／逐段之签者，按字而明；一行首尾俱有签者，于
  `[行首, 行尾)` 之间均分；旧式惟行首有签者，按「本行之初 → 下行之初」均分（末行
  仍用前段之时长）。行长而窗不能容者，仍以整行焕明
- 若不得歌辞之文，歌辞区存黑胶之戏（色可用则为色块，否则仍字符）；频谱之格仍显音声柱状之谱
- **切换歌词来源**：在歌词定位模式下按 `Tab` 可在内嵌/外置间切换

他器可经D-Bus歌辞接口读当前A/B两行歌辞，详说见
[Lyrics API (English)](../API_LYRICS_en_US.md)。

### 八 MPRIS 与歌词 API

译纂时启D-Bus（`libdbus-1`）者，器将于会话总线上登记MPRIS媒体会话：

- 主流轩窗之境（GNOME Shell、KDE Plasma、Cinnamon、Budgie之属）可见播弄之制与曲目之文。
- 若有乐集之面，则发 `mpris:artUrl`，轩窗媒体之件得显其面。
- 封面先读音声内嵌之图；无者，则按 `cover`、`folder`、`front`、`album`（不辨大小写，扩展名 `.jpg`、`.jpeg`、`.png`、`.webp`）寻同目录之封面。
- 提取之面统为受管之 `/tmp/ter-music-cover-*.jpg` 暂存，留最近十曲之MRU缓存，退器时净之。

同一D-Bus对象亦供开放歌辞接口：接口 `cn.org.huansoft.ter_music.Lyrics`，方法
`GetLyrics`、`GetDocument` 与 `SetSource`（于内嵌歌辞与外部歌辞之间切换），
信号 `LyricsChanged`。自 `api_version 5` 始，A/B之快照增 `char_sung`／
`char_total`，`GetDocument` 之每行增 `end`、`timing`、`char_total`，逐字之LRC
复有 `chars`（相对行首之毫秒数列），他客可依此作同式之逐字焕明。JSON之构与调用之例见
[Lyrics API (English)](../API_LYRICS_en_US.md)。

同一对象之路，复布四接口，俾他器得读曲目之文、文书封面与音程，且得御其器：

- `cn.org.huansoft.ter_music.Info`（唯读）：`GetInfo`、`GetTrackInfo`、`GetProgress`、
  `GetLyricsLines`、`GetCoverArt(charset, cols, rows)`、`GetDisplay(options)`
  （即 `ter-music show` 所出之文，毫厘不异）、`InstanceInfo`，及信号
  `InfoChanged`、`ProgressChanged`（每秒至多一发）与 `CoverChanged`。
- `cn.org.huansoft.ter_music.Control`：传输、跳转、音量、迅疾之度、播弄之制、汰聲之等，
  `ReloadConfig` 与 `Quit`。载入内容**不**在其列——内容由前端以队列之形下付。
- `cn.org.huansoft.ter_music.Queue`：核心之**径表队列**——
  `Set`/`Append`/`InsertAfter`/`RemoveAt`/`MoveUp`/`MoveDown`/`Clear`/
  `Shuffle`/`PlayAt` 与分页读取 `Get(offset, count)`，并广播 `QueueChanged`
  （条目之数、当下之标、版本之号）。惟受本地之径；一次所写不逾五百条，
  一次所读不逾千条。
- `cn.org.huansoft.ter_music.Config`：惟此一门可改核心之制。远方服务器之条目**不**在其中，
  乃前端所守（见下文）。
- `Info.GetInfo` 以 `core.api_version`（今为 `7`）与所备方法之目相质，客可先验其合否。
  第三版去 `Remote` 接口与 `track.is_remote` 之目，且凡路径之参惟受本地者；
  第四版以径表语义之 `Queue` 代内容之接口（`Playlist`、`Library`、`Favorites`、
  `History`、`DirHistory`）——内容归于前端。第五版增歌辞逐字之时
  （`char_sung`、`char_total`、`end`、`timing`、`chars`），皆为增量：新前端通
  第四版之核心（自退为整行焕明），第四版之前端亦通第五版之核心。
- `org.freedesktop.DBus.Introspectable` 与 `org.freedesktop.DBus.Peer` 俱已备，
  故 `busctl --user introspect`／`gdbus introspect` 可行。
- MPRIS之Metadata复载 `xesam:url`（恒为 `file://` 之URI，虽自远方而来者亦为本地之藏径）
  与 `xesam:trackNumber`；`OpenUri` 明答 `org.freedesktop.DBus.Error.NotSupported`，
  盖核心不复载内容矣。开卷当用 `ter-music %F`（桌面之入口）或 TUI。
- `CanQuit` 故守 `false`，使桌面媒体之件不得径杀其器；欲终之者，当用
  `ter-music daemon stop` 或 `Control.Quit`。

方法之全目与JSON之构，见
[D-Bus Info & Control API (English)](../API_DBUS_en_US.md)。

ter-music 以 Linyaps 包行时亦发此诸接口：容器用宿主会话总线，故宿主之器与包内符令所见之对象路径与接口同一。

### 九 节度之文
节度之文存于`~/.config/ter-music/config.xml`（XML格式，经libxml2解析；启器时迁至当前版本）。器初启时将自动创之（若有v1 config.json则自动迁之）。

**节度之项**：
- `default_startup_path`：默认启行之目录
- `auto_play_on_start`：启器时自动播弄（0/1）
- `remember_last_path`：记上次所临之目录（0/1）
- `show_album_cover`：显控制区之专辑封面，并为信息窗格封面之总开关（0/1；黑胶中心之标不与此开关相干）
- `show_lyrics_panel`：TUI 中已**弃用**。窗格之显隐今由布局预设定之；此键惟于 `~/.config/ter-music/ui-layout` 未有时，为初启映射之提示（`1` → `classic`，`0` → `focus`）。为通旧客，仍存于 `Config.GetAll`
- `default_playback_speed`：默播之速（0.75、1.0、1.25、1.5、2.0、3.0）
- `default_play_mode`：默播弄之制（0=序进、1=单曲回环、2=全帙周流、3=乱序一度、4=乱序杂陈……）
- `advanced_play_modes_enabled`：启进阶文件/专辑/艺术家播弄之制（0/1）
- `lyrics_alignment`：歌辞对齐之式（0=居左、1=居中、2=居右）
- `clear_history_on_startup`：启时清目录之迹（0/1）
- `resume_last_playback`：续从前之处播弄（0/1）
- `seamless_preload`：当前曲末预解下曲，达无隙播弄（0/1）
- `ui_language`：界语之ID。内置者三：`zh_CN`、`en_US`、`zh-PQ`（语包之名「中文（先秦）」；先秦文言、繁体，应用译名「端闈樂部」）；经 F7 语言视图所纳之第三方语言ID（如 `ja_JP`）亦可用。非法之ID回退 `zh_CN`。
- `volume_percent`：默音量之率（0-100）
- `audio_latency_ms`：输出时延（千分秒）
- `audio_backend`：音声后枢（0=自动、1=PulseAudio、2=ALSA、3=PipeWire）。自动之式启时历探诸枢；明择之枢倘实不可用，播时退就余枢并以状语告之，不默然而败
- `sort_mode`：排序之式（0=默、1=标题、2=艺术家、3=专辑、4=文件名）
- `cue_encoding`：CUE文字符编码（0=自动、1=UTF-8、2=GB18030、3=GBK、4=BIG5、5=Shift-JIS）
- `karaoke_mode`：卡拉OK汰聲之等（0=勿汰、1=微汰、2=盡汰，说在「六之一」）。惟兩聲並列者有效；越此范围者退归于0。此键随 `api_version` 6 而来（第七版徙接口之名，未尝动此键），而节度之文之版号**未**易（尚为9）——旧者见之不理，缺之亦以为0
- `core_exit_when_no_frontend`：无前端接入时，令核心自退（0/1，默 0）。
  置 1 者，最后一个前端既去、逾十秒之宽限，核心乃退
- `stop_core_on_exit`：前端退时止核心之播弄——前端改发 `Control.Quit`，
  不复发 `Control.Detach`，且必其前端实已接入核心（握手既成、执 `Attach` 之凭符）乃发
  （0/1，默 1——惟置 0 乃「闭文墨之界而乐声续于后」）。此键因节度契约版 8 而有，
  `config.xml` 根之 `version` 属性今书当前契约之数 `9`（旧书 `2.2`）；退而就旧版者
  不以为咎，然旧版不识此键，及其存节度也，复书属性为 `2.2` 并去此键，人所明定之值
  由是而失，他日复升级，乃以默 `1` 补之
- 键位之覆，藏于可选之 `<keybindings>` 段（节度契约 v9）。惟覆者见存；无条目之事，
  仍用其内置之键。其 XML 之例与键名之法则，见[十二 捷键速览](#十二-捷键速览)之
  「自定键位」
- 远程服器之连（SMB/SFTP/FTP/WebDAV/HTTP）**不**存于 `config.xml`：
  前端自藏于同目录之 `remote.xml`（服务器条目与密码密文，权为 0600），
  所下之曲藏于 `$XDG_CACHE_HOME/ter-music/remote/`
- 色采之节度分二层。通体之饰（24套预设主题 + 1个自定槽位，前后色彩配对保护）主侧栏、
  高亮、边框之前景、背景，书于 `config.xml`。诸格之色属于**布局**：**设置 → 界面外观**
  于六格（列表、信息、控制、频谱、歌辞、黑胶）各设前景、背景二目，读写当前布局之
  `panes[].fg` / `panes[].bg`，行末标 `[布局]` 或 `[主题]`，以见其色今由何层而定。
  自有主题基色之格（列表、控制、歌辞）未覆而改，仍易通体之饰，与旧制无异；余者皆书为
  布局之覆。详制见 [UI_LAYOUTS_en_US.md](../UI_LAYOUTS_en_US.md)
- TUI 布局之择：`~/.config/ter-music/ui-layout`（前端所书单行布局 id）；布局之文：内置 `layouts/layouts.json` 与可选之 `~/.config/ter-music/layouts.json`。择局不书 `config.xml`；色采之中，惟改格之色书用户布局文件（布局编辑器存盘亦书之）
- 均衡器：10段增益、前置放大、启/禁
- 信息显明之节度（主页信息窗格兼 CLI/D-Bus），可于**节度 → 信息显示**中厘定：
  - `info_preset`：预设（0=全、1=简、2=自定）
  - `info_fields`：基本信息诸域之位掩码（1=状态、2=制、4=序号、8=队列、16=标题、32=艺术家、64=专辑、128=制式、256=路径、512=音量、1024=迅疾；2047=尽有）。主页信息窗格惟用其所能容者（状态/制/标题/艺术家/专辑/队列/制式）；序号/路径/音量/迅疾止及 CLI/D-Bus，择域之幕标曰“仅 CLI”
  - `info_show_cover`：信息窗格与 CLI 之封面（0/1）；TUI 窗格兼须播弄之 `show_album_cover` 总开关
  - `info_cover_cols` / `info_cover_rows`：CLI 文书封面之修广，以字符之列、行计（4-40 / 2-20）
  - `info_cover_charset`：CLI 封面之字符集（0=点阵、1=ASCII、2=半块）
  - `info_show_progress`：出 CLI 音程之行（0/1）
  - `info_progress_style`：CLI 音程之行式（0=条贯＋时、1=时、2=百分、3=时＋百分）
  - `info_lyrics_lines`：CLI 歌辞之行（0=无、1=仅当前句、2=当前及其次）

器将自动存其节度，改之即生效。

### 九之一 语言包之制

Ter-Music 用 XML 之国际化（i18n）制。内置语言包在源码 `data/lang/`，纳置于 `TER_MUSIC_DATA_DIR/lang/`。

**语言包格式：**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<lang id="en_US" name="English (US)">
  <string key="general.yes">On</string>
  <string key="general.no">Off</string>
  <!-- ... 更多辞目 ... -->
</lang>
```

- 根元素 `<lang>`，`id` 为语言标识（如 "zh_CN"），`name` 为显名。
- 每辞为 `<string>` 元素，`key` 属性为键，内容为译辞。
- 键以点分：`模.子模.名`（如 `sidebar.settings.theme`、`menu.help`）。

**搜索优先序（高至低）：**

1. `~/.config/ter-music/lang/<id>.xml` — 用户自定
2. `TER_MUSIC_DATA_DIR/lang/<id>.xml` — 编译期安装前缀
3. `/usr/share/ter-music/lang/<id>.xml` — 系统全局
4. `<exe_path>/../share/ter-music/lang/<id>.xml` — 相对于可执行文件
5. `data/lang/<id>.xml` — 开发/运行目录
6. 源码 `data/lang/<id>.xml`

欲添新语言，依上制创 `<id>.xml`，置任一搜索路径（推荐 `~/.config/ter-music/lang/`）。该语言将自现于 F7 语言选择之视。

`ui_language` 不复限于内置之 `zh_CN`／`en_US`／`zh-PQ`：于 F7 择第三方语言者，存其ID；节度之验惟拒不可安全为文件名之ID。未纳置或验而不合者，回退 `zh_CN`。

**CLI 择语之序。** CLI 本地之辞（助文、谬误、表列与提示），自高而低如次：

1. `--lang <ID>`
2. `TER_MUSIC_CLI_LANG`
3. `config.xml` 之 `ui_language`（即 F7 之择）
4. `LC_ALL`
5. `LC_MESSAGES`
6. `LANG`
7. `zh_CN`

`--lang` 与 `TER_MUSIC_CLI_LANG` 可置于命令行之任意处（含子令之前），并依 locale 之名归一（`en_US.UTF-8` → `en_US`，`zh-*` → `zh_CN`；内置之 `zh-PQ` 仍作 `zh-PQ`）。`C`、`POSIX` 与空 locale 之值谓「无偏好」，仍循序而下，故节度之 `ui_language` 犹胜之。显指之语言包未纳置者，示警而回退 `zh_CN`。

`--lang` 惟改 CLI 自身之辞。`show`、`track`、`lyrics`、`cover` 所出之正文，由核心依其 `ui_language` 渲染；非改核心节度，则仍用文墨之界之语。

#### 八之二 tar.gz 语言包布之规

语言包亦可以 `.tar.gz`（或 `.tgz`）压缩包形布之，以便共享与一键纳置。于语言选择视中按 `A` 键纳之，按 `D` 键除已加之用户语言包。

**压缩包之内：**

| 文件 | 必需 | 说 |
|------|------|----|
| `lang.xml` | 是 | 语言数据文件（格式见九之一） |
| `help.txt` | 否 | 该语言之速启助文 |

唯名为 `lang.xml` 与 `help.txt` 者乃提，余者默弃。文件仅以基本名配之，可居 tar 包内任意子录。

**规格：**

| 属性 | 值 |
|------|----|
| 文件扩展名 | `.tar.gz` 或 `.tgz` |
| 归档格式 | POSIX/USTAR（标准 `tar` 格式） |
| 压缩法 | gzip |
| 单文件大小限 | 50 MB |
| 字符编码 | UTF-8 |
| 压缩级 | 任意（gzip 兼容即可） |

**制语言包：**

```bash
# 最小——仅语言数据
tar -czf mylanguage.tar.gz lang.xml

# 附带助文
tar -czf mylanguage.tar.gz lang.xml help.txt

# 文件可在子录中，唯基本名作用
tar -czf mylanguage.tar.gz some/dir/lang.xml some/dir/help.txt
```

**纳置路径：**

以语言视（`A` 键）纳之，所提文件置：
- `~/.config/ter-music/lang/<id>.xml` — 语言数据
- `~/.config/ter-music/help/help-quickstart-<id>.txt` — 助文（若含 `help.txt`）

语言 `<id>` 从 `lang.xml` 之 `<lang>` 根元素 `id` 属性读之。

**验核之序：**

程序纳包时行此验：
1. 拒非普通文件与非 `.tar.gz`/`.tgz` 扩展名
2. 提 `lang.xml` 析为 XML
3. 验根元素为 `<lang>` 且 `id` 非空
4. 拒覆内置语言（`zh_CN`、`en_US`、`zh-PQ`）之包
5. 行 50 MB 单文件大小限之检

验过，该语言即现于语言选择视。用户所加之语言，在界面上与内置者有明别。

**注：** 若 `~/.config/ter-music/lang/` 中已存同 `<id>` 之语言包，纳新 tar.gz 将默覆之。欲复误删之内置语言，请重装程序。

### 十 数据存贮之所
所有用户数据，皆存于`~/.config/ter-music/`目录之下：
```
~/.config/ter-music/
├── config.xml       # 节度之文（XML格式，libxml2解析，启时迁至当今之版）
├── library.db       # SQLite数据库（音乐库、珍存、曲帙、往迹）
├── remote.xml       # 远方服器之目（前端自有，核心不读）
├── lang/            # 用户语言包目录（覆盖内置翻译）
└── config.json.bak  # v1节度首迁之自动备份（如有）
```

**注：** 播弄队列不复落盘为文：队列乃内容，由前端于再启之时自曲库／上次所开之目录重建，游标则在启 `resume_last_playback` 时由核心复之。v1.0之JSON存储（config.json、独立favorites、history、dir_history、playlists/）已尽替以SQLite数据库library.db。v2.0初启时将自动迁移。

乐集之面不复存于 `~/.config/ter-music/`。所提之面乃受管之临时JPEG文，
在 `/tmp/ter-music-cover-*.jpg`，留最近十曲，退器时去之。

### 十一 常施用之程叙
**例：初用之法**
1. 启其器：
   ```bash
   ter-music
   ```
2. 按`O`开文件夹，输入君之乐籍目录路径，例：
   ```
   /home/yourname/Music
   ```
3. 器将扫目录中所有音声之文，显于曲帙之中
4. 以`↑` `↓`择欲闻之曲，按`Space`始播
5. 若有歌辞之文，将自动加载，于右侧同步显明
6. 以`,`与`.`可退/进五秒

**例：以曲入珍存之帙**
1. 于曲帙区择欲存之曲
2. 按`F`，底部状态栏将显"Added to favorites!"
3. 按`F5`可览所有珍存之曲
4. 于珍存之视中，可择而播之

**例：创自定曲帙**
1. 按`F4`入曲帙营理之视
2. 择"Create New Playlist"
3. 输入曲帙之名
4. 归主界，于曲帙中择曲，按`A`入曲帙

**例：览音乐库**
1. 按`M`入音乐库览器
2. 以`↑`/`↓`导航：首页 → 艺术家 → 专辑 → 曲目
3. 于艺术家按Enter览其专辑，于专辑按Enter览曲目
4. 于曲目按Enter即播
5. 再按`M`或按`Esc`归文件夹浏览

**例：管播弄队列**
1. 于文件浏览中择曲，按`a`追至队列
2. 按`Tab`切至队列视览有序之列
3. 以`J`/`K`重排，`d`移除，`D`清空
4. 于队列条目按Enter即播
5. 再按`Tab`归文件浏览

### 十二 捷键速览
| 分曹 | 键符 | 所司之事 |
| --- | --- | --- |
| **全域** | `q`/`Q` | 退其器 |
|  | `F1` | 归主界 |
|  | `F2` | 节度之视 |
|  | `F3` | 播弄往迹 |
|  | `F4` | 曲帙营理 |
|  | `F5` | 珍存之帙 |
|  | `F6` | 关于之视 |
|  | `F7` | 迁语言之视（开语言选择界面） |
|  | `F8` | 开助益之视 |
|  | `F9` | 退其器 |
|  | `Esc` | 归主界/退 |
| **焦点** | `C` | 焦点至控御区 |
|  | `L` | 焦点至曲帙区 |
|  | `Tab`/`Shift+Tab` | 切换文件/队列视 |
| **曲帙/览** | `↑`/`↓` 或 `j`/`k` | 择上/下一曲 |
|  | `Space`/`Enter` | 播所选之曲 |
|  | `O` / `o` | 开文件夹 |
|  | `F` / `f` | 入珍存之帙 |
|  | `a` | 追加至队列 |
|  | `A` | 入自定曲帙 |
|  | `i` | 插入为队列次曲 |
|  | `I` | 追加文件夹至曲帙 |
|  | `d` | 从队列移除 |
|  | `D` | 清空整个队列 |
|  | `J` | 在队列中下移 |
|  | `K` | 在队列中上移 |
|  | `S` 或 `/` | 啟拼音搜求之術 |
|  | `M` | 切换音乐库览器 |
|  | `n` | 下一曲 |
|  | `p` | 上一曲 |
|  | `h` | 显往迹弹出窗 |
|  | `1`-`5` | 速设播弄之制 |
| **控御** | `←`/`→` | 择控件 |
|  | `Space` | 启控件/开弹出菜单 |
|  | `,` | 退五秒 |
|  | `.` | 进五秒 |
|  | `-`/`_` | 减音量 |
|  | `=`/`+` | 增音量 |
|  | `R` | 重启播弄之核心 |
|  | `Ctrl+L` | 进入/退出歌词定位模式 |
| **歌辞** | `Ctrl+L` | 进入/退出歌词定位模式 |
|  | `↑`/`↓` | 择上/下一句 |

#### 自定键位

上表诸事，**除系统所守之键外**，皆可改其键。所守之键，不复列于节度之
快捷键表中：

- `F1`–`F9`：视图／退出之功能键
- `q` / `Q`：退
- `Tab` / `Shift+Tab`：文卷／队列选项卡之切换

其余动作既改，其旧诸键尽废；`Esc` 仍可取消／返。

**于节度之视改之**：按 `F2` 入 `节度 → 快捷键`，以 `↑`/`↓` 择事，按
`Enter` 而后按新键；`Esc` 罢录。同境之中已有他用者，拒而不纳，旧键如故。
`r` 复当前之项，`R` 复其全，`s` 存之（未存不落于文）。

**config.xml**：惟覆者书之，以动作之 ID 为序：

```xml
<ter-music-config version="9">
  <keybindings>
    <binding action="track.next" key="]"/>
    <binding action="track.prev" key="["/>
  </keybindings>
</ter-music-config>
```

系统所守动作之覆，行时则弃之（文件之条目仍存，`ter-music keys check` 记
其告警）；`Config.Set` 拒为新键，惟许 `"default"` 以清旧覆。

**键名之法则（v1）**：

- 单可印 ASCII 之字，辨其大小写：`a`–`z`、`A`–`Z`、`0`–`9`、`!`–`~`；
  空格必书 `space`。
- `ctrl+a` … `ctrl+z`，然除有歧者：`ctrl+c`（SIGINT）、`ctrl+h`
  （Backspace）、`ctrl+i`（Tab）、`ctrl+j`（LF）、`ctrl+m`（CR）、
  `ctrl+z`（SIGTSTP）。
- 命名之键：`up` `down` `left` `right` `home` `end` `pgup` `pgdn` `ins`
  `del` `backspace` `tab` `btab`（显为 Shift+Tab）`enter` `space` 及
  `f1`–`f12`。命名键与 `ctrl+` 之端皆用小写；可印之字仍存其大小写。
- 所不许者：`alt+…`、多键之序（如 `gg`）、非 ASCII 之文、裸控制之字、
  空名与过长之名。`Esc` 为系统所守，不可为绑定之值。

**审计之令**：`ter-music keys list [--json]` 列诸动作之境、名、现行之键、
默认之键、是否有覆及遮蔽之系；`ter-music keys check [--json]` 校节度之文，
遇未知之动作、非法之键、相冲之键或无默认键之动作，则退出之码非 0。当前
`$TERM` 偶缺 terminfo 之能，惟记告警而不为败，故一配置可适于诸端。

**复其默认**：去该动作之 `<binding>`（段中无条目则并去其段），于节度之视按
`r`/`R`，或以 `Config.Set` 发 `{"keybindings":{"动作ID":"default"}}`。

**兼容之要**：节度契约 v9 以前之器不识 `<keybindings>`，仍可启；然其存节度
也，键位之覆尽失。早于 v9 之前端亦不识 v9 核心 `Config.GetAll` 中之
`keybindings`，故宜前后端并升。欲存改绑之果，须用今之版本，于 v9 之文书之。

### 十三 端闱修广之调
本器通端闱窗牖修广之调，君改其大小时，器将自动重置局度，重绘文界。

### 十四 退其器
退之之法有三：
- 于主界按`q`
- 按`Ctrl+C`/`Ctrl+D`/`Ctrl+\`（器将正理而退，通SIGHUP/SIGTERM/SIGINT之号）
- 于选项目录中择"Exit"（即`F9`键）

退文墨之界，默认乐声亦随之而止：前端自守之节度副本里 `preferences.stop_core_on_exit` 默启为 `1`，上列退法无论用哪一条皆发 `Control.Quit`（必先乎其前端实已接入核心——握手既成，执 `Attach` 之凭符；未尝接入者，一札不传）——核心先籍会话／resume 于策，乃止播而退。欲乐声之不辍者，置此于 `0`：前端惟发 `Control.Detach`，当下之曲续播不辍，`ter-music show` ／ `ter-music next` 于任何端闱依然可用。欲明止犹在之核心，请用 `ter-music daemon stop`；若欲最后一个前端既去、逾十秒而核心自退，则设 `core_exit_when_no_frontend` 为 `true`（见 [二之三 前端与核心](#二之三-前端与核心)）。

## 卷六 术法之架构

### 一 前端与核心

本器之码，环二面而立：二面共一进程之像，而职守未尝相侵。一次译纂可惟行核心（役使），亦可惟行前端（文墨之界／符令行），二者惟于 `player` 之门面与 D-Bus 之面相接：

| 面 | 进程 | 所掌之事 |
| --- | --- | --- |
| **核心／播弄之役** | `ter-music daemon foreground`（由 `daemon start` 或文墨之界拉起；Linyaps 封缄之包另可经 D-Bus 按需唤起、或以其 systemd 用户役使常驻，deb／RPM 皆不随包予此二类文件） | 音声之器与解绎、奉行播弄之队列（**惟识径**）、传输之令、音量／迅疾／播弄之制、卡拉OK汰聲、均衡器、当下曲目之文（歌辞、文书封面、频谱）、所布之 `Lyrics`／`Info`／`Control`／`Queue`／`Config` 接口、前端之登记与心跳、节度之文 |
| **前端／内容之客** | `ter-music`（文墨之界）、`ter-music play\|show\|…`（符令行） | 目录之扫描与元数据、SQLite曲库与FTS5搜求、自定曲帙、珍存／往迹／目录往迹、排序／筛择、远方之源与其下载之藏、ncurses文墨之界 |

其接缝刻意至薄，亦便于验：

- **`player/` 门面**：`player.c` 分付于 `player_local.c`（进程内播弄，留作迁移之基线）或 `player_remote.c`（D-Bus之客，默用者）。界面惟呼门面，故前端可独立营构，同一套文墨之界亦得御任一面。
- **`queue/backend_queue.c`**：核心之径表——序次、游标与版本之号，兼条目之元数据（径、标题、艺术家、专辑、时长、CUE之偏与轨号、歌辞之源）。`audio/play_queue.c` 乃进程内后端所用之薄转发，`playlist/playlist_queue.c` 乃惟一的「内容 → 核心队列」之桥。
- **D-Bus之面，`api_version 7`**：`Queue.Set/Append/InsertAfter/RemoveAt/MoveUp/MoveDown/Clear/Shuffle/PlayAt/Get` **惟受本地之径**（远方之URL见拒），一次所写不逾五百条，分页所读以 `RPC_PAGE_MAX`（千条）为限；`QueueChanged` 广播条目之数／当下之标／版本之号，故任意多之前端皆得同其步。歌辞之接口复带逐字之时（`GetDocument` 之 `end`／`timing`／`chars`，`GetLyrics` 之 `char_sung`／`char_total`）。
- **前端之登记**：每前端皆当登记而心跳，`core_exit_when_no_frontend` 与 `Info.GetInfo.frontends` 皆本于此。断线非致命：前端按指数退避（1／2／4／8／15／30 秒）而复接，并补推其内容之队列——盖核心自身无内容也。

### 二 架构门禁

四脚本守此分界，皆已入于CI：

| 门禁 | 符令 | 律 |
| --- | --- | --- |
| 后端之纯 | `scripts/test/check-core-purity.sh` | 后端之目（`audio config core info lyrics media queue` 与 `cli/daemon.c`）不得有远方乐源之符号，亦不得引内容／界面（playlist、library、search、界面渲染、前端之首文） |
| 前端之纯 | `scripts/test/check-ui-purity.sh` | 界面之达播弄之面，**惟**经 `player` 门面——不得直连引擎播弄之全局或播弄之令 |
| 包之身份 | `scripts/test/check-package-identity.sh` | 打包之身份必处处相合：应用之 ID 在 `debian/control`、`PKGBUILD`、`.SRCINFO`、`scripts/build/*` 与 systemd 之单元当同值；版号在 `APP_VERSION`（唯一之本源）、CMake `project VERSION`、`pkgver`、`debian/changelog` 首条当同值，而合于滚动之制 `<yy>.<q>.x.y`；`TER_MUSIC_API_VERSION` 当与 `dbus-rpc-check.sh` 所断者一也；旧名 `org.yxzl` 不得存于 `#include`、接口之字符串与打包之链；而**故为不改**之身份（ executable `ter-music`、MPRIS 总线之名、配置之目）亦不得顺手而改之 |
| 配置之属 | `scripts/test/check-config-ownership.sh` | `config.xml` 为核所独有。前端**不得**用 `save_config()`／`config_save_to_xml()`：惟改己之配置镜像，以差异付之门面 `player_config_persist()`（远方之制即 `Config.Set`） |

与之相配之回归套件：`scripts/test/run-unit-tests.sh`（径表、播弄队列之契约、歌辞解析、JSON读取之器、配置之差异、布局之 schema／几何／状态、不假 ncurses 之鼠标事件判定表，兼逐格栅格之验，以证诸预设无缝铺满），及端到端之脚本 `dbus-rpc-check.sh`、`config-migration-check.sh`、`lifecycle-e2e.sh`、`offline-reconnect-e2e.sh`、`multi-frontend-e2e.sh`、`paging-deepdir-e2e.sh`、`remote-frontend-e2e.sh`、`karaoke-e2e.sh`（自符令 `karaoke` 与 `Control.GetKaraoke` 以至节度之文之落盘、核心重启之回灌，并验越界之钳与故文缺键之默）、`mouse-tui-e2e.sh`（以真的 SGR 点击／滚轮之节纳于 pty，既断 D-Bus 上所成之果，亦断每事归属之窗格——弹层既张之时，于弹层之外按之，惟收起弹层，不得穿透而及其后之列表）、`lyrics-pane-e2e.sh`（以 pty 真流经 `vt_grid.py` 还原字符之格，验歌辞格于启动即现、无歌辞之曲暂退、复至有歌辞之曲则自还而无需按任何键）、`context-menu-e2e.sh`（以真右键之节验菜单锚点与钳制、三层子单、由单以行队列与页面之事、信息页惟流光一条，及外部改队列后 stale 之拒）；又有性能之探 `perf-check.sh`（令下至状见之迟速、空闲时CPU之耗，皆立其限；CI 惟录其数，不以为断）。

### 三 模块地图

本器采分曹营治之制。**源文**在 `src/cn.org.huansoft.ter-music/<module>/` 目录；**公首文**在 `include/cn.org.huansoft.ter-music/<module>/` 目录。主干部伍列于左：

- **main/main.c**：众部之总持、参数之铨叙、前端启途之分叉（远端之式走 `frontend_init_config()`，本地基线走 `init_all_persistent_data()`）
- **player/**：播弄之门面——`player.c`（分付）、`player_local.c`（进程内之后端）、
  `player_remote.c`（D-Bus之客，默用者），其宣言见
  `include/…/player/player_backend.h`
- **core/core.c**：核心之启导，为役使与进程内基线所共用
- **cli/cli.c、cli/cli_client.c**：**前端**之符令行子目分发，及
  `play`／`pause`／`show`／……所用之薄D-Bus客；`play` 于本进程扫描内容、
  成径表之队列而下付
- **cli/daemon.c**：**核心**之进程——节度、播弄与前端之守望；绝不扫描目录
- **queue/backend_queue.c**：核心之径表（序次、游标、版本之号、本地径之验、
  单次下付之限），兼CUE之前瞻
- **audio/**：**核心**之音声引擎——解绎与播弄之线、环形之缓冲、`play_queue.c`
  （转发之层，兼本地基线所用之界面之镜）、atempo之变速、卡拉OK汰聲（`karaoke.c`，限频带削居中者，序在均衡器之先）、10段均衡器、FFT频谱之数、
  `backend_ops.c` 与 PipeWire／PulseAudio／ALSA 之输出
- **lyrics/**：**核心**之歌辞引擎——寻索、内嵌（斐氏）与外部 `.lrc` 之解析、
  来源之偏好、时轴与分页之构
- **ui/**：**前端**之ncurses文墨之界——主事之循环、控御之栏、节度、选单、
  曲帙／队列之视、珍存、往迹、浏览之视、局度（`layout.c`、`layout_spec.c`、
  `layout_geometry.c`、`layout_state.c`）、`vinyl.c`（共享彩色/字符黑胶之戏与独立之格）、
  音程、可视化之绘、`lyrics.c`（惟司渲染，其引擎在核心）、文书封面之署`ui/cover/`
  （`text_cover.c`：点阵、ASCII、半块之字，以应`ter-music cover`与`Info.GetCoverArt`之求；
  `cover_image.c`：PNG/JPEG之解、灰度之化、尺度之易；`album_cover.h`：当今封面之道与
  庋藏之目，其实定于`playlist/playlist.c`）、对谈之框、鼠迹（`mouse.c`：ncurses之边垂，
  量诸格之方域；`mouse_routing.c`：不假ncurses而定其属，由 `scripts/test/test_mouse_routing.c`
  验之；`context_menu.c`／`context_menu_main.c`：右键之单与主列表之单；`context_menu_layout.c`：
  不假ncurses之摆位/滚动，由 `scripts/test/test_context_menu_layout.c` 验之）、滚动条与共用之器
- **media/**：**核心**之D-Bus会话——`session.c`（总线之名、自省、分发）、
  `rpc_common.c`（回复之助、分页）、`rpc_info.c`、`rpc_control.c`、
  `rpc_queue.c`、`rpc_lyrics.c`、`rpc_config.c`，及MPRIS媒体播弄之接口
- **info/info.c**：播弄信息之快照与渲染（文、JSON、点阵／ASCII／半块封面之缓存），
  为 `ter-music show` 与 `Info` 接口所共用
- **config/**：节度之统——`config.c`（libxml2之读写、版本之迁、默认之值）、
  `config_json.c` + `migration.c`（v1 `config.json` → XML）、schema之常量、
  `crypto.c`（前端远程藏所之密码加密解密）
- **playlist/**：**前端**之曲帙加载与元数据——递归目录之扫、斐氏与原生APEv2
  标签之读、CUE之检知与编码之自识、专辑封面之提与MRU之藏、
  `playlist_queue.c`（内容 → 径表 之桥）
- **library/**：**前端**之SQLite曲库——库之模式（tracks与FTS5、珍存、往迹、
  曲帙）、扫描之引擎、CRUD，及 `browser/browser.c`（艺术家 → 专辑 → 曲目之导）
- **remote/**：**前端**之远方乐源——`remote.c`（SMB/SFTP/FTP/WebDAV/HTTP，
  libcurl）、`remote_store.c`（服务器之目，藏于 `<制目录>/remote.xml`）、
  `remote_cache.c`（后台下载与本地之藏）；`ui/remote_view.c` 乃
  **节度 → 远程设备**之页
- **app/open.c**：开径与会话恢复之共用元术，为文墨之界与符令行 `play` 所用
  （不复付于 `Control.OpenPath`）
- **util/json.c**：有界之小JSON读写器，为 `Lyrics`／`Info` 接口与队列之载荷所共用
- **util/utf8.c**：二面皆需之UTF-8之具（核心之歌辞、符令行之出）
- **search/search.c**：异步之搜求（兼拼音，前端）
- **i18n/、logger/**：语言包与日志纪事之统（二面共享）

## 卷七 律例
本籍遵GNU General Public License v3.0公许之律。君得自由用之、改之、布之，然所改之裔作，必同此律以开源，无得私匿。

## 卷八 免责之辞

Ter-Music者，纯然乐播之器也，本器不供、不藏、不分发任何音声之文或他项版权所护之内容。用者当自备合法所得之音声文卷。本器之本地播弄与远程播弄之能，唯为播用者合法所得之媒文而设。

与本器所播音声内容相关之一切版权及智慧财产，俱归各权主所有。因用本器播弄音声内容而致之任何版权纠葛，概由用者自负其责。撰者于因用本器而致之任何版权或他项律法之事，不担任何责任。

## 卷九 撰者
- **撰者**：浣软科技（HuanSoft）
- **邮驿**：<yxzl666xx@outlook.com>
- **本籍所藏**：<https://github.com/HuanSoft-Open-Source-Community/ter-music.git>

## 卷九 鸣谢
谨申丹悃，以谢诸彦之劻勷：

- **@guanzi008** - 覃思邃密，多所厘革：Debian之封缄元数据粲然备具，MPRIS之会话集成optional而设，DEB之封缄臻于至善，UTF-8之键入靡有疪颣，节度之导引咸就条畅，鼠迹之交互悉得其宜，曲帙之纪纲秩然不紊，目录之次列如贯珠，播弄之断而复续，效能之浚而益弘，声华之绘饰焕若披锦，中夏文界之观瞻雅饬可观
- **@Zeta** - 拓土开疆，爰启Arch Linux之域

## 卷十 襄助之请
君若有疑议、有补益，咸得献Issue与Pull Request，无任忻幸。

## 卷十一 祛疑解惑
**患：音声不发**
- 音声后端按 PipeWire → PulseAudio → ALSA 序自动检知。自动之式启时历探诸枢（ALSA者实启常器），全不中则须臾复探；所择之枢播时建流不遂，复探余枢而退就，状语与志文皆明其所迁。行 `pactl info` 或 `pw-cli info` 以察何役运行
- 探之不中之枢不复列于设界面（F2）→ 音声后端；明择之枢不可用者，亦如上退就，非默然而已
- 察扬声器音量是否开启
- 若用PipeWire，必 `pipewire` 与 `wireplumber` 之役运行
- 若用PulseAudio，行 `systemctl status pulseaudio` 以验
- 亦可于设界面（F2）→ 音声后端中手动迁转

**患：音声涩滞、断续或有杂噪**
- 机器音声之器禀赋各异，默输出时延之参未必然合于君之器
- 可试于设界面（按`F2`入设）增"输出时延"之值，或径修节文`~/.config/ter-music/config.xml`中`audio_latency_ms`之域
- 时延之域自20至250千分秒，每增10而验，至音声复其常
- 若增时延而未解，可察PipeWire/PulseAudio之制或更新音声驱策

**患：中夏文字乱形，或CJK字符显为方垒**
- 必端闱用UTF-8之编码
- 察系统locale之设：`locale`当显`LC_CTYPE=UTF-8`之伦
- 若于tty端闱中，CJK字符仍乱，可易以kmscon，其于东亚之文，所达尤善

**患：译纂之际不得头文件**
- 必尽装诸凭藉之开发包，详卷三
- 诸统多以开发包与运行之包分置，必装*-devel或*-dev之属

**患：不得开某些音声之文**
- 确证君之斐氏版通其制式
- 新版斐氏，所赅尤广，宜更易之

**患：CUE分轨不显**
- 必.cue文件与音声之文同名（如 `album.flac` + `album.cue`）
- 若CUE文字乱形，于节度 → CUE字符编码中改之（中文试GBK，日文试Shift-JIS）

**患：音乐库未显所有音声**
- 按`M`入音乐库览器，察 `~/.config/ter-music/` 下有无 `library.db`
- 音乐库于启时扫之，若君添新音声，重启器可触发重扫
- 音乐库今通**递归子目录扫描**，可嵌套目录之构
