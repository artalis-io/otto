# Mongoose HTTP Server - Claude Skills

## Overview

Mongoose is a lightweight embedded HTTP/WebSocket server library. Single-file, cross-platform, supports SSL/TLS, WebSocket, and REST APIs.

**Key features:**
- Single `mongoose.c` + `mongoose.h` (no dependencies)
- HTTP/HTTPS server and client
- WebSocket support
- Cross-platform (Linux, Windows, macOS, embedded)
- Event-driven architecture

**License:** GPLv2 or commercial

## Quick Start

```c
#include "mongoose.h"

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (mg_match(hm->uri, mg_str("/api/hello"), NULL)) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"message\": \"Hello World\"}");
        } else {
            mg_http_reply(c, 404, "", "Not Found");
        }
    }
}

int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, "http://0.0.0.0:8080", fn, NULL);
    for (;;) mg_mgr_poll(&mgr, 1000);
    mg_mgr_free(&mgr);
    return 0;
}
```

## Core Structures

### struct mg_mgr
Event manager - manages all connections.

```c
struct mg_mgr mgr;
mg_mgr_init(&mgr);           // Initialize
mg_mgr_poll(&mgr, 1000);     // Process events (1000ms timeout)
mg_mgr_free(&mgr);           // Cleanup
```

### struct mg_connection
Represents a connection. Key fields:
- `void *fn_data` - User data pointer
- `struct mg_str recv` - Received data buffer
- `struct mg_str send` - Send buffer

### struct mg_http_message
Parsed HTTP request/response:
```c
struct mg_http_message {
    struct mg_str method;    // "GET", "POST", etc.
    struct mg_str uri;       // "/api/endpoint"
    struct mg_str query;     // "key=value&..."
    struct mg_str proto;     // "HTTP/1.1"
    struct mg_str body;      // Request body
    struct mg_str head;      // All headers
    struct mg_str headers[MG_MAX_HTTP_HEADERS];  // Individual headers
};
```

### struct mg_str
String slice (pointer + length, not null-terminated):
```c
struct mg_str {
    const char *buf;
    size_t len;
};

// Create from literal
struct mg_str s = mg_str("hello");

// Create with length
struct mg_str s = mg_str_n(ptr, length);
```

## Event Types

```c
MG_EV_ERROR      // Error occurred
MG_EV_OPEN       // Connection created
MG_EV_POLL       // mg_mgr_poll iteration
MG_EV_RESOLVE    // Host name resolved
MG_EV_CONNECT    // Connection established
MG_EV_ACCEPT     // New connection accepted
MG_EV_TLS_HS     // TLS handshake complete
MG_EV_READ       // Data received
MG_EV_WRITE      // Data written
MG_EV_CLOSE      // Connection closed
MG_EV_HTTP_MSG   // HTTP request/response complete
MG_EV_WS_OPEN    // WebSocket handshake complete
MG_EV_WS_MSG     // WebSocket message received
MG_EV_WS_CTL     // WebSocket control message
```

## HTTP Server Functions

### mg_http_listen
```c
struct mg_connection *mg_http_listen(
    struct mg_mgr *mgr,
    const char *url,           // "http://0.0.0.0:8080"
    mg_event_handler_t fn,     // Event handler
    void *fn_data              // User data
);
```

### mg_http_reply
```c
void mg_http_reply(
    struct mg_connection *c,
    int status_code,           // 200, 404, etc.
    const char *headers,       // Extra headers (end with \r\n)
    const char *body_fmt,      // printf-style format
    ...                        // Format arguments
);

// Examples
mg_http_reply(c, 200, "Content-Type: application/json\r\n",
              "{\"status\": \"ok\"}");

mg_http_reply(c, 200, "", "Hello %s!", name);

mg_http_reply(c, 404, "", "Not found");
```

### mg_http_serve_dir
```c
struct mg_http_serve_opts opts = {
    .root_dir = "/var/www",
    .extra_headers = "Cache-Control: max-age=3600\r\n"
};
mg_http_serve_dir(c, hm, &opts);
```

### mg_http_serve_file
```c
struct mg_http_serve_opts opts = {
    .mime_types = "html=text/html,css=text/css,js=text/javascript"
};
mg_http_serve_file(c, hm, "index.html", &opts);
```

## URI Matching

### mg_match
```c
// Simple match
if (mg_match(hm->uri, mg_str("/api/health"), NULL)) {
    // Handle /api/health
}

// With wildcards
struct mg_str caps[3];
if (mg_match(hm->uri, mg_str("/api/users/*"), caps)) {
    // caps[0] contains the wildcard match
    int user_id = atoi(caps[0].buf);
}

// Multiple wildcards
if (mg_match(hm->uri, mg_str("/api/*/items/*"), caps)) {
    // caps[0] = first *, caps[1] = second *
}
```

### mg_strcmp
```c
if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
    // Handle POST request
}
```

## JSON Handling

### Reading JSON
```c
// Get number
double val;
if (mg_json_get_num(hm->body, "$.price", &val)) {
    printf("Price: %f\n", val);
}

// Get string (returns mg_str)
char *str = mg_json_get_str(hm->body, "$.name");
if (str) {
    printf("Name: %s\n", str);
    free(str);
}

// Get boolean
bool flag;
if (mg_json_get_bool(hm->body, "$.enabled", &flag)) {
    printf("Enabled: %d\n", flag);
}

// Get long
long id;
if (mg_json_get_long(hm->body, "$.id", &id)) {
    printf("ID: %ld\n", id);
}
```

### JSON Paths
```
$.field          - Object field
$.arr[0]         - Array element
$.obj.nested     - Nested field
$[0].field       - Array element's field
```

## REST API Pattern

```c
static void handle_api(struct mg_connection *c, struct mg_http_message *hm) {
    if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"status\": \"healthy\"}");

    } else if (mg_match(hm->uri, mg_str("/api/v1/users"), NULL)) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            // List users
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "[{\"id\": 1}, {\"id\": 2}]");
        } else if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            // Create user - parse body
            char *name = mg_json_get_str(hm->body, "$.name");
            if (name) {
                mg_http_reply(c, 201, "Content-Type: application/json\r\n",
                              "{\"id\": 3, \"name\": \"%s\"}", name);
                free(name);
            } else {
                mg_http_reply(c, 400, "", "Missing name");
            }
        } else {
            mg_http_reply(c, 405, "", "Method not allowed");
        }

    } else {
        mg_http_reply(c, 404, "", "Not found");
    }
}

static void fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        handle_api(c, (struct mg_http_message *)ev_data);
    }
}
```

## CORS Headers

```c
#define CORS_HEADERS \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Content-Type\r\n"

// Handle preflight
if (mg_strcmp(hm->method, mg_str("OPTIONS")) == 0) {
    mg_http_reply(c, 204, CORS_HEADERS, "");
    return;
}

// Add to responses
mg_http_reply(c, 200, CORS_HEADERS "Content-Type: application/json\r\n",
              "{\"data\": \"value\"}");
```

## HTTP Client

```c
static void client_fn(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_CONNECT) {
        struct mg_str host = mg_url_host("http://example.com/api");
        mg_printf(c, "GET /api HTTP/1.1\r\nHost: %.*s\r\n\r\n",
                  (int)host.len, host.buf);
    } else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;
        printf("Response: %.*s\n", (int)hm->body.len, hm->body.buf);
        c->is_draining = 1;  // Close after response
    }
}

// Make request
mg_http_connect(&mgr, "http://example.com", client_fn, NULL);
```

## WebSocket

### Server
```c
if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = ev_data;
    if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
        mg_ws_upgrade(c, hm, NULL);  // Upgrade to WebSocket
    }
} else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = ev_data;
    mg_ws_send(c, wm->data.buf, wm->data.len, WEBSOCKET_OP_TEXT);
}
```

### Send WebSocket message
```c
mg_ws_send(c, "hello", 5, WEBSOCKET_OP_TEXT);
mg_ws_send(c, binary_data, len, WEBSOCKET_OP_BINARY);
```

## Compile Flags

```c
#define MG_ENABLE_LINES 0    // Disable __FILE__/__LINE__ in errors
#define MG_ENABLE_LOG 0      // Disable logging
#define MG_ENABLE_SSL 1      // Enable TLS (requires OpenSSL/mbedTLS)
```

## Building

```makefile
# Simple
gcc -o server main.c mongoose.c

# With defines
gcc -DMG_ENABLE_LINES=0 -o server main.c mongoose.c
```

## Resources

- **Documentation:** https://mongoose.ws/documentation/
- **GitHub:** https://github.com/cesanta/mongoose
- **Examples:** https://github.com/cesanta/mongoose/tree/master/examples
