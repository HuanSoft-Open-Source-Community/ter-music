# ter-music RPM spec
#
# 版本由构建脚本通过 --define "_version <V>" 注入；这里的默认值只用于
# 手工 rpmbuild 的兜底。调试包由 build-rpm.sh 控制：
#   - 默认传 --define "debug_package %%{nil}"，不产 debuginfo/debugsource；
#   - --with-debuginfo 时不传该宏，并传 --define "_tm_build_type RelWithDebInfo"。

%{!?_version:%global _version 26.4.1.0}
%{!?_tm_build_type:%global _tm_build_type Release}

%if 0%{?rhel} == 7
%global _tm_cmake cmake3
%global _tm_cmake_cc -DCMAKE_C_COMPILER=/opt/rh/devtoolset-11/root/usr/bin/gcc
%else
%global _tm_cmake cmake
%global _tm_cmake_cc %{nil}
%endif

%global binname ter-music

Name:           cn.org.huansoft.ter-music
Version:        %{_version}
Release:        1%{?dist}
Summary:        A terminal-based music player with ncurses interface
License:        GPL-3.0-or-later
URL:            https://github.com/HuanSoft-Open-Source-Community/ter-music
Source0:        %{name}-%{_version}.tar.gz
ExclusiveArch:  x86_64

Provides:       ter-music = %{version}-%{release}
Obsoletes:      ter-music < 26.4.1.0

BuildRequires:  gcc, make, pkgconfig
BuildRequires:  ncurses-devel
BuildRequires:  libcurl-devel
BuildRequires:  libpng-devel
BuildRequires:  libjpeg-turbo-devel
BuildRequires:  libxml2-devel
BuildRequires:  dbus-devel
BuildRequires:  sqlite-devel
BuildRequires:  zlib-devel
%if 0%{?rhel} == 7
BuildRequires:  cmake3
%else
BuildRequires:  cmake
%endif

# libxml2 由运行期 dlopen 加载（.so.2 / .so.16），不会由 rpmbuild 从
# ELF NEEDED 自动生成依赖，因此这里显式声明。
Requires:       libxml2

%description
Ter-Music is a lightweight terminal-based music player for Linux systems.
It uses FFmpeg for audio decoding, PulseAudio, PipeWire or ALSA for audio
output, and ncursesw for the text user interface.

%prep
%setup -q -n %{name}-%{_version}

%build
mkdir -p build
cd build
CMAKE_CC_ARGS="%{_tm_cmake_cc}"
%{_tm_cmake} .. \
    -DCMAKE_BUILD_TYPE=%{_tm_build_type} \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_FLAGS="$RPM_OPT_FLAGS" \
    -DSTATIC_LINKING=ON \
    -DTER_MUSIC_VERSION=%{_version} \
    ${CMAKE_CC_ARGS}
make %{?_smp_mflags}

%install
cd build
make install DESTDIR=%{buildroot}

%files
%{_bindir}/ter-music
%{_datadir}/applications/ter-music.desktop
%{_datadir}/icons/hicolor/*/apps/ter-music.*
%{_datadir}/ter-music
%license LICENSE
