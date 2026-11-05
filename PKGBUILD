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
arch=('x86_64')
url="https://github.com/HuanSoft-Open-Source-Community/ter-music"
license=('GPL-3.0-only')
depends=('ffmpeg' 'ncurses' 'libxml2' 'libpng' 'libjpeg-turbo' 'curl' 'sqlite' 'zlib')
optdepends=('pulseaudio: PulseAudio audio output'
            'alsa-lib: ALSA audio output'
            'libpipewire: PipeWire audio output'
            'dbus: MPRIS media session integration')
makedepends=('cmake' 'make' 'gcc' 'git' 'pkg-config')
source=("ter-music::git+https://github.com/HuanSoft-Open-Source-Community/ter-music.git#tag=v$pkgver")
sha256sums=('SKIP')

prepare() {
  cd "$srcdir/ter-music"
  # Arch ncurses 包提供 <ncurses.h>，无 ncursesw 子目录
  find . \( -name "*.c" -o -name "*.h" \) -exec sed -i 's|<ncursesw/ncurses.h>|<ncurses.h>|g' {} +
}

build() {
  cd "$srcdir/ter-music"
  mkdir -p build
  cd build
  cmake .. \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release
  make
}

package() {
  cd "$srcdir/ter-music"
  # binary
  install -Dm755 build/ter-music "$pkgdir/usr/bin/ter-music"
  # help
  install -Dm644 data/help/help-quickstart-zh_CN.txt "$pkgdir/usr/share/ter-music/help/help-quickstart-zh_CN.txt"
  install -Dm644 data/help/help-quickstart-en_US.txt "$pkgdir/usr/share/ter-music/help/help-quickstart-en_US.txt"
  install -Dm644 data/help/help-quickstart-zh-PQ.txt "$pkgdir/usr/share/ter-music/help/help-quickstart-zh-PQ.txt"
  # layouts
  install -Dm644 data/layouts/layouts.json "$pkgdir/usr/share/ter-music/layouts/layouts.json"
  # i18n
  install -Dm644 data/lang/zh_CN.xml "$pkgdir/usr/share/ter-music/lang/zh_CN.xml"
  install -Dm644 data/lang/en_US.xml "$pkgdir/usr/share/ter-music/lang/en_US.xml"
  install -Dm644 data/lang/zh-PQ.xml "$pkgdir/usr/share/ter-music/lang/zh-PQ.xml"
  # desktop entry
  install -Dm644 data/applications/ter-music.desktop "$pkgdir/usr/share/applications/ter-music.desktop"
  # icons (XDG hicolor theme)
  for s in 32x32 48x48 128x128 scalable; do
    ext=png
    [ "$s" = "scalable" ] && ext=svg
    install -Dm644 "resources/icons/hicolor/$s/apps/ter-music.$ext" \
      "$pkgdir/usr/share/icons/hicolor/$s/apps/ter-music.$ext"
  done
}
