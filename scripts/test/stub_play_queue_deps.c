/*
 * play_queue 契约单测的链接桩
 *
 * `audio/play_queue.c` 依赖配置目录（queue.txt 路径）与 i18n（模式显示名）。
 * 契约测试只验证转发语义，不需要真实配置与语言包，这里给出最小实现。
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

const char *get_config_dir(void)
{
    return "/tmp/tm-play-queue-contract";
}

void ensure_config_dir_exists(void)
{
}

const char *i18n_get(const char *key)
{
    return key ? key : "";
}

/* audio/play_queue.c 在重建后会把“正在出声的路径”同步回队列游标；
 * 契约单测没有音频引擎，桩为空操作即可。 */
void audio_sync_queue_position(void)
{
}
