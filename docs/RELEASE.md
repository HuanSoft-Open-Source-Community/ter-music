# Ter-Music 发布流程（本地真实发行版构建 + 手动上传）

> 2026-08 起，Release 不再由 GitHub Actions 自动打包（`.github/workflows/release.yml` 已移除）。
> 打包改为在**真实的 Debian / Fedora / Deepin 机器**上原生构建（不依赖 Docker），再手动上传到 GitHub Release。
> GitHub Actions 仅保留 CI（`.github/workflows/ci.yml`，编译检查）。本地 Docker 构建体系（`scripts/docker/`、Dockerfiles、`launch-auto-build.sh`）未受影响，仍可作替代方案使用。

## 版本准备

1. 更新 `include/org.yxzl.ter-music/types.h` 中的 `APP_VERSION`（例如 `"v2.1.1"`）。
2. Git tag 名必须与 APP_VERSION 一致（`vX.Y.Z` 格式），发布时按此打 tag。
3. 确认 CI 全绿：`gh run list --workflow ci.yml`。

---

## 一、Debian 机器构建 .deb

在真实的 Debian（推荐 Debian 13）机器上执行：

```bash
# 构建工具链与开发库
sudo apt install -y \
    build-essential cmake pkg-config \
    dpkg-dev fakeroot devscripts \
    libavcodec-dev libavfilter-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev libpng-dev libjpeg-dev \
    libpulse-dev libncursesw5-dev libcurl4-openssl-dev libxml2-dev \
    libsqlite3-dev libdbus-1-dev zlib1g-dev

# 构建（原生模式，不使用 Docker）
bash scripts/build/build-deb.sh -v X.Y.Z -a amd64
```

产物：

- `build/deb/amd64/ter-music_X.Y.Z-1_amd64.deb`
- 源码包：`build/deb/source/ter-music_X.Y.Z-1.dsc`、`ter-music_X.Y.Z.orig.tar.gz`、`ter-music_X.Y.Z-1.debian.tar.xz`

---

## 二、Fedora 机器构建 .rpm

在真实的 Fedora（推荐 Fedora 40+）机器上执行：

```bash
# 构建工具链与开发库
sudo dnf install -y \
    gcc make cmake pkgconf-pkg-config rpm-build \
    ffmpeg-free-devel libpng-devel libjpeg-turbo-devel \
    pulseaudio-libs-devel ncurses-devel libcurl-devel libxml2-devel \
    sqlite-devel alsa-lib-devel pipewire-devel dbus-devel zlib-devel

# 构建（原生模式，不使用 Docker）
bash scripts/build/build-rpm.sh -v X.Y.Z -a x86_64
```

产物：

- `build/rpm/x86_64/ter-music-X.Y.Z-1.<dist>.x86_64.rpm`（`<dist>` 如 `.fc41`）
- 对应 `.src.rpm`

---

## 三、Deepin 机器构建 linyaps（linglong）

在真实的 Deepin（推荐 Deepin V25）机器上执行：

```bash
# linglong 构建器与工具链（Deepin 仓库自带）
sudo apt install -y linglong-builder cmake make

# 构建（原生模式；ll-builder 需要 user namespace 支持）
bash scripts/build/build-linyaps.sh -v X.Y.Z -a x86_64
```

产物：

- `build/linyaps/x86_64/org.yxzl.ter-music_X.Y.Z_x86_64.uab`
- `build/linyaps/x86_64/org.yxzl.ter-music_X.Y.Z.0_x86_64_binary.layer`

---

## 四、AppImage / Portable（任一 Linux 机器）

在任意 Linux（如上面的 Debian 机器）上执行：

```bash
# AppImage 需要 squashfs-tools（unsquashfs）
sudo apt install -y squashfs-tools   # Debian/Deepin
# 或
sudo dnf install -y squashfs-tools   # Fedora

# 构建 AppImage（原生）
bash scripts/build/build-appimage.sh -v X.Y.Z -a x86_64

# 构建便携包（原生）
bash scripts/build/build-portable.sh -v X.Y.Z -a x86_64
```

产物：

- `build/appimage/x86_64/ter-music-X.Y.Z-x86_64.AppImage`
- `build/portable/x86_64/ter-music-X.Y.Z-portable-x86_64.tar.gz`

---

## 五、上传 GitHub Release（手动）

收集齐 5 类产物后打 tag 并创建 Release：

```bash
# 打 tag（必须与 APP_VERSION 一致）
git tag vX.Y.Z && git push origin vX.Y.Z

# 创建 Release 并上传产物
gh release create vX.Y.Z \
    build/deb/amd64/*.deb \
    build/deb/source/ter-music_X.Y.Z-1.dsc \
    build/deb/source/ter-music_X.Y.Z.orig.tar.gz \
    build/deb/source/ter-music_X.Y.Z-1.debian.tar.xz \
    build/rpm/x86_64/ter-music-X.Y.Z-1.*.x86_64.rpm \
    build/rpm/x86_64/ter-music-X.Y.Z-1.*.src.rpm \
    build/appimage/x86_64/ter-music-X.Y.Z-x86_64.AppImage \
    build/portable/x86_64/ter-music-X.Y.Z-portable-x86_64.tar.gz \
    build/linyaps/x86_64/org.yxzl.ter-music_X.Y.Z_x86_64.uab \
    build/linyaps/x86_64/org.yxzl.ter-music_X.Y.Z.0_x86_64_binary.layer \
    --generate-notes
```

---

## 替代方案：本地 Docker 静态构建（跨版本兼容单包）

需要"一个包兼容多个发行版版本"（静态链接 FFmpeg，消除 soname 差异）时，使用既有本地 Docker 体系：

```bash
# 一键构建全部 5 种包（deb/rpm/linyaps/appimage/portable）
bash scripts/build/launch-auto-build.sh --arch amd64

# 或单独构建
bash scripts/build/build-deb.sh --static -v X.Y.Z -a amd64     # 静态 FFmpeg，兼容 Debian 10-13
bash scripts/build/build-rpm.sh --container -v X.Y.Z -a x86_64  # Rocky Linux 容器，兼容 EL8/9/10
```

> 原生构建（上文一到四节）与 Docker 静态构建互不冲突，产物命名一致，可按需选择。

---

## 发布检查清单

- [ ] `APP_VERSION`（types.h）与 tag 一致
- [ ] CI（ci.yml）全绿
- [ ] 5 类产物齐备：.deb、.rpm（+src.rpm）、.AppImage、portable .tar.gz、.uab/.layer
- [ ] 在干净 Debian / Fedora 环境各验证一次安装与启动（`--help`）
- [ ] `gh release create` 成功后核对资产列表与 v2.2.0 一致
