# 内置消音模型第三方声明（Third-Party Notice: bundled vocal-removal model）

本文件随包安装到 `<安装前缀>/share/ter-music/models/NOTICE.md`，与
`uvr-mdxnet-3-9662.onnx` 同目录。自 config v12 起，本应用**随包分发**该模型
文件作为数据资产（不进入 git），运行期只读取、校验并直接使用，不复制到
`$XDG_DATA_HOME`。

## 1. 模型

| 项 | 值 |
|---|---|
| 模型名（上游） | UVR-MDX-NET 3（`UVR_MDXNET_3_9662.onnx`） |
| 应用内 id | `uvr-mdxnet-3-9662` |
| 包内路径 | `share/ter-music/models/uvr-mdxnet-3-9662.onnx` |
| 字节数 | `29,704,436` |
| 整文件 SHA256 | `e02220e80d8253f4c2209f8924298b2b686bbdf2868b788ff5500fb9bd94aadc` |
| 官方下载 URL | https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/UVR_MDXNET_3_9662.onnx |
| 上游模型仓库 | https://github.com/TRvlvr/model_repo |
| 上游应用项目 | Ultimate Vocal Remover GUI (UVR) — https://github.com/Anjok07/ultimatevocalremovergui |
| 署名（MIT 义务） | Anjok07、TRvlvr 及 Ultimate Vocal Remover GUI 贡献者 |
| 许可 | MIT（本文件第 3 节附许可全文） |
| 是否修改 | **否**：逐字节原样分发；应用只做大小 + SHA256 校验后读取推理 |

## 2. 许可说明（含"上游无独立 LICENSE 文件"的事实）

- UVR 主项目（`Anjok07/ultimatevocalremovergui`）以 **MIT** 许可发布，本应用
  据此在保留署名与许可声明的前提下分发该模型权重。
- **事实澄清（必须如实记录）**：模型所在的上游发布渠道
  `TRvlvr/model_repo`（GitHub Releases `all_public_uvr_models`）**没有提供
  独立的 LICENSE 文件**，release 资产只有 `.onnx` 权重本身。因此本声明不
  杜撰上游不存在许可文件：MIT 条款依据的是 UVR 主项目仓库的 `LICENSE` 与
  UVR 官方 `model_data.json` 中该模型的条目，而非某个模型级 LICENSE 文件。
- 本应用整体以 **GPL** 发布；MIT 与 GPL 兼容。模型权重作为**独立的数据
  资产**随包分发（非链接进二进制），其 MIT 义务由本文件与应用内"设置 →
  播放设置"页的署名/许可/SHA256 展示共同满足。
- 分发方式：作为普通数据文件安装（`TER_MUSIC_BUNDLED_MODEL` 在构建时注入，
  见 `CMakeLists.txt`），不进入 git 仓库；运行期校验失败（缺失/大小不符/
  SHA256 不符）会回退原声并在状态行提示，绝不加载未校验的权重。

## 3. MIT 许可全文

```
MIT License

Copyright (c) Ultimate Vocal Remover GUI (UVR) contributors
(Anjok07, TRvlvr and contributors)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 4. 运行期依赖说明（非本模型的一部分）

ONNX Runtime 不是本模型的一部分：官方发布包（DEB/RPM/AppImage）把它**静态链接**
进 `ter-music` 二进制（MIT 许可，随包携带，系统无须另装 `libonnxruntime`）；源码
构建在没有静态库时保留兼容路径，在运行期 `dlopen` 系统安装的 ONNX Runtime（C API
头文件以 MIT 许可 vendored，见 `include/third_party/onnxruntime/NOTICE.md`）。
运行库不可用时消音不可用，应用照常播放原声并在状态行提示原因。
