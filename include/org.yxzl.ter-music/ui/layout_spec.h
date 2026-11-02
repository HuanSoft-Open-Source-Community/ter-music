/**
 * @file layout_spec.h
 * @brief TUI 主界面布局 JSON 规范（纯解析/校验，无 ncurses 依赖）
 *
 * 布局用千分比矩形描述：主区域宽高各映射为 1000 个单位。所有可见窗格
 * 必须严格平铺主区域：两两不重叠，且面积和恰好为 1,000,000。这样既
 * 禁止空白，也禁止重叠，且与像素取整解耦。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef UI_LAYOUT_SPEC_H
#define UI_LAYOUT_SPEC_H

#include <stddef.h>

#define LAYOUT_MAX_CATALOG   24
#define LAYOUT_MAX_PANES     6
#define LAYOUT_ID_MAX        32
#define LAYOUT_NAME_MAX      64
#define LAYOUT_KEY_MAX       48
#define LAYOUT_AREA_UNITS    1000
#define LAYOUT_AREA_TOTAL    (LAYOUT_AREA_UNITS * LAYOUT_AREA_UNITS)
#define LAYOUT_COLOR_INHERIT (-2)

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

typedef struct {
    LayoutPaneKind kind;
    int visible;                /* 0/1；默认 1 */
    int x, y, w, h;             /* 千分比，0..1000；w/h > 0 */
    int fg, bg;                 /* -2 继承主题；-1 终端默认；0..255 调色板 */
    int border_fg, border_bg;
} LayoutPaneSpec;

typedef struct {
    char id[LAYOUT_ID_MAX];
    char name[LAYOUT_NAME_MAX];
    char name_key[LAYOUT_KEY_MAX];
    int pane_count;
    LayoutPaneSpec panes[LAYOUT_MAX_PANES];
} LayoutSpec;

typedef struct {
    int count;
    int rejected;               /* 本次解析被拒绝的布局数（仅供日志） */
    char default_id[LAYOUT_ID_MAX];
    LayoutSpec layouts[LAYOUT_MAX_CATALOG];
} LayoutCatalog;

void layout_catalog_init(LayoutCatalog *catalog);

/**
 * @brief 解析一份布局 JSON 文本。
 *
 * 单个布局非法时跳过并累加 out->rejected；若没有任何合法布局则返回 -1。
 * 返回 0 表示成功且 out 中至少有一个布局。err 中给出首个错误/拒绝原因。
 */
int layout_spec_parse(const char *json, size_t len, LayoutCatalog *out,
                      char *err, size_t err_size);

int layout_spec_parse_file(const char *path, LayoutCatalog *out,
                           char *err, size_t err_size);

/**
 * @brief 把目录序列化为布局 JSON 文本。
 *
 * 输出形如 {"version":1,"default":"..","layouts":[..]}；与解析层互逆
 * （serialize → parse 得到等价目录）。未显式指定的颜色（继承 -2）与
 * 可见窗格（visible=1）不输出，保持与内置预设文件相同的精简风格。
 *
 * @return malloc 分配的 JSON 文本（调用方负责 free）；参数非法或
 *         内存不足/超出缓冲上限时返回 NULL。
 */
char *layout_spec_serialize(const LayoutCatalog *catalog);

/**
 * @brief 把目录序列化并写入 JSON 文件。
 *
 * 先写同名临时文件再原子改名，避免中途崩溃留下半截文件。
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
 * @brief 执行外观回退：vinyl 可见时隐藏 lyrics。
 *
 * 只改可见性，不改几何；输出可安全用于严格平铺校验与建窗。
 */
void layout_spec_resolve(const LayoutSpec *in, LayoutSpec *out);

/**
 * @brief 严格平铺校验。
 *
 * 规则：至少一个可见窗格；可见窗格两两不重叠；面积和 == 1,000,000。
 */
int layout_spec_validate(const LayoutSpec *spec, char *err, size_t err_size);

/**
 * @brief 生成内存版 classic 兜底布局（本身满足严格平铺）。
 * @return 0 成功，-1 参数错误。
 */
int layout_spec_build_fallback(LayoutSpec *out);

const LayoutSpec *layout_catalog_find(const LayoutCatalog *catalog, const char *id);

const char *layout_pane_id_name(LayoutPaneKind kind);
int layout_pane_kind_from_name(const char *name, LayoutPaneKind *kind);

#endif /* UI_LAYOUT_SPEC_H */
