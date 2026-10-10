#include "ui/Window.h"
#include "ui/UiTheme.h"

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

void Window::Draw(Rectangle bounds, bool owner, bool resizable) const
{
    const Ui::Theme& t = Ui::CurrentTheme();
    DrawRectangleRec(bounds, t.colors.panel);
    const Rectangle bar = TitleBar(bounds);
    DrawRectangleRec(bar, t.colors.title);
    DrawRectangleLinesEx(bounds, 1.0f, t.colors.border);

    const float px = Ui::Px(t.fontSize.title);
    Ui::DrawString(Ui::Face::Strong, title_,
                   { bounds.x + Ui::Px(t.metrics.padding), bar.y + (bar.height - px) * 0.5f }, px,
                   t.colors.text);

    // The close button is drawn, not written: a cross is the same at every size and in
    // every font.
    const Rectangle cb = CloseButton(bounds);
    const bool      hover = owner && CheckCollisionPointRec(GetMousePosition(), cb);
    const float     arm = cb.height * 0.16f;
    const Vector2   c{ cb.x + cb.width * 0.5f, cb.y + cb.height * 0.5f };
    const Color     xc = hover ? t.colors.accent : t.colors.dim;
    const float     thick = Ui::Px(1.5f);
    DrawLineEx({ c.x - arm, c.y - arm }, { c.x + arm, c.y + arm }, thick, xc);
    DrawLineEx({ c.x - arm, c.y + arm }, { c.x + arm, c.y - arm }, thick, xc);

    if (content_)
    {
        Ui::MouseScope scope(owner);  // the controls inside read the same answer
        Ui::Frame      frame(ContentArea(bounds), owner);
        content_(frame);
    }

    if (resizable)
    {
        // Three short diagonals in the corner, the grip every desktop draws.
        const Rectangle g = ResizeGrip(bounds);
        const bool      over = owner && CheckCollisionPointRec(GetMousePosition(), g);
        const Color     gc = over ? t.colors.accent : t.colors.border;
        const float     x1 = g.x + g.width - 3.0f, y1 = g.y + g.height - 3.0f;
        for (int i = 1; i <= 3; i++)
        {
            const float d = g.width * 0.25f * (float)i;
            DrawLineEx({ x1 - d, y1 }, { x1, y1 - d }, 1.0f, gc);
        }
    }
}
