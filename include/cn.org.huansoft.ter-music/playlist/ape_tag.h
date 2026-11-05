#ifndef APE_TAG_H
#define APE_TAG_H

#include <stddef.h>
#include <stdint.h>

#define APE_MAX_ITEMS  64
#define APE_KEY_LEN    256
#define APE_VALUE_LEN  8192

typedef struct {
    char key[APE_KEY_LEN];
    char value[APE_VALUE_LEN];
    size_t value_len;
    int is_binary;
} APEItem;

int parse_ape_tags(const char *path, APEItem *items, int max_items);

#endif
