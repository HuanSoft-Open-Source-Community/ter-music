# ONNX Runtime C API 头文件（vendored）+ 静态运行库

本目录只包含 **ONNX Runtime C API 的公开头文件**，以及一份说明：ONNX Runtime 在本项目里
是怎么被接进来的。

两种接入方式，对调用方完全透明（区别只在 `karaoke_model_runtime_library()` 的文案）：

| 构建方式 | ONNX Runtime 如何提供 | 谁在用 |
|---|---|---|
| **静态链接**（`-DTER_MUSIC_ONNXRUNTIME_STATIC=ON`，AUTO 探测到静态库时也会启用） | 静态库由 `scripts/build/build-onnxruntime-static.sh` 从 Debian pool 的 `onnxruntime 1.21.0+dfsg` 源码构建并链接进 `ter-music`；二进制**不需要**系统 `libonnxruntime.so*` | 官方 deb / rpm / AppImage（构建镜像 `Dockerfile.debian10`、`Dockerfile.rpm`） |
| **运行期 dlopen**（默认，源码构建 / 开发机；机器上没有静态库时） | 编译期零链接，运行期按 `libonnxruntime.so.1.21` → `.so.1` → `.so` 顺序 `dlopen`；缺库时"内置消音"不可用，原声播放不受影响 | `cmake` 直接构建、发行版自己打包（例如 Arch 用系统 `onnxruntime`） |

**两种方式都不会把 ONNX Runtime 写进 `DT_NEEDED`**：静态链接是编进二进制，动态方式是
运行期 `dlopen`（`readelf -d` 都看不到 `libonnxruntime`）。这也是发布验收的硬条件
（`scripts/test/karaoke-model-e2e.sh` 的 S13）。

为什么要静态（2026-11-27 发布前审计 B3）：模型是随包内置的，但 Debian 只有 `trixie (13)`
及之后才提供 `libonnxruntime1.21/1.23`，Debian 10/11/12 与 EL7–10 都没有这个包，AppImage
也不自带 —— 不静态化就是"装得上、卖点用不了"。

## 来源与版本

| 项 | 值 |
|---|---|
| 上游仓库 | https://github.com/microsoft/onnxruntime |
| 版本（tag） | `v1.21.0`（对应 `ORT_API_VERSION 21`） |
| 上游路径 | `include/onnxruntime/core/session/onnxruntime_c_api.h` |
| 获取 URL | https://raw.githubusercontent.com/microsoft/onnxruntime/v1.21.0/include/onnxruntime/core/session/onnxruntime_c_api.h |
| 本地文件 | `onnxruntime_c_api.h`（未做任何修改） |
| 字节数 | 219,712 |
| SHA256 | `7783e9664bdb0b1b36145b5c8587dd1bc9a399aa000edf2e3dd03003cafc6882` |
| 许可证 | MIT（见同目录 `LICENSE`；头文件本身也带有 `Copyright (c) Microsoft Corporation` / `Licensed under the MIT License` 头注释） |

> 2026-11 起 `github.com/microsoft/onnxruntime` 与 `download.onnxruntime.ai` 已 404，
> 上面的 raw URL 现在取不到；头文件是此前 vendored 的原始内容（SHA256 未变）。
> 静态库的源码改从 Debian pool 取（`onnxruntime_1.21.0+dfsg.orig.tar.xz`，
> 37,970,624 B，sha256 `32df1f82…0273`，与 Debian `.dsc` 一致），详见下节。

## 为什么 vendored

Debian/Deepin 的 `libonnxruntime1.21` 包只提供运行库、不提供开发头文件，而本项目需要
稳定的 C API 结构体布局来 `dlsym` + `GetApi(21)`（动态方式）或直接 `OrtGetApiBase()`
（静态方式）。头文件只参与本项目的编译（类型/函数指针声明）。

只用到 1.21 C API 中早已存在的稳定子集（`CreateEnv`、`CreateSessionOptions`、
`CreateSession`、`Run`、tensor/type-info 查询、内存信息等）；两种方式都按
`ORT_API_VERSION`（21）向下尝试到 16，取第一个可用的 API 表，因此旧运行库
（若函数表布局兼容）也能加载。

## 静态库（仅构建期用，不随本目录分发）

- 构建脚本：`scripts/build/build-onnxruntime-static.sh`（幂等；`--force` 重编、
  `--clean` 清缓存；构建镜像 `scripts/docker/Dockerfile.onnxruntime-static`）。
- 产物（gitignore，不入库）：`.cache/tools/onnxruntime-static/`：
  - `libonnxruntime.a`（合并后的单一静态库，PIC；含 ORT 子目标与
    protobuf-lite / onnx / flatbuffers / date / cpuinfo / re2 / abseil 等第三方归档）
  - `cxx/libstdc++.a`（**链接期**归档：ORT 是 C++，用 GCC 11 编译，目标文件引用
    GLIBCXX_3.4.29；Debian 10 的系统 libstdc++ 只到 3.4.25、EL7 更低，必须静态带上）
  - `PROVENANCE.txt` / `deps-manifest.txt` / `build.log`
- 构建镜像把它装到 `/usr/local/lib/onnxruntime-static/`，CMake 侧
  `TER_MUSIC_ONNXRUNTIME_STATIC`（`AUTO`/`ON`/`OFF`，默认 `AUTO`）自动启用。
- 工具链：`centos:7` + `devtoolset-11`（gcc 11.2.1），产物最高只引用 **GLIBC_2.17**
  符号 —— 同一份静态库可以给 Debian 10（glibc 2.28）与 EL7–10 全矩阵用。
- ORT 1.21 的 `element_wise_ops.cc` 用了 Eigen 的 `min/max<Eigen::PropagateNaN>`，
  而 Debian 的 eigen 3.4.0 发行版没有这个模板参数（上游钉的是 3.4.0 之后的提交，
  gitlab 在本构建环境 403）→ 构建时施加 Debian 自己的
  `disable-propagate-nan-in-eigen3.patch`（Debian 的 onnxruntime 包也这么做）。
- 第三方依赖由脚本在宿主预取（`codeload.github.com`；`github.com/.../archive/...`
  在 2026-11 会挂死），按 ORT `cmake/deps.txt` 的 SHA1 校验后用
  `FETCHCONTENT_SOURCE_DIR_*` 指给 CMake，并开 `FETCHCONTENT_FULLY_DISCONNECTED`。

## 模型权重的分发（config v12 起）

模型权重（`.onnx`）**不进入 git 仓库**，但自 config v12 起随安装包分发：
打包时由 `TER_MUSIC_BUNDLED_MODEL` 注入并安装到
`<安装前缀>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`，运行期只读取并
校验大小 + SHA256 后直接使用（不复制到 XDG）。模型许可、署名、来源 URL 与
"上游无独立 LICENSE 文件"的事实见包内 `resources/models/NOTICE.md`
（随包安装到 `<前缀>/share/ter-music/models/NOTICE.md`）。
