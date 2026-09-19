#ifndef MEDIA_SESSION_H
#define MEDIA_SESSION_H

#include <stdint.h>

void media_session_init(void);
void media_session_tick(void);
void media_session_shutdown(void);
void media_session_notify_seek(uint64_t position_ms);

/* 当前进程是否取得了主总线名 org.mpris.MediaPlayer2.ter_music。
 * daemon 在只拿到 .instance<pid> 回退名时需要据此拒绝以次要实例身份运行。 */
int media_session_has_primary_name(void);

/* 1 = 曾经持有总线名，随后 D-Bus 连接断开（总线消失）。
 * 核心据此判定“再也没有任何前端能联系到我”并退出；从未拿到总线的进程
 * （无会话总线的纯音频/托管场景）恒为 0，行为不变。 */
int media_session_lost(void);

/* 当前实例占用的总线名（未激活时为空串） */
const char *media_session_bus_name(void);

#endif
