/**
 * @file xml_compat.h
 * @brief libxml2 运行期兼容层：同时兼容 libxml2.so.2 与 libxml2.so.16
 *
 * Debian 10 构建的二进制如果直接链接 libxml2，DT_NEEDED 会写死
 * libxml2.so.2；Ubuntu 26.04 只提供 libxml2.so.16。这里不链接 libxml2，
 * 而是在运行期按 .so.2 → .so.16 → .so 的顺序 dlopen，再用 dlsym 绑定
 * 项目实际使用的 API。源文件继续使用 libxml2 的函数名，由本头文件的宏
 * 映射到 tm_xml_* 包装函数。
 *
 * 注意：libxml2 2.14 起官方弃用直接结构体访问。项目只保留
 * tm_xml_node_name() 一处直接读取 node->name；其余子节点遍历统一走
 * xmlFirstElementChild()/xmlNextElementSibling()。
 */

#ifndef TER_MUSIC_XML_COMPAT_H
#define TER_MUSIC_XML_COMPAT_H

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次性加载运行库；成功返回 0，失败返回 -1。重复调用安全。 */
int tm_xml_load(void);

/* 实际加载的 soname（调试/验收用），未加载时返回 "none"。 */
const char *tm_xml_runtime_soname(void);

/* 稳定访问器（把直接结构体访问收敛到这里）。 */
const xmlChar *tm_xml_node_name(const xmlNode *node);
xmlNodePtr tm_xml_first_element_child(const xmlNode *parent);
xmlNodePtr tm_xml_next_element_sibling(const xmlNode *node);

/* 项目实际使用的 libxml2 API 包装。 */
xmlDocPtr tm_xml_read_file(const char *filename, const char *encoding, int options);
xmlDocPtr tm_xml_read_memory(const char *buffer, int size, const char *url,
                             const char *encoding, int options);
xmlDocPtr tm_xml_parse_file(const char *filename);
xmlNodePtr tm_xml_doc_get_root_element(const xmlDoc *doc);
xmlNodePtr tm_xml_doc_set_root_element(xmlDocPtr doc, xmlNodePtr root);
xmlChar *tm_xml_get_prop(const xmlNode *node, const xmlChar *name);
xmlChar *tm_xml_node_get_content(const xmlNode *cur);
int tm_xml_node_set_content(xmlNodePtr cur, const xmlChar *content);
xmlDocPtr tm_xml_new_doc(const xmlChar *version);
xmlNodePtr tm_xml_new_node(xmlNsPtr ns, const xmlChar *name);
xmlNodePtr tm_xml_new_child(xmlNodePtr parent, xmlNsPtr ns,
                            const xmlChar *name, const xmlChar *content);
xmlNodePtr tm_xml_add_child(xmlNodePtr parent, xmlNodePtr cur);
xmlNodePtr tm_xml_doc_copy_node(const xmlNode *node, xmlDocPtr doc, int recursive);
xmlAttrPtr tm_xml_set_prop(xmlNodePtr node, const xmlChar *name, const xmlChar *value);
int tm_xml_save_format_file_enc(const char *filename, xmlDocPtr cur,
                                const char *encoding, int format);
int tm_xml_strcmp(const xmlChar *str1, const xmlChar *str2);
int tm_xml_strlen(const xmlChar *str);
void tm_xml_free_doc(xmlDocPtr cur);
void tm_xml_free(void *ptr);

#ifdef __cplusplus
}
#endif

#ifndef TER_MUSIC_XML_COMPAT_IMPL
/*
 * 宏映射让现有调用点保持 libxml2 原名。xmlFree 在 libxml2 2.15+ 是
 * (*__xmlFree()) 宏，必须先 undef 再覆盖。
 */
#undef xmlFree
#define xmlReadFile             tm_xml_read_file
#define xmlReadMemory           tm_xml_read_memory
#define xmlParseFile            tm_xml_parse_file
#define xmlDocGetRootElement    tm_xml_doc_get_root_element
#define xmlDocSetRootElement    tm_xml_doc_set_root_element
#define xmlGetProp              tm_xml_get_prop
#define xmlNodeGetContent       tm_xml_node_get_content
#define xmlNodeSetContent       tm_xml_node_set_content
#define xmlNewDoc               tm_xml_new_doc
#define xmlNewNode              tm_xml_new_node
#define xmlNewChild             tm_xml_new_child
#define xmlAddChild             tm_xml_add_child
#define xmlDocCopyNode          tm_xml_doc_copy_node
#define xmlSetProp              tm_xml_set_prop
#define xmlSaveFormatFileEnc    tm_xml_save_format_file_enc
#define xmlStrcmp               tm_xml_strcmp
#define xmlStrlen               tm_xml_strlen
#define xmlFreeDoc              tm_xml_free_doc
#define xmlFree                 tm_xml_free
#endif /* TER_MUSIC_XML_COMPAT_IMPL */

#endif /* TER_MUSIC_XML_COMPAT_H */
