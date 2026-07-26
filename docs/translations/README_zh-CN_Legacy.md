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

## 卷一 本志叙略
### 一 枢要功用
Ter-Music者，清简端闱之符令乐部也，专为麟纳克斯御统而造。凭斐氏FFmpeg以解绎音声，通PipeWire/PulseAudio/ALSA以传布乐响（运行时自动检测），纽氏ncursesw以营文墨之界。其用赅备，列于左：
- 赅众音之制式，凡MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS、**WV（WavPack）**之伦，罔不洞达
- **CUE分轨之能**：通FLAC/APE/WV之CUE析辞，自动检知编码（GBK/BIG5/Shift-JIS）
- **内嵌歌辞之能**：斐氏AVDictionary与APE标签之文辞，优先于外.lrc文件
- **10段图示均衡器**：ISO准频（31Hz-16kHz），双二阶IIR滤波，±12dB调幅，设中可视化条图，具**均衡器预设**与**软限幅**之能
- **17种播弄之制**：自基础（序进、单曲回环、全帙周流、乱序一度、乱序杂陈）至进阶（按文件夹/专辑/艺术家分组）
- 契LRC歌辞之文，循音程以同辉，逐节次以昭焕，毫厘不爽；**内嵌歌辞**（斐氏AVDictionary与APE标签）先于外.lrc文件。可于歌词定位模式中切换来源（Ctrl+L → Tab）
- 迅疾之度有六：曰迟、曰常、曰稍疾、曰疾、曰倍、曰三倍，任君节度
- **音乐库**：SQLite库存储，FTS5全文搜检，按艺术家/专辑/流派览之，兼**递归子目录搜索**与增量跟踪
- **播弄队列**：独立队列之界，显序号、当下播弄之标，可排序、可恒存
- 乐目营理之能，任君创置多组曲帙，随宜调遣
- 远程播乐之能，通SMB、SFTP、FTP、WebDAV、HTTP诸般远器之约，以传远方服器之乐
- 珍存所好之章，便疾取览
- 自动录纪播弄之迹，便于回溯
- 志录近所临之乐籍目录，无烦复寻
- **色采之谱**：24套预设主题 + 1个自定槽位，前后色彩配对保护
- **恒存之储**：SQLite一统（珍存、往迹、曲帙），自动从v1 JSON迁移
- 专辑封面显明之能，可于点阵中绘封面之图（可于节度中启闭）
- 全凭键符捷操，迅疾无伦
- 音程条贯实时昭显，流转顺滑，可任意跳转

### 二 造作之本旨
本器之造，恪守**清简、捷疾、元本**之宗：
- 体至清简，不藉重轩峻宇之境，所占资源至微
- 端闱元造，纯以文墨为界，宜乎无图之器、幽隐之设，与夫耽符令之流者
- 分曹列伍，部伍明晰，易于缮治增益
- 遵西土Unix之哲，专一事而工，与他器协契无间
- 略无窥伺，不录用户毫末之迹，深敬私隐

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
| 🔊 音声传布 | 通PipeWire、PulseAudio、ALSA三枢，运行时自动检测（PipeWire > Pulse > ALSA） |
| 🎛️ 10段均衡器 | ISO准图示均衡器，设中可视化条图 |
| ⏩ 迅疾节度 | 六档迅迟之度，播弄中可随意迁转 |
| 🌐 远程播乐 | 通SMB/SFTP/FTP/WebDAV/HTTP诸般远器之约 |
| 🎨 专辑封面 | 端闱中显乐集之面，可于节度中启闭 |

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
| `libcurl-devel` | 7.0+ | 远程播乐（SMB/SFTP/FTP/WebDAV） |
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
| `dbus-devel` | MPRIS D-Bus媒体会话之耦 |

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
yay -S ter-music-cn

# 用 paru（AUR 助手）
paru -S ter-music-cn
```

**用 ZPM（MengXi OS 包管器）纳置：**
```bash
# 先纳置 ZPM（若未纳置）
git clone https://aur.archlinux.org/zetapm.git
cd zetapm
makepkg -si

# 继用 ZPM 纳置 ter-music-cn
zpm -S ter-music-cn
```

**自 AUR 手工纳置：**
```bash
git clone https://aur.archlinux.org/ter-music-cn.git
cd ter-music-cn
makepkg -si
```

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

- [营构指南](../BUILD_GUIDE.md) - 营构脚本之详悉说明
- [脚本志](../../scripts/README.md) - 诸脚本之速览

所持之格式：
- **AppImage** - 通于诸种麟纳克斯发行版
- **可携包** - 尽纳必需之凭藉库
- **RPM包** - 宜于Fedora/RHEL之统
- **DEB包** - 宜于Debian/Ubuntu之统
- **玲珑包** - 宜于deepin/UOS之统
- **Arch Linux包** - 宜于Arch Linux及其衍生之统

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
```bash
ter-music [OPTIONS]

选项：
  -o, --open <path>    启器时直开指定乐籍目录
  -d, --debug          启调拭志录（录于 ter-music-debug.log）
  -h, --help           显助益之文
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

### 三 文界局度
启之，则局分三栏，其制如左：
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

- **左上方**：乐目曲帙，显当前目录下所有音声之文（Tab键切换文件浏览/播弄队列）
- **左下方**：控御之栏，具播弄控御之键与音程条贯
- **右侧**：歌辞显明之域，同步显当前所播之曲文；亦显乐集之封面（若启此能）
- **底部**：选项目录，具节度、播弄往迹、所珍、关于、退去诸选项

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

### 七 歌辞显明
本器通自动加载LRC式歌辞之文，且支持内嵌歌词：
- **内嵌歌词优先**：器先读取音声文件中的内嵌歌词（FFmpeg/APE标签）
- 若不得内嵌歌词，则回退加载外部`.lrc`文件
- 歌辞之文必与音声之文同置一目录
- 歌辞之名必与音声之名同，后缀为`.lrc`
- 例：`song.mp3` → `song.lrc`
- 器将随播弄之时，自动焕明当前歌辞之行
- 若不得歌辞之文，歌辞区将显"No lyrics loaded"
- **切换歌词来源**：在歌词定位模式下按 `Tab` 可在内嵌/外置间切换

### 八 节度之文
节度之文存于`~/.config/ter-music/config.xml`（v2.2 XML格式，经libxml2解析）。器初启时将自动创之（若有v1 config.json则自动迁之）。

**节度之项**：
- `default_startup_path`：默认启行之目录
- `auto_play_on_start`：启器时自动播弄（0/1）
- `remember_last_path`：记上次所临之目录（0/1）
- `show_album_cover`：显专辑封面（0/1）
- `show_lyrics_panel`：显歌辞之板（0/1）
- `default_playback_speed`：默播之速（0.75、1.0、1.25、1.5、2.0、3.0）
- `default_play_mode`：默播弄之制（0=序进、1=单曲回环、2=全帙周流、3=乱序一度、4=乱序杂陈……）
- `advanced_play_modes_enabled`：启进阶文件/专辑/艺术家播弄之制（0/1）
- `lyrics_alignment`：歌辞对齐之式（0=居左、1=居中、2=居右）
- `clear_history_on_startup`：启时清播弄之迹（0/1）
- `resume_last_playback`：续从前之处播弄（0/1）
- `seamless_preload`：当前曲末预解下曲，达无隙播弄（0/1）
- `ui_language`：界语（字符串ID："zh_CN"、"en_US"等，通过F7语言视图设置）
- `volume_percent`：默音量之率（0-100）
- `audio_latency_ms`：输出时延（千分秒）
- `audio_backend`：音声后枢（0=自动、1=PulseAudio、2=ALSA、3=PipeWire）
- `sort_mode`：排序之式（0=默、1=标题、2=艺术家、3=专辑、4=文件名）
- `cue_encoding`：CUE文字符编码（0=自动、1=UTF-8、2=GB18030、3=GBK、4=BIG5、5=Shift-JIS）
- `remote_connections`：所存远程服器之连（SMB/SFTP/FTP/WebDAV）
- 色采主题节度：24套预设主题 + 1个自定槽位，所有文界元素之前景、背景色
- 均衡器：10段增益、前置放大、启/禁

器将自动存其节度，改之即生效。

### 八之一 语言包之制

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

#### 八之二 tar.gz 语言包布之规

语言包亦可以 `.tar.gz`（或 `.tgz`）压缩包形布之，以便共享与一键纳置。于语言选择视中按 `A` 键纳之，按 `D` 键除已加之用户语言包。

**压缩包之内：**

| 文件 | 必需 | 说 |
|------|------|----|
| `lang.xml` | 是 | 语言数据文件（格式见八之一） |
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
4. 拒覆内置语言（`zh_CN`、`en_US`）之包
5. 行 50 MB 单文件大小限之检

验过，该语言即现于语言选择视。用户所加之语言，在界面上与内置者有明别。

**注：** 若 `~/.config/ter-music/lang/` 中已存同 `<id>` 之语言包，纳新 tar.gz 将默覆之。欲复误删之内置语言，请重装程序。

### 九 数据存贮之所
所有用户数据，皆存于`~/.config/ter-music/`目录之下：
```
~/.config/ter-music/
├── config.xml       # 节度之文（v2.2 XML格式，libxml2解析）
├── library.db       # SQLite数据库（音乐库、珍存、曲帙、往迹）
├── queue.txt        # 播弄队列恒存
├── album_cover_cache/  # 专辑封面暂存
├── lang/            # 用户语言包目录（覆盖内置翻译）
└── config.json.bak  # v1节度首迁之自动备份（如有）
```

**注：** v1.0之JSON存储（config.json、独立favorites、history、dir_history、playlists/）已尽替以SQLite数据库library.db。v2.0初启时将自动迁移。

### 十 常施用之程叙
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

### 十一 捷键速览
| 分曹 | 键符 | 所司之事 |
| --- | --- | --- |
| **全域** | `q` | 退其器 |
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
|  | `Ctrl+L` | 进入/退出歌词定位模式 |
| **歌辞** | `Ctrl+L` | 进入/退出歌词定位模式 |
|  | `↑`/`↓` | 择上/下一句 |
|  | `Enter`/`Space` | 跳转至所选歌辞行 |

### 十二 端闱修广之调
本器通端闱窗牖修广之调，君改其大小时，器将自动重置局度，重绘文界。

### 十三 退其器
退之之法有三：
- 于主界按`q`
- 按`Ctrl+C`/`Ctrl+D`/`Ctrl+\`（器将正理而退，通SIGHUP/SIGTERM/SIGINT之号）
- 于选项目录中择"Exit"（即`F9`键）

## 卷六 术法之架构
本器采分曹营治之制。**源文**在 `src/org.yxzl.ter-music/<module>/` 目录；**公首文**在 `include/org.yxzl.ter-music/<module>/` 目录。主干部伍列于左：

- **main/**：众部之总持，符令行参数之铨叙
- **ui/**：文墨界子系统 — 渲染、局度、按键之应接
  - **ui.c**：主事循环，视之迁转，按键之分发
  - **controls.c**：控御栏（播/停/上/下/音量/速度/模式弹出菜单）
  - **settings.c**：节度之视（侧栏 + 右侧选择菜单）
  - **menus.c**：选单栏、功能键（F1-F9）、弹出菜单之管摄
  - **playlist_render.c**：文件浏览与播弄队列之视渲染
  - **playlist_view.c**：曲帙营理之视
  - **favorites.c**：珍存之视
  - **history.c**：播弄往迹之视
  - **info_view.c**：关于之视
  - **help_view.c**：助益之视
  - **language_view.c**：语言选择之视（i18n语言包览器）
  - **layout.c**：端闱局度之营理（窗口修广之调）
  - **progress_ui.c**：音程条贯渲染（弹出菜单启时停UI）
  - **visualizer.c**：音频频谱可视化
  - **lyrics.c**：歌辞之加载、解析、同步显明（内嵌歌辞之能）
  - **image_loader.c**：专辑封面图之加载处置（PNG/JPEG）
  - **braille_art.c**：点阵绘艺，以显专辑封面于端闱
  - **dialog.c**：对谈之框
  - **mouse.c**：鼠迹交互之应接
  - **scrollbar.c**：滚动条复用模块
  - **utf8.c**：UTF-8字符串具
  - **util.c**：共享UI具（侧栏、色谱等）
- **audio/**：音声引擎 — 解绎、播弄、DSP
  - **audio.c**：音声总控、音量管摄、播弄之制切换、后端营理
  - **playback_thread.c**：独立播弄线程、FFmpeg解绎循环、播毕之应接
  - **segment_buffer.c**：PCM数据环形缓冲区，控RSS内存约20MB
  - **play_queue.c**：播弄队列（Fisher-Yates洗牌、17种播弄之制导航）
  - **atempo.c**：FFmpeg atempo滤镜，变速播弄
  - **equalizer.c**：10段ISO图示均衡器，双二阶IIR滤波器
  - **audio_visualizer.c**：基于FFT之频谱数据提取，供可视化之需
  - **backend_ops.c**：统一后端操作接口（音量、延时、设备初始化）
  - **backend/pipewire.c**：PipeWire音声输出（dlopen运行时加载，无编译时依赖）
  - **backend/pulse.c**：PulseAudio音声输出
  - **backend/alsa.c**：ALSA音声输出
- **playlist/**：乐目加载、元数据、CUE解析
  - **playlist.c**：目录搜检（**递归**子目录搜索）、元数据读取（FFmpeg + APEv2标签）、CUE文件检知
  - **cue_parser.c**：CUE文件逐行解析器，分轨播弄
  - **encoding.c**：CUE文字符编码自动检知与转换（iconv）
  - **ape_tag.c**：原生APEv2标签解析器，以增元数据之提取
- **library/**：SQLite音乐库
  - **library.c**：数据库模式（tracks + FTS5全文搜检、珍存、往迹、曲帙）、扫描引擎、CRUD操作
  - **browser/browser.c**：音乐库览器UI（艺术家 → 专辑 → 曲目导航）
- **config/**：节度子系统
  - **config.c**：XML节度加载/保存（libxml2、schema v2.2）
  - **migration.c**：v1 config.json → v2 config.xml 迁之
  - **schema.h**：XML元素/属性常量定义
  - **crypto.c**：远程连密码加密解密之制
- **remote.c**：远程播乐之能（SMB/SFTP/FTP/WebDAV/HTTP诸约）
- **media_session.c**：MPRIS D-Bus媒体会话之耦（可择）
- **search.c**：异步搜求之能（拼音搜求）
- **logger.c**：日志纪事之部

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
- 音声后端按 PipeWire → PulseAudio → ALSA 序自动检知。行 `pactl info` 或 `pw-cli info` 以察何役运行
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