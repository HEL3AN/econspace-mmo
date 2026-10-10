#ifndef ECONSPACE_UI_CLAY_BRIDGE_H
#define ECONSPACE_UI_CLAY_BRIDGE_H

/* The one place Clay is seen (#297). Clay is C99 and its header refuses C++ before C++20,
 * so it is compiled in a C file and the rest of the client talks to it through the plain
 * structs below. That is also the seam to cut if Clay ever limits us: Ui::Layout is written
 * against this file, not against Clay, and a replacement only has to answer the same
 * calls. Nothing here is meant to be called from anywhere but Layout.cpp. */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        UICLAY_FIT = 0,
        UICLAY_GROW = 1,
        UICLAY_PERCENT = 2,
        UICLAY_FIXED = 3,
    };

    enum
    {
        UICLAY_START = 0,
        UICLAY_CENTER = 1,
        UICLAY_END = 2,
    };

    typedef struct UiClayAxis
    {
        uint8_t kind; /* UICLAY_FIT... */
        float   min;  /* pixels; for FIXED the size itself, for PERCENT the fraction 0..1 */
        float   max;  /* pixels; 0 means unbounded */
    } UiClayAxis;

    typedef struct UiClayElement
    {
        const char* id; /* NULL for an element nobody asks about */
        int32_t     idLength;
        uint32_t    idIndex;
        uint8_t     column; /* children top to bottom rather than left to right */
        UiClayAxis  width, height;
        uint16_t    padLeft, padRight, padTop, padBottom;
        uint16_t    gap;
        uint8_t     alignX, alignY; /* UICLAY_START... */
        float       fill[4];        /* 0..255; alpha 0 draws nothing */
        float       radius;
        float       borderColor[4];
        uint16_t    borderWidth;
        uint8_t     scrollY; /* clips its children and scrolls them with the wheel */
    } UiClayElement;

    typedef struct UiClayText
    {
        uint16_t fontId;
        uint16_t fontSize; /* pixels */
        uint16_t lineHeight;
        float    color[4];
        uint8_t  wrap;  /* at words; otherwise one line */
        uint8_t  align; /* UICLAY_START... */
    } UiClayText;

    enum
    {
        UICLAY_CMD_RECT = 1,
        UICLAY_CMD_BORDER = 2,
        UICLAY_CMD_TEXT = 3,
        UICLAY_CMD_CLIP_BEGIN = 4,
        UICLAY_CMD_CLIP_END = 5,
    };

    typedef struct UiClayCommand
    {
        int         kind;
        float       x, y, w, h;
        float       color[4];
        float       radius;
        float       borderWidth;
        const char* text; /* not terminated: textLength characters */
        int32_t     textLength;
        uint16_t    fontId, fontSize, lineHeight;
        uint32_t    id;
    } UiClayCommand;

    typedef struct UiClaySize
    {
        float w, h;
    } UiClaySize;

    typedef UiClaySize (*UiClayMeasureFn)(const char* text, int32_t length, uint16_t fontId,
                                          uint16_t fontSize, void* user);
    typedef void (*UiClayErrorFn)(const char* text, int32_t length, void* user);

    typedef struct UiClay UiClay;

    /* One layout context: a window's own, so hover and scroll positions are its own. */
    UiClay* uiclay_create(int32_t maxElements, UiClayMeasureFn measure, UiClayErrorFn error,
                          void* user);
    void    uiclay_destroy(UiClay* c);

    /* Starts a layout of w x h pixels. The pointer is relative to the layout's top left;
     * pass a point far outside it when the layout does not own the mouse. */
    void uiclay_begin(UiClay* c, float w, float h, float pointerX, float pointerY, int pointerDown,
                      float wheelY, float dt);
    void uiclay_open(const UiClayElement* e);
    void uiclay_close(void);
    /* The text must stay alive until uiclay_end's commands have been drawn. */
    void uiclay_text(const char* text, int32_t length, const UiClayText* t);
    /* Lays out and returns the commands, valid until the next uiclay_begin on this context. */
    int32_t uiclay_end(UiClay* c, const UiClayCommand** out);

    /* Is the pointer over the element with this id (as the last layout placed it, asked between
     * begin and end), and where is it (asked after end). */
    int uiclay_pointer_over(UiClay* c, const char* id, int32_t length, uint32_t index);
    int uiclay_box(UiClay* c, const char* id, int32_t length, uint32_t index, float out[4]);

#ifdef __cplusplus
}
#endif

#endif
