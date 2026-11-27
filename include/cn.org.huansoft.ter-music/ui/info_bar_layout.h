/**
 * @file info_bar_layout.h
 * @brief 主界面信息窗格的列/块分区几何（纯计算，无 ncurses 依赖）
 *
 * 信息窗格不再固定为“左/中/封面”三栏，而是按窗格宽高与调用方给出的
 * 元数据/音频行数自适应排布：
 *
 *   - 横向（`stacked == 0`）：元数据在左、音频在右，宽度按两块实际文本
 *     宽度动态分配（各自不低于 `INFO_BAR_MIN_COL_WIDTH`）；空间足够时封面
 *     贴在右侧，与文字之间留一列空白，与右边框之间至少留两列空白；
 *   - 纵向（`stacked == 1`）：封面在上（可选），元数据与音频依次在下，
 *     两段都占满内容宽度；
 *   - 文本块在上/下、左/右两种排布中都可在**块内**分成 1..3 个子列
 *     （column-major 流动）：宽而矮时把 5+4 行铺到多列里，尽量全部可见；
 *   - 高度不足时按调用方给的 per-line priority 裁剪：保留优先级数值最小的
 *     行，低优先级先让位；块内行仍按原始显示顺序紧凑排列（不出现半截行）。
 *
 * 选择规则（常量见下）：
 *   - 内容宽 `cw >= INFO_BAR_VERTICAL_WIDTH`（够宽）→ 横向；
 *   - 或两列放得下（`cw >= INFO_BAR_COLUMNS_MIN_WIDTH`）但高度不足以
 *     放下 5+4 行堆叠（`ch < INFO_BAR_VERTICAL_MIN_ROWS`）→ 横向；
 *   - 其余（窄而高，或窄到放不下两列）→ 纵向堆叠/单列，按行裁剪。
 *
 * 行数可以是 0：例如用户关闭全部可显示字段时，信息窗格只画封面/边框。
 *
 * 硬性不变量（info_bar_layout_validate() 逐条校验）：
 *   - 内容区为 `x∈[1,w-2]`、`y∈[1,h-2]`，第 0 行是标题，w-1/h-1 是边框；
 *   - 文字/封面块从 `1 + INFO_BAR_PAD_LEFT` 开始，避免紧贴左边框；
 *   - 有文本时三个矩形两两不重叠；某一块行数为 0 时其矩形必须为空；
 *   - 封面隐藏时 w/h 同时为 0；显示时 `w == 2*h`、`3 <= h <= 12`，
 *     且 `cover.x + cover.w <= w - 2`（右侧至少空一列再是右边框）；
 *   - 元数据/音频矩形完整落在内容区内；有文本行时每块总宽不低于
 *     `INFO_BAR_MIN_COL_WIDTH`（有封面时文字区总宽不低于
 *     `INFO_BAR_MIN_TEXT_WITH_COVER`），块内每个子列不低于
 *     `INFO_BAR_MIN_TEXT_COL_WIDTH`；
 *   - 文本行数可以大于区域可视行数（多子列流动），但必须满足
 *     `ceil(rows / cols) <= region.h`；keep_mask 的 popcount 必须等于 rows。
 *
 * 渲染层必须只使用本结构给出的矩形与行数；validate 失败时不得绘制内容。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_INFO_BAR_LAYOUT_H
#define UI_INFO_BAR_LAYOUT_H

/* 信息窗格元数据行数上限（状态/模式/标题/艺术家/专辑或队列）与音频行数。 */
#define INFO_BAR_METADATA_LINES 5
#define INFO_BAR_AUDIO_LINES    4

/* 横向两列各自至少需要的列数；有封面时文字区至少需要的列数。 */
#define INFO_BAR_MIN_COL_WIDTH      20
#define INFO_BAR_MIN_TEXT_WITH_COVER 48

/* 文字块内部可流动的子列数上限与每个子列的最小列数。宽而矮时按
 * column-major 把行铺到多列，只有确实放不下时才按 priority 裁剪。 */
#define INFO_BAR_FLOW_MAX_COLS       3
#define INFO_BAR_MIN_TEXT_COL_WIDTH  20
#define INFO_BAR_TEXT_COL_GAP        1

/* 块内多列流动的启用条件：可视行数至少为 2（1 行时分列只会更难读）。
 * 该常量被测试直接引用，改动即视为布局规则变更。 */
#define INFO_BAR_FLOW_MIN_ROWS       2

/* cover 优先级（preferences.info_cover_priority）：
 *   0=auto：先缩小封面以保留文字的理想宽度，实在放不下才隐藏封面；
 *   1=text-first：文字理想宽度优先，封面只会更小或隐藏；
 *   2=cover-first：封面优先取最大（文字仍不低于最小可用宽度）。 */
#define INFO_COVER_PRIORITY_AUTO        0
#define INFO_COVER_PRIORITY_TEXT_FIRST  1
#define INFO_COVER_PRIORITY_COVER_FIRST 2
#define INFO_COVER_PRIORITY_COUNT       3

/* 内容区左侧留一列空白，避免文字紧贴左边框；右侧留白由封面规则保证。 */
#define INFO_BAR_PAD_LEFT 1

/* 封面的行数上下限：横向封面末列固定为 w-4；纵向封面居中并留边。
 * 最小 3 行（6 列）仍可由 cover_art 的半块网格渲染；空间不足时先缩到
 * 3 行，再按 cover 优先级决定是否隐藏。 */
#define INFO_BAR_COVER_MIN_ROWS 3
#define INFO_BAR_COVER_MAX_ROWS 12

/* 文字与封面之间、封面与右边框之间必须保留的空白列数。 */
#define INFO_BAR_COVER_GAP       1
#define INFO_BAR_COVER_RIGHT_PAD 2

/* 横向模式的最小内容宽：两列 + 一列 gutter。 */
#define INFO_BAR_COLUMNS_MIN_WIDTH (2 * INFO_BAR_MIN_COL_WIDTH + 1)

/* 内容宽低于此值且够高时改用纵向堆叠。 */
#define INFO_BAR_VERTICAL_WIDTH 64

/* 纵向模式把 5 行元数据 + 4 行音频全部放下才值得切换。 */
#define INFO_BAR_VERTICAL_MIN_ROWS (INFO_BAR_METADATA_LINES + INFO_BAR_AUDIO_LINES)

typedef struct {
    int x, y;           /* 窗格内坐标（0 基，1 = 内容区首行/首列） */
    int w, h;           /* 可用列/行数；0 表示该块不存在 */
} InfoBarRect;

typedef struct {
    int valid;          /* 1 = 分区可用；0 = 窗口过小，不应绘制内容 */
    int stacked;        /* 0 = 横向两列；1 = 纵向堆叠 */

    InfoBarRect metadata;   /* 状态/模式/标题/艺术家/专辑（或队列行） */
    InfoBarRect audio;      /* 采样率/位深/比特率/编码 */
    InfoBarRect cover;      /* 字符封面；w==0 && h==0 表示隐藏 */

    int metadata_rows;      /* 实际保留并绘制的元数据行数（0..5） */
    int audio_rows;         /* 实际保留并绘制的音频行数（0..4） */
    int metadata_cols;      /* 元数据块内子列数（1..INFO_BAR_FLOW_MAX_COLS；0 行时为 0） */
    int audio_cols;         /* 音频块内子列数（同上） */
    unsigned metadata_keep_mask; /* bit i = 原始第 i 行被保留（按显示顺序） */
    unsigned audio_keep_mask;    /* 同上，音频行 */
} InfoBarLayout;

/**
 * @brief 计算信息窗格的自适应分区（按调用方给出的行数）。
 *
 * @param window_width  窗格总列数（含左右边框）
 * @param window_height 窗格总行数（含上下边框与标题行）
 * @param want_cover    1 = 有封面可画；0 = 不预留封面空间
 * @param metadata_rows 期望的元数据行数（0..5，超出钳制）
 * @param audio_rows    期望的音频行数（0..4，超出钳制）
 * @param out           输出分区；函数会先整体清零
 *
 * 无论 want_cover 为何，空间不足时封面都会被隐藏，文字区相应加宽。
 */
void info_bar_layout_compute_rows(int window_width,
                                  int window_height,
                                  int want_cover,
                                  int metadata_rows,
                                  int audio_rows,
                                  InfoBarLayout *out);

/**
 * @brief 自适应布局（完整参数）：动态列宽、cover 优先级、高度优先级裁剪、
 *        块内多列流动。
 *
 * @param window_width   窗格总列数（含边框）
 * @param window_height  窗格总行数（含边框与标题）
 * @param want_cover     1 = 有封面可画
 * @param cover_priority INFO_COVER_PRIORITY_*（超出范围按 auto）
 * @param metadata_rows  请求的元数据行数（0..5，超出钳制）
 * @param audio_rows     请求的音频行数（0..4，超出钳制）
 * @param metadata_priority 长度 >= metadata_rows 的优先级数组；NULL = 按行号
 *                      （0,1,2...）作为优先级。数值越小越先保留。
 * @param audio_priority 同上，长度 >= audio_rows。
 * @param metadata_desired_w 元数据块内容的最大显示列宽（0 = 未知）
 * @param audio_desired_w    音频块内容的最大显示列宽（0 = 未知）
 * @param out            输出；函数先整体清零
 *
 * 宽度分配：横向时按两块 desired width 做带最小宽度的比例分配；某块没有
 * 行时把全部文字宽度给另一块。cover 是否显示及大小同时受 cover_priority、
 * 文字最小宽度、封面最小/最大行数约束。
 */
void info_bar_layout_compute_adaptive(int window_width, int window_height,
                                      int want_cover, int cover_priority,
                                      int metadata_rows, int audio_rows,
                                      const int *metadata_priority,
                                      const int *audio_priority,
                                      int metadata_desired_w,
                                      int audio_desired_w,
                                      InfoBarLayout *out);

/**
 * @brief 块内文本单元几何：把第 line 行（0 基，按保留后的紧凑序号）映射到
 *        子列/行并返回该单元格的窗口坐标与宽度。
 *
 * 渲染层与布局共用同一套取整规则，避免两边算出不同的列宽。
 *
 * @param region 文本块矩形
 * @param cols   子列数（>=1）
 * @param line   已保留行中的序号（0..lines-1）
 * @param lines  已保留行总数（用于确定视觉行数）
 * @param x,y,w  输出单元左上角与宽度
 * @return 1 成功，0 参数非法
 */
int info_bar_layout_text_cell(const InfoBarRect *region, int cols,
                              int line, int lines,
                              int *x, int *y, int *w);

/**
 * @brief 兼容包装：固定请求 5 行元数据 + 4 行音频，auto 封面优先级。
 *        不提供内容宽度（按最小宽度均衡分配）。
 *
 * 等价于 info_bar_layout_compute_rows(window_width, window_height, want_cover,
 *                                    INFO_BAR_METADATA_LINES,
 *                                    INFO_BAR_AUDIO_LINES, out)。
 */
void info_bar_layout_compute(int window_width,
                             int window_height,
                             int want_cover,
                             InfoBarLayout *out);

/**
 * @brief 校验分区是否满足本文件的全部硬性不变量。
 *
 * 渲染层在绘制前调用；返回 0 时只应绘制背景/边框，不得输出内容。
 *
 * @return 1 = 合法，0 = 非法
 */
int info_bar_layout_validate(int window_width,
                             int window_height,
                             const InfoBarLayout *layout);

#endif /* UI_INFO_BAR_LAYOUT_H */
