/**
 * @file info_bar_layout.h
 * @brief 主界面信息窗格的列/块分区几何（纯计算，无 ncurses 依赖）
 *
 * 信息窗格不再固定为“左/中/封面”三栏，而是按窗格宽高自适应排布：
 *
 *   - 横向（`stacked == 0`）：元数据在左、音频在右；空间足够时封面贴在
 *     右侧，与文字之间留一列空白，与右边框之间至少留两列空白；
 *   - 纵向（`stacked == 1`）：封面在上（可选），元数据与音频依次在下，
 *     两段都占满内容宽度。
 *
 * 选择规则（常量见下）：
 *   - 内容宽 `cw >= INFO_BAR_VERTICAL_WIDTH`（够宽）→ 横向；
 *   - 或两列放得下（`cw >= INFO_BAR_COLUMNS_MIN_WIDTH`）但高度不足以
 *     放下 5+4 行堆叠（`ch < INFO_BAR_VERTICAL_MIN_ROWS`）→ 横向；
 *   - 其余（窄而高，或窄到放不下两列）→ 纵向堆叠/单列，按行裁剪。
 *
 * 硬性不变量（info_bar_layout_validate() 逐条校验）：
 *   - 内容区为 `x∈[1,w-2]`、`y∈[1,h-2]`，第 0 行是标题，w-1/h-1 是边框；
 *   - 文字/封面块从 `1 + INFO_BAR_PAD_LEFT` 开始，避免紧贴左边框；
 *   - 三个矩形两两不重叠；
 *   - 封面隐藏时 w/h 同时为 0；显示时 `w == 2*h`、`5 <= h <= 12`，
 *     且 `cover.x + cover.w <= w - 2`（右侧至少空一列再是右边框）；
 *   - 元数据/音频矩形完整落在内容区内。
 *
 * 渲染层必须只使用本结构给出的矩形与行数；validate 失败时不得绘制内容。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_INFO_BAR_LAYOUT_H
#define UI_INFO_BAR_LAYOUT_H

/* 信息窗格固定字段行数：5 行元数据（状态/模式/标题/艺术家/专辑或队列）、
 * 4 行音频参数（采样率/位深/比特率/编码）。 */
#define INFO_BAR_METADATA_LINES 5
#define INFO_BAR_AUDIO_LINES    4

/* 横向两列各自至少需要的列数；有封面时文字区至少需要的列数。 */
#define INFO_BAR_MIN_COL_WIDTH      20
#define INFO_BAR_MIN_TEXT_WITH_COVER 48

/* 内容区左侧留一列空白，避免文字紧贴左边框；右侧留白由封面规则保证。 */
#define INFO_BAR_PAD_LEFT 1

/* 封面的行数上下限：横向封面末列固定为 w-4；纵向封面居中并留边。 */
#define INFO_BAR_COVER_MIN_ROWS 5
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

/* 纵向封面至少还要 9 行文字、1 行间隔、5 行封面。 */
#define INFO_BAR_VERTICAL_COVER_MIN_ROWS \
    (INFO_BAR_VERTICAL_MIN_ROWS + INFO_BAR_COVER_GAP + INFO_BAR_COVER_MIN_ROWS)

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

    int metadata_rows;      /* 实际可绘制的元数据行数（1..5） */
    int audio_rows;         /* 实际可绘制的音频行数（0..4） */
} InfoBarLayout;

/**
 * @brief 计算信息窗格的自适应分区。
 *
 * @param window_width  窗格总列数（含左右边框）
 * @param window_height 窗格总行数（含上下边框与标题行）
 * @param want_cover    1 = 有封面可画；0 = 不预留封面空间
 * @param out           输出分区；函数会先整体清零
 *
 * 无论 want_cover 为何，空间不足时封面都会被隐藏，文字区相应加宽。
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
