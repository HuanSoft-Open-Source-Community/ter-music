# Ter-Music 发布流程（本地真实发行版构建 + 手动上传）

> 2026-08 起，Release 不再由 GitHub Actions 自动打包（`.github/workflows/release.yml` 已移除）。
> 打包改为在**真实的 Debian / Fedora / Deepin 机器**上原生构建（不依赖 Docker），再手动上传到 GitHub Release。
> GitHub Actions 仅保留 CI（`.github/workflows/ci.yml`，编译检查）。本地 Docker 构建体系（`scripts/docker/`、Dockerfiles、`launch-auto-build.sh`）未受影响，仍可作替代方案使用。

## 版本准备

1. 更新 `include/cn.org.huansoft.ter-music/types.h` 中的 `APP_VERSION`。版本号为
   **四段**（例如 `"v26.4.1.0"`）；下文示例一律写作 `X.Y.Z.W`，指本次要发的完整版本号。
2. Git tag 名必须与 APP_VERSION 一致（`vX.Y.Z.W` 格式），发布时按此打 tag。

### 滚动发行与版本号落点

自 26.4.1.0 起改用**滚动发行**：不再攒大版本，功能合入主线后即可随时发版。版本号方案为
`<yy>.<q>.x.y` 四段——年前两位、季度、本季度主线序、修订位（`26.4.1.0` 即 2026 年第 4
季度第 1 条主线的 0 号修订）。换季度不要求跳号：`x` 只在本季度内递增，`y` 用于同一条
主线的 hotfix。

版本号只有**一个事实源**：`types.h` 的 `APP_VERSION`（`ter-music --version`、关于界面、
日志抬头、`Info.GetInfo` 的 `instance.version` 都读它）。发布时必须同步镜像的四处：

| 位置 | 字段 | 本例取值 |
| ---- | ---- | -------- |
| `include/cn.org.huansoft.ter-music/types.h` | `APP_VERSION` | `"v26.4.1.0"` |
| `CMakeLists.txt` | `project(... VERSION ...)` | `26.4.1.0` |
| `PKGBUILD` | `pkgver` | `26.4.1.0` |
| `debian/changelog` | 顶部条目版本 | `26.4.1.0-1` |

`scripts/build/` 六个打包脚本的 `detect_version()` **先读 `types.h`，再退到 git tag**，
最后才用脚本内的 `DEFAULT_VERSION` 兜底。这个顺序是刻意的：tag 曾落后于代码（仓库里
最新 tag 是 `v2.1.0`，而 `APP_VERSION` 已是 `v2.3.0`），旧顺序会把包版本打成旧 tag 的
值。正则同时放宽到四段，否则 `26.4.1.0` 会被截成 `26.4.1`。

3. 确认 CI 全绿：`gh run list --workflow ci.yml`。
4. 行为变更登记（架构反转：后端只做播放、前端持有内容的那一版）：
   - **职责边界**：核心（`daemon`）只做播放服务——音频设备、播放状态与进度、
     传输命令、音量/倍速/播放模式、执行前端下发的**本地路径队列**、当前曲目
     信息（歌词/封面/可视化）、配置、前端注册与心跳；前端（TUI/CLI）持有
     文件系统与内容——曲库（SQLite）、扫描与元数据、播放列表、歌单、收藏/
     历史/目录历史、排序/过滤/搜索、远程源与下载缓存、界面；
   - **D-Bus `api_version` 3 → 4**：撤下内容接口 `Playlist`、`Library`、
     `Favorites`、`History`、`DirHistory`，`Control.OpenPath`/`PlayIndex`/
     `GetPlaylist` 一并撤下；`Queue` 改为**路径队列**语义
     （`Set/Append/InsertAfter/RemoveAt/MoveUp/MoveDown/Clear/Shuffle/PlayAt/Get`，
     仅本地路径、单次 ≤500 条、分页读 ≤1000 条），新增 `QueueChanged` 广播；
     播放面接口为 `Lyrics`/`Info`/`Control`/`Queue`/`Config`（当时 46 个方法）；
   - **前端默认走远程门面**：`ter-music` / `ter-music tui` 默认
     `--frontend=remote`（无核心则自动拉起并把内容队列推给核心），
     新增 `--attach-only`（无核心直接退出码 3）与 `--bus NAME`；
     `--frontend=local` 保留为本机进程内播放的回归基线（后续版本删除）；
   - **`daemon --open` 语义变更**：核心不再扫描目录。`daemon start --open <目录>`
     现在是「先起核心 → 由该 CLI 进程（前端）扫描 → `Queue.Set` → `Queue.PlayAt`」；
     `daemon foreground --open` 给出明确提示；`ter-music play <目录>` 同路径；
   - **退出语义**：`q` 只退前端，核心继续播放；新增配置
     `core_exit_when_no_frontend`（默认 0，配置版本 6 → 7），置 1 时最后一个
     前端离开并过 10 秒宽限期后核心自行退出；此外，核心所属的**会话总线消失时
     核心自行退出**（总线没了就没有任何前端能联系到它，继续占着音频设备只会
     变成一个谁也停不掉的孤儿进程）；从未拿到总线的核心不受影响；
   - **次级实例管理**：`daemon start --force` 起的次级实例（总线名
     `<主名>.instance<pid>`）不再"隐身"——`daemon status` 在 stderr 逐个列出
     （pid + 总线名，stdout 维持原样以便脚本解析）、`daemon start --force`
     报告新建实例的总线名、`daemon stop --all` 一次停掉主实例与全部次级实例
     （主实例已死、只剩次级实例时同样可收尾）；
   - **配置文件归属**：`config.xml` 由核心独占写（前端经 `Config.Set` 提交，
     不直接写文件）；前端自有数据为曲库 `library.db`、`remote.xml` 与
     远程下载缓存 `$XDG_CACHE_HOME/ter-music/remote/`；队列不再落
     `queue.txt`（内容由前端的内容列表恢复，游标由 `resume_last_playback` 恢复）；
   - **只播本地文件**：MPRIS `OpenUri`、`Queue.Set` 与 CLI/daemon 的路径参数
     都只接受本地路径，收到远程 URL 会明确报错（远程源由前端下载成缓存文件
     后交给核心播放）；
   - **D-Bus `api_version` 4 → 5（歌词逐字时间，增量兼容）**：
     `Lyrics.GetDocument` 的每行增加 `end`、`timing`（`none`/`line`/`chars`）、
     `char_total`，逐字段 LRC 另带相对行首的毫秒数组 `chars`；
     `Lyrics.GetLyrics` 与 `Info.GetInfo` 的 `lyrics` 对象增加
     `char_sung`/`char_total`。新前端/CLI 只要求核心 `api_version >= 4`
     （v4 核心缺字段时逐字高亮降级为整行），v4 前端/CLI 也能接入 v5 核心；
     TUI 歌词面板按逐字点亮，旧式只有行首时间戳的歌词按「本行起点 → 下一行
     起点」均分（最后一行沿用前一段时长）。
    - **D-Bus `api_version` 5 → 6（卡拉OK/消人声，增量兼容）**：
      新增 `Control.SetKaraoke`（`(i mode) -> b`，越界回 `false` 而不报错）与
      `Control.GetKaraoke`（`() -> i`，回引擎当前实际生效的档位）；配置新增
      `preferences.karaoke_mode`（0=不启用、1=人声较弱、2=消除人声，越界钳制到
      0/2）。两方法都**不**列入 `k_required_methods`，（当时）握手门槛仍是
      `api_version >= 4`，所以 v5 前端接 v6 核心照旧可用（`api_version` 7 起门槛
      抬到 `>= 7`，见下条）；v6 前端接 v5 核心时
      `SetKaraoke` 得到 `UnknownMethod`，记 warn 后把档位显示为「不启用」
      （v5 核心没有这个能力，这就是它的真实状态）。算法为频带受限的中置消除：
      中置通道 `m=(L+R)/2` 用两段 4 阶 Butterworth（24dB/倍频，180Hz 低通 +
      6kHz 高通）分出 `lo`/`hi`，只减带内 `band=m-lo-hi`，即 `m'=m-α·band`
      （α=0/0.6/1.0），侧置通道 `d=(L-R)/2` 保持不变，`L'=m'+d`、`R'=m'-d`
      重建立体声；换档时 α 以 30ms 时间常数渐变并重新起测两条分频延时线，仅在
      样本越过数字满量程时做 tanh 软限幅。**仅立体声生效**，单声道逐比特直通；
      DSP 在播放线程的写入批次内、**排在 `eq_process` 之前**（seek、换曲、流
      重开处都调 `karaoke_reset()`）。入口三处：主页控制栏新增第 8 个按钮
      （弹层选档，滚轮无动作）、设置 → 播放设置新增末项、CLI 新增
      `ter-music karaoke [off|soft|full|0-2|cycle]`；不新增任何按键动作。
      **配置契约版本仍为 9**（新增普通键不改版本：旧版忽略该元素，缺元素即 0）。
      随附回归脚本 `scripts/test/karaoke-e2e.sh` 与单测 `test_karaoke.c`
      （衰减量、单声道直通、越界钳制）。
    - **D-Bus `api_version` 6 → 7（组织命名空间迁移，破坏性）**：接口与错误前缀由
      `org.yxzl.ter_music.*` 整体改为 `cn.org.huansoft.ter_music.*`，方法面本身一字
      未动。命名空间属于线上协议，故这不是增量：旧前端调新核心每个方法都得
      `UnknownInterface`，新前端连旧核心连 `Control.Attach`/`Info.GetInfo` 都取不到，
      不存在双栈期，`TER_MUSIC_API_MIN_VERSION` 因此由 4 抬到 7，核心与前端必须成对
      升级。总线名 `org.mpris.MediaPlayer2.ter_music`（含 `.instance<pid>` 备用名）、
      可执行文件名 `ter-music`、`/usr/share/ter-music` 与 `~/.config/ter-music` 均
      **不变**；同一版本里包名、源码/头文件目录、玲珑 APP_ID、systemd 用户单元一并
      迁到 `cn.org.huansoft`（对照表与人工待办见「〇、组织迁移」节）。自省清单因此
      只认新前缀，`scripts/test/dbus-rpc-check.sh` 增加「自省里不得残留
      `org.yxzl.`」的闭合断言。
    - **外观系统修正（布局底层改为强制平铺分栏树）**：
      - 布局 v2 JSON 以 `tree` 为权威模型：分栏节点按 `row`（左右）或
        `col`（上下）切分，`ratio` 为千分比，叶节点引用窗格种类。任何
        解析后的树都天然铺满 `1000×1000` 主区域，不再依赖事后几何校验；
        `panes` 矩形仅作为旧前端兼容镜像随文件一起写出；
      - v1 自由矩形仍可读：能 guillotine 切割的布局精确转换为树；
        空洞、重叠、越界、隐藏/重复窗格及非 guillotine 布局按阅读顺序
        自动适配并标记 `normalized`，不再整份拒绝；设置页重新加载时
        报告自动适配数量；`lyrics`/`vinyl` 并存时保留 `vinyl`；
      - 内置预设 `dual`（"六格回退"）移除，预设由 8 个改为 7 个
        （`classic`/`vinyl`/`karaoke`/`visualizer`/`focus`/`stage`/`neon`）；
        已选 `dual` 的选中状态自动回退到默认布局；
      - 控制窗格的嵌入式可视化（封面+频谱条）改为**仅在布局没有独立
        `spectrum` 窗格时绘制**（即旧版行为）；`focus`/`karaoke` 观感不变，
        `classic`/`vinyl` 等不再出现同一屏两份频谱；歌词窗格的无歌词
        黑胶回退动画同理不再画频谱条（独立 `vinyl` 窗格的频谱柱不受影响）；
      - **TUI 布局编辑器重做**（设置 → 界面外观 → 编辑布局/新建布局）：
        直接编辑分栏树，左侧窗格列表、右侧实时预览、底部三行按键提示；
        方向键按位置选择、Tab 循环、`[`/`]` 与 PgUp/PgDn 调比例、`=` 均分、
        `|`/`-` 分栏、`x` 删除、`t` 换类型、`c` 调色、`w` 交换、`Enter`
        操作菜单、`?` 帮助、`S` 保存、`Esc`/`q` 关闭（有未保存修改需二次
        确认）。所有操作在事务中校验并保持强制平铺，不再有保存时的几何
        拒绝；保存写入 v2 用户布局文件并热重载立即生效；编辑内置预设 =
        同 id 用户覆盖；颜色优先级为布局逐面板显式颜色 > 主题色，
        颜色面板 `r`/`R` 把单字段/全部字段重置为"跟随主题"。
      - **科乐美彩蛋回归修复（前景流光 + 反色高亮 + 黑胶碟面）**：在信息页输入
        Konami 序列后，不再改写主题/布局配色，也不铺渐变底色；普通格子只改
        前景，背景统一透明（终端不支持默认色时回退黑）。色板只排除纯黑/纯白
        和黑↔白轴上的中性灰，保留带色相的彩黑/彩白过渡灰，使相邻色阶更连贯；
        256 色终端走 xterm 6×6×6 色立方，8 色档（16/88 色也归入这一档）只用
        ANSI 1..6，黑白终端静默不触发。
        反色/高亮选中格（播放列表光标行、菜单/弹层选中行）不再跳过：流光落到
        反转后的背景，文字用黑/白对比色，相当于前景/背景互换。反色格保留
        `A_REVERSE`（`A_STANDOUT` 归一为反色）并把色对写成“流光前景 + 对比
        背景”，这样重复合成时仍能识别高亮格，不会在前景/背景两套语义之间来回
        切换而闪烁。中心封面与频谱柱保持原色；黑胶碟面除中心封面外重染为流光
        色，保留暗/中/亮三档与旋转高光（最亮档跟高光角度走）。主视图窗格统一经 `ui_refresh_window()` 先
        合成再 `wrefresh`，消除频谱/黑胶/歌词/进度在基础色与流光色之间交替的
        快速闪动（这是刷新顺序问题，不是数据竞态）；黑胶重染也走同一刷新入口。
        覆盖主界面窗格、F1–F8 视图、提示/状态栏与弹层，宽字符两格同色；彩蛋
        不写 `config.xml`/`layouts.json`，关闭后重绘即恢复原配色（含碟面色对）。
        阻塞式文本输入/对话框期间动画暂停。
    回归脚本：`scripts/test/check-core-purity.sh`、`check-ui-purity.sh`、
    `check-config-ownership.sh`、`check-package-identity.sh`（包名/版本/命名空间身份矩阵，
    组织迁移与滚动版本号即由它守门）、`run-unit-tests.sh`、`dbus-rpc-check.sh`、
   `config-migration-check.sh`、`lifecycle-e2e.sh`、`offline-reconnect-e2e.sh`、
   `multi-frontend-e2e.sh`、`paging-deepdir-e2e.sh`、`remote-frontend-e2e.sh`、
   `perf-check.sh`（CI 的 `gates` 作业跑三条门禁与单测，`e2e` 作业跑全部 e2e，
   并以前述性能脚本的 `--report` 记录实测数值）。
   - 历史登记（远程音乐源移交前端的那一版）：配置 schema v5 → v6 把旧的
     `<remote_connections>` 段搬到前端自有的 `<configdir>/remote.xml`
     （密码密文原样保留，文件权限 0600），核心配置此后不再含该段；核心不再
     发布 `cn.org.huansoft.ter_music.Remote`，`core.api_version` 升为 3。

> **原则：验收通过后才打 tag，绝不提前打 tag。**
> 构建产物、测试、验收全部在 tag 之前完成（构建脚本用 `git archive HEAD`
> 取源码，不依赖 tag）；tag 只在确认可发布后创建，且必须打在构建所用的
> 同一个 commit 上（打 tag 前 `git rev-parse HEAD` 与构建时比对）。
>
> **注意：** 构建脚本的 `detect_version()` 以 `types.h` 的 `APP_VERSION` 为第一优先，
> git tag 只是次级兜底；因此打 tag 之前构建不会把版本打成旧 tag。只有源码树缺少
> `types.h`（或需要临时覆盖）时才显式传 `-v X.Y.Z.W`。

---

## 〇、组织迁移的对照与人工待办（26.4.1.0 起）

仓库内的痕迹已一次性迁完，下表右侧是**当前真实状态**（可在本机核对）；表外的事本仓库
改不了，必须由发布方逐项执行，否则文档会与外部现实脱节。

| 项目 | 旧 | 新 | 迁移性质 |
| ---- | -- | -- | -------- |
| 源码/头文件目录 | `src/org.yxzl.ter-music/`、`include/org.yxzl.ter-music/` | `src/cn.org.huansoft.ter-music/`、`include/cn.org.huansoft.ter-music/` | 仓库内部，`#include` 前缀同批改 |
| Debian 源包与二进制包 | `ter-music` | `cn.org.huansoft.ter-music` | `debian/control` 带 `Provides`/`Breaks`/`Replaces`，可原地替换旧包 |
| Arch 包 | `ter-music-cn` | `cn.org.huansoft.ter-music` | `PKGBUILD` 带 `replaces`；**AUR 侧需人工新建/改名** |
| 玲珑（Linyaps）应用 ID | `org.yxzl.ter-music` | `cn.org.huansoft.ter-music` | `/opt/apps/<ID>` 安装路径随之变 |
| systemd 用户单元 | `org.yxzl.ter-music.service` | `cn.org.huansoft.ter-music.service` | 单元名即文件名，旧 enabled 链接需人工处理 |
| D-Bus 接口与错误前缀 | `org.yxzl.ter_music.*` | `cn.org.huansoft.ter_music.*` | **`api_version` 7，破坏性**；核心与前端成对升级 |
| MPRIS 总线名 | `org.mpris.MediaPlayer2.ter_music`（含 `.instance<pid>` 备用名） | 不变 | 标准名，桌面媒体键不受影响 |
| 可执行文件、数据目录、配置目录 | `ter-music`、`/usr/share/ter-music`、`~/.config/ter-music` | 不变 | 用户脚本、快捷键与既有配置一律照旧 |
| 配置文件契约版本 | `9` | 不变 | 本次只加 `karaoke_mode` 元素，未升版 |

人工待办（本仓库改不了的外部状态）：

1. **打 tag**：`v26.4.1.0` 必须在构建/AUR 推送之前存在——`PKGBUILD` 的 `#tag=v$pkgver`
   是前向引用，tag 不存在时 AUR 用户直接构建失败。
2. **AUR**：新建或改名 `cn.org.huansoft.ter-music` 仓库，推送 `PKGBUILD` + `.SRCINFO`；
   旧 `ter-music-cn` 由维护者决定下架或保留说明。README 里的 `yay/paru` 命令已按
   新名写好，AUR 侧未落地前它们不可用。
3. **apt / dnf 发布仓库**：把包从旧名迁到新名（索引组件、GPG 策略由发布方决定）。
   deb 侧已用 `Provides`/`Breaks`/`Replaces` 保证同一台机器上可直接替换，无需先卸载。
4. **通知第三方 D-Bus 客户端**：接口前缀改为 `cn.org.huansoft.ter_music.*`。这是
   `api_version` 7 的全部内容，方法面没变；旧客户端不会静默降级，而是每调用都得
   `UnknownInterface`。
5. **systemd 用户单元**：老用户升级后执行
   `systemctl --user disable --now org.yxzl.ter-music.service`，再
   `systemctl --user enable --now cn.org.huansoft.ter-music.service`，否则开机自启仍
   指向已消失的旧单元文件。
6. **玲珑**：应用 ID 变更后它是**另一个应用**，旧版不会被新版覆盖；待新版验收通过后
   人工 `ll-cli uninstall org.yxzl.ter-music`。用户数据目录是否随 ID 迁移，需按玲珑
   运行时实际行为验证后再定策略，本文不臆断。

---

## 一、Debian 机器构建 .deb

在真实的 Debian（推荐 Debian 13）机器上执行：

```bash
# 构建工具链与开发库
sudo apt install -y \
    build-essential cmake pkg-config \
    dpkg-dev fakeroot devscripts \
    libavcodec-dev libavfilter-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev libpng-dev libjpeg-dev \
    libpulse-dev libncursesw5-dev libcurl4-openssl-dev libxml2-dev \
    libsqlite3-dev libdbus-1-dev zlib1g-dev

# 构建（原生模式，不使用 Docker）
bash scripts/build/build-deb.sh -v X.Y.Z.W -a amd64
```

产物：

- `build/deb/amd64/cn.org.huansoft.ter-music_X.Y.Z.W-1_amd64.deb`
- 源码包：`build/deb/source/cn.org.huansoft.ter-music_X.Y.Z.W-1.dsc`、`cn.org.huansoft.ter-music_X.Y.Z.W.orig.tar.gz`、`cn.org.huansoft.ter-music_X.Y.Z.W-1.debian.tar.xz`

---

## 二、Fedora 机器构建 .rpm

在真实的 Fedora（推荐 Fedora 40+）机器上执行：

```bash
# 构建工具链与开发库
sudo dnf install -y \
    gcc make cmake pkgconf-pkg-config rpm-build \
    ffmpeg-free-devel libpng-devel libjpeg-turbo-devel \
    pulseaudio-libs-devel ncurses-devel libcurl-devel libxml2-devel \
    sqlite-devel alsa-lib-devel pipewire-devel dbus-devel zlib-devel

# 构建（原生模式，不使用 Docker）
bash scripts/build/build-rpm.sh -v X.Y.Z.W -a x86_64
```

产物：

- `build/rpm/x86_64/cn.org.huansoft.ter-music-X.Y.Z.W-1.<dist>.x86_64.rpm`（`<dist>` 如 `.fc41`）
- 对应 `.src.rpm`

---

## 三、Deepin 机器构建 linyaps（linglong）

在真实的 Deepin（推荐 Deepin V25）机器上执行：

```bash
# linglong 构建器与工具链（Deepin 仓库自带）；ll-builder 需要 user namespace 支持
sudo apt install -y linglong-builder linglong-box cmake make

# 构建（原生模式：ll-builder 自带 ll-box 容器，不经过 Docker）
bash scripts/build/build-linyaps.sh -v X.Y.Z.W -a x86_64
```

> **两条极易踩的前提**（详见 `docs/BUILD_GUIDE.md` 的 Linyaps 章节）：
> 1. `ll-builder --version` 必须与 `ll-cli --version` 匹配。1.13.x 的 `ll-builder`
>    导出 UAB 时**不回填** `.note.uab.sig` 段的摘要，包能生成但 1.14+ 的 `ll-cli`
>    安装会被拒绝（`section .note.uab.sig has an invalid digest`）；构建脚本已加
>    导出后断言，出问题会在打包阶段直接失败。
> 2. 1.14 的 UAB 导出还需要 `cn.org.linyaps.builder.utils ≥ 0.0.4.0`
>    （`ll-builder` 内部常量 `minimumBuilderUtilsVersion`）。官方 stable 源若只到
>    0.0.2.0，需用 linglong 源码根目录的 `linglong.yaml`（版本号即 0.0.4.0）
>    本地 `ll-builder build` 出该层并让本机仓库能解析到它。

产物：

- `build/linyaps/x86_64/cn.org.huansoft.ter-music_X.Y.Z.W_x86_64.uab`
- `build/linyaps/x86_64/cn.org.huansoft.ter-music_X.Y.Z.W.0_x86_64_binary.layer`

---

## 四、AppImage / Portable（任一 Linux 机器）

在任意 Linux（如上面的 Debian 机器）上执行：

```bash
# AppImage 需要 squashfs-tools（unsquashfs）
sudo apt install -y squashfs-tools   # Debian/Deepin
# 或
sudo dnf install -y squashfs-tools   # Fedora

# 构建 AppImage（原生）
bash scripts/build/build-appimage.sh -v X.Y.Z.W -a x86_64

# 构建便携包（原生）
bash scripts/build/build-portable.sh -v X.Y.Z.W -a x86_64
```

产物：

- `build/appimage/x86_64/ter-music-X.Y.Z.W-x86_64.AppImage`
- `build/portable/x86_64/ter-music-X.Y.Z.W-portable-x86_64.tar.gz`

---

## 五、验收、打 tag、上传 GitHub Release（手动）

收集齐 5 类产物后，**先在干净环境完成安装与启动验收**（见文末检查清单），
全部通过才执行以下步骤：

```bash
# 1. 确认 HEAD 与构建所用 commit 一致（验收期间不能有新提交混入）
git rev-parse HEAD

# 2. 打 tag（必须与 APP_VERSION 一致）并推送
git tag vX.Y.Z.W && git push origin vX.Y.Z.W

# 3. 创建 Release 并上传产物
gh release create vX.Y.Z.W \
    build/deb/amd64/*.deb \
    build/deb/source/cn.org.huansoft.ter-music_X.Y.Z.W-1.dsc \
    build/deb/source/cn.org.huansoft.ter-music_X.Y.Z.W.orig.tar.gz \
    build/deb/source/cn.org.huansoft.ter-music_X.Y.Z.W-1.debian.tar.xz \
    build/rpm/x86_64/cn.org.huansoft.ter-music-X.Y.Z.W-1.*.x86_64.rpm \
    build/rpm/x86_64/cn.org.huansoft.ter-music-X.Y.Z.W-1.*.src.rpm \
    build/appimage/x86_64/ter-music-X.Y.Z.W-x86_64.AppImage \
    build/portable/x86_64/ter-music-X.Y.Z.W-portable-x86_64.tar.gz \
    build/linyaps/x86_64/cn.org.huansoft.ter-music_X.Y.Z.W_x86_64.uab \
    build/linyaps/x86_64/cn.org.huansoft.ter-music_X.Y.Z.W.0_x86_64_binary.layer \
    --generate-notes
```

> 万一验收后发现必须重新构建：删掉本地 tag（未推送时）或重打 tag 前
> 确认远端无人基于旧 tag 构建；tag 一旦推送并被 AUR 使用就**不可移动**。

---

## 六、更新 AUR（cn.org.huansoft.ter-music，最后一步）

> **AUR 更新必须在 tag 推送之后进行。** 仓库里的 `PKGBUILD` / `.SRCINFO`
> 早已指向 `#tag=vX.Y.Z.W`（前向引用），若 tag 尚未推送，AUR 用户构建会
> 直接失败（历史上曾因乱打 tag / tag 与 AUR 不同步出过构建问题，务必
> 按顺序执行）。

```bash
# 1. 确认 tag 已推送（无输出则说明 tag 不存在，禁止继续）
git ls-remote --tags origin vX.Y.Z.W

# 2. 拉取 AUR 独立仓库
git clone ssh://aur@aur.archlinux.org/cn.org.huansoft.ter-music.git aur-ter-music

# 3. 同步打包文件（pkgver 应已在本仓库 bump 阶段改好）
cp PKGBUILD .SRCINFO aur-ter-music/
cd aur-ter-music

# 4. 本地完整预演：真实拉取 tag 源并构建，验证可复现
makepkg -s

# 5. 提交推送
git add PKGBUILD .SRCINFO
git commit -m "Update to X.Y.Z.W"
git push
```

> tag 一经推送即不可删除或移动（AUR 构建可复现性依赖 tag 不变）。
> 若推送后发现严重问题，只能 bump 到下一个版本号重走流程。

---

## 七、替代方案：本地 Docker 静态构建（跨版本兼容单包）

需要"一个包兼容多个发行版版本"（静态链接 FFmpeg，消除 soname 差异）时，使用既有本地 Docker 体系：

```bash
# 一键构建全部 5 种包（deb/rpm 走 Docker；linyaps/appimage/portable 为原生构建）
bash scripts/build/launch-auto-build.sh --arch amd64

# 或单独构建
bash scripts/build/build-deb.sh --static -v X.Y.Z.W -a amd64     # 静态 FFmpeg，兼容 Debian 10-13
bash scripts/build/build-rpm.sh --container -v X.Y.Z.W -a x86_64  # Rocky Linux 容器，兼容 EL8/9/10
```

> 原生构建（上文一到四节）与 Docker 静态构建互不冲突，产物命名一致，可按需选择。

---

## 发布检查清单

按执行顺序逐项勾选：

1. [ ] 补全 `debian/changelog` 本次变更条目（不允许只有 "Bump version"）
2. [ ] `APP_VERSION`（types.h）与待打 tag 一致，且 CMake `project VERSION`、`PKGBUILD pkgver`、`debian/changelog` 顶部条目四处同值
3. [ ] CI（ci.yml）全绿
4. [ ] 5 类产物齐备：.deb、.rpm（+src.rpm）、.AppImage、portable .tar.gz、.uab/.layer
5. [ ] 在干净 Debian / Fedora 环境各验证一次安装与启动（`--help`）
6. [ ] 栈安全回归（历史上曾因深目录栈溢出导致启动即段错误）：
   - 用 ≥10 层嵌套目录（如 `~/Documents`）启动 TUI，须正常加载播放列表、不得崩溃；
     沙箱（`ll-cli run`）与原生各测一次
   - `ulimit -s 512` 下启动 TUI 亦须正常（修复后启动路径栈需求 < 1 MB）
   - 复查栈帧：编译时加 `-fstack-usage`，不得出现 > 256 KB 的静态栈帧
     （`scan_directory_recursive` 曾为 1.26 MB、`library_load_into_playlist` 曾为 5.17 MB）
7. [ ] 在 Linyaps 环境验证 CLI 与后台播放（`ll-cli install` 后执行）：
   - 安装后先确认集成文件已导出（缺一即判失败）：
     - `ls /var/lib/linglong/entries/share/dbus-1/services/org.mpris.MediaPlayer2.ter_music.service`
     - `grep '^ExecStart' /var/lib/linglong/entries/lib/systemd/user/cn.org.huansoft.ter-music.service`
     - 两者都应含 `ll-cli run cn.org.huansoft.ter-music`（ll-builder 重写的结果）；
       激活文件必须来自包内 `files/share/dbus-1/services/`（放在 `share/services/` 不会被导出）
   - `ll-cli run cn.org.huansoft.ter-music -- ter-music version`
   - `ll-cli run cn.org.huansoft.ter-music -- ter-music daemon start`（经 D-Bus 激活；`ll-cli ps` 应列出应用）
   - 容器已在运行时读取信息须用 `ll-cli enter`（`ll-cli run` 的输出会接到该容器上）：
     `ll-cli enter cn.org.huansoft.ter-music -- /opt/apps/cn.org.huansoft.ter-music/files/bin/ter-music show --one-line`
   - 宿主侧 `systemctl --user start cn.org.huansoft.ter-music`（常驻服务，`ExecStart` 应被重写为 `ll-cli run …`）
   - 宿主侧 `busctl --user introspect org.mpris.MediaPlayer2.ter_music /org/mpris/MediaPlayer2`
     应可见 `cn.org.huansoft.ter_music` 的 `Info`／`Control`／`Lyrics` 三个接口
   - `ll-cli run cn.org.huansoft.ter-music -- ter-music daemon stop` 后 `ll-cli ps` 不再列出该应用
8. [ ] 确认 HEAD 与构建所用 commit 一致（`git rev-parse HEAD`）
9. [ ] **以上全部通过后**才打 tag：`git tag vX.Y.Z.W && git push origin vX.Y.Z.W`
10. [ ] `gh release create` 上传产物，核对资产列表与上一个 Release 一致
11. [ ] tag 推送后更新 AUR（先 `git ls-remote` 确认 tag、`makepkg -s` 预演，再 push）
