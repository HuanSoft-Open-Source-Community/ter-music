/**
 * @file help.c
 * @brief 统一 CLI 帮助数据模型、宽度换行与内置分页器
 *
 * 设计要点：
 *  - 帮助内容先渲染成“行数组”，再统一按终端列宽换行，最后交给分页器；
 *  - 宽度/高度优先取 TTY 的 TIOCGWINSZ，其次 COLUMNS/LINES，最后 80x24；
 *  - 非 TTY、TERM=dumb、TER_MUSIC_NO_PAGER 或内容不足一屏时直接输出，
 *    绝不产生 ANSI 转义序列；
 *  - 分页器使用 termios raw 模式，退出/EOF/SIGINT/SIGTERM 均恢复终端状态。
 *
 * 文案全部来自 i18n_get("cli.help...")，源码中不硬编码中英文。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "cli/help.h"

#include "i18n/i18n.h"
#include "util/utf8.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#define HELP_ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ── 数据模型 ───────────────────────────────────────────────────── */

typedef enum {
    HELP_GROUP_PLAYBACK = 0,
    HELP_GROUP_QUEUE,
    HELP_GROUP_CONFIG,
    HELP_GROUP_INFO,
    HELP_GROUP_DAEMON,
    HELP_GROUP_KEYS,
    HELP_GROUP_OTHER,
    HELP_GROUP_COUNT
} HelpGroup;

typedef struct {
    const char *flags;     /* 左列显示文本，例如 "--index N" */
    const char *key;       /* i18n 键（描述文本） */
} HelpOption;

typedef struct {
    const char *name;
    const char *summary_key;
    const char *usage_key;
    const char *detail_key;
    const HelpOption *options;
    int option_count;
} HelpSubcommand;

typedef struct {
    const char *name;
    HelpGroup group;
    const char *summary_key;
    const char *usage_key;
    const char *detail_key;
    const char *example_key;   /* 可为 NULL，表示无示例 */
    const char *exit_key;      /* NULL → cli.help.exit_codes.cli */
    const HelpOption *options;
    int option_count;
    const HelpSubcommand *subcommands;
    int subcommand_count;
} HelpCommand;

static const char *const k_group_keys[HELP_GROUP_COUNT] = {
    "cli.help.group.playback",
    "cli.help.group.queue",
    "cli.help.group.config",
    "cli.help.group.info",
    "cli.help.group.daemon",
    "cli.help.group.keys",
    "cli.help.group.other"
};

/* ── 顶层 TUI / 全局选项 ────────────────────────────────────────── */

static const HelpOption k_global_options[] = {
    { "-o, --open <path>",         "cli.help.global.opt.open" },
    { "-d, --debug",               "cli.help.global.opt.debug" },
    { "-h, --help",                "cli.help.global.opt.help" },
    { "--frontend=local|remote",   "cli.help.global.opt.frontend" },
    { "--attach-only",             "cli.help.global.opt.attach-only" },
    { "--bus <name>",              "cli.help.global.opt.bus" },
    { "--lang <lang-id>",          "cli.help.global.opt.lang" }
};

/* ── 各命令选项 ─────────────────────────────────────────────────── */

static const HelpOption k_play_options[] = {
    { "--index N",       "cli.help.cmd.play.opt.index" },
    { "--mode <mode>",   "cli.help.cmd.play.opt.mode" },
    { "--no-daemon",     "cli.help.cmd.play.opt.no-daemon" },
    { "--foreground",    "cli.help.cmd.play.opt.foreground" },
    { "--quiet",         "cli.help.cmd.play.opt.quiet" }
};

static const HelpOption k_show_options[] = {
    { "--json",                  "cli.help.cmd.show.opt.json" },
    { "--watch[=MS]",            "cli.help.cmd.show.opt.watch" },
    { "--one-line",              "cli.help.cmd.show.opt.one-line" },
    { "--full",                  "cli.help.cmd.show.opt.full" },
    { "--compact",               "cli.help.cmd.show.opt.compact" },
    { "--preset <name>",         "cli.help.cmd.show.opt.preset" },
    { "--fields <list>",         "cli.help.cmd.show.opt.fields" },
    { "--cover",                 "cli.help.cmd.show.opt.cover" },
    { "--no-cover",              "cli.help.cmd.show.opt.no-cover" },
    { "--cover-size WxH",        "cli.help.cmd.show.opt.cover-size" },
    { "--charset <name>",        "cli.help.cmd.show.opt.charset" },
    { "--progress <style>",      "cli.help.cmd.show.opt.progress" },
    { "--no-progress",           "cli.help.cmd.show.opt.no-progress" },
    { "--lyrics <0|1|2>",        "cli.help.cmd.show.opt.lyrics" },
    { "--width N",               "cli.help.cmd.show.opt.width" }
};

static const HelpOption k_daemon_options[] = {
    { "--open <path>",   "cli.help.cmd.daemon.opt.open" },
    { "--debug",         "cli.help.cmd.daemon.opt.debug" },
    { "--force",         "cli.help.cmd.daemon.opt.force" },
    { "--all",           "cli.help.cmd.daemon.opt.all" },
    { "--no-autoplay",   "cli.help.cmd.daemon.opt.no-autoplay" },
    { "--json",          "cli.help.cmd.daemon.opt.json" }
};

static const HelpOption k_daemon_start_options[] = {
    { "--open <path>",   "cli.help.cmd.daemon.start.opt.open" },
    { "--debug",         "cli.help.cmd.daemon.start.opt.debug" },
    { "--force",         "cli.help.cmd.daemon.start.opt.force" }
};

static const HelpOption k_daemon_foreground_options[] = {
    { "--open <path>",   "cli.help.cmd.daemon.foreground.opt.open" },
    { "--debug",         "cli.help.cmd.daemon.foreground.opt.debug" },
    { "--force",         "cli.help.cmd.daemon.foreground.opt.force" },
    { "--no-autoplay",   "cli.help.cmd.daemon.foreground.opt.no-autoplay" }
};

static const HelpOption k_daemon_stop_options[] = {
    { "--force",         "cli.help.cmd.daemon.stop.opt.force" },
    { "--all",           "cli.help.cmd.daemon.stop.opt.all" }
};

static const HelpOption k_daemon_restart_options[] = {
    { "--open <path>",   "cli.help.cmd.daemon.restart.opt.open" },
    { "--debug",         "cli.help.cmd.daemon.restart.opt.debug" },
    { "--force",         "cli.help.cmd.daemon.restart.opt.force" }
};

static const HelpOption k_daemon_status_options[] = {
    { "--json",          "cli.help.cmd.daemon.status.opt.json" }
};

static const HelpOption k_queue_list_options[] = {
    { "--offset N",      "cli.help.cmd.queue.list.opt.offset" },
    { "--count N",       "cli.help.cmd.queue.list.opt.count" }
};

static const HelpOption k_config_reset_options[] = {
    { "--yes",           "cli.help.cmd.config.reset.opt.yes" }
};

static const HelpOption k_keys_json_options[] = {
    { "--json",          "cli.help.cmd.keys.opt.json" }
};

static const HelpOption k_lyrics_options[] = {
    { "--lines N",       "cli.help.cmd.lyrics.opt.lines" }
};

static const HelpOption k_cover_options[] = {
    { "--width N",       "cli.help.cmd.cover.opt.width" },
    { "--height N",      "cli.help.cmd.cover.opt.height" },
    { "--charset <name>", "cli.help.cmd.cover.opt.charset" }
};

/* ── 子命令 ─────────────────────────────────────────────────────── */

static const HelpSubcommand k_daemon_subcommands[] = {
    {
        .name = "start",
        .summary_key = "cli.help.cmd.daemon.start.summary",
        .usage_key = "cli.help.cmd.daemon.start.usage",
        .detail_key = "cli.help.cmd.daemon.start.detail",
        .options = k_daemon_start_options,
        .option_count = HELP_ARRAY_LEN(k_daemon_start_options)
    },
    {
        .name = "foreground",
        .summary_key = "cli.help.cmd.daemon.foreground.summary",
        .usage_key = "cli.help.cmd.daemon.foreground.usage",
        .detail_key = "cli.help.cmd.daemon.foreground.detail",
        .options = k_daemon_foreground_options,
        .option_count = HELP_ARRAY_LEN(k_daemon_foreground_options)
    },
    {
        .name = "stop",
        .summary_key = "cli.help.cmd.daemon.stop.summary",
        .usage_key = "cli.help.cmd.daemon.stop.usage",
        .detail_key = "cli.help.cmd.daemon.stop.detail",
        .options = k_daemon_stop_options,
        .option_count = HELP_ARRAY_LEN(k_daemon_stop_options)
    },
    {
        .name = "restart",
        .summary_key = "cli.help.cmd.daemon.restart.summary",
        .usage_key = "cli.help.cmd.daemon.restart.usage",
        .detail_key = "cli.help.cmd.daemon.restart.detail",
        .options = k_daemon_restart_options,
        .option_count = HELP_ARRAY_LEN(k_daemon_restart_options)
    },
    {
        .name = "status",
        .summary_key = "cli.help.cmd.daemon.status.summary",
        .usage_key = "cli.help.cmd.daemon.status.usage",
        .detail_key = "cli.help.cmd.daemon.status.detail",
        .options = k_daemon_status_options,
        .option_count = HELP_ARRAY_LEN(k_daemon_status_options)
    },
    {
        .name = "reload",
        .summary_key = "cli.help.cmd.daemon.reload.summary",
        .usage_key = "cli.help.cmd.daemon.reload.usage",
        .detail_key = "cli.help.cmd.daemon.reload.detail",
        .options = NULL,
        .option_count = 0
    }
};

static const HelpSubcommand k_queue_subcommands[] = {
    {
        .name = "list",
        .summary_key = "cli.help.cmd.queue.list.summary",
        .usage_key = "cli.help.cmd.queue.list.usage",
        .detail_key = "cli.help.cmd.queue.list.detail",
        .options = k_queue_list_options,
        .option_count = HELP_ARRAY_LEN(k_queue_list_options)
    },
    {
        .name = "clear",
        .summary_key = "cli.help.cmd.queue.clear.summary",
        .usage_key = "cli.help.cmd.queue.clear.usage",
        .detail_key = "cli.help.cmd.queue.clear.detail",
        .options = NULL,
        .option_count = 0
    },
    {
        .name = "shuffle",
        .summary_key = "cli.help.cmd.queue.shuffle.summary",
        .usage_key = "cli.help.cmd.queue.shuffle.usage",
        .detail_key = "cli.help.cmd.queue.shuffle.detail",
        .options = NULL,
        .option_count = 0
    },
    {
        .name = "remove",
        .summary_key = "cli.help.cmd.queue.remove.summary",
        .usage_key = "cli.help.cmd.queue.remove.usage",
        .detail_key = "cli.help.cmd.queue.remove.detail",
        .options = NULL,
        .option_count = 0
    },
    {
        .name = "append",
        .summary_key = "cli.help.cmd.queue.append.summary",
        .usage_key = "cli.help.cmd.queue.append.usage",
        .detail_key = "cli.help.cmd.queue.append.detail",
        .options = NULL,
        .option_count = 0
    },
    {
        .name = "insert",
        .summary_key = "cli.help.cmd.queue.insert.summary",
        .usage_key = "cli.help.cmd.queue.insert.usage",
        .detail_key = "cli.help.cmd.queue.insert.detail",
        .options = NULL,
        .option_count = 0
    }
};

static const HelpSubcommand k_config_subcommands[] = {
    {
        .name = "get",
        .summary_key = "cli.help.cmd.config.get.summary",
        .usage_key = "cli.help.cmd.config.get.usage",
        .detail_key = "cli.help.cmd.config.get.detail",
        .options = NULL,
        .option_count = 0
    },
    {
        .name = "set",
        .summary_key = "cli.help.cmd.config.set.summary",
        .usage_key = "cli.help.cmd.config.set.usage",
        .detail_key = "cli.help.cmd.config.set.detail",
        .options = NULL,
        .option_count = 0
    },
    {
        .name = "reload",
        .summary_key = "cli.help.cmd.config.reload.summary",
        .usage_key = "cli.help.cmd.config.reload.usage",
        .detail_key = "cli.help.cmd.config.reload.detail",
        .options = NULL,
        .option_count = 0
    },
    {
        .name = "reset",
        .summary_key = "cli.help.cmd.config.reset.summary",
        .usage_key = "cli.help.cmd.config.reset.usage",
        .detail_key = "cli.help.cmd.config.reset.detail",
        .options = k_config_reset_options,
        .option_count = HELP_ARRAY_LEN(k_config_reset_options)
    }
};

static const HelpSubcommand k_keys_subcommands[] = {
    {
        .name = "list",
        .summary_key = "cli.help.cmd.keys.list.summary",
        .usage_key = "cli.help.cmd.keys.list.usage",
        .detail_key = "cli.help.cmd.keys.list.detail",
        .options = k_keys_json_options,
        .option_count = HELP_ARRAY_LEN(k_keys_json_options)
    },
    {
        .name = "check",
        .summary_key = "cli.help.cmd.keys.check.summary",
        .usage_key = "cli.help.cmd.keys.check.usage",
        .detail_key = "cli.help.cmd.keys.check.detail",
        .options = k_keys_json_options,
        .option_count = HELP_ARRAY_LEN(k_keys_json_options)
    }
};

/* ── 命令表 ─────────────────────────────────────────────────────── */

static const HelpCommand k_commands[] = {
    {
        .name = "tui",
        .group = HELP_GROUP_OTHER,
        .summary_key = "cli.help.cmd.tui.summary",
        .usage_key = "cli.help.cmd.tui.usage",
        .detail_key = "cli.help.cmd.tui.detail",
        .example_key = "cli.help.cmd.tui.example",
        .exit_key = "cli.help.exit_codes.tui"
    },
    {
        .name = "play",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.play.summary",
        .usage_key = "cli.help.cmd.play.usage",
        .detail_key = "cli.help.cmd.play.detail",
        .example_key = "cli.help.cmd.play.example",
        .exit_key = "cli.help.exit_codes.cli",
        .options = k_play_options,
        .option_count = HELP_ARRAY_LEN(k_play_options)
    },
    {
        .name = "pause",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.pause.summary",
        .usage_key = "cli.help.cmd.pause.usage",
        .detail_key = "cli.help.cmd.pause.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "resume",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.resume.summary",
        .usage_key = "cli.help.cmd.resume.usage",
        .detail_key = "cli.help.cmd.resume.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "toggle",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.toggle.summary",
        .usage_key = "cli.help.cmd.toggle.usage",
        .detail_key = "cli.help.cmd.toggle.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "stop",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.stop.summary",
        .usage_key = "cli.help.cmd.stop.usage",
        .detail_key = "cli.help.cmd.stop.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "next",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.next.summary",
        .usage_key = "cli.help.cmd.next.usage",
        .detail_key = "cli.help.cmd.next.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "prev",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.prev.summary",
        .usage_key = "cli.help.cmd.prev.usage",
        .detail_key = "cli.help.cmd.prev.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "seek",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.seek.summary",
        .usage_key = "cli.help.cmd.seek.usage",
        .detail_key = "cli.help.cmd.seek.detail",
        .example_key = "cli.help.cmd.seek.example",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "volume",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.volume.summary",
        .usage_key = "cli.help.cmd.volume.usage",
        .detail_key = "cli.help.cmd.volume.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "speed",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.speed.summary",
        .usage_key = "cli.help.cmd.speed.usage",
        .detail_key = "cli.help.cmd.speed.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "mode",
        .group = HELP_GROUP_PLAYBACK,
        .summary_key = "cli.help.cmd.mode.summary",
        .usage_key = "cli.help.cmd.mode.usage",
        .detail_key = "cli.help.cmd.mode.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "show",
        .group = HELP_GROUP_INFO,
        .summary_key = "cli.help.cmd.show.summary",
        .usage_key = "cli.help.cmd.show.usage",
        .detail_key = "cli.help.cmd.show.detail",
        .example_key = "cli.help.cmd.show.example",
        .exit_key = "cli.help.exit_codes.cli",
        .options = k_show_options,
        .option_count = HELP_ARRAY_LEN(k_show_options)
    },
    {
        .name = "daemon",
        .group = HELP_GROUP_DAEMON,
        .summary_key = "cli.help.cmd.daemon.summary",
        .usage_key = "cli.help.cmd.daemon.usage",
        .detail_key = "cli.help.cmd.daemon.detail",
        .example_key = "cli.help.cmd.daemon.example",
        .exit_key = "cli.help.exit_codes.cli",
        .options = k_daemon_options,
        .option_count = HELP_ARRAY_LEN(k_daemon_options),
        .subcommands = k_daemon_subcommands,
        .subcommand_count = HELP_ARRAY_LEN(k_daemon_subcommands)
    },
    {
        .name = "queue",
        .group = HELP_GROUP_QUEUE,
        .summary_key = "cli.help.cmd.queue.summary",
        .usage_key = "cli.help.cmd.queue.usage",
        .detail_key = "cli.help.cmd.queue.detail",
        .example_key = "cli.help.cmd.queue.example",
        .exit_key = "cli.help.exit_codes.cli",
        .subcommands = k_queue_subcommands,
        .subcommand_count = HELP_ARRAY_LEN(k_queue_subcommands)
    },
    {
        .name = "config",
        .group = HELP_GROUP_CONFIG,
        .summary_key = "cli.help.cmd.config.summary",
        .usage_key = "cli.help.cmd.config.usage",
        .detail_key = "cli.help.cmd.config.detail",
        .example_key = "cli.help.cmd.config.example",
        .exit_key = "cli.help.exit_codes.cli",
        .subcommands = k_config_subcommands,
        .subcommand_count = HELP_ARRAY_LEN(k_config_subcommands)
    },
    {
        .name = "lyrics",
        .group = HELP_GROUP_INFO,
        .summary_key = "cli.help.cmd.lyrics.summary",
        .usage_key = "cli.help.cmd.lyrics.usage",
        .detail_key = "cli.help.cmd.lyrics.detail",
        .exit_key = "cli.help.exit_codes.cli",
        .options = k_lyrics_options,
        .option_count = HELP_ARRAY_LEN(k_lyrics_options)
    },
    {
        .name = "track",
        .group = HELP_GROUP_INFO,
        .summary_key = "cli.help.cmd.track.summary",
        .usage_key = "cli.help.cmd.track.usage",
        .detail_key = "cli.help.cmd.track.detail",
        .exit_key = "cli.help.exit_codes.cli"
    },
    {
        .name = "cover",
        .group = HELP_GROUP_INFO,
        .summary_key = "cli.help.cmd.cover.summary",
        .usage_key = "cli.help.cmd.cover.usage",
        .detail_key = "cli.help.cmd.cover.detail",
        .example_key = "cli.help.cmd.cover.example",
        .exit_key = "cli.help.exit_codes.cli",
        .options = k_cover_options,
        .option_count = HELP_ARRAY_LEN(k_cover_options)
    },
    {
        .name = "keys",
        .group = HELP_GROUP_KEYS,
        .summary_key = "cli.help.cmd.keys.summary",
        .usage_key = "cli.help.cmd.keys.usage",
        .detail_key = "cli.help.cmd.keys.detail",
        .example_key = "cli.help.cmd.keys.example",
        .exit_key = "cli.help.exit_codes.cli",
        .subcommands = k_keys_subcommands,
        .subcommand_count = HELP_ARRAY_LEN(k_keys_subcommands)
    },
    {
        .name = "version",
        .group = HELP_GROUP_OTHER,
        .summary_key = "cli.help.cmd.version.summary",
        .usage_key = "cli.help.cmd.version.usage",
        .detail_key = "cli.help.cmd.version.detail",
        .exit_key = "cli.help.exit_codes.ok"
    },
    {
        .name = "help",
        .group = HELP_GROUP_OTHER,
        .summary_key = "cli.help.cmd.help.summary",
        .usage_key = "cli.help.cmd.help.usage",
        .detail_key = "cli.help.cmd.help.detail",
        .example_key = "cli.help.cmd.help.example",
        .exit_key = "cli.help.exit_codes.ok"
    }
};

/* ── 行数组与换行 ───────────────────────────────────────────────── */

typedef struct {
    char **items;
    int count;
    int capacity;
} HelpLines;

static int lines_push(HelpLines *out, const char *text)
{
    if (!out) {
        return -1;
    }
    if (out->count >= out->capacity) {
        int next = out->capacity > 0 ? out->capacity * 2 : 128;
        char **grown = realloc(out->items, (size_t)next * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        out->items = grown;
        out->capacity = next;
    }
    if (!text) {
        text = "";
    }
    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == ' ') {
        len--;
    }
    char *copy = malloc(len + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    out->items[out->count++] = copy;
    return 0;
}

static void lines_free(HelpLines *lines)
{
    if (!lines) {
        return;
    }
    for (int i = 0; i < lines->count; i++) {
        free(lines->items[i]);
    }
    free(lines->items);
    lines->items = NULL;
    lines->count = 0;
    lines->capacity = 0;
}

static int lines_push_segment(HelpLines *out, int indent, const char *text,
                              size_t len)
{
    while (len > 0 && text[len - 1] == ' ') {
        len--;
    }
    size_t total = (size_t)indent + len;
    char *buf = malloc(total + 1);
    if (!buf) {
        return -1;
    }
    memset(buf, ' ', (size_t)indent);
    memcpy(buf + indent, text, len);
    buf[total] = '\0';
    int rc = lines_push(out, buf);
    free(buf);
    return rc;
}

/* 按显示列宽折行；cont_indent < 0 时续行沿用首行缩进。
 * 不会切断 UTF-8 字符；仅在空格处优先断行。 */
static void help_wrap_line(HelpLines *out, const char *text, int width,
                           int cont_indent)
{
    if (width < 1) {
        width = 1;
    }
    if (!text) {
        text = "";
    }

    const char *p = text;
    int first_indent = 0;
    while (*p == ' ') {
        first_indent++;
        p++;
    }
    if (first_indent >= width) {
        first_indent = 0;
        p = text;
    }

    int indent = cont_indent < 0 ? first_indent : cont_indent;
    if (indent >= width) {
        indent = width > 1 ? width - 1 : 0;
    }

    if (*p == '\0') {
        lines_push(out, "");
        return;
    }

    int first = 1;
    while (*p) {
        int cur_indent = first ? first_indent : indent;
        if (cur_indent >= width) {
            cur_indent = width > 1 ? width - 1 : 0;
        }
        int avail = width - cur_indent;
        if (avail < 1) {
            avail = 1;
        }

        const char *q = p;
        int cols = 0;
        while (*q) {
            int char_width = 1;
            size_t char_len = utf8_next_char(q, NULL, &char_width);
            if (char_len == 0) {
                break;
            }
            if (cols + char_width > avail) {
                break;
            }
            cols += char_width;
            q += char_len;
        }
        if (q == p) {
            /* 单个字符比可用列还宽（例如 1 列终端里的全角字符）：
             * 仍完整取一个字符，避免切断 UTF-8。 */
            int char_width = 1;
            size_t char_len = utf8_next_char(p, NULL, &char_width);
            q = p + (char_len > 0 ? char_len : 1);
        }

        size_t chunk = (size_t)(q - p);
        if (*q) {
            const char *space = NULL;
            for (const char *r = p; r < q; r++) {
                if (*r == ' ') {
                    space = r;
                }
            }
            if (space && space > p) {
                chunk = (size_t)(space - p);
            }
        }

        lines_push_segment(out, cur_indent, p, chunk);

        if (*q == '\0') {
            break;
        }
        if (chunk < (size_t)(q - p)) {
            p += chunk;
            while (*p == ' ') {
                p++;
            }
        } else {
            p = q;
        }
        first = 0;
    }
}

static void help_wrap_text(HelpLines *out, const char *text, int width,
                           int cont_indent)
{
    if (!text) {
        text = "";
    }
    const char *start = text;
    for (;;) {
        const char *nl = strchr(start, '\n');
        size_t len = nl ? (size_t)(nl - start) : strlen(start);
        char *segment = malloc(len + 1);
        if (segment) {
            memcpy(segment, start, len);
            segment[len] = '\0';
            help_wrap_line(out, segment, width, cont_indent);
            free(segment);
        }
        if (!nl) {
            break;
        }
        start = nl + 1;
    }
}

/* 两列布局：左列补齐到 left_width 列，右列在剩余宽度内换行；
 * 续行缩进与右列起点对齐（右列起点放不下时退回上下两段）。 */
static void help_render_columns(HelpLines *out, int width, const char *left,
                                int left_width, const char *right)
{
    char padded[256];
    utf8_str_pad(padded, sizeof(padded), left ? left : "", left_width);

    int indent_cols = 2 + left_width + 2;
    if (indent_cols < width) {
        HelpLines chunks = {0};
        help_wrap_text(&chunks, right ? right : "", width - indent_cols, 0);

        size_t prefix_bytes = 2 + strlen(padded) + 2;
        for (int i = 0; i < chunks.count; i++) {
            const char *chunk = chunks.items[i];
            size_t chunk_len = strlen(chunk);
            if (i == 0) {
                size_t size = prefix_bytes + chunk_len + 1;
                char *line = malloc(size);
                if (line) {
                    snprintf(line, size, "  %s  %s", padded, chunk);
                    lines_push(out, line);
                    free(line);
                }
            } else {
                size_t size = (size_t)indent_cols + chunk_len + 1;
                char *line = malloc(size);
                if (line) {
                    memset(line, ' ', (size_t)indent_cols);
                    memcpy(line + indent_cols, chunk, chunk_len + 1);
                    lines_push(out, line);
                    free(line);
                }
            }
        }
        lines_free(&chunks);
        return;
    }

    /* 左列加缩进已占满宽度：左右两列各自按全宽换行 */
    help_wrap_text(out, left ? left : "", width, -1);
    help_wrap_text(out, right ? right : "", width, -1);
}

/* ── 占位符展开：{prog} / {page} / {pages} ──────────────────────── */

static char *help_expand_alloc(const char *src, const char *prog,
                               int page, int pages)
{
    if (!src) {
        src = "";
    }
    if (!prog) {
        prog = "";
    }

    size_t need = 1;
    for (const char *p = src; *p; ) {
        if (strncmp(p, "{prog}", 6) == 0) {
            need += strlen(prog);
            p += 6;
        } else if (strncmp(p, "{page}", 6) == 0) {
            need += 16;
            p += 6;
        } else if (strncmp(p, "{pages}", 7) == 0) {
            need += 16;
            p += 7;
        } else {
            need++;
            p++;
        }
    }

    char *out = malloc(need);
    if (!out) {
        return NULL;
    }

    size_t o = 0;
    for (const char *p = src; *p; ) {
        if (strncmp(p, "{prog}", 6) == 0) {
            size_t len = strlen(prog);
            memcpy(out + o, prog, len);
            o += len;
            p += 6;
        } else if (strncmp(p, "{page}", 6) == 0) {
            char num[16];
            int n = snprintf(num, sizeof(num), "%d", page);
            if (n > 0) {
                memcpy(out + o, num, (size_t)n);
                o += (size_t)n;
            }
            p += 6;
        } else if (strncmp(p, "{pages}", 7) == 0) {
            char num[16];
            int n = snprintf(num, sizeof(num), "%d", pages);
            if (n > 0) {
                memcpy(out + o, num, (size_t)n);
                o += (size_t)n;
            }
            p += 7;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return out;
}

static void help_add_key(HelpLines *out, const char *key, const char *prog,
                         int width)
{
    char *text = help_expand_alloc(i18n_get(key), prog, 0, 0);
    if (!text) {
        return;
    }
    help_wrap_text(out, text, width, -1);
    free(text);
}

static void help_add_blank(HelpLines *out)
{
    lines_push(out, "");
}

static int help_key_present(const char *key)
{
    return i18n_get(key) != key;
}

/* ── 终端尺寸 ───────────────────────────────────────────────────── */

static int help_terminal_width(void)
{
    struct winsize ws;
    if (isatty(STDOUT_FILENO) &&
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (int)ws.ws_col;
    }
    const char *columns = getenv("COLUMNS");
    if (columns && columns[0]) {
        int value = atoi(columns);
        if (value > 0) {
            return value;
        }
    }
    return 80;
}

static int help_terminal_height(void)
{
    struct winsize ws;
    if (isatty(STDOUT_FILENO) &&
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return (int)ws.ws_row;
    }
    const char *lines = getenv("LINES");
    if (lines && lines[0]) {
        int value = atoi(lines);
        if (value > 0) {
            return value;
        }
    }
    return 24;
}

/* ── 渲染：概览 ─────────────────────────────────────────────────── */

static int help_name_column(const HelpCommand *commands, int count,
                            HelpGroup group)
{
    int width = 0;
    for (int i = 0; i < count; i++) {
        if (commands[i].group != group) {
            continue;
        }
        int w = utf8_str_width(commands[i].name);
        if (w > width) {
            width = w;
        }
    }
    return width;
}

static void help_render_option_rows(HelpLines *out, const char *prog,
                                    int width, const HelpOption *options,
                                    int count)
{
    if (!options || count <= 0) {
        return;
    }

    int name_width = 0;
    for (int i = 0; i < count; i++) {
        int w = utf8_str_width(options[i].flags);
        if (w > name_width) {
            name_width = w;
        }
    }

    for (int i = 0; i < count; i++) {
        char *desc = help_expand_alloc(i18n_get(options[i].key), prog, 0, 0);
        if (!desc) {
            continue;
        }
        help_render_columns(out, width, options[i].flags, name_width, desc);
        free(desc);
    }
}

static void help_render_options(HelpLines *out, const char *prog, int width,
                                const HelpOption *options, int count)
{
    if (!options || count <= 0) {
        return;
    }
    help_add_key(out, "cli.help.section.options", prog, width);
    help_render_option_rows(out, prog, width, options, count);
    help_add_blank(out);
}

static void help_render_subcommands(HelpLines *out, const char *prog, int width,
                                    const HelpCommand *cmd)
{
    if (!cmd->subcommands || cmd->subcommand_count <= 0) {
        return;
    }

    int name_width = 0;
    for (int i = 0; i < cmd->subcommand_count; i++) {
        int w = utf8_str_width(cmd->subcommands[i].name);
        if (w > name_width) {
            name_width = w;
        }
    }

    help_add_key(out, "cli.help.section.subcommands", prog, width);
    for (int i = 0; i < cmd->subcommand_count; i++) {
        const HelpSubcommand *sub = &cmd->subcommands[i];
        char *summary = help_expand_alloc(i18n_get(sub->summary_key), prog, 0, 0);
        if (!summary) {
            continue;
        }
        help_render_columns(out, width, sub->name, name_width, summary);
        free(summary);
    }
    help_add_blank(out);
}

static void help_render_overview(HelpLines *out, const char *prog, int width)
{
    help_add_key(out, "cli.help.overview.usage", prog, width);
    help_add_blank(out);

    int command_count = HELP_ARRAY_LEN(k_commands);
    for (int g = 0; g < HELP_GROUP_COUNT; g++) {
        int name_width = help_name_column(k_commands, command_count,
                                          (HelpGroup)g);
        if (name_width == 0) {
            continue;
        }

        help_add_key(out, k_group_keys[g], prog, width);
        for (int i = 0; i < command_count; i++) {
            const HelpCommand *cmd = &k_commands[i];
            if (cmd->group != (HelpGroup)g) {
                continue;
            }
            char *summary = help_expand_alloc(i18n_get(cmd->summary_key),
                                              prog, 0, 0);
            if (!summary) {
                continue;
            }
            help_render_columns(out, width, cmd->name, name_width, summary);
            free(summary);
        }
        help_add_blank(out);
    }

    help_add_key(out, "cli.help.overview.global_options", prog, width);
    help_render_option_rows(out, prog, width, k_global_options,
                            HELP_ARRAY_LEN(k_global_options));
    help_add_blank(out);
    help_add_key(out, "cli.help.overview.hint", prog, width);
}

/* ── 渲染：详情 ─────────────────────────────────────────────────── */

static void help_render_command(HelpLines *out, const char *prog, int width,
                                const HelpCommand *cmd,
                                const HelpSubcommand *sub)
{
    const char *usage_key = sub ? sub->usage_key : cmd->usage_key;
    const char *detail_key = sub ? sub->detail_key : cmd->detail_key;

    help_add_key(out, usage_key, prog, width);
    help_add_blank(out);
    help_add_key(out, detail_key, prog, width);
    help_add_blank(out);

    if (sub) {
        help_render_options(out, prog, width, sub->options, sub->option_count);
    } else {
        help_render_options(out, prog, width, cmd->options, cmd->option_count);
        help_render_subcommands(out, prog, width, cmd);
    }

    if (!sub && cmd->example_key && help_key_present(cmd->example_key)) {
        help_add_key(out, "cli.help.section.examples", prog, width);
        help_add_key(out, cmd->example_key, prog, width);
        help_add_blank(out);
    }

    help_add_key(out, "cli.help.section.exit_codes", prog, width);
    help_add_key(out, cmd->exit_key ? cmd->exit_key
                                    : "cli.help.exit_codes.cli",
                 prog, width);
}

/* ── 内置分页器 ─────────────────────────────────────────────────── */

static volatile sig_atomic_t g_pager_signal_no = 0;

static void help_pager_signal(int sig)
{
    g_pager_signal_no = sig;
}

static int help_pager_enabled(int total_lines, int height)
{
    if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) {
        return 0;
    }
    const char *term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) {
        return 0;
    }
    if (getenv("TER_MUSIC_NO_PAGER")) {
        return 0;
    }
    return total_lines > height;
}

static int pager_read_byte(int timeout_ms)
{
    if (timeout_ms >= 0) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int ready = select(STDIN_FILENO + 1, &set, NULL, NULL, &tv);
        if (ready <= 0) {
            return -1;
        }
    }

    unsigned char c = 0;
    ssize_t n;
    do {
        n = read(STDIN_FILENO, &c, 1);
    } while (n < 0 && errno == EINTR && !g_pager_signal_no);

    if (n <= 0) {
        return -1;
    }
    return (int)c;
}

enum {
    PK_NONE = 0,
    PK_EOF,
    PK_QUIT,
    PK_PAGE_DOWN,
    PK_PAGE_UP,
    PK_LINE_DOWN,
    PK_LINE_UP,
    PK_TOP,
    PK_BOTTOM
};

static int pager_read_key(void)
{
    int c = pager_read_byte(-1);
    if (c < 0) {
        return PK_EOF;
    }

    switch (c) {
    case ' ':
        return PK_PAGE_DOWN;
    case 'b':
        return PK_PAGE_UP;
    case 'j':
        return PK_LINE_DOWN;
    case 'k':
        return PK_LINE_UP;
    case 'g':
        return PK_TOP;
    case 'G':
        return PK_BOTTOM;
    case 'q':
    case 'Q':
        return PK_QUIT;
    default:
        break;
    }

    if (c != 0x1b) {
        return PK_NONE;
    }

    char seq[8];
    int n = 0;

    int c2 = pager_read_byte(50);
    if (c2 < 0) {
        return PK_QUIT;   /* 单独的 ESC */
    }
    seq[n++] = (char)c2;

    if (c2 == '[' || c2 == 'O') {
        while (n < (int)sizeof(seq) - 1) {
            int c3 = pager_read_byte(50);
            if (c3 < 0) {
                break;
            }
            seq[n++] = (char)c3;
            if ((c3 >= 'A' && c3 <= 'Z') || c3 == '~') {
                break;
            }
        }
    }
    seq[n] = '\0';

    if (seq[0] == '[') {
        if (strstr(seq, "6~")) return PK_PAGE_DOWN;
        if (strstr(seq, "5~")) return PK_PAGE_UP;
        if (strchr(seq, 'B')) return PK_LINE_DOWN;
        if (strchr(seq, 'A')) return PK_LINE_UP;
        if (strchr(seq, 'H') || strstr(seq, "1~") || strstr(seq, "7~")) {
            return PK_TOP;
        }
        if (strchr(seq, 'F') || strstr(seq, "4~") || strstr(seq, "8~")) {
            return PK_BOTTOM;
        }
    } else if (seq[0] == 'O') {
        if (seq[1] == 'A') return PK_LINE_UP;
        if (seq[1] == 'B') return PK_LINE_DOWN;
        if (seq[1] == 'H') return PK_TOP;
        if (seq[1] == 'F') return PK_BOTTOM;
    }
    return PK_NONE;
}

static void pager_build_footer(char *out, size_t out_size, int width,
                               int page, int pages)
{
    (void)out_size;   /* 页脚文本远小于调用方缓冲区，宽度截断由 width 控制 */
    char *left = help_expand_alloc(i18n_get("cli.help.pager.footer"), "",
                                   page, pages);
    char *right = help_expand_alloc(i18n_get("cli.help.pager.quit"), "",
                                    page, pages);
    const char *left_text = left ? left : "";
    const char *right_text = right ? right : "";

    size_t need = strlen(left_text) + strlen(right_text) + 3;
    char *joined = malloc(need);
    if (joined) {
        snprintf(joined, need, "%s  %s", left_text, right_text);
        utf8_str_truncate(out, joined, width);
        free(joined);
    } else {
        out[0] = '\0';
    }
    free(left);
    free(right);
}

static void pager_draw(const HelpLines *lines, int offset, int rows, int width,
                       int page, int pages)
{
    fputs("\033[H\033[2J", stdout);
    for (int i = 0; i < rows; i++) {
        int index = offset + i;
        if (index >= 0 && index < lines->count) {
            fputs(lines->items[index], stdout);
        }
        fputc('\n', stdout);
    }

    char footer[1024];
    pager_build_footer(footer, sizeof(footer), width, page, pages);
    fputs(footer, stdout);
    fflush(stdout);
}

static void help_print_plain(const HelpLines *lines)
{
    for (int i = 0; i < lines->count; i++) {
        printf("%s\n", lines->items[i]);
    }
    fflush(stdout);
}

static void help_pager_run(const HelpLines *lines, int width, int height)
{
    struct termios saved;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        help_print_plain(lines);
        return;
    }

    if (height < 2) {
        height = 2;
    }
    int rows = height - 1;
    if (rows < 1) {
        rows = 1;
    }

    int pages = (lines->count + rows - 1) / rows;
    if (pages < 1) {
        pages = 1;
    }
    int max_offset = (pages - 1) * rows;
    int offset = 0;

    struct termios raw = saved;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    struct sigaction action;
    struct sigaction old_int;
    struct sigaction old_term;
    memset(&action, 0, sizeof(action));
    action.sa_handler = help_pager_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;   /* 不设 SA_RESTART，read() 可被信号打断 */
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);

    g_pager_signal_no = 0;

    int quit = 0;
    while (!g_pager_signal_no) {
        pager_draw(lines, offset, rows, width, offset / rows + 1, pages);

        int key = pager_read_key();
        if (key == PK_EOF && !g_pager_signal_no) {
            break;
        }

        switch (key) {
        case PK_QUIT:
            quit = 1;
            break;
        case PK_PAGE_DOWN:
            offset += rows;
            break;
        case PK_PAGE_UP:
            offset -= rows;
            break;
        case PK_LINE_DOWN:
            offset += 1;
            break;
        case PK_LINE_UP:
            offset -= 1;
            break;
        case PK_TOP:
            offset = 0;
            break;
        case PK_BOTTOM:
            offset = max_offset;
            break;
        default:
            break;
        }

        if (offset < 0) {
            offset = 0;
        }
        if (offset > max_offset) {
            offset = max_offset;
        }
        if (quit) {
            break;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);

    int sig = (int)g_pager_signal_no;
    g_pager_signal_no = 0;

    fputc('\n', stdout);
    fflush(stdout);

    if (sig != 0) {
        raise(sig);
    }
}

static void help_emit(HelpLines *lines, int width)
{
    int height = help_terminal_height();
    if (help_pager_enabled(lines->count, height)) {
        help_pager_run(lines, width, height);
    } else {
        help_print_plain(lines);
    }
}

/* ── 查找 ───────────────────────────────────────────────────────── */

static const char *help_normalize_command(const char *command)
{
    if (!command) {
        return NULL;
    }
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        return "help";
    }
    if (strcmp(command, "--version") == 0) {
        return "version";
    }
    return command;
}

static const HelpCommand *help_find_command(const char *command)
{
    const char *name = help_normalize_command(command);
    if (!name) {
        return NULL;
    }
    for (int i = 0; i < HELP_ARRAY_LEN(k_commands); i++) {
        if (strcmp(k_commands[i].name, name) == 0) {
            return &k_commands[i];
        }
    }
    return NULL;
}

static const HelpSubcommand *help_find_subcommand(const HelpCommand *cmd,
                                                  const char *subcommand)
{
    if (!cmd || !subcommand || !cmd->subcommands) {
        return NULL;
    }
    for (int i = 0; i < cmd->subcommand_count; i++) {
        if (strcmp(cmd->subcommands[i].name, subcommand) == 0) {
            return &cmd->subcommands[i];
        }
    }
    return NULL;
}

/* ── 公共 API ───────────────────────────────────────────────────── */

void cli_help_print_overview(const char *program)
{
    const char *prog = (program && program[0]) ? program : "ter-music";
    int width = help_terminal_width();
    HelpLines lines = {0};

    help_render_overview(&lines, prog, width);
    help_emit(&lines, width);
    lines_free(&lines);
}

int cli_help_print_command(const char *program, const char *command,
                           const char *subcommand)
{
    const HelpCommand *cmd = help_find_command(command);
    if (!cmd) {
        return -1;
    }

    const HelpSubcommand *sub = NULL;
    if (subcommand) {
        sub = help_find_subcommand(cmd, subcommand);
        if (!sub) {
            return -1;
        }
    }

    const char *prog = (program && program[0]) ? program : "ter-music";
    int width = help_terminal_width();
    HelpLines lines = {0};

    help_render_command(&lines, prog, width, cmd, sub);
    help_emit(&lines, width);
    lines_free(&lines);
    return 0;
}

void cli_help_print_all(const char *program)
{
    const char *prog = (program && program[0]) ? program : "ter-music";
    int width = help_terminal_width();
    HelpLines lines = {0};

    help_render_overview(&lines, prog, width);

    for (int i = 0; i < HELP_ARRAY_LEN(k_commands); i++) {
        const HelpCommand *cmd = &k_commands[i];
        help_add_blank(&lines);
        help_render_command(&lines, prog, width, cmd, NULL);

        for (int j = 0; j < cmd->subcommand_count; j++) {
            help_add_blank(&lines);
            help_render_command(&lines, prog, width, cmd,
                                &cmd->subcommands[j]);
        }
    }

    help_emit(&lines, width);
    lines_free(&lines);
}

int cli_help_has_command(const char *command)
{
    return help_find_command(command) != NULL;
}

int cli_help_has_subcommand(const char *command, const char *subcommand)
{
    const HelpCommand *cmd = help_find_command(command);
    return help_find_subcommand(cmd, subcommand) != NULL;
}
