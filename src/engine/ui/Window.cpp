#include "ui/Window.h"
#include "ui/Layout.h"  // Ellipsize
#include "ui/UiTheme.h"

#include <algorithm>

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
    DrawRectangleRec(bounds, t.colors.panel);
    const Rectangle bar = TitleBar(bounds);
    DrawRectangleRec(bar, t.colors.title);

    const float px = Ui::Px(t.fontSize.title);
    const float pad = Ui::Px(t.metrics.padding);
    auto width = [px](std::string_view s) { return Ui::MeasureString(Ui::Face::Strong, px, s).x; };
    if (chrome.tabs.size() >= 2)
    {
        // A stack: one tab per window, the one in front joined to the body below it.
        const int n = (int)chrome.tabs.size();
        for (int i = 0; i < n; i++)
        {
            const Rectangle tr = TabRect(bounds, i, n);
            const bool      front = i == chrome.activeTab;
            const bool      over = owner && CheckCollisionPointRec(mouse, tr);
            if (front)
                DrawRectangleRec(tr, t.colors.panel);
            else if (over)
                DrawRectangleRec(tr, t.colors.hover);
            if (i > 0)
                DrawLineEx({ tr.x, tr.y + 3.0f }, { tr.x, tr.y + tr.height - 3.0f }, 1.0f,
                           t.colors.border);
            const float       inner = pad * 0.5f;
            const std::string label = Ui::Ellipsize(chrome.tabs[i], tr.width - 2.0f * inner, width);
            Ui::DrawString(Ui::Face::Strong, label,
                           { tr.x + inner, tr.y + (tr.height - px) * 0.5f }, px,
                           front ? t.colors.text : t.colors.dim);
            if (front)
                DrawLineEx({ tr.x, tr.y + 1.0f }, { tr.x + tr.width, tr.y + 1.0f }, 2.0f,
                           t.colors.accent);
        }
    }
    else
    {
        const std::string& title = chrome.tabs.size() == 1 ? chrome.tabs[0] : title_;
        Ui::DrawString(Ui::Face::Strong, title,
                       { bounds.x + pad, bar.y + (bar.height - px) * 0.5f }, px, t.colors.text);
    }
    DrawRectangleLinesEx(bounds, 1.0f, t.colors.border);
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
