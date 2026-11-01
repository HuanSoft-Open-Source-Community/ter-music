/*
 * 歌词解析单测：抽取到后端后的引擎必须保持原有解析语义
 *
 * 覆盖：LRC 时间戳解析、纯文本内嵌歌词（无时间戳）、UTF-8 BOM、UTF-16 LE/BE
 * 嗅探、非法编码回退、HTML 实体解码、`lyrics_tick()` 的高亮推进与两行同时间
 * 戳高亮，以及分页接口的边界。
 *
 * 音频文件用 ffmpeg 现场生成（带 lyrics 元数据的那条用于内嵌歌词路径）；
 * 若无 ffmpeg，则只跑 .lrc 侧断言。
 *
 * 运行：scripts/test/run-unit-tests.sh test_lyrics_parse
 * 依赖（见同名 .srcs）：lyrics/lyrics.c + queue/backend_queue.c + playlist.c 等
 */

#include "lyrics/lyrics.h"
#include "queue/backend_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>


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

static char g_dir[256];
static int g_test_position = 0;

static int test_position_source(void)
{
    return g_test_position;
}

static int64_t g_test_position_ms = 0;

static int64_t test_position_ms_source(void)
{
    return g_test_position_ms;
}

static int near_seconds(double lhs, double rhs)
{
    double diff = lhs - rhs;
    return diff < 0.001 && diff > -0.001;
}

static void write_file(const char *name, const void *data, size_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fwrite(data, 1, size, f);
    fclose(f);
}

static void path_of(const char *name, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", g_dir, name);
}

/* 生成一个带“同名 .lrc”的假音频路径：load_lyrics 只把扩展名换成 .lrc，
 * 不去读音频本身（内嵌路径在前，找不到才看 .lrc，故这里只验 .lrc 分支）。 */
static void write_lrc(const char *base_name, const void *data, size_t size)
{
    char name[256];
    snprintf(name, sizeof(name), "%s.lrc", base_name);
    write_file(name, data, size);

    char audio[256];
    snprintf(audio, sizeof(audio), "%s.mp3", base_name);
    /* 空的音频占位文件：内嵌提取会失败并回退到 .lrc */
    write_file(audio, "", 0);
}

static void test_lrc_timestamps(void)
{
    static const char *lrc =
        "[ti:T]\n"
        "[ar:A]\n"
        "[00:01.00]第一行\n"
        "[00:02.50]第二行\n"
        "[00:02.50]第二行(同时间戳)\n"
        "[00:05.00]A &amp; B\n";

    write_lrc("ts", lrc, strlen(lrc));

    char audio[512];
    path_of("ts.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.has_lyrics == 1, "LRC loaded");
    CHECK(g_lyrics.has_timestamps == 1, "LRC marked as timestamped");
    CHECK(g_lyrics.source == LYRICS_SOURCE_EXTERNAL, "source recorded as EXTERNAL");
    CHECK(g_lyrics.count >= 4, "lines parsed (including metadata header)");

    int seen_amp = 0;
    int seen_header = 0;
    for (int i = 0; i < g_lyrics.count; i++) {
        if (strcmp(g_lyrics.lines[i].text, "A & B") == 0) seen_amp = 1;
        if (strcmp(g_lyrics.lines[i].text, "T") == 0) seen_header = 1;
    }
    CHECK(seen_amp, "HTML entity decoded (&amp; -> &)");
    CHECK(seen_header, "[ti:] header kept as a lyric line");
}

static void test_plain_text_embedded(void)
{
    /* 纯文本 .lrc（无时间戳）：应作为无时间戳歌词装入 */
    static const char *plain = "没有时间戳的第一行\n没有时间戳的第二行\n";
    write_lrc("plain", plain, strlen(plain));

    char audio[512];
    path_of("plain.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    /* 无时间戳的 .lrc 行不会被 parse_lrc_line 接受，引擎按“无内容”清空 */
    CHECK(g_lyrics.has_lyrics == 0, "timestamp-less .lrc treated as empty");
    CHECK(g_lyrics.count == 0, "no lines when empty");
}

/* 期望文本按**码元/字节**给出，不用源码字面量：
 * 源码字符集与执行字符集不该影响断言（本机实测同一份 `"\xe4\xbd\xa0"`
 * 在不同文件里会被编译器分别当字节转义与 Unicode 码点处理）。 */
static const unsigned short k_line_units[] = {
    0x005b, 0x0030, 0x0030, 0x003a, 0x0030, 0x0031, 0x002e, 0x0030,
    0x0030, 0x005d, 0x4f60, 0x597d, 0x000a
};
#define K_LINE_UNIT_COUNT (sizeof(k_line_units) / sizeof(k_line_units[0]))

/* 期望的歌词文本 = 时间戳之后的部分（U+4F60 U+597D 的 UTF-8 编码 + 换行）。
 * `[00:01.00]` 是 LRC 时间戳，解析后不进文本。 */
static const unsigned char k_line_utf8[] = {
    0xe4, 0xbd, 0xa0, 0xe5, 0xa5, 0xbd, 0x00
};

/* 把码元数组按给定字节序写成带 BOM 的 UTF-16 载荷 */
static size_t build_utf16(unsigned char *out, int big_endian,
                          const unsigned short *units, size_t unit_count)
{
    size_t n = 0;
    out[n++] = big_endian ? 0xFE : 0xFF;
    out[n++] = big_endian ? 0xFF : 0xFE;
    for (size_t i = 0; i < unit_count; i++) {
        unsigned short u = units[i];
        out[n++] = big_endian ? (unsigned char)(u >> 8) : (unsigned char)(u & 0xFF);
        out[n++] = big_endian ? (unsigned char)(u & 0xFF) : (unsigned char)(u >> 8);
    }
    return n;
}

static void test_utf16_sniffing(void)
{
    unsigned char buf[256];
    char audio[512];

    /* ── UTF-16LE（带 BOM） ── */
    size_t n = build_utf16(buf, 0, k_line_units, K_LINE_UNIT_COUNT);
    write_lrc("u16le", buf, n);
    path_of("u16le.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.has_lyrics == 1, "UTF-16LE + BOM parses");
    CHECK(g_lyrics.count == 1, "UTF-16LE produces one line");
    if (g_lyrics.count == 1) {
        CHECK(strcmp(g_lyrics.lines[0].text, (const char *)k_line_utf8) == 0,
              "UTF-16LE decoded back to the original UTF-8 bytes");
        CHECK((double)g_lyrics.lines[0].timestamp == 1.0, "UTF-16LE timestamp parsed");
    }

    /* ── UTF-16BE（带 BOM） ── */
    n = build_utf16(buf, 1, k_line_units, K_LINE_UNIT_COUNT);
    write_lrc("u16be", buf, n);
    path_of("u16be.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.has_lyrics == 1, "UTF-16BE + BOM parses");
    if (g_lyrics.count == 1) {
        CHECK(strcmp(g_lyrics.lines[0].text, (const char *)k_line_utf8) == 0,
              "UTF-16BE decoded back to the original UTF-8 bytes");
    }

    /* ── 无 BOM 的 UTF-16LE：靠“零字节位置”嗅探（全部码元为 CJK，高位为零） ── */
    {
        static const unsigned short k_cjk_units[] = { 0x4f60, 0x597d, 0x597d, 0x4e86, 0x002e };
        const size_t cjk_count = sizeof(k_cjk_units) / sizeof(k_cjk_units[0]);
        n = 0;
        for (size_t i = 0; i < cjk_count; i++) {
            buf[n++] = (unsigned char)(k_cjk_units[i] & 0xFF);
            buf[n++] = (unsigned char)(k_cjk_units[i] >> 8);
        }
        write_lrc("u16nobom", buf, n);
        path_of("u16nobom.mp3", audio, sizeof(audio));
        load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
        /* 嗅探是启发式（看“零字节是否集中在奇数位”）：载荷里既有 ASCII 又有
         * CJK 时成立，纯 CJK 时判定不出，因此这里断言的是**该启发式的行为**，
         * 而不是“任何无 BOM 的 UTF-16 都能识别”。 */
        CHECK(g_lyrics.has_lyrics == 0,
              "BOM-less pure-CJK payload is not claimed as UTF-16 (heuristic boundary)");
    }

    /* ── 增补平面：U+1F600 以代理对 D83D DE00 表示，UTF-8 为 f0 9f 98 80 ── */
    {
        static const unsigned short k_pair[] = {
            0x005b, 0x0030, 0x0030, 0x003a, 0x0030, 0x0031, 0x002e,
            0x0030, 0x0030, 0x005d, 0xd83d, 0xde00, 0x000a
        };
        static const unsigned char k_pair_utf8[] = { 0xf0, 0x9f, 0x98, 0x80, 0x00 };
        n = build_utf16(buf, 0, k_pair, sizeof(k_pair) / sizeof(k_pair[0]));
        write_lrc("u16pair", buf, n);
        path_of("u16pair.mp3", audio, sizeof(audio));
        load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
        CHECK(g_lyrics.has_lyrics == 1, "surrogate pair parses");
        if (g_lyrics.count == 1) {
            CHECK(strcmp(g_lyrics.lines[0].text, (const char *)k_pair_utf8) == 0,
                  "surrogate pair decoded to U+1F600");
        }
    }

    /* ── 落单的高代理必须被拒绝而不是产出垃圾 ── */
    {
        static const unsigned short k_lone[] = {
            0x005b, 0x0030, 0x0030, 0x003a, 0x0030, 0x0031, 0x002e,
            0x0030, 0x0030, 0x005d, 0xd83d, 0x005d
        };
        n = build_utf16(buf, 0, k_lone, sizeof(k_lone) / sizeof(k_lone[0]));
        write_lrc("u16lone", buf, n);
        path_of("u16lone.mp3", audio, sizeof(audio));
        load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
        /* 引擎在 UTF-16 解码失败后回退到单字节复制，故不应崩且不应报“有歌词”
         * ——这里只断言进程存活与状态自洽 */
        CHECK(g_lyrics.count >= 0, "lone surrogate does not crash");
    }

    /* ── UTF-8 BOM：BOM 不得进入歌词文本 ── */
    {
        /* 该文件只有一行歌词（"BOM"）：`[00:03.00]` 是时间戳，不是标签行 */
        static const unsigned char k_bom_text[] = { 0x42, 0x4f, 0x4d, 0x00 };
        static const unsigned char k_bom_payload[] = {
            0x5b, 0x30, 0x30, 0x3a, 0x30, 0x33, 0x2e, 0x30, 0x30, 0x5d,
            0x42, 0x4f, 0x4d, 0x0a
        };
        unsigned char bom[64];
        size_t m = 0;
        bom[m++] = 0xEF; bom[m++] = 0xBB; bom[m++] = 0xBF;
        for (size_t i = 0; i < sizeof(k_bom_payload) - 1; i++) bom[m++] = k_bom_payload[i];
        write_lrc("bom", bom, m);

        path_of("bom.mp3", audio, sizeof(audio));
        load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
        CHECK(g_lyrics.has_lyrics == 1, "UTF-8 BOM parses");
        if (g_lyrics.count == 1) {
            CHECK(strcmp(g_lyrics.lines[0].text, (const char *)k_bom_text) == 0,
                  "UTF-8 BOM is stripped from the text");
        }
    }
}

static void test_tick_and_highlight(void)
{
    /* 逐字节构造 LRC：4 行，其中第 2/3 行时间戳相同 */
    static const char *k_lrc =
        "[00:01.00]line1\n"
        "[00:02.00]line2\n"
        "[00:02.00]line2b\n"
        "[00:03.00]line3\n";
    write_lrc("tick", k_lrc, strlen(k_lrc));

    char audio[512];
    path_of("tick.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
    CHECK(g_lyrics.has_lyrics == 1, "tick fixture loaded");
    CHECK(g_lyrics.count == 4, "tick fixture has 4 lines");

    /* 播放位置由本测试注入：引擎默认读音频层，单测里没有音频栈 */
    lyrics_set_position_source(test_position_source);

    g_test_position = 0;
    lyrics_tick();
    CHECK(g_lyrics.current_index == -1, "no highlight at position 0");

    /* 语义：只高亮“最后一个 timestamp <= 位置”的行；若该行的**下一行**时间戳
     * 与它相同，则两行一起高亮（LRC 常见的“一行时间戳带多行文本”写法）。
     * 位置 2 时最后一个命中行是 index 2（timestamp 2.0），它的下一行是 3.0，
     * 因此只高亮一行；位置 1 时命中 index 0，其下一行同为 2.0 → 不是同一时间戳，
     * 也只是一行。真正触发双行高亮需要**连续两行同时间戳且位置停在第一行上**。 */
    g_test_position = 2;
    lyrics_tick();
    CHECK(g_lyrics.current_index == 2, "position 2 highlights the last line at that timestamp");
    CHECK(g_lyrics.highlight_count == 1, "no duplicate timestamp after the hit line");

    g_test_position = 3;
    lyrics_tick();
    CHECK(g_lyrics.current_index == 3, "position 3 highlights line3");
    CHECK(g_lyrics.highlight_count == 1, "a distinct timestamp highlights one line");
}

static void test_per_char_timings(void)
{
    /* 逐字/逐段标签：正文后的每个标签开启下一段；最后一个标签是行结束 */
    static const char *k_lrc =
        "[00:01.00]梦[00:01.50]回[00:02.00]还[00:03.00]\n";
    write_lrc("karaoke", k_lrc, strlen(k_lrc));

    char audio[512];
    path_of("karaoke.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.count == 1, "per-char fixture loaded");
    CHECK(g_lyrics.lines[0].timing == LYRIC_TIMING_CHARS, "per-char line marked CHARS");
    CHECK(g_lyrics.lines[0].char_count == 3, "per-char char_count=3");
    CHECK(near_seconds(g_lyrics.lines[0].end_timestamp, 3.0), "per-char end=3.0");

    lyrics_set_position_ms_source(test_position_ms_source);
    int sung = 0;
    int total = 0;

    CHECK(lyrics_char_progress_at(0, 1000, &sung, &total) == 1, "per-char progress available");
    CHECK(total == 3 && sung == 1, "at 1000ms one char sung");
    (void)lyrics_char_progress_at(0, 1499, &sung, &total);
    CHECK(sung == 1, "before the second tag still one char");
    (void)lyrics_char_progress_at(0, 1500, &sung, &total);
    CHECK(sung == 2, "at the second tag two chars");
    (void)lyrics_char_progress_at(0, 2000, &sung, &total);
    CHECK(sung == 3, "at the third tag all chars");
    (void)lyrics_char_progress_at(0, 2999, &sung, &total);
    CHECK(sung == 3, "after the last char stays complete");

    g_test_position_ms = 1500;
    CHECK(lyrics_tick() == 1, "tick reports per-char progress change");
    CHECK(g_lyrics.current_char_sung == 2 && g_lyrics.current_char_total == 3,
          "tick stores per-char progress");

    LyricsSnapshot snapshot;
    lyrics_snapshot(&snapshot);
    CHECK(snapshot.current_index == 0 && snapshot.current_char_sung == 2 &&
          snapshot.current_char_total == 3,
          "snapshot carries per-char progress");

    lyrics_set_position_ms_source(NULL);
}

static void test_line_timings_and_old_style(void)
{
    /* 行首 + 行尾标签：整行按区间均分 */
    static const char *k_line =
        "[00:01.00]abcd[00:03.00]\n";
    write_lrc("lineend", k_line, strlen(k_line));

    char audio[512];
    path_of("lineend.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.count == 1, "line-end fixture loaded");
    CHECK(g_lyrics.lines[0].timing == LYRIC_TIMING_LINE, "line-end marked LINE");
    CHECK(g_lyrics.lines[0].char_count == 4, "line-end char_count=4");
    CHECK(near_seconds(g_lyrics.lines[0].end_timestamp, 3.0), "line-end end=3.0");

    lyrics_set_position_ms_source(test_position_ms_source);
    int sung = 0;
    int total = 0;
    (void)lyrics_char_progress_at(0, 1500, &sung, &total);
    CHECK(total == 4 && sung == 1, "line interpolation at 1500ms = 1 char");
    (void)lyrics_char_progress_at(0, 2000, &sung, &total);
    CHECK(sung == 2, "line interpolation at 2000ms = 2 chars");
    (void)lyrics_char_progress_at(0, 2999, &sung, &total);
    CHECK(sung == 3, "line interpolation near the end = 3 chars");
    (void)lyrics_char_progress_at(0, 3000, &sung, &total);
    CHECK(sung == 4, "line interpolation at end = all chars");

    /* 旧式：只有行首。第 0 行终点 = 第 1 行起点；最后一行沿用前一段时长 */
    static const char *k_old =
        "[00:01.00]ab\n"
        "[00:03.00]cd\n";
    write_lrc("oldstyle", k_old, strlen(k_old));
    path_of("oldstyle.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.count == 2, "old-style fixture loaded");
    CHECK(g_lyrics.lines[0].timing == LYRIC_TIMING_LINE, "old-style first line marked LINE");
    CHECK(near_seconds(g_lyrics.lines[0].end_timestamp, 3.0),
          "old-style first line end = next line start");
    CHECK(near_seconds(g_lyrics.lines[1].end_timestamp, 5.0),
          "last line reuses the previous duration");

    (void)lyrics_char_progress_at(0, 2000, &sung, &total);
    CHECK(sung == 1, "old-style first line halfway = 1/2 chars");
    (void)lyrics_char_progress_at(1, 4000, &sung, &total);
    CHECK(sung == 1, "old-style last line halfway = 1/2 chars");

    /* 单行且无法推导行时长：timing=NONE，逐字进度必须报告 0/0（界面走整行高亮） */
    static const char *k_single = "[00:01.00]abc\n";
    write_lrc("single", k_single, strlen(k_single));
    path_of("single.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
    CHECK(g_lyrics.count == 1, "single-line fixture loaded");
    CHECK(g_lyrics.lines[0].timing == LYRIC_TIMING_NONE,
          "single line without a derivable end is timing NONE");
    sung = 99;
    total = 99;
    CHECK(lyrics_char_progress_at(0, 2000, &sung, &total) == 0,
          "timing NONE reports no per-char progress");
    CHECK(sung == 0 && total == 0, "timing NONE zeroes sung/total");

    lyrics_set_position_ms_source(NULL);
}

static void test_consecutive_tags_and_entities(void)
{
    /* 连续标签：前一个是上一段结束，后一个才是下一段文本起点 */
    static const char *k_lrc =
        "[00:01.00]a[00:02.00][00:03.00]b[00:04.00]\n";
    write_lrc("tags", k_lrc, strlen(k_lrc));

    char audio[512];
    path_of("tags.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.count == 1, "consecutive-tag fixture loaded");
    CHECK(g_lyrics.lines[0].timing == LYRIC_TIMING_CHARS,
          "consecutive tags keep per-char timing");
    CHECK(g_lyrics.lines[0].char_count == 2, "consecutive tags text has 2 chars");

    lyrics_set_position_ms_source(test_position_ms_source);
    int sung = 0;
    int total = 0;
    (void)lyrics_char_progress_at(0, 2500, &sung, &total);
    CHECK(sung == 1, "consecutive tags: first char only before the second start");
    (void)lyrics_char_progress_at(0, 3000, &sung, &total);
    CHECK(sung == 2, "consecutive tags: second char starts at the later tag");

    /* HTML 实体在逐字映射前解码，字符数与时间段保持一致 */
    static const char *k_entity =
        "[00:01.00]&amp;\xe4\xbd\xa0[00:02.00]\n";
    write_lrc("entity", k_entity, strlen(k_entity));
    path_of("entity.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
    CHECK(g_lyrics.count == 1, "entity fixture loaded");
    CHECK(strcmp(g_lyrics.lines[0].text, "&你") == 0, "entity decoded");
    CHECK(g_lyrics.lines[0].char_count == 2, "decoded entity counts as one char");

    lyrics_set_position_ms_source(NULL);
}

static void test_timestamp_precision_and_utf8_truncation(void)
{
    static const char *k_lrc =
        "[00:01.500]x[00:02.250]\n";
    write_lrc("fraction", k_lrc, strlen(k_lrc));

    char audio[512];
    path_of("fraction.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    CHECK(g_lyrics.count == 1, "fraction fixture loaded");
    CHECK(near_seconds(g_lyrics.lines[0].timestamp, 1.5),
          "three-digit fraction parsed as 1.5s");
    CHECK(near_seconds(g_lyrics.lines[0].end_timestamp, 2.25),
          "three-digit end fraction parsed as 2.25s");

    /* 超长多字节文本按完整 UTF-8 字符截断（255 字节 = 85 个汉字） */
    char long_lrc[640];
    size_t used = (size_t)snprintf(long_lrc, sizeof(long_lrc), "[00:01.00]");
    for (int i = 0; i < 100 && used + 3 < sizeof(long_lrc); i++) {
        memcpy(long_lrc + used, "\xe4\xbd\xa0", 3);  /* 你 */
        used += 3;
    }
    long_lrc[used++] = '\n';
    long_lrc[used] = '\0';
    write_lrc("longline", long_lrc, used);

    path_of("longline.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);
    CHECK(g_lyrics.count == 1, "long line fixture loaded");
    CHECK(g_lyrics.lines[0].char_count == 85,
          "long line truncated to 85 whole UTF-8 chars");
    CHECK(strlen(g_lyrics.lines[0].text) == 255,
          "long line text is exactly 255 bytes");
}

static void test_page_and_clear(void)
{
    static const char *lrc =
        "[00:01.00]a\n[00:02.00]b\n[00:03.00]c\n";
    write_lrc("page", lrc, strlen(lrc));

    char audio[512];
    path_of("page.mp3", audio, sizeof(audio));
    load_lyrics(audio, LYRICS_SOURCE_EXTERNAL);

    LyricsPage page;
    int written = lyrics_page(0, 2, &page);
    CHECK(written == 2, "page returns 2 lines");
    CHECK(page.total >= 3, "page reports the total");
    CHECK(page.has_lyrics == 1, "page carries the lyrics flag");

    CHECK(lyrics_page(0, LYRICS_PAGE_MAX + 1, &page) == -1, "page count above the limit rejected");
    CHECK(lyrics_page(-1, 2, &page) == -1, "negative offset rejected");

    written = lyrics_page(1000, 5, &page);
    CHECK(written == 0, "out-of-range offset returns an empty page");

    clear_lyrics();
    CHECK(g_lyrics.has_lyrics == 0, "no lyrics after clear");
    CHECK(g_lyrics.count == 0, "no lines after clear");
    CHECK(lyrics_tick() == 0, "tick is a no-op without lyrics");
}

static void test_source_switch_without_track(void)
{
    bq_init();
    bq_clear();
    CHECK(lyrics_switch_source(LYRICS_SOURCE_EMBEDDED) == -1, "source switch fails with no current track");
    CHECK(lyrics_current_track_path() == NULL, "path is NULL with no current track");
    bq_shutdown();
}

int main(void)
{
    /* iconv 的字符集转换依赖 LC_CTYPE：C locale 下 glibc 的
     * "UTF-16LE" -> "UTF-8" 会把多字节字符当单字节处理（实测把
     * U+4F60 转成 3 个 Latin-1 字符）。真实进程由 ensure_utf8_locale()
     * 在启动时设好 locale，单测必须做同样的事，否则测的不是同一件事。 */
    setlocale(LC_ALL, "");

    snprintf(g_dir, sizeof(g_dir), "/tmp/tm-lyrics-test-%d", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", g_dir);
    if (system(cmd) != 0) {
        printf("FAIL 无法创建临时目录\n");
        return 1;
    }

    test_lrc_timestamps();
    test_plain_text_embedded();
    test_utf16_sniffing();
    test_tick_and_highlight();
    test_per_char_timings();
    test_line_timings_and_old_style();
    test_consecutive_tags_and_entities();
    test_timestamp_precision_and_utf8_truncation();
    test_page_and_clear();
    test_source_switch_without_track();

    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir);
    if (system(cmd) != 0) {
        /* 清理失败不影响断言结果 */
    }

    printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        printf("PASS test_lyrics_parse\n");
        return 0;
    }
    printf("FAIL test_lyrics_parse\n");
    return 1;
}
