#include "ui/Window.h"
#include "ui/UiTheme.h"
#include "raymath.h"  // Clamp

Window::Window(std::string title, Rectangle bounds, bool open)
    : title_(std::move(title)), bounds_(bounds), open_(open)
{
}

Rectangle Window::TitleBarRect() const
{
    return { bounds_.x, bounds_.y, bounds_.width, (float)Ui::TITLE_HEIGHT };
}

Rectangle Window::CloseButtonRect() const
{
    float s = (float)Ui::TITLE_HEIGHT;
    return { bounds_.x + bounds_.width - s, bounds_.y, s, s };
}

Rectangle Window::ContentArea() const
{
    float p = (float)Ui::PADDING;
    float t = (float)Ui::TITLE_HEIGHT;
    return { bounds_.x + p, bounds_.y + t + p, bounds_.width - 2 * p,
             bounds_.height - t - 2 * p };
}

bool Window::ContainsMouse() const
{
    return open_ && CheckCollisionPointRec(GetMousePosition(), bounds_);
}

bool Window::TitleBarHit(Vector2 m) const
{
    return open_ && CheckCollisionPointRec(m, TitleBarRect());
}

bool Window::CloseButtonHit(Vector2 m) const
{
    return open_ && CheckCollisionPointRec(m, CloseButtonRect());
}

void Window::StartDrag(Vector2 m)
{
    dragging_ = true;
    dragOffset_ = { m.x - bounds_.x, m.y - bounds_.y };
}

void Window::UpdateDrag()
{
    if (!dragging_)
        return;

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
    {
        dragging_ = false;
        return;
    }

    Vector2 m = GetMousePosition();
    bounds_.x = m.x - dragOffset_.x;
    bounds_.y = m.y - dragOffset_.y;

    // Keep the window from moving entirely off-screen — the title bar stays reachable.
    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();
    bounds_.x = Clamp(bounds_.x, -bounds_.width + 60.0f, sw - 60.0f);
    bounds_.y = Clamp(bounds_.y, 0.0f, sh - (float)Ui::TITLE_HEIGHT);
}

void Window::Draw()
{
    if (!open_)
        return;

    DrawRectangleRec(bounds_, Ui::PANEL_BG);
    DrawRectangleRec(TitleBarRect(), Ui::TITLE_BG);
    DrawRectangleLinesEx(bounds_, 1.0f, Ui::PANEL_BORDER);

    Ui::Text(title_.c_str(), (int)bounds_.x + Ui::PADDING, (int)bounds_.y + 6, 16, Ui::TEXT);

    Rectangle cb = CloseButtonRect();
    bool hover = CheckCollisionPointRec(GetMousePosition(), cb);
    Ui::Text("x", (int)cb.x + 9, (int)cb.y + 6, 16, hover ? Ui::ACCENT : Ui::TEXT_DIM);

    if (content_)
        content_(ContentArea());
}
