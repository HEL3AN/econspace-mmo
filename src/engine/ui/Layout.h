#pragma once

#include "raylib.h"
#include "ui/Focus.h"
#include "ui/Fonts.h"
#include "ui/Input.h"
#include "ui/Theme.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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
    // Clips its children and scrolls them with the wheel. Clay keeps at most ten clipping
    // elements per layout, so this is for a window's list, not for every cell of it: a
    // table cuts its cells short with Ellipsize instead.
    bool scrollY = false;

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

// --- What the widgets take and give ----------------------------------------------------

// A table's column: its heading, how wide it is (the same rule as a box's), where its text
// sits, and whether clicking the heading sorts by it.
struct TableColumn
{
    std::string label;
    Size        width = Size::Grow();
    Align       align = Align::Start;
    bool        sortable = false;
};

// What a cell says, and in what colour.
struct Cell
{
    std::string text;
    Color       color{ 255, 255, 255, 255 };
    Face        face = Face::Regular;
};

// Which column a table is sorted by. The table changes it when a heading is clicked; the
// rows themselves come from the caller's model, which reads it.
struct TableSort
{
    int  column = 0;
    bool descending = false;
};

struct TableSpec
{
    std::string                              id;  // unique in its layout
    std::vector<TableColumn>                 columns;
    int                                      rows = 0;
    std::function<Cell(int row, int column)> cell;
    // A row's background: the selected one, the one being flown to; alpha 0 for none.
    std::function<Color(int row)> rowFill;
    // Optional. Clicking a sortable heading sorts by it; clicking it again turns the order
    // round only if `reversible` -- the overview's model always puts hostiles first, and
    // a reversed list would put them last.
    TableSort*  sort = nullptr;
    bool        reversible = false;
    std::string empty;  // said instead of rows when there are none
    // Optional: what a row says when the cursor rests on it -- the whole of a name cut short.
    std::function<std::string(int row)> tooltip;
};

// What happened to a table this frame; -1 for nothing.
struct TableEvents
{
    int  hovered = -1;
    int  clicked = -1;       // left button
    int  rightClicked = -1;  // right button: the place for a context menu
    bool sorted = false;     // the sort changed
};

struct TextFieldOptions
{
    std::string placeholder;  // shown dim while it is empty and not being typed in
    size_t      maxLength = 64;
    Size        width = Size::Grow();
    bool (*accept)(char) = nullptr;  // which characters it takes; null for every printable one
};

// The tooltip asked for this frame, drawn over everything by DrawTooltip at the end of the
// frame -- after the popups, so no window can sit on top of it. PendingTooltip is for tests.
void               PostTooltip(std::string_view text, Vector2 at);
void               DrawTooltip();
const std::string& PendingTooltip();
void               ClearTooltip();

// Shortens text to fit `maxPx`, ending it in "..": a name that runs into the next column
// reads as one word with it. `width` measures a string in pixels.
std::string Ellipsize(std::string_view text, float maxPx,
                      const std::function<float(std::string_view)>& width);

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
        bool    down = false;     // the left button
        bool    pressed = false;  // ...went down this frame
        bool    rightPressed = false;
        float   wheel = 0.0f;
        float   dt = 0.0f;  // seconds since the last frame: tooltips wait, scrolling glides
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

    // --- Widgets for windows that list, choose and type -------------------------------
    // A column that scrolls with the wheel, with an indicator beside it that shows how much
    // there is and where the view is, and can be dragged. `box` needs an id; its children
    // are laid out top to bottom. The indicator's room is kept even when everything fits,
    // so nothing shifts sideways when a list grows past the window.
    template <typename F> void Scroll(Box box, F&& children)
    {
        ScrollBegin(box);
        children();
        ScrollEnd(box);
    }
    float ScrollGutter() const;  // units: the room the indicator takes beside the content

    // Headings (clickable to sort), a divider, and rows that scroll. Each cell's text is
    // cut short with ".." to its column. Returns what the mouse did to it.
    TableEvents Table(const TableSpec& spec);

    // A strip of tabs; `selected` changes when one is clicked. True on that frame.
    bool Tabs(std::string_view id, const std::vector<std::string>& labels, int& selected);

    // A line of text the player types in. Clicking it takes the keyboard (the frame must
    // come from the desk); Enter submits and lets go, Esc lets go (the desk handles Esc).
    EditResult TextField(std::string_view id, std::string& text,
                         const TextFieldOptions& options = TextFieldOptions());
    // Hands the keyboard to a field, as if it had been clicked: N on the map does this.
    void TakeFocus(std::string_view id);
    bool HasFocus(std::string_view id) const;

    // A number typed in. While it is being typed it is text; Enter, or a click anywhere
    // else, takes the value (clamped to [lo, hi]); Esc puts the old one back. True on the
    // frame the value changed.
    bool NumberField(std::string_view id, float& value, float lo, float hi,
                     const char* format = "%.0f");
    // A value on a track, dragged anywhere along it. `logarithmic` spaces it by ratio, for
    // a range or a zoom where 1 to 2 is as large a step as 1000 to 2000 (lo must be > 0).
    // True on the frames the value changed.
    bool Slider(std::string_view id, float& value, float lo, float hi, bool logarithmic = false);

    // A tooltip for the element with this id: shown once it has been under the cursor for a
    // moment, beside the cursor, over everything.
    void Tooltip(std::string_view id, std::string_view text, uint32_t index = 0);

    // A keyboard focus for a layout with no desk behind it (a test), and this frame's typing
    // in place of the real keyboard's.
    void UseFocus(Focus* focus, std::string_view owner);
    void FeedKeys(const KeyInput& keys);

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
    void             ScrollBegin(const Box& box);
    void             ScrollEnd(const Box& box);
    KeyInput         Keys();  // this frame's typing, for the field with the focus
    float            TextWidthPx(const TextStyle& style, std::string_view text) const;
    // The box a text or number field is drawn as.
    void FieldBox(std::string_view id, std::string_view shown, bool dim, bool focused, Size width);

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

    Focus*      focus_ = nullptr;
    std::string owner_;  // the window's id, which the focus is held under
    bool        keysFed_ = false;
    KeyInput    keys_;
    std::string active_;  // a slider or a scroll indicator being dragged, until release
    std::map<std::string, std::string> edits_;   // number fields being typed in: the text so far
    std::string                        tipKey_;  // what the cursor has rested on, and for how long
    float                              tipTime_ = 0.0f;
    bool                               tipSeen_ = false;

    friend struct LayoutCallbacks;
};

}  // namespace Ui
