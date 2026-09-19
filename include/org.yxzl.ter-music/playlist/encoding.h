#ifndef ENCODING_H
#define ENCODING_H

#include <stddef.h>

/* ── CUE encoding preference constants ─────────────────────────────── */
/* 取值域定义在 types.h（配置层也要用，内容层只负责实现转换） */

/**
 * @brief Check whether a byte buffer is valid UTF-8.
 *
 * Validates multi-byte sequences against overlong, surrogate, and
 * out-of-range patterns.
 *
 * @param data  Input buffer (may contain null bytes).
 * @param len   Number of bytes to check.
 * @return 1 if the buffer is valid UTF-8, 0 otherwise.
 */
int encoding_is_valid_utf8(const unsigned char *data, size_t len);

/**
 * @brief Convert text from an arbitrary encoding to UTF-8 via iconv.
 *
 * Allocates and returns the converted text.  The caller must free()
 * *out_text on success.
 *
 * @param input      Raw input bytes.
 * @param input_len  Length of input.
 * @param from_code  Source iconv encoding name (e.g. "GBK", "BIG5").
 * @param out_text   On success, a malloc'd null-terminated UTF-8 string.
 * @return 0 on success, -1 on failure (iconv not available, EILSEQ, etc.).
 */
int encoding_convert_to_utf8(const unsigned char *input, size_t input_len,
                              const char *from_code, char **out_text);

/**
 * @brief Auto-detect the encoding of a buffer and convert it to UTF-8.
 *
 * Detection pipeline (in order):
 *   1. Strip UTF-8 BOM (EF BB BF) if present.
 *   2. If the remaining content is valid UTF-8 → fast path, return a
 *      BOM-stripped copy (no iconv involved).
 *   3. If preferred_encoding is not AUTO, try that encoding first.
 *   4. Try the common CJK fallbacks: GB18030 → GBK → BIG5 → Shift-JIS.
 *   5. Last resort: return the raw bytes (with BOM already stripped)
 *      as-is; the caller will see whatever bytes the original file had.
 *
 * On success *out_text is always set to a non-NULL, null-terminated,
 * malloc'd string.  The caller must free() it.
 *
 * @param data                Raw input buffer.
 * @param len                 Input length.
 * @param preferred_encoding  One of CUE_ENCODING_* constants, or
 *                            CUE_ENCODING_AUTO for pure auto-detect.
 * @param out_text            Output UTF-8 string (caller must free).
 * @return 0 on success, -1 on allocation failure.
 */
int encoding_detect_and_convert(const unsigned char *data, size_t len,
                                 int preferred_encoding,
                                 char **out_text);

/**
 * @brief Map a CUE_ENCODING_* constant to its iconv encoding name.
 *
 * @param enc One of CUE_ENCODING_*.
 * @return The iconv encoding name string (e.g. "GBK"), or NULL for
 *         CUE_ENCODING_AUTO / invalid values.
 */
const char *encoding_name_from_enum(int enc);

#endif /* ENCODING_H */
