/**
 * cs_soft_cocoa.m - macOS/Cocoa backend for software renderer
 *
 * Uses NSWindow + NSView with CGBitmapContext for display.
 */

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include "../cs_soft_internal.h"

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

@class CsSoftView;
@class CsSoftWindowDelegate;

/* ============================================================================
 * Platform context
 * ============================================================================ */

struct CsSoftPlatformCtx {
    NSWindow *window;
    CsSoftView *view;
    CsSoftWindowDelegate *delegate;

    uint8_t *pixels;
    int width;
    int height;
    int stride;

    bool should_close;
    bool vsync;

    /* Event queue (ring buffer) */
    CsSoftEvent events[64];
    int event_head;
    int event_tail;
};

/* ============================================================================
 * Event queue helpers
 * ============================================================================ */

static void push_event(CsSoftPlatformCtx *ctx, CsSoftEvent *event)
{
    int next = (ctx->event_tail + 1) % 64;
    if (next != ctx->event_head) {
        ctx->events[ctx->event_tail] = *event;
        ctx->event_tail = next;
    }
}

static bool pop_event(CsSoftPlatformCtx *ctx, CsSoftEvent *event)
{
    if (ctx->event_head == ctx->event_tail) {
        return false;
    }
    *event = ctx->events[ctx->event_head];
    ctx->event_head = (ctx->event_head + 1) % 64;
    return true;
}

/* ============================================================================
 * Key code mapping
 * ============================================================================ */

static int map_keycode(unsigned short keyCode)
{
    /* macOS virtual key codes to CsKeyCode */
    switch (keyCode) {
        case 0x33: return CS_KEY_BACKSPACE;
        case 0x30: return CS_KEY_TAB;
        case 0x24: return CS_KEY_ENTER;
        case 0x35: return CS_KEY_ESCAPE;
        case 0x31: return CS_KEY_SPACE;
        case 0x7B: return CS_KEY_LEFT;
        case 0x7E: return CS_KEY_UP;
        case 0x7C: return CS_KEY_RIGHT;
        case 0x7D: return CS_KEY_DOWN;
        case 0x75: return CS_KEY_DELETE;
        case 0x73: return CS_KEY_HOME;
        case 0x77: return CS_KEY_END;
        case 0x74: return CS_KEY_PAGE_UP;
        case 0x79: return CS_KEY_PAGE_DOWN;
        case 0x00: return CS_KEY_A;
        case 0x08: return CS_KEY_C;
        case 0x09: return CS_KEY_V;
        case 0x07: return CS_KEY_X;
        case 0x06: return CS_KEY_Z;
        default:   return CS_KEY_UNKNOWN;
    }
}

/* ============================================================================
 * Custom NSView
 * ============================================================================ */

@interface CsSoftView : NSView {
    CsSoftPlatformCtx *_ctx;
    NSTrackingArea *_trackingArea;
}
- (instancetype)initWithFrame:(NSRect)frame context:(CsSoftPlatformCtx *)ctx;
@end

@implementation CsSoftView

- (instancetype)initWithFrame:(NSRect)frame context:(CsSoftPlatformCtx *)ctx
{
    self = [super initWithFrame:frame];
    if (self) {
        _ctx = ctx;
        _trackingArea = nil;
    }
    return self;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)canBecomeKeyView
{
    return YES;
}

- (void)updateTrackingAreas
{
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
    }

    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:self.bounds
        options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
        owner:self
        userInfo:nil];
    [self addTrackingArea:_trackingArea];

    [super updateTrackingAreas];
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;

    if (!_ctx || !_ctx->pixels) return;

    /* Use NSBitmapImageRep - simpler and handles coordinates properly */
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:&_ctx->pixels
        pixelsWide:_ctx->width
        pixelsHigh:_ctx->height
        bitsPerSample:8
        samplesPerPixel:4
        hasAlpha:YES
        isPlanar:NO
        colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:_ctx->stride
        bitsPerPixel:32];

    if (!bitmap) return;

    /* Draw directly - no transform, let's see what happens */
    NSRect bounds = self.bounds;
    [bitmap drawInRect:NSMakeRect(0, 0, bounds.size.width, bounds.size.height)];
}

/* Keyboard events */
- (void)keyDown:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_KEY_DOWN;
    e.key.key_code = map_keycode(event.keyCode);
    e.key.shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    e.key.ctrl = (event.modifierFlags & NSEventModifierFlagCommand) != 0;  /* Cmd as Ctrl */
    e.key.alt = (event.modifierFlags & NSEventModifierFlagOption) != 0;
    push_event(_ctx, &e);

    /* Also send character input if printable */
    NSString *chars = event.characters;
    if (chars.length > 0) {
        unichar c = [chars characterAtIndex:0];
        if (c >= 32 && c < 127) {  /* Printable ASCII */
            CsSoftEvent charEvent = {0};
            charEvent.type = CS_SOFT_EVENT_CHAR;
            charEvent.char_input.codepoint = c;
            push_event(_ctx, &charEvent);
        }
    }
}

- (void)keyUp:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_KEY_UP;
    e.key.key_code = map_keycode(event.keyCode);
    e.key.shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    e.key.ctrl = (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    e.key.alt = (event.modifierFlags & NSEventModifierFlagOption) != 0;
    push_event(_ctx, &e);
}

/* Mouse events - flip Y from window coords (bottom-up) to buffer coords (top-down) */
static void get_mouse_coords(CsSoftView *view, NSEvent *event, int *outX, int *outY)
{
    /* Window coords: origin at bottom-left, Y increases up */
    /* Buffer coords: origin at top-left, Y increases down */
    NSPoint loc = event.locationInWindow;
    NSRect bounds = view.bounds;
    *outX = (int)loc.x;
    *outY = (int)(bounds.size.height - loc.y);
}

- (void)mouseDown:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_MOUSE_DOWN;
    get_mouse_coords(self, event, &e.mouse.x, &e.mouse.y);
    e.mouse.button = 0;  /* Left */
    push_event(_ctx, &e);
}

- (void)mouseUp:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_MOUSE_UP;
    get_mouse_coords(self, event, &e.mouse.x, &e.mouse.y);
    e.mouse.button = 0;
    push_event(_ctx, &e);
}

- (void)rightMouseDown:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_MOUSE_DOWN;
    get_mouse_coords(self, event, &e.mouse.x, &e.mouse.y);
    e.mouse.button = 2;  /* Right */
    push_event(_ctx, &e);
}

- (void)rightMouseUp:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_MOUSE_UP;
    get_mouse_coords(self, event, &e.mouse.x, &e.mouse.y);
    e.mouse.button = 2;
    push_event(_ctx, &e);
}

- (void)mouseMoved:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_MOUSE_MOVE;
    get_mouse_coords(self, event, &e.mouse.x, &e.mouse.y);
    e.mouse.button = -1;
    push_event(_ctx, &e);
}

- (void)mouseDragged:(NSEvent *)event
{
    [self mouseMoved:event];
}

- (void)rightMouseDragged:(NSEvent *)event
{
    [self mouseMoved:event];
}

- (void)scrollWheel:(NSEvent *)event
{
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_SCROLL;
    e.scroll.delta_x = (float)event.scrollingDeltaX;
    e.scroll.delta_y = (float)event.scrollingDeltaY;
    push_event(_ctx, &e);
}

@end

/* ============================================================================
 * Window delegate
 * ============================================================================ */

@interface CsSoftWindowDelegate : NSObject <NSWindowDelegate> {
    CsSoftPlatformCtx *_ctx;
}
- (instancetype)initWithContext:(CsSoftPlatformCtx *)ctx;
@end

@implementation CsSoftWindowDelegate

- (instancetype)initWithContext:(CsSoftPlatformCtx *)ctx
{
    self = [super init];
    if (self) {
        _ctx = ctx;
    }
    return self;
}

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    CsSoftEvent e = {0};
    e.type = CS_SOFT_EVENT_CLOSE;
    push_event(_ctx, &e);
    _ctx->should_close = true;
    return NO;  /* We handle closing ourselves */
}

- (void)windowDidResize:(NSNotification *)notification
{
    NSWindow *window = notification.object;
    NSRect frame = [window.contentView frame];

    int new_width = (int)frame.size.width;
    int new_height = (int)frame.size.height;

    if (new_width != _ctx->width || new_height != _ctx->height) {
        /* Reallocate pixel buffer */
        free(_ctx->pixels);
        _ctx->width = new_width;
        _ctx->height = new_height;
        _ctx->stride = new_width * 4;
        _ctx->pixels = calloc(new_width * new_height, 4);

        CsSoftEvent e = {0};
        e.type = CS_SOFT_EVENT_RESIZE;
        e.resize.width = new_width;
        e.resize.height = new_height;
        push_event(_ctx, &e);
    }
}

@end

/* ============================================================================
 * Platform API
 * ============================================================================ */

CsSoftPlatformCtx *cs_soft_cocoa_create(int w, int h, const char *title, bool vsync)
{
    /* Ensure NSApplication is initialized */
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    CsSoftPlatformCtx *ctx = calloc(1, sizeof(CsSoftPlatformCtx));
    if (!ctx) return NULL;

    ctx->width = w;
    ctx->height = h;
    ctx->stride = w * 4;
    ctx->vsync = vsync;
    ctx->pixels = calloc(w * h, 4);
    if (!ctx->pixels) {
        free(ctx);
        return NULL;
    }

    /* Create window */
    NSRect frame = NSMakeRect(100, 100, w, h);
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                               NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable |
                               NSWindowStyleMaskResizable;

    ctx->window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:style
        backing:NSBackingStoreBuffered
        defer:NO];

    if (!ctx->window) {
        free(ctx->pixels);
        free(ctx);
        return NULL;
    }

    NSString *nsTitle = [NSString stringWithUTF8String:title ? title : "ClayShards"];
    [ctx->window setTitle:nsTitle];
    [ctx->window setAcceptsMouseMovedEvents:YES];

    /* Create view */
    ctx->view = [[CsSoftView alloc] initWithFrame:frame context:ctx];
    [ctx->window setContentView:ctx->view];
    [ctx->window makeFirstResponder:ctx->view];

    /* Set delegate */
    ctx->delegate = [[CsSoftWindowDelegate alloc] initWithContext:ctx];
    [ctx->window setDelegate:ctx->delegate];

    /* Show window */
    [ctx->window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    return ctx;
}

void cs_soft_cocoa_destroy(CsSoftPlatformCtx *ctx)
{
    if (!ctx) return;

    if (ctx->window) {
        [ctx->window close];
    }

    free(ctx->pixels);
    free(ctx);
}

bool cs_soft_cocoa_poll_event(CsSoftPlatformCtx *ctx, CsSoftEvent *event)
{
    if (!ctx) return false;

    /* Process pending Cocoa events */
    @autoreleasepool {
        NSEvent *nsEvent;
        while ((nsEvent = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:nil
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES])) {
            [NSApp sendEvent:nsEvent];
        }
    }

    /* Return next event from our queue */
    return pop_event(ctx, event);
}

uint8_t *cs_soft_cocoa_get_framebuffer(CsSoftPlatformCtx *ctx)
{
    return ctx ? ctx->pixels : NULL;
}

void cs_soft_cocoa_present(CsSoftPlatformCtx *ctx)
{
    if (!ctx || !ctx->view) return;

    [ctx->view setNeedsDisplay:YES];
    [ctx->view displayIfNeeded];
}

bool cs_soft_cocoa_should_close(CsSoftPlatformCtx *ctx)
{
    return ctx ? ctx->should_close : true;
}

void cs_soft_cocoa_request_close(CsSoftPlatformCtx *ctx)
{
    if (ctx) {
        ctx->should_close = true;
    }
}

bool cs_soft_cocoa_resize(CsSoftPlatformCtx *ctx, int w, int h)
{
    if (!ctx || w <= 0 || h <= 0) return false;

    /* Resize window */
    NSRect frame = ctx->window.frame;
    NSRect content = [ctx->window contentRectForFrameRect:frame];
    content.size.width = w;
    content.size.height = h;
    frame = [ctx->window frameRectForContentRect:content];
    [ctx->window setFrame:frame display:YES];

    /* Buffer will be resized via delegate callback */
    return true;
}

void cs_soft_cocoa_get_size(CsSoftPlatformCtx *ctx, int *w, int *h)
{
    if (!ctx) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (w) *w = ctx->width;
    if (h) *h = ctx->height;
}

#endif /* __APPLE__ */
