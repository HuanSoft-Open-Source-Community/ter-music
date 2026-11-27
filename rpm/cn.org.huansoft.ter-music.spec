# ter-music RPM spec
#
# 版本由构建脚本通过 --define "_version <V>" 注入；这里的默认值只用于
# 手工 rpmbuild 的兜底。调试包由 build-rpm.sh 控制：
#   - 默认传 --define "debug_package %%{nil}"，不产 debuginfo/debugsource；
#   - --with-debuginfo 时不传该宏，并传 --define "_tm_build_type RelWithDebInfo"。
#
# 安装布局（与 deb/aur/appimage/linyaps 统一，遵循 Linyaps 目录规范）：
#   - 实体文件：/opt/apps/cn.org.huansoft.ter-music/files/{bin,share,...}
#   - /usr/bin、/usr/share/applications、/usr/share/icons/hicolor 下只放
#     指向 /opt 实体的相对符号链接（可由 rpm 在卸载时安全移除）。
#
# 内置消音模型：build-rpm.sh 用 tm_ensure_model 下载/校验后复制到源码树
# 的 .tm-bundled-model/UVR_MDXNET_3_9662.onnx（.tmp staging 内），%build 通过
# -DTER_MUSIC_BUNDLED_MODEL 注入；CMake 会再校验大小 + SHA256 才安装。

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
License:        GPL-3.0-only AND MIT
URL:            https://github.com/HuanSoft-Open-Source-Community/ter-music
Source0:        %{name}-%{_version}.tar.gz
ExclusiveArch:  x86_64

# 安装布局与模型路径（在 Name/Version 之后展开，避免宏定义顺序问题）：
#   - 实体：/opt/apps/<name>/files
#   - 模型：rpmbuild 的 BUILD 目录里 staged 的 .tm-bundled-model/
%global tm_prefix /opt/apps/%{name}/files
%global tm_model_file %{_builddir}/%{name}-%{_version}/.tm-bundled-model/UVR_MDXNET_3_9662.onnx

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
# SMB/CIFS 远程音乐源是可选能力：运行时调用 smbclient（F-06/R-05）。
# RHEL7 的 rpm 不支持弱依赖，故仅在新版本声明 Recommends。
%if 0%{?rhel} != 7
Recommends:     smbclient
%endif

%description
Ter-Music is a lightweight terminal-based music player for Linux systems.
It uses FFmpeg for audio decoding, PulseAudio, PipeWire or ALSA for audio
output, and ncursesw for the text user interface.

%prep
%setup -q -n %{name}-%{_version}

%build
mkdir -p build
cd build
test -f "%{tm_model_file}" || {
    echo "missing bundled karaoke model: %{tm_model_file}" >&2
    echo "build with scripts/build/build-rpm.sh (tm_ensure_model downloads/verifies it)" >&2
    exit 1
}
CMAKE_CC_ARGS="%{_tm_cmake_cc}"
%{_tm_cmake} .. \
    -DCMAKE_BUILD_TYPE=%{_tm_build_type} \
    -DCMAKE_INSTALL_PREFIX=%{tm_prefix} \
    -DCMAKE_C_FLAGS="$RPM_OPT_FLAGS" \
    -DSTATIC_LINKING=ON \
    -DTER_MUSIC_VERSION=%{_version} \
    -DTER_MUSIC_BUNDLED_MODEL="%{tm_model_file}" \
    ${CMAKE_CC_ARGS}
make %{?_smp_mflags}

%install
cd build
make install DESTDIR=%{buildroot}

# /usr 下只建指向 /opt 实体的相对符号链接（rpm 卸载时逐条安全移除）。
install -d %{buildroot}/usr/bin
install -d %{buildroot}/usr/share/applications
install -d %{buildroot}/usr/share/icons/hicolor/32x32/apps
install -d %{buildroot}/usr/share/icons/hicolor/48x48/apps
install -d %{buildroot}/usr/share/icons/hicolor/128x128/apps
install -d %{buildroot}/usr/share/icons/hicolor/scalable/apps

ln -sfn ../../opt/apps/%{name}/files/bin/%{binname} \
    %{buildroot}/usr/bin/%{binname}
ln -sfn ../../../opt/apps/%{name}/files/share/applications/%{binname}.desktop \
    %{buildroot}/usr/share/applications/%{binname}.desktop
link_base=../../../../../../opt/apps/%{name}/files/share/icons/hicolor
ln -sfn ${link_base}/32x32/apps/%{binname}.png \
    %{buildroot}/usr/share/icons/hicolor/32x32/apps/%{binname}.png
ln -sfn ${link_base}/48x48/apps/%{binname}.png \
    %{buildroot}/usr/share/icons/hicolor/48x48/apps/%{binname}.png
ln -sfn ${link_base}/128x128/apps/%{binname}.png \
    %{buildroot}/usr/share/icons/hicolor/128x128/apps/%{binname}.png
ln -sfn ${link_base}/scalable/apps/%{binname}.svg \
    %{buildroot}/usr/share/icons/hicolor/scalable/apps/%{binname}.svg

%files
%{tm_prefix}
/usr/bin/%{binname}
%{_datadir}/applications/%{binname}.desktop
%{_datadir}/icons/hicolor/*/apps/%{binname}.*
%license LICENSE
