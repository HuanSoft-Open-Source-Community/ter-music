/**
 * @file test_xml_compat.c
 * @brief 验证 libxml2 运行期兼容层（.so.2 / .so.16）能解析并遍历 XML
 */

#include "util/xml_compat.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    if (tm_xml_load() != 0) {
        fprintf(stderr, "FAIL tm_xml_load: %s\n", tm_xml_runtime_soname());
        return 1;
    }

    const char *xml = "<root><item key=\"a\">value</item></root>";
    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), NULL, NULL, 0);
    if (!doc) {
        fprintf(stderr, "FAIL xmlReadMemory (%s)\n", tm_xml_runtime_soname());
        return 2;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || strcmp((const char *)tm_xml_node_name(root), "root") != 0) {
        fprintf(stderr, "FAIL root name\n");
        xmlFreeDoc(doc);
        return 3;
    }

    xmlNodePtr item = tm_xml_first_element_child(root);
    if (!item || strcmp((const char *)tm_xml_node_name(item), "item") != 0) {
        fprintf(stderr, "FAIL first element child\n");
        xmlFreeDoc(doc);
        return 4;
    }

    xmlChar *key = xmlGetProp(item, (const xmlChar *)"key");
    xmlChar *content = xmlNodeGetContent(item);
    int ok = key && content &&
             strcmp((const char *)key, "a") == 0 &&
             strcmp((const char *)content, "value") == 0;
    xmlFree(key);
    xmlFree(content);
    xmlFreeDoc(doc);

    if (!ok) {
        fprintf(stderr, "FAIL prop/content\n");
        return 5;
    }

    printf("xml_compat: %s\n", tm_xml_runtime_soname());
    return 0;
}
