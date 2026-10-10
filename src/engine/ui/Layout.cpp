#include "ui/Layout.h"

#include "ui/ClayBridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Ui
{

struct LayoutCallbacks
{
    static UiClaySize Measure(const char* text, int32_t length, uint16_t fontId, uint16_t fontSize,
                              void* user)
    {
        const Layout* l = static_cast<const Layout*>(user);
        const Face    face = fontId == 1 ? Face::Strong : Face::Regular;
        const Vector2 s =
            l->measure_(face, (float)fontSize, std::string_view(text, (size_t)length));
        return { s.x, s.y };
    }
    static void Error(const char* text, int32_t length, void* user)
    {
        static_cast<Layout*>(user)->Report(std::string_view(text, (size_t)length));
    }
};

namespace
{
uint8_t Kind(Size::Kind k)
{
    switch (k)
    {
        case Size::Kind::Grow: return UICLAY_GROW;
        case Size::Kind::Fixed: return UICLAY_FIXED;
        case Size::Kind::Percent: return UICLAY_PERCENT;
        case Size::Kind::Fit: break;
    }
    return UICLAY_FIT;
}

uint8_t Side(Align a)
{
    return a == Align::Center ? UICLAY_CENTER : a == Align::End ? UICLAY_END : UICLAY_START;
}

void Rgba(Color c, float out[4])
{
    out[0] = c.r, out[1] = c.g, out[2] = c.b, out[3] = c.a;
}

Color ToColor(const float c[4])
{
    return { (unsigned char)c[0], (unsigned char)c[1], (unsigned char)c[2], (unsigned char)c[3] };
}

uint16_t U16(float v)
{
    return (uint16_t)std::clamp(v, 0.0f, 65535.0f);
}

float LineHeight(float px)
{
    return std::round(px * 1.3f);
}
}  // namespace

// --- Text styles ----------------------------------------------------------------------

TextStyle TextStyle::Small()
{
    const Theme& t = CurrentTheme();
    return { Face::Regular, t.fontSize.small, t.colors.dim };
}

TextStyle TextStyle::Label()
{
    const Theme& t = CurrentTheme();
    return { Face::Regular, t.fontSize.label, t.colors.dim };
}

TextStyle TextStyle::Body()
{
    const Theme& t = CurrentTheme();
    return { Face::Regular, t.fontSize.body, t.colors.text };
}

TextStyle TextStyle::Strong()
{
    const Theme& t = CurrentTheme();
    return { Face::Strong, t.fontSize.body, t.colors.text };
}

TextStyle TextStyle::Title()
{
    const Theme& t = CurrentTheme();
    return { Face::Strong, t.fontSize.title, t.colors.text };
}

TextStyle TextStyle::Heading()
{
    const Theme& t = CurrentTheme();
    return { Face::Strong, t.fontSize.heading, t.colors.accent };
}

// --- Layout ---------------------------------------------------------------------------

Layout::Layout(int maxElements)
    : measure_([](Face face, float px, std::string_view text)
               { return MeasureString(face, px, text); })
{
    clay_ = uiclay_create(maxElements, &LayoutCallbacks::Measure, &LayoutCallbacks::Error, this);
}

Layout::~Layout()
{
    uiclay_destroy(clay_);
}

void Layout::Report(std::string_view message)
{
    // The same complaint arrives every frame; the log hears it once.
    if (lastError_ != message)
    {
        lastError_.assign(message);
        TraceLog(LOG_WARNING, "UI layout: %s", lastError_.c_str());
    }
}

float Layout::U(float units) const
{
    return std::round(units * scale_);
}

std::string_view Layout::Keep(std::string_view s)
{
    strings_.emplace_back(s);
    return strings_.back();
}

void Layout::Begin(const Frame& frame)
{
    Pointer p;
    p.owned = frame.Owner();
    p.pos = frame.Mouse();
    p.down = frame.Down(MOUSE_BUTTON_LEFT);
    p.pressed = frame.Pressed(MOUSE_BUTTON_LEFT);
    p.rightPressed = frame.Pressed(MOUSE_BUTTON_RIGHT);
    p.wheel = frame.Wheel();
    p.dt = GetFrameTime();
    if (frame.KeyboardFocus() != nullptr)
        UseFocus(frame.KeyboardFocus(), frame.Id());
    Begin(frame.Area(), Ui::Scale(), p);
}

void Layout::Begin(Rectangle area, float scale)
{
    Begin(area, scale, Pointer());
}

void Layout::Begin(Rectangle area, float scale, const Pointer& pointer)
{
    area_ = area;
    scale_ = scale > 0.0f ? scale : 1.0f;
    pointer_ = pointer;
    strings_.clear();
    commands_.clear();
    depth_ = 0;
    keysFed_ = false;
    if (!pointer.owned || !pointer.down)
        active_.clear();  // a drag ends with the button
    if (!tipSeen_)
    {
        tipKey_.clear();  // the cursor has left what it rested on
        tipTime_ = 0.0f;
    }
    tipSeen_ = false;
    open_ = clay_ != nullptr;
    if (!open_)
        return;
    // A pointer the layout does not own is somewhere it can never be.
    const float px = pointer.owned ? pointer.pos.x - area.x : -1.0e6f;
    const float py = pointer.owned ? pointer.pos.y - area.y : -1.0e6f;
    // The wheel is in notches; a notch scrolls three rows of body text.
    const float wheel =
        pointer.owned ? pointer.wheel * U(CurrentTheme().fontSize.body) * 3.0f : 0.0f;
    uiclay_begin(clay_, std::max(area.width, 1.0f), std::max(area.height, 1.0f), px, py,
                 pointer.owned && pointer.down, wheel, pointer.dt);

    // The root: the whole area, its children in a column, as a window's content is.
    Box root;
    root.Column().Grow();
    Open(root);
}

const std::vector<DrawCommand>& Layout::End()
{
    if (!open_)
        return commands_;
    while (depth_ > 1)
    {
        Report("an element was left open at End; closed");
        Close();
    }
    Close();  // the root, opened by Begin
    open_ = false;

    const UiClayCommand* cmds = nullptr;
    const int32_t        n = uiclay_end(clay_, &cmds);
    commands_.reserve((size_t)n);
    for (int32_t i = 0; i < n; i++)
    {
        const UiClayCommand& c = cmds[i];
        DrawCommand          d;
        d.box = { area_.x + c.x, area_.y + c.y, c.w, c.h };
        d.color = ToColor(c.color);
        d.radius = c.radius;
        switch (c.kind)
        {
            case UICLAY_CMD_RECT: d.kind = DrawCommand::Kind::Rect; break;
            case UICLAY_CMD_BORDER:
                d.kind = DrawCommand::Kind::Border;
                d.width = c.borderWidth;
                break;
            case UICLAY_CMD_TEXT:
                d.kind = DrawCommand::Kind::Text;
                d.text = std::string_view(c.text, (size_t)c.textLength);
                d.face = c.fontId == 1 ? Face::Strong : Face::Regular;
                d.px = c.fontSize;
                break;
            case UICLAY_CMD_CLIP_BEGIN: d.kind = DrawCommand::Kind::ClipBegin; break;
            case UICLAY_CMD_CLIP_END: d.kind = DrawCommand::Kind::ClipEnd; break;
            default: continue;
        }
        commands_.push_back(d);
    }
    return commands_;
}

void Layout::Open(const Box& b)
{
    if (!open_)
        return;
    UiClayElement e{};
    if (!b.id.empty())
    {
        const std::string_view id = Keep(b.id);
        e.id = id.data();
        e.idLength = (int32_t)id.size();
        e.idIndex = b.index;
    }
    e.column = b.column ? 1 : 0;
    auto axis = [this](Size s)
    {
        UiClayAxis a{};
        a.kind = Kind(s.kind);
        a.min = s.kind == Size::Kind::Percent ? std::clamp(s.min, 0.0f, 1.0f) : U(s.min);
        a.max = s.kind == Size::Kind::Percent ? 0.0f : U(s.max);
        return a;
    };
    e.width = axis(b.width);
    e.height = axis(b.height);
    e.padLeft = U16(U(b.padLeft));
    e.padRight = U16(U(b.padRight));
    e.padTop = U16(U(b.padTop));
    e.padBottom = U16(U(b.padBottom));
    e.gap = U16(U(b.gap));
    e.alignX = Side(b.alignX);
    e.alignY = Side(b.alignY);
    Rgba(b.fill, e.fill);
    e.radius = U(b.radius);
    if (b.border > 0.0f && b.borderColor.a > 0)
    {
        Rgba(b.borderColor, e.borderColor);
        e.borderWidth = U16(std::max(1.0f, U(b.border)));
    }
    e.scrollY = b.scrollY ? 1 : 0;
    uiclay_open(&e);
    depth_++;
}

void Layout::Close()
{
    if (!open_ || depth_ == 0)
        return;
    uiclay_close();
    depth_--;
}

void Layout::Text(std::string_view text, const TextStyle& s)
{
    if (!open_ || text.empty())
        return;
    const std::string_view kept = Keep(text);
    UiClayText             t{};
    t.fontId = s.face == Face::Strong ? 1 : 0;
    t.fontSize = (uint16_t)FontPx(s.size * scale_);
    t.lineHeight = U16(LineHeight(t.fontSize));
    Rgba(s.color, t.color);
    t.wrap = s.wrap ? 1 : 0;
    t.align = Side(s.align);
    uiclay_text(kept.data(), (int32_t)kept.size(), &t);
}

void Layout::Spacer(float units)
{
    Box b;
    if (units > 0.0f)
        b.Width(Size::Fixed(units)).Height(Size::Fixed(units));
    else
        b.Grow();
    Open(b);
    Close();
}

void Layout::Bar(float fraction, Color color)
{
    const Theme& t = CurrentTheme();
    const float  f = std::clamp(fraction, 0.0f, 1.0f);
    Row(Box()
            .GrowX()
            .Height(Size::Fixed(t.metrics.barHeight))
            .Fill(t.colors.track)
            .Radius(t.metrics.radius),
        [&]
        {
            if (f > 0.0f)
            {
                Open(Box()
                         .Width(Size::Percent(f))
                         .Height(Size::Grow())
                         .Fill(color)
                         .Radius(t.metrics.radius));
                Close();
            }
        });
}

void Layout::Field(std::string_view label, std::string_view value, Color valueColor)
{
    Row(Box().GrowX().Gap(CurrentTheme().metrics.gap).Align(Align::Start, Align::Center),
        [&]
        {
            Text(label, TextStyle::Label());
            Spacer();
            Text(value, TextStyle::Body().Tint(valueColor));
        });
}

void Layout::Divider()
{
    const Theme& t = CurrentTheme();
    Open(Box().GrowX().Height(Size::Fixed(t.metrics.border)).Fill(t.colors.border));
    Close();
}

bool Layout::Button(std::string_view id, std::string_view label, bool highlighted)
{
    const Theme& t = CurrentTheme();
    const bool   hover = Hovered(id);
    Row(Box()
            .Id(id)
            .GrowX()
            .Height(Size::Fixed(t.metrics.buttonHeight))
            .Pad(t.metrics.padding, 0.0f)
            .Align(Align::Center, Align::Center)
            .Fill(hover ? t.colors.hover : t.colors.title)
            .Border(highlighted || hover ? t.colors.accent : t.colors.border, t.metrics.border)
            .Radius(t.metrics.radius),
        [&] { Text(label, TextStyle::Body().Tint(hover ? t.colors.accent : t.colors.text)); });
    return hover && pointer_.pressed;
}

void Layout::Chip(std::string_view label, bool on, Color onColor)
{
    const Theme& t = CurrentTheme();
    Row(Box()
            .GrowX()
            .Pad(t.metrics.gap * 0.75f, t.metrics.rowGap)
            .Align(Align::Center, Align::Center)
            .Fill(on ? Fade(onColor, 0.22f) : Color{ 0, 0, 0, 0 })
            .Border(on ? onColor : t.colors.track, t.metrics.border)
            .Radius(t.metrics.radius),
        [&] { Text(label, TextStyle::Small().Tint(on ? onColor : t.colors.dim)); });
}

bool Layout::Hovered(std::string_view id, uint32_t index) const
{
    if (clay_ == nullptr || !pointer_.owned || id.empty())
        return false;
    return uiclay_pointer_over(clay_, id.data(), (int32_t)id.size(), index) != 0;
}

bool Layout::Clicked(std::string_view id, uint32_t index) const
{
    return pointer_.pressed && Hovered(id, index);
}

Rectangle Layout::BoxOf(std::string_view id, uint32_t index) const
{
    float b[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (clay_ == nullptr || id.empty() ||
        !uiclay_box(clay_, id.data(), (int32_t)id.size(), index, b))
        return { 0.0f, 0.0f, 0.0f, 0.0f };
    return { area_.x + b[0], area_.y + b[1], b[2], b[3] };
}

// --- Drawing --------------------------------------------------------------------------

void Layout::Draw() const
{
    // Clips nest -- a table's cells inside its scrolling body -- and raylib has one scissor
    // rectangle, so the stack is kept here: each clip is the intersection with the one it is
    // inside, and ending one puts its parent's back.
    std::vector<Rectangle> clips;
    auto                   scissor = [&clips]()
    {
        if (clips.empty())
        {
            EndScissorMode();
            return;
        }
        const Rectangle& r = clips.back();
        BeginScissorMode((int)r.x, (int)r.y, (int)std::max(0.0f, r.width),
                         (int)std::max(0.0f, r.height));
    };
    for (const DrawCommand& c : commands_)
    {
        // Edges rounded rather than sizes, so a row of boxes stays a row: rounding each width
        // on its own lets the errors add up and the last box poke past its parent.
        const float     x0 = std::round(c.box.x), y0 = std::round(c.box.y);
        const Rectangle r{ x0, y0, std::round(c.box.x + c.box.width) - x0,
                           std::round(c.box.y + c.box.height) - y0 };
        const float     shorter = std::max(1.0f, std::min(r.width, r.height));
        const float     roundness = std::min(1.0f, 2.0f * c.radius / shorter);
        switch (c.kind)
        {
            case DrawCommand::Kind::Rect:
                if (c.color.a == 0 || r.width <= 0.0f || r.height <= 0.0f)
                    break;
                if (c.radius > 0.0f)
                    DrawRectangleRounded(r, roundness, 6, c.color);
                else
                    DrawRectangleRec(r, c.color);
                break;
            case DrawCommand::Kind::Border:
                if (c.color.a == 0)
                    break;
                if (c.radius > 0.0f)
                {
                    // raylib draws a rounded outline outside its rectangle and a square one
                    // inside; inset, both stay within the box Clay gave them.
                    const Rectangle in{ r.x + c.width, r.y + c.width, r.width - 2.0f * c.width,
                                        r.height - 2.0f * c.width };
                    const float     s = std::max(1.0f, std::min(in.width, in.height));
                    DrawRectangleRoundedLinesEx(in, std::min(1.0f, 2.0f * c.radius / s), 6, c.width,
                                                c.color);
                }
                else
                    DrawRectangleLinesEx(r, c.width, c.color);
                break;
            case DrawCommand::Kind::Text:
            {
                // The box is a line; the glyphs sit in its middle.
                const float y = r.y + (r.height - c.px) * 0.5f;
                DrawString(c.face, c.text, { r.x, y }, c.px, c.color);
                break;
            }
            case DrawCommand::Kind::ClipBegin:
            {
                Rectangle clip = r;
                if (!clips.empty())
                {
                    const Rectangle& o = clips.back();
                    const float      x0 = std::max(clip.x, o.x), y0 = std::max(clip.y, o.y);
                    const float      x1 = std::min(clip.x + clip.width, o.x + o.width);
                    const float      y1 = std::min(clip.y + clip.height, o.y + o.height);
                    clip = { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
                }
                clips.push_back(clip);
                scissor();
                break;
            }
            case DrawCommand::Kind::ClipEnd:
                if (!clips.empty())
                    clips.pop_back();
                scissor();
                break;
        }
    }
}

// --- The widget library (#297, slice 3) ------------------------------------------------
// Each widget is built from boxes and text like everything else in a layout, asks about the
// mouse through the same one-frame-behind ids, and keeps no state a caller has to know
// about: what it remembers between frames (a drag, a number being typed, how long the
// cursor has rested) is the layout's.

namespace
{
std::string g_tip;
Vector2     g_tipAt{ 0.0f, 0.0f };

// Long enough not to flicker up while the cursor crosses a list on its way somewhere else.
constexpr float TOOLTIP_DELAY = 0.45f;

std::string Key(std::string_view id, uint32_t index)
{
    std::string k(id);
    k += '#';
    k += std::to_string(index);
    return k;
}

bool NumberChar(char c)
{
    return (c >= '0' && c <= '9') || c == '.' || c == '-';
}

std::string Format(const char* format, float value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, value);
    return buf;
}

constexpr Color CLEAR{ 0, 0, 0, 0 };
}  // namespace

void PostTooltip(std::string_view text, Vector2 at)
{
    g_tip.assign(text);
    g_tipAt = at;
}

const std::string& PendingTooltip()
{
    return g_tip;
}

void ClearTooltip()
{
    g_tip.clear();
}

void DrawTooltip()
{
    if (g_tip.empty())
        return;
    const Theme&  t = CurrentTheme();
    const float   px = (float)FontPx(t.fontSize.label * Scale());
    const Vector2 size = MeasureString(Face::Regular, px, g_tip);
    const float   pad = Px(t.metrics.gap * 0.75f);
    Rectangle     box{ g_tipAt.x + Px(12.0f), g_tipAt.y + Px(18.0f), size.x + 2.0f * pad,
                       size.y + 2.0f * pad };
    // On the screen, whichever corner of the cursor that takes.
    if (box.x + box.width > (float)GetScreenWidth())
        box.x = std::max(0.0f, g_tipAt.x - box.width - Px(4.0f));
    if (box.y + box.height > (float)GetScreenHeight())
        box.y = std::max(0.0f, g_tipAt.y - box.height - Px(4.0f));
    box.x = std::round(box.x), box.y = std::round(box.y);
    DrawRectangleRec(box, t.colors.title);
    DrawRectangleLinesEx(box, std::max(1.0f, Px(t.metrics.border)), t.colors.border);
    DrawString(Face::Regular, g_tip, { box.x + pad, box.y + pad }, px, t.colors.text);
    g_tip.clear();
}

std::string Ellipsize(std::string_view text, float maxPx,
                      const std::function<float(std::string_view)>& width)
{
    if (maxPx <= 0.0f || width(text) <= maxPx)
        return std::string(text);
    std::string s(text);
    while (!s.empty() && width(s + "..") > maxPx)
        s.pop_back();
    while (!s.empty() && s.back() == ' ')
        s.pop_back();  // "Alpha .." reads worse than "Alpha.."
    return s + "..";
}

float Layout::TextWidthPx(const TextStyle& style, std::string_view text) const
{
    return measure_(style.face, (float)FontPx(style.size * scale_), text).x;
}

void Layout::UseFocus(Focus* focus, std::string_view owner)
{
    focus_ = focus;
    owner_.assign(owner);
}

void Layout::FeedKeys(const KeyInput& keys)
{
    keys_ = keys;
    keysFed_ = true;
}

KeyInput Layout::Keys()
{
    if (!keysFed_)
        return ReadKeys();
    keysFed_ = false;  // typing reaches one field, once
    return keys_;
}

void Layout::TakeFocus(std::string_view id)
{
    if (focus_ != nullptr)
        focus_->Take(owner_, id);
}

bool Layout::HasFocus(std::string_view id) const
{
    return focus_ != nullptr && focus_->Holds(owner_, id);
}

// --- Scrolling --------------------------------------------------------------------------

float Layout::ScrollGutter() const
{
    const Theme& t = CurrentTheme();
    return t.metrics.scrollbar + t.metrics.rowGap;
}

void Layout::ScrollBegin(const Box& box)
{
    const Theme&      t = CurrentTheme();
    const std::string bar = box.id + "~bar";
    float             sc[3] = { 0.0f, 0.0f, 0.0f };
    const bool have = open_ && !box.id.empty() &&
                      uiclay_scroll(clay_, box.id.data(), (int32_t)box.id.size(), box.index, sc);
    if (have && sc[2] > sc[1] + 0.5f)
    {
        // Dragging the indicator: the thumb follows the cursor, centred on it. Set before
        // the container opens, so this frame already shows where it went.
        const std::string key = Key(bar, box.index);
        if (pointer_.pressed && Hovered(bar, box.index))
            active_ = key;
        if (active_ == key && pointer_.down)
        {
            const Rectangle b = BoxOf(bar, box.index);
            const float     thumb = b.height * sc[1] / sc[2];
            const float     room = std::max(1.0f, b.height - thumb);
            const float f = std::clamp((pointer_.pos.y - b.y - thumb * 0.5f) / room, 0.0f, 1.0f);
            uiclay_set_scroll(clay_, box.id.data(), (int32_t)box.id.size(), box.index,
                              f * (sc[2] - sc[1]));
        }
    }

    Box outer;
    outer.width = box.width;
    outer.height = box.height;
    outer.gap = t.metrics.rowGap;
    Open(outer);
    Box inner = box;
    inner.column = true;
    inner.scrollY = true;
    inner.width = Size::Grow();
    inner.height = Size::Grow();
    Open(inner);
}

void Layout::ScrollEnd(const Box& box)
{
    Close();  // the scrolling column
    const Theme&      t = CurrentTheme();
    const std::string bar = box.id + "~bar";
    float             sc[3] = { 0.0f, 0.0f, 0.0f };
    const bool        overflow =
        open_ && !box.id.empty() &&
        uiclay_scroll(clay_, box.id.data(), (int32_t)box.id.size(), box.index, sc) &&
        sc[2] > sc[1] + 0.5f;
    const float r = t.metrics.scrollbar * 0.5f;
    Column(Box()
               .Id(bar, box.index)
               .Width(Size::Fixed(t.metrics.scrollbar))
               .Height(Size::Grow())
               .Fill(overflow ? t.colors.track : CLEAR)
               .Radius(r),
           [&]
           {
               if (!overflow)
                   return;
               const float off = std::clamp(sc[0] / sc[2], 0.0f, 1.0f);
               const float shown = std::clamp(sc[1] / sc[2], 0.05f, 1.0f);
               const bool  hot = active_ == Key(bar, box.index) || Hovered(bar, box.index);
               if (off > 0.0f)
               {
                   Open(Box().Width(Size::Grow()).Height(Size::Percent(off)));
                   Close();
               }
               Open(Box()
                        .Width(Size::Grow())
                        .Height(Size::Percent(shown))
                        .Fill(hot ? t.colors.accent : t.colors.dim)
                        .Radius(r));
               Close();
           });
    Close();  // the row holding both
}

// --- Table ------------------------------------------------------------------------------

TableEvents Layout::Table(const TableSpec& s)
{
    TableEvents       ev;
    const Theme&      t = CurrentTheme();
    const float       pad = t.metrics.gap * 0.5f;
    const std::string head = s.id + "~h", row = s.id + "~r", body = s.id + "~b";
    const int         cols = (int)s.columns.size();

    // A heading clicked: sort by it, or turn the order round if it already sorts.
    if (s.sort != nullptr)
        for (int c = 0; c < cols; c++)
            if (s.columns[c].sortable && Clicked(head, (uint32_t)c))
            {
                if (s.sort->column != c)
                {
                    s.sort->column = c;
                    s.sort->descending = false;
                    ev.sorted = true;
                }
                else if (s.reversible)
                {
                    s.sort->descending = !s.sort->descending;
                    ev.sorted = true;
                }
            }

    Column(Box().Grow(),
           [&]
           {
               Row(Box().GrowX(),
                   [&]
                   {
                       for (int c = 0; c < cols; c++)
                       {
                           const TableColumn& col = s.columns[c];
                           const bool         on = s.sort && col.sortable && s.sort->column == c;
                           const bool         hover = col.sortable && Hovered(head, (uint32_t)c);
                           std::string        label = col.label;
                           if (on)
                               label += s.sort->descending ? " ^" : " v";
                           Row(Box()
                                   .Id(head, (uint32_t)c)
                                   .Width(col.width)
                                   .Height(Size::Fixed(t.metrics.rowHeight))
                                   .Pad(pad, pad, 0.0f, 0.0f)
                                   .Align(col.align, Align::Center),
                               [&]
                               {
                                   Text(label, TextStyle::Small().Tint(on || hover ? t.colors.accent
                                                                                   : t.colors.dim));
                               });
                       }
                       // The indicator's room, so the headings stand over their columns.
                       Open(Box().Width(Size::Fixed(ScrollGutter())).Height(Size::Fixed(1.0f)));
                       Close();
                   });
               Divider();
               if (s.rows == 0 && !s.empty.empty())
                   Row(Box().GrowX().Pad(pad, t.metrics.rowGap),
                       [&] { Text(s.empty, TextStyle::Small()); });

               // A row scrolled out of sight is still somewhere; only the body's view counts.
               const Rectangle view = BoxOf(body);
               const bool inView = pointer_.owned && CheckCollisionPointRec(pointer_.pos, view);
               Scroll(Box().Id(body).Grow(),
                      [&]
                      {
                          for (int r = 0; r < s.rows; r++)
                          {
                              const bool hover = inView && Hovered(row, (uint32_t)r);
                              if (hover)
                              {
                                  if (s.tooltip)
                                      Tooltip(row, s.tooltip(r), (uint32_t)r);
                                  ev.hovered = r;
                                  if (pointer_.pressed)
                                      ev.clicked = r;
                                  if (pointer_.rightPressed)
                                      ev.rightClicked = r;
                              }
                              Color fill = s.rowFill ? s.rowFill(r) : CLEAR;
                              if (fill.a == 0 && hover)
                                  fill = t.colors.hover;
                              Row(Box()
                                      .Id(row, (uint32_t)r)
                                      .GrowX()
                                      .Height(Size::Fixed(t.metrics.rowHeight))
                                      .Fill(fill),
                                  [&]
                                  {
                                      for (int c = 0; c < cols; c++)
                                      {
                                          const TableColumn& col = s.columns[c];
                                          const Cell         cell = s.cell ? s.cell(r, c) : Cell();
                                          TextStyle st = TextStyle::Body().Tint(cell.color);
                                          st.face = cell.face;
                                          // As wide as its heading was laid out last frame.
                                          const float room =
                                              BoxOf(head, (uint32_t)c).width - 2.0f * U(pad);
                                          const std::string text =
                                              Ellipsize(cell.text, room, [&](std::string_view x)
                                                        { return TextWidthPx(st, x); });
                                          Row(Box()
                                                  .Width(col.width)
                                                  .Height(Size::Grow())
                                                  .Pad(pad, pad, 0.0f, 0.0f)
                                                  .Align(col.align, Align::Center),
                                              [&] { Text(text, st); });
                                      }
                                  });
                          }
                      });
           });
    return ev;
}

// --- Tabs -------------------------------------------------------------------------------

bool Layout::Tabs(std::string_view id, const std::vector<std::string>& labels, int& selected)
{
    const Theme& t = CurrentTheme();
    bool         changed = false;
    Row(Box().GrowX().Gap(t.metrics.rowGap),
        [&]
        {
            for (int i = 0; i < (int)labels.size(); i++)
            {
                const bool hover = Hovered(id, (uint32_t)i);
                if (hover && pointer_.pressed && i != selected)
                {
                    selected = i;
                    changed = true;
                }
                const bool on = i == selected;
                Row(Box()
                        .Id(id, (uint32_t)i)
                        .Pad(t.metrics.gap * 0.75f, t.metrics.rowGap)
                        .Align(Align::Center, Align::Center)
                        .Fill(on      ? t.colors.selected
                              : hover ? t.colors.hover
                                      : t.colors.title)
                        .Radius(t.metrics.radius),
                    [&]
                    {
                        Text(labels[i], TextStyle::Label().Tint(on      ? t.colors.accent
                                                                : hover ? t.colors.text
                                                                        : t.colors.dim));
                    });
            }
        });
    return changed;
}

// --- Fields -----------------------------------------------------------------------------

void Layout::FieldBox(std::string_view id, std::string_view shown, bool dim, bool focused,
                      Size width)
{
    const Theme& t = CurrentTheme();
    const bool   hover = Hovered(id);
    const float  pad = t.metrics.gap * 0.75f;
    TextStyle    style = TextStyle::Body().Tint(dim ? t.colors.dim : t.colors.text);
    std::string  text(shown);
    if (focused)
        text += '|';  // the caret: always at the end, since there is no cursor to move
    // Longer than the box: the end shows, where the typing is. Measured against the box the
    // previous frame laid out.
    const float room = BoxOf(id).width - 2.0f * U(pad);
    while (room > 0.0f && text.size() > 1 && TextWidthPx(style, text) > room)
        text.erase(0, 1);
    Row(Box()
            .Id(id)
            .Width(width)
            .Height(Size::Fixed(t.metrics.buttonHeight))
            .Pad(pad, 0.0f)
            .Align(Align::Start, Align::Center)
            .Fill(t.colors.title)
            .Border(focused ? t.colors.accent
                    : hover ? t.colors.dim
                            : t.colors.border,
                    t.metrics.border)
            .Radius(t.metrics.radius),
        [&] { Text(text, style); });
}

EditResult Layout::TextField(std::string_view id, std::string& text, const TextFieldOptions& o)
{
    EditResult r;
    if (focus_ != nullptr && Clicked(id))
        focus_->Take(owner_, id);
    const bool focused = HasFocus(id);
    if (focused)
    {
        focus_->Seen(owner_, id);
        r = EditText(text, Keys(), o.maxLength, o.accept);
        if (r.submitted)
            focus_->Release(Focus::Blur::Submit);
    }
    if (focus_ != nullptr)
        focus_->TakeBlur(owner_, id);  // a text field edits in place: a loss changes nothing
    const bool placeholder = text.empty() && !focused;
    FieldBox(id, placeholder ? std::string_view(o.placeholder) : std::string_view(text),
             placeholder, focused && !r.submitted, o.width);
    return r;
}

bool Layout::NumberField(std::string_view id, float& value, float lo, float hi, const char* format)
{
    bool              changed = false;
    const std::string key(id);
    // What was typed becomes the value -- if it is a number -- clamped to what is allowed.
    auto commit = [&]()
    {
        auto it = edits_.find(key);
        if (it == edits_.end())
            return;
        const char* begin = it->second.c_str();
        char*       end = nullptr;
        const float v = std::strtof(begin, &end);
        if (end != begin)
        {
            const float c = std::clamp(v, lo, hi);
            changed = c != value;
            value = c;
        }
        edits_.erase(it);
    };

    const Focus::Blur lost = focus_ != nullptr ? focus_->TakeBlur(owner_, id) : Focus::Blur::None;
    if (lost == Focus::Blur::Cancel)
        edits_.erase(key);
    else if (lost != Focus::Blur::None)
        commit();

    if (focus_ != nullptr && Clicked(id))
        focus_->Take(owner_, id);
    const bool focused = HasFocus(id);
    if (focused)
    {
        focus_->Seen(owner_, id);
        // Typing starts from nothing, with the old value shown dim until the first key: a
        // field that had to be emptied before a new number could go in is two steps for one.
        std::string&     edit = edits_[key];
        const EditResult r = EditText(edit, Keys(), 16, NumberChar);
        if (r.submitted)
        {
            commit();
            focus_->Release(Focus::Blur::Submit);
            focus_->TakeBlur(owner_, id);
        }
    }

    const auto  it = edits_.find(key);
    const bool  typing = HasFocus(id) && it != edits_.end() && !it->second.empty();
    const bool  showOld = !typing;
    std::string shown = typing ? it->second : Format(format, value);
    FieldBox(id, shown, showOld && HasFocus(id), HasFocus(id), Size::Grow());
    return changed;
}

bool Layout::Slider(std::string_view id, float& value, float lo, float hi, bool logarithmic)
{
    if (!(hi > lo))
        return false;
    const Theme& t = CurrentTheme();
    const bool   log = logarithmic && lo > 0.0f;
    auto         toFrac = [&](float v)
    {
        v = std::clamp(v, lo, hi);
        return log ? std::log(v / lo) / std::log(hi / lo) : (v - lo) / (hi - lo);
    };
    auto fromFrac = [&](float f) { return log ? lo * std::pow(hi / lo, f) : lo + (hi - lo) * f; };

    bool              changed = false;
    const std::string key = Key(id, 0);
    const Rectangle   last = BoxOf(id);  // where the track was: what the cursor is measured on
    if (pointer_.pressed && Hovered(id))
        active_ = key;
    if (active_ == key && pointer_.down && last.width > 0.0f)
    {
        const float f = std::clamp((pointer_.pos.x - last.x) / last.width, 0.0f, 1.0f);
        const float v = fromFrac(f);
        if (v != value)
        {
            value = v;
            changed = true;
        }
    }

    const bool  hot = active_ == key || Hovered(id);
    const float knob = t.metrics.barHeight;
    const float line = std::max(2.0f, t.metrics.barHeight * 0.5f);
    // The filled part is a fraction of the track less the knob, so the knob ends at the end.
    const float room = last.width > 0.0f ? std::max(0.0f, 1.0f - U(knob) / last.width) : 1.0f;
    const float f = toFrac(value) * room;
    Row(Box()
            .Id(id)
            .GrowX()
            .Height(Size::Fixed(t.metrics.buttonHeight))
            .Align(Align::Start, Align::Center),
        [&]
        {
            if (f > 0.0f)
            {
                Open(Box().Width(Size::Percent(f)).Height(Size::Fixed(line)).Fill(t.colors.accent));
                Close();
            }
            Open(Box()
                     .Width(Size::Fixed(knob))
                     .Height(Size::Fixed(knob * 2.0f))
                     .Fill(hot ? t.colors.text : t.colors.accent)
                     .Radius(t.metrics.radius));
            Close();
            Open(Box().Width(Size::Grow()).Height(Size::Fixed(line)).Fill(t.colors.track));
            Close();
        });
    return changed;
}

void Layout::Tooltip(std::string_view id, std::string_view text, uint32_t index)
{
    if (text.empty() || !Hovered(id, index))
        return;
    const std::string key = Key(id, index);
    if (tipKey_ != key)
    {
        tipKey_ = key;
        tipTime_ = 0.0f;
    }
    else
        tipTime_ += pointer_.dt;
    tipSeen_ = true;
    if (tipTime_ >= TOOLTIP_DELAY)
        PostTooltip(text, pointer_.pos);
}

}  // namespace Ui
