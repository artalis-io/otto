/*
 * sh_xml.h - Minimal XML Pull Parser for OTTO Platform
 *
 * Streaming pull parser for XML documents. No DOM construction - reads
 * tokens one at a time from an input buffer. Arena-allocated strings.
 *
 * Features:
 * - Start/end tags with attributes, self-closing tags
 * - Text content between tags
 * - Entity decoding (&amp; &lt; &gt; &quot; &apos;)
 * - Skip subtrees, skip comments/PI/CDATA
 * - Arena-allocated strings
 *
 * Usage:
 *   SHArena *arena = sh_arena_create(4096);
 *   ShXmlReader r;
 *   sh_xml_init(&r, xml_data, xml_len, arena);
 *
 *   ShXmlToken tok;
 *   while (sh_xml_next(&r, &tok) != SH_XML_EOF) {
 *       if (tok.type == SH_XML_START_TAG && strncmp(tok.name, "row", tok.name_len) == 0) {
 *           const char *id = sh_xml_attr(&tok, "id");
 *           // ...
 *       }
 *   }
 *   sh_arena_free(arena);
 */

#ifndef SH_XML_H
#define SH_XML_H

#include "sh_arena.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#ifndef SH_XML_MAX_ATTRS
#define SH_XML_MAX_ATTRS 16
#endif

/* ============================================================================
 * Token Types
 * ============================================================================ */

typedef enum {
    SH_XML_START_TAG,   /* <tag attr="val"> */
    SH_XML_END_TAG,     /* </tag> */
    SH_XML_SELF_CLOSE,  /* <tag attr="val"/> */
    SH_XML_TEXT,        /* Text content between tags */
    SH_XML_EOF,         /* End of input */
    SH_XML_ERROR        /* Parse error */
} ShXmlTokenType;

/* ============================================================================
 * Data Structures
 * ============================================================================ */

typedef struct {
    const char *key;
    size_t key_len;
    const char *val;
    size_t val_len;
} ShXmlAttr;

typedef struct {
    ShXmlTokenType type;
    const char *name;        /* Tag name (arena-allocated, null-terminated) */
    size_t name_len;
    const char *text;        /* Text content (arena-allocated, null-terminated) */
    size_t text_len;
    int attr_count;
    ShXmlAttr attrs[SH_XML_MAX_ATTRS];
} ShXmlToken;

typedef struct {
    const char *data;
    size_t len;
    size_t pos;
    SHArena *arena;
    int error;
} ShXmlReader;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Initialize reader with XML data.
 *
 * @param r     Reader to initialize
 * @param data  XML data buffer (not modified, not copied)
 * @param len   Length of data
 * @param arena Arena for string allocations
 */
void sh_xml_init(ShXmlReader *r, const char *data, size_t len, SHArena *arena);

/*
 * Read next token.
 *
 * @param r   Reader
 * @param tok Token output (filled on success)
 * @return Token type (also stored in tok->type)
 */
ShXmlTokenType sh_xml_next(ShXmlReader *r, ShXmlToken *tok);

/*
 * Find attribute value by name (null-terminated lookup).
 * Returns NULL if not found.
 *
 * @param tok  Token with attributes
 * @param name Attribute name to find
 * @return Attribute value (arena-allocated, null-terminated) or NULL
 */
const char *sh_xml_attr(const ShXmlToken *tok, const char *name);

/*
 * Skip to the matching end tag for the current start tag.
 * Call immediately after sh_xml_next() returns SH_XML_START_TAG.
 * Handles nested tags of the same name.
 *
 * @param r         Reader
 * @param tag_name  Name of the start tag to skip
 * @return 0 on success, -1 on error (truncated input)
 */
int sh_xml_skip(ShXmlReader *r, const char *tag_name);

/*
 * Decode XML entities in-place.
 * Converts &amp; &lt; &gt; &quot; &apos; to their characters.
 * Returns the new length (always <= input length).
 *
 * @param buf  Buffer to decode in-place (must be writable)
 * @param len  Length of data in buffer
 * @return New length after decoding
 */
size_t sh_xml_decode_entities(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SH_XML_H */
