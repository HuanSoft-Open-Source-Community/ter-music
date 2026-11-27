# Ter-Music 构建与打包

本文只描述当前仓库的打包系统。所有打包脚本的版本号都由 `-v/--version` 指定，不读取 git tag，也不自动探测 `types.h`；三段版本会自动补 `.0` 为四段。

## 0. 一键构建

```bash
./scripts/build/launch-auto-build.sh -v 26.4.1.0
```

默认构建：

| 类型 | 构建方式 | 产物 |
| --- | --- | --- |
| DEB | Debian 10 容器，仅 FFmpeg 静态 | `build/deb/amd64/*.deb` |
| RPM | EL7/8/9/10 容器，各一个，仅 FFmpeg 静态 | `build/rpm/el<N>/x86_64/*.rpm` |
| AppImage | Debian 10 容器，glibc 2.28 基线 | `build/appimage/x86_64/*.AppImage` |
| Linyaps | 宿主 `ll-builder` 原生构建 | `build/linyaps/x86_64/*.uab` 与 `*.layer` |
| AUR | Arch 容器内 `makepkg` + 安装验收 | `build/aur/x86_64/*.pkg.tar.zst` |

> 26.4.1.0 发布**不包含** Linyaps UAB/layer（等上游 ll-box 修复释出后再发，见
> [RELEASE.md](RELEASE.md) 第六节）；发布构建请显式传 `-t deb,rpm,appimage,aur`。

常用选项：

```bash
# 只构建部分类型
./scripts/build/launch-auto-build.sh -v 26.4.1.0 -t deb,rpm

# 只构建部分 EL
./scripts/build/launch-auto-build.sh -v 26.4.1.0 -t rpm --rpm-el 8,9

# 追加 DEB/RPM 源码包与调试包
./scripts/build/launch-auto-build.sh -v 26.4.1.0 --with-source --with-debuginfo

# 强制重建 Docker 镜像
./scripts/build/launch-auto-build.sh -v 26.4.1.0 --rebuild-images
```

默认不产出任何源码包或调试包；只有显式传入 `--with-source` / `--with-debuginfo` 才会追加，且只对 DEB/RPM 生效。

## 1. 版本与构建约定

- `-v` 必填；接受 `26.4.1.0` 或 `26.4.1`（自动补成 `26.4.1.0`），也接受 `v26.4.1.0` 并去掉前缀。
- 二进制内的 `APP_VERSION` 由 CMake 的 `-DTER_MUSIC_VERSION=<V>` 注入，因此 `ter-music version` 与包版本一致。
- 构建使用工作树（包含未提交修改），不依赖 `git archive` 或 tag。
- 所有脚本只支持 `amd64`/`x86_64`。

### 安装布局（/opt/apps）

五种包（deb/rpm/aur/appimage/linyaps）统一把实体文件装到 `/opt/apps/cn.org.huansoft.ter-music/files/{bin,share,...}`，`/usr` 下只放 6 条符号链接：

| 包内 `/usr` 路径 | 指向（相对） |
| --- | --- |
| `usr/bin/ter-music` | `../../opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music` |
| `usr/share/applications/ter-music.desktop` | `../../../opt/apps/cn.org.huansoft.ter-music/files/share/applications/ter-music.desktop` |
| `usr/share/icons/hicolor/{32x32,48x48,128x128}/apps/ter-music.png` | `../../../../../../opt/apps/cn.org.huansoft.ter-music/files/share/icons/hicolor/<size>/apps/ter-music.png` |
| `usr/share/icons/hicolor/scalable/apps/ter-music.svg` | `../../../../../../opt/apps/cn.org.huansoft.ter-music/files/share/icons/hicolor/scalable/apps/ter-music.svg` |

- CMake 在**未显式**传 `-DCMAKE_INSTALL_PREFIX` 时默认使用该前缀（Linyaps 规范）；显式传入（如 `/usr`）一律尊重。
- deb：`debian/links` 交给 `dh_link`（绝对源 → 相对链接，卸载只删链接）；rpm：`%install` 用 `ln -sfn`，`%files` 同时拥有实体与 `/usr` 链接；AUR：`package()` 用 `cmake --install` + `ln -sfn`；AppImage：AppDir 内同布局，`AppDir/usr` 全为相对链接；linyaps：原 `${PREFIX}` 即 files/ 布局。
- `scripts/test/package-layout-check.sh` 与 `scripts/test/check-package-identity.sh` 校验五条链的前缀、链接目标、模型常量与“`/usr` 无实体文件”一致性。

### 内置消音模型（构建期下载 / 校验 / 注入）

消音模型是构建期注入的数据文件，不从源码编译：

- 缓存：`.tmp/models/UVR_MDXNET_3_9662.onnx`（`TER_MUSIC_MODEL_CACHE_DIR` / `TER_MUSIC_MODEL_URL` 可覆盖，供离线镜像或测试）；命中且大小 `29704436` B、SHA-256 `e02220e8…aadc` 才复用。
- 下载：`aria2c` 优先（构建镜像内已装），失败或缺失回退 `curl`/`wget`；写 `${cache}.part.$$`，校验通过后原子改名；坏缓存先丢弃，下载内容错误（大小或 SHA-256 不符）拒绝且不留 `.part`；网络失败给出可读错误并返回非 0；`flock` 串行化并发构建。
- 注入：`cmake -DTER_MUSIC_BUNDLED_MODEL=<onnx>`；CMake 再次校验存在性、大小与整文件 SHA-256，不符直接 `FATAL_ERROR`，通过后安装为 `<prefix>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`（重命名为小写），并安装 `resources/models/NOTICE.md`。
- 五条链：`scripts/build/build-{deb,rpm,aur,appimage,linyaps}.sh` 均先 `tm_ensure_model`；`debian/rules`（env 优先，源码树 `.tm-bundled-model/` 回退，缺模型 `$(error)`）、rpm spec、`PKGBUILD`（`source` + `sha256sums`）、`build-appimage.sh`、`linglong/linglong.yaml.in` 各自传参。
- 未注入时 CMake 只 WARNING、不装占位模型：源码构建的 `make install` 产物不含模型，运行期提示“内置模型缺失”并播原声。
- `*.onnx` 与 `/.tmp/models/` 均在 `.gitignore`；`aria2` 只在构建镜像里，不新增运行时依赖。
- deb/rpm 的源码包会包含 staging 里的 `.tm-bundled-model/`（体积约 +28 MB），无外部缓存也能重建。

## 2. DEB（Debian 10 容器）

```bash
./scripts/build/build-deb.sh -v 26.4.1.0
```

选项：

| 选项 | 说明 |
| --- | --- |
| `-v, --version VERSION` | 必填 |
| `-a, --arch ARCH` | 仅 `amd64` |
| `-k, --keep-temp` | 保留 `.tmp/deb` |
| `--with-source` | 额外产出 `.dsc`、`.orig.tar.gz`、`.debian.tar.xz` |
| `--with-debuginfo` | 额外产出 `-dbgsym` deb |
| `--rebuild-image` | 强制重建 Debian 10 镜像 |
| `--no-cache` | `docker build` 不使用缓存 |

产物：

- 二进制：`build/deb/amd64/cn.org.huansoft.ter-music_<V>-1_amd64.deb`
- 源码：`build/deb/source/`（仅 `--with-source`）
- 调试：`build/deb/amd64/*-dbgsym_*.deb`（仅 `--with-debuginfo`）

安装布局：实体在 `/opt/apps/cn.org.huansoft.ter-music/files`，`/usr` 仅 `debian/links` 的 6 条相对链接；模型由 `debian/rules` 经 `TER_MUSIC_BUNDLED_MODEL`（源码树 `.tm-bundled-model/` 回退）注入并装入 `share/ter-music/models/`，缺模型直接 `$(error)` 拒绝出坏包。`build-deb.sh` 在 `dpkg-buildpackage` 后额外用 `dpkg-deb -c` 与解包断言校验 opt 实体、`/usr` 全链接、模型大小/SHA-256、NOTICE/lang/help/layouts。

### 跨发行版兼容

- 在 Debian 10（glibc 2.28）中构建，覆盖 Debian 10/11/12/13 与 Ubuntu 20.04/22.04/24.04/26.04。
- 仅 FFmpeg 静态；ncurses、libcurl、libpng、libjpeg、libdbus、sqlite、zlib 动态链接。
- **libxml2 不链接，也不静态**：`util/xml_compat.c` 在运行期依次尝试 `libxml2.so.2`、`libxml2.so.16`，因此同一份二进制可同时用于旧版和新版发行版。
- 因为不再有 `libxml2` 的 `DT_NEEDED`，DEB 显式依赖 `libxml2 | libxml2-16`：
  - Debian 10–13、Ubuntu 20.04–24.04 使用 `libxml2`；
  - Ubuntu 26.04 使用 `libxml2-16`。
- `libjpeg`、`libpng`、`libcurl` 的包名差异仍由 `debian/rules` 的 `A | B` 备选处理（同一个 soname、不同包名）。

验证：

```bash
dpkg-deb -f build/deb/amd64/*.deb Depends | grep 'libxml2 | libxml2-16'
readelf -d <解包后的 usr/bin/ter-music> | grep NEEDED   # 不应出现 libxml2/libav*
TER_MUSIC_XML_DEBUG=1 ter-music help 2>&1 | grep libxml2
```

最后一条在 Ubuntu 26.04 上应显示 `libxml2.so.16`，在 Debian 10 上应显示 `libxml2.so.2`。

## 3. RPM（EL7/8/9/10 容器）

```bash
./scripts/build/build-rpm.sh -v 26.4.1.0
```

选项：

| 选项 | 说明 |
| --- | --- |
| `-v, --version VERSION` | 必填 |
| `-a, --arch ARCH` | 仅 `x86_64` |
| `--el LIST` | `7,8,9,10` 或 `all`，默认 `all` |
| `-k, --keep-temp` | 保留 `.tmp/rpm` |
| `--with-source` | 额外产出 `.src.rpm` |
| `--with-debuginfo` | 额外产出 `-debuginfo` / `-debugsource` rpm |
| `--rebuild-image` | 强制重建对应 EL 镜像 |
| `--no-cache` | `docker build` 不使用缓存 |

为什么每个 EL 一个包：

- EL7 的 ncursesw 是 `libncursesw.so.5`、libpng 是 `libpng15.so.15`；
- EL8/9/10 是 `libncursesw.so.6`、`libpng16.so.16`。
- 在“仅 FFmpeg 静态”的前提下，一个 RPM 无法可靠覆盖四代 EL，因此在对应 EL 容器里分别构建。

产物：

- 二进制：`build/rpm/el<N>/x86_64/cn.org.huansoft.ter-music-<V>-1.el<N>.x86_64.rpm`
- 源码：`build/rpm/el<N>/source/*.src.rpm`（仅 `--with-source`）
- 调试：`build/rpm/el<N>/x86_64/*-debuginfo-*.rpm`、`*-debugsource-*.rpm`（仅 `--with-debuginfo`）

安装布局：同 §1，`%install` 建 6 条相对链接，`%files` 同时收录 `/opt/apps/.../files` 实体与 `/usr` 链接；模型随源 tarball 进 `rpmbuild`，`%build` 传 `-DTER_MUSIC_BUNDLED_MODEL`（缺文件失败）。`build-rpm.sh` 产出后用 `rpm -qpl` / `rpm -qp --queryformat` 断言 opt 实体、6 个 `/usr` 路径、模型大小与 `/usr/bin/ter-music` 必须是符号链接。

spec 位于 `rpm/cn.org.huansoft.ter-music.spec`。libxml2 同样由运行期 `dlopen` 加载，因此 spec 显式声明 `Requires: libxml2`。

## 4. AppImage（Debian 10 容器）

```bash
./scripts/build/build-appimage.sh -v 26.4.1.0
```

- 在 Debian 10（glibc 2.28）中构建，产物为 `build/appimage/x86_64/ter-music-<V>-x86_64.AppImage`。
- 内部布局与安装包一致：`AppDir/opt/apps/cn.org.huansoft.ter-music/files/...` 为实体，`AppDir/usr/{bin,share/applications,share/icons}` 全部是相对链接（`AppDir/usr` 无实体文件）；`AppRun` 从 `${HERE}/opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music` 启动。
- 仅 FFmpeg 静态；其余动态库（包括 `libxml2.so.2` 及其依赖）复制进 `files/lib`，二进制以 `$ORIGIN/../lib` 的 RPATH 查找，`AppRun` 同时设置 `LD_LIBRARY_PATH`，因此运行期 `dlopen("libxml2.so.2")` 优先命中包内副本。
- `AppRun` 另设 `XDG_DATA_HOME="${FILES}/share"`，使内置消音模型经只读回退命中包内 `files/share/ter-music/models/`；同时把用户原来的 `XDG_DATA_HOME`（或 `$HOME/.local/share`）追加进 `XDG_DATA_DIRS`，保持既有的数据目录查找行为。AppImage 场景下 `$XDG_DATA_HOME/ter-music` 里的旧用户模型不再被读取（包内模型优先）。
- 镜像内工具链固定为：`/opt/appimagetool/squashfs-root/usr/bin/appimagetool` 与 `/opt/appimagetool/runtime-x86_64`；两者都由 `docker-build.sh` 从宿主缓存 `COPY` 进镜像，容器内不再联网下载。
- 不再支持从 RPM 转换；portable 包已移除。

运行：

```bash
chmod +x build/appimage/x86_64/ter-music-*.AppImage
./build/appimage/x86_64/ter-music-*.AppImage --version
```

## 5. Linyaps（宿主原生，UAB + layer）

> 本次 26.4.1.0 发布缓发 UAB/layer；本节构建链原样保留，供上游修复释出后补发。

```bash
./scripts/build/build-linyaps.sh -v 26.4.1.0
```

前置条件：

- 宿主已安装 `ll-builder`、`ll-box`、`ll-cli`，且 `ll-builder --version` 与 `ll-cli --version` 匹配；
- `cmake`、`make`、`python3`；
- 首次构建会拉取 base/runtime，缓存在 `~/.cache/linglong-builder`。

选项：

| 选项 | 说明 |
| --- | --- |
| `-v, --version VERSION` | 必填 |
| `-a, --arch ARCH` | 仅 `x86_64` |
| `-k, --keep-temp` | 保留 `.tmp/linyaps-temp` |
| `-o, --offline` | 强制离线 |
| `-r, --refresh` | 强制刷新 base/runtime |

产物（**两者都必须成功**）：

- `build/linyaps/x86_64/cn.org.huansoft.ter-music_<V>_x86_64.uab`
- `build/linyaps/x86_64/*_binary.layer`

`linglong/linglong.yaml.in` 是模板；脚本会复制到 `.tmp` 工作目录、替换 `@VERSION@`/`@ARCH@` 后再调用 `ll-builder`。UAB 导出后会校验 `.note.uab.sig`。模型经 staging 源码树的 `bundled-model/` 注入（`ll-builder` 只拷贝源码目录，宿主绝对路径在构建容器内不可见）；`linglong.yaml.in` 在 `make install` 后自检 `share/ter-music/models/` 的模型、NOTICE、大小与 SHA-256，`build-linyaps.sh` 断言日志出现该自检通过标记。

安装/验证：

```bash
ll-cli install build/linyaps/x86_64/*.uab
ll-cli install build/linyaps/x86_64/*.layer
ll-cli run cn.org.huansoft.ter-music -- ter-music version
```

> layer 格式在上游文档中标注为已弃用，但按本项目要求仍作为正式产物保留。

## 6. AUR（Arch 容器验收，不投递）

```bash
./scripts/build/build-aur.sh -v 26.4.1.0
```

- 在 Arch 容器内以 `builder` 用户执行 `makepkg`，再用 root 执行 `pacman -U` 安装验收。
- `PKGBUILD` 的 `source` 含模型官方 URL 与 `sha256sums`（正式 `makepkg` 会下载并校验），`build()` 传 `-DTER_MUSIC_BUNDLED_MODEL="$srcdir/$_model"`；`package()` 装 opt 实体 + 6 条相对链接。
- 本地验收使用 `--noextract` 直接使用暂存源码，模型复用 `.tmp/models` 缓存，不依赖网络或 git tag。
- `PKGBUILD` 的源码是固定 commit（`_commit=...`），不是 tag。
- 构建后会检查仓库里的 `.SRCINFO` 是否与 `makepkg --printsrcinfo` 一致；不一致即失败。
- 本轮**不投递 AUR、不创建 git tag**。

产物：`build/aur/x86_64/cn.org.huansoft.ter-music-<V>-1-x86_64.pkg.tar.zst`。

## 7. Docker 镜像与镜像源

| 基础镜像 | 用途 | 源 |
| --- | --- | --- |
| `debian:10` | DEB + AppImage | 阿里 `debian-archive`（归档） |
| `centos:7` | RPM EL7 | 阿里 `centos-vault/7.9.2009` + 阿里 EPEL 7 + 阿里 SCLo（归档） |
| `rockylinux:8/9/10` | RPM EL8/9/10 | USTC `rocky` |
| `archlinux:latest` | AUR | USTC `archlinux` |
| `ubuntu:26.04` | 仅验收 | USTC `ubuntu` |

宿主缓存统一放在 `.cache/`，多个镜像并行构建时同一份资源只下载一次（`flock` 串行下载，校验通过后原子替换）：

| 缓存 | 用途 | 来源 |
| --- | --- | --- |
| `.cache/sources/ffmpeg-8.1.2.tar.gz` | 所有镜像共用 | `ffmpeg.org` |
| `.cache/tools/appimagetool-x86_64.AppImage` | AppImage 打包工具 | GitHub `AppImage/appimagetool` latest |
| `.cache/sources/squashfs-tools-4.7.5.tar.gz` | 镜像内编译 `mksquashfs`（Debian 10 自带的 4.3 不支持 `-mkfs-time`） | GitHub `plougher/squashfs-tools` |
| `.cache/tools/runtime-x86_64` | AppImage type2 runtime | GitHub `AppImage/type2-runtime` latest |

镜像构建只 `COPY` 上述已校验文件，容器内不再联网下载；只有 `--rebuild-image` 才会重建镜像层。AppImage 固定用 `--comp zstd`：新版 type2 runtime 只内建 zlib/zstd 解压支持，用 xz 打包会解包失败。

三个 Docker 镜像（`Dockerfile.debian10`/`.rpm`/`.aur`）都安装 `aria2` 作为构建期模型下载器；EL8+ 的 aria2 位于 EPEL，先装 `epel-release` 再装 aria2，失败仅告警并由 `tm_ensure_model` 回退 `curl`（镜像里已显式安装 curl）。`aria2` 不是运行时依赖。

## 8. 构建依赖

Docker 类构建只要求宿主机有可用的 Docker：

- DEB：Debian 10 / glibc 2.28，FFmpeg 8.1.2 静态；
- RPM：EL7 使用 cmake3 + devtoolset-11，EL8/9/10 使用发行版 cmake；
- AppImage：复用 Debian 10 镜像，镜像内已带 appimagetool、squashfs-tools 4.7.5 与 type2 runtime；
- AUR：Arch 镜像内带 base-devel、ffmpeg、ncurses、libxml2 等。

Linyaps 不使用 Docker，要求宿主玲珑工具链。

构建期模型下载另需 `aria2` 或 `curl`/`wget`（宿主直跑打包脚本时）；Docker 类构建已在镜像内备好，缺失时 `tm_ensure_model` 自动回退。

构建依赖里有两点与远程音乐源直接相关：

- **libcurl 是硬依赖**：`CMakeLists.txt` 用 `pkg_check_modules(LIBCURL libcurl)` 检测，缺失时以 `FATAL_ERROR` 终止，不能靠关闭远程功能绕过；Debian/RPM 的 `Build-Depends`/`BuildRequires` 已声明 `libcurl4-openssl-dev`/`libcurl-devel`，Arch 的 `depends` 已含 `curl`。
- **SMB 依赖运行期的外部程序 `smbclient`**（Samba 客户端），libcurl 的 SMB 实现未被使用。打包元数据只作可选/推荐声明：DEB `Recommends: smbclient`、Arch `optdepends`、RPM（非 RHEL7）`Recommends`；Linyaps 清单没有“可选依赖”字段，因此由应用在缺少 `smbclient` 时给出“安装 Samba 客户端”的可操作提示。缺 `smbclient` 不影响构建，只影响 SMB 连接。

## 9. 测试

### 9.1 单元测试

```bash
./scripts/test/run-unit-tests.sh                 # 全部
./scripts/test/run-unit-tests.sh test_remote_url test_remote_listing test_remote_cache
TM_UNIT_STRICT=1 ./scripts/test/run-unit-tests.sh
```

- 远程功能有三组单测：`test_remote_url`（URL 解析/构造/编解码/脱敏）、`test_remote_listing`（FTP MLSD/LIST、WebDAV 命名空间、HTML autoindex、SMB 列格式、路径归一）、`test_remote_cache`（`.part`→rename、缓存命中、LRC、取消/重试、淘汰保护集、线程竞态）。
- 外观功能有两组单测：`test_appearance`（OSC 11 应答解析、COLORFGBG 回退、WCAG 亮度分类与模式→变体、出厂/亮色表与“显式自定义不改写”、7 个内置预设的亮/暗互异性与 8/16/256 色量化、`data/layouts/layouts.json` 暗色值防漂移）与 `test_appearance_terminal`（按终端覆盖文件的 round-trip、非法条目与损坏 JSON、只读版本、原子写）。
- 某些单测需要本机未安装的开发头/库（libavcodec、ncursesw、libxml2）；这类**环境**编译失败输出 `SKIP`，不算测试失败。默认退出码 `0` = 全过或只有环境 SKIP，`1` = 有真实失败；CI 完整环境请设 `TM_UNIT_STRICT=1`，此时 SKIP 也按失败处理。

### 9.2 远程端到端测试

```bash
./scripts/test/remote-frontend-e2e.sh --bin build/ter-music
./scripts/test/remote-frontend-e2e.sh --bin build/ter-music --keep   # 保留工作目录与日志
```

前置工具：`python3`（含 `pyftpdlib`，FTP 夹具需要；可 `pip install pyftpdlib`）、`gdbus`、`dbus-run-session`、`openssl`、`curl`。音频夹具由 python `wave` 生成，**不需要 ffmpeg**。

脚本行为：

- 在私有 `dbus-run-session` 内先以 `daemon foreground` 起核心，再用 pty 驱动 TUI；这样与 Linyaps 容器一致（容器内 TUI 不能自行 fork 脱离的核心），也不会误连宿主上正在播放的实例。
- 本地回环夹具覆盖 HTTP、WebDAV、FTP、TLS/自签名 CA、401 认证、慢速下载、空目录与超过 200 首的截断目录；SMB 用遵循 Samba `process_command_string()` 语义的假 `smbclient`（断言命令注入不执行、密码不进 argv），SFTP 用黑洞监听器验证 known_hosts 缺失时 fail-closed。
- 断言核心队列里的路径全部是缓存中的本地路径、缓存无 `.part`、无残留进程；失败时保留日志并给出可复现命令。
- 退出码：`0` 全部断言通过，`1` 有断言失败，`2` 环境不满足（缺工具/构建产物），不会把环境问题伪装成产品失败。
- 真实 Samba/SFTP 服务端不在默认夹具内：SMB 只验证命令构造与安全语义（脚本注释保留了接入真实 `smbd` 的说明，当前默认运行的仍是假客户端）；SFTP 只验证 fail-closed 保护；FTPS 验证 TLS 握手，数据通道未覆盖。

### 9.3 外观端到端测试

```bash
# 外观模式、OSC 11 应答与回退链、焦点重探、输入安全、deepin-terminal 适配器
TERM=xterm-256color script -qefc \
  "python3 scripts/test/appearance-pty-e2e.py --bin build/ter-music" /dev/null

# XDG desktop portal 的 color-scheme 与 SettingChanged 跟随
dbus-run-session -- script -qefc \
  "python3 scripts/test/appearance-xdg-pty-e2e.py --bin build/ter-music" /dev/null
```

- 两个脚本都用 pty 驱动 TUI；`appearance-pty-e2e.py` 在 stdin 不是终端时会自行 `SKIP`，所以在无终端的 CI/脚本环境中要用 `script`（或真实终端）包一层。`--only 场景名` 只跑一个场景。
- 每个场景使用独立的临时 `HOME`/`XDG_CONFIG_HOME`/`XDG_CACHE_HOME`，不触碰真实用户配置；非 XDG 场景以 `--frontend=local` 启动，不依赖 D-Bus。
- `appearance-xdg-pty-e2e.py` 需在 `dbus-run-session` 内运行；脚本自己启动私有 dbus-daemon 与 `fake-xdg-portal.py` 提供 `org.freedesktop.portal.Settings`，不需要系统 portal。如果宿主当前的 D-Bus 会话里有真实 XDG portal 干扰回退场景，可在运行非 XDG 用例前 `env -u DBUS_SESSION_BUS_ADDRESS`。
- 前置工具：`python3`（仅标准库）、`dbus-daemon`/`dbus-run-session`、util-linux 的 `script`。

### 9.4 打包布局与模型检查

```bash
./scripts/test/package-layout-check.sh
TM_LAYOUT_STATIC_ONLY=1 ./scripts/test/package-layout-check.sh   # 无构建工具时仅静态
./scripts/test/check-package-identity.sh
```

- `package-layout-check.sh`：静态断言五条链的 `/opt/apps` 前缀、`/usr` 链接、模型常量与 `-DTER_MUSIC_BUNDLED_MODEL` 接线；本机有 CMake 时再从零 configure/build、`DESTDIR` 安装、按 `debian/links` 建链、`dpkg-deb` 组装/解包断言，并验证 `tm_ensure_model` 的缓存/下载/坏文件分支。
- `check-package-identity.sh`：应用 ID 与版本跨包一致，以及五条链前缀、链接目标、`common.sh`/`CMakeLists.txt`/NOTICE 模型常量一致、`*.onnx` 未进 git、aria2 不是运行时依赖。
- 需要 docker / `dpkg-buildpackage`(dh) / `rpmbuild` / `makepkg` / `ll-builder` / `mksquashfs` 的**真实包构建**不在该脚本内；`TM_LAYOUT_STATIC_ONLY=1` 会明确打印“未执行本地构建/DESTDIR 安装，不代表构建通过”。

## 9.5 CI 门禁（GitHub Actions）

`.github/workflows/ci.yml` 是发布门禁的唯一来源。作业分工：

| 作业 | 覆盖内容 |
| --- | --- |
| `build (ubuntu/fedora/arch)` | 三发行版编译 + `--help`/CLI 冒烟；Ubuntu 额外跑 daemon/D-Bus 与 PTY 启动冒烟 |
| `strict-warnings` | Ubuntu 上 `-Wall -Wextra -Werror` 全量编译 |
| `build (no D-Bus)` | 屏蔽 `dbus-1.pc` 后从零 configure/build，保证 `!HAVE_DBUS` 分支可编译 |
| `shellcheck` | `scripts/build/*.sh`、`scripts/docker/*.sh`、`scripts/test/*.sh` |
| `gates` | 纯度/配置归属/包身份/i18n 门禁、`TM_UNIT_STRICT=1` 单测、打包布局静态断言 |
| `e2e` | 既有端到端 + 外观/选择器/键位/卡拉OK 主推功能端到端 |
| `required-checks` | 汇总以上作业结果（分支保护只认它） |

### 无 D-Bus 构建

D-Bus 在 CMake 里由 `pkg_check_modules(DBUS dbus-1)` 探测，找不到时 `HAVE_DBUS` 不定义，portal/MPRIS 走 stub 分支。CI 的做法：

1. 把系统 pkg-config 目录（`/usr/lib/x86_64-linux-gnu/pkgconfig`、`/usr/share/pkgconfig`）里除 `dbus-1.pc` 外的 `.pc` 全部软链到 `/tmp/pc-nodbus`；
2. 用 `PKG_CONFIG_LIBDIR=/tmp/pc-nodbus`（并清空 `PKG_CONFIG_PATH`）配置构建，让 `pkg_check_modules` 找不到 `dbus-1`；
3. 用 `compile_commands.json` 断言没有 `-DHAVE_DBUS`，再完整构建 + `--help` 冒烟。

本机复现：

```bash
mkdir -p /tmp/pc-nodbus
for d in /usr/lib/x86_64-linux-gnu/pkgconfig /usr/share/pkgconfig; do
    for f in "$d"/*.pc; do
        [ "$(basename "$f")" = dbus-1.pc ] && continue
        ln -sf "$f" /tmp/pc-nodbus/
    done
done
env -u PKG_CONFIG_PATH PKG_CONFIG_LIBDIR=/tmp/pc-nodbus     cmake -B build-no-dbus -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-no-dbus -j2
```

### CI 里的卡拉OK素材与运行库

- **模型**：`UVR_MDXNET_3_9662.onnx` 不进 git；CI 从项目 README 记录的 URL 下载，并强制校验大小 `29,704,436 B` 与 SHA-256 `e02220e8…aadc`，校验失败即红。
- **推理运行库**：官方 GitHub release 资产已 404，CI 改用 Debian trixie 的 `libonnxruntime1.21_1.21.0+dfsg-1_amd64.deb`（`soname` 为 `libonnxruntime.so.1.21`，应用的 `dlopen` 依次尝试 `.so.1.21`/`.so.1`/`.so`），并解包它动态依赖的 XNNPACK/ONNX/protobuf/RE2/absl 等到 `/usr/local/lib` 后 `ldconfig`。这些库只要求 `glibc <= 2.38`，Ubuntu 24.04 runner 满足；`ldd` 出现 `not found` 即红。
- **曲目素材**：托管 runner 没有音乐文件，CI 用 `ffmpeg` 合成 150 秒立体声夹具（80 Hz 低音 + 440 Hz 中置占位 + 左右不同侧向音）并附 `.lrc`，作为 `karaoke-model-e2e.sh`/`karaoke-segments-e2e.sh` 的 `--track`。
- **虚拟声卡**：托管 runner 没有 `/dev/snd`，CI 启动 PulseAudio null sink 作为输出设备（`XDG_RUNTIME_DIR` 经 `GITHUB_ENV` 传给后续步骤）；否则核心的 `audio_backend=0`（AUTO）选不到后端，播放位置不会推进。
- **单元测试**：`gates` 作业设 `TM_UNIT_STRICT=1`，环境缺依赖导致的 `SKIP` 按失败处理；开发包缺一不可。
- **打包布局**：`gates` 用 `TM_LAYOUT_STATIC_ONLY=1` 跑 `package-layout-check.sh`（静态只读，不构建、不装 DESTDIR）。

`appearance-pty-e2e.py` 需要 stdin 是终端，CI 用 `script -qefc …` 包一层；`appearance-xdg-pty-e2e.py` 再套 `dbus-run-session`（脚本自带私有 `fake-xdg-portal.py`，不依赖系统 portal）。

### 未接入 CI 的测试与原因

- `audit-build-artifacts.sh`：审计 build/ 下**已构建的** DEB/RPM/AppImage/Linyaps/AUR 产物。CI 不构建软件包，五个目标只会全部 `[SKIP]`，接入不会产生门禁信号；它在发布机重建成产物后执行。
- `alsa-backend-e2e.sh`：托管 runner 没有 `/dev/snd`，脚本按设计整体 `SKIP` 并退出 0。CI 保留该步骤以验证“无设备 → 可读 SKIP、不伪装通过”的契约；真实 ALSA 断言（暂停不写设备、位置不推进等）需在带声卡的发布机上跑。
- `pipewire-*`/`perf-check --report` 等包含环境依赖或只记录实测值的步骤保持原策略：缺依赖时明确 SKIP，共享 runner 的抖动值不作阈值判定。

## 10. 故障排除

- **`libxml2 runtime not found`**：运行期既没有 `libxml2.so.2` 也没有 `libxml2.so.16`。安装对应发行版的 `libxml2`/`libxml2-16`。
- **`libxml2 (...) missing symbol ...`**：运行库版本与项目使用的 API 不匹配；把完整报错保留下来，便于定位 ABI 差异。
- **DEB 源码包失败**：检查 `.orig.tar.gz` 是否为 `<pkg>_<V>.orig.tar.gz`，且不包含 `debian/`。
- **RPM debuginfo 为空**：确认 EL7 的 `debugedit` 可用；build-rpm 会以 `RelWithDebInfo` + `$RPM_OPT_FLAGS` 构建。
- **Linyaps UAB 签名校验失败**：`ll-builder` 版本过旧或缺少 `cn.org.linyaps.builder.utils ≥ 0.0.4.0`；脚本会直接失败，不会再用 layer 降级掩盖。
- **镜像源超时/404**：构建会打印具体 URL；归档镜像（Debian 10、EL7）使用阿里归档源属预期。
- **AppImage 工具下载失败（`curl: (56)` / `Failed to download runtime file`）**：GitHub 不通或宿主缓存缺失。可手工把 `runtime-x86_64` 放进 `.cache/tools/`、把 `squashfs-tools-4.7.5.tar.gz` 放进 `.cache/sources/` 后重试。
- **AppImage 解包失败（`--appimage-extract` 返回 1）**：runtime 不认该压缩算法；构建脚本已固定 `--comp zstd`，不要改回 xz。
- **模型下载失败 / 校验失败**：`tm_ensure_model` 会打印 URL 与期望的大小/SHA-256；网络不通可手工把 `UVR_MDXNET_3_9662.onnx` 放进 `.tmp/models/` 后重试，坏文件会先被丢弃。`-DTER_MUSIC_BUNDLED_MODEL` 指向的文件不符时 CMake 直接 `FATAL_ERROR`，不会产出缺模型或带坏模型的包。
- **EL7 报 `CURLOPT_XFERINFOFUNCTION` 未声明 / ncurses 字段缺失**：源码已按 libcurl < 7.32、ncurses 5.x 做条件编译兼容；报错说明构建用的不是最新源码。
