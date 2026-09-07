/*
 * sh_api.c - Shared API response helpers.
 *
 * No transport headers here. See docs/roadmaps/transport.md.
 */
#include "sh_api.h"
#include "sh_transport_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void sh_api_response_free(ShApiResponse *resp)
{
    if (!resp) return;
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}

int sh_api_response_set(ShApiResponse *resp, int status,
                        const char *content_type,
                        const void *body, size_t body_len)
{
    if (!resp) return -1;

    memset(resp, 0, sizeof(*resp));
    resp->status_code = status;
    resp->content_type = content_type;

    if (!body || body_len == 0) {
        return 0;
    }

    /* +1 so the body is always NUL-terminated. Callers that treat it as a
     * C string (every JSON path does) then need no special casing, and the
     * extra byte is not counted in body_len. */
    resp->body = (uint8_t *)malloc(body_len + 1);
    if (!resp->body) {
        resp->status_code = 500;
        resp->content_type = "application/json";
        resp->body_len = 0;
        return -1;
    }

    memcpy(resp->body, body, body_len);
    resp->body[body_len] = '\0';
    resp->body_len = body_len;
    return 0;
}

int sh_api_response_error(ShApiResponse *resp, int status, const char *msg)
{
    char buf[512];
    char esc[384];
    size_t o = 0;

    if (!resp) return -1;
    if (!msg) msg = "error";

    /* Escape the characters JSON forbids raw in a string. Anything that does
     * not fit is truncated rather than emitted half-escaped. */
    for (const unsigned char *p = (const unsigned char *)msg;
         *p && o + 8 < sizeof(esc); p++) {
        switch (*p) {
        case '"':  esc[o++] = '\\'; esc[o++] = '"';  break;
        case '\\': esc[o++] = '\\'; esc[o++] = '\\'; break;
        case '\n': esc[o++] = '\\'; esc[o++] = 'n';  break;
        case '\r': esc[o++] = '\\'; esc[o++] = 'r';  break;
        case '\t': esc[o++] = '\\'; esc[o++] = 't';  break;
        default:
            if (*p < 0x20) {
                o += (size_t)snprintf(esc + o, sizeof(esc) - o, "\\u%04x", *p);
            } else {
                esc[o++] = (char)*p;
            }
            break;
        }
    }
    esc[o] = '\0';

    int n = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", esc);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;

    return sh_api_response_set(resp, status, "application/json",
                               buf, (size_t)n);
}

int sh_api_stream_send(ShApiStream *stream, const char *event,
                       const void *data, size_t len)
{
    if (!stream || !stream->vt || !stream->vt->send) return -1;
    return stream->vt->send(stream, event, data, len);
}

int sh_api_stream_closed(const ShApiStream *stream)
{
    if (!stream) return 1;
    if (!stream->vt || !stream->vt->closed) return 0;
    return stream->vt->closed(stream);
}
