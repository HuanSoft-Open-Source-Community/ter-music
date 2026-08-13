<div align="center">

# Ter-Music终端音乐播放器说明
![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-green.svg)
![Docker](https://img.shields.io/badge/Docker-支持-2496ED.svg)
![Python](https://img.shields.io/badge/Python-3.x-3776AB.svg)
![Shell](https://img.shields.io/badge/Shell-Bash-4EAA25.svg)
![Linyaps](https://img.shields.io/badge/Linyaps-支持-8A2BE2.svg)

</div>

**其他语言版本 / Other Languages:**
- [English](../README.md)
- [中文（文言版）](README_zh-CN_Legacy.md)
- [Lyrics API (English)](../API_LYRICS_en_US.md)

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
- **播放队列**：独立队列界面，显示序号、当前播放指示，支持排序和持久化
- 支持歌单管理，可创建多个自定义歌单，灵活切换播放
- 支持收藏喜欢的歌曲，方便快速查找播放
- 自动记录播放历史，便于回顾听过的音乐
- 保存最近访问的音乐目录，无需重复查找
- **扩展调色板**：24套预设主题 + 1个自定义槽位，前后角色彩配对保护
- **持久化存储**：SQLite统一存储（收藏、历史、歌单），自动从v1 JSON迁移
- 支持专辑封面显示，可在终端中渲染显示封面图片（设置中可开关）
- **MPRIS 与歌词 API**：通过 D-Bus 提供桌面媒体控制、`mpris:artUrl` 专辑封面，以及供其他程序读取的开放歌词接口
- 纯键盘快捷键操作，响应迅速
- 实时显示音频进度条，流畅丝滑，可任意跳转播放位置

### 二 开发宗旨
本播放器的开发遵循**简洁、快速、原生**的核心原则：
- 极致简洁，不依赖复杂的运行环境，占用系统资源极少
- 基于终端原生开发，纯字符界面，适合无图形界面、嵌入式场景，以及习惯命令行操作的用户
- 代码模块化设计，结构清晰，易于维护和功能扩展
- 遵循Unix设计哲学，专注做好音乐播放一件事，可与其他工具无缝配合
- 无任何窥探行为，不记录用户隐私数据，充分尊重用户隐私

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
| 🌐 远程播放 | 支持SMB/SFTP/FTP/WebDAV/HTTP远程音乐播放 |
| 🎨 专辑封面 | 终端专辑封面显示，可在设置中开关 |
| 🎵 MPRIS / 歌词 API | 桌面媒体控制、`mpris:artUrl` 封面，以及基于 D-Bus 的 JSON 歌词接口 |

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
```bash
ter-music [OPTIONS]

选项：
  -o, --open <path>    启动时直接打开指定的音乐目录
  -d, --debug          启用调试日志（输出到 ter-music-debug.log）
  -h, --help           显示帮助信息
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
`GetLyrics`，信号 `LyricsChanged`。JSON 结构与调用示例见
[Lyrics API (English)](../API_LYRICS_en_US.md)。

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
- `remote_connections`：保存的远程服务器连接（SMB/SFTP/FTP/WebDAV）
- 颜色主题设置：24套预设主题 + 1个自定义槽位，所有界面元素的前景色、背景色
- 均衡器设置：10段增益、前置放大、启用/禁用

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
├── config.xml       # 配置文件（v2.2 XML格式，libxml2解析）
├── library.db       # SQLite数据库（音乐库、收藏、歌单、历史）
├── queue.txt        # 播放队列持久化
├── lang/            # 用户语言包目录（覆盖内置翻译）
└── config.json.bak  # v1配置文件首次迁移时的自动备份（如有）
```

**注意：** v1.0的JSON存储（config.json、独立的favorites、history、dir_history、playlists/目录）已全部替换为SQLite数据库library.db。首次启动v2.0时会自动迁移。

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
退出方法有三种：
- 在主界面按`q`键
- 按`Ctrl+C`/`Ctrl+D`/`Ctrl+\`（播放器会优雅退出，支持SIGHUP/SIGTERM/SIGINT信号）
- 在选项菜单中选择"Exit"（即`F9`键）

## 第六章 技术架构
本播放器采用模块化设计。**源文件**位于 `src/org.yxzl.ter-music/<module>/` 目录；**公开头文件**位于 `include/org.yxzl.ter-music/<module>/` 目录。

核心代码模块如下：

- **main/**: 程序入口，命令行参数解析
- **ui/**: 用户界面子系统 — 渲染、布局、输入处理
  - **ui.c**: 主事件循环、视图切换、输入分发
  - **controls.c**: 控制栏（播放/暂停/上/下/音量/速度/模式弹出菜单）
  - **settings.c**: 设置视图（侧边栏 + 右侧选择菜单）
  - **menus.c**: 菜单栏、功能键处理（F1-F9）、弹出菜单管理
  - **playlist_render.c**: 文件浏览与播放队列视图渲染
  - **playlist_view.c**: 歌单管理视图
  - **favorites.c**: 收藏视图
  - **history.c**: 播放历史视图
  - **info_view.c**: 关于视图
  - **help_view.c**: 帮助视图
  - **language_view.c**: 语言选择视图（i18n语言包浏览器）
  - **layout.c**: 终端布局管理（窗口大小调整）
  - **progress_ui.c**: 进度条渲染（弹出菜单激活时暂停UI）
  - **visualizer.c**: 音频频谱可视化
  - **lyrics.c**: 歌词加载、解析、同步显示（支持内嵌歌词）
  - **image_loader.c**: 专辑封面图片加载处理（PNG/JPEG）
  - **braille_art.c**: 盲文点阵渲染，在终端显示专辑封面
  - **dialog.c**: 对话框
  - **mouse.c**: 鼠标交互处理
  - **scrollbar.c**: 滚动条复用模块
  - **utf8.c**: UTF-8字符串工具函数
  - **util.c**: 共享UI工具函数（侧边栏、调色板等）
- **audio/**: 音频引擎 — 解码、播放、DSP
  - **audio.c**: 核心音频控制、音量管理、播放模式切换、后端管理
  - **playback_thread.c**: 独立播放线程、FFmpeg解码循环、播放结束处理
  - **segment_buffer.c**: PCM数据环形缓冲区，控制RSS内存约20MB
  - **play_queue.c**: 播放队列（Fisher-Yates洗牌、17种播放模式导航）
  - **atempo.c**: FFmpeg atempo滤镜，变速播放
  - **equalizer.c**: 10段ISO图示均衡器，双二阶IIR滤波器
  - **audio_visualizer.c**: 基于FFT的频谱数据提取，供可视化使用
  - **backend_ops.c**: 统一后端操作接口（音量、延时、设备初始化）
  - **backend/pipewire.c**: PipeWire音频输出（dlopen运行时加载，无编译时依赖）
  - **backend/pulse.c**: PulseAudio音频输出
  - **backend/alsa.c**: ALSA音频输出
- **playlist/**: 歌单加载、元数据、CUE解析
  - **playlist.c**: 目录扫描（**递归**子目录扫描）、元数据读取（FFmpeg + APEv2标签）、CUE文件检测、专辑封面提取与 MRU 封面缓存、同目录封面回退
  - **cue_parser.c**: CUE文件逐行解析器，支持分轨播放
  - **encoding.c**: CUE文件编码自动检测与转换（iconv）
  - **ape_tag.c**: 原生APEv2标签解析器，增强元数据提取
- **library/**: SQLite音乐库
  - **library.c**: 数据库模式（tracks + FTS5全文搜索、收藏、历史、歌单）、扫描引擎、CRUD操作
  - **browser/browser.c**: 音乐库浏览器UI（艺术家 → 专辑 → 曲目导航）
- **config/**: 配置子系统
  - **config.c**: XML配置加载/保存（libxml2、schema v2.2）
  - **migration.c**: v1 config.json → v2 config.xml 迁移
  - **schema.h**: XML元素/属性常量定义
  - **crypto.c**: 远程连接密码加密解密处理
- **remote.c**: 远程音乐播放（SMB/SFTP/FTP/WebDAV/HTTP协议）
- **media_session.c**: MPRIS D-Bus 媒体会话、专辑封面 URL 与歌词 API 集成（可选）
- **search.c**: 异步搜索功能（支持拼音搜索）
- **logger.c**: 日志记录子系统

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
