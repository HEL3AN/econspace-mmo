#include "ui/Layout.h"

#include "ui/ClayBridge.h"

#include <algorithm>
#include <cmath>

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
    p.wheel = frame.Wheel();
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
                 pointer.owned && pointer.down, wheel, GetFrameTime());

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
                BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
                break;
            case DrawCommand::Kind::ClipEnd: EndScissorMode(); break;
        }
    }
}

}  // namespace Ui
