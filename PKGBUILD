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
makedepends=('cmake' 'make' 'gcc' 'git' 'pkgconf')

# 不依赖 git tag：固定 commit，发布时人工更新 _commit。
_commit=675bbb4172fc5f7b26d4488953256ee401c2f9f6
source=("ter-music::git+https://github.com/HuanSoft-Open-Source-Community/ter-music.git#commit=$_commit")
sha256sums=('SKIP')

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
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DTER_MUSIC_VERSION="$pkgver"
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
}
