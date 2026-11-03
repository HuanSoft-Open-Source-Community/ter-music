/**
 * @file layout_spec.h
 * @brief TUI 主界面布局分栏树规范（纯内存模型 + JSON，无 ncurses 依赖）
 *
 * 布局用二叉分栏树描述：split 节点按 row（左右并排）或 col（上下堆叠）
 * 切开自己的矩形，ratio 是第一个子节点占该节点跨度的千分比。叶子直接
 * 引用窗格种类。这样“平铺”是数据模型的结构性不变量：只要树合法，解析
 * 出来的矩形必然无缝隙、无重叠、面积恰好 1000×1000。
 *
 * 旧版 v1 布局用自由矩形（panes[].x/y/w/h）描述；解析层会把它们转换成
 * 分栏树。无法用 guillotine 切割表达的旧布局按阅读顺序重排，并标记
 * LayoutSpec.normalized。布局不再因几何问题被拒绝。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_LAYOUT_SPEC_H
#define UI_LAYOUT_SPEC_H

#include <stddef.h>

#define LAYOUT_MAX_CATALOG   24
#define LAYOUT_MAX_PANES     6
#define LAYOUT_MAX_NODES     (2 * LAYOUT_MAX_PANES - 1)   /* 11 */
#define LAYOUT_ID_MAX        32
#define LAYOUT_NAME_MAX      64
#define LAYOUT_KEY_MAX       48
#define LAYOUT_AREA_UNITS    1000
#define LAYOUT_AREA_TOTAL    (LAYOUT_AREA_UNITS * LAYOUT_AREA_UNITS)
#define LAYOUT_COLOR_INHERIT (-2)
#define LAYOUT_MIN_PANE_UNITS 10   /* 编辑器允许的最小窗格轴长（1%） */

/* 窗格颜色字段编号：0 = 前景，1 = 背景（与边框字段区分开）。 */
#define LAYOUT_COLOUR_FG 0
#define LAYOUT_COLOUR_BG 1

/**
 * @brief 把调色板值降到目标终端能显示的色数（256 → 16 → 8 → 无色）。
 *
 * 纯函数，不依赖 ncurses，便于单测与预设配色自检：
 * - `value < 0`（透明/继承）→ `-1`；`value > 255` 先截到 255；
 * - `colors >= 256` 原样返回；`value < colors` 也原样返回；
 * - 否则按 xterm 调色板取 RGB，映射到最近的 8 或 16 基色；
 * - `colors <= 0`（黑白终端）→ `-1`，即不启用颜色。
 */
int layout_color_quantize(int value, int colors);

/* 布局内的窗格种类；顺序必须与运行时的窗口数组一致。 */
typedef enum {
    LAYOUT_PANE_PLAYLIST = 0,
    LAYOUT_PANE_INFO,
    LAYOUT_PANE_CONTROLS,
    LAYOUT_PANE_SPECTRUM,
    LAYOUT_PANE_LYRICS,
    LAYOUT_PANE_VINYL,
    LAYOUT_PANE_COUNT
} LayoutPaneKind;

/* 分栏方向：ROW = 左右并排；COL = 上下堆叠。 */
typedef enum {
    LAYOUT_SPLIT_ROW = 0,
    LAYOUT_SPLIT_COL
} LayoutSplitDir;

/**
 * 窗格配置，按 LayoutPaneKind 下标存放。
 * enabled=1 表示该种类在分栏树中；颜色在窗格被删除后仍保留，重新加入时复用。
 */
typedef struct {
    int enabled;
    int fg, bg;                 /* -2 继承主题；-1 终端默认；0..255 调色板 */
    int border_fg, border_bg;
} LayoutPaneSpec;

typedef struct {
    int is_leaf;
    LayoutPaneKind kind;        /* leaf */
    LayoutSplitDir dir;         /* split */
    int ratio;                  /* split: 第一个子节点的跨度千分比, 1..999 */
    int child[2];               /* split: 子节点下标 */
} LayoutNode;

typedef struct {
    char id[LAYOUT_ID_MAX];
    char name[LAYOUT_NAME_MAX];
    char name_key[LAYOUT_KEY_MAX];
    int pane_count;
    LayoutPaneSpec panes[LAYOUT_PANE_COUNT];
    int root;                   /* 分栏树根节点；-1 = 空 */
    int node_count;
    LayoutNode nodes[LAYOUT_MAX_NODES];
    int normalized;             /* 载入时被自动适配（几何不再与旧文件一致） */
} LayoutSpec;

typedef struct {
    LayoutPaneKind kind;
    int x, y, w, h;             /* 千分比 */
} LayoutResolvedPane;

typedef struct {
    int pane_count;
    LayoutResolvedPane panes[LAYOUT_MAX_PANES];
} LayoutResolved;

typedef struct {
    int count;
    int adjusted;               /* 本次解析中自动适配过的布局数（仅供日志/提示） */
    int skipped;                /* 结构不可读取而跳过的布局数 */
    char default_id[LAYOUT_ID_MAX];
    LayoutSpec layouts[LAYOUT_MAX_CATALOG];
} LayoutCatalog;

void layout_catalog_init(LayoutCatalog *catalog);

/** @brief 初始化一份空布局（颜色继承、根为 -1）。 */
void layout_spec_init(LayoutSpec *spec);

/**
 * @brief 解析一份布局 JSON 文本。
 *
 * 几何不再作为拒绝条件：v2 分栏树优先；没有 tree 时尝试 v1 矩形，并自动
 * 适配为强制平铺；仍然无法解释时使用 classic 兜底并保留 id/name。
 * 只有根 JSON/布局数组不可读、或所有布局都缺少合法 id 时才返回 -1。
 * err 中给出首个结构错误或自动适配说明。
 */
int layout_spec_parse(const char *json, size_t len, LayoutCatalog *out,
                      char *err, size_t err_size);

int layout_spec_parse_file(const char *path, LayoutCatalog *out,
                           char *err, size_t err_size);

/**
 * @brief 把目录序列化为 v2 布局 JSON 文本。
 *
 * 输出形如 {"version":2,"default":"..","layouts":[..]}。每个布局同时写出
 * tree（权威）和 panes 矩形镜像（供旧版本程序读取）。未显式指定的颜色
 * （继承 -2）不输出。
 *
 * @return malloc 分配的 JSON 文本（调用方负责 free）；参数非法或
 *         内存不足/超出缓冲上限时返回 NULL。
 */
char *layout_spec_serialize(const LayoutCatalog *catalog);

/**
 * @brief 把目录序列化并写入 JSON 文件（临时文件 + 原子 rename）。
 * @return 0 成功；-1 失败（err 给出原因）。
 */
int layout_spec_write_file(const char *path, const LayoutCatalog *catalog,
                           char *err, size_t err_size);

/**
 * @brief 在用户目录内新增或覆盖一份布局（同 id 覆盖）。
 * @return 操作后的目录布局数；参数非法或目录已满（LAYOUT_MAX_CATALOG）
 *         返回 -1，调用方应提示用户。
 */
int layout_user_catalog_upsert(LayoutCatalog *user, const LayoutSpec *layout);

/**
 * @brief 从用户目录删除一份布局（id 不存在时无动作）。
 * @return 操作后的目录布局数；参数非法返回 -1。
 */
int layout_user_catalog_remove(LayoutCatalog *user, const char *id);

/**
 * @brief 把 src 合并进 dst：同 id 覆盖，新 id 追加。
 * @return 合并后 dst 的布局数；超过 LAYOUT_MAX_CATALOG 的条目被忽略。
 */
int layout_catalog_merge(LayoutCatalog *dst, const LayoutCatalog *src);

/**
 * @brief 把分栏树展开为千分比矩形。
 *
 * 每个 split 的两个子节点共享同一个切点，因此输出矩形严格平铺。
 * @return 0 成功；-1 参数错误或树无法展开（正常解析/编辑不会出现）。
 */
int layout_spec_resolve(const LayoutSpec *in, LayoutResolved *out);

/**
 * @brief 防御性严格平铺检查。
 *
 * 规则：至少一个可见窗格；每个窗格在 0..1000 内且 w/h > 0；可见窗格两两
 * 不重叠；面积和 == 1,000,000；树中每个叶子都必须 enabled 且只出现
 * 一次；enabled 的窗格必须都出现在树中；lyrics 与 vinyl 不同时启用。
 * 解析层不再用它拒绝布局，只用于运行时兜底、保存前内部检查和测试。
 */
int layout_spec_validate(const LayoutSpec *spec, char *err, size_t err_size);

/**
 * @brief 生成内存版 classic 兜底布局（本身满足严格平铺）。
 * @return 0 成功，-1 参数错误。
 */
int layout_spec_build_fallback(LayoutSpec *out);

const LayoutSpec *layout_catalog_find(const LayoutCatalog *catalog, const char *id);

/**
 * @brief 给"丢了本地化名字键"的同名覆盖补回预设的 name_key。
 *
 * 旧版布局编辑器打开预设时会无条件清掉 name_key，保存后用户文件里就只剩
 * 英文/原文名字。这里在合并前自愈：仅当 incoming 项 name_key 为空、existing
 * 同 id 有 name_key、且名字为空或与预设相同（即没被用户改名）时才补。
 */
void layout_catalog_adopt_preset_names(LayoutCatalog *incoming,
                                       const LayoutCatalog *existing);

/**
 * @brief 为派生副本挑一个未被占用的 id：`base-2`、`base-3` … `base-99`。
 *
 * 只在"同 id 覆盖写不回去"时用（例如该 id 由 TER_MUSIC_LAYOUTS 提供，
 * 合并优先级高于用户文件）。
 * @return 0 成功；-1 参数非法、id 放不下或 2..99 全被占用。
 */
int layout_catalog_next_copy_id(const LayoutCatalog *catalog, const char *base_id,
                                char *out, size_t out_size);

const char *layout_pane_id_name(LayoutPaneKind kind);
int layout_pane_kind_from_name(const char *name, LayoutPaneKind *kind);

/* ============================================================
 * 分栏树编辑操作（布局编辑器与单测使用）
 *
 * 全部采用事务式语义：失败时原 LayoutSpec 不变。
 * ============================================================ */

/** @brief 树中启用的窗格数。 */
int layout_spec_pane_count(const LayoutSpec *spec);

/** @brief 按树序（先左后右）取第 index 个窗格；越界返回 -1。 */
int layout_spec_pane_at(const LayoutSpec *spec, int index);

/** @brief 查找窗格叶子节点下标；找不到返回 -1。 */
int layout_spec_find_pane(const LayoutSpec *spec, LayoutPaneKind kind);

/** @brief 取窗格颜色配置；种类未启用时返回 NULL。 */
const LayoutPaneSpec *layout_spec_pane_config(const LayoutSpec *spec,
                                              LayoutPaneKind kind);

/**
 * @brief 取窗格颜色配置，**不做** enabled 过滤。
 *
 * 颜色在窗格被移出分栏树后仍留在 panes[] 里（见 LayoutPaneSpec 注释），
 * 设置页要按窗格种类成对显示前景/背景，因此需要一个能读到禁用窗格的入口。
 * @return 窗格配置指针；kind 越界返回 NULL。
 */
const LayoutPaneSpec *layout_spec_pane_colour(const LayoutSpec *spec,
                                              LayoutPaneKind kind);

/**
 * @brief 解析窗格某字段的生效颜色。
 *
 * 语义与 `ui_layout_apply_colors()` 一致：布局里写了显式色就用布局的，
 * `LAYOUT_COLOR_INHERIT` 才回落到调用方给出的 `base`（主题角色色）。
 * @param field       LAYOUT_COLOUR_FG / LAYOUT_COLOUR_BG。
 * @param base        继承时使用的基准色（-1 表示终端默认）。
 * @param from_layout 可为 NULL；非空时写入 1 表示取自布局覆盖，0 表示继承。
 * @return 生效颜色；参数非法时返回 `base`。
 */
int layout_spec_colour_effective(const LayoutSpec *spec, LayoutPaneKind kind,
                                 int field, int base, int *from_layout);

/**
 * @brief 双格布局 + 当前曲目无歌词 → 歌词格是否应当让位。
 *
 * 只做判定，不改动 spec；调用方（ui/layout.c）据此在解析前派生一份"去掉
 * 歌词格"的有效规格。逐曲目重算，所以曲目有歌词时返回 0（两格都画）。
 * @return 1 让位；0 不需要（布局不足两格，或曲目有歌词）。
 */
int layout_spec_lyrics_pane_suppressed(const LayoutSpec *spec,
                                      int lyrics_available);

/**
 * @brief 判断某个种类是否可以加入当前布局。
 * @return 1 可用；0 已存在或参数非法。
 */
int layout_spec_pane_available(const LayoutSpec *spec, LayoutPaneKind kind);

/**
 * @brief 把 target 叶子一分为二，新窗格在右侧（ROW）或下方（COL）。
 * @return 0 成功；-1 类型冲突、节点/窗格已满或结果窗格过小。
 */
int layout_spec_split_pane(LayoutSpec *spec, LayoutPaneKind target,
                           LayoutSplitDir dir, LayoutPaneKind new_kind);

/** @brief 删除一个窗格，由兄弟子树接管区域；根窗格不能删除。 */
int layout_spec_remove_pane(LayoutSpec *spec, LayoutPaneKind kind);

/**
 * @brief 把 old_kind 叶子改成 new_kind。
 *
 * new_kind 必须未使用（lyrics 与 vinyl 现在可以同时存在，不再互相替换）。
 */
int layout_spec_change_pane_kind(LayoutSpec *spec, LayoutPaneKind old_kind,
                                 LayoutPaneKind new_kind);

/**
 * @brief 调整"最近的该方向祖先分栏"的比例，使选中窗格变大或变小。
 *
 * @param dir LAYOUT_SPLIT_ROW 调宽度（左右分栏），LAYOUT_SPLIT_COL 调高度
 *            （上下分栏）；从选中叶子向上找第一个该方向的分栏，因此同一窗格
 *            两个方向都能调。
 *
 * 整步会撞上"某格小于 1%"时按可用步长饱和（先试整步，再折半到 1 个单位），
 * 所以界面上的比例会一直贴着极限走，而不是提前拒绝。
 * @return 0 成功；-1 参数非法或校验失败；-2 该方向没有祖先分栏；
 *         -3 连 1 个单位都放不下（已到该方向的极限）。
 */
int layout_spec_adjust_ratio(LayoutSpec *spec, LayoutPaneKind kind,
                             LayoutSplitDir dir, int delta);

/** @brief 把选中窗格父分栏恢复为 50/50。 */
int layout_spec_equalize_pane(LayoutSpec *spec, LayoutPaneKind kind);

/** @brief 交换两个窗格叶子的种类（几何随种类移动）。 */
int layout_spec_swap_panes(LayoutSpec *spec, LayoutPaneKind a,
                           LayoutPaneKind b);

#endif /* UI_LAYOUT_SPEC_H */
