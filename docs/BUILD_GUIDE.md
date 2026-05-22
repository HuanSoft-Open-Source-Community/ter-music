# Ter-Music 构建脚本

这个项目提供了多个构建脚本，用于将 Ter-Music 打包成不同的格式。

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

# 指定版本号构建
./build-rpm.sh -v 1.2.3

# 指定目标架构
./build-rpm.sh -a arm64

# 生成 debuginfo 包（默认不生成）
./build-rpm.sh --with-debuginfo

# 保留临时文件用于调试
./build-rpm.sh --keep-temp

# 显示帮助信息
./build-rpm.sh --help
```
**支持的架构：**
- x86_64: Intel/AMD 64位
- arm64: ARM 64位
- loong64: 龙芯新世界
- loongarch64: 龙芯旧世界
- sw64: 申威
- mips64: MIPS 64位

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

# 指定目标架构
./build-appimage.sh -a aarch64

# 指定版本和架构
./build-appimage.sh -v 1.4.1 -a aarch64

# 从指定 RPM 包文件转换
./build-appimage.sh -r build/rpm/x86_64/ter-music-1.0.0-1.x86_64.rpm

# 保留临时文件用于调试
./build-appimage.sh --keep-temp

# 显示帮助信息
./build-appimage.sh --help
```
**支持的架构：**
- x86_64: Intel/AMD 64位
- aarch64: ARM 64位
- loong64: 龙芯（包括新世界和旧世界）
- loongarch64: 龙芯旧世界
- sw64: 申威
- mips64: MIPS 64位

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

# 指定目标架构
./build-portable.sh -a aarch64

# 指定版本和架构
./build-portable.sh -v 1.4.1 -a aarch64

# 从指定 RPM 包文件转换
./build-portable.sh -r build/rpm/x86_64/ter-music-1.0.0-1.x86_64.rpm

# 保留临时文件用于调试
./build-portable.sh --keep-temp

# 显示帮助信息
./build-portable.sh --help
```
**支持的架构：**
- x86_64: Intel/AMD 64位
- aarch64: ARM 64位
- loong64: 龙芯（包括新世界和旧世界）
- loongarch64: 龙芯旧世界
- sw64: 申威
- mips64: MIPS 64位

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
直接从源码构建 Linyaps（如意玲珑）格式包，适合 deepin 等使用玲珑包管理的系统。
**使用方法：**
```bash
# 使用默认版本号和架构构建
./build-linyaps.sh

#指定版本号构建
./build-linyaps.sh -v 1.2.3

# 指定目标架构
./build-linyaps.sh -a arm64

# 指定版本和架构
./build-linyaps.sh -v 1.2.3 -a loong64

# 保留临时文件用于调试
./build-linyaps.sh --keep-temp

# 显示帮助信息
./build-linyaps.sh --help
```
**支持的架构：**
- x86_64: Intel/AMD 64位
- arm64: ARM 64位
- loong64: 龙芯（包括新世界和旧世界）
- mips64: MIPS 64位
- sw64: 申威

**输出：**
- UAB 包和 layer 文件将输出到: `build/linyaps/<arch>/` 目录

**安装：**
```bash
# 使用 ll-cli 安装
ll-cli install build/linyaps/org.yxzl.ter-music_1.0.0_x86_64.uab

# 运行
ll-cli run org.yxzl.ter-music
```

**优点：**
- 符合 Linyaps 打包规范
- 自动处理依赖关系
- 适合 deepin/UOS 系统用户

### 5. build-deb.sh - 构建 DEB 包
将项目构建为标准的 Debian/Ubuntu DEB 包，适合 Debian、Ubuntu、Linux Mint、deepin 等基于 Debian 的发行版。

> **推荐使用**：建议优先使用 `--container` 或 `--static` 选项在 Docker 容器中构建，可以保证构建环境一致性，避免因宿主系统库版本差异导致的兼容性问题。

**用法：**
```bash
# 使用自动检测的版本号和架构构建 DEB
./build-deb.sh

# 指定版本号构建
./build-deb.sh -v 1.4.1

# 指定目标架构
./build-deb.sh -a arm64

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
**支持的架构：**
- amd64: Intel/AMD 64位
- arm64: ARM 64位
- loong64: 龙芯新世界
- loongarch64: 龙芯旧世界
- sw64: 申威
- mips64el: MIPS 64位小端
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
- i686: Intel/AMD 32位

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
- `libcurl-devel`
- Docker（容器构建模式时必需）

> **静态构建（`--static`）**：FFmpeg 在 Docker 容器中从源码编译，无需 `ffmpeg-free-devel` 包。
> 二进制文件静态链接 FFmpeg，动态链接其他系统库，单包兼容 RHEL 8/9/10。

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
- `linglong-builder` (ll-builder)
- `cmake`
- `make`
- `pkg-config`
- `libncurses-dev`
- `libavformat-dev`
- `libavcodec-dev`
- `libswresample-dev`
- `libavutil-dev`
- `libtag1-dev`
- `libpulse-dev`

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
- `libdbus-1-dev`
- FFmpeg 开发库：`libavcodec-dev`、`libavformat-dev`、`libavutil-dev`、`libswresample-dev`、`libswscale-dev`、`libavfilter-dev`
- Docker（容器或静态构建模式时必需）

> **静态构建（`--static`）**：FFmpeg 在 Docker 容器中从源码编译，无需系统 FFmpeg dev 包。二进制文件静态链接 FFmpeg，动态链接其他系统库，单包兼容 Debian 10/11/12/13+。

### PKGBUILD 依赖：
- `base-devel`
- `cmake`
- `gcc`
- `make`
- `git`
- `ffmpeg`
- `pulseaudio`
- `ncurses`
- `libao`
- `libmad`
- `libid3tag`

在 Debian/Ubuntu 上安装构建依赖：
```bash
sudo apt install dpkg-dev fakeroot cmake make gcc libavfilter-dev libpng-dev libjpeg-dev libswscale-dev
```

## 交叉编译支持

所有构建脚本都支持在 x86_64/amd64 机器上通过交叉编译工具链构建 aarch64/arm64 架构的包。

### 方式一：使用 Docker 容器（推荐）

为了避免污染主机系统，推荐使用 Docker 容器进行交叉编译。

**前置要求：** 安装 Docker
```bash
sudo apt install docker.io
```

**1. 构建 Docker 镜像**
```bash
# 使用提供的脚本构建镜像（会自动构建 Dockerfile.cross-build）
./cross-build.sh -b

# 构建指定 Dockerfile 的镜像
./cross-build.sh -b -f scripts/cross-compile/Dockerfile.rpm --build-arg EL_VERSION=9
```

**2. 在容器中运行交叉编译**
```bash
# 构建 arm64 DEB 包（默认）
./cross-build.sh

# 构建 arm64 RPM 包
./cross-build.sh -s build-rpm.sh

# 构建 aarch64 AppImage
./cross-build.sh -s build-appimage.sh -a aarch64

# 构建可移植包
./cross-build.sh -s build-portable.sh -a arm64

# 使用指定 Dockerfile 和镜像名
./cross-build.sh -s build-rpm.sh -f scripts/cross-compile/Dockerfile.rpm -n ter-music-rpm-el9

# 传递构建参数给 docker build
./cross-build.sh -s build-rpm.sh -f scripts/cross-compile/Dockerfile.rpm --build-arg EL_VERSION=10

# 传递额外参数给构建脚本
./cross-build.sh -- --keep-temp
```

**3. 进入交互式容器 shell**
```bash
./cross-build.sh -i
```

**4. 使用 docker-compose（可选）**
```bash
# 构建镜像
docker-compose -f docker-compose.cross.yml build

# 运行（需要手动指定命令）
docker-compose -f docker-compose.cross.yml run --rm cross-build ./build-deb.sh -a arm64
```

**容器环境包含：**
- Ubuntu 22.04 基础系统
- ARM64 交叉编译工具链（gcc, g++, binutils）
- ARM64 架构的开发库（libavcodec, libavformat, libswresample, libavutil, libavfilter, libpng, libjpeg, libpulse, ncurses）
- 各种包格式构建工具（dpkg-dev, rpm, squashfs-tools 等）

### Dockerfile.rpm — RHEL 容器构建

用于在 Rocky Linux 容器中构建 RPM 包，确保依赖与目标 RHEL 平台一致。
- 通过 `EL_VERSION` build arg 选择 EL8、EL9 或 EL10
- 使用 USTC 镜像源加速国内构建
- 搭配 `build-rpm.sh --container` 使用

### Dockerfile.rpm-static — RPM 静态链接构建

基于 Rocky Linux 8（glibc 2.28，兼容最广），从源码编译 FFmpeg 7.1 静态库。
- 仅含音频解码器，最小化配置（`--disable-everything`）
- 静态链接 FFmpeg，动态链接其他系统库（soname 在 EL 版本间稳定）
- 生成的 RPM 单包兼容 RHEL 8/9/10，无 FFmpeg soname 依赖
- 使用 USTC 镜像源加速国内构建
- 搭配 `build-rpm.sh --static` 使用

### Dockerfile.deb — Debian DEB 容器构建

用于在 Docker 容器中构建 DEB 包，支持 Debian 10/11/12/13 多个版本。
- 通过 `DEBIAN_VERSION` build arg 选择 Debian 版本（默认 12）
- Debian 10 使用阿里云归档镜像（buster EOL），11+ 使用 USTC 镜像
- 安装完整的构建工具链和 FFmpeg 开发库
- 搭配 `build-deb.sh --container` 使用

### Dockerfile.deb-static — DEB 静态链接构建

基于 Debian 10（glibc 2.28，兼容最广），从源码编译 FFmpeg 7.1 静态库。
- 仅含音频解码器，最小化配置（`--disable-everything`）
- 静态链接 FFmpeg，动态链接其他系统库（soname 在 Debian 版本间稳定）
- 生成的 DEB 单包兼容 Debian 10/11/12/13+，无 FFmpeg soname 依赖
- 使用阿里云归档镜像加速国内构建
- 搭配 `build-deb.sh --static` 使用

### 方式二：在主机上直接交叉编译

如果你确定要在主机上进行交叉编译：

**安装交叉编译工具链**

在 Debian/Ubuntu 上：
```bash
# 安装交叉编译工具链
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu

# 添加 arm64 架构支持
sudo dpkg --add-architecture arm64
sudo apt update

# 安装目标架构的开发库
sudo apt install libncurses-dev:arm64 libavcodec-dev:arm64 libavformat-dev:arm64 \
                 libswresample-dev:arm64 libswscale-dev:arm64 libavutil-dev:arm64 libavfilter-dev:arm64 \
                 libpng-dev:arm64 libjpeg-dev:arm64 libpulse-dev:arm64
```

⚠️ **警告**：在主机上添加多架构支持可能会卸载某些 amd64 软件包，导致系统不稳定！建议在测试环境或虚拟机中执行。

### 使用交叉编译构建

**推荐使用容器方式**（见上方"方式一"），以下是在主机上直接编译的方法：

只需指定目标架构为 arm64/aarch64，脚本会自动检测并使用交叉编译：

```bash
# 构建 arm64 架构的 DEB 包
./build-deb.sh -a arm64

# 构建 arm64 架构的 RPM 包
./build-rpm.sh -a arm64

# 构建 aarch64 架构的 AppImage
./build-appimage.sh -a aarch64

# 构建 aarch64 架构的可移植包
./build-portable.sh -a aarch64

# 构建 arm64 架构的 Linyaps 包
./build-linyaps.sh -a arm64
```

脚本会自动：
1. 检测主机架构与目标架构是否不同
2. 检查交叉编译工具链是否已安装
3. 设置交叉编译环境变量（CC, CXX, AR, PKG_CONFIG_PATH 等）
4. 使用 CMake 工具链文件进行交叉编译

### 验证交叉编译结果

构建完成后，可以使用 `file` 命令验证生成的二进制文件架构：

```bash
# 检查 DEB 包中的二进制文件
dpkg-deb -x build/deb/arm64/ter-music_*.deb /tmp/ter-music-test
file /tmp/ter-music-test/usr/bin/ter-music
# 应显示: ELF 64-bit LSB executable, ARM aarch64

# 检查 RPM 包中的二进制文件
rpm2cpio build/rpm/arm64/ter-music-*.rpm | cpio -idmv -D /tmp/ter-music-test
file /tmp/ter-music-test/usr/bin/ter-music

# 检查可移植包中的二进制文件
tar -xzf build/portable/aarch64/ter-music-*-portable-aarch64.tar.gz -C /tmp
cd /tmp/ter-music-portable
file bin/ter-music
```

## 推荐的构建流程

现在可以直接构建可移植包或 AppImage，不需要先构建 RPM。推荐直接指定版本号构建：

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

## 故障排除

### RPM 包安装失败
如果遇到依赖问题，请确保系统已安装所有必要的开发包：
```bash
sudo dnf install ffmpeg-free-devel pulseaudio-libs-devel ncurses-devel libcurl-devel
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