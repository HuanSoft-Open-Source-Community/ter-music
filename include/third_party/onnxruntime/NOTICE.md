# ONNX Runtime C API 头文件（vendored）

本目录只包含 **ONNX Runtime C API 的公开头文件**，用于在编译期描述运行时 ABI。
本项目的 ONNX Runtime **不是** 编译期/链接期依赖：运行期用 `dlopen` 加载系统
`libonnxruntime.so*`（见 `src/cn.org.huansoft.ter-music/audio/karaoke_model.c`）。
`CMakeLists.txt` 不链接 ONNX Runtime，二进制也不会写出它的 `DT_NEEDED`。

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

## 为什么 vendored

Debian/Deepin 的 `libonnxruntime1.21` 包只提供运行库、不提供开发头文件，
而本项目需要稳定的 C API 结构体布局来 `dlsym` + `GetApi(21)`。头文件只
参与本项目的编译（类型/函数指针声明），不随运行时一起工作。

只用到 1.21 C API 中早已存在的稳定子集（`CreateEnv`、`CreateSessionOptions`、
`CreateSession`、`Run`、tensor/type-info 查询、内存信息等）；运行时按
`ORT_API_VERSION`（21）向下尝试到 16，取第一个可用的 API 表，因此旧运行库
（若函数表布局兼容）也能加载。

## 模型权重的分发（config v12 起）

模型权重（`.onnx`）**不进入 git 仓库**，但自 config v12 起随安装包分发：
打包时由 `TER_MUSIC_BUNDLED_MODEL` 注入并安装到
`<安装前缀>/share/ter-music/models/uvr-mdxnet-3-9662.onnx`，运行期只读取并
校验大小 + SHA256 后直接使用（不复制到 XDG）。模型许可、署名、来源 URL 与
"上游无独立 LICENSE 文件"的事实见包内 `resources/models/NOTICE.md`
（随包安装到 `<前缀>/share/ter-music/models/NOTICE.md`）。ONNX Runtime 本身
仍不是随包分发内容（见上文）。
