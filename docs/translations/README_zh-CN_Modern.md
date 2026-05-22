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

## 第一章 产品概述
### 一 核心功能
Ter-Music是一款简洁的终端音乐播放器，专门为Linux系统开发。它借助FFmpeg解码音频、PulseAudio输出声音、ncursesw构建终端界面，核心功能如下：
- 支持多种音频格式，包括MP3、WAV、FLAC、OGG、M4A、AAC、WMA、APE、OPUS等，均能完美解码
- 兼容LRC格式歌词，可精准跟随音频进度同步显示，分秒不差
- 提供四种循环模式：顺序播放、单曲循环、列表循环、随机播放
- 支持倍速播放，提供0.75x、1.0x、1.25x、1.5x、2.0x、3.0x六档速度调节，高效收听
- 支持远程音乐播放，可通过SMB、SFTP、FTP、WebDAV、HTTP协议播放远程服务器或NAS上的音乐
- 支持歌单管理，可创建多个自定义歌单，灵活切换播放
- 支持收藏喜欢的歌曲，方便快速查找播放
- 自动记录播放历史，便于回顾听过的音乐
- 保存最近访问的音乐目录，无需重复查找
- 终端界面色彩可自定义，支持个性化设置
- 支持专辑封面显示，可在终端中渲染显示封面图片（设置中可开关）
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
| 🔄 配置持久化 | 自定义设置、收藏内容、播放历史均自动保存，重启后仍保留 |
| 🎯 多视图切换 | 通过F2至F7快捷键，可快速切换设置、历史、歌单等视图 |
| ⚡ 响应流畅 | 每秒100帧刷新，音频进度条流转无卡顿 |
| 🔧 CMake构建 | 采用现代构建方式，跨系统兼容性好 |
| 🔊 音频后端 | 支持PulseAudio和ALSA输出，运行时可在设置中切换 |
| ⏩ 倍速播放控制 | 六档速度调节（0.75x-3.0x），播放中可随时切换 |
| 📊 信息栏 | 实时显示当前音频的采样率、位深、比特率、编码格式 |
| 🌐 远程播放 | 支持SMB/SFTP/FTP/WebDAV/HTTP远程音乐播放 |
| 🎨 专辑封面 | 终端专辑封面显示，可在设置中开关 |

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
| `pthread-devel` | 系统自带 | 多线程处理 |
| `libcurl-devel` | 7.0+ | 远程音乐播放（SMB/SFTP/FTP/WebDAV） |
| `cmake` | 3.10+ | 项目构建（编译时必需） |
| `gcc` | 7.0+ | C语言编译器（编译时必需） |
| `make` | - | 构建工具（编译时必需） |
| `pkg-config` | - | 依赖检查（编译时必需） |

### 二 Fedora / RHEL / CentOS 安装命令
```bash
sudo dnf install cmake gcc make pkg-config
sudo dnf install ffmpeg-free-devel libpng-devel libjpeg-turbo-devel pulseaudio-libs-devel ncurses-devel libcurl-devel
```

### 三 Ubuntu / Debian / Linux Mint 安装命令
```bash
sudo apt update
sudo apt install cmake gcc make pkg-config
sudo apt install libavcodec-dev libavformat-dev libswresample-dev libswscale-dev libavutil-dev libavfilter-dev libpng-dev libjpeg-dev libpulse-dev libncursesw5-dev libcurl4-openssl-dev
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
sudo pacman -S ffmpeg libpng libjpeg pulseaudio ncurses libcurl
```

## 第四章 编译步骤
### 一 克隆源码
```bash
git clone https://github.com/YXZL985/ter-music.git
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

- [构建指南](../BUILD_GUIDE.md) - 构建脚本的详细使用说明
- [脚本说明](../../scripts/README.md) - 所有构建脚本的快速参考

支持的打包格式：
- **AppImage** - 通用Linux包格式
- **便携包** - 包含所有依赖的自解压压缩包
- **RPM包** - 适用于Fedora/RHEL系发行版
- **DEB包** - 适用于Debian/Ubuntu系发行版
- **玲珑包** - 适用于deepin/UOS系统
- **Arch Linux包** - 适用于Arch Linux及其衍生发行版

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
| `D` | 临时将焦点切换到歌词区 |

**注意**：切换到歌词区后，歌词区会显示当前歌曲的歌词，但不会自动随音频进度滚动，需再次按D键恢复自动滚动。且只有焦点在歌单区时，才能切换到歌词区。

#### 歌单区操作（焦点在歌单区域）
| 按键 | 功能 |
| --- | --- |
| `↑` / `↓` | 向上/向下选择歌曲 |
| `Space` / `Enter` | 播放选中的歌曲 |
| `O` / `o` | 打开新的音乐文件夹 |
| `F` / `f` | 将选中的歌曲加入收藏列表 |
| `A` / `a` | 将选中的歌曲加入首个自定义歌单 |
| `S` | 启用音频搜索功能，通过拼音首字母检索歌曲名、歌手 |

#### 控制区操作（焦点在控制栏）
| 按键 | 功能 |
| --- | --- |
| `←` / `→` | 向左/向右选择控制按钮 |
| `Space` | 触发选中的控制按钮 |
| `,` | 播放进度后退5秒 |
| `.` | 播放进度前进5秒 |
| `D` / `d` | 跳转到当前歌词行对应的播放进度 |
| `-` / `_` | 降低音量 |
| `=` / `+` | 提高音量 |

**控制按钮说明**：
| 按钮名 | 功能 |
| --- | --- |
| `<<` | 上一曲 |
| `Play/Pause` | 播放/暂停 |
| `>>` | 下一曲 |
| `Stop` | 停止播放 |
| `Loop` | 切换循环模式（关闭 → 单曲循环 → 列表循环 → 随机播放） |
| `Speed` | 切换倍速播放（0.75x → 1.0x → 1.25x → 1.5x → 2.0x → 3.0x） |
| `Progress` | 进度条（显示当前播放进度） |
| `Volume` | 音量调节（显示当前音量百分比，可调整） |

#### 歌词区操作（焦点在歌词区域）
| 按键 | 功能 |
| --- | --- |
| `↑` / `↓` | 向上/向下选择歌词行 |
| `D` | 退出歌词区焦点，恢复自动滚动 |

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
| `F7` | 切换中英文界面 |
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
| `Esc` + `7` | 切换中英文界面 |
| `Esc` + `8` | 帮助（本页面） |
| `Esc` + `9` | 退出播放器 |
| `q` | 退出播放器 |

### 五 循环模式说明
| 模式名 | 说明 |
| --- | --- |
| `Off` | 顺序播放，播放到列表末尾停止 |
| `Single` | 单曲循环，重复播放当前歌曲 |
| `List` | 列表循环，播放完一轮后从头开始 |
| `Random` | 随机播放，随机选择下一首歌曲 |

按`Space`键触发Loop按钮，可循环切换模式。

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
本播放器支持自动加载LRC格式歌词：
- 歌词文件需与音频文件放在同一目录
- 歌词文件名需与音频文件名相同，后缀为`.lrc`
- 示例：`song.mp3` 对应 `song.lrc`
- 播放器会随播放进度自动高亮显示当前歌词行
- 若未找到歌词文件，歌词区会显示"No lyrics loaded"

### 八 配置文件

配置文件存储在`~/.config/ter-music/config.json`，播放器首次启动时会自动创建。

**配置项**：
- `default_startup_path`：默认启动目录
- `auto_play_on_start`：启动时自动播放（0/1，0为关闭，1为开启）
- `remember_last_path`：记住上次访问的目录（0/1）
- `show_album_cover`：显示专辑封面（0/1）
- `show_lyrics_panel`：显示歌词面板（0/1）
- `default_playback_speed`：默认播放速度（0.75、1.0、1.25、1.5、2.0、3.0）
- `default_loop_mode`：默认循环模式（0=顺序播放、1=单曲循环、2=列表循环、3=随机播放）
- `lyrics_alignment`：歌词对齐方式（0=居左、1=居中、2=居右）
- `clear_history_on_startup`：启动时清空播放历史（0/1）
- `resume_last_playback`：从上次位置继续播放（0/1）
- `ui_language`：界面语言（0=中文、1=English）
- `volume_percent`：默认音量百分比（0-100）
- `audio_latency_ms`：输出时延（毫秒）
- `audio_backend`：音频后端（0=自动、1=PulseAudio、2=ALSA）
- `sort_mode`：排序模式（0=默认、1=标题、2=艺术家、3=专辑、4=文件名）
- `remote_connections`：保存的远程服务器连接（SMB/SFTP/FTP/WebDAV）
- 颜色主题设置：所有界面元素的前景色、背景色

播放器会自动保存配置，修改后立即生效。

### 九 数据存储位置

所有用户数据均存储在`~/.config/ter-music/`目录下：
```
~/.config/ter-music/
├── config.json      # 配置文件
├── history        # 播放历史
├── favorites      # 收藏列表
├── dir_history    # 目录访问历史
├── playlists/     # 自定义歌单
├── remote/        # 远程连接历史
└── album_cover_cache/  # 专辑封面缓存
```

### 十 常用操作流程
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
4. 返回主界面，在歌单中选中歌曲，按`A`键加入该自定义歌单

### 十一 快捷键速览
| 分类 | 按键 | 功能 |
| --- | --- | --- |
| **全局** | `q` | 退出播放器 |
|  | `F1` | 返回主界面 |
|  | `F2`-`F6` | 切换功能视图 |
|  | `F7` | 切换中英文界面 |
|  | `Esc` | 返回主界面 |
| **焦点** | `C` | 焦点切换到控制区 |
|  | `L` | 焦点切换到歌单区 |
| **歌单** | `↑`/`↓` | 选择上/下一曲 |
|  | `Space`/`Enter` | 播放选中的歌曲 |
|  | `O` | 打开文件夹 |
|  | `F` | 加入收藏列表 |
|  | `A` | 加入自定义歌单 |
|  | `S` | 启用拼音搜索功能 |
| **控制** | `←`/`→` | 选择控制按钮 |
|  | `Space` | 触发控制按钮 |
|  | `,` | 后退5秒 |
|  | `.` | 前进5秒 |
|  | `D` | 跳转到当前歌词行对应进度 |
| **歌词** | `D` | 临时切换焦点到歌词区 |
|  | `↑`/`↓` | 选择上/下一句歌词 |
|  | `D`/`Enter`/`Space` | 跳转到选中的歌词行对应进度 |

### 十二 终端窗口大小调整
本播放器支持终端窗口大小调整，修改窗口尺寸时，播放器会自动重置布局并重新绘制界面。

### 十三 退出播放器
退出方法有三种：
- 在主界面按`q`键
- 按`Ctrl+C`（播放器会正常退出）
- 在选项菜单中选择"Exit"（即`F9`键）

## 第六章 技术架构
本播放器采用模块化设计，核心代码文件如下：
- **main.c**：程序入口，处理命令行参数解析
- **ui.c**：界面渲染、事件响应、布局管理
- **audio.c**：音频解码、FFmpeg初始化、播放控制
- **playlist.c**：歌单加载、目录扫描、元数据读取
- **progress.c**：播放进度追踪、进度跳转控制
- **lyrics.c**：歌词加载、解析、同步显示逻辑
- **menu_views.c**：各视图管理（设置、历史、收藏、歌单）
- **remote.c**：远程音乐播放（SMB/SFTP/FTP/WebDAV协议）
- **image_loader.c**：专辑封面图片加载处理（PNG/JPEG）
- **braille_art.c**：盲文点阵渲染，在终端显示专辑封面
- **media_session.c**：MPRIS D-Bus媒体会话集成（可选）
- **crypto.c**：远程连接密码加密解密处理
- **logger.c**：日志记录子系统
- **search.c**：异步搜索功能（支持拼音搜索）
- **scrollbar.c**：滚动条模块，在各窗格中复用
- **defs.h**：全局常量、数据结构定义

## 第七章 开源协议
本项目遵循GNU General Public License v3.0开源协议。你可以自由使用、修改、分发本项目，但修改后的衍生作品必须同样遵循该协议开源，不得闭源。

## 第八章 免责声明

Ter-Music 是一款纯粹的音频播放工具，本身不提供、不托管、不分发任何音频文件或其他受版权保护的内容。用户必须自行提供合法获取的音频文件。本软件的本地播放与远程播放功能，仅设计用于播放用户合法获得的媒体文件。

与本软件所播放的音频内容相关的所有版权及知识产权，均归其各自权利人所有。因使用本软件播放音频内容而产生的任何版权纠纷，概由使用者自行承担全部责任。开发者对因使用本软件引起的任何版权或其他法律问题不承担任何责任。

## 第九章 开发者
- **开发者**：燕戏竹林
- **邮箱**：<yxzl666xx@outlook.com>
- **项目仓库**：<https://github.com/YXZL985/ter-music.git>

## 第九章 致谢
在此诚挚感谢以下贡献者的帮助：

- **@guanzi008** - 深入优化多项功能：添加Debian打包元数据、实现可选MPRIS媒体会话集成、完善DEB打包、修复UTF-8输入问题、优化设置引导、支持鼠标交互、规整歌单管理、优化目录排序、修复播放中断问题、提升性能、美化频谱显示、优化中文界面展示
- **@Zeta** - 拓展Arch Linux平台支持

## 第十章 贡献指引
如果你有问题或改进建议，欢迎提交Issue和Pull Request，我们非常欢迎。

## 第十一章 常见问题解答
**问题：没有声音输出**
- 检查PulseAudio是否运行：执行`systemctl status pulseaudio`命令验证
- 检查扬声器音量是否开启
- 确认PulseAudio配置正确

**问题：播放音质不佳、声音断断续续或有杂音**
- 不同机器的音频设备性能不同，默认的输出时延参数可能不适合您的设备
- 请尝试在设置界面（按`F2`进入设置）中调高"输出时延"的值，或直接编辑配置文件`~/.config/ter-music/config.json`中的`audio_latency_ms`字段
- 时延值可调范围为20-250毫秒，建议每次增加10毫秒逐步测试，直至播放恢复正常
- 若调高时延后仍有问题，可检查PulseAudio配置或更新音频驱动

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