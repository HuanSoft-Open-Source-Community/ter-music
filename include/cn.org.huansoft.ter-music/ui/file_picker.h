/**
 * @file file_picker.h
 * @brief 文件/目录选择框（桌面对话框优先，纯 TTY 回退手输路径）
 *
 * 用途：把界面上所有"让用户给一个路径"的入口统一到一处，模块本身不关心
 * 路径拿来做什么。当前调用方（都在 ui/ 下）：
 *
 *   - ui/dialog.c       打开文件夹 / 追加文件夹        → file_picker_pick_folder()
 *   - ui/settings.c     默认音乐目录（默认启动路径）   → file_picker_pick_folder()
 *   - ui/language_view.c 添加语言包（.tar.gz 文件）    → file_picker_pick_file()
 *
 * 模块保持通用：标题/过滤器/初始目录都由调用方给；选中的目录是否可用于
 * 播放、语言包是否合法，仍由调用方原有的校验/加载路径决定。
 *
 * 后端与优先级（`TERM_MUSIC_FILE_CHOOSER` 可强制，便于测试/脚本）：
 *
 *   portal / zenity / kdialog   桌面对话框（见下面的"portal 说明"）
 *   tty                         纯终端：ui/dialog.c 的 prompt_text_input 手输路径
 *   auto（默认，含未知值）       有桌面对话框就用它，否则回退 tty
 *
 * 桌面对话框"可用"= 可执行文件在 PATH 里，且存在图形会话（`DISPLAY` 或
 * `WAYLAND_DISPLAY` 非空）。只有一半条件时不硬上：auto 直接回退 tty，
 * 强制值则报可读的"后端不可用"错误——headless/SSH 下不会卡死在等窗口。
 *
 * **portal 说明（刻意如此，接口已预留）**：本模块不自己实现
 * `org.freedesktop.portal.FileChooser` 的 D-Bus 调用，而是把 `portal` 当作
 * "优先用桌面门户式对话框"的**偏好值**，由 zenity（GTK）/kdialog（Qt）这类
 * 本身就走 xdg-desktop-portal 的对话框承接。原因：在沙箱（Linyaps/Flatpak）
 * 里这两个工具会自动改走门户，行为与直接调 FileChooser 一致；而直接实现
 * D-Bus 门户需要一个常驻会话、请求句柄与 Response 信号等待，属于另一套
 * 独立的生命周期，收益不抵风险（无桌面环境时会挂住 TUI）。枚举值与
 * `TERM_MUSIC_FILE_CHOOSER=portal` 已经保留：将来要接原生门户，只需在
 * file_picker.c 里补一个 picker_pick_portal() 并改 backend_available()，
 * 调用方（ui/settings.c）与配置/文案都不用动。
 *
 * 约定：
 *  - 成败用返回码表达（OK / CANCEL / ERROR），错误原因写 err；
 *  - 选中的路径原样返回（UTF-8，可含空格）；`~/` 在 TTY 后端输入时展开，
 *    目录选择另在入口处统一展开一次（外部对话框若回传 `~/x` 也能用）；
 *  - **目录选择**（file_picker_pick_folder）在返回 OK 之前会 stat 校验：
 *    不存在/不是目录一律 ERROR（err 写可读原因，out 置空），调用方据此
 *    拒绝写入配置或加载播放列表。**文件选择**（file_picker_pick_file）保持
 *    既有语义：只负责拿到路径，文件是否合法由调用方判断；
 *  - **外部对话框打开期间不退出 curses 备用屏幕**：zenity/kdialog 都是 GUI
 *    进程，不读 curses 的终端输入、也不写 stdout（路径走管道），所以不需要
 *    `endwin()`。旧实现 `def_prog_mode()+endwin()` 会发 rmcup 退出备用屏幕，
 *    对话框背后随即露出启动 TUI 前的 shell 内容（t8 用户报障的"闪屏"）。
 *    现在打开前/返回后只做一次 `refresh()` 补差异，不 `clearok`（不整屏闪）；
 *    TTY 后端本来就在 curses 内手输，由 prompt_text_input 自己刷新；
 *  - 子进程的 stdin/stdout/stderr 全部改道：stdout 是**唯一**的结果通道
 *    （一行路径），stderr 丢 /dev/null（绝不允许 GTK/kdialog 告警画到
 *    curses 屏幕上），stdin 接 /dev/null（不偷 TUI 的按键）；
 *  - 纯 TTY 后端把空输入当"取消"（Enter 直接过），不会写出半截路径；
 *  - 不 fork 常驻进程、不注册信号处理器、失败不影响主循环。
 *
 * @author ter-music team
 * @date 2026-11-25
 */

#ifndef UI_FILE_PICKER_H
#define UI_FILE_PICKER_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 强制后端的环境变量：portal / zenity / kdialog / tty（大小写不敏感）。 */
#define FILE_PICKER_ENV "TERM_MUSIC_FILE_CHOOSER"

/** 后端取值。AUTO = 按可用性挑选（见 file_picker_resolve_backend）。 */
enum {
    FILE_PICKER_BACKEND_AUTO     = 0,
    FILE_PICKER_BACKEND_PORTAL   = 1,
    FILE_PICKER_BACKEND_ZENITY   = 2,
    FILE_PICKER_BACKEND_KDIALOG  = 3,
    FILE_PICKER_BACKEND_TTY      = 4
};

/** 返回码。 */
enum {
    FILE_PICKER_OK     = 0,  /**< 选中并写入路径（out 非空） */
    FILE_PICKER_CANCEL = 1,  /**< 用户取消（对话框取消 / TTY 空输入） */
    FILE_PICKER_ERROR  = 2   /**< 后端不可用或执行失败（err 写可读原因） */
};

/**
 * @brief 纯逻辑：把环境变量值解析成后端枚举。
 *
 * NULL/空串/未知值都返回 FILE_PICKER_BACKEND_AUTO —— 契约要求"未知值回退
 * 自动"，绝不因为用户写了错别字就拒绝打开选择框。
 */
static inline int file_picker_backend_from_env(const char *value)
{
    if (!value || value[0] == '\0') {
        return FILE_PICKER_BACKEND_AUTO;
    }
    if (strcmp(value, "portal") == 0 || strcmp(value, "PORTAL") == 0) {
        return FILE_PICKER_BACKEND_PORTAL;
    }
    if (strcmp(value, "zenity") == 0 || strcmp(value, "ZENITY") == 0) {
        return FILE_PICKER_BACKEND_ZENITY;
    }
    if (strcmp(value, "kdialog") == 0 || strcmp(value, "KDIALOG") == 0) {
        return FILE_PICKER_BACKEND_KDIALOG;
    }
    if (strcmp(value, "tty") == 0 || strcmp(value, "TTY") == 0) {
        return FILE_PICKER_BACKEND_TTY;
    }
    return FILE_PICKER_BACKEND_AUTO;
}

/** @brief 纯逻辑：后端枚举 → 稳定名称（日志/错误文案用）。 */
static inline const char *file_picker_backend_name(int backend)
{
    switch (backend) {
        case FILE_PICKER_BACKEND_PORTAL:  return "portal";
        case FILE_PICKER_BACKEND_ZENITY:  return "zenity";
        case FILE_PICKER_BACKEND_KDIALOG: return "kdialog";
        case FILE_PICKER_BACKEND_TTY:     return "tty";
        case FILE_PICKER_BACKEND_AUTO:
        default:                          return "auto";
    }
}

/**
 * @brief 纯逻辑：该后端是否通过外部桌面 GUI 对话框选路径。
 *
 * GUI 对话框（zenity / kdialog / `portal` 偏好值）不需要真实终端，打开期间
 * TUI 的备用屏幕必须保持；`tty` 后端则在 curses 内手输，由 prompt 自己刷新。
 * 新增后端时记得在这里表态（AUTO/未知值一律按"不是 GUI"处理）。
 */
static inline int file_picker_backend_uses_gui_dialog(int backend)
{
    switch (backend) {
        case FILE_PICKER_BACKEND_PORTAL:
        case FILE_PICKER_BACKEND_ZENITY:
        case FILE_PICKER_BACKEND_KDIALOG:
            return 1;
        case FILE_PICKER_BACKEND_TTY:
        case FILE_PICKER_BACKEND_AUTO:
        default:
            return 0;
    }
}

/**
 * @brief 纯逻辑：展开路径开头的 `~/`（home 为空则不展开）。
 *
 * 其余字节**原样**保留：空格、UTF-8、`~` 以外的波浪号都不动，也不做
 * realpath/去重斜杠——调用方拿到的是"用户写的路径"，是否合法由它自己判。
 *
 * @return 0 成功；-1 参数非法或结果超出 out_size。
 */
static inline int file_picker_expand_tilde(const char *in, const char *home,
                                           char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) {
        return -1;
    }
    int need = (home && home[0] && in[0] == '~' &&
                (in[1] == '/' || in[1] == '\0'));
    int written = need
        ? snprintf(out, out_size, "%s%s", home, in + 1)
        : snprintf(out, out_size, "%s", in);
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/**
 * @brief 结合环境变量与可用性得出本次实际使用的后端。
 *
 * 顺序：`TERM_MUSIC_FILE_CHOOSER` 指定的（原样返回，不静默换后端——
 * 强制值不可用时由 file_picker_pick_* 报错，便于测试断言）；
 * 否则 zenity → kdialog → tty（zenity/kdialog 都要求可执行文件与图形会话
 * 同时在线，缺任一项都不是"可用"，直接落到 tty）。
 */
int file_picker_resolve_backend(void);

/**
 * @brief 该后端此刻是否可用。
 *
 * TTY 恒可用（真正的 curses 判空在拾取时做）；外部对话框按 PATH + 图形
 * 会话（DISPLAY/WAYLAND_DISPLAY）探测；强制值不可用时由入口报错。
 */
int file_picker_backend_available(int backend);

/**
 * @brief 弹出文件选择（标题/过滤器/初始目录由调用方给）。
 *
 * @param title       对话框标题（可空）
 * @param filter_name 过滤器显示名（可空，如 "ONNX model"）
 * @param filter_glob 过滤器通配（可空，如 "*.onnx"；多个用 ';' 分隔）
 * @param initial_dir 初始目录（可空 = 后端默认；TTY 后端只用于提示，不改 cwd）
 * @param out         选中路径输出（成功时 NUL 结尾；UTF-8，可含空格）
 * @param out_size    out 容量（建议 ≥ 4096）
 * @param err         失败原因输出（可空）
 * @param err_size    err 容量
 * @return FILE_PICKER_OK / FILE_PICKER_CANCEL / FILE_PICKER_ERROR
 */
int file_picker_pick_file(const char *title, const char *filter_name,
                          const char *filter_glob, const char *initial_dir,
                          char *out, size_t out_size,
                          char *err, size_t err_size);

/**
 * @brief 弹出目录选择（打开文件夹 / 追加文件夹 / 默认音乐目录用）。
 *
 * 与 file_picker_pick_file 同一套后端选择、备用屏幕保持与取消语义，
 * 区别只有三处：
 *   - zenity 加 `--directory`（并忽略文件过滤器）；
 *   - kdialog 用 `--getexistingdirectory`（不再要过滤器参数）；
 *   - TTY 后端提示输入"目录路径"（空输入=取消）。
 *
 * 返回 FILE_PICKER_OK 前会展开 `~/` 并 stat 校验选中路径：必须是**已存在的
 * 目录**，否则 FILE_PICKER_ERROR、out 置空、err 写可读原因（调用方不得在
 * 该情况下写配置/加载列表）。路径原样保留（空格、UTF-8 不做任何改写）。
 *
 * @param title       对话框标题（可空 = 使用 file_picker.folder_title）
 * @param initial_dir 初始目录（可空 = 后端默认 / HOME；TTY 后端只用于提示）
 * @param out         选中目录输出（成功时 NUL 结尾；UTF-8，可含空格）
 * @param out_size    out 容量（建议 ≥ 4096；小于路径长度按"路径过长"报错）
 * @param err         失败原因输出（可空）
 * @param err_size    err 容量
 * @return FILE_PICKER_OK / FILE_PICKER_CANCEL / FILE_PICKER_ERROR
 */
int file_picker_pick_folder(const char *title, const char *initial_dir,
                            char *out, size_t out_size,
                            char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* UI_FILE_PICKER_H */
