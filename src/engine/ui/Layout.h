#pragma once

#include "raylib.h"
#include "ui/Fonts.h"
#include "ui/Input.h"
#include "ui/Theme.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct UiClay;  // ClayBridge.h; nothing outside Layout.cpp sees Clay

// Laying out a window's content (#297): rows and columns that fit, grow and pad, the way a
// flexbox does, with text measured in our font. Clay does the solving; this is the API the
// client is written against, so Clay can be replaced without touching a screen.
//
// Immediate mode. A window declares its whole content every frame, and what was hovered or
// clicked is asked about by id against where the previous frame put things -- one frame
// behind, which nobody can see at sixty frames a second. Sizes are in layout units and are
// scaled here, so a screen never multiplies by Ui::Scale itself.
//
//     layout.Begin(frame);
//     layout.Column(Box().Grow().Gap(t.metrics.gap), [&] {
//         layout.Text("HULL", TextStyle::Label());
//         layout.Bar(hull, t.colors.good);
//         if (layout.Button("undock", "Undock"))
//             Undock();
//     });
//     layout.End();
//     layout.Draw();
//
// See documents/ui.md.
namespace Ui
{

// How big an element is along one axis.
struct Size
{
    enum class Kind
    {
        Fit,      // as big as its children, within [min, max]
        Grow,     // takes what its parent has left, shared with its other growing siblings
        Fixed,    // exactly this
        Percent,  // this fraction of its parent
    };
    Kind  kind = Kind::Fit;
    float min = 0.0f;  // units; the fraction for Percent
    float max = 0.0f;  // units; 0 for unbounded

    static Size Fit(float min = 0.0f, float max = 0.0f) { return { Kind::Fit, min, max }; }
    static Size Grow(float min = 0.0f, float max = 0.0f) { return { Kind::Grow, min, max }; }
    static Size Fixed(float units) { return { Kind::Fixed, units, units }; }
    static Size Percent(float fraction) { return { Kind::Percent, fraction, 0.0f }; }
};

enum class Align
{
    Start,
    Center,
    End,
};

// One element: a box that lays out its children in a row or a column.
struct Box
{
    std::string id;  // needed only to ask about it later: Hovered, Clicked, BoxOf, scrolling
    uint32_t    index = 0;
    bool        column = false;
    Size        width, height;
    float       padLeft = 0.0f, padRight = 0.0f, padTop = 0.0f, padBottom = 0.0f;
    float       gap = 0.0f;
    Ui::Align   alignX = Ui::Align::Start, alignY = Ui::Align::Start;
    Color       fill{ 0, 0, 0, 0 };
    Color       borderColor{ 0, 0, 0, 0 };
    float       border = 0.0f;
    float       radius = 0.0f;
    bool        scrollY = false;

    Box& Id(std::string_view s, uint32_t i = 0)
    {
        id.assign(s);
        index = i;
        return *this;
    }
    Box& Column()
    {
        column = true;
        return *this;
    }
    Box& Width(Size s)
    {
        width = s;
        return *this;
    }
    Box& Height(Size s)
    {
        height = s;
        return *this;
    }
    Box& Grow() { return Width(Size::Grow()).Height(Size::Grow()); }
    Box& GrowX() { return Width(Size::Grow()); }
    Box& Pad(float all) { return Pad(all, all, all, all); }
    Box& Pad(float x, float y) { return Pad(x, x, y, y); }
    Box& Pad(float l, float r, float t, float b)
    {
        padLeft = l, padRight = r, padTop = t, padBottom = b;
        return *this;
    }
    Box& Gap(float g)
    {
        gap = g;
        return *this;
    }
    Box& Align(Ui::Align x, Ui::Align y)
    {
        alignX = x, alignY = y;
        return *this;
    }
    Box& Fill(Color c)
    {
        fill = c;
        return *this;
    }
    Box& Border(Color c, float w = 1.0f)
    {
        borderColor = c, border = w;
        return *this;
    }
    Box& Radius(float r)
    {
        radius = r;
        return *this;
    }
    Box& ScrollY()
    {
        scrollY = true;
        return *this;
    }
};

// How a run of text looks. The named ones come from the theme.
struct TextStyle
{
    Face  face = Face::Regular;
    float size = 14.0f;  // units
    Color color{ 255, 255, 255, 255 };
    bool  wrap = false;  // at words, to the width it is given; otherwise one line
    Align align = Align::Start;

    static TextStyle Small();
    static TextStyle Label();    // what a value is: small and dim
    static TextStyle Body();     // the value, and running text
    static TextStyle Strong();   // the body size in the strong face
    static TextStyle Title();    // a block's heading
    static TextStyle Heading();  // a screen's heading

    TextStyle& Tint(Color c)
    {
        color = c;
        return *this;
    }
    TextStyle& Wrap()
    {
        wrap = true;
        return *this;
    }
    TextStyle& Aligned(Align a)
    {
        align = a;
        return *this;
    }
};

// What End() produces: rectangles, outlines, text and clipping, in screen pixels.
struct DrawCommand
{
    enum class Kind
    {
        Rect,
        Border,
        Text,
        ClipBegin,
        ClipEnd,
    };
    Kind             kind = Kind::Rect;
    Rectangle        box{ 0.0f, 0.0f, 0.0f, 0.0f };
    Color            color{ 0, 0, 0, 0 };
    float            radius = 0.0f;
    float            width = 0.0f;  // of an outline
    std::string_view text;
    Face             face = Face::Regular;
    float            px = 0.0f;  // the font size, already scaled
};

class Layout
{
public:
    // Measures text: by default in our font. A test hands in its own, since measuring a
    // real font needs a window.
    using Measure = std::function<Vector2(Face face, float px, std::string_view text)>;

    explicit Layout(int maxElements = 2048);
    ~Layout();
    Layout(const Layout&) = delete;
    Layout& operator=(const Layout&) = delete;

    void SetMeasure(Measure m) { measure_ = std::move(m); }

    // Starts a frame inside a window, at the current UI scale; the mouse is the window's.
    void Begin(const Frame& frame);
    // The same with everything stated: where, how big a unit is, and the mouse as far as
    // this layout is concerned (owned = false: no hover, no clicks, no wheel).
    struct Pointer
    {
        bool    owned = false;
        Vector2 pos{ 0.0f, 0.0f };
        bool    down = false;
        bool    pressed = false;
        float   wheel = 0.0f;
    };
    void Begin(Rectangle area, float scale, const Pointer& pointer);
    void Begin(Rectangle area, float scale);  // no pointer at all: a test, or a picture
    // Solves the layout; the commands are valid until the next Begin.
    const std::vector<DrawCommand>& End();
    void                            Draw() const;  // draws End()'s commands through raylib

    // --- Structure -----------------------------------------------------------------
    void                       Open(const Box& box);
    void                       Close();
    template <typename F> void Row(Box box, F&& children)
    {
        box.column = false;
        Open(box);
        children();
        Close();
    }
    template <typename F> void Column(Box box, F&& children)
    {
        box.column = true;
        Open(box);
        children();
        Close();
    }
    void Text(std::string_view text, const TextStyle& style);
    // Empty space: `units` along both axes, or, with none, whatever is left (pushes what
    // follows to the far end of a row).
    void Spacer(float units = 0.0f);

    // --- Widgets: the beginning of the library --------------------------------------
    void Bar(float fraction, Color color);  // a progress bar the width of its parent
    // A label on the left and its value on the right, on one line.
    void Field(std::string_view label, std::string_view value, Color valueColor);
    void Divider();
    // Returns true on the frame it was clicked.
    bool Button(std::string_view id, std::string_view label, bool highlighted = false);
    // A small on/off indicator: filled when on.
    void Chip(std::string_view label, bool on, Color onColor);

    // --- Asking about it -----------------------------------------------------------
    bool      Hovered(std::string_view id, uint32_t index = 0) const;
    bool      Clicked(std::string_view id, uint32_t index = 0) const;
    Rectangle BoxOf(std::string_view id, uint32_t index = 0) const;  // after End; screen
    float     U(float units) const;                                  // units to pixels
    float     ScaleFactor() const { return scale_; }
    Rectangle Area() const { return area_; }
    // The last thing Clay complained about (a duplicate id, too many elements); empty if
    // nothing has gone wrong.
    const std::string& LastError() const { return lastError_; }

private:
    std::string_view Keep(std::string_view s);  // alive until the next Begin
    void             Report(std::string_view message);

    UiClay*                  clay_ = nullptr;
    Measure                  measure_;
    std::deque<std::string>  strings_;
    std::vector<DrawCommand> commands_;
    Rectangle                area_{ 0.0f, 0.0f, 0.0f, 0.0f };
    float                    scale_ = 1.0f;
    Pointer                  pointer_;
    int                      depth_ = 0;
    bool                     open_ = false;
    std::string              lastError_;

    friend struct LayoutCallbacks;
};

}  // namespace Ui
