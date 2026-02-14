/*
 * test_xml.c - Unit tests for XML pull parser
 */

#include "sh_xml.h"
#include "sh_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Test Framework
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-55s ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("[PASS]\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n", #cond); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("[FAIL]\n    Expected: \"%s\"\n    Got:      \"%s\"\n", (b), (a)); \
        exit(1); \
    } \
} while (0)

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(xml_empty)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    sh_xml_init(&r, "", 0, arena);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_EOF);
    sh_arena_free(arena);
}

TEST(xml_single_tag)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<root>hello</root>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "root");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "hello");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_END_TAG);
    ASSERT_STREQ(tok.name, "root");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_EOF);
    sh_arena_free(arena);
}

TEST(xml_self_close)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<item/>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_SELF_CLOSE);
    ASSERT_STREQ(tok.name, "item");
    ASSERT_EQ(tok.attr_count, 0);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_EOF);
    sh_arena_free(arena);
}

TEST(xml_attributes)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<row id=\"42\" name='test' flag=\"true\"/>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_SELF_CLOSE);
    ASSERT_EQ(tok.attr_count, 3);

    const char *id = sh_xml_attr(&tok, "id");
    ASSERT(id != NULL);
    ASSERT_STREQ(id, "42");

    const char *name = sh_xml_attr(&tok, "name");
    ASSERT(name != NULL);
    ASSERT_STREQ(name, "test");

    const char *flag = sh_xml_attr(&tok, "flag");
    ASSERT(flag != NULL);
    ASSERT_STREQ(flag, "true");

    /* Non-existent attribute */
    ASSERT(sh_xml_attr(&tok, "missing") == NULL);

    sh_arena_free(arena);
}

TEST(xml_nested)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<a><b><c>deep</c></b></a>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "a");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "b");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "c");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "deep");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_END_TAG);
    ASSERT_STREQ(tok.name, "c");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_END_TAG);
    ASSERT_STREQ(tok.name, "b");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_END_TAG);
    ASSERT_STREQ(tok.name, "a");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_EOF);

    sh_arena_free(arena);
}

TEST(xml_entity_amp)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<t>A &amp; B</t>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "A & B");
    ASSERT_EQ(tok.text_len, 5);

    sh_arena_free(arena);
}

TEST(xml_entity_lt_gt)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<t>&lt;tag&gt;</t>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "<tag>");

    sh_arena_free(arena);
}

TEST(xml_entity_quot_apos)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<t>&quot;hello&apos;s&quot;</t>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "\"hello's\"");

    sh_arena_free(arena);
}

TEST(xml_entity_in_attr)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<t val=\"A &amp; B\"/>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_SELF_CLOSE);
    const char *val = sh_xml_attr(&tok, "val");
    ASSERT(val != NULL);
    ASSERT_STREQ(val, "A & B");

    sh_arena_free(arena);
}

TEST(xml_skip_subtree)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<root><skip><a><b>nested</b></a></skip><after>here</after></root>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "root");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "skip");
    ASSERT_EQ(sh_xml_skip(&r, "skip"), 0);

    /* Should now be at <after> */
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "after");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "here");

    sh_arena_free(arena);
}

TEST(xml_namespace_prefix)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<ns:tag ns:attr=\"val\">text</ns:tag>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "ns:tag");

    const char *val = sh_xml_attr(&tok, "ns:attr");
    ASSERT(val != NULL);
    ASSERT_STREQ(val, "val");

    sh_arena_free(arena);
}

TEST(xml_whitespace_in_text)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<t>  hello\n  world  </t>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "  hello\n  world  ");

    sh_arena_free(arena);
}

TEST(xml_comment_skip)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<!-- comment --><root>text</root>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "root");

    sh_arena_free(arena);
}

TEST(xml_processing_instruction)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<?xml version=\"1.0\"?><root/>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_SELF_CLOSE);
    ASSERT_STREQ(tok.name, "root");

    sh_arena_free(arena);
}

TEST(xml_cdata_skip)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<t><![CDATA[raw <data> & stuff]]></t>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    /* CDATA content is literal, no entity decoding */
    ASSERT_STREQ(tok.text, "raw <data> & stuff");

    sh_arena_free(arena);
}

TEST(xml_truncated_input)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<root><child>text";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "root");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_STREQ(tok.name, "child");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "text");
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_EOF);

    sh_arena_free(arena);
}

TEST(xml_deeply_nested)
{
    SHArena *arena = sh_arena_create(8192);
    ShXmlReader r;
    ShXmlToken tok;
    /* Build a deeply nested XML string */
    char xml[2048];
    int pos = 0;
    for (int i = 0; i < 20; i++)
        pos += snprintf(xml + pos, sizeof(xml) - (size_t)pos, "<d%d>", i);
    pos += snprintf(xml + pos, sizeof(xml) - (size_t)pos, "leaf");
    for (int i = 19; i >= 0; i--)
        pos += snprintf(xml + pos, sizeof(xml) - (size_t)pos, "</d%d>", i);

    sh_xml_init(&r, xml, (size_t)pos, arena);

    /* Read all 20 start tags */
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    }
    /* Text */
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_STREQ(tok.text, "leaf");
    /* Read all 20 end tags */
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_END_TAG);
    }
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_EOF);

    sh_arena_free(arena);
}

TEST(xml_attr_overflow)
{
    SHArena *arena = sh_arena_create(8192);
    ShXmlReader r;
    ShXmlToken tok;
    /* Build tag with more than SH_XML_MAX_ATTRS (16) attributes */
    char xml[2048];
    int pos = snprintf(xml, sizeof(xml), "<tag");
    for (int i = 0; i < 20; i++)
        pos += snprintf(xml + pos, sizeof(xml) - (size_t)pos, " a%d=\"v%d\"", i, i);
    pos += snprintf(xml + pos, sizeof(xml) - (size_t)pos, "/>");

    sh_xml_init(&r, xml, (size_t)pos, arena);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_SELF_CLOSE);
    /* Should have exactly SH_XML_MAX_ATTRS */
    ASSERT_EQ(tok.attr_count, SH_XML_MAX_ATTRS);
    /* First attr should be correct */
    const char *v0 = sh_xml_attr(&tok, "a0");
    ASSERT(v0 != NULL);
    ASSERT_STREQ(v0, "v0");

    sh_arena_free(arena);
}

TEST(xml_empty_attr_value)
{
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<tag val=\"\"/>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_SELF_CLOSE);
    const char *val = sh_xml_attr(&tok, "val");
    ASSERT(val != NULL);
    ASSERT_STREQ(val, "");

    sh_arena_free(arena);
}

TEST(xml_binary_in_text)
{
    /* Non-printable bytes should pass through */
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char xml[] = "<t>\x01\x02\x03</t>";
    sh_xml_init(&r, xml, sizeof(xml) - 1, arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);
    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);
    ASSERT_EQ(tok.text_len, 3);
    ASSERT(tok.text[0] == '\x01');
    ASSERT(tok.text[1] == '\x02');
    ASSERT(tok.text[2] == '\x03');

    sh_arena_free(arena);
}

TEST(xml_multiple_text_segments)
{
    /* Text interrupted by child elements */
    SHArena *arena = sh_arena_create(4096);
    ShXmlReader r;
    ShXmlToken tok;
    const char *xml = "<p>hello <b>world</b> end</p>";
    sh_xml_init(&r, xml, strlen(xml), arena);

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);  /* <p> */
    ASSERT_STREQ(tok.name, "p");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);        /* "hello " */
    ASSERT_STREQ(tok.text, "hello ");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_START_TAG);  /* <b> */
    ASSERT_STREQ(tok.name, "b");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);        /* "world" */
    ASSERT_STREQ(tok.text, "world");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_END_TAG);    /* </b> */
    ASSERT_STREQ(tok.name, "b");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_TEXT);        /* " end" */
    ASSERT_STREQ(tok.text, " end");

    ASSERT_EQ(sh_xml_next(&r, &tok), SH_XML_END_TAG);    /* </p> */

    sh_arena_free(arena);
}

TEST(xml_decode_entities_standalone)
{
    char buf[64];

    /* &amp; */
    strcpy(buf, "A &amp; B");
    size_t len = sh_xml_decode_entities(buf, strlen(buf));
    ASSERT_EQ(len, 5);
    ASSERT_STREQ(buf, "A & B");

    /* Multiple entities */
    strcpy(buf, "&lt;&gt;&amp;&quot;&apos;");
    len = sh_xml_decode_entities(buf, strlen(buf));
    ASSERT_EQ(len, 5);
    ASSERT_STREQ(buf, "<>&\"'");

    /* No entities */
    strcpy(buf, "plain text");
    len = sh_xml_decode_entities(buf, strlen(buf));
    ASSERT_EQ(len, 10);
    ASSERT_STREQ(buf, "plain text");

    /* Unknown entity passed through */
    strcpy(buf, "&unknown;");
    len = sh_xml_decode_entities(buf, strlen(buf));
    ASSERT_STREQ(buf, "&unknown;");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nXML Parser Tests:\n");

    RUN_TEST(xml_empty);
    RUN_TEST(xml_single_tag);
    RUN_TEST(xml_self_close);
    RUN_TEST(xml_attributes);
    RUN_TEST(xml_nested);
    RUN_TEST(xml_entity_amp);
    RUN_TEST(xml_entity_lt_gt);
    RUN_TEST(xml_entity_quot_apos);
    RUN_TEST(xml_entity_in_attr);
    RUN_TEST(xml_skip_subtree);
    RUN_TEST(xml_namespace_prefix);
    RUN_TEST(xml_whitespace_in_text);
    RUN_TEST(xml_comment_skip);
    RUN_TEST(xml_processing_instruction);
    RUN_TEST(xml_cdata_skip);
    RUN_TEST(xml_truncated_input);
    RUN_TEST(xml_deeply_nested);
    RUN_TEST(xml_attr_overflow);
    RUN_TEST(xml_empty_attr_value);
    RUN_TEST(xml_binary_in_text);
    RUN_TEST(xml_multiple_text_segments);
    RUN_TEST(xml_decode_entities_standalone);

    printf("\nXML Parser: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
