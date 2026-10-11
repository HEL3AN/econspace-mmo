#include "ui/Window.h"
#include "ui/Layout.h"  // Ellipsize
#include "ui/Paint.h"
#include "ui/UiTheme.h"

#include <algorithm>
#include <cmath>

Window::Window(std::string title, Content content)
    : title_(std::move(title)), content_(std::move(content))
{
}

Rectangle Window::TitleBar(Rectangle b)
{
    return { b.x, b.y, b.width, Ui::Px(Ui::CurrentTheme().metrics.titleHeight) };
}

Rectangle Window::CloseButton(Rectangle b)
{
    const float s = Ui::Px(Ui::CurrentTheme().metrics.titleHeight);
    return { b.x + b.width - s, b.y, s, s };
}

Rectangle Window::CollapseButton(Rectangle b)
{
    Rectangle r = CloseButton(b);
    r.x -= r.width;
    return r;
}

Rectangle Window::PinButton(Rectangle b)
{
    Rectangle r = CollapseButton(b);
    r.x -= r.width;
    return r;
}

Rectangle Window::ContentArea(Rectangle b)
{
    const float p = Ui::Px(Ui::CurrentTheme().metrics.padding);
    const float t = Ui::Px(Ui::CurrentTheme().metrics.titleHeight);
    return { b.x + p, b.y + t + p, b.width - 2 * p, b.height - t - 2 * p };
}

Rectangle Window::ResizeGrip(Rectangle b)
{
    const float s = Ui::Px(Ui::CurrentTheme().metrics.resizeGrip);
    return { b.x + b.width - s, b.y + b.height - s, s, s };
}

Rectangle Window::TabRect(Rectangle b, int i, int n)
{
    const float room = std::max(PinButton(b).x - b.x, 0.0f);
    const float w =
        std::min(Ui::Px(Ui::CurrentTheme().metrics.tabWidth), room / (float)std::max(n, 1));
    return { b.x + w * (float)i, b.y, w, Ui::Px(Ui::CurrentTheme().metrics.titleHeight) };
}

void Window::Draw(Rectangle bounds, const Chrome& chrome, Ui::Focus* focus,
                  std::string_view id) const
{
    const Ui::Theme& t = Ui::CurrentTheme();
    const bool       owner = chrome.owner;
    const Vector2    mouse = GetMousePosition();
    // The frame's shape, from the theme's tokens (style.*): its corners, its shadow, how its
    // title bar is filled, whether it is glass.
    const Ui::Corner corner = t.style.corner;
    const float      wr = corner == Ui::Corner::Square ? 0.0f : Ui::Px(t.metrics.windowRadius);
    const float      cut = corner == Ui::Corner::Chamfer ? wr : 0.0f;
    const float      line = std::max(1.0f, std::floor(t.metrics.border * Ui::Scale()));
    const bool       glass = t.style.panel == Ui::PanelStyle::Glass;
    const Color      glow = Ui::Faded(t.colors.accent, t.style.glow);

    Ui::DrawShadow(bounds, corner, wr, t.style.shadow, Ui::Scale());
    // The body. Over the blurred world when there is one and the theme is made for it: the
    // blur is opaque, so the window behind never shows through, only the world.
    const Ui::Backdrop& backdrop = Ui::CurrentBackdrop();
    if (t.style.blurs && backdrop.texture != nullptr)
    {
        Ui::FillShapeFromScreen(bounds, corner, wr, *backdrop.texture, backdrop.screen, WHITE);
        Ui::FillShape(bounds, corner, wr, t.style.blurPanel);
    }
    else
        Ui::FillShape(bounds, corner, wr, t.colors.panel);
    if (glass)  // a sheen from the top, as light on a pane
        Ui::FillTopBand({ bounds.x, bounds.y, bounds.width, bounds.height * 0.4f }, cut,
                        Color{ 255, 255, 255, 16 }, Color{ 255, 255, 255, 3 }, false);

    const Rectangle bar = TitleBar(bounds);
    switch (t.style.titleBar)
    {
        case Ui::TitleBar::Solid:
            if (corner == Ui::Corner::Rounded && wr > 0.0f)
            {
                // Rounded on top only: a rounded bar, its lower half squared off.
                Ui::FillShape(bar, corner, wr, t.colors.title);
                DrawRectangleRec({ bar.x, bar.y + bar.height * 0.5f, bar.width, bar.height * 0.5f },
                                 t.colors.title);
            }
            else
                Ui::FillTopBand(bar, cut, t.colors.title, t.colors.title, true);
            break;
        case Ui::TitleBar::Fade:
            Ui::FillTopBand(bar, cut, t.colors.title, Ui::Faded(t.colors.title, 0.0f), true);
            break;
        case Ui::TitleBar::Clear:
            if (!chrome.collapsed)
                DrawRectangleRec({ bar.x, bar.y + bar.height - line, bar.width, line },
                                 t.colors.border);
            break;
    }
    if (t.metrics.titleMark > 0.0f)  // the theme marks a title bar with a stripe down its left
        DrawRectangleRec(
            { bar.x, bar.y + cut, std::max(1.0f, Ui::Px(t.metrics.titleMark)), bar.height - cut },
            t.colors.accent);

    const float px = Ui::Px(t.fontSize.title);
    const float pad = Ui::Px(t.metrics.padding);
    auto width = [px](std::string_view s) { return Ui::MeasureString(Ui::Face::Heading, px, s).x; };
    if (chrome.tabs.size() >= 2)
    {
        // A stack: one tab per window, the one in front joined to the body below it.
        const int n = (int)chrome.tabs.size();
        for (int i = 0; i < n; i++)
        {
            const Rectangle tr = TabRect(bounds, i, n);
            const bool      front = i == chrome.activeTab;
            const bool      over = owner && CheckCollisionPointRec(mouse, tr);
            Color           ink = front ? t.colors.titleText : t.colors.dim;
            if (front)
                switch (t.style.tabs)
                {
                    case Ui::TabStyle::Filled:
                        DrawRectangleRec(tr, t.colors.accent);
                        ink = t.colors.onAccent;
                        break;
                    case Ui::TabStyle::Segmented:
                    {
                        // A raised plate in the bar, as the tabs inside a window are.
                        const float     in = Ui::Px(3.0f);
                        const Rectangle plate{ tr.x + in, tr.y + in, tr.width - 2.0f * in,
                                               tr.height - 2.0f * in };
                        Ui::FillShape({ plate.x, plate.y + 1.0f, plate.width, plate.height },
                                      Ui::Corner::Rounded, Ui::Px(t.metrics.radius),
                                      Color{ 0, 0, 0, 100 });
                        Ui::FillShape(plate, Ui::Corner::Rounded, Ui::Px(t.metrics.radius),
                                      t.colors.title);
                        break;
                    }
                    default: DrawRectangleRec(tr, t.colors.panel); break;
                }
            else if (over)
                DrawRectangleRec(tr, t.colors.hover);
            if (i > 0)
                DrawLineEx({ tr.x, tr.y + 3.0f }, { tr.x, tr.y + tr.height - 3.0f }, 1.0f,
                           t.colors.border);
            const float       inner = pad * 0.5f;
            const std::string label = Ui::Ellipsize(chrome.tabs[i], tr.width - 2.0f * inner, width);
            Ui::DrawGlowString(
                Ui::Face::Heading, label,
                { tr.x + inner, Ui::CentredTop(Ui::Face::Heading, px, tr.y, tr.height) }, px, ink,
                front ? glow : Color{ 0, 0, 0, 0 });
            if (front &&
                (t.style.tabs == Ui::TabStyle::Underline || t.style.tabs == Ui::TabStyle::Boxed))
                DrawLineEx({ tr.x, tr.y + 1.0f }, { tr.x + tr.width, tr.y + 1.0f }, 2.0f,
                           t.colors.accent);
        }
    }
    else
    {
        const std::string& title = chrome.tabs.size() == 1 ? chrome.tabs[0] : title_;
        Ui::DrawGlowString(
            Ui::Face::Heading, title,
            { bounds.x + pad, Ui::CentredTop(Ui::Face::Heading, px, bar.y, bar.height) }, px,
            t.colors.titleText, glow);
    }
    Ui::OutlineShape(bounds, corner, wr, line, t.colors.border);
    if (glass && cut > 0.0f)
    {
        // The cut corners drawn in the accent: what makes the plate read as one.
        DrawLineEx({ bounds.x, bounds.y + cut }, { bounds.x + cut, bounds.y }, line,
                   t.colors.accent);
        DrawLineEx({ bounds.x + bounds.width - cut, bounds.y + bounds.height },
                   { bounds.x + bounds.width, bounds.y + bounds.height - cut }, line,
                   t.colors.accent);
    }
    if (chrome.dropTarget)  // let go here and it becomes a tab of this
        DrawRectangleLinesEx(bar, Ui::Px(2.0f), t.colors.accent);

    // The buttons are drawn, not written: a shape is the same at every size and in every font.
    const float thick = Ui::Px(1.5f);
    auto        button = [&](Rectangle r, bool lit)
    {
        const bool hover = owner && CheckCollisionPointRec(mouse, r);
        return hover || lit ? t.colors.accent : t.colors.dim;
    };
    {
        const Rectangle cb = CloseButton(bounds);
        const float     arm = cb.height * 0.16f;
        const Vector2   c{ cb.x + cb.width * 0.5f, cb.y + cb.height * 0.5f };
        const Color     xc = button(cb, false);
        DrawLineEx({ c.x - arm, c.y - arm }, { c.x + arm, c.y + arm }, thick, xc);
        DrawLineEx({ c.x - arm, c.y + arm }, { c.x + arm, c.y - arm }, thick, xc);
    }
    {
        // Open: a bar, to fold it to its title. Collapsed: a box, to open it out again.
        const Rectangle cb = CollapseButton(bounds);
        const float     arm = cb.height * 0.17f;
        const Vector2   c{ cb.x + cb.width * 0.5f, cb.y + cb.height * 0.5f };
        const Color     col = button(cb, false);
        if (chrome.collapsed)
            DrawRectangleLinesEx({ c.x - arm, c.y - arm, 2.0f * arm, 2.0f * arm }, thick, col);
        else
            DrawLineEx({ c.x - arm, c.y + arm * 0.6f }, { c.x + arm, c.y + arm * 0.6f }, thick,
                       col);
    }
    {
        // A pushpin seen from the side: a head, a collar and a needle. Filled while pinned.
        const Rectangle pb = PinButton(bounds);
        const float     s = pb.height * 0.12f;
        const Vector2   c{ pb.x + pb.width * 0.5f, pb.y + pb.height * 0.5f };
        const Color     col = button(pb, chrome.pinned);
        const Rectangle head{ c.x - s * 0.9f, c.y - s * 2.2f, s * 1.8f, s * 1.8f };
        if (chrome.pinned)
            DrawRectangleRec(head, col);
        else
            DrawRectangleLinesEx(head, thick * 0.7f, col);
        DrawLineEx({ c.x - s * 1.7f, c.y - s * 0.3f }, { c.x + s * 1.7f, c.y - s * 0.3f }, thick,
                   col);
        DrawLineEx({ c.x, c.y - s * 0.3f }, { c.x, c.y + s * 2.4f }, thick * 0.8f, col);
    }

    if (chrome.collapsed)
        return;

    if (content_)
    {
        Ui::MouseScope scope(owner);  // the controls inside read the same answer
        Ui::Frame      frame(ContentArea(bounds), owner, focus, id);
        content_(frame);
    }

    if (chrome.resizable && !chrome.pinned)
    {
        // Three short diagonals in the corner, the grip every desktop draws.
        const Rectangle g = ResizeGrip(bounds);
        const bool      over = owner && CheckCollisionPointRec(mouse, g);
        const Color     gc = over ? t.colors.accent : t.colors.border;
        const float     x1 = g.x + g.width - 3.0f, y1 = g.y + g.height - 3.0f;
        for (int i = 1; i <= 3; i++)
        {
            const float d = g.width * 0.25f * (float)i;
            DrawLineEx({ x1 - d, y1 }, { x1, y1 - d }, 1.0f, gc);
        }
    }
}
