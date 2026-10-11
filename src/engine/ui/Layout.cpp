#include "ui/Layout.h"

#include "render/Perf.h"
#include "ui/ClayBridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Ui
{

namespace
{
// Clay carries a face as a number: its place in Ui::Face.
Face FaceOf(uint16_t fontId)
{
    return fontId < FACE_COUNT ? (Face)fontId : Face::Regular;
}

// Where a draw command falls against the clip it is in (Layout::Draw).
enum class Place : uint8_t
{
    Skip,      // draws nothing, or nothing the clip would let through
    Inside,    // wholly inside: needs no scissor
    Crossing,  // across the clip's edge: drawn under it
    Cut,       // across the clip's edge, and simple enough to cut to it without a scissor
};

Rectangle Intersect(const Rectangle& a, const Rectangle& b)
{
    const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.width, b.x + b.width);
    const float y1 = std::min(a.y + a.height, b.y + b.height);
    return { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
}

bool Contains(const Rectangle& a, const Rectangle& b)
{
    return b.x >= a.x && b.y >= a.y && b.x + b.width <= a.x + a.width &&
           b.y + b.height <= a.y + a.height;
}

bool Overlaps(const Rectangle& a, const Rectangle& b)
{
    return b.x < a.x + a.width && b.x + b.width > a.x && b.y < a.y + a.height &&
           b.y + b.height > a.y;
}

// FNV-1a over whatever a cached result was computed from.
struct Hasher
{
    uint64_t h = 1469598103934665603ull;
    Hasher&  Bytes(const void* data, size_t n)
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; i++)
            h = (h ^ p[i]) * 1099511628211ull;
        return *this;
    }
    Hasher& Add(int v) { return Bytes(&v, sizeof(v)); }
    Hasher& Add(float v) { return Bytes(&v, sizeof(v)); }
    // The length first, so "ab"+"c" and "a"+"bc" differ.
    Hasher& Add(std::string_view s) { return Add((int)s.size()).Bytes(s.data(), s.size()); }
};
}  // namespace

struct LayoutCallbacks
{
    static UiClaySize Measure(const char* text, int32_t length, uint16_t fontId, uint16_t fontSize,
                              void* user)
    {
        const Layout* l = static_cast<const Layout*>(user);
        const Face    face = FaceOf(fontId);
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
    return std::round(px * CurrentTheme().text.lineHeight);
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
    return { Face::Heading, t.fontSize.title, t.colors.text };
}

TextStyle TextStyle::Heading()
{
    const Theme& t = CurrentTheme();
    return { Face::Heading, t.fontSize.heading, t.colors.accent };
}

TextStyle TextStyle::Caption()
{
    const Theme& t = CurrentTheme();
    return { Face::Heading, t.fontSize.label, t.colors.dim };
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
    glows_.clear();
    depth_ = 0;
    scrollContent_.clear();
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
    // Clay keeps what each word measured. A theme switched since then has other fonts, and
    // its words would be laid out at the old ones' widths: values cut short, labels run
    // into each other.
    if (fontGeneration_ != FontGeneration())
    {
        uiclay_forget_measures(clay_);
        measured_.Clear();
        fits_.clear();
        fontGeneration_ = FontGeneration();
    }
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

    Render::Perf::Scope  timed(Render::Perf::Section::Solve);
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
        d.paint = (uint8_t)(c.paint & 0xFF);
        d.mark = (Mark)(c.paint >> 24);
        const uint32_t glow = (c.paint >> 8) & 0xFFFF;
        if (glow > 0 && glow <= glows_.size())
            d.glow = glows_[glow - 1];
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
                d.face = FaceOf(c.fontId);
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
    // The paint flags in the low byte; above them, which glow colour, counted from 1; the
    // mark in the top byte.
    e.paint = b.paint | ((uint32_t)b.mark << 24);
    if ((b.paint & PAINT_GLOW) && b.glow.a > 0 && glows_.size() < 0xFFFF)
    {
        glows_.push_back(b.glow);
        e.paint |= (uint32_t)glows_.size() << 8;
    }
    uiclay_open(&e);
    depth_++;
    scrollContent_.push_back(false);

    // A box that scrolls clips sideways too, and a clipped axis lets its children be as wide
    // as they like: a field's value or a row of buttons ran on past the window's edge, and
    // took every row with it. So its children go in a column exactly as wide as the view was
    // last frame, less its padding -- what is left once a scroll indicator beside it (Scroll)
    // has had its room -- and closing the box closes that column with it.
    if (b.scrollY && !b.id.empty())
    {
        const Rectangle view = BoxOf(b.id, b.index);
        Box             content;
        content.column = b.column;
        content.gap = b.gap;
        content.alignX = b.alignX;
        content.alignY = b.alignY;
        content.width = Size::Grow();
        if (view.width > 0.0f && scale_ > 0.0f)
            content.width =
                Size::Fixed(std::max(0.0f, view.width - U(b.padLeft) - U(b.padRight)) / scale_);
        content.height = Size::Grow();
        Open(content);
        scrollContent_.back() = true;
    }
}

void Layout::Close()
{
    if (!open_ || depth_ == 0)
        return;
    const bool content = !scrollContent_.empty() && scrollContent_.back();
    if (!scrollContent_.empty())
        scrollContent_.pop_back();
    uiclay_close();
    depth_--;
    if (content)
        Close();  // the scrolling box the column was in
}

void Layout::Text(std::string_view text, const TextStyle& s)
{
    if (!open_ || text.empty())
        return;
    const std::string_view kept = Keep(text);
    UiClayText             t{};
    t.fontId = (uint16_t)s.face;
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
    // Rounded only in a rounded theme: a bar is too thin to cut.
    const float radius = t.style.corner == Corner::Rounded ? t.metrics.radius : 0.0f;
    Box         track = Box().GrowX().Height(Size::Fixed(t.metrics.barHeight)).Radius(radius);
    Box fill = Box().Width(Size::Percent(f)).Height(Size::Grow()).Fill(color).Radius(radius);
    if (t.style.bar == BarStyle::Striped)
    {
        // Segments in a frame: a gauge on a panel rather than a strip of colour.
        const float frame = t.metrics.border;
        track.Height(Size::Fixed(t.metrics.barHeight + 2.0f * frame + 2.0f))
            .Border(t.colors.border, 1.0f)
            .Pad(2.0f);
        fill.Paint(PAINT_STRIPED);
    }
    else
        track.Fill(t.colors.track);
    if (t.style.glow > 0.0f)
        fill.Paint(PAINT_GLOW);
    Row(track,
        [&]
        {
            if (f > 0.0f)
            {
                Open(fill);
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
    Box          line = Box().GrowX().Height(Size::Fixed(t.metrics.border)).Fill(t.colors.border);
    if (t.style.divider == LineStyle::Dashed)
        line.Paint(PAINT_DASHED);
    Open(line);
    Close();
}

bool Layout::Button(std::string_view id, std::string_view label, bool highlighted)
{
    const Theme& t = CurrentTheme();
    const bool   hover = Hovered(id);
    // A label too long for the room wraps and the button grows taller, rather than holding
    // the row wider than its window and being cut off at the edge.
    Row(Box()
            .Id(id)
            .GrowX()
            .Height(Size::Fit(t.metrics.buttonHeight))
            .Pad(t.metrics.padding, t.metrics.rowGap * 0.5f)
            .Align(Align::Center, Align::Center)
            .Fill(hover ? t.colors.hover : t.colors.title)
            .Border(highlighted || hover ? t.colors.accent : t.colors.border, t.metrics.border)
            .ThemeCorners()
            // The lit one glows in a glowing theme: what is on stands out of the row.
            .Paint(highlighted && t.style.glow > 0.0f ? PAINT_GLOW : 0)
            .Glow(t.colors.accent),
        [&]
        {
            Text(label, TextStyle::Caption().Tint(hover ? t.colors.accent : t.colors.text).Wrap());
        });
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
            .ThemeCorners(),
        [&] { Text(label, TextStyle::Caption().Tint(on ? onColor : t.colors.dim)); });
}

void Layout::FormRowBegin(std::string_view label)
{
    const Theme& t = CurrentTheme();
    Open(Box().GrowX().Gap(t.metrics.gap).Align(Align::Start, Align::Center));
    // A label longer than its column wraps under itself rather than running into the controls.
    Row(Box().Width(Size::Fixed(t.metrics.formLabel)),
        [&] { Text(label, TextStyle::Label().Wrap()); });
    Open(Box().GrowX().Gap(t.metrics.rowGap).Align(Align::Start, Align::Center));
}

void Layout::FormRowEnd()
{
    Close();  // the controls
    Close();  // the line
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
    Render::Perf::Scope timed(Render::Perf::Section::Paint);
    // Clips nest -- a table's cells inside its scrolling body -- and raylib has one scissor
    // rectangle, so the stack is kept here: each clip is the intersection with the one it is
    // inside.
    //
    // A scissor flushes the batch, and on an Intel UHD 620 a flush costs a tenth to a quarter
    // of a millisecond of CPU (#363) -- ten of them were half the HUD. So a clip is set only
    // for what actually crosses its edge: what is wholly inside is drawn as it is, what is
    // wholly outside is not drawn at all, and the pixels are the ones a scissor would leave.
    // Most windows' content fits, and only a scrolled table's edge rows need one.
    const size_t           n = commands_.size();
    std::vector<Rectangle> box(n), ink(n), clipOf(n);
    std::vector<Place>     place(n, Place::Skip);
    std::vector<Rectangle> clips;
    for (size_t i = 0; i < n; i++)
    {
        const DrawCommand& c = commands_[i];
        // Edges rounded rather than sizes, so a row of boxes stays a row: rounding each width
        // on its own lets the errors add up and the last box poke past its parent.
        const float     x0 = std::round(c.box.x), y0 = std::round(c.box.y);
        const Rectangle r{ x0, y0, std::round(c.box.x + c.box.width) - x0,
                           std::round(c.box.y + c.box.height) - y0 };
        box[i] = r;
        // What a command can colour: never outside this, though not every pixel inside it.
        Rectangle at{ 0.0f, 0.0f, 0.0f, 0.0f };
        switch (c.kind)
        {
            case DrawCommand::Kind::Rect:
                if (c.color.a == 0 || r.width <= 0.0f || r.height <= 0.0f)
                    break;
                at = r;
                if ((c.paint & PAINT_GLOW) && CurrentTheme().style.glow > 0.0f)
                    at = { r.x - U(6.0f), r.y - U(6.0f), r.width + 2.0f * U(6.0f),
                           r.height + 2.0f * U(6.0f) };
                if (c.paint & PAINT_RAISED)
                    at.height += 1.0f;
                break;
            case DrawCommand::Kind::Border:
                if (c.color.a != 0 && r.width > 0.0f && r.height > 0.0f)
                    at = r;
                break;
            case DrawCommand::Kind::Text:
                at =
                    TextInk(c.face, c.text, { r.x, CentredTop(c.face, c.px, r.y, r.height) }, c.px);
                break;
            case DrawCommand::Kind::ClipBegin:
                clips.push_back(clips.empty() ? r : Intersect(r, clips.back()));
                continue;
            case DrawCommand::Kind::ClipEnd:
                if (!clips.empty())
                    clips.pop_back();
                continue;
        }
        ink[i] = at;
        if (at.width <= 0.0f || at.height <= 0.0f)
            continue;  // nothing to draw
        if (clips.empty())
            place[i] = Place::Inside;
        else if (!Overlaps(clips.back(), at))
            continue;  // all of it would be cut
        else if (Contains(clips.back(), at))
            place[i] = Place::Inside;
        else
        {
            // A plain rectangle and a run of letters are cut here instead, to the same pixels:
            // a row half scrolled out of a table is all this ever is.
            const bool plain = c.kind == DrawCommand::Kind::Rect && c.paint == 0 &&
                               c.radius <= 0.0f && c.mark == Mark::None;
            const bool letters = c.kind == DrawCommand::Kind::Text && FaceReady(c.face);
            place[i] = plain || letters ? Place::Cut : Place::Crossing;
            clipOf[i] = clips.back();
            if (place[i] == Place::Cut)
                ink[i] = Intersect(ink[i], clipOf[i]);
        }
    }

    bool      on = false;  // the scissor raylib has
    Rectangle cur{ 0.0f, 0.0f, 0.0f, 0.0f };
    auto      setScissor = [&](const Rectangle* want)
    {
        if (want == nullptr)
        {
            if (on)
                EndScissorMode();
            on = false;
            return;
        }
        if (on && want->x == cur.x && want->y == cur.y && want->width == cur.width &&
            want->height == cur.height)
            return;
        Render::Perf::Count(Render::Perf::Counter::Clips);
        BeginScissorMode((int)want->x, (int)want->y, (int)std::max(0.0f, want->width),
                         (int)std::max(0.0f, want->height));
        on = true;
        cur = *want;
    };
    for (size_t i = 0; i < n; i++)
    {
        const DrawCommand& c = commands_[i];
        const Rectangle&   r = box[i];
        switch (place[i])
        {
            case Place::Skip: continue;
            case Place::Inside:
            case Place::Cut:
                if (on && !Contains(cur, ink[i]))
                    setScissor(nullptr);
                break;
            case Place::Crossing: setScissor(&clipOf[i]); break;
        }
        if (place[i] == Place::Cut)
        {
            if (c.kind == DrawCommand::Kind::Text)
                DrawString(c.face, c.text, { r.x, CentredTop(c.face, c.px, r.y, r.height) }, c.px,
                           c.color, &clipOf[i]);
            else
                DrawRectangleRec(Intersect(r, clipOf[i]), c.color);
            continue;
        }
        switch (c.kind)
        {
            case DrawCommand::Kind::Rect:
            {
                if (c.color.a == 0 || r.width <= 0.0f || r.height <= 0.0f)
                    break;
                if (c.mark != Mark::None)
                {
                    DrawMark({ r.x + r.width * 0.5f, r.y + r.height * 0.5f },
                             std::min(r.width, r.height) * 0.5f, c.mark, c.color);
                    break;
                }
                const Corner corner = (c.paint & PAINT_CHAMFER) ? Corner::Chamfer
                                      : c.radius > 0.0f         ? Corner::Rounded
                                                                : Corner::Square;
                const float  glow = CurrentTheme().style.glow;
                if ((c.paint & PAINT_GLOW) && glow > 0.0f)
                {
                    Color g = c.glow.a > 0 ? c.glow : c.color;
                    g.a = (unsigned char)(255.0f * glow);
                    DrawGlow(r, corner, c.radius, g, U(6.0f));
                }
                if (c.paint & PAINT_RAISED)
                    FillShape({ r.x, r.y + 1.0f, r.width, r.height }, corner, c.radius,
                              Color{ 0, 0, 0, 100 });
                if (c.paint & PAINT_DASHED)
                    DrawDashes(r, c.color, U(10.0f), U(4.0f));
                else if (c.paint & PAINT_STRIPED)
                    DrawStripes(r, c.color, U(6.0f), U(2.0f));
                else if (c.paint & PAINT_FADE_RIGHT)
                    FillFadeRight(r, c.color);
                else if (c.paint & PAINT_FADE_UP)
                    FillFadeUp(r, c.color);
                else
                    FillShape(r, corner, c.radius, c.color);
                break;
            }
            case DrawCommand::Kind::Border:
                if (c.color.a == 0)
                    break;
                OutlineShape(r,
                             (c.paint & PAINT_CHAMFER) ? Corner::Chamfer
                             : c.radius > 0.0f         ? Corner::Rounded
                                                       : Corner::Square,
                             c.radius, c.width, c.color);
                break;
            case DrawCommand::Kind::Text:
            {
                // The box is a line; the capitals sit in its middle, by the font's own metrics.
                DrawString(c.face, c.text, { r.x, CentredTop(c.face, c.px, r.y, r.height) }, c.px,
                           c.color);
                break;
            }
            case DrawCommand::Kind::ClipBegin:
            case DrawCommand::Kind::ClipEnd: break;  // placed above
        }
    }
    setScissor(nullptr);
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

// How a table's cell is set: in the cell's own face, except that a column of figures --
// one aligned to the end -- is in the numbers face, so its digits line up row under row.
TextStyle CellStyle(const TableColumn& col, const Cell& cell)
{
    TextStyle st = TextStyle::Body().Tint(cell.color);
    st.face = cell.face == Face::Regular && col.align == Align::End ? Face::Numbers : cell.face;
    return st;
}
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
    DrawString(Face::Regular, g_tip,
               { box.x + pad, CentredTop(Face::Regular, px, box.y, box.height) }, px,
               t.colors.text);
    g_tip.clear();
}

size_t CutPoint(std::string_view text, const std::function<bool(size_t)>& fits)
{
    // On a letter's first byte: a cut inside one would draw half a letter as another.
    auto letter = [&](size_t n)
    {
        while (n > 0 && n < text.size() && ((unsigned char)text[n] & 0xC0) == 0x80)
            n--;
        return n;
    };
    // A longer run is never narrower, so the longest that fits is found by halving: a long
    // name in a narrow column used to be measured once per letter it lost (#363).
    size_t lo = 0, hi = text.size();
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo + 1) / 2;
        if (fits(letter(mid)))
            lo = mid;
        else
            hi = mid - 1;
    }
    return letter(lo);
}

std::string Ellipsize(std::string_view text, float maxPx,
                      const std::function<float(std::string_view)>& width)
{
    if (maxPx <= 0.0f || width(text) <= maxPx)
        return std::string(text);
    std::string s;  // one buffer for every length tried
    s.reserve(text.size() + 2);
    const size_t n = CutPoint(text,
                              [&](size_t k)
                              {
                                  s.assign(text.data(), k);
                                  s += "..";
                                  return width(s) <= maxPx;
                              });
    s.assign(text.data(), n);
    while (!s.empty() && s.back() == ' ')
        s.pop_back();  // "Alpha .." reads worse than "Alpha.."
    return s + "..";
}

float Layout::TextWidthPx(const TextStyle& style, std::string_view text) const
{
    return measured_.Get(style.face, (float)FontPx(style.size * scale_), text, measure_).x;
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
    inner.gap = box.gap;  // the children's column takes it (Open)
    // The children go in a column exactly as wide as the view was last frame (Open), and
    // the indicator stands beside the view rather than in it.
    Open(inner);
}

void Layout::ScrollEnd(const Box& box)
{
    Close();  // the children's column, and the scrolling column with it
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
    // A column's chip (#361): a mark about the height of a capital -- a square, or the
    // standing's shape (#362) -- and a gap after it.
    constexpr float CHIP = 9.0f, CHIP_GAP = 5.0f;
    auto            chipRoom = [&](const TableColumn& col)
    { return col.chips ? U(CHIP) + U(CHIP_GAP) : 0.0f; };
    auto chip = [&](const TableColumn& col, Color c, Mark m)
    {
        if (!col.chips)
            return;
        Open(Box().Width(Size::Fixed(CHIP)).Height(Size::Fixed(CHIP)).Fill(c).Mark(m));
        Close();
    };

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

    // Every cell asked for once: the columns are fitted to them and the rows drawn from them.
    std::vector<Cell> cells;
    const int         rows = s.cell ? std::max(s.rows, 0) : 0;
    cells.reserve((size_t)rows * (size_t)cols);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            cells.push_back(s.cell(r, c));
    const Cell none;
    auto       cellAt = [&](int r, int c) -> const Cell&
    { return r < rows ? cells[(size_t)r * (size_t)cols + (size_t)c] : none; };

    // A Fit column measured across every row, then laid out as Fixed: fitted row by row,
    // each row's cell would be as wide as its own text and the column would not line up.
    // The widths follow from the headings, the Fit columns' cells, the scale and the fonts
    // (Begin forgets them all when the fonts change), so they are fitted again only when one
    // of those has changed (#363).
    Hasher key;
    key.Add(scale_).Add(U(pad)).Add(cols);
    for (int c = 0; c < cols; c++)
    {
        const TableColumn& col = s.columns[c];
        if (col.width.kind != Size::Kind::Fit)
            continue;
        key.Add(c).Add(col.width.min).Add(col.width.max).Add(col.sortable ? 1 : 0);
        key.Add(col.chips ? 1 : 0);  // a chip's room widens the column (#361)
        key.Add(col.label).Add(TextStyle::Caption().size).Add((int)col.align);
        for (int r = 0; r < rows; r++)
        {
            const Cell& cell = cellAt(r, c);
            key.Add((int)cell.face).Add(cell.text);
        }
        key.Add(TextStyle::Body().size);
    }
    ColumnFit& fit = fits_[s.id];
    if (fit.key != key.h || fit.widths.size() != (size_t)cols)
    {
        fit.key = key.h;
        fit.widths.clear();
        for (int c = 0; c < cols; c++)
        {
            const TableColumn& col = s.columns[c];
            if (col.width.kind != Size::Kind::Fit)
            {
                fit.widths.push_back(col.width);
                continue;
            }
            // The heading with room for the sort mark it may carry.
            float widest =
                TextWidthPx(TextStyle::Caption(), col.label + (col.sortable ? " v" : ""));
            for (int r = 0; r < rows; r++)
            {
                const Cell& cell = cellAt(r, c);
                widest = std::max(widest, TextWidthPx(CellStyle(col, cell), cell.text));
            }
            widest += chipRoom(col);
            // The padding as the layout will round it, and a pixel for rounding the width, so
            // the text gets every pixel it measured.
            float units = std::ceil((widest + 2.0f * U(pad) + 1.0f) / std::max(scale_, 0.01f));
            units = std::max(units, col.width.min);
            if (col.width.max > 0.0f)
                units = std::min(units, col.width.max);
            fit.widths.push_back(Size::Fixed(units));
        }
    }
    const std::vector<Size>& widths = fit.widths;

    Column(Box().Grow(),
           [&]
           {
               // A theme that marks the lit row keeps the mark's room on every row, and over
               // them, so the columns stay under their headings.
               const float rowMark = t.metrics.rowMark;
               auto        markRoom = [&](Color c)
               {
                   if (rowMark <= 0.0f)
                       return;
                   Open(Box().Width(Size::Fixed(rowMark)).Height(Size::Grow()).Fill(c));
                   Close();
               };
               Row(Box().GrowX(),
                   [&]
                   {
                       markRoom(CLEAR);
                       for (int c = 0; c < cols; c++)
                       {
                           const TableColumn& col = s.columns[c];
                           const bool         on = s.sort && col.sortable && s.sort->column == c;
                           const bool         hover = col.sortable && Hovered(head, (uint32_t)c);
                           std::string        label = col.label;
                           if (on)
                               label += s.sort->descending ? " ^" : " v";
                           // A heading wider than its column is cut short like a cell, never
                           // left to run under the next one.
                           const TextStyle hs = TextStyle::Caption().Tint(
                               on || hover ? t.colors.accent : t.colors.dim);
                           const float room =
                               BoxOf(head, (uint32_t)c).width - 2.0f * U(pad) - chipRoom(col);
                           if (BoxOf(head, (uint32_t)c).width > 0.0f)
                               label = Ellipsize(label, room, [&](std::string_view x)
                                                 { return TextWidthPx(hs, x); });
                           Row(Box()
                                   .Id(head, (uint32_t)c)
                                   .Width(widths[c])
                                   .Height(Size::Fixed(t.metrics.rowHeight))
                                   .Pad(pad, pad, 0.0f, 0.0f)
                                   .Gap(col.chips ? CHIP_GAP : 0.0f)
                                   .Align(col.align, Align::Center),
                               [&]
                               {
                                   chip(col, CLEAR,
                                        Mark::None);  // the heading over the text, not the chip
                                   Text(label, hs);
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
                              Color      fill = s.rowFill ? s.rowFill(r) : CLEAR;
                              const bool lit = fill.a > 0;
                              if (!lit && hover)
                                  fill = t.colors.hover;
                              Row(Box()
                                      .Id(row, (uint32_t)r)
                                      .GrowX()
                                      .Height(Size::Fixed(t.metrics.rowHeight))
                                      .Fill(fill)
                                      // Glass lights a row from its mark, fading across.
                                      .Paint(lit && t.style.panel == PanelStyle::Glass
                                                 ? PAINT_FADE_RIGHT
                                                 : 0),
                                  [&]
                                  {
                                      markRoom(lit ? t.colors.accent : CLEAR);
                                      for (int c = 0; c < cols; c++)
                                      {
                                          const TableColumn& col = s.columns[c];
                                          const Cell&        cell = cellAt(r, c);
                                          const TextStyle    st = CellStyle(col, cell);
                                          // As wide as its heading was laid out last frame.
                                          const float       room = BoxOf(head, (uint32_t)c).width -
                                                                   2.0f * U(pad) - chipRoom(col);
                                          const std::string text =
                                              Ellipsize(cell.text, room, [&](std::string_view x)
                                                        { return TextWidthPx(st, x); });
                                          Row(Box()
                                                  .Width(widths[c])
                                                  .Height(Size::Grow())
                                                  .Pad(pad, pad, 0.0f, 0.0f)
                                                  .Gap(col.chips ? CHIP_GAP : 0.0f)
                                                  .Align(col.align, Align::Center),
                                              [&]
                                              {
                                                  chip(col, cell.chip, cell.chipMark);
                                                  Text(text, st);
                                              });
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
    const Theme&   t = CurrentTheme();
    const TabStyle style = t.style.tabs;
    const bool     underline = style == TabStyle::Underline;
    const float    mark = std::max(t.metrics.tabMark, 1.0f);
    bool           changed = false;
    // The strip the tabs stand in (style.tabStyle): on a rule, side by side, in a frame of
    // the border colour, or in one sunken track only as wide as they are.
    Box strip;
    switch (style)
    {
        case TabStyle::Underline: strip.GrowX(); break;
        case TabStyle::Boxed: strip.GrowX().Gap(t.metrics.rowGap); break;
        case TabStyle::Filled:
            strip.GrowX().Gap(t.metrics.border).Pad(t.metrics.border).Fill(t.colors.border);
            break;
        case TabStyle::Segmented:
            strip.Gap(2.0f).Pad(3.0f).Fill(t.colors.track).Radius(t.WidgetRadius() + 3.0f);
            break;
    }
    Row(strip,
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
                Color      fill = on ? t.colors.selected : hover ? t.colors.hover : CLEAR;
                Color      ink = on ? t.colors.accent : hover ? t.colors.text : t.colors.dim;
                Box        tab = Box().Id(id, (uint32_t)i).Column().ThemeCorners();
                switch (style)
                {
                    case TabStyle::Underline:
                        // Glass lights the chosen tab from its rule up, rather than all over.
                        if (on && t.style.panel == PanelStyle::Glass)
                            tab.Paint(PAINT_FADE_UP);
                        break;
                    case TabStyle::Boxed:
                        if (!on && !hover)
                            fill = t.colors.title;
                        break;
                    case TabStyle::Filled:
                        fill = on ? t.colors.accent : t.colors.title;
                        ink = on ? t.colors.onAccent : hover ? t.colors.accent : t.colors.dim;
                        break;
                    case TabStyle::Segmented:
                        fill = on ? t.colors.title : hover ? t.colors.hover : CLEAR;
                        ink = on || hover ? t.colors.text : t.colors.dim;
                        if (on)
                            tab.Paint(PAINT_RAISED);
                        break;
                }
                Column(tab.Fill(fill),
                       [&]
                       {
                           Row(Box()
                                   .Pad(t.metrics.gap * 1.25f, t.metrics.rowGap)
                                   .Align(Align::Center, Align::Center),
                               [&] { Text(labels[i], TextStyle::Caption().Tint(ink)); });
                           // As tall on every tab, so their labels stay in one line.
                           if (underline)
                               Column(Box()
                                          .GrowX()
                                          .Height(Size::Fixed(mark))
                                          .Align(Align::Start, Align::End)
                                          .Fill(on ? t.colors.accent : CLEAR)
                                          .Paint(on && t.style.glow > 0.0f ? PAINT_GLOW : 0),
                                      [&]
                                      {
                                          if (on)
                                              return;
                                          Open(Box()
                                                   .GrowX()
                                                   .Height(Size::Fixed(t.metrics.border))
                                                   .Fill(t.colors.border));
                                          Close();
                                      });
                       });
            }
            if (underline)  // the rule runs on under the rest of the strip
                Column(Box().GrowX().Height(Size::Grow()).Align(Align::Start, Align::End),
                       [&]
                       {
                           Open(Box()
                                    .GrowX()
                                    .Height(Size::Fixed(t.metrics.border))
                                    .Fill(t.colors.border));
                           Close();
                       });
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
    if (room > 0.0f && text.size() > 1 && TextWidthPx(style, text) > room)
    {
        // The first letter to show: found by halving, as Ellipsize finds its cut (#363).
        const std::string_view all(text);
        auto                   letter = [&](size_t n)
        {
            while (n < all.size() - 1 && ((unsigned char)all[n] & 0xC0) == 0x80)
                n++;
            return n;
        };
        size_t lo = 1, hi = all.size() - 1;
        while (lo < hi)
        {
            const size_t mid = lo + (hi - lo) / 2;
            if (TextWidthPx(style, all.substr(letter(mid))) <= room)
                hi = mid;
            else
                lo = mid + 1;
        }
        text.erase(0, letter(lo));
    }
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
            .ThemeCorners(),
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
                     .Radius(t.style.corner == Corner::Rounded ? t.metrics.radius : 0.0f));
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
