/**
 * @file xml_compat.c
 * @brief libxml2 运行期兼容层实现（见 util/xml_compat.h）
 */

#define TER_MUSIC_XML_COMPAT_IMPL 1
#include "util/xml_compat.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* ── 函数指针类型（与 libxml2 公开签名一致） ─────────────────── */

typedef xmlDocPtr (*fn_xml_read_file)(const char *, const char *, int);
typedef xmlDocPtr (*fn_xml_read_memory)(const char *, int, const char *, const char *, int);
typedef xmlDocPtr (*fn_xml_parse_file)(const char *);
typedef xmlNodePtr (*fn_xml_doc_get_root_element)(const xmlDoc *);
typedef xmlNodePtr (*fn_xml_doc_set_root_element)(xmlDocPtr, xmlNodePtr);
typedef xmlChar *(*fn_xml_get_prop)(const xmlNode *, const xmlChar *);
typedef xmlChar *(*fn_xml_node_get_content)(const xmlNode *);
typedef int (*fn_xml_node_set_content)(xmlNodePtr, const xmlChar *);
typedef xmlDocPtr (*fn_xml_new_doc)(const xmlChar *);
typedef xmlNodePtr (*fn_xml_new_node)(xmlNsPtr, const xmlChar *);
typedef xmlNodePtr (*fn_xml_new_child)(xmlNodePtr, xmlNsPtr, const xmlChar *, const xmlChar *);
typedef xmlNodePtr (*fn_xml_add_child)(xmlNodePtr, xmlNodePtr);
typedef xmlNodePtr (*fn_xml_doc_copy_node)(const xmlNode *, xmlDocPtr, int);
typedef xmlAttrPtr (*fn_xml_set_prop)(xmlNodePtr, const xmlChar *, const xmlChar *);
typedef int (*fn_xml_save_format_file_enc)(const char *, xmlDocPtr, const char *, int);
typedef int (*fn_xml_strcmp)(const xmlChar *, const xmlChar *);
typedef int (*fn_xml_strlen)(const xmlChar *);
typedef void (*fn_xml_free_doc)(xmlDocPtr);
typedef xmlNodePtr (*fn_xml_first_element_child)(xmlNodePtr);
typedef xmlNodePtr (*fn_xml_next_element_sibling)(xmlNodePtr);
typedef void (*fn_xml_mem_free)(void *);

/* ── 加载状态 ───────────────────────────────────────────────── */

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void *g_handle = NULL;
static const char *g_soname = NULL;
static int g_ok = 0;

static fn_xml_read_file p_xml_read_file;
static fn_xml_read_memory p_xml_read_memory;
static fn_xml_parse_file p_xml_parse_file;
static fn_xml_doc_get_root_element p_xml_doc_get_root_element;
static fn_xml_doc_set_root_element p_xml_doc_set_root_element;
static fn_xml_get_prop p_xml_get_prop;
static fn_xml_node_get_content p_xml_node_get_content;
static fn_xml_node_set_content p_xml_node_set_content;
static fn_xml_new_doc p_xml_new_doc;
static fn_xml_new_node p_xml_new_node;
static fn_xml_new_child p_xml_new_child;
static fn_xml_add_child p_xml_add_child;
static fn_xml_doc_copy_node p_xml_doc_copy_node;
static fn_xml_set_prop p_xml_set_prop;
static fn_xml_save_format_file_enc p_xml_save_format_file_enc;
static fn_xml_strcmp p_xml_strcmp;
static fn_xml_strlen p_xml_strlen;
static fn_xml_free_doc p_xml_free_doc;
static fn_xml_first_element_child p_xml_first_element_child;
static fn_xml_next_element_sibling p_xml_next_element_sibling;
static fn_xml_mem_free p_xml_mem_free;
static xmlFreeFunc *p_xml_free_var;

static int load_symbol(const char *name, void **out)
{
    void *sym = dlsym(g_handle, name);
    if (!sym) {
        fprintf(stderr, "ter-music: libxml2 (%s) missing symbol %s\n",
                g_soname ? g_soname : "unknown", name);
        return -1;
    }
    *out = sym;
    return 0;
}

static void load_once(void)
{
    static const char *candidates[] = {
        "libxml2.so.2",
        "libxml2.so.16",
        "libxml2.so",
        NULL
    };

    for (int i = 0; candidates[i]; i++) {
        g_handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (g_handle) {
            g_soname = candidates[i];
            break;
        }
    }
    if (!g_handle) {
        fprintf(stderr,
                "ter-music: cannot load libxml2 runtime "
                "(tried libxml2.so.2, libxml2.so.16, libxml2.so)\n");
        return;
    }

    if (load_symbol("xmlReadFile", (void **)&p_xml_read_file) != 0 ||
        load_symbol("xmlReadMemory", (void **)&p_xml_read_memory) != 0 ||
        load_symbol("xmlParseFile", (void **)&p_xml_parse_file) != 0 ||
        load_symbol("xmlDocGetRootElement", (void **)&p_xml_doc_get_root_element) != 0 ||
        load_symbol("xmlDocSetRootElement", (void **)&p_xml_doc_set_root_element) != 0 ||
        load_symbol("xmlGetProp", (void **)&p_xml_get_prop) != 0 ||
        load_symbol("xmlNodeGetContent", (void **)&p_xml_node_get_content) != 0 ||
        load_symbol("xmlNodeSetContent", (void **)&p_xml_node_set_content) != 0 ||
        load_symbol("xmlNewDoc", (void **)&p_xml_new_doc) != 0 ||
        load_symbol("xmlNewNode", (void **)&p_xml_new_node) != 0 ||
        load_symbol("xmlNewChild", (void **)&p_xml_new_child) != 0 ||
        load_symbol("xmlAddChild", (void **)&p_xml_add_child) != 0 ||
        load_symbol("xmlDocCopyNode", (void **)&p_xml_doc_copy_node) != 0 ||
        load_symbol("xmlSetProp", (void **)&p_xml_set_prop) != 0 ||
        load_symbol("xmlSaveFormatFileEnc", (void **)&p_xml_save_format_file_enc) != 0 ||
        load_symbol("xmlStrcmp", (void **)&p_xml_strcmp) != 0 ||
        load_symbol("xmlStrlen", (void **)&p_xml_strlen) != 0 ||
        load_symbol("xmlFreeDoc", (void **)&p_xml_free_doc) != 0 ||
        load_symbol("xmlFirstElementChild", (void **)&p_xml_first_element_child) != 0 ||
        load_symbol("xmlNextElementSibling", (void **)&p_xml_next_element_sibling) != 0) {
        dlclose(g_handle);
        g_handle = NULL;
        g_soname = NULL;
        return;
    }

    /*
     * 标准释放入口是 xmlFree 变量。libxml2 2.9 与 2.15+ 都保留了这个
     * 兼容变量（2.15 只是另外提供了 (*__xmlFree()) 宏）；优先解引用它。
     * xmlMemFree 是调试分配器的入口，对普通 xmlMalloc 指针会打 tag 错误，
     * 所以只作为没有 xmlFree 变量时的兜底。
     */
    {
        void *sym = dlsym(g_handle, "xmlFree");
        if (sym) {
            p_xml_free_var = (xmlFreeFunc *)sym;
            if (!*p_xml_free_var) {
                p_xml_free_var = NULL;
            }
        }
        if (!p_xml_free_var) {
            sym = dlsym(g_handle, "xmlMemFree");
            if (sym) {
                p_xml_mem_free = (fn_xml_mem_free)sym;
            }
        }
        if (!p_xml_free_var && !p_xml_mem_free) {
            fprintf(stderr,
                    "ter-music: libxml2 (%s) exposes neither xmlFree nor xmlMemFree\n",
                    g_soname);
            dlclose(g_handle);
            g_handle = NULL;
            g_soname = NULL;
            return;
        }
    }

    g_ok = 1;
    if (getenv("TER_MUSIC_XML_DEBUG")) {
        fprintf(stderr, "ter-music: libxml2 runtime: %s\n", g_soname);
    }
}

int tm_xml_load(void)
{
    pthread_once(&g_once, load_once);
    return g_ok ? 0 : -1;
}

const char *tm_xml_runtime_soname(void)
{
    return g_soname ? g_soname : "none";
}

const xmlChar *tm_xml_node_name(const xmlNode *node)
{
    return node ? node->name : NULL;
}

xmlNodePtr tm_xml_first_element_child(const xmlNode *parent)
{
    if (tm_xml_load() != 0 || !parent) {
        return NULL;
    }
    /* libxml2 的 accessor 参数不是 const，但它不会修改节点。 */
    return p_xml_first_element_child((xmlNodePtr)parent);
}

xmlNodePtr tm_xml_next_element_sibling(const xmlNode *node)
{
    if (tm_xml_load() != 0 || !node) {
        return NULL;
    }
    return p_xml_next_element_sibling((xmlNodePtr)node);
}

xmlDocPtr tm_xml_read_file(const char *filename, const char *encoding, int options)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_read_file(filename, encoding, options);
}

xmlDocPtr tm_xml_read_memory(const char *buffer, int size, const char *url,
                             const char *encoding, int options)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_read_memory(buffer, size, url, encoding, options);
}

xmlDocPtr tm_xml_parse_file(const char *filename)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_parse_file(filename);
}

xmlNodePtr tm_xml_doc_get_root_element(const xmlDoc *doc)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_doc_get_root_element(doc);
}

xmlNodePtr tm_xml_doc_set_root_element(xmlDocPtr doc, xmlNodePtr root)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_doc_set_root_element(doc, root);
}

xmlChar *tm_xml_get_prop(const xmlNode *node, const xmlChar *name)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_get_prop(node, name);
}

xmlChar *tm_xml_node_get_content(const xmlNode *cur)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_node_get_content(cur);
}

int tm_xml_node_set_content(xmlNodePtr cur, const xmlChar *content)
{
    if (tm_xml_load() != 0) {
        return -1;
    }
    return p_xml_node_set_content(cur, content);
}

xmlDocPtr tm_xml_new_doc(const xmlChar *version)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_new_doc(version);
}

xmlNodePtr tm_xml_new_node(xmlNsPtr ns, const xmlChar *name)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_new_node(ns, name);
}

xmlNodePtr tm_xml_new_child(xmlNodePtr parent, xmlNsPtr ns,
                            const xmlChar *name, const xmlChar *content)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_new_child(parent, ns, name, content);
}

xmlNodePtr tm_xml_add_child(xmlNodePtr parent, xmlNodePtr cur)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_add_child(parent, cur);
}

xmlNodePtr tm_xml_doc_copy_node(const xmlNode *node, xmlDocPtr doc, int recursive)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_doc_copy_node(node, doc, recursive);
}

xmlAttrPtr tm_xml_set_prop(xmlNodePtr node, const xmlChar *name, const xmlChar *value)
{
    if (tm_xml_load() != 0) {
        return NULL;
    }
    return p_xml_set_prop(node, name, value);
}

int tm_xml_save_format_file_enc(const char *filename, xmlDocPtr cur,
                                const char *encoding, int format)
{
    if (tm_xml_load() != 0) {
        return -1;
    }
    return p_xml_save_format_file_enc(filename, cur, encoding, format);
}

int tm_xml_strcmp(const xmlChar *str1, const xmlChar *str2)
{
    if (tm_xml_load() != 0) {
        return -1;
    }
    return p_xml_strcmp(str1, str2);
}

int tm_xml_strlen(const xmlChar *str)
{
    if (tm_xml_load() != 0) {
        return 0;
    }
    return p_xml_strlen(str);
}

void tm_xml_free_doc(xmlDocPtr cur)
{
    if (tm_xml_load() != 0 || !cur) {
        return;
    }
    p_xml_free_doc(cur);
}

void tm_xml_free(void *ptr)
{
    if (!ptr || tm_xml_load() != 0) {
        return;
    }
    if (p_xml_free_var && *p_xml_free_var) {
        (*p_xml_free_var)(ptr);
    } else if (p_xml_mem_free) {
        p_xml_mem_free(ptr);
    }
}
