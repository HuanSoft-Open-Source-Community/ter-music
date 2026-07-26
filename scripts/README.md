# 构建脚本说明

本目录包含所有用于构建、打包和交叉编译 ter-music 的脚本。

## 目录结构

```
scripts/
├── README.md                    # 本文件
├── build/                      # 构建脚本
│   ├── launch-auto-build.sh   # ★ 一键构建所有包类型（推荐入口）
│   ├── build-appimage.sh      # 构建 AppImage 包
│   ├── build-deb.sh          # 构建 DEB 包
│   ├── build-linyaps.sh      # 构建 Linyaps (如意玲珑) 包
│   ├── build-portable.sh     # 构建可移植压缩包
│   └── build-rpm.sh         # 构建 RPM 包
└── cross-compile/             # Docker 构建环境
    ├── cross-build.sh        # Docker 构建包装脚本
    ├── Dockerfile.deb-static # 静态链接 DEB 构建环境（Debian 10，FFmpeg 从源码编译）
    ├── Dockerfile.rpm        # Rocky Linux RPM 构建环境（支持 EL8/9/10）
    └── Dockerfile.rpm-static # 静态链接 RPM 构建环境（Rocky Linux 8）
```

## 打包配置目录

项目根目录下的 `packaging/` 目录包含各平台的打包配置：

```
packaging/
├── debian/        # Debian/Ubuntu 打包配置 (debuild 使用)
└── aur/           # Arch Linux AUR 打包配置 (PKGBUILD, .SRCINFO)
```

## 构建脚本使用方法

### 0. launch-auto-build.sh — 一键构建所有包类型（推荐入口）

一键构建 ter-music 所有支持的包格式。支持交互式和 CLI 两种模式：
自动管理 Docker 镜像（先构建镜像再构建包），无需手动逐条执行各个构建脚本。

**用法：**
```bash
./scripts/build/launch-auto-build.sh [选项]
```

**选项：**
- `-v, --version VERSION` — 指定版本号（默认自动检测）
- `-a, --arch ARCH` — 目标架构：`amd64`, `arm64`（逗号分隔，默认 amd64,arm64）
- `-t, --types TYPES` — 包类型：`deb,rpm,linyaps,appimage,portable`（逗号分隔，默认全部）
- `-k, --keep-temp` — 保留临时文件
- `--skip-images` — 跳过 Docker 镜像预构建
- `--rebuild-images` — 强制重新构建 Docker 镜像
- `--skip-builds` — 仅构建 Docker 镜像，跳过包构建
- `--fail-fast` — 遇构建失败立即停止
- `--no-docker` — 跳过依赖 Docker 的构建（deb/rpm）

**默认构建矩阵：**

| 架构 | deb | rpm | linyaps | appimage | portable |
|------|-----|-----|---------|----------|----------|
| amd64 | 静态链接+源码包 | 静态链接 | ✓ | ✓ | ✓ |

**示例：**
```bash
# 交互模式（不带参数运行）
./scripts/build/launch-auto-build.sh

# 指定版本构建全部包
./scripts/build/launch-auto-build.sh -v 2.1.0

# 指定架构和包类型
./scripts/build/launch-auto-build.sh -v 2.1.0 -a amd64,arm64 -t deb,rpm

# 跳过 Docker 镜像预构建（镜像已存在时）
./scripts/build/launch-auto-build.sh -v 2.1.0 --skip-images
```

**工作流程：**
1. 生成构建矩阵（包含所有需构建的架构/包类型组合）
2. 收集去重后的 Docker 镜像列表，逐一检查/构建（镜像已存在则跳过）
3. 依次执行所有包构建（失败继续，除非 `--fail-fast`）
4. 输出汇总报告（成功/失败/跳过数量及产物目录）

### 1. build-appimage.sh - 构建 AppImage 包

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
- `--static` - 静态链接 FFmpeg，消除 soname 依赖，单包兼容多个 Debian 版本

**示例：**
```bash
./scripts/build/build-deb.sh
./scripts/build/build-deb.sh -v 1.2.3 -a arm64
./scripts/build/build-deb.sh --with-source
./scripts/build/build-deb.sh --container              # 推荐：在容器中构建
./scripts/build/build-deb.sh --container --debian-version 10  # Debian 10 容器
./scripts/build/build-deb.sh --static                 # 静态链接 FFmpeg，跨版本兼容
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

## 非 x86 架构构建

非 x86 架构（如 arm64、loong64、sw64、mips64el 等）的软件包由 OBS 构建服务器统一构建和维护。

**OBS 仓库链接：** [OBS 构建服务器](https://obs22.odata.cc/package/show/home:Admin:app/ter-music)

如需为其他架构构建软件包，请直接使用 OBS 服务器，无需在本地配置交叉编译环境。

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
2. 容器构建需要 Docker 环境
3. 某些包格式可能需要特定的构建依赖
4. 建议使用 `-k, --keep-temp` 选项进行调试

## 依赖检查

各脚本会自动检查所需的构建依赖，如果缺少依赖会给出安装提示。

**常见依赖：**
- cmake, make, gcc
- pkg-config
- 开发库：libavcodec-dev, libavformat-dev, libswresample-dev, libswscale-dev, libavutil-dev, libavfilter-dev, libpulse-dev, libncurses-dev, libxml2-dev, libcurl4-openssl-dev
- 打包工具：dpkg-dev, rpm-build, linglong-builder 等
