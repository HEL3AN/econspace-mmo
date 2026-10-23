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

// What a row says: the label, and for a disabled one why it is.
static std::string ItemText(const ContextMenu::Item& it)
{
    return it.disabled.empty() ? it.label : it.label + "  --  " + it.disabled;
}

void ContextMenu::Open(Vector2 pos, std::vector<Item> items, std::string header)
{
    if (items.empty())
        return;

    items_ = std::move(items);
    header_ = std::move(header);
    open_ = true;

    width_ = Ui::Px(140.0f);
    for (const Item& it : items_)
        width_ = std::max(width_, Ui::MeasureString(Ui::Face::Regular, FontSize(), ItemText(it)).x +
                                      2.0f * Pad());
    if (!header_.empty())
        width_ = std::max(width_, Ui::MeasureString(Ui::Face::Strong, FontSize(), header_).x +
                                      2.0f * Pad());
    width_ = std::ceil(width_);

    // Keep the menu from spilling off the edge of the screen.
    const float h = HeaderHeight() + RowHeight() * (float)items_.size();
    pos.x = std::max(0.0f, std::min(pos.x, (float)GetScreenWidth() - width_));
    pos.y = std::max(0.0f, std::min(pos.y, (float)GetScreenHeight() - h));
    pos_ = pos;
}

void ContextMenu::Close()
{
    open_ = false;
    items_.clear();
    header_.clear();
}

float ContextMenu::HeaderHeight() const
{
    return header_.empty() ? 0.0f : RowHeight();
}

Rectangle ContextMenu::Bounds() const
{
    return { pos_.x, pos_.y, width_, HeaderHeight() + RowHeight() * (float)items_.size() };
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
            // A click on the header is inside the menu and runs nothing: idx is negative.
            const float           y = m.y - pos_.y - HeaderHeight();
            const int             idx = y < 0.0f ? -1 : (int)(y / RowHeight());
            std::function<void()> action;
            if (idx >= 0 && idx < (int)items_.size())
            {
                action = items_[idx].action;
                if (!items_[idx].disabled.empty())
                    return true;  // says why it cannot: the menu stays open
            }
            if (idx < 0)
                return true;  // the header: the menu stays open
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

    // What the actions are about, on the title colour, set apart by a rule.
    const float top = HeaderHeight();
    if (top > 0.0f)
    {
        const Rectangle head{ b.x, b.y, b.width, top };
        DrawRectangleRec(head, t.colors.title);
        Ui::DrawString(
            Ui::Face::Strong, header_,
            { std::round(b.x + Pad()), Ui::CentredTop(Ui::Face::Strong, px, head.y, top) }, px,
            t.colors.titleText);
        DrawLineEx({ b.x, b.y + top }, { b.x + b.width, b.y + top }, 1.0f, t.colors.border);
    }
    for (size_t i = 0; i < items_.size(); i++)
    {
        const Rectangle row{ b.x, b.y + top + rowH * (float)i, b.width, rowH };
        const bool      hover = Ui::MouseOver(row);
        const bool      off = !items_[i].disabled.empty();
        if (hover && !off)
            DrawRectangleRec(row, t.colors.selected);
        Ui::DrawString(
            Ui::Face::Regular, ItemText(items_[i]),
            { std::round(b.x + Pad()), Ui::CentredTop(Ui::Face::Regular, px, row.y, rowH) }, px,
            off     ? t.colors.dim
            : hover ? t.colors.accent
                    : t.colors.text);
    }
}
