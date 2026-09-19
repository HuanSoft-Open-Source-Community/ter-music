#include "types.h"
#include "playlist/encoding.h"

#include <iconv.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────
 * UTF-8 validation
 * ────────────────────────────────────────────────────────────────────── */
int encoding_is_valid_utf8(const unsigned char *data, size_t len) {
    size_t i = 0;

    while (i < len) {
        unsigned char c = data[i];
        if (c < 0x80) {
            i++;
            continue;
        }

        if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len || (data[i + 1] & 0xC0) != 0x80 || c < 0xC2) {
                return 0;
            }
            i += 2;
            continue;
        }

        if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len ||
                (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xE0 && data[i + 1] < 0xA0) {
                return 0;
            }
            if (c == 0xED && data[i + 1] >= 0xA0) {
                return 0;
            }
            i += 3;
            continue;
        }

        if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len ||
                (data[i + 1] & 0xC0) != 0x80 ||
                (data[i + 2] & 0xC0) != 0x80 ||
                (data[i + 3] & 0xC0) != 0x80) {
                return 0;
            }
            if (c == 0xF0 && data[i + 1] < 0x90) {
                return 0;
            }
            if (c > 0xF4 || (c == 0xF4 && data[i + 1] >= 0x90)) {
                return 0;
            }
            i += 4;
            continue;
        }

        return 0;
    }

    return 1;
}

/* ──────────────────────────────────────────────────────────────────────
 * iconv wrapper
 * ────────────────────────────────────────────────────────────────────── */
int encoding_convert_to_utf8(const unsigned char *input,
                              size_t input_len,
                              const char *from_code,
                              char **out_text) {
    if (!input || !from_code || !out_text) {
        return -1;
    }

    iconv_t cd = iconv_open("UTF-8", from_code);
    if (cd == (iconv_t)-1) {
        return -1;
    }

    size_t out_cap = input_len * 4 + 16;
    if (out_cap < 64) {
        out_cap = 64;
    }

    char *output = malloc(out_cap);
    if (!output) {
        iconv_close(cd);
        return -1;
    }

    char *out_ptr = output;
    size_t out_left = out_cap - 1;
    char *in_ptr = (char *)input;
    size_t in_left = input_len;

    while (in_left > 0) {
        size_t ret = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        if (ret != (size_t)-1) {
            continue;
        }

        if (errno == E2BIG) {
            size_t used = (size_t)(out_ptr - output);
            size_t new_cap = out_cap * 2;
            char *grown = realloc(output, new_cap);
            if (!grown) {
                free(output);
                iconv_close(cd);
                return -1;
            }
            output = grown;
            out_ptr = output + used;
            out_left = new_cap - used - 1;
            out_cap = new_cap;
            continue;
        }

        /* EILSEQ, EINVAL — stop, don't keep partial output */
        free(output);
        iconv_close(cd);
        return -1;
    }

    *out_ptr = '\0';
    *out_text = output;
    iconv_close(cd);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────
 * Enum ↔ iconv name
 * ────────────────────────────────────────────────────────────────────── */
const char *encoding_name_from_enum(int enc) {
    switch (enc) {
        case CUE_ENCODING_UTF8:      return "UTF-8";
        case CUE_ENCODING_GB18030:   return "GB18030";
        case CUE_ENCODING_GBK:       return "GBK";
        case CUE_ENCODING_BIG5:      return "BIG5";
        case CUE_ENCODING_SHIFT_JIS: return "SHIFT_JIS";
        default:                     return NULL;
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * Auto-detect & convert pipeline
 * ────────────────────────────────────────────────────────────────────── */
int encoding_detect_and_convert(const unsigned char *data, size_t len,
                                 int preferred_encoding,
                                 char **out_text) {
    if (!data || !out_text) {
        return -1;
    }

    *out_text = NULL;

    /* ── 1. Strip UTF-8 BOM if present ── */
    const unsigned char *content = data;
    size_t content_len = len;
    if (len >= 3 &&
        data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        content = data + 3;
        content_len = len - 3;
    }

    if (content_len == 0) {
        *out_text = calloc(1, 1);
        return *out_text ? 0 : -1;
    }

    /* ── 2. Fast path: already valid UTF-8 ── */
    if (encoding_is_valid_utf8(content, content_len)) {
        *out_text = strdup((const char *)content);
        return *out_text ? 0 : -1;
    }

    /* ── 3. If user has a preferred (non-AUTO) encoding, try it first ── */
    if (preferred_encoding > CUE_ENCODING_AUTO &&
        preferred_encoding < CUE_ENCODING_COUNT) {
        const char *enc_name = encoding_name_from_enum(preferred_encoding);
        if (enc_name &&
            encoding_convert_to_utf8(content, content_len, enc_name,
                                      out_text) == 0 &&
            *out_text != NULL) {
            return 0;
        }
    }

    /* ── 4. Try the common CJK fallback chain ── */
    static const int fallback_list[] = {
        CUE_ENCODING_GB18030,
        CUE_ENCODING_GBK,
        CUE_ENCODING_BIG5,
        CUE_ENCODING_SHIFT_JIS
    };
    for (size_t i = 0; i < sizeof(fallback_list) / sizeof(fallback_list[0]); i++) {
        /* Skip the one we already tried above (if any) */
        if (fallback_list[i] == preferred_encoding) {
            continue;
        }
        const char *enc_name = encoding_name_from_enum(fallback_list[i]);
        if (!enc_name) {
            continue;
        }
        if (encoding_convert_to_utf8(content, content_len, enc_name,
                                      out_text) == 0 &&
            *out_text != NULL) {
            return 0;
        }
    }

    /* ── 5. Last resort: return raw bytes (BOM already stripped) ── */
    *out_text = strdup((const char *)content);
    return *out_text ? 0 : -1;
}
