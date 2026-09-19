/*
 * backend_queue 单测：路径队列的下发、编辑、游标与播放模式
 *
 * 运行：scripts/test/run-unit-tests.sh test_backend_queue
 * 依赖（见 test_backend_queue.srcs）：queue/backend_queue.c + 日志桩
 */

#include "queue/backend_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

static const char *k_payload =
    "{\"entries\":["
    "{\"path\":\"/music/a/01.flac\",\"title\":\"A1\",\"artist\":\"X\",\"album\":\"AL\",\"duration_seconds\":180},"
    "{\"path\":\"/music/a/02.flac\",\"title\":\"A2\",\"artist\":\"X\",\"album\":\"AL\",\"duration_seconds\":181},"
    "{\"path\":\"/music/b/03.flac\",\"title\":\"B1\",\"artist\":\"Y\",\"album\":\"BL\",\"duration_seconds\":182,"
    "\"cue_offset\":12,\"is_cue\":true,\"lyrics_source\":2}"
    "]}";

static void test_set_and_query(void)
{
    bq_init();
    CHECK(bq_set_json(k_payload) == 3, "Queue.Set 接受 3 条");
    CHECK(bq_count() == 3, "计数为 3");
    CHECK(bq_position() == 0, "Set 后游标置 0");

    BackendQueueEntry entry;
    CHECK(bq_entry_at(2, &entry) == 0, "取第 3 条");
    CHECK(strcmp(entry.title, "B1") == 0, "标题解析正确");
    CHECK(entry.cue_offset == 12 && entry.is_cue == 1, "CUE 偏移与标志解析正确");
    CHECK(entry.lyrics_source == 2, "歌词来源解析正确");
    CHECK(entry.duration_seconds == 182, "时长解析正确");

    CHECK(bq_position_of_path("/music/a/02.flac") == 1, "按路径反查位置");
    CHECK(bq_position_of_path("/music/none.flac") == -1, "不存在的路径返回 -1");

    bq_shutdown();
}

static void test_bare_array_and_append(void)
{
    bq_init();
    CHECK(bq_set_json("[{\"path\":\"/m/1.mp3\"},{\"path\":\"/m/2.mp3\"}]") == 2, "裸数组也可下发");
    CHECK(bq_count() == 2, "裸数组计数 2");
    CHECK(bq_append_json("{\"entries\":[{\"path\":\"/m/3.mp3\"}]}") == 1, "Append 追加 1 条");
    CHECK(bq_count() == 3, "追加后计数 3");
    CHECK(bq_insert_after_json(0, "[{\"path\":\"/m/1b.mp3\"}]") == 1, "InsertAfter 插入 1 条");
    CHECK(bq_position_of_path("/m/1b.mp3") == 1, "插入位置正确");
    CHECK(bq_count() == 4, "插入后计数 4");

    /* 非法载荷不应改变队列 */
    CHECK(bq_set_json("not json") == -1, "非法 JSON 被拒绝");
    CHECK(bq_count() == 4, "拒绝后计数不变");
    CHECK(bq_set_json("{\"entries\":[{\"title\":\"无路径\"}]}") == 0, "无路径条目被丢弃");
    CHECK(bq_count() == 0, "置空后计数 0");
    CHECK(bq_position() == -1, "空队列游标为 -1");

    bq_shutdown();
}

static void test_edit(void)
{
    bq_init();
    bq_set_json(k_payload);
    bq_set_position(1);

    CHECK(bq_move_up(1) == 0, "上移成功");
    CHECK(bq_position() == 0, "游标跟随移动");

    BackendQueueEntry entry;
    bq_entry_at(0, &entry);
    CHECK(strcmp(entry.title, "A2") == 0, "上移后顺序正确");

    CHECK(bq_move_down(0) == 0, "下移成功");
    /* 此刻顺序为 [A1, A2, B1]、游标在 1(A2)；删掉游标之前的 A1 后，
     * 游标应前移一格继续指向同一条目 A2 */
    CHECK(bq_remove_at(0) == 0, "删除位置 0 成功");
    CHECK(bq_count() == 2, "删除后计数 2");
    CHECK(bq_position() == 0, "游标前移继续指向同一条目");
    bq_entry_at(bq_position(), &entry);
    CHECK(strcmp(entry.path, "/music/a/02.flac") == 0, "游标仍指向 A2");
    bq_entry_at(1, &entry);
    CHECK(strcmp(entry.path, "/music/b/03.flac") == 0, "B1 顺延到位置 1");

    CHECK(bq_clear() == 0 && bq_count() == 0, "Clear 清空");

    bq_shutdown();
}

static void test_advance_semantics(void)
{
    bq_init();
    bq_set_json(k_payload);
    bq_set_position(0);

    int position = -99;
    CHECK(bq_advance(PLAY_MODE_SEQUENTIAL, &position) == 1 && position == 1, "顺序模式前进到 1");
    CHECK(bq_advance(PLAY_MODE_SEQUENTIAL, &position) == 1 && position == 2, "顺序模式前进到 2");
    CHECK(bq_advance(PLAY_MODE_SEQUENTIAL, &position) == 0 && position == 2,
          "顺序模式到尾停止（游标停在最后一条）");

    bq_set_position(2);
    CHECK(bq_advance(PLAY_MODE_LIST_REPEAT, &position) == 1 && position == 0, "列表循环回绕到 0");

    bq_set_position(1);
    CHECK(bq_advance(PLAY_MODE_SINGLE_REPEAT, &position) == 1 && position == 1, "单曲循环不前进");

    CHECK(bq_rewind(PLAY_MODE_SEQUENTIAL, &position) == 1 && position == 0, "后退到 0");
    bq_set_position(0);
    CHECK(bq_rewind(PLAY_MODE_SEQUENTIAL, &position) == 1 && position == 0, "到头部停住");
    bq_set_position(0);
    CHECK(bq_rewind(PLAY_MODE_LIST_REPEAT, &position) == 1 && position == 2, "列表循环后退回绕");

    bq_set_position(0);
    CHECK(bq_peek_next(PLAY_MODE_SEQUENTIAL, &position) == 1 && position == 1, "peek 不改变游标");
    CHECK(bq_position() == 0, "peek 后游标未变");

    bq_shutdown();
}

static void test_group_modes(void)
{
    bq_init();
    bq_set_json(k_payload);
    bq_set_position(0);

    bq_apply_mode(PLAY_MODE_FOLDER_REPEAT);
    CHECK(bq_count() == 2, "文件夹模式收敛为同目录 2 条");
    CHECK(bq_position() == 0, "文件夹模式保持当前条目在游标处");

    bq_init();
    bq_set_json(k_payload);
    bq_set_position(2);   /* /music/b/03.flac，artist Y / album BL */
    bq_apply_mode(PLAY_MODE_ARTIST_REPEAT);
    CHECK(bq_count() == 1, "艺术家模式收敛为同艺术家 1 条");

    bq_init();
    bq_set_json(k_payload);
    bq_set_position(0);
    bq_apply_mode(PLAY_MODE_ALBUM_REPEAT);
    CHECK(bq_count() == 2, "专辑模式收敛为同专辑 2 条");

    bq_shutdown();
}

/* 组模式必须接受**子目录**：/music/a/01.flac 与 /music/a/sub/02.flac 同组
 * （旧实现 audio/play_queue.c 的 filter_tracks_by_folder 用 "目录前缀 + /"
 * 判定，只比 dirname 会漏掉子目录，属语义漂移） */
static void test_folder_subdirectories(void)
{
    static const char *payload =
        "{\"entries\":["
        "{\"path\":\"/music/a/01.flac\",\"title\":\"A1\",\"album\":\"AL\",\"artist\":\"X\"},"
        "{\"path\":\"/music/a/sub/02.flac\",\"title\":\"A2\",\"album\":\"AL\",\"artist\":\"X\"},"
        "{\"path\":\"/music/b/03.flac\",\"title\":\"B1\",\"album\":\"BL\",\"artist\":\"Y\"}"
        "]}";

    bq_init();
    bq_set_json(payload);
    bq_set_position(0);
    bq_apply_mode(PLAY_MODE_FOLDER_SEQUENTIAL);

    CHECK(bq_count() == 2, "文件夹模式把子目录算作同一组");
    BackendQueueEntry entry;
    bq_entry_at(1, &entry);
    CHECK(strcmp(entry.path, "/music/a/sub/02.flac") == 0, "子目录条目保留在组内");

    bq_shutdown();
}

/* 本地路径校验：核心只播放本地文件，远程 scheme 一律拒绝 */
static void test_local_path_guard(void)
{
    bq_init();
    CHECK(bq_path_is_local("/music/a.mp3"), "普通路径视为本地");
    CHECK(bq_path_is_local("file:///music/a.mp3"), "file:// 视为本地");
    CHECK(!bq_path_is_local("smb://host/share/a.mp3"), "smb:// 不是本地");
    CHECK(!bq_path_is_local("https://example.com/a.mp3"), "https:// 不是本地");
    CHECK(!bq_path_is_local(""), "空路径不是本地");

    CHECK(bq_set_json("[{\"path\":\"smb://host/share/a.mp3\"}]") == -1,
          "Queue.Set 拒绝非本地路径");
    CHECK(bq_count() == 0, "被拒载荷不改变队列");
    CHECK(bq_set_json("[{\"path\":\"/music/a.mp3\"},{\"path\":\"ftp://h/a.mp3\"}]") == -1,
          "混合载荷中只要有一条非本地就整体拒绝");
    CHECK(bq_count() == 0, "混合载荷被拒后队列仍为空");

    bq_shutdown();
}

/* 分块下发：超过 BQ_SET_MAX 的队列必须能 Set + Append 拼起来 */
static void test_chunked_delivery(void)
{
    bq_init();

    size_t capacity = (size_t)BQ_SET_MAX * 128 + 256;
    char *json = malloc(capacity);
    CHECK(json != NULL, "分配分块缓冲");
    if (!json) {
        bq_shutdown();
        return;
    }

    int total = 0;
    for (int chunk_index = 0; chunk_index < 2; chunk_index++) {
        int in_chunk = (chunk_index == 0) ? BQ_SET_MAX : 120;
        size_t pos = 0;
        json[pos++] = '[';
        for (int i = 0; i < in_chunk; i++) {
            int appended = snprintf(json + pos, capacity - pos,
                                    "%s{\"path\":\"/bulk/%04d.mp3\"}", i == 0 ? "" : ",", total + i);
            if (appended <= 0) {
                break;
            }
            pos += (size_t)appended;
        }
        json[pos++] = ']';
        json[pos] = '\0';

        int written = (chunk_index == 0) ? bq_set_json(json) : bq_append_json(json);
        CHECK(written == in_chunk, chunk_index == 0 ? "首块 Set 写入 BQ_SET_MAX 条"
                                                    : "次块 Append 写入剩余条目");
        total += in_chunk;
    }
    CHECK(bq_count() == total, "分块后总条目数正确");
    CHECK(bq_position_of_path("/bulk/0000.mp3") == 0, "首条可反查");
    CHECK(bq_position_of_path("/bulk/0619.mp3") == total - 1, "末条可反查");

    free(json);
    bq_shutdown();
}

static void test_render(void)
{
    bq_init();
    bq_set_json(k_payload);
    bq_set_position(1);

    char json[2048];
    size_t written = bq_render_get(json, sizeof(json), 0, 2);
    CHECK(written > 0 && written < sizeof(json), "Queue.Get 渲染成功");
    CHECK(strstr(json, "\"count\":3") != NULL, "count 字段存在");
    CHECK(strstr(json, "\"current_position\":1") != NULL, "游标字段存在");
    CHECK(strstr(json, "\"revision\":") != NULL, "revision 字段存在");
    CHECK(strstr(json, "null1234") == NULL, "revision 未被写成 null+数字");
    CHECK(strstr(json, "\"rows\":[") != NULL, "rows 数组存在");
    CHECK(strstr(json, "\"path\":\"/music/a/01.flac\"") != NULL, "行内含路径");

    /* 越界分页：返回空页而不是错误 */
    written = bq_render_get(json, sizeof(json), 99, 10);
    CHECK(strstr(json, "\"rows\":[]") != NULL, "越界 offset 得到空页");

    bq_shutdown();
}

int main(void)
{
    test_set_and_query();
    test_bare_array_and_append();
    test_edit();
    test_advance_semantics();
    test_group_modes();
    test_folder_subdirectories();
    test_local_path_guard();
    test_chunked_delivery();
    test_render();

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_backend_queue\n");
        return 0;
    }
    printf("FAIL test_backend_queue\n");
    return 1;
}
