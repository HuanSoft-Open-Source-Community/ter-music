# Maintainer: Zeta <zeta@localhost>
# Contributor: HuanSoft Open Source Community - yxzl

pkgname=cn.org.huansoft.ter-music
pkgver=26.4.1.0
pkgrel=1
# 组织迁移：Arch 包名由 ter-music-cn 改为本名。replaces 让 pacman 在安装新包时接管
# 旧包的同名文件（两边都装 /usr/bin/ter-music），不需要先手动 remove；AUR 上的
# 旧包仓库本身要由维护者单独下架，这一步不在本文件的能力范围内。
replaces=('ter-music-cn')
pkgdesc="Terminal based music player"
# MIT 是随包分发的内置消音模型（uvr-mdxnet-3-9662）的上游许可；应用本身 GPL。
license=('GPL-3.0-only' 'MIT')
arch=('x86_64')
url="https://github.com/HuanSoft-Open-Source-Community/ter-music"
depends=('ffmpeg' 'ncurses' 'libxml2' 'libpng' 'libjpeg-turbo' 'curl' 'sqlite' 'zlib')
optdepends=('pulseaudio: PulseAudio audio output'
            'alsa-lib: ALSA audio output'
            'libpipewire: PipeWire audio output'
            'dbus: MPRIS media session integration'
            'smbclient: SMB/CIFS remote music sources')
makedepends=('cmake' 'make' 'gcc' 'git' 'pkgconf')

# 安装布局（与 deb/rpm/appimage/linyaps 统一，遵循 Linyaps 目录规范）：
#   实体文件 -> /opt/apps/$_appid/files/{bin,share,...}
#   /usr/bin、/usr/share/{applications,icons/hicolor} -> 指向实体的相对链接
_appid=cn.org.huansoft.ter-music
_prefix=/opt/apps/$_appid/files

# 不依赖 git tag：固定 commit，发布时人工更新 _commit。
_commit=675bbb4172fc5f7b26d4488953256ee401c2f9f6
# 内置消音模型（UVR-MDX-NET 3 / 9662）：构建时由 makepkg 下载并按 sha256sums
# 校验；本地构建（scripts/build/build-aur.sh）会把缓存文件预置到 $srcdir 复用。
_model=UVR_MDXNET_3_9662.onnx
_model_url=https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/UVR_MDXNET_3_9662.onnx

source=("ter-music::git+https://github.com/HuanSoft-Open-Source-Community/ter-music.git#commit=$_commit" "$_model::$_model_url")
sha256sums=('SKIP' 'e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc')

prepare() {
  cd "$srcdir/ter-music"
  # Arch ncurses 包把头文件直接装在 /usr/include，无 ncursesw 子目录
  find . \( -name "*.c" -o -name "*.h" \) -exec sed -i 's|ncursesw/||g' {} +
}

build() {
  cd "$srcdir/ter-music"
  mkdir -p build
  cd build
  cmake .. \
    -DCMAKE_INSTALL_PREFIX="$_prefix" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTER_MUSIC_VERSION="$pkgver" \
    -DTER_MUSIC_BUNDLED_MODEL="$srcdir/$_model"
  make
}

check() {
  cd "$srcdir/ter-music"
  ./build/ter-music --help >/dev/null
  ./build/ter-music version | grep -q "$pkgver"
}

package() {
  cd "$srcdir/ter-music"
  DESTDIR="$pkgdir" cmake --install build

  # /usr 只放指向 /opt 实体的相对符号链接；pacman 卸载时逐条安全移除。
  install -d "$pkgdir/usr/bin" \
             "$pkgdir/usr/share/applications" \
             "$pkgdir/usr/share/icons/hicolor/32x32/apps" \
             "$pkgdir/usr/share/icons/hicolor/48x48/apps" \
             "$pkgdir/usr/share/icons/hicolor/128x128/apps" \
             "$pkgdir/usr/share/icons/hicolor/scalable/apps"

  ln -sfn "../../../opt/apps/$_appid/files/bin/ter-music" \
      "$pkgdir/usr/bin/ter-music"
  ln -sfn "../../../opt/apps/$_appid/files/share/applications/ter-music.desktop" \
      "$pkgdir/usr/share/applications/ter-music.desktop"
  local icon_base="../../../../../../opt/apps/$_appid/files/share/icons/hicolor"
  ln -sfn "$icon_base/32x32/apps/ter-music.png" \
      "$pkgdir/usr/share/icons/hicolor/32x32/apps/ter-music.png"
  ln -sfn "$icon_base/48x48/apps/ter-music.png" \
      "$pkgdir/usr/share/icons/hicolor/48x48/apps/ter-music.png"
  ln -sfn "$icon_base/128x128/apps/ter-music.png" \
      "$pkgdir/usr/share/icons/hicolor/128x128/apps/ter-music.png"
  ln -sfn "$icon_base/scalable/apps/ter-music.svg" \
      "$pkgdir/usr/share/icons/hicolor/scalable/apps/ter-music.svg"
}
