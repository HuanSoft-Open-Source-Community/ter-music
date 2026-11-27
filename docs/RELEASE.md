# Ter-Music 发布与验收流程

本流程只使用仓库内的打包系统。版本号由 `-v/--version` 指定（必填），打包脚本不读取 git tag，
也不自动探测 `types.h`。
默认不产出源码包或调试包；需要时显式传入 `--with-source` / `--with-debuginfo`。

## 〇、本次发布决定（26.4.1.0）

| 项 | 决定 |
| --- | --- |
| 版本 | 应用 `26.4.1.0`（`APP_VERSION` = `v26.4.1.0`），包版本 `26.4.1.0-1` |
| tag 名 | `v26.4.1.0`（验收通过后打在最终发布提交上；构建不依赖 tag） |
| 发布产物 | 1 个 DEB、4 个 RPM（EL7–10）、1 个 AppImage、1 个 AUR 包；源码包/调试包可选 |
| UAB / layer | **本次缓发**：等上游 linyaps-box PR #295 的修复释出后再发布（ll-box ≤ 2.3.5 冷启动重放问题） |
| AUR | 本轮只做本地验收，**不投递**；`_commit` 临时钉在 `32737f0`，投递前改为最终发布提交 |
| GitHub Release | 人工上传（GitHub Release workflow 已于 2026-08-17 移除） |
| 合并 | 合入 `testing`（保留 merge 痕迹）；**不合 master** |
| 执行人 | 文档：docs-release；构建：build-release；独立验收：verifier；提交/tag/上传/投递：captain 按人工确认执行 |

发布顺序固定为：**代码与文档收口 → 提交 → 全矩阵重建 → 独立验收 → 打 tag → 上传 GitHub
Release → （决定投递时）AUR**。任何一步失败都停在上一步，不得用旧产物补位。

## 一、准备版本

```bash
./scripts/build/launch-auto-build.sh -v 26.4.1.0 -t deb,rpm,appimage,aur
```

- 版本格式为 `<yy>.<q>.<x>.<y>`（年、季、线、修正）；三段会自动补 `.0`，也接受 `v` 前缀。
- 二进制 `APP_VERSION` 由 CMake 的 `-DTER_MUSIC_VERSION=<V>` 注入，与包版本一致。
- `types.h`、`CMakeLists.txt`、`PKGBUILD pkgver`、`debian/changelog` 顶部、RPM spec 默认
  `_version` 保持同一个开发默认值；打包时以 `-v` 为准，构建脚本会在临时副本里重写
  `debian/changelog` 与 `PKGBUILD` 的版本。
- 本次发布**不构建 UAB**：`launch-auto-build.sh` 的默认类型仍含 `linyaps`，发布构建请显式传
  `-t deb,rpm,appimage,aur`；`build-linyaps.sh` 保留在仓库中供上游修复释出后使用。

## 二、默认产物

| 类型 | 数量 | 路径 |
| --- | --- | --- |
| DEB | 1 | `build/deb/amd64/cn.org.huansoft.ter-music_<V>-1_amd64.deb` |
| RPM | 4 | `build/rpm/el7|8|9|10/x86_64/cn.org.huansoft.ter-music-<V>-1.el<N>.x86_64.rpm` |
| AppImage | 1 | `build/appimage/x86_64/ter-music-<V>-x86_64.AppImage` |
| AUR | 1 | `build/aur/x86_64/cn.org.huansoft.ter-music-<V>-1-x86_64.pkg.tar.zst` |
| ~~Linyaps UAB~~ | 0 | 本次缓发，不在 `build/release/` 清单内 |
| ~~Linyaps layer~~ | 0 | 本次缓发，不在 `build/release/` 清单内 |

所有产物同时复制到 `build/release/`。

- DEB 在 Debian 10（glibc 2.28）中构建，覆盖 Debian 10/11/12/13 与 Ubuntu
  20.04/22.04/24.04/26.04。
- 仅 FFmpeg 静态；libxml2 由 `util/xml_compat.c` 在运行期 `dlopen` `.so.2`/`.so.16`。
- RPM 每个 EL 一个：EL7 的 ncursesw/libpng soname 与 EL8+ 不同，单包无法在仅 FFmpeg 静态下
  可靠覆盖。
- AppImage 自带 `libxml2.so.2` 及其依赖。
- AUR 在 Arch 容器内 `makepkg` + `pacman -U` 安装验收，本轮不投递。
- 所有包统一安装到 `/opt/apps/cn.org.huansoft.ter-music/files`，`/usr` 下只有符号链接
  （`/usr/bin/ter-music`、desktop 与 4 个图标）；AppImage 内部同布局，`AppDir/usr` 全为相对
  链接。
- 所有包都内置消音模型 `share/ter-music/models/uvr-mdxnet-3-9662.onnx`（29,704,436 B，
  SHA-256 `e02220e8…aadc`）与 `models/NOTICE.md`；构建期由 `tm_ensure_model` 下载/校验后注入，
  缺模型或坏模型直接失败。源码包同样自包含（体积约 +28 MB）。
- **ONNX Runtime 交付**：发布包（DEB/RPM/AppImage）静态链接 ONNX Runtime，安装后不依赖系统
  `libonnxruntime`；源码构建保留运行期 `dlopen` 系统库的兼容路径。最终以静态化任务的实测结果
  为准，验收时用 `readelf -d` 与"无系统运行库容器里真实分离"两条断言确认。

## 三、可选源码包 / 调试包

```bash
# 只追加源码包
./scripts/build/launch-auto-build.sh -v 26.4.1.0 --with-source

# 只追加调试包
./scripts/build/launch-auto-build.sh -v 26.4.1.0 --with-debuginfo

# 两者都追加
./scripts/build/launch-auto-build.sh -v 26.4.1.0 --with-source --with-debuginfo
```

| 选项 | DEB | RPM |
| --- | --- | --- |
| `--with-source` | `build/deb/source/*.dsc`、`*.orig.tar.gz`、`*.debian.tar.xz` | `build/rpm/el<N>/source/*.src.rpm` |
| `--with-debuginfo` | `build/deb/amd64/*-dbgsym_*.deb` | `build/rpm/el<N>/x86_64/*-debuginfo-*.rpm`、`*-debugsource-*.rpm` |

说明：

- DEB 源码包通过 `dpkg-buildpackage -S` 生成，`.orig.tar.gz` 会排除 `debian/`。
- DEB 调试包由 debhelper 的 `dh_strip` 自动生成；默认用 `noautodbgsym` 抑制，请求时移除。
- RPM 源码包通过 `rpmbuild -ba` 生成。
- RPM 默认传 `debug_package %{nil}`；请求调试包时不传，并使用 `RelWithDebInfo` + `$RPM_OPT_FLAGS`。
- 这些选项只对 deb/rpm 生效；其他类型会忽略并给出警告。

## 四、验收矩阵

### 1. DEB

在 Debian 10/11/12/13、Ubuntu 20.04/22.04/24.04/26.04 容器中：

```bash
apt-get install -y /pkg/*.deb
ter-music --help
ter-music version
```

额外断言：

```bash
dpkg-deb -f /pkg/*.deb Depends | grep 'libxml2 | libxml2-16'
readelf -d /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music | grep NEEDED
# 不应有 libxml2/libav*/libonnxruntime（发布包静态链接 ONNX Runtime）
dpkg-deb -c /pkg/*.deb | grep 'opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music'  # opt 实体
dpkg-deb -c /pkg/*.deb | grep 'usr/bin/ter-music ->'                                    # /usr 仅链接
sha256sum /opt/apps/cn.org.huansoft.ter-music/files/share/ter-music/models/uvr-mdxnet-3-9662.onnx
# 期望 e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc
```

Ubuntu 26.04 上再执行：

```bash
TER_MUSIC_XML_DEBUG=1 ter-music help 2>&1 | grep libxml2.so.16
```

Debian 10 上应显示 `libxml2.so.2`。

### 2. RPM

在 EL7/8/9/10 容器中：

```bash
dnf install -y /pkg/*.rpm
ter-music --help
ter-music version
rpm -qpR /pkg/*.rpm | grep libxml2
rpm -qpR /pkg/*.rpm | grep libav || true    # 不应有 libav*
rpm -qpR /pkg/*.rpm | grep libonnxruntime || true   # 发布包静态链接，不应动态依赖
rpm -qpl /pkg/*.rpm | grep 'opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music'
rpm -qpl /pkg/*.rpm | grep 'usr/bin/ter-music'   # 必须是符号链接
```

### 3. AppImage

在 Debian 10 与 Ubuntu 24.04/26.04 容器中：

```bash
./ter-music-*.AppImage --appimage-extract-and-run --version
./ter-music-*.AppImage --appimage-extract-and-run help >/dev/null
./ter-music-*.AppImage --appimage-extract
# 解包树断言：opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music 为实体，
# usr/bin/ter-music 等为相对链接，files/share/ter-music/models/ 有模型与 NOTICE
```

### 4. AUR（只验收，不投递）

```bash
./scripts/build/build-aur.sh -v 26.4.1.0
```

成功标准：

- `makepkg` 产出 `.pkg.tar.zst`；
- `pacman -U` 安装成功；
- `ter-music --help` 与 `ter-music version` 通过；
- 仓库 `.SRCINFO` 与 `makepkg --printsrcinfo` 一致；
- `_commit` 指向最终发布提交（本轮临时为 `32737f0`），真实 `source=git+…#commit=` 能拉到该提交。
- AUR 包由源码构建，走 `dlopen` 路径：容器/宿主机需装 Arch 的 `onnxruntime-cpu`（PKGBUILD 的
  optdepends）才能真实分离；未装时验收应记录"原声 + 可读原因"，不得记作静态交付通过。

### 5. ONNX 真实分离（必测，不是只校验模型文件）

发布验收必须在一个**没有系统 `libonnxruntime`** 的容器里安装官方包，并跑一次真实模型分离；
只 `sha256sum` 模型文件不算通过。可直接复用 `scripts/test/karaoke-segments-e2e.sh`：

```bash
# 以 Debian 10 容器为例（已安装 DEB，且容器内没有 libonnxruntime）
scripts/test/karaoke-segments-e2e.sh \
  --bin /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music \
  --model /opt/apps/cn.org.huansoft.ter-music/files/share/ter-music/models/uvr-mdxnet-3-9662.onnx
```

- 该脚本覆盖：会话分段缓存生命周期、首段起播等待、seek 优先、同曲二次播放不重跑模型、
  退出清理、旧版整曲缓存只读优先、预取与 CUE 子曲目守卫；需要 `dbus-run-session`、`python3`、
  `ffmpeg`/`ffprobe`（缺工具时脚本明确 `SKIP`，不得记为 PASS）。
- 另外断言静态交付：

```bash
readelf -d /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music | grep libonnxruntime \
  && echo '意外：二进制仍动态依赖 ONNX Runtime' || echo 'OK：已静态链接'
ldconfig -p | grep onnxruntime || echo 'OK：容器内没有系统 ONNX Runtime'
```

- 若某个目标环境无法静态链接（例如极旧 glibc 的 EL7 变体），必须在发布说明与验收报告里如实
  写成"该包需要系统 `libonnxruntime`，否则原声"，不得含糊。

### 6. 验收矩阵总表

| 目标 | DEB | RPM | AppImage | AUR |
| --- | --- | --- | --- | --- |
| Debian 10/11/12/13、Ubuntu 20.04–26.04 | 安装 + version + libxml2 dlopen 断言 | — | 运行 + 解包断言 | — |
| EL7/8/9/10 | — | 安装 + version + `/usr` 链接断言 | — | — |
| Arch Linux | — | — | — | `makepkg` + `pacman -U` + version |
| 卡拉 OK 真实分离 | `karaoke-segments-e2e.sh` + 无动态 ONNX Runtime 断言 | 同左 | 解包后同左 | 同左 |

## 五、发布检查清单

按顺序执行；每项都要留下命令与 exit code。

1. [ ] **口径**：版本 `26.4.1.0`、tag `v26.4.1.0`、UAB 缓发、AUR 不投递，与
   `docs/README.md` / `docs/RELEASE.md` / `docs/RELEASE_NOTES_26.4.1.0.md` 三处一致。
2. [ ] **文档**（docs-release）：`debian/changelog` 顶部条目、`docs/RELEASE_NOTES_26.4.1.0.md`、
   三语 README、`docs/RELEASE.md`、API 文档、`debian/copyright` 完成并与代码事实一致。
3. [ ] **提交**（captain）：按仓库规矩建临时分支提交、merge 到 `testing` 保留痕迹；不合 master。
4. [ ] **构建**（build-release）：从最终提交执行 `./scripts/build/launch-auto-build.sh -v 26.4.1.0
   -t deb,rpm,appimage,aur`（需要源码/调试包时另加选项）；记录 `build/release/` 清单、大小与
   SHA-256。
5. [ ] `scripts/test/check-package-identity.sh` 通过。
6. [ ] `scripts/test/package-layout-check.sh` 通过（无构建工具的机器可用
   `TM_LAYOUT_STATIC_ONLY=1` 只跑静态断言，不能替代真实包验收）。
7. [ ] DEB 在 8 个目标发行版完成安装/启动验收；Ubuntu 26.04 显示 `libxml2.so.16`。
8. [ ] RPM 在 EL7/8/9/10 完成安装/启动验收。
9. [ ] AppImage 在 Debian 10 与 Ubuntu 24.04/26.04 可运行，解包后 `/opt` 为实体、`/usr` 全为
   链接、模型与 NOTICE 在位。
10. [ ] 卡拉 OK 真实分离在无系统 ONNX Runtime 的容器里通过（`karaoke-segments-e2e.sh` + 静态链接
    断言）。
11. [ ] AUR 本地验收通过（`_commit` 指向最终提交候选），但未推送。
12. [ ] 独立验收（verifier）出具 `.tmp/verify/REPORT.md`，逐项给出证据；FAIL 项不得进入下一步。
13. [ ] **打 tag**（captain）：在最终发布提交上创建 `v26.4.1.0` 并推送；tag 不是构建前置条件。
14. [ ] **上传 GitHub Release**：人工上传 `build/release/` 中的 DEB/RPM/AppImage 与 AUR 包
    （AUR 包也可只作附件），附 `docs/RELEASE_NOTES_26.4.1.0.md` 内容；不包含 UAB/layer。
15. [ ] **AUR 投递**（仅在决定投递时）：把 `_commit` 改为最终发布提交（tag 指向的提交）→
    Arch 容器内 `makepkg --printsrcinfo > .SRCINFO` → `./scripts/build/build-aur.sh -v 26.4.1.0`
    最后一次本地验收 → 推送 `PKGBUILD`/`.SRCINFO` 到 AUR 仓库。
16. [ ] **收尾**：确认 `build/release/` 与 GitHub Release 资产一致；在发布记录中写明版本、
    tag、资产 SHA-256、UAB 缓发原因与 AUR 状态。

## 六、UAB 缓发说明（本次不发布）

- 现象：ll-box ≤ 2.3.5 的冷启动转发在终端队列满时重放已写字节，把重复输出拼进 TUI。
- 上游：linyaps-box PR #295 已合入 `release/2.3`（merge commit `31ddf04`，2026-11-20），但
  **尚无包含该修复的发行版**（最新仍是 2.3.5），`master` 也未带上。
- 决定：ter-music 26.4.1.0 不发布 UAB/layer；等上游释出含修复的版本后，再按本流程补发
  Linyaps 产物（届时需要 `ll-builder` 环境与 `-t linyaps`）。
- 应用内的容器侧守卫只做损害限制，不改变上述上游结论。

## 七、未来投递 AUR（本轮不执行）

需要投递时，人工执行：

1. 更新 `PKGBUILD` 的 `pkgver` 与 `_commit`（必须是最终发布提交）；
2. 在 Arch 容器或本机执行 `makepkg --printsrcinfo > .SRCINFO`；
3. 运行 `./scripts/build/build-aur.sh -v <V>` 完成最后一次本地验收；
4. 再把 `PKGBUILD`/`.SRCINFO` 推送到 AUR 仓库。

tag 不是构建的前置条件；如果发布方决定打 tag，应在验收通过后进行，并与 `_commit` 指向的提交
保持一致。
