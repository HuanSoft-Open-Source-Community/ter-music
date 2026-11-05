#ifndef SEARCH_H
#define SEARCH_H

#include "types.h"

/* ── Extern globals ── */
extern SearchState g_search_state;
extern pthread_mutex_t g_search_mutex;

// 启动异步搜索。取消任何正在进行的搜索，然后启动新线程。
void search_async_start(const char *query);

// 请求取消正在进行的搜索（非阻塞，设置取消标志）。
void search_async_cancel(void);

// 如果搜索线程正在运行，返回非零值。
int search_async_is_running(void);

// 交互式搜索提示（启动字符输入循环，适用于主页 playlist 搜索）
// 内部调用 search_async_start。
void search_prompt(void);

// 清除搜索状态并取消任何正在运行的搜索线程（阻塞直到线程退出）。
void search_clear(void);

// 在文本行数组中搜索匹配项（供帮助页面等复用）
// lines: 文本行数组, line_count: 行数, query: 查询字符串
// results: 输出匹配行索引数组, max_results: 数组容量
// 返回匹配行数
int search_lines(const char **lines, int line_count, const char *query,
                 int *results, int max_results);

#endif
