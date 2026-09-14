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

/* 当前实例占用的总线名（未激活时为空串） */
const char *media_session_bus_name(void);

#endif
