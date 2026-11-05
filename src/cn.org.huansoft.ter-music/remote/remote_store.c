/**
 * @file remote_store.c
 * @brief 前端自有的远程服务器配置读写（<configdir>/remote.xml）
 *
 * 元素名与旧配置的 `<remote_connections>` 段相同（见 config/schema.h 的
 * “Legacy remote connections”一节），因此 v5 → v6 迁移是逐元素原样复制，
 * 密码密文也原样保留。
 *
 * @author 燕戏竹林 (yxzl666xx@outlook.com)
 */

#include "remote/remote_store.h"

#include "config/config.h"
#include "config/crypto.h"
#include "config/schema.h"
#include "logger/logger.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define REMOTE_STORE_CACHE_ELEMENT   "cache"
#define REMOTE_STORE_CACHE_LIMIT     "limit_mb"
#define REMOTE_STORE_CACHE_PREFETCH  "prefetch"

#define REMOTE_STORE_DEFAULT_LIMIT_MB 2048
#define REMOTE_STORE_DEFAULT_PREFETCH 2

typedef struct {
    RemoteConnectionConfig conns[MAX_REMOTE_CONNECTIONS];
    int count;
    int cache_limit_mb;
    int prefetch;
    char path[MAX_PATH_LEN];
    int loaded;
} RemoteStore;

static RemoteStore g_store;

/* ── 小工具（与 config.c 的 XML 读取同风格） ──────────────────────── */

static xmlNodePtr store_find_child(xmlNodePtr parent, const char *name)
{
    if (!parent || !name) {
        return NULL;
    }
    for (xmlNodePtr child = parent->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, (const xmlChar *)name) == 0) {
            return child;
        }
    }
    return NULL;
}

static void store_get_string(xmlNodePtr parent, const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    xmlNodePtr node = store_find_child(parent, name);
    if (!node) {
        return;
    }
    xmlChar *content = xmlNodeGetContent(node);
    if (!content) {
        return;
    }
    snprintf(out, out_size, "%s", (const char *)content);
    xmlFree(content);
}

static int store_get_int(xmlNodePtr parent, const char *name, int fallback)
{
    char buffer[32];
    store_get_string(parent, name, buffer, sizeof(buffer));
    if (buffer[0] == '\0') {
        return fallback;
    }
    return atoi(buffer);
}

/* ── 路径与默认值 ─────────────────────────────────────────────────── */

static void store_set_defaults(void)
{
    memset(g_store.conns, 0, sizeof(g_store.conns));
    g_store.count = 0;
    g_store.cache_limit_mb = REMOTE_STORE_DEFAULT_LIMIT_MB;
    g_store.prefetch = REMOTE_STORE_DEFAULT_PREFETCH;
}

static int store_resolve_path(void)
{
    const char *dir = get_config_dir();
    if (!dir || dir[0] == '\0') {
        return -1;
    }
    int written = snprintf(g_store.path, sizeof(g_store.path), "%s/%s", dir, REMOTE_FILE_NAME);
    if (written < 0 || (size_t)written >= sizeof(g_store.path)) {
        g_store.path[0] = '\0';
        return -1;
    }
    return 0;
}

const char *remote_store_path(void)
{
    return g_store.path[0] ? g_store.path : NULL;
}

/* ── 读 ───────────────────────────────────────────────────────────── */

int remote_store_load(void)
{
    store_set_defaults();

    if (!g_store.loaded) {
        if (store_resolve_path() != 0) {
            log_warn("remote_store", "Config directory is not resolved yet, remote list stays empty");
            return 0;
        }
        g_store.loaded = 1;
    }

    struct stat st;
    if (stat(g_store.path, &st) != 0) {
        return 0;   /* 还没有前端配置：空表 */
    }

    xmlDocPtr doc = xmlParseFile(g_store.path);
    if (!doc) {
        log_error("remote_store", "Failed to parse '%s'", g_store.path);
        return -1;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr conns = root ? store_find_child(root, REMOTE_SECTION_CONNS) : NULL;
    if (conns) {
        int index = 0;
        for (xmlNodePtr node = conns->children; node && index < MAX_REMOTE_CONNECTIONS; node = node->next) {
            if (node->type != XML_ELEMENT_NODE ||
                xmlStrcmp(node->name, (const xmlChar *)XML_REMOTE_CONN) != 0) {
                continue;
            }
            RemoteConnectionConfig *conn = &g_store.conns[index];
            store_get_string(node, XML_REMOTE_NAME, conn->name, sizeof(conn->name));
            conn->protocol = store_get_int(node, XML_REMOTE_PROTOCOL, 0);
            store_get_string(node, XML_REMOTE_HOST, conn->host, sizeof(conn->host));
            conn->port = store_get_int(node, XML_REMOTE_PORT, 0);
            store_get_string(node, XML_REMOTE_USERNAME, conn->username, sizeof(conn->username));

            /* 密码：文件里存密文（encrypted="1"）；旧文件也可能是明文 */
            xmlNodePtr pwd = store_find_child(node, XML_REMOTE_PASSWORD);
            if (pwd) {
                xmlChar *content = xmlNodeGetContent(pwd);
                if (content) {
                    xmlChar *enc = xmlGetProp(pwd, (const xmlChar *)XML_ATTR_PASSWORD_ENCRYPTED);
                    if (enc && xmlStrcmp(enc, (const xmlChar *)XML_VAL_ENCRYPTED) == 0) {
                        crypto_decrypt((const char *)content, conn->password, sizeof(conn->password));
                    } else {
                        snprintf(conn->password, sizeof(conn->password), "%s", (const char *)content);
                    }
                    if (enc) {
                        xmlFree(enc);
                    }
                    xmlFree(content);
                }
            }

            store_get_string(node, XML_REMOTE_PRIVKEY, conn->private_key_path,
                             sizeof(conn->private_key_path));
            store_get_string(node, XML_REMOTE_BASE_PATH, conn->base_path, sizeof(conn->base_path));
            index++;
        }
        g_store.count = index;
    }

    xmlNodePtr cache = root ? store_find_child(root, REMOTE_STORE_CACHE_ELEMENT) : NULL;
    if (cache) {
        g_store.cache_limit_mb = store_get_int(cache, REMOTE_STORE_CACHE_LIMIT,
                                               REMOTE_STORE_DEFAULT_LIMIT_MB);
        g_store.prefetch = store_get_int(cache, REMOTE_STORE_CACHE_PREFETCH,
                                         REMOTE_STORE_DEFAULT_PREFETCH);
        if (g_store.cache_limit_mb < 0) {
            g_store.cache_limit_mb = 0;   /* 0 = 不限制 */
        }
        if (g_store.prefetch < 0) {
            g_store.prefetch = 0;
        }
    }

    xmlFreeDoc(doc);
    log_info("remote_store", "Loaded %d remote connection(s) from '%s'",
             g_store.count, g_store.path);
    return g_store.count;
}

/* ── 写 ───────────────────────────────────────────────────────────── */

int remote_store_save(void)
{
    if (!g_store.loaded && store_resolve_path() != 0) {
        return -1;
    }
    g_store.loaded = 1;

    xmlDocPtr doc = xmlNewDoc((const xmlChar *)"1.0");
    if (!doc) {
        return -1;
    }
    xmlNodePtr root = xmlNewNode(NULL, (const xmlChar *)REMOTE_ROOT);
    if (!root) {
        xmlFreeDoc(doc);
        return -1;
    }
    xmlDocSetRootElement(doc, root);
    xmlSetProp(root, (const xmlChar *)XML_ATTR_VERSION, (const xmlChar *)"1");

    xmlNodePtr conns = xmlNewChild(root, NULL, (const xmlChar *)REMOTE_SECTION_CONNS, NULL);
    char buffer[32];
    for (int i = 0; i < g_store.count; i++) {
        const RemoteConnectionConfig *conn = &g_store.conns[i];
        xmlNodePtr node = xmlNewChild(conns, NULL, (const xmlChar *)XML_REMOTE_CONN, NULL);
        xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_NAME, (const xmlChar *)conn->name);

        snprintf(buffer, sizeof(buffer), "%d", conn->protocol);
        xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_PROTOCOL, (const xmlChar *)buffer);
        xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_HOST, (const xmlChar *)conn->host);

        snprintf(buffer, sizeof(buffer), "%d", conn->port);
        xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_PORT, (const xmlChar *)buffer);
        xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_USERNAME, (const xmlChar *)conn->username);

        xmlNodePtr pwd = xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_PASSWORD, NULL);
        if (conn->password[0]) {
            char encrypted[512];
            crypto_encrypt(conn->password, encrypted, sizeof(encrypted));
            xmlNodeSetContent(pwd, (const xmlChar *)encrypted);
            xmlSetProp(pwd, (const xmlChar *)XML_ATTR_PASSWORD_ENCRYPTED,
                       (const xmlChar *)XML_VAL_ENCRYPTED);
        }

        xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_PRIVKEY,
                    (const xmlChar *)conn->private_key_path);
        xmlNewChild(node, NULL, (const xmlChar *)XML_REMOTE_BASE_PATH,
                    (const xmlChar *)conn->base_path);
    }

    xmlNodePtr cache = xmlNewChild(root, NULL, (const xmlChar *)REMOTE_STORE_CACHE_ELEMENT, NULL);
    snprintf(buffer, sizeof(buffer), "%d", g_store.cache_limit_mb);
    xmlSetProp(cache, (const xmlChar *)REMOTE_STORE_CACHE_LIMIT, (const xmlChar *)buffer);
    snprintf(buffer, sizeof(buffer), "%d", g_store.prefetch);
    xmlSetProp(cache, (const xmlChar *)REMOTE_STORE_CACHE_PREFETCH, (const xmlChar *)buffer);

    char tmp_path[MAX_PATH_LEN];
    int written = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_store.path);
    if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
        xmlFreeDoc(doc);
        return -1;
    }

    int ret = xmlSaveFormatFileEnc(tmp_path, doc, "UTF-8", 1);
    xmlFreeDoc(doc);
    if (ret < 0) {
        log_error("remote_store", "Failed to write '%s'", tmp_path);
        unlink(tmp_path);
        return -1;
    }

    /* 服务器条目含密码密文：只有属主可读 */
    chmod(tmp_path, 0600);
    if (rename(tmp_path, g_store.path) != 0) {
        log_error("remote_store", "Failed to replace '%s'", g_store.path);
        unlink(tmp_path);
        return -1;
    }
    log_info("remote_store", "Saved %d remote connection(s) to '%s'", g_store.count, g_store.path);
    return 0;
}

/* ── 条目访问 ─────────────────────────────────────────────────────── */

int remote_store_count(void)
{
    return g_store.count;
}

const RemoteConnectionConfig *remote_store_get(int index)
{
    if (index < 0 || index >= g_store.count) {
        return NULL;
    }
    return &g_store.conns[index];
}

int remote_store_upsert(int index, const RemoteConnectionConfig *conn)
{
    if (!conn) {
        return -1;
    }
    if (index < 0) {
        if (g_store.count >= MAX_REMOTE_CONNECTIONS) {
            return -1;
        }
        index = g_store.count;
        g_store.count++;
    } else if (index >= g_store.count) {
        return -1;
    }
    g_store.conns[index] = *conn;
    return 0;
}

int remote_store_remove(int index)
{
    if (index < 0 || index >= g_store.count) {
        return -1;
    }
    for (int i = index; i < g_store.count - 1; i++) {
        g_store.conns[i] = g_store.conns[i + 1];
    }
    g_store.count--;
    memset(&g_store.conns[g_store.count], 0, sizeof(g_store.conns[0]));
    return 0;
}

/* ── 缓存设置 ─────────────────────────────────────────────────────── */

int remote_store_cache_limit_mb(void)
{
    return g_store.cache_limit_mb > 0 ? g_store.cache_limit_mb : REMOTE_STORE_DEFAULT_LIMIT_MB;
}

void remote_store_set_cache_limit_mb(int mb)
{
    g_store.cache_limit_mb = mb < 0 ? 0 : mb;
}

int remote_store_prefetch(void)
{
    return g_store.prefetch;
}

void remote_store_set_prefetch(int count)
{
    g_store.prefetch = count < 0 ? 0 : count;
}
