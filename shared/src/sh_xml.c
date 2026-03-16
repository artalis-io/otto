/*
 * sh_xml.c - Minimal XML Pull Parser
 *
 * Streaming pull parser for XML. Reads tokens one at a time without
 * building a DOM. All returned strings are arena-allocated.
 *
 * Handles: start/end tags, self-closing tags, attributes, text content,
 * entity decoding, comments, processing instructions, CDATA sections.
 */

#include "sh_xml.h"
#include <string.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int xml_eof(const ShXmlReader *r)
{
    return r->pos >= r->len;
}

static char xml_peek(const ShXmlReader *r)
{
    if (r->pos >= r->len) return '\0';
    return r->data[r->pos];
}

static char xml_advance(ShXmlReader *r)
{
    if (r->pos >= r->len) return '\0';
    return r->data[r->pos++];
}

static void xml_skip_ws(ShXmlReader *r)
{
    while (r->pos < r->len) {
        char c = r->data[r->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            r->pos++;
        else
            break;
    }
}

/* Check if pos starts with prefix */
static int xml_starts_with(const ShXmlReader *r, const char *prefix, size_t plen)
{
    if (r->pos + plen > r->len) return 0;
    return memcmp(r->data + r->pos, prefix, plen) == 0;
}

/* Skip past a delimiter string (e.g., "-->" or "?>") */
static int xml_skip_past(ShXmlReader *r, const char *delim, size_t dlen)
{
    while (r->pos + dlen <= r->len) {
        if (memcmp(r->data + r->pos, delim, dlen) == 0) {
            r->pos += dlen;
            return 0;
        }
        r->pos++;
    }
    /* Truncated input */
    r->pos = r->len;
    return -1;
}

/* Allocate a null-terminated copy on the arena */
static char *xml_arena_strdup(SHArena *arena, const char *src, size_t len)
{
    char *dst = (char *)sh_arena_alloc(arena, len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/* ============================================================================
 * Entity Decoding
 * ============================================================================ */

size_t sh_xml_decode_entities(char *buf, size_t len)
{
    size_t r = 0, w = 0;

    if (!buf) return 0;

    while (r < len) {
        if (buf[r] == '&') {
            /* Try to match an entity */
            size_t remaining = len - r;

            if (remaining >= 4 && memcmp(buf + r, "&lt;", 4) == 0) {
                buf[w++] = '<'; r += 4;
            } else if (remaining >= 4 && memcmp(buf + r, "&gt;", 4) == 0) {
                buf[w++] = '>'; r += 4;
            } else if (remaining >= 5 && memcmp(buf + r, "&amp;", 5) == 0) {
                buf[w++] = '&'; r += 5;
            } else if (remaining >= 6 && memcmp(buf + r, "&quot;", 6) == 0) {
                buf[w++] = '"'; r += 6;
            } else if (remaining >= 6 && memcmp(buf + r, "&apos;", 6) == 0) {
                buf[w++] = '\''; r += 6;
            } else {
                /* Unknown entity - pass through */
                buf[w++] = buf[r++];
            }
        } else {
            buf[w++] = buf[r++];
        }
    }
    buf[w] = '\0';
    return w;
}

/* Decode entities and allocate on arena */
static char *xml_decode_alloc(SHArena *arena, const char *src, size_t len,
                              size_t *out_len)
{
    char *buf = (char *)sh_arena_alloc(arena, len + 1);
    if (!buf) {
        *out_len = 0;
        return NULL;
    }
    memcpy(buf, src, len);
    buf[len] = '\0';
    *out_len = sh_xml_decode_entities(buf, len);
    return buf;
}

/* ============================================================================
 * Tag Name Parsing
 * ============================================================================ */

static int is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == ':' || (unsigned char)c >= 0x80;
}

static int is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') ||
           c == '-' || c == '.';
}

/* Parse tag name, return arena-allocated null-terminated string */
static const char *xml_parse_name(ShXmlReader *r, size_t *name_len)
{
    size_t start = r->pos;

    if (xml_eof(r) || !is_name_start(r->data[r->pos])) {
        *name_len = 0;
        return NULL;
    }

    while (r->pos < r->len && is_name_char(r->data[r->pos]))
        r->pos++;

    *name_len = r->pos - start;
    return xml_arena_strdup(r->arena, r->data + start, *name_len);
}

/* ============================================================================
 * Attribute Parsing
 * ============================================================================ */

static int xml_parse_attrs(ShXmlReader *r, ShXmlToken *tok)
{
    tok->attr_count = 0;

    while (!xml_eof(r)) {
        xml_skip_ws(r);
        char c = xml_peek(r);

        /* End of tag */
        if (c == '>' || c == '/' || c == '\0')
            return 0;

        /* Parse attribute name */
        size_t key_start = r->pos;
        if (!is_name_start(c)) return 0;

        while (r->pos < r->len && is_name_char(r->data[r->pos]))
            r->pos++;

        size_t key_len = r->pos - key_start;

        /* Skip = and whitespace */
        xml_skip_ws(r);
        if (xml_peek(r) != '=') {
            /* Attribute without value - skip */
            if (tok->attr_count < SH_XML_MAX_ATTRS) {
                ShXmlAttr *a = &tok->attrs[tok->attr_count++];
                a->key = xml_arena_strdup(r->arena, r->data + key_start, key_len);
                a->key_len = key_len;
                a->val = "";
                a->val_len = 0;
            }
            continue;
        }
        xml_advance(r); /* skip '=' */
        xml_skip_ws(r);

        /* Parse attribute value (quoted) */
        char quote = xml_peek(r);
        if (quote != '"' && quote != '\'') return -1;
        xml_advance(r); /* skip opening quote */

        size_t val_start = r->pos;
        while (r->pos < r->len && r->data[r->pos] != quote)
            r->pos++;

        size_t val_len = r->pos - val_start;
        if (!xml_eof(r)) xml_advance(r); /* skip closing quote */

        if (tok->attr_count < SH_XML_MAX_ATTRS) {
            ShXmlAttr *a = &tok->attrs[tok->attr_count++];
            a->key = xml_arena_strdup(r->arena, r->data + key_start, key_len);
            a->key_len = key_len;
            a->val = xml_decode_alloc(r->arena, r->data + val_start, val_len,
                                      &a->val_len);
        }
    }
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void sh_xml_init(ShXmlReader *r, const char *data, size_t len, SHArena *arena)
{
    if (!r) return;
    r->data = data;
    r->len = (data != NULL) ? len : 0;
    r->pos = 0;
    r->arena = arena;
    r->error = 0;
}

ShXmlTokenType sh_xml_next(ShXmlReader *r, ShXmlToken *tok)
{
    if (!r || !tok) {
        if (tok) tok->type = SH_XML_ERROR;
        return SH_XML_ERROR;
    }

    memset(tok, 0, sizeof(*tok));

    if (r->error) {
        tok->type = SH_XML_ERROR;
        return SH_XML_ERROR;
    }

    /* Skip leading whitespace between tags? No - whitespace is text content.
     * But we do skip the XML declaration and comments at top level. */

    while (!xml_eof(r)) {
        if (xml_peek(r) == '<') {
            /* Check for special sequences */
            if (xml_starts_with(r, "<!--", 4)) {
                /* Comment: skip to --> */
                r->pos += 4;
                if (xml_skip_past(r, "-->", 3) < 0) {
                    r->error = 1;
                    tok->type = SH_XML_ERROR;
                    return SH_XML_ERROR;
                }
                continue;
            }
            if (xml_starts_with(r, "<?", 2)) {
                /* Processing instruction: skip to ?> */
                r->pos += 2;
                if (xml_skip_past(r, "?>", 2) < 0) {
                    r->error = 1;
                    tok->type = SH_XML_ERROR;
                    return SH_XML_ERROR;
                }
                continue;
            }
            if (xml_starts_with(r, "<![CDATA[", 9)) {
                /* CDATA section: return as text */
                r->pos += 9;
                size_t text_start = r->pos;
                while (r->pos + 3 <= r->len) {
                    if (memcmp(r->data + r->pos, "]]>", 3) == 0)
                        break;
                    r->pos++;
                }
                size_t text_len = r->pos - text_start;
                if (r->pos + 3 <= r->len)
                    r->pos += 3; /* skip ]]> */

                tok->type = SH_XML_TEXT;
                tok->text = xml_arena_strdup(r->arena, r->data + text_start, text_len);
                tok->text_len = text_len;
                return SH_XML_TEXT;
            }
            if (xml_starts_with(r, "<!", 2)) {
                /* DOCTYPE or other declaration: skip to > */
                r->pos += 2;
                /* Handle nested brackets in DOCTYPE */
                int bracket_depth = 0;
                while (r->pos < r->len) {
                    char c = r->data[r->pos];
                    if (c == '[') bracket_depth++;
                    else if (c == ']') bracket_depth--;
                    else if (c == '>' && bracket_depth == 0) {
                        r->pos++;
                        break;
                    }
                    r->pos++;
                }
                continue;
            }

            /* End tag */
            if (xml_starts_with(r, "</", 2)) {
                r->pos += 2;
                tok->name = xml_parse_name(r, &tok->name_len);
                /* Skip to closing > */
                while (r->pos < r->len && r->data[r->pos] != '>')
                    r->pos++;
                if (!xml_eof(r)) r->pos++; /* skip '>' */

                tok->type = SH_XML_END_TAG;
                return SH_XML_END_TAG;
            }

            /* Start tag or self-closing tag */
            r->pos++; /* skip '<' */
            tok->name = xml_parse_name(r, &tok->name_len);
            if (!tok->name) {
                r->error = 1;
                tok->type = SH_XML_ERROR;
                return SH_XML_ERROR;
            }

            xml_parse_attrs(r, tok);
            xml_skip_ws(r);

            if (xml_peek(r) == '/') {
                r->pos++; /* skip '/' */
                if (xml_peek(r) == '>') r->pos++;
                tok->type = SH_XML_SELF_CLOSE;
                return SH_XML_SELF_CLOSE;
            }
            if (xml_peek(r) == '>') {
                r->pos++;
                tok->type = SH_XML_START_TAG;
                return SH_XML_START_TAG;
            }

            /* Malformed */
            r->error = 1;
            tok->type = SH_XML_ERROR;
            return SH_XML_ERROR;
        }

        /* Text content */
        size_t text_start = r->pos;
        while (r->pos < r->len && r->data[r->pos] != '<')
            r->pos++;

        size_t raw_len = r->pos - text_start;
        if (raw_len > 0) {
            /* Check if it's all whitespace */
            int all_ws = 1;
            for (size_t i = 0; i < raw_len; i++) {
                char c = r->data[text_start + i];
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    all_ws = 0;
                    break;
                }
            }
            if (all_ws) continue; /* Skip whitespace-only text nodes */

            tok->type = SH_XML_TEXT;
            tok->text = xml_decode_alloc(r->arena, r->data + text_start,
                                         raw_len, &tok->text_len);
            return SH_XML_TEXT;
        }
    }

    tok->type = SH_XML_EOF;
    return SH_XML_EOF;
}

const char *sh_xml_attr(const ShXmlToken *tok, const char *name)
{
    if (!tok || !name) return NULL;

    size_t name_len = strlen(name);
    for (int i = 0; i < tok->attr_count; i++) {
        if (tok->attrs[i].key_len == name_len &&
            memcmp(tok->attrs[i].key, name, name_len) == 0) {
            return tok->attrs[i].val;
        }
    }
    return NULL;
}

int sh_xml_skip(ShXmlReader *r, const char *tag_name)
{
    int depth = 1;
    size_t name_len;
    ShXmlToken tok;

    if (!r || !tag_name) return -1;
    name_len = strlen(tag_name);

    while (depth > 0 && !xml_eof(r)) {
        ShXmlTokenType type = sh_xml_next(r, &tok);
        if (type == SH_XML_EOF || type == SH_XML_ERROR)
            return -1;

        if (type == SH_XML_START_TAG &&
            tok.name_len == name_len &&
            memcmp(tok.name, tag_name, name_len) == 0) {
            depth++;
        } else if (type == SH_XML_END_TAG &&
                   tok.name_len == name_len &&
                   memcmp(tok.name, tag_name, name_len) == 0) {
            depth--;
        }
    }
    return 0;
}
