#include "ui/Window.h"
#include "ui/UiTheme.h"

Window::Window(std::string title, Content content)
    : title_(std::move(title)), content_(std::move(content))
{
}

Rectangle Window::TitleBar(Rectangle b)
{
    return { b.x, b.y, b.width, (float)Ui::TITLE_HEIGHT };
}

Rectangle Window::CloseButton(Rectangle b)
{
    float s = (float)Ui::TITLE_HEIGHT;
    return { b.x + b.width - s, b.y, s, s };
}

Rectangle Window::ContentArea(Rectangle b)
{
    float p = (float)Ui::PADDING;
    float t = (float)Ui::TITLE_HEIGHT;
    return { b.x + p, b.y + t + p, b.width - 2 * p, b.height - t - 2 * p };
}

void Window::Draw(Rectangle bounds, bool owner) const
{
    DrawRectangleRec(bounds, Ui::PANEL_BG);
    DrawRectangleRec(TitleBar(bounds), Ui::TITLE_BG);
    DrawRectangleLinesEx(bounds, 1.0f, Ui::PANEL_BORDER);

    Ui::Text(title_.c_str(), (int)bounds.x + Ui::PADDING, (int)bounds.y + 6, 16, Ui::TEXT);

    Rectangle cb = CloseButton(bounds);
    bool      hover = owner && CheckCollisionPointRec(GetMousePosition(), cb);
    Ui::Text("x", (int)cb.x + 9, (int)cb.y + 6, 16, hover ? Ui::ACCENT : Ui::TEXT_DIM);

    if (content_)
    {
        Ui::MouseScope scope(owner);  // the controls inside read the same answer
        Ui::Frame      frame(ContentArea(bounds), owner);
        content_(frame);
    }
}
