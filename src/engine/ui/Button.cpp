#include "ui/Button.h"
#include "ui/UiTheme.h"

Button::Button(Rectangle bounds, std::string label, std::function<void()> onClick)
    : bounds_(bounds), label_(std::move(label)), onClick_(std::move(onClick))
{
}

void Button::Process()
{
    Vector2 mouse = GetMousePosition();
    bool    hovered = CheckCollisionPointRec(mouse, bounds_);

    DrawRectangleRec(bounds_, hovered ? Color{ 48, 54, 72, 255 } : Ui::TITLE_BG);
    DrawRectangleLinesEx(bounds_, 1.0f, hovered ? Ui::ACCENT : Ui::PANEL_BORDER);

    const int fontSize = 16;
    int       textW = Ui::TextWidth(label_.c_str(), fontSize);
    Ui::Text(label_.c_str(), (int)(bounds_.x + (bounds_.width - textW) / 2),
             (int)(bounds_.y + (bounds_.height - fontSize) / 2), fontSize,
             hovered ? Ui::ACCENT : Ui::TEXT);

    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        onClick_();
}
