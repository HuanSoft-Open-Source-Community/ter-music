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

spec 位于 `rpm/cn.org.huansoft.ter-music.spec`。libxml2 同样由运行期 `dlopen` 加载，因此 spec 显式声明 `Requires: libxml2`。

## 4. AppImage（Debian 10 容器）

```bash
./scripts/build/build-appimage.sh -v 26.4.1.0
```

- 在 Debian 10（glibc 2.28）中构建，产物为 `build/appimage/x86_64/ter-music-<V>-x86_64.AppImage`。
- 仅 FFmpeg 静态；其余动态库（包括 `libxml2.so.2` 及其依赖）复制进 `AppDir/usr/lib`。
- `AppRun` 设置 `LD_LIBRARY_PATH`，因此运行期 `dlopen("libxml2.so.2")` 优先命中包内副本。
- 镜像内工具链固定为：`/opt/appimagetool/squashfs-root/usr/bin/appimagetool` 与 `/opt/appimagetool/runtime-x86_64`；两者都由 `docker-build.sh` 从宿主缓存 `COPY` 进镜像，容器内不再联网下载。
- 不再支持从 RPM 转换；portable 包已移除。

运行：

```bash
chmod +x build/appimage/x86_64/ter-music-*.AppImage
./build/appimage/x86_64/ter-music-*.AppImage --version
```

## 5. Linyaps（宿主原生，UAB + layer）

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

`linglong/linglong.yaml.in` 是模板；脚本会复制到 `.tmp` 工作目录、替换 `@VERSION@`/`@ARCH@` 后再调用 `ll-builder`。UAB 导出后会校验 `.note.uab.sig`。

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
- 本地验收使用 `--noextract` 直接使用暂存源码，不依赖网络或 git tag。
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

## 8. 构建依赖

Docker 类构建只要求宿主机有可用的 Docker：

- DEB：Debian 10 / glibc 2.28，FFmpeg 8.1.2 静态；
- RPM：EL7 使用 cmake3 + devtoolset-11，EL8/9/10 使用发行版 cmake；
- AppImage：复用 Debian 10 镜像，镜像内已带 appimagetool、squashfs-tools 4.7.5 与 type2 runtime；
- AUR：Arch 镜像内带 base-devel、ffmpeg、ncurses、libxml2 等。

Linyaps 不使用 Docker，要求宿主玲珑工具链。

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

## 10. 故障排除

- **`libxml2 runtime not found`**：运行期既没有 `libxml2.so.2` 也没有 `libxml2.so.16`。安装对应发行版的 `libxml2`/`libxml2-16`。
- **`libxml2 (...) missing symbol ...`**：运行库版本与项目使用的 API 不匹配；把完整报错保留下来，便于定位 ABI 差异。
- **DEB 源码包失败**：检查 `.orig.tar.gz` 是否为 `<pkg>_<V>.orig.tar.gz`，且不包含 `debian/`。
- **RPM debuginfo 为空**：确认 EL7 的 `debugedit` 可用；build-rpm 会以 `RelWithDebInfo` + `$RPM_OPT_FLAGS` 构建。
- **Linyaps UAB 签名校验失败**：`ll-builder` 版本过旧或缺少 `cn.org.linyaps.builder.utils ≥ 0.0.4.0`；脚本会直接失败，不会再用 layer 降级掩盖。
- **镜像源超时/404**：构建会打印具体 URL；归档镜像（Debian 10、EL7）使用阿里归档源属预期。
- **AppImage 工具下载失败（`curl: (56)` / `Failed to download runtime file`）**：GitHub 不通或宿主缓存缺失。可手工把 `runtime-x86_64` 放进 `.cache/tools/`、把 `squashfs-tools-4.7.5.tar.gz` 放进 `.cache/sources/` 后重试。
- **AppImage 解包失败（`--appimage-extract` 返回 1）**：runtime 不认该压缩算法；构建脚本已固定 `--comp zstd`，不要改回 xz。
- **EL7 报 `CURLOPT_XFERINFOFUNCTION` 未声明 / ncurses 字段缺失**：源码已按 libcurl < 7.32、ncurses 5.x 做条件编译兼容；报错说明构建用的不是最新源码。
