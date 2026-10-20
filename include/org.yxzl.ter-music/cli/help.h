/**
 * @file help.h
 * @brief 统一 CLI 帮助数据模型、宽度换行与内置分页器
 *
 * 本模块只负责“帮助数据 + 渲染 + 分页”，不参与命令分发：
 * 调用方（main.c / cli.c）自行决定何时输出概览、详情或全量帮助。
 *
 * 所有文案通过 i18n_get("cli.help...") 获取；未初始化时返回键名本身，
 * 不会崩溃（便于在没有语言包的场景下继续构建与自检）。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef CLI_HELP_H
#define CLI_HELP_H

/* 概览：分组 + 每命令一行摘要 + help <命令> 提示 */
void cli_help_print_overview(const char *program);

/* 命令详情；subcommand 为 NULL 时输出一级详情。
 * 返回 0 = 找到并输出；-1 = 未知命令/子命令（不打印错误，由调用方处理）。 */
int cli_help_print_command(const char *program, const char *command,
                           const char *subcommand);

/* 全量详情（概览 + 所有命令/子命令详情） */
void cli_help_print_all(const char *program);

/* 查询，供调用方做未知目标校验 */
int cli_help_has_command(const char *command);
int cli_help_has_subcommand(const char *command, const char *subcommand);

#endif /* CLI_HELP_H */
