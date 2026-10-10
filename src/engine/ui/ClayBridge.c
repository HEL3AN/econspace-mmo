/* Clay, compiled once, in C (#297). See ClayBridge.h for why it is behind a bridge. */
#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "ui/ClayBridge.h"

#include <stdlib.h>
#include <string.h>

struct UiClay
{
    Clay_Context*   context;
    void*           memory;
    UiClayMeasureFn measure;
    UiClayErrorFn   error;
    void*           user;
    UiClayCommand*  commands;
    int32_t         capacity;
};

static Clay_Dimensions Measure(Clay_StringSlice text, Clay_TextElementConfig* config, void* user)
{
    UiClay*    c = (UiClay*)user;
    UiClaySize s = c->measure(text.chars, text.length, config->fontId, config->fontSize, c->user);
    Clay_Dimensions d;
    d.width = s.w;
    d.height = s.h;
    return d;
}

static void Error(Clay_ErrorData e)
{
    UiClay* c = (UiClay*)e.userData;
    if (c != NULL && c->error != NULL)
        c->error(e.errorText.chars, e.errorText.length, c->user);
}

UiClay* uiclay_create(int32_t maxElements, UiClayMeasureFn measure, UiClayErrorFn error, void* user)
{
    UiClay* c = (UiClay*)calloc(1, sizeof(UiClay));
    if (c == NULL)
        return NULL;
    c->measure = measure;
    c->error = error;
    c->user = user;

    /* A new context takes its element count from the current one, so the current one is
     * cleared while this one is sized; every call that follows sets its own anyway. */
    Clay_SetCurrentContext(NULL);
    Clay_SetMaxElementCount(maxElements);
    uint32_t size = Clay_MinMemorySize();
    c->memory = malloc(size);
    if (c->memory == NULL)
    {
        free(c);
        return NULL;
    }
    Clay_Arena        arena = Clay_CreateArenaWithCapacityAndMemory(size, c->memory);
    Clay_Dimensions   dims = { 1.0f, 1.0f };
    Clay_ErrorHandler handler = { Error, c };
    c->context = Clay_Initialize(arena, dims, handler);
    if (c->context == NULL)
    {
        free(c->memory);
        free(c);
        return NULL;
    }
    Clay_SetMeasureTextFunction(Measure, c);
    return c;
}

void uiclay_destroy(UiClay* c)
{
    if (c == NULL)
        return;
    if (Clay_GetCurrentContext() == c->context)
        Clay_SetCurrentContext(NULL);
    free(c->commands);
    free(c->memory);
    free(c);
}

void uiclay_begin(UiClay* c, float w, float h, float pointerX, float pointerY, int pointerDown,
                  float wheelY, float dt)
{
    Clay_SetCurrentContext(c->context);
    /* The measuring function is one for every context; only its user data is per context. */
    Clay_SetMeasureTextFunction(Measure, c);
    Clay_Dimensions dims = { w, h };
    Clay_SetLayoutDimensions(dims);
    Clay_Vector2 p = { pointerX, pointerY };
    Clay_SetPointerState(p, pointerDown != 0);
    Clay_Vector2 wheel = { 0.0f, wheelY };
    Clay_UpdateScrollContainers(false, wheel, dt);
    Clay_BeginLayout();
}

static Clay_SizingAxis Axis(UiClayAxis a)
{
    Clay_SizingAxis s;
    memset(&s, 0, sizeof(s));
    switch (a.kind)
    {
        case UICLAY_GROW: s.type = CLAY__SIZING_TYPE_GROW; break;
        case UICLAY_PERCENT: s.type = CLAY__SIZING_TYPE_PERCENT; break;
        case UICLAY_FIXED: s.type = CLAY__SIZING_TYPE_FIXED; break;
        default: s.type = CLAY__SIZING_TYPE_FIT; break;
    }
    if (a.kind == UICLAY_PERCENT)
        s.size.percent = a.min;
    else if (a.kind == UICLAY_FIXED)
    {
        s.size.minMax.min = a.min;
        s.size.minMax.max = a.min;
    }
    else
    {
        s.size.minMax.min = a.min;
        s.size.minMax.max = a.max > 0.0f ? a.max : CLAY__MAXFLOAT;
    }
    return s;
}

static Clay_Color Rgba(const float c[4])
{
    Clay_Color out = { c[0], c[1], c[2], c[3] };
    return out;
}

void uiclay_open(const UiClayElement* e)
{
    Clay_ElementDeclaration d;
    memset(&d, 0, sizeof(d));
    if (e->id != NULL && e->idLength > 0)
    {
        Clay_String s = { false, e->idLength, e->id };
        d.id = Clay__HashString(s, e->idIndex, 0);
    }
    d.layout.layoutDirection = e->column ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT;
    d.layout.sizing.width = Axis(e->width);
    d.layout.sizing.height = Axis(e->height);
    d.layout.padding.left = e->padLeft;
    d.layout.padding.right = e->padRight;
    d.layout.padding.top = e->padTop;
    d.layout.padding.bottom = e->padBottom;
    d.layout.childGap = e->gap;
    d.layout.childAlignment.x = e->alignX == UICLAY_CENTER ? CLAY_ALIGN_X_CENTER
                                : e->alignX == UICLAY_END  ? CLAY_ALIGN_X_RIGHT
                                                           : CLAY_ALIGN_X_LEFT;
    d.layout.childAlignment.y = e->alignY == UICLAY_CENTER ? CLAY_ALIGN_Y_CENTER
                                : e->alignY == UICLAY_END  ? CLAY_ALIGN_Y_BOTTOM
                                                           : CLAY_ALIGN_Y_TOP;
    d.backgroundColor = Rgba(e->fill);
    d.cornerRadius.topLeft = d.cornerRadius.topRight = e->radius;
    d.cornerRadius.bottomLeft = d.cornerRadius.bottomRight = e->radius;
    if (e->borderWidth > 0)
    {
        d.border.color = Rgba(e->borderColor);
        d.border.width.left = d.border.width.right = e->borderWidth;
        d.border.width.top = d.border.width.bottom = e->borderWidth;
    }

    Clay__OpenElement();
    if (e->scrollY)
    {
        d.clip.vertical = true;
        d.clip.childOffset = Clay_GetScrollOffset(); /* of the element just opened */
    }
    Clay__ConfigureOpenElement(d);
}

void uiclay_close(void)
{
    Clay__CloseElement();
}

void uiclay_text(const char* text, int32_t length, const UiClayText* t)
{
    Clay_TextElementConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.textColor = Rgba(t->color);
    cfg.fontId = t->fontId;
    cfg.fontSize = t->fontSize;
    cfg.lineHeight = t->lineHeight;
    cfg.wrapMode = t->wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE;
    cfg.textAlignment = t->align == UICLAY_CENTER ? CLAY_TEXT_ALIGN_CENTER
                        : t->align == UICLAY_END  ? CLAY_TEXT_ALIGN_RIGHT
                                                  : CLAY_TEXT_ALIGN_LEFT;
    Clay_String s = { false, length, text };
    Clay__OpenTextElement(s, Clay__StoreTextElementConfig(cfg));
}

int32_t uiclay_end(UiClay* c, const UiClayCommand** out)
{
    Clay_RenderCommandArray cmds = Clay_EndLayout();
    if (cmds.length > c->capacity)
    {
        free(c->commands);
        c->capacity = cmds.length + 64;
        c->commands = (UiClayCommand*)malloc(sizeof(UiClayCommand) * (size_t)c->capacity);
        if (c->commands == NULL)
        {
            c->capacity = 0;
            *out = NULL;
            return 0;
        }
    }
    int32_t n = 0;
    for (int32_t i = 0; i < cmds.length; i++)
    {
        const Clay_RenderCommand* rc = Clay_RenderCommandArray_Get(&cmds, i);
        UiClayCommand*            o = &c->commands[n];
        memset(o, 0, sizeof(*o));
        o->x = rc->boundingBox.x;
        o->y = rc->boundingBox.y;
        o->w = rc->boundingBox.width;
        o->h = rc->boundingBox.height;
        o->id = rc->id;
        switch (rc->commandType)
        {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
            {
                const Clay_RectangleRenderData* r = &rc->renderData.rectangle;
                o->kind = UICLAY_CMD_RECT;
                memcpy(o->color, &r->backgroundColor, sizeof(o->color));
                o->radius = r->cornerRadius.topLeft;
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_BORDER:
            {
                const Clay_BorderRenderData* b = &rc->renderData.border;
                o->kind = UICLAY_CMD_BORDER;
                memcpy(o->color, &b->color, sizeof(o->color));
                o->radius = b->cornerRadius.topLeft;
                o->borderWidth = b->width.left;
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_TEXT:
            {
                const Clay_TextRenderData* t = &rc->renderData.text;
                o->kind = UICLAY_CMD_TEXT;
                memcpy(o->color, &t->textColor, sizeof(o->color));
                o->text = t->stringContents.chars;
                o->textLength = t->stringContents.length;
                o->fontId = t->fontId;
                o->fontSize = t->fontSize;
                o->lineHeight = t->lineHeight;
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: o->kind = UICLAY_CMD_CLIP_BEGIN; break;
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: o->kind = UICLAY_CMD_CLIP_END; break;
            default: continue; /* images and custom elements: not used yet */
        }
        n++;
    }
    *out = c->commands;
    return n;
}

int uiclay_pointer_over(UiClay* c, const char* id, int32_t length, uint32_t index)
{
    Clay_SetCurrentContext(c->context);
    Clay_String s = { false, length, id };
    return Clay_PointerOver(Clay_GetElementIdWithIndex(s, index)) ? 1 : 0;
}

int uiclay_box(UiClay* c, const char* id, int32_t length, uint32_t index, float out[4])
{
    Clay_SetCurrentContext(c->context);
    Clay_String      s = { false, length, id };
    Clay_ElementData d = Clay_GetElementData(Clay_GetElementIdWithIndex(s, index));
    if (!d.found)
        return 0;
    out[0] = d.boundingBox.x;
    out[1] = d.boundingBox.y;
    out[2] = d.boundingBox.width;
    out[3] = d.boundingBox.height;
    return 1;
}
