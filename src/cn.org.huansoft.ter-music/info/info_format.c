/**
 * @file info_format.c
 * @brief 音频技术信息格式化与 InfoTrack 格式比较（无全局状态）
 *
 * 这些函数只处理 InfoTrack 快照字段，不读写核心音频全局量；本地/远端
 * 前端与 CLI 共用同一套显示规则。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "info/info.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void info_format_audio_fields_from(int sample_rate,
                                   int bit_depth,
                                   int bit_rate,
                                   const char *codec_name,
                                   char *rate, size_t rate_size,
                                   char *depth, size_t depth_size,
                                   char *bitrate, size_t bitrate_size,
                                   char *codec, size_t codec_size)
{
    if (rate && rate_size) {
        if (sample_rate > 0) {
            snprintf(rate, rate_size, "%dHz", sample_rate);
        } else {
            snprintf(rate, rate_size, "--");
        }
    }
    if (depth && depth_size) {
        if (bit_depth > 0) {
            snprintf(depth, depth_size, "%dbit", bit_depth);
        } else {
            snprintf(depth, depth_size, "--");
        }
    }
    if (bitrate && bitrate_size) {
        if (bit_rate > 0) {
            snprintf(bitrate, bitrate_size, "%dkbps", bit_rate / 1000);
        } else {
            snprintf(bitrate, bitrate_size, "--");
        }
    }
    if (codec && codec_size) {
        if (codec_name && codec_name[0] != '\0') {
            char upper[32];
            size_t i;
            for (i = 0; codec_name[i] && i < sizeof(upper) - 1; i++) {
                upper[i] = (i == 0)
                    ? (char)toupper((unsigned char)codec_name[i])
                    : codec_name[i];
            }
            upper[i] = '\0';
            snprintf(codec, codec_size, "%s", upper);
        } else {
            snprintf(codec, codec_size, "--");
        }
    }
}

int info_track_format_changed(const InfoTrack *lhs, const InfoTrack *rhs)
{
    if (!lhs || !rhs) {
        return lhs != rhs;
    }
    return lhs->sample_rate != rhs->sample_rate ||
           lhs->bit_depth != rhs->bit_depth ||
           lhs->bit_rate != rhs->bit_rate ||
           strcmp(lhs->codec, rhs->codec) != 0;
}
