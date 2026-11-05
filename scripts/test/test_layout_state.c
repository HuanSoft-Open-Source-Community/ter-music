/**
 * @file test_layout_state.c
 * @brief ui/layout_state.c 的单元测试
 *
 * 覆盖读写、空白裁剪、覆盖写、缺失文件与非法参数。
 *
 * 运行：scripts/test/run-unit-tests.sh test_layout_state
 * 依赖（见同名 .srcs）：src/cn.org.huansoft.ter-music/ui/layout_state.c
 */

#include "ui/layout_spec.h"
#include "ui/layout_state.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);         \
        }                                                                    \
    } while (0)

int main(void)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ter-music-layout-test-%ld", (long)getpid());
    mkdir(dir, 0700);

    char path[320];
    snprintf(path, sizeof(path), "%s/ui-layout", dir);
    char id[LAYOUT_ID_MAX];

    CHECK(layout_state_read(path, id, sizeof(id)) != 0, "missing file fails");
    CHECK(layout_state_write(path, "classic") == 0, "write classic");
    CHECK(layout_state_read(path, id, sizeof(id)) == 0, "read classic");
    CHECK(strcmp(id, "classic") == 0, "classic round-trip");

    CHECK(layout_state_write(path, "vinyl") == 0, "overwrite vinyl");
    CHECK(layout_state_read(path, id, sizeof(id)) == 0, "read vinyl");
    CHECK(strcmp(id, "vinyl") == 0, "overwrite round-trip");

    FILE *file = fopen(path, "w");
    CHECK(file != NULL, "open raw state file");
    if (file) {
        fputs("  spaced-id \r\n", file);
        fclose(file);
    }
    CHECK(layout_state_read(path, id, sizeof(id)) == 0, "read trimmed id");
    CHECK(strcmp(id, "spaced-id") == 0, "whitespace is trimmed");

    CHECK(layout_state_write(path, "") != 0, "empty id rejected");
    CHECK(layout_state_read(path, id, 2) != 0, "short buffer rejects long id");

    unlink(path);
    rmdir(dir);

    if (g_failures == 0) {
        printf("PASS layout_state (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL layout_state (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
