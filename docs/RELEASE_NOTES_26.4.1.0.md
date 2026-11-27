# Ter-Music 26.4.1.0 — release notes

- **Application version:** `26.4.1.0` (`APP_VERSION` = `v26.4.1.0`)
- **Package versions:** DEB/RPM/AUR `26.4.1.0-1`
- **Release tag:** `v26.4.1.0` (tag name; created after acceptance, on the final release commit)
- **Previous tag:** `v2.1.0` (2026-08-01). The releases in between (2.2.0, 2.3.0, 2.4.0 and the rolling 26.4.1.0 line) were never tagged.
- **Branch:** `release/26.4.1.0-audit`; this release does not merge `master`.

This release is the first one published with the rolling version scheme `<yy>.<q>.<x>.<y>`. The
version is written once in `types.h` (`APP_VERSION`) and mirrored in the CMake project version,
the PKGBUILD `pkgver`, `debian/changelog` and the RPM spec default; the packaging scripts take
`-v 26.4.1.0` as the only authority and do not read a git tag.

## Release artefacts

| Artefact | Count | Output path | Notes |
| --- | --- | --- | --- |
| DEB | 1 | `build/deb/amd64/cn.org.huansoft.ter-music_26.4.1.0-1_amd64.deb` | Built in Debian 10 (glibc 2.28), covers Debian 10–13 and Ubuntu 20.04–26.04 |
| RPM | 4 | `build/rpm/el{7,8,9,10}/x86_64/cn.org.huansoft.ter-music-26.4.1.0-1.el<N>.x86_64.rpm` | One per EL because the ncursesw/libpng sonames differ; EL7 uses the devtoolset-11 build and libcurl < 7.32 / ncurses 5.x compatibility |
| AppImage | 1 | `build/appimage/x86_64/ter-music-26.4.1.0-x86_64.AppImage` | Built in Debian 10 (glibc 2.28); bundles `libxml2.so.2` and its dependencies |
| AUR | 1 | `build/aur/x86_64/cn.org.huansoft.ter-music-26.4.1.0-1-x86_64.pkg.tar.zst` | Built and installed for acceptance only; **not submitted** to AUR in this release |
| UAB + layer | — | — | **Deferred, not part of this release** (see below) |

DEB/RPM source packages and debuginfo packages are optional and are only produced with
`--with-source` / `--with-debuginfo`.

All packages install real files under `/opt/apps/cn.org.huansoft.ter-music/files` and only six
symlinks under `/usr` (`/usr/bin/ter-music`, the desktop file and four icons). The DEB package
uses absolute targets because `dh_link` links across `/opt` and `/usr` as Debian policy
requires, while the RPMs, the AUR package and the AppImage use relative links; the AppImage
uses the same layout inside its `AppDir`.

### Linyaps UAB is deferred

No UAB or layer is published with 26.4.1.0. The ll-box cold-start forwarder in releases up to
2.3.5 can replay bytes it has already written (splicing duplicated output into the TUI); the
upstream fix is PR #295, merged into linyaps-box `release/2.3` (merge commit `31ddf04`,
2026-11-20), but **no release contains it yet** (the latest release is still 2.3.5) and `master`
does not carry it. UAB artefacts are planned to be published after that upstream fix is released;
ter-music cannot fix the write itself and the container-side guard only bounds the damage. The
Linyaps-related runtime code path remains in the tree and builds, it is just not shipped this time.

## Highlights

### Karaoke: bundled ONNX model, segment cache, real fallback

- Vocal removal is now the bundled **UVR-MDX-NET 3** ONNX model — there is no feature switch and
  no model picker, and the old centre-cancellation DSP has been removed.
- Three levels: **Off** (original), **Faint vocals** (0.4×original mix + 0.6×model accompaniment)
  and **No vocals** (accompaniment only). The level is the switch and is stored as
  `karaoke_mode` (`0/1/2`), reachable from the control-bar popup, Settings → Playback,
  `ter-music karaoke` and `Control.SetKaraoke`.
- Separation runs on the core in a low-priority background thread (nice 10), never on the UI or
  playback thread. Accompaniment is written as 15-second segments (`segNNN.inst.flac`,
  `segNNN.soft.flac` + `segNNN.ready`, indexed by `manifest.txt`) under
  `/tmp/ter-music-karaoke-<uid>-<pid>/<model-id>/<track-hash>/` (mode 0700); one model run produces
  both stems. The whole `/tmp` session directory is removed when the player exits. Session segments
  have their own budget: 512 MB per session by default (`TER_MUSIC_KARAOKE_SEGMENT_MB` overrides
  it; `0` or less disables the budget), the oldest track directories are evicted first, the current
  track is protected and every segment write is preceded by a free-space check with a 32 MiB
  reserve.
- A complete legacy whole-track cache under `$XDG_CACHE_HOME/ter-music/karaoke/` is still read
  (read-only, takes priority, covered by the 2048 MB LRU setting).
- First play of an uncached track at a level other than Off waits for the first accompaniment
  segment (about 7–10 s on the test machine, hard cap 30 s; 8 s when playback starts at a non-zero
  position), a late segment at a boundary or after a seek is waited for up to 8 s, and changing
  the level mid-play switches the source at the current position once the target segment is ready.
  Failure of any kind (missing/damaged model, runtime unavailable, timeout) keeps the original
  playing and names the reason on the status line — the player never claims a switch that did not
  happen and never falls back to an older algorithm.
- Model verification never blocks the UI or playback: the hot path only resolves the model path and
  checks its size (a `stat`, microseconds), while the whole-file SHA-256 runs in the background
  worker before the first separation and is cached per process by path, size and modification time.
  Repairing or replacing the file invalidates that cache, so no restart is needed. Karaoke model,
  worker and cache failures are localized in all three built-in languages — the English UI no
  longer mixes Chinese (messages printed by ONNX Runtime itself remain English technical text).
- Mono tracks and tracks longer than 30 minutes are not separated; CUE sub-tracks use the original
  or a matching legacy cache; `seamless_preload=1` also prefetches the next queue entry.

### ONNX Runtime delivery

- **Official packages are self-contained**: the DEB, all four RPMs (including EL7) and the
  AppImage link a static **ONNX Runtime 1.21** into `ter-music`, so no system `libonnxruntime`
  package is needed on any target and `readelf -d` lists no `libonnxruntime`. The static library is
  built once from the Debian pool `onnxruntime 1.21.0+dfsg` source with a **GLIBC_2.17 symbol
  ceiling** (centos:7 + devtoolset-11), so the same copy serves EL7–10 and the Debian 10+ targets;
  the AppImage binary keeps the Debian 10 (glibc 2.28) baseline. The release acceptance matrix
  below requires a real separation run inside each target container, on a system without the
  runtime library.
- **Source builds** (and builds without the static library), including the AUR package, keep the
  compatible path: the core loads a system `libonnxruntime.so.1.21`, `.so.1` or `.so` with `dlopen`
  and uses the CPU execution provider only. The AUR recipe lists Arch's `onnxruntime-cpu` as an
  optional dependency for this. Without that library separation fails and the original keeps
  playing.
- The bundled model is never downloaded at run time. The build downloads it once, verifies size
  (`29,704,436` bytes) and whole-file SHA-256
  (`e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc`), CMake verifies it again and
  installs it as `<prefix>/share/ter-music/models/uvr-mdxnet-3-9662.onnx` together with
  `models/NOTICE.md`.

### Layouts, appearance, input

- **JSON layouts**: six panes (playlist, info, controls, spectrum, lyrics, vinyl), seven presets,
  forced tiling, per-pane colours and a layout editor; user overrides live in
  `~/.config/ter-music/layouts.json`.
- **Info pane and layout editor**: the info pane now adapts to its space — content-sized columns,
  `…` elision, a cover that shrinks to three rows or yields to text
  (`preferences.info_cover_priority`: auto / text-first / cover-first), importance-ordered line
  dropping and up to three sub-columns on wide, short panes — and the layout editor checks edits
  against the current terminal's minimum readable pane size, snapping with a notice while older
  smaller layouts keep loading unchanged.
- **Terminal light/dark adaptation**: Auto (default), Light or Dark (`preferences.appearance_mode`,
  contract 10). In Auto the TUI probes OSC 11, the deepin-terminal theme adapter, the XDG portal
  `color-scheme` (live `SettingChanged`), `COLORFGBG`, a `TERM` hint and finally the legacy dark
  look; factory colours and the seven presets switch to a light-safe palette and user values stay
  verbatim.
- **One file/folder chooser** for Open folder, Append folder, language-pack install and the
  default music directory: zenity/kdialog (XDG portal preferred) on a graphical session, a typed
  path in a pure TTY; `TERM_MUSIC_FILE_CHOOSER` forces a backend. In the AppImage the external
  dialog (and the external `smbclient`) no longer inherits the wrapper's
  `LD_LIBRARY_PATH`/`LD_PRELOAD`/`LD_AUDIT` — the old behaviour made zenity/kdialog load the
  bundled Debian 10 libstdc++ on modern hosts and die with a GLIBCXX mismatch — and a dialog that
  cannot start or exits abnormally falls back to the typed path with a notice; a user cancel
  still cancels, and a forced backend that is unavailable still reports an explicit error. DEB
  and RPM packages are unaffected.
- **Mouse and menus**: click activation, right-click (or middle-click) context menu, menu
  entrance/exit animation with graceful fallback, wheel changes the selected equalizer band's gain
  by ±1 dB while a click only selects.
- **Configurable key bindings** (contract 9): rebind actions in Settings → Hotkeys, validate with
  `ter-music keys check`, list actions with `ter-music keys list`; only overrides are persisted.

### Remote sources

SMB, SFTP, FTP, WebDAV and HTTP are frontend features: the frontend lists and downloads, the core
plays local paths. TLS is a per-connection switch (including implicit FTPS), certificates are
verified unless "trust self-signed certificate" is set, SFTP verifies `known_hosts` and fails
closed, SMB uses the external `smbclient` with a mode-0600 auth file and refuses names that its
`-c` parser would split, and logs/history store a redacted URL. The remote page shows progress,
supports cancel and retry, applies caps (1 GiB per file, 32 MiB per listing, 200 tracks per open,
10/120/600 s timeouts) and keeps a 2048 MB LRU cache that protects playing/queued/favourited
files.

### Packaging and build tooling

- Cross-distro matrix from one source tree: DEB (Debian 10 build, glibc 2.28), RPM per EL7/8/9/10,
  AppImage (glibc 2.28) and AUR; Linyaps builds remain available but are not shipped this time.
- FFmpeg is linked statically (audio-only configuration, no GPL components); the build images now
  enable its FLAC encoder and muxer (`--enable-encoder="flac,pcm_s16le" --enable-muxer="flac,wav"`),
  which the internal segment cache needs — without them separation would fail when writing the
  `.flac` segments (build-time fix, no user action). libxml2 is not linked at all —
  `util/xml_compat.c` `dlopen`s `libxml2.so.2` or `.so.16` at run time, so one binary runs on old
  and new distributions. libcurl remains a hard build dependency; SMB needs `smbclient` at run
  time.
- Packaging is five packaging scripts (`build-deb.sh`, `build-rpm.sh`, `build-appimage.sh`,
  `build-aur.sh`, `build-linyaps.sh`) plus `launch-auto-build.sh`; helper scripts such as
  `build-onnxruntime-static.sh` are build dependencies, not package types. The portable builder,
  the RPM-conversion path and the old six-builder set are gone; `-v/--version` is mandatory, there
  is no `detect_version` fallback, and four-component versions are accepted instead of being
  truncated.

## Upgrade notes

### Configuration contract v12

`CONFIG_CURRENT_VERSION` is **12**. A `config.xml` below 12 is migrated on the first start of the
new core and rewritten with `version="12"`; every other key (including `karaoke_mode`, theme, paths,
equalizer and key bindings) keeps its value and a second start leaves the file byte-identical.
Version 12 removes `preferences.karaoke_feature` and `preferences.karaoke_model` (the model is
bundled); `config get/set karaoke_feature` now fails as an unknown key and `Config.Set` rejects a
patch that still contains either element. The oldest released file format is contract 4
(tag `v2.1.0`, root attribute literal `"2.2"`, which still loads because the literal parses
as 2 — the minimum accepted value). Downgrades are not supported: a pre-v12 build reads
a v12 file, sees the removed keys as absent and writes the attribute back as `10` on its next save.

Version history, as it exists in the repository: contract 4 = `v2.1.0`; 7 = resident core;
8 = `stop_core_on_exit`; 9 = `<keybindings>` overrides **and** `karaoke_mode`; 10 =
`appearance_mode`; 12 = bundled ONNX model and removal of the DSP-era karaoke keys. No contract 11
build was ever released, so released config files cannot contain the two removed elements.

### D-Bus api_version 7 — upgrade as a pair

`core.api_version` is **7** and the interface namespace is `cn.org.huansoft.ter_music.*`. The
namespace is part of the wire protocol: a pre-7 frontend calling the new core gets
`UnknownInterface`, and a new frontend cannot reach a pre-7 core. `TER_MUSIC_API_MIN_VERSION` is 7,
so the core and every frontend must be upgraded together. The MPRIS bus name
`org.mpris.MediaPlayer2.ter_music` is unchanged. `Info.GetInfo` is the handshake — read
`core.api_version` first.

## Known limitations

- UVR-MDX-NET 3 is a mid/low-size model: vocals can remain in the accompaniment and similar
  instruments/backing vocals can be attenuated. `Faint vocals` deliberately keeps 40 % of the
  original, so it is a compromise, not a clean instrumental.
- Separation is CPU-heavy; on a 6-core Ryzen 5 4500U a 30-second track took about 11 s and a
  3-minute track about a minute, with about 0.8 GiB peak RSS. Weaker machines are slower.
- Session segments cost about 15.5 MB per minute of audio while the player runs. The 512 MB
  session budget (see above) bounds their total; on systems where `/tmp` is a tmpfs (Fedora/RHEL
  default) that budget counts against RAM, so lower `TER_MUSIC_KARAOKE_SEGMENT_MB` if space is
  tight. The directory is deleted when the player exits. A complete legacy whole-track cache under
  `~/.cache/ter-music/karaoke/` is kept until you delete it.
- Mono tracks, tracks over 30 minutes and CUE sub-tracks are not separated into session segments.
- Only ONNX Runtime 1.21 with the CPU execution provider is verified — the official packages use
  1.21.0 built from the Debian pool source, the source/AUR path expects `libonnxruntime.so.1.21`,
  `.so.1` or `.so`; DNNL and other execution providers are deliberately not used. A missing or
  damaged model/runtime never stops playback.
- A source build without `-DTER_MUSIC_BUNDLED_MODEL` contains no model and reports it as missing.
- Linyaps UAB is not part of this release (upstream ll-box fix not released yet); AUR is not
  submitted yet.
- The AUR recipe pins an explicit source commit (`_commit`); it is temporarily pinned to
  `32737f0` (the `testing` tip before this release branch) and is bumped to the final release
  commit before submission.
- Remote sources were verified against local fixtures (HTTP/WebDAV/FTP/FTPS/TLS/401/slow
  downloads; SMB through a faithful fake `smbclient`; SFTP known_hosts guard). A real Samba
  server, a real SSH server and the FTPS data channel were not available to the suite.
- The ll-box ≤ 2.3.5 cold-start replay limitation remains for Linyaps users; the in-app guard only
  bounds the damage.

## Installation and acceptance

CI runs only deterministic gates as required checks and keeps the full real-model karaoke suite in
an advisory job that does not block merges (see [BUILD_GUIDE.md](BUILD_GUIDE.md) §9.5); the release
acceptance below is the authoritative full run. All commands are run from the repository root on
the build host unless a container is named.

### DEB — Debian 10/11/12/13, Ubuntu 20.04/22.04/24.04/26.04

```bash
docker run --rm -v "$PWD/build/deb/amd64:/pkg:ro" <image> \
  bash -c 'apt-get update && apt-get install -y /pkg/*.deb && ter-music --help && ter-music version'
dpkg-deb -f build/deb/amd64/*.deb Depends | grep 'libxml2 | libxml2-16'
dpkg-deb -c build/deb/amd64/*.deb | grep 'opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music'
dpkg-deb -c build/deb/amd64/*.deb | grep 'usr/bin/ter-music ->'
sha256sum /opt/apps/cn.org.huansoft.ter-music/files/share/ter-music/models/uvr-mdxnet-3-9662.onnx
# expected e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc
readelf -d /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music | grep NEEDED
# must not list libonnxruntime, libxml2 or libav*
```

### RPM — EL7/8/9/10

```bash
dnf install -y /pkg/*.rpm && ter-music --help && ter-music version
rpm -qpl /pkg/*.rpm | grep 'opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music'
rpm -qpl /pkg/*.rpm | grep 'usr/bin/ter-music'   # must be a symlink
rpm -qpR /pkg/*.rpm | grep -E 'libxml2|libonnxruntime' || true
# EL7: extract the binary and check the static ONNX Runtime and the glibc ceiling
rpm2cpio /pkg/*.el7.x86_64.rpm | cpio -idm ./opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music
readelf -d  ./opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music | grep libonnxruntime || true
readelf -V  ./opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
# expected: no libonnxruntime, highest symbol GLIBC_2.17
```

### AppImage

```bash
./build/appimage/x86_64/ter-music-26.4.1.0-x86_64.AppImage --appimage-extract-and-run --version
./build/appimage/x86_64/ter-music-26.4.1.0-x86_64.AppImage --appimage-extract
# opt/apps/... is real, usr/{bin,share} is symlinks, models/ has the model + NOTICE
```

### AUR (acceptance only, not submitted)

```bash
./scripts/build/build-aur.sh -v 26.4.1.0
# makepkg output installed with pacman -U; ter-music version; .SRCINFO matches
# makepkg --printsrcinfo
```

The AUR package compiles from source and therefore takes the `dlopen` path for karaoke: install
`onnxruntime-cpu` from the Arch repositories (its optional dependency) to enable separation; the
DEB/RPM/AppImage packages do not need it.

### Real karaoke separation (must pass, not just a model checksum)

The strongest acceptance assertion for this release is a real separation run, not a file hash.
Install the package in a container that has **no** system `libonnxruntime` (the official packages
are self-contained) and run the segment end-to-end suite against the installed binary and model —
at least once on Debian 10 (DEB) and once in the `centos:7` container (EL7 RPM):

```bash
# inside Debian 10 / Ubuntu 24.04 (DEB) or centos:7 (RPM) with ffmpeg/dbus/python3 available
scripts/test/karaoke-segments-e2e.sh \
  --bin /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music \
  --model /opt/apps/cn.org.huansoft.ter-music/files/share/ter-music/models/uvr-mdxnet-3-9662.onnx
```

The suite covers the session cache lifetime, first-segment start, seek priority, replay without a
second model run, exit cleanup, legacy whole-track priority, preload and the CUE guard. It must
actually write `segNNN.inst.flac`/`segNNN.soft.flac`; if writing the FLAC segments fails, the
build image was built without FFmpeg's FLAC encoder/muxer (see BUILD_GUIDE.md). A `SKIP` for a
missing tool must be reported as a skip, never as a pass. Also verify that the binary really does
not depend on the system runtime:

```bash
readelf -d .../bin/ter-music | grep libonnxruntime && echo "unexpected dynamic dependency" || echo OK
ldconfig -p | grep onnxruntime || echo "no system ONNX Runtime (expected)"
```

### Matrix summary

| Target | DEB | RPM | AppImage | AUR |
| --- | --- | --- | --- | --- |
| Debian 10/11/12/13, Ubuntu 20.04–26.04 | install + version + `libxml2` dlopen check | — | run + self-extract | — |
| EL7/8/9/10 | — | install + version + `/usr` symlink + static ONNX Runtime (`readelf`, GLIBC_2.17) check | — | — |
| Arch Linux | — | — | — | makepkg + `pacman -U` + version |
| Karaoke separation (Debian 10 + EL7 at minimum) | `karaoke-segments-e2e.sh` + no dynamic ONNX Runtime + FLAC segments written | same | same (extracted) | `dlopen` + `onnxruntime-cpu` |

## Licensing

- ter-music is **GPL-3.0-only** (see `LICENSE`).
- The bundled UVR-MDX-NET 3 model is distributed under **MIT**; its notice is installed as
  `models/NOTICE.md` and recorded in `debian/copyright`.
- The release binaries link a statically built **FFmpeg** under **LGPL-2.1-or-later**, configured
  without `--enable-gpl`; the corresponding source (`ffmpeg-8.1.2.tar.xz`) and the exact configure
  line are recorded in `scripts/docker/Dockerfile.debian10` and `Dockerfile.rpm` and in
  `debian/copyright`.
- ONNX Runtime (static in the official packages) is **MIT**; its C API headers are vendored under
  `include/third_party/onnxruntime/`.

## Links

- Repository: <https://github.com/HuanSoft-Open-Source-Community/ter-music>
- Build & packaging guide: [BUILD_GUIDE.md](BUILD_GUIDE.md)
- Release process: [RELEASE.md](RELEASE.md)
- README (English / 简体中文 / 文言): [README.md](README.md) ·
  [README_zh-CN_Modern.md](translations/README_zh-CN_Modern.md) ·
  [README_zh-CN_Legacy.md](translations/README_zh-CN_Legacy.md)
