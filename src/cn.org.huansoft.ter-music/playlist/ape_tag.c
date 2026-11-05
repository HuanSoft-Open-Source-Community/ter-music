#include "playlist/ape_tag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define APE_TAG_FOOTER_SIZE 32
#define APE_TAG_HEADER_SIZE 32
#define APE_TAG_SIGNATURE "APETAGEX"
#define APE_FLAG_HAS_HEADER 0x20000000
#define APE_FLAG_IS_BINARY  0x00000001

#pragma pack(push, 1)
typedef struct {
    char     signature[8];
    uint32_t version;
    uint32_t tag_size;
    uint32_t item_count;
    uint32_t flags;
    char     reserved[8];
} APETagFooter;
#pragma pack(pop)

static int read_le32(FILE *f, uint32_t *out) {
    unsigned char buf[4];
    if (fread(buf, 1, 4, f) != 4) return -1;
    *out = (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
    return 0;
}

int parse_ape_tags(const char *path, APEItem *items, int max_items) {
    if (!path || !items || max_items <= 0) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    // Get file size
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long file_size = ftell(f);
    if (file_size < (long)APE_TAG_FOOTER_SIZE) {
        fclose(f);
        return 0;
    }

    // Read footer (last 32 bytes)
    if (fseek(f, file_size - APE_TAG_FOOTER_SIZE, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    APETagFooter footer;
    if (fread(&footer, 1, sizeof(footer), f) != sizeof(footer)) {
        fclose(f);
        return -1;
    }

    // Verify signature
    if (memcmp(footer.signature, APE_TAG_SIGNATURE, 8) != 0) {
        fclose(f);
        return 0;  // not an APE-tagged file
    }

    uint32_t tag_size = footer.tag_size;
    uint32_t item_count = footer.item_count;
    uint32_t flags = footer.flags;

    // Sanity checks
    if (tag_size < APE_TAG_FOOTER_SIZE || tag_size > (uint64_t)file_size) {
        fclose(f);
        return 0;
    }
    if (item_count > APE_MAX_ITEMS) {
        item_count = APE_MAX_ITEMS;
    }

    // Calculate tag start
    uint32_t tag_start = file_size - tag_size;
    if (flags & APE_FLAG_HAS_HEADER) {
        tag_start += APE_TAG_HEADER_SIZE;  // skip header
    }

    if (fseek(f, tag_start, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    int found = 0;
    for (uint32_t i = 0; i < item_count; i++) {
        uint32_t value_size, item_flags;

        if (read_le32(f, &value_size) != 0) break;
        if (read_le32(f, &item_flags) != 0) break;

        // Read key (null-terminated ASCII)
        char key[APE_KEY_LEN];
        int key_pos = 0;
        int c;
        while (key_pos < (int)sizeof(key) - 1) {
            c = fgetc(f);
            if (c == EOF) break;
            if (c == '\0') break;
            key[key_pos++] = (char)c;
        }
        key[key_pos] = '\0';

        // Clamp value size to prevent overflow
        if (value_size >= APE_VALUE_LEN) {
            value_size = APE_VALUE_LEN - 1;
        }

        // Read value
        APEItem *item = &items[found];
        size_t read_count = fread(item->value, 1, value_size, f);
        item->value[read_count] = '\0';
        item->value_len = read_count;

        // Store key
        strncpy(item->key, key, APE_KEY_LEN - 1);
        item->key[APE_KEY_LEN - 1] = '\0';

        // Binary flag
        item->is_binary = (item_flags & APE_FLAG_IS_BINARY) ? 1 : 0;

        found++;
        if (found >= max_items) break;
    }

    fclose(f);
    return found;
}
