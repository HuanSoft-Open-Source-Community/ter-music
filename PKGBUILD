# Maintainer: Zeta <zeta@localhost>
# Contributor: yxzl

pkgname=ter-music-cn
pkgver=2.1.0
pkgrel=1
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
  # i18n
  install -Dm644 data/lang/zh_CN.xml "$pkgdir/usr/share/ter-music/lang/zh_CN.xml"
  install -Dm644 data/lang/en_US.xml "$pkgdir/usr/share/ter-music/lang/en_US.xml"
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
