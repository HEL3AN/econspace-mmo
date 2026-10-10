#include "core/RangePicker.h"

#include "ui/Controls.h"
#include "ui/UiTheme.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float PANEL_W = 260.0f;
constexpr float PANEL_H = 104.0f;
// Far enough to sit off a planet; nearer than anything a sublight hold would take minutes
// to cross. The overview and a warp cover the rest.
constexpr float MAX_RANGE = 60000.0f;
}  // namespace

void RangePicker::Open(Vector2 at, const std::string& targetName, float targetSize, float initial,
                       Choose choose)
{
    name_ = targetName;
    lo_ = std::max(targetSize * 1.1f, 30.0f);
    hi_ = std::max(MAX_RANGE, lo_ * 4.0f);
    value_ = std::clamp(initial, lo_, hi_);
    choose_ = std::move(choose);
    // Under the cursor, so the click that opened it is not read as a click outside it.
    pos_ = { std::min(at.x - 12.0f, (float)GetScreenWidth() - PANEL_W),
             std::min(at.y - 12.0f, (float)GetScreenHeight() - PANEL_H) };
    open_ = true;
    justOpened_ = true;
}

Rectangle RangePicker::Bounds() const
{
    return { pos_.x, pos_.y, PANEL_W, PANEL_H };
}

void RangePicker::Draw()
{
    if (!open_)
        return;
    const Rectangle b = Bounds();
    DrawRectangleRec(b, Ui::PANEL_BG);
    DrawRectangleLinesEx(b, 1.0f, Ui::PANEL_BORDER);
    Ui::Text(name_.c_str(), (int)b.x + 10, (int)b.y + 8, 14, Ui::ACCENT);
    if (Ui::SmallButton({ b.x + b.width - 24.0f, b.y + 6.0f, 18.0f, 18.0f }, "x"))
    {
        open_ = false;
        return;
    }

    // By ratio: 200 to 400 is as large a step as 20 000 to 40 000.
    Ui::LogSlider({ b.x + 10.0f, b.y + 32.0f, b.width - 20.0f, 28.0f }, "Range", value_, lo_, hi_,
                  "%.0f");

    struct Choice
    {
        const char* label;
        int         navMode;
    };
    static const Choice choices[] = { { "Orbit", 3 }, { "Hold", 4 }, { "Follow", 5 } };
    const float         bw = (b.width - 20.0f - 2 * 6.0f) / 3.0f;
    for (int i = 0; i < 3; i++)
    {
        const Rectangle r{ b.x + 10.0f + (float)i * (bw + 6.0f), b.y + 72.0f, bw, 22.0f };
        if (Ui::SmallButton(r, choices[i].label))
        {
            open_ = false;
            if (choose_)
                choose_(choices[i].navMode, value_);
            return;
        }
    }

    // A click anywhere else puts it away, like the menu it came from -- but not the click
    // that opened it, which is still down on the frame it appears.
    if (!justOpened_ && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        !CheckCollisionPointRec(GetMousePosition(), b))
        open_ = false;
    justOpened_ = false;
}
