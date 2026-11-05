/**
 * @file layout_state.c
 * @brief 前端本地布局选中状态实现
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "ui/layout_state.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int layout_state_read(const char *path, char *id, size_t id_size)
{
    if (!path || !id || id_size == 0) {
        return -1;
    }
    id[0] = '\0';

    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }

    char buffer[128];
    if (!fgets(buffer, sizeof(buffer), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);

    char *start = buffer;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    if (*start == '\0' || strlen(start) >= id_size) {
        return -1;
    }
    strcpy(id, start);
    return 0;
}

int layout_state_write(const char *path, const char *id)
{
    if (!path || !id || id[0] == '\0') {
        return -1;
    }

    char temp[1024];
    int written = snprintf(temp, sizeof(temp), "%s.tmp.%ld",
                           path, (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(temp)) {
        return -1;
    }

    FILE *file = fopen(temp, "w");
    if (!file) {
        return -1;
    }
    int ok = (fprintf(file, "%s\n", id) > 0);
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(temp);
        return -1;
    }
    if (rename(temp, path) != 0) {
        remove(temp);
        return -1;
    }
    return 0;
}
