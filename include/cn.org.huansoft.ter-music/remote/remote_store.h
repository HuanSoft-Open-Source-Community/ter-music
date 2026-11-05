/**
 * @file remote_store.h
 * @brief 前端自有的远程服务器配置（服务器条目与密码密文）
 *
 * 核心不认识远程音乐源：这份数据由**前端**独占读写，放在
 * `<configdir>/remote.xml`，核心配置 `config.xml` 里没有对应段落
 * （config v5 → v6 迁移时，旧段被一次性搬到本文件）。
 *
 * 文件格式沿用旧配置里 `<remote_connections>` 段的元素名，迁移因此是
 * 逐元素原样复制；密码沿用 `config/crypto.c` 的密文格式，迁移过来的
 * 密文可直接解密。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#ifndef REMOTE_STORE_H
#define REMOTE_STORE_H

#include <stddef.h>

#include "remote/remote.h"   /* RemoteConnectionConfig / MAX_REMOTE_CONNECTIONS */

/* 读入 remote.xml。文件不存在按空表处理。
 * @return 条目数（>= 0）；-1 = 文件存在但无法解析 */
int remote_store_load(void);

/* 落盘（临时文件 + rename 原子替换，权限 0600）。
 * @return 0 成功；-1 失败 */
int remote_store_save(void);

/* remote.xml 的完整路径（未 load 时为 NULL） */
const char *remote_store_path(void);

/* ── 服务器条目 ───────────────────────────────────────────────────── */
int remote_store_count(void);
/* 越界返回 NULL。返回的指针内部持有，可跨调用使用到下一次修改为止 */
const RemoteConnectionConfig *remote_store_get(int index);
/* index < 0 追加；否则覆盖该条目。改动只在内存中，需 remote_store_save()
 * 落盘。@return 0 成功；-1 = 越界或表满 */
int remote_store_upsert(int index, const RemoteConnectionConfig *conn);
/* @return 0 成功；-1 = 越界 */
int remote_store_remove(int index);

/* ── 缓存设置（同一文件里的 <cache>） ─────────────────────────────── */
int  remote_store_cache_limit_mb(void);        /* 默认 2048 */
void remote_store_set_cache_limit_mb(int mb);
int  remote_store_prefetch(void);              /* 默认 2，0 = 不预取 */
void remote_store_set_prefetch(int count);

#endif /* REMOTE_STORE_H */
