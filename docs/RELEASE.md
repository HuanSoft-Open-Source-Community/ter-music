# Ter-Music 发布与验收流程

本流程只使用仓库内的打包系统。版本号由 `-v/--version` 指定，构建脚本不读取 git tag，也不要求先打 tag。默认不产出源码包或调试包；需要时显式传入 `--with-source` / `--with-debuginfo`。

## 一、准备版本

```bash
./scripts/build/launch-auto-build.sh -v 26.4.1.0
```

- 版本格式为 `<yy>.<q>.<x>.<y>`；三段会自动补 `.0`。
- 二进制 `APP_VERSION` 由 CMake 注入，与包版本一致。
- `types.h`、`CMakeLists.txt`、`PKGBUILD pkgver`、`debian/changelog` 顶部、RPM spec 默认 `_version` 仍保持同一个开发默认值；打包时以 `-v` 为准，构建脚本会在临时副本里重写 `debian/changelog` 与 `PKGBUILD` 的版本。

## 二、默认产物

| 类型 | 数量 | 路径 |
| --- | --- | --- |
| DEB | 1 | `build/deb/amd64/cn.org.huansoft.ter-music_<V>-1_amd64.deb` |
| RPM | 4 | `build/rpm/el7|8|9|10/x86_64/cn.org.huansoft.ter-music-<V>-1.el<N>.x86_64.rpm` |
| AppImage | 1 | `build/appimage/x86_64/ter-music-<V>-x86_64.AppImage` |
| Linyaps UAB | 1 | `build/linyaps/x86_64/cn.org.huansoft.ter-music_<V>_x86_64.uab` |
| Linyaps layer | 1 | `build/linyaps/x86_64/*_binary.layer` |
| AUR | 1 | `build/aur/x86_64/cn.org.huansoft.ter-music-<V>-1-x86_64.pkg.tar.zst` |

所有产物同时复制到 `build/release/`。

- DEB 在 Debian 10（glibc 2.28）中构建，覆盖 Debian 10/11/12/13 与 Ubuntu 20.04/22.04/24.04/26.04。
- 仅 FFmpeg 静态；libxml2 由 `util/xml_compat.c` 在运行期 `dlopen` `.so.2`/`.so.16`。
- RPM 每个 EL 一个：EL7 的 ncursesw/libpng soname 与 EL8+ 不同，单包无法在仅 FFmpeg 静态下可靠覆盖。
- AppImage 自带 `libxml2.so.2` 及其依赖。
- Linyaps 的 UAB 与 layer 必须同时成功。

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
readelf -d /usr/bin/ter-music | grep NEEDED      # 不应有 libxml2/libav*
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
```

### 3. AppImage

在 Debian 10 与 Ubuntu 24.04/26.04 容器中：

```bash
./ter-music-*.AppImage --appimage-extract-and-run --version
./ter-music-*.AppImage --appimage-extract-and-run help >/dev/null
```

### 4. Linyaps

```bash
ll-cli install build/linyaps/x86_64/*.uab
ll-cli run cn.org.huansoft.ter-music -- ter-music version

ll-cli uninstall cn.org.huansoft.ter-music
ll-cli install build/linyaps/x86_64/*.layer
ll-cli run cn.org.huansoft.ter-music -- ter-music version
```

两者都应成功；UAB 的 `.note.uab.sig` 校验已经在构建阶段完成。

### 5. AUR

```bash
./scripts/build/build-aur.sh -v 26.4.1.0
```

成功标准：

- `makepkg` 产出 `.pkg.tar.zst`；
- `pacman -U` 安装成功；
- `ter-music --help` 与 `ter-music version` 通过；
- 仓库 `.SRCINFO` 与 `makepkg --printsrcinfo` 一致。

本轮**不投递 AUR、不创建 git tag**。

## 五、发布检查清单

1. [ ] 确认 `-v` 与准备发布的版本一致。
2. [ ] `./scripts/build/launch-auto-build.sh -v <V>` 全部类型成功。
3. [ ] `scripts/test/check-package-identity.sh` 通过。
4. [ ] DEB 在 8 个目标发行版完成安装/启动验收；Ubuntu 26.04 显示 `libxml2.so.16`。
5. [ ] RPM 在 EL7/8/9/10 完成安装/启动验收。
6. [ ] AppImage 在 Debian 10 与 Ubuntu 24.04/26.04 可运行。
7. [ ] Linyaps UAB 与 layer 分别 `ll-cli install` 成功。
8. [ ] AUR 本地验收通过，但未推送。
9. [ ] 如需源码/调试包，确认 `--with-source` / `--with-debuginfo` 产物存在。
10. [ ] 以上全部通过后，是否打 tag、是否投递 AUR 由人工另行决定；构建系统本身不依赖 tag。

## 六、未来投递 AUR（本轮不执行）

需要投递时，人工执行：

1. 更新 `PKGBUILD` 的 `pkgver` 与 `_commit`；
2. 在 Arch 容器或本机执行 `makepkg --printsrcinfo > .SRCINFO`；
3. 运行 `./scripts/build/build-aur.sh -v <V>` 完成最后一次本地验收；
4. 再把 `PKGBUILD`/`.SRCINFO` 推送到 AUR 仓库。

tag 不是构建的前置条件；如果发布方决定打 tag，应在验收通过后进行，并与 `_commit` 指向的提交保持一致。
