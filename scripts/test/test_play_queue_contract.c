/*
 * play_queue 契约单测：门面转发层必须与后端队列（bq_*）语义逐条一致
 *
 * `audio/play_queue.{c,h}` 现在是后端路径队列的转发层，音频层与信息快照都
 * 经它取数。本测试直接驱动后端队列，再经门面读同一批结果，确认两者没有
 * 各自维护一份口径（游标、导航、编辑、持久化）。
 *
 * 运行：scripts/test/run-unit-tests.sh test_play_queue_contract
 * 依赖（见同名 .srcs）：queue/backend_queue.c + audio/play_queue.c + 日志桩
 */

#include "audio/play_queue.h"
#include "queue/backend_queue.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static const char *k_payload =
    "{\"entries\":["
    "{\"path\":\"/music/a/01.flac\",\"title\":\"A1\",\"artist\":\"X\",\"album\":\"AL\"},"
    "{\"path\":\"/music/a/02.flac\",\"title\":\"A2\",\"artist\":\"X\",\"album\":\"AL\"},"
    "{\"path\":\"/music/b/03.flac\",\"title\":\"B1\",\"artist\":\"Y\",\"album\":\"BL\"}"
    "]}";

static void test_cursor_forwarding(void)
{
    bq_init();
    bq_set_json(k_payload);

    CHECK(play_queue_count() == bq_count(), "count 与后端一致");
    CHECK(play_queue_position() == bq_position(), "游标与后端一致");
    CHECK(play_queue_is_active(&g_play_queue), "有队列时视为活跃");

    play_queue_set_position(2);
    CHECK(bq_position() == 2, "门面置游标作用到后端");
    CHECK(play_queue_count() == 3, "置游标不影响长度");

    bq_shutdown();
}

static void test_navigation_forwarding(void)
{
    bq_init();
    bq_set_json(k_payload);
    bq_set_position(0);

    /* 顺序模式：peek 只探测，advance 才推进 */
    CHECK(play_queue_peek_next(&g_play_queue, PLAY_MODE_SEQUENTIAL) == 1, "peek_next 返回下一位");
    CHECK(bq_position() == 0, "peek_next 不推进游标");
    play_queue_advance(&g_play_queue, PLAY_MODE_SEQUENTIAL);
    CHECK(bq_position() == 1, "advance 推进一位");

    /* peek_prev 与 rewind 对齐 */
    CHECK(play_queue_peek_prev(&g_play_queue, PLAY_MODE_SEQUENTIAL) == 0, "peek_prev 返回上一位");
    play_queue_rewind(&g_play_queue, PLAY_MODE_SEQUENTIAL);
    CHECK(bq_position() == 0, "rewind 退回一位");

    /* 列表循环：到尾回绕 */
    bq_set_position(2);
    CHECK(play_queue_peek_next(&g_play_queue, PLAY_MODE_LIST_REPEAT) == 0, "循环模式 peek 回绕到 0");
    play_queue_advance(&g_play_queue, PLAY_MODE_LIST_REPEAT);
    CHECK(bq_position() == 0, "循环模式 advance 回绕");

    /* 单曲循环：位置不动 */
    bq_set_position(1);
    play_queue_advance(&g_play_queue, PLAY_MODE_SINGLE_REPEAT);
    CHECK(bq_position() == 1, "单曲循环不前进");

    /* 顺序模式到尾：停在最后一条且报告到头 */
    bq_set_position(2);
    {
        int next = play_queue_peek_next(&g_play_queue, PLAY_MODE_SEQUENTIAL);
        CHECK(next == -1, "顺序模式到尾 peek 返回 -1");
        play_queue_advance(&g_play_queue, PLAY_MODE_SEQUENTIAL);
        CHECK(bq_position() == 2, "顺序模式到尾游标停在最后");
    }

    bq_shutdown();
}

static void test_edit_forwarding(void)
{
    bq_init();
    bq_set_json(k_payload);
    bq_set_position(1);

    CHECK(play_queue_move_up(&g_play_queue, 1) == 0, "上移成功");
    CHECK(play_queue_position() == 0, "游标跟随上移");

    BackendQueueEntry entry;
    CHECK(bq_entry_at(0, &entry) == 0 && strcmp(entry.title, "A2") == 0, "顺序已交换");

    CHECK(play_queue_move_down(&g_play_queue, 0) == 0, "下移成功");
    CHECK(play_queue_remove_at(&g_play_queue, 0) == 0, "删除成功");
    CHECK(play_queue_count() == 2, "删除后长度 2");
    CHECK(bq_position_of_path("/music/a/02.flac") == 0, "剩余顺序正确");

    play_queue_clear(&g_play_queue);
    CHECK(play_queue_count() == 0, "清空后长度 0");
    CHECK(play_queue_position() == -1, "清空后游标 -1");
    CHECK(!play_queue_is_active(&g_play_queue), "清空后不活跃");

    bq_shutdown();
}

static void test_mirror_sync(void)
{
    bq_init();
    bq_set_json(k_payload);

    /* 无解析器时镜像只对齐长度与游标（界面在拿到解析器前也能安全渲染） */
    play_queue_sync_mirror(NULL, NULL);
    CHECK(g_play_queue.count == 3, "镜像长度对齐");
    CHECK(g_play_queue.current_position == bq_position(), "镜像游标对齐");
    CHECK(g_play_queue.indices[0] == -1, "无解析器时下标置 -1");

    bq_shutdown();
}

int main(void)
{
    test_cursor_forwarding();
    test_navigation_forwarding();
    test_edit_forwarding();
    test_mirror_sync();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_play_queue_contract\n");
        return 0;
    }
    printf("FAIL test_play_queue_contract\n");
    return 1;
}
