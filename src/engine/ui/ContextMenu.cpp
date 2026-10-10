#include "ui/ContextMenu.h"
#include "ui/Fonts.h"
#include "ui/Input.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

// Sized from the theme at the current UI scale (#297): a row is a table's row with a little
// more room, and the menu is as wide as its longest label.
namespace
{
float RowHeight()
{
    return Ui::Px(Ui::CurrentTheme().metrics.rowHeight + 6.0f);
}

float Pad()
{
    return Ui::Px(Ui::CurrentTheme().metrics.padding * 0.75f);
}

float FontSize()
{
    return (float)Ui::FontPx(Ui::CurrentTheme().fontSize.body * Ui::Scale());
}
}  // namespace

void ContextMenu::Open(Vector2 pos, std::vector<Item> items)
{
    if (items.empty())
        return;

    items_ = std::move(items);
    open_ = true;

    width_ = Ui::Px(140.0f);
    for (const Item& it : items_)
        width_ = std::max(width_, Ui::MeasureString(Ui::Face::Regular, FontSize(), it.label).x +
                                      2.0f * Pad());
    width_ = std::ceil(width_);

    // Keep the menu from spilling off the edge of the screen.
    const float h = RowHeight() * (float)items_.size();
    pos.x = std::max(0.0f, std::min(pos.x, (float)GetScreenWidth() - width_));
    pos.y = std::max(0.0f, std::min(pos.y, (float)GetScreenHeight() - h));
    pos_ = pos;
}

void ContextMenu::Close()
{
    open_ = false;
    items_.clear();
}

Rectangle ContextMenu::Bounds() const
{
    return { pos_.x, pos_.y, width_, RowHeight() * (float)items_.size() };
}

bool ContextMenu::Update()
{
    if (!open_)
        return false;

    Vector2 m = GetMousePosition();
    bool    over = Ui::MouseOver(Bounds());

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (over)
        {
            // Copied out first: the action may open another menu, which replaces items_.
            const int             idx = (int)((m.y - pos_.y) / RowHeight());
            std::function<void()> action;
            if (idx >= 0 && idx < (int)items_.size())
                action = items_[idx].action;
            Close();
            if (action)
                action();
            return true;
        }
        Close();
        return false;
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

    const Ui::Theme& t = Ui::CurrentTheme();
    const Rectangle  b = Bounds();
    const float      rowH = RowHeight(), px = FontSize();
    DrawRectangleRec(b, t.colors.panel);
    DrawRectangleLinesEx(b, std::max(1.0f, Ui::Px(t.metrics.border)), t.colors.border);

    for (size_t i = 0; i < items_.size(); i++)
    {
        const Rectangle row{ b.x, b.y + rowH * (float)i, b.width, rowH };
        const bool      hover = Ui::MouseOver(row);
        if (hover)
            DrawRectangleRec(row, t.colors.selected);
        Ui::DrawString(Ui::Face::Regular, items_[i].label,
                       { std::round(b.x + Pad()), std::round(row.y + (rowH - px) * 0.5f) }, px,
                       hover ? t.colors.accent : t.colors.text);
    }
}
