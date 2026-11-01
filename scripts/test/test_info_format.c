/**
 * @file test_info_format.c
 * @brief info/info_format.c 的纯函数单元测试
 */

#include "info/info.h"

#include <stdio.h>
#include <string.h>

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

static void check_format(void)
{
    char rate[32], depth[32], bitrate[32], codec[32];

    info_format_audio_fields_from(44100, 16, 1149000, "flac",
                                   rate, sizeof(rate),
                                   depth, sizeof(depth),
                                   bitrate, sizeof(bitrate),
                                   codec, sizeof(codec));
    CHECK(strcmp(rate, "44100Hz") == 0, "sample rate formatting");
    CHECK(strcmp(depth, "16bit") == 0, "bit depth formatting");
    CHECK(strcmp(bitrate, "1149kbps") == 0, "bitrate formatting");
    CHECK(strcmp(codec, "Flac") == 0, "codec capitalization");

    info_format_audio_fields_from(0, 0, 0, "",
                                   rate, sizeof(rate),
                                   depth, sizeof(depth),
                                   bitrate, sizeof(bitrate),
                                   codec, sizeof(codec));
    CHECK(strcmp(rate, "--") == 0, "unknown rate placeholder");
    CHECK(strcmp(depth, "--") == 0, "unknown depth placeholder");
    CHECK(strcmp(bitrate, "--") == 0, "unknown bitrate placeholder");
    CHECK(strcmp(codec, "--") == 0, "unknown codec placeholder");

    info_format_audio_fields_from(48000, 24, 0, NULL,
                                   rate, sizeof(rate),
                                   depth, sizeof(depth),
                                   bitrate, sizeof(bitrate),
                                   codec, sizeof(codec));
    CHECK(strcmp(rate, "48000Hz") == 0, "24-bit sample rate");
    CHECK(strcmp(depth, "24bit") == 0, "24-bit depth");
    CHECK(strcmp(bitrate, "--") == 0, "missing bitrate");
    CHECK(strcmp(codec, "--") == 0, "NULL codec");

    /* 小缓冲：只要求不越界/不崩溃，内容允许截断。 */
    char tiny[4] = {'x', 'x', 'x', 'x'};
    info_format_audio_fields_from(44100, 16, 1149000, "flac",
                                   tiny, sizeof(tiny), NULL, 0, NULL, 0, NULL, 0);
    CHECK(tiny[3] == '\0' && strncmp(tiny, "441", 3) == 0, "tiny output buffer");
}

static void check_changed(void)
{
    InfoTrack a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    CHECK(info_track_format_changed(&a, &b) == 0, "same zero format");

    b.sample_rate = 44100;
    CHECK(info_track_format_changed(&a, &b) == 1, "sample rate change");
    a.sample_rate = 44100;
    CHECK(info_track_format_changed(&a, &b) == 0, "same sample rate");

    b.bit_depth = 16;
    CHECK(info_track_format_changed(&a, &b) == 1, "bit depth change");
    a.bit_depth = 16;

    b.bit_rate = 1149000;
    CHECK(info_track_format_changed(&a, &b) == 1, "bitrate change");
    a.bit_rate = 1149000;

    snprintf(b.codec, sizeof(b.codec), "flac");
    CHECK(info_track_format_changed(&a, &b) == 1, "codec change");
    snprintf(a.codec, sizeof(a.codec), "flac");
    CHECK(info_track_format_changed(&a, &b) == 0, "same format");

    CHECK(info_track_format_changed(NULL, NULL) == 0, "NULL pair equal");
    CHECK(info_track_format_changed(&a, NULL) == 1, "NULL mismatch");
}

int main(void)
{
    check_format();
    check_changed();

    if (g_failures == 0) {
        printf("PASS info_format (%d checks)\n", g_checks);
        return 0;
    }
    printf("FAIL info_format (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
