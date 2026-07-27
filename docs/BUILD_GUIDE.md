# Ter-Music 构建脚本

这个项目提供了多个构建脚本，用于将 Ter-Music 打包成不同的格式。

> 💡 **推荐使用 `launch-auto-build.sh` 一键构建** — 自动构建所有包类型、管理 Docker 镜像，无需逐条执行各个构建脚本。

## 一键构建（推荐）

### launch-auto-build.sh — 一键构建所有包类型

位于 `scripts/build/launch-auto-build.sh`，是构建 ter-music 所有包格式的统一入口。

**核心设计：** 两阶段执行。先构建所有需要的 Docker 镜像（已存在则跳过），再依次构建所有包。
支持交互式和 CLI 两种模式。

**用法：**
```bash
./scripts/build/launch-auto-build.sh [选项]
```

**选项：**

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `-v, --version VERSION` | 指定版本号 | 自动检测 |
| `-a, --arch ARCH` | 目标架构：`amd64` | `amd64` |
| `-t, --types TYPES` | 包类型（逗号分隔）：`deb,rpm,linyaps,appimage,portable` | 全部 |
| `-k, --keep-temp` | 保留临时构建文件 | 关闭 |
| `--skip-images` | 跳过 Docker 镜像预构建 | 关闭 |
| `--rebuild-images` | 强制重新构建 Docker 镜像 | 关闭 |
| `--skip-builds` | 仅构建 Docker 镜像，跳过包构建 | 关闭 |
| `--fail-fast` | 遇构建失败立即停止 | 关闭 |
| `--no-docker` | 跳过依赖 Docker 的构建（deb/rpm） | 关闭 |

**默认构建矩阵：**

| 包类型 | amd64 |
|--------|-------|
| deb | 容器构建（静态链接 + 源码包，`Dockerfile.deb-static`） |
| rpm | 容器构建（静态链接，`Dockerfile.rpm-static`） |
| linyaps | 容器构建（Docker，`Dockerfile.uab`） |
| appimage | 本地构建 |
| portable | 本地构建 |

**示例：**
```bash
# 交互模式（不带参数运行，会提示输入各项配置）
./scripts/build/launch-auto-build.sh

# 指定版本，构建全部包（amd64 默认）
./scripts/build/launch-auto-build.sh -v 2.1.0

# 指定包类型
./scripts/build/launch-auto-build.sh -v 2.1.0 -t deb,rpm

# 跳过 Docker 镜像预构建（镜像已存在时加速）
./scripts/build/launch-auto-build.sh -v 2.1.0 --skip-images

# 仅构建 Docker 镜像，不构建包
./scripts/build/launch-auto-build.sh --rebuild-images --skip-builds
```

## 可用的构建脚本

### 1. build-rpm.sh - 构建 RPM 包
将项目构建为标准的 Fedora RPM 包。

**容器构建模式**（推荐用于非 RHEL 系统）：
```bash
# 在 Rocky Linux 容器中构建（默认 EL9）
./build-rpm.sh --container

# 在指定 EL 版本的容器中构建
./build-rpm.sh --container --el-version 8
./build-rpm.sh --container --el-version 10

# 构建静态链接 RPM，单包兼容 EL8/9/10（自动启用容器模式）
./build-rpm.sh --static
```

> **💡 强烈建议**：优先构建 **EL8** 版本（即 `--el-version 8`）。EL8 基于 glibc 2.28 构建，该版本是 EL8/9/10 三者间的最低公共版本，因此生成的 RPM 可在所有三个 EL 主版本上直接运行，无需为每个版本分别构建。若构建 EL9 或 EL10 版本，则会因 glibc 要求更高而无法在更低版本的系统中安装。

**本地构建（仅在 RHEL/Fedora 系统上）：**
```bash
# 使用默认版本号和架构构建
./build-rpm.sh

# 保留临时文件用于调试
./build-rpm.sh --keep-temp

# 显示帮助信息
./build-rpm.sh --help
```
**输出：**
- RPM 包将输出到 `build/rpm/<arch>/` 目录
- 默认只生成主包，使用 `--with-debuginfo` 选项可同时生成 debuginfo 包和 debugsource 包
**安装：**
```bash
sudo dnf install build/rpm/x86_64/ter-music-*.x86_64.rpm
```

### 7. tools/start-server.py — 测试服务器工具
启动本地 SMB/FTP/SFTP/WebDAV/HTTP 服务器，用于测试远程音乐播放功能。

> **Python 环境要求**：该脚本为 Python 脚本，建议在 Conda 环境中运行，避免依赖冲突。

**Conda 环境配置（首次使用）：**
```bash
# 安装 Miniconda3（如尚未安装）
# 请访问 https://docs.anaconda.com/miniconda/ 下载安装

# 创建名为 ter-music 的虚拟环境并安装 Python
conda create -n ter-music python=3

# 激活虚拟环境
conda activate ter-music

# 安装依赖
pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r tools/requirements.txt
```

**使用方法（已配置好环境后）：**
```bash
# 确保已激活 conda 环境
conda activate ter-music

# 启动交互式菜单
python3 tools/start-server.py
```
按提示选择协议、配置端口和共享目录即可启动。支持匿名访问（FTP/SMB/HTTP）、密码认证（SFTP/WebDAV）和公钥认证（SFTP）。

### 2. build-appimage.sh - 构建 AppImage 包
直接从源码构建 AppImage 格式（也可从 RPM 转换，需要 FUSE 支持）。
**使用方法：**
```bash
# 自动检测版本和架构，直接从源码构建
./build-appimage.sh

# 指定版本号构建
./build-appimage.sh -v 1.4.1

# 从指定 RPM 包文件转换
./build-appimage.sh -r build/rpm/x86_64/ter-music-1.0.0-1.x86_64.rpm

# 保留临时文件用于调试
./build-appimage.sh --keep-temp

# 显示帮助信息
./build-appimage.sh --help
```
**输出：**
- AppImage 包将输出到: `build/appimage/<arch>/` 目录

**使用：**
```bash
# 直接运行
./build/appimage/ter-music-1.0.0-x86_64.AppImage

# 或者先添加执行权限
chmod +x build/appimage/ter-music-1.0.0-x86_64.AppImage
./build/appimage/ter-music-1.0.0-x86_64.AppImage
```

**注意：**
- AppImage 需要 FUSE 支持
- 如果系统缺少 `libfuse.so.2`，AppImage 可能无法直接运行
- 建议使用可移植包作为替代方案

### 3. build-portable.sh - 构建可移植压缩包
直接从源码构建可移植的 tar.gz 压缩包，包含所有必要的依赖库（也可从 RPM 转换）。
**使用方法：**
```bash
# 自动检测版本和架构，直接从源码构建
./build-portable.sh

# 指定版本号构建
./build-portable.sh -v 1.4.1

# 从指定 RPM 包文件转换
./build-portable.sh -r build/rpm/x86_64/ter-music-1.0.0-1.x86_64.rpm

# 保留临时文件用于调试
./build-portable.sh --keep-temp

# 显示帮助信息
./build-portable.sh --help
```
**输出：**
- 可移植包将输出到: `build/portable/<arch>/` 目录

**使用：**
```bash
# 解压
tar -xzf build/portable/ter-music-1.0.0-portable-x86_64.tar.gz

# 进入目录
cd ter-music-portable

# 运行
./run.sh
```

**优点：**
- 不需要 FUSE 支持
- 包含所有必要的依赖库
- 可以在任何兼容的 Linux 系统上运行
- 不需要安装任何依赖

### 4. build-linyaps.sh - 构建 Linyaps（如意玲珑）包

通过 Docker 容器构建 Linyaps（如意玲珑）UAB 包，适合 deepin 等使用玲珑包管理的系统。

> **需要 Docker**：Linyaps 构建依赖 `ll-builder`，该工具需要对 `/usr` 的写入权限。为避免污染宿主系统，构建在 Docker 容器（Debian 13）中进行。

**使用方法：**
```bash
# 推荐：通过 launch-auto-build.sh 一键构建
./scripts/build/launch-auto-build.sh -t linyaps -v 2.1.0

# 或手动调用（需 Docker）
./scripts/docker/docker-build.sh -p \
  -s build-linyaps.sh \
  -f scripts/docker/Dockerfile.uab \
  -n ter-music-uab-builder \
  -- -v 2.1.0 -a x86_64 --in-container

# 进入容器交互式调试
./scripts/docker/docker-build.sh -p -i \
  -f scripts/docker/Dockerfile.uab \
  -n ter-music-uab-builder
```

**选项（直接调用 build-linyaps.sh 时）：**
| 选项 | 说明 |
|------|------|
| `-v, --version VERSION` | 指定版本号 |
| `-a, --arch ARCH` | 目标架构（默认 x86_64） |
| `-k, --keep-temp` | 保留临时构建文件 |
| `--in-container` | 在 Docker 容器内运行，跳过宿主机依赖检查 |

**输出：**
- UAB 包输出到: `build/linyaps/<arch>/org.yxzl.ter-music_<version>_<arch>.uab`
- 同步复制到: `build/release/`

**安装：**
```bash
ll-cli install build/linyaps/x86_64/org.yxzl.ter-music_2.1.0_x86_64.uab
ll-cli run org.yxzl.ter-music
```

**Docker 镜像说明：**
- 镜像名：`ter-music-uab-builder`
- 基础：Debian 13 (trixie)，使用 USTC 镜像源
- 预装：`linglong-bin`、`linglong-installer`、`linglong-builder`、`xdg-utils`、`rsync`（构建依赖由 ll-builder 容器内自动安装）
- Dockerfile 路径：`scripts/docker/Dockerfile.uab`
- 容器以 `--privileged` 模式运行（`ll-builder` 需要 user namespace 支持）
- 产物所有权通过 `fix_output_ownership()` 自动修复为宿主用户

### 5. build-deb.sh - 构建 DEB 包
将项目构建为标准的 Debian/Ubuntu DEB 包，适合 Debian、Ubuntu、Linux Mint、deepin 等基于 Debian 的发行版。

> **推荐使用**：建议优先使用 `--container` 或 `--static` 选项在 Docker 容器中构建，可以保证构建环境一致性，避免因宿主系统库版本差异导致的兼容性问题。

**用法：**
```bash
# 使用自动检测的版本号和架构构建 DEB
./build-deb.sh

# 指定版本号构建
./build-deb.sh -v 1.4.1

# 在 Docker 容器中构建（推荐，确保环境一致性）
./build-deb.sh --container
./build-deb.sh --container --debian-version 10   # 指定 Debian 版本

# 静态链接 FFmpeg，单包兼容 Debian 10/11/12/13+
./build-deb.sh --static

# 同时生成源码包和 debuginfo 包
./build-deb.sh --with-source --with-debuginfo

# 保留临时文件用于调试
./build-deb.sh --keep-temp

# 显示帮助信息
./build-deb.sh --help
```
**选项说明：**
- `--container`：在 Docker 容器中构建，避免宿主系统库差异
- `--debian-version VERSION`：指定 Debian 版本（10/11/12/13，默认 12），需配合 `--container`
- `--static`：静态链接 FFmpeg，消除 soname 依赖，单包兼容多个 Debian 版本（自动使用 Debian 10 容器）
- `--with-source`：同时生成源码包
- `--with-debuginfo`：生成 debuginfo 包
**输出：**
- DEB 包将输出到: `build/deb/<arch>/` 目录
**安装：**
```bash
sudo dpkg -i build/deb/amd64/ter-music_*_amd64.deb
# 如果缺少依赖，请运行：
sudo apt install -f
```

### 6. PKGBUILD - 构建 Arch Linux 包
将项目构建为标准的 Arch Linux 包，适合 Arch Linux 和 Arch-based 发行版。
**使用方法：**
```bash
# 从 AUR 克隆 PKGBUILD
git clone https://aur.archlinux.org/ter-music-cn.git
cd ter-music-cn

# 构建并安装
makepkg -si

# 或者只构建不安装
makepkg

# 安装已构建的包
sudo pacman -U ter-music-cn-*.pkg.tar.zst
```
**支持的架构：**
- x86_64: Intel/AMD 64位

**输出：**
- Arch Linux 包将输出到当前目录

**安装：**
```bash
sudo pacman -U ter-music-cn-*.pkg.tar.zst
```

**优点：**
- 标准 Arch Linux 包格式
- 自动处理依赖关系
- 可通过 `pacman` 工具管理安装和卸载
- 适合 Arch Linux 用户

## 构建依赖

### build-rpm.sh 依赖：
- `rpm-build`
- `gcc`
- `make`
- `cmake`
- `pkg-config`
- `ffmpeg-free-devel`（提供 libavfilter/libavcodec/libavformat/libswresample/libswscale/libavutil，非静态构建时必需）
- `pulseaudio-libs-devel`
- `ncurses-devel`
- `libpng-devel`
- `libjpeg-turbo-devel`
- `libxml2-devel`
- `sqlite-devel`
- `libcurl-devel`
- Docker（容器构建模式时必需）

> **静态构建（`--static`）**：FFmpeg 在 Docker 容器中从源码编译，无需 `ffmpeg-free-devel` 包。
> 二进制文件静态链接 FFmpeg，动态链接其他系统库，单包兼容 RHEL 8/9/10。

> **可选后端**：PipeWire（`pipewire-devel`，dlopen 加载，无编译时依赖）、ALSA（`alsa-lib-devel`）、DBus（`dbus-devel`，MPRIS 集成）为可选依赖，CMake 会自动检测。

### build-appimage.sh 依赖：
- `squashfs-tools`
- `cmake`
- `make`
- `gcc`
- `wget` 或 `curl`
- FUSE（用于运行 AppImage）
- `rpm2cpio` 和 `cpio`（仅当从 RPM 转换时需要）

### build-portable.sh 依赖：
- `cmake`
- `make`
- `gcc`
- `tar`
- `rpm2cpio` 和 `cpio`（仅当从 RPM 转换时需要）

### build-linyaps.sh 依赖：
- Docker（必需，构建在容器中进行）
- 无需宿主机安装 linglong 相关包

容器内自动处理以下依赖（由 `Dockerfile.uab` 定义）：
- `linglong-builder` (ll-builder)
- `linglong-bin`
- `linglong-installer`
- `cmake`、`make`、`gcc`
- FFmpeg 开发库
- ncurses、pulseaudio、sqlite、curl、png、jpeg、xml2、dbus 等开发库

### build-deb.sh 依赖：
- `dpkg-dev`
- `fakeroot`
- `devscripts`
- `cmake`
- `make`
- `gcc`
- `libncurses-dev`
- `libpulse-dev`
- `libcurl4-openssl-dev`
- `libpng-dev`
- `libjpeg-dev`
- `libxml2-dev`
- `libsqlite3-dev`
- `libdbus-1-dev`
- FFmpeg 开发库：`libavcodec-dev`、`libavformat-dev`、`libavutil-dev`、`libswresample-dev`、`libswscale-dev`、`libavfilter-dev`
- Docker（容器或静态构建模式时必需）

> **静态构建（`--static`）**：FFmpeg 在 Docker 容器中从源码编译，无需系统 FFmpeg dev 包。二进制文件静态链接 FFmpeg，动态链接其他系统库，单包兼容 Debian 10/11/12/13+。

> **可选后端**：PipeWire（`libpipewire-0.3-dev`，dlopen 加载，无编译时依赖）、ALSA（`libasound2-dev`）为可选依赖。

### PKGBUILD 依赖：
- `base-devel`
- `cmake`
- `gcc`
- `make`
- `git`
- `ffmpeg`
- `pulseaudio`
- `ncurses`
- `libxml2`
- `sqlite`
- `zlib`

在 Debian/Ubuntu 上安装构建依赖：
```bash
sudo apt install dpkg-dev fakeroot cmake make gcc libavfilter-dev libpng-dev libjpeg-dev libswscale-dev libxml2-dev libsqlite3-dev zlib1g-dev
```

## 推荐的构建流程

**推荐使用 `launch-auto-build.sh` 一键构建所有包类型（最便捷的方式）：**

```bash
# 一键构建所有包（交互模式）
./scripts/build/launch-auto-build.sh

# 指定版本，自动构建全部包
./scripts/build/launch-auto-build.sh -v 2.1.0

# 跳过 Docker 镜像预构建（镜像已存在时加速）
./scripts/build/launch-auto-build.sh -v 2.1.0 --skip-images
```

如果只需构建单个包类型，也可以直接使用对应的构建脚本：

- 直接指定版本号构建可移植包（推荐，兼容性最好）：
  ```bash
  ./build-portable.sh -v 1.4.1
  ```

- 直接指定版本号构建 AppImage（推荐）：
  ```bash
  ./build-appimage.sh -v 1.4.1
  ```

- 自动检测版本直接构建：
  ```bash
  ./build-portable.sh
  ./build-appimage.sh
  ```

- 如果需要构建 RPM 包，可以先构建 RPM 再转换：
  ```bash
  ./build-rpm.sh -v 1.4.1
  ./build-portable.sh -r build/rpm/ter-music-*.rpm
  ```

## 分发建议

- **RPM 包**：适合 Fedora/RHEL 系统用户，可以通过包管理器安装
- **DEB 包**：适合 Debian/Ubuntu 系发行版（Ubuntu、Linux Mint、deepin 等），可以通过 dpkg/apt 安装
- **AppImage**：适合支持 FUSE 的 Linux 系统，单文件分发
- **可移植包**：适合所有 Linux 系统，兼容性最好
- **Arch Linux 包**：适合 Arch Linux 和 Arch-based 发行版，可以通过 pacman 或 AUR 安装

## 非 x86 架构构建

非 x86 架构（如 arm64、loong64、sw64、mips64el 等）的软件包由 OBS 构建服务器统一构建和维护。

**OBS 仓库链接：** [OBS 构建服务器](https://obs22.odata.cc/package/show/home:Admin:app/ter-music)

如需为其他架构构建软件包，请直接使用 OBS 服务器，无需在本地配置交叉编译环境。

## 故障排除

### RPM 包安装失败
如果遇到依赖问题，请确保系统已安装所有必要的开发包：
```bash
sudo dnf install ffmpeg-free-devel pulseaudio-libs-devel ncurses-devel libcurl-devel libxml2-devel libpng-devel libjpeg-turbo-devel sqlite-devel
```

**跨发行版构建的 RPM（在 Debian 上构建）**：如果在非 RHEL 系统上构建了 RPM，安装到 RHEL 时可能出现 FFmpeg soname 或 glibc 版本不匹配问题。解决方案：
- 使用 `./build-rpm.sh --container` 在 Rocky Linux 容器中构建
- 使用 `./build-rpm.sh --static` 构建静态链接 RPM，自动兼容 EL8/9/10

### AppImage 无法运行
如果遇到 FUSE 相关错误，请尝试：
1. 安装 FUSE：`sudo dnf install fuse`
2. 或者使用可移植包作为替代

### 可移植包运行失败
确保解压后的目录结构完整，并且 `run.sh` 脚本有执行权限。