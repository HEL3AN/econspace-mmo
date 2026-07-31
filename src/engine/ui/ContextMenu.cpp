#include "ui/ContextMenu.h"
#include "ui/UiTheme.h"
#include <cmath>

namespace
{
constexpr float MENU_WIDTH = 172.0f;
constexpr float ROW_HEIGHT = 26.0f;
}

void ContextMenu::Open(Vector2 pos, std::vector<Item> items)
{
    if (items.empty())
        return;

    items_ = std::move(items);
    open_ = true;

    // Keep the menu from spilling off the edge of the screen.
    float h = ROW_HEIGHT * (float)items_.size();
    pos.x = fminf(pos.x, (float)GetScreenWidth() - MENU_WIDTH);
    pos.y = fminf(pos.y, (float)GetScreenHeight() - h);
    pos_ = pos;
}

void ContextMenu::Close()
{
    open_ = false;
    items_.clear();
}

Rectangle ContextMenu::Bounds() const
{
    return { pos_.x, pos_.y, MENU_WIDTH, ROW_HEIGHT * (float)items_.size() };
}

bool ContextMenu::Update()
{
    if (!open_)
        return false;

    Vector2 m = GetMousePosition();
    bool    over = CheckCollisionPointRec(m, Bounds());

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (over)
        {
            int idx = (int)((m.y - pos_.y) / ROW_HEIGHT);
            if (idx >= 0 && idx < (int)items_.size() && items_[idx].action)
                items_[idx].action();
        }
        Close();
        return over;
    }

    // Right-click outside the menu — just close it (the caller opens a new one).
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !over)
        Close();

    return over;
}

void ContextMenu::Draw() const
{
    if (!open_)
        return;

    Rectangle b = Bounds();
    DrawRectangleRec(b, Ui::PANEL_BG);
    DrawRectangleLinesEx(b, 1.0f, Ui::PANEL_BORDER);

    Vector2 m = GetMousePosition();
    for (size_t i = 0; i < items_.size(); i++)
    {
        Rectangle row{ b.x, b.y + ROW_HEIGHT * (float)i, b.width, ROW_HEIGHT };
        bool      hover = CheckCollisionPointRec(m, row);
        if (hover)
            DrawRectangleRec(row, Fade(Ui::ACCENT, 0.22f));
        Ui::Text(items_[i].label.c_str(), (int)b.x + 10, (int)row.y + 6, 16,
                 hover ? Ui::ACCENT : Ui::TEXT);
    }
}
