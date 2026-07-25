#ifndef LYRICS_H
#define LYRICS_H

#include <pthread.h>

// 最大歌词行数
#define MAX_LYRIC_LINES 500
// 歌词文本最大长度
#define MAX_LYRIC_TEXT_LEN 256

// 歌词行结构
typedef struct {
    double timestamp;                // 时间戳（秒，包含毫秒）
    char text[MAX_LYRIC_TEXT_LEN];  // 歌词文本
} LyricLine;

// 歌词集合结构
typedef struct {
    LyricLine lines[MAX_LYRIC_LINES];  // 歌词行数组
    int count;                          // 歌词行数
    int current_index;                  // 当前高亮的起始索引
    int highlight_count;                // 当前需要高亮的行数（最多 2 行）
    int has_lyrics;                     // 是否成功加载歌词
    int has_timestamps;                 // 1=有 LRC 时间戳, 0=纯文本嵌入歌词
    int cursor_index;                   // 光标位置索引（编辑模式下使用）
    int source;                         // 实际加载来源: LYRICS_SOURCE_*
    pthread_mutex_t lock;               // 互斥锁
} Lyrics;

// 全局歌词变量
extern Lyrics g_lyrics;

/**
 * 从音频文件路径加载歌词
 * @param audio_path 音频文件路径（如 /path/to/song.mp3）
 * @param lyrics_source 歌词来源偏好: LYRICS_SOURCE_AUTO/EMBEDDED/EXTERNAL
 */
void load_lyrics(const char *audio_path, int lyrics_source);

/**
 * 用指定的歌词源重新加载当前曲目的歌词（用于 Tab 切换）
 * @param new_source 新的歌词来源 (LYRICS_SOURCE_EMBEDDED / LYRICS_SOURCE_EXTERNAL)
 */
void reload_lyrics_with_source(int new_source);

/**
 * 渲染歌词到窗口
 */
void render_lyrics(void);

/**
 * 更新歌词显示（根据当前播放位置）
 */
void update_lyrics_display(void);

/**
 * 清空歌词数据
 */
void clear_lyrics(void);

#endif // LYRICS_H
