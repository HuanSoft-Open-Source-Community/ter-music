# 构建脚本说明

本目录包含所有用于构建、打包和交叉编译 ter-music 的脚本。

## 目录结构

```
scripts/
├── README.md                    # 本文件
├── build/                      # 构建脚本
│   ├── build-appimage.sh      # 构建 AppImage 包
│   ├── build-deb.sh          # 构建 DEB 包
│   ├── build-linyaps.sh      # 构建 Linyaps (如意玲珑) 包
│   ├── build-portable.sh     # 构建可移植压缩包
│   └── build-rpm.sh         # 构建 RPM 包
└── cross-compile/             # 交叉编译相关
    ├── cross-build.sh        # 交叉编译包装脚本
    ├── Dockerfile           # Docker 镜像定义（Debian 13 DEB + ARM64 交叉编译）
    ├── Dockerfile.deb       # Debian DEB 构建环境（支持 Debian 10/11/12/13）
    ├── Dockerfile.rpm       # Rocky Linux RPM 构建环境（支持 EL8/9/10）
    ├── Dockerfile.rpm-static    # 静态链接 RPM 构建环境（Rocky Linux 8）
    └── docker-compose.yml   # Docker Compose 配置
```

## 打包配置目录

项目根目录下的 `packaging/` 目录包含各平台的打包配置：

```
packaging/
├── debian/        # Debian/Ubuntu 打包配置 (debuild 使用)
└── aur/           # Arch Linux AUR 打包配置 (PKGBUILD, .SRCINFO)
```

## 构建脚本使用方法

### 1. build-appimage.sh - 构建 AppImage 包

将 ter-music 打包成 AppImage 格式，可在大多数 Linux 发行版上运行。

**用法：**
```bash
./scripts/build/build-appimage.sh [选项]
```

**选项：**
- `-v, --version VERSION` - 指定版本号（默认自动检测）
- `-a, --arch ARCH` - 指定目标架构（默认自动检测）
- `-r, --rpm FILE` - 从指定 RPM 包转换
- `-k, --keep-temp` - 保留临时文件
- `-h, --help` - 显示帮助信息

**示例：**
```bash
./scripts/build/build-appimage.sh
./scripts/build/build-appimage.sh -v 1.4.1
./scripts/build/build-appimage.sh -a aarch64
```

### 2. build-deb.sh - 构建 DEB 包

将 ter-music 打包成 Debian/Ubuntu 的 DEB 包。

> **推荐使用**：建议优先使用 `--container` 选项在 Docker 容器中构建 DEB 包，可以保证构建环境一致性，避免因宿主系统库版本差异导致的兼容性问题。容器化构建会自动使用 USTC 镜像源加速国内构建。

**用法：**
```bash
./scripts/build/build-deb.sh [选项]
```

**选项：**
- `-v, --version VERSION` - 指定版本号
- `-a, --arch ARCH` - 指定目标架构
- `--with-source` - 生成源码包
- `--with-debuginfo` - 生成 debuginfo 包
- `-k, --keep-temp` - 保留临时文件
- `--container` - **推荐** 在 Docker 容器中构建 DEB（解决跨发行版兼容问题）
- `--debian-version VERSION` - 指定 Debian 版本：10、11、12 或 13（默认 12，需配合 --container）

**示例：**
```bash
./scripts/build/build-deb.sh
./scripts/build/build-deb.sh -v 1.2.3 -a arm64
./scripts/build/build-deb.sh --with-source
./scripts/build/build-deb.sh --container              # 推荐：在容器中构建
./scripts/build/build-deb.sh --container --debian-version 10  # Debian 10 容器
```

### 3. build-linyaps.sh - 构建 Linyaps 包

将 ter-music 打包成如意玲珑 (Linyaps) 格式。

**用法：**
```bash
./scripts/build/build-linyaps.sh [选项]
```

**选项：**
- `-v, --version VERSION` - 指定版本号
- `-a, --arch ARCH` - 指定目标架构
- `-k, --keep-temp` - 保留临时文件

**示例：**
```bash
./scripts/build/build-linyaps.sh
./scripts/build/build-linyaps.sh -v 1.1.2 -a loong64
```

### 4. build-portable.sh - 构建可移植包

创建包含依赖库的可移植压缩包，可在无依赖的 Linux 系统上运行。

**用法：**
```bash
./scripts/build/build-portable.sh [选项]
```

**选项：**
- `-v, --version VERSION` - 指定版本号
- `-a, --arch ARCH` - 指定目标架构
- `-r, --rpm FILE` - 从指定 RPM 包转换
- `-k, --keep-temp` - 保留临时文件

**示例：**
```bash
./scripts/build/build-portable.sh
./scripts/build/build-portable.sh -a aarch64
```

### 5. build-rpm.sh - 构建 RPM 包

将 ter-music 打包成 Fedora/RHEL 的 RPM 包。

**用法：**
```bash
./scripts/build/build-rpm.sh [选项]
```

**选项：**
- `-v, --version VERSION` - 指定版本号
- `-a, --arch ARCH` - 指定目标架构
- `--with-debuginfo` - 生成 debuginfo 包
- `-k, --keep-temp` - 保留临时文件
- `--container` - 在 Rocky Linux 容器中构建 RPM（解决跨发行版兼容问题）
- `--static` - 构建静态链接 RPM，单包兼容 EL8/9/10（自动启用 --container）
- `--el-version VERSION` - 指定目标 EL 版本：8、9 或 10（默认 9，需配合 --container）

**示例：**
```bash
./scripts/build/build-rpm.sh
./scripts/build/build-rpm.sh -v 1.2.3 -a loong64
./scripts/build/build-rpm.sh --container --el-version 10
./scripts/build/build-rpm.sh --static
```

## 交叉编译

### cross-build.sh - 交叉编译包装脚本

在 Docker 容器中进行交叉编译。

**用法：**
```bash
./scripts/cross-compile/cross-build.sh [选项] -- [构建脚本参数]
```

**选项：**
- `-b, --build-image` - 重新构建 Docker 镜像
- `-s, --script SCRIPT` - 指定构建脚本（默认 build-deb.sh）
- `-a, --arch ARCH` - 指定目标架构（默认 arm64）
- `-f, --dockerfile DOCKERFILE` - 指定 Dockerfile 路径（默认 scripts/cross-compile/Dockerfile）
- `-n, --image-name NAME` - 指定 Docker 镜像名（默认 ter-music-cross）
- `--build-arg KEY=VALUE` - 传递构建参数给 docker build
- `-i, --interactive` - 进入交互式 shell
- `--no-cache` - 构建镜像时不使用缓存

**示例：**
```bash
./scripts/cross-compile/cross-build.sh
./scripts/cross-compile/cross-build.sh -a arm64
./scripts/cross-compile/cross-build.sh -s build-rpm.sh
./scripts/cross-compile/cross-build.sh -s build-rpm.sh -f scripts/cross-compile/Dockerfile.rpm --build-arg EL_VERSION=9
./scripts/cross-compile/cross-build.sh -i
```

### Dockerfile

用于构建交叉编译环境的 Docker 镜像，基于 Debian 13 (Trixie)，包含 DEB 打包工具和 ARM64 交叉编译工具链。

### Dockerfile.deb

用于在 Docker 容器中构建 DEB 包，支持 Debian 10/11/12/13 多个版本。
- 通过 `DEBIAN_VERSION` build arg 指定 Debian 版本（默认 12）
- 使用 USTC 镜像源加速国内构建
- 支持 ARM64 交叉编译
- **建议在 CI/CD 中使用此文件进行容器化构建，以确保构建环境一致性**

### Dockerfile.rpm

用于在 Rocky Linux 容器中构建 RPM 包，确保自动生成的 soname 依赖与目标 RHEL 平台一致。
- 通过 `EL_VERSION` build arg 支持 EL8、EL9 和 EL10
- 使用 USTC 镜像源加速国内构建

### Dockerfile.rpm-static

用于静态链接构建，基于 Rocky Linux 8（glibc 2.28，兼容范围最广）。
- 从源码编译 FFmpeg 7.1（仅音频解码器）
- 静态链接 FFmpeg，动态链接其他系统库
- 生成的 RPM 无 FFmpeg soname 依赖，单包兼容 RHEL 8/9/10
- 使用 USTC 镜像源加速国内构建

### docker-compose.yml

Docker Compose 配置文件，用于快速启动交叉编译环境。

## 支持的架构

所有构建脚本支持以下架构：
- **x86_64** - Intel/AMD 64位
- **aarch64/arm64** - ARM 64位
- **loong64** - 龙芯新世界
- **loongarch64** - 龙芯旧世界
- **sw64** - 申威
- **mips64** - MIPS 64位

## 输出目录

所有构建输出都在 `build/` 目录下：
```
build/
├── appimage/     # AppImage 包
├── deb/          # DEB 包
├── linyaps/      # Linyaps 包
├── portable/     # 可移植包
└── rpm/          # RPM 包
```

## 注意事项

1. 所有脚本都使用相对路径，必须从项目根目录运行
2. 交叉编译需要 Docker 环境
3. 某些包格式可能需要特定的构建依赖
4. 建议使用 `-k, --keep-temp` 选项进行调试

## 依赖检查

各脚本会自动检查所需的构建依赖，如果缺少依赖会给出安装提示。

**常见依赖：**
- cmake, make, gcc
- pkg-config
- 开发库：libavcodec-dev, libavformat-dev, libswresample-dev, libswscale-dev, libavutil-dev, libavfilter-dev, libpulse-dev, libncurses-dev, libcurl4-openssl-dev
- 打包工具：dpkg-dev, rpm-build, linglong-builder 等
