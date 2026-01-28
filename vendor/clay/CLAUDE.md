# Clay UI Layout Library - Claude Skills

## Overview

Clay is a high-performance 2D UI layout library for C. Use it to create declarative, React-like UI layouts that can be rendered by any backend (HTML, WebGL, SDL, raylib, etc.).

**Key features:**
- Single ~4K LOC header file, zero dependencies
- Microsecond layout performance
- Flexbox-like layout model
- WASM-compatible (15KB compiled)
- Renderer-agnostic output

## Quick Start

```c
#define CLAY_IMPLEMENTATION
#include "clay.h"

// Initialize once
uint64_t memSize = Clay_MinMemorySize();
Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(memSize, malloc(memSize));
Clay_Initialize(arena, (Clay_Dimensions){1920, 1080}, (Clay_ErrorHandler){0});
Clay_SetMeasureTextFunction(MeasureText, 0);

// Every frame
Clay_BeginLayout();

CLAY(CLAY_ID("Root"), {
    .layout = {.padding = CLAY_PADDING_ALL(16)},
    .backgroundColor = {240, 240, 240, 255}
}) {
    CLAY_TEXT(CLAY_STRING("Hello!"), CLAY_TEXT_CONFIG({
        .fontSize = 24,
        .textColor = {0, 0, 0, 255}
    }));
}

Clay_RenderCommandArray commands = Clay_EndLayout();
// Render commands with your backend
```

## Frame Lifecycle

Every frame follows this sequence:

```c
// 1. Update input state
Clay_SetLayoutDimensions((Clay_Dimensions){width, height});
Clay_SetPointerState((Clay_Vector2){mouseX, mouseY}, mouseDown);
Clay_UpdateScrollContainers(true, (Clay_Vector2){scrollX, scrollY}, deltaTime);

// 2. Declare UI
Clay_BeginLayout();
// ... CLAY() macros here ...
Clay_RenderCommandArray commands = Clay_EndLayout();

// 3. Render commands
for (int i = 0; i < commands.length; i++) {
    Clay_RenderCommand *cmd = &commands.internalArray[i];
    // Handle cmd->commandType
}
```

## Element Macros

### CLAY() - Container Element

```c
CLAY(CLAY_ID("MyContainer"), {
    .layout = {
        .layoutDirection = CLAY_TOP_TO_BOTTOM,  // or CLAY_LEFT_TO_RIGHT
        .padding = CLAY_PADDING_ALL(16),
        .childGap = 8,
        .childAlignment = {
            .x = CLAY_ALIGN_X_CENTER,  // LEFT, CENTER, RIGHT
            .y = CLAY_ALIGN_Y_CENTER   // TOP, CENTER, BOTTOM
        },
        .sizing = {
            .width = CLAY_SIZING_GROW(0),
            .height = CLAY_SIZING_FIT(0, 500)
        }
    },
    .backgroundColor = {200, 200, 200, 255},
    .cornerRadius = CLAY_CORNER_RADIUS(8),
    .border = {
        .width = {.left = 1, .right = 1, .top = 1, .bottom = 1},
        .color = {100, 100, 100, 255}
    }
}) {
    // Child elements here
}
```

### CLAY_TEXT() - Text Element

```c
CLAY_TEXT(CLAY_STRING("Hello World"), CLAY_TEXT_CONFIG({
    .fontId = 0,
    .fontSize = 16,
    .textColor = {0, 0, 0, 255},
    .letterSpacing = 1,
    .lineHeight = 20,
    .wrapMode = CLAY_TEXT_WRAP_WORDS  // WORDS, NEWLINES, NONE
}));
```

## ID Macros

```c
CLAY_ID("UniqueString")      // String literal ID (recommended)
CLAY_IDI("Base", index)      // ID with numeric index for loops
CLAY_AUTO_ID()               // Auto-generated based on hierarchy
CLAY_ID_LOCAL("Name")        // Parent-relative ID for components
```

## Sizing Options

```c
CLAY_SIZING_FIXED(300)       // Exact 300px
CLAY_SIZING_GROW(0)          // Fill available space (min 0)
CLAY_SIZING_GROW(100)        // Fill space, minimum 100px
CLAY_SIZING_FIT(0, 500)      // Fit content, max 500px
CLAY_SIZING_PERCENT(0.5)     // 50% of parent
```

## Layout Direction

```c
.layoutDirection = CLAY_TOP_TO_BOTTOM   // Vertical stack
.layoutDirection = CLAY_LEFT_TO_RIGHT   // Horizontal row (default)
```

## Padding Helpers

```c
CLAY_PADDING_ALL(16)                           // All sides 16px
{.left = 8, .right = 8, .top = 4, .bottom = 4} // Individual sides
```

## Scrolling & Clipping

```c
CLAY(CLAY_ID("ScrollContainer"), {
    .layout = {.sizing = {.height = CLAY_SIZING_FIXED(400)}},
    .clip = {
        .vertical = true,
        .childOffset = Clay_GetScrollOffset()
    }
}) {
    // Scrollable content
}
```

## Floating Elements (Tooltips, Modals)

```c
CLAY(CLAY_ID("Tooltip"), {
    .floating = {
        .parentId = CLAY_ID("TriggerElement").id,
        .offset = {0, -8},
        .zIndex = 100,
        .attachment = {
            .element = CLAY_ATTACH_POINT_CENTER_BOTTOM,
            .parent = CLAY_ATTACH_POINT_CENTER_TOP
        }
    },
    .backgroundColor = {50, 50, 50, 255}
}) {
    CLAY_TEXT(CLAY_STRING("Tooltip text"), CLAY_TEXT_CONFIG({...}));
}
```

**Attach points:** `LEFT_TOP`, `LEFT_CENTER`, `LEFT_BOTTOM`, `CENTER_TOP`, `CENTER_CENTER`, `CENTER_BOTTOM`, `RIGHT_TOP`, `RIGHT_CENTER`, `RIGHT_BOTTOM`

## Interaction

### Hover Detection

```c
CLAY(CLAY_ID("Button"), {...}) {
    if (Clay_Hovered()) {
        // Change style on hover
    }
    Clay_OnHover(HandleHover, userData);
}
```

### Query After Layout

```c
if (Clay_PointerOver(CLAY_ID("MyElement"))) {
    // Pointer is over element
}

Clay_ElementData data = Clay_GetElementData(CLAY_ID("MyElement"));
// data.boundingBox.x, .y, .width, .height
```

## Images

```c
CLAY(CLAY_ID("ImageContainer"), {
    .layout = {.sizing = {.width = CLAY_SIZING_FIXED(200)}},
    .image = {.imageData = &myTexture},
    .aspectRatio = 16.0f / 9.0f
}) {}
```

## Custom Elements

```c
CLAY(CLAY_ID("3DViewport"), {
    .custom = {.customData = &my3DSceneData}
}) {}
```

Generates `CLAY_RENDER_COMMAND_TYPE_CUSTOM` - handle in your renderer.

## Render Command Types

```c
switch (cmd->commandType) {
    case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
        // cmd->renderData.rectangle.backgroundColor
        // cmd->renderData.rectangle.cornerRadius
        break;
    case CLAY_RENDER_COMMAND_TYPE_TEXT:
        // cmd->renderData.text.stringContents
        // cmd->renderData.text.textColor
        // cmd->renderData.text.fontId, fontSize
        break;
    case CLAY_RENDER_COMMAND_TYPE_IMAGE:
        // cmd->renderData.image.imageData
        break;
    case CLAY_RENDER_COMMAND_TYPE_BORDER:
        // cmd->renderData.border.color
        // cmd->renderData.border.width
        break;
    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
        // Enable clipping to cmd->boundingBox
        break;
    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
        // Disable clipping
        break;
    case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
        // cmd->renderData.custom.customData
        break;
}
```

## Text Measurement Function

**Required** - must be set before layout:

```c
Clay_Dimensions MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, uintptr_t userData) {
    // Calculate text dimensions based on font
    float width = 0;
    for (int i = 0; i < text.length; i++) {
        width += GetCharWidth(text.chars[i], config->fontId, config->fontSize);
    }
    return (Clay_Dimensions){width, config->fontSize};
}

Clay_SetMeasureTextFunction(MeasureText, 0);
```

## Reusable Components

```c
void Button(const char *label, void (*onClick)(void)) {
    CLAY(CLAY_AUTO_ID(), {
        .layout = {.padding = CLAY_PADDING_ALL(12)},
        .backgroundColor = Clay_Hovered() ?
            (Clay_Color){100, 150, 255, 255} :
            (Clay_Color){80, 120, 200, 255},
        .cornerRadius = CLAY_CORNER_RADIUS(4)
    }) {
        Clay_OnHover(HandleClick, (void*)onClick);
        CLAY_TEXT(CLAY_STRING(label), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = {255, 255, 255, 255}
        }));
    }
}

// Usage
Button("Submit", OnSubmitClicked);
Button("Cancel", OnCancelClicked);
```

## Dynamic Lists

```c
for (int i = 0; i < items.count; i++) {
    CLAY(CLAY_IDI("ListItem", i), {
        .layout = {.padding = CLAY_PADDING_ALL(8)},
        .backgroundColor = (i % 2 == 0) ?
            (Clay_Color){245, 245, 245, 255} :
            (Clay_Color){255, 255, 255, 255}
    }) {
        CLAY_TEXT(CLAY_STRING(items.data[i].name), CLAY_TEXT_CONFIG({...}));
    }
}
```

## Common Patterns

### Sidebar + Content Layout

```c
CLAY(CLAY_ID("App"), {
    .layout = {
        .layoutDirection = CLAY_LEFT_TO_RIGHT,
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}
    }
}) {
    // Sidebar
    CLAY(CLAY_ID("Sidebar"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = {.width = CLAY_SIZING_FIXED(250), .height = CLAY_SIZING_GROW(0)},
            .padding = CLAY_PADDING_ALL(16),
            .childGap = 8
        },
        .backgroundColor = {40, 40, 40, 255}
    }) {
        // Sidebar items
    }

    // Main content
    CLAY(CLAY_ID("Content"), {
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .padding = CLAY_PADDING_ALL(24)
        },
        .backgroundColor = {250, 250, 250, 255}
    }) {
        // Content
    }
}
```

### Card Component

```c
void Card(const char *title, const char *body) {
    CLAY(CLAY_AUTO_ID(), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = CLAY_PADDING_ALL(16),
            .childGap = 8,
            .sizing = {.width = CLAY_SIZING_FIXED(300)}
        },
        .backgroundColor = {255, 255, 255, 255},
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = {
            .width = {1, 1, 1, 1},
            .color = {220, 220, 220, 255}
        }
    }) {
        CLAY_TEXT(CLAY_STRING(title), CLAY_TEXT_CONFIG({
            .fontSize = 18,
            .textColor = {30, 30, 30, 255}
        }));
        CLAY_TEXT(CLAY_STRING(body), CLAY_TEXT_CONFIG({
            .fontSize = 14,
            .textColor = {100, 100, 100, 255},
            .wrapMode = CLAY_TEXT_WRAP_WORDS
        }));
    }
}
```

## Error Handling

```c
void HandleClayError(Clay_ErrorData error) {
    switch (error.errorType) {
        case CLAY_ERROR_TYPE_TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED:
            fprintf(stderr, "Must call Clay_SetMeasureTextFunction!\n");
            break;
        case CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED:
            fprintf(stderr, "Need more memory for Clay arena\n");
            break;
        case CLAY_ERROR_TYPE_DUPLICATE_ID:
            fprintf(stderr, "Duplicate element ID: %s\n", error.errorText.chars);
            break;
    }
}

Clay_Initialize(arena, dims, (Clay_ErrorHandler){HandleClayError, 0});
```

## WASM Integration

Clay compiles to ~15KB WASM. For browser deployment:

```c
// Export these for JS to call
EMSCRIPTEN_KEEPALIVE void UpdateAndRender(float width, float height,
                                           float mouseX, float mouseY,
                                           bool mouseDown) {
    Clay_SetLayoutDimensions((Clay_Dimensions){width, height});
    Clay_SetPointerState((Clay_Vector2){mouseX, mouseY}, mouseDown);

    Clay_BeginLayout();
    // ... UI declaration ...
    Clay_RenderCommandArray commands = Clay_EndLayout();

    // Pass to JS renderer or use WebGL directly
}
```

## Resources

- **Repository:** https://github.com/nicbarker/clay
- **Live Demo:** https://nicbarker.com/clay
- **Video Intro:** https://youtu.be/DYWTw19_8r4
- **Discord:** https://discord.gg/b4FTWkxdvT
